/**
 * @file compose.c
 * @brief Layer composition: which layer is in front, per pixel.
 *
 * For every pixel the two front-most visible layers are found (backgrounds by
 * priority then number, sprites in front of backgrounds of the same or lower
 * priority, the backdrop color last), filtered by the window mask, and the
 * color effect (color_fx.c) combines them. output.c turns the result into the
 * output pixel.
 */
#include "shader.h"

void Compose_Line(const FrameUniforms* f, const ComposeUniforms* u, uint16_t bgLines[4][PORT_MAX_VIEW_WIDTH],
                  const ObjPixel* objLine, const uint8_t* winMask, uint32_t* out) {
    int order[4];
    int n = 0, p, b, x;

    /* backgrounds sorted by priority, then index */
    for (p = 0; p < 4; p++) {
        for (b = 0; b < 4; b++) {
            if (u->bgOn[b] && u->bgPriority[b] == p)
                order[n++] = b;
        }
    }

    for (x = 0; x < f->viewW; x++) {
        uint8_t mask = winMask[x];
        int topLayer = LAYER_BD, secondLayer = LAYER_BD;
        uint16_t top = u->backdrop, second = u->backdrop;
        int found = 0;
        int i;
        const ObjPixel* obj = &objLine[x];
        bool objVisible = (obj->color & PPU_OPAQUE) && (mask & 0x10);
        bool objUsed = false;

        for (i = 0; i < n && found < 2; i++) {
            int bg = order[i];
            uint16_t px;
            if (objVisible && !objUsed && obj->priority <= u->bgPriority[bg]) {
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
            px = bgLines[bg][x];
            if (!(px & PPU_OPAQUE))
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

        out[x] = Output_Pixel(ColorFx_Pixel(&u->fx, top, topLayer, obj->semiTransparent, second, secondLayer,
                                            (mask & 0x20) != 0));
    }
}
