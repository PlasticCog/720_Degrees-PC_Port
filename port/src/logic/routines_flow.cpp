// Rewritten routines: text strings, skater motion and state changes, sprite link lists,
// message queue and helpers of the frame flow.  Routines with an assembly interface
// reproduce registers and condition codes; the others follow the C convention.
#include "logic.h"

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


// The step helpers (as in the other routines files): an instruction's action, its condition
// codes, then its cycles and the next instruction's address (tick).
// MOV #v,@#a / MOVB #v,@#a (33 cycles), CLR @#a / CLRB @#a (27) and the step to next
void set_w(uint16_t a, uint16_t v, uint16_t next) { poke(a, v); fl::mov(v); tick(next, 33); }
void set_b(uint16_t a, uint8_t v, uint16_t next) { pokeb(a, v); fl::movb(v); tick(next, 33); }
void clr_w(uint16_t a, uint16_t next) { poke(a, 0); fl::clr(); tick(next, 27); }
void clr_b(uint16_t a, uint16_t next) { pokeb(a, 0); fl::clr(); tick(next, 27); }
// MOV #v,R0; MOV R0,@#shadow; MOV R0,@#register at pc: bank A (0x01D2 / BANK_A) or B (0x01D4 /
// BANK_B)
void select_bank(uint16_t pc, uint16_t shadow, uint16_t reg, uint16_t v)
{
	T.r[0] = v;
	fl::mov(v);
	tick((uint16_t)(pc + 4), 18);
	poke(shadow, v);
	fl::mov(v);
	tick((uint16_t)(pc + 8), 27);
	poke(reg, v);
	fl::mov(v);
	tick((uint16_t)(pc + 0x0C), 27);
}
void bank_a(uint16_t pc, uint16_t v) { select_bank(pc, 0x01D2, 0x1400, v); }
void bank_b(uint16_t pc, uint16_t v) { select_bank(pc, 0x01D4, 0x1402, v); }

// MOVB @#0x0329,@#0x0328 at pc: the code handled becomes the last one
void keep_code(uint16_t pc)
{
	const uint8_t v = peekb(0x0329);
	fl::movb(v);
	pokeb(0x0328, v);
	tick((uint16_t)(pc + 6), 39);
}

// TST (SP)+ at pc
void drop(uint16_t pc)
{
	fl::tst(peek(T.r[6]));
	T.r[6] = (uint16_t)(T.r[6] + 2);
	tick((uint16_t)(pc + 2), 18);
}
// CMP (SP)+,(SP)+ at pc
void drop2(uint16_t pc)
{
	const uint16_t s = pop();
	const uint16_t d = pop();
	fl::cmp(s, d);
	tick((uint16_t)(pc + 2), 24);
}
// the 24 rows of the wipe at pc (MOV #0x18,R4): 64 words of R3, then 64 of R2, from R1 on
void wipe_rows(uint16_t pc)
{
	T.r[4] = 0x0018;
	fl::mov(T.r[4]);
	tick((uint16_t)(pc + 4), 18);
	for (;;)
	{
		for (int half = 0; half < 2; half++)
		{
			const uint16_t loop = (uint16_t)(pc + (half ? 0x10 : 8));
			T.r[0] = 0x0040;
			fl::mov(T.r[0]);
			tick(loop, 18);
			const uint16_t w = half ? T.r[2] : T.r[3];
			for (;;)
			{
				poke(T.r[1], w);
				fl::mov(w);
				T.r[1] = (uint16_t)(T.r[1] + 2);
				tick((uint16_t)(loop + 2), 21);
				if (--T.r[0] == 0)
				{
					tick((uint16_t)(loop + 4), 18);    // SOB
					break;
				}
				tick(loop, 18);
			}
		}
		if (--T.r[4] == 0)
		{
			tick((uint16_t)(pc + 0x16), 18);           // SOB R4
			break;
		}
		tick((uint16_t)(pc + 4), 18);
	}
}

// MOV @#from,@#to (39 cycles) / MOV k(SP),@#to (42; 33 for (SP)) and the step to next
void copy_w(uint16_t from, uint16_t to, uint16_t next)
{
	const uint16_t v = peek(from);
	fl::mov(v);
	poke(to, v);
	tick(next, 39);
}
void copy_sp(int k, uint16_t to, uint16_t next)
{
	const uint16_t v = peek((uint16_t)(T.r[6] + k));
	fl::mov(v);
	poke(to, v);
	tick(next, k ? 42 : 33);
}
// MOV @#a,-(SP) (36 cycles) / MOV k(SP),-(SP) (39) and the step to next
void push_w(uint16_t a, uint16_t next)
{
	const uint16_t v = peek(a);
	fl::mov(v);
	push(v);
	tick(next, 36);
}
void push_sp(int k, uint16_t next)
{
	const uint16_t v = peek((uint16_t)(T.r[6] + k));
	fl::mov(v);
	push(v);
	tick(next, 39);
}
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

uint16_t divide_ext(const Args &)
{
	c_save(4, 0x9038);
	auto zero = []() {                                 // 0x90A2: CLR R0
		T.r[0] = 0;
		fl::clr();
		tick(0x90A4, 12);
		c_restore(4);
		return T.r[0];
	};
	T.r[1] = peek((uint16_t)(T.r[6] + 0x14));          // n
	fl::mov(T.r[1]);
	tick(0x903C, 27);
	if (!T.r[1])
	{
		tick(0x90A2, 12);                              // BEQ
		return zero();
	}
	tick(0x903E, 12);
	T.r[0] = peek((uint16_t)(T.r[6] + 0x12));          // d
	fl::mov(T.r[0]);
	tick(0x9042, 27);
	if (!T.r[0])
	{
		tick(0x90A2, 12);                              // BEQ
		return zero();
	}
	tick(0x9044, 12);
	T.r[5] = 0;                                        // R4 / R5: n / d were negative
	fl::clr();
	tick(0x9046, 12);
	T.r[4] = 0;
	fl::clr();
	tick(0x9048, 12);
	fl::tst(T.r[1]);
	tick(0x904A, 12);
	if ((int16_t)T.r[1] >= 0)
		tick(0x9050, 12);                              // BGE
	else
	{
		tick(0x904C, 12);
		T.r[1] = fl::neg(T.r[1]);
		tick(0x904E, 12);
		T.r[4] = fl::inc(T.r[4]);
		tick(0x9050, 12);
	}
	fl::tst(T.r[0]);
	tick(0x9052, 12);
	if ((int16_t)T.r[0] >= 0)
		tick(0x9058, 12);                              // BGE
	else
	{
		tick(0x9054, 12);
		T.r[0] = fl::neg(T.r[0]);
		tick(0x9056, 12);
		T.r[5] = fl::inc(T.r[5]);
		tick(0x9058, 12);
	}
	T.r[3] = 0;                                        // d shifted up to n (R3 times)
	fl::clr();
	tick(0x905A, 12);
	for (;;)
	{
		fl::cmp(T.r[0], T.r[1]);
		tick(0x905C, 12);
		if ((int16_t)T.r[0] >= (int16_t)T.r[1])
		{
			tick(0x906A, 12);                          // BGE
			break;
		}
		tick(0x905E, 12);
		const bool top = fl::logic((uint16_t)(T.r[0] & 0x4000)) != 0;
		tick(0x9062, 18);
		if (top)
		{
			tick(0x906A, 12);                          // BNE
			break;
		}
		tick(0x9064, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x9066, 12);
		T.r[3] = fl::inc(T.r[3]);
		tick(0x9068, 12);
		tick(0x905A, 12);                              // BR
	}
	T.r[2] = 0;                                        // shift and subtract: R2 the quotient
	fl::clr();
	tick(0x906C, 12);
	for (;;)
	{
		T.r[2] = fl::asl(T.r[2]);
		tick(0x906E, 12);
		fl::cmp(T.r[0], T.r[1]);
		tick(0x9070, 12);
		if ((int16_t)T.r[0] > (int16_t)T.r[1])
			tick(0x9078, 12);                          // BGT
		else
		{
			tick(0x9072, 12);
			T.r[2] = fl::inc(T.r[2]);
			tick(0x9074, 12);
			T.r[1] = fl::sub(T.r[1], T.r[0]);
			tick(0x9076, 12);
			if (!T.r[1])
			{
				tick(0x9080, 12);                      // BEQ: nothing left
				break;
			}
			tick(0x9078, 12);
		}
		T.r[3] = fl::dec(T.r[3]);
		tick(0x907A, 12);
		if ((int16_t)T.r[3] < 0)
		{
			tick(0x9080, 12);                          // BMI
			break;
		}
		tick(0x907C, 12);
		T.r[0] = fl::asr(T.r[0]);
		tick(0x907E, 12);
		tick(0x906C, 12);                              // BR
	}
	const uint16_t mode = peek((uint16_t)(T.r[6] + 0x0E));
	fl::tst(mode);
	tick(0x9084, 27);
	if (mode)
	{
		tick(0x9086, 12);                              // the remainder, with n's sign
		fl::tst(T.r[4]);
		tick(0x9088, 12);
		if (!T.r[4])
			tick(0x908C, 12);                          // BEQ
		else
		{
			tick(0x908A, 12);
			T.r[1] = fl::neg(T.r[1]);
			tick(0x908C, 12);
		}
		T.r[0] = T.r[1];
		fl::mov(T.r[0]);
		tick(0x908E, 12);
		c_restore(4);
		return T.r[0];
	}
	tick(0x9090, 12);                                  // BEQ: the quotient
	fl::cmp(T.r[4], T.r[5]);
	tick(0x9092, 12);
	if (T.r[4] == T.r[5])
		tick(0x9096, 12);                              // BEQ
	else
	{
		tick(0x9094, 12);
		T.r[2] = fl::neg(T.r[2]);
		tick(0x9096, 12);
	}
	for (;;)                                           // shifted by what is left of R3
	{
		T.r[3] = fl::dec(T.r[3]);
		tick(0x9098, 12);
		if ((int16_t)T.r[3] < 0)
		{
			tick(0x909E, 12);                          // BMI
			break;
		}
		tick(0x909A, 12);
		T.r[2] = fl::asl(T.r[2]);
		tick(0x909C, 12);
		tick(0x9096, 12);                              // BR
	}
	T.r[0] = T.r[2];
	fl::mov(T.r[0]);
	tick(0x90A0, 12);
	c_restore(4);
	return T.r[0];
}

uint16_t divide(const Args &)
{
	push(0);                                           // divide_ext(0, ...): the quotient
	fl::clr();
	tick(0x90A8, 24);
	jsr(0x90AC, 0x9034);
	drop(0x90AC);
	rts();
	return T.r[0];
}

uint16_t modulo(const Args &)
{
	push(0x90B2);                                      // MOV PC,-(SP): not 0, the remainder
	fl::mov(0x90B2);
	tick(0x90B2, 24);
	jsr(0x90B6, 0x9034);
	drop(0x90B6);
	rts();
	return T.r[0];
}

// F:90BA  park_level(): 0x0F2D when positive; otherwise derived from the table at 0xFCAC
// for the park 0x027A and limited to 9 (10 when 0x0F31 >= 12)
uint16_t park_level(const Args &)
{
	c_save(1, 0x90BE);
	T.r[1] = sext8(peekb(0x0F2D));
	fl::movb((uint8_t)T.r[1]);
	tick(0x90C2, 24);
	if ((int16_t)T.r[1] > 0)
	{
		tick(0x90C4, 12);                              // set: that one
		T.r[2] = T.r[1];
		fl::mov(T.r[2]);
		tick(0x90C6, 12);
		tick(0x9108, 12);                              // BR
	}
	else
	{
		tick(0x90C8, 12);                              // BLE
		T.r[0] = sext8(peekb(0x027A));
		fl::movb((uint8_t)T.r[0]);
		tick(0x90CC, 24);
		T.r[0] = sext8(peekb((uint16_t)(0xFCAC + T.r[0])));
		fl::movb((uint8_t)T.r[0]);
		tick(0x90D0, 27);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
		tick(0x90D4, 18);
		T.r[0] = fl::sub(T.r[0], T.r[1]);
		tick(0x90D6, 12);
		T.r[0] = fl::dec(T.r[0]);
		tick(0x90D8, 12);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0003));
		tick(0x90DC, 18);
		T.r[0] = fl::sub(T.r[0], T.r[1]);
		tick(0x90DE, 12);
		T.r[2] = T.r[0];
		fl::mov(T.r[2]);
		tick(0x90E0, 12);
		T.r[1] = 0;
		fl::clr();
		tick(0x90E2, 12);
		const uint8_t level = peekb(0x0F31);
		fl::cmpb(level, 0x0C);
		tick(0x90E8, 30);
		if (level >= 0x0C)
		{
			tick(0x90F2, 12);                          // BCC: at most 10
			T.r[0] = 0x000A;
			fl::mov(T.r[0]);
			tick(0x90F6, 18);
		}
		else
		{
			tick(0x90EA, 12);                          // at most 9
			T.r[1] = fl::inc(T.r[1]);
			tick(0x90EC, 12);
			T.r[0] = 0x0009;
			fl::mov(T.r[0]);
			tick(0x90F0, 18);
			tick(0x90F6, 12);                          // BR
		}
		fl::cmp(T.r[2], T.r[0]);
		tick(0x90F8, 12);
		if ((int16_t)T.r[2] <= (int16_t)T.r[0])
			tick(0x9108, 12);                          // BLE
		else
		{
			tick(0x90FA, 12);
			T.r[1] = fl::ror(T.r[1]);
			tick(0x90FC, 12);
			if (!C())
			{
				tick(0x9104, 12);                      // BCC
				T.r[2] = 0x000A;
				fl::mov(T.r[2]);
				tick(0x9108, 18);
			}
			else
			{
				tick(0x90FE, 12);
				T.r[2] = 0x0009;
				fl::mov(T.r[2]);
				tick(0x9102, 18);
				tick(0x9108, 12);                      // BR
			}
		}
	}
	T.r[0] = T.r[2];
	fl::mov(T.r[0]);
	tick(0x910A, 12);
	c_restore(1);
	return T.r[0];
}

// F:8AB6 / F:8B04 (string number in R0): draw string R0 of the table at 0x0184 (bank A
// 0x2000) with the large (F:8B52) or small (F:8C12) font.  An entry is a screen offset and
// the characters; the last one has bit 7 set.  Bit 7 of the string number draws it with
// code 0x23 instead (erasing).  R1-R3 are preserved (register-save helper F144).
void draw_string(uint16_t base, uint16_t put_char)
{
	c_save(2, (uint16_t)(base + 4));
	push_w(0x01D2, (uint16_t)(base + 8));              // bank A, put back at the end
	set_w(0x01D2, 0x2000, (uint16_t)(base + 0x0E));
	copy_w(0x01D2, 0x1400, (uint16_t)(base + 0x14));
	jsr((uint16_t)(base + 0x18), 0x8068);              // select_video_bank0
	T.r[3] = T.r[0];
	fl::mov(T.r[3]);
	tick((uint16_t)(base + 0x1A), 12);
	T.r[0] = fl::asl(T.r[0]);
	tick((uint16_t)(base + 0x1C), 12);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
	tick((uint16_t)(base + 0x20), 18);
	T.r[0] = fl::add(T.r[0], peek(0x0184));            // the string table
	tick((uint16_t)(base + 0x24), 24);
	T.r[1] = peek(T.r[0]);
	fl::mov(T.r[1]);
	tick((uint16_t)(base + 0x26), 18);
	T.r[2] = peek(T.r[1]);                             // its screen offset
	fl::mov(T.r[2]);
	T.r[1] = (uint16_t)(T.r[1] + 2);
	tick((uint16_t)(base + 0x28), 18);
	T.r[2] = fl::add(T.r[2], 0x2000);
	tick((uint16_t)(base + 0x2C), 18);
	poke(0x0186, T.r[2]);
	fl::mov(T.r[2]);
	tick((uint16_t)(base + 0x30), 27);
	for (;;)                                           // the characters (the last has bit 7)
	{
		T.r[0] = sext8(peekb(T.r[1]));
		fl::movb((uint8_t)T.r[0]);
		tick((uint16_t)(base + 0x32), 18);
		fl::tstb((uint8_t)T.r[3]);
		tick((uint16_t)(base + 0x34), 12);
		if (!(T.r[3] & 0x80))
			tick((uint16_t)(base + 0x3A), 12);         // BPL
		else
		{
			tick((uint16_t)(base + 0x36), 12);         // erasing: code 0x23
			T.r[0] = 0x0023;
			fl::mov(T.r[0]);
			tick((uint16_t)(base + 0x3A), 18);
		}
		jsr((uint16_t)(base + 0x3E), put_char);
		const uint8_t c = peekb(T.r[1]);
		fl::tstb(c);
		T.r[1]++;
		tick((uint16_t)(base + 0x40), 18);
		if (c & 0x80)
		{
			tick((uint16_t)(base + 0x42), 12);         // BPL not taken
			break;
		}
		tick((uint16_t)(base + 0x30), 12);
	}
	{
		const uint16_t v = pop();
		fl::mov(v);
		poke(0x01D2, v);
	}
	tick((uint16_t)(base + 0x46), 33);
	copy_w(0x01D2, 0x1400, (uint16_t)(base + 0x4C));
	c_restore(2);
}
void draw_string_large() { draw_string(0x8AB6, 0x8B52); }
void draw_string_small() { draw_string(0x8B04, 0x8C12); }

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
	c_save(2, 0x91AA);
	T.r[1] = peek(0x01D2);                             // the banks, put back at the end
	fl::mov(T.r[1]);
	tick(0x91AE, 24);
	T.r[2] = peek(0x01D4);
	fl::mov(T.r[2]);
	tick(0x91B2, 24);
	push_w(0x01B4, 0x91B6);                            // speed * x factor
	push_w(0x02E2, 0x91BA);
	jsr(0x91BE, 0x80FA);                               // mul32
	{
		const uint8_t v = peekb(0x0205);
		fl::movb(v);
		pokeb(0x07F0, v);
	}
	tick(0x91C4, 39);
	copy_w(0x0206, 0x04F0, 0x91CA);
	{
		const uint16_t v = peek(0x01B4);               // speed * y factor
		fl::mov(v);
		poke(T.r[6], v);
	}
	tick(0x91CE, 33);
	push_w(0x02E4, 0x91D2);
	jsr(0x91D6, 0x80FA);
	{
		const uint8_t v = peekb(0x0205);
		fl::movb(v);
		pokeb(0x07F4, v);
	}
	tick(0x91DC, 39);
	copy_w(0x0206, 0x0570, 0x91E2);
	T.r[0] = sext8(peekb(0x0EA0));                     // the object carrying the skater
	fl::movb((uint8_t)T.r[0]);
	tick(0x91E6, 24);
	if (!T.r[0])
		tick(0x91FA, 12);                              // BEQ
	else
	{
		tick(0x91E8, 12);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x003F));
		tick(0x91EC, 18);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x91EE, 12);
		poke(0x04F0, fl::add(peek(0x04F0), peek((uint16_t)(0x04F0 + T.r[0]))));
		tick(0x91F4, 42);
		poke(0x0570, fl::add(peek(0x0570), peek((uint16_t)(0x0570 + T.r[0]))));
		tick(0x91FA, 42);
	}
	T.r[0] = peek(0x01B4);                             // |speed| * 2
	fl::mov(T.r[0]);
	tick(0x91FE, 24);
	if ((int16_t)T.r[0] >= 0)
		tick(0x9202, 12);                              // BGE
	else
	{
		tick(0x9200, 12);
		T.r[0] = fl::neg(T.r[0]);
		tick(0x9202, 12);
	}
	T.r[0] = fl::asl(T.r[0]);
	tick(0x9204, 12);
	T.r[3] = T.r[0];                                   // its high byte: the speed class
	fl::mov(T.r[3]);
	tick(0x9206, 12);
	T.r[3] &= 0xFF00;
	fl::clr();
	tick(0x9208, 12);                                  // CLRB R3
	T.r[3] = fl::swab(T.r[3]);
	tick(0x920A, 12);
	T.r[3] = sext8((uint8_t)T.r[3]);
	fl::movb((uint8_t)T.r[3]);
	tick(0x920C, 12);
	pokeb(0x0331, (uint8_t)T.r[3]);
	fl::movb((uint8_t)T.r[3]);
	tick(0x9210, 27);
	fl::cmpb((uint8_t)T.r[3], 0x0F);
	tick(0x9214, 18);
	if ((int8_t)T.r[3] <= 15)
		tick(0x921C, 12);                              // BLE
	else
	{
		tick(0x9216, 12);
		set_b(0x0331, 0x0F, 0x921C);                   // at most 15
	}
	poke(0x01D2, T.r[1]);
	fl::mov(T.r[1]);
	tick(0x9220, 27);
	poke(0x01D4, T.r[2]);
	fl::mov(T.r[2]);
	tick(0x9224, 27);
	copy_w(0x01D2, 0x1400, 0x922A);
	copy_w(0x01D4, 0x1402, 0x9230);
	T.r[6] = fl::add(T.r[6], 0x0006);
	tick(0x9234, 18);
	c_restore(2);
	return T.r[0];
}

