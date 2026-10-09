/**
 * @file debug.c
 * @brief Debug tools: command console, hotkeys, info overlay, savestates.
 *
 * Commands come from the in-game console (` key), from --cmd FRAME:COMMAND on
 * the command line, or from hotkeys. They are executed at a frame boundary
 * (start of WaitForNextFrame), where the game is between two frames and no
 * game function is on the stack except the main loop, so changing game state
 * or swapping the whole memory (savestates) is safe there.
 */
#include "port.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "area.h"
#include "entity.h"
#include "flags.h"
#include "game.h"
#include "item.h"
#include "main.h"
#include "player.h"
#include "room.h"
#include "save.h"
#include "transitions.h"

void* Dma_StateData(size_t* size);
void* Intr_StateData(size_t* size);

bool gDebugOverlay;
bool gDebugGodMode;

/* ---- command queue ---- */

#define MAX_QUEUE 32
#define MAX_CMD 128
static char sQueue[MAX_QUEUE][MAX_CMD];
static int sQueueCount;

void Debug_QueueCommand(const char* cmd) {
    if (sQueueCount >= MAX_QUEUE)
        return;
    snprintf(sQueue[sQueueCount++], MAX_CMD, "%s", cmd);
}

/* ---- console / message line ---- */

static bool sConsoleOpen;
static char sConsoleLine[MAX_CMD];
static char sMessage[96];
static int sMessageTimer;

static void DbgMessage(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sMessage, sizeof(sMessage), fmt, ap);
    va_end(ap);
    sMessageTimer = 180;
    Port_Log("%s", sMessage);
}

bool Debug_ConsoleOpen(void) {
    return sConsoleOpen;
}

void Debug_SetConsoleOpen(bool open) {
    sConsoleOpen = open;
    sConsoleLine[0] = 0;
}

void Debug_ConsoleText(const char* text) {
    size_t len = strlen(sConsoleLine);
    for (; *text && len + 1 < sizeof(sConsoleLine); text++) {
        if (*text == '`' || *text == '~')
            continue;
        sConsoleLine[len++] = *text;
    }
    sConsoleLine[len] = 0;
}

void Debug_ConsoleBackspace(void) {
    size_t len = strlen(sConsoleLine);
    if (len > 0)
        sConsoleLine[len - 1] = 0;
}

void Debug_ConsoleSubmit(void) {
    if (sConsoleLine[0])
        Debug_QueueCommand(sConsoleLine);
    Debug_SetConsoleOpen(false);
}

/* ---- helpers ---- */

static bool InGame(void) {
    return gMain.task == TASK_GAME;
}

static const RoomHeader* GetRoomHeader(int area, int room) {
    const RoomHeader* hdr;
    int i;
    if (area < 0 || area >= 0x90 || room < 0 || room >= MAX_ROOMS)
        return NULL;
    hdr = gAreaRoomHeaders[area];
    if (hdr == NULL)
        return NULL;
    for (i = 0; i <= room; i++) {
        if (hdr[i].map_x == 0xFFFF)
            return NULL;
    }
    if (hdr[room].tileSet_id == 0xFFFF || hdr[room].pixel_width == 0)
        return NULL;
    return &hdr[room];
}

static bool Warp(int area, int room, int x, int y) {
    static Transition t;
    const RoomHeader* hdr = GetRoomHeader(area, room);
    if (!InGame()) {
        DbgMessage("warp: start a game first");
        return false;
    }
    if (hdr == NULL) {
        DbgMessage("warp: no room %d in area %d", room, area);
        return false;
    }
    if (x < 0)
        x = hdr->pixel_width / 2;
    if (y < 0)
        y = hdr->pixel_height / 2;
    memset(&t, 0, sizeof(t));
    t.warp_type = WARP_TYPE_BORDER;
    t.area = area;
    t.room = room;
    t.endX = x;
    t.endY = y;
    t.layer = 1;
    t.facing_direction = 4;
    DoExitTransition(&t);
    DbgMessage("warp area 0x%02X room 0x%02X (%d,%d)", area, room, x, y);
    return true;
}

static void CycleRoom(int dir) {
    int area = gRoomControls.area;
    int room = gRoomControls.room;
    int i;
    for (i = 0; i < MAX_ROOMS; i++) {
        room = (room + dir + MAX_ROOMS) % MAX_ROOMS;
        if (GetRoomHeader(area, room)) {
            Warp(area, room, -1, -1);
            return;
        }
    }
}

