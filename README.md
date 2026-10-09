# The Minish Cap — PC port with an extended view

An unofficial native PC (Windows) version of *The Legend of Zelda: The Minish
Cap*, built on the [community decompilation](https://github.com/zeldaret/tmc),
that can show **more of the world than the Game Boy Advance's 240×160 screen**.

**No game data is included**, neither in this repository nor in the
downloads. The game reads its graphics, music, maps and text from your own
copy of the game (a `.gba` ROM) every time it starts.

![South Hyrule Field at 640×360](docs/pc_port/field_640x360.png)

## Contents

- [Playing](#playing)
- [How much of the world you see](#how-much-of-the-world-you-see)
- [Screenshots at different scales](#screenshots-at-different-scales)
- [Controls](#controls)
- [Settings](#settings)
- [Debug tools](#debug-tools)
- [Building from source](#building-from-source)
- [How it works](#how-it-works)
- [Status and known issues](#status-and-known-issues)

## Playing

1. Download `tmc-pc-windows.zip` from the
   [Releases](https://github.com/RastrumQuill/tmc-port/releases) page and unzip it.
2. Put your ROM next to `tmc_pc.exe` and name it `baserom.gba`. It must be the
   **USA** version, unmodified:

   | Version | SHA-1 |
   |---|---|
   | The Legend of Zelda: The Minish Cap (USA) | `b4bd50e4131b027c334547b4524e2dbbd4227130` |

   If the file is missing, the game asks you to pick it once and remembers the
   location in `tmc_pc.ini`.
3. Run `tmc_pc.exe`.

Saves are written to `tmc.sav`, the same 8 KB format GBA emulators use, so
you can bring a save over from an emulator (rename it to `tmc.sav`) or take it
back.

## How much of the world you see

Two numbers decide it:

| Setting | Meaning |
|---|---|
| window size | the size of the window (or screen, in fullscreen), in screen pixels |
| `scale` | how many screen pixels one game pixel takes |

**Visible area (in game pixels) = window size ÷ scale.**

| Window | Scale | Visible area | Compared with the GBA |
|---|---|---|---|
| any | — (F1: classic view) | 240×160 | the original picture |
| 1280×720 | 3 | 426×240 | 1.8× the area |
| 1280×720 | 2 | 640×360 | 6× the area |
| 1920×1080 | 3 | 640×360 | 6× the area |
| 1920×1080 | 2 | 960×540 | 13.5× the area |

Change the zoom while playing with `+`/`-` or the mouse wheel, or resize the
window. The game's own camera still follows Link as on the GBA. The larger view
is centred on that picture and stays inside the current room. When the room is
smaller than the view, it is centred with black bars around it. Menus, the
title screen and the map screen use the classic 240×160 picture.

## Screenshots at different scales

Same spot, same moment; the images are the game's pixels, not upscaled.

**South Hyrule Field**

| Classic 240×160 | 1280×720 at scale 3 → 426×240 |
|---|---|
| ![](docs/pc_port/field_classic_240x160.png) | ![](docs/pc_port/field_426x240.png) |

1280×720 at scale 2 → 640×360

![](docs/pc_port/field_640x360.png)

1920×1080 at scale 2 → 960×540

![](docs/pc_port/field_960x540.png)

**Hyrule Town.** The town is narrower than the larger views, so it is centred
with borders:

| Classic 240×160 | 426×240 | 640×360 |
|---|---|---|
| ![](docs/pc_port/town_classic_240x160.png) | ![](docs/pc_port/town_426x240.png) | ![](docs/pc_port/town_640x360.png) |

960×540

![](docs/pc_port/town_960x540.png)

## Controls

| GBA | Keyboard |
|---|---|
| D-pad | arrow keys or WASD |
| A | X (or K) |
| B | Z (or J) |
| L / R | Q / E (or U / I) |
| Start | Enter |
| Select | Backspace or right Shift |

Gamepads work through SDL's controller support.

| Key | Action |
|---|---|
| F1 | switch between the extended and the classic 240×160 view |
| `+` / `-`, mouse wheel | zoom (changes `scale`) |
| F11 | fullscreen |
| Tab (hold) | fast forward |

## Settings

`tmc_pc.ini` sits next to the executable and is written when you quit:

```ini
window_width = 1280
window_height = 720
scale = 3              ; screen pixels per game pixel - smaller shows more
fullscreen = 0
extended_view = 1      ; 0 = original 240x160 picture
integer_scaling = 0
vsync = 1
audio = 1
hud_corners = 1        ; move hearts, buttons and rupees to the corners of the view
save = tmc.sav
rom = baserom.gba
```

The same settings exist on the command line: `--width W --height H --scale S
--classic --fullscreen --no-audio --save FILE --rom FILE --config FILE`.

## Debug tools

The debug tools are meant for testing and for checking out spots where the
larger view matters.

| Key | Action |
|---|---|
| `` ` `` | open the command console. Type a command and press Enter; Esc closes it |
| F2 | info overlay: frame, view size, area, room, Link's position |
| F4 | give all items (20 hearts, biggest wallet, bomb bag and quiver) |
| F5 / F9 | quick save / load state (slot 0) |
| F6 / F7 | previous / next room in the current area |
| Shift+F6 / Shift+F7 | previous / next area |
| F8 | god mode (health stays full) |

![Info overlay](docs/pc_port/debug_overlay_426x240.png)

Console commands (numbers can be decimal or `0x` hex):

| Command | Effect |
|---|---|
| `warp AREA ROOM [X Y]` | go to a room; the position is relative to the room, the default is its centre |
| `room N` / `area N` | room N of the current area / room 0 of area N |
| `next`, `prev`, `nextarea`, `prevarea` | step through rooms and areas |
| `pos` | show area, room and position |
| `items` | give all items |
| `item ID [0-2]` | set one inventory entry (IDs are the `Item` list in the decomp's `include/item.h`) |
| `hearts N`, `heal`, `rupees N`, `bombs N`, `arrows N`, `shells N`, `keys N` | change stats |
| `god` | toggle god mode |
| `flag N [0/1]` | read, set or clear a story flag (`include/flags.h`) |
| `savestate [N]`, `loadstate [N]` | savestates in `tmc_stateN.bin` |
| `scale S`, `view`, `hud`, `info` | zoom, extended view on/off, HUD corners on/off, overlay on/off |
| `help` | list the commands |

Examples: `warp 2 0` (Hyrule Town, which the game swaps for Festival Town early
in the story), `warp 3 1 504 504` (South Hyrule Field), `items`, `hearts 10`.

Savestates only load in the same version of the port. Warping to a place the
story hasn't reached yet can show missing or odd objects, as with any warp
cheat.

**Testing options.** These are for automated runs:

- `--headless --frames N` runs without a window and stops after N frames.
- `--keys "100-105:START,200-900/30:A"` gives scripted input (frame range,
  optional `/period`, buttons joined with `+`).
- `--shot FRAME` saves a screenshot as `shot_FRAME.bmp`.
- `--cmd FRAME:COMMAND` runs a console command at that frame.
- `--warp AREA,ROOM,X,Y` warps once the game is running.
- `--wav FILE` records the audio.

Environment variables:

- `TMC_HASH_LOG=1` prints a per-frame hash of the game's RAM, to check that two
  builds behave identically.
- `TMC_VIEW_LOG`, `TMC_SOUND_LOG` and `TMC_NULL_LOG` print diagnostics.
- `TMC_NO_NULLGUARD` lets you run under a debugger.

## Building from source

The release builds are made by GitHub Actions (`.github/workflows/build.yml`):
every push builds `tmc-pc-windows.zip` as a downloadable artifact, and pushing
a tag `v*` publishes it as a release. Building needs no ROM.

### Windows build (from Linux or WSL)

```sh
sudo apt install build-essential git python3 curl zip gcc-mingw-w64-i686 binutils-mingw-w64-i686
git clone https://github.com/RastrumQuill/tmc-port
cd tmc-port
make pc-windows        # tmc_pc.exe + SDL2.dll; or `make pc-dist-windows` for the zip
```

On Windows itself, install [WSL](https://learn.microsoft.com/windows/wsl/install)
and run the commands above inside it. Then copy `tmc_pc.exe` and `SDL2.dll` to
a Windows folder together with your `baserom.gba`.

### Linux build (not regularly tested yet)

The game code uses 4-byte pointers in its data tables, so it is built as a
32-bit program:

```sh
sudo dpkg --add-architecture i386
sudo apt install build-essential gcc-multilib python3 libsdl2-dev:i386
make pc
./tmc_pc
```

### Repository layout

This repository is the [decompilation](https://github.com/zeldaret/tmc)
(through [tmc-test](https://github.com/RastrumQuill/tmc-test)) plus the port:

| Path | Contents |
|---|---|
| `src`, `include`, `data`, `asm` | the decompiled game. The few changes the port needs are marked `#ifdef PC` |
| `port/src`, `port/include` | the platform layer: SDL frontend, software GBA video, sound mixer and sequencer, DMA, BIOS functions, save, debug tools |
| `port/src/asm` | C versions of the game's hand-written ARM assembly |
| `port/assets` | the layout of the game data (sizes, symbols, pointers, ROM offsets), generated from a ROM but containing none of its content |
| `port/pc.mk`, `port/tools` | the PC build and its scripts |
| `INSTALL.md` | the decompilation's own build, which makes a matching GBA ROM; it still works |

Maintainers who change the game's data files run `make pc-layout` with
`baserom.gba` in the repository root. This needs the decomp's tools
(`make tools`: `cmake`, a C++ compiler and `libpng`). It regenerates
`port/assets` from fully assembled data.

## How it works

- **Native code.** The decompiled C is compiled for 32-bit x86, so pointers
  have the same size as on the GBA. The GBA's memory regions (RAM, video
  memory, palettes, sprite table, I/O registers) are mapped at their original
  addresses, so the game code runs unchanged. The game's few hand-written ARM
  assembly routines were rewritten in C.
- **Game data from the ROM.** At build time every data file of the game is
  replaced by a zero-filled stand-in with the same size, symbols and internal
  pointers (`port/assets/layout.json.gz`). At startup the executable checks the
  ROM's SHA-1 and copies the content into place: 8,500 byte ranges, 13.6 MB. The
  result is byte-identical to building with the data compiled in; this was
  checked by comparing game RAM frame by frame.
- **Video.** A software renderer emulates the GBA's video hardware: tiled and
  rotated backgrounds, sprites, windows, blending and mosaic. Its output can be
  any size. In the extended view the two map layers are drawn from the game's
  own tile map of the whole room, sprites are drawn wherever they are in the
  view, and the game's "is this on screen" checks use the larger view, so
  enemies and objects in the extra area move and animate.
- **Sound.** The game's sound engine (the GBA "m4a" sequencer and sample
  mixer) runs in C, and the GBA's tone and noise channels are emulated.

## Status and known issues

- Tested: title screen, file select, the intro, Link's house, Hyrule Town,
  South Hyrule Field, room transitions, saving, sound, and the debug tools,
  on Windows (built with MinGW). Wider testing is ongoing.
- USA version only.
- Effects that the game draws line by line for a 160-line screen (some
  wavy or split-screen effects) are stretched over the larger view.
- The HUD (hearts, buttons, rupees) is moved to the corners of the view.
  This uses a heuristic and can be turned off with `hud_corners = 0`.
- The original game sometimes reads through null pointers, which the GBA
  tolerates. The port reproduces the values the GBA would read.
- Rooms only create the objects the game expects near its camera. Very large
  views can show some objects appear when they come within the game's original
  range, as on the GBA.

This project is not affiliated with Nintendo. It contains no game data. Please
use your own legally obtained copy of the game.
