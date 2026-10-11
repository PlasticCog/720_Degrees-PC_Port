// Rewritten routines: video/palette setup, text characters, objects, timer, input edges and
// assorted game-state helpers.  Assembly-interface routines reproduce registers and flags.
// All follow the original instruction by instruction (tick after every instruction, the
// stack as the original's; see routines_core.cpp).
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

// NEG as the T-11 sets the flags
uint16_t neg(uint16_t v)
{
	const int r = -(int)v;
	f_nzvc(nzw(r) | (v == 0x8000 ? VF : 0) | ((r & 0xffff) ? CF : 0));
	return (uint16_t)r;
}

// A7:55DE  update_ticket_bits(int limit): bit i of 0x0ED7 (masks at 0xFC74) is set when the
// byte 0x025B+i (i = 1..4) is at least limit
void update_ticket_bits()
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x55E0, 24);
	T.r[0] = 4;
	fl::mov(T.r[0]);
	tick(0x55E4, 18);
	for (;;)
	{
		T.r[1] = sext8(peekb((uint16_t)(0x025B + T.r[0])));
		fl::movb((uint8_t)T.r[1]);
		tick(0x55E8, 27);
		const uint16_t limit = rw((uint16_t)(T.r[6] + 4));
		fl::cmp(T.r[1], limit);
		tick(0x55EC, 27);
		const uint8_t bit = rb((uint16_t)(0xFC73 + T.r[0]));
		if ((int16_t)T.r[1] < (int16_t)limit)
		{
			tick(0x55EE, 12);
			const uint8_t r = (uint8_t)(peekb(0x0ED7) & ~bit);
			pokeb(0x0ED7, r);
			f_nzv(nzb(r));
			tick(0x55F4, 42);
			tick(0x55FC, 12);                          // BR
		}
		else
		{
			tick(0x55F6, 12);                          // BGE
			const uint8_t r = (uint8_t)(peekb(0x0ED7) | bit);
			pokeb(0x0ED7, r);
			f_nzv(nzb(r));
			tick(0x55FC, 42);
		}
		if (--T.r[0] == 0) break;
		tick(0x55E4, 18);                              // SOB
	}
	tick(0x55FE, 18);
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x5600, 18);
	rts();
}

// F:E1FA  fill_slots(b, w1, w2, w3): slots 1..8 of the tables at 0x0ED8, 0x0EE8, 0x0EF8 get
// w3, w2, w1 and the byte table at 0x0F09 gets b
void fill_slots()
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xE1FC, 24);
	T.r[1] = 8;
	fl::mov(T.r[1]);
	tick(0xE200, 18);
	for (;;)
	{
		T.r[0] = T.r[1];
		fl::mov(T.r[0]);
		tick(0xE202, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0xE204, 12);
		const uint16_t sp = T.r[6];
		poke((uint16_t)(0x0ED8 + T.r[0]), rw((uint16_t)(sp + 0x0A)));
		fl::mov(peek((uint16_t)(T.r[6] + 0x0A)));
		tick(0xE20A, 45);
		poke((uint16_t)(0x0EE8 + T.r[0]), rw((uint16_t)(sp + 8)));
		fl::mov(peek((uint16_t)(T.r[6] + 8)));
		tick(0xE210, 45);
		poke((uint16_t)(0x0EF8 + T.r[0]), rw((uint16_t)(sp + 6)));
		fl::mov(peek((uint16_t)(T.r[6] + 6)));
		tick(0xE216, 45);
		pokeb((uint16_t)(0x0F09 + T.r[1]), rb((uint16_t)(sp + 4)));
		fl::movb(peekb((uint16_t)(T.r[6] + 4)));
		tick(0xE21C, 45);
		if (--T.r[1] == 0) break;
		tick(0xE200, 18);                              // SOB
	}
	tick(0xE21E, 18);
	T.r[1] = pop();
	T.psw &= ~CF;                                      // from the last ASL
	fl::mov(T.r[1]);
	tick(0xE220, 18);
	rts();
}

// A5:5B18: sound 5, reset the 0x01DC/0x01E6 state, sound 0x7C, then A5:5BD2(0x7C)
// (step by step: the vertical blank interrupt sends the queued sounds)
void start_sequence_a5()
{
	push(0x0005);
	fl::mov(0x0005);
	tick(0x5B1C, 30);
	jsr(0x5B20, 0x8E78);                               // queue_sound
	poke(0x01DC, 0);
	fl::clr();
	tick(0x5B24, 27);
	pokeb(0x01E6, 0xFF);
	fl::movb(0xFF);
	tick(0x5B2A, 33);
	pokeb(0x01E7, 0);
	fl::clr();
	tick(0x5B2E, 27);
	ww(T.r[6], 0x007C);
	fl::mov(0x007C);
	tick(0x5B32, 27);
	jsr(0x5B36, 0x8E78);
	pokeb(0x01A6, 0);
	fl::clr();
	tick(0x5B3A, 27);
	pokeb(0x01A7, 0);
	fl::clr();
	tick(0x5B3E, 27);
	jsr(0x5B42, 0x5BD2);
	fl::tst(pop());
	tick(0x5B44, 18);                                  // TST (SP)+
	rts();
}

