/**
 * @file bios.c
 * @brief C implementation of the GBA BIOS functions used by the game.
 */
#include "port.h"

#include <math.h>
#include <string.h>

#include "global.h"

void SoftReset(u32 resetFlags) {
    (void)resetFlags;
    Port_SoftReset();
}

void RegisterRamReset(u32 resetFlags) {
    if (resetFlags & RESET_EWRAM)
        memset((void*)PORT_EWRAM_ADDR, 0, PORT_EWRAM_SIZE);
    if (resetFlags & RESET_IWRAM)
        /* the BIOS keeps the top 0x200 bytes (stacks / interrupt vector) */
        memset((void*)PORT_IWRAM_ADDR, 0, PORT_IWRAM_SIZE - 0x200);
    if (resetFlags & RESET_PALETTE)
        memset((void*)PORT_PLTT_ADDR, 0, PORT_PLTT_SIZE);
    if (resetFlags & RESET_VRAM)
        memset((void*)PORT_VRAM_ADDR, 0, PORT_VRAM_SIZE);
    if (resetFlags & RESET_OAM)
        memset((void*)PORT_OAM_ADDR, 0, PORT_OAM_SIZE);
    if (resetFlags & RESET_REGS) {
        u16 keys = PORT_IO16(0x130);
        memset((void*)PORT_IO_ADDR, 0, 0x200);
        PORT_IO16(0x130) = keys;
        PORT_IO16(0x000) = 0x0080; /* DISPCNT: forced blank */
        PORT_IO16(0x020) = 0x100;  /* BG2PA */
        PORT_IO16(0x026) = 0x100;  /* BG2PD */
        PORT_IO16(0x030) = 0x100;  /* BG3PA */
        PORT_IO16(0x036) = 0x100;  /* BG3PD */
    }
}

void VBlankIntrWait(void) {
    Port_VBlankIntrWait();
}

void PortSystemCall(u32 num) {
    switch (num) {
        case 2: /* Halt */
        case 3: /* Stop (sleep mode): wait until the wake-up key combo is released/pressed */
            Port_VBlankIntrWait();
            break;
        default:
            Port_Log("unhandled BIOS call 0x%X", num);
            break;
    }
}

void SoundBiasReset(void) {
}

void SoundBiasSet(void) {
}

