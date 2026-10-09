/**
 * @file code_08003FC4.c
 * @brief C translation of asm/src/code_08003FC4.s (gravity, visibility,
 * animation frame stepping and simple tile/collision helpers).
 */
#include "asm_common.h"
#include "map_ptrs.h"
#include "port.h"
#include "asm.h"
#include "object.h"
#include "sound.h"
#include "structures.h"
#include "vram.h"

extern const SpritePtr gSpritePtrs[];
extern u8* const gUnk_081326EC[];
extern u8 gUnk_02024048;
extern u16 gUnk_02021F20[];
u32 sub_0806F58C(Entity*, Entity*);

u32 GravityUpdate(Entity* this, u32 gravity) {
    s32 z = this->z.WORD - this->zVelocity;
    if (z >= 0) {
        this->z.WORD = 0;
        this->zVelocity = 0;
        return 0;
    }
    this->z.WORD = z;
    this->zVelocity -= gravity;
    return z;
}

static u32 CheckEntityPickup(Entity* this, Entity* other, u32 radiusX, u32 radiusY) {
    /* entities without a hitbox read the BIOS area on the GBA (open bus 0xE3A02004);
     * use those bytes explicitly so the result doesn't depend on the platform */
    static const Hitbox sOpenBusHitbox = { 0x04, 0x20, { 0xA0, 0xE3, 0x04, 0x20 }, 0xA0, 0xE3 };
    const Hitbox* hb = other->hitbox != NULL ? other->hitbox : &sOpenBusHitbox;
    s32 offsetX = hb->offset_x;
    s32 ox, oy;
    radiusX += hb->width;
    radiusY += hb->height;
    if (U8AT(other, 0x18) & 4)
        offsetX = -offsetX;
    ox = other->x.HALF_U.HI + offsetX;
    oy = other->y.HALF_U.HI + hb->offset_y;
    if ((this->collisionLayer & other->collisionLayer) == 3)
        return 0;
    if (radiusX != 0 && radiusX * 2 < (u32)(this->x.HALF_U.HI - ox + radiusX))
        return 0;
    if (radiusY != 0 && radiusY * 2 < (u32)(this->y.HALF_U.HI - oy + radiusY))
        return 0;
    return 1;
}

u32 sub_08003FDE(Entity* this, Entity* other, u32 radiusX, u32 radiusY) {
    if (!CheckEntityPickup(this, other, radiusX, radiusY))
        return 0;
    return sub_0806F58C(this, other);
}

u32 CheckOnScreen(Entity* this) {
    /* sprites may extend up to 63 pixels beyond the visible area */
    s32 x = this->x.HALF.HI - (u16)gRoomControls.scroll_x - gPortScreenLeft + 0x3f;
    s32 y;
    if ((u32)x >= (u32)(gPortScreenWidth + 2 * 0x3f))
        return 0;
    y = this->y.HALF.HI - (u16)gRoomControls.scroll_y - gPortScreenTop + this->z.HALF.HI + 0x3f;
    if ((u32)y >= (u32)(gPortScreenHeight + 2 * 0x3f))
        return 0;
    return 1;
}

u32 sub_080040A2(Entity* this) {
    /* draw modes 2 and 3 are always drawn */
    if (U8AT(this, 0x18) & 2)
        return 1;
    return CheckOnScreen(this);
}

void Port_DrawListOverflow(u32 which, Entity* e); /* intr.c */

void DrawEntity(Entity* this) {
    u32 draw = U8AT(this, 0x18) & 3;
    u8 count;
    if (draw == 0 || (draw != 3 && !sub_080040A2(this))) {
        gUnk_02024048 = 0;
        return;
    }
    {
        u8* list = gUnk_081326EC[(U8AT(this, 0x19) & 0xC0) >> 6];
        u8 n = list[0];
        if (n < 0x40) {
            n++;
            list[0] = n;
            ((Entity**)list)[n] = this;
        } else {
            Port_DrawListOverflow((U8AT(this, 0x19) & 0xC0) >> 6, this);
        }
    }
    count = gUnk_02024048;
    gUnk_02024048 = 0;
    if (count != 0) {
        u16* sfx = gUnk_02021F20;
        do {
            SoundReq(*sfx++);
        } while (--count);
    }
}

