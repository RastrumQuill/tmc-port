# PC port: technical notes

How the port works inside. For playing, building and the debug tools, see
[README.md](README.md).

## Running the decompiled code natively

* The decompiled C is compiled for 32-bit x86, so pointers have the same size
  as on the GBA (the data tables store 4-byte pointers).
* The GBA memory regions (EWRAM, IWRAM, I/O registers, palette RAM, VRAM, OAM)
  are mapped at their original addresses (`port/src/memory.c`), and the RAM
  variables get the addresses of the GBA linker script, so the game code
  accesses them unchanged. On Windows the executable is moved above that
  range (image base 0x10000000). If the addresses are already in use when the
  game starts, it restarts itself with them reserved.
* The game's hand-written ARM assembly is replaced by C (`port/src/asm`).
* Video is a software renderer of the GBA's hardware: tiled and affine
  backgrounds, sprites, windows, blending, mosaic and per-line (HBlank DMA)
  effects. Each effect is a shader-like stage in `port/src/shaders` (see its
  [README](port/src/shaders/README.md)); `port/src/ppu.c` runs the pipeline.
* Sound: the m4a engine (sequencer and sample mixer) in C
  (`port/src/m4a_pc.c`), with the GBA's tone and noise channels emulated
  (`port/src/audio.c`).
* Saves use the 8 KB EEPROM format of GBA emulators (`port/src/eeprom_pc.c`).
* The original game sometimes reads through NULL pointers. The GBA returns
  BIOS "open bus" data for those reads, and the port emulates that with a fault
  handler (`port/src/nullguard.c`).
* `pc.mk` compiles game code with `-ftrivial-auto-var-init=zero`, so locals
  that the decompiled code reads before writing get a fixed value instead of
  stack contents. This keeps behaviour identical between builds.

## Game data and the resource pack

The executable contains none of the game's data. All game data is built from
`port/assets/layout.json.gz` as zero-filled sections with the right sizes,
symbols and pointers (`port/tools/gen_asset_skeleton.py`). The layout lists,
for every section, which runs of bytes come from which ROM offsets.
`make pc-layout` generates it from the fully assembled data files
(`port/tools/make_asset_layout.py`, needs the ROM and `make tools`).

On the first start `port/src/resources.c` does the following:

1. Finds the ROM: the `rom` setting, `baserom.gba` in the working directory
   or next to the executable, or (on Windows) a file the player picks.
2. Checks its size and SHA-1.
3. Writes `tmc_data.pak`: one compressed entry per section, named
   `<data file>:<section>`.

Every start then decompresses the pack and copies each entry's ROM runs into
place. The pointer words in between are left as the linker resolved them.

Pack format (all numbers little endian):

| Part | Contents |
|---|---|
| header | `"TMCPAK" 0x1A 0x00`, u32 version (1), u32 layout hash, u32 entry count, u32 directory offset, u32 directory size |
| data | the compressed entries |
| directory | per entry: u32 name length, name, u32 offset, u32 compressed size, u32 size, u32 CRC-32 of the uncompressed data |

Compression is a byte-oriented LZ77 variant. A token byte holds the literal
count (high nibble) and the match length − 4 (low nibble); 15 means extra
length bytes follow. Then come the literals and a 16-bit match offset. The last
sequence has literals only. `port/tools/tmcpak.py` reads and writes the same
format.

The layout hash ties a pack to the build. A pack from another version, or a
damaged one (a CRC mismatch), is recreated from the ROM.

## How the extended view works

* The game logic is unchanged and still runs with its own 240x160 camera.
  The PC view is a larger window placed around it, kept inside the room
  (or centred with borders when the room is smaller).
* The map layers are drawn from the game's pre-rendered tile map of the whole
  room, so real level geometry is shown everywhere.
* Entities are updated and drawn in the extra area too: the game's on-screen
  checks and sprite culling use the larger view.
* The HUD (hearts, buttons, rupees) is moved to the corners of the view.
* Menus, the title screen, the map and other non-gameplay screens use the
  classic 240x160 picture.
* The view is set up at vblank, when the hardware would latch the registers
  for the next frame (`port/src/view.c`).

## Limitations / known issues

* USA version only. The EU debug overlay is not supported.
* Raster (HBlank) effects designed for 160 lines are stretched over the
  larger view.
* The HUD-to-corner placement is heuristic.
* Rooms only load the entities the game expects to be near the camera, so
  some far away objects may appear when they get close, as on the GBA.
* Replacing a pack entry with data of a different size is not supported yet.
