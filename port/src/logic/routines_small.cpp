// Rewritten routines: small hardware and state helpers.  These were written in assembly in
// the original (register interface), so registers and condition codes are reproduced too.
// They follow the original instruction by instruction: after each instruction its cycles are
// charged and interrupts may come in at the next one's address (tick), so interrupts and the
// other processors see every write when they would in the original (see routines_core.cpp).
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


// B3:7AD2: remember the frame counter (0x01C4) in 0x0FB0
void save_frame_counter()
{
	const uint16_t v = peek(0x01C4);
	poke(0x0FB0, v);
	fl::mov(v);
	tick(0x7AD8, 39);
	rts();
}

// F:8E6A / F:8E72: flag at 0x01A4
void set_flag_01a4()
{
	pokeb(0x01A4, 1);
	fl::movb(1);
	tick(0x8E70, 33);
	rts();
}
void clear_flag_01a4()
{
	pokeb(0x01A4, 0);
	fl::clr();
	tick(0x8E76, 27);
	rts();
}

// F:917C: feed the watchdog
void feed_watchdog()
{
	wb(0x1800, 0);
	fl::clr();
	tick(0x9180, 27);
	rts();
}

// F:E02E: 0x0EB4 = 120
void set_0eb4()
{
	poke(0x0EB4, 0x0078);
	fl::mov(0x0078);
	tick(0xE034, 33);
	rts();
}

// F:8282: turn off interrupt source bit 2 in the shadow mask (0x018C) and the hardware
void irq_disable_bit2()
{
	const uint16_t v = peek(0x018C) & ~0x0004;
	poke(0x018C, v);
	fl::logic(v);
	tick(0x8288, 33);
	const uint16_t m = peek(0x018C);
	ww(0x1600, m);
	fl::mov(m);
	tick(0x828E, 39);
	rts();
}

// F:E6F0: screen x of object 1 = its world x - horizontal scroll
void object1_screen_x()
{
	const uint16_t x = peek(0x06F2);
	poke(0x0632, x);
	fl::mov(x);
	tick(0xE6F6, 39);
	poke(0x0632, fl::sub(peek(0x0632), peek(0x01A8)));
	tick(0xE6FC, 39);
	rts();
}

// F:8068: select video RAM window 0 (slapstic: arm at 0x8000, select at 0x8030)
void select_video_bank0()
{
	poke(0x01D0, 0x0030);
	fl::mov(0x0030);
	tick(0x806E, 33);
	fl::tst(rw(0x8000));
	tick(0x8072, 24);
	fl::tst(rw(0x8030));
	tick(0x8076, 24);
	rts();
}

// F:8096: select the video RAM window recorded in 0x01D0 again
void reselect_video_bank()
{
	T.r[0] = peek(0x01D0);
	fl::mov(T.r[0]);
	tick(0x809A, 24);
	fl::tst(rw(0x8000));
	tick(0x809E, 24);
	fl::tst(rw((uint16_t)(0x8000 + T.r[0])));
	tick(0x80A2, 27);
	rts();
}

// F:80C8: add R3:R4 to the long at (R1 = high word, R2 = low word)
void add32()
{
	poke(T.r[2], fl::add(peek(T.r[2]), T.r[4]));
	tick(0x80CA, 21);
	poke(T.r[1], fl::adc(peek(T.r[1])));
	tick(0x80CC, 21);
	poke(T.r[1], fl::add(peek(T.r[1]), T.r[3]));
	tick(0x80CE, 21);
	rts();
}

// F:80D0: subtract R3:R4 from the long at (R1 = high word, R2 = low word)
void sub32()
{
	poke(T.r[2], fl::sub(peek(T.r[2]), T.r[4]));
	tick(0x80D2, 21);
	poke(T.r[1], fl::sbc(peek(T.r[1])));
	tick(0x80D4, 21);
	poke(T.r[1], fl::sub(peek(T.r[1]), T.r[3]));
	tick(0x80D6, 21);
	rts();
}

// F:8270: point the four 32V interrupt vectors (addresses listed at 0xFC5E) at 0xB8FC
void set_32v_vectors()
{
	T.r[0] = 6;
	fl::mov(6);
	tick(0x8274, 18);
	for (;;)
	{
		const uint16_t v = 0xB8FC;
		ww(peek((uint16_t)(0xFC5E + T.r[0])), v);
		fl::mov(v);
		tick(0x827A, 42);
		T.r[0] = fl::sub(T.r[0], 2);
		tick(0x827E, 18);
		if ((int16_t)T.r[0] < 0) break;
		tick(0x8274, 12);                              // BPL
	}
	tick(0x8280, 12);
	rts();
}

