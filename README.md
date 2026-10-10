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
4. Run `Game/720degrees.exe`. The build copies the executable and `SDL3.dll` next to your ROMs.

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

## How it works

```
Game/ (your ROMs) --tools/*.py--> port/src/game/*.cpp   translated main (T-11) + sound (6502) programs
                                        |
port/src/native   runtime: interrupts, time slicing (fibers), bank dispatch
port/src/board    native hardware: memory map, playfield/sprite/text renderer,
                  POKEY, YM2151 (ymfm), TMS5220 speech, controls, EEPROM
port/src/app      SDL3 front end (window, audio, input)
port/src/level    level data model, encoder and mod patches (level editor)
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
  `tools/levels.py` and `port/src/level/leveldata.h` for the format. Edited levels are written as
  `.720mod` patch files. The native board maps extra ROM pages for them, so edited levels are not
  limited by the original ROM size.

## Tools

| Tool | Purpose |
|---|---|
| `verify720` | Lockstep check: translated game against the reference model |
| `render720` | Run the game headless; write screenshots and a WAV of the audio |
| `ref720` | Reference model with coverage, memory tracing and breakpoints (reverse engineering) |
| `leveltool` | Level statistics, render all maps to PNG, build test mods |
| `tools/listing.py` | Annotated disassembly of the main program (`re/main_t11.lst`) |
| `tools/gen_pdp11_sleigh.py` | Ghidra processor module for the DEC T-11 (`re/ghidra/PDP11`) |
| `re/ghidra/scripts/*.java` | Ghidra headless scripts: decompile the main and sound programs |

## Status

* Main and sound programs translated and verified. Video, POKEY, YM2151 and TMS5220 audio done.
* Level editor: format decoded and round-trip encoder verified for all maps. The editor UI is in progress.

## Credits

Hardware behaviour follows MAME's `atarisy2` driver and device cores (BSD-3-Clause, Aaron Giles and
contributors). YM2151 synthesis uses [ymfm](https://github.com/aaronsgiles/ymfm) (BSD-3-Clause).
UI by [Dear ImGui](https://github.com/ocornut/imgui) (MIT). [SDL3](https://libsdl.org) (zlib).
720 Degrees is a trademark of its owners; this project is an unofficial fan port.