static void CycleArea(int dir) {
    int area = gRoomControls.area;
    int i, room;
    for (i = 0; i < 0x90; i++) {
        area = (area + dir + 0x90) % 0x90;
        for (room = 0; room < MAX_ROOMS; room++) {
            if (GetRoomHeader(area, room)) {
                Warp(area, room, -1, -1);
                return;
            }
        }
    }
}

static const u8 sAllItems[] = {
    ITEM_SMITH_SWORD, ITEM_GREEN_SWORD, ITEM_RED_SWORD, ITEM_BLUE_SWORD, ITEM_FOURSWORD, ITEM_BOMBS,
    ITEM_BOW, ITEM_LIGHT_ARROW, ITEM_MAGIC_BOOMERANG, ITEM_MIRROR_SHIELD, ITEM_LANTERN_OFF, ITEM_GUST_JAR,
    ITEM_PACCI_CANE, ITEM_MOLE_MITTS, ITEM_ROCS_CAPE, ITEM_PEGASUS_BOOTS, ITEM_OCARINA, ITEM_BOTTLE1,
    ITEM_BOTTLE2, ITEM_BOTTLE3, ITEM_BOTTLE4, ITEM_GRIP_RING, ITEM_POWER_BRACELETS, ITEM_FLIPPERS,
    ITEM_SKILL_SPIN_ATTACK, ITEM_SKILL_ROLL_ATTACK, ITEM_SKILL_DASH_ATTACK, ITEM_SKILL_ROCK_BREAKER,
    ITEM_SKILL_SWORD_BEAM, ITEM_SKILL_GREAT_SPIN, ITEM_SKILL_DOWN_THRUST, ITEM_SKILL_PERIL_BEAM, ITEM_LARGE_QUIVER,
};

static void GiveAllItems(void) {
    int i;
    for (i = 0; i < (int)sizeof(sAllItems); i++)
        SetInventoryValue(sAllItems[i], 1);
    for (i = 0; i < 4; i++) {
        if (gSave.stats.bottles[i] == 0)
            gSave.stats.bottles[i] = ITEM_BOTTLE_EMPTY;
    }
    gSave.stats.bombBagType = 3;
    gSave.stats.quiverType = 3;
    gSave.stats.walletType = 3;
    ModBombs(99);
    ModArrows(99);
    ModRupees(999);
    gSave.stats.maxHealth = 20 * 8;
    gSave.stats.health = gSave.stats.maxHealth;
    LoadItemGfx();
    DbgMessage("all items");
}

/* ---- savestates ---- */

#define STATE_MAGIC 0x31534D54u /* "TMS1" */

typedef struct {
    void* ptr;
    size_t size;
} StateBlob;

void* Port_GfxSlotState(uint32_t* size); /* src/vram.c */

static int GetBlobs(StateBlob* b) {
    int n = 0;
    b[n].ptr = (void*)(uintptr_t)PORT_EWRAM_ADDR, b[n++].size = PORT_EWRAM_SIZE;
    b[n].ptr = (void*)(uintptr_t)PORT_IWRAM_ADDR, b[n++].size = PORT_IWRAM_SIZE;
    b[n].ptr = (void*)(uintptr_t)PORT_IO_ADDR, b[n++].size = PORT_IO_SIZE;
    b[n].ptr = (void*)(uintptr_t)PORT_PLTT_ADDR, b[n++].size = PORT_PLTT_SIZE;
    b[n].ptr = (void*)(uintptr_t)PORT_VRAM_ADDR, b[n++].size = PORT_VRAM_SIZE;
    b[n].ptr = (void*)(uintptr_t)PORT_OAM_ADDR, b[n++].size = PORT_OAM_SIZE;
    b[n].ptr = Dma_StateData(&b[n].size), n++;
    b[n].ptr = Intr_StateData(&b[n].size), n++;
    b[n].ptr = gPortOamExtWork, b[n++].size = sizeof(PortOamExt) * 128;
    b[n].ptr = gPortOamExtLive, b[n++].size = sizeof(PortOamExt) * 128;
    {
        uint32_t size;
        b[n].ptr = Port_GfxSlotState(&size), b[n++].size = size;
    }
    return n;
}

