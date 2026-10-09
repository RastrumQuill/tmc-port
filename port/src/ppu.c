/**
 * @file ppu.c
 * @brief Software renderer for the GBA picture processing unit.
 *
 * Renders text backgrounds (modes 0/1), affine backgrounds (modes 1/2),
 * sprites (regular, affine, semi transparent, object window), windows,
 * alpha blending, brightness effects and mosaic, scanline by scanline so that
 * HBlank DMA effects work.
 *
 * The output can be larger than 240x160. The classic screen is placed at
 * (gPortViewOffsetX, gPortViewOffsetY) inside the view and everything is
 * positioned relative to it:
 *   - sprites use their full precision coordinates (gPortOamExtLive),
 *   - affine backgrounds extend naturally,
 *   - text backgrounds follow gPpuBgMode (classic only / wrap / override map),
 *   - windows that span the whole classic screen are extended to the view.
 */
#include "port.h"

#include <string.h>

#include "global.h"

PpuBgOverride gPpuBgOverride[4];
PpuBgMode gPpuBgMode[4];
bool gPpuHudAnchor;
int gPortViewOffsetX;
int gPortViewOffsetY;

#define VRAM8 ((const uint8_t*)(uintptr_t)PORT_VRAM_ADDR)
#define PLTT16 ((const uint16_t*)(uintptr_t)PORT_PLTT_ADDR)
#define OAM16 ((const uint16_t*)(uintptr_t)PORT_OAM_ADDR)

#define REG(off) PORT_IO16(off)

/* line buffer pixel: bits 0-14 color, bit 15 opaque */
#define OPAQUE 0x8000u

enum { LAYER_BG0, LAYER_BG1, LAYER_BG2, LAYER_BG3, LAYER_OBJ, LAYER_BD };

typedef struct {
    uint16_t color;   /* with OPAQUE */
    uint8_t priority; /* 0-3 */
    uint8_t semiTransparent;
} ObjPixel;

static uint16_t sBgLine[4][PORT_MAX_VIEW_WIDTH];
static ObjPixel sObjLine[PORT_MAX_VIEW_WIDTH];
static uint8_t sObjWindow[PORT_MAX_VIEW_WIDTH];
static uint8_t sWinMask[PORT_MAX_VIEW_WIDTH];

/* internal affine reference points */
static int32_t sAffX[2], sAffY[2];

static int sViewW, sViewH;

static const uint8_t sObjSizes[3][4][2] = {
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } }, /* square */
    { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } }, /* horizontal */
    { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } }, /* vertical */
};

static inline uint32_t ToRGB(uint16_t c) {
    uint32_t r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}

static inline int32_t Sext28(uint32_t v) {
    return (int32_t)(v << 4) >> 4;
}

/* ---- text backgrounds ---- */

static inline uint16_t TextTilePixel(uint16_t entry, int px, int py, uint32_t charBase, bool bpp8) {
    uint32_t tile = entry & 0x3FF;
    if (entry & 0x400)
        px = 7 - px;
    if (entry & 0x800)
        py = 7 - py;
    if (bpp8) {
        uint32_t addr = charBase + tile * 64 + py * 8 + px;
        uint8_t idx;
        if (addr >= 0x10000)
            return 0;
        idx = VRAM8[addr];
        return idx ? (PLTT16[idx] | OPAQUE) : 0;
    } else {
        uint32_t addr = charBase + tile * 32 + py * 4 + (px >> 1);
        uint8_t idx;
        if (addr >= 0x10000)
            return 0;
        idx = (VRAM8[addr] >> ((px & 1) * 4)) & 0xF;
        return idx ? (PLTT16[((entry >> 12) << 4) | idx] | OPAQUE) : 0;
    }
}

/* tile map entry of a text background at background pixel (x, y), wrapping like hardware */
static inline uint16_t TextMapEntry(uint32_t screenBase, int size, int x, int y) {
    int w = (size & 1) ? 512 : 256;
    int h = (size & 2) ? 512 : 256;
    int tx, ty, block = 0;
    x &= w - 1;
    y &= h - 1;
    tx = x >> 3;
    ty = y >> 3;
    if (tx >= 32) {
        block += 1;
        tx -= 32;
    }
    if (ty >= 32) {
        block += (size == 3) ? 2 : 1;
        ty -= 32;
    }
    return *(const uint16_t*)(VRAM8 + ((screenBase + block * 0x800 + (ty * 32 + tx) * 2) & 0xFFFF));
}

