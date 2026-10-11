// Extras: optional changes to the original game, switched in the debug menu's "Extras" tab.
// All off is the original game.  They bring back content the game never uses and fix bugs
// found while rewriting it (docs/programming-tricks.md, the "Secrets" and "Bugs" sections).
//
// Unused content
//   dev_pause      the developers' freeze / frame step: IN0 bits 0 and 2, which the game reads
//                  (F:B6BC) but the cabinet does not wire up (F7 / F8 in the game, see DebugUI)
//   point_flags    the hidden points (types 72-82, 50 ... 1000 points; type props bit 6 makes
//                  F:C5xx show them without sprite pieces) become visible: the unused blue
//                  100-1000 flags (frames 0x265B-0x26BE, 3 pieces) and, for 50, its sign frame
//   cut_cast       the unused pedestrians: the man in suspenders (frames 0x314A-0x3209, his
//                  jogging cycle and the knocked-flat frame) takes over the unicyclists' (type
//                  97) animations, the businessman (0x3224-0x3283, walking) the tricycle kids'
//                  (type 50, palette 1).  Both only have front views (down-left / down-right)
//   hex_labels     an overlay: each object labelled with its number in the unused hex-digit
//                  sprites (frames 0x2085-0x20D0); DebugUI draws it
// Fixes
//   fix_subpixel   A7:4098 loses the velocity fraction carry (SWAB clears C before the ADC)
//   fix_roller     the roller skater of stages 2, 5, 6 and 7 (type 51, descriptor 0x02E1) names a
//                  cell without object places and never appears; 0x02E5 is the nearest place
//                  next to the other roller skaters that is one bit away (our guess at the typo)
//   fix_hazard     B3:7CB0 looks up the "jumping over hazards" bonus (table 0x45B1) and drops it
//   fix_dir3       direction 3 of the flat movement table (bank 0x2C00 0x6048 / 0x6588) is
//                  (7088, 9737) where its mirror images are (+/-7292, +/-9700)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct RomSet;

struct ExtrasSettings {
	bool dev_pause = false, point_flags = false, cut_cast = false, hex_labels = false;
	bool fix_subpixel = false, fix_roller = false, fix_hazard = false, fix_dir3 = false;

	std::string save() const;              // one "name 0|1" per line
	void load(const std::string &text);
};

class Extras {
public:
	ExtrasSettings s;
	std::string problems;                  // patches skipped because the ROM differs (e.g. a mod)

	// bring the ROM image the game runs from and the rewritten code's fix flags in line with s;
	// ROM patches are undone when switched off
	void apply(RomSet &roms);

	// the developer message stored in one program ROM chip (0: 136047-3128.6fh, 1: -4131.6mn)
	// at 0x8FD6, and how the CPU sees the two interleaved
	static std::string rom_message(const RomSet &roms, int chip);
	static std::string cpu_view(const RomSet &roms);

private:
	struct Patch { size_t offset; std::vector<uint8_t> original, data; };
	struct Group { std::vector<Patch> patches; bool built = false, ok = false, applied = false; };
	enum { POINT_FLAGS, CUT_CAST, ROLLER, DIR3, GROUPS };
	Group m_groups[GROUPS];

	void build(int group, const RomSet &roms);
	void set(int group, bool on, RomSet &roms);
};
