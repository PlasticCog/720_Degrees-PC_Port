// Rewritten routines of the fixed ROM F:B000-CFFF: the sprite chains and object movement run by
// the 32V interrupt, the map objects (park objects placed from the map table, 0x09AA-0x0E9A)
// and the decimal number printer.  Assembly-interface routines reproduce registers and flags;
// C routines leave the carry flag as the original does (callers written in assembly may
// return it).
//
// Stack contents.  For a dynamic map object the record pointer 0x09B2 still points at the
// stack argument F:CE24 got when it was created (see add_map_object), so the original reads
// whatever lies on the stack there: return addresses, saved registers and locals of the routines
// running at that moment.  The routines here therefore leave the stack as the original does:
// calls go through old_jsr() with the original's return address, the register-save helpers'
// frames are written (CFrame::save_regs) and locals live where the original keeps them; the
// registers handed to other routines are the original's working registers.
#include "logic.h"
#include <cstdio>
#include <exception>

namespace logic {
namespace steps {                                      // defined in routines_core.cpp
void rts();
void jsr(uint16_t ret, uint16_t addr);
void c_save(int n, uint16_t body);
void c_restore(int n);
}

namespace {

using namespace t11rt;
using namespace steps;

uint16_t sext8(uint8_t v) { return (uint16_t)(int16_t)(int8_t)v; }

// condition codes of the instructions the fl:: helpers do not cover (as the T-11 sets them)
uint16_t fl_asl(uint16_t v) { const uint16_t r = (uint16_t)(v << 1); f_nzvc(nzw(r) | (v >> 15)); f_shift(); return r; }
uint16_t fl_asr(uint16_t v) { const uint16_t r = (uint16_t)((int16_t)v >> 1); f_nzvc(nzw(r) | (v & 1)); f_shift(); return r; }
uint16_t fl_ror(uint16_t v) { const uint16_t r = (uint16_t)((C() << 15) | (v >> 1)); f_nzvc(nzw(r) | (v & 1)); f_shift(); return r; }
uint16_t fl_rol(uint16_t v) { const uint16_t r = (uint16_t)((v << 1) | C()); f_nzvc(nzw(r) | (v >> 15)); f_shift(); return r; }
uint16_t fl_neg(uint16_t v) { const uint16_t r = (uint16_t)-v; f_nzvc(nzw(r) | (r == 0x8000 ? VF : 0) | (r ? CF : 0)); return r; }
uint8_t fl_negb(uint8_t v) { const uint8_t r = (uint8_t)-v; f_nzvc(nzb(r) | (r == 0x80 ? VF : 0) | (r ? CF : 0)); return r; }
uint16_t fl_com(uint16_t v) { const uint16_t r = (uint16_t)~v; f_nzvc(nzw(r) | CF); return r; }
uint8_t fl_comb(uint8_t v) { const uint8_t r = (uint8_t)~v; f_nzvc(nzb(r) | CF); return r; }
uint16_t fl_swab(uint16_t v) { const uint16_t r = (uint16_t)((v << 8) | (v >> 8)); f_nzvc(nzb(r & 0xFF)); return r; }
uint16_t fl_logic(uint16_t r) { f_nzv(nzw(r)); return r; }   // BIC / BIS / XOR / BIT
uint8_t fl_logicb(uint8_t r) { f_nzv(nzb(r)); return r; }
// the stack word at SP + o (the original addresses its locals relative to the moving SP)
uint16_t sp_at(int o) { return (uint16_t)(T.r[6] + o); }

// MOV #a,R0; MOV R0,@#0x01D2; MOV R0,@#BANK_A at pc (bank_b: 0x01D4 / BANK_B)
void bank_a(uint16_t pc, uint16_t a)
{
	T.r[0] = a;
	tick((uint16_t)(pc + 4), 18);
	poke(0x01D2, T.r[0]);
	tick((uint16_t)(pc + 8), 27);
	poke(0x1400, T.r[0]);
	tick((uint16_t)(pc + 0x0C), 27);
}

void bank_b(uint16_t pc, uint16_t b)
{
	T.r[0] = b;
	tick((uint16_t)(pc + 4), 18);
	poke(0x01D4, T.r[0]);
	tick((uint16_t)(pc + 8), 27);
	poke(0x1402, T.r[0]);
	tick((uint16_t)(pc + 0x0C), 27);
}

// Cycles: the routines charge the original's exact cost on the path they take (per straight-
// line part, tools/insn_cycles.py; loops per pass with charge() at the pass's address); the
// register-save helper's epilogue is charged by CFrame at the end.  Nothing is left to the
// table charge.
// Every charge is also a point where interrupts may come in (at the address of the routine
// running, kept in g_pc), so an interrupt falls between the same parts of the routine as in
// the original (before the part it falls in) instead of after the whole routine.
uint16_t g_pc = 0;
struct At {
	uint16_t saved;
	explicit At(uint16_t pc) : saved(g_pc) { g_pc = pc; }
	~At() { g_pc = saved; }
	At(const At &) = delete;
	At &operator=(const At &) = delete;
};
// Charging n cycles of straight-line code at pc.  The board runs in slices of 50 T-11 cycles
// and the original lets it run (and takes interrupts) after the instruction that ends a slice,
// so a part that ends a slice charges up to the slice's end, lets the board run there and
// then charges the rest (again for every further slice it spans): interrupts and the sound
// CPU then see the routine's time as in the original (to within an instruction), not the
// time after the whole part.  An interrupt made pending by the last part is taken first.
void charge(uint16_t pc, int n)
{
	if (g_atomic)                                      // difference tester: charge, count the step
	{
		tick(pc, n);
		return;
	}
	if (T.check_irqs) tick(pc, 0);
	while (T.icount <= n)
	{
		const int k = T.icount > 0 ? T.icount : 0;
		n -= k;
		tick(pc, k);
	}
	T.icount -= n;
}
void cy(int n) { charge(g_pc, n); }
// An instruction that reads or writes the board (I/O registers) or changes the priority: charged
// after its access, as the original's instruction is, so that the board sees the access in the
// same time slice and an interrupt it lets in is taken right after it.
template <class F> void io(int n, F access)
{
	access();
	tick(g_pc, n);
}
// JSR R1,@#F144 / F152 / F162 and the helper's prologue; the epilogues after the body's RTS
constexpr int IN144 = 30 + 129, IN152 = 30 + 153, IN162 = 30 + 177;
constexpr int OUT144 = 12 + 18 * 3 + 21, OUT152 = 12 + 18 * 4 + 21, OUT162 = 18 * 5 + 21;
uint16_t asr(uint16_t v, int n) { return (uint16_t)((int16_t)v >> n); }

// condition codes of the instructions the fl:: helpers do not cover
void set_c(bool c) { T.psw = (uint8_t)(c ? (T.psw | CF) : (T.psw & ~CF)); }
bool carry(uint16_t a, uint16_t b) { return (uint32_t)a + b > 0xFFFF; }
bool lt() { return fN() != fV(); }                     // BLT after the last flag-setting instruction
bool gt() { return !fZ() && !lt(); }                   // BGT

// MTPS: the priority and condition codes (not the T bit); pending interrupts are rechecked
void mtps(uint8_t v)
{
	T.psw = (uint8_t)((T.psw & ~0xEF) | (v & 0xEF));
	T.check_irqs = true;
}

// bank selection: shadow (0x01D2 / 0x01D4) and hardware register
void set_bank_a(uint16_t v) { poke(0x01D2, v); poke(0x1400, v); }
void set_bank_b(uint16_t v) { poke(0x01D4, v); poke(0x1402, v); }
void restore_banks(uint16_t a, uint16_t b)
{
	poke(0x01D2, a);
	poke(0x01D4, b);
	poke(0x1400, peek(0x01D2));
	poke(0x1402, peek(0x01D4));
}
// the sprite code switches the hardware registers only (the shadows keep the caller's banks)
void hw_banks(uint16_t a, uint16_t b) { poke(0x1400, a); poke(0x1402, b); }

// JSR PC,@#addr from the original's call site (ret = the address after it): like logic::call,
// but the return address on the stack is the original's (the translated JSR does the same).
//
// Long loops let interrupts in once per pass (charge, with the cycles the original's pass
// costs, at the original's loop address); the stack pointer and registers are the original's
// there, as an interrupt saves its frame below the stack pointer.
uint16_t old_jsr(uint16_t addr, uint16_t ret)
{
	push(ret);
	T.r[7] = addr;
	shadow_push(ret);
	dispatch();
	shadow_pop();
	if (T.r[7] != ret)
	{
		fprintf(stderr, "logic: call of %04X from %04X did not return (PC=%04X)\n", addr, (uint16_t)(ret - 4), T.r[7]);
		fatal_unknown_pc(T.r[7]);
	}
	// the instruction after the JSR: interrupts may come in here
	if (!sync(ret)) throw Unwound{};
	return T.r[0];
}

// a C routine's frame: the stack pointer and R1-R5 are given back at the end
struct CFrame {
	uint16_t sp, regs[6];
	int unwinding;                                      // not when abandoned (Unwound)
	int out = 0;                                        // the helper's epilogue, charged at the end
	CFrame() : sp(T.r[6]), unwinding(std::uncaught_exceptions())
	{
		for (int i = 1; i < 6; i++) regs[i] = T.r[i];
	}
	~CFrame() noexcept(false)
	{
		if (std::uncaught_exceptions() > unwinding) return;
		T.r[6] = sp;
		for (int i = 1; i < 6; i++) T.r[i] = regs[i];
		if (out) cy(out);                               // an interrupt point, as in the original
	}
	CFrame(const CFrame &) = delete;
	CFrame &operator=(const CFrame &) = delete;
	// JSR R1,@#F138/F144/F152/F162: R1 and R2..R(1+n) saved, then the helper's return address
	void save_regs(int n, uint16_t helper_ret)
	{
		out = helper_ret == 0xF150 ? OUT144 : helper_ret == 0xF160 ? OUT152 : OUT162;
		push(T.r[1]);
		for (int r = 2; r < 2 + n; r++) push(T.r[r]);
		push(helper_ret);
	}
};
constexpr int SAVE_R2_R3 = 2, SAVE_R2_R4 = 3, SAVE_R2_R5 = 4;
constexpr uint16_t F144_RET = 0xF150, F152_RET = 0xF160, F162_RET = 0xF172;

// bits 9-15 of a map word as the original extracts them (ASR, CLRB, SWAB, MOVB): the cell offset
uint16_t field9(uint16_t d) { return sext8((uint8_t)(asr(d, 1) >> 8)); }

// ---------------------------------------------------------------- sprite chains (32V interrupt)
//
// Every object slot (32) owns a chain of sprite pieces in motion-object RAM (0x3800 + link),
// with the number of pieces in 0x04D0.  Two chains per object exist (slot and slot + 0x20, the
// lists at 0x0988/0x0989); F:ACE6 takes a chain out of the display list, F:AE02 puts it in.

// F:B03C (assembly): apply the piece counts queued by F:B602: for each of the four queue
// entries (slot 0x0989+i, count 0x098D+i) set the count, relink the second chain of a slot
// with flag 0x04, and drop the entry unless the slot's flag 0x20 asks to keep it
void apply_queued_piece_counts()
{
	fl::tstb(peekb(0x0992));
	tick(0xB040, 24);
	if (fZ())
	{
		tick(0xB090, 12);                              // BEQ
		rts();
		return;
	}
	tick(0xB042, 12);
	T.r[2] = 0x0004;
	fl::mov(T.r[2]);
	tick(0xB046, 18);
	for (;;)
	{
		T.r[1] = sext8(peekb((uint16_t)(0x0989 + T.r[2])));
		fl::movb((uint8_t)T.r[1]);
		tick(0xB04A, 27);
		if (fZ())
			tick(0xB08E, 12);                          // BEQ: free entry
		else
		{
			tick(0xB04C, 12);
			pokeb((uint16_t)(0x04D0 + T.r[1]), peekb((uint16_t)(0x098D + T.r[2])));
			tick(0xB052, 45);
			T.r[4] = 0x0490;
			fl::mov(T.r[4]);
			tick(0xB056, 18);
			T.r[4] = fl::add(T.r[4], T.r[1]);
			tick(0xB058, 12);
			fl_logicb(peekb(T.r[4]) & 0x04);
			tick(0xB05C, 24);
			if (fZ())
				tick(0xB07C, 12);                      // BEQ
			else
			{
				tick(0xB05E, 12);
				push(0x0001);
				fl::mov(0x0001);
				tick(0xB062, 30);
				T.r[3] = T.r[1];
				fl::mov(T.r[3]);
				tick(0xB064, 12);
				T.r[3] = fl::add(T.r[3], 0x0020);
				tick(0xB068, 18);
				push(T.r[3]);
				fl::mov(T.r[3]);
				tick(0xB06A, 24);
				jsr(0xB06E, 0xACE6);
				poke(T.r[6], 0x0001);
				fl::mov(0x0001);
				tick(0xB072, 27);
				push(T.r[3]);
				fl::mov(T.r[3]);
				tick(0xB074, 24);
				jsr(0xB078, 0xAE02);
				T.r[6] = fl::add(T.r[6], 0x0006);
				tick(0xB07C, 18);
			}
			pokeb(T.r[4], fl_logicb((uint8_t)(peekb(T.r[4]) & ~0x40)));
			tick(0xB080, 27);
			fl_logicb(peekb(T.r[4]) & 0x20);
			tick(0xB084, 24);
			if (!fZ())
				tick(0xB08E, 12);                      // BNE: the entry stays
			else
			{
				tick(0xB086, 12);
				pokeb(0x0992, fl::decb(peekb(0x0992)));
				tick(0xB08A, 27);
				pokeb((uint16_t)(0x0989 + T.r[2]), 0);
				fl::clr();
				tick(0xB08E, 30);
			}
		}
		if (!--T.r[2]) break;
		tick(0xB046, 18);                              // SOB
	}
	tick(0xB090, 18);
	rts();
}

// top edge used by F:B092 / F:B120: 0x4F80 - the height table entry 0x5BCC of type R1
uint16_t type_height(uint16_t type) { return peek((uint16_t)(0x5BCC + 2 * type)); }

// F:B092 (assembly): first chains of objects with flag 0x02 (shown): taken out of the display
// list unless the object (flag 0x80) is on screen (y >= 0x4F80 - height, x >= 0xD400 signed);
// objects not shown but on screen with pieces are put in.  Slots 0x0994 down to 0.
// screen tests of the chain updates (slot R3, type R1): the top edge 0x4F80 - height 0x5BCC[type]
// against y 0x0670 (CMP y,R0 at pc + 0x12), and x 0x0630 against 0xD400 (CMP at pc + 4)
void chain_y_test(uint16_t pc)
{
	T.r[4] = T.r[3];
	tick((uint16_t)(pc + 2), 12);
	T.r[4] = fl_asl(T.r[4]);
	tick((uint16_t)(pc + 4), 12);
	T.r[0] = T.r[1];
	tick((uint16_t)(pc + 6), 12);
	T.r[0] = fl_asl(T.r[0]);
	tick((uint16_t)(pc + 8), 12);
	T.r[0] = peek((uint16_t)(0x5BCC + T.r[0]));
	tick((uint16_t)(pc + 0x0C), 27);
	T.r[0] = fl::sub(T.r[0], 0x4F80);
	tick((uint16_t)(pc + 0x10), 18);
	T.r[0] = fl_neg(T.r[0]);
	tick((uint16_t)(pc + 0x12), 12);
	fl::cmp(peek((uint16_t)(0x0670 + T.r[4])), T.r[0]);
	tick((uint16_t)(pc + 0x16), 27);
}

void chain_x_test(uint16_t pc)
{
	T.r[0] = T.r[3];
	tick((uint16_t)(pc + 2), 12);
	T.r[0] = fl_asl(T.r[0]);
	tick((uint16_t)(pc + 4), 12);
	fl::cmp(peek((uint16_t)(0x0630 + T.r[0])), 0xD400);
	tick((uint16_t)(pc + 0x0A), 33);
}

void update_chains_a()
{
	T.r[3] = sext8(peekb(0x0994));
	fl::movb((uint8_t)T.r[3]);
	tick(0xB096, 24);
	if (lt())
	{
		tick(0xB11E, 12);                              // BLT: no objects
		rts();
		return;
	}
	tick(0xB098, 12);
	for (;;)
	{
		T.r[1] = 0;
		fl::clr();
		tick(0xB09A, 12);
		T.r[1] |= peekb((uint16_t)(0x0370 + T.r[3]));
		fl::logicb((uint8_t)T.r[1]);
		tick(0xB09E, 27);
		T.r[2] = 0;
		fl::clr();
		tick(0xB0A0, 12);
		T.r[2] |= peekb((uint16_t)(0x0490 + T.r[3]));
		fl::logicb((uint8_t)T.r[2]);
		tick(0xB0A4, 27);
		enum { NONE, OUT, IN } action = NONE;
		fl_logic(T.r[2] & 0x0002);
		tick(0xB0A8, 18);
		if (!fZ())
		{
			// shown: out unless on screen (flag 0x80, y and x)
			tick(0xB0AA, 12);
			fl::tstb((uint8_t)T.r[2]);
			tick(0xB0AC, 12);
			action = OUT;
			if (!fN())
				tick(0xB0D2, 12);                      // BPL
			else
			{
				tick(0xB0AE, 12);
				chain_y_test(0xB0AE);
				if (lt())
					tick(0xB0D2, 12);                  // BLT
				else
				{
					tick(0xB0C6, 12);
					chain_x_test(0xB0C6);
					if (!lt())
					{
						tick(0xB11A, 12);              // BGE: stays shown
						action = NONE;
					}
					else
						tick(0xB0D2, 12);
				}
			}
		}
		else
		{
			// not shown: in when on screen with pieces
			tick(0xB0DC, 12);
			fl::tstb((uint8_t)T.r[2]);
			tick(0xB0DE, 12);
			if (!fN())
				tick(0xB11A, 12);                      // BPL
			else
			{
				tick(0xB0E0, 12);
				fl::tstb(peekb((uint16_t)(0x04D0 + T.r[3])));
				tick(0xB0E4, 27);
				if (fZ())
					tick(0xB11A, 12);                  // BEQ
				else
				{
					tick(0xB0E6, 12);
					fl_logic(T.r[2] & 0x0002);
					tick(0xB0EA, 18);
					if (!fZ())
						tick(0xB11A, 12);              // BNE
					else
					{
						tick(0xB0EC, 12);
						chain_y_test(0xB0EC);
						if (fZ() || lt())
							tick(0xB11A, 12);          // BLE
						else
						{
							tick(0xB104, 12);
							chain_x_test(0xB104);
							if (lt())
								tick(0xB11A, 12);      // BLT
							else
							{
								tick(0xB110, 12);
								action = IN;
							}
						}
					}
				}
			}
		}
		if (action != NONE)
		{
			push(0);
			tick(action == OUT ? 0xB0D4 : 0xB112, 24);
			push(T.r[3]);
			tick(action == OUT ? 0xB0D6 : 0xB114, 24);
			if (action == OUT)
			{
				jsr(0xB0DA, 0xACE6);
				tick(0xB118, 12);                      // BR
			}
			else
				jsr(0xB118, 0xAE02);
			const uint16_t a = pop(), b = pop();
			fl::cmp(a, b);
			tick(0xB11A, 24);
		}
		T.r[3] = fl::dec(T.r[3]);
		tick(0xB11C, 12);
		if (lt()) break;
		tick(0xB098, 12);                              // BGE
	}
	tick(0xB11E, 12);
	rts();
}

// F:B120 (assembly): the same for the second chains (slot + 0x20) with flag 0x04: visible
// when y <= 0x5080 (unsigned), y + height >= 0x2000 (unsigned) and x >= 0xD400 (signed)
void update_chains_b()
{
	T.r[3] = sext8(peekb(0x0994));
	fl::movb((uint8_t)T.r[3]);
	tick(0xB124, 24);
	if (lt())
	{
		tick(0xB1C6, 12);                              // BLT: no objects
		rts();
		return;
	}
	tick(0xB126, 12);
	// y 0x0670 at most 0x5080 (unsigned), y + height 0x5BCC[type] at least 0x2000 (unsigned):
	// MOV R3,R0 at pc ... CMP R4,#0x2000 at pc + 0x1C; false when out (BHI at pc + 0x0A)
	auto y_test = [](uint16_t pc) {
		T.r[0] = T.r[3];
		tick((uint16_t)(pc + 2), 12);
		T.r[0] = fl_asl(T.r[0]);
		tick((uint16_t)(pc + 4), 12);
		fl::cmp(peek((uint16_t)(0x0670 + T.r[0])), 0x5080);
		tick((uint16_t)(pc + 0x0A), 33);
		if (!fC() && !fZ())
		{
			tick((uint16_t)(pc + 0x2E), 12);           // BHI
			return false;
		}
		tick((uint16_t)(pc + 0x0C), 12);
		T.r[4] = T.r[3];
		tick((uint16_t)(pc + 0x0E), 12);
		T.r[4] = fl_asl(T.r[4]);
		tick((uint16_t)(pc + 0x10), 12);
		T.r[0] = T.r[1];
		tick((uint16_t)(pc + 0x12), 12);
		T.r[0] = fl_asl(T.r[0]);
		tick((uint16_t)(pc + 0x14), 12);
		T.r[4] = peek((uint16_t)(0x0670 + T.r[4]));
		tick((uint16_t)(pc + 0x18), 27);
		T.r[4] = fl::add(T.r[4], peek((uint16_t)(0x5BCC + T.r[0])));
		tick((uint16_t)(pc + 0x1C), 27);
		fl::cmp(T.r[4], 0x2000);
		tick((uint16_t)(pc + 0x20), 18);
		if (fC())
		{
			tick((uint16_t)(pc + 0x2E), 12);           // BCS
			return false;
		}
		tick((uint16_t)(pc + 0x22), 12);
		return true;
	};
	for (;;)
	{
		T.r[1] = 0;
		fl::clr();
		tick(0xB128, 12);
		T.r[1] |= peekb((uint16_t)(0x0370 + T.r[3]));
		fl::logicb((uint8_t)T.r[1]);
		tick(0xB12C, 27);
		T.r[2] = 0;
		fl::clr();
		tick(0xB12E, 12);
		T.r[2] |= peekb((uint16_t)(0x0490 + T.r[3]));
		fl::logicb((uint8_t)T.r[2]);
		tick(0xB132, 27);
		enum { NONE, OUT, IN } action = NONE;
		fl_logic(T.r[2] & 0x0004);
		tick(0xB136, 18);
		if (!fZ())
		{
			// shown: out unless on screen
			tick(0xB138, 12);
			fl::tstb((uint8_t)T.r[2]);
			tick(0xB13A, 12);
			action = OUT;
			if (!fN())
				tick(0xB16A, 12);                      // BPL
			else
			{
				tick(0xB13C, 12);
				if (y_test(0xB13C))                    // out: BHI / BCS to B16A
				{
					chain_x_test(0xB15E);
					if (!lt())
					{
						tick(0xB1C2, 12);              // BGE: stays shown
						action = NONE;
					}
					else
						tick(0xB16A, 12);
				}
			}
		}
		else
		{
			tick(0xB17A, 12);
			fl::tstb((uint8_t)T.r[2]);
			tick(0xB17C, 12);
			if (!fN())
				tick(0xB1C2, 12);                      // BPL
			else
			{
				tick(0xB17E, 12);
				fl::tstb(peekb((uint16_t)(0x04D0 + T.r[3])));
				tick(0xB182, 27);
				if (fZ())
					tick(0xB1C2, 12);                  // BEQ
				else
				{
					tick(0xB184, 12);
					T.r[0] = T.r[3];
					fl::mov(T.r[0]);
					tick(0xB186, 12);
					T.r[0] = fl_asl(T.r[0]);
					tick(0xB188, 12);
					fl::cmp(peek((uint16_t)(0x0670 + T.r[0])), 0x5080);
					tick(0xB18E, 33);
					bool in = false;
					if (!fC() && !fZ())
						tick(0xB1C2, 12);              // BHI
					else
					{
						tick(0xB190, 12);
						T.r[4] = T.r[3];
						fl::mov(T.r[4]);
						tick(0xB192, 12);
						T.r[4] = fl_asl(T.r[4]);
						tick(0xB194, 12);
						T.r[0] = T.r[1];
						fl::mov(T.r[0]);
						tick(0xB196, 12);
						T.r[0] = fl_asl(T.r[0]);
						tick(0xB198, 12);
						T.r[4] = peek((uint16_t)(0x0670 + T.r[4]));
						fl::mov(T.r[4]);
						tick(0xB19C, 27);
						T.r[4] = fl::add(T.r[4], peek((uint16_t)(0x5BCC + T.r[0])));
						tick(0xB1A0, 27);
						fl::cmp(T.r[4], 0x2000);
						tick(0xB1A4, 18);
						if (fC())
							tick(0xB1C2, 12);          // BCS
						else
						{
							tick(0xB1A6, 12);
							chain_x_test(0xB1A6);
							if (fZ() || lt())
								tick(0xB1C2, 12);      // BLE
							else
							{
								tick(0xB1B2, 12);
								in = true;
							}
						}
					}
					if (in) action = IN;
				}
			}
		}
		if (action != NONE)
		{
			const uint16_t pc = action == OUT ? 0xB16A : 0xB1B2;
			push(0x0001);
			tick((uint16_t)(pc + 4), 30);
			push(T.r[3]);
			tick((uint16_t)(pc + 6), 24);
			poke(T.r[6], fl::add(peek(T.r[6]), 0x0020));
			tick((uint16_t)(pc + 0x0A), 27);
			if (action == OUT)
			{
				jsr(0xB178, 0xACE6);
				tick(0xB1C0, 12);                      // BR
			}
			else
				jsr(0xB1C0, 0xAE02);
			const uint16_t a = pop(), b = pop();
			fl::cmp(a, b);
			tick(0xB1C2, 24);
		}
		T.r[3] = fl::dec(T.r[3]);
		tick(0xB1C4, 12);
		if (lt()) break;
		tick(0xB126, 12);                              // BGE
	}
	tick(0xB1C6, 12);
	rts();
}

// F:B1EA (assembly): rebuild the sprite pieces of the chains in display list 0x0993 (0 or 1),
// following the links 0x0808 from the head 0x0988+list (current link in 0x0FEE).  Objects
// whose flags (masked by 0xFD7E+list) ask for it get their pieces from the frame data
// (0x03B0 points to it, bank 0x3000/0x3400): x offset, y offset, then link/picture words, with
// the palette byte 0x0470; the others only move their pieces by the screen-y delta 0x05F0.
// Objects with a flag of 0xFD80+list are skipped.
// the next row byte of a frame at pc (CLR, MOVB (R1)+, ASR, ASR, ADD): 0x0FF8 = byte * 64 + 0x0FF4
void next_piece_row(uint16_t pc)
{
	poke(0x0FF8, 0);
	tick((uint16_t)(pc + 4), 27);
	pokeb(0x0FF9, peekb(T.r[1]++));
	tick((uint16_t)(pc + 8), 33);
	poke(0x0FF8, asr(peek(0x0FF8), 1));
	tick((uint16_t)(pc + 0x0C), 27);
	poke(0x0FF8, asr(peek(0x0FF8), 1));
	tick((uint16_t)(pc + 0x10), 27);
	poke(0x0FF8, (uint16_t)(peek(0x0FF8) + peek(0x0FF4)));
	tick((uint16_t)(pc + 0x16), 39);
}

void build_sprite_pieces()
{
	T.r[0] = sext8(peekb(0x0993));
	fl::movb((uint8_t)T.r[0]);
	tick(0xB1EE, 24);
	push(T.r[0]);                                      // the list is kept on the stack
	fl::mov(T.r[0]);
	tick(0xB1F0, 24);
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0xB1F2, 12);
	T.r[0] = sext8(peekb((uint16_t)(0x0988 + T.r[1])));
	fl::movb((uint8_t)T.r[0]);
	tick(0xB1F6, 27);
	poke(0x0FEE, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xB1FA, 27);
	uint16_t link = T.r[0];
	while ((int16_t)link >= 0)
	{
		tick(0xB200, 12);                              // BGE
		T.r[3] = peek(0x0FEE);
		fl::mov(T.r[3]);
		tick(0xB204, 24);
		T.r[3] = fl::logic((uint16_t)(T.r[3] & 0x001F));
		tick(0xB208, 18);
		T.r[2] = 0;
		fl::clr();
		tick(0xB20A, 12);
		T.r[2] |= peekb((uint16_t)(0x0490 + T.r[3]));
		fl::logicb((uint8_t)T.r[2]);
		tick(0xB20E, 27);
		T.r[5] = peek(T.r[6]);
		fl::mov(T.r[5]);
		tick(0xB210, 18);
		const bool skip = (peekb((uint16_t)(0xFD80 + T.r[5])) & T.r[2] & 0xFF) != 0;
		fl::logicb((uint8_t)((uint8_t)T.r[2] & peekb((uint16_t)(T.r[5] + 0xFD80))));
		tick(0xB214, 27);
		if (skip)
		{
			tick(0xB216, 12);
			tick(0xB362, 18);                          // JMP
		}
		else
		{
			tick(0xB21A, 12);
			T.r[5] = 0;
			fl::clr();
			tick(0xB21C, 12);
			push(peek(T.r[6]));
			fl::mov(peek(T.r[6]));
			tick(0xB21E, 30);
			poke(T.r[6], fl::add(peek(T.r[6]), 0xFD7E));
			tick(0xB222, 27);
			T.r[5] |= peekb(pop());                    // BISB @(SP)+,R5
			fl::logicb((uint8_t)T.r[5]);
			tick(0xB224, 24);
			T.r[5] = (uint16_t)~T.r[5];
			fl::com((uint16_t)(~T.r[5]));
			tick(0xB226, 12);
			T.r[2] = fl::logic((uint16_t)(T.r[2] & (uint16_t)~T.r[5]));
			tick(0xB228, 12);
			T.r[5] = peek(0x0FEE);
			fl::mov(T.r[5]);
			tick(0xB22C, 24);
			T.r[5] = fl::asl(T.r[5]);
			tick(0xB22E, 12);
			T.r[4] = peek((uint16_t)(0x0888 + T.r[5]));
			fl::mov(T.r[4]);
			tick(0xB232, 27);
			T.r[0] = 0;
			fl::clr();
			tick(0xB234, 12);
			T.r[0] |= peekb((uint16_t)(0x04D0 + T.r[3]));
			fl::logicb((uint8_t)T.r[0]);
			tick(0xB238, 27);
			T.r[5] = T.r[3];
			fl::mov(T.r[5]);
			tick(0xB23A, 12);
			T.r[5] = fl::asl(T.r[5]);
			tick(0xB23C, 12);
			fl::tst(T.r[2]);
			tick(0xB23E, 12);                          // TST R2
			if (T.r[2])
			{
				// the pieces from the frame data
				tick(0xB244, 12);                      // BNE
				pokeb((uint16_t)(0x0490 + T.r[3]), fl::logicb((uint8_t)(peekb((uint16_t)(0x0490 + T.r[3])) & ~T.r[2])));
				tick(0xB248, 30);
				poke(0x1400, 0);
				fl::clr();
				tick(0xB24C, 27);
				poke(0x1402, 0x0400);
				fl::mov(0x0400);
				tick(0xB252, 33);
				T.r[1] = peek(peek((uint16_t)(0x03B0 + T.r[5])));
				fl::mov(T.r[1]);
				tick(0xB256, 33);
				T.r[1] = fl::add(T.r[1], 0x4000);
				tick(0xB25A, 18);
				poke(0x1400, 0x3000);
				fl::mov(0x3000);
				tick(0xB260, 33);
				poke(0x1402, 0x3400);
				fl::mov(0x3400);
				tick(0xB266, 33);
				poke(0x0FF8, 0);
				fl::clr();
				tick(0xB26A, 27);
				pokeb(0x0FF9, peekb(T.r[1]++));
				fl::movb(peekb(0x0FF9));
				tick(0xB26E, 33);
				T.r[2] = peek(0x0FF8);
				fl::mov(T.r[2]);
				tick(0xB272, 24);
				T.r[2] = fl::asr(T.r[2]);
				tick(0xB274, 12);
				T.r[2] = fl::asr(T.r[2]);
				tick(0xB276, 12);
				poke((uint16_t)(0x0530 + T.r[5]), T.r[2]);
				fl::mov(T.r[2]);
				tick(0xB27A, 30);
				poke((uint16_t)(0x3804 + T.r[4]), peek((uint16_t)(0x0630 + T.r[5])));
				tick(0xB280, 45);
				poke((uint16_t)(0x3804 + T.r[4]), fl::add(peek((uint16_t)(0x3804 + T.r[4])), T.r[2]));
				tick(0xB284, 30);
				T.r[2] = sext8(peekb(T.r[1]++));
				fl::movb((uint8_t)T.r[2]);
				tick(0xB286, 18);
				poke(0x0FF6, T.r[2]);
				fl::mov(T.r[2]);
				tick(0xB28A, 27);
				T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x0007));
				tick(0xB28E, 18);
				poke(0x0FF4, peek((uint16_t)(0x0670 + T.r[5])));
				fl::mov(peek(0x0FF4));
				tick(0xB294, 42);
				poke(0x0FF4, fl::logic((uint16_t)(peek(0x0FF4) & ~0x0007)));
				tick(0xB29A, 33);
				poke(0x0FF4, fl::logic((uint16_t)(peek(0x0FF4) | T.r[2])));
				tick(0xB29E, 27);
				poke(0x0FF8, peek(0x0FF4));
				fl::mov(peek(0x0FF8));
				tick(0xB2A4, 39);
				const bool rows2 = (int16_t)peek(0x0FF6) < 0;
				fl::tst(peek(0x0FF6));
				tick(0xB2A8, 24);
				if (rows2)
				{
					tick(0xB2AA, 12);
					next_piece_row(0xB2AA);
				}
				else
					tick(0xB2C0, 12);                  // BGE
				pokeb(0x0FFB, peekb((uint16_t)(0x0470 + T.r[3])));
				fl::movb(peekb(0x0FFB));
				tick(0xB2C6, 42);
				T.r[4] = fl::add(T.r[4], 0x3800);
				tick(0xB2CA, 18);
				poke(0x0FF0, 0);
				fl::clr();
				tick(0xB2CE, 27);
				T.r[2] = 0;
				fl::clr();
				tick(0xB2D0, 12);
				for (;;)
				{
					const bool last = (int16_t)peek(0x0FF0) < 0;
					fl::tst(peek(0x0FF0));
					tick(0xB2D4, 24);
					if (last)
					{
						tick(0xB32A, 12);              // BLT
						poke((uint16_t)(T.r[4] + 2), 0);
						fl::clr();
						tick(0xB32E, 30);
						T.r[2] = peek((uint16_t)(T.r[4] + 6));
						fl::mov(T.r[2]);
						tick(0xB332, 27);
					}
					else
					{
						tick(0xB2D6, 12);
						const bool row = (int16_t)peek(0x0FF6) >= 0;
						fl::tst(peek(0x0FF6));
						tick(0xB2DA, 24);
						if (row)
						{
							tick(0xB2DC, 12);
							next_piece_row(0xB2DC);
						}
						else
							tick(0xB2F2, 12);          // BLT
						pokeb(0x0FF2, peekb(T.r[1]++));
						fl::movb(peekb(0x0FF2));
						tick(0xB2F6, 33);
						pokeb(0x0FF3, peekb(T.r[1]++));
						fl::movb(peekb(0x0FF3));
						tick(0xB2FA, 33);
						poke(0x0FF0, peek(0x0FF2));
						fl::mov(peek(0x0FF0));
						tick(0xB300, 39);
						poke(0x0FF2, fl::logic((uint16_t)(peek(0x0FF2) & 0x7FFF)));
						tick(0xB306, 33);
						fl::tst(T.r[2]);
						tick(0xB308, 12);              // TST R2
						if (T.r[2])
						{
							tick(0xB30A, 12);
							poke(0x0FF2, fl::logic((uint16_t)(peek(0x0FF2) | 0x8000)));
							tick(0xB310, 33);
						}
						else
							tick(0xB310, 12);          // BEQ
						poke(T.r[4], peek(0x0FF8));
						T.r[4] = (uint16_t)(T.r[4] + 2);
						fl::mov(peek(0x0FF8));
						tick(0xB314, 33);
						poke(T.r[4], peek(0x0FF2));
						T.r[4] = (uint16_t)(T.r[4] + 2);
						fl::mov(peek(0x0FF2));
						tick(0xB318, 33);
						T.r[4] = fl::add(T.r[4], 2);
						tick(0xB31C, 18);
						T.r[2] = peek(T.r[4]);
						fl::mov(T.r[2]);
						tick(0xB31E, 18);
						T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x0FFF));
						tick(0xB322, 18);
						T.r[2] = fl::logic((uint16_t)(T.r[2] | peek(0x0FFA)));
						tick(0xB326, 24);
						poke(T.r[4], T.r[2]);
						fl::mov(T.r[2]);
						tick(0xB328, 21);
						tick(0xB332, 12);              // BR
					}
					T.r[5] = T.r[2];
					fl::mov(T.r[5]);
					tick(0xB334, 12);
					T.r[5] = fl::logic((uint16_t)(T.r[5] & 0x07F8));
					tick(0xB338, 18);
					T.r[4] = T.r[5];
					fl::mov(T.r[4]);
					tick(0xB33A, 12);
					T.r[4] = fl::add(T.r[4], 0x3800);
					tick(0xB33E, 18);
					T.r[0] = fl::dec(T.r[0]);
					tick(0xB340, 12);
					if (!T.r[0])
					{
						tick(0xB362, 12);              // BEQ
						break;
					}
					tick(0xB342, 12);
					tick(0xB2D0, 12);                  // BR
				}
			}
			else
			{
				// only moved by the screen-y delta
				tick(0xB240, 12);
				tick(0xB344, 18);                      // JMP
				T.r[2] = peek((uint16_t)(0x0630 + T.r[5]));
				fl::mov(T.r[2]);
				tick(0xB348, 27);
				T.r[2] = fl::add(T.r[2], peek((uint16_t)(0x0530 + T.r[5])));
				tick(0xB34C, 27);
				poke((uint16_t)(0x3804 + T.r[4]), T.r[2]);
				fl::mov(T.r[2]);
				tick(0xB350, 30);
				T.r[2] = peek((uint16_t)(0x05F0 + T.r[5]));
				fl::mov(T.r[2]);
				tick(0xB354, 27);
				for (;;)
				{
					poke((uint16_t)(0x3800 + T.r[4]), fl::add(peek((uint16_t)(0x3800 + T.r[4])), T.r[2]));
					tick(0xB358, 30);
					T.r[4] = peek((uint16_t)(0x3806 + T.r[4]));
					fl::mov(T.r[4]);
					tick(0xB35C, 27);
					T.r[4] = fl::logic((uint16_t)(T.r[4] & 0x07F8));
					tick(0xB360, 18);
					if (!--T.r[0]) break;
					tick(0xB354, 18);                  // SOB
				}
				tick(0xB362, 18);
			}
		}
		// the next object of the list: 0x0808[0x0FEE]
		push(peek(0x0FEE));
		fl::mov(peek(T.r[6]));
		tick(0xB366, 36);
		poke(T.r[6], fl::add(peek(T.r[6]), 0x0808));
		tick(0xB36A, 27);
		T.r[5] = sext8(peekb(pop()));
		fl::movb((uint8_t)T.r[5]);
		tick(0xB36C, 24);
		poke(0x0FEE, T.r[5]);
		fl::mov(T.r[5]);
		tick(0xB370, 27);
		tick(0xB1FA, 18);                              // JMP
		link = T.r[5];
	}
	tick(0xB1FC, 12);
	tick(0xB374, 18);                                  // JMP
	fl::tst(pop());
	tick(0xB376, 18);
	rts();
}

