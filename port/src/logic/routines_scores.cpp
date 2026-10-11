// Rewritten routines: score, time and statistics screens (bank B 0x2800, segment B6).  They
// follow the original instruction by instruction (tick after every instruction, the stack as
// the original's; see routines_core.cpp).
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

// B6:68E2  print_time(const int *t): t[0] with 4 digits, ':', the bytes at t+2 and t+4 with
// 2 digits each, separated by ':' (number printer F:BA9A, large font).  The arguments for
// the number printer pile up on the stack as in the original.
uint16_t print_time(const Args &a)
{
	c_save(1, 0x68E6);
	T.r[2] = a.u(0);
	fl::mov(T.r[2]);
	tick(0x68EA, 27);
	push(peek(T.r[2]));
	fl::mov(peek(T.r[6]));
	tick(0x68EC, 30);
	push(0);
	fl::clr();
	tick(0x68EE, 24);
	push(4);
	fl::mov(0x0004);
	tick(0x68F2, 30);
	push(rw(T.r[6]));
	fl::mov(peek(T.r[6]));
	tick(0x68F4, 30);                                  // MOV (SP),-(SP)
	push(0);
	fl::clr();
	tick(0x68F6, 24);
	jsr(0x68FA, 0xBA9A);
	for (int part = 1; part <= 2; part++)
	{
		const uint16_t pc = (uint16_t)(0x68FA + 0x1C * (part - 1));
		T.r[0] = 0x003A;
		fl::mov(T.r[0]);
		tick((uint16_t)(pc + 4), 18);
		jsr((uint16_t)(pc + 8), 0x8B52);               // draw_large_char(':')
		T.r[1] = sext8(peekb((uint16_t)(T.r[2] + 2 * part)));
		fl::movb((uint8_t)T.r[1]);
		tick((uint16_t)(pc + 0x0C), 27);
		ww(T.r[6], T.r[1]);
		fl::mov(T.r[1]);
		tick((uint16_t)(pc + 0x0E), 21);
		push(0);
		fl::clr();
		tick((uint16_t)(pc + 0x10), 24);
		push(2);
		fl::mov(0x0002);
		tick((uint16_t)(pc + 0x14), 30);
		push(rw(T.r[6]));
		fl::mov(peek(T.r[6]));
		tick((uint16_t)(pc + 0x16), 30);
		push(0);
		fl::clr();
		tick((uint16_t)(pc + 0x18), 24);
		jsr((uint16_t)(pc + 0x1C), 0xBA9A);
	}
	T.r[6] = fl::add(T.r[6], 0x1A);
	tick(0x6936, 18);                                  // ADD #0x1A,SP
	c_restore(1);
	return T.r[0];
}

