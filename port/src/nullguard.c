/**
 * @file nullguard.c
 * @brief Emulates GBA reads/writes of the low (BIOS) memory area.
 *
 * The original game occasionally dereferences NULL pointers (for example an
 * entity without a hitbox, or the "no interaction" object). On the GBA those
 * accesses hit the BIOS region: reads return the last BIOS opcode fetched
 * ("open bus", 0xE3A02004 after returning from a BIOS call) and writes are
 * ignored. On a PC they crash.
 *
 * This file installs a fault handler: when an access to an address below
 * 0x10000 faults, the register that holds the (NULL) base pointer is
 * temporarily redirected to a buffer filled with the open bus pattern, the
 * faulting instruction is single stepped, and the register is restored
 * afterwards. Real crashes (any other address) are not affected.
 */
#define _GNU_SOURCE
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOW_LIMIT 0x10000u
#define OPEN_BUS 0xE3A02004u

/* fake low memory, padded so that negative-free offsets up to LOW_LIMIT + 64K stay inside */
static uint32_t sFakeLow[(LOW_LIMIT * 2) / 4];
static int sGuardCount;

static void FillFakeLow(void) {
    size_t i;
    for (i = 0; i < sizeof(sFakeLow) / 4; i++)
        sFakeLow[i] = OPEN_BUS;
}

/* register numbering of x86 ModRM: eax ecx edx ebx esp ebp esi edi */
typedef struct {
    int reg;        /* register redirected, -1 if none */
    uint32_t orig;  /* its original value */
    uint32_t moved; /* value written into it */
    bool pending;
} GuardState;

static GuardState sGuard;

/* Find the base/index register used by the memory operand of the instruction at eip. */
static int FindAddressRegister(const uint8_t* p, const uint32_t* regs, uint32_t faultAddr) {
    uint8_t op, modrm, mod, rm;
    int base = -1, index = -1;
    /* prefixes */
    for (;;) {
        op = *p;
        if (op == 0x66 || op == 0xF2 || op == 0xF3 || op == 0xF0 || op == 0x2E || op == 0x3E || op == 0x26 ||
            op == 0x36 || op == 0x64 || op == 0x65) {
            p++;
            continue;
        }
        if (op == 0x67)
            return -1; /* 16 bit addressing: not produced by the compiler */
        break;
    }
    op = *p++;
    if (op == 0x0F) {
        op = *p++;
        if (op == 0x38 || op == 0x3A)
            p++;
    } else if ((op >= 0xA0 && op <= 0xA7) || op == 0xAC || op == 0xAD || op == 0xAA || op == 0xAB) {
        /* moffs or string instructions: esi/edi based */
        if (op == 0xA4 || op == 0xA5 || op == 0xAC || op == 0xAD || op == 0xA6 || op == 0xA7)
            return (regs[6] < LOW_LIMIT) ? 6 : 7;
        if (op == 0xAA || op == 0xAB)
            return 7;
        return -1;
    }
    modrm = *p++;
    mod = modrm >> 6;
    rm = modrm & 7;
    if (mod == 3)
        return -1;
    if (rm == 4) {
        uint8_t sib = *p++;
        int sibBase = sib & 7;
        int sibIndex = (sib >> 3) & 7;
        if (!(sibBase == 5 && mod == 0))
            base = sibBase;
        if (sibIndex != 4)
            index = sibIndex;
    } else if (!(rm == 5 && mod == 0)) {
        base = rm;
    }
    if (base >= 0 && regs[base] < LOW_LIMIT)
        return base;
    if (index >= 0 && regs[index] < LOW_LIMIT)
        return index;
    (void)faultAddr;
    return -1;
}

static void ReportCrash(uint32_t addr, uint32_t eip) {
    fprintf(stderr, "tmc: invalid memory access at 0x%08X (eip 0x%08X)\n", addr, eip);
}

#if defined(__linux__) && defined(__i386__)
#include <signal.h>
#include <ucontext.h>

static const int sGregIndex[8] = { REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_ESP, REG_EBP, REG_ESI, REG_EDI };

