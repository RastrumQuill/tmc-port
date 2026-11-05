/**
 * @file viewers.c
 * @brief Live viewers (palette, tiles, OAM, IO registers, memory) and
 *        watch / break on write.
 *
 * F3 cycles the viewer pages, the console command "viewer NAME" picks one.
 * Addresses are GBA addresses (the PC build maps GBA memory at the same
 * addresses) or names of the main game structs, with an optional offset:
 * "gSave+0x40".
 *
 *   watch ADDR [LEN]  logs the bytes once per frame when they change
 *   break ADDR [LEN]  logs every write to them, with the address of the code
 *                     that wrote (look it up with addr2line, see the log)
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

#include "global.h"
#include "area.h"
#include "entity.h"
#include "main.h"
#include "player.h"
#include "room.h"
#include "save.h"
#include "screen.h"

typedef enum { PAGE_OFF, PAGE_PALETTE, PAGE_TILES, PAGE_OAM, PAGE_IO, PAGE_MEM, PAGE_COUNT } ViewerPage;
static const char* const sPageNames[PAGE_COUNT] = { "off", "palette", "tiles", "oam", "io", "mem" };

static ViewerPage sPage;
static int sTilePage, sTilePal;
static uint32_t sMemAddr = 0x02000000u;

/* ---- addresses ---- */

static const struct {
    const char* name;
    const void* addr;
    uint32_t size;
} sNames[] = {
    { "gSave", &gSave, sizeof(gSave) },
    { "gPlayerEntity", &gPlayerEntity, sizeof(gPlayerEntity) },
    { "gPlayerState", &gPlayerState, sizeof(gPlayerState) },
    { "gRoomControls", &gRoomControls, sizeof(gRoomControls) },
    { "gRoomTransition", &gRoomTransition, sizeof(gRoomTransition) },
    { "gMain", &gMain, sizeof(gMain) },
    { "gScreen", &gScreen, sizeof(gScreen) },
    { "gArea", &gArea, sizeof(gArea) },
};

/* "gSave+0x40", "0x02002A00": address and the size of what it names (1 for a number) */
static bool ParseAddr(const char* s, uint32_t* addr, uint32_t* size) {
    size_t i;
    char* end;
    for (i = 0; i < sizeof(sNames) / sizeof(sNames[0]); i++) {
        size_t n = strlen(sNames[i].name);
        if (SDL_strncasecmp(s, sNames[i].name, n) == 0 && (s[n] == 0 || s[n] == '+')) {
            uint32_t off = s[n] == '+' ? (uint32_t)strtoul(s + n + 1, &end, 0) : 0;
            *addr = (uint32_t)(uintptr_t)sNames[i].addr + off;
            *size = off < sNames[i].size ? sNames[i].size - off : 1;
            return true;
        }
    }
    *addr = (uint32_t)strtoul(s, &end, 0);
    *size = 1;
    return end != s && *end == 0;
}

/* "gSave+0x40" for an address inside a named struct, else the number */
static void NameOf(uint32_t addr, char* out, size_t size) {
    size_t i;
    for (i = 0; i < sizeof(sNames) / sizeof(sNames[0]); i++) {
        uint32_t base = (uint32_t)(uintptr_t)sNames[i].addr;
        if (addr >= base && addr < base + sNames[i].size) {
            snprintf(out, size, "%s+0x%X", sNames[i].name, addr - base);
            return;
        }
    }
    snprintf(out, size, "0x%08X", addr);
}

/* ---- watch ---- */

#define MAX_WATCH 8
#define WATCH_BYTES 64
static struct {
    uint32_t addr, len;
    uint8_t last[WATCH_BYTES];
} sWatch[MAX_WATCH];
static int sWatchCount;

static void HexBytes(char* out, size_t size, const uint8_t* p, uint32_t n) {
    size_t o = 0;
    uint32_t i;
    out[0] = 0;
    for (i = 0; i < n && o + 3 < size; i++)
        o += (size_t)snprintf(out + o, size - o, "%02X", p[i]);
}

