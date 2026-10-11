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
	cyc(30 + 12);
	const uint8_t st = peekb(0x031F);
	fl::cmpb(st, 5);
	if (st == 5) return;
	cyc(24 + 12);
	const uint8_t b = peekb(0x032B);
	fl::tstb(b);
	if (b) return;
	cyc(18 + 24 + 12);
	T.r[0] = lw(0x0007 & ~peek(0x0304));
	if (!T.r[0]) return;
	cyc(24 + 12 + 27 + 12);
	T.r[0] = fl::dec(sext8(peekb(0x0F12)));
	pokeb(0x0F12, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	if ((int8_t)(uint8_t)T.r[0] > 0) return;
	cyc(144);                                          // 7878-788E
	pokeb(0x0F12, 1);
	fl::movb(1);
	T.r[2] = sext8(peekb(0x0F13));
	T.r[0] = asl_w(T.r[2]);
	T.r[1] = fl::sub(peek(0x06F0), peek((uint16_t)(0x0EDA + T.r[0])));
	if ((int16_t)T.r[1] < 0)
	{
		cyc(12);
		T.r[1] = neg_w(T.r[1]);
	}
	cyc(18 + 12);
	fl::cmp(T.r[1], 0x0600);
	if ((int16_t)T.r[1] <= 0x0600)
	{
		cyc(24 + 27 + 12);
		T.r[1] = fl::sub(peek(0x0730), peek((uint16_t)(0x0EEA + T.r[0])));
		if ((int16_t)T.r[1] < 0)
		{
			cyc(12);
			T.r[1] = neg_w(T.r[1]);
		}
		cyc(12 + 18 + 12);
		T.r[0] = T.r[1];
		fl::cmp(T.r[0], 0x0600);
		if ((int16_t)T.r[0] <= 0x0600) return;
	}
	cyc(351);                                          // 78AC-78DC
	pokeb(0x0F14, (uint8_t)T.r[2]);
	T.r[0] = fl::inc(T.r[2]);
	T.r[1] = lw(T.r[0] & 0x0007);
	pokeb(0x0F13, 0);
	fl::clr();
	pokeb(0x0F13, lb((uint8_t)(peekb(0x0F13) | (uint8_t)T.r[1])));
	T.r[1] = sext8(peekb(0x0F13));
	T.r[0] = asl_w(T.r[1]);
	poke((uint16_t)(0x0EDA + T.r[0]), peek(0x06F0));
	poke((uint16_t)(0x0EEA + T.r[0]), peek(0x0730));
	poke((uint16_t)(0x0EFA + T.r[0]), peek(0x06B0));
	const uint8_t t = peekb(0x0327);
	pokeb((uint16_t)(0x0F0A + T.r[1]), t);
	fl::movb(t);
}

// B1:78E4: start putting the skater back: objects 0-3 are reset at his position (F:843E),
// state 0x031F = 5 with 30 frames of delay (0x02FA), the trail steps back one entry.
// Returns the previous entry index - 1.
uint16_t start_trail_return(const Args &)
{
	cyc(162);                                          // 78E4-78F2
	push(T.r[1]);                                      // MOV R1,-(SP)
	call(0x843E, {peek(0x06B0), peek(0x0730), peek(0x06F0)});
	T.r[1] = pop();
	poke(0x03B0, 0x50FA);
	poke(0x03B4, 0x50FA);
	pokeb(0x031F, 5);
	const uint16_t r0 = (uint16_t)(sext8(peekb(0x0F14)) - 1);
	pokeb(0x0F14, (uint8_t)(r0 & 7));
	poke(0x02FA, 0x001E);
	return r0;
}

// B1:792A (assembly): one frame of putting the skater back.  The delay 0x02FA counts down
// elsewhere; at 1 he is lifted (vz 0x05B0 = 0xC0).  While rising above the trail entry 0x0F14
// he is steered towards it (vx/vy +-0x100/0x80); once there he drops (vz = -0xC0) and on
// landing at the entry's height he is placed on it with its terrain.
void trail_return_step()
{
	cyc(24 + 12);
	T.r[0] = peek(0x02FA);
	fl::mov(T.r[0]);
	if (T.r[0])
	{
		cyc(18 + 12);
		fl::cmp(T.r[0], 1);
		if (T.r[0] != 1)
		{
			cyc(12);                                   // BNE at 7A06
			return;
		}
		cyc(228);                                      // 7936-7956
		poke(0x05B0, 0x00C0);
		poke(0x03B2, 0x57AC);
		T.r[0] = 1;
		pokeb(0x0791, 1);
		pokeb(0x0771, 1);
		push(2);
		push(2);
		T.r[0] = call(0xB602, {});
		cyc(33 + 27 + 24);
		poke(0x03B4, 0x50FA);
		poke(0x02FA, 0);
		const uint16_t s = pop(), d = pop();
		fl::cmp(s, d);
		return;
	}
	cyc(24 + 12);
	T.r[0] = peek(0x05B0);
	fl::mov(T.r[0]);
	if ((int16_t)T.r[0] > 0)
	{
		cyc(24 + 12 + 24 + 27 + 18 + 12);
		T.r[0] = asl_w(sext8(peekb(0x0F14)));
		T.r[1] = fl::sub(peek(0x06B0), peek((uint16_t)(0x0EFA + T.r[0])));
		T.r[1] = fl::sub(T.r[1], 0x0C00);
		if ((int16_t)T.r[1] < 0) return;
		cyc(27 + 42 + 39 + 18 + 12);
		poke(0x05B0, 0);
		poke(0x04F0, peek((uint16_t)(0x0EDA + T.r[0])));
		uint16_t d = fl::sub(peek(0x04F0), peek(0x06F0));
		poke(0x04F0, d);
		T.psw &= ~(CF | VF);
		cyc((int16_t)d <= 0 ? 33 : 33 + 12);
		poke(0x04F0, lw((int16_t)d <= 0 ? 0xFF00 : 0x0100));
		cyc(42 + 39 + 18 + 12 + 33);
		poke(0x0570, peek((uint16_t)(0x0EEA + T.r[0])));
		d = fl::sub(peek(0x0570), peek(0x0730));
		poke(0x0570, d);
		T.psw &= ~(CF | VF);
		poke(0x0570, lw((int16_t)d <= 0 ? 0xFF80 : 0x0080));
		return;
	}
	cyc(12);
	if (T.r[0] == 0)
	{
		cyc(24 + 12);
		T.r[2] = lw(peek(0x04F0));
		if (T.r[2])
		{
			cyc(24 + 12 + 27 + 24 + 12 + 12);
			T.r[0] = asl_w(sext8(peekb(0x0F14)));
			T.r[1] = fl::sub(peek((uint16_t)(0x0EDA + T.r[0])), peek(0x06F0));
			T.r[1] = lw(T.r[1] ^ T.r[2]);
			if ((int16_t)T.r[1] < 0)
			{
				cyc(27);
				poke(0x04F0, 0);
				fl::clr();
			}
		}
		cyc(24 + 12);
		T.r[2] = lw(peek(0x0570));
		if (T.r[2])
		{
			cyc(24 + 12 + 27 + 24 + 12 + 12);
			T.r[0] = asl_w(sext8(peekb(0x0F14)));
			T.r[1] = fl::sub(peek((uint16_t)(0x0EEA + T.r[0])), peek(0x0730));
			T.r[1] = lw(T.r[1] ^ T.r[2]);
			if ((int16_t)T.r[1] < 0)
			{
				cyc(27);
				poke(0x0570, 0);
				fl::clr();
			}
		}
		cyc(24 + 24 + 12);
		T.r[0] = lw((uint16_t)(peek(0x04F0) | peek(0x0570)));
		if (T.r[0]) return;
		cyc(33);
		poke(0x05B0, lw(0xFF40));
		return;
	}
	cyc(24 + 12 + 24 + 27 + 18 + 12);
	T.r[0] = asl_w(sext8(peekb(0x0F14)));
	T.r[1] = fl::sub(peek(0x06B0), peek((uint16_t)(0x0EFA + T.r[0])));
	T.psw &= ~(CF | VF);
	if ((int16_t)T.r[1] > 0) return;
	cyc(174);                                          // 7A22-7A32
	poke(0x05B0, 0);
	push(peek((uint16_t)(0x0EDA + T.r[0])));
	push(peek((uint16_t)(0x0EEA + T.r[0])));
	push(peek((uint16_t)(0x0EFA + T.r[0])));
	call(0x843E, {});
	cyc(144);                                          // 7A36-7A4A
	pokeb(0x032A, 0);
	T.r[0] = sext8(peekb(0x0F14));
	pokeb(0x0327, peekb((uint16_t)(0x0F0A + T.r[0])));
	pokeb(0x0F12, 0x1E);
	T.r[6] = fl::add(T.r[6], 6);
}

// ---------------------------------------------------------------------------------------------
// Object 4 (B1)

// B1:7AA2 (assembly): steps of object 4 by its state byte 0x07B4: state 3 starts animation
// 0x5772 + 2 * 0x0394 and moves to 4; in state 4 it stays while near the skater (screen
// distance), otherwise after the delay 0x0438 it moves to 5 (animation 0x5786); in later
// states it is removed (F:B56E) when the delay runs out
void object4_step()
{
	cyc(12 + 24 + 18 + 12);
	T.r[0] = peekb(0x07B4);
	fl::cmp(T.r[0], 3);
	if ((int16_t)T.r[0] < 3) return;
	cyc(12);
	if (T.r[0] == 3)
	{
		cyc(12 + 24 + 12 + 18 + 27 + 27 + 27 + 33);
		T.r[0] = (uint16_t)(peekb(0x0394) << 1);
		T.r[0] = fl::add(T.r[0], 0x5772);
		poke(0x03B8, T.r[0]);
		pokeb(0x0774, 0);
		fl::clr();
		pokeb(0x07B4, fl::incb(peekb(0x07B4)));
		pokeb(0x0494, lb((uint8_t)(peekb(0x0494) | 0x18)));
		return;
	}
	cyc(18 + 12);
	fl::cmp(T.r[0], 4);
	if (T.r[0] == 4)
	{
		cyc(24 + 24 + 12);
		T.r[0] = fl::sub(peek(0x0638), peek(0x0630));
		if ((int16_t)T.r[0] < 0)
		{
			cyc(12);
			T.r[0] = neg_w(T.r[0]);
		}
		cyc(18 + 12);
		fl::cmp(T.r[0], 0x0800);
		if ((int16_t)T.r[0] <= 0x0800)
		{
			cyc(24 + 24 + 12);
			T.r[0] = fl::sub(peek(0x0678), peek(0x0670));
			if ((int16_t)T.r[0] < 0)
			{
				cyc(12);
				T.r[0] = neg_w(T.r[0]);
			}
			cyc(18 + 18 + 12);
			T.r[0] = fl::sub(T.r[0], 0x0200);
			fl::cmp(T.r[0], 0x0A00);
			if (T.r[0] <= 0x0A00) return;
		}
		cyc(27 + 12);
		const uint16_t n = fl::dec(peek(0x0438));
		poke(0x0438, n);
		if (n) return;
		cyc(33 + 27 + 33 + 33);
		poke(0x03B8, 0x5786);
		pokeb(0x07B4, fl::incb(peekb(0x07B4)));
		pokeb(0x0494, lb((uint8_t)(peekb(0x0494) | 0x18)));
		poke(0x0438, lw(0x0003));
		return;
	}
	cyc(27 + 12);
	const uint16_t n = fl::dec(peek(0x0438));
	poke(0x0438, n);
	if (n) return;
	cyc(30 + 30);
	push(4);
	T.r[0] = call(0xB56E, {});
	cyc(18);
	fl::tst(pop());
}

