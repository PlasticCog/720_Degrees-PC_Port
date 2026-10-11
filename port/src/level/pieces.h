// Pieces for the level editor: the structures of the original maps (ramps, ledges, pits,
// walls, ground markings) cut out as stamps that can be placed again, grouped by what they
// are in the game.
//
// What a tile is comes from its terrain byte, which the game looks up by tile code (ROM bank
// 0x0800, one byte per code; bits 3-6 height level with 3 = ground, bits 0-2 slope type,
// bit 7 a flagged tile).  Ground placeholders (tile codes 0-15, varied by the game) on flat
// ground are the open floor between structures.  A structure is a connected group of other tiles; small
// structures stay whole (a ramp with its deck and sides), large ones are cut where the kind
// of terrain changes and, if still large, into block-sized parts.
#pragma once
#include "leveldata.h"
#include <cstdint>
#include <string>
#include <vector>

struct RomSet;
struct GfxSet;

enum class PieceKind : uint8_t { Ramp, Ledge, Pit, Wall, Marking, Floor, Count };
const char *piece_kind_name(PieceKind k);          // "Ramps and slopes", ...

constexpr uint16_t PIECE_EMPTY = 0xFFFF;            // a cell of the piece's box it does not cover

struct Piece {
	int w = 0, h = 0;                               // size in tiles
	std::vector<uint16_t> tiles;                    // w*h raw tile words (PIECE_EMPTY: not part of it)
	std::vector<uint8_t> bank1;                     // graphics bank of tile-bank-1 words (Skate City zones)
	PieceKind kind = PieceKind::Marking;
	int uses = 0;                                   // occurrences in the maps it was taken from
	int map = 0, x = 0, y = 0;                      // first occurrence (map index, tile position)
	bool whole = false;                             // a prefab: a whole object (extract_prefabs)
	bool section = false;                           // a prefab cut from a long structure (2 x 2 blocks)
};

// terrain byte of a raw tile word as the game sees it (placeholders resolved first)
int tile_terrain(const RomSet &roms, const LevelMap &lm, uint16_t w, int bank1);
PieceKind terrain_kind(int terrain);

// the pieces of the given maps (one level: they share tile banks and colours); map indices in
// Piece::map refer to positions in 'maps'.  Sorted by kind, then by size (largest first).
std::vector<Piece> extract_pieces(const RomSet &roms, const std::vector<const LevelMap *> &maps);

// prefabs: the whole objects of the given maps (ramps, pools, planters, platforms, ...): every
// connected group of non-floor tiles up to 3 x 3 blocks, after the long networks it may touch
// (the curbs along the streets, long walls: a part of one kind and colour longer than that) are
// taken off; those long structures (park courses, curbs) give sections of 2 x 2 blocks on the
// block grid.  Sorted by kind, then by how often the maps use them.
std::vector<Piece> extract_prefabs(const RomSet &roms, const std::vector<const LevelMap *> &maps);
const char *prefab_kind_name(PieceKind k);         // "Ramp or slope", ...

// ARGB pixels (w*8 x h*8) of a piece in the colours of lm; uncovered cells are transparent
void render_piece(const GfxSet &gfx, const LevelMap &lm, const int8_t random_var[32], const Piece &p,
                  std::vector<uint32_t> &px);
