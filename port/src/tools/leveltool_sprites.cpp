// leveltool commands for the sprite / map object decoder (port/src/level/sprites.h):
//   sprites OUTDIR                     sheets of all frames by category, the object types and a
//                                      text listing (frames, types, animations, objects)
//   objects LEVEL OUT.png [STAGE] [PALDUMP FRAME]
//                                      the level map with its objects (Skate City: the town
//                                      list, the fixed objects and the lists of STAGE 1-7, spawn
//                                      candidates as crosses); PALDUMP/FRAME: colours from a
//                                      render720 --pal-dump file
//   objcheck RAMDUMP                   compare the decoded frames and map objects with the
//                                      game's motion object RAM and object tables in every
//                                      frame of a render720 --ram-dump file
//   objshot RAMDUMP PALDUMP FRAME OUT.png
//                                      draw the objects of one dumped frame from the decoded
//                                      frames (compare with render720 --hide-pf --hide-alpha)
//   frames OUT.png PAL ID [ID ...]     the frames ID (hex) side by side with MO palette PAL (0-3)
//   objedit-test OUT.720mod            encoder tests: unchanged lists encode to the ROM bytes;
//                                      a demo edit (town list entry 21 becomes speakers, its
//                                      definition moves, one stage 1 object moves to stage 2)
//                                      is decoded back from the patched ROM and saved as a mod
#include "sprites.h"
#include "leveldata.h"
#include "board.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace sprites;

