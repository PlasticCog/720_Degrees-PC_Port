// TMS5220C speech synthesizer as wired on Atari System 2 (/RS tied high, data written
// through a latch with /WS strobes).  Interface + status only for now; LPC synthesis is
// filled in by tms5220.cpp.
#pragma once
#include <cstdint>
#include <vector>

class Tms5220 {
public:
	Tms5220();
	void reset();
	void set_clock(double hz) { m_clock = hz; }
	void data_w(uint8_t data);
	void wsq_w(int state);                 // /WS line: falling edge writes the latched byte
	bool readyq() const;                   // /READY (true = not ready)
	void advance(int sound_cycles);        // time passes (sound CPU cycles)
	double sample_at_rate(double rate);    // next output sample, -1..1

private:
	double m_clock = 625000.0;
	uint8_t m_latch = 0;
	int m_wsq = 1;
	struct Impl;
	Impl *m;
	void write_data(uint8_t data);
public:
	~Tms5220();
};