// F:B378 (assembly): move the objects 0x0994 down to 0: animation (rate 0x0770 frames per step,
// counter 0x0790, step count 0x07B0; the frame list at 0x03B0 ends with a negative link; with
// flag 0x01 the frame data in bank 0x3000/0x3400 gives the velocity), position += velocity
// for slots from 4 on, then screen position and screen-y delta.  The two stack words it
// reserves end compared, as the original does.
// a velocity byte of the frame data (at pc: MOVB (R0)+,-(SP) ... ASL Rk): through a stack word
// sign-extended and times 4, then times 4 again in register k
void frame_velocity(uint16_t pc, int k)
{
	T.r[6] = (uint16_t)(T.r[6] - 2);
	pokeb(T.r[6], peekb(T.r[0]++));
	tick((uint16_t)(pc + 2), 30);
	const bool neg = (peekb(T.r[6]) & 0x80) != 0;
	tick((uint16_t)(pc + 4), 18);                      // TSTB (SP)
	poke((uint16_t)(T.r[6] + 4), neg ? 0xFFFF : 0);
	tick((uint16_t)(pc + 8), 30);                      // SXT 4(SP)
	const uint8_t b = peekb(T.r[6]);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	pokeb((uint16_t)(T.r[6] + 2), b);
	tick((uint16_t)(pc + 0x0C), 36);                   // MOVB (SP)+,2(SP)
	poke((uint16_t)(T.r[6] + 2), (uint16_t)(peek((uint16_t)(T.r[6] + 2)) << 1));
	tick((uint16_t)(pc + 0x10), 30);
	poke((uint16_t)(T.r[6] + 2), (uint16_t)(peek((uint16_t)(T.r[6] + 2)) << 1));
	tick((uint16_t)(pc + 0x14), 30);
	T.r[k] = peek((uint16_t)(T.r[6] + 2));
	tick((uint16_t)(pc + 0x18), 27);
	T.r[k] = (uint16_t)(T.r[k] << 1);
	tick((uint16_t)(pc + 0x1A), 12);
	T.r[k] = (uint16_t)(T.r[k] << 1);
	tick((uint16_t)(pc + 0x1C), 12);
}

void move_objects()
{
	T.r[6] = (uint16_t)(T.r[6] - 4);                    // CMP -(SP),-(SP): the rate pointer, scratch
	fl::cmp(peek((uint16_t)(T.r[6] + 2)), peek(T.r[6]));
	tick(0xB37A, 30);
	const uint16_t at_rate = T.r[6];
	T.r[5] = sext8(peekb(0x0994));
	fl::movb((uint8_t)T.r[5]);
	tick(0xB37E, 24);
	if ((int16_t)T.r[5] < 0)
	{
		tick(0xB380, 12);
		tick(0xB4B2, 18);                              // JMP
	}
	else
	{
		tick(0xB384, 12);                              // BGE
		for (;;)
		{
			const uint16_t i = T.r[5];
			const bool used = peekb((uint16_t)(0x0370 + i)) != 0;
			fl::tstb(peekb((uint16_t)(T.r[5] + 0x0370)));
			tick(0xB388, 27);
			if (!used)
			{
				tick(0xB38A, 12);
				tick(0xB4AA, 18);                      // JMP
			}
			else
			{
				tick(0xB38E, 12);
				T.r[4] = T.r[5];
				fl::mov(T.r[4]);
				tick(0xB390, 12);
				T.r[4] = fl::asl(T.r[4]);
				tick(0xB392, 12);
				const uint16_t w = T.r[4];
				T.r[3] = peek((uint16_t)(0x04F0 + w));
				fl::mov(T.r[3]);
				tick(0xB396, 27);
				T.r[2] = peek((uint16_t)(0x0570 + w));
				fl::mov(T.r[2]);
				tick(0xB39A, 27);
				poke(at_rate, 0x0770);
				fl::mov(0x0770);
				tick(0xB39E, 27);
				poke(at_rate, fl::add(peek(at_rate), T.r[5]));
				tick(0xB3A0, 21);
				const uint8_t rate = peekb(peek(at_rate));
				tick(0xB3A4, 33);                      // TSTB @0(SP)
				bool animated = false;
				if (!rate)
					tick(0xB45E, 12);                  // BEQ
				else
				{
					tick(0xB3A6, 12);
					if (rate & 0x80)
						tick(0xB45E, 12);              // BMI
					else
					{
						tick(0xB3A8, 12);
						animated = true;
					}
				}
				if (animated)
				{
					T.r[0] = 0x0790;
					fl::mov(T.r[0]);
					tick(0xB3AC, 18);
					T.r[0] = fl::add(T.r[0], T.r[5]);
					tick(0xB3AE, 12);
					const bool counting = peekb(T.r[0]) != 0;
					fl::tstb(peekb(T.r[0]));
					tick(0xB3B0, 18);
					if (counting)
					{
						tick(0xB45C, 12);              // BNE
						pokeb(T.r[0], fl::decb(peekb(T.r[0])));
						tick(0xB45E, 21);
					}
					else
					{
						// the next step of the animation
						tick(0xB3B2, 12);
						pokeb(T.r[0], peekb(peek(at_rate)));
						tick(0xB3B6, 42);
						T.r[1] = peek((uint16_t)(0x03B0 + w));
						fl::mov(T.r[1]);
						tick(0xB3BA, 27);
						T.r[1] = fl::add(T.r[1], 2);
						tick(0xB3BE, 18);
						pokeb((uint16_t)(0x07B0 + i), fl::incb(peekb((uint16_t)(0x07B0 + i))));
						tick(0xB3C2, 30);
						const bool end = (int16_t)peek(T.r[1]) < 0;
						fl::tst(peek(T.r[1]));
						tick(0xB3C4, 18);
						if (end)
						{
							tick(0xB3C6, 12);
							T.r[1] = peek(T.r[1]);
							fl::mov(T.r[1]);
							tick(0xB3C8, 18);
							T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x7FFF));
							tick(0xB3CC, 18);
							pokeb((uint16_t)(0x07B0 + i), 0);
							fl::clr();
							tick(0xB3D0, 30);
							const bool once = (peekb((uint16_t)(0x04B0 + i)) & 0x20) != 0;
							fl::logicb((uint8_t)(0x20 & peekb((uint16_t)(T.r[5] + 0x04B0))));
							tick(0xB3D6, 33);
							if (once)
							{
								tick(0xB3D8, 12);
								pokeb(peek(at_rate), 0);
								fl::clr();
								tick(0xB3DC, 36);
							}
							else
								tick(0xB3DC, 12);      // BEQ
						}
						else
							tick(0xB3DC, 12);          // BGE
						const bool moves = (peekb((uint16_t)(0x0490 + i)) & 0x01) != 0;
						fl::logicb((uint8_t)(0x01 & peekb((uint16_t)(T.r[5] + 0x0490))));
						tick(0xB3E2, 33);
						if (moves)
						{
							// the velocity from the frame data (bank 0x3000 / 0x3400)
							tick(0xB3E4, 12);
							T.r[0] = peek(T.r[1]);
							fl::mov(T.r[0]);
							tick(0xB3E6, 18);
							T.r[0] = fl::add(T.r[0], 0x3FFD);
							tick(0xB3EA, 18);
							poke(0x1400, 0x3000);
							fl::mov(0x3000);
							tick(0xB3F0, 33);
							poke(0x1402, 0x3400);
							fl::mov(0x3400);
							tick(0xB3F6, 33);
							frame_velocity(0xB3F6, 3);
							frame_velocity(0xB412, 2);
							const bool step = peekb(T.r[0]) != 0;
							fl::tstb(peekb(T.r[0]));
							tick(0xB430, 18);
							if (step)
							{
								tick(0xB432, 12);
								T.r[6] = (uint16_t)(T.r[6] - 2);
								pokeb(T.r[6], peekb(T.r[0]));
								fl::movb(peekb(T.r[6]));
								tick(0xB434, 30);
								pokeb((uint16_t)(T.r[6] + 1), 0);
								fl::clr();
								tick(0xB438, 30);
								const uint16_t v = pop();
								poke((uint16_t)(0x0430 + w), fl::sub(peek((uint16_t)(0x0430 + w)), v));
								tick(0xB43C, 36);
								T.r[0] = 0;
								fl::clr();
								tick(0xB43E, 12);
								T.r[0] |= peekb(peek(T.r[6]));
								fl::logicb((uint8_t)T.r[0]);
								tick(0xB442, 33);
								poke((uint16_t)(0x0430 + w), fl::add(peek((uint16_t)(0x0430 + w)), T.r[0]));
								tick(0xB446, 30);
							}
							else
								tick(0xB446, 12);      // BEQ
							poke(0x1400, 0);
							fl::clr();
							tick(0xB44A, 27);
							poke(0x1402, 0x0400);
							fl::mov(0x0400);
							tick(0xB450, 33);
						}
						else
							tick(0xB450, 12);          // BEQ
						poke((uint16_t)(0x03B0 + w), T.r[1]);
						fl::mov(T.r[1]);
						tick(0xB454, 30);
						pokeb((uint16_t)(0x0490 + i), fl::logicb((uint8_t)(peekb((uint16_t)(0x0490 + i)) | 0x18)));
						tick(0xB45A, 36);
						tick(0xB45E, 12);              // BR
					}
				}
				// position (from slot 4 on) and screen position
				const bool fixed = (int16_t)T.r[5] < 4;
				fl::cmp(T.r[5], 0x0004);
				tick(0xB462, 18);
				if (fixed)
				{
					tick(0xB464, 12);
					pokeb((uint16_t)(0x0490 + i), fl::logicb((uint8_t)(peekb((uint16_t)(0x0490 + i)) | 0x18)));
					tick(0xB46A, 36);
					tick(0xB47A, 12);                  // BR
				}
				else
				{
					tick(0xB46C, 12);                  // BGE
					poke((uint16_t)(0x06F0 + w), fl::add(peek((uint16_t)(0x06F0 + w)), T.r[3]));
					tick(0xB470, 30);
					poke((uint16_t)(0x0730 + w), fl::add(peek((uint16_t)(0x0730 + w)), T.r[2]));
					tick(0xB474, 30);
					poke((uint16_t)(0x06B0 + w), fl::add(peek((uint16_t)(0x06B0 + w)), peek((uint16_t)(0x05B0 + w))));
					tick(0xB47A, 45);
				}
				T.r[3] = peek((uint16_t)(0x06F0 + w));
				fl::mov(T.r[3]);
				tick(0xB47E, 27);
				T.r[3] = fl::sub(T.r[3], peek(0x01A8));
				tick(0xB482, 24);
				poke((uint16_t)(0x0630 + w), T.r[3]);
				fl::mov(T.r[3]);
				tick(0xB486, 30);
				T.r[2] = peek((uint16_t)(0x0730 + w));
				fl::mov(T.r[2]);
				tick(0xB48A, 27);
				T.r[2] = fl::add(T.r[2], peek(0x01AA));
				tick(0xB48E, 24);
				T.r[2] = fl::add(T.r[2], peek((uint16_t)(0x06B0 + w)));
				tick(0xB492, 27);
				T.r[0] = T.r[2];
				fl::mov(T.r[0]);
				tick(0xB494, 12);
				T.r[0] = fl::logic((uint16_t)(T.r[0] & ~0x0007));
				tick(0xB498, 18);
				T.r[1] = peek((uint16_t)(0x0670 + w));
				fl::mov(T.r[1]);
				tick(0xB49C, 27);
				T.r[1] = fl::logic((uint16_t)(T.r[1] & ~0x0007));
				tick(0xB4A0, 18);
				T.r[0] = fl::sub(T.r[0], T.r[1]);
				tick(0xB4A2, 12);
				poke((uint16_t)(0x05F0 + w), T.r[0]);
				fl::mov(T.r[0]);
				tick(0xB4A6, 30);
				poke((uint16_t)(0x0670 + w), T.r[2]);
				fl::mov(T.r[2]);
				tick(0xB4AA, 30);
			}
			T.r[5] = fl::dec(T.r[5]);
			tick(0xB4AC, 12);
			if (lt())
			{
				tick(0xB4B2, 12);                      // BLT
				break;
			}
			tick(0xB4AE, 12);
			tick(0xB384, 18);                          // JMP
		}
	}
	const uint16_t a = pop(), b = pop();
	fl::cmp(a, b);                                     // CMP (SP)+,(SP)+
	tick(0xB4B4, 24);
	rts();
}

// ---------------------------------------------------------------- objects

// F:B4B6  show_object(int slot): make slot (bit 7: without sprite pieces) visible: the highest
// used slot 0x0994 rises to it (or drops to the highest slot still in use), the piece count
// 0x04D0 comes from the type table 0x5C96, the screen position from the world position and the
// scroll; flag 0x80 of 0x0490.  0x0FFC = 0xFF when interrupts are enabled.  Returns slot * 2.
uint16_t show_object(const Args &)
{
	c_save(3, 0xB4BA);
	T.r[2] = peek(0x01D2);
	fl::mov(T.r[2]);
	tick(0xB4BE, 24);
	T.r[3] = peek(0x01D4);
	fl::mov(T.r[3]);
	tick(0xB4C2, 24);
	poke(0x01D2, 0);
	fl::clr();
	tick(0xB4C6, 27);
	poke(0x1400, 0);
	fl::clr();
	tick(0xB4CA, 27);
	T.r[0] = 0x0400;
	fl::mov(T.r[0]);
	tick(0xB4CE, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xB4D2, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xB4D6, 27);
	T.r[1] = peek((uint16_t)(T.r[6] + 0x0C));
	fl::mov(T.r[1]);
	tick(0xB4DA, 27);
	T.r[1] = fl::logic((uint16_t)(T.r[1] & ~0x0080));
	tick(0xB4DE, 18);
	T.r[0] = sext8(peekb(0x0994));
	fl::movb((uint8_t)T.r[0]);
	tick(0xB4E2, 24);
	const bool lower = (int16_t)T.r[1] < (int16_t)T.r[0];
	fl::cmp(T.r[1], T.r[0]);
	tick(0xB4E4, 12);
	if (!lower)
	{
		// the highest slot in use rises to this one
		tick(0xB4E6, 12);
		pokeb(0x0994, (uint8_t)T.r[1]);
		fl::movb((uint8_t)T.r[1]);
		tick(0xB4EA, 27);
		tick(0xB50A, 12);                              // BR
	}
	else
	{
		// or drops to the highest slot still in use
		tick(0xB4EC, 12);                              // BLT
		T.r[0] = sext8(peekb(0x0994));
		fl::movb((uint8_t)T.r[0]);
		tick(0xB4F0, 24);
		if ((int16_t)T.r[0] < 0)
			tick(0xB50A, 12);                          // BLT
		else
		{
			tick(0xB4F2, 12);
			for (;;)
			{
				const bool used = peekb((uint16_t)(0x0370 + T.r[0])) != 0;
				fl::tstb(peekb((uint16_t)(T.r[0] + 0x0370)));
				tick(0xB4F6, 27);
				bool top = true;
				if (used)
					tick(0xB500, 12);                  // BNE
				else
				{
					tick(0xB4F8, 12);
					top = (peekb((uint16_t)(0x0490 + T.r[0])) & 0x86) != 0;
					fl::logicb((uint8_t)(0x86 & peekb((uint16_t)(T.r[0] + 0x0490))));
					tick(0xB4FE, 33);
					tick(top ? 0xB500 : 0xB506, 12);   // BEQ
				}
				if (top)
				{
					pokeb(0x0994, (uint8_t)T.r[0]);
					tick(0xB504, 27);
					tick(0xB50A, 12);                  // BR
					break;
				}
				T.r[0] = fl::dec(T.r[0]);
				tick(0xB508, 12);
				if (lt())
				{
					tick(0xB50A, 12);
					break;
				}
				tick(0xB4F2, 12);                      // BGE
			}
		}
	}
	const bool pieces = peek((uint16_t)(T.r[6] + 0x0C)) == T.r[1];
	fl::cmp(peek((uint16_t)(T.r[6] + 0x0C)), T.r[1]);
	tick(0xB50E, 27);
	if (pieces)
	{
		tick(0xB510, 12);
		T.r[0] = 0;
		fl::clr();
		tick(0xB512, 12);
		T.r[0] |= peekb((uint16_t)(0x0370 + T.r[1]));
		fl::logicb((uint8_t)T.r[0]);
		tick(0xB516, 27);
		pokeb((uint16_t)(0x04D0 + T.r[1]), peekb((uint16_t)(0x5C96 + T.r[0])));
		tick(0xB51C, 45);
		tick(0xB522, 12);                              // BR
	}
	else
	{
		tick(0xB51E, 12);                              // BNE
		pokeb((uint16_t)(0x04D0 + T.r[1]), 0);
		fl::clr();
		tick(0xB522, 30);
	}
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0xB524, 12);
	T.r[0] = fl::asl(T.r[0]);
	tick(0xB526, 12);
	T.r[4] = peek((uint16_t)(0x06F0 + T.r[0]));
	fl::mov(T.r[4]);
	tick(0xB52A, 27);
	T.r[4] = fl::sub(T.r[4], peek(0x01A8));
	tick(0xB52E, 24);
	poke((uint16_t)(0x0630 + T.r[0]), T.r[4]);
	fl::mov(T.r[4]);
	tick(0xB532, 30);
	poke((uint16_t)(0x0670 + T.r[0]), peek(0x01AA));
	fl::mov(peek(0x01AA));
	tick(0xB538, 42);
	poke((uint16_t)(0x0670 + T.r[0]), fl::add(peek((uint16_t)(0x0670 + T.r[0])), peek((uint16_t)(0x0730 + T.r[0]))));
	tick(0xB53E, 45);
	poke((uint16_t)(0x05F0 + T.r[0]), 0);
	fl::mov(0x0000);
	tick(0xB544, 36);
	pokeb((uint16_t)(0x0490 + T.r[1]), fl_logicb(peekb((uint16_t)(0x0490 + T.r[1])) | 0x80));
	tick(0xB54A, 36);
	T.r[1] = sext8(T.psw);                             // MFPS R1
	tick(0xB54C, 12);
	const bool masked = (T.r[1] & 0x00E0) != 0;
	fl::logic((uint16_t)(0x00E0 & T.r[1]));
	tick(0xB550, 18);
	if (!masked)
	{
		// interrupts on: the sprite lists are rebuilt
		tick(0xB552, 12);
		pokeb(0x0FFC, 0xFF);
		fl::movb(0xFF);
		tick(0xB558, 33);
	}
	else
		tick(0xB558, 12);                              // BNE
	poke(0x01D2, T.r[2]);
	fl::mov(T.r[2]);
	tick(0xB55C, 27);
	poke(0x01D4, T.r[3]);
	fl::mov(T.r[3]);
	tick(0xB560, 27);
	poke(0x1400, peek(0x01D2));
	fl::mov(peek(0x01D2));
	tick(0xB566, 39);
	poke(0x1402, peek(0x01D4));
	fl::mov(peek(0x01D4));
	tick(0xB56C, 39);
	c_restore(3);
	return T.r[0];
}

// F:B602  queue_piece_count(int type, int slot): with interrupts off, put slot and its piece
// count (type table 0x5C96) in the first free entry of the queue F:B03C applies (0x0989+i,
// i = 4..1), flag the slot (0x60) and count the entry (0x0992).  Interrupts end enabled.
uint16_t queue_piece_count(const Args &)
{
	c_save(4, 0xB606);
	T.r[4] = peek(0x01D2);
	fl::mov(T.r[4]);
	tick(0xB60A, 24);
	T.r[5] = peek(0x01D4);
	fl::mov(T.r[5]);
	tick(0xB60E, 24);
	poke(0x01D2, 0);
	fl::clr();
	tick(0xB612, 27);
	poke(0x1400, 0);
	fl::clr();
	tick(0xB616, 27);
	T.r[0] = 0x0400;
	fl::mov(T.r[0]);
	tick(0xB61A, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xB61E, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xB622, 27);
	T.r[3] = 0x00E0;
	fl::mov(T.r[3]);
	tick(0xB626, 18);
	mtps((uint8_t)T.r[3]);                             // interrupts off
	tick(0xB628, 24);
	T.r[2] = 0x0004;
	fl::mov(T.r[2]);
	tick(0xB62C, 18);
	for (;;)
	{
		const bool taken = peekb((uint16_t)(0x0989 + T.r[2])) != 0;
		fl::tstb(peekb((uint16_t)(T.r[2] + 0x0989)));
		tick(0xB630, 27);
		if (!taken)
		{
			tick(0xB632, 12);
			T.r[0] = peek((uint16_t)(T.r[6] + 0x10));
			fl::mov(T.r[0]);
			tick(0xB636, 27);
			pokeb((uint16_t)(0x0989 + T.r[2]), (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0xB63A, 30);
			T.r[1] = peek((uint16_t)(T.r[6] + 0x0E));
			fl::mov(T.r[1]);
			tick(0xB63E, 27);
			pokeb((uint16_t)(0x098D + T.r[2]), peekb((uint16_t)(0x5C96 + T.r[1])));
			tick(0xB644, 45);
			pokeb((uint16_t)(0x0490 + T.r[0]), fl::logicb((uint8_t)(peekb((uint16_t)(0x0490 + T.r[0])) | 0x60)));
			tick(0xB64A, 36);
			pokeb(0x0992, fl::incb(peekb(0x0992)));
			tick(0xB64E, 27);
			tick(0xB652, 12);                          // BR
			break;
		}
		tick(0xB650, 12);                              // BNE
		if (!--T.r[2])
		{
			tick(0xB652, 18);
			break;
		}
		tick(0xB62C, 18);                              // SOB
	}
	T.r[3] = 0;
	fl::clr();
	tick(0xB654, 12);
	mtps(0);                                           // and on again
	tick(0xB658, 30);
	poke(0x01D2, T.r[4]);
	fl::mov(T.r[4]);
	tick(0xB65C, 27);
	poke(0x01D4, T.r[5]);
	fl::mov(T.r[5]);
	tick(0xB660, 27);
	poke(0x1400, peek(0x01D2));
	fl::mov(peek(0x01D2));
	tick(0xB666, 39);
	poke(0x1402, peek(0x01D4));
	fl::mov(peek(0x01D4));
	tick(0xB66C, 39);
	c_restore(4);
	return T.r[0];
}

// F:BA2E: with interrupts off, clear the text layer (F:8D3E) and the object tables
// 0x0370-0x0994, park the last sprite (0x3BFE = 0x03F8) and empty both display lists
// (0x0988/0x0989 = 0xFF).  Interrupts end enabled.  Returns 0x0995.
uint16_t clear_objects_and_text(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xBA30, 24);
	T.r[1] = 0x00E0;
	fl::mov(T.r[1]);
	tick(0xBA34, 18);
	mtps((uint8_t)T.r[1]);                             // interrupts off
	tick(0xBA36, 24);
	jsr(0xBA3A, 0x8068);
	jsr(0xBA3E, 0x8D3E);
	T.r[0] = 0x0370;
	fl::mov(T.r[0]);
	tick(0xBA42, 18);
	for (;;)
	{
		pokeb(T.r[0]++, 0);
		fl::clr();
		tick(0xBA44, 21);
		fl::cmp(T.r[0], 0x0995);
		tick(0xBA48, 18);                              // CMP R0,#0x0995
		if (T.r[0] == 0x0995) break;
		tick(0xBA42, 12);                              // BNE
	}
	tick(0xBA4A, 12);
	poke(0x3BFE, 0x03F8);
	fl::mov(0x03F8);
	tick(0xBA50, 33);
	poke(0x0988, 0xFFFF);
	fl::mov(0xFFFF);
	tick(0xBA56, 33);
	mtps(0);                                           // and on again
	tick(0xBA5A, 30);
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0xBA5C, 18);
	rts();
	return T.r[0];
}

// F:C98E  restart_object_animation(int rate, int n): the object of map object n (0x09B8, the
// argument is replaced by the slot) restarts its animation: next frame of the list 0x03B0 (a
// negative link is followed), redraw flags 0x18, rate and counter (0x0770, 0x0790) = rate.
// Not for the object queued in 0x0E6A.  Returns the slot.
uint16_t restart_object_animation(const Args &)
{
	c_save(3, 0xC992);
	T.r[3] = peek(0x01D2);                             // the banks, put back at the end
	fl::mov(T.r[3]);
	tick(0xC996, 24);
	T.r[4] = peek(0x01D4);
	fl::mov(T.r[4]);
	tick(0xC99A, 24);
	poke(0x01D2, 0);
	fl::clr();
	tick(0xC99E, 27);
	poke(0x1400, 0);
	fl::clr();
	tick(0xC9A2, 27);
	T.r[0] = 0x0400;
	fl::mov(T.r[0]);
	tick(0xC9A6, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xC9AA, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xC9AE, 27);
	const uint16_t arg_n = sp_at(0x0E), arg_rate = sp_at(0x0C);
	T.r[0] = peek(arg_n);                              // the map object
	fl::mov(T.r[0]);
	tick(0xC9B2, 27);
	const uint8_t slot8 = peekb((uint16_t)(0x09B8 + T.r[0]));
	pokeb(arg_n, slot8);                               // the argument becomes its slot
	fl::movb(slot8);
	tick(0xC9B8, 45);
	poke(arg_n, fl_logic(peek(arg_n) & 0x003F));
	tick(0xC9BE, 36);
	T.r[0] = peek(arg_n);
	fl::mov(T.r[0]);
	tick(0xC9C2, 27);
	T.r[1] = 0;
	fl::clr();
	tick(0xC9C4, 12);
	T.r[1] = fl_logicb(peekb(0x0E6A));
	tick(0xC9C8, 24);
	fl::cmp(T.r[1], T.r[0]);
	tick(0xC9CA, 12);
	if (T.r[1] == T.r[0])
		tick(0xC9F6, 12);                              // BEQ: the queued object is left alone
	else
	{
		tick(0xC9CC, 12);
		T.r[1] = T.r[0];
		fl::mov(T.r[1]);
		tick(0xC9CE, 12);
		T.r[1] = fl_asl(T.r[1]);
		tick(0xC9D0, 12);
		T.r[1] = fl::add(T.r[1], 0x03B0);              // its animation pointer
		tick(0xC9D4, 18);
		poke(T.r[1], fl::add(peek(T.r[1]), 0x0002));   // the next word of the list
		tick(0xC9D8, 27);
		T.r[2] = peek(peek(T.r[1]));
		fl::mov(T.r[2]);
		tick(0xC9DC, 33);
		if ((int16_t)T.r[2] >= 0)
			tick(0xC9E4, 12);                          // BGE: a frame
		else
		{
			tick(0xC9DE, 12);
			poke(T.r[1], T.r[2]);                      // a link: follow it
			fl::mov(T.r[2]);
			tick(0xC9E0, 21);
			poke(T.r[1], fl_logic(peek(T.r[1]) & 0x7FFF));
			tick(0xC9E4, 27);
		}
		const uint16_t flags = (uint16_t)(0x0490 + T.r[0]);
		pokeb(flags, fl_logicb((uint8_t)(peekb(flags) | 0x18)));   // redraw it
		tick(0xC9EA, 36);
		T.r[2] = peek(arg_rate);
		fl::mov(T.r[2]);
		tick(0xC9EE, 27);
		pokeb((uint16_t)(0x0790 + T.r[0]), (uint8_t)T.r[2]);
		fl::movb((uint8_t)T.r[2]);
		tick(0xC9F2, 30);
		pokeb((uint16_t)(0x0770 + T.r[0]), (uint8_t)T.r[2]);
		fl::movb((uint8_t)T.r[2]);
		tick(0xC9F6, 30);
	}
	poke(0x01D2, T.r[3]);
	fl::mov(T.r[3]);
	tick(0xC9FA, 27);
	poke(0x01D4, T.r[4]);
	fl::mov(T.r[4]);
	tick(0xC9FE, 27);
	{
		const uint16_t v = peek(0x01D2);
		fl::mov(v);
		poke(0x1400, v);
	}
	tick(0xCA04, 39);
	{
		const uint16_t v = peek(0x01D4);
		fl::mov(v);
		poke(0x1402, v);
	}
	tick(0xCA0A, 39);
	c_restore(3);
	return T.r[0];
}

// ---------------------------------------------------------------- map objects
//
// Map object n (0..0x3F placed from the map, 0x40.. dynamic) has a descriptor word 0x0AF8+2n:
// bit 15 = has an object slot, bits 9-14 = offset, bits 0-8 = map cell (column = bits 0-3).
// The cell's entry in 0x7A00 (0x7D00 for n >= 0x40, bank B 0x1400) plus the offset gives the
// definition 0x09B6 (+0x0338 for n >= 0x40) with its record 0x6040 + 8 * definition (0x09B2) and
// its properties 0x4560 (bank A 0x1000); 0x09B8+n is the object slot (bits 6-7: off screen).

