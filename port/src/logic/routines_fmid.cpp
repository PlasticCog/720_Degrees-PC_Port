// Rewritten routines of the fixed ROM F:B000-CFFF: the sprite chains and object movement run by
// the 32V interrupt, the map objects (park objects placed from the map table, 0x09AA-0x0E9A)
// and the decimal number printer.  Assembly-interface routines reproduce registers and flags;
// C routines leave the carry flag as the original does (callers written in assembly may
// return it).
#include "logic.h"

namespace logic {
namespace {

using namespace t11rt;

uint16_t sext8(uint8_t v) { return (uint16_t)(int16_t)(int8_t)v; }

// condition codes of the instructions the fl:: helpers do not cover
uint16_t asl(uint16_t d)
{
	const int r = (d << 1) & 0xFFFF;
	f_nzvc(nzw(r) | ((d >> 15) & 1));
	f_shift();
	return (uint16_t)r;
}
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

// stack space the original holds while it calls other routines (register-save helpers, locals):
// routines that are called keep the original's stack addresses (F:CE24 records one)
struct Frame {
	uint16_t n;
	explicit Frame(uint16_t bytes) : n(bytes) { T.r[6] = (uint16_t)(T.r[6] - n); }
	~Frame() { T.r[6] = (uint16_t)(T.r[6] + n); }
	Frame(const Frame &) = delete;
	Frame &operator=(const Frame &) = delete;
};
constexpr uint16_t SAVE_R2_R3 = 8, SAVE_R2_R4 = 10, SAVE_R2_R5 = 12;   // F:F144 / F:F152 / F:F162

// bits 9-15 of a map word as the original extracts them (ASR, CLRB, SWAB, MOVB): the cell offset
uint16_t field9(uint16_t d) { return sext8((uint8_t)((uint16_t)((int16_t)d >> 1) >> 8)); }

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
			call(0xACE6, {});
			ww(T.r[6], 1);
			push(T.r[3]);
			call(0xAE02, {});
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
			call(target, {});
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
			call(target, {});
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
					poke(0x0FF8, (uint16_t)((int16_t)peek(0x0FF8) >> 2));
				};
				poke(0x0FF8, 0);
				pokeb(0x0FF9, peekb(T.r[1]++));
				T.r[2] = (uint16_t)((int16_t)peek(0x0FF8) >> 2);
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
	uint16_t rate_ptr = rw((uint16_t)(T.r[6] - 4)), scratch = rw((uint16_t)(T.r[6] - 2));
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
				rate_ptr = (uint16_t)(0x0770 + i);
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
							scratch = (uint16_t)(sext8(peekb(T.r[0]++)) << 2);
							T.r[3] = (uint16_t)(scratch << 2);
							scratch = (uint16_t)(sext8(peekb(T.r[0]++)) << 2);
							T.r[2] = (uint16_t)(scratch << 2);
							const uint8_t step = peekb(T.r[0]);
							if (step)
							{
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
	fl::cmp(rate_ptr, scratch);
}

// ---------------------------------------------------------------- objects

// F:B4B6  show_object(int slot): make slot (bit 7: without sprite pieces) visible: the highest
// used slot 0x0994 rises to it (or drops to the highest slot still in use), the piece count
// 0x04D0 comes from the type table 0x5C96, the screen position from the world position and the
// scroll; flag 0x80 of 0x0490.  0x0FFC = 0xFF when interrupts are enabled.  Returns slot * 2.
uint16_t show_object(const Args &a)
{
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
	Frame f(2);
	const uint16_t r1 = T.r[1];
	T.r[1] = 0x00E0;
	mtps(0xE0);
	call(0x8068, {});
	call(0x8D3E, {});
	for (uint16_t p = 0x0370; p != 0x0995; p++) pokeb(p, 0);
	poke(0x3BFE, 0x03F8);
	poke(0x0988, 0xFFFF);
	mtps(0);
	T.r[1] = r1;
	return 0x0995;
}

// F:C98E  restart_object_animation(int rate, int n): the object of map object n (0x09B8, the
// argument is replaced by the slot) restarts its animation: next frame of the list 0x03B0 (a
// negative link is followed), redraw flags 0x18, rate and counter (0x0770, 0x0790) = rate.
// Not for the object queued in 0x0E6A.  Returns the slot.
uint16_t restart_object_animation(const Args &a)
{
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
		const uint16_t frame = peek(peek(p));
		if ((int16_t)frame < 0)
		{
			poke(p, frame);
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
		r = peekb((uint16_t)(0x0CF6 + (uint16_t)((int16_t)def >> 3))) & mask;
		set_c(true);                                    // COM
	}
	return r;
}

// F:BF5E  map_object_position(int *x, int *y): screen position of the current map object's
// definition (tables 0x5580 / 0x4AC0) adjusted by the cell's column (0xFDAA) and row (0xFDB2,
// with the scroll row 0x0E9B, one more right of column 0x099C).  Returns the column offset index.
uint16_t map_object_position(const Args &a)
{
	const uint16_t cell = peek((uint16_t)(0x0AF8 + 2 * peek(0x09AA))) & 0x01FF;
	uint16_t row = sext8(peekb(0x0E9B));
	if ((cell & 0x000F) > peek(0x099C)) row++;
	const uint16_t ri = (uint16_t)((((uint16_t)((int16_t)cell >> 4) + row) & 3) << 1);
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
	call(0xC4E4, {});
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
	const uint16_t saved[6] = { T.r[0], T.r[1], T.r[2], T.r[3], T.r[4], T.r[5] };
	Frame frame(SAVE_R2_R5);
	T.r[0] = font;
	push(font);
	call(0x8068, {});
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

	auto put = [&](uint16_t routine, uint16_t c) {
		T.r[0] = c;
		T.r[1] = r1; T.r[2] = r2; T.r[3] = r3; T.r[4] = r4; T.r[5] = r5;
		call(routine, {});
		r0 = T.r[0];
		r1 = T.r[1]; r2 = T.r[2]; r3 = T.r[3]; r4 = T.r[4]; r5 = T.r[5];
	};
	auto put_char = [&](uint16_t c) { put(large ? 0x8B52 : 0x8C12, c); };

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
					put_char(0);                        // blank leading zero
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
			put(large ? 0x8C56 : 0x8C12, r0);
			if (!separators) goto count;
			r0 = 0x002C;
			separator = true;
		}
		if (separator && (r1 == POW10 + 12 || r1 == POW10 + 24 || r1 == POW10 + 36))
		{
			if (large)
			{
				if (!r0) poke(0x0186, (uint16_t)(peek(0x0186) - 2));
				put(0x8B52, r0);
			}
			else
				put(0x8C12, r0);
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
				put_char(0);
				c = fC();
				const uint16_t old = r1;
				r1--;
				if (!((int16_t)r1 > 0 && old != 0x8000)) break;
			}
		}
	}
	for (int i = 1; i < 6; i++) T.r[i] = saved[i];
	set_c(c);
	return r0;
}

