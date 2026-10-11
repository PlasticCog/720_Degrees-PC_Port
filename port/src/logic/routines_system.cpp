// Rewritten routines: sound commands, interrupt setup and screen helpers.  They talk to the
// sound CPU, the interrupts and the video hardware, so they follow the original instruction
// by instruction: each read and write happens where the original's does and after every
// instruction its cycles are charged and interrupts may come in (tick at the next
// instruction's address; see routines_core.cpp).
#include "logic.h"

namespace logic {
namespace steps {                                      // defined in routines_core.cpp
void rts();
void jsr(uint16_t ret, uint16_t addr);
void c_save(int n, uint16_t body);
void c_restore(int n);
void vector_loops(uint16_t pc, uint16_t h1, uint16_t h2);
}

namespace {

using namespace t11rt;
using namespace steps;

uint16_t sext8(uint8_t v) { return (uint16_t)(int16_t)(int8_t)v; }

// A7:4314 (assembly): send the next command of the sound queue (ring 0x0198, read index 0x01A0,
// write index 0x01A1) when the sound CPU has taken the last one (IN0 bit 0x20 clear).
// Commands 0x48 / 0x49 also count up / down 0x02BB.
void send_queued_sound()
{
	T.r[0] = sext8(peekb(0x01A0));
	fl::movb((uint8_t)T.r[0]);
	tick(0x4318, 24);
	T.r[1] = sext8(peekb(0x01A1));
	fl::movb((uint8_t)T.r[1]);
	tick(0x431C, 24);
	fl::cmp(T.r[0], T.r[1]);
	tick(0x431E, 12);
	if (T.r[0] == T.r[1])
	{
		tick(0x4358, 12);                              // BEQ
		rts();
		return;
	}
	tick(0x4320, 12);
	const uint16_t busy = peek(0x1800) & 0x0020;
	fl::mov(busy);
	tick(0x4326, 30);                                  // BIT #0x20,@#IN0
	if (busy)
	{
		tick(0x4358, 12);                              // BNE
		rts();
		return;
	}
	tick(0x4328, 12);
	T.r[1] = 0;
	fl::clr();
	tick(0x432A, 12);
	T.r[1] = peekb((uint16_t)(0x0198 + T.r[0]));
	fl::logicb((uint8_t)T.r[1]);
	tick(0x432E, 27);
	fl::cmp(T.r[1], 0x0048);
	tick(0x4332, 18);                                  // CMP R1,#0x48
	if (T.r[1] == 0x48)
	{
		tick(0x4334, 12);
		pokeb(0x02BB, fl::incb(peekb(0x02BB)));
		tick(0x4338, 27);
		tick(0x4344, 12);                              // BR
	}
	else
	{
		tick(0x433A, 12);                              // BNE
		fl::cmp(T.r[1], 0x0049);
		tick(0x433E, 18);                              // CMP R1,#0x49
		if (T.r[1] == 0x49)
		{
			tick(0x4340, 12);
			pokeb(0x02BB, fl::decb(peekb(0x02BB)));
			tick(0x4344, 27);
		}
		else
			tick(0x4344, 12);                          // BNE
	}
	pokeb(0x1680, (uint8_t)T.r[1]);
	fl::movb((uint8_t)T.r[1]);
	tick(0x4348, 27);                                  // MOVB R1,@#SOUND_CMD
	T.r[0] = fl::inc(T.r[0]);
	tick(0x434A, 12);
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0x434C, 12);
	T.r[1] = fl::logic((uint16_t)(T.r[1] & 7));
	tick(0x4350, 18);
	pokeb(0x01A0, 0);
	fl::clr();
	tick(0x4354, 27);
	const uint8_t index = (uint8_t)(peekb(0x01A0) | T.r[1]);
	pokeb(0x01A0, index);
	f_nzvc(nzb(index));                                // C is clear from the CLRB
	tick(0x4358, 27);
	rts();
}

// A5:5CC0  queue_effect_sound(int n): sound effect n (table 0x6567, bank B 0x0400) goes to the
// first free effect channel (0x01A6, then 0x01A7); the command comes from table 0x5B0B or
// 0x5B11.  Nothing happens when both channels are busy.  Bank B stays at 0x0400.
uint16_t queue_effect_sound(const Args &a)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x5CC2, 24);
	T.r[0] = 0x0400;
	fl::mov(T.r[0]);
	tick(0x5CC6, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x5CCA, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x5CCE, 27);
	T.r[0] = a.u(0);
	fl::mov(T.r[0]);
	tick(0x5CD2, 27);
	T.r[1] = 0;
	fl::clr();
	tick(0x5CD4, 12);
	T.r[1] = peekb((uint16_t)(0x6567 + T.r[0]));
	fl::logicb((uint8_t)T.r[1]);
	tick(0x5CD8, 27);
	fl::tst(T.r[1]);
	tick(0x5CDA, 12);                                  // TST R1
	if (T.r[1] == 0)
		tick(0x5D04, 12);                              // BEQ
	else
	{
		tick(0x5CDC, 12);
		bool queue = true;
		const bool busy_a = peekb(0x01A6) != 0;
		fl::tstb(peekb(0x01A6));
		tick(0x5CE0, 24);
		if (!busy_a)
		{
			tick(0x5CE2, 12);
			pokeb(0x01A6, (uint8_t)T.r[1]);
			fl::movb((uint8_t)T.r[1]);
			tick(0x5CE6, 27);
			push(0);
			fl::clr();
			tick(0x5CE8, 24);
			pokeb(T.r[6], peekb((uint16_t)(0x5B0B + T.r[1])));
			fl::movb(peekb(T.r[6]));
			tick(0x5CEC, 36);
			tick(0x5CFE, 12);                          // BR
		}
		else
		{
			tick(0x5CEE, 12);                          // BNE
			const bool busy_b = peekb(0x01A7) != 0;
			fl::tstb(peekb(0x01A7));
			tick(0x5CF2, 24);
			if (busy_b)
			{
				tick(0x5D04, 12);                      // BNE
				queue = false;
			}
			else
			{
				tick(0x5CF4, 12);
				pokeb(0x01A7, (uint8_t)T.r[1]);
				fl::movb((uint8_t)T.r[1]);
				tick(0x5CF8, 27);
				push(0);
				fl::clr();
				tick(0x5CFA, 24);
				pokeb(T.r[6], peekb((uint16_t)(0x5B11 + T.r[1])));
				fl::movb(peekb(T.r[6]));
				tick(0x5CFE, 36);
			}
		}
		if (queue)
		{
			jsr(0x5D02, 0x8E78);                       // queue_sound
			pop();
			fl::tst(peek((uint16_t)(T.r[6] - 2)));
			tick(0x5D04, 18);                          // TST (SP)+
		}
	}
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x5D06, 18);
	rts();
	return T.r[0];
}

