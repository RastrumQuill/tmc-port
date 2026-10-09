/**
 * @file texture.c
 * @brief Texture access: tiles and palettes in GBA video memory.
 *
 * Every pixel the GBA shows comes from 8x8 tiles in VRAM: a background is a
 * map of tile numbers, a sprite is a block of consecutive tiles. Tiles hold
 * palette indices (4 bits with 16-color palettes, 8 bits with 256 colors);
 * index 0 is transparent. These functions are the only place the shader
 * stages read VRAM and palette RAM, like texture fetches in a shader.
 */
#include "shader.h"

#define VRAM8 ((const uint8_t*)(uintptr_t)PORT_VRAM_ADDR)
#define PLTT16 ((const uint16_t*)(uintptr_t)PORT_PLTT_ADDR)

/* sprite tiles start at VRAM + 0x10000 */
#define OBJ_VRAM 0x10000

uint16_t Tex_BgTilePixel(uint16_t entry, int px, int py, uint32_t charBase, bool bpp8) {
    uint32_t tile = entry & 0x3FF;
    if (entry & 0x400)
        px = 7 - px;
    if (entry & 0x800)
        py = 7 - py;
    if (bpp8) {
        uint32_t addr = charBase + tile * 64 + py * 8 + px;
        uint8_t idx;
        if (addr >= 0x10000)
            return 0;
        idx = VRAM8[addr];
        return idx ? (PLTT16[idx] | PPU_OPAQUE) : 0;
    } else {
        uint32_t addr = charBase + tile * 32 + py * 4 + (px >> 1);
        uint8_t idx;
        if (addr >= 0x10000)
            return 0;
        idx = (VRAM8[addr] >> ((px & 1) * 4)) & 0xF;
        return idx ? (PLTT16[((entry >> 12) << 4) | idx] | PPU_OPAQUE) : 0;
    }
}

uint16_t Tex_TextMapEntry(uint32_t screenBase, int size, int x, int y) {
    int w = (size & 1) ? 512 : 256;
    int h = (size & 2) ? 512 : 256;
    int tx, ty, block = 0;
    x &= w - 1;
    y &= h - 1;
    tx = x >> 3;
    ty = y >> 3;
    if (tx >= 32) {
        block += 1;
        tx -= 32;
    }
    if (ty >= 32) {
        block += (size == 3) ? 2 : 1;
        ty -= 32;
    }
    return *(const uint16_t*)(VRAM8 + ((screenBase + block * 0x800 + (ty * 32 + tx) * 2) & 0xFFFF));
}

uint8_t Tex_AffineMapTile(uint32_t screenBase, int tilesPerRow, int tx, int ty) {
    return VRAM8[(screenBase + ty * tilesPerRow + tx) & 0xFFFF];
}

uint8_t Tex_AffineTileIndex(uint32_t charBase, uint8_t tile, int px, int py) {
    return VRAM8[(charBase + tile * 64 + py * 8 + px) & 0xFFFF];
}

uint16_t Tex_BgPalette256(uint8_t index) {
    return index ? (PLTT16[index] | PPU_OPAQUE) : 0;
}

uint16_t Tex_Backdrop(void) {
    return PLTT16[0];
}

uint8_t Tex_ObjTexelIndex(const ObjTexture* tex, int tx, int ty) {
    uint32_t tileNum, addr;
    if (tex->bpp8) {
        if (tex->map1d)
            tileNum = tex->tileBase + ((ty >> 3) * (tex->w >> 3) + (tx >> 3)) * 2;
        else
            tileNum = tex->tileBase + (ty >> 3) * 32 + (tx >> 3) * 2;
        addr = OBJ_VRAM + (tileNum & 0x3FF) * 32 + (ty & 7) * 8 + (tx & 7);
        return VRAM8[addr];
    }
    if (tex->map1d)
        tileNum = tex->tileBase + (ty >> 3) * (tex->w >> 3) + (tx >> 3);
    else
        tileNum = tex->tileBase + (ty >> 3) * 32 + (tx >> 3);
    addr = OBJ_VRAM + (tileNum & 0x3FF) * 32 + (ty & 7) * 4 + ((tx & 7) >> 1);
    return (VRAM8[addr] >> ((tx & 1) * 4)) & 0xF;
}

uint16_t Tex_ObjColor(const ObjTexture* tex, uint8_t index) {
    /* sprite palettes follow the 256 background colors */
    if (tex->bpp8)
        return PLTT16[256 + index];
    return PLTT16[256 + tex->palBank * 16 + index];
}
