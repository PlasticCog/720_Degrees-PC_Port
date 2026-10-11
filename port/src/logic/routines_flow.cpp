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

// Cycles and interrupts.  Each instruction of the original is a step: its action, its
// condition codes, then tick(next, cycles), which charges its cycles and syncs at the next
// instruction's address.  The original can be interrupted after any instruction: when its
// time slice ends there, the board runs and a pending interrupt comes in before the next
// instruction.  The native code syncs at the same instruction boundaries with the same
// registers, condition codes and stack, so the board and the interrupts see what they see
// in the original.

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


// MOV Ra,R0; ASL R0; MOV 0x0908(R0),R4; MOV 0x3806(R4),R0; BIC #0x07FE,R0; MOV Rb,R5; ASL R5;
// BIS 0x0888(R5),R0; MOV R0,0x3806(R4) at pc: the last sprite of object Ra links to the first
// one of object Rb
void relink(uint16_t pc, int a, int b)
{
	T.r[0] = T.r[a];
	fl::mov(T.r[0]);
	tick((uint16_t)(pc + 2), 12);
	T.r[0] = fl::asl(T.r[0]);
	tick((uint16_t)(pc + 4), 12);
	T.r[4] = peek((uint16_t)(0x0908 + T.r[0]));
	fl::mov(T.r[4]);
	tick((uint16_t)(pc + 8), 27);
	T.r[0] = peek((uint16_t)(0x3806 + T.r[4]));
	fl::mov(T.r[0]);
	tick((uint16_t)(pc + 0x0C), 27);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & ~0x07FE));
	tick((uint16_t)(pc + 0x10), 18);
	T.r[5] = T.r[b];
	fl::mov(T.r[5]);
	tick((uint16_t)(pc + 0x12), 12);
	T.r[5] = fl::asl(T.r[5]);
	tick((uint16_t)(pc + 0x14), 12);
	T.r[0] = fl::logic((uint16_t)(T.r[0] | peek((uint16_t)(0x0888 + T.r[5]))));
	tick((uint16_t)(pc + 0x18), 27);
	poke((uint16_t)(0x3806 + T.r[4]), T.r[0]);
	fl::mov(T.r[0]);
	tick((uint16_t)(pc + 0x1C), 30);
}
// MOVB Rr,k(Rx) (30 cycles) and the step to next
void store_b(int r, uint16_t a, uint16_t next)
{
	pokeb(a, (uint8_t)T.r[r]);
	fl::movb((uint8_t)T.r[r]);
	tick(next, 30);
}


// MOV #8,Rr; MOVB @#a,R0; XOR R0,Rr; MOVB Rr,@#a at pc: the direction byte a turned round
void turn_round(uint16_t pc, int r, uint16_t a)
{
	T.r[r] = 0x0008;
	fl::mov(T.r[r]);
	tick((uint16_t)(pc + 4), 18);
	T.r[0] = sext8(peekb(a));
	fl::movb((uint8_t)T.r[0]);
	tick((uint16_t)(pc + 8), 24);
	T.r[r] = fl::logic((uint16_t)(T.r[r] ^ T.r[0]));
	tick((uint16_t)(pc + 0x0A), 12);
	pokeb(a, (uint8_t)T.r[r]);
	fl::movb((uint8_t)T.r[r]);
	tick((uint16_t)(pc + 0x0E), 27);
}
// MOVB @#from,@#to (39 cycles) and the step to next
void copy_b(uint16_t from, uint16_t to, uint16_t next)
{
	const uint8_t v = peekb(from);
	fl::movb(v);
	pokeb(to, v);
	tick(next, 39);
}

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

// F:9034  divide_ext(int mode, -, int d, int n): n / d, truncated and signed, by shift and
// subtract; with mode != 0 the remainder instead (sign of n).  0 when n or d is 0.
// R1-R5 preserved (F162).
// F:90A6  divide(int d, int n) = F:9034(0, ...);  F:90B0  modulo(int d, int n) = F:9034 with
// a non-zero mode (the PC pushed by MOV PC,-(SP))
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
	const uint8_t kick = peekb(0x01EB);
	fl::cmpb(kick, 0x04);
	tick(0x9F90, 30);
	if (kick != 4)
		tick(0x9F98, 12);                              // BNE
	else
	{
		tick(0x9F92, 12);                              // the speed back from 0x02E8
		copy_w(0x02E8, 0x01B4, 0x9F98);
	}
	jsr(0x9F9C, 0xE238);                               // reset_skater_state
	set_b(0x07B1, 0x03, 0x9FA2);
	set_b(0x01EB, 0x08, 0x9FA8);
	set_b(0x0330, 0x04, 0x9FAE);
	jsr(0x9FB2, 0x9FB4);                               // skater_take_off
	rts();
	return T.r[0];
}

// F:AFF0 (assembly): apply the queued sprite size changes: for each of the 4 requests
// (object 0x0989+i, size 0x098D+i) the object's size 0x04D0 is set; a linked object
// (0x0490 bit 1) is unlinked and linked again (F:ACE6, F:AE02).  Bit 5 of 0x0490 is cleared;
// requests whose object has no bit 6 are done (0x0992 counts the pending ones).
void process_sprite_requests()
{
	const uint8_t pending = peekb(0x0992);
	fl::tstb(pending);
	tick(0xAFF4, 24);
	if (!pending)
	{
		tick(0xB03A, 12);                              // BEQ
		rts();
		return;
	}
	tick(0xAFF6, 12);
	T.r[2] = 0x0004;
	fl::mov(T.r[2]);
	tick(0xAFFA, 18);
	for (;;)
	{
		T.r[1] = sext8(peekb((uint16_t)(0x0989 + T.r[2])));   // the object of request R2
		fl::movb((uint8_t)T.r[1]);
		tick(0xAFFE, 27);
		if (!T.r[1])
			tick(0xB038, 12);                          // BEQ
		else
		{
			tick(0xB000, 12);
			{
				const uint8_t v = peekb((uint16_t)(0x098D + T.r[2]));   // the new size
				fl::movb(v);
				pokeb((uint16_t)(0x04D0 + T.r[1]), v);
			}
			tick(0xB006, 45);
			T.r[3] = 0x0490;
			fl::mov(T.r[3]);
			tick(0xB00A, 18);
			T.r[3] = fl::add(T.r[3], T.r[1]);
			tick(0xB00C, 12);
			fl::logicb((uint8_t)(peekb(T.r[3]) & 0x02));
			tick(0xB010, 24);
			if (!(peekb(T.r[3]) & 0x02))
				tick(0xB026, 12);                      // BEQ
			else
			{
				tick(0xB012, 12);                      // linked: unlinked and linked again
				push(0);
				fl::clr();
				tick(0xB014, 24);
				push(T.r[1]);
				fl::mov(T.r[1]);
				tick(0xB016, 24);
				jsr(0xB01A, 0xACE6);                   // unlink_sprites
				poke(T.r[6], 0);
				fl::clr();
				tick(0xB01C, 21);
				push(T.r[1]);
				fl::mov(T.r[1]);
				tick(0xB01E, 24);
				jsr(0xB022, 0xAE02);                   // link_sprites
				T.r[6] = fl::add(T.r[6], 0x0006);
				tick(0xB026, 18);
			}
			pokeb(T.r[3], fl::logicb((uint8_t)(peekb(T.r[3]) & ~0x20)));
			tick(0xB02A, 27);
			fl::logicb((uint8_t)(peekb(T.r[3]) & 0x40));
			tick(0xB02E, 24);
			if (peekb(T.r[3]) & 0x40)
				tick(0xB038, 12);                      // BNE
			else
			{
				tick(0xB030, 12);                      // done
				pokeb(0x0992, fl::decb(peekb(0x0992)));
				tick(0xB034, 27);
				pokeb((uint16_t)(0x0989 + T.r[2]), 0);
				fl::clr();
				tick(0xB038, 30);
			}
		}
		if (--T.r[2] == 0)
		{
			tick(0xB03A, 18);                          // SOB
			break;
		}
		tick(0xAFFA, 18);
	}
	rts();
}

// F:A974  queue_message(int bit, int a, int b): with interrupts masked, the entry count
// 0x0358 goes up (at most 3) and the entry gets the words b (0x0362) and a (0x035A); bit is
// shifted into the mask 0x036D and a 1 into 0x036E unless the text table 0x0184 is 0x5AC8.
// Returns the shifted 0x036E (before that bit).
uint16_t queue_message(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xA976, 24);
	T.r[1] = 0x00E0;
	fl::mov(T.r[1]);
	tick(0xA97A, 18);
	mtps(0xE0);                                        // interrupts masked
	tick(0xA97C, 24);
	T.r[0] = sext8(peekb(0x0358));                     // the entry count, at most 3
	fl::movb((uint8_t)T.r[0]);
	tick(0xA980, 24);
	T.r[0] = fl::inc(T.r[0]);
	tick(0xA982, 12);
	pokeb(0x0358, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0xA986, 27);
	fl::cmpb((uint8_t)T.r[0], 0x03);
	tick(0xA98A, 18);
	if ((int8_t)(uint8_t)T.r[0] <= 3)
		tick(0xA992, 12);                              // BLE
	else
	{
		tick(0xA98C, 12);
		set_b(0x0358, 0x03, 0xA992);
	}
	T.r[0] = sext8(peekb(0x0358));
	fl::movb((uint8_t)T.r[0]);
	tick(0xA996, 24);
	T.r[0] = fl::asl(T.r[0]);
	tick(0xA998, 12);
	{
		const uint16_t v = peek((uint16_t)(T.r[6] + 8));
		fl::mov(v);
		poke((uint16_t)(0x0362 + T.r[0]), v);
	}
	tick(0xA99E, 45);
	{
		const uint16_t v = peek((uint16_t)(T.r[6] + 6));
		fl::mov(v);
		poke((uint16_t)(0x035A + T.r[0]), v);
	}
	tick(0xA9A4, 45);
	T.r[1] = 0;
	fl::clr();
	tick(0xA9A6, 12);
	mtps(0);                                           // a pending interrupt is taken here
	tick(0xA9AA, 30);
	// CLR R0 at pc; BISB @#a,R0; ASL R0; MOVB R0,@#a: the mask a shifted
	auto shift = [](uint16_t pc, uint16_t a) {
		T.r[0] = 0;
		fl::clr();
		tick((uint16_t)(pc + 2), 12);
		T.r[0] = (uint16_t)(T.r[0] | fl::logicb(peekb(a)));
		tick((uint16_t)(pc + 6), 24);
		T.r[0] = fl::asl(T.r[0]);
		tick((uint16_t)(pc + 8), 12);
		pokeb(a, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick((uint16_t)(pc + 0x0C), 27);
	};
	shift(0xA9AA, 0x036D);
	pokeb(0x036D, fl::logicb((uint8_t)(peekb(0x036D) | peekb((uint16_t)(T.r[6] + 4)))));
	tick(0xA9BC, 42);
	shift(0xA9BC, 0x036E);
	const uint16_t table = peek(0x0184);
	fl::cmp(table, 0x5AC8);
	tick(0xA9CE, 30);
	if (table == 0x5AC8)
		tick(0xA9D6, 12);                              // BEQ
	else
	{
		tick(0xA9D0, 12);
		pokeb(0x036E, fl::logicb((uint8_t)(peekb(0x036E) | 0x01)));
		tick(0xA9D6, 33);
	}
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0xA9D8, 18);
	rts();
	return T.r[0];
}

