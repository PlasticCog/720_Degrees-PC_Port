// Finding the ROM files, and a screen that installs them when they are missing.
//
// The game reads the arcade ROM set (MAME '720', rev 4: 36 ROM files and 720-eeprom.bin), as
// loose files or as MAME's 720.zip.  When they are not found, the setup screen explains this
// and lets the player pick the zip or the files with the system's file dialog (on Android the
// document picker).  They are copied into the game's ROM folder and loaded.  Loose files are
// recognised by size and CRC-32 (the values MAME lists for the set), so their names do not
// matter: Android's picker does not tell the app what a file is called.
#pragma once
#include <SDL3/SDL.h>
#include <string>
#include <vector>

struct RomSet;

// the folders searched for the ROMs, best first
std::vector<std::string> rom_folders();
// 'requested' if given, else the first folder that has the ROMs, else the install folder
std::string find_roms(const std::string &requested);
// where picked files are copied to
std::string rom_install_folder();

// runs the setup screen (Dear ImGui must be set up) until the ROMs load (true; 'romdir' is
// where they are) or the player quits (false)
bool rom_setup_screen(SDL_Window *window, SDL_Renderer *renderer, RomSet &roms, std::string &romdir, std::string message);