// F:BE10: definition (0x09B6) and record (0x09B2, for n < 0x40) of the current map object
// 0x09AA.  Returns the address of its cell entry.
// the cell entry of a map word (R1 here; R0 = (cell & 0x1FF) * 2, R3 = 1 for n >= 0x40):
// 0x7A00 + 0x0300 for the dynamic ones (pc: CMP @#0x09AA,#0x40), the entry address in `entry`
void cell_entry(uint16_t pc, int entry)
{
	T.r[3] = 0;
	fl::clr();
	tick(pc, 12);                                      // CLR R3
	fl::cmp(peek(0x09AA), 0x0040);
	tick((uint16_t)(pc + 6), 30);
	if (!lt())
	{
		tick((uint16_t)(pc + 8), 12);
		T.r[3] = fl::inc(T.r[3]);
		tick((uint16_t)(pc + 0x0A), 12);
		T.r[2] = 0x0180;
		tick((uint16_t)(pc + 0x0E), 18);
		T.r[2] = fl_asl(T.r[2]);
		tick((uint16_t)(pc + 0x10), 12);
		T.r[2] = fl::add(T.r[2], 0x7A00);
		tick((uint16_t)(pc + 0x14), 18);
		T.r[2] = fl::add(T.r[2], T.r[entry]);
		tick((uint16_t)(pc + 0x16), 12);
		T.r[entry] = T.r[2];
		fl::mov(T.r[entry]);
		tick((uint16_t)(pc + 0x18), 12);
		tick((uint16_t)(pc + 0x1E), 12);               // BR
	}
	else
	{
		tick((uint16_t)(pc + 0x1A), 12);               // BLT
		T.r[entry] = fl::add(T.r[entry], 0x7A00);
		tick((uint16_t)(pc + 0x1E), 18);
	}
}

// the definition offset of map word w: bits 9-15 (ASR, CLRB, SWAB, MOVB) into R2 at pc
void cell_offset(uint16_t pc, uint16_t w)
{
	T.r[2] = w;
	tick((uint16_t)(pc + 2), 12);
	T.r[2] = asr(T.r[2], 1);
	tick((uint16_t)(pc + 4), 12);
	T.r[2] &= 0xFF00;
	tick((uint16_t)(pc + 6), 12);
	T.r[2] = (uint16_t)((T.r[2] << 8) | (T.r[2] >> 8));
	tick((uint16_t)(pc + 8), 12);
	T.r[2] = sext8((uint8_t)T.r[2]);
	tick((uint16_t)(pc + 0x0A), 12);
}

uint16_t locate_map_object(const Args &)
{
	c_save(2, 0xBE14);
	T.r[0] = peek(0x09AA);
	fl::mov(T.r[0]);
	tick(0xBE18, 24);
	T.r[0] = fl::asl(T.r[0]);
	tick(0xBE1A, 12);
	T.r[1] = peek((uint16_t)(0x0AF8 + T.r[0]));
	fl::mov(T.r[1]);
	tick(0xBE1E, 27);
	T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x7FFF));
	tick(0xBE22, 18);
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0xBE24, 12);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x01FF));
	tick(0xBE28, 18);
	T.r[0] = fl::asl(T.r[0]);
	tick(0xBE2A, 12);
	cell_entry(0xBE2C, 0);
	cell_offset(0xBE4A, T.r[1]);
	T.r[2] = fl::add(T.r[2], peek(T.r[0]));
	tick(0xBE56, 18);
	T.r[1] = T.r[2];
	fl::mov(T.r[1]);
	tick(0xBE58, 12);
	T.r[3] = fl_ror(T.r[3]);
	tick(0xBE5A, 12);
	if (fC())
	{
		// dynamic: definition + 0x0338, no record
		tick(0xBE5C, 12);
		poke(0x09B6, T.r[1]);
		fl::mov(T.r[1]);
		tick(0xBE60, 27);
		poke(0x09B6, fl::add(peek(0x09B6), 0x0338));
		tick(0xBE66, 33);
	}
	else
	{
		tick(0xBE68, 12);                              // BCC
		poke(0x09B6, T.r[1]);
		fl::mov(T.r[1]);
		tick(0xBE6C, 27);
		T.r[1] = fl::asl(T.r[1]);
		tick(0xBE6E, 12);
		T.r[1] = fl::asl(T.r[1]);
		tick(0xBE70, 12);
		T.r[1] = fl::asl(T.r[1]);
		tick(0xBE72, 12);
		T.r[1] = fl::add(T.r[1], 0x6040);
		tick(0xBE76, 18);
		poke(0x09B2, T.r[1]);
		fl::mov(T.r[1]);
		tick(0xBE7A, 27);
	}
	c_restore(2);
	return T.r[0];
}

// F:BE7C: state of the map object whose descriptor 0x09B2 points to: negative when its cell has
// no definition; otherwise (definition recorded in 0x09B4) for a definition with a group
// (properties bits 0-3) 1 when another map object holds the group (0x0E6D+group), else 0; for
// the others the definition's bit in the bitmap 0x0CF6 (taken).
uint16_t map_object_state(const Args &)
{
	c_save(2, 0xBE80);
	T.r[0] = peek(peek(0x09B2));
	fl::mov(T.r[0]);
	tick(0xBE84, 33);
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0xBE86, 12);
	T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x01FF));
	tick(0xBE8A, 18);
	T.r[1] = fl::asl(T.r[1]);
	tick(0xBE8C, 12);
	cell_entry(0xBE8E, 1);
	T.r[1] = peek(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xBEAE, 18);
	if (lt())
		tick(0xBF14, 12);                              // BLT: no definition
	else
	{
		tick(0xBEB0, 12);
		cell_offset(0xBEB0, T.r[0]);
		T.r[2] = fl::add(T.r[2], T.r[1]);
		tick(0xBEBC, 12);
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0xBEBE, 12);
		T.r[3] = fl_ror(T.r[3]);
		tick(0xBEC0, 12);
		if (fC())
		{
			tick(0xBEC2, 12);
			T.r[0] = fl::add(T.r[0], 0x0338);
			tick(0xBEC6, 18);
		}
		else
			tick(0xBEC6, 12);                          // BCC
		poke(0x09B4, T.r[0]);
		fl::mov(T.r[0]);
		tick(0xBECA, 27);
		const bool grouped = (peekb((uint16_t)(0x4560 + T.r[0])) & 0x0F) != 0;
		fl::logicb((uint8_t)(0x0F & peekb((uint16_t)(T.r[0] + 0x4560))));
		tick(0xBED0, 33);
		if (grouped)
		{
			// 1 when another map object holds the group
			tick(0xBED2, 12);
			T.r[1] = sext8(peekb((uint16_t)(0x4560 + T.r[0])));
			fl::movb((uint8_t)T.r[1]);
			tick(0xBED6, 27);
			T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x000F));
			tick(0xBEDA, 18);
			T.r[2] = 0;
			fl::clr();
			tick(0xBEDC, 12);
			T.r[2] |= peekb((uint16_t)(0x0E6D + T.r[1]));
			fl::logicb((uint8_t)T.r[2]);
			tick(0xBEE0, 27);
			fl::tst(T.r[2]);
			tick(0xBEE2, 12);
			bool free = true;
			if (fZ())
				tick(0xBEEE, 12);                      // BEQ
			else
			{
				tick(0xBEE4, 12);
				T.r[0] = peek(0x09AA);
				fl::mov(T.r[0]);
				tick(0xBEE8, 24);
				T.r[0] = fl::inc(T.r[0]);
				tick(0xBEEA, 12);
				fl::cmp(T.r[2], T.r[0]);
				tick(0xBEEC, 12);
				free = fZ();
				tick(free ? 0xBEEE : 0xBEF2, 12);      // BNE
			}
			if (free)
			{
				T.r[1] = 0;
				fl::clr();
				tick(0xBEF0, 12);
			}
			else
			{
				T.r[1] = 0x0001;
				fl::mov(T.r[1]);
				tick(0xBEF6, 18);
			}
			tick(0xBF14, 12);                          // BR
		}
		else
		{
			// the definition's bit in the bitmap 0x0CF6 (taken)
			tick(0xBEF8, 12);                          // BEQ
			T.r[1] = T.r[0];
			fl::mov(T.r[1]);
			tick(0xBEFA, 12);
			T.r[1] = fl::asr(T.r[1]);
			tick(0xBEFC, 12);
			T.r[1] = fl::asr(T.r[1]);
			tick(0xBEFE, 12);
			T.r[1] = fl::asr(T.r[1]);
			tick(0xBF00, 12);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0007));
			tick(0xBF04, 18);
			T.r[1] = sext8(peekb((uint16_t)(0x0CF6 + T.r[1])));
			fl::movb((uint8_t)T.r[1]);
			tick(0xBF08, 27);
			T.r[2] = sext8(peekb((uint16_t)(0xFC74 + T.r[0])));
			fl::movb((uint8_t)T.r[2]);
			tick(0xBF0C, 27);
			T.r[2] = fl_com(T.r[2]);
			tick(0xBF0E, 12);
			T.r[1] = fl_logic(T.r[1] & (uint16_t)~T.r[2]);
			tick(0xBF10, 12);
			T.r[1] = fl_logic(T.r[1] & 0x00FF);
			tick(0xBF14, 18);
		}
	}
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0xBF16, 12);
	c_restore(2);
	return T.r[0];
}

// F:BF5E  map_object_position(int *x, int *y): screen position of the current map object's
// definition (tables 0x5580 / 0x4AC0) adjusted by the cell's column (0xFDAA) and row (0xFDB2,
// with the scroll row 0x0E9B, one more right of column 0x099C).  Returns the column offset index.
uint16_t map_object_position(const Args &)
{
	c_save(2, 0xBF62);
	T.r[0] = peek(0x09AA);
	fl::mov(T.r[0]);
	tick(0xBF66, 24);
	T.r[0] = fl::asl(T.r[0]);
	tick(0xBF68, 12);
	T.r[0] = peek((uint16_t)(0x0AF8 + T.r[0]));
	fl::mov(T.r[0]);
	tick(0xBF6C, 27);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x01FF));
	tick(0xBF70, 18);
	T.r[1] = sext8(peekb(0x0E9B));
	fl::movb((uint8_t)T.r[1]);
	tick(0xBF74, 24);
	T.r[3] = peek(0x099C);
	fl::mov(T.r[3]);
	tick(0xBF78, 24);
	T.r[2] = T.r[0];
	fl::mov(T.r[2]);
	tick(0xBF7A, 12);
	T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x000F));
	tick(0xBF7E, 18);
	fl::cmp(T.r[2], T.r[3]);
	tick(0xBF80, 12);                                  // CMP R2,R3
	if (T.r[2] > T.r[3])
	{
		tick(0xBF82, 12);                              // BLOS not taken: the next row
		T.r[1] = fl::inc(T.r[1]);
		tick(0xBF84, 12);
	}
	else
		tick(0xBF84, 12);
	T.r[2] = T.r[0];
	fl::mov(T.r[2]);
	tick(0xBF86, 12);
	for (uint16_t pc = 0xBF88; pc <= 0xBF8E; pc += 2)
	{
		T.r[2] = asr(T.r[2], 1);
		tick(pc, 12);                                  // ASR R2
	}
	T.r[2] = (uint16_t)(T.r[2] + T.r[1]);
	tick(0xBF90, 12);
	T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x0003));
	tick(0xBF94, 18);
	T.r[2] = fl::asl(T.r[2]);
	tick(0xBF96, 12);
	T.r[1] = peek(0x09B6);
	fl::mov(T.r[1]);
	tick(0xBF9A, 24);
	T.r[1] = fl::asl(T.r[1]);
	tick(0xBF9C, 12);
	T.r[3] = peek((uint16_t)(0xFDB2 + T.r[2]));
	fl::mov(T.r[3]);
	tick(0xBFA0, 27);
	T.r[3] = fl::add(T.r[3], peek((uint16_t)(0x4AC0 + T.r[1])));
	tick(0xBFA4, 27);
	poke(peek((uint16_t)(T.r[6] + 0x0C)), T.r[3]);     // *y
	fl::mov(T.r[3]);
	tick(0xBFA8, 36);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0003));
	tick(0xBFAC, 18);
	T.r[0] = fl::asl(T.r[0]);
	tick(0xBFAE, 12);
	T.r[2] = peek((uint16_t)(0xFDAA + T.r[0]));
	fl::mov(T.r[2]);
	tick(0xBFB2, 27);
	T.r[2] = fl::add(T.r[2], peek((uint16_t)(0x5580 + T.r[1])));
	tick(0xBFB6, 27);
	poke(peek((uint16_t)(T.r[6] + 0x0A)), T.r[2]);     // *x
	fl::mov(T.r[2]);
	tick(0xBFBA, 36);
	c_restore(2);
	return T.r[0];
}

// F:BD78 (assembly): the current map object (0x09AA) comes into view: when its cell lies in
// the window 0x0224-0x0226 (columns) x 0x0228-0x022A (rows) and, on the window's edges, its
// definition's position (0x5580 / 0x4AC0) is past the scroll fraction (0x09A6 / 0x09A4), the
// object is created (F:C4E4)
void map_object_enters()
{
	// failing a test: RTS with the flags of the comparison
	auto stays = []() {
		tick(0xBE0E, 12);
		rts();
	};
	T.r[0] = peek(0x09AA);
	fl::mov(T.r[0]);
	tick(0xBD7C, 24);
	T.r[0] = fl::asl(T.r[0]);
	tick(0xBD7E, 12);
	T.r[4] = peek((uint16_t)(0x0AF8 + T.r[0]));
	fl::mov(T.r[4]);
	tick(0xBD82, 27);
	T.r[2] = T.r[4];
	fl::mov(T.r[2]);
	tick(0xBD84, 12);
	T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x01FF));
	tick(0xBD88, 18);
	T.r[1] = T.r[2];
	fl::mov(T.r[1]);
	tick(0xBD8A, 12);
	T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x000F));
	tick(0xBD8E, 18);
	fl::cmp(T.r[1], peek(0x0224));
	tick(0xBD92, 24);
	if (lt()) return stays();
	tick(0xBD94, 12);
	fl::cmp(T.r[1], peek(0x0226));
	tick(0xBD98, 24);
	if (!fZ() && !lt()) return stays();
	tick(0xBD9A, 12);
	T.r[0] = T.r[2];
	fl::mov(T.r[0]);
	tick(0xBD9C, 12);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x01F0));
	tick(0xBDA0, 18);
	fl::cmp(T.r[0], peek(0x0228));
	tick(0xBDA4, 24);
	if (lt()) return stays();
	tick(0xBDA6, 12);
	fl::cmp(T.r[0], peek(0x022A));
	tick(0xBDAA, 24);
	if (!fZ() && !lt()) return stays();
	tick(0xBDAC, 12);
	T.r[4] = fl::logic((uint16_t)(T.r[4] & 0x7FFF));
	tick(0xBDB0, 18);
	T.r[2] = fl::asl(T.r[2]);
	tick(0xBDB2, 12);
	T.r[3] = T.r[4];
	fl::mov(T.r[3]);
	tick(0xBDB4, 12);
	T.r[3] = fl::asr(T.r[3]);
	tick(0xBDB6, 12);
	T.r[3] &= 0xFF00;
	fl::clr();
	tick(0xBDB8, 12);
	T.r[3] = (uint16_t)((T.r[3] << 8) | (T.r[3] >> 8));
	tick(0xBDBA, 12);
	T.r[3] = sext8((uint8_t)T.r[3]);
	fl::movb((uint8_t)T.r[3]);
	tick(0xBDBC, 12);
	T.r[3] = fl::add(T.r[3], peek((uint16_t)(0x7A00 + T.r[2])));
	tick(0xBDC0, 27);
	T.r[4] = T.r[3];
	fl::mov(T.r[4]);
	tick(0xBDC2, 12);
	// on the window's edges the definition's position must be past the scroll fraction
	fl::cmp(T.r[1], peek(0x0224));
	tick(0xBDC6, 24);
	if (!fZ() && !lt())
		tick(0xBDD4, 12);                              // BGT
	else
	{
		tick(0xBDC8, 12);
		T.r[2] = T.r[4];
		fl::mov(T.r[2]);
		tick(0xBDCA, 12);
		T.r[2] = fl::asl(T.r[2]);
		tick(0xBDCC, 12);
		fl::cmp(peek((uint16_t)(0x5580 + T.r[2])), peek(0x09A6));
		tick(0xBDD2, 39);
		if (!fC()) return stays();                     // BCC
		tick(0xBDD4, 12);
	}
	fl::cmp(T.r[1], peek(0x0226));
	tick(0xBDD8, 24);
	if (lt())
		tick(0xBDE6, 12);                              // BLT
	else
	{
		tick(0xBDDA, 12);
		T.r[2] = T.r[4];
		fl::mov(T.r[2]);
		tick(0xBDDC, 12);
		T.r[2] = fl::asl(T.r[2]);
		tick(0xBDDE, 12);
		fl::cmp(peek((uint16_t)(0x5580 + T.r[2])), peek(0x09A6));
		tick(0xBDE4, 39);
		if (fC()) return stays();                      // BCS
		tick(0xBDE6, 12);
	}
	fl::cmp(T.r[0], peek(0x022A));
	tick(0xBDEA, 24);
	if (lt())
		tick(0xBDF8, 12);                              // BLT
	else
	{
		tick(0xBDEC, 12);
		T.r[2] = T.r[4];
		fl::mov(T.r[2]);
		tick(0xBDEE, 12);
		T.r[2] = fl::asl(T.r[2]);
		tick(0xBDF0, 12);
		fl::cmp(peek((uint16_t)(0x4AC0 + T.r[2])), peek(0x09A4));
		tick(0xBDF6, 39);
		if (!fC()) return stays();                     // BCC
		tick(0xBDF8, 12);
	}
	fl::cmp(T.r[0], peek(0x0228));
	tick(0xBDFC, 24);
	if (!fZ() && !lt())
		tick(0xBE0A, 12);                              // BGT
	else
	{
		tick(0xBDFE, 12);
		T.r[2] = T.r[4];
		fl::mov(T.r[2]);
		tick(0xBE00, 12);
		T.r[2] = fl::asl(T.r[2]);
		tick(0xBE02, 12);
		fl::cmp(peek((uint16_t)(0x4AC0 + T.r[2])), peek(0x09A4));
		tick(0xBE08, 39);
		if (fC()) return stays();                      // BCS
		tick(0xBE0A, 12);
	}
	jsr(0xBE0E, 0xC4E4);
	rts();
}

// F:BF18: release the current map object's definition: its group (properties bits 0-3) is
// free again (0x0E6D+group = 0) and its bit in the bitmap 0x0CF6 is cleared.  Banks end at
// 0x1000 / 0x1400.  Returns the bitmap byte index.
uint16_t release_map_definition(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xBF1A, 24);
	bank_a(0xBF1A, 0x1000);
	bank_b(0xBF26, 0x1400);
	jsr(0xBF36, 0xBE10);
	T.r[1] = peek(0x09B6);
	fl::mov(T.r[1]);
	tick(0xBF3A, 24);
	T.r[0] = sext8(peekb((uint16_t)(0x4560 + T.r[1])));
	fl::movb((uint8_t)T.r[0]);
	tick(0xBF3E, 27);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x000F));
	tick(0xBF42, 18);
	if (T.r[0])
	{
		tick(0xBF44, 12);                              // the group is free again
		pokeb((uint16_t)(0x0E6D + T.r[0]), 0);
		fl::clr();
		tick(0xBF48, 30);
	}
	else
		tick(0xBF48, 12);                              // BEQ
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0xBF4A, 12);
	T.r[0] = fl_asr(T.r[0]);
	tick(0xBF4C, 12);
	T.r[0] = fl_asr(T.r[0]);
	tick(0xBF4E, 12);
	T.r[0] = fl_asr(T.r[0]);
	tick(0xBF50, 12);
	T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x0007));
	tick(0xBF54, 18);
	pokeb((uint16_t)(0x0CF6 + T.r[0]), fl::logicb((uint8_t)(peekb((uint16_t)(0x0CF6 + T.r[0])) & ~peekb((uint16_t)(0xFC74 + T.r[1])))));
	tick(0xBF5A, 45);
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0xBF5C, 18);
	rts();
	return T.r[0];
}

// F:C114: the object of the current map object turns towards the direction in byte 2 of its
// record (0x09B2): one animation frame (list 0x5F7C per type, entry per direction 0xFDBA) per
// call, the short way round; odd directions go straight to the target frame.  Types with
// 0x6502 set show the frame after.  While turning, the object heads for its definition's
// position (F:BF5E, from the type's offsets 0x62A4 / 0x636E) and returns 1; once facing the
// direction it gets the speed of the direction (tables 0x4138 / 0x4238 and 0x3FF8 / 0xFD8A,
// bank A 0x2400) and returns 0.
uint16_t turn_map_object(const Args &)
{
	c_save(4, 0xC118);
	T.r[6] = fl::sub(T.r[6], 0x0C);
	tick(0xC11C, 18);
	T.r[0] = peek(0x09B2);
	fl::mov(T.r[0]);
	tick(0xC120, 24);
	T.r[0] = sext8(peekb((uint16_t)(T.r[0] + 2)));      // the direction
	fl::movb((uint8_t)T.r[0]);
	tick(0xC124, 27);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
	tick(0xC128, 18);
	poke(0x01D2, 0);
	fl::clr();
	tick(0xC12C, 27);
	poke(0x1400, 0);
	fl::clr();
	tick(0xC130, 27);
	T.r[1] = 0x0400;
	fl::mov(T.r[1]);
	tick(0xC134, 18);
	poke(0x01D4, T.r[1]);
	fl::mov(T.r[1]);
	tick(0xC138, 27);
	poke(0x1402, T.r[1]);
	fl::mov(T.r[1]);
	tick(0xC13C, 27);
	T.r[1] = peek(0x09AA);
	fl::mov(T.r[1]);
	tick(0xC140, 24);
	T.r[3] = sext8(peekb((uint16_t)(0x09B8 + T.r[1])));
	fl::movb((uint8_t)T.r[3]);
	tick(0xC144, 27);
	T.r[3] &= 0x003F;                                  // the slot
	fl::logic(T.r[3]);
	tick(0xC148, 18);
	T.r[4] = 0;
	fl::clr();
	tick(0xC14A, 12);
	T.r[4] |= peekb((uint16_t)(0x0370 + T.r[3]));      // its type
	fl::logicb((uint8_t)T.r[4]);
	tick(0xC14E, 27);
	// (SP) type * 2, 2(SP) turned, 4(SP) .. 0xE(SP) locals
	push(0);
	fl::clr();
	tick(0xC150, 24);
	push(T.r[4]);
	fl::mov(T.r[4]);
	tick(0xC152, 24);
	poke(T.r[6], (uint16_t)(peek(T.r[6]) << 1));
	tick(0xC154, 21);
	T.r[1] = 0;
	fl::clr();
	tick(0xC156, 12);
	T.r[1] |= peekb((uint16_t)(0xFDBA + T.r[0]));
	fl::logicb((uint8_t)T.r[1]);
	tick(0xC15A, 27);
	T.r[2] = peek(T.r[6]);
	fl::mov(T.r[2]);
	tick(0xC15C, 18);
	T.r[1] = fl::add(T.r[1], peek((uint16_t)(0x5F7C + T.r[2])));
	tick(0xC160, 27);
	T.r[1] = peek(T.r[1]);                             // the target frame
	fl::mov(T.r[1]);
	tick(0xC162, 18);
	T.r[2] = sext8(peekb((uint16_t)(0x07D0 + T.r[3])));
	fl::movb((uint8_t)T.r[2]);
	tick(0xC166, 27);
	poke(sp_at(6), T.r[2]);                            // the current direction
	fl::mov(T.r[2]);
	tick(0xC16A, 30);
	T.r[2] = T.r[0];
	fl::mov(T.r[2]);
	tick(0xC16C, 12);
	T.r[2] = fl::sub(T.r[2], peek(sp_at(6)));
	tick(0xC170, 27);
	if (fZ())
		tick(0xC248, 12);                              // BEQ: facing it
	else
	{
		tick(0xC172, 12);
		if (!fN())
		{
			tick(0xC17E, 12);                          // BPL
			poke(sp_at(0x0A), 0x0002);
			fl::mov(0x0002);
			tick(0xC184, 36);
		}
		else
		{
			tick(0xC174, 12);
			T.r[2] = (uint16_t)-T.r[2];
			fl::neg((uint16_t)(-T.r[2]));
			tick(0xC176, 12);
			poke(sp_at(0x0A), 0xFFFE);
			fl::mov(0xFFFE);
			tick(0xC17C, 36);
			tick(0xC184, 12);                          // BR
		}
		fl::cmp(T.r[2], 0x0008);
		tick(0xC188, 18);
		if (fZ() || lt())
			tick(0xC18E, 12);                          // BLE
		else
		{
			tick(0xC18A, 12);
			poke(sp_at(0x0A), (uint16_t)-peek(sp_at(0x0A)));   // the short way round
			fl::neg((uint16_t)(-peek((uint16_t)(T.r[6] + 0x0A))));
			tick(0xC18E, 30);
		}
		poke(sp_at(8), 0x04B0);
		fl::mov(0x04B0);
		tick(0xC194, 36);
		poke(sp_at(8), fl::add(peek(sp_at(8)), T.r[3]));
		tick(0xC198, 30);
		const bool moving = (peekb(peek(sp_at(8))) & 0x01) != 0;
		fl::logicb((uint8_t)(peekb(rw((uint16_t)(T.r[6] + 0x0008))) & 0x01));
		tick(0xC19E, 39);
		if (!moving)
		{
			tick(0xC1A0, 12);
			T.r[5] = 0;
			fl::clr();
			tick(0xC1A2, 12);
			push(peek(sp_at(6)));
			fl::mov(peek(T.r[6]));
			tick(0xC1A6, 39);
			poke(T.r[6], fl::add(peek(T.r[6]), 0xFDBA));
			tick(0xC1AA, 27);
			T.r[5] |= peekb(pop());
			fl::logicb((uint8_t)T.r[5]);
			tick(0xC1AC, 24);
			push(peek(T.r[6]));
			fl::mov(peek(T.r[6]));
			tick(0xC1AE, 30);
			poke(T.r[6], fl::add(peek(T.r[6]), 0x5F7C));
			tick(0xC1B2, 27);
			T.r[5] = fl::add(T.r[5], peek(pop()));
			tick(0xC1B4, 24);
			T.r[2] = peek(T.r[5]);
			fl::mov(T.r[2]);
			tick(0xC1B6, 18);
			tick(0xC1C0, 12);                          // BR
		}
		else
		{
			tick(0xC1B8, 12);                          // BNE
			T.r[5] = T.r[3];
			fl::mov(T.r[5]);
			tick(0xC1BA, 12);
			T.r[5] = fl::asl(T.r[5]);
			tick(0xC1BC, 12);
			T.r[2] = peek((uint16_t)(0x03B0 + T.r[5]));
			fl::mov(T.r[2]);
			tick(0xC1C0, 27);
		}
		T.r[2] = fl::add(T.r[2], peek(sp_at(0x0A)));
		tick(0xC1C4, 27);
		fl::tst(peek(T.r[2]));
		tick(0xC1C6, 18);
		if (fN())
		{
			tick(0xC1C8, 12);
			T.r[2] = peek(T.r[2]);
			fl::mov(T.r[2]);
			tick(0xC1CA, 18);
			T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x7FFF));
			tick(0xC1CE, 18);
		}
		else
			tick(0xC1CE, 12);                          // BGE
		T.r[5] = T.r[0];
		fl::mov(T.r[5]);
		tick(0xC1D0, 12);
		T.r[5] = fl_logic(T.r[5] | peek(sp_at(6)));
		tick(0xC1D4, 27);
		T.r[5] = fl_ror(T.r[5]);
		tick(0xC1D6, 12);
		if (fC())
		{
			tick(0xC1D8, 12);                          // odd directions go straight to the target
			T.r[2] = T.r[1];
			fl::mov(T.r[2]);
			tick(0xC1DA, 12);
		}
		else
			tick(0xC1DA, 12);                          // BCC
		fl::cmp(T.r[2], T.r[1]);
		tick(0xC1DC, 12);                              // CMP R2,R1
		if (T.r[2] == T.r[1])
		{
			tick(0xC1DE, 12);
			T.r[5] = 0xFFFE;
			fl::mov(T.r[5]);
			tick(0xC1E2, 18);
			T.r[5] = (uint16_t)((T.r[5] & 0xFF00) | (uint8_t)~T.r[5]);
			fl::comb((uint8_t)(~(uint8_t)T.r[5]));
			tick(0xC1E4, 12);
			pokeb(peek(sp_at(8)), fl::logicb((uint8_t)(peekb(peek(sp_at(8))) & ~T.r[5])));
			tick(0xC1E8, 36);
			tick(0xC1F0, 12);                          // BR
		}
		else
		{
			tick(0xC1EA, 12);                          // BNE
			T.r[1] = T.r[2];
			fl::mov(T.r[1]);
			tick(0xC1EC, 12);
			poke(sp_at(2), (uint16_t)(peek(sp_at(2)) + 1));   // turned
			fl::inc((uint16_t)(peek((uint16_t)(T.r[6] + 2)) - 1));
			tick(0xC1F0, 30);
		}
		const bool after = peekb((uint16_t)(0x6502 + T.r[4])) != 0;
		fl::tstb(peekb((uint16_t)(T.r[4] + 0x6502)));
		tick(0xC1F4, 27);
		if (!after)
			tick(0xC216, 12);                          // BEQ
		else
		{
			// types that show the frame after
			tick(0xC1F6, 12);
			const bool turned = peek(sp_at(2)) != 0;
			fl::tst(peek((uint16_t)(T.r[6] + 2)));
			tick(0xC1FA, 27);
			if (!turned)
			{
				tick(0xC214, 12);                      // BEQ
				T.r[1] = peek(T.r[1]);
				fl::mov(T.r[1]);
				tick(0xC216, 18);
			}
			else
			{
				tick(0xC1FC, 12);
				const bool mv = (peekb(peek(sp_at(8))) & 0x01) != 0;
				fl::logicb((uint8_t)(peekb(rw((uint16_t)(T.r[6] + 0x0008))) & 0x01));
				tick(0xC202, 39);
				if (!mv)
					tick(0xC216, 12);                  // BEQ
				else
				{
					tick(0xC204, 12);
					T.r[1] = fl::add(T.r[1], peek(sp_at(0x0A)));
					tick(0xC208, 27);
					const bool link = (int16_t)peek(T.r[1]) < 0;
					fl::tst(peek(T.r[1]));
					tick(0xC20A, 18);
					if (!link)
						tick(0xC216, 12);              // BGE
					else
					{
						tick(0xC20C, 12);
						T.r[1] = peek(T.r[1]);
						fl::mov(T.r[1]);
						tick(0xC20E, 18);
						T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x7FFF));
						tick(0xC212, 18);
						tick(0xC216, 12);              // BR
					}
				}
			}
		}
		T.r[5] = T.r[3];
		fl::mov(T.r[5]);
		tick(0xC218, 12);
		T.r[5] = fl::asl(T.r[5]);
		tick(0xC21A, 12);
		const bool same = T.r[1] == peek((uint16_t)(0x03B0 + T.r[5]));
		fl::cmp(T.r[1], peek((uint16_t)(T.r[5] + 0x03B0)));
		tick(0xC21E, 27);
		if (same)
			tick(0xC248, 12);                          // BEQ
		else
		{
			tick(0xC220, 12);
			poke((uint16_t)(0x03B0 + T.r[5]), T.r[1]);
			fl::mov(T.r[1]);
			tick(0xC224, 30);
			pokeb((uint16_t)(0x0490 + T.r[3]), fl::logicb((uint8_t)(peekb((uint16_t)(0x0490 + T.r[3])) | 0x18)));
			tick(0xC22A, 36);
			push(peek(T.r[6]));
			fl::mov(peek(T.r[6]));
			tick(0xC22C, 30);
			poke(T.r[6], fl::add(peek(T.r[6]), 0x6110));
			tick(0xC230, 27);
			T.r[2] = peek(pop());
			fl::mov(T.r[2]);
			tick(0xC232, 24);
			if (!T.r[2])
				tick(0xC248, 12);                      // BEQ
			else
			{
				tick(0xC234, 12);
				pokeb(sp_at(0x0A), peekb((uint16_t)(0xFDBA + T.r[0])));
				fl::movb(peekb((uint16_t)(T.r[6] + 0x0A)));
				tick(0xC23A, 45);
				pokeb(sp_at(0x0B), 0);
				fl::clr();
				tick(0xC23E, 30);
				poke(sp_at(0x0A), fl::add(peek(sp_at(0x0A)), T.r[2]));
				tick(0xC242, 30);
				poke((uint16_t)(0x03F0 + T.r[5]), peek(peek(sp_at(0x0A))));
				tick(0xC248, 51);
			}
		}
	}
	// pointers to the rate (6), x speed (8), y speed (0xA) and counter (4) of the slot
	poke(sp_at(6), 0x0770);
	fl::mov(0x0770);
	tick(0xC24E, 36);
	poke(sp_at(6), fl::add(peek(sp_at(6)), T.r[3]));
	tick(0xC252, 30);
	T.r[5] = T.r[3];
	fl::mov(T.r[5]);
	tick(0xC254, 12);
	T.r[5] = fl::asl(T.r[5]);
	tick(0xC256, 12);
	poke(sp_at(8), 0x04F0);
	fl::mov(0x04F0);
	tick(0xC25C, 36);
	poke(sp_at(8), fl::add(peek(sp_at(8)), T.r[5]));
	tick(0xC260, 30);
	poke(sp_at(0x0A), 0x0570);
	fl::mov(0x0570);
	tick(0xC266, 36);
	poke(sp_at(0x0A), fl::add(peek(sp_at(0x0A)), T.r[5]));
	tick(0xC26A, 30);
	poke(sp_at(4), 0x0430);
	fl::mov(0x0430);
	tick(0xC270, 36);
	poke(sp_at(4), fl::add(peek(sp_at(4)), T.r[5]));
	tick(0xC274, 30);
	const bool turned = peek(sp_at(2)) != 0;
	fl::tst(peek((uint16_t)(T.r[6] + 2)));
	tick(0xC278, 27);
	if (!turned)
	{
		// facing the direction: the speed of the direction
		tick(0xC27A, 12);
		T.r[2] = sext8(peekb((uint16_t)(0x649D + T.r[4])));
		fl::movb((uint8_t)T.r[2]);
		tick(0xC27E, 27);
		T.r[2] &= 0x0001;                              // stands still
		fl::logic(T.r[2]);
		tick(0xC282, 18);
		if (!T.r[2])
			tick(0xC292, 12);                          // BEQ
		else
		{
			tick(0xC284, 12);
			fl::logic((uint16_t)(0x0001 & T.r[0]));
			tick(0xC288, 18);                          // BIT #1,R0
			if (!(T.r[0] & 0x0001))
				tick(0xC292, 12);                      // BEQ
			else
			{
				tick(0xC28A, 12);
				T.r[2] = 0;
				fl::clr();
				tick(0xC28C, 12);
				pokeb((uint16_t)(0x0490 + T.r[3]), fl::logicb((uint8_t)(peekb((uint16_t)(0x0490 + T.r[3])) & ~0x01)));
				tick(0xC292, 36);
			}
		}
		T.r[4] = sext8(peekb((uint16_t)(0x65CC + T.r[4])));
		fl::movb((uint8_t)T.r[4]);
		tick(0xC296, 27);
		{
			const uint8_t b = peekb(peek(sp_at(6)));
			T.r[6] = (uint16_t)(T.r[6] - 2);
			pokeb(T.r[6], b);
		}
		fl::movb(peekb(T.r[6]));
		tick(0xC29A, 45);                              // MOVB @6(SP),-(SP)
		poke(T.r[6], fl::logic((uint16_t)(peek(T.r[6]) & 0x007F)));
		tick(0xC29E, 27);
		pokeb(peek(sp_at(8)), 0);
		fl::clr();
		tick(0xC2A2, 36);
		{
			const uint8_t b = peekb(T.r[6]);
			T.r[6] = (uint16_t)(T.r[6] + 2);
			const uint16_t a = peek(sp_at(6));
			pokeb(a, (uint8_t)(peekb(a) | b));
		}
		tick(0xC2A6, 42);                              // BISB (SP)+,@6(SP): the rate without bit 7
		poke(sp_at(2), 0x1000);
		fl::mov(0x1000);
		tick(0xC2AC, 36);
		poke(0x01D2, 0x1000);
		fl::mov(0x1000);
		tick(0xC2B2, 33);
		poke(0x1400, peek(sp_at(2)));
		fl::mov(peek((uint16_t)(T.r[6] + 2)));
		tick(0xC2B8, 42);
		poke(sp_at(2), 0x1400);
		fl::mov(0x1400);
		tick(0xC2BE, 36);
		poke(0x01D4, 0x1400);
		fl::mov(0x1400);
		tick(0xC2C4, 33);
		poke(0x1402, peek(sp_at(2)));
		fl::mov(peek((uint16_t)(T.r[6] + 2)));
		tick(0xC2CA, 42);
		poke(sp_at(2), peek(0x09B2));
		fl::mov(peek((uint16_t)(T.r[6] + 2)));
		tick(0xC2D0, 42);
		T.r[1] = 0;
		fl::clr();
		tick(0xC2D2, 12);
		T.r[5] = peek(sp_at(2));
		fl::mov(T.r[5]);
		tick(0xC2D6, 27);
		T.r[1] |= peekb((uint16_t)(T.r[5] + 3));       // the record's speed
		fl::logicb((uint8_t)T.r[1]);
		tick(0xC2DA, 27);
		fl::tst(T.r[2]);
		tick(0xC2DC, 12);                              // TST R2
		if (T.r[2])
		{
			tick(0xC328, 12);                          // BNE: standing
			T.r[5] = 0x2400;
			fl::mov(T.r[5]);
			tick(0xC32C, 18);
			poke(0x01D2, T.r[5]);
			fl::mov(T.r[5]);
			tick(0xC330, 27);
			poke(0x1400, T.r[5]);
			fl::mov(T.r[5]);
			tick(0xC334, 27);
			T.r[1] = fl::asl(T.r[1]);
			tick(0xC336, 12);
			poke(peek(sp_at(4)), fl::add(peek(peek(sp_at(4))), peek((uint16_t)(0x40F8 + T.r[1]))));
			tick(0xC33C, 51);
			poke(peek(sp_at(8)), 0);
			fl::clr();
			tick(0xC340, 36);
			poke(peek(sp_at(0x0A)), 0);
			fl::clr();
			tick(0xC344, 36);
			pokeb((uint16_t)(0x0490 + T.r[3]), fl::logicb((uint8_t)(peekb((uint16_t)(0x0490 + T.r[3])) | 0x01)));
			tick(0xC34A, 36);
		}
		else
		{
			tick(0xC2DE, 12);
			T.r[5] = 0x2400;
			fl::mov(T.r[5]);
			tick(0xC2E2, 18);
			poke(0x01D2, T.r[5]);
			fl::mov(T.r[5]);
			tick(0xC2E6, 27);
			poke(0x1400, T.r[5]);
			fl::mov(T.r[5]);
			tick(0xC2EA, 27);
			T.r[5] = T.r[0];
			fl::mov(T.r[5]);
			tick(0xC2EC, 12);
			for (uint16_t pc = 0xC2EE; pc <= 0xC2F2; pc += 2)
			{
				T.r[5] = (uint16_t)(T.r[5] << 1);
				tick(pc, 12);                          // ASL R5
			}
			T.r[5] = (uint16_t)(T.r[5] + T.r[4]);
			tick(0xC2F4, 12);
			T.r[2] = T.r[5];
			fl::mov(T.r[2]);
			tick(0xC2F6, 12);
			T.r[2] = fl::asl(T.r[2]);
			tick(0xC2F8, 12);
			poke(peek(sp_at(8)), peek((uint16_t)(0x4138 + T.r[2])));
			tick(0xC2FE, 51);
			poke(peek(sp_at(0x0A)), peek((uint16_t)(0x4238 + T.r[2])));
			tick(0xC304, 51);
			fl::cmp(T.r[1], 0x0018);
			tick(0xC308, 18);                          // CMP R1,#0x18
			if (T.r[1] == 0x0018)
			{
				tick(0xC30A, 12);
				pokeb(peek(sp_at(4)), peekb((uint16_t)(0xFD8A + T.r[4])));
				tick(0xC310, 51);
				tick(0xC320, 12);                      // BR
			}
			else
			{
				tick(0xC312, 12);                      // BNE
				for (uint16_t pc = 0xC314; pc <= 0xC318; pc += 2)
				{
					T.r[1] = (uint16_t)(T.r[1] << 1);
					tick(pc, 12);                      // ASL R1
				}
				T.r[1] = (uint16_t)(T.r[1] + T.r[4]);
				tick(0xC31A, 12);
				pokeb(peek(sp_at(4)), peekb((uint16_t)(0x3FF8 + T.r[1])));
				tick(0xC320, 51);
			}
			poke(peek(sp_at(4)), fl::logic((uint16_t)(peek(peek(sp_at(4))) & 0x00FF)));
			tick(0xC326, 42);
			tick(0xC34A, 12);                          // BR
		}
		pokeb((uint16_t)(0x07D0 + T.r[3]), (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0xC34E, 30);
		T.r[0] = 0;
		fl::clr();
		tick(0xC350, 12);
		tick(0xC3E0, 12);                              // BR
	}
	else
	{
		// still turning: head for the definition's position (F:BF5E, type offsets 0x62A4 / 0x636E)
		tick(0xC352, 12);                              // BNE
		bank_a(0xC352, 0x1000);
		bank_b(0xC35E, 0x1400);
		push(0x000E);
		tick(0xC36E, 30);
		poke(T.r[6], fl::add(peek(T.r[6]), T.r[6]));
		tick(0xC370, 21);
		push(0x0012);
		fl::mov(0x0012);
		tick(0xC374, 30);
		poke(T.r[6], fl::add(peek(T.r[6]), T.r[6]));
		tick(0xC376, 21);
		jsr(0xC37A, 0xBF5E);
		poke(0x01D2, 0);
		fl::clr();
		tick(0xC37E, 27);
		poke(0x1400, 0);
		fl::clr();
		tick(0xC382, 27);
		T.r[0] = 0x0400;
		fl::mov(T.r[0]);
		tick(0xC386, 18);
		poke(0x01D4, T.r[0]);
		fl::mov(T.r[0]);
		tick(0xC38A, 27);
		poke(0x1402, T.r[0]);
		fl::mov(T.r[0]);
		tick(0xC38E, 27);
		T.r[0] = peek(sp_at(4));
		fl::mov(T.r[0]);
		tick(0xC392, 27);
		T.r[2] = peek((uint16_t)(0x62A4 + T.r[0]));
		fl::mov(T.r[2]);
		tick(0xC396, 27);
		T.r[1] = peek((uint16_t)(0x636E + T.r[0]));
		fl::mov(T.r[1]);
		tick(0xC39A, 27);
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0xC39C, 12);
		T.r[0] = fl::sub(T.r[0], peek((uint16_t)(0x06F0 + T.r[5])));
		tick(0xC3A0, 27);
		T.r[0] = fl::add(T.r[0], peek(sp_at(0x10)));
		tick(0xC3A4, 27);
		T.r[2] = T.r[0];
		fl::mov(T.r[2]);
		tick(0xC3A6, 12);
		for (uint16_t pc = 0xC3A8; pc <= 0xC3AE; pc += 2)
		{
			T.r[2] = asr(T.r[2], 1);
			tick(pc, 12);                              // ASR R2
		}
		poke(peek(sp_at(0x0C)), T.r[2]);
		fl::mov(T.r[2]);
		tick(0xC3B2, 36);
		T.r[1] = fl::sub(T.r[1], peek((uint16_t)(0x0730 + T.r[5])));
		tick(0xC3B6, 27);
		T.r[1] = fl::add(T.r[1], peek(sp_at(0x12)));
		tick(0xC3BA, 27);
		T.r[2] = T.r[1];
		fl::mov(T.r[2]);
		tick(0xC3BC, 12);
		for (uint16_t pc = 0xC3BE; pc <= 0xC3C4; pc += 2)
		{
			T.r[2] = asr(T.r[2], 1);
			tick(pc, 12);                              // ASR R2
		}
		poke(peek(sp_at(0x0E)), T.r[2]);
		tick(0xC3C8, 36);
		poke(peek(sp_at(8)), 0x0006);
		fl::mov(0x0006);
		tick(0xC3CE, 42);
		pokeb(peek(sp_at(0x0A)), fl::logicb((uint8_t)(peekb(peek(sp_at(0x0A))) | 0x80)));
		tick(0xC3D4, 42);
		pokeb((uint16_t)(0x04B0 + T.r[3]), fl::logicb((uint8_t)(peekb((uint16_t)(0x04B0 + T.r[3])) | 0x01)));
		tick(0xC3DA, 36);
		T.r[6] = (uint16_t)(T.r[6] + 4);                // CMP (SP)+,(SP)+
		fl::cmp(peek((uint16_t)(T.r[6] - 4)), peek((uint16_t)(T.r[6] - 2)));
		tick(0xC3DC, 24);
		T.r[0] = 0x0001;
		fl::mov(T.r[0]);
		tick(0xC3E0, 18);
	}
	T.r[6] = fl::add(T.r[6], 0x0010);
	tick(0xC3E4, 18);
	c_restore(4);
	return T.r[0];
}

