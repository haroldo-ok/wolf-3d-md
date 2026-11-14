# Wolfenstein 3D for the Sega Genesis / Mega Drive (shareware and full version)

A port of id Software's Wolfenstein 3D (v1.4) to the Sega Genesis, written in C for SGDK (68000). It builds from either the shareware data (`*.WL1`, Episode 1, 10 levels) or the retail data (`*.WL6`, six episodes, 60 levels).
All assets are converted from the original data files by `tools/convert.py`: wall textures, sprites, maps, HUD, pics, fonts, menus, help/end texts, digitized sounds, AdLib sound effects and IMF music.

ROM: `wolf3d-genesis.bin` is the prebuilt shareware ROM (4 MB); a build writes `out/rom.bin`. The full version is about 5 MB (see Full version below). 32 KB battery SRAM is declared in the header.

## Controls
- D-pad: move forward/back, turn left/right
- B: fire    A: run    C: use (door/switch); C + Left/Right strafes
- A + C: next weapon;  X / Y / Z: knife / pistol / machine gun (6-button pad)
- Start: pause menu (New Game, Sound, Control, Detail, Load/Save Game, Read This!, View Scores, Back to Game; B also returns to the game)

## Notes
- Rendering: 256x128 view, 128 two-pixel-wide rays, rendered to a 4bpp RAM buffer and DMA'd each frame (about 12-15 fps on an accurate emulator in high detail and 16-20 in low detail, depending on the scene; see Performance below). The Genesis has only 64 KB of RAM, so large data lives in ROM or is streamed.
- Audio: Z80 PCM driver for digitized effects (the shareware has 21). AdLib effects are rendered offline with an OPL2 model into PCM. Music is the original IMF data converted into YM2612 (5 FM channels) + PSG tracks.
- Save games: 3 slots in SRAM. High scores are stored there too.
- Not included: demo playback and the 3-D view border effects of the PC version.

## Performance
Measured headless in Genesis Plus GX (accurate 68000/VDP timing), not yet on real hardware. Every optimisation below is pixel-exact in high detail: the picture on screen is identical, frame for frame, to the code it replaced.

| Scene | First version | C renderer | Now, Detail: High | Now, Detail: Low |
|---|---|---|---|---|
| Start room, fixed sequence of 72 views (`-DVT_TOUR`, "room") | 8.17 fps | 10.51 fps | 14.7 fps (+39%) | 19.7 fps (+87%) |
| Views next to items, lamps, plants and guards, 160 views (`-DVT_TOUR`, "tour") | 6.86 fps | 8.71 fps | 12.4 fps (+43%) | 16.3 fps (+87%) |

The last two columns were measured with a newer build of the emulator core, on which the C renderer reaches 11.31 / 9.38 fps and the current code 15.77 / 13.39 fps (high) and 21.18 / 17.58 fps (low); they are scaled to the older figures so the columns compare. The benchmark draws every frame; in play, frames in which nothing changed are not drawn at all (see below).

How the frame time is spent and what was done about it:
- Wall columns are drawn by precompiled per-height 68000 scaler routines in ROM (`tools/scalers.py`). A texel shown on several screen rows is loaded once and stored from a register, so rows cost 12-20 cycles instead of 20. This is now the largest single item (about a third of a frame) and close to what one byte per ray and row allows.
- The three inner loops of the renderer are hand-written 68000 (`src/r_asm.s`); GCC's code for them spent most of its time moving values between the stack and registers.
  - Ray casting (`rc_cast`): the grid walk exists once per quadrant with everything in registers, so a ray that is turned away by a half-open door simply carries on. The projected height comes from a table (`rc_heighttab`) instead of a 140-cycle `divu`. Rays between two rays that hit the same wall face are interpolated rather than cast; the interpolation counts its small quotient up instead of using `muls` + `divs`. A ray that hits a sliding push wall is rare and is handed to the original C routine (`cast_ray_c`).
  - Tile columns (`rc_columns`): background fill, the four wall scalers of the column (found through `scaler_tab`, one lookup per ray), the sprite sweep and the upload bookkeeping.
  - Sprites (`sd_draw`): the two ends of every post are looked up in `spr_rowtab` instead of multiplied, the rows are drawn by jumping into an unrolled block (no loop counter), and a sprite at exactly 2x loads each texel once for its two rows.
