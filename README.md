# apeiron-sdl

This is a functional clone of Apeiron that I used Claude to make. It's functionally identical to the original to the point that it reproduces the same result given the same seed and inputs. It needs the original data files. 

If you're copying from an emulator, the important things are the .rsrc files. Optional bug fixes for bugs in the original are in the .ini files. I would recommend either playing with a high cycle rate or with steady_ticks set to 1. There is a new replay feature; if it is set, your games will produce a .bin file that you can drag onto the .exe to play the recording back. Press Space to pause, + or - to double or halve the speed, 0 to return to normal speed, N to return to a previously marked spot made with F8, and Escape to stop.

Below is Claude's attempt at a readme.

A port of **Apeiron 1.0.2** (Ambrosia Software, 1995, a 68k Macintosh game) to Windows and Linux, written in C with SDL2.

The goal is to play like the original, bugs included. The game logic was written from a Ghidra decompilation of the original program, and each routine was then checked against the original 68k machine code running in the [Musashi](https://github.com/kstenerud/Musashi) emulator (see [Accuracy](#accuracy)). Anything that fixes or changes the original's behaviour is an option in `apeiron.ini` and is off by default.

**This project contains no Apeiron code, art or sound.** You need your own copy of the original game files. Apeiron is (c) 1995 Ambrosia Software, Inc.

Features: the title screen, menus, instructions, high scores, intro, sound and music, mouse control (captured while you play), plugin packs such as "Star Wars for Apeiron", switches for two original bugs, and a recorder that replays a game exactly.

## What you need

The original game files, each with its Mac resource fork. For every file the program looks for `NAME.rsrc`, then `._NAME` (an AppleDouble file, as made by unzipping a Mac archive on another system), then `NAME.bin` (MacBinary):

`Apeiron`, `Apeiron Sprites`, `Apeiron Titles`, `Apeiron Sounds`, `Apeiron Music`, `Apeiron Scores`

Put them in one folder. On Linux the names are matched ignoring case.

## Quick start (Windows)

Download `apeiron-sdl-windows-x64.zip` from the Releases page, unzip it anywhere and run:

```
apeiron.exe "C:\path\to\your\game files"
```

Or set `data_dir` in `apeiron.ini` and just run `apeiron.exe`. Nothing is installed.

## Building from source

### Windows

You need Visual Studio 2022 with the C++ tools and the SDL2 2.30.9 Visual C++ development package (`SDL2-devel-2.30.9-VC.zip` from <https://github.com/libsdl-org/SDL/releases>) unzipped to `third_party\sdl2_dl\SDL2-2.30.9`. Then, in PowerShell:

```
.\build_windows.ps1
```

This builds `bin\apeiron.exe` (and copies `SDL2.dll` next to it), `bin\apeiron_export.exe` and `bin\apeiron_scores.exe`. Without the SDL2 package only the two tools are built.

If your game files are in `bin\data` (or in the folder named by the environment variable `APEIRON_DATA`) when you build, the script also gives `apeiron.exe` the original application icon. That icon is Ambrosia's, so do not publish an exe built that way. Without the files the exe has no icon of its own; the window icon is read from your Apeiron file at start-up on every platform.

### Linux (Debian, Ubuntu and similar)

```
sudo apt install build-essential libsdl2-dev
./build_linux.sh
cd bin
./apeiron /path/to/your/game/files
```

`build_linux.sh` compiles with `gcc` and the flags from `sdl2-config`, and builds `bin/apeiron`, `bin/apeiron_export` and `bin/apeiron_scores`. It copies `apeiron.ini` into `bin/` if there is none there yet. On other distributions install the C compiler and the SDL2 development package under their own names (for example `gcc` and `SDL2-devel` on Fedora, `gcc` and `sdl2` on Arch).

Linux status: all sources compile cleanly with gcc 9 on Ubuntu 20.04. The Linux build has had much less use than the Windows one; if something does not work, please open an issue with the output of `./build_linux.sh` or the error message.

A Linux binary has no icon of its own. The window icon is still taken from your Apeiron file, but a desktop launcher (`.desktop` file) would need a separate image file.

## Playing

- Click **Play Apeiron** on the title screen, or press `P`. `Q` quits. The arrows on the title screen change the sound volume and the music setting, which are saved in the scores file.
- The mouse moves the shooter and the left button fires. During play the mouse is captured (hidden, movement only).
- **Ctrl-G** lets the mouse go and shows the pointer, as in QEMU. The shooter does not move while it is loose; a click in the window takes it back (that click does not fire). The game keeps running meanwhile.
- Esc quits. In the intro and at game over, Esc or a click skips ahead.

## Settings (`apeiron.ini`)

Put `apeiron.ini` next to the program; every option is explained in the file. The main ones:

| option | what it does |
|---|---|
| `loop_hz` | game passes per second (60 by default; the original ran as fast as the Mac could) |
| `scale` | window size as a multiple of 640x480 (the picture keeps its 4:3 shape) |
| `mouse_speed` | multiplies mouse movement |
| `data_dir` | the folder with the game files |
| `fade_ms` | length of the palette fades, 0 switches them off |
| `intro` | 0 skips the start-up screens |
| `steady_ticks` | 1 makes the game clock advance by a fixed step per pass, which removes jitter from timing |
| `shots_per_step_only` | 1 avoids the original's machine-speed dependent shot handling |

### Original-bug switches

Both are off by default, which is the original's behaviour.

- `fix_coin = 1` stops the coin that rises to the top of the screen and stays there. A coin thrown up with sideways speed 0 gets a tiny drift, so the original's own gravity takes over.
- `fix_glide = 1` stops the centipede "glide". In the original, two heads that meet head-on at a side wall are put on top of each other, stay locked together swapping directions at every row, and slide down the wall and back up. With the fix, a head keeps its own place and the two separate. This was found in a recording and confirmed against the original 68k code, which does the same.

`pede_log = 1` writes `apeiron_pede.txt` whenever a head comes down five rows within 40 passes without poison. It is for studying that bug and changes nothing in the game.

## Plugin packs

A pack is a set of replacement data files (for example the "Star Wars for Apeiron" pack: `Apeiron Sprites`, `Apeiron Sounds`, `Apeiron Titles`). Make a folder holding the original files, put the pack's files over them (a `NAME.rsrc` file wins over `._NAME`), and start `apeiron.exe <that folder>` or set `data_dir`. Checked with the Star Wars pack: the starfield backdrop, TIE fighters, asteroids, X-wing lives and the yellow digits all come out, and all 36 sounds load.

## Replays

`record = 1` in `apeiron.ini` writes each game to `apeiron_replay_<date>_<time>.bin`. F8 during play adds a mark ("it just happened"). The game is deterministic, so what it takes in is all that is written: the seed of each new game and, for every step, the mouse movement, the button and the clock (plus keys, and whether a sound was still playing, which the audio thread decides). Quit the game normally so the file gets its end mark. A recording is about 10 bytes per step.

- **Watch one:** drop the `.bin` on `apeiron.exe`, or run `apeiron.exe --replay file.bin`. It plays in a window with sound. Space pauses, `+` and `-` halve or double the speed, `0` returns to normal, `N` runs fast to the next mark, Esc ends it. The window title shows the step.
- **Check one without a window:** `APEIRON_REPLAY=file apeiron.exe` plays it again, prints the marks, checks itself against the recording (a hash every 600 steps and at the end) and says whether it stayed in step. `APEIRON_REPLAY_SHOT=<step>` also writes the screen after that step to `replay_<step>.png`.
- A recording stores the `fix_coin` and `fix_glide` settings it was made with and replays with them.

The replay window's keys and picture have been checked headless (with SDL's dummy video driver) but not exhaustively on a real screen.

## Accuracy

The routines of the original's game logic (sprite engine, creatures, shots, rounds, scoring, the random number generator) were each compared with the original 68k code in Musashi on random inputs, and whole recorded games are replayed through both and compared after every step. The results, what the tests cover and what they do not cover are in [`verify/musashi/README.md`](verify/musashi/README.md). [`INFERENCES.md`](INFERENCES.md) lists the things that were worked out rather than read from the code, with the evidence for each.

To run the checks you need your own Apeiron file, Musashi and a Visual Studio compiler; the instructions are in the verify README. `python tools/split_code.py <Apeiron file> <folder>` writes the original's code resources (`code0.bin`..`code9.bin`) that the tests load.

## Troubleshooting slowdowns

- `set APEIRON_PROFILE=1` and `set APEIRON_RUNSECS=30` before starting prints, every 300 frames, the time of one game pass, the picture conversion and the present (which waits for the monitor), the longest frame, late frames, and how many Ticks passed between consecutive passes. On the development machine one pass takes about 0.02 ms, so the game's own work is not the cause of slowdowns; timing of the passes is. With `loop_hz = 60` and the Mac's 60.15 Ticks per second about 5 % of passes saw 0 or 2 Ticks instead of 1, and `steady_ticks = 1` removes that jitter.
- `hitch_log = 1` (or `APEIRON_HITCHLOG=1`) writes `apeiron_hitches.txt` in the folder you started from. It lists every frame that took more than 1.5 steps (total, drawing, waiting for the monitor, and the number of game passes in it) and, every 5 seconds, the game ticks counted against real Ticks. A slowdown with a frame line is the machine or the display, and the long part says which. A slowdown with only a low game-time percentage is Tick counting (see `INFERENCES.md`). A slowdown with neither is the game itself.

## Tools

`apeiron_export <data folder> <out folder>` writes every bitmap picture as PNG, every sound as WAV and every colour table as a swatch PNG, and prints a report. `apeiron_scores` reads the scores file. On the original 1.0.2 files the export gives 60 images, 47 sounds, 7 colour tables and 3 vector-only pictures with no failures.

## Layout

| folder | what is in it |
|---|---|
| `src/` | the game: resource fork, PICT and sound decoders, sprite engine and creatures (`creatures.c`), start-up (`game.c`), mixer, title screens, fonts |
| `tools/` | `apeiron_export`, `apeiron_scores`, `split_code.py` and small test programs |
| `verify/` | the Musashi comparison tests |

## Licence

MIT, see [`LICENSE`](LICENSE); it covers the code written for this project only. Third-party notices are in [`THIRD_PARTY.md`](THIRD_PARTY.md).
