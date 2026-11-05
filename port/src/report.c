/**
 * @file report.c
 * @brief Bug reports and crash reports that say where they happened.
 *
 * One key (F10, or the console command "report") saves a folder
 * reports/<date>_<time>/ with:
 *   report.txt  the text to paste: what happened / expected (filled in by the
 *               player), then where: game state, area, room, position,
 *               settings, version and commit, and a bookmark to get there
 *   screen.bmp  the picture at the moment the key was pressed
 *   state.bin   all game memory (console: loadstate reports/<folder>/state.bin)
 *   log.txt     the last lines the port logged
 * The text is copied to the clipboard and a dialog offers to open a GitHub
 * issue with it filled in, or the folder (to attach the files).
 *
 * The location is in the text itself, not only in the files, so a report is
 * useful even when nothing gets attached. Paths in the text and the log have
 * the user's home folder replaced by "~".
 *
 * Crashes write the same report (without the dialog) unless crash_reports = 0.
 */
#include "port.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "port_version.h"

#include "global.h"
#include "area.h"
#include "entity.h"
#include "game.h"
#include "main.h"
#include "player.h"
#include "room.h"
#include "save.h"

#define ISSUE_URL "https://github.com/RastrumQuill/tmc-port/issues/new"

const char* Port_VersionText(void) {
    return PORT_VERSION " (" PORT_COMMIT ")";
}

/* ---- privacy: hide the user's home folder and name ---- */

static void ReplaceAll(char* s, size_t size, const char* what, const char* with) {
    size_t wl = strlen(what), nl = strlen(with);
    char* p;
    if (wl < 3)
        return;
    while ((p = strstr(s, what)) != NULL) {
        size_t tail = strlen(p + wl) + 1;
        if ((size_t)(p - s) + nl + tail > size)
            return;
        memmove(p + nl, p + wl, tail);
        memcpy(p, with, nl);
    }
}

void Report_Mask(char* s, size_t size) {
    static const char* const homes[] = { "USERPROFILE", "HOME" };
    const char* user;
    int i;
    for (i = 0; i < 2; i++) {
        const char* h = getenv(homes[i]);
        if (h != NULL && strlen(h) >= 3) {
            char alt[512];
            size_t k;
            ReplaceAll(s, size, h, "~");
            /* the same path with the other slash */
            snprintf(alt, sizeof(alt), "%s", h);
            for (k = 0; alt[k]; k++)
                alt[k] = alt[k] == '\\' ? '/' : alt[k] == '/' ? '\\' : alt[k];
            ReplaceAll(s, size, alt, "~");
        }
    }
    user = getenv("USERNAME");
    if (user == NULL)
        user = getenv("USER");
    if (user != NULL && strlen(user) >= 3)
        ReplaceAll(s, size, user, "<user>");
}

/* ---- the report text ---- */

static const char* TaskName(int task) {
    static const char* const names[] = { "title", "file select", "game", "game over", "staff roll", "debug" };
    return task >= 0 && task < 6 ? names[task] : "?";
}

static const char* SubstateName(int s) {
    static const char* const names[] = { "init room", "change room", "update", "change area", "minish portal",
                                         "barrel", "?", "subtask" };
    return s >= 0 && s < 8 ? names[s] : "?";
}

