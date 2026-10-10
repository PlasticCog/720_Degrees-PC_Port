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

namespace logic {
namespace {

using namespace t11rt;

uint16_t sext8(uint8_t v) { return (uint16_t)(int16_t)(int8_t)v; }
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
// but the return address on the stack is the original's (the translated JSR does the same)
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
	return T.r[0];
}

// a C routine's frame: the stack pointer and R1-R5 are given back at the end
struct CFrame {
	uint16_t sp, regs[6];
	CFrame() : sp(T.r[6])
	{
		for (int i = 1; i < 6; i++) regs[i] = T.r[i];
	}
	~CFrame()
	{
		T.r[6] = sp;
		for (int i = 1; i < 6; i++) T.r[i] = regs[i];
	}
	CFrame(const CFrame &) = delete;
	CFrame &operator=(const CFrame &) = delete;
	// JSR R1,@#F138/F144/F152/F162: R1 and R2..R(1+n) saved, then the helper's return address
	void save_regs(int n, uint16_t helper_ret)
	{
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
	const uint8_t pending = peekb(0x0992);
	fl::tstb(pending);
	if (!pending) return;
	T.r[2] = 4;
	do
	{
		const uint8_t slot = peekb((uint16_t)(0x0989 + T.r[2]));
		T.r[1] = sext8(slot);
		fl::movb(slot);
		if (!slot) continue;
		pokeb((uint16_t)(0x04D0 + T.r[1]), peekb((uint16_t)(0x098D + T.r[2])));
		T.r[4] = fl::add(0x0490, T.r[1]);
		if (peekb(T.r[4]) & 0x04)
		{
			T.r[3] = (uint16_t)(T.r[1] + 0x20);
			push(1);
			push(T.r[3]);
			jsr(0xACE6, 0xB06E);
			ww(T.r[6], 1);
			push(T.r[3]);
			jsr(0xAE02, 0xB078);
			T.r[6] = fl::add(T.r[6], 6);
		}
		const uint8_t f = (uint8_t)(peekb(T.r[4]) & ~0x40);
		pokeb(T.r[4], f);
		f_nzv(nzb(f & 0x20));
		if (!(f & 0x20))
		{
			pokeb(0x0992, fl::decb(peekb(0x0992)));
			pokeb((uint16_t)(0x0989 + T.r[2]), 0);
			fl::clr();
		}
	} while (--T.r[2]);
}

// top edge used by F:B092 / F:B120: 0x4F80 - the height table entry 0x5BCC of type R1
uint16_t type_height(uint16_t type) { return peek((uint16_t)(0x5BCC + 2 * type)); }

// F:B092 (assembly): first chains of objects with flag 0x02 (shown): taken out of the display
// list unless the object (flag 0x80) is on screen (y >= 0x4F80 - height, x >= 0xD400 signed);
// objects not shown but on screen with pieces are put in.  Slots 0x0994 down to 0.
void update_chains_a()
{
	const uint8_t top = peekb(0x0994);
	T.r[3] = sext8(top);
	fl::movb(top);
	if (lt()) return;
	do
	{
		T.r[1] = peekb((uint16_t)(0x0370 + T.r[3]));
		T.r[2] = peekb((uint16_t)(0x0490 + T.r[3]));
		uint16_t target = 0;
		fl::tstb((uint8_t)T.r[2]);
		if (T.r[2] & 0x02)
		{
			target = 0xACE6;
			if (T.r[2] & 0x80)
			{
				T.r[4] = (uint16_t)(T.r[3] * 2);
				T.r[0] = (uint16_t)-(uint16_t)(type_height(T.r[1]) - 0x4F80);
				fl::cmp(peek((uint16_t)(0x0670 + T.r[4])), T.r[0]);
				if (!lt())
				{
					T.r[0] = (uint16_t)(T.r[3] * 2);
					fl::cmp(peek((uint16_t)(0x0630 + T.r[0])), 0xD400);
					if (!lt()) target = 0;
				}
			}
		}
		else if (T.r[2] & 0x80)
		{
			const uint8_t pieces = peekb((uint16_t)(0x04D0 + T.r[3]));
			fl::tstb(pieces);
			if (pieces)
			{
				T.r[4] = (uint16_t)(T.r[3] * 2);
				T.r[0] = (uint16_t)-(uint16_t)(type_height(T.r[1]) - 0x4F80);
				fl::cmp(peek((uint16_t)(0x0670 + T.r[4])), T.r[0]);
				if (gt())
				{
					T.r[0] = (uint16_t)(T.r[3] * 2);
					fl::cmp(peek((uint16_t)(0x0630 + T.r[0])), 0xD400);
					if (!lt()) target = 0xAE02;
				}
			}
		}
		if (target)
		{
			push(0);
			push(T.r[3]);
			jsr(target, target == 0xACE6 ? 0xB0DA : 0xB118);
			fl::cmp(rw(T.r[6]), rw((uint16_t)(T.r[6] + 2)));
			T.r[6] = (uint16_t)(T.r[6] + 4);
		}
		T.r[3] = fl::dec(T.r[3]);
	} while (!lt());
}

// F:B120 (assembly): the same for the second chains (slot + 0x20) with flag 0x04: visible
// when y <= 0x5080 (unsigned), y + height >= 0x2000 (unsigned) and x >= 0xD400 (signed)
void update_chains_b()
{
	const uint8_t top = peekb(0x0994);
	T.r[3] = sext8(top);
	fl::movb(top);
	if (lt()) return;
	do
	{
		T.r[1] = peekb((uint16_t)(0x0370 + T.r[3]));
		T.r[2] = peekb((uint16_t)(0x0490 + T.r[3]));
		uint16_t target = 0;
		const uint16_t w = (uint16_t)(T.r[3] * 2);
		fl::tstb((uint8_t)T.r[2]);
		if (T.r[2] & 0x04)
		{
			target = 0xACE6;
			if (T.r[2] & 0x80)
			{
				T.r[0] = w;
				fl::cmp(peek((uint16_t)(0x0670 + w)), 0x5080);
				if (!(!fC() && !fZ()))
				{
					T.r[4] = w;
					T.r[0] = (uint16_t)(T.r[1] * 2);
					T.r[4] = (uint16_t)(peek((uint16_t)(0x0670 + w)) + peek((uint16_t)(0x5BCC + T.r[0])));
					fl::cmp(T.r[4], 0x2000);
					if (!fC())
					{
						T.r[0] = w;
						fl::cmp(peek((uint16_t)(0x0630 + w)), 0xD400);
						if (!lt()) target = 0;
					}
				}
			}
		}
		else if (T.r[2] & 0x80)
		{
			const uint8_t pieces = peekb((uint16_t)(0x04D0 + T.r[3]));
			fl::tstb(pieces);
			if (pieces)
			{
				T.r[0] = w;
				fl::cmp(peek((uint16_t)(0x0670 + w)), 0x5080);
				if (fC() || fZ())
				{
					T.r[4] = w;
					T.r[0] = (uint16_t)(T.r[1] * 2);
					T.r[4] = (uint16_t)(peek((uint16_t)(0x0670 + w)) + peek((uint16_t)(0x5BCC + T.r[0])));
					fl::cmp(T.r[4], 0x2000);
					if (!fC())
					{
						T.r[0] = w;
						fl::cmp(peek((uint16_t)(0x0630 + w)), 0xD400);
						if (gt()) target = 0xAE02;
					}
				}
			}
		}
		if (target)
		{
			push(1);
			push((uint16_t)(T.r[3] + 0x20));
			jsr(target, target == 0xACE6 ? 0xB178 : 0xB1C0);
			fl::cmp(rw(T.r[6]), rw((uint16_t)(T.r[6] + 2)));
			T.r[6] = (uint16_t)(T.r[6] + 4);
		}
		T.r[3] = fl::dec(T.r[3]);
	} while (!lt());
}

// F:B1EA (assembly): rebuild the sprite pieces of the chains in display list 0x0993 (0 or 1),
// following the links 0x0808 from the head 0x0988+list (current link in 0x0FEE).  Objects
// whose flags (masked by 0xFD7E+list) ask for it get their pieces from the frame data
// (0x03B0 points to it, bank 0x3000/0x3400): x offset, y offset, then link/picture words, with
// the palette byte 0x0470; the others only move their pieces by the screen-y delta 0x05F0.
// Objects with a flag of 0xFD80+list are skipped.
void build_sprite_pieces()
{
	const uint16_t list = sext8(peekb(0x0993));
	const uint16_t sp = (uint16_t)(T.r[6] - 2);         // the list is kept on the stack
	ww(sp, list);
	T.r[1] = list;
	T.r[0] = sext8(peekb((uint16_t)(0x0988 + list)));
	poke(0x0FEE, T.r[0]);
	uint16_t link = T.r[0];
	while ((int16_t)link >= 0)
	{
		T.r[3] = peek(0x0FEE) & 0x001F;
		T.r[2] = peekb((uint16_t)(0x0490 + T.r[3]));
		T.r[5] = list;
		if (!(peekb((uint16_t)(0xFD80 + list)) & T.r[2]))
		{
			ww((uint16_t)(sp - 2), (uint16_t)(list + 0xFD7E));
			T.r[5] = (uint16_t)~peekb((uint16_t)(0xFD7E + list));
			T.r[2] &= (uint16_t)~T.r[5];
			T.r[5] = (uint16_t)(peek(0x0FEE) << 1);
			T.r[4] = peek((uint16_t)(0x0888 + T.r[5]));
			T.r[0] = peekb((uint16_t)(0x04D0 + T.r[3]));
			T.r[5] = (uint16_t)(T.r[3] << 1);
			if (T.r[2])
			{
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
					next_row();
					poke(0x0FF8, (uint16_t)(peek(0x0FF8) + peek(0x0FF4)));
				}
				pokeb(0x0FFB, peekb((uint16_t)(0x0470 + T.r[3])));
				T.r[4] = (uint16_t)(T.r[4] + 0x3800);
				poke(0x0FF0, 0);
				T.r[2] = 0;
				for (;;)
				{
					if ((int16_t)peek(0x0FF0) < 0)
					{
						poke((uint16_t)(T.r[4] + 2), 0);
						T.r[2] = peek((uint16_t)(T.r[4] + 6));
					}
					else
					{
						if ((int16_t)peek(0x0FF6) >= 0)
						{
							next_row();
							poke(0x0FF8, (uint16_t)(peek(0x0FF8) + peek(0x0FF4)));
						}
						pokeb(0x0FF2, peekb(T.r[1]++));
						pokeb(0x0FF3, peekb(T.r[1]++));
						poke(0x0FF0, peek(0x0FF2));
						poke(0x0FF2, peek(0x0FF2) & 0x7FFF);
						if (T.r[2]) poke(0x0FF2, peek(0x0FF2) | 0x8000);
						poke(T.r[4], peek(0x0FF8));
						T.r[4] = (uint16_t)(T.r[4] + 2);
						poke(T.r[4], peek(0x0FF2));
						T.r[4] = (uint16_t)(T.r[4] + 4);
						T.r[2] = (uint16_t)((peek(T.r[4]) & 0x0FFF) | peek(0x0FFA));
						poke(T.r[4], T.r[2]);
					}
					T.r[5] = T.r[2] & 0x07F8;
					T.r[4] = (uint16_t)(T.r[5] + 0x3800);
					if (--T.r[0] == 0) break;
				}
			}
			else
			{
				T.r[2] = (uint16_t)(peek((uint16_t)(0x0630 + T.r[5])) + peek((uint16_t)(0x0530 + T.r[5])));
				poke((uint16_t)(0x3804 + T.r[4]), T.r[2]);
				T.r[2] = peek((uint16_t)(0x05F0 + T.r[5]));
				do
				{
					poke((uint16_t)(0x3800 + T.r[4]), (uint16_t)(peek((uint16_t)(0x3800 + T.r[4])) + T.r[2]));
					T.r[4] = peek((uint16_t)(0x3806 + T.r[4])) & 0x07F8;
				} while (--T.r[0]);
			}
		}
		ww((uint16_t)(sp - 2), (uint16_t)(peek(0x0FEE) + 0x0808));
		T.r[5] = sext8(peekb((uint16_t)(peek(0x0FEE) + 0x0808)));
		poke(0x0FEE, T.r[5]);
		link = T.r[5];
	}
	fl::tst(list);
}

// F:B378 (assembly): move the objects 0x0994 down to 0: animation (rate 0x0770 frames per step,
// counter 0x0790, step count 0x07B0; the frame list at 0x03B0 ends with a negative link; with
// flag 0x01 the frame data in bank 0x3000/0x3400 gives the velocity), position += velocity
// for slots from 4 on, then screen position and screen-y delta.  The two stack words it
// reserves end compared, as the original does.
void move_objects()
{
	const uint16_t at_rate = (uint16_t)(T.r[6] - 4), at_scratch = (uint16_t)(T.r[6] - 2), at_byte = (uint16_t)(T.r[6] - 6);
	const uint8_t top = peekb(0x0994);
	T.r[5] = sext8(top);
	if (!(top & 0x80))
		for (;;)
		{
			const uint16_t i = T.r[5];
			if (peekb((uint16_t)(0x0370 + i)))
			{
				T.r[4] = (uint16_t)(i << 1);
				const uint16_t w = T.r[4];
				T.r[3] = peek((uint16_t)(0x04F0 + w));
				T.r[2] = peek((uint16_t)(0x0570 + w));
				const uint16_t rate_ptr = (uint16_t)(0x0770 + i);
				ww(at_rate, rate_ptr);
				const uint8_t rate = peekb(rate_ptr);
				if (rate && !(rate & 0x80))
				{
					T.r[0] = (uint16_t)(0x0790 + i);
					if (peekb(T.r[0]))
						pokeb(T.r[0], (uint8_t)(peekb(T.r[0]) - 1));
					else
					{
						pokeb(T.r[0], peekb(rate_ptr));
						T.r[1] = (uint16_t)(peek((uint16_t)(0x03B0 + w)) + 2);
						pokeb((uint16_t)(0x07B0 + i), (uint8_t)(peekb((uint16_t)(0x07B0 + i)) + 1));
						if ((int16_t)peek(T.r[1]) < 0)
						{
							T.r[1] = peek(T.r[1]) & 0x7FFF;
							pokeb((uint16_t)(0x07B0 + i), 0);
							if (peekb((uint16_t)(0x04B0 + i)) & 0x20) pokeb(rate_ptr, 0);
						}
						if (peekb((uint16_t)(0x0490 + i)) & 0x01)
						{
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
								ww(at_byte, step);
								poke((uint16_t)(0x0430 + w), (uint16_t)(peek((uint16_t)(0x0430 + w)) - step));
								T.r[0] = peekb(rate_ptr);
								poke((uint16_t)(0x0430 + w), (uint16_t)(peek((uint16_t)(0x0430 + w)) + T.r[0]));
							}
							hw_banks(0, 0x0400);
						}
						poke((uint16_t)(0x03B0 + w), T.r[1]);
						pokeb((uint16_t)(0x0490 + i), peekb((uint16_t)(0x0490 + i)) | 0x18);
					}
				}
				if ((int16_t)i < 4)
					pokeb((uint16_t)(0x0490 + i), peekb((uint16_t)(0x0490 + i)) | 0x18);
				else
				{
					poke((uint16_t)(0x06F0 + w), (uint16_t)(peek((uint16_t)(0x06F0 + w)) + T.r[3]));
					poke((uint16_t)(0x0730 + w), (uint16_t)(peek((uint16_t)(0x0730 + w)) + T.r[2]));
					poke((uint16_t)(0x06B0 + w), (uint16_t)(peek((uint16_t)(0x06B0 + w)) + peek((uint16_t)(0x05B0 + w))));
				}
				T.r[3] = (uint16_t)(peek((uint16_t)(0x06F0 + w)) - peek(0x01A8));
				poke((uint16_t)(0x0630 + w), T.r[3]);
				T.r[2] = (uint16_t)(peek((uint16_t)(0x0730 + w)) + peek(0x01AA) + peek((uint16_t)(0x06B0 + w)));
				T.r[0] = T.r[2] & ~0x0007;
				T.r[1] = peek((uint16_t)(0x0670 + w)) & ~0x0007;
				T.r[0] = (uint16_t)(T.r[0] - T.r[1]);
				poke((uint16_t)(0x05F0 + w), T.r[0]);
				poke((uint16_t)(0x0670 + w), T.r[2]);
			}
			T.r[5]--;
			if ((int16_t)T.r[5] < 0) break;
		}
	fl::cmp(rw(at_rate), rw(at_scratch));                // CMP (SP)+,(SP)+
}

