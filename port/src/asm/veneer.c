/**
 * @file veneer.c
 * @brief C translation of asm/src/veneer.s and the tile query routines that
 * the GBA version runs from IWRAM (asm/src/intr.s, arm_GetTile*).
 */
#include "asm_common.h"
#include "map_ptrs.h"
#include "asm.h"

extern u8 gUpdateVisibleTiles;
extern u16 gMapDataBottomSpecial[];
extern u16 gMapDataTopSpecial[];
extern u16 gBG1Buffer[];
extern u16 gBG2Buffer[];
extern const u8 gMapSpecialTileToActTile[];
extern const u8 gMapSpecialTileToCollisionData[];
extern const u8 gMapTileTypeToActTile[];
extern const u8 gMapTileTypeToCollisionData[];
extern const u16 gUnk_08000360[];
extern const u16 gUnk_080B7A3E[];
void RegisterInteractTile(u32 tileIndex, u32 tilePos, u32 layer);
void UnregisterInteractTile(u32 tilePos, s32 layer);
void sub_0807D280(u16* mapSpecial, u16* bgBuffer);
void sub_0807D46C(u16* mapSpecial, u16* bgBuffer);
void sub_0807D6D8(u16* mapSpecial, u16* bgBuffer);
void ram_sub_080B197C(u16* mapSpecial, u16* bgBuffer);

void* const gMapDataPtrs[8] = {
    gMapBottom.mapData, gMapBottom.tileTypes, gMapBottom.mapData, gMapBottom.tileTypes,
    gMapTop.mapData,    gMapTop.tileTypes,    gMapBottom.mapData, gMapBottom.tileTypes,
};

u8* const gCollisionDataPtrs[4] = {
    gMapBottom.collisionData,
    gMapBottom.collisionData,
    gMapTop.collisionData,
    gMapBottom.collisionData,
};

void* const gUnk_08000258[8] = {
    gMapBottom.mapDataOriginal, gMapBottom.tileTypes, gMapBottom.mapDataOriginal, gMapBottom.tileTypes,
    gMapTop.mapDataOriginal,    gMapTop.tileTypes,    gMapBottom.mapDataOriginal, gMapBottom.tileTypes,
};

u8* const gActTilePtrs[4] = {
    gMapBottom.actTiles,
    gMapBottom.actTiles,
    gMapTop.actTiles,
    gMapBottom.actTiles,
};

static u16* const sTileIndicesPtrs[4] = {
    gMapBottom.tileIndices,
    gMapBottom.tileIndices,
    gMapTop.tileIndices,
    gMapBottom.tileIndices,
};

/* Copies from gMapData*Special to the BG buffers depending on gUpdateVisibleTiles. */
void UpdateScrollVram(void) {
    static void (*const sUpdaters[])(u16*, u16*) = {
        ram_sub_080B197C,
        sub_0807D280,
        sub_0807D46C,
        sub_0807D6D8,
    };
    void (*fn)(u16*, u16*);
    u32 mode = gUpdateVisibleTiles;
    if (mode == 0 || mode > 4)
        return;
    fn = sUpdaters[mode - 1];
    if (gMapBottom.bgSettings != NULL)
        fn(gMapDataBottomSpecial, gBG1Buffer + 0x20);
    if (gMapTop.bgSettings != NULL)
        fn(gMapDataTopSpecial, gBG2Buffer + 0x20);
}

void SetCollisionData(u32 collisionData, u32 tilePos, u32 layer) {
    gCollisionDataPtrs[layer & 3][tilePos] = collisionData;
}

void SetActTileAtTilePos(u32 actTile, u32 tilePos, u32 layer) {
    gActTilePtrs[layer & 3][tilePos] = actTile;
}

