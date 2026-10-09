/**
 * @file memory.c
 * @brief Maps the GBA memory regions at their native addresses.
 *
 * The decompiled code addresses RAM, VRAM, OAM, palette RAM and the I/O
 * registers through fixed GBA addresses (both in C and in the data tables), and
 * all RAM variables are placed at fixed addresses by the linker. Mapping the
 * regions at the very same addresses lets that code run unmodified.
 * This requires a 32-bit process whose image does not overlap 0x02000000-0x07FFFFFF.
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#endif

typedef struct {
    uintptr_t addr;
    size_t size;
    const char* name;
} Region;

static const Region sRegions[] = {
    { PORT_EWRAM_ADDR, PORT_EWRAM_SIZE, "EWRAM" }, { PORT_IWRAM_ADDR, PORT_IWRAM_SIZE, "IWRAM" },
    { PORT_IO_ADDR, PORT_IO_SIZE, "IO" },          { PORT_PLTT_ADDR, PORT_PLTT_SIZE, "PLTT" },
    { PORT_VRAM_ADDR, PORT_VRAM_SIZE, "VRAM" },    { PORT_OAM_ADDR, PORT_OAM_SIZE, "OAM" },
};

static bool sMapped;

static void* MapAt(uintptr_t addr, size_t size) {
    /* round up to whole pages */
    size = (size + 0xFFFF) & ~(size_t)0xFFFF;
#ifdef _WIN32
    return VirtualAlloc((void*)addr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* p = mmap((void*)addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

void Port_MapMemory(void) {
    size_t i;
    if (sMapped)
        return;
    for (i = 0; i < sizeof(sRegions) / sizeof(sRegions[0]); i++) {
        void* p = MapAt(sRegions[i].addr, sRegions[i].size);
        if (p != (void*)sRegions[i].addr) {
            fprintf(stderr, "tmc: could not map %s at 0x%08lX (got %p).\n", sRegions[i].name,
                    (unsigned long)sRegions[i].addr, p);
            fprintf(stderr, "tmc: the port must be built as a 32-bit, non-PIE executable.\n");
            exit(1);
        }
    }
    sMapped = true;
}

/* Map before any other constructor (or main) can touch game memory. */
__attribute__((constructor(101))) static void MapMemoryEarly(void) {
    Port_MapMemory();
}
