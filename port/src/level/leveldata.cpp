// 720 Degrees level data: decoder, run-length encoder and mod patch builder.
#include "leveldata.h"
#include "board.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <algorithm>

namespace {
struct LevelRenderInfo { int level; int tile_bank[2]; uint16_t palette[256]; };
// Palettes are captured from the running game (they are loaded by game code, not stored
// per level).  level_palettes.inc is produced locally by tools/capture_palettes.py; without
// it the editor captures them at startup (see LevelSet::set_render_info).
#if __has_include("level_palettes.inc")
#include "level_palettes.inc"
#else
static const LevelRenderInfo LEVEL_RENDER_INFO[] = { { 0, { 0, 4 }, {} } };
#endif
}

namespace {

int bitswap_entry(uint16_t data)
{
	int n = ((data >> 10) & 077) ^ 3;
	auto b = [&](int i) { return (n >> i) & 1; };
	int e = (b(5) << 5) | (b(4) << 4) | (b(1) << 3) | (b(0) << 2) | (b(3) << 1) | b(2);
	return e + 64 * (data & 0x3ff);      // native extension: low bits select an extension group
}

const char *LEVEL_NAMES[8] = {
	"Skate City", "Park 1", "Park 2", "Park 3", "Park 4", "Screen 5", "Screen 6", "Screen 7" };

// fixed-ROM tables (see tools/levels.py)
constexpr uint32_t MAP_BANK_TAB = 0xFEB2, BLOCK_BANK_TAB = 0xFEA2, SUBST_TAB = 0xFF54;
constexpr uint16_t TOWN_BASE = 0x0180;
constexpr int TOWN_ROWS = 24;

}

int LevelSet::rom_bank_offset(uint16_t bankreg)
{
	return 0x10000 + bitswap_entry(bankreg) * 0x2000;
}

uint16_t LevelSet::r16(uint32_t off) const
{
	const auto &m = m_roms->main;
	if (off + 1 >= m.size()) return 0;
	return (uint16_t)(m[off] | (m[off + 1] << 8));
}

bool LevelBlock::operator==(const LevelBlock &o) const
{
	for (int h = 0; h < 2; h++)
		if (half[h].attr != o.half[h].attr || half[h].tiles != o.half[h].tiles) return false;
	return objects == o.objects;
}

uint32_t level_palette_rgb(uint16_t raw)
{
	static const int ZB = 115, Z3 = 78, Z2 = 37, Z1 = 17, Z0 = 9;
	static const int it[16] = { 0, ZB + Z0, ZB + Z1, ZB + Z1 + Z0, ZB + Z2, ZB + Z2 + Z0, ZB + Z2 + Z1, ZB + Z2 + Z1 + Z0,
		ZB + Z3, ZB + Z3 + Z0, ZB + Z3 + Z1, ZB + Z3 + Z1 + Z0, ZB + Z3 + Z2, ZB + Z3 + Z2 + Z0, ZB + Z3 + Z2 + Z1, ZB + Z3 + Z2 + Z1 + Z0 };
	static const int ct[16] = { 0x0, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xe, 0xf, 0xf };
	const int i = it[raw & 15];
	return 0xff000000u | (((ct[(raw >> 12) & 15] * i) >> 4) << 16) | (((ct[(raw >> 8) & 15] * i) >> 4) << 8) | ((ct[(raw >> 4) & 15] * i) >> 4);
}

// ------------------------------------------------------------------ decoding

namespace {
struct Reader {
	const std::vector<uint8_t> &m;
	uint16_t bank;                     // bank register value for window A (window B = +0x400)
	uint32_t addr;                     // CPU address 0x4000..0x7FFF
	uint8_t at(int32_t delta) const
	{
		uint32_t a = addr + delta;
		uint16_t b = bank;
		while (a >= 0x6000) { a -= 0x2000; b += 0x400; }
		while (a < 0x4000) { a += 0x2000; b -= 0x400; }
		const size_t off = LevelSet::rom_bank_offset(b) + (a - 0x4000);
		return off < m.size() ? m[off] : 0;
	}
};
}

