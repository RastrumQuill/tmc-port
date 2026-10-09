/**
 * @file mosaic.c
 * @brief Mosaic: the blocky "pixelate" effect.
 *
 * The GBA's mosaic repeats the top left pixel of every N x M block. It is
 * applied to the coordinates a layer reads from, before the texture fetch:
 * each stage snaps its x / y with these functions when mosaic is enabled for
 * it (MOSAIC register sizes, per background and per sprite). A size of 1
 * leaves the coordinate unchanged.
 */
#include "shader.h"

int Mosaic_SnapSigned(int x, int size) {
    return x - ((x % size) + size) % size;
}

int Mosaic_Snap(int x, int size) {
    return x - x % size;
}