// F:BFBC: the current map object moves to the definition F:BE7C found (0x09B4) once its object
// is in place (F:C0D8 for map objects without an object, F:C114 turning for the others; non-zero
// means not yet): the old definition (0x09B6) is released (group 0x0E6D or bitmap 0x0CF6), the
// object's palette bit 7 (0x0470) follows the new definition's bit 5, the new definition is
// taken and the descriptor becomes the record's word (bit 15 kept).  Banks end at 0x1000 /
// 0x1400.  Returns the descriptor's bit 15 (0x1400 when not moved).
uint16_t move_map_object(const Args &)
{
	c_save(4, 0xBFC0);
	T.r[6] = (uint16_t)(T.r[6] - 2);                    // TST -(SP): the descriptor
	fl::tst(peek(T.r[6]));
	tick(0xBFC2, 21);
	T.r[0] = peek(0x09AA);
	fl::mov(T.r[0]);
	tick(0xBFC6, 24);
	T.r[0] = fl::asl(T.r[0]);
	tick(0xBFC8, 12);
	const uint16_t d = peek((uint16_t)(0x0AF8 + T.r[0]));
	fl::tst(peek((uint16_t)(T.r[0] + 0x0AF8)));
	tick(0xBFCC, 27);
	if ((int16_t)d <= 0)
	{
		tick(0xBFD4, 12);                              // BLE: turn its object
		jsr(0xBFD8, 0xC114);
	}
	else
	{
		tick(0xBFCE, 12);
		jsr(0xBFD2, 0xC0D8);
		tick(0xBFD8, 12);                              // BR
	}
	fl::tst(T.r[0]);
	tick(0xBFDA, 12);                                  // TST R0
	if (T.r[0])
	{
		tick(0xC0BC, 12);                              // BNE: not yet
		bank_a(0xC0BC, 0x1000);
		bank_b(0xC0C8, 0x1400);
	}
	else
	{
		tick(0xBFDC, 12);
		bank_a(0xBFDC, 0x1000);
		bank_b(0xBFE8, 0x1400);
		// the old definition is released
		T.r[1] = peek(0x09B6);
		tick(0xBFF8, 24);
		const bool grouped = (peekb((uint16_t)(0x4560 + T.r[1])) & 0x0F) != 0;
		fl::logicb((uint8_t)(0x0F & peekb((uint16_t)(T.r[1] + 0x4560))));
		tick(0xBFFE, 33);
		if (grouped)
		{
			tick(0xC000, 12);
			T.r[1] = sext8(peekb((uint16_t)(0x4560 + T.r[1])));
			fl::movb((uint8_t)T.r[1]);
			tick(0xC004, 27);
			T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x000F));
			tick(0xC008, 18);
			T.r[0] = peek(0x09B4);
			fl::mov(T.r[0]);
			tick(0xC00C, 24);
			const bool kept = (peekb((uint16_t)(0x4560 + T.r[0])) & 0x0F) != 0;
			fl::logicb((uint8_t)(0x0F & peekb((uint16_t)(T.r[0] + 0x4560))));
			tick(0xC012, 33);
			if (kept)
				tick(0xC032, 12);                      // BNE
			else
			{
				tick(0xC014, 12);
				pokeb((uint16_t)(0x0E6D + T.r[1]), 0);
				fl::clr();
				tick(0xC018, 30);
				tick(0xC032, 12);                      // BR
			}
		}
		else
		{
			tick(0xC01A, 12);                          // BEQ
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0xC01C, 12);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0007));
			tick(0xC020, 18);
			T.r[0] = sext8(peekb((uint16_t)(0xFC74 + T.r[0])));
			fl::movb((uint8_t)T.r[0]);
			tick(0xC024, 27);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
			tick(0xC028, 18);
			for (uint16_t pc = 0xC02A; pc <= 0xC02E; pc += 2)
			{
				T.r[1] = asr(T.r[1], 1);
				tick(pc, 12);                          // ASR R1
			}
			pokeb((uint16_t)(0x0CF6 + T.r[1]), fl::logicb((uint8_t)(peekb((uint16_t)(0x0CF6 + T.r[1])) & ~T.r[0])));
			tick(0xC032, 30);
		}
		T.r[4] = peek(0x09B4);
		fl::mov(T.r[4]);
		tick(0xC036, 24);
		T.r[1] = T.r[4];
		fl::mov(T.r[1]);
		tick(0xC038, 12);
		T.r[5] = peek(0x09AA);
		fl::mov(T.r[5]);
		tick(0xC03C, 24);
		T.r[5] = fl::asl(T.r[5]);
		tick(0xC03E, 12);
		const uint16_t desc = peek((uint16_t)(0x0AF8 + T.r[5]));
		poke(T.r[6], desc);
		fl::mov(peek(T.r[6]));
		tick(0xC042, 36);
		if ((int16_t)desc < 0)
		{
			// the object's palette bit 7 follows the new definition's bit 5
			tick(0xC044, 12);
			T.r[3] = peek(0x09AA);
			fl::mov(T.r[3]);
			tick(0xC048, 24);
			T.r[2] = sext8(peekb((uint16_t)(0x09B8 + T.r[3])));
			fl::movb((uint8_t)T.r[2]);
			tick(0xC04C, 27);
			T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x003F));
			tick(0xC050, 18);
			T.r[3] = 0x0470;
			fl::mov(T.r[3]);
			tick(0xC054, 18);
			T.r[3] = fl::add(T.r[3], T.r[2]);
			tick(0xC056, 12);
			const bool bit5 = (peekb((uint16_t)(0x4560 + T.r[1])) & 0x20) != 0;
			fl::logicb((uint8_t)(0x20 & peekb((uint16_t)(T.r[1] + 0x4560))));
			tick(0xC05C, 33);
			bool redraw = false;
			if (bit5)
			{
				tick(0xC05E, 12);
				const bool set = (peekb(T.r[3]) & 0x80) != 0;
				fl::tstb(peekb(T.r[3]));
				tick(0xC060, 18);
				if (set)
					tick(0xC076, 12);                  // BLT
				else
				{
					tick(0xC062, 12);
					pokeb(T.r[3], fl::logicb((uint8_t)(peekb(T.r[3]) | 0x80)));
					tick(0xC066, 27);
					tick(0xC070, 12);                  // BR
					redraw = true;
				}
			}
			else
			{
				tick(0xC068, 12);                      // BEQ
				const bool set = (peekb(T.r[3]) & 0x80) != 0;
				fl::tstb(peekb(T.r[3]));
				tick(0xC06A, 18);
				if (!set)
					tick(0xC076, 12);                  // BGE
				else
				{
					tick(0xC06C, 12);
					pokeb(T.r[3], fl::logicb((uint8_t)(peekb(T.r[3]) & ~0x80)));
					tick(0xC070, 27);
					redraw = true;
				}
			}
			if (redraw)
			{
				pokeb((uint16_t)(0x0490 + T.r[2]), fl::logicb((uint8_t)(peekb((uint16_t)(0x0490 + T.r[2])) | 0x18)));
				tick(0xC076, 36);
			}
		}
		else
			tick(0xC076, 12);                          // BGE
		// the new definition is taken
		const bool grouped2 = (peekb((uint16_t)(0x4560 + T.r[4])) & 0x0F) != 0;
		fl::logicb((uint8_t)(0x0F & peekb((uint16_t)(T.r[4] + 0x4560))));
		tick(0xC07C, 33);
		if (grouped2)
		{
			tick(0xC07E, 12);
			T.r[1] = sext8(peekb((uint16_t)(0x4560 + T.r[4])));
			fl::movb((uint8_t)T.r[1]);
			tick(0xC082, 27);
			T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x000F));
			tick(0xC086, 18);
			T.r[2] = peek(0x09AA);
			fl::mov(T.r[2]);
			tick(0xC08A, 24);
			T.r[2] = fl::inc(T.r[2]);
			tick(0xC08C, 12);
			pokeb((uint16_t)(0x0E6D + T.r[1]), (uint8_t)T.r[2]);
			fl::movb((uint8_t)T.r[2]);
			tick(0xC090, 30);
			tick(0xC0A8, 12);                          // BR
		}
		else
		{
			tick(0xC092, 12);                          // BEQ
			T.r[2] = T.r[1];
			fl::mov(T.r[2]);
			tick(0xC094, 12);
			T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x0007));
			tick(0xC098, 18);
			T.r[0] = 0;
			fl::clr();
			tick(0xC09A, 12);
			T.r[0] |= peekb((uint16_t)(0xFC74 + T.r[2]));
			fl::logicb((uint8_t)T.r[0]);
			tick(0xC09E, 27);
			for (uint16_t pc = 0xC0A0; pc <= 0xC0A4; pc += 2)
			{
				T.r[1] = asr(T.r[1], 1);
				tick(pc, 12);                          // ASR R1
			}
			pokeb((uint16_t)(0x0CF6 + T.r[1]), fl::logicb((uint8_t)(peekb((uint16_t)(0x0CF6 + T.r[1])) | T.r[0])));
			tick(0xC0A8, 30);
		}
		// the descriptor becomes the record's word, bit 15 kept
		T.r[1] = peek(peek(0x09B2));
		fl::mov(T.r[1]);
		tick(0xC0AC, 33);
		T.r[0] = peek(T.r[6]);
		fl::mov(T.r[0]);
		tick(0xC0AE, 18);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x8000));
		tick(0xC0B2, 18);
		poke((uint16_t)(0x0AF8 + T.r[5]), T.r[1]);
		fl::mov(T.r[1]);
		tick(0xC0B6, 30);
		poke((uint16_t)(0x0AF8 + T.r[5]), fl::logic((uint16_t)(peek((uint16_t)(0x0AF8 + T.r[5])) | T.r[0])));
		tick(0xC0BA, 30);
		tick(0xC0D4, 12);                              // BR
	}
	fl::tst(pop());
	tick(0xC0D6, 18);
	c_restore(4);
	return T.r[0];
}

// F:CF5E  remove_map_object(int n): map object n goes: its object slot is reset (F:B56E) if it
// has one, its definition released (F:BF18); a fixed one (n < 0x40) clears its timer 0x0AB8,
// a dynamic one counts the event of its definition (F:D348, properties 0x4000 bits 0-3).
// Descriptor and slot entry are cleared.  Returns the definition's properties byte (0x4000).
uint16_t remove_map_object(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xCF60, 24);
	{
		const uint16_t n = rw(sp_at(4));
		poke(0x09AA, n);
		fl::mov(n);
	}
	tick(0xCF66, 42);
	T.r[1] = rw(sp_at(4));
	fl::mov(T.r[1]);
	tick(0xCF6A, 27);
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0xCF6C, 12);
	T.r[0] = fl_asl(T.r[0]);
	tick(0xCF6E, 12);
	const uint16_t desc = peek((uint16_t)(0x0AF8 + T.r[0]));
	fl::tst(desc);
	tick(0xCF72, 27);
	if ((int16_t)desc >= 0)
	{
		tick(0xCF8C, 12);                              // BGE: no object of its own
		T.r[1] = sext8(peekb((uint16_t)(0x09B8 + T.r[1])));
		fl::movb((uint8_t)T.r[1]);
		tick(0xCF90, 27);
		T.r[1] = fl_logic(T.r[1] & 0x007F);
		tick(0xCF94, 18);
	}
	else
	{
		// its object slot is reset
		tick(0xCF74, 12);
		T.r[0] = sext8(peekb((uint16_t)(0x09B8 + T.r[1])));
		fl::movb((uint8_t)T.r[0]);
		tick(0xCF78, 27);
		T.r[0] = fl_logic(T.r[0] & 0x003F);
		tick(0xCF7C, 18);
		T.r[1] = 0;
		fl::clr();
		tick(0xCF7E, 12);
		T.r[1] = fl_logicb(peekb((uint16_t)(0x0370 + T.r[0])));
		tick(0xCF82, 27);
		push(T.r[0]);
		fl::mov(T.r[0]);
		tick(0xCF84, 24);
		jsr(0xCF88, 0xB56E);                           // reset_object
		fl::tst(rw(T.r[6]));
		T.r[6] = (uint16_t)(T.r[6] + 2);
		tick(0xCF8A, 18);
		tick(0xCF94, 12);                              // BR
	}
	jsr(0xCF98, 0xBF18);                               // release_map_definition
	T.r[0] = peek(0x09AA);
	fl::mov(T.r[0]);
	tick(0xCF9C, 24);
	fl::cmp(T.r[0], 0x0040);
	tick(0xCFA0, 18);
	if ((int16_t)T.r[0] < 0x40)
	{
		// a fixed one: its timer is cleared
		tick(0xCFA2, 12);
		pokeb((uint16_t)(0x0AB8 + T.r[0]), 0);
		fl::clr();
		tick(0xCFA6, 30);
		tick(0xCFBA, 12);                              // BR
	}
	else
	{
		// a dynamic one counts the event of its definition (properties bits 0-3)
		tick(0xCFA8, 12);
		T.r[0] = peek(0x09B6);
		fl::mov(T.r[0]);
		tick(0xCFAC, 24);
		const uint8_t props = peekb((uint16_t)(0x4000 + T.r[0]));
		T.r[6] = (uint16_t)(T.r[6] - 2);
		pokeb(T.r[6], props);                          // MOVB ...,-(SP): the low byte only
		fl::movb(props);
		tick(0xCFB0, 39);
		ww(T.r[6], fl_logic(rw(T.r[6]) & 0x000F));
		tick(0xCFB4, 27);
		jsr(0xCFB8, 0xD348);                           // count_event
		fl::tst(rw(T.r[6]));
		T.r[6] = (uint16_t)(T.r[6] + 2);
		tick(0xCFBA, 18);
	}
	T.r[1] = peek(0x09AA);
	fl::mov(T.r[1]);
	tick(0xCFBE, 24);
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0xCFC0, 12);
	T.r[0] = fl_asl(T.r[0]);
	tick(0xCFC2, 12);
	poke((uint16_t)(0x0AF8 + T.r[0]), 0);
	fl::clr();
	tick(0xCFC6, 30);
	pokeb((uint16_t)(0x09B8 + T.r[1]), 0);
	fl::clr();
	tick(0xCFCA, 30);
	T.r[0] = peek(0x09B6);
	fl::mov(T.r[0]);
	tick(0xCFCE, 24);
	T.r[0] = sext8(peekb((uint16_t)(0x4000 + T.r[0])));
	fl::movb((uint8_t)T.r[0]);
	tick(0xCFD2, 27);
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0xCFD4, 18);
	rts();
	return T.r[0];
}

// F:CE24  add_map_object(int dynamic, int descriptor, int type): enter a map object in the
// first free entry (fixed ones from 0, dynamic ones from 0x0E98 = 0x40 on) unless its cell's
// definition is taken (F:BE7C, returns -1).  Fixed objects of types without 0x649D bit 7 get
// the timer 0x0AB8 = 0x0E9A + 8; definitions of types without 0x6438 bit 1 are marked taken.
// The highest entry in use (0x09AE fixed / 0x09B0 dynamic) is updated.  Returns 0.  0x09B2 is
// left pointing at the descriptor argument on the stack, as in the original.
uint16_t add_map_object(const Args &)
{
	c_save(4, 0xCE28);
	{
		const uint16_t v = peek(0x01D2);               // the banks, put back at the end
		fl::mov(v);
		push(v);
	}
	tick(0xCE2C, 36);
	{
		const uint16_t v = peek(0x01D4);
		fl::mov(v);
		push(v);
	}
	tick(0xCE30, 36);
	T.r[2] = rw(sp_at(0x14));                          // the descriptor
	fl::mov(T.r[2]);
	tick(0xCE34, 27);
	T.r[2] = fl_logic(T.r[2] & 0x01FF);                // its cell
	tick(0xCE38, 18);
	bank_a(0xCE38, 0x1000);
	bank_b(0xCE44, 0x1400);
	poke(0x09B2, 0x0014);                              // 0x09B2 -> the descriptor argument
	fl::mov(0x0014);
	tick(0xCE56, 33);
	poke(0x09B2, fl::add(peek(0x09B2), T.r[6]));
	tick(0xCE5A, 27);
	T.r[5] = 0;
	fl::clr();
	tick(0xCE5C, 12);
	const uint16_t dynamic = rw(sp_at(0x12));
	fl::tst(dynamic);
	tick(0xCE60, 27);
	if (!dynamic)
	{
		// fixed map objects from entry 0 (R5 = 1 marks them)
		tick(0xCE62, 12);
		T.r[5] = fl::inc(T.r[5]);
		tick(0xCE64, 12);
		T.r[2] = 0;
		fl::clr();
		tick(0xCE66, 12);
		T.r[1] = 0x09AE;
		fl::mov(T.r[1]);
		tick(0xCE6A, 18);
		poke(0x09AA, 0);
		fl::clr();
		tick(0xCE6E, 27);
		tick(0xCE80, 12);                              // BR
	}
	else
	{
		// dynamic ones from 0x0E98 on
		tick(0xCE70, 12);
		T.r[2] = 0;
		fl::clr();
		tick(0xCE72, 12);
		T.r[2] = fl_logicb(peekb(0x0E98));
		tick(0xCE76, 24);
		T.r[1] = 0x09B0;
		fl::mov(T.r[1]);
		tick(0xCE7A, 18);
		poke(0x09AA, 0x0040);
		fl::mov(0x0040);
		tick(0xCE80, 33);
	}
	jsr(0xCE84, 0xBE7C);                               // map_object_state
	fl::tst(T.r[0]);
	tick(0xCE86, 12);
	if (T.r[0])
	{
		// the definition is taken: banks back, -1
		tick(0xCE88, 12);
		{
			const uint16_t v = rw(sp_at(2));
			fl::mov(v);
			poke(0x01D2, v);
		}
		tick(0xCE8E, 42);
		{
			const uint16_t v = rw(T.r[6]);
			fl::mov(v);
			poke(0x01D4, v);
		}
		tick(0xCE92, 33);
		{
			const uint16_t v = peek(0x01D2);
			fl::mov(v);
			poke(0x1400, v);
		}
		tick(0xCE98, 39);
		{
			const uint16_t v = peek(0x01D4);
			fl::mov(v);
			poke(0x1402, v);
		}
		tick(0xCE9E, 39);
		T.r[0] = 0xFFFF;
		fl::mov(T.r[0]);
		tick(0xCEA2, 18);
		tick(0xCF5A, 12);                              // BR
	}
	else
	{
		tick(0xCEA4, 12);                              // BEQ
		for (;;)                                       // the first free entry
		{
			const uint8_t used = peekb((uint16_t)(0x09B8 + T.r[2]));
			fl::tstb(used);
			tick(0xCEA8, 27);
			if (!used)
			{
				tick(0xCEAE, 12);                      // BEQ
				break;
			}
			tick(0xCEAA, 12);
			T.r[2] = fl::inc(T.r[2]);
			tick(0xCEAC, 12);
			tick(0xCEA4, 12);                          // BR
		}
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0xCEB0, 12);
		T.r[0] = fl_asl(T.r[0]);
		tick(0xCEB2, 12);
		{
			const uint16_t d = rw(sp_at(0x14));
			poke((uint16_t)(0x0AF8 + T.r[0]), d);      // its descriptor
			fl::mov(d);
		}
		tick(0xCEB8, 45);
		poke(0x09AA, T.r[2]);
		fl::mov(T.r[2]);
		tick(0xCEBC, 27);
		jsr(0xCEC0, 0xBE10);                           // locate_map_object
		T.r[4] = peek(0x09B6);                         // the definition
		fl::mov(T.r[4]);
		tick(0xCEC4, 24);
		T.r[0] = T.r[4];
		fl::mov(T.r[0]);
		tick(0xCEC6, 12);
		T.r[0] = fl_asr(T.r[0]);
		tick(0xCEC8, 12);
		T.r[0] = fl_asr(T.r[0]);
		tick(0xCECA, 12);
		T.r[0] = fl_asr(T.r[0]);
		tick(0xCECC, 12);
		poke(0x01D2, 0);
		fl::clr();
		tick(0xCED0, 27);
		poke(0x1400, 0);
		fl::clr();
		tick(0xCED4, 27);
		T.r[3] = 0x0400;
		fl::mov(T.r[3]);
		tick(0xCED8, 18);
		poke(0x01D4, T.r[3]);
		fl::mov(T.r[3]);
		tick(0xCEDC, 27);
		poke(0x1402, T.r[3]);
		fl::mov(T.r[3]);
		tick(0xCEE0, 27);
		T.r[3] = rw(sp_at(0x16));                      // the type
		fl::mov(T.r[3]);
		tick(0xCEE4, 27);
		const uint8_t shared = fl_logicb((uint8_t)(peekb((uint16_t)(0x6438 + T.r[3])) & 0x02));
		tick(0xCEEA, 33);
		if (shared)
			tick(0xCEF6, 12);                          // BNE
		else
		{
			// the definition is marked taken (bitmap 0x0CF6)
			tick(0xCEEC, 12);
			T.r[4] = fl_logic(T.r[4] & 0x0007);
			tick(0xCEF0, 18);
			const uint16_t at = (uint16_t)(0x0CF6 + T.r[0]);
			pokeb(at, fl_logicb((uint8_t)(peekb(at) | peekb((uint16_t)(0xFC74 + T.r[4])))));
			tick(0xCEF6, 45);
		}
		T.r[5] = fl_ror(T.r[5]);
		tick(0xCEF8, 12);
		if (!fC())
			tick(0xCF0E, 12);                          // BCC: dynamic
		else
		{
			tick(0xCEFA, 12);
			const uint8_t tf = peekb((uint16_t)(0x649D + T.r[3]));
			fl::tstb(tf);
			tick(0xCEFE, 27);
			if (tf & 0x80)
				tick(0xCF0E, 12);                      // BMI: no timer
			else
			{
				// its timer, 8 ticks ahead
				tick(0xCF00, 12);
				T.r[0] = 0;
				fl::clr();
				tick(0xCF02, 12);
				T.r[0] = fl_logicb(peekb(0x0E9A));
				tick(0xCF06, 24);
				T.r[0] = fl::add(T.r[0], 0x0008);
				tick(0xCF0A, 18);
				pokeb((uint16_t)(0x0AB8 + T.r[2]), (uint8_t)T.r[0]);
				fl::movb((uint8_t)T.r[0]);
				tick(0xCF0E, 30);
			}
		}
		pokeb((uint16_t)(0x09B8 + T.r[2]), (uint8_t)T.r[3]);
		fl::movb((uint8_t)T.r[3]);
		tick(0xCF12, 30);
		poke(0x01D2, 0);
		fl::clr();
		tick(0xCF16, 27);
		poke(0x1400, 0);
		fl::clr();
		tick(0xCF1A, 27);
		T.r[0] = 0x0400;
		fl::mov(T.r[0]);
		tick(0xCF1E, 18);
		poke(0x01D4, T.r[0]);
		fl::mov(T.r[0]);
		tick(0xCF22, 27);
		poke(0x1402, T.r[0]);
		fl::mov(T.r[0]);
		tick(0xCF26, 27);
		poke(0x09AA, T.r[2]);
		fl::mov(T.r[2]);
		tick(0xCF2A, 27);
		const uint16_t top = peek(T.r[1]);             // the highest entry in use
		fl::cmp(T.r[2], top);
		tick(0xCF2C, 18);
		if ((int16_t)T.r[2] > (int16_t)top)
			tick(0xCF3A, 12);                          // BGT
		else
		{
			tick(0xCF2E, 12);
			T.r[2] = top;
			fl::mov(T.r[2]);
			tick(0xCF30, 18);
			for (;;)
			{
				const uint8_t used = peekb((uint16_t)(0x09B8 + T.r[2]));
				fl::tstb(used);
				tick(0xCF34, 27);
				if (used)
				{
					tick(0xCF3A, 12);                  // BNE
					break;
				}
				tick(0xCF36, 12);
				T.r[2] = fl::dec(T.r[2]);
				tick(0xCF38, 12);
				tick(0xCF30, 12);                      // BR
			}
		}
		poke(T.r[1], T.r[2]);
		fl::mov(T.r[2]);
		tick(0xCF3C, 21);
		pokeb(0x0E99, 0x01);
		fl::movb(0x01);
		tick(0xCF42, 33);
		{
			const uint16_t v = rw(sp_at(2));
			fl::mov(v);
			poke(0x01D2, v);
		}
		tick(0xCF48, 42);
		{
			const uint16_t v = rw(T.r[6]);
			fl::mov(v);
			poke(0x01D4, v);
		}
		tick(0xCF4C, 33);
		{
			const uint16_t v = peek(0x01D2);
			fl::mov(v);
			poke(0x1400, v);
		}
		tick(0xCF52, 39);
		{
			const uint16_t v = peek(0x01D4);
			fl::mov(v);
			poke(0x1402, v);
		}
		tick(0xCF58, 39);
		T.r[0] = 0;
		fl::clr();
		tick(0xCF5A, 12);
	}
	const uint16_t a = pop(), b = pop();
	fl::cmp(a, b);                                     // CMP (SP)+,(SP)+
	tick(0xCF5C, 24);
	c_restore(4);
	return T.r[0];
}

