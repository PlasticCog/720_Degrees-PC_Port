// Rewritten routines: objects, terrain tables and the zone (tile bank) switches.  They follow
// the original instruction by instruction (tick after every instruction, the stack as the
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

// A5:5166: objects 1-3 move with object 0: x/y velocities (0x04F0, 0x0570) and the bytes
// 0x07F0/0x07F4 are copied; the z velocity 0x05B0 and byte 0x07F8 are passed along the chain
// 1 -> 2 -> 3, taken from object 0 only when 0x031F == 5
uint16_t copy_object0_velocity(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x5168, 24);
	T.r[1] = 3;
	fl::mov(T.r[1]);
	tick(0x516C, 18);
	for (;;)
	{
		T.r[0] = T.r[1];
		fl::mov(T.r[0]);
		tick(0x516E, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x5170, 12);
		poke((uint16_t)(0x04F0 + T.r[0]), peek(0x04F0));
		fl::mov(peek(0x04F0));
		tick(0x5176, 42);
		pokeb((uint16_t)(0x07F0 + T.r[1]), peekb(0x07F0));
		fl::movb(peekb(0x07F0));
		tick(0x517C, 42);
		poke((uint16_t)(0x0570 + T.r[0]), peek(0x0570));
		fl::mov(peek(0x0570));
		tick(0x5182, 42);
		pokeb((uint16_t)(0x07F4 + T.r[1]), peekb(0x07F4));
		fl::movb(peekb(0x07F4));
		tick(0x5188, 42);
		if (--T.r[1] == 0) break;
		tick(0x516C, 18);                              // SOB
	}
	tick(0x518A, 18);
	const bool chain = peekb(0x031F) == 5;
	fl::cmpb(peekb(0x031F), 0x05);
	tick(0x5190, 30);
	if (chain)
	{
		tick(0x5192, 12);
		poke(0x05B2, peek(0x05B0));
		fl::mov(peek(0x05B2));
		tick(0x5198, 39);
		pokeb(0x07F9, peekb(0x07F8));
		fl::movb(peekb(0x07F9));
		tick(0x519E, 39);
	}
	else
		tick(0x519E, 12);                              // BNE
	poke(0x05B4, peek(0x05B2));
	fl::mov(peek(0x05B4));
	tick(0x51A4, 39);
	pokeb(0x07FA, peekb(0x07F9));
	fl::movb(peekb(0x07FA));
	tick(0x51AA, 39);
	poke(0x05B6, peek(0x05B4));
	fl::mov(peek(0x05B6));
	tick(0x51B0, 39);
	pokeb(0x07FB, peekb(0x07FA));
	fl::movb(peekb(0x07FB));
	tick(0x51B6, 39);
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x51B8, 18);
	rts();
	return T.r[0];
}

// F:A2BE  direction_bit(int row): bit (row * 16 + direction 0x031D) of the bitmap at 0x7988
// (masks at 0xFC74); returns the masked byte
uint16_t direction_bit(const Args &a)
{
	c_save(1, 0xA2C2);
	T.r[1] = a.u(0);
	fl::mov(T.r[1]);
	tick(0xA2C6, 27);
	for (uint16_t pc = 0xA2C8; pc <= 0xA2CE; pc += 2)
	{
		T.r[1] = (uint16_t)(T.r[1] << 1);
		tick(pc, 12);                                  // ASL R1
	}
	T.r[0] = sext8(peekb(0x031D));
	fl::movb((uint8_t)T.r[0]);
	tick(0xA2D2, 24);
	T.r[1] = fl::add(T.r[1], T.r[0]);
	tick(0xA2D4, 12);
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0xA2D6, 12);
	for (uint16_t pc = 0xA2D8; pc <= 0xA2DC; pc += 2)
	{
		T.r[0] = (uint16_t)((int16_t)T.r[0] >> 1);
		tick(pc, 12);                                  // ASR R0
	}
	T.r[0] = sext8(peekb((uint16_t)(0x7988 + T.r[0])));
	fl::movb((uint8_t)T.r[0]);
	tick(0xA2E0, 27);
	T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x0007));
	tick(0xA2E4, 18);
	T.r[2] = 0;
	fl::clr();
	tick(0xA2E6, 12);
	T.r[2] = peekb((uint16_t)(0xFC74 + T.r[1]));
	fl::logicb((uint8_t)T.r[2]);
	tick(0xA2EA, 27);
	T.r[2] = (uint16_t)~T.r[2];
	fl::com((uint16_t)(~T.r[2]));
	tick(0xA2EC, 12);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & (uint16_t)~T.r[2]));
	tick(0xA2EE, 12);
	c_restore(1);
	return T.r[0];
}

