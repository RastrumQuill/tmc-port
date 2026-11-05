/**
 * @file main_pc.c
 * @brief Entry point, SDL frontend, configuration, input and frame timing.
 */
#include "port.h"

#include <SDL.h>
#include <setjmp.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "main.h"
#include "game.h"
#include "transitions.h"

extern void DoExitTransition(const Transition* data);

extern void AgbMain(void);
extern void VBlankIntr(void);
extern void HBlankIntr(void);

PortConfig gPortConfig = {
    .windowWidth = 1280,
    .windowHeight = 720,
    .scale = 3.0f,
    .fullscreen = false,
    .extendedView = true,
    .integerScaling = false,
    .vsync = true,
    .audio = true,
    .hudAnchor = true,
    .autoZoom = true,
    .frameSkipLimit = 0,
    .headless = false,
    .crashReports = true,
    .savePath = "tmc.sav",
    .dataPath = "tmc_data.pak",
};

int gPortViewWidth = GBA_WIDTH;
int gPortViewHeight = GBA_HEIGHT;

static SDL_Window* sWindow;
static SDL_Renderer* sRenderer;
static SDL_Texture* sTexture;
static int sTextureW, sTextureH;
/* size of the drawable area of the window in pixels (0 until known) */
static int sOutputW, sOutputH;
static SDL_GameController* sController;
static uint32_t* sFrame;
/* --bench: render every frame headless and report the render time */
static bool sBench;
static double sBenchSeconds;
static uint64_t sBenchFrames;
static jmp_buf sResetJump;
static uint64_t sFrameCount;
static uint64_t sNextFrameTime;
static bool sFastForward;
static uint16_t sKeys;
static uint16_t sKeyboardKeys;
static void ClearKeyboardKeys(void) {
    sKeyboardKeys = 0;
}
static const char* sConfigPath = "tmc_pc.ini";

/* ---- testing helpers: scripted input and screenshots ---- */

#define MAX_SCRIPT 2048
typedef struct {
    uint32_t start, end, period;
    uint16_t keys;
} ScriptedInput;
static ScriptedInput sScript[MAX_SCRIPT];
static int sScriptCount;
static uint32_t sShots[MAX_SCRIPT];
static int sShotCount;

static uint16_t ParseKeyName(const char* name, size_t len) {
    static const struct {
        const char* name;
        uint16_t key;
    } sNames[] = {
        { "A", A_BUTTON },      { "B", B_BUTTON },        { "SELECT", SELECT_BUTTON }, { "START", START_BUTTON },
        { "RIGHT", DPAD_RIGHT }, { "LEFT", DPAD_LEFT },   { "UP", DPAD_UP },           { "DOWN", DPAD_DOWN },
        { "R", R_BUTTON },      { "L", L_BUTTON },
    };
    size_t i;
    for (i = 0; i < sizeof(sNames) / sizeof(sNames[0]); i++) {
        if (strlen(sNames[i].name) == len && SDL_strncasecmp(sNames[i].name, name, len) == 0)
            return sNames[i].key;
    }
    return 0;
}

/* "100-110:START,200-260:RIGHT+B" */
static void ParseScript(const char* spec) {
    const char* p = spec;
    while (*p && sScriptCount < MAX_SCRIPT) {
        ScriptedInput* in = &sScript[sScriptCount];
        char* end;
        in->start = strtoul(p, &end, 10);
        in->end = in->start;
        p = end;
        if (*p == '-') {
            in->end = strtoul(p + 1, &end, 10);
            p = end;
        }
        in->period = 0;
        if (*p == '/') {
            /* repeated tap: press for 4 frames every period frames */
            in->period = strtoul(p + 1, &end, 10);
            p = end;
        }
        in->keys = 0;
        if (*p == ':') {
            p++;
            while (*p && *p != ',') {
                const char* name = p;
                while (*p && *p != ',' && *p != '+')
                    p++;
                in->keys |= ParseKeyName(name, p - name);
                if (*p == '+')
                    p++;
            }
        }
        sScriptCount++;
        if (*p == ',')
            p++;
    }
}

/* "RIGHT+B": the keys, or 0 if any name is unknown */
uint16_t Port_ParseKeys(const char* s) {
    uint16_t keys = 0;
    while (*s) {
        const char* name = s;
        uint16_t k;
        while (*s && *s != '+')
            s++;
        k = ParseKeyName(name, s - name);
        if (k == 0)
            return 0;
        keys |= k;
        if (*s == '+')
            s++;
    }
    return keys;
}

static uint16_t ScriptKeys(uint64_t frame) {
    uint16_t keys = 0;
    int i;
    for (i = 0; i < sScriptCount; i++) {
        if (frame < sScript[i].start || frame > sScript[i].end)
            continue;
        if (sScript[i].period == 0 || (frame - sScript[i].start) % sScript[i].period < 4)
            keys |= sScript[i].keys;
    }
    return keys;
}