static size_t Append(char* out, size_t size, size_t n, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
static size_t Append(char* out, size_t size, size_t n, const char* fmt, ...) {
    va_list ap;
    int k;
    if (n >= size)
        return n;
    va_start(ap, fmt);
    k = vsnprintf(out + n, size - n, fmt, ap);
    va_end(ap);
    if (k < 0)
        return n;
    return n + (size_t)k < size ? n + (size_t)k : size - 1;
}

/* where the game is, settings and version; also used for crash reports */
size_t Report_Location(char* out, size_t size, bool withBookmark) {
    size_t n = 0;
    char features[160];
    n = Append(out, size, n, "version: %s %s\n", Port_VersionText(),
#ifdef _WIN32
               "windows"
#else
               "linux"
#endif
    );
    n = Append(out, size, n, "frame: %u  mode: %s", (unsigned)Port_FrameCount(), TaskName(gMain.task));
    if (gMain.task == TASK_GAME)
        n = Append(out, size, n, " / %s", gMain.state == GAMETASK_MAIN ? SubstateName(gMain.substate) : "entering");
    n = Append(out, size, n, " (task %d %d %d)\n", gMain.task, gMain.state, gMain.substate);
    if (gMain.task == TASK_GAME) {
        n = Append(out, size, n, "area 0x%02X room 0x%02X  link x %d y %d layer %d facing %d  camera x %d y %d\n",
                   gRoomControls.area, gRoomControls.room, gPlayerEntity.base.x.HALF.HI - gRoomControls.origin_x,
                   gPlayerEntity.base.y.HALF.HI - gRoomControls.origin_y, gPlayerEntity.base.collisionLayer,
                   gPlayerEntity.base.animationState, (s16)gRoomControls.scroll_x - gRoomControls.origin_x,
                   (s16)gRoomControls.scroll_y - gRoomControls.origin_y);
        n = Append(out, size, n, "story: progress %d  hearts %d/%d  rupees %d\n", gSave.global_progress,
                   gSave.stats.health / 8, gSave.stats.maxHealth / 8, gSave.stats.rupees);
    }
    Port_FeatureList(features, sizeof(features), false);
    n = Append(out, size, n,
               "view: %s %dx%d  window %dx%d scale %g%s  hud_corners %d auto_zoom %d  port features off: %s\n",
               gPortConfig.extendedView ? "extended" : "classic", gPortViewWidth, gPortViewHeight,
               gPortConfig.windowWidth, gPortConfig.windowHeight, gPortConfig.scale,
               gPortConfig.fullscreen ? " fullscreen" : "", gPortConfig.hudAnchor, gPortConfig.autoZoom,
               features[0] ? features : "none");
    if (withBookmark) {
        char bm[2600];
        if (Bookmark_Make(bm, sizeof(bm)))
            n = Append(out, size, n, "bookmark (console: goto, with this copied): %s\n", bm);
    }
    return n;
}

/* ---- files ---- */

static void WriteText(const char* dir, const char* name, const char* text) {
    char path[300];
    FILE* f;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (f == NULL)
        return;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static void MakeFolder(char* dir, size_t size, const char* kind) {
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    char stamp[32];
    int i;
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M%S", tm);
    Port_MakeDir("reports");
    for (i = 0; i < 100; i++) {
        snprintf(dir, size, i ? "reports/%s%s_%d" : "reports/%s%s", stamp, kind, i);
        if (Port_MakeDir(dir))
            return;
    }
}

static size_t sLastLen;
static char sText[16384];

/* the report text up to the files list; the caller adds what it wrote */
static void BuildText(const char* header, const char* extra) {
    size_t n = 0;
    n = Append(sText, sizeof(sText), n, "%s", header);
    n += Report_Location(sText + n, sizeof(sText) - n, true);
    if (extra != NULL)
        n = Append(sText, sizeof(sText), n, "%s", extra);
    sLastLen = n;
}

static void WriteFiles(const char* dir, bool state) {
    char path[300];
    char* log = malloc(65536);
    snprintf(path, sizeof(path), "%s/screen.bmp", dir);
    if (!Port_WriteLastFrame(path))
        snprintf(path, sizeof(path), "(no picture: nothing was drawn yet)");
    if (state) {
        snprintf(path, sizeof(path), "%s/state.bin", dir);
        if (!Debug_WriteState(path))
            Port_Log("report: cannot write %s", path);
    }
    if (log != NULL) {
        Port_LogTail(log, 65536);
        Report_Mask(log, 65536);
        WriteText(dir, "log.txt", log);
        free(log);
    }
}

static void UrlEncode(char* out, size_t size, const char* in) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < size; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

void Report_Create(const char* note) {
    char dir[200], header[512];
    static char url[24000];
    char* body;
    size_t n;
    MakeFolder(dir, sizeof(dir), "");
    snprintf(header, sizeof(header),
             "What happened:\n%s\n\nWhat you expected:\n\n\n"
             "---- where (filled in by the game) ----\n",
             note ? note : "");
    BuildText(header, NULL);
    n = sLastLen;
    n = Append(sText, sizeof(sText), n,
               "files (in %s, attach them if you can): screen.bmp = the picture when the key was pressed, "
               "state.bin = all game memory, log.txt = the port's log. Your home folder is replaced by ~ in the "
               "texts; nothing else personal is in them.\n",
               dir);
    Report_Mask(sText, sizeof(sText));
    WriteFiles(dir, gMain.task == TASK_GAME);
    WriteText(dir, "report.txt", sText);
    Port_SetClipboard(sText);
    Port_Log("report saved to %s (text copied to the clipboard)", dir);
    if (gPortConfig.headless)
        return;
    body = malloc(sizeof(url));
    if (body == NULL)
        return;
    UrlEncode(body, sizeof(url) - 200, sText);
    snprintf(url, sizeof(url), ISSUE_URL "?title=%s&body=%s", "Bug%20report", body);
    free(body);
    switch (Port_ReportDialog(dir)) {
        case 1:
            Port_OpenUrl(url);
            Port_OpenFolder(dir);
            break;
        case 2:
            Port_OpenFolder(dir);
            break;
        default:
            break;
    }
}

/* called from the crash handler: no dialog, files only */
void Report_Crash(const char* what) {
    static bool sBusy;
    char dir[200];
    if (sBusy || !gPortConfig.crashReports)
        return;
    sBusy = true;
    MakeFolder(dir, sizeof(dir), "_crash");
    BuildText("The game crashed.\n\nWhat you were doing:\n\n\n---- where (filled in by the game) ----\n", what);
    Append(sText, sizeof(sText), sLastLen,
           "files (in %s): screen.bmp = the last picture, state.bin = all game memory, log.txt = the port's log\n",
           dir);
    Report_Mask(sText, sizeof(sText));
    WriteFiles(dir, true);
    WriteText(dir, "report.txt", sText);
    fprintf(stderr, "tmc: crash report saved to %s\n", dir);
    if (!gPortConfig.headless)
        Port_CrashDialog(dir);
}