// B1:7B3E (assembly)  start_object4(int tile): called on map tiles with bit 0x40; once object 1
// is high enough (height 0x06B2 >= 0x800) object 4 starts above it, offset by the tables at
// 0x7B2E/0x7B36 for (tile >> 3) & 3 (the argument slot is overwritten with that), in state 5
// with sound 0x7A
void start_object4()
{
	cyc(30 + 12);
	const uint16_t z = peek(0x06B2);
	fl::cmp(z, 0x0800);
	if ((int16_t)z < 0x0800) return;
	cyc(666);                                          // 7B46-7BAC
	pokeb(0x0374, 0x54);
	poke(0x03B8, 0x578A);
	poke(0x0438, 0x0017);
	T.r[0] = 4;
	pokeb(0x0794, 4);
	pokeb(0x0774, 4);
	const uint16_t arg = (uint16_t)(T.r[6] + 2);
	T.r[0] = asr_w(asr_w(asr_w(rw(arg))));
	ww(arg, T.r[0]);
	ww(arg, lw(rw(arg) & 0x0003));
	T.r[0] = asl_w(rw(arg));
	poke(0x06F8, peek((uint16_t)(0x7B2E + T.r[0])));
	poke(0x06F8, fl::add(peek(0x06F8), peek(0x06F0)));
	poke(0x0738, peek((uint16_t)(0x7B36 + T.r[0])));
	poke(0x0738, fl::add(peek(0x0738), peek(0x0730)));
	poke(0x06B8, peek(0x06B2));
	poke(0x06B8, fl::add(peek(0x06B8), 0x0600));
	pokeb(0x07B4, 5);
	push(4);
	call(0xB4B6, {});
	cyc(27 + 30);
	ww(T.r[6], 0x007A);
	T.r[0] = call(0x8E78, {});
	cyc(18);
	fl::tst(pop());
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
		step_rts();
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
	step_rts();
}

// B3:7A3A (assembly): count the countdown 0x0F1C down once a second (0x0F1E frames) and flag
// it for redrawing.  On screens 1 and 2 instead: every 16 frames, in mode 3 of 0x0F26 and
// unless the skater is being put back, 0x0F20 counts down and the time bar is flagged.
void tick_countdown()
{
	cyc(30 + 12);
	const uint8_t level = peekb(0x0F59);
	fl::cmpb(level, 1);
	bool other = level == 1;
	if (!other)
	{
		cyc(30 + 12);
		fl::cmpb(level, 2);
		other = level == 2;
	}
	if (other)
	{
		cyc(24 + 12);
		const uint16_t t = peek(0x0F20);
		fl::tst(t);
		if ((int16_t)t <= 0) return;
		cyc(30 + 12);
		if (lw(peek(0x01C4) & 0x000F)) return;
		cyc(30 + 12);
		const uint8_t m = peekb(0x0F26);
		fl::cmpb(m, 3);
		if (m != 3) return;
		cyc(30 + 12);
		const uint8_t st = peekb(0x031F);
		fl::cmpb(st, 5);
		if (st == 5) return;
		cyc(27 + 33);
		poke(0x0F20, fl::dec(peek(0x0F20)));
		pokeb(0x0FA2, lb((uint8_t)(peekb(0x0FA2) | 0x04)));
		return;
	}
	cyc(24 + 12);
	const uint8_t f = peekb(0x0F1E);
	T.r[0] = sext8(f);
	fl::movb(f);
	if (!f) return;
	cyc(12 + 12 + 27 + 12);
	T.r[1] = fl::dec(T.r[0]);
	pokeb(0x0F1E, (uint8_t)T.r[1]);
	fl::movb((uint8_t)T.r[1]);
	if ((uint8_t)T.r[1]) return;
	cyc(27 + 12);
	const uint16_t n = fl::dec(peek(0x0F1C));
	poke(0x0F1C, n);
	if (!n)
	{
		cyc(27 + 12);
		pokeb(0x0F1E, 0);
		fl::clr();
	}
	else
	{
		cyc(33);
		pokeb(0x0F1E, 0x3C);
		fl::movb(0x3C);
	}
	cyc(33);
	pokeb(0x0FA2, lb((uint8_t)(peekb(0x0FA2) | 0x10)));
}

// B3:7DD6  update_table_0fb4(int n): n = 0 sets the 32 bytes at 0x0FB4 to 3 and 0x0FE1 from
// the table at 0x4649 (bank A 0x2400) for 0x0F2E, returning 0x0F2E; otherwise n is added to
// each byte (at most 3) and -1 is returned.  The banks are restored.
uint16_t update_table_0fb4(const Args &a)
{
	const SavedRegs saved(3);
	cyc(IN_F144 + 27 + 12);
	const uint16_t n = a.u(0);
	if (n)
	{
		cyc(18);
		for (int i = 0x1F; i >= 0; i--)
		{
			const uint16_t p = (uint16_t)(0x0FB4 + i);
			uint16_t v = (uint16_t)(peekb(p) + n);
			cyc((int16_t)v > 3 ? 81 + 18 + 54 : 81 + 54);
			if ((int16_t)v > 3) v = 3;
			pokeb(p, (uint8_t)v);
		}
		return 0xFFFF;
	}
	cyc(24 + 24 + 18 + 32 * 60 + 282);
	const uint16_t bank_a = peek(0x01D2), bank_b = peek(0x01D4);
	for (int i = 0x1F; i >= 0; i--) pokeb((uint16_t)(0x0FB4 + i), 3);
	poke(0x01D2, 0x2400);
	poke(0x1400, 0x2400);
	const uint16_t r0 = peekb(0x0F2E);
	pokeb(0x0FE1, peekb((uint16_t)(0x4649 + r0)));
	poke(0x01D2, bank_a);
	poke(0x01D4, bank_b);
	poke(0x1400, peek(0x01D2));
	poke(0x1402, peek(0x01D4));
	return r0;
}

// B3:7E44: except on screen 4, the frames since 0x0FB0 (less 30, / 8) are converted by
// F:F064; a valid result n selects the amount at 0xFCFC + 2n, which is scored (F:EF24) on
// screens 0-2 and added to 0x0F34 (time bar flagged) on the others
uint16_t frame_delta_bonus(const Args &)
{
	cyc(24 + 30 + 12);
	push(T.r[1]);                                      // MOV R1,-(SP)
	struct Restore { ~Restore() { if (!std::uncaught_exceptions()) T.r[1] = pop(); } } restore;
	if (peekb(0x0F59) == 4) return T.r[0];
	cyc(180);                                          // 7E4E-7E66
	const uint16_t d = (uint16_t)(peek(0x01C4) - peek(0x0FB0) - 0x001E);
	const uint16_t r0 = call(0xF064, {(uint16_t)((int16_t)d >> 3)});
	cyc(18 + 12 + 12);
	if ((int16_t)r0 < 0) return r0;
	cyc(66);                                           // 7E70-7E7A
	const uint16_t amount = peek((uint16_t)(0xFCFC + (uint16_t)(r0 << 1)));
	if (peekb(0x0F59) < 3)
	{
		cyc(39 + 30);
		const uint16_t r = call(0xEF24, {amount});
		cyc(18 + 12);
		return r;
	}
	cyc(42 + 33);
	poke(0x0F34, (uint16_t)(peek(0x0F34) + amount));
	pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) | 0x04));
	return r0;
}

// B3:7F52: F:DD5C, then while neither 0x0F26 nor 0x0EA0 is set the game timer counts: every
// 0x0FA7 frames (counter 0x0FA6) one unit is taken off (F:F028 add_time(-1))
uint16_t tick_game_timer(const Args &)
{
	const SavedRegs saved(5);
	cyc(IN_F162 + 30);
	call(0xDD5C, {});
	cyc(24 + 24 + 18 + 12);
	const uint16_t r0 = (uint16_t)(peekb(0x0F26) | peekb(0x0EA0));
	if (r0) return r0;
	cyc(27 + 12);
	const uint8_t n = (uint8_t)(peekb(0x0FA6) - 1);
	pokeb(0x0FA6, n);
	if (n) return r0;
	cyc(39 + 30 + 30);
	pokeb(0x0FA6, peekb(0x0FA7));
	const uint16_t r = call(0xF028, {0xFFFF});
	cyc(18);
	return r;
}

// B3:739A  draw_cash_meter(int player): a meter of up to 4 segments for the player's cash
// (0x0256 for the current player 0x01F5, else 0x0294) against the thresholds at 0x523E, at
// the text position from the table 0x51E2, closed by a 2 x 2 end piece.  Returns the number
// of segments left unlit.
uint16_t draw_cash_meter(const Args &a)
{
	const SavedRegs saved(4);
	cyc(IN_F152 + 30);
	call(0x8068, {});                                  // select_video_bank0
	cyc(228);                                          // 73A2-73C6
	poke(0x0188, 0x8000);
	const uint16_t player = a.u(0);
	poke(0x0186, peek((uint16_t)(0x51E2 + (uint16_t)(player << 1))));
	poke(0x0186, (uint16_t)(peek(0x0186) + 0x2000));
	uint16_t r0 = 4;
	const bool current = player == sext8(peekb(0x01F5));
	cyc(current ? 24 + 12 : 24);
	const uint16_t cash = current ? peek(0x0256) : peek(0x0294);
	cyc(24 + 12 + 18);
	const uint16_t colour = peek(0x0188), lit = colour | 0x0087;
	for (;;)
	{
		cyc(12 + 12 + 27 + 12);
		if ((int16_t)cash >= (int16_t)peek((uint16_t)(0x523E + (uint16_t)(r0 << 1)))) break;
		cyc(12 + 12);
		if ((int16_t)r0 <= 0) break;
		cyc(192);                                      // 73EA-7404
		const uint16_t p = peek(0x0186);
		poke((uint16_t)(p + 0x82), lit);
		poke((uint16_t)(p + 0x80), lit);
		poke((uint16_t)(p + 2), lit);
		poke(p, lit);
		poke(0x0186, (uint16_t)(peek(0x0186) + 4));
		r0--;
	}
	cyc(18 + 12);
	if ((int16_t)r0 < 3)
	{
		cyc(108);                                      // 740C-7416
		const uint16_t p = peek(0x0186);
		poke((uint16_t)(p + 0x80), lit);
		poke(p, lit);
		poke(0x0186, (uint16_t)(peek(0x0186) + 2));
	}
	cyc(270);                                          // 741C-743E
	const uint16_t p = peek(0x0186);
	const uint16_t cells[4] = { p, (uint16_t)(p + 2), (uint16_t)(p + 0x80), (uint16_t)(p + 0x82) };
	for (int i = 0; i < 4; i++)
	{
		poke(cells[i], colour);
		poke(cells[i], (uint16_t)(peek(cells[i]) | (0x00FA + i)));
	}
	return r0;
}

