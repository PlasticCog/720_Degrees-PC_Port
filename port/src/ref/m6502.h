// Reference NMOS 6502 core (development oracle / sound-program reference only).
#pragma once
#include <cstdint>

class M6502Bus {
public:
	virtual ~M6502Bus() = default;
	virtual uint8_t read(uint16_t addr) = 0;
	virtual void write(uint16_t addr, uint8_t data) = 0;
};

class M6502 {
public:
	explicit M6502(M6502Bus &bus) : m_bus(bus) {}
	void reset();
	int run(int cycles);            // returns cycles consumed
	void set_irq(bool state) { m_irq = state; }
	void set_nmi(bool state) { if (state && !m_nmi_line) m_nmi_pending = true; m_nmi_line = state; }
	void set_halt(bool state) { m_halted = state; }   // held in reset
	bool halted() const { return m_halted; }
	int progress() const { return m_cycles; }

	uint8_t a = 0, x = 0, y = 0, s = 0xfd, p = 0x24;
	uint16_t pc = 0;
	uint64_t total_cycles = 0;
	void (*insn_hook)(void *ctx, uint16_t pc) = nullptr;
	void *hook_ctx = nullptr;

private:
	M6502Bus &m_bus;
	bool m_irq = false, m_nmi_line = false, m_nmi_pending = false, m_halted = false;
	int m_cycles = 0;

	uint8_t rd(uint16_t a) { return m_bus.read(a); }
	void wr(uint16_t a, uint8_t v) { m_bus.write(a, v); }
	uint16_t rd16(uint16_t a) { return rd(a) | (rd(uint16_t(a + 1)) << 8); }
	void push(uint8_t v) { wr(0x100 | s, v); s--; }
	uint8_t pull() { s++; return rd(0x100 | s); }
	void interrupt(uint16_t vector, bool brk);
	void step();
};
