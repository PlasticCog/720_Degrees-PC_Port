// Debug menu, see debugui.h.
//
// Main RAM map used here (T-11 addresses):
//   objects: 32 per-object word arrays, index i at base + 2*i
//     0x04F0 x speed   0x0570 y speed   0x05B0 height speed  0x06F0 x  0x0730 y  0x06B0 height
//     0x0630 screen x  0x0670 screen y  byte 0x0370+i: active
//     object 0 is the skater; objects 1-3 follow it (body parts)
//   skater: 0x01B4 speed, 0x01EB kick/jump phase, 0x0327 terrain code underfoot,
//           0x02EE vertical acceleration (gravity while airborne), 0x025F equipment level
//   game:   0x023E score, 0x0256 cash, 0x0254 timer, 0x025B park tickets, 0x0F59 level
// Terrain: one byte per playfield tile code in ROM bank 0x0800 (window A), the "secondary"
//   table in bank 0x0C00 (window B); bits 3-6 = height level (heights at 0xFCB0, 512 units
//   per level, level 3 = ground), bits 0-2 = slope type.
// Physics tables (bank 0x2C00, window B, indexed by equipment level): 0x7C70 jump launch
//   speed (bytes), 0x7C80 kick power, 0x7C88 top speed (words).
#include "debugui.h"
#include "audioout.h"
#include "board.h"
#include "leveldata.h"
#include "nativemachine.h"
#include "soundchips.h"
#include "t11rt.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint16_t OBJ_VX = 0x04F0, OBJ_VY = 0x0570, OBJ_VZ = 0x05B0, OBJ_X = 0x06F0, OBJ_Y = 0x0730,
	OBJ_Z = 0x06B0, OBJ_SX = 0x0630, OBJ_SY = 0x0670, OBJ_ACTIVE = 0x0370;
constexpr uint16_t SK_SPEED = 0x01B4, SK_PHASE = 0x01EB, SK_TERRAIN = 0x0327, SK_ACCEL = 0x02EE, SK_EQUIP = 0x025F;
constexpr uint16_t G_SCORE = 0x023E, G_CASH = 0x0256, G_TIMER = 0x0254, G_TICKETS = 0x025B, G_LEVEL = 0x0F59;
constexpr int HEIGHTS[16] = { -1536, -1024, -512, 0, 512, 1024, 1536, 2048, 2560, 3072, 3584, 4096, 4608, 5120, 6144, 7680 };
const char *LEVELS[8] = { "Skate City", "Park 1", "Park 2", "Park 3", "Park 4", "Screen 5", "Screen 6", "Screen 7" };
const char *PHASES[] = { "on the ground", "kick", "kick", "kick (pushing)", "jump take-off", "jump take-off", "in the air", "in the air", "in the air", "landing" };

int16_t rw(NativeMachine &m, uint16_t a) { const uint8_t *r = m.board.main_ram(); return (int16_t)(r[a] | (r[a + 1] << 8)); }
void ww(NativeMachine &m, uint16_t a, uint16_t v) { uint8_t *r = m.board.main_ram(); r[a] = (uint8_t)v; r[a + 1] = (uint8_t)(v >> 8); }
uint8_t rb(NativeMachine &m, uint16_t a) { return m.board.main_ram()[a]; }

ImU32 height_color(int t)
{
	const int level = (t >> 3) & 15, slope = t & 7;
	if (t & 0x80) return IM_COL32(255, 40, 40, 255);                 // flagged tile
	if (slope)
	{
		static const ImU32 slopes[8] = { 0, IM_COL32(255, 0, 255, 255), IM_COL32(190, 0, 255, 255), IM_COL32(120, 60, 255, 255),
			IM_COL32(255, 80, 180, 255), IM_COL32(255, 140, 220, 255), IM_COL32(200, 120, 255, 255), IM_COL32(150, 150, 255, 255) };
		return slopes[slope];
	}
	if (level == 3) return 0;                                         // ground: transparent
	if (level < 3) return IM_COL32(0, 60 + level * 50, 255, 255);     // below ground
	if (level == 15) return IM_COL32(255, 255, 255, 255);
	const float f = (level - 4) / 10.0f;                              // raised: green -> yellow -> red
	const int r = (int)std::clamp(f * 2 * 255, 0.0f, 255.0f), g = (int)std::clamp((2 - f * 2) * 255, 0.0f, 255.0f);
	return IM_COL32(r, g, 0, 255);
}

}