// B6:6938 (assembly)  divide_time(int *out, int n, const int *in): the three-part time at in
// (units of 60) divided by n into out (F:90A6 quotient, F:90B0 remainder); n = 0 clears out.
// The arguments of the calls pile up on the stack as in the original.
void divide_time()
{
	T.r[1] = rw((uint16_t)(T.r[6] + 2));
	fl::mov(T.r[1]);
	tick(0x693C, 27);
	T.r[3] = 4;
	fl::mov(T.r[3]);
	tick(0x6940, 18);
	T.r[3] = fl::add(T.r[3], T.r[1]);
	tick(0x6942, 12);
	T.r[5] = rw((uint16_t)(T.r[6] + 4));
	fl::mov(T.r[5]);
	tick(0x6946, 27);
	if (T.r[5] == 0)
	{
		tick(0x6948, 12);
		poke(T.r[1], 0);
		fl::clr();
		tick(0x694A, 21);
		poke((uint16_t)(T.r[1] + 2), 0);
		fl::clr();
		tick(0x694E, 30);
		poke(T.r[3], 0);
		fl::clr();
		tick(0x6950, 21);
		rts();
		return;
	}
	tick(0x6952, 12);
	T.r[2] = rw((uint16_t)(T.r[6] + 6));
	fl::mov(T.r[2]);
	tick(0x6956, 27);
	push(peek(T.r[2]));
	fl::mov(peek(T.r[6]));
	tick(0x6958, 30);
	push(T.r[5]);
	fl::mov(T.r[5]);
	tick(0x695A, 24);
	jsr(0x695E, 0x90A6);                               // quotient
	poke(T.r[1], T.r[0]);
	fl::mov(T.r[0]);
	tick(0x6960, 21);
	ww(T.r[6], peek(T.r[2]));
	fl::mov(peek(T.r[6]));
	tick(0x6962, 27);
	push(T.r[5]);
	fl::mov(T.r[5]);
	tick(0x6964, 24);
	jsr(0x6968, 0x90B0);                               // remainder
	ww(T.r[6], T.r[0]);
	fl::mov(T.r[0]);
	tick(0x696A, 21);
	push(0x003C);
	fl::mov(0x003C);
	tick(0x696E, 30);
	jsr(0x6972, 0x80D8);                               // mul16
	T.r[4] = T.r[0];
	fl::mov(T.r[4]);
	tick(0x6974, 12);
	T.r[4] = fl::add(T.r[4], peek((uint16_t)(T.r[2] + 2)));
	tick(0x6978, 27);
	ww(T.r[6], T.r[4]);
	fl::mov(T.r[4]);
	tick(0x697A, 21);
	push(T.r[5]);
	fl::mov(T.r[5]);
	tick(0x697C, 24);
	jsr(0x6980, 0x90A6);
	poke((uint16_t)(T.r[1] + 2), T.r[0]);
	fl::mov(T.r[0]);
	tick(0x6984, 30);
	ww(T.r[6], T.r[4]);
	fl::mov(T.r[4]);
	tick(0x6986, 21);
	push(T.r[5]);
	fl::mov(T.r[5]);
	tick(0x6988, 24);
	jsr(0x698C, 0x90B0);
	ww(T.r[6], T.r[0]);
	fl::mov(T.r[0]);
	tick(0x698E, 21);
	push(0x003C);
	fl::mov(0x003C);
	tick(0x6992, 30);
	jsr(0x6996, 0x80D8);
	T.r[4] = T.r[0];
	fl::mov(T.r[4]);
	tick(0x6998, 12);
	ww(T.r[6], T.r[4]);
	fl::mov(T.r[4]);
	tick(0x699A, 21);
	ww(T.r[6], fl::add(rw(T.r[6]), peek((uint16_t)(T.r[2] + 4))));
	tick(0x699E, 36);
	push(T.r[5]);
	fl::mov(T.r[5]);
	tick(0x69A0, 24);
	jsr(0x69A4, 0x90A6);
	poke(T.r[3], T.r[0]);
	fl::mov(T.r[0]);
	tick(0x69A6, 21);
	if ((int16_t)T.r[0] < 0)
	{
		tick(0x69A8, 12);
		poke(T.r[3], fl::add(peek(T.r[3]), 0x003C));
		tick(0x69AC, 27);
	}
	else
		tick(0x69AC, 12);                              // BGE
	T.r[6] = fl::add(T.r[6], 0x0010);
	tick(0x69B0, 18);
	rts();
}

