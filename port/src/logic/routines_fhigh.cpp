// Rewritten routines of the fixed ROM F:D000-FFFF: object slots and their spawning, the
// object scripts, palette fades, skater animation tables, score and the start buttons.
#include "logic.h"

namespace logic {
namespace {

using namespace t11rt;

uint16_t sext8(uint8_t v) { return (uint16_t)(int16_t)(int8_t)v; }

// The C routines start with a register-save helper (JSR R1,@#F138 / F144 / F152 / F162): it
// pushes R1, R2..R(n+1) and its return address (F142 / F150 / F160 / F172), runs the body and
// restores the registers.  The frame is reproduced because callees can see the stack pointer
// (F:CE24 records the address of an argument in 0x09B2); callees with a register interface
// are given the registers the original holds, so they are put back here too.
struct Helper {
	uint16_t r[6], sp;
	explicit Helper(int n)                             // n registers from R2 on (1..4)
	{
		static const uint16_t ret[] = {0xF142, 0xF150, 0xF160, 0xF172};
		for (int i = 1; i < 6; i++) r[i] = T.r[i];
		sp = T.r[6];
		push(T.r[1]);
		for (int i = 2; i < 2 + n; i++) push(T.r[i]);
		push(ret[n - 1]);
	}
	~Helper() { for (int i = 1; i < 6; i++) T.r[i] = r[i]; T.r[6] = sp; }
};
enum { F138 = 1, F144 = 2, F152 = 3, F162 = 4 };

// select both banks (shadows 0x01D2/0x01D4 and the bank registers)
void set_banks(uint16_t a, uint16_t b)
{
	poke(0x01D2, a); ww(0x1400, a);
	poke(0x01D4, b); ww(0x1402, b);
}

// put back banks saved earlier: shadows first, then the registers from the shadows
void restore_banks(uint16_t a, uint16_t b)
{
	poke(0x01D2, a);
	poke(0x01D4, b);
	ww(0x1400, peek(0x01D2));
	ww(0x1402, peek(0x01D4));
}

// F:D0F4: with banks 0x1000/0x1400, F:C3E6 runs for each slot 0x09AA = 0xAE..0 whose word in
// 0x0AF8 is negative; then object 4 is reset (F:B56E) if it is active.  Banks restored.
uint16_t update_marked_slots(const Args &)
{
	Helper h(F138);
	const uint16_t a = peek(0x01D2), b = peek(0x01D4);
	T.r[1] = a; T.r[2] = b;
	set_banks(0x1000, 0x1400);
	poke(0x09AA, 0x00AE);
	uint16_t r0;
	for (;;)
	{
		r0 = peek(0x09AA);
		if ((int16_t)r0 < 0) break;
		r0 = (uint16_t)(r0 << 1);
		if ((int16_t)peek((uint16_t)(0x0AF8 + r0)) < 0)
		{
			T.r[0] = r0;
			call(0xC3E6, {});
		}
		poke(0x09AA, (uint16_t)(peek(0x09AA) - 1));
	}
	if (peekb(0x0374))
	{
		T.r[0] = r0;
		r0 = call(0xB56E, {0x0004});
	}
	restore_banks(a, b);
	return r0;
}

// F:D15C: F:CF5E(slot) for the slots 0xFE down to 0xAF whose type byte 0x09B8 is set
uint16_t update_high_slots(const Args &)
{
	Helper h(F144);
	const uint16_t a = peek(0x01D2), b = peek(0x01D4);
	T.r[2] = a; T.r[3] = b;
	uint16_t r0 = 0x00FE;
	for (uint16_t i = 0x00FE; (int16_t)i >= 0x00AF; i--)
		if (peekb((uint16_t)(0x09B8 + i)))
		{
			T.r[0] = r0; T.r[1] = i;
			r0 = call(0xCF5E, {i});
		}
	restore_banks(a, b);
	return r0;
}

// F:D880: the eight entries of the table at 0x70C4 (bank B 0x0400) get a free slot each
// (F:CDFA find_other_slot(-1, value)) whose type 0x09B8 comes from 0x70D4, or from 0x70DC
// when the park of the entry (index / 2) is open (F:E6FE).  Returns the last slot.
uint16_t set_park_entry_types(const Args &)
{
	Helper h(F144);
	const uint16_t open = call(0xE6FE, {});
	T.r[1] = open;
	set_banks(0x0000, 0x0400);
	uint16_t r0 = 0;
	for (int16_t i = 7; i >= 0; i--)
	{
		T.r[3] = (uint16_t)i;
		T.r[2] = (uint16_t)(i << 1);
		r0 = call(0xCDFA, {0xFFFF, peek((uint16_t)(0x70C4 + 2 * i))});
		const uint16_t dst = (uint16_t)(0x09B8 + r0);
		if (!(peekb((uint16_t)(0xFC74 + (i >> 1))) & (uint8_t)open))
			pokeb(dst, peekb((uint16_t)(0x70D4 + i)));
		else
			pokeb(dst, peekb((uint16_t)(0x70DC + i)));
	}
	return r0;
}

// F:D8D2: the four entries of the table at 0x70AA (bank B 0x0400) get a slot (F:CDFA) whose
// type comes from 0x70BB, or 0x70BF when the ticket bit (0x0ED7) of the entry is set.  When
// no slot is found, the object of the slot found for value | 0x8000 is set up instead.
uint16_t set_ticket_entry_types(const Args &)
{
	Helper h(F162);
	const uint16_t a = peek(0x01D2), b = peek(0x01D4);
	T.r[4] = a; T.r[5] = b;
	set_banks(0x0000, 0x0400);
	uint16_t r0 = 0;
	for (uint16_t i = 4; i >= 1; i--)
	{
		const uint16_t w = (uint16_t)(i << 1);
		T.r[1] = i; T.r[2] = w;
		push(peek((uint16_t)(0x70AA + w)));
		push(0xFFFF);
		r0 = call(0xCDFA, {});
		const bool ticket = (peekb(0x0ED7) & peekb((uint16_t)(0xFC73 + i))) != 0;
		if ((int16_t)r0 >= 0)
			pokeb((uint16_t)(0x09B8 + r0), peekb((uint16_t)((ticket ? 0x70BF : 0x70BB) + i)));
		else
		{
			poke(T.r[6], (uint16_t)(peek((uint16_t)(0x70AA + w)) | 0x8000));
			push(0xFFFF);
			r0 = call(0xCDFA, {});
			r0 = sext8(peekb((uint16_t)(0x09B8 + r0))) & 0x003F;
			if (!(peekb(0x0ED7) & peekb((uint16_t)(0xFC73 + i))))
			{
				pokeb((uint16_t)(0x0370 + r0), peekb((uint16_t)(0x70BB + i)));
				poke((uint16_t)(0x03B0 + (uint16_t)(r0 << 1)), peek((uint16_t)(0x5B82 + w)));
			}
			else
				pokeb((uint16_t)(0x0370 + r0), peekb((uint16_t)(0x70BF + i)));
			pokeb((uint16_t)(0x0490 + r0), (uint8_t)(peekb((uint16_t)(0x0490 + r0)) | 0x18));
			T.r[6] = (uint16_t)(T.r[6] + 2);
		}
		T.r[6] = (uint16_t)(T.r[6] + 4);
	}
	restore_banks(a, b);
	return r0;
}

// F:D47C  start_object(int i): object i gets animation 0x6046[type] (type = 0x0370[i], bank B
// 0x0400), timers 0x0770/0x0790 = 8, flags 0x18 and its start sound (0x6567[type] through
// the table at 0xFDED); then F:BF18 runs for its slot (0x0390[i] into 0x09AA)
uint16_t start_object(const Args &a)
{
	Helper h(F144);
	set_banks(0x0000, 0x0400);
	const uint16_t i = a.u(0);
	pokeb((uint16_t)(0x0790 + i), 8);
	pokeb((uint16_t)(0x0770 + i), 8);
	const uint16_t type = peekb((uint16_t)(0x0370 + i));
	poke((uint16_t)(0x03B0 + (uint16_t)(i << 1)), peek((uint16_t)(0x6046 + (uint16_t)(type << 1))));
	pokeb((uint16_t)(0x0490 + i), (uint8_t)(peekb((uint16_t)(0x0490 + i)) | 0x18));
	const uint16_t snd = peekb((uint16_t)(0x6567 + type));
	T.r[0] = snd; T.r[1] = i; T.r[2] = type; T.r[3] = (uint16_t)(i << 1);
	if (snd) call(0x8E78, {peekb((uint16_t)(0xFDED + snd))});
	pokeb(0x09AA, peekb((uint16_t)(0x0390 + i)));
	pokeb(0x09AB, 0);
	return call(0xBF18, {});
}

// F:DD5C: colour 0x1092 of the text palette.  On level 0 it comes from ROM (0x5ED2, bank
// 0x2000); on the other levels it is one of four colours by where the value 0x0F20 (0x0F34
// from 0x027A = 2 on) lies against the limits 0x0FDA / 0x0FDC / 0x0FDE (0: the lowest);
// nothing changes from 0x0F26 = 6 on.
uint16_t set_colour_1092(const Args &)
{
	Helper h(F138);
	if (peekb(0x0F59))
	{
		if (peekb(0x0F26) >= 6) return T.r[0];
		const uint16_t v = peekb(0x027A) >= 2 ? peek(0x0F34) : peek(0x0F20);
		uint16_t c;
		if (v == 0) c = 0x800F;
		else if ((int16_t)v >= (int16_t)peek(0x0FDE)) c = 0xFF19;
		else if ((int16_t)v >= (int16_t)peek(0x0FDC)) c = 0xEEFA;
		else if ((int16_t)v >= (int16_t)peek(0x0FDA)) c = 0xC73A;
		else c = 0x800F;
		poke(0x1092, c);
		return c;
	}
	const uint16_t a = peek(0x01D2), b = peek(0x01D4);
	set_banks(0x2000, 0x2400);
	poke(0x1092, peek(0x5ED2));
	restore_banks(a, b);
	return 0x2400;
}

// Object scripts.  Four byte streams (pointers 0x0EC2, counters 0x0ECA) drive the words listed
// at 0xFE06: x 0x0224 and y 0x0226 (channels 0 and 1: each frame += current byte * 8) and
// 0x03BA / 0x03BC (channels 2 and 3: on each new entry the word is set from the two bytes
// after the count, or the first adds twice its value when the count is non-zero).  An entry
// is a count byte (the time until the next entry); 0xFF n jumps back n bytes and loads the
// target word from the stream (channel 0 also adds the lane offset 0xFDB2[F:DDFE]).

// F:DE28: run the scripts for one frame; objects 5 and 6 follow (x 0x06FA/0x06FC,
// y 0x073A/0x073C)
uint16_t run_object_scripts(const Args &)
{
	Helper h(F162);
	T.r[6] = (uint16_t)(T.r[6] - 2);                   // TST -(SP): local
	set_banks(0x2400, 0x2800);
	poke(0x0224, peek(0x06FA));
	poke(0x0226, peek(0x073C));
	auto step = [](uint16_t ch) {
		const uint16_t w = (uint16_t)(ch << 1);
		const uint16_t d = (uint16_t)(sext8(peekb(peek((uint16_t)(0x0EC2 + w)))) << 3);
		const uint16_t target = peek((uint16_t)(0xFE06 + w));
		poke(target, (uint16_t)(peek(target) + d));
	};
	for (uint16_t ch = 0; ch <= 3; ch++)
	{
		const uint16_t count = (uint16_t)(0x0ECA + ch);
		pokeb(count, (uint8_t)(peekb(count) - 1));
		if (peekb(count))
		{
			if (ch <= 1) step(ch);
			continue;
		}
		do
		{
			const uint16_t w = (uint16_t)(ch << 1);
			const uint16_t p = (uint16_t)(0x0EC2 + w);
			poke(p, (uint16_t)(peek(p) + 1));
			uint16_t r1 = peekb(peek(p));
			poke(p, (uint16_t)(peek(p) + 1));
			if (r1 == 0x00FF)
			{
				poke(p, (uint16_t)(peek(p) - peekb(peek(p))));
				poke(peek((uint16_t)(0xFE06 + w)), peek(peek(p)));
				poke(p, (uint16_t)(peek(p) + 2));
				if (ch == 0)
				{
					const uint16_t lane = call(0xDDFE, {});
					poke(0x0224, (uint16_t)(peek(0x0224) + peek((uint16_t)(0xFDB2 + (uint16_t)(lane << 1)))));
				}
				else if (ch == 2)
					poke(0x03BA, (uint16_t)(peek(0x03BA) + 0x00A2));
				r1 = peekb(peek(p));
				poke(p, (uint16_t)(peek(p) + 1));
			}
			pokeb(count, (uint8_t)r1);
			if (ch <= 1)
				step(ch);
			else
			{
				const uint16_t v = sext8(peekb(peek(p)));
				const uint16_t target = peek((uint16_t)(0xFE06 + w));
				if (peekb(count))
					poke(target, (uint16_t)(peek(target) + (uint16_t)(v << 1)));
				else
				{
					pokeb(target, (uint8_t)v);
					poke(p, (uint16_t)(peek(p) + 1));
					pokeb((uint16_t)(target + 1), peekb(peek(p)));
					if (ch == 2) poke(0x03BA, (uint16_t)(peek(0x03BA) + 0x00A2));
				}
			}
		} while (!peekb(count));
	}
	const uint16_t x = peek(0x0224);
	poke(0x06FA, x);
	poke(0x06FC, x);
	const uint16_t y = peek(0x0226);
	poke(0x073A, y);
	poke(0x073C, y);
	pokeb(0x0495, (uint8_t)(peekb(0x0495) | 0x18));
	pokeb(0x0496, (uint8_t)(peekb(0x0496) | 0x18));
	poke(0x043A, 0x0001);
	return y;
}

// F:DF40  start_object_scripts(int *y, int *x): pick the script set of the current slot
// (F:BE10; 0x09B6 indexes the bytes at 0x4000, bank 0x1000) and the park (0x0F2E -> 0xFDE3),
// start the four streams, place objects 5 and 6 and return their position through x and y;
// objects 5/6 are set up and F:B4B6(6) runs
uint16_t start_object_scripts(const Args &a)
{
	Helper h(F144);
	set_banks(0x1000, 0x1400);
	call(0xBE10, {});
	const uint16_t set = sext8(peekb((uint16_t)(0x4000 + peek(0x09B6)))) & 0x000F;
	set_banks(0x2400, 0x2800);
	const uint16_t base = peek((uint16_t)(0x4652 + (uint16_t)(set << 1)));
	const uint16_t park = (uint16_t)((sext8(peekb((uint16_t)(0xFDE3 + peekb(0x0F2E)))) & 0x00FF) << 3);
	uint16_t src = (uint16_t)(park + base - 8);
	for (uint16_t ch = 0; ch <= 3; ch++)
	{
		const uint16_t w = (uint16_t)(ch << 1);
		const uint16_t p = (uint16_t)(0x0EC2 + w);
		poke(p, peek(src));
		src = (uint16_t)(src + 2);
		poke(peek((uint16_t)(0xFE06 + w)), peek(peek(p)));
		poke(p, (uint16_t)(peek(p) + 1));
		pokeb((uint16_t)(0x0ECA + ch), 1);
	}
	poke(0x03BA, (uint16_t)(peek(0x03BA) + 0x00A2));
	const uint16_t lane = call(0xDDFE, {});
	const uint16_t x = (uint16_t)(peek(0x0224) + peek((uint16_t)(0xFDB2 + (uint16_t)(lane << 1))));
	poke(0x06FA, x);
	poke(0x06FC, x);
	const uint16_t y = peek(0x0226);
	poke(0x073A, y);
	poke(0x073C, y);
	poke(a.u(1), x);
	poke(a.u(0), y);
	pokeb(0x0475, 0xF0);
	pokeb(0x0476, 0xF0);
	poke(0x03FA, 0);
	poke(0x03FC, 0x7114);
	pokeb(0x0376, 2);
	poke(0x043C, 0);
	poke(0x043A, 0x0001);
	T.r[0] = y; T.r[1] = x;
	return call(0xB4B6, {0x0006});
}

// F:E036: in state 1 (0x018F) with 0x031F and 0x0377 clear, 0x0EB4 counts down; at 0
// F:CE24(0, 0, 4) and F:C4E4 run, object 7 is set up, 0x0F22 = 250, the counter 0x0EB3 steps
// (0..5) and A:5018 (bank 0x3800) runs with the same arguments
uint16_t countdown_0eb4(const Args &)
{
	if (peekb(0x018F) != 1) return T.r[0];
	if (peekb(0x031F)) return T.r[0];
	if (peekb(0x0377)) return T.r[0];
	const uint16_t t = (uint16_t)(peek(0x0EB4) - 1);
	poke(0x0EB4, t);
	if (t) return T.r[0];
	push(0x0004);
	push(0x0000);
	push(0x0000);
	call(0xCE24, {});
	call(0xC4E4, {});
	pokeb(0x0477, (uint8_t)(peekb(0x0477) | 0xC0));
	const uint8_t v = (uint8_t)(peekb(0x0777) & 0x7F);
	pokeb(0x0777, 0);
	pokeb(0x0777, v);
	pokeb(0x07D7, 0xFF);
	pokeb(0x0EB2, 1);
	poke(0x0EB4, 0);
	poke(0x0F22, 0x00FA);
	const uint8_t n = (uint8_t)(peekb(0x0EB3) + 1);
	pokeb(0x0EB3, n);
	if ((int8_t)n > 5) pokeb(0x0EB3, 0);
	set_banks(0x3800, 0x3C00);
	T.r[0] = 0x3C00;
	const uint16_t r0 = call(0x5018, {});
	T.r[6] = (uint16_t)(T.r[6] + 6);
	return r0;
}

// F:E13A  set_object7_motion(int vx0, int vy0, int col, int row): object 7 velocity (0x04FE,
// 0x057E) from the 8-column tables at 0x4138 / 0x4238 (bank A 0x2400), doubled, plus vx0 /
// vy0; a new row (0x07D7) also picks its animation 0x03BE (0x5238, or 0x527E by 0xFE18
// [0x0EB3] for col >= 4, bank A 0) and the bytes 0x0777 / 0x0797 = 4.  Banks end as
// 0x3800 / 0x3C00.
uint16_t set_object7_motion(const Args &a)
{
	Helper h(F138);
	poke(0x01D2, 0x2400);
	ww(0x1400, 0x2400);
	const uint16_t row = a.u(3);
	const uint16_t idx = (uint16_t)((uint16_t)((row << 3) + a.u(2)) << 1);
	poke(0x04FE, (uint16_t)((uint16_t)(peek((uint16_t)(0x4138 + idx)) << 1) + a.u(1)));
	poke(0x057E, (uint16_t)((uint16_t)(peek((uint16_t)(0x4238 + idx)) << 1) + a.u(0)));
	if (peekb(0x07D7) != row)
	{
		set_banks(0x0000, 0x0400);
		uint16_t r1 = 0;
		if ((int16_t)a.u(2) >= 4) r1 = peekb((uint16_t)(0xFE18 + sext8(peekb(0x0EB3))));
		if ((int16_t)r1 < 5)
		{
			const uint16_t i = (uint16_t)((uint16_t)(((int16_t)row >> 1) + (r1 << 3)) << 1);
			poke(0x03BE, peek((uint16_t)(0x5238 + i)));
		}
		else
			poke(0x03BE, peek((uint16_t)(0x527E + (uint16_t)(r1 << 1))));
		pokeb(0x0797, 4);
		pokeb(0x0777, 4);
		pokeb(0x07D7, (uint8_t)row);
	}
	set_banks(0x3800, 0x3C00);
	return 0x3C00;
}

// F:E5D2  skater_anim_a(int t): animation pointer for terrain t: 0x4F64 + 2 * 0x01F8 when
// t & 7 is 0, else 0x4C4C[t >> 3] + 0xFE2D[t & 7] + 2 * (0x01F8 & 7) (bank B 0x0400)
uint16_t skater_anim_a(const Args &a)
{
	Helper h(F162);
	const uint16_t sa = peek(0x01D2), sb = peek(0x01D4);
	set_banks(0x0000, 0x0400);
	const uint16_t dir = sext8(peekb(0x01F8)) & 0x0007;
	const uint16_t t = a.u(0);
	uint16_t r;
	if (!(t & 7))
		r = (uint16_t)((uint16_t)(sext8(peekb(0x01F8)) << 1) + 0x4F64);
	else
	{
		const uint16_t row = peek((uint16_t)(0x4C4C + (uint16_t)(((int16_t)t >> 3) << 1)));
		r = (uint16_t)(row + peekb((uint16_t)(0xFE2D + (t & 7))) + (uint16_t)(dir << 1));
	}
	restore_banks(sa, sb);
	return r;
}

// F:E648  skater_anim_b(int t): like F:E5D2 with the tables 0x4000, 0x4020 and 0xFE35; on
// terrain 2 / 10 (0x0327) at speed 0x01B4 >= 0x500 the tables 0x43E8 / 0x4408 are used
uint16_t skater_anim_b(const Args &a)
{
	Helper h(F162);
	const uint16_t sa = peek(0x01D2), sb = peek(0x01D4);
	push(sb);
	set_banks(0x0000, 0x0400);
	const uint16_t dir = sext8(peekb(0x01F8));
	const uint16_t t = a.u(0);
	uint16_t r;
	if (!(t & 7))
		r = (uint16_t)((uint16_t)(dir << 1) + 0x4000);
	else
	{
		const uint8_t terrain = peekb(0x0327);
		if ((terrain == 2 || terrain == 10) && (int16_t)peek(0x01B4) >= 0x0500)
			r = (uint16_t)((uint16_t)(dir << 1) + (terrain == 2 ? 0x43E8 : 0x4408));
		else
		{
			const uint16_t row = peek((uint16_t)(0x4020 + (uint16_t)(((int16_t)t >> 3) << 1)));
			r = (uint16_t)(row + peekb((uint16_t)(0xFE35 + (t & 7))) + (uint16_t)(dir << 1));
		}
	}
	restore_banks(sa, sb);
	return r;
}

// F:EF24  add_score(int n): the score (long at 0x023E) grows by n; each time it reaches the
// next bonus threshold (long at 0x0244) a bonus is counted in 0x025A (up to 4), the next
// threshold is the step 0x5212[0x025A + 0x0FE3] (bank 0x3800) further, and, outside state 5,
// B:72CA(1) announces it
uint16_t add_score(const Args &a)
{
	Helper h(F162);
	const uint16_t sa = peek(0x01D2), sb = peek(0x01D4);
	push(sa);
	push(sb);
	pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) | 0x01));
	T.r[1] = 0x0240; T.r[2] = 0x023E; T.r[3] = 0; T.r[4] = a.u(0);
	call(0x80C8, {});                                  // add32
	poke(0x024A, peek(0x0246));
	poke(0x0248, peek(0x0244));
	uint16_t r0;
	for (;;)
	{
		T.r[1] = 0x024A; T.r[2] = 0x0248; T.r[3] = peek(0x0240); T.r[4] = peek(0x023E);
		call(0x80D0, {});                              // sub32: distance to the threshold
		r0 = peek(0x024A);
		if ((int16_t)r0 > 0) break;
		if (r0 == 0 && peek(0x0248) != 0) break;
		pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) | 0x02));
		const uint8_t bonus = peekb(0x025A);
		pokeb(0x025A, (uint8_t)(bonus + (bonus < 4 ? 1 : 0)));
		set_banks(0x3800, 0x3C00);
		const uint16_t i = (uint16_t)((uint16_t)(peekb(0x025A) + peekb(0x0FE3)) << 1);
		T.r[0] = i;
		T.r[1] = 0x0246; T.r[2] = 0x0244; T.r[3] = 0; T.r[4] = peek((uint16_t)(0x5212 + i));
		call(0x80C8, {});                              // add32: next threshold
		poke(0x024A, peek(0x0246));
		poke(0x0248, peek(0x0244));
		if (peekb(0x018F) == 5) continue;
		T.r[5] = peek(0x0188);
		call(0x72CA, {0x0001});
		poke(0x0188, T.r[5]);
	}
	restore_banks(sa, sb);
	return r0;
}

