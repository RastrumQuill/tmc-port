/**
 * @file bg_text.c
 * @brief Text (tiled) backgrounds: the map layers, the UI layer, skies.
 *
 * A text background is a 256 or 512 pixel square map of 8x8 tiles, scrolled
 * by (hofs, vofs). The game uses them for the room's two map layers, the UI
 * (BG0: HUD, text boxes) and backgrounds like clouds.
 *
 * How a background continues outside the classic 240x160 screen in the
 * extended view (chosen per background by view.c):
 *   - CLASSIC_ONLY: transparent outside the classic screen,
 *   - WRAP: the map repeats, as the hardware would show it (skies, clouds),
 *   - ROOM_MAP: the map layers read the room's full tile map, so the level
 *     continues beyond the 32x32 tile window the game keeps in VRAM.
 *
 * BgText_HudLine draws BG0 with the HUD moved to the corners of the view.
 */
#include "shader.h"

void BgText_Line(const FrameUniforms* f, const BgTextUniforms* u, int classicLine, uint16_t* out) {
    const PpuBgOverride* map = u->roomMap;
    bool lineInside = classicLine >= 0 && classicLine < GBA_HEIGHT;
    int y = Mosaic_SnapSigned(classicLine, u->mosaicV);
    int vx;

    for (vx = 0; vx < f->viewW; vx++) {
        int cx = vx - f->viewOffsetX;
        bool inside = lineInside && cx >= 0 && cx < GBA_WIDTH;
        int sx = Mosaic_SnapSigned(cx, u->mosaicH);
        if (inside || u->extension == BG_TEXT_WRAP || (u->extension == BG_TEXT_ROOM_MAP && !map->enabled)) {
            int bx, by;
            if (!inside && u->extension != BG_TEXT_WRAP) {
                out[vx] = 0;
                continue;
            }
            /* the background as the GBA shows it */
            bx = sx + u->hofs;
            by = y + u->vofs;
            out[vx] = Tex_BgTilePixel(Tex_TextMapEntry(u->screenBase, u->size, bx, by), bx & 7, by & 7,
                                      u->charBase, u->bpp8);
        } else if (u->extension == BG_TEXT_ROOM_MAP) {
            /* outside the classic screen: the room's full tile map */
            int mx = sx + map->scrollX;
            int my = y + map->scrollY;
            int tx, ty;
            if (mx < 0 || my < 0) {
                out[vx] = 0;
                continue;
            }
            tx = mx >> 3;
            ty = my >> 3;
            if (tx >= map->widthTiles || ty >= map->heightTiles) {
                out[vx] = 0;
                continue;
            }
            out[vx] = Tex_BgTilePixel(map->map[ty * map->strideTiles + tx], mx & 7, my & 7, u->charBase, u->bpp8);
        } else {
            out[vx] = 0;
        }
    }
}

/* HUD tiles (hearts, charge bar, rupees, keys): palette 15, tiles below the message border tiles */
static bool IsHudTile(uint16_t entry) {
    uint16_t tile = entry & 0x3FF;
    return (entry >> 12) == 0xF && tile >= 0x10 && tile < 0x7B;
}

/*
 * Every HUD tile is drawn relative to the nearest corner of the view instead
 * of the classic screen; everything else stays where the classic screen is.
 */
void BgText_HudLine(const FrameUniforms* f, const BgHudUniforms* u, int viewLine, uint16_t* out) {
    int shiftR = f->viewW - GBA_WIDTH, shiftB = f->viewH - GBA_HEIGHT;
    int vx, a;
    for (vx = 0; vx < f->viewW; vx++) {
        uint16_t px = 0;
        int cx = vx - f->viewOffsetX, cy = viewLine - f->viewOffsetY;
        /* normal (non HUD) content of the classic screen */
        if (cx >= 0 && cx < GBA_WIDTH && cy >= 0 && cy < GBA_HEIGHT) {
            uint16_t e = Tex_TextMapEntry(u->screenBase, u->size, cx + u->hofs, cy + u->vofs);
            if (!IsHudTile(e))
                px = Tex_BgTilePixel(e, (cx + u->hofs) & 7, (cy + u->vofs) & 7, u->charBase, u->bpp8);
        }
        /* HUD tiles anchored to the four corners */
        for (a = 0; a < 4 && !(px & PPU_OPAQUE); a++) {
            int hx = vx - ((a & 1) ? shiftR : 0);
            int hy = viewLine - ((a & 2) ? shiftB : 0);
            uint16_t e;
            if (hx < 0 || hx >= GBA_WIDTH || hy < 0 || hy >= GBA_HEIGHT)
                continue;
            if (((hx >= GBA_WIDTH / 2) != ((a & 1) != 0)) || ((hy >= GBA_HEIGHT / 2) != ((a & 2) != 0)))
                continue;
            e = Tex_TextMapEntry(u->screenBase, u->size, hx + u->hofs, hy + u->vofs);
            if (IsHudTile(e))
                px = Tex_BgTilePixel(e, (hx + u->hofs) & 7, (hy + u->vofs) & 7, u->charBase, u->bpp8);
        }
        out[vx] = px;
    }
}
