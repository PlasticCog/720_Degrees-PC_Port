// Reference machine: the shared Board driven by interpreting CPU cores.
// DEVELOPMENT ORACLE ONLY (reverse engineering, coverage, verification of the native code).
#pragma once
#include "board.h"
#include "scheduler.h"
#include "t11.h"
#include "m6502.h"

class RefMachine : public CpuLink, public T11Bus, public M6502Bus {
public:
	explicit RefMachine(const RomSet &roms);
	void reset();
	void run_frame();

	Board board;
	T11 t11;
	M6502 m6502;
	SchedState sched;

	// CpuLink
	void main_set_irq_lines(uint8_t cp) override { t11.set_cp_state(cp); }
	void main_end_timeslice() override { t11.abort_timeslice(); }
	void sound_set_nmi(bool s) override { m6502.set_nmi(s); }
	void sound_set_irq(bool s) override { m6502.set_irq(s); }
	void sound_set_reset(bool held) override
	{
		m6502.set_halt(held);
		if (!held) m6502.reset();
	}
	// T11Bus
	bool simple_slapstic = false;    // use the native runtime's simplified slapstic model (verification)
	void tap(uint16_t a) { if (simple_slapstic) board.slapstic_simple(a); else board.slapstic_tap(a); }
	uint16_t fetch(uint16_t a) override { tap(a); return board.main_read16(a, 0xffff); }
	uint16_t read_word(uint16_t a) override { tap(a); uint16_t v = board.main_read16(a, 0xffff); if (on_data) on_data(a, false, true, v); return v; }
	uint8_t read_byte(uint16_t a) override { tap(a); uint8_t v = board.main_read8(a); if (on_data) on_data(a, false, false, v); return v; }
	void write_word(uint16_t a, uint16_t d) override { tap(a); if (on_data) on_data(a, true, true, d); board.main_write16(a, d, 0xffff); }
	void write_byte(uint16_t a, uint8_t d) override { tap(a); if (on_data) on_data(a, true, false, d); board.main_write8(a, d); }
	// data access hook (addr, is_write, is_word, value) for tracing
	std::function<void(uint16_t, bool, bool, uint16_t)> on_data;
	// M6502Bus
	uint8_t read(uint16_t a) override { return board.sound_read(a); }
	void write(uint16_t a, uint8_t d) override { board.sound_write(a, d); }

private:
	struct MainRunner { T11 &c; int run(int n) { return c.run(n); } };
	struct SoundRunner { M6502 &c; int run(int n) { return c.run(n); } };
};
