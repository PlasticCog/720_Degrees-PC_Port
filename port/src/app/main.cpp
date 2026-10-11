// 720 Degrees - native port (C++ / SDL3).
//
// The arcade board's two programs are rewritten in C++ (src/logic, src/sound); graphics,
// sound chips and controls are implemented natively here and in src/board.
//
// Default controls (change them in the game with F4, saved to controls.cfg in the
// user's settings folder):
//   Arrows / WASD ....... point the skater          Q / E ........ rotate
//   Ctrl ................ Jump                       Shift ........ Kick
//   5 / 6 ............... insert coin                P ............ pause
//   1 / 2 ............... start a one / two player game (Jump, Kick and the gamepad start one)
//   Tab ................. debug menu                 F4 ........... controls
//   F1 help   F2 service mode   F3 reset   F11 / Alt+Enter fullscreen   Esc release mouse / quit
// Xbox controller: left stick or d-pad points, right stick or LB/RB rotate, A or RT jump,
//   X or B kick, View = coin, Menu = pause.
// Mouse (click the game to capture): moving rotates, left button jumps, right button kicks.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "nativemachine.h"
#include "soundchips.h"
#include "controls.h"
#include "modloader.h"
#include "audioout.h"
#include "input_config.h"
#include "debugui.h"
#include "uiscale.h"
#include "romzip.h"
#include "window_icon.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>

namespace {

struct Options {
	std::string roms;
	bool fullscreen = false;
	int scale = 2;
	bool smooth = false;
	int dsw0 = 0x00, dsw1 = 0x55;
	bool invert_rotation = false;
	std::vector<std::string> mods;
	bool no_mods = false;
	double quit_after = 0;               // testing: quit after this many seconds and print audio stats
	std::string screenshot;              // testing: save the window to this PNG when quitting
	bool debug_open = false, overlays = false;
	int debug_tab = -1;
};

std::string find_roms(const std::string &requested)
{
	if (!requested.empty()) return requested;
	const char *base = SDL_GetBasePath();
	std::vector<std::string> candidates;
	// the Game folder next to the program, the program's own folder, then Game folders above
	// it (for running from the build directory)
	if (base)
		for (const char *rel : { "Game", "", "../Game", "../../Game", "../../../Game" })
			candidates.push_back(std::string(base) + rel);
	for (const char *rel : { "Game", "." }) candidates.push_back(rel);
	for (auto &c : candidates)
		if (rom_folder_usable(c)) return c;         // the ROM files, or a zip holding them
	return std::string(base ? base : "") + "Game";
}

std::string pref_file(const char *name)
{
	char *p = SDL_GetPrefPath("Atari", "720Degrees");
	std::string s = p ? std::string(p) + name : std::string(name);
	SDL_free(p);
	return s;
}

void load_eeprom(Board &b)
{
	std::ifstream f(pref_file("720.eeprom"), std::ios::binary);
	if (f) f.read((char *)b.eeprom(), 0x200);
}

void save_eeprom(Board &b)
{
	std::ofstream f(pref_file("720.eeprom"), std::ios::binary);
	if (f) f.write((const char *)b.eeprom(), 0x200);
}

// letterboxed 4:3 rectangle for the game image inside the window
SDL_FRect game_rect(SDL_Renderer *ren)
{
	int w = 0, h = 0;
	SDL_GetCurrentRenderOutputSize(ren, &w, &h);
	const float s = std::min(w / (float)Board::SCREEN_W, h / (float)Board::SCREEN_H);
	const float gw = Board::SCREEN_W * s, gh = Board::SCREEN_H * s;
	return { (w - gw) / 2, (h - gh) / 2, gw, gh };
}

void draw_help(const Input &input)
{
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x / 2, vp->WorkPos.y + vp->WorkSize.y / 2}, ImGuiCond_Always, {0.5f, 0.5f});
	ImGui::SetNextWindowBgAlpha(0.85f);
	ImGui::Begin("Help (F1)", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
	ImGui::TextUnformatted("720 DEGREES - native port");
	ImGui::Separator();
	for (int i = 0; i < ACTION_COUNT; i++)
	{
		const Binding &b = input.cfg.bind[i];
		std::string keys = Input::key_name(b.keys[0]);
		if (b.keys[1] != SDL_SCANCODE_UNKNOWN) keys += " / " + Input::key_name(b.keys[1]);
		std::string pad = Input::pad_name(b.pad[0]);
		if (b.pad[1].kind != PadInput::None) pad += " / " + Input::pad_name(b.pad[1]);
		ImGui::Text("%-16s %-22s %s", InputConfig::action_name((Action)i), keys.c_str(), pad.c_str());
	}
	ImGui::Separator();
	ImGui::TextUnformatted("Tab debug menu    F4 controls    F2 service mode    F3 reset");
	ImGui::TextUnformatted("F11 / Alt+Enter fullscreen    Esc release mouse / quit");
	ImGui::TextUnformatted("Mouse: click the game to capture it; move to rotate, buttons jump / kick");
	ImGui::End();
}

}

