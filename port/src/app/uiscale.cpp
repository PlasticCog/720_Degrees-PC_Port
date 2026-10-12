// The size of the game's menus, see uiscale.h
#include "uiscale.h"
#include "imgui_internal.h"
#include "imgui_impl_sdlrenderer3.h"
#include <cmath>

namespace { float g_base = 1.0f; }

void set_ui_base_scale(float base) { g_base = base; }

void apply_ui_scale(float s, float ratio)
{
	s *= g_base;
	ImGuiIO &io = ImGui::GetIO();
	io.Fonts->Clear();
	ImFontConfig cfg;
	cfg.SizePixels = std::round(13.0f * s);
	io.Fonts->AddFontDefault(&cfg);
	ImGui_ImplSDLRenderer3_DestroyFontsTexture();       // made again at the next NewFrame

	ImGuiStyle style;
	ImGui::StyleColorsDark(&style);
	style.ScaleAllSizes(s);
	ImGui::GetStyle() = style;

	if (ratio != 1.0f)
		for (ImGuiWindow *w : GImGui->Windows)
			if (!(w->Flags & ImGuiWindowFlags_ChildWindow))
			{
				w->SizeFull = ImVec2(std::floor(w->SizeFull.x * ratio), std::floor(w->SizeFull.y * ratio));
				w->Size = w->SizeFull;
			}
}