// B1:7D82: on screen 7 two palette entries blink with the frame counter (step by step)
void blink_screen7_palette()
{
	const uint8_t level = peekb(0x0F59);
	fl::cmpb(level, 7);
	tick(0x7D88, 30);
	if (level != 7)
	{
		tick(0x7DAA, 12);                              // BNE
		rts();
		return;
	}
	tick(0x7D8A, 12);
	T.r[0] = peek(0x01C4);
	fl::mov(T.r[0]);
	tick(0x7D8E, 24);
	T.r[0] = fl::asr(T.r[0]);
	tick(0x7D90, 12);
	T.r[0] = fl::asr(T.r[0]);
	tick(0x7D92, 12);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0002));
	tick(0x7D96, 18);
	const uint8_t which = peekb(0x018E);
	fl::tstb(peekb(0x018E));
	tick(0x7D9A, 24);
	uint16_t v;
	if (!which)
	{
		tick(0x7D9C, 12);
		v = rw((uint16_t)(0x7A96 + T.r[0]));
		ww(0x1148, v);
		fl::mov(v);
		tick(0x7DA2, 42);
	}
	else
	{
		tick(0x7DA4, 12);                              // BNE
		v = rw((uint16_t)(0x7A9A + T.r[0]));
		ww(0x114C, v);
		fl::mov(v);
		tick(0x7DAA, 42);
	}
	rts();
}

// B3:7A9E: snapshot of the skater (height, speed, frame, 0x031D) for the attract recorder
void snapshot_skater()
{
	poke(0x0FA8, peek(0x06B2));
	fl::mov(peek(0x0FA8));
	tick(0x7AA4, 39);
	poke(0x0FAA, peek(0x01B4));
	fl::mov(peek(0x0FAA));
	tick(0x7AAA, 39);
	poke(0x0FAC, peek(0x01C4));
	fl::mov(peek(0x0FAC));
	tick(0x7AB0, 39);
	poke(0x0FAE, 0);
	fl::clr();
	tick(0x7AB4, 27);
	pokeb(0x0FAE, peekb(0x031D));
	fl::movb(peekb(0x0FAE));
	tick(0x7ABA, 39);
	poke(0x0FB2, 0);
	fl::clr();
	tick(0x7ABE, 27);
	const uint8_t level = peekb(0x0F59);
	fl::cmpb(level, 4);
	tick(0x7AC4, 30);
	if (level == 4)
	{
		tick(0x7AC6, 12);
		const uint8_t air = peekb(0x01EA);
		fl::tstb(air);
		tick(0x7ACA, 24);
		if (air)
		{
			tick(0x7AD0, 12);                          // BNE
			rts();
			return;
		}
	}
	tick(0x7ACC, 12);                                  // BNE / BNE
	pokeb(0x01F2, 0);
	fl::clr();
	tick(0x7AD0, 27);
	rts();
}

// F:8036: load the scroll registers from the pending scroll position and tile banks.  Step by
// step; the code lies in the slapstic's range, which sees its instruction fetches too.
void load_scroll_registers()
{
	slap(0x8036);
	slap(0x8038);
	T.r[0] = peek(0x01A8);
	fl::mov(T.r[0]);
	tick(0x803A, 24);
	slap(0x803A);
	slap(0x803C);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & ~0x003F));
	tick(0x803E, 18);
	slap(0x803E);
	T.r[0] = fl::logic((uint16_t)(T.r[0] | peek(0x01CC)));
	tick(0x8042, 24);
	ww(0x1700, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x8046, 27);
	T.r[0] = peek(0x01AA);
	fl::mov(T.r[0]);
	tick(0x804A, 24);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & ~0x003F));
	tick(0x804E, 18);
	T.r[0] = fl::logic((uint16_t)(T.r[0] | peek(0x01CE)));
	tick(0x8052, 24);
	T.r[0] = fl::logic((uint16_t)(T.r[0] | 0x0010));
	tick(0x8056, 18);
	ww(0x1780, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x805A, 27);
	poke(0x01A8, peek(0x01AC));
	fl::mov(peek(0x01A8));
	tick(0x8060, 39);
	const uint16_t v = peek(0x01AE);
	poke(0x01AA, v);
	fl::mov(v);
	tick(0x8066, 39);
	rts();
}

// F:B1C8: sprite list link word for the current entry (written to 0x3806), step by step
void sprite_link()
{
	T.r[0] = 0x0988;
	fl::mov(T.r[0]);
	tick(0xB1CC, 18);
	T.r[1] = sext8(peekb(0x0993));
	fl::movb((uint8_t)T.r[1]);
	tick(0xB1D0, 24);
	T.r[0] = fl::sub(T.r[0], T.r[1]);
	tick(0xB1D2, 12);
	T.r[0] = sext8(rb((uint16_t)(T.r[0] + 1)));
	fl::movb((uint8_t)T.r[0]);
	tick(0xB1D6, 27);
	if ((int16_t)T.r[0] < 0)
	{
		tick(0xB1D8, 12);
		T.r[0] = 0;
		fl::clr();
		tick(0xB1DA, 12);
		tick(0xB1E4, 12);                              // BR
	}
	else
	{
		tick(0xB1DC, 12);                              // BGE
		T.r[1] = T.r[0];
		fl::mov(T.r[1]);
		tick(0xB1DE, 12);
		T.r[1] = fl::asl(T.r[1]);
		tick(0xB1E0, 12);
		T.r[0] = peek((uint16_t)(0x0888 + T.r[1]));
		fl::mov(T.r[0]);
		tick(0xB1E4, 27);
	}
	ww(0x3806, T.r[0]);
	f_nzvc(nzw(T.r[0]));                               // C is clear from the CLR or the ASL
	tick(0xB1E8, 27);
	rts();
}