// B3:77B4 (assembly): redraw the parts of the status line flagged in 0x0FA2 (bit 0: score
// 0x023E/0x0240, bit 1: high score 0x0244/0x0246 in the second colour unless 0x036C is set,
// bit 3: cash 0x0256 and its meter) unless 0x02BF is set; every 32 frames (0x01C2) 0x0FE2 is
// halved.  Two words of locals are reserved; the flags end from comparing them.
void update_status_display()
{
	cyc(30 + 24 + 12);
	T.r[6] = (uint16_t)(T.r[6] - 4);                   // CMP -(SP),-(SP)
	const uint8_t busy = peekb(0x02BF);
	if (!busy)
	{
		cyc(30);
		call(0x8068, {});                              // select_video_bank0
		cyc(30 + 12);
		if (peek(0x0FA2) & 0x0001)
		{
			cyc(27 + 24 + 12);
			pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) - 1));
			T.r[0] = sext8(peekb(0x01F5));
			cyc(T.r[0] ? 33 : 33 + 12);
			poke(0x0186, T.r[0] ? 0x2262 : 0x2204);
			cyc(246);                                  // 77E0-77FA
			T.r[0] = (uint16_t)(T.r[0] << 1);
			poke(0x0188, peek((uint16_t)(0x51DE + T.r[0])));
			push(peek(0x023E));
			push(peek(0x0240));
			push(6);
			push(1);
			push(1);
			T.r[0] = call(0xBA9A, {});
			cyc(18);
			T.r[6] = (uint16_t)(T.r[6] + 10);
		}
		cyc(30 + 12);
		bool high = (peek(0x0FA2) & 0x0002) != 0;
		if (high)
		{
			cyc(24 + 12);
			high = !peekb(0x036C);
		}
		if (high)
		{
			cyc(33 + 24 + 12);
			pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) & ~0x02));
			if (!peek(0x01FC))
			{
				cyc(30);
				T.r[0] = call(0xA8FE, {});
			}
			cyc(252);                                  // 7820-783E
			poke(0x0188, 0xA000);
			poke(0x0186, 0x3746);
			push(peek(0x0244));
			push(peek(0x0246));
			push(8);
			push(0);
			push(1);
			T.r[0] = call(0xBA96, {});
			cyc(84);                                   // 7842-784E
			pokeb(0x0EA6, (uint8_t)(peekb(0x0EA6) | 0x01));
			poke(0x0EAA, 0x00B4);
			T.r[6] = (uint16_t)(T.r[6] + 10);
		}
		cyc(30 + 12);
		if (peek(0x0FA2) & 0x0008)
		{
			cyc(390);                                  // 785A-788E
			pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) & ~0x08));
			poke(0x018A, 0x00A8);
			poke(0x0188, 0x8000);
			T.r[0] = (uint16_t)(sext8(peekb(0x01F5)) << 1);
			poke(0x0186, peek((uint16_t)(0x51E2 + T.r[0])));
			poke(0x0186, (uint16_t)(peek(0x0186) + 0x2000));
			push(peek(0x0256));
			push(0);
			push(6);
			push(1);
			push(1);
			T.r[0] = call(0xBA9A, {});
			cyc(24 + 21 + 30);
			T.r[0] = sext8(peekb(0x01F5));
			ww(T.r[6], T.r[0]);
			T.r[0] = call(0x739A, {});                 // draw_cash_meter
			cyc(18);
			T.r[6] = (uint16_t)(T.r[6] + 10);
		}
		cyc(33 + 30 + 12);
		poke(0x018A, 0x0001);
		if (!(peek(0x01C2) & 0x001F))
		{
			cyc(12 + 24 + 12 + 27);
			T.r[0] = (uint16_t)(peekb(0x0FE2) >> 1);
			pokeb(0x0FE2, (uint8_t)T.r[0]);
		}
	}
	fl::cmp(rw(T.r[6]), rw((uint16_t)(T.r[6] + 2)));
	T.r[6] = (uint16_t)(T.r[6] + 4);
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
			fl::cmpb(peekb(0x027A), 2);
			step(0x78FC, 30);
			if (peekb(0x027A) == 2)
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
	fl::cmp(peek(0x09A4), 0x4000);
	step(0x7B96, 30);
	if (peek(0x09A4) >= 0x4000)
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
	fl::cmp(peek(0x09A6), 0x4000);
	step(0x7BF0, 30);
	if (peek(0x09A6) < 0x4000)
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
	fl::cmp(peek(0x01FC), 0x0078);
	step(0x7C94, 30);
	if ((int16_t)peek(0x01FC) > 0x0078)
	{
		step(0x7CF4, 12);
		return;
	}
	step(0x7C96, 12);
	uint8_t b = peekb(0x0FE6);
	fl::tstb(b);
	step(0x7C9A, 24);
	bool tested = false;                               // at 7CB2 with the SUB's flags
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
		fl::tst(peek(0x01FC));
		step(0x7CB2, 24);
	}
	if (!peek(0x01FC))
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
	const SavedRegs saved(3);
	cyc(IN_F144 + 24 + 12);
	if (peekb(0x0F26))
	{
		cyc(30 + 12);
		if (peekb(0x0273) != 4) return T.r[0];
	}
	cyc(33 + 24 + 12);
	poke(0x0184, 0x5CD2);
	auto done = [&]() {                                // 7E16
		cyc(33);
		poke(0x0184, 0x5AC8);
		return T.r[0];
	};
	if (!peek(0x01FE))
	{
		cyc(12);
		return done();
	}
	cyc(27 + 12);
	poke(0x01FE, (uint16_t)(peek(0x01FE) - 1));
	if (peek(0x01FE)) return done();
	cyc(30 + 12);
	bool check = peekb(0x0336) == 0xFF;
	if (!check)
	{
		cyc(39 + 12);
		check = (peek(peek(0x0348)) & 0x0001) != 0;
	}
	bool busy = true;                                  // a message is showing
	if (check)
	{
		cyc(24 + 12 + 27 + 12);
		T.r[0] = (uint16_t)(sext8(peekb(0x0358)) << 1);
		busy = peek((uint16_t)(0x0362 + T.r[0])) != 0;
	}
	if (busy)
	{
		cyc(27 + 12);
		pokeb(0x0273, 0);
		return done();
	}
	cyc(33 + 12 + 24 + 12 + 12);
	poke(0x01FC, 0xFF4C);
	T.r[0] = peekb(0x0273);
	if (T.r[0])
	{
		// jump table at 0x7E1E for hints 1-5 (B6:7E28 sets no others; larger values would jump
		// outside the table in the original)
		static const uint16_t hint_text[5] = { 0x7860, 0x7872, 0x7886, 0x786A, 0x787C };
		if (T.r[0] > 5) return done();
		cyc(12 + 12 + 27 + (T.r[0] == 5 ? 30 : 30 + 12) + 30 + 30 + 30);
		call(0xA974, {0x0001, 0x00C8, hint_text[T.r[0] - 1]});
		cyc(12 + 24 + 42 + 27 + 12 + 18);
		T.r[0] = peekb(0x0273);
		pokeb(0x0272, (uint8_t)(peekb(0x0272) | peekb((uint16_t)(0xFC73 + T.r[0]))));
		pokeb(0x0273, 0);
		return done();
	}
	uint16_t text;
	cyc(24 + 12);
	if (!peekb(0x0F31))
	{
		cyc(30 + 12);
		text = 0x7818;
	}
	else
	{
		cyc(24 + 12);
		if (!peekb(0x025B))
		{
			cyc(30 + 12);
			text = 0x7822;
		}
		else
		{
			cyc(30);
			T.r[0] = call(0x9152, {});                 // next_random
			cyc(12 + 12 + 12 + 24 + 27 + 18 + 12 + 27 + 24 + 12);
			T.r[2] = T.r[0];
			T.r[1] = 0;
			T.r[0] = peekb((uint16_t)(0xFE7C + peekb(0x0F31)));
			T.r[3] = peekb((uint16_t)(0xFE70 + T.r[0]));
			if ((int16_t)peek(0x0256) > (int16_t)T.r[3])
			{
				cyc(18);
				for (T.r[3] = 4; T.r[3]; T.r[3]--)
				{
					cyc(12 + 12 + 12 + 18 + 27 + 12);
					T.r[0] = (uint16_t)(T.r[2] + 1);
					T.r[2] = T.r[0] & 0x0003;
					if (!peekb((uint16_t)(0x025C + T.r[2])))
					{
						cyc(12 + 12);
						if (!T.r[1])
						{
							cyc(24 + 36 + 27 + 30 + 24 + 30);
							T.r[0] = call(0xA974, {0x0000, 0x00C8, (uint16_t)(0x7830 + peekb((uint16_t)(0x7C80 + T.r[2])))});
							cyc(18 + 18);
							T.r[1] = 1;
						}
					}
					cyc(18);                           // SOB
				}
			}
			cyc(12 + 12);
			if (T.r[1]) return done();
			cyc(30);
			text = 0x7858;
		}
	}
	cyc(30 + 24 + 30);
	T.r[0] = call(0xA974, {0x0000, 0x00C8, text});
	cyc(18);
	return done();
}

// B6:7E54 (assembly)  hint_countdowns(int event): count down the hint delays (B6:7E28) while
// no message, game state or requested hint is pending.  event 0 in Skate City: hint 0 while
// in the air (0x0FB2) except in moves 0x12/0x0B, else hint 1 between 10 and 30 frames after
// 0x0FAC without equipment (0x025F); event 1: hint 3, then hint 2 (no 0x025D, Skate City);
// both then hint 4 in Skate City.  Hints already given are marked in 0x0272.
void hint_countdowns()
{
	auto count = [](uint16_t hint) {                   // L7F0E with the hint pushed
		cyc(30);
		push(hint);
		T.r[0] = call(0x7E28, {});
		cyc(18);
		fl::tst(pop());
	};
	cyc(24 + 12);
	uint8_t b = peekb(0x018E);
	fl::tstb(b);
	if (b) return;
	cyc(24 + 12);
	b = peekb(0x018F);
	fl::tstb(b);
	if (b) return;
	cyc(24 + 12);
	b = peekb(0x0273);
	fl::tstb(b);
	if (b) return;
	cyc(27 + 12);
	T.r[0] = rw((uint16_t)(T.r[6] + 2));
	fl::mov(T.r[0]);
	bool zero_case = false;
	if (!T.r[0])
	{
		cyc(24 + 12);
		b = peekb(0x0F59);
		fl::tstb(b);
		zero_case = !b;
	}
	if (zero_case)
	{
		cyc(24 + 12);
		const uint16_t air = peek(0x0FB2);
		fl::tst(air);
		if ((int16_t)air > 0)
		{
			cyc(30 + 12);
			b = peekb(0x032A);
			fl::cmpb(b, 0x12);
			if (b != 0x12)
			{
				cyc(30 + 12);
				fl::cmpb(b, 0x0B);
				if (b != 0x0B)
				{
					cyc(30 + 12);
					if (!lb(peekb(0x0272) & 0x01))
					{
						cyc(24 + 12);
						count(0);
						return;
					}
				}
			}
		}
		cyc(30 + 12);
		if (lb(peekb(0x0272) & 0x02)) return;
		cyc(24 + 12);
		b = peekb(0x025F);
		fl::tstb(b);
		if (b) return;
		cyc(24 + 24 + 18 + 12);
		T.r[0] = fl::sub(peek(0x01C4), peek(0x0FAC));
		fl::cmp(T.r[0], 0x001E);
		if ((int16_t)T.r[0] >= 0x001E) return;
		cyc(24 + 24 + 18 + 12);
		T.r[0] = fl::sub(peek(0x01C4), peek(0x0FAC));
		fl::cmp(T.r[0], 0x000A);
		if ((int16_t)T.r[0] <= 0x000A) return;
		cyc(30 + 12);
		count(1);
		return;
	}
	cyc(18 + 12);
	fl::cmp(T.r[0], 1);
	if (T.r[0] == 1)
	{
		cyc(30 + 30);
		push(3);
		T.r[0] = call(0x7E28, {});
		cyc(24 + 12);
		if (!peekb(0x025D))
		{
			cyc(30 + 12);
			if (!(peekb(0x0272) & 0x04))
			{
				cyc(24 + 12);
				if (!peekb(0x0F59))
				{
					cyc(24 + 12);
					if (!peekb(0x0273))
					{
						cyc(27 + 30);
						ww(T.r[6], 2);
						T.r[0] = call(0x7E28, {});
					}
				}
			}
		}
		cyc(18);
		fl::tst(pop());
	}
	cyc(24 + 12);
	b = peekb(0x0F59);
	fl::tstb(b);
	if (b) return;
	cyc(30 + 12);
	if (lb(peekb(0x0272) & 0x10)) return;
	cyc(24 + 12);
	b = peekb(0x0273);
	fl::tstb(b);
	if (b) return;
	cyc(30);
	count(4);
}

