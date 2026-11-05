/**
 * @file bookmark.c
 * @brief Bookmarks: a short pasteable text that puts the game at one spot.
 *
 * A bookmark holds the whole save file (story flags, items, stats) and the
 * area, room, position, layer and facing of Link. Loading one starts the game
 * the way loading a file from the file select does (SetTask(TASK_GAME) with
 * the saved spawn point), so the room is entered through the game's own code.
 * Like a save, it lands in the state of entering the room: anything that ran
 * after entering it (a cutscene, a puzzle) has to happen again.
 *
 * Text format: "TMC1:" + base64url of (zero-run-length coded payload + 16 bit
 * check). Payload: format byte, area, room, layer, facing, x (s16), y (s16),
 * the save file (sizeof(SaveFile)).
 *
 * A bookmark can be loaded at any time, also from the title screen
 * (--bookmark on the command line boots straight into it). Writing the save
 * file is turned off afterwards, so trying someone's bookmark can't replace
 * your own save.
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "entity.h"
#include "game.h"
#include "main.h"
#include "player.h"
#include "room.h"
#include "save.h"

#define FORMAT 1
#define HEADER 10
#define PAYLOAD (HEADER + sizeof(SaveFile))

bool gPortSaveLocked;

static uint8_t sPending[PAYLOAD];
static bool sHavePending;
/* frames since the bookmark was loaded, until gameplay runs */
static int sWaitFrames = -1;
static int sReadyFrames;

static const char sB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static uint16_t Check(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return (uint16_t)(h ^ (h >> 16));
}

static void BuildPayload(uint8_t* p) {
    int x = gPlayerEntity.base.x.HALF.HI - gRoomControls.origin_x;
    int y = gPlayerEntity.base.y.HALF.HI - gRoomControls.origin_y;
    p[0] = FORMAT;
    p[1] = gRoomControls.area;
    p[2] = gRoomControls.room;
    p[3] = gPlayerEntity.base.collisionLayer;
    p[4] = gPlayerEntity.base.animationState;
    p[5] = 0;
    p[6] = (uint8_t)x, p[7] = (uint8_t)(x >> 8);
    p[8] = (uint8_t)y, p[9] = (uint8_t)(y >> 8);
    memcpy(p + HEADER, &gSave, sizeof(SaveFile));
}

bool Bookmark_Make(char* out, size_t size) {
    uint8_t payload[PAYLOAD];
    uint8_t packed[PAYLOAD * 2 + 2];
    size_t n = 0, i, o;
    uint16_t check;
    uint32_t bits = 0;
    int nbits = 0;
    if (gMain.task != TASK_GAME)
        return false;
    BuildPayload(payload);
    for (i = 0; i < PAYLOAD;) {
        if (payload[i] == 0) {
            size_t run = 0;
            while (i < PAYLOAD && payload[i] == 0 && run < 255)
                i++, run++;
            packed[n++] = 0;
            packed[n++] = (uint8_t)run;
        } else {
            packed[n++] = payload[i++];
        }
    }
    check = Check(packed, n);
    packed[n++] = (uint8_t)check;
    packed[n++] = (uint8_t)(check >> 8);
    o = (size_t)snprintf(out, size, "TMC1:");
    for (i = 0; i < n; i++) {
        bits = (bits << 8) | packed[i];
        nbits += 8;
        while (nbits >= 6) {
            if (o + 1 < size)
                out[o++] = sB64[(bits >> (nbits - 6)) & 63];
            nbits -= 6;
        }
    }
    if (nbits > 0 && o + 1 < size)
        out[o++] = sB64[(bits << (6 - nbits)) & 63];
    out[o < size ? o : size - 1] = 0;
    return o + 1 < size;
}

static int B64Value(char c) {
    const char* p = c ? strchr(sB64, c) : NULL;
    return p ? (int)(p - sB64) : -1;
}

