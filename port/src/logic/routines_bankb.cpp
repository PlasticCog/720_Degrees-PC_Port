// Rewritten routines of bank window B (segments B1, B3, B6, B8): the skater's position trail,
// object 4, the status line (score, cash meter, time bar, countdown), timers, pauses and hint
// messages, palette animation, landing scores, the view scrolling, the park modes and a shift
// helper.  Most of them were written in assembly in the original (registers clobbered, flags
// left by the last instruction), so registers and condition codes are reproduced; where the
// original pushes arguments and reuses their stack slots, the stack is built the same way
// (push, then call with no arguments).
#include "logic.h"
#include <exception>

namespace logic {
namespace {

using namespace t11rt;

uint16_t sext8(uint8_t v) { return (uint16_t)(int16_t)(int8_t)v; }

// condition codes of the instructions the fl:: helpers do not cover
uint16_t asr_w(uint16_t v)
{
	const uint16_t r = (uint16_t)((int16_t)v >> 1);
	f_nzvc(nzw(r) | (v & 1));
	f_shift();
	return r;
}
uint16_t asl_w(uint16_t v)
{
	const uint16_t r = (uint16_t)(v << 1);
	f_nzvc(nzw(r) | (v >> 15));
	f_shift();
	return r;
}
uint16_t neg_w(uint16_t v)
{
	const uint16_t r = (uint16_t)-v;
	f_nzvc(nzw(r) | (r == 0x8000 ? VF : 0) | (r ? CF : 0));
	return r;
}
// Cycles: each routine charges the original's instructions on the path it takes (cyc, and tick
// once per pass of a loop that may run long); the table charges only the part common to all
// paths, the final RTS or the register-save helper's epilogue.  Callees charge themselves.
inline void cyc(int n) { T.icount -= n; }
// For routines whose intermediate states the interrupts may see (the 32V interrupt can enter
// the frame processing at any instruction): one instruction of the original done, charge it
// and let interrupts in before the next one (at next), as the original does
inline void step(uint16_t next, int cycles) { tick(next, cycles); }
// JSR PC,@#addr of the original, returning to ret: the return address is pushed and
// interrupts may come in before the callee's first instruction, as in the original.  The
// JSR's own cycles are charged.  Returns R0.
uint16_t jsr(uint16_t ret, uint16_t addr)
{
	T.icount -= 30;
	push(ret);
	T.r[7] = addr;
	if (!sync(addr)) throw Unwound{};
	shadow_push(ret);
	dispatch();
	shadow_pop();
	if (T.r[7] != ret) throw Unwound{};
	return T.r[0];
}

// The routine's final RTS PC with its interrupt point at the return address, as in the
// original: the return address is taken, interrupts may come in there, and it is put back for
// the caller's wrapper, which returns with it (the table charges 0 for such routines)
void step_rts()
{
	const uint16_t ret = pop();
	T.icount -= 21;
	T.r[7] = ret;
	if (!sync(ret)) throw Unwound{};
	push(ret);
}

// routines whose body ends at their final RTS (stepped up to it): the RTS with its interrupt
// point follows
template <void (*F)()> void with_rts() { F(); step_rts(); }
template <uint16_t (*F)(const Args &)> uint16_t c_with_rts(const Args &a)
{
	const uint16_t r0 = F(a);
	step_rts();
	return r0;
}

// The register-save helper's frame instruction by instruction, for routines with interrupt
// points after every instruction: the routine's JSR R1,@#helper and the helper's pushes on
// entry (the helper's return address F142 / F150 / F160 / F172 ends on top), and on leave()
// the body's RTS and the helper's pops (the final RTS is in the table)
struct SteppedFrame {
	int last;
	SteppedFrame(int last_, uint16_t body) : last(last_)
	{
		static const uint16_t helper[6] = { 0, 0, 0xF138, 0xF144, 0xF152, 0xF162 };
		uint16_t pc = helper[last];
		T.icount -= 30;                                // JSR R1,@#helper
		push(T.r[1]);
		T.r[1] = body;
		T.r[7] = pc;
		if (!sync(pc)) throw Unwound{};
		for (int i = 2; i <= last; i++)                // MOV Ri,-(SP)
		{
			push(T.r[i]);
			pc = (uint16_t)(pc + 2);
			step(pc, 24);
		}
		push(T.r[1]);                                  // MOV R1,-(SP)
		pc = (uint16_t)(pc + 2);
		step(pc, 24);
		T.r[1] = rw((uint16_t)(T.r[6] + 2 * last));    // MOV n(SP),R1: the caller's R1
		pc = (uint16_t)(pc + 4);
		step(pc, 27);
		T.icount -= 30;                                // JSR PC,@(SP)+
		pop();
		push((uint16_t)(pc + 2));
		T.r[7] = body;
		if (!sync(body)) throw Unwound{};
	}
	void leave()
	{
		const uint16_t hret = pop();                   // the body's RTS
		T.icount -= 21;
		T.r[7] = hret;
		if (!sync(hret)) throw Unwound{};
		uint16_t pc = (uint16_t)(0xF17A - 2 * (last - 1));
		if (last < 5) step(pc, 12);                    // BR to the pops
		for (int i = last; i >= 2; i--)                // MOV (SP)+,Ri
		{
			T.r[i] = pop();
			pc = (uint16_t)(pc + 2);
			step(pc, 18);
		}
		T.r[1] = pop();                                // MOV (SP)+,R1
		step(0xF17C, 18);
	}
};

// the register-save helpers F:F138 / F144 / F152 / F162: entry (JSR R1 included) and exit
// (the body's RTS included)
constexpr int IN_F138 = 135, IN_F144 = 159, IN_F152 = 183, IN_F162 = 207;
constexpr int OUT_F138 = 90, OUT_F144 = 108, OUT_F152 = 126, OUT_F162 = 132;

// MOV / BIS / BIC / XOR / BIT: N and Z from the result, V clear, C unchanged
uint16_t lw(uint16_t r) { f_nzv(nzw(r)); return r; }
uint8_t lb(uint8_t r) { f_nzv(nzb(r)); return r; }

// The register-save helpers F:F138 / F144 / F152 / F162 (JSR R1,@#helper at the routine's
// entry): R1 and R2..Rlast are pushed and the helper's return address (F142 / F150 / F160 /
// F172) is on top while the body runs; on return the registers come back from the stack.  The
// frame is built the same way here, so the body and its callees run at the original's stack
// depth (interrupts taken meanwhile see the same stack, and code that keeps stack addresses,
// like F:CE24, finds the same values).
struct SavedRegs {
	int last;
	explicit SavedRegs(int last_) : last(last_)
	{
		static const uint16_t helper_ret[6] = { 0, 0, 0xF142, 0xF150, 0xF160, 0xF172 };
		push(T.r[1]);
		for (int i = 2; i <= last; i++) push(T.r[i]);
		push(helper_ret[last]);
	}
	~SavedRegs()
	{
		if (std::uncaught_exceptions()) return;
		T.r[6] = (uint16_t)(T.r[6] + 2);               // the helper's return address
		for (int i = last; i >= 2; i--) T.r[i] = pop();
		T.r[1] = pop();
	}
};

// ---------------------------------------------------------------------------------------------
// The position trail (B1).  Every few frames the skater's position is recorded in an 8-entry
// ring (x 0x0EDA, y 0x0EEA, height 0x0EFA, terrain byte 0x0F0A; next index 0x0F13, last
// written 0x0F14) once he has moved more than 0x600 from the entry at the index.  When he has
// to be put back (state 0x031F = 5), he is carried back to the recorded entries.

// B1:7854 (assembly): record the skater's position in the trail when the countdown 0x0F12 runs
// out, unless he is being put back (0x031F = 5), 0x032B is set or all low three bits of
// 0x0304 are set
void record_position_trail()
{
	uint16_t *R = T.r;
	const uint8_t st = peekb(0x031F);
	fl::cmpb(st, 5);
	step(0x785A, 30);
	if (st == 5)
	{
		step(0x78E2, 12);
		return;
	}
	step(0x785C, 12);
	const uint8_t b = peekb(0x032B);
	fl::tstb(b);
	step(0x7860, 24);
	if (b)
	{
		step(0x78E2, 12);
		return;
	}
	step(0x7862, 12);
	R[0] = lw(0x0007);
	step(0x7866, 18);
	R[0] = lw(R[0] & ~peek(0x0304));
	step(0x786A, 24);
	if (!R[0])
	{
		step(0x78E2, 12);
		return;
	}
	step(0x786C, 12);
	const uint8_t c = peekb(0x0F12);
	R[0] = sext8(c);
	fl::movb(c);
	step(0x7870, 24);
	R[0] = fl::dec(R[0]);
	step(0x7872, 12);
	pokeb(0x0F12, lb((uint8_t)R[0]));
	step(0x7876, 27);
	if ((int8_t)(uint8_t)R[0] > 0)
	{
		step(0x78E2, 12);
		return;
	}
	step(0x7878, 12);
	pokeb(0x0F12, lb(1));
	step(0x787E, 33);
	const uint8_t idx = peekb(0x0F13);
	R[2] = sext8(idx);
	fl::movb(idx);
	step(0x7882, 24);
	R[0] = lw(R[2]);
	step(0x7884, 12);
	R[0] = asl_w(R[0]);
	step(0x7886, 12);
	R[1] = lw(peek(0x06F0));
	step(0x788A, 24);
	R[1] = fl::sub(R[1], peek((uint16_t)(0x0EDA + R[0])));
	step(0x788E, 27);
	if ((int16_t)R[1] < 0)
	{
		step(0x7890, 12);
		R[1] = neg_w(R[1]);
		step(0x7892, 12);
	}
	else
		step(0x7892, 12);
	fl::cmp(R[1], 0x0600);
	step(0x7896, 18);
	if ((int16_t)R[1] > 0x0600)
		step(0x78AC, 12);
	else
	{
		step(0x7898, 12);
		R[1] = lw(peek(0x0730));
		step(0x789C, 24);
		R[1] = fl::sub(R[1], peek((uint16_t)(0x0EEA + R[0])));
		step(0x78A0, 27);
		if ((int16_t)R[1] < 0)
		{
			step(0x78A2, 12);
			R[1] = neg_w(R[1]);
			step(0x78A4, 12);
		}
		else
			step(0x78A4, 12);
		R[0] = lw(R[1]);
		step(0x78A6, 12);
		fl::cmp(R[0], 0x0600);
		step(0x78AA, 18);
		if ((int16_t)R[0] <= 0x0600)
		{
			step(0x78E2, 12);
			return;
		}
		step(0x78AC, 12);
	}
	pokeb(0x0F14, lb((uint8_t)R[2]));
	step(0x78B0, 27);
	R[0] = lw(R[2]);
	step(0x78B2, 12);
	R[0] = fl::inc(R[0]);
	step(0x78B4, 12);
	R[1] = lw(R[0]);
	step(0x78B6, 12);
	R[1] = lw(R[1] & 0x0007);
	step(0x78BA, 18);
	pokeb(0x0F13, 0);
	fl::clr();
	step(0x78BE, 27);
	pokeb(0x0F13, lb((uint8_t)(peekb(0x0F13) | (uint8_t)R[1])));
	step(0x78C2, 27);
	const uint8_t n = peekb(0x0F13);
	R[1] = sext8(n);
	fl::movb(n);
	step(0x78C6, 24);
	R[0] = lw(R[1]);
	step(0x78C8, 12);
	R[0] = asl_w(R[0]);
	step(0x78CA, 12);
	poke((uint16_t)(0x0EDA + R[0]), lw(peek(0x06F0)));
	step(0x78D0, 42);
	poke((uint16_t)(0x0EEA + R[0]), lw(peek(0x0730)));
	step(0x78D6, 42);
	poke((uint16_t)(0x0EFA + R[0]), lw(peek(0x06B0)));
	step(0x78DC, 42);
	pokeb((uint16_t)(0x0F0A + R[1]), lb(peekb(0x0327)));
	step(0x78E2, 42);
}

// B1:78E4: start putting the skater back: objects 0-3 are reset at his position (F:843E),
// state 0x031F = 5 with 30 frames of delay (0x02FA), the trail steps back one entry.
// Returns the previous entry index - 1.
uint16_t start_trail_return(const Args &)
{
	uint16_t *R = T.r;
	push(R[1]);
	fl::mov(R[1]);
	step(0x78E6, 24);
	push(lw(peek(0x06F0)));
	step(0x78EA, 36);
	push(lw(peek(0x0730)));
	step(0x78EE, 36);
	push(lw(peek(0x06B0)));
	step(0x78F2, 36);
	jsr(0x78F6, 0x843E);
	poke(0x03B0, lw(0x50FA));
	step(0x78FC, 33);
	poke(0x03B4, lw(0x50FA));
	step(0x7902, 33);
	pokeb(0x031F, lb(5));
	step(0x7908, 33);
	const uint8_t c = peekb(0x0F14);
	R[0] = sext8(c);
	fl::movb(c);
	step(0x790C, 24);
	R[0] = fl::dec(R[0]);
	step(0x790E, 12);
	R[1] = lw(R[0]);
	step(0x7910, 12);
	R[1] = lw(R[1] & 0x0007);
	step(0x7914, 18);
	pokeb(0x0F14, 0);
	fl::clr();
	step(0x7918, 27);
	pokeb(0x0F14, lb((uint8_t)(peekb(0x0F14) | (uint8_t)R[1])));
	step(0x791C, 27);
	poke(0x02FA, lw(0x001E));
	step(0x7922, 33);
	R[6] = fl::add(R[6], 6);
	step(0x7926, 18);
	R[1] = pop();
	fl::mov(R[1]);
	step(0x7928, 18);
	return R[0];
}

// B1:792A (assembly): one frame of putting the skater back.  The delay 0x02FA counts down
// elsewhere; at 1 he is lifted (vz 0x05B0 = 0xC0).  While rising above the trail entry 0x0F14
// he is steered towards it (vx/vy +-0x100/0x80); once there he drops (vz = -0xC0) and on
// landing at the entry's height he is placed on it with its terrain.
void trail_return_step()
{
	uint16_t *R = T.r;
	R[0] = lw(peek(0x02FA));
	step(0x792E, 24);
	if (R[0])
	{
		step(0x7930, 12);
		fl::cmp(R[0], 1);
		step(0x7934, 18);
		if (R[0] != 1)
		{
			step(0x7A06, 12);
			step(0x7A4E, 12);                          // BNE at 7A06
			return;
		}
		step(0x7936, 12);
		poke(0x05B0, lw(0x00C0));
		step(0x793C, 33);
		poke(0x03B2, lw(0x57AC));
		step(0x7942, 33);
		R[0] = lw(1);
		step(0x7946, 18);
		pokeb(0x0791, lb(1));
		step(0x794A, 27);
		pokeb(0x0771, lb(1));
		step(0x794E, 27);
		push(2);
		fl::mov(2);
		step(0x7952, 30);
		push(2);
		fl::mov(2);
		step(0x7956, 30);
		R[0] = jsr(0x795A, 0xB602);
		poke(0x03B4, lw(0x50FA));
		step(0x7960, 33);
		poke(0x02FA, 0);
		fl::clr();
		step(0x7964, 27);
		const uint16_t s = pop(), d = pop();
		fl::cmp(s, d);
		step(0x7966, 24);
		return;
	}
	step(0x7968, 12);
	R[0] = lw(peek(0x05B0));
	step(0x796C, 24);
	if ((int16_t)R[0] > 0)
	{
		step(0x796E, 12);
		const uint8_t e = peekb(0x0F14);
		R[0] = sext8(e);
		fl::movb(e);
		step(0x7972, 24);
		R[0] = asl_w(R[0]);
		step(0x7974, 12);
		R[1] = lw(peek(0x06B0));
		step(0x7978, 24);
		R[1] = fl::sub(R[1], peek((uint16_t)(0x0EFA + R[0])));
		step(0x797C, 27);
		R[1] = fl::sub(R[1], 0x0C00);
		step(0x7980, 18);
		if ((int16_t)R[1] < 0)
		{
			step(0x7A4E, 12);
			return;
		}
		step(0x7982, 12);
		poke(0x05B0, 0);
		fl::clr();
		step(0x7986, 27);
		poke(0x04F0, lw(peek((uint16_t)(0x0EDA + R[0]))));
		step(0x798C, 42);
		uint16_t d = fl::sub(peek(0x04F0), peek(0x06F0));
		poke(0x04F0, d);
		step(0x7992, 39);
		T.psw &= ~(CF | VF);
		step(0x7994, 18);
		if ((int16_t)d <= 0)
		{
			step(0x799E, 12);
			poke(0x04F0, lw(0xFF00));
			step(0x79A4, 33);
		}
		else
		{
			step(0x7996, 12);
			poke(0x04F0, lw(0x0100));
			step(0x799C, 33);
			step(0x79A4, 12);
		}
		poke(0x0570, lw(peek((uint16_t)(0x0EEA + R[0]))));
		step(0x79AA, 42);
		d = fl::sub(peek(0x0570), peek(0x0730));
		poke(0x0570, d);
		step(0x79B0, 39);
		T.psw &= ~(CF | VF);
		step(0x79B2, 18);
		if ((int16_t)d <= 0)
		{
			step(0x79BC, 12);
			poke(0x0570, lw(0xFF80));
			step(0x79C2, 33);
		}
		else
		{
			step(0x79B4, 12);
			poke(0x0570, lw(0x0080));
			step(0x79BA, 33);
		}
		return;
	}
	step(0x79C4, 12);
	if (R[0] == 0)
	{
		step(0x79C6, 12);
		R[2] = lw(peek(0x04F0));
		step(0x79CA, 24);
		if (R[2])
		{
			step(0x79CC, 12);
			const uint8_t e = peekb(0x0F14);
			R[0] = sext8(e);
			fl::movb(e);
			step(0x79D0, 24);
			R[0] = asl_w(R[0]);
			step(0x79D2, 12);
			R[1] = lw(peek((uint16_t)(0x0EDA + R[0])));
			step(0x79D6, 27);
			R[1] = fl::sub(R[1], peek(0x06F0));
			step(0x79DA, 24);
			R[1] = lw(R[1] ^ R[2]);
			step(0x79DC, 12);
			if ((int16_t)R[1] < 0)
			{
				step(0x79DE, 12);
				poke(0x04F0, 0);
				fl::clr();
				step(0x79E2, 27);
			}
			else
				step(0x79E2, 12);
		}
		else
			step(0x79E2, 12);
		R[2] = lw(peek(0x0570));
		step(0x79E6, 24);
		if (R[2])
		{
			step(0x79E8, 12);
			const uint8_t e = peekb(0x0F14);
			R[0] = sext8(e);
			fl::movb(e);
			step(0x79EC, 24);
			R[0] = asl_w(R[0]);
			step(0x79EE, 12);
			R[1] = lw(peek((uint16_t)(0x0EEA + R[0])));
			step(0x79F2, 27);
			R[1] = fl::sub(R[1], peek(0x0730));
			step(0x79F6, 24);
			R[1] = lw(R[1] ^ R[2]);
			step(0x79F8, 12);
			if ((int16_t)R[1] < 0)
			{
				step(0x79FA, 12);
				poke(0x0570, 0);
				fl::clr();
				step(0x79FE, 27);
			}
			else
				step(0x79FE, 12);
		}
		else
			step(0x79FE, 12);
		R[0] = lw(peek(0x04F0));
		step(0x7A02, 24);
		R[0] = lw(R[0] | peek(0x0570));
		step(0x7A06, 24);
		if (R[0])
		{
			step(0x7A4E, 12);
			return;
		}
		step(0x7A08, 12);
		poke(0x05B0, lw(0xFF40));
		step(0x7A0E, 33);
		return;
	}
	step(0x7A10, 12);
	const uint8_t e = peekb(0x0F14);
	R[0] = sext8(e);
	fl::movb(e);
	step(0x7A14, 24);
	R[0] = asl_w(R[0]);
	step(0x7A16, 12);
	R[1] = lw(peek(0x06B0));
	step(0x7A1A, 24);
	R[1] = fl::sub(R[1], peek((uint16_t)(0x0EFA + R[0])));
	step(0x7A1E, 27);
	T.psw &= ~(CF | VF);
	step(0x7A20, 18);
	if ((int16_t)R[1] > 0)
	{
		step(0x7A4E, 12);
		return;
	}
	step(0x7A22, 12);
	poke(0x05B0, 0);
	fl::clr();
	step(0x7A26, 27);
	push(lw(peek((uint16_t)(0x0EDA + R[0]))));
	step(0x7A2A, 39);
	push(lw(peek((uint16_t)(0x0EEA + R[0]))));
	step(0x7A2E, 39);
	push(lw(peek((uint16_t)(0x0EFA + R[0]))));
	step(0x7A32, 39);
	jsr(0x7A36, 0x843E);
	pokeb(0x032A, 0);
	fl::clr();
	step(0x7A3A, 27);
	const uint8_t e2 = peekb(0x0F14);
	R[0] = sext8(e2);
	fl::movb(e2);
	step(0x7A3E, 24);
	pokeb(0x0327, lb(peekb((uint16_t)(0x0F0A + R[0]))));
	step(0x7A44, 42);
	pokeb(0x0F12, lb(0x1E));
	step(0x7A4A, 33);
	R[6] = fl::add(R[6], 6);
	step(0x7A4E, 18);
}

// ---------------------------------------------------------------------------------------------
// Object 4 (B1)

// B1:7AA2 (assembly): steps of object 4 by its state byte 0x07B4: state 3 starts animation
// 0x5772 + 2 * 0x0394 and moves to 4; in state 4 it stays while near the skater (screen
// distance), otherwise after the delay 0x0438 it moves to 5 (animation 0x5786); in later
// states it is removed (F:B56E) when the delay runs out
void object4_step()
{
	uint16_t *R = T.r;
	R[0] = 0;
	fl::clr();
	step(0x7AA4, 12);
	R[0] = lb(peekb(0x07B4));
	step(0x7AA8, 24);
	fl::cmp(R[0], 3);
	step(0x7AAC, 18);
	if ((int16_t)R[0] < 3)
	{
		step(0x7B2C, 12);
		return;
	}
	step(0x7AAE, 12);
	if (R[0] == 3)
	{
		step(0x7AB0, 12);
		R[0] = 0;
		fl::clr();
		step(0x7AB2, 12);
		R[0] = lb(peekb(0x0394));
		step(0x7AB6, 24);
		R[0] = asl_w(R[0]);
		step(0x7AB8, 12);
		R[0] = fl::add(R[0], 0x5772);
		step(0x7ABC, 18);
		poke(0x03B8, lw(R[0]));
		step(0x7AC0, 27);
		pokeb(0x0774, 0);
		fl::clr();
		step(0x7AC4, 27);
		pokeb(0x07B4, fl::incb(peekb(0x07B4)));
		step(0x7AC8, 27);
		pokeb(0x0494, lb((uint8_t)(peekb(0x0494) | 0x18)));
		step(0x7ACE, 33);
		return;
	}
	step(0x7AD0, 12);
	fl::cmp(R[0], 4);
	step(0x7AD4, 18);
	if (R[0] == 4)
	{
		step(0x7AD6, 12);
		R[0] = lw(peek(0x0638));
		step(0x7ADA, 24);
		R[0] = fl::sub(R[0], peek(0x0630));
		step(0x7ADE, 24);
		if ((int16_t)R[0] < 0)
		{
			step(0x7AE0, 12);
			R[0] = neg_w(R[0]);
			step(0x7AE2, 12);
		}
		else
			step(0x7AE2, 12);
		fl::cmp(R[0], 0x0800);
		step(0x7AE6, 18);
		bool count = (int16_t)R[0] > 0x0800;
		if (count)
			step(0x7AFE, 12);
		else
		{
			step(0x7AE8, 12);
			R[0] = lw(peek(0x0678));
			step(0x7AEC, 24);
			R[0] = fl::sub(R[0], peek(0x0670));
			step(0x7AF0, 24);
			if ((int16_t)R[0] < 0)
			{
				step(0x7AF2, 12);
				R[0] = neg_w(R[0]);
				step(0x7AF4, 12);
			}
			else
				step(0x7AF4, 12);
			R[0] = fl::sub(R[0], 0x0200);
			step(0x7AF8, 18);
			fl::cmp(R[0], 0x0A00);
			step(0x7AFC, 18);
			if (R[0] <= 0x0A00)
			{
				step(0x7B2C, 12);
				return;
			}
			step(0x7AFE, 12);
		}
		const uint16_t n = fl::dec(peek(0x0438));
		poke(0x0438, n);
		step(0x7B02, 27);
		if (n)
		{
			step(0x7B2C, 12);
			return;
		}
		step(0x7B04, 12);
		poke(0x03B8, lw(0x5786));
		step(0x7B0A, 33);
		pokeb(0x07B4, fl::incb(peekb(0x07B4)));
		step(0x7B0E, 27);
		pokeb(0x0494, lb((uint8_t)(peekb(0x0494) | 0x18)));
		step(0x7B14, 33);
		poke(0x0438, lw(0x0003));
		step(0x7B1A, 33);
		return;
	}
	step(0x7B1C, 12);
	const uint16_t n = fl::dec(peek(0x0438));
	poke(0x0438, n);
	step(0x7B20, 27);
	if (n)
	{
		step(0x7B2C, 12);
		return;
	}
	step(0x7B22, 12);
	push(4);
	fl::mov(4);
	step(0x7B26, 30);
	R[0] = jsr(0x7B2A, 0xB56E);
	fl::tst(pop());
	step(0x7B2C, 18);
}

// B1:7B3E (assembly)  start_object4(int tile): called on map tiles with bit 0x40; once object 1
// is high enough (height 0x06B2 >= 0x800) object 4 starts above it, offset by the tables at
// 0x7B2E/0x7B36 for (tile >> 3) & 3 (the argument slot is overwritten with that), in state 5
// with sound 0x7A
void start_object4()
{
	uint16_t *R = T.r;
	const uint16_t z = peek(0x06B2);
	fl::cmp(z, 0x0800);
	step(0x7B44, 30);
	if ((int16_t)z < 0x0800)
	{
		step(0x7BBA, 12);
		return;
	}
	step(0x7B46, 12);
	pokeb(0x0374, lb(0x54));
	step(0x7B4C, 33);
	poke(0x03B8, lw(0x578A));
	step(0x7B52, 33);
	poke(0x0438, lw(0x0017));
	step(0x7B58, 33);
	R[0] = lw(4);
	step(0x7B5C, 18);
	pokeb(0x0794, lb(4));
	step(0x7B60, 27);
	pokeb(0x0774, lb(4));
	step(0x7B64, 27);
	const uint16_t arg = (uint16_t)(R[6] + 2);
	R[0] = lw(rw(arg));
	step(0x7B68, 27);
	R[0] = asr_w(R[0]);
	step(0x7B6A, 12);
	R[0] = asr_w(R[0]);
	step(0x7B6C, 12);
	R[0] = asr_w(R[0]);
	step(0x7B6E, 12);
	ww(arg, lw(R[0]));
	step(0x7B72, 30);
	ww(arg, lw(rw(arg) & 0x0003));
	step(0x7B78, 36);
	R[0] = lw(rw(arg));
	step(0x7B7C, 27);
	R[0] = asl_w(R[0]);
	step(0x7B7E, 12);
	poke(0x06F8, lw(peek((uint16_t)(0x7B2E + R[0]))));
	step(0x7B84, 42);
	poke(0x06F8, fl::add(peek(0x06F8), peek(0x06F0)));
	step(0x7B8A, 39);
	poke(0x0738, lw(peek((uint16_t)(0x7B36 + R[0]))));
	step(0x7B90, 42);
	poke(0x0738, fl::add(peek(0x0738), peek(0x0730)));
	step(0x7B96, 39);
	poke(0x06B8, lw(peek(0x06B2)));
	step(0x7B9C, 39);
	poke(0x06B8, fl::add(peek(0x06B8), 0x0600));
	step(0x7BA2, 33);
	pokeb(0x07B4, lb(5));
	step(0x7BA8, 33);
	push(4);
	fl::mov(4);
	step(0x7BAC, 30);
	jsr(0x7BB0, 0xB4B6);
	ww(R[6], 0x007A);
	fl::mov(0x007A);
	step(0x7BB4, 27);
	R[0] = jsr(0x7BB8, 0x8E78);
	fl::tst(pop());
	step(0x7BBA, 18);
}

// ---------------------------------------------------------------------------------------------
// Status line and timers (B3)

// B3:79D4 (assembly): when flagged (0x0FA2 bit 0x10) draw the countdown 0x0F1C (two digits,
// F:BA9A) in the text layer, top left in Skate City and on screen 7, bottom left elsewhere.
// Interrupt points after every instruction, as in the original (the text cursor is shared).
void draw_countdown()
{
	const uint16_t f = lw(peek(0x0FA2) & 0x0010);
	step(0x79DA, 30);
	if (!f)
	{
		step(0x7A38, 12);
		return;
	}
	step(0x79DC, 12);
	pokeb(0x0FA2, lb((uint8_t)(peekb(0x0FA2) & ~0x10)));
	step(0x79E2, 33);
	jsr(0x79E6, 0x8068);                               // select_video_bank0
	poke(0x0188, lw(0xC000));
	step(0x79EC, 33);
	T.r[0] = 0;
	fl::clr();
	step(0x79EE, 12);
	T.r[0] = lb(peekb(0x0F59));
	step(0x79F2, 24);
	fl::tst(T.r[0]);
	step(0x79F4, 12);
	bool top = T.r[0] == 0;
	if (top)
		step(0x79FC, 12);
	else
	{
		step(0x79F6, 12);
		fl::cmp(T.r[0], 7);
		step(0x79FA, 18);
		top = T.r[0] == 7;
		step(top ? 0x79FC : 0x7A04, 12);
	}
	if (top)
	{
		poke(0x0186, lw(0x213C));
		step(0x7A02, 33);
		step(0x7A0A, 12);
	}
	else
	{
		poke(0x0186, lw(0x35BC));
		step(0x7A0A, 33);
	}
	push(lw(peek(0x0F1C)));
	step(0x7A0E, 36);
	push(0);
	fl::clr();
	step(0x7A10, 24);
	push(2);
	fl::mov(2);
	step(0x7A14, 30);
	push(1);
	fl::mov(1);
	step(0x7A18, 30);
	push(0);
	fl::clr();
	step(0x7A1A, 24);
	T.r[0] = jsr(0x7A1E, 0xBA9A);
	const uint16_t n = peek(0x0F1C);
	fl::cmp(n, 0x000A);
	step(0x7A24, 30);
	if ((int16_t)n < 10)
	{
		step(0x7A26, 12);
		poke(0x0186, fl::sub(peek(0x0186), 8));
		step(0x7A2C, 33);
		T.r[0] = lw(0x0020);
		step(0x7A30, 18);
		jsr(0x7A34, 0x8B52);                           // draw_large_char
	}
	else
		step(0x7A34, 12);
	T.r[6] = fl::add(T.r[6], 10);
	step(0x7A38, 18);
}

// B3:7A3A (assembly): count the countdown 0x0F1C down once a second (0x0F1E frames) and flag
// it for redrawing.  On screens 1 and 2 instead: every 16 frames, in mode 3 of 0x0F26 and
// unless the skater is being put back, 0x0F20 counts down and the time bar is flagged.
void tick_countdown()
{
	uint16_t *R = T.r;
	const uint8_t level = peekb(0x0F59);
	fl::cmpb(level, 1);
	step(0x7A40, 30);
	bool other = level == 1;
	if (!other)
	{
		step(0x7A42, 12);
		fl::cmpb(level, 2);
		step(0x7A48, 30);
		other = level == 2;
	}
	if (other)
	{
		step(0x7A74, 12);
		const uint16_t t = peek(0x0F20);
		fl::tst(t);
		step(0x7A78, 24);
		if ((int16_t)t <= 0)
		{
			step(0x7A9C, 12);
			return;
		}
		step(0x7A7A, 12);
		const uint16_t f = lw(peek(0x01C4) & 0x000F);
		step(0x7A80, 30);
		if (f)
		{
			step(0x7A9C, 12);
			return;
		}
		step(0x7A82, 12);
		const uint8_t m = peekb(0x0F26);
		fl::cmpb(m, 3);
		step(0x7A88, 30);
		if (m != 3)
		{
			step(0x7A9C, 12);
			return;
		}
		step(0x7A8A, 12);
		const uint8_t st = peekb(0x031F);
		fl::cmpb(st, 5);
		step(0x7A90, 30);
		if (st == 5)
		{
			step(0x7A9C, 12);
			return;
		}
		step(0x7A92, 12);
		poke(0x0F20, fl::dec(peek(0x0F20)));
		step(0x7A96, 27);
		pokeb(0x0FA2, lb((uint8_t)(peekb(0x0FA2) | 0x04)));
		step(0x7A9C, 33);
		return;
	}
	step(0x7A4A, 12);
	const uint8_t f = peekb(0x0F1E);
	R[0] = sext8(f);
	fl::movb(f);
	step(0x7A4E, 24);
	if (!f)
	{
		step(0x7A9C, 12);
		return;
	}
	step(0x7A50, 12);
	R[1] = lw(R[0]);
	step(0x7A52, 12);
	R[1] = fl::dec(R[1]);
	step(0x7A54, 12);
	pokeb(0x0F1E, lb((uint8_t)R[1]));
	step(0x7A58, 27);
	if ((uint8_t)R[1])
	{
		step(0x7A9C, 12);
		return;
	}
	step(0x7A5A, 12);
	const uint16_t n = fl::dec(peek(0x0F1C));
	poke(0x0F1C, n);
	step(0x7A5E, 27);
	if (!n)
	{
		step(0x7A60, 12);
		pokeb(0x0F1E, 0);
		fl::clr();
		step(0x7A64, 27);
		step(0x7A6C, 12);
	}
	else
	{
		step(0x7A66, 12);
		pokeb(0x0F1E, lb(0x3C));
		step(0x7A6C, 33);
	}
	pokeb(0x0FA2, lb((uint8_t)(peekb(0x0FA2) | 0x10)));
	step(0x7A72, 33);
}

// B3:7DD6  update_table_0fb4(int n): n = 0 sets the 32 bytes at 0x0FB4 to 3 and 0x0FE1 from
// the table at 0x4649 (bank A 0x2400) for 0x0F2E, returning 0x0F2E; otherwise n is added to
// each byte (at most 3) and -1 is returned.  The banks are restored.
uint16_t update_table_0fb4(const Args &a)
{
	uint16_t *R = T.r;
	SteppedFrame frame(3, 0x7DDA);
	R[2] = lw(a.u(0));
	step(0x7DDE, 27);
	if (R[2])
	{
		step(0x7E24, 12);
		R[0] = lw(0x001F);
		step(0x7E28, 18);
		for (;;)
		{
			R[1] = 0;
			fl::clr();
			step(0x7E2A, 12);
			R[1] = lb(peekb((uint16_t)(0x0FB4 + R[0])));
			step(0x7E2E, 27);
			R[1] = fl::add(R[1], R[2]);
			step(0x7E30, 12);
			fl::cmp(R[1], 3);
			step(0x7E34, 18);
			if ((int16_t)R[1] > 3)
			{
				step(0x7E36, 12);
				R[1] = lw(3);
				step(0x7E3A, 18);
			}
			else
				step(0x7E3A, 12);
			pokeb((uint16_t)(0x0FB4 + R[0]), lb((uint8_t)R[1]));
			step(0x7E3E, 30);
			R[0] = fl::dec(R[0]);
			step(0x7E40, 12);
			if ((int16_t)R[0] < 0) break;
			step(0x7E28, 12);
		}
		step(0x7E42, 12);
		const uint16_t r0 = R[0];
		frame.leave();
		return r0;
	}
	step(0x7DE0, 12);
	R[1] = lw(peek(0x01D2));
	step(0x7DE4, 24);
	R[3] = lw(peek(0x01D4));
	step(0x7DE8, 24);
	R[0] = lw(0x001F);
	step(0x7DEC, 18);
	for (;;)
	{
		pokeb((uint16_t)(0x0FB4 + R[0]), lb(3));
		step(0x7DF2, 36);
		R[0] = fl::dec(R[0]);
		step(0x7DF4, 12);
		if ((int16_t)R[0] < 0) break;
		step(0x7DEC, 12);
	}
	step(0x7DF6, 12);
	R[0] = lw(0x2400);
	step(0x7DFA, 18);
	poke(0x01D2, lw(R[0]));
	step(0x7DFE, 27);
	poke(0x1400, lw(R[0]));
	step(0x7E02, 27);
	R[0] = 0;
	fl::clr();
	step(0x7E04, 12);
	R[0] = lb(peekb(0x0F2E));
	step(0x7E08, 24);
	pokeb(0x0FE1, lb(peekb((uint16_t)(0x4649 + R[0]))));
	step(0x7E0E, 42);
	poke(0x01D2, lw(R[1]));
	step(0x7E12, 27);
	poke(0x01D4, lw(R[3]));
	step(0x7E16, 27);
	poke(0x1400, lw(peek(0x01D2)));
	step(0x7E1C, 39);
	poke(0x1402, lw(peek(0x01D4)));
	step(0x7E22, 39);
	const uint16_t r0 = R[0];
	frame.leave();
	return r0;
}

// B3:7E44: except on screen 4, the frames since 0x0FB0 (less 30, / 8) are converted by
// F:F064; a valid result n selects the amount at 0xFCFC + 2n, which is scored (F:EF24) on
// screens 0-2 and added to 0x0F34 (time bar flagged) on the others
uint16_t frame_delta_bonus(const Args &)
{
	uint16_t *R = T.r;
	push(R[1]);
	fl::mov(R[1]);
	step(0x7E46, 24);
	const uint8_t v7E4C = peekb(0x0F59);
	fl::cmpb(v7E4C, 4);
	step(0x7E4C, 30);
	if (v7E4C != 4)
	{
		step(0x7E4E, 12);
		R[0] = lw(peek(0x01C4));
		step(0x7E52, 24);
		R[0] = fl::sub(R[0], peek(0x0FB0));
		step(0x7E56, 24);
		R[1] = lw(R[0]);
		step(0x7E58, 12);
		R[1] = fl::sub(R[1], 0x001E);
		step(0x7E5C, 18);
		R[0] = lw(R[1]);
		step(0x7E5E, 12);
		R[0] = asr_w(R[0]);
		step(0x7E60, 12);
		R[0] = asr_w(R[0]);
		step(0x7E62, 12);
		R[0] = asr_w(R[0]);
		step(0x7E64, 12);
		push(R[0]);
		fl::mov(R[0]);
		step(0x7E66, 24);
		R[0] = jsr(0x7E6A, 0xF064);
		fl::tst(pop());
		step(0x7E6C, 18);
		fl::tst(R[0]);
		step(0x7E6E, 12);
		if ((int16_t)R[0] >= 0)
		{
			step(0x7E70, 12);
			R[1] = lw(R[0]);
			step(0x7E72, 12);
			R[1] = asl_w(R[1]);
			step(0x7E74, 12);
			const uint8_t v7E7A = peekb(0x0F59);
			fl::cmpb(v7E7A, 3);
			step(0x7E7A, 30);
			if (v7E7A < 3)
			{
				step(0x7E7C, 12);
				push(lw(peek((uint16_t)(0xFCFC + R[1]))));
				step(0x7E80, 39);
				R[0] = jsr(0x7E84, 0xEF24);
				fl::tst(pop());
				step(0x7E86, 18);
				step(0x7E94, 12);
			}
			else
			{
				step(0x7E88, 12);
				poke(0x0F34, fl::add(peek(0x0F34), peek((uint16_t)(0xFCFC + R[1]))));
				step(0x7E8E, 42);
				pokeb(0x0FA2, lb((uint8_t)(peekb(0x0FA2) | 0x04)));
				step(0x7E94, 33);
			}
		}
		else
			step(0x7E94, 12);
	}
	else
		step(0x7E94, 12);
	R[1] = pop();
	fl::mov(R[1]);
	step(0x7E96, 18);
	return R[0];
}

// B3:7F52: F:DD5C, then while neither 0x0F26 nor 0x0EA0 is set the game timer counts: every
// 0x0FA7 frames (counter 0x0FA6) one unit is taken off (F:F028 add_time(-1))
uint16_t tick_game_timer(const Args &)
{
	uint16_t *R = T.r;
	SteppedFrame frame(5, 0x7F56);
	jsr(0x7F5A, 0xDD5C);
	const uint8_t m = peekb(0x0F26);
	R[0] = sext8(m);
	fl::movb(m);
	step(0x7F5E, 24);
	R[0] = (uint16_t)((R[0] & 0xFF00) | lb((uint8_t)(R[0] | peekb(0x0EA0))));
	step(0x7F62, 24);
	R[0] = lw(R[0] & 0x00FF);
	step(0x7F66, 18);
	if (!R[0])
	{
		step(0x7F68, 12);
		const uint8_t n = fl::decb(peekb(0x0FA6));
		pokeb(0x0FA6, n);
		step(0x7F6C, 27);
		if (!n)
		{
			step(0x7F6E, 12);
			pokeb(0x0FA6, lb(peekb(0x0FA7)));
			step(0x7F74, 39);
			push(0xFFFF);
			fl::mov(0xFFFF);
			step(0x7F78, 30);
			R[0] = jsr(0x7F7C, 0xF028);                // add_time
			fl::tst(pop());
			step(0x7F7E, 18);
		}
		else
			step(0x7F7E, 12);
	}
	else
		step(0x7F7E, 12);
	const uint16_t r0 = R[0];
	frame.leave();
	return r0;
}

// B3:739A  draw_cash_meter(int player): a meter of up to 4 segments for the player's cash
// (0x0256 for the current player 0x01F5, else 0x0294) against the thresholds at 0x523E, at
// the text position from the table 0x51E2, closed by a 2 x 2 end piece.  Returns the number
// of segments left unlit.
uint16_t draw_cash_meter(const Args &a)
{
	uint16_t *R = T.r;
	SteppedFrame frame(4, 0x739E);
	const uint16_t player = a.u(0);
	jsr(0x73A2, 0x8068);                               // select_video_bank0
	poke(0x0188, lw(0x8000));
	step(0x73A8, 33);
	R[0] = lw(player);
	step(0x73AC, 27);
	R[0] = asl_w(R[0]);
	step(0x73AE, 12);
	poke(0x0186, lw(peek((uint16_t)(0x51E2 + R[0]))));
	step(0x73B4, 42);
	poke(0x0186, fl::add(peek(0x0186), 0x2000));
	step(0x73BA, 33);
	R[0] = lw(4);
	step(0x73BE, 18);
	const uint8_t cur = peekb(0x01F5);
	R[1] = sext8(cur);
	fl::movb(cur);
	step(0x73C2, 24);
	fl::cmp(player, R[1]);
	step(0x73C6, 27);
	if (player == R[1])
	{
		step(0x73C8, 12);
		R[1] = lw(peek(0x0256));
		step(0x73CC, 24);
		step(0x73D2, 12);
	}
	else
	{
		step(0x73CE, 12);
		R[1] = lw(peek(0x0294));
		step(0x73D2, 24);
	}
	R[4] = lw(peek(0x0188));
	step(0x73D6, 24);
	R[3] = lw(R[4]);
	step(0x73D8, 12);
	R[3] = lw(R[3] | 0x0087);
	step(0x73DC, 18);
	for (;;)
	{
		R[2] = lw(R[0]);
		step(0x73DE, 12);
		R[2] = asl_w(R[2]);
		step(0x73E0, 12);
		const uint16_t v73E4 = peek((uint16_t)(0x523E + R[2]));
		fl::cmp(R[1], v73E4);
		step(0x73E4, 27);
		if ((int16_t)R[1] >= (int16_t)v73E4)
		{
			step(0x7406, 12);
			break;
		}
		step(0x73E6, 12);
		fl::tst(R[0]);
		step(0x73E8, 12);
		if ((int16_t)R[0] <= 0)
		{
			step(0x7406, 12);
			break;
		}
		step(0x73EA, 12);
		R[2] = lw(peek(0x0186));
		step(0x73EE, 24);
		poke((uint16_t)(R[2] + 0x82), lw(R[3]));
		step(0x73F2, 30);
		poke((uint16_t)(R[2] + 0x80), lw(R[3]));
		step(0x73F6, 30);
		poke((uint16_t)(R[2] + 2), lw(R[3]));
		step(0x73FA, 30);
		poke(R[2], lw(R[3]));
		step(0x73FC, 21);
		poke(0x0186, fl::add(peek(0x0186), 4));
		step(0x7402, 33);
		R[0] = fl::dec(R[0]);
		step(0x7404, 12);
		step(0x73DC, 12);
	}
	fl::cmp(R[0], 3);
	step(0x740A, 18);
	if ((int16_t)R[0] < 3)
	{
		step(0x740C, 12);
		R[1] = lw(peek(0x0186));
		step(0x7410, 24);
		poke((uint16_t)(R[1] + 0x80), lw(R[3]));
		step(0x7414, 30);
		poke(R[1], lw(R[3]));
		step(0x7416, 21);
		poke(0x0186, fl::add(peek(0x0186), 2));
		step(0x741C, 33);
	}
	else
		step(0x741C, 12);
	R[1] = lw(peek(0x0186));
	step(0x7420, 24);
	static const uint16_t offset[4] = { 0, 2, 0x80, 0x82 };
	static const uint16_t next[8] = { 0x7422, 0x7426, 0x742A, 0x7430, 0x7434, 0x743A, 0x743E, 0x7444 };
	for (int i = 0; i < 4; i++)
	{
		const uint16_t p = (uint16_t)(R[1] + offset[i]);
		poke(p, lw(R[4]));
		step(next[2 * i], i ? 30 : 21);
		poke(p, lw((uint16_t)(peek(p) | (0x00FA + i))));
		step(next[2 * i + 1], i ? 36 : 27);
	}
	const uint16_t r0 = R[0];
	frame.leave();
	return r0;
}

// B3:77B4 (assembly): redraw the parts of the status line flagged in 0x0FA2 (bit 0: score
// 0x023E/0x0240, bit 1: high score 0x0244/0x0246 in the second colour unless 0x036C is set,
// bit 3: cash 0x0256 and its meter) unless 0x02BF is set; every 32 frames (0x01C2) 0x0FE2 is
// halved.  Two words of locals are reserved; the flags end from comparing them.
void update_status_display()
{
	uint16_t *R = T.r;
	fl::cmp(rw((uint16_t)(R[6] - 2)), rw((uint16_t)(R[6] - 4)));
	R[6] = (uint16_t)(R[6] - 4);                       // CMP -(SP),-(SP)
	step(0x77B6, 30);
	const uint8_t busy = peekb(0x02BF);
	fl::tstb(busy);
	step(0x77BA, 24);
	if (!busy)
	{
		step(0x77BC, 12);
		jsr(0x77C0, 0x8068);                           // select_video_bank0
		const uint16_t f0 = lw(peek(0x0FA2) & 0x0001);
		step(0x77C6, 30);
		if (f0)
		{
			step(0x77C8, 12);
			pokeb(0x0FA2, fl::decb(peekb(0x0FA2)));
			step(0x77CC, 27);
			const uint8_t me = peekb(0x01F5);
			R[0] = sext8(me);
			fl::movb(me);
			step(0x77D0, 24);
			if (me)
			{
				step(0x77DA, 12);
				poke(0x0186, lw(0x2262));
				step(0x77E0, 33);
			}
			else
			{
				step(0x77D2, 12);
				poke(0x0186, lw(0x2204));
				step(0x77D8, 33);
				step(0x77E0, 12);
			}
			R[0] = asl_w(R[0]);
			step(0x77E2, 12);
			poke(0x0188, lw(peek((uint16_t)(0x51DE + R[0]))));
			step(0x77E8, 42);
			push(lw(peek(0x023E)));
			step(0x77EC, 36);
			push(lw(peek(0x0240)));
			step(0x77F0, 36);
			push(6);
			fl::mov(6);
			step(0x77F4, 30);
			push(1);
			fl::mov(1);
			step(0x77F8, 30);
			push(rw(R[6]));
			fl::mov(1);
			step(0x77FA, 30);
			R[0] = jsr(0x77FE, 0xBA9A);
			R[6] = fl::add(R[6], 10);
			step(0x7802, 18);
		}
		else
			step(0x7802, 12);
		const uint16_t f1 = lw(peek(0x0FA2) & 0x0002);
		step(0x7808, 30);
		bool high = f1 != 0;
		if (high)
		{
			step(0x780A, 12);
			const uint8_t h = peekb(0x036C);
			fl::tstb(h);
			step(0x780E, 24);
			high = !h;
		}
		if (high)
		{
			step(0x7810, 12);
			pokeb(0x0FA2, lb((uint8_t)(peekb(0x0FA2) & ~0x02)));
			step(0x7816, 33);
			const uint16_t pz = peek(0x01FC);
			fl::tst(pz);
			step(0x781A, 24);
			if (!pz)
			{
				step(0x781C, 12);
				R[0] = jsr(0x7820, 0xA8FE);
			}
			else
				step(0x7820, 12);
			poke(0x0188, lw(0xA000));
			step(0x7826, 33);
			poke(0x0186, lw(0x3746));
			step(0x782C, 33);
			push(lw(peek(0x0244)));
			step(0x7830, 36);
			push(lw(peek(0x0246)));
			step(0x7834, 36);
			push(8);
			fl::mov(8);
			step(0x7838, 30);
			push(0);
			fl::clr();
			step(0x783A, 24);
			push(1);
			fl::mov(1);
			step(0x783E, 30);
			R[0] = jsr(0x7842, 0xBA96);
			pokeb(0x0EA6, lb((uint8_t)(peekb(0x0EA6) | 0x01)));
			step(0x7848, 33);
			poke(0x0EAA, lw(0x00B4));
			step(0x784E, 33);
			R[6] = fl::add(R[6], 10);
			step(0x7852, 18);
		}
		else
			step(0x7852, 12);
		const uint16_t f3 = lw(peek(0x0FA2) & 0x0008);
		step(0x7858, 30);
		if (f3)
		{
			step(0x785A, 12);
			pokeb(0x0FA2, lb((uint8_t)(peekb(0x0FA2) & ~0x08)));
			step(0x7860, 33);
			poke(0x018A, lw(0x00A8));
			step(0x7866, 33);
			poke(0x0188, lw(0x8000));
			step(0x786C, 33);
			const uint8_t me = peekb(0x01F5);
			R[0] = sext8(me);
			fl::movb(me);
			step(0x7870, 24);
			R[0] = asl_w(R[0]);
			step(0x7872, 12);
			poke(0x0186, lw(peek((uint16_t)(0x51E2 + R[0]))));
			step(0x7878, 42);
			poke(0x0186, fl::add(peek(0x0186), 0x2000));
			step(0x787E, 33);
			push(lw(peek(0x0256)));
			step(0x7882, 36);
			push(0);
			fl::clr();
			step(0x7884, 24);
			push(6);
			fl::mov(6);
			step(0x7888, 30);
			push(1);
			fl::mov(1);
			step(0x788C, 30);
			push(rw(R[6]));
			fl::mov(1);
			step(0x788E, 30);
			R[0] = jsr(0x7892, 0xBA9A);
			const uint8_t me2 = peekb(0x01F5);
			R[0] = sext8(me2);
			fl::movb(me2);
			step(0x7896, 24);
			ww(R[6], R[0]);
			fl::mov(R[0]);
			step(0x7898, 21);
			R[0] = jsr(0x789C, 0x739A);                // draw_cash_meter
			R[6] = fl::add(R[6], 10);
			step(0x78A0, 18);
		}
		else
			step(0x78A0, 12);
		poke(0x018A, lw(0x0001));
		step(0x78A6, 33);
		const uint16_t fr = lw(peek(0x01C2) & 0x001F);
		step(0x78AC, 30);
		if (!fr)
		{
			step(0x78AE, 12);
			R[0] = 0;
			fl::clr();
			step(0x78B0, 12);
			R[0] = lb(peekb(0x0FE2));
			step(0x78B4, 24);
			R[0] = asr_w(R[0]);
			step(0x78B6, 12);
			pokeb(0x0FE2, lb((uint8_t)R[0]));
			step(0x78BA, 27);
		}
		else
			step(0x78BA, 12);
	}
	else
		step(0x78BA, 12);
	fl::cmp(rw(R[6]), rw((uint16_t)(R[6] + 2)));
	R[6] = (uint16_t)(R[6] + 4);
	step(0x78BC, 24);
}

// B3:78BE (assembly): when flagged (0x0FA2 bit 2) and unless 0x0F36 is set, draw the time
// bar: 16 cells from right to left for the timer 0x0254 (or, in the modes of 0x0F26, 0x0F20
// or a scaled 0x0F34), full, empty or partly filled (8 steps per cell).  When the time is up
// in Skate City (and not in mode 0x0180 bit 0, 0x0F26 or state 0x031F = 1) the game moves to
// state 1 and the time-up messages are shown, with sound 0xA1.  It runs in the frame's
// vertical blank processing, which the 32V interrupt may enter at any instruction (the text
// cursor 0x0186 and the video window are shared): interrupts are let in after every
// instruction, as in the original.
void update_time_bar()
{
	T.r[0] = peek(0x0254);
	fl::mov(T.r[0]);
	step(0x78C2, 24);
	bool time_up = false;
	if (!T.r[0])
	{
		step(0x78C4, 12);
		const uint8_t level = peekb(0x0F59);
		fl::tstb(level);
		step(0x78C8, 24);
		time_up = level == 0;
		step(time_up ? 0x7982 : 0x78CA, 12);
	}
	else
		step(0x78CA, 12);
	if (time_up)
	{
		const uint16_t m0 = lw(peek(0x0180) & 0x0001);
		step(0x7988, 30);
		if (m0)
		{
			step(0x79D2, 12);
			return;
		}
		step(0x798A, 12);
		const uint8_t m = peekb(0x0F26);
		fl::tstb(m);
		step(0x798E, 24);
		if (m)
		{
			step(0x79D2, 12);
			return;
		}
		step(0x7990, 12);
		const uint8_t st = peekb(0x031F);
		fl::cmpb(st, 1);
		step(0x7996, 30);
		if (st == 1)
		{
			step(0x79D2, 12);
			return;
		}
		step(0x7998, 12);
		pokeb(0x018F, 1);
		fl::movb(1);
		step(0x799E, 33);
		push(0x5DEE);
		fl::mov(0x5DEE);
		step(0x79A2, 30);
		push(1);
		fl::mov(1);
		step(0x79A6, 30);
		jsr(0x79AA, 0xEAD4);
		T.r[0] = 0x00E4;
		fl::mov(0x00E4);
		step(0x79AE, 18);
		jsr(0x79B2, 0x8AB6);
		poke(0x0188, 0x6000);
		fl::mov(0x6000);
		step(0x79B8, 33);
		ww(T.r[6], 0x5F06);
		fl::mov(0x5F06);
		step(0x79BC, 27);
		push(0);
		fl::clr();
		step(0x79BE, 24);
		jsr(0x79C2, 0xEAD4);
		ww(T.r[6], 0x00A1);
		fl::mov(0x00A1);
		step(0x79C6, 27);
		T.r[0] = jsr(0x79CA, 0x8E78);                  // queue_sound
		jsr(0x79CE, 0xE02E);
		T.r[6] = fl::add(T.r[6], 6);
		step(0x79D2, 18);
		return;
	}
	const uint16_t f = lw(peek(0x0FA2) & 0x0004);
	step(0x78D0, 30);
	if (!f)
	{
		step(0x7996, 12);
		step(0x79D2, 12);                              // BEQ at 7996
		return;
	}
	step(0x78D2, 12);
	uint8_t m = peekb(0x0F36);
	fl::tstb(m);
	step(0x78D6, 24);
	if (m)
	{
		step(0x79D2, 12);
		return;
	}
	step(0x78D8, 12);
	m = peekb(0x0F26);
	fl::tstb(m);
	step(0x78DC, 24);
	if (m)
	{
		step(0x78DE, 12);
		const uint8_t k = peekb(0x027A);
		fl::cmpb(k, 2);
		step(0x78E4, 30);
		if (k < 2)
		{
			step(0x78E6, 12);
			T.r[0] = lw(peek(0x0F20));
			step(0x78EA, 24);
			step(0x7904, 12);
		}
		else
		{
			step(0x78EC, 12);
			T.r[0] = lw(peek(0x0F34));
			step(0x78F0, 24);
			T.r[0] = asr_w(T.r[0]);
			step(0x78F2, 12);
			T.r[0] = asr_w(T.r[0]);
			step(0x78F4, 12);
			T.r[0] = asr_w(T.r[0]);
			step(0x78F6, 12);
			const uint8_t v78FC = peekb(0x027A);
			fl::cmpb(v78FC, 2);
			step(0x78FC, 30);
			if (v78FC == 2)
			{
				step(0x78FE, 12);
				T.r[0] = asr_w(T.r[0]);
				step(0x7900, 12);
			}
			else
				step(0x7900, 12);
			T.r[0] = asr_w(T.r[0]);
			step(0x7902, 12);
			T.r[0] = asr_w(T.r[0]);
			step(0x7904, 12);
		}
	}
	else
		step(0x7904, 12);
	pokeb(0x0FA2, lb((uint8_t)(peekb(0x0FA2) & ~0x04)));
	step(0x790A, 33);
	T.r[3] = lw(T.r[0]);
	step(0x790C, 12);
	T.r[3] = asr_w(T.r[3]);
	step(0x790E, 12);
	T.r[3] = asr_w(T.r[3]);
	step(0x7910, 12);
	T.r[3] = asr_w(T.r[3]);
	step(0x7912, 12);
	T.r[4] = lw(T.r[0]);
	step(0x7914, 12);
	T.r[4] = lw(T.r[4] & 0x0007);
	step(0x7918, 18);
	jsr(0x791C, 0x8068);                               // select_video_bank0
	poke(0x0188, 0x4000);
	fl::mov(0x4000);
	step(0x7922, 33);
	T.r[3] = asl_w(T.r[3]);
	step(0x7924, 12);
	poke(0x0186, 0x2230);
	fl::mov(0x2230);
	step(0x792A, 33);
	m = peekb(0x0F26);
	fl::tstb(m);
	step(0x792E, 24);
	if (m)
	{
		step(0x7930, 12);
		poke(0x0186, fl::sub(peek(0x0186), 4));
		step(0x7936, 33);
		T.r[0] = 0x0026;
		fl::mov(0x0026);
		step(0x793A, 18);
		step(0x7940, 12);
	}
	else
	{
		step(0x793C, 12);
		T.r[0] = 0x001E;
		fl::mov(0x001E);
		step(0x7940, 18);
	}
	poke(0x0186, fl::add(peek(0x0186), T.r[0]));
	step(0x7944, 27);
	step(0x797C, 12);
	for (;;)
	{
		fl::tst(T.r[0]);
		step(0x797E, 12);
		if ((int16_t)T.r[0] < 0)
		{
			step(0x7980, 12);
			return;
		}
		step(0x7946, 12);
		T.r[2] = lw(peek(0x0186));
		step(0x794A, 24);
		fl::cmp(T.r[0], T.r[3]);
		step(0x794C, 12);
		if ((int16_t)T.r[0] > (int16_t)T.r[3])
		{
			step(0x794E, 12);
			T.r[1] = lw(0x009E);
			step(0x7952, 18);
			step(0x7962, 12);
		}
		else
		{
			step(0x7954, 12);
			if ((int16_t)T.r[0] < (int16_t)T.r[3])
			{
				step(0x7956, 12);
				T.r[1] = lw(0x0096);
				step(0x795A, 18);
				step(0x7962, 12);
			}
			else
			{
				step(0x795C, 12);
				T.r[1] = lw(0x009E);
				step(0x7960, 18);
				T.r[1] = fl::sub(T.r[1], T.r[4]);
				step(0x7962, 12);
			}
		}
		poke(T.r[2], lw(peek(0x0188)));
		step(0x7966, 33);
		poke(T.r[2], lw((uint16_t)(peek(T.r[2]) | T.r[1])));
		step(0x7968, 21);
		const uint16_t below = (uint16_t)(T.r[2] + 0x80);
		poke(below, lw(peek(T.r[2])));
		step(0x796C, 36);
		poke(below, fl::add(peek(below), 9));
		step(0x7972, 36);
		poke(0x0186, fl::sub(peek(0x0186), 2));
		step(0x7978, 33);
		T.r[0] = fl::sub(T.r[0], 2);
		step(0x797C, 18);
	}
}

// ---------------------------------------------------------------------------------------------
// Scroll blocks, pauses and hint messages (B6)

// B6:7B82 (assembly): follow the scroll position (0x01AC, 0x01AE) in blocks of 0x4000: when x
// leaves the block 0x09A0, 0x0996/0x0998 move by 0x10; when y leaves the block 0x09A2, the
// row 0x099A/0x099C moves by one, wrapping by 12 rows past 0x099E or below 0 (then
// 0x0996/0x0998 move by 0x50 and 0x0E9B by one).  0x09A4/0x09A6 keep the offsets in the
// block; 0x0E99 counts the block changes.  It runs in the frame's vertical blank processing,
// which the 32V interrupt may enter at any instruction, and the map objects of that interrupt
// (F:BD78) read 0x09A4/0x09A6, intermediate values included: so interrupts are let in after
// every instruction, as in the original (step).
void track_scroll_blocks()
{
	T.r[0] = peek(0x01AC);
	fl::mov(T.r[0]);
	step(0x7B86, 24);
	poke(0x09A4, T.r[0]);
	fl::mov(T.r[0]);
	step(0x7B8A, 27);
	poke(0x09A4, fl::sub(peek(0x09A4), peek(0x09A0)));
	step(0x7B90, 39);
	const uint16_t v7B96 = peek(0x09A4);
	fl::cmp(v7B96, 0x4000);
	step(0x7B96, 30);
	if (v7B96 >= 0x4000)
	{
		step(0x7B98, 12);
		const uint16_t d = peek(0x09A4);
		fl::tst(d);
		step(0x7B9C, 24);
		if ((int16_t)d > 0)
		{
			step(0x7B9E, 12);
			poke(0x0996, fl::add(peek(0x0996), 0x0010));
			step(0x7BA4, 33);
			poke(0x0998, fl::add(peek(0x0998), 0x0010));
			step(0x7BAA, 33);
			step(0x7BB8, 12);
		}
		else
		{
			step(0x7BAC, 12);
			poke(0x0996, fl::sub(peek(0x0996), 0x0010));
			step(0x7BB2, 33);
			poke(0x0998, fl::sub(peek(0x0998), 0x0010));
			step(0x7BB8, 33);
		}
		poke(0x09A0, T.r[0]);
		fl::mov(T.r[0]);
		step(0x7BBC, 27);
		poke(0x09A0, lw(peek(0x09A0) & ~0x3FFF));
		step(0x7BC2, 33);
		poke(0x09A4, T.r[0]);
		fl::mov(T.r[0]);
		step(0x7BC6, 27);
		poke(0x09A4, fl::sub(peek(0x09A4), peek(0x09A0)));
		step(0x7BCC, 39);
		const uint8_t c = peekb(0x0E99);
		T.r[0] = sext8(c);
		fl::movb(c);
		step(0x7BD0, 24);
		T.r[0] = fl::inc(T.r[0]);
		step(0x7BD2, 12);
		pokeb(0x0E99, lb((uint8_t)T.r[0]));
		step(0x7BD6, 27);
	}
	else
		step(0x7BD6, 12);
	T.r[1] = peek(0x01AE);
	fl::mov(T.r[1]);
	step(0x7BDA, 24);
	T.r[0] = peek(0x09A2);
	fl::mov(T.r[0]);
	step(0x7BDE, 24);
	T.r[0] = fl::sub(T.r[0], T.r[1]);
	step(0x7BE0, 12);
	poke(0x09A6, T.r[0]);
	fl::mov(T.r[0]);
	step(0x7BE4, 27);
	poke(0x09A6, fl::add(peek(0x09A6), 0x3FFF));
	step(0x7BEA, 33);
	const uint16_t v7BF0 = peek(0x09A6);
	fl::cmp(v7BF0, 0x4000);
	step(0x7BF0, 30);
	if (v7BF0 < 0x4000)
	{
		step(0x7C78, 12);                              // BCS to the RTS (the RTS is in the table)
		return;
	}
	step(0x7BF2, 12);
	const uint16_t d = peek(0x09A6);
	fl::tst(d);
	step(0x7BF6, 24);
	bool wrapped;
	if ((int16_t)d < 0)
	{
		step(0x7BF8, 12);
		poke(0x099A, fl::inc(peek(0x099A)));
		step(0x7BFC, 27);
		poke(0x099C, fl::inc(peek(0x099C)));
		step(0x7C00, 27);
		fl::cmp(peek(0x099C), peek(0x099E));
		step(0x7C06, 36);
		wrapped = (int16_t)peek(0x099C) > (int16_t)peek(0x099E);
		if (wrapped)
		{
			step(0x7C08, 12);
			poke(0x099C, fl::sub(peek(0x099C), 0x000C));
			step(0x7C0E, 33);
			poke(0x099A, fl::sub(peek(0x099A), 0x000C));
			step(0x7C14, 33);
			poke(0x0998, fl::add(peek(0x0998), 0x0050));
			step(0x7C1A, 33);
			poke(0x0996, fl::add(peek(0x0996), 0x0050));
			step(0x7C20, 33);
			const uint8_t c = peekb(0x0E9B);
			T.r[0] = sext8(c);
			fl::movb(c);
			step(0x7C24, 24);
			T.r[0] = fl::dec(T.r[0]);
			step(0x7C26, 12);
			step(0x7C50, 12);
		}
		else
			step(0x7C54, 12);
	}
	else
	{
		step(0x7C28, 12);
		poke(0x099A, fl::dec(peek(0x099A)));
		step(0x7C2C, 27);
		poke(0x099C, fl::dec(peek(0x099C)));
		step(0x7C30, 27);
		wrapped = (int16_t)peek(0x099C) < 0;
		if (wrapped)
		{
			step(0x7C32, 12);
			poke(0x099C, fl::add(peek(0x099C), 0x000C));
			step(0x7C38, 33);
			poke(0x099A, fl::add(peek(0x099A), 0x000C));
			step(0x7C3E, 33);
			poke(0x0998, fl::sub(peek(0x0998), 0x0050));
			step(0x7C44, 33);
			poke(0x0996, fl::sub(peek(0x0996), 0x0050));
			step(0x7C4A, 33);
			const uint8_t c = peekb(0x0E9B);
			T.r[0] = sext8(c);
			fl::movb(c);
			step(0x7C4E, 24);
			T.r[0] = fl::inc(T.r[0]);
			step(0x7C50, 12);
		}
		else
			step(0x7C54, 12);
	}
	if (wrapped)
	{
		pokeb(0x0E9B, lb((uint8_t)T.r[0]));
		step(0x7C54, 27);
	}
	poke(0x09A2, T.r[1]);
	fl::mov(T.r[1]);
	step(0x7C58, 27);
	poke(0x09A2, lw(peek(0x09A2) & ~0x3FFF));
	step(0x7C5E, 33);
	T.r[0] = peek(0x09A2);
	fl::mov(T.r[0]);
	step(0x7C62, 24);
	T.r[0] = fl::sub(T.r[0], T.r[1]);
	step(0x7C64, 12);
	poke(0x09A6, T.r[0]);
	fl::mov(T.r[0]);
	step(0x7C68, 27);
	poke(0x09A6, fl::add(peek(0x09A6), 0x3FFF));
	step(0x7C6E, 33);
	const uint8_t c = peekb(0x0E99);
	T.r[0] = sext8(c);
	fl::movb(c);
	step(0x7C72, 24);
	T.r[0] = fl::inc(T.r[0]);
	step(0x7C74, 12);
	pokeb(0x0E99, lb((uint8_t)T.r[0]));
	step(0x7C78, 27);                                  // the RTS follows (in the table)
}

// B6:7C84 (assembly): the pause counter 0x01FC.  Positive: it counts down (a new press of
// the button in 0x0193 bit 7 takes 40 more off while at most 120 are left, 0x0FE6 keeps the
// button state); when it runs out, F:A8FE and the interrupts are set up again (F:81AA) and
// the frames since the pause began (0x0200) are added to 0x0FB0 and 0x0FAC.  Negative: the
// pause begins (F:820C), it is made positive and the frame is recorded in 0x0200.
// Interrupt points after every instruction, as in the original.
void step_pause()
{
	T.r[0] = lw(peek(0x01FC));
	step(0x7C88, 24);
	if ((int16_t)T.r[0] <= 0)
	{
		step(0x7CE4, 12);
		if (T.r[0] == 0)
		{
			step(0x7CF4, 12);
			return;
		}
		step(0x7CE6, 12);
		poke(0x01FC, neg_w(peek(0x01FC)));
		step(0x7CEA, 27);
		T.r[0] = jsr(0x7CEE, 0x820C);                  // install_interrupts_b
		poke(0x0200, lw(peek(0x01C4)));
		step(0x7CF4, 39);
		return;
	}
	step(0x7C8A, 12);
	poke(0x01FC, fl::dec(peek(0x01FC)));
	step(0x7C8E, 27);
	const uint16_t v7C94 = peek(0x01FC);
	fl::cmp(v7C94, 0x0078);
	step(0x7C94, 30);
	if ((int16_t)v7C94 > 0x0078)
	{
		step(0x7CF4, 12);
		return;
	}
	step(0x7C96, 12);
	uint8_t b = peekb(0x0FE6);
	fl::tstb(b);
	step(0x7C9A, 24);
	bool tested = false;                               // at 7CB2 with the SUB's flags
	uint16_t v7CB2 = 0;
	if (b)
		step(0x7CAE, 12);
	else
	{
		step(0x7C9C, 12);
		b = peekb(0x0193);
		fl::tstb(b);
		step(0x7CA0, 24);
		if (!(b & 0x80))
			step(0x7CAE, 12);
		else
		{
			step(0x7CA2, 12);
			const uint16_t v = fl::sub(peek(0x01FC), 0x0028);
			poke(0x01FC, v);
			step(0x7CA8, 33);
			tested = (int16_t)v >= 0;
			v7CB2 = v;
			if (tested)
				step(0x7CB2, 12);
			else
			{
				step(0x7CAA, 12);
				poke(0x01FC, 0);
				fl::clr();
				step(0x7CAE, 27);
			}
		}
	}
	if (!tested)
	{
		v7CB2 = peek(0x01FC);
		fl::tst(v7CB2);
		step(0x7CB2, 24);
	}
	if (!v7CB2)
	{
		step(0x7CB4, 12);
		T.r[0] = jsr(0x7CB8, 0xA8FE);
		T.r[0] = jsr(0x7CBC, 0x81AA);                  // install_interrupts_a
		T.r[0] = lw(peek(0x01C4));
		step(0x7CC0, 24);
		T.r[0] = fl::sub(T.r[0], peek(0x0200));
		step(0x7CC4, 24);
		poke(0x0FB0, fl::add(peek(0x0FB0), T.r[0]));
		step(0x7CC8, 27);
		poke(0x0FAC, fl::add(peek(0x0FAC), T.r[0]));
		step(0x7CCC, 27);
		poke(0x01FC, 0);
		fl::clr();
		step(0x7CD0, 27);
	}
	else
		step(0x7CD0, 12);
	b = peekb(0x0193);
	fl::tstb(b);
	step(0x7CD4, 24);
	if (b & 0x80)
	{
		step(0x7CD6, 12);
		pokeb(0x0FE6, 1);
		fl::movb(1);
		step(0x7CDC, 33);
	}
	else
	{
		step(0x7CDE, 12);
		pokeb(0x0FE6, 0);
		fl::clr();
		step(0x7CE2, 27);
	}
}

// B6:7CF6: hint messages (F:A974, shown for 200 frames with a pause of 180, 0x01FC).  When the
// delay 0x01FE runs out and no message is showing, the hint requested in 0x0273 (1-5,
// recorded in 0x0272) is shown, otherwise one by the park tickets 0x025B / cash 0x0256: a
// park still to be visited (from a random start) or a general hint.  Text table 0x0184 is
// switched to 0x5CD2 meanwhile and back to 0x5AC8.  Only in mode 0 of 0x0F26 (or with hint 4).
uint16_t show_hint(const Args &)
{
	uint16_t *R = T.r;
	SteppedFrame frame(3, 0x7CFA);
	auto finish = [&]() {                              // 7E16
		poke(0x0184, lw(0x5AC8));
		step(0x7E1C, 33);
		const uint16_t r0 = R[0];
		frame.leave();
		return r0;
	};
	const uint8_t v7CFE = peekb(0x0F26);
	fl::tstb(v7CFE);
	step(0x7CFE, 24);
	if (v7CFE)
	{
		step(0x7D00, 12);
		const uint8_t v7D06 = peekb(0x0273);
		fl::cmpb(v7D06, 4);
		step(0x7D06, 30);
		if (v7D06 != 4)
		{
			step(0x7D08, 12);
			const uint16_t r0 = R[0];
			frame.leave();
			return r0;
		}
		step(0x7D0A, 12);
	}
	else
		step(0x7D0A, 12);
	poke(0x0184, lw(0x5CD2));
	step(0x7D10, 33);
	const uint16_t v7D14 = peek(0x01FE);
	fl::tst(v7D14);
	step(0x7D14, 24);
	if (!v7D14)
	{
		step(0x7D3C, 12);
		step(0x7E16, 12);
		return finish();
	}
	step(0x7D16, 12);
	const uint16_t v7D1A = fl::dec(peek(0x01FE));
	poke(0x01FE, v7D1A);
	step(0x7D1A, 27);
	if (v7D1A)
	{
		step(0x7E16, 12);
		return finish();
	}
	step(0x7D1C, 12);
	fl::cmpb(peekb(0x0336), 0xFF);
	step(0x7D22, 30);
	bool check = peekb(0x0336) == 0xFF;
	if (check)
		step(0x7D2C, 12);
	else
	{
		step(0x7D24, 12);
		check = lw(peek(peek(0x0348)) & 0x0001) != 0;
		step(0x7D2A, 39);
		step(check ? 0x7D2C : 0x7D38, 12);
	}
	bool busy = true;                                  // a message is showing
	if (check)
	{
		const uint8_t m = peekb(0x0358);
		R[0] = sext8(m);
		fl::movb(m);
		step(0x7D30, 24);
		R[0] = asl_w(R[0]);
		step(0x7D32, 12);
		fl::tst(peek((uint16_t)(0x0362 + R[0])));
		step(0x7D36, 27);
		busy = peek((uint16_t)(0x0362 + R[0])) != 0;
		step(busy ? 0x7D38 : 0x7D3E, 12);
	}
	if (busy)
	{
		pokeb(0x0273, 0);
		fl::clr();
		step(0x7D3C, 27);
		step(0x7E16, 12);
		return finish();
	}
	poke(0x01FC, lw(0xFF4C));
	step(0x7D44, 33);
	R[0] = 0;
	fl::clr();
	step(0x7D46, 12);
	R[0] = lb(peekb(0x0273));
	step(0x7D4A, 24);
	fl::tst(R[0]);
	step(0x7D4C, 12);
	if (R[0])
	{
		step(0x7D4E, 12);
		R[0] = fl::dec(R[0]);
		step(0x7D50, 12);
		R[0] = asl_w(R[0]);
		step(0x7D52, 12);
		// jump table at 0x7E1E for hints 1-5 (B6:7E28 sets no others; larger values would jump
		// outside the table in the original)
		f_nzvc(0);
		static const uint16_t entry[5] = { 0x7D56, 0x7D5C, 0x7D62, 0x7D68, 0x7D6E };
		static const uint16_t hint_text[5] = { 0x7860, 0x7872, 0x7886, 0x786A, 0x787C };
		const uint16_t h = (uint16_t)(R[0] >> 1);
		if (h > 4) return finish();
		step(entry[h], 27);
		push(hint_text[h]);
		fl::mov(hint_text[h]);
		step((uint16_t)(entry[h] + 4), 30);
		if (h < 4) step(0x7D72, 12);
		push(0x00C8);
		fl::mov(0x00C8);
		step(0x7D76, 30);
		push(1);
		fl::mov(1);
		step(0x7D7A, 30);
		R[0] = jsr(0x7D7E, 0xA974);
		R[0] = 0;
		fl::clr();
		step(0x7D80, 12);
		R[0] = lb(peekb(0x0273));
		step(0x7D84, 24);
		pokeb(0x0272, lb((uint8_t)(peekb(0x0272) | peekb((uint16_t)(0xFC73 + R[0])))));
		step(0x7D8A, 42);
		pokeb(0x0273, 0);
		fl::clr();
		step(0x7D8E, 27);
		step(0x7E12, 12);
		R[6] = fl::add(R[6], 6);
		step(0x7E16, 18);
		return finish();
	}
	step(0x7D90, 12);
	const uint8_t v7D94 = peekb(0x0F31);
	fl::tstb(v7D94);
	step(0x7D94, 24);
	uint16_t text = 0;
	if (!v7D94)
	{
		step(0x7D96, 12);
		text = 0x7818;
	}
	else
	{
		step(0x7D9C, 12);
		const uint8_t v7DA0 = peekb(0x025B);
		fl::tstb(v7DA0);
		step(0x7DA0, 24);
		if (!v7DA0)
		{
			step(0x7DA2, 12);
			text = 0x7822;
		}
		else
		{
			step(0x7DA8, 12);
			R[0] = jsr(0x7DAC, 0x9152);                // next_random
			R[2] = lw(R[0]);
			step(0x7DAE, 12);
			R[1] = 0;
			fl::clr();
			step(0x7DB0, 12);
			R[0] = 0;
			fl::clr();
			step(0x7DB2, 12);
			R[0] = lb(peekb(0x0F31));
			step(0x7DB6, 24);
			const uint8_t g = peekb((uint16_t)(0xFE7C + R[0]));
			R[0] = sext8(g);
			fl::movb(g);
			step(0x7DBA, 27);
			R[0] = lw(R[0] & 0x00FF);
			step(0x7DBE, 18);
			R[3] = 0;
			fl::clr();
			step(0x7DC0, 12);
			R[3] = lb(peekb((uint16_t)(0xFE70 + R[0])));
			step(0x7DC4, 27);
			const uint16_t v7DC8 = peek(0x0256);
			fl::cmp(v7DC8, R[3]);
			step(0x7DC8, 24);
			if ((int16_t)v7DC8 > (int16_t)R[3])
			{
				step(0x7DCA, 12);
				R[3] = lw(4);
				step(0x7DCE, 18);
				for (;;)
				{
					R[0] = lw(R[2]);
					step(0x7DD0, 12);
					R[0] = fl::inc(R[0]);
					step(0x7DD2, 12);
					R[2] = lw(R[0]);
					step(0x7DD4, 12);
					R[2] = lw(R[2] & 0x0003);
					step(0x7DD8, 18);
					fl::tstb(peekb((uint16_t)(0x025C + R[2])));
					step(0x7DDC, 27);
					bool show = !peekb((uint16_t)(0x025C + R[2]));
					if (show)
					{
						step(0x7DDE, 12);
						fl::tst(R[1]);
						step(0x7DE0, 12);
						show = !R[1];
					}
					if (show)
					{
						step(0x7DE2, 12);
						push(0);
						fl::clr();
						step(0x7DE4, 24);
						wb(R[6], lb(peekb((uint16_t)(0x7C80 + R[2]))));
						step(0x7DE8, 36);
						ww(R[6], fl::add(rw(R[6]), 0x7830));
						step(0x7DEC, 27);
						push(0x00C8);
						fl::mov(0x00C8);
						step(0x7DF0, 30);
						push(0);
						fl::clr();
						step(0x7DF2, 24);
						R[0] = jsr(0x7DF6, 0xA974);
						R[1] = lw(1);
						step(0x7DFA, 18);
						R[6] = fl::add(R[6], 6);
						step(0x7DFE, 18);
					}
					else
						step(0x7DFE, 12);
					R[3] = (uint16_t)(R[3] - 1);       // SOB
					if (!R[3]) break;
					step(0x7DCE, 18);
				}
				step(0x7E00, 18);
			}
			else
				step(0x7E00, 12);
			fl::tst(R[1]);
			step(0x7E02, 12);
			if (R[1])
			{
				step(0x7E16, 12);
				return finish();
			}
			step(0x7E04, 12);
			push(0x7858);
			fl::mov(0x7858);
			step(0x7E08, 30);
		}
	}
	if (text)
	{
		push(text);
		fl::mov(text);
		step((uint16_t)(text == 0x7818 ? 0x7D9A : 0x7DA6), 30);
		step(0x7E08, 12);
	}
	push(0x00C8);
	fl::mov(0x00C8);
	step(0x7E0C, 30);
	push(0);
	fl::clr();
	step(0x7E0E, 24);
	R[0] = jsr(0x7E12, 0xA974);
	R[6] = fl::add(R[6], 6);
	step(0x7E16, 18);
	return finish();
}

// B6:7E54 (assembly)  hint_countdowns(int event): count down the hint delays (B6:7E28) while
// no message, game state or requested hint is pending.  event 0 in Skate City: hint 0 while
// in the air (0x0FB2) except in moves 0x12/0x0B, else hint 1 between 10 and 30 frames after
// 0x0FAC without equipment (0x025F); event 1: hint 3, then hint 2 (no 0x025D, Skate City);
// both then hint 4 in Skate City.  Hints already given are marked in 0x0272.
void hint_countdowns()
{
	uint16_t *R = T.r;
	auto count = [&]() {                               // L7F0E with the hint pushed
		R[0] = jsr(0x7F12, 0x7E28);
		fl::tst(pop());
		step(0x7F14, 18);
	};
	auto byte_test = [&](uint16_t addr, uint16_t at) { // TSTB @#addr; Bxx
		const uint8_t v = peekb(addr);
		fl::tstb(v);
		step((uint16_t)(at + 4), 24);
		return v;
	};
	if (byte_test(0x018E, 0x7E54))
	{
		step(0x7F14, 12);
		return;
	}
	step(0x7E5A, 12);
	if (byte_test(0x018F, 0x7E5A))
	{
		step(0x7F14, 12);
		return;
	}
	step(0x7E60, 12);
	if (byte_test(0x0273, 0x7E60))
	{
		step(0x7F14, 12);
		return;
	}
	step(0x7E66, 12);
	R[0] = lw(rw((uint16_t)(R[6] + 2)));
	step(0x7E6A, 27);
	bool zero_case = false;
	if (!R[0])
	{
		step(0x7E6C, 12);
		zero_case = !byte_test(0x0F59, 0x7E6C);
	}
	if (zero_case)
	{
		step(0x7E72, 12);
		const uint16_t air = peek(0x0FB2);
		fl::tst(air);
		step(0x7E76, 24);
		bool hint0 = (int16_t)air > 0;
		if (hint0)
		{
			step(0x7E78, 12);
			fl::cmpb(peekb(0x032A), 0x12);
			step(0x7E7E, 30);
			hint0 = peekb(0x032A) != 0x12;
		}
		if (hint0)
		{
			step(0x7E80, 12);
			fl::cmpb(peekb(0x032A), 0x0B);
			step(0x7E86, 30);
			hint0 = peekb(0x032A) != 0x0B;
		}
		if (hint0)
		{
			step(0x7E88, 12);
			const uint8_t g = lb(peekb(0x0272) & 0x01);
			step(0x7E8E, 30);
			hint0 = !g;
		}
		if (hint0)
		{
			step(0x7E90, 12);
			push(0);
			fl::clr();
			step(0x7E92, 24);
			step(0x7F0E, 12);
			count();
			return;
		}
		step(0x7E94, 12);
		const uint8_t g1 = lb(peekb(0x0272) & 0x02);
		step(0x7E9A, 30);
		if (g1)
		{
			step(0x7F14, 12);
			return;
		}
		step(0x7E9C, 12);
		if (byte_test(0x025F, 0x7E9C))
		{
			step(0x7F14, 12);
			return;
		}
		step(0x7EA2, 12);
		for (int pass = 0; pass < 2; pass++)
		{
			const uint16_t at = pass ? 0x7EB0 : 0x7EA2;
			R[0] = lw(peek(0x01C4));
			step((uint16_t)(at + 4), 24);
			R[0] = fl::sub(R[0], peek(0x0FAC));
			step((uint16_t)(at + 8), 24);
			fl::cmp(R[0], pass ? 0x000A : 0x001E);
			step((uint16_t)(at + 12), 18);
			const bool stop = pass ? (int16_t)R[0] <= 0x000A : (int16_t)R[0] >= 0x001E;
			if (stop)
			{
				step(0x7F14, 12);
				return;
			}
			step((uint16_t)(at + 14), 12);
		}
		push(1);
		fl::mov(1);
		step(0x7EC2, 30);
		step(0x7F0E, 12);
		count();
		return;
	}
	step(0x7EC4, 12);
	fl::cmp(R[0], 1);
	step(0x7EC8, 18);
	if (R[0] == 1)
	{
		step(0x7ECA, 12);
		push(3);
		fl::mov(3);
		step(0x7ECE, 30);
		R[0] = jsr(0x7ED2, 0x7E28);
		bool second = !byte_test(0x025D, 0x7ED2);
		if (second)
		{
			step(0x7ED8, 12);
			const uint8_t g = lb(peekb(0x0272) & 0x04);
			step(0x7EDE, 30);
			second = !g;
		}
		if (second)
		{
			step(0x7EE0, 12);
			second = !byte_test(0x0F59, 0x7EE0);
		}
		if (second)
		{
			step(0x7EE6, 12);
			second = !byte_test(0x0273, 0x7EE6);
		}
		if (second)
		{
			step(0x7EEC, 12);
			ww(R[6], 2);
			fl::mov(2);
			step(0x7EF0, 27);
			R[0] = jsr(0x7EF4, 0x7E28);
		}
		else
			step(0x7EF4, 12);
		fl::tst(pop());
		step(0x7EF6, 18);
	}
	else
		step(0x7EF6, 12);
	if (byte_test(0x0F59, 0x7EF6))
	{
		step(0x7F14, 12);
		return;
	}
	step(0x7EFC, 12);
	const uint8_t g4 = lb(peekb(0x0272) & 0x10);
	step(0x7F02, 30);
	if (g4)
	{
		step(0x7F14, 12);
		return;
	}
	step(0x7F04, 12);
	if (byte_test(0x0273, 0x7F04))
	{
		step(0x7F14, 12);
		return;
	}
	step(0x7F0A, 12);
	push(4);
	fl::mov(4);
	step(0x7F0E, 30);
	count();
}

// B6:7F16 (assembly): in game state 1, show the ticket hint once: 0x7822 while there are no
// tickets (0x025B), 0x7890 when there are (F:A974, 200 frames, pause 180); 0x0279 bits 0/1
// record them.  Text table 0x0184 is 0x5CD2 meanwhile and 0x5AC8 afterwards.
void show_ticket_hint()
{
	uint16_t *R = T.r;
	const uint8_t st = peekb(0x018F);
	fl::cmpb(st, 1);
	step(0x7F1C, 30);
	if (st != 1)
	{
		step(0x7F84, 12);
		return;
	}
	step(0x7F1E, 12);
	poke(0x0184, lw(0x5CD2));
	step(0x7F24, 33);
	const uint8_t g0 = lb(peekb(0x0279) & 0x01);
	step(0x7F2A, 30);
	uint16_t text = 0;
	if (!g0)
	{
		step(0x7F2C, 12);
		const uint8_t t = peekb(0x025B);
		fl::tstb(t);
		step(0x7F30, 24);
		if (!t)
		{
			step(0x7F32, 12);
			text = 0x7822;
		}
		else
			step(0x7F56, 12);
	}
	else
	{
		step(0x7F50, 12);
		const uint8_t t = peekb(0x025B);
		fl::tstb(t);
		step(0x7F54, 24);
		step(t ? 0x7F56 : 0x7F7E, 12);
		if (!t) text = 0xFFFF;                         // nothing
	}
	if (!text)
	{
		const uint8_t g1 = lb(peekb(0x0279) & 0x02);
		step(0x7F5C, 30);
		if (!g1)
		{
			step(0x7F5E, 12);
			text = 0x7890;
		}
		else
		{
			step(0x7F7E, 12);
			text = 0xFFFF;
		}
	}
	if (text != 0xFFFF)
	{
		const uint16_t at = text == 0x7822 ? 0x7F32 : 0x7F5E;
		push(text);
		fl::mov(text);
		step((uint16_t)(at + 4), 30);
		push(0x00C8);
		fl::mov(0x00C8);
		step((uint16_t)(at + 8), 30);
		push(1);
		fl::mov(1);
		step((uint16_t)(at + 12), 30);
		R[0] = jsr((uint16_t)(at + 16), 0xA974);
		poke(0x01FC, lw(0xFF4C));
		step((uint16_t)(at + 22), 33);
		pokeb(0x0279, lb((uint8_t)(peekb(0x0279) | (text == 0x7822 ? 0x01 : 0x02))));
		step((uint16_t)(at + 28), 33);
		if (text == 0x7822) step(0x7F7A, 12);
		R[6] = fl::add(R[6], 6);
		step(0x7F7E, 18);
	}
	poke(0x0184, lw(0x5AC8));
	step(0x7F84, 33);
}

// ---------------------------------------------------------------------------------------------
// B8:7DB8 (assembly): R0 shifted by R1 (-16..15): left for positive counts, right and rounded
// for negative ones (a 17-bit sum for 6-8, the low byte dropped first for 9-12); R0 = 0 for
// other counts.  Each count has its own instruction sequence in the original (jump table at
// 0x7EEC); they are reproduced with their condition codes.  R1 ends as (count + 16) * 2
// (count + 16 when out of range).

enum ShiftOp : uint8_t { END, ASL, ASR, ROL, ROR, ROLB, RORB, SWAB, CLRB, CLC, ADC, BIC, ADD };
struct ShiftStep { ShiftOp op; uint16_t imm; };

void shift_op(ShiftOp op, uint16_t imm)
{
	uint16_t &r = T.r[0];
	const int c = C();
	switch (op)
	{
	case ASL: r = asl_w(r); break;
	case ASR: r = asr_w(r); break;
	case ROL: { const uint16_t v = r; r = (uint16_t)((v << 1) | c); f_nzvc(nzw(r) | (v >> 15)); f_shift(); break; }
	case ROR: { const uint16_t v = r; r = (uint16_t)((v >> 1) | (c << 15)); f_nzvc(nzw(r) | (v & 1)); f_shift(); break; }
	case ROLB: { const uint8_t v = (uint8_t)r, b = (uint8_t)((v << 1) | c); r = (uint16_t)((r & 0xFF00) | b); f_nzvc(nzb(b) | (v >> 7)); f_shift(); break; }
	case RORB: { const uint8_t v = (uint8_t)r, b = (uint8_t)((v >> 1) | (c << 7)); r = (uint16_t)((r & 0xFF00) | b); f_nzvc(nzb(b) | (v & 1)); f_shift(); break; }
	case SWAB: r = (uint16_t)((r >> 8) | (r << 8)); f_nzvc(nzb((uint8_t)r)); break;
	case CLRB: r &= 0xFF00; fl::clr(); break;
	case CLC: T.psw &= ~CF; break;
	case ADC: r = fl::adc(r); break;
	case BIC: r = lw((uint16_t)(r & ~imm)); break;
	case ADD: r = fl::add(r, imm); break;
	case END: break;
	}
}

void shift_rounded()
{
	static const ShiftStep seq[32][8] = {
		/* -16 */ {{ASL}, {ADC}, {BIC, 0xFFFE}},
		/* -15 */ {{ASL}, {ROL}, {ADC}, {BIC, 0xFFFC}},
		/* -14 */ {{ASL}, {ROL}, {ROL}, {ADC}, {BIC, 0xFFF8}},
		/* -13 */ {{ASL}, {ROL}, {ROL}, {ROL}, {ADC}, {BIC, 0xFFF0}},
		/* -12 */ {{CLRB}, {SWAB}, {ASR}, {ASR}, {ASR}, {ASR}, {ADC}},
		/* -11 */ {{CLRB}, {SWAB}, {ASR}, {ASR}, {ASR}, {ADC}},
		/* -10 */ {{CLRB}, {SWAB}, {ASR}, {ASR}, {ADC}},
		/*  -9 */ {{CLRB}, {SWAB}, {ASR}, {ADC}},
		/*  -8 */ {{ADD, 0x0080}, {ROLB}, {SWAB}, {BIC, 0xFE00}},
		/*  -7 */ {{ADD, 0x0040}, {ROL}, {ROLB}, {SWAB}, {BIC, 0xFC00}},
		/*  -6 */ {{ADD, 0x0020}, {ROL}, {ROL}, {ROLB}, {SWAB}, {BIC, 0xF800}},
		/*  -5 */ {{CLC}, {ROR}, {ASR}, {ASR}, {ASR}, {ASR}, {ADC}},
		/*  -4 */ {{CLC}, {ROR}, {ASR}, {ASR}, {ASR}, {ADC}},
		/*  -3 */ {{CLC}, {ROR}, {ASR}, {ASR}, {ADC}},
		/*  -2 */ {{CLC}, {ROR}, {ASR}, {ADC}},
		/*  -1 */ {{CLC}, {ROR}, {ADC}},
		/*   0 */ {},
		/*   1 */ {{ASL}},
		/*   2 */ {{ASL}, {ASL}},
		/*   3 */ {{ASL}, {ASL}, {ASL}},
		/*   4 */ {{ASL}, {ASL}, {ASL}, {ASL}},
		/*   5 */ {{ASL}, {ASL}, {ASL}, {ASL}, {ASL}},
		/*   6 */ {{ASL}, {ASL}, {ASL}, {ASL}, {ASL}, {ASL}},
		/*   7 */ {{SWAB}, {RORB}, {ROR}, {BIC, 0x007F}},
		/*   8 */ {{SWAB}, {CLRB}},
		/*   9 */ {{ASL}, {SWAB}, {CLRB}},
		/*  10 */ {{ASL}, {ASL}, {SWAB}, {CLRB}},
		/*  11 */ {{ASL}, {ASL}, {ASL}, {SWAB}, {CLRB}},
		/*  12 */ {{ASL}, {ASL}, {ASL}, {ASL}, {SWAB}, {CLRB}},
		/*  13 */ {{ROR}, {ROR}, {ROR}, {ROR}, {BIC, 0x1FFF}},
		/*  14 */ {{ROR}, {ROR}, {ROR}, {BIC, 0x3FFF}},
		/*  15 */ {{ROR}, {ROR}, {BIC, 0x7FFF}},
	};
	// where each count's sequence starts (the jump table)
	static const uint16_t entry[32] = {
		0x7E18, 0x7E22, 0x7E2E, 0x7E3C, 0x7E4C, 0x7E5C, 0x7E6A, 0x7E76,
		0x7E80, 0x7E8E, 0x7E9E, 0x7EB0, 0x7EC0, 0x7ECE, 0x7EDA, 0x7EE4,
		0x7E16, 0x7E14, 0x7E12, 0x7E10, 0x7E0E, 0x7E0C, 0x7E0A, 0x7DFE,
		0x7DF8, 0x7DF6, 0x7DF4, 0x7DF2, 0x7DF0, 0x7DE2, 0x7DD6, 0x7DCC,
	};
	T.r[1] = fl::add(T.r[1], 0x0010);
	step(0x7DBC, 18);
	const uint16_t range = lw(T.r[1] & 0xFFE0);
	step(0x7DC0, 18);
	if (range)
	{
		step(0x7DC2, 12);
		T.r[0] = 0;
		fl::clr();
		step(0x7DC4, 12);
		return;
	}
	step(0x7DC6, 12);
	const uint16_t i = T.r[1];
	T.r[1] = asl_w(T.r[1]);
	step(0x7DC8, 12);
	uint16_t pc = entry[i];
	step(pc, 27);                                      // JMP @0x7EEC(R1)
	for (const ShiftStep &st : seq[i])
	{
		if (st.op == END) break;
		shift_op(st.op, st.imm);
		const bool two_words = st.op == BIC || st.op == ADD;
		pc = (uint16_t)(pc + (two_words ? 4 : 2));
		step(pc, two_words || st.op == CLC ? 18 : 12);
	}
}

// B1:7BBC (assembly): palette animation, one part per frame (frame counter 0x01C4 & 7, 6 and 7
// do nothing), the phase from the frame counter / 8:
//   0: every 16 frames, colour 0x1038 for the equipment level 0x025C (table 0x7A64)
//   1: colours 0x1016 / 0x101C for 0x025F (tables 0x7A4C / 0x7A58)
//   2: colour 0x1004 for 0x025E (table 0x7A70)
//   3: Skate City: the time warning colour, 0x109C in game state 1, else 0x1094; cleared while
//      the timer 0x0254 is at least 8
//   4: Skate City: the water colours (rows 0x1108.. and 0x110C..); screen 7: two blinking colours
//   5: the high score flash (0x0EA6 bits, 0x0EAA countdown by 8) of colours 0x10AA and the
//      player's 0x1086 entry (ROM colours at 0x5EC6, bank A 0x2000); banks restored
void animate_palette()
{
	uint16_t *R = T.r;
	R[3] = lw(peek(0x01C4));
	step(0x7BC0, 24);
	R[1] = lw(R[3]);
	step(0x7BC2, 12);
	R[1] = lw(R[1] & 0x0007);
	step(0x7BC6, 18);
	fl::cmp(R[1], 5);
	step(0x7BCA, 18);
	if ((int16_t)R[1] > 5)
	{
		step(0x7BE6, 12);
		step(0x7CCA, 12);                              // BNE at 7BE6, 7CCA
		step(0x7D74, 12);
		return;
	}
	step(0x7BCC, 12);
	R[0] = lw(R[3]);
	step(0x7BCE, 12);
	R[0] = asr_w(R[0]);
	step(0x7BD0, 12);
	R[0] = asr_w(R[0]);
	step(0x7BD2, 12);
	R[0] = asr_w(R[0]);
	step(0x7BD4, 12);
	R[2] = lw(R[1]);
	step(0x7BD6, 12);
	R[2] = asl_w(R[2]);
	step(0x7BD8, 12);
	f_nzvc(0);                                         // jump table at 0x7D76: ADD to PC
	static const uint16_t entry[6] = { 0x7BDC, 0x7C00, 0x7C20, 0x7C3C, 0x7C7C, 0x7CE4 };
	step(entry[R[1]], 27);
	// cases 0-2: the colour for a level byte, by the frame's bit 0 (bit 1 for case 0)
	auto level_colour = [&](uint16_t level_addr, uint16_t at) {
		const uint8_t level = peekb(level_addr);
		R[2] = sext8(level);
		fl::movb(level);
		step((uint16_t)(at + 4), 24);
		return level;
	};
	switch (R[1])
	{
	case 0:
	{
		const uint8_t level = level_colour(0x025C, 0x7BDC);
		if (!level)
		{
			step(0x7C24, 12);
			step(0x7C26, 12);                          // BNE at 7C24
			return;
		}
		step(0x7BE2, 12);
		const uint16_t f = lw(R[3] & 0x000F);
		step(0x7BE6, 18);
		if (f)
		{
			step(0x7CCA, 12);
			step(0x7D74, 12);                          // BNE at 7CCA
			return;
		}
		step(0x7BE8, 12);
		R[3] = lw(R[0]);
		step(0x7BEA, 12);
		R[3] = asr_w(R[3]);
		step(0x7BEC, 12);
		R[1] = lw(R[3]);
		step(0x7BEE, 12);
		R[1] = lw(R[1] & 0x0001);
		step(0x7BF2, 18);
		R[2] = asl_w(R[2]);
		step(0x7BF4, 12);
		R[2] = fl::add(R[2], R[1]);
		step(0x7BF6, 12);
		R[2] = asl_w(R[2]);
		step(0x7BF8, 12);
		poke(0x1038, lw(peek((uint16_t)(0x7A64 + R[2]))));
		step(0x7BFE, 42);
		return;
	}
	case 1:
	{
		const uint8_t level = level_colour(0x025F, 0x7C00);
		if (!level)
		{
			step(0x7C24, 12);
			step(0x7C26, 12);
			return;
		}
		step(0x7C06, 12);
		R[1] = lw(R[0]);
		step(0x7C08, 12);
		R[1] = lw(R[1] & 0x0001);
		step(0x7C0C, 18);
		R[2] = asl_w(R[2]);
		step(0x7C0E, 12);
		R[2] = fl::add(R[2], R[1]);
		step(0x7C10, 12);
		R[2] = asl_w(R[2]);
		step(0x7C12, 12);
		poke(0x1016, lw(peek((uint16_t)(0x7A4C + R[2]))));
		step(0x7C18, 42);
		poke(0x101C, lw(peek((uint16_t)(0x7A58 + R[2]))));
		step(0x7C1E, 42);
		return;
	}
	case 2:
	{
		const uint8_t level = level_colour(0x025E, 0x7C20);
		if (!level)
		{
			step(0x7C26, 12);
			return;
		}
		step(0x7C28, 12);
		R[1] = lw(R[0]);
		step(0x7C2A, 12);
		R[1] = lw(R[1] & 0x0001);
		step(0x7C2E, 18);
		R[2] = asl_w(R[2]);
		step(0x7C30, 12);
		R[2] = fl::add(R[2], R[1]);
		step(0x7C32, 12);
		R[2] = asl_w(R[2]);
		step(0x7C34, 12);
		poke(0x1004, lw(peek((uint16_t)(0x7A70 + R[2]))));
		step(0x7C3A, 42);
		return;
	}
	case 3:
	{
		const uint8_t level = peekb(0x0F59);
		fl::tstb(level);
		step(0x7C40, 24);
		if (level)
		{
			step(0x7C26, 12);
			return;
		}
		step(0x7C42, 12);
		const uint16_t t = peek(0x0254);
		fl::cmp(t, 0x0008);
		step(0x7C48, 30);
		if ((int16_t)t >= 8)
		{
			step(0x7C76, 12);
			poke(0x1094, 0);
			fl::clr();
			step(0x7C7A, 27);
			return;
		}
		step(0x7C4A, 12);
		const uint8_t st = peekb(0x018F);
		fl::cmpb(st, 1);
		step(0x7C50, 30);
		const bool one = st == 1;
		const uint16_t at = one ? 0x7C52 : 0x7C64;
		step(at, 12);
		R[1] = lw(R[0]);
		step((uint16_t)(at + 2), 12);
		R[1] = lw(R[1] & (one ? 0x0003 : 0x0001));
		step((uint16_t)(at + 6), 18);
		R[2] = lw(R[1]);
		step((uint16_t)(at + 8), 12);
		R[2] = asl_w(R[2]);
		step((uint16_t)(at + 10), 12);
		poke(one ? 0x109C : 0x1094, lw(peek((uint16_t)((one ? 0x7A84 : 0x7A80) + R[2]))));
		step((uint16_t)(at + 16), 42);
		return;
	}
	case 4:
	{
		R[2] = 0;
		fl::clr();
		step(0x7C7E, 12);
		R[2] = lb(peekb(0x0F59));
		step(0x7C82, 24);
		fl::tst(R[2]);
		step(0x7C84, 12);
		if (R[2])
		{
			step(0x7CC6, 12);
			fl::cmp(R[2], 7);
			step(0x7CCA, 18);
			if (R[2] != 7)
			{
				step(0x7D74, 12);
				return;
			}
			step(0x7CCC, 12);
			R[1] = lw(R[0]);
			step(0x7CCE, 12);
			R[1] = lw(R[1] & 0x0001);
			step(0x7CD2, 18);
			R[2] = lw(R[1]);
			step(0x7CD4, 12);
			R[2] = asl_w(R[2]);
			step(0x7CD6, 12);
			poke(0x1148, lw(peek((uint16_t)(0x7A96 + R[2]))));
			step(0x7CDC, 42);
			poke(0x114C, lw(peek((uint16_t)(0x7A9A + R[2]))));
			step(0x7CE2, 42);
			return;
		}
		step(0x7C86, 12);
		R[1] = lw(R[0]);
		step(0x7C88, 12);
		R[1] = lw(R[1] & 0x0003);
		step(0x7C8C, 18);
		R[2] = lw(R[1]);
		step(0x7C8E, 12);
		R[2] = asl_w(R[2]);
		step(0x7C90, 12);
		R[2] = lw(peek((uint16_t)(0x7A8C + R[2])));
		step(0x7C94, 27);
		for (int i = 0; i < 5; i++)
		{
			poke((uint16_t)(0x1108 + 0x20 * i), lw(R[2]));
			step((uint16_t)(0x7C98 + 4 * i), 27);
		}
		R[2] = lw(R[1]);
		step(0x7CAA, 12);
		R[2] = asl_w(R[2]);
		step(0x7CAC, 12);
		R[0] = lw(peek((uint16_t)(0x7A8E + R[2])));
		step(0x7CB0, 27);
		for (int i = 0; i < 5; i++)
		{
			poke((uint16_t)(0x110C + 0x20 * i), lw(R[0]));
			step((uint16_t)(0x7CB4 + 4 * i), 27);
		}
		return;
	}
	default:
	{
		R[4] = lw(peek(0x01D2));
		step(0x7CE8, 24);
		R[5] = lw(peek(0x01D4));
		step(0x7CEC, 24);
		R[2] = lw(0x2000);
		step(0x7CF0, 18);
		poke(0x01D2, lw(R[2]));
		step(0x7CF4, 27);
		poke(0x1400, lw(R[2]));
		step(0x7CF8, 27);
		R[3] = 0;
		fl::clr();
		step(0x7CFA, 12);
		R[3] = lb(peekb(0x0EA6));
		step(0x7CFE, 24);
		fl::tst(R[3]);
		step(0x7D00, 12);
		if (R[3])
		{
			step(0x7D02, 12);
			const uint16_t v7D08 = fl::sub(peek(0x0EAA), 0x0008);
			poke(0x0EAA, v7D08);
			step(0x7D08, 33);
			T.psw &= ~(CF | VF);
			step(0x7D0A, 18);
			if ((int16_t)v7D08 <= 0)
			{
				step(0x7D0C, 12);
				pokeb(0x0EA6, 0);
				fl::clr();
				step(0x7D10, 27);
				const uint8_t me = peekb(0x01F5);
				R[0] = sext8(me);
				fl::movb(me);
				step(0x7D14, 24);
				R[0] = asl_w(R[0]);
				step(0x7D16, 12);
				R[0] = asl_w(R[0]);
				step(0x7D18, 12);
				R[0] = asl_w(R[0]);
				step(0x7D1A, 12);
				poke((uint16_t)(0x1086 + R[0]), lw(peek((uint16_t)(0x5EC6 + R[0]))));
				step(0x7D20, 45);
				poke(0x10AA, lw(peek(0x5EEA)));
				step(0x7D26, 39);
				step(0x7D60, 12);
			}
			else
			{
				step(0x7D28, 12);
				R[1] = lw(R[0]);
				step(0x7D2A, 12);
				R[1] = lw(R[1] & 0x0001);
				step(0x7D2E, 18);
				const uint16_t b0 = lw(R[3] & 0x0001);
				step(0x7D32, 18);
				if (b0)
				{
					step(0x7D34, 12);
					R[2] = lw(R[1]);
					step(0x7D36, 12);
					R[2] = asl_w(R[2]);
					step(0x7D38, 12);
					poke(0x10AA, lw(peek((uint16_t)(0x7A9A + R[2]))));
					step(0x7D3E, 42);
				}
				else
					step(0x7D3E, 12);
				const uint16_t b1 = lw(R[3] & 0x0002);
				step(0x7D42, 18);
				if (b1)
				{
					step(0x7D44, 12);
					const uint8_t me = peekb(0x01F5);
					R[0] = sext8(me);
					fl::movb(me);
					step(0x7D48, 24);
					R[0] = asl_w(R[0]);
					step(0x7D4A, 12);
					R[0] = asl_w(R[0]);
					step(0x7D4C, 12);
					R[0] = asl_w(R[0]);
					step(0x7D4E, 12);
					fl::tst(R[1]);
					step(0x7D50, 12);
					if (!R[1])
					{
						step(0x7D52, 12);
						R[1] = lw(peek((uint16_t)(0x5EC6 + R[0])));
						step(0x7D56, 27);
						step(0x7D5C, 12);
					}
					else
					{
						step(0x7D58, 12);
						R[1] = lw(0xFF0F);
						step(0x7D5C, 18);
					}
					poke((uint16_t)(0x1086 + R[0]), lw(R[1]));
					step(0x7D60, 30);
				}
				else
					step(0x7D60, 12);
			}
		}
		else
			step(0x7D60, 12);
		poke(0x01D2, lw(R[4]));
		step(0x7D64, 27);
		poke(0x01D4, lw(R[5]));
		step(0x7D68, 27);
		poke(0x1400, lw(peek(0x01D2)));
		step(0x7D6E, 39);
		poke(0x1402, lw(peek(0x01D4)));
		step(0x7D74, 39);
		return;
	}
	}
}

// ---------------------------------------------------------------------------------------------
// Landing score and the status line (B3)

// B3:7ADA: score a landing (on screen 4 only the first one, 0x01F2).  Bank A is 0x2400 for the
// tables at 0x451C-0x45D1 and restored afterwards.  The jump's air time 0x01C4 - 0x0FAC (in
// steps of 8), the jump height from 0x0FA8, the turn 0x02F4 (F:90A6) and the level give points
// (0x45A1, 0x4599, 0x45C1; screen 4 has its own rules), accumulated in 0x0FE2 (at most 0x80).
// The turn class indexes the per-trick counters 0x0FB4 (they wear out: 3, 2, 1, 0 rate the
// trick lower; class 3 also counts 0x0FE1 down, refilling them via B3:7DD6), and the final
// value (table 0x45D1) is awarded by F:F064 and F:EF24 or, from screen 3 on, as time 0x0F34.
// Crashes and the moves 0x12, 0x0B, 0x13 score nothing (-1).  Locals: (SP) and 2(SP) the
// saved banks B and A, 4(SP) the screen, 6(SP) half the turn class.
uint16_t score_landing(const Args &)
{
	uint16_t *R = T.r;
	SteppedFrame frame(5, 0x7ADE);
	auto local = [&](int i) { return (uint16_t)(R[6] + i); };
	auto finish = [&]() {                              // 7DD0
		R[6] = fl::add(R[6], 8);
		step(0x7DD4, 18);
		const uint16_t r0 = R[0];
		frame.leave();
		return r0;
	};
	R[6] = fl::sub(R[6], 8);
	step(0x7AE2, 18);
	ww(local(4), 0);
	fl::clr();
	step(0x7AE6, 30);
	wb(local(4), lb(peekb(0x0F59)));
	step(0x7AEC, 42);
	fl::cmp(rw(local(4)), 4);
	step(0x7AF2, 33);
	if (rw(local(4)) == 4)
	{
		step(0x7AF4, 12);
		const uint8_t v7AF8 = peekb(0x01F2);
		fl::tstb(v7AF8);
		step(0x7AF8, 24);
		if (v7AF8)
		{
			step(0x7AFA, 12);
			step(0x7DD0, 18);                          // JMP 7DD0
			return finish();
		}
		step(0x7AFE, 12);
	}
	else
		step(0x7AFE, 12);
	ww(local(2), lw(peek(0x01D2)));
	step(0x7B04, 42);
	ww(local(0), lw(peek(0x01D4)));
	step(0x7B08, 33);
	pokeb(0x01F2, lb(1));
	step(0x7B0E, 33);
	pokeb(0x0FE0, fl::incb(peekb(0x0FE0)));
	step(0x7B12, 27);
	R[0] = lw(0x2400);
	step(0x7B16, 18);
	poke(0x01D2, lw(R[0]));
	step(0x7B1A, 27);
	poke(0x1400, lw(R[0]));
	step(0x7B1E, 27);
	R[0] = lw(peek(0x02F4));
	step(0x7B22, 24);
	if ((int16_t)R[0] < 0)
	{
		step(0x7B24, 12);
		R[0] = neg_w(R[0]);
		step(0x7B26, 12);
	}
	else
		step(0x7B26, 12);
	push(R[0]);
	fl::mov(R[0]);
	step(0x7B28, 24);
	ww(R[6], fl::add(rw(R[6]), 0x0024));
	step(0x7B2C, 27);
	push(0x0048);
	fl::mov(0x0048);
	step(0x7B30, 30);
	R[0] = jsr(0x7B34, 0x90A6);
	// the two argument words stay on the stack: the locals are 4 bytes further now
	fl::cmp(R[0], 0x000F);
	step(0x7B38, 18);
	if ((int16_t)R[0] > 15)
	{
		step(0x7B3A, 12);
		R[0] = lw(15);
		step(0x7B3E, 18);
	}
	else
		step(0x7B3E, 12);
	R[3] = lw(R[0]);
	step(0x7B40, 12);
	R[5] = lw(peek(0x01C4));
	step(0x7B44, 24);
	R[5] = fl::sub(R[5], peek(0x0FAC));
	step(0x7B48, 24);
	fl::cmp(R[5], 0x0038);
	step(0x7B4C, 18);
	if ((int16_t)R[5] > 0x38)
	{
		step(0x7B4E, 12);
		R[3] = lw(0xFFFF);
		step(0x7B52, 18);
		step(0x7B5E, 12);
	}
	else
	{
		step(0x7B54, 12);
		fl::cmp(R[5], 0x0028);
		step(0x7B58, 18);
		if ((int16_t)R[5] > 0x28)
		{
			step(0x7B5A, 12);
			R[3] = fl::add(R[3], 0x0010);
			step(0x7B5E, 18);
		}
		else
			step(0x7B5E, 12);
	}
	R[5] = fl::add(R[5], 4);
	step(0x7B62, 18);
	R[5] = asr_w(R[5]);
	step(0x7B64, 12);
	R[5] = asr_w(R[5]);
	step(0x7B66, 12);
	R[5] = asr_w(R[5]);
	step(0x7B68, 12);
	fl::cmp(R[5], 0x000F);
	step(0x7B6C, 18);
	if ((int16_t)R[5] > 15)
	{
		step(0x7B6E, 12);
		R[5] = lw(15);
		step(0x7B72, 18);
	}
	else
		step(0x7B72, 12);
	R[1] = lw(peek(0x0FA8));
	step(0x7B76, 24);
	R[1] = fl::sub(R[1], peek(0x06B0));
	step(0x7B7A, 24);
	R[1] &= 0xFF00;
	fl::clr();
	step(0x7B7C, 12);
	R[1] = (uint16_t)((R[1] >> 8) | (R[1] << 8));
	f_nzvc(nzb((uint8_t)R[1]));
	step(0x7B7E, 12);
	R[1] = sext8((uint8_t)R[1]);
	fl::movb((uint8_t)R[1]);
	step(0x7B80, 12);
	R[2] = lw(R[1]);
	step(0x7B82, 12);
	R[2] = asr_w(R[2]);
	step(0x7B84, 12);
	R[1] = 0;
	fl::clr();
	step(0x7B86, 12);
	const uint16_t level = rw(local(8));
	const uint16_t v7B8A = peek(0x01B4);
	fl::tst(v7B8A);
	step(0x7B8A, 24);
	if ((int16_t)v7B8A < 0)
	{
		step(0x7B8C, 12);
		fl::cmp(level, 2);
		step(0x7B92, 33);
		if (level == 2)
		{
			step(0x7B94, 12);
			R[1] = fl::inc(R[1]);
			step(0x7B96, 12);
		}
		else
			step(0x7B96, 12);
	}
	else
		step(0x7B96, 12);
	const uint16_t v7B9A = peek(0x0FAA);
	fl::tst(v7B9A);
	step(0x7B9A, 24);
	if ((int16_t)v7B9A < 0)
	{
		step(0x7B9C, 12);
		R[4] = 0;
		fl::clr();
		step(0x7B9E, 12);
		push(lw(level));
		step(0x7BA2, 39);
		ww(R[6], fl::add(rw(R[6]), 0x451C));
		step(0x7BA6, 27);
		R[4] = lb(peekb(pop()));
		step(0x7BA8, 24);
		R[1] = fl::add(R[1], R[4]);
		step(0x7BAA, 12);
	}
	else
		step(0x7BAA, 12);
	const uint8_t move = peekb(0x032A);
	bool none;
	const uint8_t v7BB0 = peekb(0x031F);
	fl::cmpb(v7BB0, 1);
	step(0x7BB0, 30);
	if (v7BB0 == 1)
	{
		step(0x7BC8, 12);
		none = true;
	}
	else
	{
		step(0x7BB2, 12);
		fl::cmpb(move, 0x12);
		step(0x7BB8, 30);
		if (move == 0x12)
		{
			step(0x7BC8, 12);
			none = true;
		}
		else
		{
			step(0x7BBA, 12);
			fl::cmpb(move, 0x0B);
			step(0x7BC0, 30);
			if (move == 0x0B)
			{
				step(0x7BC8, 12);
				none = true;
			}
			else
			{
				step(0x7BC2, 12);
				fl::cmpb(move, 0x13);
				step(0x7BC8, 30);
				none = move == 0x13;
			}
		}
	}
	bool done = false;
	if (none)
	{
		step(0x7BCA, 12);
		step(0x7D88, 18);                              // JMP 7D88
	}
	else
	{
		step(0x7BCE, 12);
		ww(local(0x0A), lw(R[0]));
		step(0x7BD2, 30);
		ww(local(0x0A), asr_w(rw(local(0x0A))));
		step(0x7BD6, 30);
		fl::cmp(level, 4);
		step(0x7BDC, 33);
		if (level == 4)
		{
			step(0x7BDE, 12);
			fl::cmp(R[2], 0x000C);
			step(0x7BE2, 18);
			if ((int16_t)R[2] <= 12)
			{
				step(0x7BE4, 12);
				R[2] = 0;
				fl::clr();
				step(0x7BE6, 12);
				step(0x7BF8, 12);
			}
			else
			{
				step(0x7BE8, 12);
				fl::cmp(R[2], 0x0014);
				step(0x7BEC, 18);
				if ((int16_t)R[2] <= 20)
				{
					step(0x7BEE, 12);
					R[2] = lw(8);
					step(0x7BF2, 18);
					step(0x7BF8, 12);
				}
				else
				{
					step(0x7BF4, 12);
					R[2] = lw(16);
					step(0x7BF8, 18);
				}
			}
			R[0] = lw(R[2]);
			step(0x7BFA, 12);
			R[0] = fl::add(R[0], rw(local(0x0A)));
			step(0x7BFE, 27);
			R[4] = 0;
			fl::clr();
			step(0x7C00, 12);
			R[4] = lb(peekb((uint16_t)(0x4521 + R[0])));
			step(0x7C04, 27);
			R[1] = fl::add(R[1], R[4]);
			step(0x7C06, 12);
			R[2] = asl_w(R[2]);
			step(0x7C08, 12);
			R[0] = lw(R[5]);
			step(0x7C0A, 12);
			R[0] = fl::add(R[0], R[2]);
			step(0x7C0C, 12);
			R[4] = 0;
			fl::clr();
			step(0x7C0E, 12);
			R[4] = lb(peekb((uint16_t)(0x4539 + R[0])));
			step(0x7C12, 27);
			R[0] = jsr(0x7C16, 0x90BA);
			const uint8_t v7C1C = peekb(0x0FE0);
			const uint8_t v7C1C_2 = peekb((uint16_t)(0x4570 + R[0]));
			fl::cmpb(v7C1C, v7C1C_2);
			step(0x7C1C, 39);
			if (v7C1C == v7C1C_2)
			{
				step(0x7C1E, 12);
				R[4] = asl_w(R[4]);
				step(0x7C20, 12);
			}
			else
				step(0x7C20, 12);
			R[1] = fl::add(R[1], R[4]);
			step(0x7C22, 12);
			const uint16_t v7C26 = peek(0x0FB2);
			fl::tst(v7C26);
			step(0x7C26, 24);
			if (v7C26)
			{
				step(0x7C28, 12);
				R[1] = fl::add(R[1], 2);
				step(0x7C2C, 18);
			}
			else
				step(0x7C2C, 12);
			fl::cmp(R[1], 0x000E);
			step(0x7C30, 18);
			if ((int16_t)R[1] > 14)
			{
				step(0x7C32, 12);
				R[0] = jsr(0x7C36, 0x90BA);
				const uint8_t v7C3C = peekb(0x0FE0);
				const uint8_t v7C3C_2 = peekb((uint16_t)(0x4570 + R[0]));
				fl::cmpb(v7C3C, v7C3C_2);
				step(0x7C3C, 39);
				if (v7C3C != v7C3C_2)
				{
					step(0x7C3E, 12);
					R[1] = lw(14);
					step(0x7C42, 18);
				}
				else
					step(0x7C42, 12);
			}
			else
				step(0x7C42, 12);
			R[2] = lw(peek(0x0FAE));
			step(0x7C46, 24);
			const uint8_t s8 = peekb(0x01F8);
			R[0] = sext8(s8);
			fl::movb(s8);
			step(0x7C4A, 24);
			R[2] = lw(R[2] ^ R[0]);
			step(0x7C4C, 12);
			const uint16_t same = lw(R[2] & 0x0007);
			step(0x7C50, 18);
			if (!same)
			{
				step(0x7C52, 12);
				R[1] = fl::add(R[1], 2);
				step(0x7C56, 18);
			}
			else
				step(0x7C56, 12);
			const uint8_t v7C5C = peekb(0x032A);
			fl::cmpb(v7C5C, 0x0F);
			step(0x7C5C, 30);
			if (v7C5C == 0x0F)
			{
				step(0x7C5E, 12);
				R[1] = fl::add(R[1], 4);
				step(0x7C62, 18);
				pokeb(0x0EA9, lb(0x3C));
				step(0x7C68, 33);
			}
			else
				step(0x7C68, 12);
			const uint8_t v = peekb((uint16_t)(0x4584 + R[1]));
			R[1] = sext8(v);
			fl::movb(v);
			step(0x7C6C, 27);
			step(0x7D26, 12);
			step(0x7D8C, 12);
			done = true;
		}
		else
		{
			step(0x7C6E, 12);
			R[4] = 0;
			fl::clr();
			step(0x7C70, 12);
			R[4] = lb(peekb((uint16_t)(0x45A1 + R[5])));
			step(0x7C74, 27);
			fl::cmp(level, 2);
			step(0x7C7A, 33);
			if (level == 2)
			{
				step(0x7C7C, 12);
				R[4] = asr_w(R[4]);
				step(0x7C7E, 12);
			}
			else
				step(0x7C7E, 12);
			R[1] = fl::add(R[1], R[4]);
			step(0x7C80, 12);
			R[4] = 0;
			fl::clr();
			step(0x7C82, 12);
			R[0] = lw(rw(local(0x0A)));
			step(0x7C86, 27);
			R[4] = lb(peekb((uint16_t)(0x4599 + R[0])));
			step(0x7C8A, 27);
			R[1] = fl::add(R[1], R[4]);
			step(0x7C8C, 12);
			const uint8_t v7C92 = peekb(0x01EA);
			fl::cmpb(v7C92, 2);
			step(0x7C92, 30);
			if (v7C92 == 2)
			{
				step(0x7C94, 12);
				R[1] = fl::add(R[1], 2);
				step(0x7C98, 18);
			}
			else
				step(0x7C98, 12);
			R[4] = lw(peek(0x0FB2));
			step(0x7C9C, 24);
			R[4] = fl::add(R[4], 4);
			step(0x7CA0, 18);
			R[4] = asr_w(R[4]);
			step(0x7CA2, 12);
			R[4] = asr_w(R[4]);
			step(0x7CA4, 12);
			R[4] = asr_w(R[4]);
			step(0x7CA6, 12);
			fl::cmp(R[4], 0x000F);
			step(0x7CAA, 18);
			if ((int16_t)R[4] > 15)
			{
				step(0x7CAC, 12);
				R[4] = lw(15);
				step(0x7CB0, 18);
			}
			else
				step(0x7CB0, 12);
			// the air time's value from 0x45B1 is looked up but not used
			const uint8_t unused = peekb((uint16_t)(0x45B1 + R[4]));
			R[4] = sext8(unused);
			fl::movb(unused);
			step(0x7CB4, 27);
			R[4] = lw(R[4] & 0x00FF);
			step(0x7CB8, 18);
			fl::cmp(level, 2);
			step(0x7CBE, 33);
			if (level != 2)
			{
				step(0x7CC0, 12);
				fl::tst(R[2]);
				step(0x7CC2, 12);
				if ((int16_t)R[2] < 0)
				{
					step(0x7CC4, 12);
					R[2] = neg_w(R[2]);
					step(0x7CC6, 12);
				}
				else
					step(0x7CC6, 12);
				R[4] = lw(R[2]);
				step(0x7CC8, 12);
				fl::cmp(R[4], 0x000F);
				step(0x7CCC, 18);
				if ((int16_t)R[4] > 15)
				{
					step(0x7CCE, 12);
					R[4] = lw(15);
					step(0x7CD2, 18);
				}
				else
					step(0x7CD2, 12);
				const uint8_t h = peekb((uint16_t)(0x45C1 + R[4]));
				R[4] = sext8(h);
				fl::movb(h);
				step(0x7CD6, 27);
				R[4] = lw(R[4] & 0x00FF);
				step(0x7CDA, 18);
				R[1] = fl::add(R[1], R[4]);
				step(0x7CDC, 12);
			}
			else
				step(0x7CDC, 12);
			R[0] = 0;
			fl::clr();
			step(0x7CDE, 12);
			R[0] = lb(peekb(0x0FE2));
			step(0x7CE2, 24);
			R[0] = asr_w(R[0]);
			step(0x7CE4, 12);
			R[0] = asr_w(R[0]);
			step(0x7CE6, 12);
			R[0] = asr_w(R[0]);
			step(0x7CE8, 12);
			R[0] = fl::add(R[0], R[1]);
			step(0x7CEA, 12);
			R[1] = lw(R[0]);
			step(0x7CEC, 12);
			R[0] = 0;
			fl::clr();
			step(0x7CEE, 12);
			R[0] = lb(peekb(0x0FE2));
			step(0x7CF2, 24);
			R[0] = fl::add(R[0], R[1]);
			step(0x7CF4, 12);
			pokeb(0x0FE2, lb((uint8_t)R[0]));
			step(0x7CF8, 27);
			fl::cmpb((uint8_t)R[0], 0x80);
			step(0x7CFC, 18);
			if ((uint8_t)R[0] > 0x80)
			{
				step(0x7CFE, 12);
				pokeb(0x0FE2, lb(0x80));
				step(0x7D04, 33);
			}
			else
				step(0x7D04, 12);
			R[4] = 0;
			fl::clr();
			step(0x7D06, 12);
			fl::tst(R[3]);
			step(0x7D08, 12);
			if ((int16_t)R[3] < 0)
			{
				step(0x7D0A, 12);
				ww(R[6], 2);
				fl::mov(2);
				step(0x7D0E, 27);
				jsr(0x7D12, 0x7DD6);                   // update_table_0fb4
				const uint8_t v7D18 = peekb(0x0327);
				fl::cmpb(v7D18, 0x18);
				step(0x7D18, 30);
				if (v7D18 == 0x18)
				{
					step(0x7D1A, 12);
					R[4] = lw(0x0028);
					step(0x7D1E, 18);
				}
				else
					step(0x7D1E, 12);
				R[0] = lw(R[4]);
				step(0x7D20, 12);
				R[0] = fl::add(R[0], R[1]);
				step(0x7D22, 12);
				const uint8_t v = peekb((uint16_t)(0x45D1 + R[0]));
				R[1] = sext8(v);
				fl::movb(v);
				step(0x7D26, 27);
				step(0x7D8C, 12);
				done = true;
			}
			else
			{
				step(0x7D28, 12);
				R[3] = fl::add(R[3], 0x0FB4);
				step(0x7D2C, 18);
				const uint8_t v7D2E = peekb(R[3]);
				fl::tstb(v7D2E);
				step(0x7D2E, 18);
				if (v7D2E)
				{
					step(0x7D30, 12);
					const uint16_t v7D36 = peek(0x0FB2);
					fl::cmp(v7D36, 5);
					step(0x7D36, 30);
					if ((int16_t)v7D36 >= 5)
					{
						step(0x7D82, 12);
						const uint8_t v = peekb((uint16_t)(0x45D1 + R[1]));
						R[1] = sext8(v);
						fl::movb(v);
						step(0x7D86, 27);
						step(0x7D8C, 12);
					}
					else
					{
						step(0x7D38, 12);
						const uint8_t c = peekb(R[3]);
						fl::cmpb(c, 1);
						step(0x7D3C, 24);
						if (c <= 1)
						{
							step(0x7D3E, 12);
							R[4] = lw(0x0050);
							step(0x7D42, 18);
							step(0x7D4E, 12);
						}
						else
						{
							step(0x7D44, 12);
							fl::cmpb(c, 3);
							step(0x7D48, 24);
							if (c == 3)
								step(0x7D4E, 12);
							else
							{
								step(0x7D4A, 12);
								R[4] = lw(0x0028);
								step(0x7D4E, 18);
							}
						}
						const uint8_t v7D54 = peekb(0x0327);
						fl::cmpb(v7D54, 0x18);
						step(0x7D54, 30);
						if (v7D54 == 0x18)
						{
							step(0x7D56, 12);
							fl::cmp(R[4], 0x0050);
							step(0x7D5A, 18);
							if ((int16_t)R[4] < 0x50)
							{
								step(0x7D5C, 12);
								R[4] = fl::add(R[4], 0x0028);
								step(0x7D60, 18);
							}
							else
								step(0x7D60, 12);
						}
						else
							step(0x7D60, 12);
						R[0] = lw(R[4]);
						step(0x7D62, 12);
						R[0] = fl::add(R[0], R[1]);
						step(0x7D64, 12);
						const uint8_t v = peekb((uint16_t)(0x45D1 + R[0]));
						R[1] = sext8(v);
						fl::movb(v);
						step(0x7D68, 27);
						const uint8_t v7D6C = peekb(R[3]);
						fl::cmpb(v7D6C, 3);
						step(0x7D6C, 24);
						if (v7D6C == 3)
						{
							step(0x7D6E, 12);
							fl::tst(R[1]);
							step(0x7D70, 12);
							if ((int16_t)R[1] >= 0)
							{
								step(0x7D72, 12);
								const uint8_t n = fl::decb(peekb(0x0FE1));
								pokeb(0x0FE1, n);
								step(0x7D76, 27);
								if (!n)
								{
									step(0x7D78, 12);
									ww(R[6], 0);
									fl::clr();
									step(0x7D7A, 21);
									jsr(0x7D7E, 0x7DD6);   // update_table_0fb4
								}
								else
									step(0x7D7E, 12);
							}
							else
								step(0x7D7E, 12);
						}
						else
							step(0x7D7E, 12);
						pokeb(R[3], fl::decb(peekb(R[3])));
						step(0x7D80, 21);
						step(0x7D8C, 12);
					}
					done = true;
				}
				else
					step(0x7D88, 12);
			}
		}
	}
	if (!done)
	{
		R[1] = lw(0xFFFF);
		step(0x7D8C, 18);
	}
	ww(R[6], lw(R[1]));
	step(0x7D8E, 21);
	R[0] = jsr(0x7D92, 0xF064);
	R[1] = lw(R[0]);
	step(0x7D94, 12);
	if ((int16_t)R[1] >= 0)
	{
		step(0x7D96, 12);
		R[1] = asl_w(R[1]);
		step(0x7D98, 12);
		const uint8_t v7D9E = peekb(0x0F59);
		fl::cmpb(v7D9E, 3);
		step(0x7D9E, 30);
		if (v7D9E < 3)
		{
			step(0x7DA0, 12);
			ww(R[6], lw(peek((uint16_t)(0xFCFC + R[1]))));
			step(0x7DA4, 36);
			R[0] = jsr(0x7DA8, 0xEF24);
			step(0x7DB6, 12);
		}
		else
		{
			step(0x7DAA, 12);
			poke(0x0F34, fl::add(peek(0x0F34), peek((uint16_t)(0xFCFC + R[1]))));
			step(0x7DB0, 42);
			pokeb(0x0FA2, lb((uint8_t)(peekb(0x0FA2) | 0x04)));
			step(0x7DB6, 33);
		}
	}
	else
		step(0x7DB6, 12);
	poke(0x01D2, lw(rw(local(6))));
	step(0x7DBC, 42);
	poke(0x01D4, lw(rw(local(4))));
	step(0x7DC2, 42);
	poke(0x1400, lw(peek(0x01D2)));
	step(0x7DC8, 39);
	poke(0x1402, lw(peek(0x01D4)));
	step(0x7DCE, 39);
	fl::cmp(rw(R[6]), rw((uint16_t)(R[6] + 2)));
	R[6] = (uint16_t)(R[6] + 4);
	step(0x7DD0, 24);
	return finish();
}

// fill one row of 13 text cells (and the row below) at the cursor 0x0186 with the tile in R2
// (the loop at loop, R1 counting the offset), interrupt points after every instruction
void fill_status_row(uint16_t loop)
{
	uint16_t *R = T.r;
	R[1] = 0;                                          // CLR R1
	fl::clr();
	step(loop, 12);
	for (;;)
	{
		R[0] = lw(R[1]);
		step((uint16_t)(loop + 2), 12);
		R[0] = fl::add(R[0], peek(0x0186));
		step((uint16_t)(loop + 6), 24);
		poke((uint16_t)(R[0] + 0x80), lw(R[2]));
		step((uint16_t)(loop + 10), 30);
		poke(R[0], lw(R[2]));
		step((uint16_t)(loop + 12), 21);
		R[1] = fl::add(R[1], 2);
		step((uint16_t)(loop + 16), 18);
		fl::cmp(R[1], 0x0018);
		step((uint16_t)(loop + 20), 18);
		if ((int16_t)R[1] > 0x0018) break;
		step(loop, 12);
	}
	step((uint16_t)(loop + 22), 12);
}

// B3:7446: draw the whole status line: the player labels (texts 0x5DCE + 8 * player, with
// 0x10 in mode 1 of 0x0F26), the timer bar (B3:78BE) or the park's goal text (by screen,
// F:90BA), both players' scores, the high score, ticket marks (A5:4984), cash and meters,
// and in a two-player game the high score flash and a message (screen 0 only).  It runs in the
// main program; the frame processing of the vertical blank, which draws text with the same
// cursor 0x0186, may come in at any instruction: interrupts are let in after every
// instruction, as in the original.
uint16_t draw_status_line(const Args &)
{
	SteppedFrame frame(5, 0x744A);
	uint16_t *R = T.r;
	// player labels
	const uint8_t v7450 = peekb(0x0F26);
	fl::cmpb(v7450, 1);
	step(0x7450, 30);
	if (v7450 == 1)
	{
		step(0x7452, 12);
		R[1] = lw(0x0010);
		step(0x7456, 18);
		step(0x745A, 12);
	}
	else
	{
		step(0x7458, 12);
		R[1] = 0;
		fl::clr();
		step(0x745A, 12);
	}
	R[2] = 0;
	fl::clr();
	step(0x745C, 12);
	R[2] = lb(peekb(0x01F6));
	step(0x7460, 24);
	fl::tst(R[2]);
	step(0x7462, 12);
	step(0x7464, 12);                                  // BLT (never: a byte)
	for (;;)
	{
		R[0] = lw(R[2]);
		step(0x7466, 12);
		R[0] = asl_w(R[0]);
		step(0x7468, 12);
		poke(0x0188, lw(peek((uint16_t)(0x51DE + R[0]))));
		step(0x746E, 42);
		R[0] = lw(R[2]);
		step(0x7470, 12);
		R[0] = asl_w(R[0]);
		step(0x7472, 12);
		R[0] = asl_w(R[0]);
		step(0x7474, 12);
		R[0] = asl_w(R[0]);
		step(0x7476, 12);
		R[0] = fl::add(R[0], R[1]);
		step(0x7478, 12);
		R[0] = fl::add(R[0], 0x5DCE);
		step(0x747C, 18);
		push(R[0]);
		fl::mov(R[0]);
		step(0x747E, 24);
		push(0);
		fl::clr();
		step(0x7480, 24);
		jsr(0x7484, 0xEAD4);
		fl::cmp(rw(R[6]), rw((uint16_t)(R[6] + 2)));
		R[6] = (uint16_t)(R[6] + 4);
		step(0x7486, 24);
		R[2] = fl::dec(R[2]);
		step(0x7488, 12);
		if ((int16_t)R[2] < 0) break;
		step(0x7464, 12);
	}
	step(0x748A, 12);
	// the timer bar or the park's goal
	const uint8_t v748E = peekb(0x018F);
	fl::tstb(v748E);
	step(0x748E, 24);
	if (v748E)
		step(0x754E, 12);
	else
	{
		step(0x7490, 12);
		jsr(0x7494, 0x8068);                           // select_video_bank0
		R[0] = 0;
		fl::clr();
		step(0x7496, 12);
		R[0] = lb(peekb(0x0F26));
		step(0x749A, 24);
		fl::tst(R[0]);
		step(0x749C, 12);
		bool goal;
		if (R[0] == 0)
			step(0x74A4, 12), goal = false;
		else
		{
			step(0x749E, 12);
			fl::cmp(R[0], 0x000A);
			step(0x74A2, 18);
			goal = R[0] != 0x000A;
			step(goal ? 0x74D6 : 0x74A4, 12);
		}
		if (!goal)
		{
			fl::tstb(peekb(0x0F1E));
			step(0x74A8, 24);
			goal = peekb(0x0F1E) != 0;
			step(goal ? 0x74D6 : 0x74AA, 12);
		}
		if (!goal)
		{
			const uint16_t v74AE = peek(0x0254);
			fl::tst(v74AE);
			step(0x74AE, 24);
			if ((int16_t)v74AE > 0)
			{
				step(0x74B0, 12);
				poke(0x0188, lw(0x4000));
				step(0x74B6, 33);
				push(0x5DEE);
				fl::mov(0x5DEE);
				step(0x74BA, 30);
				push(0);
				fl::clr();
				step(0x74BC, 24);
				jsr(0x74C0, 0xEAD4);
				pokeb(0x0FA2, lb((uint8_t)(peekb(0x0FA2) | 0x04)));
				step(0x74C6, 33);
				jsr(0x74CA, 0x78BE);                   // update_time_bar
				step(0x754C, 12);
				fl::cmp(rw(R[6]), rw((uint16_t)(R[6] + 2)));
				R[6] = (uint16_t)(R[6] + 4);
				step(0x754E, 24);
			}
			else
			{
				step(0x74CC, 12);
				R[0] = lw(0x0064);
				step(0x74D0, 18);
				jsr(0x74D4, 0x8AB6);
				step(0x754E, 12);
			}
		}
		else
		{
			poke(0x0188, lw(0x4000));
			step(0x74DC, 33);
			R[0] = lw(0x2000);
			step(0x74E0, 18);
			poke(0x01D2, lw(R[0]));
			step(0x74E4, 27);
			poke(0x1400, lw(R[0]));
			step(0x74E8, 27);
			R[0] = 0;
			fl::clr();
			step(0x74EA, 12);
			R[0] = lb(peekb(0x0F59));
			step(0x74EE, 24);
			R[0] = fl::dec(R[0]);
			step(0x74F0, 12);
			R[0] = asl_w(R[0]);
			step(0x74F2, 12);
			// jump table at 0x77AC for screens 1-4 (ADD to PC: flags of a positive address);
			// the original jumps outside it for other screens, here the text is left out
			static const uint16_t entry[4] = { 0x74F6, 0x750E, 0x7518, 0x7522 };
			const uint16_t level = peekb(0x0F59);
			f_nzvc(0);
			if (level >= 1 && level <= 4)
			{
				step(entry[level - 1], 27);
				const uint16_t ret = (uint16_t)(entry[level - 1] + 4);
				R[0] = jsr(ret, 0x90BA);
				static const uint16_t index[4] = { 0x5E83, 0x5E8D, 0x5EA1, 0x5E97 };
				const uint8_t v = peekb((uint16_t)(index[level - 1] + R[0]));
				R[0] = sext8(v);
				fl::movb(v);
				step((uint16_t)(ret + 4), 27);
				if (level == 2) step(0x74FE, 12);
				if (level == 3) step(0x752A, 12);
				R[0] = lw(R[0] & 0x00FF);
				step(level <= 2 ? 0x7502 : 0x752E, 18);
				R[0] = asl_w(R[0]);
				step(level <= 2 ? 0x7504 : 0x7530, 12);
				R[0] = asl_w(R[0]);
				step(level <= 2 ? 0x7506 : 0x7532, 12);
				R[0] = asl_w(R[0]);
				step(level <= 2 ? 0x7508 : 0x7534, 12);
				R[0] = fl::add(R[0], level <= 2 ? 0x5E56 : 0x5EA6);
				step(level <= 2 ? 0x750C : 0x7538, 18);
				if (level <= 2) step(0x7538, 12);
				push(R[0]);
				fl::mov(R[0]);
				step(0x753A, 24);
				push(0);
				fl::clr();
				step(0x753C, 24);
				jsr(0x7540, 0xEAD4);
				R[0] = lw(0x3800);
				step(0x7544, 18);
				poke(0x01D2, lw(R[0]));
				step(0x7548, 27);
				poke(0x1400, lw(R[0]));
				step(0x754C, 27);
				fl::cmp(rw(R[6]), rw((uint16_t)(R[6] + 2)));
				R[6] = (uint16_t)(R[6] + 4);
				step(0x754E, 24);
			}
			else
				step(0x754E, 27);
		}
	}
	// scores
	const uint8_t v7552 = peekb(0x01F6);
	fl::tstb(v7552);
	step(0x7552, 24);
	if (v7552)
	{
		step(0x7554, 12);
		const uint8_t me = peekb(0x01F5);
		R[1] = sext8(me);
		fl::movb(me);
		step(0x7558, 24);
		if (me)
		{
			step(0x7562, 12);
			poke(0x0186, lw(0x2204));
			step(0x7568, 33);
		}
		else
		{
			step(0x755A, 12);
			poke(0x0186, lw(0x2262));
			step(0x7560, 33);
			step(0x7568, 12);
		}
		R[0] = lw(0x0001);
		step(0x756C, 18);
		R[1] = lw(R[1] ^ R[0]);
		step(0x756E, 12);
		R[1] = asl_w(R[1]);
		step(0x7570, 12);
		poke(0x0188, lw(peek((uint16_t)(0x51DE + R[1]))));
		step(0x7576, 42);
		R[2] = lw(peek((uint16_t)(0x51DE + R[1])));
		step(0x757A, 27);
		R[2] = lw(R[2] | 0x0087);
		step(0x757E, 18);
		fill_status_row(0x7580);
		push(peek(0x027C));
		fl::mov(peek(0x027C));
		step(0x759A, 36);
		push(peek(0x027E));
		fl::mov(peek(0x027E));
		step(0x759E, 36);
		push(6);
		fl::mov(6);
		step(0x75A2, 30);
		push(1);
		fl::mov(1);
		step(0x75A6, 30);
		push(rw(R[6]));
		fl::mov(1);
		step(0x75A8, 30);
		R[0] = jsr(0x75AC, 0xBA9A);
		const uint8_t v75B0 = peekb(0x01F5);
		fl::tstb(v75B0);
		step(0x75B0, 24);
		if (v75B0)
		{
			step(0x75BA, 12);
			poke(0x0186, lw(0x2262));
			step(0x75C0, 33);
		}
		else
		{
			step(0x75B2, 12);
			poke(0x0186, lw(0x2204));
			step(0x75B8, 33);
			step(0x75C0, 12);
		}
		R[6] = fl::add(R[6], 10);
		step(0x75C4, 18);
		step(0x75CC, 12);
	}
	else
	{
		step(0x75C6, 12);
		poke(0x0186, lw(0x2204));
		step(0x75CC, 33);
	}
	const uint8_t me = peekb(0x01F5);
	R[0] = sext8(me);
	fl::movb(me);
	step(0x75D0, 24);
	R[0] = asl_w(R[0]);
	step(0x75D2, 12);
	poke(0x0188, lw(peek((uint16_t)(0x51DE + R[0]))));
	step(0x75D8, 42);
	R[2] = lw(peek((uint16_t)(0x51DE + R[0])));
	step(0x75DC, 27);
	R[2] = lw(R[2] | 0x0087);
	step(0x75E0, 18);
	fill_status_row(0x75E2);
	push(peek(0x023E));
	fl::mov(peek(0x023E));
	step(0x75FC, 36);
	push(peek(0x0240));
	fl::mov(peek(0x0240));
	step(0x7600, 36);
	push(6);
	fl::mov(6);
	step(0x7604, 30);
	push(1);
	fl::mov(1);
	step(0x7608, 30);
	push(rw(R[6]));
	fl::mov(1);
	step(0x760A, 30);
	R[0] = jsr(0x760E, 0xBA9A);
	const uint8_t v7614 = peekb(0x0F26);
	fl::cmpb(v7614, 1);
	step(0x7614, 30);
	if (v7614 == 1)
	{
		step(0x7616, 12);
		step(0x7754, 18);
		pokeb(0x036C, 1);
		fl::movb(1);
		step(0x775A, 33);
	}
	else
	{
		step(0x761A, 12);
		// the high score
		const uint8_t v761E = peekb(0x018F);
		fl::tstb(v761E);
		step(0x761E, 24);
		if (v761E)
			step(0x7662, 12);
		else
		{
			step(0x7620, 12);
			pokeb(0x036C, 0);
			fl::clr();
			step(0x7624, 27);
			fl::tstb(peekb(0x01F6));
			step(0x7628, 24);
			bool high = !peekb(0x01F6);
			if (high)
				step(0x7632, 12);
			else
			{
				step(0x762A, 12);
				fl::cmpb(peekb(0x0F2B), 4);
				step(0x7630, 30);
				high = peekb(0x0F2B) == 4;
				step(high ? 0x7632 : 0x7662, 12);
			}
			if (high)
			{
				poke(0x0188, lw(0xA000));
				step(0x7638, 33);
				ww(R[6], 0x5DF6);
				fl::mov(0x5DF6);
				step(0x763C, 27);
				push(0);
				fl::clr();
				step(0x763E, 24);
				jsr(0x7642, 0xEAD4);
				poke(0x0186, lw(0x3746));
				step(0x7648, 33);
				ww(R[6], lw(peek(0x0244)));
				step(0x764C, 33);
				push(lw(peek(0x0246)));
				step(0x7650, 36);
				push(6);
				fl::mov(6);
				step(0x7654, 30);
				push(0);
				fl::clr();
				step(0x7656, 24);
				push(1);
				fl::mov(1);
				step(0x765A, 30);
				R[0] = jsr(0x765E, 0xBA96);
				R[6] = fl::add(R[6], 10);
				step(0x7662, 18);
			}
		}
		// ticket marks
		R[4] = 0;
		fl::clr();
		step(0x7664, 12);
		R[4] = lb(peekb(0x01F6));
		step(0x7668, 24);
		R[3] = 0;
		fl::clr();
		step(0x766A, 12);
		step(0x76BC, 12);
		for (;;)
		{
			fl::cmp(R[3], R[4]);
			step(0x76BE, 12);
			if ((int16_t)R[3] > (int16_t)R[4]) break;
			step(0x766C, 12);
			R[0] = lw(R[3]);
			step(0x766E, 12);
			R[0] = asl_w(R[0]);
			step(0x7670, 12);
			poke(0x0188, lw(peek((uint16_t)(0x51DE + R[0]))));
			step(0x7676, 42);
			R[0] = lw(0x1800);
			step(0x767A, 18);
			poke(0x01D2, lw(R[0]));
			step(0x767E, 27);
			poke(0x1400, lw(R[0]));
			step(0x7682, 27);
			const uint8_t cur = peekb(0x01F5);
			R[0] = sext8(cur);
			fl::movb(cur);
			step(0x7686, 24);
			fl::cmp(R[3], R[0]);
			step(0x7688, 12);
			if (R[3] == R[0])
			{
				step(0x768A, 12);
				const uint8_t t = peekb(0x025B);
				R[2] = sext8(t);
				fl::movb(t);
				step(0x768E, 24);
				step(0x7696, 12);
			}
			else
			{
				step(0x7690, 12);
				R[2] = 0;
				fl::clr();
				step(0x7692, 12);
				R[2] = lb(peekb(0x0299));
				step(0x7696, 24);
			}
			R[1] = 0;
			fl::clr();
			step(0x7698, 12);
			step(0x76A8, 12);
			for (;;)
			{
				R[1] = fl::inc(R[1]);
				step(0x76AA, 12);
				fl::cmp(R[1], R[2]);
				step(0x76AC, 12);
				if ((int16_t)R[1] > (int16_t)R[2]) break;
				step(0x769A, 12);
				ww(R[6], R[3]);
				fl::mov(R[3]);
				step(0x769C, 21);
				push(R[1]);
				fl::mov(R[1]);
				step(0x769E, 24);
				push(1);
				fl::mov(1);
				step(0x76A2, 30);
				R[0] = jsr(0x76A6, 0x4984);
				fl::cmp(rw(R[6]), rw((uint16_t)(R[6] + 2)));
				R[6] = (uint16_t)(R[6] + 4);
				step(0x76A8, 24);
			}
			step(0x76AE, 12);
			R[0] = lw(0x3800);
			step(0x76B2, 18);
			poke(0x01D2, lw(R[0]));
			step(0x76B6, 27);
			poke(0x1400, lw(R[0]));
			step(0x76BA, 27);
			R[3] = fl::inc(R[3]);
			step(0x76BC, 12);
		}
		step(0x76C0, 12);
		// cash and meters
		poke(0x018A, lw(0x00A8));
		step(0x76C6, 33);
		poke(0x0188, lw(0x8000));
		step(0x76CC, 33);
		const uint8_t v76D0 = peekb(0x01F6);
		fl::tstb(v76D0);
		step(0x76D0, 24);
		if (v76D0)
		{
			step(0x76D2, 12);
			R[1] = lw(0x0001);
			step(0x76D6, 18);
			const uint8_t cur = peekb(0x01F5);
			R[0] = sext8(cur);
			fl::movb(cur);
			step(0x76DA, 24);
			R[1] = lw(R[1] ^ R[0]);
			step(0x76DC, 12);
			R[1] = asl_w(R[1]);
			step(0x76DE, 12);
			poke(0x0186, lw(peek((uint16_t)(0x51E2 + R[1]))));
			step(0x76E4, 42);
			poke(0x0186, fl::add(peek(0x0186), 0x2000));
			step(0x76EA, 33);
			ww(R[6], lw(peek(0x0294)));
			step(0x76EE, 33);
			push(0);
			fl::clr();
			step(0x76F0, 24);
			push(6);
			fl::mov(6);
			step(0x76F4, 30);
			push(1);
			fl::mov(1);
			step(0x76F8, 30);
			push(rw(R[6]));
			fl::mov(1);
			step(0x76FA, 30);
			R[0] = jsr(0x76FE, 0xBA9A);
			ww(R[6], 1);
			fl::mov(1);
			step(0x7702, 27);
			const uint8_t c2 = peekb(0x01F5);
			R[0] = sext8(c2);
			fl::movb(c2);
			step(0x7706, 24);
			ww(R[6], lw((uint16_t)(rw(R[6]) ^ R[0])));
			step(0x7708, 21);
			R[0] = jsr(0x770C, 0x739A);                // draw_cash_meter
			const uint8_t c3 = peekb(0x01F5);
			R[0] = sext8(c3);
			fl::movb(c3);
			step(0x7710, 24);
			R[0] = asl_w(R[0]);
			step(0x7712, 12);
			poke(0x0186, lw(peek((uint16_t)(0x51E2 + R[0]))));
			step(0x7718, 42);
			poke(0x0186, fl::add(peek(0x0186), 0x2000));
			step(0x771E, 33);
			R[6] = fl::add(R[6], 8);
			step(0x7722, 18);
			step(0x7730, 12);
		}
		else
		{
			step(0x7724, 12);
			poke(0x0186, lw(peek(0x51E2)));
			step(0x772A, 39);
			poke(0x0186, fl::add(peek(0x0186), 0x2000));
			step(0x7730, 33);
		}
		ww(R[6], lw(peek(0x0256)));
		step(0x7734, 33);
		push(0);
		fl::clr();
		step(0x7736, 24);
		push(6);
		fl::mov(6);
		step(0x773A, 30);
		push(1);
		fl::mov(1);
		step(0x773E, 30);
		push(rw(R[6]));
		fl::mov(1);
		step(0x7740, 30);
		R[0] = jsr(0x7744, 0xBA9A);
		const uint8_t c4 = peekb(0x01F5);
		R[0] = sext8(c4);
		fl::movb(c4);
		step(0x7748, 24);
		ww(R[6], R[0]);
		fl::mov(R[0]);
		step(0x774A, 21);
		R[0] = jsr(0x774E, 0x739A);                    // draw_cash_meter
		R[6] = fl::add(R[6], 8);
		step(0x7752, 18);
		step(0x775A, 12);
	}
	// two players: high score flash and message
	poke(0x018A, lw(0x0001));
	step(0x7760, 33);
	fl::tstb(peekb(0x01F6));
	step(0x7764, 24);
	bool flash = peekb(0x01F6) != 0;
	if (flash)
	{
		step(0x7766, 12);
		fl::cmpb(peekb(0x0F2B), 4);
		step(0x776C, 30);
		flash = peekb(0x0F2B) != 4;
	}
	if (flash)
	{
		step(0x776E, 12);
		fl::cmpb(peekb(0x018F), 4);
		step(0x7774, 30);
		flash = peekb(0x018F) != 4;
	}
	if (flash)
	{
		step(0x7776, 12);
		pokeb(0x0EA6, lb((uint8_t)(peekb(0x0EA6) | 0x02)));
		step(0x777C, 33);
		poke(0x0EAA, lw(0x00B4));
		step(0x7782, 33);
		const uint8_t v7786 = peekb(0x0F59);
		fl::tstb(v7786);
		step(0x7786, 24);
		if (!v7786)
		{
			step(0x7788, 12);
			const uint8_t v778E = peekb(0x0F2B);
			fl::cmpb(v778E, 1);
			step(0x778E, 30);
			if (v778E == 1)
			{
				step(0x7790, 12);
				ww(R[6], 0x76F0);
				fl::mov(0x76F0);
				step(0x7794, 27);
				step(0x779A, 12);
			}
			else
			{
				step(0x7796, 12);
				ww(R[6], 0x75EA);
				fl::mov(0x75EA);
				step(0x779A, 27);
			}
			push(0x0078);
			fl::mov(0x0078);
			step(0x779E, 30);
			push(0);
			fl::clr();
			step(0x77A0, 24);
			R[0] = jsr(0x77A4, 0xA974);
			fl::cmp(rw(R[6]), rw((uint16_t)(R[6] + 2)));
			R[6] = (uint16_t)(R[6] + 4);
			step(0x77A6, 24);
		}
		else
			step(0x77A6, 12);
	}
	else
		step(0x77A6, 12);
	R[6] = fl::add(R[6], 10);
	step(0x77AA, 18);
	const uint16_t r0 = R[0];
	frame.leave();
	return r0;
}

// B6:7942 (assembly): scroll the view after the skater.  The target (0x0210, 0x0212) is his
// position relative to the scroll (0x01AC, 0x01AE) plus his velocity (on other screens than
// Skate City also the vertical velocity of object 1 0x05B2 and a height correction by screen,
// jump table 0x7F86).  Outside the dead zone the wanted scroll speed (0x0214, 0x0216) comes
// from F:90A6 (shifts of the overshoot) and is limited near the edges.  The scroll speed
// (0x01B0, 0x01B2) follows it in steps of 8 / 4 (at once when close, or on other screens) and
// is added to the scroll position rounded to 16 (unrounded on other screens with terrain
// 0x0327 & 7).  Two words of locals (screen, other-screen flag); the flags end from comparing
// them.
void follow_skater_scroll()
{
	uint16_t *R = T.r;
	poke(0x0218, lw(peek(0x0570)));
	step(0x7948, 39);
	push(0);
	fl::clr();
	step(0x794A, 24);
	wb(R[6], lb(peekb(0x0F59)));
	step(0x794E, 33);
	push(0);
	fl::clr();
	step(0x7950, 24);
	const uint16_t level = rw((uint16_t)(R[6] + 2));
	fl::tst(level);
	step(0x7954, 27);
	if (level)
	{
		step(0x7956, 12);
		ww(R[6], fl::inc(rw(R[6])));
		step(0x7958, 21);
		poke(0x0218, fl::add(peek(0x0218), peek(0x05B2)));
		step(0x795E, 39);
	}
	else
		step(0x795E, 12);
	R[0] = lw(peek(0x06F0));
	step(0x7962, 24);
	R[0] = fl::sub(R[0], peek(0x01AC));
	step(0x7966, 24);
	R[0] = fl::add(R[0], peek(0x04F0));
	step(0x796A, 24);
	poke(0x0210, lw(R[0]));
	step(0x796E, 27);
	R[0] = lw(peek(0x0730));
	step(0x7972, 24);
	R[0] = fl::add(R[0], peek(0x01AE));
	step(0x7976, 24);
	R[0] = fl::add(R[0], peek(0x0218));
	step(0x797A, 24);
	poke(0x0212, lw(R[0]));
	step(0x797E, 27);
	R[0] = lw(peek(0x06B2));
	step(0x7982, 24);
	R[1] = lw(level);
	step(0x7986, 27);
	R[1] = asl_w(R[1]);
	step(0x7988, 12);
	// jump table at 0x7F86 (screens 0-7; ADD to PC)
	f_nzvc(0);
	switch (level)
	{
	case 1:
		step(0x79A4, 27);
		poke(0x0212, fl::add(peek(0x0212), R[0]));
		step(0x79A8, 27);
		step(0x79BA, 12);
		break;
	case 2: case 4:
		step(0x79AA, 27);
		R[1] = lw(R[0]);
		step(0x79AC, 12);
		R[1] = fl::add(R[1], peek(0x0212));
		step(0x79B0, 24);
		poke(0x0212, lw(R[1]));
		step(0x79B4, 27);
		poke(0x0212, fl::sub(peek(0x0212), 0x1800));
		step(0x79BA, 33);
		break;
	default:
		step(0x798C, 27);
		fl::cmp(R[0], 0x1000);
		step(0x7990, 18);
		if ((int16_t)R[0] > 0x1000)
		{
			step(0x7992, 12);
			R[1] = lw(R[0]);
			step(0x7994, 12);
			R[1] = fl::add(R[1], peek(0x0212));
			step(0x7998, 24);
			poke(0x0212, lw(R[1]));
			step(0x799C, 27);
			poke(0x0212, fl::sub(peek(0x0212), 0x1000));
			step(0x79A2, 33);
			step(0x79BA, 12);
		}
		else
			step(0x79BA, 12);
		break;
	}
	poke(0x0214, 0);
	fl::clr();
	step(0x79BE, 27);
	poke(0x0216, 0);
	fl::clr();
	step(0x79C2, 27);
	R[5] = lw(peek(0x0210));
	step(0x79C6, 24);
	R[2] = lw(R[5]);
	step(0x79C8, 12);
	R[4] = lw(peek(0x0212));
	step(0x79CC, 24);
	R[0] = lw(R[4]);
	step(0x79CE, 12);
	R[0] = asr_w(R[0]);
	step(0x79D0, 12);
	fl::cmp(R[4], 0x4400);
	step(0x79D4, 18);
	// the wanted speed by F:90A6 (shift by), pushed as the original does
	auto speed = [&](uint16_t at, uint16_t shift) {
		push(R[1]);
		fl::mov(R[1]);
		step((uint16_t)(at + 2), 24);
		push(shift);
		fl::mov(shift);
		step((uint16_t)(at + 6), 30);
		R[0] = jsr((uint16_t)(at + 10), 0x90A6);
	};
	if ((int16_t)R[4] < 0x4400)
	{
		step(0x79D6, 12);
		R[1] = lw(R[2]);
		step(0x79D8, 12);
		R[1] = fl::sub(R[1], R[0]);
		step(0x79DA, 12);
		R[1] = fl::sub(R[1], 0x3000);
		step(0x79DE, 18);
		T.psw &= ~(CF | VF);
		step(0x79E0, 18);
		if ((int16_t)R[1] > 0)
		{
			step(0x79E2, 12);
			speed(0x79E2, 0xFFFB);
			poke(0x0216, lw(R[0]));
			step(0x79F0, 27);
			R[0] = asl_w(R[0]);
			step(0x79F2, 12);
			R[0] = neg_w(R[0]);
			step(0x79F4, 12);
			poke(0x0214, lw(R[0]));
			step(0x79F8, 27);
			step(0x7A1E, 12);
			fl::cmp(rw(R[6]), rw((uint16_t)(R[6] + 2)));
			R[6] = (uint16_t)(R[6] + 4);
			step(0x7A20, 24);
		}
		else
		{
			step(0x79FA, 12);
			R[3] = lw(R[0]);
			step(0x79FC, 12);
			R[3] = fl::add(R[3], R[2]);
			step(0x79FE, 12);
			R[1] = lw(0x4600);
			step(0x7A02, 18);
			R[1] = fl::sub(R[1], R[3]);
			step(0x7A04, 12);
			T.psw &= ~(CF | VF);
			step(0x7A06, 18);
			if ((int16_t)R[1] > 0)
			{
				step(0x7A08, 12);
				speed(0x7A08, 0xFFFB);
				poke(0x0216, lw(R[0]));
				step(0x7A16, 27);
				poke(0x0214, lw(R[0]));
				step(0x7A1A, 27);
				poke(0x0214, asl_w(peek(0x0214)));
				step(0x7A1E, 27);
				fl::cmp(rw(R[6]), rw((uint16_t)(R[6] + 2)));
				R[6] = (uint16_t)(R[6] + 4);
				step(0x7A20, 24);
			}
			else
				step(0x7A20, 12);
		}
		R[1] = lw(R[4]);
		step(0x7A22, 12);
		R[1] = fl::sub(R[1], 0x3800);
		step(0x7A26, 18);
		const uint16_t v7A2A = peek(0x0216);
		fl::cmp(R[1], v7A2A);
		step(0x7A2A, 24);
		if ((int16_t)R[1] >= (int16_t)v7A2A)
			step(0x7A88, 12);
		else
		{
			step(0x7A2C, 12);
			step(0x7A84, 12);
			poke(0x0216, lw(R[1]));
			step(0x7A88, 27);
		}
	}
	else
	{
		step(0x7A2E, 12);
		R[3] = lw(R[0]);
		step(0x7A30, 12);
		R[3] = fl::sub(R[3], R[2]);
		step(0x7A32, 12);
		R[1] = lw(R[3]);
		step(0x7A34, 12);
		R[1] = fl::add(R[1], 0x0200);
		step(0x7A38, 18);
		T.psw &= ~(CF | VF);
		step(0x7A3A, 18);
		if ((int16_t)R[1] > 0)
		{
			step(0x7A3C, 12);
			speed(0x7A3C, 0x0005);
			poke(0x0216, lw(R[0]));
			step(0x7A4A, 27);
			R[0] = asl_w(R[0]);
			step(0x7A4C, 12);
			R[0] = neg_w(R[0]);
			step(0x7A4E, 12);
			poke(0x0214, lw(R[0]));
			step(0x7A52, 27);
			step(0x7A76, 12);
			fl::cmp(rw(R[6]), rw((uint16_t)(R[6] + 2)));
			R[6] = (uint16_t)(R[6] + 4);
			step(0x7A78, 24);
		}
		else
		{
			step(0x7A54, 12);
			R[0] = fl::add(R[0], R[2]);
			step(0x7A56, 12);
			R[1] = lw(R[0]);
			step(0x7A58, 12);
			R[1] = fl::sub(R[1], 0x7400);
			step(0x7A5C, 18);
			T.psw &= ~(CF | VF);
			step(0x7A5E, 18);
			if ((int16_t)R[1] > 0)
			{
				step(0x7A60, 12);
				speed(0x7A60, 0x0005);
				poke(0x0216, lw(R[0]));
				step(0x7A6E, 27);
				poke(0x0214, lw(R[0]));
				step(0x7A72, 27);
				poke(0x0214, asl_w(peek(0x0214)));
				step(0x7A76, 27);
				fl::cmp(rw(R[6]), rw((uint16_t)(R[6] + 2)));
				R[6] = (uint16_t)(R[6] + 4);
				step(0x7A78, 24);
			}
			else
				step(0x7A78, 12);
		}
		R[1] = lw(R[4]);
		step(0x7A7A, 12);
		R[1] = fl::sub(R[1], 0x5000);
		step(0x7A7E, 18);
		const uint16_t v7A82 = peek(0x0216);
		fl::cmp(R[1], v7A82);
		step(0x7A82, 24);
		if ((int16_t)R[1] > (int16_t)v7A82)
		{
			step(0x7A84, 12);
			poke(0x0216, lw(R[1]));
			step(0x7A88, 27);
		}
		else
			step(0x7A88, 12);
	}
	R[1] = lw(R[5]);
	step(0x7A8A, 12);
	R[1] = fl::sub(R[1], 0x2600);
	step(0x7A8E, 18);
	if ((int16_t)R[1] < 0)
	{
		step(0x7A90, 12);
		const uint16_t v7A94 = peek(0x0214);
		fl::cmp(R[1], v7A94);
		step(0x7A94, 24);
		if ((int16_t)R[1] < (int16_t)v7A94)
		{
			step(0x7A96, 12);
			poke(0x0214, lw(R[1]));
			step(0x7A9A, 27);
		}
		else
			step(0x7A9A, 12);
	}
	else
		step(0x7A9A, 12);
	R[1] = lw(R[5]);
	step(0x7A9C, 12);
	R[1] = fl::sub(R[1], 0x5000);
	step(0x7AA0, 18);
	T.psw &= ~(CF | VF);
	step(0x7AA2, 18);
	if ((int16_t)R[1] > 0)
	{
		step(0x7AA4, 12);
		const uint16_t v7AA8 = peek(0x0214);
		fl::cmp(R[1], v7AA8);
		step(0x7AA8, 24);
		if ((int16_t)R[1] > (int16_t)v7AA8)
		{
			step(0x7AAA, 12);
			poke(0x0214, lw(R[1]));
			step(0x7AAE, 27);
		}
		else
			step(0x7AAE, 12);
	}
	else
		step(0x7AAE, 12);
	fl::tst(rw((uint16_t)(R[6] + 2)));
	step(0x7AB2, 27);
	// step the scroll speed towards the target: R2 = |target - speed| when the speed is
	// nonzero, does not oppose the target and the difference opposes the speed
	auto approach = [&](uint16_t speed_addr, uint16_t target, uint16_t step_size, uint16_t at) {
		R[2] = 0;
		fl::clr();
		step((uint16_t)(at + 2), 12);
		R[3] = lw(peek(speed_addr));
		step((uint16_t)(at + 6), 24);
		bool far = false;
		if (R[3])
		{
			step((uint16_t)(at + 8), 12);
			R[4] = lw(peek(target));
			step((uint16_t)(at + 12), 24);
			bool same = true;
			if (R[4])
			{
				step((uint16_t)(at + 14), 12);
				R[5] = lw(R[4]);
				step((uint16_t)(at + 16), 12);
				R[5] = lw(R[5] ^ R[3]);
				step((uint16_t)(at + 18), 12);
				same = (int16_t)R[5] >= 0;
				step(same ? (uint16_t)(at + 20) : (uint16_t)(at + 0x30), 12);
			}
			else
				step((uint16_t)(at + 20), 12);
			if (same)
			{
				R[2] = 0;
				fl::clr();
				step((uint16_t)(at + 22), 12);
				R[1] = lw(peek(target));
				step((uint16_t)(at + 26), 24);
				R[1] = fl::sub(R[1], R[3]);
				step((uint16_t)(at + 28), 12);
				R[4] = lw(R[3]);
				step((uint16_t)(at + 30), 12);
				R[4] = lw(R[4] ^ R[1]);
				step((uint16_t)(at + 32), 12);
				if ((int16_t)R[4] < 0)
				{
					step((uint16_t)(at + 34), 12);
					R[2] = lw(R[1]);
					step((uint16_t)(at + 36), 12);
					R[1] = lw(step_size);
					step((uint16_t)(at + 40), 18);
					fl::tst(R[2]);
					step((uint16_t)(at + 42), 12);
					if ((int16_t)R[2] < 0)
					{
						step((uint16_t)(at + 44), 12);
						R[2] = neg_w(R[2]);
						step((uint16_t)(at + 46), 12);
						R[1] = neg_w(R[1]);
						step((uint16_t)(at + 48), 12);
					}
					else
						step((uint16_t)(at + 48), 12);
				}
				else
					step((uint16_t)(at + 48), 12);
			}
		}
		else
			step((uint16_t)(at + 0x30), 12);
		fl::cmp(R[2], 0x0020);
		step((uint16_t)(at + 52), 18);
		far = (int16_t)R[2] >= 0x20;
		return far;
	};
	if (!rw((uint16_t)(R[6] + 2)))
	{
		step(0x7AB4, 12);
		if (approach(0x01B0, 0x0214, 0x0008, 0x7AB4))
		{
			step(0x7AEA, 12);
			poke(0x01B0, fl::add(peek(0x01B0), R[1]));
			step(0x7AEE, 27);
			step(0x7AF6, 12);
		}
		else
		{
			step(0x7AF0, 12);
			poke(0x01B0, lw(peek(0x0214)));
			step(0x7AF6, 39);
		}
		if (approach(0x01B2, 0x0216, 0x0004, 0x7AF6))
		{
			step(0x7B2C, 12);
			poke(0x01B2, fl::add(peek(0x01B2), R[1]));
			step(0x7B30, 27);
			step(0x7B3E, 12);
		}
		else
		{
			step(0x7B38, 12);
			poke(0x01B2, lw(peek(0x0216)));
			step(0x7B3E, 39);
		}
	}
	else
	{
		step(0x7B32, 12);
		poke(0x01B0, lw(peek(0x0214)));
		step(0x7B38, 39);
		poke(0x01B2, lw(peek(0x0216)));
		step(0x7B3E, 39);
	}
	const uint16_t flag = lw(rw(R[6]) & 0x0001);
	step(0x7B42, 24);
	bool raw = flag != 0;
	if (raw)
	{
		step(0x7B44, 12);
		raw = lb(peekb(0x0327) & 0x07) != 0;
		step(0x7B4A, 30);
	}
	if (raw)
	{
		step(0x7B4C, 12);
		poke(0x01AC, fl::add(peek(0x01AC), peek(0x01B0)));
		step(0x7B52, 39);
		poke(0x01AE, fl::sub(peek(0x01AE), peek(0x01B2)));
		step(0x7B58, 39);
		step(0x7B7E, 12);
	}
	else
	{
		step(0x7B5A, 12);
		R[0] = lw(peek(0x01B0));
		step(0x7B5E, 24);
		R[0] = fl::add(R[0], 8);
		step(0x7B62, 18);
		R[0] = lw(R[0] & ~0x000F);
		step(0x7B66, 18);
		R[0] = fl::add(R[0], peek(0x01AC));
		step(0x7B6A, 24);
		poke(0x01AC, lw(R[0]));
		step(0x7B6E, 27);
		R[0] = lw(peek(0x01B2));
		step(0x7B72, 24);
		R[0] = fl::add(R[0], 8);
		step(0x7B76, 18);
		R[0] = lw(R[0] & ~0x000F);
		step(0x7B7A, 18);
		poke(0x01AE, fl::sub(peek(0x01AE), R[0]));
		step(0x7B7E, 27);
	}
	fl::cmp(rw(R[6]), rw((uint16_t)(R[6] + 2)));
	R[6] = (uint16_t)(R[6] + 4);
	step(0x7B80, 24);
}

// ---------------------------------------------------------------------------------------------
// The park modes (B3)

// B3:65FE (assembly): one step of the park mode 0x0F26 (jump table at 0x6A0A; modes 0-8,
// larger ones would jump outside the table in the original):
//   1: the park is finished: by the game kind 0x0F2B the park's bit is recorded in 0x0F29
//      and the next park or screen is set up (B3:6A6E, B3:6E92, F:9182, F:E730)
//   2: the park's goal (0x0F27) is shown, or after the delay 0x0F22 the countdown restarts
//      (B3:65D6) and the time display is redrawn
//   3: waits for the goal (0x0F28 / B3:65D6), then sound 0x5D/0x5E and mode 4
//   4: after the delay, the result: 0x0258 from the time, B3:6E38, a prize for the medal
//      level of 0x0262 + park (cash, F:80D8), the medal is recorded, next park, mode 6
//   5: no medal: recorded, message, mode 6
//   6: the remaining time is counted into the score (F:EF24) with the status redrawn, then
//      mode 7; or messages at the end of the delay
//   7: after the delay: the last park ends the game (state 5), else B3:6E92 and mode 8
//   8: two-player turn change (B3:6D40)
void park_mode_step()
{
	uint16_t *R = T.r;
	R[0] = 0;
	fl::clr();
	step(0x6600, 12);
	R[0] = lb(peekb(0x0F26));
	step(0x6604, 24);
	R[0] = asl_w(R[0]);
	step(0x6606, 12);
	f_nzvc(0);                                         // ADD to PC: a positive address, no carry
	static const uint16_t entry[9] = { 0x6A08, 0x660A, 0x66BA, 0x674A, 0x6792, 0x685E, 0x68BC, 0x6976, 0x69D2 };
	const uint16_t mode = peekb(0x0F26);
	if (mode > 8) return;                              // outside the table in the original
	step(entry[mode], 27);
	// BR / JMP chains to the common exits
	auto exit_tst = [&](uint16_t from) {               // ... L685A: JMP 6964; TST (SP)+
		if (from == 0x6644) step(0x66EE, 12);
		if (from == 0x6644 || from == 0x66EE) step(0x67D6, 12);
		if (from != 0x685A) step(0x685A, 12);
		step(0x6964, 18);
		fl::tst(pop());
		step(0x6966, 18);
	};
	auto restart_park = [&]() {                        // L679A: after the delay 0x0F22
		jsr(0x679E, 0x820C);                           // install_interrupts_b
		push(0x0053);
		fl::mov(0x0053);
		step(0x67A2, 30);
		R[0] = jsr(0x67A6, 0x8E78);                    // queue_sound
	};
	auto mark_park = [&](uint16_t at) {                // CLR R0; BISB 0x027A,R0; BISB 0xFC74(R0),0x0F29
		R[0] = 0;
		fl::clr();
		step((uint16_t)(at + 2), 12);
		R[0] = lb(peekb(0x027A));
		step((uint16_t)(at + 6), 24);
		pokeb(0x0F29, lb((uint8_t)(peekb(0x0F29) | peekb((uint16_t)(0xFC74 + R[0])))));
		step((uint16_t)(at + 12), 42);
	};
	switch (mode)
	{
	case 1:
	{
		jsr(0x660E, 0xE106);
		pokeb(0x018F, 0);
		fl::clr();
		step(0x6612, 27);
		R[1] = 0;
		fl::clr();
		step(0x6614, 12);
		R[1] = lb(peekb(0x0F2B));
		step(0x6618, 24);
		fl::cmp(R[1], 4);
		step(0x661C, 18);
		bool first = R[1] == 4;
		if (first)
			step(0x6624, 12);
		else
		{
			step(0x661E, 12);
			fl::tstb(peekb(0x01F6));
			step(0x6622, 24);
			first = !peekb(0x01F6);
			step(first ? 0x6624 : 0x6646, 12);
		}
		if (first)
		{
			mark_park(0x6624);
			jsr(0x6634, 0x820C);
			push(0x0042);
			fl::mov(0x0042);
			step(0x6638, 30);
			jsr(0x663C, 0x8E78);
			jsr(0x6640, 0x8FD0);
			jsr(0x6644, 0x6A6E);
			exit_tst(0x6644);
			return;
		}
		fl::tst(R[1]);
		step(0x6648, 12);
		if (R[1] == 0)
		{
			step(0x664A, 12);
			jsr(0x664E, 0x820C);
			jsr(0x6652, 0x9182);
			jsr(0x6656, 0xE730);
			poke(0x0F22, 0);
			fl::clr();
			step(0x665A, 27);
			step(0x696E, 18);
			pokeb(0x0F26, lb(7));
			step(0x6974, 33);
			return;
		}
		step(0x665E, 12);
		fl::cmp(R[1], 1);
		step(0x6662, 18);
		if (R[1] == 1)
		{
			step(0x6664, 12);
			mark_park(0x6664);
			jsr(0x6674, 0x820C);
			jsr(0x6678, 0x9182);
			jsr(0x667C, 0xE730);
			jsr(0x6680, 0x6E92);
			pokeb(0x0F2B, lb(2));
			step(0x6686, 33);
			step(0x668E, 12);
			jsr(0x6692, 0x6A6E);
			return;
		}
		step(0x6688, 12);
		fl::cmp(R[1], 3);
		step(0x668C, 18);
		if (R[1] == 3)
		{
			step(0x668E, 12);
			jsr(0x6692, 0x6A6E);
			return;
		}
		step(0x6694, 12);
		fl::cmp(R[1], 5);
		step(0x6698, 18);
		if (R[1] != 5)
		{
			step(0x6796, 12);
			step(0x6798, 12);                          // BEQ at 6796
			return;
		}
		step(0x669A, 12);
		jsr(0x669E, 0x820C);
		jsr(0x66A2, 0x9182);
		jsr(0x66A6, 0x8FD0);
		jsr(0x66AA, 0xD0F4);
		jsr(0x66AE, 0xE730);
		pokeb(0x0F26, 0);
		fl::clr();
		step(0x66B2, 27);
		pokeb(0x018F, lb(7));
		step(0x66B8, 33);
		return;
	}
	case 2:
	{
		fl::tstb(peekb(0x0F27));
		step(0x66BE, 24);
		bool goal = peekb(0x0F27) != 0;
		if (goal)
		{
			step(0x66C0, 12);
			fl::tst(peek(0x0180));
			step(0x66C4, 24);
			goal = !peek(0x0180);
			step(goal ? 0x66C6 : 0x66F0, 12);
		}
		else
			step(0x66F0, 12);
		if (goal)
		{
			const uint8_t v66CC = peekb(0x027A);
			fl::cmpb(v66CC, 2);
			step(0x66CC, 30);
			if (v66CC >= 2)
			{
				step(0x66CE, 12);
				pokeb(0x0F1E, lb(0x3C));
				step(0x66D4, 33);
			}
			else
				step(0x66D4, 12);
			pokeb(0x0F26, lb(3));
			step(0x66DA, 33);
			jsr(0x66DE, 0xA8FE);
			R[0] = 0;
			fl::clr();
			step(0x66E0, 12);
			R[0] = lb(peekb(0x0F59));
			step(0x66E4, 24);
			push(0);
			fl::clr();
			step(0x66E6, 24);
			wb(R[6], lb(peekb((uint16_t)(0xFE6B + R[0]))));
			step(0x66EA, 36);
			R[0] = jsr(0x66EE, 0x8E78);                // queue_sound
			exit_tst(0x66EE);
			return;
		}
		const uint16_t delay = peek(0x0F22);
		fl::tst(delay);
		step(0x66F4, 24);
		if (delay)
		{
			step(0x6796, 12);
			step(0x6798, 12);                          // BEQ at 6796
			return;
		}
		step(0x66F6, 12);
		push(0x766C);
		fl::mov(0x766C);
		step(0x66FA, 30);
		push(0x00B4);
		fl::mov(0x00B4);
		step(0x66FE, 30);
		push(0x0001);
		fl::mov(0x0001);
		step(0x6702, 30);
		jsr(0x6706, 0xA974);
		ww(R[6], 0xFFFB);
		fl::mov(0xFFFB);
		step(0x670A, 27);
		R[0] = jsr(0x670E, 0x65D6);
		fl::tst(R[0]);
		step(0x6710, 12);
		if ((int16_t)R[0] <= 0)
		{
			step(0x6712, 12);
			poke(0x0F1C, 0);
			fl::clr();
			step(0x6716, 27);
			poke(0x0F20, 0);
			fl::clr();
			step(0x671A, 27);
			pokeb(0x0F26, lb(5));
			step(0x6720, 33);
			step(0x6728, 12);
		}
		else
		{
			step(0x6722, 12);
			poke(0x0F22, lw(0x012C));
			step(0x6728, 33);
		}
		const uint8_t v672E = peekb(0x027A);
		fl::cmpb(v672E, 2);
		step(0x672E, 30);
		if (v672E < 2)
		{
			step(0x6730, 12);
			pokeb(0x0FA2, lb((uint8_t)(peekb(0x0FA2) | 0x04)));
			step(0x6736, 33);
			jsr(0x673A, 0x78BE);                       // update_time_bar
			step(0x6746, 12);
		}
		else
		{
			step(0x673C, 12);
			pokeb(0x0FA2, lb((uint8_t)(peekb(0x0FA2) | 0x10)));
			step(0x6742, 33);
			jsr(0x6746, 0x79D4);                       // draw_countdown
		}
		step(0x68EE, 18);
		R[6] = fl::add(R[6], 6);
		step(0x68F2, 18);
		return;
	}
	case 3:
	{
		fl::tstb(peekb(0x0F28));
		step(0x674E, 24);
		bool reached = peekb(0x0F28) != 0;
		if (reached)
			step(0x6762, 12);
		else
		{
			step(0x6750, 12);
			push(0);
			fl::clr();
			step(0x6752, 24);
			R[0] = jsr(0x6756, 0x65D6);
			fl::tst(pop());
			step(0x6758, 18);
			fl::tst(R[0]);
			step(0x675A, 12);
			if (R[0])
			{
				step(0x6796, 12);
				step(0x6798, 12);                      // BEQ at 6796
				return;
			}
			step(0x675C, 12);
			fl::tstb(peekb(0x0F28));
			step(0x6760, 24);
			reached = peekb(0x0F28) != 0;
			step(reached ? 0x6762 : 0x6772, 12);
		}
		if (reached)
		{
			pokeb(0x0F1E, 0);
			fl::clr();
			step(0x6766, 27);
			poke(0x0F1C, 0);
			fl::clr();
			step(0x676A, 27);
			poke(0x0F22, lw(0x0014));
			step(0x6770, 33);
			step(0x678A, 12);
		}
		else
		{
			const uint8_t v6778 = peekb(0x027A);
			fl::cmpb(v6778, 2);
			step(0x6778, 30);
			if (v6778 < 2)
			{
				step(0x6780, 12);
				push(0x005D);
				fl::mov(0x005D);
				step(0x6784, 30);
			}
			else
			{
				step(0x677A, 12);
				push(0x005E);
				fl::mov(0x005E);
				step(0x677E, 30);
				step(0x6784, 12);
			}
			R[0] = jsr(0x6788, 0x8E78);                // queue_sound
			fl::tst(pop());
			step(0x678A, 18);
		}
		pokeb(0x0F26, lb(4));
		step(0x6790, 33);
		return;
	}
	case 4:
	{
		const uint16_t delay = peek(0x0F22);
		fl::tst(delay);
		step(0x6796, 24);
		if (delay)
		{
			step(0x6798, 12);
			return;
		}
		step(0x679A, 12);
		restart_park();
		const uint8_t v67AC = peekb(0x027A);
		fl::cmpb(v67AC, 2);
		step(0x67AC, 30);
		if (v67AC < 2)
		{
			step(0x67AE, 12);
			poke(0x0258, lw(peek(0x0F20)));
			step(0x67B4, 39);
			step(0x67BC, 12);
		}
		else
		{
			step(0x67B6, 12);
			poke(0x0258, lw(peek(0x0F34)));
			step(0x67BC, 39);
		}
		jsr(0x67C0, 0x6E38);
		R[0] = 0;
		fl::clr();
		step(0x67C2, 12);
		R[0] = lb(peekb(0x0F31));
		step(0x67C6, 24);
		const uint8_t medal = peekb((uint16_t)(0x0262 + R[0]));
		R[0] = sext8(medal);
		fl::movb(medal);
		step(0x67CA, 27);
		R[0] = lw(R[0] & 0x00FF);
		step(0x67CE, 18);
		if (!R[0])
		{
			step(0x67D0, 12);
			pokeb(0x0F26, lb(5));
			step(0x67D6, 33);
			exit_tst(0x67D6);
			return;
		}
		step(0x67D8, 12);
		R[1] = lw(R[0]);
		step(0x67DA, 12);
		R[1] = asr_w(R[1]);
		step(0x67DC, 12);
		R[1] = asr_w(R[1]);
		step(0x67DE, 12);
		R[1] = asr_w(R[1]);
		step(0x67E0, 12);
		R[0] = lw(R[1]);
		step(0x67E2, 12);
		R[0] = asr_w(R[0]);
		step(0x67E4, 12);
		R[0] = asr_w(R[0]);
		step(0x67E6, 12);
		R[0] = asr_w(R[0]);
		step(0x67E8, 12);
		R[0] = lw(R[0] & 0x0003);
		step(0x67EC, 18);
		ww(R[6], 0);
		fl::clr();
		step(0x67EE, 21);
		wb(R[6], lb(peekb((uint16_t)(0xFE8C + R[0]))));
		step(0x67F2, 36);
		R[0] = jsr(0x67F6, 0x90BA);
		push(R[0]);
		fl::mov(R[0]);
		step(0x67F8, 24);
		R[0] = jsr(0x67FC, 0x80D8);                    // mul16
		R[1] = lw(R[0]);
		step(0x67FE, 12);
		ww(R[6], 0x7676);
		fl::mov(0x7676);
		step(0x6802, 27);
		push(0x00C0);
		fl::mov(0x00C0);
		step(0x6806, 30);
		push(0);
		fl::clr();
		step(0x6808, 24);
		R[0] = jsr(0x680C, 0xA974);
		poke(0x0256, fl::add(peek(0x0256), R[1]));
		step(0x6810, 27);
		R[1] = 0;
		fl::clr();
		step(0x6812, 12);
		R[1] = lb(peekb(0x0F31));
		step(0x6816, 24);
		R[2] = 0;
		fl::clr();
		step(0x6818, 12);
		R[2] = lb(peekb(0x027A));
		step(0x681C, 24);
		for (int i = 0; i < 4; i++)
		{
			R[2] = asl_w(R[2]);
			step((uint16_t)(0x681E + 2 * i), 12);
		}
		R[0] = 0;
		fl::clr();
		step(0x6826, 12);
		R[0] = lb(peekb((uint16_t)(0x0262 + R[1])));
		step(0x682A, 27);
		R[2] = lw(R[2] | R[0]);
		step(0x682C, 12);
		R[0] = jsr(0x6830, 0x90BA);
		pokeb((uint16_t)(0x0262 + R[1]), lb((uint8_t)R[2]));
		step(0x6834, 30);
		pokeb((uint16_t)(0x0262 + R[1]), lb((uint8_t)(peekb((uint16_t)(0x0262 + R[1])) | (uint8_t)R[0])));
		step(0x6838, 30);
		fl::tstb(peekb(0x01F6));
		step(0x683C, 24);
		bool next = !peekb(0x01F6);
		if (next)
			step(0x6846, 12);
		else
		{
			step(0x683E, 12);
			fl::cmpb(peekb(0x0F2B), 2);
			step(0x6844, 30);
			next = peekb(0x0F2B) > 2;
			step(next ? 0x6846 : 0x684A, 12);
		}
		if (next)
		{
			pokeb(0x0F31, fl::incb(peekb(0x0F31)));
			step(0x684A, 27);
		}
		pokeb(0x0F26, lb(6));
		step(0x6850, 33);
		poke(0x0F22, lw(0x00C0));
		step(0x6856, 33);
		R[6] = fl::add(R[6], 6);
		step(0x685A, 18);
		exit_tst(0x685A);
		return;
	}
	case 5:
	{
		jsr(0x6862, 0x820C);                           // install_interrupts_b
		push(0x0053);
		fl::mov(0x0053);
		step(0x6866, 30);
		R[0] = jsr(0x686A, 0x8E78);                    // queue_sound
		R[1] = 0;
		fl::clr();
		step(0x686C, 12);
		R[1] = lb(peekb(0x0F31));
		step(0x6870, 24);
		R[2] = 0;
		fl::clr();
		step(0x6872, 12);
		R[2] = lb(peekb(0x027A));
		step(0x6876, 24);
		for (int i = 0; i < 4; i++)
		{
			R[2] = asl_w(R[2]);
			step((uint16_t)(0x6878 + 2 * i), 12);
		}
		R[0] = jsr(0x6882, 0x90BA);
		pokeb((uint16_t)(0x0262 + R[1]), lb((uint8_t)R[2]));
		step(0x6886, 30);
		pokeb((uint16_t)(0x0262 + R[1]), lb((uint8_t)(peekb((uint16_t)(0x0262 + R[1])) | (uint8_t)R[0])));
		step(0x688A, 30);
		fl::tstb(peekb(0x01F6));
		step(0x688E, 24);
		bool next = !peekb(0x01F6);
		if (next)
			step(0x6898, 12);
		else
		{
			step(0x6890, 12);
			fl::cmpb(peekb(0x0F2B), 2);
			step(0x6896, 30);
			next = peekb(0x0F2B) > 2;
			step(next ? 0x6898 : 0x689C, 12);
		}
		if (next)
		{
			pokeb(0x0F31, fl::incb(peekb(0x0F31)));
			step(0x689C, 27);
		}
		jsr(0x68A0, 0xA8FE);
		ww(R[6], 0x7696);
		fl::mov(0x7696);
		step(0x68A4, 27);
		push(0x00DC);
		fl::mov(0x00DC);
		step(0x68A8, 30);
		push(0);
		fl::clr();
		step(0x68AA, 24);
		R[0] = jsr(0x68AE, 0xA974);
		poke(0x0F22, lw(0x0068));
		step(0x68B4, 33);
		pokeb(0x0F26, lb(6));
		step(0x68BA, 33);
		step(0x68EE, 12);
		R[6] = fl::add(R[6], 6);
		step(0x68F2, 18);
		return;
	}
	case 6:
	{
		const uint16_t v68C2 = peek(0x0F22);
		fl::cmp(v68C2, 1);
		step(0x68C2, 30);
		if (v68C2 == 1)
		{
			step(0x68C4, 12);
			const uint8_t v68CA = peekb(0x0F59);
			fl::cmpb(v68CA, 3);
			step(0x68CA, 30);
			if (v68CA < 3)
			{
				step(0x68CC, 12);
				const uint16_t v68D0 = peek(0x0F20);
				fl::tst(v68D0);
				step(0x68D0, 24);
				if ((int16_t)v68D0 <= 0)
				{
					step(0x68D8, 12);
					R[0] = jsr(0x68DC, 0xA8FE);
					return;
				}
				step(0x68D2, 12);
				push(0x7682);
				fl::mov(0x7682);
				step(0x68D6, 30);
				step(0x68E2, 12);
			}
			else
			{
				step(0x68DE, 12);
				push(0x768C);
				fl::mov(0x768C);
				step(0x68E2, 30);
			}
			push(0x7FFF);
			fl::mov(0x7FFF);
			step(0x68E6, 30);
			push(0x0001);
			fl::mov(0x0001);
			step(0x68EA, 30);
			R[0] = jsr(0x68EE, 0xA974);
			R[6] = fl::add(R[6], 6);
			step(0x68F2, 18);
			return;
		}
		step(0x68F4, 12);
		const uint16_t delay = peek(0x0F22);
		fl::tst(delay);
		step(0x68F8, 24);
		if (delay)
		{
			step(0x697A, 12);
			step(0x6A08, 12);                          // BNE at 697A
			return;
		}
		step(0x68FA, 12);
		const uint8_t v6900 = peekb(0x0F59);
		fl::cmpb(v6900, 3);
		step(0x6900, 30);
		if (v6900 < 3)
		{
			step(0x6902, 12);
			const uint16_t v6906 = peek(0x0F20);
			fl::tst(v6906);
			step(0x6906, 24);
			if ((int16_t)v6906 <= 0)
			{
				step(0x6926, 12);
				poke(0x0F22, lw(0x0060));
				step(0x692C, 33);
				step(0x696E, 12);
				pokeb(0x0F26, lb(7));
				step(0x6974, 33);
				return;
			}
			step(0x6908, 12);
			const uint8_t v690C = peekb(0x0E95);
			fl::tstb(v690C);
			step(0x690C, 24);
			if (v690C)
			{
				step(0x6914, 12);
				push(0x0032);
				fl::mov(0x0032);
				step(0x6918, 30);
			}
			else
			{
				step(0x690E, 12);
				push(0x0064);
				fl::mov(0x0064);
				step(0x6912, 30);
				step(0x6918, 12);
			}
			jsr(0x691C, 0xEF24);
			jsr(0x6920, 0x77B4);                       // update_status_display
			poke(0x0F20, fl::dec(peek(0x0F20)));
			step(0x6924, 27);
			step(0x695A, 12);
		}
		else
		{
			step(0x692E, 12);
			const uint16_t v6932 = peek(0x0F34);
			fl::tst(v6932);
			step(0x6932, 24);
			if ((int16_t)v6932 <= 0)
			{
				step(0x6968, 12);
				poke(0x0F22, lw(0x0040));
				step(0x696E, 33);
				pokeb(0x0F26, lb(7));
				step(0x6974, 33);
				return;
			}
			step(0x6934, 12);
			poke(0x0F34, fl::sub(peek(0x0F34), 0x0020));
			step(0x693A, 33);
			push(0x0020);
			fl::mov(0x0020);
			step(0x693E, 30);
			jsr(0x6942, 0xEF24);
			const uint16_t v6948 = peek(0x0F34);
			fl::cmp(v6948, 0x0040);
			step(0x6948, 30);
			if ((int16_t)v6948 < 0x0040)
			{
				step(0x694A, 12);
				ww(R[6], lw(peek(0x0F34)));
				step(0x694E, 33);
				jsr(0x6952, 0xEF24);
				poke(0x0F34, 0);
				fl::clr();
				step(0x6956, 27);
			}
			else
				step(0x6956, 12);
			jsr(0x695A, 0x77B4);                       // update_status_display
		}
		pokeb(0x0FA2, lb((uint8_t)(peekb(0x0FA2) | 0x04)));
		step(0x6960, 33);
		R[0] = jsr(0x6964, 0x78BE);                    // update_time_bar
		fl::tst(pop());
		step(0x6966, 18);
		return;
	}
	case 7:
	{
		const uint16_t delay = peek(0x0F22);
		fl::tst(delay);
		step(0x697A, 24);
		if (delay)
		{
			step(0x6A08, 12);
			return;
		}
		step(0x697C, 12);
		const uint8_t v6980 = peekb(0x01F6);
		fl::tstb(v6980);
		step(0x6980, 24);
		bool kind2 = false;
		if (v6980)
		{
			step(0x6982, 12);
			fl::cmpb(peekb(0x0F2B), 2);
			step(0x6988, 30);
			kind2 = peekb(0x0F2B) == 2;
			step(kind2 ? 0x698A : 0x6990, 12);
		}
		else
			step(0x6990, 12);
		if (kind2)
		{
			R[0] = lw(0x000E);
			step(0x698E, 18);
			step(0x6994, 12);
		}
		else
		{
			R[0] = lw(0x000F);
			step(0x6994, 18);
		}
		R[1] = 0;
		fl::clr();
		step(0x6996, 12);
		R[1] = lb(peekb(0x0F31));
		step(0x699A, 24);
		fl::cmp(R[1], R[0]);
		step(0x699C, 12);
		if ((int16_t)R[1] > (int16_t)R[0])
		{
			step(0x69BA, 12);
			R[0] = 0;
			fl::clr();
			step(0x69BC, 12);
			R[0] = lb(peekb(0x01F5));
			step(0x69C0, 24);
			pokeb((uint16_t)(0x0F3A + R[0]), lb(1));
			step(0x69C6, 36);
			pokeb(0x018F, lb(5));
			step(0x69CC, 33);
			pokeb(0x0F26, 0);
			fl::clr();
			step(0x69D0, 27);
			return;
		}
		step(0x699E, 12);
		jsr(0x69A2, 0x820C);
		const uint8_t v69A8 = peekb(0x0F2B);
		fl::cmpb(v69A8, 2);
		step(0x69A8, 30);
		if (v69A8 == 2)
		{
			step(0x69AA, 12);
			jsr(0x69AE, 0xE730);
		}
		else
			step(0x69AE, 12);
		jsr(0x69B2, 0x6E92);
		pokeb(0x0F26, lb(8));
		step(0x69B8, 33);
		return;
	}
	case 8:
	{
		// one player, game kind 4 or 0, or kind 3 (the turn passes): B3:6D40; kind 2 moves
		// on to kind 3 in mode 1; other kinds wait
		fl::tstb(peekb(0x01F6));
		step(0x69D6, 24);
		bool turn = !peekb(0x01F6);
		if (!turn)
		{
			step(0x69D8, 12);
			fl::cmpb(peekb(0x0F2B), 4);
			step(0x69DE, 30);
			turn = peekb(0x0F2B) == 4;
			if (!turn)
			{
				step(0x69E0, 12);
				R[1] = 0;
				fl::clr();
				step(0x69E2, 12);
				R[1] = lb(peekb(0x0F2B));
				step(0x69E6, 24);
				fl::tst(R[1]);
				step(0x69E8, 12);
				turn = !R[1];
				if (!turn)
				{
					step(0x69EA, 12);
					fl::cmp(R[1], 2);
					step(0x69EE, 18);
					if (R[1] == 2)
					{
						step(0x69F0, 12);
						pokeb(0x0F26, lb(1));
						step(0x69F6, 33);
						pokeb(0x0F2B, lb(3));
						step(0x69FC, 33);
						return;
					}
					step(0x69FE, 12);
					fl::cmp(R[1], 3);
					step(0x6A02, 18);
					if (R[1] != 3)
					{
						step(0x6A08, 12);
						return;
					}
					step(0x6A04, 12);
				}
				else
					step(0x6A04, 12);
			}
			else
				step(0x6A04, 12);
		}
		else
			step(0x6A04, 12);
		jsr(0x6A08, 0x6D40);
		return;
	}
	default:                                           // mode 0: nothing
		return;
	}
}

// ---------------------------------------------------------------------------------------------
// Park set-up, tickets and the turn change (B3)

// B3:6A1C  load_demo_park(int park): the park view for the attract mode's park demo: map
// (A5:4000 with the two bytes of the tables 0x5666 / 0x566A for the park, bank A 0x1800), tile
// bank from 0x566E (F:EEE6), then F:BA2E, F:CFD6 and F:D19C.  Returns the last result.
uint16_t load_demo_park(const Args &a)
{
	uint16_t *R = T.r;
	SteppedFrame frame(3, 0x6A20);
	R[1] = lw(a.u(0));
	step(0x6A24, 27);
	R[3] = 0;
	fl::clr();
	step(0x6A26, 12);
	R[3] = lb(peekb((uint16_t)(0x5666 + R[1])));
	step(0x6A2A, 27);
	R[2] = 0;
	fl::clr();
	step(0x6A2C, 12);
	R[2] = lb(peekb((uint16_t)(0x566A + R[1])));
	step(0x6A30, 27);
	R[0] = lw(0x1800);
	step(0x6A34, 18);
	poke(0x01D2, lw(R[0]));
	step(0x6A38, 27);
	poke(0x1400, lw(R[0]));
	step(0x6A3C, 27);
	push(0);
	fl::clr();
	step(0x6A3E, 24);
	push(R[3]);
	fl::mov(R[3]);
	step(0x6A40, 24);
	push(R[2]);
	fl::mov(R[2]);
	step(0x6A42, 24);
	jsr(0x6A46, 0x4000);
	R[0] = lw(0x3800);
	step(0x6A4A, 18);
	poke(0x01D2, lw(R[0]));
	step(0x6A4E, 27);
	poke(0x1400, lw(R[0]));
	step(0x6A52, 27);
	ww(R[6], 0);
	fl::clr();
	step(0x6A54, 21);
	wb(R[6], lb(peekb((uint16_t)(0x566E + R[1]))));
	step(0x6A58, 36);
	jsr(0x6A5C, 0xEEE6);                               // set_tile_bank
	jsr(0x6A60, 0xBA2E);
	jsr(0x6A64, 0xCFD6);
	R[0] = jsr(0x6A68, 0xD19C);
	R[6] = fl::add(R[6], 6);
	step(0x6A6C, 18);
	const uint16_t r0 = R[0];
	frame.leave();
	return r0;
}

// B3:7140: count the rounds of the game kind 0x0F30 (after 4: all parks open again, 0x0F29 =
// 0, and 0x018E = 1) and choose the difficulty 0x0F2D of the next park from F:90BA: with a
// medal of grade 0xC0 already won at the park (0x0261 + park, also the second player's
// 0x029F) the level rises on the first round; otherwise it is set to -(level + 1) (at most
// 10) when not positive or on a new round.  0x0F2E gets its absolute value (returned).
uint16_t choose_park_level(const Args &)
{
	uint16_t *R = T.r;
	SteppedFrame frame(2, 0x7144);
	const uint8_t r = peekb(0x0F30);
	R[0] = sext8(r);
	fl::movb(r);
	step(0x7148, 24);
	R[0] = fl::inc(R[0]);
	step(0x714A, 12);
	pokeb(0x0F30, lb((uint8_t)R[0]));
	step(0x714E, 27);
	fl::cmpb((uint8_t)R[0], 4);
	step(0x7152, 18);
	if ((uint8_t)R[0] == 4)
	{
		step(0x7154, 12);
		pokeb(0x0F30, 0);
		fl::clr();
		step(0x7158, 27);
		pokeb(0x0F29, 0);
		fl::clr();
		step(0x715C, 27);
		pokeb(0x018E, lb(1));
		step(0x7162, 33);
	}
	else
		step(0x7162, 12);
	R[1] = 0;
	fl::clr();
	step(0x7164, 12);
	R[1] = lb(peekb(0x0F31));
	step(0x7168, 24);
	const uint8_t v716C = peekb(0x01F6);
	fl::tstb(v716C);
	step(0x716C, 24);
	if (v716C)
	{
		step(0x716E, 12);
		R[0] = 0;
		fl::clr();
		step(0x7170, 12);
		R[0] = lb(peekb((uint16_t)(0x029F + R[1])));
		step(0x7174, 27);
		step(0x7178, 12);
	}
	else
	{
		step(0x7176, 12);
		R[0] = 0;
		fl::clr();
		step(0x7178, 12);
	}
	R[2] = 0;
	fl::clr();
	step(0x717A, 12);
	R[2] = lb(peekb((uint16_t)(0x0261 + R[1])));
	step(0x717E, 27);
	R[0] = lw(R[0] | R[2]);
	step(0x7180, 12);
	const uint16_t won = lw(R[0] & 0x00C0);
	step(0x7184, 18);
	bool raise = false;                                // L7196
	if (won)
	{
		step(0x7186, 12);
		fl::tstb(peekb(0x0F2D));
		step(0x718A, 24);
		raise = (int8_t)peekb(0x0F2D) < 0;
		step(raise ? 0x7196 : 0x718C, 12);
	}
	else
		step(0x718C, 12);
	if (!raise)
	{
		R[0] = jsr(0x7190, 0x90BA);
		fl::cmp(R[0], 1);
		step(0x7194, 18);
		raise = R[0] == 1;
		step(raise ? 0x7196 : 0x71A2, 12);
	}
	bool fresh;                                        // L71AE
	if (raise)
	{
		fl::tstb(peekb(0x0F30));
		step(0x719A, 24);
		fresh = !peekb(0x0F30);
		if (!fresh)
		{
			step(0x719C, 12);
			R[0] = jsr(0x71A0, 0x90BA);
			step(0x71C8, 12);
			pokeb(0x0F2D, lb((uint8_t)R[0]));
			step(0x71CC, 27);
		}
		else
			step(0x71AE, 12);
	}
	else
	{
		const uint8_t v71A6 = peekb(0x0F2D);
		fl::tstb(v71A6);
		step(0x71A6, 24);
		if ((int8_t)v71A6 <= 0)
		{
			step(0x71CC, 12);
			fresh = false;
		}
		else
		{
			step(0x71A8, 12);
			fl::tstb(peekb(0x0F30));
			step(0x71AC, 24);
			fresh = !peekb(0x0F30);
			step(fresh ? 0x71AE : 0x71CC, 12);
		}
	}
	if (fresh)
	{
		R[0] = jsr(0x71B2, 0x90BA);
		R[0] = fl::inc(R[0]);
		step(0x71B4, 12);
		fl::cmp(R[0], 0x000A);
		step(0x71B8, 18);
		if ((int16_t)R[0] >= 10)
		{
			step(0x71BA, 12);
			R[0] = lw(0x000A);
			step(0x71BE, 18);
			step(0x71C6, 12);
		}
		else
		{
			step(0x71C0, 12);
			R[0] = jsr(0x71C4, 0x90BA);
			R[0] = fl::inc(R[0]);
			step(0x71C6, 12);
		}
		R[0] = neg_w(R[0]);
		step(0x71C8, 12);
		pokeb(0x0F2D, lb((uint8_t)R[0]));
		step(0x71CC, 27);
	}
	const uint8_t lv = peekb(0x0F2D);
	R[0] = sext8(lv);
	fl::movb(lv);
	step(0x71D0, 24);
	if ((int16_t)R[0] < 0)
	{
		step(0x71D2, 12);
		R[0] = neg_w(R[0]);
		step(0x71D4, 12);
	}
	else
		step(0x71D4, 12);
	pokeb(0x0F2E, lb((uint8_t)R[0]));
	step(0x71D8, 27);
	const uint16_t r0 = R[0];
	frame.leave();
	return r0;
}

// B3:7E98: the landing score on screen 3: unless the skater crashed (0x031F = 1), the
// trick class 0x01F1 gives a first-time bonus (table 0x5F40, bank A 0x2000; counted in
// 0x0FB4), plus 0x01F2; with terrain bit 0x40 (0x0324) also points for air time and turn
// (table 0x5F00, F:90A6).  Half of it (rounded) is converted by F:F064 into time added to
// 0x0F34 (time bar flagged).  Bank A ends as 0x3800, which is returned.
uint16_t score_landing_screen3(const Args &)
{
	uint16_t *R = T.r;
	SteppedFrame frame(2, 0x7E9C);
	R[0] = lw(0x2000);
	step(0x7EA0, 18);
	poke(0x01D2, lw(R[0]));
	step(0x7EA4, 27);
	poke(0x1400, lw(R[0]));
	step(0x7EA8, 27);
	const uint8_t v7EAE = peekb(0x031F);
	fl::cmpb(v7EAE, 1);
	step(0x7EAE, 30);
	if (v7EAE == 1)
		step(0x7F44, 12);
	else
	{
		step(0x7EB0, 12);
		R[1] = 0;
		fl::clr();
		step(0x7EB2, 12);
		R[0] = 0;
		fl::clr();
		step(0x7EB4, 12);
		R[0] = lb(peekb(0x01F1));
		step(0x7EB8, 24);
		R[2] = lw(0x0FB4);
		step(0x7EBC, 18);
		R[2] = fl::add(R[2], R[0]);
		step(0x7EBE, 12);
		const uint8_t v7EC0 = peekb(R[2]);
		fl::tstb(v7EC0);
		step(0x7EC0, 18);
		if (!v7EC0)
		{
			step(0x7EC2, 12);
			R[1] = 0;
			fl::clr();
			step(0x7EC4, 12);
			R[1] = lb(peekb((uint16_t)(0x5F40 + R[0])));
			step(0x7EC8, 27);
			pokeb(R[2], fl::incb(peekb(R[2])));
			step(0x7ECA, 21);
		}
		else
			step(0x7ECA, 12);
		R[0] = 0;
		fl::clr();
		step(0x7ECC, 12);
		R[0] = lb(peekb(0x01F2));
		step(0x7ED0, 24);
		R[1] = fl::add(R[1], R[0]);
		step(0x7ED2, 12);
		const uint8_t t = lb(peekb(0x0324) & 0x40);
		step(0x7ED8, 30);
		if (t)
		{
			step(0x7EDA, 12);
			R[0] = lw(peek(0x01C4));
			step(0x7EDE, 24);
			R[0] = fl::sub(R[0], peek(0x0FAC));
			step(0x7EE2, 24);
			R[2] = lw(R[0]);
			step(0x7EE4, 12);
			for (int i = 0; i < 4; i++)
			{
				R[2] = asr_w(R[2]);
				step((uint16_t)(0x7EE6 + 2 * i), 12);
			}
			fl::cmp(R[2], 7);
			step(0x7EF0, 18);
			if ((int16_t)R[2] > 7)
			{
				step(0x7EF2, 12);
				R[2] = lw(7);
				step(0x7EF6, 18);
			}
			else
				step(0x7EF6, 12);
			R[0] = lw(peek(0x02F4));
			step(0x7EFA, 24);
			if ((int16_t)R[0] < 0)
			{
				step(0x7EFC, 12);
				R[0] = neg_w(R[0]);
				step(0x7EFE, 12);
			}
			else
				step(0x7EFE, 12);
			push(R[0]);
			fl::mov(R[0]);
			step(0x7F00, 24);
			push(0x0090);
			fl::mov(0x0090);
			step(0x7F04, 30);
			R[0] = jsr(0x7F08, 0x90A6);
			fl::cmp(R[0], 7);
			step(0x7F0C, 18);
			if ((int16_t)R[0] > 7)
			{
				step(0x7F0E, 12);
				R[0] = lw(7);
				step(0x7F12, 18);
			}
			else
				step(0x7F12, 12);
			R[0] = asl_w(R[0]);
			step(0x7F14, 12);
			R[0] = asl_w(R[0]);
			step(0x7F16, 12);
			R[0] = asl_w(R[0]);
			step(0x7F18, 12);
			R[0] = fl::add(R[0], R[2]);
			step(0x7F1A, 12);
			R[2] = 0;
			fl::clr();
			step(0x7F1C, 12);
			R[2] = lb(peekb((uint16_t)(0x5F00 + R[0])));
			step(0x7F20, 27);
			R[1] = fl::add(R[1], R[2]);
			step(0x7F22, 12);
			fl::cmp(rw(R[6]), rw((uint16_t)(R[6] + 2)));
			R[6] = (uint16_t)(R[6] + 4);
			step(0x7F24, 24);
		}
		else
			step(0x7F24, 12);
		R[0] = lw(R[1]);
		step(0x7F26, 12);
		R[0] = fl::inc(R[0]);
		step(0x7F28, 12);
		R[1] = lw(R[0]);
		step(0x7F2A, 12);
		R[1] = asr_w(R[1]);
		step(0x7F2C, 12);
		push(R[1]);
		fl::mov(R[1]);
		step(0x7F2E, 24);
		R[0] = jsr(0x7F32, 0xF064);
		R[1] = lw(R[0]);
		step(0x7F34, 12);
		R[1] = asl_w(R[1]);
		step(0x7F36, 12);
		poke(0x0F34, fl::add(peek(0x0F34), peek((uint16_t)(0xFCFC + R[1]))));
		step(0x7F3C, 42);
		pokeb(0x0FA2, lb((uint8_t)(peekb(0x0FA2) | 0x04)));
		step(0x7F42, 33);
		fl::tst(pop());
		step(0x7F44, 18);
	}
	R[0] = lw(0x3800);
	step(0x7F48, 18);
	poke(0x01D2, lw(R[0]));
	step(0x7F4C, 27);
	poke(0x1400, lw(R[0]));
	step(0x7F50, 27);
	const uint16_t r0 = R[0];
	frame.leave();
	return r0;
}

// B3:72CA  add_tickets(int n): tickets 0x025B += n (at most 4).  In Skate City the ticket
// marks of the current player are drawn (A5:4984, mode 1, sound 0x5F; 0x0ED6 cleared) or
// erased (mode 0) for the change.  Returns the new count before the limit.
uint16_t add_tickets(const Args &a)
{
	uint16_t *R = T.r;
	SteppedFrame frame(3, 0x72CE);
	R[3] = lw(a.u(0));
	step(0x72D2, 27);
	R[1] = 0;
	fl::clr();
	step(0x72D4, 12);
	fl::tst(R[3]);
	step(0x72D6, 12);
	if ((int16_t)R[3] > 0)
	{
		step(0x72D8, 12);
		R[1] = fl::inc(R[1]);
		step(0x72DA, 12);
		push(0x005F);
		fl::mov(0x005F);
		step(0x72DE, 30);
		jsr(0x72E2, 0x8E78);                           // queue_sound
		fl::tst(pop());
		step(0x72E4, 18);
	}
	else
		step(0x72E4, 12);
	const uint8_t v72E8 = peekb(0x0F59);
	fl::tstb(v72E8);
	step(0x72E8, 24);
	if (!v72E8)
	{
		step(0x72EA, 12);
		const uint8_t me = peekb(0x01F5);
		R[0] = sext8(me);
		fl::movb(me);
		step(0x72EE, 24);
		R[0] = asl_w(R[0]);
		step(0x72F0, 12);
		poke(0x0188, lw(peek((uint16_t)(0x51DE + R[0]))));
		step(0x72F6, 42);
		R[0] = lw(0x1800);
		step(0x72FA, 18);
		poke(0x01D2, lw(R[0]));
		step(0x72FE, 27);
		poke(0x1400, lw(R[0]));
		step(0x7302, 27);
		const bool gain = R[1] & 1;                    // ROR R1; BCC
		{
			const uint16_t v = R[1];
			R[1] = (uint16_t)((v >> 1) | (C() << 15));
			f_nzvc(nzw(R[1]) | (v & 1));
			f_shift();
		}
		step(0x7304, 12);
		if (gain)
		{
			step(0x7306, 12);
			const uint8_t t = peekb(0x025B);
			R[0] = sext8(t);
			fl::movb(t);
			step(0x730A, 24);
			R[0] = fl::add(R[0], R[3]);
			step(0x730C, 12);
			fl::cmp(R[0], 4);
			step(0x7310, 18);
			if ((int16_t)R[0] > 4)
			{
				step(0x7316, 12);
				R[2] = lw(4);
				step(0x731A, 18);
			}
			else
			{
				step(0x7312, 12);
				R[2] = lw(R[0]);
				step(0x7314, 12);
				step(0x731A, 12);
			}
			const uint8_t t2 = peekb(0x025B);
			R[1] = sext8(t2);
			fl::movb(t2);
			step(0x731E, 24);
			step(0x7334, 12);
			for (;;)
			{
				R[1] = fl::inc(R[1]);
				step(0x7336, 12);
				fl::cmp(R[1], R[2]);
				step(0x7338, 12);
				if ((int16_t)R[1] > (int16_t)R[2]) break;
				step(0x7320, 12);
				const uint8_t c = peekb(0x01F5);
				R[0] = sext8(c);
				fl::movb(c);
				step(0x7324, 24);
				push(R[0]);
				fl::mov(R[0]);
				step(0x7326, 24);
				push(R[1]);
				fl::mov(R[1]);
				step(0x7328, 24);
				push(1);
				fl::mov(1);
				step(0x732C, 30);
				R[0] = jsr(0x7330, 0x4984);
				R[6] = fl::add(R[6], 6);
				step(0x7334, 18);
			}
			step(0x733A, 12);
			pokeb(0x0ED6, 0);
			fl::clr();
			step(0x733E, 27);
			step(0x7376, 12);
		}
		else
		{
			step(0x7340, 12);
			fl::tst(R[3]);
			step(0x7342, 12);
			if ((int16_t)R[3] < 0)
			{
				step(0x7344, 12);
				const uint8_t t = peekb(0x025B);
				R[0] = sext8(t);
				fl::movb(t);
				step(0x7348, 24);
				R[0] = fl::add(R[0], R[3]);
				step(0x734A, 12);
				R[0] = fl::inc(R[0]);
				step(0x734C, 12);
				T.psw &= ~(CF | VF);
				step(0x734E, 18);
				if ((int16_t)R[0] > 0)
				{
					step(0x7350, 12);
					R[2] = lw(R[0]);
					step(0x7352, 12);
					step(0x7358, 12);
				}
				else
				{
					step(0x7354, 12);
					R[2] = lw(1);
					step(0x7358, 18);
				}
				const uint8_t t2 = peekb(0x025B);
				R[1] = sext8(t2);
				fl::movb(t2);
				step(0x735C, 24);
				step(0x7372, 12);
				for (;;)
				{
					fl::cmp(R[1], R[2]);
					step(0x7374, 12);
					if ((int16_t)R[1] < (int16_t)R[2]) break;
					step(0x735E, 12);
					const uint8_t c = peekb(0x01F5);
					R[0] = sext8(c);
					fl::movb(c);
					step(0x7362, 24);
					push(R[0]);
					fl::mov(R[0]);
					step(0x7364, 24);
					push(R[1]);
					fl::mov(R[1]);
					step(0x7366, 24);
					push(0);
					fl::clr();
					step(0x7368, 24);
					R[0] = jsr(0x736C, 0x4984);
					R[6] = fl::add(R[6], 6);
					step(0x7370, 18);
					R[1] = fl::dec(R[1]);
					step(0x7372, 12);
				}
				step(0x7376, 12);
			}
			else
				step(0x7376, 12);
		}
		R[0] = lw(0x3800);
		step(0x737A, 18);
		poke(0x01D2, lw(R[0]));
		step(0x737E, 27);
		poke(0x1400, lw(R[0]));
		step(0x7382, 27);
	}
	else
		step(0x7382, 12);
	const uint8_t t = peekb(0x025B);
	R[0] = sext8(t);
	fl::movb(t);
	step(0x7386, 24);
	R[0] = fl::add(R[0], R[3]);
	step(0x7388, 12);
	pokeb(0x025B, lb((uint8_t)R[0]));
	step(0x738C, 27);
	fl::cmpb((uint8_t)R[0], 4);
	step(0x7390, 18);
	if ((int8_t)(uint8_t)R[0] > 4)
	{
		step(0x7392, 12);
		pokeb(0x025B, lb(4));
		step(0x7398, 33);
	}
	else
		step(0x7398, 12);
	const uint16_t r0 = R[0];
	frame.leave();
	return r0;
}

// B3:6D40: next turn / next park: in a game kind with a next park (B3:7140), the time is
// restarted (F:F028 add_time(0)); the screen is set up (F:8328 with the start position of
// the park 0x027A, or of the town while 0x0F29 is clear, tables 0x5646-0x5664), tile bank
// 0x567A + park, trick counters reset (B3:7DD6), in a two-player game the game kind steps
// (0x0F2B + 1, & 3), ticket bits (A7:55DE), F:D19C, interrupts (F:81AA), F:8F74, the status
// line (B3:7446) and the park state cleared.  Returns the status line's result.  Waits in
// its callees.
uint16_t next_turn(const Args &)
{
	uint16_t *R = T.r;
	SteppedFrame frame(5, 0x6D44);
	fl::tstb(peekb(0x0F2B));
	step(0x6D48, 24);
	bool choose = peekb(0x0F2B) != 0;
	if (choose)
		step(0x6D50, 12);
	else
	{
		step(0x6D4A, 12);
		fl::tstb(peekb(0x01F6));
		step(0x6D4E, 24);
		choose = !peekb(0x01F6);
		step(choose ? 0x6D50 : 0x6D5A, 12);
	}
	if (choose)
	{
		const uint8_t v6D54 = peekb(0x0F26);
		fl::tstb(v6D54);
		step(0x6D54, 24);
		if (v6D54)
		{
			step(0x6D56, 12);
			jsr(0x6D5A, 0x7140);                       // choose_park_level
		}
		else
			step(0x6D5A, 12);
	}
	poke(0x0EAA, lw(0x0001));
	step(0x6D60, 33);
	const uint8_t v6D64 = peekb(0x0F26);
	fl::tstb(v6D64);
	step(0x6D64, 24);
	if (v6D64)
	{
		step(0x6D66, 12);
		push(0);
		fl::clr();
		step(0x6D68, 24);
		jsr(0x6D6C, 0xF028);                           // add_time
		fl::tst(pop());
		step(0x6D6E, 18);
	}
	else
		step(0x6D6E, 12);
	const uint8_t v6D72 = peekb(0x0F29);
	fl::tstb(v6D72);
	step(0x6D72, 24);
	if (!v6D72)
	{
		step(0x6D74, 12);
		push(lw(peek(0x564E)));
		step(0x6D78, 36);
		push(lw(peek(0x5658)));
		step(0x6D7C, 36);
		push(0);
		fl::clr();
		step(0x6D7E, 24);
		push(0);
		fl::clr();
		step(0x6D80, 24);
		push(0);
		fl::clr();
		step(0x6D82, 24);
		wb(R[6], lb(peekb(0x565E)));
		step(0x6D86, 33);
		push(0);
		fl::clr();
		step(0x6D88, 24);
		wb(R[6], lb(peekb(0x5664)));
		step(0x6D8C, 33);
		step(0x6DB0, 12);
	}
	else
	{
		step(0x6D8E, 12);
		R[1] = 0;
		fl::clr();
		step(0x6D90, 12);
		R[1] = lb(peekb(0x027A));
		step(0x6D94, 24);
		R[0] = lw(R[1]);
		step(0x6D96, 12);
		R[0] = asl_w(R[0]);
		step(0x6D98, 12);
		push(lw(peek((uint16_t)(0x5646 + R[0]))));
		step(0x6D9C, 39);
		push(lw(peek((uint16_t)(0x5650 + R[0]))));
		step(0x6DA0, 39);
		push(0);
		fl::clr();
		step(0x6DA2, 24);
		push(0);
		fl::clr();
		step(0x6DA4, 24);
		push(0);
		fl::clr();
		step(0x6DA6, 24);
		wb(R[6], lb(peekb((uint16_t)(0x565A + R[1]))));
		step(0x6DAA, 36);
		push(0);
		fl::clr();
		step(0x6DAC, 24);
		wb(R[6], lb(peekb((uint16_t)(0x5660 + R[1]))));
		step(0x6DB0, 36);
	}
	jsr(0x6DB4, 0x8328);
	R[0] = 0;
	fl::clr();
	step(0x6DB6, 12);
	R[0] = lb(peekb(0x027A));
	step(0x6DBA, 24);
	R[0] = asl_w(R[0]);
	step(0x6DBC, 12);
	ww(R[6], lw(peek((uint16_t)(0x567A + R[0]))));
	step(0x6DC0, 36);
	jsr(0x6DC4, 0xEEE6);                               // set_tile_bank
	ww(R[6], 0);
	fl::clr();
	step(0x6DC6, 21);
	jsr(0x6DCA, 0x7DD6);                               // update_table_0fb4
	pokeb(0x0EA4, 0);
	fl::clr();
	step(0x6DCE, 27);
	pokeb(0x0ED8, 0);
	fl::clr();
	step(0x6DD2, 27);
	fl::tstb(peekb(0x01F6));
	step(0x6DD6, 24);
	bool step_kind = peekb(0x01F6) != 0;
	if (step_kind)
	{
		step(0x6DD8, 12);
		fl::cmpb(peekb(0x0F2B), 4);
		step(0x6DDE, 30);
		step_kind = peekb(0x0F2B) != 4;
		step(step_kind ? 0x6DE0 : 0x6DF6, 12);
	}
	else
		step(0x6DF6, 12);
	if (step_kind)
	{
		R[0] = 0;
		fl::clr();
		step(0x6DE2, 12);
		R[0] = lb(peekb(0x0F2B));
		step(0x6DE6, 24);
		R[0] = fl::inc(R[0]);
		step(0x6DE8, 12);
		R[1] = lw(R[0]);
		step(0x6DEA, 12);
		R[1] = lw(R[1] & 0x0003);
		step(0x6DEE, 18);
		pokeb(0x0F2B, 0);
		fl::clr();
		step(0x6DF2, 27);
		pokeb(0x0F2B, lb((uint8_t)(peekb(0x0F2B) | (uint8_t)R[1])));
		step(0x6DF6, 27);
	}
	ww(R[6], 3);
	fl::mov(3);
	step(0x6DFA, 27);
	jsr(0x6DFE, 0x55DE);                               // update_ticket_bits
	jsr(0x6E02, 0xD19C);
	jsr(0x6E06, 0x81AA);                               // install_interrupts_a
	jsr(0x6E0A, 0x8F74);
	pokeb(0x0F26, 0);
	fl::clr();
	step(0x6E0E, 27);
	R[0] = jsr(0x6E12, 0x7446);                        // draw_status_line
	pokeb(0x0F28, 0);
	fl::clr();
	step(0x6E16, 27);
	pokeb(0x0F27, 0);
	fl::clr();
	step(0x6E1A, 27);
	pokeb(0x0279, 0);
	fl::clr();
	step(0x6E1E, 27);
	poke(0x01FE, lw(0x001E));
	step(0x6E24, 33);
	pokeb(0x0ED6, 0);
	fl::clr();
	step(0x6E28, 27);
	pokeb(0x0F2A, lb((uint8_t)(peekb(0x0F2A) & ~0x3F)));
	step(0x6E2E, 33);
	pokeb(0x018F, 0);
	fl::clr();
	step(0x6E32, 27);
	R[6] = fl::add(R[6], 12);
	step(0x6E36, 18);
	const uint16_t r0 = R[0];
	frame.leave();
	return r0;
}

// ---------------------------------------------------------------------------------------------
// Game and park start screens (B3) and self test screens (B6).  These wait (for vertical
// blanks, the fade, the sound CPU, button presses) in their callees or themselves; like all
// routines here they run with the original's interrupt points, so waiting loops are polled
// instruction by instruction.

// B3:654C: the start of a game in Skate City: A?:419A, the palette fade set up (0x0EAC = 13,
// 0x0EAE = 2, end routine F:DC3C), a vertical blank, the view (F:8328 at 0x5000 / 0x3800, by
// 6, 3, 7), scroll 0x01AE = 0xE000, interrupts (F:81AA), F:8F74, sound 0x43, the texts 0x78AC
// (and 0x75EA with two players), A?:4D30(0, 0x76FC), and the countdown (8 seconds) shown
uint16_t start_game_town(const Args &)
{
	uint16_t *R = T.r;
	SteppedFrame frame(5, 0x6550);
	jsr(0x6554, 0x419A);
	poke(0x0EAC, lw(0x000D));
	step(0x655A, 33);
	poke(0x0EAE, lw(0x0002));
	step(0x6560, 33);
	poke(0x0EB0, lw(0xDC3C));
	step(0x6566, 33);
	jsr(0x656A, 0x8D50);                               // wait_vblank
	static const uint16_t args[6] = { 0x3800, 0x5000, 0, 7, 3, 6 };
	static const uint16_t next[6] = { 0x656E, 0x6572, 0x6574, 0x6578, 0x657C, 0x6580 };
	for (int i = 0; i < 6; i++)
	{
		push(args[i]);
		fl::mov(args[i]);
		step(next[i], args[i] ? 30 : 24);
	}
	jsr(0x6584, 0x8328);
	poke(0x01AE, lw(0xE000));
	step(0x658A, 33);
	jsr(0x658E, 0x81AA);                               // install_interrupts_a
	jsr(0x6592, 0x8F74);
	ww(R[6], 0x0043);
	fl::mov(0x0043);
	step(0x6596, 27);
	jsr(0x659A, 0x8E78);                               // queue_sound
	ww(R[6], 0x78AC);
	fl::mov(0x78AC);
	step(0x659E, 27);
	jsr(0x65A2, 0xA9FC);
	const uint8_t v65A6 = peekb(0x01F6);
	fl::tstb(v65A6);
	step(0x65A6, 24);
	if (v65A6)
	{
		step(0x65A8, 12);
		ww(R[6], 0x75EA);
		fl::mov(0x75EA);
		step(0x65AC, 27);
		jsr(0x65B0, 0xA9FC);
	}
	else
		step(0x65B0, 12);
	ww(R[6], 0x76FC);
	fl::mov(0x76FC);
	step(0x65B4, 27);
	push(0);
	fl::clr();
	step(0x65B6, 24);
	jsr(0x65BA, 0x4D30);
	poke(0x0F1C, lw(0x0008));
	step(0x65C0, 33);
	pokeb(0x0FA2, lb((uint8_t)(peekb(0x0FA2) | 0x10)));
	step(0x65C6, 33);
	jsr(0x65CA, 0x79D4);                               // draw_countdown
	pokeb(0x0F1E, lb(0x3C));
	step(0x65D0, 33);
	R[6] = fl::add(R[6], 0x000E);
	step(0x65D4, 18);
	const uint16_t r0 = R[0];
	frame.leave();
	return r0;
}

// B3:71DA: start of a park: interrupts set up (F:820C), sound level 3, fade to level 12
// (F:DC24, waits), the title and difficulty texts (A7:52FC picks 0x5E06 + 8n), the countdown
// 0x0F1C = 12 - 0x0F2E (halved, at least 2, when 0x0F2A is not the park's bit), the open parks
// (F:E6FE, texts 0x5E36 + 8n), the time or time-up text, A5:4D30 and the countdown display
uint16_t park_start_screen(const Args &)
{
	uint16_t *R = T.r;
	SteppedFrame frame(2, 0x71DE);
	poke(0x01DC, 0);
	fl::clr();
	step(0x71E2, 27);
	jsr(0x71E6, 0x820C);                               // install_interrupts_b
	jsr(0x71EA, 0xA8FE);
	pokeb(0x01E8, lb(3));
	step(0x71F0, 33);
	push(0x000C);
	fl::mov(0x000C);
	step(0x71F4, 30);
	jsr(0x71F8, 0xDC24);                               // wait_fade
	poke(0x0188, lw(0xE000));
	step(0x71FE, 33);
	ww(R[6], 0x75D6);
	fl::mov(0x75D6);
	step(0x7202, 27);
	jsr(0x7206, 0xA9FC);
	R[0] = jsr(0x720A, 0x52FC);                        // difficulty_step
	R[2] = lw(R[0]);
	step(0x720C, 12);
	R[0] = asl_w(R[0]);
	step(0x720E, 12);
	R[0] = asl_w(R[0]);
	step(0x7210, 12);
	R[0] = asl_w(R[0]);
	step(0x7212, 12);
	R[0] = fl::add(R[0], 0x5E06);
	step(0x7216, 18);
	ww(R[6], lw(R[0]));
	step(0x7218, 21);
	push(0);
	fl::clr();
	step(0x721A, 24);
	jsr(0x721E, 0xEAD4);
	pokeb(0x0F2A, lb((uint8_t)(peekb(0x0F2A) & ~0x80)));
	step(0x7224, 33);
	poke(0x0F1C, lw(0x000C));
	step(0x722A, 33);
	R[0] = 0;
	fl::clr();
	step(0x722C, 12);
	R[0] = lb(peekb(0x0F2E));
	step(0x7230, 24);
	poke(0x0F1C, fl::sub(peek(0x0F1C), R[0]));
	step(0x7234, 27);
	R[1] = 0;
	fl::clr();
	step(0x7236, 12);
	R[1] = lb(peekb((uint16_t)(0xFC74 + R[2])));
	step(0x723A, 27);
	const uint8_t kind = peekb(0x0F2A);
	R[0] = sext8(kind);
	fl::movb(kind);
	step(0x723E, 24);
	fl::cmp(R[0], R[1]);
	step(0x7240, 12);
	if (R[0] != R[1])
	{
		step(0x7242, 12);
		R[0] = lw(peek(0x0F1C));
		step(0x7246, 24);
		R[0] = asr_w(R[0]);
		step(0x7248, 12);
		fl::cmp(R[0], 2);
		step(0x724C, 18);
		if ((int16_t)R[0] < 2)
		{
			step(0x724E, 12);
			R[0] = lw(2);
			step(0x7252, 18);
		}
		else
			step(0x7252, 12);
		poke(0x0F1C, lw(R[0]));
		step(0x7256, 27);
	}
	else
		step(0x7256, 12);
	R[0] = jsr(0x725A, 0xE6FE);                        // open_parks
	R[2] = lw(R[0]);
	step(0x725C, 12);
	R[1] = lw(3);
	step(0x7260, 18);
	for (;;)
	{
		const uint8_t open = lb(peekb((uint16_t)(0xFC74 + R[1])) & (uint8_t)R[2]);
		step(0x7264, 27);
		if (open)
		{
			step(0x7266, 12);
			R[0] = lw(R[1]);
			step(0x7268, 12);
			R[0] = asl_w(R[0]);
			step(0x726A, 12);
			R[0] = asl_w(R[0]);
			step(0x726C, 12);
			R[0] = asl_w(R[0]);
			step(0x726E, 12);
			R[0] = fl::add(R[0], 0x5E36);
			step(0x7272, 18);
			ww(R[6], lw(R[0]));
			step(0x7274, 21);
			push(0);
			fl::clr();
			step(0x7276, 24);
			jsr(0x727A, 0xEAD4);
			fl::tst(pop());
			step(0x727C, 18);
		}
		else
			step(0x727C, 12);
		R[1] = fl::dec(R[1]);
		step(0x727E, 12);
		if ((int16_t)R[1] < 0) break;
		step(0x7260, 12);
	}
	step(0x7280, 12);
	const uint16_t v7284 = peek(0x0254);
	fl::tst(v7284);
	step(0x7284, 24);
	if (!v7284)
	{
		step(0x7286, 12);
		ww(R[6], 0x5F06);
		fl::mov(0x5F06);
		step(0x728A, 27);
		push(1);
		fl::mov(1);
		step(0x728E, 30);
		jsr(0x7292, 0xEAD4);
		step(0x72A8, 12);
	}
	else
	{
		step(0x7294, 12);
		ww(R[6], 0x5DEE);
		fl::mov(0x5DEE);
		step(0x7298, 27);
		push(1);
		fl::mov(1);
		step(0x729C, 30);
		jsr(0x72A0, 0xEAD4);
		R[0] = lw(0x00E4);
		step(0x72A4, 18);
		jsr(0x72A8, 0x8AB6);
	}
	ww(R[6], 0x76FC);
	fl::mov(0x76FC);
	step(0x72AC, 27);
	push(0);
	fl::clr();
	step(0x72AE, 24);
	jsr(0x72B2, 0x4D30);
	pokeb(0x0FA2, lb((uint8_t)(peekb(0x0FA2) | 0x10)));
	step(0x72B8, 33);
	jsr(0x72BC, 0x79D4);                               // draw_countdown
	pokeb(0x0F1E, lb(0x3C));
	step(0x72C2, 33);
	R[6] = fl::add(R[6], 8);
	step(0x72C6, 18);
	const uint16_t r0 = R[0];
	frame.leave();
	return r0;
}

// B6:74E4 (assembly, self test): the sprite test: a column of 8 sprites (motion object list at
// 0x3800, picture 0x3000 + steps) moved with the controller (F:F896) until a button press;
// holding the button turns the pictures over (+8 every frame).  Locals: (SP) scratch,
// 2(SP) picture, 4(SP) list pointer, 6(SP) first picture, 8(SP) x, 0xA(SP) y.
void selftest_sprite_screen()
{
	uint16_t *R = T.r;
	auto L = [&](int off) { return (uint16_t)(R[6] + off); };
	R[6] = fl::sub(R[6], 0x000C);
	step(0x74E8, 18);
	jsr(0x74EC, 0x8D2C);                               // clear_playfield_ram
	jsr(0x74F0, 0x8CE8);                               // clear_playfield
	jsr(0x74F4, 0x8D3E);                               // clear_text_ram
	jsr(0x74F8, 0xDB4E);
	poke(0x0184, lw(0x5BBC));
	step(0x74FE, 33);
	R[0] = lw(0x0023);
	step(0x7502, 18);
	jsr(0x7506, 0x8AB6);
	poke(0x0184, lw(0x5C04));
	step(0x750C, 33);
	R[0] = lw(0x001F);
	step(0x7510, 18);
	jsr(0x7514, 0x8AB6);
	R[0] = lw(0x0020);
	step(0x7518, 18);
	jsr(0x751C, 0x8AB6);
	ww(L(0x0A), lw(0x3000));
	step(0x7522, 36);
	ww(L(0x08), lw(0x4000));
	step(0x7528, 36);
	ww(L(0x06), lw(0x00C0));
	step(0x752E, 36);
	for (;;)
	{
		jsr(0x7532, 0x8D50);                           // wait_vblank
		ww(L(2), lw(rw(L(6))));
		step(0x7538, 45);
		ww(L(4), lw(0x3800));
		step(0x753E, 36);
		poke(0x3804, lw(rw(L(0x0A))));
		step(0x7544, 42);
		R[0] = lw(8);
		step(0x7548, 18);
		for (;;)
		{
			poke(rw(L(4)), lw(rw(L(8))));
			step(0x754E, 51);
			ww(L(4), fl::add(rw(L(4)), 2));
			step(0x7554, 36);
			poke(rw(L(4)), lw(rw(L(2))));
			step(0x755A, 51);
			ww(L(4), fl::add(rw(L(4)), 4));
			step(0x7560, 36);
			R[1] = lw(2);
			step(0x7564, 18);
			R[1] = fl::add(R[1], rw(L(4)));
			step(0x7568, 27);
			ww(L(0), lw(R[1]));
			step(0x756A, 21);
			poke(rw(L(4)), lw(R[1]));
			step(0x756E, 36);
			ww(L(4), fl::add(rw(L(4)), 2));
			step(0x7574, 36);
			ww(L(2), fl::add(rw(L(2)), 0x0800));
			step(0x757A, 36);
			ww(L(2), lw(rw(L(2)) | 0x8000));
			step(0x7580, 36);
			R[0] = (uint16_t)(R[0] - 1);               // SOB
			if (!R[0]) break;
			step(0x7548, 18);
		}
		step(0x7582, 18);
		const uint8_t held = lb(peekb(0x0193) & 0x40);
		step(0x7588, 30);
		bool turn = !held;
		if (turn)
		{
			step(0x758A, 12);
			fl::cmpb(peekb(0x0FE7), 0x3C);
			step(0x7590, 30);
			turn = peekb(0x0FE7) != 0x3C;
		}
		if (turn)
		{
			step(0x7592, 12);
			fl::tstb(peekb(0x0FE7));
			step(0x7596, 24);
			turn = peekb(0x0FE7) != 0;
		}
		if (turn)
		{
			step(0x7598, 12);
			ww(L(6), fl::add(rw(L(6)), 8));
			step(0x759E, 36);
		}
		else
			step(0x759E, 12);
		jsr(0x75A2, 0xF8EA);                           // button_hold_timer
		push(0);
		fl::clr();
		step(0x75A4, 24);
		R[0] = jsr(0x75A8, 0xF896);                    // rotary_zone_step
		R[0] = fl::add(R[0], rw(L(0x0A)));
		step(0x75AC, 27);
		ww(L(0x0A), lw(R[0]));
		step(0x75B0, 30);
		ww(L(0), 1);
		fl::mov(1);
		step(0x75B4, 27);
		R[0] = jsr(0x75B8, 0xF896);
		R[0] = fl::add(R[0], rw(L(0x0C)));
		step(0x75BC, 27);
		ww(L(0x0C), lw(R[0]));
		step(0x75C0, 30);
		fl::tst(pop());
		step(0x75C2, 18);
		const uint8_t v75C6 = peekb(0x0FE6);
		fl::tstb(v75C6);
		step(0x75C6, 24);
		if (v75C6) break;
		step(0x752E, 12);
	}
	step(0x75C8, 12);
	jsr(0x75CC, 0x8D3E);                               // clear_text_ram
	pokeb(0x0FE6, 0);
	fl::clr();
	step(0x75D0, 27);
	R[6] = fl::add(R[6], 0x000C);
	step(0x75D4, 18);
}

// B6:7388 (assembly, self test): the colour bars: the colour test pattern (B6:7304), titles,
// 39 motion objects in rows of 8 at 0x3800 (colours from 0x7048), test colours in the palette;
// then until a button press the object at the cursor 0x3800 + 8n is moved with the controller
// and the cursor steps on while the button is held (n shown, F:BA9A).  Locals: (SP) the
// cursor, 2(SP) n.
void selftest_colour_bars_screen()
{
	uint16_t *R = T.r;
	fl::cmp(rw((uint16_t)(R[6] - 2)), rw((uint16_t)(R[6] - 4)));
	R[6] = (uint16_t)(R[6] - 4);                       // CMP -(SP),-(SP)
	step(0x738A, 30);
	jsr(0x738E, 0x7304);                               // colour_test_pattern
	poke(0x0188, lw(0xE000));
	step(0x7394, 33);
	poke(0x0184, lw(0x5BBC));
	step(0x739A, 33);
	R[0] = lw(0x0021);
	step(0x739E, 18);
	jsr(0x73A2, 0x8AB6);
	poke(0x0184, lw(0x5C04));
	step(0x73A8, 33);
	R[0] = lw(0x001F);
	step(0x73AC, 18);
	jsr(0x73B0, 0x8AB6);
	R[0] = lw(0x0020);
	step(0x73B4, 18);
	jsr(0x73B8, 0x8AB6);
	R[0] = lw(0x0007);
	step(0x73BC, 18);
	R[3] = lw(0x7000);
	step(0x73C0, 18);
	ww(R[6], lw(0x3800));
	step(0x73C4, 27);
	for (;;)
	{
		R[0] = fl::inc(R[0]);
		step(0x73C6, 12);
		const uint16_t b = lw(R[0] & 0x0007);
		step(0x73CA, 18);
		if (!b)
		{
			step(0x73CC, 12);
			R[2] = lw(0x1000);
			step(0x73D0, 18);
			R[3] = fl::sub(R[3], 0x0C00);
			step(0x73D4, 18);
		}
		else
			step(0x73D4, 12);
		poke(rw(R[6]), lw(R[3]));
		step(0x73D8, 36);
		ww(R[6], fl::add(rw(R[6]), 2));
		step(0x73DC, 27);
		poke(rw(R[6]), lw(1));
		step(0x73E2, 42);
		ww(R[6], fl::add(rw(R[6]), 2));
		step(0x73E6, 27);
		poke(rw(R[6]), lw(R[2]));
		step(0x73EA, 36);
		ww(R[6], fl::add(rw(R[6]), 2));
		step(0x73EE, 27);
		R[1] = lw(2);
		step(0x73F2, 18);
		R[1] = fl::add(R[1], rw(R[6]));
		step(0x73F4, 18);
		R[4] = lw(R[1]);
		step(0x73F6, 12);
		R[1] = lw(R[0]);
		step(0x73F8, 12);
		R[1] = lw(R[1] & 0x0003);
		step(0x73FC, 18);
		R[1] = asl_w(R[1]);
		step(0x73FE, 12);
		R[4] = lw(R[4] & 0x0FFF);
		step(0x7402, 18);
		R[4] = lw(R[4] | peek((uint16_t)(0x7048 + R[1])));
		step(0x7406, 27);
		poke(rw(R[6]), lw(R[4]));
		step(0x740A, 36);
		ww(R[6], fl::add(rw(R[6]), 2));
		step(0x740E, 27);
		R[2] = fl::add(R[2], 0x0C00);
		step(0x7412, 18);
		fl::cmp(rw(R[6]), 0x3938);
		step(0x7416, 24);
		if (rw(R[6]) >= 0x3938) break;
		step(0x73C4, 12);
	}
	step(0x7418, 12);
	ww((uint16_t)(R[6] + 2), 0);
	fl::clr();
	step(0x741C, 30);
	ww(R[6], lw(0x3800));
	step(0x7420, 27);
	jsr(0x7424, 0x8D50);                               // wait_vblank
	static const uint16_t colour_addr[8] = { 0x101C, 0x103C, 0x105C, 0x107C, 0x1110, 0x1112, 0x1114, 0x1116 };
	static const uint16_t colour[8] = { 0x00BB, 0x0CCB, 0xC0CB, 0xAAAB, 0x0A0A, 0x0A0A, 0xA00A, 0xA00A };
	for (int i = 0; i < 8; i++)
	{
		poke(colour_addr[i], lw(colour[i]));
		step((uint16_t)(0x742A + 6 * i), 33);
	}
	for (;;)
	{
		const uint8_t v7458 = peekb(0x0FE6);
		fl::tstb(v7458);
		step(0x7458, 24);
		if (v7458) break;
		step(0x745A, 12);
		jsr(0x745E, 0xF8EA);                           // button_hold_timer
		jsr(0x7462, 0x8D50);                           // wait_vblank
		poke(0x0184, lw(0x5BBC));
		step(0x7468, 33);
		R[0] = lw(0x0022);
		step(0x746C, 18);
		jsr(0x7470, 0x8AB6);
		push(lw(rw((uint16_t)(R[6] + 2))));
		step(0x7474, 39);
		push(0);
		fl::clr();
		step(0x7476, 24);
		push(2);
		fl::mov(2);
		step(0x747A, 30);
		push(rw(R[6]));
		fl::mov(2);
		step(0x747C, 30);
		push(0);
		fl::clr();
		step(0x747E, 24);
		jsr(0x7482, 0xBA9A);
		ww(R[6], 0);
		fl::clr();
		step(0x7484, 21);
		R[0] = jsr(0x7488, 0xF896);                    // rotary_zone_step
		const uint16_t cursor = rw((uint16_t)(R[6] + 0x0A));
		R[0] = fl::add(R[0], peek(cursor));
		step(0x748C, 33);
		poke(cursor, lw(R[0]));
		step(0x7490, 36);
		ww(R[6], 1);
		fl::mov(1);
		step(0x7494, 27);
		R[0] = jsr(0x7498, 0xF896);
		R[1] = lw(rw((uint16_t)(R[6] + 0x0A)));
		step(0x749C, 27);
		R[0] = fl::add(R[0], peek((uint16_t)(R[1] + 4)));
		step(0x74A0, 27);
		poke((uint16_t)(R[1] + 4), lw(R[0]));
		step(0x74A4, 30);
		const uint8_t held = lb(peekb(0x0193) & 0x40);
		step(0x74AA, 30);
		bool next = !held;
		if (next)
		{
			step(0x74AC, 12);
			fl::cmpb(peekb(0x0FE7), 0x3C);
			step(0x74B2, 30);
			next = peekb(0x0FE7) != 0x3C;
		}
		if (next)
		{
			step(0x74B4, 12);
			fl::tstb(peekb(0x0FE7));
			step(0x74B8, 24);
			next = peekb(0x0FE7) != 0;
		}
		if (next)
		{
			step(0x74BA, 12);
			ww((uint16_t)(R[6] + 0x0A), fl::add(rw((uint16_t)(R[6] + 0x0A)), 8));
			step(0x74C0, 36);
			ww((uint16_t)(R[6] + 0x0C), fl::inc(rw((uint16_t)(R[6] + 0x0C))));
			step(0x74C4, 30);
			fl::cmp(rw((uint16_t)(R[6] + 0x0C)), 0x0027);
			step(0x74CA, 33);
			if (rw((uint16_t)(R[6] + 0x0C)) == 0x0027)
			{
				step(0x74CC, 12);
				ww((uint16_t)(R[6] + 0x0A), lw(0x3800));
				step(0x74D2, 36);
				ww((uint16_t)(R[6] + 0x0C), 0);
				fl::clr();
				step(0x74D6, 30);
			}
			else
				step(0x74D6, 12);
		}
		else
			step(0x74D6, 12);
		R[6] = fl::add(R[6], 0x000A);
		step(0x74DA, 18);
		step(0x7454, 12);
	}
	step(0x74DC, 12);
	pokeb(0x0FE6, 0);
	fl::clr();
	step(0x74E0, 27);
	fl::cmp(rw(R[6]), rw((uint16_t)(R[6] + 2)));
	R[6] = (uint16_t)(R[6] + 4);
	step(0x74E2, 24);
}

// B6:713E (assembly, self test): the playfield test: a pattern of tiles over the whole
// playfield (both video windows through F:8078) and the playfield colours; after a button
// press (F:8D60) a frame of corner tiles, and the colour 0x1100 cycles through 5 entries
// (texts from 0x7060) with each button press until a press of the other button (0x0FE6).
// Locals: (SP) rows, 2(SP) bands, 4(SP) halves, 6(SP) tile step.
void selftest_playfield_screen()
{
	uint16_t *R = T.r;
	auto L = [&](int off) { return (uint16_t)(R[6] + off); };
	R[6] = fl::sub(R[6], 8);
	step(0x7142, 18);
	jsr(0x7146, 0x8D3E);                               // clear_text_ram
	jsr(0x714A, 0x8D2C);                               // clear_playfield_ram
	jsr(0x714E, 0x8CE8);                               // clear_playfield
	poke(0x0184, lw(0x5BBC));
	step(0x7154, 33);
	R[0] = lw(0x0019);
	step(0x7158, 18);
	jsr(0x715C, 0x8AB6);
	poke(0x01AE, 0);
	fl::clr();
	step(0x7160, 27);
	poke(0x01AC, lw(0x0011));
	step(0x7166, 33);
	R[2] = lw(0x0800);
	step(0x716A, 18);
	R[1] = lw(0x4000);
	step(0x716E, 18);
	ww(L(6), 0);
	fl::clr();
	step(0x7172, 30);
	ww(L(4), lw(2));
	step(0x7178, 36);
	for (;;)                                           // halves
	{
		ww(L(2), lw(6));
		step(0x717E, 36);
		for (;;)                                       // bands
		{
			R[2] = fl::add(R[2], rw(L(6)));
			step(0x7182, 27);
			ww(L(0), lw(8));
			step(0x7186, 27);
			for (;;)                                   // rows
			{
				R[3] = 0;
				fl::clr();
				step(0x7188, 12);
				for (;;)                               // 16 tiles
				{
					R[4] = lw(2);
					step(0x718C, 18);
					for (;;)                           // 2 cells
					{
						R[0] = lw(R[1]);
						step(0x718E, 12);
						R[0] = lw(R[0] & 0x2000);
						step(0x7192, 18);
						R[0] = lw(R[0] | 0x4000);
						step(0x7196, 18);
						jsr(0x719A, 0x8078);           // select_video_bank
						R[0] = lw(R[1]);
						step(0x719C, 12);
						R[0] = lw(R[0] & 0x1FFE);
						step(0x71A0, 18);
						R[5] = lw(R[0]);
						step(0x71A2, 12);
						R[5] = lw(R[5] | 0x2000);
						step(0x71A6, 18);
						poke(R[5], lw(R[2]));
						step(0x71A8, 21);
						poke(R[5], lw((uint16_t)(peek(R[5]) | R[3])));
						step(0x71AA, 21);
						R[1] = fl::add(R[1], 2);
						step(0x71AE, 18);
						R[4] = (uint16_t)(R[4] - 1);   // SOB
						if (!R[4]) break;
						step(0x718C, 18);
					}
					step(0x71B0, 18);
					R[3] = fl::inc(R[3]);
					step(0x71B2, 12);
					fl::cmp(R[3], 0x000F);
					step(0x71B6, 18);
					if ((int16_t)R[3] > 15) break;
					step(0x7188, 12);
				}
				step(0x71B8, 12);
				R[1] = fl::add(R[1], 0x00C0);
				step(0x71BC, 18);
				ww(L(0), fl::dec(rw(L(0))));
				step(0x71BE, 21);
				if (!rw(L(0))) break;
				step(0x7186, 12);
			}
			step(0x71C0, 12);
			ww(L(2), fl::dec(rw(L(2))));
			step(0x71C4, 30);
			if (!rw(L(2))) break;
			step(0x717E, 12);
		}
		step(0x71C6, 12);
		ww(L(6), lw(0x0800));
		step(0x71CC, 36);
		R[1] = lw(0x4040);
		step(0x71D0, 18);
		ww(L(4), fl::dec(rw(L(4))));
		step(0x71D4, 30);
		if (!rw(L(4))) break;
		step(0x7178, 12);
	}
	step(0x71D6, 12);
	jsr(0x71DA, 0x8D50);                               // wait_vblank
	R[2] = lw(0xFFEF);
	step(0x71DE, 18);
	R[4] = lw(4);
	step(0x71E2, 18);
	for (;;)
	{
		R[0] = lw(R[4]);
		step(0x71E4, 12);
		R[0] = asl_w(R[0]);
		step(0x71E6, 12);
		R[1] = lw(peek((uint16_t)(0x703E + R[0])));
		step(0x71EA, 27);
		R[2] = fl::add(R[2], 0x0020);
		step(0x71EE, 18);
		R[3] = lw(0x0010);
		step(0x71F2, 18);
		for (;;)
		{
			R[0] = lw(R[3]);
			step(0x71F4, 12);
			R[0] = fl::add(R[0], R[2]);
			step(0x71F6, 12);
			R[0] = asl_w(R[0]);
			step(0x71F8, 12);
			poke((uint16_t)(0x1100 + R[0]), lw(R[1]));
			step(0x71FC, 30);
			R[1] = fl::inc(R[1]);
			step(0x71FE, 12);
			R[3] = (uint16_t)(R[3] - 1);               // SOB
			if (!R[3]) break;
			step(0x71F2, 18);
		}
		step(0x7200, 18);
		R[4] = (uint16_t)(R[4] - 1);                   // SOB
		if (!R[4]) break;
		step(0x71E2, 18);
	}
	step(0x7202, 18);
	R[2] = lw(0xFFFF);
	step(0x7206, 18);
	R[0] = lw(0x1000);
	step(0x720A, 18);
	R[5] = lw(3);
	step(0x720E, 18);
	for (;;)
	{
		R[1] = lw(0x000F);
		step(0x7212, 18);
		R[2] = fl::add(R[2], 0x0020);
		step(0x7216, 18);
		R[4] = lw(0x0010);
		step(0x721A, 18);
		for (;;)
		{
			R[3] = lw(R[4]);
			step(0x721C, 12);
			R[3] = fl::add(R[3], R[2]);
			step(0x721E, 12);
			R[3] = asl_w(R[3]);
			step(0x7220, 12);
			poke((uint16_t)(0x1100 + R[3]), lw(R[1]));
			step(0x7224, 30);
			R[1] = fl::add(R[1], R[0]);
			step(0x7226, 12);
			R[4] = (uint16_t)(R[4] - 1);               // SOB
			if (!R[4]) break;
			step(0x721A, 18);
		}
		step(0x7228, 18);
		R[3] = lw(R[0]);
		step(0x722A, 12);
		R[3] = asr_w(R[3]);
		step(0x722C, 12);
		R[3] = asr_w(R[3]);
		step(0x722E, 12);
		R[0] = lw(R[3]);
		step(0x7230, 12);
		R[0] = asr_w(R[0]);
		step(0x7232, 12);
		R[0] = asr_w(R[0]);
		step(0x7234, 12);
		R[5] = (uint16_t)(R[5] - 1);                   // SOB
		if (!R[5]) break;
		step(0x720E, 18);
	}
	step(0x7236, 18);
	jsr(0x723A, 0x8D60);                               // wait_button_press
	jsr(0x723E, 0x8CE8);                               // clear_playfield
	jsr(0x7242, 0x8068);                               // select_video_bank0
	poke(0x1086, 0);
	fl::clr();
	step(0x7246, 27);
	poke(0x2000, lw(0x0130));
	step(0x724C, 33);
	poke(0x207E, lw(0x0131));
	step(0x7252, 33);
	poke(0x3780, lw(0x0132));
	step(0x7258, 33);
	poke(0x37FE, lw(0x0133));
	step(0x725E, 33);
	R[2] = lw(0xFFFF);
	step(0x7262, 18);
	for (;;)
	{
		R[2] = fl::inc(R[2]);
		step(0x7264, 12);
		fl::cmp(R[2], 4);
		step(0x7268, 18);
		if ((int16_t)R[2] > 4)
		{
			step(0x726A, 12);
			R[2] = 0;
			fl::clr();
			step(0x726C, 12);
		}
		else
			step(0x726C, 12);
		R[1] = lw(R[2]);
		step(0x726E, 12);
		R[1] = asl_w(R[1]);
		step(0x7270, 12);
		R[0] = lw(peek((uint16_t)(0x7060 + R[1])));
		step(0x7274, 27);
		jsr(0x7278, 0x8AB6);
		poke(0x1100, lw(peek((uint16_t)(0x7056 + R[1]))));
		step(0x727E, 42);
		bool done = false;
		for (;;)
		{
			const uint8_t held = lb(peekb(0x0193) & 0x40);
			step(0x7284, 30);
			if (held)
			{
				step(0x7292, 12);
				break;
			}
			step(0x7286, 12);
			const uint8_t v728A = peekb(0x0FE6);
			fl::tstb(v728A);
			step(0x728A, 24);
			if (v728A)
			{
				step(0x728C, 12);
				pokeb(0x0FE6, 0);
				fl::clr();
				step(0x7290, 27);
				step(0x72AA, 12);
				done = true;
				break;
			}
			step(0x727E, 12);
		}
		if (done) break;
		for (;;)
		{
			const uint8_t held = lb(peekb(0x0193) & 0x40);
			step(0x7298, 30);
			if (!held) break;
			step(0x7292, 12);
		}
		step(0x729A, 12);
		R[1] = lw(peek((uint16_t)(0x7060 + R[1])));
		step(0x729E, 27);
		R[1] = lw(R[1] | 0x0080);
		step(0x72A2, 18);
		R[0] = lw(R[1]);
		step(0x72A4, 12);
		jsr(0x72A8, 0x8AB6);
		step(0x7262, 12);
	}
	R[6] = fl::add(R[6], 8);
	step(0x72AE, 18);
}

// B6:6B7E (assembly, self test): the controller test: the controller position (F:8D86,
// F:8E2E steps) and the two buttons are shown every frame until a press of the other button
// (0x0FE6); the settings chosen (bits in 8(SP)) are then sent to the sound CPU (commands 0x16
// and the setting, F:8F50, F:8F62 answers polled up to 11 times) and confirmed after a button
// press.  Locals: (SP) scratch / poll counter, 2(SP) position parity, 4(SP) and 6(SP) the two
// toggles, 8(SP) the settings.
void selftest_controls_screen()
{
	uint16_t *R = T.r;
	auto L = [&](int off) { return (uint16_t)(R[6] + off); };
	R[6] = fl::sub(R[6], 0x000A);
	step(0x6B82, 18);
	jsr(0x6B86, 0x8D2C);                               // clear_playfield_ram
	jsr(0x6B8A, 0x8D86);
	jsr(0x6B8E, 0x8E2E);                               // rotary_step
	poke(0x0184, lw(0x5C04));
	step(0x6B94, 33);
	R[0] = lw(0x001F);
	step(0x6B98, 18);
	jsr(0x6B9C, 0x8AB6);
	for (int i = 0; i < 4; i++)
	{
		ww(L(8 - 2 * i), 0);
		fl::clr();
		step((uint16_t)(0x6BA0 + 4 * i), 30);
	}
	R[1] = 0;
	fl::clr();
	step(0x6BAE, 12);
	for (;;)
	{
		R[0] = lw(R[1]);
		step(0x6BB0, 12);
		R[0] = fl::add(R[0], 0x003D);
		step(0x6BB4, 18);
		jsr(0x6BB8, 0x8AB6);
		R[1] = fl::inc(R[1]);
		step(0x6BBA, 12);
		fl::cmp(R[1], 5);
		step(0x6BBE, 18);
		if ((int16_t)R[1] > 5) break;
		step(0x6BAE, 12);
	}
	step(0x6BC0, 12);
	for (;;)                                           // while the button is still pressed
	{
		const uint8_t v6BC4 = peekb(0x0193);
		fl::tstb(v6BC4);
		step(0x6BC4, 24);
		if (!(v6BC4 & 0x80)) break;
		step(0x6BC6, 12);
		ww(L(0), fl::inc(rw(L(0))));
		step(0x6BC8, 21);
		step(0x6BC0, 12);
	}
	step(0x6BCA, 12);
	for (;;)
	{
		jsr(0x6BCE, 0x8D50);                           // wait_vblank
		jsr(0x6BD2, 0x8D86);
		R[0] = jsr(0x6BD6, 0x8E2E);                    // rotary_step
		ww(L(0), lw(R[0]));
		step(0x6BD8, 21);
		R[0] = lw(rw(L(2)));
		step(0x6BDC, 27);
		R[0] = fl::add(R[0], rw(L(0)));
		step(0x6BDE, 18);
		ww(L(2), lw(R[0]));
		step(0x6BE2, 30);
		ww(L(2), lw(rw(L(2)) & 0x0001));
		step(0x6BE8, 36);
		const uint8_t pressed = lb(peekb(0x0193) & 0x40);
		step(0x6BEE, 30);
		if (pressed)
		{
			step(0x6BF0, 12);
			fl::tst(rw(L(2)));
			step(0x6BF4, 27);
			if (!rw(L(2)))
			{
				step(0x6BF6, 12);
				R[0] = lw(1);
				step(0x6BFA, 18);
				ww(L(6), lw((uint16_t)(rw(L(6)) ^ R[0])));
				step(0x6BFE, 30);
				step(0x6C08, 12);
			}
			else
			{
				step(0x6C00, 12);
				R[0] = lw(1);
				step(0x6C04, 18);
				ww(L(4), lw((uint16_t)(rw(L(4)) ^ R[0])));
				step(0x6C08, 30);
			}
			for (;;)
			{
				const uint8_t held = lb(peekb(0x0193) & 0x40);
				step(0x6C0E, 30);
				if (!held) break;
				step(0x6C08, 12);
			}
			step(0x6C10, 12);
		}
		else
			step(0x6C10, 12);
		fl::tst(rw(L(6)));
		step(0x6C14, 27);
		if (!rw(L(6)))
		{
			step(0x6C16, 12);
			R[0] = lw(0x0043);
			step(0x6C1A, 18);
			jsr(0x6C1E, 0x8AB6);
			ww(L(8), lw(rw(L(8)) & ~0x0001));
			step(0x6C24, 36);
			step(0x6C34, 12);
		}
		else
		{
			step(0x6C26, 12);
			R[0] = lw(0x0044);
			step(0x6C2A, 18);
			jsr(0x6C2E, 0x8AB6);
			ww(L(8), lw(rw(L(8)) | 0x0001));
			step(0x6C34, 36);
		}
		fl::tst(rw(L(2)));
		step(0x6C38, 27);
		if (!rw(L(2)))
		{
			step(0x6C3A, 12);
			R[0] = lw(0x003F);
			step(0x6C3E, 18);
			step(0x6C44, 12);
		}
		else
		{
			step(0x6C40, 12);
			R[0] = lw(0x0023);
			step(0x6C44, 18);
		}
		jsr(0x6C48, 0x8B52);                           // draw_large_char
		fl::tst(rw(L(4)));
		step(0x6C4C, 27);
		if (!rw(L(4)))
		{
			step(0x6C4E, 12);
			R[0] = lw(0x0045);
			step(0x6C52, 18);
			jsr(0x6C56, 0x8AB6);
			ww(L(8), lw(rw(L(8)) & ~0x0006));
			step(0x6C5C, 36);
			step(0x6C6C, 12);
		}
		else
		{
			step(0x6C5E, 12);
			R[0] = lw(0x0046);
			step(0x6C62, 18);
			jsr(0x6C66, 0x8AB6);
			ww(L(8), lw(rw(L(8)) | 0x0006));
			step(0x6C6C, 36);
		}
		fl::tst(rw(L(2)));
		step(0x6C70, 27);
		if (rw(L(2)))
		{
			step(0x6C72, 12);
			R[0] = lw(0x003F);
			step(0x6C76, 18);
			step(0x6C7C, 12);
		}
		else
		{
			step(0x6C78, 12);
			R[0] = lw(0x0023);
			step(0x6C7C, 18);
		}
		jsr(0x6C80, 0x8B52);                           // draw_large_char
		const uint8_t v6C84 = peekb(0x0FE6);
		fl::tstb(v6C84);
		step(0x6C84, 24);
		if (v6C84) break;
		step(0x6BCA, 12);
	}
	step(0x6C86, 12);
	pokeb(0x0FE6, 0);
	fl::clr();
	step(0x6C8A, 27);
	fl::tst(rw(L(8)));
	step(0x6C8E, 27);
	if (rw(L(8)))
	{
		step(0x6C90, 12);
		jsr(0x6C94, 0x8D2C);                           // clear_playfield_ram
		R[0] = lw(0x0047);
		step(0x6C98, 18);
		jsr(0x6C9C, 0x8AB6);
		R[0] = lw(0x0048);
		step(0x6CA0, 18);
		jsr(0x6CA4, 0x8AB6);
		push(0x0016);
		fl::mov(0x0016);
		step(0x6CA8, 30);
		jsr(0x6CAC, 0x8EA2);                           // send_sound_command
		jsr(0x6CB0, 0x8F50);
		ww(R[6], lw(rw(L(0x0A))));
		step(0x6CB4, 36);
		jsr(0x6CB8, 0x8EA2);                           // send_sound_command
		R[1] = lw(0x000B);
		step(0x6CBC, 18);
		for (;;)
		{
			pokeb(0x01A2, 0);
			fl::clr();
			step(0x6CC0, 27);
			jsr(0x6CC4, 0x8F62);
			const uint8_t v6CC8 = peekb(0x01A2);
			fl::tstb(v6CC8);
			step(0x6CC8, 24);
			if (!v6CC8)
			{
				step(0x6CCC, 12);
				break;
			}
			step(0x6CCA, 12);
			R[1] = (uint16_t)(R[1] - 1);               // SOB
			if (!R[1])
			{
				step(0x6CCC, 18);
				break;
			}
			step(0x6CBC, 18);
		}
		R[0] = lw(0x0148);
		step(0x6CD0, 18);
		jsr(0x6CD4, 0x8AB6);
		const uint8_t v6CD8 = peekb(0x01A2);
		fl::tstb(v6CD8);
		step(0x6CD8, 24);
		if (!v6CD8)
		{
			step(0x6CDA, 12);
			R[0] = lw(0x0049);
			step(0x6CDE, 18);
			jsr(0x6CE2, 0x8AB6);
			step(0x6CE8, 12);
		}
		else
		{
			step(0x6CE4, 12);
			jsr(0x6CE8, 0x8A9E);
		}
		R[0] = lw(0x001F);
		step(0x6CEC, 18);
		jsr(0x6CF0, 0x8AB6);
		jsr(0x6CF4, 0x8D60);                           // wait_button_press
		fl::tst(pop());
		step(0x6CF6, 18);
	}
	else
		step(0x6CF6, 12);
	R[6] = fl::add(R[6], 0x000A);
	step(0x6CFA, 18);
}

// B6:6CFC (assembly, self test): the switch tests: every 16 frames the DIP switch settings
// (A?:435A reads them into 0x01FA, 0x01F3) are shown as texts until a press of the other
// button; then the coin and game settings 0x01F4 are edited with the controller and the
// button (two toggles in 4(SP) and 6(SP), the setting in 2(SP)) and sent to the sound CPU
// (commands 0x1B and the setting, F:8F50, F:8F62).  Locals: (SP) scratch, 2(SP) setting,
// 4(SP) position parity, 6(SP) toggle, 8(SP) step.
void selftest_switches_screen()
{
	uint16_t *R = T.r;
	auto L = [&](int off) { return (uint16_t)(R[6] + off); };
	R[6] = fl::sub(R[6], 0x000A);
	step(0x6D00, 18);
	jsr(0x6D04, 0x8D2C);                               // clear_playfield_ram
	R[0] = lw(0x001F);
	step(0x6D08, 18);
	jsr(0x6D0C, 0x8AB6);
	R[1] = 0;
	fl::clr();
	step(0x6D0E, 12);
	for (;;)
	{
		R[0] = lw(R[1]);
		step(0x6D10, 12);
		R[0] = fl::add(R[0], 0x004A);
		step(0x6D14, 18);
		jsr(0x6D18, 0x8AB6);
		R[1] = fl::inc(R[1]);
		step(0x6D1A, 12);
		fl::cmp(R[1], 4);
		step(0x6D1E, 18);
		if ((int16_t)R[1] > 4) break;
		step(0x6D0E, 12);
	}
	step(0x6D20, 12);
	for (;;)
	{
		const uint8_t v6D24 = peekb(0x0193);
		fl::tstb(v6D24);
		step(0x6D24, 24);
		if (!(v6D24 & 0x80)) break;
		step(0x6D26, 12);
		ww(L(0), fl::inc(rw(L(0))));
		step(0x6D28, 21);
		step(0x6D20, 12);
	}
	step(0x6D2A, 12);
	// a text by two bits of the switches (the 2-bit groups at (SP), each shown and shifted)
	auto show = [&](uint16_t at, uint16_t mask, uint16_t base) {
		R[0] = lw(rw(L(0)));
		step((uint16_t)(at + 2), 18);
		R[0] = lw(R[0] & ~mask);
		step((uint16_t)(at + 6), 18);
		R[0] = fl::add(R[0], base);
		step((uint16_t)(at + 10), 18);
		jsr((uint16_t)(at + 14), 0x8AB6);
	};
	auto shift = [&](uint16_t at) {
		ww(L(0), asr_w(rw(L(0))));
		step((uint16_t)(at + 2), 21);
	};
	auto text = [&](uint16_t at, uint16_t t) {
		R[0] = lw(t);
		step((uint16_t)(at + 4), 18);
		jsr((uint16_t)(at + 8), 0x8AB6);
	};
	for (;;)
	{
		jsr(0x6D2E, 0x8D50);                           // wait_vblank
		const uint8_t f = lb(peekb(0x01C4) & 0x0F);
		step(0x6D34, 30);
		if (!f)
		{
			step(0x6D36, 12);
			jsr(0x6D3A, 0x435A);
			ww(L(0), 0);
			fl::clr();
			step(0x6D3C, 21);
			wb(L(0), lb(peekb(0x01FA)));
			step(0x6D40, 33);
			show(0x6D40, 0xFFFC, 0x004F);
			shift(0x6D4E);
			shift(0x6D50);
			text(0x6D52, 0x0053);
			show(0x6D5A, 0xFFFC, 0x0054);
			shift(0x6D68);
			shift(0x6D6A);
			text(0x6D6C, 0x0058);
			show(0x6D74, 0xFFFE, 0x0059);
			shift(0x6D82);
			show(0x6D84, 0xFFF8, 0x005B);
			R[0] = 0;
			fl::clr();
			step(0x6D94, 12);
			R[0] = lb(peekb(0x01F3));
			step(0x6D98, 24);
			for (int i = 0; i < 6; i++)
			{
				R[0] = asr_w(R[0]);
				step((uint16_t)(0x6D9A + 2 * i), 12);
			}
			R[0] = fl::add(R[0], 0x0063);
			step(0x6DA8, 18);
			jsr(0x6DAC, 0x8AB6);
		}
		else
			step(0x6DAC, 12);
		const uint8_t v6DB0 = peekb(0x0FE6);
		fl::tstb(v6DB0);
		step(0x6DB0, 24);
		if (v6DB0) break;
		step(0x6D2A, 12);
	}
	step(0x6DB2, 12);
	pokeb(0x0FE6, 0);
	fl::clr();
	step(0x6DB6, 27);
	jsr(0x6DBA, 0x8D2C);                               // clear_playfield_ram
	R[0] = lw(0x001F);
	step(0x6DBE, 18);
	jsr(0x6DC2, 0x8AB6);
	poke(0x0184, lw(0x5CD2));
	step(0x6DC8, 33);
	R[0] = lw(0x005C);
	step(0x6DCC, 18);
	jsr(0x6DD0, 0x8AB6);
	ww(L(4), 0);
	fl::clr();
	step(0x6DD4, 30);
	wb(L(2), lb(peekb(0x01F4)));
	step(0x6DDA, 42);
	ww(L(2), lw(rw(L(2)) & 0x0003));
	step(0x6DE0, 36);
	// BIT (PC),2(SP): the word after the opcode (2) is the source
	if (!lw(rw(L(2)) & 0x0002))
	{
		step(0x6DE4, 33);
		step(0x6DE6, 12);
		R[0] = lw(1);
		step(0x6DEA, 18);
		ww(L(2), lw((uint16_t)(rw(L(2)) ^ R[0])));
		step(0x6DEE, 30);
	}
	else
	{
		step(0x6DE4, 33);
		step(0x6DEE, 12);
	}
	R[0] = 0;
	fl::clr();
	step(0x6DF0, 12);
	R[0] = lb(peekb(0x01F4));
	step(0x6DF4, 24);
	for (int i = 0; i < 5; i++)
	{
		R[0] = asr_w(R[0]);
		step((uint16_t)(0x6DF6 + 2 * i), 12);
	}
	ww(L(6), lw(R[0]));
	step(0x6E02, 30);
	ww(L(6), asr_w(rw(L(6))));
	step(0x6E06, 30);
	ww(L(6), asr_w(rw(L(6))));
	step(0x6E0A, 30);
	R[1] = lw(0x000C);
	step(0x6E0E, 18);
	for (;;)
	{
		R[0] = lw(R[1]);
		step(0x6E10, 12);
		R[0] = fl::add(R[0], 0x0050);
		step(0x6E14, 18);
		jsr(0x6E18, 0x8AB6);
		R[1] = fl::inc(R[1]);
		step(0x6E1A, 12);
		fl::cmp(R[1], 0x0015);
		step(0x6E1E, 18);
		if ((int16_t)R[1] > 0x15) break;
		step(0x6E0E, 12);
	}
	step(0x6E20, 12);
	for (;;)
	{
		const uint8_t v6E24 = peekb(0x0193);
		fl::tstb(v6E24);
		step(0x6E24, 24);
		if (!(v6E24 & 0x80)) break;
		step(0x6E26, 12);
		ww(L(0), fl::inc(rw(L(0))));
		step(0x6E28, 21);
		step(0x6E20, 12);
	}
	step(0x6E2A, 12);
	for (;;)
	{
		jsr(0x6E2E, 0x8D50);                           // wait_vblank
		jsr(0x6E32, 0x435A);
		ww(L(0), 0);
		fl::clr();
		step(0x6E34, 21);
		wb(L(0), lb(peekb(0x01F3)));
		step(0x6E38, 33);
		R[1] = 0;
		fl::clr();
		step(0x6E3A, 12);
		for (;;)
		{
			R[0] = lw(rw(L(0)));
			step(0x6E3C, 18);
			R[0] = lw(R[0] & 0x0003);
			step(0x6E40, 18);
			R[0] = fl::add(R[0], R[1]);
			step(0x6E42, 12);
			R[0] = fl::add(R[0], 0x0050);
			step(0x6E46, 18);
			jsr(0x6E4A, 0x8AB6);
			ww(L(0), asr_w(rw(L(0))));
			step(0x6E4C, 21);
			ww(L(0), asr_w(rw(L(0))));
			step(0x6E4E, 21);
			R[1] = fl::add(R[1], 4);
			step(0x6E52, 18);
			fl::cmp(R[1], 8);
			step(0x6E56, 18);
			if ((int16_t)R[1] > 8) break;
			step(0x6E3A, 12);
		}
		step(0x6E58, 12);
		jsr(0x6E5C, 0x8D86);
		R[0] = jsr(0x6E60, 0x8E2E);                    // rotary_step
		ww(L(8), lw(R[0]));
		step(0x6E64, 30);
		R[0] = lw(rw(L(4)));
		step(0x6E68, 27);
		R[0] = fl::add(R[0], rw(L(8)));
		step(0x6E6C, 27);
		ww(L(4), lw(R[0]));
		step(0x6E70, 30);
		ww(L(4), lw(rw(L(4)) & 0x0001));
		step(0x6E76, 36);
		const uint8_t pressed = lb(peekb(0x0193) & 0x40);
		step(0x6E7C, 30);
		if (pressed)
		{
			step(0x6E7E, 12);
			fl::tst(rw(L(4)));
			step(0x6E82, 27);
			if (!rw(L(4)))
			{
				step(0x6E84, 12);
				ww(L(2), fl::inc(rw(L(2))));
				step(0x6E88, 30);
				if ((int16_t)rw(L(2)) < 0)
				{
					step(0x6E8A, 12);
					ww(L(2), lw(3));
					step(0x6E90, 36);
					step(0x6EA8, 12);
				}
				else
				{
					step(0x6E92, 12);
					fl::cmp(rw(L(2)), 3);
					step(0x6E98, 33);
					if ((int16_t)rw(L(2)) > 3)
					{
						step(0x6E9A, 12);
						ww(L(2), 0);
						fl::clr();
						step(0x6E9E, 30);
						step(0x6EA8, 12);
					}
					else
						step(0x6EA8, 12);
				}
			}
			else
			{
				step(0x6EA0, 12);
				R[0] = lw(1);
				step(0x6EA4, 18);
				ww(L(6), lw((uint16_t)(rw(L(6)) ^ R[0])));
				step(0x6EA8, 30);
			}
			for (;;)
			{
				const uint8_t held = lb(peekb(0x0193) & 0x40);
				step(0x6EAE, 30);
				if (!held) break;
				step(0x6EA8, 12);
			}
			step(0x6EB0, 12);
		}
		else
			step(0x6EB0, 12);
		R[0] = lw(rw(L(2)));
		step(0x6EB4, 27);
		R[0] = fl::add(R[0], 0x0066);
		step(0x6EB8, 18);
		jsr(0x6EBC, 0x8AB6);
		fl::tst(rw(L(4)));
		step(0x6EC0, 27);
		if (!rw(L(4)))
		{
			step(0x6EC2, 12);
			R[0] = lw(0x003F);
			step(0x6EC6, 18);
			step(0x6ECC, 12);
		}
		else
		{
			step(0x6EC8, 12);
			R[0] = lw(0x0023);
			step(0x6ECC, 18);
		}
		jsr(0x6ED0, 0x8B52);                           // draw_large_char
		fl::tst(rw(L(6)));
		step(0x6ED4, 27);
		if (!rw(L(6)))
		{
			step(0x6ED6, 12);
			R[0] = lw(0x006A);
			step(0x6EDA, 18);
			step(0x6EE0, 12);
		}
		else
		{
			step(0x6EDC, 12);
			R[0] = lw(0x006B);
			step(0x6EE0, 18);
		}
		jsr(0x6EE4, 0x8AB6);
		fl::tst(rw(L(4)));
		step(0x6EE8, 27);
		if (rw(L(4)))
		{
			step(0x6EEA, 12);
			R[0] = lw(0x003F);
			step(0x6EEE, 18);
			step(0x6EF4, 12);
		}
		else
		{
			step(0x6EF0, 12);
			R[0] = lw(0x0023);
			step(0x6EF4, 18);
		}
		jsr(0x6EF8, 0x8B52);                           // draw_large_char
		const uint8_t v6EFC = peekb(0x0FE6);
		fl::tstb(v6EFC);
		step(0x6EFC, 24);
		if (v6EFC) break;
		step(0x6E2A, 12);
	}
	step(0x6EFE, 12);
	if (!lw(rw(L(2)) & 0x0002))                        // BIT (PC),2(SP)
	{
		step(0x6F02, 33);
		step(0x6F04, 12);
		R[0] = lw(1);
		step(0x6F08, 18);
		ww(L(2), lw((uint16_t)(rw(L(2)) ^ R[0])));
		step(0x6F0C, 30);
	}
	else
	{
		step(0x6F02, 33);
		step(0x6F0C, 12);
	}
	push(0x001B);
	fl::mov(0x001B);
	step(0x6F10, 30);
	jsr(0x6F14, 0x8EA2);                               // send_sound_command
	jsr(0x6F18, 0x8F50);
	R[0] = lw(rw(L(8)));
	step(0x6F1C, 27);
	for (int i = 0; i < 7; i++)
	{
		R[0] = asl_w(R[0]);
		step((uint16_t)(0x6F1E + 2 * i), 12);
	}
	ww(R[6], lw(R[0]));
	step(0x6F2C, 21);
	ww(R[6], lw((uint16_t)(rw(R[6]) | rw(L(4)))));
	step(0x6F30, 36);
	jsr(0x6F34, 0x8EA2);                               // send_sound_command
	jsr(0x6F38, 0x8F62);
	pokeb(0x0FE6, 0);
	fl::clr();
	step(0x6F3C, 27);
	poke(0x0184, lw(0x5C04));
	step(0x6F42, 33);
	R[6] = fl::add(R[6], 0x000C);
	step(0x6F46, 18);
}

// B6:69B2 (assembly, self test): the statistics screen: titles, the sound CPU's version
// (command 0x14, F:8F0C into 0x0370), five counters of the list at 0x0370 (F:BA9A), the
// third and fourth also kept (0(SP) and R2 = 2 x fifth + fourth), then three play times
// (F:68E2 prints a {minutes, seconds, frames} triple built on the stack) and two averages
// (B6:6938 divides a time by a count); until a button press (F:8D60).  Locals: 2(SP) the list
// pointer, 0(SP) a count, 4(SP) a result triple, 0xA(SP) the time triple.
void selftest_statistics_screen()
{
	uint16_t *R = T.r;
	auto L = [&](int off) { return (uint16_t)(R[6] + off); };
	R[6] = fl::sub(R[6], 0x0010);
	step(0x69B6, 18);
	R[0] = lw(0x3800);
	step(0x69BA, 18);
	poke(0x01D2, lw(R[0]));
	step(0x69BE, 27);
	poke(0x1400, lw(R[0]));
	step(0x69C2, 27);
	poke(0x0184, lw(0x5C04));
	step(0x69C8, 33);
	jsr(0x69CC, 0x8D2C);                               // clear_playfield_ram
	R[0] = lw(0x001F);
	step(0x69D0, 18);
	jsr(0x69D4, 0x8AB6);
	ww(L(2), lw(0x0370));
	step(0x69DA, 36);
	R[0] = lw(0x0030);
	step(0x69DE, 18);
	jsr(0x69E2, 0x8AB6);
	push(0x0014);
	fl::mov(0x0014);
	step(0x69E6, 30);
	jsr(0x69EA, 0x8EA2);                               // send_sound_command
	ww(R[6], 0x001A);
	fl::mov(0x001A);
	step(0x69EE, 27);
	push(0x0370);
	fl::mov(0x0370);
	step(0x69F2, 30);
	jsr(0x69F6, 0x8F0C);
	R[2] = 0;
	fl::clr();
	step(0x69F8, 12);
	R[1] = lw(1);
	step(0x69FC, 18);
	for (;;)
	{
		R[0] = lw(R[1]);
		step(0x69FE, 12);
		R[0] = fl::add(R[0], 0x0030);
		step(0x6A02, 18);
		jsr(0x6A06, 0x8AB6);
		ww(R[6], lw(peek(rw(L(6)))));
		step(0x6A0A, 42);
		push(0);
		fl::clr();
		step(0x6A0C, 24);
		push(5);
		fl::mov(5);
		step(0x6A10, 30);
		push(1);
		fl::mov(1);
		step(0x6A14, 30);
		{
			const uint16_t v = rw(R[6]);
			push(v);
			fl::mov(v);
		}
		step(0x6A16, 30);
		jsr(0x6A1A, 0xBA9A);
		fl::cmp(R[1], 3);
		step(0x6A1E, 18);
		if ((int16_t)R[1] >= 3)
		{
			step(0x6A20, 12);
			R[0] = lw(R[1]);
			step(0x6A22, 12);
			R[0] = fl::sub(R[0], 3);
			step(0x6A26, 18);
			R[0] = asl_w(R[0]);
			step(0x6A28, 12);
			// jump table at 0x6B78 for counters 3-5 (ADD to PC)
			f_nzvc(0);
			if (R[1] == 3)
			{
				step(0x6A2C, 27);
				ww(L(0x0C), lw(peek(rw(L(0x0E)))));
				step(0x6A32, 51);
				step(0x6A44, 12);
			}
			else if (R[1] == 4)
			{
				step(0x6A34, 27);
				R[2] = lw(peek(rw(L(0x0E))));
				step(0x6A38, 33);
				step(0x6A44, 12);
			}
			else
			{
				step(0x6A3A, 27);
				R[0] = lw(peek(rw(L(0x0E))));
				step(0x6A3E, 33);
				R[0] = asl_w(R[0]);
				step(0x6A40, 12);
				R[0] = fl::add(R[0], R[2]);
				step(0x6A42, 12);
				R[2] = lw(R[0]);
				step(0x6A44, 12);
			}
		}
		else
			step(0x6A44, 12);
		ww(L(0x0E), fl::add(rw(L(0x0E)), 2));
		step(0x6A4A, 36);
		R[6] = fl::add(R[6], 8);
		step(0x6A4E, 18);
		R[1] = fl::inc(R[1]);
		step(0x6A50, 12);
		fl::cmp(R[1], 5);
		step(0x6A54, 18);
		if ((int16_t)R[1] > 5) break;
		step(0x69FC, 12);
	}
	step(0x6A56, 12);
	// a time triple from the list into 0xE(SP) (+ the stack offset), the list pointer at p(SP)
	auto load_time = [&](uint16_t at, int p, int t) {
		ww(L(t), lw(peek(rw(L(p)))));
		step((uint16_t)(at + 6), 51);
		ww(L(p), fl::add(rw(L(p)), 2));
		step((uint16_t)(at + 12), 36);
		wb(L(t + 2), lb(peekb(rw(L(p)))));
		step((uint16_t)(at + 18), 51);
		wb(L(t + 3), 0);
		fl::clr();
		step((uint16_t)(at + 22), 30);
		R[0] = lw(peek(rw(L(p))));
		step((uint16_t)(at + 26), 33);
		R[0] &= 0xFF00;
		fl::clr();
		step((uint16_t)(at + 28), 12);
		R[0] = (uint16_t)((R[0] >> 8) | (R[0] << 8));
		f_nzvc(nzb((uint8_t)R[0]));
		step((uint16_t)(at + 30), 12);
		ww(L(t + 4), lw(R[0]));
		step((uint16_t)(at + 34), 30);
	};
	// (SP) = SP + off: the address of a stack triple
	auto address_arg = [&](uint16_t at, uint16_t off) {
		ww(R[6], lw(off));
		step((uint16_t)(at + 4), 27);
		ww(R[6], fl::add(rw(R[6]), R[6]));
		step((uint16_t)(at + 6), 21);
	};
	R[0] = lw(0x0036);
	step(0x6A5A, 18);
	jsr(0x6A5E, 0x8AB6);
	load_time(0x6A5E, 6, 0x0E);
	address_arg(0x6A80, 0x000E);
	jsr(0x6A8A, 0x68E2);                               // print_time
	ww(L(6), fl::add(rw(L(6)), 2));
	step(0x6A90, 36);
	R[0] = lw(0x0037);
	step(0x6A94, 18);
	jsr(0x6A98, 0x8AB6);
	load_time(0x6A98, 6, 0x0E);
	address_arg(0x6ABA, 0x000E);
	jsr(0x6AC4, 0x68E2);                               // print_time
	ww(L(6), fl::add(rw(L(6)), 2));
	step(0x6ACA, 36);
	load_time(0x6ACA, 6, 0x0E);
	address_arg(0x6AEC, 0x000E);
	push(R[2]);
	fl::mov(R[2]);
	step(0x6AF4, 24);
	push(0x000C);
	fl::mov(0x000C);
	step(0x6AF8, 30);
	ww(R[6], fl::add(rw(R[6]), R[6]));
	step(0x6AFA, 21);
	jsr(0x6AFE, 0x6938);                               // divide_time
	R[0] = lw(0x0039);
	step(0x6B02, 18);
	jsr(0x6B06, 0x8AB6);
	R[0] = lw(0x003A);
	step(0x6B0A, 18);
	jsr(0x6B0E, 0x8AB6);
	address_arg(0x6B0E, 0x000C);
	jsr(0x6B18, 0x68E2);                               // print_time
	ww(L(0x0A), fl::add(rw(L(0x0A)), 2));
	step(0x6B1E, 36);
	load_time(0x6B1E, 0x0A, 0x12);
	address_arg(0x6B40, 0x0012);
	push(lw(rw(L(8))));
	step(0x6B4A, 39);
	push(0x0010);
	fl::mov(0x0010);
	step(0x6B4E, 30);
	ww(R[6], fl::add(rw(R[6]), R[6]));
	step(0x6B50, 21);
	jsr(0x6B54, 0x6938);                               // divide_time
	R[0] = lw(0x003B);
	step(0x6B58, 18);
	jsr(0x6B5C, 0x8AB6);
	R[0] = lw(0x003C);
	step(0x6B60, 18);
	jsr(0x6B64, 0x8AB6);
	address_arg(0x6B64, 0x0010);
	jsr(0x6B6E, 0x68E2);                               // print_time
	jsr(0x6B72, 0x8D60);                               // wait_button_press
	R[6] = fl::add(R[6], 0x001C);
	step(0x6B76, 18);
}

}

LOGIC_TABLE(bankb,
	asm_routine("B1", 0x7854, "record_position_trail", with_rts<record_position_trail>, 0),
	c_routine("B1", 0x78E4, "start_trail_return", c_with_rts<start_trail_return>, 0, 0),
	asm_routine("B1", 0x792A, "trail_return_step", with_rts<trail_return_step>, 0),
	asm_routine("B1", 0x7AA2, "object4_step", with_rts<object4_step>, 0),
	asm_routine("B1", 0x7B3E, "start_object4", with_rts<start_object4>, 0),
	asm_routine("B3", 0x79D4, "draw_countdown", with_rts<draw_countdown>, 0),
	asm_routine("B3", 0x7A3A, "tick_countdown", with_rts<tick_countdown>, 0),
	c_routine("B3", 0x7DD6, "update_table_0fb4", c_with_rts<update_table_0fb4>, 0, 0),
	c_routine("B3", 0x7E44, "frame_delta_bonus", c_with_rts<frame_delta_bonus>, 0, 0),
	c_routine("B3", 0x7F52, "tick_game_timer", c_with_rts<tick_game_timer>, 0, 0),
	c_routine("B3", 0x739A, "draw_cash_meter", c_with_rts<draw_cash_meter>, 0, 0),
	asm_routine("B3", 0x77B4, "update_status_display", with_rts<update_status_display>, 0),
	asm_routine("B3", 0x78BE, "update_time_bar", with_rts<update_time_bar>, 0),
	asm_routine("B6", 0x7B82, "track_scroll_blocks", with_rts<track_scroll_blocks>, 0),
	asm_routine("B6", 0x7C84, "step_pause", with_rts<step_pause>, 0),
	c_routine("B6", 0x7CF6, "show_hint", c_with_rts<show_hint>, 0, 0),
	asm_routine("B6", 0x7E54, "hint_countdowns", with_rts<hint_countdowns>, 0),
	asm_routine("B6", 0x7F16, "show_ticket_hint", with_rts<show_ticket_hint>, 0),
	asm_routine("B8", 0x7DB8, "shift_rounded", with_rts<shift_rounded>, 0),
	asm_routine("B1", 0x7BBC, "animate_palette", with_rts<animate_palette>, 0),
	c_routine("B3", 0x7ADA, "score_landing", c_with_rts<score_landing>, 0, 0),
	c_routine("B3", 0x7446, "draw_status_line", c_with_rts<draw_status_line>, 0, 0),
	asm_routine("B6", 0x7942, "follow_skater_scroll", with_rts<follow_skater_scroll>, 0),
	asm_routine("B3", 0x65FE, "park_mode_step", with_rts<park_mode_step>, 0),
	c_routine("B3", 0x6A1C, "load_demo_park", c_with_rts<load_demo_park>, 0, 0),
	c_routine("B3", 0x7140, "choose_park_level", c_with_rts<choose_park_level>, 0, 0),
	c_routine("B3", 0x7E98, "score_landing_screen3", c_with_rts<score_landing_screen3>, 0, 0),
	c_routine("B3", 0x72CA, "add_tickets", c_with_rts<add_tickets>, 0, 0),
	wait_c_routine("B3", 0x6D40, "next_turn", c_with_rts<next_turn>, 0),
	wait_c_routine("B3", 0x654C, "start_game_town", c_with_rts<start_game_town>, 0),
	wait_c_routine("B3", 0x71DA, "park_start_screen", c_with_rts<park_start_screen>, 0),
	wait_routine("B6", 0x74E4, "selftest_sprite_screen", with_rts<selftest_sprite_screen>, 0),
	wait_routine("B6", 0x7388, "selftest_colour_bars_screen", with_rts<selftest_colour_bars_screen>, 0),
	wait_routine("B6", 0x713E, "selftest_playfield_screen", with_rts<selftest_playfield_screen>, 0),
	wait_routine("B6", 0x6B7E, "selftest_controls_screen", with_rts<selftest_controls_screen>, 0),
	wait_routine("B6", 0x6CFC, "selftest_switches_screen", with_rts<selftest_switches_screen>, 0),
	wait_routine("B6", 0x69B2, "selftest_statistics_screen", with_rts<selftest_statistics_screen>, 0),
)

}
