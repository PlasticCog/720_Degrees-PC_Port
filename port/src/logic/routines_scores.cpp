// Rewritten routines: score, time and statistics screens (bank B 0x2800, segment B6).
#include "logic.h"

namespace logic {
namespace {

using namespace t11rt;

struct KeepRegs {
	int n;
	uint16_t r[6];
	explicit KeepRegs(int count) : n(count) { for (int i = 1; i <= n; i++) r[i] = T.r[i]; }
	~KeepRegs() { for (int i = 1; i <= n; i++) T.r[i] = r[i]; }
};

uint16_t sext8(uint8_t v) { return (uint16_t)(int16_t)(int8_t)v; }

// B6:68E2  print_time(const int *t): t[0] with 4 digits, ':', the bytes at t+2 and t+4 with
// 2 digits each, separated by ':' (number printer F:BA9A, large font)
uint16_t print_time(const Args &a)
{
	KeepRegs keep(2);
	const uint16_t t = a.u(0);
	call(0xBA9A, {0, 4, 4, 0, peek(t)});
	T.r[0] = 0x003A;
	call(0x8B52, {});
	call(0xBA9A, {0, 2, 2, 0, sext8(peekb((uint16_t)(t + 2)))});
	T.r[0] = 0x003A;
	call(0x8B52, {});
	return call(0xBA9A, {0, 2, 2, 0, sext8(peekb((uint16_t)(t + 4)))});
}

// B6:6938 (assembly)  divide_time(int *out, int n, const int *in): the three-part time at in
// (units of 60) divided by n into out (F:90A6 quotient, F:90B0 remainder); n = 0 clears out
void divide_time()
{
	const uint16_t sp = T.r[6];
	const uint16_t out = rw((uint16_t)(sp + 2)), n = rw((uint16_t)(sp + 4));
	T.r[1] = out;
	T.r[3] = (uint16_t)(out + 4);
	T.r[5] = n;
	if (n == 0)
	{
		poke(out, 0);
		poke((uint16_t)(out + 2), 0);
		poke(T.r[3], 0);
		fl::clr();
		return;
	}
	const uint16_t in = rw((uint16_t)(sp + 6));
	T.r[2] = in;
	poke(out, call(0x90A6, {n, peek(in)}));
	uint16_t r = call(0x90B0, {n, peek(in)});
	uint16_t r4 = (uint16_t)(call(0x80D8, {0x003C, r}) + peek((uint16_t)(in + 2)));
	poke((uint16_t)(out + 2), call(0x90A6, {n, r4}));
	r = call(0x90B0, {n, r4});
	r4 = call(0x80D8, {0x003C, r});
	T.r[0] = call(0x90A6, {n, (uint16_t)(r4 + peek((uint16_t)(in + 4)))});
	poke(T.r[3], T.r[0]);
	if ((int16_t)T.r[0] < 0) poke(T.r[3], (uint16_t)(peek(T.r[3]) + 0x003C));
	T.r[4] = r4;
	fl::add((uint16_t)(sp - 0x10), 0x0010);            // ADD #16,SP
}

// B6:6FAE  draw_bar_table(int same_line, int count, int first): heading texts, then for
// i = 1..count the text first+i (F:8B04), the next value of the list at 0x020C (number
// printer F:BA96) and a bar of it: one tile 0x96 per 8, the rest as tile 0x9E - remainder
uint16_t draw_bar_table(const Args &a)
{
	KeepRegs keep(2);
	call(0x8D2C, {});                                  // clear_text_ram
	T.r[0] = 1;
	call(0x8AB6, {});
	T.r[0] = 0;
	call(0x8B04, {});
	T.r[0] = a.u(2);
	uint16_t r0 = call(0x8AB6, {});
	for (int16_t i = 1; i <= (int16_t)a.u(1); i++)
	{
		T.r[0] = (uint16_t)(i + a.u(2));
		call(0x8B04, {});
		const uint16_t list = peek(0x020C);
		int16_t v = (int16_t)peek(list);
		poke(0x020C, (uint16_t)(list + 2));
		r0 = call(0xBA96, {1, 1, 4, 0, (uint16_t)v});
		if (a.u(0) == 0)
		{
			r0 = (uint16_t)(peek(0x0186) + 0x0080);
			poke(0x0186, r0);
			poke(0x0186, peek(0x0186) & ~0x007F);
		}
		while (v >= 8)
		{
			poke(peek(0x0186), 0x0096);
			poke(0x0186, (uint16_t)(peek(0x0186) + 2));
			v = (int16_t)(v - 8);
		}
		poke(peek(0x0186), 0x009E);
		poke(peek(0x0186), (uint16_t)(peek(peek(0x0186)) - v));
	}
	return r0;
}

// B6:6F48 (assembly): statistics screens: sound command 0x15, F:8F0C(0x370, 0x5A), two bar
// tables of the list at 0x037A, each until a button press, then text 0x18 until another
void statistics_screens()
{
	T.icount -= 30;
	call(0x8D2C, {});                                  // clear_text_ram
	poke(0x0184, 0x5BBC);
	T.icount -= 33 + 30 + 30;
	call(0x8EA2, {0x0015});                            // send_sound_command
	T.icount -= 27 + 30 + 30;
	call(0x8F0C, {0x0370, 0x005A});
	poke(0x020C, 0x037A);
	T.icount -= 33 + 24 + 12;
	if (peekb(0x01A2))
	{
		T.icount -= 30;
		call(0x8A9E, {0x0370, 0x005A});
	}
	T.icount -= 27 + 30 + 24 + 30;
	call(0x6FAE, {0, 0x0012, 0x0002, 0x005A});         // draw_bar_table
	T.icount -= 30;
	call(0x8D60, {});                                  // wait_button_press
	poke(0x020C, (uint16_t)(peek(0x020C) + 4));
	T.icount -= 33 + 27 + 30 + 24 + 30;
	call(0x6FAE, {0, 0x0012, 0x0002, 0x0012, 0x0002, 0x005A});
	T.r[0] = 0x0018;
	T.icount -= 18 + 30;
	call(0x8AB6, {});
	T.icount -= 30;
	call(0x8D60, {});
	T.icount -= 18;
	fl::add((uint16_t)(T.r[6] - 0x0C), 0x000C);        // ADD #12,SP
}

// B6:7304: colour test: clear the screen, digits 0-3 in the large font, then 8 rows of 4 x 8
// playfield tiles in each of the four palettes (video window 0x4000 via F:8078)
uint16_t colour_test_pattern(const Args &)
{
	KeepRegs keep(5);
	call(0x8CE8, {});                                  // clear_playfield
	call(0x8D2C, {});                                  // clear_text_ram
	poke(0x0186, 0x2411);
	for (int i = 0; i <= 3; i++)
	{
		T.r[0] = (uint16_t)(i + 0x30);
		call(0x8C56, {});                              // draw_big_char
		poke(0x0186, (uint16_t)(peek(0x0186) + 0x18));
	}
	T.r[0] = 0x4000;
	call(0x8078, {});                                  // select_video_bank
	uint16_t r0 = 0x430C;
	for (int row = 8; row > 0; row--)
	{
		uint16_t pal = 0;
		for (int r4 = 3; r4 >= 0; r4--)
		{
			const uint16_t tile = (uint16_t)(r4 + 8);
			for (int k = 0; k < 8; k++)
			{
				const uint16_t addr = (uint16_t)((r0 & ~0xE001) | 0x2000);
				poke(addr, pal);
				poke(addr, (uint16_t)(peek(addr) | tile));
				r0 = (uint16_t)(r0 + 2);
			}
			pal = (uint16_t)(pal + 0x4000);
			r0 = (uint16_t)(r0 + 0x0C);
		}
		r0 = (uint16_t)(r0 + 0x90);
	}
	return r0;
}


// B6:706A (assembly, self test): the character sets: titles, the digits in the big font,
// the letters A-^ in the large font, then digits and letters in the small font, ten per line
// with the next colour (0x0188 += 0x2000) per line; until a button press
void selftest_charset_screen()
{
	poke(0x0184, 0x5BBC);
	T.r[0] = 0x001F;
	T.icount -= 33 + 18 + 30;
	call(0x8AB6, {});
	poke(0x0184, 0x5C04);
	T.r[0] = 0x001F;
	T.icount -= 33 + 18 + 30;
	call(0x8AB6, {});
	poke(0x0186, 0x2322);
	poke(0x0188, 0);
	T.icount -= 33 + 27 + 18;
	for (uint16_t c = 0x30; c <= 0x39; c++)
	{
		T.r[0] = c;
		T.icount -= 12 + 30;
		call(0x8C56, {});                              // draw_big_char
		poke(0x0186, (uint16_t)(peek(0x0186) + 2));
		T.icount -= 33 + 12 + 18 + 12;
	}
	uint16_t n = 10;
	T.icount -= 18 + 18;
	auto letters = [&](uint16_t line_step, uint16_t draw) {
		for (uint16_t c = 0x41; c <= 0x5E; c++)
		{
			T.icount -= 18 + 12;
			if (n == 10)
			{
				poke(0x0186, (uint16_t)(peek(0x0186) + line_step));
				n = 0;
				poke(0x0188, (uint16_t)(peek(0x0188) + 0x2000));
				T.icount -= 33 + 12 + 33;
			}
			n++;
			T.r[0] = c;
			T.icount -= 12 + 12 + 30;
			call(draw, {});
			poke(0x0186, (uint16_t)(peek(0x0186) + 2));
			T.icount -= 33 + 12 + 18 + 12;
		}
	};
	letters(0x0144, 0x8B52);                           // draw_large_char
	poke(0x0186, 0x2F2C);
	poke(0x0188, (uint16_t)(peek(0x0188) + 0x2000));
	T.icount -= 33 + 33 + 18;
	for (uint16_t c = 0x30; c <= 0x39; c++)
	{
		T.r[0] = c;
		T.icount -= 12 + 30;
		call(0x8C12, {});                              // put_small_char
		poke(0x0186, (uint16_t)(peek(0x0186) + 2));
		T.icount -= 33 + 12 + 18 + 12;
	}
	n = 10;
	T.icount -= 18 + 18;
	letters(0x0158, 0x8C12);
	T.icount -= 30;
	call(0x8D60, {});                                  // wait_button_press
	T.icount -= 30;
	call(0x8D2C, {});                                  // clear_text_ram
	pokeb(0x0FE6, 0);
	T.icount -= 27;
	T.r[1] = n;
	T.r[2] = 0x005F;
	fl::clr();
}

}

LOGIC_TABLE(scores,
	c_routine("B6", 0x68E2, "print_time", print_time, 0, 906),
	asm_routine("B6", 0x6938, "divide_time", divide_time, 189),
	c_routine("B6", 0x6FAE, "draw_bar_table", draw_bar_table, 0, 11661),
	wait_routine("B6", 0x6F48, "statistics_screens", statistics_screens, 21),
	c_routine("B6", 0x7304, "colour_test_pattern", colour_test_pattern, 0, 52932),
	wait_routine("B6", 0x706A, "selftest_charset_screen", selftest_charset_screen, 21),
)

}