bool LevelSet::decode_block(LevelBlock &blk, int level, uint16_t p, bool alt, std::string &err) const
{
	const auto &m = m_roms->main;
	uint16_t base = r16(BLOCK_BANK_TAB + 2 * level);
	if (alt) base = 0x6000;
	else if (level >= 1 && level <= 4 && (p & 1)) base = 0xA000;
	Reader rd{m, (uint16_t)(((p >> 3) & 0x1C00) + base), (uint32_t)(0x4000 + (p & 0x1fff))};
	const uint8_t b0 = rd.at(0), b1 = rd.at(1), b2 = rd.at(2);
	blk.half[0].attr = b0 & 0x7f;
	blk.half[1].attr = b1;
	blk.orig_ptr = p;
	blk.orig_alt = alt;
	blk.objects.clear();
	if (b0 & 0x80)
	{
		int d = -1;
		blk.objects.push_back(rd.at(d));
		while (!(rd.at(--d) & 0x80) && blk.objects.size() < 64) blk.objects.push_back(rd.at(d));
	}
	for (int h = 0; h < 2; h++)
	{
		int pos = 3 + (h ? b2 : 0);
		const int start = pos;
		int count = 0, mode = 0;
		uint16_t cur = 0, last = 0;
		for (int i = 0; i < 128; i++)
		{
			bool fresh = false;
			if (count == 0)
			{
				const uint8_t c = rd.at(pos++);
				count = c & 0x3f;
				mode = (c >> 6) & 3;
				cur = (uint16_t)((cur & 0xff00) | rd.at(pos++));
				if (mode == 2) cur = (uint16_t)((cur & 0x00ff) | (rd.at(pos++) << 8));
				fresh = true;
			}
			else if (mode == 0) cur = (uint16_t)((cur & 0xff00) | rd.at(pos++));
			else if (mode == 2) { cur = (uint16_t)(rd.at(pos) | (rd.at(pos + 1) << 8)); pos += 2; }
			else if (mode == 3) cur++;
			const uint16_t out = (mode == 1 && !fresh) ? last : cur;
			blk.half[h].tiles[i] = out;
			last = out;
			count = (count - 1) & 0xffff;
		}
		if (h == 0 && pos - start != b2)
		{
			err = "top half size mismatch";
			return false;
		}
		if (pos - start > 600) { err = "half too large"; return false; }
		if (h == 1)
		{
			// keep the original encoding so unedited blocks behave exactly as in the arcade
			blk.orig_body.resize(pos);
			for (int i = 0; i < pos; i++) blk.orig_body[i] = rd.at(i);
		}
	}
	return true;
}