// B6:6FAE  draw_bar_table(int same_line, int count, int first): heading texts, then for
// i = 1..count the text first+i (F:8B04), the next value of the list at 0x020C (number
// printer F:BA96) and a bar of it: one tile 0x96 per 8, the rest as tile 0x9E - remainder
uint16_t draw_bar_table(const Args &a)
{
	c_save(1, 0x6FB2);
	jsr(0x6FB6, 0x8D2C);                               // clear_text_ram
	T.r[0] = 1;
	fl::mov(T.r[0]);
	tick(0x6FBA, 18);
	jsr(0x6FBE, 0x8AB6);
	T.r[0] = 0;
	fl::mov(T.r[0]);
	tick(0x6FC2, 18);
	jsr(0x6FC6, 0x8B04);
	T.r[0] = a.u(2);
	fl::mov(T.r[0]);
	tick(0x6FCA, 27);
	jsr(0x6FCE, 0x8AB6);
	T.r[2] = 0;
	fl::clr();
	tick(0x6FD0, 12);
	tick(0x7036, 12);                                  // BR
	for (;;)
	{
		T.r[2] = fl::inc(T.r[2]);
		tick(0x7038, 12);
		const bool more = (int16_t)T.r[2] <= (int16_t)a.u(1);
		fl::cmp(T.r[2], peek((uint16_t)(T.r[6] + 0x0A)));
		tick(0x703C, 27);                              // CMP R2,0xA(SP)
		if (!more) break;
		tick(0x6FD2, 12);                              // BLE
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0x6FD4, 12);
		T.r[0] = fl::add(T.r[0], a.u(2));
		tick(0x6FD8, 27);
		jsr(0x6FDC, 0x8B04);
		T.r[1] = peek(peek(0x020C));
		fl::mov(T.r[1]);
		tick(0x6FE0, 33);
		poke(0x020C, fl::add(peek(0x020C), 2));
		tick(0x6FE6, 33);
		push(T.r[1]);
		fl::mov(T.r[1]);
		tick(0x6FE8, 24);
		push(0);
		fl::clr();
		tick(0x6FEA, 24);
		push(4);
		fl::mov(0x0004);
		tick(0x6FEE, 30);
		push(1);
		fl::mov(0x0001);
		tick(0x6FF2, 30);
		push(rw(T.r[6]));
		fl::mov(peek(T.r[6]));
		tick(0x6FF4, 30);                              // MOV (SP),-(SP)
		jsr(0x6FF8, 0xBA96);
		const bool same_line = a.u(0) != 0;
		fl::tst(peek((uint16_t)(T.r[6] + 0x12)));
		tick(0x6FFC, 27);                              // TST 0x12(SP)
		if (same_line)
			tick(0x7010, 12);                          // BNE
		else
		{
			tick(0x6FFE, 12);
			T.r[0] = peek(0x0186);
			fl::mov(T.r[0]);
			tick(0x7002, 24);
			T.r[0] = fl::add(T.r[0], 0x0080);
			tick(0x7006, 18);
			poke(0x0186, T.r[0]);
			fl::mov(T.r[0]);
			tick(0x700A, 27);
			poke(0x0186, fl::logic((uint16_t)(peek(0x0186) & ~0x007F)));
			tick(0x7010, 33);
		}
		for (;;)
		{
			fl::cmp(T.r[1], 0x0008);
			tick(0x7014, 18);                          // CMP R1,#8
			if ((int16_t)T.r[1] < 8) break;
			tick(0x7016, 12);
			poke(peek(0x0186), 0x0096);
			fl::mov(0x0096);
			tick(0x701C, 42);
			poke(0x0186, fl::add(peek(0x0186), 2));
			tick(0x7022, 33);
			T.r[1] = fl::sub(T.r[1], 8);
			tick(0x7026, 18);
			tick(0x7010, 12);                          // BR
		}
		tick(0x7028, 12);                              // BLT
		poke(peek(0x0186), 0x009E);
		fl::mov(0x009E);
		tick(0x702E, 42);
		poke(peek(0x0186), fl::sub(peek(peek(0x0186)), T.r[1]));
		tick(0x7032, 36);
		T.r[6] = fl::add(T.r[6], 0x0A);
		tick(0x7036, 18);                              // ADD #0x0A,SP
	}
	tick(0x703E, 12);
	c_restore(1);
	return T.r[0];
}

