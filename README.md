# 720 Degrees - native PC port

A native port of Atari Games' **720 Degrees** (1986 arcade, rev 4) for Windows, Linux and macOS, written in C++ with SDL3,
plus a level editor. Both of the arcade board's programs, the main program (`port/src/logic`) and
the sound program (`port/src/sound`), are newly written C++, checked instruction by instruction
against the original programs; the game and the editor contain no translated program code.
Graphics, sound chips and controls are reimplemented natively. No CPU emulator runs inside the
game.

> You need your own copy of the arcade ROMs: the game reads its graphics, sounds and data from
> them. Nothing derived from the ROMs is stored in this repository: the translated program code,
> which only the test tools use, is generated on your machine from your ROM files.

## Quick start

Ready-made builds for Windows (x64), Linux (x86-64, ARM64) and macOS (Apple Silicon, Intel) are
on the [Releases](https://github.com/PlasticCog/720_Degrees-PC_Port/releases) page: unpack the
zip, put your ROM files (or `720.zip`) into its `Game` folder and run `720degrees`
(`720degrees.exe` on Windows). The programs are not signed: on macOS, clear the download
quarantine first (`xattr -dr com.apple.quarantine <unpacked folder>`) or open them once with
right-click > Open. To build it yourself:

1. Put the 37 ROM files of the MAME set `720` (rev 4) into the [`Game`](Game/README.txt) folder, or just
   drop the MAME `720.zip` there; it is unpacked automatically.
2. Install a C++20 compiler, CMake 3.20+, Ninja and Python 3 (details per platform below).
3. Build:

   ```sh
   cmake -S port -B port/build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build port/build
   ```

   Configuration translates the program ROMs to C++ (`tools/t11_to_cpp.py`,
   `tools/m6502_to_cpp.py`) for the tools that compare the rewrite with the original; the game
   and the editor do not use it and also build without the ROMs (`-DD720_TRANSLATION=OFF`, or
   automatically when no ROMs are found). On Windows it downloads the SDL3 development package if
   SDL3 is not installed; on Linux and macOS it builds SDL3 from source as a static library.
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
third-party licences into `dist/720degrees-<version>-<platform>.zip`. The game and the editor
contain no translated program code. A normal build can still include data generated from your
ROMs (`port/src/level/level_palettes.inc`, if present); configure with `-DD720_ROM_FREE=ON`, as
the release builds are, to leave it out:

```sh
cmake -S port -B port/build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DD720_ROM_FREE=ON
cmake --build port/build-release --target 720degrees editor720
python tools/package.py --build-dir port/build-release
```

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
| Start a one / two player game | 1 / 2 | any button: one player | any button: one player |
| Pause | P | Menu | |
| Debug menu / controls / help | Tab / F4 / F1 | | |
| Service mode / reset | F2 / F3 | | |
| Fullscreen / windowed | F, F11 or Alt+Enter | | |

Any controller SDL recognises works; the button names follow the Xbox layout. Controllers can be
connected and removed while the game runs.

On the title screens the arcade game starts two players when Jump is pressed with two credits in.
Here only the 2 key starts a two-player game; Jump, Kick, the mouse, the gamepad and the 1 key
start one player (in the continue countdown the start keys continue).

High scores and settings are kept in the game's EEPROM image, saved in the same settings folder
as `720.eeprom`. DIP switches: `--dsw0 0xNN --dsw1 0xNN`.

## Debug menu

Press **Tab** in the game. The menu has these tabs:

* **Game.** Pause, single-frame steps, game speed, machine reset, hiding the playfield, sprite
  or text layer, and the menu size (100-250 %, also for the help and controls windows; saved).
* **Overlays.** Sprite boxes with their screen coordinates, and a coloured mask of the terrain
  height map. Hover over the game to read the tile and terrain value under the mouse.
* **Skater.** Position, velocity, speed, kick or jump phase and the terrain under the skater.
* **Objects.** The game's 32 object slots with world position, height and velocity.
* **Physics.** Jump power, gravity, kick power, top speed and kick start speed, changed live.
* **Cheats.** Freeze or refill the timer, add a park ticket, set cash, score and equipment level;
  no water and no bushes / thorns, which are drawn as the floor around them.
* **Sound.** Send any sound command to the sound CPU, replay the commands heard so far, watch the
  level of each sound chip and mute chips.
* **Memory.** Edit main RAM, keep watches (optionally frozen) and search RAM for values.
* **Extras.** Content the original has but never uses, and fixes for its bugs, all off by default
  and saved: the developers' hidden freeze / frame-step inputs (F7 / F8), the hidden points shown
  as the unused point flags, the two cut pedestrians walking the town, objects labelled with the
  unused hex-digit sprites, the messages hidden in the ROM chips; fixes for the lost sub-pixel
  movement, the roller skater who never appears, the dropped bonus for jumping over hazards and a
  lopsided movement direction.
* **Tricks.** How the original game works: its programming tricks (isometric movement, masking,
  ramps, spins, sprites, interrupts), hidden content and bugs. It shows
  [docs/programming-tricks.md](docs/programming-tricks.md), built into the game, with a search box
  and a larger pop-out window.

## Level editor

Run `editor720.exe`. It edits Skate City, the six layouts of each of the four parks and the
attract-mode screens, and saves your changes as a mod file in the `mods` folder next to the game.
The game loads every `.720mod` file in that folder when it starts; `--no-mods` turns them off and
`--mod FILE` adds one.

* **Map.** Click a cell to select it. In *Paint blocks* mode, drag to paint the brush block.
  Right-drag pans, the wheel zooms, middle-click picks up the block under the cursor and a
  double-click opens a block in the block editor.
* **Blocks.** Every block of the level, in the order the map uses them (or by number), with how
  often it is used. Click one to make it the brush. *Make unique* gives the selected cell its own
  copy, so that editing it does not change other cells.
* **Block editor.** A block is 16 x 16 tiles in two halves. Left-drag paints the brush tile and
  right-click picks up a tile. Each half also has a zone byte: in Skate City its upper bits select
  the tile set used for "bank 1" tiles while that part of the town is on screen.
* **Library.**
  * *Prefabs*: whole objects from the level's original maps (ramps, slopes, pools, platforms,
    walls, markings) and 2 x 2-block sections of long ones (park courses, curbs). Click one, then
    click on the map to place it (the Stamp tool) or in the block editor.
  * *Parts*: smaller pieces cut from the original maps' structures.
  * *Tiles*: each kind's tiles where the maps use them, in blocks of the level with the other
    tiles dimmed; click a tile to paint with it.
  * *All tiles*: the raw tile sheet; choose the bank, palette and priority bits of the brush here.
    Tiles 0 to 15 are ground placeholders, which the game replaces with varied ground tiles.
  * *Sprites*: every sprite frame and object type.
