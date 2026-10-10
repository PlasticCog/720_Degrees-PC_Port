// Rewritten routines: video/palette setup, text characters, objects, timer, input edges and
// assorted game-state helpers.  Assembly-interface routines reproduce registers and flags.
#include "logic.h"

namespace logic {
namespace {

using namespace t11rt;

uint16_t sext8(uint8_t v) { return (uint16_t)(int16_t)(int8_t)v; }

// set both bank registers (shadow copies in 0x01D2/0x01D4 and the hardware)
void set_banks(uint16_t a, uint16_t b)
{
	poke(0x01D2, a); ww(0x1400, a);
	poke(0x01D4, b); ww(0x1402, b);
}

// A7:55DE  update_ticket_bits(int limit): bit i of 0x0ED7 (masks at 0xFC74) is set when the
// byte 0x025B+i (i = 1..4) is at least limit
void update_ticket_bits()
{
	const uint16_t limit = rw((uint16_t)(T.r[6] + 2));
	const uint16_t saved_r1 = T.r[1];
	for (int i = 4; i >= 1; i--)
	{
		const uint16_t v = sext8(peekb((uint16_t)(0x025B + i)));
		fl::cmp(v, limit);
		const uint8_t bit = rb((uint16_t)(0xFC73 + i));
		uint8_t r = peekb(0x0ED7);
		r = (int16_t)v < (int16_t)limit ? (uint8_t)(r & ~bit) : (uint8_t)(r | bit);
		pokeb(0x0ED7, r);
		f_nzv(nzb(r));
	}
	T.r[0] = 0;
	T.r[1] = saved_r1;
	fl::mov(T.r[1]);
}

// F:E1FA  fill_slots(b, w1, w2, w3): slots 1..8 of the tables at 0x0ED8, 0x0EE8, 0x0EF8 get
// w3, w2, w1 and the byte table at 0x0F09 gets b
void fill_slots()
{
	const uint16_t sp = T.r[6];
	const uint16_t b = rw((uint16_t)(sp + 2)), w1 = rw((uint16_t)(sp + 4)), w2 = rw((uint16_t)(sp + 6)), w3 = rw((uint16_t)(sp + 8));
	for (int i = 8; i >= 1; i--)
	{
		poke((uint16_t)(0x0ED8 + 2 * i), w3);
		poke((uint16_t)(0x0EE8 + 2 * i), w2);
		poke((uint16_t)(0x0EF8 + 2 * i), w1);
		pokeb((uint16_t)(0x0F09 + i), (uint8_t)b);
	}
	T.r[0] = 2;
	T.psw &= ~CF;                       // from the last ASL
	fl::mov(T.r[1]);
}

// A5:5B18: sound 5, reset the 0x01DC/0x01E6 state, sound 0x7C, then A5:5BD2(0x7C)
void start_sequence_a5()
{
	call(0x8E78, {0x0005});
	poke(0x01DC, 0);
	pokeb(0x01E6, 0xFF);
	pokeb(0x01E7, 0);
	call(0x8E78, {0x007C});
	pokeb(0x01A6, 0);
	pokeb(0x01A7, 0);
	call(0x5BD2, {0x007C});
	fl::tst(0x007C);
}

// B1:7D82: on screen 7 two palette entries blink with the frame counter
void blink_screen7_palette()
{
	const uint8_t level = peekb(0x0F59);
	fl::cmpb(level, 7);
	if (level != 7) return;
	uint16_t r0 = peek(0x01C4);
	r0 = (uint16_t)((int16_t)r0 >> 2) & 0x0002;
	T.r[0] = r0;
	const uint8_t which = peekb(0x018E);
	uint16_t v;
	if (!which) { v = rw((uint16_t)(0x7A96 + r0)); ww(0x1148, v); }
	else { v = rw((uint16_t)(0x7A9A + r0)); ww(0x114C, v); }
	fl::mov(v);
}

// B3:7A9E: snapshot of the skater (height, speed, frame, 0x031D) for the attract recorder
void snapshot_skater()
{
	poke(0x0FA8, peek(0x06B2));
	poke(0x0FAA, peek(0x01B4));
	poke(0x0FAC, peek(0x01C4));
	poke(0x0FAE, 0);
	pokeb(0x0FAE, peekb(0x031D));
	poke(0x0FB2, 0);
	const uint8_t level = peekb(0x0F59);
	fl::cmpb(level, 4);
	if (level == 4)
	{
		const uint8_t air = peekb(0x01EA);
		fl::tstb(air);
		if (air) return;
	}
	pokeb(0x01F2, 0);
	fl::clr();
}

// F:8036: load the scroll registers from the pending scroll position and tile banks
void load_scroll_registers()
{
	uint16_t x = (uint16_t)((peek(0x01A8) & ~0x003F) | peek(0x01CC));
	ww(0x1700, x);
	uint16_t y = (uint16_t)((peek(0x01AA) & ~0x003F) | peek(0x01CE) | 0x0010);
	ww(0x1780, y);
	T.r[0] = y;
	poke(0x01A8, peek(0x01AC));
	const uint16_t v = peek(0x01AE);
	poke(0x01AA, v);
	fl::mov(v);
}

// F:B1C8: sprite list link word for the current entry (written to 0x3806)
void sprite_link()
{
	const uint16_t r1 = sext8(peekb(0x0993));
	const uint16_t p = (uint16_t)(0x0988 - r1);
	const uint16_t v = sext8(rb((uint16_t)(p + 1)));
	uint16_t r0;
	if ((int16_t)v < 0)
	{
		r0 = 0;
		T.r[1] = r1;
		T.psw &= ~CF;
	}
	else
	{
		T.r[1] = (uint16_t)(v << 1);
		if (v & 0x8000) T.psw |= CF; else T.psw &= ~CF;
		r0 = peek((uint16_t)(0x0888 + T.r[1]));
	}
	T.r[0] = r0;
	ww(0x3806, r0);
	fl::mov(r0);
}

// A5:51BA: objects 1-3 take the position (x, y, height) of object 0, with their bytes
void copy_object0_position()
{
	for (int i = 3; i >= 1; i--)
	{
		poke((uint16_t)(0x06F0 + 2 * i), peek(0x06F0));
		pokeb((uint16_t)(0x07FC + i), peekb(0x07FC));
		poke((uint16_t)(0x0730 + 2 * i), peek(0x0730));
		pokeb((uint16_t)(0x0800 + i), peekb(0x0800));
		poke((uint16_t)(0x06B0 + 2 * i), peek(0x06B0));
		pokeb((uint16_t)(0x0804 + i), peekb(0x0804));
	}
	T.r[0] = 0;
	T.psw &= ~CF;
	fl::mov(T.r[1]);
}

// A7:52FC: R0 = level from 0x0996 (>= 160 adds 3) and 0x099A (4-6 adds 1, 7+ adds 2)
void difficulty_step()
{
	const uint16_t a = peek(0x0996);
	fl::cmp(a, 0x00A0);
	uint16_t r0 = (int16_t)a < 0x00A0 ? 0 : 3;
	if (r0 == 0) fl::clr(); else fl::mov(r0);
	const uint16_t b = peek(0x099A);
	fl::cmp(b, 0x0004);
	if ((int16_t)b >= 4)
	{
		fl::cmp(b, 0x0007);
		if ((int16_t)b < 7) r0 = fl::inc(r0);
		else r0 = fl::add(r0, 2);
	}
	T.r[0] = r0;
}

// F:80A4: read the input port; 0x0194 keeps the inverted last state, 0x0193 collects new
// presses (bits that went from released to pressed)
void read_input_edges()
{
	uint16_t r0 = sext8(rb(0x1800));
	uint16_t r1 = sext8((uint8_t)r0);
	r0 = (uint16_t)((r0 & 0xFF00) | (uint8_t)~r0);
	uint16_t r2 = sext8(peekb(0x0194));
	r1 ^= r2;
	pokeb(0x0194, (uint8_t)r0);
	r2 = sext8(peekb(0x0193));
	r0 ^= r2;
	r1 = (uint16_t)((r1 & 0xFF00) | (uint8_t)~r1);
	r0 = (uint16_t)((r0 & 0xFF00) | (uint8_t)(r0 & ~r1));
	r2 ^= r0;
	pokeb(0x0193, (uint8_t)r2);
	T.r[0] = r0; T.r[1] = r1; T.r[2] = r2;
	f_nzvc(nzb((uint8_t)r2) | CF);
}

// A5:4894: Skate City: address (0x0F56) and bit (0x0F58) of the alternate-bank flag of the
// map cell at 0x0F48
void town_alt_cell_bit()
{
	const uint8_t level = peekb(0x0F59);
	fl::tstb(level);
	if (level) return;
	uint16_t r0 = (uint16_t)(peek(0x0F48) - 0x0180);
	poke(0x0F56, r0);
	poke(0x0F56, (uint16_t)((int16_t)peek(0x0F56) >> 1));
	r0 = peek(0x0F56) & 0x0007;
	T.r[0] = r0;
	pokeb(0x0F58, rb((uint16_t)(0xFC74 + r0)));
	poke(0x0F56, (uint16_t)((int16_t)peek(0x0F56) >> 3));
	poke(0x0F56, fl::add(peek(0x0F56), 0x7F16));
}

// A5:4A0A (registers): R2 = |R2| folded into 0..8, R1 = its sign (flipped when folded)
void fold_direction()
{
	uint16_t r1 = 0, r2 = T.r[2];
	fl::clr();
	fl::tst(r2);
	if (r2)
	{
		if ((int16_t)r2 < 0)
		{
			const int r = -(int)r2;
			f_nzvc(nzw(r) | (r2 == 0x8000 ? VF : 0) | ((r & 0xffff) ? CF : 0));
			r2 = (uint16_t)r;
			r1 = fl::dec(r1);
		}
		else
			r1 = fl::inc(r1);
	}
	fl::cmp(r2, 0x0008);
	if ((int16_t)r2 > 8)
	{
		int r = -(int)r2;
		r2 = (uint16_t)r;
		r2 = fl::add(r2, 0x0010);
		r = -(int)r1;
		f_nzvc(nzw(r) | (r1 == 0x8000 ? VF : 0) | ((r & 0xffff) ? CF : 0));
		r1 = (uint16_t)r;
	}
	T.r[1] = r1; T.r[2] = r2;
}

// B6:7E28  countdown(i): decrement the byte counter 0x0274+i; when it runs out, record
// i+1 in 0x0273, reload it from the table at 0x7C7A and set 0x01FE = 30
uint16_t countdown(const Args &a)
{
	const uint16_t i = a.u(0);
	const uint16_t p = (uint16_t)(0x0274 + i);
	const uint8_t v = (uint8_t)(peekb(p) - 1);
	pokeb(p, v);
	if ((int8_t)v > 0) return i;
	pokeb(0x0273, (uint8_t)(i + 1));
	pokeb(p, rb((uint16_t)(0x7C7A + i)));
	poke(0x01FE, 0x001E);
	return i;
}

// F:9152: next value of the 32-entry random table at 0x02C0 (index in 0x02E0)
void next_random()
{
	const uint16_t saved_r1 = T.r[1];
	const uint8_t i = (uint8_t)((peekb(0x02E0) + 1) & 0x1F);
	pokeb(0x02E0, i);
	T.r[0] = rb((uint16_t)(0x02C0 + i));
	T.r[1] = saved_r1;
	T.psw &= ~CF;
	fl::mov(T.r[1]);
}

// F:DDFE: R0 = (0x0E9B, plus one if the low nibble of 0x0AF8[2*0x0395] exceeds 0x099C) & 3
uint16_t lane_choice(const Args &)
{
	uint16_t r1 = sext8(peekb(0x0E9B));
	const uint16_t r0 = (uint16_t)(peekb(0x0395) << 1);
	const uint16_t r3 = peek(0x099C);
	const uint16_t r2 = sext8(peekb((uint16_t)(0x0AF8 + r0))) & 0x000F;
	if (r2 > r3) r1++;
	return r1 & 0x0003;
}

// F:8CE8: clear both halves of the playfield video RAM.  This takes over two frames, so the
// loops let interrupts in after every word, as the original does.
void clear_playfield()
{
	wb(0x1800, 0);
	T.r[0] = 0x4000;
	call(0x8078, {});
	for (uint32_t a = 0x2000; a <= 0x3FFE; a += 2)
	{
		ww((uint16_t)a, 0);
		tick(0x8CF8, 21 + 18 + 12);
	}
	wb(0x1800, 0);
	T.r[0] = 0x6000;
	call(0x8078, {});
	for (uint32_t a = 0x2000; a <= 0x3FFE; a += 2)
	{
		ww((uint16_t)a, 0);
		tick(0x8D10, 21 + 18 + 12);
	}
	T.r[0] = 0x4000;
	fl::cmp(0x4000, 0x3FFE);
}

// F:CDFA  find_other(i, value): highest slot j (from 0x09B0 down) with 0x0AF8[j] == value
// and j != i; R0 = j or -1
void find_other_slot()
{
	const uint16_t saved_r1 = T.r[1];
	const uint16_t sp = T.r[6];
	const uint16_t i = rw((uint16_t)(sp + 2)), value = rw((uint16_t)(sp + 4));
	uint16_t r1 = peek(0x09B0);
	uint16_t r0 = 0xFFFF;
	f_nzv(nzw(r1));
	if ((int16_t)r1 >= 0)
	{
		for (;;)
		{
			r0 = (uint16_t)(r1 << 1);
			if (r1 & 0x8000) T.psw |= CF; else T.psw &= ~CF;
			const uint16_t w = peek((uint16_t)(0x0AF8 + r0));
			fl::cmp(w, value);
			if (w == value)
			{
				fl::cmp(r1, i);
				if (r1 != i) { r0 = r1; break; }
			}
			r1 = fl::dec(r1);
			if ((int16_t)r1 < 0) { r0 = 0xFFFF; break; }
		}
	}
	T.r[0] = r0;
	T.r[1] = saved_r1;
	fl::mov(T.r[1]);
}

// F:8C56 (character code in R0): draw a 2x2-tile character of the large font into the text
// layer at the cursor (0x0186), which advances by one character
void draw_big_char()
{
	uint16_t r0 = (uint16_t)(T.r[0] - 0x0030);
	r0 = (uint16_t)(r0 << 2);
	r0 |= peek(0x0188);
	uint16_t r1 = peek(0x0186);
	r0 = (uint16_t)(r0 + peek(0x018A));
	ww(r1, r0); r1 += 2;
	r0++;
	ww(r1, r0); r1 += 2;
	poke(0x0186, r1);
	fl::add(r1, 0x007C);
	r1 = (uint16_t)(r1 + 0x007C);
	r0++;
	ww(r1, r0); r1 += 2;
	r0++;
	ww(r1, r0);
	T.r[0] = r0;
	fl::mov(T.r[1]);                     // R1/R2 are restored by the register-save helper
}

// F:E6FE: bit mask of parks (masks at 0xFC74, 3..0) that are open: unlocked in 0x0F29, or,
// in mode 1 of 0x0F2B, every park except the one in 0x02B8
uint16_t open_parks(const Args &)
{
	uint16_t r0 = 0;
	for (int i = 3; i >= 0; i--)
	{
		const uint8_t bit = rb((uint16_t)(0xFC74 + i));
		bool set = (peekb(0x0F29) & bit) != 0;
		if (!set && peekb(0x0F2B) == 1) set = i != peekb(0x02B8);
		if (set) r0 |= bit;
	}
	return r0;
}

// F:F028  add_time(n): n = 0 starts the timer (0x0254 = 127) and its countdown bytes from
// the table at 0x51E6; otherwise n is added and the timer stops at 0
void add_time()
{
	pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) | 0x04));
	const uint16_t n = rw((uint16_t)(T.r[6] + 2));
	T.r[0] = n;
	if (n == 0)
	{
		const uint16_t a = peekb(0x0F2E);
		T.r[1] = peekb(0x0FE4);
		T.r[0] = fl::add(a, T.r[1]);
		const uint8_t c = rb((uint16_t)(0x51E6 + T.r[0]));
		pokeb(0x0FA7, c);
		pokeb(0x0FA6, rb((uint16_t)(0x51E6 + T.r[0])));
		poke(0x0254, 0x007F);
		fl::mov(0x007F);
		return;
	}
	const uint16_t t = fl::add(peek(0x0254), n);
	poke(0x0254, t);
	T.psw &= ~(CF | VF);
	if ((int16_t)t < 0)
	{
		poke(0x0254, 0);
		fl::clr();
	}
}

