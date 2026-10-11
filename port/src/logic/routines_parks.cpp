// Rewritten routines: parks, tickets, cash and the town position checks.  They follow the
// original instruction by instruction (tick after every instruction, the stack as the
// original's; see routines_core.cpp).
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

// B3:7F80: clear the bytes 0x0FB4-0x0FD7
uint16_t clear_0fb4(const Args &)
{
	T.r[0] = 0x0024;
	fl::mov(T.r[0]);
	tick(0x7F84, 18);
	for (;;)
	{
		pokeb((uint16_t)(0x0FB3 + T.r[0]), 0);
		fl::clr();
		tick(0x7F88, 30);
		if (--T.r[0] == 0) break;
		tick(0x7F84, 18);                              // SOB
	}
	tick(0x7F8A, 18);
	rts();
	return 0;
}

// A7:5602: bit i of 0x0ED7 (masks at 0xFC74) is set when the byte 0x025B+i (i = 1..4) is
// positive (A7:55DE does the same against a limit)
uint16_t set_ticket_bits(const Args &)
{
	T.r[0] = 4;
	fl::mov(T.r[0]);
	tick(0x5606, 18);
	for (;;)
	{
		const bool positive = (int8_t)peekb((uint16_t)(0x025B + T.r[0])) > 0;
		fl::tstb(peekb((uint16_t)(T.r[0] + 0x025B)));
		tick(0x560A, 27);
		const uint8_t m = rb((uint16_t)(0xFC73 + T.r[0]));
		if (positive)
		{
			tick(0x560C, 12);
			pokeb(0x0ED7, fl::logicb((uint8_t)(peekb(0x0ED7) | m)));
			tick(0x5612, 42);
			tick(0x561A, 12);                          // BR
		}
		else
		{
			tick(0x5614, 12);                          // BLE
			pokeb(0x0ED7, fl::logicb((uint8_t)(peekb(0x0ED7) & ~m)));
			tick(0x561A, 42);
		}
		if (--T.r[0] == 0) break;
		tick(0x5606, 18);                              // SOB
	}
	tick(0x561C, 18);
	rts();
	return 0;
}

// F:F002  add_cash(int n): cash 0x0256 += n, kept within 0..9999 (0x0FA2 bit 3: redraw)
uint16_t add_cash(const Args &a)
{
	pokeb(0x0FA2, fl::logicb((uint8_t)(peekb(0x0FA2) | 0x08)));
	tick(0xF008, 33);
	poke(0x0256, fl::add(peek(0x0256), a.u(0)));
	tick(0xF00E, 42);
	if ((int16_t)peek(0x0256) < 0)
	{
		tick(0xF010, 12);
		poke(0x0256, 0);
		fl::clr();
		tick(0xF014, 27);
	}
	else
		tick(0xF014, 12);                              // BPL
	const bool over = (int16_t)peek(0x0256) > 9999;
	fl::cmp(peek(0x0256), 0x270F);
	tick(0xF01A, 30);
	if (over)
	{
		tick(0xF01C, 12);
		poke(0x0256, 9999);
		fl::mov(0x270F);
		tick(0xF022, 33);
	}
	else
		tick(0xF022, 12);                              // BLE
	pokeb(0x0ED8, 0);
	fl::clr();
	tick(0xF026, 27);
	rts();
	return T.r[0];
}

// B3:65D6  add_park_counter(int n): in parks 0 and 1 (0x027A) 0x0F20 += 4n, otherwise
// 0x0F1C += n; returns the new value
uint16_t add_park_counter(const Args &a)
{
	const bool small = peekb(0x027A) < 2;
	fl::cmpb(peekb(0x027A), 0x02);
	tick(0x65DC, 30);
	if (small)
	{
		tick(0x65EC, 12);                              // BCS
		T.r[0] = a.u(0);
		fl::mov(T.r[0]);
		tick(0x65F0, 27);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x65F2, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x65F4, 12);
		T.r[0] = fl::add(T.r[0], peek(0x0F20));
		tick(0x65F8, 24);
		poke(0x0F20, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x65FC, 27);
	}
	else
	{
		tick(0x65DE, 12);
		T.r[0] = peek(0x0F1C);
		fl::mov(T.r[0]);
		tick(0x65E2, 24);
		T.r[0] = fl::add(T.r[0], a.u(0));
		tick(0x65E6, 27);
		poke(0x0F1C, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x65EA, 27);
	}
	rts();
	return T.r[0];
}