// F:843E  place_skater(int z, int y, int x): objects 0-3 at (x, y, z) without velocity, the
// skater's motion state reset (speed, ramp, terrain, gravity, ...), F:E238
uint16_t place_skater(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x8440, 24);
	T.r[1] = 0x0003;
	fl::mov(T.r[1]);
	tick(0x8444, 18);
	for (;;)                                           // objects 3..0
	{
		T.r[0] = T.r[1];
		fl::mov(T.r[0]);
		tick(0x8446, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x8448, 12);
		static const struct { uint16_t table; int k; uint16_t next; } at[3] = {
			{0x06F0, 8, 0x844E}, {0x0730, 6, 0x8454}, {0x06B0, 4, 0x845A},   // x, y, z
		};
		for (const auto &p : at)
		{
			const uint16_t v = peek((uint16_t)(T.r[6] + p.k));
			fl::mov(v);
			poke((uint16_t)(p.table + T.r[0]), v);
			tick(p.next, 45);
		}
		poke((uint16_t)(0x04F0 + T.r[0]), 0);          // no velocity
		fl::clr();
		tick(0x845E, 30);
		poke((uint16_t)(0x0570 + T.r[0]), 0);
		fl::clr();
		tick(0x8462, 30);
		poke((uint16_t)(0x05B0 + T.r[0]), 0);
		fl::clr();
		tick(0x8466, 30);
		pokeb((uint16_t)(0x07F0 + T.r[1]), 0);
		fl::clr();
		tick(0x846A, 30);
		pokeb((uint16_t)(0x07F4 + T.r[1]), 0);
		fl::clr();
		tick(0x846E, 30);
		pokeb((uint16_t)(0x07F8 + T.r[1]), 0);
		fl::clr();
		tick(0x8472, 30);
		T.r[1] = fl::dec(T.r[1]);
		tick(0x8474, 12);
		if ((int16_t)T.r[1] < 0)
		{
			tick(0x8476, 12);                          // BGE not taken
			break;
		}
		tick(0x8444, 12);
	}
	clr_w(0x01B4, 0x847A);                             // the motion state
	set_b(0x01EA, 0x01, 0x8480);
	clr_b(0x031E, 0x8484);
	set_b(0x0328, 0xFE, 0x848A);
	set_b(0x0320, 0x01, 0x8490);
	clr_b(0x031F, 0x8494);
	clr_b(0x0323, 0x8498);
	clr_b(0x0EA0, 0x849C);
	set_b(0x031B, 0xFF, 0x84A2);
	clr_b(0x032F, 0x84A6);
	clr_w(0x02FC, 0x84AA);
	clr_w(0x02FE, 0x84AE);
	clr_w(0x02EE, 0x84B2);
	set_b(0x0325, 0xFF, 0x84B8);
	clr_w(0x01B0, 0x84BC);
	clr_w(0x01B2, 0x84C0);
	set_w(0x0ED2, 0x3800, 0x84C6);
	set_w(0x0ED4, 0xA800, 0x84CC);
	jsr(0x84D0, 0xE238);                               // reset_skater_state
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x84D2, 18);
	rts();
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
	bank_a(0x944C, 0x0800);
	bank_b(0x9458, 0x0C00);
	T.r[3] = 0x0003;
	fl::mov(T.r[3]);
	tick(0x9468, 18);
	for (;;)                                           // the tiles' terrain table entries
	{
		T.r[0] = T.r[3];
		fl::mov(T.r[0]);
		tick(0x946A, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x946C, 12);
		T.r[1] = peek((uint16_t)(0x0300 + T.r[0]));
		fl::mov(T.r[1]);
		tick(0x9470, 27);
		const bool second = fl::logic((uint16_t)(T.r[1] & 0x0400)) != 0;
		tick(0x9474, 18);
		if (!second)
		{
			tick(0x9482, 12);                          // BEQ: the first tile bank
			T.r[2] = T.r[1];
			fl::mov(T.r[2]);
			tick(0x9484, 12);
			T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x03FF));
			tick(0x9488, 18);
			T.r[2] = fl::add(T.r[2], peek(0x01D6));
			tick(0x948C, 24);
		}
		else
		{
			tick(0x9476, 12);
			T.r[2] = T.r[1];
			fl::mov(T.r[2]);
			tick(0x9478, 12);
			T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x03FF));
			tick(0x947C, 18);
			T.r[2] = fl::add(T.r[2], peek(0x01D8));
			tick(0x9480, 24);
			tick(0x948C, 12);                          // BR
		}
		poke((uint16_t)(0x0300 + T.r[0]), T.r[2]);
		fl::mov(T.r[2]);
		tick(0x9490, 30);
		if (--T.r[3] == 0)
		{
			tick(0x9492, 18);                          // SOB
			break;
		}
		tick(0x9468, 18);
	}
	T.r[0] = 0x0001;
	fl::mov(T.r[0]);
	tick(0x9496, 18);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x9498, 12);
	T.r[0] = peek((uint16_t)(0x0302 + T.r[0]));
	fl::mov(T.r[0]);
	tick(0x949C, 27);
	{
		const uint8_t v = peekb((uint16_t)(0x6000 + T.r[0]));
		fl::movb(v);
		pokeb(0x032A, v);
	}
	tick(0x94A2, 42);
	T.r[0] = peek(0x0304);
	fl::mov(T.r[0]);
	tick(0x94A6, 24);
	{
		const uint8_t v = peekb((uint16_t)(0x6000 + T.r[0]));
		fl::movb(v);
		pokeb(0x032B, v);
	}
	tick(0x94AC, 42);
	T.r[2] = 0x0003;
	fl::mov(T.r[2]);
	tick(0x94B0, 18);
	for (;;)                                           // the terrain codes
	{
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0x94B2, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x94B4, 12);
		T.r[0] = fl::add(T.r[0], 0x0300);
		tick(0x94B8, 18);
		T.r[1] = peek(T.r[0]);
		fl::mov(T.r[1]);
		tick(0x94BA, 18);
		const uint8_t mode = peekb(0x032F);
		fl::cmpb(mode, 0x02);
		tick(0x94C0, 30);
		bool second = false;
		if (mode != 2)
			tick(0x94C8, 12);                          // BNE
		else
		{
			tick(0x94C2, 12);
			const uint8_t t = peekb((uint16_t)(0x6000 + T.r[1]));
			fl::tstb(t);
			tick(0x94C6, 27);
			second = (int8_t)t < 0;
			tick(second ? 0x94D0 : 0x94C8, 12);        // BLT
		}
		if (second)
		{
			{
				const uint8_t v = peekb((uint16_t)(0x6000 + T.r[1]));
				fl::movb(v);
				pokeb(T.r[0], v);
			}
			tick(0x94D4, 36);
			poke(T.r[0], fl::logic((uint16_t)(peek(T.r[0]) & 0x007F)));
			tick(0x94D8, 27);
		}
		else
		{
			T.r[3] = sext8(peekb((uint16_t)(0x4000 + T.r[1])));
			fl::movb((uint8_t)T.r[3]);
			tick(0x94CC, 27);
			poke(T.r[0], T.r[3]);
			fl::mov(T.r[3]);
			tick(0x94CE, 21);
			tick(0x94D8, 12);                          // BR
		}
		if (--T.r[2] == 0)
		{
			tick(0x94DA, 18);                          // SOB
			break;
		}
		tick(0x94B0, 18);
	}
	rts();
}

// F:934C (assembly): probe the playfield under the skater: from (x + 0x0A00 + 8 * dx,
// y + z + 0x0300 + 8 * (dy + dz)) (direction factors 0x02E3/0x02E5/0x02E7) three cells are
// read stepping back along the direction; per cell 0x0300 / 0x0306 / 0x030C / 0x0312 get the
// tile word, the row and column offsets and the video window address; 0x032E collects the
// high bits of the tiles; the last tile with bit 14 is remembered in 0x021C / 0x0214 / 0x021A
void probe_ground()
{
	bank_a(0x934C, 0x0800);
	bank_b(0x9358, 0x0C00);
	copy_w(0x06F0, 0x0220, 0x936A);                    // the probe: x + 0x0A00, y + z + 0x0300
	poke(0x0220, fl::add(peek(0x0220), 0x0A00));
	tick(0x9370, 33);
	T.r[0] = peek(0x0730);
	fl::mov(T.r[0]);
	tick(0x9374, 24);
	T.r[0] = fl::add(T.r[0], peek(0x06B0));
	tick(0x9378, 24);
	poke(0x021E, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x937C, 27);
	poke(0x021E, fl::add(peek(0x021E), 0x0300));
	tick(0x9382, 33);
	T.r[3] = sext8(peekb(0x02E3));                     // the steps: the direction factors * 8
	fl::movb((uint8_t)T.r[3]);
	tick(0x9386, 24);
	for (uint16_t pc = 0x9388; pc <= 0x938C; pc += 2)
	{
		T.r[3] = fl::asl(T.r[3]);
		tick(pc, 12);
	}
	T.r[0] = sext8(peekb(0x02E5));
	fl::movb((uint8_t)T.r[0]);
	tick(0x9390, 24);
	T.r[1] = sext8(peekb(0x02E7));
	fl::movb((uint8_t)T.r[1]);
	tick(0x9394, 24);
	T.r[0] = fl::add(T.r[0], T.r[1]);
	tick(0x9396, 12);
	for (uint16_t pc = 0x9398; pc <= 0x939C; pc += 2)
	{
		T.r[0] = fl::asl(T.r[0]);
		tick(pc, 12);
	}
	T.r[5] = T.r[3];
	fl::mov(T.r[5]);
	tick(0x939E, 12);
	poke(0x0212, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x93A2, 27);
	poke(0x0220, fl::add(peek(0x0220), T.r[3]));
	tick(0x93A6, 27);
	poke(0x021E, fl::add(peek(0x021E), T.r[0]));
	tick(0x93AA, 27);
	clr_b(0x032E, 0x93AE);
	set_b(0x0329, 0xFD, 0x93B4);
	T.r[4] = 0x0003;
	fl::mov(T.r[4]);
	tick(0x93B8, 18);
	for (;;)                                           // three points, stepping back
	{
		T.r[1] = peek(0x0220);
		fl::mov(T.r[1]);
		tick(0x93BC, 24);
		T.r[1] = fl::logic((uint16_t)(T.r[1] & 0xFF00));
		tick(0x93C0, 18);
		T.r[0] = peek(0x021E);
		fl::mov(T.r[0]);
		tick(0x93C4, 24);
		T.r[0] = fl::asr(T.r[0]);
		tick(0x93C6, 12);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x3F00));
		tick(0x93CA, 18);
		T.r[2] = 0x3F00;                               // the playfield row
		fl::mov(T.r[2]);
		tick(0x93CE, 18);
		T.r[2] = fl::sub(T.r[2], T.r[0]);
		tick(0x93D0, 12);
		poke(0x0218, T.r[2]);
		fl::mov(T.r[2]);
		tick(0x93D4, 27);
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0x93D6, 12);
		jsr(0x93DA, 0x8078);                           // select_video_bank
		T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x1F00));
		tick(0x93DE, 18);
		T.r[1] = fl::logic((uint16_t)(T.r[1] & 0xFE00));
		tick(0x93E2, 18);
		T.r[3] = T.r[1];
		fl::mov(T.r[3]);
		tick(0x93E4, 12);
		T.r[3] &= 0xFF00;
		fl::clr();
		tick(0x93E6, 12);                              // CLRB R3
		T.r[3] = fl::swab(T.r[3]);
		tick(0x93E8, 12);
		T.r[3] = fl::add(T.r[3], T.r[2]);
		tick(0x93EA, 12);
		T.r[3] = peek((uint16_t)(0x2000 + T.r[3]));    // the tile there
		fl::mov(T.r[3]);
		tick(0x93EE, 27);
		const bool high = fl::logic((uint16_t)(T.r[3] & 0x4000)) != 0;
		tick(0x93F2, 18);
		if (!high)
			tick(0x9404, 12);                          // BEQ
		else
		{
			tick(0x93F4, 12);                          // the last point on high ground
			copy_w(0x021E, 0x021C, 0x93FA);
			poke(0x0214, T.r[1]);
			fl::mov(T.r[1]);
			tick(0x93FE, 27);
			copy_w(0x0218, 0x021A, 0x9404);
		}
		T.r[0] = T.r[3];
		fl::mov(T.r[0]);
		tick(0x9406, 12);
		T.r[0] &= 0xFF00;
		fl::clr();
		tick(0x9408, 12);                              // CLRB R0
		T.r[0] = fl::swab(T.r[0]);
		tick(0x940A, 12);
		T.r[0] = sext8((uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x940C, 12);
		pokeb(0x032E, fl::logicb((uint8_t)(peekb(0x032E) | T.r[0])));
		tick(0x9410, 27);
		T.r[0] = T.r[4];
		fl::mov(T.r[0]);
		tick(0x9412, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x9414, 12);
		poke((uint16_t)(0x0300 + T.r[0]), T.r[3]);     // tile, row, column, playfield row
		fl::mov(T.r[3]);
		tick(0x9418, 30);
		poke((uint16_t)(0x0306 + T.r[0]), T.r[2]);
		fl::mov(T.r[2]);
		tick(0x941C, 30);
		poke((uint16_t)(0x030C + T.r[0]), T.r[1]);
		fl::mov(T.r[1]);
		tick(0x9420, 30);
		{
			const uint16_t v = peek(0x0218);
			fl::mov(v);
			poke((uint16_t)(0x0312 + T.r[0]), v);
		}
		tick(0x9426, 42);
		poke(0x0220, fl::sub(peek(0x0220), T.r[5]));
		tick(0x942A, 27);
		poke(0x021E, fl::sub(peek(0x021E), peek(0x0212)));
		tick(0x9430, 39);
		if (--T.r[4] == 0)
		{
			tick(0x9432, 18);                          // SOB
			break;
		}
		tick(0x93B8, 18);
	}
	copy_w(0x030A, 0x021E, 0x9438);
	copy_w(0x0310, 0x0220, 0x943E);
	copy_w(0x0316, 0x0218, 0x9444);
	pokeb(0x032E, fl::logicb((uint8_t)(peekb(0x032E) & 0xC0)));
	tick(0x944A, 33);
	rts();
}

