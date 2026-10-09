/**
 * @file player_asm.c
 * @brief C translation of asm/src/player.s (player tile collisions, tile
 * interactions and ice physics).
 */
#include "asm_common.h"
#include "map_ptrs.h"
#include "asm.h"
#include "object.h"

extern const u8 gUnk_080082DC[];
extern const u8 gUnk_0800833C[];
extern const u8 gUnk_0800839C[];
extern const u8 gUnk_080083FC[];
extern const u8 gUnk_0800845C[];
extern const u8 gUnk_080084BC[];
extern const u8 gUnk_0800851C[];
extern const u16* const gUnk_0800823C[];
extern const s8 gUnk_08007DF4[];
extern const KeyValuePair gUnk_080046A4[];
extern const u8 gUnk_080047F6[];
extern const KeyValuePair gMapActTileToSurfaceType[];
extern const s16 gSineTable[];

const KeyValuePair* FindEntryForKey(u32 key, const KeyValuePair* list);
void LinearMoveDirectionOLD(Entity* this, u32 speed, u32 dir);
void ResetPlayerVelocity(void);
void sub_0807A5B8(u32);
void sub_0807B7D8(u32 tileType, u32 tilePos, u32 layer);
void RestorePrevTileEntity(u32 tilePos, u32 layer);
void RespawnPlayer(void);
void UpdateSpriteForCollisionLayer(Entity*);

/* sub_080086D8: is the sub tile at room coordinates (x, y) solid? */
static u32 ProbeCollision(Entity* this, const u8* table, u32 x, u32 y) {
    u32 index = ((x & 0x3F0) >> 4) + ((y & 0x3F0) << 2);
    u32 c = gCollisionDataPtrs[this->collisionLayer & 3][index];
    if (gPlayerState.swim_state != 0 && gPlayerState.floor_type != 0x18) {
        if (c < 0x10)
            c = 0xF;
    }
    if (c < 0x10) {
        if (!(y & 8))
            c >>= 2;
        if (!(x & 8))
            c >>= 1;
    } else if (c != 0xFF) {
        const u16* rows = gUnk_0800823C[table[c - 0x10]];
        c = rows[y & 0xF] >> ((x & 0xF) ^ 0xF);
    }
    return c & 1;
}

static const u8* GetPlayerCollisionTable(void) {
    if (gPlayerState.swim_state != 0)
        return (gPlayerState.flags & PL_MINISH) ? gUnk_0800839C : gUnk_080083FC;
    if (gPlayerState.jump_status != 0 || (gPlayerState.flags & 0x01000000))
        return gUnk_0800845C;
    if (gPlayerState.flags & PL_MINISH)
        return gUnk_0800833C;
    if (gPlayerState.gustJarState != 0 || gPlayerState.heldObject != 0)
        return gUnk_080084BC;
    if (gPlayerState.attachedBeetleCount != 0)
        return gUnk_0800851C;
    return gUnk_080082DC;
}

/* Computes the collision flags of the entity by probing 8 points around its hitbox. */
static void sub_080085CC(Entity* this) {
    const u8* table = GetPlayerCollisionTable();
    Hitbox* hb = this->hitbox;
    u32 bits = 0;
    s32 cx = hb->offset_x + (s32)(this->x.HALF_U.HI - gRoomControls.origin_x);
    s32 cy = hb->offset_y + (s32)(this->y.HALF_U.HI - gRoomControls.origin_y);
    s32 x, y;
    int i;

    /* left/right sides */
    x = cx + hb->unk2[0];
    for (i = 0; i < 2; i++) {
        bits <<= 2;
        bits |= ProbeCollision(this, table, x, cy + hb->unk2[1]);
        bits <<= 1;
        bits |= ProbeCollision(this, table, x, cy - hb->unk2[1]);
        bits <<= 1;
        x -= hb->unk2[0] * 2;
    }
    /* top/bottom sides */
    y = cy + hb->unk2[3];
    for (i = 0; i < 2; i++) {
        bits <<= 2;
        bits |= ProbeCollision(this, table, cx + hb->unk2[2], y);
        bits <<= 1;
        bits |= ProbeCollision(this, table, cx - hb->unk2[2], y);
        bits <<= 1;
        y -= hb->unk2[3] * 2;
    }
    this->collisions = (u16)bits;
}

