// Scripted / random input playback, used by the headless tools and the verification
// harness so the reference model and the native game can be fed identical inputs.
#pragma once
#include "board.h"
#include "controls.h"
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdlib>

struct InputScript {
	struct Event { int frame; std::string what, value; };
	std::vector<Event> events;
	size_t pos = 0;
	int random_seed = 0;
	int rotation = 0;
	RotaryController rotary;
	bool use_rotary = false;

	bool load(const std::string &path)
	{
		std::ifstream f(path);
		if (!f) return false;
		std::string line;
		while (std::getline(f, line))
		{
			if (line.empty() || line[0] == '#') continue;
			std::istringstream ss(line);
			Event e;
			ss >> e.frame >> e.what >> e.value;
			events.push_back(e);
		}
		return true;
	}

	// apply inputs for frame f; 'snapshot' receives a name if a screenshot was requested
	void apply(int f, Inputs &in, std::string &snapshot)
	{
		while (pos < events.size() && events[pos].frame <= f)
		{
			const Event &e = events[pos++];
			const int v = atoi(e.value.c_str());
			if (e.what == "coin1") in.coin1 = v;
			else if (e.what == "coin2") in.coin2 = v;
			else if (e.what == "b1") in.button1 = v;
			else if (e.what == "b2") in.button2 = v;
			else if (e.what == "test") in.test = v;
			else if (e.what == "rotate") { rotation += v; in.leta_rotate = rotation & 0xff; }
			else if (e.what == "spin") { use_rotary = true; rotary.add_rotation(v); }        // gradual, counts
			else if (e.what == "aim") { use_rotary = true; if (e.value == "none") rotary.clear_target(); else rotary.set_target_angle(v); }
			else if (e.what == "png") snapshot = e.value;
		}
		if (use_rotary)
		{
			rotary.update();
			in.leta_rotate = rotary.rotate_count();
			in.leta_center = rotary.center_count();
		}
		if (random_seed)
		{
			// crude "monkey" player: coin every ~40 s, random rotation and buttons
			auto rnd = [&]() { m_seed = m_seed * 1664525u + 1013904223u; return m_seed >> 16; };
			if (!m_seeded) { m_seed = (uint32_t)random_seed; m_seeded = true; }
			in.coin1 = (f % 2400) >= 100 && (f % 2400) < 106;
			if (m_rot_hold-- <= 0) { m_rot_dir = (int)(rnd() % 5) - 2; m_rot_hold = 5 + rnd() % 60; }
			rotation += m_rot_dir;
			in.leta_rotate = rotation & 0xff;
			if (m_b1 > 0) m_b1--; else if (rnd() % 40 == 0) m_b1 = 3 + rnd() % 20;
			if (m_b2 > 0) m_b2--; else if (rnd() % 50 == 0) m_b2 = 3 + rnd() % 30;
			in.button1 = m_b1 > 0;
			in.button2 = m_b2 > 0;
		}
	}

private:
	uint32_t m_seed = 0;
	bool m_seeded = false;
	int m_rot_dir = 0, m_rot_hold = 0, m_b1 = 0, m_b2 = 0;
};
