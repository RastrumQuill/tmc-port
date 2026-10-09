/**
 * @file window.c
 * @brief Windows: which layers and effects are visible where.
 *
 * Two rectangles (WIN0, WIN1), the object window (shapes of OBJ-window
 * sprites) and "outside" each carry six enable bits: BG0-3, OBJ and color
 * effects. The game uses them for spotlights in dark rooms, the circle
 * transitions, text box areas and similar. The result is a per pixel mask
 * used by compose.c.
 *
 * In the extended view, a window edge on the border of the classic screen is
 * moved to the border of the view, so windows covering the screen (or one
 * side of it) keep doing so.
 */
#include "shader.h"

#include <string.h>

/*
 * Window range in view coordinates. On hardware, R > size or L > R means R = size.
 */
static void WindowRange(uint16_t reg, int classicSize, int viewOffset, int viewSize, int* lo, int* hi) {
    int a = reg >> 8;
    int b = reg & 0xFF;
    if (b > classicSize || a > b)
        b = classicSize;
    *lo = (a == 0) ? 0 : a + viewOffset;
    *hi = (b >= classicSize) ? viewSize : b + viewOffset;
}

void Window_Line(const FrameUniforms* f, const WindowUniforms* u, int viewLine, const uint8_t* objWindow,
                 uint8_t* mask) {
    int x;
    bool in0 = false, in1 = false;
    int x0lo = 0, x0hi = 0, x1lo = 0, x1hi = 0;

    if (!u->win0 && !u->win1 && !u->objWin) {
        memset(mask, 0x3F, f->viewW);
        return;
    }
    if (u->win0) {
        int lo, hi;
        WindowRange(u->win0V, GBA_HEIGHT, f->viewOffsetY, f->viewH, &lo, &hi);
        in0 = viewLine >= lo && viewLine < hi;
        WindowRange(u->win0H, GBA_WIDTH, f->viewOffsetX, f->viewW, &x0lo, &x0hi);
    }
    if (u->win1) {
        int lo, hi;
        WindowRange(u->win1V, GBA_HEIGHT, f->viewOffsetY, f->viewH, &lo, &hi);
        in1 = viewLine >= lo && viewLine < hi;
        WindowRange(u->win1H, GBA_WIDTH, f->viewOffsetX, f->viewW, &x1lo, &x1hi);
    }
    for (x = 0; x < f->viewW; x++) {
        if (in0 && x >= x0lo && x < x0hi)
            mask[x] = u->winIn & 0x3F;
        else if (in1 && x >= x1lo && x < x1hi)
            mask[x] = (u->winIn >> 8) & 0x3F;
        else if (u->objWin && objWindow[x])
            mask[x] = (u->winOut >> 8) & 0x3F;
        else
            mask[x] = u->winOut & 0x3F;
    }
}
