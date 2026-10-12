// Finding and installing the ROM files, see romsetup.h
#include "romsetup.h"
#include "board.h"
#include "romzip.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "uiscale.h"
#include <mutex>

namespace {

// the files of MAME's set '720' (rev 4): name, size, CRC-32
struct RomFile { const char *name; uint32_t size, crc; };
const RomFile ROM_FILES[] = {
	{"136047-1101.5t", 0x10000, 0x2AC77B80}, {"136047-1102.5sr", 0x10000, 0xF19C3B06},
	{"136047-1103.5p", 0x10000, 0x78F9AB90}, {"136047-1104.5n", 0x10000, 0x77CE4A7F},
	{"136047-1105.5m", 0x10000, 0xBEF5A025}, {"136047-1106.5l", 0x10000, 0x92A159C8},
	{"136047-1107.5kj", 0x10000, 0x0A94A3EF}, {"136047-1108.5jh", 0x10000, 0x9815EDA6},
	{"136047-1109.6t", 0x10000, 0x0A46B693}, {"136047-1110.6sr", 0x10000, 0x457D7E38},
	{"136047-1111.6p", 0x10000, 0xFFAD0A5B}, {"136047-1112.6n", 0x10000, 0x06664580},
	{"136047-1113.6m", 0x10000, 0x7445DC0F}, {"136047-1114.6l", 0x10000, 0x23EACEB0},
	{"136047-1115.6kj", 0x10000, 0x0CC8DE53}, {"136047-1116.6jh", 0x10000, 0x2D8F1369},
	{"136047-1117.6c", 0x08000, 0x5A55F149}, {"136047-1118.6d", 0x08000, 0x9BB2429E},
	{"136047-1119.7d", 0x08000, 0x8F7B20E5}, {"136047-1120.7c", 0x08000, 0x46AF6D35},
	{"136047-1121.6a", 0x08000, 0x7ADB5F9A}, {"136047-1122.6b", 0x08000, 0x41B60141},
	{"136047-1123.7a", 0x08000, 0x501881D5}, {"136047-1124.7b", 0x08000, 0x096F2574},
	{"136047-1125.4t", 0x04000, 0x6B7E2328}, {"136047-1129.6hj", 0x10000, 0xEABF0B01},
	{"136047-1130.6k", 0x10000, 0x93FBA845}, {"136047-1132.6p", 0x10000, 0xA24F333E},
	{"136047-1133.6r", 0x10000, 0x53C177BE}, {"136047-1135.2b", 0x04000, 0xB1F157D0},
	{"136047-2134.2a", 0x04000, 0x0DB4CA28}, {"136047-2136.2cd", 0x04000, 0x00B06BEC},
	{"136047-3126.7lm", 0x04000, 0x43ABD367}, {"136047-3127.7mn", 0x04000, 0x772E1E5B},
	{"136047-3128.6fh", 0x10000, 0xBF6F425B}, {"136047-4131.6mn", 0x10000, 0x2EA8A20F},
	{"720-eeprom.bin", 0x00200, 0xCFE1C24E},
};

std::string pref_dir()
{
	char *p = SDL_GetPrefPath("Atari", "720Degrees");
	std::string s = p ? p : "";
	SDL_free(p);
	return s;
}

// the file dialog's answer arrives on another thread (Android's UI thread, a Windows worker)
struct Picked {
	std::mutex lock;
	bool done = false;
	std::vector<std::string> files;
	std::string error;
};
Picked g_picked;           // outlives the screen, in case a dialog is still open when it ends

void SDLCALL on_picked(void *, const char *const *list, int)
{
	std::lock_guard<std::mutex> g(g_picked.lock);
	g_picked.files.clear();
	g_picked.error.clear();
	if (!list) g_picked.error = SDL_GetError();
	else for (; *list; list++) g_picked.files.push_back(*list);
	g_picked.done = true;
}

// copies the picked files into 'dir': zip files as they are, loose files under their set
// name; returns a report of what could not be used
std::string install(const std::vector<std::string> &files, const std::string &dir, int &installed)
{
	std::string report;
	installed = 0;
	SDL_CreateDirectory(dir.c_str());
	for (size_t i = 0; i < files.size(); i++)
	{
		size_t n = 0;
		uint8_t *d = (uint8_t *)SDL_LoadFile(files[i].c_str(), &n);
		const std::string what = files.size() > 1 ? "File " + std::to_string(i + 1) : std::string("The file");
		if (!d) { report += what + " could not be read: " + SDL_GetError() + "\n"; continue; }
		std::string name;
		if (n >= 4 && d[0] == 'P' && d[1] == 'K' && d[2] == 3 && d[3] == 4)
		{
			for (int k = 1; name.empty(); k++)            // keep zips picked earlier
			{
				const std::string cand = "picked" + std::to_string(k) + ".zip";
				if (!SDL_GetPathInfo((dir + "/" + cand).c_str(), nullptr)) name = cand;
			}
		}
		else
		{
			const uint32_t crc = rom_crc32(d, n);
			for (const RomFile &f : ROM_FILES)
				if (f.size == n && f.crc == crc) name = f.name;
		}
		if (name.empty()) report += what + " is not part of the 720 ROM set (rev 4).\n";
		else if (!SDL_SaveFile((dir + "/" + name).c_str(), d, n)) report += what + " could not be saved: " + SDL_GetError() + "\n";
		else installed++;
		SDL_free(d);
	}
	return report;
}

}

