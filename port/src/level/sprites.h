// Sprites (motion objects) and map objects of 720 Degrees, decoded from the ROM set for the
// level editor: every frame the animation data can show, the animations, the object types and
// the objects each level places, with an encoder for in-place edits of the object lists.
//
// ---------------------------------------------------------------------------------------------
// Frame data (bank registers 0x3000 / 0x3400: 16 KB at CPU 0x4000-0x7FFF, ROM entries 15 / 11)
//   A frame is addressed by its offset F (0x0000-0x3FFF) into that area; F is the frame id here.
//     byte 0  x offset of the first column (signed, pixels)
//     byte 1  bits 0-2: picture bank (bits 11-13 of every piece's picture code);
//             bit 7 set: one y offset byte follows and holds for all columns
//     then per column: [y offset byte, unless bit 7] picture word (little endian):
//             bits 0-10 picture code, 11-13 height - 1 (in 16x16 tiles), 14 h-flip,
//             15 last column of the frame
//   Columns are 16 pixels wide and placed left to right (motion object "next to the last one");
//   a column's bottom edge is y offset pixels above the object's position (motion object y
//   counts upwards).  F:B1EA builds the motion objects of an object from its current frame and
//   palette byte (0x0470: bits 4-5 palette, 6-7 priority); unused pieces of the object's chain
//   (piece count per type, table 0x5C96) get picture 0.
//   Objects whose type has flag 0x01 (table 0x649D) move by the 3 bytes in front of each frame
//   (x and y velocity * 16, a counter adjustment) as the animation steps (F:B378).
//   The frames are packed back to back; the ones no animation refers to are kept as unused.
//
// Animation data (bank A register 0 = ROM entry 12, CPU 0x4000-0x5BCB)
//   An object's animation pointer (0x03B0 + 2 * slot) addresses a word of a list:
//     word < 0x4000            frame id
//     word 0x4000-0x7FFF       pointer to another list (direction rings, see below)
//     word >= 0x8000           link: continue at (word & 0x7FFF); ends every looping list
//   F:B378 steps to the next word every `rate` frames (0x0770) and follows links.
//   Direction rings: types without animation (rate 0) keep one frame per direction in a ring
//   (adjacent directions in adjacent words, closed by a link); animated ones keep list pointers
//   there.  F:C114 turns an object by stepping through its ring.
//   The whole area holds only frame ids, pointers and links, so every frame id in it is a frame
//   the game can show.
//
// Object types (101; bank A 0 / bank B 0x0400)
//   0x5F7C+2t animation: props bit 7 -> list address, else direction table (entry for
//             direction d at + byte 0xFDBA[d]: a frame (ring position) or, when the type has a
//             rate, a list pointer)
//   0x6046+2t second animation (F:D47C start_object), 0x6110+2t collision boxes (per direction)
//   0x61DA+t  -, 0x623F+t behaviour mode (F:CB24), 0x62A4+2t / 0x636E+2t x / y offset (world)
//   0x6438+t  props: bit 7 fixed animation, bits 4-5 palette, bit 6 without sprite pieces,
//             bit 3 removed when off screen, bit 2 one-shot animation, bit 1 shared definition
//   0x649D+t  flags: bit 0 velocity from frames, bits 4/6 counted, bit 7 no timer
//   0x6502+t  animation rate (frames per step), 0x5C96+t sprite pieces, 0x6567+t sound
//
// Sprite palette: palette RAM 0x1000-0x107F (MO palettes 0-3, 16 colours, pen 15 transparent)
//   is loaded by F:DB4E from bank B 0x0400 0x7F2C + 0x20 * p, each group of 16 in reverse
//   order (ROM word k = pen 15 - k).  This matches the palette RAM captured during play in the
//   town and the parks (SpriteSet::palette).  The attract / results screens load colour sets 0
//   and 2 into palettes 0 and 1 (F:DC3C from bank 0x1C00 0x7F0C + 0x20 * set; screen_palette,
//   also matching captured palette RAM); player 2's skater uses palette 0 from bank 0x1C00
//   0x7F2C (F:DB4E with 0x01F5 set; player2_palette, not captured).
//
// Map objects (Skate City only; the parks place none)
//   descriptor: bits 0-3 cell row, bits 4-8 cell column, bits 9-14 definition offset.  A cell is
//   2 x 2 map blocks (256 x 256 pixels); the town has 24 x 12 cells.  The cell's first
//   definition is in 0x7A00 + 2 * cell (fixed objects) or 0x7D00 + 2 * cell (dynamic ones,
//   definitions + 0x0338) in bank B 0x1400, 0xFFFF = none.
//   definition d: x 0x4AC0 + 2d, y 0x5580 + 2d (bank A 0x1000; world units, 64 per pixel, both
//   0..0x3FFF inside the cell), properties 0x4560 + d; fixed definitions also have a record at
//   0x6040 + 8d (bank B 0x1400): {next descriptor, direction, speed, alternative next
//   descriptor, direction, speed} - the waypoints walking / driving objects follow.
//   Position in the level map (pixels, the same coordinates as LevelMap blocks of 128 pixels):
//     x = 256 * column + (def x + type x offset) / 64
//     y = 256 * (row + 1) - (def y + type y offset) / 64
//   (world x = 0x4000 * column + def x, world y = 0x6000 - 0x4000 * row + def y, both mod
//   0x10000; F:BF5E map_object_position.  The town's rows wrap with a shift of 5 cells, which
//   only matters for world coordinates.)
//   These are the object's position (hot spot); its frame's pieces are drawn relative to it.
//   Lists (type word, descriptor word, ..., 0xFFFF; bank B 0x0400):
//     0x6696      dynamic objects added when the town is set up (F:CFD6): pennants, ticket
//                 booths, signs, discs, the speakers
//     0x70E4/70E8 four fixed objects (type bytes, descriptor words; F:D19C): the park gates
//     0x6B9E+2s   per town stage s = 0x0F2E (1-7; 6 and 7 share a list): pointer to a list of
//                 fixed objects (cars, people); one entry (stages 2, 5-7, descriptor 0x02E1)
//                 names a cell without definitions and is skipped by the game
//     0x708C+8g   four type-1 objects per group g = 0xFDE3[s] (scripted skaters, F:DF40)
//     0x70C4/D4/DC park entries and 0x70AA/BB/BF ticket booths: descriptor -> type when closed /
//                 open; the town list objects with these descriptors get the type (F:D880/D8D2)
//     spawns: per stage the counts of kinds 1-9 (0x68D0+2s -> 9 bytes), their types
//     (0x6862+2s -> 10 bytes; kinds 11-19: 0x6935+kind), candidate descriptors per kind
//     (0x6948+2*kind -> list of 0xFD95[kind]+1 words; F:D35E picks one at random)
//   Attract / results screens (levels 5 and 6): up to 5 objects from 0x7D94 (types), 0x7D9A
//   (lists), 0x7DA4 / 0x7DAE (world x / y), count 0xFF2A[screen] (F:EE6E); map x = world x / 64,
//   map y = 128 - world y / 64 (mod 512), the same relation as in the town.
//   Editing: the lists are read from ROM at run time, so a mod patch can change them in place:
//   types, descriptors, removing entries (the town list keeps its 37 entry capacity; the stage
//   lists can be repacked into the space all of them use, with the pointer table 0x6B9E
//   updated) and the definition positions (moves every object using that definition).  Adding
//   definitions or growing the town list would need code changes (fixed addresses).
//
// Names: the categories and names come from where a frame first appears in the animation data
// (the data is grouped by object) and from looking at the rendered frames; skater / board
// tables of 16 (8) directions are named "dir N".  Object type names follow their frames.
//
// Verification (leveltool sprites / objects / objcheck / objshot / objedit-test against
// render720 runs; see leveltool_sprites.cpp): every frame the game showed in 7 scenarios
// (random play, the four parks) is in the set; the motion object RAM of every shown object
// equals its decoded frame at its position except in transient states (pieces not built yet,
// rebuild pending, y lag of objects re-entering the draw list); per-object pixel comparisons
// with sprite-only screenshots are exact for objects that did not move during the frame; the
// palettes equal the captured palette RAM; static map objects (speakers, gates, signs, discs,
// pennants, hidden points) sit exactly at the decoded positions in game RAM and on screen.
//
// Use: SpriteSet::load(roms) once; frames / animations / types for the browser, render() or
// draw() to show a frame; objects (with lists, spawns, entry_types) for the map; lists +
// encode_lists() / encode_definition_position() -> ModPatch chunks to save edits.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct RomSet;
struct GfxSet;

