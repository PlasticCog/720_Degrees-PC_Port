# 720 Degrees - native PC port

A native port of Atari Games' **720 Degrees** (1986 arcade, rev 4) for Windows, Linux and macOS, written in C++ with SDL3,
plus a level editor. The game logic is the original arcade program, statically translated from
its machine code into C++; graphics, sound chips and controls are reimplemented natively. No CPU
emulator runs inside the game.

> You need your own copy of the arcade ROMs. Nothing derived from the ROMs is stored in this
> repository: the translated program code is generated on your machine from your ROM files.

## Quick start

1. Put the 37 ROM files of the MAME set `720` (rev 4) into the [`Game`](Game/README.txt) folder, or just
   drop the MAME `720.zip` there; it is unpacked automatically.
2. Install a C++20 compiler, CMake 3.20+, Ninja and Python 3 (details per platform below).
3. Build:

   ```sh
   cmake -S port -B port/build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build port/build
   ```

   Configuration translates the program ROMs to C++ (`tools/t11_to_cpp.py`,
   `tools/m6502_to_cpp.py`). On Windows it downloads the SDL3 development package if SDL3 is not
   installed; on Linux and macOS it builds SDL3 from source as a static library.
4. Run `720degrees` (`720degrees.exe` on Windows) in the top folder of the repository. The build puts
   the game and the level editor there; the `Game` folder holds only the ROMs.

```
720degrees(.exe)   the game
editor720(.exe)    the level editor
SDL3.dll           Windows only
Game/              your ROM files
mods/              level mods (created by the editor)
```

### Windows

MSYS2 UCRT64 (GCC) is the tested toolchain: `pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,python}`.
Visual Studio 2022 also works (`-G "Visual Studio 17 2022"`, SDL3's VC package is downloaded).

### Linux

```sh
sudo apt install build-essential cmake ninja-build python3 libx11-dev libxext-dev libxrandr-dev \
  libxcursor-dev libxi-dev libxss-dev libxtst-dev libwayland-dev libxkbcommon-dev libegl1-mesa-dev \
  libasound2-dev libpulse-dev libudev-dev libdbus-1-dev
```

(Debian/Ubuntu package names; SDL3 picks X11 or Wayland and ALSA or PulseAudio/PipeWire at run time.)

### macOS

Install the Xcode command line tools (`xcode-select --install`) and `brew install cmake ninja python`.
Apple Silicon and Intel Macs are both supported.

### Packaging

`python tools/package.py` puts the built programs, an empty `Game` folder, this README and the
third-party licences into `dist/720degrees-<version>-<platform>.zip`. The program code inside the
executables is translated from your ROMs, so such a package is for your own use: do not publish it.

## Controls

Default controls are below. Press **F4** in the game to change any of them; the settings are
saved in `controls.cfg` in the game's settings folder (`%APPDATA%\Atari\720Degrees` on Windows).

| Action | Keyboard | Xbox controller | Mouse |
|---|---|---|---|
| Point the skater | Arrows / WASD | Left stick or d-pad | |
| Rotate | Q / E | Right stick, LB / RB | Move left/right after clicking the game |
| Jump | Ctrl | A or RT | Left button |
| Kick | Shift | X or B | Right button |
| Coin | 5 (left), 6 (right) | View | |
| Pause | P | Menu | |
| Debug menu / controls / help | Tab / F4 / F1 | | |
| Service mode / reset / fullscreen | F2 / F3 / F11 or Alt+Enter | | |

Any controller SDL recognises works; the button names follow the Xbox layout. Controllers can be
connected and removed while the game runs.

High scores and settings are kept in the game's EEPROM image, saved in the same settings folder
as `720.eeprom`. DIP switches: `--dsw0 0xNN --dsw1 0xNN`.

## Debug menu

Press **Tab** in the game. The menu has these tabs:

* **Game.** Pause, single-frame steps, game speed, machine reset, and hiding the playfield, sprite
  or text layer.
* **Overlays.** Sprite boxes with their screen coordinates, and a coloured mask of the terrain
  height map. Hover over the game to read the tile and terrain value under the mouse.
* **Skater.** Position, velocity, speed, kick or jump phase and the terrain under the skater.
* **Objects.** The game's 32 object slots with world position, height and velocity.
* **Physics.** Jump power, gravity, kick power, top speed and kick start speed, changed live.
* **Cheats.** Freeze or refill the timer, add a park ticket, set cash, score and equipment level.
* **Sound.** Send any sound command to the sound CPU, replay the commands heard so far, watch the
  level of each sound chip and mute chips.
* **Memory.** Edit main RAM, keep watches (optionally frozen) and search RAM for values.

## Level editor

Run `editor720.exe`. It edits Skate City, the six layouts of each of the four parks and the
attract-mode screens, and saves your changes as a mod file in the `mods` folder next to the game.
The game loads every `.720mod` file in that folder when it starts; `--no-mods` turns them off and
`--mod FILE` adds one.

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
* **File menu.** *Save mod* (Ctrl+S), *Open mod* to continue editing, *Export map as PNG* (into
  the `exports` folder).
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
| `render720` | Run the game headless; write screenshots, a WAV of the audio (`--wav`), per-chip audio levels (`--audio-stats N`), the sound command log (`--sound-log FILE`) and per-frame RAM dumps (`--ram-dump FILE`); poke RAM with `--poke FRAME:ADDR:VALUE` |
| `ref720` | Reference model with coverage, breakpoints (`--break ADDR`) and RAM write watches (`--watch A-B --watch-frames F-G`) for reverse engineering |
| `coro_test` | Self-test of the coroutine (context switch) backend, run in CI |
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
* Platforms: Windows is the tested platform for the full game. On Linux and macOS (x86-64 and ARM64)
  everything that does not need the ROMs is built and tested in CI, including the context switch
  the translated game runs on (`port/src/native/coro_posix.cpp`); the full game has not been run
  there yet.

## Credits

Hardware behaviour follows MAME's `atarisy2` driver and device cores (BSD-3-Clause, Aaron Giles and
contributors). YM2151 synthesis uses [ymfm](https://github.com/aaronsgiles/ymfm) (BSD-3-Clause).
UI by [Dear ImGui](https://github.com/ocornut/imgui) (MIT). [SDL3](https://libsdl.org) (zlib).
720 Degrees is a trademark of its owners; this project is an unofficial fan port.