/* HUD tiles (hearts, charge bar, rupees, keys): palette 15, tiles below the message border tiles */
static inline bool IsHudTile(uint16_t entry) {
    uint16_t tile = entry & 0x3FF;
    return (entry >> 12) == 0xF && tile >= 0x10 && tile < 0x7B;
}

/*
 * BG0 with the HUD moved to the corners of the view: every HUD tile is drawn
 * relative to the nearest corner of the view instead of the classic screen.
 */
static void RenderHudBg0(int vy) {
    uint16_t cnt = REG(0x08);
    uint32_t charBase = ((cnt >> 2) & 3) * 0x4000;
    uint32_t screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    bool bpp8 = (cnt >> 7) & 1;
    int size = cnt >> 14;
    int hofs = REG(0x10) & 0x1FF;
    int vofs = REG(0x12) & 0x1FF;
    int shiftR = sViewW - GBA_WIDTH, shiftB = sViewH - GBA_HEIGHT;
    uint16_t* out = sBgLine[0];
    int vx, a;
    for (vx = 0; vx < sViewW; vx++) {
        uint16_t px = 0;
        int cx = vx - gPortViewOffsetX, cy = vy - gPortViewOffsetY;
        /* normal (non HUD) content of the classic screen */
        if (cx >= 0 && cx < GBA_WIDTH && cy >= 0 && cy < GBA_HEIGHT) {
            uint16_t e = TextMapEntry(screenBase, size, cx + hofs, cy + vofs);
            if (!IsHudTile(e))
                px = TextTilePixel(e, (cx + hofs) & 7, (cy + vofs) & 7, charBase, bpp8);
        }
        /* HUD tiles anchored to the four corners */
        for (a = 0; a < 4 && !(px & OPAQUE); a++) {
            int hx = vx - ((a & 1) ? shiftR : 0);
            int hy = vy - ((a & 2) ? shiftB : 0);
            uint16_t e;
            if (hx < 0 || hx >= GBA_WIDTH || hy < 0 || hy >= GBA_HEIGHT)
                continue;
            if (((hx >= GBA_WIDTH / 2) != ((a & 1) != 0)) || ((hy >= GBA_HEIGHT / 2) != ((a & 2) != 0)))
                continue;
            e = TextMapEntry(screenBase, size, hx + hofs, hy + vofs);
            if (IsHudTile(e))
                px = TextTilePixel(e, (hx + hofs) & 7, (hy + vofs) & 7, charBase, bpp8);
        }
        out[vx] = px;
    }
}

static void RenderTextBg(int bg, int line /* classic line */, int mosaicH, int mosaicV) {
    uint16_t cnt = REG(0x08 + bg * 2);
    uint32_t charBase = ((cnt >> 2) & 3) * 0x4000;
    uint32_t screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    bool bpp8 = (cnt >> 7) & 1;
    int size = cnt >> 14;
    int hofs = REG(0x10 + bg * 4) & 0x1FF;
    int vofs = REG(0x12 + bg * 4) & 0x1FF;
    PpuBgMode mode = gPpuBgMode[bg];
    const PpuBgOverride* ovr = &gPpuBgOverride[bg];
    uint16_t* out = sBgLine[bg];
    int vx;
    int y = line;
    bool lineInside = line >= 0 && line < GBA_HEIGHT;

    if ((cnt & 0x40) && mosaicV > 1)
        y -= ((y % mosaicV) + mosaicV) % mosaicV;

    for (vx = 0; vx < sViewW; vx++) {
        int cx = vx - gPortViewOffsetX;
        bool inside = lineInside && cx >= 0 && cx < GBA_WIDTH;
        int sx = cx;
        if ((cnt & 0x40) && mosaicH > 1)
            sx -= ((sx % mosaicH) + mosaicH) % mosaicH;
        if (inside || mode == PPU_BG_WRAP || (mode == PPU_BG_OVERRIDE && !ovr->enabled)) {
            int bx, by;
            uint16_t entry;
            if (!inside && mode != PPU_BG_WRAP) {
                out[vx] = 0;
                continue;
            }
            bx = sx + hofs;
            by = y + vofs;
            entry = TextMapEntry(screenBase, size, bx, by);
            out[vx] = TextTilePixel(entry, bx & 7, by & 7, charBase, bpp8);
        } else if (mode == PPU_BG_OVERRIDE) {
            int mx = sx + ovr->scrollX;
            int my = y + ovr->scrollY;
            int tx, ty;
            if (mx < 0 || my < 0) {
                out[vx] = 0;
                continue;
            }
            tx = mx >> 3;
            ty = my >> 3;
            if (tx >= ovr->widthTiles || ty >= ovr->heightTiles) {
                out[vx] = 0;
                continue;
            }
            out[vx] = TextTilePixel(ovr->map[ty * ovr->strideTiles + tx], mx & 7, my & 7, charBase, bpp8);
        } else {
            out[vx] = 0;
        }
    }
}

