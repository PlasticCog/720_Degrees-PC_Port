#include "refmachine.h"

RefMachine::RefMachine(const RomSet &roms) : board(roms, *this), t11(*this), m6502(*this)
{
	board.chips().now = [this]() { return sched.sound_slice_start + (uint64_t)m6502.progress(); };
}

void RefMachine::reset()
{
	sched = SchedState{};
	board.reset();
	t11.reset(0x8000);
}

void RefMachine::run_frame()
{
	MainRunner mr{t11};
	SoundRunner sr{m6502};
	::run_frame(board, mr, sr, sched);
}
