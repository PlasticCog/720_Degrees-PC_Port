// The size of the game's menus (debug menu, controls, help): 100 % - 250 %, chosen in the debug
// menu and saved in ui.cfg.  The font is built at the chosen size (crisp text, not stretched)
// and the style's spacing scaled with it; ui_px() scales the menus' fixed widths the same way.
#pragma once
#include "imgui.h"

inline float ui_px(float v) { return v * ImGui::GetFontSize() / 13.0f; }

// build the fonts and the style for scale s (call outside NewFrame / Render); the open windows
// grow or shrink by ratio (the new scale over the old one)
void apply_ui_scale(float s, float ratio = 1.0f);
// a factor under the chosen size: 1 on computers, larger on phones and tablets so 100 % is
// comfortable to touch (call before the first apply_ui_scale)
void set_ui_base_scale(float base);