// F:CFD6: clear the map object tables (0x09AA-0x0E9A), set the empty ranges (0x09AE = -1,
// 0x09B0 = 0x3F, next dynamic entry 0x0E98 = 0x40) and add the level's fixed map objects from
// the list at 0x6696 (bank B 0x0400; descriptor, type; ends with 0xFFFF).  Banks are restored.
// Returns the last F:CE24 result.
uint16_t init_map_objects(const Args &)
{
	c_save(2, 0xCFDA);
	T.r[2] = peek(0x01D2);                             // the banks, put back at the end
	fl::mov(T.r[2]);
	tick(0xCFDE, 24);
	T.r[3] = peek(0x01D4);
	fl::mov(T.r[3]);
	tick(0xCFE2, 24);
	T.r[0] = 0x09AA;
	fl::mov(T.r[0]);
	tick(0xCFE6, 18);
	tick(0xCFEA, 12);                                  // BR
	for (;;)                                           // the tables 0x09AA-0x0E9A cleared
	{
		fl::cmp(T.r[0], 0x0E9A);
		tick(0xCFEE, 18);
		if (T.r[0] > 0x0E9A)
		{
			tick(0xCFF0, 12);                          // BLOS not taken
			break;
		}
		tick(0xCFE8, 12);
		pokeb(T.r[0], 0);
		T.r[0]++;
		fl::clr();
		tick(0xCFEA, 21);
	}
	pokeb(0x0E93, 0);
	fl::clr();
	tick(0xCFF4, 27);
	poke(0x09AE, 0xFFFF);                              // no fixed map objects
	fl::mov(0xFFFF);
	tick(0xCFFA, 33);
	poke(0x09B0, 0x003F);                              // no dynamic ones
	fl::mov(0x003F);
	tick(0xD000, 33);
	pokeb(0x0E98, 0x40);
	fl::movb(0x40);
	tick(0xD006, 33);
	poke(0x01D2, 0);
	fl::clr();
	tick(0xD00A, 27);
	poke(0x1400, 0);
	fl::clr();
	tick(0xD00E, 27);
	T.r[0] = 0x0400;
	fl::mov(T.r[0]);
	tick(0xD012, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xD016, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xD01A, 27);
	T.r[1] = 0x6696;                                   // the level's list (type, descriptor)
	fl::mov(T.r[1]);
	tick(0xD01E, 18);
	for (;;)
	{
		const uint16_t type = peek(T.r[1]);
		fl::cmp(type, 0xFFFF);
		tick(0xD022, 24);
		if (type == 0xFFFF)
		{
			tick(0xD03C, 12);                          // BEQ
			break;
		}
		tick(0xD024, 12);
		push(type);
		fl::mov(type);
		tick(0xD026, 30);
		{
			const uint16_t desc = peek((uint16_t)(T.r[1] + 2));
			fl::mov(desc);
			push(desc);
		}
		tick(0xD02A, 39);
		push(0x0001);
		fl::mov(0x0001);
		tick(0xD02E, 30);
		jsr(0xD032, 0xCE24);                           // add_map_object
		T.r[1] = fl::add(T.r[1], 0x0004);
		tick(0xD036, 18);
		T.r[6] = fl::add(T.r[6], 0x0006);
		tick(0xD03A, 18);
		tick(0xD01E, 12);                              // BR
	}
	poke(0x01D2, T.r[2]);
	fl::mov(T.r[2]);
	tick(0xD040, 27);
	poke(0x01D4, T.r[3]);
	fl::mov(T.r[3]);
	tick(0xD044, 27);
	{
		const uint16_t v = peek(0x01D2);
		fl::mov(v);
		poke(0x1400, v);
	}
	tick(0xD04A, 39);
	{
		const uint16_t v = peek(0x01D4);
		fl::mov(v);
		poke(0x1402, v);
	}
	tick(0xD050, 39);
	c_restore(2);
	return T.r[0];
}

// F:C3E6: the object of the current map object goes off screen: type 4 stays; types counted
// in 0x0E97 / 0x0E96 (0x649D bits 6 / 4) count up; objects flagged 0x10 (0x04B0) remove their
// map object for good (F:CF5E); the others are reset (slot 5 also resets slot 6), the map
// object keeps its type in 0x09B8 and loses its slot flag; fixed map objects get a timer from
// the animation counter (unless 0x649D bit 7) and A5:5D08 is told the type.  Banks restored.
uint16_t map_object_leaves(const Args &)
{
	c_save(4, 0xC3EA);
	T.r[4] = peek(0x01D2);
	fl::mov(T.r[4]);
	tick(0xC3EE, 24);
	T.r[5] = peek(0x01D4);
	fl::mov(T.r[5]);
	tick(0xC3F2, 24);
	poke(0x01D2, 0);
	fl::clr();
	tick(0xC3F6, 27);
	poke(0x1400, 0);
	fl::clr();
	tick(0xC3FA, 27);
	T.r[0] = 0x0400;
	fl::mov(T.r[0]);
	tick(0xC3FE, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xC402, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xC406, 27);
	T.r[1] = peek(0x09AA);
	fl::mov(T.r[1]);
	tick(0xC40A, 24);
	T.r[3] = sext8(peekb((uint16_t)(0x09B8 + T.r[1])));
	fl::movb((uint8_t)T.r[3]);
	tick(0xC40E, 27);
	T.r[3] &= 0x003F;                                  // the slot
	fl::logic(T.r[3]);
	tick(0xC412, 18);
	T.r[2] = 0;
	fl::clr();
	tick(0xC414, 12);
	T.r[2] |= peekb((uint16_t)(0x0370 + T.r[3]));      // its type
	fl::logicb((uint8_t)T.r[2]);
	tick(0xC418, 27);
	fl::cmp(T.r[2], 0x0004);
	tick(0xC41C, 18);
	if (fZ())
	{
		tick(0xC41E, 12);                              // type 4 stays
		poke(0x01D2, T.r[4]);
		fl::mov(T.r[4]);
		tick(0xC422, 27);
		poke(0x01D4, T.r[5]);
		fl::mov(T.r[5]);
		tick(0xC426, 27);
		poke(0x1400, peek(0x01D2));
		fl::mov(peek(0x01D2));
		tick(0xC42C, 39);
		poke(0x1402, peek(0x01D4));
		fl::mov(peek(0x01D4));
		tick(0xC432, 39);
		c_restore(4);
		return T.r[0];
	}
	tick(0xC434, 12);                                  // BNE
	const bool counted97 = (peekb((uint16_t)(0x649D + T.r[2])) & 0x40) != 0;
	fl::logicb((uint8_t)(0x40 & peekb((uint16_t)(T.r[2] + 0x649D))));
	tick(0xC43A, 33);
	if (counted97)
	{
		tick(0xC43C, 12);
		T.r[0] = sext8(peekb(0x0E97));
		fl::movb((uint8_t)T.r[0]);
		tick(0xC440, 24);
		T.r[0] = fl::inc(T.r[0]);
		tick(0xC442, 12);
		pokeb(0x0E97, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0xC446, 27);
	}
	else
		tick(0xC446, 12);                              // BEQ
	const bool counted96 = (peekb((uint16_t)(0x649D + T.r[2])) & 0x10) != 0;
	fl::logicb((uint8_t)(0x10 & peekb((uint16_t)(T.r[2] + 0x649D))));
	tick(0xC44C, 33);
	if (counted96)
	{
		tick(0xC44E, 12);
		T.r[0] = sext8(peekb(0x0E96));
		fl::movb((uint8_t)T.r[0]);
		tick(0xC452, 24);
		T.r[0] = fl::inc(T.r[0]);
		tick(0xC454, 12);
		pokeb(0x0E96, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0xC458, 27);
	}
	else
		tick(0xC458, 12);                              // BEQ
	const bool removed = (peekb((uint16_t)(0x04B0 + T.r[3])) & 0x10) != 0;
	fl::logicb((uint8_t)(peekb((uint16_t)(T.r[3] + 0x04B0)) & 0x10));
	tick(0xC45E, 33);
	if (removed)
	{
		tick(0xC460, 12);                              // gone for good
		push(peek(0x09AA));
		fl::mov(peek(T.r[6]));
		tick(0xC464, 36);
		jsr(0xC468, 0xCF5E);
		tick(0xC4CC, 12);                              // BR
	}
	else
	{
		tick(0xC46A, 12);                              // BEQ
		T.r[2] = 0;
		fl::clr();
		tick(0xC46C, 12);
		T.r[2] |= peekb((uint16_t)(0x0370 + T.r[3]));
		fl::logicb((uint8_t)T.r[2]);
		tick(0xC470, 27);
		pokeb((uint16_t)(0x09B8 + T.r[1]), (uint8_t)T.r[2]);   // the map object keeps the type
		fl::movb((uint8_t)T.r[2]);
		tick(0xC474, 30);
		T.r[0] = T.r[3];
		fl::mov(T.r[0]);
		tick(0xC476, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0xC478, 12);
		T.r[1] = peek((uint16_t)(0x0430 + T.r[0]));    // the animation counter
		fl::mov(T.r[1]);
		tick(0xC47C, 27);
		push(T.r[3]);
		fl::mov(T.r[3]);
		tick(0xC47E, 24);
		jsr(0xC482, 0xB56E);
		fl::cmp(T.r[3], 0x0005);
		tick(0xC486, 18);                              // CMP R3,#5
		if (T.r[3] == 5)
		{
			tick(0xC488, 12);
			poke(T.r[6], 0x0006);
			fl::mov(0x0006);
			tick(0xC48C, 27);
			jsr(0xC490, 0xB56E);
		}
		else
			tick(0xC490, 12);                          // BNE
		T.r[3] = peek(0x09AA);
		fl::mov(T.r[3]);
		tick(0xC494, 24);
		T.r[0] = T.r[3];
		fl::mov(T.r[0]);
		tick(0xC496, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0xC498, 12);
		poke((uint16_t)(0x0AF8 + T.r[0]), fl::logic((uint16_t)(peek((uint16_t)(0x0AF8 + T.r[0])) & 0x7FFF)));
		tick(0xC49E, 36);
		const bool dynamic = (int16_t)T.r[3] >= 0x40;
		fl::cmp(T.r[3], 0x0040);
		tick(0xC4A2, 18);
		if (dynamic)
			tick(0xC4CC, 12);                          // BGE
		else
		{
			tick(0xC4A4, 12);
			const bool untimed = (peekb((uint16_t)(0x649D + T.r[2])) & 0x80) != 0;
			fl::tstb(peekb((uint16_t)(T.r[2] + 0x649D)));
			tick(0xC4A8, 27);
			if (untimed)
				tick(0xC4BA, 12);                      // BMI
			else
			{
				tick(0xC4AA, 12);                      // a timer from the counter
				T.r[1] = fl::asr(T.r[1]);
				tick(0xC4AC, 12);
				T.r[1] = fl::asr(T.r[1]);
				tick(0xC4AE, 12);
				T.r[0] = 0;
				fl::clr();
				tick(0xC4B0, 12);
				T.r[0] |= peekb(0x0E9A);
				fl::logicb((uint8_t)T.r[0]);
				tick(0xC4B4, 24);
				T.r[1] = fl::add(T.r[1], T.r[0]);
				tick(0xC4B6, 12);
				pokeb((uint16_t)(0x0AB8 + T.r[3]), (uint8_t)T.r[1]);
				fl::movb((uint8_t)T.r[1]);
				tick(0xC4BA, 30);
			}
			bank_a(0xC4BA, 0x1800);
			poke(T.r[6], T.r[2]);
			tick(0xC4C8, 21);
			jsr(0xC4CC, 0x5D08);
		}
	}
	poke(0x01D2, T.r[4]);
	fl::mov(T.r[4]);
	tick(0xC4D0, 27);
	poke(0x01D4, T.r[5]);
	fl::mov(T.r[5]);
	tick(0xC4D4, 27);
	poke(0x1400, peek(0x01D2));
	fl::mov(peek(0x01D2));
	tick(0xC4DA, 39);
	poke(0x1402, peek(0x01D4));
	fl::mov(peek(0x01D4));
	tick(0xC4E0, 39);
	fl::tst(pop());
	tick(0xC4E2, 18);
	c_restore(4);
	return T.r[0];
}

// F:C4E4: create the object of the current map object (0x09AA, type in 0x09B8): types counted
// down in 0x0E96 (dynamic map objects, 0x649D bit 4) or 0x0E97 (fixed ones, bit 6; this also
// plays the type's sound, A5:5CC0) need a count left.  Type 1 takes slot 5 (if free, 0x0375)
// and is placed by F:DF40, type 4 slot 7 (F:E0C2), the others a free slot (F:B5B8) at their
// definition's position (F:BF5E) facing the record's direction.  Animation, palette and flags
// come from the type tables (bank B 0x0400); the object is shown (F:B4B6) and the map object
// records its slot and on-screen sides.  Returns 2 * map object (or what was left in R0 when
// nothing was created).
uint16_t create_map_object(const Args &)
{
	c_save(4, 0xC4E8);
	T.r[6] = fl::sub(T.r[6], 0x0E);
	tick(0xC4EC, 18);
	push(peek(0x01D2));                                // banks: 2(SP) A, (SP) B
	fl::mov(peek(T.r[6]));
	tick(0xC4F0, 36);
	push(peek(0x01D4));
	fl::mov(peek(T.r[6]));
	tick(0xC4F4, 36);
	poke(0x01D2, 0);
	fl::clr();
	tick(0xC4F8, 27);
	poke(0x1400, 0);
	fl::clr();
	tick(0xC4FC, 27);
	T.r[0] = 0x0400;
	fl::mov(T.r[0]);
	tick(0xC500, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xC504, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xC508, 27);
	T.r[0] = peek(0x09AA);
	fl::mov(T.r[0]);
	tick(0xC50C, 24);
	T.r[2] = sext8(peekb((uint16_t)(0x09B8 + T.r[0]))); // the type
	fl::movb((uint8_t)T.r[2]);
	tick(0xC510, 27);
	bool exit_now = false, removed = false;
	const bool dynamic = (int16_t)T.r[0] >= 0x40;
	fl::cmp(T.r[0], 0x0040);
	tick(0xC514, 18);
	if (dynamic)
	{
		// counted types (0x649D bit 4) need a count left in 0x0E96
		tick(0xC516, 12);
		const bool counted = (peekb((uint16_t)(0x649D + T.r[2])) & 0x10) != 0;
		fl::logicb((uint8_t)(0x10 & peekb((uint16_t)(T.r[2] + 0x649D))));
		tick(0xC51C, 33);
		if (!counted)
			tick(0xC592, 12);                          // BEQ
		else
		{
			tick(0xC51E, 12);
			T.r[0] = sext8(peekb(0x0E96));
			fl::movb((uint8_t)T.r[0]);
			tick(0xC522, 24);
			if (!T.r[0])
			{
				tick(0xC5B4, 12);                      // BEQ: none left
				exit_now = true;
			}
			else
			{
				tick(0xC524, 12);
				T.r[1] = T.r[0];
				fl::mov(T.r[1]);
				tick(0xC526, 12);
				T.r[1] = fl::dec(T.r[1]);
				tick(0xC528, 12);
				pokeb(0x0E96, (uint8_t)T.r[1]);
				fl::movb((uint8_t)T.r[1]);
				tick(0xC52C, 27);
				tick(0xC592, 12);                      // BR
			}
		}
	}
	else
	{
		tick(0xC52E, 12);                              // BLT
		const bool counted = (peekb((uint16_t)(0x649D + T.r[2])) & 0x40) != 0;
		fl::logicb((uint8_t)(0x40 & peekb((uint16_t)(T.r[2] + 0x649D))));
		tick(0xC534, 33);
		if (!counted)
			tick(0xC592, 12);                          // BEQ
		else
		{
			tick(0xC536, 12);
			T.r[0] = sext8(peekb(0x0E97));
			fl::movb((uint8_t)T.r[0]);
			tick(0xC53A, 24);
			if (!T.r[0])
			{
				// none left: the map object is counted (F:D348) and removed (F:CF5E)
				tick(0xC53C, 12);
				fl::cmp(T.r[2], 0x002F);
				tick(0xC540, 18);                      // CMP R2,#0x2F
				if ((int16_t)T.r[2] >= 0x2F)
				{
					tick(0xC550, 12);
					T.r[4] = T.r[2];
					fl::mov(T.r[4]);
					tick(0xC552, 12);
					T.r[4] = fl::sub(T.r[4], 0x002F);
					tick(0xC556, 18);
					T.r[1] = T.r[4];
					fl::mov(T.r[1]);
					tick(0xC558, 12);
					T.r[1] = fl::add(T.r[1], 0x0012);
					tick(0xC55C, 18);
				}
				else
				{
					tick(0xC542, 12);
					T.r[0] = T.r[2];
					fl::mov(T.r[0]);
					tick(0xC544, 12);
					T.r[0] = fl::sub(T.r[0], 0x000A);
					tick(0xC548, 18);
					T.r[1] = T.r[0];
					fl::mov(T.r[1]);
					tick(0xC54A, 12);
					T.r[1] = fl::add(T.r[1], 0x000B);
					tick(0xC54E, 18);
					tick(0xC55C, 12);                  // BR
				}
				push(T.r[1]);
				fl::mov(T.r[1]);
				tick(0xC55E, 24);
				jsr(0xC562, 0xD348);
				poke(T.r[6], peek(0x09AA));
				fl::mov(peek(T.r[6]));
				tick(0xC566, 33);
				jsr(0xC56A, 0xCF5E);
				tick(0xC830, 18);                      // JMP
				removed = true;
			}
			else
			{
				tick(0xC56E, 12);                      // BNE
				T.r[1] = T.r[0];
				fl::mov(T.r[1]);
				tick(0xC570, 12);
				T.r[1] = fl::dec(T.r[1]);
				tick(0xC572, 12);
				pokeb(0x0E97, (uint8_t)T.r[1]);
				fl::movb((uint8_t)T.r[1]);
				tick(0xC576, 27);
				bank_a(0xC576, 0x1800);
				push(T.r[2]);
				tick(0xC584, 24);
				jsr(0xC588, 0x5CC0);
				poke(0x01D2, 0);
				fl::clr();
				tick(0xC58C, 27);
				poke(0x1400, 0);
				fl::clr();
				tick(0xC590, 27);
				pop();
				fl::tst(peek((uint16_t)(T.r[6] - 2)));
				tick(0xC592, 18);                      // TST (SP)+
			}
		}
	}
	if (!exit_now && !removed)
	{
		// an object slot: 5 for type 1 (when free), 7 for type 4, else F:B5B8
		fl::cmp(T.r[2], 0x0001);
		tick(0xC596, 18);
		bool got = false;
		if (fZ())
		{
			tick(0xC5AE, 12);                          // BEQ
			const bool taken = peekb(0x0375) != 0;
			fl::tstb(peekb(0x0375));
			tick(0xC5B2, 24);
			if (taken)
			{
				tick(0xC5B4, 12);                      // BEQ not taken
				exit_now = true;
			}
			else
			{
				tick(0xC5CE, 12);
				T.r[3] = 0x0005;
				fl::mov(T.r[3]);
				tick(0xC5D2, 18);
				got = true;
			}
		}
		else
		{
			tick(0xC598, 12);
			fl::cmp(T.r[2], 0x0004);
			tick(0xC59C, 18);
			if (fZ())
			{
				tick(0xC59E, 12);
				T.r[3] = 0x0007;
				fl::mov(T.r[3]);
				tick(0xC5A2, 18);
				tick(0xC5AA, 12);                      // BR
			}
			else
			{
				tick(0xC5A4, 12);                      // BNE
				jsr(0xC5A8, 0xB5B8);
				T.r[3] = T.r[0];
				fl::mov(T.r[3]);
				tick(0xC5AA, 12);
			}
			if ((int16_t)T.r[3] >= 0)
			{
				tick(0xC5D2, 12);                      // BGE
				got = true;
			}
			else
			{
				tick(0xC5AC, 12);
				tick(0xC5B4, 12);                      // BR: no slot
				exit_now = true;
			}
		}
		(void)got;
	}
	if (exit_now)
	{
		poke(0x01D2, peek((uint16_t)(T.r[6] + 2)));
		fl::mov(peek(0x01D2));
		tick(0xC5BA, 42);
		poke(0x01D4, peek(T.r[6]));
		fl::mov(peek(0x01D4));
		tick(0xC5BE, 33);
		poke(0x1400, peek(0x01D2));
		fl::mov(peek(0x01D2));
		tick(0xC5C4, 39);
		poke(0x1402, peek(0x01D4));
		fl::mov(peek(0x01D4));
		tick(0xC5CA, 39);
		tick(0xC84A, 18);                              // JMP
		T.r[6] = fl::add(T.r[6], 0x0012);
		tick(0xC84E, 18);
		c_restore(4);
		return T.r[0];
	}
	if (!removed)
	{
		pokeb((uint16_t)(0x0370 + T.r[3]), (uint8_t)T.r[2]);
		tick(0xC5D6, 30);
		pokeb((uint16_t)(0x0390 + T.r[3]), peekb(0x09AA));
		fl::movb(peekb(0x09AA));
		tick(0xC5DC, 42);
		poke(sp_at(4), 0x0470);
		fl::mov(0x0470);
		tick(0xC5E2, 36);
		poke(sp_at(4), fl::add(peek(sp_at(4)), T.r[3]));
		tick(0xC5E6, 30);
		poke(sp_at(0x0C), 0x6438);
		fl::mov(0x6438);
		tick(0xC5EC, 36);
		poke(sp_at(0x0C), fl::add(peek(sp_at(0x0C)), T.r[2]));
		tick(0xC5F0, 30);
		pokeb(peek(sp_at(4)), peekb(peek(sp_at(0x0C))));   // palette byte from 0x6438[type]
		tick(0xC5F6, 57);
		pokeb(peek(sp_at(4)), fl::logicb((uint8_t)(peekb(peek(sp_at(4))) & 0x30)));
		tick(0xC5FC, 42);
		fl::cmp(T.r[2], 0x0001);
		tick(0xC600, 18);
		if (fZ())
		{
			// type 1: object scripts (F:DF40) with the two locals
			tick(0xC602, 12);
			push(0x0010);
			fl::mov(0x0010);
			tick(0xC606, 30);
			poke(T.r[6], fl::add(peek(T.r[6]), T.r[6]));
			tick(0xC608, 21);
			push(0x0014);
			fl::mov(0x0014);
			tick(0xC60C, 30);
			poke(T.r[6], fl::add(peek(T.r[6]), T.r[6]));
			tick(0xC60E, 21);
			jsr(0xC612, 0xDF40);
			pop();
			fl::tst(peek((uint16_t)(T.r[6] - 2)));
			tick(0xC614, 18);                          // TST (SP)+
			tick(0xC7B2, 18);                          // JMP
		}
		else
		{
			tick(0xC618, 12);                          // BNE
			T.r[5] = T.r[3];
			fl::mov(T.r[5]);
			tick(0xC61A, 12);
			T.r[5] = fl::asl(T.r[5]);
			tick(0xC61C, 12);
			poke(sp_at(6), 0x06F0);
			fl::mov(0x06F0);
			tick(0xC622, 36);
			poke(sp_at(6), fl::add(peek(sp_at(6)), T.r[5]));
			tick(0xC626, 30);
			T.r[4] = T.r[2];
			fl::mov(T.r[4]);
			tick(0xC628, 12);
			T.r[4] = fl::asl(T.r[4]);
			tick(0xC62A, 12);
			poke(peek(sp_at(6)), peek((uint16_t)(0x62A4 + T.r[4])));
			tick(0xC630, 51);
			poke(sp_at(0x0A), 0x0730);
			fl::mov(0x0730);
			tick(0xC636, 36);
			poke(sp_at(0x0A), fl::add(peek(sp_at(0x0A)), T.r[5]));
			tick(0xC63A, 30);
			poke(peek(sp_at(0x0A)), peek((uint16_t)(0x636E + T.r[4])));
			tick(0xC640, 51);
			poke(sp_at(8), 0x0770);
			fl::mov(0x0770);
			tick(0xC646, 36);
			poke(sp_at(8), fl::add(peek(sp_at(8)), T.r[3]));
			tick(0xC64A, 30);
			pokeb(peek(sp_at(8)), peekb((uint16_t)(0x6502 + T.r[2])));
			tick(0xC650, 51);
			pokeb((uint16_t)(0x0790 + T.r[3]), peekb((uint16_t)(0x6502 + T.r[2])));
			tick(0xC656, 45);
			bank_a(0xC656, 0x1000);
			bank_b(0xC662, 0x1400);
			jsr(0xC672, 0xBE10);
			pokeb(peek(sp_at(4)), fl::logicb((uint8_t)(peekb(peek(sp_at(4))) | 0x40)));
			tick(0xC678, 42);
			T.r[0] = peek(0x09B6);
			fl::mov(T.r[0]);
			tick(0xC67C, 24);
			const bool bit5 = (peekb((uint16_t)(0x4560 + T.r[0])) & 0x20) != 0;
			fl::logicb((uint8_t)(0x20 & peekb((uint16_t)(T.r[0] + 0x4560))));
			tick(0xC682, 33);
			if (bit5)
			{
				tick(0xC684, 12);
				pokeb(peek(sp_at(4)), fl::logicb((uint8_t)(peekb(peek(sp_at(4))) | 0x80)));
				tick(0xC68A, 42);
			}
			else
				tick(0xC68A, 12);                      // BEQ
			fl::cmp(T.r[2], 0x0004);
			tick(0xC68E, 18);
			if (fZ())
			{
				tick(0xC6BC, 12);                      // BEQ: type 4
				push(T.r[3]);
				fl::mov(T.r[3]);
				tick(0xC6BE, 24);
				jsr(0xC6C2, 0xE0C2);
				poke(sp_at(0x10), peek(peek(sp_at(8))));
				fl::mov(peek((uint16_t)(T.r[6] + 0x10)));
				tick(0xC6C8, 51);
				poke(sp_at(0x12), peek(peek(sp_at(0x0C))));
				fl::mov(peek((uint16_t)(T.r[6] + 0x12)));
				tick(0xC6CE, 51);
				T.r[0] = 0x0006;
				fl::mov(T.r[0]);
				tick(0xC6D2, 18);
			}
			else
			{
				// at the definition's position (F:BF5E) facing the record's direction
				tick(0xC690, 12);
				push(0x0010);
				fl::mov(0x0010);
				tick(0xC694, 30);
				poke(T.r[6], fl::add(peek(T.r[6]), T.r[6]));
				tick(0xC696, 21);
				push(0x0014);
				fl::mov(0x0014);
				tick(0xC69A, 30);
				poke(T.r[6], fl::add(peek(T.r[6]), T.r[6]));
				tick(0xC69C, 21);
				jsr(0xC6A0, 0xBF5E);
				poke(peek(sp_at(0x0A)), fl::add(peek(peek(sp_at(0x0A))), peek(sp_at(0x12))));
				tick(0xC6A6, 51);
				poke(peek(sp_at(0x0E)), fl::add(peek(peek(sp_at(0x0E))), peek(sp_at(0x14))));
				tick(0xC6AC, 51);
				T.r[0] = peek(0x09B2);
				fl::mov(T.r[0]);
				tick(0xC6B0, 24);
				T.r[0] = sext8(peekb((uint16_t)(T.r[0] + 2)));
				fl::movb((uint8_t)T.r[0]);
				tick(0xC6B4, 27);
				T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
				tick(0xC6B8, 18);
				pop();
				fl::tst(peek((uint16_t)(T.r[6] - 2)));
				tick(0xC6BA, 18);                      // TST (SP)+
				tick(0xC6D2, 12);                      // BR
			}
			pokeb((uint16_t)(0x07D0 + T.r[3]), (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0xC6D6, 30);
			poke(0x01D2, 0);
			fl::clr();
			tick(0xC6DA, 27);
			poke(0x1400, 0);
			fl::clr();
			tick(0xC6DE, 27);
			poke(sp_at(0x0C), 0x0400);
			fl::mov(0x0400);
			tick(0xC6E4, 36);
			poke(0x01D4, 0x0400);
			fl::mov(0x0400);
			tick(0xC6EA, 33);
			poke(0x1402, peek(sp_at(0x0C)));
			fl::mov(peek((uint16_t)(T.r[6] + 0x0C)));
			tick(0xC6F0, 42);
			const bool fixed_frame = (peekb(peek(sp_at(0x0E))) & 0x80) != 0;
			tick(0xC6F4, 33);
			if (fixed_frame)
			{
				tick(0xC6F6, 12);
				poke((uint16_t)(0x03B0 + T.r[5]), peek((uint16_t)(0x5F7C + T.r[4])));
				tick(0xC6FC, 45);
				poke((uint16_t)(0x03F0 + T.r[5]), peek((uint16_t)(0x6110 + T.r[4])));
				tick(0xC702, 45);
				const bool once = (peekb(peek(sp_at(0x0E))) & 0x04) != 0;
				fl::logicb((uint8_t)(0x04 & peekb(rw((uint16_t)(T.r[6] + 0x000E)))));
				tick(0xC708, 39);
				if (once)
				{
					tick(0xC70A, 12);
					pokeb((uint16_t)(0x04B0 + T.r[3]), fl::logicb((uint8_t)(peekb((uint16_t)(0x04B0 + T.r[3])) | 0x20)));
					tick(0xC710, 36);
				}
				else
					tick(0xC710, 12);                  // BEQ
				fl::cmp(T.r[2], 0x0057);
				tick(0xC714, 18);
				bool fast = false;
				if (fZ())
				{
					tick(0xC716, 12);
					const bool f = peekb(0x01E7) != 0;
					fl::tstb(peekb(0x01E7));
					tick(0xC71A, 24);
					if (f)
					{
						tick(0xC71C, 12);
						fast = true;
					}
					else
						tick(0xC768, 12);              // BEQ
				}
				else
					tick(0xC768, 12);                  // BNE
				if (fast)
				{
					poke(sp_at(0x0C), 0x0005);
					tick(0xC722, 36);
					pokeb((uint16_t)(0x0790 + T.r[3]), 0x05);
					fl::movb(0x05);
					tick(0xC728, 36);
					pokeb(peek(sp_at(0x0A)), 0x05);
					fl::movb(0x05);
					tick(0xC72E, 42);
					tick(0xC768, 12);                  // BR
				}
			}
			else
			{
				// the frame list of the direction (0x5F7C[type] + 0xFDBA[direction])
				tick(0xC730, 12);                      // BPL
				{
					const uint8_t b = peekb((uint16_t)(0xFDBA + T.r[0]));
					T.r[6] = (uint16_t)(T.r[6] - 2);
					pokeb(T.r[6], b);
				}
				fl::movb(peekb(T.r[6]));
				tick(0xC734, 39);
				pokeb((uint16_t)(T.r[6] + 1), 0);
				fl::clr();
				tick(0xC738, 30);
				poke(T.r[6], fl::add(peek(T.r[6]), peek((uint16_t)(0x5F7C + T.r[4]))));
				tick(0xC73C, 36);
				{
					const uint16_t v = pop();
					poke(sp_at(0x0C), v);
				}
				fl::mov(peek((uint16_t)(T.r[6] + 0x0C)));
				tick(0xC740, 36);
				T.r[1] = peek(peek(sp_at(0x0C)));
				fl::mov(T.r[1]);
				tick(0xC744, 33);
				const bool after = peekb((uint16_t)(0x6502 + T.r[2])) != 0;
				fl::tstb(peekb((uint16_t)(T.r[2] + 0x6502)));
				tick(0xC748, 27);
				if (after)
				{
					tick(0xC74A, 12);
					T.r[1] = peek(T.r[1]);
					fl::mov(T.r[1]);
					tick(0xC74C, 18);
					pokeb(peek(sp_at(0x0A)), fl::logicb((uint8_t)(peekb(peek(sp_at(0x0A))) | 0x80)));
					tick(0xC752, 42);
				}
				else
					tick(0xC752, 12);                  // BEQ
				poke((uint16_t)(0x03B0 + T.r[5]), T.r[1]);
				fl::mov(T.r[1]);
				tick(0xC756, 30);
				T.r[4] = peek((uint16_t)(0x6110 + T.r[4]));
				fl::mov(T.r[4]);
				tick(0xC75A, 27);
				{
					const uint8_t b = peekb((uint16_t)(0xFDBA + T.r[0]));
					T.r[6] = (uint16_t)(T.r[6] - 2);
					pokeb(T.r[6], b);
				}
				fl::movb(peekb(T.r[6]));
				tick(0xC75E, 39);
				pokeb((uint16_t)(T.r[6] + 1), 0);
				fl::clr();
				tick(0xC762, 30);
				T.r[4] = fl::add(T.r[4], pop());
				tick(0xC764, 18);
				poke((uint16_t)(0x03F0 + T.r[5]), peek(T.r[4]));
				tick(0xC768, 36);
			}
			// moving types and the animation counter (from the timer, or 0x10)
			T.r[0] = sext8(peekb((uint16_t)(0x649D + T.r[2])));
			fl::movb((uint8_t)T.r[0]);
			tick(0xC76C, 27);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0001));
			tick(0xC770, 18);
			pokeb((uint16_t)(0x0490 + T.r[3]), fl::logicb((uint8_t)(peekb((uint16_t)(0x0490 + T.r[3])) | T.r[0])));
			tick(0xC774, 30);
			T.r[0] = 0;
			fl::clr();
			tick(0xC776, 12);
			T.r[0] |= peekb((uint16_t)(0x623F + T.r[2]));
			fl::logicb((uint8_t)T.r[0]);
			tick(0xC77A, 27);
			fl::tst(T.r[0]);
			tick(0xC77C, 12);                          // TST R0
			if (!T.r[0])
				tick(0xC7B2, 12);                      // BEQ
			else
			{
				tick(0xC77E, 12);
				fl::cmp(T.r[0], 0x0006);
				tick(0xC782, 18);                      // CMP R0,#6
				if (T.r[0] == 6)
					tick(0xC7B2, 12);                  // BEQ
				else
				{
					tick(0xC784, 12);
					T.r[0] = peek(0x09AA);
					fl::mov(T.r[0]);
					tick(0xC788, 24);
					const bool dyn = (int16_t)T.r[0] >= 0x40;
					fl::cmp(T.r[0], 0x0040);
					tick(0xC78C, 18);
					if (dyn)
					{
						tick(0xC7AA, 12);              // BGE
						T.r[1] = 0x0010;
						fl::mov(T.r[1]);
						tick(0xC7AE, 18);
					}
					else
					{
						tick(0xC78E, 12);
						T.r[4] = 0;
						fl::clr();
						tick(0xC790, 12);
						T.r[4] |= peekb((uint16_t)(0x0AB8 + T.r[0]));
						fl::logicb((uint8_t)T.r[4]);
						tick(0xC794, 27);
						T.r[2] = 0;
						fl::clr();
						tick(0xC796, 12);
						T.r[2] |= peekb(0x0E9A);
						fl::logicb((uint8_t)T.r[2]);
						tick(0xC79A, 24);
						T.r[4] = fl::sub(T.r[4], T.r[2]);
						tick(0xC79C, 12);
						T.r[1] = 0;
						fl::clr();
						tick(0xC79E, 12);
						T.r[1] |= (uint8_t)T.r[4];
						fl::logicb((uint8_t)T.r[1]);
						tick(0xC7A0, 12);
						T.r[1] = fl::asl(T.r[1]);
						tick(0xC7A2, 12);
						T.r[1] = fl::asl(T.r[1]);
						tick(0xC7A4, 12);
						pokeb((uint16_t)(0x0AB8 + T.r[0]), 0);
						fl::clr();
						tick(0xC7A8, 30);
						tick(0xC7AE, 12);              // BR
					}
					poke((uint16_t)(0x0430 + T.r[5]), T.r[1]);
					fl::mov(T.r[1]);
					tick(0xC7B2, 30);
				}
			}
		}
		// shown (F:B4B6; bit 6 of the palette source: without pieces); slot and flags recorded
		poke(0x01D2, 0);
		fl::clr();
		tick(0xC7B6, 27);
		poke(0x1400, 0);
		fl::clr();
		tick(0xC7BA, 27);
		T.r[0] = 0x0400;
		fl::mov(T.r[0]);
		tick(0xC7BE, 18);
		poke(0x01D4, T.r[0]);
		fl::mov(T.r[0]);
		tick(0xC7C2, 27);
		poke(0x1402, T.r[0]);
		fl::mov(T.r[0]);
		tick(0xC7C6, 27);
		const bool flag10 = (peekb(peek(sp_at(0x0E))) & 0x08) != 0;
		fl::logicb((uint8_t)(0x08 & peekb(rw((uint16_t)(T.r[6] + 0x000E)))));
		tick(0xC7CC, 39);
		if (flag10)
		{
			tick(0xC7CE, 12);
			pokeb((uint16_t)(0x04B0 + T.r[3]), fl::logicb((uint8_t)(peekb((uint16_t)(0x04B0 + T.r[3])) | 0x10)));
			tick(0xC7D4, 36);
		}
		else
			tick(0xC7D4, 12);                          // BEQ
		const bool bare = (peekb(peek(sp_at(0x0E))) & 0x40) != 0;
		fl::logicb((uint8_t)(0x40 & peekb(rw((uint16_t)(T.r[6] + 0x000E)))));
		tick(0xC7DA, 39);
		if (!bare)
		{
			tick(0xC7DC, 12);
			poke(T.r[6], T.r[3]);
			fl::mov(T.r[3]);
			tick(0xC7DE, 21);
			tick(0xC7E6, 12);                          // BR
		}
		else
		{
			tick(0xC7E0, 12);                          // BNE
			poke(T.r[6], T.r[3]);
			fl::mov(T.r[3]);
			tick(0xC7E2, 21);
			poke(T.r[6], fl::logic((uint16_t)(peek(T.r[6]) | 0x0080)));
			tick(0xC7E6, 27);
		}
		jsr(0xC7EA, 0xB4B6);
		T.r[0] = peek(0x09AA);
		fl::mov(T.r[0]);
		tick(0xC7EE, 24);
		T.r[4] = 0x09B8;
		fl::mov(T.r[4]);
		tick(0xC7F2, 18);
		T.r[4] = fl::add(T.r[4], T.r[0]);
		tick(0xC7F4, 12);
		pokeb(T.r[4], (uint8_t)T.r[3]);
		fl::movb((uint8_t)T.r[3]);
		tick(0xC7F6, 21);
		T.r[0] = fl::asl(T.r[0]);
		tick(0xC7F8, 12);
		poke((uint16_t)(0x0AF8 + T.r[0]), fl::logic((uint16_t)(peek((uint16_t)(0x0AF8 + T.r[0])) | 0x8000)));
		tick(0xC7FE, 36);
		T.r[3] = peek((uint16_t)(0x0AF8 + T.r[0]));
		fl::mov(T.r[3]);
		tick(0xC802, 27);
		T.r[3] = fl::logic((uint16_t)(T.r[3] & 0x01F0));
		tick(0xC806, 18);
		T.r[2] = peek(0x0228);
		fl::mov(T.r[2]);
		tick(0xC80A, 24);
		T.r[2] = fl::add(T.r[2], 0x0020);
		tick(0xC80E, 18);
		fl::cmp(T.r[3], T.r[2]);
		tick(0xC810, 12);                              // CMP R3,R2
		if ((int16_t)T.r[3] < (int16_t)T.r[2])
			tick(0xC816, 12);                          // BLT
		else
		{
			tick(0xC812, 12);
			pokeb(T.r[4], fl::logicb((uint8_t)(peekb(T.r[4]) | 0x80)));
			tick(0xC816, 27);
		}
		T.r[2] = peek(0x0224);
		fl::mov(T.r[2]);
		tick(0xC81A, 24);
		T.r[2] = fl::add(T.r[2], 0x0002);
		tick(0xC81E, 18);
		T.r[3] = T.r[2];
		fl::mov(T.r[3]);
		tick(0xC820, 12);
		T.r[1] = sext8(peekb((uint16_t)(0x0AF8 + T.r[0])));
		fl::movb((uint8_t)T.r[1]);
		tick(0xC824, 27);
		T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x000F));
		tick(0xC828, 18);
		fl::cmp(T.r[1], T.r[3]);
		tick(0xC82A, 12);                              // CMP R1,R3
		if (T.r[1] >= T.r[3])
			tick(0xC830, 12);                          // BCC
		else
		{
			tick(0xC82C, 12);
			pokeb(T.r[4], fl::logicb((uint8_t)(peekb(T.r[4]) | 0x40)));
			tick(0xC830, 27);
		}
	}
	poke(0x01D2, peek(sp_at(4)));
	fl::mov(peek(0x01D2));
	tick(0xC836, 42);
	poke(0x01D4, peek(sp_at(2)));
	fl::mov(peek(0x01D4));
	tick(0xC83C, 42);
	poke(0x1400, peek(0x01D2));
	fl::mov(peek(0x01D2));
	tick(0xC842, 39);
	poke(0x1402, peek(0x01D4));
	fl::mov(peek(0x01D4));
	tick(0xC848, 39);
	pop();
	fl::tst(peek((uint16_t)(T.r[6] - 2)));
	tick(0xC84A, 18);                                  // TST (SP)+
	T.r[6] = fl::add(T.r[6], 0x0012);
	tick(0xC84E, 18);
	c_restore(4);
	return T.r[0];
}

