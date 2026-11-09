# Musashi differential harness (Apeiron 1.0.2)

Runs the original 68k code in the Musashi emulator (Karl Stenerud, MIT-style licence)
from https://github.com/kstenerud/Musashi (not included: clone it into `../musashi_src`, unmodified) on the same memory as the C re-implementation and compares
results. The memory image is built the way the Ghidra script of the author's private analysis folder did it (CODE n at
0x20000 + (n-1)*0x10000, A5 world at 0x200000 with the jump table rewritten as JMP absolute). A-line traps are skipped and counted.

    python ../../tools/split_code.py "<your Apeiron file>" code      (once: writes code0.bin..code9.bin from your own copy)
    git clone https://github.com/kstenerud/Musashi.git ../musashi_src
    .\build_musashi.ps1
    .\bin\test_rng.exe code

| Test | What | Result |
|---|---|---|
| `test_creatures` | 19 routines of CODE 3 against `src/creatures.c`: sprite deactivate, default move, 8 frame callbacks, gecko, scorpion, flea, spaceship, coin, multiplier, psychedelic mushroom, bullet, mushroom removal. Random sprite, grid, globals and RNG seed per trial; every touched byte and the list of calls to unported routines (sound, explosion, mushroom allocator, stubbed) compared | 300,000 trials each, 5.7 million in all, 0 mismatches |
| `test_creatures` (pede) | `seg3_4BC6`, the Pentipede head and body move callback, against `move_pede`: 1,000,000 trials with random segment records, 10-entry position rings, direction/next/target/dive fields, two grids, bounds and step globals; the overlap search (`seg3_055E`) is stubbed with chosen results; a mutation check (6 deliberate changes to the C code) was caught each time | 0 mismatches |
| `test_step` | `seg3_1A72` (score counting up, extra life every 20,000, lives and bonus display) and `seg3_1BB6` (the per-step update: timers, mushroom/flea limits by score, level display, player hits, coin pick-ups, death and respawn) against `update_score_display` and `step_update`: 1,000,000 trials each. 23 routines they call (sound, drawing, spawning, collision search, key test) and the `_SetEntries` trap are stubbed and compared by address and raw stack arguments; long multiply/divide and the RNG run for real. 7 deliberate changes to the C code were all caught | 0 mismatches |
| `test_engine` | the sprite engine: list creation (0712), sprite allocation (0782), unlink (0884), activate (0902), overlap search (055E), move pass (00EA) and erase/draw pass (020C..), 300,000 trials each with random lists, sprites, flags, class tables and row tables. The generated drawing code is replaced by register-logging stubs, so each blit's address, A0, A1, D0, D1 and the order are compared; `_NewPtr` is a bump allocator on both sides, `_DisposPtr` is logged. 7 deliberate changes were caught | 0 mismatches |
| `test_spawn` | seg3_3E26 explosion, 31A0 mushroom, 3558 fragments, 363A sparks, 3740/3854 score digits: every new sprite record, list link, heap use and RNG seed compared; 4 deliberate changes caught. Their six sprite callbacks (4B6A, 472E, 444EE, 444BA, 46C8, 469A) are in `test_creatures` | 0 mismatches |
| `test_shots` | the shooter move (5216) and frame (54C8), shot move (54DA) and firing (2256), coin move (454C), root (3968) and coin (3EF4) spawns, the special bullet (4010) and the shot hit handler (557A, all hit cases: mushrooms with the poison/special/coin/score outcomes, pede segments and chain splitting, flea, multiplier, gecko, psychedelic, coin, fodder, root, scorpion): 100,000 trials each (500,000 for the hit handler), 12 deliberate changes caught. The sound player and the pause hook are stubbed | 0 mismatches |
| `test_spawn` (timed spawners) | also seg3_3A36 psychedelic mushroom, 3B02 multiplier and 3BD8 fodder spawners (random empty cell, 1 in 50 fodder groups); 50,000 trials each, 2 deliberate changes caught | 0 mismatches |
| `test_spawn` (fleas, reinforcements) | also seg3_3CEA flea spawner and seg3_2128 (the extra pede head after a touchdown; the harness now emulates `_SetRect`); 30,000 trials each, 2 deliberate changes caught | 0 mismatches |
| `test_spawn` (shield, launches, round) | also seg3_2048 shield start, 4874/490E shield frame and follow, 20CC house cleaning, 2334 timed launches of scorpion/gecko/fleas, 30F6 clear playfield, 299A build round (timers, mushroom repair, the whole Pentipede chain with position rings, gecko, scorpion, shooter, shield); 30,000 trials each, mutations caught. Stubbed: the sound player and sound-playing check, the Monitor/Sound tool calls (0x36A, 0x39A, 0x3E2) and the screen redraw (432F8) | 0 mismatches |
| `test_loop` | one pass of the in-game loop (`seg3_123C`, entered at its body 0x41244; `game_pass`) with a random whole-game state: engine move pass, mushroom dirty marking (0F2C, also tested alone in `test_shots`), per-step update, shot and shield hit handling, draw pass, palette flash and the end of a wave with a new round; all ported routines run natively on both sides, the Mac side (sound, drawing, palette, Toolbox, end-of-round waits) is stubbed and compared; 60,000 trials | 0 mismatches |
| `test_replay` | a whole recorded game (`apeiron_replay_*.bin`, see the main README) played through the port and, step by step, through the original loop body `seg3_123C`; after every step the application globals, engine, lists, grids, sprites, heap and random seed of the two are compared. Inputs given to both: the recorded mouse, button, Ticks and sound-playing answers. Both sides allocate with the port's allocator. Eight recordings (waves 1, 3, 6, 8, 12, 18, 25, 40, auto-fire and a swaying shooter, 2-5 thousand steps each); a deliberate difference every 50 steps was caught | 28,115 steps compared, 0 differences |
| `test_rng` | `seg9_014E` against `src/rng.c`: 20,000 random and edge-case seeds and ranges, plus the reseed path | 20,001 checks, 0 mismatches |