// F:A8FE  flush_text_list(): the entries 0..0x0336 of the list at 0x0348 (bank B 0x2800) are
// drawn: a pointer to a value from 0x4000 on goes to F:EAD4, others to A7:4D30; the list is
// emptied (0x0336 = 0xFF) and the banks restored.  R1-R5 preserved (F162).
uint16_t flush_text_list(const Args &)
{
	c_save(4, 0xA902);
	T.r[4] = peek(0x01D2);                             // the banks, put back at the end
	fl::mov(T.r[4]);
	tick(0xA906, 24);
	T.r[5] = peek(0x01D4);
	fl::mov(T.r[5]);
	tick(0xA90A, 24);
	T.r[3] = sext8(peekb(0x0336));                     // the last entry
	fl::movb((uint8_t)T.r[3]);
	tick(0xA90E, 24);
	T.r[2] = 0;
	fl::clr();
	tick(0xA910, 12);
	tick(0xA954, 12);                                  // BR
	for (;;)
	{
		fl::cmp(T.r[2], T.r[3]);
		tick(0xA956, 12);
		if ((int16_t)T.r[2] > (int16_t)T.r[3])
		{
			tick(0xA958, 12);                          // BLE not taken
			break;
		}
		tick(0xA912, 12);
		bank_b(0xA912, 0x2800);
		T.r[1] = T.r[2];
		fl::mov(T.r[1]);
		tick(0xA920, 12);
		T.r[1] = fl::asl(T.r[1]);
		tick(0xA922, 12);
		const uint16_t p = peek((uint16_t)(0x0348 + T.r[1]));
		const uint16_t v = peek(p);
		fl::cmp(v, 0x4000);
		tick(0xA928, 39);
		if ((int16_t)v >= 0x4000)
		{
			tick(0xA92A, 12);                          // a tile block
			{
				const uint16_t w = peek(peek((uint16_t)(0x0348 + T.r[1])));
				fl::mov(w);
				push(w);
			}
			tick(0xA92E, 45);
			push(0x0001);
			fl::mov(0x0001);
			tick(0xA932, 30);
			jsr(0xA936, 0xEAD4);                       // draw_tile_block
			tick(0xA950, 12);                          // BR
		}
		else
		{
			tick(0xA938, 12);                          // BLT
			bank_a(0xA938, 0x3800);
			{
				const uint16_t w = peek((uint16_t)(0x0348 + T.r[1]));
				fl::mov(w);
				push(w);
			}
			tick(0xA948, 39);
			push(0x0001);
			fl::mov(0x0001);
			tick(0xA94C, 30);
			jsr(0xA950, 0x4D30);
		}
		drop2(0xA950);
		T.r[2] = fl::inc(T.r[2]);
		tick(0xA954, 12);
	}
	set_b(0x0336, 0xFF, 0xA95E);                       // the list emptied
	poke(0x01D2, T.r[4]);
	fl::mov(T.r[4]);
	tick(0xA962, 27);
	poke(0x01D4, T.r[5]);
	fl::mov(T.r[5]);
	tick(0xA966, 27);
	copy_w(0x01D2, 0x1400, 0xA96C);
	copy_w(0x01D4, 0x1402, 0xA972);
	c_restore(4);
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
uint16_t skater_phase10(const Args &)
{
	c_save(1, 0xA45E);
	T.r[1] = peek(0x01D2);                             // the banks, put back at the end
	fl::mov(T.r[1]);
	tick(0xA462, 24);
	T.r[2] = peek(0x01D4);
	fl::mov(T.r[2]);
	tick(0xA466, 24);
	clr_w(0x01D2, 0xA46A);
	clr_w(0x1400, 0xA46E);
	bank_b(0xA46E, 0x0400);
	T.r[0] = peek((uint16_t)(T.r[6] + 0x0A));          // the direction, even
	fl::mov(T.r[0]);
	tick(0xA47E, 27);
	T.r[0] = fl::asr(T.r[0]);
	tick(0xA480, 12);
	T.r[0] = fl::asl(T.r[0]);
	tick(0xA482, 12);
	const uint16_t side = peek((uint16_t)(T.r[6] + 8));
	fl::tst(side);
	tick(0xA486, 27);
	if ((int16_t)side >= 0)
	{
		tick(0xA488, 12);
		{
			const uint16_t v = peek((uint16_t)(0x4EAC + T.r[0]));
			fl::mov(v);
			poke(0x03B2, v);
		}
		tick(0xA48E, 42);
		poke(0x03B4, T.r[0]);
		fl::mov(T.r[0]);
		tick(0xA492, 27);
		poke(0x03B4, fl::add(peek(0x03B4), 0x42C0));
		tick(0xA498, 33);
		tick(0xA4AA, 12);                              // BR
	}
	else
	{
		tick(0xA49A, 12);                              // BLT
		{
			const uint16_t v = peek((uint16_t)(0x4EBC + T.r[0]));
			fl::mov(v);
			poke(0x03B2, v);
		}
		tick(0xA4A0, 42);
		poke(0x03B4, T.r[0]);
		fl::mov(T.r[0]);
		tick(0xA4A4, 27);
		poke(0x03B4, fl::add(peek(0x03B4), 0x42B0));
		tick(0xA4AA, 33);
	}
	clr_w(0x02FC, 0xA4AE);
	jsr(0xA4B2, 0xE238);                               // reset_skater_state
	set_b(0x01EB, 0x0A, 0xA4B8);
	set_b(0x0330, 0x0A, 0xA4BE);
	clr_b(0x07B1, 0xA4C2);
	T.r[0] = 0x0003;
	fl::mov(T.r[0]);
	tick(0xA4C6, 18);
	pokeb(0x0791, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0xA4CA, 27);
	pokeb(0x0771, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0xA4CE, 27);
	set_b(0x01EA, 0x02, 0xA4D4);
	bank_a(0xA4D4, 0x3800);
	bank_b(0xA4E0, 0x3C00);
	jsr(0xA4F0, 0x7AD2);                               // save_frame_counter
	push(0x0057);
	fl::mov(0x0057);
	tick(0xA4F4, 30);
	jsr(0xA4F8, 0x8E78);                               // queue_sound(0x57)
	poke(0x01D2, T.r[1]);
	fl::mov(T.r[1]);
	tick(0xA4FC, 27);
	poke(0x01D4, T.r[2]);
	fl::mov(T.r[2]);
	tick(0xA500, 27);
	copy_w(0x01D2, 0x1400, 0xA506);
	copy_w(0x01D4, 0x1402, 0xA50C);
	drop(0xA50C);
	c_restore(1);
	return T.r[0];
}

// Sprite draw lists.  Objects are kept in doubly linked lists (next 0x0808, previous 0x0848,
// list heads 0x0988) in drawing order; each object owns a chain of motion object slots
// (first 0x0888, last 0x0908) whose link fields (motion object RAM 0x3806, bits 1-10) chain
// all sprites of a list together.

// F:ACE6  unlink_sprites(int obj, int list): take obj out of list (the previous object's
// sprites link to the next one's, or end the chain with 0x03F8), clear the list's bits
// (0xFD7C) in 0x0490 and free the object's sprite slots.  Returns the last slot.
uint16_t unlink_sprites(const Args &)
{
	c_save(4, 0xACEA);
	T.r[3] = peek((uint16_t)(T.r[6] + 0x0E));          // the object
	fl::mov(T.r[3]);
	tick(0xACEE, 27);
	T.r[2] = sext8(peekb((uint16_t)(0x0848 + T.r[3])));   // the previous one
	fl::movb((uint8_t)T.r[2]);
	tick(0xACF2, 27);
	T.r[1] = sext8(peekb((uint16_t)(0x0808 + T.r[3])));   // the next one
	fl::movb((uint8_t)T.r[1]);
	tick(0xACF6, 27);
	fl::tst(T.r[2]);
	tick(0xACF8, 12);
	bool link_back = true;                             // the next one gets the previous one
	if ((int16_t)T.r[2] >= 0)
	{
		tick(0xACFA, 12);
		pokeb((uint16_t)(0x0808 + T.r[2]), (uint8_t)T.r[1]);
		fl::movb((uint8_t)T.r[1]);
		tick(0xACFE, 30);
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0xAD00, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0xAD02, 12);
		T.r[0] = peek((uint16_t)(0x0908 + T.r[0]));   // its last sprite slot
		fl::mov(T.r[0]);
		tick(0xAD06, 27);
		T.r[4] = 0x3806;
		fl::mov(T.r[4]);
		tick(0xAD0A, 18);
		T.r[4] = fl::add(T.r[4], T.r[0]);
		tick(0xAD0C, 12);
		fl::tst(T.r[1]);
		tick(0xAD0E, 12);
		if ((int16_t)T.r[1] >= 0)
		{
			tick(0xAD10, 12);                          // links to the next one's sprites
			T.r[0] = peek(T.r[4]);
			fl::mov(T.r[0]);
			tick(0xAD12, 18);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & ~0x07FE));
			tick(0xAD16, 18);
			T.r[5] = T.r[1];
			fl::mov(T.r[5]);
			tick(0xAD18, 12);
			T.r[5] = fl::asl(T.r[5]);
			tick(0xAD1A, 12);
			T.r[0] = fl::logic((uint16_t)(T.r[0] | peek((uint16_t)(0x0888 + T.r[5]))));
			tick(0xAD1E, 27);
			poke(T.r[4], T.r[0]);
			fl::mov(T.r[0]);
			tick(0xAD20, 21);
			tick(0xAD3C, 12);                          // BR
		}
		else
		{
			tick(0xAD22, 12);                          // BLT: or ends the chain
			T.r[0] = peek(T.r[4]);
			fl::mov(T.r[0]);
			tick(0xAD24, 18);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & ~0x07FE));
			tick(0xAD28, 18);
			T.r[0] = fl::logic((uint16_t)(T.r[0] | 0x03F8));
			tick(0xAD2C, 18);
			poke(T.r[4], T.r[0]);
			fl::mov(T.r[0]);
			tick(0xAD2E, 21);
			tick(0xAD40, 12);                          // BR
			link_back = false;
		}
	}
	else
	{
		tick(0xAD30, 12);                              // BLT: the first one: the list head
		T.r[0] = peek((uint16_t)(T.r[6] + 0x10));
		fl::mov(T.r[0]);
		tick(0xAD34, 27);
		pokeb((uint16_t)(0x0988 + T.r[0]), (uint8_t)T.r[1]);
		fl::movb((uint8_t)T.r[1]);
		tick(0xAD38, 30);
		fl::tst(T.r[1]);
		tick(0xAD3A, 12);
		link_back = (int16_t)T.r[1] >= 0;
		tick(link_back ? 0xAD3C : 0xAD40, 12);         // BLT
	}
	if (link_back)
	{
		pokeb((uint16_t)(0x0848 + T.r[1]), (uint8_t)T.r[2]);
		fl::movb((uint8_t)T.r[2]);
		tick(0xAD40, 30);
	}
	T.r[0] = T.r[3];                                   // the bits of the list cleared
	fl::mov(T.r[0]);
	tick(0xAD42, 12);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x001F));
	tick(0xAD46, 18);
	T.r[4] = peek((uint16_t)(T.r[6] + 0x10));
	fl::mov(T.r[4]);
	tick(0xAD4A, 27);
	{
		const uint16_t a = (uint16_t)(0x0490 + T.r[0]);
		pokeb(a, fl::logicb((uint8_t)(peekb(a) & ~peekb((uint16_t)(0xFD7C + T.r[4])))));
	}
	tick(0xAD50, 45);
	T.r[3] = fl::asl(T.r[3]);
	tick(0xAD52, 12);
	T.r[0] = peek((uint16_t)(0x0888 + T.r[3]));       // the sprite slots freed
	fl::mov(T.r[0]);
	tick(0xAD56, 27);
	T.r[1] = peek((uint16_t)(0x0908 + T.r[3]));
	fl::mov(T.r[1]);
	tick(0xAD5A, 27);
	for (;;)
	{
		T.r[2] = peek((uint16_t)(0x3806 + T.r[0]));
		fl::mov(T.r[2]);
		tick(0xAD5E, 27);
		T.r[2] = fl::logic((uint16_t)(T.r[2] & ~0xF807));
		tick(0xAD62, 18);
		poke((uint16_t)(0x3806 + T.r[0]), 0);
		fl::clr();
		tick(0xAD66, 30);
		fl::cmp(T.r[0], T.r[1]);
		tick(0xAD68, 12);
		if (T.r[0] == T.r[1])
		{
			tick(0xAD6E, 12);                          // BEQ: the last one
			break;
		}
		tick(0xAD6A, 12);
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0xAD6C, 12);
		tick(0xAD5A, 12);                              // BR
	}
	c_restore(4);
	return T.r[0];
}

// F:AE02  link_sprites(int obj, int list): give obj its 0x04D0 sprite slots (free slots from
// slot 1 on, chained; the last one ends with 0x03F9), set the list's bits (0xFD7C, 0xFD7E) in
// 0x0490, clear bit 2 of 0x04B0, and insert obj into list before the first object it is in
// front of (F:AD70), else at the end.  R1-R5 preserved (F162).
uint16_t link_sprites(const Args &)
{
	c_save(4, 0xAE06);
	T.r[4] = peek((uint16_t)(T.r[6] + 0x0E));          // the object
	fl::mov(T.r[4]);
	tick(0xAE0A, 27);
	T.r[0] = T.r[4];
	fl::mov(T.r[0]);
	tick(0xAE0C, 12);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x001F));
	tick(0xAE10, 18);
	T.r[1] = peek((uint16_t)(T.r[6] + 0x10));          // the list
	fl::mov(T.r[1]);
	tick(0xAE14, 27);
	T.r[2] = sext8(peekb((uint16_t)(0x0490 + T.r[0])));   // its bits for the list
	fl::movb((uint8_t)T.r[2]);
	tick(0xAE18, 27);
	T.r[2] = (uint16_t)((T.r[2] & 0xFF00) | fl::logicb((uint8_t)(T.r[2] | peekb((uint16_t)(0xFD7C + T.r[1])))));
	tick(0xAE1C, 27);
	T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x00FF));
	tick(0xAE20, 18);
	{
		const uint8_t v = peekb((uint16_t)(0xFD7E + T.r[1]));
		fl::movb(v);
		pokeb((uint16_t)(0x0490 + T.r[0]), v);
	}
	tick(0xAE26, 45);
	{
		const uint16_t a = (uint16_t)(0x0490 + T.r[0]);
		pokeb(a, fl::logicb((uint8_t)(peekb(a) | (uint8_t)T.r[2])));
	}
	tick(0xAE2A, 30);
	{
		const uint16_t a = (uint16_t)(0x04B0 + T.r[0]);
		pokeb(a, fl::logicb((uint8_t)(peekb(a) & ~0x04)));
	}
	tick(0xAE30, 36);
	T.r[1] = 0;                                        // the sprite slots: the first one
	fl::clr();
	tick(0xAE32, 12);
	T.r[2] = 0x0008;
	fl::mov(T.r[2]);
	tick(0xAE36, 18);
	T.r[3] = 0;
	fl::clr();
	tick(0xAE38, 12);
	T.r[3] = (uint16_t)(T.r[3] | fl::logicb(peekb((uint16_t)(0x04D0 + T.r[0]))));   // its size
	tick(0xAE3C, 27);
	fl::tst(T.r[3]);
	tick(0xAE3E, 12);
	if ((int16_t)T.r[3] <= 0)
		tick(0xAE6C, 12);                              // BLE
	else
	{
		tick(0xAE40, 12);
		for (;;)
		{
			for (;;)                                   // the next free slot
			{
				const uint16_t w = peek((uint16_t)(0x3806 + T.r[2]));
				fl::tst(w);
				tick(0xAE44, 27);
				if (!w)
				{
					tick(0xAE4C, 12);                  // BEQ
					break;
				}
				tick(0xAE46, 12);
				T.r[2] = fl::add(T.r[2], 0x0008);
				tick(0xAE4A, 18);
				tick(0xAE40, 12);                      // BR
			}
			fl::tst(T.r[1]);
			tick(0xAE4E, 12);
			if (T.r[1])
			{
				tick(0xAE54, 12);                      // BNE: chained to the one before
				poke((uint16_t)(0x3806 + T.r[0]), T.r[2]);
				fl::mov(T.r[2]);
				tick(0xAE58, 30);
			}
			else
			{
				tick(0xAE50, 12);
				T.r[1] = T.r[2];
				fl::mov(T.r[1]);
				tick(0xAE52, 12);
				tick(0xAE58, 12);                      // BR
			}
			static const uint16_t cleared[3] = {0x3800, 0x3C00, 0x3802};
			for (int i = 0; i < 3; i++)
			{
				poke((uint16_t)(cleared[i] + T.r[2]), 0);
				fl::clr();
				tick((uint16_t)(0xAE5C + 4 * i), 30);
			}
			T.r[0] = T.r[2];
			fl::mov(T.r[0]);
			tick(0xAE66, 12);
			T.r[2] = fl::add(T.r[2], 0x0008);
			tick(0xAE6A, 18);
			if (--T.r[3] == 0)
			{
				tick(0xAE6C, 18);                      // SOB
				break;
			}
			tick(0xAE40, 18);
		}
	}
	T.r[3] = T.r[4];
	fl::mov(T.r[3]);
	tick(0xAE6E, 12);
	T.r[3] = fl::asl(T.r[3]);
	tick(0xAE70, 12);
	poke((uint16_t)(0x0888 + T.r[3]), T.r[1]);         // the first and the last slot
	fl::mov(T.r[1]);
	tick(0xAE74, 30);
	T.r[2] = T.r[0];
	fl::mov(T.r[2]);
	tick(0xAE76, 12);
	poke((uint16_t)(0x0908 + T.r[3]), T.r[2]);
	fl::mov(T.r[2]);
	tick(0xAE7A, 30);
	poke((uint16_t)(0x3806 + T.r[0]), 0x03F9);
	fl::mov(0x03F9);
	tick(0xAE80, 36);
	T.r[2] = T.r[4];
	fl::mov(T.r[2]);
	tick(0xAE82, 12);
	T.r[3] = 0xFFFF;                                   // the one before: none yet
	fl::mov(T.r[3]);
	tick(0xAE86, 18);
	T.r[1] = peek((uint16_t)(T.r[6] + 0x10));
	fl::mov(T.r[1]);
	tick(0xAE8A, 27);
	T.r[1] = sext8(peekb((uint16_t)(0x0988 + T.r[1])));   // the head of the list
	fl::movb((uint8_t)T.r[1]);
	tick(0xAE8E, 27);
	for (;;)
	{
		if ((int16_t)T.r[1] < 0)
		{
			tick(0xAEFA, 12);                          // BLT: at the end of the list
			break;
		}
		tick(0xAE90, 12);
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0xAE92, 12);
		jsr(0xAE96, 0xAD70);                           // sprite_in_front
		const bool front = (T.r[0] & 1) != 0;
		T.r[0] = fl::ror(T.r[0]);
		tick(0xAE98, 12);
		if (front)
		{
			tick(0xAE9A, 12);                          // in before R1
			fl::tst(T.r[3]);
			tick(0xAE9C, 12);
			if ((int16_t)T.r[3] >= 0)
			{
				tick(0xAE9E, 12);
				store_b(2, (uint16_t)(0x0808 + T.r[3]), 0xAEA2);
				relink(0xAEA2, 3, 2);
				tick(0xAEC8, 12);                      // BR
			}
			else
			{
				tick(0xAEC0, 12);                      // BLT: the new head
				T.r[0] = peek((uint16_t)(T.r[6] + 0x10));
				fl::mov(T.r[0]);
				tick(0xAEC4, 27);
				store_b(2, (uint16_t)(0x0988 + T.r[0]), 0xAEC8);
			}
			store_b(2, (uint16_t)(0x0848 + T.r[1]), 0xAECC);
			store_b(1, (uint16_t)(0x0808 + T.r[2]), 0xAED0);
			relink(0xAED0, 2, 1);
			store_b(3, (uint16_t)(0x0848 + T.r[2]), 0xAEF0);
			c_restore(4);
			return T.r[0];
		}
		tick(0xAEF2, 12);                              // BCC: on along the list
		T.r[3] = T.r[1];
		fl::mov(T.r[3]);
		tick(0xAEF4, 12);
		T.r[1] = sext8(peekb((uint16_t)(0x0808 + T.r[1])));
		fl::movb((uint8_t)T.r[1]);
		tick(0xAEF8, 27);
		tick(0xAE8E, 12);                              // BR
	}
	fl::tst(T.r[3]);                                   // the last one
	tick(0xAEFC, 12);
	if ((int16_t)T.r[3] >= 0)
	{
		tick(0xAEFE, 12);
		store_b(2, (uint16_t)(0x0808 + T.r[3]), 0xAF02);
		relink(0xAF02, 3, 2);
		tick(0xAF28, 12);                              // BR
	}
	else
	{
		tick(0xAF20, 12);                              // BLT: the only one
		T.r[0] = peek((uint16_t)(T.r[6] + 0x10));
		fl::mov(T.r[0]);
		tick(0xAF24, 27);
		store_b(2, (uint16_t)(0x0988 + T.r[0]), 0xAF28);
	}
	store_b(3, (uint16_t)(0x0848 + T.r[2]), 0xAF2C);
	store_b(1, (uint16_t)(0x0808 + T.r[2]), 0xAF30);
	c_restore(4);
	return T.r[0];
}