bool LevelSet::load(const RomSet &roms, std::string &err)
{
	m_roms = &roms;
	maps.clear();
	auto b3 = [](uint32_t a) { return 0x10000 + 3 * 0x2000 + (a - 0x6000); };
	const auto &m = roms.main;
	struct Def { int level, cls; };
	std::vector<Def> defs = { {0, 0} };
	for (int lv = 1; lv <= 4; lv++)
		for (int c = 0; c < 4; c++) defs.push_back({lv, c});
	defs.push_back({5, 0}); defs.push_back({6, 0}); defs.push_back({7, 0});

	for (const Def &d : defs)
	{
		LevelMap lm;
		lm.level = d.level;
		lm.cls = d.cls;
		char nm[64];
		if (d.level >= 1 && d.level <= 4) snprintf(nm, sizeof(nm), "%s, class %d", LEVEL_NAMES[d.level], d.cls + 1);
		else snprintf(nm, sizeof(nm), "%s", LEVEL_NAMES[d.level]);
		lm.name = nm;
		if (d.level >= 1 && d.level <= 4)
		{
			const uint16_t sub = r16(b3(0x7F8C) + 2 * d.level);
			// two duplicate maps exist for the same park; skip identical class entries
			lm.base = r16(b3(sub) + 2 * d.cls);
			lm.stride = m[b3(sub) + 12 + d.cls];
		}
		else
		{
			lm.base = r16(b3(0x7F8C) + 2 * d.level);
			lm.stride = m[b3(0x7F9C) + d.level];
		}
		lm.cols = lm.stride / 2;
		const uint16_t mapbank = r16(MAP_BANK_TAB + 2 * d.level);
		const int maxrows = d.level == 0 ? TOWN_ROWS : 64;
		std::map<std::pair<uint16_t, bool>, int> index;
		std::vector<int> cells;
		std::vector<uint8_t> alts;
		int rows = 0;
		for (int cy = 0; cy < maxrows; cy++)
		{
			std::vector<int> row;
			std::vector<uint8_t> ralt;
			bool ok = true;
			for (int cx = 0; cx < lm.cols && ok; cx++)
			{
				const uint16_t mptr = (uint16_t)(lm.base + cy * lm.stride + cx * 2);
				Reader rd{m, (uint16_t)(((mptr >> 3) & 0x1C00) + mapbank), (uint32_t)(0x4000 + (mptr & 0x1fff))};
				const uint16_t p = (uint16_t)(rd.at(0) | (rd.at(1) << 8));
				bool alt = false;
				if (d.level == 0)
				{
					const int cell = (mptr - TOWN_BASE) >> 1;
					Reader bm{m, 0x2C00, (uint32_t)(0x5F16 + ((mptr - TOWN_BASE) >> 4))};   // 0x7F16 in window B (bank 0x2C00)
					alt = (bm.at(0) & (1 << (cell & 7))) != 0;
				}
				auto key = std::make_pair(p, alt);
				auto it = index.find(key);
				if (it == index.end())
				{
					LevelBlock blk;
					std::string e2;
					if (!decode_block(blk, d.level, p, alt, e2))
					{
						if (getenv("D720_LEVEL_DEBUG"))
							fprintf(stderr, "level %d class %d cell %d,%d P=%04X alt=%d: %s\n", d.level, d.cls, cx, cy, p, (int)alt, e2.c_str());
						ok = false;
						break;
					}
					index[key] = (int)lm.blocks.size();
					row.push_back((int)lm.blocks.size());
					lm.blocks.push_back(std::move(blk));
				}
				else
					row.push_back(it->second);
				ralt.push_back(alt);
			}
			if (!ok || (int)row.size() != lm.cols) break;
			cells.insert(cells.end(), row.begin(), row.end());
			alts.insert(alts.end(), ralt.begin(), ralt.end());
			rows++;
		}
		lm.rows = rows;
		lm.cells = std::move(cells);
		lm.alt = std::move(alts);
		if (lm.rows == 0) continue;
		const LevelRenderInfo *ri = &LEVEL_RENDER_INFO[0];
		for (const auto &r : LEVEL_RENDER_INFO) if (r.level == d.level) ri = &r;
		lm.tile_bank[0] = ri->tile_bank[0];
		lm.tile_bank[1] = ri->tile_bank[1];
		memcpy(lm.palette.data(), ri->palette, sizeof(ri->palette));
		bool any = false;
		for (uint16_t v : lm.palette) any |= v != 0;
		if (!any)
			for (int i = 0; i < 256; i++)   // neutral grey ramp until real colours are captured
				lm.palette[i] = (uint16_t)(((i & 15) * 0x1110) | 0x000f);
		bool dup = false;
		for (auto &o : maps)
			if (o.level == lm.level && o.base == lm.base && o.stride == lm.stride) { o.shared_classes.push_back(lm.cls); dup = true; }
		if (dup) continue;
		maps.push_back(std::move(lm));
	}
	if (maps.empty()) { err = "no levels decoded"; return false; }
	return true;
}

// ------------------------------------------------------------------ encoding