// B6:6F48 (assembly): statistics screens: sound command 0x15, F:8F0C(0x370, 0x5A), two bar
// tables of the list at 0x037A, each until a button press, then text 0x18 until another.
// The arguments pile up on the stack as in the original.
void statistics_screens()
{
	jsr(0x6F4C, 0x8D2C);                               // clear_text_ram
	poke(0x0184, 0x5BBC);
	fl::mov(0x5BBC);
	tick(0x6F52, 33);
	push(0x0015);
	fl::mov(0x0015);
	tick(0x6F56, 30);
	jsr(0x6F5A, 0x8EA2);                               // send_sound_command
	ww(T.r[6], 0x005A);
	fl::mov(0x005A);
	tick(0x6F5E, 27);
	push(0x0370);
	fl::mov(0x0370);
	tick(0x6F62, 30);
	jsr(0x6F66, 0x8F0C);
	poke(0x020C, 0x037A);
	fl::mov(0x037A);
	tick(0x6F6C, 33);
	const bool dead = peekb(0x01A2) != 0;
	fl::tstb(peekb(0x01A2));
	tick(0x6F70, 24);
	if (dead)
	{
		tick(0x6F72, 12);
		jsr(0x6F76, 0x8A9E);
	}
	else
		tick(0x6F76, 12);                              // BEQ
	ww(T.r[6], 0x0002);
	fl::mov(0x0002);
	tick(0x6F7A, 27);
	push(0x0012);
	fl::mov(0x0012);
	tick(0x6F7E, 30);
	push(0);
	fl::clr();
	tick(0x6F80, 24);
	jsr(0x6F84, 0x6FAE);                               // draw_bar_table
	jsr(0x6F88, 0x8D60);                               // wait_button_press
	poke(0x020C, fl::add(peek(0x020C), 4));
	tick(0x6F8E, 33);
	ww(T.r[6], 0x0002);
	fl::mov(0x0002);
	tick(0x6F92, 27);
	push(0x0012);
	fl::mov(0x0012);
	tick(0x6F96, 30);
	push(0);
	fl::clr();
	tick(0x6F98, 24);
	jsr(0x6F9C, 0x6FAE);
	T.r[0] = 0x0018;
	fl::mov(T.r[0]);
	tick(0x6FA0, 18);
	jsr(0x6FA4, 0x8AB6);
	jsr(0x6FA8, 0x8D60);
	T.r[6] = fl::add(T.r[6], 0x000C);
	tick(0x6FAC, 18);
	rts();
}