static void WriteBmp(const char* path, const uint32_t* pixels, int pitch, int w, int h) {
    FILE* f = fopen(path, "wb");
    int rowSize = (w * 3 + 3) & ~3;
    uint32_t fileSize = 54 + rowSize * h;
    uint8_t header[54] = { 'B', 'M' };
    int x, y;
    uint8_t* row;
    if (f == NULL)
        return;
    header[2] = fileSize;
    header[3] = fileSize >> 8;
    header[4] = fileSize >> 16;
    header[5] = fileSize >> 24;
    header[10] = 54;
    header[14] = 40;
    header[18] = w;
    header[19] = w >> 8;
    header[22] = h;
    header[23] = h >> 8;
    header[26] = 1;
    header[28] = 24;
    fwrite(header, 1, 54, f);
    row = calloc(rowSize, 1);
    for (y = h - 1; y >= 0; y--) {
        for (x = 0; x < w; x++) {
            uint32_t c = pixels[y * pitch + x];
            row[x * 3 + 0] = c;
            row[x * 3 + 1] = c >> 8;
            row[x * 3 + 2] = c >> 16;
        }
        fwrite(row, 1, rowSize, f);
    }
    free(row);
    fclose(f);
}

/* --warp AREA,ROOM,X,Y: warp once gameplay is running (testing aid) */
static bool sWarpPending;
static Transition sWarp;

static void ParseWarp(const char* spec) {
    unsigned a, r, x, y;
    if (sscanf(spec, "%i,%i,%i,%i", &a, &r, &x, &y) != 4) {
        Port_Log("bad --warp '%s' (AREA,ROOM,X,Y)", spec);
        return;
    }
    memset(&sWarp, 0, sizeof(sWarp));
    sWarp.warp_type = 0;
    sWarp.area = a;
    sWarp.room = r;
    sWarp.endX = x;
    sWarp.endY = y;
    sWarp.layer = 1;
    sWarp.facing_direction = 4;
    sWarpPending = true;
}

static void WarpTick(void) {
    char cmd[64];
    if (!sWarpPending || gMain.task != TASK_GAME || gMain.state != GAMETASK_MAIN ||
        gMain.substate != GAMEMAIN_UPDATE)
        return;
    sWarpPending = false;
    /* executed at the next frame boundary, not inside the vblank handler */
    snprintf(cmd, sizeof(cmd), "warp %d %d %d %d", sWarp.area, sWarp.room, sWarp.endX, sWarp.endY);
    Debug_QueueCommand(cmd);
}

/* --cmd FRAME:COMMAND */
#define MAX_CMDS 2048
static struct {
    uint32_t frame;
    char* text;
} sCmds[MAX_CMDS];
static int sCmdCount;

static void ParseCommand(const char* spec) {
    char* end;
    unsigned long frame = strtoul(spec, &end, 10);
    if (*end != ':' || sCmdCount >= MAX_CMDS) {
        Port_Log("bad --cmd '%s' (FRAME:COMMAND)", spec);
        return;
    }
    sCmds[sCmdCount].frame = (uint32_t)frame;
    sCmds[sCmdCount].text = SDL_strdup(end + 1);
    sCmdCount++;
}

static void CommandTick(uint64_t frame) {
    int i;
    for (i = 0; i < sCmdCount; i++) {
        if (sCmds[i].frame == frame)
            Debug_QueueCommand(sCmds[i].text);
    }
}

static bool ShotThisFrame(uint64_t frame) {
    int i;
    for (i = 0; i < sShotCount; i++) {
        if (sShots[i] == frame)
            return true;
    }
    return false;
}

/* ---- logging ---- */

/* the last log lines, for bug reports */
#define LOG_LINES 200
static char sLogRing[LOG_LINES][200];
static int sLogNext;

void Port_Log(const char* fmt, ...) {
    va_list ap;
    char line[200];
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fprintf(stderr, "tmc: %s\n", line);
    snprintf(sLogRing[sLogNext % LOG_LINES], sizeof(sLogRing[0]), "%u: %s", (unsigned)sFrameCount, line);
    sLogNext++;
}

void Port_LogTail(char* out, size_t size) {
    size_t n = 0;
    int i = sLogNext > LOG_LINES ? sLogNext - LOG_LINES : 0;
    out[0] = 0;
    for (; i < sLogNext && n + 2 < size; i++) {
        int k = snprintf(out + n, size - n, "%s\n", sLogRing[i % LOG_LINES]);
        if (k < 0)
            break;
        n += (size_t)k;
    }
}

/* ---- host helpers ---- */

uint64_t Port_FrameCount(void) {
    return sFrameCount;
}

void Port_SetClipboard(const char* text) {
    if (!gPortConfig.headless)
        SDL_SetClipboardText(text);
}

bool Port_GetClipboard(char* out, size_t size) {
    char* t;
    out[0] = 0;
    if (gPortConfig.headless || !SDL_HasClipboardText())
        return false;
    t = SDL_GetClipboardText();
    if (t == NULL)
        return false;
    snprintf(out, size, "%s", t);
    SDL_free(t);
    return out[0] != 0;
}

bool Port_MakeDir(const char* path) {
#ifdef _WIN32
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0755) == 0;
#endif
}

static uint32_t* sCapture;
static int sCaptureW, sCaptureH;
static bool sCaptured, sDrawnOnce;

void Port_CaptureScreen(void) {
    if (!sDrawnOnce)
        return;
    if (sCapture == NULL)
        sCapture = malloc(PORT_MAX_VIEW_WIDTH * PORT_MAX_VIEW_HEIGHT * sizeof(uint32_t));
    if (sCapture == NULL)
        return;
    memcpy(sCapture, sFrame, PORT_MAX_VIEW_WIDTH * gPortViewHeight * sizeof(uint32_t));
    sCaptureW = gPortViewWidth;
    sCaptureH = gPortViewHeight;
    sCaptured = true;
}

