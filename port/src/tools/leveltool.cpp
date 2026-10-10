// leveltool: inspect the 720 Degrees level data and test the encoder / mod pipeline.
//   leveltool --roms DIR stats
//   leveltool --roms DIR png OUTDIR            render every map to PNG (town palette)
//   leveltool --roms DIR identity OUT.720mod   relocate all levels unchanged (pipeline test)
#include "leveldata.h"
#include "board.h"
#include <cstdio>
#include <map>
#include <cstring>
#include <set>
#include <string>
#include <vector>

// decode a half from encoded bytes (mirror of the game's decoder) for round-trip tests
static bool decode_rle(const std::vector<uint8_t> &d, std::array<uint16_t, 128> &out)
{
	size_t pos = 0;
	int count = 0, mode = 0;
	uint16_t cur = 0, last = 0;
	auto rd = [&]() { return pos < d.size() ? d[pos++] : 0; };
	for (int i = 0; i < 128; i++)
	{
		bool fresh = false;
		if (count == 0)
		{
			const uint8_t c = rd();
			count = c & 0x3f;
			mode = (c >> 6) & 3;
			cur = (uint16_t)((cur & 0xff00) | rd());
			if (mode == 2) cur = (uint16_t)((cur & 0xff) | (rd() << 8));
			fresh = true;
		}
		else if (mode == 0) cur = (uint16_t)((cur & 0xff00) | rd());
		else if (mode == 2) { uint16_t lo = rd(); cur = (uint16_t)(lo | (rd() << 8)); }
		else if (mode == 3) cur++;
		out[i] = (mode == 1 && !fresh) ? last : cur;
		last = out[i];
		count = (count - 1) & 0xffff;
	}
	return pos == d.size();
}

