// 720 Degrees rotary controller model.
//
// The arcade stick rotates freely; two optical discs report its orientation:
// a 72-tooth "rotate" disc counted at 2x (144 counts per turn) and a 2-tooth "center"
// disc whose counter changes when the stick passes the straight-up position.
// We keep an absolute stick position (0..143 counts, 0 = up, clockwise positive),
// move it toward a target direction (keyboard / gamepad stick) or by relative amounts
// (mouse / rotate keys), and derive both counters exactly like MAME's spinner model.
#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>

class RotaryController {
public:
	// absolute target direction in degrees (0 = up, 90 = right), or NAN for none
	void set_target_angle(double deg) { m_target = deg; }
	void clear_target() { m_target = NAN; }
	// relative rotation in counts (positive = clockwise)
	void add_rotation(double counts) { m_pending += counts; }

	// advance one video frame; 'max_speed' = counts per frame when chasing a target
	void update(double max_speed = 6.0)
	{
		if (!std::isnan(m_target))
		{
			const double target_pos = std::fmod(m_target / 360.0 * 144.0 + 144.0, 144.0);
			double diff = target_pos - std::fmod(m_pos_f + 144.0 * 1000, 144.0);
			while (diff > 72) diff -= 144;
			while (diff < -72) diff += 144;
			const double step = std::clamp(diff, -max_speed, max_speed);
			m_pending += step;
		}
		int steps = (int)std::trunc(m_pending);
		m_pending -= steps;
		while (steps > 0) { step(+1); steps--; }
		while (steps < 0) { step(-1); steps++; }
	}

	uint8_t rotate_count() const { return (uint8_t)(m_rotate & 0xff); }
	uint8_t center_count() const { return (uint8_t)(m_center & 0xff); }

private:
	double m_target = NAN;
	double m_pending = 0;
	double m_pos_f = 0;          // continuous position (counts)
	int m_pos = 0;               // 0..143
	int m_rotate = 0;            // free-running rotate counter
	int m_center = 0;            // free-running center counter

	void step(int dir)
	{
		m_rotate += dir;
		m_pos_f += dir;
		if (dir > 0)
		{
			if (++m_pos > 143) m_pos = 0;
			else if (m_pos == 2 || m_pos == 3 || m_pos == 141 || m_pos == 142) m_center++;
		}
		else
		{
			if (--m_pos < 0) m_pos = 143;
			else if (m_pos == 2 || m_pos == 3 || m_pos == 141 || m_pos == 142) m_center--;
		}
	}
};