uint16_t print_number_small(const Args &a) { return print_number(a, 0x0000); }
uint16_t print_number_large(const Args &a) { return print_number(a, 0x8000); }

}

LOGIC_TABLE(fmid,
	asm_routine("F", 0xB03C, "apply_queued_piece_counts", apply_queued_piece_counts, 59),
	asm_routine("F", 0xB092, "update_chains_a", update_chains_a, 4519),
	asm_routine("F", 0xB120, "update_chains_b", update_chains_b, 4636),
	asm_routine("F", 0xB1EA, "build_sprite_pieces", build_sprite_pieces, 11675),
	asm_routine("F", 0xB378, "move_objects", move_objects, 9224),
	c_routine("F", 0xB4B6, "show_object", show_object, 0, 1227),
	c_routine("F", 0xB602, "queue_piece_count", queue_piece_count, 0, 990),
	c_routine("F", 0xBA2E, "clear_objects_and_text", clear_objects_and_text, 0, 132999),
	c_routine("F", 0xC98E, "restart_object_animation", restart_object_animation, 0, 1047),
	c_routine("F", 0xBE10, "locate_map_object", locate_map_object, 0, 686),
	c_routine("F", 0xBE7C, "map_object_state", map_object_state, 0, 733),
	c_routine("F", 0xBF5E, "map_object_position", map_object_position, 0, 798),
	asm_routine("F", 0xBD78, "map_object_enters", map_object_enters, 301),
	c_routine("F", 0xBA96, "print_number_small", print_number_small, 0, 6161),
	c_routine("F", 0xBA9A, "print_number_large", print_number_large, 0, 5406),
)

}