/* ---- affine backgrounds ---- */

static void RenderAffineBg(int bg, int mosaicH) {
    uint16_t cnt = REG(0x08 + bg * 2);
    uint32_t charBase = ((cnt >> 2) & 3) * 0x4000;
    uint32_t screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    int size = 128 << (cnt >> 14);
    int tiles = size >> 3;
    bool wrap = (cnt >> 13) & 1;
    int idx = bg - 2;
    int16_t pa = (int16_t)REG(0x20 + idx * 0x10);
    int16_t pc = (int16_t)REG(0x24 + idx * 0x10);
    uint16_t* out = sBgLine[bg];
    int vx;
    /* sAffX/Y hold the reference point of classic x = 0 for this line */
    int32_t x0 = sAffX[idx] - pa * gPortViewOffsetX;
    int32_t y0 = sAffY[idx] - pc * gPortViewOffsetX;

    for (vx = 0; vx < sViewW; vx++) {
        int sx = vx;
        int32_t tx, ty;
        uint8_t tile, pix;
        if ((cnt & 0x40) && mosaicH > 1)
            sx -= sx % mosaicH;
        tx = (x0 + pa * sx) >> 8;
        ty = (y0 + pc * sx) >> 8;
        if (wrap) {
            tx &= size - 1;
            ty &= size - 1;
        } else if (tx < 0 || ty < 0 || tx >= size || ty >= size) {
            out[vx] = 0;
            continue;
        }
        tile = VRAM8[(screenBase + (ty >> 3) * tiles + (tx >> 3)) & 0xFFFF];
        pix = VRAM8[(charBase + tile * 64 + (ty & 7) * 8 + (tx & 7)) & 0xFFFF];
        out[vx] = pix ? (PLTT16[pix] | OPAQUE) : 0;
    }
}

/* ---- sprites ---- */