static void WatchTick(void) {
    int i;
    for (i = 0; i < sWatchCount; i++) {
        const uint8_t* now = (const uint8_t*)(uintptr_t)sWatch[i].addr;
        uint32_t first, last;
        char name[48], a[40], b[40];
        if (memcmp(now, sWatch[i].last, sWatch[i].len) == 0)
            continue;
        for (first = 0; now[first] == sWatch[i].last[first]; first++)
            ;
        for (last = sWatch[i].len - 1; now[last] == sWatch[i].last[last]; last--)
            ;
        if (last - first >= 16)
            last = first + 15;
        NameOf(sWatch[i].addr + first, name, sizeof(name));
        HexBytes(a, sizeof(a), sWatch[i].last + first, last - first + 1);
        HexBytes(b, sizeof(b), now + first, last - first + 1);
        Port_Log("watch %s: %s -> %s", name, a, b);
        memcpy(sWatch[i].last, now, sWatch[i].len);
    }
}

/* ---- break ---- */

static bool sBreakOn;
static bool sBreakHinted;

static void BreakTick(void) {
    BreakHit hits[64];
    int i, dropped, n;
    if (!sBreakOn)
        return;
    n = Port_BreakTake(hits, 64, &dropped);
    for (i = 0; i < n && i < 16; i++) {
        char name[48];
        NameOf(hits[i].addr, name, sizeof(name));
        Port_Log("break: write to %s  %08X -> %08X  by code at 0x%08X", name, hits[i].before, hits[i].after,
                 hits[i].eip);
    }
    if (n > 16 || dropped > 0)
        Port_Log("break: %d more writes this frame", n - (n > 16 ? 16 : n) + dropped);
    if (n > 0 && !sBreakHinted) {
        sBreakHinted = true;
        Port_Log("break: to find the function: addr2line -f -e tmc_pc%s 0x%08X",
#ifdef _WIN32
                 ".exe",
#else
                 "",
#endif
                 hits[0].eip);
    }
}

void Viewer_Tick(void) {
    WatchTick();
    BreakTick();
}

/* ---- commands ---- */

bool Viewer_Command(int argc, char** argv) {
    const char* cmd = argv[0];
    uint32_t addr, size;
    if (strcmp(cmd, "viewer") == 0) {
        int i;
        if (argc < 2) {
            sPage = (ViewerPage)((sPage + 1) % PAGE_COUNT);
        } else {
            for (i = 0; i < PAGE_COUNT; i++)
                if (strcmp(argv[1], sPageNames[i]) == 0)
                    sPage = (ViewerPage)i;
        }
        Debug_Message("viewer %s", sPageNames[sPage]);
        return true;
    }
    if (strcmp(cmd, "tiles") == 0) {
        sTilePage = argc > 1 ? atoi(argv[1]) % 8 : (sTilePage + 1) % 8;
        sTilePal = argc > 2 ? atoi(argv[2]) & 15 : sTilePal;
        sPage = PAGE_TILES;
        return true;
    }
    if (strcmp(cmd, "mem") == 0) {
        if (argc > 1 && ParseAddr(argv[1], &addr, &size))
            sMemAddr = addr & ~15u;
        sPage = PAGE_MEM;
        return true;
    }
    if (strcmp(cmd, "watch") == 0) {
        if (argc < 2 || !ParseAddr(argv[1], &addr, &size)) {
            Debug_Message("watch ADDR|NAME [LEN]");
            return true;
        }
        if (argc > 2)
            size = (uint32_t)strtoul(argv[2], NULL, 0);
        if (size == 0)
            size = 1;
        if (size > WATCH_BYTES)
            size = WATCH_BYTES;
        if (sWatchCount >= MAX_WATCH || !Port_IsReadable((const void*)(uintptr_t)addr, size)) {
            Debug_Message("cannot watch 0x%08X", addr);
            return true;
        }
        sWatch[sWatchCount].addr = addr;
        sWatch[sWatchCount].len = size;
        memcpy(sWatch[sWatchCount].last, (const void*)(uintptr_t)addr, size);
        sWatchCount++;
        Debug_Message("watching 0x%08X, %u bytes", addr, size);
        return true;
    }
    if (strcmp(cmd, "unwatch") == 0) {
        sWatchCount = 0;
        Debug_Message("watches off");
        return true;
    }
    if (strcmp(cmd, "break") == 0) {
        if (argc < 2 || !ParseAddr(argv[1], &addr, &size)) {
            Debug_Message("break ADDR|NAME [LEN]");
            return true;
        }
        if (argc > 2)
            size = (uint32_t)strtoul(argv[2], NULL, 0);
        sBreakOn = Port_BreakSet(addr, size);
        Debug_Message(sBreakOn ? "break on writes to 0x%08X, %u bytes" : "cannot break on 0x%08X (%u bytes)", addr,
                      size);
        return true;
    }
    if (strcmp(cmd, "unbreak") == 0) {
        Port_BreakClear();
        sBreakOn = false;
        Debug_Message("break off");
        return true;
    }
    return false;
}

