/**
 * @file eeprom_pc.c
 * @brief File backed replacement for src/eeprom.c.
 *
 * The cartridge has an 8 KiB EEPROM that is accessed in 64-bit blocks. The
 * save file uses the same byte layout as common emulators (the bit stream of
 * each block, most significant bit first), so an existing .sav file from an
 * emulator can be copied over and used directly.
 */
#include "port.h"

#include <stdio.h>
#include <string.h>

#include "global.h"
#include "gba/eeprom.h"

#define EEPROM_SIZE 0x2000

static u8 sEeprom[EEPROM_SIZE];
static u16 sNumBlocks = EEPROM_SIZE / 8;
static bool sDirty;

u16 EEPROMConfigure(u16 unk_1) {
    if (unk_1 == 0x40) {
        sNumBlocks = 0x400;
        return 0;
    }
    sNumBlocks = 0x40;
    return unk_1 == 4 ? 0 : 1;
}

/* data[3] holds the first 16 bits of the block, data[0] the last. */
u16 EEPROMRead(u16 address, u16* data) {
    const u8* block;
    int i;
    if (address >= sNumBlocks)
        return EEPROM_OUT_OF_RANGE;
    block = &sEeprom[address * 8];
    for (i = 0; i < 4; i++)
        data[3 - i] = (u16)((block[i * 2] << 8) | block[i * 2 + 1]);
    return 0;
}

static u16 EEPROMWrite(u16 address, const u16* data) {
    u8* block;
    int i;
    if (address >= sNumBlocks)
        return EEPROM_OUT_OF_RANGE;
    block = &sEeprom[address * 8];
    for (i = 0; i < 4; i++) {
        block[i * 2] = (u8)(data[3 - i] >> 8);
        block[i * 2 + 1] = (u8)data[3 - i];
    }
    sDirty = true;
    return 0;
}

u16 EEPROMWrite1(u16 address, const u16* data) {
    return EEPROMWrite(address, data);
}

u16 EEPROMCompare(u16 address, const u16* data) {
    u16 buffer[4];
    if (address >= sNumBlocks)
        return EEPROM_OUT_OF_RANGE;
    EEPROMRead(address, buffer);
    return memcmp(buffer, data, sizeof(buffer)) ? EEPROM_COMPARE_FAILED : 0;
}

u16 EEPROMWrite0_8k_Check(u16 address, const u16* data) {
    u16 ret = EEPROMWrite(address, data);
    if (ret == 0)
        ret = EEPROMCompare(address, data);
    /* the game saves in many small writes; flushing every write keeps saves safe on crashes */
    Save_Flush();
    return ret;
}

void Save_Load(void) {
    FILE* f;
    memset(sEeprom, 0xFF, sizeof(sEeprom));
    f = fopen(gPortConfig.savePath, "rb");
    if (f != NULL) {
        size_t n = fread(sEeprom, 1, sizeof(sEeprom), f);
        fclose(f);
        Port_Log("loaded save %s (%u bytes)", gPortConfig.savePath, (unsigned)n);
    }
    sDirty = false;
}

void Save_Flush(void) {
    FILE* f;
    if (!sDirty || gPortConfig.headless)
        return;
    f = fopen(gPortConfig.savePath, "wb");
    if (f == NULL) {
        Port_Log("could not write save %s", gPortConfig.savePath);
        return;
    }
    fwrite(sEeprom, 1, sizeof(sEeprom), f);
    fclose(f);
    sDirty = false;
}