// A5:51BA: objects 1-3 take the position (x, y, height) of object 0, with their bytes
void copy_object0_position()
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x51BC, 24);
	T.r[0] = 3;
	fl::mov(T.r[0]);
	tick(0x51C0, 18);
	for (;;)
	{
		T.r[1] = T.r[0];
		fl::mov(T.r[1]);
		tick(0x51C2, 12);
		T.r[1] = fl::asl(T.r[1]);
		tick(0x51C4, 12);
		poke((uint16_t)(0x06F0 + T.r[1]), peek(0x06F0));
		fl::mov(peek(0x06F0));
		tick(0x51CA, 42);
		pokeb((uint16_t)(0x07FC + T.r[0]), peekb(0x07FC));
		fl::movb(peekb(0x07FC));
		tick(0x51D0, 42);
		poke((uint16_t)(0x0730 + T.r[1]), peek(0x0730));
		fl::mov(peek(0x0730));
		tick(0x51D6, 42);
		pokeb((uint16_t)(0x0800 + T.r[0]), peekb(0x0800));
		fl::movb(peekb(0x0800));
		tick(0x51DC, 42);
		poke((uint16_t)(0x06B0 + T.r[1]), peek(0x06B0));
		fl::mov(peek(0x06B0));
		tick(0x51E2, 42);
		pokeb((uint16_t)(0x0804 + T.r[0]), peekb(0x0804));
		fl::movb(peekb(0x0804));
		tick(0x51E8, 42);
		if (--T.r[0] == 0) break;
		tick(0x51C0, 18);                              // SOB
	}
	tick(0x51EA, 18);
	T.r[1] = pop();
	T.psw &= ~CF;                                      // from the last ASL
	fl::mov(T.r[1]);
	tick(0x51EC, 18);
	rts();
}

// A7:52FC: R0 = level from 0x0996 (>= 160 adds 3) and 0x099A (4-6 adds 1, 7+ adds 2)
void difficulty_step()
{
	const uint16_t a = peek(0x0996);
	fl::cmp(a, 0x00A0);
	tick(0x5302, 30);
	if ((int16_t)a < 0x00A0)
	{
		tick(0x530A, 12);                              // BLT
		T.r[0] = 0;
		fl::clr();
		tick(0x530C, 12);
	}
	else
	{
		tick(0x5304, 12);
		T.r[0] = 3;
		fl::mov(3);
		tick(0x5308, 18);
		tick(0x530C, 12);                              // BR
	}
	const uint16_t b = peek(0x099A);
	fl::cmp(b, 0x0004);
	tick(0x5312, 30);
	if ((int16_t)b < 4)
	{
		tick(0x5324, 12);                              // BLT
		rts();
		return;
	}
	tick(0x5314, 12);
	const uint16_t b2 = peek(0x099A);
	fl::cmp(b2, 0x0007);
	tick(0x531A, 30);
	if ((int16_t)b2 < 7)
	{
		tick(0x531C, 12);
		T.r[0] = fl::inc(T.r[0]);
		tick(0x531E, 12);
		rts();
		return;
	}
	tick(0x5320, 12);                                  // BGE
	T.r[0] = fl::add(T.r[0], 2);
	tick(0x5324, 18);
	rts();
}

