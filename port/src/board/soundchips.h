// Sound hardware of the System 2 audio board: 2x POKEY, YM2151, TMS5220C and the
// mixer/control latches.  Time base: sound CPU cycles (14.318181 MHz / 8).
// POKEY behaviour follows MAME's pokey.cpp (BSD-3-Clause); the YM2151 uses ymfm (BSD-3-Clause).
#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include <functional>

class Pokey {
public:
	Pokey();
	void reset();
	void step();                          // one 1.79 MHz clock
	uint8_t read(int offset, uint8_t allpot_input);
	void write(int offset, uint8_t data);
	int output() const;                   // 0..60 (sum of 4 channel volumes)

private:
	struct Channel {
		uint8_t audf = 0, audc = 0xb0;
		int32_t borrow_cnt = 0, counter = 0;
		uint8_t output = 0, filter_sample = 0;
		void sample() { filter_sample = output; }
		void reset_channel() { counter = audf ^ 0xff; borrow_cnt = 0; }
		void inc_chan(int cycles) { counter = (counter + 1) & 0xff; if (counter == 0 && borrow_cnt == 0) borrow_cnt = cycles; }
		int check_borrow() { if (borrow_cnt > 0) { borrow_cnt--; return borrow_cnt == 0; } return 0; }
	};
	Channel m_ch[4];
	uint32_t m_poly4[0x0f], m_poly5[0x1f], m_poly9[0x1ff];
	std::vector<uint32_t> m_poly17;
	uint32_t m_p4 = 0, m_p5 = 0, m_p9 = 0, m_p17 = 0;
	int m_clock_cnt[3] = {0, 0, 0};
	uint8_t m_audctl = 0, m_skctl = 0, m_skstat = 0, m_irqst = 0x08, m_irqen = 0, m_allpot = 0;
	uint8_t m_potx[8] = {}, m_pot_counter = 0;
	uint32_t m_out_raw = 0;
	bool m_old_raw_inval = true;
	void process_channel(int ch);
	void potgo();
};

class Ym2151Host;
class Tms5220;

class SoundChips {
public:
	SoundChips();
	~SoundChips();
	void reset();

	// bring the chips' internal state up to the given sound-CPU cycle
	void sync(uint64_t cycle);
	std::function<uint64_t()> now;        // current sound CPU cycle (set by the machine)

	uint8_t pokey_read(int which, int offset, uint8_t allpot);
	void pokey_write(int which, int offset, uint8_t data);
	uint8_t ym_read(int offset);
	void ym_write(int offset, uint8_t data);
	bool tms_readyq();
	void tms_data_w(uint8_t data);
	void tms_wsq_w(int state);
	void mixer_w(uint8_t data);
	void switch_w(uint8_t data);
	void sndrst_w(uint8_t data);

	// audio output: stereo int16 at 'output_rate()' (YM2151 native rate, ~55.9 kHz)
	static constexpr int CYCLES_PER_SAMPLE = 32;   // 6502 cycles per YM2151 sample
	static double output_rate() { return (14318181.0 / 8.0) / CYCLES_PER_SAMPLE; }
	bool audio_enabled = true;
	size_t take_samples(int16_t *dst, size_t max_frames);   // drains generated stereo frames
	size_t available_frames() const { return m_audio.size() / 2; }

	// diagnostics: per-chip signal energy and register writes since the last reset
	struct Stats {
		double ym = 0, pokey[2] = {0, 0}, tms = 0;   // sums of squared output
		uint64_t samples = 0;
		uint32_t ym_writes = 0, pokey_writes[2] = {0, 0}, tms_writes = 0;
	} stats;

private:
	Pokey m_pokey[2];
	std::unique_ptr<Ym2151Host> m_ym;
	std::unique_ptr<Tms5220> m_tms;
	uint64_t m_cycle = 0;
	uint32_t m_sub = 0;                    // cycles into the current output sample
	int32_t m_pokey_acc[2] = {0, 0};
	double m_gain_ym = 1.0, m_gain_pokey = 1.0, m_gain_tms = 1.0;
	uint8_t m_sndrst = 0;
	std::vector<int16_t> m_audio;
	void produce_sample();
};
