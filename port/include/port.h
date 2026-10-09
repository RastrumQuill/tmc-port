/**
 * @file port.h
 * @brief Interface between the decompiled game code and the PC platform layer.
 *
 * The PC port runs the decompiled C code natively as a 32-bit program. The GBA
 * memory regions (EWRAM, IWRAM, I/O, palette, VRAM, OAM) are mapped at their
 * real GBA addresses, so all of the game's hard-coded addresses keep working.
 * Hardware side effects (DMA, interrupts, the PPU, the BIOS, the EEPROM) are
 * emulated in software by the files in port/src.
 */
#ifndef PORT_H
#define PORT_H

#include <stdint.h>
#include <stdbool.h>

/* ---- GBA memory regions (mapped at their native addresses) ---- */
#define PORT_EWRAM_ADDR 0x02000000u
#define PORT_EWRAM_SIZE 0x40000u
#define PORT_IWRAM_ADDR 0x03000000u
#define PORT_IWRAM_SIZE 0x8000u
#define PORT_IO_ADDR 0x04000000u
#define PORT_IO_SIZE 0x400u
#define PORT_PLTT_ADDR 0x05000000u
#define PORT_PLTT_SIZE 0x400u
#define PORT_VRAM_ADDR 0x06000000u
#define PORT_VRAM_SIZE 0x18000u
#define PORT_OAM_ADDR 0x07000000u
#define PORT_OAM_SIZE 0x400u

#define PORT_IO16(off) (*(volatile uint16_t*)(uintptr_t)(PORT_IO_ADDR + (off)))
#define PORT_IO32(off) (*(volatile uint32_t*)(uintptr_t)(PORT_IO_ADDR + (off)))

/* GBA native resolution. */
#define GBA_WIDTH 240
#define GBA_HEIGHT 160

/* Largest supported logical view (in GBA pixels). */
#define PORT_MAX_VIEW_WIDTH 1024
#define PORT_MAX_VIEW_HEIGHT 640

/**
 * Runtime configuration (tmc_pc.ini / command line).
 *
 * The amount of the world that is visible is controlled by two values:
 *   - the window size (in real screen pixels), and
 *   - the scale (how many screen pixels one GBA pixel occupies).
 * The logical view size is window / scale. With the default 3x scale and a
 * 1280x720 window the game shows 426x240 GBA pixels instead of 240x160.
 */
typedef struct {
    int windowWidth;  /**< window client width in screen pixels */
    int windowHeight; /**< window client height in screen pixels */
    float scale;      /**< screen pixels per GBA pixel (may be fractional) */
    bool fullscreen;
    bool extendedView;    /**< false = classic 240x160 (scaled), true = see more of the world */
    bool integerScaling;  /**< snap scale to whole numbers when the window is resized */
    bool vsync;
    bool audio;
    bool hudAnchor;       /**< move the HUD to the corners of the extended view */
    int frameSkipLimit;   /**< headless/testing: exit after this many frames (0 = never) */
    bool headless;        /**< no window, no audio (smoke tests) */
    char romPath[512];    /**< unused at runtime; assets are baked in at build time */
    char savePath[512];
} PortConfig;

extern PortConfig gPortConfig;

/** Current logical view size in GBA pixels (240x160 unless extended view is active). */
extern int gPortViewWidth;
extern int gPortViewHeight;

/** Recompute gPortViewWidth/Height from the window size, scale and room. */
void Port_UpdateViewSize(void);

/**
 * Visible area used by the game's on-screen checks, relative to the camera
 * (gRoomControls.scroll_x/y). Classic GBA: left=0, top=0, width=240, height=160.
 * With the extended view this grows to cover everything that is drawn.
 */
extern int gPortScreenLeft;
extern int gPortScreenTop;
extern int gPortScreenWidth;
extern int gPortScreenHeight;