// F:9B82  skater_motion_params(): index = direction 0x031D, moved into the slope tables by
// the terrain 0x0327 (bank B 0x2C00); direction factors 0x02E2-0x02E6 from the tables at
// 0x6048 / 0x6588 / 0x6AC8; on a ramp (0x01EA) the gravity 0x02EE (0x7668, halved on park 3
// below the speed limit 0x7C68), otherwise the jump values 0x02EA (0x7008) / 0x02EC (0x7228).
// Bank B restored; returns the index.  R1-R4 preserved (F152).
uint16_t skater_motion_params(const Args &)
{
	c_save(3, 0x9B86);
	T.r[4] = peek(0x01D4);                             // bank B, put back at the end
	fl::mov(T.r[4]);
	tick(0x9B8A, 24);
	bank_b(0x9B8A, 0x2C00);
	T.r[0] = sext8(peekb(0x031D));                     // the direction
	fl::movb((uint8_t)T.r[0]);
	tick(0x9B9A, 24);
	T.r[3] = sext8(peekb(0x0327));                     // the terrain
	fl::movb((uint8_t)T.r[3]);
	tick(0x9B9E, 24);
	const bool sloped = fl::logic((uint16_t)(T.r[3] & 0x0007)) != 0;
	tick(0x9BA2, 18);
	if (!sloped)
		tick(0x9BDE, 12);                              // BEQ: the direction alone
	else
	{
		tick(0x9BA4, 12);
		T.r[1] = 0x0007;
		fl::mov(T.r[1]);
		tick(0x9BA8, 18);
		T.r[1] = fl::logic((uint16_t)(T.r[1] & ~T.r[3]));
		tick(0x9BAA, 12);
		if (!T.r[1])
		{
			tick(0x9BCE, 12);                          // BEQ: 7, the row of a block at 0x220
			T.r[1] = T.r[3];
			fl::mov(T.r[1]);
			tick(0x9BD0, 12);
			T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x0038));
			tick(0x9BD4, 18);
			T.r[1] = fl::asl(T.r[1]);
			tick(0x9BD6, 12);
			T.r[1] = fl::add(T.r[1], T.r[0]);
			tick(0x9BD8, 12);
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0x9BDA, 12);
			T.r[0] = fl::add(T.r[0], 0x0220);
			tick(0x9BDE, 18);
		}
		else
		{
			tick(0x9BAC, 12);                          // the row table at 0x79E8
			T.r[1] = T.r[3];
			fl::mov(T.r[1]);
			tick(0x9BAE, 12);
			for (uint16_t pc = 0x9BB0; pc <= 0x9BB4; pc += 2)
			{
				T.r[1] = fl::asr(T.r[1]);
				tick(pc, 12);
			}
			T.r[1] = fl::asl(T.r[1]);
			tick(0x9BB6, 12);
			T.r[2] = T.r[3];
			fl::mov(T.r[2]);
			tick(0x9BB8, 12);
			T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x0007));
			tick(0x9BBC, 18);
			for (uint16_t pc = 0x9BBE; pc <= 0x9BC4; pc += 2)
			{
				T.r[2] = fl::asl(T.r[2]);
				tick(pc, 12);
			}
			T.r[2] = fl::add(T.r[2], peek((uint16_t)(0x79E8 + T.r[1])));
			tick(0x9BC8, 27);
			T.r[2] = fl::add(T.r[2], T.r[0]);
			tick(0x9BCA, 12);
			T.r[0] = T.r[2];
			fl::mov(T.r[0]);
			tick(0x9BCC, 12);
			tick(0x9BDE, 12);                          // BR
		}
	}
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0x9BE0, 12);
	T.r[1] = fl::asl(T.r[1]);
	tick(0x9BE2, 12);
	static const struct { uint16_t table, to, next; } factor[3] = {
		{0x6048, 0x02E2, 0x9BE8}, {0x6588, 0x02E4, 0x9BEE}, {0x6AC8, 0x02E6, 0x9BF4},
	};
	for (const auto &f : factor)                       // the direction factors
	{
		const uint16_t v = peek((uint16_t)(f.table + T.r[1]));
		fl::mov(v);
		poke(f.to, v);
		tick(f.next, 42);
	}
	const uint8_t ramp = peekb(0x01EA);
	fl::tstb(ramp);
	tick(0x9BF8, 24);
	if (ramp)
	{
		tick(0x9BFA, 12);                              // the gravity
		T.r[2] = sext8(peekb((uint16_t)(0x7668 + T.r[0])));
		fl::movb((uint8_t)T.r[2]);
		tick(0x9BFE, 27);
		poke(0x02EE, T.r[2]);
		fl::mov(T.r[2]);
		tick(0x9C02, 27);
		const uint8_t level = peekb(0x0F59);
		fl::cmpb(level, 0x03);
		tick(0x9C08, 30);
		if (level != 3)
			tick(0x9C54, 12);                          // BNE
		else
		{
			tick(0x9C0A, 12);                          // park 3: halved below the speed limit
			fl::tst(T.r[2]);
			tick(0x9C0C, 12);
			if ((int16_t)T.r[2] >= 0)
				tick(0x9C54, 12);                      // BGE
			else
			{
				tick(0x9C0E, 12);
				const uint16_t speed = peek(0x01B4);
				fl::tst(speed);
				tick(0x9C12, 24);
				if ((int16_t)speed <= 0)
					tick(0x9C54, 12);                  // BLE
				else
				{
					tick(0x9C14, 12);
					T.r[1] = sext8(peekb(0x025C));
					fl::movb((uint8_t)T.r[1]);
					tick(0x9C18, 24);
					T.r[1] = fl::asl(T.r[1]);
					tick(0x9C1A, 12);
					const uint16_t speed2 = peek(0x01B4);
					const uint16_t limit = peek((uint16_t)(0x7C68 + T.r[1]));
					fl::cmp(speed2, limit);
					tick(0x9C20, 39);
					if ((int16_t)speed2 >= (int16_t)limit)
						tick(0x9C54, 12);              // BGE
					else
					{
						tick(0x9C22, 12);
						poke(0x02EE, fl::asr(peek(0x02EE)));
						tick(0x9C26, 27);
						tick(0x9C54, 12);              // BR
					}
				}
			}
		}
	}
	else
	{
		tick(0x9C28, 12);                              // BEQ: the jump values
		T.r[2] = 0x0007;
		fl::mov(T.r[2]);
		tick(0x9C2C, 18);
		T.r[2] = fl::logic((uint16_t)(T.r[2] & ~T.r[3]));
		tick(0x9C2E, 12);
		if (!T.r[2])
		{
			tick(0x9C4C, 12);                          // BEQ
			clr_w(0x02EA, 0x9C50);
			clr_w(0x02EC, 0x9C54);
		}
		else
		{
			tick(0x9C30, 12);
			{
				const uint8_t v = peekb((uint16_t)(0x7008 + T.r[0]));
				fl::movb(v);
				pokeb(0x02EA, v);
			}
			tick(0x9C36, 42);
			clr_b(0x02EB, 0x9C3A);
			const uint16_t jump = peek(0x02EA);
			fl::tst(jump);
			tick(0x9C3E, 24);
			if (!jump)
				tick(0x9C44, 12);                      // BEQ
			else
			{
				tick(0x9C40, 12);
				poke(0x02EA, fl::inc(peek(0x02EA)));
				tick(0x9C44, 27);
			}
			{
				const uint16_t v = peek((uint16_t)(0x7228 + T.r[1]));
				fl::mov(v);
				poke(0x02EC, v);
			}
			tick(0x9C4A, 42);
			tick(0x9C54, 12);                          // BR
		}
	}
	poke(0x01D4, T.r[4]);
	fl::mov(T.r[4]);
	tick(0x9C58, 27);
	poke(0x1402, T.r[4]);
	fl::mov(T.r[4]);
	tick(0x9C5C, 27);
	c_restore(3);
	return T.r[0];
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
	bank_b(0x9236, 0x2C00);
	T.r[0] = sext8(peekb(0x0193));                     // the push button now
	fl::movb((uint8_t)T.r[0]);
	tick(0x9246, 24);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0080));
	tick(0x924A, 18);
	T.r[1] = 0;                                        // and before
	fl::clr();
	tick(0x924C, 12);
	T.r[1] = fl::logicb(peekb(0x0333));
	tick(0x9250, 24);
	fl::cmp(T.r[0], T.r[1]);
	tick(0x9252, 12);
	if (T.r[0] == T.r[1])
		tick(0x927A, 12);                              // BEQ
	else
	{
		// it changed: the push adds to 0x02FE by the equipment and the speed class
		tick(0x9254, 12);
		{
			const uint8_t v = peekb(0x0193);
			fl::movb(v);
			pokeb(0x0333, v);
		}
		tick(0x925A, 39);
		pokeb(0x0333, fl::logicb((uint8_t)(peekb(0x0333) & 0x80)));
		tick(0x9260, 33);
		T.r[0] = sext8(peekb(0x025F));
		fl::movb((uint8_t)T.r[0]);
		tick(0x9264, 24);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x9266, 12);
		poke(0x02FE, fl::add(peek(0x02FE), peek((uint16_t)(0x7C80 + T.r[0]))));
		tick(0x926C, 42);
		T.r[0] = sext8(peekb(0x0331));
		fl::movb((uint8_t)T.r[0]);
		tick(0x9270, 24);
		T.r[1] = 0;
		fl::clr();
		tick(0x9272, 12);
		T.r[1] = fl::logicb(peekb((uint16_t)(0x7CB0 + T.r[0])));
		tick(0x9276, 27);
		poke(0x02FE, fl::add(peek(0x02FE), T.r[1]));
		tick(0x927A, 27);
	}
	T.r[0] = sext8(peekb(0x025F));
	fl::movb((uint8_t)T.r[0]);
	tick(0x927E, 24);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x9280, 12);
	T.r[1] = peek((uint16_t)(0x7C88 + T.r[0]));        // the equipment's top speed
	fl::mov(T.r[1]);
	tick(0x9284, 27);
	T.r[3] = peek(0x02FC);
	fl::mov(T.r[3]);
	tick(0x9288, 24);
	T.r[5] = 0;
	fl::clr();
	tick(0x928A, 12);
	fl::cmp(T.r[3], T.r[1]);
	tick(0x928C, 12);
	if ((int16_t)T.r[3] <= (int16_t)T.r[1])
	{
		tick(0x9294, 12);                              // BLE
		T.r[0] = 0x0040;
		fl::mov(T.r[0]);
		tick(0x9298, 18);
	}
	else
	{
		tick(0x928E, 12);
		T.r[5] = fl::inc(T.r[5]);
		tick(0x9290, 12);
		T.r[0] = 0;
		fl::clr();
		tick(0x9292, 12);
		tick(0x9298, 12);                              // BR
	}
	T.r[4] = 0;
	fl::clr();
	tick(0x929A, 12);
	T.r[4] = fl::logicb(peekb(0x0333));
	tick(0x929E, 24);
	fl::tst(T.r[4]);
	tick(0x92A0, 12);
	if (!T.r[4])
		tick(0x92C8, 12);                              // BEQ
	else
	{
		tick(0x92A2, 12);
		T.r[2] = T.r[0];
		fl::mov(T.r[2]);
		tick(0x92A4, 12);
		T.r[2] = fl::add(T.r[2], peek(0x01B4));
		tick(0x92A8, 24);
		fl::cmp(T.r[3], T.r[2]);
		tick(0x92AA, 12);
		if ((int16_t)T.r[3] <= (int16_t)T.r[2])
			tick(0x92C8, 12);                          // BLE
		else
		{
			tick(0x92AC, 12);
			const uint8_t level = peekb(0x0F59);
			fl::cmpb(level, 0x03);
			tick(0x92B2, 30);
			bool clear;
			if (level != 3)
			{
				tick(0x92C4, 12);                      // BNE
				clear = true;
			}
			else
			{
				tick(0x92B4, 12);
				const uint16_t zone = peek(0x0ED0);
				fl::cmp(zone, 0x0002);
				tick(0x92BA, 30);
				if ((int16_t)zone > 2)
				{
					tick(0x92C4, 12);                  // BGT
					clear = true;
				}
				else
				{
					tick(0x92BC, 12);
					const uint16_t speed = peek(0x01B4);
					fl::cmp(speed, 0xFE80);
					tick(0x92C2, 30);
					clear = (int16_t)speed >= (int16_t)0xFE80;
					tick(clear ? 0x92C4 : 0x92C8, 12); // BLT
				}
			}
			if (clear)
				clr_b(0x0320, 0x92C8);
		}
	}
	{
		const uint8_t kick = peekb(0x0330);
		fl::cmpb(kick, 0x04);
		tick(0x92CE, 30);
		bool set;
		if (kick == 4)
		{
			tick(0x92E4, 12);                          // BEQ
			set = true;
		}
		else
		{
			tick(0x92D0, 12);
			const uint8_t air = peekb(0x031E);
			fl::tstb(air);
			tick(0x92D4, 24);
			if (air)
			{
				tick(0x92E4, 12);                      // BNE
				set = true;
			}
			else
			{
				tick(0x92D6, 12);
				T.r[2] = sext8(peekb(0x0327));
				fl::movb((uint8_t)T.r[2]);
				tick(0x92DA, 24);
				T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x0007));
				tick(0x92DE, 18);
				fl::cmp(T.r[2], 0x0004);
				tick(0x92E2, 18);
				set = T.r[2] >= 4;
				tick(set ? 0x92E4 : 0x92EA, 12);       // BCS
			}
		}
		if (set)
			set_b(0x0320, 0x01, 0x92EA);
	}
	{
		const bool off = fl::logic((uint16_t)(peek(0x01C4) & 0x000F)) != 0;   // every 16 frames
		tick(0x92F0, 30);
		if (off)
		{
			tick(0x934A, 12);                          // BNE
			rts();
			return;
		}
	}
	tick(0x92F2, 12);
	poke(0x02FE, fl::asr(peek(0x02FE)));               // the push decays, at least 0x180
	tick(0x92F6, 27);
	{
		const uint16_t push = peek(0x02FE);
		fl::cmp(push, 0x0180);
		tick(0x92FC, 30);
		if ((int16_t)push >= 0x0180)
			tick(0x9304, 12);                          // BGE
		else
		{
			tick(0x92FE, 12);
			set_w(0x02FE, 0x0180, 0x9304);
		}
	}
	T.r[0] = peek(0x01B4);                             // the speed limited by the top speed
	fl::mov(T.r[0]);
	tick(0x9308, 24);
	fl::cmp(T.r[0], T.r[1]);
	tick(0x930A, 12);
	if ((int16_t)T.r[0] <= (int16_t)T.r[1])
		tick(0x930E, 12);                              // BLE
	else
	{
		tick(0x930C, 12);
		T.r[0] = T.r[1];
		fl::mov(T.r[0]);
		tick(0x930E, 12);
	}
	{
		const uint16_t push = peek(0x02FE);            // at least the push
		fl::cmp(T.r[0], push);
		tick(0x9312, 24);
		if ((int16_t)T.r[0] >= (int16_t)push)
			tick(0x9318, 12);                          // BGE
		else
		{
			tick(0x9314, 12);
			T.r[0] = peek(0x02FE);
			fl::mov(T.r[0]);
			tick(0x9318, 24);
		}
	}
	T.r[1] = fl::add(T.r[1], 0x0080);
	tick(0x931C, 18);
	fl::cmp(T.r[0], T.r[1]);
	tick(0x931E, 12);
	if ((int16_t)T.r[0] <= (int16_t)T.r[1])
		tick(0x9322, 12);                              // BLE
	else
	{
		tick(0x9320, 12);
		T.r[0] = T.r[1];
		fl::mov(T.r[0]);
		tick(0x9322, 12);
	}
	fl::cmp(T.r[3], T.r[0]);
	tick(0x9324, 12);
	if ((int16_t)T.r[3] < (int16_t)T.r[0])
	{
		tick(0x9326, 12);                              // 0x02FC half way towards it
		T.r[0] = fl::sub(T.r[0], T.r[3]);
		tick(0x9328, 12);
		T.r[0] = fl::asr(T.r[0]);
		tick(0x932A, 12);
		T.r[0] = fl::add(T.r[0], peek(0x02FC));
		tick(0x932E, 24);
		poke(0x02FC, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x9332, 27);
		rts();
		return;
	}
	tick(0x9334, 12);                                  // BGE
	fl::tst(T.r[4]);
	tick(0x9336, 12);
	if (T.r[4])
	{
		tick(0x9338, 12);
		T.r[5] = fl::ror(T.r[5]);
		tick(0x933A, 12);
		if (!C())
		{
			tick(0x9344, 12);                          // BCC: up by 6
			poke(0x02FC, fl::add(peek(0x02FC), 0x0006));
			tick(0x934A, 33);
			rts();
			return;
		}
		tick(0x933C, 12);
	}
	else
		tick(0x933C, 12);                              // BEQ
	poke(0x02FC, fl::sub(peek(0x02FC), 0x0018));       // down by 24
	tick(0x9342, 33);
	rts();
}

// F:899C (interrupt, vertical blank during play): palette fade (F:DBE2) and the pending
// palette routine (F:DDEC), banked palette and sound level updates, watchdog, frame counters
// and acknowledges, timers 0x0F18 / 0x0F22, scroll registers and input, the sound queue;
// the text pointer, banks and video window are put back
void irq_vblank_play()
{
	for (int i = 0; i <= 5; i++)
	{
		push(T.r[i]);
		fl::mov(T.r[i]);
		tick((uint16_t)(0x899E + 2 * i), 24);
	}
	push_w(0x01D0, 0x89AC);                            // kept for the end
	push_w(0x01D2, 0x89B0);
	push_w(0x01D4, 0x89B4);
	push_w(0x0186, 0x89B8);
	jsr(0x89BC, 0xDBE2);                               // step_palette_fade
	jsr(0x89C0, 0xDDEC);                               // run_palette_job
	bank_a(0x89C0, 0x1800);
	bank_b(0x89CC, 0x1C00);
	jsr(0x89DC, 0x7D82);
	jsr(0x89E0, 0x917C);                               // feed_watchdog
	pokeb(0x0192, fl::incb(peekb(0x0192)));            // the frame counters and acknowledges
	tick(0x89E4, 27);
	clr_w(0x15E0, 0x89E8);
	clr_w(0x15C0, 0x89EC);
	clr_b(0x0191, 0x89F0);
	poke(0x01C4, fl::inc(peek(0x01C4)));
	tick(0x89F4, 27);
	{
		const uint16_t left = peek(0x0F18);            // the timer 0x0F18 (-1: off)
		fl::cmp(left, 0xFFFF);
		tick(0x89FA, 30);
		if (left == 0xFFFF)
			tick(0x8A00, 12);                          // BEQ
		else
		{
			tick(0x89FC, 12);
			poke(0x0F18, fl::dec(peek(0x0F18)));
			tick(0x8A00, 27);
		}
	}
	bank_a(0x8A00, 0x1800);
	jsr(0x8A10, 0x5D86);
	bank_a(0x8A10, 0x3800);
	bank_b(0x8A1C, 0x3C00);
	jsr(0x8A2C, 0x7A3A);
	jsr(0x8A30, 0x80A4);                               // read_input_edges
	jsr(0x8A34, 0x8036);                               // load_scroll_registers
	{
		const uint16_t bee = peek(0x0F22);             // the bee timer
		fl::tst(bee);
		tick(0x8A38, 24);
		if ((int16_t)bee <= 0)
			tick(0x8A44, 12);                          // BLE
		else
		{
			tick(0x8A3A, 12);
			const uint16_t frozen = peek(0x0180);
			fl::tst(frozen);
			tick(0x8A3E, 24);
			if (frozen)
				tick(0x8A44, 12);                      // BNE
			else
			{
				tick(0x8A40, 12);
				poke(0x0F22, fl::dec(peek(0x0F22)));
				tick(0x8A44, 27);
			}
		}
	}
	{
		const uint8_t busy = peekb(0x01A4);            // the sound queue
		fl::tstb(busy);
		tick(0x8A48, 24);
		if (busy)
			tick(0x8A66, 12);                          // BNE
		else
		{
			tick(0x8A4A, 12);
			bank_a(0x8A4A, 0x3800);
			bank_b(0x8A56, 0x3C00);
			jsr(0x8A66, 0x4314);
		}
	}
	copy_sp(0, 0x0186, 0x8A6A);                        // put back
	copy_sp(4, 0x01D2, 0x8A70);
	copy_w(0x01D2, 0x1400, 0x8A76);
	copy_sp(2, 0x01D4, 0x8A7C);
	copy_w(0x01D4, 0x1402, 0x8A82);
	copy_sp(6, 0x01D0, 0x8A88);
	jsr(0x8A8C, 0x8096);                               // reselect_video_bank
	T.r[6] = fl::add(T.r[6], 0x0008);
	tick(0x8A90, 18);
	for (int i = 5; i >= 0; i--)
	{
		T.r[i] = pop();
		fl::mov(T.r[i]);
		tick((uint16_t)(0x8A92 + 2 * (5 - i)), 18);
	}
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
	pokeb(0x0196, fl::incb(peekb(0x0196)));
	tick(0x8516, 27);
	push_w(0x01D0, 0x851A);                            // kept for the end
	push_w(0x01D2, 0x851E);
	push_w(0x01D4, 0x8522);
	push_w(0x0186, 0x8526);
	push_w(0x09B2, 0x852A);
	push_w(0x09B6, 0x852E);
	push_w(0x09B4, 0x8532);
	copy_w(0x09AA, 0x09A8, 0x8538);
	jsr(0x853C, 0x8036);                               // load_scroll_registers
	bank_a(0x853C, 0x1800);
	bank_b(0x8548, 0x1C00);
	jsr(0x8558, 0x7BBC);                               // the palette animation
	{
		const uint8_t level = peekb(0x0F59);
		fl::cmpb(level, 0x04);
		tick(0x855E, 30);
		if (level != 4)
			tick(0x8564, 12);                          // BNE
		else
		{
			tick(0x8560, 12);
			jsr(0x8564, 0xDD10);
		}
	}
	{
		const uint8_t state = peekb(0x018F);
		fl::tstb(state);
		tick(0x8568, 24);
		if (state)
			tick(0x857A, 12);                          // BNE
		else
		{
			tick(0x856A, 12);
			bank_b(0x856A, 0x3C00);
			jsr(0x857A, 0x7F52);                       // the game timer
		}
	}
	bank_a(0x857A, 0x3800);
	bank_b(0x8586, 0x3C00);
	jsr(0x8596, 0x7A3A);
	jsr(0x859A, 0x917C);                               // feed_watchdog
	jsr(0x859E, 0x80A4);                               // read_input_edges
	{
		const uint8_t busy = peekb(0x01A4);            // the sound queue unless the sound CPU
		fl::tstb(busy);                                // is being talked to
		tick(0x85A2, 24);
		if (busy)
			tick(0x85C0, 12);                          // BNE
		else
		{
			tick(0x85A4, 12);
			bank_a(0x85A4, 0x3800);
			bank_b(0x85B0, 0x3C00);
			jsr(0x85C0, 0x4314);
		}
	}
	{
		const uint16_t bee = peek(0x0F22);             // the bee timer
		fl::tst(bee);
		tick(0x85C4, 24);
		if ((int16_t)bee <= 0)
			tick(0x85D0, 12);                          // BLE
		else
		{
			tick(0x85C6, 12);
			const uint16_t frozen = peek(0x0180);
			fl::tst(frozen);
			tick(0x85CA, 24);
			if (frozen)
				tick(0x85D0, 12);                      // BNE
			else
			{
				tick(0x85CC, 12);
				poke(0x0F22, fl::dec(peek(0x0F22)));
				tick(0x85D0, 27);
			}
		}
	}
	{
		const uint8_t level = peekb(0x0F59);           // the town's zone scan
		fl::tstb(level);
		tick(0x85D4, 24);
		if (level)
			tick(0x85F2, 12);                          // BNE
		else
		{
			tick(0x85D6, 12);
			bank_a(0x85D6, 0x1800);
			bank_b(0x85E2, 0x1C00);
			jsr(0x85F2, 0x454A);                       // scan_zone_switches
		}
	}
	{
		const uint8_t busy = peekb(0x01A4);
		fl::tstb(busy);
		tick(0x85F6, 24);
		if (busy)
			tick(0x8634, 12);                          // BNE
		else
		{
			// the controller and sound exchange: F:5BD2 every 32 frames, F:910C 16 later
			tick(0x85F8, 12);
			jsr(0x85FC, 0x8D86);                       // read_controller
			const bool off = fl::logic((uint16_t)(peek(0x01C4) & 0x001F)) != 0;
			tick(0x8602, 30);
			if (!off)
			{
				tick(0x8604, 12);
				bank_a(0x8604, 0x1800);
				bank_b(0x8610, 0x1C00);
				jsr(0x8620, 0x5BD2);                   // town_music
				tick(0x8634, 12);                      // BR
			}
			else
			{
				tick(0x8622, 12);                      // BNE
				T.r[0] = peek(0x01C4);
				fl::mov(T.r[0]);
				tick(0x8626, 24);
				T.r[0] = fl::add(T.r[0], 0x0010);
				tick(0x862A, 18);
				const bool off2 = fl::logic((uint16_t)(T.r[0] & 0x001F)) != 0;
				tick(0x862E, 18);
				if (off2)
					tick(0x8634, 12);                  // BNE
				else
				{
					tick(0x8630, 12);
					jsr(0x8634, 0x910C);               // read_sound_status
				}
			}
		}
	}
	{
		const uint8_t held = peekb(0x0995);            // the sprites held
		fl::tstb(held);
		tick(0x8638, 24);
		if (held)
			tick(0x86CC, 12);                          // BNE
		else
		{
			tick(0x863A, 12);
			const uint8_t level = peekb(0x0F59);
			fl::tstb(level);
			tick(0x863E, 24);
			if (level)
				tick(0x8644, 12);                      // BNE
			else
			{
				tick(0x8640, 12);
				jsr(0x8644, 0xBC52);                   // check_map_objects_in_view
			}
			const uint8_t level2 = peekb(0x0F59);
			fl::cmpb(level2, 0x01);
			tick(0x864A, 30);
			if (level2 > 1)
				tick(0x8650, 12);                      // BHI
			else
			{
				tick(0x864C, 12);
				jsr(0x8650, 0xCB24);                   // update_objects
			}
			bank_a(0x8650, 0x2400);
			bank_b(0x865C, 0x2800);
			jsr(0x866C, 0x7B82);                       // the scroll blocks
			jsr(0x8670, 0xA510);                       // object_contact
			const uint8_t chaser = peekb(0x0374);
			fl::tstb(chaser);
			tick(0x8674, 24);
			if (!chaser)
				tick(0x8692, 12);                      // BEQ
			else
			{
				tick(0x8676, 12);
				bank_a(0x8676, 0x1800);
				bank_b(0x8682, 0x1C00);
				jsr(0x8692, 0x7AA2);
			}
			const uint8_t state = peekb(0x018F);
			fl::cmpb(state, 0x04);
			tick(0x8698, 30);
			if (state == 4)
			{
				tick(0x86C0, 12);                      // BEQ: the demo's countdown 0x0F18
				const uint16_t left = peek(0x0F18);
				fl::cmp(left, 0xFFFF);
				tick(0x86C6, 30);
				if (left == 0xFFFF)
					tick(0x86CC, 12);                  // BEQ
				else
				{
					tick(0x86C8, 12);
					poke(0x0F18, fl::dec(peek(0x0F18)));
					tick(0x86CC, 27);
				}
			}
			else
			{
				tick(0x869A, 12);
				bank_a(0x869A, 0x3800);
				bank_b(0x86A6, 0x3C00);
				jsr(0x86B6, 0x538E);                   // terrain_event
				jsr(0x86BA, 0x50F8);                   // fallen_skater
				jsr(0x86BE, 0xE036);                   // countdown_0eb4
				tick(0x86CC, 12);                      // BR
			}
		}
	}
	copy_w(0x09A8, 0x09AA, 0x86D2);                    // put back
	copy_sp(0, 0x09B4, 0x86D6);
	copy_sp(2, 0x09B6, 0x86DC);
	copy_sp(4, 0x09B2, 0x86E2);
	copy_sp(6, 0x0186, 0x86E8);
	copy_sp(0x0A, 0x01D2, 0x86EE);
	copy_w(0x01D2, 0x1400, 0x86F4);
	copy_sp(8, 0x01D4, 0x86FA);
	copy_w(0x01D4, 0x1402, 0x8700);
	copy_sp(0x0C, 0x01D0, 0x8706);
	jsr(0x870A, 0x8096);                               // reselect_video_bank
	pokeb(0x0196, fl::decb(peekb(0x0196)));
	tick(0x870E, 27);
	T.r[6] = fl::add(T.r[6], 0x000E);
	tick(0x8712, 18);
	rts();
}