bool Port_WriteLastFrame(const char* path) {
    if (sCaptured) {
        sCaptured = false;
        WriteBmp(path, sCapture, PORT_MAX_VIEW_WIDTH, sCaptureW, sCaptureH);
        return true;
    }
    if (!sDrawnOnce)
        return false;
    WriteBmp(path, sFrame, PORT_MAX_VIEW_WIDTH, gPortViewWidth, gPortViewHeight);
    return true;
}

void Port_OpenUrl(const char* url) {
    if (SDL_OpenURL(url) != 0)
        Port_Log("could not open %s: %s", url, SDL_GetError());
}

void Port_OpenFolder(const char* dir) {
    char url[1200];
    char* cwd = SDL_GetBasePath();
#ifdef _WIN32
    char here[1024];
    if (_getcwd(here, sizeof(here)) != NULL) {
        size_t k;
        snprintf(url, sizeof(url), "file:///%s/%s", here, dir);
        for (k = 0; url[k]; k++)
            if (url[k] == '\\')
                url[k] = '/';
    } else
#else
    char here[1024];
    if (getcwd(here, sizeof(here)) != NULL)
        snprintf(url, sizeof(url), "file://%s/%s", here, dir);
    else
#endif
    {
        snprintf(url, sizeof(url), "file://%s/%s", cwd ? cwd : ".", dir);
    }
    SDL_free(cwd);
    Port_OpenUrl(url);
}

int Port_ReportDialog(const char* dir) {
    char text[400];
    int button = 0;
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Open a GitHub issue" },
        { 0, 2, "Open the folder" },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Close" },
    };
    SDL_MessageBoxData box = { SDL_MESSAGEBOX_INFORMATION, NULL, "Bug report", text, 3, buttons, NULL };
    box.window = sWindow;
    snprintf(text, sizeof(text),
             "Saved to %s\n\nThe report text is copied to the clipboard: paste it where you report the bug "
             "(GitHub, Discord, email) and say what happened. Attach the files in the folder if you can.\n\n"
             "GitHub needs an account and opens with the text filled in.",
             dir);
    ClearKeyboardKeys();
    if (SDL_ShowMessageBox(&box, &button) != 0)
        return 0;
    return button;
}

void Port_CrashDialog(const char* dir) {
    char text[300];
    snprintf(text, sizeof(text),
             "The game crashed. A report was saved to %s (the game's folder).\nPlease send it with what you were "
             "doing. Turn this off with crash_reports = 0 in tmc_pc.ini.",
             dir);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "The Minish Cap", text, sWindow);
}

void Port_Fatal(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "tmc: fatal: %s\n", buf);
    if (!gPortConfig.headless)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "The Minish Cap", buf, sWindow);
    exit(1);
}

/* ---- configuration ---- */

static bool ParseBool(const char* v) {
    return !(strcmp(v, "0") == 0 || SDL_strcasecmp(v, "false") == 0 || SDL_strcasecmp(v, "no") == 0 ||
             SDL_strcasecmp(v, "off") == 0);
}

static void SetOption(const char* key, const char* value) {
    if (strcmp(key, "window_width") == 0)
        gPortConfig.windowWidth = atoi(value);
    else if (strcmp(key, "window_height") == 0)
        gPortConfig.windowHeight = atoi(value);
    else if (strcmp(key, "scale") == 0)
        gPortConfig.scale = (float)atof(value);
    else if (strcmp(key, "fullscreen") == 0)
        gPortConfig.fullscreen = ParseBool(value);
    else if (strcmp(key, "extended_view") == 0)
        gPortConfig.extendedView = ParseBool(value);
    else if (strcmp(key, "integer_scaling") == 0)
        gPortConfig.integerScaling = ParseBool(value);
    else if (strcmp(key, "vsync") == 0)
        gPortConfig.vsync = ParseBool(value);
    else if (strcmp(key, "audio") == 0)
        gPortConfig.audio = ParseBool(value);
    else if (strcmp(key, "hud_corners") == 0)
        gPortConfig.hudAnchor = ParseBool(value);
    else if (strcmp(key, "auto_zoom") == 0)
        gPortConfig.autoZoom = ParseBool(value);
    else if (strcmp(key, "crash_reports") == 0)
        gPortConfig.crashReports = ParseBool(value);
    else if (strcmp(key, "save") == 0)
        snprintf(gPortConfig.savePath, sizeof(gPortConfig.savePath), "%s", value);
    else if (strcmp(key, "rom") == 0)
        snprintf(gPortConfig.romPath, sizeof(gPortConfig.romPath), "%s", value);
    else if (strcmp(key, "data") == 0)
        snprintf(gPortConfig.dataPath, sizeof(gPortConfig.dataPath), "%s", value);
    else
        Port_Log("unknown option '%s'", key);
}

