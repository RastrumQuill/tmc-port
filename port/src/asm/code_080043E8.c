/**
 * @file code_080043E8.c
 * @brief C translation of asm/src/code_080043E8.s.
 */
#include "asm_common.h"
#include "asm_internal.h"
#include "asm.h"
#include "sound.h"

extern u8 gUnk_02024048;
extern u16 gUnk_02021F20[];
extern const u16 gUnk_080C93E0[];
bool32 PlayerCanBeMoved(void);
void CalculateEntityTileCollisions(Entity* this, u32 direction, u32 collisionType);
bool32 ProcessMovementInternal(Entity*, s32, s32, u32);
void sub_08079E58(s32 speed, u32 direction);
u32 UpdateCollisionLayer(Entity*);
void CreatePitFallFx(Entity*);
void CreateDrownFx(Entity*);
void CreateLavaDrownFx(Entity*);
void CreateSwampDrownFx(Entity*);

typedef struct {
    u16 tile;
    u16 hazard;
} HazardTile;

static const HazardTile sHazardList[] = {
    { 0xD, 1 }, { 0x10, 2 }, { 0x11, 2 }, { 0x5A, 3 }, { 0x13, 4 }, { 0, 0 },
};

u32 GetTileHazardType(Entity* this) {
    u32 actTile;
    const HazardTile* h;
    if (this->action == 0)
        return 0;
    actTile = UpdateCollisionLayer(this);
    if (this->z.HALF.HI < 0 || actTile == 0)
        return 0;
    for (h = sHazardList; h->tile != 0; h++) {
        if (h->tile == actTile)
            return h->hazard;
    }
    return 0;
}

static void (*const sHazardFx[])(Entity*) = {
    CreatePitFallFx,
    CreateDrownFx,
    CreateLavaDrownFx,
    CreateSwampDrownFx,
};

u32 sub_0800442E(Entity* this) {
    u32 hazard = GetTileHazardType(this);
    if (hazard == 0)
        return 0;
    sHazardFx[hazard - 1](this);
    return 1;
}

void CalcCollisionStaticEntity(Entity* target, Entity* origin) {
    Asm_CalcCollisionStaticEntity(target, origin);
}

u32 sub_0800445C(Entity* this) {
    if (!PlayerCanBeMoved())
        return 0;
    if (!Asm_CalcCollisionStaticEntity(this, &gPlayerEntity.base))
        return 0;
    if (gPlayerEntity.base.action == 2)
        gPlayerEntity.base.subAction = 3;
    return 1;
}

void EnqueueSFX(u32 sfx) {
    u8 count = gUnk_02024048;
    if (count < 8) {
        gUnk_02024048 = count + 1;
        gUnk_02021F20[count] = sfx;
    }
}

void SoundReqClipped(Entity* this, u32 sfx) {
    if (CheckOnScreen(this))
        SoundReq(sfx);
}

void sub_080044AE(Entity* this, u32 speed, u32 direction) {
    if (this == &gPlayerEntity.base) {
        sub_08079E58(speed, direction);
        return;
    }
    CalculateEntityTileCollisions(this, direction, 2);
    ProcessMovementInternal(this, speed, direction, 2);
}

u32 BounceUpdate(Entity* this, u32 acceleration) {
    s32 z = this->z.WORD - this->zVelocity;
    u32 v, result = 1;
    if (z < 0) {
        this->z.WORD = z;
        this->zVelocity -= acceleration;
        return 2;
    }
    this->z.WORD = 1;
    v = -(this->zVelocity - (s32)acceleration);
    v >>= 1;
    v += v >> 2;
    if ((v >> 12) < 0xC) {
        result = 0;
        v = 0;
    }
    this->zVelocity = v;
    return result;
}

static void SetLayer(Entity* this, u32 layer, u32 spriteBits) {
    this->collisionLayer = layer;
    U8AT(this, 0x1b) = (U8AT(this, 0x1b) & ~0xC0) + spriteBits;
    U8AT(this, 0x19) = (U8AT(this, 0x19) & ~0xC0) + spriteBits;
}

void sub_08004542(Entity* this) {
    SetLayer(this, 2, 0x40);
}

void ResetCollisionLayer(Entity* this) {
    SetLayer(this, 1, 0x80);
}

void sub_0800451C(Entity* this) {
    switch (GetActTileAtEntity(this)) {
        case 0xC:
        case 0xB:
            sub_08004542(this);
            break;
        case 0xA:
        case 0x9:
            ResetCollisionLayer(this);
            break;
        case 0x26:
        case 0x27:
            SetLayer(this, 3, 0x40);
            break;
    }
}

/** Turn the direction one step towards the target direction. */
void sub_08004596(Entity* this, u32 target) {
    u32 dir = this->direction;
    if (dir < 0x20) {
        u32 diff = target - dir;
        if (diff == 0)
            return;
        target = dir + (((diff & 0x1F) < 0x10) ? 1 : -1);
    }
    this->direction = target & 0x1F;
}

u32 sub_080045B4(Entity* this, u32 x, u32 y) {
    return Asm_CalcCollisionDirection(this->x.HALF.HI, this->y.HALF.HI, x, y);
}

u32 GetFacingDirection(Entity* origin, Entity* target) {
    return Asm_CalcCollisionDirection(origin->x.HALF.HI, origin->y.HALF.HI, target->x.HALF.HI, target->y.HALF.HI);
}

u32 CalculateDirectionTo(u32 x1, u32 y1, u32 x2, u32 y2) {
    return Asm_CalcCollisionDirection(x1, y1, x2, y2);
}

u32 CalculateDirectionFromOffsets(s32 x, s32 y) {
    u32 angle = 0x40;
    if (x != 0) {
        s32 q = (y << 8) / x;
        u32 absQ = q < 0 ? -q : q;
        u32 i, end;
        if (absQ < 0x106) {
            if (absQ < 0x6e) {
                angle = 0;
                i = 0;
                end = 0x20;
            } else {
                i = 0x20;
                end = 0x40;
            }
        } else if (absQ < 0x280) {
            i = 0x40;
            end = 0x60;
        } else {
            i = 0x60;
            end = 0x7e;
        }
        /* i and end are byte offsets into the u16 table */
        for (; i < end; i += 2) {
            const u16* e = (const u16*)((const u8*)gUnk_080C93E0 + i);
            if (absQ >= e[0] && absQ < e[1]) {
                angle = (i >> 1) + 1;
                break;
            }
        }
    }
    if (x >= 0)
        return y < 0 ? 0x40 - angle : 0x40 + angle;
    return y < 0 ? 0xC0 + angle : 0xC0 - angle;
}