- The visibility map is stamped with a per-frame value instead of being cleared every frame.
- Ceiling and floor: per tile column the renderer remembers which rows of the RAM buffer, and of each of the two VRAM view buffers, already hold plain ceiling or floor. Those rows are neither refilled nor DMA'd again. Anything else that draws into the view area bumps `vid_mode_serial`, which resets this bookkeeping (and the visibility stamps).
- V-int DMA: the uploads program the VDP DMA registers directly and keep the Z80 off the bus once per batch. Up to four adjacent tile columns go out as one transfer (they are adjacent in RAM and in VRAM alike), which cut the number of transfers per frame from about 32 to about 8; each one costs far more than the bytes between two columns do. Four is the sweet spot: larger groups delay the upload until it no longer overlaps the drawing.
- Finding the visible objects: the tilemap index of every static object is cached (`statspot`, rebuilt when `stat_serial` changes), so the scan is three instructions per object; actors are ruled out with nine compares before the full test.
- Sprites: each sprite's ray range is trimmed to its non-empty texel columns, and the tile column loop sweeps the sprite list left to right instead of testing every sprite in every column.
- Unchanged frames are not drawn. The weapon and the status bar are no longer part of the view, so the view only depends on the viewpoint, the doors, the push wall and the visible sprites. `R_Frame` hashes the first three; when they are the same as in the last frame drawn it does not cast the rays again, and when the sprite list hashes the same too it leaves that frame on screen. Standing still, the game loop then runs at the tic rate instead of the frame rate.
- Detail: Low (main menu; High is the default and is not saved). Only the even rays are cast and drawn, each 4 pixels wide, so the picture is exactly the high detail one with every odd ray replaced by the ray to its left; the weapon and the status bar keep their resolution. The rays are drawn by a second set of scalers that move words (`scaler_tab2`), from a copy of the wall textures in which every texel is stored twice (`tools/walls2.py`), so a 4 pixel wide row costs what a 2 pixel wide one does. `rcl_columns` and the low detail sprite drawer are the same source as the high detail ones, assembled with a flag.
- ROM: the low detail tables bring the image to 4004 KB of the 4096 KB a cartridge without a mapper can hold, so about 90 KB are left.
- RAM: the per-frame sprite list and the object cache live in the tail of `screenbuf` that the game mode leaves unused, so the work RAM has about 2.1 KB of stack to spare at the deepest point measured (it was 740 bytes).

- The weapon is hardware sprites over the view (`tools/weapon.py`, `vid_weapon`) instead of being scaled into it every frame, which also saves refilling the background behind it and uploading it; about 4% of a frame. Each of its 20 frames is pre-rendered into tiles in ROM (29 KB), uploaded to the off-screen one of two tile banks when the frame changes, and shown by switching the sprite table. Sprite colour 0 is transparent but the weapons use view colour 0, so the sprites use palette line 3, a copy of the view palette that `vid.c` keeps in step with line 0 (fades and damage flashes included). Anything that draws over the view, and every mode switch, hides the sprites.

### Measuring and checking
The test hooks are compile-time options; a normal build contains none of them.
- `make EXTRA_FLAGS="-Isrc -g1 -DVT_TOUR"`, then `tools/vtbench.py tour out/rom.bin out/release/rom.out` prints the room and tour frame rates. The sequence depends only on the frame number (actors are frozen), so builds are compared on exactly the same work.
- `-DVT_SEQ` renders the same 240 views and stores a checksum of the displayed VRAM after each frame. `tools/vtbench.py cmp ROM_A ELF_A ROM_B ELF_B` compares two such builds frame by frame.
- `-DVT_SEQ -DVT_DOORS` (`src/vt_doors.c`) swaps those views for ones that look at doors in every state of opening and at a sliding push wall, which the normal sequence hardly shows; compare two such builds the same way.
- `-DVT_LOWDETAIL` starts in low detail. With `-DVT_EVENSUM` the checksums are taken over the picture low detail has to show (every odd ray replaced by the ray to its left), so a high and a low detail build made with it must give the same 240 checksums.
- The weapon is not in the view buffer any more, so against a build from before that change compare with `-DVT_NOWEAPON` on both sides (checksums) and without it (the final screenshot, which includes the sprites). `-DVT_SEQ -DVT_WEAPON` shows the weapon frame given by the ROM byte `vt_weapon_sel`, to compare screenshots of all 20.
- `-DVT_MAPS` shows every map for 100 frames and moves on (checks all levels load). `-DWOLF_TEST=4` plays the sounds listed in `-DWOLF_SNDS=...`; any other `WOLF_TEST` value starts a game at `-DWOLF_EP=<0..5>` `-DWOLF_MAP=<0..9>`.
- `-DSTACKPAINT` fills the free stack with a marker at boot, so a RAM dump shows the deepest stack use.
- `tools/harness` drives the Genesis Plus GX libretro core headless (scripted pad input, screenshots, RAM dumps); `tools/fps.sh`, `tools/run.sh` and `tools/prof.sh` use it.

