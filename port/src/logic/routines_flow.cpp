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

// Cycle accounting.  Every routine charges exactly what its original's instructions cost
// on the path taken, where the original spends it: the part before each call is charged
// before the call (the callee then runs at the original's time and charges itself), the
// part common to all paths at the end (the table's cost).
void cyc(int n) { T.icount -= n; }
// the register-save helpers F138 / F144 / F152 / F162 (R1-R2 / R1-R3 / R1-R4 / R1-R5): the
// JSR R1,@#helper with the saves before the body, the restores and RTS after the body's RTS
constexpr int SAVE12_IN = 135, SAVE12_OUT = 69;
constexpr int SAVE13_IN = 159, SAVE13_OUT = 87;
constexpr int SAVE14_IN = 183, SAVE14_OUT = 105;
constexpr int SAVE15_IN = 207, SAVE15_OUT = 111;

// Interrupts.  The original can be interrupted after any instruction; what an interrupt
// handler sees and changes in memory only depends on which memory accesses come before it.
// A routine therefore syncs (logic::tick) at the address of each memory access, charging
// the cycles of the instructions since the previous one, and at each callee's entry (the
// JSR included): an interrupt due meanwhile is taken at the same place as in the original.
struct Clock {
	int p = 0;                                         // cycles not charged yet
	void add(int n) { p += n; }
	void at(uint16_t pc) { tick(pc, p); p = 0; }       // before the memory access at pc
	void enter(uint16_t callee, int jsr = 30) { tick(callee, p + jsr); p = 0; }
	void flush() { cyc(p); p = 0; }
	// the final RTS: the original pops the return address and syncs there; the slot is put
	// back for the caller of the native routine, which pops it
	void leave()
	{
		const uint16_t sp = T.r[6], ret = rw(sp);
		T.r[6] = (uint16_t)(sp + 2);
		at(ret);
		T.r[6] = sp;
		ww(sp, ret);
	}
};

// F:9034  divide_ext(int mode, -, int d, int n): n / d, truncated and signed, by shift and
// subtract; with mode != 0 the remainder instead (sign of n).  0 when n or d is 0.
// R1-R5 preserved (F162).
// F:90A6  divide(int d, int n) = F:9034(0, ...);  F:90B0  modulo(int d, int n) = F:9034 with
// a non-zero mode (the PC pushed by MOV PC,-(SP))
uint16_t divide_ext(const Args &a)
{
	const uint16_t mode = a.u(0);
	uint16_t d = a.u(2), n = a.u(3);
	int c = 27 + 12;
	if (n == 0)
	{
		cyc(c + 12);
		return 0;
	}
	c += 27 + 12;
	if (d == 0)
	{
		cyc(c + 12);
		return 0;
	}
	c += 48;
	const bool nneg = (int16_t)n < 0, dneg = (int16_t)d < 0;
	if (nneg)
	{
		n = (uint16_t)-n;
		c += 24;
	}
	c += 24;
	if (dneg)
	{
		d = (uint16_t)-d;
		c += 24;
	}
	c += 12;
	int shift = 0;
	for (;;)
	{
		c += 24;                                       // CMP R0,R1; BGE
		if ((int16_t)d >= (int16_t)n) break;
		c += 30;                                       // BIT #0x4000,R0; BNE
		if (d & 0x4000) break;
		d = (uint16_t)(d << 1);
		shift++;
		c += 36;
	}
	c += 12;
	uint16_t q = 0;
	for (;;)
	{
		q = (uint16_t)(q << 1);
		c += 36;                                       // ASL R2; CMP R0,R1; BGT
		if ((int16_t)d <= (int16_t)n)
		{
			q++;
			n = (uint16_t)(n - d);
			c += 36;
			if (n == 0) break;
		}
		c += 24;                                       // DEC R3; BMI
		if (--shift < 0) break;
		d = (uint16_t)((int16_t)d >> 1);
		c += 24;
	}
	c += 39;
	if (mode)
	{
		cyc(c + 24 + (nneg ? 12 : 0) + 12);
		return nneg ? (uint16_t)-n : n;
	}
	c += 24;
	if (nneg != dneg)
	{
		q = (uint16_t)-q;
		c += 12;
	}
	for (;;)
	{
		c += 24;
		if (--shift < 0) break;
		q = (uint16_t)(q << 1);
		c += 24;
	}
	cyc(c + 12);
	return q;
}

uint16_t divide(const Args &)
{
	cyc(24 + 30);
	return call(0x9034, {0});
}

uint16_t modulo(const Args &)
{
	cyc(24 + 30);
	return call(0x9034, {0x90B2});
}

