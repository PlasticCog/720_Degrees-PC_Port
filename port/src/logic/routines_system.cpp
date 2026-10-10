// Rewritten routines: sound commands, interrupt setup and screen helpers.
#include "logic.h"

namespace logic {
namespace {

using namespace t11rt;

uint16_t sext8(uint8_t v) { return (uint16_t)(int16_t)(int8_t)v; }

// A7:4314 (assembly): send the next command of the sound queue (ring 0x0198, read index 0x01A0,
// write index 0x01A1) when the sound CPU has taken the last one (IN0 bit 0x20 clear).
// Commands 0x48 / 0x49 also count up / down 0x02BB.
void send_queued_sound()
{
	T.r[0] = sext8(peekb(0x01A0));
	T.r[1] = sext8(peekb(0x01A1));
	fl::cmp(T.r[0], T.r[1]);
	if (T.r[0] == T.r[1]) return;
	const uint16_t busy = peek(0x1800) & 0x0020;
	fl::mov(busy);
	if (busy) return;
	T.r[1] = peekb((uint16_t)(0x0198 + T.r[0]));
	if (T.r[1] == 0x48) pokeb(0x02BB, (uint8_t)(peekb(0x02BB) + 1));
	else if (T.r[1] == 0x49) pokeb(0x02BB, (uint8_t)(peekb(0x02BB) - 1));
	pokeb(0x1680, (uint8_t)T.r[1]);
	T.r[0] = (uint16_t)(T.r[0] + 1);
	T.r[1] = T.r[0] & 7;
	pokeb(0x01A0, (uint8_t)T.r[1]);
	f_nzvc(nzb(T.r[1]));
}

// A5:5CC0  queue_effect_sound(int n): sound effect n (table 0x6567, bank B 0x0400) goes to the
// first free effect channel (0x01A6, then 0x01A7); the command comes from table 0x5B0B or
// 0x5B11.  Nothing happens when both channels are busy.  Bank B stays at 0x0400.
uint16_t queue_effect_sound(const Args &a)
{
	poke(0x01D4, 0x0400);
	poke(0x1402, 0x0400);
	const uint16_t n = a.u(0);
	const uint8_t s = peekb((uint16_t)(0x6567 + n));
	if (!s) return n;
	uint8_t cmd;
	if (!peekb(0x01A6))
	{
		pokeb(0x01A6, s);
		cmd = peekb((uint16_t)(0x5B0B + s));
	}
	else if (!peekb(0x01A7))
	{
		pokeb(0x01A7, s);
		cmd = peekb((uint16_t)(0x5B11 + s));
	}
	else
		return n;
	T.r[0] = n;
	return call(0x8E78, {cmd});
}

// A5:5D86 (assembly): every 16 frames step the sound level 0x01E9 towards the target 0x01E8
// and queue its command (table 0x5D82); on the other frames a reached target is cleared
// (0x01E8 = 0xFF, idle).  A target equal to the level on a 16th frame still steps down.
void step_sound_level()
{
	const uint8_t target = peekb(0x01E8);
	T.r[0] = sext8(target);
	fl::movb(target);
	if (target & 0x80) return;
	T.r[1] = peekb(0x01E9);
	if (peek(0x01C4) & 0x000F)
	{
		fl::cmp(T.r[0], T.r[1]);
		if (T.r[0] == T.r[1])
		{
			pokeb(0x01E8, 0xFF);
			fl::movb(0xFF);
		}
		return;
	}
	const uint8_t level = peekb(0x01E9);
	pokeb(0x01E9, (uint8_t)((int16_t)T.r[0] > (int16_t)T.r[1] ? level + 1 : level - 1));
	T.r[0] = peekb(0x01E9);
	const uint16_t cmd = peekb((uint16_t)(0x5D82 + T.r[0]));
	T.r[0] = call(0x8E78, {cmd});
	fl::tst(cmd);
}

// F:81AA / F:820C: install the interrupt handlers: the eight vectors listed at 0xFC4E get
// handler h1 (PSW 0xE0), the four 32V vectors listed at 0xFC5E handler h2 (PSW 0xC0); the
// interrupt enables are off meanwhile and end as 0x000C.  Returns the last vector address.
uint16_t install_vectors(uint16_t h1, uint16_t h2)
{
	poke(0x018C, 0);
	poke(0x1600, 0);
	pokeb(0x1800, 0);
	for (int i = 14; i >= 0; i -= 2)
	{
		const uint16_t v = peek((uint16_t)(0xFC4E + i));
		poke(v, h1);
		poke((uint16_t)(v + 2), 0x00E0);
	}
	uint16_t v = 0;
	for (int i = 6; i >= 0; i -= 2)
	{
		v = peek((uint16_t)(0xFC5E + i));
		poke(v, h2);
		poke((uint16_t)(v + 2), 0x00C0);
	}
	return v;
}

uint16_t install_interrupts_a(const Args &)
{
	const uint16_t r0 = install_vectors(0x84D4, 0xB66E);
	pokeb(0x0196, 0);
	pokeb(0x0191, 0x4D);
	pokeb(0x0197, 1);
	poke(0x018C, 0x000C);
	poke(0x1600, 0x000C);
	return r0;
}

uint16_t install_interrupts_b(const Args &)
{
	install_vectors(0x899C, 0xB9D4);
	pokeb(0x0196, 0);
	pokeb(0x0197, 0);
	pokeb(0x0191, 0x4D);
	poke(0x018C, 0x000C);
	poke(0x1600, 0x000C);
	for (int i = 0x3E; i >= 0; i -= 2) poke((uint16_t)(0x05F0 + i), 0);   // F:BA5E clear_object_dy
	return 0xFFFE;
}

// F:DB00 (assembly): playfield palette 0x1100-0x119F = the 80 colours at 0x7DAC (bank B 0x1C00)
// minus sext(0x0EA7), for fading; the banks are restored afterwards
void fade_playfield_palette()
{
	T.r[5] = peek(0x01D2);
	const uint16_t saved_b = peek(0x01D4);
	poke(0x01D2, 0x1800);
	poke(0x1400, 0x1800);
	poke(0x01D4, 0x1C00);
	poke(0x1402, 0x1C00);
	T.r[1] = sext8(peekb(0x0EA7));
	uint16_t src = 0x7E4C, dst = 0x11A0;
	for (int n = 0; n < 0x50; n++)
	{
		src -= 2;
		dst -= 2;
		T.r[0] = fl::sub(peek(src), T.r[1]);
		poke(dst, T.r[0]);
	}
	T.r[2] = dst;
	T.r[3] = src;
	T.r[4] = 0;
	poke(0x01D2, T.r[5]);
	poke(0x01D4, saved_b);
	poke(0x1400, peek(0x01D2));
	poke(0x1402, peek(0x01D4));
	fl::mov(saved_b);
}

// F:8C12 (assembly): write character R0 with the small font at the text pointer 0x0186 and
// advance it; 0 only advances.  Codes up to 0x40 map through the table at 0x8CA6 (negative
// entries are negated), higher codes are code + 0x99; the colour bits 0x0188 are added.
void put_small_char()
{
	uint16_t c = T.r[0];
	if (c == 0)
	{
		poke(0x0186, fl::add(peek(0x0186), 2));
		return;
	}
	c &= 0x007F;
	bool carry = false;
	if (c != 0)
	{
		if ((int16_t)c > 0x40)
			c = (uint16_t)(c + 0x99);
		else
		{
			const uint16_t t = (uint16_t)(c - 0x20);
			carry = (t & 0x8000) != 0;                  // ASL
			c = peek((uint16_t)(0x8CA6 + (uint16_t)(t << 1)));
			if (c & 0x8000)
			{
				c = (uint16_t)-c;
				carry = true;                           // NEG of a non-zero value
			}
		}
		c |= peek(0x0188);
	}
	const uint16_t p = peek(0x0186);
	poke(p, c);
	poke(0x0186, (uint16_t)(p + 2));
	T.r[0] = c;
	f_nzvc(nzw(T.r[1]) | (carry ? CF : 0));             // MOV (SP)+,R1
}

}

LOGIC_TABLE(system,
	asm_routine("A7", 0x4314, "send_queued_sound", send_queued_sound, 116),
	c_routine("A5", 0x5CC0, "queue_effect_sound", queue_effect_sound, 0, 299),
	asm_routine("A5", 0x5D86, "step_sound_level", step_sound_level, 57),
	c_routine("F", 0x81AA, "install_interrupts_a", install_interrupts_a, 0, 1959),
	c_routine("F", 0x820C, "install_interrupts_b", install_interrupts_b, 0, 3942),
	asm_routine("F", 0xDB00, "fade_playfield_palette", fade_playfield_palette, 6441),
	asm_routine("F", 0x8C12, "put_small_char", put_small_char, 286),
)

}