// F:AF32 (assembly): one bubble pass over the draw list 0x0993: an object that F:AD70 does
// not put in front of its successor swaps places with it (list and sprite links updated).
// A word on the stack holds the doubled index of the object moving back.
void sort_sprite_list()
{
	T.r[6] = (uint16_t)(T.r[6] - 2);                   // a local word
	fl::tst(peek(T.r[6]));
	tick(0xAF34, 21);
	T.r[0] = sext8(peekb(0x0993));                     // the list
	fl::movb((uint8_t)T.r[0]);
	tick(0xAF38, 24);
	T.r[3] = sext8(peekb((uint16_t)(0x0988 + T.r[0])));
	fl::movb((uint8_t)T.r[3]);
	tick(0xAF3C, 27);
	if ((int16_t)T.r[3] < 0)
		tick(0xAFEC, 12);                              // BLT: empty
	else
	{
		tick(0xAF3E, 12);
		T.r[1] = sext8(peekb((uint16_t)(0x0808 + T.r[3])));
		fl::movb((uint8_t)T.r[1]);
		tick(0xAF42, 27);
		for (;;)
		{
			if ((int16_t)T.r[1] < 0)
			{
				tick(0xAFEC, 12);                      // BLT: at the end
				break;
			}
			tick(0xAF44, 12);
			T.r[0] = T.r[3];
			fl::mov(T.r[0]);
			tick(0xAF46, 12);
			jsr(0xAF4A, 0xAD70);                       // sprite_in_front
			const bool front = (T.r[0] & 1) != 0;
			T.r[0] = fl::ror(T.r[0]);
			tick(0xAF4C, 12);
			if (front)
				tick(0xAFE4, 12);                      // BCS: in order
			else
			{
				// R1 goes in before R3
				tick(0xAF4E, 12);
				T.r[2] = sext8(peekb((uint16_t)(0x0848 + T.r[3])));
				fl::movb((uint8_t)T.r[2]);
				tick(0xAF52, 27);
				if ((int16_t)T.r[2] >= 0)
				{
					tick(0xAF54, 12);
					store_b(1, (uint16_t)(0x0808 + T.r[2]), 0xAF58);
					relink(0xAF58, 2, 1);
					tick(0xAF7E, 12);                  // BR
				}
				else
				{
					tick(0xAF76, 12);                  // BLT: the new head
					T.r[0] = sext8(peekb(0x0993));
					fl::movb((uint8_t)T.r[0]);
					tick(0xAF7A, 24);
					store_b(1, (uint16_t)(0x0988 + T.r[0]), 0xAF7E);
				}
				store_b(2, (uint16_t)(0x0848 + T.r[1]), 0xAF82);
				T.r[2] = sext8(peekb((uint16_t)(0x0808 + T.r[1])));
				fl::movb((uint8_t)T.r[2]);
				tick(0xAF86, 27);
				poke(T.r[6], T.r[3]);
				fl::mov(T.r[3]);
				tick(0xAF88, 21);
				poke(T.r[6], fl::asl(peek(T.r[6])));
				tick(0xAF8A, 21);
				T.r[4] = peek(T.r[6]);
				fl::mov(T.r[4]);
				tick(0xAF8C, 18);
				T.r[0] = peek((uint16_t)(0x0908 + T.r[4]));
				fl::mov(T.r[0]);
				tick(0xAF90, 27);
				T.r[5] = 0x3806;
				fl::mov(T.r[5]);
				tick(0xAF94, 18);
				T.r[5] = fl::add(T.r[5], T.r[0]);
				tick(0xAF96, 12);
				fl::tst(T.r[2]);
				tick(0xAF98, 12);
				if ((int16_t)T.r[2] >= 0)
				{
					tick(0xAF9A, 12);                  // R3 links to the one after R1
					store_b(3, (uint16_t)(0x0848 + T.r[2]), 0xAF9E);
					T.r[0] = peek(T.r[5]);
					fl::mov(T.r[0]);
					tick(0xAFA0, 18);
					T.r[0] = fl::logic((uint16_t)(T.r[0] & ~0x07FE));
					tick(0xAFA4, 18);
					T.r[4] = T.r[2];
					fl::mov(T.r[4]);
					tick(0xAFA6, 12);
					T.r[4] = fl::asl(T.r[4]);
					tick(0xAFA8, 12);
					T.r[0] = fl::logic((uint16_t)(T.r[0] | peek((uint16_t)(0x0888 + T.r[4]))));
					tick(0xAFAC, 27);
					poke(T.r[5], T.r[0]);
					fl::mov(T.r[0]);
					tick(0xAFAE, 21);
					tick(0xAFBC, 12);                  // BR
				}
				else
				{
					tick(0xAFB0, 12);                  // BLT: or ends the chain
					T.r[4] = peek(T.r[5]);
					fl::mov(T.r[4]);
					tick(0xAFB2, 18);
					T.r[4] = fl::logic((uint16_t)(T.r[4] & ~0x07FE));
					tick(0xAFB6, 18);
					T.r[4] = fl::logic((uint16_t)(T.r[4] | 0x03F8));
					tick(0xAFBA, 18);
					poke(T.r[5], T.r[4]);
					fl::mov(T.r[4]);
					tick(0xAFBC, 21);
				}
				store_b(2, (uint16_t)(0x0808 + T.r[3]), 0xAFC0);
				store_b(3, (uint16_t)(0x0808 + T.r[1]), 0xAFC4);
				T.r[5] = T.r[1];                       // R1's sprites link to R3's
				fl::mov(T.r[5]);
				tick(0xAFC6, 12);
				T.r[5] = fl::asl(T.r[5]);
				tick(0xAFC8, 12);
				T.r[4] = peek((uint16_t)(0x0908 + T.r[5]));
				fl::mov(T.r[4]);
				tick(0xAFCC, 27);
				T.r[5] = peek((uint16_t)(0x3806 + T.r[4]));
				fl::mov(T.r[5]);
				tick(0xAFD0, 27);
				T.r[5] = fl::logic((uint16_t)(T.r[5] & ~0x07FE));
				tick(0xAFD4, 18);
				T.r[0] = peek(T.r[6]);
				fl::mov(T.r[0]);
				tick(0xAFD6, 18);
				T.r[5] = fl::logic((uint16_t)(T.r[5] | peek((uint16_t)(0x0888 + T.r[0]))));
				tick(0xAFDA, 27);
				poke((uint16_t)(0x3806 + T.r[4]), T.r[5]);
				fl::mov(T.r[5]);
				tick(0xAFDE, 30);
				store_b(1, (uint16_t)(0x0848 + T.r[3]), 0xAFE2);
				T.r[1] = T.r[3];
				fl::mov(T.r[1]);
				tick(0xAFE4, 12);
			}
			T.r[3] = T.r[1];                           // on along the list
			fl::mov(T.r[3]);
			tick(0xAFE6, 12);
			T.r[1] = sext8(peekb((uint16_t)(0x0808 + T.r[1])));
			fl::movb((uint8_t)T.r[1]);
			tick(0xAFEA, 27);
			tick(0xAF42, 12);                          // BR
		}
	}
	drop(0xAFEC);
	rts();
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
	c_save(2, 0x9FB8);
	T.r[1] = peek(0x01D2);                             // the banks, put back at the end
	fl::mov(T.r[1]);
	tick(0x9FBC, 24);
	T.r[2] = peek(0x01D4);
	fl::mov(T.r[2]);
	tick(0x9FC0, 24);
	clr_b(0x01EA, 0x9FC4);
	bank_a(0x9FC4, 0x3800);
	bank_b(0x9FD0, 0x3C00);
	const uint8_t park = peekb(0x0F59);
	fl::cmpb(park, 0x04);
	tick(0x9FE2, 30);
	if (park == 4)
		tick(0x9FE8, 12);                              // BEQ
	else
	{
		tick(0x9FE4, 12);
		jsr(0x9FE8, 0x7A9E);                           // snapshot_skater
	}
	const uint16_t speed = peek(0x01B4);
	fl::tst(speed);
	tick(0x9FEC, 24);
	if ((int16_t)speed >= 0)
		tick(0xA000, 12);                              // BGE
	else
	{
		tick(0x9FEE, 12);                              // backwards: the opposite direction
		turn_round(0x9FEE, 3, 0x031D);
		poke(0x01B4, fl::neg(peek(0x01B4)));
		tick(0xA000, 27);
	}
	copy_w(0x01B4, 0x02E8, 0xA006);
	jsr(0xA00A, 0x9B82);                               // skater_motion_params
	jsr(0xA00E, 0x91A6);                               // skater_velocity
	const uint8_t carried = peekb(0x0EA0);
	fl::tstb(carried);
	tick(0xA012, 24);
	if (!carried)
		tick(0xA018, 12);                              // BEQ
	else
	{
		tick(0xA014, 12);
		jsr(0xA018, 0xD742);
	}
	push_w(0x01B4, 0xA01C);                            // speed * jump factor
	push_w(0x02EA, 0xA020);
	jsr(0xA024, 0x80FA);                               // mul32
	copy_b(0x0206, 0x01B5, 0xA02A);
	copy_b(0x0205, 0x01B4, 0xA030);
	{
		const uint16_t v = peek(0x01B4);               // speed * z factor
		fl::mov(v);
		poke(T.r[6], v);
	}
	tick(0xA034, 33);
	push_w(0x02EC, 0xA038);
	jsr(0xA03C, 0x80FA);                               // mul32
	copy_b(0x0205, 0x07F8, 0xA042);
	copy_w(0x0206, 0x05B0, 0xA048);
	const uint8_t carried2 = peekb(0x0EA0);
	fl::tstb(carried2);
	tick(0xA04C, 24);
	if (!carried2)
		tick(0xA052, 12);                              // BEQ
	else
	{
		tick(0xA04E, 12);
		jsr(0xA052, 0xD6BE);                           // slope_of_object_0ea0
	}
	clr_b(0x0323, 0xA056);                             // the air timers
	copy_w(0x01C6, 0x02F6, 0xA05C);
	poke(0x02F6, fl::asl(peek(0x02F6)));
	tick(0xA060, 27);
	clr_w(0x02F4, 0xA064);
	clr_w(0x02F2, 0xA068);
	set_w(0x02EE, t11rt::g_patch_gravity, 0xA06E);     // patch point (default 12)
	poke(T.r[6], 0x004A);
	fl::mov(0x004A);
	tick(0xA072, 27);
	jsr(0xA076, 0x8E78);                               // queue_sound(0x4A)
	poke(0x01D2, T.r[1]);
	fl::mov(T.r[1]);
	tick(0xA07A, 27);
	poke(0x01D4, T.r[2]);
	fl::mov(T.r[2]);
	tick(0xA07E, 27);
	copy_w(0x01D2, 0x1400, 0xA084);
	copy_w(0x01D4, 0x1402, 0xA08A);
	T.r[6] = fl::add(T.r[6], 0x0006);
	tick(0xA08E, 18);
	c_restore(2);
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
	c_save(2, 0xA2F4);
	T.r[2] = peek(0x01D2);                             // the banks, put back at the end
	fl::mov(T.r[2]);
	tick(0xA2F8, 24);
	T.r[3] = peek(0x01D4);
	fl::mov(T.r[3]);
	tick(0xA2FC, 24);
	bank_b(0xA2FC, 0x2C00);
	jsr(0xA30C, 0xE238);                               // reset_skater_state
	set_b(0x031F, 0x01, 0xA312);
	T.r[0] = sext8(peekb(0x025E));                     // the friction of the equipment
	fl::movb((uint8_t)T.r[0]);
	tick(0xA316, 24);
	{
		const uint8_t v = peekb((uint16_t)(0x7CAC + T.r[0]));
		fl::movb(v);
		pokeb(0x02FA, v);
	}
	tick(0xA31C, 42);
	clr_b(0x02FB, 0xA320);
	const uint8_t ramp = peekb(0x01EA);
	fl::cmpb(ramp, 0x02);
	tick(0xA326, 30);
	if (ramp != 2)
		tick(0xA32E, 12);                              // BNE
	else
	{
		tick(0xA328, 12);
		set_b(0x01EA, 0x01, 0xA32E);
	}
	set_b(0x0325, 0xFF, 0xA334);
	clr_w(0x02FC, 0xA338);
	clr_w(0x02FE, 0xA33C);
	clr_w(0x01DC, 0xA340);
	const uint8_t ramp2 = peekb(0x01EA);
	fl::tstb(ramp2);
	tick(0xA344, 24);
	if (ramp2)
		tick(0xA35E, 12);                              // BNE
	else
	{
		tick(0xA346, 12);
		set_w(0x01B4, 0x0160, 0xA34C);
		const uint16_t vz = peek(0x05B2);
		fl::tst(vz);
		tick(0xA350, 24);
		if ((int16_t)vz <= 0)
			tick(0xA356, 12);                          // BLE
		else
		{
			tick(0xA352, 12);
			clr_w(0x05B2, 0xA356);
		}
		jsr(0xA35A, 0x9B82);                           // skater_motion_params
		jsr(0xA35E, 0x91A6);                           // skater_velocity
	}
	const uint16_t speed = peek(0x01B4);
	fl::tst(speed);
	tick(0xA362, 24);
	if ((int16_t)speed >= 0)
		tick(0xA376, 12);                              // BGE
	else
	{
		tick(0xA364, 12);                              // the speed made positive, turned round
		poke(0x01B4, fl::neg(peek(0x01B4)));
		tick(0xA368, 27);
		turn_round(0xA368, 1, 0x031D);
	}
	T.r[1] = peek(0x01B4);
	fl::mov(T.r[1]);
	tick(0xA37A, 24);
	T.r[0] = sext8(peekb(0x025D));                     // the board
	fl::movb((uint8_t)T.r[0]);
	tick(0xA37E, 24);
	T.r[0] = fl::asl(T.r[0]);
	tick(0xA380, 12);
	bool limit = false;
	{
		const uint16_t top = peek((uint16_t)(0x7C90 + T.r[0]));
		fl::cmp(T.r[1], top);
		tick(0xA384, 27);
		if ((int16_t)T.r[1] < (int16_t)top)
			tick(0xA3D0, 12);                          // BLT
		else
		{
			tick(0xA386, 12);
			const uint8_t slope = fl::logicb((uint8_t)(peekb(0x0327) & 0x07));
			tick(0xA38C, 30);
			limit = !slope;
			tick(limit ? 0xA38E : 0xA3D0, 12);         // BNE
		}
	}
	if (limit)
	{
		{
			const uint16_t v = peek((uint16_t)(0x7C90 + T.r[0]));   // held at the limit
			fl::mov(v);
			poke(0x01B4, v);
		}
		tick(0xA394, 42);
		clr_w(0x01D2, 0xA398);
		clr_w(0x1400, 0xA39C);
		bank_b(0xA39C, 0x0400);
		T.r[0] = sext8(peekb(0x031D));
		fl::movb((uint8_t)T.r[0]);
		tick(0xA3AC, 24);
		T.r[0] = fl::asl(T.r[0]);
		tick(0xA3AE, 12);
		{
			const uint16_t v = peek((uint16_t)(0x4488 + T.r[0]));
			fl::mov(v);
			poke(0x03B4, v);
		}
		tick(0xA3B4, 42);
		T.r[0] = 0x0006;
		fl::mov(T.r[0]);
		tick(0xA3B8, 18);
		pokeb(0x0792, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0xA3BC, 27);
		pokeb(0x0772, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0xA3C0, 27);
		jsr(0xA3C4, 0x9152);                           // next_random
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0003));
		tick(0xA3C8, 18);
		push(0);
		fl::clr();
		tick(0xA3CA, 24);
		{
			const uint8_t v = peekb((uint16_t)(0xFCF6 + T.r[0]));   // a random sound
			fl::movb(v);
			pokeb(T.r[6], v);
		}
		tick(0xA3CE, 36);
		tick(0xA42C, 12);                              // BR
	}
	else
	{
		T.r[0] = sext8(peekb(0x01F8));
		fl::movb((uint8_t)T.r[0]);
		tick(0xA3D4, 24);
		T.r[0] = fl::asl(T.r[0]);
		tick(0xA3D6, 12);
		T.r[0] = fl::add(T.r[0], 0x4428);
		tick(0xA3DA, 18);
		poke(0x03B4, T.r[0]);
		fl::mov(T.r[0]);
		tick(0xA3DE, 27);
		const uint8_t park = peekb(0x0F59);
		fl::cmpb(park, 0x03);
		tick(0xA3E4, 30);
		bool held = false;
		if (park != 3)
			tick(0xA40E, 12);                          // BNE
		else
		{
			tick(0xA3E6, 12);                          // park 3: on a slope, by the z velocity
			T.r[0] = sext8(peekb(0x0327));
			fl::movb((uint8_t)T.r[0]);
			tick(0xA3EA, 24);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0007));
			tick(0xA3EE, 18);
			fl::cmp(T.r[0], 0x0005);
			tick(0xA3F2, 18);
			if (T.r[0] < 5)
				tick(0xA40E, 12);                      // BCS
			else
			{
				tick(0xA3F4, 12);
				auto fall = [](uint16_t pc) {
					T.r[0] = peek(0x05B2);
					fl::mov(T.r[0]);
					tick((uint16_t)(pc + 4), 24);
					T.r[0] = fl::asl(T.r[0]);
					tick((uint16_t)(pc + 6), 12);
					T.r[0] = fl::neg(T.r[0]);
					tick((uint16_t)(pc + 8), 12);
				};
				fall(0xA3F4);
				fl::cmp(T.r[1], T.r[0]);
				tick(0xA3FE, 12);
				if ((int16_t)T.r[1] >= (int16_t)T.r[0])
					tick(0xA40E, 12);                  // BGE
				else
				{
					tick(0xA400, 12);
					fall(0xA400);
					poke(0x01B4, T.r[0]);
					fl::mov(T.r[0]);
					tick(0xA40C, 27);
					tick(0xA428, 12);                  // BR
					held = true;
				}
			}
		}
		if (!held)
		{
			fl::cmp(T.r[1], 0x0180);                   // 0x180-0x280
			tick(0xA412, 18);
			if ((int16_t)T.r[1] < 0x0180)
			{
				tick(0xA414, 12);
				set_w(0x01B4, 0x0180, 0xA41A);
				tick(0xA428, 12);                      // BR
			}
			else
			{
				tick(0xA41C, 12);                      // BGE
				fl::cmp(T.r[1], 0x0280);
				tick(0xA420, 18);
				if ((int16_t)T.r[1] <= 0x0280)
					tick(0xA428, 12);                  // BLE
				else
				{
					tick(0xA422, 12);
					set_w(0x01B4, 0x0280, 0xA428);
				}
			}
		}
		push(0x0054);
		fl::mov(0x0054);
		tick(0xA42C, 30);
	}
	jsr(0xA430, 0x8E78);                               // queue_sound
	copy_b(0x031D, 0x01F8, 0xA436);
	set_w(0x03B0, 0x50FA, 0xA43C);
	set_w(0x03B2, 0x50FA, 0xA442);
	poke(0x01D2, T.r[2]);
	fl::mov(T.r[2]);
	tick(0xA446, 27);
	poke(0x01D4, T.r[3]);
	fl::mov(T.r[3]);
	tick(0xA44A, 27);
	copy_w(0x01D2, 0x1400, 0xA450);
	copy_w(0x01D4, 0x1402, 0xA456);
	drop(0xA456);
	c_restore(2);
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
uint16_t skater_touch_down(const Args &)
{
	c_save(3, 0xA094);
	bank_b(0xA094, 0x2C00);
	const uint8_t park = peekb(0x0F59);
	fl::cmpb(park, 0x04);
	tick(0xA0A6, 30);
	if (park != 4)
		tick(0xA0E8, 12);                              // BNE
	else
	{
		tick(0xA0A8, 12);
		const uint8_t done = peekb(0x01F2);
		fl::tstb(done);
		tick(0xA0AC, 24);
		if (done)
			tick(0xA0E8, 12);                          // BNE
		else
		{
			tick(0xA0AE, 12);                          // park 4: the slots refilled for the level
			bank_a(0xA0AE, 0x2400);
			jsr(0xA0BE, 0x90BA);                       // park_level
			T.r[3] = 0;
			fl::clr();
			tick(0xA0C0, 12);
			T.r[3] = (uint16_t)((T.r[3] & 0xFF00) | fl::logicb((uint8_t)(T.r[3] | peekb((uint16_t)(0x4579 + T.r[0])))));
			tick(0xA0C4, 27);
			T.r[1] = 0;
			fl::clr();
			tick(0xA0C6, 12);
			T.r[1] = (uint16_t)((T.r[1] & 0xFF00) | fl::logicb((uint8_t)(T.r[1] | peekb(0x0FE0))));
			tick(0xA0CA, 24);
			T.r[3] = fl::add(T.r[3], T.r[1]);
			tick(0xA0CC, 12);
			T.r[0] = T.r[3];
			fl::mov(T.r[0]);
			tick(0xA0CE, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0xA0D0, 12);
			static const uint16_t table[3] = {0x7E50, 0x7E8A, 0x7EC4};
			for (int i = 0; i < 3; i++)
			{
				const uint16_t v = peek((uint16_t)(table[i] + T.r[0]));
				fl::mov(v);
				push(v);
				tick((uint16_t)(0xA0D4 + 4 * i), 39);
			}
			push(0x0078);
			fl::mov(0x0078);
			tick(0xA0E0, 30);
			jsr(0xA0E4, 0xE1FA);                       // fill_slots
			T.r[6] = fl::add(T.r[6], 0x0008);
			tick(0xA0E8, 18);
		}
	}
	bank_a(0xA0E8, 0x1800);
	jsr(0xA0F8, 0x51BA);                               // copy_object0_position
	T.r[0] = sext8(peekb(0x0EA0));                     // minus the carrier's velocity
	fl::movb((uint8_t)T.r[0]);
	tick(0xA0FC, 24);
	if (!T.r[0])
		tick(0xA10C, 12);                              // BEQ
	else
	{
		tick(0xA0FE, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0xA100, 12);
		poke(0x04F0, fl::sub(peek(0x04F0), peek((uint16_t)(0x04F0 + T.r[0]))));
		tick(0xA106, 42);
		poke(0x0570, fl::sub(peek(0x0570), peek((uint16_t)(0x0570 + T.r[0]))));
		tick(0xA10C, 42);
	}
	T.r[3] = sext8(peekb(0x0327));                     // the old terrain, put back at the end
	fl::movb((uint8_t)T.r[3]);
	tick(0xA110, 24);
	{
		const uint8_t v = peekb((uint16_t)(T.r[6] + 0x0C));
		fl::movb(v);
		pokeb(0x0327, v);
	}
	tick(0xA116, 42);
	set_b(0x01EA, 0x01, 0xA11C);
	const uint8_t kick = peekb(0x0330);
	fl::cmpb(kick, 0x40);
	tick(0xA122, 30);
	if (kick != 0x40)
		tick(0xA128, 12);                              // BNE
	else
	{
		tick(0xA124, 12);
		jsr(0xA128, 0xE238);                           // reset_skater_state
	}
	T.r[0] = sext8(peekb(0x0327));
	fl::movb((uint8_t)T.r[0]);
	tick(0xA12C, 24);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0007));
	tick(0xA130, 18);
	fl::cmp(T.r[0], 0x0005);
	tick(0xA134, 18);
	if (T.r[0] < 5)
		tick(0xA150, 12);                              // BCS
	else
	{
		tick(0xA136, 12);                              // a corner code sets the direction
		T.r[0] = sext8(peekb(0x0327));
		fl::movb((uint8_t)T.r[0]);
		tick(0xA13A, 24);
		for (uint16_t pc = 0xA13C; pc <= 0xA140; pc += 2)
		{
			T.r[0] = fl::asr(T.r[0]);
			tick(pc, 12);
		}
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0003));
		tick(0xA144, 18);
		{
			const uint8_t v = peekb((uint16_t)(0xFCD0 + T.r[0]));
			fl::movb(v);
			pokeb(0x031D, v);
		}
		tick(0xA14A, 42);
		set_b(0x0325, 0x7F, 0xA150);
	}
	const uint8_t pending = peekb(0x0323);
	fl::cmpb(pending, 0x01);
	tick(0xA156, 30);
	if (pending == 1)
	{
		tick(0xA158, 12);                              // a pending landing
		jsr(0xA15C, 0xA2F0);                           // skater_land
		bank_b(0xA15C, 0x2800);
		push(0x0001);
		fl::mov(0x0001);
		tick(0xA16C, 30);
		jsr(0xA170, 0x7E54);
		clr_b(0x0323, 0xA174);
		tick(0xA262, 12);                              // BR
		tick(0xA29A, 12);                              // BR
		drop(0xA29A);
		goto put_back;
	}
	tick(0xA176, 12);                                  // BNE
	{
		const uint8_t mode = peekb(0x031F);
		fl::cmpb(mode, 0x03);
		tick(0xA17C, 30);
		bool landed = mode == 3;
		if (!landed)
		{
			tick(0xA17E, 12);
			fl::cmpb(mode, 0x01);
			tick(0xA184, 30);
			landed = mode == 1;
		}
		if (landed)
		{
			tick(0xA210, 12);                          // BEQ
			tick(0xA29C, 12);                          // BR
			goto put_back;
		}
	}
	tick(0xA186, 12);
	T.r[0] = sext8(peekb(0x01F8));
	fl::movb((uint8_t)T.r[0]);
	tick(0xA18A, 24);
	push(T.r[0]);
	fl::mov(T.r[0]);
	tick(0xA18C, 24);
	jsr(0xA190, 0xA2BE);                               // direction_bit
	drop(0xA190);
	fl::tst(T.r[0]);
	tick(0xA194, 12);
	if (!T.r[0])
	{
		tick(0xA196, 12);                              // he turns to 0x01F8
		T.r[1] = sext8(peekb(0x031D));
		fl::movb((uint8_t)T.r[1]);
		tick(0xA19A, 24);
		copy_b(0x01F8, 0x031D, 0xA1A0);
		jsr(0xA1A4, 0x9B82);                           // skater_motion_params
		bank_a(0xA1A4, 0x3800);
		jsr(0xA1B4, 0x410C);
		const uint8_t state = peekb(0x031E);
		fl::tstb(state);
		tick(0xA1B8, 24);
		if (!state)
			tick(0xA1C2, 12);                          // BEQ
		else
		{
			tick(0xA1BA, 12);
			push(T.r[1]);
			fl::mov(T.r[1]);
			tick(0xA1BC, 24);
			jsr(0xA1C0, 0x9EEE);                       // skater_deflect
			drop(0xA1C0);
		}
		const uint8_t park2 = peekb(0x0F59);
		fl::cmpb(park2, 0x03);
		tick(0xA1C8, 30);
		if (park2 == 3)
		{
			tick(0xA29C, 12);                          // BEQ
			goto put_back;
		}
		tick(0xA1CA, 12);
		T.r[1] = peek(0x02E8);                         // the speed before the jump
		fl::mov(T.r[1]);
		tick(0xA1CE, 24);
		// MOV @#0x01B4,R0 at pc, made positive (on at pc + 8)
		auto speed = [](uint16_t pc) {
			T.r[0] = peek(0x01B4);
			fl::mov(T.r[0]);
			tick((uint16_t)(pc + 4), 24);
			if ((int16_t)T.r[0] >= 0)
				tick((uint16_t)(pc + 8), 12);          // BGE
			else
			{
				tick((uint16_t)(pc + 6), 12);
				T.r[0] = fl::neg(T.r[0]);
				tick((uint16_t)(pc + 8), 12);
			}
		};
		const uint8_t slope = fl::logicb((uint8_t)(peekb(0x0327) & 0x07));
		tick(0xA1D4, 30);
		bool faster = false;
		if (!slope)
			tick(0xA1E2, 12);                          // BEQ
		else
		{
			tick(0xA1D6, 12);
			speed(0xA1D6);
			fl::cmp(T.r[1], T.r[0]);
			tick(0xA1E0, 12);
			faster = (int16_t)T.r[1] > (int16_t)T.r[0];
			tick(faster ? 0xA204 : 0xA1E2, 12);        // BGT
		}
		if (!faster)
		{
			speed(0xA1E2);                             // fast for the terrain (0x7BE8)?
			T.r[2] = sext8(peekb(0x0327));
			fl::movb((uint8_t)T.r[2]);
			tick(0xA1EE, 24);
			T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x0007));
			tick(0xA1F2, 18);
			T.r[2] = fl::asl(T.r[2]);
			tick(0xA1F4, 12);
			T.r[2] = fl::asl(T.r[2]);
			tick(0xA1F6, 12);
			T.r[4] = sext8(peekb(0x025C));
			fl::movb((uint8_t)T.r[4]);
			tick(0xA1FA, 24);
			T.r[2] = fl::add(T.r[2], T.r[4]);
			tick(0xA1FC, 12);
			T.r[2] = fl::asl(T.r[2]);
			tick(0xA1FE, 12);
			const uint16_t top = peek((uint16_t)(0x7BE8 + T.r[2]));
			fl::cmp(T.r[0], top);
			tick(0xA202, 27);
			if ((int16_t)T.r[0] <= (int16_t)top)
			{
				tick(0xA29C, 12);                      // BLE
				goto put_back;
			}
			tick(0xA204, 12);
		}
		const uint16_t v = peek(0x01B4);               // the speed from 0x02E8, signed
		fl::tst(v);
		tick(0xA208, 24);
		if ((int16_t)v > 0)
			tick(0xA20C, 12);                          // BGT
		else
		{
			tick(0xA20A, 12);
			T.r[1] = fl::neg(T.r[1]);
			tick(0xA20C, 12);
		}
		poke(0x01B4, T.r[1]);
		fl::mov(T.r[1]);
		tick(0xA210, 27);
		tick(0xA29C, 12);                              // BR
		goto put_back;
	}
	tick(0xA212, 12);                                  // BNE
	jsr(0xA216, 0xE6F0);                               // object1_screen_x
	jsr(0xA21A, 0x9B82);                               // skater_motion_params
	T.r[2] = peek(0x05B2);                             // the z velocity
	fl::mov(T.r[2]);
	tick(0xA21E, 24);
	bank_a(0xA21E, 0x3800);
	jsr(0xA22E, 0x410C);
	T.r[0] = sext8(peekb(0x025D));                     // the board
	fl::movb((uint8_t)T.r[0]);
	tick(0xA232, 24);
	T.r[0] = fl::asl(T.r[0]);
	tick(0xA234, 12);
	{
		bool land = false;
		const uint16_t speed = peek(0x01B4);
		const uint16_t top = peek((uint16_t)(0x7C90 + T.r[0]));
		fl::cmp(speed, top);
		tick(0xA23A, 39);
		if ((int16_t)speed < (int16_t)top)
			tick(0xA264, 12);                          // BLT
		else
		{
			tick(0xA23C, 12);
			const uint16_t fall = peek((uint16_t)(0x7C98 + T.r[0]));
			fl::cmp(T.r[2], fall);
			tick(0xA240, 27);
			if ((int16_t)T.r[2] >= (int16_t)fall)
				tick(0xA264, 12);                      // BGE
			else
			{
				tick(0xA242, 12);
				const uint8_t park3 = peekb(0x0F59);
				fl::cmpb(park3, 0x03);
				tick(0xA248, 30);
				land = park3 != 3;
				tick(land ? 0xA24A : 0xA264, 12);      // BEQ
			}
		}
		if (land)
		{
			jsr(0xA24E, 0xA2F0);                       // skater_land
			bank_b(0xA24E, 0x2800);
			push(0x0002);
			fl::mov(0x0002);
			tick(0xA25E, 30);
			jsr(0xA262, 0x7E54);
			tick(0xA29A, 12);                          // BR
			drop(0xA29A);
			goto put_back;
		}
	}
	T.r[2] = sext8(peekb(0x01F8));                     // the side of the turn
	fl::movb((uint8_t)T.r[2]);
	tick(0xA268, 24);
	T.r[0] = sext8(peekb(0x031D));
	fl::movb((uint8_t)T.r[0]);
	tick(0xA26C, 24);
	T.r[2] = fl::sub(T.r[2], T.r[0]);
	tick(0xA26E, 12);
	if (T.r[2] & 0x8000)
	{
		tick(0xA270, 12);
		set_w(0x0222, 0xFFFF, 0xA276);
		T.r[2] = fl::neg(T.r[2]);
		tick(0xA278, 12);
		tick(0xA280, 12);                              // BR
	}
	else
	{
		tick(0xA27A, 12);                              // BPL
		set_w(0x0222, 0x0001, 0xA280);
	}
	fl::cmp(T.r[2], 0x0008);
	tick(0xA284, 18);
	if ((int16_t)T.r[2] <= 8)
		tick(0xA28A, 12);                              // BLE
	else
	{
		tick(0xA286, 12);
		poke(0x0222, fl::neg(peek(0x0222)));
		tick(0xA28A, 27);
	}
	T.r[0] = sext8(peekb(0x01F8));
	fl::movb((uint8_t)T.r[0]);
	tick(0xA28E, 24);
	push(T.r[0]);
	fl::mov(T.r[0]);
	tick(0xA290, 24);
	push_w(0x0222, 0xA294);
	jsr(0xA298, 0xA45A);                               // skater_phase10
	drop(0xA298);
	drop(0xA29A);
