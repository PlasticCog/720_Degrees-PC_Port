// leveltool: inspect the 720 Degrees level data and test the encoder / mod pipeline.
//   leveltool --roms DIR stats
//   leveltool --roms DIR png OUTDIR            render every map to PNG (town palette)
//   leveltool --roms DIR identity OUT.720mod   relocate all levels unchanged (pipeline test)
#include "leveldata.h"
#include "board.h"
#include <cstdio>
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
								const int code = ((lm.tile_bank[(w >> 10) & 1] << 10) | (w & 0x3ff)) & 8191;
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
			snprintf(name, sizeof(name), "%s/map_%02zu_L%d_C%d.png", arg.c_str(), mi, lm.level, lm.cls);
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
	fprintf(stderr, "unknown command\n");
	return 1;
}
