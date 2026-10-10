// Remappable controls, see input_config.h
#include "input_config.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

constexpr double PI = 3.14159265358979323846;

const char *ACTION_NAMES[ACTION_COUNT] = {
	"Point up", "Point down", "Point left", "Point right", "Rotate left", "Rotate right",
	"Jump", "Kick", "Coin (left)", "Coin (right)", "Pause",
};
const char *ACTION_KEYS[ACTION_COUNT] = {     // names used in the settings file
	"aim_up", "aim_down", "aim_left", "aim_right", "rotate_left", "rotate_right",
	"jump", "kick", "coin1", "coin2", "pause",
};

std::string trim(std::string s)
{
	while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
	size_t i = 0;
	while (i < s.size() && isspace((unsigned char)s[i])) i++;
	return s.substr(i);
}

std::string pad_to_string(const PadInput &p)
{
	if (p.kind == PadInput::Button)
		if (const char *s = SDL_GetGamepadStringForButton((SDL_GamepadButton)p.index)) return s;
	if (p.kind == PadInput::Axis)
		if (const char *s = SDL_GetGamepadStringForAxis((SDL_GamepadAxis)p.index)) return std::string(p.dir > 0 ? "+" : "-") + s;
	return "";
}

PadInput pad_from_string(const std::string &s)
{
	if (s.empty()) return {};
	if (s[0] == '+' || s[0] == '-')
	{
		const SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(s.c_str() + 1);
		if (a != SDL_GAMEPAD_AXIS_INVALID) return PadInput::axis(a, s[0] == '+' ? 1 : -1);
		return {};
	}
	const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(s.c_str());
	if (b != SDL_GAMEPAD_BUTTON_INVALID) return PadInput::button(b);
	return {};
}

}

// ------------------------------------------------------------------ configuration

const char *InputConfig::action_name(Action a) { return ACTION_NAMES[(int)a]; }

