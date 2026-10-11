// Pieces for the level editor, see pieces.h.
#include "pieces.h"
#include "board.h"
#include <algorithm>
#include <map>
#include <tuple>

namespace {

constexpr int MAX_W = 20, MAX_H = 20;              // larger structures are cut up
constexpr int CHUNK = 16;                           // ... into parts of at most a block
constexpr size_t MIN_TILES = 4;                     // smaller bits are left to the single tiles

bool is_placeholder(uint16_t w) { return (w & 0x7ff) < 16; }

// the tile code the game draws for a word (placeholders keep their code: they are floor)
int tile_code(const LevelMap &lm, uint16_t w, int bank1)
{
	const int bank = (w >> 10) & 1 ? bank1 : lm.tile_bank[0];
	return ((bank << 10) | (w & 0x3ff)) & 8191;
}

struct Grid {
	int w = 0, h = 0;
	std::vector<uint16_t> word;
	std::vector<uint8_t> bank1;
	std::vector<PieceKind> kind;
};

Grid build_grid(const RomSet &roms, const LevelMap &lm)
{
	Grid g;
	g.w = lm.cols * 16;
	g.h = lm.rows * 16;
	g.word.assign((size_t)g.w * g.h, 0);
	g.bank1.assign(g.word.size(), 0);
	g.kind.assign(g.word.size(), PieceKind::Floor);
	for (int cy = 0; cy < lm.rows; cy++)
		for (int cx = 0; cx < lm.cols; cx++)
		{
			const int bi = lm.cell(cx, cy);
			if (bi < 0) continue;
			const LevelBlock &b = lm.blocks[bi];
			for (int h = 0; h < 2; h++)
			{
				const int bank1 = lm.half_bank1(b.half[h]);
				for (int ty = 0; ty < 8; ty++)
					for (int tx = 0; tx < 16; tx++)
					{
						const size_t i = (size_t)(cy * 16 + h * 8 + ty) * g.w + cx * 16 + tx;
						const uint16_t w = b.half[h].tiles[ty * 16 + tx];
						g.word[i] = w;
						g.bank1[i] = (uint8_t)bank1;
						// placeholders are open floor where they are flat ground; on ramps and
						// platforms they are the surface of the structure
						const PieceKind k = terrain_kind(tile_terrain(roms, lm, w, bank1));
						g.kind[i] = is_placeholder(w) && k == PieceKind::Marking ? PieceKind::Floor : k;
					}
			}
		}
	return g;
}

// the dominant kind of a structure: slopes make it a ramp, then raised, pits, walls
PieceKind structure_kind(const int count[(int)PieceKind::Count], int total)
{
	if (count[(int)PieceKind::Ramp] * 10 >= total) return PieceKind::Ramp;
	if (count[(int)PieceKind::Ledge] * 4 >= total) return PieceKind::Ledge;
	if (count[(int)PieceKind::Pit] * 4 >= total) return PieceKind::Pit;
	if (count[(int)PieceKind::Wall] * 4 >= total) return PieceKind::Wall;
	return PieceKind::Marking;
}

struct Region { std::vector<int> cells; int x0, y0, x1, y1; };

// connected regions of the cells accepted by 'same' (8-neighbourhood), within 'mask'
template <class Same>
std::vector<Region> regions(const Grid &g, const std::vector<int> &cells, std::vector<int> &mark, int stamp, Same same)
{
	std::vector<Region> out;
	std::vector<int> stack;
	for (int start : cells)
	{
		if (mark[start] != stamp) continue;
		Region r{{}, g.w, g.h, -1, -1};
		mark[start] = stamp + 1;
		stack.push_back(start);
		while (!stack.empty())
		{
			const int c = stack.back();
			stack.pop_back();
			r.cells.push_back(c);
			const int x = c % g.w, y = c / g.w;
			r.x0 = std::min(r.x0, x); r.y0 = std::min(r.y0, y);
			r.x1 = std::max(r.x1, x); r.y1 = std::max(r.y1, y);
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++)
				{
					const int nx = x + dx, ny = y + dy;
					if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= g.w || ny >= g.h) continue;
					const int n = ny * g.w + nx;
					if (mark[n] == stamp && same(c, n))
					{
						mark[n] = stamp + 1;
						stack.push_back(n);
					}
				}
		}
		out.push_back(std::move(r));
	}
	return out;
}

