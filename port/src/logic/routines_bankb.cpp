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
// the register-save helpers F:F138 / F144 / F152 / F162: entry (JSR R1 included) and exit
// (the body's RTS included)
constexpr int IN_F138 = 135, IN_F144 = 159, IN_F152 = 183, IN_F162 = 207;
constexpr int OUT_F138 = 90, OUT_F144 = 108, OUT_F152 = 126, OUT_F162 = 132;

// MOV / BIS / BIC / XOR / BIT: N and Z from the result, V clear, C unchanged
uint16_t lw(uint16_t r) { f_nzv(nzw(r)); return r; }
uint8_t lb(uint8_t r) { f_nzv(nzb(r)); return r; }

// the register-save helpers F:F138 / F144 / F152 / F162 restore R1 and R2..Rlast on return
struct SavedRegs {
	uint16_t r[6];
	int last;
	explicit SavedRegs(int last_) : last(last_) { for (int i = 1; i <= last; i++) r[i] = T.r[i]; }
	~SavedRegs() { if (!std::uncaught_exceptions()) for (int i = 1; i <= last; i++) T.r[i] = r[i]; }
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
	call(0x843E, {peek(0x06B0), peek(0x0730), peek(0x06F0)});
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
// F:BA9A) in the text layer, top left in Skate City and on screen 7, bottom left elsewhere
void draw_countdown()
{
	cyc(30 + 12);
	const uint16_t f = lw(peek(0x0FA2) & 0x0010);
	if (!f) return;
	cyc(33 + 30);
	pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) & ~0x10));
	call(0x8068, {});                                  // select_video_bank0
	cyc(33 + 12 + 24 + 12 + 12);
	poke(0x0188, 0xC000);
	T.r[0] = peekb(0x0F59);
	if (T.r[0] == 0)
		cyc(33 + 12);
	else
		cyc(18 + 12 + (T.r[0] == 7 ? 33 + 12 : 33));
	poke(0x0186, (T.r[0] == 0 || T.r[0] == 7) ? 0x213C : 0x35BC);
	cyc(36 + 24 + 30 + 30 + 24 + 30);
	push(peek(0x0F1C));
	push(0);
	push(2);
	push(1);
	push(0);
	T.r[0] = call(0xBA9A, {});
	cyc(30 + 12);
	const uint16_t n = peek(0x0F1C);
	if ((int16_t)n < 10)
	{
		cyc(33 + 18 + 30);
		poke(0x0186, (uint16_t)(peek(0x0186) - 8));
		T.r[0] = 0x0020;
		call(0x8B52, {});                              // draw_large_char
	}
	cyc(18);
	T.r[6] = fl::add(T.r[6], 10);
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
// state 1 and the time-up messages are shown, with sound 0xA1.
void update_time_bar()
{
	cyc(24 + 12);
	T.r[0] = peek(0x0254);
	fl::mov(T.r[0]);
	bool time_up = false;
	if (!T.r[0])
	{
		cyc(24 + 12);
		const uint8_t level = peekb(0x0F59);
		fl::tstb(level);
		time_up = level == 0;
	}
	if (time_up)
	{
		cyc(30 + 12);
		if (lw(peek(0x0180) & 0x0001)) return;
		cyc(24 + 12);
		const uint8_t m = peekb(0x0F26);
		fl::tstb(m);
		if (m) return;
		cyc(30 + 12);
		const uint8_t st = peekb(0x031F);
		fl::cmpb(st, 1);
		if (st == 1) return;
		cyc(33 + 30 + 30 + 30);
		pokeb(0x018F, 1);
		push(0x5DEE);
		push(1);
		call(0xEAD4, {});
		cyc(18 + 30);
		T.r[0] = 0x00E4;
		call(0x8AB6, {});
		cyc(33 + 27 + 24 + 30);
		poke(0x0188, 0x6000);
		ww(T.r[6], 0x5F06);
		push(0);
		call(0xEAD4, {});
		cyc(27 + 30);
		ww(T.r[6], 0x00A1);
		T.r[0] = call(0x8E78, {});                     // queue_sound
		cyc(30);
		call(0xE02E, {});
		cyc(18);
		T.r[6] = fl::add(T.r[6], 6);
		return;
	}
	cyc(30 + 12);
	if (!lw(peek(0x0FA2) & 0x0004))
	{
		cyc(12);                                       // BEQ at 7996
		return;
	}
	cyc(24 + 12);
	uint8_t m = peekb(0x0F36);
	fl::tstb(m);
	if (m) return;
	cyc(24 + 12);
	m = peekb(0x0F26);
	fl::tstb(m);
	if (m)
	{
		cyc(30 + 12);
		const uint8_t k = peekb(0x027A);
		fl::cmpb(k, 2);
		if (k < 2)
		{
			cyc(24 + 12);
			T.r[0] = lw(peek(0x0F20));
		}
		else
		{
			cyc(24 + 12 * 3 + 30 + 12 + 24);
			T.r[0] = asr_w(asr_w(asr_w(peek(0x0F34))));
			fl::cmpb(peekb(0x027A), 2);
			if (peekb(0x027A) == 2)
			{
				cyc(12);
				T.r[0] = asr_w(T.r[0]);
			}
			T.r[0] = asr_w(asr_w(T.r[0]));
		}
	}
	cyc(141);                                          // 7904-7918
	pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) & ~0x04));
	T.r[3] = (uint16_t)((int16_t)T.r[0] >> 3);
	T.r[4] = T.r[0] & 0x0007;
	call(0x8068, {});                                  // select_video_bank0
	cyc(33 + 12 + 33 + 24 + 12);
	poke(0x0188, 0x4000);
	T.r[3] = (uint16_t)(T.r[3] << 1);
	poke(0x0186, 0x2230);
	if (peekb(0x0F26))
	{
		cyc(33 + 18 + 12);
		poke(0x0186, (uint16_t)(peek(0x0186) - 4));
		T.r[0] = 0x0026;
	}
	else
	{
		cyc(18);
		T.r[0] = 0x001E;
	}
	cyc(27 + 12);
	poke(0x0186, (uint16_t)(peek(0x0186) + T.r[0]));
	for (;;)
	{
		cyc(12 + 12);
		if ((int16_t)T.r[0] < 0) break;
		cyc(24 + 12 + 12);
		T.r[2] = peek(0x0186);
		if ((int16_t)T.r[0] > (int16_t)T.r[3])
		{
			cyc(18 + 12);
			T.r[1] = 0x009E;
		}
		else
		{
			cyc(12 + 18 + 12);
			T.r[1] = (int16_t)T.r[0] < (int16_t)T.r[3] ? 0x0096 : (uint16_t)(0x009E - T.r[4]);
		}
		cyc(33 + 21 + 36 + 36 + 33 + 18);
		poke(T.r[2], peek(0x0188));
		poke(T.r[2], (uint16_t)(peek(T.r[2]) | T.r[1]));
		const uint16_t below = (uint16_t)(T.r[2] + 0x80);
		poke(below, peek(T.r[2]));
		poke(below, (uint16_t)(peek(below) + 9));
		poke(0x0186, (uint16_t)(peek(0x0186) - 2));
		T.r[0] = (uint16_t)(T.r[0] - 2);
	}
	fl::tst(T.r[0]);
}