DebugUI::DebugUI(RomSet &roms) : m_roms(roms)
{
	m_tab = (size_t)LevelSet::rom_bank_offset(0x2C00) + (0x7C70 - 0x6000);
	const auto &rom = m_roms.main;
	if (m_tab + 0x20 <= rom.size())
		for (int i = 0; i < 4; i++)
		{
			m_orig_jump[i] = rom[m_tab + i];
			m_orig_kick[i] = (uint16_t)(rom[m_tab + 0x10 + 2 * i] | (rom[m_tab + 0x11 + 2 * i] << 8));
			m_orig_top[i] = (uint16_t)(rom[m_tab + 0x18 + 2 * i] | (rom[m_tab + 0x19 + 2 * i] << 8));
		}
}

void DebugUI::attach(NativeMachine &m)
{
	m.board.on_sound_command = [this](uint8_t c) {
		m_cmd_count[c]++;
		m_cmd_log.push_back({m_frame, c});
		if (m_cmd_log.size() > 200) m_cmd_log.pop_front();
	};
	apply_physics();
}

void DebugUI::apply_physics()
{
	auto &rom = m_roms.main;
	if (m_tab + 0x20 > rom.size()) return;
	for (int i = 0; i < 4; i++)
	{
		rom[m_tab + i] = (uint8_t)std::clamp((int)std::lround(m_orig_jump[i] * m_jump_pct / 100.0), 1, 255);
		const int k = std::clamp((int)std::lround(m_orig_kick[i] * m_kick_pct / 100.0), 0, 0x7fff);
		const int t = std::clamp((int)std::lround(m_orig_top[i] * m_top_pct / 100.0), 0, 0x7fff);
		rom[m_tab + 0x10 + 2 * i] = (uint8_t)k; rom[m_tab + 0x11 + 2 * i] = (uint8_t)(k >> 8);
		rom[m_tab + 0x18 + 2 * i] = (uint8_t)t; rom[m_tab + 0x19 + 2 * i] = (uint8_t)(t >> 8);
	}
	t11rt::g_patch_gravity = (uint16_t)m_gravity;
	t11rt::g_patch_kick_speed = (uint16_t)m_kick_start;
}

void DebugUI::after_frame(NativeMachine &m)
{
	m_frame++;
	if (m_freeze_timer) ww(m, G_TIMER, m_timer_value);
	for (auto &w : m_watches)
		if (w.freeze)
		{
			if (w.word) ww(m, w.addr, w.value);
			else m.board.main_ram()[w.addr] = (uint8_t)w.value;
		}
	auto &chips = m.board.chips();
	chips.debug_gain[0] = m_mute[0] ? 0.0f : 1.0f;
	chips.debug_gain[1] = m_mute[1] ? 0.0f : 1.0f;
	chips.debug_gain[2] = m_mute[2] ? 0.0f : 1.0f;
	if (m_frame % 15 == 0)
	{
		auto &st = chips.stats;
		auto db = [&](double e) { return st.samples && e > 0 ? (float)(10 * std::log10(e / st.samples)) : -99.0f; };
		m_chip_db[0] = db(st.ym); m_chip_db[1] = db(st.pokey[0]); m_chip_db[2] = db(st.pokey[1]); m_chip_db[3] = db(st.tms);
		st = SoundChips::Stats();
	}
}

int DebugUI::terrain_byte(int code, bool secondary) const
{
	const size_t off = (size_t)LevelSet::rom_bank_offset(secondary ? 0x0C00 : 0x0800) + (code & 0x1fff);
	return off < m_roms.main.size() ? m_roms.main[off] : 0;
}

// ------------------------------------------------------------------ overlays

