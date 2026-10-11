// Rewritten routines: power-on, self test and the controller.  They follow the original
// instruction by instruction (tick after every instruction, the stack as the original's; see
// routines_core.cpp).  The interrupt handlers' final RTI is charged in the table.
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

// F:F3A8: interrupt handlers of the self test: the eight vectors listed at 0xFC4E get F:8938
// (PSW 0xE0), the four 32V vectors at 0xFC5E F:8924 (PSW 0xC0), the sound command vector
// 0x0038 F:891E (PSW 0xA0); only the 32V interrupt is enabled.  Returns the last vector.
// The vectors are written with the interrupts off, where nothing sees them, so their two
// loops are charged at once.
uint16_t install_selftest_interrupts(const Args &)
{
	push(T.r[1]);
	tick(0xF3AA, 24);
	poke(0x1600, 0);
	fl::clr();
	tick(0xF3AE, 27);
	poke(0x1580, 0);
	tick(0xF3B2, 27);
	poke(0x15E0, 0);
	tick(0xF3B6, 27);
	poke(0x15C0, 0);
	tick(0xF3BA, 27);
	for (int i = 14; i >= 0; i -= 2)
	{
		const uint16_t v = peek((uint16_t)(0xFC4E + i));
		poke(v, 0x8938);
		poke((uint16_t)(v + 2), 0x00E0);
	}
	uint16_t v = 0;
	for (int i = 6; i >= 0; i -= 2)
	{
		v = peek((uint16_t)(0xFC5E + i));
		poke(v, 0x8924);
		poke((uint16_t)(v + 2), 0x00C0);
	}
	T.r[0] = v;
	tick(0xF3EE, 18 + 8 * (42 + 27 + 36 + 18 + 12) + 18 + 4 * (42 + 27 + 36 + 18 + 12));
	poke(0x0038, 0x891E);
	tick(0xF3F4, 33);
	poke(0x003A, 0x00A0);
	tick(0xF3FA, 33);
	poke(0x018C, 0x0008);
	tick(0xF400, 33);
	poke(0x1600, 0x0008);                              // the 32V interrupt on
	tick(0xF406, 33);
	T.r[1] = pop();
	tick(0xF408, 18);
	rts();
	return v;
}

// F:8924 (interrupt, 32V in the self test): count the quarter frame in 0x0191 (step by step;
// the RTI is in the table)
void irq_32v_count()
{
	push(T.r[0]);
	tick(0x8926, 24);
	T.r[0] = sext8(peekb(0x0191));
	fl::movb((uint8_t)T.r[0]);
	tick(0x892A, 24);
	T.r[0] = fl::inc(T.r[0]);
	tick(0x892C, 12);
	pokeb(0x0191, (uint8_t)T.r[0]);
	tick(0x8930, 27);
	poke(0x15C0, 0);                                   // acknowledge
	fl::clr();
	tick(0x8934, 27);
	T.r[0] = pop();
	fl::mov(T.r[0]);
	tick(0x8936, 18);
}

// F:8D60 (assembly): wait, frame by frame, until the vertical blank interrupt reports a
// button press in 0x0FE6; clear it again (step by step)
void wait_button_press()
{
	pokeb(0x0FE6, 0);
	tick(0x8D64, 27);
	for (;;)
	{
		jsr(0x8D68, 0x8D50);                           // wait_vblank
		const uint8_t b = peekb(0x0FE6);
		fl::tstb(b);
		tick(0x8D6C, 24);
		if (b) break;
		tick(0x8D64, 12);                              // BEQ
	}
	tick(0x8D6E, 12);
	pokeb(0x0FE6, 0);
	fl::clr();
	tick(0x8D72, 27);
	rts();
}

// F:F40C (assembly): load the palettes (F:DABA, F:DA74, F:DB4E), text colour 0xE000, and run
// the four display routines of bank B 0x2800 (B6:69B2, 6F48, 6B7E, 6CFC); step by step
void init_display_b6()
{
	jsr(0xF410, 0xDABA);
	jsr(0xF414, 0xDA74);
	jsr(0xF418, 0xDB4E);
	poke(0x0188, 0xE000);
	tick(0xF41E, 33);
	T.r[0] = 0x2800;
	tick(0xF422, 18);
	poke(0x01D4, 0x2800);
	tick(0xF426, 27);
	poke(0x1402, 0x2800);
	tick(0xF42A, 27);
	jsr(0xF42E, 0x69B2);
	jsr(0xF432, 0x6F48);
	jsr(0xF436, 0x6B7E);
	jsr(0xF43A, 0x6CFC);
	rts();
}

