// On-screen touch controls for phones and tablets (on by default on Android, --touch elsewhere).
//
// A floating stick on the left half of the screen points the skater (it appears where the
// thumb lands), JUMP and KICK sit at the bottom right, and small buttons along the top insert
// a coin, start one or two players, pause and open the menu.  They are drawn in the bars beside
// the 4:3 game picture when there is room, over the picture otherwise, and fade away while a
// gamepad or the keyboard is used.  Touches on the menus' windows are passed to Dear ImGui.
#pragma once
#include <SDL3/SDL.h>
#include "imgui.h"
#include <cstdint>

struct InputFrame;

class TouchControls {
public:
	enum Button { Jump, Kick, Coin, Start1, Start2, Pause, Menu, ButtonCount };

	bool enabled = false;

	// places the controls for this frame: the display size (ImGui units), the safe area free of
	// notches and system bars, the game picture, and the size of one density-independent pixel
	void layout(ImVec2 display, ImVec4 safe, ImVec4 game, float dp);
	// true when the event was a touch the controls or the menus used
	bool handle_event(const SDL_Event &e);
	// adds the held controls to the frame's inputs
	void apply(InputFrame &f, bool invert) const;
	// the Pause and Menu buttons: true once per tap
	bool take_tap(Button b);
	void draw(ImDrawList *dl) const;

private:
	struct Spot { ImVec2 c; ImVec2 half; bool round; const char *label; };
	Spot m_spot[ButtonCount] = {};
	ImVec2 m_display = {1, 1};
	float m_dp = 1, m_stick_r = 60, m_font = 13;
	ImVec2 m_stick_home = {0, 0};

	struct Hold { bool on = false; SDL_FingerID id = 0; };   // finger ids start at 0 on Android
	Hold m_hold[ButtonCount];
	uint64_t m_pressed_at[ButtonCount] = {};      // a quick tap still counts for a few frames
	bool m_tapped[ButtonCount] = {};
	Hold m_stick;
	ImVec2 m_stick_origin = {0, 0}, m_stick_pos = {0, 0};
	Hold m_ui;                                     // the finger working the menus
	bool m_shown = true;                           // false while a gamepad or keyboard is in use

	bool held(Button b) const;
	int hit(ImVec2 p) const;
};