static void StatePath(char* buf, size_t size, int slot) {
    snprintf(buf, size, "tmc_state%d.bin", slot);
}

static void DbgSaveState(int slot) {
    StateBlob b[16];
    int i, n = GetBlobs(b);
    uint32_t hdr[2] = { STATE_MAGIC, 0 };
    char path[64];
    FILE* f;
    StatePath(path, sizeof(path), slot);
    f = fopen(path, "wb");
    if (f == NULL) {
        DbgMessage("cannot write %s", path);
        return;
    }
    for (i = 0; i < n; i++)
        hdr[1] += (uint32_t)b[i].size;
    fwrite(hdr, sizeof(hdr), 1, f);
    for (i = 0; i < n; i++)
        fwrite(b[i].ptr, 1, b[i].size, f);
    fclose(f);
    DbgMessage("saved state %d", slot);
}

static void DbgLoadState(int slot) {
    StateBlob b[16];
    int i, n = GetBlobs(b);
    uint32_t hdr[2], total = 0;
    char path[64];
    FILE* f;
    StatePath(path, sizeof(path), slot);
    f = fopen(path, "rb");
    if (f == NULL) {
        DbgMessage("no state %d", slot);
        return;
    }
    for (i = 0; i < n; i++)
        total += (uint32_t)b[i].size;
    if (fread(hdr, sizeof(hdr), 1, f) != 1 || hdr[0] != STATE_MAGIC || hdr[1] != total) {
        fclose(f);
        DbgMessage("state %d is from another version", slot);
        return;
    }
    for (i = 0; i < n; i++) {
        if (fread(b[i].ptr, 1, b[i].size, f) != b[i].size) {
            DbgMessage("state %d is truncated", slot);
            break;
        }
    }
    fclose(f);
    /* the view is normally latched at vblank, latch it again for the loaded state */
    View_PrepareFrame();
    DbgMessage("loaded state %d", slot);
}

/* ---- commands ---- */

static int Num(const char* s, int def) {
    char* end;
    long v;
    if (s == NULL)
        return def;
    v = strtol(s, &end, 0);
    return end == s ? def : (int)v;
}

static const char* const sHelp[] = {
    "warp AREA ROOM [X Y]  room N  area N  next/prev  nextarea/prevarea",
    "items  item ID [0-2]  equip ID A|B  hearts N  heal  rupees N  bombs N  arrows N  shells N  keys N",
    "god  flag N [0/1]  savestate [N]  loadstate [N]  scale S  view (toggle) hud  info  pos",
};