// F:F4C0 (assembly): "OK" in the large font
void print_ok()
{
	T.r[0] = 0x004F;
	fl::mov(T.r[0]);
	tick(0xF4C4, 18);
	jsr(0xF4C8, 0x8B52);                               // draw_large_char
	T.r[0] = 0x004B;
	fl::mov(T.r[0]);
	tick(0xF4CC, 18);
	jsr(0xF4D0, 0x8B52);
	rts();
}

// F:F8EA: 0x0FE7 counts down while input bit 0x40 of 0x0193 is held (60 otherwise), then
// F:8D86 runs
uint16_t button_hold_timer(const Args &)
{
	c_save(4, 0xF8EE);
	const bool held = (peekb(0x0193) & 0x40) != 0;
	fl::logicb((uint8_t)(0x40 & peekb(0x0193)));
	tick(0xF8F4, 30);
	if (held)
	{
		tick(0xF8F6, 12);
		const bool running = peekb(0x0FE7) != 0;
		tick(0xF8FA, 24);
		if (running)
		{
			tick(0xF8FC, 12);
			pokeb(0x0FE7, (uint8_t)(peekb(0x0FE7) - 1));
			tick(0xF900, 27);
			tick(0xF908, 12);                          // BR
		}
		else
			tick(0xF908, 12);                          // BEQ
	}
	else
	{
		tick(0xF902, 12);                              // BEQ
		pokeb(0x0FE7, 0x3C);
		fl::movb(0x3C);
		tick(0xF908, 33);
	}
	jsr(0xF90C, 0x8D86);
	c_restore(4);
	return T.r[0];
}

// F:8E2E  rotary_step(): direction in which the controller (0x01C6, 144 counts per turn)
// moved since the last step position 0x02BC: +1 / -1, the shorter way round; 0 while it is
// less than 16 counts away (otherwise the step position follows)
uint16_t rotary_step(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x8E30, 24);
	T.r[1] = peek(0x01C6);
	fl::mov(T.r[1]);
	tick(0x8E34, 24);
	T.r[1] = fl::sub(T.r[1], peek(0x02BC));
	tick(0x8E38, 24);
	if ((int16_t)T.r[1] < 0)
	{
		tick(0x8E3A, 12);
		T.r[0] = 0xFFFF;
		tick(0x8E3E, 18);
		T.r[1] = (uint16_t)-T.r[1];
		fl::neg((uint16_t)(-T.r[1]));
		tick(0x8E40, 12);
		tick(0x8E46, 12);                              // BR
	}
	else
	{
		tick(0x8E42, 12);                              // BPL
		T.r[0] = 1;
		fl::mov(T.r[0]);
		tick(0x8E46, 18);
	}
	fl::cmp(T.r[1], 0x0048);
	tick(0x8E4A, 18);                                  // CMP R1,#0x48
	if ((int16_t)T.r[1] > 0x48)
	{
		tick(0x8E4C, 12);
		T.r[0] = (uint16_t)-T.r[0];
		fl::neg((uint16_t)(-T.r[0]));
		tick(0x8E4E, 12);
		T.r[1] = (uint16_t)-T.r[1];
		fl::neg((uint16_t)(-T.r[1]));
		tick(0x8E50, 12);
		T.r[1] = fl::add(T.r[1], 0x0090);
		tick(0x8E54, 18);
	}
	else
		tick(0x8E54, 12);                              // BLE
	fl::cmp(T.r[1], 0x0010);
	tick(0x8E58, 18);                                  // CMP R1,#0x10
	if ((int16_t)T.r[1] < 0x10)
	{
		tick(0x8E5A, 12);
		T.r[0] = 0;
		fl::clr();
		tick(0x8E5C, 12);
		tick(0x8E64, 12);                              // BR
	}
	else
	{
		tick(0x8E5E, 12);                              // BGE
		poke(0x02BC, peek(0x01C6));
		fl::mov(peek(0x02BC));
		tick(0x8E64, 39);
	}
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x8E66, 18);
	rts();
	return T.r[0];
}