// F:F064  start_object4(int kind): object 4 appears next to the skater (0x0400 right and 0x0300
// down, 0x0800 more on terrain 5-7) with kind (0..9; -1 on screen 7, negative: nothing) in
// 0x0394; if it was not active, it is activated (type 0x53) and F:B4B6(4) runs.  Returns kind.
uint16_t start_object4(const Args &a)
{
	Helper h(F138);
	const uint16_t arg = (uint16_t)(a.sp + 2);
	if (peekb(0x0F59) == 7) poke(arg, 0xFFFF);
	if ((int16_t)peek(arg) < 0) return peek(arg);
	if ((int16_t)peek(arg) > 9) poke(arg, 0x0009);
	poke(0x0438, 0x001E);
	poke(0x0638, 0x8040);
	poke(0x06F8, (uint16_t)(peek(0x06F0) + 0x0400));
	poke(0x0738, (uint16_t)(peek(0x0730) + 0x0300));
	if ((sext8(peekb(0x0327)) & 7) >= 5) poke(0x0738, (uint16_t)(peek(0x0738) + 0x0800));
	poke(0x06B8, peek(0x06B0));
	poke(0x03B8, 0x576A);
	pokeb(0x0394, peekb(arg));
	pokeb(0x0794, 3);
	pokeb(0x0774, 3);
	pokeb(0x07B4, 0);
	poke(0x03F8, 0);
	pokeb(0x0474, (uint8_t)(peekb(0x0470) & 0xC0));
	pokeb(0x0474, (uint8_t)(peekb(0x0474) | 0x20));
	pokeb(0x0494, (uint8_t)(peekb(0x0494) | 0x18));
	if (!peekb(0x0374))
	{
		const uint16_t sa = peek(0x01D2), sb = peek(0x01D4);
		T.r[1] = sa; T.r[2] = sb;
		pokeb(0x0374, 0x53);
		T.r[0] = peekb(0x0470) & 0xC0;
		call(0xB4B6, {0x0004});
		restore_banks(sa, sb);
	}
	return peek(arg);
}