/* ---- platform services ---- */
void Port_MapMemory(void);
/** Make NULL-pointer accesses behave like GBA BIOS-area accesses (port/src/nullguard.c). */
void Port_InstallNullGuard(void);
void Port_Init(int argc, char** argv);
void Port_Shutdown(void);
/** Called by the BIOS VBlankIntrWait: renders + presents a frame, polls input, runs IRQs. */
void Port_VBlankIntrWait(void);
/** Soft reset (BIOS SoftReset): restarts AgbMain. */
void Port_SoftReset(void) __attribute__((noreturn));
void Port_Fatal(const char* fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void Port_Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- DMA ---- */
void Port_DmaOnHBlank(int line);
void Port_DmaOnVBlank(void);

/* ---- PPU ---- */
/**
 * Renders a full frame of view w x h GBA pixels into an XRGB8888 buffer.
 * Lines outside 0..159 are rendered with the scanline state of the nearest
 * real line (HBlank effects are stretched).
 */
void Ppu_RenderFrame(uint32_t* out, int pitchPixels, int w, int h);

/**
 * Extended background source.
 *
 * On the GBA a text background reads its tile map from a 32x32 (or 64x64)
 * screen block in VRAM that wraps around. That is enough for 240x160 but not
 * for a larger view, so the PC renderer can instead read a background's tile
 * map from an arbitrarily large buffer in RAM. The game already keeps a
 * pre-rendered tile map of the whole room (gMapDataBottomSpecial /
 * gMapDataTopSpecial, 128x128 tiles), so the map layers use those directly.
 */
typedef enum {
    /** only drawn inside the classic 240x160 area (UI layers, unknown content) */
    PPU_BG_CLASSIC_ONLY,
    /** drawn everywhere; text backgrounds simply wrap around (repeating backgrounds) */
    PPU_BG_WRAP,
    /** classic area from VRAM, everything outside from the override map */
    PPU_BG_OVERRIDE,
} PpuBgMode;

/** How each background is extended beyond the classic area (set by view.c every frame). */
extern PpuBgMode gPpuBgMode[4];

typedef struct {
    bool enabled;
    const uint16_t* map; /**< tile map entries (same format as VRAM text BG entries) */
    int strideTiles;     /**< entries per row in map */
    int widthTiles;      /**< valid width in tiles (outside reads as transparent) */
    int heightTiles;     /**< valid height in tiles */
    int scrollX;         /**< map pixel shown at the left edge of the classic 240x160 area */
    int scrollY;         /**< map pixel shown at the top edge of the classic 240x160 area */
} PpuBgOverride;

extern PpuBgOverride gPpuBgOverride[4];

/** Position of the classic 240x160 area inside the (extended) view, in view pixels. */
extern int gPortViewOffsetX;
extern int gPortViewOffsetY;

/**
 * Extended sprite coordinates.
 *
 * OAM can only hold a 9 bit x and an 8 bit y coordinate, which is not enough
 * for a view larger than 240x160. The sprite builder (port/src/asm/intr.c)
 * records the full coordinates of every OAM entry it writes here; the PPU
 * uses them as long as the OAM entry still matches.
 * Coordinates are relative to the classic 240x160 screen.
 */
typedef struct {
    int16_t x;
    int16_t y;
    uint16_t attr0;
    uint16_t attr1;
    uint8_t valid;
    /** HUD sprite: PORT_ANCHOR_* flags, anchored to the corners of the view */
    uint8_t anchor;
} PortOamExt;

#define PORT_ANCHOR_HUD 0x80
#define PORT_ANCHOR_RIGHT 0x01
#define PORT_ANCHOR_BOTTOM 0x02

/** Set while the game draws its UI elements (sprites get PORT_ANCHOR_HUD). */
extern bool gPortHudSprites;
/** Set by view.c when the HUD should be moved to the corners of the extended view. */
extern bool gPpuHudAnchor;

/** written by the sprite builder (mirrors gOAMControls.oam) */
extern PortOamExt gPortOamExtWork[128];
/** copied from gPortOamExtWork whenever gOAMControls.oam is copied to OAM */
extern PortOamExt gPortOamExtLive[128];
void Port_OnOamCopy(const void* src, void* dest, uint32_t bytes);

/* ---- audio ---- */
void Audio_Init(void);
void Audio_Shutdown(void);
/** Called once per frame after the sound driver ran (PSG update + output). */
void Audio_Frame(void);
/** Hand one frame worth of mixed PCM (signed 8 bit stereo from the m4a mixer) to the audio device. */
void Audio_SubmitFrame(const int8_t* left, const int8_t* right, int samples, int sampleRate);

/* ---- save ---- */
void Save_Load(void);
void Save_Flush(void);

/* ---- input ---- */
uint16_t Input_GetKeys(void); /**< GBA KEYINPUT bits, 1 = pressed */

/* ---- extended view hooks (port/src/view.c) ---- */
/** Called once per frame before rendering to set up BG overrides from game state. */
void View_PrepareFrame(void);
/** True while the game is in a state where showing more than 240x160 is safe. */
bool View_IsExtendedActive(void);

#endif /* PORT_H */
