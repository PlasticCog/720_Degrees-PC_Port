// Rewritten routines: text layer, video memory and the display interrupts.
#include "logic.h"

namespace logic {
namespace {

using namespace t11rt;

// F:8B52 (character code in R0): draw a character of the large font (2 x 2 tiles) at the text
// cursor 0x0186 with the colour bits 0x0188 and advance it; 0 only advances.  Digits are
// drawn by F:8C56, ',' is two tiles, other codes use the 4-byte tile groups at 0x4000:
// symbols through the table at 0x8C86 ('-' and '=' in the second palette), letters from 0x41
// on in order.  R1-R3 are preserved (register-save helper F144).
void draw_large_char()
{
	if (T.r[0] == 0)
	{
		poke(0x0186, fl::add(peek(0x0186), 4));
		return;
	}
	uint16_t c = T.r[0] & 0x007F;
	uint16_t colour = peek(0x0188);
	if (c >= 0x30 && c <= 0x39)
	{
		T.r[0] = c;
		call(0x8C56, {});
		return;
	}
	if (c == 0x2C)
	{
		const uint16_t p = peek(0x0186);
		poke(p, (uint16_t)(0x0087 | colour));
		poke(0x0186, (uint16_t)(p + 2));
		const uint16_t below = fl::add((uint16_t)(p + 2), 0x007E);
		poke(below, (uint16_t)(0x007D | colour));
		T.r[0] = c;
		fl::mov(T.r[1]);
		return;
	}
	uint16_t g;
	if (c == 0x1F)
		g = 0x0074;
	else if (c == 0x2D)
	{
		g = 0x0084;
		colour |= 0x0100;
	}
	else
	{
		if (c == 0x3D) colour |= 0x0100;
		if ((int16_t)c > 0x40)
			g = (uint16_t)((c - 0x41) << 2);
		else
			g = peekb((uint16_t)(0x8C86 + c - 0x20));
	}
	const uint16_t src = (uint16_t)(0x4000 + g);
	const uint16_t p = peek(0x0186);
	poke(p, (uint16_t)(peekb(src) | colour));
	poke((uint16_t)(p + 2), (uint16_t)(peekb((uint16_t)(src + 1)) | colour));
	poke(0x0186, (uint16_t)(p + 4));
	const uint16_t below = fl::add((uint16_t)(p + 4), 0x007C);
	poke(below, (uint16_t)(peekb((uint16_t)(src + 2)) | colour));
	poke((uint16_t)(below + 2), (uint16_t)(peekb((uint16_t)(src + 3)) | colour));
	T.r[0] = (uint16_t)(src + 3);
	f_nzvc(nzw(T.r[1]));                               // the CLRBs after the ADD left C clear
}

// F:8D2C / F:8D3E (assembly): clear video RAM window 0 from 0x2000 to 0x37FF (playfield)
// or from 0x3800 to 0x3FFF (text layer and sprites); interrupts may come in after every word
void clear_video(uint16_t from, uint16_t last, uint16_t loop_pc)
{
	call(0x8068, {});                                  // select_video_bank0
	uint16_t a = from;
	do
	{
		poke(a, 0);
		a = (uint16_t)(a + 2);
		tick(loop_pc, 21 + 18 + 12);
	} while (a <= last);
	T.r[0] = a;
	fl::cmp(a, last);
}
void clear_playfield_ram() { clear_video(0x2000, 0x37FE, 0x8D34); }
void clear_text_ram() { clear_video(0x3800, 0x3FFE, 0x8D46); }

// F:B9D4 (interrupt, 32V during play): count the quarter frame in 0x0191; at the first and
// the fourth the sprite list link is rebuilt (F:B1C8) with 0x0993 = 1 / 0.  Other interrupts
// are allowed meanwhile (PSW 0); video bank 0 is selected and the previous bank restored.
void irq_32v_play()
{
	uint16_t saved[6];
	for (int i = 0; i < 6; i++) saved[i] = T.r[i];
	poke(0x15C0, 0);                                   // acknowledge
	pokeb(0x0191, (uint8_t)(peekb(0x0191) + 1));
	T.psw = 0;
	push(peek(0x01D0));
	call(0x8068, {});                                  // select_video_bank0
	const uint8_t quarter = peekb(0x0191);
	if (quarter == 1 || quarter == 4)
	{
		pokeb(0x0993, quarter == 1 ? 1 : 0);
		call(0xB1C8, {});                              // sprite_link
	}
	poke(0x01D0, rw(T.r[6]));
	call(0x8096, {});                                  // reselect_video_bank
	T.r[6] = (uint16_t)(T.r[6] + 2);
	for (int i = 0; i < 6; i++) T.r[i] = saved[i];
}


// F:DD10 (assembly): while 0x0EA9 counts down (every fourth frame) the colours 0x11C2-0x11C6
// rotate through the four at 0x7A9A; at 0 they are restored from 0x7E6C
void flash_text_colours()
{
	const uint8_t n = peekb(0x0EA9);
	if (!n)
	{
		fl::tstb(n);
		return;
	}
	const uint16_t phase = peek(0x01C4) & 0x0003;
	fl::mov(phase);
	if (phase) return;
	pokeb(0x0EA9, (uint8_t)(n - 1));
	T.r[3] = peekb(0x0EA9);
	uint16_t v = 0;
	if (T.r[3] == 0)
	{
		for (int i = 3; i >= 1; i--)
		{
			T.r[0] = (uint16_t)(i * 2);
			v = peek((uint16_t)(0x7E6C + T.r[0]));
			poke((uint16_t)(0x11C0 + T.r[0]), v);
		}
		T.r[1] = 0;
	}
	else
	{
		for (int i = 3; i >= 1; i--)
		{
			T.r[2] = (uint16_t)(i * 2);
			T.r[0] = (uint16_t)(((T.r[3] + i - 1) & 3) * 2);
			v = peek((uint16_t)(0x7A9A + T.r[0]));
			poke((uint16_t)(0x11C0 + T.r[2]), v);
		}
		T.r[1] = 0;
	}
	f_nzvc(nzw(v));
}

}

LOGIC_TABLE(screen,
	asm_routine("F", 0x8B52, "draw_large_char", draw_large_char, 835),
	asm_routine("F", 0x8D2C, "clear_playfield_ram", clear_playfield_ram, 69),
	asm_routine("F", 0x8D3E, "clear_text_ram", clear_text_ram, 69),
	irq_routine("F", 0xB9D4, "irq_32v_play", irq_32v_play, 618),
	asm_routine("F", 0xDD10, "flash_text_colours", flash_text_colours, 57),
)

}