/* Checks the collision of the sub tile at x, y using a table for the special collision types. */
static u32 CheckSubTileCollision(Entity* this, const u16* table, u32 x, u32 y) {
    u32 rx = x - gRoomControls.origin_x;
    u32 ry = y - gRoomControls.origin_y;
    u32 index = ((rx & 0x3F0) >> 4) + ((ry & 0x3F0) << 2);
    u32 c = gCollisionDataPtrs[this->collisionLayer & 3][index];
    if (c < 0x10) {
        if (!(y & 8))
            c >>= 2;
        if (!(x & 8))
            c >>= 1;
    } else if (c != 0xFF) {
        u32 sx, sy;
        c = table[c - 0x10];
        sx = x & 0xF;
        if (sx >= 4) {
            c >>= 1;
            if (sx >= 8) {
                c >>= 1;
                if (sx >= 0xC)
                    c >>= 1;
            }
        }
        sy = y & 0xF;
        if (sy >= 4) {
            c >>= 4;
            if (sy >= 8) {
                c >>= 4;
                if (sy >= 0xC)
                    c >>= 4;
            }
        }
    }
    return c & 1;
}

u32 sub_080040D8(Entity* this, u8* table, s32 x, s32 y) {
    return CheckSubTileCollision(this, (const u16*)table, x, y);
}

bool32 sub_080040E2(Entity* this, u8* table) {
    return CheckSubTileCollision(this, (const u16*)table, this->x.HALF_U.HI, this->y.HALF_U.HI);
}

Entity* sub_080040EC(Entity* this, u8* table) {
    CheckSubTileCollision(this, (const u16*)table, this->x.HALF_U.HI, this->y.HALF_U.HI);
    return this;
}

void SnapToTile(Entity* this) {
    this->x.WORD = (this->x.WORD & ~0xFFFFF) + 0x80000;
    this->y.WORD = (this->y.WORD & ~0xFFFFF) + 0x80000;
}

void sub_0800417E(Entity* this, u32 collisions) {
    u32 dir = this->direction;
    if (collisions & 0xEE00)
        dir = 0x20 - dir;
    if (collisions & 0xEE)
        dir = 0x10 - dir;
    this->direction = dir & 0x1F;
}

static u32 InRectRadius(Entity* a, Entity* b, u32 radiusX, u32 radiusY) {
    if (radiusX != 0 && radiusX * 2 < (u32)(a->x.HALF_U.HI - b->x.HALF_U.HI + radiusX))
        return 0;
    if (radiusY != 0 && radiusY * 2 < (u32)(a->y.HALF_U.HI - b->y.HALF_U.HI + radiusY))
        return 0;
    return 1;
}

u32 sub_0800419C(Entity* a, Entity* b, u32 radiusX, u32 radiusY) {
    return InRectRadius(a, b, radiusX, radiusY);
}

bool32 EntityInRectRadius(Entity* a, Entity* b, u32 radiusX, u32 radiusY) {
    if (!(a->collisionLayer & b->collisionLayer & 3))
        return 0;
    return InRectRadius(a, b, radiusX, radiusY);
}

u32 CalcDistance(s32 x, s32 y) {
    return Sqrt((x * x + y * y) << 8);
}

s32 sub_080041E8(s32 x1, s32 y1, s32 x2, s32 y2) {
    return CalcDistance(x1 - x2, y1 - y2);
}

u32 sub_080041DC(Entity* this, u32 x, u32 y) {
    return sub_080041E8(this->x.HALF.HI, this->y.HALF.HI, x, y);
}

/* Returns the tile in front (according to animationState) of tilePos; pos is a byte offset into mapData. */
static u32 GetTileInFront(Entity* this, u32 animState, u32 pos, u32* tileOut) {
    u32 tile;
    if (animState & 3)
        pos += (animState & 4) ? -2 : 2;
    if ((animState & 3) != 2)
        pos += ((animState + 1) & 4) ? 0x80 : -0x80;
    pos &= 0x1FFF;
    tile = *(u16*)((u8*)LAYER_MAPDATA(this->collisionLayer & 3) + pos);
    if (!(tile & 0x4000))
        tile = *(u16*)((u8*)LAYER_TILETYPES(this->collisionLayer & 3) + ((tile << 17) >> 16));
    *tileOut = tile;
    return pos;
}

u32 sub_08004202(Entity* this, u8* out, u32 pos) {
    u32 tile;
    pos = GetTileInFront(this, this->animationState, pos, &tile);
    *(u32*)out = tile;
    return pos;
}

/* ---- animation ---- */

static void FrameZero(Entity* this) {
    u8* frame;
    this->lastFrameIndex = this->frameIndex;
    frame = this->animPtr;
    this->frameIndex = frame[0];
    this->frameDuration = frame[1];
    this->frameSpriteSettings = frame[2];
    this->frame = frame[3];
    frame += 4;
    if (frame[-1] & 0x80)
        frame -= frame[0] * 4; /* loop back */
    this->animPtr = frame;
}