Piece make_piece(const Grid &g, const std::vector<int> &cells, int x0, int y0, int x1, int y1, int map)
{
	Piece p;
	p.w = x1 - x0 + 1;
	p.h = y1 - y0 + 1;
	p.tiles.assign((size_t)p.w * p.h, PIECE_EMPTY);
	p.bank1.assign(p.tiles.size(), 0);
	int count[(int)PieceKind::Count] = {};
	for (int c : cells)
	{
		const int x = c % g.w, y = c / g.w;
		if (x < x0 || x > x1 || y < y0 || y > y1) continue;
		const size_t i = (size_t)(y - y0) * p.w + (x - x0);
		p.tiles[i] = g.word[c];
		p.bank1[i] = g.bank1[c];
		count[(int)g.kind[c]]++;
	}
	p.kind = structure_kind(count, (int)cells.size());
	p.map = map;
	p.x = x0;
	p.y = y0;
	return p;
}

// cut a region into pieces of at most CHUNK x CHUNK tiles
void add_chunks(const Grid &g, const Region &r, int map, std::vector<Piece> &out)
{
	for (int y = r.y0; y <= r.y1; y += CHUNK)
		for (int x = r.x0; x <= r.x1; x += CHUNK)
		{
			std::vector<int> part;
			int x0 = g.w, y0 = g.h, x1 = -1, y1 = -1;
			for (int c : r.cells)
			{
				const int cx = c % g.w, cy = c / g.w;
				if (cx < x || cx >= x + CHUNK || cy < y || cy >= y + CHUNK) continue;
				part.push_back(c);
				x0 = std::min(x0, cx); y0 = std::min(y0, cy);
				x1 = std::max(x1, cx); y1 = std::max(y1, cy);
			}
			if (part.size() >= MIN_TILES) out.push_back(make_piece(g, part, x0, y0, x1, y1, map));
		}
}

}

const char *piece_kind_name(PieceKind k)
{
	switch (k)
	{
	case PieceKind::Ramp: return "Ramps and slopes";
	case PieceKind::Ledge: return "Ledges, platforms and blocks";
	case PieceKind::Pit: return "Pits, water and dips";
	case PieceKind::Wall: return "Walls and obstacles";
	case PieceKind::Marking: return "Ground markings and decoration";
	case PieceKind::Floor: return "Floor";
	default: return "?";
	}
}

int tile_terrain(const RomSet &roms, const LevelMap &lm, uint16_t w, int bank1)
{
	int code = tile_code(lm, w, bank1);
	const int low = w & 0x7ff;
	if (low < 16 && lm.subst[low]) code = tile_code(lm, (uint16_t)(lm.subst[low] | (w & 0xf800)), bank1);
	const size_t off = (size_t)LevelSet::rom_bank_offset(0x0800) + code;
	return off < roms.main.size() ? roms.main[off] : 0;
}

PieceKind terrain_kind(int t)
{
	if (t & 0x80) return PieceKind::Wall;
	if (t & 7) return PieceKind::Ramp;
	const int level = (t >> 3) & 15;
	if (level < 3) return PieceKind::Pit;
	if (level > 3) return PieceKind::Ledge;
	return PieceKind::Marking;
}

