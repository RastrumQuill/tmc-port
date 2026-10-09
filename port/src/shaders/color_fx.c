/**
 * @file color_fx.c
 * @brief Color effects: alpha blending and fades (BLDCNT / BLDALPHA / BLDY).
 *
 * The GBA's color math, per pixel, on the two front-most layers:
 *   - alpha blend: top * eva/16 + second * evb/16 (water, shadows, ghosts,
 *     semi-transparent sprites, fog),
 *   - brighten: towards white by evy/16 (flashes, fade to white),
 *   - darken: towards black by evy/16 (fade to black, dark rooms).
 * Each channel is 5 bits; results are clamped to 31.
 *
 * Semi-transparent sprites always blend with the layer below when that layer
 * is a second target, whatever the selected effect is.
 */
#include "shader.h"

uint16_t ColorFx_Blend(uint16_t a, uint16_t b, int eva, int evb) {
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

uint16_t ColorFx_Brighten(uint16_t c, int evy) {
    int r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
    r += ((31 - r) * evy) >> 4;
    g += ((31 - g) * evy) >> 4;
    b += ((31 - b) * evy) >> 4;
    return (uint16_t)(r | (g << 5) | (b << 10));
}

uint16_t ColorFx_Darken(uint16_t c, int evy) {
    int r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
    r -= (r * evy) >> 4;
    g -= (g * evy) >> 4;
    b -= (b * evy) >> 4;
    return (uint16_t)(r | (g << 5) | (b << 10));
}

uint16_t ColorFx_Pixel(const ColorFxUniforms* u, uint16_t top, int topLayer, bool topSemiTransparent,
                       uint16_t second, int secondLayer, bool effectsEnabled) {
    uint16_t c = top & 0x7FFF;
    if (topLayer == LAYER_OBJ && topSemiTransparent && (u->secondTargets & (1 << secondLayer)))
        return ColorFx_Blend(c, second & 0x7FFF, u->eva, u->evb);
    if (!effectsEnabled || !(u->firstTargets & (1 << topLayer)))
        return c;
    switch (u->effect) {
        case FX_ALPHA:
            if (u->secondTargets & (1 << secondLayer))
                c = ColorFx_Blend(c, second & 0x7FFF, u->eva, u->evb);
            break;
        case FX_BRIGHTEN:
            c = ColorFx_Brighten(c, u->evy);
            break;
        case FX_DARKEN:
            c = ColorFx_Darken(c, u->evy);
            break;
        default:
            break;
    }
    return c;
}
