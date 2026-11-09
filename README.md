# apeiron-sdl

A portable re-implementation of Apeiron 1.0.2 (Ambrosia Software, 1995, 68k Macintosh) in C with SDL2 for Windows and Linux. No installation: a folder of programs plus the
user's own copy of the original data files, which this project never contains.

## Status

Phase 1 of the plan is done: the resource layer and an exporter tool.

| piece | what it does |
|---|---|
| `src/rsrc.c` | opens a Mac resource fork from `NAME.rsrc`, `._NAME` (AppleDouble), `NAME.bin` (MacBinary) or a raw fork |
| `src/pict.c` | PICT decoder to an 8-bit indexed image with its own palette (PackBitsRect, **PackBitsRgn**, BitsRect/Rgn, DirectBits 16/32 bit; vector-only pictures are reported) |
| `src/snd.c` | `snd ` (8-bit mono, standard header) and `clut` decoders |
| `src/writers.c` | PNG (indexed and RGB) and WAV writers, no dependencies |
| `tools/apeiron_export.c` | `apeiron_export <data_dir> <out_dir>`: writes every bitmap PICT as PNG, every `snd ` as WAV, every `clut` as a swatch PNG, and prints a report |

## Build

Windows (Visual Studio 2022): `.\build_windows.ps1` produces `bin\apeiron_export.exe`.
Any platform with CMake and a C compiler: `cmake -S . -B build && cmake --build build` (Linux not tried yet).

## Check

```
bin\apeiron_export.exe "<folder with the original files>" out\original
```

Results on the original Apeiron 1.0.2 files: 60 images, 47 sounds (42 effects + 2 music + 3 from the registration program), 7 colour tables, 3
vector-only pictures, 0 failures; on the Star Wars pack: 46 images (including its `PackBitsRgn` sprites), 42 sounds, 0 failures. Every
PNG has the same size as its PICT frame, and every WAV is 8-bit mono at 22,255 Hz.

## SDL front end (first milestone)