// F:80A4: read the input port; 0x0194 keeps the inverted last state, 0x0193 collects new
// presses (bits that went from released to pressed).  Step by step.
void read_input_edges()
{
	T.r[0] = sext8(rb(0x1800));
	fl::movb((uint8_t)T.r[0]);
	tick(0x80A8, 24);
	T.r[1] = sext8((uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[1]);
	tick(0x80AA, 12);
	T.r[0] = (uint16_t)((T.r[0] & 0xFF00) | (uint8_t)~T.r[0]);
	fl::comb((uint8_t)(~(uint8_t)T.r[0]));
	tick(0x80AC, 12);                                  // COMB R0
	T.r[2] = sext8(peekb(0x0194));
	fl::movb((uint8_t)T.r[2]);
	tick(0x80B0, 24);
	T.r[1] ^= T.r[2];
	fl::logic(T.r[1]);
	tick(0x80B2, 12);
	pokeb(0x0194, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x80B6, 27);
	T.r[2] = sext8(peekb(0x0193));
	fl::movb((uint8_t)T.r[2]);
	tick(0x80BA, 24);
	T.r[0] ^= T.r[2];
	fl::logic(T.r[0]);
	tick(0x80BC, 12);
	T.r[1] = (uint16_t)((T.r[1] & 0xFF00) | (uint8_t)~T.r[1]);
	fl::comb((uint8_t)(~(uint8_t)T.r[1]));
	tick(0x80BE, 12);                                  // COMB R1
	T.r[0] = (uint16_t)((T.r[0] & 0xFF00) | (uint8_t)(T.r[0] & ~T.r[1]));
	fl::logicb((uint8_t)T.r[0]);
	tick(0x80C0, 12);                                  // BICB R1,R0
	T.r[2] ^= T.r[0];
	fl::logic(T.r[2]);
	tick(0x80C2, 12);
	pokeb(0x0193, (uint8_t)T.r[2]);
	f_nzvc(nzb((uint8_t)T.r[2]) | CF);                 // C from the COMB
	tick(0x80C6, 27);
	rts();
}

// A5:4894: Skate City: address (0x0F56) and bit (0x0F58) of the alternate-bank flag of the
// map cell at 0x0F48
void town_alt_cell_bit()
{
	const uint8_t level = peekb(0x0F59);
	fl::tstb(level);
	tick(0x4898, 24);
	if (level)
	{
		tick(0x48CA, 12);                              // BNE
		rts();
		return;
	}
	tick(0x489A, 12);
	T.r[0] = peek(0x0F48);
	fl::mov(T.r[0]);
	tick(0x489E, 24);
	T.r[0] = fl::sub(T.r[0], 0x0180);
	tick(0x48A2, 18);
	poke(0x0F56, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x48A6, 27);
	poke(0x0F56, fl::asr(peek(0x0F56)));
	tick(0x48AA, 27);
	T.r[0] = peek(0x0F56);
	fl::mov(T.r[0]);
	tick(0x48AE, 24);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0007));
	tick(0x48B2, 18);
	pokeb(0x0F58, rb((uint16_t)(0xFC74 + T.r[0])));
	fl::movb(peekb(0x0F58));
	tick(0x48B8, 42);
	for (uint16_t pc = 0x48BC; pc <= 0x48C4; pc += 4)
	{
		poke(0x0F56, fl::asr(peek(0x0F56)));
		tick(pc, 27);                                  // ASR @#0x0F56
	}
	poke(0x0F56, fl::add(peek(0x0F56), 0x7F16));
	tick(0x48CA, 33);
	rts();
}

// A5:4A0A (registers): R2 = |R2| folded into 0..8, R1 = its sign (flipped when folded)
void fold_direction()
{
	T.r[1] = 0;
	fl::clr();
	tick(0x4A0C, 12);
	fl::tst(T.r[2]);
	tick(0x4A0E, 12);
	if (T.r[2] == 0)
		tick(0x4A1A, 12);                              // BEQ
	else
	{
		tick(0x4A10, 12);
		if ((int16_t)T.r[2] < 0)
		{
			tick(0x4A12, 12);
			T.r[2] = neg(T.r[2]);
			fl::neg((uint16_t)(-T.r[2]));
			tick(0x4A14, 12);
			T.r[1] = fl::dec(T.r[1]);
			tick(0x4A16, 12);
			tick(0x4A1A, 12);                          // BR
		}
		else
		{
			tick(0x4A18, 12);                          // BGE
			T.r[1] = fl::inc(T.r[1]);
			tick(0x4A1A, 12);
		}
	}
	fl::cmp(T.r[2], 0x0008);
	tick(0x4A1E, 18);
	if ((int16_t)T.r[2] > 8)
	{
		tick(0x4A20, 12);
		T.r[2] = neg(T.r[2]);
		fl::neg((uint16_t)(-T.r[2]));
		tick(0x4A22, 12);
		T.r[2] = fl::add(T.r[2], 0x0010);
		tick(0x4A26, 18);
		T.r[1] = neg(T.r[1]);
		fl::neg((uint16_t)(-T.r[1]));
		tick(0x4A28, 12);
	}
	else
		tick(0x4A28, 12);                              // BLE
	rts();
}

// B6:7E28  countdown(i): decrement the byte counter 0x0274+i; when it runs out, record
// i+1 in 0x0273, reload it from the table at 0x7C7A and set 0x01FE = 30
uint16_t countdown(const Args &a)
{
	c_save(1, 0x7E2C);
	T.r[0] = a.u(0);
	fl::mov(T.r[0]);
	tick(0x7E30, 27);
	T.r[1] = 0x0274;
	fl::mov(T.r[1]);
	tick(0x7E34, 18);
	T.r[1] = fl::add(T.r[1], T.r[0]);
	tick(0x7E36, 12);
	T.r[2] = sext8(peekb(T.r[1]));
	fl::movb((uint8_t)T.r[2]);
	tick(0x7E38, 18);
	T.r[2] = fl::dec(T.r[2]);
	tick(0x7E3A, 12);
	pokeb(T.r[1], (uint8_t)T.r[2]);
	fl::movb((uint8_t)T.r[2]);
	tick(0x7E3C, 21);
	if ((int8_t)(uint8_t)T.r[2] > 0)
		tick(0x7E52, 12);                              // BGT
	else
	{
		tick(0x7E3E, 12);
		T.r[2] = 1;
		fl::mov(T.r[2]);
		tick(0x7E42, 18);
		T.r[2] = fl::add(T.r[2], T.r[0]);
		tick(0x7E44, 12);
		pokeb(0x0273, (uint8_t)T.r[2]);
		fl::movb((uint8_t)T.r[2]);
		tick(0x7E48, 27);
		pokeb(T.r[1], rb((uint16_t)(0x7C7A + T.r[0])));
		fl::movb(peekb(T.r[1]));
		tick(0x7E4C, 36);
		poke(0x01FE, 0x001E);
		fl::mov(0x001E);
		tick(0x7E52, 33);
	}
	c_restore(1);
	return T.r[0];
}

