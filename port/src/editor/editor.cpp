// editor720 - level editor for the 720 Degrees native port (SDL3 + Dear ImGui).
//
// Edits the playfield maps decoded by port/src/level (Skate City, the parks of each class
// and the attract screens) and saves .720mod files into the mods folder next to the game.
//
//   Map          left click: select a cell, or paint the brush block in Paint mode
//                right drag: pan   wheel: zoom   middle click: pick block   double click: edit block
//   Block editor left click/drag: paint the brush tile (or stamp the brush piece)
//                right click: pick up a tile
//   Library      Pieces: structures of the original maps (ramps, ledges, pits, walls,
//                markings) to stamp; Tiles: single tiles by kind; All tiles: the raw banks
//   Map, Stamp   left click: put the brush piece or tile there (shared blocks are copied)
//
//   editor720 --capture-test    runs the palette capture headless and prints what it found
//
// Platform notes: everything here goes through SDL3, ImGui and the C++17 standard library;
// the only platform-specific name is the game executable (GAME_EXE below).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "leveldata.h"
#include "board.h"
#include "nativemachine.h"
#include "inputscript.h"
#include "romzip.h"
#include "pieces.h"
#include "../app/window_icon.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

#ifdef _WIN32
constexpr const char *GAME_EXE = "720degrees.exe";
#else
constexpr const char *GAME_EXE = "720degrees";
#endif
constexpr uint32_t RANDOM_VAR_TAB = 0xFEE2;   // fixed ROM: signed ground-tile variation offsets
constexpr int LEVEL_ID_RAM = 0x0F59;          // main RAM: number of the level being shown
constexpr ImGuiWindowFlags PANEL = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
	ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

std::string pref_file(const char *name)
{
	char *p = SDL_GetPrefPath("Atari", "720Degrees");
	std::string s = p ? std::string(p) + name : std::string(name);
	SDL_free(p);
	return s;
}

bool has_roms(const std::string &dir)
{
	return rom_folder_usable(dir);              // the ROM files, or a zip holding them
}

// folder the program runs from: the game, the mods folder and exports live here
std::string app_dir()
{
	const char *base = SDL_GetBasePath();
	return base ? std::string(base) : std::string("./");
}

// same search order as the game: the Game folder next to the program, then the program's
// own folder, then Game folders above it (for running from the build directory)
std::string find_rom_dir(const std::string &requested)
{
	if (!requested.empty()) return requested.back() == '/' || requested.back() == '\\' ? requested : requested + "/";
	const std::string base = app_dir();
	std::vector<std::string> c;
	for (const char *rel : { "Game/", "", "../Game/", "../../Game/", "../../../Game/" })
		c.push_back(base + rel);
	c.push_back("Game/");
	for (auto &d : c)
		if (has_roms(d)) return d;
	return base + "Game/";
}

std::vector<std::string> list_mods(const std::string &dir)
{
	std::vector<std::string> out;
	std::error_code ec;
	if (!std::filesystem::is_directory(dir, ec)) return out;
	for (const auto &e : std::filesystem::directory_iterator(dir, ec))
		if (e.path().extension() == ".720mod") out.push_back(e.path().filename().string());
	std::sort(out.begin(), out.end());
	return out;
}

