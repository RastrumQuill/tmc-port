# Rendering stages ("shaders")

The port draws the GBA picture in software, one scanline at a time, like the
real hardware. Every visual effect is its own file here, written like a
shader:

- a **uniforms** struct with everything the stage reads. `ppu.c` fills it from
  the GBA's video registers once per line.
- a **function** that computes the pixels of one line from those uniforms.

Stages read graphics only through `texture.c` and never touch registers, so
each one can be changed or replaced on its own. `ppu.c` is just the pipeline
that runs them in hardware order. The defaults reproduce the GBA exactly.

## Pipeline

Per scanline:

| Stage | File | What it does |
|---|---|---|
| backgrounds | `bg_text.c` | tiled map layers, UI layer, skies; how they continue outside the classic 240×160 screen; HUD moved to the view corners |
| | `bg_affine.c` | rotated / scaled backgrounds (title screen, Deepwood Shrine's rolling barrel room) |
| sprites | `obj.c` | all sprites; regular ones 1:1, scaled / rotated ones through the sampler in `scaling.c` |
| windows | `window.c` | which layers and effects are visible where (spotlights, circle transitions, text box areas) |
| composition | `compose.c` | which layer is in front per pixel |
| color effects | `color_fx.c` | alpha blending, fade to white / black |
| output | `output.c` | GBA 15-bit color → 24-bit pixel (color correction goes here) |

Shared helpers:

- `texture.c` reads tiles and palettes from video memory, like texture fetches.
- `mosaic.c` handles the mosaic (pixelate) effect.

Per frame:

| Stage | File | What it does |
|---|---|---|
| presentation | `present.c` | the finished frame scaled to the window (nearest neighbour), centred with black bars |

`shader.h` declares every stage and its uniforms.

## Changing how the giant bosses are scaled

The large bosses are drawn from **affine sprites**: 64×64 texel textures
scaled and rotated by a 2×2 matrix. These include Big Green ChuChu, Big Octo
(and its frozen form) and Gyorg. The same mechanism draws Wallmasters, the
Great Fairy, the giant Minish-scale objects (book, mushroom, Link), ice
blocks, lily pads, the portal close-up, vortexes and heart containers. The
hardware takes the *nearest* texel for every screen pixel. That one decision
lives in `scaling.c`:

```c
ObjSampler Scaling_ObjSampler(const ObjAffineSprite* sprite);   /* which model a sprite uses */
static bool ObjSampleNearest(sprite, u, v, screenX, screenY, &color);  /* the default model */
```

A sampler receives:

- `u`, `v`: the texture coordinate **with sub-texel precision** (8.8 fixed
  point, 256 = one texel). The fractional part, `u & 0xFF`, is what a
  filtering model needs.
- `sprite`: which texture it is and how it is drawn. `tex` gives the size,
  tile number, palette and color depth. It also has the OAM slot, the matrix
  `pa pb pc pd`, and the size of the box on screen.
- `screenX`, `screenY`: the view pixel being drawn.

It returns the color, or `false` for transparent.

Ways to use this:

- **Filtering** (bilinear, bicubic, xBR / HQx-style edge interpolation): read
  the neighbouring texels with `Tex_ObjTexelIndex` / `Tex_ObjColor` and blend
  them by the fractional bits. Keep transparent texels (index 0) out of the
  blend, or the outline bleeds.
- **AI upscaling or replacement art**:
  1. Decode the sprite's texture once. `sprite->tex` (tile base, palette)
     identifies it.
  2. Run it through a model, or look up a hand-made high resolution version,
     and cache it by tiles and palette.
  3. Sample the larger image at `u * factor / 256`, `v * factor / 256`.
- **Choosing which sprites**: `Scaling_ObjSampler` sees every scaled sprite
  before it is drawn. It can pick a model by texture size (`64×64` = boss
  bodies), by tile range or palette bank (one boss's graphics), or by how much
  the matrix magnifies. Everything else can keep the nearest neighbour.

Scaled backgrounds work the same way, through `Scaling_BgSampler` and
`BgSampleNearest`.

### Resolution

The stages work in **GBA pixels**: the view is, for example, 640×360 game
pixels, and `present.c` then enlarges each of them to a block of screen
pixels. A sampler can make a scaled boss smoother at GBA-pixel resolution.
Detail finer than one GBA pixel needs one of these:

- an upscaler on the whole picture in `present.c`, or
- rendering the view at a higher internal resolution, so that one GBA pixel
  is several output pixels. This would mean giving the stages a
  subpixel-per-pixel factor; that change isn't made yet.

## Other places to hook in

- `present.c`: whole-frame post-processing (CRT, scanlines, smoothing, frame
  upscalers). The texture filter is chosen in `Present_CreateTexture`.
- `output.c`: color correction for every pixel (GBA LCD look, gamma).
- `color_fx.c`: the blend and fade formulas.

## Checking a change

The game is deterministic. Before and after a change, run the same scripted
session and compare the screenshots:

```sh
tmc_pc --headless --frames 14601 --keys "..." --cmd 13510:"warp 0x60 14" --shot 14450
```

A change in a model should only alter the pixels it is meant to. This
reorganisation was checked that way: 153 screenshots at three view sizes,
covering the title screen, field, town, transitions and the Big Octo fight,
are byte-identical to the renderer before the split.