std::vector<uint8_t> LevelSet::encode_half(const BlockHalf &h)
{
	// optimal run-length coding by dynamic programming over (position, current high byte).
	// The decoder keeps a 16-bit "current" word; runs of modes 0/1/3 reuse its high byte.
	const auto &t = h.tiles;
	constexpr int INF = 1 << 28;
	struct Choice { int len = 0, mode = 0, prev_hi = 0; };
	// cost[pos][hi]: min bytes to encode tiles[0..pos) ending with current high byte 'hi'
	std::vector<std::array<int, 256>> cost(129);
	std::vector<std::array<Choice, 256>> ch(129);
	for (auto &c : cost) c.fill(INF);
	cost[0][0] = 0;
	for (int pos = 0; pos < 128; pos++)
		for (int hi = 0; hi < 256; hi++)
		{
			const int base = cost[pos][hi];
			if (base >= INF) continue;
			const int maxlen = std::min(63, 128 - pos);
			// mode 2: explicit words
			for (int n = 1; n <= maxlen; n++)
			{
				const int c = base + 1 + 2 * n;
				const int nhi = t[pos + n - 1] >> 8;
				if (c < cost[pos + n][nhi]) { cost[pos + n][nhi] = c; ch[pos + n][nhi] = {n, 2, hi}; }
			}
			if ((t[pos] >> 8) != hi) continue;
			// mode 0: same high byte, new low byte per tile
			for (int n = 1; n <= maxlen; n++)
			{
				if ((t[pos + n - 1] >> 8) != hi) break;
				const int c = base + 1 + n;
				if (c < cost[pos + n][hi]) { cost[pos + n][hi] = c; ch[pos + n][hi] = {n, 0, hi}; }
			}
			// mode 1: repeat
			for (int n = 1; n <= maxlen; n++)
			{
				if (t[pos + n - 1] != t[pos]) break;
				const int c = base + 2;
				if (c < cost[pos + n][hi]) { cost[pos + n][hi] = c; ch[pos + n][hi] = {n, 1, hi}; }
			}
			// mode 3: incrementing words
			for (int n = 2; n <= maxlen; n++)
			{
				if (t[pos + n - 1] != (uint16_t)(t[pos] + n - 1)) break;
				const int nhi = t[pos + n - 1] >> 8;
				const int c = base + 2;
				if (c < cost[pos + n][nhi]) { cost[pos + n][nhi] = c; ch[pos + n][nhi] = {n, 3, hi}; }
			}
		}
	int besthi = 0;
	for (int hi = 1; hi < 256; hi++) if (cost[128][hi] < cost[128][besthi]) besthi = hi;
	// backtrack
	std::vector<Choice> runs;
	int pos = 128, hi = besthi;
	while (pos > 0)
	{
		const Choice c = ch[pos][hi];
		runs.push_back(c);
		pos -= c.len;
		hi = c.prev_hi;
	}
	std::reverse(runs.begin(), runs.end());
	std::vector<uint8_t> out;
	pos = 0;
	for (const Choice &c : runs)
	{
		out.push_back((uint8_t)((c.mode << 6) | (c.len & 0x3f)));
		out.push_back((uint8_t)(t[pos] & 0xff));
		if (c.mode == 2)
		{
			out.push_back((uint8_t)(t[pos] >> 8));
			for (int i = 1; i < c.len; i++) { out.push_back((uint8_t)t[pos + i]); out.push_back((uint8_t)(t[pos + i] >> 8)); }
		}
		else if (c.mode == 0)
			for (int i = 1; i < c.len; i++) out.push_back((uint8_t)t[pos + i]);
		pos += c.len;
	}
	return out;
}

// ------------------------------------------------------------------ mod patches

bool ModPatch::save(const std::string &path) const
{
	std::ofstream f(path, std::ios::binary);
	if (!f) return false;
	auto w32 = [&](uint32_t v) { f.write((const char *)&v, 4); };
	f.write("720MOD01", 8);
	w32(required_size);
	w32(town_alt_base | ((uint32_t)park_odd_base << 16));
	w32((uint32_t)chunks.size());
	for (const auto &c : chunks)
	{
		w32(c.offset);
		w32((uint32_t)c.data.size());
		f.write((const char *)c.data.data(), c.data.size());
	}
	return (bool)f;
}

bool ModPatch::load(const std::string &path, std::string &err)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) { err = "cannot open " + path; return false; }
	char magic[8];
	f.read(magic, 8);
	if (memcmp(magic, "720MOD01", 8)) { err = "not a 720 Degrees mod file"; return false; }
	auto r32 = [&]() { uint32_t v = 0; f.read((char *)&v, 4); return v; };
	required_size = r32();
	{
		const uint32_t v = r32();
		town_alt_base = (uint16_t)v;
		park_odd_base = (uint16_t)(v >> 16);
		if (!park_odd_base) park_odd_base = 0xA000;
	}
	const uint32_t n = r32();
	chunks.clear();
	for (uint32_t i = 0; i < n && f; i++)
	{
		Chunk c;
		c.offset = r32();
		c.data.resize(r32());
		f.read((char *)c.data.data(), c.data.size());
		chunks.push_back(std::move(c));
	}
	if (!f) { err = "truncated mod file"; return false; }
	return true;
}