namespace sprites {

// CPU view of the main program ROM with the bank registers at (bank_a, bank_b)
struct RomView {
	const std::vector<uint8_t> *main = nullptr;
	uint16_t bank_a = 0x0000, bank_b = 0x0400;
	int64_t offset(uint16_t addr) const;   // offset in RomSet::main, -1 for RAM / IO
	uint8_t b(uint16_t addr) const;
	uint16_t w(uint16_t addr) const;
};
int64_t bank_offset(uint16_t bankreg);     // RomSet::main offset of the 8 KB page a bank value selects

enum class Category {
	Skater, Helmet, Skateboard, Chaser, Vehicle, Person, Item, Effect, Sign, Fixture, Screen, Blank,
	Unused, Count };
const char *category_name(Category c);

struct Piece {
	int x = 0, y = 0;        // top-left corner relative to the hot spot (pixels, y down)
	int code = 0;            // picture code of the top tile (GfxSet::sprites index)
	int tiles = 1;           // column height in 16x16 tiles (codes code .. code + tiles - 1)
	bool hflip = false;
};

struct Frame {
	uint16_t id = 0;                     // offset in the frame data
	uint32_t rom_offset = 0;             // RomSet::main offset of byte 0
	int size = 0;                        // bytes
	int8_t x_offset = 0;
	uint8_t mode = 0;
	std::vector<Piece> pieces;
	int left = 0, top = 0, right = 0, bottom = 0;   // bounding box (right / bottom exclusive)
	bool motion = false;                 // used by a type that moves by frame velocities
	int8_t vx = 0, vy = 0;               // velocity bytes in front of the frame (* 16 per step)
	uint8_t step = 0;
	bool referenced = false;             // in the animation data (false: unused frame)
	uint16_t first_ref = 0;              // lowest animation data address referring to it
	Category category = Category::Unused;
	std::string name;
	int palette = 0;                     // MO palette the game shows it with (0-3)
	bool screen_colours = false;         // shown on the attract / results screens (screen_palette)
	std::vector<int> types;              // object types whose animations use it
	int width() const { return right - left; }
	int height() const { return bottom - top; }
};

struct Animation {
	uint16_t addr = 0;                   // CPU address (bank A 0) of the first word
	std::vector<uint16_t> frames;        // frame ids in display order
	int loop = -1;                       // index the list continues at after the last frame
	uint16_t next = 0;                   // link target when it continues in another list
	int rate = 0;                        // frames per step (from the type; 0 = not animated)
	std::string name;
};

struct ObjectType {
	int type = 0;
	std::string name;
	Category category = Category::Unused;
	uint8_t props = 0, flags = 0, mode = 0, rate = 0, pieces = 0, sound = 0;
	int palette = 0;                     // props bits 4-5
	int16_t off_x = 0, off_y = 0;        // world units (64 per pixel)
	uint16_t anim = 0, anim2 = 0, boxes = 0;
	bool directional = false;            // anim is a direction table
	std::array<uint16_t, 16> dir_entry{};   // directional: address of the table entry (0 = none)
	std::array<uint16_t, 16> dir_list{};    // list shown for direction d (animated types)
	std::array<int, 16> dir_frame{};        // first frame for direction d (-1 = none)
	int first_frame = -1;                // non-directional: first frame
	std::vector<uint16_t> frames;        // every frame its animations reach
};

struct Definition {
	int index = -1;
	uint16_t x = 0, y = 0;               // world offsets inside the cell
	uint8_t props = 0;
	bool has_record = false;
	uint16_t next[2] = {0, 0};           // record: next descriptors (waypoints)
	uint8_t dir[2] = {0, 0}, speed[2] = {0, 0};
	uint32_t x_rom = 0, y_rom = 0;       // RomSet::main offsets of the position words
};

enum class ObjectSource { TownList, FixedFour, StageList, StageSkaters, Screen };
const char *source_name(ObjectSource s);

struct MapObject {
	ObjectSource source = ObjectSource::TownList;
	int stage = 0;                       // town stage (0x0F2E) for stage lists, else 0
	int index = 0;                       // entry index in its list
	int level = 0;                       // LevelMap level (0 Skate City, 5-7 screens)
	int type = 0;
	uint16_t descriptor = 0;
	bool dynamic = false;                // cell table 0x7D00, definitions + 0x0338
	int cell_col = 0, cell_row = 0, def_offset = 0;
	int definition = -1;                 // -1: the cell has no definition
	int world_x = 0, world_y = 0;        // after the type offsets
	double map_x = 0, map_y = 0;         // level map pixels of the hot spot
	int direction = -1;                  // initial direction (record), -1 if not directional
	uint16_t anim = 0;                   // initial animation list address
	int frame = -1;                      // initial frame id
	uint32_t rom_offset = 0;             // RomSet::main offset of the entry (type word / byte)
	std::string note;
};

// park entries and ticket booths: the town list objects with these descriptors get their type
// when the town is set up (F:D880 / F:D8D2), by whether the park is open / the ticket is held
struct EntryType {
	uint16_t descriptor = 0;
	int closed_type = 0, open_type = 0;
	int index = 0;                       // park (F:D880: entry / 2) or ticket (F:D8D2: 1-4)
	bool ticket = false;
};

struct SpawnKind {
	int kind = 0;
	uint16_t list = 0;                   // candidate descriptor list (bank B 0x0400)
	std::vector<uint16_t> descriptors;
	std::array<int, 8> count{};          // per stage 1-7 (kinds 1-9)
	std::array<int, 8> type{};           // per stage (kinds 1-10), or the fixed type (11-19)
	bool dynamic = false;                // kinds 1-10 are dynamic map objects
};

// one editable list as the game reads it (bank B 0x0400)
struct ObjectList {
	ObjectSource source = ObjectSource::TownList;
	int stage = 0;
	uint16_t addr = 0;                   // CPU address of the first entry
	uint32_t rom_offset = 0;
	int capacity = 0;                    // entries that fit in place (without the terminator)
	std::vector<std::pair<uint16_t, uint16_t>> entries;   // (type, descriptor)
};

class SpriteSet {
public:
	bool load(const RomSet &roms, std::string &err);