// F:90BA  park_level(): 0x0F2D when positive; otherwise derived from the table at 0xFCAC
// for the park 0x027A and limited to 9 (10 when 0x0F31 >= 12)
uint16_t park_level(const Args &)
{
	const uint16_t r1 = sext8(peekb(0x0F2D));
	if ((int16_t)r1 > 0)
	{
		cyc(24 + 12 + 12 + 12 + 12);
		return r1;
	}
	int c = 24 + 12 + 189;
	uint16_t v = peekb((uint16_t)(0xFCAC + sext8(peekb(0x027A))));
	v = (uint16_t)(((uint16_t)(v - r1 - 1) & 3) - r1);
	uint16_t limit;
	if (peekb(0x0F31) < 0x0C)
	{
		limit = 9;
		c += 12 + 18 + 12;
	}
	else
	{
		limit = 10;
		c += 18;
	}
	c += 24;
	if ((int16_t)v > (int16_t)limit)
	{
		v = limit;
		c += limit == 9 ? 54 : 42;
	}
	cyc(c + 12);
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
	cyc(SAVE13_IN + 36 + 33 + 39 + 30);
	call(0x8068, {});                                  // select_video_bank0 (R0 kept)
	cyc(147);
	T.r[3] = T.r[0];
	uint16_t p = peek((uint16_t)(((T.r[0] << 1) & 0x00FF) + peek(0x0184)));
	T.r[2] = (uint16_t)(peek(p) + 0x2000);
	p = (uint16_t)(p + 2);
	poke(0x0186, T.r[2]);
	for (;;)
	{
		T.r[1] = p;
		const bool erase = (T.r[3] & 0x0080) != 0;
		T.r[0] = erase ? 0x0023 : sext8(peekb(p));
		cyc(42 + (erase ? 18 : 0) + 30);
		call(put_char, {});
		cyc(30);
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
	const bool kick4 = peekb(0x01EB) == 4;
	if (kick4) poke(0x01B4, peek(0x02E8));
	cyc(30 + 12 + (kick4 ? 39 : 0) + 30);
	call(0xE238, {});
	pokeb(0x07B1, 3);
	pokeb(0x01EB, 8);
	pokeb(0x0330, 4);
	cyc(33 + 33 + 33 + 30);
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
	cyc(36);
	if (!n) return;
	cyc(18);
	for (T.r[2] = 4;; )
	{
		const uint8_t o = peekb((uint16_t)(0x0989 + T.r[2]));
		T.r[1] = sext8(o);
		fl::movb(o);
		cyc(39);
		if (o)
		{
			pokeb((uint16_t)(0x04D0 + T.r[1]), peekb((uint16_t)(0x098D + T.r[2])));
			T.r[3] = fl::add(0x0490, T.r[1]);
			const uint8_t f = peekb(T.r[3]);
			cyc(111);
			if (f & 0x02)
			{
				cyc(78);
				call(0xACE6, {T.r[1], 0});
				cyc(75);
				call(0xAE02, {T.r[1], 0});
				cyc(18);
				fl::add((uint16_t)(T.r[6] - 6), 6);
			}
			const uint8_t g = (uint8_t)(peekb(T.r[3]) & ~0x20);
			pokeb(T.r[3], g);
			f_nzv(nzb(g & 0x40));
			cyc(63);
			if (!(g & 0x40))
			{
				pokeb(0x0992, fl::decb(peekb(0x0992)));
				pokeb((uint16_t)(0x0989 + T.r[2]), 0);
				fl::clr();
				cyc(57);
			}
		}
		cyc(18);
		if (!--T.r[2]) break;
	}
}

// F:A974  queue_message(int bit, int a, int b): with interrupts masked, the entry count
// 0x0358 goes up (at most 3) and the entry gets the words b (0x0362) and a (0x035A); bit is
// shifted into the mask 0x036D and a 1 into 0x036E unless the text table 0x0184 is 0x5AC8.
// Returns the shifted 0x036E (before that bit).
uint16_t queue_message(const Args &a)
{
	int c = 159;
	mtps(0xE0);
	const uint8_t n = (uint8_t)(peekb(0x0358) + 1);
	pokeb(0x0358, n);
	if ((int8_t)n > 3)
	{
		pokeb(0x0358, 3);
		c += 33;
	}
	const uint16_t i = (uint16_t)(sext8(peekb(0x0358)) << 1);
	poke((uint16_t)(0x0362 + i), a.u(2));
	poke((uint16_t)(0x035A + i), a.u(1));
	mtps(0);
	uint16_t r0 = (uint16_t)(peekb(0x036D) << 1);
	pokeb(0x036D, (uint8_t)r0);
	pokeb(0x036D, (uint8_t)(peekb(0x036D) | a.b(0)));
	r0 = (uint16_t)(peekb(0x036E) << 1);
	pokeb(0x036E, (uint8_t)r0);
	c += 402;
	if (peek(0x0184) != 0x5AC8)
	{
		pokeb(0x036E, (uint8_t)(peekb(0x036E) | 1));
		c += 33;
	}
	cyc(c);
	return r0;
}

// F:A8FE  flush_text_list(): the entries 0..0x0336 of the list at 0x0348 (bank B 0x2800) are
// drawn: a pointer to a value from 0x4000 on goes to F:EAD4, others to A7:4D30; the list is
// emptied (0x0336 = 0xFF) and the banks restored.  R1-R5 preserved (F162).
uint16_t flush_text_list(const Args &)
{
	const uint16_t saved[5] = { T.r[1], T.r[2], T.r[3], T.r[4], T.r[5] };
	cyc(SAVE15_IN + 96);
	T.r[4] = peek(0x01D2);
	T.r[5] = peek(0x01D4);
	T.r[3] = sext8(peekb(0x0336));
	for (T.r[2] = 0;; T.r[2]++)
	{
		cyc(24);
		if ((int16_t)T.r[2] > (int16_t)T.r[3]) break;
		bank_b(0x2800);
		T.r[1] = (uint16_t)(T.r[2] << 1);
		const uint16_t p = peek((uint16_t)(0x0348 + T.r[1]));
		const uint16_t v = peek(p);
		if ((int16_t)v >= 0x4000)
		{
			cyc(147 + 105);
			call(0xEAD4, {1, v});
			cyc(12 + 36);
		}
		else
		{
			bank_a(0x3800);
			cyc(147 + 171);
			call(0x4D30, {1, p});
			cyc(36);
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
	Clock k;
	// the register-save helper's frame: R2, R3, R1 and its return address
	push(T.r[2]);
	push(T.r[3]);
	push(T.r[1]);
	push(0xF150);
	k.add(SAVE13_IN);
	k.at(0x91AA); const uint16_t a = peek(0x01D2); k.add(24);
	k.at(0x91AE); const uint16_t b = peek(0x01D4); k.add(24);
	k.at(0x91B2); push(peek(0x01B4)); k.add(36);
	k.at(0x91B6); push(peek(0x02E2)); k.add(36);
	k.enter(0x80FA);
	call_from(0x91BE, 0x80FA, {});                     // mul32
	pokeb(0x07F0, peekb(0x0205)); k.add(39);
	k.at(0x91C4); poke(0x04F0, peek(0x0206)); k.add(39);
	k.at(0x91CA); poke(T.r[6], peek(0x01B4)); k.add(33);
	k.at(0x91CE); push(peek(0x02E4)); k.add(36);
	k.enter(0x80FA);
	call_from(0x91D6, 0x80FA, {});
	pokeb(0x07F4, peekb(0x0205)); k.add(39);
	k.at(0x91DC); poke(0x0570, peek(0x0206)); k.add(39);
	k.at(0x91E2); const uint8_t carrier = peekb(0x0EA0); k.add(24 + 12);
	if (carrier)
	{
		const uint16_t i = (uint16_t)((sext8(carrier) & 0x003F) << 1);
		k.add(18 + 12);
		k.at(0x91EE); poke(0x04F0, (uint16_t)(peek(0x04F0) + peek((uint16_t)(0x04F0 + i)))); k.add(42);
		k.at(0x91F4); poke(0x0570, (uint16_t)(peek(0x0570) + peek((uint16_t)(0x0570 + i)))); k.add(42);
	}
	k.at(0x91FA);
	uint16_t s = peek(0x01B4);
	k.add(24 + 12);
	if ((int16_t)s < 0)
	{
		s = (uint16_t)-s;
		k.add(12);
	}
	s = (uint16_t)(s << 1);
	k.add(12 * 5);
	const uint8_t cls = (uint8_t)(s >> 8);
	k.at(0x920C); pokeb(0x0331, cls); k.add(27 + 18 + 12);
	if ((int8_t)cls > 15)
	{
		k.at(0x9216); pokeb(0x0331, 15); k.add(33);
	}
	k.at(0x921C); poke(0x01D2, a); k.add(27);
	k.at(0x9220); poke(0x01D4, b); k.add(27);
	k.at(0x9224); poke(0x1400, peek(0x01D2)); k.add(39);
	k.at(0x922A); poke(0x1402, peek(0x01D4)); k.add(39);
	T.r[6] = (uint16_t)(T.r[6] + 6 + 2);               // ADD #6,SP; RTS into the helper
	T.r[1] = pop();
	T.r[3] = pop();
	T.r[2] = pop();
	k.add(18 + 21 + SAVE13_OUT);
	k.leave();
	return s;
}

// F:843E  place_skater(int z, int y, int x): objects 0-3 at (x, y, z) without velocity, the
// skater's motion state reset (speed, ramp, terrain, gravity, ...), F:E238
uint16_t place_skater(const Args &a)
{
	Clock k;
	k.add(24 + 18);                                    // MOV R1,-(SP); MOV #3,R1
	for (int i = 3; i >= 0; i--)
	{
		const uint16_t w = (uint16_t)(2 * i);
		k.add(12 + 12);
		k.at(0x8448); poke((uint16_t)(0x06F0 + w), a.u(2)); k.add(45);
		k.at(0x844E); poke((uint16_t)(0x0730 + w), a.u(1)); k.add(45);
		k.at(0x8454); poke((uint16_t)(0x06B0 + w), a.u(0)); k.add(45);
		k.at(0x845A); poke((uint16_t)(0x04F0 + w), 0); k.add(30);
		k.at(0x845E); poke((uint16_t)(0x0570 + w), 0); k.add(30);
		k.at(0x8462); poke((uint16_t)(0x05B0 + w), 0); k.add(30);
		k.at(0x8466); pokeb((uint16_t)(0x07F0 + i), 0); k.add(30);
		k.at(0x846A); pokeb((uint16_t)(0x07F4 + i), 0); k.add(30);
		k.at(0x846E); pokeb((uint16_t)(0x07F8 + i), 0); k.add(30);
		k.add(12 + 12);                                // DEC R1, BGE
	}
	k.at(0x8476); poke(0x01B4, 0); k.add(27);
	k.at(0x847A); pokeb(0x01EA, 1); k.add(33);
	k.at(0x8480); pokeb(0x031E, 0); k.add(27);
	k.at(0x8484); pokeb(0x0328, 0xFE); k.add(33);
	k.at(0x848A); pokeb(0x0320, 1); k.add(33);
	k.at(0x8490); pokeb(0x031F, 0); k.add(27);
	k.at(0x8494); pokeb(0x0323, 0); k.add(27);
	k.at(0x8498); pokeb(0x0EA0, 0); k.add(27);
	k.at(0x849C); pokeb(0x031B, 0xFF); k.add(33);
	k.at(0x84A2); pokeb(0x032F, 0); k.add(27);
	k.at(0x84A6); poke(0x02FC, 0); k.add(27);
	k.at(0x84AA); poke(0x02FE, 0); k.add(27);
	k.at(0x84AE); poke(0x02EE, 0); k.add(27);
	k.at(0x84B2); pokeb(0x0325, 0xFF); k.add(33);
	k.at(0x84B8); poke(0x01B0, 0); k.add(27);
	k.at(0x84BC); poke(0x01B2, 0); k.add(27);
	k.at(0x84C0); poke(0x0ED2, 0x3800); k.add(33);
	k.at(0x84C6); poke(0x0ED4, 0xA800); k.add(33);
	T.r[0] = 0;
	k.enter(0xE238);
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
	cyc(SAVE12_IN + 264 + ((int16_t)a.u(0) >= 0 ? 114 : 102) + 57);
	call(0xE238, {});
	pokeb(0x01EB, 0x0A);
	pokeb(0x0330, 0x0A);
	pokeb(0x07B1, 0);
	pokeb(0x0791, 3);
	pokeb(0x0771, 3);
	pokeb(0x01EA, 2);
	bank_a(0x3800);
	bank_b(0x3C00);
	cyc(372);
	call(0x7AD2, {});
	cyc(60);
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
	int c = 105;
	if ((int16_t)prev >= 0)
	{
		c += 135;
		pokeb((uint16_t)(0x0808 + prev), (uint8_t)next);
		if ((int16_t)next >= 0)
		{
			chain_sprites(prev, next);
			pokeb((uint16_t)(0x0848 + next), (uint8_t)prev);
			c += 120 + 30;
		}
		else
		{
			const uint16_t at = (uint16_t)(0x3806 + peek((uint16_t)(0x0908 + (prev << 1))));
			poke(at, (uint16_t)((peek(at) & ~0x07FE) | 0x03F8));
			c += 87;
		}
	}
	else
	{
		c += 81;
		pokeb((uint16_t)(0x0988 + list), (uint8_t)next);
		if ((int16_t)next >= 0)
		{
			pokeb((uint16_t)(0x0848 + next), (uint8_t)prev);
			c += 30;
		}
	}
	const uint16_t flags = (uint16_t)(0x0490 + (obj & 0x1F));
	pokeb(flags, (uint8_t)(peekb(flags) & ~peekb((uint16_t)(0xFD7C + list))));
	c += 168;
	uint16_t slot = peek((uint16_t)(0x0888 + (obj << 1)));
	const uint16_t last = peek((uint16_t)(0x0908 + (obj << 1)));
	for (;;)
	{
		const uint16_t next_slot = peek((uint16_t)(0x3806 + slot)) & 0x07F8;
		poke((uint16_t)(0x3806 + slot), 0);
		c += 99;
		if (slot == last) break;
		slot = next_slot;
		c += 24;
	}
	cyc(c);
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
	int c = SAVE15_IN + 360;
	const uint8_t bits = (uint8_t)(peekb((uint16_t)(0x0490 + i)) | peekb((uint16_t)(0xFD7C + list)));
	pokeb((uint16_t)(0x0490 + i), peekb((uint16_t)(0xFD7E + list)));
	pokeb((uint16_t)(0x0490 + i), (uint8_t)(peekb((uint16_t)(0x0490 + i)) | bits));
	pokeb((uint16_t)(0x04B0 + i), (uint8_t)(peekb((uint16_t)(0x04B0 + i)) & ~0x04));
	uint16_t first = 0, last = i, slot = 8;
	uint16_t n = peekb((uint16_t)(0x04D0 + i));
	if ((int16_t)n > 0)
		do
		{
			c += 39;
			while (peek((uint16_t)(0x3806 + slot)))
			{
				slot = (uint16_t)(slot + 8);
				c += 30 + 39;
			}
			c += 24;
			if (!first)
			{
				first = slot;
				c += 24;
			}
			else
			{
				poke((uint16_t)(0x3806 + last), slot);
				c += 30;
			}
			poke((uint16_t)(0x3800 + slot), 0);
			poke((uint16_t)(0x3C00 + slot), 0);
			poke((uint16_t)(0x3802 + slot), 0);
			last = slot;
			slot = (uint16_t)(slot + 8);
			c += 138;
		} while (--n);
	poke((uint16_t)(0x0888 + (obj << 1)), first);
	poke((uint16_t)(0x0908 + (obj << 1)), last);
	poke((uint16_t)(0x3806 + last), 0x03F9);
	c += 216;
	uint16_t prev = 0xFFFF;
	uint16_t next = sext8(peekb((uint16_t)(0x0988 + list)));
	uint16_t r0;
	for (;;)
	{
		c += 12;
		if ((int16_t)next < 0)
		{
			// at the end of the list
			c += 24;
			if ((int16_t)prev >= 0)
			{
				pokeb((uint16_t)(0x0808 + prev), (uint8_t)obj);
				r0 = chain_sprites(prev, obj);
				c += 219;
			}
			else
			{
				r0 = list;
				pokeb((uint16_t)(0x0988 + list), (uint8_t)obj);
				c += 57;
			}
			pokeb((uint16_t)(0x0848 + obj), (uint8_t)prev);
			pokeb((uint16_t)(0x0808 + obj), (uint8_t)next);
			cyc(c + 60);
			break;
		}
		T.r[0] = obj;
		T.r[1] = next;
		T.r[2] = obj;
		T.r[3] = prev;
		cyc(c + 42);
		call(0xAD70, {});                              // F:AD70 R0 in front of R1?
		c = 24;
		if (T.r[0] & 1)
		{
			// before next
			c += 24;
			if ((int16_t)prev >= 0)
			{
				pokeb((uint16_t)(0x0808 + prev), (uint8_t)obj);
				chain_sprites(prev, obj);
				c += 219;
			}
			else
			{
				pokeb((uint16_t)(0x0988 + list), (uint8_t)obj);
				c += 57;
			}
			pokeb((uint16_t)(0x0848 + next), (uint8_t)obj);
			pokeb((uint16_t)(0x0808 + obj), (uint8_t)next);
			r0 = chain_sprites(obj, next);
			pokeb((uint16_t)(0x0848 + obj), (uint8_t)prev);
			cyc(c + 267);
			break;
		}
		prev = next;
		next = sext8(peekb((uint16_t)(0x0808 + next)));
		c += 51;
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
	int c = 84;
	T.r[0] = sext8(peekb(0x0993));
	T.r[3] = sext8(peekb((uint16_t)(0x0988 + T.r[0])));
	if (!(T.r[3] & 0x8000))
	{
		T.r[1] = sext8(peekb((uint16_t)(0x0808 + T.r[3])));
		c += 27;
		for (;;)
		{
			c += 12;
			if (T.r[1] & 0x8000) break;
			T.r[0] = T.r[3];
			cyc(c + 42);
			call(0xAD70, {});                          // F:AD70 R0 in front of R1?
			T.r[0] = ror(T.r[0]);
			c = 24;
			if (!fC())
			{
				// R1 moves in front of R3
				c += 39;
				T.r[2] = sext8(peekb((uint16_t)(0x0848 + T.r[3])));
				if (!(T.r[2] & 0x8000))
				{
					pokeb((uint16_t)(0x0808 + T.r[2]), (uint8_t)T.r[1]);
					T.r[4] = peek((uint16_t)(0x0908 + (T.r[2] << 1)));
					T.r[5] = (uint16_t)(T.r[1] << 1);
					T.r[0] = chain_sprites(T.r[2], T.r[1]);
					c += 219;
				}
				else
				{
					T.r[0] = sext8(peekb(0x0993));
					pokeb((uint16_t)(0x0988 + T.r[0]), (uint8_t)T.r[1]);
					c += 54;
				}
				pokeb((uint16_t)(0x0848 + T.r[1]), (uint8_t)T.r[2]);
				T.r[2] = sext8(peekb((uint16_t)(0x0808 + T.r[1])));
				poke(T.r[6], (uint16_t)(T.r[3] << 1));
				T.r[4] = peek(T.r[6]);
				T.r[0] = peek((uint16_t)(0x0908 + T.r[4]));
				T.r[5] = (uint16_t)(0x3806 + T.r[0]);
				c += 198;
				if (!(T.r[2] & 0x8000))
				{
					pokeb((uint16_t)(0x0848 + T.r[2]), (uint8_t)T.r[3]);
					T.r[4] = (uint16_t)(T.r[2] << 1);
					T.r[0] = (uint16_t)((peek(T.r[5]) & ~0x07FE) | peek((uint16_t)(0x0888 + T.r[4])));
					poke(T.r[5], T.r[0]);
					c += 150;
				}
				else
				{
					T.r[4] = (uint16_t)((peek(T.r[5]) & ~0x07FE) | 0x03F8);
					poke(T.r[5], T.r[4]);
					c += 75;
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
				c += 273;
			}
			T.r[3] = T.r[1];
			T.r[1] = sext8(peekb((uint16_t)(0x0808 + T.r[1])));
			c += 51;
		}
	}
	cyc(c);
	fl::tst(peek(T.r[6]));
	T.r[6] = (uint16_t)(T.r[6] + 2);
}

// F:944C (assembly): the probed cells 1-3 (0x0302-0x0306) become tile numbers (low 10 bits
// plus the tile bank base 0x01D6, or 0x01D8 for bit 10); 0x032A / 0x032B = the attribute byte
// (0x6000) of cell 2; then each cell holds its terrain code: the byte at 0x4000 + tile, or
// with mode 0x032F == 2 a negative attribute byte without bit 7
void cell_terrain_codes()
{
	int c = 162 + 183;
	bank_a(0x0800);
	bank_b(0x0C00);
	for (T.r[3] = 3; T.r[3]; T.r[3]--)
	{
		T.r[0] = (uint16_t)(T.r[3] << 1);
		T.r[1] = peek((uint16_t)(0x0300 + T.r[0]));
		T.r[2] = (uint16_t)((T.r[1] & 0x03FF) + peek((T.r[1] & 0x0400) ? 0x01D8 : 0x01D6));
		poke((uint16_t)(0x0300 + T.r[0]), T.r[2]);
		c += 81 + ((T.r[1] & 0x0400) ? 66 : 54) + 48;
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
		c += 102 + 18;
		bool attr = false;
		if (mode == 2)
		{
			const uint8_t v = peekb((uint16_t)(0x6000 + T.r[1]));
			fl::tstb(v);
			attr = (v & 0x80) != 0;
			c += 39;
		}
		if (attr)
		{
			pokeb(T.r[0], peekb((uint16_t)(0x6000 + T.r[1])));
			const uint16_t v = peek(T.r[0]) & 0x007F;
			poke(T.r[0], v);
			fl::mov(v);
			c += 63;
		}
		else
		{
			T.r[3] = sext8(peekb((uint16_t)(0x4000 + T.r[1])));
			poke(T.r[0], T.r[3]);
			fl::mov(T.r[3]);
			c += 60;
		}
	}
	cyc(c);
}

// F:934C (assembly): probe the playfield under the skater: from (x + 0x0A00 + 8 * dx,
// y + z + 0x0300 + 8 * (dy + dz)) (direction factors 0x02E3/0x02E5/0x02E7) three cells are
// read stepping back along the direction; per cell 0x0300 / 0x0306 / 0x030C / 0x0312 get the
// tile word, the row and column offsets and the video window address; 0x032E collects the
// high bits of the tiles; the last tile with bit 14 is remembered in 0x021C / 0x0214 / 0x021A
void probe_ground()
{
	Clock k;
	T.r[0] = 0x0800; k.add(18);
	k.at(0x9350); poke(0x01D2, T.r[0]); k.add(27);
	k.at(0x9354); poke(0x1400, T.r[0]); k.add(27);
	T.r[0] = 0x0C00; k.add(18);
	k.at(0x935C); poke(0x01D4, T.r[0]); k.add(27);
	k.at(0x9360); poke(0x1402, T.r[0]); k.add(27);
	k.at(0x9364); poke(0x0220, peek(0x06F0)); k.add(39);
	k.at(0x936A); poke(0x0220, (uint16_t)(peek(0x0220) + 0x0A00)); k.add(33);
	k.at(0x9370); T.r[0] = peek(0x0730); k.add(24);
	k.at(0x9374); T.r[0] = (uint16_t)(T.r[0] + peek(0x06B0)); k.add(24);
	k.at(0x9378); poke(0x021E, T.r[0]); k.add(27);
	k.at(0x937C); poke(0x021E, (uint16_t)(peek(0x021E) + 0x0300)); k.add(33);
	k.at(0x9382); T.r[3] = (uint16_t)(sext8(peekb(0x02E3)) << 3); k.add(24 + 36);
	k.at(0x938C); T.r[0] = sext8(peekb(0x02E5)); k.add(24);
	k.at(0x9390); T.r[1] = sext8(peekb(0x02E7)); k.add(24);
	T.r[0] = (uint16_t)((T.r[0] + T.r[1]) << 3); k.add(12 + 36);
	T.r[5] = T.r[3]; k.add(12);
	k.at(0x939E); poke(0x0212, T.r[0]); k.add(27);
	k.at(0x93A2); poke(0x0220, (uint16_t)(peek(0x0220) + T.r[3])); k.add(27);
	k.at(0x93A6); poke(0x021E, (uint16_t)(peek(0x021E) + T.r[0])); k.add(27);
	k.at(0x93AA); pokeb(0x032E, 0); k.add(27);
	k.at(0x93AE); pokeb(0x0329, 0xFD); k.add(33);
	T.r[4] = 3; k.add(18);
	do
	{
		k.at(0x93B8); T.r[1] = peek(0x0220) & 0xFF00; k.add(24 + 18);
		k.at(0x93C0); T.r[0] = peek(0x021E); k.add(24);
		T.r[0] = (uint16_t)((int16_t)T.r[0] >> 1) & 0x3F00; k.add(12 + 18);
		T.r[2] = (uint16_t)(0x3F00 - T.r[0]); k.add(18 + 12);
		k.at(0x93D0); poke(0x0218, T.r[2]); k.add(27);
		T.r[0] = T.r[2]; k.add(12);
		k.enter(0x8078);
		call_from(0x93DA, 0x8078, {});                 // select_video_bank
		T.r[2] &= 0x1F00;
		T.r[1] &= 0xFE00;
		T.r[3] = (uint16_t)((T.r[1] >> 8) + T.r[2]);
		k.add(18 + 18 + 12 + 12 + 12 + 12);
		k.at(0x93EA); T.r[3] = peek((uint16_t)(0x2000 + T.r[3])); k.add(27 + 18 + 12);
		if (T.r[3] & 0x4000)
		{
			k.at(0x93F4); poke(0x021C, peek(0x021E)); k.add(39);
			k.at(0x93FA); poke(0x0214, T.r[1]); k.add(27);
			k.at(0x93FE); poke(0x021A, peek(0x0218)); k.add(39);
		}
		T.r[0] = sext8((uint8_t)(T.r[3] >> 8)); k.add(12 * 4);
		k.at(0x940C); pokeb(0x032E, (uint8_t)(peekb(0x032E) | (uint8_t)T.r[0])); k.add(27);
		T.r[0] = (uint16_t)(T.r[4] << 1); k.add(12 + 12);
		k.at(0x9414); poke((uint16_t)(0x0300 + T.r[0]), T.r[3]); k.add(30);
		k.at(0x9418); poke((uint16_t)(0x0306 + T.r[0]), T.r[2]); k.add(30);
		k.at(0x941C); poke((uint16_t)(0x030C + T.r[0]), T.r[1]); k.add(30);
		k.at(0x9420); poke((uint16_t)(0x0312 + T.r[0]), peek(0x0218)); k.add(42);
		k.at(0x9426); poke(0x0220, (uint16_t)(peek(0x0220) - T.r[5])); k.add(27);
		k.at(0x942A); poke(0x021E, fl::sub(peek(0x021E), peek(0x0212))); k.add(39 + 18);
	} while (--T.r[4]);
	k.at(0x9432); poke(0x021E, peek(0x030A)); k.add(39);
	k.at(0x9438); poke(0x0220, peek(0x0310)); k.add(39);
	k.at(0x943E); poke(0x0218, peek(0x0316)); k.add(39);
	k.at(0x9444);
	const uint8_t v = (uint8_t)(peekb(0x032E) & ~0x3F);
	pokeb(0x032E, v);
	f_nzv(nzb(v));
	k.add(33 + 21);
	k.leave();
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
	int c = 174 + 186;
	uint16_t idx = sext8(peekb(0x031D));
	const uint16_t t = sext8(peekb(0x0327));
	if (t & 7)
	{
		c += 42;
		if ((t & 7) != 7)
		{
			idx = (uint16_t)(((t & 7) << 4) + peek((uint16_t)(0x79E8 + (uint16_t)(((int16_t)t >> 3) << 1))) + idx);
			c += 201;
		}
		else
		{
			idx = (uint16_t)(((t & 0x38) << 1) + idx + 0x0220);
			c += 84;
		}
	}
	const uint16_t w = (uint16_t)(idx << 1);
	poke(0x02E2, peek((uint16_t)(0x6048 + w)));
	poke(0x02E4, peek((uint16_t)(0x6588 + w)));
	poke(0x02E6, peek((uint16_t)(0x6AC8 + w)));
	if (peekb(0x01EA))
	{
		c += 96;
		const uint16_t g = sext8(peekb((uint16_t)(0x7668 + idx)));
		poke(0x02EE, g);
		if (peekb(0x0F59) == 3)
		{
			c += 24;
			if ((int16_t)g < 0)
			{
				c += 36;
				if ((int16_t)peek(0x01B4) > 0)
				{
					c += 87;
					const uint16_t k = (uint16_t)(sext8(peekb(0x025C)) << 1);
					if ((int16_t)peek(0x01B4) < (int16_t)peek((uint16_t)(0x7C68 + k)))
					{
						poke(0x02EE, (uint16_t)((int16_t)peek(0x02EE) >> 1));
						c += 39;
					}
				}
			}
		}
	}
	else if ((t & 7) != 7)
	{
		c += 42 + 105 + 54;
		pokeb(0x02EA, peekb((uint16_t)(0x7008 + idx)));
		pokeb(0x02EB, 0);
		if (peek(0x02EA))
		{
			poke(0x02EA, (uint16_t)(peek(0x02EA) + 1));
			c += 27;
		}
		poke(0x02EC, peek((uint16_t)(0x7228 + w)));
	}
	else
	{
		c += 42 + 54;
		poke(0x02EA, 0);
		poke(0x02EC, 0);
	}
	poke(0x01D4, saved_b);
	poke(0x1402, saved_b);
	cyc(c);
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
	cyc(SAVE13_IN + 261);
	if (peekb(0x0F59) != 4)
	{
		cyc(30);
		call(0x7A9E, {});                              // snapshot_skater
	}
	cyc(36);
	if ((int16_t)peek(0x01B4) < 0)
	{
		T.r[0] = sext8(peekb(0x031D));
		T.r[3] = (uint16_t)(0x0008 ^ T.r[0]);
		pokeb(0x031D, (uint8_t)T.r[3]);
		poke(0x01B4, (uint16_t)-peek(0x01B4));
		cyc(108);
	}
	poke(0x02E8, peek(0x01B4));
	cyc(69);
	call(0x9B82, {});
	cyc(30);
	call(0x91A6, {});
	cyc(36);
	if (peekb(0x0EA0))
	{
		cyc(30);
		call(0xD742, {});
	}
	cyc(102);
	call(0x80FA, {peek(0x02EA), peek(0x01B4)});        // mul32
	pokeb(0x01B5, peekb(0x0206));
	pokeb(0x01B4, peekb(0x0205));
	cyc(177);
	call(0x80FA, {peek(0x02EC), peek(0x01B4)});
	pokeb(0x07F8, peekb(0x0205));
	poke(0x05B0, peek(0x0206));
	cyc(114);
	if (peekb(0x0EA0))
	{
		cyc(30);
		call(0xD6BE, {});
	}
	pokeb(0x0323, 0);
	poke(0x02F6, peek(0x01C6));
	poke(0x02F6, (uint16_t)(peek(0x02F6) << 1));
	poke(0x02F4, 0);
	poke(0x02F2, 0);
	poke(0x02EE, t11rt::g_patch_gravity);             // patch point F:A068 (default 12)
	cyc(237);
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
	int c = 174 + 123 + 60 + 42 + 42;
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
		c += 240;
	}
	T.r[0] = (uint16_t)(sext8(peekb(0x025F)) << 1);
	T.r[1] = peek((uint16_t)(0x7C88 + T.r[0]));
	T.r[3] = peek(0x02FC);
	T.r[5] = 0;
	if ((int16_t)T.r[3] > (int16_t)T.r[1])
	{
		T.r[5] = 1;
		T.r[0] = 0;
		c += 36;
	}
	else
	{
		T.r[0] = 0x0040;
		c += 18;
	}
	T.r[4] = peekb(0x0333);
	if (T.r[4])
	{
		c += 60;
		T.r[2] = (uint16_t)(T.r[0] + peek(0x01B4));
		if ((int16_t)T.r[3] > (int16_t)T.r[2])
		{
			c += 42;
			bool clear = true;
			if (peekb(0x0F59) == 3)
			{
				c += 42;
				if ((int16_t)peek(0x0ED0) <= 2)
				{
					c += 42;
					clear = (int16_t)peek(0x01B4) >= (int16_t)0xFE80;
				}
			}
			if (clear)
			{
				pokeb(0x0320, 0);
				c += 27;
			}
		}
	}
	bool carry = false;                                // from CMP R2,#4
	if (peekb(0x0330) == 4)
	{
		pokeb(0x0320, 1);
		c += 33;
	}
	else if (peekb(0x031E) != 0)
	{
		pokeb(0x0320, 1);
		c += 36 + 33;
	}
	else
	{
		c += 36 + 72;
		T.r[2] = sext8(peekb(0x0327)) & 7;
		if (T.r[2] < 4)
			carry = true;
		else
		{
			pokeb(0x0320, 1);
			c += 33;
		}
	}
	if (peek(0x01C4) & 0x000F)
	{
		f_nzvc(nzw(peek(0x01C4) & 0x000F) | (carry ? CF : 0));
		cyc(c);
		return;
	}
	c += 69 + 48 + 36 + 42 + 24;
	poke(0x02FE, (uint16_t)((int16_t)peek(0x02FE) >> 1));
	if ((int16_t)peek(0x02FE) < 0x0180)
	{
		poke(0x02FE, 0x0180);
		c += 33;
	}
	T.r[0] = peek(0x01B4);
	if ((int16_t)T.r[0] > (int16_t)T.r[1])
	{
		T.r[0] = T.r[1];
		c += 12;
	}
	if ((int16_t)T.r[0] < (int16_t)peek(0x02FE))
	{
		T.r[0] = peek(0x02FE);
		c += 24;
	}
	T.r[1] = (uint16_t)(T.r[1] + 0x0080);
	if ((int16_t)T.r[0] > (int16_t)T.r[1])
	{
		T.r[0] = T.r[1];
		c += 12;
	}
	if ((int16_t)T.r[3] < (int16_t)T.r[0])
	{
		T.r[0] = fl::add(asr((uint16_t)(T.r[0] - T.r[3])), peek(0x02FC));
		poke(0x02FC, T.r[0]);
		fl::mov(T.r[0]);
		cyc(c + 75);
		return;
	}
	bool down = true;
	c += 24;
	if (T.r[4])
	{
		fl::tst(T.r[4]);
		T.r[5] = ror(T.r[5]);
		down = fC();
		c += 24;
	}
	if (down)
		poke(0x02FC, fl::sub(peek(0x02FC), 0x0018));
	else
		poke(0x02FC, fl::add(peek(0x02FC), 0x0006));
	cyc(c + 33);
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
	cyc(318);
	call(0xDBE2, {});
	cyc(30);
	call(0xDDEC, {});
	bank_a(0x1800);
	bank_b(0x1C00);
	cyc(174);
	call(0x7D82, {});                                  // blink_screen7_palette
	cyc(30);
	call(0x917C, {});                                  // feed_watchdog
	pokeb(0x0192, (uint8_t)(peekb(0x0192) + 1));
	poke(0x15E0, 0);
	poke(0x15C0, 0);
	pokeb(0x0191, 0);
	poke(0x01C4, (uint16_t)(peek(0x01C4) + 1));
	int c = 177 + 102;
	if (peek(0x0F18) != 0xFFFF)
	{
		poke(0x0F18, (uint16_t)(peek(0x0F18) - 1));
		c += 27;
	}
	bank_a(0x1800);
	cyc(c);
	call(0x5D86, {});                                  // step_sound_level
	bank_a(0x3800);
	bank_b(0x3C00);
	cyc(174);
	call(0x7A3A, {});
	cyc(30);
	call(0x80A4, {});                                  // read_input_edges
	cyc(30);
	call(0x8036, {});                                  // load_scroll_registers
	c = 36 + 36;
	if ((int16_t)peek(0x0F22) > 0)
	{
		c += 36;
		if (!peek(0x0180))
		{
			poke(0x0F22, (uint16_t)(peek(0x0F22) - 1));
			c += 27;
		}
	}
	if (!peekb(0x01A4))
	{
		bank_a(0x3800);
		bank_b(0x3C00);
		cyc(c + 174);
		call(0x4314, {});                              // send_queued_sound
		c = 0;
	}
	const uint16_t sp = T.r[6];
	poke(0x0186, rw(sp));
	poke(0x01D2, rw((uint16_t)(sp + 4)));
	poke(0x1400, peek(0x01D2));
	poke(0x01D4, rw((uint16_t)(sp + 2)));
	poke(0x1402, peek(0x01D4));
	poke(0x01D0, rw((uint16_t)(sp + 6)));
	cyc(c + 267);
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
	cyc(SAVE13_IN + 150);
	call(0xE238, {});                                  // reset_skater_state
	int c = 168 + 150;
	pokeb(0x031F, 1);
	T.r[0] = sext8(peekb(0x025E));
	pokeb(0x02FA, peekb((uint16_t)(0x7CAC + T.r[0])));
	pokeb(0x02FB, 0);
	if (peekb(0x01EA) == 2)
	{
		pokeb(0x01EA, 1);
		c += 33;
	}
	pokeb(0x0325, 0xFF);
	poke(0x02FC, 0);
	poke(0x02FE, 0);
	poke(0x01DC, 0);
	if (!peekb(0x01EA))
	{
		c += 69;
		poke(0x01B4, 0x0160);
		if ((int16_t)peek(0x05B2) > 0)
		{
			poke(0x05B2, 0);
			c += 27;
		}
		cyc(c + 30);
		call(0x9B82, {});
		cyc(30);
		call(0x91A6, {});
		c = 0;
	}
	c += 36;
	if ((int16_t)peek(0x01B4) < 0)
	{
		poke(0x01B4, (uint16_t)-peek(0x01B4));
		T.r[0] = sext8(peekb(0x031D));
		T.r[1] = (uint16_t)(0x0008 ^ T.r[0]);
		pokeb(0x031D, (uint8_t)T.r[1]);
		c += 108;
	}
	c += 99;
	T.r[1] = peek(0x01B4);
	T.r[0] = (uint16_t)(sext8(peekb(0x025D)) << 1);
	const uint16_t limit = peek((uint16_t)(0x7C90 + T.r[0]));
	uint16_t sound;
	if ((int16_t)T.r[1] >= (int16_t)limit) c += 42;
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
		cyc(c + 348);
		T.r[0] = call(0x9152, {}) & 3;                 // next_random
		sound = peekb((uint16_t)(0xFCF6 + T.r[0]));
		c = 78 + 12;
	}
	else
	{
		c += 123;
		T.r[0] = (uint16_t)((sext8(peekb(0x01F8)) << 1) + 0x4428);
		poke(0x03B4, T.r[0]);
		bool done = false;
		if (peekb(0x0F59) == 3)
		{
			c += 72;
			T.r[0] = sext8(peekb(0x0327)) & 7;
			if (T.r[0] >= 5)
			{
				c += 72;
				T.r[0] = (uint16_t)-(uint16_t)(peek(0x05B2) << 1);
				if ((int16_t)T.r[1] < (int16_t)T.r[0])
				{
					poke(0x01B4, T.r[0]);
					done = true;
					c += 87;
				}
			}
		}
		if (!done)
		{
			c += 30;
			if ((int16_t)T.r[1] < 0x0180)
			{
				poke(0x01B4, 0x0180);
				c += 45;
			}
			else
			{
				c += 30;
				if ((int16_t)T.r[1] > 0x0280)
				{
					poke(0x01B4, 0x0280);
					c += 33;
				}
			}
		}
		sound = 0x0054;
		c += 30;
	}
	cyc(c + 30);
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
	int c = SAVE14_IN + 114;
	if (peekb(0x0F59) == 4)
	{
		c += 36;
		if (!peekb(0x01F2))
		{
			bank_a(0x2400);
			cyc(c + 102);
			T.r[0] = call(0x90BA, {});                 // park_level
			T.r[3] = peekb((uint16_t)(0x4579 + T.r[0]));
			T.r[1] = peekb(0x0FE0);
			T.r[3] = (uint16_t)(T.r[3] + T.r[1]);
			T.r[0] = (uint16_t)(T.r[3] << 1);
			const uint16_t w3 = peek((uint16_t)(0x7E50 + T.r[0]));
			const uint16_t w2 = peek((uint16_t)(0x7E8A + T.r[0]));
			const uint16_t w1 = peek((uint16_t)(0x7EC4 + T.r[0]));
			cyc(288);
			call(0xE1FA, {0x0078, w1, w2, w3});        // fill_slots
			c = 18;
		}
	}
	bank_a(0x1800);
	cyc(c + 102);
	call(0x51BA, {});                                  // copy_object0_position
	c = 36 + 141;
	T.r[0] = sext8(peekb(0x0EA0));
	if (T.r[0])
	{
		T.r[0] = (uint16_t)(T.r[0] << 1);
		poke(0x04F0, (uint16_t)(peek(0x04F0) - peek((uint16_t)(0x04F0 + T.r[0]))));
		poke(0x0570, (uint16_t)(peek(0x0570) - peek((uint16_t)(0x0570 + T.r[0]))));
		c += 96;
	}
	T.r[3] = sext8(peekb(0x0327));
	pokeb(0x0327, a.b(0));
	pokeb(0x01EA, 1);
	if (peekb(0x0330) == 0x40)
	{
		cyc(c + 30);
		call(0xE238, {});                              // reset_skater_state
		c = 0;
	}
	c += 72 + 42;
	T.r[0] = sext8(peekb(0x0327)) & 7;
	if (T.r[0] >= 5)
	{
		T.r[0] = (uint16_t)((int16_t)sext8(peekb(0x0327)) >> 3) & 3;
		pokeb(0x031D, peekb((uint16_t)(0xFCD0 + T.r[0])));
		pokeb(0x0325, 0x7F);
		c += 153;
	}
	if (peekb(0x0323) == 1)
	{
		cyc(c + 30);
		call(0xA2F0, {});                              // skater_land
		bank_b(0x2800);
		cyc(132);
		call(0x7E54, {1});
		pokeb(0x0323, 0);
		c = 39 + 12 + 18;
	}
	else if (peekb(0x031F) == 3)
		c += 42 + 12;
	else if (peekb(0x031F) == 1)
		c += 42 + 42 + 12;
	else
	{
		T.r[0] = sext8(peekb(0x01F8));
		cyc(c + 42 + 42 + 78);
		call(0xA2BE, {T.r[0]});                        // direction_bit
		c = 42;
		if (!T.r[0])
		{
			T.r[1] = sext8(peekb(0x031D));
			pokeb(0x031D, peekb(0x01F8));
			cyc(c + 93);
			call(0x9B82, {});                          // skater_motion_params
			bank_a(0x3800);
			cyc(102);
			call(0x410C, {});
			c = 36;
			if (peekb(0x031E))
			{
				cyc(c + 54);
				call(0x9EEE, {T.r[1]});                // skater_deflect
				c = 18;
			}
			c += 42;
			if (peekb(0x0F59) != 3)
			{
				c += 66;
				T.r[1] = peek(0x02E8);
				bool faster = false;
				if (peekb(0x0327) & 7)
				{
					T.r[0] = abs16(peek(0x01B4));
					faster = (int16_t)T.r[1] > (int16_t)T.r[0];
					c += 36 + ((int16_t)peek(0x01B4) < 0 ? 12 : 0) + 24;
				}
				if (!faster)
				{
					T.r[0] = abs16(peek(0x01B4));
					T.r[2] = (uint16_t)((((sext8(peekb(0x0327)) & 7) << 2) + sext8(peekb(0x025C))) << 1);
					faster = (int16_t)T.r[0] > (int16_t)peek((uint16_t)(0x7BE8 + T.r[2]));
					c += 36 + ((int16_t)peek(0x01B4) < 0 ? 12 : 0) + 153;
				}
				if (faster)
				{
					c += 36;
					if ((int16_t)peek(0x01B4) <= 0)
					{
						T.r[1] = (uint16_t)-T.r[1];
						c += 12;
					}
					poke(0x01B4, T.r[1]);
					c += 27 + 12;
				}
			}
		}
		else
		{
			cyc(c + 30);
			call(0xE6F0, {});                          // object1_screen_x
			cyc(30);
			call(0x9B82, {});                          // skater_motion_params
			T.r[2] = peek(0x05B2);
			bank_a(0x3800);
			cyc(126);
			call(0x410C, {});
			T.r[0] = (uint16_t)(sext8(peekb(0x025D)) << 1);
			c = 87;
			bool land = false;
			if ((int16_t)peek(0x01B4) >= (int16_t)peek((uint16_t)(0x7C90 + T.r[0])))
			{
				c += 39;
				if ((int16_t)T.r[2] < (int16_t)peek((uint16_t)(0x7C98 + T.r[0])))
				{
					c += 42;
					land = peekb(0x0F59) != 3;
				}
			}
			if (land)
			{
				cyc(c + 30);
				call(0xA2F0, {});                      // skater_land
				bank_b(0x2800);
				cyc(132);
				call(0x7E54, {2});
				c = 12 + 18;
			}
			else
			{
				c += 72;
				T.r[2] = sext8(peekb(0x01F8));
				T.r[0] = sext8(peekb(0x031D));
				T.r[2] = (uint16_t)(T.r[2] - T.r[0]);
				if (T.r[2] & 0x8000)
				{
					poke(0x0222, 0xFFFF);
					T.r[2] = (uint16_t)-T.r[2];
					c += 57;
				}
				else
				{
					poke(0x0222, 1);
					c += 33;
				}
				c += 30;
				if ((int16_t)T.r[2] > 8)
				{
					poke(0x0222, (uint16_t)-peek(0x0222));
					c += 27;
				}
				T.r[0] = sext8(peekb(0x01F8));
				cyc(c + 114);
				call(0xA45A, {peek(0x0222), T.r[0]});  // skater_phase10
				c = 18 + 18;
			}
		}
	}
	pokeb(0x0327, (uint8_t)T.r[3]);
	c += 69;
	if (peekb(0x031F) != 1)
	{
		bank_b(0x2800);
		cyc(c + 126);
		call(0x7E54, {0});
		c = 18;
	}
	cyc(c);
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
	const uint16_t cell = peek(0x0304);
	int c = 225 + ((cell & 0x0400) ? 54 : 42) + 99;
	T.r[2] = tile_number(cell);
	T.r[0] = peekb(0x032E);
	const uint8_t mode = peekb(0x032F);
	T.r[1] = (uint16_t)(sext8(mode) << 1);
	if (mode == 0)
	{
		c += 30;
		if (T.r[0] & 0x40)
		{
			pokeb(0x032F, 2);
			f_nzvc(0);
			cyc(c + 33);
			return;
		}
		c += 30;
		fl::cmp(T.r[0], 0x0080);
		if (T.r[0] == 0x0080)
		{
			pokeb(0x032F, 1);
			f_nzvc(0);
			c += 33;
		}
		cyc(c);
		return;
	}
	if (mode == 1)
	{
		c += 24;
		if (!T.r[0])
		{
			pokeb(0x032F, 0);
			fl::clr();
			cyc(c + 18 + 27);
			return;
		}
		c += 30;
		if (!(T.r[0] & 0x40))
		{
			f_nzvc(ZF);
			cyc(c);
			return;
		}
		pokeb(0x0329, 0xFF);
		c += 75;
		if (peekb(0x0328) == 0xFF)
		{
			f_nzvc(ZF);
			cyc(c);
			return;
		}
		c += 36;
		if (peekb(0x01EA))
		{
			c += 18 + 36;
			T.r[0] = peek(0x01B4);
			if (!(T.r[0] & 0x8000))
			{
				c += 42;
				if (!(peek(0x0306) & 0x4000))
				{
					fl::tst(T.r[0]);
					cyc(c + 24);
					return;
				}
			}
			else
			{
				c += 42;
				if (!(peek(0x0302) & 0x4000))
				{
					f_nzvc(ZF);
					cyc(c);
					return;
				}
			}
			cyc(c + 60);
			call(0x9EEE, {0x003F});                    // skater_deflect
			poke(0x0222, 1);
			pokeb(0x0328, peekb(0x0329));
			fl::tst(0x003F);
			cyc(45 + 51 + 18);
			return;
		}
		poke(0x0220, peek(0x0214));
		T.r[0] = (uint16_t)((int16_t)(uint16_t)(peek(0x06B2) - peek(0x06B0) + peek(0x021C)) >> 1) & 0x3F00;
		T.r[1] = (uint16_t)(0x3E00 - T.r[0]);
		T.r[0] = T.r[1];
		cyc(c + 213);
		call(0x8078, {});                              // select_video_bank
		poke(0x0218, T.r[1]);
		T.r[1] &= 0x1F00;
		poke(0x021E, T.r[1]);
		T.r[0] = peekb(0x0221);
		T.r[1] = (uint16_t)(T.r[1] + T.r[0]);
		const uint16_t tile = peek((uint16_t)(0x2000 + T.r[1]));
		c = 177 + ((tile & 0x0400) ? 54 : 42) + 66;
		T.r[1] = tile_number(tile);
		T.r[3] = sext8(peekb((uint16_t)(0x6000 + T.r[2])));
		bool solid = (peekb((uint16_t)(0x6000 + T.r[1])) & 0x80) != 0;
		if (!solid)
		{
			c += 30;
			if (T.r[3] & 0x40)
			{
				T.r[0] = (uint16_t)(7 & ~(T.r[3] & 0xFF));
				solid = T.r[0] == 0;
				c += 42;
			}
		}
		if (solid)
		{
			c += 30;
			if (T.r[3] & 0x40)
			{
				bank_a(0x1800);
				bank_b(0x1C00);
				cyc(c + 198);
				call(0x7B3E, {T.r[3]});
				bank_a(0x0800);
				bank_b(0x0C00);
				c = 162;
			}
			T.r[0] = sext8(peekb(0x031D));
			T.r[1] = (uint16_t)(0x0008 ^ T.r[0]);
			pokeb(0x031D, (uint8_t)T.r[1]);
			poke(0x01B4, 0x0240);
			T.r[0] = sext8(peekb(0x0327));
			poke(0x021A, T.r[0]);
			pokeb(0x0327, 0);
			cyc(c + 222);
			call(0x9B82, {});                          // skater_motion_params
			cyc(30);
			call(0x91A6, {});                          // skater_velocity
			pokeb(0x0327, peekb(0x021A));
			cyc(69);
			call(0x9B82, {});
			c = 36 + 150;
			if ((int16_t)peek(0x05B2) > 0)
			{
				poke(0x05B2, 0);
				c += 27;
			}
			poke(0x0222, 1);
			pokeb(0x031E, 2);
			pokeb(0x031C, 6);
			pokeb(0x0328, peekb(0x0329));
		}
		else
		{
			pokeb(0x032F, 2);
			c += 57;
			if (T.r[3] & 0x8000)
			{
				poke(0x0212, 0x0100);
				poke(0x06B0, peek(0x06B2));
				T.r[0] = peek(0x021E);
				T.r[1] = peek(0x0220);
				cyc(c + 180);
				call(0x9C5E, {0x7FFF});                // trace_cells
				T.r[2] = T.r[0];
				c = 54;
				if (peek(0x0222) != 0x7FFF)
				{
					poke(0x0304, T.r[2]);
					poke(0x06B0, peek(0x0222));
					poke(0x0222, 0xFFFF);
					c += 111;
				}
				else
				{
					poke(0x06B0, (uint16_t)(peek(0x06B0) - 0x0200));
					poke(0x0222, 1);
					c += 66;
				}
				pokeb(0x031E, 2);
				pokeb(0x031C, 6);
				pokeb(0x0328, peekb(0x0329));
				c += 123;
			}
		}
		cyc(c + 30);
		call(0x9B82, {});
		const uint16_t speed = peek(0x01B4), factor = peek(0x02EC);
		cyc(102);
		call(0x80FA, {factor, speed});                 // mul32
		pokeb(0x07F8, peekb(0x0205));
		poke(0x05B0, peek(0x0206));
		fl::cmp(factor, speed);                        // CMP (SP)+,(SP)+
		cyc(102);
		return;
	}
	if (mode == 2)
	{
		c += 24;
		if (!T.r[0])
		{
			pokeb(0x032F, 0);
			fl::clr();
			cyc(c + 27);
			return;
		}
		c += 39;
		const uint8_t attr = peekb((uint16_t)(0x6000 + T.r[2]));
		if (attr & 0x80)
		{
			fl::tstb(attr);
			cyc(c);
			return;
		}
		c += 24;
		if (!(T.r[0] & 0x80))
		{
			fl::tstb((uint8_t)T.r[0]);
			cyc(c);
			return;
		}
		pokeb(0x0329, 0xFF);
		c += 75;
		if (peekb(0x0328) == 0xFF)
		{
			f_nzvc(ZF);
			cyc(c);
			return;
		}
		pokeb(0x032F, 1);
		poke(0x0212, 0x0100);
		T.r[0] = peek(0x021E);
		T.r[1] = peek(0x0220);
		cyc(c + 174);
		call(0x9C5E, {0x7FFE});                        // trace_cells
		T.r[2] = T.r[0];
		T.r[1] = peek(0x0222);
		c = 66;
		bool up = false;
		if (T.r[1] != 0x7FFF)
		{
			c += 66;
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
				c += 153 + 51;
			}
		}
		if (!up)
		{
			pokeb(0x032F, 2);
			poke(0x0222, 1);
			c += 66;
		}
		fl::tst(0x7FFE);                               // TST (SP)+
		cyc(c + 18);
		return;
	}
	cyc(c);
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
	int c = 180;
	const uint16_t pending = peek(0x0222);
	if (pending)
	{
		fl::tst(pending);
		cyc(c);
		return;
	}
	c += 87;
	poke(0x021C, 0xFFFF);
	if (!(7 & ~peek(0x0304)))
	{
		poke(0x021C, 1);
		c += 45;
	}
	else
	{
		c += 36 + 54;
		if ((int16_t)peek(0x01B4) > 0)
		{
			if (!(7 & ~peek(0x0306)))
			{
				poke(0x021C, 2);
				c += 45;
			}
		}
		else if (!(7 & ~peek(0x0302)))
		{
			poke(0x021C, 0);
			c += 27;
		}
	}
	c += 36;
	T.r[0] = peek(0x021C);
	if (T.r[0] & 0x8000)
	{
		f_nzvc(NF);
		cyc(c);
		return;
	}
	T.r[0] = (uint16_t)(T.r[0] << 1);
	const uint8_t code = peekb((uint16_t)(0x0302 + T.r[0]));
	pokeb(0x0329, code);
	c += 105;
	if (code == peekb(0x0328))
	{
		f_nzvc(ZF);
		cyc(c);
		return;
	}
	T.r[3] = sext8(code);
	T.r[1] = 0;
	c += 69;
	if (T.r[3] == 0x0037)
	{
		T.r[2] = 0xFFFF;
		c += 18;
	}
	else
	{
		T.r[1] = 1;
		cyc(c + 66);
		T.r[2] = call(0x9E52, {T.r[3]});              // slope_table_value
		c = 42;
	}
	c += 24;
	if (!(T.r[2] & 0x8000))
	{
		T.r[0] = (uint16_t)(peek(0x021C) << 1);
		poke(0x021E, peek((uint16_t)(0x0308 + T.r[0])));
		poke(0x0220, peek((uint16_t)(0x030E + T.r[0])));
		poke(0x0218, peek((uint16_t)(0x0314 + T.r[0])));
		T.r[0] = peek((uint16_t)(0x0314 + T.r[0]));
		cyc(c + 219);
		call(0x8078, {});                              // select_video_bank
		c = 36;
		if (peekb(0x01EA))
		{
			c += 72;
			T.r[0] = sext8(peekb(0x0327)) & 7;
			if (T.r[0] >= 5)
			{
				c += 96;
				T.r[1] = sext8(peekb(0x0327)) & 0x0038;
				T.r[0] = T.r[3] & 0x0038;
				if (T.r[1] == T.r[0])
				{
					c += 54;
					T.r[0] = (uint16_t)(7 & ~peek(0x0304));
					if (T.r[0])
					{
						f_nzvc(nzw(T.r[0]));
						cyc(c);
						return;
					}
					pokeb(0x0327, (uint8_t)T.r[3]);
					cyc(c + 57);
					call(0x9FB4, {});                  // skater_take_off
					pokeb(0x031E, 2);
					pokeb(0x031C, 2);
					pokeb(0x0328, peekb(0x0329));
					bank_a(0x1800);
					cyc(207);
					call(0x5264, {});
					bank_a(0x0800);
					bank_b(0x2C00);
					fl::mov(0x2C00);
					cyc(144);
					return;
				}
			}
			cyc(c + 54);
			call(0x9EEE, {T.r[2]});                    // skater_deflect
			pokeb(0x0328, peekb(0x0329));
			fl::tst(T.r[2]);
			cyc(12 + 39 + 18 + 18);
			return;
		}
		c += 30;
		uint16_t slot = T.r[3];                        // the word left on the stack
		if (T.r[3] & 0x40)
		{
			bank_a(0x1800);
			bank_b(0x1C00);
			cyc(c + 198);
			call(0x7B3E, {T.r[3]});
			bank_a(0x0800);
			bank_b(0x0C00);
			T.r[3] = 0x8000;
			c = 174;
		}
		else
		{
			T.r[3] = (uint16_t)(peek(0x06B2) + peek(0x05B2));
			poke(0x0212, 0xFF00);
			slot = T.r[3];
			T.r[0] = peek(0x021E);
			T.r[1] = peek(0x0220);
			cyc(c + 183);
			call(0x9C5E, {T.r[3]});                    // trace_cells
			T.r[3] = T.r[0];
			c = 12;
		}
		c += 30;
		if (T.r[3] == 0x8000)
		{
			slot = T.r[2];
			cyc(c + 51);
			call(0x9E96, {T.r[2]});                    // skater_bounce
			pokeb(0x0328, peekb(0x0329));
			c = 39 + 18;
		}
		else
		{
			c += 42;
			if (peek(0x021C) == 1)
			{
				poke(0x0304, T.r[3]);
				poke(0x06B0, peek(0x0222));
				pokeb(0x031E, 2);
				pokeb(0x031C, 6);
				pokeb(0x0328, peekb(0x0329));
				c += 144 + 39;
			}
			c += 18;
		}
		fl::tst(slot);                                 // TST (SP)+
		cyc(c + 18);
		return;
	}
	c += 18 + 24;
	fl::tst(T.r[2]);
	T.r[1] = ror(T.r[1]);
	if (fC())
	{
		bank_a(0x1800);
		cyc(c + 138);
		T.r[2] = call(0x51EE, {peek(0x0304)});
		bank_a(0x0800);
		c = 150 + 24;
		if ((int16_t)peek(0x01B4) < 0)
		{
			T.r[2] = (uint16_t)-T.r[2];
			c += 12;
		}
	}
	else
	{
		c += 42;
		if (peek(0x021C) == 1)
		{
			T.r[2] = 1;
			c += 30 + 12;
		}
		else
		{
			T.r[2] = 0;
			c += 12 + 24;
		}
	}
	if ((int16_t)T.r[2] <= 0)
	{
		fl::tst(T.r[2]);
		cyc(c);
		return;
	}
	poke(0x0212, 0x0100);
	poke(0x0218, peek(0x0316));
	T.r[0] = peek(0x030A);
	T.r[1] = peek(0x0310);
	cyc(c + 180);
	call(0x9C5E, {0x7FFD});                            // trace_cells
	poke(0x0304, T.r[0]);
	c = 105 + 42;
	T.r[0] = (uint16_t)(peek(0x06B2) - peek(0x0222));
	if ((int16_t)T.r[0] >= 0x0200)
	{
		poke(0x06B0, peek(0x0222));
		c += 51;
	}
	else
	{
		poke(0x06B0, peek(0x06B2));
		c += 39;
	}
	if (peekb(0x0F59) == 4)
	{
		c += 78;
		T.r[0] = (uint16_t)(peek(0x06B2) - peek(0x06B0));
		if ((int16_t)T.r[0] > 0x2800)
		{
			bank_a(0x3800);
			bank_b(0x3C00);
			cyc(c + 174);
			call(0x7A9E, {});                          // snapshot_skater
			c = 0;
		}
	}
	pokeb(0x0328, peekb(0x0329));
	pokeb(0x031C, 2);
	pokeb(0x031E, 2);
	fl::tst(0x7FFD);                                   // TST (SP)+
	cyc(c + 105 + 18);
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
		cyc(SAVE15_IN + 207 + 84);
		call(0xEAD4, {});
		T.r[2] = (uint16_t)(T.r[2] + 2);
		cyc(18 + 12);
	}
	else
	{
		bank_a(0x3800);
		push(T.r[2]);
		push(0);
		cyc(SAVE15_IN + 207 + 150);
		call(0x4D30, {});
		T.r[2] = (uint16_t)(T.r[2] + 4);
		cyc(18);
	}
	// level class bits of the current rating entry (0x0262 table, by 0x0F31 / 0x0F2B)
	auto rating_class = []() -> uint16_t {
		uint16_t i = peekb(0x0F31);
		if (peekb(0x0F2B) != 2) i = (uint16_t)(i - 1);
		return (uint16_t)((peekb((uint16_t)(0x0262 + i)) >> 6) & 3);
	};
	constexpr int RATING_CLASS = 42 + 48 + 135;        // CMPB 0x0F2B .. BIC #0xFFFC,R0
	for (;;)
	{
		poke(0x020C, peek(T.r[2]));
		T.r[2] = (uint16_t)(T.r[2] + 2);
		if (peek(0x020C) == 0xFFFF)
		{
			cyc(75 + 18);
			break;
		}
		T.r[3] = peekb(0x020C);
		if (!(T.r[3] & 0x80))
		{
			const uint8_t how = peekb(0x020D);
			T.r[0] = (uint16_t)(how << 1);
			const int c = 75 + 48 + 75;                // fetch, kind test, jump by the high byte
			switch (how)
			{
			case 0:
				T.r[0] = T.r[3];
				cyc(c + 24 + 30);
				call(0x8AB6, {});                      // draw_string_large
				break;
			case 1:
				T.r[0] = T.r[3];
				cyc(c + 12 + 30);
				call(0x8B04, {});                      // draw_string_small
				break;
			case 2:
				T.r[0] = (uint16_t)(rating_class() + T.r[3] - 1);
				cyc(c + RATING_CLASS + 36 + 30);
				call(0x8AB6, {});
				break;
			case 3:
				cyc(c + 30);
				T.r[0] = (uint16_t)(call(0x5326, {}) + T.r[3]);
				cyc(24 + 30);
				call(0x8AB6, {});
				break;
			case 4:
				T.r[0] = (uint16_t)(peekb(0x02B8) + T.r[3]);
				cyc(c + 102);
				call(0x8AB6, {});
				break;
			case 5:
				T.r[0] = (uint16_t)(peekb(0x0261) + T.r[3]);
				cyc(c + 102);
				call(0x8AB6, {});
				break;
			case 6:
				cyc(c + 30);
				T.r[0] = call(0x9152, {});             // next_random
				T.r[1] = T.r[0] & 1;
				T.r[4] = (uint16_t)(T.r[1] << 1);
				T.r[0] = (uint16_t)(peekb(0x020C) + T.r[4]);
				cyc(132);
				call(0x8AB6, {});
				T.r[0] = (uint16_t)(peekb(0x020C) + T.r[4] + 1);
				cyc(102);
				call(0x8AB6, {});
				break;
			case 7:
				T.r[1] = (uint16_t)((peek(0x020A) & 1) << 1);
				T.r[0] = (uint16_t)(T.r[1] + T.r[3]);
				cyc(c + 108);
				call(0x8AB6, {});
				T.r[0] = (uint16_t)(peekb(0x020C) + T.r[1] + 1);
				cyc(90);
				call(0x8AB6, {});
				break;
			default:                                   // past the table (next table's data): unused
				cyc(c);
				continue;
			}
			cyc(12);                                   // BR back to the next command
			continue;
		}
		const uint16_t sel = (uint16_t)(T.r[3] - 0x80);
		T.r[0] = (uint16_t)(sel << 1);
		int c = 75 + 48 + 69;                          // fetch, number test, jump by the low byte
		switch (sel)
		{
		case 0:
			T.r[1] = 0;
			c += 24;
			break;
		case 1: case 2: case 3: case 4:
			T.r[5] = sext8(peekb(0x027A));
			T.r[0] = (uint16_t)(T.r[3] - 0x81);
			pokeb(0x027A, (uint8_t)T.r[0]);
			cyc(c + 111);
			T.r[1] = call(0x90BA, {});                 // park_level
			pokeb(0x027A, (uint8_t)T.r[5]);
			c = 51;
			break;
		case 5: case 6: case 7:
			T.r[1] = peek(0x0F38);
			c += 36;
			break;
		case 8:
			T.r[0] = rating_class();
			poke(T.r[6], peekb((uint16_t)(0xFE8C + T.r[0])));
			cyc(c + RATING_CLASS + 21 + 36 + 30);
			call(0x90BA, {});
			cyc(24 + 30);
			T.r[1] = call(0x80D8, {T.r[0]});           // mul16 with the slot
			c = 42 - 18;                               // MOV R0,R1, BR (TST (SP)+ in the call)
			c += 18;
			break;
		case 9:
			T.r[0] = peekb((uint16_t)(0xFE7C + peekb(0x0F31)));
			T.r[1] = peekb((uint16_t)(0xFE70 + T.r[0]));
			c += 132;
			break;
		case 10:
			T.r[1] = (uint16_t)(peekb(0x01F9) >> 2);
			c += 72;
			break;
		case 11:
			cyc(c + 30);
			T.r[1] = call(0x90BA, {});
			c = 24;
			break;
		case 12:
			T.r[1] = (uint16_t)(peekb(0x01F5) + 1);
			c += 60;
			break;
		case 13:
			T.r[0] = peekb(0x01F5);
			T.r[1] = (uint16_t)(2 - T.r[0]);
			c += 78;
			break;
		case 14: case 15: case 16: case 17:            // the bytes 0x025C-0x025F
			T.r[1] = peekb((uint16_t)(0x01CE + T.r[3]));
			c += 51;
			if (T.r[1] & 0x80)
			{
				T.r[1] = 0;
				c += 24;
			}
			break;
		case 18:
			T.r[1] = peek(0x0248);
			c += 24;
			break;
		default:                                       // past the table: unused
			cyc(c);
			continue;
		}
		c += 66;
		T.r[4] = peekb(0x020D);
		if (T.r[4] & 0x00F8)
		{
			T.r[0] = (uint16_t)(((T.r[4] >> 1) & ~3) + peek(0x0186));
			poke(0x0186, T.r[0]);
			poke(0x0186, (uint16_t)(peek(0x0186) - 0x0040));
			c += 138;
		}
		else
		{
			poke(0x0186, (uint16_t)(peek(T.r[2]) + 0x2000));
			T.r[2] = (uint16_t)(T.r[2] + 2);
			c += 66;
		}
		c += 57;
		const uint16_t format = T.r[4] & 7;
		T.r[4] = (uint16_t)(format << 1);
		switch (format)
		{
		case 0:
			if ((int16_t)T.r[1] < 10)
			{
				T.r[4] = (uint16_t)(T.r[1] + 0x30);
				T.r[0] = T.r[4];
				c += 84;
			}
			else
			{
				T.r[0] = 0x003D;
				c += 48;
			}
			cyc(c + 30);
			call(0x8B52, {});                          // draw_large_char
			cyc(12);
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
			cyc(c + 438 + 60);
			call(0xBA9A, {1, 0, 3, 0});
			cyc(18);
			break;
		}
		case 2:
			poke(T.r[6], T.r[1]);
			cyc(c + 87 + 90);
			call(0xBA9A, {1, 1, 4, 0});
			cyc(18);
			break;
		case 3:
			poke(T.r[6], T.r[1]);
			cyc(c + 141 + 30);
			call(0xBA9A, {0, 1, 2, 0});
			cyc(18);
			break;
		case 4:
			poke(T.r[6], peek(0x023E));
			cyc(c + 165 + 30);
			call(0xBA96, {1, 0, 6, peek(0x0240)});
			cyc(12 + 18);
			break;
		case 5:
			poke(T.r[6], peek(0x0242));
			cyc(c + 135 + 30);
			call(0xBA96, {0, 0, 3, 0});
			cyc(12 + 18);
			break;
		case 6:
			T.r[4] = (uint16_t)(T.r[1] + 0x30);
			T.r[0] = T.r[4];
			cyc(c + 72);
			call(0x8C12, {});                          // put_small_char
			cyc(12);
			break;
		case 7:
			poke(T.r[6], T.r[1]);
			cyc(c + 75 + 90);
			call(0xBA9A, {1, 1, 5, 0});
			cyc(18);
			break;
		}
		cyc(18);                                       // JMP back to the next command
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
	Clock k;
	T.r[6] = (uint16_t)(T.r[6] - 6);                    // SUB #6,SP: the local frame
	const uint16_t fp = T.r[6];                        // fp+2: &0x0370[object], fp+4: on the ramp
	k.add(18);
	k.at(0xA514);
	const bool timer = peekb(0x0EA2) != 0;
	k.add(24 + 12);
	if (!timer)
	{
		k.at(0xA51A); poke(0x023A, peek(0x03F0)); k.add(39);
		k.at(0xA520); poke(0x0238, peek(0x0670)); k.add(39 + 12);
	}
	else
	{
		k.at(0xA528); poke(0x023A, 0x7106); k.add(33);
		k.at(0xA52E); poke(0x0238, peek(0x0670)); k.add(39);
		k.at(0xA534); poke(0x0238, (uint16_t)(peek(0x0238) - peek(0x06B0))); k.add(39);
	}
	T.r[0] = 0x3800; k.add(18);
	k.at(0xA53E); poke(0x01D2, T.r[0]); k.add(27);
	k.at(0xA542); poke(0x1400, T.r[0]); k.add(27);
	T.r[0] = 0x0400; k.add(18);
	k.at(0xA54A); poke(0x01D4, T.r[0]); k.add(27);
	k.at(0xA54E); poke(0x1402, T.r[0]); k.add(27);
	k.enter(0x5684);
	call_from(0xA556, 0x5684, {});
	T.r[1] = T.r[0]; k.add(12);
	k.at(0xA558); poke(0x01D2, 0); k.add(27);
	k.at(0xA55C); poke(0x1400, 0); k.add(27 + 12 + 12);
	auto finish = [&]() {                              // A8CC: ADD #6,SP; RTS
		k.add(18 + 21);
		T.r[6] = fl::add(T.r[6], 6);
		k.leave();
	};
	auto store_object = [&]() {                        // A8B4: MOVB R1,@#0x0334; BR
		k.at(0xA8B4); pokeb(0x0334, (uint8_t)T.r[1]); k.add(27 + 12);
		finish();
	};
	if ((int16_t)T.r[1] <= 0)
	{
		k.add(18);
		k.at(0xA8BA);
		const uint8_t t = peekb(0x0EA2);
		k.add(24 + 12);
		if (t)
		{
			k.at(0xA8C0); pokeb(0x0EA2, (uint8_t)(peekb(0x0EA2) - 1)); k.add(27);
		}
		k.at(0xA8C4); pokeb(0x0EA1, 0); k.add(27);
		k.at(0xA8C8); pokeb(0x0334, 0); k.add(27);
		finish();
		return;
	}
	poke((uint16_t)(fp + 2), 0x0370);
	poke((uint16_t)(fp + 2), (uint16_t)(0x0370 + T.r[1]));
	k.add(36 + 30 + 12);
	auto kind = [fp]() -> uint16_t { return peekb(peek((uint16_t)(fp + 2))); };
	k.at(0xA574); T.r[0] = kind(); k.add(33);
	poke((uint16_t)(fp + 4), 0); k.add(30);
	k.at(0xA57C);
	const bool ramp = peekb(0x01EA) != 0;
	k.add(24 + 12);
	if (ramp)
	{
		poke((uint16_t)(fp + 4), (uint16_t)(peek((uint16_t)(fp + 4)) + 1));
		k.add(30 + 12);
	}
	else
	{
		k.at(0xA588); T.r[3] = peek(0x06B2); k.add(24);
		k.at(0xA58C); T.r[3] = (uint16_t)(T.r[3] - peek(0x06B0)); k.add(24);
		T.r[2] = 0; k.add(12);
		k.at(0xA592); T.r[2] = peekb((uint16_t)(0x6631 + T.r[0])); k.add(27);
		T.r[2] = (uint16_t)(T.r[2] << 8); k.add(12 + 12 + 12 + 12);
		if ((int16_t)T.r[3] > (int16_t)T.r[2])
		{
			k.add(18);
			finish();
			return;
		}
	}
	k.at(0xA5A2); T.r[0] = peekb((uint16_t)(0x61DA + T.r[0])); k.add(27 + 18);
	T.r[2] = (uint16_t)(T.r[0] << 1); k.add(12 + 12 + 27);
	// CLR R0, JSR F:AD70 (object 0 in front of R1?), ROR, BCC, MOV #4 / #12: the direction
	auto facing = [&](uint16_t jsr, uint16_t ret) -> uint16_t {
		T.r[0] = 0; k.add(12);
		k.enter(0xAD70);
		call_from(ret, 0xAD70, {});
		(void)jsr;
		const bool front = (T.r[0] & 1) != 0;
		T.r[0] = ror(T.r[0]);
		k.add(12 + 12 + 18 + (front ? 12 : 0));
		return front ? 4 : 12;
	};
	switch (T.r[0])
	{
	case 2: case 7:
	{
		T.r[0] = 0; k.add(12);
		k.at(0xA5BA); T.r[0] = peekb(0x0334); k.add(24 + 12 + 12);
		if (T.r[0] == T.r[1])
		{
			k.add(18);
			store_object();
			return;
		}
		k.at(0xA5C2);
		const bool riding = peekb(0x031E) != 0;
		k.add(24 + 12);
		if (!riding)
		{
			T.r[0] = facing(0xA5CA, 0xA5CE);
			k.at(0xA5DC); pokeb(0x031D, (uint8_t)T.r[0]); k.add(27);
		}
		k.at(0xA5E0); pokeb(0x01F8, peekb(0x031D)); k.add(39);
		k.at(0xA5E6); poke(0x01B4, 0x0514); k.add(33);
		k.enter(0xA2F0);
		call_from(0xA5F0, 0xA2F0, {});                 // skater_land
		T.r[0] = 0; k.add(12);
		k.at(0xA5F2); T.r[0] = kind(); k.add(33 + 18 + 12);
		if (T.r[0] != 4)
		{
			k.add(18);
			store_object();
			return;
		}
		push(0); k.add(24);
		k.at(0xA5FE); poke(T.r[6], peekb((uint16_t)(0x0390 + T.r[1]))); k.add(36);
		k.enter(0xCF5E);
		call_from(0xA606, 0xCF5E, {});
		k.enter(0xE222);
		call_from(0xA60A, 0xE222, {});                 // state1_to_8
		k.add(12 * 4 + 18);                            // BR, BR, BR, BR, TST (SP)+
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(rw((uint16_t)(T.r[6] - 2)));
		store_object();
		return;
	}
	case 3: case 4:
	{
		push(0x0067); k.add(30);
		k.enter(0x8E78);
		call_from(0xA6AE, 0x8E78, {});                 // queue_sound
		T.r[2] = (uint16_t)(T.r[1] << 1); k.add(12 + 12);
		k.at(0xA6B2); poke((uint16_t)(0x03F0 + T.r[2]), 0); k.add(30);
		poke(T.r[6], T.r[1]); k.add(21);
		push(0); k.add(24);
		k.at(0xA6BA); poke(T.r[6], peekb(0x5CDE)); k.add(33);
		k.enter(0xB602);
		call_from(0xA6C2, 0xB602, {});
		k.at(0xA6C2); pokeb((uint16_t)(0x04B0 + T.r[1]), (uint8_t)(peekb((uint16_t)(0x04B0 + T.r[1])) | 0x08)); k.add(36);
		T.r[0] = 7; k.add(18);
		k.at(0xA6CC); pokeb((uint16_t)(0x0790 + T.r[1]), 7); k.add(30);
		k.at(0xA6D0); pokeb((uint16_t)(0x0770 + T.r[1]), 7); k.add(30);
		k.at(0xA6D4); poke((uint16_t)(0x0430 + T.r[2]), 0x0080); k.add(36);
		T.r[0] = 0; k.add(12);
		k.at(0xA6DC); T.r[0] = kind(); k.add(33);
		T.r[0] = (uint16_t)((T.r[0] - 0x48) << 1); k.add(18 + 12);
		k.at(0xA6E6); poke(T.r[6], peek((uint16_t)(0xFCFA + T.r[0]))); k.add(36);
		k.enter(0xEF24);
		call_from(0xA6EE, 0xEF24, {});
		fl::cmp(rw(T.r[6]), rw((uint16_t)(T.r[6] + 2)));
		T.r[6] = (uint16_t)(T.r[6] + 4);
		k.add(24 + 12 + 18);
		store_object();
		return;
	}
	case 5:
	{
		k.at(0xA6F2);
		const bool carried = peekb(0x0EA0) != 0;
		k.add(24 + 12);
		if (!carried)
		{
			k.at(0xA6F8);
			const bool mode1 = peekb(0x031F) == 1;
			k.add(30 + 12);
			if (mode1)
				k.add(12);
			else
			{
				k.at(0xA700); pokeb(0x0EA0, (uint8_t)T.r[1]); k.add(27);
				k.at(0xA704); pokeb(0x0EA0, (uint8_t)-peekb(0x0EA0)); k.add(27);
			}
		}
		k.add(18);
		store_object();
		return;
	}
	case 6:
	{
		T.r[0] = 0; k.add(12);
		k.at(0xA60E); T.r[0] = peekb(0x0334); k.add(24 + 12 + 12);
		if (T.r[0] == T.r[1])
		{
			k.add(18);
			store_object();
			return;
		}
		k.at(0xA616); T.r[0] = peek(0x06B2); k.add(24);
		k.at(0xA61A); T.r[0] = (uint16_t)(T.r[0] - peek(0x06B0)); k.add(24 + 18 + 12);
		if ((int16_t)T.r[0] < 0x0380)
		{
			T.r[0] = facing(0xA626, 0xA62A);
			k.at(0xA638); pokeb(0x031D, (uint8_t)T.r[0]); k.add(27);
			k.at(0xA63C); pokeb(0x01F8, (uint8_t)T.r[0]); k.add(27);
			k.at(0xA640); poke(0x01B4, 0x0514); k.add(33);
			k.enter(0xA2F0);
			call_from(0xA64A, 0xA2F0, {});             // skater_land
			k.add(12);
		}
		else
		{
			T.r[0] = 0; k.add(12);
			k.at(0xA64E); T.r[0] = peekb(0x0E6C); k.add(24 + 12 + 12);
			if (T.r[0])
			{
				push(T.r[0]); k.add(24);
				k.enter(0xF064);
				call_from(0xA65C, 0xF064, {});
				T.r[0] = 0; k.add(12);
				k.at(0xA65E); T.r[0] = peekb(0x0E6C); k.add(24);
				T.r[0] = (uint16_t)(T.r[0] << 1); k.add(12);
				k.at(0xA664); poke(T.r[6], peek((uint16_t)(0xFCFA + T.r[0]))); k.add(36);
				k.enter(0xEF24);
				call_from(0xA66C, 0xEF24, {});
				k.at(0xA66C);
				const bool same = kind() == peekb(0x0E6B);
				k.add(45 + 12);
				if (same)
				{
					k.at(0xA674); pokeb(0x0E6C, (uint8_t)(peekb(0x0E6C) - 1)); k.add(27);
				}
				k.at(0xA678); pokeb(0x0E6B, (uint8_t)kind()); k.add(48);
				T.r[6] = (uint16_t)(T.r[6] + 2); k.add(18);
			}
		}
		T.r[0] = 0x3800; k.add(18);
		k.at(0xA684); poke(0x01D2, T.r[0]); k.add(27);
		k.at(0xA688); poke(0x1400, T.r[0]); k.add(27);
		T.r[0] = 0x3C00; k.add(18);
		k.at(0xA690); poke(0x01D4, T.r[0]); k.add(27);
		k.at(0xA694); poke(0x1402, T.r[0]); k.add(27);
		push(T.r[1]); k.add(24);
		k.enter(0x4ECC);
		call_from(0xA69E, 0x4ECC, {});
		poke(T.r[6], T.r[1]); k.add(21);
		k.enter(0xD47C);
		call_from(0xA6A4, 0xD47C, {});
		k.add(12 + 12 + 12 + 18);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(rw((uint16_t)(T.r[6] - 2)));
		store_object();
		return;
	}
	case 8:
	{
		push(0x0061); k.add(30);
		k.enter(0x8E78);
		call_from(0xA714, 0x8E78, {});                 // queue_sound
		T.r[0] = 0; k.add(12);
		k.at(0xA716); T.r[0] = kind(); k.add(33);
		T.r[0] = (uint16_t)(T.r[0] - 0x38); k.add(18);
		k.at(0xA71E); T.r[2] = sext8(peekb((uint16_t)(0xFD10 + T.r[0]))); k.add(27);
		poke(T.r[6], T.r[2]); k.add(21);
		k.enter(0xF002);
		call_from(0xA728, 0xF002, {});
		poke(T.r[6], 0); k.add(21);
		k.at(0xA72A); poke(T.r[6], peekb((uint16_t)(0x0390 + T.r[1]))); k.add(36);
		k.enter(0xCF5E);
		call_from(0xA732, 0xCF5E, {});
		k.add(12 + 12 + 18);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(rw((uint16_t)(T.r[6] - 2)));
		store_object();
		return;
	}
	case 9:
	{
		T.r[0] = 0; k.add(12);
		k.at(0xA736); T.r[0] = kind(); k.add(33 + 18 + 12);
		if (T.r[0] == 0x14)
		{
			T.r[0] = 7;
			k.add(18 + 12);
		}
		else
		{
			T.r[0] = 0x0F;
			k.add(18);
		}
		k.at(0xA74A); pokeb(0x0329, (uint8_t)T.r[0]); k.add(27);
		T.r[2] = 0; k.add(12);
		k.at(0xA750); T.r[2] = peekb(0x0328); k.add(24 + 12 + 12);
		if (T.r[2] == T.r[0])
		{
			k.add(18);
			store_object();
			return;
		}
		k.at(0xA758); pokeb(0x0328, (uint8_t)T.r[0]); k.add(27 + 33 + 12);
		if (peek((uint16_t)(fp + 4)) & 1)
		{
			push(T.r[0]); k.add(24);
			k.enter(0x9E52);
			call_from(0xA76A, 0x9E52, {});             // slope_table_value
			poke(T.r[6], T.r[0]); k.add(21);
			k.enter(0x9EEE);
			call_from(0xA770, 0x9EEE, {});             // skater_deflect
		}
		else
		{
			push(0x0008); k.add(30);
			T.r[0] = 0; k.add(12);
			k.at(0xA778); T.r[0] = peekb(0x031D); k.add(24);
			poke(T.r[6], (uint16_t)(rw(T.r[6]) ^ T.r[0])); k.add(21);
			k.enter(0x9E96);
			call_from(0xA782, 0x9E96, {});             // skater_bounce
		}
		k.add(12 + 12 + 18);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(rw((uint16_t)(T.r[6] - 2)));
		store_object();
		return;
	}
	case 10:
	{
		push(T.r[1]); k.add(24);
		k.enter(0xA8D2);
		call_from(0xA78A, 0xA8D2, {});
		T.r[4] = T.r[0]; k.add(12);
		T.r[0] = 0; k.add(12);
		k.at(0xA78E); T.r[0] = kind(); k.add(33);
		T.r[0] = (uint16_t)(T.r[0] - 0x1A); k.add(18);
		T.r[5] = (uint16_t)(T.r[4] << 1); k.add(12 + 12);
		T.r[2] = (uint16_t)(0x03B0 + T.r[5]); k.add(18 + 12);
		poke(fp, T.r[1]);
		poke(fp, (uint16_t)(T.r[1] << 1)); k.add(30 + 30);
		push(0x03B0);
		poke(T.r[6], (uint16_t)(rw(T.r[6]) + rw((uint16_t)(T.r[6] + 4))));
		T.r[3] = pop(); k.add(30 + 36 + 18);
		bool far = true;
		k.add(18 + 12);
		if ((int16_t)T.r[0] >= 5) k.add(18 + 12);
		if ((int16_t)T.r[0] < 5 || (int16_t)T.r[0] > 7)
		{
			T.r[0] = 0; k.add(12);
			k.enter(0xAD70);
			call_from(0xA7C4, 0xAD70, {});
			far = !(T.r[0] & 1);
			T.r[0] = ror(T.r[0]);
			k.add(12 + 12);
		}
		if (!far)
		{
			k.at(0xA7C8); poke(T.r[2], (uint16_t)(peek(T.r[2]) + 2)); k.add(27);
			k.at(0xA7CC); poke(T.r[3], (uint16_t)(peek(T.r[3]) + 2)); k.add(27);
			T.r[0] = 0x0018; k.add(18 + 12);
		}
		else
		{
			k.at(0xA7D6); poke(0x0F20, (uint16_t)(peek(0x0F20) + 6)); k.add(33);
			k.at(0xA7DC);
			const bool over = (int16_t)peek(0x0F20) > 0x0080;
			k.add(30 + 12);
			if (over)
			{
				k.at(0xA7E4); poke(0x0F20, 0x0080); k.add(33);
			}
			k.at(0xA7EA); pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) | 0x04)); k.add(33);
			k.at(0xA7F0); poke(T.r[2], (uint16_t)(peek(T.r[2]) + 4)); k.add(27);
			k.at(0xA7F4); poke(T.r[3], (uint16_t)(peek(T.r[3]) + 4)); k.add(27);
			T.r[0] = 0x0017; k.add(18);
		}
		k.at(0xA7FC); pokeb(peek((uint16_t)(fp + 2)), (uint8_t)T.r[0]); k.add(36);
		k.at(0xA800); pokeb((uint16_t)(0x0370 + T.r[4]), (uint8_t)T.r[0]); k.add(30);
		k.at(0xA804); pokeb((uint16_t)(0x0490 + T.r[1]), (uint8_t)(peekb((uint16_t)(0x0490 + T.r[1])) | 0x18)); k.add(36);
		k.at(0xA80A); pokeb((uint16_t)(0x0490 + T.r[4]), (uint8_t)(peekb((uint16_t)(0x0490 + T.r[4])) | 0x18)); k.add(36);
		T.r[0] = peek(fp); k.add(27);
		k.at(0xA814); poke((uint16_t)(0x0430 + T.r[0]), 0x0010); k.add(36);
		k.at(0xA81A); poke((uint16_t)(0x0430 + T.r[5]), 0x000F); k.add(36);
		k.add(12 + 18);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(rw((uint16_t)(T.r[6] - 2)));
		store_object();
		return;
	}
	case 11:
	{
		push(T.r[1]); k.add(24);
		k.enter(0xA8D2);
		call_from(0xA828, 0xA8D2, {});
		T.r[3] = T.r[0]; k.add(12);
		T.r[4] = (uint16_t)(T.r[3] << 1); k.add(12 + 12);
		k.at(0xA82E); poke((uint16_t)(0x03B0 + T.r[4]), (uint16_t)(peek((uint16_t)(0x03B0 + T.r[4])) + 2)); k.add(36);
		T.r[0] = (uint16_t)(T.r[1] << 1); k.add(12 + 12);
		k.at(0xA838); poke((uint16_t)(0x03B0 + T.r[0]), (uint16_t)(peek((uint16_t)(0x03B0 + T.r[0])) + 2)); k.add(36);
		T.r[5] = 0x0018; k.add(18);
		k.at(0xA842); pokeb(peek((uint16_t)(fp + 2)), 0x18); k.add(36);
		k.at(0xA846); pokeb((uint16_t)(0x0370 + T.r[3]), 0x18); k.add(30);
		k.at(0xA84A); pokeb((uint16_t)(0x0490 + T.r[1]), (uint8_t)(peekb((uint16_t)(0x0490 + T.r[1])) | 0x18)); k.add(36);
		k.at(0xA850); pokeb((uint16_t)(0x0490 + T.r[3]), (uint8_t)(peekb((uint16_t)(0x0490 + T.r[3])) | 0x18)); k.add(36);
		k.at(0xA856); poke((uint16_t)(0x0430 + T.r[0]), 0x0010); k.add(36);
		k.at(0xA85C); poke((uint16_t)(0x0430 + T.r[4]), 0x000F); k.add(36);
		k.add(12 + 18);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(rw((uint16_t)(T.r[6] - 2)));
		store_object();
		return;
	}
	case 12:
	{
		k.at(0xA864);
		const bool level = peek(0x06B2) == peek(0x06B0);
		k.add(36 + 12);
		if (!level)
		{
			store_object();
			return;
		}
		k.at(0xA86C); T.r[0] = peek(0x01B4); k.add(24 + 12);
		if ((int16_t)T.r[0] < 0)
		{
			T.r[0] = (uint16_t)-T.r[0];
			k.add(12);
		}
		k.add(18 + 12);
		if ((int16_t)T.r[0] < 0x0180)
		{
			store_object();
			return;
		}
		T.r[0] = 2; k.add(18);
		k.at(0xA87E); pokeb((uint16_t)(0x0790 + T.r[1]), 2); k.add(30);
		k.at(0xA882); pokeb((uint16_t)(0x0770 + T.r[1]), 2); k.add(30);
		T.r[0] = 0x2C00; k.add(18);
		k.at(0xA88A); poke(0x01D4, T.r[0]); k.add(27);
		k.at(0xA88E); poke(0x1402, T.r[0]); k.add(27);
		k.at(0xA892); T.r[0] = sext8(peekb(0x025D)); k.add(24);
		T.r[0] = (uint16_t)(T.r[0] << 1); k.add(12);
		k.at(0xA898); poke(0x05B2, (uint16_t)(peek(0x05B2) + peek((uint16_t)(0x7CA4 + T.r[0])))); k.add(42 + 33 + 12);
		if (peek((uint16_t)(fp + 4)) & 1)
		{
			k.enter(0x9F8A);
			call_from(0xA8AA, 0x9F8A, {});             // skater_jump
		}
		push(0x007B); k.add(30);
		k.enter(0x8E78);
		call_from(0xA8B2, 0x8E78, {});                 // queue_sound
		k.add(18);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(rw((uint16_t)(T.r[6] - 2)));
		store_object();
		return;
	}
	default:
		// types 0 and 1 do nothing; 13 is the next table's first word (0, as type 0) and
		// higher kinds of the table are not object types
		T.r[0] = 0;
		k.add(12 + 18);
		store_object();
		return;
	}
}