// ---------------------------------------------------------------- objects

// F:B4B6  show_object(int slot): make slot (bit 7: without sprite pieces) visible: the highest
// used slot 0x0994 rises to it (or drops to the highest slot still in use), the piece count
// 0x04D0 comes from the type table 0x5C96, the screen position from the world position and the
// scroll; flag 0x80 of 0x0490.  0x0FFC = 0xFF when interrupts are enabled.  Returns slot * 2.
uint16_t show_object(const Args &a)
{
	CFrame frame;
	frame.save_regs(SAVE_R2_R4, F152_RET);
	const uint16_t saved_a = peek(0x01D2), saved_b = peek(0x01D4);
	set_bank_a(0);
	set_bank_b(0x0400);
	const uint16_t arg = a.u(0), slot = arg & ~0x0080;
	if ((int16_t)slot >= (int16_t)sext8(peekb(0x0994)))
		pokeb(0x0994, (uint8_t)slot);
	else
		for (int16_t i = (int16_t)sext8(peekb(0x0994)); i >= 0; i--)
			if (peekb((uint16_t)(0x0370 + i)) || (peekb((uint16_t)(0x0490 + i)) & 0x86))
			{
				pokeb(0x0994, (uint8_t)i);
				break;
			}
	pokeb((uint16_t)(0x04D0 + slot), arg == slot ? peekb((uint16_t)(0x5C96 + peekb((uint16_t)(0x0370 + slot)))) : 0);
	const uint16_t w = (uint16_t)(slot << 1);
	poke((uint16_t)(0x0630 + w), (uint16_t)(peek((uint16_t)(0x06F0 + w)) - peek(0x01A8)));
	const uint16_t sy = peek(0x01AA), y = peek((uint16_t)(0x0730 + w));
	poke((uint16_t)(0x0670 + w), (uint16_t)(sy + y));
	set_c(carry(sy, y));
	poke((uint16_t)(0x05F0 + w), 0);
	pokeb((uint16_t)(0x0490 + slot), peekb((uint16_t)(0x0490 + slot)) | 0x80);
	if (!(T.psw & 0xE0)) pokeb(0x0FFC, 0xFF);
	restore_banks(saved_a, saved_b);
	return w;
}

// F:B602  queue_piece_count(int type, int slot): with interrupts off, put slot and its piece
// count (type table 0x5C96) in the first free entry of the queue F:B03C applies (0x0989+i,
// i = 4..1), flag the slot (0x60) and count the entry (0x0992).  Interrupts end enabled.
uint16_t queue_piece_count(const Args &a)
{
	CFrame frame;
	frame.save_regs(SAVE_R2_R5, F162_RET);
	const uint16_t saved_a = peek(0x01D2), saved_b = peek(0x01D4);
	set_bank_a(0);
	set_bank_b(0x0400);
	mtps(0xE0);
	uint16_t r0 = 0x0400;
	for (uint16_t i = 4; i; i--)
		if (!peekb((uint16_t)(0x0989 + i)))
		{
			r0 = a.u(1);
			pokeb((uint16_t)(0x0989 + i), (uint8_t)r0);
			pokeb((uint16_t)(0x098D + i), peekb((uint16_t)(0x5C96 + a.u(0))));
			pokeb((uint16_t)(0x0490 + r0), peekb((uint16_t)(0x0490 + r0)) | 0x60);
			pokeb(0x0992, (uint8_t)(peekb(0x0992) + 1));
			break;
		}
	mtps(0);
	restore_banks(saved_a, saved_b);
	return r0;
}

