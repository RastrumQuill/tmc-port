/**
 * @file romassets.c
 * @brief Loads the game's data from the player's ROM at startup.
 *
 * The executable contains no game content: the data sections built from the
 * decompilation's data files are zero filled (see port/tools/gen_asset_skeleton.py)
 * and gPortRomTable lists where every run of their content lives in the ROM.
 * The pointers between them are already resolved by the linker and are not
 * part of the table.
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

typedef struct {
    uint8_t* dest;
    uint32_t romOffset;
    uint32_t size;
} RomRun;

extern const RomRun gPortRomTable[];
extern const uint32_t gPortRomTableCount;
extern const uint32_t gPortRomSize;

/* The Legend of Zelda - The Minish Cap (USA) */
static const char sExpectedSha1[] = "b4bd50e4131b027c334547b4524e2dbbd4227130";

/* ---- SHA-1 ---- */

#define ROL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void Sha1Block(uint32_t h[5], const uint8_t* p) {
    uint32_t w[80], a, b, c, d, e, f, k, t;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20)
            f = (b & c) | (~b & d), k = 0x5A827999;
        else if (i < 40)
            f = b ^ c ^ d, k = 0x6ED9EBA1;
        else if (i < 60)
            f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        else
            f = b ^ c ^ d, k = 0xCA62C1D6;
        t = ROL(a, 5) + f + e + k + w[i];
        e = d, d = c, c = ROL(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
}

static void Sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint8_t tail[128];
    size_t full = len & ~(size_t)63, rest = len - full, tailLen;
    uint64_t bits = (uint64_t)len * 8;
    size_t i;
    for (i = 0; i < full; i += 64)
        Sha1Block(h, data + i);
    memset(tail, 0, sizeof(tail));
    memcpy(tail, data + full, rest);
    tail[rest] = 0x80;
    tailLen = rest + 9 <= 64 ? 64 : 128;
    for (i = 0; i < 8; i++)
        tail[tailLen - 1 - i] = (uint8_t)(bits >> (i * 8));
    for (i = 0; i < tailLen; i += 64)
        Sha1Block(h, tail + i);
    for (i = 0; i < 20; i++)
        out[i] = (uint8_t)(h[i / 4] >> (24 - (i % 4) * 8));
}

/* ---- finding the ROM ---- */

static uint8_t* LoadWholeFile(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    uint8_t* buf;
    long len;
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 64 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)len);
    if (buf == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)len;
    return buf;
}

/* directory of the executable, with a trailing separator */
static void ExeDir(char* out, size_t size) {
    out[0] = 0;
#ifdef _WIN32
    {
        char* sep;
        DWORD n = GetModuleFileNameA(NULL, out, (DWORD)size);
        if (n == 0 || n >= size) {
            out[0] = 0;
            return;
        }
        sep = strrchr(out, '\\');
        if (sep != NULL)
            sep[1] = 0;
    }
#else
    {
        char* sep;
        ssize_t n = readlink("/proc/self/exe", out, size - 1);
        if (n <= 0) {
            out[0] = 0;
            return;
        }
        out[n] = 0;
        sep = strrchr(out, '/');
        if (sep != NULL)
            sep[1] = 0;
    }
#endif
}

#ifdef _WIN32
static bool AskForRom(char* path, size_t size) {
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    path[0] = 0;
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "GBA ROM (*.gba)\0*.gba\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = (DWORD)size;
    ofn.lpstrTitle = "Select your The Minish Cap (USA) ROM";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) != 0;
}
#endif

static void MakeWritable(void* p, size_t size) {
#ifdef _WIN32
    DWORD old;
    VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &old);
#else
    uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)p & ~(page - 1);
    uintptr_t end = ((uintptr_t)p + size + page - 1) & ~(page - 1);
    mprotect((void*)start, end - start, PROT_READ | PROT_WRITE | PROT_EXEC);
#endif
}

static const char* CheckRom(const uint8_t* rom, size_t size) {
    uint8_t sha[20];
    char hex[41];
    int i;
    if (size != gPortRomSize)
        return "it is not the USA version of The Minish Cap (wrong size)";
    Sha1(rom, size, sha);
    for (i = 0; i < 20; i++)
        snprintf(hex + i * 2, 3, "%02x", sha[i]);
    if (strcmp(hex, sExpectedSha1) != 0)
        return "it is not an unmodified USA ROM of The Minish Cap (SHA-1 mismatch)";
    return NULL;
}

/**
 * Finds the ROM (the configured path, baserom.gba in the working directory or next
 * to the executable, or - on Windows - a file picked by the player), verifies it and
 * copies the game data into place. Returns the path that was used.
 */
bool Port_LoadRomAssets(char* romPath, size_t romPathSize) {
    char candidates[3][1024];
    uint8_t* rom = NULL;
    size_t size = 0;
    const char* problem = NULL;
    const char* used = NULL;
    int i, n = 0;
    uint32_t r;

    if (romPath[0])
        snprintf(candidates[n++], sizeof(candidates[0]), "%s", romPath);
    snprintf(candidates[n++], sizeof(candidates[0]), "baserom.gba");
    ExeDir(candidates[n], sizeof(candidates[0]));
    strncat(candidates[n], "baserom.gba", sizeof(candidates[0]) - strlen(candidates[n]) - 1);
    n++;

    for (i = 0; i < n && rom == NULL; i++) {
        rom = LoadWholeFile(candidates[i], &size);
        if (rom != NULL) {
            problem = CheckRom(rom, size);
            if (problem != NULL) {
                Port_Log("%s: %s", candidates[i], problem);
                free(rom);
                rom = NULL;
            } else {
                used = candidates[i];
            }
        }
    }
#ifdef _WIN32
    while (rom == NULL && !gPortConfig.headless) {
        static char picked[1024];
        char msg[512];
        if (problem != NULL) {
            snprintf(msg, sizeof(msg),
                     "The selected file can't be used: %s.\n\nPlease select an unmodified ROM of "
                     "The Legend of Zelda: The Minish Cap (USA).",
                     problem);
            MessageBoxA(NULL, msg, "The Minish Cap", MB_OK | MB_ICONWARNING);
        }
        if (!AskForRom(picked, sizeof(picked)))
            break;
        rom = LoadWholeFile(picked, &size);
        if (rom == NULL) {
            problem = "it could not be read";
            continue;
        }
        problem = CheckRom(rom, size);
        if (problem != NULL) {
            free(rom);
            rom = NULL;
        } else {
            used = picked;
        }
    }
#endif
    if (rom == NULL) {
        Port_Fatal("The game data is loaded from your own copy of the game.\n"
                   "Place an unmodified ROM of The Legend of Zelda: The Minish Cap (USA)\n"
                   "named baserom.gba next to the executable (SHA-1 %s).",
                   sExpectedSha1);
        return false;
    }

    for (r = 0; r < gPortRomTableCount; r++) {
        const RomRun* run = &gPortRomTable[r];
        MakeWritable(run->dest, run->size);
        memcpy(run->dest, rom + run->romOffset, run->size);
    }
    free(rom);
    snprintf(romPath, romPathSize, "%s", used);
    Port_Log("loaded the game data from %s", used);
    return true;
}