// ---------------------------------------------------------------------------------------------
// Scroll blocks, pauses and hint messages (B6)

// B6:7B82 (assembly): follow the scroll position (0x01AC, 0x01AE) in blocks of 0x4000: when x
// leaves the block 0x09A0, 0x0996/0x0998 move by 0x10; when y leaves the block 0x09A2, the
// row 0x099A/0x099C moves by one, wrapping by 12 rows past 0x099E or below 0 (then
// 0x0996/0x0998 move by 0x50 and 0x0E9B by one).  0x09A4/0x09A6 keep the offsets in the
// block; 0x0E99 counts the block changes.
void track_scroll_blocks()
{
	cyc(24 + 27 + 39 + 30 + 12);
	T.r[0] = peek(0x01AC);
	poke(0x09A4, T.r[0]);
	poke(0x09A4, fl::sub(peek(0x09A4), peek(0x09A0)));
	fl::cmp(peek(0x09A4), 0x4000);
	if (peek(0x09A4) >= 0x4000)
	{
		cyc(24 + 12);
		const uint16_t d = peek(0x09A4);
		fl::tst(d);
		if ((int16_t)d > 0)
		{
			cyc(33 + 33 + 12);
			poke(0x0996, fl::add(peek(0x0996), 0x0010));
			poke(0x0998, fl::add(peek(0x0998), 0x0010));
		}
		else
		{
			cyc(33 + 33);
			poke(0x0996, fl::sub(peek(0x0996), 0x0010));
			poke(0x0998, fl::sub(peek(0x0998), 0x0010));
		}
		cyc(27 + 33 + 27 + 39 + 24 + 12 + 27);
		poke(0x09A0, T.r[0]);
		poke(0x09A0, lw(peek(0x09A0) & ~0x3FFF));
		poke(0x09A4, T.r[0]);
		poke(0x09A4, fl::sub(peek(0x09A4), peek(0x09A0)));
		T.r[0] = fl::inc(sext8(peekb(0x0E99)));
		pokeb(0x0E99, lb((uint8_t)T.r[0]));
	}
	cyc(24 + 24 + 12 + 27 + 33 + 30 + 12);
	T.r[1] = peek(0x01AE);
	T.r[0] = fl::sub(peek(0x09A2), T.r[1]);
	poke(0x09A6, T.r[0]);
	poke(0x09A6, fl::add(peek(0x09A6), 0x3FFF));
	fl::cmp(peek(0x09A6), 0x4000);
	if (peek(0x09A6) < 0x4000) return;
	cyc(24 + 12);
	const uint16_t d = peek(0x09A6);
	fl::tst(d);
	bool wrapped;
	if ((int16_t)d < 0)
	{
		cyc(27 + 27 + 36 + 12);
		poke(0x099A, fl::inc(peek(0x099A)));
		poke(0x099C, fl::inc(peek(0x099C)));
		fl::cmp(peek(0x099C), peek(0x099E));
		wrapped = (int16_t)peek(0x099C) > (int16_t)peek(0x099E);
		if (wrapped)
		{
			cyc(33 * 4 + 24 + 12 + 12);
			poke(0x099C, fl::sub(peek(0x099C), 0x000C));
			poke(0x099A, fl::sub(peek(0x099A), 0x000C));
			poke(0x0998, fl::add(peek(0x0998), 0x0050));
			poke(0x0996, fl::add(peek(0x0996), 0x0050));
			T.r[0] = fl::dec(sext8(peekb(0x0E9B)));
		}
	}
	else
	{
		cyc(27 + 27 + 12);
		poke(0x099A, fl::dec(peek(0x099A)));
		poke(0x099C, fl::dec(peek(0x099C)));
		wrapped = (int16_t)peek(0x099C) < 0;
		if (wrapped)
		{
			cyc(33 * 4 + 24 + 12);
			poke(0x099C, fl::add(peek(0x099C), 0x000C));
			poke(0x099A, fl::add(peek(0x099A), 0x000C));
			poke(0x0998, fl::sub(peek(0x0998), 0x0050));
			poke(0x0996, fl::sub(peek(0x0996), 0x0050));
			T.r[0] = fl::inc(sext8(peekb(0x0E9B)));
		}
	}
	if (wrapped)
	{
		cyc(27);
		pokeb(0x0E9B, lb((uint8_t)T.r[0]));
	}
	cyc(27 + 33 + 24 + 12 + 27 + 33 + 24 + 12 + 27);
	poke(0x09A2, T.r[1]);
	poke(0x09A2, lw(peek(0x09A2) & ~0x3FFF));
	T.r[0] = fl::sub(peek(0x09A2), T.r[1]);
	poke(0x09A6, T.r[0]);
	poke(0x09A6, fl::add(peek(0x09A6), 0x3FFF));
	T.r[0] = fl::inc(sext8(peekb(0x0E99)));
	pokeb(0x0E99, lb((uint8_t)T.r[0]));
}