// A5:5D86 (assembly): every 16 frames step the sound level 0x01E9 towards the target 0x01E8
// and queue its command (table 0x5D82); on the other frames a reached target is cleared
// (0x01E8 = 0xFF, idle).  A target equal to the level on a 16th frame still steps down.
void step_sound_level()
{
	const uint8_t target = peekb(0x01E8);
	T.r[0] = sext8(target);
	fl::movb(target);
	tick(0x5D8A, 24);
	if (target & 0x80)
	{
		tick(0x5DCC, 12);                              // BLT
		rts();
		return;
	}
	tick(0x5D8C, 12);
	const uint16_t phase = peek(0x01C4) & 0x000F;
	fl::mov(phase);
	tick(0x5D92, 30);                                  // BIT #0x0F,@#0x01C4
	if (phase)
	{
		tick(0x5DBC, 12);                              // BNE
		T.r[1] = 0;
		fl::clr();
		tick(0x5DBE, 12);
		T.r[1] = peekb(0x01E9);
		fl::logicb((uint8_t)T.r[1]);
		tick(0x5DC2, 24);
		fl::cmp(T.r[0], T.r[1]);
		tick(0x5DC4, 12);
		if (T.r[0] == T.r[1])
		{
			tick(0x5DC6, 12);
			pokeb(0x01E8, 0xFF);
			fl::movb(0xFF);
			tick(0x5DCC, 33);
		}
		else
			tick(0x5DCC, 12);                          // BNE
		rts();
		return;
	}
	tick(0x5D94, 12);
	T.r[1] = 0;
	fl::clr();
	tick(0x5D96, 12);
	T.r[1] = peekb(0x01E9);
	fl::logicb((uint8_t)T.r[1]);
	tick(0x5D9A, 24);
	fl::cmp(T.r[0], T.r[1]);
	tick(0x5D9C, 12);
	if ((int16_t)T.r[0] > (int16_t)T.r[1])
	{
		tick(0x5D9E, 12);
		pokeb(0x01E9, fl::incb(peekb(0x01E9)));
		tick(0x5DA2, 27);
		tick(0x5DA8, 12);                              // BR
	}
	else
	{
		tick(0x5DA4, 12);                              // BLE
		pokeb(0x01E9, fl::decb(peekb(0x01E9)));
		tick(0x5DA8, 27);
	}
	T.r[0] = 0;
	fl::clr();
	tick(0x5DAA, 12);
	T.r[0] = peekb(0x01E9);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x5DAE, 24);
	push(0);
	fl::clr();
	tick(0x5DB0, 24);
	pokeb(T.r[6], rb((uint16_t)(0x5D82 + T.r[0])));
	fl::movb(peekb(T.r[6]));
	tick(0x5DB4, 36);
	jsr(0x5DB8, 0x8E78);                               // queue_sound
	fl::tst(pop());
	tick(0x5DBA, 18);                                  // TST (SP)+
	rts();
}

