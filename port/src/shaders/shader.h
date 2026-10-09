/**
 * @file shader.h
 * @brief The GBA picture, split into shader-like stages.
 *
 * The renderer (port/src/ppu.c) emulates the GBA's video hardware one
 * scanline at a time, so that effects which change registers between lines
 * (HBlank DMA: wavy water, heat haze, split screens) keep working. Every
 * visual effect lives in its own file in this directory, written like a
 * shader: a small "uniforms" struct with everything the stage reads (taken
 * from the hardware registers once per line by the pipeline), and a function
 * that computes pixels from it. The stages only read video memory through
 * texture.c and never touch registers themselves.
 *
 * Pipeline per scanline (port/src/ppu.c drives it):
 *
 *   bg_text.c     text (tiled) backgrounds  ->  layer lines BG0-BG3
 *   bg_affine.c   rotated / scaled backgrounds   (sampled through scaling.c)
 *   obj.c         sprites, incl. rotated / scaled ones (sampled through scaling.c)
 *   window.c      which layers and effects are enabled per pixel
 *   compose.c     priority sorting of the layers, then color_fx.c
 *   color_fx.c    alpha blending, fade to white / black
 *   output.c      GBA color (BGR555) -> output pixel
 *
 * and once per frame, on the way to the window:
 *
 *   present.c     scaling of the finished frame to the window (nearest neighbour)
 *
 * mosaic.c holds the mosaic effect, which several stages apply to their
 * coordinates, and texture.c the access to tiles and palettes.
 *
 * Changing the look:
 *   - scaling.c is the scaling model of everything the game draws scaled or
 *     rotated. Affine sprites: the bosses Big Green ChuChu, Big Octo (and its
 *     frozen form) and Gyorg, Wallmasters, the Great Fairy, the giant
 *     Minish-scale objects (book, mushroom, Link), ice blocks, lily pads, the
 *     portal close-up, vortexes, heart containers. Affine backgrounds: the
 *     title screen and the rolling barrel room in Deepwood Shrine.
 *     The samplers get the full description of the sprite / background and
 *     sub-texel coordinates, so a filtering, upscaling or AI model can replace
 *     the nearest neighbour lookup there without touching anything else.
 *   - present.c is the place for whole-frame post-processing (CRT, smoothing,
 *     upscalers working on the finished picture).
 *   - color_fx.c holds the GBA's blend formulas; output.c the color conversion.
 *
 * Colors between stages are GBA colors: 15-bit BGR555 with PPU_OPAQUE set for
 * a drawn pixel (bit 15 clear = transparent). Coordinates "classic" are the
 * 240x160 GBA screen; "view" is the (possibly larger) PC view, with the classic
 * screen at (viewOffsetX, viewOffsetY) inside it.
 */
#ifndef SHADER_H
#define SHADER_H

#include <stdbool.h>
#include <stdint.h>

#include "port.h"

/** layer pixel: bits 0-14 BGR555 color, bit 15 set when drawn */
#define PPU_OPAQUE 0x8000u

enum { LAYER_BG0, LAYER_BG1, LAYER_BG2, LAYER_BG3, LAYER_OBJ, LAYER_BD };

/** a sprite pixel after sprite processing */
typedef struct {
    uint16_t color;   /**< with PPU_OPAQUE */
    uint8_t priority; /**< 0-3, 4 = none */
    uint8_t semiTransparent;
} ObjPixel;

/** per frame values shared by all stages */
typedef struct {
    int viewW, viewH;         /**< size of the output picture */
    int viewOffsetX;          /**< position of the classic 240x160 screen in the view */
    int viewOffsetY;
} FrameUniforms;

/* ------------------------------------------------------------------ texture.c */

/** pixel of a 8x8 background tile (map entry with flip bits and palette), as a layer pixel */
uint16_t Tex_BgTilePixel(uint16_t entry, int px, int py, uint32_t charBase, bool bpp8);
/** map entry of a text background at background pixel (x, y), wrapping like the hardware */
uint16_t Tex_TextMapEntry(uint32_t screenBase, int size, int x, int y);
/** tile number of an affine background map cell */
uint8_t Tex_AffineMapTile(uint32_t screenBase, int tilesPerRow, int tx, int ty);
/** palette index of an affine background (always 256 colors) tile pixel */
uint8_t Tex_AffineTileIndex(uint32_t charBase, uint8_t tile, int px, int py);
/** a 256 color background palette entry as a layer pixel (index 0 = transparent) */
uint16_t Tex_BgPalette256(uint8_t index);
/** the backdrop color */
uint16_t Tex_Backdrop(void);