// F:D19C: set up the slots of a level: object queue state, entries (F:D8D2, F:D880),
// slots 0-3 (F:CF5E), the four fixed objects (F:CE24 from 0x70E4/0x70E8, bank B 0x0400);
// for a park (0x0F2E) above the last one set up (0x0E94) also the slots 4-0xAE whose type
// has bit 0 of 0x6438 clear, the park's objects (0x708C and the list at 0x6B9E) and the
// object counters 0x0E7E-0x0E86 (total 0x0E93) from 0x68D0; finally A:48CC (bank 0x1800)
uint16_t setup_level_slots(const Args &)
{
	Helper h(F162);
	const uint16_t sa = peek(0x01D2), sb = peek(0x01D4);
	push(sa);
	push(sb);
	pokeb(0x0E98, 0x40);
	poke(0x0E96, 0x020E);
	poke(0x09AC, 0);
	pokeb(0x0E6B, 0xFF);
	pokeb(0x0E6C, 5);
	pokeb(0x0E68, 0);
	pokeb(0x0E6A, 0xFF);
	call(0xD8D2, {});
	call(0xD880, {});
	for (int16_t i = 3; i >= 0; i--)
		if (peekb((uint16_t)(0x09B8 + i)))
		{
			T.r[1] = (uint16_t)i;
			call(0xCF5E, {(uint16_t)i});
		}
	set_banks(0x0000, 0x0400);
	for (int16_t i = 3; i >= 0; i--)
	{
		T.r[1] = (uint16_t)i;
		T.r[0] = (uint16_t)(i << 1);
		const uint16_t b = peekb((uint16_t)(0x70E4 + i));
		call(0xCE24, {0, peek((uint16_t)(0x70E8 + 2 * i)), b});
	}
	const int16_t done = (int16_t)sext8(peekb(0x0E94));
	if ((int16_t)peekb(0x0F2E) > done)
	{
		for (int16_t i = 0x00AE; i >= 4; i--)
		{
			const uint16_t t = sext8(peekb((uint16_t)(0x09B8 + i)));
			if (!t) continue;
			set_banks(0x0000, 0x0400);
			if (!(peekb((uint16_t)(0x6438 + t)) & 0x01))
			{
				T.r[1] = t; T.r[2] = (uint16_t)i; T.r[3] = t;
				call(0xCF5E, {(uint16_t)i});
			}
		}
		set_banks(0x0000, 0x0400);
		uint16_t p = (uint16_t)((uint16_t)(peekb((uint16_t)(0xFDE3 + peekb(0x0F2E))) << 3) + 0x708C - 8);
		for (int n = 4; n >= 1; n--)
		{
			T.r[1] = p; T.r[2] = (uint16_t)n;
			call(0xCE24, {0, peek(p), 0x0001});
			p = (uint16_t)(p + 2);
		}
		uint16_t q = peek((uint16_t)(0x6B9E + (uint16_t)(peekb(0x0F2E) << 1)));
		while (peek(q) != 0xFFFF)
		{
			T.r[1] = q;
			const uint16_t w = peek(q);
			call(0xCE24, {0, peek((uint16_t)(q + 2)), w});
			q = (uint16_t)(q + 4);
		}
		pokeb(0x0E93, 0);
		for (uint16_t i = 0x14; i >= 1; i--) pokeb((uint16_t)(0x0E7D + i), 0);
		const uint16_t table = (uint16_t)(0x68D0 + (uint16_t)(peekb(0x0F2E) << 1));
		for (uint16_t i = 9; i >= 1; i--)
		{
			const uint8_t v = peekb((uint16_t)(i + peek(table) - 1));
			pokeb((uint16_t)(0x0E7D + i), v);
			pokeb(0x0E93, (uint8_t)(peekb(0x0E93) + v));
		}
		pokeb(0x0E94, peekb(0x0F2E));
	}
	set_banks(0x1800, 0x1C00);
	T.r[0] = 0x1C00;
	const uint16_t r0 = call(0x48CC, {});
	restore_banks(sa, sb);
	return r0;
}