// B6:72B0 (assembly, self test): fill the playfield with tile 0x0134 and wait for a button
// press (0x0FE6); each press and release of input bit 0x40 (0x0193) steps the colour 0x1086
// through the three values at 0x7050.  R0 ends as the colour index, R1 as twice that.  Step
// by step: the inputs and 0x0FE6 come from the vertical blank interrupt.
void selftest_colour_screen()
{
	poke(0x0188, 0);
	tick(0x72B4, 27);
	pokeb(0x1800, 0);
	tick(0x72B8, 27);
	jsr(0x72BC, 0x8068);                               // select_video_bank0
	poke(0x1100, 0);
	tick(0x72C0, 27);
	T.r[0] = 0x2000;
	tick(0x72C4, 18);
	for (;;)
	{
		poke(T.r[0], 0x0134);
		T.r[0] += 2;
		tick(0x72C8, 27);                              // MOV #0x0134,(R0)+
		fl::cmp(T.r[0], 0x37FE);
		tick(0x72CC, 18);
		if (T.r[0] > 0x37FE) break;
		tick(0x72C4, 12);                              // BLOS
	}
	tick(0x72CE, 12);
	T.r[0] = 0xFFFF;
	tick(0x72D2, 18);
	for (;;)                                           // the next colour
	{
		T.r[0]++;
		tick(0x72D4, 12);
		tick(0x72D8, 18);                              // CMP R0,#2
		if ((int16_t)T.r[0] > 2)
		{
			tick(0x72DA, 12);
			T.r[0] = 0;
			tick(0x72DC, 12);
		}
		else
			tick(0x72DC, 12);                          // BLE
		T.r[1] = T.r[0];
		tick(0x72DE, 12);
		T.r[1] = (uint16_t)(T.r[1] << 1);
		tick(0x72E0, 12);
		poke(0x1086, peek((uint16_t)(0x7050 + T.r[1])));
		tick(0x72E6, 42);
		for (;;)                                       // until bit 0x40 or the button
		{
			const bool held = (peekb(0x0193) & 0x40) != 0;
			tick(0x72EC, 30);
			if (held)
			{
				tick(0x72FA, 12);                      // BNE
				break;
			}
			tick(0x72EE, 12);
			const bool pressed = peekb(0x0FE6) != 0;
			tick(0x72F2, 24);
			if (pressed)
			{
				tick(0x72F4, 12);
				pokeb(0x0FE6, 0);
				fl::clr();
				tick(0x72F8, 27);
				rts();
				return;
			}
			tick(0x72E6, 12);                          // BEQ
		}
		for (;;)                                       // until bit 0x40 is released
		{
			const bool held = (peekb(0x0193) & 0x40) != 0;
			tick(0x7300, 30);
			if (!held)
			{
				tick(0x72D2, 12);                      // BEQ
				break;
			}
			tick(0x7302, 12);
			tick(0x72FA, 12);                          // BR
		}
	}
	rts();
}