u16 Sqrt(u32 num) {
    u32 res = 0;
    u32 bit = 1u << 30;
    while (bit > num)
        bit >>= 2;
    while (bit != 0) {
        if (num >= res + bit) {
            num -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (u16)res;
}

/* Polynomial used by the BIOS ArcTan function. */
static s32 BiosArcTan(s32 i) {
    s32 a = -((i * i) >> 14);
    s32 b = ((0xA9 * a) >> 14) + 0x390;
    b = ((b * a) >> 14) + 0x91C;
    b = ((b * a) >> 14) + 0xFB6;
    b = ((b * a) >> 14) + 0x16AA;
    b = ((b * a) >> 14) + 0x2081;
    b = ((b * a) >> 14) + 0x3651;
    b = ((b * a) >> 14) + 0xA2F9;
    return (i * b) >> 16;
}

u16 ArcTan2(s16 x, s16 y) {
    if (y == 0)
        return x >= 0 ? 0 : 0x8000;
    if (x == 0)
        return y >= 0 ? 0x4000 : 0xC000;
    if (y >= 0) {
        if (x >= 0) {
            if (x >= y)
                return (u16)BiosArcTan((y << 14) / x);
        } else if (-x >= y) {
            return (u16)(BiosArcTan((y << 14) / x) + 0x8000);
        }
        return (u16)(0x4000 - BiosArcTan((x << 14) / y));
    } else {
        if (x <= 0) {
            if (-x > -y)
                return (u16)(BiosArcTan((y << 14) / x) + 0x8000);
        } else if (x >= -y) {
            return (u16)(BiosArcTan((y << 14) / x) + 0x10000);
        }
        return (u16)(0xC000 - BiosArcTan((x << 14) / y));
    }
}

void CpuSet(const void* src, void* dest, u32 control) {
    u32 count = control & 0x1FFFFF;
    bool fixed = (control & CPU_SET_SRC_FIXED) != 0;
    u32 i;
    if (control & CPU_SET_32BIT) {
        const u32* s = (const u32*)((uintptr_t)src & ~3u);
        u32* d = (u32*)((uintptr_t)dest & ~3u);
        if (fixed) {
            u32 v = *s;
            for (i = 0; i < count; i++)
                d[i] = v;
        } else {
            memmove(d, s, count * 4);
        }
    } else {
        const u16* s = (const u16*)((uintptr_t)src & ~1u);
        u16* d = (u16*)((uintptr_t)dest & ~1u);
        if (fixed) {
            u16 v = *s;
            for (i = 0; i < count; i++)
                d[i] = v;
        } else {
            memmove(d, s, count * 2);
        }
    }
}

void CpuFastSet(const void* src, void* dest, u32 control) {
    u32 count = ((control & 0x1FFFFF) + 7) & ~7u;
    const u32* s = (const u32*)((uintptr_t)src & ~3u);
    u32* d = (u32*)((uintptr_t)dest & ~3u);
    u32 i;
    if (control & CPU_FAST_SET_SRC_FIXED) {
        u32 v = *s;
        for (i = 0; i < count; i++)
            d[i] = v;
    } else {
        memmove(d, s, count * 4);
    }
}

static s16 BiosSin(u8 angle) {
    return (s16)lround(sin(angle * (2.0 * M_PI / 256.0)) * 16384.0);
}

static s16 BiosCos(u8 angle) {
    return BiosSin((u8)(angle + 64));
}

void BgAffineSet(struct BgAffineSrcData* src, struct BgAffineDstData* dest, s32 count) {
    for (; count > 0; count--, src++, dest++) {
        u8 angle = src->alpha >> 8;
        s32 s = BiosSin(angle);
        s32 c = BiosCos(angle);
        s32 pa = (src->sx * c) >> 14;
        s32 pb = -((src->sx * s) >> 14);
        s32 pc = (src->sy * s) >> 14;
        s32 pd = (src->sy * c) >> 14;
        dest->pa = (s16)pa;
        dest->pb = (s16)pb;
        dest->pc = (s16)pc;
        dest->pd = (s16)pd;
        dest->dx = src->texX - (pa * src->scrX + pb * src->scrY);
        dest->dy = src->texY - (pc * src->scrX + pd * src->scrY);
    }
}

void ObjAffineSet(struct ObjAffineSrcData* src, void* dest, s32 count, s32 offset) {
    /* the BIOS reads 8 byte source entries (scale x, scale y, angle, padding);
     * the C struct has no padding, so step through the bytes */
    const u8* s8 = (const u8*)src;
    u8* d = dest;
    for (; count > 0; count--, s8 += 8) {
        src = (struct ObjAffineSrcData*)s8;
        u8 angle = src->rotation >> 8;
        s32 s = BiosSin(angle);
        s32 c = BiosCos(angle);
        *(s16*)(d + 0 * offset) = (s16)((src->xScale * c) >> 14);
        *(s16*)(d + 1 * offset) = (s16)(-((src->xScale * s) >> 14));
        *(s16*)(d + 2 * offset) = (s16)((src->yScale * s) >> 14);
        *(s16*)(d + 3 * offset) = (s16)((src->yScale * c) >> 14);
        d += 4 * offset;
    }
}

/* LZ77 as used by the BIOS: header 0x10 | size << 8 */
static void LZ77UnComp(const void* src, void* dest, bool vram) {
    const u8* s = src;
    u8* d = dest;
    u32 size = (s[1] | (s[2] << 8) | (s[3] << 16));
    u32 written = 0;
    s += 4;
    (void)vram; /* VRAM needs 16-bit writes on hardware; irrelevant here */
    while (written < size) {
        u8 flags = *s++;
        int i;
        for (i = 0; i < 8 && written < size; i++, flags <<= 1) {
            if (flags & 0x80) {
                u32 len = (s[0] >> 4) + 3;
                u32 disp = (((s[0] & 0xF) << 8) | s[1]) + 1;
                s += 2;
                while (len-- && written < size) {
                    d[written] = d[written - disp];
                    written++;
                }
            } else {
                d[written++] = *s++;
            }
        }
    }
}

void LZ77UnCompWram(const void* src, void* dest) {
    LZ77UnComp(src, dest, false);
}

void LZ77UnCompVram(const void* src, void* dest) {
    LZ77UnComp(src, dest, true);
}

static void RLUnComp(const void* src, void* dest) {
    const u8* s = src;
    u8* d = dest;
    u32 size = (s[1] | (s[2] << 8) | (s[3] << 16));
    u32 written = 0;
    s += 4;
    while (written < size) {
        u8 flag = *s++;
        if (flag & 0x80) {
            u32 len = (flag & 0x7F) + 3;
            u8 v = *s++;
            while (len-- && written < size)
                d[written++] = v;
        } else {
            u32 len = (flag & 0x7F) + 1;
            while (len-- && written < size)
                d[written++] = *s++;
        }
    }
}

void RLUnCompWram(const void* src, void* dest) {
    RLUnComp(src, dest);
}

void RLUnCompVram(const void* src, void* dest) {
    RLUnComp(src, dest);
}

int MultiBoot(struct MultiBootParam* mp) {
    (void)mp;
    return 1;
}

s32 Div(s32 num, s32 denom) {
    if (denom == 0)
        return num < 0 ? -1 : 1;
    return num / denom;
}

s32 Mod(s32 num, s32 denom) {
    if (denom == 0)
        return num;
    return num % denom;
}

s64 PortDivAndMod(s32 num, s32 denom) {
    union SplitDWord r;
    r.HALF.LO = Div(num, denom);
    r.HALF.HI = Mod(num, denom);
    return r.DWORD;
}