// F:E78E  query_on_button_press(): on a new press of a button (0x0193 bits 0xC0) while 0x0190
// is clear, query the sound CPU (F:8E6A, command 10, F:8F50; then 1 / 2 for button 0x80 /
// 0x40, or 0x80 | (0x0F2E - 1) in state 9; five answer bytes, F:8F62, F:8E72).  A first
// answer with bit 7 set returns 1 and sets the state 0x018F to 6 / 7 (in state 9 it needs
// answer bit 6 instead).  Without a press 0x0190 is cleared.  Waits for the sound CPU, so the
// cycles are charged here.
uint16_t query_on_button_press(const Args &)
{
	Helper h(F138);
	constexpr int ENTRY = 30 + 24 + 24 + 27 + 30;          // JSR R1,@#F138 and the helper
	T.icount -= ENTRY + 30 + 12;                          // BITB, BEQ
	if (!(peekb(0x0193) & 0xC0))
	{
		T.icount -= 27 + 12;
		pokeb(0x0190, 0);
		return 0;
	}
	T.icount -= 24 + 12;
	if (peekb(0x0190))
	{
		T.icount -= 12;
		return 0;
	}
	uint16_t r1 = T.r[1];
	T.icount -= 30;
	call(0x8E6A, {});
	T.icount -= 30 + 30;
	push(0x000A);
	call(0x8EA2, {});
	T.icount -= 30;
	call(0x8F50, {});
	T.icount -= 30 + 12;
	if (peekb(0x018F) == 9)
	{
		T.icount -= 12 + 24 + 12 + 21 + 27 + 12;
		T.r[0] = (uint16_t)(peekb(0x0F2E) - 1);
		poke(T.r[6], (uint16_t)(T.r[0] | 0x0080));
	}
	else
	{
		T.icount -= 24 + 12;
		if (peekb(0x0193) & 0x80)
		{
			T.icount -= 18 + 27 + 12;
			r1 = 6;
			poke(T.r[6], 0x0001);
		}
		else
		{
			T.icount -= 18 + 27;
			r1 = 7;
			poke(T.r[6], 0x0002);
		}
	}
	T.r[1] = r1;
	T.icount -= 30;
	call(0x8EA2, {});
	T.icount -= 30;
	const uint16_t answer = call(0x8ED2, {});
	T.r[2] = answer;
	T.icount -= 12;
	for (int i = 0; i < 4; i++)
	{
		T.icount -= 30;
		call(0x8ED2, {});
	}
	T.icount -= 30;
	call(0x8F62, {});
	T.icount -= 30;
	call(0x8E72, {});
	T.r[6] = (uint16_t)(T.r[6] + 2);
	T.icount -= 12 + 12;
	if (!(answer & 0x80))
	{
		T.icount -= 18 + 12 + 12;
		return 0;
	}
	T.icount -= 30 + 12;
	if (peekb(0x018F) != 9)
	{
		T.icount -= 27 + 12;
		pokeb(0x018F, (uint8_t)r1);
	}
	else
	{
		T.icount -= 18 + 12;
		if (!(answer & 0x40))
		{
			T.icount -= 18 + 12 + 12;
			return 0;
		}
	}
	T.icount -= 18 + 18;
	return 1;
}

// ---------------------------------------------------------------- assembly interface

// F:D35E (registers): place one waiting object.  The byte counters 0x0E7E-0x0E90 (total
// 0x0E93) count the objects still to place, by kind 19..1.  For the first kind with objects
// left, a random cell (0x0E92 += F:9152, masked with 0xFD95[kind]) of the kind's list 0x6948
// (bank B 0x0400) is tried: kinds 1-3 need its column bits (>> 4 & 3) to differ from the
// tile bank 0x01CE - 1, the others a cell outside the window 0x099A-0x099C / 0x0996-0x0998.
// The object comes from F:CE24(flag, cell, type) (kinds up to 10: type from the park's list
// 0x6862, flag 1; others 0x6935, flag 0); when it is placed (R0 = 0) the counters drop by 1.
void spawn_counted_object()
{
	const uint16_t sp0 = T.r[6];
	T.r[6] = (uint16_t)(sp0 - 8);                       // SUB #8,SP: locals
	uint16_t &r0 = T.r[0], &r1 = T.r[1], &r2 = T.r[2], &r3 = T.r[3], &r4 = T.r[4], &r5 = T.r[5];
	if (peekb(0x0E93))
		for (r4 = 0x0013;;)
		{
			const uint16_t counter = (uint16_t)(0x0E7D + r4);
			if (peekb(counter))
			{
				set_banks(0x0000, 0x0400);
				r0 = 0x0400;
				call(0x9152, {});                      // next_random
				r5 = peekb(0x0E92);
				r0 = (uint16_t)(r0 + r5);
				pokeb(0x0E92, (uint8_t)r0);
				r1 = peek(0x0E92);
				r0 = (uint16_t)~sext8(peekb((uint16_t)(0xFD95 + r4)));
				r1 &= (uint16_t)~r0;
				r1 &= 0x00FF;
				r0 = (uint16_t)(r4 << 1);
				r5 = (uint16_t)((uint16_t)(r1 << 1) + peek((uint16_t)(0x6948 + r0)));
				r3 = peek(r5);
				bool skip = false;
				if ((int16_t)r4 > 3)
				{
					r2 = r3 & 0x000F;
					if ((int16_t)r2 >= (int16_t)peek(0x099A) && (int16_t)r2 <= (int16_t)peek(0x099C))
					{
						r2 = r3 & 0x01F0;
						skip = (int16_t)r2 >= (int16_t)peek(0x0996) && (int16_t)r2 < (int16_t)peek(0x0998);
					}
				}
				else
				{
					r5 = (uint16_t)((int16_t)r1 >> 4);
					r0 = (uint16_t)(peek(0x01CE) - 1);
					skip = (r5 & 3) == r0;
				}
				if (!skip)
				{
					uint16_t flag, type;
					if ((int16_t)r4 > 10)
					{
						type = peekb((uint16_t)(0x6935 + r4));
						flag = 0;
					}
					else
					{
						r0 = (uint16_t)(peek((uint16_t)(0x6862 + (uint16_t)(peekb(0x0F2E) << 1))) + r4);
						type = peekb((uint16_t)(r0 - 1));
						flag = 1;
					}
					call(0xCE24, {flag, r3, type});
					if (r0 == 0)
					{
						pokeb(counter, (uint8_t)(peekb(counter) - 1));
						r0 = (uint16_t)(sext8(peekb(0x0E93)) - 1);
						pokeb(0x0E93, (uint8_t)r0);
						break;
					}
				}
			}
			r4 = (uint16_t)(r4 - 1);
			if (r4 == 0) break;
		}
	T.r[6] = sp0;
	fl::add((uint16_t)(sp0 - 8), 8);                    // ADD #8,SP
}

// F:D4E4 (registers): position of the skater (object 0) relative to object 0x0EA0.  R5 = the
// object's shape record (table 0x4354 through 0xFDBA[0x07D0[obj]], bank A 0x2400), R4 =
// column (x distance + 0x0A00 - record[-4]: high byte / 2) also in 0x0E9E, R3 = height
// (record[-2] - (y + z distance + 0x0300)) also in 0x0E9C; when positive, R3 becomes the
// row's offset byte from 0x4338.  R1 is preserved.
void offset_to_object_0ea0()
{
	const uint16_t saved_r1 = T.r[1];
	set_banks(0x2400, 0x2800);
	uint16_t r1 = sext8(peekb(0x0EA0));
	uint16_t r0 = peekb((uint16_t)(0x07D0 + r1));
	uint16_t r5 = (uint16_t)(sext8(peekb((uint16_t)(0xFDBA + r0))) + 0x4354);
	r5 = peek(r5);
	r1 = (uint16_t)(r1 << 1);
	r0 = (uint16_t)(peek(0x06F0) - peek((uint16_t)(0x06F0 + r1)));
	uint16_t r4 = (uint16_t)(r0 + 0x0A00 - peek((uint16_t)(r5 - 4)));
	r0 = sext8((uint8_t)(r4 >> 8));
	r4 = (uint16_t)((int16_t)r0 >> 1);
	r0 = (uint16_t)(peek(0x0730) + peek(0x06B0) - peek((uint16_t)(0x0730 + r1)));
	uint16_t r3 = (uint16_t)-(uint16_t)(r0 + 0x0300);
	r3 = fl::add(r3, peek((uint16_t)(r5 - 2)));
	poke(0x0E9E, r4);
	poke(0x0E9C, r3);
	if ((int16_t)r3 > 0)
	{
		r0 = (uint16_t)((int16_t)sext8((uint8_t)(r3 >> 8)) >> 1);
		r3 = peekb((uint16_t)(0x4338 + r0));
		T.psw &= ~CF;                                  // CLR R3
	}
	T.r[0] = r0; T.r[3] = r3; T.r[4] = r4; T.r[5] = r5;
	T.r[1] = saved_r1;
	fl::mov(saved_r1);
}