/* r0 = tile index, r1 = tile position, r2 = layer */
void SetTile(u32 tileIndex, u32 tilePos, u32 layer) {
    u16* mapData = LAYER_MAPDATA(layer & 3);
    u32 oldTile = mapData[tilePos];
    mapData[tilePos] = tileIndex;
    if (tileIndex >= 0x4000) {
        u32 special = tileIndex - 0x4000;
        SetActTileAtTilePos(gMapSpecialTileToActTile[special], tilePos, layer);
        SetCollisionData(gMapSpecialTileToCollisionData[special], tilePos, layer);
        UnregisterInteractTile(tilePos, layer);
        RegisterInteractTile(oldTile, tilePos, layer);
    } else {
        u32 tileType = LAYER_TILETYPES(layer & 3)[tileIndex];
        SetActTileAtTilePos(gMapTileTypeToActTile[tileType], tilePos, layer);
        SetCollisionData(gMapTileTypeToCollisionData[tileType], tilePos, layer);
        UnregisterInteractTile(tilePos, layer);
    }
}

/* Sets the tile with the given tile type. */
void CloneTile(u32 tileType, u32 tilePos, u32 layer) {
    SetTile(sTileIndicesPtrs[layer & 3][tileType], tilePos, layer);
}

u32 GetTileIndex(u32 tilePos, u32 layer) {
    return LAYER_MAPDATA(layer & 3)[tilePos];
}

/* ---- tile queries (originally ARM code in IWRAM) ---- */

#define ROOM_TILE(v) (((u32)(v) >> 4) & 0x3F)

static u32 TileTypeFromTables(void* const* tables, u32 tilePos, u32 layer) {
    u32 tile = ((u16*)tables[(layer & 3) * 2])[tilePos];
    if (tile >= 0x4000)
        return tile;
    return ((u16*)tables[(layer & 3) * 2 + 1])[tile];
}

u32 GetTileTypeAtTilePos(u32 tilePos, u32 layer) {
    return TileTypeFromTables(gMapDataPtrs, tilePos, layer);
}

u32 GetTileTypeAtRoomTile(u32 tileX, u32 tileY, u32 layer) {
    return GetTileTypeAtTilePos(tileX + (tileY << 6), layer);
}

u32 GetTileTypeAtRoomCoords(u32 roomX, u32 roomY, u32 layer) {
    return GetTileTypeAtRoomTile(ROOM_TILE(roomX), ROOM_TILE(roomY), layer);
}

u32 GetTileTypeAtWorldCoords(s32 worldX, s32 worldY, u32 layer) {
    return GetTileTypeAtRoomCoords(worldX - gRoomControls.origin_x, worldY - gRoomControls.origin_y, layer);
}

u32 GetTileTypeAtEntity(Entity* this) {
    return GetTileTypeAtWorldCoords(this->x.HALF_U.HI, this->y.HALF_U.HI, this->collisionLayer);
}

u32 GetTileTypeRelativeToEntity(Entity* this, s32 xOffset, s32 yOffset) {
    return GetTileTypeAtWorldCoords(this->x.HALF_U.HI + xOffset, this->y.HALF_U.HI + yOffset, this->collisionLayer);
}

/* Like GetTileTypeAtEntity but based on the original (unmodified) map data. */
u32 GetTileAtEntityPos(Entity* this) {
    u32 x = this->x.HALF_U.HI - gRoomControls.origin_x;
    u32 y = this->y.HALF_U.HI - gRoomControls.origin_y;
    return TileTypeFromTables(gUnk_08000258, ROOM_TILE(x) + (ROOM_TILE(y) << 6), this->collisionLayer);
}

u32 GetActTileAtTilePos(u16 tilePos, u8 layer) {
    return gActTilePtrs[layer & 3][tilePos];
}

u32 GetActTileAtRoomTile(u32 tileX, u32 tileY, u32 layer) {
    return GetActTileAtTilePos(tileX + (tileY << 6), layer);
}

u32 GetActTileAtRoomCoords(u32 roomX, u32 roomY, u32 layer) {
    return GetActTileAtRoomTile(ROOM_TILE(roomX), ROOM_TILE(roomY), layer);
}

u32 GetActTileAtWorldCoords(u32 worldX, u32 worldY, u32 layer) {
    return GetActTileAtRoomCoords(worldX - gRoomControls.origin_x, worldY - gRoomControls.origin_y, layer);
}