// F:9E52  slope_table_value(int t): signed byte from the 8 x 16 table at 0x7908 (bank B 0x2C00):
// row = bits 3-5 of t, column = the skater direction 0x031D (reversed when the speed 0x01B4
// is negative).  Bank B is restored afterwards.
uint16_t slope_table_value(const Args &a)
{
	c_save(1, 0x9E56);
	T.r[2] = peek(0x01D4);
	fl::mov(T.r[2]);
	tick(0x9E5A, 24);
	T.r[0] = 0x2C00;
	fl::mov(T.r[0]);
	tick(0x9E5E, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x9E62, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x9E66, 27);
	T.r[1] = sext8(peekb(0x031D));
	fl::movb((uint8_t)T.r[1]);
	tick(0x9E6A, 24);
	const bool backwards = (int16_t)peek(0x01B4) < 0;
	fl::tst(peek(0x01B4));
	tick(0x9E6E, 24);
	if (backwards)
	{
		tick(0x9E70, 12);
		T.r[0] = 0x0008;
		fl::mov(T.r[0]);
		tick(0x9E74, 18);
		T.r[1] ^= T.r[0];
		fl::logic(T.r[1]);
		tick(0x9E76, 12);
	}
	else
		tick(0x9E76, 12);                              // BGE
	T.r[0] = a.u(0);
	fl::mov(T.r[0]);
	tick(0x9E7A, 27);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0038));
	tick(0x9E7E, 18);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x9E80, 12);
	T.r[0] = fl::add(T.r[0], T.r[1]);
	tick(0x9E82, 12);
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0x9E84, 12);
	T.r[1] = sext8(peekb((uint16_t)(0x7908 + T.r[1])));
	fl::movb((uint8_t)T.r[1]);
	tick(0x9E88, 27);
	poke(0x01D4, T.r[2]);
	fl::mov(T.r[2]);
	tick(0x9E8C, 27);
	poke(0x1402, peek(0x01D4));
	fl::mov(peek(0x01D4));
	tick(0x9E92, 39);
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0x9E94, 12);
	c_restore(1);
	return T.r[0];
}

// F:B56E  reset_object(int i): clear object slot i (inactive, no velocity, sprite off screen)
uint16_t reset_object(const Args &a)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xB570, 24);
	T.r[0] = a.u(0);
	fl::mov(T.r[0]);
	tick(0xB574, 27);
	const uint16_t i = T.r[0];
	pokeb((uint16_t)(0x0370 + i), 0);
	fl::clr();
	tick(0xB578, 30);
	T.r[1] = i;
	fl::mov(T.r[1]);
	tick(0xB57A, 12);
	T.r[1] = fl::asl(T.r[1]);
	tick(0xB57C, 12);
	const uint16_t w = T.r[1];
	poke((uint16_t)(0x0430 + w), 0);
	fl::clr();
	tick(0xB580, 30);
	pokeb((uint16_t)(0x0490 + i), fl::logicb((uint8_t)(peekb((uint16_t)(0x0490 + i)) & ~0x98)));
	tick(0xB586, 36);
	pokeb((uint16_t)(0x04B0 + i), fl::logicb((uint8_t)(peekb((uint16_t)(0x04B0 + i)) & ~0x40)));
	tick(0xB58C, 36);
	poke((uint16_t)(0x0630 + w), 0x9000);
	fl::mov(0x9000);
	tick(0xB592, 36);
	poke((uint16_t)(0x0670 + w), 0);
	fl::clr();
	tick(0xB596, 30);
	poke((uint16_t)(0x05F0 + w), 0x8200);
	fl::mov(0x8200);
	tick(0xB59C, 36);
	poke((uint16_t)(0x06B0 + w), 0);
	fl::clr();
	tick(0xB5A0, 30);
	pokeb((uint16_t)(0x0770 + i), 0);
	fl::clr();
	tick(0xB5A4, 30);
	poke((uint16_t)(0x0570 + w), 0);
	fl::clr();
	tick(0xB5A8, 30);
	poke((uint16_t)(0x04F0 + w), 0);
	fl::clr();
	tick(0xB5AC, 30);
	poke((uint16_t)(0x05B0 + w), 0);
	fl::clr();
	tick(0xB5B0, 30);
	poke((uint16_t)(0x03F0 + w), 0);
	fl::clr();
	tick(0xB5B4, 30);
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0xB5B6, 18);
	rts();
	return T.r[0];
}