// F:BA2E: with interrupts off, clear the text layer (F:8D3E) and the object tables
// 0x0370-0x0994, park the last sprite (0x3BFE = 0x03F8) and empty both display lists
// (0x0988/0x0989 = 0xFF).  Interrupts end enabled.  Returns 0x0995.
uint16_t clear_objects_and_text(const Args &)
{
	CFrame frame;
	push(T.r[1]);
	T.r[1] = 0x00E0;
	mtps(0xE0);
	jsr(0x8068, 0xBA3A);
	jsr(0x8D3E, 0xBA3E);
	for (uint16_t p = 0x0370; p != 0x0995; p++) pokeb(p, 0);
	poke(0x3BFE, 0x03F8);
	poke(0x0988, 0xFFFF);
	mtps(0);
	return 0x0995;
}

// F:C98E  restart_object_animation(int rate, int n): the object of map object n (0x09B8, the
// argument is replaced by the slot) restarts its animation: next frame of the list 0x03B0 (a
// negative link is followed), redraw flags 0x18, rate and counter (0x0770, 0x0790) = rate.
// Not for the object queued in 0x0E6A.  Returns the slot.
uint16_t restart_object_animation(const Args &a)
{
	CFrame frame;
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
		const uint16_t p = (uint16_t)(0x03B0 + 2 * slot), v = peek(p);
		poke(p, (uint16_t)(v + 2));
		c = carry(v, 2);
		const uint16_t f = peek(peek(p));
		if ((int16_t)f < 0)
		{
			poke(p, f);
			poke(p, peek(p) & 0x7FFF);
		}
		pokeb((uint16_t)(0x0490 + slot), peekb((uint16_t)(0x0490 + slot)) | 0x18);
		const uint8_t rate = (uint8_t)a.u(0);
		pokeb((uint16_t)(0x0790 + slot), rate);
		pokeb((uint16_t)(0x0770 + slot), rate);
	}
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
	CFrame frame;
	frame.save_regs(SAVE_R2_R3, F144_RET);
	const uint16_t n = peek(0x09AA);
	const uint16_t d = peek((uint16_t)(0x0AF8 + 2 * n)) & 0x7FFF;
	const bool dynamic = (int16_t)n >= 0x40;
	const uint16_t entry = (uint16_t)((dynamic ? 0x7D00 : 0x7A00) + ((d & 0x01FF) << 1));
	const uint16_t def = (uint16_t)(field9(d) + peek(entry));
	poke(0x09B6, def);
	if (dynamic)
	{
		poke(0x09B6, (uint16_t)(def + 0x0338));
		set_c(carry(def, 0x0338));
	}
	else
	{
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
	CFrame frame;
	frame.save_regs(SAVE_R2_R3, F144_RET);
	const uint16_t d = peek(peek(0x09B2));
	const bool dynamic = (int16_t)peek(0x09AA) >= 0x40;
	const uint16_t cell = peek((uint16_t)((dynamic ? 0x7D00 : 0x7A00) + ((d & 0x01FF) << 1)));
	if ((int16_t)cell < 0)
	{
		set_c(false);
		return cell;
	}
	uint16_t def = (uint16_t)(field9(d) + cell);
	if (dynamic) def = (uint16_t)(def + 0x0338);
	poke(0x09B4, def);
	const uint8_t prop = peekb((uint16_t)(0x4560 + def));
	uint16_t r;
	if (prop & 0x0F)
	{
		const uint16_t holder = peekb((uint16_t)(0x0E6D + (prop & 0x0F)));
		const uint16_t self = (uint16_t)(peek(0x09AA) + 1);
		if (!holder || holder == self)
		{
			r = 0;
			set_c(false);
		}
		else
		{
			r = 1;
			set_c(holder < self);
		}
	}
	else
	{
		const uint16_t mask = peekb((uint16_t)(0xFC74 + (def & 7)));
		r = peekb((uint16_t)(0x0CF6 + asr(def, 3))) & mask;
		set_c(true);                                    // COM
	}
	return r;
}

// F:BF5E  map_object_position(int *x, int *y): screen position of the current map object's
// definition (tables 0x5580 / 0x4AC0) adjusted by the cell's column (0xFDAA) and row (0xFDB2,
// with the scroll row 0x0E9B, one more right of column 0x099C).  Returns the column offset index.
uint16_t map_object_position(const Args &a)
{
	CFrame frame;
	frame.save_regs(SAVE_R2_R3, F144_RET);
	const uint16_t cell = peek((uint16_t)(0x0AF8 + 2 * peek(0x09AA))) & 0x01FF;
	uint16_t row = sext8(peekb(0x0E9B));
	if ((cell & 0x000F) > peek(0x099C)) row++;
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
	T.r[0] = (uint16_t)(peek(0x09AA) << 1);
	T.r[4] = peek((uint16_t)(0x0AF8 + T.r[0]));
	T.r[2] = T.r[4] & 0x01FF;
	T.r[1] = T.r[2] & 0x000F;
	fl::cmp(T.r[1], peek(0x0224));
	if (lt()) return;
	fl::cmp(T.r[1], peek(0x0226));
	if (gt()) return;
	T.r[0] = T.r[2] & 0x01F0;
	fl::cmp(T.r[0], peek(0x0228));
	if (lt()) return;
	fl::cmp(T.r[0], peek(0x022A));
	if (gt()) return;
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
		fl::cmp(pos(0x5580), peek(0x09A6));
		if (!fC()) return;
	}
	fl::cmp(T.r[1], peek(0x0226));
	if (!lt())
	{
		fl::cmp(pos(0x5580), peek(0x09A6));
		if (fC()) return;
	}
	fl::cmp(T.r[0], peek(0x022A));
	if (!lt())
	{
		fl::cmp(pos(0x4AC0), peek(0x09A4));
		if (!fC()) return;
	}
	fl::cmp(T.r[0], peek(0x0228));
	if (!gt())
	{
		fl::cmp(pos(0x4AC0), peek(0x09A4));
		if (fC()) return;
	}
	jsr(0xC4E4, 0xBE0E);
}