// F:D566 (registers): the skater against object -0x0EA0 (0x0EA0 negative: just reached).
// F:AD70 (R0 = 0, R1 = -obj) gives a side bit kept on the stack; 0x0EA0 is made positive
// (copy in 0x0EA1, 0x0EA2 = 5) and F:D4E4 locates the skater.  Outside the shape 0x0EA0 is cleared.
// Otherwise the shape's height bytes under the skater are followed up (rows of 0x12 bytes):
// at or above the result the skater is let go (0x0EA0 cleared, direction 4 / 12 by the side,
// speed 0x01B4 from 0x7C90[0x025D] in bank B 0x2C00, then F:A2F0); below it its height
// 0x06B0 is set and F:D778 runs.
void contact_object_0ea0()
{
	T.r[6] = (uint16_t)(T.r[6] - 2);                     // TST -(SP): local for the side
	set_banks(0x0000, 0x0400);
	T.r[1] = (uint16_t)-sext8(peekb(0x0EA0));
	T.r[0] = 0;
	call(0xAD70, {});
	ww(T.r[6], T.r[0]);
	pokeb(0x0EA0, (uint8_t)-peekb(0x0EA0));
	pokeb(0x0EA1, peekb(0x0EA0));
	pokeb(0x0EA2, 5);
	call(0xD4E4, {});
	T.r[0] = T.r[4] | T.r[3];
	if ((int16_t)T.r[0] < 0)
		pokeb(0x0EA0, 0);
	else
	{
		T.r[0] = peek(0x06B0);
		T.r[1] = (uint16_t)(T.r[3] + T.r[5] + T.r[4]);
		if (peekb(T.r[1]))
			for (;;)
			{
				T.r[3] = (uint16_t)(T.r[3] - 0x0012);
				T.r[0] = (uint16_t)(T.r[0] + 0x0200);
				T.r[1] = (uint16_t)(T.r[3] + T.r[5] + T.r[4]);
				T.r[2] = peekb(T.r[1]) & 0x007F;
				if (rw(T.r[6]) & 1)
				{
					if (T.r[2] != 0 && (int16_t)T.r[3] > 0) continue;
					T.r[0] = (uint16_t)(T.r[0] - 0x0100);
					break;
				}
				T.r[1] = 7 & (uint16_t)~T.r[2];
				if (T.r[1] != 0) break;
			}
		if ((int16_t)T.r[0] >= (int16_t)peek(0x06B2))
		{
			pokeb(0x0EA0, 0);
			pokeb(0x031D, (rw(T.r[6]) & 1) ? 4 : 0x0C);
			T.r[0] = 0x2C00;
			poke(0x01D4, 0x2C00);
			ww(0x1402, 0x2C00);
			T.r[0] = (uint16_t)(sext8(peekb(0x025D)) << 1);
			poke(0x01B4, peek((uint16_t)(0x7C90 + T.r[0])));
			call(0xA2F0, {});
		}
		else
		{
			poke(0x06B0, T.r[0]);
			call(0xD778, {});
		}
	}
	const uint16_t side = rw(T.r[6]);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	fl::tst(side);
}

// F:DB4E (registers): sprite palette (0x1000-0x107F) = the colours at 0x7EEC-0x7FAB (bank B
// 0x0400; the first 16 from 0x7F0C-0x7F2B of bank 0x1C00 when 0x01F5 is set) minus
// sext(0x0EA7), each group of 16 in reverse order.  Banks restored.
void fade_sprite_palette()
{
	push(peek(0x01D2));
	push(peek(0x01D4));
	set_banks(0x0000, 0x0400);
	T.r[0] = 0x0400;
	T.r[3] = sext8(peekb(0x0EA7));
	T.r[2] = 0x7FAC;
	for (int16_t n = 0x2F; n >= 0; n--)
	{
		T.r[2] = (uint16_t)(T.r[2] - 2);
		T.r[1] = (uint16_t)(peek(T.r[2]) - T.r[3]);
		T.r[4] = (uint16_t)(((uint16_t)n << 1) ^ 0x001E);
		T.r[0] = 0x001E;
		poke((uint16_t)(0x1020 + T.r[4]), T.r[1]);
	}
	if (peekb(0x01F5))
	{
		set_banks(0x1800, 0x1C00);
		T.r[0] = 0x1C00;
		T.r[2] = 0x7F4C;
	}
	for (int16_t n = 0x0F; n >= 0; n--)
	{
		T.r[2] = (uint16_t)(T.r[2] - 2);
		T.r[1] = (uint16_t)(peek(T.r[2]) - T.r[3]);
		T.r[4] = (uint16_t)(((uint16_t)n << 1) ^ 0x001E);
		T.r[0] = 0x001E;
		poke((uint16_t)(0x1000 + T.r[4]), T.r[1]);
	}
	T.r[5] = 0xFFFF;
	poke(0x01D2, rw((uint16_t)(T.r[6] + 2)));
	poke(0x01D4, pop());
	ww(0x1400, peek(0x01D2));
	ww(0x1402, peek(0x01D4));
	fl::tst(pop());
}

// F:DBE2 (registers): one step of the palette fade towards 0x0EA8 (negative: none): on odd
// frames the level 0x0EA7 moves one step and the sprite palette is redone (F:DB4E); on even
// frames a reached target is cleared (0xFF) and the playfield palette is redone (F:DB00)
void step_palette_fade()
{
	const uint8_t target = peekb(0x0EA8);
	fl::tstb(target);
	if (target & 0x80) return;
	T.r[0] = sext8(peekb(0x0EA7));
	T.r[1] = sext8(peekb(0x0EA8));
	if (peek(0x01C4) & 1)
	{
		T.r[1] = (int16_t)T.r[1] > (int16_t)T.r[0] ? (uint16_t)(T.r[0] + 1) : (uint16_t)(T.r[0] - 1);
		pokeb(0x0EA7, (uint8_t)T.r[1]);
		call(0xDB4E, {});
		return;
	}
	if (T.r[1] == T.r[0]) pokeb(0x0EA8, 0xFF);
	call(0xDB00, {});
}

// F:DC3C (registers; run from 0x0EB0 by F:DDEC): load palette block 0x0EAC from colour set
// 0x0EAE (bank B 0x1C00): blocks 0-7 are 4 text colours (0x1080 + 8 * block, from 0x7F6C),
// 8-15 16 playfield colours (0x1100, from 0x7EAC), 16 on 16 sprite colours (0x1000,
// reversed, from 0x7F0C).  Banks restored.
void load_palette_block()
{
	uint16_t *r = T.r;
	r[5] = peek(0x01D2);
	push(peek(0x01D4));
	r[1] = peek(0x0EAC);
	r[0] = peek(0x0EAE);
	r[2] = 0x1800;
	poke(0x01D2, 0x1800);
	ww(0x1400, 0x1800);
	r[2] = 0x1C00;
	poke(0x01D4, 0x1C00);
	ww(0x1402, 0x1C00);
	if ((int16_t)r[1] <= 7)
	{
		r[3] = (uint16_t)(r[1] << 2);
		r[4] = (uint16_t)(r[0] << 2);
		for (int16_t n = 3; n >= 0; n--)
		{
			r[1] = (uint16_t)((uint16_t)(n + r[3]) << 1);
			r[0] = (uint16_t)((uint16_t)(n + r[4]) << 1);
			poke((uint16_t)(0x1080 + r[1]), peek((uint16_t)(0x7F6C + r[0])));
		}
	}
	else
	{
		r[4] = (uint16_t)(r[0] << 4);
		const bool pf = (int16_t)r[1] <= 15;
		r[3] = (uint16_t)((uint16_t)(r[1] - (pf ? 8 : 16)) << 4);
		for (int16_t n = 15; n >= 0; n--)
		{
			r[1] = (uint16_t)((uint16_t)(n + r[3]) << 1);
			if (!pf) r[1] ^= 0x001E;
			r[0] = (uint16_t)((uint16_t)(n + r[4]) << 1);
			if (pf) poke((uint16_t)(0x1100 + r[1]), peek((uint16_t)(0x7EAC + r[0])));
			else poke((uint16_t)(0x1000 + r[1]), peek((uint16_t)(0x7F0C + r[0])));
		}
	}
	r[2] = 0xFFFF;
	poke(0x01D2, r[5]);
	poke(0x01D4, pop());
	ww(0x1400, peek(0x01D2));
	const uint16_t v = peek(0x01D4);
	ww(0x1402, v);
	T.psw = (uint8_t)((T.psw & ~CF) | ((r[4] >> 15) & 1));      // C of the last ASL R0
	fl::mov(v);
}

// F:DDEC (registers): run the palette job queued in 0x0EB0 (a routine address) and clear it
void run_palette_job()
{
	T.r[0] = peek(0x0EB0);
	fl::mov(T.r[0]);
	if (!T.r[0]) return;
	call(T.r[0], {});
	poke(0x0EB0, 0);
	fl::clr();
}