// F:9152: next value of the 32-entry random table at 0x02C0 (index in 0x02E0), step by step
void next_random()
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x9154, 24);
	T.r[0] = 0;
	fl::clr();
	tick(0x9156, 12);
	T.r[0] |= peekb(0x02E0);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x915A, 24);
	T.r[0] = fl::inc(T.r[0]);
	tick(0x915C, 12);
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0x915E, 12);
	T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x001F));
	tick(0x9162, 18);
	pokeb(0x02E0, 0);
	fl::clr();
	tick(0x9166, 27);
	poke(0x02E0, fl::logic((uint16_t)(peek(0x02E0) | T.r[1])));
	tick(0x916A, 27);
	T.r[0] = 0;
	fl::clr();
	tick(0x916C, 12);
	T.r[0] |= peekb(0x02E0);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x9170, 24);
	T.r[0] = sext8(rb((uint16_t)(0x02C0 + T.r[0])));
	fl::movb((uint8_t)T.r[0]);
	tick(0x9174, 27);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
	tick(0x9178, 18);
	T.r[1] = pop();
	T.psw &= ~CF;                                      // from the CLR R0
	fl::mov(T.r[1]);
	tick(0x917A, 18);
	rts();
}

// F:DDFE: R0 = (0x0E9B, plus one if the low nibble of 0x0AF8[2*0x0395] exceeds 0x099C) & 3
uint16_t lane_choice(const Args &)
{
	c_save(2, 0xDE02);
	T.r[1] = sext8(peekb(0x0E9B));
	fl::movb((uint8_t)T.r[1]);
	tick(0xDE06, 24);
	T.r[0] = 0;
	fl::clr();
	tick(0xDE08, 12);
	T.r[0] = peekb(0x0395);
	fl::logicb((uint8_t)T.r[0]);
	tick(0xDE0C, 24);
	T.r[0] = fl::asl(T.r[0]);
	tick(0xDE0E, 12);
	T.r[3] = peek(0x099C);
	fl::mov(T.r[3]);
	tick(0xDE12, 24);
	T.r[2] = sext8(peekb((uint16_t)(0x0AF8 + T.r[0])));
	fl::movb((uint8_t)T.r[2]);
	tick(0xDE16, 27);
	T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x000F));
	tick(0xDE1A, 18);
	fl::cmp(T.r[2], T.r[3]);
	tick(0xDE1C, 12);                                  // CMP R2,R3
	if (T.r[2] > T.r[3])
	{
		tick(0xDE1E, 12);
		T.r[1] = fl::inc(T.r[1]);
		tick(0xDE20, 12);
	}
	else
		tick(0xDE20, 12);                              // BLOS
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0xDE22, 12);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0003));
	tick(0xDE26, 18);
	c_restore(2);
	return T.r[0];
}

// F:8CE8: clear both halves of the playfield video RAM (step by step: this takes over two
// frames, with interrupts coming in all the time)
void clear_playfield()
{
	for (int half = 0; half < 2; half++)
	{
		const uint16_t pc = half ? 0x8D00 : 0x8CE8;
		wb(0x1800, 0);
		fl::clr();
		tick((uint16_t)(pc + 4), 27);                  // CLRB @#IN0 (watchdog)
		T.r[0] = half ? 0x6000 : 0x4000;
		fl::mov(T.r[0]);
		tick((uint16_t)(pc + 8), 18);
		jsr((uint16_t)(pc + 12), 0x8078);              // select_video_bank
		T.r[0] = 0x2000;
		fl::mov(T.r[0]);
		tick((uint16_t)(pc + 16), 18);
		const uint16_t loop = (uint16_t)(pc + 16);
		for (;;)
		{
			ww(T.r[0], 0);
			fl::clr();
			T.r[0] += 2;
			tick((uint16_t)(loop + 2), 21);            // CLR (R0)+
			fl::cmp(T.r[0], 0x3FFE);
			tick((uint16_t)(loop + 6), 18);
			if (T.r[0] > 0x3FFE) break;
			tick(loop, 12);                            // BLOS
		}
		tick((uint16_t)(loop + 8), 12);
	}
	rts();
}