// F:BCB8 (assembly): keep the current map object's on-screen state up to date.  Without an
// object (descriptor bit 15 clear) it may enter (F:BD78).  With one, bits 7 / 6 of its slot
// entry 0x09B8 record the side of the screen in x / y (beyond 0x4000); an object that crossed
// from one side to the other far off screen (beyond 0xC000) leaves (F:C3E6).
void check_map_object_view()
{
	T.r[1] = peek(0x09AA);
	fl::mov(T.r[1]);
	tick(0xBCBC, 24);
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0xBCBE, 12);
	T.r[0] = fl::asl(T.r[0]);
	tick(0xBCC0, 12);
	fl::tst(peek((uint16_t)(0x0AF8 + T.r[0])));
	tick(0xBCC4, 27);
	if (!lt())
	{
		// no object yet: does it come into view (also one screen further when 0x099A < 0)?
		tick(0xBD38, 12);                              // BGE
		jsr(0xBD3C, 0xBD78);
		fl::tst(peek(0x099A));
		tick(0xBD40, 24);
		if (!lt())
			tick(0xBD76, 12);                          // BGE
		else
		{
			tick(0xBD42, 12);
			poke(0x0224, fl::add(peek(0x0224), 0x000C));
			tick(0xBD48, 33);
			poke(0x0226, fl::add(peek(0x0226), 0x000C));
			tick(0xBD4E, 33);
			poke(0x0228, fl::sub(peek(0x0228), 0x0050));
			tick(0xBD54, 33);
			poke(0x022A, fl::sub(peek(0x022A), 0x0050));
			tick(0xBD5A, 33);
			jsr(0xBD5E, 0xBD78);
			poke(0x0224, peek(0x099A));
			fl::mov(peek(0x0224));
			tick(0xBD64, 39);
			poke(0x0226, peek(0x099C));
			fl::mov(peek(0x0226));
			tick(0xBD6A, 39);
			poke(0x0228, peek(0x0996));
			fl::mov(peek(0x0228));
			tick(0xBD70, 39);
			const uint16_t v = peek(0x0998);
			poke(0x022A, v);
			fl::mov(v);
			tick(0xBD76, 39);
		}
		rts();
		return;
	}
	// the object's off-screen bits (0x80 x, 0x40 y) in 0x09B8 + n; off screen it leaves (F:C3E6)
	tick(0xBCC6, 12);
	T.r[1] = fl::add(T.r[1], 0x09B8);
	tick(0xBCCA, 18);
	T.r[0] = sext8(peekb(T.r[1]));
	fl::movb((uint8_t)T.r[0]);
	tick(0xBCCC, 18);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x003F));
	tick(0xBCD0, 18);
	T.r[2] = T.r[0];
	fl::mov(T.r[2]);
	tick(0xBCD2, 12);
	T.r[2] = fl::asl(T.r[2]);
	tick(0xBCD4, 12);
	T.r[0] = peek((uint16_t)(0x0630 + T.r[2]));
	fl::mov(T.r[0]);
	tick(0xBCD8, 27);
	bool leaves = false;
	if (lt())
	{
		tick(0xBCEC, 12);                              // BLT
		fl::cmp(T.r[0], 0xC000);
		tick(0xBCF0, 18);
		if (fC())
		{
			tick(0xBCF8, 12);                          // BCS
			fl::tstb(peekb(T.r[1]));
			tick(0xBCFA, 18);
			leaves = !fZ() && !lt();
			tick(leaves ? 0xBD32 : 0xBCFC, 12);        // BGT
		}
		else
		{
			tick(0xBCF2, 12);
			fl::tstb(peekb(T.r[1]));
			tick(0xBCF4, 18);
			if (!lt())
				tick(0xBCFC, 12);                      // BGE
			else
			{
				tick(0xBCF6, 12);
				tick(0xBD32, 12);                      // BR
				leaves = true;
			}
		}
	}
	else
	{
		tick(0xBCDA, 12);
		fl::cmp(T.r[0], 0x4000);
		tick(0xBCDE, 18);
		if (!lt())
		{
			tick(0xBCE6, 12);                          // BGE
			pokeb(T.r[1], fl_logicb(peekb(T.r[1]) | 0x80));
			tick(0xBCEA, 27);
		}
		else
		{
			tick(0xBCE0, 12);
			pokeb(T.r[1], fl_logicb(peekb(T.r[1]) & ~0x80));
			tick(0xBCE4, 27);
		}
		tick(0xBCFC, 12);                              // BR
	}
	if (!leaves)
	{
		T.r[0] = peek(0x09AA);
		fl::mov(T.r[0]);
		tick(0xBD00, 24);
		T.r[1] = 0x09B8;
		fl::mov(T.r[1]);
		tick(0xBD04, 18);
		T.r[1] = fl::add(T.r[1], T.r[0]);
		tick(0xBD06, 12);
		T.r[0] = peek((uint16_t)(0x0670 + T.r[2]));
		fl::mov(T.r[0]);
		tick(0xBD0A, 27);
		if (!fZ() && !lt())
		{
			tick(0xBD0C, 12);
			fl::cmp(T.r[0], 0x4000);
			tick(0xBD10, 18);
			if (!lt())
			{
				tick(0xBD18, 12);                      // BGE
				pokeb(T.r[1], fl_logicb(peekb(T.r[1]) | 0x40));
				tick(0xBD1C, 27);
			}
			else
			{
				tick(0xBD12, 12);
				pokeb(T.r[1], fl_logicb(peekb(T.r[1]) & ~0x40));
				tick(0xBD16, 27);
			}
			rts();
			return;
		}
		tick(0xBD1E, 12);                              // BLE
		fl::cmp(T.r[0], 0xC000);
		tick(0xBD22, 18);
		if (fC())
		{
			tick(0xBD2C, 12);                          // BCS
			fl_logicb(peekb(T.r[1]) & 0x40);
			tick(0xBD30, 24);
			if (!fZ())
			{
				tick(0xBD76, 12);                      // BNE
				rts();
				return;
			}
			tick(0xBD32, 12);
		}
		else
		{
			tick(0xBD24, 12);
			fl_logicb(peekb(T.r[1]) & 0x40);
			tick(0xBD28, 24);
			if (fZ())
			{
				tick(0xBD76, 12);                      // BEQ
				rts();
				return;
			}
			tick(0xBD2A, 12);
			tick(0xBD32, 12);                          // BR
		}
	}
	jsr(0xBD36, 0xC3E6);
	rts();
}

// F:C850 (assembly; R1-R5 kept): the current map object takes the definition its record
// points to (F:BE7C) when that is free: F:BFBC, R0 = 1; otherwise R0 = -1
void try_map_object_move()
{
	jsr(0xC854, 0xBE7C);                               // map_object_state
	fl::tst(T.r[0]);
	tick(0xC856, 12);
	if (T.r[0])
	{
		tick(0xC862, 12);                              // BNE: the definition is taken
		T.r[0] = 0xFFFF;
		fl::mov(T.r[0]);
		tick(0xC866, 18);
	}
	else
	{
		tick(0xC858, 12);
		jsr(0xC85C, 0xBFBC);                           // take_map_definition
		T.r[0] = 0x0001;
		fl::mov(T.r[0]);
		tick(0xC860, 18);
	}
	rts();
}

// F:C868  try_next_map_definition(int group): the same with the next record entry (0x09B2 + 4),
// only if that definition is free and in the given group (0xFD91 by properties 0x4000 bits
// 0-3; definitions without a group always qualify).  Returns 0 when moved, else -1.
uint16_t try_next_map_definition(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xC86A, 24);
	poke(0x09B2, fl::add(peek(0x09B2), 0x0004));      // the record's next entry
	tick(0xC870, 33);
	jsr(0xC874, 0xBE7C);                               // map_object_state
	fl::tst(T.r[0]);
	tick(0xC876, 12);
	bool moved = false;
	if (T.r[0])
		tick(0xC89E, 12);                              // BNE: taken
	else
	{
		tick(0xC878, 12);
		T.r[0] = peek(0x09B4);
		fl::mov(T.r[0]);
		tick(0xC87C, 24);
		const uint8_t group = (uint8_t)(peekb((uint16_t)(0x4000 + T.r[0])) & 0x0F);
		fl_logicb(group);
		tick(0xC882, 33);
		if (!group)
		{
			tick(0xC898, 12);                          // BEQ: no group, always fine
			moved = true;
		}
		else
		{
			tick(0xC884, 12);
			T.r[0] = sext8(peekb((uint16_t)(0x4000 + T.r[0])));
			fl::movb((uint8_t)T.r[0]);
			tick(0xC888, 27);
			T.r[0] = fl_logic(T.r[0] & 0x000F);
			tick(0xC88C, 18);
			T.r[1] = 0;
			fl::clr();
			tick(0xC88E, 12);
			T.r[1] = fl_logicb(peekb((uint16_t)(0xFD91 + T.r[0])));
			tick(0xC892, 27);
			fl::cmp(T.r[1], rw(sp_at(4)));             // the wanted group
			tick(0xC896, 27);
			moved = fZ();
			tick(moved ? 0xC898 : 0xC89E, 12);         // BNE
		}
	}
	if (moved)
	{
		jsr(0xC89C, 0xBFBC);                           // take_map_definition
		tick(0xC8A4, 12);                              // BR
		T.r[0] = 0;
		fl::clr();
		tick(0xC8A6, 12);
	}
	else
	{
		T.r[0] = 0xFFFF;
		fl::mov(T.r[0]);
		tick(0xC8A2, 18);
		tick(0xC8A6, 12);                              // BR
	}
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0xC8A8, 18);
	rts();
	return T.r[0];
}

// F:CA4C (assembly): the map object of object slot 0x022C (map object 0x09AA, located) moves
// on: objects flagged 0x01 (0x04B0) follow their record (the second half with flag 0x02);
// the others toggle their definition's bit in the bitmap 0x0DAA and try their own record
// (F:C850) or, for definitions with properties bit 6, the next entry (F:C868) in their type's
// group.  When neither works the object stops (speed 0, animation from 0xFD8A by 0x65CC kind,
// 0x0770 bit 7); flag 0x02 of 0x04B0 records whether it moved (R0 = 0) or not.
void update_map_object_slot()
{
	T.r[1] = peek(0x022C);                             // the object slot
	fl::mov(T.r[1]);
	tick(0xCA50, 24);
	T.r[5] = 0x04B0;
	fl::mov(T.r[5]);
	tick(0xCA54, 18);
	T.r[5] = fl::add(T.r[5], T.r[1]);
	tick(0xCA56, 12);
	const uint8_t follows = fl_logicb((uint8_t)(peekb(T.r[5]) & 0x01));
	tick(0xCA5A, 24);
	if (follows)
	{
		// it follows its record (the second half with flag 0x02)
		tick(0xCA5C, 12);
		const uint8_t second = fl_logicb((uint8_t)(peekb(T.r[5]) & 0x02));
		tick(0xCA60, 24);
		if (!second)
			tick(0xCA68, 12);                          // BEQ
		else
		{
			tick(0xCA62, 12);
			poke(0x09B2, fl::add(peek(0x09B2), 0x0004));
			tick(0xCA68, 33);
		}
		jsr(0xCA6C, 0xBE7C);                           // map_object_state
		jsr(0xCA70, 0xBFBC);                           // take_map_definition
		rts();
		return;
	}
	tick(0xCA72, 12);                                  // BEQ
	T.r[0] = 0xFFFF;
	fl::mov(T.r[0]);
	tick(0xCA76, 18);
	T.r[2] = peek(0x09B6);                             // the definition
	fl::mov(T.r[2]);
	tick(0xCA7A, 24);
	T.r[3] = T.r[2];
	fl::mov(T.r[3]);
	tick(0xCA7C, 12);
	T.r[3] = fl_asr(T.r[3]);
	tick(0xCA7E, 12);
	T.r[3] = fl_asr(T.r[3]);
	tick(0xCA80, 12);
	T.r[3] = fl_asr(T.r[3]);
	tick(0xCA82, 12);
	T.r[4] = 0x0DAA;
	fl::mov(T.r[4]);
	tick(0xCA86, 18);
	T.r[4] = fl::add(T.r[4], T.r[3]);                  // its byte in the bitmap 0x0DAA
	tick(0xCA88, 12);
	T.r[3] = T.r[2];
	fl::mov(T.r[3]);
	tick(0xCA8A, 12);
	T.r[3] = fl_logic(T.r[3] & 0x0007);
	tick(0xCA8E, 18);
	const uint16_t bit = (uint16_t)(0xFC74 + T.r[3]);
	const bool set = fl_logicb((uint8_t)(peekb(T.r[4]) & peekb(bit))) != 0;
	tick(0xCA92, 33);
	bool test = true;                                  // go on at CAD2 (else at CB12)
	if (!set)
	{
		tick(0xCA94, 12);
		jsr(0xCA98, 0xC850);                           // try_map_object_move
		const bool own = fl_logicb((uint8_t)(peekb((uint16_t)(0x4560 + T.r[2])) & 0x40)) != 0;
		tick(0xCA9E, 33);
		if (!own)
			tick(0xCAD2, 12);                          // BEQ
		else
		{
			tick(0xCAA0, 12);
			pokeb(T.r[4], fl_logicb((uint8_t)(peekb(T.r[4]) | peekb(bit))));
			tick(0xCAA4, 36);
			fl::tst(T.r[0]);
			tick(0xCAA6, 12);
			if ((int16_t)T.r[0] >= 0)
			{
				tick(0xCB12, 12);                      // BGE: moved
				test = false;
			}
			else
			{
				tick(0xCAA8, 12);
				push(0);
				fl::clr();
				tick(0xCAAA, 24);
				const uint8_t type = peekb((uint16_t)(0x0370 + T.r[1]));
				pokeb(T.r[6], type);                   // its type's group
				fl::movb(type);
				tick(0xCAAE, 36);
				jsr(0xCAB2, 0xC868);                   // try_next_map_definition
				tick(0xCAD0, 12);                      // BR
				fl::tst(rw(T.r[6]));
				T.r[6] = (uint16_t)(T.r[6] + 2);
				tick(0xCAD2, 18);
			}
		}
	}
	else
	{
		tick(0xCAB4, 12);
		pokeb(T.r[4], fl_logicb((uint8_t)(peekb(T.r[4]) & ~peekb(bit))));
		tick(0xCAB8, 36);
		push(0);
		fl::clr();
		tick(0xCABA, 24);
		const uint8_t type = peekb((uint16_t)(0x0370 + T.r[1]));
		pokeb(T.r[6], type);
		fl::movb(type);
		tick(0xCABE, 36);
		jsr(0xCAC2, 0xC868);                           // try_next_map_definition
		fl::tst(T.r[0]);
		tick(0xCAC4, 12);
		if ((int16_t)T.r[0] >= 0)
			tick(0xCAD0, 12);                          // BGE
		else
		{
			tick(0xCAC6, 12);
			poke(0x09B2, fl::sub(peek(0x09B2), 0x0004));
			tick(0xCACC, 33);
			jsr(0xCAD0, 0xC850);                       // try_map_object_move
		}
		fl::tst(rw(T.r[6]));
		T.r[6] = (uint16_t)(T.r[6] + 2);
		tick(0xCAD2, 18);
	}
	if (test)
	{
		fl::tst(T.r[0]);
		tick(0xCAD4, 12);
		if ((int16_t)T.r[0] < 0)
		{
			// nowhere to go: the object stops (speed 0, the standing animation of its kind)
			tick(0xCAD6, 12);
			poke(0x01D2, 0);
			fl::clr();
			tick(0xCADA, 27);
			poke(0x1400, 0);
			fl::clr();
			tick(0xCADE, 27);
			T.r[0] = 0x0400;
			fl::mov(T.r[0]);
			tick(0xCAE2, 18);
			poke(0x01D4, T.r[0]);
			fl::mov(T.r[0]);
			tick(0xCAE6, 27);
			poke(0x1402, T.r[0]);
			fl::mov(T.r[0]);
			tick(0xCAEA, 27);
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0xCAEC, 12);
			T.r[0] = fl_asl(T.r[0]);
			tick(0xCAEE, 12);
			T.r[2] = 0;
			fl::clr();
			tick(0xCAF0, 12);
			T.r[2] = fl_logicb(peekb((uint16_t)(0x0370 + T.r[1])));
			tick(0xCAF4, 27);
			T.r[2] = sext8(peekb((uint16_t)(0x65CC + T.r[2])));
			fl::movb((uint8_t)T.r[2]);
			tick(0xCAF8, 27);
			{
				const uint8_t v = peekb((uint16_t)(0xFD8A + T.r[2]));
				pokeb((uint16_t)(0x0430 + T.r[0]), v);
				fl::movb(v);
			}
			tick(0xCAFE, 45);
			pokeb((uint16_t)(0x0431 + T.r[0]), 0);
			fl::clr();
			tick(0xCB02, 30);
			poke((uint16_t)(0x04F0 + T.r[0]), 0);
			fl::clr();
			tick(0xCB06, 30);
			poke((uint16_t)(0x0570 + T.r[0]), 0);
			fl::clr();
			tick(0xCB0A, 30);
			const uint16_t anim = (uint16_t)(0x0770 + T.r[1]);
			pokeb(anim, fl_logicb((uint8_t)(peekb(anim) | 0x80)));
			tick(0xCB10, 36);
			rts();
			return;
		}
		tick(0xCB12, 12);                              // BGE
	}
	// moved (R0 = 0) or held (R0 > 0): flag 0x02 records it
	if (!T.r[0])
	{
		tick(0xCB1E, 12);                              // BEQ
		pokeb(T.r[5], fl_logicb((uint8_t)(peekb(T.r[5]) | 0x02)));
		tick(0xCB22, 27);
	}
	else
	{
		tick(0xCB14, 12);
		T.r[0] = 0xFFFD;
		fl::mov(T.r[0]);
		tick(0xCB18, 18);
		T.r[0] = (uint16_t)((T.r[0] & 0xFF00) | fl_comb((uint8_t)T.r[0]));
		tick(0xCB1A, 12);
		pokeb(T.r[5], fl_logicb((uint8_t)(peekb(T.r[5]) & ~(uint8_t)T.r[0])));
		tick(0xCB1C, 21);
	}
	rts();
}

// F:BC52 (assembly): check four map objects a frame for entering or leaving the view
// (F:BCB8), going round from 0x09AC (skipping 0x09A8 and those above 0x09AE without a slot
// entry); the view window 0x0224-0x022A is taken from 0x0996-0x099C.  Banks 0x1000 / 0x1400.
void check_map_objects_in_view()
{
	bank_a(0xBC52, 0x1000);
	bank_b(0xBC5E, 0x1400);
	poke(0x0224, peek(0x099A));
	tick(0xBC70, 39);
	poke(0x0226, peek(0x099C));
	fl::mov(peek(0x0226));
	tick(0xBC76, 39);
	poke(0x0228, peek(0x0996));
	fl::mov(peek(0x0228));
	tick(0xBC7C, 39);
	poke(0x022A, peek(0x0998));
	fl::mov(peek(0x022A));
	tick(0xBC82, 39);
	push(0x0004);
	fl::mov(0x0004);
	tick(0xBC86, 30);
	for (;;)
	{
		poke(0x09AA, peek(0x09AC));
		fl::mov(peek(0x09AA));
		tick(0xBC8C, 39);
		poke(0x09AC, fl::inc(peek(0x09AC)));
		tick(0xBC90, 27);
		T.r[0] = peek(0x09AA);
		fl::mov(T.r[0]);
		tick(0xBC94, 24);
		fl::cmp(T.r[0], peek(0x09A8));
		tick(0xBC98, 24);
		if (fZ())
			tick(0xBCAA, 12);                          // BEQ
		else
		{
			tick(0xBC9A, 12);
			fl::cmp(T.r[0], peek(0x09AE));
			tick(0xBC9E, 24);
			if (!fZ() && !lt())
				tick(0xBCAA, 12);                      // BGT
			else
			{
				tick(0xBCA0, 12);
				fl::tstb(peekb((uint16_t)(0x09B8 + T.r[0])));
				tick(0xBCA4, 27);
				if (fZ())
					tick(0xBCAA, 12);                  // BEQ
				else
				{
					tick(0xBCA6, 12);
					jsr(0xBCAA, 0xBCB8);
				}
			}
		}
		poke(T.r[6], fl::dec(peek(T.r[6])));
		tick(0xBCAC, 21);
		if (fZ()) break;
		tick(0xBC86, 12);                              // BNE
	}
	tick(0xBCAE, 12);
	poke(0x09AC, fl_logic(peek(0x09AC) & 0x003F));
	tick(0xBCB4, 33);
	fl::tst(pop());
	tick(0xBCB6, 18);
	rts();
}

// F:C8AA (assembly): advance the map object clock 0x0E9A; fixed map objects (0x09AE down to 0)
// whose timer 0x0AB8 is due toggle their definition's bit in 0x0DAA and try to move on
// (F:C850 / F:C868 as F:CA4C does); when that fails the timer is set 8 ticks ahead.  A timer
// left at 0 becomes 1.  The result of the last attempt stays in the routine's stack word,
// which the original tests on return.
void tick_map_object_timers()
{
	T.r[6] = (uint16_t)(T.r[6] - 2);                   // TST -(SP): the routine's result word
	const uint16_t result = T.r[6];
	fl::tst(rw(result));
	tick(0xC8AC, 21);
	pokeb(0x0E9A, fl::incb(peekb(0x0E9A)));            // the map object clock
	tick(0xC8B0, 27);
	T.r[0] = 0;
	fl::clr();
	tick(0xC8B2, 12);
	T.r[0] = fl_logicb(peekb(0x0E9A));
	tick(0xC8B6, 24);
	fl::tst(T.r[0]);
	tick(0xC8B8, 12);
	if (!T.r[0])
		tick(0xC98A, 12);                              // BEQ
	else
	{
		tick(0xC8BA, 12);
		const uint16_t top = peek(0x09AE);
		fl::mov(top);
		poke(0x09AA, top);
		tick(0xC8C0, 39);
		T.r[1] = T.r[0];
		fl::mov(T.r[1]);
		tick(0xC8C2, 12);
		for (;;)                                       // fixed map objects 0x09AE down to 0
		{
			T.r[0] = peek(0x09AA);
			fl::mov(T.r[0]);
			tick(0xC8C6, 24);
			if ((int16_t)T.r[0] < 0)
			{
				tick(0xC98A, 12);                      // BLT
				break;
			}
			tick(0xC8C8, 12);
			T.r[4] = 0;
			fl::clr();
			tick(0xC8CA, 12);
			T.r[4] = fl_logicb(peekb((uint16_t)(0x0AB8 + T.r[0])));
			tick(0xC8CE, 27);
			fl::cmp(T.r[4], T.r[1]);
			tick(0xC8D0, 12);
			if (T.r[4] != T.r[1])
				tick(0xC984, 12);                      // BNE: its timer is not due
			else
			{
				tick(0xC8D2, 12);
				bank_a(0xC8D2, 0x1000);
				bank_b(0xC8DE, 0x1400);
				jsr(0xC8EE, 0xBE10);                   // locate_map_object
				ww(result, 0xFFFF);
				fl::mov(0xFFFF);
				tick(0xC8F2, 27);
				T.r[3] = peek(0x09B6);                 // the definition
				fl::mov(T.r[3]);
				tick(0xC8F6, 24);
				T.r[2] = T.r[3];
				fl::mov(T.r[2]);
				tick(0xC8F8, 12);
				T.r[2] = fl_asr(T.r[2]);
				tick(0xC8FA, 12);
				T.r[2] = fl_asr(T.r[2]);
				tick(0xC8FC, 12);
				T.r[2] = fl_asr(T.r[2]);
				tick(0xC8FE, 12);
				T.r[5] = 0x0DAA;
				fl::mov(T.r[5]);
				tick(0xC902, 18);
				T.r[5] = fl::add(T.r[5], T.r[2]);      // its byte in the bitmap 0x0DAA
				tick(0xC904, 12);
				T.r[4] = T.r[3];
				fl::mov(T.r[4]);
				tick(0xC906, 12);
				T.r[4] = fl_logic(T.r[4] & 0x0007);
				tick(0xC90A, 18);
				const uint16_t bit = (uint16_t)(0xFC74 + T.r[4]);
				const bool set = fl_logicb((uint8_t)(peekb(T.r[5]) & peekb(bit))) != 0;
				tick(0xC90E, 33);
				bool test = true;                      // go on at C95E (else at C974)
				if (!set)
				{
					tick(0xC910, 12);
					jsr(0xC914, 0xC850);               // try_map_object_move
					ww(result, T.r[0]);
					fl::mov(T.r[0]);
					tick(0xC916, 21);
					const bool own = fl_logicb((uint8_t)(peekb((uint16_t)(0x4560 + T.r[3])) & 0x40)) != 0;
					tick(0xC91C, 33);
					if (!own)
						tick(0xC95E, 12);              // BEQ
					else
					{
						tick(0xC91E, 12);
						pokeb(T.r[5], fl_logicb((uint8_t)(peekb(T.r[5]) | peekb(bit))));
						tick(0xC922, 36);
						fl::tst(T.r[0]);
						tick(0xC924, 12);
						if ((int16_t)T.r[0] >= 0)
						{
							tick(0xC974, 12);          // BGE: moved
							test = false;
						}
						else
						{
							tick(0xC926, 12);
							T.r[0] = peek(0x09AA);
							fl::mov(T.r[0]);
							tick(0xC92A, 24);
							T.r[4] = sext8(peekb((uint16_t)(0x09B8 + T.r[0])));
							fl::movb((uint8_t)T.r[4]);
							tick(0xC92E, 27);
							push(T.r[4]);              // its type's group
							fl::mov(T.r[4]);
							tick(0xC930, 24);
							jsr(0xC934, 0xC868);       // try_next_map_definition
							tick(0xC958, 12);          // BR
							ww(sp_at(2), T.r[0]);
							fl::mov(T.r[0]);
							tick(0xC95C, 30);
							fl::tst(rw(T.r[6]));
							T.r[6] = (uint16_t)(T.r[6] + 2);
							tick(0xC95E, 18);
						}
					}
				}
				else
				{
					tick(0xC936, 12);
					pokeb(T.r[5], fl_logicb((uint8_t)(peekb(T.r[5]) & ~peekb(bit))));
					tick(0xC93A, 36);
					T.r[0] = peek(0x09AA);
					fl::mov(T.r[0]);
					tick(0xC93E, 24);
					T.r[4] = sext8(peekb((uint16_t)(0x09B8 + T.r[0])));
					fl::movb((uint8_t)T.r[4]);
					tick(0xC942, 27);
					push(T.r[4]);
					fl::mov(T.r[4]);
					tick(0xC944, 24);
					jsr(0xC948, 0xC868);               // try_next_map_definition
					ww(sp_at(2), T.r[0]);
					fl::mov(T.r[0]);
					tick(0xC94C, 30);
					if ((int16_t)T.r[0] >= 0)
						tick(0xC95C, 12);              // BGE
					else
					{
						tick(0xC94E, 12);
						poke(0x09B2, fl::sub(peek(0x09B2), 0x0004));   // back to its own entry
						tick(0xC954, 33);
						jsr(0xC958, 0xC850);           // try_map_object_move
						ww(sp_at(2), T.r[0]);
						fl::mov(T.r[0]);
						tick(0xC95C, 30);
					}
					fl::tst(rw(T.r[6]));
					T.r[6] = (uint16_t)(T.r[6] + 2);
					tick(0xC95E, 18);
				}
				if (test)
				{
					const uint16_t r = rw(result);
					fl::tst(r);
					tick(0xC960, 18);
					if ((int16_t)r >= 0)
						tick(0xC974, 12);              // BGE
					else
					{
						// no luck: try again 8 ticks later
						tick(0xC962, 12);
						T.r[0] = peek(0x09AA);
						fl::mov(T.r[0]);
						tick(0xC966, 24);
						T.r[4] = 0;
						fl::clr();
						tick(0xC968, 12);
						T.r[4] = fl_logicb(peekb(0x0E9A));
						tick(0xC96C, 24);
						T.r[4] = fl::add(T.r[4], 0x0008);
						tick(0xC970, 18);
						pokeb((uint16_t)(0x0AB8 + T.r[0]), (uint8_t)T.r[4]);
						fl::movb((uint8_t)T.r[4]);
						tick(0xC974, 30);
					}
				}
				T.r[0] = peek(0x09AA);
				fl::mov(T.r[0]);
				tick(0xC978, 24);
				const uint8_t timer = peekb((uint16_t)(0x0AB8 + T.r[0]));
				fl::tstb(timer);
				tick(0xC97C, 27);
				if (timer)
					tick(0xC984, 12);                  // BNE
				else
				{
					tick(0xC97E, 12);
					pokeb((uint16_t)(0x0AB8 + T.r[0]), 0x01);   // a timer left at 0 becomes 1
					fl::movb(0x01);
					tick(0xC984, 36);
				}
			}
			poke(0x09AA, fl::dec(peek(0x09AA)));
			tick(0xC988, 27);
			tick(0xC8C2, 12);                          // BR
		}
	}
	fl::tst(rw(T.r[6]));                               // TST (SP)+: the result for the caller
	T.r[6] = (uint16_t)(T.r[6] + 2);
	tick(0xC98C, 18);
	rts();
}