static void Execute(char* line) {
    char* argv[8];
    int argc = 0;
    char* tok = strtok(line, " \t");
    const char* cmd;
    while (tok && argc < 8) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    if (argc == 0)
        return;
    cmd = argv[0];
#define ARG(i, d) Num(argc > (i) ? argv[i] : NULL, d)
#define IS(s) (strcmp(cmd, s) == 0)
    if (IS("help") || IS("?")) {
        int i;
        for (i = 0; i < (int)(sizeof(sHelp) / sizeof(sHelp[0])); i++)
            Port_Log("%s", sHelp[i]);
        DbgMessage("commands: see console/stdout, or PC_PORT.md");
    } else if (IS("savestate") || IS("ss")) {
        DbgSaveState(ARG(1, 0));
    } else if (IS("loadstate") || IS("ls")) {
        DbgLoadState(ARG(1, 0));
    } else if (IS("scale")) {
        float s = argc > 1 ? (float)atof(argv[1]) : 3.0f;
        if (s >= 1.0f && s <= 8.0f)
            gPortConfig.scale = s;
        DbgMessage("scale %.2f", gPortConfig.scale);
    } else if (IS("view")) {
        gPortConfig.extendedView = !gPortConfig.extendedView;
        DbgMessage("extended view %s", gPortConfig.extendedView ? "on" : "off");
    } else if (IS("hud")) {
        gPortConfig.hudAnchor = !gPortConfig.hudAnchor;
        DbgMessage("hud corners %s", gPortConfig.hudAnchor ? "on" : "off");
    } else if (IS("info")) {
        gDebugOverlay = !gDebugOverlay;
    } else if (!InGame()) {
        DbgMessage("'%s' needs a running game", cmd);
    } else if (IS("warp")) {
        Warp(ARG(1, 0), ARG(2, 0), ARG(3, -1), ARG(4, -1));
    } else if (IS("room")) {
        Warp(gRoomControls.area, ARG(1, 0), ARG(2, -1), ARG(3, -1));
    } else if (IS("area")) {
        Warp(ARG(1, 0), ARG(2, 0), -1, -1);
    } else if (IS("next")) {
        CycleRoom(1);
    } else if (IS("prev")) {
        CycleRoom(-1);
    } else if (IS("nextarea")) {
        CycleArea(1);
    } else if (IS("prevarea")) {
        CycleArea(-1);
    } else if (IS("pos")) {
        if (argc >= 3) {
            /* move Link inside the room (no reload) */
            gPlayerEntity.base.x.HALF.HI = gRoomControls.origin_x + ARG(1, 0);
            gPlayerEntity.base.y.HALF.HI = gRoomControls.origin_y + ARG(2, 0);
        }
        DbgMessage("area 0x%02X room 0x%02X x %d y %d", gRoomControls.area, gRoomControls.room,
                gPlayerEntity.base.x.HALF.HI - gRoomControls.origin_x,
                gPlayerEntity.base.y.HALF.HI - gRoomControls.origin_y);
    } else if (IS("items")) {
        GiveAllItems();
    } else if (IS("item")) {
        int id = ARG(1, -1);
        if (id <= 0 || id >= 0x88) {
            DbgMessage("item: bad id");
        } else {
            SetInventoryValue(id, ARG(2, 1));
            LoadItemGfx();
            DbgMessage("item 0x%02X = %d", id, ARG(2, 1));
        }
    } else if (IS("equip")) {
        int id = ARG(1, -1);
        const char* slot = argc > 2 ? argv[2] : "A";
        if (id <= 0 || id >= 0x88) {
            DbgMessage("equip: bad id");
        } else {
            if (GetInventoryValue(id) == 0)
                SetInventoryValue(id, 1);
            ForceEquipItem(id, (slot[0] == 'B' || slot[0] == 'b') ? EQUIP_SLOT_B : EQUIP_SLOT_A);
            LoadItemGfx();
            DbgMessage("equip 0x%02X on %s", id, (slot[0] == 'B' || slot[0] == 'b') ? "B" : "A");
        }
    } else if (IS("hearts")) {
        int h = ARG(1, 20);
        if (h < 1)
            h = 1;
        if (h > 20)
            h = 20;
        gSave.stats.maxHealth = h * 8;
        gSave.stats.health = h * 8;
        DbgMessage("%d hearts", h);
    } else if (IS("heal")) {
        gSave.stats.health = gSave.stats.maxHealth;
        DbgMessage("healed");
    } else if (IS("rupees")) {
        gSave.stats.rupees = 0;
        ModRupees(ARG(1, 999));
        DbgMessage("rupees %d", gSave.stats.rupees);
    } else if (IS("bombs")) {
        gSave.stats.bombCount = 0;
        ModBombs(ARG(1, 99));
    } else if (IS("arrows")) {
        gSave.stats.arrowCount = 0;
        ModArrows(ARG(1, 99));
    } else if (IS("shells")) {
        gSave.stats.shells = ARG(1, 999);
        SetInventoryValue(ITEM_SHELLS, 1);
    } else if (IS("keys")) {
        gSave.dungeonKeys[gArea.dungeon_idx] = ARG(1, 9);
        DbgMessage("keys %d", gSave.dungeonKeys[gArea.dungeon_idx]);
    } else if (IS("god")) {
        gDebugGodMode = !gDebugGodMode;
        DbgMessage("god mode %s", gDebugGodMode ? "on" : "off");
    } else if (IS("flag")) {
        int flag = ARG(1, -1);
        if (flag < 0) {
            DbgMessage("flag N [0/1]");
        } else if (argc > 2) {
            if (ARG(2, 1))
                SetGlobalFlag(flag);
            else
                ClearGlobalFlag(flag);
            DbgMessage("flag 0x%X = %d", flag, CheckGlobalFlag(flag) ? 1 : 0);
        } else {
            DbgMessage("flag 0x%X = %d", flag, CheckGlobalFlag(flag) ? 1 : 0);
        }
    } else {
        DbgMessage("unknown command '%s' (try help)", cmd);
    }
#undef ARG
#undef IS
}