// F:84D4 (interrupt, vertical blank in attract mode): count the frame (0x01C4, 0x0192),
// acknowledge both display interrupts, restart the quarter-frame count 0x0191 and, with
// interrupts allowed again, do the frame's work (F:8512) unless it is still running
void irq_vblank_attract()
{
	for (int i = 0; i <= 5; i++)
	{
		push(T.r[i]);
		fl::mov(T.r[i]);
		tick((uint16_t)(0x84D6 + 2 * i), 24);
	}
	poke(0x01C4, fl::inc(peek(0x01C4)));               // the frame counter
	tick(0x84E4, 27);
	pokeb(0x0192, fl::incb(peekb(0x0192)));
	tick(0x84E8, 27);
	clr_w(0x15C0, 0x84EC);                             // ACK_32V_IRQ
	clr_b(0x0191, 0x84F0);
	clr_w(0x15E0, 0x84F4);                             // ACK_VBLANK_IRQ
	T.r[0] = 0;
	fl::clr();
	tick(0x84F6, 12);
	mtps(0);
	tick(0x84FA, 30);
	const uint8_t busy = peekb(0x0196);
	fl::tstb(busy);
	tick(0x84FE, 24);
	if (busy)
		tick(0x8504, 12);                              // BNE
	else
	{
		tick(0x8500, 12);
		jsr(0x8504, 0x8512);                           // attract_frame
	}
	for (int i = 5; i >= 0; i--)
	{
		T.r[i] = pop();
		fl::mov(T.r[i]);
		tick((uint16_t)(0x8506 + 2 * (5 - i)), 18);
	}
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
	bank_a(0x94DC, 0x0800);
	bank_b(0x94E8, 0x0C00);
	clr_w(0x0222, 0x94F8);
	T.r[2] = peek(0x0304);                             // cell 2's tile: its attribute entry
	fl::mov(T.r[2]);
	tick(0x94FC, 24);
	{
		const bool second = fl::logic((uint16_t)(T.r[2] & 0x0400)) != 0;
		tick(0x9500, 18);
		if (!second)
		{
			tick(0x950C, 12);                          // BEQ
			T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x03FF));
			tick(0x9510, 18);
			T.r[2] = fl::add(T.r[2], peek(0x01D6));
			tick(0x9514, 24);
		}
		else
		{
			tick(0x9502, 12);
			T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x03FF));
			tick(0x9506, 18);
			T.r[2] = fl::add(T.r[2], peek(0x01D8));
			tick(0x950A, 24);
			tick(0x9514, 12);                          // BR
		}
	}
	T.r[0] = 0;
	fl::clr();
	tick(0x9516, 12);
	T.r[0] = fl::logicb(peekb(0x032E));                // the tiles' bits
	tick(0x951A, 24);
	T.r[1] = sext8(peekb(0x032F));                     // the mode
	fl::movb((uint8_t)T.r[1]);
	tick(0x951E, 24);
	T.r[1] = fl::asl(T.r[1]);
	tick(0x9520, 12);
	uint16_t at = fl::add(0x9524, peek((uint16_t)(0xFCF0 + T.r[1])));
	tick(at, 27);                                      // ADD 0xFCF0(R1),PC
	for (;;)
	{
		switch (at)
		{
		case 0x9524:                                   // mode 0: bit 6 starts mode 2, a lone bit 7 mode 1
		{
			const bool b6 = fl::logic((uint16_t)(T.r[0] & 0x0040)) != 0;
			tick(0x9528, 18);
			if (b6)
			{
				tick(0x952A, 12);
				set_b(0x032F, 0x02, 0x9530);
				rts();
				return;
			}
			tick(0x9532, 12);                          // BEQ
			fl::cmp(T.r[0], 0x0080);
			tick(0x9536, 18);
			if (T.r[0] != 0x0080)
			{
				tick(0x9538, 12);                      // BEQ not taken
				rts();
				return;
			}
			tick(0x953A, 12);
			set_b(0x032F, 0x01, 0x9540);
			rts();
			return;
		}
		case 0x9542:                                   // mode 1: at an edge
		{
			fl::tst(T.r[0]);
			tick(0x9544, 12);
			if (!T.r[0])
			{
				tick(0x9546, 12);                      // BNE not taken
				tick(0x9718, 18);                      // JMP
				at = 0x9718;
				break;
			}
			tick(0x954A, 12);
			const bool b6 = fl::logic((uint16_t)(T.r[0] & 0x0040)) != 0;
			tick(0x954E, 18);
			if (!b6)
			{
				tick(0x9538, 12);                      // BEQ
				rts();
				return;
			}
			tick(0x9550, 12);
			set_b(0x0329, 0xFF, 0x9556);
			const uint8_t last = peekb(0x0328);
			fl::cmpb(last, 0xFF);
			tick(0x955C, 30);
			if (last == 0xFF)
			{
				tick(0x9538, 12);                      // BEQ
				rts();
				return;
			}
			tick(0x955E, 12);
			const uint8_t ramp = peekb(0x01EA);
			fl::tstb(ramp);
			tick(0x9562, 24);
			if (ramp)
			{
				tick(0x9564, 12);
				tick(0x96EA, 18);                      // JMP
				at = 0x96EA;
				break;
			}
			tick(0x9568, 12);                          // BEQ: the cell beyond the edge
			copy_w(0x0214, 0x0220, 0x956E);
			T.r[0] = peek(0x06B2);
			fl::mov(T.r[0]);
			tick(0x9572, 24);
			T.r[0] = fl::sub(T.r[0], peek(0x06B0));
			tick(0x9576, 24);
			T.r[0] = fl::add(T.r[0], peek(0x021C));
			tick(0x957A, 24);
			T.r[0] = fl::asr(T.r[0]);
			tick(0x957C, 12);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x3F00));
			tick(0x9580, 18);
			T.r[1] = 0x3E00;
			fl::mov(T.r[1]);
			tick(0x9584, 18);
			T.r[1] = fl::sub(T.r[1], T.r[0]);
			tick(0x9586, 12);
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0x9588, 12);
			jsr(0x958C, 0x8078);                       // select_video_bank
			poke(0x0218, T.r[1]);
			fl::mov(T.r[1]);
			tick(0x9590, 27);
			T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x1F00));
			tick(0x9594, 18);
			poke(0x021E, T.r[1]);
			fl::mov(T.r[1]);
			tick(0x9598, 27);
			T.r[0] = 0;
			fl::clr();
			tick(0x959A, 12);
			T.r[0] = fl::logicb(peekb(0x0221));
			tick(0x959E, 24);
			T.r[1] = fl::add(T.r[1], T.r[0]);
			tick(0x95A0, 12);
			T.r[1] = peek((uint16_t)(0x2000 + T.r[1]));   // its tile
			fl::mov(T.r[1]);
			tick(0x95A4, 27);
			const bool second = fl::logic((uint16_t)(T.r[1] & 0x0400)) != 0;
			tick(0x95A8, 18);
			if (!second)
			{
				tick(0x95B4, 12);                      // BEQ
				T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x03FF));
				tick(0x95B8, 18);
				T.r[1] = fl::add(T.r[1], peek(0x01D6));
				tick(0x95BC, 24);
			}
			else
			{
				tick(0x95AA, 12);
				T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x03FF));
				tick(0x95AE, 18);
				T.r[1] = fl::add(T.r[1], peek(0x01D8));
				tick(0x95B2, 24);
				tick(0x95BC, 12);                      // BR
			}
			T.r[3] = sext8(peekb((uint16_t)(0x6000 + T.r[2])));   // this cell's attribute
			fl::movb((uint8_t)T.r[3]);
			tick(0x95C0, 27);
			const uint8_t beyond = peekb((uint16_t)(0x6000 + T.r[1]));
			fl::tstb(beyond);
			tick(0x95C4, 27);
			bool solid;
			if ((int8_t)beyond < 0)
			{
				tick(0x95D4, 12);                      // BLT
				solid = true;
			}
			else
			{
				tick(0x95C6, 12);
				const bool b6x = fl::logicb((uint8_t)(T.r[3] & 0x40)) != 0;
				tick(0x95CA, 18);
				if (!b6x)
				{
					tick(0x9668, 12);                  // BEQ
					solid = false;
				}
				else
				{
					tick(0x95CC, 12);                  // bit 6 with a corner code (no low bits)
					T.r[0] = 0x0007;
					fl::mov(T.r[0]);
					tick(0x95D0, 18);
					T.r[0] = (uint16_t)((T.r[0] & 0xFF00) | fl::logicb((uint8_t)(T.r[0] & ~T.r[3])));
					tick(0x95D2, 12);                  // BICB R3,R0
					solid = (T.r[0] & 0xFF) == 0;
					tick(solid ? 0x95D4 : 0x9668, 12); // BNE
				}
			}
			at = solid ? 0x95D4 : 0x9668;
			break;
		}
		case 0x95D4:                                   // a solid cell: he bounces back
		{
			const bool b6 = fl::logic((uint16_t)(T.r[3] & 0x0040)) != 0;
			tick(0x95D8, 18);
			if (!b6)
				tick(0x9612, 12);                      // BEQ
			else
			{
				tick(0x95DA, 12);
				bank_a(0x95DA, 0x1800);
				bank_b(0x95E6, 0x1C00);
				push(T.r[3]);
				fl::mov(T.r[3]);
				tick(0x95F4, 24);
				jsr(0x95F8, 0x7B3E);
				bank_a(0x95F8, 0x0800);
				bank_b(0x9604, 0x0C00);
				drop(0x9610);
			}
			T.r[1] = 0x0008;                           // the direction reversed
			fl::mov(T.r[1]);
			tick(0x9616, 18);
			T.r[0] = sext8(peekb(0x031D));
			fl::movb((uint8_t)T.r[0]);
			tick(0x961A, 24);
			T.r[1] = fl::logic((uint16_t)(T.r[1] ^ T.r[0]));
			tick(0x961C, 12);
			pokeb(0x031D, (uint8_t)T.r[1]);
			fl::movb((uint8_t)T.r[1]);
			tick(0x9620, 27);
			set_w(0x01B4, 0x0240, 0x9626);             // speed 0x240
			T.r[0] = sext8(peekb(0x0327));
			fl::movb((uint8_t)T.r[0]);
			tick(0x962A, 24);
			poke(0x021A, T.r[0]);
			fl::mov(T.r[0]);
			tick(0x962E, 27);
			clr_b(0x0327, 0x9632);
			jsr(0x9636, 0x9B82);                       // skater_motion_params
			jsr(0x963A, 0x91A6);                       // skater_velocity
			{
				const uint8_t v = peekb(0x021A);
				fl::movb(v);
				pokeb(0x0327, v);
			}
			tick(0x9640, 39);
			jsr(0x9644, 0x9B82);
			const uint16_t vz = peek(0x05B2);
			fl::tst(vz);
			tick(0x9648, 24);
			if ((int16_t)vz <= 0)
				tick(0x964E, 12);                      // BLE
			else
			{
				tick(0x964A, 12);
				clr_w(0x05B2, 0x964E);
			}
			set_w(0x0222, 0x0001, 0x9654);
			set_b(0x031E, 0x02, 0x965A);
			set_b(0x031C, 0x06, 0x9660);
			{
				const uint8_t v = peekb(0x0329);
				fl::movb(v);
				pokeb(0x0328, v);
			}
			tick(0x9666, 39);
			tick(0x96CA, 12);                          // BR
			at = 0x96CA;
			break;
		}
		case 0x9668:                                   // mode 2; a negative attribute: down
		{
			set_b(0x032F, 0x02, 0x966E);
			fl::tst(T.r[3]);
			tick(0x9670, 12);
			if ((int16_t)T.r[3] >= 0)
			{
				tick(0x96CA, 12);                      // BGE
				at = 0x96CA;
				break;
			}
			tick(0x9672, 12);
			set_w(0x0212, 0x0100, 0x9678);
			copy_w(0x06B2, 0x06B0, 0x967E);
			push(0x7FFF);
			fl::mov(0x7FFF);
			tick(0x9682, 30);
			T.r[0] = peek(0x021E);
			fl::mov(T.r[0]);
			tick(0x9686, 24);
			T.r[1] = peek(0x0220);
			fl::mov(T.r[1]);
			tick(0x968A, 24);
			jsr(0x968E, 0x9C5E);                       // trace_cells(0x7FFF): the ground below
			T.r[2] = T.r[0];
			fl::mov(T.r[2]);
			tick(0x9690, 12);
			const uint16_t found = peek(0x0222);
			fl::cmp(found, 0x7FFF);
			tick(0x9696, 30);
			if (found != 0x7FFF)
			{
				tick(0x9698, 12);
				poke(0x0304, T.r[2]);
				fl::mov(T.r[2]);
				tick(0x969C, 27);
				copy_w(0x0222, 0x06B0, 0x96A2);
				set_w(0x0222, 0xFFFF, 0x96A8);
				tick(0x96B6, 12);                      // BR
			}
			else
			{
				tick(0x96AA, 12);                      // BEQ: none, down by 0x200
				poke(0x06B0, fl::sub(peek(0x06B0), 0x0200));
				tick(0x96B0, 33);
				set_w(0x0222, 0x0001, 0x96B6);
			}
			set_b(0x031E, 0x02, 0x96BC);
			set_b(0x031C, 0x06, 0x96C2);
			{
				const uint8_t v = peekb(0x0329);
				fl::movb(v);
				pokeb(0x0328, v);
			}
			tick(0x96C8, 39);
			drop(0x96C8);
			at = 0x96CA;
			break;
		}
		case 0x96CA:                                   // the motion values, the vertical velocity
			jsr(0x96CE, 0x9B82);                       // skater_motion_params
			push_w(0x01B4, 0x96D2);
			push_w(0x02EC, 0x96D6);
			jsr(0x96DA, 0x80FA);                       // mul32
			{
				const uint8_t v = peekb(0x0205);
				fl::movb(v);
				pokeb(0x07F8, v);
			}
			tick(0x96E0, 39);
			copy_w(0x0206, 0x05B0, 0x96E6);
			drop2(0x96E6);
			rts();
			return;
		case 0x96EA:                                   // on the ramp: a wall in the direction of travel
		{
			T.r[0] = peek(0x01B4);
			fl::mov(T.r[0]);
			tick(0x96EE, 24);
			bool check1 = false;
			if ((int16_t)T.r[0] < 0)
			{
				tick(0x96FC, 12);                      // BLT: backwards, cell 1
				check1 = true;
			}
			else
			{
				tick(0x96F0, 12);                      // forwards, cell 3
				const bool wall = fl::logic((uint16_t)(peek(0x0306) & 0x4000)) != 0;
				tick(0x96F6, 30);
				if (wall)
					tick(0x9704, 12);                  // BNE
				else
				{
					tick(0x96F8, 12);
					fl::tst(T.r[0]);
					tick(0x96FA, 12);
					if ((int16_t)T.r[0] >= 0)
					{
						tick(0x9798, 12);              // BGE
						rts();
						return;
					}
					tick(0x96FC, 12);
					check1 = true;
				}
			}
			if (check1)
			{
				const bool wall = fl::logic((uint16_t)(peek(0x0302) & 0x4000)) != 0;
				tick(0x9702, 30);
				if (!wall)
				{
					tick(0x9798, 12);                  // BEQ
					rts();
					return;
				}
				tick(0x9704, 12);
			}
			push(0x003F);
			fl::mov(0x003F);
			tick(0x9708, 30);
			jsr(0x970C, 0x9EEE);                       // skater_deflect(0x3F)
			set_w(0x0222, 0x0001, 0x9712);
			tick(0x9782, 12);                          // BR
			at = 0x9782;
			break;
		}
		case 0x9714:                                   // mode 2: on a level above
			fl::tst(T.r[0]);
			tick(0x9716, 12);
			if (T.r[0])
			{
				tick(0x971E, 12);                      // BNE
				at = 0x971E;
				break;
			}
			tick(0x9718, 12);
			at = 0x9718;
			break;
		case 0x9718:                                   // nothing under him: mode 0
			clr_b(0x032F, 0x971C);
			rts();
			return;
		case 0x971E:
		{
			const uint8_t attr = peekb((uint16_t)(0x6000 + T.r[2]));
			fl::tstb(attr);
			tick(0x9722, 27);
			if ((int8_t)attr < 0)
			{
				tick(0x9798, 12);                      // BLT
				rts();
				return;
			}
			tick(0x9724, 12);
			fl::tstb((uint8_t)T.r[0]);
			tick(0x9726, 12);
			if (!(T.r[0] & 0x80))
			{
				tick(0x9798, 12);                      // BPL
				rts();
				return;
			}
			tick(0x9728, 12);                          // a lone bit 7: back up onto the level
			set_b(0x0329, 0xFF, 0x972E);
			const uint8_t last = peekb(0x0328);
			fl::cmpb(0xFF, last);
			tick(0x9734, 30);
			if (last == 0xFF)
			{
				tick(0x9798, 12);                      // BEQ
				rts();
				return;
			}
			tick(0x9736, 12);
			set_b(0x032F, 0x01, 0x973C);
			set_w(0x0212, 0x0100, 0x9742);
			push(0x7FFE);
			fl::mov(0x7FFE);
			tick(0x9746, 30);
			T.r[0] = peek(0x021E);
			fl::mov(T.r[0]);
			tick(0x974A, 24);
			T.r[1] = peek(0x0220);
			fl::mov(T.r[1]);
			tick(0x974E, 24);
			jsr(0x9752, 0x9C5E);                       // trace_cells(0x7FFE)
			T.r[2] = T.r[0];
			fl::mov(T.r[2]);
			tick(0x9754, 12);
			T.r[1] = peek(0x0222);
			fl::mov(T.r[1]);
			tick(0x9758, 24);
			fl::cmp(T.r[1], 0x7FFF);
			tick(0x975C, 18);
			bool up = false;
			if (T.r[1] == 0x7FFF)
				tick(0x978A, 12);                      // BEQ
			else
			{
				tick(0x975E, 12);
				T.r[0] = peek(0x06B2);
				fl::mov(T.r[0]);
				tick(0x9762, 24);
				T.r[0] = fl::sub(T.r[0], T.r[1]);
				tick(0x9764, 12);
				T.psw = (uint8_t)(T.psw & ~(CF | VF));
				tick(0x9766, 18);                      // CCC CV
				up = (int16_t)T.r[0] > 0;
				tick(up ? 0x9768 : 0x978A, 12);        // BLE: below object 1 only
			}
			if (up)
			{
				poke(0x0304, T.r[2]);
				fl::mov(T.r[2]);
				tick(0x976C, 27);
				poke(0x06B0, T.r[1]);
				fl::mov(T.r[1]);
				tick(0x9770, 27);
				set_w(0x0222, 0xFFFF, 0x9776);
				set_b(0x031E, 0x02, 0x977C);
				set_b(0x031C, 0x06, 0x9782);
				at = 0x9782;
				break;
			}
			set_b(0x032F, 0x02, 0x9790);
			set_w(0x0222, 0x0001, 0x9796);
			at = 0x9796;
			break;
		}
		case 0x9782:
			{
				const uint8_t v = peekb(0x0329);
				fl::movb(v);
				pokeb(0x0328, v);
			}
			tick(0x9788, 39);
			tick(0x9796, 12);                          // BR
			at = 0x9796;
			break;
		case 0x9796:
			drop(0x9796);
			rts();
			return;
		default:                                       // modes beyond the table
			leave_to(at);
		}
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
	bank_a(0x979A, 0x0800);
	bank_b(0x97A6, 0x0C00);
	{
		const uint16_t handled = peek(0x0222);         // an edge was handled
		fl::tst(handled);
		tick(0x97B6, 24);
		if (handled)
		{
			tick(0x97B8, 12);                          // BEQ not taken
			rts();
			return;
		}
		tick(0x97BA, 12);
	}
	// the probed cell ahead: 1 with a corner code, else 2 or 0 by the direction of travel
	set_w(0x021C, 0xFFFF, 0x97C0);
	T.r[0] = 0x0007;
	fl::mov(T.r[0]);
	tick(0x97C4, 18);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & ~peek(0x0304)));
	tick(0x97C8, 24);
	if (!T.r[0])
	{
		tick(0x97CA, 12);
		set_w(0x021C, 0x0001, 0x97D0);
		tick(0x97F8, 12);                              // BR
	}
	else
	{
		tick(0x97D2, 12);                              // BNE
		const uint16_t speed = peek(0x01B4);
		fl::tst(speed);
		tick(0x97D6, 24);
		if ((int16_t)speed <= 0)
		{
			tick(0x97EA, 12);                          // BLE
			T.r[0] = 0x0007;
			fl::mov(T.r[0]);
			tick(0x97EE, 18);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & ~peek(0x0302)));
			tick(0x97F2, 24);
			if (T.r[0])
				tick(0x97F8, 12);                      // BNE
			else
			{
				tick(0x97F4, 12);
				clr_w(0x021C, 0x97F8);
			}
		}
		else
		{
			tick(0x97D8, 12);
			T.r[0] = 0x0007;
			fl::mov(T.r[0]);
			tick(0x97DC, 18);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & ~peek(0x0306)));
			tick(0x97E0, 24);
			if (T.r[0])
				tick(0x97F8, 12);                      // BNE
			else
			{
				tick(0x97E2, 12);
				set_w(0x021C, 0x0002, 0x97E8);
				tick(0x97F8, 12);                      // BR
			}
		}
	}
	T.r[0] = peek(0x021C);
	fl::mov(T.r[0]);
	tick(0x97FC, 24);
	if ((int16_t)T.r[0] < 0)
	{
		tick(0x97B8, 12);                              // BLT: none
		rts();
		return;
	}
	tick(0x97FE, 12);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x9800, 12);
	{
		const uint8_t v = peekb((uint16_t)(0x0302 + T.r[0]));   // its code, once per new code
		fl::movb(v);
		pokeb(0x0329, v);
	}
	tick(0x9806, 42);
	{
		const uint8_t code = peekb((uint16_t)(0x0302 + T.r[0]));
		const uint8_t last = peekb(0x0328);
		fl::cmpb(code, last);
		tick(0x980C, 39);
		if (code == last)
		{
			tick(0x97B8, 12);                          // BEQ
			rts();
			return;
		}
	}
	tick(0x980E, 12);
	T.r[3] = sext8(peekb((uint16_t)(0x0302 + T.r[0])));
	fl::movb((uint8_t)T.r[3]);
	tick(0x9812, 27);
	T.r[1] = 0;
	fl::clr();
	tick(0x9814, 12);
	fl::cmp(T.r[3], 0x0037);
	tick(0x9818, 18);
	if (T.r[3] == 0x0037)
	{
		tick(0x9828, 12);                              // BEQ
		T.r[2] = 0xFFFF;
		fl::mov(T.r[2]);
		tick(0x982C, 18);
	}
	else
	{
		tick(0x981A, 12);
		T.r[1] = fl::inc(T.r[1]);
		tick(0x981C, 12);
		push(T.r[3]);
		fl::mov(T.r[3]);
		tick(0x981E, 24);
		jsr(0x9822, 0x9E52);                           // slope_table_value
		T.r[2] = T.r[0];
		fl::mov(T.r[2]);
		tick(0x9824, 12);
		drop(0x9824);
		tick(0x982C, 12);                              // BR
	}
	fl::tst(T.r[2]);
	tick(0x982E, 12);
	uint16_t at;
	if ((int16_t)T.r[2] < 0)
	{
		tick(0x9830, 12);                              // BGE not taken: a step
		tick(0x9968, 18);                              // JMP
		at = 0x9968;
	}
	else
	{
		tick(0x9834, 12);                              // a wall
		at = 0x9834;
	}
	for (;;)
	{
		switch (at)
		{
		case 0x9834:
		{
			T.r[0] = peek(0x021C);
			fl::mov(T.r[0]);
			tick(0x9838, 24);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x983A, 12);
			static const struct { uint16_t from, to, next; } cell[3] = {
				{0x0308, 0x021E, 0x9840}, {0x030E, 0x0220, 0x9846}, {0x0314, 0x0218, 0x984C},
			};
			for (const auto &c : cell)
			{
				const uint16_t v = peek((uint16_t)(c.from + T.r[0]));
				fl::mov(v);
				poke(c.to, v);
				tick(c.next, 42);
			}
			T.r[0] = peek((uint16_t)(0x0314 + T.r[0]));
			fl::mov(T.r[0]);
			tick(0x9850, 27);
			jsr(0x9854, 0x8078);                       // select_video_bank
			const uint8_t ramp = peekb(0x01EA);
			fl::tstb(ramp);
			tick(0x9858, 24);
			if (ramp)
			{
				tick(0x98F0, 12);                      // BNE
				at = 0x98F0;
				break;
			}
			tick(0x985A, 12);
			const bool b6 = fl::logic((uint16_t)(T.r[3] & 0x0040)) != 0;
			tick(0x985E, 18);
			if (b6)
			{
				tick(0x9860, 12);                      // B1:7B3E walls always rise above him
				bank_a(0x9860, 0x1800);
				bank_b(0x986C, 0x1C00);
				push(T.r[3]);
				fl::mov(T.r[3]);
				tick(0x987A, 24);
				jsr(0x987E, 0x7B3E);
				bank_a(0x987E, 0x0800);
				bank_b(0x988A, 0x0C00);
				T.r[3] = 0x8000;
				fl::mov(T.r[3]);
				tick(0x989A, 18);
				tick(0x98BA, 12);                      // BR
			}
			else
			{
				tick(0x989C, 12);                      // BEQ: F:9C5E with his height
				T.r[3] = peek(0x06B2);
				fl::mov(T.r[3]);
				tick(0x98A0, 24);
				T.r[3] = fl::add(T.r[3], peek(0x05B2));
				tick(0x98A4, 24);
				set_w(0x0212, 0xFF00, 0x98AA);
				push(T.r[3]);
				fl::mov(T.r[3]);
				tick(0x98AC, 24);
				T.r[0] = peek(0x021E);
				fl::mov(T.r[0]);
				tick(0x98B0, 24);
				T.r[1] = peek(0x0220);
				fl::mov(T.r[1]);
				tick(0x98B4, 24);
				jsr(0x98B8, 0x9C5E);                   // trace_cells
				T.r[3] = T.r[0];
				fl::mov(T.r[3]);
				tick(0x98BA, 12);
			}
			fl::cmp(T.r[3], 0x8000);
			tick(0x98BE, 18);
			if (T.r[3] == 0x8000)
			{
				tick(0x98C0, 12);                      // above him: he bounces
				poke(T.r[6], T.r[2]);
				fl::mov(T.r[2]);
				tick(0x98C2, 21);
				jsr(0x98C6, 0x9E96);                   // skater_bounce
				at = 0x98C6;
				break;
			}
			tick(0x98D0, 12);                          // BNE
			const uint16_t which = peek(0x021C);
			fl::cmp(which, 0x0001);
			tick(0x98D6, 30);
			if (which != 1)
			{
				tick(0x98CC, 12);                      // BNE
				at = 0x98CC;
				break;
			}
			tick(0x98D8, 12);                          // from cell 1: onto it
			poke(0x0304, T.r[3]);
			fl::mov(T.r[3]);
			tick(0x98DC, 27);
			copy_w(0x0222, 0x06B0, 0x98E2);
			set_b(0x031E, 0x02, 0x98E8);
			set_b(0x031C, 0x06, 0x98EE);
			tick(0x98C6, 12);                          // BR
			at = 0x98C6;
			break;
		}
		case 0x98C6:
			keep_code(0x98C6);
			at = 0x98CC;
			break;
		case 0x98CC:
			tick(0x9A2E, 18);                          // JMP
			at = 0x9A2E;
			break;
		case 0x98F0:                                   // on the ramp
		{
			T.r[0] = sext8(peekb(0x0327));
			fl::movb((uint8_t)T.r[0]);
			tick(0x98F4, 24);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0007));
			tick(0x98F8, 18);
			fl::cmp(T.r[0], 0x0005);
			tick(0x98FC, 18);
			if (T.r[0] < 5)
			{
				tick(0x9960, 12);                      // BCS
				at = 0x9960;
				break;
			}
			tick(0x98FE, 12);
			T.r[1] = sext8(peekb(0x0327));
			fl::movb((uint8_t)T.r[1]);
			tick(0x9902, 24);
			T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x0038));
			tick(0x9906, 18);
			T.r[0] = T.r[3];
			fl::mov(T.r[0]);
			tick(0x9908, 12);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0038));
			tick(0x990C, 18);
			fl::cmp(T.r[1], T.r[0]);
			tick(0x990E, 12);
			if (T.r[1] != T.r[0])
			{
				tick(0x9960, 12);                      // BNE
				at = 0x9960;
				break;
			}
			tick(0x9910, 12);
			T.r[0] = 0x0007;
			fl::mov(T.r[0]);
			tick(0x9914, 18);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & ~peek(0x0304)));
			tick(0x9918, 24);
			if (T.r[0])
			{
				tick(0x991A, 12);                      // BEQ not taken
				rts();
				return;
			}
			tick(0x991C, 12);                          // a matching corner takes him off
			pokeb(0x0327, (uint8_t)T.r[3]);
			fl::movb((uint8_t)T.r[3]);
			tick(0x9920, 27);
			jsr(0x9924, 0x9FB4);                       // skater_take_off
			set_b(0x031E, 0x02, 0x992A);
			set_b(0x031C, 0x02, 0x9930);
			keep_code(0x9930);
			bank_a(0x9936, 0x1800);
			jsr(0x9946, 0x5264);                       // start_trick
			bank_a(0x9946, 0x0800);
			bank_b(0x9952, 0x2C00);
			rts();
			return;
		}
		case 0x9960:                                   // deflected
			push(T.r[2]);
			fl::mov(T.r[2]);
			tick(0x9962, 24);
			jsr(0x9966, 0x9EEE);                       // skater_deflect
			tick(0x98C6, 12);                          // BR
			at = 0x98C6;
			break;
		case 0x9968:                                   // a step
		{
			T.r[1] = fl::ror(T.r[1]);
			tick(0x996A, 12);
			bool test = true;                          // TST R2 at 0x99AA
			if (C())
			{
				tick(0x996C, 12);                      // going up it: the height in the direction
				bank_a(0x996C, 0x1800);
				push_w(0x0304, 0x997C);
				jsr(0x9980, 0x51EE);                   // slope_height
				T.r[2] = T.r[0];
				fl::mov(T.r[2]);
				tick(0x9982, 12);
				bank_a(0x9982, 0x0800);
				const uint16_t speed = peek(0x01B4);
				fl::tst(speed);
				tick(0x9992, 24);
				if ((int16_t)speed >= 0)
					tick(0x9996, 12);                  // BGE
				else
				{
					tick(0x9994, 12);
					T.r[2] = fl::neg(T.r[2]);
					tick(0x9996, 12);
				}
				drop(0x9996);
				tick(0x99AA, 12);                      // BR
			}
			else
			{
				tick(0x999A, 12);                      // BCC
				const uint16_t which = peek(0x021C);
				fl::cmp(which, 0x0001);
				tick(0x99A0, 30);
				if (which == 1)
				{
					tick(0x99A2, 12);
					T.r[2] = 0x0001;
					fl::mov(T.r[2]);
					tick(0x99A6, 18);
					tick(0x99AC, 12);                  // BR
					test = false;
				}
				else
				{
					tick(0x99A8, 12);                  // BNE
					T.r[2] = 0;
					fl::clr();
					tick(0x99AA, 12);
				}
			}
			if (test)
			{
				fl::tst(T.r[2]);
				tick(0x99AC, 12);
			}
			if ((int16_t)T.r[2] <= 0)
			{
				tick(0x9A30, 12);                      // BLE
				rts();
				return;
			}
			tick(0x99AE, 12);                          // the next level (F:9C5E mode 0x7FFD)
			set_w(0x0212, 0x0100, 0x99B4);
			copy_w(0x0316, 0x0218, 0x99BA);
			push(0x7FFD);
			fl::mov(0x7FFD);
			tick(0x99BE, 30);
			T.r[0] = peek(0x030A);
			fl::mov(T.r[0]);
			tick(0x99C2, 24);
			T.r[1] = peek(0x0310);
			fl::mov(T.r[1]);
			tick(0x99C6, 24);
			jsr(0x99CA, 0x9C5E);                       // trace_cells
			poke(0x0304, T.r[0]);
			fl::mov(T.r[0]);
			tick(0x99CE, 27);
			T.r[0] = peek(0x06B2);
			fl::mov(T.r[0]);
			tick(0x99D2, 24);
			T.r[0] = fl::sub(T.r[0], peek(0x0222));
			tick(0x99D6, 24);
			fl::cmp(T.r[0], 0x0200);
			tick(0x99DA, 18);
			if ((int16_t)T.r[0] < 0x0200)
			{
				tick(0x99E4, 12);                      // BLT
				copy_w(0x06B2, 0x06B0, 0x99EA);
			}
			else
			{
				tick(0x99DC, 12);                      // up to 0x200 below object 1
				copy_w(0x0222, 0x06B0, 0x99E2);
				tick(0x99EA, 12);                      // BR
			}
			const uint8_t level = peekb(0x0F59);
			fl::cmpb(level, 0x04);
			tick(0x99F0, 30);
			if (level != 4)
				tick(0x9A1C, 12);                      // BNE
			else
			{
				tick(0x99F2, 12);
				T.r[0] = peek(0x06B2);
				fl::mov(T.r[0]);
				tick(0x99F6, 24);
				T.r[0] = fl::sub(T.r[0], peek(0x06B0));
				tick(0x99FA, 24);
				fl::cmp(T.r[0], 0x2800);
				tick(0x99FE, 18);
				if ((int16_t)T.r[0] <= 0x2800)
					tick(0x9A1C, 12);                  // BLE
				else
				{
					tick(0x9A00, 12);
					bank_a(0x9A00, 0x3800);
					bank_b(0x9A0C, 0x3C00);
					jsr(0x9A1C, 0x7A9E);
				}
			}
			keep_code(0x9A1C);
			set_b(0x031C, 0x02, 0x9A28);
			set_b(0x031E, 0x02, 0x9A2E);
			at = 0x9A2E;
			break;
		}
		case 0x9A2E:
			drop(0x9A2E);
			rts();
			return;
		default:
			leave_to(at);
		}
	}
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
	c_save(4, 0x9C62);
	T.r[6] = (uint16_t)(T.r[6] - 2);                   // the slot of the position
	fl::tst(peek(T.r[6]));
	tick(0x9C64, 21);
	push(T.r[1]);                                      // the column
	fl::mov(T.r[1]);
	tick(0x9C66, 24);
	const uint16_t sp = T.r[6];
	poke((uint16_t)(sp + 2), T.r[0]);
	fl::mov(T.r[0]);
	tick(0x9C6A, 30);
	T.r[0] = peek(0x0218);
	fl::mov(T.r[0]);
	tick(0x9C6E, 24);
	jsr(0x9C72, 0x8078);                               // select_video_bank
	T.r[2] = peek(0x06B0);                             // the height, from the z of the skater
	fl::mov(T.r[2]);
	tick(0x9C76, 24);
	T.r[0] = peek(0x0212);
	fl::mov(T.r[0]);
	tick(0x9C7A, 24);
	T.r[0] = fl::neg(T.r[0]);
	tick(0x9C7C, 12);
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0x9C7E, 12);
	T.r[1] = fl::asl(T.r[1]);
	tick(0x9C80, 12);
	const uint8_t park = peekb(0x0F59);
	fl::tstb(park);
	tick(0x9C84, 24);
	if (park)
	{
		tick(0x9C8C, 12);                              // BNE
		T.r[4] = 0x001E;
		fl::mov(T.r[4]);
		tick(0x9C90, 18);
	}
	else
	{
		tick(0x9C86, 12);
		T.r[4] = 0x000A;
		fl::mov(T.r[4]);
		tick(0x9C8A, 18);
		tick(0x9C90, 12);                              // BR
	}
	T.r[5] = peek((uint16_t)(sp + 0x12));              // the height or mode
	fl::mov(T.r[5]);
	tick(0x9C94, 27);
	// BIT #0x0400,R3 at pc: tile word R3 -> tile number (bank base 0x01D8 for bit 10, else
	// 0x01D6), on to pc + 0x18
	auto tile = [](uint16_t pc) {
		fl::logic(T.r[3] & 0x0400);
		tick((uint16_t)(pc + 4), 18);
		const bool hi = (T.r[3] & 0x0400) != 0;
		const uint16_t at = (uint16_t)(pc + (hi ? 6 : 0x10));
		tick(at, 12);                                  // BEQ
		T.r[3] = fl::logic((uint16_t)(T.r[3] & 0x03FF));
		tick((uint16_t)(at + 4), 18);
		T.r[3] = fl::add(T.r[3], peek(hi ? 0x01D8 : 0x01D6));
		tick((uint16_t)(at + 8), 24);
		if (hi)
			tick((uint16_t)(pc + 0x18), 12);           // BR
	};
	// MOV R3,R0 at pc: a flat code (bits 0-2 clear, R3 >> 3 not 15) gives its height from the
	// table at 0xFCB0 in R2 (true, at the BR at pc + 0x1C); else on to pc + 0x1E
	auto flat_height = [](uint16_t pc) {
		auto shifted = [](uint16_t at) {
			T.r[0] = T.r[3];
			fl::mov(T.r[0]);
			tick((uint16_t)(at + 2), 12);
			for (int i = 1; i <= 3; i++)
			{
				T.r[0] = fl::asr(T.r[0]);
				tick((uint16_t)(at + 2 + 2 * i), 12);
			}
		};
		shifted(pc);
		fl::cmp(T.r[0], 0x000F);
		tick((uint16_t)(pc + 0x0C), 18);
		if (T.r[0] == 0x000F)
		{
			tick((uint16_t)(pc + 0x1E), 12);           // BEQ
			return false;
		}
		tick((uint16_t)(pc + 0x0E), 12);
		shifted((uint16_t)(pc + 0x0E));
		T.r[0] = fl::asl(T.r[0]);
		tick((uint16_t)(pc + 0x18), 12);
		T.r[2] = peek((uint16_t)(0xFCB0 + T.r[0]));
		fl::mov(T.r[2]);
		tick((uint16_t)(pc + 0x1C), 27);
		return true;
	};
	// BIC #1,R2 at 0x9DC8 and the BR to 0x9DDA
	auto even = []() {
		T.r[2] = fl::logic((uint16_t)(T.r[2] & ~1));
		tick(0x9DCC, 18);
		tick(0x9DDA, 12);
	};
	for (;;)
	{
		fl::cmp(T.r[2], T.r[5]);
		tick(0x9C96, 12);
		if ((int16_t)T.r[2] >= (int16_t)T.r[5])
		{
			tick(0x9C98, 12);                          // the height is reached
			T.r[3] = 0x8000;
			fl::mov(T.r[3]);
			tick(0x9C9C, 18);
			tick(0x9D64, 12);                          // BR
			tick(0x9DDA, 12);                          // BR
			break;
		}
		tick(0x9C9E, 12);                              // BLT
		poke((uint16_t)(sp + 2), fl::add(peek((uint16_t)(sp + 2)), peek(0x0212)));
		tick(0x9CA4, 42);
		T.r[2] = fl::add(T.r[2], T.r[1]);
		tick(0x9CA6, 12);
		fl::logic(peek((uint16_t)(sp + 2)) & 0x2000);
		tick(0x9CAC, 33);
		if (!(peek((uint16_t)(sp + 2)) & 0x2000))
			tick(0x9CC2, 12);                          // BEQ
		else
		{
			tick(0x9CAE, 12);                          // on into the next video window
			poke((uint16_t)(sp + 2), fl::logic((uint16_t)(peek((uint16_t)(sp + 2)) & 0x1F00)));
			tick(0x9CB4, 36);
			poke(0x0218, fl::add(peek(0x0218), 0x2000));
			tick(0x9CBA, 33);
			T.r[0] = peek(0x0218);
			fl::mov(T.r[0]);
			tick(0x9CBE, 24);
			jsr(0x9CC2, 0x8078);                       // select_video_bank
		}
		T.r[0] = peek((uint16_t)(sp + 2));
		fl::mov(T.r[0]);
		tick(0x9CC6, 27);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x1F00));
		tick(0x9CCA, 18);
		push(0);
		fl::clr();
		tick(0x9CCC, 24);
		{
			const uint8_t v = peekb((uint16_t)(T.r[6] + 3));
			fl::movb(v);
			pokeb(T.r[6], v);
		}
		tick(0x9CD0, 36);
		T.r[0] = fl::add(T.r[0], pop());
		tick(0x9CD2, 18);
		T.r[3] = T.r[0];
		fl::mov(T.r[3]);
		tick(0x9CD4, 12);
		T.r[3] = fl::logic((uint16_t)(T.r[3] & ~0xE001));
		tick(0x9CD8, 18);
		T.r[3] = peek((uint16_t)(0x2000 + T.r[3]));    // the tile word
		fl::mov(T.r[3]);
		tick(0x9CDC, 27);
		fl::cmp(T.r[5], 0x7FFE);
		tick(0x9CE0, 18);
		bool found = false;
		if (T.r[5] == 0x7FFE)
		{
			tick(0x9CE2, 12);                          // only cells with bit 14
			fl::logic(T.r[3] & 0x4000);
			tick(0x9CE6, 18);
			if (!(T.r[3] & 0x4000))
				tick(0x9DCE, 12);                      // BEQ
			else
			{
				tick(0x9CE8, 12);
				tile(0x9CE8);
				T.r[3] = sext8(peekb((uint16_t)(0x4000 + T.r[3])));
				fl::movb((uint8_t)T.r[3]);
				tick(0x9D04, 27);
				fl::logic(T.r[3] & 0x0007);
				tick(0x9D08, 18);
				bool flat = false;
				if (T.r[3] & 0x0007)
					tick(0x9D28, 12);                  // BNE
				else
				{
					tick(0x9D0A, 12);
					flat = flat_height(0x9D0A);
				}
				if (flat)
					tick(0x9DDA, 12);                  // BR
				else
				{
					T.r[3] = fl::logic((uint16_t)(T.r[3] & 0x003F));
					tick(0x9D2C, 18);
					T.r[0] = peek(0x06B0);
					fl::mov(T.r[0]);
					tick(0x9D30, 24);
					T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x01FF));
					tick(0x9D34, 18);
					T.r[0] = fl::add(T.r[0], T.r[2]);
					tick(0x9D36, 12);
					T.r[2] = T.r[0];
					fl::mov(T.r[2]);
					tick(0x9D38, 12);
					tick(0x9DC8, 12);                  // BR
					even();
				}
				found = true;
			}
		}
		else
		{
			tick(0x9D3A, 12);                          // BNE
			tile(0x9D3A);
			fl::cmp(T.r[5], 0x7FFF);
			tick(0x9D56, 18);
			if (T.r[5] == 0x7FFF)
			{
				tick(0x9D58, 12);                      // only negative attribute bytes
				T.r[0] = sext8(peekb((uint16_t)(0x6000 + T.r[3])));
				fl::movb((uint8_t)T.r[0]);
				tick(0x9D5C, 27);
				if (!(T.r[0] & 0x8000))
					tick(0x9DCE, 12);                  // BGE
				else
				{
					tick(0x9D5E, 12);
					T.r[3] = T.r[0];
					fl::mov(T.r[3]);
					tick(0x9D60, 12);
					T.r[3] = fl::logic((uint16_t)(T.r[3] & 0x007F));
					tick(0x9D64, 18);
					tick(0x9DDA, 12);                  // BR
					found = true;
				}
			}
			else
			{
				tick(0x9D66, 12);                      // BNE
				const uint8_t depth = peekb(0x032F);
				fl::cmpb(depth, 0x01);
				tick(0x9D6C, 30);
				bool attr = false;
				if ((int8_t)depth <= 1)
					tick(0x9D74, 12);                  // BLE
				else
				{
					tick(0x9D6E, 12);
					const uint8_t a = peekb((uint16_t)(0x6000 + T.r[3]));
					fl::tstb(a);
					tick(0x9D72, 27);
					attr = (a & 0x80) != 0;
					tick(attr ? 0x9D7A : 0x9D74, 12);  // BLT
				}
				if (attr)
				{
					T.r[3] = sext8(peekb((uint16_t)(0x6000 + T.r[3])));
					fl::movb((uint8_t)T.r[3]);
					tick(0x9D7E, 27);
					T.r[3] = fl::logic((uint16_t)(T.r[3] & 0x007F));
					tick(0x9D82, 18);
				}
				else
				{
					T.r[3] = sext8(peekb((uint16_t)(0x4000 + T.r[3])));
					fl::movb((uint8_t)T.r[3]);
					tick(0x9D78, 27);
					tick(0x9D82, 12);                  // BR
				}
				fl::logic(T.r[3] & 0x0007);
				tick(0x9D86, 18);
				if (!(T.r[3] & 0x0007))
					tick(0x9D8C, 12);                  // BEQ
				else
				{
					tick(0x9D88, 12);
					T.r[3] = fl::logic((uint16_t)(T.r[3] & 0x003F));
					tick(0x9D8C, 18);
				}
				T.r[0] = 0x0007;
				fl::mov(T.r[0]);
				tick(0x9D90, 18);
				T.r[0] = fl::logic((uint16_t)(T.r[0] & ~T.r[3]));
				tick(0x9D92, 12);
				bool take = true;
				if (T.r[0])
					tick(0x9D9A, 12);                  // BNE
				else
				{
					tick(0x9D94, 12);
					fl::cmp(T.r[3], 0x003F);
					tick(0x9D98, 18);
					take = T.r[3] == 0x003F;
					tick(take ? 0x9D9A : 0x9DCE, 12);  // BNE
				}
				if (take)
				{
					fl::logic(T.r[3] & 0x0007);
					tick(0x9D9E, 18);
					bool flat = false;
					if (T.r[3] & 0x0007)
						tick(0x9DBE, 12);              // BNE
					else
					{
						tick(0x9DA0, 12);
						flat = flat_height(0x9DA0);
					}
					if (flat)
						tick(0x9DC8, 12);              // BR
					else
					{
						T.r[0] = peek(0x06B0);
						fl::mov(T.r[0]);
						tick(0x9DC2, 24);
						T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x01FF));
						tick(0x9DC6, 18);
						T.r[2] = fl::add(T.r[2], T.r[0]);
						tick(0x9DC8, 12);
					}
					even();
					found = true;
				}
			}
		}
		if (found)
			break;
		T.r[4] = fl::dec(T.r[4]);                      // 0x9DCE: the next cell
		tick(0x9DD0, 12);
		if (!T.r[4])
		{
			tick(0x9DD6, 12);                          // BEQ: none answers
			T.r[2] = 0x7FFF;
			fl::mov(T.r[2]);
			tick(0x9DDA, 18);
			break;
		}
		tick(0x9DD2, 12);
		tick(0x9C94, 18);                              // JMP @#0x9C94
	}
	poke(0x0222, T.r[2]);
	fl::mov(T.r[2]);
	tick(0x9DDE, 27);
	T.r[0] = T.r[3];
	fl::mov(T.r[0]);
	tick(0x9DE0, 12);
	drop2(0x9DE0);
	c_restore(4);
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
	T.r[1] = peek(0x0304);                             // the probed cell's terrain code
	fl::mov(T.r[1]);
	tick(0x9A36, 24);
	const uint8_t ramp = peekb(0x01EA);
	fl::tstb(ramp);
	tick(0x9A3A, 24);
	if (ramp)
	{
		// on a ramp: he takes off when object 1 is above it or the ramp is steep enough
		tick(0x9A3C, 12);
		T.r[0] = peek(0x06B2);
		fl::mov(T.r[0]);
		tick(0x9A40, 24);
		T.r[0] = fl::sub(T.r[0], peek(0x06B0));
		tick(0x9A44, 24);
		T.psw = (uint8_t)(T.psw & ~(CF | VF));
		tick(0x9A46, 18);                              // CCC CV
		if ((int16_t)T.r[0] > 0)
			tick(0x9A9C, 12);                          // BGT
		else
		{
			tick(0x9A48, 12);
			T.r[2] = sext8(peekb(0x0327));
			fl::movb((uint8_t)T.r[2]);
			tick(0x9A4C, 24);
			fl::cmp(T.r[2], T.r[1]);
			tick(0x9A4E, 12);
			if (T.r[2] == T.r[1])
				tick(0x9A9C, 12);                      // BEQ
			else
			{
				tick(0x9A50, 12);
				T.r[2] = 0x0007;
				fl::mov(T.r[2]);
				tick(0x9A54, 18);
				T.r[2] = fl::logic((uint16_t)(T.r[2] & ~T.r[1]));
				tick(0x9A56, 12);
				if (!T.r[2])
					tick(0x9A9C, 12);                  // BEQ: a corner code
				else
				{
					tick(0x9A58, 12);                  // the new slope (bank A 0x1800 by R2)
					T.r[2] = 0x1800;
					fl::mov(T.r[2]);
					tick(0x9A5C, 18);
					poke(0x01D2, T.r[2]);
					fl::mov(T.r[2]);
					tick(0x9A60, 27);
					poke(0x1400, T.r[2]);
					fl::mov(T.r[2]);
					tick(0x9A64, 27);
					push(T.r[1]);
					fl::mov(T.r[1]);
					tick(0x9A66, 24);
					jsr(0x9A6A, 0x51EE);               // slope_height
					T.r[2] = 0x0800;
					fl::mov(T.r[2]);
					tick(0x9A6E, 18);
					poke(0x01D2, T.r[2]);
					fl::mov(T.r[2]);
					tick(0x9A72, 27);
					poke(0x1400, T.r[2]);
					fl::mov(T.r[2]);
					tick(0x9A76, 27);
					T.r[3] = T.r[0];
					fl::mov(T.r[3]);
					tick(0x9A78, 12);
					T.r[2] = peek(0x01B4);
					fl::mov(T.r[2]);
					tick(0x9A7C, 24);
					if ((int16_t)T.r[2] >= 0)
						tick(0x9A80, 12);              // BGE
					else
					{
						tick(0x9A7E, 12);
						T.r[0] = fl::neg(T.r[0]);
						tick(0x9A80, 12);
					}
					fl::tst(T.r[0]);
					tick(0x9A82, 12);
					if ((int16_t)T.r[0] <= 0)
						tick(0x9A9A, 12);              // BLE
					else
					{
						tick(0x9A84, 12);              // steep enough for the speed?
						T.r[3] = fl::swab(T.r[3]);
						tick(0x9A86, 12);
						T.r[3] = sext8((uint8_t)T.r[3]);
						fl::movb((uint8_t)T.r[3]);
						tick(0x9A88, 12);
						poke(T.r[6], T.r[3]);
						fl::mov(T.r[3]);
						tick(0x9A8A, 21);
						push(T.r[2]);
						fl::mov(T.r[2]);
						tick(0x9A8C, 24);
						poke(T.r[6], fl::asr(peek(T.r[6])));
						tick(0x9A8E, 21);
						poke(T.r[6], fl::asr(peek(T.r[6])));
						tick(0x9A90, 21);
						jsr(0x9A94, 0x80D8);           // mul16
						T.r[0] = fl::sub(T.r[0], 0x1200);
						tick(0x9A98, 18);
						drop(0x9A98);
					}
					drop(0x9A9A);
				}
			}
		}
		fl::tst(T.r[0]);
		tick(0x9A9E, 12);
		if ((int16_t)T.r[0] <= 0)
			tick(0x9B08, 12);                          // BLE
		else
		{
			tick(0x9AA0, 12);
			jsr(0x9AA4, 0x9F8A);                       // skater_jump
			tick(0x9B08, 12);                          // BR
		}
	}
	else
	{
		// in the air: close enough above the ground while falling, he lands
		tick(0x9AA6, 12);                              // BEQ
		const uint16_t vz = peek(0x05B2);
		const uint16_t vz0 = peek(0x05B0);
		fl::cmp(vz, vz0);
		tick(0x9AAC, 36);
		if ((int16_t)vz >= (int16_t)vz0)
			tick(0x9B08, 12);                          // BGE
		else
		{
			tick(0x9AAE, 12);
			T.r[0] = peek(0x06B2);
			fl::mov(T.r[0]);
			tick(0x9AB2, 24);
			T.r[0] = fl::add(T.r[0], peek(0x05B2));
			tick(0x9AB6, 24);
			T.r[2] = peek(0x06B0);
			fl::mov(T.r[2]);
			tick(0x9ABA, 24);
			T.r[2] = fl::add(T.r[2], peek(0x05B0));
			tick(0x9ABE, 24);
			T.r[0] = fl::sub(T.r[0], T.r[2]);
			tick(0x9AC0, 12);
			fl::cmp(T.r[0], 0x0040);
			tick(0x9AC4, 18);
			if ((int16_t)T.r[0] >= 0x40)
				tick(0x9B08, 12);                      // BGE
			else
			{
				tick(0x9AC6, 12);
				push(0x0047);
				fl::mov(0x0047);
				tick(0x9ACA, 30);
				jsr(0x9ACE, 0x8E78);                   // queue_sound(0x47)
				poke(T.r[6], 0x004B);
				fl::mov(0x004B);
				tick(0x9AD2, 27);
				jsr(0x9AD6, 0x8E78);                   // queue_sound(0x4B)
				poke(T.r[6], T.r[1]);
				fl::mov(T.r[1]);
				tick(0x9AD8, 21);
				jsr(0x9ADC, 0xA090);                   // skater_touch_down
				bank_a(0x9ADC, 0x3800);
				bank_b(0x9AE8, 0x3C00);
				const uint8_t level = peekb(0x0F59);
				fl::cmpb(level, 0x03);
				tick(0x9AFA, 30);
				if (level == 3)
				{
					tick(0x9B02, 12);                  // BEQ
					jsr(0x9B06, 0x7E98);
				}
				else
				{
					tick(0x9AFC, 12);
					jsr(0x9B00, 0x7ADA);               // score_landing
					tick(0x9B06, 12);                  // BR
				}
				drop(0x9B06);
			}
		}
	}
	// a new terrain is taken over
	T.r[0] = sext8(peekb(0x0327));
	fl::movb((uint8_t)T.r[0]);
	tick(0x9B0C, 24);
	fl::cmp(T.r[0], T.r[1]);
	tick(0x9B0E, 12);
	if (T.r[0] == T.r[1])
	{
		tick(0x9B80, 12);                              // BEQ
		rts();
		return;
	}
	tick(0x9B10, 12);
	T.r[2] = 0x0007;
	fl::mov(T.r[2]);
	tick(0x9B14, 18);
	T.r[2] = fl::logic((uint16_t)(T.r[2] & ~T.r[1]));
	tick(0x9B16, 12);
	if (!T.r[2])
	{
		tick(0x9B80, 12);                              // BEQ: not to a corner code
		rts();
		return;
	}
	tick(0x9B18, 12);
	T.r[2] = sext8(peekb(0x032D));
	fl::movb((uint8_t)T.r[2]);
	tick(0x9B1C, 24);
	fl::cmp(T.r[1], T.r[2]);
	tick(0x9B1E, 12);
	if (T.r[1] == T.r[2])
	{
		tick(0x9B20, 12);                              // the same one again: counted down
		T.r[2] = sext8(peekb(0x032C));
		fl::movb((uint8_t)T.r[2]);
		tick(0x9B24, 24);
		T.r[2] = fl::dec(T.r[2]);
		tick(0x9B26, 12);
		pokeb(0x032C, (uint8_t)T.r[2]);
		fl::movb((uint8_t)T.r[2]);
		tick(0x9B2A, 27);
		tick(0x9B32, 12);                              // BR
	}
	else
	{
		tick(0x9B2C, 12);                              // BNE
		set_b(0x032C, 0x0A, 0x9B32);
	}
	pokeb(0x032D, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x9B36, 27);
	T.r[2] = 0x0007;
	fl::mov(T.r[2]);
	tick(0x9B3A, 18);
	T.r[2] = fl::logic((uint16_t)(T.r[2] & ~T.r[0]));
	tick(0x9B3C, 12);
	if (T.r[2])
		tick(0x9B4E, 12);                              // BNE
	else
	{
		tick(0x9B3E, 12);                              // leaving a corner code
		const bool sloped = fl::logic((uint16_t)(T.r[1] & 0x0007)) != 0;
		tick(0x9B42, 18);
		if (sloped)
			tick(0x9B4E, 12);                          // BNE
		else
		{
			tick(0x9B44, 12);                          // onto the flat: the direction mapped
			T.r[0] = sext8(peekb(0x031D));
			fl::movb((uint8_t)T.r[0]);
			tick(0x9B48, 24);
			{
				const uint8_t v = peekb((uint16_t)(0xFCE0 + T.r[0]));
				fl::movb(v);
				pokeb(0x031D, v);
			}
			tick(0x9B4E, 42);
		}
	}
	pokeb(0x0327, (uint8_t)T.r[1]);
	fl::movb((uint8_t)T.r[1]);
	tick(0x9B52, 27);
	jsr(0x9B56, 0x9B82);                               // skater_motion_params
	const uint8_t ramp2 = peekb(0x01EA);
	fl::tstb(ramp2);
	tick(0x9B5A, 24);
	if (ramp2)
	{
		tick(0x9B80, 12);                              // BNE
		rts();
		return;
	}
	tick(0x9B5C, 12);                                  // in the air: the jump's z velocity
	push_w(0x01B4, 0x9B60);
	push_w(0x02EC, 0x9B64);
	jsr(0x9B68, 0x80FA);                               // mul32
	{
		const uint8_t v = peekb(0x0205);
		fl::movb(v);
		pokeb(0x07F8, v);
	}
	tick(0x9B6E, 39);
	copy_w(0x0206, 0x05B0, 0x9B74);
	const uint8_t carried = peekb(0x0EA0);
	fl::tstb(carried);
	tick(0x9B78, 24);
	if (!carried)
		tick(0x9B7E, 12);                              // BEQ
	else
	{
		tick(0x9B7A, 12);
		jsr(0x9B7E, 0xD6BE);                           // slope_of_object_0ea0
	}
	drop2(0x9B7E);
	rts();
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
	tick(0x8156, 18);                                  // JMP @#0x8156: the power-on sequence
	clr_w(0x1600, 0x815A);                             // interrupts off
	clr_b(0x1800, 0x815E);                             // the watchdog
	clr_w(0x1600, 0x8162);
	T.r[0] = 0x0FFE;
	fl::mov(T.r[0]);
	tick(0x8166, 18);
	for (;;)                                           // RAM 0x0080-0x0FFF cleared
	{
		poke(T.r[0], 0);
		fl::clr();
		tick(0x8168, 21);
		T.r[0] = fl::sub(T.r[0], 0x0002);
		tick(0x816C, 18);
		fl::cmp(T.r[0], 0x0080);
		tick(0x8170, 18);
		if ((int16_t)T.r[0] < 0x0080)
		{
			tick(0x8172, 12);                          // BGE not taken
			break;
		}
		tick(0x8166, 12);
	}
	clr_b(0x1800, 0x8176);
	jsr(0x817A, 0x8D2C);                               // clear_playfield_ram
	jsr(0x817E, 0x8CE8);                               // clear_playfield
	clr_b(0x1800, 0x8182);
	set_b(0x0190, 0x01, 0x8188);
	bank_a(0x8188, 0x3800);
	bank_b(0x8194, 0x3C00);
	jsr(0x81A4, 0x4B00);
	jsr(0x81A8, 0x829E);                               // attract_mode: does not return
	rts();
}