// F:BF18: release the current map object's definition: its group (properties bits 0-3) is
// free again (0x0E6D+group = 0) and its bit in the bitmap 0x0CF6 is cleared.  Banks end at
// 0x1000 / 0x1400.  Returns the bitmap byte index.
uint16_t release_map_definition(const Args &)
{
	CFrame frame;
	push(T.r[1]);
	T.r[0] = 0x1000;
	set_bank_a(0x1000);
	T.r[0] = 0x1400;
	set_bank_b(0x1400);
	jsr(0xBE10, 0xBF36);
	const uint16_t def = peek(0x09B6);
	const uint16_t group = peekb((uint16_t)(0x4560 + def)) & 0x0F;
	if (group) pokeb((uint16_t)(0x0E6D + group), 0);
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
	CFrame frame;
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
		if ((int16_t)R[2] < 0)
		{
			R[2] = (uint16_t)-R[2];
			ww(L6, 0xFFFE);
		}
		else
			ww(L6, 0x0002);
		if ((int16_t)R[2] > 8) ww(L6, (uint16_t)-rw(L6));   // the short way round
		ww(L4, (uint16_t)(0x04B0 + R[3]));
		if (!(peekb(rw(L4)) & 0x01))
		{
			R[5] = 0;
			ww((uint16_t)(sp - 2), (uint16_t)(rw(L2) + 0xFDBA));
			R[5] = peekb(rw((uint16_t)(sp - 2)));
			ww((uint16_t)(sp - 2), (uint16_t)(rw(TW) + 0x5F7C));
			R[5] = (uint16_t)(R[5] + peek(rw((uint16_t)(sp - 2))));
			R[2] = peek(R[5]);
		}
		else
		{
			R[5] = (uint16_t)(R[3] << 1);
			R[2] = peek((uint16_t)(0x03B0 + R[5]));
		}
		R[2] = (uint16_t)(R[2] + rw(L6));
		if ((int16_t)peek(R[2]) < 0) R[2] = peek(R[2]) & 0x7FFF;
		R[5] = R[0] | rw(L2);
		const bool odd = (R[5] & 1) != 0;
		R[5] = (uint16_t)(R[5] >> 1);                           // ROR, carry clear
		if (odd) R[2] = R[1];
		if (R[2] == R[1])
		{
			R[5] = 0xFF01;
			pokeb(rw(L4), peekb(rw(L4)) & ~0x01);
		}
		else
		{
			R[1] = R[2];
			ww(TURNED, (uint16_t)(rw(TURNED) + 1));
		}
		if (peekb((uint16_t)(0x6502 + R[4])))
		{
			if (!rw(TURNED))
				R[1] = peek(R[1]);
			else if (peekb(rw(L4)) & 0x01)
			{
				R[1] = (uint16_t)(R[1] + rw(L6));
				if ((int16_t)peek(R[1]) < 0) R[1] = peek(R[1]) & 0x7FFF;
			}
		}
		R[5] = (uint16_t)(R[3] << 1);
		if (R[1] != peek((uint16_t)(0x03B0 + R[5])))
		{
			poke((uint16_t)(0x03B0 + R[5]), R[1]);
			pokeb((uint16_t)(0x0490 + R[3]), peekb((uint16_t)(0x0490 + R[3])) | 0x18);
			ww((uint16_t)(sp - 2), (uint16_t)(rw(TW) + 0x6110));
			R[2] = peek(rw((uint16_t)(sp - 2)));
			if (R[2])
			{
				ww(L6, (uint16_t)(peekb((uint16_t)(0xFDBA + R[0])) + R[2]));
				poke((uint16_t)(0x03F0 + R[5]), peek(rw(L6)));
			}
		}
	}
	ww(L2, (uint16_t)(0x0770 + R[3]));
	R[5] = (uint16_t)(R[3] << 1);
	ww(L4, (uint16_t)(0x04F0 + R[5]));
	ww(L6, (uint16_t)(0x0570 + R[5]));
	ww(L0, (uint16_t)(0x0430 + R[5]));
	if (!rw(TURNED))
	{
		R[2] = sext8(peekb((uint16_t)(0x649D + R[4]))) & 1;    // stands still
		if (R[2] && (R[0] & 1))
		{
			R[2] = 0;
			pokeb((uint16_t)(0x0490 + R[3]), peekb((uint16_t)(0x0490 + R[3])) & ~0x01);
		}
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
			R[5] = (uint16_t)((R[0] << 3) + R[4]);
			R[2] = (uint16_t)(R[5] << 1);
			poke(rw(L4), peek((uint16_t)(0x4138 + R[2])));
			poke(rw(L6), peek((uint16_t)(0x4238 + R[2])));
			if (R[1] == 0x18)
				pokeb(rw(L0), peekb((uint16_t)(0xFD8A + R[4])));
			else
			{
				R[1] = (uint16_t)((R[1] << 3) + R[4]);
				pokeb(rw(L0), peekb((uint16_t)(0x3FF8 + R[1])));
			}
			poke(rw(L0), peek(rw(L0)) & 0x00FF);
		}
		else
		{
			R[1] = (uint16_t)(R[1] << 1);
			poke(rw(L0), (uint16_t)(peek(rw(L0)) + peek((uint16_t)(0x40F8 + R[1]))));
			poke(rw(L4), 0);
			poke(rw(L6), 0);
			pokeb((uint16_t)(0x0490 + R[3]), peekb((uint16_t)(0x0490 + R[3])) | 0x01);
		}
		pokeb((uint16_t)(0x07D0 + R[3]), (uint8_t)R[0]);
		R[0] = 0;
	}
	else
	{
		R[0] = 0x1000;
		set_bank_a(0x1000);
		R[0] = 0x1400;
		set_bank_b(0x1400);
		push(L8);
		push(LA);
		jsr(0xBF5E, 0xC37A);
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
	CFrame frame;
	frame.save_regs(SAVE_R2_R5, F162_RET);
	T.r[6] = (uint16_t)(T.r[6] - 2);
	const uint16_t at_desc = T.r[6];
	T.r[0] = (uint16_t)(peek(0x09AA) << 1);
	if ((int16_t)peek((uint16_t)(0x0AF8 + T.r[0])) > 0)
		jsr(0xC0D8, 0xBFD2);
	else
		jsr(0xC114, 0xBFD8);
	const uint16_t r = T.r[0];
	set_bank_a(0x1000);
	set_bank_b(0x1400);
	set_c(false);                                       // TST (SP)+
	if (r) return 0x1400;
	const uint16_t old = peek(0x09B6);
	const uint8_t old_prop = peekb((uint16_t)(0x4560 + old));
	if (old_prop & 0x0F)
	{
		if (!(peekb((uint16_t)(0x4560 + peek(0x09B4))) & 0x0F)) pokeb((uint16_t)(0x0E6D + (old_prop & 0x0F)), 0);
	}
	else
	{
		const uint16_t i = asr(old, 3);
		pokeb((uint16_t)(0x0CF6 + i), (uint8_t)(peekb((uint16_t)(0x0CF6 + i)) & ~peekb((uint16_t)(0xFC74 + (old & 7)))));
	}
	const uint16_t def = peek(0x09B4);
	const uint16_t w = (uint16_t)(peek(0x09AA) << 1);
	ww(at_desc, peek((uint16_t)(0x0AF8 + w)));
	const uint16_t desc = rw(at_desc);
	if ((int16_t)desc < 0)
	{
		const uint16_t slot = sext8(peekb((uint16_t)(0x09B8 + peek(0x09AA)))) & 0x003F;
		const uint16_t pal = (uint16_t)(0x0470 + slot);
		const bool want = (peekb((uint16_t)(0x4560 + def)) & 0x20) != 0, has = (peekb(pal) & 0x80) != 0;
		if (want != has)
		{
			pokeb(pal, want ? (uint8_t)(peekb(pal) | 0x80) : (uint8_t)(peekb(pal) & ~0x80));
			pokeb((uint16_t)(0x0490 + slot), peekb((uint16_t)(0x0490 + slot)) | 0x18);
		}
	}
	const uint8_t prop = peekb((uint16_t)(0x4560 + def));
	if (prop & 0x0F)
		pokeb((uint16_t)(0x0E6D + (prop & 0x0F)), (uint8_t)(peek(0x09AA) + 1));
	else
	{
		const uint16_t i = asr(def, 3);
		pokeb((uint16_t)(0x0CF6 + i), peekb((uint16_t)(0x0CF6 + i)) | peekb((uint16_t)(0xFC74 + (def & 7))));
	}
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
	CFrame frame;
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
		jsr(0xB56E, 0xCF88);
		R[6] = (uint16_t)(R[6] + 2);
	}
	else
		R[1] = sext8(peekb((uint16_t)(0x09B8 + R[1]))) & 0x007F;
	jsr(0xBF18, 0xCF98);
	R[0] = peek(0x09AA);
	if ((int16_t)R[0] < 0x40)
		pokeb((uint16_t)(0x0AB8 + R[0]), 0);
	else
	{
		R[0] = peek(0x09B6);
		push(peekb((uint16_t)(0x4000 + R[0])) & 0x0F);
		jsr(0xD348, 0xCFB8);
		R[6] = (uint16_t)(R[6] + 2);
	}
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
	CFrame frame;
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
		R[5] = 1;
		R[2] = 0;
		R[1] = 0x09AE;
		poke(0x09AA, 0);
	}
	else
	{
		R[2] = peekb(0x0E98);
		R[1] = 0x09B0;
		poke(0x09AA, 0x0040);
	}
	const bool fixed = R[5] != 0;
	const uint16_t top = R[1];
	uint16_t r0;
	if (jsr(0xBE7C, 0xCE84) != 0)
	{
		restore_banks(rw((uint16_t)(sp + 2)), rw(sp));
		r0 = 0xFFFF;
	}
	else
	{
		uint16_t i = R[2];
		while (peekb((uint16_t)(0x09B8 + i))) i++;
		R[2] = i;
		R[0] = (uint16_t)(i << 1);
		poke((uint16_t)(0x0AF8 + R[0]), desc);
		poke(0x09AA, i);
		jsr(0xBE10, 0xCEC0);
		const uint16_t def = peek(0x09B6), byte = asr(def, 3);
		set_bank_a(0);
		set_bank_b(0x0400);
		if (!(peekb((uint16_t)(0x6438 + type)) & 0x02))
			pokeb((uint16_t)(0x0CF6 + byte), peekb((uint16_t)(0x0CF6 + byte)) | peekb((uint16_t)(0xFC74 + (def & 7))));
		if (fixed && !(peekb((uint16_t)(0x649D + type)) & 0x80))
			pokeb((uint16_t)(0x0AB8 + i), (uint8_t)(peekb(0x0E9A) + 8));
		pokeb((uint16_t)(0x09B8 + i), (uint8_t)type);
		set_bank_a(0);
		set_bank_b(0x0400);
		poke(0x09AA, i);
		if ((int16_t)i <= (int16_t)peek(top))
		{
			i = peek(top);
			while (!peekb((uint16_t)(0x09B8 + i))) i--;
		}
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
	CFrame frame;
	frame.save_regs(SAVE_R2_R3, F144_RET);
	uint16_t *R = T.r;
	R[2] = peek(0x01D2);
	R[3] = peek(0x01D4);
	for (R[0] = 0x09AA; R[0] <= 0x0E9A; R[0]++) pokeb(R[0], 0);
	pokeb(0x0E93, 0);
	poke(0x09AE, 0xFFFF);
	poke(0x09B0, 0x003F);
	pokeb(0x0E98, 0x40);
	poke(0x01D2, 0);
	poke(0x1400, 0);
	R[0] = 0x0400;
	set_bank_b(0x0400);
	for (R[1] = 0x6696; peek(R[1]) != 0xFFFF; R[1] = (uint16_t)(R[1] + 4))
	{
		push(peek(R[1]));
		push(peek((uint16_t)(R[1] + 2)));
		push(1);
		jsr(0xCE24, 0xD032);
		R[6] = (uint16_t)(R[6] + 6);
	}
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
	CFrame frame;
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
		restore_banks(R[4], R[5]);
		set_c(false);
		return R[0];
	}
	if (peekb((uint16_t)(0x649D + R[2])) & 0x40)
	{
		R[0] = (uint16_t)(sext8(peekb(0x0E97)) + 1);
		pokeb(0x0E97, (uint8_t)R[0]);
	}
	if (peekb((uint16_t)(0x649D + R[2])) & 0x10)
	{
		R[0] = (uint16_t)(sext8(peekb(0x0E96)) + 1);
		pokeb(0x0E96, (uint8_t)R[0]);
	}
	if (peekb((uint16_t)(0x04B0 + R[3])) & 0x10)
	{
		push(peek(0x09AA));
		jsr(0xCF5E, 0xC468);
	}
	else
	{
		R[2] = peekb((uint16_t)(0x0370 + R[3]));
		pokeb((uint16_t)(0x09B8 + R[1]), (uint8_t)R[2]);
		R[0] = (uint16_t)(R[3] << 1);
		R[1] = peek((uint16_t)(0x0430 + R[0]));
		push(R[3]);
		jsr(0xB56E, 0xC482);
		if (R[3] == 5)
		{
			ww(R[6], 6);
			jsr(0xB56E, 0xC490);
		}
		R[3] = peek(0x09AA);
		R[0] = (uint16_t)(R[3] << 1);
		poke((uint16_t)(0x0AF8 + R[0]), peek((uint16_t)(0x0AF8 + R[0])) & 0x7FFF);
		if ((int16_t)R[3] < 0x40)
		{
			if (!(peekb((uint16_t)(0x649D + R[2])) & 0x80))
			{
				R[1] = asr(R[1], 2);
				R[0] = peekb(0x0E9A);
				R[1] = (uint16_t)(R[1] + R[0]);
				pokeb((uint16_t)(0x0AB8 + R[3]), (uint8_t)R[1]);
			}
			R[0] = 0x1800;
			set_bank_a(0x1800);
			ww(R[6], R[2]);
			jsr(0x5D08, 0xC4CC);
		}
	}
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
	CFrame frame;
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
		set_c(carry(sp, 0x12));                         // ADD #0x12,SP
		return R[0];
	};
	poke(0x01D2, 0);
	poke(0x1400, 0);
	R[0] = 0x0400;
	set_bank_b(0x0400);
	R[0] = peek(0x09AA);
	R[2] = sext8(peekb((uint16_t)(0x09B8 + R[0])));      // type
	if ((int16_t)R[0] >= 0x40)
	{
		if (peekb((uint16_t)(0x649D + R[2])) & 0x10)
		{
			R[0] = sext8(peekb(0x0E96));
			if (!R[0])
			{
				banks_back();
				return done();
			}
			R[1] = (uint16_t)(R[0] - 1);
			pokeb(0x0E96, (uint8_t)R[1]);
		}
	}
	else if (peekb((uint16_t)(0x649D + R[2])) & 0x40)
	{
		R[0] = sext8(peekb(0x0E97));
		if (!R[0])
		{
			// none left: count the event and remove the map object (not reached in the tests)
			if ((int16_t)R[2] < 0x2F)
			{
				R[0] = (uint16_t)(R[2] - 0x0A);
				R[1] = (uint16_t)(R[0] + 0x0B);
			}
			else
			{
				R[4] = (uint16_t)(R[2] - 0x2F);
				R[1] = (uint16_t)(R[4] + 0x12);
			}
			push(R[1]);
			jsr(0xD348, 0xC562);
			ww(R[6], peek(0x09AA));
			jsr(0xCF5E, 0xC56A);
			banks_back();
			R[6] = (uint16_t)(R[6] + 2);
			return done();
		}
		R[1] = (uint16_t)(R[0] - 1);
		pokeb(0x0E97, (uint8_t)R[1]);
		R[0] = 0x1800;
		set_bank_a(0x1800);
		push(R[2]);
		jsr(0x5CC0, 0xC588);
		poke(0x01D2, 0);
		poke(0x1400, 0);
		R[6] = (uint16_t)(R[6] + 2);
	}
	if (R[2] == 1)
	{
		if (peekb(0x0375))
		{
			banks_back();
			return done();
		}
		R[3] = 5;
	}
	else if (R[2] == 4)
		R[3] = 7;
	else
	{
		jsr(0xB5B8, 0xC5A8);
		R[3] = R[0];
		if ((int16_t)R[3] < 0)
		{
			banks_back();
			return done();
		}
	}
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
		jsr(0xDF40, 0xC612);
		R[6] = (uint16_t)(R[6] + 2);
	}
	else
	{
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
		pokeb(rw(PAL), peekb(rw(PAL)) | 0x40);
		R[0] = peek(0x09B6);
		if (peekb((uint16_t)(0x4560 + R[0])) & 0x20) pokeb(rw(PAL), peekb(rw(PAL)) | 0x80);
		if (R[2] != 4)
		{
			push(OFF_A);
			push(OFF_B);
			jsr(0xBF5E, 0xC6A0);
			poke(rw(PX), (uint16_t)(peek(rw(PX)) + rw(OFF_A)));
			poke(rw(PY), (uint16_t)(peek(rw(PY)) + rw(OFF_B)));
			R[0] = peekb((uint16_t)(peek(0x09B2) + 2));  // the record's direction
			R[6] = (uint16_t)(R[6] + 2);
		}
		else
		{
			push(R[3]);
			jsr(0xE0C2, 0xC6C2);
			ww(OFF_A, peek(rw(PX)));
			ww(OFF_B, peek(rw(PY)));
			R[0] = 6;
		}
		pokeb((uint16_t)(0x07D0 + R[3]), (uint8_t)R[0]);
		poke(0x01D2, 0);
		poke(0x1400, 0);
		ww(PY, 0x0400);
		poke(0x01D4, 0x0400);
		poke(0x1402, rw(PY));
		if (peekb(rw(PROPS)) & 0x80)
		{
			poke((uint16_t)(0x03B0 + R[5]), peek((uint16_t)(0x5F7C + R[4])));
			poke((uint16_t)(0x03F0 + R[5]), peek((uint16_t)(0x6110 + R[4])));
			if (peekb(rw(PROPS)) & 0x04) pokeb((uint16_t)(0x04B0 + R[3]), peekb((uint16_t)(0x04B0 + R[3])) | 0x20);
			if (R[2] == 0x57 && peekb(0x01E7))
			{
				ww(PY, 5);
				pokeb((uint16_t)(0x0790 + R[3]), 5);
				pokeb(rw(ANIM), 5);
			}
		}
		else
		{
			const uint16_t d = peekb((uint16_t)(0xFDBA + R[0]));
			ww((uint16_t)(R[6] - 2), (uint16_t)(d + peek((uint16_t)(0x5F7C + R[4]))));
			ww(PY, rw((uint16_t)(R[6] - 2)));
			R[1] = peek(rw(PY));
			if (peekb((uint16_t)(0x6502 + R[2])))
			{
				R[1] = peek(R[1]);
				pokeb(rw(ANIM), peekb(rw(ANIM)) | 0x80);
			}
			poke((uint16_t)(0x03B0 + R[5]), R[1]);
			R[4] = peek((uint16_t)(0x6110 + R[4]));
			ww((uint16_t)(R[6] - 2), d);
			R[4] = (uint16_t)(R[4] + d);
			poke((uint16_t)(0x03F0 + R[5]), peek(R[4]));
		}
		R[0] = sext8(peekb((uint16_t)(0x649D + R[2]))) & 1;
		pokeb((uint16_t)(0x0490 + R[3]), peekb((uint16_t)(0x0490 + R[3])) | (uint8_t)R[0]);
		R[0] = peekb((uint16_t)(0x623F + R[2]));
		if (R[0] && R[0] != 6)
		{
			R[0] = peek(0x09AA);
			if ((int16_t)R[0] < 0x40)
			{
				R[4] = peekb((uint16_t)(0x0AB8 + R[0]));
				R[2] = peekb(0x0E9A);
				R[4] = (uint16_t)(R[4] - R[2]);
				R[1] = (uint16_t)((R[4] & 0x00FF) << 2);
				pokeb((uint16_t)(0x0AB8 + R[0]), 0);
			}
			else
				R[1] = 0x0010;
			poke((uint16_t)(0x0430 + R[5]), R[1]);
		}
	}
	poke(0x01D2, 0);
	poke(0x1400, 0);
	R[0] = 0x0400;
	set_bank_b(0x0400);
	if (peekb(rw(PROPS)) & 0x08) pokeb((uint16_t)(0x04B0 + R[3]), peekb((uint16_t)(0x04B0 + R[3])) | 0x10);
	ww(R[6], (peekb(rw(PROPS)) & 0x40) ? (uint16_t)(R[3] | 0x0080) : R[3]);
	jsr(0xB4B6, 0xC7EA);
	R[0] = peek(0x09AA);
	R[4] = (uint16_t)(0x09B8 + R[0]);
	pokeb(R[4], (uint8_t)R[3]);
	R[0] = (uint16_t)(R[0] << 1);
	poke((uint16_t)(0x0AF8 + R[0]), peek((uint16_t)(0x0AF8 + R[0])) | 0x8000);
	R[3] = peek((uint16_t)(0x0AF8 + R[0])) & 0x01F0;
	R[2] = (uint16_t)(peek(0x0228) + 0x20);
	if (!((int16_t)R[3] < (int16_t)R[2])) pokeb(R[4], peekb(R[4]) | 0x80);
	R[2] = (uint16_t)(peek(0x0224) + 2);
	R[3] = R[2];
	R[1] = sext8(peekb((uint16_t)(0x0AF8 + R[0]))) & 0x000F;
	if (R[1] < R[3]) pokeb(R[4], peekb(R[4]) | 0x40);
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
	T.r[1] = peek(0x09AA);
	T.r[0] = (uint16_t)(T.r[1] << 1);
	if ((int16_t)peek((uint16_t)(0x0AF8 + T.r[0])) < 0)
	{
		T.r[1] = (uint16_t)(T.r[1] + 0x09B8);
		T.r[0] = sext8(peekb(T.r[1])) & 0x003F;
		T.r[2] = (uint16_t)(T.r[0] << 1);
		T.r[0] = peek((uint16_t)(0x0630 + T.r[2]));
		bool leave;
		if ((int16_t)T.r[0] >= 0)
		{
			fl::cmp(T.r[0], 0x4000);
			const uint8_t b = lt() ? (uint8_t)(peekb(T.r[1]) & ~0x80) : (uint8_t)(peekb(T.r[1]) | 0x80);
			pokeb(T.r[1], b);
			leave = false;
		}
		else
		{
			fl::cmp(T.r[0], 0xC000);
			const uint8_t b = peekb(T.r[1]);
			leave = fC() ? (int8_t)b > 0 : (int8_t)b < 0;
		}
		if (!leave)
		{
			T.r[0] = peek(0x09AA);
			T.r[1] = (uint16_t)(0x09B8 + T.r[0]);
			T.r[0] = peek((uint16_t)(0x0670 + T.r[2]));
			if ((int16_t)T.r[0] > 0)
			{
				fl::cmp(T.r[0], 0x4000);
				const uint8_t b = lt() ? (uint8_t)(peekb(T.r[1]) & ~0x40) : (uint8_t)(peekb(T.r[1]) | 0x40);
				pokeb(T.r[1], b);
				f_nzv(nzb(b));
				return;
			}
			fl::cmp(T.r[0], 0xC000);
			const uint8_t b = peekb(T.r[1]);
			f_nzv(nzb(b & 0x40));
			if (fC() ? (b & 0x40) != 0 : (b & 0x40) == 0) return;
		}
		jsr(0xC3E6, 0xBD36);
		return;
	}
	jsr(0xBD78, 0xBD3C);
	const uint16_t v = peek(0x099A);
	fl::tst(v);
	if ((int16_t)v >= 0) return;
	// a second try with the window widened by the scroll margins
	poke(0x0224, (uint16_t)(peek(0x0224) + 0x0C));
	poke(0x0226, (uint16_t)(peek(0x0226) + 0x0C));
	poke(0x0228, (uint16_t)(peek(0x0228) - 0x50));
	poke(0x022A, (uint16_t)(peek(0x022A) - 0x50));
	jsr(0xBD78, 0xBD5E);
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
	if (jsr(0xBE7C, 0xC854))
	{
		fl::tst(T.r[0]);
		T.r[0] = 0xFFFF;
		fl::mov(T.r[0]);
		return;
	}
	jsr(0xBFBC, 0xC85C);
	T.r[0] = 1;
	fl::mov(T.r[0]);
}