	std::vector<Frame> frames;           // sorted by id
	std::vector<Animation> animations;   // sorted by address
	std::vector<ObjectType> types;       // index = type number (0-100)
	std::array<std::array<uint16_t, 16>, 4> palette{};   // MO palettes during play (raw colour words)
	std::array<std::array<uint16_t, 16>, 4> screen_palette{};   // attract / results screens
	std::array<uint16_t, 16> player2_palette{};           // palette 0 for player 2's skater

	const Frame *frame(uint16_t id) const;
	const Animation *animation(uint16_t addr) const;
	// frames of the list starting at addr (follows links until a frame repeats; max 64)
	std::vector<uint16_t> list_frames(uint16_t addr, int max = 64) const;

	// render a frame into RGBA (bytes R, G, B, A in memory order); the hot spot is at
	// (-frame.left, -frame.top).  pal16: 16 raw colour words (default: the frame's own palette)
	std::vector<uint32_t> render(const GfxSet &gfx, const Frame &f, const uint16_t *pal16 = nullptr) const;
	// draw a frame with its hot spot at (x, y) into an ARGB8888 image (as write_png expects)
	void draw(const GfxSet &gfx, const Frame &f, uint32_t *argb, int w, int h, int x, int y,
		const uint16_t *pal16 = nullptr) const;