// B6:7F16 (assembly): in game state 1, show the ticket hint once: 0x7822 while there are no
// tickets (0x025B), 0x7890 when there are (F:A974, 200 frames, pause 180); 0x0279 bits 0/1
// record them.  Text table 0x0184 is 0x5CD2 meanwhile and 0x5AC8 afterwards.
void show_ticket_hint()
{
	cyc(30 + 12);
	const uint8_t st = peekb(0x018F);
	fl::cmpb(st, 1);
	if (st != 1) return;
	cyc(33 + 30 + 12);
	poke(0x0184, 0x5CD2);
	uint16_t text = 0;
	uint8_t bit = 0;
	const bool first_shown = lb(peekb(0x0279) & 0x01) != 0;
	cyc(24 + 12);
	const uint8_t tickets = peekb(0x025B);
	fl::tstb(tickets);
	if (!first_shown && !tickets)
	{
		text = 0x7822;
		bit = 0x01;
	}
	else if (tickets)
	{
		cyc(30 + 12);
		if (!lb(peekb(0x0279) & 0x02))
		{
			text = 0x7890;
			bit = 0x02;
		}
	}
	if (text)
	{
		cyc(30 + 30 + 30 + 30);
		push(text);
		push(0x00C8);
		push(0x0001);
		T.r[0] = call(0xA974, {});
		cyc(bit == 0x01 ? 33 + 33 + 12 + 18 : 33 + 33 + 18);
		poke(0x01FC, 0xFF4C);
		pokeb(0x0279, (uint8_t)(peekb(0x0279) | bit));
		T.r[6] = fl::add(T.r[6], 6);
	}
	cyc(33);
	poke(0x0184, lw(0x5AC8));
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
	cyc(op == CLC || op == BIC || op == ADD ? 18 : 12);
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
	cyc(18 + 18 + 12);
	T.r[1] = fl::add(T.r[1], 0x0010);
	if (lw(T.r[1] & 0xFFE0))
	{
		cyc(12);
		T.r[0] = 0;
		fl::clr();
		return;
	}
	const uint16_t i = T.r[1];
	cyc(12 + 27);
	T.r[1] = asl_w(T.r[1]);
	for (const ShiftStep &s : seq[i])
	{
		if (s.op == END) break;
		shift_op(s.op, s.imm);
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
	cyc(24 + 12 + 18 + 18 + 12);
	T.r[3] = peek(0x01C4);
	T.r[1] = lw(T.r[3] & 0x0007);
	fl::cmp(T.r[1], 5);
	if ((int16_t)T.r[1] > 5)
	{
		cyc(12 + 12);                                  // BNE at 7BE6 and 7CCA
		return;
	}
	cyc(12 * 6 + 27);
	T.r[0] = asr_w(asr_w(asr_w(T.r[3])));
	T.r[2] = asl_w(T.r[1]);
	f_nzvc(0);                                         // jump table at 0x7D76: ADD to PC, no carry
	switch (T.r[1])
	{
	case 0:
	{
		cyc(24 + 12);
		const uint8_t level = peekb(0x025C);
		T.r[2] = sext8(level);
		fl::movb(level);
		if (!level)
		{
			cyc(12);
			return;
		}
		cyc(18 + 12);
		if (lw(T.r[3] & 0x000F))
		{
			cyc(12);
			return;
		}
		cyc(12 + 12 + 12 + 18 + 12 + 12 + 12 + 42);
		T.r[3] = asr_w(T.r[0]);
		T.r[1] = lw(T.r[3] & 0x0001);
		T.r[2] = asl_w(T.r[2]);
		T.r[2] = fl::add(T.r[2], T.r[1]);
		T.r[2] = asl_w(T.r[2]);
		const uint16_t v = peek((uint16_t)(0x7A64 + T.r[2]));
		poke(0x1038, v);
		fl::mov(v);
		return;
	}
	case 1:
	{
		cyc(24 + 12);
		const uint8_t level = peekb(0x025F);
		T.r[2] = sext8(level);
		fl::movb(level);
		if (!level)
		{
			cyc(12);
			return;
		}
		cyc(12 + 18 + 12 + 12 + 12 + 42 + 42);
		T.r[1] = lw(T.r[0] & 0x0001);
		T.r[2] = asl_w(T.r[2]);
		T.r[2] = fl::add(T.r[2], T.r[1]);
		T.r[2] = asl_w(T.r[2]);
		poke(0x1016, peek((uint16_t)(0x7A4C + T.r[2])));
		const uint16_t v = peek((uint16_t)(0x7A58 + T.r[2]));
		poke(0x101C, v);
		fl::mov(v);
		return;
	}
	case 2:
	{
		cyc(24 + 12);
		const uint8_t level = peekb(0x025E);
		T.r[2] = sext8(level);
		fl::movb(level);
		if (!level) return;
		cyc(12 + 18 + 12 + 12 + 12 + 42);
		T.r[1] = lw(T.r[0] & 0x0001);
		T.r[2] = asl_w(T.r[2]);
		T.r[2] = fl::add(T.r[2], T.r[1]);
		T.r[2] = asl_w(T.r[2]);
		const uint16_t v = peek((uint16_t)(0x7A70 + T.r[2]));
		poke(0x1004, v);
		fl::mov(v);
		return;
	}
	case 3:
	{
		cyc(24 + 12);
		const uint8_t level = peekb(0x0F59);
		fl::tstb(level);
		if (level) return;
		cyc(30 + 12);
		const uint16_t t = peek(0x0254);
		fl::cmp(t, 0x0008);
		if ((int16_t)t >= 8)
		{
			cyc(27);
			poke(0x1094, 0);
			fl::clr();
			return;
		}
		cyc(30 + 12 + 96);
		const uint8_t st = peekb(0x018F);
		fl::cmpb(st, 1);
		uint16_t v;
		if (st == 1)
		{
			T.r[1] = lw(T.r[0] & 0x0003);
			T.r[2] = asl_w(T.r[1]);
			v = peek((uint16_t)(0x7A84 + T.r[2]));
			poke(0x109C, v);
		}
		else
		{
			T.r[1] = lw(T.r[0] & 0x0001);
			T.r[2] = asl_w(T.r[1]);
			v = peek((uint16_t)(0x7A80 + T.r[2]));
			poke(0x1094, v);
		}
		fl::mov(v);
		return;
	}
	case 4:
	{
		cyc(12 + 24 + 12 + 12);
		T.r[2] = peekb(0x0F59);
		fl::tst(T.r[2]);
		if (T.r[2])
		{
			cyc(18 + 12);
			fl::cmp(T.r[2], 7);
			if (T.r[2] != 7) return;
			cyc(12 + 18 + 12 + 12 + 42 + 42);
			T.r[1] = lw(T.r[0] & 0x0001);
			T.r[2] = asl_w(T.r[1]);
			poke(0x1148, peek((uint16_t)(0x7A96 + T.r[2])));
			const uint16_t v = peek((uint16_t)(0x7A9A + T.r[2]));
			poke(0x114C, v);
			fl::mov(v);
			return;
		}
		cyc(402);                                      // 7C86-7CC0
		T.r[1] = lw(T.r[0] & 0x0003);
		T.r[2] = asl_w(T.r[1]);
		T.r[2] = peek((uint16_t)(0x7A8C + T.r[2]));
		for (uint16_t a = 0x1108; a <= 0x1188; a += 0x20) poke(a, T.r[2]);
		T.r[2] = asl_w(T.r[1]);
		T.r[0] = peek((uint16_t)(0x7A8E + T.r[2]));
		for (uint16_t a = 0x110C; a <= 0x118C; a += 0x20) poke(a, T.r[0]);
		fl::mov(T.r[0]);
		return;
	}
	default:
	{
		cyc(180);                                      // 7CE4-7D00
		T.r[4] = peek(0x01D2);
		T.r[5] = peek(0x01D4);
		T.r[2] = 0x2000;
		poke(0x01D2, T.r[2]);
		poke(0x1400, T.r[2]);
		T.r[3] = peekb(0x0EA6);
		fl::tst(T.r[3]);
		if (T.r[3])
		{
			cyc(33 + 18 + 12);
			poke(0x0EAA, fl::sub(peek(0x0EAA), 0x0008));
			T.psw &= ~(CF | VF);
			if ((int16_t)peek(0x0EAA) <= 0)
			{
				cyc(183);                              // 7D0C-7D26
				pokeb(0x0EA6, 0);
				fl::clr();
				T.r[0] = asl_w(asl_w(asl_w(sext8(peekb(0x01F5)))));
				poke((uint16_t)(0x1086 + T.r[0]), peek((uint16_t)(0x5EC6 + T.r[0])));
				poke(0x10AA, peek(0x5EEA));
			}
			else
			{
				cyc(12 + 18 + 18 + 12);
				T.r[1] = lw(T.r[0] & 0x0001);
				if (lw(T.r[3] & 0x0001))
				{
					cyc(12 + 12 + 42);
					T.r[2] = asl_w(T.r[1]);
					poke(0x10AA, peek((uint16_t)(0x7A9A + T.r[2])));
				}
				cyc(18 + 12);
				if (lw(T.r[3] & 0x0002))
				{
					cyc(24 + 12 + 12 + 12 + 12 + 12);
					T.r[0] = asl_w(asl_w(asl_w(sext8(peekb(0x01F5)))));
					fl::tst(T.r[1]);
					if (!T.r[1])
					{
						cyc(27 + 12);
						T.r[1] = peek((uint16_t)(0x5EC6 + T.r[0]));
					}
					else
					{
						cyc(18);
						T.r[1] = 0xFF0F;
					}
					cyc(30);
					poke((uint16_t)(0x1086 + T.r[0]), T.r[1]);
				}
			}
		}
		cyc(27 + 27 + 39 + 39);
		poke(0x01D2, T.r[4]);
		poke(0x01D4, T.r[5]);
		poke(0x1400, peek(0x01D2));
		const uint16_t v = peek(0x01D4);
		poke(0x1402, v);
		fl::mov(v);
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
// Crashes and the moves 0x12, 0x0B, 0x13 score nothing (-1).
uint16_t score_landing(const Args &)
{
	const SavedRegs saved(5);
	cyc(IN_F162 + 18 + 30 + 42 + 33 + 12);
	const uint16_t level = peekb(0x0F59);
	if (level == 4)
	{
		cyc(24 + 12);
		if (peekb(0x01F2))
		{
			cyc(18);                                   // JMP 7DD0
			return T.r[0];
		}
	}
	cyc(243);                                          // 7AFE-7B22
	const uint16_t bank_a = peek(0x01D2), bank_b = peek(0x01D4);
	pokeb(0x01F2, 1);
	pokeb(0x0FE0, (uint8_t)(peekb(0x0FE0) + 1));
	poke(0x01D2, 0x2400);
	poke(0x1400, 0x2400);
	uint16_t turn = peek(0x02F4);
	if ((int16_t)turn < 0)
	{
		cyc(12);
		turn = (uint16_t)-turn;
	}
	cyc(24 + 27 + 30 + 30);
	uint16_t r0 = call(0x90A6, {0x0048, (uint16_t)(turn + 0x0024)});
	cyc(18 + 12);
	if ((int16_t)r0 > 15)
	{
		cyc(18);
		r0 = 15;
	}
	cyc(12 + 24 + 24 + 18 + 12);
	uint16_t r3 = r0;
	uint16_t r5 = (uint16_t)(peek(0x01C4) - peek(0x0FAC));
	if ((int16_t)r5 > 0x38)
	{
		cyc(18 + 12);
		r3 = 0xFFFF;
	}
	else
	{
		cyc(18 + 12);
		if ((int16_t)r5 > 0x28)
		{
			cyc(18);
			r3 = (uint16_t)(r3 + 0x10);
		}
	}
	cyc(18 + 12 * 3 + 18 + 12);
	r5 = (uint16_t)((int16_t)(uint16_t)(r5 + 4) >> 3);
	if ((int16_t)r5 > 15)
	{
		cyc(18);
		r5 = 15;
	}
	cyc(156);                                          // 7B72-7B8A
	uint16_t r1 = sext8((uint8_t)((uint16_t)(peek(0x0FA8) - peek(0x06B0)) >> 8));
	uint16_t r2 = (uint16_t)((int16_t)r1 >> 1);
	r1 = 0;
	if ((int16_t)peek(0x01B4) < 0)
	{
		cyc(33 + 12);
		if (level == 2)
		{
			cyc(12);
			r1++;
		}
	}
	cyc(24 + 12);
	if ((int16_t)peek(0x0FAA) < 0)
	{
		cyc(114);                                      // 7B9C-7BA8
		r1 = (uint16_t)(r1 + peekb((uint16_t)(level + 0x451C)));
	}
	const uint8_t move = peekb(0x032A);
	bool none;
	cyc(30 + 12);
	if (peekb(0x031F) == 1)
		none = true;
	else
	{
		cyc(30 + 12);
		if (move == 0x12)
			none = true;
		else
		{
			cyc(30 + 12);
			if (move == 0x0B)
				none = true;
			else
			{
				cyc(30);
				none = move == 0x13;
			}
		}
	}
	cyc(12);                                           // BNE at 7BC8
	if (none) cyc(18);                                 // JMP 7D88
	if (!none)
	{
		cyc(30 + 30 + 33 + 12);
		const uint16_t half = (uint16_t)((int16_t)r0 >> 1);
		bool done = false;
		if (level == 4)
		{
			cyc(18 + 12);
			if ((int16_t)r2 <= 12)
			{
				cyc(12 + 12);
				r2 = 0;
			}
			else
			{
				cyc(18 + 12);
				if ((int16_t)r2 <= 20)
				{
					cyc(18 + 12);
					r2 = 8;
				}
				else
				{
					cyc(18);
					r2 = 16;
				}
			}
			cyc(195);                                  // 7BF8-7C12
			r0 = (uint16_t)(r2 + half);
			r1 = (uint16_t)(r1 + peekb((uint16_t)(0x4521 + r0)));
			r2 = (uint16_t)(r2 << 1);
			r0 = (uint16_t)(r5 + r2);
			uint16_t r4 = peekb((uint16_t)(0x4539 + r0));
			r0 = call(0x90BA, {});
			cyc(39 + 12);
			if (peekb(0x0FE0) == peekb((uint16_t)(0x4570 + r0)))
			{
				cyc(12);
				r4 = (uint16_t)(r4 << 1);
			}
			cyc(12 + 24 + 12);
			r1 = (uint16_t)(r1 + r4);
			if (peek(0x0FB2))
			{
				cyc(18);
				r1 = (uint16_t)(r1 + 2);
			}
			cyc(18 + 12);
			if ((int16_t)r1 > 14)
			{
				cyc(30);
				r0 = call(0x90BA, {});
				cyc(39 + 12);
				if (peekb(0x0FE0) != peekb((uint16_t)(0x4570 + r0)))
				{
					cyc(18);
					r1 = 14;
				}
			}
			cyc(24 + 24 + 12 + 18 + 12);
			r0 = sext8(peekb(0x01F8));
			if (!((peek(0x0FAE) ^ r0) & 0x0007))
			{
				cyc(18);
				r1 = (uint16_t)(r1 + 2);
			}
			cyc(30 + 12);
			if (peekb(0x032A) == 0x0F)
			{
				cyc(18 + 33);
				r1 = (uint16_t)(r1 + 4);
				pokeb(0x0EA9, 0x3C);
			}
			cyc(27 + 12 + 12);
			r1 = sext8(peekb((uint16_t)(0x4584 + r1)));
			done = true;
		}
		else
		{
			cyc(12 + 27 + 33 + 12);
			uint16_t r4 = peekb((uint16_t)(0x45A1 + r5));
			if (level == 2)
			{
				cyc(12);
				r4 = (uint16_t)((int16_t)r4 >> 1);
			}
			cyc(12 + 12 + 27 + 27 + 12 + 30 + 12);
			r1 = (uint16_t)(r1 + r4);
			r0 = half;
			r1 = (uint16_t)(r1 + peekb((uint16_t)(0x4599 + r0)));
			if (peekb(0x01EA) == 2)
			{
				cyc(18);
				r1 = (uint16_t)(r1 + 2);
			}
			// the original also looks up 0x45B1 by the air time 0x0FB2 here; the value is unused
			cyc(24 + 18 + 36 + 18 + 12);
			if ((int16_t)(uint16_t)((int16_t)(uint16_t)(peek(0x0FB2) + 4) >> 3) > 15) cyc(18);
			cyc(27 + 18 + 33 + 12);
			if (level != 2)
			{
				cyc(12 + 12);
				if ((int16_t)r2 < 0)
				{
					cyc(12);
					r2 = (uint16_t)-r2;
				}
				cyc(12 + 18 + 12);
				if ((int16_t)r2 > 15) cyc(18);
				cyc(27 + 18 + 12);
				r4 = (int16_t)r2 > 15 ? 15 : r2;
				r1 = (uint16_t)(r1 + peekb((uint16_t)(0x45C1 + r4)));
			}
			cyc(201);                                  // 7CDC-7CFC
			r0 = (uint16_t)((peekb(0x0FE2) >> 3) + r1);
			r1 = r0;
			r0 = (uint16_t)(peekb(0x0FE2) + r1);
			pokeb(0x0FE2, (uint8_t)r0);
			if ((uint8_t)r0 > 0x80)
			{
				cyc(33);
				pokeb(0x0FE2, 0x80);
			}
			cyc(12 + 12 + 12);
			uint16_t bonus = 0;
			if ((int16_t)r3 < 0)
			{
				cyc(27 + 30);
				call(0x7DD6, {0x0002});                // update_table_0fb4
				cyc(30 + 12);
				if (peekb(0x0327) == 0x18)
				{
					cyc(18);
					bonus = 0x28;
				}
				cyc(12 + 12 + 27 + 12);
				r0 = (uint16_t)(bonus + r1);
				r1 = sext8(peekb((uint16_t)(0x45D1 + r0)));
				done = true;
			}
			else
			{
				cyc(18 + 18 + 12);
				const uint16_t counter = (uint16_t)(r3 + 0x0FB4);
				if (peekb(counter))
				{
					cyc(30 + 12);
					if ((int16_t)peek(0x0FB2) >= 5)
					{
						cyc(27 + 12);
						r1 = sext8(peekb((uint16_t)(0x45D1 + r1)));
					}
					else
					{
						cyc(24 + 12);
						const uint8_t c = peekb(counter);
						if (c <= 1)
						{
							cyc(18 + 12);
							bonus = 0x50;
						}
						else
						{
							cyc(24 + 12);
							if (c != 3)
							{
								cyc(18);
								bonus = 0x28;
							}
						}
						cyc(30 + 12);
						if (peekb(0x0327) == 0x18)
						{
							cyc(18 + 12);
							if ((int16_t)bonus < 0x50)
							{
								cyc(18);
								bonus = (uint16_t)(bonus + 0x28);
							}
						}
						cyc(87);                       // 7D60-7D6C
						r0 = (uint16_t)(bonus + r1);
						r1 = sext8(peekb((uint16_t)(0x45D1 + r0)));
						if (peekb(counter) == 3)
						{
							cyc(12 + 12);
							if ((int16_t)r1 >= 0)
							{
								cyc(27 + 12);
								const uint8_t n = (uint8_t)(peekb(0x0FE1) - 1);
								pokeb(0x0FE1, n);
								if (!n)
								{
									cyc(21 + 30);
									call(0x7DD6, {0x0000});    // update_table_0fb4
								}
							}
						}
						cyc(21 + 12);
						pokeb(counter, (uint8_t)(peekb(counter) - 1));
					}
					done = true;
				}
			}
		}
		none = !done;
	}
	if (none)
	{
		cyc(18);
		r1 = 0xFFFF;
	}
	cyc(21 + 30);
	r0 = call(0xF064, {r1});
	cyc(12 + 12);
	if ((int16_t)r0 >= 0)
	{
		cyc(12 + 30 + 12);
		const uint16_t amount = peek((uint16_t)(0xFCFC + (uint16_t)(r0 << 1)));
		if (peekb(0x0F59) < 3)
		{
			cyc(36 + 30);
			r0 = call(0xEF24, {amount});
			cyc(12);
		}
		else
		{
			cyc(42 + 33);
			poke(0x0F34, (uint16_t)(peek(0x0F34) + amount));
			pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) | 0x04));
		}
	}
	cyc(42 + 42 + 39 + 39 + 24);
	poke(0x01D2, bank_a);
	poke(0x01D4, bank_b);
	poke(0x1400, peek(0x01D2));
	poke(0x1402, peek(0x01D4));
	return r0;
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
	fl::cmpb(peekb(0x0F26), 1);
	step(0x7450, 30);
	if (peekb(0x0F26) == 1)
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
	fl::tstb(peekb(0x018F));
	step(0x748E, 24);
	if (peekb(0x018F))
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
			fl::tst(peek(0x0254));
			step(0x74AE, 24);
			if ((int16_t)peek(0x0254) > 0)
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
	fl::tstb(peekb(0x01F6));
	step(0x7552, 24);
	if (peekb(0x01F6))
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
		fl::tstb(peekb(0x01F5));
		step(0x75B0, 24);
		if (peekb(0x01F5))
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
	fl::cmpb(peekb(0x0F26), 1);
	step(0x7614, 30);
	if (peekb(0x0F26) == 1)
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
		fl::tstb(peekb(0x018F));
		step(0x761E, 24);
		if (peekb(0x018F))
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
		fl::tstb(peekb(0x01F6));
		step(0x76D0, 24);
		if (peekb(0x01F6))
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
		fl::tstb(peekb(0x0F59));
		step(0x7786, 24);
		if (!peekb(0x0F59))
		{
			step(0x7788, 12);
			fl::cmpb(peekb(0x0F2B), 1);
			step(0x778E, 30);
			if (peekb(0x0F2B) == 1)
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
	cyc(39 + 24 + 33 + 24 + 27 + 12);
	poke(0x0218, peek(0x0570));
	push(0);
	wb(T.r[6], peekb(0x0F59));
	push(0);
	const uint16_t level = rw((uint16_t)(T.r[6] + 2));
	if (level)
	{
		cyc(21 + 39);
		ww(T.r[6], (uint16_t)(rw(T.r[6]) + 1));
		poke(0x0218, (uint16_t)(peek(0x0218) + peek(0x05B2)));
	}
	cyc(288);                                          // 795E-7988
	T.r[0] = (uint16_t)(peek(0x06F0) - peek(0x01AC) + peek(0x04F0));
	poke(0x0210, T.r[0]);
	T.r[0] = (uint16_t)(peek(0x0730) + peek(0x01AE) + peek(0x0218));
	poke(0x0212, T.r[0]);
	T.r[0] = peek(0x06B2);
	T.r[1] = (uint16_t)(level << 1);
	// jump table at 0x7F86 (screens 0-7)
	switch (level)
	{
	case 1:
		cyc(27 + 12);
		poke(0x0212, (uint16_t)(peek(0x0212) + T.r[0]));
		break;
	case 2: case 4:
		cyc(12 + 24 + 27 + 33);
		T.r[1] = (uint16_t)(T.r[0] + peek(0x0212));
		poke(0x0212, T.r[1]);
		poke(0x0212, (uint16_t)(peek(0x0212) - 0x1800));
		break;
	default:
		cyc(18 + 12);
		if ((int16_t)T.r[0] > 0x1000)
		{
			cyc(12 + 24 + 27 + 33 + 12);
			T.r[1] = (uint16_t)(T.r[0] + peek(0x0212));
			poke(0x0212, T.r[1]);
			poke(0x0212, (uint16_t)(peek(0x0212) - 0x1000));
		}
		break;
	}
	cyc(27 + 27 + 24 + 12 + 24 + 12 + 12 + 18 + 12);
	poke(0x0214, 0);
	poke(0x0216, 0);
	T.r[5] = peek(0x0210);
	T.r[2] = T.r[5];
	T.r[4] = peek(0x0212);
	T.r[0] = (uint16_t)((int16_t)T.r[4] >> 1);
	if ((int16_t)T.r[4] < 0x4400)
	{
		cyc(12 + 12 + 18 + 18 + 12);
		T.r[1] = (uint16_t)(T.r[2] - T.r[0] - 0x3000);
		if ((int16_t)T.r[1] > 0)
		{
			cyc(24 + 30 + 30);
			T.r[0] = call(0x90A6, {0xFFFB, T.r[1]});
			cyc(27 + 12 + 12 + 27 + 12 + 24);
			poke(0x0216, T.r[0]);
			T.r[0] = (uint16_t)-(uint16_t)(T.r[0] << 1);
			poke(0x0214, T.r[0]);
		}
		else
		{
			cyc(12 + 12 + 18 + 12 + 18 + 12);
			T.r[3] = (uint16_t)(T.r[0] + T.r[2]);
			T.r[1] = (uint16_t)(0x4600 - T.r[3]);
			if ((int16_t)T.r[1] > 0)
			{
				cyc(24 + 30 + 30);
				T.r[0] = call(0x90A6, {0xFFFB, T.r[1]});
				cyc(27 + 27 + 27 + 24);
				poke(0x0216, T.r[0]);
				poke(0x0214, T.r[0]);
				poke(0x0214, (uint16_t)(peek(0x0214) << 1));
			}
		}
		cyc(12 + 18 + 24 + 12);
		T.r[1] = (uint16_t)(T.r[4] - 0x3800);
		if ((int16_t)T.r[1] < (int16_t)peek(0x0216))
		{
			cyc(12 + 27);
			poke(0x0216, T.r[1]);
		}
	}
	else
	{
		cyc(12 + 12 + 12 + 18 + 18 + 12);
		T.r[3] = (uint16_t)(T.r[0] - T.r[2]);
		T.r[1] = (uint16_t)(T.r[3] + 0x0200);
		if ((int16_t)T.r[1] > 0)
		{
			cyc(24 + 30 + 30);
			T.r[0] = call(0x90A6, {0x0005, T.r[1]});
			cyc(27 + 12 + 12 + 27 + 12 + 24);
			poke(0x0216, T.r[0]);
			T.r[0] = (uint16_t)-(uint16_t)(T.r[0] << 1);
			poke(0x0214, T.r[0]);
		}
		else
		{
			cyc(12 + 12 + 18 + 18 + 12);
			T.r[0] = (uint16_t)(T.r[0] + T.r[2]);
			T.r[1] = (uint16_t)(T.r[0] - 0x7400);
			if ((int16_t)T.r[1] > 0)
			{
				cyc(24 + 30 + 30);
				T.r[0] = call(0x90A6, {0x0005, T.r[1]});
				cyc(27 + 27 + 27 + 24);
				poke(0x0216, T.r[0]);
				poke(0x0214, T.r[0]);
				poke(0x0214, (uint16_t)(peek(0x0214) << 1));
			}
		}
		cyc(12 + 18 + 24 + 12);
		T.r[1] = (uint16_t)(T.r[4] - 0x5000);
		if ((int16_t)T.r[1] > (int16_t)peek(0x0216))
		{
			cyc(27);
			poke(0x0216, T.r[1]);
		}
	}
	cyc(12 + 18 + 12);
	T.r[1] = (uint16_t)(T.r[5] - 0x2600);
	if ((int16_t)T.r[1] < 0)
	{
		cyc(24 + 12);
		if ((int16_t)T.r[1] < (int16_t)peek(0x0214))
		{
			cyc(27);
			poke(0x0214, T.r[1]);
		}
	}
	cyc(12 + 18 + 18 + 12);
	T.r[1] = (uint16_t)(T.r[5] - 0x5000);
	if ((int16_t)T.r[1] > 0)
	{
		cyc(24 + 12);
		if ((int16_t)T.r[1] > (int16_t)peek(0x0214))
		{
			cyc(27);
			poke(0x0214, T.r[1]);
		}
	}
	// step the scroll speed towards the target: R2 = |target - speed| when the speed is
	// nonzero, does not oppose the target and the difference opposes the speed
	auto approach = [](uint16_t speed, uint16_t target, uint16_t step) {
		cyc(12 + 24 + 12);
		T.r[2] = 0;
		T.r[3] = peek(speed);
		if (T.r[3])
		{
			cyc(24 + 12);
			T.r[4] = peek(target);
			bool same = true;
			if (T.r[4])
			{
				cyc(12 + 12 + 12);
				T.r[5] = T.r[4] ^ T.r[3];
				same = (int16_t)T.r[5] >= 0;
			}
			if (same)
			{
				cyc(12 + 24 + 12 + 12 + 12 + 12);
				T.r[2] = 0;
				T.r[1] = (uint16_t)(peek(target) - T.r[3]);
				T.r[4] = T.r[3] ^ T.r[1];
				if ((int16_t)T.r[4] < 0)
				{
					cyc(12 + 18 + 12 + 12);
					T.r[2] = T.r[1];
					T.r[1] = step;
					if ((int16_t)T.r[2] < 0)
					{
						cyc(12 + 12);
						T.r[2] = (uint16_t)-T.r[2];
						T.r[1] = (uint16_t)-T.r[1];
					}
				}
			}
		}
		cyc(18 + 12);
		if ((int16_t)T.r[2] >= 0x20)
		{
			cyc(27 + 12);
			poke(speed, (uint16_t)(peek(speed) + T.r[1]));
		}
		else
		{
			cyc(39);
			poke(speed, peek(target));
		}
	};
	cyc(27 + 12);
	if (!level)
	{
		approach(0x01B0, 0x0214, 0x0008);
		approach(0x01B2, 0x0216, 0x0004);
	}
	else
	{
		cyc(39 + 39);
		poke(0x01B0, peek(0x0214));
		poke(0x01B2, peek(0x0216));
	}
	cyc(24 + 12);
	bool raw = (rw(T.r[6]) & 0x0001) != 0;
	if (raw)
	{
		cyc(30 + 12);
		raw = (peekb(0x0327) & 0x07) != 0;
	}
	if (raw)
	{
		cyc(39 + 39 + 12);
		poke(0x01AC, (uint16_t)(peek(0x01AC) + peek(0x01B0)));
		poke(0x01AE, (uint16_t)(peek(0x01AE) - peek(0x01B2)));
	}
	else
	{
		cyc(198);                                      // 7B5A-7B7A
		T.r[0] = (uint16_t)(((peek(0x01B0) + 8) & ~0x000F) + peek(0x01AC));
		poke(0x01AC, T.r[0]);
		T.r[0] = (uint16_t)((peek(0x01B2) + 8) & ~0x000F);
		poke(0x01AE, (uint16_t)(peek(0x01AE) - T.r[0]));
	}
	fl::cmp(rw(T.r[6]), rw((uint16_t)(T.r[6] + 2)));
	T.r[6] = (uint16_t)(T.r[6] + 4);
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
	cyc(12 + 24 + 12 + 27);
	T.r[0] = (uint16_t)(peekb(0x0F26) << 1);
	f_nzvc(0);                                         // ADD to PC: a positive address, no carry
	auto restart_park = [&]() {                        // L679A: after the delay 0x0F22
		cyc(30);
		call(0x820C, {});                              // install_interrupts_b
		cyc(30 + 30);
		push(0x0053);
		call(0x8E78, {});                              // queue_sound
	};
	switch (peekb(0x0F26))
	{
	case 1:
	{
		cyc(30);
		call(0xE106, {});
		cyc(27 + 12 + 24 + 18 + 12);
		pokeb(0x018F, 0);
		T.r[1] = peekb(0x0F2B);
		bool first = T.r[1] == 4;
		if (!first)
		{
			cyc(24 + 12);
			first = !peekb(0x01F6);
		}
		if (first)
		{
			cyc(12 + 24 + 42 + 30);
			T.r[0] = peekb(0x027A);
			pokeb(0x0F29, (uint8_t)(peekb(0x0F29) | peekb((uint16_t)(0xFC74 + T.r[0]))));
			call(0x820C, {});
			cyc(30 + 30);
			push(0x0042);
			call(0x8E78, {});
			cyc(30);
			call(0x8FD0, {});
			cyc(30);
			call(0x6A6E, {});
			cyc(12 + 12 + 12 + 18 + 18);
			fl::tst(pop());
			return;
		}
		cyc(12 + 12);
		if (T.r[1] == 0)
		{
			cyc(30);
			call(0x820C, {});
			cyc(30);
			call(0x9182, {});
			cyc(30);
			call(0xE730, {});
			cyc(27 + 18 + 33);
			poke(0x0F22, 0);
			pokeb(0x0F26, 7);
			f_nzvc(0);
			return;
		}
		cyc(18 + 12);
		if (T.r[1] == 1)
		{
			cyc(12 + 24 + 42 + 30);
			T.r[0] = peekb(0x027A);
			pokeb(0x0F29, (uint8_t)(peekb(0x0F29) | peekb((uint16_t)(0xFC74 + T.r[0]))));
			call(0x820C, {});
			cyc(30);
			call(0x9182, {});
			cyc(30);
			call(0xE730, {});
			cyc(30);
			call(0x6E92, {});
			cyc(33 + 12 + 30);
			pokeb(0x0F2B, 2);
			call(0x6A6E, {});
			return;
		}
		cyc(18 + 12);
		if (T.r[1] == 3)
		{
			fl::cmp(T.r[1], 3);
			cyc(30);
			call(0x6A6E, {});
			return;
		}
		cyc(18 + 12);
		fl::cmp(T.r[1], 5);
		if (T.r[1] != 5)
		{
			cyc(12);                                   // BEQ at 6796
			return;
		}
		cyc(30);
		call(0x820C, {});
		cyc(30);
		call(0x9182, {});
		cyc(30);
		call(0x8FD0, {});
		cyc(30);
		call(0xD0F4, {});
		cyc(30);
		call(0xE730, {});
		cyc(27 + 33);
		pokeb(0x0F26, 0);
		fl::clr();
		pokeb(0x018F, 7);
		fl::movb(7);
		return;
	}
	case 2:
	{
		cyc(24 + 12);
		bool goal = peekb(0x0F27) != 0;
		if (goal)
		{
			cyc(24 + 12);
			goal = !peek(0x0180);
		}
		if (goal)
		{
			cyc(30 + 12);
			if (peekb(0x027A) >= 2)
			{
				cyc(33);
				pokeb(0x0F1E, 0x3C);
			}
			cyc(33 + 30);
			pokeb(0x0F26, 3);
			call(0xA8FE, {});
			cyc(12 + 24 + 24 + 36 + 30);
			T.r[0] = peekb(0x0F59);
			push(peekb((uint16_t)(0xFE6B + T.r[0])));
			call(0x8E78, {});                          // queue_sound
			cyc(12 + 12 + 18 + 18);
			fl::tst(pop());
			return;
		}
		cyc(24 + 12);
		const uint16_t delay = peek(0x0F22);
		fl::tst(delay);
		if (delay)
		{
			cyc(12);                                   // BEQ at 6796
			return;
		}
		cyc(30 + 30 + 30 + 30);
		push(0x766C);
		push(0x00B4);
		push(0x0001);
		call(0xA974, {});
		cyc(27 + 30);
		ww(T.r[6], 0xFFFB);
		T.r[0] = call(0x65D6, {});
		cyc(12 + 12);
		if ((int16_t)T.r[0] <= 0)
		{
			cyc(27 + 27 + 33 + 12);
			poke(0x0F1C, 0);
			poke(0x0F20, 0);
			pokeb(0x0F26, 5);
		}
		else
		{
			cyc(33);
			poke(0x0F22, 0x012C);
		}
		cyc(30 + 12);
		if (peekb(0x027A) < 2)
		{
			cyc(33 + 30);
			pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) | 0x04));
			call(0x78BE, {});                          // update_time_bar
			cyc(12);
		}
		else
		{
			cyc(33 + 30);
			pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) | 0x10));
			call(0x79D4, {});                          // draw_countdown
		}
		cyc(18 + 18);
		T.r[6] = fl::add(T.r[6], 6);
		return;
	}
	case 3:
	{
		cyc(24 + 12);
		if (!peekb(0x0F28))
		{
			cyc(24 + 30);
			T.r[0] = call(0x65D6, {0x0000});
			cyc(18 + 12 + 12);
			fl::tst(T.r[0]);
			if (T.r[0])
			{
				cyc(12);                               // BEQ at 6796
				return;
			}
			cyc(24 + 12);
		}
		if (peekb(0x0F28))
		{
			cyc(27 + 27 + 33 + 12);
			pokeb(0x0F1E, 0);
			poke(0x0F1C, 0);
			poke(0x0F22, 0x0014);
		}
		else
		{
			cyc(30 + 12);
			const bool low = peekb(0x027A) < 2;
			cyc(low ? 30 : 30 + 12);
			push(low ? 0x005D : 0x005E);
			cyc(30);
			T.r[0] = call(0x8E78, {});                 // queue_sound
			cyc(18);
			fl::tst(pop());
		}
		cyc(33);
		pokeb(0x0F26, 4);
		fl::movb(4);
		return;
	}
	case 4:
	{
		cyc(24 + 12);
		const uint16_t delay = peek(0x0F22);
		fl::tst(delay);
		if (delay) return;
		restart_park();
		cyc(30 + 12);
		const bool low = peekb(0x027A) < 2;
		cyc(low ? 39 + 12 : 39);
		poke(0x0258, low ? peek(0x0F20) : peek(0x0F34));
		cyc(30);
		call(0x6E38, {});
		cyc(12 + 24 + 27 + 18 + 12);
		T.r[0] = peekb((uint16_t)(0x0262 + peekb(0x0F31)));
		if (!T.r[0])
		{
			cyc(33 + 12 + 18 + 18);
			pokeb(0x0F26, 5);
			fl::tst(pop());
			return;
		}
		cyc(201);                                      // 67D8-67F2
		T.r[1] = (uint16_t)((int16_t)T.r[0] >> 3);
		T.r[0] = (uint16_t)(((int16_t)T.r[1] >> 3) & 0x0003);
		ww(T.r[6], peekb((uint16_t)(0xFE8C + T.r[0])));
		T.r[0] = call(0x90BA, {});
		cyc(24 + 30);
		push(T.r[0]);
		T.r[0] = call(0x80D8, {});                     // mul16
		cyc(123);                                      // 67FC-6808
		T.r[1] = T.r[0];
		ww(T.r[6], 0x7676);
		push(0x00C0);
		push(0x0000);
		T.r[0] = call(0xA974, {});
		cyc(228);                                      // 680C-682C
		poke(0x0256, (uint16_t)(peek(0x0256) + T.r[1]));
		T.r[1] = peekb(0x0F31);
		T.r[2] = (uint16_t)(peekb(0x027A) << 4);
		T.r[0] = peekb((uint16_t)(0x0262 + T.r[1]));
		T.r[2] |= T.r[0];
		T.r[0] = call(0x90BA, {});
		cyc(30 + 30 + 24 + 12);
		pokeb((uint16_t)(0x0262 + T.r[1]), (uint8_t)T.r[2]);
		pokeb((uint16_t)(0x0262 + T.r[1]), (uint8_t)(peekb((uint16_t)(0x0262 + T.r[1])) | (uint8_t)T.r[0]));
		bool next = !peekb(0x01F6);
		if (!next)
		{
			cyc(30 + 12);
			next = peekb(0x0F2B) > 2;
		}
		if (next)
		{
			cyc(27);
			pokeb(0x0F31, (uint8_t)(peekb(0x0F31) + 1));
		}
		cyc(33 + 33 + 18 + 18 + 18);
		pokeb(0x0F26, 6);
		poke(0x0F22, 0x00C0);
		T.r[6] = (uint16_t)(T.r[6] + 6);
		fl::tst(pop());
		return;
	}
	case 5:
	{
		restart_park();
		cyc(12 + 24 + 12 + 24 + 48 + 30);
		T.r[1] = peekb(0x0F31);
		T.r[2] = (uint16_t)(peekb(0x027A) << 4);
		T.r[0] = call(0x90BA, {});
		cyc(30 + 30 + 24 + 12);
		pokeb((uint16_t)(0x0262 + T.r[1]), (uint8_t)T.r[2]);
		pokeb((uint16_t)(0x0262 + T.r[1]), (uint8_t)(peekb((uint16_t)(0x0262 + T.r[1])) | (uint8_t)T.r[0]));
		bool next = !peekb(0x01F6);
		if (!next)
		{
			cyc(30 + 12);
			next = peekb(0x0F2B) > 2;
		}
		if (next)
		{
			cyc(27);
			pokeb(0x0F31, (uint8_t)(peekb(0x0F31) + 1));
		}
		cyc(30);
		call(0xA8FE, {});
		cyc(27 + 30 + 24 + 30);
		ww(T.r[6], 0x7696);
		push(0x00DC);
		push(0x0000);
		T.r[0] = call(0xA974, {});
		cyc(33 + 33 + 12 + 18);
		poke(0x0F22, 0x0068);
		pokeb(0x0F26, 6);
		T.r[6] = fl::add(T.r[6], 6);
		return;
	}
	case 6:
	{
		cyc(30 + 12);
		if (peek(0x0F22) == 1)
		{
			cyc(30 + 12);
			if (peekb(0x0F59) < 3)
			{
				cyc(24 + 12);
				if ((int16_t)peek(0x0F20) <= 0)
				{
					cyc(30);
					T.r[0] = call(0xA8FE, {});
					return;
				}
				cyc(30 + 12);
				push(0x7682);
			}
			else
			{
				cyc(30);
				push(0x768C);
			}
			cyc(30 + 30 + 30);
			push(0x7FFF);
			push(0x0001);
			T.r[0] = call(0xA974, {});
			cyc(18);
			T.r[6] = fl::add(T.r[6], 6);
			return;
		}
		cyc(24 + 12);
		const uint16_t delay = peek(0x0F22);
		fl::tst(delay);
		if (delay)
		{
			cyc(12);                                   // BNE at 697A
			return;
		}
		cyc(30 + 12);
		if (peekb(0x0F59) < 3)
		{
			cyc(24 + 12);
			if ((int16_t)peek(0x0F20) <= 0)
			{
				cyc(33 + 12 + 33);
				poke(0x0F22, 0x0060);
				pokeb(0x0F26, 7);
				f_nzvc(0);                             // C cleared by the TST of 0x0F20
				return;
			}
			cyc(24 + 12);
			const bool half = peekb(0x0E95) != 0;
			cyc(half ? 30 : 30 + 12);
			push(half ? 0x0032 : 0x0064);
			cyc(30);
			call(0xEF24, {});
			cyc(30);
			call(0x77B4, {});                          // update_status_display
			cyc(27 + 12);
			poke(0x0F20, (uint16_t)(peek(0x0F20) - 1));
		}
		else
		{
			cyc(24 + 12);
			if ((int16_t)peek(0x0F34) <= 0)
			{
				cyc(33 + 33);
				poke(0x0F22, 0x0040);
				pokeb(0x0F26, 7);
				f_nzvc(0);                             // C cleared by the TST of 0x0F34
				return;
			}
			cyc(33 + 30 + 30);
			poke(0x0F34, (uint16_t)(peek(0x0F34) - 0x0020));
			push(0x0020);
			call(0xEF24, {});
			cyc(30 + 12);
			if ((int16_t)peek(0x0F34) < 0x0040)
			{
				cyc(33 + 30);
				ww(T.r[6], peek(0x0F34));
				call(0xEF24, {});
				cyc(27);
				poke(0x0F34, 0);
			}
			cyc(30);
			call(0x77B4, {});                          // update_status_display
		}
		cyc(33 + 30);
		pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) | 0x04));
		T.r[0] = call(0x78BE, {});                     // update_time_bar
		cyc(18);
		fl::tst(pop());
		return;
	}
	case 7:
	{
		cyc(24 + 12);
		const uint16_t delay = peek(0x0F22);
		fl::tst(delay);
		if (delay) return;
		cyc(24 + 12);
		bool kind2 = false;
		if (peekb(0x01F6))
		{
			cyc(30 + 12);
			kind2 = peekb(0x0F2B) == 2;
		}
		cyc(kind2 ? 18 + 12 : 18);
		T.r[0] = kind2 ? 0x000E : 0x000F;
		cyc(12 + 24 + 12 + 12);
		T.r[1] = peekb(0x0F31);
		fl::cmp(T.r[1], T.r[0]);
		if ((int16_t)T.r[1] > (int16_t)T.r[0])
		{
			cyc(12 + 24 + 36 + 33 + 27);
			T.r[0] = peekb(0x01F5);
			pokeb((uint16_t)(0x0F3A + T.r[0]), 1);
			pokeb(0x018F, 5);
			pokeb(0x0F26, 0);
			fl::clr();
			return;
		}
		cyc(30);
		call(0x820C, {});
		cyc(30 + 12);
		if (peekb(0x0F2B) == 2)
		{
			cyc(30);
			call(0xE730, {});
		}
		cyc(30);
		call(0x6E92, {});
		cyc(33);
		pokeb(0x0F26, 8);
		fl::movb(8);
		return;
	}
	case 8:
	{
		// one player, game kind 4 or 0, or kind 3 (the turn passes): B3:6D40; kind 2 moves
		// on to kind 3 in mode 1; other kinds wait
		cyc(24 + 12);
		const uint8_t players = peekb(0x01F6);
		fl::tstb(players);
		if (players)
		{
			cyc(30 + 12);
			const uint8_t kind = peekb(0x0F2B);
			fl::cmpb(kind, 4);
			if (kind != 4)
			{
				cyc(12 + 24 + 12 + 12);
				T.r[1] = kind;
				fl::tst(T.r[1]);
				if (T.r[1])
				{
					cyc(18 + 12);
					fl::cmp(T.r[1], 2);
					if (T.r[1] == 2)
					{
						cyc(33 + 33);
						pokeb(0x0F26, 1);
						pokeb(0x0F2B, 3);
						fl::movb(3);
						return;
					}
					cyc(18 + 12);
					fl::cmp(T.r[1], 3);
					if (T.r[1] != 3) return;
				}
			}
		}
		cyc(30);
		call(0x6D40, {});
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
	const SavedRegs saved(3);
	cyc(IN_F144 + 279);                                // 6A20-6A42
	const uint16_t park = a.u(0);
	const uint16_t b = peekb((uint16_t)(0x5666 + park));
	const uint16_t c = peekb((uint16_t)(0x566A + park));
	poke(0x01D2, 0x1800);
	poke(0x1400, 0x1800);
	push(0);
	push(b);
	push(c);
	call(0x4000, {});
	cyc(18 + 27 + 27 + 21 + 36 + 30);
	poke(0x01D2, 0x3800);
	poke(0x1400, 0x3800);
	ww(T.r[6], peekb((uint16_t)(0x566E + park)));
	call(0xEEE6, {});                                  // set_tile_bank
	cyc(30);
	call(0xBA2E, {});
	cyc(30);
	call(0xCFD6, {});
	cyc(30);
	const uint16_t r0 = call(0xD19C, {});
	T.r[6] = (uint16_t)(T.r[6] + 6);
	return r0;
}