int main(int argc, char **argv)
{
	std::string roms = "Game", cmd, arg;
	for (int i = 1; i < argc; i++)
	{
		std::string a = argv[i];
		if (a == "--roms" && i + 1 < argc) roms = argv[++i];
		else if (cmd.empty()) cmd = a;
		else arg = a;
	}
	RomSet rs;
	std::string err;
	if (!rs.load(roms, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
	LevelSet ls;
	if (!ls.load(rs, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }

	if (cmd == "stats" || cmd.empty())
	{
		size_t total_bad = 0;
		for (auto &lm : ls.maps)
		{
			size_t bytes = 0, objs = 0;
			int bad = 0;
			for (auto &b : lm.blocks)
			{
				for (int h = 0; h < 2; h++)
				{
					auto enc = LevelSet::encode_half(b.half[h]);
					bytes += enc.size();
					std::array<uint16_t, 128> back{};
					if (!decode_rle(enc, back) || back != b.half[h].tiles) bad++;
				}
				bytes += 3 + (b.objects.empty() ? 0 : b.objects.size() + 1);
				objs += b.objects.size();
			}
			total_bad += bad;
			printf("%-22s level %d base %04X stride %3d  %2dx%-2d cells  %4zu blocks  %6zu bytes encoded  %zu objects  roundtrip-fail %d\n",
				lm.name.c_str(), lm.level, lm.base, lm.stride, lm.cols, lm.rows, lm.blocks.size(), bytes, objs, bad);
		}
		printf("round-trip failures: %zu\n", total_bad);
		return total_bad ? 2 : 0;
	}
	if (cmd == "png")
	{
		GfxSet gfx;
		gfx.decode(rs);
		for (size_t mi = 0; mi < ls.maps.size(); mi++)
		{
			auto &lm = ls.maps[mi];
			const int W = lm.cols * 128, H = lm.rows * 128;
			std::vector<uint32_t> img((size_t)W * H, 0xff000000);
			uint32_t pal[256];
			for (int i = 0; i < 256; i++) pal[i] = level_palette_rgb(lm.palette[i]);
			for (int cy = 0; cy < lm.rows; cy++)
				for (int cx = 0; cx < lm.cols; cx++)
				{
					const LevelBlock &b = lm.blocks[lm.cell(cx, cy)];
					for (int h = 0; h < 2; h++)
						for (int ty = 0; ty < 8; ty++)
							for (int tx = 0; tx < 16; tx++)
							{
								uint16_t w = b.half[h].tiles[ty * 16 + tx];
								const int code = ((((w >> 10) & 1 ? lm.half_bank1(b.half[h]) : lm.tile_bank[0]) << 10) | (w & 0x3ff)) & 8191;
								const int color = (w >> 11) & 7;
								for (int py = 0; py < 8; py++)
									for (int px = 0; px < 8; px++)
									{
										const int pen = gfx.tiles[code * 64 + py * 8 + px];
										const int x = cx * 128 + tx * 8 + px, y = cy * 128 + h * 64 + ty * 8 + py;
										img[(size_t)y * W + x] = pal[128 + color * 16 + pen];
									}
							}
				}
			char name[512];
			snprintf(name, sizeof(name), "%s/map_%02zu_L%d_layout%d.png", arg.c_str(), mi, lm.level, lm.layout + 1);
			write_png(name, img.data(), W, H);
			printf("wrote %s (%dx%d)\n", name, W, H);
		}
		return 0;
	}
	if (cmd == "identity")
	{
		for (auto &lm : ls.maps) lm.dirty = true;
		std::string report;
		ModPatch p = ls.build_patch(rs, report);
		printf("%s", report.c_str());
		if (!p.save(arg)) { fprintf(stderr, "cannot write %s\n", arg.c_str()); return 1; }
		printf("wrote %s: %zu chunks, ROM size 0x%X\n", arg.c_str(), p.chunks.size(), p.required_size);
		return 0;
	}
	if (cmd == "demo-mod")
	{
		// a visible test edit: Skate City's ground placeholders switch to the next palette,
		// and every town block is re-encoded (exercises the encoder in the game)
		for (auto &lm : ls.maps)
			if (lm.level == 0)
			{
				for (auto &b : lm.blocks)
				{
					for (auto &h : b.half)
						for (auto &t : h.tiles)
							if ((t & 0x7ff) < 16) t = (uint16_t)(t ^ 0x0800);
					b.modified = true;
				}
				lm.dirty = true;
			}
		std::string report;
		ModPatch p = ls.build_patch(rs, report);
		printf("%s", report.c_str());
		if (!p.save(arg)) { fprintf(stderr, "cannot write %s\n", arg.c_str()); return 1; }
		printf("wrote %s\n", arg.c_str());
		return 0;
	}
	if (cmd == "pngbanks")
	{
		// pngbanks "MAP B0 B1 FILE": render one map (index as in 'stats') with given tile banks
		int mi = 0, b0 = 0, b1 = 0;
		char file[512] = "";
		if (sscanf(arg.c_str(), "%d %d %d %511s", &mi, &b0, &b1, file) != 4 || mi < 0 || mi >= (int)ls.maps.size())
		{
			fprintf(stderr, "usage: leveltool pngbanks \"MAP B0 B1 FILE\"\n");
			return 1;
		}
		GfxSet gfx;
		gfx.decode(rs);
		const LevelMap &lm = ls.maps[mi];
		const int W = lm.cols * 128, H = lm.rows * 128;
		std::vector<uint32_t> img((size_t)W * H);
		for (int cy = 0; cy < lm.rows; cy++)
			for (int cx = 0; cx < lm.cols; cx++)
			{
				const LevelBlock &b = lm.blocks[lm.cell(cx, cy)];
				for (int h = 0; h < 2; h++)
					for (int ty = 0; ty < 8; ty++)
						for (int tx = 0; tx < 16; tx++)
						{
							const uint16_t w = b.half[h].tiles[ty * 16 + tx];
							const int code = (((((w >> 10) & 1) ? b1 : b0) << 10) | (w & 0x3ff)) & 8191;
							for (int py = 0; py < 8; py++)
								for (int px = 0; px < 8; px++)
									img[(size_t)(cy * 128 + h * 64 + ty * 8 + py) * W + cx * 128 + tx * 8 + px] =
										level_palette_rgb(lm.palette[128 + ((w >> 11) & 7) * 16 + gfx.tiles[code * 64 + py * 8 + px]]);
						}
			}
		return write_png(file, img.data(), W, H) ? 0 : 1;
	}
	if (cmd == "bankscan")
	{
		// For each map, try every tile bank for the tiles with the bank bit set (and, separately,
		// for the others) and count pixels that differ across the edges they share with their
		// neighbours.  The bank the game really uses gives seamless edges.
		GfxSet gfx;
		gfx.decode(rs);
		for (const auto &lm : ls.maps)
		{
			const int W = lm.cols * 16, H = lm.rows * 16;           // map size in tiles
			std::vector<uint16_t> tw((size_t)W * H);
			for (int cy = 0; cy < lm.rows; cy++)
				for (int cx = 0; cx < lm.cols; cx++)
				{
					const LevelBlock &b = lm.blocks[lm.cell(cx, cy)];
					for (int h = 0; h < 2; h++)
						for (int ty = 0; ty < 8; ty++)
							for (int tx = 0; tx < 16; tx++)
								tw[(size_t)(cy * 16 + h * 8 + ty) * W + cx * 16 + tx] = b.half[h].tiles[ty * 16 + tx];
				}
			auto score = [&](int which, int bank) {
				auto pix = [&](uint16_t w, int x, int y) {
					const int bk = ((w >> 10) & 1) == which ? bank : lm.tile_bank[(w >> 10) & 1];
					const int code = ((bk << 10) | (w & 0x3ff)) & 8191;
					return ((w >> 11) & 7) * 16 + gfx.tiles[code * 64 + y * 8 + x];
				};
				long bad = 0, edges = 0;
				for (int y = 0; y < H; y++)
					for (int x = 0; x < W; x++)
					{
						const uint16_t a = tw[(size_t)y * W + x];
						if ((a & 0x7ff) < 16) continue;                  // ground placeholders
						if (x + 1 < W)
						{
							const uint16_t b = tw[(size_t)y * W + x + 1];
							if ((b & 0x7ff) >= 16 && ((((a >> 10) & 1) == which) || (((b >> 10) & 1) == which)))
								for (int i = 0; i < 8; i++, edges++) bad += pix(a, 7, i) != pix(b, 0, i);
						}
						if (y + 1 < H)
						{
							const uint16_t b = tw[(size_t)(y + 1) * W + x];
							if ((b & 0x7ff) >= 16 && ((((a >> 10) & 1) == which) || (((b >> 10) & 1) == which)))
								for (int i = 0; i < 8; i++, edges++) bad += pix(a, i, 7) != pix(b, i, 0);
						}
					}
				return edges ? 100.0 * bad / edges : -1.0;
			};
			for (int which = 0; which < 2; which++)
			{
				int best = -1;
				double best_s = 1e9;
				std::string line;
				for (int bank = 0; bank < 8; bank++)
				{
					const double s = score(which, bank);
					if (s < 0) continue;
					char t[32];
					snprintf(t, sizeof(t), " %d:%.0f", bank, s);
					line += t;
					if (s < best_s) { best_s = s; best = bank; }
				}
				printf("%-22s bank-%d tiles: current %d, best %d   (%% edge pixels differing:%s)\n",
					lm.name.c_str(), which, lm.tile_bank[which], best, line.c_str());
			}
		}
		return 0;
	}
	if (cmd == "zones")
	{
		// zone byte per block half: bits 3+ = playfield tile bank 1, bits 0-2 = switch mode
		for (const auto &lm : ls.maps)
		{
			std::map<int, int> hist;
			printf("%s: bank of each half (top/bottom rows), '.' = 0\n", lm.name.c_str());
			for (int cy = 0; cy < lm.rows; cy++)
				for (int h = 0; h < 2; h++)
				{
					std::string row;
					for (int cx = 0; cx < lm.cols; cx++)
					{
						const int a = lm.blocks[lm.cell(cx, cy)].half[h].attr;
						hist[a]++;
						const int b = (a >> 3) & 15;
						row += b ? "0123456789ABCDEF"[b] : '.';
					}
					printf("  %s\n", row.c_str());
				}
			std::map<int, int> b1;
			for (const auto &b : lm.blocks)
				for (int h = 0; h < 2; h++)
				{
					bool any = false;
					for (uint16_t t : b.half[h].tiles) any |= ((t >> 10) & 1) && (t & 0x7ff) > 15;
					if (any) b1[(b.half[h].attr >> 3) & 15]++;
				}
			printf("  block halves using tile bank 1, by zone bank:");
			for (auto &kv : b1) printf(" %d:%d", kv.first, kv.second);
			printf("\n");
			printf("  zone bytes:");
			for (auto &kv : hist) printf(" %02X:%d", kv.first, kv.second);
			printf("\n");
		}
		return 0;
	}
	if (cmd == "roundtrip")
	{
		// build a patch with every map (or only the maps of level 'arg') marked edited, apply
		// it, decode the patched ROM and compare every cell with the original
		const int only = arg.empty() ? -1 : atoi(arg.c_str());
		for (auto &lm : ls.maps) lm.dirty = only < 0 || lm.level == only;
		std::string report;
		ModPatch p = ls.build_patch(rs, report);
		printf("%s", report.c_str());
		RomSet patched = rs;
		p.apply(patched);
		LevelSet ps;
		ps.town_alt_base = p.town_alt_base;
		ps.park_odd_base = p.park_odd_base;
		if (!ps.load(patched, err)) { fprintf(stderr, "patched: %s\n", err.c_str()); return 1; }
		int bad = 0;
		for (const auto &lm : ls.maps)
		{
			const LevelMap *pm = nullptr;
			for (const auto &m : ps.maps) if (m.level == lm.level && m.layout == lm.layout) pm = &m;
			if (!pm || pm->rows != lm.rows || pm->cols != lm.cols) { printf("%s: shape differs\n", lm.name.c_str()); bad++; continue; }
			int diff = 0;
			for (size_t c = 0; c < lm.cells.size(); c++)
				diff += !(lm.blocks[lm.cells[c]] == pm->blocks[pm->cells[c]]);
			if (diff) { printf("%s: %d cells differ\n", lm.name.c_str(), diff); bad++; }
		}
		printf("town alt base %04X, park odd base %04X, ROM size 0x%X: %s\n", p.town_alt_base, p.park_odd_base,
			p.required_size, bad ? "MISMATCH" : "all maps identical");
		return bad ? 2 : 0;
	}
	fprintf(stderr, "unknown command\n");
	return 1;
}