bool confirm(SDL_Window *w, const char *msg, const char *yes)
{
	const SDL_MessageBoxButtonData buttons[] = {
		{ SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel" },
		{ SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, yes },
	};
	const SDL_MessageBoxData data = { SDL_MESSAGEBOX_WARNING, w, "720 Degrees level editor", msg, 2, buttons, nullptr };
	int id = 0;
	return SDL_ShowMessageBox(&data, &id) && id == 1;
}

// ---------------------------------------------------------------- palette capture
// Palettes are set up by game code at runtime, so they are captured by running the native
// game headless with random input and recording palette RAM shortly after each level
// starts.  (Tile banks come from ROM tables; see LevelMap::half_bank1.)
struct CapturedInfo { int level; uint16_t palette[256]; };

struct PaletteCapture {
	static constexpr int MAX_FRAMES = 9000, SETTLE_FRAMES = 90;
	std::unique_ptr<NativeMachine> machine;
	InputScript input;
	int frame = 0, last_level = -1, countdown = -1;
	std::map<int, CapturedInfo> got;
	bool done = false;

	void start(const RomSet &roms)
	{
		got.clear();
		frame = 0;
		last_level = countdown = -1;
		done = false;
		input = InputScript();
		input.random_seed = 3;
		machine = std::make_unique<NativeMachine>(roms);
		machine->reset();
		machine->board.chips().audio_enabled = false;
	}
	bool complete() const { return got.count(0) && got.count(5) && got.count(6) && got.count(7); }
	void run_one()
	{
		std::string snap;
		input.apply(frame, machine->board.input, snap);
		machine->run_frame();
		frame++;
		Board &b = machine->board;
		const int level = b.main_ram()[LEVEL_ID_RAM];
		if (level != last_level) { last_level = level; countdown = SETTLE_FRAMES; }
		else if (countdown > 0 && --countdown == 0 && level < 8 && !got.count(level))
		{
			CapturedInfo ci{level, {}};
			memcpy(ci.palette, b.palette_ram(), sizeof(ci.palette));
			got[level] = ci;
		}
		if (frame >= MAX_FRAMES || complete()) { done = true; machine.reset(); }
	}
	// runs the game for about 'budget_ns'; returns progress 0..1
	float step(uint64_t budget_ns)
	{
		const uint64_t t0 = SDL_GetTicksNS();
		while (!done && SDL_GetTicksNS() - t0 < budget_ns) run_one();
		return done ? 1.0f : (float)frame / MAX_FRAMES;
	}
	bool save(const std::string &path) const
	{
		std::ofstream f(path, std::ios::binary);
		const uint32_t n = (uint32_t)got.size();
		f.write("720PAL02", 8);
		f.write((const char *)&n, 4);
		for (auto &kv : got) f.write((const char *)&kv.second, sizeof(CapturedInfo));
		return (bool)f;
	}
	bool load(const std::string &path)
	{
		std::ifstream f(path, std::ios::binary);
		char magic[8];
		uint32_t n = 0;
		if (!f.read(magic, 8) || memcmp(magic, "720PAL02", 8) || !f.read((char *)&n, 4) || n == 0 || n > 16) return false;
		std::map<int, CapturedInfo> g;
		for (uint32_t i = 0; i < n; i++)
		{
			CapturedInfo ci;
			if (!f.read((char *)&ci, sizeof(ci)) || ci.level < 0 || ci.level > 7) return false;
			g[ci.level] = ci;
		}
		got = std::move(g);
		done = true;
		return true;
	}
};

// ---------------------------------------------------------------- editor
struct Snapshot {
	int map;
	std::vector<int> cells;
	std::vector<uint8_t> alt;
	std::vector<LevelBlock> blocks;
	bool dirty;
};

struct Editor {
	SDL_Window *win = nullptr;
	SDL_Renderer *ren = nullptr;
	RomSet roms;                          // the original ROM set
	std::unique_ptr<RomSet> level_roms;   // original + the opened mod; 'levels' decodes from it
	GfxSet gfx;
	LevelSet original, levels;
	int8_t random_var[32] = {};
	std::string rom_dir, app, mod_file;     // ROM folder; program folder (mods, exports, game)
	PaletteCapture capture;
	bool capturing = false;

	int map_index = 0;
	float zoom = 0.25f;
	ImVec2 pan{0, 0};
	bool fit_pending = true;
	int tool = 0;                         // 0 select, 1 paint
	int sel_cx = -1, sel_cy = -1;
	int brush_block = -1;
	int edit_block = -1;
	uint16_t brush_tile = 0x0010;
	int pick_bank = 0, pick_pal = 0, pick_prio = 0;
	bool show_grid = true, show_attr = false, show_alt = false;
	bool painting = false, tile_painting = false;
	bool unsaved = false, open_save_popup = false;
	std::string status = "Ready";
	char mod_name[128] = "my_level";
	char object_text[256] = "";
	int object_text_for = -1;

	std::vector<SDL_Texture *> block_tex;
	std::vector<char> block_dirty;

	// library (pieces.h) of the current map's level, taken from the original maps
	struct LibTile { uint16_t word; uint8_t bank1; PieceKind kind; int uses; };
	int lib_level = -1;
	std::vector<Piece> pieces;
	std::vector<SDL_Texture *> piece_tex;
	std::vector<LibTile> lib_tiles;
	SDL_Texture *lib_tile_tex = nullptr;          // atlas of lib_tiles, 32 per row
	int brush_piece = -1;                         // index into pieces, -1: the brush is brush_tile
	bool lib_common_only = true;
	SDL_Texture *picker_tex = nullptr;
	int picker_key = -1;
	std::vector<Snapshot> undo_stack, redo_stack;

	LevelMap &map() { return levels.maps[map_index]; }

	const LevelMap *original_map(const LevelMap &lm) const
	{
		for (const LevelMap &m : original.maps)
			if (m.level == lm.level && m.layout == lm.layout) return &m;
		return nullptr;
	}

	// ------------------------------------------------------------ loading
	bool init(std::string &err)
	{
		if (!roms.load(rom_dir, err)) return false;
		if (!original.load(roms, err)) return false;
		gfx.decode(roms);
		for (int i = 0; i < 32; i++) random_var[i] = (int8_t)roms.main[RANDOM_VAR_TAB + i];
		return open_levels("", err);
	}

	// decode the levels from the original ROM set plus an optional mod
	bool open_levels(const std::string &mod_path, std::string &err)
	{
		auto r = std::make_unique<RomSet>(roms);
		LevelSet ls;
		if (!mod_path.empty())
		{
			ModPatch p;
			if (!p.load(mod_path, err)) return false;
			p.apply(*r);
			ls.town_alt_base = p.town_alt_base;
			ls.park_odd_base = p.park_odd_base;
		}
		if (!ls.load(*r, err)) return false;
		// a level counts as edited when it differs from the original ROM
		for (LevelMap &lm : ls.maps)
		{
			const LevelMap *o = original_map(lm);
			bool same = o && o->cols == lm.cols && o->rows == lm.rows;
			for (size_t c = 0; same && c < lm.cells.size(); c++)
				same = lm.blocks[lm.cells[c]] == o->blocks[o->cells[c]];
			lm.dirty = !same;
		}
		level_roms = std::move(r);
		levels = std::move(ls);
		mod_file = mod_path;
		if (!mod_path.empty())
			snprintf(mod_name, sizeof(mod_name), "%s", std::filesystem::path(mod_path).stem().string().c_str());
		map_index = 0;
		sel_cx = sel_cy = brush_block = edit_block = -1;
		object_text_for = -1;
		undo_stack.clear();
		redo_stack.clear();
		unsaved = false;
		fit_pending = true;
		apply_capture();
		return true;
	}

	void apply_capture()
	{
		const CapturedInfo *town = capture.got.count(0) ? &capture.got.at(0) : nullptr;
		for (int level = 0; level < 8; level++)
		{
			// parks are not reached by the random-input run; they use the town's colours
			const CapturedInfo *ci = capture.got.count(level) ? &capture.got.at(level) : town;
			if (ci) levels.set_palette(level, ci->palette);
		}
		invalidate_all();
	}

	// ------------------------------------------------------------ rendering
	uint32_t pal_rgb(const LevelMap &lm, int idx) const { return level_palette_rgb(lm.palette[idx & 255]); }

	// ground placeholders (codes 0..15) are replaced through the level's table plus a
	// pseudo-random variation, as the game does when it unpacks a block
	uint16_t resolve_tile(const LevelMap &lm, uint16_t w, int salt) const
	{
		const int code = w & 0x7ff;
		if (code <= 15 && lm.subst[code])
			return (uint16_t)((lm.subst[code] | (w & 0xf800)) + random_var[(salt * 13 + 5) & 31]);
		return w;
	}

	void draw_tile(const LevelMap &lm, uint32_t *dst, int pitch, uint16_t w, int salt, int bank1) const
	{
		w = resolve_tile(lm, w, salt);
		const int bank = (w >> 10) & 1 ? bank1 : lm.tile_bank[0];
		const int code = ((bank << 10) | (w & 0x3ff)) & 8191;
		const int color = (w >> 11) & 7;
		const uint8_t *src = &gfx.tiles[code * 64];
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++)
				dst[y * pitch + x] = pal_rgb(lm, 128 + color * 16 + src[y * 8 + x]);
	}

	void render_block_pixels(const LevelMap &lm, const LevelBlock &b, std::vector<uint32_t> &px) const
	{
		px.assign(128 * 128, 0xff000000);
		for (int h = 0; h < 2; h++)
		{
			const int bank1 = lm.half_bank1(b.half[h]);
			for (int ty = 0; ty < 8; ty++)
				for (int tx = 0; tx < 16; tx++)
					draw_tile(lm, &px[(h * 64 + ty * 8) * 128 + tx * 8], 128, b.half[h].tiles[ty * 16 + tx], h * 128 + ty * 16 + tx, bank1);
		}
	}

	// graphics bank shown for "bank 1" in the tile picker: the edited block's zone in
	// Skate City, otherwise the level's bank
	int picker_bank1()
	{
		const LevelMap &lm = map();
		if (edit_block >= 0 && edit_block < (int)lm.blocks.size())
			for (int h = 0; h < 2; h++)
				if (lm.half_bank1(lm.blocks[edit_block].half[h]) != lm.tile_bank[1]) return lm.half_bank1(lm.blocks[edit_block].half[h]);
		return lm.tile_bank[1];
	}

	SDL_Texture *block_texture(int bi)
	{
		LevelMap &lm = map();
		if (block_tex.size() != lm.blocks.size())
		{
			block_tex.resize(lm.blocks.size(), nullptr);
			block_dirty.resize(lm.blocks.size(), 1);
		}
		if (bi < 0 || bi >= (int)lm.blocks.size()) return nullptr;
		if (!block_tex[bi])
		{
			block_tex[bi] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 128, 128);
			SDL_SetTextureScaleMode(block_tex[bi], SDL_SCALEMODE_NEAREST);
			block_dirty[bi] = 1;
		}
		if (block_dirty[bi])
		{
			std::vector<uint32_t> px;
			render_block_pixels(lm, lm.blocks[bi], px);
			SDL_UpdateTexture(block_tex[bi], nullptr, px.data(), 128 * 4);
			block_dirty[bi] = 0;
		}
		return block_tex[bi];
	}

	void invalidate_all()
	{
		for (auto *t : block_tex) if (t) SDL_DestroyTexture(t);
		block_tex.clear();
		block_dirty.clear();
		picker_key = -1;
		drop_library_textures();
	}

	// ------------------------------------------------------------ library
	void drop_library_textures()
	{
		for (auto *t : piece_tex) if (t) SDL_DestroyTexture(t);
		piece_tex.assign(pieces.size(), nullptr);
		if (lib_tile_tex) SDL_DestroyTexture(lib_tile_tex);
		lib_tile_tex = nullptr;
	}

	void build_library()
	{
		const LevelMap &lm = map();
		if (lib_level == lm.level) return;
		lib_level = lm.level;
		std::vector<const LevelMap *> maps;
		for (const LevelMap &m : original.maps)
			if (m.level == lm.level) maps.push_back(&m);
		pieces = extract_pieces(roms, maps);
		brush_piece = -1;
		// single tiles: every tile word the level's maps use, with how often
		std::map<std::pair<uint16_t, uint8_t>, LibTile> tiles;
		for (const LevelMap *m : maps)
			for (int c : m->cells)
			{
				if (c < 0) continue;
				for (const BlockHalf &h : m->blocks[c].half)
				{
					const uint8_t b1 = (uint8_t)m->half_bank1(h);
					for (uint16_t w : h.tiles)
					{
						const uint16_t key = (uint16_t)(w & 0x3fff);
						auto it = tiles.find({key, b1});
						if (it == tiles.end())
						{
							const PieceKind k = (w & 0x7ff) < 16 ? PieceKind::Floor : terrain_kind(tile_terrain(roms, *m, w, b1));
							it = tiles.emplace(std::make_pair(key, b1), LibTile{key, b1, k, 0}).first;
						}
						it->second.uses++;
					}
				}
			}
		lib_tiles.clear();
		for (auto &kv : tiles) lib_tiles.push_back(kv.second);
		std::stable_sort(lib_tiles.begin(), lib_tiles.end(), [](const LibTile &a, const LibTile &b) {
			if (a.kind != b.kind) return a.kind < b.kind;
			return (a.word & 0x7ff) < (b.word & 0x7ff);
		});
		drop_library_textures();
	}

	SDL_Texture *piece_texture(int i)
	{
		if (i < 0 || i >= (int)pieces.size()) return nullptr;
		if (piece_tex.size() != pieces.size()) piece_tex.assign(pieces.size(), nullptr);
		if (!piece_tex[i])
		{
			const Piece &p = pieces[i];
			std::vector<uint32_t> px;
			render_piece(gfx, map(), random_var, p, px);
			piece_tex[i] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, p.w * 8, p.h * 8);
			SDL_SetTextureScaleMode(piece_tex[i], SDL_SCALEMODE_NEAREST);
			SDL_SetTextureBlendMode(piece_tex[i], SDL_BLENDMODE_BLEND);
			SDL_UpdateTexture(piece_tex[i], nullptr, px.data(), p.w * 8 * 4);
		}
		return piece_tex[i];
	}

	SDL_Texture *lib_tiles_texture()
	{
		if (lib_tile_tex || lib_tiles.empty()) return lib_tile_tex;
		const int rows = ((int)lib_tiles.size() + 31) / 32;
		std::vector<uint32_t> px((size_t)256 * rows * 8, 0);
		const LevelMap &lm = map();
		for (size_t i = 0; i < lib_tiles.size(); i++)
		{
			Piece one;
			one.w = one.h = 1;
			one.tiles = {lib_tiles[i].word};
			one.bank1 = {lib_tiles[i].bank1};
			std::vector<uint32_t> t;
			render_piece(gfx, lm, random_var, one, t);
			const int x0 = (int)(i % 32) * 8, y0 = (int)(i / 32) * 8;
			for (int y = 0; y < 8; y++)
				memcpy(&px[(size_t)(y0 + y) * 256 + x0], &t[(size_t)y * 8], 8 * 4);
		}
		lib_tile_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 256, rows * 8);
		SDL_SetTextureScaleMode(lib_tile_tex, SDL_SCALEMODE_NEAREST);
		SDL_UpdateTexture(lib_tile_tex, nullptr, px.data(), 256 * 4);
		return lib_tile_tex;
	}

	// the brush as a piece: the chosen piece or the single brush tile
	Piece brush_as_piece()
	{
		if (brush_piece >= 0 && brush_piece < (int)pieces.size()) return pieces[brush_piece];
		Piece one;
		one.w = one.h = 1;
		one.tiles = {brush_tile};
		one.bank1 = {(uint8_t)picker_bank1()};
		return one;
	}

	SDL_Texture *brush_texture()
	{
		if (brush_piece >= 0) return piece_texture(brush_piece);
		static SDL_Texture *one_tex = nullptr;
		static uint32_t one_key = 0xffffffff;
		const uint32_t key = brush_tile | (uint32_t)map_index << 16;
		if (!one_tex)
		{
			one_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 8, 8);
			SDL_SetTextureScaleMode(one_tex, SDL_SCALEMODE_NEAREST);
			SDL_SetTextureBlendMode(one_tex, SDL_BLENDMODE_BLEND);
		}
		if (key != one_key)
		{
			std::vector<uint32_t> px;
			render_piece(gfx, map(), random_var, brush_as_piece(), px);
			SDL_UpdateTexture(one_tex, nullptr, px.data(), 8 * 4);
			one_key = key;
		}
		return one_tex;
	}

	void mark_block_dirty(int bi)
	{
		if (bi >= 0 && bi < (int)block_dirty.size()) block_dirty[bi] = 1;
	}

	// stamp p into block bi with its top-left tile at (tx, ty); returns the tiles changed
	int stamp_in_block(int bi, const Piece &p, int tx, int ty)
	{
		LevelMap &lm = map();
		LevelBlock &b = lm.blocks[bi];
		int n = 0;
		for (int y = 0; y < p.h; y++)
			for (int x = 0; x < p.w; x++)
			{
				const uint16_t w = p.tiles[(size_t)y * p.w + x];
				const int bx = tx + x, by = ty + y;
				if (w == PIECE_EMPTY || bx < 0 || by < 0 || bx >= 16 || by >= 16) continue;
				uint16_t &t = b.half[by / 8].tiles[(by % 8) * 16 + bx];
				if (t != w) { t = w; n++; }
			}
		if (n)
		{
			b.modified = true;
			lm.dirty = true;
			mark_block_dirty(bi);
		}
		return n;
	}

	// stamp p into the map with its top-left tile at (gx, gy) in map tiles; cells whose block
	// other cells use too get their own copy first, so only this place changes
	int stamp_on_map(const Piece &p, int gx, int gy)
	{
		LevelMap &lm = map();
		int n = 0, zone = 0;
		for (int y = 0; y < p.h; y++)
			for (int x = 0; x < p.w; x++)
			{
				const size_t i = (size_t)y * p.w + x;
				const uint16_t w = p.tiles[i];
				const int mx = gx + x, my = gy + y;
				if (w == PIECE_EMPTY || mx < 0 || my < 0 || mx >= lm.cols * 16 || my >= lm.rows * 16) continue;
				int &cell = lm.cells[(my / 16) * lm.cols + mx / 16];
				if (cell < 0) continue;
				const int lx = mx % 16, ly = my % 16;
				if (lm.blocks[cell].half[ly / 8].tiles[(ly % 8) * 16 + lx] == w) continue;
				if (usage(cell) > 1) cell = duplicate_block(cell);
				LevelBlock &b = lm.blocks[cell];
				b.half[ly / 8].tiles[(ly % 8) * 16 + lx] = w;
				b.modified = true;
				mark_block_dirty(cell);
				if (((w >> 10) & 1) && lm.half_bank1(b.half[ly / 8]) != p.bank1[i]) zone++;
				n++;
			}
		if (n) lm.dirty = true;
		if (zone)
			status = std::to_string(zone) + " tile(s) use tile bank 1, which shows other graphics in this zone (see the block's zone bytes)";
		return n;
	}

	// a piece or tile thumbnail laid out in a flowing row; returns true when clicked
	bool flow_image(SDL_Texture *t, ImVec2 size, ImVec2 uv0, ImVec2 uv1, float avail, float &row, bool selected)
	{
		const float sp = ImGui::GetStyle().ItemSpacing.x;
		if (row > 0 && row + size.x <= avail) ImGui::SameLine();
		else row = 0;
		const ImVec2 p0 = ImGui::GetCursorScreenPos();
		ImGui::Image((ImTextureID)(intptr_t)t, size, uv0, uv1);
		const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
		ImDrawList *dl = ImGui::GetWindowDrawList();
		if (selected) dl->AddRect({p0.x - 1, p0.y - 1}, {p0.x + size.x + 1, p0.y + size.y + 1}, IM_COL32(255, 255, 0, 255), 0, 0, 2);
		else if (ImGui::IsItemHovered()) dl->AddRect(p0, {p0.x + size.x, p0.y + size.y}, IM_COL32(255, 255, 255, 160));
		row += size.x + sp;
		return clicked;
	}

	void ui_pieces()
	{
		ImGui::Checkbox("Common pieces only", &lib_common_only);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Pieces the original maps of this level use more than once.\nUntick to see every structure, including one-off parts of larger ones.");
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("Click a piece, then click in the block editor or use the map's Stamp tool.");
		ImGui::PopStyleColor();
		ImGui::BeginChild("pieces", {0, 0}, ImGuiChildFlags_Borders);
		const float avail = ImGui::GetContentRegionAvail().x;
		for (int k = 0; k < (int)PieceKind::Count; k++)
		{
			std::vector<int> list;
			for (int i = 0; i < (int)pieces.size(); i++)
				if ((int)pieces[i].kind == k && (!lib_common_only || pieces[i].uses > 1)) list.push_back(i);
			if (list.empty()) continue;
			char head[96];
			snprintf(head, sizeof(head), "%s (%zu)###kind%d", piece_kind_name((PieceKind)k), list.size(), k);
			if (!ImGui::CollapsingHeader(head, k == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) continue;
			float row = 0;
			for (int i : list)
			{
				const Piece &p = pieces[i];
				const float pw = p.w * 8.0f, ph = p.h * 8.0f;
				const float s = std::max(pw, ph) > 96 ? 96 / std::max(pw, ph) : (std::max(pw, ph) <= 32 ? 2.0f : 1.0f);
				ImGui::PushID(i);
				if (flow_image(piece_texture(i), {pw * s, ph * s}, {0, 0}, {1, 1}, avail, row, i == brush_piece))
				{
					brush_piece = i;
					status = std::string("Brush: ") + piece_kind_name(p.kind) + " piece " + std::to_string(p.w) + "x" + std::to_string(p.h);
				}
				if (ImGui::IsItemHovered())
				{
					ImGui::BeginTooltip();
					ImGui::Text("%d x %d tiles, used %d time%s in the original maps", p.w, p.h, p.uses, p.uses == 1 ? "" : "s");
					const float ts = std::min(2.0f, 320 / std::max(pw, ph));
					ImGui::Image((ImTextureID)(intptr_t)piece_texture(i), {pw * ts, ph * ts});
					ImGui::EndTooltip();
				}
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
	}

	void ui_lib_tiles()
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("The tiles this level uses, by kind. Click one to paint with it.");
		ImGui::PopStyleColor();
		ImGui::BeginChild("libtiles", {0, 0}, ImGuiChildFlags_Borders);
		SDL_Texture *t = lib_tiles_texture();
		const float avail = ImGui::GetContentRegionAvail().x;
		const int rows = ((int)lib_tiles.size() + 31) / 32;
		for (int k = 0; k < (int)PieceKind::Count && t; k++)
		{
			std::vector<int> list;
			for (int i = 0; i < (int)lib_tiles.size(); i++)
				if ((int)lib_tiles[i].kind == k) list.push_back(i);
			if (list.empty()) continue;
			char head[96];
			snprintf(head, sizeof(head), "%s (%zu)###tkind%d", k == (int)PieceKind::Floor ? "Floor (varied by the game)" : piece_kind_name((PieceKind)k), list.size(), k);
			if (!ImGui::CollapsingHeader(head)) continue;
			float row = 0;
			for (int i : list)
			{
				const ImVec2 uv0 = {(i % 32) / 32.0f, (float)(i / 32) / rows};
				const ImVec2 uv1 = {uv0.x + 1 / 32.0f, uv0.y + 1.0f / rows};
				ImGui::PushID(i);
				const LibTile &lt = lib_tiles[i];
				if (flow_image(t, {16, 16}, uv0, uv1, avail, row, brush_piece < 0 && (brush_tile & 0x3fff) == lt.word))
				{
					brush_piece = -1;
					brush_tile = (uint16_t)(lt.word | (pick_prio << 14));
					pick_bank = (lt.word >> 10) & 1;
					pick_pal = (lt.word >> 11) & 7;
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("tile %d, bank %d, palette %d, used %d times", lt.word & 0x3ff, (lt.word >> 10) & 1, (lt.word >> 11) & 7, lt.uses);
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
	}

	SDL_Texture *picker_texture()
	{
		const LevelMap &lm = map();
		const int gbank = pick_bank ? picker_bank1() : lm.tile_bank[0];
		const int key = (map_index * 16 + gbank) * 8 + pick_pal;
		if (picker_tex && picker_key == key) return picker_tex;
		if (!picker_tex)
		{
			picker_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 256, 256);
			SDL_SetTextureScaleMode(picker_tex, SDL_SCALEMODE_NEAREST);
		}
		std::vector<uint32_t> px(256 * 256);
		for (int i = 0; i < 1024; i++)
		{
			const int code = ((gbank << 10) | i) & 8191;
			const uint8_t *src = &gfx.tiles[code * 64];
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 8; x++)
					px[((i / 32) * 8 + y) * 256 + (i % 32) * 8 + x] = pal_rgb(lm, 128 + pick_pal * 16 + src[y * 8 + x]);
		}
		SDL_UpdateTexture(picker_tex, nullptr, px.data(), 256 * 4);
		picker_key = key;
		return picker_tex;
	}

	// ------------------------------------------------------------ editing
	void push_undo()
	{
		LevelMap &lm = map();
		undo_stack.push_back({map_index, lm.cells, lm.alt, lm.blocks, lm.dirty});
		if (undo_stack.size() > 60) undo_stack.erase(undo_stack.begin());
		redo_stack.clear();
		unsaved = true;
	}
	void restore(std::vector<Snapshot> &from, std::vector<Snapshot> &to)
	{
		if (from.empty()) return;
		Snapshot s = std::move(from.back());
		from.pop_back();
		LevelMap &lm = levels.maps[s.map];
		to.push_back({s.map, lm.cells, lm.alt, lm.blocks, lm.dirty});
		lm.cells = std::move(s.cells);
		lm.alt = std::move(s.alt);
		lm.blocks = std::move(s.blocks);
		lm.dirty = s.dirty;
		map_index = s.map;
		if (edit_block >= (int)lm.blocks.size()) edit_block = -1;
		if (brush_block >= (int)lm.blocks.size()) brush_block = -1;
		object_text_for = -1;
		unsaved = true;
		invalidate_all();
	}
	int usage(int bi)
	{
		int n = 0;
		for (int c : map().cells) n += c == bi;
		return n;
	}
	int duplicate_block(int bi)
	{
		LevelMap &lm = map();
		LevelBlock copy = lm.blocks[bi];
		copy.modified = true;
		lm.blocks.push_back(copy);
		return (int)lm.blocks.size() - 1;
	}
	void select_map(int i)
	{
		map_index = i;
		invalidate_all();
		sel_cx = sel_cy = brush_block = edit_block = -1;
		object_text_for = -1;
		fit_pending = true;
	}
	void revert_map()
	{
		LevelMap &lm = map();
		const LevelMap *o = original_map(lm);
		if (!o) return;
		push_undo();
		lm.cells = o->cells;
		lm.alt = o->alt;
		lm.blocks = o->blocks;
		lm.dirty = false;
		sel_cx = sel_cy = brush_block = edit_block = -1;
		object_text_for = -1;
		invalidate_all();
		status = "Reverted " + lm.name + " to the original";
	}

	// ------------------------------------------------------------ files
	bool save_mod(const std::string &path, std::string &report)
	{
		ModPatch p = levels.build_patch(roms, report);
		if (report.find("ERROR") != std::string::npos) return false;
		if (p.chunks.empty()) { report = "no level differs from the original"; return false; }
		std::error_code ec;
		std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
		if (!p.save(path)) { report = "cannot write " + path; return false; }
		return true;
	}

	void save_named()
	{
		std::string name = mod_name;
		for (char &c : name) if (strchr("<>:\"/\\|?*", c)) c = '_';
		if (name.empty()) name = "my_level";
		const std::string path = app + "mods/" + name + ".720mod";
		std::string report;
		if (save_mod(path, report))
		{
			unsaved = false;
			mod_file = path;
			for (char &c : report) if (c == '\n') c = ' ';
			status = "Saved " + path + ".  " + report;
		}
		else
			status = "Not saved: " + report;
	}

	void export_png()
	{
		const LevelMap &lm = map();
		const int W = lm.cols * 128, H = lm.rows * 128;
		std::vector<uint32_t> img((size_t)W * H);
		std::vector<uint32_t> px;
		for (int cy = 0; cy < lm.rows; cy++)
			for (int cx = 0; cx < lm.cols; cx++)
			{
				render_block_pixels(lm, lm.blocks[lm.cell(cx, cy)], px);
				for (int y = 0; y < 128; y++)
					memcpy(&img[(size_t)(cy * 128 + y) * W + cx * 128], &px[y * 128], 128 * 4);
			}
		std::string name = lm.name;
		for (char &c : name) if (!isalnum((unsigned char)c)) c = '_';
		std::error_code ec;
		std::filesystem::create_directories(app + "exports", ec);
		const std::string path = app + "exports/" + name + ".png";
		status = write_png(path, img.data(), W, H) ? "Exported " + path : "PNG export failed";
	}

	void play_test()
	{
		const std::string exe = app + GAME_EXE;
		std::error_code ec;
		if (!std::filesystem::exists(exe, ec)) { status = "Play test: " + exe + " not found (build the game first)"; return; }
		bool edited = false;
		for (auto &m : levels.maps) edited |= m.dirty;
		const std::string path = pref_file("playtest.720mod");
		std::string report;
		if (edited && !save_mod(path, report)) { status = "Play test: " + report; return; }
		std::vector<const char *> args = { exe.c_str(), "--no-mods" };
		if (edited) { args.push_back("--mod"); args.push_back(path.c_str()); }
		args.push_back(nullptr);
		SDL_Process *proc = SDL_CreateProcess(args.data(), false);
		if (!proc) { status = "Play test: could not start the game: " + std::string(SDL_GetError()); return; }
		SDL_DestroyProcess(proc);       // the game keeps running on its own
		status = edited ? "Started the game with your edits (other mods are not loaded)" : "Started the game (no edits yet)";
	}

	// ------------------------------------------------------------ UI
	void ui_menu(bool &quit)
	{
		if (!ImGui::BeginMainMenuBar()) return;
		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("New (original levels)") && (!unsaved || confirm(win, "Discard your unsaved edits?", "Discard")))
			{
				std::string err;
				status = open_levels("", err) ? "Loaded the original levels" : "Error: " + err;
			}
			if (ImGui::BeginMenu("Open mod"))
			{
				const auto mods = list_mods(app + "mods");
				if (mods.empty()) ImGui::TextDisabled("No mods in %smods", app.c_str());
				for (const auto &m : mods)
					if (ImGui::MenuItem(m.c_str()) && (!unsaved || confirm(win, "Discard your unsaved edits?", "Discard")))
					{
						std::string err;
						status = open_levels(app + "mods/" + m, err) ? "Opened " + m : "Could not open " + m + ": " + err;
					}
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("Save mod...", "Ctrl+S")) open_save_popup = true;
			if (ImGui::MenuItem("Export map as PNG")) export_png();
			ImGui::Separator();
			if (ImGui::MenuItem("Quit")) quit = true;
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Edit"))
		{
			if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undo_stack.empty())) restore(undo_stack, redo_stack);
			if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redo_stack.empty())) restore(redo_stack, undo_stack);
			ImGui::Separator();
			if (ImGui::MenuItem("Revert this level to the original")) revert_map();
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("View"))
		{
			ImGui::MenuItem("Grid", nullptr, &show_grid);
			ImGui::MenuItem("Zone bytes", nullptr, &show_attr);
			ImGui::MenuItem("Alternate-bank cells (Skate City)", nullptr, &show_alt);
			ImGui::Separator();
			if (ImGui::MenuItem("Recapture level colours"))
			{
				capture.start(roms);
				capturing = true;
			}
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Play test (F5)")) play_test();
		ImGui::EndMainMenuBar();
	}

	void ui_save_popup()
	{
		if (open_save_popup) { ImGui::OpenPopup("Save mod"); open_save_popup = false; }
		if (ImGui::BeginPopupModal("Save mod", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextUnformatted("Saves every edited level to one mod file in the game's mods folder.");
			ImGui::TextUnformatted("The game loads all mods in that folder when it starts.");
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			const bool enter = ImGui::InputText("name", mod_name, sizeof(mod_name), ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::TextDisabled("%smods/%s.720mod", app.c_str(), mod_name);
			if (ImGui::Button("Save") || enter)
			{
				save_named();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}

	void ui_levels(float x, float y, float w, float h)
	{
		ImGui::SetNextWindowPos({x, y});
		ImGui::SetNextWindowSize({w, h});
		ImGui::Begin("Levels", nullptr, PANEL);
		if (ImGui::BeginChild("list", {0, std::max(60.0f, h - 150)}, ImGuiChildFlags_Borders))
		{
			for (int i = 0; i < (int)levels.maps.size(); i++)
			{
				const LevelMap &lm = levels.maps[i];
				std::string label = lm.name + (lm.dirty ? "  (edited)" : "");
				if (ImGui::Selectable(label.c_str(), i == map_index)) select_map(i);
			}
		}
		ImGui::EndChild();
		const LevelMap &lm = map();
		ImGui::Text("%d x %d cells, %zu blocks", lm.cols, lm.rows, lm.blocks.size());
		if (!lm.shared_layouts.empty())
		{
			std::string s = "This map is also used as layout";
			for (int c : lm.shared_layouts) s += " " + std::to_string(c + 1);
			ImGui::TextWrapped("%s.", s.c_str());
		}
		if (lm.level >= 1 && lm.level <= 4)
			ImGui::TextWrapped("Each park has six layouts; the game moves to later layouts as play progresses.");
		if (lm.level >= 1 && lm.level <= 4 && !capture.got.count(lm.level))
			ImGui::TextWrapped("Colours are approximate: the parks use Skate City's palette here.");
		ImGui::TextDisabled("Mod: %s", mod_file.empty() ? "(none, original levels)" : std::filesystem::path(mod_file).filename().string().c_str());
		ImGui::End();
	}

	void ui_map(float x, float y, float w, float h)
	{
		ImGui::SetNextWindowPos({x, y});
		ImGui::SetNextWindowSize({w, h});
		ImGui::Begin("Map", nullptr, PANEL | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		LevelMap &lm = map();
		ImGui::RadioButton("Select", &tool, 0);
		ImGui::SameLine();
		ImGui::RadioButton("Paint blocks", &tool, 1);
		ImGui::SameLine();
		ImGui::RadioButton("Stamp", &tool, 2);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Put the brush piece or tile (Library panel) on the map, tile by tile.\nBlocks other cells share are copied, so only the place you click changes.");
		ImGui::SameLine();
		ImGui::Checkbox("Grid", &show_grid);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(140);
		ImGui::SliderFloat("Zoom", &zoom, 0.05f, 2.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		ImGui::SameLine();
		if (ImGui::Button("Fit")) fit_pending = true;

		ImDrawList *dl = ImGui::GetWindowDrawList();
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		ImVec2 avail = ImGui::GetContentRegionAvail();
		avail.x = std::max(avail.x, 32.0f);
		avail.y = std::max(avail.y, 32.0f);
		if (fit_pending)
		{
			zoom = std::clamp(std::min(avail.x / (lm.cols * 128.0f), avail.y / (lm.rows * 128.0f)), 0.05f, 2.0f);
			pan = {0, 0};
			fit_pending = false;
		}
		ImGui::InvisibleButton("map_canvas", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
		const bool hovered = ImGui::IsItemHovered();
		ImGuiIO &io = ImGui::GetIO();
		if (hovered && io.MouseWheel != 0)
		{
			const ImVec2 m = {io.MousePos.x - origin.x, io.MousePos.y - origin.y};
			const float c = 128.0f * zoom;
			const float wx = (m.x - pan.x) / c, wy = (m.y - pan.y) / c;
			zoom = std::clamp(zoom * (io.MouseWheel > 0 ? 1.15f : 1 / 1.15f), 0.05f, 2.0f);
			const float nc = 128.0f * zoom;
			pan = {m.x - wx * nc, m.y - wy * nc};
		}
		if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right))
		{
			pan.x += io.MouseDelta.x;
			pan.y += io.MouseDelta.y;
		}
		const float c2 = 128.0f * zoom;
		dl->PushClipRect(origin, {origin.x + avail.x, origin.y + avail.y}, true);
		dl->AddRectFilled(origin, {origin.x + avail.x, origin.y + avail.y}, IM_COL32(20, 20, 24, 255));
		const int cx0 = std::max(0, (int)std::floor(-pan.x / c2)), cy0 = std::max(0, (int)std::floor(-pan.y / c2));
		const int cx1 = std::min(lm.cols - 1, (int)std::floor((avail.x - pan.x) / c2));
		const int cy1 = std::min(lm.rows - 1, (int)std::floor((avail.y - pan.y) / c2));
		for (int cy = cy0; cy <= cy1; cy++)
			for (int cx = cx0; cx <= cx1; cx++)
			{
				const ImVec2 p0 = {origin.x + pan.x + cx * c2, origin.y + pan.y + cy * c2};
				const ImVec2 p1 = {p0.x + c2, p0.y + c2};
				const int bi = lm.cell(cx, cy);
				if (SDL_Texture *t = block_texture(bi)) dl->AddImage((ImTextureID)(intptr_t)t, p0, p1);
				if (show_grid && c2 >= 16) dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 40));
				if (show_alt && lm.level == 0 && lm.alt[cy * lm.cols + cx])
					dl->AddRectFilled(p0, p1, IM_COL32(255, 128, 0, 60));
				if (bi == edit_block) dl->AddRect(p0, p1, IM_COL32(0, 200, 255, 200), 0, 0, 1.5f);
				if (show_attr && c2 >= 48 && bi >= 0)
				{
					char t[32];
					snprintf(t, sizeof(t), "%02X/%02X", lm.blocks[bi].half[0].attr, lm.blocks[bi].half[1].attr);
					dl->AddRectFilled({p0.x + 2, p0.y + 2}, {p0.x + 44, p0.y + 17}, IM_COL32(0, 0, 0, 160));
					dl->AddText({p0.x + 4, p0.y + 3}, IM_COL32(255, 255, 0, 255), t);
				}
			}
		if (sel_cx >= 0 && sel_cx < lm.cols && sel_cy < lm.rows)
		{
			const ImVec2 p0 = {origin.x + pan.x + sel_cx * c2, origin.y + pan.y + sel_cy * c2};
			dl->AddRect(p0, {p0.x + c2, p0.y + c2}, IM_COL32(255, 255, 0, 255), 0, 0, 2.0f);
		}
		dl->PopClipRect();

		const int mcx = (int)std::floor((io.MousePos.x - origin.x - pan.x) / c2);
		const int mcy = (int)std::floor((io.MousePos.y - origin.y - pan.y) / c2);
		const bool in_map = hovered && mcx >= 0 && mcy >= 0 && mcx < lm.cols && mcy < lm.rows;
		if (tool == 2 && hovered)
		{
			// stamp: the brush follows the mouse tile by tile
			const float t8 = 8 * zoom;
			const int gx = (int)std::floor((io.MousePos.x - origin.x - pan.x) / t8);
			const int gy = (int)std::floor((io.MousePos.y - origin.y - pan.y) / t8);
			const Piece p = brush_as_piece();
			const ImVec2 g0 = {origin.x + pan.x + gx * t8, origin.y + pan.y + gy * t8};
			dl->PushClipRect(origin, {origin.x + avail.x, origin.y + avail.y}, true);
			dl->AddImage((ImTextureID)(intptr_t)brush_texture(), g0, {g0.x + p.w * t8, g0.y + p.h * t8}, {0, 0}, {1, 1}, IM_COL32(255, 255, 255, 170));
			dl->AddRect(g0, {g0.x + p.w * t8, g0.y + p.h * t8}, IM_COL32(255, 255, 0, 200));
			dl->PopClipRect();
			const bool single = p.w == 1 && p.h == 1;
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { push_undo(); painting = true; }
			if (painting && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || (single && ImGui::IsMouseDown(ImGuiMouseButton_Left))))
				stamp_on_map(p, gx, gy);
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) painting = false;
		}
		if (in_map && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			sel_cx = mcx;
			sel_cy = mcy;
			if (tool == 1 && brush_block >= 0) { push_undo(); painting = true; }
		}
		if (tool == 1 && painting && in_map && ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			int &c = lm.cells[mcy * lm.cols + mcx];
			if (c != brush_block) { c = brush_block; lm.dirty = true; }
		}
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) painting = false;
		if (in_map && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
		{
			brush_block = lm.cell(mcx, mcy);
			status = "Brush block " + std::to_string(brush_block);
		}
		if (in_map && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && tool == 0)
			edit_block = lm.cell(mcx, mcy);
		if (in_map && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
		{
			const int bi = lm.cell(mcx, mcy);
			ImGui::SetTooltip("cell %d,%d   block %d   zones %02X/%02X%s", mcx, mcy, bi,
				lm.blocks[bi].half[0].attr, lm.blocks[bi].half[1].attr,
				lm.level == 0 && lm.alt[mcy * lm.cols + mcx] ? "   (alternate bank)" : "");
		}
		ImGui::End();
	}

	void ui_blocks(float x, float y, float w, float h)
	{
		ImGui::SetNextWindowPos({x, y});
		ImGui::SetNextWindowSize({w, h});
		ImGui::Begin("Blocks", nullptr, PANEL);
		LevelMap &lm = map();
		if (sel_cx >= 0 && sel_cx < lm.cols && sel_cy < lm.rows)
		{
			const int bi = lm.cell(sel_cx, sel_cy);
			ImGui::Text("Cell %d,%d uses block %d (%d cells)", sel_cx, sel_cy, bi, usage(bi));
			if (ImGui::Button("Edit")) edit_block = bi;
			ImGui::SameLine();
			if (ImGui::Button("Make unique"))
			{
				push_undo();
				const int nb = duplicate_block(bi);
				lm.cells[sel_cy * lm.cols + sel_cx] = nb;
				lm.dirty = true;
				edit_block = nb;
				status = "Cell " + std::to_string(sel_cx) + "," + std::to_string(sel_cy) + " now has its own copy, block " + std::to_string(nb);
			}
			ImGui::SameLine();
			if (ImGui::Button("Use as brush")) { brush_block = bi; tool = 1; }
		}
		else
			ImGui::TextDisabled("Click a map cell to select it.");
		ImGui::Text("Brush: %s", brush_block >= 0 ? ("block " + std::to_string(brush_block)).c_str() : "none");
		ImGui::Separator();
		ImGui::BeginChild("thumbs");
		const float th = 52;
		const float avail = ImGui::GetContentRegionAvail().x;
		const int per_row = std::max(1, (int)(avail / (th + 2 * ImGui::GetStyle().FramePadding.x + ImGui::GetStyle().ItemSpacing.x)));
		for (int i = 0; i < (int)lm.blocks.size(); i++)
		{
			ImGui::PushID(i);
			const bool sel = i == brush_block;
			if (sel) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(220, 180, 0, 255));
			if (ImGui::ImageButton("b", (ImTextureID)(intptr_t)block_texture(i), {th, th}))
			{
				brush_block = i;
				tool = 1;
			}
			if (sel) ImGui::PopStyleColor();
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("block %d%s\nused by %d cells\nzones %02X/%02X%s\ndouble-click to edit", i,
					lm.blocks[i].modified ? " (edited)" : "", usage(i), lm.blocks[i].half[0].attr, lm.blocks[i].half[1].attr,
					lm.blocks[i].objects.empty() ? "" : "\nhas object stamps");
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) edit_block = i;
			}
			if ((i + 1) % per_row) ImGui::SameLine();
			ImGui::PopID();
		}
		ImGui::EndChild();
		ImGui::End();
	}

	void ui_block_editor(float x, float y, float w, float h)
	{
		ImGui::SetNextWindowPos({x, y});
		ImGui::SetNextWindowSize({w, h});
		ImGui::Begin("Block editor", nullptr, PANEL);
		LevelMap &lm = map();
		if (edit_block < 0 || edit_block >= (int)lm.blocks.size())
		{
			edit_block = -1;
			ImGui::TextWrapped("Double-click a map cell or a block to edit it.");
			ImGui::End();
			return;
		}
		LevelBlock &b = lm.blocks[edit_block];
		const int used = usage(edit_block);
		ImGui::Text("Block %d, used by %d cell%s%s", edit_block, used, used == 1 ? "" : "s", b.modified ? ", edited" : "");
		if (used > 1)
			ImGui::TextColored({1, 0.7f, 0.2f, 1}, "Edits change every cell using this block.");
		else
			ImGui::TextUnformatted(" ");
		const float scale = std::clamp(std::floor(std::min(w - 20, h - 150) / 128.0f), 1.0f, 4.0f);
		const ImVec2 p0 = ImGui::GetCursorScreenPos();
		const ImVec2 sz = {128 * scale, 128 * scale};
		ImGui::InvisibleButton("blk", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
		const bool hovered = ImGui::IsItemHovered();
		ImDrawList *dl = ImGui::GetWindowDrawList();
		if (SDL_Texture *t = block_texture(edit_block)) dl->AddImage((ImTextureID)(intptr_t)t, p0, {p0.x + sz.x, p0.y + sz.y});
		for (int i = 1; i < 16; i++)
		{
			dl->AddLine({p0.x + i * 8 * scale, p0.y}, {p0.x + i * 8 * scale, p0.y + sz.y}, IM_COL32(255, 255, 255, 30));
			dl->AddLine({p0.x, p0.y + i * 8 * scale}, {p0.x + sz.x, p0.y + i * 8 * scale}, IM_COL32(255, 255, 255, i == 8 ? 120 : 30));
		}
		ImGuiIO &io = ImGui::GetIO();
		if (hovered)
		{
			const int tx = (int)((io.MousePos.x - p0.x) / (8 * scale)), ty = (int)((io.MousePos.y - p0.y) / (8 * scale));
			if (tx >= 0 && ty >= 0 && tx < 16 && ty < 16)
			{
				dl->AddRect({p0.x + tx * 8 * scale, p0.y + ty * 8 * scale}, {p0.x + (tx + 1) * 8 * scale, p0.y + (ty + 1) * 8 * scale}, IM_COL32(255, 255, 0, 255));
				uint16_t &tw = b.half[ty / 8].tiles[(ty % 8) * 16 + tx];
				ImGui::SetTooltip("tile %d,%d = %04X\nindex %d, bank %d, palette %d, priority %d%s", tx, ty, tw,
					tw & 0x3ff, (tw >> 10) & 1, (tw >> 11) & 7, (tw >> 14) & 3,
					(tw & 0x7ff) < 16 ? "\nground placeholder (varied by the game)" : "");
				if (brush_piece >= 0)
				{
					// stamping a piece: show where it goes, put it there on a click
					const Piece &p = pieces[brush_piece];
					dl->PushClipRect(p0, {p0.x + sz.x, p0.y + sz.y}, true);
					const ImVec2 g0 = {p0.x + tx * 8 * scale, p0.y + ty * 8 * scale};
					dl->AddImage((ImTextureID)(intptr_t)piece_texture(brush_piece), g0, {g0.x + p.w * 8 * scale, g0.y + p.h * 8 * scale},
						{0, 0}, {1, 1}, IM_COL32(255, 255, 255, 170));
					dl->PopClipRect();
					if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
					{
						push_undo();
						if (!stamp_in_block(edit_block, p, tx, ty)) undo_stack.pop_back();
					}
				}
				else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { push_undo(); tile_painting = true; }
				if (tile_painting && ImGui::IsMouseDown(ImGuiMouseButton_Left) && tw != brush_tile)
				{
					tw = brush_tile;
					b.modified = true;
					lm.dirty = true;
					block_dirty[edit_block] = 1;
				}
				if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
				{
					brush_piece = -1;
					brush_tile = tw;
					pick_bank = (tw >> 10) & 1;
					pick_pal = (tw >> 11) & 7;
					pick_prio = (tw >> 14) & 3;
				}
			}
		}
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) tile_painting = false;

		ImGui::TextDisabled("Zone bytes (hex), top and bottom half");
		const uint8_t one = 1;
		uint8_t a0 = b.half[0].attr, a1 = b.half[1].attr;
		ImGui::SetNextItemWidth(110);
		if (ImGui::InputScalar("top", ImGuiDataType_U8, &a0, &one, nullptr, "%02X", ImGuiInputTextFlags_CharsHexadecimal) && a0 != b.half[0].attr)
		{
			push_undo();
			b.half[0].attr = a0 & 0x7f;
			b.modified = lm.dirty = true;
			block_dirty[edit_block] = 1;
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(110);
		if (ImGui::InputScalar("bottom", ImGuiDataType_U8, &a1, &one, nullptr, "%02X", ImGuiInputTextFlags_CharsHexadecimal) && a1 != b.half[1].attr)
		{
			push_undo();
			b.half[1].attr = a1;
			b.modified = lm.dirty = true;
			block_dirty[edit_block] = 1;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Skate City: bits 3+ choose the tile set used for bank-1 tiles once this half\n"
				"scrolls into view (0 = keep the current set); bits 0-2 choose where in the half the\n"
				"switch happens. Other levels do not switch tile sets.");
		if (object_text_for != edit_block)
		{
			std::string s;
			for (size_t i = 0; i < b.objects.size(); i++) s += (i ? "," : "") + std::to_string(b.objects[i]);
			snprintf(object_text, sizeof(object_text), "%s", s.c_str());
			object_text_for = edit_block;
		}
		ImGui::SetNextItemWidth(std::max(120.0f, w - 140));
		if (ImGui::InputText("objects", object_text, sizeof(object_text), ImGuiInputTextFlags_EnterReturnsTrue))
		{
			std::vector<uint8_t> objs;
			std::stringstream ss(object_text);
			std::string tok;
			while (std::getline(ss, tok, ','))
				if (tok.find_first_not_of(" ") != std::string::npos) objs.push_back((uint8_t)std::clamp(atoi(tok.c_str()), 0, 254));
			if (objs != b.objects)
			{
				push_undo();
				b.objects = objs;
				b.modified = lm.dirty = true;
			}
			object_text_for = -1;
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Object stamp numbers drawn over this block, comma separated (Enter to apply)");
		ImGui::End();
	}

	void ui_library(float x, float y, float w, float h)
	{
		ImGui::SetNextWindowPos({x, y});
		ImGui::SetNextWindowSize({w, h});
		ImGui::Begin("Library", nullptr, PANEL);
		build_library();
		if (brush_piece >= 0 && brush_piece < (int)pieces.size())
		{
			const Piece &p = pieces[brush_piece];
			ImGui::Text("Brush: %s piece, %d x %d", piece_kind_name(p.kind), p.w, p.h);
			ImGui::SameLine();
			if (ImGui::SmallButton("Back to single tile")) brush_piece = -1;
		}
		else
			ImGui::Text("Brush: tile %04X", brush_tile);
		if (ImGui::BeginTabBar("library"))
		{
			if (ImGui::BeginTabItem("Pieces")) { ui_pieces(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Tiles")) { ui_lib_tiles(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("All tiles")) { ui_raw_tiles(); ImGui::EndTabItem(); }
			ImGui::EndTabBar();
		}
		ImGui::End();
	}

	void ui_raw_tiles()
	{
		ImGui::SetNextItemWidth(60); ImGui::SliderInt("bank", &pick_bank, 0, 1);
		ImGui::SameLine(); ImGui::SetNextItemWidth(70); ImGui::SliderInt("palette", &pick_pal, 0, 7);
		ImGui::SameLine(); ImGui::SetNextItemWidth(60); ImGui::SliderInt("priority", &pick_prio, 0, 3);
		brush_tile = (uint16_t)((brush_tile & 0x3ff) | (pick_bank << 10) | (pick_pal << 11) | (pick_prio << 14));
		SDL_Texture *pt = picker_texture();
		const int bi = brush_tile & 0x3ff;
		const ImVec2 uv0 = {(bi % 32) / 32.0f, (bi / 32) / 32.0f};
		ImGui::Image((ImTextureID)(intptr_t)pt, {32, 32}, uv0, {uv0.x + 1 / 32.0f, uv0.y + 1 / 32.0f});
		ImGui::SameLine();
		ImGui::Text("Brush tile %04X\nindex %d%s", brush_tile, bi, bi < 16 ? " (ground placeholder)" : "");
		ImGui::BeginChild("picker", {0, 0}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
		const float scale = 2.0f;
		const ImVec2 p0 = ImGui::GetCursorScreenPos();
		ImGui::Image((ImTextureID)(intptr_t)pt, {256 * scale, 256 * scale});
		if (ImGui::IsItemHovered())
		{
			ImGuiIO &io = ImGui::GetIO();
			const int tx = std::clamp((int)((io.MousePos.x - p0.x) / (8 * scale)), 0, 31);
			const int ty = std::clamp((int)((io.MousePos.y - p0.y) / (8 * scale)), 0, 31);
			const int idx = ty * 32 + tx;
			ImGui::SetTooltip("tile %d%s", idx, idx < 16 ? " (ground placeholder)" : "");
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				brush_tile = (uint16_t)(idx | (pick_bank << 10) | (pick_pal << 11) | (pick_prio << 14));
				brush_piece = -1;
			}
		}
		const ImVec2 b0 = {p0.x + (bi % 32) * 8 * scale, p0.y + (bi / 32) * 8 * scale};
		ImGui::GetWindowDrawList()->AddRect(b0, {b0.x + 8 * scale, b0.y + 8 * scale}, IM_COL32(255, 255, 0, 255));
		ImGui::EndChild();
	}

	void ui_status(float x, float y, float w, float h)
	{
		ImGui::SetNextWindowPos({x, y});
		ImGui::SetNextWindowSize({w, h});
		ImGui::Begin("status", nullptr, PANEL | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs);
		ImGui::TextUnformatted(status.c_str());
		ImGui::End();
	}

	void ui_capture()
	{
		const ImGuiViewport *vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x / 2, vp->WorkPos.y + vp->WorkSize.y / 2}, 0, {0.5f, 0.5f});
		ImGui::Begin("Preparing", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
		ImGui::TextUnformatted("Running the game briefly to capture the level colours.");
		ImGui::TextUnformatted("This happens once; the result is cached.");
		const float p = capture.step(14'000'000);
		ImGui::ProgressBar(p, {380, 0});
		ImGui::End();
		if (capture.done)
		{
			capture.save(pref_file("720_palettes.bin"));
			apply_capture();
			capturing = false;
			status = "Captured colours for " + std::to_string(capture.got.size()) + " level(s)";
		}
	}

	void frame(bool &quit)
	{
		ImGuiIO &io = ImGui::GetIO();
		if (capturing) { ui_capture(); return; }
		if (!io.WantTextInput)
		{
			if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) restore(undo_stack, redo_stack);
			if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) restore(redo_stack, undo_stack);
			if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) open_save_popup = true;
			if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) play_test();
		}
		ui_menu(quit);
		const ImGuiViewport *vp = ImGui::GetMainViewport();
		const float X = vp->WorkPos.x, Y = vp->WorkPos.y, W = vp->WorkSize.x;
		const float SH = ImGui::GetFrameHeight() + 8;
		const float H = std::max(200.0f, vp->WorkSize.y - SH);
		const float left = 300, right = 470;
		const float levels_h = std::min(340.0f, H * 0.4f);
		const float editor_h = std::min(H * 0.5f, 128 * 3 + 220.0f);
		ui_levels(X, Y, left, levels_h);
		ui_blocks(X, Y + levels_h, left, H - levels_h);
		ui_map(X + left, Y, std::max(100.0f, W - left - right), H);
		ui_block_editor(X + W - right, Y, right, editor_h);
		ui_library(X + W - right, Y + editor_h, right, H - editor_h);
		ui_status(X, Y + H, W, SH);
		ui_save_popup();
	}
};