// B3:7140: count the rounds of the game kind 0x0F30 (after 4: all parks open again, 0x0F29 =
// 0, and 0x018E = 1) and choose the difficulty 0x0F2D of the next park from F:90BA: with a
// medal of grade 0xC0 already won at the park (0x0261 + park, also the second player's
// 0x029F) the level rises on the first round; otherwise it is set to -(level + 1) (at most
// 10) when not positive or on a new round.  0x0F2E gets its absolute value (returned).
uint16_t choose_park_level(const Args &)
{
	const SavedRegs saved(2);
	cyc(IN_F138 + 24 + 12 + 27 + 18 + 12);
	uint16_t r0 = (uint16_t)(sext8(peekb(0x0F30)) + 1);
	pokeb(0x0F30, (uint8_t)r0);
	if ((uint8_t)r0 == 4)
	{
		cyc(27 + 27 + 33);
		pokeb(0x0F30, 0);
		pokeb(0x0F29, 0);
		pokeb(0x018E, 1);
	}
	cyc(12 + 24 + 24 + 12);
	const uint16_t park = peekb(0x0F31);
	if (peekb(0x01F6))
	{
		cyc(12 + 27 + 12);
		r0 = peekb((uint16_t)(0x029F + park));
	}
	else
	{
		cyc(12);
		r0 = 0;
	}
	cyc(12 + 27 + 12 + 18 + 12);
	r0 |= peekb((uint16_t)(0x0261 + park));
	bool raise = false;                                // L7196
	if (r0 & 0x00C0)
	{
		cyc(24 + 12);
		raise = (int8_t)peekb(0x0F2D) < 0;
	}
	if (!raise)
	{
		cyc(30);
		const uint16_t level = call(0x90BA, {});
		cyc(18 + 12);
		raise = level == 1;
	}
	enum { SET_LEVEL, NEW_LEVEL, KEEP } step;
	if (raise)
	{
		cyc(24 + 12);
		step = peekb(0x0F30) ? SET_LEVEL : NEW_LEVEL;
	}
	else
	{
		cyc(24 + 12);
		if ((int8_t)peekb(0x0F2D) <= 0)
			step = KEEP;
		else
		{
			cyc(24 + 12);
			step = peekb(0x0F30) ? KEEP : NEW_LEVEL;
		}
	}
	if (step == SET_LEVEL)
	{
		cyc(30);
		const uint16_t level = call(0x90BA, {});
		cyc(12 + 27);
		pokeb(0x0F2D, (uint8_t)level);
	}
	else if (step == NEW_LEVEL)
	{
		cyc(30);
		r0 = (uint16_t)(call(0x90BA, {}) + 1);
		cyc(12 + 18 + 12);
		if ((int16_t)r0 >= 10)
		{
			cyc(18 + 12);
			r0 = 10;
		}
		else
		{
			cyc(30);
			r0 = (uint16_t)(call(0x90BA, {}) + 1);
			cyc(12);
		}
		cyc(12 + 27);
		pokeb(0x0F2D, (uint8_t)-r0);
	}
	cyc(24 + 12);
	r0 = sext8(peekb(0x0F2D));
	if ((int16_t)r0 < 0)
	{
		cyc(12);
		r0 = (uint16_t)-r0;
	}
	cyc(27);
	pokeb(0x0F2E, (uint8_t)r0);
	return r0;
}