// F:F896  rotary_zone_step(int vertical): -0x40 / +0x40 / 0 from the controller angle
// 0x01C6 (0..143): one direction for the angles around one end of the chosen axis, the other
// for the opposite end; 0 in between or while 0x0FE7 counts down
uint16_t rotary_zone_step(const Args &a)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xF898, 24);
	T.r[0] = 0;
	fl::clr();
	tick(0xF89A, 12);
	const bool waiting = peekb(0x0FE7) != 0;
	fl::tstb(peekb(0x0FE7));
	tick(0xF89E, 24);
	if (waiting)
	{
		tick(0xF8E6, 12);                              // BNE
		goto done;
	}
	tick(0xF8A0, 12);
	T.r[1] = peek(0x01C6);
	tick(0xF8A4, 24);
	{
		const int16_t r = (int16_t)T.r[1];
		const bool vertical = a.u(0) != 0;
		tick(0xF8A8, 27);                              // TST 4(SP)
		if (!vertical)
		{
			tick(0xF8AA, 12);
			tick(0xF8AE, 18);                          // CMP R1,#0x09
			if (r > 0x09)
			{
				tick(0xF8B0, 12);
				tick(0xF8B4, 18);                      // CMP R1,#0x3F
				if (r < 0x3F)
				{
					tick(0xF8E2, 12);                  // BLT
					goto plus;
				}
			}
			tick(0xF8B6, 12);                          // BLE / BLT
			tick(0xF8BA, 18);                          // CMP R1,#0x51
			if (r <= 0x51)
			{
				tick(0xF8E6, 12);                      // BLE
				goto done;
			}
			tick(0xF8BC, 12);
			tick(0xF8C0, 18);                          // CMP R1,#0x87
			if (r >= 0x87)
			{
				tick(0xF8E6, 12);                      // BGE
				goto done;
			}
			tick(0xF8C2, 12);
			tick(0xF8D0, 12);                          // BR
			goto minus;
		}
		tick(0xF8C4, 12);                              // BNE
		tick(0xF8C8, 18);                              // CMP R1,#0x2D
		if (r > 0x2D)
		{
			tick(0xF8CA, 12);
			tick(0xF8CE, 18);                          // CMP R1,#0x63
			if (r < 0x63)
			{
				tick(0xF8D0, 12);
				goto minus;
			}
		}
		tick(0xF8D6, 12);                              // BLE / BGE
		tick(0xF8DA, 18);                              // CMP R1,#0x1B
		if (r < 0x1B)
		{
			tick(0xF8E2, 12);                          // BLT
			goto plus;
		}
		tick(0xF8DC, 12);
		tick(0xF8E0, 18);                              // CMP R1,#0x75
		if (r <= 0x75)
		{
			tick(0xF8E6, 12);                          // BLE
			goto done;
		}
		tick(0xF8E2, 12);
		goto plus;
	}
minus:
	T.r[0] = 0xFFC0;
	tick(0xF8D4, 18);
	tick(0xF8E6, 12);                                  // BR
	goto done;
plus:
	T.r[0] = 0x0040;
	tick(0xF8E6, 18);
done:
	T.r[1] = pop();
	tick(0xF8E8, 18);
	rts();
	return T.r[0];
}

// F:8938 (interrupt, vertical blank in the self test): scroll registers, frame counters
// 0x01C4 / 0x0192, watchdog, inputs; a new press of the button in bit 7 of 0x0193 sets 0x0FE6.
// With IN0 bit 15 set (test switch off) the program restarts at F:8000.  Step by step; the
// RTI is in the table.
void irq_vblank_selftest()
{
	push(T.r[0]);
	fl::mov(T.r[0]);
	tick(0x893A, 24);
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x893C, 24);
	push(T.r[2]);
	fl::mov(T.r[2]);
	tick(0x893E, 24);
	push(T.r[3]);
	fl::mov(T.r[3]);
	tick(0x8940, 24);
	push(T.r[4]);
	fl::mov(T.r[4]);
	tick(0x8942, 24);
	push(T.r[5]);
	fl::mov(T.r[5]);
	tick(0x8944, 24);
	poke(0x15E0, 0);                                   // acknowledge
	fl::clr();
	tick(0x8948, 27);
	jsr(0x894C, 0x8036);                               // load_scroll_registers
	poke(0x01C4, fl::inc(peek(0x01C4)));
	tick(0x8950, 27);
	pokeb(0x0192, fl::incb(peekb(0x0192)));
	tick(0x8954, 27);
	const bool test_off = (peek(0x1800) & 0x8000) != 0;
	fl::logic((uint16_t)(0x8000 & peek(0x1800)));
	tick(0x895A, 30);
	if (test_off)
	{
		tick(0x895C, 12);
		T.icount -= 30;                                // JSR PC,@#0x8000: the reset
		push(0x8960);
		leave_to(0x8000);
	}
	tick(0x8960, 12);                                  // BEQ
	jsr(0x8964, 0x917C);                               // feed_watchdog
	jsr(0x8968, 0x80A4);                               // read_input_edges
	const bool pressed = (peekb(0x0193) & 0x80) != 0;
	fl::tstb(peekb(0x0193));
	tick(0x896C, 24);
	if (pressed)
	{
		tick(0x896E, 12);
		const bool held = peekb(0x0190) != 0;
		fl::tstb(peekb(0x0190));
		tick(0x8972, 24);
		if (held)
			tick(0x8986, 12);                          // BNE
		else
		{
			tick(0x8974, 12);
			pokeb(0x0190, 1);
			fl::movb(0x01);
			tick(0x897A, 33);
			pokeb(0x0FE6, 1);
			tick(0x8980, 33);
			tick(0x8986, 12);                          // BR
		}
	}
	else
	{
		tick(0x8982, 12);                              // BPL
		pokeb(0x0190, 0);
		fl::clr();
		tick(0x8986, 27);
	}
	poke(0x15C0, 0);                                   // acknowledge
	fl::clr();
	tick(0x898A, 27);
	jsr(0x898E, 0x8096);                               // reselect_video_bank
	T.r[5] = pop();
	fl::mov(T.r[5]);
	tick(0x8990, 18);
	T.r[4] = pop();
	fl::mov(T.r[4]);
	tick(0x8992, 18);
	T.r[3] = pop();
	fl::mov(T.r[3]);
	tick(0x8994, 18);
	T.r[2] = pop();
	fl::mov(T.r[2]);
	tick(0x8996, 18);
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x8998, 18);
	T.r[0] = pop();
	fl::mov(T.r[0]);
	tick(0x899A, 18);
}

