// Sprites and map objects of 720 Degrees: see sprites.h for the formats.
#include "sprites.h"
#include "leveldata.h"
#include "board.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <set>

namespace sprites {

namespace {
constexpr uint16_t FRAME_BANK_A = 0x3000, FRAME_BANK_B = 0x3400;   // frame data
constexpr uint16_t ANIM_FIRST = 0x4000, ANIM_END = 0x5BCC;           // animation data (bank A 0)
constexpr int MAX_PIECES = 16, NUM_TYPES = 101;
// object type tables (bank A 0 / bank B 0x0400)
constexpr uint16_t T_ANIM = 0x5F7C, T_ANIM2 = 0x6046, T_BOXES = 0x6110, T_MODE = 0x623F, T_OFFX = 0x62A4,
	T_OFFY = 0x636E, T_PROPS = 0x6438, T_FLAGS = 0x649D, T_RATE = 0x6502, T_PIECES = 0x5C96, T_SOUND = 0x6567;
constexpr uint16_t DIR_OFFSETS = 0xFDBA;      // byte offset into a direction table per direction 0-15
constexpr uint16_t SPRITE_PALETTE = 0x7F2C;   // bank B 0x0400, 4 x 16 words, reversed
// map objects
constexpr uint16_t MAP_BANK_A = 0x1000, MAP_BANK_B = 0x1400;
constexpr uint16_t CELLS_FIXED = 0x7A00, CELLS_DYNAMIC = 0x7D00, DYNAMIC_DEFS = 0x0338;
constexpr uint16_t DEF_X = 0x4AC0, DEF_Y = 0x5580, DEF_PROPS = 0x4560, DEF_RECORD = 0x6040;
constexpr int NUM_DEFS = (DEF_Y - DEF_X) / 2;
constexpr uint16_t TOWN_LIST = 0x6696, FIXED_TYPES = 0x70E4, FIXED_DESCS = 0x70E8;
constexpr uint16_t STAGE_LISTS = 0x6B9E, STAGE_SKATERS = 0x708C, STAGE_GROUP = 0xFDE3;
constexpr uint16_t SPAWN_COUNTS = 0x68D0, SPAWN_TYPES = 0x6862, SPAWN_TYPES_HI = 0x6935, SPAWN_LISTS = 0x6948,
	SPAWN_MASKS = 0xFD95;
constexpr uint16_t SCREEN_TYPES = 0x7D94, SCREEN_LISTS = 0x7D9A, SCREEN_X = 0x7DA4, SCREEN_Y = 0x7DAE,
	SCREEN_COUNT = 0xFF2A;

struct Range { uint16_t from, to; Category cat; const char *name; int palette; int dirs; bool screen = false; };
// The animation data is laid out by object; a frame is named after the place it first appears
// (checked against the rendered frames).  dirs: the range holds tables of 16 (or 8) directions
// (the skater's direction 0x01F8; 8: direction & 7), so the name gets "dir N"; 0: lists.
const Range RANGES[] = {
	{0x4000, 0x4020, Category::Skater, "skater riding", 0, 16},
	{0x4030, 0x42B0, Category::Skater, "skater riding on slopes", 0, 16},
	{0x42B0, 0x42D0, Category::Skater, "skater crouching", 0, 16},
	{0x42D0, 0x4350, Category::Skater, "skater pushing off", 0, 0},
	{0x4350, 0x43E8, Category::Skater, "skater jumping", 0, 0},
	{0x43E8, 0x4428, Category::Skater, "skater riding fast", 0, 16},
	{0x4428, 0x4448, Category::Skater, "skater crouching low", 0, 16},
	{0x4448, 0x44A8, Category::Skater, "skater landing / falling", 0, 0},
	{0x44A8, 0x45AC, Category::Skater, "skater aerial tricks", 0, 0},
	{0x45AC, 0x45AE, Category::Blank, "blank", 0, 0},
	{0x45AE, 0x461E, Category::Skater, "skater tumbling", 0, 0},
	{0x461E, 0x4C3C, Category::Helmet, "helmet", 0, 0},
	{0x4C3C, 0x4C4C, Category::Skateboard, "skateboard", 1, 8},
	{0x4C5C, 0x4E1C, Category::Skateboard, "skateboard on slopes", 1, 8},
	{0x4E1C, 0x4E2C, Category::Skateboard, "skateboard in the air", 1, 8},
	{0x4E2C, 0x4E3C, Category::Skateboard, "skateboard in the air", 1, 16},
	{0x4E3C, 0x4ECC, Category::Skateboard, "skateboard tricks", 1, 0},
	{0x4ECC, 0x4F64, Category::Skateboard, "skateboard jumping", 1, 0},
	{0x4F64, 0x4F84, Category::Skateboard, "skateboard riding", 1, 16},
	{0x4F84, 0x5104, Category::Skateboard, "skateboard", 1, 0},
	{0x5104, 0x5134, Category::Chaser, "killer bees", 1, 0},
	{0x5134, 0x5190, Category::Chaser, "chaser: darts", 1, 0},
	{0x5190, 0x5204, Category::Chaser, "chaser: scissors", 1, 0},
	{0x5204, 0x5228, Category::Chaser, "chaser: jaws", 1, 0},
	{0x5228, 0x5238, Category::Chaser, "chaser: skull / axe", 1, 0},
	{0x528C, 0x5334, Category::Vehicle, "car", 2, 0},
	{0x5334, 0x533E, Category::Vehicle, "rider on a mower", 2, 0},
	{0x533E, 0x53B4, Category::Person, "breakdancer (floor moves)", 2, 0},
	{0x53B4, 0x5414, Category::Person, "dancer", 2, 0},
	{0x5414, 0x547C, Category::Person, "bodybuilder", 2, 0},
	{0x547C, 0x54EC, Category::Person, "unicyclist", 2, 0},
	{0x54EC, 0x55C2, Category::Person, "roller skater in white", 2, 0},
	{0x55C2, 0x5640, Category::Vehicle, "kid on a tricycle", 2, 0},
	{0x5640, 0x5718, Category::Vehicle, "BMX rider", 2, 0},
	{0x5718, 0x571A, Category::Effect, "cross", 0, 0},
	{0x571A, 0x5720, Category::Screen, "skateboard wheel", 0, 0, true},
	{0x5720, 0x576A, Category::Screen, "720 logo", 1, 0, true},
	{0x576A, 0x5786, Category::Effect, "score", 2, 0},
	{0x5786, 0x5796, Category::Effect, "sparkle", 2, 0},
	{0x5796, 0x57A8, Category::Item, "disc", 3, 0},
	{0x57A8, 0x57BE, Category::Effect, "splash / ouch", 2, 0},
	{0x57BE, 0x57CA, Category::Effect, "marker", 0, 0},
	{0x57CA, 0x58CE, Category::Sign, "points sign", 0, 0},
	{0x58CE, 0x5910, Category::Fixture, "flag", 2, 0},
	{0x5910, 0x5916, Category::Fixture, "speakers", 3, 0},
	{0x5916, 0x5920, Category::Fixture, "pennant", 0, 0},
	{0x5920, 0x5A24, Category::Item, "dollar bill", 2, 0},
	{0x5A24, 0x5A64, Category::Person, "frisbee thrower", 2, 0},
	{0x5A64, 0x5A7C, Category::Item, "frisbee", 2, 0},
	{0x5A7C, 0x5A9C, Category::Screen, "medal", 0, 0, true},
	{0x5A9C, 0x5B64, Category::Person, "ticket seller", 1, 0},
	{0x5B64, 0x5B74, Category::Sign, "shop / park sign", 3, 0},
	{0x5B74, 0x5B80, Category::Fixture, "park gate", 0, 0},
};

const Range *range_of(uint16_t addr)
{
	for (const auto &r : RANGES)
		if (addr >= r.from && addr < r.to) return &r;
	return nullptr;
}

// code that sets animation pointers to fixed lists (F:, A7:, B1:), for the animation list
const uint16_t CODE_LISTS[] = { 0x50FA, 0x576A, 0x5786, 0x578A, 0x57A8, 0x57AA, 0x57AC, 0x571A, 0x5722 };

int s8(uint8_t v) { return (int8_t)v; }
bool in_anim(uint16_t a) { return a >= ANIM_FIRST && a < ANIM_END; }

// parse the frame at id; false if it is not a valid frame
bool parse_frame(const RomView &fv, uint16_t id, Frame &f)
{
	if (id >= 0x4000) return false;
	uint32_t p = id;
	auto byte = [&](uint8_t &v) { if (p >= 0x4000) return false; v = fv.b((uint16_t)(0x4000 + p++)); return true; };
	uint8_t x = 0, mode = 0, y = 0, lo = 0, hi = 0;
	if (!byte(x) || !byte(mode)) return false;
	if (mode & 0x78) return false;
	const bool common = (mode & 0x80) != 0;
	if (common && !byte(y)) return false;
	f = Frame();
	f.id = id;
	f.x_offset = (int8_t)x;
	f.mode = mode;
	for (int k = 0; k < MAX_PIECES; k++)
	{
		if (!common && !byte(y)) return false;
		if (!byte(lo) || !byte(hi)) return false;
		const uint16_t word = (uint16_t)(lo | (hi << 8));
		Piece pc;
		pc.tiles = ((word >> 11) & 7) + 1;
		pc.code = (word & 0x7FF) | ((mode & 7) << 11);
		pc.hflip = (word & 0x4000) != 0;
		pc.x = f.x_offset + 16 * k;
		pc.y = -s8(y) - 16 * pc.tiles;
		f.pieces.push_back(pc);
		if (word & 0x8000)
		{
			f.size = (int)(p - id);
			f.rom_offset = (uint32_t)fv.offset((uint16_t)(0x4000 + id));
			f.left = f.top = 1 << 20;
			f.right = f.bottom = -(1 << 20);
			for (const auto &q : f.pieces)
			{
				f.left = std::min(f.left, q.x);
				f.top = std::min(f.top, q.y);
				f.right = std::max(f.right, q.x + 16);
				f.bottom = std::max(f.bottom, q.y + 16 * q.tiles);
			}
			return true;
		}
	}
	return false;
}
}   // namespace

const char *category_name(Category c)
{
	static const char *n[] = { "skater", "helmet", "skateboard", "chaser", "vehicle", "person", "item",
		"effect", "sign", "fixture", "screen", "blank", "unused" };
	const int i = (int)c;
	return i >= 0 && i < (int)Category::Count ? n[i] : "?";
}

const char *source_name(ObjectSource s)
{
	switch (s)
	{
	case ObjectSource::TownList: return "town list";
	case ObjectSource::FixedFour: return "town fixed";
	case ObjectSource::StageList: return "stage list";
	case ObjectSource::StageSkaters: return "stage skaters";
	case ObjectSource::Screen: return "screen";
	}
	return "?";
}

int64_t bank_offset(uint16_t bankreg) { return LevelSet::rom_bank_offset(bankreg); }

int64_t RomView::offset(uint16_t addr) const
{
	if (addr >= 0x8000) return addr;
	if (addr >= 0x6000) return bank_offset(bank_b) + (addr - 0x6000);
	if (addr >= 0x4000) return bank_offset(bank_a) + (addr - 0x4000);
	return -1;
}

uint8_t RomView::b(uint16_t addr) const
{
	const int64_t o = offset(addr);
	return o >= 0 && main && (size_t)o < main->size() ? (*main)[(size_t)o] : 0;
}

uint16_t RomView::w(uint16_t addr) const { return (uint16_t)(b(addr) | (b((uint16_t)(addr + 1)) << 8)); }

RomView SpriteSet::anim_view() const { return RomView{&m_roms->main, 0x0000, 0x0400}; }

// ------------------------------------------------------------------ frames

void SpriteSet::decode_frames()
{
	const RomView av = anim_view(), fv{&m_roms->main, FRAME_BANK_A, FRAME_BANK_B};
	std::map<uint16_t, uint16_t> first;                  // id -> first reference
	for (uint16_t a = ANIM_FIRST; a < ANIM_END; a += 2)
	{
		const uint16_t v = av.w(a);
		Frame f;
		if (v < 0x4000 && !first.count(v) && parse_frame(fv, v, f)) first[v] = a;
	}
	frames.clear();
	for (const auto &[id, ref] : first)
	{
		Frame f;
		parse_frame(fv, id, f);
		f.referenced = true;
		f.first_ref = ref;
		frames.push_back(std::move(f));
	}
	// frames between the referenced ones: the data is packed back to back, and 3 bytes of
	// velocities may precede a frame
	std::vector<Frame> extra;
	for (size_t i = 0; i < frames.size(); i++)
	{
		uint32_t p = frames[i].id + frames[i].size;
		const uint32_t end = i + 1 < frames.size() ? frames[i + 1].id : 0x4000;
		while (p < end && end - p != 3)
		{
			Frame f;
			if (parse_frame(fv, (uint16_t)p, f) && (uint32_t)(f.id + f.size) <= end) {}
			else if (p + 3 < end && parse_frame(fv, (uint16_t)(p + 3), f) && (uint32_t)(f.id + f.size) <= end) {}
			else break;
			bool blank = true;                           // zero filled space is not a frame
			for (int k = 0; k < f.size && blank; k++) blank = fv.b((uint16_t)(0x4000 + f.id + k)) == 0;
			if (blank) break;
			p = f.id + f.size;
			extra.push_back(std::move(f));
		}
	}
	for (auto &f : extra) frames.push_back(std::move(f));
	std::sort(frames.begin(), frames.end(), [](const Frame &a, const Frame &b) { return a.id < b.id; });
}

const Frame *SpriteSet::frame(uint16_t id) const
{
	auto it = std::lower_bound(frames.begin(), frames.end(), id, [](const Frame &f, uint16_t v) { return f.id < v; });
	return it != frames.end() && it->id == id ? &*it : nullptr;
}

const Animation *SpriteSet::animation(uint16_t addr) const
{
	auto it = std::lower_bound(animations.begin(), animations.end(), addr, [](const Animation &a, uint16_t v) { return a.addr < v; });
	return it != animations.end() && it->addr == addr ? &*it : nullptr;
}

std::vector<uint16_t> SpriteSet::list_frames(uint16_t addr, int max) const
{
	const RomView av = anim_view();
	std::vector<uint16_t> out;
	std::set<uint16_t> visited;
	uint16_t a = addr;
	for (int n = 0; n < 4 * max && (int)out.size() < max; n++)
	{
		if (!in_anim(a) || !visited.insert(a).second) break;
		const uint16_t v = av.w(a);
		if (v & 0x8000) { a = v & 0x7FFF; continue; }
		if (v >= 0x4000) break;                          // a pointer: not a plain list
		out.push_back(v);
		a = (uint16_t)(a + 2);
	}
	return out;
}

// ------------------------------------------------------------------ object types

namespace {
// names after what the types show (see the type sheet of 'leveltool sprites'); the ones
// without animation data or with animations set by code are named by their use
struct TypeName { int type; const char *name; };
const TypeName TYPE_NAMES[] = {
	{1, "skateboard / shadow"}, {2, "skater"}, {3, "helmet"}, {4, "chaser (killer bees)"}, {6, "720 logo"},
	{10, "convertible"}, {11, "sports car"}, {12, "wedge car"}, {13, "pickup truck"}, {14, "hot rod"},
	{17, "points flag (animation points at frame 2687; unused)"},
	{21, "closed sign (animation points at frame 26FC; unused)"},
	{22, "closed sign (animation points at frame 2707; unused)"},
	{47, "convertible"}, {48, "wedge car"}, {72, "points"}, {73, "points"}, {74, "points"}, {75, "points"},
	{76, "points"}, {77, "points"}, {78, "points"}, {79, "points"}, {80, "points"}, {81, "points"}, {82, "points"},
	{83, "score"}, {86, "medal"}, {92, "shop wall"}, {93, "ticket needed / park closed sign"},
	{94, "park closed sign"}, {95, "shop closed sign"}, {96, "shop closed sign"}, {100, "skateboard wheel"},
};
}

void SpriteSet::decode_types()
{
	const RomView av = anim_view();
	// start addresses of all type tables: a direction table ends where the next one starts
	std::set<uint16_t> starts;
	for (int t = 0; t < NUM_TYPES; t++)
		for (uint16_t tab : {T_ANIM, T_ANIM2, T_BOXES})
			starts.insert(av.w((uint16_t)(tab + 2 * t)));
	starts.insert(ANIM_END);
	starts.insert(0x6000);
	// list starts: a list that has no link of its own ends where another one starts
	std::set<uint16_t> entries(std::begin(CODE_LISTS), std::end(CODE_LISTS));
	for (int t = 0; t < NUM_TYPES; t++)
	{
		const uint16_t a = av.w((uint16_t)(T_ANIM + 2 * t)), a2 = av.w((uint16_t)(T_ANIM2 + 2 * t));
		if (av.b((uint16_t)(T_PROPS + t)) & 0x80) entries.insert(a);
		entries.insert(a2);
	}
	for (uint16_t a = ANIM_FIRST; a < ANIM_END; a += 2)
	{
		const uint16_t v = av.w(a);
		if (v >= 0x4000 && v < 0x8000 && in_anim(v)) entries.insert(v);   // pointers
	}
	types.assign(NUM_TYPES, ObjectType());
	for (int t = 0; t < NUM_TYPES; t++)
	{
		ObjectType &ty = types[t];
		ty.type = t;
		ty.props = av.b((uint16_t)(T_PROPS + t));
		ty.flags = av.b((uint16_t)(T_FLAGS + t));
		ty.mode = av.b((uint16_t)(T_MODE + t));
		ty.rate = av.b((uint16_t)(T_RATE + t));
		ty.pieces = av.b((uint16_t)(T_PIECES + t));
		ty.sound = av.b((uint16_t)(T_SOUND + t));
		ty.palette = (ty.props >> 4) & 3;
		ty.off_x = (int16_t)av.w((uint16_t)(T_OFFX + 2 * t));
		ty.off_y = (int16_t)av.w((uint16_t)(T_OFFY + 2 * t));
		ty.anim = av.w((uint16_t)(T_ANIM + 2 * t));
		ty.anim2 = av.w((uint16_t)(T_ANIM2 + 2 * t));
		ty.boxes = av.w((uint16_t)(T_BOXES + 2 * t));
		ty.dir_frame.fill(-1);
		std::set<uint16_t> reach;
		// every frame a list (or a ring with list pointers) leads to
		std::function<void(uint16_t, int)> walk = [&](uint16_t a, int depth) {
			std::set<uint16_t> visited;
			for (int n = 0; n < 256 && in_anim(a) && visited.insert(a).second; n++)
			{
				const uint16_t v = av.w(a);
				if (v & 0x8000) { a = v & 0x7FFF; continue; }
				if (v >= 0x4000) { if (depth < 2 && in_anim(v)) walk(v, depth + 1); }
				else reach.insert(v);
				a = (uint16_t)(a + 2);
				if (entries.count(a)) break;             // ran into the next list
			}
		};
		const bool table = !(ty.props & 0x80) && ty.anim >= ANIM_FIRST && ty.anim < 0x6000;
		if (in_anim(ty.anim) || table)
		{
			if (ty.props & 0x80)
			{
				const auto fr = list_frames(ty.anim, 1);
				if (!fr.empty()) ty.first_frame = fr[0];
				walk(ty.anim, 0);
			}
			else
			{
				ty.directional = true;
				const int extent = *starts.upper_bound(ty.anim) - ty.anim;
				for (int d = 0; d < 16; d++)
				{
					const uint8_t off = av.b((uint16_t)(DIR_OFFSETS + d));
					if (off >= extent || off >= 0x20) continue;
					const uint16_t e = av.w((uint16_t)(ty.anim + off));
					if (!in_anim(e)) continue;
					ty.dir_entry[d] = e;
					const uint16_t v = av.w(e);
					if (ty.rate && v >= 0x4000 && in_anim(v))
					{
						ty.dir_list[d] = v;
						const auto fr = list_frames(v, 1);
						if (!fr.empty()) ty.dir_frame[d] = fr[0];
					}
					else if (v < 0x4000)
					{
						ty.dir_list[d] = e;
						ty.dir_frame[d] = v;
					}
					walk(e, 0);
				}
			}
			if (in_anim(ty.anim2)) walk(ty.anim2, 0);
		}
		for (uint16_t f : reach)
			if (frame(f)) ty.frames.push_back(f);
	}
}

// ------------------------------------------------------------------ names

void SpriteSet::name_frames()
{
	const RomView av = anim_view();
	std::map<uint16_t, Frame *> by_id;
	for (auto &f : frames) by_id[f.id] = &f;
	for (const auto &ty : types)
		for (uint16_t id : ty.frames)
			by_id[id]->types.push_back(ty.type);
	// number the frames of each range in order of appearance
	std::map<const Range *, int> counter;
	char buf[160];
	for (uint16_t a = ANIM_FIRST; a < ANIM_END; a += 2)
	{
		const uint16_t v = av.w(a);
		auto it = by_id.find(v);
		if (v >= 0x4000 || it == by_id.end() || it->second->first_ref != a) continue;
		Frame *f = it->second;
		const Range *r = range_of(a);
		if (!r)
		{
			snprintf(buf, sizeof(buf), "frame %04X", v);
			f->name = buf;
			continue;
		}
		f->category = r->cat;
		f->palette = r->palette;
		f->screen_colours = r->screen;
		auto dir_name = [&](const Range *q, uint16_t at, char *out, size_t n, const char *prefix) {
			const int size = 2 * q->dirs, table = (at - q->from) / size, dir = ((at - q->from) % size) / 2;
			if (q->to - q->from > size) snprintf(out, n, "%s%s, table %d dir %d", prefix, q->name, table, dir);
			else snprintf(out, n, "%s%s, dir %d", prefix, q->name, dir);
		};
		if (r->cat == Category::Helmet)
		{
			// the helmet tables mirror the skater's, 0x061E bytes further (F:E5BC)
			const Range *b = range_of((uint16_t)(a - 0x061E));
			if (b && b->dirs) dir_name(b, (uint16_t)(a - 0x061E), buf, sizeof(buf), "helmet: ");
			else snprintf(buf, sizeof(buf), "helmet: %s #%d", b ? b->name : "?", ++counter[r]);
		}
		else if (r->dirs)
			dir_name(r, a, buf, sizeof(buf), "");
		else
			snprintf(buf, sizeof(buf), "%s #%d", r->name, ++counter[r]);
		f->name = buf;
	}
	const RomView fv{&m_roms->main, FRAME_BANK_A, FRAME_BANK_B};
	for (auto &f : frames)
	{
		if (!f.referenced)
		{
			f.category = Category::Unused;
			snprintf(buf, sizeof(buf), "unused frame %04X", f.id);
			f.name = buf;
		}
		// the palette of the (first) map object type that shows it, else the range's
		if (!f.types.empty() && f.types[0] >= 4) f.palette = types[f.types[0]].palette;
		for (int t : f.types)
			if (types[t].flags & 0x01) f.motion = true;
		if (f.motion && f.id >= 3)
		{
			f.vx = (int8_t)fv.b((uint16_t)(0x4000 + f.id - 3));
			f.vy = (int8_t)fv.b((uint16_t)(0x4000 + f.id - 2));
			f.step = fv.b((uint16_t)(0x4000 + f.id - 1));
		}
	}
	// a type is named and categorised after its frames
	for (auto &ty : types)
	{
		ty.name = "unused";
		if (!ty.frames.empty())
		{
			const Frame *f = frame(ty.directional && ty.dir_frame[0] >= 0 ? (uint16_t)ty.dir_frame[0] : ty.frames[0]);
			for (int d = 0; d < 16 && ty.directional; d++)
				if (ty.dir_frame[d] >= 0) { f = frame((uint16_t)ty.dir_frame[d]); break; }
			if (!ty.directional && ty.first_frame >= 0) f = frame((uint16_t)ty.first_frame);
			if (f)
			{
				ty.category = f->category;
				const Range *r = range_of(f->first_ref);
				ty.name = r ? r->name : f->name;
			}
		}
		for (const auto &tn : TYPE_NAMES)
			if (tn.type == ty.type) ty.name = tn.name;
		if (ty.props & 0x40) ty.name += " (no sprite)";
	}
}

void SpriteSet::collect_animations()
{
	const RomView av = anim_view();
	std::map<uint16_t, int> entry;                       // list address -> rate
	auto add = [&](uint16_t a, int rate) {
		if (!in_anim(a)) return;
		auto it = entry.find(a);
		if (it == entry.end()) entry[a] = rate;
		else if (!it->second) it->second = rate;
	};
	for (const auto &ty : types)
	{
		if (!ty.directional) add(ty.anim, ty.rate);
		add(ty.anim2, 8);                                // F:D47C starts it at rate 8
		for (int d = 0; d < 16; d++)
			if (ty.dir_list[d] && ty.rate) add(ty.dir_list[d], ty.rate);
	}
	for (uint16_t a : CODE_LISTS) add(a, 0);
	for (uint16_t a = ANIM_FIRST; a < ANIM_END; a += 2)
	{
		const uint16_t v = av.w(a);
		if (v & 0x8000) add(v & 0x7FFF, 0);              // link targets
		else if (v >= 0x4000) add(v, 0);                 // pointers
	}
	animations.clear();
	for (const auto &[addr, rate] : entry)
	{
		Animation an;
		an.addr = addr;
		an.rate = rate;
		uint16_t a = addr, start = addr;
		bool plain = true;
		std::set<uint16_t> visited;
		for (int n = 0; n < 64; n++)
		{
			if (!in_anim(a) || !visited.insert(a).second) break;
			const uint16_t v = av.w(a);
			if (v & 0x8000)
			{
				const uint16_t t = v & 0x7FFF;
				if (an.frames.empty()) { a = start = t; continue; }   // starts with a link
				if (t >= start && t < a && !((t - start) & 1)) an.loop = (t - start) / 2;
				else an.next = t;
				break;
			}
			if (v >= 0x4000) { plain = false; break; }   // a direction ring
			an.frames.push_back(v);
			a = (uint16_t)(a + 2);
		}
		// only lists that end with a link are animations (the others are tables the game
		// indexes, e.g. the skater's frames by direction)
		if (!plain || an.frames.empty() || (an.loop < 0 && !an.next)) continue;
		if (const Frame *f = frame(an.frames[0]))
		{
			const size_t hash = f->name.rfind(" #");
			an.name = hash != std::string::npos ? f->name.substr(0, hash) : f->name;
		}
		for (const auto &ty : types)
			if (ty.anim == addr && !ty.directional && !ty.frames.empty()) an.name = ty.name;
		animations.push_back(std::move(an));
	}
}

// ------------------------------------------------------------------ rendering

std::vector<uint32_t> SpriteSet::render(const GfxSet &gfx, const Frame &f, const uint16_t *pal16) const
{
	const int w = f.width(), h = f.height();
	std::vector<uint32_t> img((size_t)w * h, 0);
	draw(gfx, f, img.data(), w, h, -f.left, -f.top, pal16);
	for (auto &p : img)                                  // ARGB -> R, G, B, A bytes
	{
		const uint32_t a = p >> 24, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
		p = r | (g << 8) | (b << 16) | (a << 24);
	}
	return img;
}

void SpriteSet::draw(const GfxSet &gfx, const Frame &f, uint32_t *argb, int w, int h, int x, int y, const uint16_t *pal16) const
{
	uint32_t pal[16];
	const uint16_t *src = pal16 ? pal16 : (f.screen_colours ? screen_palette : palette)[f.palette & 3].data();
	for (int i = 0; i < 16; i++) pal[i] = level_palette_rgb(src[i]);
	for (const auto &pc : f.pieces)
		for (int t = 0; t < pc.tiles; t++)
		{
			const uint8_t *tile = &gfx.sprites[(size_t)((pc.code + t) & 8191) * 256];
			for (int py = 0; py < 16; py++)
			{
				const int yy = y + pc.y + 16 * t + py;
				if (yy < 0 || yy >= h) continue;
				for (int px = 0; px < 16; px++)
				{
					const int xx = x + pc.x + px;
					if (xx < 0 || xx >= w) continue;
					const int pen = tile[py * 16 + (pc.hflip ? 15 - px : px)];
					if (pen != 15) argb[(size_t)yy * w + xx] = pal[pen];
				}
			}
		}
}

// ------------------------------------------------------------------ map objects

Definition SpriteSet::definition(int index) const
{
	Definition d;
	const RomView mv{&m_roms->main, MAP_BANK_A, MAP_BANK_B};
	if (index < 0 || index >= NUM_DEFS) return d;
	d.index = index;
	d.x = mv.w((uint16_t)(DEF_X + 2 * index));
	d.y = mv.w((uint16_t)(DEF_Y + 2 * index));
	d.x_rom = (uint32_t)mv.offset((uint16_t)(DEF_X + 2 * index));
	d.y_rom = (uint32_t)mv.offset((uint16_t)(DEF_Y + 2 * index));
	d.props = mv.b((uint16_t)(DEF_PROPS + index));
	if (index < DYNAMIC_DEFS)
	{
		const uint16_t r = (uint16_t)(DEF_RECORD + 8 * index);
		d.has_record = true;
		for (int k = 0; k < 2; k++)
		{
			d.next[k] = mv.w((uint16_t)(r + 4 * k));
			d.dir[k] = mv.b((uint16_t)(r + 4 * k + 2));
			d.speed[k] = mv.b((uint16_t)(r + 4 * k + 3));
		}
	}
	return d;
}

void SpriteSet::cell_definitions(int col, int row, bool dynamic, int &first, int &count) const
{
	const RomView mv{&m_roms->main, MAP_BANK_A, MAP_BANK_B};
	const uint16_t tab = dynamic ? CELLS_DYNAMIC : CELLS_FIXED;
	const int cell = ((col & 0x1F) << 4) | (row & 15);
	first = -1;
	count = 0;
	const uint16_t e = mv.w((uint16_t)(tab + 2 * cell));
	if (e & 0x8000) return;
	// the definitions of a cell end where the next higher cell entry starts
	int next = -1;
	for (int c = 0; c < 0x180; c++)
	{
		const uint16_t v = mv.w((uint16_t)(tab + 2 * c));
		if (!(v & 0x8000) && v > e && (next < 0 || v < next)) next = v;
	}
	first = e + (dynamic ? DYNAMIC_DEFS : 0);
	count = next > 0 ? next - e : 1;
}

bool SpriteSet::descriptor_position(uint16_t desc, bool dynamic, int type, double &x, double &y) const
{
	const RomView mv{&m_roms->main, MAP_BANK_A, MAP_BANK_B};
	const int cell = desc & 0x1FF, col = cell >> 4, row = cell & 15;
	const uint16_t e = mv.w((uint16_t)((dynamic ? CELLS_DYNAMIC : CELLS_FIXED) + 2 * cell));
	if (e & 0x8000) return false;
	int def = (e + ((desc >> 9) & 0x3F)) & 0xFFFF;
	if (dynamic) def += DYNAMIC_DEFS;
	const Definition d = definition(def);
	const ObjectType *ty = type >= 0 && type < (int)types.size() ? &types[type] : nullptr;
	const int wx = (int16_t)d.x + (ty ? ty->off_x : 0), wy = (int16_t)d.y + (ty ? ty->off_y : 0);
	x = 256.0 * col + wx / 64.0;
	y = 256.0 * (row + 1) - wy / 64.0;
	return true;
}

void SpriteSet::map_to_cell(double x, double y, int type, int &col, int &row, uint16_t &def_x, uint16_t &def_y) const
{
	const ObjectType *ty = type >= 0 && type < (int)types.size() ? &types[type] : nullptr;
	const double ox = ty ? ty->off_x / 64.0 : 0, oy = ty ? ty->off_y / 64.0 : 0;
	// the definition's position, without the type offset
	const double dx = x - ox, dy = y + oy;
	col = std::clamp((int)(dx / 256), 0, 31);
	row = std::clamp((int)std::ceil(dy / 256) - 1, 0, 15);  // a cell spans y (256 row, 256 (row + 1)]
	const double wx = (dx - 256.0 * col) * 64, wy = (256.0 * (row + 1) - dy) * 64;
	def_x = (uint16_t)std::clamp((int)(wx + 0.5), 0, 0x3FFF);
	def_y = (uint16_t)std::clamp((int)(wy + 0.5), 0, 0x3FFF);
}

MapObject SpriteSet::place(ObjectSource src, int stage, int index, int type, uint16_t desc, bool dynamic, uint32_t rom) const
{
	MapObject o;
	o.source = src;
	o.stage = stage;
	o.index = index;
	o.type = type;
	o.descriptor = desc;
	o.dynamic = dynamic;
	o.rom_offset = rom;
	o.cell_row = desc & 15;
	o.cell_col = (desc >> 4) & 0x1F;
	o.def_offset = (desc >> 9) & 0x3F;
	const RomView mv{&m_roms->main, MAP_BANK_A, MAP_BANK_B};
	const uint16_t e = mv.w((uint16_t)((dynamic ? CELLS_DYNAMIC : CELLS_FIXED) + 2 * (desc & 0x1FF)));
	const ObjectType *ty = type >= 0 && type < (int)types.size() ? &types[type] : nullptr;
	if (!(e & 0x8000))
	{
		o.definition = ((e + o.def_offset) & 0xFFFF) + (dynamic ? DYNAMIC_DEFS : 0);
		const Definition d = definition(o.definition);
		o.world_x = (0x4000 * o.cell_col + d.x + (ty ? ty->off_x : 0)) & 0xFFFF;
		o.world_y = (0x6000 - 0x4000 * o.cell_row + d.y + (ty ? ty->off_y : 0)) & 0xFFFF;
		descriptor_position(desc, dynamic, type, o.map_x, o.map_y);
		if (ty && ty->directional && d.has_record) o.direction = d.dir[0];
	}
	else
		o.note = "cell without definition: the game does not place it (F:CE24 refuses it)";
	if (ty)
	{
		if (ty->directional && o.direction >= 0 && o.direction < 16)
		{
			o.anim = ty->dir_list[o.direction];
			o.frame = ty->dir_frame[o.direction];
		}
		else if (!ty->directional)
		{
			o.anim = ty->anim;
			o.frame = ty->first_frame;
		}
		if (type == 1) o.note = "scripted skater (F:DF40 sets its position and animation)";
		if (ty->props & 0x40) o.note = "no sprite pieces (invisible)";
	}
	return o;
}

void SpriteSet::decode_objects()
{
	const RomView av = anim_view();
	objects.clear();
	lists.clear();
	spawns.clear();
	auto rom = [&](uint16_t a) { return (uint32_t)av.offset(a); };
	// town list (dynamic objects, F:CFD6)
	{
		ObjectList l;
		l.source = ObjectSource::TownList;
		l.addr = TOWN_LIST;
		l.rom_offset = rom(TOWN_LIST);
		uint16_t a = TOWN_LIST;
		for (int i = 0; i < 64 && av.w(a) != 0xFFFF; i++, a = (uint16_t)(a + 4))
		{
			l.entries.push_back({av.w(a), av.w((uint16_t)(a + 2))});
			objects.push_back(place(ObjectSource::TownList, 0, i, av.w(a), av.w((uint16_t)(a + 2)), true, rom(a)));
		}
		l.capacity = (int)l.entries.size();
		lists.push_back(l);
	}
	// four fixed objects (F:D19C)
	for (int i = 0; i < 4; i++)
		objects.push_back(place(ObjectSource::FixedFour, 0, i, av.b((uint16_t)(FIXED_TYPES + i)),
			av.w((uint16_t)(FIXED_DESCS + 2 * i)), false, rom((uint16_t)(FIXED_TYPES + i))));
	// stages: type-1 skaters and lists
	for (int s = 1; s <= stages; s++)
	{
		const uint16_t g = (uint16_t)((av.b((uint16_t)(STAGE_GROUP + s)) << 3) + STAGE_SKATERS - 8);
		for (int i = 0; i < 4; i++)
			objects.push_back(place(ObjectSource::StageSkaters, s, i, 1, av.w((uint16_t)(g + 2 * i)), false, rom((uint16_t)(g + 2 * i))));
		const uint16_t p = av.w((uint16_t)(STAGE_LISTS + 2 * s));
		ObjectList l;
		l.source = ObjectSource::StageList;
		l.stage = s;
		l.addr = p;
		l.rom_offset = rom(p);
		uint16_t a = p;
		for (int i = 0; i < 128 && av.w(a) != 0xFFFF; i++, a = (uint16_t)(a + 4))
		{
			l.entries.push_back({av.w(a), av.w((uint16_t)(a + 2))});
			objects.push_back(place(ObjectSource::StageList, s, i, av.w(a), av.w((uint16_t)(a + 2)), false, rom(a)));
		}
		l.capacity = (int)l.entries.size();
		lists.push_back(l);
	}
	// spawn kinds (F:D35E)
	for (int k = 1; k <= 19; k++)
	{
		SpawnKind sk;
		sk.kind = k;
		sk.dynamic = k <= 10;
		sk.list = av.w((uint16_t)(SPAWN_LISTS + 2 * k));
		const int mask = av.b((uint16_t)(SPAWN_MASKS + k));
		if (sk.list >= 0x4000 && mask)
			for (int i = 0; i <= mask; i++) sk.descriptors.push_back(av.w((uint16_t)(sk.list + 2 * i)));
		for (int s = 1; s <= stages; s++)
		{
			if (k <= 9) sk.count[s] = av.b((uint16_t)(av.w((uint16_t)(SPAWN_COUNTS + 2 * s)) + k - 1));
			sk.type[s] = k <= 10 ? av.b((uint16_t)(av.w((uint16_t)(SPAWN_TYPES + 2 * s)) + k - 1))
				: av.b((uint16_t)(SPAWN_TYPES_HI + k));
		}
		spawns.push_back(sk);
	}
	// park entries (F:D880: 8 descriptors at 0x70C4, types 0x70D4 closed / 0x70DC open) and
	// ticket booths (F:D8D2: 4 descriptors at 0x70AC, types 0x70BC / 0x70C0 with the ticket)
	entry_types.clear();
	for (int i = 0; i < 8; i++)
	{
		EntryType e;
		e.descriptor = av.w((uint16_t)(0x70C4 + 2 * i));
		e.closed_type = av.b((uint16_t)(0x70D4 + i));
		e.open_type = av.b((uint16_t)(0x70DC + i));
		e.index = i >> 1;
		entry_types.push_back(e);
	}
	for (int i = 1; i <= 4; i++)
	{
		EntryType e;
		e.descriptor = av.w((uint16_t)(0x70AA + 2 * i));
		e.closed_type = av.b((uint16_t)(0x70BB + i));
		e.open_type = av.b((uint16_t)(0x70BF + i));
		e.index = i;
		e.ticket = true;
		entry_types.push_back(e);
	}
	for (auto &o : objects)
		for (const auto &e : entry_types)
			if (o.source == ObjectSource::TownList && o.descriptor == e.descriptor)
			{
				char b[96];
				snprintf(b, sizeof(b), "type set at start: %d (%s %d closed / no ticket), %d (open / ticket)",
					e.closed_type, e.ticket ? "ticket" : "park", e.index, e.open_type);
				o.note = b;
			}
	// attract screens 5 and 6 (F:EE6E): balls and the logo in world coordinates; the screens are
	// set up from map column 0 at scroll (0x2000, 0), so map x = world x / 64 and
	// map y = 128 - world y / 64 (mod 512) as in the town
	for (int screen = 0; screen < 2; screen++)
	{
		const int last = av.b((uint16_t)(SCREEN_COUNT + screen));
		for (int i = 0; i <= last && i < 5; i++)
		{
			MapObject o;
			o.source = ObjectSource::Screen;
			o.level = 5 + screen;
			o.index = i;
			o.type = av.b((uint16_t)(SCREEN_TYPES + i));
			o.anim = av.w((uint16_t)(SCREEN_LISTS + 2 * i));
			const auto fr = list_frames(o.anim, 1);
			o.frame = fr.empty() ? -1 : fr[0];
			o.world_x = av.w((uint16_t)(SCREEN_X + 2 * i));
			o.world_y = av.w((uint16_t)(SCREEN_Y + 2 * i));
			o.map_x = o.world_x / 64.0;
			o.map_y = 128.0 - o.world_y / 64.0;
			while (o.map_y < 0) o.map_y += 512;
			o.rom_offset = rom((uint16_t)(SCREEN_TYPES + i));
			o.note = i < 4 ? "palette 0 of the screen's colour set" : "palette 1 of the screen's colour set";
			objects.push_back(o);
		}
	}
}

// ------------------------------------------------------------------ editing

bool SpriteSet::encode_lists(const std::vector<ObjectList> &edited, std::vector<Patch> &out, std::string &err) const
{
	const RomView av = anim_view();
	auto bytes = [](const std::vector<std::pair<uint16_t, uint16_t>> &e) {
		std::vector<uint8_t> b;
		for (const auto &[t, d] : e)
		{
			b.push_back((uint8_t)t); b.push_back((uint8_t)(t >> 8));
			b.push_back((uint8_t)d); b.push_back((uint8_t)(d >> 8));
		}
		b.push_back(0xFF); b.push_back(0xFF);
		return b;
	};
	// town list: in place
	for (const auto &l : edited)
		if (l.source == ObjectSource::TownList)
		{
			if ((int)l.entries.size() > lists[0].capacity)
			{
				err = "the town list holds at most " + std::to_string(lists[0].capacity) + " objects";
				return false;
			}
			out.push_back({lists[0].rom_offset, bytes(l.entries)});
		}
	// stage lists: repacked into the space all of them use (consecutive in bank B 0x0400)
	std::vector<const ObjectList *> st(stages + 1, nullptr);
	bool any = false;
	for (const auto &l : edited)
		if (l.source == ObjectSource::StageList && l.stage >= 1 && l.stage <= stages) { st[l.stage] = &l; any = true; }
	if (!any) return true;
	uint16_t lo = 0xFFFF, hi = 0;
	for (const auto &l : lists)
		if (l.source == ObjectSource::StageList)
		{
			lo = std::min(lo, l.addr);
			hi = std::max(hi, (uint16_t)(l.addr + 4 * l.entries.size() + 2));
		}
	std::vector<uint8_t> image;
	std::vector<uint16_t> ptr(stages + 1, 0);
	std::map<std::vector<uint8_t>, uint16_t> shared;     // identical lists are stored once
	for (int s = 1; s <= stages; s++)
	{
		const ObjectList *l = st[s];
		for (const auto &o : lists)
			if (!l && o.source == ObjectSource::StageList && o.stage == s) l = &o;
		const auto b = bytes(l->entries);
		auto it = shared.find(b);
		if (it != shared.end()) { ptr[s] = it->second; continue; }
		ptr[s] = (uint16_t)(lo + image.size());
		shared[b] = ptr[s];
		image.insert(image.end(), b.begin(), b.end());
	}
	if (lo + image.size() > hi)
	{
		err = "the stage lists need " + std::to_string(image.size()) + " bytes, " + std::to_string(hi - lo) + " are available";
		return false;
	}
	image.resize(hi - lo, 0xFF);
	out.push_back({(uint32_t)av.offset(lo), image});
	std::vector<uint8_t> table;
	for (int s = 1; s <= stages; s++) { table.push_back((uint8_t)ptr[s]); table.push_back((uint8_t)(ptr[s] >> 8)); }
	out.push_back({(uint32_t)av.offset((uint16_t)(STAGE_LISTS + 2)), table});
	return true;
}

std::vector<SpriteSet::Patch> SpriteSet::encode_definition_position(int def, uint16_t x, uint16_t y) const
{
	const Definition d = definition(def);
	if (d.index < 0) return {};
	return { {d.x_rom, {(uint8_t)x, (uint8_t)(x >> 8)}}, {d.y_rom, {(uint8_t)y, (uint8_t)(y >> 8)}} };
}

// ------------------------------------------------------------------ load

bool SpriteSet::load(const RomSet &roms, std::string &err)
{
	m_roms = &roms;
	if (roms.main.size() < 0x90000) { err = "main ROM image too small"; return false; }
	const RomView av = anim_view();
	for (int p = 0; p < 4; p++)
		for (int k = 0; k < 16; k++)
			palette[p][15 - k] = av.w((uint16_t)(SPRITE_PALETTE + 0x20 * p + 2 * k));
	// colour sets of bank 0x1C00 (F:DC3C, F:DB4E)
	const RomView cv{&roms.main, 0x1800, 0x1C00};
	screen_palette = palette;
	for (int k = 0; k < 16; k++)
	{
		screen_palette[0][15 - k] = cv.w((uint16_t)(0x7F0C + 2 * k));
		screen_palette[1][15 - k] = cv.w((uint16_t)(0x7F4C + 2 * k));
		player2_palette[15 - k] = cv.w((uint16_t)(0x7F2C + 2 * k));
	}
	decode_frames();
	if (frames.empty()) { err = "no sprite frames found"; return false; }
	decode_types();
	name_frames();
	collect_animations();
	decode_objects();
	return true;
}

}   // namespace sprites
