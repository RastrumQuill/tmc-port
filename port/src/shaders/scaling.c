/**
 * @file scaling.c
 * @brief The scaling model: how scaled and rotated graphics are sampled.
 *
 * The GBA scales and rotates two kinds of graphics with a 2x2 matrix
 * (8.8 fixed point):
 *
 *   - affine sprites: the large bosses (Big Green ChuChu, Big Octo, Gyorg),
 *     Wallmasters, the Great Fairy, the giant Minish-scale objects, ice
 *     blocks, lily pads, spinning or growing effects (see obj.c);
 *   - affine backgrounds: the title screen, the rolling barrel room in
 *     Deepwood Shrine (see bg_affine.c).
 *
 * For every pixel on screen the hardware computes a texture coordinate
 * through the matrix and takes the nearest texel. These samplers receive that
 * coordinate *with* its fractional part (8.8 fixed point, 256 = one texel)
 * plus a full description of what is being sampled, and return a color.
 *
 * This is the place to change the look of scaled graphics:
 *
 *   - bilinear / bicubic / xBR-style filtering: blend the texels around
 *     (u, v) using the fractional bits (u & 0xFF, v & 0xFF). Keep returning
 *     false where the result is transparent (texel index 0) so edges and
 *     sprite outlines stay correct.
 *   - AI or offline upscaled textures: ObjAffineSprite.tex identifies the
 *     sprite's tiles and palette; decode it once (Tex_ObjTexelIndex for every
 *     texel, Tex_ObjColor for the colors), upscale or look up a replacement
 *     image, cache it, and sample that at (u / 256 * factor, v / 256 * factor).
 *     ObjAffineSprite.boxW / boxH and the matrix tell how large it appears on
 *     screen.
 *   - per sprite choice: Scaling_ObjSampler decides which model a sprite
 *     uses, e.g. only sprites of 64x64 texels (the boss bodies), only a
 *     certain tile range or palette bank (a specific boss's graphics), or
 *     only when the matrix magnifies (|pa| or |pd| below 0x100).
 *
 * The defaults below are the hardware's nearest neighbour lookup.
 *
 * Note: on screen, each GBA pixel is later scaled to the window by
 * present.c; a sampler here works in GBA pixels (the view). For detail finer
 * than a GBA pixel, the view itself would need a higher internal resolution.
 */
#include "shader.h"

/* ---- sprites ---- */

/** the hardware's model: the texel containing (u, v) */
static bool ObjSampleNearest(const ObjAffineSprite* sprite, int32_t u, int32_t v, int screenX, int screenY,
                             uint16_t* color) {
    int tx = u >> 8;
    int ty = v >> 8;
    uint8_t index;
    (void)screenX;
    (void)screenY;
    if (tx < 0 || ty < 0 || tx >= sprite->tex.w || ty >= sprite->tex.h)
        return false;
    index = Tex_ObjTexelIndex(&sprite->tex, tx, ty);
    if (index == 0)
        return false;
    *color = Tex_ObjColor(&sprite->tex, index);
    return true;
}

ObjSampler Scaling_ObjSampler(const ObjAffineSprite* sprite) {
    (void)sprite;
    /* e.g.: if (sprite->tex.w == 64 && sprite->tex.h == 64) return ObjSampleBossModel; */
    return ObjSampleNearest;
}

/* ---- backgrounds ---- */

/** the hardware's model: the texel containing (u, v), repeating or transparent outside */
static uint16_t BgSampleNearest(const BgAffineLayer* layer, int32_t u, int32_t v, int screenX, int screenY) {
    int32_t tx = u >> 8;
    int32_t ty = v >> 8;
    uint8_t tile;
    (void)screenX;
    (void)screenY;
    if (layer->wrap) {
        tx &= layer->size - 1;
        ty &= layer->size - 1;
    } else if (tx < 0 || ty < 0 || tx >= layer->size || ty >= layer->size) {
        return 0;
    }
    tile = Tex_AffineMapTile(layer->screenBase, layer->size >> 3, tx >> 3, ty >> 3);
    return Tex_BgPalette256(Tex_AffineTileIndex(layer->charBase, tile, tx & 7, ty & 7));
}

BgSampler Scaling_BgSampler(const BgAffineLayer* layer) {
    (void)layer;
    return BgSampleNearest;
}