// A7:5360: park (0-3) whose gate is at the town position 0x0996 / 0x099A
uint16_t park_at_gate(const Args &)
{
	const bool north = (int16_t)peek(0x099A) > 4;
	fl::cmp(peek(0x099A), 0x0004);
	tick(0x5366, 30);
	tick((uint16_t)(north ? 0x5368 : 0x537A), 12);     // BLE
	const bool east = (int16_t)peek(0x0996) > 0x00B0;
	if (north)
	{
		fl::cmp(peek(0x0996), 0x00B0);
		tick(0x536E, 30);
		if (east)
		{
			tick(0x5370, 12);
			T.r[0] = 0;
			fl::clr();
			tick(0x5372, 12);
		}
		else
		{
			tick(0x5374, 12);                          // BLE
			T.r[0] = 3;
			fl::mov(T.r[0]);
			tick(0x5378, 18);
		}
	}
	else
	{
		fl::cmp(peek(0x0996), 0x00B0);
		tick(0x5380, 30);
		if (east)
		{
			tick(0x5382, 12);
			T.r[0] = 2;
			fl::mov(T.r[0]);
			tick(0x5386, 18);
		}
		else
		{
			tick(0x5388, 12);                          // BLE
			T.r[0] = 1;
			fl::mov(T.r[0]);
			tick(0x538C, 18);
		}
	}
	rts();
	return T.r[0];
}

// A7:5326: park (0-3) whose area contains the town position 0x0996 / 0x099A
uint16_t park_at_area(const Args &)
{
	T.r[0] = peek(0x0996);
	fl::mov(T.r[0]);
	tick(0x532A, 24);
	const int16_t x = (int16_t)T.r[0];
	fl::cmp(T.r[0], 0x0050);
	tick(0x532E, 18);                                  // CMP R0,#0x50
	if (x < 0x0050)
	{
		tick(0x5330, 12);
		T.r[0] = 0;
		fl::clr();
		tick(0x5332, 12);
		rts();
		return 0;
	}
	tick(0x5334, 12);                                  // BGE
	fl::cmp(T.r[0], 0x0100);
	tick(0x5338, 18);                                  // CMP R0,#0x100
	if (x > 0x0100)
	{
		tick(0x533A, 12);
		T.r[0] = 3;
		fl::mov(T.r[0]);
		tick(0x533E, 18);
		rts();
		return 3;
	}
	tick(0x5340, 12);                                  // BLE
	const bool north = (int16_t)peek(0x099A) > 6;
	fl::cmp(peek(0x099A), 0x0006);
	tick(0x5346, 30);
	bool two;
	if (north)
	{
		tick(0x5354, 12);                              // BGT
		two = true;
	}
	else
	{
		tick(0x5348, 12);
		const bool south = (int16_t)peek(0x099A) >= 0;
		fl::tst(peek(0x099A));
		tick(0x534C, 24);
		if (south)
		{
			tick(0x535A, 12);                          // BGE
			two = false;
		}
		else
		{
			tick(0x534E, 12);
			fl::cmp(T.r[0], 0x00D0);
			tick(0x5352, 18);                          // CMP R0,#0xD0
			two = x > 0x00D0;
			tick((uint16_t)(two ? 0x5354 : 0x535A), 12);   // BLE
		}
	}
	if (two)
	{
		T.r[0] = 2;
		fl::mov(T.r[0]);
		tick(0x5358, 18);
	}
	else
	{
		T.r[0] = 1;
		tick(0x535E, 18);
	}
	rts();
	return T.r[0];
}

