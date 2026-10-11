// Abstract sound program executor (translated 6502 program, or the reference
// interpreter while the translation is being verified).
#pragma once
#include <cstdint>
#include <memory>

class Board;

class SoundCpu {
public:
	virtual ~SoundCpu() = default;
	virtual void reset() = 0;
	virtual int run(int cycles) = 0;          // returns cycles consumed
	virtual int progress() const = 0;         // cycles consumed so far in the current run()
	virtual void set_nmi(bool state) = 0;
	virtual void nmi_state(bool &line, bool &pending) const = 0;   // for snapshots
	virtual void restore_nmi(bool line, bool pending) = 0;
	virtual void set_irq(bool state) = 0;
	virtual void set_reset(bool held) = 0;
	virtual uint16_t pc() const = 0;
};

std::unique_ptr<SoundCpu> make_sound_cpu(Board &board);
