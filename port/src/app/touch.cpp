// On-screen touch controls, see touch.h
#include "touch.h"
#include "input_config.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cfloat>
#include <cmath>

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr uint64_t TAP_MS = 100;          // a tap shorter than a frame still presses for this long

float len(ImVec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// is this point on one of the menus' windows (or is a popup open that a tap outside closes)?
bool over_ui(ImVec2 p)
{
	ImGuiContext *g = ImGui::GetCurrentContext();
	if (!g) return false;
	if (g->OpenPopupStack.Size > 0) return true;
	for (ImGuiWindow *w : g->Windows)
		if (w->Active && !w->Hidden && !(w->Flags & (ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_ChildWindow)) && w->Rect().Contains(p))
			return true;
	return false;
}

}

void TouchControls::layout(ImVec2 display, ImVec4 safe, ImVec4 game, float dp)
{
	m_display = display;
	m_dp = dp;
	const float sx = safe.x, sy = safe.y, sw = safe.z, sh = safe.w;
	// sizes: a finger's width, and never tiny on a large screen
	const float margin = std::max(12 * dp, sh * 0.012f);
	const float rb = std::clamp(42 * dp, sh * 0.09f, sh * 0.14f);              // JUMP / KICK
	const float hs = std::clamp(34 * dp, sh * 0.055f, sh * 0.09f), ws = hs * 2.3f;   // the small buttons
	m_font = hs * 0.5f;
	m_stick_r = std::clamp(64 * dp, sh * 0.13f, sh * 0.2f);

	// JUMP and KICK: at the bottom right, in the middle of the bar beside the picture if it is wide
	const float right = sx + sw, bottom = sy + sh;
	const float bar = std::min(game.x - sx, right - (game.x + game.z));
	const float jump_x = bar >= rb * 4.4f + 2 * margin ? right - bar / 2 + rb * 1.05f : right - margin - rb;
	// KICK up and to the left of JUMP, steeper where the bar is narrow so it stays off the picture
	const float room = jump_x - (game.x + game.z);
	const float kdx = std::clamp(room - rb, rb * 1.25f, rb * 2.15f), kdy = std::sqrt(rb * rb * 2.2f * 2.2f - kdx * kdx);
	m_spot[Jump] = { {jump_x, bottom - margin - rb}, {rb, rb}, true, "JUMP" };
	m_spot[Kick] = { {jump_x - kdx, bottom - margin - rb - kdy}, {rb, rb}, true, "KICK" };

	// small buttons: in a column in the bar beside the picture when it is wide enough, else in
	// a row along the top of the screen
	const bool column = bar >= ws + 2 * margin;
	const char *left_labels[2] = { "MENU", "PAUSE" };
	const Button left_buttons[2] = { Menu, Pause };
	const char *right_labels[3] = { "COIN", "1P", "2P" };
	const Button right_buttons[3] = { Coin, Start1, Start2 };
	for (int i = 0; i < 2; i++)
	{
		const ImVec2 c = column ? ImVec2(sx + bar / 2, sy + margin + hs / 2 + i * (hs + margin))
		                        : ImVec2(sx + margin + ws / 2 + i * (ws + margin), sy + margin + hs / 2);
		m_spot[left_buttons[i]] = { c, {ws / 2, hs / 2}, false, left_labels[i] };
	}
	for (int i = 0; i < 3; i++)
	{
		const ImVec2 c = column ? ImVec2(right - bar / 2, sy + margin + hs / 2 + i * (hs + margin))
		                        : ImVec2(right - margin - ws / 2 - (2 - i) * (ws + margin), sy + margin + hs / 2);
		m_spot[right_buttons[i]] = { c, {ws / 2, hs / 2}, false, right_labels[i] };
	}
	m_stick_home = { sx + std::max(margin + m_stick_r, std::min(bar / 2, sw * 0.2f)), sy + sh * 0.62f };
}

int TouchControls::hit(ImVec2 p) const
{
	const float slack = 6 * m_dp;
	for (int i = 0; i < ButtonCount; i++)
	{
		const Spot &s = m_spot[i];
		const ImVec2 d = { p.x - s.c.x, p.y - s.c.y };
		if (s.round ? len(d) <= s.half.x + slack : std::fabs(d.x) <= s.half.x + slack && std::fabs(d.y) <= s.half.y + slack)
			return i;
	}
	return -1;
}

