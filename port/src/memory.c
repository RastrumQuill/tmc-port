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
#include <string.h>

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

#ifdef _WIN32
#define RELAUNCH_ENV "TMC_GBA_MEMORY_RESERVED"
/* set in a process whose GBA memory was reserved by its parent (see Relaunch) */
static bool sReservedByParent;
#endif

static size_t RoundSize(size_t size) {
    return (size + 0xFFFF) & ~(size_t)0xFFFF;
}

static void* MapAt(uintptr_t addr, size_t size) {
    size = RoundSize(size);
#ifdef _WIN32
    if (sReservedByParent)
        return VirtualAlloc((void*)addr, size, MEM_COMMIT, PAGE_READWRITE);
    return VirtualAlloc((void*)addr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* p = mmap((void*)addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

#ifdef _WIN32
/*
 * Something (a DLL, a heap, address space randomization) already uses one of the
 * fixed GBA addresses in this process. Start the game again, suspended, reserve the
 * GBA memory in the new process before any of its own code runs, and let it run.
 */
static int Relaunch(void) {
    int attempt;
    fprintf(stderr, "tmc: the GBA memory addresses are in use, restarting with them reserved\n");
    SetEnvironmentVariableA(RELAUNCH_ENV, "1");
    for (attempt = 0; attempt < 8; attempt++) {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        DWORD code = 1;
        size_t i;
        bool ok = true;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        if (!CreateProcessW(NULL, GetCommandLineW(), NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi))
            return 1;
        for (i = 0; i < sizeof(sRegions) / sizeof(sRegions[0]) && ok; i++) {
            if (VirtualAllocEx(pi.hProcess, (void*)sRegions[i].addr, RoundSize(sRegions[i].size), MEM_RESERVE,
                               PAGE_READWRITE) != (void*)sRegions[i].addr)
                ok = false;
        }
        if (!ok) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            continue;
        }
        ResumeThread(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return (int)code;
    }
    return 1;
}
#endif

void Port_MapMemory(void) {
    size_t i;
    if (sMapped)
        return;
#ifdef _WIN32
    sReservedByParent = getenv(RELAUNCH_ENV) != NULL;
    /* test hook: occupy EWRAM's address to exercise the relaunch */
    if (!sReservedByParent && getenv("TMC_TEST_OCCUPY_EWRAM"))
        VirtualAlloc((void*)0x02000000, 0x10000, MEM_RESERVE, PAGE_READWRITE);
#endif
    for (i = 0; i < sizeof(sRegions) / sizeof(sRegions[0]); i++) {
        void* p = MapAt(sRegions[i].addr, sRegions[i].size);
        if (p != (void*)sRegions[i].addr) {
#ifdef _WIN32
            if (!sReservedByParent)
                exit(Relaunch());
#endif
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

/* ---- reads and DMA through out of range pointers ---- */

bool Port_IsReadable(const void* p, size_t size) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t a = (uintptr_t)p, end = a + size;
    while (a < end) {
        if (VirtualQuery((const void*)a, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
#else
    long page = 4096;
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)(page - 1), end = (uintptr_t)p + size;
    unsigned char v;
    for (; a < end; a += page) {
        if (mincore((void*)a, page, &v) != 0)
            return false;
    }
    return true;
#endif
}

/*
 * The GBA decodes only 28 address bits and mirrors each memory region over its
 * whole 16 MB slot. Pointers the game computes out of range (and that the PC
 * does not map) are folded the same way.
 */
uint32_t Port_GbaAddress(uint32_t a) {
    uint32_t region;
    if (a >= 0x10000000u) {
        if (Port_IsReadable((const void*)(uintptr_t)a, 1))
            return a;
        a &= 0x0FFFFFFFu;
    }
    region = a >> 24;
    switch (region) {
        case 2:
            return PORT_EWRAM_ADDR + (a & (PORT_EWRAM_SIZE - 1));
        case 3:
            return PORT_IWRAM_ADDR + (a & (PORT_IWRAM_SIZE - 1));
        case 5:
            return PORT_PLTT_ADDR + (a & (PORT_PLTT_SIZE - 1));
        case 6:
            /* the port's extra sprite VRAM lies after the GBA's 96 KB */
            if ((a & 0xFFFFFF) < PORT_VRAM_SIZE)
                return a;
            a &= 0x1FFFF;
            if (a >= 0x18000)
                a -= 0x8000;
            return PORT_VRAM_ADDR + a;
        case 7:
            return PORT_OAM_ADDR + (a & (PORT_OAM_SIZE - 1));
        default:
            return a;
    }
}