// F:829E (assembly; waits, does not return): the attract mode: display setup (F:BA2E),
// interrupts of the play mode (F:820C) with the pending acknowledges cleared and interrupts
// allowed, game state 4, the palettes loaded by the vertical blank (0x0EB0 = F:DA74, F:DABA,
// F:DC3C, F:DB4E, one frame each), playfield cleared, text table 0x5AC8, no texts pending,
// then the attract main loop F:8714
void attract_mode()
{
	jsr(0x82A2, 0xBA2E);                               // clear_objects_and_text
	jsr(0x82A6, 0x820C);                               // install_interrupts_b
	clr_w(0x15E0, 0x82AA);                             // the pending acknowledges cleared
	clr_w(0x15C0, 0x82AE);
	clr_w(0x1580, 0x82B2);
	T.r[0] = 0;
	fl::clr();
	tick(0x82B4, 12);
	mtps(0);                                           // interrupts allowed
	tick(0x82B8, 30);
	set_b(0x018F, 0x04, 0x82BE);                       // game state 4
	set_b(0x018E, 0x01, 0x82C4);
	set_b(0x0F25, 0x02, 0x82CA);
	clr_b(0x0EA7, 0x82CE);
	set_b(0x0EA8, 0xFF, 0x82D4);
	set_w(0x0EB0, 0xDA74, 0x82DA);                     // the palettes, a frame each
	jsr(0x82DE, 0x8D50);                               // wait_vblank
	set_w(0x0EB0, 0xDABA, 0x82E4);
	jsr(0x82E8, 0x8D50);
	set_w(0x0EB0, 0xDC3C, 0x82EE);
	set_w(0x0EAC, 0x000F, 0x82F4);
	clr_w(0x0EAE, 0x82F8);
	jsr(0x82FC, 0x8D50);
	set_w(0x0EB0, 0xDB4E, 0x8302);
	jsr(0x8306, 0x8D50);
	jsr(0x830A, 0x8CE8);                               // clear_playfield
	set_w(0x0184, 0x5AC8, 0x8310);                     // the text table
	set_w(0x018A, 0x0001, 0x8316);
	set_b(0x0336, 0xFF, 0x831C);
	set_w(0x0EC2, 0xFFFF, 0x8322);
	jsr(0x8326, 0x8714);                               // attract_loop: does not return
	rts();
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
	uint16_t at = 0x8714;
	for (;;)
	{
		switch (at)
		{
		case 0x8714:                                   // a new frame?
		{
			const uint8_t fresh = peekb(0x0192);
			fl::tstb(fresh);
			tick(0x8718, 24);
			if (!fresh)
			{
				tick(0x8726, 12);                      // BEQ
				at = 0x8726;
				break;
			}
			tick(0x871A, 12);
			at = 0x871A;
			break;
		}
		case 0x871A:                                   // yes: phase 0 again, the frame counted
			clr_b(0x0192, 0x871E);
			clr_b(0x02BA, 0x8722);
			poke(0x01C2, fl::inc(peek(0x01C2)));
			tick(0x8726, 27);
			at = 0x8726;
			break;
		case 0x8726:                                   // a strip of the map
			bank_a(0x8726, 0x1800);
			jsr(0x8736, 0x41CE);                       // draw_map_strip
			at = 0x8736;
			break;
		case 0x8736:
		{
			const uint8_t fresh = peekb(0x0192);
			fl::tstb(fresh);
			tick(0x873A, 24);
			if (fresh)
			{
				tick(0x871A, 12);                      // BNE
				at = 0x871A;
				break;
			}
			tick(0x873C, 12);
			const uint8_t phase = peekb(0x02BA);
			fl::cmpb(phase, 0x06);
			tick(0x8742, 30);
			if ((int8_t)phase > 6)
			{
				tick(0x8714, 12);                      // BGT: all done
				at = 0x8714;
				break;
			}
			tick(0x8744, 12);
			T.r[0] = sext8(peekb(0x02BA));
			fl::movb((uint8_t)T.r[0]);
			tick(0x8748, 24);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x874A, 12);
			at = fl::add(0x874E, peek((uint16_t)(0xFC66 + T.r[0])));
			tick(at, 27);                              // ADD 0xFC66(R0),PC: the phase
			break;
		}
		case 0x874E:                                   // phase 0
			bank_a(0x874E, 0x3800);
			bank_b(0x875A, 0x3C00);
			jsr(0x876A, 0x5BB8);                       // game_over_states
			bank_a(0x876A, 0x3800);
			bank_b(0x8776, 0x3C00);
			jsr(0x8786, 0x5248);
			tick(0x8884, 12);                          // BR
			tick(0x8910, 12);                          // BR
			at = 0x8910;
			break;
		case 0x8788:                                   // phase 1: the attract display
		{
			const uint8_t game = peekb(0x0F26);
			fl::cmpb(game, 0x06);
			tick(0x878E, 30);
			bool show;
			if (game == 6)
			{
				tick(0x8798, 12);                      // BEQ
				show = true;
			}
			else
			{
				tick(0x8790, 12);
				show = fl::logic((uint16_t)(peek(0x01C2) & 0x000F)) == 0;
				tick(0x8796, 30);
				tick(show ? 0x8798 : 0x87CA, 12);      // BNE
			}
			if (show)
			{
				bank_a(0x8798, 0x3800);
				bank_b(0x87A4, 0x3C00);
				T.r[0] = sext8(peekb(0x018F));
				fl::movb((uint8_t)T.r[0]);
				tick(0x87B4, 24);
				bool first;
				if (!T.r[0])
				{
					tick(0x87BC, 12);                  // BEQ
					first = true;
				}
				else
				{
					tick(0x87B6, 12);
					fl::cmp(T.r[0], 0x0001);
					tick(0x87BA, 18);
					first = T.r[0] == 1;
					tick(first ? 0x87BC : 0x87C0, 12); // BNE
				}
				if (first)
					jsr(0x87C0, 0x77B4);
				const uint8_t state = peekb(0x018F);
				fl::tstb(state);
				tick(0x87C4, 24);
				if (state)
					tick(0x87CA, 12);                  // BNE
				else
				{
					tick(0x87C6, 12);
					jsr(0x87CA, 0x78BE);
				}
			}
			bank_a(0x87CA, 0x3800);
			bank_b(0x87D6, 0x3C00);
			jsr(0x87E6, 0x79D4);
			tick(0x88B0, 12);                          // BR
			tick(0x8910, 12);                          // BR
			at = 0x8910;
			break;
		}
		case 0x87E8:                                   // phase 2: map slots, messages, B6 screens
		{
			const uint8_t level = peekb(0x0F59);
			fl::cmpb(level, 0x01);
			tick(0x87EE, 30);
			if (level == 1)
			{
				tick(0x87F0, 12);
				jsr(0x87F4, 0xBBEC);                   // check_some_map_slots
				tick(0x881E, 12);                      // BR
			}
			else
			{
				tick(0x87F6, 12);                      // BNE
				const uint8_t slots = peekb(0x0E99);
				fl::tstb(slots);
				tick(0x87FA, 24);
				if (!slots)
					tick(0x881E, 12);                  // BEQ
				else
				{
					tick(0x87FC, 12);
					const uint8_t level2 = peekb(0x0F59);
					fl::tstb(level2);
					tick(0x8800, 24);
					if (level2)
						tick(0x881E, 12);              // BNE
					else
					{
						tick(0x8802, 12);
						bank_a(0x8802, 0x1800);
						bank_b(0x880E, 0x1C00);
						jsr(0x881E, 0x48CC);
					}
				}
			}
			bank_a(0x881E, 0x3800);
			jsr(0x882E, 0x4C28);                       // show_next_message
			const uint8_t busy = peekb(0x018E);
			fl::tstb(busy);
			tick(0x8832, 24);
			if (busy)
			{
				tick(0x8910, 12);                      // BNE
				at = 0x8910;
				break;
			}
			tick(0x8834, 12);
			const uint8_t busy2 = peekb(0x0F36);
			fl::tstb(busy2);
			tick(0x8838, 24);
			if (busy2)
			{
				tick(0x8910, 12);                      // BNE
				at = 0x8910;
				break;
			}
			tick(0x883A, 12);
			bank_b(0x883A, 0x2800);
			jsr(0x884A, 0x7F16);
			jsr(0x884E, 0x7CF6);                       // show_hint
			jsr(0x8852, 0x7C84);
			tick(0x8910, 12);                          // BR
			at = 0x8910;
			break;
		}
		case 0x8854:                                   // phase 3: alternate frames
		{
			const uint8_t on = peekb(0x0197);
			fl::tstb(on);
			tick(0x8858, 24);
			if (!on)
			{
				tick(0x8910, 12);                      // BEQ
				at = 0x8910;
				break;
			}
			tick(0x885A, 12);
			const bool odd = fl::logic((uint16_t)(peek(0x01C2) & 0x0001)) != 0;
			tick(0x8860, 30);
			if (!odd)
			{
				tick(0x8862, 12);
				jsr(0x8866, 0xD35E);                   // spawn_counted_object
				tick(0x8910, 12);                      // BR
			}
			else
			{
				tick(0x8868, 12);                      // BNE
				bank_a(0x8868, 0x1800);
				bank_b(0x8874, 0x1C00);
				jsr(0x8884, 0x5B46);                   // rolling_sound
				tick(0x8910, 12);                      // BR
			}
			at = 0x8910;
			break;
		}
		case 0x8886:                                   // phase 4
			bank_a(0x8886, 0x3800);
			jsr(0x8896, 0x4B84);                       // expire_messages
			tick(0x8910, 12);                          // BR
			at = 0x8910;
			break;
		case 0x8898:                                   // phase 5: every 4th frame
		{
			T.r[0] = peek(0x01C2);
			fl::mov(T.r[0]);
			tick(0x889C, 24);
			T.r[0] = fl::add(T.r[0], 0x0002);
			tick(0x88A0, 18);
			const bool off = fl::logic((uint16_t)(T.r[0] & 0x0003)) != 0;
			tick(0x88A4, 18);
			if (off)
			{
				tick(0x8910, 12);                      // BNE
				at = 0x8910;
				break;
			}
			tick(0x88A6, 12);
			const uint8_t on = peekb(0x0197);
			fl::tstb(on);
			tick(0x88AA, 24);
			if (!on)
			{
				tick(0x8910, 12);                      // BEQ
				at = 0x8910;
				break;
			}
			tick(0x88AC, 12);
			jsr(0x88B0, 0xC8AA);                       // tick_map_object_timers
			tick(0x8910, 12);                          // BR
			at = 0x8910;
			break;
		}
		case 0x88B2:                                   // phase 6: every 64th frame, coins and start
		{
			const bool off = fl::logic((uint16_t)(peek(0x01C2) & 0x003F)) != 0;
			tick(0x88B8, 30);
			if (off)
			{
				tick(0x8910, 12);                      // BNE
				at = 0x8910;
				break;
			}
			tick(0x88BA, 12);
			const bool normal = fl::logic((uint16_t)(peek(0x1800) & 0x8000)) != 0;
			tick(0x88C0, 30);
			if (normal)
				tick(0x88DE, 12);                      // BNE
			else
			{
				tick(0x88C2, 12);                      // the test switch: the self test
				bank_a(0x88C2, 0x3800);
				bank_b(0x88CE, 0x3C00);
				jsr(0x88DE, 0xF372);                   // enter_selftest
			}
			bank_a(0x88DE, 0x3800);
			bank_b(0x88EA, 0x3C00);
			jsr(0x88FA, 0x4B3E);
			const uint8_t state = peekb(0x018F);
			fl::cmpb(state, 0x04);
			tick(0x8900, 30);
			if (state != 4)
				tick(0x8906, 12);                      // BNE
			else
			{
				tick(0x8902, 12);
				jsr(0x8906, 0x435A);
			}
			const uint8_t dead = peekb(0x01A2);        // the sound CPU marked dead
			fl::tstb(dead);
			tick(0x890A, 24);
			if (!dead)
				tick(0x8910, 12);                      // BEQ
			else
			{
				tick(0x890C, 12);
				jsr(0x8910, 0x4B00);
			}
			at = 0x8910;
			break;
		}
		case 0x8910:                                   // the next phase
			T.r[0] = sext8(peekb(0x02BA));
			fl::movb((uint8_t)T.r[0]);
			tick(0x8914, 24);
			T.r[0] = fl::inc(T.r[0]);
			tick(0x8916, 12);
			pokeb(0x02BA, (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0x891A, 27);
			tick(0x8736, 18);                          // JMP
			at = 0x8736;
			break;
		default:
			leave_to(at);
		}
	}
}

