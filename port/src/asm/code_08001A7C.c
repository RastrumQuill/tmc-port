/**
 * @file code_08001A7C.c
 * @brief C translation of asm/src/code_08001A7C.s (the data tables live in port/data).
 */
#include "asm_common.h"
#include "port.h"

extern const u8 gUnk_0800232E[];
extern const u8 gUnk_08002342[];
extern const u16 gUnk_0800275C[];
extern const s16 gSineTable[];
extern u32 Random(void);
s16 FixedMul(s16 r0, s16 r1);

/* Each 0xFF byte means that entity member must match (type2, type, id). */
static const u32 sEntityTypeBitmasks[] = { 0x00FFFFFF, 0x00FFFF00, 0x00FF00FF, 0x00FF0000 };

static const u8* FindFuser(Entity* entity) {
    const u8* entry;
    u32 key;
    if (entity->kind == ENEMY) {
        entry = gUnk_0800232E;
    } else if (entity->kind == NPC) {
        entry = gUnk_08002342;
    } else {
        return NULL;
    }
    key = (entity->id << 16) | (entity->type << 8) | entity->type2;
    for (;;) {
        u32 pattern, maskIndex = 0, mask;
        entry += 6;
        if (entry[0] == 0)
            return NULL;
        pattern = entry[0] << 16;
        if (entry[1] == 0xFF)
            maskIndex = 2;
        pattern |= entry[1] << 8;
        if (entry[2] == 0xFF)
            maskIndex += 1;
        pattern |= entry[2];
        mask = sEntityTypeBitmasks[maskIndex];
        if ((key & mask) == (pattern & mask))
            return entry;
    }
}

u32 GetFuserId(Entity* entity) {
    const u8* entry = FindFuser(entity);
    return entry != NULL ? entry[3] : 0;
}

s64 PortGetFuserIdAndTextId(Entity* entity) {
    const u8* entry = FindFuser(entity);
    union SplitDWord r;
    r.HALF_U.LO = entry != NULL ? entry[3] : 0;
    r.HALF_U.HI = entry != NULL ? *(const u16*)(entry + 4) : 0;
    return r.DWORD;
}

static void BlitNibbles(const u8* src, u8* dest, const u8* lut, u32 param, bool skipZero) {
    u8 keepMask = 0xF0;
    int i;
    dest += (param >> 3) * 0x40;
    if (param & 1) {
        keepMask = 0x0F;
        lut += 0x10;
    }
    dest += (param >> 1) & 3;
    for (i = 0; i < 0x10; i++) {
        u8 v = lut[*src];
        if (!skipZero || v != 0)
            *dest = (*dest & keepMask) | v;
        dest += 4;
        src += 8;
    }
}

void sub_080026C4(u8* src, u8* dest, u8* lut, u32 param) {
    BlitNibbles(src, dest, lut, param, false);
}

void sub_080026F2(u8* src, void* dest, u8* lut, u32 param) {
    BlitNibbles(src, dest, lut, param, true);
}

void UnpackTextNibbles(void* src, u8* dest) {
    const u8* s = src;
    int i;
    for (i = 0; i < 0x40; i++) {
        dest[i * 2] = s[i] & 0xF;
        dest[i * 2 + 1] = s[i] >> 4;
    }
}

u32 GetNextFunction(Entity* this) {
    u8 gustJarState = this->gustJarState;
    if (!(gustJarState & 4) && (this->contactFlags & 0x80))
        return 1;
    if (this->knockbackDuration != 0)
        return 2;
    if (this->health == 0)
        return (this->action | this->subAction) ? 3 : 0;
    if (gustJarState & 4)
        return 5;
    if (this->confusedTime != 0)
        return 4;
    return 0;
}

/* collision related, probably leftover from Four Swords */
static u32 CalcCollisionDirectionOLD(u32 dir, u32 collisions) {
    switch (dir >> 3) {
        case 0:
            if (!(collisions & 0xE))
                return dir;
            if (!(collisions & 0xE004))
                return 8;
            if (!(collisions & 0xE02))
                return 0x18;
            return dir;
        case 1:
            if (!(collisions & 0xE000))
                return dir;
            if (!(collisions & 0x200E))
                return 0;
            if (!(collisions & 0x40E0))
                return 0x10;
            return dir;
        case 2:
            if (!(collisions & 0xE0))
                return dir;
            if (!(collisions & 0xE040))
                return 8;
            if (!(collisions & 0xE20))
                return 0x18;
            return dir;
        default:
            if (!(collisions & 0xE00))
                return dir;
            if (!(collisions & 0x20E))
                return 0;
            if (!(collisions & 0x4E0))
                return 0x10;
            return dir;
    }
}

/* almost identical to LinearMoveDirection */
void LinearMoveDirectionOLD(Entity* this, u32 speed, u32 dir) {
    u32 collisions;
    s32 delta;
    if (dir & 0x80)
        return;
    collisions = this->collisions;
    if (!(dir & 7)) {
        u32 newDir = CalcCollisionDirectionOLD(dir, collisions);
        if (newDir != dir) {
            dir = newDir;
            speed = 0x100;
        }
    }
    collisions &= gUnk_0800275C[dir];
    if (!(collisions & 0xEE00)) {
        delta = (u16)gSineTable[dir * 8];
        if (delta != 0)
            delta = FixedMul((s16)delta, (s16)speed) << 8;
        this->x.WORD += delta;
    }
    if (!(collisions & 0xEE)) {
        delta = (u16)gSineTable[dir * 8 + 64];
        if (delta != 0)
            delta = FixedMul((s16)delta, (s16)speed) << 8;
        this->y.WORD -= delta;
    }
}

void sub_080028E0(Entity* this) {
    s8 iframes = this->iframes;
    if (iframes != 0)
        this->iframes = iframes < 0 ? iframes + 1 : iframes - 1;
}

u32 GetRandomByWeight(const u8* weights) {
    s32 r = Random() & 0xFF;
    u32 i = 0;
    do {
        r -= weights[i++];
    } while (r >= 0);
    return i - 1;
}

u32 CheckRectOnScreen(s32 x, s32 y, u32 radiusX, u32 radiusY) {
    s32 left = (u16)gRoomControls.scroll_x - gRoomControls.origin_x - radiusX + gPortScreenLeft;
    s32 top;
    if ((u32)(x - left) >= radiusX * 2 + gPortScreenWidth)
        return 0;
    top = (u16)gRoomControls.scroll_y - gRoomControls.origin_y - radiusY + gPortScreenTop;
    if ((u32)(y - top) >= radiusY * 2 + gPortScreenHeight)
        return 0;
    return 1;
}

u32 CheckPlayerInRegion(u32 centerX, u32 centerY, u32 radiusX, u32 radiusY) {
    s32 left = gPlayerEntity.base.x.HALF_U.HI - gRoomControls.origin_x - radiusX;
    s32 top;
    if (centerX - left >= radiusX * 2)
        return 0;
    top = gPlayerEntity.base.y.HALF_U.HI - gRoomControls.origin_y - radiusY;
    if (centerY - top >= radiusY * 2)
        return 0;
    return 1;
}
