// Remappable controls for the game: keyboard keys and gamepad buttons/axes per action,
// gamepad stick options, and the controls window (F4) that edits them.
//
// The arcade machine has a rotating joystick and two buttons (Jump and Kick).  The port
// turns a pointed direction (keys, d-pad or a stick) into a target angle for the joystick
// and the rotate actions into continuous rotation.  Defaults target an Xbox controller.
#pragma once
#include <SDL3/SDL.h>
#include <array>
#include <string>
#include <vector>

enum class Action {
	AimUp, AimDown, AimLeft, AimRight, RotateLeft, RotateRight,
	Jump, Kick, Coin1, Coin2, Pause, Count
};
constexpr int ACTION_COUNT = (int)Action::Count;

// a gamepad input: a button, or one direction of an axis (sticks and triggers)
struct PadInput {
	enum Kind : uint8_t { None, Button, Axis } kind = None;
	int index = 0;          // SDL_GamepadButton or SDL_GamepadAxis
	int dir = 1;            // axis direction: +1 or -1
	bool operator==(const PadInput &o) const { return kind == o.kind && index == o.index && (kind != Axis || dir == o.dir); }
	static PadInput button(int b) { PadInput p; p.kind = Button; p.index = b; return p; }
	static PadInput axis(int a, int d) { PadInput p; p.kind = Axis; p.index = a; p.dir = d; return p; }
};

struct Binding {
	SDL_Scancode keys[2] = { SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN };
	PadInput pad[2];
};

struct InputConfig {
	std::array<Binding, ACTION_COUNT> bind;
	int aim_stick = 0;              // stick that points the skater: 0 left, 1 right, -1 none
	int rotate_stick = 1;           // stick whose X axis rotates the skater: 0 left, 1 right, -1 none
	float deadzone = 0.35f;         // stick deflection needed to point the skater
	float rotate_speed = 3.0f;      // degrees per frame for the rotate actions / full stick
	float mouse_sensitivity = 0.25f;
	bool mouse_enabled = true;      // captured mouse: horizontal motion rotates, buttons jump/kick
	bool invert_rotation = false;

	void set_defaults();
	bool load(const std::string &path);
	bool save(const std::string &path) const;

	static const char *action_name(Action a);
};

// state of all inputs for one emulated frame
struct InputFrame {
	bool aim = false;               // a direction is pointed: angle_deg is the target
	double angle_deg = 0;           // 0 = up, 90 = right
	double rotate = 0;              // degrees to rotate this frame (+ clockwise)
	bool jump = false, kick = false, coin1 = false, coin2 = false;
};

class Input {
public:
	InputConfig cfg;

	void open_gamepads();
	void close_gamepads();
	// returns true when the event was consumed (gamepad hotplug, or a rebinding capture)
	bool handle_event(const SDL_Event &e);
	// was this key/button event the (edge-triggered) action?
	bool is_action(const SDL_Event &e, Action a) const;
	// poll the current state; 'keys_allowed' is false while the UI has keyboard focus
	InputFrame poll(bool keys_allowed, bool mouse_captured, double mouse_dx);

	// the controls window; returns true while it is open
	void draw_window(bool *open);
	bool capturing() const { return m_capture_action >= 0; }

	std::string config_path;
	static std::string key_name(SDL_Scancode sc);
	static std::string pad_name(const PadInput &p);
	int gamepad_count() const { return (int)m_pads.size(); }

private:
	std::vector<SDL_Gamepad *> m_pads;
	int m_capture_action = -1, m_capture_slot = -1;   // slot 0-1 keys, 2-3 pad
	double pad_value(const PadInput &p) const;       // 0..1
	bool pressed(Action a, bool keys_allowed, const bool *ks) const;
	void stick(int which, double &x, double &y) const;
};