static void LoadConfig(const char* path) {
    char line[600];
    FILE* f = fopen(path, "r");
    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f)) {
        char* eq;
        char* key = line;
        char* value;
        char* end;
        while (*key == ' ' || *key == '\t')
            key++;
        if (*key == '#' || *key == ';' || *key == '[' || *key == '\n' || *key == 0)
            continue;
        eq = strchr(key, '=');
        if (eq == NULL)
            continue;
        *eq = 0;
        value = eq + 1;
        for (end = eq - 1; end >= key && (*end == ' ' || *end == '\t'); end--)
            *end = 0;
        while (*value == ' ' || *value == '\t')
            value++;
        for (end = value + strlen(value) - 1; end >= value && (*end == '\n' || *end == '\r' || *end == ' '); end--)
            *end = 0;
        SetOption(key, value);
    }
    fclose(f);
}

static void SaveConfig(const char* path) {
    FILE* f = fopen(path, "w");
    if (f == NULL)
        return;
    fprintf(f, "# The Minish Cap PC port settings\n");
    fprintf(f, "# The visible area is window size / scale (in GBA pixels; the GBA shows 240x160).\n");
    fprintf(f, "window_width = %d\n", gPortConfig.windowWidth);
    fprintf(f, "window_height = %d\n", gPortConfig.windowHeight);
    fprintf(f, "scale = %g\n", gPortConfig.scale);
    fprintf(f, "fullscreen = %d\n", gPortConfig.fullscreen);
    fprintf(f, "# 1 = show more of the world, 0 = classic 240x160 picture\n");
    fprintf(f, "extended_view = %d\n", gPortConfig.extendedView);
    fprintf(f, "integer_scaling = %d\n", gPortConfig.integerScaling);
    fprintf(f, "vsync = %d\n", gPortConfig.vsync);
    fprintf(f, "audio = %d\n", gPortConfig.audio);
    fprintf(f, "# 1 = keep the HUD in the corners of the extended view\n");
    fprintf(f, "hud_corners = %d\n", gPortConfig.hudAnchor);
    fprintf(f, "# 1 = zoom in on rooms smaller than the view, so they fill the window\n");
    fprintf(f, "auto_zoom = %d\n", gPortConfig.autoZoom);
    fprintf(f, "# 1 = save a report folder (reports/) when the game crashes\n");
    fprintf(f, "crash_reports = %d\n", gPortConfig.crashReports);
    fprintf(f, "save = %s\n", gPortConfig.savePath);
    fprintf(f, "data = %s\n", gPortConfig.dataPath);
    if (gPortConfig.romPath[0])
        fprintf(f, "rom = %s\n", gPortConfig.romPath);
    fclose(f);
}

static void Usage(const char* argv0) {
    printf("usage: %s [options]\n"
           "  --width N / --height N   window size in screen pixels\n"
           "  --scale F                screen pixels per GBA pixel (smaller = see more)\n"
           "  --classic                original 240x160 view\n"
           "  --fullscreen\n"
           "  --config FILE            settings file (default tmc_pc.ini)\n"
           "  --save FILE              save file (default tmc.sav, emulator .sav files work)\n"
           "  --rom FILE               your The Minish Cap (USA) ROM, used once to create the game data\n"
           "  --data FILE              game data resource pack (default tmc_data.pak)\n"
           "  --rebuild-data           create the resource pack again from the ROM\n"
           "  --no-audio\n"
           "  --headless --frames N    run without a window for N frames (testing)\n"
           "  --keys SPEC              scripted input, e.g. 100-110:START,200-260:RIGHT+B,300-900/30:A\n"
           "  --shot FRAME             save shot_FRAME.bmp (repeatable)\n"
           "  --bench                  render every frame headless and report the render time\n"
           "  --warp AREA,ROOM,X,Y     warp once in game (testing)\n"
           "  --wav FILE               record the audio (testing)\n"
           "  --cmd FRAME:COMMAND      run a debug console command at a frame (repeatable)\n"
           "  --bookmark TEXT|NAME     start at a bookmark (TMC1:..., or bookmarks/NAME.txt)\n"
           "  --route FILE             play a test route (see port/routes/README.md)\n"
           "In game: F1 toggles the extended view, +/- (or mouse wheel) zoom,\n"
           "F11 fullscreen, Tab fast forward, F10 bug report.\n"
           "Debug: ` console (type help), F2 info, F3 viewers, F4 all items,\n"
           "F5/F9 save/load state, F6/F7 previous/next room (Shift: area), F8 god mode.\n",
           argv0);
}

