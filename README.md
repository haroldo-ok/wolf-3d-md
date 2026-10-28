# Wolfenstein 3D for the Sega Genesis / Mega Drive (shareware Episode 1)

A port of id Software's Wolfenstein 3D (shareware v1.4, Episode 1, 10 levels) to the Sega Genesis, written in C for SGDK (68000).
All assets are converted from the original shareware data files by `tools/convert.py`: wall textures, sprites, maps, HUD, pics, fonts, menus, help/end texts, digitized sounds, AdLib sound effects and IMF music.

ROM: `wolf3d-genesis.bin` (prebuilt, 3.1 MB; a build writes `out/rom.bin`), 32 KB battery SRAM declared in the header.

## Controls
- D-pad: move forward/back, turn left/right
- B: fire    A: run    C: use (door/switch); C + Left/Right strafes
- A + C: next weapon;  X / Y / Z: knife / pistol / machine gun (6-button pad)
- Start: pause menu (New Game, Sound, Control, Load/Save Game, Read This!, View Scores, Back to Game; B also returns to the game)

## Notes
- Rendering: 256x128 view, 128 two-pixel-wide rays, rendered to a 4bpp RAM buffer and DMA'd each frame (about 8.5-10.5 fps on an accurate emulator, depending on the scene; see Performance below). The Genesis has only 64 KB of RAM, so large data lives in ROM or is streamed.
- Audio: Z80 PCM driver for digitized effects (the shareware has 21). AdLib effects are rendered offline with an OPL2 model into PCM. Music is the original IMF data converted into YM2612 (5 FM channels) + PSG tracks.
- Save games: 3 slots in SRAM. High scores are stored there too.
- Not included: demo playback, the registered episodes (not in the shareware data), and the 3-D view border effects of the PC version.

## Performance
Measured headless in Genesis Plus GX (accurate 68000/VDP timing), not yet on real hardware. Every optimisation below is pixel-exact: the rendered view is identical, frame for frame, to the code it replaced.

| Scene | Before | Now |
|---|---|---|
| Start room, fixed sequence of 72 views (`-DVT_TOUR`, "room") | 8.17 fps | 10.51 fps (+29%) |
| Views next to items, lamps, plants and guards, 160 views (`-DVT_TOUR`, "tour") | 6.86 fps | 8.71 fps (+27%) |
| Spinning in place in the start room | 8.01 fps | 10.17 fps (+27%) |

How the frame time is spent and what was done about it:
- Wall columns are drawn by precompiled per-height 68000 scaler routines in ROM (`tools/scalers.py`). A texel shown on several screen rows is loaded once and stored from a register, so rows cost 12-20 cycles instead of 20.
- Ray casting: rays between two rays that hit the same wall face are interpolated rather than cast, the grid walk is specialised per quadrant, wall distance uses 16-bit signed multiplies (`muls.w`/`divu.w` instead of libgcc calls), and the visibility map is stamped with a per-frame value instead of being cleared every frame.
- Ceiling and floor: per tile column the renderer remembers which rows of the RAM buffer, and of each of the two VRAM view buffers, already hold plain ceiling or floor. Those rows are neither refilled nor DMA'd again; in open rooms that removes most of the fill and much of the upload. Anything else that draws into the view area bumps `vid_mode_serial`, which resets this bookkeeping (and the visibility stamps).
- V-int DMA: the column uploads program the VDP DMA registers directly and keep the Z80 off the bus once per batch instead of going through `DMA_doDma` (argument checks, auto-increment, Z80 bus request/release) for each of the ~35 transfers per frame.
- Sprites: each sprite's ray range is trimmed to its non-empty texel columns (the weapon no longer walks ~40 empty columns every frame), the weapon's exact 2x scale has a fast path (one texel load per two rows), short sprite posts finish in asm instead of a C loop, and render_columns sweeps the sprite list left to right instead of testing every sprite in every column.
- Smaller items: the floor/ceiling fill uses `move.l Dn,(An)+` (12 instead of 24 cycles per longword), the static-object visibility scan is a tight asm loop, port 2 is not polled.
- RAM: the 64 KB work RAM is nearly full (about 740 bytes of stack are left unused at the deepest point measured), so the new per-frame tables live in the tail of `screenbuf` that the game mode leaves unused.

### Measuring and checking
The test hooks are compile-time options; a normal build contains none of them.
- `make EXTRA_FLAGS="-Isrc -g1 -DVT_TOUR"`, then `tools/vtbench.py tour out/rom.bin out/release/rom.out` prints the room and tour frame rates. The sequence depends only on the frame number (actors are frozen), so builds are compared on exactly the same work.
- `-DVT_SEQ` renders the same 240 views and stores a checksum of the displayed VRAM after each frame. `tools/vtbench.py cmp ROM_A ELF_A ROM_B ELF_B` compares two such builds frame by frame.
- `-DSTACKPAINT` fills the free stack with a marker at boot, so a RAM dump shows the deepest stack use.
- `tools/harness` drives the Genesis Plus GX libretro core headless (scripted pad input, screenshots, RAM dumps); `tools/fps.sh`, `tools/run.sh` and `tools/prof.sh` use it.

## Building
Needs SGDK 2.x and an m68k GCC toolchain, plus the shareware data files (`VSWAP.WL1`, `MAPHEAD.WL1`, `GAMEMAPS.WL1`, `AUDIOT.WL1`, `AUDIOHED.WL1`, `VGAGRAPH.WL1`, `VGAHEAD.WL1`, `VGADICT.WL1`) from https://archive.org/details/wolf3dsw .
`make data` runs `tools/convert.py`; then `make EXTRA_FLAGS="-Isrc -g1"`.

## Credits and licence
Wolfenstein 3D is (c) id Software. The game data is the shareware release, whose licence permits limited, non-commercial redistribution as-is; the original game source (github.com/id-Software/wolf3d) is GPL. Made as a fan port with reference to haroldo-ok's Catacomb Abyss Mega Drive port. This is an unofficial port.
