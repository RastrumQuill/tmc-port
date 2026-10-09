/**
 * @file ppu.c
 * @brief The GBA picture, rendered by the shader-like stages in port/src/shaders.
 *
 * This file is only the pipeline: once per scanline it reads the video
 * registers into each stage's uniforms and runs the stages in hardware order
 * (see port/src/shaders/shader.h for the overview). Rendering line by line
 * keeps HBlank DMA effects (register changes between lines) working.
 *
 * The output can be larger than 240x160. The classic screen is placed at
 * (gPortViewOffsetX, gPortViewOffsetY) inside the view and everything is
 * positioned relative to it.
 */
#include "port.h"
#include "shaders/shader.h"

#include "global.h"

PpuBgOverride gPpuBgOverride[4];
PpuBgMode gPpuBgMode[4];
bool gPpuHudAnchor;
int gPortViewOffsetX;
int gPortViewOffsetY;

#define REG(off) PORT_IO16(off)

/* the layers of one line */
static uint16_t sBgLine[4][PORT_MAX_VIEW_WIDTH];
static ObjPixel sObjLine[PORT_MAX_VIEW_WIDTH];
static uint8_t sObjWindow[PORT_MAX_VIEW_WIDTH];
static uint8_t sWinMask[PORT_MAX_VIEW_WIDTH];

/* internal affine reference points of BG2 / BG3 at classic x = 0 of the current line */
static int32_t sAffX[2], sAffY[2];

static inline int32_t Sext28(uint32_t v) {
    return (int32_t)(v << 4) >> 4;
}

static int ClampEv(int v) {
    return v > 16 ? 16 : v;
}

/* ---- uniforms from the registers ---- */

static void TextUniforms(int bg, int mosaicH, int mosaicV, BgTextUniforms* u) {
    uint16_t cnt = REG(0x08 + bg * 2);
    bool mosaic = (cnt >> 6) & 1;
    u->charBase = ((cnt >> 2) & 3) * 0x4000;
    u->screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    u->bpp8 = (cnt >> 7) & 1;
    u->size = cnt >> 14;
    u->hofs = REG(0x10 + bg * 4) & 0x1FF;
    u->vofs = REG(0x12 + bg * 4) & 0x1FF;
    u->mosaicH = mosaic ? mosaicH : 1;
    u->mosaicV = mosaic ? mosaicV : 1;
    switch (gPpuBgMode[bg]) {
        case PPU_BG_WRAP:
            u->extension = BG_TEXT_WRAP;
            break;
        case PPU_BG_OVERRIDE:
            u->extension = BG_TEXT_ROOM_MAP;
            break;
        default:
            u->extension = BG_TEXT_CLASSIC_ONLY;
            break;
    }
    u->roomMap = &gPpuBgOverride[bg];
}

static void HudUniforms(BgHudUniforms* u) {
    uint16_t cnt = REG(0x08);
    u->charBase = ((cnt >> 2) & 3) * 0x4000;
    u->screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    u->bpp8 = (cnt >> 7) & 1;
    u->size = cnt >> 14;
    u->hofs = REG(0x10) & 0x1FF;
    u->vofs = REG(0x12) & 0x1FF;
}

static void AffineUniforms(int bg, int mosaicH, BgAffineUniforms* u) {
    uint16_t cnt = REG(0x08 + bg * 2);
    int idx = bg - 2;
    u->layer.bg = bg;
    u->layer.charBase = ((cnt >> 2) & 3) * 0x4000;
    u->layer.screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    u->layer.size = 128 << (cnt >> 14);
    u->layer.wrap = (cnt >> 13) & 1;
    u->layer.pa = (int16_t)REG(0x20 + idx * 0x10);
    u->layer.pc = (int16_t)REG(0x24 + idx * 0x10);
    u->refX = sAffX[idx];
    u->refY = sAffY[idx];
    u->mosaicH = ((cnt >> 6) & 1) ? mosaicH : 1;
}

static void ObjectUniforms(uint16_t dispcnt, int mode, ObjUniforms* u) {
    u->enabled = (dispcnt & DISPCNT_OBJ_ON) != 0;
    u->map1d = (dispcnt >> 6) & 1;
    u->bitmapMode = mode >= 3;
    u->mosaicH = ((REG(0x4C) >> 8) & 0xF) + 1;
    u->mosaicV = ((REG(0x4C) >> 12) & 0xF) + 1;
    u->hudAnchor = gPpuHudAnchor;
}

static void WinUniforms(uint16_t dispcnt, WindowUniforms* u) {
    u->win0 = (dispcnt >> 13) & 1;
    u->win1 = (dispcnt >> 14) & 1;
    u->objWin = (dispcnt >> 15) & 1;
    u->win0H = REG(0x40);
    u->win1H = REG(0x42);
    u->win0V = REG(0x44);
    u->win1V = REG(0x46);
    u->winIn = REG(0x48);
    u->winOut = REG(0x4A);
}