// F:E254 (registers): the skater's phase 0x01EB.  After F:9236, a press of button 0x40
// (0x0193) sets 0x0332 (2, or 1 when already set; terrain bit 4 also sets 0x0324 bit 7).
// With 0x0320 clear and 0x0330 != 1 phase 1 starts (0x0320 = 0x0330 = 1, timers from 0xFE3E
// [0x0331]; from 0x01EA = 2 first: speed made positive, direction from 0x01F8, 0x01EA = 1,
// F:E238, sound 0x58, B:7E44).  Otherwise 0x0332 = 2 with 0x01EA = 1 on terrain classes 0-2
// (not on level 3) starts phase 6 (z speed 0x05B2 > 0x30, raised by 0x7C70[0x025F]) or 4
// (speed kept in 0x02E8 and stopped).  Then the animation pointers 0x03B0-0x03B6 are set
// from the terrain (F:E5D2, F:E648) and the phase steps (jump table at 0xFE54, phases 0-11).
void update_skater_phase()
{
	uint16_t *r = T.r;
	call(0x9236, {});
	set_banks(0x0000, 0x0400);
	r[0] = 0x0400;
	if (!peekb(0x031F))
	{
		if (peekb(0x0193) & 0x40)
		{
			pokeb(0x0332, peekb(0x0332) ? 1 : 2);
			if (peekb(0x0327) & 4) pokeb(0x0324, (uint8_t)(peekb(0x0324) | 0x80));
		}
		else
			pokeb(0x0332, 0);
		if (!peekb(0x0320) && peekb(0x0330) != 1)
		{
			if (peekb(0x01EA) == 2)
			{
				if ((int16_t)peek(0x01B4) < 0) poke(0x01B4, (uint16_t)-peek(0x01B4));
				pokeb(0x031D, peekb(0x01F8));
				pokeb(0x01EA, 1);
				call(0xE238, {});                      // reset_skater_state
				push(0x0058);
				call(0x8E78, {});                      // queue_sound(0x58)
				set_banks(0x3800, 0x3C00);
				r[0] = 0x3C00;
				call(0x7E44, {});
				set_banks(0x0000, 0x0400);
				r[0] = 0x0400;
				r[6] = (uint16_t)(r[6] + 2);
			}
			pokeb(0x0320, 1);
			pokeb(0x01EB, 1);
			pokeb(0x0330, 1);
			pokeb(0x07B2, 0);
			r[0] = sext8(peekb((uint16_t)(0xFE3E + sext8(peekb(0x0331)))));
			pokeb(0x0792, (uint8_t)r[0]);
			pokeb(0x0772, (uint8_t)r[0]);
		}
		else if (peekb(0x0332) == 2 && peekb(0x01EA) == 1)
		{
			r[0] = sext8(peekb(0x0327)) & 7;
			if (r[0] < 3 && peekb(0x0F59) != 3)
			{
				call(0xE238, {});                      // reset_skater_state
				r[0] = 4;
				pokeb(0x0330, 4);
				if ((int16_t)peek(0x05B2) > 0x0030)
				{
					pokeb(0x01EB, 6);
					pokeb(0x07B1, 1);
					call(0x9FB4, {});
					r[0] = 0x2C00;
					poke(0x01D4, 0x2C00);
					ww(0x1402, 0x2C00);
					r[0] = sext8(peekb(0x025F));
					r[1] = peekb((uint16_t)(0x7C70 + r[0]));
					poke(0x05B2, (uint16_t)(peek(0x05B2) + r[1]));
					r[0] = 0x0400;
					poke(0x01D4, 0x0400);
					ww(0x1402, 0x0400);
				}
				else
				{
					pokeb(0x01EB, 4);
					pokeb(0x07B1, 0);
					r[1] = peek(0x01B4);
					poke(0x02E8, r[1]);
					poke(0x01B4, 0);
					if ((int16_t)r[1] < 0) r[1] = (uint16_t)-r[1];
					r[1] = (int16_t)r[1] >= 0x01C0 ? 2 : 8;
					pokeb(0x0791, (uint8_t)r[1]);
					pokeb(0x0771, (uint8_t)r[1]);
					pokeb(0x0792, (uint8_t)r[1]);
					pokeb(0x0772, (uint8_t)r[1]);
				}
			}
		}
	}
	const uint8_t state = peekb(0x031F);
	if (state != 1 && state != 5)
	{
		r[1] = sext8(peekb(0x01F8)) & 7;
		r[0] = (uint16_t)(r[1] << 1);
		poke(0x03F0, peek((uint16_t)(0x70F0 + r[0])));
		push(peekb(0x0327));
		call(0xE5D2, {});                              // skater_anim_a(terrain)
		r[2] = r[0];
		bool still = peekb(0x01EA) == 2;
		if (!still)
		{
			r[0] = 7 & (uint16_t)~peekb(0x0327);
			still = r[0] == 0;
		}
		poke(0x03B0, still ? 0x50FA : r[2]);
		r[3] = sext8(peekb(0x01EB));
		if (r[3] & 0x0040)
		{
			r[0] = 0x1800;
			poke(0x01D2, 0x1800);
			ww(0x1400, 0x1800);
			call(0x54CE, {});
		}
		else
		{
			r[0] = (uint16_t)(r[3] << 1);
			auto next_phase = [r]() {
				r[0] = (uint16_t)(r[3] + 1);
				pokeb(0x01EB, (uint8_t)r[0]);
				pokeb(0x07B1, (uint8_t)(peekb(0x07B1) + 1));
			};
			switch (r[3])
			{
			case 0:
				if (!(peekb(0x0327) & 7) && (int16_t)peek(0x01C8) >= 8)
				{
					r[0] = (uint16_t)(((int16_t)sext8(peekb(0x01F8)) >> 1) << 1);
					const bool back = (peekb(0x0321) & 0x80) != 0;
					r[1] = (uint16_t)(r[0] + (back ? 0x42B0 : 0x42C0));
					r[2] = (uint16_t)(r[0] + (back ? 0x4E1C : 0x4E2C));
					poke(0x03B0, r[2]);
				}
				else
				{
					ww(r[6], peekb(0x0327));
					call(0xE648, {});                  // skater_anim_b(terrain)
					r[1] = r[0];
				}
				poke(0x03B2, r[2]);
				poke(0x03B4, r[1]);
				break;
			case 1:
				if (peekb(0x07B2) == 2)
				{
					r[0] = (uint16_t)(r[3] + 1);
					pokeb(0x01EB, (uint8_t)r[0]);
				}
				break;
			case 2: case 3:
				if (peekb(0x07B2) == 3) call(0xE238, {});
				break;
			case 4:
				if (peekb(0x07B1))
				{
					call(0xE238, {});
					pokeb(0x0330, 4);
					pokeb(0x01EB, 5);
				}
				else
					poke(0x03B0, 0x50FA);
				break;
			case 5: case 6:
				r[0] = (uint16_t)(peek(0x05B2) - peek(0x05B0));
				if (r[0] & 0x8000) next_phase();
				break;
			case 7:
				r[0] = (uint16_t)(peek(0x06B2) - peek(0x06B0));
				if ((int16_t)r[0] <= 0x0600) next_phase();
				break;
			case 8:
				if (peekb(0x01EA) == 1)
				{
					r[0] = 0x0014;
					pokeb(0x0791, 0x14);
					pokeb(0x0771, 0x14);
					next_phase();
				}
				break;
			case 9:
				if (peekb(0x0791) == 2) call(0xE238, {});
				break;
			case 10:
				if (peekb(0x07B1) == 6)
				{
					pokeb(0x0791, 0);
					pokeb(0x0771, 0);
					r[0] = (uint16_t)(r[3] + 1);
					pokeb(0x01EB, (uint8_t)r[0]);
				}
				break;
			case 11:
				r[1] = 0;
				break;
			default:                                   // the table has no entries for these
				break;
			}
			if (peekb(0x0330) == 4)
			{
				r[1] = (uint16_t)(peekb(0x07B1) << 1);
				if ((int8_t)peekb(0x01EB) > 8)
					poke(0x03B2, peek(0x03B0));
				else
				{
					r[0] = (uint16_t)(sext8(peekb(0x01F8)) << 1);
					poke(0x03B2, peek((uint16_t)(0x4F44 + r[0])));
					poke(0x03B2, (uint16_t)(peek(0x03B2) + r[1]));
				}
				r[0] = (uint16_t)(sext8(peekb(0x01F8)) << 1);
				poke(0x03B4, peek((uint16_t)(0x43C8 + r[0])));
				poke(0x03B4, (uint16_t)(peek(0x03B4) + r[1]));
			}
			else if (peekb(0x0330) == 1)
			{
				poke(0x03B2, peek(0x03B0));
				r[1] = (uint16_t)(peekb(0x07B2) << 1);
				r[0] = (uint16_t)(sext8(peekb(0x01F8)) << 1);
				poke(0x03B4, peek((uint16_t)(0x4330 + r[0])));
				poke(0x03B4, (uint16_t)(peek(0x03B4) + r[1]));
			}
		}
		r[6] = (uint16_t)(r[6] + 2);                     // TST (SP)+
	}
	if (peekb(0x025D) && peekb(0x031F) != 5)
	{
		poke(0x03B6, peek(0x03B4));
		poke(0x03B6, fl::add(peek(0x03B6), 0x061E));
		return;
	}
	poke(0x03B6, 0x50FA);
	f_nzvc(0);                                         // C clear from the TSTB / CMPB before
}

// ---------------------------------------------------------------- packed tile blocks

// Packed tile runs (F:EAD4 and F:E82E).  A run starts with a byte: bits 0-5 the number of
// tiles (0: 65536), bits 6/7 the mode, then the tile's low byte (0x0F4C); mode 2 also has
// the high byte (0x0F4D).  The following tiles of the run: mode 0 a new low byte each, mode 1
// the same tile, mode 2 two new bytes each, mode 3 the tile number + 1 each.  The mode is
// kept in 0x0F5F (0-3).
uint8_t run_mode(uint8_t head)
{
	const bool b6 = (head & 0x40) != 0;
	return (head & 0x80) ? (b6 ? 3 : 2) : (b6 ? 1 : 0);
}

