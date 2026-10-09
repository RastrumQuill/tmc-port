/**
 * @file resources.c
 * @brief The game data: built once from the player's ROM, then loaded from a resource pack.
 *
 * The executable contains no game content. The data sections built from the
 * decompilation's data files are zero filled (port/tools/gen_asset_skeleton.py);
 * gPortDataEntries describes them as named entries, one per section, with the
 * runs of bytes that come from the ROM. Pointers between the sections are
 * already resolved by the linker and are not part of the data.
 *
 * On the first start the ROM is verified and every entry is extracted into
 * tmc_data.pak, compressed. Later starts only read that pack. Entries are
 * stored by name, so a pack can be inspected and its entries replaced (as long
 * as their size stays the same), which is a first step towards mods.
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
    const char* name; /**< "<data file>:<section>", e.g. "data/sound/sounds:.rodata" */
    uint8_t* dest;    /**< the zero filled section in the executable */
    uint32_t size;
    uint32_t firstRun;
    uint32_t runCount;
} DataEntry;

typedef struct {
    uint32_t offset; /**< inside the entry */
    uint32_t romOffset;
    uint32_t size;
} DataRun;

extern const DataEntry gPortDataEntries[];
extern const DataRun gPortDataRuns[];
extern const uint32_t gPortDataEntryCount;
extern const uint32_t gPortDataLayoutHash;
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

/* ---- CRC-32 ---- */

static uint32_t Crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    if (table[1] == 0) {
        uint32_t k, v;
        int b;
        for (k = 0; k < 256; k++) {
            v = k;
            for (b = 0; b < 8; b++)
                v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[k] = v;
        }
    }
    for (i = 0; i < n; i++)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---- LZ compression ----
 * A byte oriented LZ77 format: a token byte holds the literal count (high
 * nibble) and the match length - 4 (low nibble), 15 means more length bytes
 * follow (each 255 adds 255, the first smaller byte ends it); then the
 * literals, then a 16-bit little endian match offset. The last sequence has
 * literals only.
 */

#define LZ_HASH_BITS 16
#define LZ_MIN_MATCH 4
#define LZ_TAIL 8 /* the last bytes are always literals */

static size_t LzBound(size_t n) {
    return n + n / 255 + 16;
}

static uint8_t* LzPutLength(uint8_t* o, size_t len) {
    while (len >= 255) {
        *o++ = 255;
        len -= 255;
    }
    *o++ = (uint8_t)len;
    return o;
}

static uint8_t* LzSequence(uint8_t* o, const uint8_t* lit, size_t litLen, size_t offset, size_t matchLen) {
    uint8_t* token = o++;
    size_t m = matchLen ? matchLen - LZ_MIN_MATCH : 0;
    *token = (uint8_t)(((litLen >= 15 ? 15 : litLen) << 4) | (m >= 15 ? 15 : m));
    if (litLen >= 15)
        o = LzPutLength(o, litLen - 15);
    memcpy(o, lit, litLen);
    o += litLen;
    if (matchLen) {
        *o++ = (uint8_t)offset;
        *o++ = (uint8_t)(offset >> 8);
        if (m >= 15)
            o = LzPutLength(o, m - 15);
    }
    return o;
}

static uint32_t LzHash(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return (v * 2654435761u) >> (32 - LZ_HASH_BITS);
}

