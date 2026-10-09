/**
 * @file asm_common.h
 * @brief Helpers for the C translations of the hand written GBA assembly.
 *
 * The translations in this directory follow the original assembly closely
 * (port/src/asm/<file>.c mirrors asm/src/<file>.s). Raw structure offsets are
 * used where the original code uses them and no named field exists.
 */
#ifndef ASM_COMMON_H
#define ASM_COMMON_H

#include "global.h"
#include "entity.h"
#include "room.h"
#include "player.h"
#include "map.h"

#define U8AT(p, off) (*(u8*)((u8*)(p) + (off)))
#define S8AT(p, off) (*(s8*)((u8*)(p) + (off)))
#define U16AT(p, off) (*(u16*)((u8*)(p) + (off)))
#define S16AT(p, off) (*(s16*)((u8*)(p) + (off)))
#define U32AT(p, off) (*(u32*)((u8*)(p) + (off)))
#define S32AT(p, off) (*(s32*)((u8*)(p) + (off)))
#define PTRAT(p, off) (*(void**)((u8*)(p) + (off)))

static inline u32 RotateRight(u32 v, u32 n) {
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

#endif