// F:B5B8  alloc_object_slot(): first slot from 8 on whose 0x0490 flags (0x86) are clear;
// recorded in 0x0FFC, or, when 0x0196 is set, the slot in 0x0FFC is skipped instead.
// Returns the slot (claimed: flags 0x0490 = 0, 0x04B0 = 4) or -1 when none below 32 is free.
uint16_t alloc_object_slot(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xB5BA, 24);
	T.r[0] = 8;
	fl::mov(T.r[0]);
	tick(0xB5BE, 18);
	const bool skip = peekb(0x0196) != 0;
	fl::tstb(peekb(0x0196));
	tick(0xB5C2, 24);
	if (!skip)
	{
		tick(0xB5C4, 12);
		for (;;)
		{
			pokeb(0x0FFC, (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0xB5C8, 27);
			const bool used = (peekb((uint16_t)(0x0490 + T.r[0])) & 0x86) != 0;
			fl::logicb((uint8_t)(0x86 & peekb((uint16_t)(T.r[0] + 0x0490))));
			tick(0xB5CE, 33);
			if (!used) break;
			tick(0xB5D0, 12);
			T.r[0] = fl::inc(T.r[0]);
			tick(0xB5D2, 12);
			tick(0xB5C4, 12);                          // BR
		}
	}
	else
	{
		tick(0xB5D4, 12);                              // BNE
		for (;;)
		{
			T.r[1] = sext8(peekb(0x0FFC));
			fl::movb((uint8_t)T.r[1]);
			tick(0xB5D8, 24);
			fl::cmp(T.r[1], T.r[0]);
			tick(0xB5DA, 12);                          // CMP R1,R0
			if (T.r[1] == T.r[0])
				tick(0xB5E4, 12);                      // BEQ
			else
			{
				tick(0xB5DC, 12);
				const bool used = (peekb((uint16_t)(0x0490 + T.r[0])) & 0x86) != 0;
				fl::logicb((uint8_t)(0x86 & peekb((uint16_t)(T.r[0] + 0x0490))));
				tick(0xB5E2, 33);
				if (!used) break;
				tick(0xB5E4, 12);
			}
			T.r[0] = fl::inc(T.r[0]);
			tick(0xB5E6, 12);
			tick(0xB5D4, 12);                          // BR
		}
	}
	tick(0xB5E8, 12);                                  // BEQ
	fl::cmp(T.r[0], 0x0020);
	tick(0xB5EC, 18);                                  // CMP R0,#0x20
	if ((int16_t)T.r[0] >= 0x20)
	{
		tick(0xB5FA, 12);                              // BGE
		T.r[0] = 0xFFFF;
		fl::mov(T.r[0]);
		tick(0xB5FE, 18);
	}
	else
	{
		tick(0xB5EE, 12);
		pokeb((uint16_t)(0x0490 + T.r[0]), 0);
		fl::clr();
		tick(0xB5F2, 30);
		pokeb((uint16_t)(0x04B0 + T.r[0]), 4);
		fl::movb(0x04);
		tick(0xB5F8, 36);
		tick(0xB5FE, 12);                              // BR
	}
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0xB600, 18);
	rts();
	return T.r[0];
}

