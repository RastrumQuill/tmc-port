/**
 * @file misc.c
 * @brief C translations of the small hand written assembly files:
 * code_08000E44.s, code_08000F10.s, code_08007CAC.s, script.s, enemy.s and
 * projectileUpdate.s.
 */
#include "asm_common.h"
#include "asm.h"
#include "enemy.h"
#include "object.h"
#include "functions.h"
#include "effects.h"

extern u32 gRand;
extern u8 gUnk_03003DE0;
extern u8 gUnk_03000C30[];

/* ---- code_08000E44.s ---- */

/** Sign of a number: -1, 0 or 1. */
u32 sub_08000E44(u32 value) {
    s32 v = (s32)value;
    if (v == 0)
        return 0;
    return v < 0 ? (u32)-1 : 1;
}

u32 Random(void) {
    u32 r = gRand;
    r = RotateRight(r + (r << 1), 13);
    gRand = r;
    return r >> 1;
}

/** Population count. */
u32 sub_08000E62(u32 v) {
    v = (v & 0x55555555) + ((v >> 1) & 0x55555555);
    v = (v & 0x33333333) + ((v >> 2) & 0x33333333);
    v = (v & 0x0F0F0F0F) + ((v >> 4) & 0x0F0F0F0F);
    v = (v & 0x00FF00FF) + ((v >> 8) & 0x00FF00FF);
    return (v + (v << 16)) >> 16;
}

static void QueueResource(u32 type, const void* src, void* dest, u32 size) {
    u8 count = gUnk_03003DE0;
    u8* entry;
    if (count >= 0x28)
        return;
    entry = gUnk_03000C30 + count * 12;
    gUnk_03003DE0 = count + 1;
    entry[0] = (u8)type;
    *(u16*)(entry + 2) = (u16)size;
    *(const void**)(entry + 4) = src;
    *(void**)(entry + 8) = dest;
}

/** Queue an LZ77 decompression into VRAM for the next vblank. */
void sub_08000E92(const void* src, void* dest, u32 size) {
    QueueResource(1, src, dest, size);
}

/** Queue a DMA copy for the next vblank. */
void LoadResourceAsync(const void* src, void* dest, u32 size) {
    QueueResource(0, src, dest, size);
}

/* ---- code_08000F10.s ---- */

/** Returns 1 if all count bits starting at bit are set. */
u32 CheckBits(void* base, u32 bit, u32 count) {
    const u8* p = (const u8*)base;
    do {
        if (!(p[bit >> 3] & (1 << (bit & 7))))
            return 0;
        bit++;
    } while (--count != 0);
    return 1;
}

void SumDropProbabilities(s16* out, const s16* a, const s16* b, const s16* c) {
    int i;
    for (i = 15; i >= 0; i--)
        out[i] = a[i] + b[i] + c[i];
}

u32 SumDropProbabilities2(s16* out, const s16* a, const s16* b, const s16* c) {
    s32 sum = 0;
    int i;
    for (i = 15; i >= 0; i--) {
        s32 v = a[i] + b[i] + c[i];
        if (v < 0)
            v = 0;
        out[i] = (s16)v;
        sum += v;
    }
    return sum;
}

/* ---- code_08007CAC.s ---- */

/** Searches a 0 terminated key/value list. Returns the entry or NULL. */
const KeyValuePair* FindEntryForKey(u32 key, const KeyValuePair* list) {
    for (;; list++) {
        if (list->key == 0)
            return NULL;
        if (list->key == key)
            return list;
    }
}

u32 FindValueForKey(u32 key, const KeyValuePair* list) {
    const KeyValuePair* entry = FindEntryForKey(key, list);
    return entry != NULL ? entry->value : 0;
}

void sub_08007DCE(void) {
    DoPlayerAction(&gPlayerEntity);
}

/* ---- script.s ---- */

u32 GetNextScriptCommandHalfword(u16* p) {
    return p[0];
}

u32 GetNextScriptCommandHalfwordAfterCommandMetadata(u16* p) {
    return p[1];
}

u32 GetNextScriptCommandWord(u16* p) {
    return p[0] | (p[1] << 16);
}

u32 GetNextScriptCommandWordAfterCommandMetadata(u16* p) {
    return p[1] | (p[2] << 16);
}

static const u8 sLayerSpriteBits[] = { 0x80, 0x80, 0x80, 0x80, 0x40, 0x40, 0x40, 0x40 };

void UpdateSpriteForCollisionLayer(Entity* this) {
    const u8* bits = &sLayerSpriteBits[(this->collisionLayer & 3) * 2];
    U8AT(this, 0x19) = (U8AT(this, 0x19) & ~0xC0) | bits[0];
    U8AT(this, 0x1b) = (U8AT(this, 0x1b) & ~0xC0) | bits[1];
}

typedef struct {
    u16 tile;
    u8 srcLayer;
    u8 destLayer;
} TransitionTile;

/* The first 8 entries precede gTransitionTiles in the original table. */
static const TransitionTile sTransitionTilesAll[] = {
    { 0x2A, 3, 3 }, { 0x2D, 3, 3 }, { 0x2B, 3, 3 }, { 0x2C, 3, 3 }, { 0x4C, 3, 3 }, { 0x4E, 3, 3 },
    { 0x4D, 3, 3 }, { 0x4F, 3, 3 },
    /* gTransitionTiles */
    { 0x0A, 2, 1 }, { 0x09, 2, 1 }, { 0x0C, 1, 2 }, { 0x0B, 1, 2 }, { 0x52, 3, 3 }, { 0x27, 3, 3 },
    { 0x26, 3, 3 }, { 0, 0, 0 },
};
#define gTransitionTiles (&sTransitionTilesAll[8])

