// Rewritten routines: power-on, self test and the controller.
#include "logic.h"

namespace logic {
namespace {

using namespace t11rt;

// R1..Rn as the register-save helper of a C routine restores them on return (callees that
// are not C routines may change them)
struct KeepRegs {
	int n;
	uint16_t r[6];
	explicit KeepRegs(int count) : n(count) { for (int i = 1; i <= n; i++) r[i] = T.r[i]; }
	~KeepRegs() { for (int i = 1; i <= n; i++) T.r[i] = r[i]; }
};

// F:F3A8: interrupt handlers of the self test: the eight vectors listed at 0xFC4E get F:8938
// (PSW 0xE0), the four 32V vectors at 0xFC5E F:8924 (PSW 0xC0), the sound command vector
// 0x0038 F:891E (PSW 0xA0); only the 32V interrupt is enabled.  Returns the last vector.
uint16_t install_selftest_interrupts(const Args &)
{
	poke(0x1600, 0);
	poke(0x1580, 0);
	poke(0x15E0, 0);
	poke(0x15C0, 0);
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
	poke(0x0038, 0x891E);
	poke(0x003A, 0x00A0);
	poke(0x018C, 0x0008);
	poke(0x1600, 0x0008);
	return v;
}

// F:8924 (interrupt, 32V in the self test): count the quarter frame in 0x0191
void irq_32v_count()
{
	pokeb(0x0191, (uint8_t)(peekb(0x0191) + 1));
	poke(0x15C0, 0);                                   // acknowledge
}

// F:8D60 (assembly): wait, frame by frame, until the vertical blank interrupt reports a
// button press in 0x0FE6; clear it again
void wait_button_press()
{
	pokeb(0x0FE6, 0);
	T.icount -= 27;
	do
	{
		T.icount -= 30;
		call(0x8D50, {});                              // wait_vblank
		T.icount -= 24 + 12;
	} while (!peekb(0x0FE6));
	pokeb(0x0FE6, 0);
	T.icount -= 27;
	fl::clr();
}

// F:F40C (assembly): load the palettes (F:DABA, F:DA74, F:DB4E), text colour 0xE000, and run
// the four display routines of bank B 0x2800 (B6:69B2, 6F48, 6B7E, 6CFC)
void init_display_b6()
{
	call(0xDABA, {});
	call(0xDA74, {});
	call(0xDB4E, {});
	poke(0x0188, 0xE000);
	T.r[0] = 0x2800;
	poke(0x01D4, 0x2800);
	poke(0x1402, 0x2800);
	call(0x69B2, {});
	call(0x6F48, {});
	call(0x6B7E, {});
	call(0x6CFC, {});
}

// F:F4C0 (assembly): "OK" in the large font
void print_ok()
{
	T.r[0] = 0x004F;
	call(0x8B52, {});
	T.r[0] = 0x004B;
	call(0x8B52, {});
}

// F:F8EA: 0x0FE7 counts down while input bit 0x40 of 0x0193 is held (60 otherwise), then
// F:8D86 runs
uint16_t button_hold_timer(const Args &)
{
	KeepRegs keep(5);
	if (peekb(0x0193) & 0x40)
	{
		if (peekb(0x0FE7)) pokeb(0x0FE7, (uint8_t)(peekb(0x0FE7) - 1));
	}
	else
		pokeb(0x0FE7, 0x3C);
	return call(0x8D86, {});
}

// F:8E2E  rotary_step(): direction in which the controller (0x01C6, 144 counts per turn)
// moved since the last step position 0x02BC: +1 / -1, the shorter way round; 0 while it is
// less than 16 counts away (otherwise the step position follows)
uint16_t rotary_step(const Args &)
{
	int16_t d = (int16_t)(peek(0x01C6) - peek(0x02BC));
	int16_t r0;
	if (d < 0)
	{
		r0 = -1;
		d = (int16_t)-d;
	}
	else
		r0 = 1;
	if (d > 0x48)
	{
		r0 = (int16_t)-r0;
		d = (int16_t)(-d + 0x90);
	}
	if (d < 0x10) return 0;
	poke(0x02BC, peek(0x01C6));
	return (uint16_t)r0;
}


// B6:72B0 (assembly, self test): fill the playfield with tile 0x0134 and wait for a button
// press (0x0FE6); each press and release of input bit 0x40 (0x0193) steps the colour 0x1086
// through the three values at 0x7050.  R0 ends as the colour index, R1 as twice that.
void selftest_colour_screen()
{
	poke(0x0188, 0);
	pokeb(0x1800, 0);
	T.icount -= 27 + 27 + 30;
	call(0x8068, {});                                  // select_video_bank0
	poke(0x1100, 0);
	T.icount -= 27 + 18;
	for (uint16_t a = 0x2000; a <= 0x37FE; a = (uint16_t)(a + 2))
	{
		poke(a, 0x0134);
		tick(0x72C4, 27 + 18 + 12);
	}
	T.icount -= 18;
	uint16_t r0 = 0xFFFF;
	for (;;)
	{
		r0++;
		T.icount -= 12 + 18 + 12;
		if ((int16_t)r0 > 2)
		{
			r0 = 0;
			T.icount -= 12;
		}
		T.r[0] = r0;
		T.r[1] = (uint16_t)(r0 * 2);
		poke(0x1086, peek((uint16_t)(0x7050 + T.r[1])));
		T.icount -= 12 + 12 + 42;
		for (;;)
		{
			if (peekb(0x0193) & 0x40)
			{
				T.icount -= 30 + 12;
				break;
			}
			if (peekb(0x0FE6))
			{
				T.icount -= 30 + 12 + 24 + 12 + 27;
				pokeb(0x0FE6, 0);
				fl::clr();
				return;
			}
			tick(0x72E6, 30 + 12 + 24 + 12);
		}
		while (peekb(0x0193) & 0x40) tick(0x72FA, 30 + 12 + 12);
		T.icount -= 30 + 12;
	}
}

// F:F896  rotary_zone_step(int vertical): -0x40 / +0x40 / 0 from the controller angle
// 0x01C6 (0..143): one direction for the angles around one end of the chosen axis, the other
// for the opposite end; 0 in between or while 0x0FE7 counts down
uint16_t rotary_zone_step(const Args &a)
{
	if (peekb(0x0FE7)) return 0;
	const int16_t r = (int16_t)peek(0x01C6);
	if (a.u(0) == 0)
	{
		if (r > 0x09 && r < 0x3F) return 0x0040;
		if (r <= 0x51 || r >= 0x87) return 0;
		return 0xFFC0;
	}
	if (r > 0x2D && r < 0x63) return 0xFFC0;
	if (r < 0x1B) return 0x0040;
	if (r <= 0x75) return 0;
	return 0x0040;
}


// F:8938 (interrupt, vertical blank in the self test): scroll registers, frame counters
// 0x01C4 / 0x0192, watchdog, inputs; a new press of the button in bit 7 of 0x0193 sets 0x0FE6.
// With IN0 bit 15 set (test switch off) the program restarts at F:8000.
void irq_vblank_selftest()
{
	uint16_t saved[6];
	for (int i = 0; i < 6; i++) saved[i] = T.r[i];
	poke(0x15E0, 0);                                   // acknowledge
	call(0x8036, {});                                  // load_scroll_registers
	poke(0x01C4, (uint16_t)(peek(0x01C4) + 1));
	pokeb(0x0192, (uint8_t)(peekb(0x0192) + 1));
	if (peek(0x1800) & 0x8000)
	{
		for (int i = 0; i < 6; i++) push(saved[i]);
		push(0x8960);
		leave_to(0x8000);                              // reset
	}
	call(0x917C, {});                                  // feed_watchdog
	call(0x80A4, {});                                  // read_input_edges
	if (peekb(0x0193) & 0x80)
	{
		if (!peekb(0x0190))
		{
			pokeb(0x0190, 1);
			pokeb(0x0FE6, 1);
		}
	}
	else
		pokeb(0x0190, 0);
	poke(0x15C0, 0);                                   // acknowledge
	call(0x8096, {});                                  // reselect_video_bank
	for (int i = 0; i < 6; i++) T.r[i] = saved[i];
}

// F:F90E (assembly, self test): scroll test: the town map (A5:4000 with 0x15, 0x0E, 0) and two
// texts (F:8AB6); until a button press the controller scrolls the playfield (F:F896)
void selftest_scroll_screen()
{
	T.icount -= 30;
	call(0x8CE8, {});                                  // clear_playfield
	pokeb(0x0F2D, 1);
	poke(0x01D2, 0x1800);
	poke(0x1400, 0x1800);
	T.r[0] = 0x3C00;
	poke(0x01D4, 0x3C00);
	poke(0x1402, 0x3C00);
	T.icount -= 33 + 18 + 27 + 27 + 18 + 27 + 27 + 24 + 30 + 30 + 30;
	call(0x4000, {0x0015, 0x000E, 0});
	poke(0x0184, 0x5BBC);
	T.r[0] = 0x0020;
	T.icount -= 33 + 18 + 30;
	call(0x8AB6, {});
	poke(0x0184, 0x5C04);
	T.r[0] = 0x001F;
	T.icount -= 33 + 18 + 30;
	call(0x8AB6, {});
	for (;;)
	{
		T.icount -= 24 + 12;
		if (peekb(0x0FE6)) break;
		T.icount -= 30;
		call(0x8D50, {});                              // wait_vblank
		T.icount -= 30;
		call(0xF8EA, {});                              // button_hold_timer
		T.icount -= 21 + 30;
		uint16_t d = call(0xF896, {0});                // rotary_zone_step
		poke(0x01AE, (uint16_t)(peek(0x01AE) - d));
		T.icount -= 27 + 27 + 30;
		d = call(0xF896, {1});
		T.r[0] = (uint16_t)(d + peek(0x01AC));
		poke(0x01AC, T.r[0]);
		T.icount -= 24 + 27 + 12;
	}
	pokeb(0x0FE6, 0);
	T.icount -= 27 + 18;
	fl::add((uint16_t)(T.r[6] - 6), 6);                // ADD #6,SP
}

// F:F98E (self test): clear the screen, a title (F:8AB6) and the 16 items listed at 0x7798
// (bank B 0x2800), each drawn by A7:4D30 with a 2 x 2 tile hole cleared at its position
uint16_t selftest_items_screen(const Args &)
{
	KeepRegs keep(2);
	call(0x8068, {});                                  // select_video_bank0
	call(0x8D2C, {});                                  // clear_playfield_ram
	call(0x8CE8, {});                                  // clear_playfield
	poke(0x0184, 0x5C04);
	T.r[0] = 0x001F;
	call(0x8AB6, {});
	poke(0x0184, 0x5CD2);
	uint16_t r0 = T.r[0];
	for (int i = 15; i >= 0; i--)
	{
		const uint16_t item = (uint16_t)(0x7798 + i * 8);
		poke(0x01D2, 0x3800);
		poke(0x1400, 0x3800);
		T.r[0] = 0x2800;
		poke(0x01D4, 0x2800);
		poke(0x1402, 0x2800);
		r0 = call(0x4D30, {0, item});
		const uint16_t p = (uint16_t)(peek(item) + 0x2082);
		poke((uint16_t)(p + 0x82), 0);
		poke((uint16_t)(p + 0x80), 0);
		poke((uint16_t)(p + 2), 0);
		poke(p, 0);
	}
	return r0;
}

}

LOGIC_TABLE(boot,
	c_routine("F", 0xF3A8, "install_selftest_interrupts", install_selftest_interrupts, 0, 1959),
	irq_routine("F", 0x8924, "irq_32v_count", irq_32v_count, 156),
	wait_routine("F", 0x8D60, "wait_button_press", wait_button_press, 21),
	asm_routine("F", 0xF40C, "init_display_b6", init_display_b6, 336),
	asm_routine("F", 0xF4C0, "print_ok", print_ok, 117),
	c_routine("F", 0xF8EA, "button_hold_timer", button_hold_timer, 0, 453),
	c_routine("F", 0x8E2E, "rotary_step", rotary_step, 0, 230),
	wait_routine("B6", 0x72B0, "selftest_colour_screen", selftest_colour_screen, 21),
	c_routine("F", 0xF896, "rotary_zone_step", rotary_zone_step, 0, 111),
	irq_routine("F", 0x8938, "irq_vblank_selftest", irq_vblank_selftest, 612),
	wait_routine("F", 0xF90E, "selftest_scroll_screen", selftest_scroll_screen, 21),
	c_routine("F", 0xF98E, "selftest_items_screen", selftest_items_screen, 0, 8367),
)

}
