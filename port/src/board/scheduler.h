// Frame scheduler shared by the native runtime and the reference model, so both
// interleave the two programs and the board events identically.
//
// A frame is 416 scanlines of 40 us.  Each scanline is split into SUB slices; in every
// slice the main program runs for its share of 10 MHz T-11 cycles, then the sound program
// for its share of 1.79 MHz 6502 cycles.  Board events (32V, VBLANK, sound timer IRQ)
// happen on slice boundaries.
#pragma once
#include "board.h"
#include "soundchips.h"
#include <cstdint>

struct SchedState {
	int64_t main_debt = 0;          // T-11 cycles overrun from the previous slice
	double sound_target = 0;        // fractional 6502 cycle target
	uint64_t sound_done = 0;        // 6502 cycles executed since power-on
	uint64_t sound_slice_start = 0;
	double next_sound_irq = 0;
};

constexpr int SCHED_SUB = 8;

// Main / Sound must provide:  int run(int cycles)  -> cycles consumed
//                             int progress() const -> cycles consumed so far in current run()
template <class Main, class Sound>
void run_frame(Board &board, Main &main, Sound &sound, SchedState &st)
{
	constexpr int MAIN_PER_SUB = 400 / SCHED_SUB;
	const double SOUND_PER_SUB = Board::SOUND_CLOCK * Board::LINE_TIME / SCHED_SUB;
	const double SOUND_IRQ_PERIOD = Board::SOUND_CLOCK / (20000000.0 / 2 / 16 / 16 / 16 / 10);

	for (int line = 0; line < Board::VTOTAL; line++)
	{
		board.begin_line(line);
		for (int s = 0; s < SCHED_SUB; s++)
		{
			const int64_t want = MAIN_PER_SUB - st.main_debt;
			int64_t done = 0;
			while (done < want)
			{
				done += main.run((int)(want - done));
				board.apply_pending();
			}
			st.main_debt = done - want;

			st.sound_target += SOUND_PER_SUB;
			while ((double)st.sound_done < st.sound_target)
			{
				st.sound_slice_start = st.sound_done;
				const int n = (int)(st.sound_target - (double)st.sound_done) + 1;
				st.sound_done += (uint64_t)sound.run(n);
			}
			st.sound_slice_start = st.sound_done;
			if ((double)st.sound_done >= st.next_sound_irq)
			{
				st.next_sound_irq += SOUND_IRQ_PERIOD;
				board.sound_periodic_irq();
			}
			board.chips().sync(st.sound_done);
		}
	}
}