// B3:7E98: the landing score on screen 3: unless the skater crashed (0x031F = 1), the
// trick class 0x01F1 gives a first-time bonus (table 0x5F40, bank A 0x2000; counted in
// 0x0FB4), plus 0x01F2; with terrain bit 0x40 (0x0324) also points for air time and turn
// (table 0x5F00, F:90A6).  Half of it (rounded) is converted by F:F064 into time added to
// 0x0F34 (time bar flagged).  Bank A ends as 0x3800, which is returned.
uint16_t score_landing_screen3(const Args &)
{
	const SavedRegs saved(2);
	cyc(IN_F138 + 18 + 27 + 27 + 30 + 12);
	poke(0x01D2, 0x2000);
	poke(0x1400, 0x2000);
	if (peekb(0x031F) != 1)
	{
		cyc(12 + 12 + 24 + 18 + 12 + 18 + 12);
		uint16_t r1 = 0;
		const uint16_t trick = peekb(0x01F1);
		const uint16_t counter = (uint16_t)(0x0FB4 + trick);
		if (!peekb(counter))
		{
			cyc(12 + 27 + 21);
			r1 = peekb((uint16_t)(0x5F40 + trick));
			pokeb(counter, (uint8_t)(peekb(counter) + 1));
		}
		cyc(12 + 24 + 12 + 30 + 12);
		r1 = (uint16_t)(r1 + peekb(0x01F2));
		if (peekb(0x0324) & 0x40)
		{
			cyc(138);                                  // 7EDA-7EF0
			uint16_t air = (uint16_t)((int16_t)(uint16_t)(peek(0x01C4) - peek(0x0FAC)) >> 4);
			if ((int16_t)air > 7)
			{
				cyc(18);
				air = 7;
			}
			cyc(24 + 12);
			uint16_t turn = peek(0x02F4);
			if ((int16_t)turn < 0)
			{
				cyc(12);
				turn = (uint16_t)-turn;
			}
			cyc(24 + 30 + 30);
			uint16_t r0 = call(0x90A6, {0x0090, turn});
			cyc(18 + 12);
			if ((int16_t)r0 > 7)
			{
				cyc(18);
				r0 = 7;
			}
			cyc(123);                                  // 7F12-7F22
			r0 = (uint16_t)((r0 << 3) + air);
			r1 = (uint16_t)(r1 + peekb((uint16_t)(0x5F00 + r0)));
		}
		cyc(102);                                      // 7F24-7F2E
		const uint16_t n = call(0xF064, {(uint16_t)((int16_t)(uint16_t)(r1 + 1) >> 1)});
		cyc(117);                                      // 7F32-7F42
		poke(0x0F34, (uint16_t)(peek(0x0F34) + peek((uint16_t)(0xFCFC + (uint16_t)(n << 1)))));
		pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) | 0x04));
	}
	poke(0x01D2, 0x3800);
	poke(0x1400, 0x3800);
	return 0x3800;
}