// F:CDD0  find_slot_with(int except, int value): highest slot i <= 0x09AE, other than except,
// whose word 0x0AF8[i] equals value; -1 if none
uint16_t find_slot_with(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xCDD2, 24);
	T.r[1] = peek(0x09AE);
	fl::mov(T.r[1]);
	tick(0xCDD6, 24);
	bool found = false;
	if ((int16_t)T.r[1] < 0)
		tick(0xCDF2, 12);                              // BLT
	else
	{
		tick(0xCDD8, 12);
		for (;;)
		{
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0xCDDA, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0xCDDC, 12);
			const bool same = peek((uint16_t)(0x0AF8 + T.r[0])) == rw((uint16_t)(T.r[6] + 6));
			fl::cmp(peek((uint16_t)(T.r[0] + 0x0AF8)), peek((uint16_t)(T.r[6] + 6)));
			tick(0xCDE2, 42);
			if (same)
			{
				tick(0xCDE4, 12);
				const bool except = T.r[1] == rw((uint16_t)(T.r[6] + 4));
				fl::cmp(T.r[1], peek((uint16_t)(T.r[6] + 4)));
				tick(0xCDE8, 27);
				if (!except)
				{
					tick(0xCDEA, 12);
					T.r[0] = T.r[1];
					fl::mov(T.r[0]);
					tick(0xCDEC, 12);
					tick(0xCDF6, 12);                  // BR
					found = true;
					break;
				}
				tick(0xCDEE, 12);                      // BEQ
			}
			else
				tick(0xCDEE, 12);                      // BNE
			T.r[1] = fl::dec(T.r[1]);
			tick(0xCDF0, 12);
			if ((int16_t)T.r[1] < 0)
			{
				tick(0xCDF2, 12);
				break;
			}
			tick(0xCDD8, 12);                          // BGE
		}
	}
	if (!found)
	{
		T.r[0] = 0xFFFF;
		tick(0xCDF6, 18);
	}
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0xCDF8, 18);
	rts();
	return T.r[0];
}

// F:E106: when 0x0377 is set, F:CF5E(0x0397); the banks are restored afterwards
uint16_t call_cf5e_keep_banks(const Args &)
{
	c_save(1, 0xE10A);
	T.r[1] = peek(0x01D2);
	fl::mov(T.r[1]);
	tick(0xE10E, 24);
	T.r[2] = peek(0x01D4);
	fl::mov(T.r[2]);
	tick(0xE112, 24);
	const bool on = peekb(0x0377) != 0;
	fl::tstb(peekb(0x0377));
	tick(0xE116, 24);
	if (on)
	{
		tick(0xE118, 12);
		push(0);
		fl::clr();
		tick(0xE11A, 24);
		pokeb(T.r[6], peekb(0x0397));
		fl::movb(peekb(T.r[6]));
		tick(0xE11E, 33);
		jsr(0xE122, 0xCF5E);
		pop();
		fl::tst(peek((uint16_t)(T.r[6] - 2)));
		tick(0xE124, 18);
	}
	else
		tick(0xE124, 12);                              // BEQ
	poke(0x01D2, T.r[1]);
	fl::mov(T.r[1]);
	tick(0xE128, 27);
	poke(0x01D4, T.r[2]);
	fl::mov(T.r[2]);
	tick(0xE12C, 27);
	poke(0x1400, peek(0x01D2));
	fl::mov(peek(0x01D2));
	tick(0xE132, 39);
	poke(0x1402, peek(0x01D4));
	fl::mov(peek(0x01D4));
	tick(0xE138, 39);
	c_restore(1);
	return T.r[0];
}

// A7:50C8: when 0x0377 is set, F:E13A(0, 0, 2, A7:4F98() ^ 8) and 0x0EB4 = 250
uint16_t a7_50c8(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x50CA, 24);
	const bool on = peekb(0x0377) != 0;
	fl::tstb(peekb(0x0377));
	tick(0x50CE, 24);
	if (on)
	{
		tick(0x50D0, 12);
		jsr(0x50D4, 0x4F98);
		T.r[1] = T.r[0];
		fl::mov(T.r[1]);
		tick(0x50D6, 12);
		T.r[0] = 0x0008;
		fl::mov(T.r[0]);
		tick(0x50DA, 18);
		T.r[1] ^= T.r[0];
		fl::logic(T.r[1]);
		tick(0x50DC, 12);
		push(T.r[1]);
		fl::mov(T.r[1]);
		tick(0x50DE, 24);
		push(0x0002);
		fl::mov(0x0002);
		tick(0x50E2, 30);
		push(0);
		fl::clr();
		tick(0x50E4, 24);
		push(0);
		fl::clr();
		tick(0x50E6, 24);
		jsr(0x50EA, 0xE13A);
		poke(0x0EB4, 0x00FA);
		fl::mov(0x00FA);
		tick(0x50F0, 33);
		T.r[6] = fl::add(T.r[6], 8);
		tick(0x50F4, 18);                              // ADD #8,SP
	}
	else
		tick(0x50F4, 12);                              // BEQ
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x50F6, 18);
	rts();
	return T.r[0];
}