static void ParseArgs(int argc, char** argv) {
    int i;
    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* next = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--config") == 0 && next) {
            sConfigPath = next;
            i++;
        }
    }
    LoadConfig(sConfigPath);
    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* next = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--width") == 0 && next)
            gPortConfig.windowWidth = atoi(argv[++i]);
        else if (strcmp(a, "--height") == 0 && next)
            gPortConfig.windowHeight = atoi(argv[++i]);
        else if (strcmp(a, "--scale") == 0 && next)
            gPortConfig.scale = (float)atof(argv[++i]);
        else if (strcmp(a, "--classic") == 0)
            gPortConfig.extendedView = false;
        else if (strcmp(a, "--fullscreen") == 0)
            gPortConfig.fullscreen = true;
        else if (strcmp(a, "--no-audio") == 0)
            gPortConfig.audio = false;
        else if (strcmp(a, "--save") == 0 && next)
            snprintf(gPortConfig.savePath, sizeof(gPortConfig.savePath), "%s", argv[++i]);
        else if (strcmp(a, "--rom") == 0 && next)
            snprintf(gPortConfig.romPath, sizeof(gPortConfig.romPath), "%s", argv[++i]);
        else if (strcmp(a, "--data") == 0 && next)
            snprintf(gPortConfig.dataPath, sizeof(gPortConfig.dataPath), "%s", argv[++i]);
        else if (strcmp(a, "--rebuild-data") == 0)
            gPortConfig.rebuildData = true;
        else if (strcmp(a, "--bench") == 0)
            sBench = true;
        else if (strcmp(a, "--headless") == 0)
            gPortConfig.headless = true;
        else if (strcmp(a, "--frames") == 0 && next)
            gPortConfig.frameSkipLimit = atoi(argv[++i]);
        else if (strcmp(a, "--wav") == 0 && next) {
            extern void Audio_StartDump(const char* path);
            Audio_StartDump(argv[++i]);
        } else if (strcmp(a, "--warp") == 0 && next)
            ParseWarp(argv[++i]);
        else if (strcmp(a, "--keys") == 0 && next)
            ParseScript(argv[++i]);
        else if (strcmp(a, "--shot") == 0 && next) {
            if (sShotCount < MAX_SCRIPT)
                sShots[sShotCount++] = strtoul(argv[++i], NULL, 10);
            else
                i++;
        } else if (strcmp(a, "--cmd") == 0 && next) {
            ParseCommand(argv[++i]);
        } else if (strcmp(a, "--bookmark") == 0 && next) {
            char cmd[3000];
            snprintf(cmd, sizeof(cmd), "goto %s", argv[++i]);
            Debug_QueueCommand(cmd);
        } else if (strcmp(a, "--route") == 0 && next) {
            Route_Load(argv[++i]);
        } else if (strcmp(a, "--config") == 0 && next)
            i++;
        else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            Usage(argv[0]);
            exit(0);
        } else {
            Port_Log("unknown argument '%s'", a);
        }
    }
    if (gPortConfig.scale < 0.5f)
        gPortConfig.scale = 0.5f;
    if (gPortConfig.windowWidth < GBA_WIDTH)
        gPortConfig.windowWidth = GBA_WIDTH;
    if (gPortConfig.windowHeight < GBA_HEIGHT)
        gPortConfig.windowHeight = GBA_HEIGHT;
}

/* ---- view size ---- */

/*
 * The view is the window's drawable area divided by the scale: zooming out
 * (smaller scale) shows more of the world, and the picture always fills the
 * window.
 */
void Port_UpdateViewSize(void) {
    int w, h, outW, outH;
    float scale = gPortConfig.scale;
    if (gPortConfig.integerScaling && scale >= 1.0f)
        scale = (float)(int)scale;
    if (!gPortConfig.extendedView || !View_IsExtendedActive()) {
        gPortViewWidth = GBA_WIDTH;
        gPortViewHeight = GBA_HEIGHT;
        return;
    }
    outW = sOutputW > 0 ? sOutputW : gPortConfig.windowWidth;
    outH = sOutputH > 0 ? sOutputH : gPortConfig.windowHeight;
    /*
     * One scale for both axes, so the picture keeps its aspect ratio when it fills
     * the window: zoomed in until a small room fills the window, but never showing
     * less than the GBA's 240x160 nor more than the largest view.
     */
    if (gPortConfig.autoZoom) {
        float fit = View_RoomFitScale(outW, outH);
        if (fit > scale)
            scale = fit;
    }
    if (scale > (float)outW / GBA_WIDTH)
        scale = (float)outW / GBA_WIDTH;
    if (scale > (float)outH / GBA_HEIGHT)
        scale = (float)outH / GBA_HEIGHT;
    if (scale < (float)outW / PORT_MAX_VIEW_WIDTH)
        scale = (float)outW / PORT_MAX_VIEW_WIDTH;
    if (scale < (float)outH / PORT_MAX_VIEW_HEIGHT)
        scale = (float)outH / PORT_MAX_VIEW_HEIGHT;
    w = (int)(outW / scale + 0.5f);
    h = (int)(outH / scale + 0.5f);
    if (w < GBA_WIDTH)
        w = GBA_WIDTH;
    if (h < GBA_HEIGHT)
        h = GBA_HEIGHT;
    if (w > PORT_MAX_VIEW_WIDTH)
        w = PORT_MAX_VIEW_WIDTH;
    if (h > PORT_MAX_VIEW_HEIGHT)
        h = PORT_MAX_VIEW_HEIGHT;
    gPortViewWidth = w;
    gPortViewHeight = h;
}

/* ---- video ---- */

