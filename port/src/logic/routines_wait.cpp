// Rewritten routines that wait: for the vertical blank, for a palette fade, for the sound CPU.
// They poll like the original (see tick in logic.h) and charge the original's cycles on every
// path themselves; the cost in the table is only the final return.
#include "logic.h"

namespace logic {
namespace {

using namespace t11rt;

uint16_t sext8(uint8_t v) { return (uint16_t)(int16_t)(int8_t)v; }

constexpr int WATCHDOG = 30 + 27 + 21;                  // JSR F:917C, CLRB 0x1800, RTS

// F:8D50 (assembly): wait for the next vertical blank: clear 0x0192 and poll until the
// interrupt sets it again.  R0 counts the passes.
void wait_vblank()
{
	pokeb(0x0192, 0);
	tick(0x8D54, 27);
	for (;;)
	{
		const uint8_t v = peekb(0x0192);
		if (v)
		{
			T.icount -= 24 + 12;
			fl::tstb(v);
			return;
		}
		T.r[0]++;
		tick(0x8D54, 24 + 12 + 12 + 12);
	}
}

// F:DC24 (assembly)  wait_fade(int level): unless the palette level 0x0EA7 already equals level,
// make it the fade target (0x0EA8) and wait until the fade interrupt marks it reached
// (0x0EA8 negative).  R0 ends as the last value read, sign-extended.
void wait_fade()
{
	const uint16_t level = rw((uint16_t)(T.r[6] + 2));
	T.r[0] = sext8(peekb(0x0EA7));
	fl::cmp(T.r[0], level);
	if (T.r[0] == level)
	{
		T.icount -= 24 + 27 + 12;
		return;
	}
	pokeb(0x0EA8, (uint8_t)level);
	tick(0xDC34, 24 + 27 + 12 + 42);
	for (;;)
	{
		const uint8_t v = peekb(0x0EA8);
		T.r[0] = sext8(v);
		if (v & 0x80)
		{
			T.icount -= 24 + 12;
			fl::movb(v);
			return;
		}
		tick(0xDC34, 24 + 12);
	}
}

// F:8EA2  send_sound_command(int cmd): write cmd to the sound CPU once its command buffer is
// free (IN0 bit 0x20 clear), feeding the watchdog meanwhile.  After 64999 busy polls the
// sound CPU is marked dead (0x01A2 = 5); nothing is sent while 0x01A2 is set.
uint16_t send_sound_command(const Args &a)
{
	T.icount -= 24 + 24 + 12;                          // save R1, test 0x01A2
	if (peekb(0x01A2)) return T.r[0];
	tick(0x8EAE, 18);
	for (uint16_t n = 0xFDE8;;)
	{
		if (!(peek(0x1800) & 0x0020))
		{
			T.icount -= 30 + 12 + 42;
			poke(0x1680, a.u(0));
			return T.r[0];
		}
		if (--n == 0)
		{
			T.icount -= 30 + 12 + 12 + 12 + 33 + 12;
			pokeb(0x01A2, 5);
			return T.r[0];
		}
		wb(0x1800, 0);                                 // F:917C feed the watchdog
		tick(0x8EAE, 30 + 12 + 12 + 12 + WATCHDOG + 12);
	}
}

// F:8ED2  read_sound_response(): wait for a byte from the sound CPU (IN0 bit 0x10) and return
// it, feeding the watchdog meanwhile.  Without an answer for 3 x 65535 polls the sound CPU is
// marked dead (0x01A2 = 2) and 0 is returned, as it is while 0x01A2 is set.
uint16_t read_sound_response(const Args &)
{
	T.icount -= 30 + 129 + 12 + 24 + 12;               // register save, CLR R1, test 0x01A2
	if (peekb(0x01A2))
	{
		T.icount -= 12;
		return 0;
	}
	tick(0x8EE2, 18);
	uint16_t r1 = 0, r2 = 3;
	for (;;)
	{
		if (peek(0x1800) & 0x0010)
		{
			T.icount -= 30 + 12 + 12 + 24 + 12;
			return peekb(0x1C00);
		}
		wb(0x1800, 0);                                 // F:917C feed the watchdog
		int pass = 30 + 12 + WATCHDOG + 12 + 18 + 12;
		if (--r1 == 1)
		{
			pass += 18;
			if (--r2 == 0)
			{
				T.icount -= pass + 33 + 12;
				pokeb(0x01A2, 2);
				return 0;
			}
		}
		tick(0x8EE2, pass);
	}
}

}

LOGIC_TABLE(wait,
	wait_routine("F", 0x8D50, "wait_vblank", wait_vblank, 21),
	wait_routine("F", 0xDC24, "wait_fade", wait_fade, 21),
	wait_c_routine("F", 0x8EA2, "send_sound_command", send_sound_command, 18 + 21),
	wait_c_routine("F", 0x8ED2, "read_sound_response", read_sound_response, 21 + 12 + 18 + 18 + 18 + 21),
)

}
