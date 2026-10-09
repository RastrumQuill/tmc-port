/**
 * @file map_ptrs.h
 * @brief Per collision layer pointer tables (from asm/src/veneer.s).
 *
 * Layers 0 and 3 alias the bottom map layer.
 */
#ifndef MAP_PTRS_H
#define MAP_PTRS_H

#include "map.h"

/** [layer * 2] = mapData, [layer * 2 + 1] = tileTypes */
extern void* const gMapDataPtrs[8];
#define gTileTypesPtrs (&gMapDataPtrs[1])
extern u8* const gCollisionDataPtrs[4];
/** [layer * 2] = mapDataOriginal, [layer * 2 + 1] = tileTypes */
extern void* const gUnk_08000258[8];
extern u8* const gActTilePtrs[4];

#define LAYER_MAPDATA(layer) ((u16*)gMapDataPtrs[(layer)*2])
#define LAYER_TILETYPES(layer) ((u16*)gMapDataPtrs[(layer)*2 + 1])

#endif
