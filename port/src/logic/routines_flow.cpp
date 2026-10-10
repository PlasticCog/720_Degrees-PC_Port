// Rewritten routines: text strings, skater motion and state changes, sprite link lists,
// message queue and helpers of the frame flow.  Routines with an assembly interface
// reproduce registers and condition codes; the others follow the C convention.
#include "logic.h"

namespace logic {
namespace {

using namespace t11rt;

uint16_t sext8(uint8_t v) { return (uint16_t)(int16_t)(int8_t)v; }

// MOV #v,R0 / MOV R0,@#shadow / MOV R0,@#bank register: select a bank through R0
void bank_a(uint16_t v) { T.r[0] = v; poke(0x01D2, v); poke(0x1400, v); }
void bank_b(uint16_t v) { T.r[0] = v; poke(0x01D4, v); poke(0x1402, v); }

// put both banks back from saved shadow values (shadows first, then the registers)
void restore_banks(uint16_t a, uint16_t b)
{
	poke(0x01D2, a);
	poke(0x01D4, b);
	poke(0x1400, peek(0x01D2));
	poke(0x1402, peek(0x01D4));
}

// MTPS: processor priority and flags (the T bit is kept); interrupts are looked at again
void mtps(uint8_t v)
{
	T.psw = (uint8_t)((T.psw & ~0xEF) | (v & 0xEF));
	T.check_irqs = true;
}

// single-operand instructions with the condition codes the T-11 sets
uint16_t asl(uint16_t d)
{
	const uint16_t r = (uint16_t)(d << 1);
	f_nzvc(nzw(r) | ((d >> 15) & 1));
	f_shift();
	return r;
}
uint16_t asr(uint16_t d)
{
	const uint16_t r = (uint16_t)((d >> 1) | (d & 0x8000));
	f_nzvc(nzw(r) | (d & 1));
	f_shift();
	return r;
}
uint16_t ror(uint16_t d)
{
	const uint16_t r = (uint16_t)((d >> 1) | (C() << 15));
	f_nzvc(nzw(r) | (d & 1));
	f_shift();
	return r;
}

// F:90A6  divide(int d, int n): n / d, truncated and signed, by shift and subtract (F:9034
// with mode 0); 0 when n or d is 0
uint16_t divide(const Args &a)
{
	uint16_t d = a.u(0), n = a.u(1);
	if (n == 0 || d == 0) return 0;
	const bool nneg = (int16_t)n < 0, dneg = (int16_t)d < 0;
	if (nneg) n = (uint16_t)-n;
	if (dneg) d = (uint16_t)-d;
	int shift = 0;
	while ((int16_t)d < (int16_t)n && !(d & 0x4000))
	{
		d = (uint16_t)(d << 1);
		shift++;
	}
	uint16_t q = 0;
	for (;;)
	{
		q = (uint16_t)(q << 1);
		if ((int16_t)d <= (int16_t)n)
		{
			q++;
			n = (uint16_t)(n - d);
			if (n == 0) break;
		}
		if (--shift < 0) break;
		d = (uint16_t)((int16_t)d >> 1);
	}
	if (nneg != dneg) q = (uint16_t)-q;
	while (--shift >= 0) q = (uint16_t)(q << 1);
	return q;
}

// F:90BA  park_level(): 0x0F2D when positive; otherwise derived from the table at 0xFCAC
// for the park 0x027A and limited to 9 (10 when 0x0F31 >= 12)
uint16_t park_level(const Args &)
{
	const uint16_t r1 = sext8(peekb(0x0F2D));
	if ((int16_t)r1 > 0) return r1;
	uint16_t v = peekb((uint16_t)(0xFCAC + sext8(peekb(0x027A))));
	v = (uint16_t)(((uint16_t)(v - r1 - 1) & 3) - r1);
	const uint16_t limit = peekb(0x0F31) < 0x0C ? 9 : 10;
	if ((int16_t)v > (int16_t)limit) v = limit;
	return v;
}

// F:8AB6 / F:8B04 (string number in R0): draw string R0 of the table at 0x0184 (bank A
// 0x2000) with the large (F:8B52) or small (F:8C12) font.  An entry is a screen offset and
// the characters; the last one has bit 7 set.  Bit 7 of the string number draws it with
// code 0x23 instead (erasing).  R1-R3 are preserved (register-save helper F144).
void draw_string(uint16_t put_char)
{
	const uint16_t r1 = T.r[1], r2 = T.r[2], r3 = T.r[3];
	const uint16_t saved_a = peek(0x01D2);
	poke(0x01D2, 0x2000);
	poke(0x1400, peek(0x01D2));
	call(0x8068, {});                                  // select_video_bank0 (R0 kept)
	T.r[3] = T.r[0];
	uint16_t p = peek((uint16_t)(((T.r[0] << 1) & 0x00FF) + peek(0x0184)));
	T.r[2] = (uint16_t)(peek(p) + 0x2000);
	p = (uint16_t)(p + 2);
	poke(0x0186, T.r[2]);
	for (;;)
	{
		T.r[1] = p;
		T.r[0] = (T.r[3] & 0x0080) ? 0x0023 : sext8(peekb(p));
		call(put_char, {});
		const uint8_t c = peekb(p);
		p = (uint16_t)(p + 1);
		if (c & 0x80) break;
	}
	poke(0x01D2, saved_a);
	poke(0x1400, peek(0x01D2));
	T.r[1] = r1;
	T.r[2] = r2;
	T.r[3] = r3;
	f_nzvc(nzw(T.r[1]));                               // C clear from the last TSTB
}
void draw_string_large() { draw_string(0x8B52); }
void draw_string_small() { draw_string(0x8C12); }

// F:9F8A: from kick phase 4 the speed comes back from 0x02E8; then the kick state is reset,
// phase 0x01EB = 8, 0x0330 = 4, and the skater takes off (F:9FB4)
uint16_t skater_jump(const Args &)
{
	if (peekb(0x01EB) == 4) poke(0x01B4, peek(0x02E8));
	call(0xE238, {});
	pokeb(0x07B1, 3);
	pokeb(0x01EB, 8);
	pokeb(0x0330, 4);
	return call(0x9FB4, {});
}

// F:AFF0 (assembly): apply the queued sprite size changes: for each of the 4 requests
// (object 0x0989+i, size 0x098D+i) the object's size 0x04D0 is set; a linked object
// (0x0490 bit 1) is unlinked and linked again (F:ACE6, F:AE02).  Bit 5 of 0x0490 is cleared;
// requests whose object has no bit 6 are done (0x0992 counts the pending ones).
void process_sprite_requests()
{
	const uint8_t n = peekb(0x0992);
	fl::tstb(n);
	if (!n) return;
	T.r[2] = 4;
	do
	{
		const uint8_t o = peekb((uint16_t)(0x0989 + T.r[2]));
		T.r[1] = sext8(o);
		fl::movb(o);
		if (!o) continue;
		pokeb((uint16_t)(0x04D0 + T.r[1]), peekb((uint16_t)(0x098D + T.r[2])));
		T.r[3] = fl::add(0x0490, T.r[1]);
		const uint8_t f = peekb(T.r[3]);
		if (f & 0x02)
		{
			call(0xACE6, {T.r[1], 0});
			call(0xAE02, {T.r[1], 0});
			fl::add((uint16_t)(T.r[6] - 6), 6);
		}
		const uint8_t g = (uint8_t)(peekb(T.r[3]) & ~0x20);
		pokeb(T.r[3], g);
		f_nzv(nzb(g & 0x40));
		if (!(g & 0x40))
		{
			pokeb(0x0992, fl::decb(peekb(0x0992)));
			pokeb((uint16_t)(0x0989 + T.r[2]), 0);
			fl::clr();
		}
	} while (--T.r[2]);
}

// F:A974  queue_message(int bit, int a, int b): with interrupts masked, the entry count
// 0x0358 goes up (at most 3) and the entry gets the words b (0x0362) and a (0x035A); bit is
// shifted into the mask 0x036D and a 1 into 0x036E unless the text table 0x0184 is 0x5AC8.
// Returns the shifted 0x036E (before that bit).
uint16_t queue_message(const Args &a)
{
	mtps(0xE0);
	const uint8_t n = (uint8_t)(peekb(0x0358) + 1);
	pokeb(0x0358, n);
	if ((int8_t)n > 3) pokeb(0x0358, 3);
	const uint16_t i = (uint16_t)(sext8(peekb(0x0358)) << 1);
	poke((uint16_t)(0x0362 + i), a.u(2));
	poke((uint16_t)(0x035A + i), a.u(1));
	mtps(0);
	uint16_t r0 = (uint16_t)(peekb(0x036D) << 1);
	pokeb(0x036D, (uint8_t)r0);
	pokeb(0x036D, (uint8_t)(peekb(0x036D) | a.b(0)));
	r0 = (uint16_t)(peekb(0x036E) << 1);
	pokeb(0x036E, (uint8_t)r0);
	if (peek(0x0184) != 0x5AC8) pokeb(0x036E, (uint8_t)(peekb(0x036E) | 1));
	return r0;
}

// F:A8FE  flush_text_list(): the entries 0..0x0336 of the list at 0x0348 (bank B 0x2800) are
// drawn: a pointer to a value from 0x4000 on goes to F:EAD4, others to A7:4D30; the list is
// emptied (0x0336 = 0xFF) and the banks restored.  R1-R5 preserved (F162).
uint16_t flush_text_list(const Args &)
{
	const uint16_t saved[5] = { T.r[1], T.r[2], T.r[3], T.r[4], T.r[5] };
	T.r[4] = peek(0x01D2);
	T.r[5] = peek(0x01D4);
	T.r[3] = sext8(peekb(0x0336));
	for (T.r[2] = 0; (int16_t)T.r[2] <= (int16_t)T.r[3]; T.r[2]++)
	{
		bank_b(0x2800);
		T.r[1] = (uint16_t)(T.r[2] << 1);
		const uint16_t p = peek((uint16_t)(0x0348 + T.r[1]));
		const uint16_t v = peek(p);
		if ((int16_t)v >= 0x4000)
			call(0xEAD4, {1, v});
		else
		{
			bank_a(0x3800);
			call(0x4D30, {1, p});
		}
	}
	pokeb(0x0336, 0xFF);
	restore_banks(T.r[4], T.r[5]);
	for (int i = 0; i < 5; i++) T.r[i + 1] = saved[i];
	return T.r[0];
}

// F:91A6  skater_velocity(): x / y velocities of object 0 = speed 0x01B4 times the direction
// factors 0x02E2 / 0x02E4 (F:80FA, middle bytes of the product; the low byte to 0x07F0 /
// 0x07F4), plus the velocity of the object carrying the skater (0x0EA0); 0x0331 = speed
// class (|speed| * 2 >> 8, at most 15).  Returns |speed| * 2.
uint16_t skater_velocity(const Args &)
{
	const uint16_t a = peek(0x01D2), b = peek(0x01D4);
	call(0x80FA, {peek(0x02E2), peek(0x01B4)});
	pokeb(0x07F0, peekb(0x0205));
	poke(0x04F0, peek(0x0206));
	call(0x80FA, {peek(0x02E4), peek(0x01B4)});
	pokeb(0x07F4, peekb(0x0205));
	poke(0x0570, peek(0x0206));
	const uint8_t carrier = peekb(0x0EA0);
	if (carrier)
	{
		const uint16_t i = (uint16_t)((sext8(carrier) & 0x003F) << 1);
		poke(0x04F0, (uint16_t)(peek(0x04F0) + peek((uint16_t)(0x04F0 + i))));
		poke(0x0570, (uint16_t)(peek(0x0570) + peek((uint16_t)(0x0570 + i))));
	}
	uint16_t s = peek(0x01B4);
	if ((int16_t)s < 0) s = (uint16_t)-s;
	s = (uint16_t)(s << 1);
	const uint8_t cls = (uint8_t)(s >> 8);
	pokeb(0x0331, cls);
	if ((int8_t)cls > 15) pokeb(0x0331, 15);
	restore_banks(a, b);
	return s;
}

// F:843E  place_skater(int z, int y, int x): objects 0-3 at (x, y, z) without velocity, the
// skater's motion state reset (speed, ramp, terrain, gravity, ...), F:E238
uint16_t place_skater(const Args &a)
{
	for (int i = 3; i >= 0; i--)
	{
		const uint16_t w = (uint16_t)(2 * i);
		poke((uint16_t)(0x06F0 + w), a.u(2));
		poke((uint16_t)(0x0730 + w), a.u(1));
		poke((uint16_t)(0x06B0 + w), a.u(0));
		poke((uint16_t)(0x04F0 + w), 0);
		poke((uint16_t)(0x0570 + w), 0);
		poke((uint16_t)(0x05B0 + w), 0);
		pokeb((uint16_t)(0x07F0 + i), 0);
		pokeb((uint16_t)(0x07F4 + i), 0);
		pokeb((uint16_t)(0x07F8 + i), 0);
	}
	poke(0x01B4, 0);
	pokeb(0x01EA, 1);
	pokeb(0x031E, 0);
	pokeb(0x0328, 0xFE);
	pokeb(0x0320, 1);
	pokeb(0x031F, 0);
	pokeb(0x0323, 0);
	pokeb(0x0EA0, 0);
	pokeb(0x031B, 0xFF);
	pokeb(0x032F, 0);
	poke(0x02FC, 0);
	poke(0x02FE, 0);
	poke(0x02EE, 0);
	pokeb(0x0325, 0xFF);
	poke(0x01B0, 0);
	poke(0x01B2, 0);
	poke(0x0ED2, 0x3800);
	poke(0x0ED4, 0xA800);
	T.r[0] = 0;
	return call(0xE238, {});
}

// F:A45A  skater_phase10(int side, int dir): animation from the tables at 0x4EAC (side >= 0) or
// 0x4EBC (bank A 0, bank B 0x0400) for the direction dir; kick state reset, phase 0x01EB and
// 0x0330 = 10, ramp state 2, frame counter saved (B3:7AD2), sound 0x57; banks restored
uint16_t skater_phase10(const Args &a)
{
	const uint16_t saved_a = peek(0x01D2), saved_b = peek(0x01D4);
	poke(0x01D2, 0);
	poke(0x1400, 0);
	bank_b(0x0400);
	const uint16_t i = a.u(1) & 0xFFFE;
	if ((int16_t)a.u(0) >= 0)
	{
		poke(0x03B2, peek((uint16_t)(0x4EAC + i)));
		poke(0x03B4, (uint16_t)(i + 0x42C0));
	}
	else
	{
		poke(0x03B2, peek((uint16_t)(0x4EBC + i)));
		poke(0x03B4, (uint16_t)(i + 0x42B0));
	}
	poke(0x02FC, 0);
	T.r[0] = i;
	call(0xE238, {});
	pokeb(0x01EB, 0x0A);
	pokeb(0x0330, 0x0A);
	pokeb(0x07B1, 0);
	pokeb(0x0791, 3);
	pokeb(0x0771, 3);
	pokeb(0x01EA, 2);
	bank_a(0x3800);
	bank_b(0x3C00);
	call(0x7AD2, {});
	const uint16_t r0 = call(0x8E78, {0x0057});
	restore_banks(saved_a, saved_b);
	return r0;
}

// Sprite draw lists.  Objects are kept in doubly linked lists (next 0x0808, previous 0x0848,
// list heads 0x0988) in drawing order; each object owns a chain of motion object slots
// (first 0x0888, last 0x0908) whose link fields (motion object RAM 0x3806, bits 1-10) chain
// all sprites of a list together.

// the last sprite of object a links to the first sprite of object b; returns the link word
uint16_t chain_sprites(uint16_t a, uint16_t b)
{
	const uint16_t at = (uint16_t)(0x3806 + peek((uint16_t)(0x0908 + (a << 1))));
	const uint16_t v = (uint16_t)((peek(at) & ~0x07FE) | peek((uint16_t)(0x0888 + (b << 1))));
	poke(at, v);
	return v;
}

// F:ACE6  unlink_sprites(int obj, int list): take obj out of list (the previous object's
// sprites link to the next one's, or end the chain with 0x03F8), clear the list's bits
// (0xFD7C) in 0x0490 and free the object's sprite slots.  Returns the last slot.
uint16_t unlink_sprites(const Args &a)
{
	const uint16_t obj = a.u(0), list = a.u(1);
	const uint16_t prev = sext8(peekb((uint16_t)(0x0848 + obj)));
	const uint16_t next = sext8(peekb((uint16_t)(0x0808 + obj)));
	if ((int16_t)prev >= 0)
	{
		pokeb((uint16_t)(0x0808 + prev), (uint8_t)next);
		if ((int16_t)next >= 0)
		{
			chain_sprites(prev, next);
			pokeb((uint16_t)(0x0848 + next), (uint8_t)prev);
		}
		else
		{
			const uint16_t at = (uint16_t)(0x3806 + peek((uint16_t)(0x0908 + (prev << 1))));
			poke(at, (uint16_t)((peek(at) & ~0x07FE) | 0x03F8));
		}
	}
	else
	{
		pokeb((uint16_t)(0x0988 + list), (uint8_t)next);
		if ((int16_t)next >= 0) pokeb((uint16_t)(0x0848 + next), (uint8_t)prev);
	}
	const uint16_t flags = (uint16_t)(0x0490 + (obj & 0x1F));
	pokeb(flags, (uint8_t)(peekb(flags) & ~peekb((uint16_t)(0xFD7C + list))));
	uint16_t slot = peek((uint16_t)(0x0888 + (obj << 1)));
	const uint16_t last = peek((uint16_t)(0x0908 + (obj << 1)));
	for (;;)
	{
		const uint16_t next_slot = peek((uint16_t)(0x3806 + slot)) & 0x07F8;
		poke((uint16_t)(0x3806 + slot), 0);
		if (slot == last) break;
		slot = next_slot;
	}
	return slot;
}

// F:AE02  link_sprites(int obj, int list): give obj its 0x04D0 sprite slots (free slots from
// slot 1 on, chained; the last one ends with 0x03F9), set the list's bits (0xFD7C, 0xFD7E) in
// 0x0490, clear bit 2 of 0x04B0, and insert obj into list before the first object it is in
// front of (F:AD70), else at the end.  R1-R5 preserved (F162).
uint16_t link_sprites(const Args &a)
{
	const uint16_t saved[5] = { T.r[1], T.r[2], T.r[3], T.r[4], T.r[5] };
	const uint16_t obj = a.u(0), list = a.u(1);
	const uint16_t i = obj & 0x1F;
	const uint8_t bits = (uint8_t)(peekb((uint16_t)(0x0490 + i)) | peekb((uint16_t)(0xFD7C + list)));
	pokeb((uint16_t)(0x0490 + i), peekb((uint16_t)(0xFD7E + list)));
	pokeb((uint16_t)(0x0490 + i), (uint8_t)(peekb((uint16_t)(0x0490 + i)) | bits));
	pokeb((uint16_t)(0x04B0 + i), (uint8_t)(peekb((uint16_t)(0x04B0 + i)) & ~0x04));
	uint16_t first = 0, last = i, slot = 8;
	uint16_t n = peekb((uint16_t)(0x04D0 + i));
	if ((int16_t)n > 0)
		do
		{
			while (peek((uint16_t)(0x3806 + slot))) slot = (uint16_t)(slot + 8);
			if (!first)
				first = slot;
			else
				poke((uint16_t)(0x3806 + last), slot);
			poke((uint16_t)(0x3800 + slot), 0);
			poke((uint16_t)(0x3C00 + slot), 0);
			poke((uint16_t)(0x3802 + slot), 0);
			last = slot;
			slot = (uint16_t)(slot + 8);
		} while (--n);
	poke((uint16_t)(0x0888 + (obj << 1)), first);
	poke((uint16_t)(0x0908 + (obj << 1)), last);
	poke((uint16_t)(0x3806 + last), 0x03F9);
	uint16_t prev = 0xFFFF;
	uint16_t next = sext8(peekb((uint16_t)(0x0988 + list)));
	uint16_t r0;
	for (;;)
	{
		if ((int16_t)next < 0)
		{
			// at the end of the list
			if ((int16_t)prev >= 0)
			{
				pokeb((uint16_t)(0x0808 + prev), (uint8_t)obj);
				r0 = chain_sprites(prev, obj);
			}
			else
			{
				r0 = list;
				pokeb((uint16_t)(0x0988 + list), (uint8_t)obj);
			}
			pokeb((uint16_t)(0x0848 + obj), (uint8_t)prev);
			pokeb((uint16_t)(0x0808 + obj), (uint8_t)next);
			break;
		}
		T.r[0] = obj;
		T.r[1] = next;
		T.r[2] = obj;
		T.r[3] = prev;
		call(0xAD70, {});                              // F:AD70 R0 in front of R1?
		if (T.r[0] & 1)
		{
			// before next
			if ((int16_t)prev >= 0)
			{
				pokeb((uint16_t)(0x0808 + prev), (uint8_t)obj);
				chain_sprites(prev, obj);
			}
			else
				pokeb((uint16_t)(0x0988 + list), (uint8_t)obj);
			pokeb((uint16_t)(0x0848 + next), (uint8_t)obj);
			pokeb((uint16_t)(0x0808 + obj), (uint8_t)next);
			r0 = chain_sprites(obj, next);
			pokeb((uint16_t)(0x0848 + obj), (uint8_t)prev);
			break;
		}
		prev = next;
		next = sext8(peekb((uint16_t)(0x0808 + next)));
	}
	for (int k = 0; k < 5; k++) T.r[k + 1] = saved[k];
	return r0;
}

// F:AF32 (assembly): one bubble pass over the draw list 0x0993: an object that F:AD70 does
// not put in front of its successor swaps places with it (list and sprite links updated).
// A word on the stack holds the doubled index of the object moving back.
void sort_sprite_list()
{
	T.r[6] = (uint16_t)(T.r[6] - 2);                    // TST -(SP): the local word
	T.r[0] = sext8(peekb(0x0993));
	T.r[3] = sext8(peekb((uint16_t)(0x0988 + T.r[0])));
	if (!(T.r[3] & 0x8000))
	{
		T.r[1] = sext8(peekb((uint16_t)(0x0808 + T.r[3])));
		while (!(T.r[1] & 0x8000))
		{
			T.r[0] = T.r[3];
			call(0xAD70, {});                          // F:AD70 R0 in front of R1?
			T.r[0] = ror(T.r[0]);
			if (!fC())
			{
				// R1 moves in front of R3
				T.r[2] = sext8(peekb((uint16_t)(0x0848 + T.r[3])));
				if (!(T.r[2] & 0x8000))
				{
					pokeb((uint16_t)(0x0808 + T.r[2]), (uint8_t)T.r[1]);
					T.r[4] = peek((uint16_t)(0x0908 + (T.r[2] << 1)));
					T.r[5] = (uint16_t)(T.r[1] << 1);
					T.r[0] = chain_sprites(T.r[2], T.r[1]);
				}
				else
				{
					T.r[0] = sext8(peekb(0x0993));
					pokeb((uint16_t)(0x0988 + T.r[0]), (uint8_t)T.r[1]);
				}
				pokeb((uint16_t)(0x0848 + T.r[1]), (uint8_t)T.r[2]);
				T.r[2] = sext8(peekb((uint16_t)(0x0808 + T.r[1])));
				poke(T.r[6], (uint16_t)(T.r[3] << 1));
				T.r[4] = peek(T.r[6]);
				T.r[0] = peek((uint16_t)(0x0908 + T.r[4]));
				T.r[5] = (uint16_t)(0x3806 + T.r[0]);
				if (!(T.r[2] & 0x8000))
				{
					pokeb((uint16_t)(0x0848 + T.r[2]), (uint8_t)T.r[3]);
					T.r[4] = (uint16_t)(T.r[2] << 1);
					T.r[0] = (uint16_t)((peek(T.r[5]) & ~0x07FE) | peek((uint16_t)(0x0888 + T.r[4])));
					poke(T.r[5], T.r[0]);
				}
				else
				{
					T.r[4] = (uint16_t)((peek(T.r[5]) & ~0x07FE) | 0x03F8);
					poke(T.r[5], T.r[4]);
				}
				pokeb((uint16_t)(0x0808 + T.r[3]), (uint8_t)T.r[2]);
				pokeb((uint16_t)(0x0808 + T.r[1]), (uint8_t)T.r[3]);
				T.r[5] = (uint16_t)(T.r[1] << 1);
				T.r[4] = peek((uint16_t)(0x0908 + T.r[5]));
				T.r[0] = peek(T.r[6]);
				T.r[5] = (uint16_t)((peek((uint16_t)(0x3806 + T.r[4])) & ~0x07FE) | peek((uint16_t)(0x0888 + T.r[0])));
				poke((uint16_t)(0x3806 + T.r[4]), T.r[5]);
				pokeb((uint16_t)(0x0848 + T.r[3]), (uint8_t)T.r[1]);
				T.r[1] = T.r[3];
			}
			T.r[3] = T.r[1];
			T.r[1] = sext8(peekb((uint16_t)(0x0808 + T.r[1])));
		}
	}
	fl::tst(peek(T.r[6]));
	T.r[6] = (uint16_t)(T.r[6] + 2);
}

// F:944C (assembly): the probed cells 1-3 (0x0302-0x0306) become tile numbers (low 10 bits
// plus the tile bank base 0x01D6, or 0x01D8 for bit 10); 0x032A / 0x032B = the attribute byte
// (0x6000) of cell 2; then each cell holds its terrain code: the byte at 0x4000 + tile, or
// with mode 0x032F == 2 a negative attribute byte without bit 7
void cell_terrain_codes()
{
	bank_a(0x0800);
	bank_b(0x0C00);
	for (T.r[3] = 3; T.r[3]; T.r[3]--)
	{
		T.r[0] = (uint16_t)(T.r[3] << 1);
		T.r[1] = peek((uint16_t)(0x0300 + T.r[0]));
		T.r[2] = (uint16_t)((T.r[1] & 0x03FF) + peek((T.r[1] & 0x0400) ? 0x01D8 : 0x01D6));
		poke((uint16_t)(0x0300 + T.r[0]), T.r[2]);
	}
	T.r[0] = peek(0x0304);
	pokeb(0x032A, peekb((uint16_t)(0x6000 + T.r[0])));
	T.r[0] = peek(0x0304);
	pokeb(0x032B, peekb((uint16_t)(0x6000 + T.r[0])));
	for (T.r[2] = 3; T.r[2]; T.r[2]--)
	{
		T.r[0] = fl::add(asl(T.r[2]), 0x0300);
		T.r[1] = peek(T.r[0]);
		const uint8_t mode = peekb(0x032F);
		fl::cmpb(mode, 2);
		bool attr = false;
		if (mode == 2)
		{
			const uint8_t v = peekb((uint16_t)(0x6000 + T.r[1]));
			fl::tstb(v);
			attr = (v & 0x80) != 0;
		}
		if (attr)
		{
			pokeb(T.r[0], peekb((uint16_t)(0x6000 + T.r[1])));
			const uint16_t v = peek(T.r[0]) & 0x007F;
			poke(T.r[0], v);
			fl::mov(v);
		}
		else
		{
			T.r[3] = sext8(peekb((uint16_t)(0x4000 + T.r[1])));
			poke(T.r[0], T.r[3]);
			fl::mov(T.r[3]);
		}
	}
}

// F:934C (assembly): probe the playfield under the skater: from (x + 0x0A00 + 8 * dx,
// y + z + 0x0300 + 8 * (dy + dz)) (direction factors 0x02E3/0x02E5/0x02E7) three cells are
// read stepping back along the direction; per cell 0x0300 / 0x0306 / 0x030C / 0x0312 get the
// tile word, the row and column offsets and the video window address; 0x032E collects the
// high bits of the tiles; the last tile with bit 14 is remembered in 0x021C / 0x0214 / 0x021A
void probe_ground()
{
	bank_a(0x0800);
	bank_b(0x0C00);
	poke(0x0220, peek(0x06F0));
	poke(0x0220, (uint16_t)(peek(0x0220) + 0x0A00));
	T.r[0] = (uint16_t)(peek(0x0730) + peek(0x06B0));
	poke(0x021E, T.r[0]);
	poke(0x021E, (uint16_t)(peek(0x021E) + 0x0300));
	T.r[3] = (uint16_t)(sext8(peekb(0x02E3)) << 3);
	T.r[0] = sext8(peekb(0x02E5));
	T.r[1] = sext8(peekb(0x02E7));
	T.r[0] = (uint16_t)((T.r[0] + T.r[1]) << 3);
	T.r[5] = T.r[3];
	poke(0x0212, T.r[0]);
	poke(0x0220, (uint16_t)(peek(0x0220) + T.r[3]));
	poke(0x021E, (uint16_t)(peek(0x021E) + T.r[0]));
	pokeb(0x032E, 0);
	pokeb(0x0329, 0xFD);
	for (T.r[4] = 3; T.r[4]; T.r[4]--)
	{
		T.r[1] = peek(0x0220) & 0xFF00;
		T.r[0] = (uint16_t)((int16_t)peek(0x021E) >> 1) & 0x3F00;
		T.r[2] = (uint16_t)(0x3F00 - T.r[0]);
		poke(0x0218, T.r[2]);
		T.r[0] = T.r[2];
		call(0x8078, {});                              // select_video_bank
		T.r[2] &= 0x1F00;
		T.r[1] &= 0xFE00;
		T.r[3] = (uint16_t)((T.r[1] >> 8) + T.r[2]);
		T.r[3] = peek((uint16_t)(0x2000 + T.r[3]));
		if (T.r[3] & 0x4000)
		{
			poke(0x021C, peek(0x021E));
			poke(0x0214, T.r[1]);
			poke(0x021A, peek(0x0218));
		}
		T.r[0] = sext8((uint8_t)(T.r[3] >> 8));
		pokeb(0x032E, (uint8_t)(peekb(0x032E) | (uint8_t)T.r[0]));
		T.r[0] = (uint16_t)(T.r[4] << 1);
		poke((uint16_t)(0x0300 + T.r[0]), T.r[3]);
		poke((uint16_t)(0x0306 + T.r[0]), T.r[2]);
		poke((uint16_t)(0x030C + T.r[0]), T.r[1]);
		poke((uint16_t)(0x0312 + T.r[0]), peek(0x0218));
		poke(0x0220, (uint16_t)(peek(0x0220) - T.r[5]));
		poke(0x021E, fl::sub(peek(0x021E), peek(0x0212)));
	}
	poke(0x021E, peek(0x030A));
	poke(0x0220, peek(0x0310));
	poke(0x0218, peek(0x0316));
	const uint8_t v = (uint8_t)(peekb(0x032E) & ~0x3F);
	pokeb(0x032E, v);
	f_nzv(nzb(v));
}

// F:9B82  skater_motion_params(): index = direction 0x031D, moved into the slope tables by
// the terrain 0x0327 (bank B 0x2C00); direction factors 0x02E2-0x02E6 from the tables at
// 0x6048 / 0x6588 / 0x6AC8; on a ramp (0x01EA) the gravity 0x02EE (0x7668, halved on park 3
// below the speed limit 0x7C68), otherwise the jump values 0x02EA (0x7008) / 0x02EC (0x7228).
// Bank B restored; returns the index.  R1-R4 preserved (F152).
uint16_t skater_motion_params(const Args &)
{
	const uint16_t saved_b = peek(0x01D4);
	bank_b(0x2C00);
	uint16_t idx = sext8(peekb(0x031D));
	const uint16_t t = sext8(peekb(0x0327));
	if (t & 7)
	{
		if ((t & 7) != 7)
			idx = (uint16_t)(((t & 7) << 4) + peek((uint16_t)(0x79E8 + (uint16_t)(((int16_t)t >> 3) << 1))) + idx);
		else
			idx = (uint16_t)(((t & 0x38) << 1) + idx + 0x0220);
	}
	const uint16_t w = (uint16_t)(idx << 1);
	poke(0x02E2, peek((uint16_t)(0x6048 + w)));
	poke(0x02E4, peek((uint16_t)(0x6588 + w)));
	poke(0x02E6, peek((uint16_t)(0x6AC8 + w)));
	if (peekb(0x01EA))
	{
		const uint16_t g = sext8(peekb((uint16_t)(0x7668 + idx)));
		poke(0x02EE, g);
		if (peekb(0x0F59) == 3 && (int16_t)g < 0 && (int16_t)peek(0x01B4) > 0)
		{
			const uint16_t k = (uint16_t)(sext8(peekb(0x025C)) << 1);
			if ((int16_t)peek(0x01B4) < (int16_t)peek((uint16_t)(0x7C68 + k)))
				poke(0x02EE, (uint16_t)((int16_t)peek(0x02EE) >> 1));
		}
	}
	else if ((t & 7) != 7)
	{
		pokeb(0x02EA, peekb((uint16_t)(0x7008 + idx)));
		pokeb(0x02EB, 0);
		if (peek(0x02EA)) poke(0x02EA, (uint16_t)(peek(0x02EA) + 1));
		poke(0x02EC, peek((uint16_t)(0x7228 + w)));
	}
	else
	{
		poke(0x02EA, 0);
		poke(0x02EC, 0);
	}
	poke(0x01D4, saved_b);
	poke(0x1402, saved_b);
	return idx;
}

// F:9FB4  skater_take_off(): leave the ramp (0x01EA = 0): skater state saved (B3:7A9E) unless
// on park 4, a negative speed is turned into the opposite direction, speed remembered in 0x02E8,
// motion values (F:9B82, F:91A6), speed and z velocity scaled by the jump factors 0x02EA /
// 0x02EC (F:80FA), air timers cleared, gravity 0x02EE set, sound 0x4A; banks restored.
// R1-R3 preserved (F144).
uint16_t skater_take_off(const Args &)
{
	const uint16_t r1 = T.r[1], r2 = T.r[2], r3 = T.r[3];
	T.r[1] = peek(0x01D2);
	T.r[2] = peek(0x01D4);
	pokeb(0x01EA, 0);
	bank_a(0x3800);
	bank_b(0x3C00);
	if (peekb(0x0F59) != 4) call(0x7A9E, {});         // snapshot_skater
	if ((int16_t)peek(0x01B4) < 0)
	{
		T.r[0] = sext8(peekb(0x031D));
		T.r[3] = (uint16_t)(0x0008 ^ T.r[0]);
		pokeb(0x031D, (uint8_t)T.r[3]);
		poke(0x01B4, (uint16_t)-peek(0x01B4));
	}
	poke(0x02E8, peek(0x01B4));
	call(0x9B82, {});
	call(0x91A6, {});
	if (peekb(0x0EA0)) call(0xD742, {});
	call(0x80FA, {peek(0x02EA), peek(0x01B4)});        // mul32
	pokeb(0x01B5, peekb(0x0206));
	pokeb(0x01B4, peekb(0x0205));
	call(0x80FA, {peek(0x02EC), peek(0x01B4)});
	pokeb(0x07F8, peekb(0x0205));
	poke(0x05B0, peek(0x0206));
	if (peekb(0x0EA0)) call(0xD6BE, {});
	pokeb(0x0323, 0);
	poke(0x02F6, peek(0x01C6));
	poke(0x02F6, (uint16_t)(peek(0x02F6) << 1));
	poke(0x02F4, 0);
	poke(0x02F2, 0);
	poke(0x02EE, t11rt::g_patch_gravity);             // patch point F:A068 (default 12)
	const uint16_t r0 = call(0x8E78, {0x004A});        // queue_sound
	restore_banks(T.r[1], T.r[2]);
	T.r[1] = r1;
	T.r[2] = r2;
	T.r[3] = r3;
	return r0;
}

// F:9236 (assembly): the push force: when the button bit 7 of 0x0193 changes (0x0333) the
// boost 0x02FE grows by the equipment (0x7C80) and speed class (0x7CB0) values; the
// ability flag 0x0320 is updated; every 16th frame the boost decays (at least 0x180) and the
// push 0x02FC moves towards the speed limited by the equipment (0x7C88, + 0x80), or falls by
// 0x18 / rises by 6.  Bank B is left at 0x2C00.
void update_push()
{
	bank_b(0x2C00);
	T.r[0] = sext8(peekb(0x0193)) & 0x0080;
	T.r[1] = peekb(0x0333);
	if (T.r[0] != T.r[1])
	{
		pokeb(0x0333, peekb(0x0193));
		pokeb(0x0333, (uint8_t)(peekb(0x0333) & 0x80));
		T.r[0] = (uint16_t)(sext8(peekb(0x025F)) << 1);
		poke(0x02FE, (uint16_t)(peek(0x02FE) + peek((uint16_t)(0x7C80 + T.r[0]))));
		T.r[0] = sext8(peekb(0x0331));
		T.r[1] = peekb((uint16_t)(0x7CB0 + T.r[0]));
		poke(0x02FE, (uint16_t)(peek(0x02FE) + T.r[1]));
	}
	T.r[0] = (uint16_t)(sext8(peekb(0x025F)) << 1);
	T.r[1] = peek((uint16_t)(0x7C88 + T.r[0]));
	T.r[3] = peek(0x02FC);
	T.r[5] = 0;
	if ((int16_t)T.r[3] > (int16_t)T.r[1])
	{
		T.r[5] = 1;
		T.r[0] = 0;
	}
	else
		T.r[0] = 0x0040;
	T.r[4] = peekb(0x0333);
	if (T.r[4])
	{
		T.r[2] = (uint16_t)(T.r[0] + peek(0x01B4));
		if ((int16_t)T.r[3] > (int16_t)T.r[2])
			if (peekb(0x0F59) != 3 || (int16_t)peek(0x0ED0) > 2 || (int16_t)peek(0x01B4) >= (int16_t)0xFE80)
				pokeb(0x0320, 0);
	}
	bool carry = false;                                // from CMP R2,#4
	if (peekb(0x0330) == 4 || peekb(0x031E) != 0)
		pokeb(0x0320, 1);
	else
	{
		T.r[2] = sext8(peekb(0x0327)) & 7;
		if (T.r[2] < 4)
			carry = true;
		else
			pokeb(0x0320, 1);
	}
	if (peek(0x01C4) & 0x000F)
	{
		f_nzvc(nzw(peek(0x01C4) & 0x000F) | (carry ? CF : 0));
		return;
	}
	poke(0x02FE, (uint16_t)((int16_t)peek(0x02FE) >> 1));
	if ((int16_t)peek(0x02FE) < 0x0180) poke(0x02FE, 0x0180);
	T.r[0] = peek(0x01B4);
	if ((int16_t)T.r[0] > (int16_t)T.r[1]) T.r[0] = T.r[1];
	if ((int16_t)T.r[0] < (int16_t)peek(0x02FE)) T.r[0] = peek(0x02FE);
	T.r[1] = (uint16_t)(T.r[1] + 0x0080);
	if ((int16_t)T.r[0] > (int16_t)T.r[1]) T.r[0] = T.r[1];
	if ((int16_t)T.r[3] < (int16_t)T.r[0])
	{
		T.r[0] = fl::add(asr((uint16_t)(T.r[0] - T.r[3])), peek(0x02FC));
		poke(0x02FC, T.r[0]);
		fl::mov(T.r[0]);
		return;
	}
	bool down = true;
	if (T.r[4])
	{
		fl::tst(T.r[4]);
		T.r[5] = ror(T.r[5]);
		down = fC();
	}
	if (down)
		poke(0x02FC, fl::sub(peek(0x02FC), 0x0018));
	else
		poke(0x02FC, fl::add(peek(0x02FC), 0x0006));
}

// F:899C (interrupt, vertical blank during play): palette fade (F:DBE2) and the pending
// palette routine (F:DDEC), banked palette and sound level updates, watchdog, frame counters
// and acknowledges, timers 0x0F18 / 0x0F22, scroll registers and input, the sound queue;
// the text pointer, banks and video window are put back
void irq_vblank_play()
{
	for (int i = 0; i <= 5; i++) push(T.r[i]);
	push(peek(0x01D0));
	push(peek(0x01D2));
	push(peek(0x01D4));
	push(peek(0x0186));
	call(0xDBE2, {});
	call(0xDDEC, {});
	bank_a(0x1800);
	bank_b(0x1C00);
	call(0x7D82, {});                                  // blink_screen7_palette
	call(0x917C, {});                                  // feed_watchdog
	pokeb(0x0192, (uint8_t)(peekb(0x0192) + 1));
	poke(0x15E0, 0);
	poke(0x15C0, 0);
	pokeb(0x0191, 0);
	poke(0x01C4, (uint16_t)(peek(0x01C4) + 1));
	if (peek(0x0F18) != 0xFFFF) poke(0x0F18, (uint16_t)(peek(0x0F18) - 1));
	bank_a(0x1800);
	call(0x5D86, {});                                  // step_sound_level
	bank_a(0x3800);
	bank_b(0x3C00);
	call(0x7A3A, {});
	call(0x80A4, {});                                  // read_input_edges
	call(0x8036, {});                                  // load_scroll_registers
	if ((int16_t)peek(0x0F22) > 0 && !peek(0x0180)) poke(0x0F22, (uint16_t)(peek(0x0F22) - 1));
	if (!peekb(0x01A4))
	{
		bank_a(0x3800);
		bank_b(0x3C00);
		call(0x4314, {});                              // send_queued_sound
	}
	const uint16_t sp = T.r[6];
	poke(0x0186, rw(sp));
	poke(0x01D2, rw((uint16_t)(sp + 4)));
	poke(0x1400, peek(0x01D2));
	poke(0x01D4, rw((uint16_t)(sp + 2)));
	poke(0x1402, peek(0x01D4));
	poke(0x01D0, rw((uint16_t)(sp + 6)));
	call(0x8096, {});                                  // reselect_video_bank
	T.r[6] = (uint16_t)(T.r[6] + 8);
	for (int i = 5; i >= 0; i--) T.r[i] = pop();
}

uint16_t abs16(uint16_t v) { return (int16_t)v < 0 ? (uint16_t)-v : v; }

// tile word -> tile number: low 10 bits plus the base of its tile bank (bit 10: 0x01D8)
uint16_t tile_number(uint16_t tile)
{
	return (uint16_t)((tile & 0x03FF) + peek((tile & 0x0400) ? 0x01D8 : 0x01D6));
}

// F:A2F0  skater_land(): back on the ground (mode 0x031F = 1): kick state reset, ground
// friction 0x02FA from the equipment (0x7CAC), push / boost / 0x01DC cleared, speed made
// positive (direction reversed instead); at the speed limit of the board (0x7C90) on flat
// ground the speed is held there with the animation from 0x4488 (objects 1-2 state 6) and
// a random sound (0xFCF6), otherwise the animation for 0x01F8 and the speed kept within
// 0x180-0x280 (sound 0x54).  Banks restored; R1-R3 preserved (F144).
uint16_t skater_land(const Args &)
{
	const uint16_t r1 = T.r[1], r2 = T.r[2], r3 = T.r[3];
	T.r[2] = peek(0x01D2);
	T.r[3] = peek(0x01D4);
	bank_b(0x2C00);
	call(0xE238, {});                                  // reset_skater_state
	pokeb(0x031F, 1);
	T.r[0] = sext8(peekb(0x025E));
	pokeb(0x02FA, peekb((uint16_t)(0x7CAC + T.r[0])));
	pokeb(0x02FB, 0);
	if (peekb(0x01EA) == 2) pokeb(0x01EA, 1);
	pokeb(0x0325, 0xFF);
	poke(0x02FC, 0);
	poke(0x02FE, 0);
	poke(0x01DC, 0);
	if (!peekb(0x01EA))
	{
		poke(0x01B4, 0x0160);
		if ((int16_t)peek(0x05B2) > 0) poke(0x05B2, 0);
		call(0x9B82, {});
		call(0x91A6, {});
	}
	if ((int16_t)peek(0x01B4) < 0)
	{
		poke(0x01B4, (uint16_t)-peek(0x01B4));
		T.r[0] = sext8(peekb(0x031D));
		T.r[1] = (uint16_t)(0x0008 ^ T.r[0]);
		pokeb(0x031D, (uint8_t)T.r[1]);
	}
	T.r[1] = peek(0x01B4);
	T.r[0] = (uint16_t)(sext8(peekb(0x025D)) << 1);
	const uint16_t limit = peek((uint16_t)(0x7C90 + T.r[0]));
	uint16_t sound;
	if ((int16_t)T.r[1] >= (int16_t)limit && !(peekb(0x0327) & 7))
	{
		poke(0x01B4, limit);
		poke(0x01D2, 0);
		poke(0x1400, 0);
		bank_b(0x0400);
		T.r[0] = (uint16_t)(sext8(peekb(0x031D)) << 1);
		poke(0x03B4, peek((uint16_t)(0x4488 + T.r[0])));
		T.r[0] = 6;
		pokeb(0x0792, 6);
		pokeb(0x0772, 6);
		T.r[0] = call(0x9152, {}) & 3;                 // next_random
		sound = peekb((uint16_t)(0xFCF6 + T.r[0]));
	}
	else
	{
		T.r[0] = (uint16_t)((sext8(peekb(0x01F8)) << 1) + 0x4428);
		poke(0x03B4, T.r[0]);
		bool done = false;
		if (peekb(0x0F59) == 3)
		{
			T.r[0] = sext8(peekb(0x0327)) & 7;
			if (T.r[0] >= 5)
			{
				T.r[0] = (uint16_t)-(uint16_t)(peek(0x05B2) << 1);
				if ((int16_t)T.r[1] < (int16_t)T.r[0])
				{
					poke(0x01B4, T.r[0]);
					done = true;
				}
			}
		}
		if (!done)
		{
			if ((int16_t)T.r[1] < 0x0180)
				poke(0x01B4, 0x0180);
			else if ((int16_t)T.r[1] > 0x0280)
				poke(0x01B4, 0x0280);
		}
		sound = 0x0054;
	}
	call(0x8E78, {sound});                             // queue_sound
	pokeb(0x01F8, peekb(0x031D));
	poke(0x03B0, 0x50FA);
	poke(0x03B2, 0x50FA);
	restore_banks(T.r[2], T.r[3]);
	T.r[1] = r1;
	T.r[2] = r2;
	T.r[3] = r3;
	return T.r[0];
}

// F:A090  skater_touch_down(int terrain): on park 4 the slots are refilled for the level
// (F:90BA, tables 0x4579 / 0x7E50-0x7EC4, F:E1FA); objects 1-3 follow object 0 (A5:51BA)
// minus the carrier's velocity; the skater is on the ramp (0x01EA = 1) with terrain
// `terrain` meanwhile (corner codes 5-7 set the direction from 0xFCD0).  A pending landing
// (0x0323) lands him (F:A2F0, B6:7E54(1)); otherwise, unless already landed / mode 3, he
// turns to the direction 0x01F8 when F:A2BE has no bit for it (F:9B82, A7:410C, F:9EEE,
// speed from 0x02E8 when fast for the terrain, 0x7BE8), else he lands at the board's limit
// (B6:7E54(2)) or F:A45A runs (side from the turn).  The old terrain is put back;
// B6:7E54(0) unless landed.
// R1-R4 preserved (F152).
uint16_t skater_touch_down(const Args &a)
{
	const uint16_t saved[4] = { T.r[1], T.r[2], T.r[3], T.r[4] };
	bank_b(0x2C00);
	if (peekb(0x0F59) == 4 && !peekb(0x01F2))
	{
		bank_a(0x2400);
		T.r[0] = call(0x90BA, {});                     // park_level
		T.r[3] = peekb((uint16_t)(0x4579 + T.r[0]));
		T.r[1] = peekb(0x0FE0);
		T.r[3] = (uint16_t)(T.r[3] + T.r[1]);
		T.r[0] = (uint16_t)(T.r[3] << 1);
		const uint16_t w3 = peek((uint16_t)(0x7E50 + T.r[0]));
		const uint16_t w2 = peek((uint16_t)(0x7E8A + T.r[0]));
		const uint16_t w1 = peek((uint16_t)(0x7EC4 + T.r[0]));
		call(0xE1FA, {0x0078, w1, w2, w3});            // fill_slots
	}
	bank_a(0x1800);
	call(0x51BA, {});                                  // copy_object0_position
	T.r[0] = sext8(peekb(0x0EA0));
	if (T.r[0])
	{
		T.r[0] = (uint16_t)(T.r[0] << 1);
		poke(0x04F0, (uint16_t)(peek(0x04F0) - peek((uint16_t)(0x04F0 + T.r[0]))));
		poke(0x0570, (uint16_t)(peek(0x0570) - peek((uint16_t)(0x0570 + T.r[0]))));
	}
	T.r[3] = sext8(peekb(0x0327));
	pokeb(0x0327, a.b(0));
	pokeb(0x01EA, 1);
	if (peekb(0x0330) == 0x40) call(0xE238, {});      // reset_skater_state
	T.r[0] = sext8(peekb(0x0327)) & 7;
	if (T.r[0] >= 5)
	{
		T.r[0] = (uint16_t)((int16_t)sext8(peekb(0x0327)) >> 3) & 3;
		pokeb(0x031D, peekb((uint16_t)(0xFCD0 + T.r[0])));
		pokeb(0x0325, 0x7F);
	}
	if (peekb(0x0323) == 1)
	{
		call(0xA2F0, {});                              // skater_land
		bank_b(0x2800);
		call(0x7E54, {1});
		pokeb(0x0323, 0);
	}
	else if (peekb(0x031F) != 3 && peekb(0x031F) != 1)
	{
		T.r[0] = sext8(peekb(0x01F8));
		call(0xA2BE, {T.r[0]});                        // direction_bit
		if (!T.r[0])
		{
			T.r[1] = sext8(peekb(0x031D));
			pokeb(0x031D, peekb(0x01F8));
			call(0x9B82, {});                          // skater_motion_params
			bank_a(0x3800);
			call(0x410C, {});
			if (peekb(0x031E)) call(0x9EEE, {T.r[1]}); // skater_deflect
			if (peekb(0x0F59) != 3)
			{
				T.r[1] = peek(0x02E8);
				bool faster = false;
				if (peekb(0x0327) & 7)
				{
					T.r[0] = abs16(peek(0x01B4));
					faster = (int16_t)T.r[1] > (int16_t)T.r[0];
				}
				if (!faster)
				{
					T.r[0] = abs16(peek(0x01B4));
					T.r[2] = (uint16_t)((((sext8(peekb(0x0327)) & 7) << 2) + sext8(peekb(0x025C))) << 1);
					faster = (int16_t)T.r[0] > (int16_t)peek((uint16_t)(0x7BE8 + T.r[2]));
				}
				if (faster)
				{
					if ((int16_t)peek(0x01B4) <= 0) T.r[1] = (uint16_t)-T.r[1];
					poke(0x01B4, T.r[1]);
				}
			}
		}
		else
		{
			call(0xE6F0, {});                          // object1_screen_x
			call(0x9B82, {});                          // skater_motion_params
			T.r[2] = peek(0x05B2);
			bank_a(0x3800);
			call(0x410C, {});
			T.r[0] = (uint16_t)(sext8(peekb(0x025D)) << 1);
			if ((int16_t)peek(0x01B4) >= (int16_t)peek((uint16_t)(0x7C90 + T.r[0]))
				&& (int16_t)T.r[2] < (int16_t)peek((uint16_t)(0x7C98 + T.r[0])) && peekb(0x0F59) != 3)
			{
				call(0xA2F0, {});                      // skater_land
				bank_b(0x2800);
				call(0x7E54, {2});
			}
			else
			{
				T.r[2] = sext8(peekb(0x01F8));
				T.r[0] = sext8(peekb(0x031D));
				T.r[2] = (uint16_t)(T.r[2] - T.r[0]);
				if (T.r[2] & 0x8000)
				{
					poke(0x0222, 0xFFFF);
					T.r[2] = (uint16_t)-T.r[2];
				}
				else
					poke(0x0222, 1);
				if ((int16_t)T.r[2] > 8) poke(0x0222, (uint16_t)-peek(0x0222));
				T.r[0] = sext8(peekb(0x01F8));
				call(0xA45A, {peek(0x0222), T.r[0]});  // skater_phase10
			}
		}
	}
	pokeb(0x0327, (uint8_t)T.r[3]);
	if (peekb(0x031F) != 1)
	{
		bank_b(0x2800);
		call(0x7E54, {0});
	}
	for (int k = 0; k < 4; k++) T.r[k + 1] = saved[k];
	return T.r[0];
}

// F:94DC (assembly): edge handling by the mode 0x032F, from the high bits of the probed
// tiles (0x032E) and the attributes (0x6000) of the probed cell 2:
//   0  bit 6 starts mode 2, a lone bit 7 mode 1
//   1  (bit 6 on the ground, not after 0x0328 == 0xFF): on the ramp a wall in the direction
//      of travel (cells 1 / 3, bit 14) deflects the skater (F:9EEE(0x3F)); otherwise the cell
//      beyond the edge (0x0214 / 0x021C) decides: a solid one (negative attribute, or bit 6
//      with a corner code) bounces him back (B1:7B3E for bit 6, direction reversed, speed
//      0x240, motion values, state 2), else mode 2 and, for a negative attribute, a drop to
//      the ground below (F:9C5E mode 0x7FFF) or by 0x200
//   2  a lone bit 7 over a non-negative attribute: back up onto the level found by F:9C5E
//      (mode 0x7FFE) when it is below object 1, else mode 2
// The jump table 0xFCF0 has only these three modes; its further entries are the next
// table's data, so other modes do nothing here.
void edge_mode()
{
	bank_a(0x0800);
	bank_b(0x0C00);
	poke(0x0222, 0);
	T.r[2] = tile_number(peek(0x0304));
	T.r[0] = peekb(0x032E);
	const uint8_t mode = peekb(0x032F);
	T.r[1] = (uint16_t)(sext8(mode) << 1);
	if (mode == 0)
	{
		if (T.r[0] & 0x40)
		{
			pokeb(0x032F, 2);
			f_nzvc(0);
			return;
		}
		fl::cmp(T.r[0], 0x0080);
		if (T.r[0] == 0x0080)
		{
			pokeb(0x032F, 1);
			f_nzvc(0);
		}
		return;
	}
	if (mode == 1)
	{
		if (!T.r[0])
		{
			pokeb(0x032F, 0);
			fl::clr();
			return;
		}
		if (!(T.r[0] & 0x40))
		{
			f_nzvc(ZF);
			return;
		}
		pokeb(0x0329, 0xFF);
		if (peekb(0x0328) == 0xFF)
		{
			f_nzvc(ZF);
			return;
		}
		if (peekb(0x01EA))
		{
			T.r[0] = peek(0x01B4);
			if (!(T.r[0] & 0x8000))
			{
				if (!(peek(0x0306) & 0x4000))
				{
					fl::tst(T.r[0]);
					return;
				}
			}
			else if (!(peek(0x0302) & 0x4000))
			{
				f_nzvc(ZF);
				return;
			}
			call(0x9EEE, {0x003F});                    // skater_deflect
			poke(0x0222, 1);
			pokeb(0x0328, peekb(0x0329));
			fl::tst(0x003F);
			return;
		}
		poke(0x0220, peek(0x0214));
		T.r[0] = (uint16_t)((int16_t)(uint16_t)(peek(0x06B2) - peek(0x06B0) + peek(0x021C)) >> 1) & 0x3F00;
		T.r[1] = (uint16_t)(0x3E00 - T.r[0]);
		T.r[0] = T.r[1];
		call(0x8078, {});                              // select_video_bank
		poke(0x0218, T.r[1]);
		T.r[1] &= 0x1F00;
		poke(0x021E, T.r[1]);
		T.r[0] = peekb(0x0221);
		T.r[1] = (uint16_t)(T.r[1] + T.r[0]);
		T.r[1] = tile_number(peek((uint16_t)(0x2000 + T.r[1])));
		T.r[3] = sext8(peekb((uint16_t)(0x6000 + T.r[2])));
		bool solid = (peekb((uint16_t)(0x6000 + T.r[1])) & 0x80) != 0;
		if (!solid && (T.r[3] & 0x40))
		{
			T.r[0] = (uint16_t)(7 & ~(T.r[3] & 0xFF));
			solid = T.r[0] == 0;
		}
		if (solid)
		{
			if (T.r[3] & 0x40)
			{
				bank_a(0x1800);
				bank_b(0x1C00);
				call(0x7B3E, {T.r[3]});
				bank_a(0x0800);
				bank_b(0x0C00);
			}
			T.r[0] = sext8(peekb(0x031D));
			T.r[1] = (uint16_t)(0x0008 ^ T.r[0]);
			pokeb(0x031D, (uint8_t)T.r[1]);
			poke(0x01B4, 0x0240);
			T.r[0] = sext8(peekb(0x0327));
			poke(0x021A, T.r[0]);
			pokeb(0x0327, 0);
			call(0x9B82, {});                          // skater_motion_params
			call(0x91A6, {});                          // skater_velocity
			pokeb(0x0327, peekb(0x021A));
			call(0x9B82, {});
			if ((int16_t)peek(0x05B2) > 0) poke(0x05B2, 0);
			poke(0x0222, 1);
			pokeb(0x031E, 2);
			pokeb(0x031C, 6);
			pokeb(0x0328, peekb(0x0329));
		}
		else
		{
			pokeb(0x032F, 2);
			if (T.r[3] & 0x8000)
			{
				poke(0x0212, 0x0100);
				poke(0x06B0, peek(0x06B2));
				T.r[0] = peek(0x021E);
				T.r[1] = peek(0x0220);
				call(0x9C5E, {0x7FFF});                // trace_cells
				T.r[2] = T.r[0];
				if (peek(0x0222) != 0x7FFF)
				{
					poke(0x0304, T.r[2]);
					poke(0x06B0, peek(0x0222));
					poke(0x0222, 0xFFFF);
				}
				else
				{
					poke(0x06B0, (uint16_t)(peek(0x06B0) - 0x0200));
					poke(0x0222, 1);
				}
				pokeb(0x031E, 2);
				pokeb(0x031C, 6);
				pokeb(0x0328, peekb(0x0329));
			}
		}
		call(0x9B82, {});
		const uint16_t speed = peek(0x01B4), factor = peek(0x02EC);
		call(0x80FA, {factor, speed});                 // mul32
		pokeb(0x07F8, peekb(0x0205));
		poke(0x05B0, peek(0x0206));
		fl::cmp(factor, speed);                        // CMP (SP)+,(SP)+
		return;
	}
	if (mode == 2)
	{
		if (!T.r[0])
		{
			pokeb(0x032F, 0);
			fl::clr();
			return;
		}
		const uint8_t attr = peekb((uint16_t)(0x6000 + T.r[2]));
		if (attr & 0x80)
		{
			fl::tstb(attr);
			return;
		}
		if (!(T.r[0] & 0x80))
		{
			fl::tstb((uint8_t)T.r[0]);
			return;
		}
		pokeb(0x0329, 0xFF);
		if (peekb(0x0328) == 0xFF)
		{
			f_nzvc(ZF);
			return;
		}
		pokeb(0x032F, 1);
		poke(0x0212, 0x0100);
		T.r[0] = peek(0x021E);
		T.r[1] = peek(0x0220);
		call(0x9C5E, {0x7FFE});                        // trace_cells
		T.r[2] = T.r[0];
		T.r[1] = peek(0x0222);
		bool up = false;
		if (T.r[1] != 0x7FFF)
		{
			T.r[0] = (uint16_t)(peek(0x06B2) - T.r[1]);
			if ((int16_t)T.r[0] > 0)
			{
				poke(0x0304, T.r[2]);
				poke(0x06B0, T.r[1]);
				poke(0x0222, 0xFFFF);
				pokeb(0x031E, 2);
				pokeb(0x031C, 6);
				pokeb(0x0328, peekb(0x0329));
				up = true;
			}
		}
		if (!up)
		{
			pokeb(0x032F, 2);
			poke(0x0222, 1);
		}
		fl::tst(0x7FFE);                               // TST (SP)+
	}
}

// F:979A (assembly): walls.  Unless an edge was handled (0x0222), the probed cell ahead
// (1 with a corner code, else 2 or 0 by the direction of travel; 0x021C) is checked once per
// new code (0x0329 / 0x0328): a wall code (slope F:9E52 not negative) bounces the skater
// (F:9E96) when the wall rises above him (B1:7B3E walls always; F:9C5E with his height
// otherwise), or puts him onto it from cell 1; on the ramp he is deflected (F:9EEE), or a
// matching corner takes him off (F:9FB4, A5:5264).  A negative slope is a step: going up
// it (A5:51EE in the direction of travel) the next level is found (F:9C5E mode 0x7FFD) and
// the skater moves onto it (only up to 0x200 below object 1), state 2.
void check_wall()
{
	bank_a(0x0800);
	bank_b(0x0C00);
	const uint16_t pending = peek(0x0222);
	if (pending)
	{
		fl::tst(pending);
		return;
	}
	poke(0x021C, 0xFFFF);
	if (!(7 & ~peek(0x0304)))
		poke(0x021C, 1);
	else if ((int16_t)peek(0x01B4) > 0)
	{
		if (!(7 & ~peek(0x0306))) poke(0x021C, 2);
	}
	else if (!(7 & ~peek(0x0302)))
		poke(0x021C, 0);
	T.r[0] = peek(0x021C);
	if (T.r[0] & 0x8000)
	{
		f_nzvc(NF);
		return;
	}
	T.r[0] = (uint16_t)(T.r[0] << 1);
	const uint8_t code = peekb((uint16_t)(0x0302 + T.r[0]));
	pokeb(0x0329, code);
	if (code == peekb(0x0328))
	{
		f_nzvc(ZF);
		return;
	}
	T.r[3] = sext8(code);
	T.r[1] = 0;
	if (T.r[3] == 0x0037)
		T.r[2] = 0xFFFF;
	else
	{
		T.r[1] = 1;
		T.r[2] = call(0x9E52, {T.r[3]});              // slope_table_value
	}
	if (!(T.r[2] & 0x8000))
	{
		T.r[0] = (uint16_t)(peek(0x021C) << 1);
		poke(0x021E, peek((uint16_t)(0x0308 + T.r[0])));
		poke(0x0220, peek((uint16_t)(0x030E + T.r[0])));
		poke(0x0218, peek((uint16_t)(0x0314 + T.r[0])));
		T.r[0] = peek((uint16_t)(0x0314 + T.r[0]));
		call(0x8078, {});                              // select_video_bank
		if (peekb(0x01EA))
		{
			T.r[0] = sext8(peekb(0x0327)) & 7;
			if (T.r[0] >= 5)
			{
				T.r[1] = sext8(peekb(0x0327)) & 0x0038;
				T.r[0] = T.r[3] & 0x0038;
				if (T.r[1] == T.r[0])
				{
					T.r[0] = (uint16_t)(7 & ~peek(0x0304));
					if (T.r[0])
					{
						f_nzvc(nzw(T.r[0]));
						return;
					}
					pokeb(0x0327, (uint8_t)T.r[3]);
					call(0x9FB4, {});                  // skater_take_off
					pokeb(0x031E, 2);
					pokeb(0x031C, 2);
					pokeb(0x0328, peekb(0x0329));
					bank_a(0x1800);
					call(0x5264, {});
					bank_a(0x0800);
					bank_b(0x2C00);
					fl::mov(0x2C00);
					return;
				}
			}
			call(0x9EEE, {T.r[2]});                    // skater_deflect
			pokeb(0x0328, peekb(0x0329));
			fl::tst(T.r[2]);
			return;
		}
		uint16_t slot = T.r[3];                        // the word left on the stack
		if (T.r[3] & 0x40)
		{
			bank_a(0x1800);
			bank_b(0x1C00);
			call(0x7B3E, {T.r[3]});
			bank_a(0x0800);
			bank_b(0x0C00);
			T.r[3] = 0x8000;
		}
		else
		{
			T.r[3] = (uint16_t)(peek(0x06B2) + peek(0x05B2));
			poke(0x0212, 0xFF00);
			slot = T.r[3];
			T.r[0] = peek(0x021E);
			T.r[1] = peek(0x0220);
			call(0x9C5E, {T.r[3]});                    // trace_cells
			T.r[3] = T.r[0];
		}
		if (T.r[3] == 0x8000)
		{
			slot = T.r[2];
			call(0x9E96, {T.r[2]});                    // skater_bounce
			pokeb(0x0328, peekb(0x0329));
		}
		else if (peek(0x021C) == 1)
		{
			poke(0x0304, T.r[3]);
			poke(0x06B0, peek(0x0222));
			pokeb(0x031E, 2);
			pokeb(0x031C, 6);
			pokeb(0x0328, peekb(0x0329));
		}
		fl::tst(slot);                                 // TST (SP)+
		return;
	}
	fl::tst(T.r[2]);
	T.r[1] = ror(T.r[1]);
	if (fC())
	{
		bank_a(0x1800);
		T.r[2] = call(0x51EE, {peek(0x0304)});
		bank_a(0x0800);
		if ((int16_t)peek(0x01B4) < 0) T.r[2] = (uint16_t)-T.r[2];
	}
	else
		T.r[2] = peek(0x021C) == 1 ? 1 : 0;
	if ((int16_t)T.r[2] <= 0)
	{
		fl::tst(T.r[2]);
		return;
	}
	poke(0x0212, 0x0100);
	poke(0x0218, peek(0x0316));
	T.r[0] = peek(0x030A);
	T.r[1] = peek(0x0310);
	call(0x9C5E, {0x7FFD});                            // trace_cells
	poke(0x0304, T.r[0]);
	T.r[0] = (uint16_t)(peek(0x06B2) - peek(0x0222));
	if ((int16_t)T.r[0] >= 0x0200)
		poke(0x06B0, peek(0x0222));
	else
		poke(0x06B0, peek(0x06B2));
	if (peekb(0x0F59) == 4)
	{
		T.r[0] = (uint16_t)(peek(0x06B2) - peek(0x06B0));
		if ((int16_t)T.r[0] > 0x2800)
		{
			bank_a(0x3800);
			bank_b(0x3C00);
			call(0x7A9E, {});                          // snapshot_skater
		}
	}
	pokeb(0x0328, peekb(0x0329));
	pokeb(0x031C, 2);
	pokeb(0x031E, 2);
	fl::tst(0x7FFD);                                   // TST (SP)+
}

// F:A9FC  run_text_script(const int *script): first the screen part of the script (an
// address from 0x4000 on: F:EAD4, else A7:4D30 with the script), then commands (word: low
// byte, high byte; 0xFFFF ends) until the end; bank B 0x2800 meanwhile, banks restored.
//   low byte < 0x80: string, drawn by the high byte: 0 large, 1 small, 2-7 large with a
//                    variant (level class, A?:5326, 0x02B8, 0x0261, random pair, 0x020A pair)
//   low byte >= 0x80: a number (0: zero, 0x81-0x84: level of park n, 0x85-0x87: 0x0F38,
//                    0x88-0x8D: ratings, the park in 0x01F5, ..., 0x8E-0x91: the bytes
//                    0x025C-0x025F, 0x92: the word 0x0248) at the screen position
//                    from the high byte bits 3-7 (relative) or the next word, in the format
//                    of bits 0-2: digit, digit block, F:BA9A / F:BA96 counters, small digit
// The arguments of the screen part stay on the stack; the counter formats pass their last
// argument in that slot.  R1-R5 preserved (F162).
uint16_t run_text_script(const Args &a)
{
	const uint16_t saved[5] = { T.r[1], T.r[2], T.r[3], T.r[4], T.r[5] };
	push(peek(0x01D2));
	push(peek(0x01D4));
	bank_b(0x2800);
	T.r[2] = a.u(0);
	if ((int16_t)peek(T.r[2]) >= 0x4000)
	{
		push(peek(T.r[2]));
		push(0);
		call(0xEAD4, {});
		T.r[2] = (uint16_t)(T.r[2] + 2);
	}
	else
	{
		bank_a(0x3800);
		push(T.r[2]);
		push(0);
		call(0x4D30, {});
		T.r[2] = (uint16_t)(T.r[2] + 4);
	}
	// level class bits of the current rating entry (0x0262 table, by 0x0F31 / 0x0F2B)
	auto rating_class = []() -> uint16_t {
		uint16_t i = peekb(0x0F31);
		if (peekb(0x0F2B) != 2) i = (uint16_t)(i - 1);
		return (uint16_t)((peekb((uint16_t)(0x0262 + i)) >> 6) & 3);
	};
	for (;;)
	{
		poke(0x020C, peek(T.r[2]));
		T.r[2] = (uint16_t)(T.r[2] + 2);
		if (peek(0x020C) == 0xFFFF) break;
		T.r[3] = peekb(0x020C);
		if (!(T.r[3] & 0x80))
		{
			const uint8_t how = peekb(0x020D);
			T.r[0] = (uint16_t)(how << 1);
			switch (how)
			{
			case 0:
				T.r[0] = T.r[3];
				call(0x8AB6, {});                      // draw_string_large
				break;
			case 1:
				T.r[0] = T.r[3];
				call(0x8B04, {});                      // draw_string_small
				break;
			case 2:
				T.r[0] = (uint16_t)(rating_class() + T.r[3] - 1);
				call(0x8AB6, {});
				break;
			case 3:
				T.r[0] = (uint16_t)(call(0x5326, {}) + T.r[3]);
				call(0x8AB6, {});
				break;
			case 4:
				T.r[0] = (uint16_t)(peekb(0x02B8) + T.r[3]);
				call(0x8AB6, {});
				break;
			case 5:
				T.r[0] = (uint16_t)(peekb(0x0261) + T.r[3]);
				call(0x8AB6, {});
				break;
			case 6:
				T.r[0] = call(0x9152, {});             // next_random
				T.r[1] = T.r[0] & 1;
				T.r[4] = (uint16_t)(T.r[1] << 1);
				T.r[0] = (uint16_t)(peekb(0x020C) + T.r[4]);
				call(0x8AB6, {});
				T.r[0] = (uint16_t)(peekb(0x020C) + T.r[4] + 1);
				call(0x8AB6, {});
				break;
			case 7:
				T.r[1] = (uint16_t)((peek(0x020A) & 1) << 1);
				T.r[0] = (uint16_t)(T.r[1] + T.r[3]);
				call(0x8AB6, {});
				T.r[0] = (uint16_t)(peekb(0x020C) + T.r[1] + 1);
				call(0x8AB6, {});
				break;
			default:                                   // past the table (next table's data): unused
				break;
			}
			continue;
		}
		const uint16_t sel = (uint16_t)(T.r[3] - 0x80);
		T.r[0] = (uint16_t)(sel << 1);
		switch (sel)
		{
		case 0:
			T.r[1] = 0;
			break;
		case 1: case 2: case 3: case 4:
			T.r[5] = sext8(peekb(0x027A));
			T.r[0] = (uint16_t)(T.r[3] - 0x81);
			pokeb(0x027A, (uint8_t)T.r[0]);
			T.r[1] = call(0x90BA, {});                 // park_level
			pokeb(0x027A, (uint8_t)T.r[5]);
			break;
		case 5: case 6: case 7:
			T.r[1] = peek(0x0F38);
			break;
		case 8:
			T.r[0] = rating_class();
			poke(T.r[6], peekb((uint16_t)(0xFE8C + T.r[0])));
			call(0x90BA, {});
			T.r[1] = call(0x80D8, {T.r[0]});           // mul16 with the slot
			break;
		case 9:
			T.r[0] = peekb((uint16_t)(0xFE7C + peekb(0x0F31)));
			T.r[1] = peekb((uint16_t)(0xFE70 + T.r[0]));
			break;
		case 10:
			T.r[1] = (uint16_t)(peekb(0x01F9) >> 2);
			break;
		case 11:
			T.r[1] = call(0x90BA, {});
			break;
		case 12:
			T.r[1] = (uint16_t)(peekb(0x01F5) + 1);
			break;
		case 13:
			T.r[0] = peekb(0x01F5);
			T.r[1] = (uint16_t)(2 - T.r[0]);
			break;
		case 14: case 15: case 16: case 17:            // the bytes 0x025C-0x025F
			T.r[1] = peekb((uint16_t)(0x01CE + T.r[3]));
			if (T.r[1] & 0x80) T.r[1] = 0;
			break;
		case 18:
			T.r[1] = peek(0x0248);
			break;
		default:                                       // past the table: unused
			continue;
		}
		T.r[4] = peekb(0x020D);
		if (T.r[4] & 0x00F8)
		{
			T.r[0] = (uint16_t)(((T.r[4] >> 1) & ~3) + peek(0x0186));
			poke(0x0186, T.r[0]);
			poke(0x0186, (uint16_t)(peek(0x0186) - 0x0040));
		}
		else
		{
			poke(0x0186, (uint16_t)(peek(T.r[2]) + 0x2000));
			T.r[2] = (uint16_t)(T.r[2] + 2);
		}
		const uint16_t format = T.r[4] & 7;
		T.r[4] = (uint16_t)(format << 1);
		switch (format)
		{
		case 0:
			if ((int16_t)T.r[1] < 10)
			{
				T.r[4] = (uint16_t)(T.r[1] + 0x30);
				T.r[0] = T.r[4];
			}
			else
				T.r[0] = 0x003D;
			call(0x8B52, {});                          // draw_large_char
			break;
		case 1:
		{
			T.r[0] = peek(0x0186);
			T.r[4] = peek(0x0188);
			poke(T.r[0], (uint16_t)(T.r[4] | 0x0138));
			poke((uint16_t)(T.r[0] + 2), (uint16_t)(T.r[4] | 0x0139));
			poke((uint16_t)(T.r[0] + 0x80), (uint16_t)(T.r[4] | 0x013A));
			poke((uint16_t)(T.r[0] + 0x82), (uint16_t)(T.r[4] | 0x013B));
			poke(0x0186, (uint16_t)(peek(0x0186) + 4));
			poke(T.r[6], T.r[1]);
			call(0xBA9A, {1, 0, 3, 0});
			break;
		}
		case 2:
			poke(T.r[6], T.r[1]);
			call(0xBA9A, {1, 1, 4, 0});
			break;
		case 3:
			poke(T.r[6], T.r[1]);
			call(0xBA9A, {0, 1, 2, 0});
			break;
		case 4:
			poke(T.r[6], peek(0x023E));
			call(0xBA96, {1, 0, 6, peek(0x0240)});
			break;
		case 5:
			poke(T.r[6], peek(0x0242));
			call(0xBA96, {0, 0, 3, 0});
			break;
		case 6:
			T.r[4] = (uint16_t)(T.r[1] + 0x30);
			T.r[0] = T.r[4];
			call(0x8C12, {});                          // put_small_char
			break;
		case 7:
			poke(T.r[6], T.r[1]);
			call(0xBA9A, {1, 1, 5, 0});
			break;
		}
	}
	const uint16_t sp = T.r[6];
	poke(0x01D2, rw((uint16_t)(sp + 6)));
	poke(0x01D4, rw((uint16_t)(sp + 4)));
	poke(0x1400, peek(0x01D2));
	poke(0x1402, peek(0x01D4));
	T.r[6] = (uint16_t)(T.r[6] + 8);
	for (int k = 0; k < 5; k++) T.r[k + 1] = saved[k];
	return T.r[0];
}

// F:A510 (assembly; attract mode): contact of the skater with another object (A7:5684 finds
// it; bank A 0, bank B 0x0400 for the tables).  An object too far below a skater in the air
// (0x6631) is ignored; otherwise its type (0x61DA by the object's kind 0x0370) decides, once
// per object (0x0334):
//   2, 7   the skater lands on it (direction from F:AD70, speed 0x514, F:A2F0); kind 4 also
//          ends the run (F:CF5E, F:E222)
//   3, 4   the object is hit (sound 0x67, F:B602, animation 0xFCFA, F:EF24)
//   5      the skater rides it (carrier 0x0EA0 = -object) unless on the ground in mode 1
//   6      bumped: near the skater's height as for 2; else the queued event 0x0E6C runs
//          (F:F064, F:EF24); then A7:4ECC and F:D47C
//   8      F:F002 / F:CF5E (sound 0x61);  9  bounce or deflect by the kind (0x14 or other)
//   10, 11 the pair of objects (F:A8D2) both change kind (0x17 / 0x18), with points 0x0F20
//   12     on level ground fast enough: a bump (sound 0x7B, F:9F8A on the ramp)
// Without an object the contact timers 0x0EA2 / 0x0EA1 / 0x0334 run out.  The local frame
// on the stack (object kind pointer, ramp flag) is kept as the original's.
void object_contact()
{
	T.r[6] = (uint16_t)(T.r[6] - 6);
	const uint16_t fp = T.r[6];                        // fp+2: &0x0370[object], fp+4: on the ramp
	if (!peekb(0x0EA2))
	{
		poke(0x023A, peek(0x03F0));
		poke(0x0238, peek(0x0670));
	}
	else
	{
		poke(0x023A, 0x7106);
		poke(0x0238, peek(0x0670));
		poke(0x0238, (uint16_t)(peek(0x0238) - peek(0x06B0)));
	}
	bank_a(0x3800);
	bank_b(0x0400);
	call(0x5684, {});
	T.r[1] = T.r[0];
	poke(0x01D2, 0);
	poke(0x1400, 0);
	if ((int16_t)T.r[1] <= 0)
	{
		if (peekb(0x0EA2)) pokeb(0x0EA2, (uint8_t)(peekb(0x0EA2) - 1));
		pokeb(0x0EA1, 0);
		pokeb(0x0334, 0);
		T.r[6] = fl::add(T.r[6], 6);
		return;
	}
	poke((uint16_t)(fp + 2), (uint16_t)(0x0370 + T.r[1]));
	auto kind = [fp]() -> uint16_t { return peekb(peek((uint16_t)(fp + 2))); };
	T.r[0] = kind();
	poke((uint16_t)(fp + 4), 0);
	if (peekb(0x01EA))
		poke((uint16_t)(fp + 4), (uint16_t)(peek((uint16_t)(fp + 4)) + 1));
	else
	{
		T.r[3] = (uint16_t)(peek(0x06B2) - peek(0x06B0));
		T.r[2] = (uint16_t)(peekb((uint16_t)(0x6631 + T.r[0])) << 8);
		if ((int16_t)T.r[3] > (int16_t)T.r[2])
		{
			T.r[6] = fl::add(T.r[6], 6);
			return;
		}
	}
	T.r[0] = peekb((uint16_t)(0x61DA + T.r[0]));
	T.r[2] = (uint16_t)(T.r[0] << 1);
	// F:AD70 with object 0: 4 when the skater is in front of the object, else 12
	auto facing = []() -> uint16_t {
		T.r[0] = 0;
		call(0xAD70, {});
		return (T.r[0] & 1) ? 4 : 12;
	};
	switch (T.r[0])
	{
	case 2: case 7:
		T.r[0] = peekb(0x0334);
		if (T.r[0] == T.r[1]) break;
		if (!peekb(0x031E))
		{
			T.r[0] = facing();
			pokeb(0x031D, (uint8_t)T.r[0]);
		}
		pokeb(0x01F8, peekb(0x031D));
		poke(0x01B4, 0x0514);
		call(0xA2F0, {});                              // skater_land
		T.r[0] = kind();
		if (T.r[0] != 4) break;
		call(0xCF5E, {peekb((uint16_t)(0x0390 + T.r[1]))});
		call(0xE222, {});                              // state1_to_8
		break;
	case 3: case 4:
		call(0x8E78, {0x0067});                        // queue_sound
		T.r[2] = (uint16_t)(T.r[1] << 1);
		poke((uint16_t)(0x03F0 + T.r[2]), 0);
		call(0xB602, {peekb(0x5CDE), T.r[1]});
		pokeb((uint16_t)(0x04B0 + T.r[1]), (uint8_t)(peekb((uint16_t)(0x04B0 + T.r[1])) | 0x08));
		T.r[0] = 7;
		pokeb((uint16_t)(0x0790 + T.r[1]), 7);
		pokeb((uint16_t)(0x0770 + T.r[1]), 7);
		poke((uint16_t)(0x0430 + T.r[2]), 0x0080);
		T.r[0] = (uint16_t)((kind() - 0x48) << 1);
		call(0xEF24, {peek((uint16_t)(0xFCFA + T.r[0])), T.r[1]});
		break;
	case 5:
		if (!peekb(0x0EA0) && peekb(0x031F) != 1)
		{
			pokeb(0x0EA0, (uint8_t)T.r[1]);
			pokeb(0x0EA0, (uint8_t)-peekb(0x0EA0));
		}
		break;
	case 6:
		T.r[0] = peekb(0x0334);
		if (T.r[0] == T.r[1]) break;
		T.r[0] = (uint16_t)(peek(0x06B2) - peek(0x06B0));
		if ((int16_t)T.r[0] < 0x0380)
		{
			T.r[0] = facing();
			pokeb(0x031D, (uint8_t)T.r[0]);
			pokeb(0x01F8, (uint8_t)T.r[0]);
			poke(0x01B4, 0x0514);
			call(0xA2F0, {});                          // skater_land
		}
		else
		{
			T.r[0] = peekb(0x0E6C);
			if (T.r[0])
			{
				call(0xF064, {T.r[0]});
				T.r[0] = (uint16_t)(peekb(0x0E6C) << 1);
				call(0xEF24, {peek((uint16_t)(0xFCFA + T.r[0]))});
				if (kind() == peekb(0x0E6B)) pokeb(0x0E6C, (uint8_t)(peekb(0x0E6C) - 1));
				pokeb(0x0E6B, (uint8_t)kind());
			}
		}
		bank_a(0x3800);
		bank_b(0x3C00);
		call(0x4ECC, {T.r[1]});
		call(0xD47C, {T.r[1]});
		break;
	case 8:
		call(0x8E78, {0x0061});                        // queue_sound
		T.r[0] = (uint16_t)(kind() - 0x38);
		T.r[2] = sext8(peekb((uint16_t)(0xFD10 + T.r[0])));
		call(0xF002, {T.r[2]});
		call(0xCF5E, {peekb((uint16_t)(0x0390 + T.r[1]))});
		break;
	case 9:
		T.r[0] = kind() == 0x14 ? 7 : 0x0F;
		pokeb(0x0329, (uint8_t)T.r[0]);
		T.r[2] = peekb(0x0328);
		if (T.r[2] == T.r[0]) break;
		pokeb(0x0328, (uint8_t)T.r[0]);
		if (peek((uint16_t)(fp + 4)) & 1)
		{
			call(0x9E52, {T.r[0]});                    // slope_table_value
			call(0x9EEE, {T.r[0]});                    // skater_deflect
		}
		else
		{
			T.r[0] = peekb(0x031D);
			call(0x9E96, {(uint16_t)(0x0008 ^ T.r[0])}); // skater_bounce
		}
		break;
	case 10:
	{
		T.r[0] = call(0xA8D2, {T.r[1]});
		T.r[4] = T.r[0];
		T.r[0] = (uint16_t)(kind() - 0x1A);
		T.r[5] = (uint16_t)(T.r[4] << 1);
		T.r[2] = (uint16_t)(0x03B0 + T.r[5]);
		poke(fp, (uint16_t)(T.r[1] << 1));
		T.r[3] = (uint16_t)(0x03B0 + peek(fp));
		bool far = true;
		if ((int16_t)T.r[0] < 5 || (int16_t)T.r[0] > 7)
		{
			T.r[0] = 0;
			call(0xAD70, {});
			far = !(T.r[0] & 1);
		}
		if (!far)
		{
			poke(T.r[2], (uint16_t)(peek(T.r[2]) + 2));
			poke(T.r[3], (uint16_t)(peek(T.r[3]) + 2));
			T.r[0] = 0x0018;
		}
		else
		{
			poke(0x0F20, (uint16_t)(peek(0x0F20) + 6));
			if ((int16_t)peek(0x0F20) > 0x0080) poke(0x0F20, 0x0080);
			pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) | 0x04));
			poke(T.r[2], (uint16_t)(peek(T.r[2]) + 4));
			poke(T.r[3], (uint16_t)(peek(T.r[3]) + 4));
			T.r[0] = 0x0017;
		}
		pokeb(peek((uint16_t)(fp + 2)), (uint8_t)T.r[0]);
		pokeb((uint16_t)(0x0370 + T.r[4]), (uint8_t)T.r[0]);
		pokeb((uint16_t)(0x0490 + T.r[1]), (uint8_t)(peekb((uint16_t)(0x0490 + T.r[1])) | 0x18));
		pokeb((uint16_t)(0x0490 + T.r[4]), (uint8_t)(peekb((uint16_t)(0x0490 + T.r[4])) | 0x18));
		T.r[0] = peek(fp);
		poke((uint16_t)(0x0430 + T.r[0]), 0x0010);
		poke((uint16_t)(0x0430 + T.r[5]), 0x000F);
		break;
	}
	case 11:
		T.r[0] = call(0xA8D2, {T.r[1]});
		T.r[3] = T.r[0];
		T.r[4] = (uint16_t)(T.r[3] << 1);
		poke((uint16_t)(0x03B0 + T.r[4]), (uint16_t)(peek((uint16_t)(0x03B0 + T.r[4])) + 2));
		T.r[0] = (uint16_t)(T.r[1] << 1);
		poke((uint16_t)(0x03B0 + T.r[0]), (uint16_t)(peek((uint16_t)(0x03B0 + T.r[0])) + 2));
		T.r[5] = 0x0018;
		pokeb(peek((uint16_t)(fp + 2)), 0x18);
		pokeb((uint16_t)(0x0370 + T.r[3]), 0x18);
		pokeb((uint16_t)(0x0490 + T.r[1]), (uint8_t)(peekb((uint16_t)(0x0490 + T.r[1])) | 0x18));
		pokeb((uint16_t)(0x0490 + T.r[3]), (uint8_t)(peekb((uint16_t)(0x0490 + T.r[3])) | 0x18));
		poke((uint16_t)(0x0430 + T.r[0]), 0x0010);
		poke((uint16_t)(0x0430 + T.r[4]), 0x000F);
		break;
	case 12:
		if (peek(0x06B2) != peek(0x06B0)) break;
		T.r[0] = abs16(peek(0x01B4));
		if ((int16_t)T.r[0] < 0x0180) break;
		T.r[0] = 2;
		pokeb((uint16_t)(0x0790 + T.r[1]), 2);
		pokeb((uint16_t)(0x0770 + T.r[1]), 2);
		bank_b(0x2C00);
		T.r[0] = (uint16_t)(sext8(peekb(0x025D)) << 1);
		poke(0x05B2, (uint16_t)(peek(0x05B2) + peek((uint16_t)(0x7CA4 + T.r[0]))));
		if (peek((uint16_t)(fp + 4)) & 1) call(0x9F8A, {});   // skater_jump
		call(0x8E78, {0x007B});                        // queue_sound
		break;
	default:
		// types 0 and 1 do nothing; 13 is the next table's first word (0, as type 0) and
		// higher kinds of the table are not object types
		T.r[0] = 0;
		break;
	}
	pokeb(0x0334, (uint8_t)T.r[1]);
	T.r[6] = fl::add(T.r[6], 6);
}

}