/* dst must hold LzBound(n) bytes; returns the compressed size */
static size_t LzCompress(const uint8_t* src, size_t n, uint8_t* dst, int32_t* table) {
    size_t ip = 0, anchor = 0;
    uint8_t* o = dst;
    size_t i;
    for (i = 0; i < ((size_t)1 << LZ_HASH_BITS); i++)
        table[i] = -1;
    while (n > LZ_TAIL + LZ_MIN_MATCH && ip + LZ_TAIL + LZ_MIN_MATCH <= n) {
        uint32_t h = LzHash(src + ip);
        int32_t ref = table[h];
        table[h] = (int32_t)ip;
        if (ref >= 0 && ip - (size_t)ref <= 0xFFFF && memcmp(src + ref, src + ip, LZ_MIN_MATCH) == 0) {
            size_t len = LZ_MIN_MATCH;
            while (ip + len < n - LZ_TAIL && src[ref + len] == src[ip + len])
                len++;
            o = LzSequence(o, src + anchor, ip - anchor, ip - (size_t)ref, len);
            ip += len;
            anchor = ip;
        } else {
            ip++;
        }
    }
    o = LzSequence(o, src + anchor, n - anchor, 0, 0);
    return (size_t)(o - dst);
}

static bool LzGetLength(const uint8_t** p, const uint8_t* end, size_t* len) {
    uint8_t b;
    do {
        if (*p >= end)
            return false;
        b = *(*p)++;
        *len += b;
    } while (b == 255);
    return true;
}

static bool LzDecompress(const uint8_t* src, size_t n, uint8_t* dst, size_t outSize) {
    const uint8_t* end = src + n;
    size_t op = 0;
    while (src < end) {
        uint8_t token = *src++;
        size_t lit = token >> 4, match = token & 15, offset;
        if (lit == 15 && !LzGetLength(&src, end, &lit))
            return false;
        if (lit > (size_t)(end - src) || lit > outSize - op)
            return false;
        memcpy(dst + op, src, lit);
        src += lit;
        op += lit;
        if (src == end)
            break;
        if (end - src < 2)
            return false;
        offset = src[0] | (size_t)src[1] << 8;
        src += 2;
        if (match == 15 && !LzGetLength(&src, end, &match))
            return false;
        match += LZ_MIN_MATCH;
        if (offset == 0 || offset > op || match > outSize - op)
            return false;
        while (match--) {
            dst[op] = dst[op - offset];
            op++;
        }
    }
    return op == outSize;
}

/* ---- the resource pack ----
 * header: "TMCPAK" 0x1A 0x00, u32 version, u32 layout hash, u32 entry count,
 *         u32 directory offset, u32 directory size
 * data:   the compressed entries
 * directory, per entry: u32 name length, name, u32 offset, u32 compressed size,
 *         u32 size, u32 CRC-32 of the uncompressed data
 * All numbers are little endian.
 */

#define PACK_VERSION 1
static const char sPackMagic[8] = { 'T', 'M', 'C', 'P', 'A', 'K', 0x1A, 0 };

static void Put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

