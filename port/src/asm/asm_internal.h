/**
 * @file asm_internal.h
 * @brief Functions shared between the translated assembly files.
 */
#ifndef ASM_INTERNAL_H
#define ASM_INTERNAL_H

#include "global.h"
#include "entity.h"

/** arm_CalcCollisionDirection: direction (0..0x1F) from (x1, y1) towards (x2, y2). */
u32 Asm_CalcCollisionDirection(s32 x1, s32 y1, s32 x2, s32 y2);

/** arm_sub_080B227C: push the second entity out of the first one. */
u32 Asm_CalcCollisionStaticEntity(Entity* target, Entity* origin);

#endif