// F:F90E (assembly, self test): scroll test: the town map (A5:4000 with 0x15, 0x0E, 0) and two
// texts (F:8AB6); until a button press the controller scrolls the playfield (F:F896).  Step
// by step; the three arguments stay on the stack and are reused for F:F896.
void selftest_scroll_screen()
{
	jsr(0xF912, 0x8CE8);                               // clear_playfield
	pokeb(0x0F2D, 1);
	tick(0xF918, 33);
	T.r[0] = 0x1800;
	tick(0xF91C, 18);
	poke(0x01D2, T.r[0]);
	tick(0xF920, 27);
	poke(0x1400, T.r[0]);
	tick(0xF924, 27);
	T.r[0] = 0x3C00;
	tick(0xF928, 18);
	poke(0x01D4, T.r[0]);
	tick(0xF92C, 27);
	poke(0x1402, T.r[0]);
	tick(0xF930, 27);
	push(0);
	tick(0xF932, 24);
	push(0x000E);
	tick(0xF936, 30);
	push(0x0015);
	tick(0xF93A, 30);
	jsr(0xF93E, 0x4000);
	poke(0x0184, 0x5BBC);
	tick(0xF944, 33);
	T.r[0] = 0x0020;
	tick(0xF948, 18);
	jsr(0xF94C, 0x8AB6);
	poke(0x0184, 0x5C04);
	tick(0xF952, 33);
	T.r[0] = 0x001F;
	tick(0xF956, 18);
	jsr(0xF95A, 0x8AB6);
	for (;;)
	{
		const uint8_t b = peekb(0x0FE6);
		fl::tstb(b);
		tick(0xF95E, 24);
		if (b)
		{
			tick(0xF984, 12);                          // BNE
			break;
		}
		tick(0xF960, 12);
		jsr(0xF964, 0x8D50);                           // wait_vblank
		jsr(0xF968, 0xF8EA);                           // button_hold_timer
		ww(T.r[6], 0);
		tick(0xF96A, 21);
		jsr(0xF96E, 0xF896);                           // rotary_zone_step(0)
		poke(0x01AE, fl::sub(peek(0x01AE), T.r[0]));
		tick(0xF972, 27);
		ww(T.r[6], 1);
		tick(0xF976, 27);
		jsr(0xF97A, 0xF896);                           // rotary_zone_step(1)
		T.r[0] = fl::add(T.r[0], peek(0x01AC));
		tick(0xF97E, 24);
		poke(0x01AC, T.r[0]);
		fl::mov(T.r[0]);
		tick(0xF982, 27);
		tick(0xF95A, 12);                              // BR
	}
	pokeb(0x0FE6, 0);
	tick(0xF988, 27);
	T.r[6] = fl::add(T.r[6], 6);
	tick(0xF98C, 18);
	rts();
}