void InputConfig::set_defaults()
{
	*this = InputConfig();
	auto set = [&](Action a, SDL_Scancode k1, SDL_Scancode k2, PadInput p1, PadInput p2 = {}) {
		Binding &b = bind[(int)a];
		b.keys[0] = k1; b.keys[1] = k2; b.pad[0] = p1; b.pad[1] = p2;
	};
	using P = PadInput;
	set(Action::AimUp, SDL_SCANCODE_UP, SDL_SCANCODE_W, P::button(SDL_GAMEPAD_BUTTON_DPAD_UP));
	set(Action::AimDown, SDL_SCANCODE_DOWN, SDL_SCANCODE_S, P::button(SDL_GAMEPAD_BUTTON_DPAD_DOWN));
	set(Action::AimLeft, SDL_SCANCODE_LEFT, SDL_SCANCODE_A, P::button(SDL_GAMEPAD_BUTTON_DPAD_LEFT));
	set(Action::AimRight, SDL_SCANCODE_RIGHT, SDL_SCANCODE_D, P::button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
	set(Action::RotateLeft, SDL_SCANCODE_Q, SDL_SCANCODE_UNKNOWN, P::button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
	set(Action::RotateRight, SDL_SCANCODE_E, SDL_SCANCODE_UNKNOWN, P::button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
	set(Action::Jump, SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL, P::button(SDL_GAMEPAD_BUTTON_SOUTH), P::axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1));
	set(Action::Kick, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT, P::button(SDL_GAMEPAD_BUTTON_WEST), P::button(SDL_GAMEPAD_BUTTON_EAST));
	set(Action::Coin1, SDL_SCANCODE_5, SDL_SCANCODE_UNKNOWN, P::button(SDL_GAMEPAD_BUTTON_BACK));
	set(Action::Coin2, SDL_SCANCODE_6, SDL_SCANCODE_UNKNOWN, {});
	set(Action::Pause, SDL_SCANCODE_P, SDL_SCANCODE_UNKNOWN, P::button(SDL_GAMEPAD_BUTTON_START));
}

bool InputConfig::save(const std::string &path) const
{
	std::ofstream f(path);
	if (!f) return false;
	f << "# 720 Degrees controls (edit in the game with F4)\n";
	f << "aim_stick=" << aim_stick << "\nrotate_stick=" << rotate_stick << "\ndeadzone=" << deadzone
	  << "\nrotate_speed=" << rotate_speed << "\nmouse_sensitivity=" << mouse_sensitivity
	  << "\nmouse=" << (mouse_enabled ? 1 : 0) << "\ninvert_rotation=" << (invert_rotation ? 1 : 0) << "\n";
	for (int i = 0; i < ACTION_COUNT; i++)
	{
		const Binding &b = bind[i];
		f << ACTION_KEYS[i] << "=";
		for (int k = 0; k < 2; k++)
			f << (b.keys[k] != SDL_SCANCODE_UNKNOWN ? SDL_GetScancodeName(b.keys[k]) : "") << "|";
		f << pad_to_string(b.pad[0]) << "|" << pad_to_string(b.pad[1]) << "\n";
	}
	return (bool)f;
}

bool InputConfig::load(const std::string &path)
{
	std::ifstream f(path);
	if (!f) return false;
	set_defaults();
	std::string line;
	while (std::getline(f, line))
	{
		line = trim(line);
		if (line.empty() || line[0] == '#') continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		const std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
		if (key == "aim_stick") aim_stick = std::clamp(atoi(val.c_str()), -1, 1);
		else if (key == "rotate_stick") rotate_stick = std::clamp(atoi(val.c_str()), -1, 1);
		else if (key == "deadzone") deadzone = std::clamp((float)atof(val.c_str()), 0.05f, 0.95f);
		else if (key == "rotate_speed") rotate_speed = std::clamp((float)atof(val.c_str()), 0.5f, 12.0f);
		else if (key == "mouse_sensitivity") mouse_sensitivity = std::clamp((float)atof(val.c_str()), 0.02f, 2.0f);
		else if (key == "mouse") mouse_enabled = atoi(val.c_str()) != 0;
		else if (key == "invert_rotation") invert_rotation = atoi(val.c_str()) != 0;
		else
			for (int i = 0; i < ACTION_COUNT; i++)
				if (key == ACTION_KEYS[i])
				{
					std::vector<std::string> parts;
					std::stringstream ss(val);
					std::string part;
					while (std::getline(ss, part, '|')) parts.push_back(trim(part));
					parts.resize(4);
					Binding b;
					for (int k = 0; k < 2; k++)
						b.keys[k] = parts[k].empty() ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(parts[k].c_str());
					b.pad[0] = pad_from_string(parts[2]);
					b.pad[1] = pad_from_string(parts[3]);
					bind[i] = b;
				}
	}
	return true;
}

// ------------------------------------------------------------------ devices and polling

void Input::open_gamepads()
{
	int n = 0;
	SDL_JoystickID *ids = SDL_GetGamepads(&n);
	for (int i = 0; ids && i < n; i++)
		if (SDL_Gamepad *g = SDL_OpenGamepad(ids[i])) m_pads.push_back(g);
	SDL_free(ids);
}

void Input::close_gamepads()
{
	for (auto *g : m_pads) SDL_CloseGamepad(g);
	m_pads.clear();
}

double Input::pad_value(const PadInput &p) const
{
	double best = 0;
	for (auto *g : m_pads)
	{
		if (p.kind == PadInput::Button)
			best = std::max(best, SDL_GetGamepadButton(g, (SDL_GamepadButton)p.index) ? 1.0 : 0.0);
		else if (p.kind == PadInput::Axis)
			best = std::max(best, std::clamp(SDL_GetGamepadAxis(g, (SDL_GamepadAxis)p.index) / 32767.0 * p.dir, 0.0, 1.0));
	}
	return best;
}

bool Input::pressed(Action a, bool keys_allowed, const bool *ks) const
{
	const Binding &b = cfg.bind[(int)a];
	if (keys_allowed)
		for (auto k : b.keys)
			if (k != SDL_SCANCODE_UNKNOWN && ks[k]) return true;
	for (const auto &p : b.pad)
		if (pad_value(p) > 0.5) return true;
	return false;
}

void Input::stick(int which, double &x, double &y) const
{
	x = y = 0;
	if (which < 0) return;
	const SDL_GamepadAxis ax = which ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX;
	const SDL_GamepadAxis ay = which ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY;
	double best = -1;
	for (auto *g : m_pads)
	{
		const double gx = SDL_GetGamepadAxis(g, ax) / 32767.0, gy = SDL_GetGamepadAxis(g, ay) / 32767.0;
		if (gx * gx + gy * gy > best) { best = gx * gx + gy * gy; x = gx; y = gy; }
	}
}

InputFrame Input::poll(bool keys_allowed, bool mouse_captured, double mouse_dx)
{
	InputFrame f;
	const bool *ks = SDL_GetKeyboardState(nullptr);
	auto on = [&](Action a) { return pressed(a, keys_allowed, ks); };
	const double sgn = cfg.invert_rotation ? -1.0 : 1.0;

	const int dx = on(Action::AimRight) - on(Action::AimLeft);
	const int dy = on(Action::AimDown) - on(Action::AimUp);
	if (dx || dy)
	{
		f.aim = true;
		f.angle_deg = std::atan2((double)dx, (double)-dy) * 180.0 / PI;
	}
	else
	{
		double sx, sy;
		stick(cfg.aim_stick, sx, sy);
		if (sx * sx + sy * sy > cfg.deadzone * cfg.deadzone)
		{
			f.aim = true;
			f.angle_deg = std::atan2(sx, -sy) * 180.0 / PI;
		}
	}
	f.angle_deg *= sgn;

	f.rotate = (on(Action::RotateRight) - on(Action::RotateLeft)) * cfg.rotate_speed;
	if (cfg.rotate_stick >= 0 && cfg.rotate_stick != cfg.aim_stick)
	{
		double sx, sy;
		stick(cfg.rotate_stick, sx, sy);
		if (std::fabs(sx) > 0.25) f.rotate += (sx - (sx > 0 ? 0.25 : -0.25)) / 0.75 * cfg.rotate_speed * 1.5;
	}
	if (mouse_captured && cfg.mouse_enabled) f.rotate += mouse_dx * cfg.mouse_sensitivity;
	f.rotate *= sgn;

	f.jump = on(Action::Jump);
	f.kick = on(Action::Kick);
	f.coin1 = on(Action::Coin1);
	f.coin2 = on(Action::Coin2);
	if (mouse_captured && cfg.mouse_enabled)
	{
		const SDL_MouseButtonFlags mb = SDL_GetMouseState(nullptr, nullptr);
		f.jump |= (mb & SDL_BUTTON_LMASK) != 0;
		f.kick |= (mb & SDL_BUTTON_RMASK) != 0;
	}
	return f;
}

bool Input::is_action(const SDL_Event &e, Action a) const
{
	const Binding &b = cfg.bind[(int)a];
	if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat)
		return (b.keys[0] != SDL_SCANCODE_UNKNOWN && e.key.scancode == b.keys[0]) ||
		       (b.keys[1] != SDL_SCANCODE_UNKNOWN && e.key.scancode == b.keys[1]);
	if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
		for (const auto &p : b.pad)
			if (p.kind == PadInput::Button && p.index == e.gbutton.button) return true;
	return false;
}

bool Input::handle_event(const SDL_Event &e)
{
	if (e.type == SDL_EVENT_GAMEPAD_ADDED)
	{
		for (auto *g : m_pads) if (SDL_GetGamepadID(g) == e.gdevice.which) return true;
		if (SDL_Gamepad *g = SDL_OpenGamepad(e.gdevice.which)) m_pads.push_back(g);
		return true;
	}
	if (e.type == SDL_EVENT_GAMEPAD_REMOVED)
	{
		for (size_t i = 0; i < m_pads.size(); i++)
			if (SDL_GetGamepadID(m_pads[i]) == e.gdevice.which) { SDL_CloseGamepad(m_pads[i]); m_pads.erase(m_pads.begin() + i); break; }
		return true;
	}
	if (m_capture_action < 0) return false;

	// rebinding: the next key (slots 0-1) or gamepad input (slots 2-3) becomes the binding
	Binding &b = cfg.bind[m_capture_action];
	auto done = [&]() { m_capture_action = m_capture_slot = -1; if (!config_path.empty()) cfg.save(config_path); };
	if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat)
	{
		if (e.key.scancode == SDL_SCANCODE_ESCAPE) { m_capture_action = m_capture_slot = -1; return true; }
		if (e.key.scancode == SDL_SCANCODE_BACKSPACE || e.key.scancode == SDL_SCANCODE_DELETE)
		{
			if (m_capture_slot < 2) b.keys[m_capture_slot] = SDL_SCANCODE_UNKNOWN;
			else b.pad[m_capture_slot - 2] = {};
			done();
			return true;
		}
		if (m_capture_slot < 2)
		{
			for (auto &o : cfg.bind)        // a key belongs to one action only
				for (auto &k : o.keys) if (k == e.key.scancode) k = SDL_SCANCODE_UNKNOWN;
			b.keys[m_capture_slot] = e.key.scancode;
			done();
		}
		return true;
	}
	if (m_capture_slot >= 2)
	{
		PadInput p;
		if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) p = PadInput::button(e.gbutton.button);
		else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && std::abs((int)e.gaxis.value) > 24000)
			p = PadInput::axis(e.gaxis.axis, e.gaxis.value > 0 ? 1 : -1);
		if (p.kind != PadInput::None)
		{
			for (auto &o : cfg.bind)
				for (auto &q : o.pad) if (q == p) q = {};
			b.pad[m_capture_slot - 2] = p;
			done();
			return true;
		}
	}
	return e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
}

