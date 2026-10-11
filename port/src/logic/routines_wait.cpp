// Rewritten routines that wait: for the vertical blank, for a palette fade, for the sound CPU.
// They poll like the original (see tick in logic.h).  Their timing decides when interrupts
// and the sound CPU see what they do, so they follow the original instruction by instruction:
// each read and write happens where the original's does, the stack and registers are as the
// original's, and after every instruction its cycles are charged and interrupts let in
// (tick at the next instruction's address; step helpers in routines_core.cpp).
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

// F:8D50 (assembly): wait for the next vertical blank: clear 0x0192 and poll until the
// interrupt sets it again.  R0 counts the passes.
void wait_vblank()
{
	pokeb(0x0192, 0);
	fl::clr();
	tick(0x8D54, 27);                                  // CLRB @#0x0192
	for (;;)
	{
		const uint8_t v = peekb(0x0192);
		fl::tstb(v);
		tick(0x8D58, 24);                              // TSTB @#0x0192
		if (v)
		{
			tick(0x8D5E, 12);                          // BNE
			rts();
			return;
		}
		tick(0x8D5A, 12);
		T.r[0] = fl::inc(T.r[0]);
		tick(0x8D5C, 12);                              // INC R0
		tick(0x8D54, 12);                              // BR
	}
}

// F:DC24 (assembly)  wait_fade(int level): unless the palette level 0x0EA7 already equals level,
// make it the fade target (0x0EA8) and wait until the fade interrupt marks it reached
// (0x0EA8 negative).  R0 ends as the last value read, sign-extended.
void wait_fade()
{
	T.r[0] = sext8(peekb(0x0EA7));
	fl::movb((uint8_t)T.r[0]);
	tick(0xDC28, 24);                                  // MOVB @#0x0EA7,R0
	const uint16_t level = rw((uint16_t)(T.r[6] + 2));
	fl::cmp(T.r[0], level);
	tick(0xDC2C, 27);                                  // CMP R0,2(SP)
	if (T.r[0] == level)
	{
		tick(0xDC3A, 12);                              // BEQ
		rts();
		return;
	}
	tick(0xDC2E, 12);
	const uint8_t target = rb((uint16_t)(T.r[6] + 2));
	pokeb(0x0EA8, target);
	fl::movb(target);
	tick(0xDC34, 42);                                  // MOVB 2(SP),@#0x0EA8
	for (;;)
	{
		const uint8_t v = peekb(0x0EA8);
		T.r[0] = sext8(v);
		fl::movb(v);
		tick(0xDC38, 24);                              // MOVB @#0x0EA8,R0
		if (v & 0x80)
		{
			tick(0xDC3A, 12);                          // BGE
			rts();
			return;
		}
		tick(0xDC34, 12);
	}
}

// F:8EA2  send_sound_command(int cmd): write cmd to the sound CPU once its command buffer is
// free (IN0 bit 0x20 clear), feeding the watchdog meanwhile.  After 64999 busy polls the
// sound CPU is marked dead (0x01A2 = 5); nothing is sent while 0x01A2 is set.
uint16_t send_sound_command(const Args &a)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x8EA4, 24);                                  // MOV R1,-(SP)
	const bool dead = peekb(0x01A2) != 0;
	fl::tstb(peekb(0x01A2));
	tick(0x8EA8, 24);                                  // TSTB @#0x01A2
	if (dead)
		tick(0x8ECE, 12);                              // BNE
	else
	{
		tick(0x8EAA, 12);
		T.r[1] = 0xFDE8;
		fl::mov(T.r[1]);
		tick(0x8EAE, 18);                              // MOV #0xFDE8,R1
		for (;;)
		{
			const bool busy = (peek(0x1800) & 0x0020) != 0;
			fl::logic((uint16_t)(0x0020 & peek(0x1800)));
			tick(0x8EB4, 30);                          // BIT #0x20,@#IN0
			if (!busy)
			{
				tick(0x8EC8, 12);                      // BEQ
				poke(0x1680, a.u(0));
				fl::mov(peek((uint16_t)(T.r[6] + 4)));
				tick(0x8ECE, 42);                      // MOV 4(SP),@#SOUND_CMD
				break;
			}
			tick(0x8EB6, 12);
			T.r[1] = fl::dec(T.r[1]);
			tick(0x8EB8, 12);                          // DEC R1
			if (T.r[1] == 0)
			{
				tick(0x8EBA, 12);
				pokeb(0x01A2, 5);
				fl::movb(0x05);
				tick(0x8EC0, 33);                      // MOVB #5,@#0x01A2
				tick(0x8ECE, 12);                      // BR
				break;
			}
			tick(0x8EC2, 12);                          // BNE
			jsr(0x8EC6, 0x917C);                       // feed_watchdog
			tick(0x8EAE, 12);                          // BR
		}
	}
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x8ED0, 18);                                  // MOV (SP)+,R1
	rts();
	return T.r[0];
}

