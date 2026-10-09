/**
 * @file tileswap.c
 * @brief Tile graphics for the extended view in rooms that swap tile sets.
 *
 * Hyrule Town and Minish Village are too big for one set of tile graphics.
 * Their managers load a different set into VRAM depending on which region of
 * the room the camera shows (CheckRegionsOnScreen), and the GBA screen only
 * ever shows tiles of the loaded set. The extended view shows more of the
 * room, so tiles far from the camera would be drawn with the wrong graphics.
 *
 * The managers register their tables here (slots of VRAM, the graphics and
 * palettes of each group). For a tile in the extended area, the renderer asks
 * which group the game would load if the camera were centred on that tile,
 * and reads that group's graphics from the game data instead of VRAM.
 */
#include "port.h"

#include <string.h>

#include "global.h"
#include "room.h"

#define MAX_SLOTS 3
#define MAX_GROUPS 8
#define MAX_CHUNKS 8

typedef struct {
    const uint8_t* src;
    uint32_t dest; /* VRAM offset */
    uint32_t size;
} Chunk;

typedef struct {
    const uint16_t* regions; /* {group, x, y, w, h}..., 0xff */
    int chunkCount[MAX_GROUPS];
    Chunk chunks[MAX_GROUPS][MAX_CHUNKS];
    int paletteGroup[MAX_GROUPS]; /* -1: none */
    uint16_t palette[MAX_GROUPS][256];
} Slot;

/* the layout of the game's palette group entries (src/common.c) */
typedef struct {
    u16 paletteId;
    u8 destPaletteNum;
    u8 numPalettes;
} PortPaletteGroup;

extern const PortPaletteGroup* gPaletteGroups[];
extern const u8 gGlobalGfxAndPalettes[];

static Slot sSlots[MAX_SLOTS];
static bool sUsed[MAX_SLOTS];
static int sArea = -1, sRoom = -1;
static bool sActive;

void Port_TileSwapRegister(int slot, const uint16_t* regions) {
    int g;
    if (slot < 0 || slot >= MAX_SLOTS)
        return;
    if (sArea != gRoomControls.area || sRoom != gRoomControls.room) {
        memset(sUsed, 0, sizeof(sUsed));
        sArea = gRoomControls.area;
        sRoom = gRoomControls.room;
    }
    sUsed[slot] = true;
    sSlots[slot].regions = regions;
    for (g = 0; g < MAX_GROUPS; g++) {
        sSlots[slot].chunkCount[g] = 0;
        sSlots[slot].paletteGroup[g] = -1;
    }
}

void Port_TileSwapAddChunk(int slot, int group, const void* src, const void* dest, uint32_t size) {
    Slot* s;
    Chunk* c;
    if (slot < 0 || slot >= MAX_SLOTS || group < 0 || group >= MAX_GROUPS)
        return;
    s = &sSlots[slot];
    if (s->chunkCount[group] >= MAX_CHUNKS)
        return;
    c = &s->chunks[group][s->chunkCount[group]++];
    c->src = src;
    c->dest = (uint32_t)((uintptr_t)dest - PORT_VRAM_ADDR);
    c->size = size;
}

void Port_TileSwapSetPaletteGroup(int slot, int group, int paletteGroup) {
    if (slot < 0 || slot >= MAX_SLOTS || group < 0 || group >= MAX_GROUPS)
        return;
    sSlots[slot].paletteGroup[group] = paletteGroup;
}

/* the background palettes with a group's palettes loaded over the current ones */
static void BuildPalette(uint16_t* out, int paletteGroup) {
    const PortPaletteGroup* p = gPaletteGroups[paletteGroup];
    memcpy(out, (const void*)(uintptr_t)PORT_PLTT_ADDR, 256 * sizeof(uint16_t));
    for (;;) {
        int dest = p->destPaletteNum;
        int n = p->numPalettes & 0xF;
        if (n == 0)
            n = 16;
        if (dest < 16) {
            if (dest + n > 16)
                n = 16 - dest;
            memcpy(out + dest * 16, &gGlobalGfxAndPalettes[p->paletteId * 32], n * 32);
        }
        if (!(p->numPalettes & 0x80))
            break;
        p++;
    }
}

void Port_TileSwapPrepareFrame(void) {
    int i, g;
    sActive = sArea == gRoomControls.area && sRoom == gRoomControls.room;
    if (!sActive)
        return;
    for (i = 0; i < MAX_SLOTS; i++) {
        if (!sUsed[i])
            continue;
        for (g = 0; g < MAX_GROUPS; g++) {
            if (sSlots[i].paletteGroup[g] >= 0)
                BuildPalette(sSlots[i].palette[g], sSlots[i].paletteGroup[g]);
        }
    }
}

/* the group CheckRegionsOnScreen would pick with the camera at (camX, camY), room relative */
static int RegionGroup(const uint16_t* r, int camX, int camY) {
    for (; *r != 0xff; r += 5) {
        uint32_t x = (uint32_t)(camX - r[1] + GBA_WIDTH);
        uint32_t y = (uint32_t)(camY - r[2] + GBA_HEIGHT);
        if (x < (uint32_t)(r[3] + GBA_WIDTH) && y < (uint32_t)(r[4] + GBA_HEIGHT))
            return r[0];
    }
    return 0xff;
}

const uint8_t* Port_TileSwapCharData(uint32_t charBase, uint16_t entry, bool bpp8, int mx, int my,
                                     const uint16_t** palette) {
    uint32_t addr = charBase + (entry & 0x3FF) * (bpp8 ? 64 : 32);
    int i, c;
    if (!sActive)
        return NULL;
    for (i = 0; i < MAX_SLOTS; i++) {
        const Slot* s = &sSlots[i];
        int group;
        if (!sUsed[i])
            continue;
        group = RegionGroup(s->regions, mx - GBA_WIDTH / 2, my - GBA_HEIGHT / 2);
        if (group >= MAX_GROUPS || group == gRoomVars.graphicsGroups[i])
            continue;
        for (c = 0; c < s->chunkCount[group]; c++) {
            const Chunk* ch = &s->chunks[group][c];
            if (addr >= ch->dest && addr < ch->dest + ch->size) {
                if (s->paletteGroup[group] >= 0)
                    *palette = s->palette[group];
                /* a base that the VRAM offset indexes */
                return (const uint8_t*)((uintptr_t)ch->src - ch->dest);
            }
        }
    }
    return NULL;
}