// F:8F50 / F:8F62 (assembly; waits for the sound CPU): its answer (F:8ED2) must be 0xAA /
// 0x55, otherwise the sound CPU is marked dead (0x01A2 = 3 / 4)
void expect_answer(uint16_t here, uint8_t want, uint8_t dead)
{
	jsr((uint16_t)(here + 4), 0x8ED2);                 // read_sound_response
	fl::cmp(T.r[0], want);
	tick((uint16_t)(here + 8), 18);
	if (T.r[0] == want)
		tick((uint16_t)(here + 0x10), 12);             // BEQ
	else
	{
		tick((uint16_t)(here + 0x0A), 12);
		set_b(0x01A2, dead, (uint16_t)(here + 0x10));  // the sound CPU marked dead
	}
	rts();
}
void expect_answer_aa() { expect_answer(0x8F50, 0xAA, 3); }
void expect_answer_55() { expect_answer(0x8F62, 0x55, 4); }

// F:8F0C  read_sound_bytes(char *buf, int n) (waits for the sound CPU): n bytes from the sound
// CPU (F:8ED2) as they come (IN0 bit 4), then its closing answer 0x55 (F:8F62); the
// watchdog is fed meanwhile.  Without a byte for 3 x 65536 polls the sound CPU is marked
// dead (0x01A2 = 2); nothing happens while it is.  n on the stack counts down.
uint16_t read_sound_bytes(const Args &)
{
	c_save(2, 0x8F10);
	T.r[1] = peek((uint16_t)(T.r[6] + 0x0A));          // where to
	fl::mov(T.r[1]);
	tick(0x8F14, 27);
	T.r[2] = 0;
	fl::clr();
	tick(0x8F16, 12);
	T.r[3] = 0x0003;
	fl::mov(T.r[3]);
	tick(0x8F1A, 18);
	const uint8_t dead = peekb(0x01A2);
	fl::tstb(dead);
	tick(0x8F1E, 24);
	if (dead)
	{
		tick(0x8F4E, 12);                              // BNE
		c_restore(2);
		return T.r[0];
	}
	tick(0x8F20, 12);
	for (;;)                                           // up to 3 x 65536 tries for each byte
	{
		const bool ready = fl::logic((uint16_t)(peek(0x1800) & 0x0010)) != 0;
		tick(0x8F26, 30);
		if (ready)
		{
			tick(0x8F28, 12);
			jsr(0x8F2C, 0x8ED2);                       // read_sound_response
			pokeb(T.r[1], (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			T.r[1]++;
			tick(0x8F2E, 21);
			const uint16_t left = fl::dec(peek((uint16_t)(T.r[6] + 0x0C)));
			poke((uint16_t)(T.r[6] + 0x0C), left);
			tick(0x8F32, 30);
			if (!left)
			{
				tick(0x8F34, 12);                      // all there
				jsr(0x8F38, 0x8F62);                   // expect_answer_55
				c_restore(2);
				return T.r[0];
			}
			tick(0x8F3A, 12);                          // BNE
			T.r[3] = 0x0003;
			fl::mov(T.r[3]);
			tick(0x8F3E, 18);
			T.r[2] = 0;
			fl::clr();
			tick(0x8F40, 12);
		}
		else
			tick(0x8F40, 12);                          // BEQ
		jsr(0x8F44, 0x917C);                           // feed_watchdog
		if (--T.r[2] != 0)
		{
			tick(0x8F20, 18);                          // SOB R2
			continue;
		}
		tick(0x8F46, 18);
		if (--T.r[3] != 0)
		{
			tick(0x8F20, 18);                          // SOB R3
			continue;
		}
		tick(0x8F48, 18);
		set_b(0x01A2, 0x02, 0x8F4E);                   // no answer: the sound CPU is dead
		c_restore(2);
		return T.r[0];
	}
}

// F:8D86 (assembly; waits for the sound CPU): the controller angle.  With interrupts masked
// the sound CPU is asked (command 0x17, F:8EA2) for 4 bytes into 0x0224 (F:8F0C); the
// negated reading is the step (0x01C8 = |step|, 0x01CA = step).  With auto-centring
// (0x0226) a small step moves the angle 0x01C6 back towards the centre 0x24 (by the offset
// 0x0227; the first time it is set to 0x24); the angle 0x0224 = 0x01C6 + step, kept within
// 0..0x8F, becomes the new 0x01C6.
void read_controller()
{
	T.r[1] = 0x00E0;
	fl::mov(T.r[1]);
	tick(0x8D8A, 18);
	mtps((uint8_t)T.r[1]);                             // interrupts off
	tick(0x8D8C, 24);
	push(0x0017);
	fl::mov(0x0017);
	tick(0x8D90, 30);
	jsr(0x8D94, 0x8EA2);                               // send_sound_command(0x17)
	poke(T.r[6], 0x0004);
	fl::mov(0x0004);
	tick(0x8D98, 27);
	push(0x0224);
	fl::mov(0x0224);
	tick(0x8D9C, 30);
	jsr(0x8DA0, 0x8F0C);                               // read_sound_bytes(0x0224, 4)
	mtps(0);
	tick(0x8DA4, 30);
	poke(0x0224, fl::neg(peek(0x0224)));               // the rotation since the last time
	tick(0x8DA8, 27);
	T.r[1] = peek(0x0224);
	fl::mov(T.r[1]);
	tick(0x8DAC, 24);
	if ((int16_t)T.r[1] < 0)
	{
		tick(0x8DB2, 12);                              // BLT
		T.r[0] = T.r[1];
		fl::mov(T.r[0]);
		tick(0x8DB4, 12);
		T.r[0] = fl::neg(T.r[0]);
		tick(0x8DB6, 12);
	}
	else
	{
		tick(0x8DAE, 12);
		T.r[0] = T.r[1];
		fl::mov(T.r[0]);
		tick(0x8DB0, 12);
		tick(0x8DB6, 12);                              // BR
	}
	poke(0x01C8, T.r[0]);                              // its size and the rotation
	fl::mov(T.r[0]);
	tick(0x8DBA, 27);
	poke(0x01CA, T.r[1]);
	fl::mov(T.r[1]);
	tick(0x8DBE, 27);
	{
		const uint8_t centre = peekb(0x0226);          // the centre sensor seen
		fl::tstb(centre);
		tick(0x8DC2, 24);
		if (!centre)
			tick(0x8E04, 12);                          // BEQ
		else
		{
			tick(0x8DC4, 12);
			fl::cmp(T.r[0], 0x0006);
			tick(0x8DC8, 18);
			if ((int16_t)T.r[0] > 6)
				tick(0x8E04, 12);                      // BGT: too fast to tell
			else
			{
				tick(0x8DCA, 12);
				const uint8_t passed = peekb(0x02BE);
				fl::tstb(passed);
				tick(0x8DCE, 24);
				if (!passed)
				{
					tick(0x8DD0, 12);                  // the first time: the centre is 0x24
					pokeb(0x02BE, fl::incb(peekb(0x02BE)));
					tick(0x8DD4, 27);
					set_w(0x01C6, 0x0024, 0x8DDA);
					tick(0x8E04, 12);                  // BR
				}
				else
				{
					// the position moves one step towards the centre
					tick(0x8DDC, 12);                  // BNE
					T.r[0] = sext8(peekb(0x0227));
					fl::movb((uint8_t)T.r[0]);
					tick(0x8DE0, 24);
					T.r[0] = fl::add(T.r[0], peek(0x01C6));
					tick(0x8DE4, 24);
					fl::cmp(T.r[0], 0x0024);
					tick(0x8DE8, 18);
					if ((int16_t)T.r[0] > 0x24)
					{
						tick(0x8DEA, 12);
						poke(0x01C6, fl::dec(peek(0x01C6)));
						tick(0x8DEE, 27);
						tick(0x8E04, 12);              // BR
					}
					else
					{
						tick(0x8DF0, 12);              // BLE
						T.r[0] = peek(0x01C6);
						fl::mov(T.r[0]);
						tick(0x8DF4, 24);
						T.r[1] = sext8(peekb(0x0227));
						fl::movb((uint8_t)T.r[1]);
						tick(0x8DF8, 24);
						T.r[0] = fl::sub(T.r[0], T.r[1]);
						tick(0x8DFA, 12);
						fl::cmp(T.r[0], 0x0024);
						tick(0x8DFE, 18);
						if ((int16_t)T.r[0] >= 0x24)
							tick(0x8E04, 12);          // BGE
						else
						{
							tick(0x8E00, 12);
							poke(0x01C6, fl::inc(peek(0x01C6)));
							tick(0x8E04, 27);
						}
					}
				}
			}
		}
	}
	poke(0x0224, fl::add(peek(0x0224), peek(0x01C6))); // the position, 0..0x8F
	tick(0x8E0A, 39);
	T.r[0] = peek(0x0224);
	fl::mov(T.r[0]);
	tick(0x8E0E, 24);
	if ((int16_t)T.r[0] < 0)
	{
		tick(0x8E10, 12);
		poke(0x0224, fl::add(peek(0x0224), 0x0090));
		tick(0x8E16, 33);
		tick(0x8E24, 12);                              // BR
	}
	else
	{
		tick(0x8E18, 12);                              // BGE
		fl::cmp(T.r[0], 0x0090);
		tick(0x8E1C, 18);
		if ((int16_t)T.r[0] < 0x90)
			tick(0x8E24, 12);                          // BLT
		else
		{
			tick(0x8E1E, 12);
			poke(0x0224, fl::sub(peek(0x0224), 0x0090));
			tick(0x8E24, 33);
		}
	}
	copy_w(0x0224, 0x01C6, 0x8E2A);
	drop2(0x8E2A);
	rts();
}

// F:910C (assembly; waits for the sound CPU): with interrupts masked, command 0x18 (F:8EA2)
// and two status bytes (F:8ED2) stored at 0x02C0 + the 5-bit counter 0x02E1 (counted up
// first), then the closing answer (F:8F62)
void read_sound_status()
{
	T.r[2] = 0x00E0;
	fl::mov(T.r[2]);
	tick(0x9110, 18);
	mtps((uint8_t)T.r[2]);                             // interrupts masked
	tick(0x9112, 24);
	push(0x0018);
	fl::mov(0x0018);
	tick(0x9116, 30);
	jsr(0x911A, 0x8EA2);                               // send_sound_command(0x18)
	T.r[3] = 0x0002;
	fl::mov(T.r[3]);
	tick(0x911E, 18);
	for (;;)                                           // two status bytes
	{
		T.r[0] = 0;
		fl::clr();
		tick(0x9120, 12);
		T.r[0] = fl::logicb(peekb(0x02E1));
		tick(0x9124, 24);
		T.r[0] = fl::inc(T.r[0]);
		tick(0x9126, 12);
		T.r[1] = T.r[0];
		fl::mov(T.r[1]);
		tick(0x9128, 12);
		T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x001F));
		tick(0x912C, 18);
		clr_b(0x02E1, 0x9130);                         // the 5-bit counter counted up
		pokeb(0x02E1, fl::logicb((uint8_t)(peekb(0x02E1) | T.r[1])));
		tick(0x9134, 27);
		T.r[1] = 0;
		fl::clr();
		tick(0x9136, 12);
		T.r[1] = fl::logicb(peekb(0x02E1));
		tick(0x913A, 24);
		jsr(0x913E, 0x8ED2);                           // read_sound_response
		pokeb((uint16_t)(0x02C0 + T.r[1]), (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x9142, 30);
		if (--T.r[3] == 0)
		{
			tick(0x9144, 18);                          // SOB
			break;
		}
		tick(0x911E, 18);
	}
	jsr(0x9148, 0x8F62);                               // expect_answer_55
	T.r[2] = 0;
	fl::clr();
	tick(0x914A, 12);
	mtps(0);
	tick(0x914E, 30);
	drop(0x914E);
	rts();
}

// F:8D74  wait_frames(int n) (waits): n vertical blanks (F:8D50)
uint16_t wait_frames(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x8D76, 24);
	T.r[1] = peek((uint16_t)(T.r[6] + 4));             // n
	fl::mov(T.r[1]);
	tick(0x8D7A, 27);
	if ((int16_t)T.r[1] <= 0)
		tick(0x8D82, 12);                              // BLE
	else
	{
		tick(0x8D7C, 12);
		for (;;)
		{
			jsr(0x8D80, 0x8D50);                       // wait_vblank
			if (--T.r[1] == 0)
			{
				tick(0x8D82, 18);                      // SOB
				break;
			}
			tick(0x8D7C, 18);
		}
	}
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x8D84, 18);
	rts();
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
	c_save(4, 0x8F78);
	jsr(0x8F7C, 0x8068);                               // select_video_bank0
	T.r[3] = 0xC102;                                   // the tiles of the two halves of a row
	fl::mov(T.r[3]);
	tick(0x8F80, 18);
	T.r[2] = 0xC109;
	fl::mov(T.r[2]);
	tick(0x8F84, 18);
	T.r[5] = 0x0007;
	fl::mov(T.r[5]);
	tick(0x8F88, 18);
	for (;;)                                           // 8 steps, three frames each
	{
		T.r[1] = 0x2000;
		fl::mov(T.r[1]);
		tick(0x8F8C, 18);
		push(0x0003);
		fl::mov(0x0003);
		tick(0x8F90, 30);
		jsr(0x8F94, 0x8D74);                           // wait_frames(3)
		T.r[0] = T.r[5];
		fl::mov(T.r[0]);
		tick(0x8F96, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x8F98, 12);
		{
			const uint16_t v = peek((uint16_t)(0xFC9C + T.r[0]));   // the step's colour
			fl::mov(v);
			poke(0x10B6, v);
		}
		tick(0x8F9E, 42);
		wipe_rows(0x8F9E);
		fl::cmp(T.r[5], 0x0001);
		tick(0x8FB8, 18);
		if (T.r[5] == 1)
		{
			tick(0x8FC0, 12);                          // BEQ: the last step clears
			T.r[3] = 0;
			fl::clr();
			tick(0x8FC2, 12);
			T.r[2] = 0;
			fl::clr();
			tick(0x8FC4, 12);
		}
		else
		{
			tick(0x8FBA, 12);
			T.r[3] = fl::inc(T.r[3]);
			tick(0x8FBC, 12);
			T.r[2] = fl::inc(T.r[2]);
			tick(0x8FBE, 12);
			tick(0x8FC4, 12);                          // BR
		}
		drop(0x8FC4);
		T.r[5] = fl::dec(T.r[5]);
		tick(0x8FC8, 12);
		if ((int16_t)T.r[5] < 0)
		{
			tick(0x8FCA, 12);                          // BGE not taken
			break;
		}
		tick(0x8F88, 12);
	}
	clr_b(0x02BF, 0x8FCE);
	c_restore(4);
	return T.r[0];
}

