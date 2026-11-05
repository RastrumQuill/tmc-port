# Known issues

Open problems in the PC port, each with a bookmark to see it. To look at
one: open the console (`` ` ``), type `goto ` and paste the bookmark
(Ctrl+V), or start with `tmc_pc --bookmark TMC1:...`. Loading a bookmark
turns off saving until the game is restarted, so your own save is safe.

To report a new one, press **F10** in the game: it saves a report with the
spot, a picture and the game memory, and offers to open a GitHub issue with
the text filled in.

## Open

None known right now.

<!--
Template:

### Short title
What goes wrong, in which view (classic / extended) and window size.
Bookmark: `TMC1:...`
Route: port/routes/NAME.route (once there is one)
Issue: #N
-->

## Fixed, with a route that checks them

These are re-run before each release (`port/tools/routes.sh`, see
[port/routes/README.md](port/routes/README.md)).

| Issue | Route |
|---|---|
| Lon Lon Ranch whirlwinds: fuchsia rectangle for single frames | `whirlwind` |
| Minish Woods light rays: seams, drawn outside the map, tiled while fading | `rays` |
| Hyrule Castle garden: seams in Vaati's teleport | `vaati` |
| Deepwood Shrine barrel: walkways misaligned in the extended view | `barrel` |

## Limits of bookmarks

- A bookmark enters the room the way loading a save does. Anything that
  happened in the room after entering it (a cutscene, a pushed block, a
  boss fight) starts again.
- A bookmark made with another version of the port may not load ("from
  another version") if the save layout changed.
