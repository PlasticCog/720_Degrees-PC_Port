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
// calls go through jsr() with the original's return address, the register-save helpers'
// frames are written (CFrame::save_regs) and locals live where the original keeps them; the
// registers handed to other routines are the original's working registers.
#include "logic.h"
#include <cstdio>
#include <exception>

namespace logic {
namespace {

using namespace t11rt;

uint16_t sext8(uint8_t v) { return (uint16_t)(int16_t)(int8_t)v; }

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
uint16_t jsr(uint16_t addr, uint16_t ret)
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
	At here(0xB03C);
	cy(36);
	const uint8_t pending = peekb(0x0992);
	fl::tstb(pending);
	if (!pending)
	{
		cy(21);
		return;
	}
	cy(18);
	T.r[2] = 4;
	do
	{
		int c = 39;                                     // F:B046-B04A
		const uint8_t slot = peekb((uint16_t)(0x0989 + T.r[2]));
		T.r[1] = sext8(slot);
		fl::movb(slot);
		if (slot)
		{
			c += 111;                                   // F:B04C-B05C
			pokeb((uint16_t)(0x04D0 + T.r[1]), peekb((uint16_t)(0x098D + T.r[2])));
			T.r[4] = fl::add(0x0490, T.r[1]);
			if (peekb(T.r[4]) & 0x04)
			{
				T.r[3] = (uint16_t)(T.r[1] + 0x20);
				push(1);
				push(T.r[3]);
				cy(c + 114);
				c = 0;
				jsr(0xACE6, 0xB06E);
				cy(81);
				ww(T.r[6], 1);
				push(T.r[3]);
				jsr(0xAE02, 0xB078);
				c += 18;
				T.r[6] = fl::add(T.r[6], 6);
			}
			c += 63;                                    // F:B07C-B084
			const uint8_t f = (uint8_t)(peekb(T.r[4]) & ~0x40);
			pokeb(T.r[4], f);
			f_nzv(nzb(f & 0x20));
			if (!(f & 0x20))
			{
				c += 57;
				pokeb(0x0992, fl::decb(peekb(0x0992)));
				pokeb((uint16_t)(0x0989 + T.r[2]), 0);
				fl::clr();
			}
		}
		charge(T.r[2] > 1 ? 0xB046 : 0xB090, c + 18);     // SOB
	} while (--T.r[2]);
	cy(21);
}

// top edge used by F:B092 / F:B120: 0x4F80 - the height table entry 0x5BCC of type R1
uint16_t type_height(uint16_t type) { return peek((uint16_t)(0x5BCC + 2 * type)); }

// F:B092 (assembly): first chains of objects with flag 0x02 (shown): taken out of the display
// list unless the object (flag 0x80) is on screen (y >= 0x4F80 - height, x >= 0xD400 signed);
// objects not shown but on screen with pieces are put in.  Slots 0x0994 down to 0.
void update_chains_a()
{
	At here(0xB092);
	cy(36);
	const uint8_t top = peekb(0x0994);
	T.r[3] = sext8(top);
	fl::movb(top);
	if (lt())
	{
		cy(21);
		return;
	}
	do
	{
		int c = 108;                                    // F:B098-B0A8
		T.r[1] = peekb((uint16_t)(0x0370 + T.r[3]));
		T.r[2] = peekb((uint16_t)(0x0490 + T.r[3]));
		uint16_t target = 0;
		fl::tstb((uint8_t)T.r[2]);
		if (T.r[2] & 0x02)
		{
			c += 24;
			target = 0xACE6;
			if (T.r[2] & 0x80)
			{
				c += 144;
				T.r[4] = (uint16_t)(T.r[3] * 2);
				T.r[0] = (uint16_t)-(uint16_t)(type_height(T.r[1]) - 0x4F80);
				fl::cmp(peek((uint16_t)(0x0670 + T.r[4])), T.r[0]);
				if (!lt())
				{
					c += 69;
					T.r[0] = (uint16_t)(T.r[3] * 2);
					fl::cmp(peek((uint16_t)(0x0630 + T.r[0])), 0xD400);
					if (!lt()) target = 0;
				}
			}
		}
		else
		{
			c += 24;
			if (T.r[2] & 0x80)
			{
				c += 39;
				const uint8_t pieces = peekb((uint16_t)(0x04D0 + T.r[3]));
				fl::tstb(pieces);
				if (pieces)
				{
					c += 30 + 144;
					T.r[4] = (uint16_t)(T.r[3] * 2);
					T.r[0] = (uint16_t)-(uint16_t)(type_height(T.r[1]) - 0x4F80);
					fl::cmp(peek((uint16_t)(0x0670 + T.r[4])), T.r[0]);
					if (gt())
					{
						c += 69;
						T.r[0] = (uint16_t)(T.r[3] * 2);
						fl::cmp(peek((uint16_t)(0x0630 + T.r[0])), 0xD400);
						if (!lt()) target = 0xAE02;
					}
				}
			}
		}
		if (target)
		{
			cy(c + 78);
			c = 0;
			push(0);
			push(T.r[3]);
			jsr(target, target == 0xACE6 ? 0xB0DA : 0xB118);
			c += (target == 0xACE6 ? 12 : 0) + 24;
			fl::cmp(rw(T.r[6]), rw((uint16_t)(T.r[6] + 2)));
			T.r[6] = (uint16_t)(T.r[6] + 4);
		}
		T.r[3] = fl::dec(T.r[3]);
		const bool more = !lt();
		charge(more ? 0xB098 : 0xB11E, c + 24);           // DEC, BGE
		if (!more) break;
	} while (true);
	cy(21);
}

// F:B120 (assembly): the same for the second chains (slot + 0x20) with flag 0x04: visible
// when y <= 0x5080 (unsigned), y + height >= 0x2000 (unsigned) and x >= 0xD400 (signed)
void update_chains_b()
{
	At here(0xB120);
	cy(36);
	const uint8_t top = peekb(0x0994);
	T.r[3] = sext8(top);
	fl::movb(top);
	if (lt())
	{
		cy(21);
		return;
	}
	do
	{
		int c = 108;                                    // F:B126-B136
		T.r[1] = peekb((uint16_t)(0x0370 + T.r[3]));
		T.r[2] = peekb((uint16_t)(0x0490 + T.r[3]));
		uint16_t target = 0;
		const uint16_t w = (uint16_t)(T.r[3] * 2);
		fl::tstb((uint8_t)T.r[2]);
		if (T.r[2] & 0x04)
		{
			c += 24;
			target = 0xACE6;
			if (T.r[2] & 0x80)
			{
				c += 69;
				T.r[0] = w;
				fl::cmp(peek((uint16_t)(0x0670 + w)), 0x5080);
				if (!(!fC() && !fZ()))
				{
					c += 132;
					T.r[4] = w;
					T.r[0] = (uint16_t)(T.r[1] * 2);
					T.r[4] = (uint16_t)(peek((uint16_t)(0x0670 + w)) + peek((uint16_t)(0x5BCC + T.r[0])));
					fl::cmp(T.r[4], 0x2000);
					if (!fC())
					{
						c += 69;
						T.r[0] = w;
						fl::cmp(peek((uint16_t)(0x0630 + w)), 0xD400);
						if (!lt()) target = 0;
					}
				}
			}
		}
		else
		{
			c += 24;
			if (T.r[2] & 0x80)
			{
				c += 39;
				const uint8_t pieces = peekb((uint16_t)(0x04D0 + T.r[3]));
				fl::tstb(pieces);
				if (pieces)
				{
					c += 69;
					T.r[0] = w;
					fl::cmp(peek((uint16_t)(0x0670 + w)), 0x5080);
					if (fC() || fZ())
					{
						c += 132;
						T.r[4] = w;
						T.r[0] = (uint16_t)(T.r[1] * 2);
						T.r[4] = (uint16_t)(peek((uint16_t)(0x0670 + w)) + peek((uint16_t)(0x5BCC + T.r[0])));
						fl::cmp(T.r[4], 0x2000);
						if (!fC())
						{
							c += 69;
							T.r[0] = w;
							fl::cmp(peek((uint16_t)(0x0630 + w)), 0xD400);
							if (gt()) target = 0xAE02;
						}
					}
				}
			}
		}
		if (target)
		{
			cy(c + 111);
			c = 0;
			push(1);
			push((uint16_t)(T.r[3] + 0x20));
			jsr(target, target == 0xACE6 ? 0xB178 : 0xB1C0);
			c += (target == 0xACE6 ? 12 : 0) + 24;
			fl::cmp(rw(T.r[6]), rw((uint16_t)(T.r[6] + 2)));
			T.r[6] = (uint16_t)(T.r[6] + 4);
		}
		T.r[3] = fl::dec(T.r[3]);
		const bool more = !lt();
		charge(more ? 0xB126 : 0xB1C6, c + 24);           // DEC, BGE
		if (!more) break;
	} while (true);
	cy(21);
}

// F:B1EA (assembly): rebuild the sprite pieces of the chains in display list 0x0993 (0 or 1),
// following the links 0x0808 from the head 0x0988+list (current link in 0x0FEE).  Objects
// whose flags (masked by 0xFD7E+list) ask for it get their pieces from the frame data
// (0x03B0 points to it, bank 0x3000/0x3400): x offset, y offset, then link/picture words, with
// the palette byte 0x0470; the others only move their pieces by the screen-y delta 0x05F0.
// Objects with a flag of 0xFD80+list are skipped.
void build_sprite_pieces()
{
	At here(0xB1EA);
	cy(24 + 24 + 12 + 27 + 27);                         // F:B1EA-B1F6
	const uint16_t list = sext8(peekb(0x0993));
	push(list);                                         // the list is kept on the stack
	const uint16_t sp = T.r[6];
	T.r[1] = list;
	T.r[0] = sext8(peekb((uint16_t)(0x0988 + list)));
	poke(0x0FEE, T.r[0]);
	uint16_t link = T.r[0];
	while ((int16_t)link >= 0)
	{
		// cycles of this object's pass at B1FA, except the piece loops (they charge themselves)
		int cycles = 12 + 138;                          // BGE, B200-B214
		T.r[3] = peek(0x0FEE) & 0x001F;
		T.r[2] = peekb((uint16_t)(0x0490 + T.r[3]));
		T.r[5] = list;
		if (peekb((uint16_t)(0xFD80 + list)) & T.r[2])
			cycles += 18;                               // JMP B362
		else
		{
			cycles += 267;                              // B21A-B23E
			ww((uint16_t)(sp - 2), (uint16_t)(list + 0xFD7E));
			T.r[5] = (uint16_t)~peekb((uint16_t)(0xFD7E + list));
			T.r[2] &= (uint16_t)~T.r[5];
			T.r[5] = (uint16_t)(peek(0x0FEE) << 1);
			T.r[4] = peek((uint16_t)(0x0888 + T.r[5]));
			T.r[0] = peekb((uint16_t)(0x04D0 + T.r[3]));
			T.r[5] = (uint16_t)(T.r[3] << 1);
			if (T.r[2])
			{
				cycles += 660;                          // B244-B2A8
				const uint16_t w = T.r[5];
				pokeb((uint16_t)(0x0490 + T.r[3]), (uint8_t)(peekb((uint16_t)(0x0490 + T.r[3])) & ~T.r[2]));
				hw_banks(0, 0x0400);
				T.r[1] = (uint16_t)(peek(peek((uint16_t)(0x03B0 + w))) + 0x4000);
				hw_banks(0x3000, 0x3400);
				// 0x0FF8 = next frame byte * 256 / 4 (+ 0x0FF4 for the piece rows)
				auto next_row = [&]() {
					poke(0x0FF8, 0);
					pokeb(0x0FF9, peekb(T.r[1]++));
					poke(0x0FF8, asr(peek(0x0FF8), 2));
				};
				poke(0x0FF8, 0);
				pokeb(0x0FF9, peekb(T.r[1]++));
				T.r[2] = asr(peek(0x0FF8), 2);
				poke((uint16_t)(0x0530 + w), T.r[2]);
				poke((uint16_t)(0x3804 + T.r[4]), peek((uint16_t)(0x0630 + w)));
				poke((uint16_t)(0x3804 + T.r[4]), (uint16_t)(peek((uint16_t)(0x3804 + T.r[4])) + T.r[2]));
				T.r[2] = sext8(peekb(T.r[1]++));
				poke(0x0FF6, T.r[2]);
				T.r[2] &= 0x0007;
				poke(0x0FF4, peek((uint16_t)(0x0670 + w)));
				poke(0x0FF4, peek(0x0FF4) & ~0x0007);
				poke(0x0FF4, peek(0x0FF4) | T.r[2]);
				poke(0x0FF8, peek(0x0FF4));
				if ((int16_t)peek(0x0FF6) < 0)
				{
					cycles += 153;                      // B2AA-B2BA
					next_row();
					poke(0x0FF8, (uint16_t)(peek(0x0FF8) + peek(0x0FF4)));
				}
				cycles += 99;                           // B2C0-B2CE
				pokeb(0x0FFB, peekb((uint16_t)(0x0470 + T.r[3])));
				T.r[4] = (uint16_t)(T.r[4] + 0x3800);
				poke(0x0FF0, 0);
				T.r[2] = 0;
				for (;;)
				{
					int pass = 36;                      // B2D0-B2D4
					if ((int16_t)peek(0x0FF0) < 0)
					{
						pass += 57;                     // B32A-B32E
						poke((uint16_t)(T.r[4] + 2), 0);
						T.r[2] = peek((uint16_t)(T.r[4] + 6));
					}
					else
					{
						pass += 36;                     // B2D6-B2DA
						if ((int16_t)peek(0x0FF6) >= 0)
						{
							pass += 153;                // B2DC-B2EC
							next_row();
							poke(0x0FF8, (uint16_t)(peek(0x0FF8) + peek(0x0FF4)));
						}
						pass += 162;                    // B2F2-B308
						pokeb(0x0FF2, peekb(T.r[1]++));
						pokeb(0x0FF3, peekb(T.r[1]++));
						poke(0x0FF0, peek(0x0FF2));
						poke(0x0FF2, peek(0x0FF2) & 0x7FFF);
						if (T.r[2])
						{
							pass += 33;                 // B30A
							poke(0x0FF2, peek(0x0FF2) | 0x8000);
						}
						pass += 177;                    // B310-B328
						poke(T.r[4], peek(0x0FF8));
						T.r[4] = (uint16_t)(T.r[4] + 2);
						poke(T.r[4], peek(0x0FF2));
						T.r[4] = (uint16_t)(T.r[4] + 4);
						T.r[2] = (uint16_t)((peek(T.r[4]) & 0x0FFF) | peek(0x0FFA));
						poke(T.r[4], T.r[2]);
					}
					pass += 84;                         // B332-B340
					T.r[5] = T.r[2] & 0x07F8;
					T.r[4] = (uint16_t)(T.r[5] + 0x3800);
					if (--T.r[0] == 0)
					{
						charge(0xB362, pass);
						break;
					}
					charge(0xB2D0, pass + 12);            // BR B2D0
				}
			}
			else
			{
				cycles += 18 + 111;                     // JMP B344, B344-B350
				T.r[2] = (uint16_t)(peek((uint16_t)(0x0630 + T.r[5])) + peek((uint16_t)(0x0530 + T.r[5])));
				poke((uint16_t)(0x3804 + T.r[4]), T.r[2]);
				T.r[2] = peek((uint16_t)(0x05F0 + T.r[5]));
				for (;;)
				{
					poke((uint16_t)(0x3800 + T.r[4]), (uint16_t)(peek((uint16_t)(0x3800 + T.r[4])) + T.r[2]));
					T.r[4] = peek((uint16_t)(0x3806 + T.r[4])) & 0x07F8;
					--T.r[0];
					charge(T.r[0] ? 0xB354 : 0xB362, 30 + 27 + 18 + 18);   // ADD, MOV, BIC, SOB
					if (!T.r[0]) break;
				}
			}
		}
		cycles += 132;                                  // B362-B370
		ww((uint16_t)(sp - 2), (uint16_t)(peek(0x0FEE) + 0x0808));
		T.r[5] = sext8(peekb((uint16_t)(peek(0x0FEE) + 0x0808)));
		poke(0x0FEE, T.r[5]);
		fl::mov(T.r[5]);
		link = T.r[5];
		charge(0xB1FA, cycles);
	}
	cy(12 + 18 + 18 + 21);                              // BGE, JMP F:B374, TST (SP)+, RTS
	fl::tst(pop());
}

// F:B378 (assembly): move the objects 0x0994 down to 0: animation (rate 0x0770 frames per step,
// counter 0x0790, step count 0x07B0; the frame list at 0x03B0 ends with a negative link; with
// flag 0x01 the frame data in bank 0x3000/0x3400 gives the velocity), position += velocity
// for slots from 4 on, then screen position and screen-y delta.  The two stack words it
// reserves end compared, as the original does.
void move_objects()
{
	At here(0xB378);
	cy(30 + 24 + 12);                                   // CMP -(SP),-(SP); MOVB @#0x0994,R5; BGE
	T.r[6] = (uint16_t)(T.r[6] - 4);
	const uint16_t at_rate = T.r[6], at_scratch = (uint16_t)(T.r[6] + 2), at_byte = (uint16_t)(T.r[6] - 2);
	const uint8_t top = peekb(0x0994);
	T.r[5] = sext8(top);
	if (top & 0x80)
		cy(18);                                         // JMP F:B4B2
	else
		for (;;)
		{
			int cycles = 27 + 12;                       // B384 TSTB, BNE
			const uint16_t i = T.r[5];
			if (!peekb((uint16_t)(0x0370 + i)))
				cycles += 18;                           // JMP B4AA
			else
			{
				cycles += 171;                          // B38E-B3A4
				T.r[4] = (uint16_t)(i << 1);
				const uint16_t w = T.r[4];
				T.r[3] = peek((uint16_t)(0x04F0 + w));
				T.r[2] = peek((uint16_t)(0x0570 + w));
				const uint16_t rate_ptr = (uint16_t)(0x0770 + i);
				ww(at_rate, rate_ptr);
				const uint8_t rate = peekb(rate_ptr);
				if (rate) cycles += 12;                 // BMI
				if (rate && !(rate & 0x80))
				{
					cycles += 60;                       // B3A8-B3B0
					T.r[0] = (uint16_t)(0x0790 + i);
					if (peekb(T.r[0]))
					{
						cycles += 21;                   // B45C DECB
						pokeb(T.r[0], (uint8_t)(peekb(T.r[0]) - 1));
					}
					else
					{
						cycles += 147;                  // B3B2-B3C4
						pokeb(T.r[0], peekb(rate_ptr));
						T.r[1] = (uint16_t)(peek((uint16_t)(0x03B0 + w)) + 2);
						pokeb((uint16_t)(0x07B0 + i), (uint8_t)(peekb((uint16_t)(0x07B0 + i)) + 1));
						if ((int16_t)peek(T.r[1]) < 0)
						{
							cycles += 111;              // B3C6-B3D6
							T.r[1] = peek(T.r[1]) & 0x7FFF;
							pokeb((uint16_t)(0x07B0 + i), 0);
							if (peekb((uint16_t)(0x04B0 + i)) & 0x20)
							{
								cycles += 36;           // B3D8
								pokeb(rate_ptr, 0);
							}
						}
						cycles += 45;                   // B3DC-B3E2
						if (peekb((uint16_t)(0x0490 + i)) & 0x01)
						{
							cycles += 102 + 225 + 225 + 30;   // B3E4-B430
							T.r[0] = (uint16_t)(peek(T.r[1]) + 0x3FFD);
							hw_banks(0x3000, 0x3400);
							// velocity bytes * 16 (the original goes through a stack word)
							uint8_t b = peekb(T.r[0]++);
							wb(at_byte, b);
							ww(at_scratch, (uint16_t)(sext8(b) << 2));
							T.r[3] = (uint16_t)(rw(at_scratch) << 2);
							b = peekb(T.r[0]++);
							wb(at_byte, b);
							ww(at_scratch, (uint16_t)(sext8(b) << 2));
							T.r[2] = (uint16_t)(rw(at_scratch) << 2);
							const uint8_t step = peekb(T.r[0]);
							if (step)
							{
								cycles += 171;          // B432-B442
								ww(at_byte, step);
								poke((uint16_t)(0x0430 + w), (uint16_t)(peek((uint16_t)(0x0430 + w)) - step));
								T.r[0] = peekb(rate_ptr);
								poke((uint16_t)(0x0430 + w), (uint16_t)(peek((uint16_t)(0x0430 + w)) + T.r[0]));
							}
							cycles += 60;               // B446-B44A
							hw_banks(0, 0x0400);
						}
						cycles += 78;                   // B450-B45A
						poke((uint16_t)(0x03B0 + w), T.r[1]);
						pokeb((uint16_t)(0x0490 + i), peekb((uint16_t)(0x0490 + i)) | 0x18);
					}
				}
				cycles += 30;                           // B45E-B462
				if ((int16_t)i < 4)
				{
					cycles += 48;                       // B464-B46A
					pokeb((uint16_t)(0x0490 + i), peekb((uint16_t)(0x0490 + i)) | 0x18);
				}
				else
				{
					cycles += 105;                      // B46C-B474
					poke((uint16_t)(0x06F0 + w), (uint16_t)(peek((uint16_t)(0x06F0 + w)) + T.r[3]));
					poke((uint16_t)(0x0730 + w), (uint16_t)(peek((uint16_t)(0x0730 + w)) + T.r[2]));
					poke((uint16_t)(0x06B0 + w), (uint16_t)(peek((uint16_t)(0x06B0 + w)) + peek((uint16_t)(0x05B0 + w))));
				}
				cycles += 306;                          // B47A-B4A6
				T.r[3] = (uint16_t)(peek((uint16_t)(0x06F0 + w)) - peek(0x01A8));
				poke((uint16_t)(0x0630 + w), T.r[3]);
				T.r[2] = (uint16_t)(peek((uint16_t)(0x0730 + w)) + peek(0x01AA) + peek((uint16_t)(0x06B0 + w)));
				T.r[0] = T.r[2] & ~0x0007;
				T.r[1] = peek((uint16_t)(0x0670 + w)) & ~0x0007;
				T.r[0] = (uint16_t)(T.r[0] - T.r[1]);
				poke((uint16_t)(0x05F0 + w), T.r[0]);
				poke((uint16_t)(0x0670 + w), T.r[2]);
			}
			cycles += 24;                               // B4AA DEC, BLT
			T.r[5]--;
			if ((int16_t)T.r[5] < 0)
			{
				charge(0xB4B2, cycles);
				break;
			}
			charge(0xB384, cycles + 18);                  // JMP B384
		}
	cy(24 + 21);
	fl::cmp(rw(at_rate), rw(at_scratch));                // CMP (SP)+,(SP)+
	T.r[6] = (uint16_t)(T.r[6] + 4);
}

// ---------------------------------------------------------------- objects

// F:B4B6  show_object(int slot): make slot (bit 7: without sprite pieces) visible: the highest
// used slot 0x0994 rises to it (or drops to the highest slot still in use), the piece count
// 0x04D0 comes from the type table 0x5C96, the screen position from the world position and the
// scroll; flag 0x80 of 0x0490.  0x0FFC = 0xFF when interrupts are enabled.  Returns slot * 2.
uint16_t show_object(const Args &a)
{
	At here(0xB4B6);
	CFrame frame;
	cy(IN152 + 267);
	frame.save_regs(SAVE_R2_R4, F152_RET);
	const uint16_t saved_a = peek(0x01D2), saved_b = peek(0x01D4);
	set_bank_a(0);
	set_bank_b(0x0400);
	const uint16_t arg = a.u(0), slot = arg & ~0x0080;
	if ((int16_t)slot >= (int16_t)sext8(peekb(0x0994)))
	{
		cy(39);
		pokeb(0x0994, (uint8_t)slot);
	}
	else
	{
		cy(36);
		for (int16_t i = (int16_t)sext8(peekb(0x0994)); i >= 0; i--)
		{
			cy(39);
			bool used = peekb((uint16_t)(0x0370 + i)) != 0;
			if (!used)
			{
				cy(45);
				used = (peekb((uint16_t)(0x0490 + i)) & 0x86) != 0;
			}
			if (used)
			{
				cy(39);
				pokeb(0x0994, (uint8_t)i);
				break;
			}
			cy(24);
		}
	}
	cy(39);
	if (arg == slot)
	{
		cy(96);
		pokeb((uint16_t)(0x04D0 + slot), peekb((uint16_t)(0x5C96 + peekb((uint16_t)(0x0370 + slot)))));
	}
	else
	{
		cy(30);
		pokeb((uint16_t)(0x04D0 + slot), 0);
	}
	cy(306);
	const uint16_t w = (uint16_t)(slot << 1);
	poke((uint16_t)(0x0630 + w), (uint16_t)(peek((uint16_t)(0x06F0 + w)) - peek(0x01A8)));
	const uint16_t sy = peek(0x01AA), y = peek((uint16_t)(0x0730 + w));
	poke((uint16_t)(0x0670 + w), (uint16_t)(sy + y));
	set_c(carry(sy, y));
	poke((uint16_t)(0x05F0 + w), 0);
	pokeb((uint16_t)(0x0490 + slot), peekb((uint16_t)(0x0490 + slot)) | 0x80);
	if (!(T.psw & 0xE0))
	{
		cy(33);
		pokeb(0x0FFC, 0xFF);
	}
	cy(153);
	restore_banks(saved_a, saved_b);
	return w;
}

// F:B602  queue_piece_count(int type, int slot): with interrupts off, put slot and its piece
// count (type table 0x5C96) in the first free entry of the queue F:B03C applies (0x0989+i,
// i = 4..1), flag the slot (0x60) and count the entry (0x0992).  Interrupts end enabled.
uint16_t queue_piece_count(const Args &a)
{
	At here(0xB602);
	CFrame frame;
	cy(IN162 + 24 + 24 + 27 + 27 + 18 + 27 + 27 + 18);  // up to MOV #0xE0,R3
	frame.save_regs(SAVE_R2_R5, F162_RET);
	const uint16_t saved_a = peek(0x01D2), saved_b = peek(0x01D4);
	set_bank_a(0);
	set_bank_b(0x0400);
	io(24, [] { mtps(0xE0); });                         // interrupts off
	cy(18);
	uint16_t r0 = 0x0400;
	for (uint16_t i = 4;;)
	{
		cy(27 + 12);                                    // TSTB 0x989(R2); BNE
		if (!peekb((uint16_t)(0x0989 + i)))
		{
			cy(27 + 30 + 27 + 45 + 36 + 27 + 12);       // F:B632-B64E
			r0 = a.u(1);
			pokeb((uint16_t)(0x0989 + i), (uint8_t)r0);
			pokeb((uint16_t)(0x098D + i), peekb((uint16_t)(0x5C96 + a.u(0))));
			pokeb((uint16_t)(0x0490 + r0), peekb((uint16_t)(0x0490 + r0)) | 0x60);
			pokeb(0x0992, (uint8_t)(peekb(0x0992) + 1));
			break;
		}
		cy(18);                                         // SOB
		if (!--i) break;
	}
	cy(12);
	io(30, [] { mtps(0); });                            // and on again
	cy(27 + 27 + 39 + 39 + 21);                         // F:B658-B66C
	restore_banks(saved_a, saved_b);
	return r0;
}

// F:BA2E: with interrupts off, clear the text layer (F:8D3E) and the object tables
// 0x0370-0x0994, park the last sprite (0x3BFE = 0x03F8) and empty both display lists
// (0x0988/0x0989 = 0xFF).  Interrupts end enabled.  Returns 0x0995.
uint16_t clear_objects_and_text(const Args &)
{
	At here(0xBA2E);
	CFrame frame;
	cy(24 + 18);
	push(T.r[1]);
	T.r[1] = 0x00E0;
	io(24, [] { mtps(0xE0); });                         // interrupts off
	cy(30);
	jsr(0x8068, 0xBA3A);
	cy(30);
	jsr(0x8D3E, 0xBA3E);
	cy(18);
	for (T.r[0] = 0x0370; T.r[0] != 0x0995;)
	{
		pokeb(T.r[0]++, 0);
		charge(T.r[0] != 0x0995 ? 0xBA42 : 0xBA4A, 21 + 18 + 12);     // CLRB (R0)+, CMP, BNE
	}
	cy(33 + 33);
	poke(0x3BFE, 0x03F8);
	poke(0x0988, 0xFFFF);
	io(30, [] { mtps(0); });                            // and on again
	cy(18 + 21);
	return 0x0995;
}

// F:C98E  restart_object_animation(int rate, int n): the object of map object n (0x09B8, the
// argument is replaced by the slot) restarts its animation: next frame of the list 0x03B0 (a
// negative link is followed), redraw flags 0x18, rate and counter (0x0770, 0x0790) = rate.
// Not for the object queued in 0x0E6A.  Returns the slot.
uint16_t restart_object_animation(const Args &a)
{
	At here(0xC98E);
	CFrame frame;
	cy(IN152 + 369);                                    // F:C992-C9CA
	frame.save_regs(SAVE_R2_R4, F152_RET);
	const uint16_t saved_a = peek(0x01D2), saved_b = peek(0x01D4);
	set_bank_a(0);
	set_bank_b(0x0400);
	const uint16_t n = a.u(1), at = (uint16_t)(a.sp + 4);
	pokeb(at, peekb((uint16_t)(0x09B8 + n)));
	poke(at, peek(at) & 0x003F);
	const uint16_t slot = peek(at);
	bool c = false;
	if (peekb(0x0E6A) != slot)
	{
		cy(12 + 12 + 18 + 27 + 33 + 12);                // F:C9CC-C9DC
		const uint16_t p = (uint16_t)(0x03B0 + 2 * slot), v = peek(p);
		poke(p, (uint16_t)(v + 2));
		c = carry(v, 2);
		const uint16_t f = peek(peek(p));
		if ((int16_t)f < 0)
		{
			cy(21 + 27);
			poke(p, f);
			poke(p, peek(p) & 0x7FFF);
		}
		cy(36 + 27 + 30 + 30);                          // F:C9E4-C9F2
		pokeb((uint16_t)(0x0490 + slot), peekb((uint16_t)(0x0490 + slot)) | 0x18);
		const uint8_t rate = (uint8_t)a.u(0);
		pokeb((uint16_t)(0x0790 + slot), rate);
		pokeb((uint16_t)(0x0770 + slot), rate);
	}
	cy(27 + 27 + 39 + 39 + 21);                         // F:C9F6-CA0A
	restore_banks(saved_a, saved_b);
	set_c(c);
	return slot;
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
uint16_t locate_map_object(const Args &)
{
	At here(0xBE10);
	CFrame frame;
	cy(IN144 + 177);
	frame.save_regs(SAVE_R2_R3, F144_RET);
	const uint16_t n = peek(0x09AA);
	const uint16_t d = peek((uint16_t)(0x0AF8 + 2 * n)) & 0x7FFF;
	const bool dynamic = (int16_t)n >= 0x40;
	cy(dynamic ? 96 : 18);
	cy(114);
	const uint16_t entry = (uint16_t)((dynamic ? 0x7D00 : 0x7A00) + ((d & 0x01FF) << 1));
	const uint16_t def = (uint16_t)(field9(d) + peek(entry));
	poke(0x09B6, def);
	if (dynamic)
	{
		cy(81);
		poke(0x09B6, (uint16_t)(def + 0x0338));
		set_c(carry(def, 0x0338));
	}
	else
	{
		cy(129);
		const uint16_t rec = (uint16_t)(def << 3);
		poke(0x09B2, (uint16_t)(rec + 0x6040));
		set_c(carry(rec, 0x6040));
	}
	return entry;
}

// F:BE7C: state of the map object whose descriptor 0x09B2 points to: negative when its cell has
// no definition; otherwise (definition recorded in 0x09B4) for a definition with a group
// (properties bits 0-3) 1 when another map object holds the group (0x0E6D+group), else 0; for
// the others the definition's bit in the bitmap 0x0CF6 (taken).
uint16_t map_object_state(const Args &)
{
	At here(0xBE7C);
	CFrame frame;
	cy(IN144 + 129);
	frame.save_regs(SAVE_R2_R3, F144_RET);
	const uint16_t d = peek(peek(0x09B2));
	const bool dynamic = (int16_t)peek(0x09AA) >= 0x40;
	cy(dynamic ? 96 : 18);
	cy(30);
	const uint16_t cell = peek((uint16_t)((dynamic ? 0x7D00 : 0x7A00) + ((d & 0x01FF) << 1)));
	if ((int16_t)cell < 0)
	{
		cy(33);
		set_c(false);
		return cell;
	}
	cy(108);
	uint16_t def = (uint16_t)(field9(d) + cell);
	if (dynamic)
	{
		cy(18);
		def = (uint16_t)(def + 0x0338);
	}
	cy(72);
	poke(0x09B4, def);
	const uint8_t prop = peekb((uint16_t)(0x4560 + def));
	uint16_t r;
	if (prop & 0x0F)
	{
		cy(108);
		const uint16_t holder = peekb((uint16_t)(0x0E6D + (prop & 0x0F)));
		const uint16_t self = (uint16_t)(peek(0x09AA) + 1);
		if (holder) cy(60);
		if (!holder || holder == self)
		{
			cy(24);
			r = 0;
			set_c(false);
		}
		else
		{
			cy(30);
			r = 1;
			set_c(holder < self);
		}
	}
	else
	{
		cy(162);
		const uint16_t mask = peekb((uint16_t)(0xFC74 + (def & 7)));
		r = peekb((uint16_t)(0x0CF6 + asr(def, 3))) & mask;
		set_c(true);                                    // COM
	}
	cy(33);
	return r;
}

// F:BF5E  map_object_position(int *x, int *y): screen position of the current map object's
// definition (tables 0x5580 / 0x4AC0) adjusted by the cell's column (0xFDAA) and row (0xFDB2,
// with the scroll row 0x0E9B, one more right of column 0x099C).  Returns the column offset index.
uint16_t map_object_position(const Args &a)
{
	At here(0xBF5E);
	CFrame frame;
	cy(IN144 + 24 + 12 + 27 + 18 + 24 + 24 + 12 + 18 + 12 + 12);   // F:BF62-BF80
	frame.save_regs(SAVE_R2_R3, F144_RET);
	const uint16_t cell = peek((uint16_t)(0x0AF8 + 2 * peek(0x09AA))) & 0x01FF;
	uint16_t row = sext8(peekb(0x0E9B));
	if ((cell & 0x000F) > peek(0x099C))
	{
		cy(12);
		row++;
	}
	cy(369);                                            // F:BF84-BFBA
	const uint16_t ri = (uint16_t)((((uint16_t)(asr(cell, 4) + row)) & 3) << 1);
	const uint16_t def2 = (uint16_t)(peek(0x09B6) << 1);
	poke(a.u(1), (uint16_t)(peek((uint16_t)(0xFDB2 + ri)) + peek((uint16_t)(0x4AC0 + def2))));
	const uint16_t ci = (uint16_t)((cell & 3) << 1);
	const uint16_t cx = peek((uint16_t)(0xFDAA + ci)), dx = peek((uint16_t)(0x5580 + def2));
	set_c(carry(cx, dx));
	poke(a.u(0), (uint16_t)(cx + dx));
	return ci;
}

// F:BD78 (assembly): the current map object (0x09AA) comes into view: when its cell lies in
// the window 0x0224-0x0226 (columns) x 0x0228-0x022A (rows) and, on the window's edges, its
// definition's position (0x5580 / 0x4AC0) is past the scroll fraction (0x09A6 / 0x09A4), the
// object is created (F:C4E4)
void map_object_enters()
{
	At here(0xBD78);
	cy(159);
	T.r[0] = (uint16_t)(peek(0x09AA) << 1);
	T.r[4] = peek((uint16_t)(0x0AF8 + T.r[0]));
	T.r[2] = T.r[4] & 0x01FF;
	T.r[1] = T.r[2] & 0x000F;
	fl::cmp(T.r[1], peek(0x0224));
	if (lt()) return cy(21);
	cy(36);
	fl::cmp(T.r[1], peek(0x0226));
	if (gt()) return cy(21);
	cy(66);
	T.r[0] = T.r[2] & 0x01F0;
	fl::cmp(T.r[0], peek(0x0228));
	if (lt()) return cy(21);
	cy(36);
	fl::cmp(T.r[0], peek(0x022A));
	if (gt()) return cy(21);
	cy(165);
	T.r[4] &= 0x7FFF;
	T.r[2] = (uint16_t)(T.r[2] << 1);
	T.r[3] = (uint16_t)(field9(T.r[4]) + peek((uint16_t)(0x7A00 + T.r[2])));
	T.r[4] = T.r[3];
	auto pos = [&](uint16_t table) {
		T.r[2] = (uint16_t)(T.r[4] << 1);
		return peek((uint16_t)(table + T.r[2]));
	};
	fl::cmp(T.r[1], peek(0x0224));
	if (!gt())
	{
		cy(75);
		fl::cmp(pos(0x5580), peek(0x09A6));
		if (!fC()) return cy(21);
	}
	cy(36);
	fl::cmp(T.r[1], peek(0x0226));
	if (!lt())
	{
		cy(75);
		fl::cmp(pos(0x5580), peek(0x09A6));
		if (fC()) return cy(21);
	}
	cy(36);
	fl::cmp(T.r[0], peek(0x022A));
	if (!lt())
	{
		cy(75);
		fl::cmp(pos(0x4AC0), peek(0x09A4));
		if (!fC()) return cy(21);
	}
	cy(36);
	fl::cmp(T.r[0], peek(0x0228));
	if (!gt())
	{
		cy(75);
		fl::cmp(pos(0x4AC0), peek(0x09A4));
		if (fC()) return cy(21);
	}
	cy(30);
	jsr(0xC4E4, 0xBE0E);
	cy(21);
}

// F:BF18: release the current map object's definition: its group (properties bits 0-3) is
// free again (0x0E6D+group = 0) and its bit in the bitmap 0x0CF6 is cleared.  Banks end at
// 0x1000 / 0x1400.  Returns the bitmap byte index.
uint16_t release_map_definition(const Args &)
{
	At here(0xBF18);
	CFrame frame;
	cy(24 + 18 + 27 + 27 + 18 + 27 + 27 + 30);          // F:BF18-BF32
	push(T.r[1]);
	T.r[0] = 0x1000;
	set_bank_a(0x1000);
	T.r[0] = 0x1400;
	set_bank_b(0x1400);
	jsr(0xBE10, 0xBF36);
	cy(24 + 27 + 18 + 12);                              // F:BF36-BF42
	const uint16_t def = peek(0x09B6);
	const uint16_t group = peekb((uint16_t)(0x4560 + def)) & 0x0F;
	if (group)
	{
		cy(30);
		pokeb((uint16_t)(0x0E6D + group), 0);
	}
	cy(12 * 4 + 18 + 45 + 18 + 21);                     // F:BF48-BF5C
	const uint16_t i = asr(def, 3);
	pokeb((uint16_t)(0x0CF6 + i), (uint8_t)(peekb((uint16_t)(0x0CF6 + i)) & ~peekb((uint16_t)(0xFC74 + (def & 7)))));
	set_c((def >> 2) & 1);                              // the last ASR
	return i;
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
	At here(0xC114);
	CFrame frame;
	cy(IN162 + 600);                                    // F:C118-C170
	frame.save_regs(SAVE_R2_R5, F162_RET);
	T.r[6] = (uint16_t)(T.r[6] - 0x0C);                 // six locals
	uint16_t *R = T.r;
	R[0] = peekb((uint16_t)(peek(0x09B2) + 2));          // the direction
	poke(0x01D2, 0);
	poke(0x1400, 0);
	R[1] = 0x0400;
	set_bank_b(0x0400);
	R[1] = peek(0x09AA);
	R[3] = sext8(peekb((uint16_t)(0x09B8 + R[1]))) & 0x003F;   // slot
	R[4] = peekb((uint16_t)(0x0370 + R[3]));                    // type
	push(0);                                                    // turned
	push(R[4]);
	const uint16_t sp = R[6];
	ww(sp, (uint16_t)(R[4] << 1));
	const uint16_t TW = sp, TURNED = (uint16_t)(sp + 2), L0 = (uint16_t)(sp + 4), L2 = (uint16_t)(sp + 6),
		L4 = (uint16_t)(sp + 8), L6 = (uint16_t)(sp + 0x0A), L8 = (uint16_t)(sp + 0x0C), LA = (uint16_t)(sp + 0x0E);
	R[1] = peekb((uint16_t)(0xFDBA + R[0]));
	R[2] = rw(TW);
	R[1] = (uint16_t)(R[1] + peek((uint16_t)(0x5F7C + R[2])));
	R[1] = peek(R[1]);                                          // target frame
	R[2] = sext8(peekb((uint16_t)(0x07D0 + R[3])));
	ww(L2, R[2]);                                               // current direction
	R[2] = (uint16_t)(R[0] - rw(L2));
	if (R[2])
	{
		cy(12);
		if ((int16_t)R[2] < 0)
		{
			cy(60);
			R[2] = (uint16_t)-R[2];
			ww(L6, 0xFFFE);
		}
		else
		{
			cy(36);
			ww(L6, 0x0002);
		}
		cy(30);
		if ((int16_t)R[2] > 8)
		{
			cy(30);
			ww(L6, (uint16_t)-rw(L6));                          // the short way round
		}
		cy(117);
		ww(L4, (uint16_t)(0x04B0 + R[3]));
		if (!(peekb(rw(L4)) & 0x01))
		{
			cy(213);
			R[5] = 0;
			ww((uint16_t)(sp - 2), (uint16_t)(rw(L2) + 0xFDBA));
			R[5] = peekb(rw((uint16_t)(sp - 2)));
			ww((uint16_t)(sp - 2), (uint16_t)(rw(TW) + 0x5F7C));
			R[5] = (uint16_t)(R[5] + peek(rw((uint16_t)(sp - 2))));
			R[2] = peek(R[5]);
		}
		else
		{
			cy(51);
			R[5] = (uint16_t)(R[3] << 1);
			R[2] = peek((uint16_t)(0x03B0 + R[5]));
		}
		cy(57);
		R[2] = (uint16_t)(R[2] + rw(L6));
		if ((int16_t)peek(R[2]) < 0)
		{
			cy(36);
			R[2] = peek(R[2]) & 0x7FFF;
		}
		cy(63);
		R[5] = R[0] | rw(L2);
		const bool odd = (R[5] & 1) != 0;
		R[5] = (uint16_t)(R[5] >> 1);                           // ROR, carry clear
		if (odd)
		{
			cy(12);
			R[2] = R[1];
		}
		cy(24);
		if (R[2] == R[1])
		{
			cy(78);
			R[5] = 0xFF01;
			pokeb(rw(L4), peekb(rw(L4)) & ~0x01);
		}
		else
		{
			cy(42);
			R[1] = R[2];
			ww(TURNED, (uint16_t)(rw(TURNED) + 1));
		}
		cy(39);
		if (peekb((uint16_t)(0x6502 + R[4])))
		{
			cy(39);
			if (!rw(TURNED))
			{
				cy(18);
				R[1] = peek(R[1]);
			}
			else
			{
				cy(51);
				if (peekb(rw(L4)) & 0x01)
				{
					cy(57);
					R[1] = (uint16_t)(R[1] + rw(L6));
					if ((int16_t)peek(R[1]) < 0)
					{
						cy(48);
						R[1] = peek(R[1]) & 0x7FFF;
					}
				}
			}
		}
		cy(63);
		R[5] = (uint16_t)(R[3] << 1);
		if (R[1] != peek((uint16_t)(0x03B0 + R[5])))
		{
			cy(159);
			poke((uint16_t)(0x03B0 + R[5]), R[1]);
			pokeb((uint16_t)(0x0490 + R[3]), peekb((uint16_t)(0x0490 + R[3])) | 0x18);
			ww((uint16_t)(sp - 2), (uint16_t)(rw(TW) + 0x6110));
			R[2] = peek(rw((uint16_t)(sp - 2)));
			if (R[2])
			{
				cy(156);
				ww(L6, (uint16_t)(peekb((uint16_t)(0xFDBA + R[0])) + R[2]));
				poke((uint16_t)(0x03F0 + R[5]), peek(rw(L6)));
			}
		}
	}
	cy(327);                                                    // F:C248-C278
	ww(L2, (uint16_t)(0x0770 + R[3]));
	R[5] = (uint16_t)(R[3] << 1);
	ww(L4, (uint16_t)(0x04F0 + R[5]));
	ww(L6, (uint16_t)(0x0570 + R[5]));
	ww(L0, (uint16_t)(0x0430 + R[5]));
	if (!rw(TURNED))
	{
		cy(57);
		R[2] = sext8(peekb((uint16_t)(0x649D + R[4]))) & 1;    // stands still
		if (R[2])
		{
			cy(30);
			if (R[0] & 1)
			{
				cy(48);
				R[2] = 0;
				pokeb((uint16_t)(0x0490 + R[3]), peekb((uint16_t)(0x0490 + R[3])) & ~0x01);
			}
		}
		cy(531);
		R[4] = sext8(peekb((uint16_t)(0x65CC + R[4])));
		ww((uint16_t)(sp - 2), peekb(rw(L2)) & 0x7F);
		pokeb(rw(L2), (uint8_t)(peekb(rw(L2)) & 0x7F));
		ww(TURNED, 0x1000);
		set_bank_a(0x1000);
		ww(TURNED, 0x1400);
		set_bank_b(0x1400);
		ww(TURNED, peek(0x09B2));
		R[5] = rw(TURNED);
		R[1] = peekb((uint16_t)(R[5] + 3));                    // speed
		R[5] = 0x2400;
		set_bank_a(0x2400);
		if (!R[2])
		{
			cy(288);
			R[5] = (uint16_t)((R[0] << 3) + R[4]);
			R[2] = (uint16_t)(R[5] << 1);
			poke(rw(L4), peek((uint16_t)(0x4138 + R[2])));
			poke(rw(L6), peek((uint16_t)(0x4238 + R[2])));
			if (R[1] == 0x18)
			{
				cy(63);
				pokeb(rw(L0), peekb((uint16_t)(0xFD8A + R[4])));
			}
			else
			{
				cy(99);
				R[1] = (uint16_t)((R[1] << 3) + R[4]);
				pokeb(rw(L0), peekb((uint16_t)(0x3FF8 + R[1])));
			}
			cy(54);
			poke(rw(L0), peek(rw(L0)) & 0x00FF);
		}
		else
		{
			cy(243);
			R[1] = (uint16_t)(R[1] << 1);
			poke(rw(L0), (uint16_t)(peek(rw(L0)) + peek((uint16_t)(0x40F8 + R[1]))));
			poke(rw(L4), 0);
			poke(rw(L6), 0);
			pokeb((uint16_t)(0x0490 + R[3]), peekb((uint16_t)(0x0490 + R[3])) | 0x01);
		}
		cy(54);
		pokeb((uint16_t)(0x07D0 + R[3]), (uint8_t)R[0]);
		R[0] = 0;
	}
	else
	{
		cy(276);
		R[0] = 0x1000;
		set_bank_a(0x1000);
		R[0] = 0x1400;
		set_bank_b(0x1400);
		push(L8);
		push(LA);
		jsr(0xBF5E, 0xC37A);
		cy(681);
		R[0] = 0x0400;
		set_bank_a(0);
		set_bank_b(0x0400);
		R[0] = rw(TW);
		R[2] = peek((uint16_t)(0x62A4 + R[0]));
		R[1] = peek((uint16_t)(0x636E + R[0]));
		R[0] = (uint16_t)(R[2] - peek((uint16_t)(0x06F0 + R[5])) + rw(L8));
		R[2] = asr(R[0], 4);
		poke(rw(L4), R[2]);
		R[1] = (uint16_t)(R[1] - peek((uint16_t)(0x0730 + R[5])) + rw(LA));
		R[2] = asr(R[1], 4);
		poke(rw(L6), R[2]);
		poke(rw(L0), 6);
		pokeb(rw(L2), peekb(rw(L2)) | 0x80);
		pokeb((uint16_t)(0x04B0 + R[3]), peekb((uint16_t)(0x04B0 + R[3])) | 0x01);
		R[0] = 1;
	}
	cy(39);
	set_c(carry(sp, 0x10));                             // ADD #0x10,SP
	return R[0];
}

// F:BFBC: the current map object moves to the definition F:BE7C found (0x09B4) once its object
// is in place (F:C0D8 for map objects without an object, F:C114 turning for the others; non-zero
// means not yet): the old definition (0x09B6) is released (group 0x0E6D or bitmap 0x0CF6), the
// object's palette bit 7 (0x0470) follows the new definition's bit 5, the new definition is
// taken and the descriptor becomes the record's word (bit 15 kept).  Banks end at 0x1000 /
// 0x1400.  Returns the descriptor's bit 15 (0x1400 when not moved).
uint16_t move_map_object(const Args &)
{
	At here(0xBFBC);
	CFrame frame;
	cy(IN162 + 96);                                     // F:BFC0-BFCC
	frame.save_regs(SAVE_R2_R5, F162_RET);
	T.r[6] = (uint16_t)(T.r[6] - 2);
	const uint16_t at_desc = T.r[6];
	T.r[0] = (uint16_t)(peek(0x09AA) << 1);
	if ((int16_t)peek((uint16_t)(0x0AF8 + T.r[0])) > 0)
	{
		cy(30);
		jsr(0xC0D8, 0xBFD2);
		cy(12);
	}
	else
	{
		cy(30);
		jsr(0xC114, 0xBFD8);
	}
	cy(24);
	const uint16_t r = T.r[0];
	set_bank_a(0x1000);
	set_bank_b(0x1400);
	set_c(false);                                       // TST (SP)+
	if (r)
	{
		cy(144 + 39);
		return 0x1400;
	}
	cy(213);
	const uint16_t old = peek(0x09B6);
	const uint8_t old_prop = peekb((uint16_t)(0x4560 + old));
	if (old_prop & 0x0F)
	{
		cy(114);
		if (!(peekb((uint16_t)(0x4560 + peek(0x09B4))) & 0x0F))
		{
			cy(42);
			pokeb((uint16_t)(0x0E6D + (old_prop & 0x0F)), 0);
		}
	}
	else
	{
		cy(141);
		const uint16_t i = asr(old, 3);
		pokeb((uint16_t)(0x0CF6 + i), (uint8_t)(peekb((uint16_t)(0x0CF6 + i)) & ~peekb((uint16_t)(0xFC74 + (old & 7)))));
	}
	cy(120);
	const uint16_t def = peek(0x09B4);
	const uint16_t w = (uint16_t)(peek(0x09AA) << 1);
	ww(at_desc, peek((uint16_t)(0x0AF8 + w)));
	const uint16_t desc = rw(at_desc);
	if ((int16_t)desc < 0)
	{
		cy(144 + 30);
		const uint16_t slot = sext8(peekb((uint16_t)(0x09B8 + peek(0x09AA)))) & 0x003F;
		const uint16_t pal = (uint16_t)(0x0470 + slot);
		const bool want = (peekb((uint16_t)(0x4560 + def)) & 0x20) != 0, has = (peekb(pal) & 0x80) != 0;
		if (want != has)
		{
			cy((want ? 39 : 27) + 36);
			pokeb(pal, want ? (uint8_t)(peekb(pal) | 0x80) : (uint8_t)(peekb(pal) & ~0x80));
			pokeb((uint16_t)(0x0490 + slot), peekb((uint16_t)(0x0490 + slot)) | 0x18);
		}
	}
	cy(45);
	const uint8_t prop = peekb((uint16_t)(0x4560 + def));
	if (prop & 0x0F)
	{
		cy(123);
		pokeb((uint16_t)(0x0E6D + (prop & 0x0F)), (uint8_t)(peek(0x09AA) + 1));
	}
	else
	{
		cy(135);
		const uint16_t i = asr(def, 3);
		pokeb((uint16_t)(0x0CF6 + i), peekb((uint16_t)(0x0CF6 + i)) | peekb((uint16_t)(0xFC74 + (def & 7))));
	}
	cy(141 + 39);
	const uint16_t word = peek(peek(0x09B2)), keep = desc & 0x8000;
	poke((uint16_t)(0x0AF8 + w), word);
	poke((uint16_t)(0x0AF8 + w), peek((uint16_t)(0x0AF8 + w)) | keep);
	return keep;
}

// F:CF5E  remove_map_object(int n): map object n goes: its object slot is reset (F:B56E) if it
// has one, its definition released (F:BF18); a fixed one (n < 0x40) clears its timer 0x0AB8,
// a dynamic one counts the event of its definition (F:D348, properties 0x4000 bits 0-3).
// Descriptor and slot entry are cleared.  Returns the definition's properties byte (0x4000).
uint16_t remove_map_object(const Args &a)
{
	At here(0xCF5E);
	CFrame frame;
	cy(156);                                            // F:CF5E-CF72
	push(T.r[1]);
	uint16_t *R = T.r;
	poke(0x09AA, a.u(0));
	R[1] = a.u(0);
	R[0] = (uint16_t)(R[1] << 1);
	if ((int16_t)peek((uint16_t)(0x0AF8 + R[0])) < 0)
	{
		R[0] = sext8(peekb((uint16_t)(0x09B8 + R[1]))) & 0x003F;
		R[1] = peekb((uint16_t)(0x0370 + R[0]));
		push(R[0]);
		cy(138);
		jsr(0xB56E, 0xCF88);
		cy(30);
		R[6] = (uint16_t)(R[6] + 2);
	}
	else
	{
		cy(45);
		R[1] = sext8(peekb((uint16_t)(0x09B8 + R[1]))) & 0x007F;
	}
	cy(30);
	jsr(0xBF18, 0xCF98);
	cy(54);
	R[0] = peek(0x09AA);
	if ((int16_t)R[0] < 0x40)
	{
		cy(42);
		pokeb((uint16_t)(0x0AB8 + R[0]), 0);
	}
	else
	{
		R[0] = peek(0x09B6);
		push(peekb((uint16_t)(0x4000 + R[0])) & 0x0F);
		cy(120);
		jsr(0xD348, 0xCFB8);
		cy(18);
		R[6] = (uint16_t)(R[6] + 2);
	}
	cy(198);
	const uint16_t k = peek(0x09AA);
	poke((uint16_t)(0x0AF8 + 2 * k), 0);
	pokeb((uint16_t)(0x09B8 + k), 0);
	set_c(false);
	return sext8(peekb((uint16_t)(0x4000 + peek(0x09B6))));
}

// F:CE24  add_map_object(int dynamic, int descriptor, int type): enter a map object in the
// first free entry (fixed ones from 0, dynamic ones from 0x0E98 = 0x40 on) unless its cell's
// definition is taken (F:BE7C, returns -1).  Fixed objects of types without 0x649D bit 7 get
// the timer 0x0AB8 = 0x0E9A + 8; definitions of types without 0x6438 bit 1 are marked taken.
// The highest entry in use (0x09AE fixed / 0x09B0 dynamic) is updated.  Returns 0.  0x09B2 is
// left pointing at the descriptor argument on the stack, as in the original.
uint16_t add_map_object(const Args &a)
{
	At here(0xCE24);
	CFrame frame;
	cy(IN162 + 372);                                    // F:CE28-CE60
	frame.save_regs(SAVE_R2_R5, F162_RET);
	uint16_t *R = T.r;
	push(peek(0x01D2));
	push(peek(0x01D4));
	const uint16_t sp = R[6];
	const uint16_t desc = a.u(1), type = a.u(2);
	R[2] = desc & 0x01FF;
	R[0] = 0x1000;
	set_bank_a(0x1000);
	R[0] = 0x1400;
	set_bank_b(0x1400);
	poke(0x09B2, (uint16_t)(sp + 0x14));                // the descriptor argument
	R[5] = 0;
	if (a.u(0) == 0)
	{
		cy(81);
		R[5] = 1;
		R[2] = 0;
		R[1] = 0x09AE;
		poke(0x09AA, 0);
	}
	else
	{
		cy(87);
		R[2] = peekb(0x0E98);
		R[1] = 0x09B0;
		poke(0x09AA, 0x0040);
	}
	const bool fixed = R[5] != 0;
	const uint16_t top = R[1];
	uint16_t r0;
	cy(30);
	const uint16_t state = jsr(0xBE7C, 0xCE84);
	cy(24);
	if (state != 0)
	{
		cy(183 + 45);
		restore_banks(rw((uint16_t)(sp + 2)), rw(sp));
		r0 = 0xFFFF;
	}
	else
	{
		uint16_t i = R[2];
		for (;;)
		{
			cy(39);
			if (!peekb((uint16_t)(0x09B8 + i))) break;
			cy(24);
			i++;
		}
		R[2] = i;
		R[0] = (uint16_t)(i << 1);
		poke((uint16_t)(0x0AF8 + R[0]), desc);
		poke(0x09AA, i);
		cy(126);
		jsr(0xBE10, 0xCEC0);
		cy(270);
		const uint16_t def = peek(0x09B6), byte = asr(def, 3);
		set_bank_a(0);
		set_bank_b(0x0400);
		if (!(peekb((uint16_t)(0x6438 + type)) & 0x02))
		{
			cy(63);
			pokeb((uint16_t)(0x0CF6 + byte), peekb((uint16_t)(0x0CF6 + byte)) | peekb((uint16_t)(0xFC74 + (def & 7))));
		}
		cy(24);
		if (fixed)
		{
			cy(39);
			if (!(peekb((uint16_t)(0x649D + type)) & 0x80))
			{
				cy(84);
				pokeb((uint16_t)(0x0AB8 + i), (uint8_t)(peekb(0x0E9A) + 8));
			}
		}
		cy(213);
		pokeb((uint16_t)(0x09B8 + i), (uint8_t)type);
		set_bank_a(0);
		set_bank_b(0x0400);
		poke(0x09AA, i);
		if ((int16_t)i <= (int16_t)peek(top))
		{
			cy(18);
			i = peek(top);
			for (;;)
			{
				cy(39);
				if (peekb((uint16_t)(0x09B8 + i))) break;
				cy(24);
				i--;
			}
		}
		cy(219 + 45);
		poke(top, i);
		pokeb(0x0E99, 1);
		restore_banks(rw((uint16_t)(sp + 2)), rw(sp));
		r0 = 0;
	}
	set_c(rw(sp) < rw((uint16_t)(sp + 2)));             // CMP (SP)+,(SP)+ of the saved banks
	return r0;
}

// F:CFD6: clear the map object tables (0x09AA-0x0E9A), set the empty ranges (0x09AE = -1,
// 0x09B0 = 0x3F, next dynamic entry 0x0E98 = 0x40) and add the level's fixed map objects from
// the list at 0x6696 (bank B 0x0400; descriptor, type; ends with 0xFFFF).  Banks are restored.
// Returns the last F:CE24 result.
uint16_t init_map_objects(const Args &)
{
	At here(0xCFD6);
	CFrame frame;
	cy(IN144 + 24 + 24 + 18 + 12 + 18 + 12);            // F:CFDA-CFE6, the first CMP / BLOS
	frame.save_regs(SAVE_R2_R3, F144_RET);
	uint16_t *R = T.r;
	R[2] = peek(0x01D2);
	R[3] = peek(0x01D4);
	for (R[0] = 0x09AA; R[0] <= 0x0E9A;)
	{
		pokeb(R[0]++, 0);
		charge(R[0] <= 0x0E9A ? 0xCFE8 : 0xCFF0, 21 + 18 + 12);       // CLRB (R0)+, CMP, BLOS
	}
	cy(27 + 33 + 33 + 33 + 27 + 27 + 18 + 27 + 27 + 18);   // F:CFF0-D01A
	pokeb(0x0E93, 0);
	poke(0x09AE, 0xFFFF);
	poke(0x09B0, 0x003F);
	pokeb(0x0E98, 0x40);
	poke(0x01D2, 0);
	poke(0x1400, 0);
	R[0] = 0x0400;
	set_bank_b(0x0400);
	for (R[1] = 0x6696;;)
	{
		charge(0xD01E, 24 + 12);                        // CMP (R1),#-1; BEQ
		if (peek(R[1]) == 0xFFFF) break;
		cy(30 + 39 + 30 + 30);                          // F:D024-D02E
		push(peek(R[1]));
		push(peek((uint16_t)(R[1] + 2)));
		push(1);
		jsr(0xCE24, 0xD032);
		cy(18 + 18 + 12);                               // F:D032-D03A
		R[1] = (uint16_t)(R[1] + 4);
		R[6] = (uint16_t)(R[6] + 6);
	}
	cy(27 + 27 + 39 + 39 + 21);                         // F:D03C-D050
	restore_banks(R[2], R[3]);
	set_c(false);
	return R[0];
}

// F:C3E6: the object of the current map object goes off screen: type 4 stays; types counted
// in 0x0E97 / 0x0E96 (0x649D bits 6 / 4) count up; objects flagged 0x10 (0x04B0) remove their
// map object for good (F:CF5E); the others are reset (slot 5 also resets slot 6), the map
// object keeps its type in 0x09B8 and loses its slot flag; fixed map objects get a timer from
// the animation counter (unless 0x649D bit 7) and A5:5D08 is told the type.  Banks restored.
uint16_t map_object_leaves(const Args &)
{
	At here(0xC3E6);
	CFrame frame;
	cy(IN162 + 312);                                    // F:C3EA-C41C
	frame.save_regs(SAVE_R2_R5, F162_RET);
	uint16_t *R = T.r;
	R[4] = peek(0x01D2);
	R[5] = peek(0x01D4);
	poke(0x01D2, 0);
	poke(0x1400, 0);
	R[0] = 0x0400;
	set_bank_b(0x0400);
	R[1] = peek(0x09AA);
	R[3] = sext8(peekb((uint16_t)(0x09B8 + R[1]))) & 0x003F;
	R[2] = peekb((uint16_t)(0x0370 + R[3]));
	if (R[2] == 4)
	{
		cy(153);
		restore_banks(R[4], R[5]);
		set_c(false);
		return R[0];
	}
	cy(45);
	if (peekb((uint16_t)(0x649D + R[2])) & 0x40)
	{
		cy(63);
		R[0] = (uint16_t)(sext8(peekb(0x0E97)) + 1);
		pokeb(0x0E97, (uint8_t)R[0]);
	}
	cy(45);
	if (peekb((uint16_t)(0x649D + R[2])) & 0x10)
	{
		cy(63);
		R[0] = (uint16_t)(sext8(peekb(0x0E96)) + 1);
		pokeb(0x0E96, (uint8_t)R[0]);
	}
	cy(45);
	if (peekb((uint16_t)(0x04B0 + R[3])) & 0x10)
	{
		push(peek(0x09AA));
		cy(66);
		jsr(0xCF5E, 0xC468);
		cy(12);
	}
	else
	{
		R[2] = peekb((uint16_t)(0x0370 + R[3]));
		pokeb((uint16_t)(0x09B8 + R[1]), (uint8_t)R[2]);
		R[0] = (uint16_t)(R[3] << 1);
		R[1] = peek((uint16_t)(0x0430 + R[0]));
		push(R[3]);
		cy(174);
		jsr(0xB56E, 0xC482);
		cy(30);
		if (R[3] == 5)
		{
			ww(R[6], 6);
			cy(57);
			jsr(0xB56E, 0xC490);
		}
		cy(114);
		R[3] = peek(0x09AA);
		R[0] = (uint16_t)(R[3] << 1);
		poke((uint16_t)(0x0AF8 + R[0]), peek((uint16_t)(0x0AF8 + R[0])) & 0x7FFF);
		if ((int16_t)R[3] < 0x40)
		{
			cy(39);
			if (!(peekb((uint16_t)(0x649D + R[2])) & 0x80))
			{
				cy(102);
				R[1] = asr(R[1], 2);
				R[0] = peekb(0x0E9A);
				R[1] = (uint16_t)(R[1] + R[0]);
				pokeb((uint16_t)(0x0AB8 + R[3]), (uint8_t)R[1]);
			}
			cy(123);
			R[0] = 0x1800;
			set_bank_a(0x1800);
			ww(R[6], R[2]);
			jsr(0x5D08, 0xC4CC);
		}
	}
	cy(171);
	restore_banks(R[4], R[5]);
	set_c(false);
	return R[0];
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
	At here(0xC4E4);
	CFrame frame;
	cy(IN162 + 297);                                    // F:C4E8-C514
	frame.save_regs(SAVE_R2_R5, F162_RET);
	uint16_t *R = T.r;
	R[6] = (uint16_t)(R[6] - 0x0E);                      // seven locals
	push(peek(0x01D2));
	push(peek(0x01D4));
	const uint16_t sp = R[6];
	const uint16_t PAL = (uint16_t)(sp + 4), PX = (uint16_t)(sp + 6), ANIM = (uint16_t)(sp + 8),
		PY = (uint16_t)(sp + 0x0A), PROPS = (uint16_t)(sp + 0x0C), OFF_A = (uint16_t)(sp + 0x0E), OFF_B = (uint16_t)(sp + 0x10);
	auto banks_back = [&]() { restore_banks(rw((uint16_t)(sp + 2)), rw(sp)); };
	auto done = [&]() {
		cy(39);                                         // ADD #0x12,SP; RTS
		set_c(carry(sp, 0x12));
		return R[0];
	};
	auto give_up = [&]() {                              // F:C5B4: nothing created
		cy(171);
		banks_back();
		return done();
	};
	poke(0x01D2, 0);
	poke(0x1400, 0);
	R[0] = 0x0400;
	set_bank_b(0x0400);
	R[0] = peek(0x09AA);
	R[2] = sext8(peekb((uint16_t)(0x09B8 + R[0])));      // type
	if ((int16_t)R[0] >= 0x40)
	{
		cy(45);
		if (peekb((uint16_t)(0x649D + R[2])) & 0x10)
		{
			cy(36);
			R[0] = sext8(peekb(0x0E96));
			if (!R[0]) return give_up();
			cy(63);
			R[1] = (uint16_t)(R[0] - 1);
			pokeb(0x0E96, (uint8_t)R[1]);
		}
	}
	else
	{
		cy(45);
		if (peekb((uint16_t)(0x649D + R[2])) & 0x40)
		{
			cy(36);
			R[0] = sext8(peekb(0x0E97));
			if (!R[0])
			{
				// none left: count the event and remove the map object (not reached in the tests)
				cy(30);
				if ((int16_t)R[2] < 0x2F)
				{
					cy(72);
					R[0] = (uint16_t)(R[2] - 0x0A);
					R[1] = (uint16_t)(R[0] + 0x0B);
				}
				else
				{
					cy(60);
					R[4] = (uint16_t)(R[2] - 0x2F);
					R[1] = (uint16_t)(R[4] + 0x12);
				}
				push(R[1]);
				cy(54);
				jsr(0xD348, 0xC562);
				cy(63);
				ww(R[6], peek(0x09AA));
				jsr(0xCF5E, 0xC56A);
				cy(18 + 180);
				banks_back();
				R[6] = (uint16_t)(R[6] + 2);
				return done();
			}
			cy(177);
			R[1] = (uint16_t)(R[0] - 1);
			pokeb(0x0E97, (uint8_t)R[1]);
			R[0] = 0x1800;
			set_bank_a(0x1800);
			push(R[2]);
			jsr(0x5CC0, 0xC588);
			cy(72);
			poke(0x01D2, 0);
			poke(0x1400, 0);
			R[6] = (uint16_t)(R[6] + 2);
		}
	}
	cy(30);
	if (R[2] == 1)
	{
		cy(36);
		if (peekb(0x0375)) return give_up();
		cy(18);
		R[3] = 5;
	}
	else
	{
		cy(30);
		if (R[2] == 4)
		{
			cy(30 + 12);
			R[3] = 7;
		}
		else
		{
			cy(30);
			jsr(0xB5B8, 0xC5A8);
			cy(12 + 12);
			R[3] = R[0];
			if ((int16_t)R[3] < 0)
			{
				cy(12);
				return give_up();
			}
		}
	}
	cy(333);                                            // F:C5D2-C600
	pokeb((uint16_t)(0x0370 + R[3]), (uint8_t)R[2]);
	pokeb((uint16_t)(0x0390 + R[3]), peekb(0x09AA));
	ww(PAL, (uint16_t)(0x0470 + R[3]));
	ww(PROPS, (uint16_t)(0x6438 + R[2]));
	pokeb(rw(PAL), peekb(rw(PROPS)));
	pokeb(rw(PAL), peekb(rw(PAL)) & 0x30);
	if (R[2] == 1)
	{
		push(OFF_A);
		push(OFF_B);
		cy(132);
		jsr(0xDF40, 0xC612);
		cy(36);
		R[6] = (uint16_t)(R[6] + 2);
	}
	else
	{
		cy(618);                                        // F:C618-C66E
		R[5] = (uint16_t)(R[3] << 1);
		ww(PX, (uint16_t)(0x06F0 + R[5]));
		R[4] = (uint16_t)(R[2] << 1);
		poke(rw(PX), peek((uint16_t)(0x62A4 + R[4])));
		ww(PY, (uint16_t)(0x0730 + R[5]));
		poke(rw(PY), peek((uint16_t)(0x636E + R[4])));
		ww(ANIM, (uint16_t)(0x0770 + R[3]));
		pokeb(rw(ANIM), peekb((uint16_t)(0x6502 + R[2])));
		pokeb((uint16_t)(0x0790 + R[3]), peekb((uint16_t)(0x6502 + R[2])));
		R[0] = 0x1000;
		set_bank_a(0x1000);
		R[0] = 0x1400;
		set_bank_b(0x1400);
		jsr(0xBE10, 0xC672);
		cy(111);
		pokeb(rw(PAL), peekb(rw(PAL)) | 0x40);
		R[0] = peek(0x09B6);
		if (peekb((uint16_t)(0x4560 + R[0])) & 0x20)
		{
			cy(42);
			pokeb(rw(PAL), peekb(rw(PAL)) | 0x80);
		}
		cy(30);
		if (R[2] != 4)
		{
			push(OFF_A);
			push(OFF_B);
			cy(132);
			jsr(0xBF5E, 0xC6A0);
			cy(201);
			poke(rw(PX), (uint16_t)(peek(rw(PX)) + rw(OFF_A)));
			poke(rw(PY), (uint16_t)(peek(rw(PY)) + rw(OFF_B)));
			R[0] = peekb((uint16_t)(peek(0x09B2) + 2));  // the record's direction
			R[6] = (uint16_t)(R[6] + 2);
		}
		else
		{
			push(R[3]);
			cy(54);
			jsr(0xE0C2, 0xC6C2);
			cy(120);
			ww(OFF_A, peek(rw(PX)));
			ww(OFF_B, peek(rw(PY)));
			R[0] = 6;
		}
		cy(240);                                        // F:C6D2-C6F4
		pokeb((uint16_t)(0x07D0 + R[3]), (uint8_t)R[0]);
		poke(0x01D2, 0);
		poke(0x1400, 0);
		ww(PY, 0x0400);
		poke(0x01D4, 0x0400);
		poke(0x1402, rw(PY));
		if (peekb(rw(PROPS)) & 0x80)
		{
			cy(141);
			poke((uint16_t)(0x03B0 + R[5]), peek((uint16_t)(0x5F7C + R[4])));
			poke((uint16_t)(0x03F0 + R[5]), peek((uint16_t)(0x6110 + R[4])));
			if (peekb(rw(PROPS)) & 0x04)
			{
				cy(36);
				pokeb((uint16_t)(0x04B0 + R[3]), peekb((uint16_t)(0x04B0 + R[3])) | 0x20);
			}
			cy(30);
			if (R[2] == 0x57)
			{
				cy(36);
				if (peekb(0x01E7))
				{
					cy(126);
					ww(PY, 5);
					pokeb((uint16_t)(0x0790 + R[3]), 5);
					pokeb(rw(ANIM), 5);
				}
			}
		}
		else
		{
			cy(213);
			const uint16_t d = peekb((uint16_t)(0xFDBA + R[0]));
			ww((uint16_t)(R[6] - 2), (uint16_t)(d + peek((uint16_t)(0x5F7C + R[4]))));
			ww(PY, rw((uint16_t)(R[6] - 2)));
			R[1] = peek(rw(PY));
			if (peekb((uint16_t)(0x6502 + R[2])))
			{
				cy(60);
				R[1] = peek(R[1]);
				pokeb(rw(ANIM), peekb(rw(ANIM)) | 0x80);
			}
			cy(180);
			poke((uint16_t)(0x03B0 + R[5]), R[1]);
			R[4] = peek((uint16_t)(0x6110 + R[4]));
			ww((uint16_t)(R[6] - 2), d);
			R[4] = (uint16_t)(R[4] + d);
			poke((uint16_t)(0x03F0 + R[5]), peek(R[4]));
		}
		cy(138);
		R[0] = sext8(peekb((uint16_t)(0x649D + R[2]))) & 1;
		pokeb((uint16_t)(0x0490 + R[3]), peekb((uint16_t)(0x0490 + R[3])) | (uint8_t)R[0]);
		R[0] = peekb((uint16_t)(0x623F + R[2]));
		if (R[0])
		{
			cy(30);
			if (R[0] != 6)
			{
				cy(54);
				R[0] = peek(0x09AA);
				if ((int16_t)R[0] < 0x40)
				{
					cy(177);
					R[4] = peekb((uint16_t)(0x0AB8 + R[0]));
					R[2] = peekb(0x0E9A);
					R[4] = (uint16_t)(R[4] - R[2]);
					R[1] = (uint16_t)((R[4] & 0x00FF) << 2);
					pokeb((uint16_t)(0x0AB8 + R[0]), 0);
				}
				else
				{
					cy(18);
					R[1] = 0x0010;
				}
				cy(30);
				poke((uint16_t)(0x0430 + R[5]), R[1]);
			}
		}
	}
	cy(177);                                            // F:C7B2-C7CC
	poke(0x01D2, 0);
	poke(0x1400, 0);
	R[0] = 0x0400;
	set_bank_b(0x0400);
	if (peekb(rw(PROPS)) & 0x08)
	{
		cy(36);
		pokeb((uint16_t)(0x04B0 + R[3]), peekb((uint16_t)(0x04B0 + R[3])) | 0x10);
	}
	cy(51);
	const bool no_pieces = (peekb(rw(PROPS)) & 0x40) != 0;
	cy(no_pieces ? 48 : 33);
	ww(R[6], no_pieces ? (uint16_t)(R[3] | 0x0080) : R[3]);
	cy(30);
	jsr(0xB4B6, 0xC7EA);
	cy(234);                                            // F:C7EA-C810
	R[0] = peek(0x09AA);
	R[4] = (uint16_t)(0x09B8 + R[0]);
	pokeb(R[4], (uint8_t)R[3]);
	R[0] = (uint16_t)(R[0] << 1);
	poke((uint16_t)(0x0AF8 + R[0]), peek((uint16_t)(0x0AF8 + R[0])) | 0x8000);
	R[3] = peek((uint16_t)(0x0AF8 + R[0])) & 0x01F0;
	R[2] = (uint16_t)(peek(0x0228) + 0x20);
	if (!((int16_t)R[3] < (int16_t)R[2]))
	{
		cy(27);
		pokeb(R[4], peekb(R[4]) | 0x80);
	}
	cy(123);
	R[2] = (uint16_t)(peek(0x0224) + 2);
	R[3] = R[2];
	R[1] = sext8(peekb((uint16_t)(0x0AF8 + R[0]))) & 0x000F;
	if (R[1] < R[3])
	{
		cy(27);
		pokeb(R[4], peekb(R[4]) | 0x40);
	}
	cy(180);
	banks_back();
	R[6] = (uint16_t)(R[6] + 2);
	return done();
}

// F:BCB8 (assembly): keep the current map object's on-screen state up to date.  Without an
// object (descriptor bit 15 clear) it may enter (F:BD78).  With one, bits 7 / 6 of its slot
// entry 0x09B8 record the side of the screen in x / y (beyond 0x4000); an object that crossed
// from one side to the other far off screen (beyond 0xC000) leaves (F:C3E6).
void check_map_object_view()
{
	At here(0xBCB8);
	cy(87);
	T.r[1] = peek(0x09AA);
	T.r[0] = (uint16_t)(T.r[1] << 1);
	if ((int16_t)peek((uint16_t)(0x0AF8 + T.r[0])) < 0)
	{
		cy(117);
		T.r[1] = (uint16_t)(T.r[1] + 0x09B8);
		T.r[0] = sext8(peekb(T.r[1])) & 0x003F;
		T.r[2] = (uint16_t)(T.r[0] << 1);
		T.r[0] = peek((uint16_t)(0x0630 + T.r[2]));
		bool leave;
		if ((int16_t)T.r[0] >= 0)
		{
			cy(30 + 39);
			fl::cmp(T.r[0], 0x4000);
			const uint8_t b = lt() ? (uint8_t)(peekb(T.r[1]) & ~0x80) : (uint8_t)(peekb(T.r[1]) | 0x80);
			pokeb(T.r[1], b);
			leave = false;
		}
		else
		{
			cy(30);
			fl::cmp(T.r[0], 0xC000);
			const uint8_t b = peekb(T.r[1]);
			if (fC())
			{
				cy(30);
				leave = (int8_t)b > 0;
			}
			else
			{
				cy(30);
				leave = (int8_t)b < 0;
				if (leave) cy(12);
			}
		}
		if (!leave)
		{
			cy(93);
			T.r[0] = peek(0x09AA);
			T.r[1] = (uint16_t)(0x09B8 + T.r[0]);
			T.r[0] = peek((uint16_t)(0x0670 + T.r[2]));
			if ((int16_t)T.r[0] > 0)
			{
				cy(30 + 48);
				fl::cmp(T.r[0], 0x4000);
				const uint8_t b = lt() ? (uint8_t)(peekb(T.r[1]) & ~0x40) : (uint8_t)(peekb(T.r[1]) | 0x40);
				pokeb(T.r[1], b);
				f_nzv(nzb(b));
				return;
			}
			cy(30);
			fl::cmp(T.r[0], 0xC000);
			const bool c = fC();
			const uint8_t b = peekb(T.r[1]);
			f_nzv(nzb(b & 0x40));
			cy(36);
			if (c ? (b & 0x40) != 0 : (b & 0x40) == 0)
			{
				cy(21);
				return;
			}
			if (!c) cy(12);
		}
		cy(30);
		jsr(0xC3E6, 0xBD36);
		cy(21);
		return;
	}
	cy(30);
	jsr(0xBD78, 0xBD3C);
	cy(36);
	const uint16_t v = peek(0x099A);
	fl::tst(v);
	if ((int16_t)v >= 0)
	{
		cy(21);
		return;
	}
	// a second try with the window widened by the scroll margins
	cy(162);
	poke(0x0224, (uint16_t)(peek(0x0224) + 0x0C));
	poke(0x0226, (uint16_t)(peek(0x0226) + 0x0C));
	poke(0x0228, (uint16_t)(peek(0x0228) - 0x50));
	poke(0x022A, (uint16_t)(peek(0x022A) - 0x50));
	jsr(0xBD78, 0xBD5E);
	cy(156 + 21);
	poke(0x0224, peek(0x099A));
	poke(0x0226, peek(0x099C));
	poke(0x0228, peek(0x0996));
	const uint16_t r = peek(0x0998);
	poke(0x022A, r);
	fl::mov(r);
}

// F:C850 (assembly; R1-R5 kept): the current map object takes the definition its record
// points to (F:BE7C) when that is free: F:BFBC, R0 = 1; otherwise R0 = -1
void try_map_object_move()
{
	At here(0xC850);
	cy(30);
	if (jsr(0xBE7C, 0xC854))
	{
		cy(24 + 39);
		fl::tst(T.r[0]);
		T.r[0] = 0xFFFF;
		fl::mov(T.r[0]);
		return;
	}
	cy(24 + 30);
	jsr(0xBFBC, 0xC85C);
	cy(39);
	T.r[0] = 1;
	fl::mov(T.r[0]);
}

// F:C868  try_next_map_definition(int group): the same with the next record entry (0x09B2 + 4),
// only if that definition is free and in the given group (0xFD91 by properties 0x4000 bits
// 0-3; definitions without a group always qualify).  Returns 0 when moved, else -1.
uint16_t try_next_map_definition(const Args &a)
{
	At here(0xC868);
	CFrame frame;
	cy(87);
	push(T.r[1]);
	poke(0x09B2, (uint16_t)(peek(0x09B2) + 4));
	cy(24);
	if (jsr(0xBE7C, 0xC874))
	{
		cy(30 + 39);
		set_c(false);
		return 0xFFFF;
	}
	cy(69);
	T.r[0] = peek(0x09B4);
	if (peekb((uint16_t)(0x4000 + T.r[0])) & 0x0F)
	{
		cy(123);
		T.r[0] = sext8(peekb((uint16_t)(0x4000 + T.r[0]))) & 0x000F;
		T.r[1] = peekb((uint16_t)(0xFD91 + T.r[0]));
		if (T.r[1] != a.u(0))
		{
			cy(30 + 39);
			set_c(T.r[1] < a.u(0));                     // CMP R1,arg
			return 0xFFFF;
		}
	}
	cy(30);
	jsr(0xBFBC, 0xC89C);
	cy(12 + 12 + 39);
	set_c(false);
	return 0;
}

// F:CA4C (assembly): the map object of object slot 0x022C (map object 0x09AA, located) moves
// on: objects flagged 0x01 (0x04B0) follow their record (the second half with flag 0x02);
// the others toggle their definition's bit in the bitmap 0x0DAA and try their own record
// (F:C850) or, for definitions with properties bit 6, the next entry (F:C868) in their type's
// group.  When neither works the object stops (speed 0, animation from 0xFD8A by 0x65CC kind,
// 0x0770 bit 7); flag 0x02 of 0x04B0 records whether it moved (R0 = 0) or not.
void update_map_object_slot()
{
	At here(0xCA4C);
	cy(90);
	T.r[1] = peek(0x022C);
	T.r[5] = (uint16_t)(0x04B0 + T.r[1]);
	if (peekb(T.r[5]) & 0x01)
	{
		cy(36);
		if (peekb(T.r[5]) & 0x02)
		{
			cy(33);
			poke(0x09B2, (uint16_t)(peek(0x09B2) + 4));
		}
		cy(30);
		jsr(0xBE7C, 0xCA6C);
		cy(30);
		jsr(0xBFBC, 0xCA70);
		cy(21);
		return;
	}
	cy(195);
	T.r[0] = 0xFFFF;
	T.r[2] = peek(0x09B6);
	T.r[3] = asr(T.r[2], 3);
	T.r[4] = (uint16_t)(0x0DAA + T.r[3]);
	T.r[3] = T.r[2] & 7;
	const uint16_t bit = (uint16_t)(0xFC74 + T.r[3]);
	bool test = true;                                   // through F:CAD2
	if (!(peekb(T.r[4]) & peekb(bit)))
	{
		cy(30);
		jsr(0xC850, 0xCA98);
		cy(45);
		if (peekb((uint16_t)(0x4560 + T.r[2])) & 0x40)
		{
			cy(60);
			pokeb(T.r[4], peekb(T.r[4]) | peekb(bit));
			if ((int16_t)T.r[0] < 0)
			{
				push(peekb((uint16_t)(0x0370 + T.r[1])));
				cy(90);
				jsr(0xC868, 0xCAB2);
				cy(12 + 18);
				T.r[6] = (uint16_t)(T.r[6] + 2);
			}
			else
				test = false;
		}
	}
	else
	{
		pokeb(T.r[4], (uint8_t)(peekb(T.r[4]) & ~peekb(bit)));
		push(peekb((uint16_t)(0x0370 + T.r[1])));
		cy(126);
		jsr(0xC868, 0xCAC2);
		cy(24);
		if ((int16_t)T.r[0] < 0)
		{
			cy(63);
			poke(0x09B2, (uint16_t)(peek(0x09B2) - 4));
			jsr(0xC850, 0xCAD0);
		}
		cy(18);
		T.r[6] = (uint16_t)(T.r[6] + 2);
	}
	if (test) cy(24);
	fl::tst(T.r[0]);
	if ((int16_t)T.r[0] < 0)
	{
		cy(408);
		poke(0x01D2, 0);
		poke(0x1400, 0);
		T.r[0] = 0x0400;
		set_bank_b(0x0400);
		T.r[0] = (uint16_t)(T.r[1] << 1);
		T.r[2] = peekb((uint16_t)(0x0370 + T.r[1]));
		T.r[2] = sext8(peekb((uint16_t)(0x65CC + T.r[2])));
		pokeb((uint16_t)(0x0430 + T.r[0]), peekb((uint16_t)(0xFD8A + T.r[2])));
		pokeb((uint16_t)(0x0431 + T.r[0]), 0);
		poke((uint16_t)(0x04F0 + T.r[0]), 0);
		poke((uint16_t)(0x0570 + T.r[0]), 0);
		fl::clr();
		const uint8_t f = peekb((uint16_t)(0x0770 + T.r[1])) | 0x80;
		pokeb((uint16_t)(0x0770 + T.r[1]), f);
		f_nzv(nzb(f));
		return;
	}
	cy(12);
	if (!T.r[0])
	{
		cy(48);
		const uint8_t f = peekb(T.r[5]) | 0x02;
		pokeb(T.r[5], f);
		f_nzv(nzb(f));
		return;
	}
	cy(72);
	T.r[0] = 0xFF02;                                    // MOV #0xFFFD,R0; COMB R0
	const uint8_t f = (uint8_t)(peekb(T.r[5]) & ~0x02);
	pokeb(T.r[5], f);
	f_nzvc(nzb(f) | CF);
}

// F:BC52 (assembly): check four map objects a frame for entering or leaving the view
// (F:BCB8), going round from 0x09AC (skipping 0x09A8 and those above 0x09AE without a slot
// entry); the view window 0x0224-0x022A is taken from 0x0996-0x099C.  Banks 0x1000 / 0x1400.
void check_map_objects_in_view()
{
	At here(0xBC52);
	cy(330);
	T.r[0] = 0x1000;
	set_bank_a(0x1000);
	T.r[0] = 0x1400;
	set_bank_b(0x1400);
	poke(0x0224, peek(0x099A));
	poke(0x0226, peek(0x099C));
	poke(0x0228, peek(0x0996));
	poke(0x022A, peek(0x0998));
	push(4);
	do
	{
		int c = 126;                                    // F:BC86-BC98
		poke(0x09AA, peek(0x09AC));
		poke(0x09AC, (uint16_t)(peek(0x09AC) + 1));
		T.r[0] = peek(0x09AA);
		bool go = T.r[0] != peek(0x09A8);
		if (go)
		{
			c += 36;
			go = (int16_t)T.r[0] <= (int16_t)peek(0x09AE);
		}
		if (go)
		{
			c += 39;
			go = peekb((uint16_t)(0x09B8 + T.r[0])) != 0;
		}
		if (go)
		{
			cy(c + 30);
			c = 0;
			jsr(0xBCB8, 0xBCAA);
		}
		ww(T.r[6], (uint16_t)(rw(T.r[6]) - 1));
		charge(rw(T.r[6]) ? 0xBC86 : 0xBCAE, c + 33);   // DEC (SP), BNE
	} while (rw(T.r[6]));
	cy(72);
	poke(0x09AC, peek(0x09AC) & 0x003F);
	fl::tst(pop());
}

// F:C8AA (assembly): advance the map object clock 0x0E9A; fixed map objects (0x09AE down to 0)
// whose timer 0x0AB8 is due toggle their definition's bit in 0x0DAA and try to move on
// (F:C850 / F:C868 as F:CA4C does); when that fails the timer is set 8 ticks ahead.  A timer
// left at 0 becomes 1.  The result of the last attempt stays in the routine's stack word,
// which the original tests on return.
void tick_map_object_timers()
{
	At here(0xC8AA);
	cy(108);
	T.r[6] = (uint16_t)(T.r[6] - 2);
	const uint16_t result = T.r[6];
	pokeb(0x0E9A, (uint8_t)(peekb(0x0E9A) + 1));
	T.r[0] = peekb(0x0E9A);
	if (T.r[0])
	{
		cy(51);
		poke(0x09AA, peek(0x09AE));
		T.r[1] = T.r[0];
		for (;;)
		{
			T.r[0] = peek(0x09AA);
			if ((int16_t)T.r[0] < 0)
			{
				cy(36);
				break;
			}
			// cycles of this pass at C8C2, without the routines it calls
			int cycles = 99;                            // C8C2-C8D0
			T.r[4] = peekb((uint16_t)(0x0AB8 + T.r[0]));
			if (T.r[4] == T.r[1])
			{
				cycles += 144 + 30 + 204;               // C8D2-C8E6, JSR, C8EE-C90E
				T.r[0] = 0x1000;
				set_bank_a(0x1000);
				T.r[0] = 0x1400;
				set_bank_b(0x1400);
				jsr(0xBE10, 0xC8EE);
				ww(result, 0xFFFF);
				T.r[3] = peek(0x09B6);
				T.r[2] = asr(T.r[3], 3);
				T.r[5] = (uint16_t)(0x0DAA + T.r[2]);
				T.r[4] = T.r[3] & 7;
				const uint16_t bit = (uint16_t)(0xFC74 + T.r[4]);
				bool check = true;
				if (!(peekb(T.r[5]) & peekb(bit)))
				{
					cycles += 96;                       // C910-C91C
					jsr(0xC850, 0xC914);
					ww(result, T.r[0]);
					if (peekb((uint16_t)(0x4560 + T.r[3])) & 0x40)
					{
						cycles += 60;                   // C91E-C924
						pokeb(T.r[5], peekb(T.r[5]) | peekb(bit));
						if ((int16_t)T.r[0] >= 0)
							check = false;
						else
						{
							cycles += 117 + 78;         // C926-C934, C958-C960
							T.r[0] = peek(0x09AA);
							T.r[4] = sext8(peekb((uint16_t)(0x09B8 + T.r[0])));
							push(T.r[4]);
							jsr(0xC868, 0xC934);
							ww(result, T.r[0]);
							T.r[6] = (uint16_t)(T.r[6] + 2);
						}
					}
					else
						cycles += 30;                   // C95E-C960
				}
				else
				{
					cycles += 183;                      // C936-C94C
					pokeb(T.r[5], (uint8_t)(peekb(T.r[5]) & ~peekb(bit)));
					T.r[0] = peek(0x09AA);
					T.r[4] = sext8(peekb((uint16_t)(0x09B8 + T.r[0])));
					push(T.r[4]);
					jsr(0xC868, 0xC948);
					ww(result, T.r[0]);
					if ((int16_t)T.r[0] < 0)
					{
						cycles += 63 + 30;              // C94E-C954, C958
						poke(0x09B2, (uint16_t)(peek(0x09B2) - 4));
						jsr(0xC850, 0xC958);
						ww(result, T.r[0]);
					}
					cycles += 48;                       // C95C-C960
					T.r[6] = (uint16_t)(T.r[6] + 2);
				}
				if (check && (int16_t)rw(result) < 0)
				{
					cycles += 108;                      // C962-C970
					T.r[0] = peek(0x09AA);
					T.r[4] = (uint16_t)(peekb(0x0E9A) + 8);
					pokeb((uint16_t)(0x0AB8 + T.r[0]), (uint8_t)T.r[4]);
				}
				cycles += 63;                           // C974-C97C
				T.r[0] = peek(0x09AA);
				if (!peekb((uint16_t)(0x0AB8 + T.r[0])))
				{
					cycles += 36;                       // C97E
					pokeb((uint16_t)(0x0AB8 + T.r[0]), 1);
				}
			}
			poke(0x09AA, (uint16_t)(peek(0x09AA) - 1));
			charge(0xC8C2, cycles + 39);                  // C984 DEC, BR C8C2
		}
	}
	cy(39);
	fl::tst(rw(result));
	T.r[6] = (uint16_t)(T.r[6] + 2);
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
	At here(0xCB24);
	cy(57);                                             // F:CB24-CB2A
	T.r[6] = (uint16_t)(T.r[6] - 2);
	const uint16_t mode_at = T.r[6];
	if (peekb(0x0E68))
	{
		cy(39);
		const uint8_t n = (uint8_t)(peekb(0x0E68) - 1);
		pokeb(0x0E68, n);
		if (!n)
		{
			cy(174);
			T.r[0] = 0x3800;
			set_bank_a(0x3800);
			T.r[0] = 0x3C00;
			set_bank_b(0x3C00);
			jsr(0x4F42, 0xCB4E);
		}
	}
	cy(51);
	T.r[0] = sext8(peekb(0x0994));
	poke(0x022C, T.r[0]);
	auto slot2 = []() { return (uint16_t)(peek(0x022C) << 1); };
	for (;;)
	{
		int c = 54;                                     // F:CB56-CB5E
		T.r[0] = peek(0x022C);
		if ((int16_t)T.r[0] < 5)
		{
			cy(c + 18);                                 // JMP F:CDCC
			break;
		}
		c += 72;                                        // F:CB64-CB6E
		T.r[1] = (uint16_t)((T.r[0] << 1) + 0x0430);
		bool act = false;
		if (peek(T.r[1]))
		{
			c += 51;                                    // F:CB70-CB74
			const uint16_t count = (uint16_t)(peek(T.r[1]) - 1);
			poke(T.r[1], count);
			act = true;
			if ((int16_t)count > 0)
			{
				c += 45;                                // F:CB76-CB7C
				act = !(peekb((uint16_t)(0x04B0 + T.r[0])) & 0x0C);
			}
		}
		if (act)
		{
			pokeb((uint16_t)(0x04B0 + T.r[0]), peekb((uint16_t)(0x04B0 + T.r[0])) | 0x08);
			poke(0x01D2, 0);
			poke(0x1400, 0);
			T.r[1] = 0x0400;
			set_bank_b(0x0400);
			T.r[1] = peekb((uint16_t)(0x0370 + T.r[0]));
			ww(mode_at, peekb((uint16_t)(0x623F + T.r[1])));
			T.r[1] = 0x1000;
			set_bank_a(0x1000);
			T.r[1] = 0x1400;
			set_bank_b(0x1400);
			pokeb(0x09AA, peekb((uint16_t)(0x0390 + T.r[0])));
			pokeb(0x09AB, 0);
			cy(c + 510);                                // F:CB7E-CBC8
			c = 0;
			jsr(0xBE10, 0xCBCC);
			c += 57;                                    // dispatch F:CBCC-CBD0
			const uint16_t mode = rw(mode_at);
			T.r[0] = (uint16_t)(mode << 1);
			auto call_ca4c = [&](uint16_t ret) {
				cy(c + 30);
				c = 0;
				jsr(0xCA4C, ret);
			};
			switch (mode)
			{
			case 0:
			case 11:
				c += 78 + 12 + 12;
				T.r[0] = slot2();
				poke((uint16_t)(0x0430 + T.r[0]), 0);
				break;
			case 1:
				call_ca4c(0xCCEA);
				c += 12;
				break;
			case 2:
				cy(c + 30);
				c = 0;
				jsr(0xDE28, 0xCBE4);
				c += 12 + 12 + 12;
				break;
			case 3:
				cy(c + 30);
				c = 0;
				jsr(0xCA0C, 0xCBEA);
				c += 12;
				call_ca4c(0xCCEA);
				c += 12;
				break;
			case 4:
				cy(c + 174);
				c = 0;
				T.r[0] = 0x3800;
				set_bank_a(0x3800);
				T.r[0] = 0x3C00;
				set_bank_b(0x3C00);
				jsr(0x5018, 0xCC08);
				c += 12 + 12;
				break;
			case 5:
				cy(c + 102);
				c = 0;
				T.r[0] = slot2();
				poke((uint16_t)(0x0430 + T.r[0]), 0x0010);
				jsr(0xBCB8, 0xCC1A);
				c += 12 + 12;
				break;
			case 6:
			case 10:
				c += 240;
				poke(0x01D2, 0);
				poke(0x1400, 0);
				T.r[0] = 0x0400;
				set_bank_b(0x0400);
				T.r[0] = peekb((uint16_t)(0x0370 + peek(0x022C)));
				if (peekb((uint16_t)(0x649D + T.r[0])) & 0x10)
				{
					c += 63;
					T.r[0] = (uint16_t)(sext8(peekb(0x0E96)) + 1);
					pokeb(0x0E96, (uint8_t)T.r[0]);
				}
				push(peek(0x09AA));
				cy(c + 66);
				c = 0;
				jsr(0xCF5E, 0xCC56);
				c += 18 + 30;
				T.r[6] = (uint16_t)(T.r[6] + 2);
				break;
			case 7:
				c += 129;
				T.r[0] = sext8(peekb((uint16_t)(0x4000 + peek(0x09B6)))) & 0x000F;
				T.r[1] = slot2();
				if (T.r[0]) c += 51;
				if (T.r[0] && (peekb(0x0EA4) & peekb((uint16_t)(0xFC73 + T.r[0]))))
				{
					c += 42;
					poke((uint16_t)(0x03F0 + T.r[1]), 0);
					call_ca4c(0xCCEA);
					c += 12;
				}
				else
				{
					c += 90 + 12 + 12;
					poke((uint16_t)(0x04F0 + T.r[1]), 0);
					poke((uint16_t)(0x0570 + T.r[1]), 0);
					poke((uint16_t)(0x0430 + T.r[1]), 0);
				}
				break;
			case 8:
				c += 108;
				T.r[0] = (uint16_t)((sext8(peekb((uint16_t)(0x4000 + peek(0x09B6)))) & 0x000F) << 1);
				switch (T.r[0] >> 1)
				{
				case 0:
					call_ca4c(0xCCEA);
					c += 12;
					break;
				case 1:
					c += 438;
					poke(0x01D2, 0);
					poke(0x1400, 0);
					T.r[0] = 0x0400;
					set_bank_b(0x0400);
					T.r[0] = peek(0x022C);
					T.r[1] = (uint16_t)(T.r[0] << 1);
					T.r[0] = (uint16_t)((peekb((uint16_t)(0x0370 + T.r[0])) - 0x2B) << 1);
					poke((uint16_t)(0x03B0 + T.r[1]), peek((uint16_t)(0x5408 + T.r[0])));
					T.r[0] = 0x1000;
					set_bank_a(0x1000);
					T.r[0] = 0x1400;
					set_bank_b(0x1400);
					call_ca4c(0xCCEA);
					c += 12;
					break;
				case 2:
					c += 69;
					T.r[0] = peek(0x022C);
					if (!(peekb((uint16_t)(0x04B0 + T.r[0])) & 0x01))
					{
						// another map object on the same cell (descriptor + 0x400, then + 0x600)
						T.r[0] = (uint16_t)(peek(0x09AA) << 1);
						push((uint16_t)(peek((uint16_t)(0x0AF8 + T.r[0])) + 0x0400));
						push(peek(0x09AA));
						cy(c + 168);
						c = 0;
						jsr(0xCDD0, 0xCD0E);
						c += 42;
						T.r[1] = T.r[0];
						bool found = T.r[1] != 0xFFFF;
						if (!found)
						{
							T.r[0] = (uint16_t)(peek(0x09AA) << 1);
							ww(T.r[6], (uint16_t)(peek((uint16_t)(0x0AF8 + T.r[0])) + 0x0600));
							push(peek(0x09AA));
							cy(c + 165);
							c = 0;
							jsr(0xCDD0, 0xCD2C);
							c += 60;
							T.r[1] = T.r[0];
							T.r[6] = (uint16_t)(T.r[6] + 2);
							found = T.r[1] != 0xFFFF;
						}
						if (found)
						{
							c += 84;
							T.r[0] = sext8(peekb((uint16_t)(0x09B8 + T.r[1]))) & 0x003F;
							if (peekb((uint16_t)(0x0770 + T.r[0])) & 0x80)
							{
								ww(T.r[6], T.r[1]);
								push(4);
								cy(c + 81);
								c = 0;
								jsr(0xC98E, 0xCD4E);
								c += 111;
								T.r[0] = (uint16_t)((sext8(peekb((uint16_t)(0x09B8 + T.r[1]))) & 0x003F) << 1);
								poke((uint16_t)(0x0430 + T.r[0]), 0x0700);
								T.r[6] = (uint16_t)(T.r[6] + 2);
							}
						}
						c += 24;
						T.r[6] = (uint16_t)(T.r[6] + 4);
					}
					cy(c + 30);
					c = 0;
					jsr(0xCA4C, 0xCD66);
					c += 69;
					T.r[0] = peek(0x022C);
					if (!(peekb((uint16_t)(0x04B0 + T.r[0])) & 0x01))
					{
						c += 84 + 12;
						pokeb((uint16_t)(0x0770 + T.r[0]), peekb((uint16_t)(0x0770 + T.r[0])) | 0x80);
						T.r[0] = (uint16_t)(T.r[0] << 1);
						poke((uint16_t)(0x0430 + T.r[0]), 0x7FFF);
					}
					break;
				default:                                // the table's other entries are not code
					break;
				}
				break;
			case 9:
				c += 69;
				T.r[0] = peek(0x022C);
				if (peekb((uint16_t)(0x0490 + T.r[0])) & 0x06)
				{
					c += 72;
					T.r[0] = (uint16_t)(T.r[0] << 1);
					poke((uint16_t)(0x0430 + T.r[0]), 0x000F);
				}
				else
				{
					poke(0x01D2, 0);
					poke(0x1400, 0);
					T.r[0] = 0x0400;
					set_bank_b(0x0400);
					push(peek(0x09AA));
					cy(c + 192);
					c = 0;
					jsr(0xCF5E, 0xCDAA);
					c += 63 + 30;
					T.r[0] = (uint16_t)(sext8(peekb(0x0E96)) + 1);
					pokeb(0x0E96, (uint8_t)T.r[0]);
					T.r[6] = (uint16_t)(T.r[6] + 2);
				}
				break;
			default:                                    // beyond the jump table (not in 0x623F)
				break;
			}
		}
		else
			c += 12 + 12;                               // F:CC1A, F:CCEA
		poke(0x022C, (uint16_t)(peek(0x022C) - 1));
		charge(0xCB56, c + 45);                           // F:CDC4-CDC8 DEC, JMP
	}
	cy(39);
	fl::tst(rw(mode_at));
	T.r[6] = (uint16_t)(T.r[6] + 2);
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
			jsr(0x8096, ret_8096);
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
		jsr(0x8068, ret_8068);
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
		jsr(0x8096, ret);
		io(27, [] { pokeb(0x0FED, (uint8_t)(peekb(0x0FED) - 1)); });   // DECB: not busy any more
	}
};

// F:B8FC (interrupt, 32V, installed by F:8270): sprite lists only: quarter 1 rebuilds list 1
// (F:B1C8, F:B1EA), moves the objects (F:B378) and updates the second chains (F:B120);
// quarter 4 rebuilds list 0 and updates the first chains (F:B092)
void irq_32v_sprites()
{
	At here(0xB8FC);
	Irq32v irq;
	cy(27 + 24 + 12);                                   // INCB @#0x0191; TSTB @#0x0FED; BEQ
	if (!irq.enter(0xB91E, 0xB93E, 12)) return;
	cy(102);
	const uint8_t quarter = peekb(0x0191);
	if (quarter == 1)
	{
		cy(63);
		pokeb(0x0993, 1);
		jsr(0xB1C8, 0xB95A);
		cy(30);
		jsr(0xB1EA, 0xB95E);
		cy(90);
		hw_banks(0, 0x0400);
		jsr(0xB378, 0xB96C);
		cy(90);
		hw_banks(0, 0x0400);
		jsr(0xB120, 0xB97A);
		cy(12);
	}
	else
	{
		cy(42);
		if (quarter == 4)
		{
			cy(57);
			pokeb(0x0993, 0);
			jsr(0xB1C8, 0xB98C);
			cy(30);
			jsr(0xB1EA, 0xB990);
			cy(90);
			hw_banks(0, 0x0400);
			jsr(0xB092, 0xB99E);
		}
	}
	irq.leave(0xB9BE);
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
	At here(0xB66E);
	Irq32v irq;
	cy(27 + 24 + 12);                                   // INCB @#0x0191; TSTB @#0x0FED; BEQ
	if (!irq.enter(0xB690, 0xB6B2, 18)) return;
	cy(102);
	const uint8_t quarter = peekb(0x0191);
	if (quarter == 1)
	{
		cy(75);
		pokeb(0x0993, 1);
		if (peekb(0x0193) & 0x01)
		{
			cy(108);
			poke(0x0180, 1);
			pokeb(0x0995, 1);
			if (peekb(0x0193) & 0x04)
			{
				cy(63);
				pokeb(0x0FEB, (uint8_t)(peekb(0x0FEB) - 1));
				bool reset = true;
				if (peekb(0x0FEA))
				{
					cy(36);
					reset = !peekb(0x0FEB);
				}
				if (reset)
				{
					cy(96);
					pokeb(0x0FEA, 1);
					pokeb(0x0995, 0);
					if (!peekb(0x0FEB))
					{
						cy(45);
						pokeb(0x0FEB, 2);
					}
				}
			}
			else
			{
				cy(72);
				pokeb(0x0FEB, 0x3C);
				pokeb(0x0FEA, 0);
			}
		}
		else
		{
			cy(27);
			pokeb(0x0995, 0);
		}
		cy(30);
		jsr(0xB1C8, 0xB722);
		cy(36);
		if (!peekb(0x0995))
		{
			cy(30);
			jsr(0xB03C, 0xB72C);
			cy(30);
			jsr(0xB1EA, 0xB730);
			cy(60);
			hw_banks(0, 0x0400);
		}
		cy(42);
		if (peek(0x01C4) & 1)
		{
			cy(30);
			jsr(0xB120, 0xB746);
			cy(12);
		}
		else
		{
			cy(42);
			if (!(peek(0x01C4) & 3))
			{
				cy(30);
				jsr(0xAF32, 0xB754);
			}
		}
		cy(36);
		if (!peekb(0x0995))
		{
			cy(42);
			if (peekb(0x018F) != 4)
			{
				cy(42);
				if (peekb(0x0F59) != 7)
				{
					cy(174);
					T.r[0] = 0x2400;
					set_bank_a(0x2400);
					T.r[0] = 0x2800;
					set_bank_b(0x2800);
					jsr(0x7942, 0xB786);
				}
				cy(30);
				jsr(0xE254, 0xB78A);
				cy(102);
				T.r[0] = 0x3800;
				set_bank_a(0x3800);
				jsr(0x4098, 0xB79A);
				cy(60);
				hw_banks(0, 0x0400);
			}
			cy(90);
			hw_banks(0, 0x0400);
			jsr(0xB378, 0xB7B2);
		}
		cy(18);
	}
	else
	{
		cy(42);
		if (quarter == 4)
		{
			cy(57);
			pokeb(0x0993, 0);
			jsr(0xB1C8, 0xB7C6);
			cy(36);
			if (!peekb(0x0995))
			{
				cy(30);
				jsr(0xAFF0, 0xB7D0);
				cy(30);
				jsr(0xB1EA, 0xB7D4);
				cy(60);
				hw_banks(0, 0x0400);
			}
			cy(66);
			T.r[0] = (uint16_t)(peek(0x01C4) + 1);
			if (T.r[0] & 1)
			{
				cy(30);
				jsr(0xB092, 0xB7EE);
				cy(12);
			}
			else
			{
				cy(30);
				if (!(T.r[0] & 3))
				{
					cy(30);
					jsr(0xAF32, 0xB7FA);
				}
			}
			cy(42);
			if (peekb(0x018F) != 4)
			{
				cy(36);
				if (!peekb(0x0995))
				{
					cy(42);
					if (peekb(0x031F) == 5)
					{
						cy(174);
						T.r[0] = 0x1800;
						set_bank_a(0x1800);
						T.r[0] = 0x1C00;
						set_bank_b(0x1C00);
						jsr(0x792A, 0xB8B6);
					}
					else
					{
						cy(36);
						if (peekb(0x0EA0) & 0x80)
						{
							cy(30);
							jsr(0xD566, 0xB81A);
							cy(24);
						}
						cy(12);
						if (peekb(0x0EA0))
						{
							cy(30);
							jsr(0xD632, 0xB868);
						}
						else
						{
							cy(30);
							jsr(0x934C, 0xB824);
							cy(114);
							T.r[0] = 0x1800;
							set_bank_a(0x1800);
							bool usual = true;
							if (peekb(0x0F59) == 3)
							{
								cy(30);
								const uint16_t r = jsr(0x58D0, 0xB83C);
								cy(24);
								if (r == 0)
								{
									cy(30);
									jsr(0x944C, 0xB844);
									cy(102);
									T.r[0] = 0x1800;
									set_bank_a(0x1800);
									jsr(0x5A50, 0xB854);
									cy(12);
									usual = false;
								}
							}
							if (usual)
							{
								cy(30);
								jsr(0x94DC, 0xB85A);
								cy(30);
								jsr(0x944C, 0xB85E);
							}
							cy(30);
							jsr(0x979A, 0xB862);
							cy(12);
						}
						cy(30);
						jsr(0x9A32, 0xB86C);
						cy(174);
						T.r[0] = 0x1800;
						set_bank_a(0x1800);
						T.r[0] = 0x1C00;
						set_bank_b(0x1C00);
						jsr(0x7854, 0xB888);
						cy(102);
						T.r[0] = 0x1800;
						set_bank_a(0x1800);
						jsr(0x4A2A, 0xB898);
						cy(12);
					}
					cy(102);
					T.r[0] = 0x1800;
					set_bank_a(0x1800);
					jsr(0x5166, 0xB8C6);
				}
			}
		}
		else
			cy(18);
	}
	irq.leave(0xB8E6);
}

// ---------------------------------------------------------------- numbers

// F:BA96 / F:BA9A  print_number(int separators, int lead, int digits, long value): print the
// value (high word, low word) with the given number of digits (at most 10; only the low digits
// are kept) at the text cursor, with the small font (F:8C12) or the large one (F:BA9A: F:8B52,
// digits F:8C56).  Leading zeros: lead = 0 drops them and pads with blanks at the end (left
// aligned); lead = k blanks up to digits - k of them in place.  separators != 0 puts ',' after
// the thousands, millions and billions digits.  The cursor routines get the original's
// registers; R0 is what the last of them returns.
constexpr uint16_t POW10 = 0xBA6A;                     // 10^n at 0xBA6A + 4n (high word first)

uint16_t print_number(const Args &a, uint16_t font)
{
	CFrame frame;
	cy(33 + 177);                                       // JSR R1,F162 (PC-relative) and its prologue
	frame.save_regs(SAVE_R2_R5, F162_RET);
	const uint16_t H = T.r[6];
	auto arg = [&](int off) { return rw((uint16_t)(H + off)); };
	uint16_t r0 = font, r1 = T.r[1], r2 = T.r[2], r3 = T.r[3], r4 = T.r[4], r5 = T.r[5];
	// the character routines get the original's registers
	auto put = [&](uint16_t routine, uint16_t ret, uint16_t c) {
		cy(33);                                         // JSR PC,... (PC-relative)
		T.r[0] = c;
		T.r[1] = r1; T.r[2] = r2; T.r[3] = r3; T.r[4] = r4; T.r[5] = r5;
		jsr(routine, ret);
		r0 = T.r[0];
		r1 = T.r[1]; r2 = T.r[2]; r3 = T.r[3]; r4 = T.r[4]; r5 = T.r[5];
	};
	// DECB R4 and the condition codes BGT / BGE test
	bool n = false, z = false, v = false;
	auto decb_r4 = [&]() {
		const uint8_t b = (uint8_t)r4, d = (uint8_t)(b - 1);
		r4 = (uint16_t)((r4 & 0xFF00) | d);
		n = (d & 0x80) != 0;
		z = d == 0;
		v = b == 0x80;
	};
	// [R3:R2] -= the power of ten at R1 while it does not borrow (F:BB0A / F:BB32 loops); the
	// failing subtraction is undone.  extra: cycles of the INC R0 in front of each try
	auto divide = [&](int extra) {
		uint16_t q = 0;
		for (;;)
		{
			cy(extra + 27 + 12 + 12);                   // (INC R0,) SUB 2(R1),R2; SBC R3; BCS
			if (extra) q++;
			const uint16_t lo = peek((uint16_t)(r1 + 2)), hi = peek(r1);
			const bool b1 = r2 < lo;
			r2 = (uint16_t)(r2 - lo);
			bool b2 = false;
			if (b1)
			{
				b2 = r3 == 0;
				r3 = (uint16_t)(r3 - 1);
			}
			if (b2) break;
			cy(18 + 12);                                // SUB (R1),R3; BCC
			const bool b3 = r3 < hi;
			r3 = (uint16_t)(r3 - hi);
			if (!b3) continue;
			cy(18);                                     // ADD (R1),R3
			r3 = (uint16_t)(r3 + hi);
			break;
		}
		cy(27 + 12);                                    // ADD 2(R1),R2; ADC R3
		const uint16_t lo = peek((uint16_t)(r1 + 2));
		const bool c = (uint32_t)r2 + lo > 0xFFFF;
		r2 = (uint16_t)(r2 + lo);
		r3 = (uint16_t)(r3 + (c ? 1 : 0));
		return q;
	};

	cy(24);                                             // MOV R0,-(SP)
	push(r0);
	cy(33);
	T.r[0] = r0; T.r[1] = r1; T.r[2] = r2; T.r[3] = r3; T.r[4] = r4; T.r[5] = r5;
	jsr(0x8068, 0xBAA8);
	r0 = T.r[0];
	cy(18 + 27 + 27 + 27);                              // MOV (SP)+,R4; MOV args; BISB
	r4 = pop();
	r2 = arg(0x16);
	r3 = arg(0x14);
	r4 = (uint16_t)((r4 & 0xFF00) | (uint8_t)(r4 | arg(0x12)));
	cy(18 + 12);                                        // CMPB #10,R4; BCC
	if ((uint8_t)r4 > 10)
	{
		cy(12 + 18);
		r4 = (uint16_t)((r4 & 0xFF00) | 10);
	}
	cy(12 + 27 + 12);                                   // MOVB R4,R5; MOVB lead,R0; BNE
	r5 = sext8((uint8_t)r4);
	r0 = sext8((uint8_t)arg(0x10));
	if (!(uint8_t)r0)
	{
		cy(18);
		r0 = 0x8001;
	}
	cy(12 + 12 + 12 + 12 + 18);                         // SUB R0,R5; MOVB R4,R1; ASL; ASL; ADD
	r5 = (uint16_t)(r5 - r0);
	r1 = (uint16_t)((uint16_t)(sext8((uint8_t)r4) << 2) + 0xBA6A);
	cy(18 + 12);                                        // CMP (R1),R3; BHI
	bool reduce;
	if (peek(r1) > r3)
		reduce = false;
	else
	{
		cy(12);                                         // BNE
		if (peek(r1) != r3)
			reduce = true;
		else
		{
			cy(27 + 12);                                // CMP 2(R1),R2; BHI
			reduce = !(peek((uint16_t)(r1 + 2)) > r2);
		}
	}
	if (reduce)
	{
		// too many digits: the value is reduced modulo 10^digits (F:BAE6-BB20)
		cy(12 + 12);                                    // CLRB R5; MOV R1,R0
		r5 &= 0xFF00;
		r0 = r1;
		bool at_top = false;
		for (;;)
		{
			cy(18 + 12);                                // CMP #0xBA8E,R1; BCS
			if (0xBA8E < r1) break;
			cy(12);                                     // BEQ
			if (r1 == 0xBA8E)
			{
				at_top = true;
				break;
			}
			cy(18 + 18 + 12);                           // ADD #4,R1; CMP (R1),R3; BHI
			r1 = (uint16_t)(r1 + 4);
			if (peek(r1) > r3) break;
			cy(12);                                     // BNE
			if (peek(r1) != r3) continue;
			cy(27 + 12);                                // CMP 2(R1),R2; BLOS
			if (peek((uint16_t)(r1 + 2)) <= r2) continue;
			break;
		}
		for (;;)
		{
			if (!at_top)
			{
				cy(18 + 12 + 12);                       // SUB #4,R1; CMP R1,R0; BCS
				r1 = (uint16_t)(r1 - 4);
				if (r1 < r0)
				{
					cy(18);                             // ADD #4,R1
					r1 = (uint16_t)(r1 + 4);
					break;
				}
			}
			at_top = false;
			divide(0);
			cy(12);                                     // BR F:BB02
		}
	}
	cy(12);                                             // DECB R4
	decb_r4();
	for (;;)
	{
		cy(12);                                         // BGT
		bool last = false, blank = false;
		if (!z && n == v)
		{
			cy(18 + 12);                                // SUB #4,R1; CLR R0
			r1 = (uint16_t)(r1 - 4);
			const uint16_t q = divide(12);
			cy(12 + 12);                                // DEC R0; BNE
			r0 = (uint16_t)(q - 1);
			if (r0)
			{
				cy(12);                                 // CLRB R5
				r5 &= 0xFF00;
			}
			else
			{
				cy(12 + 12);                            // DECB R5; BMI
				r5 = (uint16_t)((r5 & 0xFF00) | (uint8_t)(r5 - 1));
				if (r5 & 0x0080)
				{
					cy(12);                             // CLRB R5
					r5 &= 0xFF00;
				}
				else
				{
					cy(12 + 12);                        // TST R5; BMI
					if (r5 & 0x8000) goto next;         // a dropped leading zero
					cy(18 + 12 + 12);                   // MOV #0,R0; TST R4; BPL
					r0 = 0;
					if (r4 & 0x8000)
					{
						put(0x8B52, 0xBB60, 0);
						cy(27 + 12);                    // TST separators; BNE
						if (!arg(0x0E))
						{
							cy(12);                     // BR F:BBBA
							goto count;
						}
					}
					else
						put(0x8C12, 0xBB6C, 0);
					cy(18 + 12);                        // MOV #0,R0; BR F:BB90
					r0 = 0;
					blank = true;
				}
			}
		}
		else
		{
			cy(12 + 12);                                // MOV R2,R0; BR F:BB74
			r0 = r2;
			last = true;
		}
		if (!blank)
		{
			cy(18 + 12 + 12);                           // ADD #'0',R0; TST R4; BPL
			r0 = (uint16_t)(r0 + 0x30);
			if (r4 & 0x8000)
			{
				put(0x8C56, 0xBB80, r0);
				cy(12);                                 // BR
			}
			else
				put(0x8C12, 0xBB86, r0);
			cy(27 + 12);                                // TST separators; BEQ
			if (!arg(0x0E)) goto count;
			cy(18);                                     // MOV #',',R0
			r0 = 0x002C;
		}
		(void)last;
		cy(18 + 12);                                    // CMP R1,#0xBA76; BEQ
		if (r1 != 0xBA76)
		{
			cy(18 + 12);
			if (r1 != 0xBA82)
			{
				cy(18 + 12);
				if (r1 != 0xBA8E) goto count;
			}
		}
		cy(12 + 12);                                    // TST R4; BPL
		if (r4 & 0x8000)
		{
			cy(12 + 12);                                // TST R0; BNE
			if (!r0)
			{
				cy(36);
				poke(0x0186, (uint16_t)(peek(0x0186) - 2));
			}
			put(0x8B52, 0xBBB4, r0);
			cy(12);                                     // BR
		}
		else
			put(0x8C12, 0xBBBA, r0);
	count:
		cy(18);                                         // ADD #0x100,R5
		r5 = (uint16_t)(r5 + 0x0100);
	next:
		cy(12 + 12);                                    // DECB R4; BGE
		decb_r4();
		if (n != v) break;
	}
	bool c = false;
	cy(12 + 12);                                        // TST R5; BPL
	if (r5 & 0x8000)
	{
		cy(27 + 12 + 18 + 12 + 12);                     // MOV digits,R1; SWAB; BIC; SUB; BEQ
		r1 = arg(0x12);
		r5 = (uint16_t)(((r5 >> 8) | (r5 << 8)) & 0x003F);
		c = r1 < r5;
		r1 = (uint16_t)(r1 - r5);
		if (r1)
			for (;;)
			{
				cy(18 + 12 + 12);                       // MOV #0,R0; TST R4; BPL
				r0 = 0;
				if (r4 & 0x8000)
				{
					put(0x8B52, 0xBBE0, 0);
					cy(12);
				}
				else
					put(0x8C12, 0xBBE6, 0);
				c = fC();
				cy(12 + 12);                            // DEC R1; BGT
				const uint16_t old = r1;
				r1 = (uint16_t)(r1 - 1);
				if (!((int16_t)r1 > 0 && old != 0x8000)) break;
			}
	}
	cy(21);                                             // RTS
	T.r[1] = r1; T.r[2] = r2; T.r[3] = r3; T.r[4] = r4; T.r[5] = r5;
	set_c(c);
	return r0;
}

uint16_t print_number_small(const Args &a)
{
	At here(0xBA96);
	cy(12 + 12);                                        // CLR R0; BR
	return print_number(a, 0x0000);
}
uint16_t print_number_large(const Args &a)
{
	At here(0xBA9A);
	cy(18);                                             // MOV #0x8000,R0
	return print_number(a, 0x8000);
}

// F:BBEC (registers): the view rectangle 0x0996-0x099C is copied to 0x0224-0x022A (x / y
// first) and the next eight map slots 0x09AC (wrapping from 0x09B0 to 0x0E98) are looked at:
// those with a type (0x09B8) get F:BCB8 with 0x09AA = the slot.  Banks 0x1000 / 0x1400.
void check_some_map_slots()
{
	At here(0xBBEC);
	cy(18 + 27 + 27 + 18 + 27 + 27 + 39 + 39 + 39 + 39 + 30);   // F:BBEC-BC1C
	T.r[0] = 0x1000;
	set_bank_a(0x1000);
	T.r[0] = 0x1400;
	set_bank_b(0x1400);
	poke(0x0224, peek(0x099A));
	poke(0x0226, peek(0x099C));
	poke(0x0228, peek(0x0996));
	poke(0x022A, peek(0x0998));
	push(0x0008);
	do
	{
		charge(0xBC20, 27 + 36 + 12);                   // INC; CMP; BLE
		poke(0x09AC, (uint16_t)(peek(0x09AC) + 1));
		if ((int16_t)peek(0x09AC) > (int16_t)peek(0x09B0))
		{
			cy(27 + 39);
			poke(0x09AC, 0);
			pokeb(0x09AC, peekb(0x0E98));
		}
		cy(39 + 24 + 27 + 12);                          // F:BC36-BC44
		poke(0x09AA, peek(0x09AC));
		T.r[0] = peek(0x09AA);
		if (peekb((uint16_t)(0x09B8 + T.r[0])))
		{
			cy(30);
			jsr(0xBCB8, 0xBC4A);
		}
		cy(21 + 12);                                    // DEC (SP); BNE
		poke(T.r[6], (uint16_t)(peek(T.r[6]) - 1));
	} while (peek(T.r[6]));
	cy(18 + 21);
	fl::tst(pop());
}

// F:CA0C (registers): a map object in mode 3 is animated: unless the skater's object
// (0x022C) has bit 0 of 0x04B0 set, a record (0x09B6, bank A) with bit 4 of its first byte
// looks for the object of this slot (F:CDD0 find_slot_with(0x09AA, 0x0AF8[0x09AA])) and
// restarts its animation 5 (F:C98E).
void animate_map_object()
{
	At here(0xCA0C);
	cy(24 + 33 + 12);
	T.r[0] = peek(0x022C);
	uint8_t bits = peekb((uint16_t)(0x04B0 + T.r[0])) & 0x01;
	fl::movb(bits);
	if (bits)
	{
		cy(21);
		return;
	}
	cy(24 + 33 + 12);
	T.r[0] = peek(0x09B6);
	bits = peekb((uint16_t)(0x4000 + T.r[0])) & 0x10;
	fl::movb(bits);
	if (!bits)
	{
		cy(21);
		return;
	}
	cy(24 + 12 + 39 + 36 + 30);                         // F:CA24-CA32
	T.r[0] = (uint16_t)(peek(0x09AA) << 1);
	push(peek((uint16_t)(0x0AF8 + T.r[0])));
	push(peek(0x09AA));
	jsr(0xCDD0, 0xCA36);
	cy(18 + 12);
	if (T.r[0] != 0xFFFF)
	{
		cy(21 + 30 + 30);
		poke(T.r[6], T.r[0]);
		push(0x0005);
		jsr(0xC98E, 0xCA46);
		cy(18);
		T.r[6] = (uint16_t)(T.r[6] + 2);
	}
	cy(24 + 21);
	const uint16_t a = pop(), b = pop();
	fl::cmp(a, b);                                      // CMP (SP)+,(SP)+
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