int main(int argc, char **argv)
{
	Options opt;
	for (int i = 1; i < argc; i++)
	{
		std::string a = argv[i];
		auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
		if (a == "--roms") opt.roms = next();
		else if (a == "--fullscreen") opt.fullscreen = true;
		else if (a == "--scale") opt.scale = std::max(1, atoi(next().c_str()));
		else if (a == "--smooth") opt.smooth = true;
		else if (a == "--dsw0") opt.dsw0 = (int)strtol(next().c_str(), nullptr, 0);
		else if (a == "--dsw1") opt.dsw1 = (int)strtol(next().c_str(), nullptr, 0);
		else if (a == "--invert-rotation") opt.invert_rotation = true;
		else if (a == "--mod") opt.mods.push_back(next());
		else if (a == "--no-mods") opt.no_mods = true;
		else if (a == "--quit-after") opt.quit_after = atof(next().c_str());
		else if (a == "--screenshot") opt.screenshot = next();
		else if (a == "--debug") opt.debug_open = true;
		else if (a == "--debug-tab") { opt.debug_open = true; opt.debug_tab = atoi(next().c_str()); }
		else if (a == "--overlays") opt.overlays = true;
	}

	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))
	{
		SDL_Log("SDL_Init failed: %s", SDL_GetError());
		return 1;
	}

	RomSet roms;
	std::string err;
	const std::string romdir = find_roms(opt.roms);
	if (!roms.load(romdir, err))
	{
		const std::string msg = "Could not load the 720 Degrees ROM set from '" + romdir + "':\n" + err +
			"\n\nPut the ROM files in the 'Game' folder next to the game, or pass --roms <folder>.";
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "720 Degrees", msg.c_str(), nullptr);
		return 1;
	}

	SDL_Window *window = nullptr;
	SDL_Renderer *renderer = nullptr;
	if (!SDL_CreateWindowAndRenderer("720 Degrees", 512 * opt.scale, 384 * opt.scale, SDL_WINDOW_RESIZABLE, &window, &renderer))
	{
		SDL_Log("window creation failed: %s", SDL_GetError());
		return 1;
	}
	set_window_icon(window);
	SDL_SetRenderVSync(renderer, 1);
	if (opt.fullscreen) SDL_SetWindowFullscreen(window, true);
	SDL_Texture *tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 512, 384);
	SDL_SetTextureScaleMode(tex, opt.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	static const std::string imgui_ini = pref_file("debug_layout.ini");
	ImGui::GetIO().IniFilename = imgui_ini.c_str();
	ImGui::StyleColorsDark();
	ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
	ImGui_ImplSDLRenderer3_Init(renderer);

	AudioOut audio;
	if (!audio.open((int)std::lround(SoundChips::output_rate())))
		SDL_Log("audio unavailable: %s", SDL_GetError());

	Input input;
	input.config_path = pref_file("controls.cfg");
	if (!input.cfg.load(input.config_path))
	{
		input.cfg.set_defaults();
		input.cfg.save(input.config_path);      // a starting point for hand edits
	}
	if (opt.invert_rotation) input.cfg.invert_rotation = true;
	input.open_gamepads();

	// level mods: mods/*.720mod next to the game (unless --no-mods) and --mod <file>
	{
		std::string errors;
		std::vector<std::string> applied;
		if (!opt.no_mods)
		{
			const char *base = SDL_GetBasePath();
			applied = apply_mod_folder(roms, std::string(base ? base : "") + "mods", errors);
		}
		for (const auto &m : opt.mods)
		{
			std::string e;
			if (apply_mod_file(roms, m, e)) applied.push_back(m);
			else errors += m + ": " + e + "\n";
		}
		for (const auto &a : applied) SDL_Log("level mod loaded: %s", a.c_str());
		if (!errors.empty())
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "720 Degrees", ("Some level mods could not be loaded:\n" + errors).c_str(), nullptr);
	}

	auto machine = std::make_unique<NativeMachine>(roms);
	DebugUI dbg(roms);
	dbg.load_extras(pref_file("extras.cfg"));
	dbg.load_ui(pref_file("ui.cfg"));
	apply_ui_scale(dbg.ui_scale());
	auto start_machine = [&]() {
		machine->reset();
		load_eeprom(machine->board);
		machine->board.input.dsw0 = (uint8_t)opt.dsw0;
		machine->board.input.dsw1 = (uint8_t)opt.dsw1;
		dbg.attach(*machine);
	};
	start_machine();
	dbg.visible = opt.debug_open;
	dbg.select_tab = opt.debug_tab;
	if (opt.overlays) dbg.set_overlays(true, true);

	RotaryController rotary;
	RunControl run;
	bool running = true, help = false, mouse_captured = false, test_switch = false;
	double mouse_accum = 0;
	std::vector<uint32_t> pixels(512 * 384);
	std::vector<int16_t> samples(8192 * 2), frame_audio;
	const double frame_ns = 1e9 * 640.0 * 416.0 / 16000000.0;   // 60.096 Hz
	uint64_t next_frame = SDL_GetTicksNS();
	uint64_t fps_t0 = next_frame;
	int fps_frames = 0;
	bool was_paused = false;
	const uint64_t t_start = next_frame;
	uint64_t total_frames = 0;
	double fps = 0;
	ImGuiIO &io = ImGui::GetIO();

	auto release_mouse = [&]() { if (mouse_captured) { mouse_captured = false; SDL_SetWindowRelativeMouseMode(window, false); } };
	auto toggle_fullscreen = [&]() { SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)); };

	while (running)
	{
		SDL_Event e;
		while (SDL_PollEvent(&e))
		{
			ImGui_ImplSDL3_ProcessEvent(&e);
			// closing the window quits, whatever else is going on (rebinding, other windows)
			if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) { running = false; continue; }
			if (input.handle_event(e)) continue;          // gamepad hotplug or rebinding
			const bool ui_keys = io.WantTextInput;
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && !ui_keys)
			{
				switch (e.key.key)
				{
				case SDLK_TAB: dbg.visible = !dbg.visible; if (dbg.visible) release_mouse(); break;
				case SDLK_F4: run.show_controls = !run.show_controls; if (run.show_controls) release_mouse(); break;
				case SDLK_ESCAPE:
					if (mouse_captured) release_mouse();
					else if (run.show_controls) run.show_controls = false;
					else if (dbg.visible) dbg.visible = false;
					else if (help) help = false;
					else running = false;
					break;
				case SDLK_F1: help = !help; break;
				case SDLK_F2: test_switch = !test_switch; break;
				case SDLK_F3: save_eeprom(machine->board); start_machine(); break;
				case SDLK_F11: toggle_fullscreen(); break;
				case SDLK_RETURN: if (e.key.mod & SDL_KMOD_ALT) toggle_fullscreen(); break;
				default: break;
				}
			}
			if (!ui_keys && input.is_action(e, Action::Pause)) run.paused = !run.paused;
			if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !mouse_captured && !io.WantCaptureMouse && input.cfg.mouse_enabled &&
			    !dbg.visible && !run.show_controls)
			{
				mouse_captured = true;
				SDL_SetWindowRelativeMouseMode(window, true);
			}
			if (e.type == SDL_EVENT_MOUSE_MOTION && mouse_captured) mouse_accum += e.motion.xrel;
		}
		if (run.reset) { run.reset = false; save_eeprom(machine->board); start_machine(); }

		// ---- controls
		const InputFrame in_frame = input.poll(!io.WantTextInput && !input.capturing(), mouse_captured, mouse_accum);
		mouse_accum = 0;
		if (in_frame.aim) rotary.set_target_angle(in_frame.angle_deg);
		else rotary.clear_target();
		if (in_frame.rotate != 0) rotary.add_rotation(in_frame.rotate);

		// ---- run the game at its native 60.096 Hz (scaled by the debug speed setting)
		const uint64_t now = SDL_GetTicksNS();
		const double step_ns = frame_ns / std::clamp(run.speed, 0.05f, 8.0f);
		if (now > next_frame + (uint64_t)(step_ns * 8)) next_frame = now;   // resync after a stall
		int frames_run = 0;
		if (was_paused && !run.paused) audio.restart();
		was_paused = run.paused;
		auto run_one = [&]() {
			rotary.update();
			Inputs &in = machine->board.input;
			in.leta_rotate = rotary.rotate_count();
			in.leta_center = rotary.center_count();
			// arcade button 1 = Kick, button 2 = Jump.  While the attract mode waits for a start
			// (state 0x018F = 4) the arcade program starts one player with button 1 and two (with
			// two credits) with button 2; here only the 2 key starts two players, everything else
			// (Jump, Kick, the mouse, the gamepad, the 1 key) one.  In the continue countdown
			// (state 9) the start keys press button 1 too; elsewhere they do nothing.
			const uint8_t state = machine->board.main_ram()[0x018F];
			bool b1 = in_frame.kick, b2 = in_frame.jump;
			if (!test_switch && state == 4)
			{
				b1 = in_frame.kick || in_frame.jump || in_frame.start1;
				b2 = in_frame.start2;
			}
			else if (!test_switch && state == 9)
				b1 = b1 || in_frame.start1 || in_frame.start2;
			in.button1 = b1;
			in.button2 = b2;
			in.coin1 = in_frame.coin1;
			in.coin2 = in_frame.coin2;
			in.test = test_switch;
			dbg.apply_inputs(in);
			machine->run_frame();
			dbg.after_frame(*machine);
			frames_run++;
			fps_frames++;
			total_frames++;
			auto &chips = machine->board.chips();
			frame_audio.clear();
			size_t n;
			while ((n = chips.take_samples(samples.data(), samples.size() / 2)) > 0)
				frame_audio.insert(frame_audio.end(), samples.begin(), samples.begin() + n * 2);
			audio.push(frame_audio.data(), frame_audio.size() / 2);
		};
		if (run.paused)
		{
			next_frame = now;
			for (; run.step > 0; run.step--) run_one();
		}
		else
			while (now >= next_frame && frames_run < 8)
			{
				run_one();
				next_frame += (uint64_t)step_ns;
			}

		// ---- present
		if (now - fps_t0 >= 1000000000ull)
		{
			fps = fps_frames * 1e9 / (double)(now - fps_t0);
			fps_t0 = now;
			fps_frames = 0;
		}
		machine->board.frame_rgb(pixels.data());
		SDL_UpdateTexture(tex, nullptr, pixels.data(), 512 * 4);
		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
		SDL_RenderClear(renderer);
		const SDL_FRect gr = game_rect(renderer);
		SDL_RenderTexture(renderer, tex, nullptr, &gr);
		dbg.draw_sdl_overlays(*machine, renderer, gr);

		if (float ratio; dbg.take_ui_scale_change(ratio)) apply_ui_scale(dbg.ui_scale(), ratio);
		ImGui_ImplSDLRenderer3_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();
		dbg.draw(*machine, gr, run, audio, fps);
		if (run.show_controls) input.draw_window(&run.show_controls);
		if (help) draw_help(input);
		if (run.paused)
			ImGui::GetForegroundDrawList()->AddText({gr.x + gr.w / 2 - 24, gr.y + 8}, IM_COL32(255, 255, 255, 255), "PAUSED");
		ImGui::Render();
		ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
		const bool quitting = opt.quit_after > 0 && (SDL_GetTicksNS() - t_start) / 1e9 >= opt.quit_after;
		if (quitting && !opt.screenshot.empty())
			if (SDL_Surface *s = SDL_RenderReadPixels(renderer, nullptr))
			{
				if (SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888))
				{
					std::vector<uint32_t> px((size_t)c->w * c->h);
					for (int y = 0; y < c->h; y++)
						memcpy(&px[(size_t)y * c->w], (const uint8_t *)c->pixels + (size_t)y * c->pitch, (size_t)c->w * 4);
					write_png(opt.screenshot, px.data(), c->w, c->h);
					SDL_DestroySurface(c);
				}
				SDL_DestroySurface(s);
			}
		SDL_RenderPresent(renderer);
		if (quitting) running = false;
	}
	if (opt.quit_after > 0)
	{
		const auto &st = audio.stats;
		printf("%llu frames in %.1f s; audio queue ran dry %llu times in %llu frames (min %.1f ms, mean %.1f ms), %llu dropped\n",
			(unsigned long long)total_frames, (SDL_GetTicksNS() - t_start) / 1e9, (unsigned long long)st.dry,
			(unsigned long long)st.checks, st.checks ? st.min_ms : 0.0, st.checks ? st.sum_ms / st.checks : 0.0,
			(unsigned long long)st.dropped);
	}

	// leaving: silent and gone from the screen at once, the EEPROM saved; should tearing down
	// (the machine, the gamepads, SDL) hang, the process ends anyway a few seconds later
	audio.close();
	SDL_HideWindow(window);
	save_eeprom(machine->board);
	fflush(stdout);
	std::thread([] { std::this_thread::sleep_for(std::chrono::seconds(3)); std::_Exit(0); }).detach();
	machine.reset();
	input.close_gamepads();
	ImGui_ImplSDLRenderer3_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();
	SDL_DestroyTexture(tex);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
}