// F:9E96  skater_bounce(int dir): new direction dir (0x3F: the opposite one), speed 0x160,
// motion values for that direction (F:9B82 with terrain 0x0327 cleared, F:91A6, then again
// with the terrain), no upward z velocity for object 1; state 0x031E = 2 for 6 (0x031C)
uint16_t skater_bounce(const Args &a)
{
	const uint16_t r1 = T.r[1];
	int c = 69;
	if (a.u(0) == 0x003F)
	{
		T.r[0] = sext8(peekb(0x031D));
		T.r[1] = (uint16_t)(0x0008 ^ T.r[0]);
		pokeb(0x031D, (uint8_t)T.r[1]);
		c += 93;
	}
	else
	{
		pokeb(0x031D, a.b(0));
		c += 42;
	}
	poke(0x01B4, 0x0160);
	T.r[1] = sext8(peekb(0x0327));
	pokeb(0x0327, 0);
	cyc(c + 114);
	call(0x9B82, {});
	cyc(30);
	call(0x91A6, {});
	pokeb(0x0327, (uint8_t)T.r[1]);
	cyc(57);
	call(0x9B82, {});
	c = 36;
	if ((int16_t)peek(0x05B2) > 0)
	{
		poke(0x05B2, 0);
		c += 27;
	}
	pokeb(0x031E, 2);
	pokeb(0x031C, 6);
	cyc(c);
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
	int c = SAVE12_IN + 69;
	bool positive;                                     // which bound: 0x0160 or 0xFEA0
	bool force;                                        // set the bound unconditionally
	if (dir == 0x003F)
	{
		pokeb(0x031B, peekb(0x031D));
		positive = (int16_t)speed < 0;
		force = true;
		c += positive ? 75 : 63;
	}
	else
	{
		pokeb(0x031B, (uint8_t)dir);
		positive = (int16_t)speed >= 0;
		force = false;
		c += 66;
		if (!positive)
		{
			T.r[0] = sext8((uint8_t)dir);
			T.r[2] = (uint16_t)(0x0008 ^ T.r[0]);
			pokeb(0x031B, (uint8_t)T.r[2]);
			c += 114;
		}
	}
	if (positive)
	{
		if (!force) c += 30;
		if (force || (int16_t)speed < 0x0160)
		{
			poke(0x01B4, 0x0160);
			c += 33;
		}
	}
	else if (force || (int16_t)speed > (int16_t)0xFEA0)
	{
		poke(0x01B4, 0xFEA0);
		c += 45;
	}
	c += 42;
	if (peekb(0x01EA) == 2)
	{
		c += 36;
		if ((int16_t)peek(0x01B4) < 0)
		{
			flip_heading();
			c += 81;
		}
		cyc(c + 30);
		call(0xA2F0, {});
		c = 0;
	}
	c += 42;
	if (peekb(0x031F) == 1)
	{
		c += 36;
		if ((int16_t)peek(0x01B4) < 0)
		{
			flip_heading();
			poke(0x01B4, (uint16_t)-peek(0x01B4));
			c += 108;
		}
	}
	pokeb(0x031E, 1);
	pokeb(0x031C, 8);
	cyc(c + 66);
	T.r[1] = r1;
	T.r[2] = r2;
	return T.r[0];
}