void DebugUI::draw_sdl_overlays(NativeMachine &m, SDL_Renderer *ren, const SDL_FRect &game)
{
	m.board.debug.capture_tiles = m_show_height;
	if (!m_show_height) return;
	if (!m_height_tex)
	{
		m_height_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, Board::SCREEN_W, Board::SCREEN_H);
		SDL_SetTextureBlendMode(m_height_tex, SDL_BLENDMODE_BLEND);
		SDL_SetTextureScaleMode(m_height_tex, SDL_SCALEMODE_NEAREST);
	}
	m_height_px.resize(Board::SCREEN_W * Board::SCREEN_H);
	const uint16_t *codes = m.board.debug_tile_codes();
	const uint32_t a = (uint32_t)std::clamp(m_height_alpha * 255.0f, 0.0f, 255.0f) << 24;
	int last = -1;
	uint32_t last_px = 0;
	for (size_t i = 0; i < m_height_px.size(); i++)
	{
		if (codes[i] != last)
		{
			last = codes[i];
			const ImU32 c = height_color(terrain_byte(last, m_height_secondary));
			// ImU32 is ABGR; the texture wants ARGB
			last_px = c ? (a | ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff)) : 0;
		}
		m_height_px[i] = last_px;
	}
	SDL_UpdateTexture(m_height_tex, nullptr, m_height_px.data(), Board::SCREEN_W * 4);
	SDL_RenderTexture(ren, m_height_tex, nullptr, &game);
}

void DebugUI::draw_sprite_overlay(NativeMachine &m, const SDL_FRect &game)
{
	ImDrawList *dl = ImGui::GetBackgroundDrawList();
	const float sx = game.w / Board::SCREEN_W, sy = game.h / Board::SCREEN_H;
	const uint16_t *mo = m.board.mo_ram();
	bool visited[256] = {};
	int link = 0, last_x = 0;
	dl->PushClipRect({game.x, game.y}, {game.x + game.w, game.y + game.h}, true);
	for (int n = 0; n < 256 && !visited[link]; n++)
	{
		const uint16_t *e = &mo[link * 4];
		visited[link] = true;
		const int code = (e[1] & 0x7ff) | ((e[0] & 7) << 11);
		int xpos = (e[2] >> 6) & 0x3ff, ypos = -((e[0] >> 6) & 0x1ff);
		const int height = ((e[1] >> 11) & 7) + 1;
		ypos -= height << 4;
		if (e[1] & 0x8000) xpos = last_x + 16;
		last_x = xpos;
		xpos &= 1023;
		ypos &= 511;
		if (xpos >= 640) xpos -= 1024;
		if (ypos >= Board::VTOTAL) ypos -= 512;
		const int idx = link;
		link = (e[3] >> 3) & 0xff;
		if (xpos <= -16 || xpos >= Board::SCREEN_W || ypos >= Board::SCREEN_H || ypos + height * 16 <= 0) continue;
		const ImVec2 p0 = {game.x + xpos * sx, game.y + ypos * sy}, p1 = {game.x + (xpos + 16) * sx, game.y + (ypos + height * 16) * sy};
		dl->AddRect(p0, p1, IM_COL32(0, 255, 255, 220));
		if (m_sprite_labels)
		{
			char t[48];
			snprintf(t, sizeof(t), "%d (%d,%d)", idx, xpos, ypos);
			dl->AddText({p0.x + 1, p0.y - 13}, IM_COL32(0, 0, 0, 255), t);
			dl->AddText({p0.x, p0.y - 14}, IM_COL32(0, 255, 255, 255), t);
		}
		(void)code;
	}
	dl->PopClipRect();
}