void Viewer_HotKey(void) {
    sPage = (ViewerPage)((sPage + 1) % PAGE_COUNT);
}

/* ---- drawing ---- */

static uint32_t Rgb(uint16_t c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
}

static void Box(uint32_t* frame, int pitch, int w, int h, int x, int y, int bw, int bh, uint32_t color) {
    int i, j;
    for (j = y; j < y + bh && j < h; j++)
        for (i = x; i < x + bw && i < w; i++)
            if (i >= 0 && j >= 0)
                frame[j * pitch + i] = color;
}

static void DrawPalette(uint32_t* frame, int pitch, int w, int h) {
    const uint16_t* pal = (const uint16_t*)(uintptr_t)PORT_PLTT_ADDR;
    int half, i, cell = 7, x0 = 4, y0 = 12;
    Debug_Shade(frame, pitch, w, h, 0, 0, 2 * 16 * cell + 16, 16 * cell + 16);
    Debug_Text(frame, pitch, w, h, x0, 3, "PALETTE   BG", 0xFFFFFF);
    Debug_Text(frame, pitch, w, h, x0 + 16 * cell + 6, 3, "OBJ", 0xFFFFFF);
    for (half = 0; half < 2; half++) {
        for (i = 0; i < 256; i++) {
            int x = x0 + half * (16 * cell + 6) + (i & 15) * cell, y = y0 + (i >> 4) * cell;
            Box(frame, pitch, w, h, x, y, cell - 1, cell - 1, Rgb(pal[half * 256 + i]));
        }
    }
}

static void DrawTiles(uint32_t* frame, int pitch, int w, int h) {
    const uint8_t* vram = (const uint8_t*)(uintptr_t)(PORT_VRAM_ADDR + (uint32_t)sTilePage * 0x4000u);
    const uint16_t* pal = (const uint16_t*)(uintptr_t)PORT_PLTT_ADDR + (sTilePage >= 4 ? 256 : 0) + sTilePal * 16;
    int t, x, y, x0 = 2, y0 = 10;
    char buf[96];
    Debug_Shade(frame, pitch, w, h, 0, 0, 32 * 8 + 4, 16 * 8 + 12);
    snprintf(buf, sizeof(buf), "TILES PAGE %d (%s 0x%08X) PAL %d   TILES PAGE [PAL]", sTilePage,
             sTilePage >= 4 ? "OBJ" : "BG", PORT_VRAM_ADDR + sTilePage * 0x4000u, sTilePal);
    Debug_Text(frame, pitch, w, h, 2, 2, buf, 0xFFFFFF);
    for (t = 0; t < 512; t++) {
        const uint8_t* tile = vram + t * 32;
        for (y = 0; y < 8; y++) {
            for (x = 0; x < 8; x++) {
                int px = x0 + (t & 31) * 8 + x, py = y0 + (t >> 5) * 8 + y;
                int c = (tile[y * 4 + x / 2] >> ((x & 1) * 4)) & 15;
                if (px >= w || py >= h)
                    continue;
                frame[py * pitch + px] = c ? Rgb(pal[c]) : (((x ^ y) & 4) ? 0x202020 : 0x303030);
            }
        }
    }
}

