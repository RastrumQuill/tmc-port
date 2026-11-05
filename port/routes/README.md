# Routes

A route replays one issue from a fixed spot, so it can be checked again
before each release. Each `.route` file holds a bookmark (where to start,
with the whole save: story flags, items), timed input, screenshots and
console commands.

```
tmc_pc --route port/routes/rays.route --headless
```

## Format

One item per line. `#` starts a comment.

| Line | Meaning |
|---|---|
| `bookmark TMC1:...` | where to start (or a file, e.g. `bookmarks/NAME.txt`) |
| `0-40 RIGHT+B` | hold keys on route frames 0 to 40 |
| `60 A` | press keys on one frame |
| `100-400/30 A` | tap a key every 30 frames (down for 4 frames) |
| `80 shot NAME` | save `NAME.bmp` |
| `100-200/10 shot NAME` | save `NAME_00100.bmp`, `NAME_00110.bmp`, ... |
| `90 COMMAND` | any console command (`pos`, `flag`, `fills`, `report`, ...) |
| `200 end` | quit |
| `#! ARGS` | extra command-line arguments for the runner (window size) |

Keys: `A B L R START SELECT UP DOWN LEFT RIGHT`.

Route frame 0 is the first frame after the bookmark has loaded and the
room has faded in. A bookmark enters the room the way loading a save does,
so the room starts fresh: a cutscene in it plays again from the start.

## Making a route

1. Go to the spot (play there, or `warp`, `flag`, `items` in the console).
2. Type `bookmark NAME` in the console. It saves `bookmarks/NAME.txt`
   and copies the text.
3. Write `NAME.route` with that bookmark and the steps that show the issue.
4. Run it and check the shots show the issue (on the build that has it) or
   look right (on the build that fixed it).

A bug report (F10) already contains the bookmark of the spot where it was
made.

## Running all routes

```
port/tools/routes.sh EXE OUT [BASELINE]
```

Runs every route here with EXE, shots in `OUT/<route>/`. With BASELINE (an
`OUT` folder of an earlier run) it lists the shots that changed. For the
Windows build under Linux: `RUN=wine port/tools/routes.sh tmc_pc.exe out`.
Set `PAK` to the `tmc_data.pak` to use (default: next to EXE).

Before a release: run the routes with the previous release as BASELINE and
look at every changed shot.

## Routes

| Route | Issue |
|---|---|
| `whirlwind` | Lon Lon Ranch whirlwinds: fuchsia rectangle for single frames |
| `rays` | Minish Woods light rays: seams, drawn outside the map, tiled while fading |
| `vaati` | Hyrule Castle garden: seams in Vaati's teleport |
| `barrel` | Deepwood Shrine barrel: walkways misaligned in the extended view |

Known open issues and their bookmarks: [KNOWN_ISSUES.md](../../KNOWN_ISSUES.md).