// F:EAD4  draw_tile_block(int erase, int *block): block = {packed data, video address (low 7
// bits: column * 2), rows, columns}; the pointer argument is advanced past the header.  Tiles
// get the colour bits 0x0188 (or are cleared when erase is set); tile 0x149 is transparent.
// Video bank 0 is selected; the banks are restored.
uint16_t draw_tile_block(const Args &a)
{
	Helper h(F162);
	const uint16_t sa = peek(0x01D2), sb = peek(0x01D4);
	T.r[6] = (uint16_t)(T.r[6] - 0x000C);                // SUB #0xC,SP: locals, banks saved there
	ww((uint16_t)(T.r[6] + 0x000A), sa);
	ww((uint16_t)(T.r[6] + 0x0008), sb);
	set_banks(0x1800, 0x1C00);
	T.r[0] = 0x1C00;
	call(0x8068, {});                                  // select_video_bank0
	const uint16_t slot = (uint16_t)(a.sp + 4);
	uint16_t r3 = peek(peek(slot));
	poke(slot, (uint16_t)(peek(slot) + 2));
	uint16_t r2 = peek(peek(slot)) & (uint16_t)~0x007F;
	uint16_t r1 = peek(peek(slot)) & 0x007E;
	poke(slot, (uint16_t)(peek(slot) + 2));
	uint16_t rows = peek(peek(slot));
	poke(slot, (uint16_t)(peek(slot) + 2));
	const uint16_t cols = peek(peek(slot));
	uint16_t r0 = 0;
	poke(0x0F4C, 0);
	const uint16_t back = (uint16_t)(cols << 1);
	if ((int16_t)rows > 0)
		do
		{
			uint16_t n = cols;
			if ((int16_t)n > 0)
				do
				{
					if (r0 == 0)
					{
						const uint8_t head = peekb(r3++);
						pokeb(0x0F5F, head);
						r0 = sext8(head) & 0x003F;
						pokeb(0x0F4C, peekb(r3++));
						const uint8_t mode = run_mode(peekb(0x0F5F));
						pokeb(0x0F5F, mode);
						if (mode == 2) pokeb(0x0F4D, peekb(r3++));
					}
					else
						switch (peekb(0x0F5F))
						{
						case 0: pokeb(0x0F4C, peekb(r3++)); break;
						case 1: break;
						case 2: pokeb(0x0F4C, peekb(r3++)); pokeb(0x0F4D, peekb(r3++)); break;
						case 3: poke(0x0F4C, (uint16_t)(peek(0x0F4C) + 1)); break;
						default: break;                    // not set by the run heads
						}
					if ((peek(0x0F4C) & 0x03FF) != 0x0149)
					{
						const uint16_t at = (uint16_t)((r1 & 0x00FF) | r2);
						if (!a.u(0))
						{
							poke(0x0F4C, (uint16_t)(peek(0x0F4C) | peek(0x0188)));
							poke(at, peek(0x0F4C));
						}
						else
							poke(at, 0);
					}
					r1 = (uint16_t)(r1 + 2);
					r0 = (uint16_t)(r0 - 1);
				} while (--n != 0);
			r1 = (uint16_t)(r1 - back);
			r2 = (uint16_t)(r2 + 0x0080);
		} while (--rows != 0);
	restore_banks(sa, sb);
	return r0;
}

// F:EC20  draw_map_object(int i): object record i at 0x4B58 (bank 0x8000): {type, shape,
// column, row}.  The shape gives the size (0x4BCB columns x 0x4BC0 rows), the bank base
// (0x4BD6) and the frame table (0x4BE2); the frame comes from the type: 0-3 F:90BA with
// 0x027A = type, 4-7 the ticket count 0x0258[type] (0-2), 8-11 0xFE7C[0x0F31] - 1; objects
// of types 4-11 whose ticket bit (0x0ED7) is set are not drawn.  The frame's tiles are drawn
// into the playfield at the map position (0x0F42/0x0F45), keeping the colour bits, inside
// the playfield half of 0x0F42; zero tiles are skipped (the column does not advance).
uint16_t draw_map_object(const Args &a)
{
	Helper h(F162);
	T.r[6] = (uint16_t)(T.r[6] - 8);                   // CMP -(SP),-(SP) and two pushed sizes
	poke(0x01D2, 0x8000);
	ww(0x1400, 0x8000);
	uint16_t r0 = (uint16_t)((uint16_t)(a.u(0) << 2) + 0x4B58);
	uint16_t r3 = r0;
	const uint16_t type = peekb(r3++);
	uint16_t r1 = peekb(r3);
	uint16_t rows = peekb((uint16_t)(0x4BC0 + r1));
	const uint16_t cols = peekb((uint16_t)(0x4BCB + r1));
	uint16_t r5 = peekb((uint16_t)(0x4BD6 + r1)) ? 0x6000 : 0x4000;
	r0 = (uint16_t)(r1 << 1);
	r1 = peek((uint16_t)(0x4BE2 + r0));
	r3++;
	r0 = (uint16_t)((uint16_t)(peekb(r3++) << 1) + peekb(0x0F45));
	const uint16_t col0 = r0;
	r0 = (uint16_t)((uint16_t)(peekb(r3) << 8) + peek(0x0F42));
	uint16_t r2 = r0;
	r0 = (uint16_t)(type << 1);
	if (type <= 3)
	{
		const uint8_t saved = peekb(0x027A);
		pokeb(0x027A, (uint8_t)type);
		T.r[0] = r0; T.r[1] = r1; T.r[2] = r2; T.r[3] = r3; T.r[4] = type; T.r[5] = r5;
		r0 = call(0x90BA, {});
		r3 = (uint16_t)(r0 - 1);
		pokeb(0x027A, saved);
	}
	else if (type <= 7)
	{
		if (peekb(0x0ED7) & peekb((uint16_t)(0xFC70 + type))) return r0;
		r3 = sext8(peekb((uint16_t)(0x0258 + type)));
		if ((int16_t)r3 < 0 || (int16_t)r3 >= 3) r3 = 0;
	}
	else if (type <= 11)
	{
		if (peekb(0x0ED7) & peekb((uint16_t)(0xFC6C + type))) return r0;
		r0 = peekb(0x0F31);
		r3 = (uint16_t)(peekb((uint16_t)(0xFE7C + r0)) - 1);
	}
	else
		return r0;                                     // the jump table has no entries for these
	r0 = (uint16_t)((uint16_t)(r3 << 1) + r1);
	r3 = peek((uint16_t)(0x4000 + r0));
	r0 = (uint16_t)(((int16_t)r3 >> 3) & 0x1C00);
	r5 = (uint16_t)(r0 + r5);
	poke(0x01D2, r5);
	poke(0x01D4, (uint16_t)(r5 + 0x0400));
	ww(0x1400, peek(0x01D2));
	ww(0x1402, peek(0x01D4));
	r3 = (uint16_t)((r3 & 0x1FFF) + 0x4000);
	const uint16_t half = peek(0x0F42) & 0x0800;
	if (half)
	{
		r2 = (uint16_t)(r2 - 0x0800);
		r0 = (uint16_t)(cols << 1);
		while (!(r2 & 0x0800))
		{
			r2 = (uint16_t)(r2 + 0x0100);
			rows--;
			r3 = (uint16_t)(r3 + r0);
		}
	}
	T.r[0] = r2;
	call(0x8078, {});                                  // select_video_bank
	r0 = r2 & 0x1F00;
	r2 = (uint16_t)(r0 + 0x2000);
	if ((int16_t)rows > 0)
		do
		{
			if ((r2 & 0x0800) != half) break;
			r1 = col0;
			uint16_t n = cols;
			if ((int16_t)n > 0)
				do
				{
					r0 = peek(r3);
					r3 = (uint16_t)(r3 + 2);
					if (!r0) continue;
					if ((int16_t)r0 >= 0x0800) r0 = (uint16_t)((r0 & 0x03FF) + 0x0400);
					const uint16_t at = (uint16_t)((r1 & 0x00FF) | r2);
					r0 |= peek(at) & 0xF800;
					poke(at, r0);
					r1 = (uint16_t)(r1 + 2);
				} while (--n != 0);
			r2 = (uint16_t)(r2 + 0x0100);
		} while (--rows != 0);
	return r0;
}

