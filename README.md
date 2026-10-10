# 720 Degrees - native PC port

A native Windows port of Atari Games' **720 Degrees** (1986 arcade, rev 4) written in C++ with SDL3,
plus a level editor. The game logic is the original arcade program, statically translated from
its machine code into C++; graphics, sound chips and controls are reimplemented natively. No CPU
emulator runs inside the game.

> You need your own copy of the arcade ROMs. Nothing derived from the ROMs is stored in this
> repository: the translated program code is generated on your machine from your ROM files.

## Quick start

1. Put the 37 ROM files of the MAME set `720` (rev 4) into the [`Game`](Game/README.txt) folder.
2. Install a C++20 compiler, CMake 3.20+, Ninja (or Visual Studio 2022) and Python 3.
3. Build:

   ```sh
   cmake -S port -B port/build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build port/build
   ```

   Configuration translates the program ROMs to C++ (`tools/t11_to_cpp.py`,
   `tools/m6502_to_cpp.py`) and downloads the SDL3 development package if SDL3 is not installed.
4. Run `Game/720degrees.exe`. The build copies the game, the level editor (`editor720.exe`) and
   `SDL3.dll` next to your ROMs.

Tested with MSYS2 UCRT64 GCC 15. Visual Studio uses the SDL3 VC package (`-G "Visual Studio 17 2022"`).

## Controls

| Action | Keyboard | Gamepad | Mouse |
|---|---|---|---|
| Point the skater | Arrows / WASD (8 directions) | Left stick or d-pad | Move left/right after clicking the window |
| Rotate | Q / E | | |
| Jump | Space / Z | A | Left button |
| Kick | Left Ctrl / X | B / X | Right button |
| Coin | 5 (left), 6 (right) | Back | |
| Service mode | F2 | | |
| Reset / Pause / Fullscreen | F3 / P / F11 or Alt+Enter | | |
| Help overlay | F1 | | |

High scores and settings are kept in the game's EEPROM image, saved under
`%APPDATA%\Atari\720Degrees\720.eeprom`. DIP switches: `--dsw0 0xNN --dsw1 0xNN`.

## Level editor

Run `Game/editor720.exe`. It edits Skate City, the six layouts of each of the four parks and the
attract-mode screens, and saves your changes as a mod file in `Game/mods`. The game loads every
`.720mod` file in that folder when it starts; `--no-mods` turns them off and `--mod FILE` adds one.

* **Map.** Click a cell to select it. In *Paint blocks* mode, drag to paint the brush block.
  Right-drag pans, the wheel zooms, middle-click picks up the block under the cursor and a
  double-click opens a block in the block editor.
* **Blocks.** Every block of the level, with how often it is used. Click one to make it the brush.
  *Make unique* gives the selected cell its own copy, so that editing it does not change other cells.
* **Block editor.** A block is 16 x 16 tiles in two halves. Left-drag paints the brush tile and
  right-click picks up a tile. Each half also has a zone byte: in Skate City its upper bits select
  the tile set used for "bank 1" tiles while that part of the town is on screen.
* **Tiles.** The tile sheet of the current level. Choose the bank, palette and priority bits of the
  brush here. Tiles 0 to 15 are ground placeholders, which the game replaces with varied ground tiles.
* **File menu.** *Save mod* (Ctrl+S), *Open mod* to continue editing, *Export map as PNG*.
  **Play test** (F5) starts the game with your current edits only. Undo and redo are Ctrl+Z / Ctrl+Y.

On its first start the editor runs the game headless for a few seconds to capture the level
colours. The random-input run does not reach the parks, so parks are shown with Skate City's palette.

Mods that edit different levels can be combined. Editing any park rewrites all four parks, because
they share one block bank, so two park mods replace each other.

## How it works

```
Game/ (your ROMs) --tools/*.py--> port/src/game/*.cpp   translated main (T-11) + sound (6502) programs
                                        |
port/src/native   runtime: interrupts, time slicing (fibers), bank dispatch
port/src/board    native hardware: memory map, playfield/sprite/text renderer,
                  POKEY, YM2151 (ymfm), TMS5220 speech, controls, EEPROM
port/src/app      SDL3 front end (window, audio, input)
port/src/level    level data model, encoder and mod patches
port/src/editor   level editor (SDL3 + Dear ImGui)
```

* **Translation.** `tools/codedb.py` finds every instruction of the main program (coverage logs in
  `re/coverage` plus static analysis of jump tables and banked calls). `tools/t11_to_cpp.py` emits
  one C++ function per routine with exact flag, bus and cycle semantics. Interrupts are taken at
  instruction boundaries, so the game behaves exactly like the arcade board, including where the
  32V interrupt preempts the main loop. The game logic runs inside those interrupts.
* **Verification.** `verify720` runs the translated program next to an instruction-level
  reference model (`port/src/ref`, development only) and compares all RAM, video memory, CPU
  registers and every pixel after each frame. Four 30,000-frame random-play sessions are
  bit-identical.
* **Levels.** Maps are grids of 16x16-tile blocks stored as run-length-coded halves. See
  `tools/levels.py` and `port/src/level/leveldata.h` for the format. Each park has six map
  layouts, and maps are stored back to back, so a map ends where the next one begins. Edited
  levels are written as `.720mod` patch files. The native board maps extra ROM pages for them,
  so edited levels are not limited by the original ROM size. Each level uses its own extension
  pages, which is what lets mods for different levels combine.

## Tools

| Tool | Purpose |
|---|---|
| `verify720` | Lockstep check: translated game against the reference model |
| `render720` | Run the game headless; write screenshots and a WAV of the audio |
| `ref720` | Reference model with coverage, memory tracing and breakpoints (reverse engineering) |
| `editor720` | Level editor; `--capture-test` and `--screenshot FILE` for automated checks |
| `leveltool` | `stats`, `png DIR` (render all maps), `zones` (zone bytes per map), `roundtrip [LEVEL]` (patch, re-decode and compare), `identity FILE` / `demo-mod FILE` (test mods) |
| `tools/listing.py` | Annotated disassembly of the main program (`re/main_t11.lst`) |
| `tools/gen_pdp11_sleigh.py` | Ghidra processor module for the DEC T-11 (`re/ghidra/PDP11`) |
| `re/ghidra/scripts/*.java` | Ghidra headless scripts: decompile the main and sound programs |

## Status

* Main and sound programs translated and verified. Video, POKEY, YM2151 and TMS5220 audio done.
* Level editor: all maps decode and re-encode exactly. An unedited mod plays pixel-identically to
  the original, and edits show up in the game.
* Still open: the park palettes, which the colour capture does not reach, and the exact meaning of
  the zone switch modes.
* Windows is the first target. Linux and macOS are planned; the main platform-specific piece is
  the fiber-based time slicing in `port/src/native/coro_win32.cpp`.

## Credits

Hardware behaviour follows MAME's `atarisy2` driver and device cores (BSD-3-Clause, Aaron Giles and
contributors). YM2151 synthesis uses [ymfm](https://github.com/aaronsgiles/ymfm) (BSD-3-Clause).
UI by [Dear ImGui](https://github.com/ocornut/imgui) (MIT). [SDL3](https://libsdl.org) (zlib).
720 Degrees is a trademark of its owners; this project is an unofficial fan port.