// F:8ED2  read_sound_response(): wait for a byte from the sound CPU (IN0 bit 0x10) and return
// it, feeding the watchdog meanwhile.  Without an answer for 3 x 65535 polls the sound CPU is
// marked dead (0x01A2 = 2) and 0 is returned, as it is while 0x01A2 is set.
uint16_t read_sound_response(const Args &)
{
	c_save(2, 0x8ED6);
	T.r[1] = 0;
	fl::clr();
	tick(0x8ED8, 12);                                  // CLR R1
	const bool dead = peekb(0x01A2) != 0;
	fl::tstb(peekb(0x01A2));
	tick(0x8EDC, 24);                                  // TSTB @#0x01A2
	if (dead)
		tick(0x8F08, 12);                              // BNE
	else
	{
		tick(0x8EDE, 12);
		T.r[2] = 3;
		fl::mov(T.r[2]);
		tick(0x8EE2, 18);                              // MOV #3,R2
		for (;;)
		{
			const bool ready = (peek(0x1800) & 0x0010) != 0;
			fl::logic((uint16_t)(0x0010 & peek(0x1800)));
			tick(0x8EE8, 30);                          // BIT #0x10,@#IN0
			if (ready)
			{
				tick(0x8EEA, 12);
				T.r[3] = 0;
				fl::clr();
				tick(0x8EEC, 12);                      // CLR R3
				T.r[3] = peekb(0x1C00);
				fl::logicb((uint8_t)T.r[3]);
				tick(0x8EF0, 24);                      // BISB @#SOUND_RESPONSE,R3
				T.r[0] = T.r[3];
				fl::mov(T.r[0]);
				tick(0x8EF2, 12);                      // MOV R3,R0
				c_restore(2);
				return T.r[0];
			}
			tick(0x8EF4, 12);                          // BEQ
			jsr(0x8EF8, 0x917C);                       // feed_watchdog
			T.r[1] = fl::dec(T.r[1]);
			tick(0x8EFA, 12);                          // DEC R1
			fl::cmp(T.r[1], 0x0001);
			tick(0x8EFE, 18);                          // CMP R1,#1
			if (T.r[1] != 1)
			{
				tick(0x8EE2, 12);                      // BNE
				continue;
			}
			tick(0x8F00, 12);
			if (--T.r[2] != 0)
			{
				tick(0x8EE2, 18);                      // SOB
				continue;
			}
			tick(0x8F02, 18);
			pokeb(0x01A2, 2);
			fl::movb(0x02);
			tick(0x8F08, 33);                          // MOVB #2,@#0x01A2
			break;
		}
	}
	T.r[0] = 0;
	fl::clr();
	tick(0x8F0A, 12);                                  // CLR R0
	c_restore(2);
	return 0;
}

}

LOGIC_TABLE(wait,
	wait_routine("F", 0x8D50, "wait_vblank", wait_vblank, 0),
	wait_routine("F", 0xDC24, "wait_fade", wait_fade, 0),
	wait_c_routine("F", 0x8EA2, "send_sound_command", send_sound_command, 0),
	wait_c_routine("F", 0x8ED2, "read_sound_response", read_sound_response, 0),
)

}