// F:CB24 (assembly): once a frame, for the object slots 0x0994 down to 5 (0x022C): when the
// object's counter 0x0430 runs out (or while it counts and flags 0x0C of 0x04B0 are clear) it
// is flagged (0x08) and acts by the mode of its type (table 0x623F, bank B 0x0400) for its map
// object (0x0390, located by F:BE10): 1 move on (F:CA4C), 2 F:DE28, 4 A7:5018, 5 view check
// (F:BCB8, counter 16); modes 0, 3 and 6-11 do not occur in the tables (code kept as in the
// original; 0x623F only holds 0-9).  Before that the queued object (0x0E68 frames) is
// started (A7:4F42).  The last mode stays in the routine's stack word, tested on return.
void update_objects()
{
	T.r[6] = (uint16_t)(T.r[6] - 2);                   // TST -(SP): the routine's mode word
	const uint16_t mode_at = T.r[6];
	fl::tst(rw(mode_at));
	tick(0xCB26, 21);
	const uint8_t queued = peekb(0x0E68);              // frames until the queued object starts
	fl::tstb(queued);
	tick(0xCB2A, 24);
	if (!queued)
		tick(0xCB4E, 12);                              // BEQ
	else
	{
		tick(0xCB2C, 12);
		const uint8_t left = fl::decb(peekb(0x0E68));
		pokeb(0x0E68, left);
		tick(0xCB30, 27);
		if (left)
			tick(0xCB4E, 12);                          // BNE
		else
		{
			tick(0xCB32, 12);
			bank_a(0xCB32, 0x3800);
			bank_b(0xCB3E, 0x3C00);
			jsr(0xCB4E, 0x4F42);                       // spawn_queued_object
		}
	}
	T.r[0] = sext8(peekb(0x0994));                     // the highest slot in use
	fl::movb((uint8_t)T.r[0]);
	tick(0xCB52, 24);
	poke(0x022C, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xCB56, 27);
	auto slot2 = []() {                                // MOV @#0x022C,R0; ASL R0 at pc, pc + 4
		T.r[0] = peek(0x022C);
		fl::mov(T.r[0]);
	};
	for (;;)
	{
		T.r[0] = peek(0x022C);
		fl::mov(T.r[0]);
		tick(0xCB5A, 24);
		fl::cmp(T.r[0], 0x0005);
		tick(0xCB5E, 18);
		if ((int16_t)T.r[0] < 5)
		{
			tick(0xCB60, 12);                          // BGE not taken
			tick(0xCDCC, 18);                          // JMP
			break;
		}
		tick(0xCB64, 12);
		T.r[1] = T.r[0];
		fl::mov(T.r[1]);
		tick(0xCB66, 12);
		T.r[1] = fl_asl(T.r[1]);
		tick(0xCB68, 12);
		T.r[1] = fl::add(T.r[1], 0x0430);              // the slot's counter
		tick(0xCB6C, 18);
		const uint16_t count = peek(T.r[1]);
		fl::tst(count);
		tick(0xCB6E, 18);
		bool act = false;
		if (!count)
			tick(0xCC1A, 12);                          // BEQ: not counting
		else
		{
			tick(0xCB70, 12);
			const uint16_t now = fl::dec(peek(T.r[1]));
			poke(T.r[1], now);
			tick(0xCB72, 21);
			T.psw = (uint8_t)(T.psw & ~(CF | VF));     // CCC CV
			tick(0xCB74, 18);
			if ((int16_t)now <= 0)
			{
				tick(0xCB7E, 12);                      // BLE: ran out
				act = true;
			}
			else
			{
				tick(0xCB76, 12);
				const uint8_t busy = fl_logicb((uint8_t)(peekb((uint16_t)(0x04B0 + T.r[0])) & 0x0C));
				tick(0xCB7C, 33);
				act = !busy;
				tick(busy ? 0xCC1A : 0xCB7E, 12);      // BNE
			}
		}
		if (!act)
		{
			tick(0xCCEA, 12);                          // CC1A: BR
			tick(0xCDC4, 12);                          // CCEA: BR
		}
		else
		{
			const uint16_t flags = (uint16_t)(0x04B0 + T.r[0]);
			pokeb(flags, fl_logicb((uint8_t)(peekb(flags) | 0x08)));
			tick(0xCB84, 36);
			poke(0x01D2, 0);
			fl::clr();
			tick(0xCB88, 27);
			poke(0x1400, 0);
			fl::clr();
			tick(0xCB8C, 27);
			T.r[1] = 0x0400;
			fl::mov(T.r[1]);
			tick(0xCB90, 18);
			poke(0x01D4, T.r[1]);
			fl::mov(T.r[1]);
			tick(0xCB94, 27);
			poke(0x1402, T.r[1]);
			fl::mov(T.r[1]);
			tick(0xCB98, 27);
			T.r[1] = 0;
			fl::clr();
			tick(0xCB9A, 12);
			T.r[1] = fl_logicb(peekb((uint16_t)(0x0370 + T.r[0])));   // the type
			tick(0xCB9E, 27);
			{
				const uint8_t m = peekb((uint16_t)(0x623F + T.r[1]));   // its mode
				pokeb(mode_at, m);
				fl::movb(m);
			}
			tick(0xCBA2, 36);
			pokeb((uint16_t)(mode_at + 1), 0);
			fl::clr();
			tick(0xCBA6, 30);
			T.r[1] = 0x1000;
			fl::mov(T.r[1]);
			tick(0xCBAA, 18);
			poke(0x01D2, T.r[1]);
			fl::mov(T.r[1]);
			tick(0xCBAE, 27);
			poke(0x1400, T.r[1]);
			fl::mov(T.r[1]);
			tick(0xCBB2, 27);
			T.r[1] = 0x1400;
			fl::mov(T.r[1]);
			tick(0xCBB6, 18);
			poke(0x01D4, T.r[1]);
			fl::mov(T.r[1]);
			tick(0xCBBA, 27);
			poke(0x1402, T.r[1]);
			fl::mov(T.r[1]);
			tick(0xCBBE, 27);
			{
				const uint8_t n = peekb((uint16_t)(0x0390 + T.r[0]));   // its map object
				pokeb(0x09AA, n);
				fl::movb(n);
			}
			tick(0xCBC4, 42);
			pokeb(0x09AB, 0);
			fl::clr();
			tick(0xCBC8, 27);
			jsr(0xCBCC, 0xBE10);                       // locate_map_object
			T.r[0] = rw(mode_at);
			fl::mov(T.r[0]);
			tick(0xCBCE, 18);
			T.r[0] = fl_asl(T.r[0]);
			tick(0xCBD0, 12);
			// ADD 0xFDCA(R0),PC: the mode's code
			const uint16_t at = fl::add(0xCBD4, peek((uint16_t)(0xFDCA + T.r[0])));
			tick(at, 27);
			auto cce6 = []() {                         // CCE6: move on, then CCEA: BR CDC4
				jsr(0xCCEA, 0xCA4C);                   // update_map_object_slot
				tick(0xCDC4, 12);
			};
			auto cc84 = []() {                         // CC84: BR CD80; CD80: BR CDC4
				tick(0xCD80, 12);
				tick(0xCDC4, 12);
			};
			switch (at)
			{
			case 0xCBD4:                               // stopped: counter cleared
				slot2();
				tick(0xCBD8, 24);
				T.r[0] = fl_asl(T.r[0]);
				tick(0xCBDA, 12);
				poke((uint16_t)(0x0430 + T.r[0]), 0);
				fl::clr();
				tick(0xCBDE, 30);
				tick(0xCC84, 12);                      // BR
				cc84();
				break;
			case 0xCBE0:                               // scripted (the town's skaters)
				jsr(0xCBE4, 0xDE28);                   // run_object_scripts
				tick(0xCC84, 12);                      // BR
				cc84();
				break;
			case 0xCBE6:                               // animated, then moving on
				jsr(0xCBEA, 0xCA0C);                   // animate_map_object
				tick(0xCCE6, 12);                      // BR
				cce6();
				break;
			case 0xCBEC:                               // the chaser
				bank_a(0xCBEC, 0x3800);
				bank_b(0xCBF8, 0x3C00);
				jsr(0xCC08, 0x5018);                   // chaser_step
				tick(0xCCEA, 12);                      // BR
				tick(0xCDC4, 12);                      // CCEA: BR
				break;
			case 0xCC0A:                               // checked against the view every 16 frames
				slot2();
				tick(0xCC0E, 24);
				T.r[0] = fl_asl(T.r[0]);
				tick(0xCC10, 12);
				poke((uint16_t)(0x0430 + T.r[0]), 0x0010);
				fl::mov(0x0010);
				tick(0xCC16, 36);
				jsr(0xCC1A, 0xBCB8);                   // check_map_object_view
				tick(0xCCEA, 12);                      // CC1A: BR
				tick(0xCDC4, 12);                      // CCEA: BR
				break;
			case 0xCC1C:                               // removed (a counted type gives its count back)
			{
				poke(0x01D2, 0);
				fl::clr();
				tick(0xCC20, 27);
				poke(0x1400, 0);
				fl::clr();
				tick(0xCC24, 27);
				T.r[0] = 0x0400;
				fl::mov(T.r[0]);
				tick(0xCC28, 18);
				poke(0x01D4, T.r[0]);
				fl::mov(T.r[0]);
				tick(0xCC2C, 27);
				poke(0x1402, T.r[0]);
				fl::mov(T.r[0]);
				tick(0xCC30, 27);
				T.r[0] = peek(0x022C);
				fl::mov(T.r[0]);
				tick(0xCC34, 24);
				T.r[0] = sext8(peekb((uint16_t)(0x0370 + T.r[0])));
				fl::movb((uint8_t)T.r[0]);
				tick(0xCC38, 27);
				T.r[0] = fl_logic(T.r[0] & 0x00FF);
				tick(0xCC3C, 18);
				const uint8_t counted = fl_logicb((uint8_t)(peekb((uint16_t)(0x649D + T.r[0])) & 0x10));
				tick(0xCC42, 33);
				if (!counted)
					tick(0xCC4E, 12);                  // BEQ
				else
				{
					tick(0xCC44, 12);
					T.r[0] = sext8(peekb(0x0E96));
					fl::movb((uint8_t)T.r[0]);
					tick(0xCC48, 24);
					T.r[0] = fl::inc(T.r[0]);
					tick(0xCC4A, 12);
					pokeb(0x0E96, (uint8_t)T.r[0]);
					fl::movb((uint8_t)T.r[0]);
					tick(0xCC4E, 27);
				}
				{
					const uint16_t n = peek(0x09AA);
					fl::mov(n);
					push(n);
				}
				tick(0xCC52, 36);
				jsr(0xCC56, 0xCF5E);                   // remove_map_object
				tick(0xCDB4, 18);                      // JMP
				fl::tst(rw(T.r[6]));
				T.r[6] = (uint16_t)(T.r[6] + 2);
				tick(0xCDB6, 18);
				tick(0xCDC4, 12);                      // BR
				break;
			}
			case 0xCC5A:                               // waits for its group's event (0x0EA4)
			{
				T.r[0] = peek(0x09B6);
				fl::mov(T.r[0]);
				tick(0xCC5E, 24);
				T.r[0] = sext8(peekb((uint16_t)(0x4000 + T.r[0])));
				fl::movb((uint8_t)T.r[0]);
				tick(0xCC62, 27);
				T.r[0] = fl_logic(T.r[0] & 0x000F);
				tick(0xCC66, 18);
				T.r[1] = peek(0x022C);
				fl::mov(T.r[1]);
				tick(0xCC6A, 24);
				T.r[1] = fl_asl(T.r[1]);
				tick(0xCC6C, 12);
				fl::tst(T.r[0]);
				tick(0xCC6E, 12);
				bool go = false;
				if (!T.r[0])
					tick(0xCC78, 12);                  // BEQ
				else
				{
					tick(0xCC70, 12);
					go = fl_logicb((uint8_t)(peekb(0x0EA4) & peekb((uint16_t)(0xFC73 + T.r[0])))) != 0;
					tick(0xCC76, 39);
					tick(go ? 0xCC86 : 0xCC78, 12);    // BNE
				}
				if (go)
				{
					poke((uint16_t)(0x03F0 + T.r[1]), 0);
					fl::clr();
					tick(0xCC8A, 30);
					tick(0xCCE6, 12);                  // BR
					cce6();
				}
				else
				{
					poke((uint16_t)(0x04F0 + T.r[1]), 0);
					fl::clr();
					tick(0xCC7C, 30);
					poke((uint16_t)(0x0570 + T.r[1]), 0);
					fl::clr();
					tick(0xCC80, 30);
					poke((uint16_t)(0x0430 + T.r[1]), 0);
					fl::clr();
					tick(0xCC84, 30);
					cc84();
				}
				break;
			}
			case 0xCC8C:                               // by the definition's group (table 0xFDDE)
			{
				T.r[0] = peek(0x09B6);
				fl::mov(T.r[0]);
				tick(0xCC90, 24);
				T.r[0] = sext8(peekb((uint16_t)(0x4000 + T.r[0])));
				fl::movb((uint8_t)T.r[0]);
				tick(0xCC94, 27);
				T.r[0] = fl_logic(T.r[0] & 0x000F);
				tick(0xCC98, 18);
				T.r[0] = fl_asl(T.r[0]);
				tick(0xCC9A, 12);
				const uint16_t to = fl::add(0xCC9E, peek((uint16_t)(0xFDDE + T.r[0])));
				tick(to, 27);                          // ADD 0xFDDE(R0),PC
				if (to == 0xCC9E)
				{
					// a new animation for the type (table 0x5408 by type - 0x2B), then moving on
					poke(0x01D2, 0);
					fl::clr();
					tick(0xCCA2, 27);
					poke(0x1400, 0);
					fl::clr();
					tick(0xCCA6, 27);
					T.r[0] = 0x0400;
					fl::mov(T.r[0]);
					tick(0xCCAA, 18);
					poke(0x01D4, T.r[0]);
					fl::mov(T.r[0]);
					tick(0xCCAE, 27);
					poke(0x1402, T.r[0]);
					fl::mov(T.r[0]);
					tick(0xCCB2, 27);
					T.r[0] = peek(0x022C);
					fl::mov(T.r[0]);
					tick(0xCCB6, 24);
					T.r[1] = T.r[0];
					fl::mov(T.r[1]);
					tick(0xCCB8, 12);
					T.r[1] = fl_asl(T.r[1]);
					tick(0xCCBA, 12);
					T.r[0] = sext8(peekb((uint16_t)(0x0370 + T.r[0])));
					fl::movb((uint8_t)T.r[0]);
					tick(0xCCBE, 27);
					T.r[0] = fl_logic(T.r[0] & 0x00FF);
					tick(0xCCC2, 18);
					T.r[0] = fl::sub(T.r[0], 0x002B);
					tick(0xCCC6, 18);
					T.r[0] = fl_asl(T.r[0]);
					tick(0xCCC8, 12);
					{
						const uint16_t v = peek((uint16_t)(0x5408 + T.r[0]));
						poke((uint16_t)(0x03B0 + T.r[1]), v);
						fl::mov(v);
					}
					tick(0xCCCE, 45);
					bank_a(0xCCCE, 0x1000);
					bank_b(0xCCDA, 0x1400);
					cce6();
				}
				else if (to == 0xCCEC)
				{
					// group 2: wakes a held partner on the same cell (descriptor + 0x400 /
					// + 0x600), then moves on and is held itself
					slot2();
					tick(0xCCF0, 24);
					const uint8_t follows = fl_logicb((uint8_t)(peekb((uint16_t)(0x04B0 + T.r[0])) & 0x01));
					tick(0xCCF6, 33);
					if (follows)
						tick(0xCD62, 12);              // BNE
					else
					{
						tick(0xCCF8, 12);
						T.r[0] = peek(0x09AA);
						fl::mov(T.r[0]);
						tick(0xCCFC, 24);
						T.r[0] = fl_asl(T.r[0]);
						tick(0xCCFE, 12);
						{
							const uint16_t d = peek((uint16_t)(0x0AF8 + T.r[0]));
							fl::mov(d);
							push(d);
						}
						tick(0xCD02, 39);
						ww(T.r[6], fl::add(rw(T.r[6]), 0x0400));
						tick(0xCD06, 27);
						{
							const uint16_t n = peek(0x09AA);
							fl::mov(n);
							push(n);
						}
						tick(0xCD0A, 36);
						jsr(0xCD0E, 0xCDD0);           // find_slot_with
						T.r[1] = T.r[0];
						fl::mov(T.r[1]);
						tick(0xCD10, 12);
						fl::cmp(T.r[1], 0xFFFF);
						tick(0xCD14, 18);
						bool found = T.r[1] != 0xFFFF;
						if (found)
							tick(0xCD36, 12);          // BNE
						else
						{
							tick(0xCD16, 12);
							T.r[0] = peek(0x09AA);
							fl::mov(T.r[0]);
							tick(0xCD1A, 24);
							T.r[0] = fl_asl(T.r[0]);
							tick(0xCD1C, 12);
							{
								const uint16_t d = peek((uint16_t)(0x0AF8 + T.r[0]));
								ww(T.r[6], d);
								fl::mov(d);
							}
							tick(0xCD20, 36);
							ww(T.r[6], fl::add(rw(T.r[6]), 0x0600));
							tick(0xCD24, 27);
							{
								const uint16_t n = peek(0x09AA);
								fl::mov(n);
								push(n);
							}
							tick(0xCD28, 36);
							jsr(0xCD2C, 0xCDD0);       // find_slot_with
							T.r[1] = T.r[0];
							fl::mov(T.r[1]);
							tick(0xCD2E, 12);
							fl::tst(rw(T.r[6]));
							T.r[6] = (uint16_t)(T.r[6] + 2);
							tick(0xCD30, 18);
							fl::cmp(T.r[1], 0xFFFF);
							tick(0xCD34, 18);
							found = T.r[1] != 0xFFFF;
							tick(found ? 0xCD36 : 0xCD60, 12);   // BEQ
						}
						if (found)
						{
							T.r[0] = sext8(peekb((uint16_t)(0x09B8 + T.r[1])));
							fl::movb((uint8_t)T.r[0]);
							tick(0xCD3A, 27);
							T.r[0] = fl_logic(T.r[0] & 0x003F);
							tick(0xCD3E, 18);
							const uint8_t anim = peekb((uint16_t)(0x0770 + T.r[0]));
							fl::tstb(anim);
							tick(0xCD42, 27);
							if (!(anim & 0x80))
								tick(0xCD60, 12);      // BPL: not held
							else
							{
								tick(0xCD44, 12);
								ww(T.r[6], T.r[1]);
								fl::mov(T.r[1]);
								tick(0xCD46, 21);
								push(0x0004);
								fl::mov(0x0004);
								tick(0xCD4A, 30);
								jsr(0xCD4E, 0xC98E);   // restart_object_animation
								T.r[0] = sext8(peekb((uint16_t)(0x09B8 + T.r[1])));
								fl::movb((uint8_t)T.r[0]);
								tick(0xCD52, 27);
								T.r[0] = fl_logic(T.r[0] & 0x003F);
								tick(0xCD56, 18);
								T.r[0] = fl_asl(T.r[0]);
								tick(0xCD58, 12);
								poke((uint16_t)(0x0430 + T.r[0]), 0x0700);
								fl::mov(0x0700);
								tick(0xCD5E, 36);
								fl::tst(rw(T.r[6]));
								T.r[6] = (uint16_t)(T.r[6] + 2);
								tick(0xCD60, 18);
							}
						}
						const uint16_t a = pop(), b = pop();
						fl::cmp(a, b);                 // CMP (SP)+,(SP)+
						tick(0xCD62, 24);
					}
					jsr(0xCD66, 0xCA4C);               // update_map_object_slot
					slot2();
					fl::mov(T.r[0]);
					tick(0xCD6A, 24);
					const uint8_t still = fl_logicb((uint8_t)(peekb((uint16_t)(0x04B0 + T.r[0])) & 0x01));
					tick(0xCD70, 33);
					if (still)
						tick(0xCDC4, 12);              // BNE
					else
					{
						tick(0xCD72, 12);
						const uint16_t anim = (uint16_t)(0x0770 + T.r[0]);
						pokeb(anim, fl_logicb((uint8_t)(peekb(anim) | 0x80)));
						tick(0xCD78, 36);
						T.r[0] = fl_asl(T.r[0]);
						tick(0xCD7A, 12);
						poke((uint16_t)(0x0430 + T.r[0]), 0x7FFF);
						fl::mov(0x7FFF);
						tick(0xCD80, 36);
						tick(0xCDC4, 12);              // BR
					}
				}
				else                                   // 0xCCE6: moving on
					cce6();
				break;
			}
			case 0xCD82:                               // removed once it is off both lists
			{
				slot2();
				tick(0xCD86, 24);
				const uint8_t shown = fl_logicb((uint8_t)(peekb((uint16_t)(0x0490 + T.r[0])) & 0x06));
				tick(0xCD8C, 33);
				if (shown)
				{
					tick(0xCDB8, 12);                  // BNE
					slot2();
					fl::mov(T.r[0]);
					tick(0xCDBC, 24);
					T.r[0] = fl_asl(T.r[0]);
					tick(0xCDBE, 12);
					poke((uint16_t)(0x0430 + T.r[0]), 0x000F);
					fl::mov(0x000F);
					tick(0xCDC4, 36);
				}
				else
				{
					tick(0xCD8E, 12);
					poke(0x01D2, 0);
					fl::clr();
					tick(0xCD92, 27);
					poke(0x1400, 0);
					fl::clr();
					tick(0xCD96, 27);
					T.r[0] = 0x0400;
					fl::mov(T.r[0]);
					tick(0xCD9A, 18);
					poke(0x01D4, T.r[0]);
					fl::mov(T.r[0]);
					tick(0xCD9E, 27);
					poke(0x1402, T.r[0]);
					fl::mov(T.r[0]);
					tick(0xCDA2, 27);
					{
						const uint16_t n = peek(0x09AA);
						fl::mov(n);
						push(n);
					}
					tick(0xCDA6, 36);
					jsr(0xCDAA, 0xCF5E);               // remove_map_object
					T.r[0] = sext8(peekb(0x0E96));
					fl::movb((uint8_t)T.r[0]);
					tick(0xCDAE, 24);
					T.r[0] = fl::inc(T.r[0]);
					tick(0xCDB0, 12);
					pokeb(0x0E96, (uint8_t)T.r[0]);
					fl::movb((uint8_t)T.r[0]);
					tick(0xCDB4, 27);
					fl::tst(rw(T.r[6]));
					T.r[6] = (uint16_t)(T.r[6] + 2);
					tick(0xCDB6, 18);
					tick(0xCDC4, 12);                  // BR
				}
				break;
			}
			default:                                   // 0xCCE6 (mode 1): moving on
				cce6();
				break;
			}
		}
		poke(0x022C, fl::dec(peek(0x022C)));
		tick(0xCDC8, 27);
		tick(0xCB56, 18);                              // JMP
	}
	fl::tst(rw(T.r[6]));                               // TST (SP)+: the last mode for the caller
	T.r[6] = (uint16_t)(T.r[6] + 2);
	tick(0xCDCE, 18);
	rts();
}

// ---------------------------------------------------------------- 32V interrupts
//
// Four times a frame.  Both handlers acknowledge, count the quarter (0x0191) and, unless a
// handler is already running (0x0FED), let other interrupts in (priority 0), select video bank
// 0 and work on the sprites; banks and video bank are restored at the end.  Registers and banks
// are kept on the stack as in the original.

struct Irq32v {
	Irq32v()
	{
		cy(24 * 6 + 18);                                // registers saved, SUB #6,SP
		for (int i = 0; i <= 5; i++) push(T.r[i]);
		T.r[6] = (uint16_t)(T.r[6] - 6);
		io(27, [] { poke(0x15C0, 0); });                // acknowledge
		pokeb(0x0191, (uint8_t)(peekb(0x0191) + 1));
	}
	int unwinding = std::uncaught_exceptions();
	// ADD #6,SP and the registers back, one instruction at a time (interrupts are on: one
	// coming in finds the stack as in the original); the RTI is the table charge
	~Irq32v() noexcept(false)
	{
		if (std::uncaught_exceptions() > unwinding) return;
		T.r[6] = (uint16_t)(T.r[6] + 6);
		tick(g_pc, 18);
		for (int i = 5; i >= 0; i--)
		{
			T.r[i] = pop();
			tick(g_pc, 18);
		}
	}
	Irq32v(const Irq32v &) = delete;
	Irq32v &operator=(const Irq32v &) = delete;
	// false when a handler is already running (this one only reselects the video bank)
	bool enter(uint16_t ret_8096, uint16_t ret_8068, int to_exit)
	{
		if (peekb(0x0FED))
		{
			cy(30);
			old_jsr(0x8096, ret_8096);
			cy(to_exit);                                // JMP / BR to the exit
			return false;
		}
		cy(27 + 42 + 42 + 33 + 12);                     // up to CLR R0
		pokeb(0x0FED, (uint8_t)(peekb(0x0FED) + 1));
		ww((uint16_t)(T.r[6] + 4), peek(0x01D2));
		ww((uint16_t)(T.r[6] + 2), peek(0x01D4));
		ww(T.r[6], peek(0x01D0));
		T.r[0] = 0;
		io(30, [] { mtps(0); });                        // interrupts let in
		cy(30);
		old_jsr(0x8068, ret_8068);
		hw_banks(0, 0x0400);
		return true;
	}
	void leave(uint16_t ret)
	{
		cy(225);                                        // banks back, JSR F:8096
		poke(0x01D0, rw(T.r[6]));
		poke(0x01D2, rw((uint16_t)(T.r[6] + 4)));
		poke(0x1400, peek(0x01D2));
		poke(0x01D4, rw((uint16_t)(T.r[6] + 2)));
		poke(0x1402, peek(0x01D4));
		old_jsr(0x8096, ret);
		io(27, [] { pokeb(0x0FED, (uint8_t)(peekb(0x0FED) - 1)); });   // DECB: not busy any more
	}
};

// F:B8FC (interrupt, 32V, installed by F:8270): sprite lists only: quarter 1 rebuilds list 1
// (F:B1C8, F:B1EA), moves the objects (F:B378) and updates the second chains (F:B120);
// quarter 4 rebuilds list 0 and updates the first chains (F:B092)
// the start of both 32V handlers at pc0: registers saved, room for the banks and video bank,
// acknowledge, count the quarter (0x0191); then TSTB @#0x0FED (busy?) and its BEQ to q
bool irq32_busy(uint16_t pc0, uint16_t q)
{
	for (int i = 0; i <= 5; i++)
	{
		push(T.r[i]);
		tick((uint16_t)(pc0 + 2 + 2 * i), 24);
	}
	T.r[6] = (uint16_t)(T.r[6] - 6);
	tick((uint16_t)(pc0 + 0x10), 18);
	poke(0x15C0, 0);                                   // acknowledge
	tick((uint16_t)(pc0 + 0x14), 27);
	pokeb(0x0191, (uint8_t)(peekb(0x0191) + 1));
	tick((uint16_t)(pc0 + 0x18), 27);
	const bool busy = peekb(0x0FED) != 0;
	tick((uint16_t)(pc0 + 0x1C), 24);
	tick(busy ? (uint16_t)(pc0 + 0x1E) : q, 12);       // BEQ
	return busy;
}

// not busy (q: INCB @#0x0FED): banks and video bank kept on the stack, other interrupts let in,
// video bank 0 and banks 0 / 0x0400
void irq32_open(uint16_t q)
{
	pokeb(0x0FED, (uint8_t)(peekb(0x0FED) + 1));
	tick((uint16_t)(q + 4), 27);
	poke((uint16_t)(T.r[6] + 4), peek(0x01D2));
	tick((uint16_t)(q + 0x0A), 42);
	poke((uint16_t)(T.r[6] + 2), peek(0x01D4));
	tick((uint16_t)(q + 0x10), 42);
	poke(T.r[6], peek(0x01D0));
	tick((uint16_t)(q + 0x14), 33);
	T.r[0] = 0;
	tick((uint16_t)(q + 0x16), 12);
	mtps(0);
	tick((uint16_t)(q + 0x1A), 30);
	jsr((uint16_t)(q + 0x1E), 0x8068);
	poke(0x1400, 0);
	tick((uint16_t)(q + 0x22), 27);
	poke(0x1402, 0x0400);
	tick((uint16_t)(q + 0x28), 33);
}

// the end (x: MOV (SP),@#0x01D0): banks and video bank back (F:8096), not busy any more
void irq32_close(uint16_t x)
{
	poke(0x01D0, peek(T.r[6]));
	tick((uint16_t)(x + 4), 33);
	poke(0x01D2, peek((uint16_t)(T.r[6] + 4)));
	tick((uint16_t)(x + 0x0A), 42);
	poke(0x1400, peek(0x01D2));
	tick((uint16_t)(x + 0x10), 39);
	poke(0x01D4, peek((uint16_t)(T.r[6] + 2)));
	tick((uint16_t)(x + 0x16), 42);
	poke(0x1402, peek(0x01D4));
	tick((uint16_t)(x + 0x1C), 39);
	jsr((uint16_t)(x + 0x20), 0x8096);
	pokeb(0x0FED, (uint8_t)(peekb(0x0FED) - 1));
	tick((uint16_t)(x + 0x24), 27);
}

// y: ADD #6,SP, then the registers back; the RTI is the table charge
void irq32_restore(uint16_t y)
{
	T.r[6] = (uint16_t)(T.r[6] + 6);
	tick((uint16_t)(y + 4), 18);
	for (int i = 5; i >= 0; i--)
	{
		T.r[i] = pop();
		tick((uint16_t)(y + 4 + 2 * (6 - i)), 18);
	}
}

void irq_32v_sprites()
{
	if (irq32_busy(0xB8FC, 0xB920))
	{
		jsr(0xB91E, 0x8096);
		tick(0xB9C2, 12);                              // BR
		irq32_restore(0xB9C2);
		return;
	}
	irq32_open(0xB920);
	const bool q1 = peekb(0x0191) == 1;
	fl::cmpb(peekb(0x0191), 0x01);
	tick(0xB94E, 30);
	if (q1)
	{
		tick(0xB950, 12);
		pokeb(0x0993, 1);
		fl::movb(0x01);
		tick(0xB956, 33);
		jsr(0xB95A, 0xB1C8);
		jsr(0xB95E, 0xB1EA);
		poke(0x1400, 0);
		fl::clr();
		tick(0xB962, 27);
		poke(0x1402, 0x0400);
		fl::mov(0x0400);
		tick(0xB968, 33);
		jsr(0xB96C, 0xB378);
		poke(0x1400, 0);
		fl::clr();
		tick(0xB970, 27);
		poke(0x1402, 0x0400);
		fl::mov(0x0400);
		tick(0xB976, 33);
		jsr(0xB97A, 0xB120);
		tick(0xB99E, 12);                              // BR
	}
	else
	{
		tick(0xB97C, 12);                              // BNE
		const bool q4 = peekb(0x0191) == 4;
		fl::cmpb(peekb(0x0191), 0x04);
		tick(0xB982, 30);
		if (!q4)
			tick(0xB99E, 12);                          // BNE
		else
		{
			tick(0xB984, 12);
			pokeb(0x0993, 0);
			fl::clr();
			tick(0xB988, 27);
			jsr(0xB98C, 0xB1C8);
			jsr(0xB990, 0xB1EA);
			poke(0x1400, 0);
			fl::clr();
			tick(0xB994, 27);
			poke(0x1402, 0x0400);
			fl::mov(0x0400);
			tick(0xB99A, 33);
			jsr(0xB99E, 0xB092);
		}
	}
	irq32_close(0xB99E);
	irq32_restore(0xB9C2);
}