/** sprite as a texture: everything needed to read its pixels */
typedef struct {
    int w, h;          /**< size in texels (8..64) */
    uint32_t tileBase; /**< first tile in sprite VRAM */
    bool bpp8;         /**< 256 colors (else 16 colors from palBank) */
    bool map1d;        /**< 1D tile mapping (DISPCNT bit 6) */
    int palBank;
} ObjTexture;

/** palette index of a sprite texel (0 = transparent); tx, ty inside the sprite */
uint8_t Tex_ObjTexelIndex(const ObjTexture* tex, int tx, int ty);
/** BGR555 color of a sprite palette index */
uint16_t Tex_ObjColor(const ObjTexture* tex, uint8_t index);

/* ------------------------------------------------------------------ mosaic.c */

/** start of the mosaic block containing x (also for negative x) */
int Mosaic_SnapSigned(int x, int size);
/** start of the mosaic block containing x, for x >= 0 */
int Mosaic_Snap(int x, int size);

/* ------------------------------------------------------------------ scaling.c */

/**
 * A scaled / rotated sprite, as seen by its sampler. Texture coordinates are
 * in 1/256 texel (8.8 fixed point); (0, 0) is the top left corner of the
 * sprite's top left texel.
 */
typedef struct {
    ObjTexture tex;
    int oamIndex;        /**< 0-127 */
    int16_t pa, pb, pc, pd; /**< affine matrix (8.8): texture step per screen pixel */
    bool doubleSize;     /**< drawn in a box twice the texture size */
    int boxW, boxH;      /**< size of the drawn box on screen */
} ObjAffineSprite;

/**
 * Sampler of a scaled sprite: color at texture coordinate (u, v) in 8.8 fixed
 * point. Returns false for a transparent result (outside the texture or a
 * transparent texel). screenX / screenY is the view pixel being drawn.
 */
typedef bool (*ObjSampler)(const ObjAffineSprite* sprite, int32_t u, int32_t v, int screenX, int screenY,
                           uint16_t* color);

/** picks the sampler (the scaling model) for a scaled sprite; called once per sprite and line */
ObjSampler Scaling_ObjSampler(const ObjAffineSprite* sprite);

/** a rotated / scaled background, as seen by its sampler */
typedef struct {
    int bg;              /**< 2 or 3 */
    int size;            /**< 128..1024 pixels, square */
    bool wrap;           /**< repeat outside (else transparent) */
    uint32_t charBase;
    uint32_t screenBase;
    int16_t pa, pc;      /**< texture step per screen pixel along the line (8.8) */
} BgAffineLayer;

/** sampler of an affine background: layer pixel at texture coordinate (u, v) in 8.8 */
typedef uint16_t (*BgSampler)(const BgAffineLayer* layer, int32_t u, int32_t v, int screenX, int screenY);

/** picks the sampler (the scaling model) for an affine background line */
BgSampler Scaling_BgSampler(const BgAffineLayer* layer);

/* ------------------------------------------------------------------ bg_text.c */

typedef enum {
    BG_TEXT_CLASSIC_ONLY, /**< only inside the classic screen */
    BG_TEXT_WRAP,         /**< the 256/512 pixel map repeats over the whole view */
    BG_TEXT_ROOM_MAP,     /**< outside the classic screen: the room's full tile map */
} BgTextExtension;

typedef struct {
    uint32_t charBase, screenBase;
    bool bpp8;
    int size;                /**< BGxCNT screen size 0-3 */
    int hofs, vofs;          /**< scroll */
    int mosaicH, mosaicV;    /**< 1 = off */
    BgTextExtension extension;
    const PpuBgOverride* roomMap; /**< for BG_TEXT_ROOM_MAP, may be disabled */
} BgTextUniforms;

/** one line of a text background; classicLine may be outside 0-159 in the extended view */
void BgText_Line(const FrameUniforms* f, const BgTextUniforms* u, int classicLine, uint16_t* out);