// F:8FD0 (waits): the texts are drawn (F:A8FE), then the playfield is filled in 8 steps, 3
// frames apart, with palette entry 0x10B6 from 0xFC9C and the tile pair 0xC108 / 0xC10F counting
// down (the last step 0xC087); 0x02BF counts up.  R1-R5 preserved (F162).
uint16_t wipe_screen_in(const Args &)
{
	c_save(4, 0x8FD4);
	jsr(0x8FD8, 0xA8FE);                               // flush_text_list
	jsr(0x8FDC, 0x8068);                               // select_video_bank0
	T.r[3] = 0xC108;
	fl::mov(T.r[3]);
	tick(0x8FE0, 18);
	T.r[2] = 0xC10F;
	fl::mov(T.r[2]);
	tick(0x8FE4, 18);
	T.r[5] = 0;
	fl::clr();
	tick(0x8FE6, 12);
	for (;;)                                           // 8 steps, three frames each
	{
		T.r[1] = 0x2000;
		fl::mov(T.r[1]);
		tick(0x8FEA, 18);
		push(0x0003);
		fl::mov(0x0003);
		tick(0x8FEE, 30);
		jsr(0x8FF2, 0x8D74);                           // wait_frames(3)
		T.r[0] = T.r[5];
		fl::mov(T.r[0]);
		tick(0x8FF4, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x8FF6, 12);
		{
			const uint16_t v = peek((uint16_t)(0xFC9C + T.r[0]));   // the step's colour
			fl::mov(v);
			poke(0x10B6, v);
		}
		tick(0x8FFC, 42);
		wipe_rows(0x8FFC);
		fl::cmp(T.r[5], 0x0006);
		tick(0x9016, 18);
		if (T.r[5] == 6)
		{
			tick(0x901E, 12);                          // BEQ: the last step's tiles
			T.r[3] = 0xC087;
			fl::mov(T.r[3]);
			tick(0x9022, 18);
			T.r[2] = T.r[3];
			fl::mov(T.r[2]);
			tick(0x9024, 12);
		}
		else
		{
			tick(0x9018, 12);
			T.r[2] = fl::dec(T.r[2]);
			tick(0x901A, 12);
			T.r[3] = fl::dec(T.r[3]);
			tick(0x901C, 12);
			tick(0x9024, 12);                          // BR
		}
		drop(0x9024);
		T.r[5] = fl::inc(T.r[5]);
		tick(0x9028, 12);
		fl::cmp(T.r[5], 0x0007);
		tick(0x902C, 18);
		if ((int16_t)T.r[5] > 7)
		{
			tick(0x902E, 12);                          // BLE not taken
			break;
		}
		tick(0x8FE6, 12);
	}
	pokeb(0x02BF, fl::incb(peekb(0x02BF)));
	tick(0x9032, 27);
	c_restore(4);
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
	push_w(0x01D2, 0x832C);                            // the banks, put back at the end
	push_w(0x01D4, 0x8330);
	bank_a(0x8330, 0x1800);
	push_sp(0x0A, 0x8340);                             // A5:4000(a, b, c)
	push_sp(0x0A, 0x8344);
	push_sp(0x0A, 0x8348);
	jsr(0x834C, 0x4000);                               // init_map_view
	bank_a(0x834C, 0x3800);
	bank_b(0x8358, 0x3C00);
	jsr(0x8368, 0x8036);                               // load_scroll_registers
	set_b(0x0370, 0x01, 0x836E);                       // objects 0-3: the skater's parts
	set_b(0x0470, 0x30, 0x8374);
	set_b(0x0371, 0x01, 0x837A);
	set_b(0x0471, 0x10, 0x8380);
	set_b(0x0372, 0x02, 0x8386);
	clr_b(0x0472, 0x838A);
	set_b(0x0373, 0x03, 0x8390);
	clr_b(0x0473, 0x8394);
	set_w(0x03B6, 0x50FA, 0x839A);
	{
		const uint16_t v = peek((uint16_t)(T.r[6] + 0x16));   // place_skater(dz, y - scroll y,
		fl::mov(v);                                            // x + scroll x)
		poke(T.r[6], v);
	}
	tick(0x839E, 36);
	poke(T.r[6], fl::add(peek(T.r[6]), peek(0x01A8)));
	tick(0x83A2, 33);
	push_sp(0x14, 0x83A6);
	poke(T.r[6], fl::sub(peek(T.r[6]), peek(0x01AA)));
	tick(0x83AA, 33);
	push_sp(0x14, 0x83AE);
	jsr(0x83B2, 0x843E);                               // place_skater
	T.r[1] = 0;
	fl::clr();
	tick(0x83B4, 12);
	for (;;)                                           // objects 0-3 shown
	{
		T.r[0] = T.r[1];
		fl::mov(T.r[0]);
		tick(0x83B6, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x83B8, 12);
		poke((uint16_t)(0x03F0 + T.r[0]), 0x0001);
		fl::mov(0x0001);
		tick(0x83BE, 36);
		poke(T.r[6], T.r[1]);
		fl::mov(T.r[1]);
		tick(0x83C0, 21);
		jsr(0x83C4, 0xB4B6);                           // show_object
		T.r[1] = fl::inc(T.r[1]);
		tick(0x83C6, 12);
		fl::cmp(T.r[1], 0x0003);
		tick(0x83CA, 18);
		if ((int16_t)T.r[1] > 3)
		{
			tick(0x83CC, 12);                          // BLE not taken
			break;
		}
		tick(0x83B4, 12);
	}
	{
		const uint16_t v = peek(0x06F0);               // fill_slots(120, z, y, x)
		fl::mov(v);
		poke(T.r[6], v);
	}
	tick(0x83D0, 33);
	push_w(0x0730, 0x83D4);
	push_w(0x06B0, 0x83D8);
	push(0x0078);
	fl::mov(0x0078);
	tick(0x83DC, 30);
	jsr(0x83E0, 0xE1FA);                               // fill_slots
	clr_b(0x0F13, 0x83E4);
	clr_b(0x0F14, 0x83E8);
	bank_a(0x83E8, 0x1800);
	bank_b(0x83F4, 0x1C00);
	jsr(0x8404, 0x5B18);                               // the sound sequence
	clr_b(0x01F8, 0x8408);
	clr_b(0x032A, 0x840C);
	set_b(0x0ED0, 0x02, 0x8412);
	jsr(0x8416, 0x8D50);                               // wait_vblank
	set_w(0x0EB0, 0xDB4E, 0x841C);                     // the sprite palette
	jsr(0x8420, 0x8D50);
	{
		const uint16_t v = peek((uint16_t)(T.r[6] + 0x12));   // the banks back
		fl::mov(v);
		poke(0x01D2, v);
	}
	tick(0x8426, 42);
	{
		const uint16_t v = peek((uint16_t)(T.r[6] + 0x10));
		fl::mov(v);
		poke(0x01D4, v);
	}
	tick(0x842C, 42);
	{
		const uint16_t v = peek(0x01D2);
		fl::mov(v);
		poke(0x1400, v);
	}
	tick(0x8432, 39);
	{
		const uint16_t v = peek(0x01D4);
		fl::mov(v);
		poke(0x1402, v);
	}
	tick(0x8438, 39);
	T.r[6] = fl::add(T.r[6], 0x0014);
	tick(0x843C, 18);
	rts();
}

