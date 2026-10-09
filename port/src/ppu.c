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
 *
 * Speed: a frame is rendered in two passes. The first walks the lines in
 * order and records the video registers each line sees (HBlank DMA changes
 * them between lines); the second renders the lines from those snapshots on
 * all CPU cores. Text backgrounds decode each 8 pixel tile row once, and the
 * sprites are parsed once per frame.
 */
#include "port.h"

#include <SDL.h>
#include <stdlib.h>
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

/* line buffer pixel: bits 0-14 color, bit 15 opaque */
#define OPAQUE 0x8000u

enum { LAYER_BG0, LAYER_BG1, LAYER_BG2, LAYER_BG3, LAYER_OBJ, LAYER_BD };

typedef struct {
    uint16_t color;   /* with OPAQUE */
    uint8_t priority; /* 0-3 */
    uint8_t semiTransparent;
} ObjPixel;

/* the video registers 0x00-0x57 and the affine reference points, as one line sees them */
#define LINE_REGS (0x58 / 2)
typedef struct {
    uint16_t io[LINE_REGS];
    int32_t affX[2], affY[2]; /* reference points of BG2 / BG3 at classic x = 0 */
} LineState;

/* everything one rendering thread works with */
typedef struct {
    const LineState* st;
    uint16_t bgLine[4][PORT_MAX_VIEW_WIDTH];
    ObjPixel objLine[PORT_MAX_VIEW_WIDTH];
    uint8_t objWindow[PORT_MAX_VIEW_WIDTH];
    uint8_t winMask[PORT_MAX_VIEW_WIDTH];
    /* the two front-most layers of each pixel */
    uint16_t top[PORT_MAX_VIEW_WIDTH], second[PORT_MAX_VIEW_WIDTH];
    uint8_t topLayer[PORT_MAX_VIEW_WIDTH], secondLayer[PORT_MAX_VIEW_WIDTH];
} RenderCtx;

#define REG(off) (ctx->st->io[(off) >> 1])

/* frame constants */
static int sViewW, sViewH;
static LineState* sLines;
static int sLinesCap;

static const uint8_t sObjSizes[3][4][2] = {
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } }, /* square */
    { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } }, /* horizontal */
    { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } }, /* vertical */
};

static uint32_t sRgbLut[0x8000];

static void InitRgbLut(void) {
    uint32_t c;
    for (c = 0; c < 0x8000; c++) {
        uint32_t r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
        sRgbLut[c] = (r << 16) | (g << 8) | b;
    }
}

static inline uint32_t ToRGB(uint16_t c) {
    return sRgbLut[c & 0x7FFF];
}

static inline int32_t Sext28(uint32_t v) {
    return (int32_t)(v << 4) >> 4;
}

/* ---- text backgrounds ---- */

/*
 * The 8 pixels of one row of a background tile (map entry with flip bits and
 * palette). charData is the memory the tile numbers refer to: VRAM, or the
 * graphics a region of the room would have loaded (tileswap.c).
 */
static void DecodeTileRow(uint16_t entry, int py, const uint8_t* charData, uint32_t charBase, bool bpp8,
                          uint16_t row[8]) {
    uint32_t tile = entry & 0x3FF;
    int i;
    if (entry & 0x800)
        py = 7 - py;
    if (bpp8) {
        uint32_t addr = charBase + tile * 64 + py * 8;
        if (addr >= 0x10000) {
            memset(row, 0, 8 * sizeof(uint16_t));
            return;
        }
        for (i = 0; i < 8; i++) {
            int px = (entry & 0x400) ? 7 - i : i;
            uint8_t idx = charData[addr + px];
            row[i] = idx ? (PLTT16[idx] | OPAQUE) : 0;
        }
    } else {
        uint32_t addr = charBase + tile * 32 + py * 4;
        const uint16_t* pal = PLTT16 + ((entry >> 12) << 4);
        if (addr >= 0x10000) {
            memset(row, 0, 8 * sizeof(uint16_t));
            return;
        }
        uint32_t bits;
        memcpy(&bits, charData + addr, 4);
        if (bits == 0) {
            memset(row, 0, 8 * sizeof(uint16_t));
            return;
        }
        if (entry & 0x400) {
            for (i = 7; i >= 0; i--, bits >>= 4) {
                uint32_t idx = bits & 0xF;
                row[i] = idx ? (pal[idx] | OPAQUE) : 0;
            }
        } else {
            for (i = 0; i < 8; i++, bits >>= 4) {
                uint32_t idx = bits & 0xF;
                row[i] = idx ? (pal[idx] | OPAQUE) : 0;
            }
        }
    }
}

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