static uint32_t Get32(const uint8_t* p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool WritePack(const uint8_t* rom, const char* path) {
    char tmp[1100];
    FILE* f;
    uint8_t header[28];
    uint8_t* raw = NULL;
    uint8_t* packed = NULL;
    uint8_t* dir;
    size_t dirSize = 0, dirCap, pos = sizeof(header);
    int32_t* table = malloc(sizeof(int32_t) << LZ_HASH_BITS);
    uint32_t e, r;
    bool ok = table != NULL;

    dirCap = 64;
    for (e = 0; e < gPortDataEntryCount; e++)
        dirCap += 20 + strlen(gPortDataEntries[e].name);
    dir = malloc(dirCap);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (f == NULL || dir == NULL)
        ok = false;
    if (ok)
        ok = fwrite(header, 1, sizeof(header), f) == sizeof(header);
    for (e = 0; e < gPortDataEntryCount && ok; e++) {
        const DataEntry* entry = &gPortDataEntries[e];
        size_t nameLen = strlen(entry->name), packedSize;
        free(raw);
        free(packed);
        raw = calloc(1, entry->size ? entry->size : 1);
        packed = malloc(LzBound(entry->size));
        if (raw == NULL || packed == NULL) {
            ok = false;
            break;
        }
        for (r = 0; r < entry->runCount; r++) {
            const DataRun* run = &gPortDataRuns[entry->firstRun + r];
            memcpy(raw + run->offset, rom + run->romOffset, run->size);
        }
        packedSize = LzCompress(raw, entry->size, packed, table);
        ok = fwrite(packed, 1, packedSize, f) == packedSize;
        Put32(dir + dirSize, (uint32_t)nameLen);
        memcpy(dir + dirSize + 4, entry->name, nameLen);
        Put32(dir + dirSize + 4 + nameLen, (uint32_t)pos);
        Put32(dir + dirSize + 8 + nameLen, (uint32_t)packedSize);
        Put32(dir + dirSize + 12 + nameLen, entry->size);
        Put32(dir + dirSize + 16 + nameLen, Crc32(raw, entry->size));
        dirSize += 20 + nameLen;
        pos += packedSize;
    }
    if (ok)
        ok = fwrite(dir, 1, dirSize, f) == dirSize;
    if (ok) {
        memcpy(header, sPackMagic, 8);
        Put32(header + 8, PACK_VERSION);
        Put32(header + 12, gPortDataLayoutHash);
        Put32(header + 16, gPortDataEntryCount);
        Put32(header + 20, (uint32_t)pos);
        Put32(header + 24, (uint32_t)dirSize);
        ok = fseek(f, 0, SEEK_SET) == 0 && fwrite(header, 1, sizeof(header), f) == sizeof(header);
    }
    if (f != NULL && fclose(f) != 0)
        ok = false;
    free(raw);
    free(packed);
    free(dir);
    free(table);
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok)
        remove(tmp);
    return ok;
}

/* Copies the pack's content into place; returns NULL or what is wrong with the pack. */
static const char* LoadPack(const char* path) {
    size_t size = 0, dirPos, dirEnd;
    uint8_t* pack = LoadWholeFile(path, &size);
    uint8_t* raw = NULL;
    const char* problem = NULL;
    uint32_t e, r, count;
    if (pack == NULL)
        return "missing";
    if (size < 28 || memcmp(pack, sPackMagic, 8) != 0 || Get32(pack + 8) != PACK_VERSION) {
        free(pack);
        return "not a resource pack of this port";
    }
    if (Get32(pack + 12) != gPortDataLayoutHash) {
        free(pack);
        return "made for another version of the port";
    }
    count = Get32(pack + 16);
    dirPos = Get32(pack + 20);
    dirEnd = dirPos + Get32(pack + 24);
    if (count != gPortDataEntryCount || dirEnd > size || dirPos > dirEnd) {
        free(pack);
        return "damaged (directory)";
    }
    for (e = 0; e < count && problem == NULL; e++) {
        const DataEntry* entry = &gPortDataEntries[e];
        uint32_t nameLen, offset, packedSize, rawSize, crc;
        if (dirEnd - dirPos < 4 || dirEnd - dirPos - 4 < (nameLen = Get32(pack + dirPos)) + 16) {
            problem = "damaged (directory)";
            break;
        }
        if (nameLen != strlen(entry->name) || memcmp(pack + dirPos + 4, entry->name, nameLen) != 0) {
            problem = "made for another version of the port (entries differ)";
            break;
        }
        offset = Get32(pack + dirPos + 4 + nameLen);
        packedSize = Get32(pack + dirPos + 8 + nameLen);
        rawSize = Get32(pack + dirPos + 12 + nameLen);
        crc = Get32(pack + dirPos + 16 + nameLen);
        dirPos += 20 + nameLen;
        if (rawSize != entry->size || offset > size || packedSize > size - offset) {
            problem = "damaged (entry size)";
            break;
        }
        free(raw);
        raw = malloc(rawSize ? rawSize : 1);
        if (raw == NULL || !LzDecompress(pack + offset, packedSize, raw, rawSize) || Crc32(raw, rawSize) != crc) {
            problem = "damaged (entry data)";
            break;
        }
        for (r = 0; r < entry->runCount; r++) {
            const DataRun* run = &gPortDataRuns[entry->firstRun + r];
            MakeWritable(entry->dest + run->offset, run->size);
            memcpy(entry->dest + run->offset, raw + run->offset, run->size);
        }
    }
    free(raw);
    free(pack);
    return problem;
}