// F:DA02  open_park_gate(int park): sound 0x62; the gate object of the park (slot whose
// 0x0AF8 word is the park's entry at 0x70E8 in bank B 0x0400, with bit 15) starts its
// animation (0x0430 = 1); the park is marked open in 0x0EA4.  Banks are restored.
uint16_t open_park_gate(const Args &)
{
	c_save(2, 0xDA06);
	T.r[2] = peek(0x01D2);
	fl::mov(T.r[2]);
	tick(0xDA0A, 24);
	T.r[3] = peek(0x01D4);
	fl::mov(T.r[3]);
	tick(0xDA0E, 24);
	poke(0x01D2, 0);
	fl::clr();
	tick(0xDA12, 27);
	poke(0x1400, 0);
	fl::clr();
	tick(0xDA16, 27);
	T.r[0] = 0x0400;
	fl::mov(T.r[0]);
	tick(0xDA1A, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDA1E, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDA22, 27);
	push(0x0062);
	fl::mov(0x0062);
	tick(0xDA26, 30);
	jsr(0xDA2A, 0x8E78);                               // queue_sound
	T.r[1] = rw((uint16_t)(T.r[6] + 0x0C));            // the park
	fl::mov(T.r[1]);
	tick(0xDA2E, 27);
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0xDA30, 12);
	T.r[0] = fl::asl(T.r[0]);
	tick(0xDA32, 12);
	ww(T.r[6], peek((uint16_t)(0x70E8 + T.r[0])));
	fl::mov(peek(T.r[6]));
	tick(0xDA36, 36);
	ww(T.r[6], fl::logic((uint16_t)(rw(T.r[6]) | 0x8000)));
	tick(0xDA3A, 27);
	push(0xFFFF);
	fl::mov(0xFFFF);
	tick(0xDA3E, 30);
	jsr(0xDA42, 0xCDD0);                               // find_slot_with
	fl::tst(T.r[0]);
	tick(0xDA44, 12);                                  // TST R0
	if ((int16_t)T.r[0] < 0)
		tick(0xDA56, 12);                              // BLT
	else
	{
		tick(0xDA46, 12);
		T.r[0] = sext8(peekb((uint16_t)(0x09B8 + T.r[0])));
		fl::movb((uint8_t)T.r[0]);
		tick(0xDA4A, 27);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x003F));
		tick(0xDA4E, 18);
		T.r[0] = fl::asl(T.r[0]);
		tick(0xDA50, 12);
		poke((uint16_t)(0x0430 + T.r[0]), 0x0001);
		fl::mov(0x0001);
		tick(0xDA56, 36);
	}
	pokeb(0x0EA4, fl::logicb((uint8_t)(peekb(0x0EA4) | rb((uint16_t)(0xFC74 + T.r[1])))));
	tick(0xDA5C, 42);
	poke(0x01D2, T.r[2]);
	fl::mov(T.r[2]);
	tick(0xDA60, 27);
	poke(0x01D4, T.r[3]);
	fl::mov(T.r[3]);
	tick(0xDA64, 27);
	poke(0x1400, peek(0x01D2));
	fl::mov(peek(0x01D2));
	tick(0xDA6A, 39);
	poke(0x1402, peek(0x01D4));
	fl::mov(peek(0x01D4));
	tick(0xDA70, 39);
	T.r[6] += 4;
	fl::cmp(peek((uint16_t)(T.r[6] - 4)), peek((uint16_t)(T.r[6] - 2)));
	tick(0xDA72, 24);                                  // CMP (SP)+,(SP)+
	c_restore(2);
	return T.r[0];
}

