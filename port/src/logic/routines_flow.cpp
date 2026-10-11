// Rewritten routines: text strings, skater motion and state changes, sprite link lists,
// message queue and helpers of the frame flow.  Routines with an assembly interface
// reproduce registers and condition codes; the others follow the C convention.
#include "logic.h"

namespace logic {
namespace {

using namespace t11rt;

uint16_t sext8(uint8_t v) { return (uint16_t)(int16_t)(int8_t)v; }

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
uint16_t neg(uint16_t d)
{
	const uint16_t r = (uint16_t)-d;
	f_nzvc(nzw(r) | (r == 0x8000 ? VF : 0) | (r ? CF : 0));
	return r;
}
uint16_t ror(uint16_t d)
{
	const uint16_t r = (uint16_t)((d >> 1) | (C() << 15));
	f_nzvc(nzw(r) | (d & 1));
	f_shift();
	return r;
}

// Cycles and interrupts.  Every routine charges what its original's instructions cost on the
// path taken, instruction by instruction (Clock::add), and callees charge themselves.  The
// original can be interrupted after any instruction: when its time slice ends there, the
// board runs and a pending interrupt comes in before the next instruction.  The native code
// syncs at the same instruction boundaries, so the board and the interrupts see the same
// times; it also syncs (logic::tick) before each memory access, at each callee's entry (the
// JSR included) and at the return address after the RTS, so an interrupt there sees memory
// and the stack as in the original.
void cyc(int n) { T.icount -= n; }
struct Clock {
	int p = 0;                                         // cycles not charged yet
	uint16_t pc = 0;                                   // address of the last sync
	// one instruction of n cycles.  The original syncs after every instruction: when its time
	// slice ends there (or an interrupt is to be looked at), the board runs and a pending
	// interrupt is taken before the next instruction.  The native code syncs at the same
	// instruction boundary, so interrupts start at the original's time (the PC pushed is the
	// last sync address, registers may be ahead; memory is as in the original).
	void add(int n)
	{
		p += n;
		if (T.icount - p <= 0 || T.check_irqs) at(pc);
	}
	template <class... M> void add(int n, int m, M... more) { add(n); add(m, more...); }
	// one instruction of n cycles, followed by the one at next
	void step(int n, uint16_t next) { pc = next; add(n); }
	void at(uint16_t a) { pc = a; tick(a, p); p = 0; }  // before the memory access at a
	// JSR PC,@#callee returning to ret: the return address is pushed, the original syncs at
	// the callee (an interrupt there lands below the return address), and syncs at ret after
	// the callee's RTS (call_from)
	uint16_t call(uint16_t callee, uint16_t ret)
	{
		push(ret);
		tick(callee, p + 30);
		p = 0;
		T.r[6] = (uint16_t)(T.r[6] + 2);
		pc = ret;
		return call_from(ret, callee, {});
	}
	void flush() { cyc(p); p = 0; }
	// JSR R1,@#helper at entry, the register-save helpers F138 / F144 / F152 / F162 (R1-R2 ..
	// R1-R5): the JSR pushes R1, the helper pushes R2..Rn and the body address, gets R1 back
	// and replaces the body address by its own return address (JSR PC,@(SP)+)
	static uint16_t back(int last)
	{
		static const uint16_t b[6] = { 0, 0, 0xF142, 0xF150, 0xF160, 0xF172 };
		return b[last];
	}
	void save(uint16_t entry, int last)
	{
		static const uint16_t helper[6] = { 0, 0, 0xF138, 0xF144, 0xF152, 0xF162 };
		const uint16_t body = (uint16_t)(entry + 4);
		uint16_t a = helper[last];
		push(T.r[1]);
		T.r[1] = body;
		step(30, a);
		for (int r = 2; r <= last; r++)
		{
			push(T.r[r]);
			step(24, a += 2);
		}
		push(T.r[1]);
		step(24, a += 2);
		T.r[1] = rw((uint16_t)(T.r[6] + 2 * last));
		step(27, a += 4);
		ww(T.r[6], back(last));
		step(30, body);
	}
	// the body's RTS into the helper, its BR to the restores and MOV (SP)+,Rn .. MOV (SP)+,R1
	// (the helper's RTS is leave's)
	void restore(int last)
	{
		uint16_t a = back(last);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		step(21, a);
		if (last < 5)
		{
			a = (uint16_t)(0xF172 + 2 * (5 - last));
			step(12, a);
		}
		for (int r = last; r >= 2; r--)
		{
			T.r[r] = pop();
			step(18, a += 2);
		}
		T.r[1] = pop();
		step(18, a += 2);
	}
	// the final RTS: the original pops the return address and syncs there; the slot is put
	// back for the caller of the native routine, which pops it
	void leave()
	{
		const uint16_t sp = T.r[6], ret = rw(sp);
		T.r[6] = (uint16_t)(sp + 2);
		p += 21;
		at(ret);
		T.r[6] = sp;
		ww(sp, ret);
	}
};

// The routines below follow the original instruction by instruction (Clock): they charge
// each instruction, sync before each memory access and keep the original's stack image
// (register saves of the helpers F138-F162, arguments, return addresses through call_from).

// MOV #v,R0 / MOV R0,@#shadow / MOV R0,@#bank register at pc, pc+4, pc+8
void sel_a(Clock &k, uint16_t pc, uint16_t v)
{
	T.r[0] = v;
	k.add(18);
	k.at((uint16_t)(pc + 4)); poke(0x01D2, v); k.add(27);
	k.at((uint16_t)(pc + 8)); poke(0x1400, v); k.add(27);
}
void sel_b(Clock &k, uint16_t pc, uint16_t v)
{
	T.r[0] = v;
	k.add(18);
	k.at((uint16_t)(pc + 4)); poke(0x01D4, v); k.add(27);
	k.at((uint16_t)(pc + 8)); poke(0x1402, v); k.add(27);
}
void sel_ab(Clock &k, uint16_t pc, uint16_t a, uint16_t b)
{
	sel_a(k, pc, a);
	sel_b(k, (uint16_t)(pc + 12), b);
}


// F:9034  divide_ext(int mode, -, int d, int n): n / d, truncated and signed, by shift and
// subtract; with mode != 0 the remainder instead (sign of n).  0 when n or d is 0.
// R1-R5 preserved (F162).
// F:90A6  divide(int d, int n) = F:9034(0, ...);  F:90B0  modulo(int d, int n) = F:9034 with
// a non-zero mode (the PC pushed by MOV PC,-(SP))
// the body of F:9034 (after the register-save helper): R0 = d, R1 = n, R2 = the quotient,
// R3 = the shift count, R4 / R5 = n / d negative
uint16_t divide_body(Clock &k, uint16_t mode, uint16_t d, uint16_t n)
{
	// no memory accesses: each instruction with its flags and the address of the next one
	T.r[1] = n; fl::mov(n); k.step(27, 0x903C);
	if (!n)
	{
		k.step(12, 0x90A2);
		T.r[0] = 0; fl::clr(); k.step(12, 0x90A4);
		return 0;
	}
	k.step(12, 0x903E);
	T.r[0] = d; fl::mov(d); k.step(27, 0x9042);
	if (!d)
	{
		k.step(12, 0x90A2);
		T.r[0] = 0; fl::clr(); k.step(12, 0x90A4);
		return 0;
	}
	k.step(12, 0x9044);
	T.r[5] = 0; fl::clr(); k.step(12, 0x9046);
	T.r[4] = 0; fl::clr(); k.step(12, 0x9048);
	fl::tst(T.r[1]); k.step(12, 0x904A);
	if (T.r[1] & 0x8000)
	{
		k.step(12, 0x904C);
		T.r[1] = neg(T.r[1]); k.step(12, 0x904E);
		T.r[4] = fl::inc(T.r[4]); k.step(12, 0x9050);
	}
	else
		k.step(12, 0x9050);
	fl::tst(T.r[0]); k.step(12, 0x9052);
	if (T.r[0] & 0x8000)
	{
		k.step(12, 0x9054);
		T.r[0] = neg(T.r[0]); k.step(12, 0x9056);
		T.r[5] = fl::inc(T.r[5]); k.step(12, 0x9058);
	}
	else
		k.step(12, 0x9058);
	T.r[3] = 0; fl::clr(); k.step(12, 0x905A);
	// shift the divisor up to the dividend
	for (;;)
	{
		fl::cmp(T.r[0], T.r[1]); k.step(12, 0x905C);
		if ((int16_t)T.r[0] >= (int16_t)T.r[1])
		{
			k.step(12, 0x906A);
			break;
		}
		k.step(12, 0x905E);
		f_nzv(nzw(T.r[0] & 0x4000)); k.step(18, 0x9062);
		if (T.r[0] & 0x4000)
		{
			k.step(12, 0x906A);
			break;
		}
		k.step(12, 0x9064);
		T.r[0] = asl(T.r[0]); k.step(12, 0x9066);
		T.r[3] = fl::inc(T.r[3]); k.step(12, 0x9068);
		k.step(12, 0x905A);
	}
	T.r[2] = 0; fl::clr(); k.step(12, 0x906C);
	for (;;)
	{
		T.r[2] = asl(T.r[2]); k.step(12, 0x906E);
		fl::cmp(T.r[0], T.r[1]); k.step(12, 0x9070);
		if ((int16_t)T.r[0] <= (int16_t)T.r[1])
		{
			k.step(12, 0x9072);
			T.r[2] = fl::inc(T.r[2]); k.step(12, 0x9074);
			T.r[1] = fl::sub(T.r[1], T.r[0]); k.step(12, 0x9076);
			if (!T.r[1])
			{
				k.step(12, 0x9080);
				break;
			}
			k.step(12, 0x9078);
		}
		else
			k.step(12, 0x9078);
		T.r[3] = fl::dec(T.r[3]); k.step(12, 0x907A);
		if (T.r[3] & 0x8000)
		{
			k.step(12, 0x9080);
			break;
		}
		k.step(12, 0x907C);
		T.r[0] = asr(T.r[0]); k.step(12, 0x907E);
		k.step(12, 0x906C);
	}
	fl::tst(mode); k.step(27, 0x9084);                 // TST 0xE(SP): the mode
	if (mode)
	{
		k.step(12, 0x9086);
		fl::tst(T.r[4]); k.step(12, 0x9088);
		if (T.r[4])
		{
			k.step(12, 0x908A);
			T.r[1] = neg(T.r[1]); k.step(12, 0x908C);
		}
		else
			k.step(12, 0x908C);
		T.r[0] = T.r[1]; fl::mov(T.r[0]); k.step(12, 0x908E);
		return T.r[0];
	}
	k.step(12, 0x9090);
	fl::cmp(T.r[4], T.r[5]); k.step(12, 0x9092);
	if (T.r[4] != T.r[5])
	{
		k.step(12, 0x9094);
		T.r[2] = neg(T.r[2]); k.step(12, 0x9096);
	}
	else
		k.step(12, 0x9096);
	for (;;)
	{
		T.r[3] = fl::dec(T.r[3]); k.step(12, 0x9098);
		if (T.r[3] & 0x8000)
		{
			k.step(12, 0x909E);
			break;
		}
		k.step(12, 0x909A);
		T.r[2] = asl(T.r[2]); k.step(12, 0x909C);
		k.step(12, 0x9096);
	}
	T.r[0] = T.r[2]; fl::mov(T.r[0]); k.step(12, 0x90A0);
	return T.r[0];
}

uint16_t divide_ext(const Args &a)
{
	Clock k;
	k.save(0x9034, 5);
	const uint16_t r = divide_body(k, a.u(0), a.u(2), a.u(3));
	k.restore(5);
	k.leave();
	return r;
}

uint16_t divide(const Args &)
{
	Clock k;
	push(0); k.add(24);
	k.call(0x9034, 0x90AC);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	k.add(18);
	k.leave();
	return T.r[0];
}

uint16_t modulo(const Args &)
{
	Clock k;
	push(0x90B2); k.add(24);
	k.call(0x9034, 0x90B6);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	k.add(18);
	k.leave();
	return T.r[0];
}

// F:90BA  park_level(): 0x0F2D when positive; otherwise derived from the table at 0xFCAC
// for the park 0x027A and limited to 9 (10 when 0x0F31 >= 12)
uint16_t park_level(const Args &)
{
	Clock k;
	k.save(0x90BA, 2);
	k.at(0x90BE); T.r[1] = sext8(peekb(0x0F2D)); k.add(24, 12);
	if ((int16_t)T.r[1] > 0)
	{
		T.r[2] = T.r[1];
		k.add(12, 12);
	}
	else
	{
		k.at(0x90C8); T.r[0] = sext8(peekb(0x027A)); k.add(24);
		k.at(0x90CC); T.r[0] = peekb((uint16_t)(0xFCAC + T.r[0])); k.add(27, 18);
		T.r[0] = (uint16_t)(((uint16_t)(T.r[0] - T.r[1] - 1) & 3) - T.r[1]); k.add(12, 12, 18, 12);
		T.r[2] = T.r[0]; k.add(12);
		T.r[1] = 0; k.add(12);
		k.at(0x90E2);
		const bool small = peekb(0x0F31) < 0x0C;
		k.add(30, 12);
		if (small)
		{
			T.r[1] = 1;
			T.r[0] = 9;
			k.add(12, 18, 12);
		}
		else
		{
			T.r[0] = 10;
			k.add(18);
		}
		k.add(12, 12);
		if ((int16_t)T.r[2] > (int16_t)T.r[0])
		{
			T.r[1] = ror(T.r[1]);
			T.r[2] = fC() ? 9 : 10;
			k.add(12 + 12 + 18 + (fC() ? 12 : 0));
		}
	}
	T.r[0] = T.r[2]; k.add(12);
	k.restore(2);
	k.leave();
	return T.r[0];
}

// F:8AB6 / F:8B04 (string number in R0): draw string R0 of the table at 0x0184 (bank A
// 0x2000) with the large (F:8B52) or small (F:8C12) font.  An entry is a screen offset and
// the characters; the last one has bit 7 set.  Bit 7 of the string number draws it with
// code 0x23 instead (erasing).  R1-R3 are preserved (register-save helper F144).
void draw_string(uint16_t put_char)
{
	Clock k;
	k.save((uint16_t)(put_char == 0x8B52 ? 0x8AB6 : 0x8B04), 3);
	k.at(0x8ABA); push(peek(0x01D2)); k.add(36);
	k.at(0x8ABE); poke(0x01D2, 0x2000); k.add(33);
	k.at(0x8AC4); poke(0x1400, peek(0x01D2)); k.add(39);
	const uint16_t base = put_char == 0x8B52 ? 0x8ABA : 0x8B08;   // the two copies of the code
	k.call(0x8068, (uint16_t)(base + 0x14));           // select_video_bank0 (R0 kept)
	T.r[3] = T.r[0];
	T.r[0] = (uint16_t)((T.r[0] << 1) & 0x00FF);
	k.add(12, 12, 18);
	k.at((uint16_t)(base + 0x1C)); T.r[0] = (uint16_t)(T.r[0] + peek(0x0184)); k.add(24);
	k.at((uint16_t)(base + 0x20)); T.r[1] = peek(T.r[0]); k.add(18);
	k.at((uint16_t)(base + 0x22)); T.r[2] = peek(T.r[1]); T.r[1] = (uint16_t)(T.r[1] + 2); k.add(18);
	T.r[2] = (uint16_t)(T.r[2] + 0x2000); k.add(18);
	k.at((uint16_t)(base + 0x28)); poke(0x0186, T.r[2]); k.add(27);
	for (;;)
	{
		k.at((uint16_t)(base + 0x2C)); T.r[0] = sext8(peekb(T.r[1])); k.add(18, 12, 12);
		if (T.r[3] & 0x0080)
		{
			T.r[0] = 0x0023;
			k.add(18);
		}
		k.call(put_char, (uint16_t)(base + 0x3A));
		k.at((uint16_t)(base + 0x3A));
		const uint8_t c = peekb(T.r[1]);
		T.r[1] = (uint16_t)(T.r[1] + 1);
		k.add(18, 12);
		if (c & 0x80) break;
	}
	k.at((uint16_t)(base + 0x3E)); poke(0x01D2, pop()); k.add(33);
	k.at((uint16_t)(base + 0x42)); poke(0x1400, peek(0x01D2)); k.add(39);
	k.restore(3);
	f_nzvc(nzw(T.r[1]));                               // C clear from the last TSTB
	k.leave();
}
void draw_string_large() { draw_string(0x8B52); }
void draw_string_small() { draw_string(0x8C12); }

// F:9F8A: from kick phase 4 the speed comes back from 0x02E8; then the kick state is reset,
// phase 0x01EB = 8, 0x0330 = 4, and the skater takes off (F:9FB4)
uint16_t skater_jump(const Args &)
{
	Clock k;
	k.at(0x9F8A);
	const bool kick4 = peekb(0x01EB) == 4;
	k.add(30, 12);
	if (kick4)
	{
		k.at(0x9F92); poke(0x01B4, peek(0x02E8)); k.add(39);
	}
	k.call(0xE238, 0x9F9C);                            // reset_skater_state
	k.at(0x9F9C); pokeb(0x07B1, 3); k.add(33);
	k.at(0x9FA2); pokeb(0x01EB, 8); k.add(33);
	k.at(0x9FA8); pokeb(0x0330, 4); k.add(33);
	k.call(0x9FB4, 0x9FB2);                            // skater_take_off
	k.leave();
	return T.r[0];
}

// F:AFF0 (assembly): apply the queued sprite size changes: for each of the 4 requests
// (object 0x0989+i, size 0x098D+i) the object's size 0x04D0 is set; a linked object
// (0x0490 bit 1) is unlinked and linked again (F:ACE6, F:AE02).  Bit 5 of 0x0490 is cleared;
// requests whose object has no bit 6 are done (0x0992 counts the pending ones).
void process_sprite_requests()
{
	Clock k;
	k.at(0xAFF0);
	const uint8_t n = peekb(0x0992);
	fl::tstb(n);
	k.add(24, 12);
	if (n)
	{
		T.r[2] = 4; k.add(18);
		do
		{
			k.at(0xAFFA);
			const uint8_t o = peekb((uint16_t)(0x0989 + T.r[2]));
			T.r[1] = sext8(o);
			fl::movb(o);
			k.add(27, 12);
			if (o)
			{
				k.at(0xB000); pokeb((uint16_t)(0x04D0 + T.r[1]), peekb((uint16_t)(0x098D + T.r[2]))); k.add(45);
				T.r[3] = 0x0490; k.add(18);
				T.r[3] = fl::add(T.r[3], T.r[1]); k.add(12);
				k.at(0xB00C);
				const uint8_t f = peekb(T.r[3]);
				k.add(24, 12);
				if (f & 0x02)
				{
					push(0); k.add(24);
					push(T.r[1]); k.add(24);
					k.call(0xACE6, 0xB01A);            // unlink_sprites
					poke((uint16_t)(T.r[6] + 0), 0);   // CLR (SP)... the slot of the first argument
					k.add(21);
					push(T.r[1]); k.add(24);
					k.call(0xAE02, 0xB022);            // link_sprites
					T.r[6] = fl::add(T.r[6], 6); k.add(18);
				}
				k.at(0xB026);
				const uint8_t g = (uint8_t)(peekb(T.r[3]) & ~0x20);
				pokeb(T.r[3], g);
				k.add(27);
				k.at(0xB02A);
				f_nzv(nzb(g & 0x40));
				k.add(24, 12);
				if (!(g & 0x40))
				{
					k.at(0xB030); pokeb(0x0992, fl::decb(peekb(0x0992))); k.add(27);
					k.at(0xB034); pokeb((uint16_t)(0x0989 + T.r[2]), 0); k.add(30);
					fl::clr();
				}
			}
			k.add(18);
		} while (--T.r[2]);
	}
	k.leave();
}

// F:A974  queue_message(int bit, int a, int b): with interrupts masked, the entry count
// 0x0358 goes up (at most 3) and the entry gets the words b (0x0362) and a (0x035A); bit is
// shifted into the mask 0x036D and a 1 into 0x036E unless the text table 0x0184 is 0x5AC8.
// Returns the shifted 0x036E (before that bit).
uint16_t queue_message(const Args &a)
{
	Clock k;
	push(T.r[1]); k.add(24);
	T.r[1] = 0x00E0; k.add(18);
	mtps(0xE0); k.add(24);
	k.at(0xA97C); T.r[0] = sext8(peekb(0x0358)); k.add(24);
	T.r[0]++; k.add(12);
	k.at(0xA982); pokeb(0x0358, (uint8_t)T.r[0]); k.add(27, 18, 12);
	if ((int8_t)(uint8_t)T.r[0] > 3)
	{
		k.at(0xA98C); pokeb(0x0358, 3); k.add(33);
	}
	k.at(0xA992); T.r[0] = (uint16_t)(sext8(peekb(0x0358)) << 1); k.add(24, 12);
	k.at(0xA998); poke((uint16_t)(0x0362 + T.r[0]), a.u(2)); k.add(45);
	k.at(0xA99E); poke((uint16_t)(0x035A + T.r[0]), a.u(1)); k.add(45);
	T.r[1] = 0; k.add(12);
	mtps(0); k.step(30, 0xA9AA);                       // a pending interrupt is taken here
	T.r[0] = 0; k.add(12);
	k.at(0xA9AC); T.r[0] = (uint16_t)(peekb(0x036D) << 1); k.add(24, 12);
	k.at(0xA9B2); pokeb(0x036D, (uint8_t)T.r[0]); k.add(27);
	k.at(0xA9B6); pokeb(0x036D, (uint8_t)(peekb(0x036D) | a.b(0))); k.add(42);
	T.r[0] = 0; k.add(12);
	k.at(0xA9BE); T.r[0] = (uint16_t)(peekb(0x036E) << 1); k.add(24, 12);
	k.at(0xA9C4); pokeb(0x036E, (uint8_t)T.r[0]); k.add(27);
	k.at(0xA9C8);
	const bool other = peek(0x0184) != 0x5AC8;
	k.add(30, 12);
	if (other)
	{
		k.at(0xA9D0); pokeb(0x036E, (uint8_t)(peekb(0x036E) | 1)); k.add(33);
	}
	T.r[1] = pop();
	k.add(18);
	k.leave();
	return T.r[0];
}

// F:A8FE  flush_text_list(): the entries 0..0x0336 of the list at 0x0348 (bank B 0x2800) are
// drawn: a pointer to a value from 0x4000 on goes to F:EAD4, others to A7:4D30; the list is
// emptied (0x0336 = 0xFF) and the banks restored.  R1-R5 preserved (F162).
uint16_t flush_text_list(const Args &)
{
	Clock k;
	k.save(0xA8FE, 5);
	k.at(0xA902); T.r[4] = peek(0x01D2); k.add(24);
	k.at(0xA906); T.r[5] = peek(0x01D4); k.add(24);
	k.at(0xA90A); T.r[3] = sext8(peekb(0x0336)); k.add(24);
	T.r[2] = 0; k.add(12, 12);
	for (;;)
	{
		k.add(12, 12);
		if ((int16_t)T.r[2] > (int16_t)T.r[3]) break;
		sel_b(k, 0xA912, 0x2800);
		T.r[1] = (uint16_t)(T.r[2] << 1); k.add(12, 12);
		k.at(0xA922);
		const uint16_t p = peek((uint16_t)(0x0348 + T.r[1]));
		const bool block = (int16_t)peek(p) >= 0x4000;
		k.add(39, 12);
		if (block)
		{
			k.at(0xA92A); push(peek(peek((uint16_t)(0x0348 + T.r[1])))); k.add(45);
			push(1); k.add(30);
			k.call(0xEAD4, 0xA936);                    // draw_tile_block
			k.add(12);
		}
		else
		{
			sel_a(k, 0xA938, 0x3800);
			k.at(0xA944); push(peek((uint16_t)(0x0348 + T.r[1]))); k.add(39);
			push(1); k.add(30);
			k.call(0x4D30, 0xA950);
		}
		T.r[6] = (uint16_t)(T.r[6] + 4);
		T.r[2]++;
		k.add(24, 12);
	}
	k.at(0xA958); pokeb(0x0336, 0xFF); k.add(33);
	k.at(0xA95E); poke(0x01D2, T.r[4]); k.add(27);
	k.at(0xA962); poke(0x01D4, T.r[5]); k.add(27);
	k.at(0xA966); poke(0x1400, peek(0x01D2)); k.add(39);
	k.at(0xA96C); poke(0x1402, peek(0x01D4)); k.add(39);
	k.restore(5);
	k.leave();
	return T.r[0];
}

// F:91A6  skater_velocity(): x / y velocities of object 0 = speed 0x01B4 times the direction
// factors 0x02E2 / 0x02E4 (F:80FA, middle bytes of the product; the low byte to 0x07F0 /
// 0x07F4), plus the velocity of the object carrying the skater (0x0EA0); 0x0331 = speed
// class (|speed| * 2 >> 8, at most 15).  Returns |speed| * 2.
uint16_t skater_velocity(const Args &)
{
	Clock k;
	k.save(0x91A6, 3);
	k.at(0x91AA); const uint16_t a = T.r[1] = peek(0x01D2); k.add(24);
	k.at(0x91AE); const uint16_t b = T.r[2] = peek(0x01D4); k.add(24);
	k.at(0x91B2); push(peek(0x01B4)); k.add(36);
	k.at(0x91B6); push(peek(0x02E2)); k.add(36);
	k.call(0x80FA, 0x91BE);                     // mul32
	pokeb(0x07F0, peekb(0x0205)); k.add(39);
	k.at(0x91C4); poke(0x04F0, peek(0x0206)); k.add(39);
	k.at(0x91CA); poke(T.r[6], peek(0x01B4)); k.add(33);
	k.at(0x91CE); push(peek(0x02E4)); k.add(36);
	k.call(0x80FA, 0x91D6);
	pokeb(0x07F4, peekb(0x0205)); k.add(39);
	k.at(0x91DC); poke(0x0570, peek(0x0206)); k.add(39);
	k.at(0x91E2); const uint8_t carrier = peekb(0x0EA0); k.add(24, 12);
	if (carrier)
	{
		const uint16_t i = (uint16_t)((sext8(carrier) & 0x003F) << 1);
		k.add(18, 12);
		k.at(0x91EE); poke(0x04F0, (uint16_t)(peek(0x04F0) + peek((uint16_t)(0x04F0 + i)))); k.add(42);
		k.at(0x91F4); poke(0x0570, (uint16_t)(peek(0x0570) + peek((uint16_t)(0x0570 + i)))); k.add(42);
	}
	k.at(0x91FA);
	uint16_t s = peek(0x01B4);
	k.add(24, 12);
	if ((int16_t)s < 0)
	{
		s = (uint16_t)-s;
		k.add(12);
	}
	s = (uint16_t)(s << 1);
	k.add(12, 12, 12, 12, 12);
	const uint8_t cls = (uint8_t)(s >> 8);
	k.at(0x920C); pokeb(0x0331, cls); k.add(27, 18, 12);
	if ((int8_t)cls > 15)
	{
		k.at(0x9216); pokeb(0x0331, 15); k.add(33);
	}
	k.at(0x921C); poke(0x01D2, a); k.add(27);
	k.at(0x9220); poke(0x01D4, b); k.add(27);
	k.at(0x9224); poke(0x1400, peek(0x01D2)); k.add(39);
	k.at(0x922A); poke(0x1402, peek(0x01D4)); k.add(39);
	T.r[6] = (uint16_t)(T.r[6] + 6); k.add(18);         // ADD #6,SP
	T.r[3] = s;
	k.restore(3);
	k.leave();
	return s;
}

// F:843E  place_skater(int z, int y, int x): objects 0-3 at (x, y, z) without velocity, the
// skater's motion state reset (speed, ramp, terrain, gravity, ...), F:E238
uint16_t place_skater(const Args &a)
{
	Clock k;
	push(T.r[1]);
	k.add(24, 18);                                    // MOV R1,-(SP); MOV #3,R1
	for (int i = 3; i >= 0; i--)
	{
		const uint16_t w = (uint16_t)(2 * i);
		T.r[1] = (uint16_t)i;
		T.r[0] = w;
		k.add(12, 12);
		k.at(0x8448); poke((uint16_t)(0x06F0 + w), a.u(2)); k.add(45);
		k.at(0x844E); poke((uint16_t)(0x0730 + w), a.u(1)); k.add(45);
		k.at(0x8454); poke((uint16_t)(0x06B0 + w), a.u(0)); k.add(45);
		k.at(0x845A); poke((uint16_t)(0x04F0 + w), 0); k.add(30);
		k.at(0x845E); poke((uint16_t)(0x0570 + w), 0); k.add(30);
		k.at(0x8462); poke((uint16_t)(0x05B0 + w), 0); k.add(30);
		k.at(0x8466); pokeb((uint16_t)(0x07F0 + i), 0); k.add(30);
		k.at(0x846A); pokeb((uint16_t)(0x07F4 + i), 0); k.add(30);
		k.at(0x846E); pokeb((uint16_t)(0x07F8 + i), 0); k.add(30);
		k.add(12, 12);                                // DEC R1, BGE
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
	T.r[1] = 0xFFFF;
	k.call(0xE238, 0x84D0);                            // reset_skater_state
	T.r[1] = pop();
	k.add(18);
	k.leave();
	return T.r[0];
}

// F:A45A  skater_phase10(int side, int dir): animation from the tables at 0x4EAC (side >= 0) or
// 0x4EBC (bank A 0, bank B 0x0400) for the direction dir; kick state reset, phase 0x01EB and
// 0x0330 = 10, ramp state 2, frame counter saved (B3:7AD2), sound 0x57; banks restored
uint16_t skater_phase10(const Args &a)
{
	Clock k;
	k.save(0xA45A, 2);
	k.at(0xA45E); T.r[1] = peek(0x01D2); k.add(24);
	k.at(0xA462); T.r[2] = peek(0x01D4); k.add(24);
	k.at(0xA466); poke(0x01D2, 0); k.add(27);
	k.at(0xA46A); poke(0x1400, 0); k.add(27);
	sel_b(k, 0xA46E, 0x0400);
	T.r[0] = a.u(1) & 0xFFFE; k.add(27, 12, 12);
	k.add(27, 12);
	if ((int16_t)a.u(0) >= 0)
	{
		k.at(0xA488); poke(0x03B2, peek((uint16_t)(0x4EAC + T.r[0]))); k.add(42);
		k.at(0xA48E); poke(0x03B4, T.r[0]); k.add(27);
		k.at(0xA492); poke(0x03B4, (uint16_t)(peek(0x03B4) + 0x42C0)); k.add(33, 12);
	}
	else
	{
		k.at(0xA49A); poke(0x03B2, peek((uint16_t)(0x4EBC + T.r[0]))); k.add(42);
		k.at(0xA4A0); poke(0x03B4, T.r[0]); k.add(27);
		k.at(0xA4A4); poke(0x03B4, (uint16_t)(peek(0x03B4) + 0x42B0)); k.add(33);
	}
	k.at(0xA4AA); poke(0x02FC, 0); k.add(27);
	k.call(0xE238, 0xA4B2);                            // reset_skater_state
	k.at(0xA4B2); pokeb(0x01EB, 0x0A); k.add(33);
	k.at(0xA4B8); pokeb(0x0330, 0x0A); k.add(33);
	k.at(0xA4BE); pokeb(0x07B1, 0); k.add(27);
	T.r[0] = 3; k.add(18);
	k.at(0xA4C6); pokeb(0x0791, 3); k.add(27);
	k.at(0xA4CA); pokeb(0x0771, 3); k.add(27);
	k.at(0xA4CE); pokeb(0x01EA, 2); k.add(33);
	sel_ab(k, 0xA4D4, 0x3800, 0x3C00);
	k.call(0x7AD2, 0xA4F0);                            // save_frame_counter
	push(0x0057); k.add(30);
	k.call(0x8E78, 0xA4F8);                            // queue_sound
	k.at(0xA4F8); poke(0x01D2, T.r[1]); k.add(27);
	k.at(0xA4FC); poke(0x01D4, T.r[2]); k.add(27);
	k.at(0xA500); poke(0x1400, peek(0x01D2)); k.add(39);
	k.at(0xA506); poke(0x1402, peek(0x01D4)); k.add(39);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	k.add(18);
	k.restore(2);
	k.leave();
	return T.r[0];
}

// Sprite draw lists.  Objects are kept in doubly linked lists (next 0x0808, previous 0x0848,
// list heads 0x0988) in drawing order; each object owns a chain of motion object slots
// (first 0x0888, last 0x0908) whose link fields (motion object RAM 0x3806, bits 1-10) chain
// all sprites of a list together.

// F:ACE6  unlink_sprites(int obj, int list): take obj out of list (the previous object's
// sprites link to the next one's, or end the chain with 0x03F8), clear the list's bits
// (0xFD7C) in 0x0490 and free the object's sprite slots.  Returns the last slot.
uint16_t unlink_sprites(const Args &a)
{
	Clock k;
	k.save(0xACE6, 5);
	T.r[3] = a.u(0); k.add(27);
	k.at(0xACEE); T.r[2] = sext8(peekb((uint16_t)(0x0848 + T.r[3]))); k.add(27);
	k.at(0xACF2); T.r[1] = sext8(peekb((uint16_t)(0x0808 + T.r[3]))); k.add(27, 12, 12);
	bool link_next;
	if (!(T.r[2] & 0x8000))
	{
		k.at(0xACFA); pokeb((uint16_t)(0x0808 + T.r[2]), (uint8_t)T.r[1]); k.add(30);
		T.r[0] = (uint16_t)(T.r[2] << 1); k.add(12, 12);
		k.at(0xAD02); T.r[0] = peek((uint16_t)(0x0908 + T.r[0])); k.add(27);
		T.r[4] = (uint16_t)(0x3806 + T.r[0]); k.add(18, 12, 12, 12);
		if (!(T.r[1] & 0x8000))
		{
			k.at(0xAD10); T.r[0] = peek(T.r[4]) & ~0x07FE; k.add(18, 18);
			T.r[5] = (uint16_t)(T.r[1] << 1); k.add(12, 12);
			k.at(0xAD1A); T.r[0] |= peek((uint16_t)(0x0888 + T.r[5])); k.add(27);
			k.at(0xAD1E); poke(T.r[4], T.r[0]); k.add(21, 12);
			link_next = true;
		}
		else
		{
			k.at(0xAD22); T.r[0] = (uint16_t)((peek(T.r[4]) & ~0x07FE) | 0x03F8); k.add(18, 18, 18);
			k.at(0xAD2C); poke(T.r[4], T.r[0]); k.add(21, 12);
			link_next = false;
		}
	}
	else
	{
		T.r[0] = a.u(1); k.add(27);
		k.at(0xAD34); pokeb((uint16_t)(0x0988 + T.r[0]), (uint8_t)T.r[1]); k.add(30, 12, 12);
		link_next = !(T.r[1] & 0x8000);
	}
	if (link_next)
	{
		k.at(0xAD3C); pokeb((uint16_t)(0x0848 + T.r[1]), (uint8_t)T.r[2]); k.add(30);
	}
	T.r[0] = T.r[3] & 0x001F; k.add(12, 18);
	T.r[4] = a.u(1); k.add(27);
	k.at(0xAD4A); pokeb((uint16_t)(0x0490 + T.r[0]), (uint8_t)(peekb((uint16_t)(0x0490 + T.r[0])) & ~peekb((uint16_t)(0xFD7C + T.r[4])))); k.add(45);
	T.r[3] = (uint16_t)(T.r[3] << 1); k.add(12);
	k.at(0xAD52); T.r[0] = peek((uint16_t)(0x0888 + T.r[3])); k.add(27);
	k.at(0xAD56); T.r[1] = peek((uint16_t)(0x0908 + T.r[3])); k.add(27);
	for (;;)
	{
		k.at(0xAD5A); T.r[2] = peek((uint16_t)(0x3806 + T.r[0])) & 0x07F8; k.add(27, 18);
		k.at(0xAD62); poke((uint16_t)(0x3806 + T.r[0]), 0); k.add(30, 12, 12);
		if (T.r[0] == T.r[1]) break;
		T.r[0] = T.r[2];
		k.add(12, 12);
	}
	const uint16_t r0 = T.r[0];
	k.restore(5);
	k.leave();
	return r0;
}

// F:AE02  link_sprites(int obj, int list): give obj its 0x04D0 sprite slots (free slots from
// slot 1 on, chained; the last one ends with 0x03F9), set the list's bits (0xFD7C, 0xFD7E) in
// 0x0490, clear bit 2 of 0x04B0, and insert obj into list before the first object it is in
// front of (F:AD70), else at the end.  R1-R5 preserved (F162).
uint16_t link_sprites(const Args &a)
{
	Clock k;
	k.save(0xAE02, 5);
	T.r[4] = a.u(0); k.add(27);
	T.r[0] = T.r[4] & 0x001F; k.add(12, 18);
	T.r[1] = a.u(1); k.add(27);
	k.at(0xAE14); T.r[2] = sext8(peekb((uint16_t)(0x0490 + T.r[0]))); k.add(27);
	k.at(0xAE18); T.r[2] = (uint16_t)((T.r[2] | peekb((uint16_t)(0xFD7C + T.r[1]))) & 0x00FF); k.add(27, 18);
	k.at(0xAE20); pokeb((uint16_t)(0x0490 + T.r[0]), peekb((uint16_t)(0xFD7E + T.r[1]))); k.add(45);
	k.at(0xAE26); pokeb((uint16_t)(0x0490 + T.r[0]), (uint8_t)(peekb((uint16_t)(0x0490 + T.r[0])) | (uint8_t)T.r[2])); k.add(30);
	k.at(0xAE2A); pokeb((uint16_t)(0x04B0 + T.r[0]), (uint8_t)(peekb((uint16_t)(0x04B0 + T.r[0])) & ~0x04)); k.add(36);
	T.r[1] = 0; k.add(12);
	T.r[2] = 8; k.add(18);
	T.r[3] = 0; k.add(12);
	k.at(0xAE38); T.r[3] = peekb((uint16_t)(0x04D0 + T.r[0])); k.add(27, 12, 12);
	if ((int16_t)T.r[3] > 0)
		do
		{
			for (;;)
			{
				k.at(0xAE40);
				const bool used = peek((uint16_t)(0x3806 + T.r[2])) != 0;
				k.add(27, 12);
				if (!used) break;
				T.r[2] = (uint16_t)(T.r[2] + 8);
				k.add(18, 12);
			}
			k.add(12, 12);
			if (!T.r[1])
			{
				T.r[1] = T.r[2];
				k.add(12, 12);
			}
			else
			{
				k.at(0xAE54); poke((uint16_t)(0x3806 + T.r[0]), T.r[2]); k.add(30);
			}
			k.at(0xAE58); poke((uint16_t)(0x3800 + T.r[2]), 0); k.add(30);
			k.at(0xAE5C); poke((uint16_t)(0x3C00 + T.r[2]), 0); k.add(30);
			k.at(0xAE60); poke((uint16_t)(0x3802 + T.r[2]), 0); k.add(30);
			T.r[0] = T.r[2];
			T.r[2] = (uint16_t)(T.r[2] + 8);
			k.add(12, 18, 18);
		} while (--T.r[3]);
	T.r[3] = (uint16_t)(T.r[4] << 1); k.add(12, 12);
	k.at(0xAE70); poke((uint16_t)(0x0888 + T.r[3]), T.r[1]); k.add(30);
	T.r[2] = T.r[0]; k.add(12);
	k.at(0xAE76); poke((uint16_t)(0x0908 + T.r[3]), T.r[2]); k.add(30);
	k.at(0xAE7A); poke((uint16_t)(0x3806 + T.r[0]), 0x03F9); k.add(36);
	T.r[2] = T.r[4]; k.add(12);
	T.r[3] = 0xFFFF; k.add(18);
	T.r[1] = a.u(1); k.add(27);
	k.at(0xAE8A); T.r[1] = sext8(peekb((uint16_t)(0x0988 + T.r[1]))); k.add(27);
	const uint16_t list = a.u(1);
	// make the link field of the last sprite of object a point to the first one of object b
	auto chain = [&k](uint16_t a_obj, uint16_t b_obj, uint16_t pc) {
		T.r[0] = (uint16_t)(a_obj << 1); k.add(12, 12);
		k.at((uint16_t)(pc + 4)); T.r[4] = peek((uint16_t)(0x0908 + T.r[0])); k.add(27);
		k.at((uint16_t)(pc + 8)); T.r[0] = peek((uint16_t)(0x3806 + T.r[4])) & ~0x07FE; k.add(27, 18);
		T.r[5] = (uint16_t)(b_obj << 1); k.add(12, 12);
		k.at((uint16_t)(pc + 0x14)); T.r[0] |= peek((uint16_t)(0x0888 + T.r[5])); k.add(27);
		k.at((uint16_t)(pc + 0x18)); poke((uint16_t)(0x3806 + T.r[4]), T.r[0]); k.add(30);
	};
	for (;;)
	{
		k.add(12);
		if (T.r[1] & 0x8000)
		{
			// at the end of the list
			k.add(12, 12);
			if (!(T.r[3] & 0x8000))
			{
				k.at(0xAEFE); pokeb((uint16_t)(0x0808 + T.r[3]), (uint8_t)T.r[2]); k.add(30);
				chain(T.r[3], T.r[2], 0xAF02);
				k.add(12);
			}
			else
			{
				T.r[0] = list; k.add(27);
				k.at(0xAF24); pokeb((uint16_t)(0x0988 + T.r[0]), (uint8_t)T.r[2]); k.add(30);
			}
			k.at(0xAF28); pokeb((uint16_t)(0x0848 + T.r[2]), (uint8_t)T.r[3]); k.add(30);
			k.at(0xAF2C); pokeb((uint16_t)(0x0808 + T.r[2]), (uint8_t)T.r[1]); k.add(30);
			break;
		}
		T.r[0] = T.r[2]; k.add(12);
		k.call(0xAD70, 0xAE96);                        // sprite_in_front
		T.r[0] = ror(T.r[0]);
		k.add(12, 12);
		if (fC())
		{
			// before R1
			k.add(12, 12);
			if (!(T.r[3] & 0x8000))
			{
				k.at(0xAE9E); pokeb((uint16_t)(0x0808 + T.r[3]), (uint8_t)T.r[2]); k.add(30);
				chain(T.r[3], T.r[2], 0xAEA2);
				k.add(12);
			}
			else
			{
				T.r[0] = list; k.add(27);
				k.at(0xAEC4); pokeb((uint16_t)(0x0988 + T.r[0]), (uint8_t)T.r[2]); k.add(30);
			}
			k.at(0xAEC8); pokeb((uint16_t)(0x0848 + T.r[1]), (uint8_t)T.r[2]); k.add(30);
			k.at(0xAECC); pokeb((uint16_t)(0x0808 + T.r[2]), (uint8_t)T.r[1]); k.add(30);
			chain(T.r[2], T.r[1], 0xAED0);
			k.at(0xAEEC); pokeb((uint16_t)(0x0848 + T.r[2]), (uint8_t)T.r[3]); k.add(30);
			break;
		}
		T.r[3] = T.r[1]; k.add(12);
		k.at(0xAEF4); T.r[1] = sext8(peekb((uint16_t)(0x0808 + T.r[1]))); k.add(27, 12);
	}
	const uint16_t r0 = T.r[0];
	k.restore(5);
	k.leave();
	return r0;
}

// F:AF32 (assembly): one bubble pass over the draw list 0x0993: an object that F:AD70 does
// not put in front of its successor swaps places with it (list and sprite links updated).
// A word on the stack holds the doubled index of the object moving back.
void sort_sprite_list()
{
	Clock k;
	T.r[6] = (uint16_t)(T.r[6] - 2);                    // TST -(SP): the local word
	k.add(21);
	k.at(0xAF34); T.r[0] = sext8(peekb(0x0993)); k.add(24);
	k.at(0xAF38); T.r[3] = sext8(peekb((uint16_t)(0x0988 + T.r[0]))); k.add(27, 12);
	if (!(T.r[3] & 0x8000))
	{
		k.at(0xAF3E); T.r[1] = sext8(peekb((uint16_t)(0x0808 + T.r[3]))); k.add(27);
		for (;;)
		{
			k.add(12);
			if (T.r[1] & 0x8000) break;
			T.r[0] = T.r[3]; k.add(12);
			k.call(0xAD70, 0xAF4A);                    // sprite_in_front
			T.r[0] = ror(T.r[0]);
			k.add(12, 12);
			if (!fC())
			{
				// R1 moves in front of R3
				k.at(0xAF4E); T.r[2] = sext8(peekb((uint16_t)(0x0848 + T.r[3]))); k.add(27, 12);
				if (!(T.r[2] & 0x8000))
				{
					k.at(0xAF54); pokeb((uint16_t)(0x0808 + T.r[2]), (uint8_t)T.r[1]); k.add(30);
					T.r[0] = (uint16_t)(T.r[2] << 1); k.add(12, 12);
					k.at(0xAF5C); T.r[4] = peek((uint16_t)(0x0908 + T.r[0])); k.add(27);
					k.at(0xAF60); T.r[0] = peek((uint16_t)(0x3806 + T.r[4])) & ~0x07FE; k.add(27, 18);
					T.r[5] = (uint16_t)(T.r[1] << 1); k.add(12, 12);
					k.at(0xAF6C); T.r[0] |= peek((uint16_t)(0x0888 + T.r[5])); k.add(27);
					k.at(0xAF70); poke((uint16_t)(0x3806 + T.r[4]), T.r[0]); k.add(30, 12);
				}
				else
				{
					k.at(0xAF76); T.r[0] = sext8(peekb(0x0993)); k.add(24);
					k.at(0xAF7A); pokeb((uint16_t)(0x0988 + T.r[0]), (uint8_t)T.r[1]); k.add(30);
				}
				k.at(0xAF7E); pokeb((uint16_t)(0x0848 + T.r[1]), (uint8_t)T.r[2]); k.add(30);
				k.at(0xAF82); T.r[2] = sext8(peekb((uint16_t)(0x0808 + T.r[1]))); k.add(27);
				poke(T.r[6], T.r[3]); k.add(21);
				poke(T.r[6], (uint16_t)(T.r[3] << 1)); k.add(21);
				T.r[4] = peek(T.r[6]); k.add(18);
				k.at(0xAF8C); T.r[0] = peek((uint16_t)(0x0908 + T.r[4])); k.add(27);
				T.r[5] = (uint16_t)(0x3806 + T.r[0]); k.add(18, 12, 12, 12);
				if (!(T.r[2] & 0x8000))
				{
					k.at(0xAF9A); pokeb((uint16_t)(0x0848 + T.r[2]), (uint8_t)T.r[3]); k.add(30);
					k.at(0xAF9E); T.r[0] = peek(T.r[5]) & ~0x07FE; k.add(18, 18);
					T.r[4] = (uint16_t)(T.r[2] << 1); k.add(12, 12);
					k.at(0xAFA8); T.r[0] |= peek((uint16_t)(0x0888 + T.r[4])); k.add(27);
					k.at(0xAFAC); poke(T.r[5], T.r[0]); k.add(21, 12);
				}
				else
				{
					k.at(0xAFB0); T.r[4] = (uint16_t)((peek(T.r[5]) & ~0x07FE) | 0x03F8); k.add(18, 18, 18);
					k.at(0xAFBA); poke(T.r[5], T.r[4]); k.add(21);
				}
				k.at(0xAFBC); pokeb((uint16_t)(0x0808 + T.r[3]), (uint8_t)T.r[2]); k.add(30);
				k.at(0xAFC0); pokeb((uint16_t)(0x0808 + T.r[1]), (uint8_t)T.r[3]); k.add(30);
				T.r[5] = (uint16_t)(T.r[1] << 1); k.add(12, 12);
				k.at(0xAFC8); T.r[4] = peek((uint16_t)(0x0908 + T.r[5])); k.add(27);
				k.at(0xAFCC); T.r[5] = peek((uint16_t)(0x3806 + T.r[4])) & ~0x07FE; k.add(27, 18);
				T.r[0] = peek(T.r[6]); k.add(18);
				k.at(0xAFD6); T.r[5] |= peek((uint16_t)(0x0888 + T.r[0])); k.add(27);
				k.at(0xAFDA); poke((uint16_t)(0x3806 + T.r[4]), T.r[5]); k.add(30);
				k.at(0xAFDE); pokeb((uint16_t)(0x0848 + T.r[3]), (uint8_t)T.r[1]); k.add(30);
				T.r[1] = T.r[3]; k.add(12);
			}
			T.r[3] = T.r[1]; k.add(12);
			k.at(0xAFE6); T.r[1] = sext8(peekb((uint16_t)(0x0808 + T.r[1]))); k.add(27, 12);
		}
	}
	fl::tst(peek(T.r[6]));
	T.r[6] = (uint16_t)(T.r[6] + 2);
	k.add(18);
	k.leave();
}

// F:944C (assembly): the probed cells 1-3 (0x0302-0x0306) become tile numbers (low 10 bits
// plus the tile bank base 0x01D6, or 0x01D8 for bit 10); 0x032A / 0x032B = the attribute byte
// (0x6000) of cell 2; then each cell holds its terrain code: the byte at 0x4000 + tile, or
// with mode 0x032F == 2 a negative attribute byte without bit 7
void cell_terrain_codes()
{
	Clock k;
	sel_ab(k, 0x944C, 0x0800, 0x0C00);
	T.r[3] = 3; k.add(18);
	do
	{
		T.r[0] = (uint16_t)(T.r[3] << 1); k.add(12, 12);
		k.at(0x946C); T.r[1] = peek((uint16_t)(0x0300 + T.r[0])); k.add(27, 18, 12);
		const bool hi = (T.r[1] & 0x0400) != 0;
		T.r[2] = T.r[1] & 0x03FF; k.add(12, 18);
		k.at(hi ? 0x947C : 0x9488);
		T.r[2] = (uint16_t)(T.r[2] + peek(hi ? 0x01D8 : 0x01D6));
		k.add(24, (hi ? 12 : 0));
		k.at(0x948C); poke((uint16_t)(0x0300 + T.r[0]), T.r[2]); k.add(30, 18);
	} while (--T.r[3]);
	T.r[0] = 2; k.add(18, 12);
	k.at(0x9498); T.r[0] = peek(0x0304); k.add(27);
	k.at(0x949C); pokeb(0x032A, peekb((uint16_t)(0x6000 + T.r[0]))); k.add(42);
	k.at(0x94A2); T.r[0] = peek(0x0304); k.add(24);
	k.at(0x94A6); pokeb(0x032B, peekb((uint16_t)(0x6000 + T.r[0]))); k.add(42);
	T.r[2] = 3; k.add(18);
	do
	{
		T.r[0] = fl::add(asl(T.r[2]), 0x0300); k.add(12, 12, 18);
		k.at(0x94B8); T.r[1] = peek(T.r[0]); k.add(18);
		k.at(0x94BA);
		const uint8_t mode = peekb(0x032F);
		fl::cmpb(mode, 2);
		k.add(30, 12);
		bool attr = false;
		if (mode == 2)
		{
			k.at(0x94C2);
			const uint8_t v = peekb((uint16_t)(0x6000 + T.r[1]));
			fl::tstb(v);
			attr = (v & 0x80) != 0;
			k.add(27, 12);
		}
		if (attr)
		{
			k.at(0x94D0); pokeb(T.r[0], peekb((uint16_t)(0x6000 + T.r[1]))); k.add(36);
			k.at(0x94D4);
			const uint16_t v = peek(T.r[0]) & 0x007F;
			poke(T.r[0], v);
			fl::mov(v);
			k.add(27);
		}
		else
		{
			k.at(0x94C8); T.r[3] = sext8(peekb((uint16_t)(0x4000 + T.r[1]))); k.add(27);
			k.at(0x94CC); poke(T.r[0], T.r[3]); fl::mov(T.r[3]); k.add(21, 12);
		}
		k.add(18);
	} while (--T.r[2]);
	k.leave();
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
	k.at(0x9382); T.r[3] = (uint16_t)(sext8(peekb(0x02E3)) << 3); k.add(24, 12, 12, 12);
	k.at(0x938C); T.r[0] = sext8(peekb(0x02E5)); k.add(24);
	k.at(0x9390); T.r[1] = sext8(peekb(0x02E7)); k.add(24);
	T.r[0] = (uint16_t)((T.r[0] + T.r[1]) << 3); k.add(12, 12, 12, 12);
	T.r[5] = T.r[3]; k.add(12);
	k.at(0x939E); poke(0x0212, T.r[0]); k.add(27);
	k.at(0x93A2); poke(0x0220, (uint16_t)(peek(0x0220) + T.r[3])); k.add(27);
	k.at(0x93A6); poke(0x021E, (uint16_t)(peek(0x021E) + T.r[0])); k.add(27);
	k.at(0x93AA); pokeb(0x032E, 0); k.add(27);
	k.at(0x93AE); pokeb(0x0329, 0xFD); k.add(33);
	T.r[4] = 3; k.add(18);
	do
	{
		k.at(0x93B8); T.r[1] = peek(0x0220) & 0xFF00; k.add(24, 18);
		k.at(0x93C0); T.r[0] = peek(0x021E); k.add(24);
		T.r[0] = (uint16_t)((int16_t)T.r[0] >> 1) & 0x3F00; k.add(12, 18);
		T.r[2] = (uint16_t)(0x3F00 - T.r[0]); k.add(18, 12);
		k.at(0x93D0); poke(0x0218, T.r[2]); k.add(27);
		T.r[0] = T.r[2]; k.add(12);
		k.call(0x8078, 0x93DA);                 // select_video_bank
		T.r[2] &= 0x1F00;
		T.r[1] &= 0xFE00;
		T.r[3] = (uint16_t)((T.r[1] >> 8) + T.r[2]);
		k.add(18, 18, 12, 12, 12, 12);
		k.at(0x93EA); T.r[3] = peek((uint16_t)(0x2000 + T.r[3])); k.add(27, 18, 12);
		if (T.r[3] & 0x4000)
		{
			k.at(0x93F4); poke(0x021C, peek(0x021E)); k.add(39);
			k.at(0x93FA); poke(0x0214, T.r[1]); k.add(27);
			k.at(0x93FE); poke(0x021A, peek(0x0218)); k.add(39);
		}
		T.r[0] = sext8((uint8_t)(T.r[3] >> 8)); k.add(12, 12, 12, 12);
		k.at(0x940C); pokeb(0x032E, (uint8_t)(peekb(0x032E) | (uint8_t)T.r[0])); k.add(27);
		T.r[0] = (uint16_t)(T.r[4] << 1); k.add(12, 12);
		k.at(0x9414); poke((uint16_t)(0x0300 + T.r[0]), T.r[3]); k.add(30);
		k.at(0x9418); poke((uint16_t)(0x0306 + T.r[0]), T.r[2]); k.add(30);
		k.at(0x941C); poke((uint16_t)(0x030C + T.r[0]), T.r[1]); k.add(30);
		k.at(0x9420); poke((uint16_t)(0x0312 + T.r[0]), peek(0x0218)); k.add(42);
		k.at(0x9426); poke(0x0220, (uint16_t)(peek(0x0220) - T.r[5])); k.add(27);
		k.at(0x942A); poke(0x021E, fl::sub(peek(0x021E), peek(0x0212))); k.add(39, 18);
	} while (--T.r[4]);
	k.at(0x9432); poke(0x021E, peek(0x030A)); k.add(39);
	k.at(0x9438); poke(0x0220, peek(0x0310)); k.add(39);
	k.at(0x943E); poke(0x0218, peek(0x0316)); k.add(39);
	k.at(0x9444);
	const uint8_t v = (uint8_t)(peekb(0x032E) & ~0x3F);
	pokeb(0x032E, v);
	f_nzv(nzb(v));
	k.add(33);
	k.leave();
}

// F:9B82  skater_motion_params(): index = direction 0x031D, moved into the slope tables by
// the terrain 0x0327 (bank B 0x2C00); direction factors 0x02E2-0x02E6 from the tables at
// 0x6048 / 0x6588 / 0x6AC8; on a ramp (0x01EA) the gravity 0x02EE (0x7668, halved on park 3
// below the speed limit 0x7C68), otherwise the jump values 0x02EA (0x7008) / 0x02EC (0x7228).
// Bank B restored; returns the index.  R1-R4 preserved (F152).
uint16_t skater_motion_params(const Args &)
{
	Clock k;
	k.save(0x9B82, 4);
	k.at(0x9B86); T.r[4] = peek(0x01D4); k.add(24);
	sel_b(k, 0x9B8A, 0x2C00);
	k.at(0x9B96); T.r[0] = sext8(peekb(0x031D)); k.add(24);
	k.at(0x9B9A); T.r[3] = sext8(peekb(0x0327)); k.add(24, 18, 12);
	if (T.r[3] & 7)
	{
		T.r[1] = (uint16_t)(7 & ~T.r[3]); k.add(18, 12, 12);
		if (T.r[1])
		{
			// a slope: its row of the table at 0x79E8 plus 16 per step
			T.r[1] = (uint16_t)(((int16_t)T.r[3] >> 3) << 1); k.add(12, 12, 12, 12, 12);
			T.r[2] = (uint16_t)((T.r[3] & 7) << 4); k.add(12, 18, 12, 12, 12, 12);
			k.at(0x9BC4); T.r[2] = (uint16_t)(T.r[2] + peek((uint16_t)(0x79E8 + T.r[1]))); k.add(27);
			T.r[2] = (uint16_t)(T.r[2] + T.r[0]); k.add(12);
			T.r[0] = T.r[2]; k.add(12, 12);
		}
		else
		{
			T.r[1] = (uint16_t)(((T.r[3] & 0x0038) << 1) + T.r[0]); k.add(12, 18, 12, 12);
			T.r[0] = (uint16_t)(T.r[1] + 0x0220); k.add(12, 18);
		}
	}
	T.r[1] = (uint16_t)(T.r[0] << 1); k.add(12, 12);
	k.at(0x9BE2); poke(0x02E2, peek((uint16_t)(0x6048 + T.r[1]))); k.add(42);
	k.at(0x9BE8); poke(0x02E4, peek((uint16_t)(0x6588 + T.r[1]))); k.add(42);
	k.at(0x9BEE); poke(0x02E6, peek((uint16_t)(0x6AC8 + T.r[1]))); k.add(42);
	k.at(0x9BF4);
	const bool ramp = peekb(0x01EA) != 0;
	k.add(24, 12);
	if (ramp)
	{
		k.at(0x9BFA); T.r[2] = sext8(peekb((uint16_t)(0x7668 + T.r[0]))); k.add(27);
		k.at(0x9BFE); poke(0x02EE, T.r[2]); k.add(27);
		k.at(0x9C02);
		const bool park3 = peekb(0x0F59) == 3;
		k.add(30, 12);
		if (park3)
		{
			k.add(12, 12);
			if (T.r[2] & 0x8000)
			{
				k.at(0x9C0E);
				const bool moving = (int16_t)peek(0x01B4) > 0;
				k.add(24, 12);
				if (moving)
				{
					k.at(0x9C14); T.r[1] = (uint16_t)(sext8(peekb(0x025C)) << 1); k.add(24, 12);
					k.at(0x9C1A);
					const bool slow = (int16_t)peek(0x01B4) < (int16_t)peek((uint16_t)(0x7C68 + T.r[1]));
					k.add(39, 12);
					if (slow)
					{
						k.at(0x9C22); poke(0x02EE, (uint16_t)((int16_t)peek(0x02EE) >> 1)); k.add(27, 12);
					}
				}
			}
		}
	}
	else
	{
		T.r[2] = (uint16_t)(7 & ~T.r[3]); k.add(18, 12, 12);
		if (T.r[2])
		{
			k.at(0x9C30); pokeb(0x02EA, peekb((uint16_t)(0x7008 + T.r[0]))); k.add(42);
			k.at(0x9C36); pokeb(0x02EB, 0); k.add(27);
			k.at(0x9C3A);
			const bool any = peek(0x02EA) != 0;
			k.add(24, 12);
			if (any)
			{
				k.at(0x9C40); poke(0x02EA, (uint16_t)(peek(0x02EA) + 1)); k.add(27);
			}
			k.at(0x9C44); poke(0x02EC, peek((uint16_t)(0x7228 + T.r[1]))); k.add(42, 12);
		}
		else
		{
			k.at(0x9C4C); poke(0x02EA, 0); k.add(27);
			k.at(0x9C50); poke(0x02EC, 0); k.add(27);
		}
	}
	k.at(0x9C54); poke(0x01D4, T.r[4]); k.add(27);
	k.at(0x9C58); poke(0x1402, T.r[4]); k.add(27);
	const uint16_t r0 = T.r[0];
	k.restore(4);
	k.leave();
	return r0;
}

// F:9FB4  skater_take_off(): leave the ramp (0x01EA = 0): skater state saved (B3:7A9E) unless
// on park 4, a negative speed is turned into the opposite direction, speed remembered in 0x02E8,
// motion values (F:9B82, F:91A6), speed and z velocity scaled by the jump factors 0x02EA /
// 0x02EC (F:80FA), air timers cleared, gravity 0x02EE set, sound 0x4A; banks restored.
// R1-R3 preserved (F144).
uint16_t skater_take_off(const Args &)
{
	Clock k;
	k.save(0x9FB4, 3);
	k.at(0x9FB8); T.r[1] = peek(0x01D2); k.add(24);
	k.at(0x9FBC); T.r[2] = peek(0x01D4); k.add(24);
	k.at(0x9FC0); pokeb(0x01EA, 0); k.add(27);
	sel_ab(k, 0x9FC4, 0x3800, 0x3C00);
	k.at(0x9FDC);
	const bool park4 = peekb(0x0F59) == 4;
	k.add(30, 12);
	if (!park4) k.call(0x7A9E, 0x9FE8);                // snapshot_skater
	k.at(0x9FE8);
	const bool back = (int16_t)peek(0x01B4) < 0;
	k.add(24, 12);
	if (back)
	{
		T.r[3] = 8; k.add(18);
		k.at(0x9FF2); T.r[0] = sext8(peekb(0x031D)); k.add(24);
		T.r[3] ^= T.r[0]; k.add(12);
		k.at(0x9FF8); pokeb(0x031D, (uint8_t)T.r[3]); k.add(27);
		k.at(0x9FFC); poke(0x01B4, (uint16_t)-peek(0x01B4)); k.add(27);
	}
	k.at(0xA000); poke(0x02E8, peek(0x01B4)); k.add(39);
	k.call(0x9B82, 0xA00A);                            // skater_motion_params
	k.call(0x91A6, 0xA00E);                            // skater_velocity
	k.at(0xA00E);
	const bool carried = peekb(0x0EA0) != 0;
	k.add(24, 12);
	if (carried) k.call(0xD742, 0xA018);
	k.at(0xA018); push(peek(0x01B4)); k.add(36);
	k.at(0xA01C); push(peek(0x02EA)); k.add(36);
	k.call(0x80FA, 0xA024);                            // mul32
	k.at(0xA024); pokeb(0x01B5, peekb(0x0206)); k.add(39);
	k.at(0xA02A); pokeb(0x01B4, peekb(0x0205)); k.add(39);
	k.at(0xA030); poke(T.r[6], peek(0x01B4)); k.add(33);
	k.at(0xA034); push(peek(0x02EC)); k.add(36);
	k.call(0x80FA, 0xA03C);
	k.at(0xA03C); pokeb(0x07F8, peekb(0x0205)); k.add(39);
	k.at(0xA042); poke(0x05B0, peek(0x0206)); k.add(39);
	k.at(0xA048);
	const bool carried2 = peekb(0x0EA0) != 0;
	k.add(24, 12);
	if (carried2) k.call(0xD6BE, 0xA052);
	k.at(0xA052); pokeb(0x0323, 0); k.add(27);
	k.at(0xA056); poke(0x02F6, peek(0x01C6)); k.add(39);
	k.at(0xA05C); poke(0x02F6, (uint16_t)(peek(0x02F6) << 1)); k.add(27);
	k.at(0xA060); poke(0x02F4, 0); k.add(27);
	k.at(0xA064); poke(0x02F2, 0); k.add(27);
	k.at(0xA068); poke(0x02EE, t11rt::g_patch_gravity); k.add(33);   // patch point (default 12)
	poke(T.r[6], 0x004A); k.add(27);
	k.call(0x8E78, 0xA076);                            // queue_sound
	k.at(0xA076); poke(0x01D2, T.r[1]); k.add(27);
	k.at(0xA07A); poke(0x01D4, T.r[2]); k.add(27);
	k.at(0xA07E); poke(0x1400, peek(0x01D2)); k.add(39);
	k.at(0xA084); poke(0x1402, peek(0x01D4)); k.add(39);
	T.r[6] = (uint16_t)(T.r[6] + 6);
	k.add(18);
	k.restore(3);
	k.leave();
	return T.r[0];
}

// F:9236 (assembly): the push force: when the button bit 7 of 0x0193 changes (0x0333) the
// boost 0x02FE grows by the equipment (0x7C80) and speed class (0x7CB0) values; the
// ability flag 0x0320 is updated; every 16th frame the boost decays (at least 0x180) and the
// push 0x02FC moves towards the speed limited by the equipment (0x7C88, + 0x80), or falls by
// 0x18 / rises by 6.  Bank B is left at 0x2C00.
void update_push()
{
	Clock k;
	sel_b(k, 0x9236, 0x2C00);
	k.at(0x9242); T.r[0] = sext8(peekb(0x0193)) & 0x0080; k.add(24, 18);
	T.r[1] = 0; k.add(12);
	k.at(0x924C); T.r[1] = peekb(0x0333); k.add(24, 12, 12);
	if (T.r[0] != T.r[1])
	{
		// the button changed: boost by the equipment and the speed class
		k.at(0x9254); pokeb(0x0333, peekb(0x0193)); k.add(39);
		k.at(0x925A); pokeb(0x0333, (uint8_t)(peekb(0x0333) & 0x80)); k.add(33);
		k.at(0x9260); T.r[0] = (uint16_t)(sext8(peekb(0x025F)) << 1); k.add(24, 12);
		k.at(0x9266); poke(0x02FE, (uint16_t)(peek(0x02FE) + peek((uint16_t)(0x7C80 + T.r[0])))); k.add(42);
		k.at(0x926C); T.r[0] = sext8(peekb(0x0331)); k.add(24);
		T.r[1] = 0; k.add(12);
		k.at(0x9272); T.r[1] = peekb((uint16_t)(0x7CB0 + T.r[0])); k.add(27);
		k.at(0x9276); poke(0x02FE, (uint16_t)(peek(0x02FE) + T.r[1])); k.add(27);
	}
	k.at(0x927A); T.r[0] = (uint16_t)(sext8(peekb(0x025F)) << 1); k.add(24, 12);
	k.at(0x9280); T.r[1] = peek((uint16_t)(0x7C88 + T.r[0])); k.add(27);
	k.at(0x9284); T.r[3] = peek(0x02FC); k.add(24);
	T.r[5] = 0; k.add(12, 12, 12);
	if ((int16_t)T.r[3] > (int16_t)T.r[1])
	{
		T.r[5] = 1;
		T.r[0] = 0;
		k.add(12, 12, 12);
	}
	else
	{
		T.r[0] = 0x0040; k.add(18);
	}
	T.r[4] = 0; k.add(12);
	k.at(0x929A); T.r[4] = peekb(0x0333); k.add(24, 12, 12);
	if (T.r[4])
	{
		T.r[2] = T.r[0]; k.add(12);
		k.at(0x92A4); T.r[2] = (uint16_t)(T.r[2] + peek(0x01B4)); k.add(24, 12, 12);
		if ((int16_t)T.r[3] > (int16_t)T.r[2])
		{
			k.at(0x92AC);
			bool clear = peekb(0x0F59) != 3;
			k.add(30, 12);
			if (!clear)
			{
				k.at(0x92B4);
				const bool low = (int16_t)peek(0x0ED0) <= 2;
				k.add(30, 12);
				clear = !low;
				if (low)
				{
					k.at(0x92BC); clear = (int16_t)peek(0x01B4) >= (int16_t)0xFE80; k.add(30, 12);
				}
			}
			if (clear)
			{
				k.at(0x92C4); pokeb(0x0320, 0); k.add(27);
			}
		}
	}
	bool carry = false;                                // from CMP R2,#4
	bool able = true;
	k.at(0x92C8);
	const bool kick = peekb(0x0330) == 4;
	k.add(30, 12);
	if (!kick)
	{
		k.at(0x92D0);
		const bool air = peekb(0x031E) != 0;
		k.add(24, 12);
		if (!air)
		{
			k.at(0x92D6); T.r[2] = sext8(peekb(0x0327)) & 7; k.add(24, 18, 18, 12);
			carry = T.r[2] < 4;
			able = !carry;
		}
	}
	if (able)
	{
		k.at(0x92E4); pokeb(0x0320, 1); k.add(33);
	}
	k.at(0x92EA);
	const uint16_t frame = peek(0x01C4) & 0x000F;
	k.add(30, 12);
	if (frame)
	{
		f_nzvc(nzw(frame) | (carry ? CF : 0));
		k.leave();
		return;
	}
	// every 16th frame
	k.at(0x92F2); poke(0x02FE, (uint16_t)((int16_t)peek(0x02FE) >> 1)); k.add(27);
	k.at(0x92F6);
	const bool small = (int16_t)peek(0x02FE) < 0x0180;
	k.add(30, 12);
	if (small)
	{
		k.at(0x92FE); poke(0x02FE, 0x0180); k.add(33);
	}
	k.at(0x9304); T.r[0] = peek(0x01B4); k.add(24, 12, 12);
	if ((int16_t)T.r[0] > (int16_t)T.r[1])
	{
		T.r[0] = T.r[1]; k.add(12);
	}
	k.at(0x930E);
	const bool less = (int16_t)T.r[0] < (int16_t)peek(0x02FE);
	k.add(24, 12);
	if (less)
	{
		k.at(0x9314); T.r[0] = peek(0x02FE); k.add(24);
	}
	T.r[1] = (uint16_t)(T.r[1] + 0x0080); k.add(18, 12, 12);
	if ((int16_t)T.r[0] > (int16_t)T.r[1])
	{
		T.r[0] = T.r[1]; k.add(12);
	}
	k.add(12, 12);
	if ((int16_t)T.r[3] < (int16_t)T.r[0])
	{
		T.r[0] = asr((uint16_t)(T.r[0] - T.r[3])); k.add(12, 12);
		k.at(0x932A); T.r[0] = fl::add(T.r[0], peek(0x02FC)); k.add(24);
		k.at(0x932E); poke(0x02FC, T.r[0]); fl::mov(T.r[0]); k.add(27);
		k.leave();
		return;
	}
	bool down = true;
	k.add(12, 12);
	if (T.r[4])
	{
		fl::tst(T.r[4]);
		T.r[5] = ror(T.r[5]);
		down = fC();
		k.add(12, 12);
	}
	if (down)
	{
		k.at(0x933C); poke(0x02FC, fl::sub(peek(0x02FC), 0x0018));
	}
	else
	{
		k.at(0x9344); poke(0x02FC, fl::add(peek(0x02FC), 0x0006));
	}
	k.add(33);
	k.leave();
}

// F:899C (interrupt, vertical blank during play): palette fade (F:DBE2) and the pending
// palette routine (F:DDEC), banked palette and sound level updates, watchdog, frame counters
// and acknowledges, timers 0x0F18 / 0x0F22, scroll registers and input, the sound queue;
// the text pointer, banks and video window are put back
void irq_vblank_play()
{
	Clock k;
	for (int i = 0; i <= 5; i++) push(T.r[i]);
	k.add(24, 24, 24, 24, 24, 24);
	k.at(0x89A8); push(peek(0x01D0)); k.add(36);
	k.at(0x89AC); push(peek(0x01D2)); k.add(36);
	k.at(0x89B0); push(peek(0x01D4)); k.add(36);
	k.at(0x89B4); push(peek(0x0186)); k.add(36);
	k.call(0xDBE2, 0x89BC);                            // step_palette_fade
	k.call(0xDDEC, 0x89C0);                            // run_palette_job
	sel_ab(k, 0x89C0, 0x1800, 0x1C00);
	k.call(0x7D82, 0x89DC);                            // blink_screen7_palette
	k.call(0x917C, 0x89E0);                            // feed_watchdog
	k.at(0x89E0); pokeb(0x0192, (uint8_t)(peekb(0x0192) + 1)); k.add(27);
	k.at(0x89E4); poke(0x15E0, 0); k.add(27);
	k.at(0x89E8); poke(0x15C0, 0); k.add(27);
	k.at(0x89EC); pokeb(0x0191, 0); k.add(27);
	k.at(0x89F0); poke(0x01C4, (uint16_t)(peek(0x01C4) + 1)); k.add(27);
	k.at(0x89F4);
	const bool stopped = peek(0x0F18) == 0xFFFF;
	k.add(30, 12);
	if (!stopped)
	{
		k.at(0x89FC); poke(0x0F18, (uint16_t)(peek(0x0F18) - 1)); k.add(27);
	}
	sel_a(k, 0x8A00, 0x1800);
	k.call(0x5D86, 0x8A10);                            // step_sound_level
	sel_ab(k, 0x8A10, 0x3800, 0x3C00);
	k.call(0x7A3A, 0x8A2C);
	k.call(0x80A4, 0x8A30);                            // read_input_edges
	k.call(0x8036, 0x8A34);                            // load_scroll_registers
	k.at(0x8A34);
	const bool running = (int16_t)peek(0x0F22) > 0;
	k.add(24, 12);
	if (running)
	{
		k.at(0x8A3A);
		const bool held = peek(0x0180) != 0;
		k.add(24, 12);
		if (!held)
		{
			k.at(0x8A40); poke(0x0F22, (uint16_t)(peek(0x0F22) - 1)); k.add(27);
		}
	}
	k.at(0x8A44);
	const bool quiet = peekb(0x01A4) != 0;
	k.add(24, 12);
	if (!quiet)
	{
		sel_ab(k, 0x8A4A, 0x3800, 0x3C00);
		k.call(0x4314, 0x8A66);                        // send_queued_sound
	}
	const uint16_t sp = T.r[6];
	k.at(0x8A66); poke(0x0186, rw(sp)); k.add(33);
	k.at(0x8A6A); poke(0x01D2, rw((uint16_t)(sp + 4))); k.add(42);
	k.at(0x8A70); poke(0x1400, peek(0x01D2)); k.add(39);
	k.at(0x8A76); poke(0x01D4, rw((uint16_t)(sp + 2))); k.add(42);
	k.at(0x8A7C); poke(0x1402, peek(0x01D4)); k.add(39);
	k.at(0x8A82); poke(0x01D0, rw((uint16_t)(sp + 6))); k.add(42);
	k.call(0x8096, 0x8A8C);                            // reselect_video_bank
	T.r[6] = (uint16_t)(T.r[6] + 8); k.add(18);
	for (int i = 5; i >= 0; i--)
	{
		T.r[i] = pop(); k.add(18);
	}
	k.flush();                                         // the RTI (in the table)
}

// F:8512 (assembly; from the attract mode vertical blank F:84D4): the frame's work outside
// the game: scroll registers, the banked palette animation and timers, watchdog, input, the
// sound queue, the bee timer 0x0F22, the town's zone scan, the controller / sound exchange
// (F:8D86, F:5BD2 every 32 frames, F:910C 16 frames later), and unless the sprites are held
// (0x0995) the map objects, scroll blocks, demo contacts and the skater's animation.  The
// video window, banks, text pointer and 0x09B2-0x09B6 are kept on the stack and put back;
// 0x0196 counts the nesting meanwhile.
void attract_frame()
{
	Clock k;
	k.at(0x8512); pokeb(0x0196, fl::incb(peekb(0x0196))); k.add(27);
	static const uint16_t kept[7] = { 0x01D0, 0x01D2, 0x01D4, 0x0186, 0x09B2, 0x09B6, 0x09B4 };
	for (int i = 0; i < 7; i++)
	{
		k.at((uint16_t)(0x8516 + 4 * i));
		const uint16_t v = peek(kept[i]);
		fl::mov(v);
		push(v);
		k.add(36);
	}
	k.at(0x8532);
	{
		const uint16_t v = peek(0x09AA);
		fl::mov(v);
		poke(0x09A8, v);
	}
	k.add(39);
	k.call(0x8036, 0x853C);                            // load_scroll_registers
	sel_ab(k, 0x853C, 0x1800, 0x1C00);
	k.call(0x7BBC, 0x8558);                            // animate_palette
	k.at(0x8558);
	const uint8_t level = peekb(0x0F59);
	fl::cmpb(level, 4);
	k.add(30, 12);
	if (level == 4) k.call(0xDD10, 0x8564);            // flash_text_colours
	k.at(0x8564);
	{
		const uint8_t state = peekb(0x018F);
		fl::tstb(state);
		k.add(24, 12);
		if (!state)
		{
			sel_b(k, 0x856A, 0x3C00);
			k.call(0x7F52, 0x857A);                    // tick_game_timer
		}
	}
	sel_ab(k, 0x857A, 0x3800, 0x3C00);
	k.call(0x7A3A, 0x8596);
	k.call(0x917C, 0x859A);                            // feed_watchdog
	k.call(0x80A4, 0x859E);                            // read_input_edges
	k.at(0x859E);
	{
		const uint8_t locked = peekb(0x01A4);
		fl::tstb(locked);
		k.add(24, 12);
		if (!locked)
		{
			sel_ab(k, 0x85A4, 0x3800, 0x3C00);
			k.call(0x4314, 0x85C0);                    // send_queued_sound
		}
	}
	k.at(0x85C0);
	{
		const uint16_t bees = peek(0x0F22);
		fl::tst(bees);
		k.add(24, 12);
		if ((int16_t)bees > 0)
		{
			k.at(0x85C6);
			const uint16_t held = peek(0x0180);
			fl::tst(held);
			k.add(24, 12);
			if (!held)
			{
				k.at(0x85CC); poke(0x0F22, fl::dec(peek(0x0F22))); k.add(27);
			}
		}
	}
	k.at(0x85D0);
	{
		const uint8_t lv = peekb(0x0F59);
		fl::tstb(lv);
		k.add(24, 12);
		if (!lv)
		{
			sel_ab(k, 0x85D6, 0x1800, 0x1C00);
			k.call(0x454A, 0x85F2);                    // scan_zone_switches
		}
	}
	k.at(0x85F2);
	{
		const uint8_t locked = peekb(0x01A4);
		fl::tstb(locked);
		k.add(24, 12);
		if (!locked)
		{
			k.call(0x8D86, 0x85FC);                    // read_controller
			k.at(0x85FC);
			const uint16_t frame = peek(0x01C4);
			f_nzv(nzw(frame & 0x001F));
			k.add(30, 12);
			if (!(frame & 0x001F))
			{
				sel_ab(k, 0x8604, 0x1800, 0x1C00);
				k.call(0x5BD2, 0x8620);                // poll_sound_status
				k.add(12);                             // BR
			}
			else
			{
				k.at(0x8622); T.r[0] = peek(0x01C4); fl::mov(T.r[0]); k.add(24);
				T.r[0] = fl::add(T.r[0], 0x0010); k.add(18);
				f_nzv(nzw(T.r[0] & 0x001F)); k.add(18, 12);
				if (!(T.r[0] & 0x001F)) k.call(0x910C, 0x8634);   // read_sound_status
			}
		}
	}
	k.at(0x8634);
	const uint8_t held = peekb(0x0995);
	fl::tstb(held);
	k.add(24, 12);
	if (!held)
	{
		k.at(0x863A);
		const uint8_t lv = peekb(0x0F59);
		fl::tstb(lv);
		k.add(24, 12);
		if (!lv) k.call(0xBC52, 0x8644);               // check_map_objects_in_view
		k.at(0x8644);
		const uint8_t lv2 = peekb(0x0F59);
		fl::cmpb(lv2, 1);
		k.add(30, 12);
		if (lv2 <= 1) k.call(0xCB24, 0x8650);          // update_objects
		sel_ab(k, 0x8650, 0x2400, 0x2800);
		k.call(0x7B82, 0x866C);                        // track_scroll_blocks
		k.call(0xA510, 0x8670);                        // object_contact
		k.at(0x8670);
		const uint8_t skater = peekb(0x0374);
		fl::tstb(skater);
		k.add(24, 12);
		if (skater)
		{
			sel_ab(k, 0x8676, 0x1800, 0x1C00);
			k.call(0x7AA2, 0x8692);
		}
		k.at(0x8692);
		const uint8_t state = peekb(0x018F);
		fl::cmpb(state, 4);
		k.add(30, 12);
		if (state != 4)
		{
			sel_ab(k, 0x869A, 0x3800, 0x3C00);
			k.call(0x538E, 0x86B6);                    // terrain_event
			k.call(0x50F8, 0x86BA);                    // fallen_skater
			k.call(0xE036, 0x86BE);
			k.add(12);                                 // BR
		}
		else
		{
			k.at(0x86C0);
			const uint16_t t = peek(0x0F18);
			fl::cmp(t, 0xFFFF);
			k.add(30, 12);
			if (t != 0xFFFF)
			{
				k.at(0x86C8); poke(0x0F18, fl::dec(peek(0x0F18))); k.add(27);
			}
		}
	}
	k.at(0x86CC);
	{
		const uint16_t v = peek(0x09A8);
		fl::mov(v);
		poke(0x09AA, v);
	}
	k.add(39);
	const uint16_t sp = T.r[6];
	struct Back { uint16_t pc; uint16_t off; uint16_t to; int cycles; };
	static const Back back[5] = {
		{0x86D2, 0x0, 0x09B4, 33}, {0x86D6, 0x2, 0x09B6, 42}, {0x86DC, 0x4, 0x09B2, 42},
		{0x86E2, 0x6, 0x0186, 42}, {0x86E8, 0xA, 0x01D2, 42}};
	for (const Back &b : back)
	{
		k.at(b.pc);
		const uint16_t v = rw((uint16_t)(sp + b.off));
		fl::mov(v);
		poke(b.to, v);
		k.add(b.cycles);
	}
	k.at(0x86EE); { const uint16_t v = peek(0x01D2); fl::mov(v); poke(0x1400, v); } k.add(39);
	k.at(0x86F4); { const uint16_t v = rw((uint16_t)(sp + 8)); fl::mov(v); poke(0x01D4, v); } k.add(42);
	k.at(0x86FA); { const uint16_t v = peek(0x01D4); fl::mov(v); poke(0x1402, v); } k.add(39);
	k.at(0x8700); { const uint16_t v = rw((uint16_t)(sp + 0xC)); fl::mov(v); poke(0x01D0, v); } k.add(42);
	k.call(0x8096, 0x870A);                            // reselect_video_bank
	k.at(0x870A); pokeb(0x0196, fl::decb(peekb(0x0196))); k.add(27);
	T.r[6] = fl::add(T.r[6], 0x000E); k.add(18);
	k.leave();
}

// F:84D4 (interrupt, vertical blank in attract mode): count the frame (0x01C4, 0x0192),
// acknowledge both display interrupts, restart the quarter-frame count 0x0191 and, with
// interrupts allowed again, do the frame's work (F:8512) unless it is still running
void irq_vblank_attract()
{
	Clock k;
	for (int i = 0; i <= 5; i++)
	{
		k.at((uint16_t)(0x84D4 + 2 * i));
		fl::mov(T.r[i]);
		push(T.r[i]);
		k.add(24);
	}
	k.at(0x84E0); poke(0x01C4, fl::inc(peek(0x01C4))); k.add(27);
	k.at(0x84E4); pokeb(0x0192, fl::incb(peekb(0x0192))); k.add(27);
	k.at(0x84E8); poke(0x15C0, 0); fl::clr(); k.add(27);
	k.at(0x84EC); pokeb(0x0191, 0); fl::clr(); k.add(27);
	k.at(0x84F0); poke(0x15E0, 0); fl::clr(); k.add(27);
	T.r[0] = 0; fl::clr(); k.add(12);
	mtps(0); k.step(30, 0x84FA);                       // a pending interrupt is taken here
	k.at(0x84FA);
	const uint8_t busy = peekb(0x0196);
	fl::tstb(busy);
	k.add(24, 12);
	if (!busy) k.call(0x8512, 0x8504);                 // attract_frame
	for (int i = 5; i >= 0; i--)
	{
		k.at((uint16_t)(0x8504 + 2 * (5 - i)));
		T.r[i] = pop();
		fl::mov(T.r[i]);
		k.add(18);
	}
	k.flush();                                         // the RTI (in the table)
}

// F:A2F0  skater_land(): back on the ground (mode 0x031F = 1): kick state reset, ground
// friction 0x02FA from the equipment (0x7CAC), push / boost / 0x01DC cleared, speed made
// positive (direction reversed instead); at the speed limit of the board (0x7C90) on flat
// ground the speed is held there with the animation from 0x4488 (objects 1-2 state 6) and
// a random sound (0xFCF6), otherwise the animation for 0x01F8 and the speed kept within
// 0x180-0x280 (sound 0x54).  Banks restored; R1-R3 preserved (F144).
uint16_t skater_land(const Args &)
{
	Clock k;
	k.save(0xA2F0, 3);
	k.at(0xA2F4); T.r[2] = peek(0x01D2); k.add(24);
	k.at(0xA2F8); T.r[3] = peek(0x01D4); k.add(24);
	sel_b(k, 0xA2FC, 0x2C00);
	k.call(0xE238, 0xA30C);                            // reset_skater_state
	k.at(0xA30C); pokeb(0x031F, 1); k.add(33);
	k.at(0xA312); T.r[0] = sext8(peekb(0x025E)); k.add(24);
	k.at(0xA316); pokeb(0x02FA, peekb((uint16_t)(0x7CAC + T.r[0]))); k.add(42);
	k.at(0xA31C); pokeb(0x02FB, 0); k.add(27);
	k.at(0xA320);
	const bool ramp2 = peekb(0x01EA) == 2;
	k.add(30, 12);
	if (ramp2)
	{
		k.at(0xA328); pokeb(0x01EA, 1); k.add(33);
	}
	k.at(0xA32E); pokeb(0x0325, 0xFF); k.add(33);
	k.at(0xA334); poke(0x02FC, 0); k.add(27);
	k.at(0xA338); poke(0x02FE, 0); k.add(27);
	k.at(0xA33C); poke(0x01DC, 0); k.add(27);
	k.at(0xA340);
	const bool on_ramp = peekb(0x01EA) != 0;
	k.add(24, 12);
	if (!on_ramp)
	{
		k.at(0xA346); poke(0x01B4, 0x0160); k.add(33);
		k.at(0xA34C);
		const bool up = (int16_t)peek(0x05B2) > 0;
		k.add(24, 12);
		if (up)
		{
			k.at(0xA352); poke(0x05B2, 0); k.add(27);
		}
		k.call(0x9B82, 0xA35A);                        // skater_motion_params
		k.call(0x91A6, 0xA35E);                        // skater_velocity
	}
	k.at(0xA35E);
	const bool back = (int16_t)peek(0x01B4) < 0;
	k.add(24, 12);
	if (back)
	{
		k.at(0xA364); poke(0x01B4, (uint16_t)-peek(0x01B4)); k.add(27);
		T.r[1] = 8; k.add(18);
		k.at(0xA36C); T.r[0] = sext8(peekb(0x031D)); k.add(24);
		T.r[1] ^= T.r[0]; k.add(12);
		k.at(0xA372); pokeb(0x031D, (uint8_t)T.r[1]); k.add(27);
	}
	k.at(0xA376); T.r[1] = peek(0x01B4); k.add(24);
	k.at(0xA37A); T.r[0] = (uint16_t)(sext8(peekb(0x025D)) << 1); k.add(24, 12);
	k.at(0xA380);
	const uint16_t limit = peek((uint16_t)(0x7C90 + T.r[0]));
	k.add(27, 12);
	bool fast = false;
	if ((int16_t)T.r[1] >= (int16_t)limit)
	{
		k.at(0xA386);
		fast = !(peekb(0x0327) & 7);
		k.add(30, 12);
	}
	if (fast)
	{
		k.at(0xA38E); poke(0x01B4, peek((uint16_t)(0x7C90 + T.r[0]))); k.add(42);
		k.at(0xA394); poke(0x01D2, 0); k.add(27);
		k.at(0xA398); poke(0x1400, 0); k.add(27);
		sel_b(k, 0xA39C, 0x0400);
		k.at(0xA3A8); T.r[0] = (uint16_t)(sext8(peekb(0x031D)) << 1); k.add(24, 12);
		k.at(0xA3AE); poke(0x03B4, peek((uint16_t)(0x4488 + T.r[0]))); k.add(42);
		T.r[0] = 6; k.add(18);
		k.at(0xA3B8); pokeb(0x0792, 6); k.add(27);
		k.at(0xA3BC); pokeb(0x0772, 6); k.add(27);
		k.call(0x9152, 0xA3C4);                        // next_random
		T.r[0] &= 3; k.add(18);
		push(0); k.add(24);
		k.at(0xA3CA); poke(T.r[6], peekb((uint16_t)(0xFCF6 + T.r[0]))); k.add(36, 12);
	}
	else
	{
		k.at(0xA3D0); T.r[0] = (uint16_t)(sext8(peekb(0x01F8)) << 1); k.add(24, 12);
		T.r[0] = (uint16_t)(T.r[0] + 0x4428); k.add(18);
		k.at(0xA3DA); poke(0x03B4, T.r[0]); k.add(27);
		k.at(0xA3DE);
		const bool park3 = peekb(0x0F59) == 3;
		k.add(30, 12);
		bool done = false;
		if (park3)
		{
			k.at(0xA3E6); T.r[0] = sext8(peekb(0x0327)) & 7; k.add(24, 18, 18, 12);
			if (T.r[0] >= 5)
			{
				k.at(0xA3F4); T.r[0] = peek(0x05B2); k.add(24);
				T.r[0] = (uint16_t)-(uint16_t)(T.r[0] << 1); k.add(12, 12, 12, 12);
				if ((int16_t)T.r[1] < (int16_t)T.r[0])
				{
					k.at(0xA400); T.r[0] = peek(0x05B2); k.add(24);
					T.r[0] = (uint16_t)-(uint16_t)(T.r[0] << 1); k.add(12, 12);
					k.at(0xA408); poke(0x01B4, T.r[0]); k.add(27, 12);
					done = true;
				}
			}
		}
		if (!done)
		{
			k.add(18, 12);
			if ((int16_t)T.r[1] < 0x0180)
			{
				k.at(0xA414); poke(0x01B4, 0x0180); k.add(33, 12);
			}
			else
			{
				k.add(18, 12);
				if ((int16_t)T.r[1] > 0x0280)
				{
					k.at(0xA422); poke(0x01B4, 0x0280); k.add(33);
				}
			}
		}
		push(0x0054); k.add(30);
	}
	k.call(0x8E78, 0xA430);                            // queue_sound
	k.at(0xA430); pokeb(0x01F8, peekb(0x031D)); k.add(39);
	k.at(0xA436); poke(0x03B0, 0x50FA); k.add(33);
	k.at(0xA43C); poke(0x03B2, 0x50FA); k.add(33);
	k.at(0xA442); poke(0x01D2, T.r[2]); k.add(27);
	k.at(0xA446); poke(0x01D4, T.r[3]); k.add(27);
	k.at(0xA44A); poke(0x1400, peek(0x01D2)); k.add(39);
	k.at(0xA450); poke(0x1402, peek(0x01D4)); k.add(39);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	k.add(18);
	k.restore(3);
	k.leave();
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
	Clock k;
	k.save(0xA090, 4);
	sel_b(k, 0xA094, 0x2C00);
	k.at(0xA0A0);
	const bool park4 = peekb(0x0F59) == 4;
	k.add(30, 12);
	if (park4)
	{
		k.at(0xA0A8);
		const bool skip = peekb(0x01F2) != 0;
		k.add(24, 12);
		if (!skip)
		{
			sel_a(k, 0xA0AE, 0x2400);
			k.call(0x90BA, 0xA0BE);                    // park_level
			T.r[3] = 0; k.add(12);
			k.at(0xA0C0); T.r[3] = peekb((uint16_t)(0x4579 + T.r[0])); k.add(27);
			T.r[1] = 0; k.add(12);
			k.at(0xA0C6); T.r[1] = peekb(0x0FE0); k.add(24);
			T.r[3] = (uint16_t)(T.r[3] + T.r[1]); k.add(12);
			T.r[0] = (uint16_t)(T.r[3] << 1); k.add(12, 12);
			k.at(0xA0D0); push(peek((uint16_t)(0x7E50 + T.r[0]))); k.add(39);
			k.at(0xA0D4); push(peek((uint16_t)(0x7E8A + T.r[0]))); k.add(39);
			k.at(0xA0D8); push(peek((uint16_t)(0x7EC4 + T.r[0]))); k.add(39);
			push(0x0078); k.add(30);
			k.call(0xE1FA, 0xA0E4);                    // fill_slots
			T.r[6] = (uint16_t)(T.r[6] + 8); k.add(18);
		}
	}
	sel_a(k, 0xA0E8, 0x1800);
	k.call(0x51BA, 0xA0F8);                            // copy_object0_position
	k.at(0xA0F8); T.r[0] = sext8(peekb(0x0EA0)); k.add(24, 12);
	if (T.r[0])
	{
		T.r[0] = (uint16_t)(T.r[0] << 1); k.add(12);
		k.at(0xA100); poke(0x04F0, (uint16_t)(peek(0x04F0) - peek((uint16_t)(0x04F0 + T.r[0])))); k.add(42);
		k.at(0xA106); poke(0x0570, (uint16_t)(peek(0x0570) - peek((uint16_t)(0x0570 + T.r[0])))); k.add(42);
	}
	k.at(0xA10C); T.r[3] = sext8(peekb(0x0327)); k.add(24);
	k.at(0xA110); pokeb(0x0327, a.b(0)); k.add(42);
	k.at(0xA116); pokeb(0x01EA, 1); k.add(33);
	k.at(0xA11C);
	const bool reset = peekb(0x0330) == 0x40;
	k.add(30, 12);
	if (reset) k.call(0xE238, 0xA128);                 // reset_skater_state
	k.at(0xA128); T.r[0] = sext8(peekb(0x0327)) & 7; k.add(24, 18, 18, 12);
	if (T.r[0] >= 5)
	{
		k.at(0xA136); T.r[0] = sext8(peekb(0x0327)); k.add(24);
		T.r[0] = (uint16_t)((int16_t)T.r[0] >> 3) & 3; k.add(12, 12, 12, 18);
		k.at(0xA144); pokeb(0x031D, peekb((uint16_t)(0xFCD0 + T.r[0]))); k.add(42);
		k.at(0xA14A); pokeb(0x0325, 0x7F); k.add(33);
	}
	k.at(0xA150);
	const bool landing = peekb(0x0323) == 1;
	k.add(30, 12);
	if (landing)
	{
		k.call(0xA2F0, 0xA15C);                        // skater_land
		sel_b(k, 0xA15C, 0x2800);
		push(1); k.add(30);
		k.call(0x7E54, 0xA170);
		k.at(0xA170); pokeb(0x0323, 0); k.add(27, 12, 12);
		T.r[6] = (uint16_t)(T.r[6] + 2); k.add(18);
	}
	else
	{
		k.at(0xA176);
		const uint8_t mode = peekb(0x031F);
		k.add(30, 12);
		if (mode == 3)
			k.add(12);
		else
		{
			k.add(30, 12);
			if (mode == 1)
				k.add(12);
			else
			{
				k.at(0xA186); T.r[0] = sext8(peekb(0x01F8)); k.add(24);
				push(T.r[0]); k.add(24);
				k.call(0xA2BE, 0xA190);                // direction_bit
				T.r[6] = (uint16_t)(T.r[6] + 2);
				k.add(18, 12, 12);
				if (!T.r[0])
				{
					k.at(0xA196); T.r[1] = sext8(peekb(0x031D)); k.add(24);
					k.at(0xA19A); pokeb(0x031D, peekb(0x01F8)); k.add(39);
					k.call(0x9B82, 0xA1A4);            // skater_motion_params
					sel_a(k, 0xA1A4, 0x3800);
					k.call(0x410C, 0xA1B4);
					k.at(0xA1B4);
					const bool state = peekb(0x031E) != 0;
					k.add(24, 12);
					if (state)
					{
						push(T.r[1]); k.add(24);
						k.call(0x9EEE, 0xA1C0);        // skater_deflect
						T.r[6] = (uint16_t)(T.r[6] + 2); k.add(18);
					}
					k.at(0xA1C2);
					const bool park3 = peekb(0x0F59) == 3;
					k.add(30, 12);
					if (!park3)
					{
						k.at(0xA1CA); T.r[1] = peek(0x02E8); k.add(24);
						k.at(0xA1CE);
						const bool sloped = (peekb(0x0327) & 7) != 0;
						k.add(30, 12);
						bool faster = false;
						if (sloped)
						{
							k.at(0xA1D6); T.r[0] = peek(0x01B4); k.add(24, 12);
							if ((int16_t)T.r[0] < 0)
							{
								T.r[0] = (uint16_t)-T.r[0];
								k.add(12);
							}
							faster = (int16_t)T.r[1] > (int16_t)T.r[0];
							k.add(12, 12);
						}
						if (!faster)
						{
							k.at(0xA1E2); T.r[0] = peek(0x01B4); k.add(24, 12);
							if ((int16_t)T.r[0] < 0)
							{
								T.r[0] = (uint16_t)-T.r[0];
								k.add(12);
							}
							k.at(0xA1EA); T.r[2] = sext8(peekb(0x0327)) & 7; k.add(24, 18);
							T.r[2] = (uint16_t)(T.r[2] << 2); k.add(12, 12);
							k.at(0xA1F6); T.r[4] = sext8(peekb(0x025C)); k.add(24);
							T.r[2] = (uint16_t)((T.r[2] + T.r[4]) << 1); k.add(12, 12);
							k.at(0xA1FE); faster = (int16_t)T.r[0] > (int16_t)peek((uint16_t)(0x7BE8 + T.r[2])); k.add(27, 12);
						}
						if (faster)
						{
							k.at(0xA204);
							const bool forward = (int16_t)peek(0x01B4) > 0;
							k.add(24, 12);
							if (!forward)
							{
								T.r[1] = (uint16_t)-T.r[1];
								k.add(12);
							}
							k.at(0xA20C); poke(0x01B4, T.r[1]); k.add(27, 12);
						}
					}
				}
				else
				{
					k.call(0xE6F0, 0xA216);            // object1_screen_x
					k.call(0x9B82, 0xA21A);            // skater_motion_params
					k.at(0xA21A); T.r[2] = peek(0x05B2); k.add(24);
					sel_a(k, 0xA21E, 0x3800);
					k.call(0x410C, 0xA22E);
					k.at(0xA22E); T.r[0] = (uint16_t)(sext8(peekb(0x025D)) << 1); k.add(24, 12);
					bool land = false;
					k.at(0xA234);
					const bool at_limit = (int16_t)peek(0x01B4) >= (int16_t)peek((uint16_t)(0x7C90 + T.r[0]));
					k.add(39, 12);
					if (at_limit)
					{
						k.at(0xA23C);
						const bool slow_fall = (int16_t)T.r[2] < (int16_t)peek((uint16_t)(0x7C98 + T.r[0]));
						k.add(27, 12);
						if (slow_fall)
						{
							k.at(0xA242);
							land = peekb(0x0F59) != 3;
							k.add(30, 12);
						}
					}
					if (land)
					{
						k.call(0xA2F0, 0xA24E);        // skater_land
						sel_b(k, 0xA24E, 0x2800);
						push(2); k.add(30);
						k.call(0x7E54, 0xA262);
						T.r[6] = (uint16_t)(T.r[6] + 2);
						k.add(12, 18);
					}
					else
					{
						k.at(0xA264); T.r[2] = sext8(peekb(0x01F8)); k.add(24);
						k.at(0xA268); T.r[0] = sext8(peekb(0x031D)); k.add(24);
						T.r[2] = (uint16_t)(T.r[2] - T.r[0]); k.add(12, 12);
						if (T.r[2] & 0x8000)
						{
							k.at(0xA270); poke(0x0222, 0xFFFF); k.add(33);
							T.r[2] = (uint16_t)-T.r[2]; k.add(12, 12);
						}
						else
						{
							k.at(0xA27A); poke(0x0222, 1); k.add(33);
						}
						k.add(18, 12);
						if ((int16_t)T.r[2] > 8)
						{
							k.at(0xA286); poke(0x0222, (uint16_t)-peek(0x0222)); k.add(27);
						}
						k.at(0xA28A); T.r[0] = sext8(peekb(0x01F8)); k.add(24);
						push(T.r[0]); k.add(24);
						k.at(0xA290); push(peek(0x0222)); k.add(36);
						k.call(0xA45A, 0xA298);        // skater_phase10
						T.r[6] = (uint16_t)(T.r[6] + 4);
						k.add(18, 18);
					}
				}
			}
		}
	}
	k.at(0xA29C); pokeb(0x0327, (uint8_t)T.r[3]); k.add(27);
	k.at(0xA2A0);
	const bool mode1 = peekb(0x031F) == 1;
	k.add(30, 12);
	if (!mode1)
	{
		sel_b(k, 0xA2A8, 0x2800);
		push(0); k.add(24);
		k.call(0x7E54, 0xA2BA);
		T.r[6] = (uint16_t)(T.r[6] + 2); k.add(18);
	}
	k.restore(4);
	k.leave();
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
	Clock k;
	sel_ab(k, 0x94DC, 0x0800, 0x0C00);
	k.at(0x94F4); poke(0x0222, 0); k.add(27);
	k.at(0x94F8); T.r[2] = peek(0x0304); k.add(24, 18, 12);
	if (T.r[2] & 0x0400)
	{
		T.r[2] &= 0x03FF; k.add(18);
		k.at(0x9506); T.r[2] = (uint16_t)(T.r[2] + peek(0x01D8)); k.add(24, 12);
	}
	else
	{
		T.r[2] &= 0x03FF; k.add(18);
		k.at(0x9510); T.r[2] = (uint16_t)(T.r[2] + peek(0x01D6)); k.add(24);
	}
	T.r[0] = 0; k.add(12);
	k.at(0x9516); T.r[0] = peekb(0x032E); k.add(24);
	k.at(0x951A);
	const uint8_t mode = peekb(0x032F);
	T.r[1] = sext8(mode);
	k.add(24);
	T.r[1] = (uint16_t)(T.r[1] << 1); k.add(12, 27);
	if (mode == 0)
	{
		k.add(18, 12);
		if (T.r[0] & 0x40)
		{
			k.at(0x952A); pokeb(0x032F, 2); k.add(33);
			f_nzvc(0);
			k.leave();
			return;
		}
		fl::cmp(T.r[0], 0x0080);
		k.add(18, 12);
		if (T.r[0] == 0x0080)
		{
			k.at(0x953A); pokeb(0x032F, 1); k.add(33);
			f_nzvc(0);
		}
		k.leave();
		return;
	}
	if (mode == 1)
	{
		k.add(12, 12);
		if (!T.r[0])
		{
			k.add(18);
			k.at(0x9718); pokeb(0x032F, 0); k.add(27);
			fl::clr();
			k.leave();
			return;
		}
		k.add(18, 12);
		if (!(T.r[0] & 0x40))
		{
			f_nzvc(ZF);
			k.leave();
			return;
		}
		k.at(0x9550); pokeb(0x0329, 0xFF); k.add(33);
		k.at(0x9556);
		const bool off = peekb(0x0328) == 0xFF;
		k.add(30, 12);
		if (off)
		{
			f_nzvc(ZF);
			k.leave();
			return;
		}
		k.at(0x955E);
		const bool ramp = peekb(0x01EA) != 0;
		k.add(24, 12);
		if (ramp)
		{
			k.add(18);
			k.at(0x96EA); T.r[0] = peek(0x01B4); k.add(24, 12);
			bool wall;
			if (!(T.r[0] & 0x8000))
			{
				k.at(0x96F0); wall = (peek(0x0306) & 0x4000) != 0; k.add(30, 12);
				if (!wall)
				{
					fl::tst(T.r[0]);
					k.add(12, 12);
					k.leave();
					return;
				}
			}
			else
			{
				k.at(0x96FC); wall = (peek(0x0302) & 0x4000) != 0; k.add(30, 12);
				if (!wall)
				{
					f_nzvc(ZF);
					k.leave();
					return;
				}
			}
			push(0x003F); k.add(30);
			k.call(0x9EEE, 0x970C);                    // skater_deflect
			k.at(0x970C); poke(0x0222, 1); k.add(33, 12);
			k.at(0x9782); pokeb(0x0328, peekb(0x0329)); k.add(39, 12);
			fl::tst(rw(T.r[6]));
			T.r[6] = (uint16_t)(T.r[6] + 2);
			k.add(18);
			k.leave();
			return;
		}
		k.at(0x9568); poke(0x0220, peek(0x0214)); k.add(39);
		k.at(0x956E); T.r[0] = peek(0x06B2); k.add(24);
		k.at(0x9572); T.r[0] = (uint16_t)(T.r[0] - peek(0x06B0)); k.add(24);
		k.at(0x9576); T.r[0] = (uint16_t)(T.r[0] + peek(0x021C)); k.add(24);
		T.r[0] = (uint16_t)((int16_t)T.r[0] >> 1) & 0x3F00; k.add(12, 18);
		T.r[1] = (uint16_t)(0x3E00 - T.r[0]); k.add(18, 12);
		T.r[0] = T.r[1]; k.add(12);
		k.call(0x8078, 0x958C);                        // select_video_bank
		k.at(0x958C); poke(0x0218, T.r[1]); k.add(27);
		T.r[1] &= 0x1F00; k.add(18);
		k.at(0x9594); poke(0x021E, T.r[1]); k.add(27);
		T.r[0] = 0; k.add(12);
		k.at(0x959A); T.r[0] = peekb(0x0221); k.add(24);
		T.r[1] = (uint16_t)(T.r[1] + T.r[0]); k.add(12);
		k.at(0x95A0); T.r[1] = peek((uint16_t)(0x2000 + T.r[1])); k.add(27, 18, 12);
		if (T.r[1] & 0x0400)
		{
			T.r[1] &= 0x03FF; k.add(18);
			k.at(0x95AE); T.r[1] = (uint16_t)(T.r[1] + peek(0x01D8)); k.add(24, 12);
		}
		else
		{
			T.r[1] &= 0x03FF; k.add(18);
			k.at(0x95B8); T.r[1] = (uint16_t)(T.r[1] + peek(0x01D6)); k.add(24);
		}
		k.at(0x95BC); T.r[3] = sext8(peekb((uint16_t)(0x6000 + T.r[2]))); k.add(27);
		k.at(0x95C0);
		bool solid = (peekb((uint16_t)(0x6000 + T.r[1])) & 0x80) != 0;
		k.add(27, 12);
		if (!solid)
		{
			k.add(18, 12);
			if (T.r[3] & 0x40)
			{
				T.r[0] = (uint16_t)(7 & ~(T.r[3] & 0xFF));
				solid = T.r[0] == 0;
				k.add(18, 12, 12);
			}
		}
		if (solid)
		{
			k.add(18, 12);
			if (T.r[3] & 0x40)
			{
				sel_ab(k, 0x95DA, 0x1800, 0x1C00);
				push(T.r[3]); k.add(24);
				k.call(0x7B3E, 0x95F8);                // B1:7B3E start_object4
				sel_ab(k, 0x95F8, 0x0800, 0x0C00);
				T.r[6] = (uint16_t)(T.r[6] + 2); k.add(18);
			}
			T.r[1] = 8; k.add(18);
			k.at(0x9616); T.r[0] = sext8(peekb(0x031D)); k.add(24);
			T.r[1] ^= T.r[0]; k.add(12);
			k.at(0x961C); pokeb(0x031D, (uint8_t)T.r[1]); k.add(27);
			k.at(0x9620); poke(0x01B4, 0x0240); k.add(33);
			k.at(0x9626); T.r[0] = sext8(peekb(0x0327)); k.add(24);
			k.at(0x962A); poke(0x021A, T.r[0]); k.add(27);
			k.at(0x962E); pokeb(0x0327, 0); k.add(27);
			k.call(0x9B82, 0x9636);                    // skater_motion_params
			k.call(0x91A6, 0x963A);                    // skater_velocity
			k.at(0x963A); pokeb(0x0327, peekb(0x021A)); k.add(39);
			k.call(0x9B82, 0x9644);
			k.at(0x9644);
			const bool up = (int16_t)peek(0x05B2) > 0;
			k.add(24, 12);
			if (up)
			{
				k.at(0x964A); poke(0x05B2, 0); k.add(27);
			}
			k.at(0x964E); poke(0x0222, 1); k.add(33);
			k.at(0x9654); pokeb(0x031E, 2); k.add(33);
			k.at(0x965A); pokeb(0x031C, 6); k.add(33);
			k.at(0x9660); pokeb(0x0328, peekb(0x0329)); k.add(39, 12);
		}
		else
		{
			k.at(0x9668); pokeb(0x032F, 2); k.add(33, 12, 12);
			if (T.r[3] & 0x8000)
			{
				k.at(0x9672); poke(0x0212, 0x0100); k.add(33);
				k.at(0x9678); poke(0x06B0, peek(0x06B2)); k.add(39);
				push(0x7FFF); k.add(30);
				k.at(0x9682); T.r[0] = peek(0x021E); k.add(24);
				k.at(0x9686); T.r[1] = peek(0x0220); k.add(24);
				k.call(0x9C5E, 0x968E);                // trace_cells
				T.r[2] = T.r[0]; k.add(12);
				k.at(0x9690);
				const bool found = peek(0x0222) != 0x7FFF;
				k.add(30, 12);
				if (found)
				{
					k.at(0x9698); poke(0x0304, T.r[2]); k.add(27);
					k.at(0x969C); poke(0x06B0, peek(0x0222)); k.add(39);
					k.at(0x96A2); poke(0x0222, 0xFFFF); k.add(33, 12);
				}
				else
				{
					k.at(0x96AA); poke(0x06B0, (uint16_t)(peek(0x06B0) - 0x0200)); k.add(33);
					k.at(0x96B0); poke(0x0222, 1); k.add(33);
				}
				k.at(0x96B6); pokeb(0x031E, 2); k.add(33);
				k.at(0x96BC); pokeb(0x031C, 6); k.add(33);
				k.at(0x96C2); pokeb(0x0328, peekb(0x0329)); k.add(39);
				T.r[6] = (uint16_t)(T.r[6] + 2); k.add(18);
			}
		}
		k.call(0x9B82, 0x96CE);                        // skater_motion_params
		k.at(0x96CE); push(peek(0x01B4)); k.add(36);
		k.at(0x96D2); push(peek(0x02EC)); k.add(36);
		k.call(0x80FA, 0x96DA);                        // mul32
		k.at(0x96DA); pokeb(0x07F8, peekb(0x0205)); k.add(39);
		k.at(0x96E0); poke(0x05B0, peek(0x0206)); k.add(39);
		fl::cmp(rw(T.r[6]), rw((uint16_t)(T.r[6] + 2)));
		T.r[6] = (uint16_t)(T.r[6] + 4);
		k.add(24);
		k.leave();
		return;
	}
	if (mode == 2)
	{
		k.add(12, 12);
		if (!T.r[0])
		{
			k.at(0x9718); pokeb(0x032F, 0); k.add(27);
			fl::clr();
			k.leave();
			return;
		}
		k.at(0x971E);
		const uint8_t attr = peekb((uint16_t)(0x6000 + T.r[2]));
		k.add(27, 12);
		if (attr & 0x80)
		{
			fl::tstb(attr);
			k.leave();
			return;
		}
		k.add(12, 12);
		if (!(T.r[0] & 0x80))
		{
			fl::tstb((uint8_t)T.r[0]);
			k.leave();
			return;
		}
		k.at(0x9728); pokeb(0x0329, 0xFF); k.add(33);
		k.at(0x972E);
		const bool off = peekb(0x0328) == 0xFF;
		k.add(30, 12);
		if (off)
		{
			f_nzvc(ZF);
			k.leave();
			return;
		}
		k.at(0x9736); pokeb(0x032F, 1); k.add(33);
		k.at(0x973C); poke(0x0212, 0x0100); k.add(33);
		push(0x7FFE); k.add(30);
		k.at(0x9746); T.r[0] = peek(0x021E); k.add(24);
		k.at(0x974A); T.r[1] = peek(0x0220); k.add(24);
		k.call(0x9C5E, 0x9752);                        // trace_cells
		T.r[2] = T.r[0]; k.add(12);
		k.at(0x9754); T.r[1] = peek(0x0222); k.add(24, 18, 12);
		bool up = false;
		if (T.r[1] != 0x7FFF)
		{
			k.at(0x975E); T.r[0] = peek(0x06B2); k.add(24);
			T.r[0] = (uint16_t)(T.r[0] - T.r[1]); k.add(12, 18, 12);
			if ((int16_t)T.r[0] > 0)
			{
				k.at(0x9768); poke(0x0304, T.r[2]); k.add(27);
				k.at(0x976C); poke(0x06B0, T.r[1]); k.add(27);
				k.at(0x9770); poke(0x0222, 0xFFFF); k.add(33);
				k.at(0x9776); pokeb(0x031E, 2); k.add(33);
				k.at(0x977C); pokeb(0x031C, 6); k.add(33);
				k.at(0x9782); pokeb(0x0328, peekb(0x0329)); k.add(39, 12);
				up = true;
			}
		}
		if (!up)
		{
			k.at(0x978A); pokeb(0x032F, 2); k.add(33);
			k.at(0x9790); poke(0x0222, 1); k.add(33);
		}
		fl::tst(rw(T.r[6]));
		T.r[6] = (uint16_t)(T.r[6] + 2);
		k.add(18);
		k.leave();
		return;
	}
	// modes 3 and up: the table entries are the next table's data; not used
	k.leave();
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
	Clock k;
	sel_ab(k, 0x979A, 0x0800, 0x0C00);
	k.at(0x97B2);
	const uint16_t pending = peek(0x0222);
	k.add(24, 12);
	if (pending)
	{
		fl::tst(pending);
		k.leave();
		return;
	}
	k.at(0x97BA); poke(0x021C, 0xFFFF); k.add(33);
	T.r[0] = 7; k.add(18);
	k.at(0x97C4); T.r[0] &= ~peek(0x0304); k.add(24, 12);
	if (!T.r[0])
	{
		k.at(0x97CA); poke(0x021C, 1); k.add(33, 12);
	}
	else
	{
		k.at(0x97D2);
		const bool forward = (int16_t)peek(0x01B4) > 0;
		k.add(24, 12);
		T.r[0] = 7; k.add(18);
		if (forward)
		{
			k.at(0x97DC); T.r[0] &= ~peek(0x0306); k.add(24, 12);
			if (!T.r[0])
			{
				k.at(0x97E2); poke(0x021C, 2); k.add(33, 12);
			}
		}
		else
		{
			k.at(0x97EE); T.r[0] &= ~peek(0x0302); k.add(24, 12);
			if (!T.r[0])
			{
				k.at(0x97F4); poke(0x021C, 0); k.add(27);
			}
		}
	}
	k.at(0x97F8); T.r[0] = peek(0x021C); k.add(24, 12);
	if (T.r[0] & 0x8000)
	{
		f_nzvc(NF);
		k.leave();
		return;
	}
	T.r[0] = (uint16_t)(T.r[0] << 1); k.add(12);
	k.at(0x9800);
	const uint8_t code = peekb((uint16_t)(0x0302 + T.r[0]));
	pokeb(0x0329, code);
	k.add(42);
	k.at(0x9806);
	const bool same = code == peekb(0x0328);
	k.add(39, 12);
	if (same)
	{
		f_nzvc(ZF);
		k.leave();
		return;
	}
	k.at(0x980E); T.r[3] = sext8(peekb((uint16_t)(0x0302 + T.r[0]))); k.add(27);
	T.r[1] = 0; k.add(12, 18, 12);
	if (T.r[3] == 0x0037)
	{
		T.r[2] = 0xFFFF;
		k.add(18);
	}
	else
	{
		T.r[1] = 1; k.add(12);
		push(T.r[3]); k.add(24);
		k.call(0x9E52, 0x9822);                        // slope_table_value
		T.r[2] = T.r[0];
		T.r[6] = (uint16_t)(T.r[6] + 2);
		k.add(12, 18, 12);
	}
	k.add(12, 12);
	if (!(T.r[2] & 0x8000))
	{
		k.at(0x9834); T.r[0] = peek(0x021C); k.add(24);
		T.r[0] = (uint16_t)(T.r[0] << 1); k.add(12);
		k.at(0x983A); poke(0x021E, peek((uint16_t)(0x0308 + T.r[0]))); k.add(42);
		k.at(0x9840); poke(0x0220, peek((uint16_t)(0x030E + T.r[0]))); k.add(42);
		k.at(0x9846); poke(0x0218, peek((uint16_t)(0x0314 + T.r[0]))); k.add(42);
		k.at(0x984C); T.r[0] = peek((uint16_t)(0x0314 + T.r[0])); k.add(27);
		k.call(0x8078, 0x9854);                        // select_video_bank
		k.at(0x9854);
		const bool ramp = peekb(0x01EA) != 0;
		k.add(24, 12);
		if (ramp)
		{
			k.at(0x98F0); T.r[0] = sext8(peekb(0x0327)) & 7; k.add(24, 18, 18, 12);
			if (T.r[0] >= 5)
			{
				k.at(0x98FE); T.r[1] = sext8(peekb(0x0327)) & 0x0038; k.add(24, 18);
				T.r[0] = T.r[3] & 0x0038; k.add(12, 18, 12, 12);
				if (T.r[1] == T.r[0])
				{
					T.r[0] = 7; k.add(18);
					k.at(0x9914); T.r[0] &= ~peek(0x0304); k.add(24, 12);
					if (T.r[0])
					{
						f_nzvc(nzw(T.r[0]));
						k.leave();
						return;
					}
					k.at(0x991C); pokeb(0x0327, (uint8_t)T.r[3]); k.add(27);
					k.call(0x9FB4, 0x9924);            // skater_take_off
					k.at(0x9924); pokeb(0x031E, 2); k.add(33);
					k.at(0x992A); pokeb(0x031C, 2); k.add(33);
					k.at(0x9930); pokeb(0x0328, peekb(0x0329)); k.add(39);
					sel_a(k, 0x9936, 0x1800);
					k.call(0x5264, 0x9946);
					sel_ab(k, 0x9946, 0x0800, 0x2C00);
					fl::mov(0x2C00);
					k.leave();
					return;
				}
			}
			push(T.r[2]); k.add(24);
			k.call(0x9EEE, 0x9966);                    // skater_deflect
			k.add(12);
			k.at(0x98C6); pokeb(0x0328, peekb(0x0329)); k.add(39, 18);
			fl::tst(rw(T.r[6]));
			T.r[6] = (uint16_t)(T.r[6] + 2);
			k.add(18);
			k.leave();
			return;
		}
		k.add(18, 12);
		if (T.r[3] & 0x40)
		{
			sel_ab(k, 0x9860, 0x1800, 0x1C00);
			push(T.r[3]); k.add(24);
			k.call(0x7B3E, 0x987E);                    // B1:7B3E start_object4
			sel_ab(k, 0x987E, 0x0800, 0x0C00);
			T.r[3] = 0x8000; k.add(18, 12);
		}
		else
		{
			k.at(0x989C); T.r[3] = peek(0x06B2); k.add(24);
			k.at(0x98A0); T.r[3] = (uint16_t)(T.r[3] + peek(0x05B2)); k.add(24);
			k.at(0x98A4); poke(0x0212, 0xFF00); k.add(33);
			push(T.r[3]); k.add(24);
			k.at(0x98AC); T.r[0] = peek(0x021E); k.add(24);
			k.at(0x98B0); T.r[1] = peek(0x0220); k.add(24);
			k.call(0x9C5E, 0x98B8);                    // trace_cells
			T.r[3] = T.r[0]; k.add(12);
		}
		k.add(18, 12);
		if (T.r[3] == 0x8000)
		{
			poke(T.r[6], T.r[2]); k.add(21);
			k.call(0x9E96, 0x98C6);                    // skater_bounce
			k.at(0x98C6); pokeb(0x0328, peekb(0x0329)); k.add(39, 18);
		}
		else
		{
			k.at(0x98D0);
			const bool cell1 = peek(0x021C) == 1;
			k.add(30, 12);
			if (cell1)
			{
				k.at(0x98D8); poke(0x0304, T.r[3]); k.add(27);
				k.at(0x98DC); poke(0x06B0, peek(0x0222)); k.add(39);
				k.at(0x98E2); pokeb(0x031E, 2); k.add(33);
				k.at(0x98E8); pokeb(0x031C, 6); k.add(33, 12);
				k.at(0x98C6); pokeb(0x0328, peekb(0x0329)); k.add(39);
			}
			k.add(18);
		}
		fl::tst(rw(T.r[6]));
		T.r[6] = (uint16_t)(T.r[6] + 2);
		k.add(18);
		k.leave();
		return;
	}
	k.add(18);
	T.r[1] = ror(T.r[1]);
	k.add(12, 12);
	if (fC())
	{
		sel_a(k, 0x996C, 0x1800);
		k.at(0x9978); push(peek(0x0304)); k.add(36);
		k.call(0x51EE, 0x9980);                        // A5:51EE
		T.r[2] = T.r[0]; k.add(12);
		sel_a(k, 0x9982, 0x0800);
		k.at(0x998E);
		const bool back = (int16_t)peek(0x01B4) < 0;
		k.add(24, 12);
		if (back)
		{
			T.r[2] = (uint16_t)-T.r[2];
			k.add(12);
		}
		T.r[6] = (uint16_t)(T.r[6] + 2);
		k.add(18, 12, 12, 12);
	}
	else
	{
		k.at(0x999A);
		const bool cell1 = peek(0x021C) == 1;
		k.add(30, 12);
		if (cell1)
		{
			T.r[2] = 1;
			k.add(18, 12, 12);
		}
		else
		{
			T.r[2] = 0;
			k.add(12, 12, 12);
		}
	}
	if ((int16_t)T.r[2] <= 0)
	{
		fl::tst(T.r[2]);
		k.leave();
		return;
	}
	k.at(0x99AE); poke(0x0212, 0x0100); k.add(33);
	k.at(0x99B4); poke(0x0218, peek(0x0316)); k.add(39);
	push(0x7FFD); k.add(30);
	k.at(0x99BE); T.r[0] = peek(0x030A); k.add(24);
	k.at(0x99C2); T.r[1] = peek(0x0310); k.add(24);
	k.call(0x9C5E, 0x99CA);                            // trace_cells
	k.at(0x99CA); poke(0x0304, T.r[0]); k.add(27);
	k.at(0x99CE); T.r[0] = peek(0x06B2); k.add(24);
	k.at(0x99D2); T.r[0] = (uint16_t)(T.r[0] - peek(0x0222)); k.add(24, 18, 12);
	if ((int16_t)T.r[0] >= 0x0200)
	{
		k.at(0x99DC); poke(0x06B0, peek(0x0222)); k.add(39, 12);
	}
	else
	{
		k.at(0x99E4); poke(0x06B0, peek(0x06B2)); k.add(39);
	}
	k.at(0x99EA);
	const bool park4 = peekb(0x0F59) == 4;
	k.add(30, 12);
	if (park4)
	{
		k.at(0x99F2); T.r[0] = peek(0x06B2); k.add(24);
		k.at(0x99F6); T.r[0] = (uint16_t)(T.r[0] - peek(0x06B0)); k.add(24, 18, 12);
		if ((int16_t)T.r[0] > 0x2800)
		{
			sel_ab(k, 0x9A00, 0x3800, 0x3C00);
			k.call(0x7A9E, 0x9A1C);                    // snapshot_skater
		}
	}
	k.at(0x9A1C); pokeb(0x0328, peekb(0x0329)); k.add(39);
	k.at(0x9A22); pokeb(0x031C, 2); k.add(33);
	k.at(0x9A28); pokeb(0x031E, 2); k.add(33);
	fl::tst(rw(T.r[6]));
	T.r[6] = (uint16_t)(T.r[6] + 2);
	k.add(18);
	k.leave();
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
	Clock k;
	k.save(0xA9FC, 5);
	k.at(0xAA00); push(peek(0x01D2)); k.add(36);
	k.at(0xAA04); push(peek(0x01D4)); k.add(36);
	sel_b(k, 0xAA08, 0x2800);
	T.r[2] = a.u(0); k.add(27);
	k.at(0xAA18);
	const bool block = (int16_t)peek(T.r[2]) >= 0x4000;
	k.add(24, 12);
	if (block)
	{
		k.at(0xAA1E); push(peek(T.r[2])); k.add(30);
		push(0); k.add(24);
		k.call(0xEAD4, 0xAA26);                        // draw_tile_block
		T.r[2] = (uint16_t)(T.r[2] + 2); k.add(18, 12);
	}
	else
	{
		sel_a(k, 0xAA2C, 0x3800);
		push(T.r[2]); k.add(24);
		push(0); k.add(24);
		k.call(0x4D30, 0xAA40);
		T.r[2] = (uint16_t)(T.r[2] + 4); k.add(18);
	}
	// the level class bits of the current rating entry (0x0262 by 0x0F31 / 0x0F2B) into R0
	auto rating_class = [&k](uint16_t pc) {
		k.at(pc);
		const bool two = peekb(0x0F2B) == 2;
		k.add(30, 12);
		T.r[0] = 0; k.add(12);
		k.at((uint16_t)(two ? pc + 0x0A : pc + 0x12)); T.r[0] = peekb(0x0F31); k.add(24);
		if (two)
			k.add(12);
		else
		{
			T.r[0]--;
			k.add(12);
		}
		k.at((uint16_t)(pc + 0x18)); T.r[0] = peekb((uint16_t)(0x0262 + T.r[0])); k.add(27, 18);
		T.r[0] = (uint16_t)((T.r[0] >> 6) & 3); k.add(12, 12, 12, 12, 12, 12, 18);
	};
	for (;;)
	{
		k.at(0xAA44); poke(0x020C, peek(T.r[2])); T.r[2] = (uint16_t)(T.r[2] + 2); k.add(33);
		k.at(0xAA48);
		const bool end = peek(0x020C) == 0xFFFF;
		k.add(30, 12);
		if (end)
		{
			k.add(18);
			break;
		}
		T.r[3] = 0; k.add(12);
		k.at(0xAA56); T.r[3] = peekb(0x020C); k.add(24, 12);
		if (!(T.r[3] & 0x80))
		{
			T.r[0] = 0; k.add(12);
			k.at(0xAA5E);
			const uint8_t how = peekb(0x020D);
			T.r[0] = how;
			k.add(24);
			T.r[0] = (uint16_t)(T.r[0] << 1); k.add(12, 27);
			switch (how)
			{
			case 0:
				T.r[0] = T.r[3]; k.add(12, 12);
				k.call(0x8AB6, 0xAB06);                // draw_string_large
				break;
			case 1:
				T.r[0] = T.r[3]; k.add(12);
				k.call(0x8B04, 0xAA72);                // draw_string_small
				break;
			case 2:
				rating_class(0xAA74);
				T.r[0] = (uint16_t)(T.r[0] + T.r[3] - 1); k.add(12, 12, 12);
				k.call(0x8AB6, 0xAB06);
				break;
			case 3:
				k.call(0x5326, 0xAAAE);
				T.r[0] = (uint16_t)(T.r[0] + T.r[3]); k.add(12, 12);
				k.call(0x8AB6, 0xAB06);
				break;
			case 4:
			case 5:
				T.r[0] = 0; k.add(12);
				k.at(how == 4 ? 0xAAB4 : 0xAABC); T.r[0] = peekb(how == 4 ? 0x02B8 : 0x0261); k.add(24, 12);
				T.r[0] = (uint16_t)(T.r[0] + T.r[3]); k.add(12, 12);
				k.call(0x8AB6, 0xAB06);
				break;
			case 6:
				k.call(0x9152, 0xAAC6);                // next_random
				T.r[1] = T.r[0] & 1; k.add(12, 18);
				T.r[4] = (uint16_t)(T.r[1] << 1); k.add(12, 12);
				T.r[0] = 0; k.add(12);
				k.at(0xAAD2); T.r[0] = (uint16_t)(peekb(0x020C) + T.r[4]); k.add(24, 12);
				k.call(0x8AB6, 0xAADC);
				T.r[0] = 0; k.add(12);
				k.at(0xAADE); T.r[0] = (uint16_t)(peekb(0x020C) + T.r[4] + 1); k.add(24, 12, 12, 12);
				k.call(0x8AB6, 0xAB06);
				break;
			case 7:
				k.at(0xAAE6); T.r[1] = (uint16_t)((peek(0x020A) & 1) << 1); k.add(24, 18, 12);
				T.r[0] = (uint16_t)(T.r[1] + T.r[3]); k.add(12, 12);
				k.call(0x8AB6, 0xAAF8);
				T.r[0] = 0; k.add(12);
				k.at(0xAAFA); T.r[0] = (uint16_t)(peekb(0x020C) + T.r[1] + 1); k.add(24, 12, 12);
				k.call(0x8AB6, 0xAB06);
				break;
			default:                                   // past the table (next table's data): unused
				continue;
			}
			k.add(12);
			continue;
		}
		T.r[0] = (uint16_t)((T.r[3] - 0x80) << 1); k.add(12, 18, 12, 27);
		switch (T.r[3] - 0x80)
		{
		case 0:
			T.r[1] = 0; k.add(12, 12);
			break;
		case 1: case 2: case 3: case 4:
			k.at(0xAB14); T.r[5] = sext8(peekb(0x027A)); k.add(24);
			T.r[0] = (uint16_t)(T.r[3] - 0x81); k.add(12, 18);
			k.at(0xAB1E); pokeb(0x027A, (uint8_t)T.r[0]); k.add(27);
			k.call(0x90BA, 0xAB26);                    // park_level
			T.r[1] = T.r[0]; k.add(12);
			k.at(0xAB28); pokeb(0x027A, (uint8_t)T.r[5]); k.add(27, 12);
			break;
		case 5: case 6: case 7:
			k.at(0xAB2E); T.r[1] = peek(0x0F38); k.add(24, 12);
			break;
		case 8:
			rating_class(0xAB34);
			poke(T.r[6], 0); k.add(21);
			k.at(0xAB66); poke(T.r[6], peekb((uint16_t)(0xFE8C + T.r[0]))); k.add(36);
			k.call(0x90BA, 0xAB6E);
			push(T.r[0]); k.add(24);
			k.call(0x80D8, 0xAB74);                    // mul16
			T.r[1] = T.r[0];
			T.r[6] = (uint16_t)(T.r[6] + 2);
			k.add(12, 18, 12);
			break;
		case 9:
			T.r[0] = 0; k.add(12);
			k.at(0xAB7C); T.r[0] = peekb(0x0F31); k.add(24);
			k.at(0xAB80); T.r[0] = peekb((uint16_t)(0xFE7C + T.r[0])); k.add(27, 18);
			T.r[1] = 0; k.add(12);
			k.at(0xAB8A); T.r[1] = peekb((uint16_t)(0xFE70 + T.r[0])); k.add(27, 12);
			break;
		case 10:
			T.r[1] = 0; k.add(12);
			k.at(0xAB92); T.r[1] = (uint16_t)(peekb(0x01F9) >> 2); k.add(24, 12, 12, 12);
			break;
		case 11:
			k.call(0x90BA, 0xABA0);
			T.r[1] = T.r[0]; k.add(12, 12);
			break;
		case 12:
			T.r[1] = 0; k.add(12);
			k.at(0xABA6); T.r[1] = (uint16_t)(peekb(0x01F5) + 1); k.add(24, 12, 12);
			break;
		case 13:
			T.r[1] = 2; k.add(18);
			T.r[0] = 0; k.add(12);
			k.at(0xABB4); T.r[0] = peekb(0x01F5); k.add(24);
			T.r[1] = (uint16_t)(T.r[1] - T.r[0]); k.add(12, 12);
			break;
		case 14: case 15: case 16: case 17:            // the bytes 0x025C-0x025F
			T.r[1] = 0; k.add(12);
			k.at(0xABBE); T.r[1] = peekb((uint16_t)(0x01CE + T.r[3])); k.add(27, 12);
			if (T.r[1] & 0x80)
			{
				T.r[1] = 0;
				k.add(12, 12);
			}
			break;
		case 18:
			k.at(0xABC8); T.r[1] = peek(0x0248); k.add(24);
			break;
		default:                                       // past the table: unused
			continue;
		}
		T.r[4] = 0; k.add(12);
		k.at(0xABCE); T.r[4] = peekb(0x020D); k.add(24, 18, 12);
		if (T.r[4] & 0x00F8)
		{
			T.r[0] = (uint16_t)((T.r[4] >> 1) & ~3); k.add(12, 12, 18);
			k.at(0xABE0); T.r[0] = (uint16_t)(T.r[0] + peek(0x0186)); k.add(24);
			k.at(0xABE4); poke(0x0186, T.r[0]); k.add(27);
			k.at(0xABE8); poke(0x0186, (uint16_t)(peek(0x0186) - 0x0040)); k.add(33, 12);
		}
		else
		{
			k.at(0xABF0); poke(0x0186, peek(T.r[2])); T.r[2] = (uint16_t)(T.r[2] + 2); k.add(33);
			k.at(0xABF4); poke(0x0186, (uint16_t)(peek(0x0186) + 0x2000)); k.add(33);
		}
		const uint16_t format = T.r[4] & 7;
		T.r[4] = (uint16_t)(format << 1); k.add(18, 12, 27);
		switch (format)
		{
		case 0:
			k.add(18, 12);
			if ((int16_t)T.r[1] < 10)
			{
				T.r[4] = (uint16_t)(T.r[1] + 0x30);
				T.r[0] = T.r[4];
				k.add(12, 18, 12, 12);
			}
			else
			{
				T.r[0] = 0x003D;
				k.add(18);
			}
			k.call(0x8B52, 0xAC1C);                    // draw_large_char
			k.add(12);
			break;
		case 1:
		{
			k.at(0xAC1E); T.r[0] = peek(0x0186); k.add(24);
			k.at(0xAC22); T.r[4] = peek(0x0188); k.add(24);
			k.at(0xAC26); poke(T.r[0], T.r[4]); k.add(21);
			k.at(0xAC28); poke(T.r[0], (uint16_t)(peek(T.r[0]) | 0x0138)); k.add(27);
			k.at(0xAC2C); poke((uint16_t)(T.r[0] + 2), T.r[4]); k.add(30);
			k.at(0xAC30); poke((uint16_t)(T.r[0] + 2), (uint16_t)(peek((uint16_t)(T.r[0] + 2)) | 0x0139)); k.add(36);
			k.at(0xAC36); poke((uint16_t)(T.r[0] + 0x80), T.r[4]); k.add(30);
			k.at(0xAC3A); poke((uint16_t)(T.r[0] + 0x80), (uint16_t)(peek((uint16_t)(T.r[0] + 0x80)) | 0x013A)); k.add(36);
			k.at(0xAC40); poke((uint16_t)(T.r[0] + 0x82), T.r[4]); k.add(30);
			k.at(0xAC44); poke((uint16_t)(T.r[0] + 0x82), (uint16_t)(peek((uint16_t)(T.r[0] + 0x82)) | 0x013B)); k.add(36);
			k.at(0xAC4A); poke(0x0186, (uint16_t)(peek(0x0186) + 4)); k.add(33);
			poke(T.r[6], T.r[1]); k.add(21);
			push(0); k.add(24);
			push(3); k.add(30);
			push(0); k.add(24, 12);
			push(1); k.add(30);
			k.call(0xBA9A, 0xACC0);
			T.r[6] = (uint16_t)(T.r[6] + 8); k.add(18);
			break;
		}
		case 2:
			poke(T.r[6], T.r[1]); k.add(21);
			push(0); k.add(24);
			push(4); k.add(30, 12);
			push(1); k.add(30);
			push(1); k.add(30);
			k.call(0xBA9A, 0xACC0);
			T.r[6] = (uint16_t)(T.r[6] + 8); k.add(18);
			break;
		case 3:
			poke(T.r[6], T.r[1]); k.add(21);
			push(0); k.add(24);
			push(2); k.add(30);
			push(1); k.add(30);
			push(0); k.add(24, 12);
			k.call(0xBA9A, 0xACC0);
			T.r[6] = (uint16_t)(T.r[6] + 8); k.add(18);
			break;
		case 4:
			k.at(0xAC76); poke(T.r[6], peek(0x023E)); k.add(33);
			k.at(0xAC7A); push(peek(0x0240)); k.add(36);
			push(6); k.add(30);
			push(0); k.add(24);
			push(1); k.add(30, 12);
			k.call(0xBA96, 0xAC9C);
			T.r[6] = (uint16_t)(T.r[6] + 8); k.add(12, 18);
			break;
		case 5:
			k.at(0xAC8A); poke(T.r[6], peek(0x0242)); k.add(33);
			push(0); k.add(24);
			push(3); k.add(30);
			push(0); k.add(24);
			push(0); k.add(24);
			k.call(0xBA96, 0xAC9C);
			T.r[6] = (uint16_t)(T.r[6] + 8); k.add(12, 18);
			break;
		case 6:
			T.r[4] = (uint16_t)(T.r[1] + 0x30);
			T.r[0] = T.r[4];
			k.add(12, 18, 12);
			k.call(0x8C12, 0xACAA);                    // put_small_char
			k.add(12);
			break;
		case 7:
			poke(T.r[6], T.r[1]); k.add(21);
			push(0); k.add(24);
			push(5); k.add(30);
			push(1); k.add(30);
			push(1); k.add(30);
			k.call(0xBA9A, 0xACC0);
			T.r[6] = (uint16_t)(T.r[6] + 8); k.add(18);
			break;
		}
		k.add(18);                                     // JMP back to the next command
	}
	const uint16_t sp = T.r[6];
	k.at(0xACC8); poke(0x01D2, rw((uint16_t)(sp + 6))); k.add(42);
	k.at(0xACCE); poke(0x01D4, rw((uint16_t)(sp + 4))); k.add(42);
	k.at(0xACD4); poke(0x1400, peek(0x01D2)); k.add(39);
	k.at(0xACDA); poke(0x1402, peek(0x01D4)); k.add(39);
	T.r[6] = (uint16_t)(T.r[6] + 8);
	k.add(18);
	k.restore(5);
	k.leave();
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
	k.add(24, 12);
	if (!timer)
	{
		k.at(0xA51A); poke(0x023A, peek(0x03F0)); k.add(39);
		k.at(0xA520); poke(0x0238, peek(0x0670)); k.add(39, 12);
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
	k.call(0x5684, 0xA556);
	T.r[1] = T.r[0]; k.add(12);
	k.at(0xA558); poke(0x01D2, 0); k.add(27);
	k.at(0xA55C); poke(0x1400, 0); k.add(27, 12, 12);
	auto finish = [&]() {                              // A8CC: ADD #6,SP; RTS
		k.add(18);
		T.r[6] = fl::add(T.r[6], 6);
		k.leave();
	};
	auto store_object = [&]() {                        // A8B4: MOVB R1,@#0x0334; BR
		k.at(0xA8B4); pokeb(0x0334, (uint8_t)T.r[1]); k.add(27, 12);
		finish();
	};
	if ((int16_t)T.r[1] <= 0)
	{
		k.add(18);
		k.at(0xA8BA);
		const uint8_t t = peekb(0x0EA2);
		k.add(24, 12);
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
	k.add(36, 30, 12);
	auto kind = [fp]() -> uint16_t { return peekb(peek((uint16_t)(fp + 2))); };
	k.at(0xA574); T.r[0] = kind(); k.add(33);
	poke((uint16_t)(fp + 4), 0); k.add(30);
	k.at(0xA57C);
	const bool ramp = peekb(0x01EA) != 0;
	k.add(24, 12);
	if (ramp)
	{
		poke((uint16_t)(fp + 4), (uint16_t)(peek((uint16_t)(fp + 4)) + 1));
		k.add(30, 12);
	}
	else
	{
		k.at(0xA588); T.r[3] = peek(0x06B2); k.add(24);
		k.at(0xA58C); T.r[3] = (uint16_t)(T.r[3] - peek(0x06B0)); k.add(24);
		T.r[2] = 0; k.add(12);
		k.at(0xA592); T.r[2] = peekb((uint16_t)(0x6631 + T.r[0])); k.add(27);
		T.r[2] = (uint16_t)(T.r[2] << 8); k.add(12, 12, 12, 12);
		if ((int16_t)T.r[3] > (int16_t)T.r[2])
		{
			k.add(18);
			finish();
			return;
		}
	}
	k.at(0xA5A2); T.r[0] = peekb((uint16_t)(0x61DA + T.r[0])); k.add(27, 18);
	T.r[2] = (uint16_t)(T.r[0] << 1); k.add(12, 12, 27);
	// CLR R0, JSR F:AD70 (object 0 in front of R1?), ROR, BCC, MOV #4 / #12: the direction
	auto facing = [&](uint16_t jsr, uint16_t ret) -> uint16_t {
		T.r[0] = 0; k.add(12);
		k.call(0xAD70, ret);
		(void)jsr;
		const bool front = (T.r[0] & 1) != 0;
		T.r[0] = ror(T.r[0]);
		k.add(12, 12, 18, (front ? 12 : 0));
		return front ? 4 : 12;
	};
	switch (T.r[0])
	{
	case 2: case 7:
	{
		T.r[0] = 0; k.add(12);
		k.at(0xA5BA); T.r[0] = peekb(0x0334); k.add(24, 12, 12);
		if (T.r[0] == T.r[1])
		{
			k.add(18);
			store_object();
			return;
		}
		k.at(0xA5C2);
		const bool riding = peekb(0x031E) != 0;
		k.add(24, 12);
		if (!riding)
		{
			T.r[0] = facing(0xA5CA, 0xA5CE);
			k.at(0xA5DC); pokeb(0x031D, (uint8_t)T.r[0]); k.add(27);
		}
		k.at(0xA5E0); pokeb(0x01F8, peekb(0x031D)); k.add(39);
		k.at(0xA5E6); poke(0x01B4, 0x0514); k.add(33);
		k.call(0xA2F0, 0xA5F0);                 // skater_land
		T.r[0] = 0; k.add(12);
		k.at(0xA5F2); T.r[0] = kind(); k.add(33, 18, 12);
		if (T.r[0] != 4)
		{
			k.add(18);
			store_object();
			return;
		}
		push(0); k.add(24);
		k.at(0xA5FE); poke(T.r[6], peekb((uint16_t)(0x0390 + T.r[1]))); k.add(36);
		k.call(0xCF5E, 0xA606);
		k.call(0xE222, 0xA60A);                 // state1_to_8
		k.add(12, 12, 12, 12, 18);                            // BR, BR, BR, BR, TST (SP)+
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(rw((uint16_t)(T.r[6] - 2)));
		store_object();
		return;
	}
	case 3: case 4:
	{
		push(0x0067); k.add(30);
		k.call(0x8E78, 0xA6AE);                 // queue_sound
		T.r[2] = (uint16_t)(T.r[1] << 1); k.add(12, 12);
		k.at(0xA6B2); poke((uint16_t)(0x03F0 + T.r[2]), 0); k.add(30);
		poke(T.r[6], T.r[1]); k.add(21);
		push(0); k.add(24);
		k.at(0xA6BA); poke(T.r[6], peekb(0x5CDE)); k.add(33);
		k.call(0xB602, 0xA6C2);
		k.at(0xA6C2); pokeb((uint16_t)(0x04B0 + T.r[1]), (uint8_t)(peekb((uint16_t)(0x04B0 + T.r[1])) | 0x08)); k.add(36);
		T.r[0] = 7; k.add(18);
		k.at(0xA6CC); pokeb((uint16_t)(0x0790 + T.r[1]), 7); k.add(30);
		k.at(0xA6D0); pokeb((uint16_t)(0x0770 + T.r[1]), 7); k.add(30);
		k.at(0xA6D4); poke((uint16_t)(0x0430 + T.r[2]), 0x0080); k.add(36);
		T.r[0] = 0; k.add(12);
		k.at(0xA6DC); T.r[0] = kind(); k.add(33);
		T.r[0] = (uint16_t)((T.r[0] - 0x48) << 1); k.add(18, 12);
		k.at(0xA6E6); poke(T.r[6], peek((uint16_t)(0xFCFA + T.r[0]))); k.add(36);
		k.call(0xEF24, 0xA6EE);
		fl::cmp(rw(T.r[6]), rw((uint16_t)(T.r[6] + 2)));
		T.r[6] = (uint16_t)(T.r[6] + 4);
		k.add(24, 12, 18);
		store_object();
		return;
	}
	case 5:
	{
		k.at(0xA6F2);
		const bool carried = peekb(0x0EA0) != 0;
		k.add(24, 12);
		if (!carried)
		{
			k.at(0xA6F8);
			const bool mode1 = peekb(0x031F) == 1;
			k.add(30, 12);
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
		k.at(0xA60E); T.r[0] = peekb(0x0334); k.add(24, 12, 12);
		if (T.r[0] == T.r[1])
		{
			k.add(18);
			store_object();
			return;
		}
		k.at(0xA616); T.r[0] = peek(0x06B2); k.add(24);
		k.at(0xA61A); T.r[0] = (uint16_t)(T.r[0] - peek(0x06B0)); k.add(24, 18, 12);
		if ((int16_t)T.r[0] < 0x0380)
		{
			T.r[0] = facing(0xA626, 0xA62A);
			k.at(0xA638); pokeb(0x031D, (uint8_t)T.r[0]); k.add(27);
			k.at(0xA63C); pokeb(0x01F8, (uint8_t)T.r[0]); k.add(27);
			k.at(0xA640); poke(0x01B4, 0x0514); k.add(33);
			k.call(0xA2F0, 0xA64A);             // skater_land
			k.add(12);
		}
		else
		{
			T.r[0] = 0; k.add(12);
			k.at(0xA64E); T.r[0] = peekb(0x0E6C); k.add(24, 12, 12);
			if (T.r[0])
			{
				push(T.r[0]); k.add(24);
				k.call(0xF064, 0xA65C);
				T.r[0] = 0; k.add(12);
				k.at(0xA65E); T.r[0] = peekb(0x0E6C); k.add(24);
				T.r[0] = (uint16_t)(T.r[0] << 1); k.add(12);
				k.at(0xA664); poke(T.r[6], peek((uint16_t)(0xFCFA + T.r[0]))); k.add(36);
				k.call(0xEF24, 0xA66C);
				k.at(0xA66C);
				const bool same = kind() == peekb(0x0E6B);
				k.add(45, 12);
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
		k.call(0x4ECC, 0xA69E);
		poke(T.r[6], T.r[1]); k.add(21);
		k.call(0xD47C, 0xA6A4);
		k.add(12, 12, 12, 18);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(rw((uint16_t)(T.r[6] - 2)));
		store_object();
		return;
	}
	case 8:
	{
		push(0x0061); k.add(30);
		k.call(0x8E78, 0xA714);                 // queue_sound
		T.r[0] = 0; k.add(12);
		k.at(0xA716); T.r[0] = kind(); k.add(33);
		T.r[0] = (uint16_t)(T.r[0] - 0x38); k.add(18);
		k.at(0xA71E); T.r[2] = sext8(peekb((uint16_t)(0xFD10 + T.r[0]))); k.add(27);
		poke(T.r[6], T.r[2]); k.add(21);
		k.call(0xF002, 0xA728);
		poke(T.r[6], 0); k.add(21);
		k.at(0xA72A); poke(T.r[6], peekb((uint16_t)(0x0390 + T.r[1]))); k.add(36);
		k.call(0xCF5E, 0xA732);
		k.add(12, 12, 18);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(rw((uint16_t)(T.r[6] - 2)));
		store_object();
		return;
	}
	case 9:
	{
		T.r[0] = 0; k.add(12);
		k.at(0xA736); T.r[0] = kind(); k.add(33, 18, 12);
		if (T.r[0] == 0x14)
		{
			T.r[0] = 7;
			k.add(18, 12);
		}
		else
		{
			T.r[0] = 0x0F;
			k.add(18);
		}
		k.at(0xA74A); pokeb(0x0329, (uint8_t)T.r[0]); k.add(27);
		T.r[2] = 0; k.add(12);
		k.at(0xA750); T.r[2] = peekb(0x0328); k.add(24, 12, 12);
		if (T.r[2] == T.r[0])
		{
			k.add(18);
			store_object();
			return;
		}
		k.at(0xA758); pokeb(0x0328, (uint8_t)T.r[0]); k.add(27, 33, 12);
		if (peek((uint16_t)(fp + 4)) & 1)
		{
			push(T.r[0]); k.add(24);
			k.call(0x9E52, 0xA76A);             // slope_table_value
			poke(T.r[6], T.r[0]); k.add(21);
			k.call(0x9EEE, 0xA770);             // skater_deflect
		}
		else
		{
			push(0x0008); k.add(30);
			T.r[0] = 0; k.add(12);
			k.at(0xA778); T.r[0] = peekb(0x031D); k.add(24);
			poke(T.r[6], (uint16_t)(rw(T.r[6]) ^ T.r[0])); k.add(21);
			k.call(0x9E96, 0xA782);             // skater_bounce
		}
		k.add(12, 12, 18);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(rw((uint16_t)(T.r[6] - 2)));
		store_object();
		return;
	}
	case 10:
	{
		push(T.r[1]); k.add(24);
		k.call(0xA8D2, 0xA78A);
		T.r[4] = T.r[0]; k.add(12);
		T.r[0] = 0; k.add(12);
		k.at(0xA78E); T.r[0] = kind(); k.add(33);
		T.r[0] = (uint16_t)(T.r[0] - 0x1A); k.add(18);
		T.r[5] = (uint16_t)(T.r[4] << 1); k.add(12, 12);
		T.r[2] = (uint16_t)(0x03B0 + T.r[5]); k.add(18, 12);
		poke(fp, T.r[1]);
		poke(fp, (uint16_t)(T.r[1] << 1)); k.add(30, 30);
		push(0x03B0);
		poke(T.r[6], (uint16_t)(rw(T.r[6]) + rw((uint16_t)(T.r[6] + 4))));
		T.r[3] = pop(); k.add(30, 36, 18);
		bool far = true;
		k.add(18, 12);
		if ((int16_t)T.r[0] >= 5) k.add(18, 12);
		if ((int16_t)T.r[0] < 5 || (int16_t)T.r[0] > 7)
		{
			T.r[0] = 0; k.add(12);
			k.call(0xAD70, 0xA7C4);
			far = !(T.r[0] & 1);
			T.r[0] = ror(T.r[0]);
			k.add(12, 12);
		}
		if (!far)
		{
			k.at(0xA7C8); poke(T.r[2], (uint16_t)(peek(T.r[2]) + 2)); k.add(27);
			k.at(0xA7CC); poke(T.r[3], (uint16_t)(peek(T.r[3]) + 2)); k.add(27);
			T.r[0] = 0x0018; k.add(18, 12);
		}
		else
		{
			k.at(0xA7D6); poke(0x0F20, (uint16_t)(peek(0x0F20) + 6)); k.add(33);
			k.at(0xA7DC);
			const bool over = (int16_t)peek(0x0F20) > 0x0080;
			k.add(30, 12);
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
		k.add(12, 18);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(rw((uint16_t)(T.r[6] - 2)));
		store_object();
		return;
	}
	case 11:
	{
		push(T.r[1]); k.add(24);
		k.call(0xA8D2, 0xA828);
		T.r[3] = T.r[0]; k.add(12);
		T.r[4] = (uint16_t)(T.r[3] << 1); k.add(12, 12);
		k.at(0xA82E); poke((uint16_t)(0x03B0 + T.r[4]), (uint16_t)(peek((uint16_t)(0x03B0 + T.r[4])) + 2)); k.add(36);
		T.r[0] = (uint16_t)(T.r[1] << 1); k.add(12, 12);
		k.at(0xA838); poke((uint16_t)(0x03B0 + T.r[0]), (uint16_t)(peek((uint16_t)(0x03B0 + T.r[0])) + 2)); k.add(36);
		T.r[5] = 0x0018; k.add(18);
		k.at(0xA842); pokeb(peek((uint16_t)(fp + 2)), 0x18); k.add(36);
		k.at(0xA846); pokeb((uint16_t)(0x0370 + T.r[3]), 0x18); k.add(30);
		k.at(0xA84A); pokeb((uint16_t)(0x0490 + T.r[1]), (uint8_t)(peekb((uint16_t)(0x0490 + T.r[1])) | 0x18)); k.add(36);
		k.at(0xA850); pokeb((uint16_t)(0x0490 + T.r[3]), (uint8_t)(peekb((uint16_t)(0x0490 + T.r[3])) | 0x18)); k.add(36);
		k.at(0xA856); poke((uint16_t)(0x0430 + T.r[0]), 0x0010); k.add(36);
		k.at(0xA85C); poke((uint16_t)(0x0430 + T.r[4]), 0x000F); k.add(36);
		k.add(12, 18);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(rw((uint16_t)(T.r[6] - 2)));
		store_object();
		return;
	}
	case 12:
	{
		k.at(0xA864);
		const bool level = peek(0x06B2) == peek(0x06B0);
		k.add(36, 12);
		if (!level)
		{
			store_object();
			return;
		}
		k.at(0xA86C); T.r[0] = peek(0x01B4); k.add(24, 12);
		if ((int16_t)T.r[0] < 0)
		{
			T.r[0] = (uint16_t)-T.r[0];
			k.add(12);
		}
		k.add(18, 12);
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
		k.at(0xA898); poke(0x05B2, (uint16_t)(peek(0x05B2) + peek((uint16_t)(0x7CA4 + T.r[0])))); k.add(42, 33, 12);
		if (peek((uint16_t)(fp + 4)) & 1)
		{
			k.call(0x9F8A, 0xA8AA);             // skater_jump
		}
		push(0x007B); k.add(30);
		k.call(0x8E78, 0xA8B2);                 // queue_sound
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
		k.add(12, 18);
		store_object();
		return;
	}
}

// F:9E96  skater_bounce(int dir): new direction dir (0x3F: the opposite one), speed 0x160,
// motion values for that direction (F:9B82 with terrain 0x0327 cleared, F:91A6, then again
// with the terrain), no upward z velocity for object 1; state 0x031E = 2 for 6 (0x031C)
uint16_t skater_bounce(const Args &a)
{
	Clock k;
	push(T.r[1]); k.add(24);
	k.add(33, 12);
	if (a.u(0) == 0x003F)
	{
		T.r[1] = 8; k.add(18);
		k.at(0x9EA4); T.r[0] = sext8(peekb(0x031D)); k.add(24);
		T.r[1] ^= T.r[0]; k.add(12);
		k.at(0x9EAA); pokeb(0x031D, (uint8_t)T.r[1]); k.add(27, 12);
	}
	else
	{
		k.at(0x9EB0); pokeb(0x031D, a.b(0)); k.add(42);
	}
	k.at(0x9EB6); poke(0x01B4, 0x0160); k.add(33);
	k.at(0x9EBC); T.r[1] = sext8(peekb(0x0327)); k.add(24);
	k.at(0x9EC0); pokeb(0x0327, 0); k.add(27);
	k.call(0x9B82, 0x9EC8);                            // skater_motion_params
	k.call(0x91A6, 0x9ECC);                            // skater_velocity
	k.at(0x9ECC); pokeb(0x0327, (uint8_t)T.r[1]); k.add(27);
	k.call(0x9B82, 0x9ED4);
	k.at(0x9ED4);
	const bool up = (int16_t)peek(0x05B2) > 0;
	k.add(24, 12);
	if (up)
	{
		k.at(0x9EDA); poke(0x05B2, 0); k.add(27);
	}
	k.at(0x9EDE); pokeb(0x031E, 2); k.add(33);
	k.at(0x9EE4); pokeb(0x031C, 6); k.add(33);
	T.r[1] = pop();
	k.add(18);
	k.leave();
	return T.r[0];
}

// F:9EEE  skater_deflect(int dir): heading 0x031B = dir (0x3F: the current direction, with the
// speed reversed to 0x160 the other way); otherwise the speed keeps its sign and is at least
// 0x160 (a negative speed takes the opposite heading).  On a ramp (0x01EA == 2) F:A2F0 lands
// the skater; mode 0x031F == 1 turns a negative speed around.  State 0x031E = 1 for 8.
uint16_t skater_deflect(const Args &a)
{
	Clock k;
	k.save(0x9EEE, 2);
	const uint16_t dir = a.u(0);
	k.at(0x9EF2); T.r[1] = peek(0x01B4); k.add(24, 33, 12);
	const uint16_t speed = T.r[1];
	bool positive, force;
	if (dir == 0x003F)
	{
		k.at(0x9EFE); pokeb(0x031B, peekb(0x031D)); k.add(39, 12, 12);
		positive = (int16_t)speed < 0;
		force = true;
		if (positive) k.add(12);
	}
	else
	{
		k.at(0x9F0A); pokeb(0x031B, (uint8_t)dir); k.add(42, 12, 12);
		positive = (int16_t)speed >= 0;
		force = false;
		if (!positive)
		{
			T.r[2] = 8; k.add(18);
			T.r[0] = sext8((uint8_t)dir); k.add(27);
			T.r[2] ^= T.r[0]; k.add(12);
			k.at(0x9F1E); pokeb(0x031B, (uint8_t)T.r[2]); k.add(27, 18, 12);
		}
	}
	if (positive)
	{
		if (!force) k.add(18, 12);
		if (force || (int16_t)speed < 0x0160)
		{
			k.at(0x9F36); poke(0x01B4, 0x0160); k.add(33);
		}
	}
	else if (force || (int16_t)speed > (int16_t)0xFEA0)
	{
		k.at(0x9F28); poke(0x01B4, 0xFEA0); k.add(33, 12);
	}
	k.at(0x9F3C);
	const bool ramp2 = peekb(0x01EA) == 2;
	k.add(30, 12);
	if (ramp2)
	{
		k.at(0x9F44);
		const bool back = (int16_t)peek(0x01B4) < 0;
		k.add(24, 12);
		if (back)
		{
			T.r[1] = 8; k.add(18);
			k.at(0x9F4E); T.r[0] = sext8(peekb(0x031B)); k.add(24);
			T.r[1] ^= T.r[0]; k.add(12);
			k.at(0x9F54); pokeb(0x031B, (uint8_t)T.r[1]); k.add(27);
		}
		k.call(0xA2F0, 0x9F5C);                        // skater_land
	}
	k.at(0x9F5C);
	const bool mode1 = peekb(0x031F) == 1;
	k.add(30, 12);
	if (mode1)
	{
		k.at(0x9F64);
		const bool back = (int16_t)peek(0x01B4) < 0;
		k.add(24, 12);
		if (back)
		{
			T.r[1] = 8; k.add(18);
			k.at(0x9F6E); T.r[0] = sext8(peekb(0x031B)); k.add(24);
			T.r[1] ^= T.r[0]; k.add(12);
			k.at(0x9F74); pokeb(0x031B, (uint8_t)T.r[1]); k.add(27);
			k.at(0x9F78); poke(0x01B4, (uint16_t)-peek(0x01B4)); k.add(27);
		}
	}
	k.at(0x9F7C); pokeb(0x031E, 1); k.add(33);
	k.at(0x9F82); pokeb(0x031C, 8); k.add(33);
	k.restore(2);
	k.leave();
	return T.r[0];
}

// F:AD70 (assembly, objects R0 and R1): R0 = 1 when object R0 is drawn in front of object
// R1.  Objects 0-3 (the skater) keep their order and stand for the skater against others;
// the object carrying the skater (0x0EA0) is behind it.  R1-R4 preserved (F152); C is only
// set by the comparison of two skater objects.
void sprite_in_front()
{
	Clock k;
	k.save(0xAD70, 4);
	T.r[0] &= 0x001F; k.add(18);
	T.r[1] &= 0x001F; k.add(18);
	int carry = 0;
	// ADB0: screen order of objects R0 / R1 from their positions and animation frames
	auto in_order = [&k]() {
		T.r[0] = (uint16_t)(T.r[0] << 1); k.add(12);
		k.at(0xADB2); T.r[2] = peek((uint16_t)(0x03F0 + T.r[0])); k.add(27, 12);
		if (!T.r[2])
		{
			T.r[0] = 1; k.add(18);
			return;
		}
		T.r[1] = (uint16_t)(T.r[1] << 1); k.add(12);
		k.at(0xADBA); T.r[3] = peek((uint16_t)(0x03F0 + T.r[1])); k.add(27, 12);
		if (!T.r[3])
		{
			T.r[0] = 0; k.add(12);
			return;
		}
		k.at(0xADC0); T.r[4] = peek((uint16_t)(0x0630 + T.r[0])); k.add(27);
		k.at(0xADC4); T.r[4] = (uint16_t)(T.r[4] + peek((uint16_t)(T.r[2] + 8))); k.add(27);
		k.at(0xADC8); T.r[4] = (uint16_t)(T.r[4] - peek((uint16_t)(0x0630 + T.r[1]))); k.add(27);
		k.at(0xADCC); T.r[4] = (uint16_t)(T.r[4] - peek((uint16_t)(T.r[3] + 8))); k.add(27, 12);
		const bool left = (T.r[4] & 0x8000) != 0;
		const uint16_t pc = left ? 0xADD2 : 0xADE4;
		k.at(pc); T.r[0] = peek((uint16_t)(0x0670 + T.r[0])); k.add(27);
		k.at((uint16_t)(pc + 4)); T.r[0] = (uint16_t)(T.r[0] + peek((uint16_t)(T.r[2] + (left ? 0x0C : 0x0A)))); k.add(27);
		k.at((uint16_t)(pc + 8)); T.r[0] = (uint16_t)(T.r[0] - peek((uint16_t)(0x0670 + T.r[1]))); k.add(27);
		k.at((uint16_t)(pc + 12)); T.r[0] = (uint16_t)(T.r[0] - peek((uint16_t)(T.r[3] + (left ? 0x0A : 0x0C)))); k.add(27);
		if (left) k.add(12);                           // BR
		k.add(12, 12);
		if (T.r[0] & 0x8000)
		{
			T.r[0] = 0; k.add(12);
		}
		else
		{
			T.r[0] = 1; k.add(18);
		}
	};
	k.add(18, 12);
	if (T.r[0] < 4)
	{
		k.add(18, 12);
		if (T.r[1] < 4)
		{
			// two skater objects keep their order
			T.r[2] = 0; k.add(12, 12, 12);
			carry = T.r[0] < T.r[1] ? CF : 0;
			if (carry)
			{
				T.r[2]++; k.add(12);
			}
			T.r[0] = T.r[2]; k.add(12);
		}
		else
		{
			k.at(0xAD94); T.r[2] = sext8(peekb(0x0EA0)); k.add(24, 12, 12);
			if (T.r[1] == T.r[2])
			{
				T.r[0] = 0; k.add(12);
			}
			else
			{
				T.r[0] = 0; k.add(12, 12);
				in_order();
			}
		}
	}
	else
	{
		k.add(18, 12);
		if (T.r[1] >= 4)
			in_order();
		else
		{
			k.at(0xADA6); T.r[2] = sext8(peekb(0x0EA0)); k.add(24, 12, 12);
			if (T.r[0] == T.r[2])
			{
				T.r[0] = 1; k.add(18);
			}
			else
			{
				T.r[1] = 0; k.add(12);
				in_order();
			}
		}
	}
	k.restore(4);
	f_nzvc(nzw(T.r[1]) | carry);
	k.leave();
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
	Clock k;
	k.save(0x9C5E, 5);
	T.r[6] = (uint16_t)(T.r[6] - 2); k.add(21);       // TST -(SP): the slot of the position
	push(T.r[1]); k.add(24);                           // the column
	const uint16_t sp = T.r[6];
	ww((uint16_t)(sp + 2), T.r[0]); k.add(30);
	auto pos = [sp]() { return rw((uint16_t)(sp + 2)); };
	k.at(0x9C6A); T.r[0] = peek(0x0218); k.add(24);
	k.call(0x8078, 0x9C72);                            // select_video_bank
	k.at(0x9C72); T.r[2] = peek(0x06B0); k.add(24);
	k.at(0x9C76); T.r[0] = (uint16_t)-peek(0x0212); k.add(24, 12);
	T.r[1] = (uint16_t)(T.r[0] << 1); k.add(12, 12);
	k.at(0x9C80);
	const bool far = peekb(0x0F59) != 0;
	k.add(24, 12);
	T.r[4] = far ? 0x001E : 0x000A;
	k.add(18);
	if (!far) k.add(12);
	T.r[5] = rw((uint16_t)(sp + 0x12)); k.add(27);
	// tile word R3 -> tile number (bank base 0x01D8 for bit 10, else 0x01D6), code at pc
	auto tile = [&k](uint16_t pc) {
		k.add(18, 12);
		const bool hi = (T.r[3] & 0x0400) != 0;
		T.r[3] &= 0x03FF; k.add(18);
		k.at((uint16_t)(pc + (hi ? 0x0A : 0x14)));
		T.r[3] = (uint16_t)(T.r[3] + peek(hi ? 0x01D8 : 0x01D6));
		k.add(24, (hi ? 12 : 0));
	};
	for (;;)
	{
		k.add(12, 12);
		if ((int16_t)T.r[2] >= (int16_t)T.r[5])
		{
			T.r[3] = 0x8000;
			k.add(18, 12, 12);
			break;
		}
		k.at(0x9C9E); ww((uint16_t)(sp + 2), (uint16_t)(pos() + peek(0x0212))); k.add(42);
		T.r[2] = (uint16_t)(T.r[2] + T.r[1]); k.add(12, 33, 12);
		if (pos() & 0x2000)
		{
			ww((uint16_t)(sp + 2), pos() & 0x1F00); k.add(36);
			k.at(0x9CB4); poke(0x0218, (uint16_t)(peek(0x0218) + 0x2000)); k.add(33);
			k.at(0x9CBA); T.r[0] = peek(0x0218); k.add(24);
			k.call(0x8078, 0x9CC2);                    // select_video_bank
		}
		T.r[0] = pos() & 0x1F00; k.add(27, 18);
		push((uint16_t)(rw(sp) >> 8)); k.add(24, 36);  // CLR -(SP); MOVB 3(SP),(SP)
		T.r[0] = (uint16_t)(T.r[0] + pop()); k.add(18);
		T.r[3] = T.r[0] & ~0xE001; k.add(12, 18);
		k.at(0x9CD8); T.r[3] = peek((uint16_t)(0x2000 + T.r[3])); k.add(27, 18, 12);
		if (T.r[5] == 0x7FFE)
		{
			// only cells with bit 14
			k.add(18, 12);
			if (T.r[3] & 0x4000)
			{
				tile(0x9CE8);
				k.at(0x9D00); T.r[3] = sext8(peekb((uint16_t)(0x4000 + T.r[3]))); k.add(27, 18, 12);
				bool flat = false;
				if (!(T.r[3] & 7))
				{
					T.r[0] = (uint16_t)((int16_t)T.r[3] >> 3); k.add(12, 12, 12, 12, 18, 12);
					flat = T.r[0] != 0x000F;
				}
				if (flat)
				{
					T.r[0] = (uint16_t)(((int16_t)T.r[3] >> 3) << 1); k.add(12, 12, 12, 12, 12);
					k.at(0x9D22); T.r[2] = peek((uint16_t)(0xFCB0 + T.r[0])); k.add(27, 12);
					break;
				}
				T.r[3] &= 0x003F; k.add(18);
				k.at(0x9D2C); T.r[0] = peek(0x06B0) & 0x01FF; k.add(24, 18);
				T.r[0] = (uint16_t)(T.r[0] + T.r[2]); k.add(12);
				T.r[2] = T.r[0] & ~1; k.add(12, 12, 18, 12);
				break;
			}
		}
		else
		{
			tile(0x9D3A);
			k.add(18, 12);
			if (T.r[5] == 0x7FFF)
			{
				// only negative attribute bytes
				k.at(0x9D58); T.r[0] = sext8(peekb((uint16_t)(0x6000 + T.r[3]))); k.add(27, 12);
				if (T.r[0] & 0x8000)
				{
					T.r[3] = T.r[0] & 0x007F; k.add(12, 18, 12);
					break;
				}
			}
			else
			{
				k.at(0x9D66);
				const bool deep = (int8_t)peekb(0x032F) > 1;
				k.add(30, 12);
				bool attr = false;
				if (deep)
				{
					k.at(0x9D6E); attr = (peekb((uint16_t)(0x6000 + T.r[3])) & 0x80) != 0; k.add(27, 12);
				}
				if (attr)
				{
					k.at(0x9D7A); T.r[3] = sext8(peekb((uint16_t)(0x6000 + T.r[3]))) & 0x007F; k.add(27, 18);
				}
				else
				{
					k.at(0x9D74); T.r[3] = sext8(peekb((uint16_t)(0x4000 + T.r[3]))); k.add(27, 12);
				}
				k.add(18, 12);
				if (T.r[3] & 7)
				{
					T.r[3] &= 0x003F; k.add(18);
				}
				T.r[0] = (uint16_t)(7 & ~T.r[3]); k.add(18, 12, 12);
				bool take = T.r[0] != 0;
				if (!take)
				{
					k.add(18, 12);
					take = T.r[3] == 0x003F;
				}
				if (take)
				{
					k.add(18, 12);
					bool flat = false;
					if (!(T.r[3] & 7))
					{
						T.r[0] = (uint16_t)((int16_t)T.r[3] >> 3); k.add(12, 12, 12, 12, 18, 12);
						flat = T.r[0] != 0x000F;
					}
					if (flat)
					{
						T.r[0] = (uint16_t)(((int16_t)T.r[3] >> 3) << 1); k.add(12, 12, 12, 12, 12);
						k.at(0x9DB8); T.r[2] = peek((uint16_t)(0xFCB0 + T.r[0])); k.add(27, 12);
					}
					else
					{
						k.at(0x9DBE); T.r[0] = peek(0x06B0) & 0x01FF; k.add(24, 18);
						T.r[2] = (uint16_t)(T.r[2] + T.r[0]); k.add(12);
					}
					T.r[2] &= ~1; k.add(18, 12);
					break;
				}
			}
		}
		k.add(12, 12);
		if (--T.r[4] == 0)
		{
			T.r[2] = 0x7FFF; k.add(18);
			break;
		}
		k.add(18);                                     // JMP @#0x9C94
	}
	k.at(0x9DDA); poke(0x0222, T.r[2]); k.add(27);
	T.r[0] = T.r[3]; k.add(12);
	const uint16_t col = pop(), p = pop();
	fl::cmp(col, p);                                   // CMP (SP)+,(SP)+
	k.add(24);
	k.restore(5);
	f_nzv(nzw(T.r[1]));
	k.leave();
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
	Clock k;
	k.at(0x9A32); T.r[1] = peek(0x0304); k.add(24);
	k.at(0x9A36);
	const bool ramp = peekb(0x01EA) != 0;
	k.add(24, 12);
	if (ramp)
	{
		k.at(0x9A3C); T.r[0] = peek(0x06B2); k.add(24);
		k.at(0x9A40); T.r[0] = (uint16_t)(T.r[0] - peek(0x06B0)); k.add(24, 18, 12);
		if ((int16_t)T.r[0] <= 0)
		{
			k.at(0x9A48); T.r[2] = sext8(peekb(0x0327)); k.add(24, 12, 12);
			if (T.r[2] != T.r[1])
			{
				T.r[2] = (uint16_t)(7 & ~T.r[1]);
				k.add(18, 12, 12);
				if (T.r[2])
				{
					T.r[2] = 0x1800; k.add(18);
					k.at(0x9A5C); poke(0x01D2, T.r[2]); k.add(27);
					k.at(0x9A60); poke(0x1400, T.r[2]); k.add(27);
					push(T.r[1]); k.add(24);
					k.call(0x51EE, 0x9A6A);            // A5:51EE
					T.r[2] = 0x0800; k.add(18);
					k.at(0x9A6E); poke(0x01D2, T.r[2]); k.add(27);
					k.at(0x9A72); poke(0x1400, T.r[2]); k.add(27);
					T.r[3] = T.r[0]; k.add(12);
					k.at(0x9A78); T.r[2] = peek(0x01B4); k.add(24, 12);
					if ((int16_t)T.r[2] < 0)
					{
						T.r[0] = (uint16_t)-T.r[0];
						k.add(12);
					}
					k.add(12, 12);
					if ((int16_t)T.r[0] > 0)
					{
						T.r[3] = sext8((uint8_t)(T.r[3] >> 8)); k.add(12, 12);
						poke(T.r[6], T.r[3]); k.add(21);
						push(T.r[2]); k.add(24);
						poke(T.r[6], (uint16_t)((int16_t)rw(T.r[6]) >> 2)); k.add(21, 21);
						k.call(0x80D8, 0x9A94);        // mul16
						T.r[0] = (uint16_t)(T.r[0] - 0x1200);
						T.r[6] = (uint16_t)(T.r[6] + 2);
						k.add(18, 18);
					}
					T.r[6] = (uint16_t)(T.r[6] + 2);
					k.add(18);
				}
			}
		}
		k.add(12, 12);
		if ((int16_t)T.r[0] > 0)
		{
			k.call(0x9F8A, 0x9AA4);                    // skater_jump
			k.add(12);
		}
	}
	else
	{
		k.at(0x9AA6);
		const bool falling = (int16_t)peek(0x05B2) < (int16_t)peek(0x05B0);
		k.add(36, 12);
		if (falling)
		{
			k.at(0x9AAE); T.r[0] = peek(0x06B2); k.add(24);
			k.at(0x9AB2); T.r[0] = (uint16_t)(T.r[0] + peek(0x05B2)); k.add(24);
			k.at(0x9AB6); T.r[2] = peek(0x06B0); k.add(24);
			k.at(0x9ABA); T.r[2] = (uint16_t)(T.r[2] + peek(0x05B0)); k.add(24);
			T.r[0] = (uint16_t)(T.r[0] - T.r[2]); k.add(12, 18, 12);
			if ((int16_t)T.r[0] < 0x0040)
			{
				push(0x0047); k.add(30);
				k.call(0x8E78, 0x9ACE);                // queue_sound
				poke(T.r[6], 0x004B); k.add(27);
				k.call(0x8E78, 0x9AD6);
				poke(T.r[6], T.r[1]); k.add(21);
				k.call(0xA090, 0x9ADC);                // skater_touch_down
				sel_ab(k, 0x9ADC, 0x3800, 0x3C00);
				k.at(0x9AF4);
				const bool park3 = peekb(0x0F59) == 3;
				k.add(30, 12);
				if (park3)
					k.call(0x7E98, 0x9B06);
				else
				{
					k.call(0x7ADA, 0x9B00);
					k.add(12);
				}
				T.r[6] = (uint16_t)(T.r[6] + 2);
				k.add(18);
			}
		}
	}
	k.at(0x9B08); T.r[0] = sext8(peekb(0x0327)); k.add(24);
	fl::cmp(T.r[0], T.r[1]);
	k.add(12, 12);
	if (T.r[0] == T.r[1])
	{
		k.leave();
		return;
	}
	T.r[2] = (uint16_t)(7 & ~T.r[1]);
	f_nzv(nzw(T.r[2]));
	k.add(18, 12, 12);
	if (!T.r[2])
	{
		k.leave();
		return;
	}
	k.at(0x9B18); T.r[2] = sext8(peekb(0x032D)); k.add(24, 12, 12);
	if (T.r[1] == T.r[2])
	{
		k.at(0x9B20); T.r[2] = sext8(peekb(0x032C)); k.add(24);
		T.r[2] = (uint16_t)(T.r[2] - 1); k.add(12);
		k.at(0x9B26); pokeb(0x032C, (uint8_t)T.r[2]); k.add(27, 12);
	}
	else
	{
		k.at(0x9B2C); pokeb(0x032C, 0x0A); k.add(33);
	}
	k.at(0x9B32); pokeb(0x032D, (uint8_t)T.r[0]); k.add(27);
	T.r[2] = (uint16_t)(7 & ~T.r[0]); k.add(18, 12, 12);
	if (!T.r[2])
	{
		k.add(18, 12);
		if (!(T.r[1] & 7))
		{
			k.at(0x9B44); T.r[0] = sext8(peekb(0x031D)); k.add(24);
			k.at(0x9B48); pokeb(0x031D, peekb((uint16_t)(0xFCE0 + T.r[0]))); k.add(42);
		}
	}
	k.at(0x9B4E); pokeb(0x0327, (uint8_t)T.r[1]); k.add(27);
	k.call(0x9B82, 0x9B56);                            // skater_motion_params
	k.at(0x9B56);
	const uint8_t ramp2 = peekb(0x01EA);
	k.add(24, 12);
	if (ramp2)
	{
		fl::tstb(ramp2);
		k.leave();
		return;
	}
	k.at(0x9B5C); push(peek(0x01B4)); k.add(36);
	k.at(0x9B60); push(peek(0x02EC)); k.add(36);
	k.call(0x80FA, 0x9B68);                            // mul32
	k.at(0x9B68); pokeb(0x07F8, peekb(0x0205)); k.add(39);
	k.at(0x9B6E); poke(0x05B0, peek(0x0206)); k.add(39);
	k.at(0x9B74);
	const bool carried = peekb(0x0EA0) != 0;
	k.add(24, 12);
	if (carried) k.call(0xD6BE, 0x9B7E);
	fl::cmp(rw(T.r[6]), rw((uint16_t)(T.r[6] + 2)));
	T.r[6] = (uint16_t)(T.r[6] + 4);
	k.add(24);
	k.leave();
}


// F:8134 (assembly): a short busy delay of 20 loop passes, R0 = 0
void short_delay()
{
	Clock k;
	T.r[0] = 0x0014; fl::mov(T.r[0]); k.step(18, 0x8138);
	do
	{
		T.r[0]--; k.step(18, T.r[0] ? 0x8138 : 0x813A);  // SOB R0
	} while (T.r[0]);
	k.leave();
}

// F:8000 (reset): stack at 0x017E, vector 4 = vector 8; with the test switch on (IN0 bit 15
// clear) the self test (F:F17E, banks 0x3800 / 0x3C00), otherwise the power-on sequence
// (F:8156): interrupts off, RAM 0x0080-0x0FFF cleared, video RAM cleared (F:8D2C, F:8CE8),
// 0x0190 = 1, banks 0x3800 / 0x3C00, A7:4B00 and the attract mode F:829E, which does not
// return.  The code at 0x8000-0x8035 is fetched through the slapstic: its fetches are replayed.
void reset_entry()
{
	slap(0x8000); slap(0x8002);
	T.r[6] = 0x017E;
	fl::mov(T.r[6]);
	tick(0x8004, 18);
	slap(0x8004); slap(0x8006);
	uint16_t v = peek(0x0008);
	fl::mov(v);
	slap(0x8008);
	poke(0x0004, v);
	tick(0x800A, 39);
	slap(0x800A); slap(0x800C);
	v = peek(0x000A);
	fl::mov(v);
	slap(0x800E);
	poke(0x0006, v);
	tick(0x8010, 39);
	slap(0x8010); slap(0x8012);
	const uint16_t in0 = peek(0x1800);
	fl::tst(in0);
	tick(0x8014, 24);
	slap(0x8014);
	if (!(in0 & 0x8000))
	{
		tick(0x801A, 12);
		slap(0x801A); slap(0x801C);
		fl::mov(0x3800);
		slap(0x801E);
		poke(0x01D2, 0x3800);
		tick(0x8020, 33);
		slap(0x8020); slap(0x8022);
		v = peek(0x01D2);
		fl::mov(v);
		slap(0x8024);
		poke(0x1400, v);
		tick(0x8026, 39);
		slap(0x8026); slap(0x8028);
		fl::mov(0x3C00);
		slap(0x802A);
		poke(0x01D4, 0x3C00);
		tick(0x802C, 33);
		slap(0x802C); slap(0x802E);
		v = peek(0x01D4);
		fl::mov(v);
		slap(0x8030);
		poke(0x1402, v);
		tick(0x8032, 39);
		slap(0x8032); slap(0x8034);
		tick(0xF17E, 18);
		tail_jump(0xF17E);                             // the self test
		return;
	}
	tick(0x8016, 12);
	slap(0x8016); slap(0x8018);
	tick(0x8156, 18);
	Clock k;
	poke(0x1600, 0); fl::clr(); k.add(27);
	k.at(0x815A); pokeb(0x1800, 0); k.add(27);
	k.at(0x815E); poke(0x1600, 0); k.add(27);
	T.r[0] = 0x0FFE; k.add(18);
	do
	{
		k.at(0x8166); poke(T.r[0], 0); k.add(21);
		T.r[0] = (uint16_t)(T.r[0] - 2);
		fl::cmp(T.r[0], 0x0080);
		k.add(18, 18, 12);
	} while ((int16_t)T.r[0] >= 0x0080);
	k.at(0x8172); pokeb(0x1800, 0); k.add(27);
	k.call(0x8D2C, 0x817A);                     // clear_playfield_ram
	k.call(0x8CE8, 0x817E);                     // clear_playfield
	k.at(0x817E); pokeb(0x1800, 0); k.add(27);
	k.at(0x8182); pokeb(0x0190, 1); k.add(33);
	sel_ab(k, 0x8188, 0x3800, 0x3C00);
	k.call(0x4B00, 0x81A4);
	k.call(0x829E, 0x81A8);                     // attract mode: does not return
	k.leave();
}

// F:829E (assembly; waits, does not return): the attract mode: display setup (F:BA2E),
// interrupts of the play mode (F:820C) with the pending acknowledges cleared and interrupts
// allowed, game state 4, the palettes loaded by the vertical blank (0x0EB0 = F:DA74, F:DABA,
// F:DC3C, F:DB4E, one frame each), playfield cleared, text table 0x5AC8, no texts pending,
// then the attract main loop F:8714
void attract_mode()
{
	Clock k;
	k.call(0xBA2E, 0x82A2);
	k.call(0x820C, 0x82A6);                     // install_interrupts_b
	k.at(0x82A6); poke(0x15E0, 0); k.add(27);
	k.at(0x82AA); poke(0x15C0, 0); k.add(27);
	k.at(0x82AE); poke(0x1580, 0); k.add(27);
	T.r[0] = 0; k.add(12);
	mtps(0); k.step(30, 0x82B8);
	k.at(0x82B8); pokeb(0x018F, 4); k.add(33);
	k.at(0x82BE); pokeb(0x018E, 1); k.add(33);
	k.at(0x82C4); pokeb(0x0F25, 2); k.add(33);
	k.at(0x82CA); pokeb(0x0EA7, 0); k.add(27);
	k.at(0x82CE); pokeb(0x0EA8, 0xFF); k.add(33);
	k.at(0x82D4); poke(0x0EB0, 0xDA74); k.add(33);
	k.call(0x8D50, 0x82DE);                     // wait_vblank
	k.at(0x82DE); poke(0x0EB0, 0xDABA); k.add(33);
	k.call(0x8D50, 0x82E8);
	k.at(0x82E8); poke(0x0EB0, 0xDC3C); k.add(33);
	k.at(0x82EE); poke(0x0EAC, 0x000F); k.add(33);
	k.at(0x82F4); poke(0x0EAE, 0); k.add(27);
	k.call(0x8D50, 0x82FC);
	k.at(0x82FC); poke(0x0EB0, 0xDB4E); k.add(33);
	k.call(0x8D50, 0x8306);
	k.call(0x8CE8, 0x830A);                     // clear_playfield
	k.at(0x830A); poke(0x0184, 0x5AC8); k.add(33);
	k.at(0x8310); poke(0x018A, 1); k.add(33);
	k.at(0x8316); pokeb(0x0336, 0xFF); k.add(33);
	k.at(0x831C); poke(0x0EC2, 0xFFFF); k.add(33);
	k.call(0x8714, 0x8326);                     // attract_loop: does not return
	k.leave();
}

// one phase of the attract main loop (see F:8714)
void attract_phase(Clock &k, uint8_t phase)
{
	switch (phase)
	{
	case 0:
		sel_ab(k, 0x874E, 0x3800, 0x3C00);
		k.call(0x5BB8, 0x876A);
		sel_ab(k, 0x876A, 0x3800, 0x3C00);
		k.call(0x5248, 0x8786);
		k.add(12, 12);
		break;
	case 1:
	{
		k.at(0x8788);
		bool show = peekb(0x0F26) == 6;
		k.add(30, 12);
		if (!show)
		{
			k.at(0x8790);
			show = !(peek(0x01C2) & 0x000F);
			k.add(30, 12);
		}
		if (show)
		{
			sel_ab(k, 0x8798, 0x3800, 0x3C00);
			k.at(0x87B0); T.r[0] = sext8(peekb(0x018F)); k.add(24, 12);
			bool title = T.r[0] == 0;
			if (!title)
			{
				k.add(18, 12);
				title = T.r[0] == 1;
			}
			if (title)
			{
				k.call(0x77B4, 0x87C0);
			}
			k.at(0x87C0);
			const bool state0 = peekb(0x018F) == 0;
			k.add(24, 12);
			if (state0)
			{
				k.call(0x78BE, 0x87CA);
			}
		}
		sel_ab(k, 0x87CA, 0x3800, 0x3C00);
		k.call(0x79D4, 0x87E6);
		k.add(12, 12);
		break;
	}
	case 2:
	{
		k.at(0x87E8);
		const bool park1 = peekb(0x0F59) == 1;
		k.add(30, 12);
		if (park1)
		{
			k.call(0xBBEC, 0x87F4);
			k.add(12);
		}
		else
		{
			k.at(0x87F6);
			const bool slots = peekb(0x0E99) != 0;
			k.add(24, 12);
			if (slots)
			{
				k.at(0x87FC);
				const bool park0 = peekb(0x0F59) == 0;
				k.add(24, 12);
				if (park0)
				{
					sel_ab(k, 0x8802, 0x1800, 0x1C00);
					k.call(0x48CC, 0x881E);
				}
			}
		}
		sel_a(k, 0x881E, 0x3800);
		k.call(0x4C28, 0x882E);
		k.at(0x882E);
		const bool busy = peekb(0x018E) != 0;
		k.add(24, 12);
		if (busy) break;
		k.at(0x8834);
		const bool busy2 = peekb(0x0F36) != 0;
		k.add(24, 12);
		if (busy2) break;
		sel_b(k, 0x883A, 0x2800);
		k.call(0x7F16, 0x884A);
		k.call(0x7CF6, 0x884E);
		k.call(0x7C84, 0x8852);
		k.add(12);
		break;
	}
	case 3:
	{
		k.at(0x8854);
		const bool on = peekb(0x0197) != 0;
		k.add(24, 12);
		if (!on) break;
		k.at(0x885A);
		const bool odd = (peek(0x01C2) & 1) != 0;
		k.add(30, 12);
		if (!odd)
		{
			k.call(0xD35E, 0x8866);
		}
		else
		{
			sel_ab(k, 0x8868, 0x1800, 0x1C00);
			k.call(0x5B46, 0x8884);
		}
		k.add(12);
		break;
	}
	case 4:
		sel_a(k, 0x8886, 0x3800);
		k.call(0x4B84, 0x8896);
		k.add(12);
		break;
	case 5:
	{
		k.at(0x8898); T.r[0] = peek(0x01C2); k.add(24);
		T.r[0] = (uint16_t)(T.r[0] + 2); k.add(18, 18, 12);
		if (T.r[0] & 3) break;
		k.at(0x88A6);
		const bool on = peekb(0x0197) != 0;
		k.add(24, 12);
		if (!on) break;
		k.call(0xC8AA, 0x88B0);
		k.add(12);
		break;
	}
	case 6:
	{
		k.at(0x88B2);
		const bool now = !(peek(0x01C2) & 0x003F);
		k.add(30, 12);
		if (!now) break;
		k.at(0x88BA);
		const bool sw = (peek(0x1800) & 0x8000) != 0;
		k.add(30, 12);
		if (!sw)
		{
			sel_ab(k, 0x88C2, 0x3800, 0x3C00);
			k.call(0xF372, 0x88DE);
		}
		sel_ab(k, 0x88DE, 0x3800, 0x3C00);
		k.call(0x4B3E, 0x88FA);
		k.at(0x88FA);
		const bool state4 = peekb(0x018F) == 4;
		k.add(30, 12);
		if (state4)
		{
			k.call(0x435A, 0x8906);
		}
		k.at(0x8906);
		const bool dead = peekb(0x01A2) != 0;
		k.add(24, 12);
		if (dead)
		{
			k.call(0x4B00, 0x8910);
		}
		break;
	}
	}
}

// F:8714 (assembly; waits, does not return): the main loop of the attract mode.  A new frame
// (0x0192 set by the vertical blank) restarts the phase count 0x02BA and counts 0x01C2; the
// map is drawn in strips (A5:41CE) whenever there is time; the phases 0-6 of a frame do:
// 0 B3:5BB8, B3:5248;  1 the attract display (B3:77B4 / 78BE by the state, every 16th frame
// or in game phase 6) and B3:79D4;  2 park 1 / map slots (F:BBEC, A5:48CC), A7:4C28 and,
// unless the screen is busy (0x018E / 0x0F36), B6:7F16, 7CF6, 7C84;  3 F:D35E or A5:5B46 on
// alternate frames;  4 A7:4B84;  5 every 4th frame F:C8AA;  6 every 64th frame the coin /
// start handling (B3:F372 with IN0 bit 15 clear, B3:4B3E, B3:435A in state 4, A7:4B00 with
// the sound CPU marked dead).  Phases 3 and 5 only run with 0x0197 set.
void attract_loop()
{
	Clock k;
	enum { L8714, L871A, L8726, L8736 } at = L8714;
	for (;;)
		switch (at)
		{
		case L8714:
		{
			k.at(0x8714);
			const bool frame = peekb(0x0192) != 0;
			k.add(24, 12);
			at = frame ? L871A : L8726;
			break;
		}
		case L871A:
			k.at(0x871A); pokeb(0x0192, 0); k.add(27);
			k.at(0x871E); pokeb(0x02BA, 0); k.add(27);
			k.at(0x8722); poke(0x01C2, (uint16_t)(peek(0x01C2) + 1)); k.add(27);
			at = L8726;
			break;
		case L8726:
			sel_a(k, 0x8726, 0x1800);
			k.call(0x41CE, 0x8736);             // A5:41CE draw_map_strip
			at = L8736;
			break;
		case L8736:
		{
			k.at(0x8736);
			const bool frame = peekb(0x0192) != 0;
			k.add(24, 12);
			if (frame)
			{
				at = L871A;
				break;
			}
			k.at(0x873C);
			const uint8_t phase = peekb(0x02BA);
			k.add(30, 12);
			if ((int8_t)phase > 6)
			{
				at = L8714;
				break;
			}
			k.at(0x8744); T.r[0] = sext8(peekb(0x02BA)); k.add(24);
			T.r[0] = (uint16_t)(T.r[0] << 1); k.add(12, 27);
			attract_phase(k, (uint8_t)(T.r[0] >> 1));
			k.at(0x8910); T.r[0] = (uint16_t)(sext8(peekb(0x02BA)) + 1); k.add(24, 12);
			k.at(0x8916); pokeb(0x02BA, (uint8_t)T.r[0]); k.add(27, 18);
			at = L8736;
			break;
		}
		}
}

// F:8F50 / F:8F62 (assembly; waits for the sound CPU): its answer (F:8ED2) must be 0xAA /
// 0x55, otherwise the sound CPU is marked dead (0x01A2 = 3 / 4)
void expect_answer(uint16_t here, uint8_t want, uint8_t dead)
{
	Clock k;
	k.call(0x8ED2, (uint16_t)(here + 4));       // read_sound_response
	fl::cmp(T.r[0], want);
	k.add(18, 12);
	if (T.r[0] != want)
	{
		k.at((uint16_t)(here + 0x0A)); pokeb(0x01A2, dead); k.add(33);
		fl::movb(dead);
	}
	k.leave();
}
void expect_answer_aa() { expect_answer(0x8F50, 0xAA, 3); }
void expect_answer_55() { expect_answer(0x8F62, 0x55, 4); }

// F:8F0C  read_sound_bytes(char *buf, int n) (waits for the sound CPU): n bytes from the sound
// CPU (F:8ED2) as they come (IN0 bit 4), then its closing answer 0x55 (F:8F62); the
// watchdog is fed meanwhile.  Without a byte for 3 x 65536 polls the sound CPU is marked
// dead (0x01A2 = 2); nothing happens while it is.  n on the stack counts down.
uint16_t read_sound_bytes(const Args &)
{
	Clock k;
	k.save(0x8F0C, 3);
	T.r[1] = rw((uint16_t)(T.r[6] + 0x0A)); k.add(27);
	T.r[2] = 0; k.add(12);
	T.r[3] = 3; k.add(18);
	k.at(0x8F1A);
	const bool dead = peekb(0x01A2) != 0;
	k.add(24, 12);
	if (!dead)
		for (;;)
		{
			k.at(0x8F20);
			const bool ready = (peek(0x1800) & 0x0010) != 0;
			k.add(30, 12);
			if (ready)
			{
				k.call(0x8ED2, 0x8F2C);         // read_sound_response
				pokeb(T.r[1], (uint8_t)T.r[0]);
				T.r[1]++;
				k.add(21);
				const uint16_t left = (uint16_t)(rw((uint16_t)(T.r[6] + 0x0C)) - 1);
				ww((uint16_t)(T.r[6] + 0x0C), left);
				k.add(30, 12);
				if (!left)
				{
					k.call(0x8F62, 0x8F38);     // expect_answer_55
					break;
				}
				T.r[3] = 3;
				T.r[2] = 0;
				k.add(18, 12);
			}
			k.call(0x917C, 0x8F44);             // feed_watchdog
			k.add(18);
			if (--T.r[2]) continue;
			k.add(18);
			if (--T.r[3]) continue;
			k.at(0x8F48); pokeb(0x01A2, 2); k.add(33);
			break;
		}
	k.restore(3);
	k.leave();
	return T.r[0];
}

// F:8D86 (assembly; waits for the sound CPU): the controller angle.  With interrupts masked
// the sound CPU is asked (command 0x17, F:8EA2) for 4 bytes into 0x0224 (F:8F0C); the
// negated reading is the step (0x01C8 = |step|, 0x01CA = step).  With auto-centring
// (0x0226) a small step moves the angle 0x01C6 back towards the centre 0x24 (by the offset
// 0x0227; the first time it is set to 0x24); the angle 0x0224 = 0x01C6 + step, kept within
// 0..0x8F, becomes the new 0x01C6.
void read_controller()
{
	Clock k;
	T.r[1] = 0x00E0; k.add(18);
	mtps(0xE0); k.add(24);
	push(0x0017); k.add(30);
	k.call(0x8EA2, 0x8D94);                     // send_sound_command
	poke(T.r[6], 4); k.add(27);
	push(0x0224); k.add(30);
	k.call(0x8F0C, 0x8DA0);                     // read_sound_bytes
	mtps(0); k.step(30, 0x8DA4);
	k.at(0x8DA4); poke(0x0224, (uint16_t)-peek(0x0224)); k.add(27);
	k.at(0x8DA8); T.r[1] = peek(0x0224); k.add(24, 12);
	T.r[0] = (int16_t)T.r[1] < 0 ? (uint16_t)-T.r[1] : T.r[1];
	k.add(12, 12);
	k.at(0x8DB6); poke(0x01C8, T.r[0]); k.add(27);
	k.at(0x8DBA); poke(0x01CA, T.r[1]); k.add(27);
	k.at(0x8DBE);
	const bool centring = peekb(0x0226) != 0;
	k.add(24, 12);
	if (centring)
	{
		k.add(18, 12);
		if ((int16_t)T.r[0] <= 6)
		{
			k.at(0x8DCA);
			const bool started = peekb(0x02BE) != 0;
			k.add(24, 12);
			if (!started)
			{
				k.at(0x8DD0); pokeb(0x02BE, (uint8_t)(peekb(0x02BE) + 1)); k.add(27);
				k.at(0x8DD4); poke(0x01C6, 0x0024); k.add(33, 12);
			}
			else
			{
				k.at(0x8DDC); T.r[0] = sext8(peekb(0x0227)); k.add(24);
				k.at(0x8DE0); T.r[0] = (uint16_t)(T.r[0] + peek(0x01C6)); k.add(24, 18, 12);
				if ((int16_t)T.r[0] > 0x0024)
				{
					k.at(0x8DEA); poke(0x01C6, (uint16_t)(peek(0x01C6) - 1)); k.add(27, 12);
				}
				else
				{
					k.at(0x8DF0); T.r[0] = peek(0x01C6); k.add(24);
					k.at(0x8DF4); T.r[1] = sext8(peekb(0x0227)); k.add(24);
					T.r[0] = (uint16_t)(T.r[0] - T.r[1]); k.add(12, 18, 12);
					if ((int16_t)T.r[0] < 0x0024)
					{
						k.at(0x8E00); poke(0x01C6, (uint16_t)(peek(0x01C6) + 1)); k.add(27);
					}
				}
			}
		}
	}
	k.at(0x8E04); poke(0x0224, (uint16_t)(peek(0x0224) + peek(0x01C6))); k.add(39);
	k.at(0x8E0A); T.r[0] = peek(0x0224); k.add(24, 12);
	if ((int16_t)T.r[0] < 0)
	{
		k.at(0x8E10); poke(0x0224, (uint16_t)(peek(0x0224) + 0x0090)); k.add(33, 12);
	}
	else
	{
		k.add(18, 12);
		if ((int16_t)T.r[0] >= 0x0090)
		{
			k.at(0x8E1E); poke(0x0224, (uint16_t)(peek(0x0224) - 0x0090)); k.add(33);
		}
	}
	k.at(0x8E24); poke(0x01C6, peek(0x0224)); k.add(39);
	fl::cmp(rw(T.r[6]), rw((uint16_t)(T.r[6] + 2)));
	T.r[6] = (uint16_t)(T.r[6] + 4);
	k.add(24);
	k.leave();
}

// F:910C (assembly; waits for the sound CPU): with interrupts masked, command 0x18 (F:8EA2)
// and two status bytes (F:8ED2) stored at 0x02C0 + the 5-bit counter 0x02E1 (counted up
// first), then the closing answer (F:8F62)
void read_sound_status()
{
	Clock k;
	T.r[2] = 0x00E0; k.add(18);
	mtps(0xE0); k.add(24);
	push(0x0018); k.add(30);
	k.call(0x8EA2, 0x911A);                     // send_sound_command
	T.r[3] = 2; k.add(18);
	do
	{
		T.r[0] = 0; k.add(12);
		k.at(0x9120); T.r[0] = peekb(0x02E1); k.add(24);
		T.r[0]++;
		T.r[1] = T.r[0] & 0x001F;
		k.add(12, 12, 18);
		k.at(0x912C); pokeb(0x02E1, 0); k.add(27);
		k.at(0x9130); pokeb(0x02E1, (uint8_t)(peekb(0x02E1) | (uint8_t)T.r[1])); k.add(27);
		T.r[1] = 0; k.add(12);
		k.at(0x9136); T.r[1] = peekb(0x02E1); k.add(24);
		k.call(0x8ED2, 0x913E);                 // read_sound_response
		pokeb((uint16_t)(0x02C0 + T.r[1]), (uint8_t)T.r[0]);
		k.add(30, 18);
	} while (--T.r[3]);
	k.call(0x8F62, 0x9148);                     // expect_answer_55
	T.r[2] = 0; k.add(12);
	mtps(0); k.step(30, 0x914E);                // a pending interrupt is taken here
	fl::tst(rw(T.r[6]));
	T.r[6] = (uint16_t)(T.r[6] + 2);
	k.add(18);
	k.leave();
}

// F:8D74  wait_frames(int n) (waits): n vertical blanks (F:8D50)
uint16_t wait_frames(const Args &)
{
	Clock k;
	push(T.r[1]); k.add(24);
	T.r[1] = rw((uint16_t)(T.r[6] + 4)); k.add(27, 12);
	if ((int16_t)T.r[1] > 0)
		do
		{
			k.call(0x8D50, 0x8D80);             // wait_vblank
			k.add(18);
		} while (--T.r[1]);
	T.r[1] = pop(); k.add(18);
	k.leave();
	return T.r[0];
}

// fill the playfield (video window 0, 0x2000-0x37FF) with 24 pairs of rows of the tiles R3 / R2
void fill_rows(Clock &k, uint16_t row1, uint16_t row2)
{
	// MOV #0x18,R4; 1$: MOV #0x40,R0; 2$: MOV R3,(R1)+; SOB R0,2$; MOV #0x40,R0;
	// 3$: MOV R2,(R1)+; SOB R0,3$; SOB R4,1$ (with flags and next addresses: the long loop is
	// where the interrupts of a wipe come in)
	T.r[4] = 0x0018; fl::mov(T.r[4]); k.step(18, (uint16_t)(row1 - 4));
	do
	{
		T.r[0] = 0x0040; fl::mov(T.r[0]); k.step(18, row1);
		do
		{
			k.at(row1); poke(T.r[1], T.r[3]); T.r[1] = (uint16_t)(T.r[1] + 2); fl::mov(T.r[3]); k.step(21, (uint16_t)(row1 + 2));
			T.r[0]--; k.step(18, T.r[0] ? row1 : (uint16_t)(row1 + 4));
		} while (T.r[0]);
		T.r[0] = 0x0040; fl::mov(T.r[0]); k.step(18, row2);
		do
		{
			k.at(row2); poke(T.r[1], T.r[2]); T.r[1] = (uint16_t)(T.r[1] + 2); fl::mov(T.r[2]); k.step(21, (uint16_t)(row2 + 2));
			T.r[0]--; k.step(18, T.r[0] ? row2 : (uint16_t)(row2 + 4));
		} while (T.r[0]);
		T.r[4]--; k.step(18, T.r[4] ? (uint16_t)(row1 - 4) : (uint16_t)(row2 + 6));
	} while (T.r[4]);
}

// F:8F74 (waits): wipe the playfield in 8 steps, 3 frames apart: palette entry 0x10B6 from
// the table at 0xFC9C and the playfield filled with the tile pair 0xC102 / 0xC109, counting up
// (the last step blank); 0x02BF = 0.  R1-R5 preserved (F162).
uint16_t wipe_screen_out(const Args &)
{
	Clock k;
	k.save(0x8F74, 5);
	k.call(0x8068, 0x8F7C);                     // select_video_bank0
	T.r[3] = 0xC102;
	T.r[2] = 0xC109;
	T.r[5] = 7;
	k.add(18, 18, 18);
	for (;;)
	{
		T.r[1] = 0x2000; k.add(18);
		push(3); k.add(30);
		k.call(0x8D74, 0x8F94);                 // wait_frames(3)
		T.r[0] = (uint16_t)(T.r[5] << 1); k.add(12, 12);
		k.at(0x8F98); poke(0x10B6, peek((uint16_t)(0xFC9C + T.r[0]))); k.add(42);
		fill_rows(k, 0x8FA6, 0x8FAE);
		k.add(18, 12);
		if (T.r[5] == 1)
		{
			T.r[3] = 0;
			T.r[2] = 0;
			k.add(12, 12);
		}
		else
		{
			T.r[3]++;
			T.r[2]++;
			k.add(12, 12, 12);
		}
		T.r[6] = (uint16_t)(T.r[6] + 2);
		k.add(18, 12, 12);
		T.r[5]--;
		if ((int16_t)T.r[5] < 0) break;
	}
	k.at(0x8FCA); pokeb(0x02BF, 0); k.add(27);
	k.restore(5);
	k.leave();
	return T.r[0];
}

// F:8FD0 (waits): the texts are drawn (F:A8FE), then the playfield is filled in 8 steps, 3
// frames apart, with palette entry 0x10B6 from 0xFC9C and the tile pair 0xC108 / 0xC10F counting
// down (the last step 0xC087); 0x02BF counts up.  R1-R5 preserved (F162).
uint16_t wipe_screen_in(const Args &)
{
	Clock k;
	k.save(0x8FD0, 5);
	k.call(0xA8FE, 0x8FD8);                     // flush_text_list
	k.call(0x8068, 0x8FDC);                     // select_video_bank0
	T.r[3] = 0xC108;
	T.r[2] = 0xC10F;
	T.r[5] = 0;
	k.add(18, 18, 12);
	for (;;)
	{
		T.r[1] = 0x2000; k.add(18);
		push(3); k.add(30);
		k.call(0x8D74, 0x8FF2);                 // wait_frames(3)
		T.r[0] = (uint16_t)(T.r[5] << 1); k.add(12, 12);
		k.at(0x8FF6); poke(0x10B6, peek((uint16_t)(0xFC9C + T.r[0]))); k.add(42);
		fill_rows(k, 0x9004, 0x900C);
		k.add(18, 12);
		if (T.r[5] == 6)
		{
			T.r[3] = 0xC087;
			T.r[2] = T.r[3];
			k.add(18, 12);
		}
		else
		{
			T.r[2]--;
			T.r[3]--;
			k.add(12, 12, 12);
		}
		T.r[6] = (uint16_t)(T.r[6] + 2);
		T.r[5]++;
		k.add(18, 12, 18, 12);
		if ((int16_t)T.r[5] > 7) break;
	}
	k.at(0x902E); pokeb(0x02BF, (uint8_t)(peekb(0x02BF) + 1)); k.add(27);
	k.restore(5);
	k.leave();
	return T.r[0];
}

// F:8328 (assembly; waits)  start_view(a, b, c, dz, y, x): the map view (A5:4000(a, b, c)),
// scroll registers, objects 0-3 active as the skater (kinds 0x30 / 0x10 / 0 / 0, animation
// 0x50FA for object 3) placed at (x + scroll x, y - scroll y, dz) (F:843E) with their
// animations started (F:B4B6), the slots filled with the position (F:E1FA), 0x0F13 / 0x0F14
// cleared, the sound sequence started (A5:5B18), 0x01F8 / 0x032A cleared, 0x0ED0 = 2, then two
// frames (F:8D50) with the sprite palette loaded (0x0EB0 = F:DB4E); banks restored
void start_view()
{
	Clock k;
	k.at(0x8328); push(peek(0x01D2)); k.add(36);
	k.at(0x832C); push(peek(0x01D4)); k.add(36);
	sel_a(k, 0x8330, 0x1800);
	push(rw((uint16_t)(T.r[6] + 0x0A))); k.add(39);
	push(rw((uint16_t)(T.r[6] + 0x0A))); k.add(39);
	push(rw((uint16_t)(T.r[6] + 0x0A))); k.add(39);
	k.call(0x4000, 0x834C);                     // A5:4000 init_map_view
	sel_ab(k, 0x834C, 0x3800, 0x3C00);
	k.call(0x8036, 0x8368);                     // load_scroll_registers
	k.at(0x8368); pokeb(0x0370, 1); k.add(33);
	k.at(0x836E); pokeb(0x0470, 0x30); k.add(33);
	k.at(0x8374); pokeb(0x0371, 1); k.add(33);
	k.at(0x837A); pokeb(0x0471, 0x10); k.add(33);
	k.at(0x8380); pokeb(0x0372, 2); k.add(33);
	k.at(0x8386); pokeb(0x0472, 0); k.add(27);
	k.at(0x838A); pokeb(0x0373, 3); k.add(33);
	k.at(0x8390); pokeb(0x0473, 0); k.add(27);
	k.at(0x8394); poke(0x03B6, 0x50FA); k.add(33);
	poke(T.r[6], rw((uint16_t)(T.r[6] + 0x16))); k.add(36);
	k.at(0x839E); poke(T.r[6], (uint16_t)(rw(T.r[6]) + peek(0x01A8))); k.add(33);
	push(rw((uint16_t)(T.r[6] + 0x14))); k.add(39);
	k.at(0x83A6); poke(T.r[6], (uint16_t)(rw(T.r[6]) - peek(0x01AA))); k.add(33);
	push(rw((uint16_t)(T.r[6] + 0x14))); k.add(39);
	k.call(0x843E, 0x83B2);                     // place_skater
	T.r[1] = 0; k.add(12);
	do
	{
		T.r[0] = (uint16_t)(T.r[1] << 1); k.add(12, 12);
		k.at(0x83B8); poke((uint16_t)(0x03F0 + T.r[0]), 1); k.add(36);
		poke(T.r[6], T.r[1]); k.add(21);
		k.call(0xB4B6, 0x83C4);
		T.r[1]++;
		k.add(12, 18, 12);
	} while ((int16_t)T.r[1] <= 3);
	k.at(0x83CC); poke(T.r[6], peek(0x06F0)); k.add(33);
	k.at(0x83D0); push(peek(0x0730)); k.add(36);
	k.at(0x83D4); push(peek(0x06B0)); k.add(36);
	push(0x0078); k.add(30);
	k.call(0xE1FA, 0x83E0);                     // fill_slots
	k.at(0x83E0); pokeb(0x0F13, 0); k.add(27);
	k.at(0x83E4); pokeb(0x0F14, 0); k.add(27);
	sel_ab(k, 0x83E8, 0x1800, 0x1C00);
	k.call(0x5B18, 0x8404);                     // start_sequence_a5
	k.at(0x8404); pokeb(0x01F8, 0); k.add(27);
	k.at(0x8408); pokeb(0x032A, 0); k.add(27);
	k.at(0x840C); pokeb(0x0ED0, 2); k.add(33);
	k.call(0x8D50, 0x8416);                     // wait_vblank
	k.at(0x8416); poke(0x0EB0, 0xDB4E); k.add(33);
	k.call(0x8D50, 0x8420);
	k.at(0x8420); poke(0x01D2, rw((uint16_t)(T.r[6] + 0x12))); k.add(42);
	k.at(0x8426); poke(0x01D4, rw((uint16_t)(T.r[6] + 0x10))); k.add(42);
	k.at(0x842C); poke(0x1400, peek(0x01D2)); k.add(39);
	k.at(0x8432); poke(0x1402, peek(0x01D4)); k.add(39);
	T.r[6] = fl::add(T.r[6], 0x0014);
	k.add(18);
	k.leave();
}

// F:891E (interrupt; not reached by the tests): acknowledge the sound CPU's interrupt
void irq_sound_ack()
{
	Clock k;
	k.at(0x891E); poke(0x1580, 0); k.add(27);
	k.flush();                                         // the RTI (in the table)
}

// F:8A9E (assembly; not reached by the tests): string 0x19 and the one for the sound CPU's
// fault 0x01A2 (0x19 + code)
void show_sound_fault()
{
	Clock k;
	T.r[0] = 0x0019; k.add(18);
	k.call(0x8AB6, 0x8AA6);                     // draw_string_large
	T.r[0] = 0; k.add(12);
	k.at(0x8AA8); T.r[0] = peekb(0x01A2); k.add(24);
	T.r[0] = fl::add(T.r[0], 0x0019); k.add(18);
	k.call(0x8AB6, 0x8AB4);
	k.leave();
}

// F:9182 (assembly; not reached by the tests): sounds 0x53, 0x42, 0x70 and 0x76
void queue_four_sounds()
{
	Clock k;
	push(0x0053); k.add(30);
	k.call(0x8E78, 0x918A);                     // queue_sound
	poke(T.r[6], 0x0042); k.add(27);
	k.call(0x8E78, 0x9192);
	poke(T.r[6], 0x0070); k.add(27);
	k.call(0x8E78, 0x919A);
	poke(T.r[6], 0x0076); k.add(27);
	k.call(0x8E78, 0x91A2);
	fl::tst(rw(T.r[6]));
	T.r[6] = (uint16_t)(T.r[6] + 2);
	k.add(18);
	k.leave();
}

// F:A8D2  partner_object(int obj) (not reached by the tests): the slot (F:CDFA) of the object
// kind 0x0390[obj] with the value 0x0AF8[kind]; returns the object of that slot (0x09B8, 5 bits)
uint16_t partner_object(const Args &)
{
	Clock k;
	push(T.r[1]); k.add(24);
	T.r[1] = rw((uint16_t)(T.r[6] + 4)); k.add(27);
	T.r[0] = 0; k.add(12);
	k.at(0xA8DA); T.r[0] = peekb((uint16_t)(0x0390 + T.r[1])); k.add(27);
	T.r[0] = (uint16_t)(T.r[0] << 1); k.add(12);
	k.at(0xA8E0); T.r[0] = peek((uint16_t)(0x0AF8 + T.r[0])); k.add(27);
	push(T.r[0]); k.add(24);
	push(0); k.add(24);
	k.at(0xA8E8); poke(T.r[6], peekb((uint16_t)(0x0390 + T.r[1]))); k.add(36);
	k.call(0xCDFA, 0xA8F0);                     // find_other_slot
	k.at(0xA8F0); T.r[0] = sext8(peekb((uint16_t)(0x09B8 + T.r[0]))) & 0x001F; k.add(27, 18);
	T.r[6] = (uint16_t)(T.r[6] + 4);
	T.r[1] = pop();
	k.add(24, 18);
	k.leave();
	return T.r[0];
}

// F:9DE4 (not reached by the tests): object 0 drops to the level found by F:9C5E (mode 0x7FFD)
// from the probed cell ahead (cell 0 or 2 by the direction of travel); banks restored.
// R1-R4 preserved (F152).
uint16_t drop_to_level(const Args &)
{
	Clock k;
	k.save(0x9DE4, 4);
	k.at(0x9DE8); T.r[3] = peek(0x01D2); k.add(24);
	k.at(0x9DEC); T.r[4] = peek(0x01D4); k.add(24);
	sel_ab(k, 0x9DF0, 0x0800, 0x0C00);
	k.at(0x9E08); poke(0x0212, 0x0100); k.add(33);
	k.at(0x9E0E);
	T.r[2] = (int16_t)peek(0x01B4) < 0 ? 0 : 2;
	k.add(24, 12, (T.r[2] ? 18 + 12 : 12));
	T.r[2] = (uint16_t)(T.r[2] << 1); k.add(12);
	k.at(0x9E1E); poke(0x0218, peek((uint16_t)(0x0314 + T.r[2]))); k.add(42);
	push(0x7FFD); k.add(30);
	k.at(0x9E28); T.r[0] = peek((uint16_t)(0x0308 + T.r[2])); k.add(27);
	k.at(0x9E2C); T.r[1] = peek((uint16_t)(0x030E + T.r[2])); k.add(27);
	k.call(0x9C5E, 0x9E34);                     // trace_cells
	k.at(0x9E34); poke(0x06B0, peek(0x0222)); k.add(39);
	k.at(0x9E3A); poke(0x01D2, T.r[3]); k.add(27);
	k.at(0x9E3E); poke(0x01D4, T.r[4]); k.add(27);
	k.at(0x9E42); poke(0x1400, peek(0x01D2)); k.add(39);
	k.at(0x9E48); poke(0x1402, peek(0x01D4)); k.add(39);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	k.add(18);
	k.restore(4);
	k.leave();
	return T.r[0];
}

}

LOGIC_TABLE(flow,
	c_routine("F", 0x9034, "divide_ext", divide_ext, 0, 0),
	c_routine("F", 0x90A6, "divide", divide, 0, 0),
	c_routine("F", 0x90B0, "modulo", modulo, 0, 0),
	c_routine("F", 0x90BA, "park_level", park_level, 0, 0),
	asm_routine("F", 0x8AB6, "draw_string_large", draw_string_large, 0),
	asm_routine("F", 0x8B04, "draw_string_small", draw_string_small, 0),
	c_routine("F", 0x9F8A, "skater_jump", skater_jump, 0, 0),
	asm_routine("F", 0xAFF0, "process_sprite_requests", process_sprite_requests, 0),
	c_routine("F", 0xA974, "queue_message", queue_message, 0, 0),
	c_routine("F", 0xA8FE, "flush_text_list", flush_text_list, 0, 0),
	c_routine("F", 0x91A6, "skater_velocity", skater_velocity, 0, 0),
	c_routine("F", 0x843E, "place_skater", place_skater, 0, 0),
	c_routine("F", 0xA45A, "skater_phase10", skater_phase10, 0, 0),
	c_routine("F", 0xACE6, "unlink_sprites", unlink_sprites, 0, 0),
	c_routine("F", 0xAE02, "link_sprites", link_sprites, 0, 0),
	asm_routine("F", 0xAF32, "sort_sprite_list", sort_sprite_list, 0),
	asm_routine("F", 0x944C, "cell_terrain_codes", cell_terrain_codes, 0),
	asm_routine("F", 0x934C, "probe_ground", probe_ground, 0),
	c_routine("F", 0x9B82, "skater_motion_params", skater_motion_params, 0, 0),
	c_routine("F", 0x9FB4, "skater_take_off", skater_take_off, 0, 0),
	asm_routine("F", 0x9236, "update_push", update_push, 0),
	irq_routine("F", 0x899C, "irq_vblank_play", irq_vblank_play, 24),
	c_routine("F", 0xA2F0, "skater_land", skater_land, 0, 0),
	c_routine("F", 0xA090, "skater_touch_down", skater_touch_down, 0, 0),
	asm_routine("F", 0x94DC, "edge_mode", edge_mode, 0),
	asm_routine("F", 0x979A, "check_wall", check_wall, 0),
	c_routine("F", 0xA9FC, "run_text_script", run_text_script, 0, 0),
	asm_routine("F", 0xA510, "object_contact", object_contact, 0),
	c_routine("F", 0x9E96, "skater_bounce", skater_bounce, 0, 0),
	c_routine("F", 0x9EEE, "skater_deflect", skater_deflect, 0, 0),
	asm_routine("F", 0xAD70, "sprite_in_front", sprite_in_front, 0),
	asm_routine("F", 0x9C5E, "trace_cells", trace_cells, 0),
	asm_routine("F", 0x9A32, "check_terrain", check_terrain, 0),
	wait_routine("F", 0x8000, "reset_entry", reset_entry, 0),
	asm_routine("F", 0x8134, "short_delay", short_delay, 0),
	wait_routine("F", 0x829E, "attract_mode", attract_mode, 0),
	wait_routine("F", 0x8714, "attract_loop", attract_loop, 0),
	wait_routine("F", 0x8F50, "expect_answer_aa", expect_answer_aa, 0),
	wait_routine("F", 0x8F62, "expect_answer_55", expect_answer_55, 0),
	wait_c_routine("F", 0x8F0C, "read_sound_bytes", read_sound_bytes, 0),
	wait_routine("F", 0x8D86, "read_controller", read_controller, 0),
	wait_routine("F", 0x910C, "read_sound_status", read_sound_status, 0),
	wait_c_routine("F", 0x8D74, "wait_frames", wait_frames, 0),
	wait_c_routine("F", 0x8F74, "wipe_screen_out", wipe_screen_out, 0),
	wait_c_routine("F", 0x8FD0, "wipe_screen_in", wipe_screen_in, 0),
	wait_routine("F", 0x8328, "start_view", start_view, 0),
	irq_routine("F", 0x891E, "irq_sound_ack", irq_sound_ack, 24),
	asm_routine("F", 0x8A9E, "show_sound_fault", show_sound_fault, 0),
	asm_routine("F", 0x9182, "queue_four_sounds", queue_four_sounds, 0),
	c_routine("F", 0xA8D2, "partner_object", partner_object, 0, 0),
	c_routine("F", 0x9DE4, "drop_to_level", drop_to_level, 0, 0),
	wait_routine("F", 0x8512, "attract_frame", attract_frame, 0),
	irq_wait_routine("F", 0x84D4, "irq_vblank_attract", irq_vblank_attract, 24),
)

}