// F:81AA / F:820C: install the interrupt handlers: the eight vectors listed at 0xFC4E get
// handler h1 (PSW 0xE0), the four 32V vectors listed at 0xFC5E handler h2 (PSW 0xC0); the
// interrupt enables are off meanwhile and end as 0x000C.  The start of both: MOV R1,-(SP),
// the interrupts off, then the vectors.  Returns the last vector address.
uint16_t install_start(uint16_t pc, uint16_t h1, uint16_t h2)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick((uint16_t)(pc + 2), 24);
	poke(0x018C, 0);
	fl::clr();
	tick((uint16_t)(pc + 6), 27);
	poke(0x1600, 0);
	fl::clr();
	tick((uint16_t)(pc + 10), 27);
	pokeb(0x1800, 0);
	fl::clr();
	tick((uint16_t)(pc + 14), 27);
	vector_loops((uint16_t)(pc + 14), h1, h2);
	return T.r[0];
}

uint16_t install_interrupts_a(const Args &)
{
	const uint16_t r0 = install_start(0x81AA, 0x84D4, 0xB66E);
	pokeb(0x0196, 0);
	fl::clr();
	tick(0x81F0, 27);
	pokeb(0x0191, 0x4D);
	fl::movb(0x4D);
	tick(0x81F6, 33);
	pokeb(0x0197, 1);
	fl::movb(0x01);
	tick(0x81FC, 33);
	poke(0x018C, 0x000C);
	fl::mov(0x000C);
	tick(0x8202, 33);
	poke(0x1600, 0x000C);                              // interrupts on
	fl::mov(0x000C);
	tick(0x8208, 33);
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x820A, 18);
	rts();
	return r0;
}

uint16_t install_interrupts_b(const Args &)
{
	install_start(0x820C, 0x899C, 0xB9D4);
	pokeb(0x0196, 0);
	fl::clr();
	tick(0x8252, 27);
	pokeb(0x0197, 0);
	fl::clr();
	tick(0x8256, 27);
	pokeb(0x0191, 0x4D);
	fl::movb(0x4D);
	tick(0x825C, 33);
	poke(0x018C, 0x000C);
	fl::mov(0x000C);
	tick(0x8262, 33);
	poke(0x1600, 0x000C);                              // interrupts on
	fl::mov(0x000C);
	tick(0x8268, 33);
	jsr(0x826C, 0xBA5E);                               // clear_object_dy
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x826E, 18);
	rts();
	return T.r[0];
}

// F:DB00 (assembly): playfield palette 0x1100-0x119F = the 80 colours at 0x7DAC (bank B 0x1C00)
// minus sext(0x0EA7), for fading; the banks are restored afterwards.  Step by step.
void fade_playfield_palette()
{
	T.r[5] = peek(0x01D2);
	fl::mov(T.r[5]);
	tick(0xDB04, 24);
	push(peek(0x01D4));
	fl::mov(peek(T.r[6]));
	tick(0xDB08, 36);
	T.r[0] = 0x1800;
	fl::mov(T.r[0]);
	tick(0xDB0C, 18);
	poke(0x01D2, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDB10, 27);
	poke(0x1400, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDB14, 27);
	T.r[0] = 0x1C00;
	fl::mov(T.r[0]);
	tick(0xDB18, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDB1C, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDB20, 27);
	T.r[1] = sext8(peekb(0x0EA7));
	fl::movb((uint8_t)T.r[1]);
	tick(0xDB24, 24);
	T.r[3] = 0x7E4C;
	fl::mov(T.r[3]);
	tick(0xDB28, 18);
	T.r[2] = 0x11A0;
	fl::mov(T.r[2]);
	tick(0xDB2C, 18);
	T.r[4] = 0x0050;
	fl::mov(T.r[4]);
	tick(0xDB30, 18);
	for (;;)
	{
		T.r[3] -= 2;
		T.r[0] = peek(T.r[3]);
		fl::mov(T.r[0]);
		tick(0xDB32, 21);
		T.r[0] = fl::sub(T.r[0], T.r[1]);
		tick(0xDB34, 12);
		T.r[2] -= 2;
		poke(T.r[2], T.r[0]);
		fl::mov(T.r[0]);
		tick(0xDB36, 24);
		if (--T.r[4] == 0) break;
		tick(0xDB30, 18);                              // SOB
	}
	tick(0xDB38, 18);
	poke(0x01D2, T.r[5]);
	fl::mov(T.r[5]);
	tick(0xDB3C, 27);
	const uint16_t saved_b = pop();
	poke(0x01D4, saved_b);
	fl::mov(peek(0x01D4));
	tick(0xDB40, 33);
	poke(0x1400, peek(0x01D2));
	fl::mov(peek(0x01D2));
	tick(0xDB46, 39);
	poke(0x1402, peek(0x01D4));
	fl::mov(saved_b);                                  // C from the last SUB
	tick(0xDB4C, 39);
	rts();
}