void DebugUI::draw_height_probe(NativeMachine &m, const SDL_FRect &game)
{
	const ImVec2 mp = ImGui::GetIO().MousePos;
	if (ImGui::GetIO().WantCaptureMouse) return;
	const int px = (int)((mp.x - game.x) * Board::SCREEN_W / game.w), py = (int)((mp.y - game.y) * Board::SCREEN_H / game.h);
	if (px < 0 || py < 0 || px >= Board::SCREEN_W || py >= Board::SCREEN_H) return;
	const int code = m.board.debug_tile_codes()[py * Board::SCREEN_W + px];
	const int t = terrain_byte(code, false), t2 = terrain_byte(code, true);
	ImGui::SetTooltip("pixel %d,%d  tile %d (bank %d, index %d)\nterrain %02X: height level %d = %d, slope %d%s\nsecondary %02X",
		px, py, code, code >> 10, code & 0x3ff, t, (t >> 3) & 15, HEIGHTS[(t >> 3) & 15], t & 7, (t & 0x80) ? ", flagged" : "", t2);
}

// ------------------------------------------------------------------ windows

void DebugUI::draw(NativeMachine &m, const SDL_FRect &game, RunControl &run, const AudioOut &audio, double fps)
{
	if (m_show_sprites) draw_sprite_overlay(m, game);
	if (m_show_height) draw_height_probe(m, game);
	if (!visible) return;
	ImGui::SetNextWindowPos({10, 10}, ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize({470, 560}, ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Debug (Tab)", &visible)) { ImGui::End(); return; }
	if (ImGui::BeginTabBar("tabs"))
	{
		auto tab = [&](int i, const char *name) {
			const bool open = ImGui::BeginTabItem(name, nullptr, select_tab == i ? ImGuiTabItemFlags_SetSelected : 0);
			return open;
		};
		if (tab(0, "Game")) { tab_game(m, run, audio, fps); ImGui::EndTabItem(); }
		if (tab(1, "Overlays")) { tab_overlays(m); ImGui::EndTabItem(); }
		if (tab(2, "Skater")) { tab_skater(m); ImGui::EndTabItem(); }
		if (tab(3, "Objects")) { tab_objects(m); ImGui::EndTabItem(); }
		if (tab(4, "Physics")) { tab_physics(); ImGui::EndTabItem(); }
		if (tab(5, "Cheats")) { tab_cheats(m); ImGui::EndTabItem(); }
		if (tab(6, "Sound")) { tab_sound(m); ImGui::EndTabItem(); }
		if (tab(7, "Memory")) { tab_memory(m); ImGui::EndTabItem(); }
		ImGui::EndTabBar();
		select_tab = -1;
	}
	ImGui::End();
}

void DebugUI::tab_game(NativeMachine &m, RunControl &run, const AudioOut &audio, double fps)
{
	const int level = rb(m, G_LEVEL);
	ImGui::Text("%.1f fps   frame %llu   level %d (%s)", fps, (unsigned long long)m.board.frame_number, level, level < 8 ? LEVELS[level] : "?");
	ImGui::Checkbox("Pause (P)", &run.paused);
	ImGui::SameLine();
	ImGui::BeginDisabled(!run.paused);
	if (ImGui::Button("Step 1 frame")) run.step = 1;
	ImGui::SameLine();
	if (ImGui::Button("Step 10")) run.step = 10;
	ImGui::EndDisabled();
	ImGui::SetNextItemWidth(200);
	ImGui::SliderFloat("Game speed", &run.speed, 0.1f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
	ImGui::SameLine();
	if (ImGui::SmallButton("1x")) run.speed = 1.0f;
	if (ImGui::Button("Reset machine (F3)")) run.reset = true;
	ImGui::SameLine();
	if (ImGui::Button("Controls... (F4)")) run.show_controls = true;
	ImGui::SeparatorText("Layers");
	ImGui::Checkbox("Hide playfield", &m.board.debug.hide_pf);
	ImGui::SameLine();
	ImGui::Checkbox("Hide sprites", &m.board.debug.hide_mo);
	ImGui::SameLine();
	ImGui::Checkbox("Hide text", &m.board.debug.hide_alpha);
	ImGui::SeparatorText("Video");
	ImGui::Text("scroll x %04X  y %04X   tile banks %d / %d", m.board.xscroll(), m.board.yscroll(), m.board.pf_tile_bank(0), m.board.pf_tile_bank(1));
	ImGui::SeparatorText("Audio");
	const auto &st = audio.stats;
	ImGui::Text("queue: mean %.1f ms, min %.1f ms, ran dry %llu times, dropped %llu",
		st.checks ? st.sum_ms / st.checks : 0.0, st.checks ? st.min_ms : 0.0, (unsigned long long)st.dry, (unsigned long long)st.dropped);
	ImGui::SeparatorText("Keys");
	ImGui::TextDisabled("Tab debug menu   F4 controls   F1 help   P pause   F3 reset\nF2 service mode   F11 fullscreen   Esc release mouse / quit");
}

void DebugUI::tab_overlays(NativeMachine &m)
{
	ImGui::Checkbox("Sprite boxes and coordinates", &m_show_sprites);
	ImGui::SameLine();
	ImGui::Checkbox("labels", &m_sprite_labels);
	ImGui::TextDisabled("Boxes show each hardware sprite: list index (screen x, y).");
	ImGui::Separator();
	ImGui::Checkbox("Terrain height map", &m_show_height);
	ImGui::SetNextItemWidth(200);
	ImGui::SliderFloat("opacity", &m_height_alpha, 0.1f, 1.0f, "%.2f");
	ImGui::Checkbox("Show the secondary terrain table instead", &m_height_secondary);
	ImGui::TextWrapped("Every playfield tile has a terrain byte: a height level and a slope type. "
		"Hover over the game to read the value under the mouse.");
	auto swatch = [](ImU32 c, const char *label) {
		ImGui::ColorButton(label, ImGui::ColorConvertU32ToFloat4(c), ImGuiColorEditFlags_NoTooltip, {14, 14});
		ImGui::SameLine();
		ImGui::TextUnformatted(label);
	};
	swatch(height_color(0 << 3), "below ground");
	swatch(height_color(4 << 3), "raised, low");
	swatch(height_color(9 << 3), "raised, middle");
	swatch(height_color(14 << 3), "raised, high");
	swatch(height_color(1), "slopes (one colour per slope type)");
	swatch(height_color(0x80), "flagged tile (bit 7)");
	ImGui::TextDisabled("Ground level is left clear.");
}

void DebugUI::tab_skater(NativeMachine &m)
{
	const int phase = rb(m, SK_PHASE), terrain = rb(m, SK_TERRAIN);
	ImGui::Text("position  x %6d  y %6d  height %6d", rw(m, OBJ_X), rw(m, OBJ_Y), rw(m, OBJ_Z));
	ImGui::Text("velocity  x %6d  y %6d  height %6d", rw(m, OBJ_VX), rw(m, OBJ_VY), rw(m, OBJ_VZ));
	ImGui::Text("speed %d   phase %d (%s)", rw(m, SK_SPEED), phase, phase < (int)(sizeof(PHASES) / sizeof(PHASES[0])) ? PHASES[phase] : "?");
	ImGui::Text("terrain underfoot %02X: height level %d (%d), slope %d", terrain, (terrain >> 3) & 15, HEIGHTS[(terrain >> 3) & 15], terrain & 7);
	ImGui::Text("vertical acceleration %d   equipment level %d", rw(m, SK_ACCEL), rb(m, SK_EQUIP));
	ImGui::Separator();
	ImGui::Text("score %u   cash $%d   timer %d   park tickets %d", (unsigned)(uint16_t)rw(m, G_SCORE), rw(m, G_CASH), rw(m, G_TIMER), rb(m, G_TICKETS));
	ImGui::Text("joystick: rotate count %d  center count %d   buttons: jump %d kick %d",
		m.board.input.leta_rotate, m.board.input.leta_center, m.board.input.button2, m.board.input.button1);
}

void DebugUI::tab_objects(NativeMachine &m)
{
	static bool all = false;
	ImGui::Checkbox("Show inactive objects", &all);
	if (ImGui::BeginTable("objs", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, {0, -1}))
	{
		const char *cols[] = { "#", "x", "y", "height", "vx", "vy", "vz", "screen x", "screen y" };
		ImGui::TableSetupScrollFreeze(0, 1);
		for (const char *c : cols) ImGui::TableSetupColumn(c);
		ImGui::TableHeadersRow();
		for (int i = 0; i < 32; i++)
		{
			const bool active = rb(m, (uint16_t)(OBJ_ACTIVE + i)) != 0;
			if (!active && !all) continue;
			const uint16_t o = (uint16_t)(2 * i);
			ImGui::TableNextRow();
			if (i == 0) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(60, 60, 20, 255));
			ImGui::TableNextColumn(); ImGui::Text("%d%s", i, i == 0 ? " skater" : "");
			const uint16_t addrs[] = { OBJ_X, OBJ_Y, OBJ_Z, OBJ_VX, OBJ_VY, OBJ_VZ, OBJ_SX, OBJ_SY };
			for (uint16_t a : addrs) { ImGui::TableNextColumn(); ImGui::Text("%d", rw(m, (uint16_t)(a + o))); }
		}
		ImGui::EndTable();
	}
}

void DebugUI::tab_physics()
{
	bool ch = false;
	ImGui::TextWrapped("These change the game's own physics values while it runs. They reset when the game is restarted.");
	ImGui::SetNextItemWidth(220);
	ch |= ImGui::SliderFloat("Jump power", &m_jump_pct, 25.0f, 150.0f, "%.0f%%");
	ImGui::TextDisabled("  launch speed %d / %d / %d / %d by equipment level (max 255)",
		m_roms.main[m_tab], m_roms.main[m_tab + 1], m_roms.main[m_tab + 2], m_roms.main[m_tab + 3]);
	ImGui::SetNextItemWidth(220);
	ch |= ImGui::SliderInt("Gravity", &m_gravity, 1, 40);
	ImGui::TextDisabled("  height speed lost per frame in the air (game: 12); lower = higher, longer jumps");
	ImGui::SetNextItemWidth(220);
	ch |= ImGui::SliderFloat("Kick power", &m_kick_pct, 25.0f, 300.0f, "%.0f%%");
	ImGui::SetNextItemWidth(220);
	ch |= ImGui::SliderFloat("Top speed", &m_top_pct, 25.0f, 300.0f, "%.0f%%");
	ImGui::SetNextItemWidth(220);
	ch |= ImGui::SliderInt("Kick start speed", &m_kick_start, 0x40, 0x600);
	ImGui::TextDisabled("  speed after a kick from standstill (game: 384)");
	if (ImGui::Button("Restore the game's values"))
	{
		m_jump_pct = m_kick_pct = m_top_pct = 100;
		m_gravity = 12;
		m_kick_start = 0x180;
		ch = true;
	}
	if (ch) apply_physics();
}

void DebugUI::tab_cheats(NativeMachine &m)
{
	if (ImGui::Checkbox("Freeze timer", &m_freeze_timer) && m_freeze_timer)
	{
		m_timer_value = (uint16_t)rw(m, G_TIMER);
		if (m_timer_value < 20) m_timer_value = 124;
	}
	ImGui::SameLine();
	if (ImGui::Button("Refill timer")) ww(m, G_TIMER, 124), m_timer_value = 124;
	if (ImGui::Button("Add a park ticket")) m.board.main_ram()[G_TICKETS] = (uint8_t)std::min(4, rb(m, G_TICKETS) + 1);
	ImGui::SameLine();
	ImGui::TextDisabled("(%d held)", rb(m, G_TICKETS));
	static int cash = 500, score = 10000;
	ImGui::SetNextItemWidth(120);
	ImGui::InputInt("##cash", &cash);
	ImGui::SameLine();
	if (ImGui::Button("Set cash")) ww(m, G_CASH, (uint16_t)std::clamp(cash, 0, 9999));
	ImGui::SetNextItemWidth(120);
	ImGui::InputInt("##score", &score);
	ImGui::SameLine();
	if (ImGui::Button("Set score")) ww(m, G_SCORE, (uint16_t)std::clamp(score, 0, 65535));
	int equip = rb(m, SK_EQUIP);
	ImGui::SetNextItemWidth(120);
	if (ImGui::SliderInt("Equipment level", &equip, 0, 3)) m.board.main_ram()[SK_EQUIP] = (uint8_t)equip;
	ImGui::TextDisabled("Equipment (bought in the shops) selects the jump and speed table entries.");
}

void DebugUI::tab_sound(NativeMachine &m)
{
	ImGui::SeparatorText("Sound test");
	{
		uint8_t c = (uint8_t)m_sound_cmd;
		const uint8_t one = 1;
		ImGui::SetNextItemWidth(110);
		if (ImGui::InputScalar("Command (hex)", ImGuiDataType_U8, &c, &one, nullptr, "%02X", ImGuiInputTextFlags_CharsHexadecimal))
			m_sound_cmd = c;
	}
	ImGui::SameLine();
	if (ImGui::Button("Play")) m.board.debug_send_sound((uint8_t)m_sound_cmd);
	ImGui::TextDisabled("Sends the command to the sound CPU as the game would (%02X).", m_sound_cmd);
	if (!m_cmd_count.empty())
	{
		ImGui::TextUnformatted("Heard this session (click to play again):");
		int n = 0;
		for (const auto &kv : m_cmd_count)
		{
			char b[24];
			snprintf(b, sizeof(b), "%02X x%d", kv.first, kv.second);
			if (n++ % 6) ImGui::SameLine();
			if (ImGui::SmallButton(b)) { m_sound_cmd = kv.first; m.board.debug_send_sound((uint8_t)kv.first); }
		}
	}
	ImGui::SeparatorText("Chips");
	const char *names[4] = { "YM2151 (music and effects)", "POKEY 1", "POKEY 2", "TMS5220 (speech)" };
	for (int i = 0; i < 4; i++)
	{
		const float v = std::clamp((m_chip_db[i] + 60.0f) / 60.0f, 0.0f, 1.0f);
		char t[24];
		snprintf(t, sizeof(t), m_chip_db[i] > -98 ? "%.0f dB" : "silent", m_chip_db[i]);
		ImGui::ProgressBar(v, {160, 0}, t);
		ImGui::SameLine();
		ImGui::TextUnformatted(names[i]);
	}
	ImGui::Checkbox("Mute YM2151", &m_mute[0]);
	ImGui::SameLine();
	ImGui::Checkbox("Mute POKEYs", &m_mute[1]);
	ImGui::SameLine();
	ImGui::Checkbox("Mute speech", &m_mute[2]);
	ImGui::SeparatorText("Recent commands");
	if (ImGui::BeginChild("log", {0, 0}, ImGuiChildFlags_Borders))
	{
		for (auto it = m_cmd_log.rbegin(); it != m_cmd_log.rend(); ++it)
			if (it->second != 0x17) ImGui::Text("frame %llu: %02X", (unsigned long long)it->first, it->second);
		ImGui::TextDisabled("(command 17, sent every few frames, is hidden)");
	}
	ImGui::EndChild();
}

void DebugUI::tab_memory(NativeMachine &m)
{
	uint8_t *ram = m.board.main_ram();
	ImGui::SeparatorText("Watches");
	ImGui::SetNextItemWidth(80);
	ImGui::InputInt("##addr", &m_watch_addr, 1, 16, ImGuiInputTextFlags_CharsHexadecimal);
	m_watch_addr = std::clamp(m_watch_addr, 0, 0xffe);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(120);
	ImGui::InputText("name", m_watch_name, sizeof(m_watch_name));
	ImGui::SameLine();
	if (ImGui::Button("Add word")) m_watches.push_back({(uint16_t)m_watch_addr, true, false, 0, m_watch_name});
	ImGui::SameLine();
	if (ImGui::Button("Add byte")) m_watches.push_back({(uint16_t)m_watch_addr, false, false, 0, m_watch_name});
	for (size_t i = 0; i < m_watches.size(); i++)
	{
		Watch &w = m_watches[i];
		ImGui::PushID((int)i);
		int v = w.word ? (int)(int16_t)(ram[w.addr] | (ram[w.addr + 1] << 8)) : ram[w.addr];
		ImGui::Text("%04X %-10s", w.addr, w.name.c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(110);
		if (ImGui::InputInt("##v", &v, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
		{
			if (w.word) { ram[w.addr] = (uint8_t)v; ram[w.addr + 1] = (uint8_t)(v >> 8); }
			else ram[w.addr] = (uint8_t)v;
			w.value = (uint16_t)v;
		}
		ImGui::SameLine();
		if (ImGui::Checkbox("freeze", &w.freeze) && w.freeze) w.value = (uint16_t)v;
		ImGui::SameLine();
		if (ImGui::SmallButton("x")) { m_watches.erase(m_watches.begin() + i); ImGui::PopID(); break; }
		ImGui::PopID();
	}

	ImGui::SeparatorText("Search");
	ImGui::SetNextItemWidth(110);
	ImGui::InputInt("value", &m_search_value);
	ImGui::SameLine();
	ImGui::Checkbox("word", &m_search_word);
	auto value_at = [&](uint16_t a) { return m_search_word ? (int)(int16_t)(ram[a] | (ram[a + 1] << 8)) : (int)ram[a]; };
	auto prev_at = [&](uint16_t a) { return m_search_word ? (int)(int16_t)(m_search_prev[a] | (m_search_prev[a + 1] << 8)) : (int)m_search_prev[a]; };
	auto filter = [&](auto keep) {
		std::vector<uint16_t> out;
		for (uint16_t a : m_candidates) if (keep(a)) out.push_back(a);
		m_candidates.swap(out);
		m_search_prev.assign(ram, ram + 0x1000);
	};
	if (ImGui::Button("New search"))
	{
		m_candidates.clear();
		for (int a = 0; a < 0x1000 - (m_search_word ? 1 : 0); a += m_search_word ? 2 : 1) m_candidates.push_back((uint16_t)a);
		m_search_prev.assign(ram, ram + 0x1000);
		m_search_active = true;
	}
	ImGui::BeginDisabled(!m_search_active);
	ImGui::SameLine();
	if (ImGui::Button("= value")) filter([&](uint16_t a) { return value_at(a) == m_search_value; });
	ImGui::SameLine();
	if (ImGui::Button("changed")) filter([&](uint16_t a) { return value_at(a) != prev_at(a); });
	ImGui::SameLine();
	if (ImGui::Button("same")) filter([&](uint16_t a) { return value_at(a) == prev_at(a); });
	ImGui::SameLine();
	if (ImGui::Button("up")) filter([&](uint16_t a) { return value_at(a) > prev_at(a); });
	ImGui::SameLine();
	if (ImGui::Button("down")) filter([&](uint16_t a) { return value_at(a) < prev_at(a); });
	ImGui::EndDisabled();
	if (m_search_active)
	{
		ImGui::Text("%zu candidates", m_candidates.size());
		for (size_t i = 0; i < m_candidates.size() && i < 12; i++)
		{
			ImGui::Text("%04X = %d", m_candidates[i], value_at(m_candidates[i]));
			ImGui::SameLine();
			ImGui::PushID((int)i);
			if (ImGui::SmallButton("watch")) m_watches.push_back({m_candidates[i], m_search_word, false, 0, ""});
			ImGui::PopID();
		}
	}

	ImGui::SeparatorText("Main RAM");
	if (ImGui::BeginChild("hex", {0, 0}, ImGuiChildFlags_Borders))
	{
		ImGuiListClipper clip;
		clip.Begin(0x1000 / 16);
		while (clip.Step())
			for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++)
			{
				ImGui::Text("%04X", row * 16);
				for (int c = 0; c < 16; c++)
				{
					const int a = row * 16 + c;
					ImGui::SameLine(0, c == 8 ? 10 : 4);
					ImGui::PushID(a);
					ImGui::SetNextItemWidth(20);
					ImGui::InputScalar("##b", ImGuiDataType_U8, &ram[a], nullptr, nullptr, "%02X",
						ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_AutoSelectAll);
					ImGui::PopID();
				}
			}
	}
	ImGui::EndChild();
}
