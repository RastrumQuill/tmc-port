/**
 * @file route.c
 * @brief Test routes: a bookmark plus timed input, screenshots and commands.
 *
 * A route reproduces one issue from a known spot, so it can be re-run before
 * every release (port/tools/routes.sh) and the shots compared. File format,
 * one item per line ('#' starts a comment):
 *
 *   bookmark TMC1:...            where to start (or bookmarks/NAME.txt)
 *   0-40 RIGHT+B                 hold keys on frames 0..40
 *   60 A                         press keys on one frame
 *   100-400/30 A                 tap every 30 frames (4 frames down)
 *   80 shot swirl                save swirl.bmp
 *   100-200/10 shot spin         save spin_00100.bmp, spin_00110.bmp, ...
 *   90 fills                     any console command
 *   200 end                      exit the game
 *
 * Frames count from the moment the bookmark has loaded and gameplay runs
 * (frame 0 = first frame Link can move). Without a bookmark they count from
 * the start of the program. Shots go to the current folder.
 *
 * A line "#! ARGS" is not read by the game: port/tools/routes.sh passes ARGS
 * on the command line (e.g. the window size the route is meant for).
 */
#include "port.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ITEMS 512

typedef enum { ITEM_KEYS, ITEM_SHOT, ITEM_CMD, ITEM_END } ItemKind;

typedef struct {
    uint32_t start, end, period;
    ItemKind kind;
    uint16_t keys;
    char text[120];
} RouteItem;

static RouteItem sItems[MAX_ITEMS];
static int sCount;
static bool sActive;
static bool sStarted;
static int64_t sFrame = -1;
static const char* sShot;
static char sName[64];

static char* Trim(char* s) {
    char* e;
    while (isspace((unsigned char)*s))
        s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

static void ReadBookmarkFile(const char* path, char* out, size_t size) {
    FILE* f = fopen(path, "rb");
    out[0] = 0;
    if (f == NULL)
        return;
    while (fgets(out, (int)size, f) != NULL) {
        char* t = Trim(out);
        if (strncmp(t, "TMC1:", 5) == 0) {
            memmove(out, t, strlen(t) + 1);
            fclose(f);
            return;
        }
    }
    out[0] = 0;
    fclose(f);
}

bool Route_Load(const char* path) {
    char line[3000];
    int lineNo = 0;
    const char* base;
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        Port_Log("route: cannot open %s", path);
        return false;
    }
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(sName, sizeof(sName), "%s", base);
    if (strchr(sName, '.'))
        *strchr(sName, '.') = 0;
    sCount = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        char *t, *rest, *hash;
        RouteItem* it;
        lineNo++;
        hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        t = Trim(line);
        if (*t == 0)
            continue;
        if (strncmp(t, "bookmark ", 9) == 0) {
            char text[2800];
            const char* err;
            rest = Trim(t + 9);
            if (strncmp(rest, "TMC1:", 5) == 0)
                snprintf(text, sizeof(text), "%s", rest);
            else
                ReadBookmarkFile(rest, text, sizeof(text));
            err = Bookmark_Load(text);
            if (err != NULL)
                Port_Log("route %s line %d: %s", sName, lineNo, err);
            continue;
        }
        if (!isdigit((unsigned char)*t) || sCount >= MAX_ITEMS) {
            Port_Log("route %s line %d: expected FRAME ...", sName, lineNo);
            continue;
        }
        it = &sItems[sCount];
        memset(it, 0, sizeof(*it));
        it->start = it->end = strtoul(t, &rest, 10);
        if (*rest == '-')
            it->end = strtoul(rest + 1, &rest, 10);
        if (*rest == '/')
            it->period = strtoul(rest + 1, &rest, 10);
        rest = Trim(rest);
        if (strcmp(rest, "end") == 0) {
            it->kind = ITEM_END;
        } else if (strncmp(rest, "shot ", 5) == 0) {
            it->kind = ITEM_SHOT;
            snprintf(it->text, sizeof(it->text), "%s", Trim(rest + 5));
        } else if ((it->keys = Port_ParseKeys(rest)) != 0) {
            it->kind = ITEM_KEYS;
        } else {
            it->kind = ITEM_CMD;
            snprintf(it->text, sizeof(it->text), "%s", rest);
        }
        sCount++;
    }
    fclose(f);
    sActive = true;
    Port_Log("route %s: %d steps", sName, sCount);
    return true;
}

bool Route_Active(void) {
    return sActive;
}

/* called once per frame, in vblank, before input is latched */
void Route_Tick(void) {
    int i;
    sShot = NULL;
    if (!sActive)
        return;
    if (!sStarted) {
        if (Bookmark_Busy())
            return;
        sStarted = true;
        Port_Log("route %s: start", sName);
    }
    sFrame++;
    for (i = 0; i < sCount; i++) {
        RouteItem* it = &sItems[i];
        if (it->kind == ITEM_SHOT && it->end > it->start) {
            /* a shot range: NAME_FRAME.bmp every period frames (every frame without one) */
            static char name[140];
            if (sFrame < it->start || sFrame > it->end || (it->period && (sFrame - it->start) % it->period))
                continue;
            snprintf(name, sizeof(name), "%s_%05d", it->text, (int)sFrame);
            sShot = name;
            continue;
        }
        if ((uint32_t)sFrame != it->start)
            continue;
        switch (it->kind) {
            case ITEM_SHOT:
                sShot = it->text;
                break;
            case ITEM_CMD:
                Debug_QueueCommand(it->text);
                break;
            case ITEM_END:
                Port_Log("route %s: end at route frame %d", sName, (int)sFrame);
                Port_Shutdown();
                exit(0);
            default:
                break;
        }
    }
}

uint16_t Route_Keys(void) {
    uint16_t keys = 0;
    int i;
    if (!sActive || !sStarted)
        return 0;
    for (i = 0; i < sCount; i++) {
        const RouteItem* it = &sItems[i];
        if (it->kind != ITEM_KEYS || sFrame < it->start || sFrame > it->end)
            continue;
        if (it->period == 0 || (sFrame - it->start) % it->period < 4)
            keys |= it->keys;
    }
    return keys;
}

/* the shot to save from the next frame drawn, or NULL */
const char* Route_ShotDue(void) {
    return sShot;
}