/* decode into sPending; returns an error text or NULL */
static const char* Decode(const char* text, uint8_t* payload) {
    uint8_t packed[PAYLOAD * 2 + 2];
    size_t n = 0, i, o = 0;
    uint32_t bits = 0;
    int nbits = 0;
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r' || *text == '`')
        text++;
    if (strncmp(text, "TMC1:", 5) != 0)
        return "not a bookmark (they start with TMC1:)";
    for (text += 5; *text; text++) {
        int v = B64Value(*text);
        if (v < 0) {
            if (*text == ' ' || *text == '\n' || *text == '\r' || *text == '`')
                break;
            return "bookmark has a bad character";
        }
        bits = (bits << 6) | (uint32_t)v;
        nbits += 6;
        if (nbits >= 8) {
            if (n >= sizeof(packed))
                return "bookmark is too long";
            packed[n++] = (uint8_t)(bits >> (nbits - 8));
            nbits -= 8;
        }
    }
    if (n < 3)
        return "bookmark is too short";
    n -= 2;
    if (Check(packed, n) != (uint16_t)(packed[n] | (packed[n + 1] << 8)))
        return "bookmark is incomplete or mistyped";
    for (i = 0; i < n; i++) {
        if (packed[i] == 0) {
            size_t run;
            if (i + 1 >= n)
                return "bookmark is damaged";
            run = packed[++i];
            if (o + run > PAYLOAD)
                return "bookmark is damaged";
            memset(payload + o, 0, run);
            o += run;
        } else {
            if (o >= PAYLOAD)
                return "bookmark is damaged";
            payload[o++] = packed[i];
        }
    }
    if (o != PAYLOAD || payload[0] != FORMAT)
        return "bookmark is from another version";
    return NULL;
}

const char* Bookmark_Load(const char* text) {
    const char* err = Decode(text, sPending);
    if (err != NULL)
        return err;
    sHavePending = true;
    return NULL;
}

bool Bookmark_Describe(const char* text, char* out, size_t size) {
    uint8_t p[PAYLOAD];
    if (Decode(text, p) != NULL)
        return false;
    snprintf(out, size, "area 0x%02X room 0x%02X x %d y %d", p[1], p[2], (int16_t)(p[6] | (p[7] << 8)),
             (int16_t)(p[8] | (p[9] << 8)));
    return true;
}

/* Called at the frame boundary. Returns true while a bookmark is being entered. */
bool Bookmark_Tick(void) {
    if (sHavePending) {
        /* the game's start-up must have run (title task), like picking a file */
        if (gMain.task == TASK_TITLE && gMain.state == 0)
            return true;
        sHavePending = false;
        memcpy(&gSave, sPending + HEADER, sizeof(SaveFile));
        gSave.saved_status.area_next = sPending[1];
        gSave.saved_status.room_next = sPending[2];
        gSave.saved_status.layer = sPending[3] ? sPending[3] : 1;
        gSave.saved_status.start_anim = sPending[4];
        gSave.saved_status.spawn_type = PL_SPAWN_DEFAULT;
        gSave.saved_status.start_pos_x = (int16_t)(sPending[6] | (sPending[7] << 8));
        gSave.saved_status.start_pos_y = (int16_t)(sPending[8] | (sPending[9] << 8));
        gPortSaveLocked = true;
        SetTask(TASK_GAME);
        sWaitFrames = 0;
        sReadyFrames = 0;
        Port_Log("bookmark: area 0x%02X room 0x%02X x %d y %d (saving is off until restart)", sPending[1],
                 sPending[2], gSave.saved_status.start_pos_x, gSave.saved_status.start_pos_y);
        return true;
    }
    if (sWaitFrames >= 0) {
        sWaitFrames++;
        /* wait for the room to be entered and the fade in to end */
        if (gMain.task == TASK_GAME && gMain.state == GAMETASK_MAIN && gMain.substate == GAMEMAIN_UPDATE)
            sReadyFrames++;
        else
            sReadyFrames = 0;
        if (sReadyFrames >= 2 || sWaitFrames > 2000) {
            sWaitFrames = -1;
            return false;
        }
        return true;
    }
    return false;
}

bool Bookmark_Busy(void) {
    return sHavePending || sWaitFrames >= 0;
}