// F:8C12 (assembly): write character R0 with the small font at the text pointer 0x0186 and
// advance it; 0 only advances.  Codes up to 0x40 map through the table at 0x8CA6 (negative
// entries are negated), higher codes are code + 0x99; the colour bits 0x0188 are added.
// Step by step (the play interrupt draws with the same text pointer).
void put_small_char()
{
	fl::tst(T.r[0]);
	tick(0x8C14, 12);                                  // TST R0
	if (T.r[0] == 0)
	{
		tick(0x8C16, 12);
		poke(0x0186, fl::add(peek(0x0186), 2));
		tick(0x8C1C, 33);
		rts();
		return;
	}
	tick(0x8C1E, 12);                                  // BNE
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x8C20, 24);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x007F));
	tick(0x8C24, 18);
	fl::cmp(T.r[0], 0x0000);
	tick(0x8C28, 18);                                  // CMP R0,#0
	bool carry = false;                                // C as the end leaves it
	if (T.r[0] == 0)
		tick(0x8C48, 12);                              // BEQ
	else
	{
		tick(0x8C2A, 12);
		fl::cmp(T.r[0], 0x0040);
		tick(0x8C2E, 18);                              // CMP R0,#0x40
		if ((int16_t)T.r[0] > 0x40)
		{
			tick(0x8C40, 12);                          // BGT
			T.r[0] = fl::add(T.r[0], 0x0099);
			tick(0x8C44, 18);
		}
		else
		{
			tick(0x8C30, 12);
			T.r[0] = fl::sub(T.r[0], 0x0020);
			tick(0x8C34, 18);
			carry = (T.r[0] & 0x8000) != 0;
			T.r[0] = fl::asl(T.r[0]);
			tick(0x8C36, 12);                          // ASL R0
			T.r[0] = peek((uint16_t)(0x8CA6 + T.r[0]));
			fl::mov(T.r[0]);
			tick(0x8C3A, 27);
			if (T.r[0] & 0x8000)
			{
				tick(0x8C3C, 12);
				T.r[0] = (uint16_t)-T.r[0];
				carry = true;                          // NEG of a non-zero value
				fl::neg((uint16_t)(-T.r[0]));
				tick(0x8C3E, 12);
				tick(0x8C44, 12);                      // BR
			}
			else
				tick(0x8C44, 12);                      // BPL
		}
		T.r[0] = fl::logic((uint16_t)(T.r[0] | peek(0x0188)));
		tick(0x8C48, 24);
	}
	T.r[1] = peek(0x0186);
	fl::mov(T.r[1]);
	tick(0x8C4C, 24);
	ww(T.r[1], T.r[0]);
	T.r[1] += 2;
	fl::mov(T.r[0]);
	tick(0x8C4E, 21);
	poke(0x0186, T.r[1]);
	fl::mov(T.r[1]);
	tick(0x8C52, 27);
	T.r[1] = pop();
	f_nzvc(nzw(T.r[1]) | (carry ? CF : 0));             // MOV (SP)+,R1
	tick(0x8C54, 18);
	rts();
}

}

LOGIC_TABLE(system,
	asm_routine("A7", 0x4314, "send_queued_sound", send_queued_sound, 0),
	c_routine("A5", 0x5CC0, "queue_effect_sound", queue_effect_sound, 0, 0),
	asm_routine("A5", 0x5D86, "step_sound_level", step_sound_level, 0),
	c_routine("F", 0x81AA, "install_interrupts_a", install_interrupts_a, 0, 0),
	c_routine("F", 0x820C, "install_interrupts_b", install_interrupts_b, 0, 0),
	asm_routine("F", 0xDB00, "fade_playfield_palette", fade_playfield_palette, 0),
	asm_routine("F", 0x8C12, "put_small_char", put_small_char, 0),
)

}