// A7:4F42: if the object queued in 0x0E6A is ready (0x04B0 bit 0x40), start it from the
// template at 0x0E5E-0x0E69 and clear the queue (0x0E6A = 0xFF).  Returns the slot.
uint16_t spawn_queued_object(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x4F44, 24);
	T.r[0] = 0;
	fl::clr();
	tick(0x4F46, 12);
	T.r[0] = peekb(0x0E6A);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x4F4A, 24);
	const uint16_t i = T.r[0];
	const bool ready = (peekb((uint16_t)(0x04B0 + i)) & 0x40) != 0;
	fl::logicb((uint8_t)(peekb((uint16_t)(T.r[0] + 0x04B0)) & 0x40));
	tick(0x4F50, 33);
	if (!ready)
		tick(0x4F94, 12);                              // BEQ
	else
	{
		tick(0x4F52, 12);
		pokeb(0x0E68, 0);
		fl::clr();
		tick(0x4F56, 27);
		pokeb(0x0E6A, 0xFF);
		fl::movb(0xFF);
		tick(0x4F5C, 33);
		T.r[1] = i;
		fl::mov(T.r[1]);
		tick(0x4F5E, 12);
		T.r[1] = fl::asl(T.r[1]);
		tick(0x4F60, 12);
		const uint16_t w = T.r[1];
		poke((uint16_t)(0x0430 + w), peek(0x0E66));
		fl::mov(peek(0x0E66));
		tick(0x4F66, 42);
		pokeb((uint16_t)(0x0770 + i), peekb(0x0E69));
		fl::movb(peekb(0x0E69));
		tick(0x4F6C, 42);
		poke((uint16_t)(0x04F0 + w), peek(0x0E5E));
		fl::mov(peek(0x0E5E));
		tick(0x4F72, 42);
		poke((uint16_t)(0x0570 + w), peek(0x0E60));
		fl::mov(peek(0x0E60));
		tick(0x4F78, 42);
		poke((uint16_t)(0x03B0 + w), peek(0x0E62));
		fl::mov(peek(0x0E62));
		tick(0x4F7E, 42);
		poke((uint16_t)(0x03F0 + w), peek(0x0E64));
		fl::mov(peek(0x0E64));
		tick(0x4F84, 42);
		pokeb((uint16_t)(0x0490 + i), fl::logicb((uint8_t)(peekb((uint16_t)(0x0490 + i)) | 0x18)));
		tick(0x4F8A, 36);
		T.r[1] = 0xFFBF;
		fl::mov(T.r[1]);
		tick(0x4F8E, 18);
		T.r[1] = (uint16_t)((T.r[1] & 0xFF00) | (uint8_t)~T.r[1]);
		fl::comb((uint8_t)(~(uint8_t)T.r[1]));
		tick(0x4F90, 12);                              // COMB R1
		pokeb((uint16_t)(0x04B0 + i), fl::logicb((uint8_t)(peekb((uint16_t)(0x04B0 + i)) & (uint8_t)~T.r[1])));
		tick(0x4F94, 30);
	}
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x4F96, 18);
	rts();
	return T.r[0];
}