// screen order of two objects (not the skater, or object 0 standing for it): an object
// without animation (0x03F0) is in front; otherwise the screen positions plus the frame
// offsets of the animations (x +8; y +10 / +12 depending on the side) decide
uint16_t objects_in_order(uint16_t o0, uint16_t o1)
{
	const uint16_t f0 = peek((uint16_t)(0x03F0 + 2 * o0));
	if (!f0) return 1;
	const uint16_t f1 = peek((uint16_t)(0x03F0 + 2 * o1));
	if (!f1) return 0;
	const uint16_t dx = (uint16_t)(peek((uint16_t)(0x0630 + 2 * o0)) + peek((uint16_t)(f0 + 8))
		- peek((uint16_t)(0x0630 + 2 * o1)) - peek((uint16_t)(f1 + 8)));
	const bool left = (dx & 0x8000) != 0;
	const uint16_t dy = (uint16_t)(peek((uint16_t)(0x0670 + 2 * o0)) + peek((uint16_t)(f0 + (left ? 0x0C : 0x0A)))
		- peek((uint16_t)(0x0670 + 2 * o1)) - peek((uint16_t)(f1 + (left ? 0x0A : 0x0C))));
	return (dy & 0x8000) ? 0 : 1;
}

// F:AD70 (assembly, objects R0 and R1): R0 = 1 when object R0 is drawn in front of object
// R1.  Objects 0-3 (the skater) keep their order and stand for the skater against others;
// the object carrying the skater (0x0EA0) is behind it.  R1-R4 preserved (F152); C is only
// set by the comparison of two skater objects.
void sprite_in_front()
{
	const uint16_t o0 = T.r[0] & 0x1F, o1 = T.r[1] & 0x1F;
	uint16_t r;
	int carry = 0;
	int c = 66 + 30;
	auto in_order = [&c](uint16_t a, uint16_t b) -> uint16_t {
		c += 51;
		const uint16_t f0 = peek((uint16_t)(0x03F0 + 2 * a));
		if (!f0) return c += 18, 1;
		c += 51;
		const uint16_t f1 = peek((uint16_t)(0x03F0 + 2 * b));
		if (!f1) return c += 12, 0;
		const uint16_t dx = (uint16_t)(peek((uint16_t)(0x0630 + 2 * a)) + peek((uint16_t)(f0 + 8))
			- peek((uint16_t)(0x0630 + 2 * b)) - peek((uint16_t)(f1 + 8)));
		const bool left = (dx & 0x8000) != 0;
		const uint16_t dy = (uint16_t)(peek((uint16_t)(0x0670 + 2 * a)) + peek((uint16_t)(f0 + (left ? 0x0C : 0x0A)))
			- peek((uint16_t)(0x0670 + 2 * b)) - peek((uint16_t)(f1 + (left ? 0x0A : 0x0C))));
		c += 120 + (left ? 120 : 108) + 24;
		if (dy & 0x8000) return c += 12, 0;
		return c += 18, 1;
	};
	const uint16_t carrier = sext8(peekb(0x0EA0));
	if (o0 < 4)
	{
		if (o1 < 4)
		{
			r = o0 < o1;
			carry = o0 < o1 ? CF : 0;
			c += 36 + (r ? 12 : 0) + 12;
		}
		else
		{
			c += 48;
			if (o1 == carrier)
			{
				r = 0;
				c += 12;
			}
			else
			{
				c += 24;
				r = in_order(0, o1);
			}
		}
	}
	else if (o1 < 4)
	{
		c += 48;
		if (o0 == carrier)
		{
			r = 1;
			c += 18;
		}
		else
		{
			c += 12;
			r = in_order(o0, 0);
		}
	}
	else
		r = in_order(o0, o1);
	T.r[0] = r;
	cyc(c);
	f_nzvc(nzw(T.r[1]) | carry);
}

