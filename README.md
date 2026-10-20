# Wolfenstein 3D for the Sega Genesis / Mega Drive (shareware Episode 1)

A port of id Software's Wolfenstein 3D (shareware v1.4, Episode 1, 10 levels) to the Sega Genesis, written in C for SGDK (68000).
All assets are converted from the original shareware data files by `tools/convert.py`: wall textures, sprites, maps, HUD, pics, fonts, menus, help/end texts, digitized sounds, AdLib sound effects and IMF music.

ROM: `rom.bin` (3 MB, 32 KB battery SRAM declared in the header).

## Controls
- D-pad: move forward/back, turn left/right
- B: fire    A: run    C: use (door/switch); C + Left/Right strafes
- A + C: next weapon;  X / Y / Z: knife / pistol / machine gun (6-button pad)
- Start: pause menu (New Game, Sound, Control, Load/Save Game, Read This!, View Scores, Back to Game)

## Notes
- Rendering: 256x128 view, 128 two-pixel-wide rays, rendered to a 4bpp RAM buffer and DMA'd each frame (about 8-9 fps on an accurate emulator). The Genesis has only 64 KB of RAM, so large data lives in ROM or is streamed. Speed-ups: wall columns are drawn by precompiled per-height 68000 scaler routines in ROM (`tools/scalers.py`, 20 cycles/row), rays between two rays that hit the same wall face are interpolated rather than cast, the grid walk is specialised per quadrant, wall distance uses 16-bit signed multiplies, and floor/ceiling are only filled where no wall draws.
- Audio: Z80 PCM driver for digitized effects (the shareware has 21). AdLib effects are rendered offline with an OPL2 model into PCM. Music is the original IMF data converted into YM2612 (5 FM channels) + PSG tracks.
- Save games: 3 slots in SRAM. High scores are stored there too.
- Not included: demo playback, the registered episodes (not in the shareware data), and the 3-D view border effects of the PC version.

## Building
Needs SGDK 2.x and an m68k GCC toolchain, plus the shareware data files (`VSWAP.WL1`, `MAPHEAD.WL1`, `GAMEMAPS.WL1`, `AUDIOT.WL1`, `AUDIOHED.WL1`, `VGAGRAPH.WL1`, `VGAHEAD.WL1`, `VGADICT.WL1`) from https://archive.org/details/wolf3dsw .
`make data` runs `tools/convert.py`; then `make EXTRA_FLAGS="-Isrc -g1"`.

## Credits and licence
Wolfenstein 3D is (c) id Software. The game data is the shareware release, whose licence permits limited, non-commercial redistribution as-is; the original game source (github.com/id-Software/wolf3d) is GPL. Made as a fan port with reference to haroldo-ok's Catacomb Abyss Mega Drive port. This is an unofficial port.