// F:C0D8: per-slot value: 0x0AB8[slot] = table(0xFD82)[table(0x65CC, bank 0x0400)[0x09B8[slot]]]
// + 0x0E9A, for slot = 0x09AA; switches bank A to 0 and bank B to 0x0400
uint16_t slot_value(const Args &)
{
	set_banks(0x0000, 0x0400);
	const uint16_t slot = peek(0x09AA);
	uint16_t r0 = sext8(peekb((uint16_t)(0x09B8 + slot)));
	r0 = sext8(rb((uint16_t)(0x65CC + r0)));
	const uint16_t v = (uint16_t)(rb((uint16_t)(0xFD82 + r0)) + peekb(0x0E9A));
	pokeb((uint16_t)(0x0AB8 + slot), (uint8_t)v);
	return 0;
}

// F:DA74: copy 32 colours from ROM (bank 0x2000, 0x5EC0) to palette entries 0x40-0x5F
void load_text_palette()
{
	const uint16_t a = peek(0x01D2), b = peek(0x01D4);
	set_banks(0x2000, 0x2400);
	for (int i = 32; i >= 1; i--) ww((uint16_t)(0x107E + 2 * i), rw((uint16_t)(0x5EBE + 2 * i)));
	poke(0x01D2, a);
	poke(0x01D4, b);
	ww(0x1400, peek(0x01D2));
	const uint16_t v = peek(0x01D4);
	ww(0x1402, v);
	T.r[0] = 2; T.r[1] = 0; T.r[2] = a; T.r[3] = b;
	T.psw &= ~CF;
	fl::mov(v);
}