/* VRAM offset of the tile map entry of a text background at background pixel (x, y), wrapping like hardware */
static inline uint32_t TextMapAddr(uint32_t screenBase, int size, int x, int y) {
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
    return (screenBase + block * 0x800 + (ty * 32 + tx) * 2) & 0xFFFF;
}

static inline uint16_t TextMapEntry(uint32_t screenBase, int size, int x, int y) {
    return *(const uint16_t*)(VRAM8 + TextMapAddr(screenBase, size, x, y));
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
static void RenderHudBg0(RenderCtx* ctx, int vy) {
    uint16_t cnt = REG(0x08);
    uint32_t charBase = ((cnt >> 2) & 3) * 0x4000;
    uint32_t screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    bool bpp8 = (cnt >> 7) & 1;
    int size = cnt >> 14;
    int hofs = REG(0x10) & 0x1FF;
    int vofs = REG(0x12) & 0x1FF;
    int shiftR = sViewW - GBA_WIDTH, shiftB = sViewH - GBA_HEIGHT;
    uint16_t* out = ctx->bgLine[0];
    int lo[5], hi[5], n = 0, i;
    int vx, a;
    int cy = vy - gPortViewOffsetY;

    /* only the classic screen and the four corner quarters can have pixels */
    if (cy >= 0 && cy < GBA_HEIGHT) {
        lo[n] = gPortViewOffsetX;
        hi[n++] = gPortViewOffsetX + GBA_WIDTH;
    }
    for (a = 0; a < 4; a++) {
        int hy = vy - ((a & 2) ? shiftB : 0);
        if (hy < 0 || hy >= GBA_HEIGHT || ((hy >= GBA_HEIGHT / 2) != ((a & 2) != 0)))
            continue;
        lo[n] = (a & 1) ? shiftR + GBA_WIDTH / 2 : 0;
        hi[n++] = (a & 1) ? shiftR + GBA_WIDTH : GBA_WIDTH / 2;
    }
    memset(out, 0, sViewW * sizeof(uint16_t));
    for (i = 0; i < n; i++) {
        int start = lo[i] < 0 ? 0 : lo[i];
        int end = hi[i] > sViewW ? sViewW : hi[i];
        for (vx = start; vx < end; vx++) {
            uint16_t px = 0;
            int cx = vx - gPortViewOffsetX;
            if (out[vx])
                continue; /* done by an overlapping range */
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
}

/* hardware tile map pixels for view x in [a, b); bx = background x of view x = a */
static void TextSpanHw(uint16_t* out, int a, int b, int bx, int by, uint32_t screenBase, int size, uint32_t charBase,
                       bool bpp8) {
    uint16_t row[8];
    while (a < b) {
        int px = bx & 7;
        int n = 8 - px;
        if (n > b - a)
            n = b - a;
        DecodeTileRow(*(const uint16_t*)(VRAM8 + TextMapAddr(screenBase, size, bx, by)), by & 7, VRAM8, charBase, bpp8,
                      row);
        memcpy(out + a, row + px, n * sizeof(uint16_t));
        a += n;
        bx += n;
    }
}

/* room map pixels for view x in [a, b); mx = room x of view x = a, my = room y of the line */
static void TextSpanRoom(uint16_t* out, int a, int b, int mx, int my, const PpuBgOverride* ovr, uint32_t charBase,
                         bool bpp8) {
    uint16_t row[8];
    int ty = my >> 3;
    if (my < 0 || ty >= ovr->heightTiles) {
        memset(out + a, 0, (b - a) * sizeof(uint16_t));
        return;
    }
    if (mx < 0) {
        int n = -mx < b - a ? -mx : b - a;
        memset(out + a, 0, n * sizeof(uint16_t));
        a += n;
        mx += n;
    }
    while (a < b) {
        int tx = mx >> 3;
        int px = mx & 7;
        int n = 8 - px;
        uint16_t entry;
        const uint8_t* charData;
        if (tx >= ovr->widthTiles) {
            memset(out + a, 0, (b - a) * sizeof(uint16_t));
            return;
        }
        if (n > b - a)
            n = b - a;
        entry = ovr->map[ty * ovr->strideTiles + tx];
        charData = Port_TileSwapCharData(charBase, entry, bpp8, mx, my);
        DecodeTileRow(entry, my & 7, charData ? charData : VRAM8, charBase, bpp8, row);
        memcpy(out + a, row + px, n * sizeof(uint16_t));
        a += n;
        mx += n;
    }
}

/* mosaic: every pixel looks up its own (snapped) position */
static void RenderTextBgMosaic(RenderCtx* ctx, int bg, int y, bool lineInside, int mosaicH) {
    uint16_t cnt = REG(0x08 + bg * 2);
    uint32_t charBase = ((cnt >> 2) & 3) * 0x4000;
    uint32_t screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    bool bpp8 = (cnt >> 7) & 1;
    int size = cnt >> 14;
    int hofs = REG(0x10 + bg * 4) & 0x1FF;
    int vofs = REG(0x12 + bg * 4) & 0x1FF;
    PpuBgMode mode = gPpuBgMode[bg];
    const PpuBgOverride* ovr = &gPpuBgOverride[bg];
    uint16_t* out = ctx->bgLine[bg];
    int vx;
    int by = y + vofs, my = y + ovr->scrollY;

    for (vx = 0; vx < sViewW; vx++) {
        int cx = vx - gPortViewOffsetX;
        bool inside = lineInside && cx >= 0 && cx < GBA_WIDTH;
        int sx = cx - ((cx % mosaicH) + mosaicH) % mosaicH;
        if (inside || mode == PPU_BG_WRAP)
            TextSpanHw(out, vx, vx + 1, sx + hofs, by, screenBase, size, charBase, bpp8);
        else if (mode == PPU_BG_OVERRIDE && ovr->enabled)
            TextSpanRoom(out, vx, vx + 1, sx + ovr->scrollX, my, ovr, charBase, bpp8);
        else
            out[vx] = 0;
    }
}

static void RenderTextBg(RenderCtx* ctx, int bg, int line /* classic line */, int mosaicH, int mosaicV) {
    uint16_t cnt = REG(0x08 + bg * 2);
    uint32_t charBase = ((cnt >> 2) & 3) * 0x4000;
    uint32_t screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    bool bpp8 = (cnt >> 7) & 1;
    int size = cnt >> 14;
    int hofs = REG(0x10 + bg * 4) & 0x1FF;
    int vofs = REG(0x12 + bg * 4) & 0x1FF;
    PpuBgMode mode = gPpuBgMode[bg];
    const PpuBgOverride* ovr = &gPpuBgOverride[bg];
    uint16_t* out = ctx->bgLine[bg];
    int y = line;
    bool lineInside = line >= 0 && line < GBA_HEIGHT;
    int ox = gPortViewOffsetX;
    int seg[4], s;

    if ((cnt & 0x40) && mosaicV > 1)
        y -= ((y % mosaicV) + mosaicV) % mosaicV;
    if ((cnt & 0x40) && mosaicH > 1) {
        RenderTextBgMosaic(ctx, bg, y, lineInside, mosaicH);
        return;
    }

    /* left of the classic screen, the classic screen, right of it */
    seg[0] = 0;
    seg[1] = ox < 0 ? 0 : (ox > sViewW ? sViewW : ox);
    seg[2] = ox + GBA_WIDTH < seg[1] ? seg[1] : (ox + GBA_WIDTH > sViewW ? sViewW : ox + GBA_WIDTH);
    seg[3] = sViewW;
    for (s = 0; s < 3; s++) {
        int a = seg[s], b = seg[s + 1];
        bool inside = s == 1 && lineInside;
        if (a >= b)
            continue;
        if (inside || mode == PPU_BG_WRAP)
            TextSpanHw(out, a, b, a - ox + hofs, y + vofs, screenBase, size, charBase, bpp8);
        else if (mode == PPU_BG_OVERRIDE && ovr->enabled)
            TextSpanRoom(out, a, b, a - ox + ovr->scrollX, y + ovr->scrollY, ovr, charBase, bpp8);
        else
            memset(out + a, 0, (b - a) * sizeof(uint16_t));
    }
}

/* ---- affine backgrounds ---- */

static void RenderAffineBg(RenderCtx* ctx, int bg, int mosaicH) {
    uint16_t cnt = REG(0x08 + bg * 2);
    uint32_t charBase = ((cnt >> 2) & 3) * 0x4000;
    uint32_t screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    int size = 128 << (cnt >> 14);
    int tiles = size >> 3;
    bool wrap = (cnt >> 13) & 1;
    int idx = bg - 2;
    int16_t pa = (int16_t)REG(0x20 + idx * 0x10);
    int16_t pc = (int16_t)REG(0x24 + idx * 0x10);
    uint16_t* out = ctx->bgLine[bg];
    int vx;
    /* the reference point of classic x = 0 for this line */
    int32_t x0 = ctx->st->affX[idx] - pa * gPortViewOffsetX;
    int32_t y0 = ctx->st->affY[idx] - pc * gPortViewOffsetX;

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

/* a sprite, parsed once per frame */
typedef struct {
    uint16_t a1;
    int x, y;           /* top left of the drawn box, before the shift */
    int shiftX, shiftY; /* view offset (or HUD corner offset) */
    int w, h, bw, bh;
    bool affine, bpp8, mosaic;
    int objMode, prio, palBank;
    uint32_t tileBase;
    int16_t pa, pb, pc, pd;
} ObjDesc;

static ObjDesc sObjs[128];
static int sObjCount;

static void PrepareObjects(void) {
    int i;
    sObjCount = 0;
    for (i = 0; i < 128; i++) {
        uint16_t a0 = OAM16[i * 4 + 0];
        uint16_t a1 = OAM16[i * 4 + 1];
        uint16_t a2 = OAM16[i * 4 + 2];
        bool affine = (a0 >> 8) & 1;
        bool doubleSize = affine && ((a0 >> 9) & 1);
        int shape = (a0 >> 14) & 3;
        int sizeIdx = (a1 >> 14) & 3;
        int objMode = (a0 >> 10) & 3;
        const PortOamExt* ext = &gPortOamExtLive[i];
        ObjDesc* o;

        if (!affine && ((a0 >> 9) & 1))
            continue; /* disabled */
        if (shape == 3 || objMode == 3)
            continue;
        o = &sObjs[sObjCount++];
        o->a1 = a1;
        o->affine = affine;
        o->objMode = objMode;
        o->bpp8 = (a0 >> 13) & 1;
        o->mosaic = (a0 >> 12) & 1;
        o->w = sObjSizes[shape][sizeIdx][0];
        o->h = sObjSizes[shape][sizeIdx][1];
        o->bw = doubleSize ? o->w * 2 : o->w;
        o->bh = doubleSize ? o->h * 2 : o->h;
        o->tileBase = a2 & 0x3FF;
        o->palBank = (a2 >> 12) & 0xF;
        o->prio = (a2 >> 10) & 3;
        o->shiftX = gPortViewOffsetX;
        o->shiftY = gPortViewOffsetY;
        if (ext->valid && ext->attr0 == a0 && ext->attr1 == a1) {
            o->x = ext->x;
            o->y = ext->y;
            if ((ext->anchor & PORT_ANCHOR_HUD) && gPpuHudAnchor) {
                o->shiftX = (ext->anchor & PORT_ANCHOR_RIGHT) ? sViewW - GBA_WIDTH : 0;
                o->shiftY = (ext->anchor & PORT_ANCHOR_BOTTOM) ? sViewH - GBA_HEIGHT : 0;
            }
        } else {
            o->x = a1 & 0x1FF;
            o->y = a0 & 0xFF;
            if (o->x >= GBA_WIDTH)
                o->x -= 512;
            if (o->y + o->bh > 256)
                o->y -= 256;
        }
        o->pa = 0x100;
        o->pb = 0;
        o->pc = 0;
        o->pd = 0x100;
        if (affine) {
            int p = (a1 >> 9) & 0x1F;
            o->pa = (int16_t)OAM16[p * 16 + 3];
            o->pb = (int16_t)OAM16[p * 16 + 7];
            o->pc = (int16_t)OAM16[p * 16 + 11];
            o->pd = (int16_t)OAM16[p * 16 + 15];
        }
    }
}

static void RenderObjects(RenderCtx* ctx, int line /* classic line */, bool mode345) {
    uint16_t dispcnt = REG(0x00);
    bool map1d = (dispcnt >> 6) & 1;
    int mosaicH = (REG(0x4C) >> 8) & 0xF;
    int mosaicV = (REG(0x4C) >> 12) & 0xF;
    ObjPixel* objLine = ctx->objLine;
    uint8_t* objWindow = ctx->objWindow;
    int i;
    mosaicH++;
    mosaicV++;

    for (i = 0; i < sViewW; i++) {
        objLine[i].color = 0;
        objLine[i].priority = 4;
        objLine[i].semiTransparent = 0;
    }
    memset(objWindow, 0, sViewW);
    if (!(dispcnt & DISPCNT_OBJ_ON))
        return;

    for (i = 0; i < sObjCount; i++) {
        const ObjDesc* o = &sObjs[i];
        int w = o->w, h = o->h, bw = o->bw, x = o->x;
        int ly = line + gPortViewOffsetY - o->shiftY - o->y;
        int vx, start, end;
        if (ly < 0 || ly >= o->bh)
            continue;
        if (mode345 && o->tileBase < 512)
            continue;
        if (o->mosaic && mosaicV > 1)
            ly -= ly % mosaicV;
        /* only the part of the box inside the view */
        start = -(x + o->shiftX);
        if (start < 0)
            start = 0;
        end = sViewW - (x + o->shiftX);
        if (end > bw)
            end = bw;

        for (vx = start; vx < end; vx++) {
            int sx = x + vx + o->shiftX;
            int tx, ty, lx = vx;
            uint32_t tileNum, addr;
            uint8_t idx;
            uint16_t color;
            if (o->mosaic && mosaicH > 1)
                lx -= lx % mosaicH;
            if (o->affine) {
                int cx = lx - bw / 2;
                int cy = ly - o->bh / 2;
                tx = ((o->pa * cx + o->pb * cy) >> 8) + w / 2;
                ty = ((o->pc * cx + o->pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h)
                    continue;
            } else {
                tx = (o->a1 & 0x1000) ? w - 1 - lx : lx;
                ty = (o->a1 & 0x2000) ? h - 1 - ly : ly;
            }
            if (o->bpp8) {
                if (map1d)
                    tileNum = o->tileBase + ((ty >> 3) * (w >> 3) + (tx >> 3)) * 2;
                else
                    tileNum = o->tileBase + (ty >> 3) * 32 + (tx >> 3) * 2;
                addr = 0x10000 + (tileNum & 0x3FF) * 32 + (ty & 7) * 8 + (tx & 7);
                idx = VRAM8[addr];
                if (idx == 0)
                    continue;
                color = PLTT16[256 + idx];
            } else {
                if (map1d)
                    tileNum = o->tileBase + (ty >> 3) * (w >> 3) + (tx >> 3);
                else
                    tileNum = o->tileBase + (ty >> 3) * 32 + (tx >> 3);
                addr = 0x10000 + (tileNum & 0x3FF) * 32 + (ty & 7) * 4 + ((tx & 7) >> 1);
                idx = (VRAM8[addr] >> ((tx & 1) * 4)) & 0xF;
                if (idx == 0)
                    continue;
                color = PLTT16[256 + o->palBank * 16 + idx];
            }
            if (o->objMode == 2) {
                objWindow[sx] = 1;
                continue;
            }
            /* lower OAM index wins on equal priority (OAM is walked in order) */
            if (!(objLine[sx].color & OPAQUE) || o->prio < objLine[sx].priority) {
                objLine[sx].color = color | OPAQUE;
                objLine[sx].priority = o->prio;
                objLine[sx].semiTransparent = o->objMode == 1;
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

static bool BuildWindowMask(RenderCtx* ctx, int vy) {
    uint16_t dispcnt = REG(0x00);
    uint16_t winin = REG(0x48);
    uint16_t winout = REG(0x4A);
    bool win0 = (dispcnt >> 13) & 1, win1 = (dispcnt >> 14) & 1, objwin = (dispcnt >> 15) & 1;
    uint8_t* mask = ctx->winMask;
    int x;
    bool in0 = false, in1 = false;
    int x0lo = 0, x0hi = 0, x1lo = 0, x1hi = 0;

    if (!win0 && !win1 && !objwin) {
        memset(mask, 0x3F, sViewW);
        return false;
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
            mask[x] = winin & 0x3F;
        else if (in1 && x >= x1lo && x < x1hi)
            mask[x] = (winin >> 8) & 0x3F;
        else if (objwin && ctx->objWindow[x])
            mask[x] = (winout >> 8) & 0x3F;
        else
            mask[x] = winout & 0x3F;
    }
    return true;
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

/* paints the opaque, window-enabled pixels of one layer over the layers below it */
static void PaintLayer(RenderCtx* ctx, int layer, const uint16_t* src, bool windows) {
    uint8_t bit = 1 << layer;
    uint16_t* top = ctx->top;
    uint16_t* second = ctx->second;
    uint8_t* topLayer = ctx->topLayer;
    uint8_t* secondLayer = ctx->secondLayer;
    const uint8_t* mask = ctx->winMask;
    int x;
    for (x = 0; x < sViewW; x++) {
        uint16_t px = src[x];
        if ((px & OPAQUE) && (!windows || (mask[x] & bit))) {
            second[x] = top[x];
            secondLayer[x] = topLayer[x];
            top[x] = px;
            topLayer[x] = layer;
        }
    }
}

static void PaintObjects(RenderCtx* ctx, int prio) {
    int x;
    for (x = 0; x < sViewW; x++) {
        const ObjPixel* obj = &ctx->objLine[x];
        if (obj->priority == prio && (obj->color & OPAQUE) && (ctx->winMask[x] & 0x10)) {
            ctx->second[x] = ctx->top[x];
            ctx->secondLayer[x] = ctx->topLayer[x];
            ctx->top[x] = obj->color;
            ctx->topLayer[x] = LAYER_OBJ;
        }
    }
}

/*
 * The two front-most visible layers of every pixel, by painting back to front:
 * per priority 3..0 the backgrounds of that priority (higher index first), then
 * the sprites of that priority. Then the color effects.
 */
static void ComposeLine(RenderCtx* ctx, uint32_t* out, const bool bgOn[4], bool windows) {
    uint16_t bldcnt = REG(0x50);
    uint16_t bldalpha = REG(0x52);
    int eva = bldalpha & 0x1F, evb = (bldalpha >> 8) & 0x1F;
    int evy = REG(0x54) & 0x1F;
    int effect = (bldcnt >> 6) & 3;
    uint16_t backdrop = PLTT16[0];
    bool objPrio[5] = { false };
    int p, b, x;
    if (eva > 16)
        eva = 16;
    if (evb > 16)
        evb = 16;
    if (evy > 16)
        evy = 16;

    for (x = 0; x < sViewW; x++) {
        ctx->top[x] = backdrop;
        ctx->second[x] = backdrop;
        objPrio[ctx->objLine[x].priority] = true;
    }
    memset(ctx->topLayer, LAYER_BD, sViewW);
    memset(ctx->secondLayer, LAYER_BD, sViewW);
    for (p = 3; p >= 0; p--) {
        for (b = 3; b >= 0; b--) {
            if (bgOn[b] && (REG(0x08 + b * 2) & 3) == p)
                PaintLayer(ctx, b, ctx->bgLine[b], windows);
        }
        if (objPrio[p])
            PaintObjects(ctx, p);
    }

    for (x = 0; x < sViewW; x++) {
        uint8_t mask = ctx->winMask[x];
        int topLayer = ctx->topLayer[x], secondLayer = ctx->secondLayer[x];
        uint16_t c = ctx->top[x] & 0x7FFF;
        if (topLayer == LAYER_OBJ && ctx->objLine[x].semiTransparent && (bldcnt & (1 << (8 + secondLayer)))) {
            c = Blend(c, ctx->second[x] & 0x7FFF, eva, evb);
        } else if ((mask & 0x20) && (bldcnt & (1 << topLayer))) {
            switch (effect) {
                case 1:
                    if (bldcnt & (1 << (8 + secondLayer)))
                        c = Blend(c, ctx->second[x] & 0x7FFF, eva, evb);
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

/* ---- one line ---- */

static void RenderLine(RenderCtx* ctx, uint32_t* dst, int vy) {
    int classicLine = vy - gPortViewOffsetY;
    uint16_t dispcnt = REG(0x00);
    int mode, i;
    bool bgOn[4];
    int mosaicBgH, mosaicBgV;
    bool windows;

    if (dispcnt & DISPCNT_FORCED_BLANK) {
        for (i = 0; i < sViewW; i++)
            dst[i] = 0xFFFFFF;
        return;
    }
    mode = dispcnt & 7;
    mosaicBgH = (REG(0x4C) & 0xF) + 1;
    mosaicBgV = ((REG(0x4C) >> 4) & 0xF) + 1;
    for (i = 0; i < 4; i++)
        bgOn[i] = (dispcnt >> (8 + i)) & 1;
    if (mode == 1)
        bgOn[3] = false;
    if (mode >= 3) {
        /* bitmap modes are not used by the game */
        bgOn[0] = bgOn[1] = bgOn[2] = bgOn[3] = false;
    }
    for (i = 0; i < 4; i++) {
        if (!bgOn[i])
            continue;
        if (mode == 0 || (mode == 1 && i < 2)) {
            if (i == 0 && gPpuHudAnchor)
                RenderHudBg0(ctx, vy);
            else
                RenderTextBg(ctx, i, classicLine, mosaicBgH, mosaicBgV);
        } else {
            RenderAffineBg(ctx, i, mosaicBgH);
        }
    }
    RenderObjects(ctx, classicLine, mode >= 3);
    windows = BuildWindowMask(ctx, vy);
    ComposeLine(ctx, dst, bgOn, windows);
}

/* ---- threads ---- */

#define MAX_WORKERS 15
#define LINES_PER_JOB 8

static RenderCtx* sMainCtx;
static RenderCtx* sWorkerCtx[MAX_WORKERS];
static SDL_Thread* sWorkers[MAX_WORKERS];
static SDL_sem* sWorkStart;
static SDL_sem* sWorkDone;
static int sWorkerCount = -1;
static SDL_atomic_t sNextJob;
static uint32_t* sJobOut;
static int sJobPitch;

static void RenderJobs(RenderCtx* ctx) {
    for (;;) {
        int first = SDL_AtomicAdd(&sNextJob, 1) * LINES_PER_JOB;
        int vy, last;
        if (first >= sViewH)
            break;
        last = first + LINES_PER_JOB;
        if (last > sViewH)
            last = sViewH;
        for (vy = first; vy < last; vy++) {
            ctx->st = &sLines[vy];
            RenderLine(ctx, sJobOut + vy * sJobPitch, vy);
        }
    }
}

static int WorkerMain(void* arg) {
    RenderCtx* ctx = arg;
    for (;;) {
        SDL_SemWait(sWorkStart);
        RenderJobs(ctx);
        SDL_SemPost(sWorkDone);
    }
    return 0;
}

/* TMC_RENDER_THREADS=N sets the number of threads (1 = render on the main thread only) */
static void StartWorkers(void) {
    const char* env = getenv("TMC_RENDER_THREADS");
    int n = env ? atoi(env) - 1 : SDL_GetCPUCount() - 1;
    int i;
    if (n < 0)
        n = 0;
    if (n > MAX_WORKERS)
        n = MAX_WORKERS;
    InitRgbLut();
    sMainCtx = calloc(1, sizeof(RenderCtx));
    sWorkerCount = 0;
    if (n == 0)
        return;
    sWorkStart = SDL_CreateSemaphore(0);
    sWorkDone = SDL_CreateSemaphore(0);
    if (sWorkStart == NULL || sWorkDone == NULL)
        return;
    for (i = 0; i < n; i++) {
        sWorkerCtx[i] = calloc(1, sizeof(RenderCtx));
        sWorkers[i] = SDL_CreateThread(WorkerMain, "render", sWorkerCtx[i]);
        if (sWorkers[i] == NULL)
            break;
        sWorkerCount++;
    }
}

/* ---- the frame ---- */

static void SnapshotLine(LineState* st, const int32_t affX[2], const int32_t affY[2]) {
    memcpy(st->io, (const void*)(uintptr_t)PORT_IO_ADDR, sizeof(st->io));
    st->affX[0] = affX[0];
    st->affX[1] = affX[1];
    st->affY[0] = affY[0];
    st->affY[1] = affY[1];
}

void Ppu_RenderFrame(uint32_t* out, int pitch, int w, int h) {
    int32_t affX[2], affY[2];
    int vy, i;
    bool parallel;

    if (sWorkerCount < 0)
        StartWorkers();
    if (h > sLinesCap) {
        free(sLines);
        sLines = malloc(sizeof(LineState) * h);
        sLinesCap = h;
    }
    sViewW = w;
    sViewH = h;
    if (gPortViewOffsetX < 0 || gPortViewOffsetX > w - GBA_WIDTH)
        gPortViewOffsetX = (w - GBA_WIDTH) / 2;
    if (gPortViewOffsetY < 0 || gPortViewOffsetY > h - GBA_HEIGHT)
        gPortViewOffsetY = (h - GBA_HEIGHT) / 2;
    PrepareObjects();

    /* latch the affine reference points; lines above the classic screen extrapolate */
    for (i = 0; i < 2; i++) {
        int32_t pb = (int16_t)PORT_IO16(0x22 + i * 0x10);
        int32_t pd = (int16_t)PORT_IO16(0x26 + i * 0x10);
        affX[i] = Sext28(PORT_IO32(0x28 + i * 0x10)) - pb * gPortViewOffsetY;
        affY[i] = Sext28(PORT_IO32(0x2C + i * 0x10)) - pd * gPortViewOffsetY;
    }

    /*
     * Pass 1, in order: the registers every line sees. HBlank DMA runs after each of
     * the 160 real lines. The lines can be rendered afterwards and in parallel as long
     * as the DMA only changes registers (it does in this game; otherwise each line is
     * rendered right away).
     */
    parallel = sWorkerCount > 0 && Port_DmaHBlankOnlyIo();
    for (vy = 0; vy < h; vy++) {
        int classicLine = vy - gPortViewOffsetY;
        SnapshotLine(&sLines[vy], affX, affY);
        if (!parallel) {
            sMainCtx->st = &sLines[vy];
            RenderLine(sMainCtx, out + vy * pitch, vy);
        }
        /* advance the affine reference points */
        for (i = 0; i < 2; i++) {
            affX[i] += (int16_t)PORT_IO16(0x22 + i * 0x10);
            affY[i] += (int16_t)PORT_IO16(0x26 + i * 0x10);
        }
        /* HBlank DMA and VCOUNT only exist for the 160 real lines */
        if (classicLine >= 0 && classicLine < GBA_HEIGHT) {
            PORT_IO16(0x006) = (uint16_t)classicLine;
            Port_DmaOnHBlank(classicLine);
        }
    }
    if (!parallel)
        return;

    /* pass 2: the lines on all cores */
    sJobOut = out;
    sJobPitch = pitch;
    SDL_AtomicSet(&sNextJob, 0);
    for (i = 0; i < sWorkerCount; i++)
        SDL_SemPost(sWorkStart);
    RenderJobs(sMainCtx);
    for (i = 0; i < sWorkerCount; i++)
        SDL_SemWait(sWorkDone);
}

/* A frame that is not displayed: only the per-line side effects (VCOUNT, HBlank DMA). */
void Ppu_SkipFrame(void) {
    int line;
    for (line = 0; line < GBA_HEIGHT; line++) {
        PORT_IO16(0x006) = (uint16_t)line;
        Port_DmaOnHBlank(line);
    }
}