// F:E82E (registers): unpack the 8 x 16 tile block of the map cell 0x0F48 into the playfield
// at 0x0F42 (row 0x0F43, column 0x0F45; video bank by F:8078).  The cell's block number is in
// the level's map (bank base 0xFEB2[level]); the block itself is in the bank from 0xFEA2
// [level] (0x6000 for the town's alternate cells flagged at 0x0F56 / 0x0F58, 0xA000 for odd
// blocks in parks 1-4).  The block's first byte (bit 7: it has objects) or, in the right
// half of the playfield, its byte 1 is the cell's terrain byte (0x0F60 + row / column); byte
// 2 then skips ahead.  Tiles 0-15 with a non-zero entry in the tile table at 0x0F4E become
// that entry plus a random variant (0xFEE2, index 0x0FA0).  The objects are listed backwards
// before the block and drawn by F:EC20.  The banks end as 0x1800 / 0x3C00.
void unpack_map_block()
{
	uint16_t *r = T.r;
	const uint16_t sp0 = r[6];
	r[6] = (uint16_t)(sp0 - 0x000A);                    // SUB #0xA,SP: locals
	call(0x9152, {});                                  // next_random
	r[0] = (uint16_t)(r[0] + peek(0x0FA0));
	poke(0x0FA0, r[0]);
	r[0] = peek(0x0F42);
	call(0x8078, {});                                  // select_video_bank
	r[4] = peek(0x0F48);
	r[0] = (uint16_t)(((int16_t)r[4] >> 3) & 0x1C00);
	r[5] = peekb(0x0F59);
	r[3] = (uint16_t)(r[5] << 1);
	r[0] = (uint16_t)(r[0] + peek((uint16_t)(0xFEB2 + r[3])));
	r[2] = r[0];
	poke(0x01D2, r[2]);
	r[0] = (uint16_t)(r[0] + 0x0400);
	poke(0x01D4, r[0]);
	ww(0x1400, peek(0x01D2));
	ww(0x1402, peek(0x01D4));
	r[4] = (uint16_t)((r[4] & 0x1FFF) + 0x4000);
	r[4] = peek(r[4]);
	r[1] = 0x2C00;
	poke(0x01D4, 0x2C00);
	ww(0x1402, 0x2C00);
	if (peekb(peek(0x0F56)) & peekb(0x0F58))
		r[2] = g_patch_town_alt_base;                  // F:E8B6
	else
	{
		r[2] = peek((uint16_t)(0xFEA2 + r[3]));
		if ((int16_t)r[5] > 0 && (int16_t)r[5] <= 4 && (r[4] & 1))
			r[2] = g_patch_park_odd_base;              // F:E8B0
	}
	r[0] = peek(0x0F42);
	call(0x8078, {});
	r[0] = (uint16_t)((((int16_t)r[4] >> 3) & 0x1C00) + r[2]);
	r[2] = r[0];
	poke(0x01D2, r[2]);
	r[0] = (uint16_t)(r[0] + 0x0400);
	poke(0x01D4, r[0]);
	ww(0x1400, peek(0x01D2));
	ww(0x1402, peek(0x01D4));
	r[4] = (uint16_t)((r[4] & 0x1FFF) + 0x4000);
	r[0] = sext8(peekb(r[4]));
	const bool objects = (r[0] & 0x80) != 0;
	const uint16_t block = r[4], bank = r[2];
	pokeb(0x0F51, peekb(0x0F43));
	pokeb(0x0F51, (uint8_t)(peekb(0x0F51) | 0x20));
	pokeb(0x0F50, peekb(0x0F45));
	if (peek(0x0F42) & 0x0800)
	{
		r[1] = peekb((uint16_t)(r[4] + 1));
		r[3] = peekb((uint16_t)(r[4] + 2));
		r[4] = (uint16_t)(r[4] + r[3]);
	}
	else
		r[1] = sext8(peekb(r[4])) & 0x007F;
	r[0] = (uint16_t)(peekb(0x0F45) >> 3);
	r[5] = sext8((uint8_t)(peek(0x0F42) >> 8)) & 0x0038;
	r[0] = (uint16_t)(r[0] >> 2);
	r[5] = (uint16_t)(r[5] + r[0]);
	pokeb((uint16_t)(0x0F60 + r[5]), (uint8_t)r[1]);
	r[3] = 0;
	poke(0x0F4C, 0);
	r[2] = 0;
	r[0] = 0;
	r[4] = (uint16_t)(r[4] + 3);
	for (int row = 8; row > 0; row--)
	{
		for (int col = 16; col > 0; col--)
		{
			bool repeat = false;                       // mode 1: the same tile again
			if (r[3] == 0)
			{
				const uint8_t head = peekb(r[4]++);
				r[3] = sext8(head);
				pokeb(0x0F5F, head);
				r[3] &= 0x003F;
				pokeb(0x0F4C, peekb(r[4]++));
				r[1] = sext8(peekb(0x0F5F));
				r[5] = (r[1] & 0x40) ? 1 : 0;
				if (!(r[1] & 0x80)) r[5] = 0;          // ROR R5 (C clear after TST R1)
				const uint8_t mode = run_mode((uint8_t)r[1]);
				pokeb(0x0F5F, mode);
				if (mode == 2) pokeb(0x0F4D, peekb(r[4]++));
			}
			else
			{
				r[1] = (uint16_t)(sext8(peekb(0x0F5F)) << 1);
				switch (peekb(0x0F5F))
				{
				case 0: pokeb(0x0F4C, peekb(r[4]++)); break;
				case 1: repeat = true; break;
				case 2: pokeb(0x0F4C, peekb(r[4]++)); pokeb(0x0F4D, peekb(r[4]++)); break;
				case 3: poke(0x0F4C, (uint16_t)(peek(0x0F4C) + 1)); break;
				default: break;                        // not set by the run heads
				}
			}
			bool plain;
			if (repeat)
				plain = r[2] == 0;                     // the variant computed last time
			else
			{
				r[0] = 0;
				r[2] = peek(0x0F4C) & 0x07FF;
				if ((int16_t)r[2] > 0x000F)
				{
					r[2] = 0;
					plain = true;
				}
				else
				{
					r[1] = (uint16_t)((uint16_t)(r[2] << 1) + peek(0x0F4E));
					r[2] = peek(r[1]);
					if (r[2] == 0)
						plain = true;
					else
					{
						r[1] = peek(0x0F4C) & 0xF800;
						r[2] |= r[1];
						plain = false;
					}
				}
			}
			if (plain)
				r[1] = peek(0x0F4C);
			else
			{
				r[1] = (uint16_t)(peek(0x0FA0) + 1);
				poke(0x0FA0, r[1]);
				poke(0x0FA0, peek(0x0FA0) & 0x001F);
				r[5] = peek(0x0FA0);
				r[1] = (uint16_t)(sext8(peekb((uint16_t)(0xFEE2 + r[5]))) + r[2]);
			}
			poke(peek(0x0F50), r[1]);
			pokeb(0x0F50, (uint8_t)(peekb(0x0F50) + 1));
			pokeb(0x0F50, (uint8_t)(peekb(0x0F50) + 1));
			r[3] = (uint16_t)(r[3] - 1);
		}
		r[1] = (uint16_t)(peekb(0x0F50) - 0x0020);
		pokeb(0x0F50, (uint8_t)r[1]);
		pokeb(0x0F51, (uint8_t)(peekb(0x0F51) + 1));
	}
	if (objects)
	{
		r[4] = (uint16_t)(block + 0x1FFF);
		const uint16_t ba = (uint16_t)(bank - 0x0400);
		poke(0x01D2, ba);
		r[1] = (uint16_t)(ba + 0x0400);
		poke(0x01D4, r[1]);
		ww(0x1400, peek(0x01D2));
		ww(0x1402, peek(0x01D4));
		do
		{
			r[0] = sext8(peekb(r[4]));
			call(0xEC20, {r[0]});                      // draw_map_object
			poke(0x01D2, ba);
			poke(0x01D4, r[1]);
			ww(0x1400, peek(0x01D2));
			ww(0x1402, peek(0x01D4));
			r[4] = (uint16_t)(r[4] - 1);
		} while (!(peekb(r[4]) & 0x80));
	}
	r[0] = 0x1800;
	poke(0x01D2, 0x1800);
	ww(0x1400, 0x1800);
	r[0] = 0x3C00;
	poke(0x01D4, 0x3C00);
	ww(0x1402, 0x3C00);
	r[6] = sp0;
	fl::add((uint16_t)(sp0 - 0x000A), 0x000A);          // ADD #0xA,SP
}

}

LOGIC_TABLE(fhigh,
	c_routine("F", 0xD0F4, "update_marked_slots", update_marked_slots, 0, 41784),
	c_routine("F", 0xD15C, "update_high_slots", update_high_slots, 0, 6999),
	c_routine("F", 0xD880, "set_park_entry_types", set_park_entry_types, 0, 33576),
	c_routine("F", 0xD8D2, "set_ticket_entry_types", set_ticket_entry_types, 0, 19335),
	c_routine("F", 0xD47C, "start_object", start_object, 0, 2331),
	c_routine("F", 0xDD5C, "set_colour_1092", set_colour_1092, 0, 624),
	c_routine("F", 0xDE28, "run_object_scripts", run_object_scripts, 0, 2722),
	c_routine("F", 0xDF40, "start_object_scripts", start_object_scripts, 0, 4893),
	c_routine("F", 0xE036, "countdown_0eb4", countdown_0eb4, 0, 83),
	c_routine("F", 0xE13A, "set_object7_motion", set_object7_motion, 0, 1065),
	c_routine("F", 0xE5D2, "skater_anim_a", skater_anim_a, 0, 828),
	c_routine("F", 0xE648, "skater_anim_b", skater_anim_b, 0, 808),
	c_routine("F", 0xEF24, "add_score", add_score, 0, 1164),
	c_routine("F", 0xF064, "start_object4", start_object4, 0, 497),
	c_routine("F", 0xD19C, "setup_level_slots", setup_level_slots, 0, 323232),
	wait_c_routine("F", 0xE78E, "query_on_button_press", query_on_button_press, 21 + 12 + 18 + 18 + 21),
	asm_routine("F", 0xD35E, "spawn_counted_object", spawn_counted_object, 1984),
	asm_routine("F", 0xD4E4, "offset_to_object_0ea0", offset_to_object_0ea0, 822),
	asm_routine("F", 0xD566, "contact_object_0ea0", contact_object_0ea0, 4923),
	asm_routine("F", 0xDB4E, "fade_sprite_palette", fade_sprite_palette, 9528),
	asm_routine("F", 0xDBE2, "step_palette_fade", step_palette_fade, 59),
	asm_routine("F", 0xDC3C, "load_palette_block", load_palette_block, 2578),
	asm_routine("F", 0xDDEC, "run_palette_job", run_palette_job, 57),
	asm_routine("F", 0xE254, "update_skater_phase", update_skater_phase, 2950),
	c_routine("F", 0xEAD4, "draw_tile_block", draw_tile_block, 0, 240306),
	c_routine("F", 0xEC20, "draw_map_object", draw_map_object, 0, 3500),
	asm_routine("F", 0xE82E, "unpack_map_block", unpack_map_block, 65592),
)

}