// ------------------------------------------------------------------ names

std::string Input::key_name(SDL_Scancode sc)
{
	if (sc == SDL_SCANCODE_UNKNOWN) return "-";
	const char *n = SDL_GetScancodeName(sc);
	return n && *n ? n : "?";
}

std::string Input::pad_name(const PadInput &p)
{
	if (p.kind == PadInput::None) return "-";
	if (p.kind == PadInput::Button)
		switch (p.index)
		{
		case SDL_GAMEPAD_BUTTON_SOUTH: return "A";
		case SDL_GAMEPAD_BUTTON_EAST: return "B";
		case SDL_GAMEPAD_BUTTON_WEST: return "X";
		case SDL_GAMEPAD_BUTTON_NORTH: return "Y";
		case SDL_GAMEPAD_BUTTON_BACK: return "View";
		case SDL_GAMEPAD_BUTTON_GUIDE: return "Xbox";
		case SDL_GAMEPAD_BUTTON_START: return "Menu";
		case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "LS click";
		case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "RS click";
		case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "LB";
		case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RB";
		case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-pad up";
		case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-pad down";
		case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-pad left";
		case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-pad right";
		case SDL_GAMEPAD_BUTTON_MISC1: return "Share";
		default: return "Button " + std::to_string(p.index);
		}
	switch (p.index)
	{
	case SDL_GAMEPAD_AXIS_LEFTX: return p.dir > 0 ? "Left stick right" : "Left stick left";
	case SDL_GAMEPAD_AXIS_LEFTY: return p.dir > 0 ? "Left stick down" : "Left stick up";
	case SDL_GAMEPAD_AXIS_RIGHTX: return p.dir > 0 ? "Right stick right" : "Right stick left";
	case SDL_GAMEPAD_AXIS_RIGHTY: return p.dir > 0 ? "Right stick down" : "Right stick up";
	case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "LT";
	case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "RT";
	default: return "Axis " + std::to_string(p.index);
	}
}