// F:DABA: copy 128 colours from ROM (bank 0x1C00, 0x7DAC) to palette entries 0x80-0xFF
void load_playfield_palette()
{
	const uint16_t a = peek(0x01D2), b = peek(0x01D4);
	set_banks(0x1800, 0x1C00);
	for (int i = 0x7F; i >= 0; i--) ww((uint16_t)(0x1100 + 2 * i), rw((uint16_t)(0x7DAC + 2 * i)));
	poke(0x01D2, a);
	poke(0x01D4, b);
	ww(0x1400, peek(0x01D2));
	const uint16_t v = peek(0x01D4);
	ww(0x1402, v);
	T.r[0] = 0x1100; T.r[1] = 0x7DAC; T.r[2] = 0; T.r[3] = a; T.r[4] = b;
	fl::mov(v);
}

}

LOGIC_TABLE(misc,
	asm_routine("A7", 0x55DE, "update_ticket_bits", update_ticket_bits, 633),
	asm_routine("F", 0xE1FA, "fill_slots", fill_slots, 1857),
	asm_routine("A5", 0x5B18, "start_sequence_a5", start_sequence_a5, 328),
	asm_routine("B1", 0x7D82, "blink_screen7_palette", blink_screen7_palette, 85),
	asm_routine("B3", 0x7A9E, "snapshot_skater", snapshot_skater, 300),
	asm_routine("F", 0x8036, "load_scroll_registers", load_scroll_registers, 303),
	asm_routine("F", 0xB1C8, "sprite_link", sprite_link, 189),
	asm_routine("A5", 0x51BA, "copy_object0_position", copy_object0_position, 963),
	asm_routine("A7", 0x52FC, "difficulty_step", difficulty_step, 165),
	asm_routine("F", 0x80A4, "read_input_edges", read_input_edges, 231),
	asm_routine("A5", 0x4894, "town_alt_cell_bit", town_alt_cell_bit, 191),
	asm_routine("A5", 0x4A0A, "fold_direction", fold_direction, 96),
	c_routine("B6", 0x7E28, "countdown", countdown, 0, 365),
	asm_routine("F", 0x9152, "next_random", next_random, 276),
	c_routine("F", 0xDDFE, "lane_choice", lane_choice, 0, 462),
	asm_routine("F", 0x8CE8, "clear_playfield", clear_playfield, 203),
	asm_routine("F", 0xCDFA, "find_other_slot", find_other_slot, 4331),
	asm_routine("F", 0x8C56, "draw_big_char", draw_big_char, 504),
	c_routine("F", 0xE6FE, "open_parks", open_parks, 0, 725),
	asm_routine("F", 0xF028, "add_time", add_time, 152),
	c_routine("F", 0xC0D8, "slot_value", slot_value, 0, 612),
	asm_routine("F", 0xDA74, "load_text_palette", load_text_palette, 3147),
	asm_routine("F", 0xDABA, "load_playfield_palette", load_playfield_palette, 6927),
)

}