u32 GetActTileAtEntity(Entity* this) {
    return GetActTileAtWorldCoords(this->x.HALF_U.HI, this->y.HALF_U.HI, this->collisionLayer);
}

u32 GetActTileRelativeToEntity(Entity* this, s32 xOffset, s32 yOffset) {
    return GetActTileAtWorldCoords(this->x.HALF_U.HI + xOffset, this->y.HALF_U.HI + yOffset, this->collisionLayer);
}

u32 GetCollisionDataAtTilePos(u32 tilePos, u32 layer) {
    return gCollisionDataPtrs[layer & 3][tilePos];
}

u32 GetCollisionDataAtRoomTile(u32 tileX, u32 tileY, u32 layer) {
    return GetCollisionDataAtTilePos(tileX + (tileY << 6), layer);
}

u32 GetCollisionDataAtRoomCoords(u32 roomX, u32 roomY, u32 layer) {
    return GetCollisionDataAtRoomTile(ROOM_TILE(roomX), ROOM_TILE(roomY), layer);
}

u32 GetCollisionDataAtWorldCoords(u32 worldX, u32 worldY, u32 layer) {
    return GetCollisionDataAtRoomCoords(worldX - gRoomControls.origin_x, worldY - gRoomControls.origin_y, layer);
}

u32 GetCollisionDataAtEntity(Entity* this) {
    return GetCollisionDataAtWorldCoords(this->x.HALF_U.HI, this->y.HALF_U.HI, this->collisionLayer);
}

u32 GetCollisionDataRelativeTo(Entity* this, s32 xOffset, s32 yOffset) {
    return GetCollisionDataAtWorldCoords(this->x.HALF_U.HI + xOffset, this->y.HALF_U.HI + yOffset,
                                         this->collisionLayer);
}

u32 GetActTileForTileType(u32 tileType) {
    if (tileType & 0x4000)
        return gMapSpecialTileToActTile[tileType & 0x3FFF];
    return gMapTileTypeToActTile[tileType & 0x3FFF];
}

u32 sub_080B1B68(u32 tileIndex, u32 layer) {
    if (tileIndex >> 14)
        return tileIndex;
    return LAYER_TILETYPES(layer & 3)[tileIndex];
}

u32 sub_080B1B84(u32 tilePos, u32 layer) {
    u32 tileType = GetTileTypeAtTilePos(tilePos, layer);
    const u16* table = (tileType & 0x4000) ? gUnk_080B7A3E : gUnk_08000360;
    return table[tileType & 0x3FFF];
}

u32 sub_080B1BA4(u32 tilePos, u32 layer, u32 mask) {
    return sub_080B1B84(tilePos, layer) & mask;
}

/* actTile at the entity position plus an offset */
u32 sub_080B1BCC(Entity* this, s32 dx, s32 dy) {
    u32 x = dx + this->x.HALF_U.HI - gRoomControls.origin_x;
    u32 y = dy + this->y.HALF_U.HI - gRoomControls.origin_y;
    u32 index = ((x & 0x3F0) + ((y & 0x3F0) << 6)) >> 4;
    return gActTilePtrs[this->collisionLayer & 3][index];
}

/* Called when gUpdateVisibleTiles == 1: copy the visible part of the room map into the BG buffer. */
void ram_sub_080B197C(u16* mapSpecial, u16* bgBuffer) {
    u32 dx = (u32)(u16)gRoomControls.scroll_x - gRoomControls.origin_x;
    u32 dy = (u32)(u16)gRoomControls.scroll_y - gRoomControls.origin_y;
    u32 offset = ((dx >> 4) + ((dy >> 4) << 7)) * 2; /* in u16 units */
    const u16* src = mapSpecial + offset;
    u16* dest = bgBuffer - 0x20;
    int rows = 0x17;
    if (dy < 8) {
        /* first row comes from the current metatile row */
        DmaSet(3, src, dest, 0x80000020);
        dest += 0x20;
        rows--;
    } else {
        src -= 0x80;
    }
    while (rows-- > 0) {
        DmaSet(3, src, dest, 0x80000020);
        src += 0x80;
        dest += 0x20;
    }
}
