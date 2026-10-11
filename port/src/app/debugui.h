// Debug menu (TAB): overlays (sprites, terrain height map), game objects and skater state,
// physics tuning (jump power, gravity, kick and top speed), cheats, sound test, memory
// viewer / search, emulation control (pause, frame step, speed, layers), "Extras": unused
// content brought back and bug fixes (extras.h, saved in extras.cfg), and "Tricks": the
// document docs/programming-tricks.md (built into the program) about how the game works.
//
// RAM and ROM locations used here were found with ref720 --watch and render720 --ram-dump
// (see port/src/app/debugui.cpp for the list).
#pragma once
#include <SDL3/SDL.h>
#include "docview.h"
#include "extras.h"
#include <memory>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

struct RomSet;
struct Inputs;
struct GfxSet;
namespace sprites { class SpriteSet; }
class NativeMachine;
class AudioOut;

struct RunControl {
	bool paused = false;
	int step = 0;                 // frames to run while paused
	float speed = 1.0f;           // emulation speed factor
	bool reset = false;           // request a machine reset
	bool show_controls = false;   // open the controls window
};

class DebugUI {
public:
	explicit DebugUI(RomSet &roms);
	~DebugUI();
	bool visible = false;
	void set_overlays(bool sprites, bool height) { m_show_sprites = sprites; m_show_height = height; }
	int select_tab = -1;                                  // open this tab on the next frame

	void attach(NativeMachine &m);                        // after creating or resetting the machine
	void after_frame(NativeMachine &m);                   // after every emulated frame
	// extras: load (and apply) the saved settings; set the inputs the extras drive before a frame
	void load_extras(const std::string &path);
	void apply_inputs(Inputs &in);
	// the menu size (1.0 - 2.5, uiscale.h), saved in ui.cfg; a change is taken by main, which
	// rebuilds the fonts outside the ImGui frame
	void load_ui(const std::string &path);
	float ui_scale() const { return m_ui_scale; }
	bool take_ui_scale_change(float &ratio)
	{
		if (!m_ui_scale_changed) return false;
		m_ui_scale_changed = false;
		ratio = m_ui_scale / m_ui_scale_old;
		return true;
	}
	// overlays drawn with SDL onto the game image (before ImGui renders)
	void draw_sdl_overlays(NativeMachine &m, SDL_Renderer *ren, const SDL_FRect &game);
	// ImGui windows and ImGui-drawn overlays; call between ImGui::NewFrame and ImGui::Render
	void draw(NativeMachine &m, const SDL_FRect &game, RunControl &run, const AudioOut &audio, double fps);

private:
	RomSet &m_roms;
	uint64_t m_frame = 0;
	float m_ui_scale = 1.0f, m_ui_scale_old = 1.0f;
	bool m_ui_scale_changed = false, m_fit_window = true;
	std::string m_ui_path;
	void save_ui();

	// overlays
	bool m_show_sprites = false, m_sprite_labels = true, m_show_height = false, m_height_secondary = false;
	float m_height_alpha = 0.55f;
	SDL_Texture *m_height_tex = nullptr;
	std::vector<uint32_t> m_height_px;

	// physics (ROM tables in bank 0x2C00 and translated-code patch points)
	size_t m_tab = 0;                                     // ROM offset of the 0x7C70 table
	uint8_t m_orig_jump[4] = {};
	uint16_t m_orig_kick[4] = {}, m_orig_top[4] = {};
	float m_jump_pct = 100, m_kick_pct = 100, m_top_pct = 100;
	int m_gravity = 12, m_kick_start = 0x180;
	void apply_physics();

	// cheats
	bool m_freeze_timer = false;
	uint16_t m_timer_value = 0;

	// sound
	int m_sound_cmd = 0x17;
	std::map<int, int> m_cmd_count;
	std::deque<std::pair<uint64_t, int>> m_cmd_log;
	float m_chip_db[4] = {-99, -99, -99, -99};          // YM, POKEY 1, POKEY 2, speech
	bool m_mute[3] = {false, false, false};

	// extras (unused content, bug fixes)
	Extras m_extras;
	std::string m_extras_path;
	bool m_dev_freeze = false;
	int m_dev_step_frames = 0;
	std::unique_ptr<GfxSet> m_gfx;                        // hex-digit labels: the decoded sprites
	std::unique_ptr<sprites::SpriteSet> m_sprites;
	SDL_Texture *m_digit_tex[16] = {};
	void save_extras();
	void draw_hex_labels(NativeMachine &m, SDL_Renderer *ren, const SDL_FRect &game);

	// the programming tricks document
	DocView m_tricks;
	bool m_tricks_loaded = false, m_tricks_window = false;

	// memory
	struct Watch { uint16_t addr; bool word; bool freeze; uint16_t value; std::string name; };
	std::vector<Watch> m_watches;
	std::vector<uint16_t> m_candidates;
	std::vector<uint8_t> m_search_prev;
	bool m_search_word = false, m_search_active = false;
	int m_search_value = 0;
	char m_watch_name[32] = "";
	int m_watch_addr = 0x0254;

	void tab_game(NativeMachine &m, RunControl &run, const AudioOut &audio, double fps);
	void tab_overlays(NativeMachine &m);
	void tab_skater(NativeMachine &m);
	void tab_objects(NativeMachine &m);
	void tab_physics();
	void tab_cheats(NativeMachine &m);
	void tab_sound(NativeMachine &m);
	void tab_memory(NativeMachine &m);
	void tab_tricks();
	void tab_extras(NativeMachine &m);
	void draw_sprite_overlay(NativeMachine &m, const SDL_FRect &game);
	void draw_height_probe(NativeMachine &m, const SDL_FRect &game);
	int terrain_byte(int code, bool secondary) const;
};