// B6:7304: colour test: clear the screen, digits 0-3 in the large font, then 8 rows of 4 x 8
// playfield tiles in each of the four palettes (video window 0x4000 via F:8078).  Two locals
// on the stack: the tile address and the row count.
uint16_t colour_test_pattern(const Args &)
{
	c_save(4, 0x7308);
	T.r[6] -= 4;
	fl::cmp(peek((uint16_t)(T.r[6] + 2)), peek(T.r[6]));
	tick(0x730A, 30);                                  // CMP -(SP),-(SP)
	jsr(0x730E, 0x8CE8);                               // clear_playfield
	jsr(0x7312, 0x8D2C);                               // clear_text_ram
	poke(0x0186, 0x2411);
	fl::mov(0x2411);
	tick(0x7318, 33);
	T.r[1] = 0;
	fl::clr();
	tick(0x731A, 12);
	for (;;)
	{
		T.r[0] = T.r[1];
		fl::mov(T.r[0]);
		tick(0x731C, 12);
		T.r[0] = fl::add(T.r[0], 0x0030);
		tick(0x7320, 18);
		jsr(0x7324, 0x8C56);                           // draw_big_char
		poke(0x0186, fl::add(peek(0x0186), 0x0018));
		tick(0x732A, 33);
		T.r[1] = fl::inc(T.r[1]);
		tick(0x732C, 12);
		fl::cmp(T.r[1], 0x0003);
		tick(0x7330, 18);                              // CMP R1,#3
		if ((int16_t)T.r[1] > 3) break;
		tick(0x731A, 12);                              // BLE
	}
	tick(0x7332, 12);
	T.r[0] = 0x4000;
	fl::mov(T.r[0]);
	tick(0x7336, 18);
	jsr(0x733A, 0x8078);                               // select_video_bank
	T.r[0] = 0x430C;
	fl::mov(T.r[0]);
	tick(0x733E, 18);
	ww((uint16_t)(T.r[6] + 2), 8);
	fl::mov(0x0008);
	tick(0x7344, 36);
	for (;;)                                           // rows
	{
		T.r[1] = 0;
		fl::clr();
		tick(0x7346, 12);
		T.r[4] = 3;
		fl::mov(T.r[4]);
		tick(0x734A, 18);
		for (;;)                                       // palettes
		{
			T.r[5] = T.r[4];
			fl::mov(T.r[5]);
			tick(0x734C, 12);
			T.r[5] = fl::add(T.r[5], 8);
			tick(0x7350, 18);
			T.r[3] = 8;
			fl::mov(T.r[3]);
			tick(0x7354, 18);
			for (;;)                                   // tiles
			{
				T.r[2] = T.r[0];
				fl::mov(T.r[2]);
				tick(0x7356, 12);
				T.r[2] = fl::logic((uint16_t)(T.r[2] & (uint16_t)~0xE001));
				tick(0x735A, 18);
				ww(T.r[6], T.r[2]);
				fl::mov(T.r[2]);
				tick(0x735C, 21);
				ww(T.r[6], fl::logic((uint16_t)(rw(T.r[6]) | 0x2000)));
				tick(0x7360, 27);
				const uint16_t addr = rw(T.r[6]);
				poke(addr, T.r[1]);
				fl::mov(T.r[1]);
				tick(0x7364, 36);
				poke(addr, fl::logic((uint16_t)(peek(addr) | T.r[5])));
				tick(0x7368, 36);
				T.r[0] = fl::add(T.r[0], 2);
				tick(0x736C, 18);
				if (--T.r[3] == 0) break;
				tick(0x7354, 18);                      // SOB
			}
			tick(0x736E, 18);
			T.r[1] = fl::add(T.r[1], 0x4000);
			tick(0x7372, 18);
			T.r[0] = fl::add(T.r[0], 0x000C);
			tick(0x7376, 18);
			T.r[4] = fl::dec(T.r[4]);
			tick(0x7378, 12);
			if ((int16_t)T.r[4] < 0) break;
			tick(0x734A, 12);                          // BGE
		}
		tick(0x737A, 12);
		T.r[0] = fl::add(T.r[0], 0x0090);
		tick(0x737E, 18);
		const uint16_t rows = (uint16_t)(rw((uint16_t)(T.r[6] + 2)) - 1);
		ww((uint16_t)(T.r[6] + 2), rows);
		fl::dec((uint16_t)(peek((uint16_t)(T.r[6] + 2)) + 1));
		tick(0x7382, 30);
		if (rows == 0) break;
		tick(0x7344, 12);                              // BNE
	}
	tick(0x7384, 12);
	T.r[6] += 4;
	fl::cmp(peek((uint16_t)(T.r[6] - 4)), peek((uint16_t)(T.r[6] - 2)));
	tick(0x7386, 24);                                  // CMP (SP)+,(SP)+
	c_restore(4);
	return T.r[0];
}


