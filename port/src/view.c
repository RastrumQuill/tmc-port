/**
 * @file view.c
 * @brief Extended view: shows more of the world than the GBA's 240x160.
 *
 * The game logic still runs with its original 240x160 camera; the PC view is
 * a larger window centered on it:
 *
 *  +------------------------------------------------+  view (gPortViewWidth x gPortViewHeight)
 *  |                                                |
 *  |         +-------------------+                  |
 *  |         |  classic 240x160  |  <- camera of    |
 *  |         |  (the game's own  |     the game     |
 *  |         |   picture)        |                  |
 *  |         +-------------------+                  |
 *  |                                                |
 *  +------------------------------------------------+
 *
 * - The two map layers are drawn from the pre-rendered tile map of the whole
 *   room (gMapDataBottomSpecial / gMapDataTopSpecial), so there is real level
 *   geometry everywhere, not just inside the 240x160 window.
 * - The view is kept inside the room when the room is big enough and centered
 *   (with black borders) when it is smaller.
 * - Sprites are culled against the whole view (gPortScreen*), so entities and
 *   on-screen checks work in the extra area too.
 * - The UI layer (BG0) stays in the classic area.
 * - Outside of normal gameplay (title, menus, map, cutscene subtasks) the
 *   classic 240x160 picture is shown.
 *
 * Everything is latched at vblank, matching when the hardware latches the
 * registers, VRAM and OAM the next frame is displayed with.
 */
#include "port.h"

#include <stdlib.h>

#include "global.h"
#include "game.h"
#include "main.h"
#include "map.h"
#include "room.h"
#include "screen.h"

extern u16 gMapDataTopSpecial[];
extern u16 gMapDataBottomSpecial[];

/* full room tile map: 128x128 8x8 tiles */
#define SPECIAL_MAP_STRIDE 128

int gPortClipLeft, gPortClipTop, gPortClipRight = GBA_WIDTH, gPortClipBottom = GBA_HEIGHT;
bool gPortSpriteEdgeFade;
int gPortScreenLeft = 0;
int gPortScreenTop = 0;
int gPortScreenWidth = GBA_WIDTH;
int gPortScreenHeight = GBA_HEIGHT;

static bool sActive;

bool View_IsExtendedActive(void) {
    return sActive;
}

static int BgIndex(const BgSettings* bg) {
    if (bg == &gScreen.bg0)
        return 0;
    if (bg == &gScreen.bg1)
        return 1;
    if (bg == (const BgSettings*)&gScreen.bg2)
        return 2;
    if (bg == (const BgSettings*)&gScreen.bg3)
        return 3;
    return -1;
}

static bool GameplayState(void) {
    if (gMain.task != TASK_GAME || gMain.state != GAMETASK_MAIN)
        return false;
    switch (gMain.substate) {
        case GAMEMAIN_CHANGEROOM:
        case GAMEMAIN_UPDATE:
        case GAMEMAIN_BARRELUPDATE:
            break;
        default:
            return false;
    }
    if ((gScreen.lcd.displayControl & 7) != 0)
        return false;
    if (gMapBottom.bgSettings == NULL && gMapTop.bgSettings == NULL)
        return false;
    if (gRoomControls.width == 0 || gRoomControls.height == 0)
        return false;
    return true;
}

