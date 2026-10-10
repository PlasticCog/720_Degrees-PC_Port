// Reference DEC T-11 CPU core (development oracle only - not used by the native game).
// Behaviour, bus access order and cycle counts follow MAME's t11 core
// (Source_Emulator/mame/src/devices/cpu/t11, BSD-3-Clause, Aaron Giles).
#pragma once
#include <cstdint>

class T11Bus {
public:
	virtual ~T11Bus() = default;
	virtual uint16_t fetch(uint16_t addr) = 0;            // opcode / operand word fetch
	virtual uint16_t read_word(uint16_t addr) = 0;        // addr is even
	virtual uint8_t read_byte(uint16_t addr) = 0;
	virtual void write_word(uint16_t addr, uint16_t data) = 0;
	virtual void write_byte(uint16_t addr, uint8_t data) = 0;
};

class T11 {
public:
	explicit T11(T11Bus &bus) : m_bus(bus) {}

	void reset(uint16_t initial_pc);
	// run for the given number of cycles; returns cycles actually consumed
	int run(int cycles);
	// interrupt request lines CP0..CP3 as a 4 bit mask
	void set_cp_state(uint8_t state);
	// make the core re-check interrupts after the current instruction
	void request_irq_check() { m_check_irqs = true; }
	// ends the current timeslice after the current instruction
	void abort_timeslice() { if (m_icount > 0) { m_aborted += m_icount; m_icount = 0; } }

	uint16_t r[8] = {};
	uint8_t psw = 0;
	uint16_t ppc = 0;
	uint64_t total_cycles = 0;
	uint64_t total_insns = 0;

	// optional per-instruction hook (for coverage / tracing)
	void (*insn_hook)(void *ctx, uint16_t pc) = nullptr;
	void (*irq_hook)(void *ctx, uint8_t vector, uint16_t new_pc) = nullptr;
	void *hook_ctx = nullptr;

private:
	T11Bus &m_bus;
	int m_icount = 0;
	int m_aborted = 0;
	uint16_t m_initial_pc = 0x8000;
	uint8_t m_cp_state = 0;
	bool m_wait_state = false;
	bool m_check_irqs = false;
	bool m_trace_trap = false;

	void execute_one();
	void check_irqs();
	void take_interrupt(uint8_t vector);
	void trap_to(uint16_t vector);

	uint16_t fetch() { uint16_t v = m_bus.fetch(r[7] & 0xfffe); r[7] += 2; return v; }
	int rword(int a) { return m_bus.read_word(a & 0xfffe); }
	int rbyte(int a) { return m_bus.read_byte(a & 0xffff); }
	void wword(int a, int v) { m_bus.write_word(a & 0xfffe, v & 0xffff); }
	void wbyte(int a, int v) { m_bus.write_byte(a & 0xffff, v & 0xff); }
	void push(int v) { r[6] -= 2; wword(r[6], v); }
	int pop() { int v = rword(r[6]); r[6] += 2; return v; }

	int ea_byte(int mode, int reg);
	int ea_word(int mode, int reg);
};