// F:891E (interrupt; not reached by the tests): acknowledge the sound CPU's interrupt
void irq_sound_ack()
{
	clr_w(0x1580, 0x8922);                             // the RTI is in the table
}

// F:8A9E (assembly; not reached by the tests): string 0x19 and the one for the sound CPU's
// fault 0x01A2 (0x19 + code)
void show_sound_fault()
{
	T.r[0] = 0x0019;
	fl::mov(T.r[0]);
	tick(0x8AA2, 18);
	jsr(0x8AA6, 0x8AB6);                               // draw_string_large(0x19)
	T.r[0] = 0;
	fl::clr();
	tick(0x8AA8, 12);
	T.r[0] = fl::logicb(peekb(0x01A2));
	tick(0x8AAC, 24);
	T.r[0] = fl::add(T.r[0], 0x0019);
	tick(0x8AB0, 18);
	jsr(0x8AB4, 0x8AB6);                               // draw_string_large(0x19 + fault)
	rts();
}

// F:9182 (assembly; not reached by the tests): sounds 0x53, 0x42, 0x70 and 0x76
void queue_four_sounds()
{
	push(0x0053);
	fl::mov(0x0053);
	tick(0x9186, 30);
	jsr(0x918A, 0x8E78);                               // queue_sound
	static const uint16_t more[3] = {0x0042, 0x0070, 0x0076};
	for (int i = 0; i < 3; i++)
	{
		const uint16_t pc = (uint16_t)(0x918A + 8 * i);
		poke(T.r[6], more[i]);
		fl::mov(more[i]);
		tick((uint16_t)(pc + 4), 27);
		jsr((uint16_t)(pc + 8), 0x8E78);
	}
	drop(0x91A2);
	rts();
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
	c_save(3, 0x9DE8);
	T.r[3] = peek(0x01D2);                             // the banks, put back at the end
	fl::mov(T.r[3]);
	tick(0x9DEC, 24);
	T.r[4] = peek(0x01D4);
	fl::mov(T.r[4]);
	tick(0x9DF0, 24);
	bank_a(0x9DF0, 0x0800);
	bank_b(0x9DFC, 0x0C00);
	set_w(0x0212, 0x0100, 0x9E0E);
	const uint16_t speed = peek(0x01B4);
	fl::tst(speed);
	tick(0x9E12, 24);
	if ((int16_t)speed < 0)
	{
		tick(0x9E1A, 12);                              // BLT: cell 0 behind
		T.r[2] = 0;
		fl::clr();
		tick(0x9E1C, 12);
	}
	else
	{
		tick(0x9E14, 12);                              // cell 2 ahead
		T.r[2] = 0x0002;
		fl::mov(T.r[2]);
		tick(0x9E18, 18);
		tick(0x9E1C, 12);                              // BR
	}
	T.r[2] = fl::asl(T.r[2]);
	tick(0x9E1E, 12);
	{
		const uint16_t v = peek((uint16_t)(0x0314 + T.r[2]));
		fl::mov(v);
		poke(0x0218, v);
	}
	tick(0x9E24, 42);
	push(0x7FFD);
	fl::mov(0x7FFD);
	tick(0x9E28, 30);
	T.r[0] = peek((uint16_t)(0x0308 + T.r[2]));
	fl::mov(T.r[0]);
	tick(0x9E2C, 27);
	T.r[1] = peek((uint16_t)(0x030E + T.r[2]));
	fl::mov(T.r[1]);
	tick(0x9E30, 27);
	jsr(0x9E34, 0x9C5E);                               // trace_cells
	copy_w(0x0222, 0x06B0, 0x9E3A);
	poke(0x01D2, T.r[3]);
	fl::mov(T.r[3]);
	tick(0x9E3E, 27);
	poke(0x01D4, T.r[4]);
	fl::mov(T.r[4]);
	tick(0x9E42, 27);
	copy_w(0x01D2, 0x1400, 0x9E48);
	copy_w(0x01D4, 0x1402, 0x9E4E);
	drop(0x9E4E);
	c_restore(3);
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