	// ---- map objects
	std::vector<MapObject> objects;      // every placed object (town lists, stages, screens)
	std::vector<SpawnKind> spawns;
	std::vector<EntryType> entry_types;
	std::vector<ObjectList> lists;       // the editable lists (town list, stage lists)
	int stages = 7;

	Definition definition(int index) const;
	// first definition and count of a cell (dynamic: table 0x7D00); count 0 = none
	void cell_definitions(int col, int row, bool dynamic, int &first, int &count) const;
	// position of a descriptor (+ type offsets) in level map pixels; false without definition
	bool descriptor_position(uint16_t desc, bool dynamic, int type, double &x, double &y) const;
	// the inverse for placing: cell and in-cell world offsets (0..0x3FFF, clamped) of a level map
	// position for an object of the type; a definition moved there shows it at (x, y)
	void map_to_cell(double x, double y, int type, int &col, int &row, uint16_t &def_x, uint16_t &def_y) const;
	static uint16_t make_descriptor(int col, int row, int offset)
	{
		return (uint16_t)(((offset & 0x3F) << 9) | ((col & 0x1F) << 4) | (row & 0x0F));
	}

	// ---- editing: byte patches (RomSet::main offset, bytes) for the ModPatch mechanism
	struct Patch { uint32_t offset; std::vector<uint8_t> data; };
	// the town list in place (at most its capacity), or all stage lists repacked into their
	// combined space with the pointer table; false (err) when they do not fit
	bool encode_lists(const std::vector<ObjectList> &edited, std::vector<Patch> &out, std::string &err) const;
	// move a definition (world offsets inside its cell, 0..0x3FFF): patches of its x and y words
	std::vector<Patch> encode_definition_position(int def, uint16_t x, uint16_t y) const;

private:
	const RomSet *m_roms = nullptr;
	RomView anim_view() const;
	void decode_frames();
	void decode_types();
	void name_frames();
	void collect_animations();
	void decode_objects();
	MapObject place(ObjectSource src, int stage, int index, int type, uint16_t desc, bool dynamic, uint32_t rom) const;
};

}   // namespace sprites