void ResolveCollisionLayer(Entity* this) {
    if (this->collisionLayer == 0) {
        u32 layer = 1;
        u32 tileType = GetTileTypeAtWorldCoords(this->x.HALF_U.HI, this->y.HALF_U.HI, 2);
        if (tileType != 0) {
            /* the original search starts one entry into the table */
            const TransitionTile* t = &sTransitionTilesAll[1];
            u32 actTile = GetActTileForTileType(tileType);
            layer = 2;
            for (; t->tile != 0; t++) {
                if (t->tile == actTile) {
                    layer = t->destLayer;
                    break;
                }
            }
        }
        this->collisionLayer = layer;
    }
    UpdateSpriteForCollisionLayer(this);
}

u32 CheckOnLayerTransition(Entity* this) {
    u32 actTile = GetActTileAtEntity(this);
    const TransitionTile* t;
    for (t = gTransitionTiles; t->tile != 0; t++) {
        if (t->tile == actTile) {
            if (this->collisionLayer != t->srcLayer)
                this->collisionLayer = t->destLayer;
            break;
        }
    }
    return actTile;
}

u32 UpdateCollisionLayer(Entity* this) {
    u32 actTile = CheckOnLayerTransition(this);
    UpdateSpriteForCollisionLayer(this);
    return actTile;
}

/* ---- enemy.s ---- */

extern void (*const gEnemyFunctions[])(Entity*);
extern void (*const gProjectileFunctions[])(Entity*);
bool32 EnemyInit(Enemy* this);
bool32 ProjectileInit(Entity* this);
void Knockback1(Entity*);
void Knockback2(Entity*);

void EnemyUpdate(Entity* this) {
    if (this->action == 0) {
        if (!EnemyInit((Enemy*)this))
            DeleteThisEntity();
    } else {
        if (EntityDisabled(this))
            goto draw;
        sub_080028E0(this);
    }
    if (!(U8AT(this, 0x6d) & 0x10)) {
        gEnemyFunctions[this->id](this);
        this->contactFlags &= 0x7F;
    }
draw:
    DrawEntity(this);
}

void sub_08001214(Entity* this) {
    if (!(this->gustJarState & 1)) {
        this->gustJarState = 1; /* sic: the original stores 1, not gustJarState | 1 */
        this->timer = (this->frame & 0x40) ? 0x20 : 1;
    }
    if (--this->timer == 0) {
        CreatePitFallFx(this);
        return;
    }
    UpdateAnimationVariableFrames(this, 4);
}

static const s8 sConfusedOffsets[] = { 0, 1, 0, -1 };

void GenericConfused(Entity* this) {
    u32 t = this->confusedTime - 1;
    this->confusedTime = (u8)t;
    if (t < 0x3c) {
        this->spriteOffsetX = sConfusedOffsets[t & 3];
        if (t == 0) {
            Entity* fx = *(Entity**)((u8*)this + 0x68);
            if (fx != NULL && fx->kind == 6 && fx->id == 0xf && fx->type == 0x1c)
                EnemyDetachFX(this);
        }
    }
    GravityUpdate(this, Q_8_8(24));
}

void (*const gUnk_080012C8[])(Entity*) = {
    NULL, sub_08001214, CreateDrownFx, CreateLavaDrownFx, CreateSwampDrownFx,
};

void sub_08001290(Entity* this, u32 index) {
    if (index != 0)
        gUnk_080012C8[index](this);
}

s32 sub_080012DC(Entity* this) {
    s32 hazard;
    if (this->gustJarState & 4)
        return 0;
    hazard = GetTileHazardType(this);
    if (hazard == 4)
        return 0;
    if (hazard == 0) {
        if (this->gustJarState & 1)
            this->flags |= 0x80;
        return 0;
    }
    if (hazard != 1) {
        this->timer = 1;
        this->gustJarState |= 1;
    }
    return hazard;
}

void EnemyFunctionHandler(Entity* this, EntityActionArray functions) {
    s32 index = sub_080012DC(this);
    if (index != 0) {
        gUnk_080012C8[index](this);
    } else {
        functions[GetNextFunction(this)](this);
    }
}

void GenericKnockback(Entity* this) {
    Knockback1(this);
}

void sub_08001318(Entity* this) {
    if (this->z.HALF.HI < 0)
        this->direction = 0xFF;
    GenericKnockback(this);
}

void GenericKnockback2(Entity* this) {
    Knockback2(this);
}

u32 sub_0800132C(Entity* this, Entity* other) {
    if ((this->collisionLayer & other->collisionLayer) == 0)
        return 0xFF;
    if ((u32)(this->x.HALF_U.HI - other->x.HALF_U.HI + 8) < 0x11 &&
        (u32)(this->y.HALF_U.HI - other->y.HALF_U.HI + 8) < 0x11)
        return 0xFF;
    return GetFacingDirection(this, other);
}

/* ---- projectileUpdate.s ---- */

void ProjectileUpdate(Entity* this) {
    if (this->action == 0) {
        if (!ProjectileInit(this))
            DeleteThisEntity();
    } else {
        if (EntityDisabled(this))
            goto draw;
        sub_080028E0(this);
    }
    gProjectileFunctions[this->id](this);
    this->contactFlags &= 0x7F;
draw:
    DrawEntity(this);
}

/* ---- data/scripts/CreateDustFromScript.inc ---- */

void CreateDustFromScript(Entity* this) {
    CreateDeathFx(this);
}
