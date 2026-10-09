The Legend of Zelda: The Minish Cap - PC port
=============================================

An unofficial native Windows version of The Minish Cap, built from the
community decompilation, that can show more of the world than the
Game Boy Advance's 240x160 screen.

This download contains NO game data. You need your own copy of the game:

  1. Dump your cartridge (or use a backup you made of it) to a .gba file.
     It must be the USA version, unmodified:
     SHA-1 b4bd50e4131b027c334547b4524e2dbbd4227130
  2. Name it baserom.gba and put it next to tmc_pc.exe.
     (If it is missing, the game asks you to pick the file once and
     remembers it in tmc_pc.ini.)
  3. Run tmc_pc.exe.

How much of the world you see
-----------------------------
visible area (in game pixels) = window size / scale

  1280x720 window, scale 3  ->  426x240   (the GBA shows 240x160)
  1280x720 window, scale 2  ->  640x360
  1920x1080 window, scale 2 ->  960x540

Change the zoom in game with + / - or the mouse wheel, resize the window, or
edit tmc_pc.ini (written when you quit). F1 switches to the original
240x160 view.

Controls
--------
  D-pad          arrow keys or WASD
  A / B          X / Z   (or K / J)
  L / R          Q / E   (or U / I)
  Start          Enter
  Select         Backspace or right Shift
  Gamepads work out of the box.

  F1   extended / classic view      F11  fullscreen
  + -  zoom (also the mouse wheel)  Tab  fast forward (hold)

Debug tools
-----------
  `      command console (type help)    F2  info overlay
  F4     all items                      F8  god mode
  F5/F9  quick save / load state        F6/F7  previous / next room (Shift: area)

Console commands: warp AREA ROOM [X Y], room N, area N, next, prev, items,
item ID [0-2], hearts N, heal, rupees N, bombs N, arrows N, shells N, keys N,
god, flag N [0/1], savestate [N], loadstate [N], scale S, view, hud, info, pos

Saves go to tmc.sav (the same format as GBA emulators).

More information and the source code: https://github.com/RastrumQuill/tmc-port
SDL2 (zlib license) is included as SDL2.dll, see README-SDL.txt and SDL2-LICENSE.txt.
