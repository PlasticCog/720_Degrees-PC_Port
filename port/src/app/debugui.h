// Debug menu (TAB): overlays (sprites, terrain height map), game objects and skater state,
// physics tuning (jump power, gravity, kick and top speed), cheats, sound test, memory
// viewer / search and emulation control (pause, frame step, speed, layers).
//
// RAM and ROM locations used here were found with ref720 --watch and render720 --ram-dump
// (see port/src/app/debugui.cpp for the list).
#pragma once
#include <SDL3/SDL.h>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

struct RomSet;
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
	bool visible = false;
	void set_overlays(bool sprites, bool height) { m_show_sprites = sprites; m_show_height = height; }
	int select_tab = -1;                                  // open this tab on the next frame

	void attach(NativeMachine &m);                        // after creating or resetting the machine
	void after_frame(NativeMachine &m);                   // after every emulated frame
	// overlays drawn with SDL onto the game image (before ImGui renders)
	void draw_sdl_overlays(NativeMachine &m, SDL_Renderer *ren, const SDL_FRect &game);
	// ImGui windows and ImGui-drawn overlays; call between ImGui::NewFrame and ImGui::Render
	void draw(NativeMachine &m, const SDL_FRect &game, RunControl &run, const AudioOut &audio, double fps);

private:
	RomSet &m_roms;
	uint64_t m_frame = 0;

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
	void draw_sprite_overlay(NativeMachine &m, const SDL_FRect &game);
	void draw_height_probe(NativeMachine &m, const SDL_FRect &game);
	int terrain_byte(int code, bool secondary) const;
};
