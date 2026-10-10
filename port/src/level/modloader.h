// Loading level mods (.720mod files written by the level editor) into the ROM image used by
// the native game, and pointing the translated code's patch points at relocated data.
#pragma once
#include "leveldata.h"
#include "board.h"
#include "t11rt.h"
#include <filesystem>
#include <string>
#include <vector>

inline bool apply_mod_file(RomSet &roms, const std::string &path, std::string &err)
{
	ModPatch p;
	if (!p.load(path, err)) return false;
	p.apply(roms);
	// a mod that leaves a patch point at its default keeps what an earlier mod set
	if (p.town_alt_base != 0x6000) t11rt::g_patch_town_alt_base = p.town_alt_base;
	if (p.park_odd_base != 0xA000) t11rt::g_patch_park_odd_base = p.park_odd_base;
	return true;
}

// applies every *.720mod in 'dir' (sorted by name); returns the names applied
inline std::vector<std::string> apply_mod_folder(RomSet &roms, const std::string &dir, std::string &errors)
{
	std::vector<std::string> applied;
	std::error_code ec;
	if (!std::filesystem::is_directory(dir, ec)) return applied;
	std::vector<std::filesystem::path> files;
	for (const auto &e : std::filesystem::directory_iterator(dir, ec))
		if (e.path().extension() == ".720mod") files.push_back(e.path());
	std::sort(files.begin(), files.end());
	for (const auto &f : files)
	{
		std::string err;
		if (apply_mod_file(roms, f.string(), err)) applied.push_back(f.filename().string());
		else errors += f.filename().string() + ": " + err + "\n";
	}
	return applied;
}