// F:CDFA  find_other(i, value): highest slot j (from 0x09B0 down) with 0x0AF8[j] == value
// and j != i; R0 = j or -1
void find_other_slot()
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xCDFC, 24);
	T.r[1] = peek(0x09B0);
	f_nzv(nzw(T.r[1]));
	tick(0xCE00, 24);
	bool found = false;
	if ((int16_t)T.r[1] < 0)
		tick(0xCE1C, 12);                              // BLT
	else
	{
		tick(0xCE02, 12);
		for (;;)
		{
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0xCE04, 12);
			if (T.r[0] & 0x8000) T.psw |= CF; else T.psw &= ~CF;
			T.r[0] = fl::asl(T.r[0]);
			tick(0xCE06, 12);
			const uint16_t w = peek((uint16_t)(0x0AF8 + T.r[0]));
			const uint16_t value = rw((uint16_t)(T.r[6] + 6));
			fl::cmp(w, value);
			tick(0xCE0C, 42);
			if (w == value)
			{
				tick(0xCE0E, 12);
				const uint16_t i = rw((uint16_t)(T.r[6] + 4));
				fl::cmp(T.r[1], i);
				tick(0xCE12, 27);
				if (T.r[1] != i)
				{
					tick(0xCE14, 12);
					T.r[0] = T.r[1];
					fl::mov(T.r[0]);
					tick(0xCE16, 12);
					tick(0xCE20, 12);                  // BR
					found = true;
					break;
				}
				tick(0xCE18, 12);                      // BEQ
			}
			else
				tick(0xCE18, 12);                      // BNE
			T.r[1] = fl::dec(T.r[1]);
			tick(0xCE1A, 12);
			if ((int16_t)T.r[1] < 0)
			{
				tick(0xCE1C, 12);
				break;
			}
			tick(0xCE02, 12);                          // BGE
		}
	}
	if (!found)
	{
		T.r[0] = 0xFFFF;
		fl::mov(T.r[0]);
		tick(0xCE20, 18);
	}
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0xCE22, 18);
	rts();
}

// F:8C56 (character code in R0): draw a 2x2-tile character of the large font into the text
// layer at the cursor (0x0186), which advances by one character
void draw_big_char()
{
	c_save(1, 0x8C5A);
	T.r[0] = fl::sub(T.r[0], 0x0030);
	tick(0x8C5E, 18);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x8C60, 12);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x8C62, 12);
	T.r[0] = fl::logic((uint16_t)(T.r[0] | peek(0x0188)));
	tick(0x8C66, 24);
	T.r[1] = peek(0x0186);
	fl::mov(T.r[1]);
	tick(0x8C6A, 24);
	T.r[0] = fl::add(T.r[0], peek(0x018A));
	tick(0x8C6E, 24);
	ww(T.r[1], T.r[0]);
	T.r[1] += 2;
	fl::mov(T.r[0]);
	tick(0x8C70, 21);
	T.r[0] = fl::inc(T.r[0]);
	tick(0x8C72, 12);
	ww(T.r[1], T.r[0]);
	T.r[1] += 2;
	fl::mov(T.r[0]);
	tick(0x8C74, 21);
	poke(0x0186, T.r[1]);
	fl::mov(T.r[1]);
	tick(0x8C78, 27);
	T.r[1] = fl::add(T.r[1], 0x007C);
	tick(0x8C7C, 18);
	T.r[0] = fl::inc(T.r[0]);
	tick(0x8C7E, 12);
	ww(T.r[1], T.r[0]);
	T.r[1] += 2;
	fl::mov(T.r[0]);
	tick(0x8C80, 21);
	T.r[0] = fl::inc(T.r[0]);
	tick(0x8C82, 12);
	ww(T.r[1], T.r[0]);
	fl::mov(T.r[0]);
	tick(0x8C84, 21);
	c_restore(1);                                      // C stays from the ADD
}

// F:E6FE: bit mask of parks (masks at 0xFC74, 3..0) that are open: unlocked in 0x0F29, or,
// in mode 1 of 0x0F2B, every park except the one in 0x02B8
uint16_t open_parks(const Args &)
{
	c_save(1, 0xE702);
	T.r[0] = 0;
	fl::clr();
	tick(0xE704, 12);
	T.r[1] = 3;
	fl::mov(T.r[1]);
	tick(0xE708, 18);
	for (;;)
	{
		const bool set = (peekb(0x0F29) & rb((uint16_t)(0xFC74 + T.r[1]))) != 0;
		fl::logicb((uint8_t)(peekb(0x0F29) & peekb((uint16_t)(T.r[1] + 0xFC74))));
		tick(0xE70E, 39);
		bool add = true;
		if (set)
			tick(0xE722, 12);                          // BNE
		else
		{
			tick(0xE710, 12);
			const bool mode1 = peekb(0x0F2B) == 1;
			fl::cmpb(peekb(0x0F2B), 0x01);
			tick(0xE716, 30);
			if (!mode1)
			{
				tick(0xE72A, 12);                      // BNE
				add = false;
			}
			else
			{
				tick(0xE718, 12);
				T.r[2] = 0;
				fl::clr();
				tick(0xE71A, 12);
				T.r[2] = peekb(0x02B8);
				fl::logicb((uint8_t)T.r[2]);
				tick(0xE71E, 24);
				fl::cmp(T.r[1], T.r[2]);
				tick(0xE720, 12);                      // CMP R1,R2
				if (T.r[1] == T.r[2])
				{
					tick(0xE72A, 12);                  // BEQ
					add = false;
				}
				else
					tick(0xE722, 12);
			}
		}
		if (add)
		{
			T.r[2] = 0;
			fl::clr();
			tick(0xE724, 12);
			T.r[2] = rb((uint16_t)(0xFC74 + T.r[1]));
			fl::logicb((uint8_t)T.r[2]);
			tick(0xE728, 27);
			T.r[0] = fl::logic((uint16_t)(T.r[0] | T.r[2]));
			tick(0xE72A, 12);
		}
		T.r[1] = fl::dec(T.r[1]);
		tick(0xE72C, 12);
		if ((int16_t)T.r[1] < 0) break;
		tick(0xE708, 12);                              // BGE
	}
	tick(0xE72E, 12);
	c_restore(1);
	return T.r[0];
}