std::vector<std::string> rom_folders()
{
	std::vector<std::string> out;
#ifdef SDL_PLATFORM_ANDROID
	// the app's folder on the shared storage (reachable over USB: Android/data/<app>/files/Game),
	// then the private one the setup screen installs to
	if (SDL_GetAndroidExternalStorageState() & SDL_ANDROID_EXTERNAL_STORAGE_READ)
		if (const char *ext = SDL_GetAndroidExternalStoragePath()) out.push_back(std::string(ext) + "/Game");
	out.push_back(rom_install_folder());
#else
	// the Game folder next to the program, the program's own folder, then Game folders above
	// it (for running from the build directory)
	const char *base = SDL_GetBasePath();
	if (base)
		for (const char *rel : { "Game", "", "../Game", "../../Game", "../../../Game" })
			out.push_back(std::string(base) + rel);
	for (const char *rel : { "Game", "." }) out.push_back(rel);
#endif
	return out;
}

std::string rom_install_folder()
{
#ifdef SDL_PLATFORM_ANDROID
	return pref_dir() + "Game";
#else
	const char *base = SDL_GetBasePath();
	return std::string(base ? base : "") + "Game";
#endif
}

std::string find_roms(const std::string &requested)
{
	if (!requested.empty()) return requested;
	for (const auto &c : rom_folders())
		if (rom_folder_usable(c)) return c;         // the ROM files, or a zip holding them
	return rom_install_folder();
}

bool rom_setup_screen(SDL_Window *window, SDL_Renderer *renderer, RomSet &roms, std::string &romdir, std::string message)
{
	const std::string dir = rom_install_folder();
	const std::string shown = rom_folders().front();       // the folder the player can reach
	bool waiting = false;
	for (;;)
	{
		SDL_Event e;
		while (SDL_PollEvent(&e))
		{
			ImGui_ImplSDL3_ProcessEvent(&e);
			if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return false;
			if (e.type == SDL_EVENT_KEY_DOWN && (e.key.key == SDLK_AC_BACK || e.key.key == SDLK_ESCAPE)) return false;
		}
		{
			std::lock_guard<std::mutex> g(g_picked.lock);
			if (g_picked.done)
			{
				g_picked.done = false;
				waiting = false;
				if (!g_picked.error.empty()) message = "The file dialog failed: " + g_picked.error;
				else if (!g_picked.files.empty())
				{
					int installed = 0;
					const std::string report = install(g_picked.files, dir, installed);
					std::string err;
					if (roms.load(dir, err)) { romdir = dir; return true; }
					message = report + (installed ? "The ROM set is still incomplete:\n" : "") + err;
				}
			}
		}

		ImGui_ImplSDLRenderer3_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();
		const ImGuiViewport *vp = ImGui::GetMainViewport();
		const float pad = ui_px(16);
		ImGui::SetNextWindowPos({vp->WorkPos.x + pad, vp->WorkPos.y + pad});
		ImGui::SetNextWindowSize({vp->WorkSize.x - 2 * pad, vp->WorkSize.y - 2 * pad});
		ImGui::Begin("720 Degrees - ROM files", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
		ImGui::TextWrapped("720 Degrees needs the ROM files of the arcade game: the MAME ROM set '720' (rev 4), "
			"36 ROM files and 720-eeprom.bin.  They are not included with the game.");
		ImGui::Spacing();
		ImGui::TextWrapped("Choose MAME's 720.zip, or select all the loose files at once.  The game keeps a copy.");
		ImGui::Spacing();
		const ImVec2 big = {ui_px(220), ui_px(44)};
		ImGui::BeginDisabled(waiting);
		if (ImGui::Button(waiting ? "Waiting for the file dialog..." : "Choose ROM files...", big))
		{
			waiting = true;
			SDL_ShowOpenFileDialog(on_picked, nullptr, window, nullptr, 0, nullptr, true);
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Quit", {ui_px(120), big.y})) { ImGui::End(); ImGui::Render(); return false; }
		ImGui::Spacing();
		ImGui::TextWrapped("Or copy the files into this folder and start the game again:");
		ImGui::Indent();
		ImGui::TextWrapped("%s", shown.c_str());
		ImGui::Unindent();
		if (!message.empty())
		{
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.4f, 1.0f));
			ImGui::TextWrapped("%s", message.c_str());
			ImGui::PopStyleColor();
		}
		ImGui::End();
		ImGui::Render();
		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
		SDL_RenderClear(renderer);
		ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
		SDL_RenderPresent(renderer);
	}
}