// F:E0C2: place object 7 relative to the scroll position (0x01A8, -0x01AA), on the side
// given by the signs of object 0 velocities.  Returns the y written.
uint16_t place_object7(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xE0C4, 24);
	poke(0x06FE, peek(0x01A8));
	fl::mov(peek(0x06FE));
	tick(0xE0CA, 39);
	T.r[1] = peek(0x01AA);
	fl::mov(T.r[1]);
	tick(0xE0CE, 24);
	T.r[1] = (uint16_t)-T.r[1];
	fl::neg((uint16_t)(-T.r[1]));
	tick(0xE0D0, 12);
	poke(0x073E, T.r[1]);
	fl::mov(T.r[1]);
	tick(0xE0D4, 27);
	const bool left = (int16_t)peek(0x04F0) < 0;
	fl::tst(peek(0x04F0));
	tick(0xE0D8, 24);
	if (left)
	{
		tick(0xE0DA, 12);
		T.r[0] = 0x8E00;
		fl::mov(T.r[0]);
		tick(0xE0DE, 18);
		tick(0xE0E4, 12);                              // BR
	}
	else
	{
		tick(0xE0E0, 12);                              // BGE
		T.r[0] = 0xF200;
		fl::mov(T.r[0]);
		tick(0xE0E4, 18);
	}
	T.r[0] = fl::add(T.r[0], peek(0x01A8));
	tick(0xE0E8, 24);
	poke(0x06FE, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xE0EC, 27);
	const bool up = (int16_t)peek(0x0570) < 0;
	fl::tst(peek(0x0570));
	tick(0xE0F0, 24);
	if (up)
	{
		tick(0xE0F2, 12);
		T.r[0] = 0x6E00;
		fl::mov(T.r[0]);
		tick(0xE0F6, 18);
		tick(0xE0FC, 12);                              // BR
	}
	else
	{
		tick(0xE0F8, 12);                              // BGE
		T.r[0] = 0x1200;
		fl::mov(T.r[0]);
		tick(0xE0FC, 18);
	}
	T.r[0] = fl::add(T.r[0], T.r[1]);
	tick(0xE0FE, 12);
	poke(0x073E, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xE102, 27);
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0xE104, 18);
	rts();
	return T.r[0];
}

// Zone switches.  The park map is drawn with two tile banks; when the scroll position reaches
// a cell (column 0x0234 = 0..15, row 0x0232 = 0..7) whose zone byte asks for it, the second
// bank changes to that zone (F:EEE6 set_tile_bank).  The low 3 bits of the zone byte select
// a condition; the four routines are the checks for the four edges the scroll crosses.
// Modes 6 and 7 jump outside the original tables and are not used by the game; here they
// never switch.  R0 is returned as the original leaves it.

int16_t zone_col() { return (int16_t)peek(0x0234); }
int16_t zone_row() { return (int16_t)peek(0x0232); }

// the zone byte (argument at k(SP)) into R1, its mode * 2 into R0; then ADD tbl(R0),PC at pc
// (the instruction before it is at pc - 12); returns the mode
uint16_t zone_mode(uint16_t pc, int k)
{
	T.r[1] = rw((uint16_t)(T.r[6] + k));
	tick((uint16_t)(pc - 8), 27);                      // MOV k(SP),R1
	T.r[0] = T.r[1];
	tick((uint16_t)(pc - 6), 12);
	T.r[0] &= 0x0007;
	tick((uint16_t)(pc - 2), 18);
	T.r[0] = (uint16_t)(T.r[0] << 1);
	tick(pc, 12);                                      // ASL R0
	return T.r[0] >> 1;
}

// MOV R1,-(SP); JSR PC,@#F:EEE6 (set_tile_bank); TST (SP)+ at pc
void switch_zone(uint16_t pc)
{
	push(T.r[1]);
	tick((uint16_t)(pc + 2), 24);
	jsr((uint16_t)(pc + 6), 0xEEE6);
	pop();
	tick((uint16_t)(pc + 8), 18);
}

// A5:43F8
uint16_t zone_switch_a(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x43FA, 24);
	const uint16_t mode = zone_mode(0x4406, 4);
	static const uint16_t target[8] = {0x4432, 0x442A, 0x440A, 0x4412, 0x441A, 0x442A, 0x4432, 0x4432};
	tick(target[mode], 27);                            // ADD 0x4436(R0),PC
	bool sw = mode == 1 || mode == 5;
	if (mode >= 2 && mode <= 4)
	{
		int16_t limit;
		if (mode == 4)
		{
			T.r[0] = 0x000F;
			tick(0x441E, 18);
			T.r[0] = fl::sub(T.r[0], zone_col());
			tick(0x4422, 24);
			T.r[0] = fl::asr(T.r[0]);
			tick(0x4424, 12);
			limit = (int16_t)T.r[0];
			sw = zone_row() < limit;
			fl::cmp(peek(0x0232), T.r[0]);
			tick(0x4428, 24);                          // CMP @#0x0232,R0
		}
		else
		{
			sw = mode == 2 ? zone_col() < 14 : zone_row() < 7;
			tick((uint16_t)(mode == 2 ? 0x4410 : 0x4418), 30);
			tick(0x4428, 12);                          // BR
		}
		tick((uint16_t)(sw ? 0x442A : 0x4432), 12);    // BGE
	}
	if (sw) switch_zone(0x442A);
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x4434, 18);
	rts();
	return T.r[0];
}