// F:B66E (interrupt, 32V, installed by F:81AA): the game's handler.  Quarter 1: sprite list 1,
// queued piece counts (F:B03C), second chains on odd frames (F:B120) or F:AF32 every fourth,
// then the per-frame game work (B6:7942 except on level 7, F:E254, A7:4098; not in state 4)
// and the object movement (F:B378).  Quarter 4: sprite list 0 (F:AFF0, F:B1EA), first chains
// or F:AF32, and unless in state 4 the skater: B1:792A while 0x031F = 5, otherwise F:D566 /
// F:D632 or F:934C, A5:58D0 / A5:5A50 (level 3), F:94DC, F:944C, F:979A, then F:9A32, B1:7854,
// A5:4A2A; A5:5166 last.  0x0193 bit 0 (a test switch) pauses the sprites (0x0995), bit 2
// single-steps them.
void irq_32v_game()
{
	if (irq32_busy(0xB66E, 0xB694))
	{
		jsr(0xB690, 0x8096);
		tick(0xB8EA, 18);                              // JMP
		irq32_restore(0xB8EA);
		return;
	}
	irq32_open(0xB694);
	const bool q1 = peekb(0x0191) == 1;
	fl::cmpb(peekb(0x0191), 0x01);
	tick(0xB6C2, 30);
	if (q1)
	{
		tick(0xB6C4, 12);
		pokeb(0x0993, 1);
		fl::movb(0x01);
		tick(0xB6CA, 33);
		const bool pause = (peekb(0x0193) & 0x01) != 0;
		fl::logicb((uint8_t)(0x01 & peekb(0x0193)));
		tick(0xB6D0, 30);
		if (pause)
		{
			// test switch: sprites paused (0x0995), bit 2 steps them every 0x3C frames
			tick(0xB6D2, 12);
			poke(0x0180, 0x0001);
			fl::mov(0x0001);
			tick(0xB6D8, 33);
			pokeb(0x0995, 1);
			fl::movb(0x01);
			tick(0xB6DE, 33);
			const bool step = (peekb(0x0193) & 0x04) != 0;
			fl::logicb((uint8_t)(0x04 & peekb(0x0193)));
			tick(0xB6E4, 30);
			if (step)
			{
				tick(0xB6E6, 12);
				pokeb(0x0FEB, fl::decb(peekb(0x0FEB)));
				tick(0xB6EA, 27);
				const bool stepping = peekb(0x0FEA) != 0;
				fl::tstb(peekb(0x0FEA));
				tick(0xB6EE, 24);
				bool go = true;
				if (!stepping)
					tick(0xB6F6, 12);                  // BEQ
				else
				{
					tick(0xB6F0, 12);
					const bool wait = peekb(0x0FEB) != 0;
					fl::tstb(peekb(0x0FEB));
					tick(0xB6F4, 24);
					if (wait)
					{
						tick(0xB71E, 12);              // BNE
						go = false;
					}
					else
						tick(0xB6F6, 12);
				}
				if (go)
				{
					pokeb(0x0FEA, 1);
					fl::movb(0x01);
					tick(0xB6FC, 33);
					pokeb(0x0995, 0);
					fl::clr();
					tick(0xB700, 27);
					const bool wait = peekb(0x0FEB) != 0;
					fl::tstb(peekb(0x0FEB));
					tick(0xB704, 24);
					if (wait)
						tick(0xB71E, 12);              // BNE
					else
					{
						tick(0xB706, 12);
						pokeb(0x0FEB, 2);
						fl::movb(0x02);
						tick(0xB70C, 33);
						tick(0xB71E, 12);              // BR
					}
				}
			}
			else
			{
				tick(0xB70E, 12);                      // BEQ
				pokeb(0x0FEB, 0x3C);
				fl::movb(0x3C);
				tick(0xB714, 33);
				pokeb(0x0FEA, 0);
				fl::clr();
				tick(0xB718, 27);
				tick(0xB71E, 12);                      // BR
			}
		}
		else
		{
			tick(0xB71A, 12);                          // BEQ
			pokeb(0x0995, 0);
			fl::clr();
			tick(0xB71E, 27);
		}
		jsr(0xB722, 0xB1C8);
		const bool paused = peekb(0x0995) != 0;
		fl::tstb(peekb(0x0995));
		tick(0xB726, 24);
		if (paused)
			tick(0xB73A, 12);                          // BNE
		else
		{
			tick(0xB728, 12);
			jsr(0xB72C, 0xB03C);
			jsr(0xB730, 0xB1EA);
			poke(0x1400, 0);
			fl::clr();
			tick(0xB734, 27);
			poke(0x1402, 0x0400);
			fl::mov(0x0400);
			tick(0xB73A, 33);
		}
		const bool odd = (peek(0x01C4) & 0x0001) != 0;
		fl::logic((uint16_t)(0x0001 & peek(0x01C4)));
		tick(0xB740, 30);
		if (odd)
		{
			tick(0xB742, 12);
			jsr(0xB746, 0xB120);
			tick(0xB754, 12);                          // BR
		}
		else
		{
			tick(0xB748, 12);                          // BEQ
			const bool fourth = (peek(0x01C4) & 0x0003) == 0;
			fl::logic((uint16_t)(0x0003 & peek(0x01C4)));
			tick(0xB74E, 30);
			if (!fourth)
				tick(0xB754, 12);                      // BNE
			else
			{
				tick(0xB750, 12);
				jsr(0xB754, 0xAF32);
			}
		}
		const bool paused2 = peekb(0x0995) != 0;
		fl::tstb(peekb(0x0995));
		tick(0xB758, 24);
		if (paused2)
			tick(0xB7B2, 12);                          // BNE
		else
		{
			tick(0xB75A, 12);
			const bool state4 = peekb(0x018F) == 4;
			fl::cmpb(peekb(0x018F), 0x04);
			tick(0xB760, 30);
			if (state4)
				tick(0xB7A4, 12);                      // BEQ
			else
			{
				tick(0xB762, 12);
				const bool level7 = peekb(0x0F59) == 7;
				fl::cmpb(peekb(0x0F59), 0x07);
				tick(0xB768, 30);
				if (level7)
					tick(0xB786, 12);                  // BEQ
				else
				{
					tick(0xB76A, 12);
					bank_a(0xB76A, 0x2400);
					bank_b(0xB776, 0x2800);
					jsr(0xB786, 0x7942);
				}
				jsr(0xB78A, 0xE254);
				bank_a(0xB78A, 0x3800);
				jsr(0xB79A, 0x4098);
				poke(0x1400, 0);
				fl::clr();
				tick(0xB79E, 27);
				poke(0x1402, 0x0400);
				fl::mov(0x0400);
				tick(0xB7A4, 33);
			}
			poke(0x1400, 0);
			fl::clr();
			tick(0xB7A8, 27);
			poke(0x1402, 0x0400);
			fl::mov(0x0400);
			tick(0xB7AE, 33);
			jsr(0xB7B2, 0xB378);
		}
		tick(0xB8C6, 18);                              // JMP
	}
	else
	{
		tick(0xB7B6, 12);                              // BNE
		const bool q4 = peekb(0x0191) == 4;
		fl::cmpb(peekb(0x0191), 0x04);
		tick(0xB7BC, 30);
		if (!q4)
		{
			tick(0xB7B2, 12);                          // BNE
			tick(0xB8C6, 18);                          // JMP
		}
		else
		{
			tick(0xB7BE, 12);
			pokeb(0x0993, 0);
			fl::clr();
			tick(0xB7C2, 27);
			jsr(0xB7C6, 0xB1C8);
			const bool paused = peekb(0x0995) != 0;
			fl::tstb(peekb(0x0995));
			tick(0xB7CA, 24);
			if (paused)
				tick(0xB7DE, 12);                      // BNE
			else
			{
				tick(0xB7CC, 12);
				jsr(0xB7D0, 0xAFF0);
				jsr(0xB7D4, 0xB1EA);
				poke(0x1400, 0);
				fl::clr();
				tick(0xB7D8, 27);
				poke(0x1402, 0x0400);
				fl::mov(0x0400);
				tick(0xB7DE, 33);
			}
			T.r[0] = peek(0x01C4);
			fl::mov(T.r[0]);
			tick(0xB7E2, 24);
			T.r[0] = fl::inc(T.r[0]);
			tick(0xB7E4, 12);
			fl::logic((uint16_t)(0x0001 & T.r[0]));
			tick(0xB7E8, 18);                          // BIT #1,R0
			if (T.r[0] & 0x0001)
			{
				tick(0xB7EA, 12);
				jsr(0xB7EE, 0xB092);
				tick(0xB7FA, 12);                      // BR
			}
			else
			{
				tick(0xB7F0, 12);                      // BEQ
				fl::logic((uint16_t)(0x0003 & T.r[0]));
				tick(0xB7F4, 18);                      // BIT #3,R0
				if (T.r[0] & 0x0003)
					tick(0xB7FA, 12);                  // BNE
				else
				{
					tick(0xB7F6, 12);
					jsr(0xB7FA, 0xAF32);
				}
			}
			const bool state4 = peekb(0x018F) == 4;
			fl::cmpb(peekb(0x018F), 0x04);
			tick(0xB800, 30);
			if (state4)
				tick(0xB8C6, 12);                      // BEQ
			else
			{
				tick(0xB802, 12);
				const bool paused2 = peekb(0x0995) != 0;
				fl::tstb(peekb(0x0995));
				tick(0xB806, 24);
				if (paused2)
					tick(0xB8C6, 12);                  // BNE
				else
				{
					// the skater
					tick(0xB808, 12);
					const bool phase5 = peekb(0x031F) == 5;
					fl::cmpb(peekb(0x031F), 0x05);
					tick(0xB80E, 30);
					if (phase5)
					{
						tick(0xB89A, 12);              // BEQ
						bank_a(0xB89A, 0x1800);
						bank_b(0xB8A6, 0x1C00);
						jsr(0xB8B6, 0x792A);
					}
					else
					{
						tick(0xB810, 12);
						uint8_t contact = peekb(0x0EA0);
						fl::tstb(peekb(0x0EA0));
						tick(0xB814, 24);
						if (contact & 0x80)
						{
							tick(0xB816, 12);          // just reached the object
							jsr(0xB81A, 0xD566);
							contact = peekb(0x0EA0);
							fl::tstb(peekb(0x0EA0));
							tick(0xB81E, 24);
						}
						else
							tick(0xB81E, 12);          // BGE
						if (contact)
						{
							tick(0xB864, 12);          // BNE
							jsr(0xB868, 0xD632);
						}
						else
						{
							tick(0xB820, 12);
							jsr(0xB824, 0x934C);
							bank_a(0xB824, 0x1800);
							const bool level3 = peekb(0x0F59) == 3;
							fl::cmpb(peekb(0x0F59), 0x03);
							tick(0xB836, 30);
							bool usual = true;
							if (level3)
							{
								tick(0xB838, 12);
								jsr(0xB83C, 0x58D0);
								fl::tst(T.r[0]);
								tick(0xB83E, 12);      // TST R0
								if (T.r[0])
									tick(0xB856, 12);  // BNE
								else
								{
									tick(0xB840, 12);
									jsr(0xB844, 0x944C);
									bank_a(0xB844, 0x1800);
									jsr(0xB854, 0x5A50);
									tick(0xB85E, 12);  // BR
									usual = false;
								}
							}
							else
								tick(0xB856, 12);      // BNE
							if (usual)
							{
								jsr(0xB85A, 0x94DC);
								jsr(0xB85E, 0x944C);
							}
							jsr(0xB862, 0x979A);
							tick(0xB868, 12);          // BR
						}
						jsr(0xB86C, 0x9A32);
						bank_a(0xB86C, 0x1800);
						bank_b(0xB878, 0x1C00);
						jsr(0xB888, 0x7854);
						bank_a(0xB888, 0x1800);
						jsr(0xB898, 0x4A2A);
						tick(0xB8B6, 12);              // BR
					}
					bank_a(0xB8B6, 0x1800);
					jsr(0xB8C6, 0x5166);
				}
			}
		}
	}
	irq32_close(0xB8C6);
	irq32_restore(0xB8EA);
}

// ---------------------------------------------------------------- numbers

// F:BA96 / F:BA9A  print_number(int separators, int lead, int digits, long value): print the
// value (high word, low word) with the given number of digits (at most 10; only the low digits
// are kept) at the text cursor, with the small font (F:8C12) or the large one (F:BA9A: F:8B52,
// digits F:8C56).  Leading zeros: lead = 0 drops them and pads with blanks at the end (left
// aligned); lead = k blanks up to digits - k of them in place.  separators != 0 puts ',' after
// the thousands, millions and billions digits.  The cursor routines get the original's
// registers; R0 is what the last of them returns.
constexpr uint16_t POW10 = 0xBA6A;                     // JSR PC,addr in PC-relative mode (33 cycles instead of 30) from ret - 4
void jsr_rel(uint16_t ret, uint16_t addr)
{
	push(ret);
	T.r[7] = addr;
	tick(addr, 33);
	shadow_push(ret);
	dispatch();
	shadow_pop();
	if (T.r[7] != ret) throw Unwound{};
}

// JSR R1,F162 in PC-relative mode (33 cycles) at body - 4: steps::c_save(4, body) otherwise
void c_save_rel4(uint16_t body)
{
	uint16_t pc = 0xF162;
	const uint16_t r1 = T.r[1];
	push(T.r[1]);
	T.r[1] = body;
	tick(pc, 33);                                      // JSR R1,F162
	for (int i = 2; i <= 5; i++)
	{
		push(T.r[i]);
		pc += 2;
		tick(pc, 24);                                  // MOV Ri,-(SP)
	}
	push(T.r[1]);
	pc += 2;
	tick(pc, 24);                                      // MOV R1,-(SP)
	T.r[1] = r1;
	pc += 4;
	tick(pc, 27);                                      // MOV 0xA(SP),R1
	ww(T.r[6], (uint16_t)(pc + 2));
	tick(body, 30);                                    // JSR PC,@(SP)+
}

// the body of F:BA96 / F:BA9A from F:BA9E (R0 = font: 0 small, 0x8000 large)
uint16_t print_number()
{
	c_save_rel4(0xBAA2);
	push(T.r[0]);
	tick(0xBAA4, 24);
	jsr_rel(0xBAA8, 0x8068);
	T.r[4] = pop();
	tick(0xBAAA, 18);
	// arguments: 0xE(SP) separators, 0x10 lead, 0x12 digits, 0x14 / 0x16 value (high, low)
	auto arg = [](int off) { return peek((uint16_t)(T.r[6] + off)); };
	T.r[2] = arg(0x16);
	tick(0xBAAE, 27);
	T.r[3] = arg(0x14);
	tick(0xBAB2, 27);
	T.r[4] |= (uint8_t)arg(0x12);
	tick(0xBAB6, 27);
	fl::cmpb(0x0A, (uint8_t)T.r[4]);
	tick(0xBABA, 18);
	if (fC())
	{
		tick(0xBABC, 12);                              // at most ten digits
		T.r[4] &= 0xFF00;
		tick(0xBABE, 12);
		T.r[4] |= 0x000A;
		tick(0xBAC2, 18);
	}
	else
		tick(0xBAC2, 12);                              // BCC
	T.r[5] = sext8((uint8_t)T.r[4]);
	tick(0xBAC4, 12);
	T.r[0] = sext8((uint8_t)arg(0x10));
	tick(0xBAC8, 27);
	if (!T.r[0])
	{
		tick(0xBACA, 12);
		T.r[0] = 0x8001;
		tick(0xBACE, 18);
	}
	else
		tick(0xBACE, 12);                              // BNE
	T.r[5] = (uint16_t)(T.r[5] - T.r[0]);
	tick(0xBAD0, 12);
	T.r[1] = sext8((uint8_t)T.r[4]);
	tick(0xBAD2, 12);
	T.r[1] = (uint16_t)(T.r[1] << 1);
	tick(0xBAD4, 12);
	T.r[1] = (uint16_t)(T.r[1] << 1);
	tick(0xBAD6, 12);
	T.r[1] = (uint16_t)(T.r[1] + 0xBA6A);              // 10^n at 0xBA6A + 4n (high word first)
	tick(0xBADA, 18);
	// [R3:R2] -= the power of ten at R1 while it does not borrow, from pc (SUB 2(R1),R2 / INC R0)
	// to the ADC R3 that undoes the failing subtraction
	auto subtract = [](uint16_t pc, bool count) {
		for (;;)
		{
			if (count)
			{
				T.r[0]++;
				tick((uint16_t)(pc + 2), 12);          // INC R0
				pc += 2;
			}
			T.r[2] = fl::sub(T.r[2], peek((uint16_t)(T.r[1] + 2)));
			tick((uint16_t)(pc + 4), 27);
			T.r[3] = fl::sbc(T.r[3]);
			tick((uint16_t)(pc + 6), 12);
			if (fC())
			{
				tick((uint16_t)(pc + 0x0E), 12);       // BCS
				break;
			}
			tick((uint16_t)(pc + 8), 12);
			T.r[3] = fl::sub(T.r[3], peek(T.r[1]));
			tick((uint16_t)(pc + 0x0A), 18);
			if (!fC())
			{
				tick(count ? (uint16_t)(pc - 2) : pc, 12);   // BCC
				if (count) pc -= 2;
				continue;
			}
			tick((uint16_t)(pc + 0x0C), 12);
			T.r[3] = fl::add(T.r[3], peek(T.r[1]));
			tick((uint16_t)(pc + 0x0E), 18);
			break;
		}
		T.r[2] = fl::add(T.r[2], peek((uint16_t)(T.r[1] + 2)));
		tick((uint16_t)(pc + 0x12), 27);
		T.r[3] = fl::adc(T.r[3]);
		tick((uint16_t)(pc + 0x14), 12);
	};
	fl::cmp(peek(T.r[1]), T.r[3]);
	tick(0xBADC, 18);
	bool reduce;
	if (!fC() && !fZ())
	{
		tick(0xBB24, 12);                              // BHI: the value fits
		reduce = false;
	}
	else
	{
		tick(0xBADE, 12);
		if (!fZ())
		{
			tick(0xBAE6, 12);                          // BNE
			reduce = true;
		}
		else
		{
			tick(0xBAE0, 12);
			fl::cmp(peek((uint16_t)(T.r[1] + 2)), T.r[2]);
			tick(0xBAE4, 27);
			reduce = fC() || fZ();
			tick(reduce ? 0xBAE6 : 0xBB24, 12);        // BHI
		}
	}
	if (reduce)
	{
		// too many digits: the value is reduced modulo 10^digits (F:BAE6-BB20)
		T.r[5] &= 0xFF00;
		tick(0xBAE8, 12);
		T.r[0] = T.r[1];
		tick(0xBAEA, 12);
		bool from_top = false;
		for (;;)
		{
			fl::cmp(0xBA8E, T.r[1]);
			tick(0xBAEE, 18);
			if (fC())
			{
				tick(0xBB02, 12);                      // BCS
				break;
			}
			tick(0xBAF0, 12);
			if (fZ())
			{
				tick(0xBB0A, 12);                      // BEQ
				from_top = true;
				break;
			}
			tick(0xBAF2, 12);
			T.r[1] = (uint16_t)(T.r[1] + 4);
			tick(0xBAF6, 18);
			fl::cmp(peek(T.r[1]), T.r[3]);
			tick(0xBAF8, 18);
			if (!fC() && !fZ())
			{
				tick(0xBB02, 12);                      // BHI
				break;
			}
			tick(0xBAFA, 12);
			if (!fZ())
			{
				tick(0xBAEA, 12);                      // BNE
				continue;
			}
			tick(0xBAFC, 12);
			fl::cmp(peek((uint16_t)(T.r[1] + 2)), T.r[2]);
			tick(0xBB00, 27);
			if (fC() || fZ())
			{
				tick(0xBAEA, 12);                      // BLOS
				continue;
			}
			tick(0xBB02, 12);
			break;
		}
		for (;;)
		{
			if (!from_top)
			{
				T.r[1] = (uint16_t)(T.r[1] - 4);
				tick(0xBB06, 18);
				fl::cmp(T.r[1], T.r[0]);
				tick(0xBB08, 12);
				if (fC())
				{
					tick(0xBB20, 12);                  // BCS
					T.r[1] = (uint16_t)(T.r[1] + 4);
					tick(0xBB24, 18);
					break;
				}
				tick(0xBB0A, 12);
			}
			from_top = false;
			subtract(0xBB0A, false);
			tick(0xBB02, 12);                          // BR
		}
	}
	T.r[4] = (uint16_t)((T.r[4] & 0xFF00) | fl::decb((uint8_t)T.r[4]));
	tick(0xBB26, 12);
	for (;;)
	{
		// one digit (DECB R4 was the last flag change)
		enum { DIGIT, BLANK, COUNT, NEXT } step = DIGIT;
		if (!fZ() && fN() == fV())
		{
			tick(0xBB2C, 12);                          // BGT
			T.r[1] = (uint16_t)(T.r[1] - 4);
			tick(0xBB30, 18);
			T.r[0] = 0;
			tick(0xBB32, 12);
			subtract(0xBB32, true);
			T.r[0] = fl::dec(T.r[0]);
			tick(0xBB4A, 12);
			if (!fZ())
			{
				tick(0xBB72, 12);                      // BNE: a non-zero digit
				T.r[5] &= 0xFF00;
				tick(0xBB74, 12);
			}
			else
			{
				tick(0xBB4C, 12);
				T.r[5] = (uint16_t)((T.r[5] & 0xFF00) | fl::decb((uint8_t)T.r[5]));
				tick(0xBB4E, 12);
				if (fN())
				{
					tick(0xBB72, 12);                  // BMI: a zero that is shown
					T.r[5] &= 0xFF00;
					tick(0xBB74, 12);
				}
				else
				{
					tick(0xBB50, 12);
					fl::tst(T.r[5]);
					tick(0xBB52, 12);
					if (fN())
					{
						tick(0xBBBE, 12);              // BMI: a dropped leading zero
						step = NEXT;
					}
					else
					{
						// a blank in place of a leading zero
						tick(0xBB54, 12);
						T.r[0] = 0;
						tick(0xBB58, 18);
						fl::tst(T.r[4]);
						tick(0xBB5A, 12);
						step = BLANK;
						if (fN())
						{
							tick(0xBB5C, 12);
							jsr_rel(0xBB60, 0x8B52);
							fl::tst(arg(0x0E));
							tick(0xBB64, 27);
							if (fZ())
							{
								tick(0xBB66, 12);
								tick(0xBBBA, 12);      // BR
								step = COUNT;
							}
							else
								tick(0xBB6C, 12);      // BNE
						}
						else
						{
							tick(0xBB68, 12);          // BPL
							jsr_rel(0xBB6C, 0x8C12);
						}
						if (step == BLANK)
						{
							T.r[0] = 0;
							tick(0xBB70, 18);
							tick(0xBB90, 12);          // BR
						}
					}
				}
			}
		}
		else
		{
			tick(0xBB28, 12);
			T.r[0] = T.r[2];                           // the last digit
			tick(0xBB2A, 12);
			tick(0xBB74, 12);                          // BR
		}
		if (step == DIGIT)
		{
			T.r[0] = (uint16_t)(T.r[0] + 0x0030);
			tick(0xBB78, 18);
			fl::tst(T.r[4]);
			tick(0xBB7A, 12);
			if (fN())
			{
				tick(0xBB7C, 12);
				jsr_rel(0xBB80, 0x8C56);
				tick(0xBB86, 12);                      // BR
			}
			else
			{
				tick(0xBB82, 12);                      // BPL
				jsr_rel(0xBB86, 0x8C12);
			}
			fl::tst(arg(0x0E));
			tick(0xBB8A, 27);
			if (fZ())
			{
				tick(0xBBBA, 12);                      // BEQ: no separators
				step = COUNT;
			}
			else
			{
				tick(0xBB8C, 12);
				T.r[0] = 0x002C;
				tick(0xBB90, 18);
			}
		}
		if (step == DIGIT || step == BLANK)
		{
			// a separator (or its blank) after the billions, millions and thousands
			bool sep = false;
			static const uint16_t at[3] = {0xBA76, 0xBA82, 0xBA8E};
			for (int k = 0; k < 3 && !sep; k++)
			{
				const uint16_t pc = (uint16_t)(0xBB90 + 6 * k);
				fl::cmp(T.r[1], at[k]);
				tick((uint16_t)(pc + 4), 18);
				sep = fZ();
				if (k < 2)
					tick(sep ? 0xBBA2 : (uint16_t)(pc + 6), 12);   // BEQ
				else
					tick(sep ? 0xBBA2 : 0xBBBA, 12);   // BNE
			}
			if (sep)
			{
				fl::tst(T.r[4]);
				tick(0xBBA4, 12);
				if (fN())
				{
					tick(0xBBA6, 12);
					fl::tst(T.r[0]);
					tick(0xBBA8, 12);
					if (fZ())
					{
						tick(0xBBAA, 12);
						poke(0x0186, fl::sub(peek(0x0186), 0x0002));
						tick(0xBBB0, 36);
					}
					else
						tick(0xBBB0, 12);              // BNE
					jsr_rel(0xBBB4, 0x8B52);
					tick(0xBBBA, 12);                  // BR
				}
				else
				{
					tick(0xBBB6, 12);                  // BPL
					jsr_rel(0xBBBA, 0x8C12);
				}
			}
			step = COUNT;
		}
		if (step == COUNT)
		{
			T.r[5] = fl::add(T.r[5], 0x0100);          // a digit place used
			tick(0xBBBE, 18);
		}
		T.r[4] = (uint16_t)((T.r[4] & 0xFF00) | fl::decb((uint8_t)T.r[4]));
		tick(0xBBC0, 12);
		if (fN() != fV()) break;
		tick(0xBB26, 12);                              // BGE
	}
	tick(0xBBC2, 12);
	fl::tst(T.r[5]);
	tick(0xBBC4, 12);
	if (!fN())
		tick(0xBBEA, 12);                              // BPL
	else
	{
		// left aligned: blanks for the dropped digits
		tick(0xBBC6, 12);
		T.r[1] = arg(0x12);
		tick(0xBBCA, 27);
		T.r[5] = fl_swab(T.r[5]);
		tick(0xBBCC, 12);
		T.r[5] &= 0x003F;
		tick(0xBBD0, 18);
		T.r[1] = fl::sub(T.r[1], T.r[5]);
		tick(0xBBD2, 12);
		if (fZ())
			tick(0xBBEA, 12);                          // BEQ
		else
		{
			tick(0xBBD4, 12);
			for (;;)
			{
				T.r[0] = 0;
				tick(0xBBD8, 18);
				fl::tst(T.r[4]);
				tick(0xBBDA, 12);
				if (fN())
				{
					tick(0xBBDC, 12);
					jsr_rel(0xBBE0, 0x8B52);
					tick(0xBBE6, 12);                  // BR
				}
				else
				{
					tick(0xBBE2, 12);                  // BPL
					jsr_rel(0xBBE6, 0x8C12);
				}
				T.r[1] = fl::dec(T.r[1]);
				tick(0xBBE8, 12);
				if (fZ() || fN() != fV()) break;
				tick(0xBBD4, 12);                      // BGT
			}
			tick(0xBBEA, 12);
		}
	}
	c_restore(4);
	return T.r[0];
}

uint16_t print_number_small(const Args &)
{
	T.r[0] = 0;
	fl::clr();
	tick(0xBA98, 12);
	tick(0xBA9E, 12);                                  // BR
	return print_number();
}
uint16_t print_number_large(const Args &)
{
	T.r[0] = 0x8000;
	fl::mov(T.r[0]);
	tick(0xBA9E, 18);
	return print_number();
}

// F:BBEC (registers): the view rectangle 0x0996-0x099C is copied to 0x0224-0x022A (x / y
// first) and the next eight map slots 0x09AC (wrapping from 0x09B0 to 0x0E98) are looked at:
// those with a type (0x09B8) get F:BCB8 with 0x09AA = the slot.  Banks 0x1000 / 0x1400.
void check_some_map_slots()
{
	bank_a(0xBBEC, 0x1000);
	bank_b(0xBBF8, 0x1400);
	poke(0x0224, peek(0x099A));
	tick(0xBC0A, 39);
	poke(0x0226, peek(0x099C));
	fl::mov(peek(0x0226));
	tick(0xBC10, 39);
	poke(0x0228, peek(0x0996));
	fl::mov(peek(0x0228));
	tick(0xBC16, 39);
	poke(0x022A, peek(0x0998));
	fl::mov(peek(0x022A));
	tick(0xBC1C, 39);
	push(0x0008);
	fl::mov(0x0008);
	tick(0xBC20, 30);
	for (;;)
	{
		poke(0x09AC, fl::inc(peek(0x09AC)));
		tick(0xBC24, 27);
		const bool wrap = (int16_t)peek(0x09AC) > (int16_t)peek(0x09B0);
		fl::cmp(peek(0x09AC), peek(0x09B0));
		tick(0xBC2A, 36);
		if (wrap)
		{
			tick(0xBC2C, 12);
			poke(0x09AC, 0);
			fl::clr();
			tick(0xBC30, 27);
			pokeb(0x09AC, peekb(0x0E98));
			fl::movb(peekb(0x09AC));
			tick(0xBC36, 39);
		}
		else
			tick(0xBC36, 12);                          // BLE
		poke(0x09AA, peek(0x09AC));
		fl::mov(peek(0x09AA));
		tick(0xBC3C, 39);
		T.r[0] = peek(0x09AA);
		fl::mov(T.r[0]);
		tick(0xBC40, 24);
		const bool typed = peekb((uint16_t)(0x09B8 + T.r[0])) != 0;
		fl::tstb(peekb((uint16_t)(T.r[0] + 0x09B8)));
		tick(0xBC44, 27);
		if (typed)
		{
			tick(0xBC46, 12);
			jsr(0xBC4A, 0xBCB8);
		}
		else
			tick(0xBC4A, 12);                          // BEQ
		poke(T.r[6], fl::dec(peek(T.r[6])));
		tick(0xBC4C, 21);
		if (fZ()) break;
		tick(0xBC20, 12);                              // BNE
	}
	tick(0xBC4E, 12);
	fl::tst(pop());
	tick(0xBC50, 18);
	rts();
}

// F:CA0C (registers): a map object in mode 3 is animated: unless the skater's object
// (0x022C) has bit 0 of 0x04B0 set, a record (0x09B6, bank A) with bit 4 of its first byte
// looks for the object of this slot (F:CDD0 find_slot_with(0x09AA, 0x0AF8[0x09AA])) and
// restarts its animation 5 (F:C98E).
void animate_map_object()
{
	T.r[0] = peek(0x022C);                             // the skater's object
	fl::mov(T.r[0]);
	tick(0xCA10, 24);
	const uint8_t held = fl_logicb((uint8_t)(peekb((uint16_t)(0x04B0 + T.r[0])) & 0x01));
	tick(0xCA16, 33);
	if (held)
		tick(0xCA4A, 12);                              // BNE
	else
	{
		tick(0xCA18, 12);
		T.r[0] = peek(0x09B6);
		fl::mov(T.r[0]);
		tick(0xCA1C, 24);
		const uint8_t animated = fl_logicb((uint8_t)(peekb((uint16_t)(0x4000 + T.r[0])) & 0x10));
		tick(0xCA22, 33);
		if (!animated)
			tick(0xCA4A, 12);                          // BEQ
		else
		{
			tick(0xCA24, 12);
			T.r[0] = peek(0x09AA);
			fl::mov(T.r[0]);
			tick(0xCA28, 24);
			T.r[0] = fl_asl(T.r[0]);
			tick(0xCA2A, 12);
			{
				const uint16_t v = peek((uint16_t)(0x0AF8 + T.r[0]));
				fl::mov(v);
				push(v);
			}
			tick(0xCA2E, 39);
			{
				const uint16_t v = peek(0x09AA);
				fl::mov(v);
				push(v);
			}
			tick(0xCA32, 36);
			jsr(0xCA36, 0xCDD0);                       // find_slot_with
			fl::cmp(T.r[0], 0xFFFF);
			tick(0xCA3A, 18);
			if (T.r[0] == 0xFFFF)
				tick(0xCA48, 12);                      // BEQ
			else
			{
				tick(0xCA3C, 12);
				ww(T.r[6], T.r[0]);
				fl::mov(T.r[0]);
				tick(0xCA3E, 21);
				push(0x0005);
				fl::mov(0x0005);
				tick(0xCA42, 30);
				jsr(0xCA46, 0xC98E);                   // restart_object_animation
				fl::tst(rw(T.r[6]));
				T.r[6] = (uint16_t)(T.r[6] + 2);
				tick(0xCA48, 18);
			}
			const uint16_t a = pop(), b = pop();
			fl::cmp(a, b);                             // CMP (SP)+,(SP)+
			tick(0xCA4A, 24);
		}
	}
	rts();
}

}

LOGIC_TABLE(fmid,
	asm_routine("F", 0xB03C, "apply_queued_piece_counts", apply_queued_piece_counts, 0),
	asm_routine("F", 0xB092, "update_chains_a", update_chains_a, 0),
	asm_routine("F", 0xB120, "update_chains_b", update_chains_b, 0),
	asm_routine("F", 0xB1EA, "build_sprite_pieces", build_sprite_pieces, 0),
	asm_routine("F", 0xB378, "move_objects", move_objects, 0),
	c_routine("F", 0xB4B6, "show_object", show_object, 0, 0),
	c_routine("F", 0xB602, "queue_piece_count", queue_piece_count, 0, 0),
	c_routine("F", 0xBA2E, "clear_objects_and_text", clear_objects_and_text, 0, 0),
	c_routine("F", 0xC98E, "restart_object_animation", restart_object_animation, 0, 0),
	c_routine("F", 0xBE10, "locate_map_object", locate_map_object, 0, 0),
	c_routine("F", 0xBE7C, "map_object_state", map_object_state, 0, 0),
	c_routine("F", 0xBF5E, "map_object_position", map_object_position, 0, 0),
	asm_routine("F", 0xBD78, "map_object_enters", map_object_enters, 0),
	c_routine("F", 0xBA96, "print_number_small", print_number_small, 0, 0),
	c_routine("F", 0xBA9A, "print_number_large", print_number_large, 0, 0),
	c_routine("F", 0xBF18, "release_map_definition", release_map_definition, 0, 0),
	c_routine("F", 0xC114, "turn_map_object", turn_map_object, 0, 0),
	c_routine("F", 0xCF5E, "remove_map_object", remove_map_object, 0, 0),
	c_routine("F", 0xCE24, "add_map_object", add_map_object, 0, 0),
	c_routine("F", 0xCFD6, "init_map_objects", init_map_objects, 0, 0),
	c_routine("F", 0xC3E6, "map_object_leaves", map_object_leaves, 0, 0),
	asm_routine("F", 0xBCB8, "check_map_object_view", check_map_object_view, 0),
	c_routine("F", 0xBFBC, "move_map_object", move_map_object, 0, 0),
	c_routine("F", 0xC4E4, "create_map_object", create_map_object, 0, 0),
	irq_routine("F", 0xB8FC, "irq_32v_sprites", irq_32v_sprites, 24),          // RTI
	irq_routine("F", 0xB66E, "irq_32v_game", irq_32v_game, 24),                // RTI
	asm_routine("F", 0xC850, "try_map_object_move", try_map_object_move, 0),
	c_routine("F", 0xC868, "try_next_map_definition", try_next_map_definition, 0, 0),
	asm_routine("F", 0xCA4C, "update_map_object_slot", update_map_object_slot, 0),
	asm_routine("F", 0xBC52, "check_map_objects_in_view", check_map_objects_in_view, 0),
	asm_routine("F", 0xC8AA, "tick_map_object_timers", tick_map_object_timers, 0),
	asm_routine("F", 0xCB24, "update_objects", update_objects, 0),
	asm_routine("F", 0xBBEC, "check_some_map_slots", check_some_map_slots, 0),
	asm_routine("F", 0xCA0C, "animate_map_object", animate_map_object, 0),
)

}