std::vector<Piece> extract_pieces(const RomSet &roms, const std::vector<const LevelMap *> &maps)
{
	std::vector<Piece> found;
	for (size_t mi = 0; mi < maps.size(); mi++)
	{
		const Grid g = build_grid(roms, *maps[mi]);
		std::vector<int> content;
		for (int i = 0; i < g.w * g.h; i++)
			if (g.kind[i] != PieceKind::Floor) content.push_back(i);
		std::vector<int> mark(g.word.size(), -1);
		for (int c : content) mark[c] = 0;
		const auto structures = regions(g, content, mark, 0, [](int, int) { return true; });
		for (const Region &r : structures)
		{
			if (r.cells.size() < MIN_TILES) continue;
			if (r.x1 - r.x0 < MAX_W && r.y1 - r.y0 < MAX_H)
			{
				found.push_back(make_piece(g, r.cells, r.x0, r.y0, r.x1, r.y1, (int)mi));
				continue;
			}
			// a large structure: its parts of one kind of terrain, cut to block size if large
			for (int c : r.cells) mark[c] = 2;
			const auto parts = regions(g, r.cells, mark, 2, [&](int a, int b) { return g.kind[a] == g.kind[b]; });
			for (const Region &p : parts)
			{
				if (p.cells.size() < MIN_TILES) continue;
				if (p.x1 - p.x0 < MAX_W && p.y1 - p.y0 < MAX_H)
					found.push_back(make_piece(g, p.cells, p.x0, p.y0, p.x1, p.y1, (int)mi));
				else
					add_chunks(g, p, (int)mi, found);
			}
		}
	}
	// the same piece found again counts as a use
	std::map<std::tuple<int, int, std::vector<uint16_t>>, size_t> seen;
	std::vector<Piece> out;
	for (Piece &p : found)
	{
		auto key = std::make_tuple(p.w, p.h, p.tiles);
		auto it = seen.find(key);
		if (it != seen.end()) { out[it->second].uses++; continue; }
		p.uses = 1;
		seen.emplace(std::move(key), out.size());
		out.push_back(std::move(p));
	}
	std::stable_sort(out.begin(), out.end(), [](const Piece &a, const Piece &b) {
		if (a.kind != b.kind) return a.kind < b.kind;
		const int fa = (int)std::count_if(a.tiles.begin(), a.tiles.end(), [](uint16_t t) { return t != PIECE_EMPTY; });
		const int fb = (int)std::count_if(b.tiles.begin(), b.tiles.end(), [](uint16_t t) { return t != PIECE_EMPTY; });
		if (fa != fb) return fa > fb;                   // whole structures before small bits
		return a.uses > b.uses;
	});
	return out;
}

const char *prefab_kind_name(PieceKind k)
{
	switch (k)
	{
	case PieceKind::Ramp: return "Ramp or slope";
	case PieceKind::Ledge: return "Ledge, platform or block";
	case PieceKind::Pit: return "Pit, pool or water";
	case PieceKind::Wall: return "Wall or obstacle";
	case PieceKind::Marking: return "Marking or decoration";
	case PieceKind::Floor: return "Floor";
	default: return "?";
	}
}