// B3:72CA  add_tickets(int n): tickets 0x025B += n (at most 4).  In Skate City the ticket
// marks of the current player are drawn (A5:4984, mode 1, sound 0x5F; 0x0ED6 cleared) or
// erased (mode 0) for the change.  Returns the new count before the limit.
uint16_t add_tickets(const Args &a)
{
	const SavedRegs saved(3);
	cyc(IN_F144 + 27 + 12 + 12 + 12);
	const uint16_t n = a.u(0);
	const bool gain = (int16_t)n > 0;
	if (gain)
	{
		cyc(12 + 30 + 30);
		call(0x8E78, {0x005F});                       // queue_sound
		cyc(18);
	}
	cyc(24 + 12);
	if (!peekb(0x0F59))
	{
		cyc(174);                                      // 72EA-7304
		poke(0x0188, peek((uint16_t)(0x51DE + (uint16_t)(sext8(peekb(0x01F5)) << 1))));
		poke(0x01D2, 0x1800);
		poke(0x1400, 0x1800);
		if (gain)
		{
			const int16_t sum = (int16_t)(sext8(peekb(0x025B)) + n);
			cyc(24 + 12 + 18 + 12 + (sum > 4 ? 18 : 12 + 12) + 24 + 12);
			const int16_t last = sum > 4 ? 4 : sum;
			for (int16_t i = (int16_t)(sext8(peekb(0x025B)) + 1);; i++)
			{
				cyc(12 + 12 + 12);
				if (i > last) break;
				cyc(24 + 24 + 24 + 30 + 30);
				call(0x4984, {0x0001, (uint16_t)i, sext8(peekb(0x01F5))});
				cyc(18);
			}
			cyc(27 + 12);
			pokeb(0x0ED6, 0);
		}
		else
		{
			cyc(12 + 12);
			if ((int16_t)n < 0)
			{
				const int16_t sum = (int16_t)(sext8(peekb(0x025B)) + n + 1);
				cyc(24 + 12 + 12 + 18 + 12 + (sum > 0 ? 12 + 12 : 18) + 24 + 12);
				const int16_t last = sum > 0 ? sum : 1;
				for (int16_t i = (int16_t)sext8(peekb(0x025B));; i--)
				{
					cyc(12 + 12);
					if (i < last) break;
					cyc(24 + 24 + 24 + 24 + 30);
					call(0x4984, {0x0000, (uint16_t)i, sext8(peekb(0x01F5))});
					cyc(18 + 12);
				}
			}
		}
		cyc(18 + 27 + 27);
		poke(0x01D2, 0x3800);
		poke(0x1400, 0x3800);
	}
	const uint16_t r0 = (uint16_t)(sext8(peekb(0x025B)) + n);
	pokeb(0x025B, (uint8_t)r0);
	if ((int8_t)(uint8_t)r0 > 4)
	{
		cyc(33);
		pokeb(0x025B, 4);
	}
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
	const SavedRegs saved(5);
	cyc(IN_F162 + 24 + 12);
	bool choose = peekb(0x0F2B) != 0;
	if (!choose)
	{
		cyc(24 + 12);
		choose = !peekb(0x01F6);
	}
	if (choose)
	{
		cyc(24 + 12);
		if (peekb(0x0F26))
		{
			cyc(30);
			call(0x7140, {});                          // choose_park_level
		}
	}
	cyc(33 + 24 + 12);
	poke(0x0EAA, 0x0001);
	if (peekb(0x0F26))
	{
		cyc(24 + 30);
		call(0xF028, {0x0000});                        // add_time
		cyc(18);
	}
	cyc(24 + 12);
	uint16_t w1, w2, b1, b2;
	if (!peekb(0x0F29))
	{
		cyc(36 + 36 + 24 + 24 + 24 + 33 + 24 + 33 + 12);
		w1 = peek(0x564E);
		w2 = peek(0x5658);
		b1 = peekb(0x565E);
		b2 = peekb(0x5664);
	}
	else
	{
		cyc(12 + 24 + 12 + 12 + 39 + 39 + 24 + 24 + 24 + 36 + 24 + 36);
		const uint16_t park = peekb(0x027A), i = (uint16_t)(park << 1);
		w1 = peek((uint16_t)(0x5646 + i));
		w2 = peek((uint16_t)(0x5650 + i));
		b1 = peekb((uint16_t)(0x565A + park));
		b2 = peekb((uint16_t)(0x5660 + park));
	}
	push(w1);
	push(w2);
	push(0);
	push(0);
	push(b1);
	push(b2);
	cyc(30);
	call(0x8328, {});
	cyc(12 + 24 + 12 + 36 + 30);
	ww(T.r[6], peek((uint16_t)(0x567A + (uint16_t)(peekb(0x027A) << 1))));
	call(0xEEE6, {});                                  // set_tile_bank
	cyc(21 + 30);
	ww(T.r[6], 0);
	call(0x7DD6, {});                                  // update_table_0fb4
	cyc(27 + 27 + 24 + 12);
	pokeb(0x0EA4, 0);
	pokeb(0x0ED8, 0);
	if (peekb(0x01F6))
	{
		cyc(30 + 12);
		if (peekb(0x0F2B) != 4)
		{
			cyc(12 + 24 + 12 + 12 + 18 + 27 + 27);
			const uint16_t kind = (uint16_t)((peekb(0x0F2B) + 1) & 0x0003);
			pokeb(0x0F2B, 0);
			pokeb(0x0F2B, (uint8_t)(peekb(0x0F2B) | kind));
		}
	}
	cyc(27 + 30);
	ww(T.r[6], 3);
	call(0x55DE, {});                                  // update_ticket_bits
	cyc(30);
	call(0xD19C, {});
	cyc(30);
	call(0x81AA, {});                                  // install_interrupts_a
	cyc(30);
	call(0x8F74, {});
	cyc(27 + 30);
	pokeb(0x0F26, 0);
	const uint16_t r0 = call(0x7446, {});              // draw_status_line
	pokeb(0x0F28, 0);
	pokeb(0x0F27, 0);
	pokeb(0x0279, 0);
	poke(0x01FE, 0x001E);
	pokeb(0x0ED6, 0);
	pokeb(0x0F2A, (uint8_t)(peekb(0x0F2A) & ~0x3F));
	pokeb(0x018F, 0);
	T.r[6] = (uint16_t)(T.r[6] + 12);
	return r0;
}

}