typedef struct {
    uint32_t charBase, screenBase;
    bool bpp8;
    int size;
    int hofs, vofs;
} BgHudUniforms;

/** one line of BG0 with the HUD tiles moved to the corners of the view */
void BgText_HudLine(const FrameUniforms* f, const BgHudUniforms* u, int viewLine, uint16_t* out);

/* ------------------------------------------------------------------ bg_affine.c */

typedef struct {
    BgAffineLayer layer;
    int32_t refX, refY;  /**< texture coordinate (8.8) of classic x = 0 on this line */
    int mosaicH;         /**< 1 = off */
} BgAffineUniforms;

void BgAffine_Line(const FrameUniforms* f, const BgAffineUniforms* u, int viewLine, uint16_t* out);

/* ------------------------------------------------------------------ obj.c */

typedef struct {
    bool enabled;         /**< DISPCNT sprites on */
    bool map1d;
    bool bitmapMode;      /**< modes 3-5: the lower half of sprite VRAM is the bitmap */
    int mosaicH, mosaicV; /**< 1 = off */
    bool hudAnchor;       /**< HUD sprites at the view corners */
} ObjUniforms;

/** all sprites on one line: fills objLine and the object window mask */
void Obj_Line(const FrameUniforms* f, const ObjUniforms* u, int classicLine, ObjPixel* objLine,
              uint8_t* objWindow);

/* ------------------------------------------------------------------ window.c */

typedef struct {
    bool win0, win1, objWin;
    uint16_t win0H, win0V, win1H, win1V; /**< WINxH / WINxV registers */
    uint16_t winIn, winOut;
} WindowUniforms;

/** per pixel enable bits (BG0-3, OBJ, color effect) for one view line */
void Window_Line(const FrameUniforms* f, const WindowUniforms* u, int viewLine, const uint8_t* objWindow,
                 uint8_t* mask);

/* ------------------------------------------------------------------ color_fx.c */

typedef enum { FX_NONE, FX_ALPHA, FX_BRIGHTEN, FX_DARKEN } ColorEffect;

typedef struct {
    ColorEffect effect;
    uint16_t firstTargets;  /**< BLDCNT bits 0-5: layers the effect applies to */
    uint16_t secondTargets; /**< BLDCNT bits 8-13 >> 8: layers that can be blended below */
    int eva, evb;           /**< alpha blend weights 0-16 */
    int evy;                /**< fade strength 0-16 */
} ColorFxUniforms;

uint16_t ColorFx_Blend(uint16_t a, uint16_t b, int eva, int evb);
uint16_t ColorFx_Brighten(uint16_t c, int evy);
uint16_t ColorFx_Darken(uint16_t c, int evy);

/**
 * The color effect for one pixel. top / second are the two front-most visible
 * layer pixels; effectsEnabled is the window's effect bit.
 */
uint16_t ColorFx_Pixel(const ColorFxUniforms* u, uint16_t top, int topLayer, bool topSemiTransparent,
                       uint16_t second, int secondLayer, bool effectsEnabled);

/* ------------------------------------------------------------------ compose.c */

typedef struct {
    bool bgOn[4];
    int bgPriority[4];
    uint16_t backdrop;
    ColorFxUniforms fx;
} ComposeUniforms;

/** layers -> final GBA colors of one view line */
void Compose_Line(const FrameUniforms* f, const ComposeUniforms* u, uint16_t bgLines[4][PORT_MAX_VIEW_WIDTH],
                  const ObjPixel* objLine, const uint8_t* winMask, uint32_t* out);

/* ------------------------------------------------------------------ output.c */

/** GBA BGR555 color -> 0x00RRGGBB output pixel */
uint32_t Output_Pixel(uint16_t color);
/** the color of a line in forced blank */
uint32_t Output_ForcedBlankPixel(void);

/* ------------------------------------------------------------------ present.c */

struct SDL_Renderer;
struct SDL_Texture;

/** the texture the frames are uploaded to (sets its filtering) */
struct SDL_Texture* Present_CreateTexture(struct SDL_Renderer* renderer, int maxW, int maxH);
/** uploads the frame and draws it scaled into the window */
void Present_Frame(struct SDL_Renderer* renderer, struct SDL_Texture* texture, const uint32_t* frame, int pitchPixels,
                   int viewW, int viewH, bool integerScaling);

#endif /* SHADER_H */