static void ComposeUniformsFromRegs(const bool bgOn[4], ComposeUniforms* u) {
    uint16_t bldcnt = REG(0x50);
    uint16_t bldalpha = REG(0x52);
    int b;
    for (b = 0; b < 4; b++) {
        u->bgOn[b] = bgOn[b];
        u->bgPriority[b] = REG(0x08 + b * 2) & 3;
    }
    u->backdrop = Tex_Backdrop();
    u->fx.effect = (ColorEffect)((bldcnt >> 6) & 3);
    u->fx.firstTargets = bldcnt & 0x3F;
    u->fx.secondTargets = bldcnt >> 8;
    u->fx.eva = ClampEv(bldalpha & 0x1F);
    u->fx.evb = ClampEv((bldalpha >> 8) & 0x1F);
    u->fx.evy = ClampEv(REG(0x54) & 0x1F);
}

/* ---- the frame ---- */

void Ppu_RenderFrame(uint32_t* out, int pitch, int w, int h) {
    FrameUniforms frame;
    int vy, i;

    if (gPortViewOffsetX < 0 || gPortViewOffsetX > w - GBA_WIDTH)
        gPortViewOffsetX = (w - GBA_WIDTH) / 2;
    if (gPortViewOffsetY < 0 || gPortViewOffsetY > h - GBA_HEIGHT)
        gPortViewOffsetY = (h - GBA_HEIGHT) / 2;
    frame.viewW = w;
    frame.viewH = h;
    frame.viewOffsetX = gPortViewOffsetX;
    frame.viewOffsetY = gPortViewOffsetY;

    /* latch the affine reference points; lines above the classic screen extrapolate */
    for (i = 0; i < 2; i++) {
        int32_t pb = (int16_t)REG(0x22 + i * 0x10);
        int32_t pd = (int16_t)REG(0x26 + i * 0x10);
        sAffX[i] = Sext28(PORT_IO32(0x28 + i * 0x10)) - pb * gPortViewOffsetY;
        sAffY[i] = Sext28(PORT_IO32(0x2C + i * 0x10)) - pd * gPortViewOffsetY;
    }

    for (vy = 0; vy < h; vy++) {
        uint32_t* dst = out + vy * pitch;
        int classicLine = vy - gPortViewOffsetY;
        uint16_t dispcnt = REG(0x00);

        if (dispcnt & DISPCNT_FORCED_BLANK) {
            for (i = 0; i < w; i++)
                dst[i] = Output_ForcedBlankPixel();
        } else {
            int mode = dispcnt & 7;
            int mosaicBgH = (REG(0x4C) & 0xF) + 1;
            int mosaicBgV = ((REG(0x4C) >> 4) & 0xF) + 1;
            bool bgOn[4];
            ObjUniforms obj;
            WindowUniforms win;
            ComposeUniforms compose;

            for (i = 0; i < 4; i++)
                bgOn[i] = (dispcnt >> (8 + i)) & 1;
            if (mode == 1)
                bgOn[3] = false;
            if (mode >= 3) {
                /* bitmap modes are not used by the game */
                bgOn[0] = bgOn[1] = bgOn[2] = bgOn[3] = false;
            }

            /* backgrounds */
            for (i = 0; i < 4; i++) {
                if (!bgOn[i])
                    continue;
                if (mode == 0 || (mode == 1 && i < 2)) {
                    if (i == 0 && gPpuHudAnchor) {
                        BgHudUniforms hud;
                        HudUniforms(&hud);
                        BgText_HudLine(&frame, &hud, vy, sBgLine[0]);
                    } else {
                        BgTextUniforms text;
                        TextUniforms(i, mosaicBgH, mosaicBgV, &text);
                        BgText_Line(&frame, &text, classicLine, sBgLine[i]);
                    }
                } else {
                    BgAffineUniforms aff;
                    AffineUniforms(i, mosaicBgH, &aff);
                    BgAffine_Line(&frame, &aff, vy, sBgLine[i]);
                }
            }

            /* sprites, windows, composition */
            ObjectUniforms(dispcnt, mode, &obj);
            Obj_Line(&frame, &obj, classicLine, sObjLine, sObjWindow);
            WinUniforms(dispcnt, &win);
            Window_Line(&frame, &win, vy, sObjWindow, sWinMask);
            ComposeUniformsFromRegs(bgOn, &compose);
            Compose_Line(&frame, &compose, sBgLine, sObjLine, sWinMask, dst);
        }

        /* advance the affine reference points */
        for (i = 0; i < 2; i++) {
            sAffX[i] += (int16_t)REG(0x22 + i * 0x10);
            sAffY[i] += (int16_t)REG(0x26 + i * 0x10);
        }
        /* HBlank DMA and VCOUNT only exist for the 160 real lines */
        if (classicLine >= 0 && classicLine < GBA_HEIGHT) {
            PORT_IO16(0x006) = (uint16_t)classicLine;
            Port_DmaOnHBlank(classicLine);
        }
    }
}