void sub_0800857C(Entity* this) {
    if (!(this->type2 & 0x80) && !(gPlayerState.jump_status & 0x80))
        sub_080085CC(this);
    LinearMoveDirectionOLD(this, (u16)this->speed, this->direction);
}

void sub_080085B0(Entity* this) {
    sub_080085CC(this);
}

u32 sub_080086B4(u32 x, u32 y, const u8* table) {
    return ProbeCollision(&gPlayerEntity.base, table, x, y);
}

/* Entry of gUnk_080047F6: u16 filterMask, u8 objectId, u8 objectType, u8, u8, u16 newTile */
u16* DoTileInteraction(Entity* this, u32 filter, u32 x, u32 y) {
    const KeyValuePair* kv;
    const u8* entry;
    u32 id, type, tilePos, layer, newTile;

    if (gRoomControls.reload_flags == 1)
        return NULL;
    kv = FindEntryForKey(GetTileTypeAtWorldCoords(x, y, this->collisionLayer), gUnk_080046A4);
    if (kv == NULL)
        return NULL;
    entry = gUnk_080047F6 + kv->value * 8;
    if (!((*(const u16*)entry >> filter) & 1))
        return NULL;

    id = entry[2];
    type = entry[3];
    if (id != 0xFF && filter != 6 && filter != 0xE && filter != 0xA && filter != 0xB &&
        (filter != 0xD || (id == 0xF && type == 0x17))) {
        Entity* obj = CreateObject(id, type, id == 0xF ? 0x80 : 0);
        if (obj != NULL) {
            if (entry[2] != 0) {
                obj->x.HALF.HI = (x & ~0xF) + 8;
                obj->y.HALF.HI = (y & ~0xF) + 8;
            } else {
                obj->x.HALF.HI = this->x.HALF.HI;
                obj->y.HALF.HI = this->y.HALF.HI;
                /* The original also writes z and the parent pointer through a wrong base register,
                 * which lands in the (read only) BIOS area on hardware and has no effect. */
            }
            obj->collisionLayer = this->collisionLayer;
            UpdateSpriteForCollisionLayer(obj);
        }
    }

    tilePos = ((x - gRoomControls.origin_x) >> 4) + (((y - gRoomControls.origin_y) >> 4) << 6);
    layer = this->collisionLayer;
    newTile = *(const u16*)(entry + 6);
    if (newTile & 0x4000) {
        if (newTile == 0xFFFF) {
            RestorePrevTileEntity(tilePos, layer);
        } else {
            GetLayerByIndex(layer)->mapData[tilePos & 0xFFF] = newTile;
        }
    } else {
        sub_0807B7D8(newTile, tilePos, layer);
    }
    return (u16*)entry;
}

const u8* DoTileInteractionOffset(Entity* this, u32 filter, s32 dx, s32 dy) {
    return (const u8*)DoTileInteraction(this, filter, dx + this->x.HALF_U.HI, dy + this->y.HALF_U.HI);
}

u32* DoTileInteractionHere(Entity* this, u32 filter) {
    return (u32*)DoTileInteraction(this, filter, this->x.HALF_U.HI, this->y.HALF_U.HI);
}

s32 DoItemTileInteraction(Entity* this, u32 filter, u8* behavior) {
    u32 i = this->animationState & 6;
    const u8* result = DoTileInteractionOffset(this, filter, gUnk_08007DF4[i], gUnk_08007DF4[i + 1]);
    if (result != NULL) {
        behavior[3] = result[2];
        behavior[7] = result[3];
        behavior[8] = result[5];
    }
    return (s32)(uintptr_t)result;
}

/* ---- ice / velocity physics ---- */

static const s8 sVelocities1[] = { 0, -3, 3, -3, 3, 0, 3, 3, 0, 3, -3, 3, -3, 0, -3, -3 };
static const s8 sIceVelocities[] = { 0, -10, 10, -10, 10, 0, 10, 10, 0, 10, -10, 10, -10, 0, -10, -10 };
static const s8 sVelocities3[] = { 0, 6, -6, 0, 0, -6, 6, 0 };