// A5:4442
uint16_t zone_switch_b(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x4444, 24);
	const uint16_t mode = zone_mode(0x4450, 4);
	static const uint16_t target[8] = {0x4486, 0x4454, 0x447E, 0x4464, 0x4476, 0x447E, 0x4486, 0x4486};
	tick(target[mode], 27);                            // ADD 0x448A(R0),PC
	bool sw = mode == 2 || mode == 5;
	if (mode == 1)
	{
		const bool one = zone_col() == 1;
		tick(0x445A, 30);
		if (one)
			tick(0x4486, 12);                          // BEQ
		else
		{
			tick(0x445C, 12);
			const bool zero = zone_col() == 0;
			fl::tst(peek(0x0234));
			tick(0x4460, 24);
			if (zero)
				tick(0x4486, 12);                      // BEQ
			else
			{
				tick(0x4462, 12);
				tick(0x447E, 12);                      // BR
				sw = true;
			}
		}
	}
	else if (mode == 3 || mode == 4)
	{
		if (mode == 3)
		{
			T.r[0] = (uint16_t)zone_col();
			tick(0x4468, 24);
			T.r[0] = fl::dec(T.r[0]);
			tick(0x446A, 12);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x000F));
			tick(0x446E, 18);
			T.r[0] = fl::asr(T.r[0]);
			tick(0x4470, 12);
			sw = zone_row() < (int16_t)T.r[0];
			fl::cmp(peek(0x0232), T.r[0]);
			tick(0x4474, 24);                          // CMP @#0x0232,R0
			tick(0x447C, 12);                          // BR
		}
		else
		{
			sw = zone_row() < 7;
			fl::cmp(peek(0x0232), 0x0007);
			tick(0x447C, 30);
		}
		tick((uint16_t)(sw ? 0x447E : 0x4486), 12);    // BGE
	}
	if (sw) switch_zone(0x447E);
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x4488, 18);
	rts();
	return T.r[0];
}

// A5:4496
uint16_t zone_switch_c(const Args &)
{
	c_save(3, 0x449A);
	const uint16_t mode = zone_mode(0x44A6, 0x0C);
	static const uint16_t target[8] = {0x44DE, 0x44AA, 0x44B4, 0x44D6, 0x44CE, 0x44D6, 0x44DE, 0x44DE};
	tick(target[mode], 27);                            // ADD 0x44E0(R0),PC
	bool sw = mode == 3 || mode == 5;
	if (mode == 1)
	{
		sw = zone_row() != 1;
		tick(0x44B0, 30);
		if (sw)
		{
			tick(0x44B2, 12);
			tick(0x44D6, 12);                          // BR
		}
		else
			tick(0x44DE, 12);                          // BEQ
	}
	else if (mode == 2)
	{
		T.r[2] = (uint16_t)zone_row();
		tick(0x44B8, 24);
		T.r[2] = fl::dec(T.r[2]);
		tick(0x44BA, 12);
		T.r[0] = (uint16_t)zone_col();
		fl::mov(T.r[0]);
		tick(0x44BE, 24);
		T.r[0] = fl::asr(T.r[0]);
		tick(0x44C0, 12);
		T.r[4] = T.r[0];
		fl::mov(T.r[4]);
		tick(0x44C2, 12);
		T.r[3] = T.r[2];
		fl::mov(T.r[3]);
		tick(0x44C4, 12);
		T.r[3] = fl::logic((uint16_t)(T.r[3] & 0x0007));
		tick(0x44C8, 18);
		fl::cmp(T.r[3], T.r[4]);
		tick(0x44CA, 12);                              // CMP R3,R4
		sw = T.r[3] > T.r[4];
		if (sw)
		{
			tick(0x44CC, 12);
			tick(0x44D6, 12);                          // BR
		}
		else
			tick(0x44DE, 12);                          // BLOS
	}
	else if (mode == 4)
	{
		sw = zone_col() < 14;
		fl::cmp(peek(0x0234), 0x000E);
		tick(0x44D4, 30);
		tick((uint16_t)(sw ? 0x44D6 : 0x44DE), 12);    // BGE
	}
	if (sw) switch_zone(0x44D6);
	c_restore(3);
	return T.r[0];
}