static void InitVideo(void) {
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    char title[160];
    if (gPortConfig.fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    snprintf(title, sizeof(title), "The Legend of Zelda: The Minish Cap - PC port %s", Port_VersionText());
    sWindow = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               gPortConfig.windowWidth, gPortConfig.windowHeight, flags);
    if (sWindow == NULL)
        Port_Fatal("SDL_CreateWindow: %s", SDL_GetError());
    sRenderer = SDL_CreateRenderer(sWindow, -1, SDL_RENDERER_ACCELERATED | (gPortConfig.vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
    if (sRenderer == NULL)
        sRenderer = SDL_CreateRenderer(sWindow, -1, 0);
    if (sRenderer == NULL)
        Port_Fatal("SDL_CreateRenderer: %s", SDL_GetError());
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_GetRendererOutputSize(sRenderer, &sOutputW, &sOutputH);
}

/* the texture has the size of the view, so only visible pixels are uploaded */
static void EnsureTexture(int w, int h) {
    if (sTexture != NULL && sTextureW == w && sTextureH == h)
        return;
    if (sTexture != NULL)
        SDL_DestroyTexture(sTexture);
    sTexture = SDL_CreateTexture(sRenderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (sTexture == NULL)
        Port_Fatal("SDL_CreateTexture: %s", SDL_GetError());
    sTextureW = w;
    sTextureH = h;
}

static void PresentFrame(void) {
    int outW, outH, winW, winH;
    SDL_Rect dst;
    SDL_GetWindowSize(sWindow, &winW, &winH);
    if (!gPortConfig.fullscreen && (winW != gPortConfig.windowWidth || winH != gPortConfig.windowHeight)) {
        gPortConfig.windowWidth = winW;
        gPortConfig.windowHeight = winH;
    }
    SDL_GetRendererOutputSize(sRenderer, &outW, &outH);
    sOutputW = outW;
    sOutputH = outH;
    EnsureTexture(gPortViewWidth, gPortViewHeight);
    SDL_UpdateTexture(sTexture, NULL, sFrame, PORT_MAX_VIEW_WIDTH * 4);
    if (gPortViewWidth == GBA_WIDTH && gPortViewHeight == GBA_HEIGHT) {
        /* classic picture: as large as fits, square pixels, black bars */
        float scale = (float)outW / GBA_WIDTH;
        if ((float)outH / GBA_HEIGHT < scale)
            scale = (float)outH / GBA_HEIGHT;
        if (gPortConfig.integerScaling && scale >= 1.0f)
            scale = (float)(int)scale;
        dst.w = (int)(GBA_WIDTH * scale);
        dst.h = (int)(GBA_HEIGHT * scale);
        dst.x = (outW - dst.w) / 2;
        dst.y = (outH - dst.h) / 2;
    } else {
        /* extended view: the view was sized from the window, fill it */
        dst.x = 0;
        dst.y = 0;
        dst.w = outW;
        dst.h = outH;
    }
    SDL_SetRenderDrawColor(sRenderer, 0, 0, 0, 255);
    SDL_RenderClear(sRenderer);
    SDL_RenderCopy(sRenderer, sTexture, NULL, &dst);
    SDL_RenderPresent(sRenderer);
}

/* ---- input ---- */

static uint16_t sPadKeys;

static uint16_t KeyForScancode(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_X:
        case SDL_SCANCODE_K:
            return A_BUTTON;
        case SDL_SCANCODE_Z:
        case SDL_SCANCODE_J:
            return B_BUTTON;
        case SDL_SCANCODE_BACKSPACE:
        case SDL_SCANCODE_RSHIFT:
            return SELECT_BUTTON;
        case SDL_SCANCODE_RETURN:
            return START_BUTTON;
        case SDL_SCANCODE_RIGHT:
        case SDL_SCANCODE_D:
            return DPAD_RIGHT;
        case SDL_SCANCODE_LEFT:
        case SDL_SCANCODE_A:
            return DPAD_LEFT;
        case SDL_SCANCODE_UP:
        case SDL_SCANCODE_W:
            return DPAD_UP;
        case SDL_SCANCODE_DOWN:
        case SDL_SCANCODE_S:
            return DPAD_DOWN;
        case SDL_SCANCODE_Q:
        case SDL_SCANCODE_U:
            return L_BUTTON;
        case SDL_SCANCODE_E:
        case SDL_SCANCODE_I:
            return R_BUTTON;
        default:
            return 0;
    }
}

static void PollController(void) {
    sPadKeys = 0;
    if (sController == NULL)
        return;
#define BTN(b) SDL_GameControllerGetButton(sController, SDL_CONTROLLER_BUTTON_##b)
    if (BTN(B))
        sPadKeys |= A_BUTTON;
    if (BTN(A))
        sPadKeys |= B_BUTTON;
    if (BTN(BACK))
        sPadKeys |= SELECT_BUTTON;
    if (BTN(START))
        sPadKeys |= START_BUTTON;
    if (BTN(DPAD_RIGHT))
        sPadKeys |= DPAD_RIGHT;
    if (BTN(DPAD_LEFT))
        sPadKeys |= DPAD_LEFT;
    if (BTN(DPAD_UP))
        sPadKeys |= DPAD_UP;
    if (BTN(DPAD_DOWN))
        sPadKeys |= DPAD_DOWN;
    if (BTN(LEFTSHOULDER))
        sPadKeys |= L_BUTTON;
    if (BTN(RIGHTSHOULDER))
        sPadKeys |= R_BUTTON;
#undef BTN
    {
        int ax = SDL_GameControllerGetAxis(sController, SDL_CONTROLLER_AXIS_LEFTX);
        int ay = SDL_GameControllerGetAxis(sController, SDL_CONTROLLER_AXIS_LEFTY);
        if (ax > 16000)
            sPadKeys |= DPAD_RIGHT;
        if (ax < -16000)
            sPadKeys |= DPAD_LEFT;
        if (ay > 16000)
            sPadKeys |= DPAD_DOWN;
        if (ay < -16000)
            sPadKeys |= DPAD_UP;
    }
}

uint16_t Input_GetKeys(void) {
    return sKeys;
}

static void Zoom(float factor) {
    gPortConfig.scale *= factor;
    if (gPortConfig.scale < 1.0f)
        gPortConfig.scale = 1.0f;
    if (gPortConfig.scale > 8.0f)
        gPortConfig.scale = 8.0f;
    Port_Log("scale %.2f", gPortConfig.scale);
}

static void HandleEvents(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_QUIT:
                Port_Shutdown();
                exit(0);
            case SDL_TEXTINPUT:
                if (Debug_ConsoleOpen())
                    Debug_ConsoleText(ev.text.text);
                break;
            case SDL_KEYDOWN: {
                SDL_Scancode sc = ev.key.keysym.scancode;
                if (sc == SDL_SCANCODE_GRAVE && ev.key.repeat == 0) {
                    Debug_SetConsoleOpen(!Debug_ConsoleOpen());
                    if (Debug_ConsoleOpen())
                        SDL_StartTextInput();
                    else
                        SDL_StopTextInput();
                    sKeyboardKeys = 0;
                    break;
                }
                if (Debug_ConsoleOpen()) {
                    if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
                        Debug_ConsoleSubmit();
                        SDL_StopTextInput();
                    } else if (sc == SDL_SCANCODE_ESCAPE) {
                        Debug_SetConsoleOpen(false);
                        SDL_StopTextInput();
                    } else if (sc == SDL_SCANCODE_BACKSPACE) {
                        Debug_ConsoleBackspace();
                    } else if (sc == SDL_SCANCODE_V && (ev.key.keysym.mod & KMOD_CTRL)) {
                        char* clip = SDL_GetClipboardText();
                        if (clip != NULL) {
                            char* c;
                            for (c = clip; *c; c++)
                                if (*c == '\n' || *c == '\r' || *c == '\t')
                                    *c = ' ';
                            Debug_ConsoleText(clip);
                            SDL_free(clip);
                        }
                    }
                    break;
                }
                if (ev.key.repeat == 0 && sc >= SDL_SCANCODE_F2 && sc <= SDL_SCANCODE_F9 &&
                    Debug_HotKey(2 + (sc - SDL_SCANCODE_F2), (ev.key.keysym.mod & KMOD_SHIFT) != 0))
                    break;
                if (ev.key.repeat == 0) {
                    switch (ev.key.keysym.scancode) {
                        case SDL_SCANCODE_F1:
                            gPortConfig.extendedView = !gPortConfig.extendedView;
                            break;
                        case SDL_SCANCODE_F10:
                            /* the picture now, the rest at the next frame boundary */
                            Port_CaptureScreen();
                            Debug_QueueCommand("report");
                            break;
                        case SDL_SCANCODE_F11:
                            gPortConfig.fullscreen = !gPortConfig.fullscreen;
                            SDL_SetWindowFullscreen(sWindow, gPortConfig.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                            break;
                        case SDL_SCANCODE_EQUALS:
                        case SDL_SCANCODE_KP_PLUS:
                            Zoom(1.25f);
                            break;
                        case SDL_SCANCODE_MINUS:
                        case SDL_SCANCODE_KP_MINUS:
                            Zoom(0.8f);
                            break;
                        case SDL_SCANCODE_TAB:
                            sFastForward = true;
                            break;
                        default:
                            break;
                    }
                }
                sKeyboardKeys |= KeyForScancode(ev.key.keysym.scancode);
                break;
            }
            case SDL_KEYUP:
                if (ev.key.keysym.scancode == SDL_SCANCODE_TAB)
                    sFastForward = false;
                sKeyboardKeys &= ~KeyForScancode(ev.key.keysym.scancode);
                break;
            case SDL_MOUSEWHEEL:
                Zoom(ev.wheel.y > 0 ? 1.1f : 1.0f / 1.1f);
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (sController == NULL)
                    sController = SDL_GameControllerOpen(ev.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (sController != NULL &&
                    SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(sController)) == ev.cdevice.which) {
                    SDL_GameControllerClose(sController);
                    sController = NULL;
                }
                break;
        }
    }
    PollController();
    sKeys = sKeyboardKeys | sPadKeys;
    /* the GBA cannot press opposite directions at the same time */
    if ((sKeys & (DPAD_LEFT | DPAD_RIGHT)) == (DPAD_LEFT | DPAD_RIGHT))
        sKeys &= ~(DPAD_LEFT | DPAD_RIGHT);
    if ((sKeys & (DPAD_UP | DPAD_DOWN)) == (DPAD_UP | DPAD_DOWN))
        sKeys &= ~(DPAD_UP | DPAD_DOWN);
}

