// The native game machine: translated main program + board hardware model.
#pragma once
#include "board.h"
#include "scheduler.h"
#include "soundcpu.h"
#include <memory>

// T-11 cycles run since power-on, exact also in the middle of a time slice (for tooling)
uint64_t main_cycles_now();

class NativeMachine : public CpuLink {
public:
	explicit NativeMachine(const RomSet &roms);
	~NativeMachine();
	void reset();
	void run_frame();

	Board board;
	SchedState sched;
	std::unique_ptr<SoundCpu> sound;

	// CpuLink
	void main_set_irq_lines(uint8_t cp) override;
	void main_end_timeslice() override;
	void sound_set_nmi(bool s) override { sound->set_nmi(s); }
	void sound_set_irq(bool s) override { sound->set_irq(s); }
	void sound_set_reset(bool held) override { sound->set_reset(held); }

private:
	void *m_fiber = nullptr;
};
