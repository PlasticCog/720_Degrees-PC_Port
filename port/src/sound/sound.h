// Rewritten sound program: hand-written C++ replacements for the routines of the original sound
// CPU (6502) program, replacing the automatic translation routine by routine, as port/src/logic
// does for the main program.
//
// How it fits together
//   routines.txt          lists the rewritten routines; the translator (tools/m6502_to_cpp.py)
//                         renames each original to snd_..._orig and generates a wrapper that
//                         calls sound::enter()
//   routines_*.cpp        the replacements, registered with SOUND_TABLE(...)
//   difference tester     (with logic::g_difftest) runs original and replacement from the same
//                         state and compares registers, cycles, sound RAM, every access to the
//                         chips and latches (the original's reads are replayed to the
//                         replacement) and every step; the run continues with the original's
//                         result
//
// The routines follow the original instruction by instruction: an instruction's cycles (cyc,
// charged before its memory access, as the 6502 does it), its action and flags, then step()
// at the next instruction's address, where the board may run and interrupts come in.
#pragma once
#include "s65rt.h"
#include <cstdint>
#include <string>

namespace sound {

enum : uint8_t {
	RTI = 1,         // interrupt handler: returns with RTI
	NOTEST = 2,      // never returns (the main loop): runs, but is not compared
	INNER = 4,       // starts inside a routine of the translation (reached by a jump there):
	                 // an entry only for the build without the translation
};

using Fn = void (*)();

struct Routine {
	uint16_t addr;
	const char *name;
	Fn fn;
	uint8_t flags;
};

// each routines_<name>.cpp defines its table with SOUND_TABLE(name, ...); sound.cpp lists the
// tables (TABLES there)
using TableFn = const Routine *(*)(int &count);
#define SOUND_TABLE(name, ...) \
	const sound::Routine *sound_##name(int &count) \
	{ static const sound::Routine t[] = { __VA_ARGS__ }; count = (int)(sizeof(t) / sizeof(t[0])); return t; }
inline Routine routine(uint16_t addr, const char *name, Fn f, uint8_t flags = 0) { return Routine{addr, name, f, flags}; }

// the program left the routine at another address (an interrupt returned elsewhere): the
// frame that expects the new PC continues
struct Unwound {};

// ---- the steps of an instruction
using s65rt::S;
using s65rt::SR;
using s65rt::SROM;

inline void cyc(int n) { S.cyc += n; }
// the instruction is done, the next one is at next: interrupts and the board come in here
inline void step(uint16_t next)
{
	if (!s65rt::sync6(next)) throw Unwound{};
}
// a conditional branch at pc: taken to target (one more cycle, two across a page), else on
inline bool branch(uint16_t pc, bool taken, uint16_t target)
{
	cyc(2);
	const uint16_t next = (uint16_t)(pc + 2);
	if (taken)
	{
		cyc(((target ^ next) & 0xff00) ? 2 : 1);
		step(target);
	}
	else
		step(next);
	return taken;
}
// abs,X / abs,Y and (zp),Y reads: the address, one more cycle when it crosses a page
inline uint16_t idx(uint16_t base, uint8_t i)
{
	const uint16_t a = (uint16_t)(base + i);
	if ((base ^ a) & 0xff00) cyc(1);
	return a;
}
inline uint16_t zp16(uint8_t z) { return (uint16_t)(SR[z] | SR[(uint8_t)(z + 1)] << 8); }
// memory: RAM, ROM and the chips (through the board, or the difference test's record)
inline uint8_t rd(uint16_t a) { return s65rt::srd(a); }
inline void wr(uint16_t a, uint8_t v) { s65rt::swr(a, v); }
inline void push(uint8_t v) { s65rt::spush(v); }
inline uint8_t pull() { return s65rt::spull(); }
inline void nz(uint8_t v) { s65rt::setnz(v); }

// JSR at pc (to target, returning to pc + 3), RTS, RTI and JMP to another routine
void jsr(uint16_t pc, uint16_t target);
void rts();
void rti();
void jmp(uint16_t target);
// RTS used as a jump (the address pushed before): on to the code there, which returns to this
// routine's caller
void rts_jump();
// continue the program at pc, abandoning the routine and its callers up to one that expects pc
[[noreturn]] inline void leave_to(uint16_t pc)
{
	S.pc = pc;
	throw Unwound{};
}

// called by the generated wrappers; false = run the original translation
bool enter(int index);
// without the translation (the native build's dispatch): run the rewritten routine that
// starts at pc; false if there is none
bool run_entry(uint16_t pc);

std::string report(bool all = false);

} // namespace sound
