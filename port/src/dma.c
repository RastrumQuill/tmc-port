/**
 * @file dma.c
 * @brief Software DMA controller.
 *
 * Immediate transfers are performed on the spot. HBlank and VBlank timed
 * transfers are remembered and replayed by the PPU (Port_DmaOnHBlank) and the
 * frame loop (Port_DmaOnVBlank). Sound FIFO transfers are ignored, the audio
 * code reads the mixer buffer directly.
 */
#include "port.h"

#include <string.h>

#include "global.h"

#define DMA_REG_BASE 0xB0
#define DMA_CHANNEL_SIZE 12

typedef struct {
    u32 src;
    u32 dest;
    u32 control;
    u32 curSrc;
    u32 curDest;
    bool active;
} DmaChannel;

static DmaChannel sDma[4];

static void StoreRegs(u32 n, const DmaChannel* ch) {
    PORT_IO32(DMA_REG_BASE + n * DMA_CHANNEL_SIZE + 0) = ch->src;
    PORT_IO32(DMA_REG_BASE + n * DMA_CHANNEL_SIZE + 4) = ch->dest;
    PORT_IO32(DMA_REG_BASE + n * DMA_CHANNEL_SIZE + 8) = ch->active ? ch->control : (ch->control & 0x7FFFFFFF);
}

static s32 Step(u32 mode, u32 unit) {
    switch (mode) {
        case 0: /* increment */
        case 3: /* increment + reload (dest only) */
            return (s32)unit;
        case 1: /* decrement */
            return -(s32)unit;
        default: /* fixed */
            return 0;
    }
}

static void Transfer(u32 n, DmaChannel* ch) {
    u32 control = ch->control;
    u32 count = control & 0xFFFF;
    u32 unit = (control & (DMA_32BIT << 16)) ? 4 : 2;
    s32 srcStep = Step((control >> 23) & 3, unit);
    s32 destStep = Step((control >> 21) & 3, unit);
    u32 s = ch->curSrc;
    u32 d = ch->curDest;
    u32 i;

    if (count == 0)
        count = (n == 3) ? 0x10000 : 0x4000;
    if (d == PORT_OAM_ADDR)
        Port_OnOamCopy((const void*)(uintptr_t)s, (void*)(uintptr_t)d, count * unit);
    if (unit == 4) {
        s &= ~3u;
        d &= ~3u;
        if (srcStep == 4 && destStep == 4) {
            memmove((void*)(uintptr_t)d, (const void*)(uintptr_t)s, count * 4);
            s += count * 4;
            d += count * 4;
        } else {
            for (i = 0; i < count; i++, s += srcStep, d += destStep)
                *(u32*)(uintptr_t)d = *(const u32*)(uintptr_t)s;
        }
    } else {
        s &= ~1u;
        d &= ~1u;
        if (srcStep == 2 && destStep == 2) {
            memmove((void*)(uintptr_t)d, (const void*)(uintptr_t)s, count * 2);
            s += count * 2;
            d += count * 2;
        } else {
            for (i = 0; i < count; i++, s += srcStep, d += destStep)
                *(u16*)(uintptr_t)d = *(const u16*)(uintptr_t)s;
        }
    }
    ch->curSrc = s;
    ch->curDest = (((control >> 21) & 3) == 3) ? ch->dest : d;
    if (!(control & (DMA_REPEAT << 16)) || ((control >> 28) & 3) == 0) {
        ch->active = false;
    }
    StoreRegs(n, ch);
}

void PortDmaSet(u32 n, const void* src, void* dest, u32 control) {
    DmaChannel* ch = &sDma[n & 3];
    n &= 3;
    ch->src = (u32)(uintptr_t)src;
    ch->dest = (u32)(uintptr_t)dest;
    ch->control = control;
    ch->curSrc = ch->src;
    ch->curDest = ch->dest;
    ch->active = (control & 0x80000000u) != 0;
    StoreRegs(n, ch);
    if (!ch->active)
        return;
    switch ((control >> 28) & 3) {
        case 0: /* start now */
            Transfer(n, ch);
            break;
        case 1: /* vblank */
        case 2: /* hblank */
            break;
        case 3: /* special: sound FIFO / video capture, handled elsewhere */
            ch->active = false;
            break;
    }
}

void PortDmaStop(u32 n) {
    n &= 3;
    sDma[n].active = false;
    sDma[n].control &= ~((DMA_ENABLE | DMA_REPEAT | DMA_START_MASK | DMA_DREQ_ON) << 16);
    StoreRegs(n, &sDma[n]);
}

void Port_DmaOnHBlank(int line) {
    u32 n;
    (void)line;
    for (n = 0; n < 4; n++) {
        if (sDma[n].active && ((sDma[n].control >> 28) & 3) == 2)
            Transfer(n, &sDma[n]);
    }
}

void Port_DmaOnVBlank(void) {
    u32 n;
    for (n = 0; n < 4; n++) {
        if (sDma[n].active && ((sDma[n].control >> 28) & 3) == 1)
            Transfer(n, &sDma[n]);
    }
}

/* savestates (port/src/debug.c) */
void* Dma_StateData(size_t* size) {
    *size = sizeof(sDma);
    return sDma;
}

/* true when the active HBlank transfers only write I/O registers (the renderer may then snapshot
 * the registers per line and render the lines in any order) */
bool Port_DmaHBlankOnlyIo(void) {
    u32 n;
    for (n = 0; n < 4; n++) {
        const DmaChannel* ch = &sDma[n];
        u32 control = ch->control, count, unit, step;
        u32 lo, hi;
        if (!ch->active || ((control >> 28) & 3) != 2)
            continue;
        count = control & 0xFFFF;
        if (count == 0)
            count = (n == 3) ? 0x10000 : 0x4000;
        unit = (control & (DMA_32BIT << 16)) ? 4 : 2;
        step = (((control >> 21) & 3) == 2) ? 0 : count * unit; /* fixed destination writes one unit */
        lo = ch->curDest;
        hi = ch->curDest + (step ? step : unit);
        if (((control >> 21) & 3) == 1) { /* decrementing */
            lo = ch->curDest - count * unit + unit;
            hi = ch->curDest + unit;
        }
        if (lo < PORT_IO_ADDR || hi > PORT_IO_ADDR + PORT_IO_SIZE)
            return false;
    }
    return true;
}