void ModPatch::apply(RomSet &roms) const
{
	if (roms.main.size() < required_size) roms.main.resize(required_size, 0);
	for (const auto &c : chunks)
	{
		if (c.offset + c.data.size() > roms.main.size()) roms.main.resize(c.offset + c.data.size(), 0);
		memcpy(&roms.main[c.offset], c.data.data(), c.data.size());
	}
}

namespace {
// linear writer over the 8+1 relative pages of one bank base
struct PageSpace {
	uint16_t base;                       // bank register base (low bits = extension group)
	std::vector<uint8_t> bytes;          // linear image, page k at k*0x2000
	size_t used = 0;
	bool place(const std::vector<uint8_t> &pre, const std::vector<uint8_t> &body, uint16_t &ptr, bool even)
	{
		size_t start = used + pre.size();
		if (even && (start & 1)) { start++; }
		const size_t end = start + body.size();
		if (start >= 8 * 0x2000 || end > 9 * 0x2000) return false;
		if (bytes.size() < end) bytes.resize(end, 0xff);
		memcpy(&bytes[start - pre.size()], pre.data(), pre.size());
		memcpy(&bytes[start], body.data(), body.size());
		used = end;
		const int page = (int)(start >> 13);
		ptr = (uint16_t)((page << 13) | (start & 0x1fff));
		return true;
	}
};
}