## Full version (WL6)
`convert.py` looks at the extension of the files in the data directory (WL6 is preferred when both are present; a fourth argument `WL1` or `WL6` forces one). With WL6 data:
- New Game first shows the episode list (six episodes, B goes back), then the difficulty. Each of the 60 levels has its own music, ceiling colour and par time, which are parsed from the original GPL source (`tools/srctab.py`), and each episode has its own victory text.
- Maps are stored compressed (plane 0 as byte RLE, plane 1 as sparse rows) and unpacked per level; save games store the episode.
- The 16-colour view palette is chosen per data set (`viewpal.json` for WL1, `viewpal_wl6.json` is generated on the first WL6 run).
- The help text of the registered game refers to pictures that do not exist in the Genesis layout; only the three that map to real WL6 pictures are kept.
- Sound: the digitized sounds and AdLib effects of the full game would be 2.7 MB. To keep the ROM under 5 MB, the AdLib rendering of every sound that also has a digitized version is left out (those play digitized even with the AdLib setting) and silence at the end of the AdLib sounds is cut, which leaves 1.6 MB. `convert.py` writes it to `src/gen/pcm.bin` and `make` appends that file with `tools/packrom.py` right behind the code and data, at `WOLF_PCM_BASE` (default 0x320000, which makes the ROM 4,980,736 bytes; the Makefile exports it, `make data` and the build must use the same value, and the code and data must not be bigger than it, otherwise `packrom.py` stops and you raise it in steps of 0x20000). The ROM is padded to a multiple of 128 KB and its header end address and checksum are patched. Samples never cross a 512 KB boundary. At start-up the game looks for a marker at the start of the page at 0x400000, first through a banked window at 0x380000 (`SYS_setBank(7, n)`) and, if that is not present, linearly; with neither, sound effects are off. Samples below 0x380000 are always read directly. `WOLF_PCM_EXTERN=0` keeps the sounds inside the image (shareware default).
- Testing: in Genesis Plus GX all 60 levels load and render, and the linear mapping plays every sound identically to the linked-in data. On real hardware, a user (CanalGamebit) reports the full-version ROM running on a Sega Genesis Model 1 with an Everdrive V3 Pro, with Episode 6 started and playable (a quick test; sound and the rest of the game were not reported on, and which of the two page mappings the cart used is not known). The banked window path could not be tested in the emulator, because its SSF header mode is silent even with the sounds linked in. Whether a flash cart or emulator exposes the pages linearly (most do up to 8 MB) decides which path is used.
- The retail data is commercial: do not commit or distribute `src/gen` or the ROM built from it.

## Building
Needs SGDK 2.x and an m68k GCC toolchain, plus the data files: either the full version (`VSWAP.WL6`, `MAPHEAD.WL6`, `GAMEMAPS.WL6`, `AUDIOT.WL6`, `AUDIOHED.WL6`, `VGAGRAPH.WL6`, `VGAHEAD.WL6`, `VGADICT.WL6`) or the shareware ones (`VSWAP.WL1`, `MAPHEAD.WL1`, `GAMEMAPS.WL1`, `AUDIOT.WL1`, `AUDIOHED.WL1`, `VGAGRAPH.WL1`, `VGAHEAD.WL1`, `VGADICT.WL1`) from https://archive.org/details/wolf3dsw .
`make data WOLF_DATA=<dir with the data files> WOLF_SRC=<checkout of github.com/id-Software/wolf3d>` runs `tools/convert.py` (the source is needed for the sound names and the level tables); then `make EXTRA_FLAGS="-Isrc -g1"`. The sprite row and wall height tables depend only on the view geometry, and the weapon tiles are made from the converted sprites, and the doubled wall textures from the converted walls, so all three can be regenerated without the data files: `python3 tools/rtables.py .`, `python3 tools/weapon.py .` and `python3 tools/walls2.py .`

## Credits and licence
Wolfenstein 3D is (c) id Software. The game data is the shareware release, whose licence permits limited, non-commercial redistribution as-is; the original game source (github.com/id-Software/wolf3d) is GPL. Made as a fan port with reference to haroldo-ok's Catacomb Abyss Mega Drive port. This is an unofficial port.