static void DrawOam(uint32_t* frame, int pitch, int w, int h) {
    static const char* const sizes[3][4] = {
        { "8X8", "16X16", "32X32", "64X64" }, { "16X8", "32X8", "32X16", "64X32" }, { "8X16", "8X32", "16X32", "32X64" }
    };
    const uint16_t* oam = (const uint16_t*)(uintptr_t)PORT_OAM_ADDR;
    int i, n = 0, rows = (h - 12) / 7, colW = 27 * 4;
    char buf[64];
    Debug_Shade(frame, pitch, w, h, 0, 0, w, h);
    Debug_Text(frame, pitch, w, h, 2, 2, "OAM  N: X Y TILE PAL PRIO SIZE (A=AFFINE H/V=FLIP B=BLEND)", 0xFFFFFF);
    for (i = 0; i < 128; i++) {
        uint16_t a0 = oam[i * 4], a1 = oam[i * 4 + 1], a2 = oam[i * 4 + 2];
        int shape = a0 >> 14, x, y;
        if (!(a0 & 0x100) && (a0 & 0x200))
            continue; /* hidden */
        x = a1 & 0x1FF;
        if (x >= 256)
            x -= 512;
        y = a0 & 0xFF;
        snprintf(buf, sizeof(buf), "%3d:%4d%4d %3X %X %d %s%s%s%s%s", i, x, y, a2 & 0x3FF, a2 >> 12, (a2 >> 10) & 3,
                 shape < 3 ? sizes[shape][a1 >> 14] : "?", (a0 & 0x100) ? " A" : "",
                 !(a0 & 0x100) && (a1 & 0x1000) ? " H" : "", !(a0 & 0x100) && (a1 & 0x2000) ? " V" : "",
                 ((a0 >> 10) & 3) == 1 ? " B" : "");
        Debug_Text(frame, pitch, w, h, 2 + (n / rows) * colW, 10 + (n % rows) * 7, buf, 0xC0FFC0);
        n++;
    }
    snprintf(buf, sizeof(buf), "%d SHOWN, %d EXTRA (PC)", n, gPortOamExtraLiveCount);
    Debug_Text(frame, pitch, w, h, w - (int)strlen(buf) * 4 - 2, 2, buf, 0xFFFF80);
}