std::vector<Piece> extract_prefabs(const RomSet &roms, const std::vector<const LevelMap *> &maps)
{
	constexpr int MAX_SIDE = 48;                    // 3 blocks
	constexpr size_t MIN_CELLS = 6;
	auto fits = [&](const Region &r) { return r.x1 - r.x0 < MAX_SIDE && r.y1 - r.y0 < MAX_SIDE; };
	std::vector<Piece> found;
	for (size_t mi = 0; mi < maps.size(); mi++)
	{
		const Grid g = build_grid(roms, *maps[mi]);
		std::vector<int> content;
		for (int i = 0; i < g.w * g.h; i++)
			if (g.kind[i] != PieceKind::Floor) content.push_back(i);
		std::vector<int> mark(g.word.size(), -1);
		for (int c : content) mark[c] = 0;
		const auto structures = regions(g, content, mark, 0, [](int, int) { return true; });
		int stamp = 2;
		for (const Region &r : structures)
		{
			if (r.cells.size() < MIN_CELLS) continue;
			if (fits(r))
			{
				found.push_back(make_piece(g, r.cells, r.x0, r.y0, r.x1, r.y1, (int)mi));
				continue;
			}
			// take off the long networks: parts of one kind and colour that do not fit
			for (int c : r.cells) mark[c] = stamp;
			const auto parts = regions(g, r.cells, mark, stamp, [&](int a, int b) {
				return g.kind[a] == g.kind[b] && ((g.word[a] >> 11) & 7) == ((g.word[b] >> 11) & 7);
			});
			stamp += 2;
			std::vector<int> rest;
			for (const Region &p : parts)
			{
				if (fits(p))
				{
					rest.insert(rest.end(), p.cells.begin(), p.cells.end());
					continue;
				}
				// a long structure: sections of 2 x 2 blocks on the block grid
				for (int y = p.y0 / 16 * 16; y <= p.y1; y += 32)
					for (int x = p.x0 / 16 * 16; x <= p.x1; x += 32)
					{
						std::vector<int> part;
						int x0 = g.w, y0 = g.h, x1 = -1, y1 = -1;
						for (int c : p.cells)
						{
							const int cx = c % g.w, cy = c / g.w;
							if (cx < x || cx >= x + 32 || cy < y || cy >= y + 32) continue;
							part.push_back(c);
							x0 = std::min(x0, cx); y0 = std::min(y0, cy);
							x1 = std::max(x1, cx); y1 = std::max(y1, cy);
						}
						if (part.size() < 3 * MIN_CELLS) continue;
						found.push_back(make_piece(g, part, x0, y0, x1, y1, (int)mi));
						found.back().section = true;
					}
			}
			for (int c : rest) mark[c] = stamp;
			const auto objects = regions(g, rest, mark, stamp, [](int, int) { return true; });
			stamp += 2;
			for (const Region &o : objects)
				if (o.cells.size() >= MIN_CELLS && fits(o))
					found.push_back(make_piece(g, o.cells, o.x0, o.y0, o.x1, o.y1, (int)mi));
		}
	}
	std::map<std::tuple<int, int, std::vector<uint16_t>>, size_t> seen;
	std::vector<Piece> out;
	for (Piece &p : found)
	{
		auto key = std::make_tuple(p.w, p.h, p.tiles);
		auto it = seen.find(key);
		if (it != seen.end()) { out[it->second].uses++; continue; }
		p.uses = 1;
		p.whole = true;
		seen.emplace(std::move(key), out.size());
		out.push_back(std::move(p));
	}
	std::stable_sort(out.begin(), out.end(), [](const Piece &a, const Piece &b) {
		if (a.section != b.section) return b.section;
		if (a.kind != b.kind) return a.kind < b.kind;
		if (a.uses != b.uses) return a.uses > b.uses;
		return a.w * a.h > b.w * b.h;
	});
	return out;
}

void render_piece(const GfxSet &gfx, const LevelMap &lm, const int8_t random_var[32], const Piece &p,
                  std::vector<uint32_t> &px)
{
	const int W = p.w * 8;
	px.assign((size_t)W * p.h * 8, 0);
	for (int ty = 0; ty < p.h; ty++)
		for (int tx = 0; tx < p.w; tx++)
		{
			const size_t i = (size_t)ty * p.w + tx;
			uint16_t w = p.tiles[i];
			if (w == PIECE_EMPTY) continue;
			const int low = w & 0x7ff;
			if (low < 16 && lm.subst[low])
				w = (uint16_t)((lm.subst[low] | (w & 0xf800)) + random_var[((ty * 16 + tx) * 13 + 5) & 31]);
			const int code = tile_code(lm, w, p.bank1[i]);
			const int color = (w >> 11) & 7;
			const uint8_t *src = &gfx.tiles[(size_t)code * 64];
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 8; x++)
					px[(size_t)(ty * 8 + y) * W + tx * 8 + x] = level_palette_rgb(lm.palette[(128 + color * 16 + src[y * 8 + x]) & 255]);
		}
}