// F:C868  try_next_map_definition(int group): the same with the next record entry (0x09B2 + 4),
// only if that definition is free and in the given group (0xFD91 by properties 0x4000 bits
// 0-3; definitions without a group always qualify).  Returns 0 when moved, else -1.
uint16_t try_next_map_definition(const Args &a)
{
	CFrame frame;
	push(T.r[1]);
	poke(0x09B2, (uint16_t)(peek(0x09B2) + 4));
	if (jsr(0xBE7C, 0xC874))
	{
		set_c(false);
		return 0xFFFF;
	}
	T.r[0] = peek(0x09B4);
	if (peekb((uint16_t)(0x4000 + T.r[0])) & 0x0F)
	{
		T.r[0] = sext8(peekb((uint16_t)(0x4000 + T.r[0]))) & 0x000F;
		T.r[1] = peekb((uint16_t)(0xFD91 + T.r[0]));
		if (T.r[1] != a.u(0))
		{
			set_c(T.r[1] < a.u(0));                     // CMP R1,arg
			return 0xFFFF;
		}
	}
	jsr(0xBFBC, 0xC89C);
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
	T.r[1] = peek(0x022C);
	T.r[5] = (uint16_t)(0x04B0 + T.r[1]);
	if (peekb(T.r[5]) & 0x01)
	{
		if (peekb(T.r[5]) & 0x02) poke(0x09B2, (uint16_t)(peek(0x09B2) + 4));
		jsr(0xBE7C, 0xCA6C);
		jsr(0xBFBC, 0xCA70);
		return;
	}
	T.r[0] = 0xFFFF;
	T.r[2] = peek(0x09B6);
	T.r[3] = asr(T.r[2], 3);
	T.r[4] = (uint16_t)(0x0DAA + T.r[3]);
	T.r[3] = T.r[2] & 7;
	const uint16_t bit = (uint16_t)(0xFC74 + T.r[3]);
	if (!(peekb(T.r[4]) & peekb(bit)))
	{
		jsr(0xC850, 0xCA98);
		if (peekb((uint16_t)(0x4560 + T.r[2])) & 0x40)
		{
			pokeb(T.r[4], peekb(T.r[4]) | peekb(bit));
			if ((int16_t)T.r[0] < 0)
			{
				push(peekb((uint16_t)(0x0370 + T.r[1])));
				jsr(0xC868, 0xCAB2);
				T.r[6] = (uint16_t)(T.r[6] + 2);
			}
		}
	}
	else
	{
		pokeb(T.r[4], (uint8_t)(peekb(T.r[4]) & ~peekb(bit)));
		push(peekb((uint16_t)(0x0370 + T.r[1])));
		jsr(0xC868, 0xCAC2);
		if ((int16_t)T.r[0] < 0)
		{
			poke(0x09B2, (uint16_t)(peek(0x09B2) - 4));
			jsr(0xC850, 0xCAD0);
		}
		T.r[6] = (uint16_t)(T.r[6] + 2);
	}
	fl::tst(T.r[0]);
	if ((int16_t)T.r[0] < 0)
	{
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
	if (!T.r[0])
	{
		const uint8_t f = peekb(T.r[5]) | 0x02;
		pokeb(T.r[5], f);
		f_nzv(nzb(f));
		return;
	}
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
		poke(0x09AA, peek(0x09AC));
		poke(0x09AC, (uint16_t)(peek(0x09AC) + 1));
		T.r[0] = peek(0x09AA);
		if (T.r[0] != peek(0x09A8) && (int16_t)T.r[0] <= (int16_t)peek(0x09AE) && peekb((uint16_t)(0x09B8 + T.r[0])))
			jsr(0xBCB8, 0xBCAA);
		ww(T.r[6], (uint16_t)(rw(T.r[6]) - 1));
	} while (rw(T.r[6]));
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
	T.r[6] = (uint16_t)(T.r[6] - 2);
	const uint16_t result = T.r[6];
	pokeb(0x0E9A, (uint8_t)(peekb(0x0E9A) + 1));
	T.r[0] = peekb(0x0E9A);
	if (T.r[0])
	{
		poke(0x09AA, peek(0x09AE));
		T.r[1] = T.r[0];
		for (;;)
		{
			T.r[0] = peek(0x09AA);
			if ((int16_t)T.r[0] < 0) break;
			T.r[4] = peekb((uint16_t)(0x0AB8 + T.r[0]));
			if (T.r[4] == T.r[1])
			{
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
					jsr(0xC850, 0xC914);
					ww(result, T.r[0]);
					if (peekb((uint16_t)(0x4560 + T.r[3])) & 0x40)
					{
						pokeb(T.r[5], peekb(T.r[5]) | peekb(bit));
						if ((int16_t)T.r[0] >= 0)
							check = false;
						else
						{
							T.r[0] = peek(0x09AA);
							T.r[4] = sext8(peekb((uint16_t)(0x09B8 + T.r[0])));
							push(T.r[4]);
							jsr(0xC868, 0xC934);
							ww(result, T.r[0]);
							T.r[6] = (uint16_t)(T.r[6] + 2);
						}
					}
				}
				else
				{
					pokeb(T.r[5], (uint8_t)(peekb(T.r[5]) & ~peekb(bit)));
					T.r[0] = peek(0x09AA);
					T.r[4] = sext8(peekb((uint16_t)(0x09B8 + T.r[0])));
					push(T.r[4]);
					jsr(0xC868, 0xC948);
					ww(result, T.r[0]);
					if ((int16_t)T.r[0] < 0)
					{
						poke(0x09B2, (uint16_t)(peek(0x09B2) - 4));
						jsr(0xC850, 0xC958);
						ww(result, T.r[0]);
					}
					T.r[6] = (uint16_t)(T.r[6] + 2);
				}
				if (check && (int16_t)rw(result) < 0)
				{
					T.r[0] = peek(0x09AA);
					T.r[4] = (uint16_t)(peekb(0x0E9A) + 8);
					pokeb((uint16_t)(0x0AB8 + T.r[0]), (uint8_t)T.r[4]);
				}
				T.r[0] = peek(0x09AA);
				if (!peekb((uint16_t)(0x0AB8 + T.r[0]))) pokeb((uint16_t)(0x0AB8 + T.r[0]), 1);
			}
			poke(0x09AA, (uint16_t)(peek(0x09AA) - 1));
		}
	}
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
	T.r[6] = (uint16_t)(T.r[6] - 2);
	const uint16_t mode_at = T.r[6];
	if (peekb(0x0E68))
	{
		const uint8_t n = (uint8_t)(peekb(0x0E68) - 1);
		pokeb(0x0E68, n);
		if (!n)
		{
			T.r[0] = 0x3800;
			set_bank_a(0x3800);
			T.r[0] = 0x3C00;
			set_bank_b(0x3C00);
			jsr(0x4F42, 0xCB4E);
		}
	}
	T.r[0] = sext8(peekb(0x0994));
	poke(0x022C, T.r[0]);
	auto slot2 = []() { return (uint16_t)(peek(0x022C) << 1); };
	for (;; poke(0x022C, (uint16_t)(peek(0x022C) - 1)))
	{
		T.r[0] = peek(0x022C);
		if ((int16_t)T.r[0] < 5) break;
		T.r[1] = (uint16_t)((T.r[0] << 1) + 0x0430);
		if (!peek(T.r[1])) continue;
		const uint16_t count = (uint16_t)(peek(T.r[1]) - 1);
		poke(T.r[1], count);
		if ((int16_t)count > 0 && (peekb((uint16_t)(0x04B0 + T.r[0])) & 0x0C)) continue;
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
		jsr(0xBE10, 0xCBCC);
		const uint16_t mode = rw(mode_at);
		T.r[0] = (uint16_t)(mode << 1);
		switch (mode)
		{
		case 0:
		case 11:
			T.r[0] = slot2();
			poke((uint16_t)(0x0430 + T.r[0]), 0);
			break;
		case 1:
			jsr(0xCA4C, 0xCCEA);
			break;
		case 2:
			jsr(0xDE28, 0xCBE4);
			break;
		case 3:
			jsr(0xCA0C, 0xCBEA);
			jsr(0xCA4C, 0xCCEA);
			break;
		case 4:
			T.r[0] = 0x3800;
			set_bank_a(0x3800);
			T.r[0] = 0x3C00;
			set_bank_b(0x3C00);
			jsr(0x5018, 0xCC08);
			break;
		case 5:
			T.r[0] = slot2();
			poke((uint16_t)(0x0430 + T.r[0]), 0x0010);
			jsr(0xBCB8, 0xCC1A);
			break;
		case 6:
		case 10:
			poke(0x01D2, 0);
			poke(0x1400, 0);
			T.r[0] = 0x0400;
			set_bank_b(0x0400);
			T.r[0] = peekb((uint16_t)(0x0370 + peek(0x022C)));
			if (peekb((uint16_t)(0x649D + T.r[0])) & 0x10)
			{
				T.r[0] = (uint16_t)(sext8(peekb(0x0E96)) + 1);
				pokeb(0x0E96, (uint8_t)T.r[0]);
			}
			push(peek(0x09AA));
			jsr(0xCF5E, 0xCC56);
			T.r[6] = (uint16_t)(T.r[6] + 2);
			break;
		case 7:
			T.r[0] = sext8(peekb((uint16_t)(0x4000 + peek(0x09B6)))) & 0x000F;
			T.r[1] = slot2();
			if (T.r[0] && (peekb(0x0EA4) & peekb((uint16_t)(0xFC73 + T.r[0]))))
			{
				poke((uint16_t)(0x03F0 + T.r[1]), 0);
				jsr(0xCA4C, 0xCCEA);
			}
			else
			{
				poke((uint16_t)(0x04F0 + T.r[1]), 0);
				poke((uint16_t)(0x0570 + T.r[1]), 0);
				poke((uint16_t)(0x0430 + T.r[1]), 0);
			}
			break;
		case 8:
			T.r[0] = (uint16_t)((sext8(peekb((uint16_t)(0x4000 + peek(0x09B6)))) & 0x000F) << 1);
			switch (T.r[0] >> 1)
			{
			case 0:
				jsr(0xCA4C, 0xCCEA);
				break;
			case 1:
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
				jsr(0xCA4C, 0xCCEA);
				break;
			case 2:
				T.r[0] = peek(0x022C);
				if (!(peekb((uint16_t)(0x04B0 + T.r[0])) & 0x01))
				{
					// another map object on the same cell (descriptor + 0x400, then + 0x600)
					T.r[0] = (uint16_t)(peek(0x09AA) << 1);
					push((uint16_t)(peek((uint16_t)(0x0AF8 + T.r[0])) + 0x0400));
					push(peek(0x09AA));
					jsr(0xCDD0, 0xCD0E);
					T.r[1] = T.r[0];
					bool found = T.r[1] != 0xFFFF;
					if (!found)
					{
						T.r[0] = (uint16_t)(peek(0x09AA) << 1);
						ww(T.r[6], (uint16_t)(peek((uint16_t)(0x0AF8 + T.r[0])) + 0x0600));
						push(peek(0x09AA));
						jsr(0xCDD0, 0xCD2C);
						T.r[1] = T.r[0];
						T.r[6] = (uint16_t)(T.r[6] + 2);
						found = T.r[1] != 0xFFFF;
					}
					if (found)
					{
						T.r[0] = sext8(peekb((uint16_t)(0x09B8 + T.r[1]))) & 0x003F;
						if (peekb((uint16_t)(0x0770 + T.r[0])) & 0x80)
						{
							ww(T.r[6], T.r[1]);
							push(4);
							jsr(0xC98E, 0xCD4E);
							T.r[0] = (uint16_t)((sext8(peekb((uint16_t)(0x09B8 + T.r[1]))) & 0x003F) << 1);
							poke((uint16_t)(0x0430 + T.r[0]), 0x0700);
							T.r[6] = (uint16_t)(T.r[6] + 2);
						}
					}
					T.r[6] = (uint16_t)(T.r[6] + 4);
				}
				jsr(0xCA4C, 0xCD66);
				T.r[0] = peek(0x022C);
				if (!(peekb((uint16_t)(0x04B0 + T.r[0])) & 0x01))
				{
					pokeb((uint16_t)(0x0770 + T.r[0]), peekb((uint16_t)(0x0770 + T.r[0])) | 0x80);
					T.r[0] = (uint16_t)(T.r[0] << 1);
					poke((uint16_t)(0x0430 + T.r[0]), 0x7FFF);
				}
				break;
			default:                                    // the table's other entries are not code
				break;
			}
			break;
		case 9:
			T.r[0] = peek(0x022C);
			if (peekb((uint16_t)(0x0490 + T.r[0])) & 0x06)
			{
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
				jsr(0xCF5E, 0xCDAA);
				T.r[0] = (uint16_t)(sext8(peekb(0x0E96)) + 1);
				pokeb(0x0E96, (uint8_t)T.r[0]);
				T.r[6] = (uint16_t)(T.r[6] + 2);
			}
			break;
		default:                                        // beyond the jump table (not in 0x623F)
			break;
		}
	}
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
		for (int i = 0; i <= 5; i++) push(T.r[i]);
		T.r[6] = (uint16_t)(T.r[6] - 6);
		poke(0x15C0, 0);                                // acknowledge
		pokeb(0x0191, (uint8_t)(peekb(0x0191) + 1));
	}
	~Irq32v()
	{
		T.r[6] = (uint16_t)(T.r[6] + 6);
		for (int i = 5; i >= 0; i--) T.r[i] = pop();
	}
	Irq32v(const Irq32v &) = delete;
	Irq32v &operator=(const Irq32v &) = delete;
	// false when a handler is already running (this one only reselects the video bank)
	bool enter(uint16_t ret_8096, uint16_t ret_8068)
	{
		if (peekb(0x0FED))
		{
			jsr(0x8096, ret_8096);
			return false;
		}
		pokeb(0x0FED, (uint8_t)(peekb(0x0FED) + 1));
		ww((uint16_t)(T.r[6] + 4), peek(0x01D2));
		ww((uint16_t)(T.r[6] + 2), peek(0x01D4));
		ww(T.r[6], peek(0x01D0));
		T.r[0] = 0;
		mtps(0);
		jsr(0x8068, ret_8068);
		hw_banks(0, 0x0400);
		return true;
	}
	void leave(uint16_t ret)
	{
		poke(0x01D0, rw(T.r[6]));
		poke(0x01D2, rw((uint16_t)(T.r[6] + 4)));
		poke(0x1400, peek(0x01D2));
		poke(0x01D4, rw((uint16_t)(T.r[6] + 2)));
		poke(0x1402, peek(0x01D4));
		jsr(0x8096, ret);
		pokeb(0x0FED, (uint8_t)(peekb(0x0FED) - 1));
	}
};