* **File menu.** *Save mod* (Ctrl+S), *Open mod* to continue editing, *Export map as PNG* (into
  the `exports` folder).
  **Play test** (F5) starts the game with your current edits only. Undo and redo are Ctrl+Z / Ctrl+Y.

On its first start the editor runs the game headless for a few seconds to capture the level
colours. The random-input run does not reach the parks, so parks are shown with Skate City's palette.

Mods that edit different levels can be combined. Editing any park rewrites all four parks, because
they share one block bank, so two park mods replace each other.

## How it works

```
Game/ (your ROMs) --tools/*.py--> port/src/game/*.cpp   translated main (T-11) + sound (6502)
                                                        programs, for the test tools only
port/src/logic    the main program rewritten: 317 routines, instruction by instruction
port/src/sound    the sound program rewritten: 98 routines, instruction by instruction
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
* **Rewrite.** Every routine of both programs has been rewritten: the main program in
  `port/src/logic`, the sound program in `port/src/sound`. Each one does what the original's
  instructions do, one instruction at a time: the same memory accesses, registers, condition
  codes, stack contents and cycles, with interrupts let in at the same instruction boundaries.
  `render720 --difftest` runs every call both ways (the rewrite and the translated original) and
  compares the results and every step, for the sound CPU also every access to the sound chips
  and latches; `tools/difftest_all.py` does that over 17 scenarios (attract mode, random play,
  each park, events, tricks, self test, high score tables and new entries, two players). `tools/compare_runs.py`
  compares whole runs frame by frame (RAM including the stack, screens, sound commands, the
  sound CPU's RAM and every chip and latch access with its time); they are byte-identical. The
  game and the editor are built without any translated code (`gamecore_native`): both CPUs run
  only the rewrite.
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
| `render720native` | `render720` built like the game, without any translated code |
| `tools/difftest_all.py` | Difference test of the rewritten routines against the original over all scenarios |
| `tools/compare_runs.py` | Whole runs of the rewrite against the original, frame by frame (`--exe render720native --original-exe render720` for the game's build) |
| `render720` | Run the game headless; write screenshots, a WAV of the audio (`--wav`), per-chip audio levels (`--audio-stats N`), the sound command log (`--sound-log FILE`) and per-frame RAM dumps (`--ram-dump FILE`) and palette dumps (`--pal-dump FILE`); `--hide-pf` / `--hide-alpha` leave layers out of the screenshots; poke RAM with `--poke FRAME:ADDR:VALUE` |
| `ref720` | Reference model with coverage, breakpoints (`--break ADDR`) and RAM write watches (`--watch A-B --watch-frames F-G`) for reverse engineering |
| `coro_test` | Self-test of the coroutine (context switch) backend, run in CI |
| `editor720` | Level editor; `--capture-test` and `--screenshot FILE` for automated checks |
| `leveltool` | `stats`, `png DIR` (render all maps), `zones` (zone bytes per map), `roundtrip [LEVEL]` (patch, re-decode and compare), `identity FILE` / `demo-mod FILE` (test mods); sprites and map objects (`port/src/level/sprites.h`): `sprites DIR` (sheets of all frames, types, listing), `objects LEVEL OUT.png [STAGE] [PALDUMP FRAME]` (map with its objects), `objcheck RAMDUMP` / `objshot RAMDUMP PALDUMP FRAME OUT.png` (compare with the running game), `objedit-test FILE` (object list encoder) |
| `tools/listing.py` | Annotated disassembly of the main program (`re/main_t11.lst`) |
| `tools/gen_pdp11_sleigh.py` | Ghidra processor module for the DEC T-11 (`re/ghidra/PDP11`) |
| `re/ghidra/scripts/*.java` | Ghidra headless scripts: decompile the main and sound programs |

## Status

* Main and sound programs rewritten and verified against the original; the game and the editor
  run without any translated code. A few paths that no test scenario reaches (for example the
  self test's fault reports, some music commands and three rarely used sound board commands)
  follow the original but have not been compared with it. Video, POKEY, YM2151 and TMS5220 audio done.
* Level editor: all maps decode and re-encode exactly. An unedited mod plays pixel-identically to
  the original, and edits show up in the game.
* Still open: the park palettes, which the colour capture does not reach, and the exact meaning of
  the zone switch modes.
* Platforms: Windows is the tested platform for the full game. On Linux and macOS (x86-64 and ARM64)
  the game, the editor and the tools are built in CI, which has no ROMs, and the context switch the
  game runs on (`port/src/native/coro_posix.cpp`) is tested there; the full game has not been run
  on these systems yet. The release builds for them are made by `.github/workflows/release.yml`.

## Credits

Hardware behaviour follows MAME's `atarisy2` driver and device cores (BSD-3-Clause, Aaron Giles and
contributors). YM2151 synthesis uses [ymfm](https://github.com/aaronsgiles/ymfm) (BSD-3-Clause).
UI by [Dear ImGui](https://github.com/ocornut/imgui) (MIT). [SDL3](https://libsdl.org) (zlib).
720 Degrees is a trademark of its owners; this project is an unofficial fan port.