// A5:44EC
uint16_t zone_switch_d(const Args &)
{
	c_save(3, 0x44F0);
	const uint16_t mode = zone_mode(0x44FC, 0x0C);
	static const uint16_t target[8] = {0x453C, 0x4500, 0x4522, 0x452C, 0x4534, 0x4534, 0x453C, 0x453C};
	tick(target[mode], 27);                            // ADD 0x453E(R0),PC
	bool sw = mode == 4 || mode == 5;
	if (mode == 1)
	{
		T.r[2] = (uint16_t)zone_row();
		tick(0x4504, 24);
		T.r[2] = fl::dec(T.r[2]);
		tick(0x4506, 12);
		T.r[0] = 0x0010;
		fl::mov(T.r[0]);
		tick(0x450A, 18);
		T.r[0] = fl::sub(T.r[0], zone_col());
		tick(0x450E, 24);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x000F));
		tick(0x4512, 18);
		T.r[0] = fl::asr(T.r[0]);
		tick(0x4514, 12);
		T.r[4] = T.r[0];
		fl::mov(T.r[4]);
		tick(0x4516, 12);
		T.r[3] = T.r[2];
		fl::mov(T.r[3]);
		tick(0x4518, 12);
		T.r[3] = fl::logic((uint16_t)(T.r[3] & 0x0007));
		tick(0x451C, 18);
		fl::cmp(T.r[3], T.r[4]);
		tick(0x451E, 12);                              // CMP R3,R4
		sw = T.r[3] > T.r[4];
		if (sw)
		{
			tick(0x4520, 12);
			tick(0x4534, 12);                          // BR
		}
		else
			tick(0x453C, 12);                          // BLOS
	}
	else if (mode == 2)
	{
		sw = zone_row() != 1;
		tick(0x4528, 30);
		if (sw)
		{
			tick(0x452A, 12);
			tick(0x4534, 12);                          // BR
		}
		else
			tick(0x453C, 12);                          // BEQ
	}
	else if (mode == 3)
	{
		sw = zone_col() > 2;
		fl::cmp(peek(0x0234), 0x0002);
		tick(0x4532, 30);
		tick((uint16_t)(sw ? 0x4534 : 0x453C), 12);    // BLE
	}
	if (sw) switch_zone(0x4534);
	c_restore(3);
	return T.r[0];
}

}

LOGIC_TABLE(world,
	c_routine("A5", 0x5166, "copy_object0_velocity", copy_object0_velocity, 0, 0),
	c_routine("F", 0xA2BE, "direction_bit", direction_bit, 0, 0),
	c_routine("F", 0x9E52, "slope_table_value", slope_table_value, 0, 0),
	c_routine("F", 0xB56E, "reset_object", reset_object, 0, 0),
	c_routine("F", 0xB5B8, "alloc_object_slot", alloc_object_slot, 0, 0),
	c_routine("A7", 0x4F42, "spawn_queued_object", spawn_queued_object, 0, 0),
	c_routine("F", 0xE0C2, "place_object7", place_object7, 0, 0),
	c_routine("A5", 0x43F8, "zone_switch_a", zone_switch_a, 0, 0),
	c_routine("A5", 0x4442, "zone_switch_b", zone_switch_b, 0, 0),
	c_routine("A5", 0x4496, "zone_switch_c", zone_switch_c, 0, 0),
	c_routine("A5", 0x44EC, "zone_switch_d", zone_switch_d, 0, 0),
)

}
