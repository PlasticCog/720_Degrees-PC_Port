// Rewritten routines: small hardware and state helpers.  These were written in assembly in
// the original (register interface), so registers and condition codes are reproduced too.
#include "logic.h"

namespace logic {
namespace {

using namespace t11rt;

// B3:7AD2: remember the frame counter (0x01C4) in 0x0FB0
void save_frame_counter()
{
	const uint16_t v = peek(0x01C4);
	poke(0x0FB0, v);
	fl::mov(v);
}

// F:8E6A / F:8E72: flag at 0x01A4
void set_flag_01a4() { pokeb(0x01A4, 1); fl::movb(1); }
void clear_flag_01a4() { pokeb(0x01A4, 0); fl::clr(); }

// F:917C: feed the watchdog
void feed_watchdog() { wb(0x1800, 0); fl::clr(); }

// F:E02E: 0x0EB4 = 120
void set_0eb4() { poke(0x0EB4, 0x0078); fl::mov(0x0078); }

// F:8282: turn off interrupt source bit 2 in the shadow mask (0x018C) and the hardware
void irq_disable_bit2()
{
	const uint16_t v = peek(0x018C) & ~0x0004;
	poke(0x018C, v);
	ww(0x1600, peek(0x018C));
	fl::mov(v);
}

// F:E6F0: screen x of object 1 = its world x - horizontal scroll
void object1_screen_x()
{
	poke(0x0632, peek(0x06F2));
	poke(0x0632, fl::sub(peek(0x0632), peek(0x01A8)));
}

// F:8068: select video RAM window 0 (slapstic: arm at 0x8000, select at 0x8030)
void select_video_bank0()
{
	poke(0x01D0, 0x0030);
	rw(0x8000);
	fl::tst(rw(0x8030));
}

// F:8096: select the video RAM window recorded in 0x01D0 again
void reselect_video_bank()
{
	T.r[0] = peek(0x01D0);
	rw(0x8000);
	fl::tst(rw((uint16_t)(0x8000 + T.r[0])));
}

// F:80C8: add R3:R4 to the long at (R1 = high word, R2 = low word)
void add32()
{
	poke(T.r[2], fl::add(peek(T.r[2]), T.r[4]));
	poke(T.r[1], fl::adc(peek(T.r[1])));
	poke(T.r[1], fl::add(peek(T.r[1]), T.r[3]));
}

// F:80D0: subtract R3:R4 from the long at (R1 = high word, R2 = low word)
void sub32()
{
	poke(T.r[2], fl::sub(peek(T.r[2]), T.r[4]));
	poke(T.r[1], fl::sbc(peek(T.r[1])));
	poke(T.r[1], fl::sub(peek(T.r[1]), T.r[3]));
}

// F:8270: point the four 32V interrupt vectors (addresses listed at 0xFC5E) at 0xB8FC
void set_32v_vectors()
{
	for (int i = 6; i >= 0; i -= 2)
	{
		const uint16_t v = 0xB8FC;
		ww(peek((uint16_t)(0xFC5E + i)), v);
		fl::mov(v);
	}
	T.r[0] = fl::sub(0, 2);
}

// F:BA5E: clear the per-object screen-y deltas (0x05F0, 32 words)
void clear_object_dy()
{
	for (int i = 0x3E; i >= 0; i -= 2) poke((uint16_t)(0x05F0 + i), 0);
	T.r[0] = fl::sub(0, 2);
}

// F:E222: game state 1 moves to state 8 with a one-second delay (0x0F22 = 60)
void state1_to_8()
{
	const uint8_t st = peekb(0x018F);
	fl::cmpb(st, 1);
	if (st != 1) return;
	pokeb(0x018F, 8);
	fl::movb(8);
	poke(0x0F22, 0x003C);
	fl::mov(0x003C);
}

// F:D348: count(n): for n != 0 increment the byte counter 0x0E7D+n and the total 0x0E93
void count_event()
{
	const uint16_t n = rw((uint16_t)(T.r[6] + 2));
	T.r[0] = n;
	fl::mov(n);
	if (!n) return;
	const uint16_t a = (uint16_t)(0x0E7D + n);
	pokeb(a, fl::incb(peekb(a)));
	const uint8_t total = peekb(0x0E93);
	fl::movb(total);
	T.r[0] = fl::inc((uint16_t)(int16_t)(int8_t)total);
	pokeb(0x0E93, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
}

// F:E238: reset the skater's kick/jump state
void reset_skater_state()
{
	pokeb(0x01EB, 0);
	pokeb(0x0330, 0);
	pokeb(0x0320, 1);
	pokeb(0x0771, 0);
	pokeb(0x0791, 0);
	pokeb(0x0772, 0);
	fl::clr();
}

}

LOGIC_TABLE(small,
	asm_routine("B3", 0x7AD2, "save_frame_counter", save_frame_counter, 60),
	asm_routine("F", 0x8E6A, "set_flag_01a4", set_flag_01a4, 54),
	asm_routine("F", 0x8E72, "clear_flag_01a4", clear_flag_01a4, 48),
	asm_routine("F", 0x917C, "feed_watchdog", feed_watchdog, 48),
	asm_routine("F", 0xE02E, "set_0eb4", set_0eb4, 54),
	asm_routine("F", 0x8282, "irq_disable_bit2", irq_disable_bit2, 93),
	asm_routine("F", 0xE6F0, "object1_screen_x", object1_screen_x, 99),
	asm_routine("F", 0x8068, "select_video_bank0", select_video_bank0, 102),
	asm_routine("F", 0x8096, "reselect_video_bank", reselect_video_bank, 96),
	asm_routine("F", 0x80C8, "add32", add32, 84),
	asm_routine("F", 0x80D0, "sub32", sub32, 84),
	asm_routine("F", 0x8270, "set_32v_vectors", set_32v_vectors, 327),
	asm_routine("F", 0xBA5E, "clear_object_dy", clear_object_dy, 1959),
	asm_routine("F", 0xE222, "state1_to_8", state1_to_8, 129),
	asm_routine("F", 0xD348, "count_event", count_event, 153),
	asm_routine("F", 0xE238, "reset_skater_state", reset_skater_state, 189),
)

}
