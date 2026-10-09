/**
 * @file bg_affine.c
 * @brief Rotated / scaled backgrounds (BG2 and BG3 in video modes 1 and 2).
 *
 * The background is a square 256-color image (128 to 1024 pixels) drawn
 * through a 2x2 matrix. Along a line the texture coordinate advances by
 * (pa, pc) per pixel; the line's start point (refX, refY) is the hardware's
 * internal reference point, which the pipeline advances by (pb, pd) per line
 * so that mid-frame register changes (HBlank DMA) work as on the GBA.
 *
 * The extended view continues the line naturally to the left and right of
 * the classic screen. The texel lookup itself is the background sampler in
 * scaling.c.
 */
#include "shader.h"

void BgAffine_Line(const FrameUniforms* f, const BgAffineUniforms* u, int viewLine, uint16_t* out) {
    const BgAffineLayer* layer = &u->layer;
    BgSampler sample = Scaling_BgSampler(layer);
    /* refX / refY are at classic x = 0, the line starts at view x = 0 */
    int32_t x0 = u->refX - layer->pa * f->viewOffsetX;
    int32_t y0 = u->refY - layer->pc * f->viewOffsetX;
    int vx;
    for (vx = 0; vx < f->viewW; vx++) {
        int sx = Mosaic_Snap(vx, u->mosaicH);
        out[vx] = sample(layer, x0 + layer->pa * sx, y0 + layer->pc * sx, vx, viewLine);
    }
}
