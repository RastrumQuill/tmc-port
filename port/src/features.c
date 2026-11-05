/**
 * @file features.c
 * @brief Switches for the port's own presentation features.
 *
 * Every feature the port adds to the picture (outside of the game's own
 * 240x160) can be turned off on its own, so a pair of screenshots isolates one
 * of them: TMC_OFF=wrap,clip on the command line environment, or the console
 * command "feature NAME [0|1]" ("features" lists them). None of them changes
 * the game state: a run with any of them off plays out exactly the same.
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* const sNames[FEAT_COUNT] = {
    [FEAT_ROOM_MAP] = "room_map",   [FEAT_WRAP] = "wrap",           [FEAT_BG_HINTS] = "bg_hints",
    [FEAT_ROOM_CLIP] = "clip",      [FEAT_EDGE_FADE] = "edge_fade", [FEAT_TILE_SWAP] = "tile_swap",
    [FEAT_EXTRA_SPRITES] = "extra_sprites",
};

bool gPortFeature[FEAT_COUNT] = { true, true, true, true, true, true, true };

bool Port_FeatureSet(const char* name, int on) {
    int i;
    bool all = strcmp(name, "all") == 0;
    bool found = false;
    for (i = 0; i < FEAT_COUNT; i++) {
        if (all || strcmp(name, sNames[i]) == 0) {
            gPortFeature[i] = on < 0 ? !gPortFeature[i] : on != 0;
            found = true;
        }
    }
    return found;
}

void Port_FeatureInit(void) {
    const char* env = getenv("TMC_OFF");
    char buf[256];
    char* tok;
    if (env == NULL)
        return;
    snprintf(buf, sizeof(buf), "%s", env);
    for (tok = strtok(buf, ", "); tok; tok = strtok(NULL, ", ")) {
        if (!Port_FeatureSet(tok, 0))
            Port_Log("TMC_OFF: unknown feature '%s'", tok);
    }
}

/* the names of the features that are off (all: every feature with its state) */
void Port_FeatureList(char* out, size_t size, bool all) {
    size_t n = 0;
    int i;
    out[0] = 0;
    for (i = 0; i < FEAT_COUNT; i++) {
        int k;
        if (!all && gPortFeature[i])
            continue;
        k = all ? snprintf(out + n, size - n, "%s%s=%d", n ? " " : "", sNames[i], gPortFeature[i])
                : snprintf(out + n, size - n, "%s%s", n ? "," : "", sNames[i]);
        if (k < 0 || (size_t)k >= size - n)
            break;
        n += (size_t)k;
    }
}