LOGIC_TABLE(flow,
	c_routine("F", 0x90A6, "divide", divide, 0, 1604),
	c_routine("F", 0x90BA, "park_level", park_level, 0, 305),
	asm_routine("F", 0x8AB6, "draw_string_large", draw_string_large, 1680),
	asm_routine("F", 0x8B04, "draw_string_small", draw_string_small, 2361),
	c_routine("F", 0x9F8A, "skater_jump", skater_jump, 0, 222),
	asm_routine("F", 0xAFF0, "process_sprite_requests", process_sprite_requests, 57),
	c_routine("F", 0xA974, "queue_message", queue_message, 0, 620),
	c_routine("F", 0xA8FE, "flush_text_list", flush_text_list, 0, 745),
	c_routine("F", 0x91A6, "skater_velocity", skater_velocity, 0, 1012),
	c_routine("F", 0x843E, "place_skater", place_skater, 0, 2091),
	c_routine("F", 0xA45A, "skater_phase10", skater_phase10, 0, 1236),
	c_routine("F", 0xACE6, "unlink_sprites", unlink_sprites, 0, 1162),
	c_routine("F", 0xAE02, "link_sprites", link_sprites, 0, 3340),
	asm_routine("F", 0xAF32, "sort_sprite_list", sort_sprite_list, 661),
	asm_routine("F", 0x944C, "cell_terrain_codes", cell_terrain_codes, 1498),
	asm_routine("F", 0x934C, "probe_ground", probe_ground, 2886),
	c_routine("F", 0x9B82, "skater_motion_params", skater_motion_params, 0, 857),
	c_routine("F", 0x9FB4, "skater_take_off", skater_take_off, 0, 1520),
	asm_routine("F", 0x9236, "update_push", update_push, 615),
	irq_routine("F", 0x899C, "irq_vblank_play", irq_vblank_play, 1741),
	c_routine("F", 0xA2F0, "skater_land", skater_land, 0, 1569),
	c_routine("F", 0xA090, "skater_touch_down", skater_touch_down, 0, 1783),
	asm_routine("F", 0x94DC, "edge_mode", edge_mode, 457),
	asm_routine("F", 0x979A, "check_wall", check_wall, 415),
	c_routine("F", 0xA9FC, "run_text_script", run_text_script, 0, 1791),
	asm_routine("F", 0xA510, "object_contact", object_contact, 556),
)

}