// F:F98E (self test): clear the screen, a title (F:8AB6) and the 16 items listed at 0x7798
// (bank B 0x2800), each drawn by A7:4D30 with a 2 x 2 tile hole cleared at its position
uint16_t selftest_items_screen(const Args &)
{
	c_save(1, 0xF992);
	jsr(0xF996, 0x8068);                               // select_video_bank0
	jsr(0xF99A, 0x8D2C);                               // clear_playfield_ram
	jsr(0xF99E, 0x8CE8);                               // clear_playfield
	poke(0x0184, 0x5C04);
	tick(0xF9A4, 33);
	T.r[0] = 0x001F;
	tick(0xF9A8, 18);
	jsr(0xF9AC, 0x8AB6);
	poke(0x0184, 0x5CD2);
	fl::mov(0x5CD2);
	tick(0xF9B2, 33);
	T.r[2] = 0x000F;
	tick(0xF9B6, 18);
	for (;;)
	{
		T.r[0] = T.r[2];
		tick(0xF9B8, 12);
		for (uint16_t pc = 0xF9BA; pc <= 0xF9BE; pc += 2)
		{
			T.r[0] = (uint16_t)(T.r[0] << 1);
			tick(pc, 12);                              // ASL R0
		}
		T.r[0] = fl::add(T.r[0], 0x7798);
		tick(0xF9C2, 18);
		T.r[1] = T.r[0];
		tick(0xF9C4, 12);
		T.r[0] = 0x3800;
		tick(0xF9C8, 18);
		poke(0x01D2, T.r[0]);
		tick(0xF9CC, 27);
		poke(0x1400, T.r[0]);
		tick(0xF9D0, 27);
		T.r[0] = 0x2800;
		tick(0xF9D4, 18);
		poke(0x01D4, T.r[0]);
		tick(0xF9D8, 27);
		poke(0x1402, T.r[0]);
		tick(0xF9DC, 27);
		push(T.r[1]);
		tick(0xF9DE, 24);
		push(0);
		fl::clr();
		tick(0xF9E0, 24);
		jsr(0xF9E4, 0x4D30);
		T.r[1] = peek(T.r[1]);
		tick(0xF9E6, 18);
		T.r[1] = (uint16_t)(T.r[1] + 0x2082);
		tick(0xF9EA, 18);
		poke((uint16_t)(T.r[1] + 0x82), 0);
		fl::clr();
		tick(0xF9EE, 30);
		poke((uint16_t)(T.r[1] + 0x80), 0);
		tick(0xF9F2, 30);
		poke((uint16_t)(T.r[1] + 2), 0);
		tick(0xF9F6, 30);
		poke(T.r[1], 0);
		tick(0xF9F8, 21);
		T.r[6] += 4;
		fl::cmp(peek((uint16_t)(T.r[6] - 2)), peek((uint16_t)(T.r[6] - 2)));
		fl::cmp(peek((uint16_t)(T.r[6] - 2)), peek((uint16_t)(T.r[6] - 2)));
		fl::cmp(peek((uint16_t)(T.r[6] - 2)), peek((uint16_t)(T.r[6] - 2)));
		tick(0xF9FA, 24);                              // CMP (SP)+,(SP)+
		T.r[2]--;
		tick(0xF9FC, 12);
		if ((int16_t)T.r[2] < 0) break;
		tick(0xF9B6, 12);                              // BGE
	}
	tick(0xF9FE, 12);
	c_restore(1);
	return T.r[0];
}

}

LOGIC_TABLE(boot,
	c_routine("F", 0xF3A8, "install_selftest_interrupts", install_selftest_interrupts, 0, 0),
	irq_routine("F", 0x8924, "irq_32v_count", irq_32v_count, 24),
	wait_routine("F", 0x8D60, "wait_button_press", wait_button_press, 0),
	asm_routine("F", 0xF40C, "init_display_b6", init_display_b6, 0),
	asm_routine("F", 0xF4C0, "print_ok", print_ok, 0),
	c_routine("F", 0xF8EA, "button_hold_timer", button_hold_timer, 0, 0),
	c_routine("F", 0x8E2E, "rotary_step", rotary_step, 0, 0),
	wait_routine("B6", 0x72B0, "selftest_colour_screen", selftest_colour_screen, 0),
	c_routine("F", 0xF896, "rotary_zone_step", rotary_zone_step, 0, 0),
	irq_routine("F", 0x8938, "irq_vblank_selftest", irq_vblank_selftest, 24),
	wait_routine("F", 0xF90E, "selftest_scroll_screen", selftest_scroll_screen, 0),
	c_routine("F", 0xF98E, "selftest_items_screen", selftest_items_screen, 0, 0),
)

}