// F:9C5E (assembly; start position R0, column R1, on the stack a height or a mode
// 0x7FFD-0x7FFF): follow the probe line through the playfield (0x0212 per step, video window
// 0x0218 moving on by 0x2000) while the height R2 (from the skater's z, -2 * 0x0212 per step)
// stays below the given one, at most 10 cells (30 off park 0); 0x8000 when the height is
// reached.  The first cell that answers gives its terrain code in R0 and its ground height
// in 0x0222 (table 0xFCB0 for flat codes, else the cell height plus the low bits of the z);
// mode 0x7FFE takes only cells with bit 14, 0x7FFF only negative attribute bytes; 0x7FFF in
// 0x0222 when none does.  R1-R5 preserved (F162).
void trace_cells()
{
	const uint16_t saved[5] = { T.r[1], T.r[2], T.r[3], T.r[4], T.r[5] };
	uint16_t pos = T.r[0];
	const uint16_t col = T.r[1];
	const uint16_t mode = rw((uint16_t)(T.r[6] + 2));
	T.r[0] = peek(0x0218);
	cyc(SAVE15_IN + 129);
	call(0x8078, {});                                  // select_video_bank
	T.r[2] = peek(0x06B0);
	T.r[0] = (uint16_t)-peek(0x0212);
	T.r[1] = (uint16_t)(T.r[0] << 1);
	T.r[4] = peekb(0x0F59) ? 0x001E : 0x000A;
	T.r[5] = mode;
	int c = 120 + (peekb(0x0F59) ? 18 : 30) + 27;
	for (;;)
	{
		c += 24;
		if ((int16_t)T.r[2] >= (int16_t)T.r[5])
		{
			T.r[3] = 0x8000;
			c += 30 + 12;
			break;
		}
		pos = (uint16_t)(pos + peek(0x0212));
		T.r[2] = (uint16_t)(T.r[2] + T.r[1]);
		c += 99;
		if (pos & 0x2000)
		{
			pos &= 0x1F00;
			poke(0x0218, (uint16_t)(peek(0x0218) + 0x2000));
			T.r[0] = peek(0x0218);
			cyc(c + 123);
			call(0x8078, {});
			c = 0;
		}
		c += 210;
		T.r[0] = (uint16_t)((pos & 0x1F00) + (col >> 8));
		T.r[3] = peek((uint16_t)(0x2000 + (T.r[0] & 0x1FFE)));
		if (T.r[5] == 0x7FFE)
		{
			c += 30;
			if (T.r[3] & 0x4000)
			{
				c += 30 + ((T.r[3] & 0x0400) ? 54 : 42) + 57;
				T.r[3] = sext8(peekb((uint16_t)(0x4000 + tile_number(T.r[3]))));
				if (!(T.r[3] & 7))
				{
					c += 78;
					if (((int16_t)T.r[3] >> 3) != 0x0F)
					{
						T.r[0] = (uint16_t)(((int16_t)T.r[3] >> 3) << 1);
						T.r[2] = peek((uint16_t)(0xFCB0 + T.r[0]));
						c += 99;
						break;
					}
				}
				T.r[3] &= 0x003F;
				T.r[0] = (uint16_t)((peek(0x06B0) & 0x01FF) + T.r[2]);
				T.r[2] = T.r[0] & ~1;
				c += 96 + 30;
				break;
			}
		}
		else
		{
			c += 30 + ((T.r[3] & 0x0400) ? 54 : 42) + 30;
			T.r[3] = tile_number(T.r[3]);
			if (T.r[5] == 0x7FFF)
			{
				c += 39;
				T.r[0] = sext8(peekb((uint16_t)(0x6000 + T.r[3])));
				if (T.r[0] & 0x8000)
				{
					T.r[3] = T.r[0] & 0x007F;
					c += 30 + 12;
					break;
				}
			}
			else
			{
				c += 42;
				if ((int8_t)peekb(0x032F) > 1)
				{
					c += 39;
					if (peekb((uint16_t)(0x6000 + T.r[3])) & 0x80)
					{
						T.r[3] = sext8(peekb((uint16_t)(0x6000 + T.r[3]))) & 0x007F;
						c += 45;
					}
					else
					{
						T.r[3] = sext8(peekb((uint16_t)(0x4000 + T.r[3])));
						c += 39;
					}
				}
				else
				{
					T.r[3] = sext8(peekb((uint16_t)(0x4000 + T.r[3])));
					c += 39;
				}
				c += 30;
				if (T.r[3] & 7)
				{
					T.r[3] &= 0x003F;
					c += 18;
				}
				c += 42;
				bool take = (T.r[3] & 7) != 7;
				if (!take)
				{
					c += 30;
					take = T.r[3] == 0x003F;
				}
				if (take)
				{
					c += 30;
					bool flat = false;
					if (!(T.r[3] & 7))
					{
						c += 78;
						flat = ((int16_t)T.r[3] >> 3) != 0x0F;
					}
					if (flat)
					{
						T.r[0] = (uint16_t)(((int16_t)T.r[3] >> 3) << 1);
						T.r[2] = peek((uint16_t)(0xFCB0 + T.r[0]));
						c += 99;
					}
					else
					{
						T.r[0] = peek(0x06B0) & 0x01FF;
						T.r[2] = (uint16_t)(T.r[2] + T.r[0]);
						c += 54;
					}
					T.r[2] &= ~1;
					c += 30;
					break;
				}
			}
		}
		c += 24;
		if (--T.r[4] == 0)
		{
			T.r[2] = 0x7FFF;
			c += 18;
			break;
		}
		c += 18;
	}
	poke(0x0222, T.r[2]);
	T.r[0] = T.r[3];
	cyc(c);
	fl::cmp(col, pos);                                 // CMP (SP)+,(SP)+ on the two locals
	for (int k = 0; k < 5; k++) T.r[k + 1] = saved[k];
	f_nzv(nzw(T.r[1]));
}