LOGIC_TABLE(bankb,
	asm_routine("B1", 0x7854, "record_position_trail", record_position_trail, 21),
	c_routine("B1", 0x78E4, "start_trail_return", start_trail_return, 0, 309),
	asm_routine("B1", 0x792A, "trail_return_step", trail_return_step, 21),
	asm_routine("B1", 0x7AA2, "object4_step", object4_step, 21),
	asm_routine("B1", 0x7B3E, "start_object4", start_object4, 21),
	asm_routine("B3", 0x79D4, "draw_countdown", draw_countdown, 0),
	asm_routine("B3", 0x7A3A, "tick_countdown", tick_countdown, 21),
	c_routine("B3", 0x7DD6, "update_table_0fb4", update_table_0fb4, 0, OUT_F144),
	c_routine("B3", 0x7E44, "frame_delta_bonus", frame_delta_bonus, 0, 39),
	c_routine("B3", 0x7F52, "tick_game_timer", tick_game_timer, 0, OUT_F162),
	c_routine("B3", 0x739A, "draw_cash_meter", draw_cash_meter, 0, OUT_F152),
	asm_routine("B3", 0x77B4, "update_status_display", update_status_display, 45),
	asm_routine("B3", 0x78BE, "update_time_bar", update_time_bar, 21),
	asm_routine("B6", 0x7B82, "track_scroll_blocks", track_scroll_blocks, 21),
	asm_routine("B6", 0x7C84, "step_pause", step_pause, 21),
	c_routine("B6", 0x7CF6, "show_hint", show_hint, 0, OUT_F144),
	asm_routine("B6", 0x7E54, "hint_countdowns", hint_countdowns, 21),
	asm_routine("B6", 0x7F16, "show_ticket_hint", show_ticket_hint, 21),
	asm_routine("B8", 0x7DB8, "shift_rounded", shift_rounded, 21),
	asm_routine("B1", 0x7BBC, "animate_palette", animate_palette, 21),
	c_routine("B3", 0x7ADA, "score_landing", score_landing, 0, 18 + OUT_F162),
	c_routine("B3", 0x7446, "draw_status_line", draw_status_line, 0, 21),
	asm_routine("B6", 0x7942, "follow_skater_scroll", follow_skater_scroll, 24 + 21),
	asm_routine("B3", 0x65FE, "park_mode_step", park_mode_step, 21),
	c_routine("B3", 0x6A1C, "load_demo_park", load_demo_park, 0, 18 + OUT_F144),
	c_routine("B3", 0x7140, "choose_park_level", choose_park_level, 0, OUT_F138),
	c_routine("B3", 0x7E98, "score_landing_screen3", score_landing_screen3, 0, 72 + OUT_F138),
	c_routine("B3", 0x72CA, "add_tickets", add_tickets, 0, 93 + OUT_F144),
	wait_c_routine("B3", 0x6D40, "next_turn", next_turn, 201 + 18 + OUT_F162),
)

}