// F:F028  add_time(n): n = 0 starts the timer (0x0254 = 127) and its countdown bytes from
// the table at 0x51E6; otherwise n is added and the timer stops at 0
void add_time()
{
	pokeb(0x0FA2, fl::logicb((uint8_t)(peekb(0x0FA2) | 0x04)));
	tick(0xF02E, 33);
	const uint16_t n = rw((uint16_t)(T.r[6] + 2));
	T.r[0] = n;
	fl::mov(T.r[0]);
	tick(0xF032, 27);
	if (n == 0)
	{
		tick(0xF034, 12);
		T.r[0] = 0;
		fl::clr();
		tick(0xF036, 12);
		T.r[0] = peekb(0x0F2E);
		fl::logicb((uint8_t)T.r[0]);
		tick(0xF03A, 24);
		T.r[1] = 0;
		fl::clr();
		tick(0xF03C, 12);
		T.r[1] = peekb(0x0FE4);
		fl::logicb((uint8_t)T.r[1]);
		tick(0xF040, 24);
		T.r[0] = fl::add(T.r[0], T.r[1]);
		tick(0xF042, 12);
		pokeb(0x0FA7, rb((uint16_t)(0x51E6 + T.r[0])));
		fl::movb(peekb(0x0FA7));
		tick(0xF048, 42);
		pokeb(0x0FA6, rb((uint16_t)(0x51E6 + T.r[0])));
		fl::movb(peekb(0x0FA6));
		tick(0xF04E, 42);
		poke(0x0254, 0x007F);
		fl::mov(0x007F);
		tick(0xF054, 33);
		tick(0xF05C, 12);                              // BR
		tick(0xF062, 12);                              // BGE
		rts();
		return;
	}
	tick(0xF056, 12);                                  // BNE
	const uint16_t t = fl::add(peek(0x0254), n);
	poke(0x0254, t);
	tick(0xF05A, 27);
	T.psw &= ~(CF | VF);
	tick(0xF05C, 18);                                  // CCC CV
	if ((int16_t)t < 0)
	{
		tick(0xF05E, 12);
		poke(0x0254, 0);
		fl::clr();
		tick(0xF062, 27);
	}
	else
		tick(0xF062, 12);                              // BGE
	rts();
}

// F:C0D8: per-slot value: 0x0AB8[slot] = table(0xFD82)[table(0x65CC, bank 0x0400)[0x09B8[slot]]]
// + 0x0E9A, for slot = 0x09AA; switches bank A to 0 and bank B to 0x0400
uint16_t slot_value(const Args &)
{
	c_save(2, 0xC0DC);
	poke(0x01D2, 0);
	fl::clr();
	tick(0xC0E0, 27);
	ww(0x1400, 0);
	fl::clr();
	tick(0xC0E4, 27);
	T.r[0] = 0x0400;
	fl::mov(T.r[0]);
	tick(0xC0E8, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xC0EC, 27);
	ww(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xC0F0, 27);
	T.r[1] = peek(0x09AA);
	fl::mov(T.r[1]);
	tick(0xC0F4, 24);
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0xC0F6, 12);
	T.r[0] = sext8(peekb((uint16_t)(0x09B8 + T.r[0])));
	fl::movb((uint8_t)T.r[0]);
	tick(0xC0FA, 27);
	T.r[0] = sext8(rb((uint16_t)(0x65CC + T.r[0])));
	fl::movb((uint8_t)T.r[0]);
	tick(0xC0FE, 27);
	T.r[2] = 0;
	fl::clr();
	tick(0xC100, 12);
	T.r[2] = rb((uint16_t)(0xFD82 + T.r[0]));
	fl::logicb((uint8_t)T.r[2]);
	tick(0xC104, 27);
	T.r[3] = 0;
	fl::clr();
	tick(0xC106, 12);
	T.r[3] = peekb(0x0E9A);
	fl::logicb((uint8_t)T.r[3]);
	tick(0xC10A, 24);
	T.r[2] = fl::add(T.r[2], T.r[3]);
	tick(0xC10C, 12);
	pokeb((uint16_t)(0x0AB8 + T.r[1]), (uint8_t)T.r[2]);
	fl::movb((uint8_t)T.r[2]);
	tick(0xC110, 30);
	T.r[0] = 0;
	fl::clr();
	tick(0xC112, 12);
	c_restore(2);
	return 0;
}