int capture_test(const std::string &rom_dir)
{
	RomSet roms;
	std::string err;
	if (!roms.load(rom_dir, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
	PaletteCapture cap;
	const uint64_t t0 = SDL_GetTicksNS();
	cap.start(roms);
	while (!cap.done) cap.step(1'000'000'000);
	printf("%d frames in %.1f s\n", cap.frame, (SDL_GetTicksNS() - t0) / 1e9);
	for (auto &kv : cap.got)
		printf("level %d: palette[128..131] %04X %04X %04X %04X\n", kv.first,
			kv.second.palette[128], kv.second.palette[129], kv.second.palette[130], kv.second.palette[131]);
	return cap.complete() ? 0 : 2;
}

}

int main(int argc, char **argv)
{
	std::string rom_dir, shot, open_mod;
	bool test = false;
	int shot_map = 0, shot_block = -1;
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		if (a == "--roms" && i + 1 < argc) rom_dir = argv[++i];
		else if (a == "--capture-test") test = true;
		// --screenshot FILE [--map N] [--block B]: render the UI once, save it and exit
		else if (a == "--screenshot" && i + 1 < argc) shot = argv[++i];
		else if (a == "--map" && i + 1 < argc) shot_map = atoi(argv[++i]);
		else if (a == "--block" && i + 1 < argc) shot_block = atoi(argv[++i]);
		else if (a == "--open" && i + 1 < argc) open_mod = argv[++i];
	}
	if (test) return capture_test(find_rom_dir(rom_dir));

	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		SDL_Log("SDL_Init failed: %s", SDL_GetError());
		return 1;
	}
	static Editor ed;
	ed.rom_dir = find_rom_dir(rom_dir);
	ed.app = app_dir();
	std::string err;
	if (!ed.init(err))
	{
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "720 Degrees level editor",
			("Could not load the levels from the ROM files in\n" + ed.rom_dir + "\n\n" + err +
			 "\n\nPut the 720 Degrees ROM files in the Game folder next to the editor.").c_str(), nullptr);
		return 1;
	}
	if (ed.capture.load(pref_file("720_palettes.bin"))) ed.apply_capture();
	else { ed.capture.start(ed.roms); ed.capturing = true; }
	if (!open_mod.empty())
		ed.status = ed.open_levels(open_mod, err) ? "Opened " + open_mod : "Could not open " + open_mod + ": " + err;
	if (!shot.empty())
	{
		if (ed.capturing)
		{
			while (!ed.capture.done) ed.capture.step(1'000'000'000);
			ed.capture.save(pref_file("720_palettes.bin"));
			ed.apply_capture();
			ed.capturing = false;
		}
		ed.map_index = std::clamp(shot_map, 0, (int)ed.levels.maps.size() - 1);
		ed.edit_block = shot_block;
		ed.show_attr = true;
	}

	if (!SDL_CreateWindowAndRenderer("720 Degrees - Level Editor", 1600, 960,
	                                 shot.empty() ? SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED : 0, &ed.win, &ed.ren))
	{
		SDL_Log("window creation failed: %s", SDL_GetError());
		return 1;
	}
	set_window_icon(ed.win);
	SDL_SetRenderVSync(ed.ren, 1);
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;
	ImGui::StyleColorsDark();
	ImGui_ImplSDL3_InitForSDLRenderer(ed.win, ed.ren);
	ImGui_ImplSDLRenderer3_Init(ed.ren);

	bool quit = false;
	while (!quit)
	{
		SDL_Event e;
		bool close = false;
		while (SDL_PollEvent(&e))
		{
			ImGui_ImplSDL3_ProcessEvent(&e);
			if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) close = true;
		}
		ImGui_ImplSDLRenderer3_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();
		ed.frame(quit);
		ImGui::Render();
		SDL_SetRenderDrawColor(ed.ren, 30, 30, 34, 255);
		SDL_RenderClear(ed.ren);
		ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ed.ren);
		static int shot_frames = 0;
		if (!shot.empty() && ++shot_frames == 4)
		{
			if (SDL_Surface *s = SDL_RenderReadPixels(ed.ren, nullptr))
			{
				if (SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888))
				{
					std::vector<uint32_t> px((size_t)c->w * c->h);
					for (int y = 0; y < c->h; y++)
						memcpy(&px[(size_t)y * c->w], (const uint8_t *)c->pixels + (size_t)y * c->pitch, (size_t)c->w * 4);
					write_png(shot, px.data(), c->w, c->h);
					SDL_DestroySurface(c);
				}
				SDL_DestroySurface(s);
			}
			ed.unsaved = false;
			quit = true;
		}
		SDL_RenderPresent(ed.ren);
		if (close || quit)
			quit = !ed.unsaved || confirm(ed.win, "You have unsaved edits. Quit without saving?", "Quit");
	}
	ed.invalidate_all();
	if (ed.picker_tex) SDL_DestroyTexture(ed.picker_tex);
	ed.drop_library_textures();
	ImGui_ImplSDLRenderer3_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();
	SDL_DestroyRenderer(ed.ren);
	SDL_DestroyWindow(ed.win);
	SDL_Quit();
	return 0;
}