static s16 ClampPlayerVelocity(s32 v) {
    if (v >= 0) {
        if (v >= 0x180)
            v = 0x180;
    } else if ((u32)v < (u32)-0x180) {
        v = -0x180;
    }
    return (s16)v;
}

static void AddPlayerVelocity(s32 dx, s32 dy) {
    gPlayerState.vel_x = ClampPlayerVelocity((s16)gPlayerState.vel_x + dx);
    gPlayerState.vel_y = ClampPlayerVelocity((s16)gPlayerState.vel_y + dy);
}

static void ApplyFriction(u16* vel) {
    s32 v = (s16)*vel;
    if (v >= 0) {
        v -= 3;
        if (v < 0)
            v = 0;
    } else {
        v += 3;
        if (v > 0)
            v = 0;
    }
    *vel = (u16)v;
}

static void MoveWithVelocity(Entity* this) {
    s32 v = (s16)gPlayerState.vel_x;
    u32 dir;
    if (v != 0) {
        dir = 8;
        if (v < 0) {
            dir = 0x18;
            v = -v;
        }
        LinearMoveDirectionOLD(this, v, dir);
        sub_0807A5B8(dir);
    }
    v = (s16)gPlayerState.vel_y;
    if (v != 0) {
        dir = 0x10;
        if (v < 0) {
            dir = 0;
            v = -v;
        }
        LinearMoveDirectionOLD(this, v, dir);
        sub_0807A5B8(dir);
    }
    if (gPlayerState.jump_status == 0) {
        ApplyFriction(&gPlayerState.vel_x);
        ApplyFriction(&gPlayerState.vel_y);
    }
}

static void Accelerate(Entity* this, u32 dir) {
    const s8* v;
    if (gPlayerState.heldObject == 2 || gPlayerState.heldObject == 1) {
        ResetPlayerVelocity();
        return;
    }
    v = sIceVelocities;
    if (gPlayerState.jump_status != 0) {
        u32 rel = (((dir >> 2) - (this->animationState & ~1)) + 2) & 7;
        v = sVelocities1;
        if (rel > 4) {
            v = &sVelocities3[this->animationState & ~1];
            AddPlayerVelocity(v[0], v[1]);
            MoveWithVelocity(this);
            return;
        }
    }
    v += (dir >> 2) << 1;
    AddPlayerVelocity(v[0], v[1]);
    MoveWithVelocity(this);
}

void sub_08008926(Entity* this) {
    u32 dir;
    if ((gPlayerState.field_0x7 | gPlayerState.field_0xa) != 0)
        return;
    dir = gPlayerState.direction;
    this->direction = dir;
    if (dir & 0x80) {
        MoveWithVelocity(this);
        return;
    }
    Accelerate(this, dir);
}

void UpdateIcePlayerVelocity(Entity* this) {
    Accelerate(this, (this->animationState >> 1) << 3);
}

void sub_08008AA0(Entity* this) {
    u32 dir;
    if (gPlayerState.floor_type == 1)
        return;
    dir = gPlayerState.direction;
    if (dir == 0xFF)
        return;
    gPlayerState.vel_x = gSineTable[dir * 8];
    gPlayerState.vel_y = -gSineTable[dir * 8 + 64];
}

/* Returns TRUE if one of the four sides has no collision. */
static bool32 HasNonCollidedSide(Entity* this) {
    u32 c = this->collisions;
    int i;
    for (i = 0; i < 4; i++, c >>= 4) {
        if (!(c & 0xE))
            return TRUE;
    }
    return FALSE;
}

void sub_08008AC6(Entity* this) {
    if ((gPlayerState.swim_state & 0xF) != 0)
        return;
    if (gPlayerState.flags & 0x02000020)
        return;
    if (HasNonCollidedSide(this))
        return;
    this->iframes = (s8)0xE2;
    RespawnPlayer();
}

static u32 CheckNEastTile(Entity* this) {
    u32 actTile = GetActTileRelativeToEntity(this, 0, 0);
    const KeyValuePair* kv;
    if (actTile & 0x4000)
        return 0;
    kv = FindEntryForKey(actTile, gMapActTileToSurfaceType);
    return kv != NULL && kv->value == 1;
}

u32 PlayerCheckNEastTile(void) {
    return CheckNEastTile(&gPlayerEntity.base);
}