put_back:
	pokeb(0x0327, (uint8_t)T.r[3]);
	fl::movb((uint8_t)T.r[3]);
	tick(0xA2A0, 27);
	const uint8_t mode = peekb(0x031F);
	fl::cmpb(mode, 0x01);
	tick(0xA2A6, 30);
	if (mode == 1)
		tick(0xA2BC, 12);                              // BEQ
	else
	{
		tick(0xA2A8, 12);
		bank_b(0xA2A8, 0x2800);
		push(0);
		fl::clr();
		tick(0xA2B6, 24);
		jsr(0xA2BA, 0x7E54);
		drop(0xA2BA);
	}
	c_restore(3);
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
uint16_t run_text_script(const Args &)
{
	c_save(4, 0xAA00);
	push_w(0x01D2, 0xAA04);                            // the banks, put back at the end
	push_w(0x01D4, 0xAA08);
	bank_b(0xAA08, 0x2800);
	T.r[2] = peek((uint16_t)(T.r[6] + 0x12));          // the script
	fl::mov(T.r[2]);
	tick(0xAA18, 27);
	{
		const uint16_t v = peek(T.r[2]);
		fl::cmp(v, 0x4000);
		tick(0xAA1C, 24);
		if ((int16_t)v >= 0x4000)
		{
			tick(0xAA1E, 12);                          // the screen part: a tile block
			{
				const uint16_t w = peek(T.r[2]);
				fl::mov(w);
				push(w);
			}
			tick(0xAA20, 30);
			push(0);
			fl::clr();
			tick(0xAA22, 24);
			jsr(0xAA26, 0xEAD4);                       // draw_tile_block
			T.r[2] = fl::add(T.r[2], 0x0002);
			tick(0xAA2A, 18);
			tick(0xAA44, 12);                          // BR
		}
		else
		{
			tick(0xAA2C, 12);                          // BLT
			bank_a(0xAA2C, 0x3800);
			push(T.r[2]);
			fl::mov(T.r[2]);
			tick(0xAA3A, 24);
			push(0);
			fl::clr();
			tick(0xAA3C, 24);
			jsr(0xAA40, 0x4D30);
			T.r[2] = fl::add(T.r[2], 0x0004);
			tick(0xAA44, 18);
		}
	}
	// CLR Rr at pc; BISB src,Rr (cyc cycles): the byte at a, on at pc + 6
	auto load = [](uint16_t pc, int r, uint16_t a, int cyc) {
		T.r[r] = 0;
		fl::clr();
		tick((uint16_t)(pc + 2), 12);
		T.r[r] = (uint16_t)(T.r[r] | fl::logicb(peekb(a)));
		tick((uint16_t)(pc + 6), cyc);
	};
	// CMPB @#0x0F2B,#2 at pc ...: the level class bits of the current rating entry (0x0262 by
	// 0x0F31, one less unless 0x0F2B is 2) in R0, on at pc + 0x30
	auto rating_class = [&load](uint16_t pc) {
		const uint8_t mode = peekb(0x0F2B);
		fl::cmpb(mode, 0x02);
		tick((uint16_t)(pc + 6), 30);
		if (mode == 2)
		{
			tick((uint16_t)(pc + 8), 12);
			load((uint16_t)(pc + 8), 0, 0x0F31, 24);
			tick((uint16_t)(pc + 0x18), 12);           // BR
		}
		else
		{
			tick((uint16_t)(pc + 0x10), 12);           // BNE
			load((uint16_t)(pc + 0x10), 0, 0x0F31, 24);
			T.r[0] = fl::dec(T.r[0]);
			tick((uint16_t)(pc + 0x18), 12);
		}
		T.r[0] = sext8(peekb((uint16_t)(0x0262 + T.r[0])));
		fl::movb((uint8_t)T.r[0]);
		tick((uint16_t)(pc + 0x1C), 27);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
		tick((uint16_t)(pc + 0x20), 18);
		for (int i = 1; i <= 6; i++)
		{
			T.r[0] = fl::asr(T.r[0]);
			tick((uint16_t)(pc + 0x20 + 2 * i), 12);
		}
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0003));
		tick((uint16_t)(pc + 0x30), 18);
	};
	// MOV #v,-(SP) / CLR -(SP) and the step to next
	auto arg = [](uint16_t v, uint16_t next) {
		push(v);
		if (v)
			fl::mov(v);
		else
			fl::clr();
		tick(next, v ? 30 : 24);
	};
	// MOV R1,(SP) at pc: the number into the slot of the screen part
	auto number = [](uint16_t pc) {
		poke(T.r[6], T.r[1]);
		fl::mov(T.r[1]);
		tick((uint16_t)(pc + 2), 21);
	};
	for (;;)
	{
		// 0xAA44: the next command
		{
			const uint16_t v = peek(T.r[2]);
			T.r[2] = (uint16_t)(T.r[2] + 2);
			fl::mov(v);
			poke(0x020C, v);
		}
		tick(0xAA48, 33);
		const uint16_t cmd = peek(0x020C);
		fl::cmp(cmd, 0xFFFF);
		tick(0xAA4E, 30);
		if (cmd == 0xFFFF)
		{
			tick(0xAA50, 12);                          // the end
			tick(0xACC8, 18);                          // JMP
			break;
		}
		tick(0xAA54, 12);
		load(0xAA54, 3, 0x020C, 24);
		if (!(T.r[3] & 0x80))
		{
			// a string, drawn by the high byte
			tick(0xAA5C, 12);
			load(0xAA5C, 0, 0x020D, 24);
			T.r[0] = fl::asl(T.r[0]);
			tick(0xAA64, 12);
			const uint16_t at = fl::add(0xAA68, peek((uint16_t)(0xFD36 + T.r[0])));
			tick(at, 27);                              // ADD 0xFD36(R0),PC
			switch (at)
			{
			case 0xAA68:                               // 0: large
				T.r[0] = T.r[3];
				fl::mov(T.r[0]);
				tick(0xAA6A, 12);
				tick(0xAB02, 12);                      // BR
				break;
			case 0xAA6C:                               // 1: small
				T.r[0] = T.r[3];
				fl::mov(T.r[0]);
				tick(0xAA6E, 12);
				jsr(0xAA72, 0x8B04);                   // draw_string_small
				tick(0xAA44, 12);                      // BR
				continue;
			case 0xAA74:                               // 2: by the level class
				rating_class(0xAA74);
				T.r[0] = fl::add(T.r[0], T.r[3]);
				tick(0xAAA6, 12);
				T.r[0] = fl::dec(T.r[0]);
				tick(0xAAA8, 12);
				tick(0xAB02, 12);                      // BR
				break;
			case 0xAAAA:                               // 3-5: by A?:5326, 0x02B8, 0x0261
			case 0xAAB2:
			case 0xAABA:
				if (at == 0xAAAA)
					jsr(0xAAAE, 0x5326);
				else
				{
					load(at, 0, at == 0xAAB2 ? 0x02B8 : 0x0261, 24);
					tick(0xAAAE, 12);                  // BR
				}
				T.r[0] = fl::add(T.r[0], T.r[3]);
				tick(0xAAB0, 12);
				tick(0xAB02, 12);                      // BR
				break;
			case 0xAAC2:                               // 6: a random pair
				jsr(0xAAC6, 0x9152);                   // next_random
				T.r[1] = T.r[0];
				fl::mov(T.r[1]);
				tick(0xAAC8, 12);
				T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x0001));
				tick(0xAACC, 18);
				T.r[4] = T.r[1];
				fl::mov(T.r[4]);
				tick(0xAACE, 12);
				T.r[4] = fl::asl(T.r[4]);
				tick(0xAAD0, 12);
				load(0xAAD0, 0, 0x020C, 24);
				T.r[0] = fl::add(T.r[0], T.r[4]);
				tick(0xAAD8, 12);
				jsr(0xAADC, 0x8AB6);                   // draw_string_large
				load(0xAADC, 0, 0x020C, 24);
				T.r[0] = fl::add(T.r[0], T.r[4]);
				tick(0xAAE4, 12);
				tick(0xAB00, 12);                      // BR
				T.r[0] = fl::inc(T.r[0]);
				tick(0xAB02, 12);
				break;
			case 0xAAE6:                               // 7: the pair by 0x020A
				T.r[1] = peek(0x020A);
				fl::mov(T.r[1]);
				tick(0xAAEA, 24);
				T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x0001));
				tick(0xAAEE, 18);
				T.r[1] = fl::asl(T.r[1]);
				tick(0xAAF0, 12);
				T.r[0] = T.r[1];
				fl::mov(T.r[0]);
				tick(0xAAF2, 12);
				T.r[0] = fl::add(T.r[0], T.r[3]);
				tick(0xAAF4, 12);
				jsr(0xAAF8, 0x8AB6);                   // draw_string_large
				load(0xAAF8, 0, 0x020C, 24);
				T.r[0] = fl::add(T.r[0], T.r[1]);
				tick(0xAB00, 12);
				T.r[0] = fl::inc(T.r[0]);
				tick(0xAB02, 12);
				break;
			default:                                   // high bytes beyond the table
				leave_to(at);
			}
			jsr(0xAB06, 0x8AB6);                       // draw_string_large
			tick(0xAA44, 12);                          // BR
			continue;
		}
		// a number in R1
		tick(0xAB08, 12);                              // BMI
		T.r[0] = T.r[3];
		fl::mov(T.r[0]);
		tick(0xAB0A, 12);
		T.r[0] = fl::sub(T.r[0], 0x0080);
		tick(0xAB0E, 18);
		T.r[0] = fl::asl(T.r[0]);
		tick(0xAB10, 12);
		const uint16_t at = fl::add(0xAB14, peek((uint16_t)(0xFD46 + T.r[0])));
		tick(at, 27);                                  // ADD 0xFD46(R0),PC
		switch (at)
		{
		case 0xAB14:                                   // 0x81-0x84: the level of park n
			T.r[5] = sext8(peekb(0x027A));
			fl::movb((uint8_t)T.r[5]);
			tick(0xAB18, 24);
			T.r[0] = T.r[3];
			fl::mov(T.r[0]);
			tick(0xAB1A, 12);
			T.r[0] = fl::sub(T.r[0], 0x0081);
			tick(0xAB1E, 18);
			pokeb(0x027A, (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0xAB22, 27);
			jsr(0xAB26, 0x90BA);                       // park_level
			T.r[1] = T.r[0];
			fl::mov(T.r[1]);
			tick(0xAB28, 12);
			pokeb(0x027A, (uint8_t)T.r[5]);
			fl::movb((uint8_t)T.r[5]);
			tick(0xAB2C, 27);
			tick(0xABCC, 12);                          // BR
			break;
		case 0xAB2E:                                   // 0x85-0x87: 0x0F38
			T.r[1] = peek(0x0F38);
			fl::mov(T.r[1]);
			tick(0xAB32, 24);
			tick(0xABCC, 12);                          // BR
			break;
		case 0xAB34:                                   // 0x88: by the level class
			rating_class(0xAB34);
			poke(T.r[6], 0);
			fl::clr();
			tick(0xAB66, 21);
			{
				const uint8_t v = peekb((uint16_t)(0xFE8C + T.r[0]));
				fl::movb(v);
				pokeb(T.r[6], v);
			}
			tick(0xAB6A, 36);
			jsr(0xAB6E, 0x90BA);                       // park_level
			push(T.r[0]);
			fl::mov(T.r[0]);
			tick(0xAB70, 24);
			jsr(0xAB74, 0x80D8);                       // mul16
			T.r[1] = T.r[0];
			fl::mov(T.r[1]);
			tick(0xAB76, 12);
			drop(0xAB76);
			tick(0xABCC, 12);                          // BR
			break;
		case 0xAB7A:                                   // 0x89
			load(0xAB7A, 0, 0x0F31, 24);
			T.r[0] = sext8(peekb((uint16_t)(0xFE7C + T.r[0])));
			fl::movb((uint8_t)T.r[0]);
			tick(0xAB84, 27);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
			tick(0xAB88, 18);
			load(0xAB88, 1, (uint16_t)(0xFE70 + T.r[0]), 27);
			tick(0xABCC, 12);                          // BR
			break;
		case 0xAB90:                                   // 0x8A: 0x01F9 / 4
			load(0xAB90, 1, 0x01F9, 24);
			T.r[1] = fl::asr(T.r[1]);
			tick(0xAB98, 12);
			T.r[1] = fl::asr(T.r[1]);
			tick(0xAB9A, 12);
			tick(0xABCC, 12);                          // BR
			break;
		case 0xAB9C:                                   // 0x8B: the level
			jsr(0xABA0, 0x90BA);                       // park_level
			T.r[1] = T.r[0];
			fl::mov(T.r[1]);
			tick(0xABA2, 12);
			tick(0xABCC, 12);                          // BR
			break;
		case 0xABA4:                                   // 0x8C: the park
			load(0xABA4, 1, 0x01F5, 24);
			T.r[1] = fl::inc(T.r[1]);
			tick(0xABAC, 12);
			tick(0xABCC, 12);                          // BR
			break;
		case 0xABAE:                                   // 0x8D: 2 - the park
			T.r[1] = 0x0002;
			fl::mov(T.r[1]);
			tick(0xABB2, 18);
			load(0xABB2, 0, 0x01F5, 24);
			T.r[1] = fl::sub(T.r[1], T.r[0]);
			tick(0xABBA, 12);
			tick(0xABCC, 12);                          // BR
			break;
		case 0xABBC:                                   // 0x8E-0x91: 0x025C-0x025F
			load(0xABBC, 1, (uint16_t)(0x01CE + T.r[3]), 27);
			if (!(T.r[1] & 0x80))
			{
				tick(0xABCC, 12);                      // BPL
				break;
			}
			tick(0xABC4, 12);                          // negative: 0
			[[fallthrough]];
		case 0xABC4:                                   // 0x80: zero
			T.r[1] = 0;
			fl::clr();
			tick(0xABC6, 12);
			tick(0xABCC, 12);                          // BR
			break;
		case 0xABC8:                                   // 0x92: the word 0x0248
			T.r[1] = peek(0x0248);
			fl::mov(T.r[1]);
			tick(0xABCC, 24);
			break;
		default:                                       // numbers beyond the table
			leave_to(at);
		}
		// the screen position: relative (bits 3-7) or the next word
		load(0xABCC, 4, 0x020D, 24);
		fl::logic(T.r[4] & 0x00F8);
		tick(0xABD6, 18);
		if (T.r[4] & 0x00F8)
		{
			tick(0xABD8, 12);
			T.r[0] = T.r[4];
			fl::mov(T.r[0]);
			tick(0xABDA, 12);
			T.r[0] = fl::asr(T.r[0]);
			tick(0xABDC, 12);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & ~0x0003));
			tick(0xABE0, 18);
			T.r[0] = fl::add(T.r[0], peek(0x0186));
			tick(0xABE4, 24);
			poke(0x0186, T.r[0]);
			fl::mov(T.r[0]);
			tick(0xABE8, 27);
			poke(0x0186, fl::sub(peek(0x0186), 0x0040));
			tick(0xABEE, 33);
			tick(0xABFA, 12);                          // BR
		}
		else
		{
			tick(0xABF0, 12);                          // BEQ
			{
				const uint16_t v = peek(T.r[2]);
				T.r[2] = (uint16_t)(T.r[2] + 2);
				fl::mov(v);
				poke(0x0186, v);
			}
			tick(0xABF4, 33);
			poke(0x0186, fl::add(peek(0x0186), 0x2000));
			tick(0xABFA, 33);
		}
		T.r[4] = fl::logic((uint16_t)(T.r[4] & 0x0007));
		tick(0xABFE, 18);
		T.r[4] = fl::asl(T.r[4]);
		tick(0xAC00, 12);
		const uint16_t form = fl::add(0xAC04, peek((uint16_t)(0xFD6C + T.r[4])));
		tick(form, 27);                                // ADD 0xFD6C(R4),PC: the format
		switch (form)
		{
		case 0xAC04:                                   // 0: a digit
			fl::cmp(T.r[1], 0x000A);
			tick(0xAC08, 18);
			if ((int16_t)T.r[1] >= 0x000A)
			{
				tick(0xAC14, 12);                      // BGE
				T.r[0] = 0x003D;
				fl::mov(T.r[0]);
				tick(0xAC18, 18);
			}
			else
			{
				tick(0xAC0A, 12);
				T.r[4] = T.r[1];
				fl::mov(T.r[4]);
				tick(0xAC0C, 12);
				T.r[4] = fl::add(T.r[4], 0x0030);
				tick(0xAC10, 18);
				T.r[0] = T.r[4];
				fl::mov(T.r[0]);
				tick(0xAC12, 12);
				tick(0xAC18, 12);                      // BR
			}
			jsr(0xAC1C, 0x8B52);
			tick(0xACC4, 12);                          // BR
			break;
		case 0xAC1E:                                   // 1: a digit block
		{
			T.r[0] = peek(0x0186);
			fl::mov(T.r[0]);
			tick(0xAC22, 24);
			T.r[4] = peek(0x0188);
			fl::mov(T.r[4]);
			tick(0xAC26, 24);
			static const struct { uint16_t k, tile; } cell[4] = {
				{0x0000, 0x0138}, {0x0002, 0x0139}, {0x0080, 0x013A}, {0x0082, 0x013B},
			};
			uint16_t pc = 0xAC26;
			for (const auto &c : cell)
			{
				const uint16_t a = (uint16_t)(T.r[0] + c.k);
				poke(a, T.r[4]);
				fl::mov(T.r[4]);
				pc = (uint16_t)(pc + (c.k ? 4 : 2));
				tick(pc, c.k ? 30 : 21);
				poke(a, fl::logic((uint16_t)(peek(a) | c.tile)));
				pc = (uint16_t)(pc + (c.k ? 6 : 4));
				tick(pc, c.k ? 36 : 27);
			}
			poke(0x0186, fl::add(peek(0x0186), 0x0004));
			tick(0xAC50, 33);
			number(0xAC50);
			arg(0, 0xAC54);
			arg(3, 0xAC58);
			arg(0, 0xAC5A);
			tick(0xACB8, 12);                          // BR
			arg(1, 0xACBC);
			jsr(0xACC0, 0xBA9A);
			T.r[6] = fl::add(T.r[6], 0x0008);
			tick(0xACC4, 18);
			break;
		}
		case 0xAC5C:                                   // 2
			number(0xAC5C);
			arg(0, 0xAC60);
			arg(4, 0xAC64);
			tick(0xACB4, 12);                          // BR
			arg(1, 0xACB8);
			arg(1, 0xACBC);
			jsr(0xACC0, 0xBA9A);
			T.r[6] = fl::add(T.r[6], 0x0008);
			tick(0xACC4, 18);
			break;
		case 0xAC66:                                   // 3
			number(0xAC66);
			arg(0, 0xAC6A);
			arg(2, 0xAC6E);
			arg(1, 0xAC72);
			arg(0, 0xAC74);
			tick(0xACBC, 12);                          // BR
			jsr(0xACC0, 0xBA9A);
			T.r[6] = fl::add(T.r[6], 0x0008);
			tick(0xACC4, 18);
			break;
		case 0xAC76:                                   // 4: the counters 0x023E / 0x0240
		case 0xAC8A:                                   // 5: the counter 0x0242
		{
			const bool pair = form == 0xAC76;
			{
				const uint16_t v = peek(pair ? 0x023E : 0x0242);
				fl::mov(v);
				poke(T.r[6], v);
			}
			tick((uint16_t)(form + 4), 33);
			if (pair)
			{
				push_w(0x0240, 0xAC7E);
				arg(6, 0xAC82);
				arg(0, 0xAC84);
				arg(1, 0xAC88);
				tick(0xAC98, 12);                      // BR
			}
			else
			{
				arg(0, 0xAC90);
				arg(3, 0xAC94);
				arg(0, 0xAC96);
				arg(0, 0xAC98);
			}
			jsr(0xAC9C, 0xBA96);
			tick(0xACC0, 12);                          // BR
			T.r[6] = fl::add(T.r[6], 0x0008);
			tick(0xACC4, 18);
			break;
		}
		case 0xAC9E:                                   // 6: a small digit
			T.r[4] = T.r[1];
			fl::mov(T.r[4]);
			tick(0xACA0, 12);
			T.r[4] = fl::add(T.r[4], 0x0030);
			tick(0xACA4, 18);
			T.r[0] = T.r[4];
			fl::mov(T.r[0]);
			tick(0xACA6, 12);
			jsr(0xACAA, 0x8C12);
			tick(0xACC4, 12);                          // BR
			break;
		case 0xACAC:                                   // 7
			number(0xACAC);
			arg(0, 0xACB0);
			arg(5, 0xACB4);
			arg(1, 0xACB8);
			arg(1, 0xACBC);
			jsr(0xACC0, 0xBA9A);
			T.r[6] = fl::add(T.r[6], 0x0008);
			tick(0xACC4, 18);
			break;
		default:
			leave_to(form);
		}
		tick(0xAA44, 18);                              // JMP @#0xAA44
	}
	{
		const uint16_t v = peek((uint16_t)(T.r[6] + 6));
		fl::mov(v);
		poke(0x01D2, v);
	}
	tick(0xACCE, 42);
	{
		const uint16_t v = peek((uint16_t)(T.r[6] + 4));
		fl::mov(v);
		poke(0x01D4, v);
	}
	tick(0xACD4, 42);
	copy_w(0x01D2, 0x1400, 0xACDA);
	copy_w(0x01D4, 0x1402, 0xACE0);
	T.r[6] = fl::add(T.r[6], 0x0008);
	tick(0xACE4, 18);
	c_restore(4);
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
	T.r[6] = fl::sub(T.r[6], 0x0006);                  // the local frame
	tick(0xA514, 18);
	const uint16_t fp = T.r[6];                        // fp+2: &0x0370[object], fp+4: on the ramp
	{
		const uint8_t timer = peekb(0x0EA2);
		fl::tstb(timer);
		tick(0xA518, 24);
		if (!timer)
		{
			tick(0xA51A, 12);
			copy_w(0x03F0, 0x023A, 0xA520);
			copy_w(0x0670, 0x0238, 0xA526);
			tick(0xA53A, 12);                          // BR
		}
		else
		{
			tick(0xA528, 12);                          // BNE
			set_w(0x023A, 0x7106, 0xA52E);
			copy_w(0x0670, 0x0238, 0xA534);
			poke(0x0238, fl::sub(peek(0x0238), peek(0x06B0)));
			tick(0xA53A, 39);
		}
	}
	bank_a(0xA53A, 0x3800);
	bank_b(0xA546, 0x0400);
	jsr(0xA556, 0x5684);                               // the object touched
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0xA558, 12);
	clr_w(0x01D2, 0xA55C);
	clr_w(0x1400, 0xA560);
	fl::tst(T.r[1]);
	tick(0xA562, 12);
	if ((int16_t)T.r[1] <= 0)
	{
		tick(0xA564, 12);                              // none: the contact timers run out
		tick(0xA8BA, 18);                              // JMP
		const uint8_t t = peekb(0x0EA2);
		fl::tstb(t);
		tick(0xA8BE, 24);
		if (!t)
			tick(0xA8C4, 12);                          // BEQ
		else
		{
			tick(0xA8C0, 12);
			pokeb(0x0EA2, fl::decb(peekb(0x0EA2)));
			tick(0xA8C4, 27);
		}
		clr_b(0x0EA1, 0xA8C8);
		clr_b(0x0334, 0xA8CC);
		goto finish;
	}
	{
		tick(0xA568, 12);                              // BGT
		poke((uint16_t)(fp + 2), 0x0370);
		fl::mov(0x0370);
		tick(0xA56E, 36);
		poke((uint16_t)(fp + 2), fl::add(peek((uint16_t)(fp + 2)), T.r[1]));
		tick(0xA572, 30);
		// CLR Rr at pc; BISB @k(SP),Rr: the kind of the object (on at pc + 6)
		auto kind = [fp](uint16_t pc, int r) {
			T.r[r] = 0;
			fl::clr();
			tick((uint16_t)(pc + 2), 12);
			T.r[r] = (uint16_t)(T.r[r] | fl::logicb(peekb(peek((uint16_t)(fp + 2)))));
			tick((uint16_t)(pc + 6), 33);
		};
		// CLR R0 at pc; BISB @#a,R0 (on at pc + 6)
		auto byte = [](uint16_t pc, uint16_t a) {
			T.r[0] = 0;
			fl::clr();
			tick((uint16_t)(pc + 2), 12);
			T.r[0] = (uint16_t)(T.r[0] | fl::logicb(peekb(a)));
			tick((uint16_t)(pc + 6), 24);
		};
		// CLR R0 at pc, JSR F:AD70 (object 0 in front of R1?), ROR R0, BCC, MOV #4 / #12: the
		// direction in R0, on at pc + 0x14
		auto facing = [](uint16_t pc) {
			T.r[0] = 0;
			fl::clr();
			tick((uint16_t)(pc + 2), 12);
			jsr((uint16_t)(pc + 6), 0xAD70);           // sprite_in_front
			const bool front = (T.r[0] & 1) != 0;
			T.r[0] = fl::ror(T.r[0]);
			tick((uint16_t)(pc + 8), 12);
			if (front)
			{
				tick((uint16_t)(pc + 0x0A), 12);
				T.r[0] = 0x0004;
				fl::mov(T.r[0]);
				tick((uint16_t)(pc + 0x0E), 18);
				tick((uint16_t)(pc + 0x14), 12);       // BR
			}
			else
			{
				tick((uint16_t)(pc + 0x10), 12);       // BCC
				T.r[0] = 0x000C;
				fl::mov(T.r[0]);
				tick((uint16_t)(pc + 0x14), 18);
			}
		};
		kind(0xA572, 0);
		poke((uint16_t)(fp + 4), 0);
		fl::clr();
		tick(0xA57C, 30);
		const uint8_t ramp = peekb(0x01EA);
		fl::tstb(ramp);
		tick(0xA580, 24);
		if (ramp)
		{
			tick(0xA582, 12);
			poke((uint16_t)(fp + 4), fl::inc(peek((uint16_t)(fp + 4))));
			tick(0xA586, 30);
			tick(0xA5A2, 12);                          // BR
		}
		else
		{
			tick(0xA588, 12);                          // in the air: too far above it?
			T.r[3] = peek(0x06B2);
			fl::mov(T.r[3]);
			tick(0xA58C, 24);
			T.r[3] = fl::sub(T.r[3], peek(0x06B0));
			tick(0xA590, 24);
			T.r[2] = 0;
			fl::clr();
			tick(0xA592, 12);
			T.r[2] = (uint16_t)(T.r[2] | fl::logicb(peekb((uint16_t)(0x6631 + T.r[0]))));
			tick(0xA596, 27);
			T.r[2] = fl::swab(T.r[2]);
			tick(0xA598, 12);
			T.r[2] &= 0xFF00;
			fl::clr();
			tick(0xA59A, 12);
			fl::cmp(T.r[3], T.r[2]);
			tick(0xA59C, 12);
			if ((int16_t)T.r[3] > (int16_t)T.r[2])
			{
				tick(0xA59E, 12);
				tick(0xA8CC, 18);                      // JMP
				goto finish;
			}
			tick(0xA5A2, 12);                          // BLE
		}
		T.r[0] = sext8(peekb((uint16_t)(0x61DA + T.r[0])));   // the type of the kind
		fl::movb((uint8_t)T.r[0]);
		tick(0xA5A6, 27);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
		tick(0xA5AA, 18);
		T.r[2] = T.r[0];
		fl::mov(T.r[2]);
		tick(0xA5AC, 12);
		T.r[2] = fl::asl(T.r[2]);
		tick(0xA5AE, 12);
		const uint16_t at = fl::add(0xA5B2, peek((uint16_t)(0xFD1C + T.r[2])));
		tick(at, 27);                                  // ADD 0xFD1C(R2),PC
		switch (at)
		{
		case 0xA5B2:                                   // types 0, 1 (and 13): nothing
			T.r[0] = 0;
			fl::clr();
			tick(0xA5B4, 12);
			tick(0xA8B4, 18);                          // JMP
			goto store;
		case 0xA5B8:                                   // types 2, 7: he lands on it
		{
			byte(0xA5B8, 0x0334);
			fl::cmp(T.r[0], T.r[1]);
			tick(0xA5C0, 12);
			if (T.r[0] == T.r[1])
			{
				tick(0xA5B4, 12);                      // BEQ: once per object
				tick(0xA8B4, 18);                      // JMP
				goto store;
			}
			tick(0xA5C2, 12);
			const uint8_t state = peekb(0x031E);
			fl::tstb(state);
			tick(0xA5C6, 24);
			if (state)
				tick(0xA5E0, 12);                      // BNE
			else
			{
				tick(0xA5C8, 12);
				facing(0xA5C8);
				pokeb(0x031D, (uint8_t)T.r[0]);
				fl::movb((uint8_t)T.r[0]);
				tick(0xA5E0, 27);
			}
			copy_b(0x031D, 0x01F8, 0xA5E6);
			set_w(0x01B4, 0x0514, 0xA5EC);
			jsr(0xA5F0, 0xA2F0);                       // skater_land
			kind(0xA5F0, 0);
			fl::cmp(T.r[0], 0x0004);
			tick(0xA5FA, 18);
			if (T.r[0] != 4)
			{
				tick(0xA5B4, 12);                      // BNE
				tick(0xA8B4, 18);                      // JMP
				goto store;
			}
			tick(0xA5FC, 12);                          // kind 4 ends the run
			push(0);
			fl::clr();
			tick(0xA5FE, 24);
			{
				const uint8_t v = peekb((uint16_t)(0x0390 + T.r[1]));
				fl::movb(v);
				pokeb(T.r[6], v);
			}
			tick(0xA602, 36);
			jsr(0xA606, 0xCF5E);
			jsr(0xA60A, 0xE222);                       // state1_to_8
			tick(0xA6A4, 12);                          // BR
			tick(0xA782, 12);                          // BR
			tick(0xA862, 12);                          // BR
			tick(0xA8B2, 12);                          // BR
			drop(0xA8B2);
			goto store;
		}
		case 0xA6A6:                                   // types 3, 4: the object is hit
			push(0x0067);
			fl::mov(0x0067);
			tick(0xA6AA, 30);
			jsr(0xA6AE, 0x8E78);                       // queue_sound(0x67)
			T.r[2] = T.r[1];
			fl::mov(T.r[2]);
			tick(0xA6B0, 12);
			T.r[2] = fl::asl(T.r[2]);
			tick(0xA6B2, 12);
			poke((uint16_t)(0x03F0 + T.r[2]), 0);
			fl::clr();
			tick(0xA6B6, 30);
			poke(T.r[6], T.r[1]);
			fl::mov(T.r[1]);
			tick(0xA6B8, 21);
			push(0);
			fl::clr();
			tick(0xA6BA, 24);
			{
				const uint8_t v = peekb(0x5CDE);
				fl::movb(v);
				pokeb(T.r[6], v);
			}
			tick(0xA6BE, 33);
			jsr(0xA6C2, 0xB602);
			{
				const uint16_t a = (uint16_t)(0x04B0 + T.r[1]);
				pokeb(a, fl::logicb((uint8_t)(peekb(a) | 0x08)));
			}
			tick(0xA6C8, 36);
			T.r[0] = 0x0007;
			fl::mov(T.r[0]);
			tick(0xA6CC, 18);
			pokeb((uint16_t)(0x0790 + T.r[1]), (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0xA6D0, 30);
			pokeb((uint16_t)(0x0770 + T.r[1]), (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0xA6D4, 30);
			poke((uint16_t)(0x0430 + T.r[2]), 0x0080);
			fl::mov(0x0080);
			tick(0xA6DA, 36);
			kind(0xA6DA, 0);
			T.r[0] = fl::sub(T.r[0], 0x0048);
			tick(0xA6E4, 18);
			T.r[0] = fl::asl(T.r[0]);
			tick(0xA6E6, 12);
			{
				const uint16_t v = peek((uint16_t)(0xFCFA + T.r[0]));   // the animation
				fl::mov(v);
				poke(T.r[6], v);
			}
			tick(0xA6EA, 36);
			jsr(0xA6EE, 0xEF24);
			drop2(0xA6EE);
			tick(0xA708, 12);                          // BR
			tick(0xA8B4, 18);                          // JMP
			goto store;
		case 0xA6F2:                                   // type 5: he rides it
		{
			const uint8_t carrier = peekb(0x0EA0);
			fl::tstb(carrier);
			tick(0xA6F6, 24);
			if (carrier)
				tick(0xA708, 12);                      // BNE
			else
			{
				tick(0xA6F8, 12);
				const uint8_t mode = peekb(0x031F);
				fl::cmpb(mode, 0x01);
				tick(0xA6FE, 30);
				if (mode == 1)
				{
					tick(0xA756, 12);                  // BEQ (onto the BEQ at 0xA756)
					tick(0xA708, 12);
				}
				else
				{
					tick(0xA700, 12);
					pokeb(0x0EA0, (uint8_t)T.r[1]);
					fl::movb((uint8_t)T.r[1]);
					tick(0xA704, 27);
					pokeb(0x0EA0, fl::negb(peekb(0x0EA0)));
					tick(0xA708, 27);
				}
			}
			tick(0xA8B4, 18);                          // JMP
			goto store;
		}
		case 0xA60C:                                   // type 6: bumped
		{
			byte(0xA60C, 0x0334);
			fl::cmp(T.r[0], T.r[1]);
			tick(0xA614, 12);
			if (T.r[0] == T.r[1])
			{
				tick(0xA5B4, 12);                      // BEQ: once per object
				tick(0xA8B4, 18);                      // JMP
				goto store;
			}
			tick(0xA616, 12);
			T.r[0] = peek(0x06B2);
			fl::mov(T.r[0]);
			tick(0xA61A, 24);
			T.r[0] = fl::sub(T.r[0], peek(0x06B0));
			tick(0xA61E, 24);
			fl::cmp(T.r[0], 0x0380);
			tick(0xA622, 18);
			if ((int16_t)T.r[0] < 0x0380)
			{
				tick(0xA624, 12);                      // near the height of the skater: as 2
				facing(0xA624);
				pokeb(0x031D, (uint8_t)T.r[0]);
				fl::movb((uint8_t)T.r[0]);
				tick(0xA63C, 27);
				pokeb(0x01F8, (uint8_t)T.r[0]);
				fl::movb((uint8_t)T.r[0]);
				tick(0xA640, 27);
				set_w(0x01B4, 0x0514, 0xA646);
				jsr(0xA64A, 0xA2F0);                   // skater_land
				tick(0xA680, 12);                      // BR
			}
			else
			{
				tick(0xA64C, 12);                      // BGE: the queued event 0x0E6C
				byte(0xA64C, 0x0E6C);
				fl::tst(T.r[0]);
				tick(0xA654, 12);
				if (!T.r[0])
					tick(0xA680, 12);                  // BEQ
				else
				{
					tick(0xA656, 12);
					push(T.r[0]);
					fl::mov(T.r[0]);
					tick(0xA658, 24);
					jsr(0xA65C, 0xF064);
					byte(0xA65C, 0x0E6C);
					T.r[0] = fl::asl(T.r[0]);
					tick(0xA664, 12);
					{
						const uint16_t v = peek((uint16_t)(0xFCFA + T.r[0]));
						fl::mov(v);
						poke(T.r[6], v);
					}
					tick(0xA668, 36);
					jsr(0xA66C, 0xEF24);
					const uint8_t k = peekb(peek((uint16_t)(T.r[6] + 4)));
					const uint8_t last = peekb(0x0E6B);
					fl::cmpb(k, last);
					tick(0xA672, 45);
					if (k != last)
						tick(0xA678, 12);              // BNE
					else
					{
						tick(0xA674, 12);
						pokeb(0x0E6C, fl::decb(peekb(0x0E6C)));
						tick(0xA678, 27);
					}
					{
						const uint8_t v = peekb(peek((uint16_t)(T.r[6] + 4)));
						fl::movb(v);
						pokeb(0x0E6B, v);
					}
					tick(0xA67E, 48);
					drop(0xA67E);
				}
			}
			bank_a(0xA680, 0x3800);
			bank_b(0xA68C, 0x3C00);
			push(T.r[1]);
			fl::mov(T.r[1]);
			tick(0xA69A, 24);
			jsr(0xA69E, 0x4ECC);
			poke(T.r[6], T.r[1]);
			fl::mov(T.r[1]);
			tick(0xA6A0, 21);
			jsr(0xA6A4, 0xD47C);
			tick(0xA782, 12);                          // BR
			tick(0xA862, 12);                          // BR
			tick(0xA8B2, 12);                          // BR
			drop(0xA8B2);
			goto store;
		}
		case 0xA70C:                                   // type 8
			push(0x0061);
			fl::mov(0x0061);
			tick(0xA710, 30);
			jsr(0xA714, 0x8E78);                       // queue_sound(0x61)
			kind(0xA714, 0);
			T.r[0] = fl::sub(T.r[0], 0x0038);
			tick(0xA71E, 18);
			T.r[2] = sext8(peekb((uint16_t)(0xFD10 + T.r[0])));
			fl::movb((uint8_t)T.r[2]);
			tick(0xA722, 27);
			poke(T.r[6], T.r[2]);
			fl::mov(T.r[2]);
			tick(0xA724, 21);
			jsr(0xA728, 0xF002);
			poke(T.r[6], 0);
			fl::clr();
			tick(0xA72A, 21);
			{
				const uint8_t v = peekb((uint16_t)(0x0390 + T.r[1]));
				fl::movb(v);
				pokeb(T.r[6], v);
			}
			tick(0xA72E, 36);
			jsr(0xA732, 0xCF5E);
			tick(0xA820, 12);                          // BR
			tick(0xA8B2, 12);                          // BR
			drop(0xA8B2);
			goto store;
		case 0xA734:                                   // type 9: bounce or deflect
			kind(0xA734, 0);
			fl::cmp(T.r[0], 0x0014);
			tick(0xA73E, 18);
			if (T.r[0] == 0x14)
			{
				tick(0xA740, 12);
				T.r[0] = 0x0007;
				fl::mov(T.r[0]);
				tick(0xA744, 18);
				tick(0xA74A, 12);                      // BR
			}
			else
			{
				tick(0xA746, 12);                      // BNE
				T.r[0] = 0x000F;
				fl::mov(T.r[0]);
				tick(0xA74A, 18);
			}
			pokeb(0x0329, (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0xA74E, 27);
			T.r[2] = 0;
			fl::clr();
			tick(0xA750, 12);
			T.r[2] = (uint16_t)(T.r[2] | fl::logicb(peekb(0x0328)));
			tick(0xA754, 24);
			fl::cmp(T.r[2], T.r[0]);
			tick(0xA756, 12);
			if (T.r[2] == T.r[0])
			{
				tick(0xA708, 12);                      // BEQ
				tick(0xA8B4, 18);                      // JMP
				goto store;
			}
			tick(0xA758, 12);
			pokeb(0x0328, (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0xA75C, 27);
			fl::logic(peek((uint16_t)(fp + 4)) & 0x0001);
			tick(0xA762, 33);
			if (peek((uint16_t)(fp + 4)) & 0x0001)
			{
				tick(0xA764, 12);                      // on the ramp: deflected
				push(T.r[0]);
				fl::mov(T.r[0]);
				tick(0xA766, 24);
				jsr(0xA76A, 0x9E52);                   // slope_table_value
				poke(T.r[6], T.r[0]);
				fl::mov(T.r[0]);
				tick(0xA76C, 21);
				jsr(0xA770, 0x9EEE);                   // skater_deflect
				tick(0xA862, 12);                      // BR
			}
			else
			{
				tick(0xA772, 12);                      // BEQ: bounced the other way
				push(0x0008);
				fl::mov(0x0008);
				tick(0xA776, 30);
				byte(0xA776, 0x031D);
				poke(T.r[6], fl::logic((uint16_t)(peek(T.r[6]) ^ T.r[0])));
				tick(0xA77E, 21);
				jsr(0xA782, 0x9E96);                   // skater_bounce
				tick(0xA862, 12);                      // BR
			}
			tick(0xA8B2, 12);                          // BR
			drop(0xA8B2);
			goto store;
		case 0xA784:                                   // type 10: the pair change kind
		{
			push(T.r[1]);
			fl::mov(T.r[1]);
			tick(0xA786, 24);
			jsr(0xA78A, 0xA8D2);                       // partner_object
			T.r[4] = T.r[0];
			fl::mov(T.r[4]);
			tick(0xA78C, 12);
			kind(0xA78C, 0);
			T.r[0] = fl::sub(T.r[0], 0x001A);
			tick(0xA796, 18);
			T.r[5] = T.r[4];
			fl::mov(T.r[5]);
			tick(0xA798, 12);
			T.r[5] = fl::asl(T.r[5]);
			tick(0xA79A, 12);
			T.r[2] = 0x03B0;
			fl::mov(T.r[2]);
			tick(0xA79E, 18);
			T.r[2] = fl::add(T.r[2], T.r[5]);
			tick(0xA7A0, 12);
			poke(fp, T.r[1]);
			fl::mov(T.r[1]);
			tick(0xA7A4, 30);
			poke(fp, fl::asl(peek(fp)));
			tick(0xA7A8, 30);
			push(0x03B0);
			fl::mov(0x03B0);
			tick(0xA7AC, 30);
			poke(T.r[6], fl::add(peek(T.r[6]), peek((uint16_t)(T.r[6] + 4))));
			tick(0xA7B0, 36);
			T.r[3] = pop();
			fl::mov(T.r[3]);
			tick(0xA7B2, 18);
			bool near = false;
			fl::cmp(T.r[0], 0x0005);
			tick(0xA7B6, 18);
			bool look = (int16_t)T.r[0] < 5;
			if (look)
				tick(0xA7BE, 12);                      // BLT
			else
			{
				tick(0xA7B8, 12);
				fl::cmp(T.r[0], 0x0007);
				tick(0xA7BC, 18);
				look = (int16_t)T.r[0] > 7;
				tick(look ? 0xA7BE : 0xA7D6, 12);      // BLE
			}
			if (look)
			{
				T.r[0] = 0;
				fl::clr();
				tick(0xA7C0, 12);
				jsr(0xA7C4, 0xAD70);                   // sprite_in_front
				near = (T.r[0] & 1) != 0;
				T.r[0] = fl::ror(T.r[0]);
				tick(0xA7C6, 12);
				tick(near ? 0xA7C8 : 0xA7D6, 12);      // BCC
			}
			if (near)
			{
				poke(T.r[2], fl::add(peek(T.r[2]), 0x0002));
				tick(0xA7CC, 27);
				poke(T.r[3], fl::add(peek(T.r[3]), 0x0002));
				tick(0xA7D0, 27);
				T.r[0] = 0x0018;
				fl::mov(T.r[0]);
				tick(0xA7D4, 18);
				tick(0xA7FC, 12);                      // BR
			}
			else
			{
				poke(0x0F20, fl::add(peek(0x0F20), 0x0006));   // points, at most 0x80
				tick(0xA7DC, 33);
				const uint16_t points = peek(0x0F20);
				fl::cmp(points, 0x0080);
				tick(0xA7E2, 30);
				if ((int16_t)points <= 0x0080)
					tick(0xA7EA, 12);                  // BLE
				else
				{
					tick(0xA7E4, 12);
					set_w(0x0F20, 0x0080, 0xA7EA);
				}
				pokeb(0x0FA2, fl::logicb((uint8_t)(peekb(0x0FA2) | 0x04)));
				tick(0xA7F0, 33);
				poke(T.r[2], fl::add(peek(T.r[2]), 0x0004));
				tick(0xA7F4, 27);
				poke(T.r[3], fl::add(peek(T.r[3]), 0x0004));
				tick(0xA7F8, 27);
				T.r[0] = 0x0017;
				fl::mov(T.r[0]);
				tick(0xA7FC, 18);
			}
			pokeb(peek((uint16_t)(T.r[6] + 4)), (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0xA800, 36);
			pokeb((uint16_t)(0x0370 + T.r[4]), (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0xA804, 30);
			{
				const uint16_t a = (uint16_t)(0x0490 + T.r[1]);
				pokeb(a, fl::logicb((uint8_t)(peekb(a) | 0x18)));
			}
			tick(0xA80A, 36);
			{
				const uint16_t a = (uint16_t)(0x0490 + T.r[4]);
				pokeb(a, fl::logicb((uint8_t)(peekb(a) | 0x18)));
			}
			tick(0xA810, 36);
			T.r[0] = peek((uint16_t)(T.r[6] + 2));
			fl::mov(T.r[0]);
			tick(0xA814, 27);
			poke((uint16_t)(0x0430 + T.r[0]), 0x0010);
			fl::mov(0x0010);
			tick(0xA81A, 36);
			poke((uint16_t)(0x0430 + T.r[5]), 0x000F);
			fl::mov(0x000F);
			tick(0xA820, 36);
			tick(0xA8B2, 12);                          // BR
			drop(0xA8B2);
			goto store;
		}
		case 0xA822:                                   // type 11: the pair change kind
			push(T.r[1]);
			fl::mov(T.r[1]);
			tick(0xA824, 24);
			jsr(0xA828, 0xA8D2);                       // partner_object
			T.r[3] = T.r[0];
			fl::mov(T.r[3]);
			tick(0xA82A, 12);
			T.r[4] = T.r[3];
			fl::mov(T.r[4]);
			tick(0xA82C, 12);
			T.r[4] = fl::asl(T.r[4]);
			tick(0xA82E, 12);
			poke((uint16_t)(0x03B0 + T.r[4]), fl::add(peek((uint16_t)(0x03B0 + T.r[4])), 0x0002));
			tick(0xA834, 36);
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0xA836, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0xA838, 12);
			poke((uint16_t)(0x03B0 + T.r[0]), fl::add(peek((uint16_t)(0x03B0 + T.r[0])), 0x0002));
			tick(0xA83E, 36);
			T.r[5] = 0x0018;
			fl::mov(T.r[5]);
			tick(0xA842, 18);
			pokeb(peek((uint16_t)(T.r[6] + 4)), (uint8_t)T.r[5]);
			fl::movb((uint8_t)T.r[5]);
			tick(0xA846, 36);
			pokeb((uint16_t)(0x0370 + T.r[3]), (uint8_t)T.r[5]);
			fl::movb((uint8_t)T.r[5]);
			tick(0xA84A, 30);
			{
				const uint16_t a = (uint16_t)(0x0490 + T.r[1]);
				pokeb(a, fl::logicb((uint8_t)(peekb(a) | 0x18)));
			}
			tick(0xA850, 36);
			{
				const uint16_t a = (uint16_t)(0x0490 + T.r[3]);
				pokeb(a, fl::logicb((uint8_t)(peekb(a) | 0x18)));
			}
			tick(0xA856, 36);
			poke((uint16_t)(0x0430 + T.r[0]), 0x0010);
			fl::mov(0x0010);
			tick(0xA85C, 36);
			poke((uint16_t)(0x0430 + T.r[4]), 0x000F);
			fl::mov(0x000F);
			tick(0xA862, 36);
			tick(0xA8B2, 12);                          // BR
			drop(0xA8B2);
			goto store;
		case 0xA864:                                   // type 12: a bump on level ground
		{
			const uint16_t z = peek(0x06B2), ground = peek(0x06B0);
			fl::cmp(z, ground);
			tick(0xA86A, 36);
			if (z != ground)
			{
				tick(0xA8B4, 12);                      // BNE
				goto store;
			}
			tick(0xA86C, 12);
			T.r[0] = peek(0x01B4);
			fl::mov(T.r[0]);
			tick(0xA870, 24);
			if ((int16_t)T.r[0] >= 0)
				tick(0xA874, 12);                      // BGE
			else
			{
				tick(0xA872, 12);
				T.r[0] = fl::neg(T.r[0]);
				tick(0xA874, 12);
			}
			fl::cmp(T.r[0], 0x0180);
			tick(0xA878, 18);
			if ((int16_t)T.r[0] < 0x0180)
			{
				tick(0xA8B4, 12);                      // BLT: not fast enough
				goto store;
			}
			tick(0xA87A, 12);
			T.r[0] = 0x0002;
			fl::mov(T.r[0]);
			tick(0xA87E, 18);
			pokeb((uint16_t)(0x0790 + T.r[1]), (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0xA882, 30);
			pokeb((uint16_t)(0x0770 + T.r[1]), (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0xA886, 30);
			bank_b(0xA886, 0x2C00);
			T.r[0] = sext8(peekb(0x025D));             // the board
			fl::movb((uint8_t)T.r[0]);
			tick(0xA896, 24);
			T.r[0] = fl::asl(T.r[0]);
			tick(0xA898, 12);
			poke(0x05B2, fl::add(peek(0x05B2), peek((uint16_t)(0x7CA4 + T.r[0]))));
			tick(0xA89E, 42);
			fl::logic(peek((uint16_t)(fp + 4)) & 0x0001);
			tick(0xA8A4, 33);
			if (!(peek((uint16_t)(fp + 4)) & 0x0001))
				tick(0xA8AA, 12);                      // BEQ
			else
			{
				tick(0xA8A6, 12);
				jsr(0xA8AA, 0x9F8A);                   // skater_jump
			}
			push(0x007B);
			fl::mov(0x007B);
			tick(0xA8AE, 30);
			jsr(0xA8B2, 0x8E78);                       // queue_sound(0x7B)
			drop(0xA8B2);
			goto store;
		}
		default:                                       // kinds beyond the types
			leave_to(at);
		}
	}
store:
	pokeb(0x0334, (uint8_t)T.r[1]);                    // the object handled
	fl::movb((uint8_t)T.r[1]);
	tick(0xA8B8, 27);
	tick(0xA8CC, 12);                                  // BR
finish:
	T.r[6] = fl::add(T.r[6], 0x0006);
	tick(0xA8D0, 18);
	rts();
}

// F:9E96  skater_bounce(int dir): new direction dir (0x3F: the opposite one), speed 0x160,
// motion values for that direction (F:9B82 with terrain 0x0327 cleared, F:91A6, then again
// with the terrain), no upward z velocity for object 1; state 0x031E = 2 for 6 (0x031C)
uint16_t skater_bounce(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x9E98, 24);
	const uint16_t dir = peek((uint16_t)(T.r[6] + 4));
	fl::cmp(dir, 0x003F);
	tick(0x9E9E, 33);
	if (dir == 0x003F)
	{
		tick(0x9EA0, 12);                              // the opposite direction
		turn_round(0x9EA0, 1, 0x031D);
		tick(0x9EB6, 12);                              // BR
	}
	else
	{
		tick(0x9EB0, 12);                              // BNE
		const uint8_t v = peekb((uint16_t)(T.r[6] + 4));
		fl::movb(v);
		pokeb(0x031D, v);
		tick(0x9EB6, 42);
	}
	set_w(0x01B4, 0x0160, 0x9EBC);
	T.r[1] = sext8(peekb(0x0327));                     // the terrain, put back after F:91A6
	fl::movb((uint8_t)T.r[1]);
	tick(0x9EC0, 24);
	clr_b(0x0327, 0x9EC4);
	jsr(0x9EC8, 0x9B82);                               // skater_motion_params
	jsr(0x9ECC, 0x91A6);                               // skater_velocity
	pokeb(0x0327, (uint8_t)T.r[1]);
	fl::movb((uint8_t)T.r[1]);
	tick(0x9ED0, 27);
	jsr(0x9ED4, 0x9B82);                               // skater_motion_params
	const uint16_t vz = peek(0x05B2);
	fl::tst(vz);
	tick(0x9ED8, 24);
	if ((int16_t)vz <= 0)
		tick(0x9EDE, 12);                              // BLE
	else
	{
		tick(0x9EDA, 12);                              // no upward z velocity
		clr_w(0x05B2, 0x9EDE);
	}
	set_b(0x031E, 0x02, 0x9EE4);
	set_b(0x031C, 0x06, 0x9EEA);
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x9EEC, 18);
	rts();
	return T.r[0];
}

// F:9EEE  skater_deflect(int dir): heading 0x031B = dir (0x3F: the current direction, with the
// speed reversed to 0x160 the other way); otherwise the speed keeps its sign and is at least
// 0x160 (a negative speed takes the opposite heading).  On a ramp (0x01EA == 2) F:A2F0 lands
// the skater; mode 0x031F == 1 turns a negative speed around.  State 0x031E = 1 for 8.
uint16_t skater_deflect(const Args &)
{
	c_save(1, 0x9EF2);
	T.r[1] = peek(0x01B4);                             // the speed
	fl::mov(T.r[1]);
	tick(0x9EF6, 24);
	const uint16_t dir = peek((uint16_t)(T.r[6] + 8));
	fl::cmp(dir, 0x003F);
	tick(0x9EFC, 33);
	bool slow;                                         // the speed becomes 0x160 (else 0xFEA0)
	if (dir == 0x003F)
	{
		tick(0x9EFE, 12);                              // the current direction, speed reversed
		copy_b(0x031D, 0x031B, 0x9F04);
		fl::tst(T.r[1]);
		tick(0x9F06, 12);
		if ((int16_t)T.r[1] >= 0)
		{
			tick(0x9F28, 12);                          // BGE
			slow = false;
		}
		else
		{
			tick(0x9F08, 12);
			tick(0x9F36, 12);                          // BR
			slow = true;
		}
	}
	else
	{
		tick(0x9F0A, 12);                              // BNE
		{
			const uint8_t v = peekb((uint16_t)(T.r[6] + 8));
			fl::movb(v);
			pokeb(0x031B, v);
		}
		tick(0x9F10, 42);
		fl::tst(T.r[1]);
		tick(0x9F12, 12);
		if ((int16_t)T.r[1] >= 0)
		{
			tick(0x9F30, 12);                          // BGE: at least 0x160
			fl::cmp(T.r[1], 0x0160);
			tick(0x9F34, 18);
			if ((int16_t)T.r[1] >= 0x0160)
			{
				tick(0x9F3C, 12);                      // BGE
				goto ramp;
			}
			tick(0x9F36, 12);
			slow = true;
		}
		else
		{
			tick(0x9F14, 12);                          // backwards: the opposite heading
			T.r[2] = 0x0008;
			fl::mov(T.r[2]);
			tick(0x9F18, 18);
			T.r[0] = sext8(peekb((uint16_t)(T.r[6] + 8)));
			fl::movb((uint8_t)T.r[0]);
			tick(0x9F1C, 27);
			T.r[2] = fl::logic((uint16_t)(T.r[2] ^ T.r[0]));
			tick(0x9F1E, 12);
			pokeb(0x031B, (uint8_t)T.r[2]);
			fl::movb((uint8_t)T.r[2]);
			tick(0x9F22, 27);
			fl::cmp(T.r[1], 0xFEA0);
			tick(0x9F26, 18);
			if ((int16_t)T.r[1] <= (int16_t)0xFEA0)
			{
				tick(0x9F3C, 12);                      // BLE
				goto ramp;
			}
			tick(0x9F28, 12);
			slow = false;
		}
	}
	if (slow)
		set_w(0x01B4, 0x0160, 0x9F3C);
	else
	{
		set_w(0x01B4, 0xFEA0, 0x9F2E);
		tick(0x9F3C, 12);                              // BR
	}
ramp:
	{
		const uint8_t ramp = peekb(0x01EA);
		fl::cmpb(ramp, 0x02);
		tick(0x9F42, 30);
		if (ramp != 2)
			tick(0x9F5C, 12);                          // BNE
		else
		{
			tick(0x9F44, 12);                          // on the ramp he lands
			const uint16_t speed = peek(0x01B4);
			fl::tst(speed);
			tick(0x9F48, 24);
			if ((int16_t)speed >= 0)
				tick(0x9F58, 12);                      // BGE
			else
			{
				tick(0x9F4A, 12);
				turn_round(0x9F4A, 1, 0x031B);
			}
			jsr(0x9F5C, 0xA2F0);                       // skater_land
		}
	}
	{
		const uint8_t mode = peekb(0x031F);
		fl::cmpb(mode, 0x01);
		tick(0x9F62, 30);
		if (mode != 1)
			tick(0x9F7C, 12);                          // BNE
		else
		{
			tick(0x9F64, 12);                          // mode 1 turns a negative speed around
			const uint16_t speed = peek(0x01B4);
			fl::tst(speed);
			tick(0x9F68, 24);
			if ((int16_t)speed >= 0)
				tick(0x9F7C, 12);                      // BGE
			else
			{
				tick(0x9F6A, 12);
				turn_round(0x9F6A, 1, 0x031B);
				poke(0x01B4, fl::neg(peek(0x01B4)));
				tick(0x9F7C, 27);
			}
		}
	}
	set_b(0x031E, 0x01, 0x9F82);
	set_b(0x031C, 0x08, 0x9F88);
	c_restore(1);
	return T.r[0];
}

// F:AD70 (assembly, objects R0 and R1): R0 = 1 when object R0 is drawn in front of object
// R1.  Objects 0-3 (the skater) keep their order and stand for the skater against others;
// the object carrying the skater (0x0EA0) is behind it.  R1-R4 preserved (F152); C is only
// set by the comparison of two skater objects.
void sprite_in_front()
{
	c_save(3, 0xAD74);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x001F));
	tick(0xAD78, 18);
	T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x001F));
	tick(0xAD7C, 18);
	// MOVB @#0x0EA0,R2 at pc; CMP Rr,R2: the object carrying the skater?
	auto carrier = [](uint16_t pc, int r) {
		T.r[2] = sext8(peekb(0x0EA0));
		fl::movb((uint8_t)T.r[2]);
		tick((uint16_t)(pc + 4), 24);
		fl::cmp(T.r[r], T.r[2]);
		tick((uint16_t)(pc + 6), 12);
		return T.r[r] == T.r[2];
	};
	fl::cmp(T.r[0], 0x0004);
	tick(0xAD80, 18);
	if ((int16_t)T.r[0] < 4)
	{
		tick(0xAD82, 12);
		fl::cmp(T.r[1], 0x0004);
		tick(0xAD86, 18);
		if ((int16_t)T.r[1] < 4)
		{
			tick(0xAD88, 12);                          // two skater objects keep their order
			T.r[2] = 0;
			fl::clr();
			tick(0xAD8A, 12);
			fl::cmp(T.r[0], T.r[1]);
			tick(0xAD8C, 12);
			if ((int16_t)T.r[0] >= (int16_t)T.r[1])
				tick(0xAD90, 12);                      // BGE
			else
			{
				tick(0xAD8E, 12);
				T.r[2] = fl::inc(T.r[2]);
				tick(0xAD90, 12);
			}
			T.r[0] = T.r[2];
			fl::mov(T.r[0]);
			tick(0xAD92, 12);
			c_restore(3);
			return;
		}
		tick(0xAD94, 12);                              // BGE
		if (carrier(0xAD94, 1))
		{
			tick(0xADFE, 12);                          // BEQ: the skater is in front
			goto behind;
		}
		tick(0xAD9C, 12);
		T.r[0] = 0;                                    // the skater for object 0
		fl::clr();
		tick(0xAD9E, 12);
		tick(0xADB0, 12);                              // BR
	}
	else
	{
		tick(0xADA0, 12);                              // BGE
		fl::cmp(T.r[1], 0x0004);
		tick(0xADA4, 18);
		if ((int16_t)T.r[1] >= 4)
			tick(0xADB0, 12);                          // BGE
		else
		{
			tick(0xADA6, 12);
			if (carrier(0xADA6, 0))
			{
				tick(0xADF8, 12);                      // BEQ
				goto in_front;
			}
			tick(0xADAE, 12);
			T.r[1] = 0;
			fl::clr();
			tick(0xADB0, 12);
		}
	}
	T.r[0] = fl::asl(T.r[0]);                          // by the positions and the sizes
	tick(0xADB2, 12);
	T.r[2] = peek((uint16_t)(0x03F0 + T.r[0]));
	fl::mov(T.r[2]);
	tick(0xADB6, 27);
	if (!T.r[2])
	{
		tick(0xADF8, 12);                              // BEQ
		goto in_front;
	}
	tick(0xADB8, 12);
	T.r[1] = fl::asl(T.r[1]);
	tick(0xADBA, 12);
	T.r[3] = peek((uint16_t)(0x03F0 + T.r[1]));
	fl::mov(T.r[3]);
	tick(0xADBE, 27);
	if (!T.r[3])
	{
		tick(0xADFE, 12);                              // BEQ
		goto behind;
	}
	tick(0xADC0, 12);
	T.r[4] = peek((uint16_t)(0x0630 + T.r[0]));
	fl::mov(T.r[4]);
	tick(0xADC4, 27);
	T.r[4] = fl::add(T.r[4], peek((uint16_t)(T.r[2] + 8)));
	tick(0xADC8, 27);
	T.r[4] = fl::sub(T.r[4], peek((uint16_t)(0x0630 + T.r[1])));
	tick(0xADCC, 27);
	T.r[4] = fl::sub(T.r[4], peek((uint16_t)(T.r[3] + 8)));
	tick(0xADD0, 27);
	{
		const bool up = (T.r[4] & 0x8000) != 0;
		const uint16_t pc = up ? 0xADD2 : 0xADE4;
		tick(pc, 12);                                  // BPL
		const uint16_t mine = up ? 0x0C : 0x0A, other = up ? 0x0A : 0x0C;
		T.r[0] = peek((uint16_t)(0x0670 + T.r[0]));
		fl::mov(T.r[0]);
		tick((uint16_t)(pc + 4), 27);
		T.r[0] = fl::add(T.r[0], peek((uint16_t)(T.r[2] + mine)));
		tick((uint16_t)(pc + 8), 27);
		T.r[0] = fl::sub(T.r[0], peek((uint16_t)(0x0670 + T.r[1])));
		tick((uint16_t)(pc + 0x0C), 27);
		T.r[0] = fl::sub(T.r[0], peek((uint16_t)(T.r[3] + other)));
		tick((uint16_t)(pc + 0x10), 27);
		if (up)
			tick(0xADF4, 12);                          // BR
	}
	fl::tst(T.r[0]);
	tick(0xADF6, 12);
	if ((int16_t)T.r[0] < 0)
	{
		tick(0xADFE, 12);                              // BLT
		goto behind;
	}
	tick(0xADF8, 12);
in_front:
	T.r[0] = 0x0001;
	fl::mov(T.r[0]);
	tick(0xADFC, 18);
	c_restore(3);
	return;
behind:
	T.r[0] = 0;
	fl::clr();
	tick(0xAE00, 12);
	c_restore(3);
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
	T.r[0] = 0x0014;
	fl::mov(T.r[0]);
	tick(0x8138, 18);
	for (;;)
	{
		if (--T.r[0] == 0)
		{
			tick(0x813A, 18);                          // SOB R0
			break;
		}
		tick(0x8138, 18);
	}
	rts();
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
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xA8D4, 24);
	T.r[1] = peek((uint16_t)(T.r[6] + 4));             // the object
	fl::mov(T.r[1]);
	tick(0xA8D8, 27);
	T.r[0] = 0;
	fl::clr();
	tick(0xA8DA, 12);
	T.r[0] = (uint16_t)(T.r[0] | fl::logicb(peekb((uint16_t)(0x0390 + T.r[1]))));
	tick(0xA8DE, 27);
	T.r[0] = fl::asl(T.r[0]);
	tick(0xA8E0, 12);
	T.r[0] = peek((uint16_t)(0x0AF8 + T.r[0]));
	fl::mov(T.r[0]);
	tick(0xA8E4, 27);
	push(T.r[0]);
	fl::mov(T.r[0]);
	tick(0xA8E6, 24);
	push(0);
	fl::clr();
	tick(0xA8E8, 24);
	{
		const uint8_t v = peekb((uint16_t)(0x0390 + T.r[1]));
		fl::movb(v);
		pokeb(T.r[6], v);
	}
	tick(0xA8EC, 36);
	jsr(0xA8F0, 0xCDFA);                               // find_other_slot
	T.r[0] = sext8(peekb((uint16_t)(0x09B8 + T.r[0])));
	fl::movb((uint8_t)T.r[0]);
	tick(0xA8F4, 27);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x001F));
	tick(0xA8F8, 18);
	drop2(0xA8F8);
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0xA8FC, 18);
	rts();
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