/* ---- frame loop ---- */

#define GBA_FRAME_NS 16742706ull /* 280896 cycles at 16.78 MHz */

static void WaitForFrameTime(void) {
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t frame = GBA_FRAME_NS * freq / 1000000000ull;
    if (sFastForward) {
        sNextFrameTime = now;
        return;
    }
    if (sNextFrameTime == 0 || now > sNextFrameTime + frame * 4)
        sNextFrameTime = now;
    sNextFrameTime += frame;
    while ((now = SDL_GetPerformanceCounter()) < sNextFrameTime) {
        uint64_t left = (sNextFrameTime - now) * 1000 / freq;
        if (left > 2)
            SDL_Delay((Uint32)(left - 1));
    }
}

static bool IrqEnabled(uint16_t flag) {
    return (PORT_IO16(0x208) & 1) && (PORT_IO16(0x200) & flag);
}

void Port_VBlankIntrWait(void) {
    bool present;
    /* VCOUNT interrupt (line 80): the game mixes audio there */
    if (IrqEnabled(INTR_FLAG_VCOUNT) && (PORT_IO16(0x004) & DISPSTAT_VCOUNT_INTR)) {
        PORT_IO16(0x006) = 80;
        HBlankIntr();
    }
    Audio_Frame();

    /* fast forward only shows every 4th frame; frames nobody sees are not rendered */
    present = !gPortConfig.headless && (!sFastForward || (sFrameCount & 3) == 0);
    if (present || (gPortConfig.headless && (sBench || ShotThisFrame(sFrameCount) || Route_ShotDue()))) {
        /* the frame is displayed with the state latched at the previous vblank */
        uint64_t t0 = sBench ? SDL_GetPerformanceCounter() : 0;
        static int benchW, benchH;
        if (sBench && (benchW != gPortViewWidth || benchH != gPortViewHeight)) {
            /* average over the current view size only */
            benchW = gPortViewWidth;
            benchH = gPortViewHeight;
            sBenchSeconds = 0;
            sBenchFrames = 0;
        }
        Ppu_RenderFrame(sFrame, PORT_MAX_VIEW_WIDTH, gPortViewWidth, gPortViewHeight);
        if (sBench) {
            sBenchSeconds += (double)(SDL_GetPerformanceCounter() - t0) / SDL_GetPerformanceFrequency();
            sBenchFrames++;
        }
        sDrawnOnce = true;
        Debug_DrawOverlay(sFrame, PORT_MAX_VIEW_WIDTH, gPortViewWidth, gPortViewHeight, sFrameCount);
        if (ShotThisFrame(sFrameCount)) {
            char name[64];
            snprintf(name, sizeof(name), "shot_%u.bmp", (unsigned)sFrameCount);
            WriteBmp(name, sFrame, PORT_MAX_VIEW_WIDTH, gPortViewWidth, gPortViewHeight);
        }
        if (Route_ShotDue()) {
            char name[140];
            snprintf(name, sizeof(name), "%s.bmp", Route_ShotDue());
            WriteBmp(name, sFrame, PORT_MAX_VIEW_WIDTH, gPortViewWidth, gPortViewHeight);
        }
    } else {
        /* still run what happens between the lines (HBlank DMA), the game state stays the same */
        Ppu_SkipFrame();
    }
    if (!gPortConfig.headless) {
        if (present)
            PresentFrame();
        HandleEvents();
    }
    PORT_IO16(0x130) = (uint16_t)(~(sKeys | ScriptKeys(sFrameCount) | Route_Keys()) & 0x3FF);

    /* vblank */
    PORT_IO16(0x006) = 160;
    Port_DmaOnVBlank();
    if (IrqEnabled(INTR_FLAG_VBLANK))
        VBlankIntr();
    View_PrepareFrame();
    WarpTick();
    CommandTick(sFrameCount);
    Route_Tick();
    {
        extern void Debug_HashFrame(uint64_t);
        Debug_HashFrame(sFrameCount);
    }
    PORT_IO16(0x006) = 0;

    sFrameCount++;
    if (gPortConfig.frameSkipLimit > 0 && sFrameCount >= (uint64_t)gPortConfig.frameSkipLimit) {
        Port_Log("reached %d frames, exiting", gPortConfig.frameSkipLimit);
        Port_Shutdown();
        exit(0);
    }
    if (!gPortConfig.headless)
        WaitForFrameTime();
}