/* ---- finding the ROM ---- */

/* Looks for the ROM (the configured path, baserom.gba in the working directory or
 * next to the executable, or - on Windows - a file picked by the player). */
static uint8_t* FindRom(char* romPath, size_t romPathSize) {
    char candidates[3][1024];
    uint8_t* rom = NULL;
    size_t size = 0;
    const char* problem = NULL;
    int i, n = 0;

    if (romPath[0])
        snprintf(candidates[n++], sizeof(candidates[0]), "%s", romPath);
    snprintf(candidates[n++], sizeof(candidates[0]), "baserom.gba");
    ExeDir(candidates[n], sizeof(candidates[0]));
    strncat(candidates[n], "baserom.gba", sizeof(candidates[0]) - strlen(candidates[n]) - 1);
    n++;

    for (i = 0; i < n; i++) {
        rom = LoadWholeFile(candidates[i], &size);
        if (rom == NULL)
            continue;
        problem = CheckRom(rom, size);
        if (problem == NULL) {
            snprintf(romPath, romPathSize, "%s", candidates[i]);
            return rom;
        }
        Port_Log("%s: %s", candidates[i], problem);
        free(rom);
    }
#ifdef _WIN32
    while (!gPortConfig.headless) {
        static char picked[1024];
        char msg[600];
        if (problem != NULL) {
            snprintf(msg, sizeof(msg),
                     "The selected file can't be used: %s.\n\nPlease select an unmodified ROM of "
                     "The Legend of Zelda: The Minish Cap (USA).",
                     problem);
            MessageBoxA(NULL, msg, "The Minish Cap", MB_OK | MB_ICONWARNING);
        } else {
            MessageBoxA(NULL,
                        "The game data is created once from your own copy of the game.\n\n"
                        "Please select an unmodified ROM of The Legend of Zelda: The Minish Cap (USA).",
                        "The Minish Cap", MB_OK | MB_ICONINFORMATION);
        }
        if (!AskForRom(picked, sizeof(picked)))
            break;
        rom = LoadWholeFile(picked, &size);
        if (rom == NULL) {
            problem = "it could not be read";
            continue;
        }
        problem = CheckRom(rom, size);
        if (problem == NULL) {
            snprintf(romPath, romPathSize, "%s", picked);
            return rom;
        }
        free(rom);
    }
#endif
    return NULL;
}

/**
 * Loads the game data from the resource pack. If there is no usable pack (or
 * rebuild is set), creates it from the player's ROM first.
 */
void Port_LoadGameData(const char* packPath, char* romPath, size_t romPathSize, bool rebuild) {
    const char* problem = rebuild ? "rebuild requested" : LoadPack(packPath);
    uint8_t* rom;
    if (problem == NULL) {
        Port_Log("loaded the game data from %s", packPath);
        return;
    }
    if (strcmp(problem, "missing") != 0)
        Port_Log("%s: %s, creating it again from the ROM", packPath, problem);
    rom = FindRom(romPath, romPathSize);
    if (rom == NULL) {
        Port_Fatal("The game data is created once from your own copy of the game.\n"
                   "Place an unmodified ROM of The Legend of Zelda: The Minish Cap (USA)\n"
                   "named baserom.gba next to the executable (SHA-1 %s).",
                   sExpectedSha1);
        return;
    }
    Port_Log("creating %s from %s", packPath, romPath);
    if (!WritePack(rom, packPath)) {
        free(rom);
        Port_Fatal("Could not write the game data file %s.", packPath);
        return;
    }
    free(rom);
    problem = LoadPack(packPath);
    if (problem != NULL)
        Port_Fatal("The game data file %s that was just created can't be read: %s.", packPath, problem);
    Port_Log("loaded the game data from %s", packPath);
}