namespace {

// 3x5 font for labels: digits, A-Z (subset used), ':' '-' '.'
const char *glyph(char c)
{
	switch (c)
	{
	case '0': return "111101101101111"; case '1': return "010110010010111"; case '2': return "111001111100111";
	case '3': return "111001111001111"; case '4': return "101101111001001"; case '5': return "111100111001111";
	case '6': return "111100111101111"; case '7': return "111001010010010"; case '8': return "111101111101111";
	case '9': return "111101111001111"; case 'A': return "010101111101101"; case 'B': return "110101110101110";
	case 'C': return "011100100100011"; case 'D': return "110101101101110"; case 'E': return "111100110100111";
	case 'F': return "111100110100100"; case 'T': return "111010010010010"; case 'S': return "011100010001110";
	case 'K': return "101110100110101"; case 'U': return "101101101101111"; case 'N': return "111101101101101";
	case 'X': return "101101010101101"; case ':': return "000010000010000"; case '-': return "000000111000000";
	case '.': return "000000000000010"; case 'P': return "111101111100100"; case 'R': return "110101110101101";
	case 'O': return "111101101101111"; case 'L': return "100100100100111"; case 'I': return "111010010010111";
	case 'G': return "011100101101011"; case 'M': return "101111111101101"; case 'H': return "101101111101101";
	case 'Y': return "101101010010010"; case 'W': return "101101111111101"; case 'V': return "101101101101010";
	case 'J': return "001001001101010"; case 'Q': return "010101101110011"; case 'Z': return "111001010100111";
	default: return nullptr;
	}
}

struct Image {
	int w = 0, h = 0;
	std::vector<uint32_t> px;
	Image(int w_, int h_, uint32_t fill) : w(w_), h(h_), px((size_t)w_ * h_, fill) {}
	void put(int x, int y, uint32_t c) { if (x >= 0 && y >= 0 && x < w && y < h) px[(size_t)y * w + x] = c; }
	void text(int x, int y, const std::string &s, uint32_t c, int scale = 2)
	{
		for (char ch : s)
		{
			const char *g = glyph((char)toupper((unsigned char)ch));
			if (g)
				for (int i = 0; i < 15; i++)
					if (g[i] == '1')
						for (int sy = 0; sy < scale; sy++)
							for (int sx = 0; sx < scale; sx++) put(x + (i % 3) * scale + sx, y + (i / 3) * scale + sy, c);
			x += 4 * scale;
		}
	}
	void rect(int x0, int y0, int x1, int y1, uint32_t c)
	{
		for (int x = x0; x <= x1; x++) { put(x, y0, c); put(x, y1, c); }
		for (int y = y0; y <= y1; y++) { put(x0, y, c); put(x1, y, c); }
	}
	void cross(int x, int y, int r, uint32_t c)
	{
		for (int i = -r; i <= r; i++) { put(x + i, y + i, c); put(x + i, y - i, c); }
	}
	bool save(const std::string &path) const { return write_png(path, px.data(), w, h); }
};

std::string hex4(unsigned v) { char b[8]; snprintf(b, sizeof(b), "%04X", v & 0xFFFF); return b; }

// a sheet of frames packed in rows, each labelled with its id
void frame_sheet(const SpriteSet &ss, const GfxSet &gfx, const std::vector<const Frame *> &list, const std::string &path, int width = 1600)
{
	struct Pos { const Frame *f; int x, y; };
	std::vector<Pos> pos;
	int x = 4, y = 4, row_h = 0;
	for (const Frame *f : list)
	{
		const int fw = std::max(f->width(), 34) + 6, fh = f->height() + 14;
		if (x + fw > width) { x = 4; y += row_h; row_h = 0; }
		pos.push_back({f, x, y});
		x += fw;
		row_h = std::max(row_h, fh);
	}
	Image img(width, y + row_h + 4, 0xff283828);
	for (const auto &p : pos)
	{
		ss.draw(gfx, *p.f, img.px.data(), img.w, img.h, p.x - p.f->left, p.y - p.f->top);
		img.text(p.x, p.y + p.f->height() + 2, hex4(p.f->id), p.f->referenced ? 0xffffff40 : 0xffff8080);
	}
	img.save(path);
}

// level map image (as 'leveltool png')
Image render_map(const LevelMap &lm, const GfxSet &gfx)
{
	Image img(lm.cols * 128, lm.rows * 128, 0xff000000);
	for (int cy = 0; cy < lm.rows; cy++)
		for (int cx = 0; cx < lm.cols; cx++)
		{
			const LevelBlock &b = lm.blocks[lm.cell(cx, cy)];
			for (int h = 0; h < 2; h++)
				for (int ty = 0; ty < 8; ty++)
					for (int tx = 0; tx < 16; tx++)
					{
						const uint16_t w = b.half[h].tiles[ty * 16 + tx];
						const int code = ((((w >> 10) & 1 ? lm.half_bank1(b.half[h]) : lm.tile_bank[0]) << 10) | (w & 0x3ff)) & 8191;
						for (int py = 0; py < 8; py++)
							for (int px = 0; px < 8; px++)
								img.px[(size_t)(cy * 128 + h * 64 + ty * 8 + py) * img.w + cx * 128 + tx * 8 + px] =
									level_palette_rgb(lm.palette[128 + ((w >> 11) & 7) * 16 + gfx.tiles[code * 64 + py * 8 + px]]);
					}
		}
	return img;
}

// render720 --ram-dump record: main RAM 0x1000 bytes, motion object RAM 0x400 words, x/y scroll
constexpr size_t DUMP_REC = 0x1000 + 0x800 + 4;
struct Dump {
	std::vector<uint8_t> data;
	size_t frames() const { return data.size() / DUMP_REC; }
	const uint8_t *rec(size_t i) const { return &data[i * DUMP_REC]; }
	bool load(const std::string &path, size_t rec = DUMP_REC)
	{
		FILE *f = fopen(path.c_str(), "rb");
		if (!f) return false;
		fseek(f, 0, SEEK_END);
		const long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		data.resize((size_t)n / rec * rec);
		const bool ok = fread(data.data(), 1, data.size(), f) == data.size();
		fclose(f);
		return ok;
	}
};
uint16_t rw(const uint8_t *r, uint16_t a) { return (uint16_t)(r[a] | (r[a + 1] << 8)); }
uint16_t mo(const uint8_t *r, int i) { return rw(r, (uint16_t)(0x1000 + 2 * i)); }

// frame id an object shows: the word its animation pointer addresses (RAM or ROM, banks 0/0x400)
int object_frame(const RomSet &rs, const uint8_t *r, int slot)
{
	const uint16_t lp = rw(r, (uint16_t)(0x03B0 + 2 * slot));
	uint16_t v;
	if (lp < 0x0FFF) v = rw(r, lp);
	else if (lp >= 0x4000) { RomView av{&rs.main, 0, 0x0400}; v = av.w(lp); }
	else return -1;
	return v < 0x4000 ? v : -1;
}

}   // namespace