// F:BA5E: clear the per-object screen-y deltas (0x05F0, 32 words)
void clear_object_dy()
{
	T.r[0] = 0x003E;
	fl::mov(0x003E);
	tick(0xBA62, 18);
	for (;;)
	{
		poke((uint16_t)(0x05F0 + T.r[0]), 0);
		fl::clr();
		tick(0xBA66, 30);
		T.r[0] = fl::sub(T.r[0], 2);
		tick(0xBA6A, 18);
		if ((int16_t)T.r[0] < 0) break;
		tick(0xBA62, 12);                              // BPL
	}
	tick(0xBA6C, 12);
	rts();
}

// F:E222: game state 1 moves to state 8 with a one-second delay (0x0F22 = 60)
void state1_to_8()
{
	const uint8_t st = peekb(0x018F);
	fl::cmpb(st, 1);
	tick(0xE228, 30);
	if (st != 1)
	{
		tick(0xE236, 12);                              // BNE
		rts();
		return;
	}
	tick(0xE22A, 12);
	pokeb(0x018F, 8);
	fl::movb(8);
	tick(0xE230, 33);
	poke(0x0F22, 0x003C);
	fl::mov(0x003C);
	tick(0xE236, 33);
	rts();
}

// F:D348: count(n): for n != 0 increment the byte counter 0x0E7D+n and the total 0x0E93
void count_event()
{
	const uint16_t n = rw((uint16_t)(T.r[6] + 2));
	T.r[0] = n;
	fl::mov(n);
	tick(0xD34C, 27);
	if (!n)
	{
		tick(0xD35C, 12);                              // BEQ
		rts();
		return;
	}
	tick(0xD34E, 12);
	const uint16_t a = (uint16_t)(0x0E7D + n);
	pokeb(a, fl::incb(peekb(a)));
	tick(0xD352, 30);
	const uint8_t total = peekb(0x0E93);
	T.r[0] = (uint16_t)(int16_t)(int8_t)total;
	fl::movb(total);
	tick(0xD356, 24);
	T.r[0] = fl::inc(T.r[0]);
	tick(0xD358, 12);
	pokeb(0x0E93, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0xD35C, 27);
	rts();
}

// F:E238: reset the skater's kick/jump state
void reset_skater_state()
{
	pokeb(0x01EB, 0);
	fl::clr();
	tick(0xE23C, 27);
	pokeb(0x0330, 0);
	fl::clr();
	tick(0xE240, 27);
	pokeb(0x0320, 1);
	fl::movb(0x01);
	tick(0xE246, 33);
	pokeb(0x0771, 0);
	fl::clr();
	tick(0xE24A, 27);
	pokeb(0x0791, 0);
	fl::clr();
	tick(0xE24E, 27);
	pokeb(0x0772, 0);
	fl::clr();
	tick(0xE252, 27);
	rts();
}

}

LOGIC_TABLE(small,
	asm_routine("B3", 0x7AD2, "save_frame_counter", save_frame_counter, 0),
	asm_routine("F", 0x8E6A, "set_flag_01a4", set_flag_01a4, 0),
	asm_routine("F", 0x8E72, "clear_flag_01a4", clear_flag_01a4, 0),
	asm_routine("F", 0x917C, "feed_watchdog", feed_watchdog, 0),
	asm_routine("F", 0xE02E, "set_0eb4", set_0eb4, 0),
	asm_routine("F", 0x8282, "irq_disable_bit2", irq_disable_bit2, 0),
	asm_routine("F", 0xE6F0, "object1_screen_x", object1_screen_x, 0),
	asm_routine("F", 0x8068, "select_video_bank0", select_video_bank0, 0),
	asm_routine("F", 0x8096, "reselect_video_bank", reselect_video_bank, 0),
	asm_routine("F", 0x80C8, "add32", add32, 0),
	asm_routine("F", 0x80D0, "sub32", sub32, 0),
	asm_routine("F", 0x8270, "set_32v_vectors", set_32v_vectors, 0),
	asm_routine("F", 0xBA5E, "clear_object_dy", clear_object_dy, 0),
	asm_routine("F", 0xE222, "state1_to_8", state1_to_8, 0),
	asm_routine("F", 0xD348, "count_event", count_event, 0),
	asm_routine("F", 0xE238, "reset_skater_state", reset_skater_state, 0),
)

}