/** Called from WaitForNextFrame before the vblank wait: a safe point to change game state. */
void Port_FrameBoundary(void) {
    int i;
    for (i = 0; i < sQueueCount; i++)
        Execute(sQueue[i]);
    sQueueCount = 0;
    if (gDebugGodMode && InGame())
        gSave.stats.health = gSave.stats.maxHealth;
    /* messages stay up for a fixed number of game frames */
    if (sMessageTimer > 0)
        sMessageTimer--;
}

/* ---- hotkeys ---- */

bool Debug_HotKey(int fkey, bool shift) {
    char buf[32];
    switch (fkey) {
        case 2:
            gDebugOverlay = !gDebugOverlay;
            return true;
        case 4:
            Debug_QueueCommand("items");
            return true;
        case 5:
            Debug_QueueCommand("savestate 0");
            return true;
        case 6:
            Debug_QueueCommand(shift ? "prevarea" : "prev");
            return true;
        case 7:
            Debug_QueueCommand(shift ? "nextarea" : "next");
            return true;
        case 8:
            Debug_QueueCommand("god");
            return true;
        case 9:
            snprintf(buf, sizeof(buf), "loadstate 0");
            Debug_QueueCommand(buf);
            return true;
        default:
            return false;
    }
}

/* ---- overlay ---- */