// ------------------------------------------------------------------ controls window

void Input::draw_window(bool *open)
{
	ImGui::SetNextWindowSize({620, 520}, ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Controls", open)) { ImGui::End(); return; }
	bool changed = false;
	if (m_capture_action >= 0)
		ImGui::TextColored({1, 0.85f, 0.2f, 1}, "%s: press a %s (Esc cancels, Backspace clears)",
			InputConfig::action_name((Action)m_capture_action), m_capture_slot < 2 ? "key" : "gamepad button, trigger or stick direction");
	else
		ImGui::TextDisabled("Click a binding, then press the new key or gamepad input.");

	if (ImGui::BeginTable("bindings", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 110);
		ImGui::TableSetupColumn("Key");
		ImGui::TableSetupColumn("Key 2");
		ImGui::TableSetupColumn("Gamepad");
		ImGui::TableSetupColumn("Gamepad 2");
		ImGui::TableHeadersRow();
		for (int i = 0; i < ACTION_COUNT; i++)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(InputConfig::action_name((Action)i));
			for (int slot = 0; slot < 4; slot++)
			{
				ImGui::TableNextColumn();
				ImGui::PushID(i * 4 + slot);
				const bool active = m_capture_action == i && m_capture_slot == slot;
				std::string label = active ? "..." : slot < 2 ? key_name(cfg.bind[i].keys[slot]) : pad_name(cfg.bind[i].pad[slot - 2]);
				if (active) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 150, 0, 255));
				if (ImGui::Button(label.c_str(), {-1, 0})) { m_capture_action = i; m_capture_slot = slot; }
				if (active) ImGui::PopStyleColor();
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
	}
	ImGui::Separator();
	const char *sticks[] = { "Off", "Left stick", "Right stick" };
	int aim = cfg.aim_stick + 1, rot = cfg.rotate_stick + 1;
	ImGui::SetNextItemWidth(140);
	if (ImGui::Combo("Stick that points the skater", &aim, sticks, 3)) { cfg.aim_stick = aim - 1; changed = true; }
	ImGui::SetNextItemWidth(140);
	if (ImGui::Combo("Stick that rotates (X axis)", &rot, sticks, 3)) { cfg.rotate_stick = rot - 1; changed = true; }
	ImGui::SetNextItemWidth(220);
	changed |= ImGui::SliderFloat("Stick deadzone", &cfg.deadzone, 0.1f, 0.9f, "%.2f");
	ImGui::SetNextItemWidth(220);
	changed |= ImGui::SliderFloat("Rotate speed (degrees/frame)", &cfg.rotate_speed, 0.5f, 10.0f, "%.1f");
	changed |= ImGui::Checkbox("Mouse: click the game to capture it; moving rotates, left = jump, right = kick", &cfg.mouse_enabled);
	ImGui::SetNextItemWidth(220);
	changed |= ImGui::SliderFloat("Mouse sensitivity", &cfg.mouse_sensitivity, 0.05f, 1.5f, "%.2f");
	changed |= ImGui::Checkbox("Invert rotation", &cfg.invert_rotation);
	if (ImGui::Button("Reset to defaults")) { cfg.set_defaults(); changed = true; }
	ImGui::SameLine();
	if (m_pads.empty()) ImGui::TextDisabled("No gamepad connected.");
	else
	{
		std::string names;
		for (auto *g : m_pads) { const char *n = SDL_GetGamepadName(g); names += (names.empty() ? "" : ", ") + std::string(n ? n : "gamepad"); }
		ImGui::TextDisabled("Connected: %s", names.c_str());
	}
	if (changed && !config_path.empty()) cfg.save(config_path);
	ImGui::End();
}
