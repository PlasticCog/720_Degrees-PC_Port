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

// F:9E96  skater_bounce(int dir): new direction dir (0x3F: the opposite one), speed 0x160,
// motion values for that direction (F:9B82 with terrain 0x0327 cleared, F:91A6, then again
// with the terrain), no upward z velocity for object 1; state 0x031E = 2 for 6 (0x031C)
uint16_t skater_bounce(const Args &a)
{
	const uint16_t r1 = T.r[1];
	if (a.u(0) == 0x003F)
	{
		T.r[0] = sext8(peekb(0x031D));
		T.r[1] = (uint16_t)(0x0008 ^ T.r[0]);
		pokeb(0x031D, (uint8_t)T.r[1]);
	}
	else
		pokeb(0x031D, a.b(0));
	poke(0x01B4, 0x0160);
	T.r[1] = sext8(peekb(0x0327));
	pokeb(0x0327, 0);
	call(0x9B82, {});
	call(0x91A6, {});
	pokeb(0x0327, (uint8_t)T.r[1]);
	call(0x9B82, {});
	if ((int16_t)peek(0x05B2) > 0) poke(0x05B2, 0);
	pokeb(0x031E, 2);
	pokeb(0x031C, 6);
	T.r[1] = r1;
	return T.r[0];
}

// flip the direction bit 3 of the heading 0x031B (through R1, R0)
void flip_heading()
{
	T.r[0] = sext8(peekb(0x031B));
	T.r[1] = (uint16_t)(0x0008 ^ T.r[0]);
	pokeb(0x031B, (uint8_t)T.r[1]);
}

// F:9EEE  skater_deflect(int dir): heading 0x031B = dir (0x3F: the current direction, with the
// speed reversed to 0x160 the other way); otherwise the speed keeps its sign and is at least
// 0x160 (a negative speed takes the opposite heading).  On a ramp (0x01EA == 2) F:A2F0 lands
// the skater; mode 0x031F == 1 turns a negative speed around.  State 0x031E = 1 for 8.
uint16_t skater_deflect(const Args &a)
{
	const uint16_t r1 = T.r[1], r2 = T.r[2];
	const uint16_t dir = a.u(0);
	const uint16_t speed = peek(0x01B4);
	T.r[1] = speed;
	bool positive;                                     // which bound: 0x0160 or 0xFEA0
	bool force;                                        // set the bound unconditionally
	if (dir == 0x003F)
	{
		pokeb(0x031B, peekb(0x031D));
		positive = (int16_t)speed < 0;
		force = true;
	}
	else
	{
		pokeb(0x031B, (uint8_t)dir);
		positive = (int16_t)speed >= 0;
		force = false;
		if (!positive)
		{
			T.r[0] = sext8((uint8_t)dir);
			T.r[2] = (uint16_t)(0x0008 ^ T.r[0]);
			pokeb(0x031B, (uint8_t)T.r[2]);
		}
	}
	if (positive)
	{
		if (force || (int16_t)speed < 0x0160) poke(0x01B4, 0x0160);
	}
	else
	{
		if (force || (int16_t)speed > (int16_t)0xFEA0) poke(0x01B4, 0xFEA0);
	}
	if (peekb(0x01EA) == 2)
	{
		if ((int16_t)peek(0x01B4) < 0) flip_heading();
		call(0xA2F0, {});
	}
	if (peekb(0x031F) == 1 && (int16_t)peek(0x01B4) < 0)
	{
		flip_heading();
		poke(0x01B4, (uint16_t)-peek(0x01B4));
	}
	pokeb(0x031E, 1);
	pokeb(0x031C, 8);
	T.r[1] = r1;
	T.r[2] = r2;
	return T.r[0];
}

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

// F:A45A  skater_fall(int side, int dir): animation from the tables at 0x4EAC (side >= 0) or
// 0x4EBC (bank A 0, bank B 0x0400) for the direction dir; kick state reset, phase 0x01EB and
// 0x0330 = 10, ramp state 2, frame counter saved (B3:7AD2), sound 0x57; banks restored
uint16_t skater_fall(const Args &a)
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

}

LOGIC_TABLE(flow,
	c_routine("F", 0x90A6, "divide", divide, 0, 1667),
	c_routine("F", 0x90BA, "park_level", park_level, 0, 297),
	asm_routine("F", 0x8AB6, "draw_string_large", draw_string_large, 17114),
	asm_routine("F", 0x8B04, "draw_string_small", draw_string_small, 7835),
	c_routine("F", 0x9E96, "skater_bounce", skater_bounce, 0, 7566),
	c_routine("F", 0x9EEE, "skater_deflect", skater_deflect, 0, 553),
	c_routine("F", 0x9F8A, "skater_jump", skater_jump, 0, 12765),
	asm_routine("F", 0xAFF0, "process_sprite_requests", process_sprite_requests, 61),
	c_routine("F", 0xA974, "queue_message", queue_message, 0, 633),
	c_routine("F", 0xA8FE, "flush_text_list", flush_text_list, 0, 4857),
	c_routine("F", 0x91A6, "skater_velocity", skater_velocity, 0, 4949),
	c_routine("F", 0x843E, "place_skater", place_skater, 0, 2280),
	c_routine("F", 0xA45A, "skater_fall", skater_fall, 0, 1767),
	c_routine("F", 0xACE6, "unlink_sprites", unlink_sprites, 0, 1199),
	c_routine("F", 0xAE02, "link_sprites", link_sprites, 0, 5504),
	asm_routine("F", 0xAF32, "sort_sprite_list", sort_sprite_list, 2992),
	asm_routine("F", 0x944C, "cell_terrain_codes", cell_terrain_codes, 1480),
	asm_routine("F", 0x934C, "probe_ground", probe_ground, 3288),
	c_routine("F", 0x9B82, "skater_motion_params", skater_motion_params, 0, 840),
	c_routine("F", 0x9FB4, "skater_take_off", skater_take_off, 0, 11334),
	asm_routine("F", 0x9236, "update_push", update_push, 617),
	irq_routine("F", 0x899C, "irq_vblank_play", irq_vblank_play, 2898),
)

}