bool TouchControls::handle_event(const SDL_Event &e)
{
	if (!enabled) return false;
	switch (e.type)
	{
	case SDL_EVENT_KEY_DOWN:
		if (e.key.key != SDLK_AC_BACK) m_shown = false;
		return false;
	case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		m_shown = false;
		return false;
	case SDL_EVENT_GAMEPAD_AXIS_MOTION:
		if (std::abs(e.gaxis.value) > 16000) m_shown = false;
		return false;
	case SDL_EVENT_FINGER_DOWN:
	case SDL_EVENT_FINGER_MOTION:
	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED:
		break;
	default:
		return false;
	}

	const ImVec2 p = { e.tfinger.x * m_display.x, e.tfinger.y * m_display.y };
	const SDL_FingerID id = e.tfinger.fingerID;
	// a desktop mouse standing in for a finger (--touch) reaches the menus by itself
	const bool from_mouse = e.tfinger.touchID == SDL_MOUSE_TOUCHID;
	ImGuiIO &io = ImGui::GetIO();

	if (e.type == SDL_EVENT_FINGER_DOWN)
	{
		if (over_ui(p))
		{
			if (!m_ui.on && !from_mouse)
			{
				m_ui = { true, id };
				io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
				io.AddMousePosEvent(p.x, p.y);
				io.AddMouseButtonEvent(0, true);
			}
			return true;
		}
		m_shown = true;
		const int b = hit(p);
		if (b >= 0)
		{
			if (!m_hold[b].on)
			{
				m_hold[b] = { true, id };
				m_pressed_at[b] = SDL_GetTicks();
				m_tapped[b] = true;
			}
		}
		else if (p.x < m_display.x * 0.5f && !m_stick.on)
		{
			m_stick = { true, id };
			m_stick_origin = m_stick_pos = p;
		}
		return true;
	}

	if (e.type == SDL_EVENT_FINGER_MOTION)
	{
		if (m_ui.on && m_ui.id == id) { if (!from_mouse) io.AddMousePosEvent(p.x, p.y); return true; }
		if (m_stick.on && m_stick.id == id)
		{
			m_stick_pos = p;
			// the stick follows a thumb that slides past its edge, so turning back responds at once
			const ImVec2 d = { p.x - m_stick_origin.x, p.y - m_stick_origin.y };
			const float l = len(d);
			if (l > m_stick_r)
				m_stick_origin = { p.x - d.x / l * m_stick_r, p.y - d.y / l * m_stick_r };
			return true;
		}
		for (const Hold &h : m_hold)
			if (h.on && h.id == id) return true;
		return false;
	}

	// finger up / canceled
	bool used = false;
	if (m_ui.on && m_ui.id == id)
	{
		m_ui.on = false;
		if (!from_mouse)
		{
			io.AddMousePosEvent(p.x, p.y);
			io.AddMouseButtonEvent(0, false);
			io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);      // no hover left behind by a lifted finger
		}
		used = true;
	}
	if (m_stick.on && m_stick.id == id) { m_stick.on = false; used = true; }
	for (Hold &h : m_hold)
		if (h.on && h.id == id) { h.on = false; used = true; }
	return used;
}

bool TouchControls::held(Button b) const
{
	return m_hold[b].on || (m_pressed_at[b] && SDL_GetTicks() - m_pressed_at[b] < TAP_MS);
}

void TouchControls::apply(InputFrame &f, bool invert) const
{
	if (!enabled) return;
	if (m_stick.on)
	{
		const double dx = m_stick_pos.x - m_stick_origin.x, dy = m_stick_pos.y - m_stick_origin.y;
		if (dx * dx + dy * dy > (0.25 * m_stick_r) * (0.25 * m_stick_r))
		{
			f.aim = true;
			f.angle_deg = std::atan2(dx, -dy) * 180.0 / PI * (invert ? -1.0 : 1.0);
		}
	}
	f.jump |= held(Jump);
	f.kick |= held(Kick);
	f.coin1 |= held(Coin);
	f.start1 |= held(Start1);
	f.start2 |= held(Start2);
}

bool TouchControls::take_tap(Button b)
{
	const bool t = m_tapped[b];
	m_tapped[b] = false;
	return t;
}

void TouchControls::draw(ImDrawList *dl) const
{
	if (!enabled || !m_shown) return;
	ImFont *font = ImGui::GetFont();
	// dark and see-through, so the labels read over the game picture as well as over black
	const ImU32 line = IM_COL32(255, 255, 255, 120), fill = IM_COL32(0, 0, 0, 90), down = IM_COL32(255, 255, 255, 110);
	const ImU32 text = IM_COL32(255, 255, 255, 210);
	const float thick = std::max(1.5f, 2 * m_dp);
	auto label = [&](ImVec2 c, const char *s, float size) {
		const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, s);
		dl->AddText(font, size, {c.x - ts.x / 2, c.y - ts.y / 2}, text, s);
	};
	for (int i = 0; i < ButtonCount; i++)
	{
		const Spot &s = m_spot[i];
		const bool on = held((Button)i);
		if (s.round)
		{
			dl->AddCircleFilled(s.c, s.half.x, on ? down : fill, 48);
			dl->AddCircle(s.c, s.half.x, line, 48, thick);
			label(s.c, s.label, s.half.x * 0.5f);
		}
		else
		{
			const ImVec2 a = {s.c.x - s.half.x, s.c.y - s.half.y}, b = {s.c.x + s.half.x, s.c.y + s.half.y};
			dl->AddRectFilled(a, b, on ? down : fill, s.half.y);
			dl->AddRect(a, b, line, s.half.y, 0, thick);
			label(s.c, s.label, m_font);
		}
	}
	if (m_stick.on)
	{
		ImVec2 d = { m_stick_pos.x - m_stick_origin.x, m_stick_pos.y - m_stick_origin.y };
		const float l = len(d);
		if (l > m_stick_r) d = { d.x / l * m_stick_r, d.y / l * m_stick_r };
		dl->AddCircleFilled(m_stick_origin, m_stick_r, fill, 48);
		dl->AddCircle(m_stick_origin, m_stick_r, line, 48, thick);
		dl->AddCircleFilled({m_stick_origin.x + d.x, m_stick_origin.y + d.y}, m_stick_r * 0.42f, down, 32);
	}
	else
	{
		dl->AddCircle(m_stick_home, m_stick_r, IM_COL32(255, 255, 255, 50), 48, thick);
		dl->AddCircleFilled(m_stick_home, m_stick_r * 0.42f, IM_COL32(255, 255, 255, 40), 32);
		label({m_stick_home.x, m_stick_home.y + m_stick_r + m_font}, "STEER", m_font);
	}
}