// F:DA74: copy 32 colours from ROM (bank 0x2000, 0x5EC0) to palette entries 0x40-0x5F, step
// by step
void load_text_palette()
{
	T.r[2] = peek(0x01D2);
	fl::mov(T.r[2]);
	tick(0xDA78, 24);
	T.r[3] = peek(0x01D4);
	fl::mov(T.r[3]);
	tick(0xDA7C, 24);
	T.r[0] = 0x2000;
	fl::mov(T.r[0]);
	tick(0xDA80, 18);
	poke(0x01D2, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDA84, 27);
	ww(0x1400, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDA88, 27);
	T.r[0] = 0x2400;
	fl::mov(T.r[0]);
	tick(0xDA8C, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDA90, 27);
	ww(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDA94, 27);
	T.r[1] = 0x0020;
	fl::mov(T.r[1]);
	tick(0xDA98, 18);
	for (;;)
	{
		T.r[0] = T.r[1];
		fl::mov(T.r[0]);
		tick(0xDA9A, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0xDA9C, 12);
		{
			const uint16_t c = rw((uint16_t)(0x5EBE + T.r[0]));
			ww((uint16_t)(0x107E + T.r[0]), c);
			fl::mov(c);
		}
		tick(0xDAA2, 45);
		if (--T.r[1] == 0) break;
		tick(0xDA98, 18);                              // SOB
	}
	tick(0xDAA4, 18);
	poke(0x01D2, T.r[2]);
	fl::mov(T.r[2]);
	tick(0xDAA8, 27);
	poke(0x01D4, T.r[3]);
	fl::mov(T.r[3]);
	tick(0xDAAC, 27);
	ww(0x1400, peek(0x01D2));
	fl::mov(peek(0x01D2));
	tick(0xDAB2, 39);
	const uint16_t v = peek(0x01D4);
	ww(0x1402, v);
	T.psw &= ~CF;                                      // from the last ASL
	fl::mov(v);
	tick(0xDAB8, 39);
	rts();
}

// F:DABA: copy 128 colours from ROM (bank 0x1C00, 0x7DAC) to palette entries 0x80-0xFF, step
// by step
void load_playfield_palette()
{
	T.r[3] = peek(0x01D2);
	fl::mov(T.r[3]);
	tick(0xDABE, 24);
	T.r[4] = peek(0x01D4);
	fl::mov(T.r[4]);
	tick(0xDAC2, 24);
	T.r[0] = 0x1800;
	fl::mov(T.r[0]);
	tick(0xDAC6, 18);
	poke(0x01D2, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDACA, 27);
	ww(0x1400, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDACE, 27);
	T.r[0] = 0x1C00;
	fl::mov(T.r[0]);
	tick(0xDAD2, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDAD6, 27);
	ww(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDADA, 27);
	T.r[1] = 0x7EAC;
	fl::mov(T.r[1]);
	tick(0xDADE, 18);
	T.r[0] = 0x1200;
	fl::mov(T.r[0]);
	tick(0xDAE2, 18);
	T.r[2] = 0x0080;
	fl::mov(T.r[2]);
	tick(0xDAE6, 18);
	for (;;)
	{
		T.r[1] -= 2;
		const uint16_t c = rw(T.r[1]);
		T.r[0] -= 2;
		ww(T.r[0], c);
		fl::mov(c);
		tick(0xDAE8, 33);                              // MOV -(R1),-(R0)
		if (--T.r[2] == 0) break;
		tick(0xDAE6, 18);                              // SOB
	}
	tick(0xDAEA, 18);
	poke(0x01D2, T.r[3]);
	fl::mov(T.r[3]);
	tick(0xDAEE, 27);
	poke(0x01D4, T.r[4]);
	fl::mov(T.r[4]);
	tick(0xDAF2, 27);
	ww(0x1400, peek(0x01D2));
	fl::mov(peek(0x01D2));
	tick(0xDAF8, 39);
	const uint16_t v = peek(0x01D4);
	ww(0x1402, v);
	fl::mov(v);
	tick(0xDAFE, 39);
	rts();
}

}

LOGIC_TABLE(misc,
	asm_routine("A7", 0x55DE, "update_ticket_bits", update_ticket_bits, 0),
	asm_routine("F", 0xE1FA, "fill_slots", fill_slots, 0),
	asm_routine("A5", 0x5B18, "start_sequence_a5", start_sequence_a5, 0),
	asm_routine("B1", 0x7D82, "blink_screen7_palette", blink_screen7_palette, 0),
	asm_routine("B3", 0x7A9E, "snapshot_skater", snapshot_skater, 0),
	asm_routine("F", 0x8036, "load_scroll_registers", load_scroll_registers, 0),
	asm_routine("F", 0xB1C8, "sprite_link", sprite_link, 0),
	asm_routine("A5", 0x51BA, "copy_object0_position", copy_object0_position, 0),
	asm_routine("A7", 0x52FC, "difficulty_step", difficulty_step, 0),
	asm_routine("F", 0x80A4, "read_input_edges", read_input_edges, 0),
	asm_routine("A5", 0x4894, "town_alt_cell_bit", town_alt_cell_bit, 0),
	asm_routine("A5", 0x4A0A, "fold_direction", fold_direction, 0),
	c_routine("B6", 0x7E28, "countdown", countdown, 0, 0),
	asm_routine("F", 0x9152, "next_random", next_random, 0),
	c_routine("F", 0xDDFE, "lane_choice", lane_choice, 0, 0),
	asm_routine("F", 0x8CE8, "clear_playfield", clear_playfield, 0),
	asm_routine("F", 0xCDFA, "find_other_slot", find_other_slot, 0),
	asm_routine("F", 0x8C56, "draw_big_char", draw_big_char, 0),
	c_routine("F", 0xE6FE, "open_parks", open_parks, 0, 0),
	asm_routine("F", 0xF028, "add_time", add_time, 0),
	c_routine("F", 0xC0D8, "slot_value", slot_value, 0, 0),
	asm_routine("F", 0xDA74, "load_text_palette", load_text_palette, 0),
	asm_routine("F", 0xDABA, "load_playfield_palette", load_playfield_palette, 0),
)

}