static void RenderObjects(int line /* classic line */, bool mode345) {
    uint16_t dispcnt = REG(0x00);
    bool map1d = (dispcnt >> 6) & 1;
    int mosaicH = (REG(0x4C) >> 8) & 0xF;
    int mosaicV = (REG(0x4C) >> 12) & 0xF;
    int i;
    mosaicH++;
    mosaicV++;

    for (i = 0; i < sViewW; i++) {
        sObjLine[i].color = 0;
        sObjLine[i].priority = 4;
        sObjLine[i].semiTransparent = 0;
        sObjWindow[i] = 0;
    }
    if (!(dispcnt & DISPCNT_OBJ_ON))
        return;

    for (i = 0; i < 128; i++) {
        uint16_t a0 = OAM16[i * 4 + 0];
        uint16_t a1 = OAM16[i * 4 + 1];
        uint16_t a2 = OAM16[i * 4 + 2];
        bool affine = (a0 >> 8) & 1;
        bool doubleSize = affine && ((a0 >> 9) & 1);
        int shape = (a0 >> 14) & 3;
        int sizeIdx = (a1 >> 14) & 3;
        int objMode = (a0 >> 10) & 3;
        bool bpp8 = (a0 >> 13) & 1;
        bool mosaic = (a0 >> 12) & 1;
        int w, h, bw, bh, x, y, ly, vx;
        int shiftX = gPortViewOffsetX, shiftY = gPortViewOffsetY;
        int16_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        uint32_t tileBase = a2 & 0x3FF;
        int palBank = (a2 >> 12) & 0xF;
        int prio = (a2 >> 10) & 3;
        const PortOamExt* ext = &gPortOamExtLive[i];

        if (!affine && ((a0 >> 9) & 1))
            continue; /* disabled */
        if (shape == 3 || objMode == 3)
            continue;
        w = sObjSizes[shape][sizeIdx][0];
        h = sObjSizes[shape][sizeIdx][1];
        bw = doubleSize ? w * 2 : w;
        bh = doubleSize ? h * 2 : h;

        if (ext->valid && ext->attr0 == a0 && ext->attr1 == a1) {
            x = ext->x;
            y = ext->y;
            if ((ext->anchor & PORT_ANCHOR_HUD) && gPpuHudAnchor) {
                shiftX = (ext->anchor & PORT_ANCHOR_RIGHT) ? sViewW - GBA_WIDTH : 0;
                shiftY = (ext->anchor & PORT_ANCHOR_BOTTOM) ? sViewH - GBA_HEIGHT : 0;
            }
        } else {
            x = a1 & 0x1FF;
            y = a0 & 0xFF;
            if (x >= GBA_WIDTH)
                x -= 512;
            if (y + bh > 256)
                y -= 256;
        }
        ly = line + gPortViewOffsetY - shiftY - y;
        if (ly < 0 || ly >= bh)
            continue;
        if (mode345 && tileBase < 512)
            continue;
        if (mosaic && mosaicV > 1)
            ly -= ly % mosaicV;

        if (affine) {
            int p = (a1 >> 9) & 0x1F;
            pa = (int16_t)OAM16[p * 16 + 3];
            pb = (int16_t)OAM16[p * 16 + 7];
            pc = (int16_t)OAM16[p * 16 + 11];
            pd = (int16_t)OAM16[p * 16 + 15];
        }

        for (vx = 0; vx < bw; vx++) {
            int sx = x + vx + shiftX;
            int tx, ty, lx = vx;
            uint32_t tileNum, addr;
            uint8_t idx;
            uint16_t color;
            if (sx < 0 || sx >= sViewW)
                continue;
            if (mosaic && mosaicH > 1)
                lx -= lx % mosaicH;
            if (affine) {
                int cx = lx - bw / 2;
                int cy = ly - bh / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h)
                    continue;
            } else {
                tx = (a1 & 0x1000) ? w - 1 - lx : lx;
                ty = (a1 & 0x2000) ? h - 1 - ly : ly;
            }
            if (bpp8) {
                if (map1d)
                    tileNum = tileBase + ((ty >> 3) * (w >> 3) + (tx >> 3)) * 2;
                else
                    tileNum = tileBase + (ty >> 3) * 32 + (tx >> 3) * 2;
                addr = 0x10000 + (tileNum & 0x3FF) * 32 + (ty & 7) * 8 + (tx & 7);
                idx = VRAM8[addr];
                if (idx == 0)
                    continue;
                color = PLTT16[256 + idx];
            } else {
                if (map1d)
                    tileNum = tileBase + (ty >> 3) * (w >> 3) + (tx >> 3);
                else
                    tileNum = tileBase + (ty >> 3) * 32 + (tx >> 3);
                addr = 0x10000 + (tileNum & 0x3FF) * 32 + (ty & 7) * 4 + ((tx & 7) >> 1);
                idx = (VRAM8[addr] >> ((tx & 1) * 4)) & 0xF;
                if (idx == 0)
                    continue;
                color = PLTT16[256 + palBank * 16 + idx];
            }
            if (objMode == 2) {
                sObjWindow[sx] = 1;
                continue;
            }
            /* lower OAM index wins on equal priority (OAM is walked in order) */
            if (!(sObjLine[sx].color & OPAQUE) || prio < sObjLine[sx].priority) {
                sObjLine[sx].color = color | OPAQUE;
                sObjLine[sx].priority = prio;
                sObjLine[sx].semiTransparent = objMode == 1;
            }
        }
    }
}

/* ---- windows ---- */

/*
 * Window range in view coordinates. On hardware, R > size or L > R means R = size.
 * An edge of the classic screen is extended to the edge of the view, so windows
 * that cover the screen (or a border of it) keep doing so in the extended view.
 */
static void WindowRange(uint16_t reg, int classicSize, int viewOffset, int viewSize, int* lo, int* hi) {
    int a = reg >> 8;
    int b = reg & 0xFF;
    if (b > classicSize || a > b)
        b = classicSize;
    *lo = (a == 0) ? 0 : a + viewOffset;
    *hi = (b >= classicSize) ? viewSize : b + viewOffset;
}