static void OnSegv(int sig, siginfo_t* info, void* ctx) {
    ucontext_t* uc = ctx;
    greg_t* g = uc->uc_mcontext.gregs;
    uint32_t addr = (uint32_t)(uintptr_t)info->si_addr;
    uint32_t regs[8];
    int i, r;
    (void)sig;
    for (i = 0; i < 8; i++)
        regs[i] = (uint32_t)g[sGregIndex[i]];
    if (addr < LOW_LIMIT && !sGuard.pending) {
        r = FindAddressRegister((const uint8_t*)(uintptr_t)g[REG_EIP], regs, addr);
        if (r >= 0 && r != 4) {
            sGuard.reg = r;
            sGuard.orig = regs[r];
            sGuard.moved = regs[r] + (uint32_t)(uintptr_t)sFakeLow;
            sGuard.pending = true;
            g[sGregIndex[r]] = (greg_t)sGuard.moved;
            g[REG_EFL] |= 0x100; /* single step */
            if (sGuardCount++ < 8 || getenv("TMC_NULL_LOG"))
                Port_Log("emulated BIOS area access at 0x%04X (eip 0x%08X)", addr, (uint32_t)g[REG_EIP]);
            return;
        }
    }
    ReportCrash(addr, (uint32_t)g[REG_EIP]);
    signal(SIGSEGV, SIG_DFL);
}

static void OnTrap(int sig, siginfo_t* info, void* ctx) {
    ucontext_t* uc = ctx;
    greg_t* g = uc->uc_mcontext.gregs;
    (void)sig;
    (void)info;
    if (!sGuard.pending)
        return;
    if ((uint32_t)g[sGregIndex[sGuard.reg]] == sGuard.moved)
        g[sGregIndex[sGuard.reg]] = (greg_t)sGuard.orig;
    g[REG_EFL] &= ~0x100;
    sGuard.pending = false;
    FillFakeLow(); /* undo writes */
}

void Port_InstallNullGuard(void) {
    struct sigaction sa;
    FillFakeLow();
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = OnSegv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sa.sa_sigaction = OnTrap;
    sigaction(SIGTRAP, &sa, NULL);
}

#elif defined(_WIN32) && defined(_M_IX86) || (defined(_WIN32) && defined(__i386__))
#include <windows.h>

static DWORD* RegPtr(CONTEXT* c, int r) {
    switch (r) {
        case 0:
            return &c->Eax;
        case 1:
            return &c->Ecx;
        case 2:
            return &c->Edx;
        case 3:
            return &c->Ebx;
        case 4:
            return &c->Esp;
        case 5:
            return &c->Ebp;
        case 6:
            return &c->Esi;
        default:
            return &c->Edi;
    }
}

static LONG CALLBACK OnException(EXCEPTION_POINTERS* ep) {
    CONTEXT* c = ep->ContextRecord;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        uint32_t addr = (uint32_t)ep->ExceptionRecord->ExceptionInformation[1];
        uint32_t regs[8];
        int i, r;
        for (i = 0; i < 8; i++)
            regs[i] = *RegPtr(c, i);
        if (addr < LOW_LIMIT && !sGuard.pending) {
            r = FindAddressRegister((const uint8_t*)(uintptr_t)c->Eip, regs, addr);
            if (r >= 0 && r != 4) {
                sGuard.reg = r;
                sGuard.orig = regs[r];
                sGuard.moved = regs[r] + (uint32_t)(uintptr_t)sFakeLow;
                sGuard.pending = true;
                *RegPtr(c, r) = sGuard.moved;
                c->EFlags |= 0x100;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        ReportCrash(addr, c->Eip);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == EXCEPTION_SINGLE_STEP && sGuard.pending) {
        if (*RegPtr(c, sGuard.reg) == sGuard.moved)
            *RegPtr(c, sGuard.reg) = sGuard.orig;
        sGuard.pending = false;
        FillFakeLow();
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void Port_InstallNullGuard(void) {
    FillFakeLow();
    AddVectoredExceptionHandler(1, OnException);
}

#else
void Port_InstallNullGuard(void) {
    FillFakeLow();
}
#endif