int sprite_command(const RomSet &rs, LevelSet &ls, const std::string &cmd, const std::vector<std::string> &args)
{
	if (cmd != "sprites" && cmd != "objects" && cmd != "objcheck" && cmd != "objshot" && cmd != "objedit-test" && cmd != "frames")
		return -1;
	SpriteSet ss;
	std::string err;
	if (!ss.load(rs, err)) { fprintf(stderr, "sprites: %s\n", err.c_str()); return 1; }
	GfxSet gfx;
	gfx.decode(rs);

	if (cmd == "frames")
	{
		if (args.size() < 3) { fprintf(stderr, "usage: leveltool frames OUT.png PAL ID [ID ...]\n"); return 1; }
		const int pal = atoi(args[1].c_str()) & 3;
		std::vector<const Frame *> list;
		for (size_t i = 2; i < args.size(); i++)
			if (const Frame *f = ss.frame((uint16_t)strtol(args[i].c_str(), nullptr, 16))) list.push_back(f);
		int w = 8, h = 8;
		for (const Frame *f : list) { w += f->width() + 8; h = std::max(h, f->height() + 16); }
		std::vector<uint32_t> img((size_t)w * h, 0xFF283828);
		int x = 8;
		for (const Frame *f : list)
		{
			ss.draw(gfx, *f, img.data(), w, h, x - f->left, 8 - f->top, ss.palette[pal].data());
			x += f->width() + 8;
		}
		write_png(args[0], img.data(), w, h);
		printf("%zu frames, palette %d -> %s\n", list.size(), pal, args[0].c_str());
		return 0;
	}
	if (cmd == "sprites")
	{
		if (args.empty()) { fprintf(stderr, "usage: leveltool sprites OUTDIR\n"); return 1; }
		const std::string dir = args[0];
		std::map<Category, std::vector<const Frame *>> bycat;
		int referenced = 0;
		for (const auto &f : ss.frames)
		{
			bycat[f.category].push_back(&f);
			referenced += f.referenced;
		}
		for (const auto &[cat, list] : bycat)
		{
			const std::string path = dir + "/sprites_" + category_name(cat) + ".png";
			frame_sheet(ss, gfx, list, path);
			printf("%-11s %4zu frames -> %s\n", category_name(cat), list.size(), path.c_str());
		}
		printf("%zu frames (%d referenced by the animation data, %zu unused), %zu animations\n",
			ss.frames.size(), referenced, ss.frames.size() - referenced, ss.animations.size());
		// types: one row per type with its frames for each direction (or its frames)
		{
			std::vector<std::pair<int, std::vector<const Frame *>>> rows;
			for (const auto &ty : ss.types)
			{
				std::vector<const Frame *> fl;
				std::set<int> seen;
				if (ty.directional)
				{
					for (int d = 0; d < 16; d++)
						if (ty.dir_frame[d] >= 0 && seen.insert(ty.dir_frame[d]).second) fl.push_back(ss.frame((uint16_t)ty.dir_frame[d]));
				}
				for (uint16_t id : ty.frames)
					if ((int)fl.size() < 24 && seen.insert(id).second) fl.push_back(ss.frame(id));
				if (!fl.empty()) rows.push_back({ty.type, fl});
			}
			int H = 4;
			for (const auto &r : rows)
			{
				int h = 0;
				for (const Frame *f : r.second) h = std::max(h, f->height());
				H += h + 18;
			}
			Image img(1900, H, 0xff283828);
			int y = 4;
			for (const auto &r : rows)
			{
				int h = 0, x = 60;
				for (const Frame *f : r.second) h = std::max(h, f->height());
				img.text(4, y + 2, "T" + std::to_string(r.first), 0xffffffff);
				for (const Frame *f : r.second)
				{
					if (x + f->width() > img.w) break;
					ss.draw(gfx, *f, img.px.data(), img.w, img.h, x - f->left, y - f->top,
						f->screen_colours ? nullptr : ss.palette[ss.types[r.first].palette].data());
					img.text(x, y + h + 3, hex4(f->id), 0xffffff40, 1);
					x += std::max(f->width(), 20) + 4;
				}
				y += h + 18;
			}
			img.save(dir + "/sprite_types.png");
		}
		// listing
		FILE *t = fopen((dir + "/sprites.txt").c_str(), "w");
		if (!t) { fprintf(stderr, "cannot write %s/sprites.txt\n", dir.c_str()); return 1; }
		fprintf(t, "# frames: id category palette size(bytes) bbox(l t r b) pieces [types] name\n");
		for (const auto &f : ss.frames)
		{
			fprintf(t, "%04X %-10s p%d %3d (%d %d %d %d) %2zu [", f.id, category_name(f.category), f.palette, f.size,
				f.left, f.top, f.right, f.bottom, f.pieces.size());
			for (size_t i = 0; i < f.types.size(); i++) fprintf(t, "%s%d", i ? "," : "", f.types[i]);
			fprintf(t, "] %s", f.name.c_str());
			if (f.motion) fprintf(t, "  velocity (%d, %d) step %d", f.vx, f.vy, f.step);
			fprintf(t, "\n");
		}
		fprintf(t, "\n# types: type props flags mode rate pieces palette offset anim anim2 name\n");
		for (const auto &ty : ss.types)
		{
			fprintf(t, "%3d %02X %02X %2d %3d %2d p%d (%d,%d) %04X %04X %s%s", ty.type, ty.props, ty.flags, ty.mode, ty.rate,
				ty.pieces, ty.palette, ty.off_x, ty.off_y, ty.anim, ty.anim2, ty.name.c_str(), ty.directional ? "  dirs:" : "");
			for (int d = 0; d < 16 && ty.directional; d++)
				if (ty.dir_frame[d] >= 0) fprintf(t, " %d=%04X/%04X", d, ty.dir_list[d], ty.dir_frame[d]);
			if (!ty.directional && ty.first_frame >= 0) fprintf(t, "  first frame %04X", ty.first_frame);
			fprintf(t, "\n");
		}
		fprintf(t, "\n# animations: address rate loop frames name\n");
		for (const auto &an : ss.animations)
		{
			fprintf(t, "%04X %2d %3d ", an.addr, an.rate, an.loop);
			for (uint16_t f : an.frames) fprintf(t, "%04X ", f);
			if (an.next) fprintf(t, "-> %04X ", an.next);
			fprintf(t, " %s\n", an.name.c_str());
		}
		fprintf(t, "\n# map objects: source stage index level type descriptor(dyn) cell def world map dir anim frame rom note\n");
		for (const auto &o : ss.objects)
			fprintf(t, "%-13s %d %2d L%d %3d %04X%s c%02d r%02d +%d def %4d w(%04X,%04X) map(%.1f,%.1f) dir %d anim %04X frame %04X rom %05X %s %s\n",
				source_name(o.source), o.stage, o.index, o.level, o.type, o.descriptor, o.dynamic ? "d" : " ", o.cell_col,
				o.cell_row, o.def_offset, o.definition, o.world_x, o.world_y, o.map_x, o.map_y, o.direction, o.anim,
				o.frame & 0xFFFF, o.rom_offset, ss.types[o.type < 101 ? o.type : 0].name.c_str(), o.note.c_str());
		fprintf(t, "\n# spawn kinds: kind list candidates counts(stage 1-7) types(stage 1-7)\n");
		for (const auto &sk : ss.spawns)
		{
			fprintf(t, "%2d %04X %2zu  counts", sk.kind, sk.list, sk.descriptors.size());
			for (int s = 1; s <= ss.stages; s++) fprintf(t, " %2d", sk.count[s]);
			fprintf(t, "  types");
			for (int s = 1; s <= ss.stages; s++) fprintf(t, " %3d", sk.type[s]);
			fprintf(t, "\n");
		}
		fclose(t);
		printf("wrote %s/sprites.txt, %s/sprite_types.png\n", dir.c_str(), dir.c_str());
		return 0;
	}

	if (cmd == "objects")
	{
		if (args.size() < 2) { fprintf(stderr, "usage: leveltool objects LEVEL OUT.png [STAGE] [PALDUMP FRAME]\n"); return 1; }
		const int level = atoi(args[0].c_str()), stage = args.size() > 2 ? atoi(args[2].c_str()) : 1;
		if (args.size() > 4)
		{
			Dump pd;
			if (!pd.load(args[3], 0x200)) { fprintf(stderr, "cannot read %s\n", args[3].c_str()); return 1; }
			const size_t fr = (size_t)atoi(args[4].c_str());
			if (fr >= pd.data.size() / 0x200) { fprintf(stderr, "no frame %zu\n", fr); return 1; }
			uint16_t pal[256];
			memcpy(pal, &pd.data[fr * 0x200], 0x200);
			ls.set_palette(level, pal);
		}
		const LevelMap *lm = nullptr;
		for (const auto &m : ls.maps)
			if (m.level == level && !lm) lm = &m;
		if (!lm) { fprintf(stderr, "no level %d\n", level); return 1; }
		Image img = render_map(*lm, gfx);
		// cell grid of the map objects (2 x 2 blocks)
		if (level == 0)
			for (int y = 0; y < img.h; y++)
				for (int x = 0; x < img.w; x++)
					if (!(x % 256) || !(y % 256)) img.put(x, y, 0xff606060);
		// spawn candidates of the stage
		static const uint32_t kind_colour[4] = { 0xffff4040, 0xff40ff40, 0xff4080ff, 0xffffff40 };
		if (level == 0)
			for (const auto &sk : ss.spawns)
			{
				if (sk.kind <= 9 && !sk.count[stage]) continue;
				for (uint16_t d : sk.descriptors)
				{
					double x, y;
					if (ss.descriptor_position(d, sk.dynamic, sk.type[stage], x, y))
						img.cross((int)x, (int)y, 3, kind_colour[sk.kind & 3]);
				}
			}
		int shown = 0;
		printf("source        stage idx type  desc  cell     def   map (x, y)      dir frame name\n");
		for (const auto &o : ss.objects)
		{
			if (o.level != level) continue;
			if ((o.source == ObjectSource::StageList || o.source == ObjectSource::StageSkaters) && o.stage != stage) continue;
			printf("%-13s %d %3d %4d  %04X  c%02d r%02d %5d  (%7.1f, %7.1f) %3d %04X  %s %s\n", source_name(o.source), o.stage,
				o.index, o.type, o.descriptor, o.cell_col, o.cell_row, o.definition, o.map_x, o.map_y, o.direction,
				o.frame & 0xFFFF, ss.types[o.type < 101 ? o.type : 0].name.c_str(), o.note.c_str());
			if (o.definition < 0 && o.source != ObjectSource::Screen) continue;
			const Frame *f = o.frame >= 0 ? ss.frame((uint16_t)o.frame) : nullptr;
			const int x = (int)o.map_x, y = (int)o.map_y;
			if (f && !(ss.types[o.type < 101 ? o.type : 0].props & 0x40))
			{
				const bool screen = o.source == ObjectSource::Screen;
				const int pal = screen ? (o.index < 4 ? 0 : 1) : ss.types[o.type].palette;
				ss.draw(gfx, *f, img.px.data(), img.w, img.h, x, y, (screen ? ss.screen_palette : ss.palette)[pal].data());
				img.rect(x + f->left - 1, y + f->top - 1, x + f->right, y + f->bottom, 0xffffffff);
			}
			else
				img.rect(x - 6, y - 6, x + 6, y + 6, 0xffff00ff);
			img.cross(x, y, 4, 0xffffffff);
			img.text(x + 6, y + 4, "T" + std::to_string(o.type), 0xffffffff);
			shown++;
		}
		printf("%d objects drawn\n", shown);
		return img.save(args[1]) ? 0 : 1;
	}

	if (cmd == "objcheck")
	{
		if (args.empty()) { fprintf(stderr, "usage: leveltool objcheck RAMDUMP\n"); return 1; }
		Dump d;
		if (!d.load(args[0])) { fprintf(stderr, "cannot read %s\n", args[0].c_str()); return 1; }
		long objs = 0, ok = 0, prev = 0, unbuilt = 0, stale = 0, bad = 0, nonframe = 0, unknown = 0;
		std::set<uint16_t> shown;
		std::map<std::string, long> bad_by;
		long mobj = 0, mfound = 0, mpos = 0, mmoved = 0, mnotfound = 0;
		std::map<int, std::pair<long, long>> pos_by_type;   // type -> (exact, differs)
		// descriptors of the decoded objects (and spawn candidates) per class
		std::set<std::pair<int, uint32_t>> known;           // (dynamic, type << 16 | desc)
		for (const auto &o : ss.objects) known.insert({o.dynamic, (uint32_t)(o.type << 16) | o.descriptor});
		for (const auto &sk : ss.spawns)
			for (int s = 1; s <= ss.stages; s++)
				for (uint16_t dd : sk.descriptors) known.insert({sk.dynamic, (uint32_t)(sk.type[s] << 16) | dd});
		for (const auto &e : ss.entry_types)
			for (int dyn = 0; dyn < 2; dyn++)
				for (int t : {e.closed_type, e.open_type}) known.insert({dyn != 0, (uint32_t)(t << 16) | e.descriptor});
		std::map<std::string, long> unknown_by;
		for (size_t i = 0; i < d.frames(); i++)
		{
			const uint8_t *r = d.rec(i);
			for (int s = 0; s < 32; s++)
			{
				const uint8_t flags = r[0x0490 + s], pieces = r[0x04D0 + s];
				if (!(flags & 0x80) || !pieces || (flags & 0x06) == 0) continue;   // shown, in a draw list
				objs++;
				const int id = object_frame(rs, r, s);
				if (id < 0) { nonframe++; continue; }
				const Frame *f = ss.frame((uint16_t)id);
				if (!f) { unknown++; continue; }
				shown.insert((uint16_t)id);
				// the object's motion objects, from its first link, against the object's state at
				// the end of this frame or of the previous one (list 1 is rebuilt by the first 32V
				// interrupt, before the objects move)
				auto check = [&](const uint8_t *st, const Frame *f) {
				int e = rw(r, (uint16_t)(0x0888 + 2 * s)) >> 3;
				const uint16_t sx = rw(st, (uint16_t)(0x0630 + 2 * s)), sy = rw(st, (uint16_t)(0x0670 + 2 * s));
				std::string why;
				for (int k = 0; k < pieces && why.empty(); k++)
				{
					const uint16_t e0 = mo(r, 4 * e), e1 = mo(r, 4 * e + 1), e2 = mo(r, 4 * e + 2), e3 = mo(r, 4 * e + 3);
					if (k < (int)f->pieces.size())
					{
						const Piece &p = f->pieces[k];
						const int yoff = -(p.y + 16 * p.tiles);
						const uint16_t word = (uint16_t)((p.code & 0x7FF) | ((p.tiles - 1) << 11) | (p.hflip ? 0x4000 : 0));
						if ((e1 & 0x7FFF) != word) why = "picture";
						else if ((e0 & 7) != (f->mode & 7)) why = "bank";
						else if ((uint16_t)(e0 & ~7) != (uint16_t)((sy & ~7) + yoff * 64)) why = "y";
						else if (k == 0 && e2 != (uint16_t)(sx + f->x_offset * 64)) why = "x";
						else if (k > 0 && !(e1 & 0x8000)) why = "column link";
					}
					else if (e1 != 0) why = "unused piece";
					e = (e3 >> 3) & 0xFF;
				}
				return why;
				};
				std::string why = check(r, f);
				if (!why.empty() && i > 0)
				{
					const int pid = object_frame(rs, d.rec(i - 1), s);
					const Frame *pf = pid >= 0 ? ss.frame((uint16_t)pid) : nullptr;
					if (pf && check(d.rec(i - 1), pf).empty()) { prev++; continue; }
				}
				if (why.empty()) ok++;
				else if (!mo(r, 4 * (rw(r, (uint16_t)(0x0888 + 2 * s)) >> 3) + 1)) unbuilt++;   // pieces not built yet
				else if (flags & 0x18) stale++;              // rebuild pending: the RAM shows an older frame
				else { bad++; bad_by[why]++; if (bad <= 5) printf("frame %zu slot %d frame %04X: %s differs\n", i, s, id, why.c_str()); }
			}
			// map objects with an object slot
			if (r[0x0F59] != 0) continue;                    // Skate City only
			for (int n = 0; n < 0xFF; n++)
			{
				const uint16_t desc = rw(r, (uint16_t)(0x0AF8 + 2 * n));
				if (!(desc & 0x8000)) continue;
				const int slot = r[0x09B8 + n] & 0x3F, type = r[0x0370 + slot];
				if (!type || type > 100) continue;
				mobj++;
				const bool dyn = n >= 0x40;
				if (!known.count({dyn, (uint32_t)(type << 16) | (desc & 0x7FFF)}))
				{
					mnotfound++;
					unknown_by[std::string(ss.types[type].mode == 1 || ss.types[type].mode == 4 ? "moving " : "") + "type " + std::to_string(type)]++;
					continue;
				}
				mfound++;
				// expected world position, with the town's wrap adjustment of F:BF5E
				double mx, my;
				if (!ss.descriptor_position(desc & 0x7FFF, dyn, type, mx, my)) continue;
				const int cell = desc & 0x1FF;
				int row = (int8_t)r[0x0E9B];
				if ((cell & 15) > (int16_t)rw(r, 0x099C)) row++;
				const int col = cell >> 4;
				const uint16_t ex = (uint16_t)((((col + row) & 3) - (col & 3)) * 0x4000 + (int)((mx - 256 * col) * 64 + 0.5) + 0x4000 * col);
				const uint16_t ey = (uint16_t)(0x6000 - 0x4000 * (cell & 15) + (int)((256 * ((cell & 15) + 1) - my) * 64 + 0.5));
				const uint16_t ax = rw(r, (uint16_t)(0x06F0 + 2 * slot)), ay = rw(r, (uint16_t)(0x0730 + 2 * slot));
				if (ax == ex && ay == ey) { mpos++; pos_by_type[type].first++; }
				else { mmoved++; pos_by_type[type].second++; }
			}
		}
		printf("%zu dumped frames: %ld shown objects; motion object RAM = decoded frame at the object's position: %ld, "
			"the same for the previous frame's state: %ld, pieces not built yet: %ld, stale (rebuild pending): %ld, differ: %ld; without a frame pointer %ld, unknown frames %ld\n",
			d.frames(), objs, ok, prev, unbuilt, stale, bad, nonframe, unknown);
		for (const auto &[why, n] : bad_by) printf("  differ in %s: %ld\n", why.c_str(), n);
		printf("%zu distinct frames shown\n", shown.size());
		printf("map objects with a slot (Skate City): %ld, %ld found in the decoded lists / spawn tables, %ld not found; at the decoded position: %ld, elsewhere (moving): %ld\n",
			mobj, mfound, mnotfound, mpos, mmoved);
		for (const auto &[k, n] : unknown_by) printf("  not in the lists: %s (%ld)\n", k.c_str(), n);
		for (const auto &[t, c] : pos_by_type)
			printf("  type %3d %-24s at decoded position %6ld  elsewhere %6ld\n", t, ss.types[t].name.c_str(), c.first, c.second);
		return bad || unknown ? 2 : 0;
	}

	if (cmd == "objshot")
	{
		if (args.size() < 4) { fprintf(stderr, "usage: leveltool objshot RAMDUMP PALDUMP FRAME OUT.png\n"); return 1; }
		Dump d, pd;
		if (!d.load(args[0]) || !pd.load(args[1], 0x200)) { fprintf(stderr, "cannot read the dumps\n"); return 1; }
		const size_t fi = (size_t)atoi(args[2].c_str());
		if (fi >= d.frames() || fi >= pd.data.size() / 0x200) { fprintf(stderr, "no frame %zu\n", fi); return 1; }
		const uint8_t *r = d.rec(fi);
		const uint16_t *pal = (const uint16_t *)&pd.data[fi * 0x200];
		// draw order: the motion object list from entry 0, by the object owning each entry
		std::map<int, int> owner;
		for (int s = 0; s < 32; s++)
		{
			if (!(r[0x0490 + s] & 0x80) || !r[0x04D0 + s]) continue;
			int e = rw(r, (uint16_t)(0x0888 + 2 * s)) >> 3;
			for (int k = 0; k < r[0x04D0 + s]; k++) { owner[e] = s; e = (mo(r, 4 * e + 3) >> 3) & 0xFF; }
		}
		std::vector<int> order;
		std::set<int> done, visited;
		for (int e = 0, n = 0; n < 256 && visited.insert(e).second; n++, e = (mo(r, 4 * e + 3) >> 3) & 0xFF)
			if (owner.count(e) && done.insert(owner[e]).second) order.push_back(owner[e]);
		// objects of the other draw list (the hardware list is switched during the frame): in the
		// order of the game's draw lists (heads 0x0988 / 0x0989, links 0x0808)
		for (int list = 0; list < 2; list++)
			for (int o = (int8_t)r[0x0988 + list], n = 0; o >= 0 && n < 64; o = (int8_t)r[0x0808 + o], n++)
				if (done.insert(o & 0x1F).second && (r[0x0490 + (o & 0x1F)] & 0x80)) order.push_back(o & 0x1F);
		Image img(Board::SCREEN_W, Board::SCREEN_H, 0xff000000);
		// owner of each pixel: slot + 1 of the object drawn there, 255 where objects overlap
		std::vector<uint8_t> ids((size_t)img.w * img.h, 0);
		for (int s : order)
		{
			const int id = object_frame(rs, r, s);
			const Frame *f = id >= 0 ? ss.frame((uint16_t)id) : nullptr;
			if (!f) continue;
			const int p = (r[0x0470 + s] >> 4) & 3;
			const int x = (rw(r, (uint16_t)(0x0630 + 2 * s)) >> 6) & 0x3FF;
			const int y = -((rw(r, (uint16_t)(0x0670 + 2 * s)) >> 6) & 0x1FF);
			// wrap like the motion object hardware (x 1024, y 512)
			const int xx = x >= 640 ? x - 1024 : x, yy = ((y % 512) + 512) % 512;
			Image layer(img.w, img.h, 0);
			for (int dy : {-512, 0})
			{
				ss.draw(gfx, *f, img.px.data(), img.w, img.h, xx, yy + dy, &pal[16 * p]);
				ss.draw(gfx, *f, layer.px.data(), layer.w, layer.h, xx, yy + dy, &pal[16 * p]);
			}
			for (size_t k = 0; k < ids.size(); k++)
				if (layer.px[k]) ids[k] = ids[k] ? 255 : (uint8_t)(s + 1);
			printf("slot %2d type %3d frame %04X palette %d at (%d, %d) %s\n", s, r[0x0370 + s], f->id, p, xx, yy, f->name.c_str());
		}
		if (FILE *o = fopen((args[3] + ".ids").c_str(), "wb")) { fwrite(ids.data(), 1, ids.size(), o); fclose(o); }
		return img.save(args[3]) ? 0 : 1;
	}
	if (cmd == "objedit-test")
	{
		if (args.empty()) { fprintf(stderr, "usage: leveltool objedit-test OUT.720mod\n"); return 1; }
		int fails = 0;
		// 1. the unchanged lists encode to the bytes in the ROM
		std::vector<SpriteSet::Patch> same;
		if (!ss.encode_lists(ss.lists, same, err)) { fprintf(stderr, "encode: %s\n", err.c_str()); return 1; }
		for (const auto &p : same)
			if (memcmp(&rs.main[p.offset], p.data.data(), p.data.size()))
			{
				printf("unchanged lists: patch at %05X (%zu bytes) differs from the ROM\n", p.offset, p.data.size());
				fails++;
			}
		printf("unchanged lists: %zu patches, %s\n", same.size(), fails ? "DIFFER" : "identical to the ROM");
		// map_to_cell is the inverse of the decoded positions
		{
			int bad = 0, n = 0;
			for (const auto &o : ss.objects)
			{
				if (o.definition < 0 || o.source == ObjectSource::Screen) continue;
				int col, row;
				uint16_t dx, dy;
				ss.map_to_cell(o.map_x, o.map_y, o.type, col, row, dx, dy);
				const Definition d = ss.definition(o.definition);
				n++;
				if (col != o.cell_col || row != o.cell_row || dx != d.x || dy != d.y)
				{
					bad++;
					printf("  def %d (%04X, %04X) cell %d,%d -> %d,%d (%04X, %04X)", o.definition, d.x, d.y, o.cell_col, o.cell_row, col, row, dx, dy);
					putchar(10);
				}
			}
			printf("map_to_cell: %d of %d object positions give back their cell and definition offsets", n - bad, n);
			putchar(10);
			fails += bad;
		}
		// 2. an edit
		std::vector<ObjectList> edited = ss.lists;
		ObjectList *town = nullptr, *s1 = nullptr, *s2 = nullptr;
		for (auto &l : edited)
		{
			if (l.source == ObjectSource::TownList) town = &l;
			if (l.source == ObjectSource::StageList && l.stage == 1) s1 = &l;
			if (l.source == ObjectSource::StageList && l.stage == 2) s2 = &l;
		}
		town->entries[21].first = 87;                    // a disc becomes speakers
		const auto moved = s1->entries.back();           // the last stage 1 object goes to stage 2
		s1->entries.pop_back();
		s2->entries.push_back(moved);
		std::vector<SpriteSet::Patch> patches;
		if (!ss.encode_lists(edited, patches, err)) { fprintf(stderr, "encode: %s\n", err.c_str()); return 1; }
		int def = -1;
		for (const auto &o : ss.objects)
			if (o.source == ObjectSource::TownList && o.index == 21) def = o.definition;
		const Definition d0 = ss.definition(def);
		for (auto &p : ss.encode_definition_position(def, (uint16_t)(d0.x + 0x0400), d0.y)) patches.push_back(p);
		ModPatch mp;
		for (const auto &p : patches) mp.chunks.push_back({p.offset, p.data});
		RomSet patched = rs;
		mp.apply(patched);
		SpriteSet ps;
		if (!ps.load(patched, err)) { fprintf(stderr, "patched: %s\n", err.c_str()); return 1; }
		auto find = [](const SpriteSet &set, ObjectSource src, int stage, int index) -> const MapObject * {
			for (const auto &o : set.objects)
				if (o.source == src && o.stage == stage && o.index == index) return &o;
			return nullptr;
		};
		const MapObject *a = find(ss, ObjectSource::TownList, 0, 21), *b = find(ps, ObjectSource::TownList, 0, 21);
		// expected: 16 pixels right (the definition moved by 0x400) plus the change of the type's x offset
		const double ex = a->map_x + 16.0 + (ss.types[87].off_x - ss.types[a->type].off_x) / 64.0;
		const bool t_ok = b && b->type == 87 && b->descriptor == a->descriptor && b->map_x == ex;
		printf("town list entry 21: type %d -> %d, map x %.1f -> %.1f (expected %.1f): %s",
			a->type, b ? b->type : -1, a->map_x, b ? b->map_x : 0.0, ex, t_ok ? "ok" : "WRONG");
		putchar(10);
		fails += !t_ok;
		int n1 = 0, n2 = 0;
		bool found = false;
		for (const auto &o : ps.objects)
		{
			if (o.source == ObjectSource::StageList && o.stage == 1) n1++;
			if (o.source == ObjectSource::StageList && o.stage == 2) { n2++; found |= o.type == moved.first && o.descriptor == moved.second; }
		}
		printf("stage 1: %zu -> %d objects, stage 2: %zu -> %d objects, moved object in stage 2: %s\n",
			ss.lists[1].entries.size(), n1, ss.lists[2].entries.size(), n2, found ? "yes" : "NO");
		fails += !(n1 == (int)ss.lists[1].entries.size() - 1 && n2 == (int)ss.lists[2].entries.size() + 1 && found);
		// the other stages are unchanged
		for (int st = 3; st <= ss.stages; st++)
		{
			std::vector<std::pair<int, uint16_t>> x, y;
			for (const auto &o : ss.objects) if (o.source == ObjectSource::StageList && o.stage == st) x.push_back({o.type, o.descriptor});
			for (const auto &o : ps.objects) if (o.source == ObjectSource::StageList && o.stage == st) y.push_back({o.type, o.descriptor});
			if (x != y) { printf("stage %d changed\n", st); fails++; }
		}
		if (!mp.save(args[0])) { fprintf(stderr, "cannot write %s\n", args[0].c_str()); return 1; }
		printf("wrote %s (%zu chunks); %s\n", args[0].c_str(), mp.chunks.size(), fails ? "FAILED" : "all checks passed");
		return fails ? 2 : 0;
	}
	return -1;
}
