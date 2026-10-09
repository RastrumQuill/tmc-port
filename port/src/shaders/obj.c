/**
 * @file obj.c
 * @brief Sprites (OBJ): characters, enemies, items, effects, bosses.
 *
 * Up to 128 sprites from OAM, each 8x8 to 64x64 texels, drawn in OAM order
 * per line; a lower OAM index wins between sprites of the same priority.
 *
 *   - regular sprites: texels 1:1 on screen, optionally flipped;
 *   - affine sprites: drawn through a 2x2 matrix (scaled, rotated, sheared),
 *     optionally in a box twice their size ("double size") so the scaled
 *     picture has room. Their texels are read by the sampler that
 *     Scaling_ObjSampler (scaling.c) picks, which is where the scaling model
 *     of the giant bosses can be changed.
 *
 * Sprite modes: normal, semi-transparent (alpha blended in color_fx.c) and
 * object window (the sprite's shape becomes a window mask, see window.c).
 *
 * Positions use the port's full precision coordinates (gPortOamExtLive) when
 * a sprite is outside the GBA's 9-bit range, so sprites work everywhere in the
 * extended view; HUD sprites can be anchored to the corners of the view.
 */
#include "shader.h"

#define OAM16 ((const uint16_t*)(uintptr_t)PORT_OAM_ADDR)

static const uint8_t sObjSizes[3][4][2] = {
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } }, /* square */
    { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } }, /* horizontal */
    { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } }, /* vertical */
};

/* writes one sprite pixel, keeping the front-most one */
static void PutPixel(ObjPixel* objLine, uint8_t* objWindow, int sx, uint16_t color, int objMode, int prio) {
    if (objMode == 2) {
        objWindow[sx] = 1;
        return;
    }
    /* lower OAM index wins on equal priority (OAM is walked in order) */
    if (!(objLine[sx].color & PPU_OPAQUE) || prio < objLine[sx].priority) {
        objLine[sx].color = color | PPU_OPAQUE;
        objLine[sx].priority = prio;
        objLine[sx].semiTransparent = objMode == 1;
    }
}

void Obj_Line(const FrameUniforms* f, const ObjUniforms* u, int classicLine, ObjPixel* objLine,
              uint8_t* objWindow) {
    int i;

    for (i = 0; i < f->viewW; i++) {
        objLine[i].color = 0;
        objLine[i].priority = 4;
        objLine[i].semiTransparent = 0;
        objWindow[i] = 0;
    }
    if (!u->enabled)
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
        bool mosaic = (a0 >> 12) & 1;
        int bw, bh, x, y, ly, vx;
        int shiftX = f->viewOffsetX, shiftY = f->viewOffsetY;
        int prio = (a2 >> 10) & 3;
        const PortOamExt* ext = &gPortOamExtLive[i];
        ObjTexture tex;

        if (!affine && ((a0 >> 9) & 1))
            continue; /* disabled */
        if (shape == 3 || objMode == 3)
            continue;
        tex.w = sObjSizes[shape][sizeIdx][0];
        tex.h = sObjSizes[shape][sizeIdx][1];
        tex.tileBase = a2 & 0x3FF;
        tex.bpp8 = (a0 >> 13) & 1;
        tex.map1d = u->map1d;
        tex.palBank = (a2 >> 12) & 0xF;
        bw = doubleSize ? tex.w * 2 : tex.w;
        bh = doubleSize ? tex.h * 2 : tex.h;

        /* position */
        if (ext->valid && ext->attr0 == a0 && ext->attr1 == a1) {
            x = ext->x;
            y = ext->y;
            if ((ext->anchor & PORT_ANCHOR_HUD) && u->hudAnchor) {
                shiftX = (ext->anchor & PORT_ANCHOR_RIGHT) ? f->viewW - GBA_WIDTH : 0;
                shiftY = (ext->anchor & PORT_ANCHOR_BOTTOM) ? f->viewH - GBA_HEIGHT : 0;
            }
        } else {
            x = a1 & 0x1FF;
            y = a0 & 0xFF;
            if (x >= GBA_WIDTH)
                x -= 512;
            if (y + bh > 256)
                y -= 256;
        }
        ly = classicLine + f->viewOffsetY - shiftY - y;
        if (ly < 0 || ly >= bh)
            continue;
        if (u->bitmapMode && tex.tileBase < 512)
            continue;
        if (mosaic)
            ly = Mosaic_Snap(ly, u->mosaicV);

        if (affine) {
            /* scaled / rotated: texture coordinates through the matrix, texels from the sampler */
            ObjAffineSprite sprite;
            ObjSampler sample;
            int p = (a1 >> 9) & 0x1F;
            int cy = ly - bh / 2;
            sprite.tex = tex;
            sprite.oamIndex = i;
            sprite.pa = (int16_t)OAM16[p * 16 + 3];
            sprite.pb = (int16_t)OAM16[p * 16 + 7];
            sprite.pc = (int16_t)OAM16[p * 16 + 11];
            sprite.pd = (int16_t)OAM16[p * 16 + 15];
            sprite.doubleSize = doubleSize;
            sprite.boxW = bw;
            sprite.boxH = bh;
            sample = Scaling_ObjSampler(&sprite);
            for (vx = 0; vx < bw; vx++) {
                int sx = x + vx + shiftX;
                int cx;
                int32_t tu, tv;
                uint16_t color;
                if (sx < 0 || sx >= f->viewW)
                    continue;
                cx = (mosaic ? Mosaic_Snap(vx, u->mosaicH) : vx) - bw / 2;
                /* 8.8 texture coordinate, the matrix is centered on the sprite */
                tu = sprite.pa * cx + sprite.pb * cy + (tex.w / 2) * 256;
                tv = sprite.pc * cx + sprite.pd * cy + (tex.h / 2) * 256;
                if (!sample(&sprite, tu, tv, sx, classicLine + f->viewOffsetY, &color))
                    continue;
                PutPixel(objLine, objWindow, sx, color, objMode, prio);
            }
        } else {
            /* regular: texels 1:1, optionally flipped */
            for (vx = 0; vx < bw; vx++) {
                int sx = x + vx + shiftX;
                int lx, tx, ty;
                uint8_t index;
                if (sx < 0 || sx >= f->viewW)
                    continue;
                lx = mosaic ? Mosaic_Snap(vx, u->mosaicH) : vx;
                tx = (a1 & 0x1000) ? tex.w - 1 - lx : lx;
                ty = (a1 & 0x2000) ? tex.h - 1 - ly : ly;
                index = Tex_ObjTexelIndex(&tex, tx, ty);
                if (index == 0)
                    continue;
                PutPixel(objLine, objWindow, sx, Tex_ObjColor(&tex, index), objMode, prio);
            }
        }
    }
}