// F:9A32 (assembly): react to the terrain code of the probed cell 2 (0x0304): on a ramp
// the skater takes off (F:9F8A) when object 1 is above it, or when the ramp under a new
// slope (A5:51EE) is steep enough for the speed (F:80D8); in the air, close enough above
// the ground while falling, he lands (sounds 0x47 / 0x4B, F:A090, B3:7ADA or 7E98 on park
// 3).  A new terrain is taken over (0x0327, repeat count 0x032C / 0x032D, direction mapped
// by 0xFCE0 when leaving a corner code) with new motion values (F:9B82) and, in the air,
// the jump's z velocity (F:80FA).
void check_terrain()
{
	T.r[1] = peek(0x0304);
	int c = 60;
	if (peekb(0x01EA))
	{
		c += 78;
		T.r[0] = (uint16_t)(peek(0x06B2) - peek(0x06B0));
		if ((int16_t)T.r[0] <= 0)
		{
			c += 48;
			T.r[2] = sext8(peekb(0x0327));
			if (T.r[2] != T.r[1])
			{
				c += 42;
				T.r[2] = (uint16_t)(7 & ~T.r[1]);
				if (T.r[2])
				{
					T.r[2] = 0x1800;
					poke(0x01D2, T.r[2]);
					poke(0x1400, T.r[2]);
					cyc(c + 126);
					call(0x51EE, {T.r[1]});
					T.r[2] = 0x0800;
					poke(0x01D2, T.r[2]);
					poke(0x1400, T.r[2]);
					T.r[3] = T.r[0];
					T.r[2] = peek(0x01B4);
					c = 120 + 24;
					if ((int16_t)T.r[2] < 0)
					{
						T.r[0] = (uint16_t)-T.r[0];
						c += 12;
					}
					if ((int16_t)T.r[0] > 0)
					{
						T.r[3] = sext8((uint8_t)(T.r[3] >> 8));
						cyc(c + 141);
						call(0x80D8, {(uint16_t)((int16_t)T.r[2] >> 2), T.r[3]});   // mul16
						T.r[0] = (uint16_t)(T.r[0] - 0x1200);
						c = 36 - 18;                   // SUB, TST (SP)+ (the call pops)
						c += 18;
					}
					c += 18;
				}
			}
		}
		c += 24;
		if ((int16_t)T.r[0] > 0)
		{
			cyc(c + 30);
			call(0x9F8A, {});                          // skater_jump
			c = 12;
		}
	}
	else
	{
		c += 48;
		if ((int16_t)peek(0x05B2) < (int16_t)peek(0x05B0))
		{
			c += 138;
			T.r[2] = (uint16_t)(peek(0x06B0) + peek(0x05B0));
			T.r[0] = (uint16_t)(peek(0x06B2) + peek(0x05B2) - T.r[2]);
			if ((int16_t)T.r[0] < 0x0040)
			{
				cyc(c + 60);
				call(0x8E78, {0x0047});                // queue_sound
				cyc(57);
				call(0x8E78, {0x004B});
				cyc(51);
				call(0xA090, {T.r[1]});
				bank_a(0x3800);
				bank_b(0x3C00);
				const bool park3 = peekb(0x0F59) == 3;
				cyc(186 + 30);
				call(park3 ? 0x7E98 : 0x7ADA, {T.r[1]});
				c = (park3 ? 0 : 12) + 18;
			}
		}
	}
	c += 48;
	T.r[0] = sext8(peekb(0x0327));
	fl::cmp(T.r[0], T.r[1]);
	if (T.r[0] == T.r[1])
	{
		cyc(c);
		return;
	}
	c += 42;
	T.r[2] = (uint16_t)(7 & ~T.r[1]);
	f_nzv(nzw(T.r[2]));
	if (!T.r[2])
	{
		cyc(c);
		return;
	}
	c += 48;
	T.r[2] = sext8(peekb(0x032D));
	if (T.r[1] == T.r[2])
	{
		T.r[2] = (uint16_t)(sext8(peekb(0x032C)) - 1);
		pokeb(0x032C, (uint8_t)T.r[2]);
		c += 75;
	}
	else
	{
		pokeb(0x032C, 0x0A);
		c += 33;
	}
	pokeb(0x032D, (uint8_t)T.r[0]);
	c += 69;
	T.r[2] = (uint16_t)(7 & ~T.r[0]);
	if (!T.r[2])
	{
		c += 30;
		if (!(T.r[1] & 7))
		{
			T.r[0] = sext8(peekb(0x031D));
			pokeb(0x031D, peekb((uint16_t)(0xFCE0 + T.r[0])));
			c += 66;
		}
	}
	pokeb(0x0327, (uint8_t)T.r[1]);
	cyc(c + 57);
	call(0x9B82, {});                                  // skater_motion_params
	const uint8_t ramp = peekb(0x01EA);
	if (ramp)
	{
		fl::tstb(ramp);
		cyc(36);
		return;
	}
	const uint16_t speed = peek(0x01B4), factor = peek(0x02EC);
	cyc(36 + 102);
	call(0x80FA, {factor, speed});                     // mul32
	pokeb(0x07F8, peekb(0x0205));
	poke(0x05B0, peek(0x0206));
	c = 114;
	if (peekb(0x0EA0))
	{
		cyc(c + 30);
		call(0xD6BE, {});
		c = 0;
	}
	fl::cmp(factor, speed);                            // CMP (SP)+,(SP)+
	cyc(c + 24);
}

}