// B3:6E38: medal level of the value 0x0258 against the thresholds 0x0FDA/0x0FDC/0x0FDE
// (3 -> 0xC0, 2 -> 0x80, 1 -> 0x40, none -> 0) into the byte 0x0262 + 0x0F31; bank A 0x3800.
// Returns 0x0F31 when a level was reached, else 0x3800.
uint16_t set_medal_level(const Args &)
{
	c_save(3, 0x6E3C);
	T.r[3] = peek(0x0258);
	fl::mov(T.r[3]);
	tick(0x6E40, 24);
	if ((int16_t)T.r[3] > 0)
	{
		tick(0x6E42, 12);
		T.r[2] = 3;
		fl::mov(T.r[2]);
		tick(0x6E46, 18);
		for (;;)
		{
			T.r[0] = T.r[2];
			fl::mov(T.r[0]);
			tick(0x6E48, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x6E4A, 12);
			const bool below = (int16_t)T.r[3] < (int16_t)peek((uint16_t)(0x0FD8 + T.r[0]));
			fl::cmp(T.r[3], peek((uint16_t)(T.r[0] + 0x0FD8)));
			tick(0x6E4E, 27);
			if (!below)
			{
				tick(0x6E50, 12);
				T.r[0] = 0;
				fl::clr();
				tick(0x6E52, 12);
				T.r[0] = peekb(0x0F31);
				fl::logicb((uint8_t)T.r[0]);
				tick(0x6E56, 24);
				T.r[1] = T.r[2];
				fl::mov(T.r[1]);
				tick(0x6E58, 12);
				for (uint16_t pc = 0x6E5A; pc <= 0x6E5E; pc += 2)
				{
					T.r[1] = fl::asl(T.r[1]);
					tick(pc, 12);                      // ASL R1
				}
				T.r[4] = T.r[1];
				fl::mov(T.r[4]);
				tick(0x6E60, 12);
				for (uint16_t pc = 0x6E62; pc <= 0x6E66; pc += 2)
				{
					T.r[4] = fl::asl(T.r[4]);
					tick(pc, 12);                      // ASL R4
				}
				pokeb((uint16_t)(0x0262 + T.r[0]), (uint8_t)T.r[4]);
				fl::movb((uint8_t)T.r[4]);
				tick(0x6E6A, 30);
				T.r[1] = 0x3800;
				fl::mov(T.r[1]);
				tick(0x6E6E, 18);
				poke(0x01D2, T.r[1]);
				fl::mov(T.r[1]);
				tick(0x6E72, 27);
				poke(0x1400, T.r[1]);
				fl::mov(T.r[1]);
				tick(0x6E76, 27);
				c_restore(3);
				return T.r[0];
			}
			tick(0x6E78, 12);                          // BLT
			if (--T.r[2] == 0) break;
			tick(0x6E46, 18);                          // SOB
		}
		tick(0x6E7A, 18);
	}
	else
		tick(0x6E7A, 12);                              // BLE
	T.r[0] = 0;
	fl::clr();
	tick(0x6E7C, 12);
	T.r[0] = peekb(0x0F31);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x6E80, 24);
	pokeb((uint16_t)(0x0262 + T.r[0]), 0);
	fl::clr();
	tick(0x6E84, 30);
	T.r[0] = 0x3800;
	fl::mov(T.r[0]);
	tick(0x6E88, 18);
	poke(0x01D2, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x6E8C, 27);
	poke(0x1400, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x6E90, 27);
	c_restore(3);
	return T.r[0];
}