static int Clamp(int v, int lo, int hi) {
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/* left edge of the view in world pixels for one axis */
static int ViewStart(int scroll, int origin, int roomSize, int classicSize, int viewSize) {
    int start;
    if (roomSize >= viewSize) {
        start = Clamp(scroll + classicSize / 2 - viewSize / 2, origin, origin + roomSize - viewSize);
    } else {
        start = origin - (viewSize - roomSize) / 2;
    }
    /* the game's own 240x160 picture must stay fully visible */
    return Clamp(start, scroll - (viewSize - classicSize), scroll);
}

static void SetupOverride(MapLayer* layer, const u16* specialMap) {
    int bg;
    PpuBgOverride* o;
    BgSettings* settings = layer->bgSettings;
    int dx, dy;
    if (settings == NULL)
        return;
    bg = BgIndex(settings);
    if (bg < 0 || bg > 3)
        return;
    o = &gPpuBgOverride[bg];
    dx = (u16)gRoomControls.scroll_x - gRoomControls.origin_x;
    dy = (u16)gRoomControls.scroll_y - gRoomControls.origin_y;
    o->enabled = true;
    o->map = specialMap;
    o->strideTiles = SPECIAL_MAP_STRIDE;
    o->widthTiles = Clamp((gRoomControls.width + 7) / 8, 0, SPECIAL_MAP_STRIDE);
    o->heightTiles = Clamp((gRoomControls.height + 7) / 8, 0, SPECIAL_MAP_STRIDE);
    /* the BG buffer holds whole 16x16 tiles starting one 8 pixel row above the camera,
     * the BG offset (including screen shake) selects the pixel inside */
    o->scrollX = (dx & ~0xF) + (s16)settings->xOffset;
    o->scrollY = (dy & ~0xF) - 8 + (s16)settings->yOffset;
    gPpuBgMode[bg] = PPU_BG_OVERRIDE;
}

float View_RoomFitScale(int outW, int outH) {
    float sx, sy, fit;
    if (gRoomControls.width <= 0 || gRoomControls.height <= 0)
        return 0;
    sx = (float)outW / gRoomControls.width;
    sy = (float)outH / gRoomControls.height;
    /* the whole room, as large as it fits */
    fit = sx < sy ? sx : sy;
    /* corridors no wider (taller) than the GBA screen fill that side, as on the GBA */
    if (gRoomControls.width <= GBA_WIDTH && sx > fit)
        fit = sx;
    if (gRoomControls.height <= GBA_HEIGHT && sy > fit)
        fit = sy;
    return fit;
}

/* the modes game code asked for (Port_ViewBgHint), with the frame they were asked in */
static struct {
    PpuBgMode mode;
    int parallax[2];
    unsigned frame;
} sBgHint[4];
static unsigned sViewFrame = 2;

void Port_ViewBgHint(int bg, PpuBgMode mode, int parallaxX, int parallaxY) {
    if (bg < 0 || bg > 3)
        return;
    sBgHint[bg].mode = mode;
    sBgHint[bg].parallax[0] = parallaxX;
    sBgHint[bg].parallax[1] = parallaxY;
    sBgHint[bg].frame = sViewFrame;
}

void View_PrepareFrame(void) {
    int i;
    sViewFrame++;
    for (i = 0; i < 4; i++) {
        gPpuBgOverride[i].enabled = false;
        gPpuBgMode[i] = PPU_BG_CLASSIC_ONLY;
    }

    sActive = gPortConfig.extendedView && GameplayState();
    Port_UpdateViewSize();
    gPpuHudAnchor = false;
    gPortClipLeft = 0;
    gPortClipTop = 0;
    gPortClipRight = gPortViewWidth;
    gPortClipBottom = gPortViewHeight;
    gPortSpriteEdgeFade = false;

    if (!sActive || (gPortViewWidth == GBA_WIDTH && gPortViewHeight == GBA_HEIGHT)) {
        gPortViewOffsetX = (gPortViewWidth - GBA_WIDTH) / 2;
        gPortViewOffsetY = (gPortViewHeight - GBA_HEIGHT) / 2;
        gPortScreenLeft = 0;
        gPortScreenTop = 0;
        gPortScreenWidth = GBA_WIDTH;
        gPortScreenHeight = GBA_HEIGHT;
        return;
    }

    {
        int scrollX = gRoomControls.scroll_x;
        int scrollY = gRoomControls.scroll_y;
        int viewX = ViewStart(scrollX, gRoomControls.origin_x, gRoomControls.width, GBA_WIDTH, gPortViewWidth);
        int viewY = ViewStart(scrollY, gRoomControls.origin_y, gRoomControls.height, GBA_HEIGHT, gPortViewHeight);
        gPortViewOffsetX = scrollX - viewX;
        gPortViewOffsetY = scrollY - viewY;
        /* the room, and always the classic screen */
        gPortClipLeft = Clamp(gRoomControls.origin_x - viewX, 0, gPortViewOffsetX);
        gPortClipTop = Clamp(gRoomControls.origin_y - viewY, 0, gPortViewOffsetY);
        gPortClipRight = Clamp(gRoomControls.origin_x + gRoomControls.width - viewX, gPortViewOffsetX + GBA_WIDTH,
                               gPortViewWidth);
        gPortClipBottom = Clamp(gRoomControls.origin_y + gRoomControls.height - viewY,
                                gPortViewOffsetY + GBA_HEIGHT, gPortViewHeight);
        gPortSpriteEdgeFade = true;
    }
    gPortScreenLeft = -gPortViewOffsetX;
    gPortScreenTop = -gPortViewOffsetY;
    gPortScreenWidth = gPortViewWidth;
    gPortScreenHeight = gPortViewHeight;

    SetupOverride(&gMapBottom, gMapDataBottomSpecial);
    SetupOverride(&gMapTop, gMapDataTopSpecial);
    gPpuHudAnchor = gPortConfig.hudAnchor;
    if (getenv("TMC_VIEW_LOG")) {
        int i;
        Port_Log("room origin %d,%d size %dx%d scroll %d,%d view %dx%d offset %d,%d", gRoomControls.origin_x,
                 gRoomControls.origin_y, gRoomControls.width, gRoomControls.height, gRoomControls.scroll_x,
                 gRoomControls.scroll_y, gPortViewWidth, gPortViewHeight, gPortViewOffsetX, gPortViewOffsetY);
        for (i = 0; i < 4; i++)
            if (gPpuBgOverride[i].enabled)
                Port_Log("  bg%d override scroll %d,%d tiles %dx%d", i, gPpuBgOverride[i].scrollX,
                         gPpuBgOverride[i].scrollY, gPpuBgOverride[i].widthTiles, gPpuBgOverride[i].heightTiles);
    }
    /*
     * BG1-BG3 layers that are not a room map are repeating layers: sky, clouds,
     * fog, light rays, darkness, or the clover border of the Minish paths. They
     * repeat over the view (clipped to the room by the renderer).
     */
    {
        int bg;
        for (bg = 1; bg < 4; bg++) {
            if (gPpuBgMode[bg] != PPU_BG_CLASSIC_ONLY)
                continue;
            gPpuBgMode[bg] = PPU_BG_WRAP;
            /* asked for in this frame's game update or the one before */
            if (sViewFrame - sBgHint[bg].frame <= 1) {
                gPpuBgMode[bg] = sBgHint[bg].mode;
                gPpuBgParallax[bg][0] = sBgHint[bg].parallax[0];
                gPpuBgParallax[bg][1] = sBgHint[bg].parallax[1];
            }
        }
    }
}