static void BuildWindowMask(int vy) {
    uint16_t dispcnt = REG(0x00);
    uint16_t winin = REG(0x48);
    uint16_t winout = REG(0x4A);
    bool win0 = (dispcnt >> 13) & 1, win1 = (dispcnt >> 14) & 1, objwin = (dispcnt >> 15) & 1;
    int x;
    bool in0 = false, in1 = false;
    int x0lo = 0, x0hi = 0, x1lo = 0, x1hi = 0;

    if (!win0 && !win1 && !objwin) {
        memset(sWinMask, 0x3F, sViewW);
        return;
    }
    if (win0) {
        int lo, hi;
        WindowRange(REG(0x44), GBA_HEIGHT, gPortViewOffsetY, sViewH, &lo, &hi);
        in0 = vy >= lo && vy < hi;
        WindowRange(REG(0x40), GBA_WIDTH, gPortViewOffsetX, sViewW, &x0lo, &x0hi);
    }
    if (win1) {
        int lo, hi;
        WindowRange(REG(0x46), GBA_HEIGHT, gPortViewOffsetY, sViewH, &lo, &hi);
        in1 = vy >= lo && vy < hi;
        WindowRange(REG(0x42), GBA_WIDTH, gPortViewOffsetX, sViewW, &x1lo, &x1hi);
    }
    for (x = 0; x < sViewW; x++) {
        if (in0 && x >= x0lo && x < x0hi)
            sWinMask[x] = winin & 0x3F;
        else if (in1 && x >= x1lo && x < x1hi)
            sWinMask[x] = (winin >> 8) & 0x3F;
        else if (objwin && sObjWindow[x])
            sWinMask[x] = (winout >> 8) & 0x3F;
        else
            sWinMask[x] = winout & 0x3F;
    }
}

/* ---- compositing ---- */

static inline uint16_t Blend(uint16_t a, uint16_t b, int eva, int evb) {
    int r = ((a & 0x1F) * eva + (b & 0x1F) * evb) >> 4;
    int g = (((a >> 5) & 0x1F) * eva + ((b >> 5) & 0x1F) * evb) >> 4;
    int bl = (((a >> 10) & 0x1F) * eva + ((b >> 10) & 0x1F) * evb) >> 4;
    if (r > 31)
        r = 31;
    if (g > 31)
        g = 31;
    if (bl > 31)
        bl = 31;
    return (uint16_t)(r | (g << 5) | (bl << 10));
}

static inline uint16_t Brighten(uint16_t c, int evy) {
    int r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
    r += ((31 - r) * evy) >> 4;
    g += ((31 - g) * evy) >> 4;
    b += ((31 - b) * evy) >> 4;
    return (uint16_t)(r | (g << 5) | (b << 10));
}

static inline uint16_t Darken(uint16_t c, int evy) {
    int r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
    r -= (r * evy) >> 4;
    g -= (g * evy) >> 4;
    b -= (b * evy) >> 4;
    return (uint16_t)(r | (g << 5) | (b << 10));
}

