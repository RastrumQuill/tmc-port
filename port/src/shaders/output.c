/**
 * @file output.c
 * @brief Output encoding: GBA colors to the PC frame buffer.
 *
 * GBA colors have 5 bits per channel (BGR555). They are expanded to 8 bits by
 * repeating the top bits, so 31 becomes 255 and 0 stays 0. A color correction
 * (the GBA's dark, washed out LCD, gamma, color blindness filters) belongs
 * here; it applies to every pixel of the game picture.
 */
#include "shader.h"

uint32_t Output_Pixel(uint16_t color) {
    uint32_t r = color & 0x1F, g = (color >> 5) & 0x1F, b = (color >> 10) & 0x1F;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}

uint32_t Output_ForcedBlankPixel(void) {
    /* the GBA shows white while the display is blanked */
    return 0xFFFFFF;
}