// B6:706A (assembly, self test): the character sets: titles, the digits in the big font,
// the letters A-^ in the large font, then digits and letters in the small font, ten per line
// with the next colour (0x0188 += 0x2000) per line; until a button press
void selftest_charset_screen()
{
	poke(0x0184, 0x5BBC);
	fl::mov(0x5BBC);
	tick(0x7070, 33);
	T.r[0] = 0x001F;
	fl::mov(T.r[0]);
	tick(0x7074, 18);
	jsr(0x7078, 0x8AB6);
	poke(0x0184, 0x5C04);
	fl::mov(0x5C04);
	tick(0x707E, 33);
	T.r[0] = 0x001F;
	fl::mov(T.r[0]);
	tick(0x7082, 18);
	jsr(0x7086, 0x8AB6);
	poke(0x0186, 0x2322);
	fl::mov(0x2322);
	tick(0x708C, 33);
	poke(0x0188, 0);
	fl::clr();
	tick(0x7090, 27);
	// the digits 0-9 (counter register r) drawn by draw at the loop at pc
	auto digits = [](uint16_t pc, int r, uint16_t draw) {
		T.r[r] = 0x0030;
		tick(pc, 18);
		for (;;)
		{
			T.r[0] = T.r[r];
			tick((uint16_t)(pc + 2), 12);
			jsr((uint16_t)(pc + 6), draw);
			poke(0x0186, (uint16_t)(peek(0x0186) + 2));
			tick((uint16_t)(pc + 0x0C), 33);
			T.r[r]++;
			tick((uint16_t)(pc + 0x0E), 12);
			tick((uint16_t)(pc + 0x12), 18);           // CMP Rr,#0x39
			if ((int16_t)T.r[r] > 0x39) break;
			tick(pc, 12);                              // BLE
		}
		tick((uint16_t)(pc + 0x14), 12);
	};
	// the letters 0x41-0x5E, ten per line (line step, next colour), drawn by draw at pc
	auto letters = [](uint16_t pc, uint16_t line_step, uint16_t draw) {
		T.r[1] = 0x000A;
		tick((uint16_t)(pc - 4), 18);
		T.r[2] = 0x0041;
		tick(pc, 18);
		for (;;)
		{
			tick((uint16_t)(pc + 4), 18);              // CMP R1,#10
			if (T.r[1] == 0x000A)
			{
				tick((uint16_t)(pc + 6), 12);
				poke(0x0186, (uint16_t)(peek(0x0186) + line_step));
				tick((uint16_t)(pc + 0x0C), 33);
				T.r[1] = 0;
				tick((uint16_t)(pc + 0x0E), 12);
				poke(0x0188, (uint16_t)(peek(0x0188) + 0x2000));
				tick((uint16_t)(pc + 0x14), 33);
			}
			else
				tick((uint16_t)(pc + 0x14), 12);       // BNE
			T.r[1]++;
			tick((uint16_t)(pc + 0x16), 12);
			T.r[0] = T.r[2];
			tick((uint16_t)(pc + 0x18), 12);
			jsr((uint16_t)(pc + 0x1C), draw);
			poke(0x0186, (uint16_t)(peek(0x0186) + 2));
			tick((uint16_t)(pc + 0x22), 33);
			T.r[2]++;
			tick((uint16_t)(pc + 0x24), 12);
			tick((uint16_t)(pc + 0x28), 18);           // CMP R2,#0x5E
			if ((int16_t)T.r[2] > 0x5E) break;
			tick(pc, 12);                              // BLE
		}
		tick((uint16_t)(pc + 0x2A), 12);
	};
	digits(0x7094, 1, 0x8C56);                         // draw_big_char
	letters(0x70B0, 0x0144, 0x8B52);                   // draw_large_char
	poke(0x0186, 0x2F2C);
	fl::mov(0x2F2C);
	tick(0x70E0, 33);
	poke(0x0188, fl::add(peek(0x0188), 0x2000));
	tick(0x70E6, 33);
	digits(0x70EA, 2, 0x8C12);                         // put_small_char
	letters(0x7106, 0x0158, 0x8C12);
	jsr(0x7134, 0x8D60);                               // wait_button_press
	jsr(0x7138, 0x8D2C);                               // clear_text_ram
	pokeb(0x0FE6, 0);
	fl::clr();
	tick(0x713C, 27);
	rts();
}

}

LOGIC_TABLE(scores,
	c_routine("B6", 0x68E2, "print_time", print_time, 0, 0),
	asm_routine("B6", 0x6938, "divide_time", divide_time, 0),
	c_routine("B6", 0x6FAE, "draw_bar_table", draw_bar_table, 0, 0),
	wait_routine("B6", 0x6F48, "statistics_screens", statistics_screens, 0),
	c_routine("B6", 0x7304, "colour_test_pattern", colour_test_pattern, 0, 0),
	wait_routine("B6", 0x706A, "selftest_charset_screen", selftest_charset_screen, 0),
)

}