// B6:7C84 (assembly): the pause counter 0x01FC.  Positive: it counts down (a new press of
// the button in 0x0193 bit 7 takes 40 more off while at most 120 are left, 0x0FE6 keeps the
// button state); when it runs out, F:A8FE and the interrupts are set up again (F:81AA) and
// the frames since the pause began (0x0200) are added to 0x0FB0 and 0x0FAC.  Negative: the
// pause begins (F:820C), it is made positive and the frame is recorded in 0x0200.
void step_pause()
{
	cyc(24 + 12);
	T.r[0] = peek(0x01FC);
	fl::mov(T.r[0]);
	if ((int16_t)T.r[0] <= 0)
	{
		cyc(12);
		if (T.r[0] == 0) return;
		cyc(27 + 30);
		poke(0x01FC, neg_w(peek(0x01FC)));
		T.r[0] = call(0x820C, {});                     // install_interrupts_b
		cyc(39);
		poke(0x0200, lw(peek(0x01C4)));
		return;
	}
	cyc(27 + 30 + 12);
	poke(0x01FC, fl::dec(peek(0x01FC)));
	fl::cmp(peek(0x01FC), 0x0078);
	if ((int16_t)peek(0x01FC) > 0x0078) return;
	bool tested = false;
	cyc(24 + 12);
	uint8_t b = peekb(0x0FE6);
	fl::tstb(b);
	if (!b)
	{
		cyc(24 + 12);
		b = peekb(0x0193);
		fl::tstb(b);
		if (b & 0x80)
		{
			cyc(33 + 12);
			const uint16_t v = fl::sub(peek(0x01FC), 0x0028);
			poke(0x01FC, v);
			tested = (int16_t)v >= 0;
			if (!tested)
			{
				cyc(27);
				poke(0x01FC, 0);
				fl::clr();
			}
		}
	}
	if (!tested)
	{
		cyc(24);
		fl::tst(peek(0x01FC));
	}
	cyc(12);
	if (!peek(0x01FC))
	{
		cyc(30);
		T.r[0] = call(0xA8FE, {});
		cyc(30);
		T.r[0] = call(0x81AA, {});                     // install_interrupts_a
		cyc(24 + 24 + 27 + 27 + 27);
		T.r[0] = fl::sub(peek(0x01C4), peek(0x0200));
		poke(0x0FB0, fl::add(peek(0x0FB0), T.r[0]));
		poke(0x0FAC, fl::add(peek(0x0FAC), T.r[0]));
		poke(0x01FC, 0);
		fl::clr();
	}
	cyc(24 + 12);
	b = peekb(0x0193);
	fl::tstb(b);
	if (b & 0x80)
	{
		cyc(33);
		pokeb(0x0FE6, 1);
		fl::movb(1);
	}
	else
	{
		cyc(27);
		pokeb(0x0FE6, 0);
		fl::clr();
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

// fill one row of 13 text cells (and the row below) at the cursor 0x0186 with tile 0x87
void fill_status_row(uint16_t colour)
{
	const uint16_t tile = colour | 0x0087;
	for (uint16_t i = 0; (int16_t)i <= 0x18; i = (uint16_t)(i + 2))
	{
		cyc(12 + 24 + 30 + 21 + 18 + 18 + 12);
		const uint16_t p = (uint16_t)(i + peek(0x0186));
		poke((uint16_t)(p + 0x80), tile);
		poke(p, tile);
	}
}

// B3:7446: draw the whole status line: the player labels (texts 0x5DCE + 8 * player, with
// 0x10 in mode 1 of 0x0F26), the timer bar (B3:78BE) or the park's goal text (by screen,
// F:90BA), both players' scores, the high score, ticket marks (A5:4984), cash and meters,
// and in a two-player game the high score flash and a message (screen 0 only)
uint16_t draw_status_line(const Args &)
{
	const SavedRegs saved(5);
	cyc(IN_F162 + 30 + 12);
	const uint16_t mode1 = peekb(0x0F26) == 1 ? 0x0010 : 0x0000;
	cyc(mode1 ? 18 + 12 : 12);
	cyc(12 + 24 + 12 + 12);
	for (int16_t p = (int16_t)peekb(0x01F6); p >= 0; p--)
	{
		cyc(222);                                      // 7464-7480
		poke(0x0188, peek((uint16_t)(0x51DE + 2 * p)));
		T.r[0] = call(0xEAD4, {0x0000, (uint16_t)(8 * p + mode1 + 0x5DCE)});
		cyc(24 + 12 + 12);
	}
	cyc(24 + 12);
	if (!peekb(0x018F))
	{
		cyc(30);
		call(0x8068, {});                              // select_video_bank0
		cyc(12 + 24 + 12 + 12);
		T.r[0] = peekb(0x0F26);
		bool timer = T.r[0] == 0;
		if (!timer)
		{
			cyc(18 + 12);
			timer = T.r[0] == 0x000A;
		}
		if (timer)
		{
			cyc(24 + 12);
			timer = !peekb(0x0F1E);
		}
		if (timer)
		{
			cyc(24 + 12);
			if ((int16_t)peek(0x0254) > 0)
			{
				cyc(117);                              // 74B0-74BC
				poke(0x0188, 0x4000);
				T.r[0] = call(0xEAD4, {0x0000, 0x5DEE});
				cyc(33 + 30);
				pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) | 0x04));
				T.r[0] = call(0x78BE, {});                 // update_time_bar
				cyc(12 + 24);
			}
			else
			{
				cyc(18 + 30);
				T.r[0] = 0x0064;
				T.r[0] = call(0x8AB6, {});
				cyc(12);
			}
		}
		else
		{
			cyc(192);                                  // 74D6-74F2
			poke(0x0188, 0x4000);
			poke(0x01D2, 0x2000);
			poke(0x1400, 0x2000);
			// jump table at 0x77AC for screens 1-4; the original jumps outside it for others
			const uint16_t level = peekb(0x0F59);
			static const uint16_t goal_index[4] = { 0x5E83, 0x5E8D, 0x5EA1, 0x5E97 };
			static const uint16_t goal_text[4] = { 0x5E56, 0x5E56, 0x5EA6, 0x5EA6 };
			static const int goal_cycles[4] = { 111, 123, 111, 99 };
			if (level >= 1 && level <= 4)
			{
				cyc(30);
				T.r[0] = call(0x90BA, {});
				cyc(goal_cycles[level - 1]);
				T.r[0] = peekb((uint16_t)(goal_index[level - 1] + T.r[0]));
				T.r[0] = (uint16_t)((T.r[0] << 3) + goal_text[level - 1]);
				cyc(24 + 24 + 30);
				T.r[0] = call(0xEAD4, {0x0000, T.r[0]});
				cyc(18 + 27 + 27);
			}
			poke(0x01D2, 0x3800);
			poke(0x1400, 0x3800);
			cyc(24);
		}
	}
	const uint16_t me = sext8(peekb(0x01F5));
	cyc(24 + 12);
	if (peekb(0x01F6))
	{
		cyc(24 + 12 + (me ? 33 : 33 + 12));
		poke(0x0186, me ? 0x2204 : 0x2262);
		cyc(141);                                      // 7568-757E
		const uint16_t other = (uint16_t)((me ^ 1) << 1);
		poke(0x0188, peek((uint16_t)(0x51DE + other)));
		fill_status_row(peek((uint16_t)(0x51DE + other)));
		cyc(192);                                      // 7596-75A8
		T.r[0] = call(0xBA9A, {0x0001, 0x0001, 0x0006, peek(0x027E), peek(0x027C)});
		cyc(24 + 12 + (peekb(0x01F5) ? 33 : 33 + 12) + 18 + 12);
		poke(0x0186, peekb(0x01F5) ? 0x2262 : 0x2204);
	}
	else
	{
		cyc(33);
		poke(0x0186, 0x2204);
	}
	cyc(135);                                          // 75CC-75E0
	const uint16_t mine = (uint16_t)(sext8(peekb(0x01F5)) << 1);
	poke(0x0188, peek((uint16_t)(0x51DE + mine)));
	fill_status_row(peek((uint16_t)(0x51DE + mine)));
	cyc(192);                                          // 75F8-760A
	T.r[0] = call(0xBA9A, {0x0001, 0x0001, 0x0006, peek(0x0240), peek(0x023E)});
	cyc(30 + 12);
	if (peekb(0x0F26) == 1)
	{
		cyc(18 + 33);
		pokeb(0x036C, 1);
	}
	else
	{
		cyc(24 + 12);
		if (!peekb(0x018F))
		{
			cyc(27 + 24 + 12);
			pokeb(0x036C, 0);
			bool high = !peekb(0x01F6);
			if (!high)
			{
				cyc(30 + 12);
				high = peekb(0x0F2B) == 4;
			}
			if (high)
			{
				cyc(114);                              // 7632-763E
				poke(0x0188, 0xA000);
				T.r[0] = call(0xEAD4, {0x0000, 0x5DF6});
				cyc(216);                              // 7642-765A
				poke(0x0186, 0x3746);
				T.r[0] = call(0xBA96, {0x0001, 0x0000, 0x0006, peek(0x0246), peek(0x0244)});
				cyc(18);
			}
		}
		cyc(12 + 24 + 12 + 12);
		const int16_t players = (int16_t)peekb(0x01F6);
		for (int16_t p = 0;; p++)
		{
			cyc(12 + 12);
			if (p > players) break;
			cyc(186 + 36 + 12 + 12);                   // 766C-7698
			poke(0x0188, peek((uint16_t)(0x51DE + 2 * p)));
			poke(0x01D2, 0x1800);
			poke(0x1400, 0x1800);
			T.r[0] = sext8(peekb(0x01F5));
			const int16_t marks = (uint16_t)p == T.r[0] ? (int16_t)sext8(peekb(0x025B)) : (int16_t)peekb(0x0299);
			for (int16_t i = 1;; i++)
			{
				cyc(12 + 12 + 12);
				if (i > marks) break;
				cyc(105);                              // 769A-76A2
				T.r[0] = call(0x4984, {0x0001, (uint16_t)i, (uint16_t)p});
				cyc(24);
			}
			cyc(84);                                   // 76AE-76BA
			poke(0x01D2, 0x3800);
			poke(0x1400, 0x3800);
		}
		cyc(102);                                      // 76C0-76D0
		poke(0x018A, 0x00A8);
		poke(0x0188, 0x8000);
		if (peekb(0x01F6))
		{
			cyc(318);                                  // 76D2-76FA
			const uint16_t other = (uint16_t)(1 ^ sext8(peekb(0x01F5)));
			poke(0x0186, (uint16_t)(peek((uint16_t)(0x51E2 + (uint16_t)(other << 1))) + 0x2000));
			call(0xBA9A, {0x0001, 0x0001, 0x0006, 0x0000, peek(0x0294)});
			cyc(102);                                  // 76FE-7708
			call(0x739A, {(uint16_t)(1 ^ sext8(peekb(0x01F5)))});   // draw_cash_meter
			cyc(141);                                  // 770C-7722
			const uint16_t r0 = (uint16_t)(sext8(peekb(0x01F5)) << 1);
			poke(0x0186, (uint16_t)(peek((uint16_t)(0x51E2 + r0)) + 0x2000));
		}
		else
		{
			cyc(39 + 33);
			poke(0x0186, (uint16_t)(peek(0x51E2) + 0x2000));
		}
		cyc(177);                                      // 7730-7740
		call(0xBA9A, {0x0001, 0x0001, 0x0006, 0x0000, peek(0x0256)});
		cyc(75);                                       // 7744-774A
		T.r[0] = call(0x739A, {sext8(peekb(0x01F5))});             // draw_cash_meter
		cyc(18 + 12);
	}
	cyc(33 + 24 + 12);
	poke(0x018A, 0x0001);
	if (peekb(0x01F6))
	{
		cyc(30 + 12);
		if (peekb(0x0F2B) != 4)
		{
			cyc(30 + 12);
			if (peekb(0x018F) != 4)
			{
				cyc(102);                              // 7776-7786
				pokeb(0x0EA6, (uint8_t)(peekb(0x0EA6) | 0x02));
				poke(0x0EAA, 0x00B4);
				if (!peekb(0x0F59))
				{
					const bool kind1 = peekb(0x0F2B) == 1;
					cyc(30 + 12 + (kind1 ? 27 + 12 : 27) + 30 + 24 + 30);
					T.r[0] = call(0xA974, {0x0000, 0x0078, kind1 ? (uint16_t)0x76F0 : (uint16_t)0x75EA});
					cyc(24);
				}
			}
		}
	}
	return T.r[0];
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
	asm_routine("B3", 0x79D4, "draw_countdown", draw_countdown, 21),
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
	c_routine("B3", 0x7446, "draw_status_line", draw_status_line, 0, 18 + OUT_F162),
	asm_routine("B6", 0x7942, "follow_skater_scroll", follow_skater_scroll, 24 + 21),
	asm_routine("B3", 0x65FE, "park_mode_step", park_mode_step, 21),
	c_routine("B3", 0x6A1C, "load_demo_park", load_demo_park, 0, 18 + OUT_F144),
	c_routine("B3", 0x7140, "choose_park_level", choose_park_level, 0, OUT_F138),
	c_routine("B3", 0x7E98, "score_landing_screen3", score_landing_screen3, 0, 72 + OUT_F138),
	c_routine("B3", 0x72CA, "add_tickets", add_tickets, 0, 93 + OUT_F144),
	wait_c_routine("B3", 0x6D40, "next_turn", next_turn, 201 + 18 + OUT_F162),
)

}