Adding a test: write `test_x.c` using `image_load`, `wr16/wr32` to set up memory, `m68k_call(addr, args, wide, n, limit)` for the original,
the C function on a copy of the same state, then compare; add its name to the list in `build_musashi.ps1`.
Routines that call Toolbox traps need the trap emulated in `harness_instr_hook` first.

Notes
- A `jsr d(A5)` lands at jump-table entry + 2 (entry + 0 holds the segment offset), so the harness writes the JMP at entry + 2.
- `dasm.exe <dir> <hexstart> <hexend>` disassembles the original code with Musashi's disassembler.
- A first run showed 10 mismatches that were not port bugs: the pede-body test followed random sprite fields as pointers and corrupted the 68k image for later tests; it now uses a valid record.
- Original quirks kept: the spaceship stores the plain pixel value (not 16.16) in its V position when it hits the bottom edge; the coin adds 1 to the high word of its vertical velocity (word add); the scorpion compares and wraps with 16-bit values.
- A-line traps are skipped by the harness and logged like stubbed calls (address = the trap opcode). `_SetEntries` (0xAA3F) is a Pascal procedure, so the harness pops its 8 bytes of arguments; without that the 68k stack went wrong and the run fell into address 0.
- **The game is 68020 code.** The compiled routines use the scaled index mode `(A1,D1.w*4)`, which the 68000 does not have; the harness first ran a 68000 and jumped into garbage. It now runs a 68020 (all earlier tests still pass). The original therefore needs a 68020 or better.
- Harness traps: `_NewPtr` (bump allocator at 0x500000), `_DisposPtr` (logged), `_OffsetRect` (done), `_SwapMMUMode` (ignored), `_SetEntries` (logged, pops 8 bytes). Register stubs (n = -2 or -3 in `harness_add_stub`) log A0, A1, D0, D1 or A0, D0.

## Playing a recording through the original code (`test_replay`)
    .\build_musashi.ps1
    .\bin\test_replay.exe code <data folder> <recording.bin> [max differing steps, default 10]
(`code` is the folder made by `split_code.py`; `<data folder>` is the user's game files.) It prints each step where the 68k code and the port differ (region, address or A5 offset, both values), then a summary; exit code 0 means no differences and the port stayed in step with the recording. `MUT=1` plants a difference every 50 steps (to see that it notices), `TRACE_STEP=n` prints the 68k instruction trace of step n, `DBG_FRAMES=1` lists sprites whose frame has no drawing routine.
What it does not cover: a step in which an end-of-round loop, the pause or a cheat key ran, and the first state of every new game, are taken from the port (the 68k copy is set equal to it again); the 68k drawing is stubbed, so screens are not compared; the port's class data stand-in has no entry for frame 0, so for the 68k step that entry is a routine that returns at once (a pede head is drawn at frame 0 before its first frame step; the real class data has something there). A recording made with `fix_coin = 1` follows the fixed coin rule, so the 68k side will differ at the first stuck coin; record with the switches off to compare with the original.
