# Cybattler native recompilation

A native Windows / Linux / Steam Deck build of **Cybattler** (Jaleco, 1993, Mega System 1 type C).
Both 68000 CPUs (main 12 MHz, sound 7 MHz) are **statically recompiled to C** from the ROM; the rest of the board
(tilemaps, sprites, priority PROM, YM2151, 2x OKI M6295, the I/O MCU) is reimplemented in C.

No ROM data is included. You need your own `cybattlr.zip` (MAME set).

## Running

Put `cybattlr.zip` next to the program (or in `roms/`, or `~/Downloads/`), or pass its path:

```
cybattler.exe C:\Users\you\Downloads\cybattlr.zip
./Cybattler-x86_64.AppImage ~/Downloads/cybattlr.zip
```

The game is vertical (ROT90). It's rotated for a horizontal screen by default and goes fullscreen on its own on a Steam Deck.

| Action | Keyboard | Gamepad / Steam Deck |
|---|---|---|
| Move | Arrows | D-pad / left stick |
| Shot / Sub-weapon | Z / X (or LCtrl / LAlt) | A or X / B or Y |
| Start | 1 or Enter | Start |
| Coin | 5 (P2: 6) | Select/Back |
| P2 | I J K L, Q / W | second controller |
| Service | 9 | |
| Save / load state | F5 / F7 | |
| Reset | F3 | |
| Fullscreen | F or F11 | |
| TATE (no rotation, for a rotated monitor) | T | |
| Smooth / sharp scaling | S | |
| Pause / quit | P / Esc | L+R+Start quits |

DIP switches: `--dsw1 ff --dsw2 bf` (the defaults: 1 coin 1 credit, normal difficulty, demo sounds on).

### Steam Deck
1. Copy `Cybattler-x86_64.AppImage` and `cybattlr.zip` into the same folder (for example `~/Games/Cybattler/`).
2. Right-click the AppImage > Properties > Permissions > "Is executable".
3. In Steam, choose *Add a Non-Steam Game*, pick the AppImage, then play it from Game Mode.
   It only needs glibc 2.27 or newer and picks up X11, Wayland, PipeWire, Pulse or ALSA at runtime.

## How the recompiler works (`tools/recomp68k.py`)

1. **Discovery.** It disassembles recursively from the 68000 exception vectors, then adds seed addresses
   (`seeds/*.txt`) that came from tracing real play sessions on the interpreter. Cybattler runs a small
   cooperative task kernel that switches tasks with `RTE` and makes system calls through line-F opcodes.
   Without the traces, static analysis only reaches about 800 instructions. With them, 15,299 main-CPU and
   1,030 sound-CPU instructions are recompiled.
2. **Translation.** Each instruction becomes a labelled C block. That block calls the exact Musashi opcode handler for its
   opcode word directly (the fetch, decode and jump-table steps are removed) and charges that instruction's own cycle cost.
   Branches whose targets are known become plain `goto`s. Indirect flow (RTS/RTE, jump tables, interrupts) goes through a `switch` on the PC.
   The code is split into 4 KiB chunks, one C function per chunk.
3. **Fallback.** A PC that was never discovered is run on the interpreter and logged to `missing_*.txt`.
   You add it to the seeds and regenerate. In the verification runs this happened 0 times.

The recompiled code makes the same state changes as the Musashi interpreter, one instruction at a time, cycle counts included.
That lets the two be compared by state hash:

```
cybattler --interp --headless 40000 --script play.txt --hash a.txt cybattlr.zip
cybattler          --headless 40000 --script play.txt --hash b.txt cybattlr.zip
cmp a.txt b.txt     # identical for all 40,000 frames (~12 minutes of scripted play)
```

## Building

```
tools/regen.sh path/to/cybattlr.zip          # regenerates generated/*.c from your ROM
cmake -S . -B build && cmake --build build   # Linux (needs SDL2 dev)
```
Windows (cross from Linux): `cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DSDL2_DIR=<SDL2-mingw>/x86_64-w64-mingw32/lib/cmake/SDL2 -DCYB_STATIC_SDL=ON`.
For the portable Steam Deck build, compile SDL2 and the game with `zig cc -target x86_64-linux-gnu.2.28` (see `packaging/`).

`generated/` holds C translated from the ROM, so it is not shipped with the source. Rebuild it with `tools/regen.sh`.

## Hardware notes
* Main CPU map, interrupts (IRQ4 vblank at line 240, IRQ1 at line 96, IRQ2 at line 16 and from the MCU) and video follow MAME's `megasys1` driver (BSD-3-Clause, Luca Elia / David Haywood).
* The TMP91640 I/O MCU is emulated at a high level (HLE). The command codes are `56 52 53 54 55` (coins, P1, P2, DSW1, DSW2). There is also a start-up check (`f2` -> `0d`) and a handshake (`06`).
* Sprites lag two frames behind (buffered at vblank), as on the real board. Layer order comes from the priority PROM, `pr-91028.12`.

## Third-party code
Musashi (Karl Stenerud, MIT-style), ymfm (Aaron Giles, BSD-3), miniz (MIT), SDL2 (zlib).

## Known limits / next steps
* Flip-screen DIP (rendering) isn't implemented.
* Mid-frame raster changes are applied per scanline, not per pixel.
* Next: native C for hot opcodes (MOVE/ADD/CMP/Bcc) in place of handler calls, plugging into the shared M2-style frontend.