LOGIC_TABLE(flow,
	c_routine("F", 0x9034, "divide_ext", divide_ext, 0, SAVE15_IN + 21 + SAVE15_OUT),
	c_routine("F", 0x90A6, "divide", divide, 0, 18 + 21),
	c_routine("F", 0x90B0, "modulo", modulo, 0, 18 + 21),
	c_routine("F", 0x90BA, "park_level", park_level, 0, SAVE12_IN + 21 + SAVE12_OUT),
	asm_routine("F", 0x8AB6, "draw_string_large", draw_string_large, 33 + 39 + 21 + SAVE13_OUT),
	asm_routine("F", 0x8B04, "draw_string_small", draw_string_small, 33 + 39 + 21 + SAVE13_OUT),
	c_routine("F", 0x9F8A, "skater_jump", skater_jump, 0, 21),
	asm_routine("F", 0xAFF0, "process_sprite_requests", process_sprite_requests, 21),
	c_routine("F", 0xA974, "queue_message", queue_message, 0, 18 + 21),
	c_routine("F", 0xA8FE, "flush_text_list", flush_text_list, 0, 165 + 21 + SAVE15_OUT),
	c_routine("F", 0x91A6, "skater_velocity", skater_velocity, 0, 0),
	c_routine("F", 0x843E, "place_skater", place_skater, 0, 18 + 21),
	c_routine("F", 0xA45A, "skater_phase10", skater_phase10, 0, 150 + 21 + SAVE12_OUT),
	c_routine("F", 0xACE6, "unlink_sprites", unlink_sprites, 0, SAVE15_IN + 21 + SAVE15_OUT),
	c_routine("F", 0xAE02, "link_sprites", link_sprites, 0, 21 + SAVE15_OUT),
	asm_routine("F", 0xAF32, "sort_sprite_list", sort_sprite_list, 18 + 21),
	asm_routine("F", 0x944C, "cell_terrain_codes", cell_terrain_codes, 21),
	asm_routine("F", 0x934C, "probe_ground", probe_ground, 0),
	c_routine("F", 0x9B82, "skater_motion_params", skater_motion_params, 0, SAVE14_IN + 75 + SAVE14_OUT),
	c_routine("F", 0x9FB4, "skater_take_off", skater_take_off, 0, 150 + 21 + SAVE13_OUT),
	asm_routine("F", 0x9236, "update_push", update_push, 21),
	irq_routine("F", 0x899C, "irq_vblank_play", irq_vblank_play, 126 + 24),
	c_routine("F", 0xA2F0, "skater_land", skater_land, 0, 255 + 21 + SAVE13_OUT),
	c_routine("F", 0xA090, "skater_touch_down", skater_touch_down, 0, 21 + SAVE14_OUT),
	asm_routine("F", 0x94DC, "edge_mode", edge_mode, 21),
	asm_routine("F", 0x979A, "check_wall", check_wall, 21),
	c_routine("F", 0xA9FC, "run_text_script", run_text_script, 0, 180 + 21 + SAVE15_OUT),
	asm_routine("F", 0xA510, "object_contact", object_contact, 0),
	c_routine("F", 0x9E96, "skater_bounce", skater_bounce, 0, 84 + 21),
	c_routine("F", 0x9EEE, "skater_deflect", skater_deflect, 0, 21 + SAVE12_OUT),
	asm_routine("F", 0xAD70, "sprite_in_front", sprite_in_front, SAVE14_IN + 21 + SAVE14_OUT),
	asm_routine("F", 0x9C5E, "trace_cells", trace_cells, 63 + 21 + SAVE15_OUT),
	asm_routine("F", 0x9A32, "check_terrain", check_terrain, 21),
)

}