static void ComposeLine(uint32_t* out, bool bgOn[4]) {
    uint16_t bldcnt = REG(0x50);
    uint16_t bldalpha = REG(0x52);
    int eva = bldalpha & 0x1F, evb = (bldalpha >> 8) & 0x1F;
    int evy = REG(0x54) & 0x1F;
    int effect = (bldcnt >> 6) & 3;
    uint16_t backdrop = PLTT16[0];
    int bgPrio[4];
    int order[4];
    int n = 0, p, b, x;
    if (eva > 16)
        eva = 16;
    if (evb > 16)
        evb = 16;
    if (evy > 16)
        evy = 16;

    /* backgrounds sorted by priority, then index */
    for (p = 0; p < 4; p++) {
        for (b = 0; b < 4; b++) {
            bgPrio[b] = REG(0x08 + b * 2) & 3;
            if (bgOn[b] && bgPrio[b] == p)
                order[n++] = b;
        }
    }

    for (x = 0; x < sViewW; x++) {
        uint8_t mask = sWinMask[x];
        int topLayer = LAYER_BD, secondLayer = LAYER_BD;
        uint16_t top = backdrop, second = backdrop;
        int found = 0;
        int i;
        const ObjPixel* obj = &sObjLine[x];
        bool objVisible = (obj->color & OPAQUE) && (mask & 0x10);
        bool objUsed = false;
        uint16_t c;

        for (i = 0; i < n && found < 2; i++) {
            int bg = order[i];
            uint16_t px;
            if (objVisible && !objUsed && obj->priority <= bgPrio[bg]) {
                if (found == 0) {
                    top = obj->color;
                    topLayer = LAYER_OBJ;
                } else {
                    second = obj->color;
                    secondLayer = LAYER_OBJ;
                }
                found++;
                objUsed = true;
                if (found >= 2)
                    break;
            }
            if (!(mask & (1 << bg)))
                continue;
            px = sBgLine[bg][x];
            if (!(px & OPAQUE))
                continue;
            if (found == 0) {
                top = px;
                topLayer = bg;
            } else {
                second = px;
                secondLayer = bg;
            }
            found++;
        }
        if (found < 2 && objVisible && !objUsed) {
            if (found == 0) {
                top = obj->color;
                topLayer = LAYER_OBJ;
            } else {
                second = obj->color;
                secondLayer = LAYER_OBJ;
            }
        }

        c = top & 0x7FFF;
        if (topLayer == LAYER_OBJ && obj->semiTransparent && (bldcnt & (1 << (8 + secondLayer)))) {
            c = Blend(c, second & 0x7FFF, eva, evb);
        } else if ((mask & 0x20) && (bldcnt & (1 << topLayer))) {
            switch (effect) {
                case 1:
                    if (bldcnt & (1 << (8 + secondLayer)))
                        c = Blend(c, second & 0x7FFF, eva, evb);
                    break;
                case 2:
                    c = Brighten(c, evy);
                    break;
                case 3:
                    c = Darken(c, evy);
                    break;
            }
        }
        out[x] = ToRGB(c);
    }
}

void Ppu_RenderFrame(uint32_t* out, int pitch, int w, int h) {
    uint16_t dispcnt;
    int vy, line = 0;
    int i;

    sViewW = w;
    sViewH = h;
    if (gPortViewOffsetX < 0 || gPortViewOffsetX > w - GBA_WIDTH)
        gPortViewOffsetX = (w - GBA_WIDTH) / 2;
    if (gPortViewOffsetY < 0 || gPortViewOffsetY > h - GBA_HEIGHT)
        gPortViewOffsetY = (h - GBA_HEIGHT) / 2;

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
        int mode;
        bool bgOn[4];
        int mosaicBgH, mosaicBgV;

        dispcnt = REG(0x00);
        if (dispcnt & DISPCNT_FORCED_BLANK) {
            for (i = 0; i < w; i++)
                dst[i] = 0xFFFFFF;
        } else {
            mode = dispcnt & 7;
            mosaicBgH = (REG(0x4C) & 0xF) + 1;
            mosaicBgV = ((REG(0x4C) >> 4) & 0xF) + 1;
            for (i = 0; i < 4; i++)
                bgOn[i] = (dispcnt >> (8 + i)) & 1;
            if (mode == 1)
                bgOn[3] = false;
            if (mode >= 3) {
                /* bitmap modes are not used by the game */
                bgOn[0] = bgOn[1] = bgOn[3] = false;
                bgOn[2] = false;
            }
            for (i = 0; i < 4; i++) {
                if (!bgOn[i])
                    continue;
                if (mode == 0 || (mode == 1 && i < 2))
                {
                    if (i == 0 && gPpuHudAnchor)
                        RenderHudBg0(vy);
                    else
                        RenderTextBg(i, classicLine, mosaicBgH, mosaicBgV);
                }
                else
                    RenderAffineBg(i, mosaicBgH);
            }
            RenderObjects(classicLine, mode >= 3);
            BuildWindowMask(vy);
            ComposeLine(dst, bgOn);
        }

        /* advance affine reference points */
        for (i = 0; i < 2; i++) {
            sAffX[i] += (int16_t)REG(0x22 + i * 0x10);
            sAffY[i] += (int16_t)REG(0x26 + i * 0x10);
        }
        /* HBlank DMA and VCOUNT only exist for the 160 real lines */
        if (classicLine >= 0 && classicLine < GBA_HEIGHT) {
            line = classicLine;
            PORT_IO16(0x006) = (uint16_t)line;
            Port_DmaOnHBlank(line);
        }
    }
    (void)line;
}