// F:B8FC (interrupt, 32V, installed by F:8270): sprite lists only: quarter 1 rebuilds list 1
// (F:B1C8, F:B1EA), moves the objects (F:B378) and updates the second chains (F:B120);
// quarter 4 rebuilds list 0 and updates the first chains (F:B092)
void irq_32v_sprites()
{
	Irq32v irq;
	if (!irq.enter(0xB91E, 0xB93E)) return;
	const uint8_t quarter = peekb(0x0191);
	if (quarter == 1)
	{
		pokeb(0x0993, 1);
		jsr(0xB1C8, 0xB95A);
		jsr(0xB1EA, 0xB95E);
		hw_banks(0, 0x0400);
		jsr(0xB378, 0xB96C);
		hw_banks(0, 0x0400);
		jsr(0xB120, 0xB97A);
	}
	else if (quarter == 4)
	{
		pokeb(0x0993, 0);
		jsr(0xB1C8, 0xB98C);
		jsr(0xB1EA, 0xB990);
		hw_banks(0, 0x0400);
		jsr(0xB092, 0xB99E);
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
	Irq32v irq;
	if (!irq.enter(0xB690, 0xB6B2)) return;
	const uint8_t quarter = peekb(0x0191);
	if (quarter == 1)
	{
		pokeb(0x0993, 1);
		if (peekb(0x0193) & 0x01)
		{
			poke(0x0180, 1);
			pokeb(0x0995, 1);
			if (peekb(0x0193) & 0x04)
			{
				pokeb(0x0FEB, (uint8_t)(peekb(0x0FEB) - 1));
				if (!peekb(0x0FEA) || !peekb(0x0FEB))
				{
					pokeb(0x0FEA, 1);
					pokeb(0x0995, 0);
					if (!peekb(0x0FEB)) pokeb(0x0FEB, 2);
				}
			}
			else
			{
				pokeb(0x0FEB, 0x3C);
				pokeb(0x0FEA, 0);
			}
		}
		else
			pokeb(0x0995, 0);
		jsr(0xB1C8, 0xB722);
		if (!peekb(0x0995))
		{
			jsr(0xB03C, 0xB72C);
			jsr(0xB1EA, 0xB730);
			hw_banks(0, 0x0400);
		}
		if (peek(0x01C4) & 1)
			jsr(0xB120, 0xB746);
		else if (!(peek(0x01C4) & 3))
			jsr(0xAF32, 0xB754);
		if (!peekb(0x0995))
		{
			if (peekb(0x018F) != 4)
			{
				if (peekb(0x0F59) != 7)
				{
					T.r[0] = 0x2400;
					set_bank_a(0x2400);
					T.r[0] = 0x2800;
					set_bank_b(0x2800);
					jsr(0x7942, 0xB786);
				}
				jsr(0xE254, 0xB78A);
				T.r[0] = 0x3800;
				set_bank_a(0x3800);
				jsr(0x4098, 0xB79A);
				hw_banks(0, 0x0400);
			}
			hw_banks(0, 0x0400);
			jsr(0xB378, 0xB7B2);
		}
	}
	else if (quarter == 4)
	{
		pokeb(0x0993, 0);
		jsr(0xB1C8, 0xB7C6);
		if (!peekb(0x0995))
		{
			jsr(0xAFF0, 0xB7D0);
			jsr(0xB1EA, 0xB7D4);
			hw_banks(0, 0x0400);
		}
		T.r[0] = (uint16_t)(peek(0x01C4) + 1);
		if (T.r[0] & 1)
			jsr(0xB092, 0xB7EE);
		else if (!(T.r[0] & 3))
			jsr(0xAF32, 0xB7FA);
		if (peekb(0x018F) != 4 && !peekb(0x0995))
		{
			if (peekb(0x031F) == 5)
			{
				T.r[0] = 0x1800;
				set_bank_a(0x1800);
				T.r[0] = 0x1C00;
				set_bank_b(0x1C00);
				jsr(0x792A, 0xB8B6);
			}
			else
			{
				if (peekb(0x0EA0) & 0x80) jsr(0xD566, 0xB81A);
				if (peekb(0x0EA0))
					jsr(0xD632, 0xB868);
				else
				{
					jsr(0x934C, 0xB824);
					T.r[0] = 0x1800;
					set_bank_a(0x1800);
					bool usual = true;
					if (peekb(0x0F59) == 3 && jsr(0x58D0, 0xB83C) == 0)
					{
						jsr(0x944C, 0xB844);
						T.r[0] = 0x1800;
						set_bank_a(0x1800);
						jsr(0x5A50, 0xB854);
						usual = false;
					}
					if (usual)
					{
						jsr(0x94DC, 0xB85A);
						jsr(0x944C, 0xB85E);
					}
					jsr(0x979A, 0xB862);
				}
				jsr(0x9A32, 0xB86C);
				T.r[0] = 0x1800;
				set_bank_a(0x1800);
				T.r[0] = 0x1C00;
				set_bank_b(0x1C00);
				jsr(0x7854, 0xB888);
				T.r[0] = 0x1800;
				set_bank_a(0x1800);
				jsr(0x4A2A, 0xB898);
			}
			T.r[0] = 0x1800;
			set_bank_a(0x1800);
			jsr(0x5166, 0xB8C6);
		}
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
	frame.save_regs(SAVE_R2_R5, F162_RET);
	T.r[0] = font;
	push(font);
	jsr(0x8068, 0xBAA8);
	uint16_t r0, r1, r2 = a.u(4), r3 = a.u(3), r4 = pop(), r5;
	r4 = (uint16_t)(r4 | (a.u(2) & 0x00FF));
	if ((uint8_t)r4 > 10) r4 = (uint16_t)((r4 & 0xFF00) | 10);
	r5 = sext8((uint8_t)r4);
	r0 = sext8((uint8_t)a.u(1));
	if (!(r0 & 0x00FF)) r0 = 0x8001;
	r5 = (uint16_t)(r5 - r0);
	r1 = (uint16_t)((sext8((uint8_t)r4) << 2) + POW10);
	const bool large = (r4 & 0x8000) != 0;
	const bool separators = a.u(0) != 0;

	auto pow10 = [](uint16_t p) { return ((uint32_t)peek(p) << 16) | peek((uint16_t)(p + 2)); };
	uint32_t v = ((uint32_t)r3 << 16) | r2;
	if (v >= pow10(r1))
	{
		// too many digits: keep the low ones (value modulo 10^digits)
		r5 &= 0xFF00;
		r0 = r1;
		bool reduce = false;
		for (;;)
		{
			if (r1 > 0xBA8E) break;
			if (r1 == 0xBA8E) { reduce = true; break; }
			r1 = (uint16_t)(r1 + 4);
			if (pow10(r1) > v) break;
		}
		for (;;)
		{
			if (reduce)
			{
				const uint32_t p = pow10(r1);
				while (v >= p) v -= p;
			}
			reduce = true;
			r1 = (uint16_t)(r1 - 4);
			if (r1 < r0)
			{
				r1 = (uint16_t)(r1 + 4);
				break;
			}
		}
		r2 = (uint16_t)v;
		r3 = (uint16_t)(v >> 16);
	}

	// the character routines, called with the original's registers from its call sites
	auto put = [&](uint16_t routine, uint16_t ret, uint16_t c) {
		T.r[0] = c;
		T.r[1] = r1; T.r[2] = r2; T.r[3] = r3; T.r[4] = r4; T.r[5] = r5;
		jsr(routine, ret);
		r0 = T.r[0];
		r1 = T.r[1]; r2 = T.r[2]; r3 = T.r[3]; r4 = T.r[4]; r5 = T.r[5];
	};

	r4 = (uint16_t)((r4 & 0xFF00) | (uint8_t)(r4 - 1));
	for (;;)
	{
		bool print = true, separator = false, last = false;
		if ((int8_t)r4 > 0)
		{
			r1 = (uint16_t)(r1 - 4);
			const uint32_t p = pow10(r1);
			r0 = p ? (uint16_t)(v / p) : 0;           // (the table has no zero entry)
			if (p) v %= p;
			r2 = (uint16_t)v;
			r3 = (uint16_t)(v >> 16);
			if (r0 == 0)
			{
				r5 = (uint16_t)((r5 & 0xFF00) | (uint8_t)(r5 - 1));
				if (!(r5 & 0x0080))
				{
					print = false;
					if ((int16_t)r5 < 0) goto next;     // dropped leading zero: not counted
					if (large)                          // blank leading zero
						put(0x8B52, 0xBB60, 0);
					else
						put(0x8C12, 0xBB6C, 0);
					if (large && !separators) goto count;
					r0 = 0;
					separator = true;
				}
			}
		}
		else
		{
			r0 = r2;                                    // last digit: the remainder
			last = true;
		}
		if (print)
		{
			if (!last) r5 &= 0xFF00;                    // a digit was shown: later zeros print
			r0 = (uint16_t)(r0 + 0x30);
			if (large)
				put(0x8C56, 0xBB80, r0);
			else
				put(0x8C12, 0xBB86, r0);
			if (!separators) goto count;
			r0 = 0x002C;
			separator = true;
		}
		if (separator && (r1 == POW10 + 12 || r1 == POW10 + 24 || r1 == POW10 + 36))
		{
			if (large)
			{
				if (!r0) poke(0x0186, (uint16_t)(peek(0x0186) - 2));
				put(0x8B52, 0xBBB4, r0);
			}
			else
				put(0x8C12, 0xBBBA, r0);
		}
	count:
		r5 = (uint16_t)(r5 + 0x0100);
	next:
		r4 = (uint16_t)((r4 & 0xFF00) | (uint8_t)(r4 - 1));
		if (r4 & 0x0080) break;
	}
	bool c = false;
	if ((int16_t)r5 < 0)
	{
		const uint16_t printed = (uint16_t)(((r5 >> 8) | (r5 << 8)) & 0x003F);
		r5 = printed;
		r1 = a.u(2);
		c = r1 < printed;
		r1 = (uint16_t)(r1 - printed);
		if (r1)
		{
			for (;;)
			{
				if (large)
					put(0x8B52, 0xBBE0, 0);
				else
					put(0x8C12, 0xBBE6, 0);
				c = fC();
				const uint16_t old = r1;
				r1--;
				if (!((int16_t)r1 > 0 && old != 0x8000)) break;
			}
		}
	}
	set_c(c);
	return r0;
}

uint16_t print_number_small(const Args &a) { return print_number(a, 0x0000); }
uint16_t print_number_large(const Args &a) { return print_number(a, 0x8000); }

}

LOGIC_TABLE(fmid,
	asm_routine("F", 0xB03C, "apply_queued_piece_counts", apply_queued_piece_counts, 58),
	asm_routine("F", 0xB092, "update_chains_a", update_chains_a, 3372),
	asm_routine("F", 0xB120, "update_chains_b", update_chains_b, 3278),
	asm_routine("F", 0xB1EA, "build_sprite_pieces", build_sprite_pieces, 10481),
	asm_routine("F", 0xB378, "move_objects", move_objects, 6590),
	c_routine("F", 0xB4B6, "show_object", show_object, 0, 1240),
	c_routine("F", 0xB602, "queue_piece_count", queue_piece_count, 0, 990),
	c_routine("F", 0xBA2E, "clear_objects_and_text", clear_objects_and_text, 0, 80502),
	c_routine("F", 0xC98E, "restart_object_animation", restart_object_animation, 0, 1047),
	c_routine("F", 0xBE10, "locate_map_object", locate_map_object, 0, 686),
	c_routine("F", 0xBE7C, "map_object_state", map_object_state, 0, 729),
	c_routine("F", 0xBF5E, "map_object_position", map_object_position, 0, 798),
	asm_routine("F", 0xBD78, "map_object_enters", map_object_enters, 263),
	c_routine("F", 0xBA96, "print_number_small", print_number_small, 0, 3276),
	c_routine("F", 0xBA9A, "print_number_large", print_number_large, 0, 2648),
	c_routine("F", 0xBF18, "release_map_definition", release_map_definition, 0, 429),
	c_routine("F", 0xC114, "turn_map_object", turn_map_object, 0, 2946),
	c_routine("F", 0xCF5E, "remove_map_object", remove_map_object, 0, 611),
	c_routine("F", 0xCE24, "add_map_object", add_map_object, 0, 3777),
	c_routine("F", 0xCFD6, "init_map_objects", init_map_objects, 0, 73209),
	c_routine("F", 0xC3E6, "map_object_leaves", map_object_leaves, 0, 1405),
	asm_routine("F", 0xBCB8, "check_map_object_view", check_map_object_view, 232),
	c_routine("F", 0xBFBC, "move_map_object", move_map_object, 0, 1298),
	c_routine("F", 0xC4E4, "create_map_object", create_map_object, 0, 4033),
	irq_routine("F", 0xB8FC, "irq_32v_sprites", irq_32v_sprites, 1084),
	irq_routine("F", 0xB66E, "irq_32v_game", irq_32v_game, 1371),
	asm_routine("F", 0xC850, "try_map_object_move", try_map_object_move, 115),
	c_routine("F", 0xC868, "try_next_map_definition", try_next_map_definition, 0, 289),
	asm_routine("F", 0xCA4C, "update_map_object_slot", update_map_object_slot, 390),
	asm_routine("F", 0xBC52, "check_map_objects_in_view", check_map_objects_in_view, 1295),
	asm_routine("F", 0xC8AA, "tick_map_object_timers", tick_map_object_timers, 4451),
	asm_routine("F", 0xCB24, "update_objects", update_objects, 2617),
)

}
