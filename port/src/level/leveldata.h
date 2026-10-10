// Level data model for 720 Degrees: decode the playfield maps from the ROM set into an
// editable form, re-encode edits, and build mod patches the native game applies at load.
//
// ROM format (see tools/levels.py for the full description):
//   map: grid of 16-bit cell words read through banked pointers (bank = ((M>>3)&0x1C00)+mapbank)
//   cell word P: banked pointer to a block (bank = ((P>>3)&0x1C00)+blockbank, addr = P&0x1FFF)
//   block: [object list, read backwards][attr0|0x80 if objects][attr1][top size][RLE top][RLE bottom]
//   each half: 16x8 tiles, run-length coded (4 modes); tile word = index|bank<<10|pal<<11|prio<<14
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <array>

struct RomSet;

struct BlockHalf {
	uint8_t attr = 0;                    // zone byte (top half: 7 bits); see LevelMap::half_bank1
	std::array<uint16_t, 128> tiles{};   // raw tile words (codes < 16 are randomised ground)
};

struct LevelBlock {
	BlockHalf half[2];
	std::vector<uint8_t> objects;        // stamp indices, in the order the game draws them
	// provenance (original ROM location)
	uint16_t orig_ptr = 0;
	bool orig_alt = false;
	bool modified = false;
	std::vector<uint8_t> orig_body;      // original header + RLE bytes (used while unmodified)
	bool operator==(const LevelBlock &o) const;
};

struct LevelMap {
	int level = 0;                       // 0 Skate City, 1-4 parks, 5-7 attract screens
	int layout = 0;                      // parks: map variant 0-5 (FE8F maps game progress to it)
	std::string name;
	bool dirty = false;                  // edited: relocate this level when building a mod
	std::vector<int> shared_layouts;     // other park layouts using the very same map
	uint16_t base = 0;                   // banked pointer of cell (0,0)
	int stride = 0;                      // bytes per map row
	int cols = 0, rows = 0;
	std::vector<int> cells;              // block index per cell (rows*cols), -1 = invalid
	std::vector<uint8_t> alt;            // town: cell uses the alternate block bank
	std::vector<LevelBlock> blocks;
	// rendering
	int tile_bank[2] = {0, 0};           // playfield tile banks selected by tile word bit 10
	bool zone_banks = false;             // Skate City: a half's zone byte selects tile bank 1
	std::array<uint16_t, 256> palette{};
	std::array<uint16_t, 16> subst{};    // ground placeholder substitution (tile codes 0..15)

	// Tile bank 1 while a block half is on screen.  In Skate City the half's zone byte
	// (BlockHalf::attr) holds the bank in bits 3+ and a switch mode in bits 0-2; the game
	// switches bank 1 as the view enters zoned halves.  Halves with zone 0 keep the current
	// bank (none of the original ones use bank 1 tiles).
	int half_bank1(const BlockHalf &h) const
	{
		const int z = (h.attr >> 3) & 15;
		return zone_banks && z ? z : tile_bank[1];
	}

	int cell(int cx, int cy) const { return cells[cy * cols + cx]; }
	uint16_t cell_ptr(int cx, int cy) const { return (uint16_t)(base + cy * stride + cx * 2); }
};

// a set of byte patches to apply on top of the original main CPU ROM image
struct ModPatch {
	uint32_t required_size = 0x90000;    // main region size including extension pages
	struct Chunk { uint32_t offset; std::vector<uint8_t> data; };
	std::vector<Chunk> chunks;
	// translated-code patch points (see port/src/native/patchpoints.h)
	uint16_t town_alt_base = 0x6000;     // E8B6: block bank for the town's alternate cells
	uint16_t park_odd_base = 0xA000;     // E8B0: block bank for odd block pointers in parks
	bool save(const std::string &path) const;
	bool load(const std::string &path, std::string &err);
	void apply(RomSet &roms) const;
};

class LevelSet {
public:
	bool load(const RomSet &roms, std::string &err);
	std::vector<LevelMap> maps;          // town, parks x classes, screens
	// banks behind the translated code's patch points; set from a mod before load() to
	// decode a modded ROM image
	uint16_t town_alt_base = 0x6000, park_odd_base = 0xA000;

	// decode helpers (exposed for tools)
	static int rom_bank_offset(uint16_t bankreg);          // ROM offset of an 8K page selected by bank register value
	// encode a half; returns RLE bytes
	static std::vector<uint8_t> encode_half(const BlockHalf &h);
	// replace the rendering palette of every map of a level (captured from the running game)
	void set_palette(int level, const uint16_t palette[256]);
	// build a mod patch for all maps that contain modifications
	ModPatch build_patch(const RomSet &original, std::string &report) const;

private:
	const RomSet *m_roms = nullptr;
	uint16_t r16(uint32_t off) const;
	bool decode_block(LevelBlock &b, int level, uint16_t p, bool alt, std::string &err) const;
};

// tile rendering helpers
uint32_t level_palette_rgb(uint16_t raw);