/* 3x5 font, each glyph is 5 rows of 3 bits */
static const char sFontChars[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:.,-/=+()_>?!'%[]";
static const uint16_t sFont[] = {
    0x0000, 0x7B6F, 0x2C97, 0x62A7, 0x628E, 0x5BC9, 0x798E, 0x39EF, 0x7292, 0x7BEF, 0x7BCE, 0x2BED,
    0x6BAE, 0x3923, 0x6B6E, 0x79A7, 0x79A4, 0x396B, 0x5BED, 0x7497, 0x126A, 0x5BAD, 0x4927, 0x5FED,
    0x6B6D, 0x2B6A, 0x6BA4, 0x2B73, 0x6BAD, 0x388E, 0x7492, 0x5B6F, 0x5B6A, 0x5BFD, 0x5AAD, 0x5A92,
    0x72A7, 0x0410, 0x0002, 0x0014, 0x01C0, 0x12A4, 0x0E38, 0x05D0, 0x2922, 0x224A, 0x0007, 0x4454,
    0x6282, 0x2482, 0x2400, 0x52A5, 0x6926, 0x324B,
};

static int GlyphIndex(char c) {
    const char* p;
    if (c >= 'a' && c <= 'z')
        c -= 32;
    p = strchr(sFontChars, c);
    return p ? (int)(p - sFontChars) : 0;
}

static void DrawText(uint32_t* frame, int pitch, int w, int h, int x, int y, const char* s, uint32_t color) {
    for (; *s; s++, x += 4) {
        uint16_t g = sFont[GlyphIndex(*s)];
        int r, c;
        for (r = 0; r < 5; r++) {
            for (c = 0; c < 3; c++) {
                if (g & (1 << (14 - r * 3 - c))) {
                    int px = x + c, py = y + r;
                    if (px >= 0 && py >= 0 && px + 1 < w && py + 1 < h) {
                        frame[(py + 1) * pitch + px + 1] = 0x000000;
                        frame[py * pitch + px] = color;
                    }
                }
            }
        }
    }
}

static void FillRect(uint32_t* frame, int pitch, int w, int h, int x, int y, int rw, int rh) {
    int i, j;
    for (j = y; j < y + rh && j < h; j++)
        for (i = x; i < x + rw && i < w; i++)
            if (i >= 0 && j >= 0)
                frame[j * pitch + i] = (frame[j * pitch + i] >> 2) & 0x3F3F3F;
}

void Debug_DrawOverlay(uint32_t* frame, int pitch, int w, int h, uint64_t frameCount) {
    char buf[96];
    if (gDebugOverlay) {
        int y = 2;
        FillRect(frame, pitch, w, h, 0, 0, 4 * 30 + 4, 7 * 4 + 2);
        snprintf(buf, sizeof(buf), "FRAME %u  VIEW %dX%d", (unsigned)frameCount, w, h);
        DrawText(frame, pitch, w, h, 2, y, buf, 0xFFFFFF);
        y += 7;
        snprintf(buf, sizeof(buf), "TASK %d %d %d", gMain.task, gMain.state, gMain.substate);
        DrawText(frame, pitch, w, h, 2, y, buf, 0xFFFFFF);
        y += 7;
        if (InGame()) {
            snprintf(buf, sizeof(buf), "AREA %02X ROOM %02X", gRoomControls.area, gRoomControls.room);
            DrawText(frame, pitch, w, h, 2, y, buf, 0xFFFF80);
            y += 7;
            snprintf(buf, sizeof(buf), "X %d Y %d%s", gPlayerEntity.base.x.HALF.HI - gRoomControls.origin_x,
                     gPlayerEntity.base.y.HALF.HI - gRoomControls.origin_y, gDebugGodMode ? " GOD" : "");
            DrawText(frame, pitch, w, h, 2, y, buf, 0xFFFF80);
        }
    }
    if (sConsoleOpen) {
        FillRect(frame, pitch, w, h, 0, h - 9, w, 9);
        snprintf(buf, sizeof(buf), "> %s_", sConsoleLine);
        DrawText(frame, pitch, w, h, 2, h - 7, buf, 0x80FF80);
    } else if (sMessageTimer > 0) {
        FillRect(frame, pitch, w, h, 0, h - 9, (int)strlen(sMessage) * 4 + 4, 9);
        DrawText(frame, pitch, w, h, 2, h - 7, sMessage, 0xFFFFFF);
    }
}

/* ---- determinism check: TMC_HASH_LOG=1 prints a hash of the game RAM every frame ---- */

/* words that look like pointers into the program image differ between builds and are skipped */
static uint32_t Fnv(const uint8_t* p, size_t n, uint32_t h) {
    const uint32_t* w = (const uint32_t*)p;
    size_t i;
    for (i = 0; i < n / 4; i++) {
        uint32_t v = w[i];
        if (v >= 0x08000000u && v < 0x20000000u)
            v = 0;
        h = (h ^ v) * 16777619u;
    }
    return h;
}

static void HashRegions(uint64_t frameCount) {
    /* finer grained hashes to locate a difference */
    static const char* env;
    uint32_t base, size, off;
    if (env == NULL)
        env = getenv("TMC_HASH_DETAIL");
    if (env == NULL || strtoul(env, NULL, 0) != frameCount)
        return;
    if (getenv("TMC_DUMP")) {
        uint32_t a = strtoul(getenv("TMC_DUMP"), NULL, 16);
        for (off = 0; off < 0x400; off += 4)
            printf("mem %08X %08X\n", a + off, *(uint32_t*)(uintptr_t)(a + off));
    }
    for (base = PORT_EWRAM_ADDR, size = PORT_EWRAM_SIZE; base;) {
        for (off = 0; off < size; off += 0x100)
            printf("blk %08X %08X\n", base + off, Fnv((const uint8_t*)(uintptr_t)(base + off), 0x100, 2166136261u));
        if (base == PORT_EWRAM_ADDR)
            base = PORT_IWRAM_ADDR, size = PORT_IWRAM_SIZE;
        else
            base = 0;
    }
}

void Debug_HashFrame(uint64_t frameCount) {
    static int enabled = -1;
    uint32_t e, i;
    if (enabled < 0)
        enabled = getenv("TMC_HASH_LOG") != NULL;
    if (!enabled)
        return;
    e = Fnv((const uint8_t*)(uintptr_t)PORT_EWRAM_ADDR, PORT_EWRAM_SIZE, 2166136261u);
    /* 0x03004000-0x030043FF holds the sound code copied to RAM (native code in the port) */
    i = Fnv((const uint8_t*)(uintptr_t)PORT_IWRAM_ADDR, 0x4000, 2166136261u);
    i = Fnv((const uint8_t*)(uintptr_t)PORT_IWRAM_ADDR + 0x4400, PORT_IWRAM_SIZE - 0x4400, i);
    printf("hash %u %08X %08X\n", (unsigned)frameCount, e, i);
    if (getenv("TMC_DUMP_ENT")) {
        const uint8_t* p = (const uint8_t*)(uintptr_t)strtoul(getenv("TMC_DUMP_ENT"), NULL, 16);
        int k;
        printf("ent %u", (unsigned)frameCount);
        for (k = 0; k < 0x88; k++)
            printf("%s%02X", (k & 3) ? "" : " ", p[k]);
        printf("\n");
    }
    HashRegions(frameCount);
}