// F:D97C  set_gate_animation(int closing, int gate): the gate object (slot whose 0x0AF8 word is
// the gate's entry at 0x70AC in bank B 0x0400, with bit 15) gets animation 0x03B0 from the
// table 0x5B8C (or 0x5BAC when closing), one of two variants chosen at random.  Banks are
// restored.
uint16_t set_gate_animation(const Args &)
{
	c_save(2, 0xD980);
	T.r[2] = peek(0x01D2);
	fl::mov(T.r[2]);
	tick(0xD984, 24);
	T.r[3] = peek(0x01D4);
	fl::mov(T.r[3]);
	tick(0xD988, 24);
	poke(0x01D2, 0);
	fl::clr();
	tick(0xD98C, 27);
	poke(0x1400, 0);
	fl::clr();
	tick(0xD990, 27);
	T.r[0] = 0x0400;
	fl::mov(T.r[0]);
	tick(0xD994, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xD998, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xD99C, 27);
	T.r[0] = rw((uint16_t)(T.r[6] + 0x0C));            // the gate
	fl::mov(T.r[0]);
	tick(0xD9A0, 27);
	T.r[0] = fl::asl(T.r[0]);
	tick(0xD9A2, 12);
	push(peek((uint16_t)(0x70AC + T.r[0])));
	fl::mov(peek(T.r[6]));
	tick(0xD9A6, 39);
	ww(T.r[6], fl::logic((uint16_t)(rw(T.r[6]) | 0x8000)));
	tick(0xD9AA, 27);
	push(0xFFFF);
	fl::mov(0xFFFF);
	tick(0xD9AE, 30);
	jsr(0xD9B2, 0xCDFA);                               // find_other_slot
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0xD9B4, 12);
	if ((int16_t)T.r[1] < 0)
		tick(0xD9EA, 12);                              // BLT
	else
	{
		tick(0xD9B6, 12);
		T.r[1] = sext8(peekb((uint16_t)(0x09B8 + T.r[1])));
		fl::movb((uint8_t)T.r[1]);
		tick(0xD9BA, 27);
		T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x003F));
		tick(0xD9BE, 18);
		jsr(0xD9C2, 0x9152);                           // next_random
		const bool odd = T.r[0] & 1;
		T.r[0] = fl::ror(T.r[0]);                      // ROR (the carry bit in is lost below)
		tick(0xD9C4, 12);
		if (odd)
		{
			tick(0xD9CA, 12);                          // BCS
			T.r[0] = 0x0008;
			fl::mov(T.r[0]);
			tick(0xD9CE, 18);
		}
		else
		{
			tick(0xD9C6, 12);
			T.r[0] = 0;
			fl::clr();
			tick(0xD9C8, 12);
			tick(0xD9CE, 12);                          // BR
		}
		T.r[0] = fl::add(T.r[0], rw((uint16_t)(T.r[6] + 0x10)));
		tick(0xD9D2, 27);
		T.r[1] = fl::asl(T.r[1]);
		tick(0xD9D4, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0xD9D6, 12);
		const bool closing = rw((uint16_t)(T.r[6] + 0x0E)) != 0;
		fl::tst(peek((uint16_t)(T.r[6] + 0x0E)));
		tick(0xD9DA, 27);
		if (!closing)
		{
			tick(0xD9DC, 12);
			poke((uint16_t)(0x03B0 + T.r[1]), peek((uint16_t)(0x5B8C + T.r[0])));
			fl::mov(peek((uint16_t)(0x03B0 + T.r[1])));
			tick(0xD9E2, 45);
			tick(0xD9EA, 12);                          // BR
		}
		else
		{
			tick(0xD9E4, 12);                          // BNE
			poke((uint16_t)(0x03B0 + T.r[1]), peek((uint16_t)(0x5BAC + T.r[0])));
			fl::mov(peek((uint16_t)(0x03B0 + T.r[1])));
			tick(0xD9EA, 45);
		}
	}
	poke(0x01D2, T.r[2]);
	fl::mov(T.r[2]);
	tick(0xD9EE, 27);
	poke(0x01D4, T.r[3]);
	fl::mov(T.r[3]);
	tick(0xD9F2, 27);
	poke(0x1400, peek(0x01D2));
	fl::mov(peek(0x01D2));
	tick(0xD9F8, 39);
	poke(0x1402, peek(0x01D4));
	fl::mov(peek(0x01D4));
	tick(0xD9FE, 39);
	T.r[6] += 4;
	fl::cmp(peek((uint16_t)(T.r[6] - 4)), peek((uint16_t)(T.r[6] - 2)));
	tick(0xDA00, 24);                                  // CMP (SP)+,(SP)+
	c_restore(2);
	return T.r[0];
}

}

LOGIC_TABLE(parks,
	c_routine("B3", 0x7F80, "clear_0fb4", clear_0fb4, 0, 0),
	c_routine("A7", 0x5602, "set_ticket_bits", set_ticket_bits, 0, 0),
	c_routine("F", 0xF002, "add_cash", add_cash, 0, 0),
	c_routine("B3", 0x65D6, "add_park_counter", add_park_counter, 0, 0),
	c_routine("A7", 0x5360, "park_at_gate", park_at_gate, 0, 0),
	c_routine("A7", 0x5326, "park_at_area", park_at_area, 0, 0),
	c_routine("F", 0xCDD0, "find_slot_with", find_slot_with, 0, 0),
	c_routine("F", 0xE106, "call_cf5e_keep_banks", call_cf5e_keep_banks, 0, 0),
	c_routine("A7", 0x50C8, "a7_50c8", a7_50c8, 0, 0),
	c_routine("F", 0xDA02, "open_park_gate", open_park_gate, 0, 0),
	c_routine("B3", 0x6E38, "set_medal_level", set_medal_level, 0, 0),
	c_routine("F", 0xD97C, "set_gate_animation", set_gate_animation, 0, 0),
)

}