/* An invisible, endlessly looping animation. Used where the GBA would read
 * the animation table through a NULL pointer (which reads BIOS memory there). */
static const u8 sNullAnimation[] = { 0xFF, 0xFF, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00 };

void InitializeAnimation(Entity* this, u32 animIndex) {
    void** animations = gSpritePtrs[(u16)this->spriteIndex].animations;
    this->animIndex = animIndex;
    if (animations == NULL || animations[animIndex] == NULL)
        this->animPtr = (void*)sNullAnimation;
    else
        this->animPtr = animations[animIndex];
    FrameZero(this);
}

void UpdateAnimationVariableFrames(Entity* this, u32 amount) {
    s32 duration = this->frameDuration - amount;
    u8* frame;
    if (duration == 0) {
        FrameZero(this);
        return;
    }
    if (duration > 0) {
        this->frameDuration = duration;
        return;
    }
    frame = this->animPtr;
    for (;;) {
        u8 flags;
        duration += frame[1];
        if (duration > 0)
            break;
        flags = frame[3];
        frame += 4;
        if (flags & 0x80)
            frame -= frame[0] * 4;
    }
    this->animPtr = frame;
    FrameZero(this);
    this->frameDuration = duration;
}

void GetNextFrame(Entity* this) {
    UpdateAnimationVariableFrames(this, 1);
}

void sub_080042D0(Entity* this, u32 frameIndex, u16 spriteIndex) {
    const SpritePtr* sprite;
    const u8* frame;
    u8* slot;
    u8 oldCount;
    u32 newPtr, oldPtr;
    if (frameIndex == 0xFF)
        return;
    sprite = &gSpritePtrs[spriteIndex];
    if (sprite->frames == NULL)
        return;
    frame = (const u8*)sprite->frames + frameIndex * 4;
    if (frame[0] == 0)
        return;
    slot = (u8*)&gGFXSlots + 4 + this->spriteAnimation[0] * 12;
    if ((slot[0] & 0xF) < 5)
        return;
    oldCount = slot[6];
    slot[6] = frame[0];
    newPtr = (u32)(uintptr_t)sprite->ptr + (*(const u16*)(frame + 2) << 5);
    oldPtr = *(u32*)(slot + 8);
    *(u32*)(slot + 8) = newPtr;
    if (((oldPtr - newPtr) | (u8)(oldCount - frame[0])) != 0)
        slot[0] = (slot[0] & 0x0F) + 0x30;
}

static void UpdateGfxIfFrameChanged(Entity* this) {
    u8 frameIndex = this->frameIndex;
    u8 last = this->lastFrameIndex;
    this->lastFrameIndex = frameIndex;
    if (frameIndex != last)
        sub_080042D0(this, frameIndex, this->spriteIndex);
}

void InitAnimationForceUpdate(Entity* this, u32 animIndex) {
    InitializeAnimation(this, animIndex);
    this->lastFrameIndex = 0xFF;
    UpdateGfxIfFrameChanged(this);
}

void sub_080042BA(Entity* this, u32 amount) {
    UpdateAnimationVariableFrames(this, amount);
    UpdateGfxIfFrameChanged(this);
}

void UpdateAnimationSingleFrame(Entity* this) {
    sub_080042BA(this, 1);
}

/* ---- splash / fall effects ---- */

static void CreateFxAndDelete(Entity* this, u32 fx) {
    Entity* obj = CreateObject(0xF /* SPECIAL_FX */, fx, 0);
    if (obj != NULL) {
        obj->x.HALF.HI = this->x.HALF.HI;
        obj->y.HALF.HI = this->y.HALF.HI;
        obj->z.HALF.HI = this->z.HALF.HI;
        if (this->kind == ENEMY)
            obj->type2 = 1;
    }
    DeleteEntity(this);
}

void CreateDrownFx(Entity* this) {
    CreateFxAndDelete(this, 0xB); /* FX_WATER_SPLASH */
}

void CreateLavaDrownFx(Entity* this) {
    CreateFxAndDelete(this, 0xC); /* FX_LAVA_SPLASH */
}

void CreateSwampDrownFx(Entity* this) {
    CreateFxAndDelete(this, 0x52); /* FX_GREEN_SPLASH */
}

void CreatePitFallFx(Entity* this) {
    CreateFxAndDelete(this, 0); /* FX_FALL_DOWN */
}