void Port_SoftReset(void) {
    longjmp(sResetJump, 1);
}

void ram_IntrMain(void) {
    /* interrupts are dispatched by Port_VBlankIntrWait */
}

void Port_Shutdown(void) {
    if (sBench && sBenchFrames)
        Port_Log("bench: %llu frames rendered, %.3f ms per frame (last view %dx%d)", (unsigned long long)sBenchFrames,
                 sBenchSeconds * 1000.0 / sBenchFrames, gPortViewWidth, gPortViewHeight);
    extern void Audio_StopDump(void);
    Audio_StopDump();
    Save_Flush();
    if (!gPortConfig.headless) {
        SaveConfig(sConfigPath);
        Audio_Shutdown();
        SDL_Quit();
    }
}

int main(int argc, char** argv) {
    Port_MapMemory();
    if (getenv("TMC_NO_NULLGUARD") == NULL) /* lets a debugger use SIGTRAP */
        Port_InstallNullGuard();
    Port_FeatureInit();
    ParseArgs(argc, argv);
    Port_LoadGameData(gPortConfig.dataPath, gPortConfig.romPath, sizeof(gPortConfig.romPath), gPortConfig.rebuildData);
    sFrame = calloc(PORT_MAX_VIEW_WIDTH * PORT_MAX_VIEW_HEIGHT, sizeof(uint32_t));
    if (!gPortConfig.headless) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0)
            Port_Fatal("SDL_Init: %s", SDL_GetError());
        InitVideo();
        if (gPortConfig.audio)
            Audio_Init();
    }
    Save_Load();
    PORT_IO16(0x130) = 0x3FF;
    setjmp(sResetJump);
    AgbMain();
    return 0;
}