`build_windows.ps1` also builds `bin\apeiron.exe` when SDL2 is present in `third_party\sdl2_dl\SDL2-2.30.9` (the official SDL2 2.30.9 Visual C++ development
package from github.com/libsdl-org/SDL/releases, unzipped there; `SDL2.dll` is copied next to the program). `apeiron.exe [data folder]` reads `apeiron.ini`,
loads the original files (each file's resource fork as `._Name`, `Name.rsrc` or MacBinary, see `rsrc.h`), draws the title art with the game's own palettes,
runs a fixed-rate loop at `loop_hz`, keeps 4:3 at any window size, turns mouse movement into pending movement values, and quits on Esc. `APEIRON_SHOT=file.bmp`
saves the first frame and exits (works with `SDL_VIDEODRIVER=dummy`). The game logic (`src/creatures.c`) is not connected yet: that needs the start-up that
`seg2_02F4` does (lists, classes, grids, the sprite class strips) and the blit routines that replace the generated drawing code.

## The playfield runs (second milestone)

`src/game.c` builds the start-up state the original made in `seg2_02F4` and its helpers: the 16 MB 68k-style memory image (with the jump table
rebuilt from the `CODE 0` resource of the user's `Apeiron` file, so jump-table callbacks resolve), the 21 sprite classes cut from the strips of `Apeiron Sprites`
(index 0 transparent), the five sprite lists, the grids, the engine record and the three off-screen buffers. The engine's generated drawing routines are replaced by host blits
(`game_ext`): they draw a class frame into the work buffer, and copy its shape from one buffer to another, exactly where the original's routines would. The screen palette is the one
carried by the side-panel picture (Apeiron Titles PICT 401); the sprite strips' own palettes are not the one in use. Heap blocks are reused (`reuse_memory`), the random number generator is
seeded from the clock. `apeiron.exe` starts a game (3 lives, wave 1, 30 mushrooms, a Pentipede), runs `game_pass` at `loop_hz`, feeds it the mouse and left button, and quits on Esc or when the
game ends. Test mode: `APEIRON_SHOT=f.bmp APEIRON_PASSES=n [APEIRON_AUTOFIRE=1]` with `SDL_VIDEODRIVER=dummy`.

Not done yet: the side-panel numbers (score, bonus, lives, wave; `415E4`, `1428`, `157A`), sound, the title screen and menus, the end-of-wave sequences (`26C2`, `2836`), high score load and
save, palette fades, level backdrops and the `Apeiron` loading of plugin packs. Starting values that were inferred rather than read (lives 3, one shot at a time, the shooter's box limits) are in `game_init` and `game_new`.

## Side-panel numbers and sound (third milestone)

- **Palette:** the original drew each picture into an off-screen port that had the game's colour table, so every picture's colours were remapped to the nearest entry of that
  table. The port does the same once at load (`remap_image`); the game table is the `clut` 128 of the `Apeiron` file (`APEIRON_CLUT=id` tries another). Index 0 stays white = transparent.
- **Panel:** score, bonus and wave are drawn with the 11 digit glyphs of PICT 1100 (24 x 32, right to left, as `seg3_15E4`), the lives as eight 16 x 16 icons 20 px apart (`seg3_157A`) and the
  multiplier and power-ups as icons 15 px apart (`seg3_1428`), at the positions the original's panel setup computes (score 597,133; bonus 597,233; wave 540,432; lives 597,340; power-ups 492,381).
- **Sound:** the 36 sound globals are set to the `snd ` ids from `sound_table.json`; `src/mixer.c` is the Sound Tool's four channels with the original priority rule (a new sound takes a free
  channel, else replaces the lowest-priority channel if its own priority is not lower), linear resampling from 22254.5 Hz to the 22050 Hz device, volume setting 0..3. The sound-playing check (`3D2`) asks the mixer.
  Music (`Apeiron Music`, ids 1000/1001) is not played yet. Audio was run against SDL's dummy driver only; I have not listened to it.

## Investigating slowdowns
`set APEIRON_PROFILE=1` and `set APEIRON_RUNSECS=30` before starting `apeiron.exe` prints, every 300 frames: the time of one game pass, the picture conversion and the present (which waits for the monitor), the longest frame, late frames,
how many game ticks the game counted against real ticks, and how many Ticks passed between consecutive passes (0 / 1 / 2 / 3 / 4+). On the development machine one pass takes about 0.02 ms, so the game's own work is not the cause of slowdowns;
the timing of the passes is: with `loop_hz = 60` and the Mac's 60.15 Ticks per second, about 5 % of passes saw 0 or 2 Ticks instead of 1. `steady_ticks = 1` in `apeiron.ini` removes that jitter.

## Plugin packs
A pack is a set of replacement data files (for example the "Star Wars for Apeiron" pack: `Apeiron Sprites`, `Apeiron Sounds`, `Apeiron Titles`). Make a folder holding the original files, put the pack's files over them (a `NAME.rsrc` file wins over `._NAME`), and start `apeiron.exe <that folder>` or set `data_dir`. Checked with the Star Wars pack: the starfield backdrop, TIE fighters, asteroids, X-wing lives and the yellow digits all come out, and all 36 sounds load.

## Title screen
`apeiron.exe` now starts on the title screen: click Play Apeiron (or press P), Quit (Q), the arrows change the sound volume and music (saved in the scores file), and after a game the title shows again with the new score in white. Instructions (three pages with demonstration sprites), Clear Scores and About work. `APEIRON_NOTITLE=1` starts a game at once.

## Fades and fonts
Screens change as in the original: a grey flash, fade to white and to black (Play, game over, Instructions), and fade-ins. `fade_ms` in `apeiron.ini` is the length of one 30-step fade (`0` switches the effects off; `APEIRON_NOFADE=1` does the same for tests). While the caterpillar plays at game over, Esc or the mouse button skips ahead to the score and the title. The text uses fonts rendered from Noto Serif and Liberation Sans (SIL Open Font License) by `tools/gen_fonts.py`; the Mac fonts of the original are not available.

## Start-up intro and cursor
The program opens with the Ambrosia logo and the swirling Apeiron logo with the intro theme (Esc or a click skips; `intro = 0` in `apeiron.ini` turns it off), then the title with the pointing-hand cursor of the original.

## Linux (Debian, Ubuntu)
`sudo apt install build-essential libsdl2-dev`, then `./build_linux.sh` and `cd bin && ./apeiron <folder with the original files>`. File names are case-sensitive there: the data files must be called exactly `Apeiron`, `Apeiron Sprites`, `Apeiron Titles`, `Apeiron Sounds`, `Apeiron Music` and `Apeiron Scores`, each as `NAME.rsrc`, `._NAME` or `NAME.bin` (see the data section). All sources compile with gcc 9 on Ubuntu 20.04 (WSL); linking and running there has not been tried.

## Mouse capture
During play the mouse is captured (hidden, movement only). **Ctrl-G** lets it go and shows the pointer, as in QEMU; while it is loose the shooter does not move; a click in the window takes it back (that click does not fire). Ctrl-G works on the game-over screens too (the mouse stays captured and hidden through them until the title), not only during play. The game keeps running meanwhile.

## Icon
The window icon is the application icon of the original (`icl8` 128 with the mask from `ICN#` 128), read from your Apeiron file when the game starts; this works on Windows and Linux. On Windows `build_windows.ps1` also gives `apeiron.exe` that icon when the data files are in `bin\data` (or in the folder named by the environment variable `APEIRON_DATA`) while building; the `.ico` and the compiled icon stay on your disk. A Linux binary has no icon of its own: a desktop launcher (`.desktop` file) would have to name an image file.

## Slowdown log
Set `hitch_log = 1` in `apeiron.ini` (or `APEIRON_HITCHLOG=1`) and play; `apeiron_hitches.txt` appears in the folder you started from. It lists every frame that took more than 1.5 steps (total, drawing, and waiting for the monitor, with the number of game passes in it) and, every 5 seconds, the game ticks counted against real Ticks. A slowdown with a frame line is the machine or the display (the part that is long says which); a slowdown with only a low game-time percentage is Tick counting (see INFERENCES.md); a slowdown with neither is the game itself.

## Original-bug switches
`apeiron.ini`: `fix_coin = 1` stops the coin that rises to the top of the screen and stays there (it gives a coin thrown up with sideways speed 0 a tiny drift, so the original's own gravity takes over). `fix_glide = 1` stops the centipede "glide": in the original, two heads that meet head-on at a side wall are put on top of each other, stay locked together swapping directions at every row, and slide down the wall and back up; with the fix the head keeps its own place and the two separate (found in a recording and checked against the original 68k code, which does the same; the fix is stored in a recording's header, so a recording replays with the setting it was made with). `pede_log = 1` writes `apeiron_pede.txt` when a centipede head comes down five rows within 40 passes without poison (the "glide"), for finding its cause; it changes nothing in the game.

Replays: `record = 1` in `apeiron.ini` writes each game to `apeiron_replay_<date>_<time>.bin` (F8 in the game adds a mark, for "it just happened"). The game is deterministic, so what it takes in is all that is written: the seed of each new game, and for every step the mouse movement, the button and the clock (plus keys, and whether a sound was still playing, which the audio thread decides). `APEIRON_REPLAY=file apeiron.exe` plays one again without a window, with the head watcher on, prints the marks, checks itself against the recording (a hash every 600 steps and at the end) and says whether it stayed in step; `APEIRON_REPLAY_SHOT=<step>` also writes the screen after that step to `replay_<step>.png`. **Watching a replay:** drop the `.bin` on `apeiron.exe`, or run `apeiron.exe --replay file.bin`. It plays in a window with sound at the ini's `loop_hz` (and `scale`). Space pauses, `+` / `-` halve or double the speed, `0` returns to normal, `N` runs fast to the next mark (F8 mark from the game), Esc or closing the window ends it; the window title shows the step. At the end the last frame stays up. Checked headless with the dummy video driver (a 1,170-step replay ran to its end in about 20 s, as it should at 60 Hz); the keys and the picture itself have not been tried on a real screen.

**Checking a recording against the original code:** `verify\musashi\bin\test_replay.exe <folder with code0.bin..code9.bin> <data folder> <recording.bin>` plays it through the port and the original 68k code and compares them after every step. The code folder is made from your own Apeiron file with `python tools/split_code.py <Apeiron file> <folder>`; Musashi is fetched separately (see `verify/musashi/README.md`).

Checked: recordings of 9,000-pass runs (with a game over and a new game inside one) replayed in step, to the final state.