static void DrawIo(uint32_t* frame, int pitch, int w, int h) {
    static const struct {
        uint16_t off;
        const char* name;
        uint8_t wide;
    } regs[] = {
        { 0x00, "DISPCNT", 0 }, { 0x04, "DISPSTAT", 0 }, { 0x06, "VCOUNT", 0 },   { 0x08, "BG0CNT", 0 },
        { 0x0A, "BG1CNT", 0 },  { 0x0C, "BG2CNT", 0 },   { 0x0E, "BG3CNT", 0 },   { 0x10, "BG0HOFS", 0 },
        { 0x12, "BG0VOFS", 0 }, { 0x14, "BG1HOFS", 0 },  { 0x16, "BG1VOFS", 0 },  { 0x18, "BG2HOFS", 0 },
        { 0x1A, "BG2VOFS", 0 }, { 0x1C, "BG3HOFS", 0 },  { 0x1E, "BG3VOFS", 0 },  { 0x20, "BG2PA", 0 },
        { 0x22, "BG2PB", 0 },   { 0x24, "BG2PC", 0 },    { 0x26, "BG2PD", 0 },    { 0x28, "BG2X", 1 },
        { 0x2C, "BG2Y", 1 },    { 0x30, "BG3PA", 0 },    { 0x32, "BG3PB", 0 },    { 0x34, "BG3PC", 0 },
        { 0x36, "BG3PD", 0 },   { 0x38, "BG3X", 1 },     { 0x3C, "BG3Y", 1 },     { 0x40, "WIN0H", 0 },
        { 0x42, "WIN1H", 0 },   { 0x44, "WIN0V", 0 },    { 0x46, "WIN1V", 0 },    { 0x48, "WININ", 0 },
        { 0x4A, "WINOUT", 0 },  { 0x4C, "MOSAIC", 0 },   { 0x50, "BLDCNT", 0 },   { 0x52, "BLDALPHA", 0 },
        { 0x54, "BLDY", 0 },    { 0xB8, "DMA0CNT", 0 },  { 0xC4, "DMA1CNT", 0 },  { 0xD0, "DMA2CNT", 0 },
        { 0xDC, "DMA3CNT", 0 }, { 0x130, "KEYINPUT", 0 }, { 0x200, "IE", 0 },     { 0x202, "IF", 0 },
        { 0x208, "IME", 0 },
    };
    int i, rows = (h - 12) / 7;
    char buf[48];
    Debug_Shade(frame, pitch, w, h, 0, 0, w, h);
    Debug_Text(frame, pitch, w, h, 2, 2, "IO REGISTERS (AS THE GAME LAST WROTE THEM)", 0xFFFFFF);
    for (i = 0; i < (int)(sizeof(regs) / sizeof(regs[0])); i++) {
        if (regs[i].wide)
            snprintf(buf, sizeof(buf), "%-8s %08X", regs[i].name, PORT_IO32(regs[i].off));
        else
            snprintf(buf, sizeof(buf), "%-8s %04X", regs[i].name, PORT_IO16(regs[i].off));
        Debug_Text(frame, pitch, w, h, 2 + (i / rows) * 80, 10 + (i % rows) * 7, buf, 0xC0FFC0);
    }
}

static void DrawMem(uint32_t* frame, int pitch, int w, int h) {
    int rows = (h - 12) / 7, r, i;
    char buf[96], name[48];
    Debug_Shade(frame, pitch, w, h, 0, 0, w, h);
    NameOf(sMemAddr, name, sizeof(name));
    snprintf(buf, sizeof(buf), "MEM %s   MEM ADDR|GSAVE|GPLAYERENTITY|GROOMCONTROLS|GMAIN|GSCREEN|GAREA", name);
    Debug_Text(frame, pitch, w, h, 2, 2, buf, 0xFFFFFF);
    for (r = 0; r < rows; r++) {
        uint32_t a = sMemAddr + (uint32_t)r * 16;
        size_t o;
        if (!Port_IsReadable((const void*)(uintptr_t)a, 16)) {
            snprintf(buf, sizeof(buf), "%08X: (NOT MAPPED)", a);
            Debug_Text(frame, pitch, w, h, 2, 10 + r * 7, buf, 0x808080);
            continue;
        }
        o = (size_t)snprintf(buf, sizeof(buf), "%08X:", a);
        for (i = 0; i < 16; i++)
            o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%s%02X", (i & 3) ? " " : "  ",
                                  ((const uint8_t*)(uintptr_t)a)[i]);
        Debug_Text(frame, pitch, w, h, 2, 10 + r * 7, buf, 0xC0FFC0);
    }
}

void Viewer_Draw(uint32_t* frame, int pitch, int w, int h) {
    switch (sPage) {
        case PAGE_PALETTE:
            DrawPalette(frame, pitch, w, h);
            break;
        case PAGE_TILES:
            DrawTiles(frame, pitch, w, h);
            break;
        case PAGE_OAM:
            DrawOam(frame, pitch, w, h);
            break;
        case PAGE_IO:
            DrawIo(frame, pitch, w, h);
            break;
        case PAGE_MEM:
            DrawMem(frame, pitch, w, h);
            break;
        default:
            break;
    }
}