ModPatch LevelSet::build_patch(const RomSet &orig, std::string &report) const
{
	ModPatch patch;
	report.clear();
	int next_group = 1;
	bool park_secondary_used = false;
	auto add_chunk = [&](uint32_t off, std::vector<uint8_t> data) {
		patch.required_size = std::max<uint32_t>(patch.required_size, off + (uint32_t)data.size());
		patch.chunks.push_back({off, std::move(data)});
	};
	(void)orig;

	for (int level = 0; level < 8; level++)
	{
		std::vector<const LevelMap *> lmaps;
		bool dirty = false;
		for (const LevelMap &lm : maps)
			if (lm.level == level) { lmaps.push_back(&lm); dirty |= lm.dirty; }
		if (!dirty || lmaps.empty()) continue;

		// Relocate every block of every map of this level into extension pages.
		// Two spaces: primary (the level block bank table entry) and secondary
		// (town: alternate-cell bank; parks: odd-pointer bank), each up to 64 KB.
		const bool town = level == 0;
		const bool park = level >= 1 && level <= 4;
		const bool can_secondary = town || (park && !park_secondary_used);
		PageSpace prim{(uint16_t)next_group++};
		PageSpace sec{(uint16_t)(can_secondary ? next_group++ : 0)};
		bool sec_used = false;

		struct Placed { uint16_t ptr; bool secondary; };
		std::vector<std::vector<Placed>> placed(lmaps.size());
		std::map<std::vector<uint8_t>, Placed> dedupe;      // identical encoded blocks share storage
		for (size_t mi = 0; mi < lmaps.size(); mi++)
		{
			const LevelMap &lm = *lmaps[mi];
			placed[mi].resize(lm.blocks.size());
			for (size_t bi = 0; bi < lm.blocks.size(); bi++)
			{
				const LevelBlock &b = lm.blocks[bi];
				std::vector<uint8_t> body;
				if (!b.modified && !b.orig_body.empty())
					body = b.orig_body;
				else
				{
					std::vector<uint8_t> top = encode_half(b.half[0]);
					std::vector<uint8_t> bot = encode_half(b.half[1]);
					body.push_back((uint8_t)((b.half[0].attr & 0x7f) | (b.objects.empty() ? 0 : 0x80)));
					body.push_back(b.half[1].attr);
					body.push_back((uint8_t)top.size());
					body.insert(body.end(), top.begin(), top.end());
					body.insert(body.end(), bot.begin(), bot.end());
				}
				std::vector<uint8_t> pre;
				if (!b.objects.empty())
				{
					pre.push_back(0xff);
					for (auto it = b.objects.rbegin(); it != b.objects.rend(); ++it) pre.push_back(*it);
				}
				std::vector<uint8_t> key = pre;
				key.insert(key.end(), body.begin(), body.end());
				bool want_sec = false;
				if (town)
					for (size_t c = 0; c < lm.cells.size(); c++)
						if (lm.cells[c] == (int)bi && lm.alt[c]) { want_sec = true; break; }
				key.push_back(want_sec ? 1 : 0);
				auto it = dedupe.find(key);
				if (it != dedupe.end()) { placed[mi][bi] = it->second; continue; }
				Placed pl{0, false};
				// parks: primary block pointers must be even, secondary pointers odd
				auto try_space = [&](PageSpace &ps, bool secondary) {
					if (secondary && !can_secondary) return false;
					PageSpace backup = ps;
					if (park)
					{
						const size_t start = ps.used + pre.size();
						if (((start & 1) != 0) != secondary) ps.used++;
					}
					uint16_t ptr;
					if (!ps.place(pre, body, ptr, false)) { ps = backup; return false; }
					pl = {ptr, secondary};
					return true;
				};
				const bool ok = want_sec ? (try_space(sec, true) || try_space(prim, false))
				                         : (try_space(prim, false) || try_space(sec, true));
				if (!ok)
				{
					report += "ERROR: level " + std::to_string(level) + ": block data does not fit (" +
						std::to_string(prim.used + sec.used) + " bytes)\n";
					patch.chunks.clear();
					return patch;
				}
				sec_used |= pl.secondary;
				placed[mi][bi] = pl;
				dedupe[key] = pl;
			}
		}
		auto emit_space = [&](const PageSpace &ps) {
			for (size_t page = 0; page * 0x2000 < ps.bytes.size(); page++)
			{
				const uint16_t bankreg = (uint16_t)(ps.base + page * 0x400);
				const uint32_t off = (uint32_t)rom_bank_offset(bankreg);
				const size_t from = page * 0x2000;
				const size_t len = std::min<size_t>(0x2000, ps.bytes.size() - from);
				add_chunk(off, std::vector<uint8_t>(ps.bytes.begin() + from, ps.bytes.begin() + from + len));
			}
		};
		emit_space(prim);
		if (sec_used) emit_space(sec);

		const uint16_t mapbank = r16(MAP_BANK_TAB + 2 * level);
		std::vector<uint8_t> bitmap(6 * TOWN_ROWS, 0);
		for (size_t mi = 0; mi < lmaps.size(); mi++)
		{
			const LevelMap &lm = *lmaps[mi];
			for (int cy = 0; cy < lm.rows; cy++)
				for (int cx = 0; cx < lm.cols; cx++)
				{
					const uint16_t mptr = lm.cell_ptr(cx, cy);
					const Placed &pl = placed[mi][lm.cell(cx, cy)];
					const uint16_t b = (uint16_t)(((mptr >> 3) & 0x1C00) + mapbank);
					for (int k = 0; k < 2; k++)
					{
						const uint32_t lin = (uint32_t)(mptr & 0x1fff) + k;
						const uint16_t bb = (uint16_t)(lin >= 0x2000 ? b + 0x400 : b);
						add_chunk((uint32_t)rom_bank_offset(bb) + (lin & 0x1fff), { (uint8_t)(k ? pl.ptr >> 8 : pl.ptr) });
					}
					if (town && pl.secondary)
					{
						const int cell = (mptr - TOWN_BASE) >> 1;
						bitmap[cell >> 3] |= (uint8_t)(1 << (cell & 7));
					}
				}
		}
		add_chunk(BLOCK_BANK_TAB + 2 * level, { (uint8_t)prim.base, (uint8_t)(prim.base >> 8) });
		if (town)
		{
			patch.town_alt_base = sec.base;
			add_chunk((uint32_t)rom_bank_offset(0x2C00) + (0x7F16 - 0x6000), bitmap);
		}
		if (park && sec_used)
		{
			patch.park_odd_base = sec.base;
			park_secondary_used = true;
		}
		char line[200];
		snprintf(line, sizeof(line), "level %d: %zu map(s) relocated, %zu + %zu bytes of block data\n",
			level, lmaps.size(), prim.used, sec_used ? sec.used : (size_t)0);
		report += line;
	}
	return patch;
}

void LevelSet::set_render_info(int level, const int tile_bank[2], const uint16_t palette[256])
{
	for (LevelMap &lm : maps)
		if (lm.level == level)
		{
			lm.tile_bank[0] = tile_bank[0];
			lm.tile_bank[1] = tile_bank[1];
			memcpy(lm.palette.data(), palette, 256 * sizeof(uint16_t));
		}
}
