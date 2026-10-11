// Rewritten routines of the banked segments A5 and A7: the Skate City map view, object
// motion, sounds driven by the game state, the message boxes of the text layer, the game's
// states after a game, high scores, tricks and the skater's motion.
// All follow the original instruction by instruction: after each instruction its cycles are
// charged and interrupts may come in at the next instruction (tick, logic.h); calls go
// through steps::jsr with the real return address and register saves through c_save /
// c_restore, so the stack is the original's (see routines_core.cpp).  The table charges are
// therefore 0.
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
uint16_t asr(uint16_t v, int n) { return (uint16_t)((int16_t)v >> n); }

// flags as the T-11 sets them, for the routines whose condition codes an interrupt may push
// onto the stack mid-way (the stack has to be the original's)
uint16_t swab(uint16_t v)
{
	v = (uint16_t)((v >> 8) | (v << 8));
	f_nzvc(nzb(v));
	return v;
}
uint16_t asl(uint16_t v)
{
	const uint16_t r = (uint16_t)(v << 1);
	const int c = v >> 15, n = r >> 15;
	f_nzvc(nzw(r) | (c ? CF : 0) | ((n ^ c) ? VF : 0));
	return r;
}


// ---------------------------------------------------------------- Skate City map view
// The town map is drawn cell by cell around the scroll position.  0x0F40/0x0F42 are the
// playfield position of the next row/column to draw, 0x0F46/0x0F48 the map cell pointers,
// 0x0F44/0x0F45 the column (32 per cell), 0x0F5A the map width in cells, 0x0F5B the row,
// 0x0F5C/0x0F5D the step and number of cells of the pending strip.

// A5:438C  step_map_column(int dir): move the column strip one cell right (dir > 0) or left
// and set up a vertical strip of 8 cells (0x0F4A = 0xF800)
uint16_t step_map_column(const Args &a)
{
	fl::tst(peek((uint16_t)(T.r[6] + 2)));
	tick(0x4390, 27);                                  // TST 2(SP)
	if (a.s(0) > 0)
	{
		tick(0x4392, 12);
		T.r[0] = 0;
		fl::clr();
		tick(0x4394, 12);
		T.r[0] = peekb(0x0F44);
		fl::logicb((uint8_t)T.r[0]);
		tick(0x4398, 24);
		T.r[0] = fl::add(T.r[0], 0x0020);
		tick(0x439C, 18);
		pokeb(0x0F44, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x43A0, 27);
		poke(0x0F46, fl::add(peek(0x0F46), 2));
		tick(0x43A6, 33);
		T.r[0] = 0;
		fl::clr();
		tick(0x43A8, 12);
		T.r[0] = peekb(0x0F44);
		fl::logicb((uint8_t)T.r[0]);
		tick(0x43AC, 24);
		T.r[0] = fl::add(T.r[0], 0x00A0);
		tick(0x43B0, 18);
		pokeb(0x0F45, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x43B4, 27);
		poke(0x0F48, peek(0x0F46));
		fl::mov(peek(0x0F48));
		tick(0x43BA, 39);
		poke(0x0F48, fl::add(peek(0x0F48), 0x000A));
		tick(0x43C0, 33);
		tick(0x43E0, 12);                              // BR
	}
	else
	{
		tick(0x43C2, 12);                              // BLE
		T.r[0] = 0;
		fl::clr();
		tick(0x43C4, 12);
		T.r[0] = peekb(0x0F44);
		fl::logicb((uint8_t)T.r[0]);
		tick(0x43C8, 24);
		T.r[0] = fl::sub(T.r[0], 0x0020);
		tick(0x43CC, 18);
		pokeb(0x0F44, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x43D0, 27);
		poke(0x0F46, fl::sub(peek(0x0F46), 2));
		tick(0x43D6, 33);
		pokeb(0x0F45, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x43DA, 27);
		poke(0x0F48, peek(0x0F46));
		fl::mov(peek(0x0F48));
		tick(0x43E0, 39);
	}
	poke(0x0F42, peek(0x0F40));
	fl::mov(peek(0x0F42));
	tick(0x43E6, 39);
	pokeb(0x0F5D, 8);
	fl::movb(0x08);
	tick(0x43EC, 33);
	pokeb(0x0F5C, 0);
	fl::clr();
	tick(0x43F0, 27);
	poke(0x0F4A, 0xF800);
	fl::mov(0xF800);
	tick(0x43F6, 33);
	rts();
	return T.r[0];
}

// A5:42BC (assembly)  step_map_row(int dir): move the row strip one cell up (dir > 0) or down
// and set up a horizontal strip of 6 cells.  A row wraps around the map (0x0F5B counts it,
// 0x099E rows); R0/R1 are left as the computation leaves them.
void step_map_row()
{
	const int16_t dir = (int16_t)rw((uint16_t)(T.r[6] + 2));
	fl::tst(peek((uint16_t)(T.r[6] + 2)));
	tick(0x42C0, 27);                                  // TST 2(SP)
	if (dir > 0)
	{
		tick(0x42C2, 12);
		poke(0x0F40, fl::sub(peek(0x0F40), 0x0800));
		tick(0x42C8, 33);
		T.r[1] = 0;
		fl::clr();
		tick(0x42CA, 12);
		T.r[1] = peekb(0x0F5A);
		fl::logicb((uint8_t)T.r[1]);
		tick(0x42CE, 24);
		const bool odd = (peek(0x0F40) & 0x0800) != 0;
		fl::logic((uint16_t)(0x0800 & peek(0x0F40)));
		tick(0x42D4, 30);
		if (odd)
		{
			tick(0x42D6, 12);
			poke(0x0F46, fl::sub(peek(0x0F46), T.r[1]));
			tick(0x42DA, 27);
			T.r[0] = sext8(peekb(0x0F5B));
			fl::movb((uint8_t)T.r[0]);
			tick(0x42DE, 24);
			T.r[0] = fl::dec(T.r[0]);
			tick(0x42E0, 12);
			pokeb(0x0F5B, (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0x42E4, 27);
			if ((int8_t)T.r[0] < 0)                    // BGE on the byte stored
			{
				tick(0x42E6, 12);
				pokeb(0x0F5B, 0x17);
				fl::movb(0x17);
				tick(0x42EC, 33);
				T.r[0] = peek(0x0F46);
				fl::mov(T.r[0]);
				tick(0x42F0, 24);
				T.r[0] = fl::sub(T.r[0], 0x0180);
				tick(0x42F4, 18);
				poke(0x0F46, 0x0A6C);
				fl::mov(0x0A6C);
				tick(0x42FA, 33);
				poke(0x0F46, fl::add(peek(0x0F46), T.r[0]));
				tick(0x42FE, 27);
			}
			else
				tick(0x42FE, 12);
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0x4300, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x4302, 12);
			T.r[0] = fl::add(T.r[0], T.r[1]);
			tick(0x4304, 12);
			tick(0x430C, 12);                          // BR
		}
		else
		{
			tick(0x4306, 12);                          // BEQ
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0x4308, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x430A, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x430C, 12);
		}
		poke(0x0F48, peek(0x0F46));
		fl::mov(peek(0x0F48));
		tick(0x4312, 39);
		poke(0x0F48, fl::sub(peek(0x0F48), T.r[0]));
		tick(0x4316, 27);
		poke(0x0F42, peek(0x0F40));
		fl::mov(peek(0x0F42));
		tick(0x431C, 39);
		poke(0x0F42, fl::sub(peek(0x0F42), 0x3800));
		tick(0x4322, 33);
		tick(0x4374, 12);                              // BR
	}
	else
	{
		tick(0x4324, 12);                              // BLE
		poke(0x0F40, fl::add(peek(0x0F40), 0x0800));
		tick(0x432A, 33);
		const bool odd = (peek(0x0F40) & 0x0800) != 0;
		fl::logic((uint16_t)(0x0800 & peek(0x0F40)));
		tick(0x4330, 30);
		if (!odd)
		{
			tick(0x4332, 12);
			T.r[0] = 0;
			fl::clr();
			tick(0x4334, 12);
			T.r[0] = peekb(0x0F5A);
			fl::logicb((uint8_t)T.r[0]);
			tick(0x4338, 24);
			poke(0x0F46, fl::add(peek(0x0F46), T.r[0]));
			tick(0x433C, 27);
			T.r[0] = sext8(peekb(0x0F5B));
			fl::movb((uint8_t)T.r[0]);
			tick(0x4340, 24);
			T.r[0] = fl::inc(T.r[0]);
			tick(0x4342, 12);
			pokeb(0x0F5B, (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0x4346, 27);
			T.r[0] = peek(0x099E);
			fl::mov(T.r[0]);
			tick(0x434A, 24);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x434C, 12);
			T.r[1] = sext8(peekb(0x0F5B));
			fl::movb((uint8_t)T.r[1]);
			tick(0x4350, 24);
			fl::cmp(T.r[1], T.r[0]);
			tick(0x4352, 12);                          // CMP R1,R0
			if (T.r[1] == T.r[0])
			{
				tick(0x4354, 12);
				pokeb(0x0F5B, 0);
				fl::clr();
				tick(0x4358, 27);
				T.r[0] = peek(0x0F46);
				fl::mov(T.r[0]);
				tick(0x435C, 24);
				T.r[0] = fl::sub(T.r[0], 0x0A80);
				tick(0x4360, 18);
				T.r[0] = fl::add(T.r[0], 0x0194);
				tick(0x4364, 18);
				poke(0x0F46, T.r[0]);
				fl::mov(T.r[0]);
				tick(0x4368, 27);
			}
			else
				tick(0x4368, 12);                      // BNE
		}
		else
			tick(0x4368, 12);                          // BNE
		poke(0x0F42, peek(0x0F40));
		fl::mov(peek(0x0F42));
		tick(0x436E, 39);
		poke(0x0F48, peek(0x0F46));
		fl::mov(peek(0x0F48));
		tick(0x4374, 39);
	}
	pokeb(0x0F45, peekb(0x0F44));
	fl::movb(peekb(0x0F45));
	tick(0x437A, 39);
	pokeb(0x0F5C, 0x20);
	fl::movb(0x20);
	tick(0x4380, 33);
	poke(0x0F4A, 0);
	fl::clr();
	tick(0x4384, 27);
	pokeb(0x0F5D, 6);
	f_nzvc(0);
	tick(0x438A, 33);
	rts();
}

// A5:41CE (assembly): draw the cells of the pending strip (F:E82E draws one cell), then start
// the next strip if the scroll position has moved a cell away from the drawn area (0x0F3E
// rows, 0x0F3C columns).  Registers end as the callees leave them; the flags are those of the
// final stack word (the row or column difference).
// The main loop calls this over and over while it waits for the next frame, so interrupts
// arrive anywhere in it: each instruction is followed by an interrupt point (step), as in
// the original.
void draw_map_strip()
{
	T.r[6] = (uint16_t)(T.r[6] - 2);                   // CLR -(SP): the local word
	const uint16_t local = T.r[6];
	poke(local, 0);
	fl::clr();
	tick(0x41D0, 24);
	poke(local, peekb(0x0F5D));                        // MOVB (sign extension is not done
	fl::movb(peekb(T.r[6]));
	tick(0x41D4, 33);                                  // for a memory destination)
	for (;;)
	{
		const bool cells = peek(local) != 0;
		fl::tst(peek(T.r[6]));
		tick(0x41D6, 18);                              // TST (SP)
		if (!cells)
		{
			tick(0x4254, 12);                          // BEQ
			break;
		}
		tick(0x41D8, 12);
		do
		{
			pokeb(0x0F5D, fl::decb(peekb(0x0F5D)));
			tick(0x41DC, 27);
			poke(0x0F42, fl::logic((uint16_t)(peek(0x0F42) & ~0xC001)));
			tick(0x41E2, 33);
			jsr(0x41E6, 0xE82E);                       // unpack_map_block
			poke(0x0F42, fl::add(peek(0x0F42), peek(0x0F4A)));
			tick(0x41EC, 39);
			T.r[0] = peek(0x0F42);
			fl::mov(T.r[0]);
			tick(0x41F0, 24);
			T.r[1] = peek(0x0F4A);
			fl::mov(T.r[1]);
			tick(0x41F4, 24);
			T.r[1] = (uint16_t)~T.r[1];
			fl::com((uint16_t)(~T.r[1]));
			tick(0x41F6, 12);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & (uint16_t)~T.r[1]));
			tick(0x41F8, 12);
			fl::logic((uint16_t)(0x0800 & T.r[0]));
			tick(0x41FC, 18);                          // BIT #0x0800,R0
			if (T.r[0] & 0x0800)
			{
				tick(0x41FE, 12);
				T.r[0] = 0;
				fl::clr();
				tick(0x4200, 12);
				T.r[0] = peekb(0x0F5A);
				fl::logicb((uint8_t)T.r[0]);
				tick(0x4204, 24);
				poke(0x0F48, fl::sub(peek(0x0F48), T.r[0]));
				tick(0x4208, 27);
				poke(0x0F56, fl::sub(peek(0x0F56), 6));
				tick(0x420E, 33);
			}
			else
				tick(0x420E, 12);
			T.r[0] = 0;
			fl::clr();
			tick(0x4210, 12);
			T.r[0] = peekb(0x0F45);
			fl::logicb((uint8_t)T.r[0]);
			tick(0x4214, 24);
			T.r[1] = sext8(peekb(0x0F5C));
			fl::movb((uint8_t)T.r[1]);
			tick(0x4218, 24);
			T.r[0] = fl::add(T.r[0], T.r[1]);
			tick(0x421A, 12);
			pokeb(0x0F45, (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0x421E, 27);
			fl::tstb((uint8_t)T.r[1]);
			tick(0x4220, 12);                          // TSTB R1
			if ((uint8_t)T.r[1])
			{
				tick(0x4222, 12);
				poke(0x0F48, fl::add(peek(0x0F48), 2));
				tick(0x4228, 33);
				T.r[0] = 0;
				fl::clr();
				tick(0x422A, 12);
				T.r[0] = peekb(0x0F58);
				fl::logicb((uint8_t)T.r[0]);
				tick(0x422E, 24);
				fl::tst(T.r[0]);
				tick(0x4230, 12);                      // TST R0
				if (T.r[0])
				{
					tick(0x4232, 12);
					T.r[1] = T.r[0];
					fl::mov(T.r[1]);
					tick(0x4234, 12);
					T.r[1] = fl::asl(T.r[1]);
					tick(0x4236, 12);
					pokeb(0x0F58, (uint8_t)T.r[1]);
					fl::movb((uint8_t)T.r[1]);
					tick(0x423A, 27);
					if (!(uint8_t)T.r[1])
					{
						tick(0x423C, 12);
						pokeb(0x0F58, fl::incb(peekb(0x0F58)));
						tick(0x4240, 27);
						poke(0x0F56, fl::inc(peek(0x0F56)));
						tick(0x4244, 27);
					}
					else
						tick(0x4244, 12);
				}
				else
					tick(0x4244, 12);
			}
			else
				tick(0x4244, 12);
			poke(local, 0);
			fl::clr();
			tick(0x4246, 21);
			poke(local, peekb(0x0F5D));
			fl::movb(peekb(T.r[6]));
			tick(0x424A, 33);
			fl::tst(peek(T.r[6]));
			tick(0x424C, 18);                          // TST (SP)
			const bool more = peek(local) != 0;
			tick(more ? 0x41D8 : 0x424E, 12);          // BNE
			if (!more) break;
		} while (true);
		pokeb(0x0F5E, fl::incb(peekb(0x0F5E)));
		tick(0x4252, 27);
		tick(0x41D4, 12);                              // BR 41D4
	}
	T.r[0] = peek(0x01AA);
	fl::mov(T.r[0]);
	tick(0x4258, 24);
	T.r[0] = fl::add(T.r[0], 0x0800);
	tick(0x425C, 18);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & (uint16_t)~0x0FFF));
	tick(0x4260, 18);
	poke(local, peek(0x0F3E));
	fl::mov(peek(T.r[6]));
	tick(0x4264, 33);
	poke(local, (uint16_t)(peek(local) - T.r[0]));
	const bool row = peek(local) != 0;
	fl::sub((uint16_t)(peek(T.r[6]) + T.r[0]), T.r[0]);
	tick(0x4266, 21);
	if (row)
	{
		tick(0x4268, 12);
		push(peek(local));                             // MOV (SP),-(SP): the argument
		fl::mov(peek(T.r[6]));
		tick(0x426A, 30);
		jsr(0x426E, 0x42BC);                           // step_map_row
		const bool down = (int16_t)peek(local) >= 0;
		fl::tst(peek((uint16_t)(T.r[6] + 2)));
		tick(0x4272, 27);                              // TST 2(SP)
		if (down)
		{
			tick(0x427C, 12);
			poke(0x0F3E, fl::sub(peek(0x0F3E), 0x1000));
			tick(0x4282, 33);
		}
		else
		{
			tick(0x4274, 12);
			poke(0x0F3E, fl::add(peek(0x0F3E), 0x1000));
			tick(0x427A, 33);
		}
		tick(0x42B2, 12);                              // BR 42B2
	}
	else
	{
		tick(0x4284, 12);
		T.r[0] = peek(0x01A8);
		fl::mov(T.r[0]);
		tick(0x4288, 24);
		T.r[0] = fl::add(T.r[0], 0x1000);
		tick(0x428C, 18);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & (uint16_t)~0x1FFF));
		tick(0x4290, 18);
		T.r[0] = fl::sub(T.r[0], peek(0x0F3C));
		tick(0x4294, 24);
		poke(local, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4296, 21);
		if (!T.r[0])
		{
			tick(0x42B8, 12);                          // BEQ 42B8
			T.r[6] = (uint16_t)(T.r[6] + 2);           // TST (SP)+
			fl::tst(peek(local));
			tick(0x42BA, 18);
			rts();
			return;
		}
		tick(0x4298, 12);
		push(T.r[0]);                                  // MOV R0,-(SP): the argument
		fl::mov(T.r[0]);
		tick(0x429A, 24);
		jsr(0x429E, 0x438C);                           // step_map_column
		const bool right = (int16_t)peek(local) > 0;
		fl::tst(peek((uint16_t)(T.r[6] + 2)));
		tick(0x42A2, 27);                              // TST 2(SP)
		if (right)
		{
			tick(0x42A4, 12);
			poke(0x0F3C, fl::add(peek(0x0F3C), 0x2000));
			tick(0x42AA, 33);
			tick(0x42B2, 12);                          // BR 42B2
		}
		else
		{
			tick(0x42AC, 12);
			poke(0x0F3C, fl::sub(peek(0x0F3C), 0x2000));
			tick(0x42B2, 33);
		}
	}
	jsr(0x42B6, 0x4894);                               // town_alt_cell_bit
	T.r[6] = (uint16_t)(T.r[6] + 2);                   // TST (SP)+: the argument
	fl::tst(peek((uint16_t)(T.r[6] - 2)));
	tick(0x42B8, 18);
	T.r[6] = (uint16_t)(T.r[6] + 2);                   // TST (SP)+: the local word
	fl::tst(peek(local));
	tick(0x42BA, 18);
	rts();
}

// A5:4000  init_map_view(int col, int row, int level): set the scroll position and the map
// pointers for the cell (col, row - 4) of level, then draw the 8 strips around it.  The
// row argument is left decremented on the stack, as the original does.
uint16_t init_map_view(const Args &a)
{
	c_save(4, 0x4004);
	// ROR through the carry, as the original computes the scroll positions
	uint16_t c = 0;
	auto ror = [&](uint16_t v) {
		const uint16_t r = (uint16_t)((c << 15) | (v >> 1));
		c = v & 1;
		return r;
	};
	T.r[6] = (uint16_t)(T.r[6] - 2);                   // TST -(SP): the strip counter
	const uint16_t counter = T.r[6];
	fl::tst(peek(T.r[6]));
	tick(0x4006, 21);
	pokeb(0x0F59, a.b(2));
	fl::movb(peekb(0x0F59));
	tick(0x400C, 42);
	c = a.u(1) < 4;
	poke((uint16_t)(a.sp + 4), (uint16_t)(a.u(1) - 4));
	fl::sub((uint16_t)(peek((uint16_t)(T.r[6] + 0x12)) + 0x0004), 0x0004);
	tick(0x4012, 36);
	T.r[2] = a.u(1);
	fl::mov(T.r[2]);
	tick(0x4016, 27);
	pokeb(0x0F5B, (uint8_t)T.r[2]);
	fl::movb((uint8_t)T.r[2]);
	tick(0x401A, 27);
	T.r[3] = a.u(0);
	fl::mov(T.r[3]);
	tick(0x401E, 27);
	T.r[0] = T.r[3];
	fl::mov(T.r[0]);
	tick(0x4020, 12);
	for (uint16_t pc = 0x4022; pc <= 0x4028; pc = (uint16_t)(pc + 2))
	{
		T.r[0] = ror(T.r[0]);
		tick(pc, 12);
	}
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0xE000));
	tick(0x402C, 18);
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0x402E, 12);
	c = (T.r[1] + 0x2000u) >> 16;
	T.r[1] = fl::add(T.r[1], 0x2000);
	tick(0x4032, 18);
	poke(0x01A8, T.r[1]);
	fl::mov(T.r[1]);
	tick(0x4036, 27);
	poke(0x01AC, T.r[1]);
	fl::mov(T.r[1]);
	tick(0x403A, 27);
	T.r[0] = T.r[2];
	fl::mov(T.r[0]);
	tick(0x403C, 12);
	for (uint16_t pc = 0x403E; pc <= 0x4044; pc = (uint16_t)(pc + 2))
	{
		T.r[0] = ror(T.r[0]);
		tick(pc, 12);
	}
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0xE000));
	tick(0x4048, 18);
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0x404A, 12);
	T.r[1] = fl::add(T.r[1], 0x1000);
	tick(0x404E, 18);
	poke(0x01AA, T.r[1]);
	fl::mov(T.r[1]);
	tick(0x4052, 27);
	poke(0x01AE, T.r[1]);
	fl::mov(T.r[1]);
	tick(0x4056, 27);
	poke(0x0F3C, peek(0x01A8));
	fl::mov(peek(0x0F3C));
	tick(0x405C, 39);
	poke(0x0F3C, fl::logic((uint16_t)(peek(0x0F3C) & 0xE000)));
	tick(0x4062, 33);
	poke(0x0F3E, T.r[1]);
	fl::mov(T.r[1]);
	tick(0x4066, 27);
	poke(0x0F3E, fl::logic((uint16_t)(peek(0x0F3E) & 0xF000)));
	tick(0x406C, 33);
	poke(0x01B0, 0);
	fl::clr();
	tick(0x4070, 27);
	poke(0x01B2, 0);
	fl::clr();
	tick(0x4074, 27);
	T.r[1] = 0;
	fl::clr();
	tick(0x4076, 12);
	T.r[1] = a.b(2);
	fl::logicb((uint8_t)T.r[1]);
	tick(0x407A, 27);
	pokeb(0x01CC, rb((uint16_t)(0xFEC2 + T.r[1])));
	fl::movb(peekb(0x01CC));
	tick(0x4080, 42);
	pokeb(0x01CD, 0);
	fl::clr();
	tick(0x4084, 27);
	pokeb(0x01CE, rb((uint16_t)(0xFECA + T.r[1])));
	fl::movb(peekb(0x01CE));
	tick(0x408A, 42);
	pokeb(0x01CF, 0);
	fl::clr();
	tick(0x408E, 27);
	T.r[0] = peek(0x01CC);
	fl::mov(T.r[0]);
	tick(0x4092, 24);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x4094, 12);
	poke(0x01D6, rw((uint16_t)(0xFED2 + T.r[0])));
	fl::mov(peek(0x01D6));
	tick(0x409A, 42);
	T.r[0] = peek(0x01CE);
	fl::mov(T.r[0]);
	tick(0x409E, 24);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x40A0, 12);
	poke(0x01D8, rw((uint16_t)(0xFED2 + T.r[0])));
	fl::mov(peek(0x01D8));
	tick(0x40A6, 42);
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0x40A8, 12);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x40AA, 12);
	poke(0x0F46, peek((uint16_t)(0x7F8C + T.r[0])));
	fl::mov(peek(0x0F46));
	tick(0x40B0, 42);
	fl::tst(T.r[1]);
	tick(0x40B2, 12);                                  // TST R1
	bool town = false;
	if ((int16_t)T.r[1] > 0)
	{
		tick(0x40B4, 12);
		fl::cmp(T.r[1], 0x0005);
		tick(0x40B8, 18);                              // CMP R1,#5
		town = (int16_t)T.r[1] < 5;
		tick(town ? 0x40BA : 0x40E6, 12);              // BGE
	}
	else
		tick(0x40E6, 12);                              // BLE
	if (town)
	{
		jsr(0x40BE, 0x90BA);
		T.r[1] = 0;
		fl::clr();
		tick(0x40C0, 12);
		T.r[1] = rb((uint16_t)(0xFE8F + T.r[0]));
		fl::logicb((uint8_t)T.r[1]);
		tick(0x40C4, 27);
		T.r[1] = fl::add(T.r[1], peek(0x0F46));
		tick(0x40C8, 24);
		pokeb(0x0F5A, peekb((uint16_t)(T.r[1] + 0x000C)));
		fl::movb(peekb(0x0F5A));
		tick(0x40CE, 42);
		jsr(0x40D2, 0x90BA);
		T.r[0] = sext8(rb((uint16_t)(0xFE8F + T.r[0])));
		fl::movb((uint8_t)T.r[0]);
		tick(0x40D6, 27);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
		tick(0x40DA, 18);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x40DC, 12);
		T.r[0] = fl::add(T.r[0], peek(0x0F46));
		tick(0x40E0, 24);
		poke(0x0F46, peek(T.r[0]));
		fl::mov(peek(0x0F46));
		tick(0x40E4, 33);
		tick(0x40EC, 12);                              // BR
	}
	else
	{
		pokeb(0x0F5A, peekb((uint16_t)(0x7F9C + T.r[1])));
		fl::movb(peekb(0x0F5A));
		tick(0x40EC, 42);
	}
	push(T.r[2]);
	fl::mov(T.r[2]);
	tick(0x40EE, 24);
	push(0);
	fl::clr();
	tick(0x40F0, 24);
	pokeb(T.r[6], peekb(0x0F5A));
	fl::movb(peekb(T.r[6]));
	tick(0x40F4, 33);
	jsr(0x40F8, 0x80D8);                               // mul16
	T.r[0] = fl::add(T.r[0], peek(0x0F46));
	tick(0x40FC, 24);
	T.r[1] = T.r[3];
	fl::mov(T.r[1]);
	tick(0x40FE, 12);
	T.r[1] = fl::asl(T.r[1]);
	tick(0x4100, 12);
	T.r[0] = fl::add(T.r[0], T.r[1]);
	tick(0x4102, 12);
	poke(0x0F46, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x4106, 27);
	T.r[0] = T.r[2];
	fl::mov(T.r[0]);
	tick(0x4108, 12);
	T.r[0] = (uint16_t)((T.r[0] >> 8) | (T.r[0] << 8));
	tick(0x410A, 12);                                  // SWAB
	T.r[0] &= 0xFF00;
	fl::clr();
	tick(0x410C, 12);                                  // CLRB
	for (uint16_t pc = 0x410E; pc <= 0x4114; pc = (uint16_t)(pc + 2))
	{
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(pc, 12);
	}
	poke(0x0F40, T.r[0]);
	tick(0x4118, 27);
	poke(0x0F40, fl::sub(peek(0x0F40), 0x0800));
	tick(0x411E, 33);
	T.r[0] = T.r[3];
	fl::mov(T.r[0]);
	tick(0x4120, 12);
	for (uint16_t pc = 0x4122; pc <= 0x412A; pc = (uint16_t)(pc + 2))
	{
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(pc, 12);
	}
	pokeb(0x0F44, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x412E, 27);
	poke(0x099A, T.r[2]);
	fl::mov(T.r[2]);
	tick(0x4132, 27);
	poke(0x099A, asr(peek(0x099A), 1));
	tick(0x4136, 27);
	T.r[0] = T.r[3];
	fl::mov(T.r[0]);
	tick(0x4138, 12);
	T.r[0] = fl::dec(T.r[0]);
	tick(0x413A, 12);
	poke(0x0996, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x413E, 27);
	poke(0x0996, asr(peek(0x0996), 1));
	tick(0x4142, 27);
	poke(0x099C, peek(0x099A));
	fl::mov(peek(0x099C));
	tick(0x4148, 39);
	poke(0x099C, fl::add(peek(0x099C), 3));
	tick(0x414E, 33);
	for (uint16_t pc = 0x4152; pc <= 0x415E; pc = (uint16_t)(pc + 4))
	{
		poke(0x0996, (uint16_t)(peek(0x0996) << 1));
		tick(pc, 27);
	}
	poke(0x0998, peek(0x0996));
	tick(0x4164, 39);
	poke(0x0998, fl::add(peek(0x0998), 0x0040));
	tick(0x416A, 33);
	T.r[0] = 0;
	fl::clr();
	tick(0x416C, 12);
	T.r[0] = peekb(0x0F59);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x4170, 24);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x4172, 12);
	poke(0x0F4E, rw((uint16_t)(0xFF54 + T.r[0])));
	fl::mov(peek(0x0F4E));
	tick(0x4178, 42);
	const bool other = peekb(0x0F59) != 0;
	fl::tstb(peekb(0x0F59));
	tick(0x417C, 24);                                  // TSTB @#0x0F59
	if (other)
	{
		tick(0x4186, 12);                              // BNE
		poke(0x099E, 0x0FFF);
		fl::mov(0x0FFF);
		tick(0x418C, 33);
	}
	else
	{
		tick(0x417E, 12);
		poke(0x099E, 0x000C);
		fl::mov(0x000C);
		tick(0x4184, 33);
		tick(0x418C, 12);                              // BR
	}
	pokeb(0x0E9B, 0);
	fl::clr();
	tick(0x4190, 27);
	poke(0x09A0, peek(0x01A8));
	fl::mov(peek(0x09A0));
	tick(0x4196, 39);
	poke(0x09A0, fl::logic((uint16_t)(peek(0x09A0) & 0xC000)));
	tick(0x419C, 33);
	poke(0x09A2, peek(0x01AA));
	fl::mov(peek(0x09A2));
	tick(0x41A2, 39);
	poke(0x09A2, fl::logic((uint16_t)(peek(0x09A2) & 0xC000)));
	tick(0x41A8, 33);
	pokeb(0x0F58, 0);
	fl::clr();
	tick(0x41AC, 27);
	poke(counter, 0x0008);
	fl::mov(0x0008);
	tick(0x41B2, 36);
	for (;;)
	{
		poke(T.r[6], 0xFFFF);
		fl::mov(0xFFFF);
		tick(0x41B6, 27);
		jsr(0x41BA, 0x42BC);                           // step_map_row(-1)
		jsr(0x41BE, 0x4894);                           // town_alt_cell_bit
		jsr(0x41C2, 0x41CE);                           // draw_map_strip
		poke(counter, (uint16_t)(peek(counter) - 1));
		const bool more = peek(counter) != 0;
		fl::dec((uint16_t)(peek((uint16_t)(T.r[6] + 4)) + 1));
		tick(0x41C6, 30);                              // DEC 4(SP)
		if (!more)
		{
			tick(0x41C8, 12);                          // BNE
			break;
		}
		tick(0x41B2, 12);
	}
	T.r[6] = fl::add(T.r[6], 6);
	tick(0x41CC, 18);
	c_restore(4);
	return T.r[0];
}

// A5:48CC (assembly): the map slots from 0x09B0 down to 0x0E98: a slot whose cell (0x0AF8:
// column in bits 0-3, row in bits 4-8) is inside the view (columns 0x099A-0x099C, rows
// 0x0996-0x0998) is activated (F:C4E4) unless it is active (bit 15), one outside is
// deactivated (F:C3E6).  The flag word on the stack is passed to the callees.
void update_map_slots()
{
	T.r[6] = (uint16_t)(T.r[6] - 2);                   // TST -(SP): the flag word
	const uint16_t local = T.r[6];
	fl::tst(peek(T.r[6]));
	tick(0x48CE, 21);
	pokeb(0x0E99, 0);
	fl::clr();
	tick(0x48D2, 27);
	poke(0x09AA, peek(0x09B0));
	fl::mov(peek(0x09AA));
	tick(0x48D8, 39);
	T.r[4] = peek(0x099A);
	fl::mov(T.r[4]);
	tick(0x48DC, 24);
	T.r[2] = peek(0x099C);
	fl::mov(T.r[2]);
	tick(0x48E0, 24);
	T.r[3] = peek(0x0996);
	fl::mov(T.r[3]);
	tick(0x48E4, 24);
	T.r[1] = peek(0x0998);
	fl::mov(T.r[1]);
	tick(0x48E8, 24);
	fl::tst(T.r[4]);
	tick(0x48EA, 12);                                  // TST R4
	if ((int16_t)T.r[4] < 0)
	{
		tick(0x48EC, 12);
		fl::cmp(T.r[3], 0x00C0);
		tick(0x48F0, 18);                              // CMP R3,#0x00C0
		if ((int16_t)T.r[3] >= 0x00C0)
		{
			tick(0x48F2, 12);
			T.r[4] = fl::add(T.r[4], 0x000C);
			tick(0x48F6, 18);
			T.r[2] = fl::add(T.r[2], 0x000C);
			tick(0x48FA, 18);
			T.r[3] = fl::sub(T.r[3], 0x0050);
			tick(0x48FE, 18);
			T.r[1] = fl::sub(T.r[1], 0x0050);
			tick(0x4902, 18);
		}
		else
			tick(0x4902, 12);                          // BLT
	}
	else
		tick(0x4902, 12);                              // BGE
	for (;;)
	{
		T.r[0] = peek(0x09AA);
		fl::mov(T.r[0]);
		tick(0x4906, 24);
		push(0);
		fl::clr();
		tick(0x4908, 24);
		pokeb(T.r[6], peekb(0x0E98));
		fl::movb(peekb(T.r[6]));
		tick(0x490C, 33);
		const bool done = (int16_t)T.r[0] < (int16_t)pop();
		fl::cmp(T.r[0], peek((uint16_t)(T.r[6] - 2)));
		tick(0x490E, 18);                              // CMP R0,(SP)+
		if (done)
		{
			tick(0x4958, 12);                          // BLT
			break;
		}
		tick(0x4910, 12);
		const bool used = peekb((uint16_t)(0x09B8 + T.r[0])) != 0;
		fl::tstb(peekb((uint16_t)(T.r[0] + 0x09B8)));
		tick(0x4914, 27);
		if (used)
		{
			tick(0x4916, 12);
			poke(local, 0);
			fl::clr();
			tick(0x4918, 21);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x491A, 12);
			T.r[0] = peek((uint16_t)(0x0AF8 + T.r[0]));
			fl::mov(T.r[0]);
			tick(0x491E, 27);
			T.r[5] = T.r[0];
			fl::mov(T.r[5]);
			tick(0x4920, 12);
			T.r[5] = fl::logic((uint16_t)(T.r[5] & 0x000F));
			tick(0x4924, 18);
			fl::cmp(T.r[5], T.r[4]);
			tick(0x4926, 12);                          // CMP R5,R4
			if ((int16_t)T.r[5] < (int16_t)T.r[4])
				tick(0x493C, 12);                      // BLT
			else
			{
				tick(0x4928, 12);
				fl::cmp(T.r[5], T.r[2]);
				tick(0x492A, 12);                      // CMP R5,R2
				if ((int16_t)T.r[5] > (int16_t)T.r[2])
					tick(0x493C, 12);                  // BGT
				else
				{
					tick(0x492C, 12);
					T.r[5] = T.r[0];
					fl::mov(T.r[5]);
					tick(0x492E, 12);
					T.r[5] = fl::logic((uint16_t)(T.r[5] & 0x01F0));
					tick(0x4932, 18);
					fl::cmp(T.r[5], T.r[3]);
					tick(0x4934, 12);                  // CMP R5,R3
					if ((int16_t)T.r[5] < (int16_t)T.r[3])
						tick(0x493C, 12);              // BLT
					else
					{
						tick(0x4936, 12);
						fl::cmp(T.r[5], T.r[1]);
						tick(0x4938, 12);              // CMP R5,R1
						if ((int16_t)T.r[5] >= (int16_t)T.r[1])
							tick(0x493C, 12);          // BGE
						else
						{
							tick(0x493A, 12);
							poke(local, fl::inc(peek(local)));
							tick(0x493C, 21);
						}
					}
				}
			}
			const bool inside = peek(local) != 0;
			fl::tst(peek(T.r[6]));
			tick(0x493E, 18);                          // TST (SP)
			if (!inside)
			{
				tick(0x4940, 12);
				fl::tst(T.r[0]);
				tick(0x4942, 12);                      // TST R0
				if ((int16_t)T.r[0] < 0)
				{
					tick(0x4944, 12);
					jsr(0x4948, 0xC3E6);
					tick(0x4952, 12);                  // BR
				}
				else
					tick(0x4952, 12);                  // BGE
			}
			else
			{
				tick(0x494A, 12);                      // BNE
				fl::tst(T.r[0]);
				tick(0x494C, 12);                      // TST R0
				if ((int16_t)T.r[0] >= 0)
				{
					tick(0x494E, 12);
					jsr(0x4952, 0xC4E4);
				}
				else
					tick(0x4952, 12);                  // BLT
			}
		}
		else
			tick(0x4952, 12);                          // BEQ
		poke(0x09AA, fl::dec(peek(0x09AA)));
		tick(0x4956, 27);
		tick(0x4902, 12);                              // BR
	}
	T.r[6] = (uint16_t)(T.r[6] + 2);
	fl::tst(peek(local));
	tick(0x495A, 18);                                  // TST (SP)+
	rts();
}

// ---------------------------------------------------------------- sounds

// A5:5D08  stop_effect_sound(int n): if sound effect n (table 0x6567, bank B 0x0400) plays on
// an effect channel (0x01A6 / 0x01A7), free the channel and queue its stop command
uint16_t stop_effect_sound(const Args &a)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x5D0A, 24);
	T.r[0] = 0x0400;
	fl::mov(T.r[0]);
	tick(0x5D0E, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x5D12, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x5D16, 27);
	T.r[0] = a.u(0);
	fl::mov(T.r[0]);
	tick(0x5D1A, 27);
	T.r[0] = sext8(peekb((uint16_t)(0x6567 + T.r[0])));
	fl::movb((uint8_t)T.r[0]);
	tick(0x5D1E, 27);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
	tick(0x5D22, 18);
	if (T.r[0])
	{
		tick(0x5D24, 12);
		T.r[1] = 0;
		fl::clr();
		tick(0x5D26, 12);
		T.r[1] = peekb(0x01A6);
		fl::logicb((uint8_t)T.r[1]);
		tick(0x5D2A, 24);
		fl::cmp(T.r[1], T.r[0]);
		tick(0x5D2C, 12);                              // CMP R1,R0
		bool found = true;
		if (T.r[1] == T.r[0])
		{
			tick(0x5D2E, 12);
			pokeb(0x01A6, 0);
			fl::clr();
			tick(0x5D32, 27);
			push(0x0070);
			fl::mov(0x0070);
			tick(0x5D36, 30);
			tick(0x5D4A, 12);                          // BR
		}
		else
		{
			tick(0x5D38, 12);                          // BNE
			T.r[1] = 0;
			fl::clr();
			tick(0x5D3A, 12);
			T.r[1] = peekb(0x01A7);
			fl::logicb((uint8_t)T.r[1]);
			tick(0x5D3E, 24);
			fl::cmp(T.r[1], T.r[0]);
			tick(0x5D40, 12);                          // CMP R1,R0
			if (T.r[1] == T.r[0])
			{
				tick(0x5D42, 12);
				pokeb(0x01A7, 0);
				fl::clr();
				tick(0x5D46, 27);
				push(0x0076);
				fl::mov(0x0076);
				tick(0x5D4A, 30);
			}
			else
			{
				tick(0x5D50, 12);                      // BNE
				found = false;
			}
		}
		if (found)
		{
			jsr(0x5D4E, 0x8E78);                       // queue_sound
			T.r[6] = (uint16_t)(T.r[6] + 2);
			fl::tst(peek((uint16_t)(T.r[6] - 2)));
			tick(0x5D50, 18);                          // TST (SP)+
		}
	}
	else
		tick(0x5D50, 12);                              // BEQ
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x5D52, 18);
	rts();
	return T.r[0];
}

// A5:5B46: rolling sound: when the skater's speed differs from the sound's level 0x01DC by
// at least 50, the level steps by 27 towards it with the matching sound command (0x46/0x48
// up, 0x49 down, 0x53 to silence below speed 128).  Not while 0x031F or 0x0330 is set.
uint16_t rolling_sound(const Args &)
{
	c_save(2, 0x5B4A);
	const bool fallen = peekb(0x031F) != 0;
	fl::tstb(peekb(0x031F));
	tick(0x5B4E, 24);                                  // TSTB @#0x031F
	if (fallen)
	{
		tick(0x5BD0, 12);                              // BNE
		c_restore(2);
		return T.r[0];
	}
	tick(0x5B50, 12);
	const bool other = peekb(0x0330) != 0;
	fl::tstb(peekb(0x0330));
	tick(0x5B54, 24);                                  // TSTB @#0x0330
	if (other)
	{
		tick(0x5BD0, 12);                              // BNE
		c_restore(2);
		return T.r[0];
	}
	tick(0x5B56, 12);
	T.r[2] = peek(0x01B4);
	fl::mov(T.r[2]);
	tick(0x5B5A, 24);
	T.r[3] = 0;
	fl::clr();
	tick(0x5B5C, 12);
	fl::tst(T.r[2]);
	tick(0x5B5E, 12);                                  // TST R2
	if (T.r[2] & 0x8000)
	{
		tick(0x5B66, 12);                              // BLT
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0x5B68, 12);
		T.r[0] = (uint16_t)-T.r[0];
		fl::neg((uint16_t)(-T.r[0]));
		tick(0x5B6A, 12);
	}
	else
	{
		tick(0x5B60, 12);
		T.r[3] = fl::inc(T.r[3]);
		tick(0x5B62, 12);
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0x5B64, 12);
		tick(0x5B6A, 12);                              // BR
	}
	T.r[0] = fl::sub(T.r[0], peek(0x01DC));
	tick(0x5B6E, 24);
	if (T.r[0] & 0x8000)
	{
		tick(0x5B74, 12);                              // BMI
		T.r[1] = T.r[0];
		fl::mov(T.r[1]);
		tick(0x5B76, 12);
		T.r[1] = (uint16_t)-T.r[1];
		fl::neg((uint16_t)(-T.r[1]));
		tick(0x5B78, 12);
	}
	else
	{
		tick(0x5B70, 12);
		T.r[1] = T.r[0];
		fl::mov(T.r[1]);
		tick(0x5B72, 12);
		tick(0x5B78, 12);                              // BR
	}
	fl::cmp(T.r[1], 0x0032);
	tick(0x5B7C, 18);                                  // CMP R1,#0x0032
	if ((int16_t)T.r[1] < 0x0032)
	{
		tick(0x5B7E, 12);                              // BGE
		T.r[0] = 0;
		fl::clr();
		tick(0x5B80, 12);
	}
	else
		tick(0x5B80, 12);
	fl::tst(T.r[0]);
	tick(0x5B82, 12);                                  // TST R0
	if (!T.r[0])
	{
		tick(0x5BD0, 12);                              // BEQ
		c_restore(2);
		return T.r[0];
	}
	tick(0x5B84, 12);
	if ((int16_t)T.r[0] > 0)
	{
		tick(0x5B86, 12);                              // BLE
		const bool up = (int16_t)peek(0x01DC) > 0;
		fl::tst(peek(0x01DC));
		tick(0x5B8A, 24);                              // TST @#0x01DC
		if (up)
		{
			tick(0x5B92, 12);                          // BGT
			push(0x0048);
			fl::mov(0x0048);
			tick(0x5B96, 30);
		}
		else
		{
			tick(0x5B8C, 12);
			push(0x0046);
			fl::mov(0x0046);
			tick(0x5B90, 30);
			tick(0x5B96, 12);                          // BR
		}
		jsr(0x5B9A, 0x8E78);                           // queue_sound
		poke(0x01DC, fl::add(peek(0x01DC), 0x001B));
		tick(0x5BA0, 33);
		tick(0x5BCE, 12);                              // BR
	}
	else
	{
		tick(0x5BA2, 12);                              // BLE
		poke(0x01DC, fl::sub(peek(0x01DC), 0x001B));
		tick(0x5BA8, 33);
		const bool forward = (T.r[3] & 1) != 0;
		T.r[3] = (uint16_t)(T.r[3] >> 1);              // ROR R3 (its old bit 0 into C)
		tick(0x5BAA, 12);
		if (forward)
			tick(0x5BAE, 12);                          // BCS
		else
		{
			tick(0x5BAC, 12);
			T.r[2] = (uint16_t)-T.r[2];
			fl::neg((uint16_t)(-T.r[2]));
			tick(0x5BAE, 12);
		}
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0x5BB0, 12);
		fl::cmp(T.r[0], 0x0080);
		tick(0x5BB4, 18);                              // CMP R0,#0x0080
		bool silence = false;
		if ((int16_t)T.r[0] < 0x0080)
		{
			tick(0x5BB6, 12);                          // BGE
			silence = peek(0x01DC) != 0;
			fl::tst(peek(0x01DC));
			tick(0x5BBA, 24);                          // TST @#0x01DC
			tick(silence ? 0x5BBC : 0x5BC6, 12);       // BEQ
		}
		else
			tick(0x5BC6, 12);                          // BGE
		if (silence)
		{
			poke(0x01DC, 0);
			fl::clr();
			tick(0x5BC0, 27);
			push(0x0053);
			fl::mov(0x0053);
			tick(0x5BC4, 30);
			tick(0x5BCA, 12);                          // BR
		}
		else
		{
			push(0x0049);
			fl::mov(0x0049);
			tick(0x5BCA, 30);
		}
		jsr(0x5BCE, 0x8E78);                           // queue_sound
	}
	T.r[6] = (uint16_t)(T.r[6] + 2);
	fl::tst(peek((uint16_t)(T.r[6] - 2)));
	tick(0x5BD0, 18);                                  // TST (SP)+
	c_restore(2);
	return T.r[0];
}

// A5:5BD2 (assembly): Skate City music.  The music level 0x01E6 (0-13, commands from 0x5AF6)
// follows the distance of the view (0x0996, 0x099A) from the cell column 9, row 5.  Every
// 256 frames the sound CPU is asked whether a tune is playing (command 0x0D, interrupts off
// meanwhile); if not, a random tune not played yet (bits of 0x01DA, commands at 0x5B04)
// starts, the first time with F:C98E(5, 0x40) when 0x0B78 is negative.
void town_music()
{
	const uint8_t other = peekb(0x0F59);
	fl::tstb(other);
	tick(0x5BD6, 24);
	if (other)
	{
		tick(0x5CBE, 12);                              // BNE
		rts();
		return;
	}
	tick(0x5BD8, 12);
	T.r[1] = peek(0x0996);
	fl::mov(T.r[1]);
	tick(0x5BDC, 24);
	T.r[0] = peek(0x099A);
	fl::mov(T.r[0]);
	tick(0x5BE0, 24);
	if (T.r[0] & 0x8000)
	{
		tick(0x5BE2, 12);                              // BGE
		T.r[1] = fl::add(T.r[1], 0x0050);
		tick(0x5BE6, 18);
		T.r[0] = fl::add(T.r[0], 0x000C);
		tick(0x5BEA, 18);
	}
	else
		tick(0x5BEA, 12);
	T.r[2] = T.r[1];
	fl::mov(T.r[2]);
	tick(0x5BEC, 12);
	T.r[2] = fl::sub(T.r[2], 0x0090);
	tick(0x5BF0, 18);
	if (T.r[2] & 0x8000)
	{
		tick(0x5BF2, 12);                              // BPL
		T.r[2] = (uint16_t)-T.r[2];
		fl::neg((uint16_t)(-T.r[2]));
		tick(0x5BF4, 12);
	}
	else
		tick(0x5BF4, 12);
	T.r[1] = T.r[2];
	fl::mov(T.r[1]);
	tick(0x5BF6, 12);
	for (uint16_t pc = 0x5BF8; pc <= 0x5BFE; pc = (uint16_t)(pc + 2))
	{
		T.r[1] = asr(T.r[1], 1);
		tick(pc, 12);
	}
	T.r[2] = T.r[0];
	fl::mov(T.r[2]);
	tick(0x5C00, 12);
	T.r[2] = fl::sub(T.r[2], 5);
	tick(0x5C04, 18);
	if (T.r[2] & 0x8000)
	{
		tick(0x5C06, 12);                              // BPL
		T.r[2] = (uint16_t)-T.r[2];
		fl::neg((uint16_t)(-T.r[2]));
		tick(0x5C08, 12);
	}
	else
		tick(0x5C08, 12);
	T.r[0] = T.r[2];
	fl::mov(T.r[0]);
	tick(0x5C0A, 12);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x5C0C, 12);
	fl::cmp(T.r[1], T.r[0]);
	tick(0x5C0E, 12);                                  // CMP R1,R0
	if ((int16_t)T.r[1] <= (int16_t)T.r[0])
	{
		tick(0x5C10, 12);                              // BGT
		T.r[1] = T.r[0];
		fl::mov(T.r[1]);
		tick(0x5C12, 12);
	}
	else
		tick(0x5C12, 12);
	fl::cmp(T.r[1], 0x000D);
	tick(0x5C16, 18);                                  // CMP R1,#13
	if ((int16_t)T.r[1] > 0x000D)
	{
		tick(0x5C18, 12);                              // BLE
		T.r[1] = 0x000D;
		fl::mov(T.r[1]);
		tick(0x5C1C, 18);
	}
	else
		tick(0x5C1C, 12);
	T.r[0] = 0;
	fl::clr();
	tick(0x5C1E, 12);
	T.r[0] = peekb(0x01E6);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x5C22, 24);
	fl::cmp(T.r[1], T.r[0]);
	tick(0x5C24, 12);                                  // CMP R1,R0
	if (T.r[1] != T.r[0])
	{
		tick(0x5C26, 12);                              // BEQ
		push(0);
		fl::clr();
		tick(0x5C28, 24);
		pokeb(T.r[6], peekb((uint16_t)(0x5AF6 + T.r[1])));
		fl::movb(peekb(T.r[6]));
		tick(0x5C2C, 36);
		jsr(0x5C30, 0x8E78);                           // queue_sound(level command)
		pokeb(0x01E6, (uint8_t)T.r[1]);
		fl::movb((uint8_t)T.r[1]);
		tick(0x5C34, 27);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(peek((uint16_t)(T.r[6] - 2)));
		tick(0x5C36, 18);                              // TST (SP)+
	}
	else
		tick(0x5C36, 12);
	const uint8_t frame = peekb(0x01C4);
	fl::tstb(frame);
	tick(0x5C3A, 24);
	if (frame)
	{
		tick(0x5CBE, 12);                              // BNE
		rts();
		return;
	}
	tick(0x5C3C, 12);
	T.r[2] = 0x00E0;
	fl::mov(T.r[2]);
	tick(0x5C40, 18);
	T.psw = (uint8_t)((T.psw & 0x10) | (T.r[2] & 0xEF)); // MTPS R2: interrupts off
	T.check_irqs = true;
	tick(0x5C42, 24);
	push(0x000D);
	const uint16_t arg = T.r[6];
	fl::mov(0x000D);
	tick(0x5C46, 30);
	jsr(0x5C4A, 0x8EA2);                               // send_sound_command(0x0D)
	jsr(0x5C4E, 0x8ED2);                               // read_sound_response
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0x5C50, 12);
	jsr(0x5C54, 0x8F62);
	T.r[2] = 0;
	fl::clr();
	tick(0x5C56, 12);
	T.psw = (uint8_t)(T.psw & 0x10);                   // MTPS #0
	T.check_irqs = true;
	tick(0x5C5A, 30);
	fl::tst(T.r[1]);
	tick(0x5C5C, 12);                                  // TST R1
	if (!T.r[1])
	{
		tick(0x5C5E, 12);                              // BNE
		const bool left = peekb(0x01DA) != 0;
		fl::tstb(peekb(0x01DA));
		tick(0x5C62, 24);                              // TSTB @#0x01DA
		if (!left)
		{
			tick(0x5C64, 12);
			pokeb(0x01DA, 0xFF);
			fl::movb(0xFF);
			tick(0x5C6A, 33);
		}
		else
			tick(0x5C6A, 12);                          // BNE
		jsr(0x5C6E, 0x9152);                           // next_random
		for (;;)
		{
			T.r[1] = T.r[0];
			fl::mov(T.r[1]);
			tick(0x5C70, 12);
			T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x0007));
			tick(0x5C74, 18);
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0x5C76, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x5C78, 12);
			const bool fresh = (peekb(0x01DA) & rb((uint16_t)(0xFC7C + T.r[0]))) != 0;
			fl::logicb((uint8_t)(peekb((uint16_t)(T.r[0] + 0xFC7C)) & peekb(0x01DA)));
			tick(0x5C7E, 39);                          // BITB 0xFC7C(R0),@#0x01DA
			if (fresh)
			{
				tick(0x5C86, 12);                      // BNE
				break;
			}
			tick(0x5C80, 12);
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0x5C82, 12);
			T.r[0] = fl::inc(T.r[0]);
			tick(0x5C84, 12);
			tick(0x5C6E, 12);                          // BR
		}
		T.r[0] = T.r[1];
		tick(0x5C88, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x5C8A, 12);
		pokeb(0x01DA, fl::logicb((uint8_t)(peekb(0x01DA) & ~rb((uint16_t)(0xFC7C + T.r[0])))));
		tick(0x5C90, 42);
		T.r[1] = sext8(peekb((uint16_t)(0x5B04 + T.r[1])));
		fl::movb((uint8_t)T.r[1]);
		tick(0x5C94, 27);
		T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x00FF));
		tick(0x5C98, 18);
		poke(arg, T.r[1]);
		fl::mov(T.r[1]);
		tick(0x5C9A, 21);
		jsr(0x5C9E, 0x8E78);                           // queue_sound(tune)
		const bool started = peekb(0x01E7) != 0;
		fl::tstb(peekb(0x01E7));
		tick(0x5CA2, 24);                              // TSTB @#0x01E7
		if (started)
			tick(0x5CBC, 12);                          // BNE
		else
		{
			tick(0x5CA4, 12);
			const bool neg = (int16_t)peek(0x0B78) < 0;
			fl::tst(peek(0x0B78));
			tick(0x5CA8, 24);                          // TST @#0x0B78
			if (neg)
			{
				tick(0x5CAA, 12);                      // BGE
				poke(arg, 0x0040);
				fl::mov(0x0040);
				tick(0x5CAE, 27);
				push(0x0005);
				fl::mov(0x0005);
				tick(0x5CB2, 30);
				jsr(0x5CB6, 0xC98E);
				T.r[6] = (uint16_t)(T.r[6] + 2);
				fl::tst(peek((uint16_t)(T.r[6] - 2)));
				tick(0x5CB8, 18);                      // TST (SP)+
			}
			else
				tick(0x5CB8, 12);
			pokeb(0x01E7, fl::incb(peekb(0x01E7)));
			tick(0x5CBC, 27);
		}
	}
	else
		tick(0x5CBC, 12);                              // BNE
	T.r[6] = (uint16_t)(T.r[6] + 2);
	fl::tst(peek(arg));
	tick(0x5CBE, 18);                                  // TST (SP)+
	rts();
}

// ---------------------------------------------------------------- objects

// A5:4984  draw_text_block_3x3(int kind, int row, int col): draw a 3 x 3 block of text tiles
// (table 0x495C: kind 0, or any other kind as 9; colour 0x0188) at row * 0x180 plus the
// column offset 0x4980[col].  A non-zero kind is written back as 9 into the argument.
uint16_t draw_text_block_3x3(const Args &a)
{
	c_save(3, 0x4988);
	jsr(0x498C, 0x8068);                               // select_video_bank0
	T.r[1] = a.u(2);
	fl::mov(T.r[1]);
	tick(0x4990, 27);
	T.r[1] = fl::asl(T.r[1]);
	tick(0x4992, 12);
	push(a.u(1));
	fl::mov(peek(T.r[6]));
	tick(0x4996, 39);
	push(0x0180);
	fl::mov(0x0180);
	tick(0x499A, 30);
	jsr(0x499E, 0x80D8);                               // mul16
	T.r[0] = fl::add(T.r[0], peek((uint16_t)(0x4980 + T.r[1])));
	tick(0x49A2, 27);
	poke(0x0186, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x49A6, 27);
	poke(0x0186, fl::add(peek(0x0186), 0x1E80));
	tick(0x49AC, 33);
	fl::tst(peek((uint16_t)(T.r[6] + 0x10)));
	tick(0x49B0, 27);                                  // TST 0x10(SP)
	if (a.u(0))
	{
		tick(0x49B2, 12);
		poke((uint16_t)(a.sp + 2), 0x0009);
		fl::mov(0x0009);
		tick(0x49B8, 36);
	}
	else
		tick(0x49B8, 12);                              // BEQ
	T.r[3] = a.u(0);
	fl::mov(T.r[3]);
	tick(0x49BC, 27);
	T.r[3] = fl::add(T.r[3], 6);
	tick(0x49C0, 18);
	T.r[2] = a.u(0);
	fl::mov(T.r[2]);
	tick(0x49C4, 27);
	tick(0x4A02, 12);                                  // BR
	for (;;)
	{
		fl::cmp(T.r[2], T.r[3]);
		tick(0x4A04, 12);                              // CMP R2,R3
		if ((int16_t)T.r[2] > (int16_t)T.r[3])
		{
			tick(0x4A06, 12);                          // BLE
			break;
		}
		tick(0x49C6, 12);
		T.r[0] = peek(0x0186);
		fl::mov(T.r[0]);
		tick(0x49CA, 24);
		uint16_t pc = 0x49CA;
		for (int j = 0; j < 3; j++)
		{
			T.r[1] = T.r[2];
			tick((uint16_t)(pc + 2), 12);
			T.r[1] = (uint16_t)(T.r[1] << 1);
			tick((uint16_t)(pc + 4), 12);
			T.r[4] = peek((uint16_t)(0x495C + 2 * j + T.r[1]));
			tick((uint16_t)(pc + 8), 27);
			T.r[4] |= peek(0x0188);
			tick((uint16_t)(pc + 12), 24);
			poke((uint16_t)(T.r[0] + 2 * j), T.r[4]);
			pc = (uint16_t)(pc + (j ? 16 : 14));       // MOV R4,(R0) is one word shorter
			tick(pc, j ? 30 : 21);
		}
		poke(0x0186, (uint16_t)(peek(0x0186) + 0x0080));
		tick(0x49FE, 33);
		T.r[2] = fl::add(T.r[2], 3);
		tick(0x4A02, 18);
	}
	T.r[6] = (uint16_t)(T.r[6] + 4);
	tick(0x4A08, 24);                                  // CMP (SP)+,(SP)+
	c_restore(3);
	return T.r[0];
}

// A5:51EE  slope_height(int t): 0x02E6 minus the height table entry 0x6AC8 (bank B 0x2C00)
// for the skater direction 0x031D and the terrain t: low bits 0 use the direction alone, 7
// the row (bits 3-5) of a 16-entry block at 0x220, others the row table at 0x79E8.  Bank B
// ends as 0x0C00.
uint16_t slope_height(const Args &a)
{
	c_save(1, 0x51F2);
	T.r[0] = 0x2C00;
	fl::mov(T.r[0]);
	tick(0x51F6, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x51FA, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x51FE, 27);
	T.r[0] = sext8(peekb(0x031D));
	fl::movb((uint8_t)T.r[0]);
	tick(0x5202, 24);
	T.r[2] = a.u(0);
	fl::mov(T.r[2]);
	tick(0x5206, 27);
	fl::logic((uint16_t)(0x0007 & T.r[2]));
	tick(0x520A, 18);                                  // BIT #7,R2
	if (T.r[2] & 7)
	{
		tick(0x520C, 12);
		T.r[1] = 0x0007;
		fl::mov(T.r[1]);
		tick(0x5210, 18);
		T.r[1] = fl::logic((uint16_t)(T.r[1] & (uint16_t)~T.r[2]));
		tick(0x5212, 12);
		if (T.r[1])
		{
			tick(0x5214, 12);                          // BEQ
			T.r[1] = T.r[2];
			fl::mov(T.r[1]);
			tick(0x5216, 12);
			T.r[1] = fl::asr(T.r[1]);
			tick(0x5218, 12);
			T.r[1] = fl::asr(T.r[1]);
			tick(0x521A, 12);
			T.r[1] = fl::asr(T.r[1]);
			tick(0x521C, 12);
			T.r[1] = fl::asl(T.r[1]);
			tick(0x521E, 12);
			T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x0007));
			tick(0x5222, 18);
			T.r[2] = fl::asl(T.r[2]);
			tick(0x5224, 12);
			T.r[2] = fl::asl(T.r[2]);
			tick(0x5226, 12);
			T.r[2] = fl::asl(T.r[2]);
			tick(0x5228, 12);
			T.r[2] = fl::asl(T.r[2]);
			tick(0x522A, 12);
			T.r[2] = fl::add(T.r[2], peek((uint16_t)(0x79E8 + T.r[1])));
			tick(0x522E, 27);
			T.r[2] = fl::add(T.r[2], T.r[0]);
			tick(0x5230, 12);
			T.r[0] = T.r[2];
			fl::mov(T.r[0]);
			tick(0x5232, 12);
			tick(0x5244, 12);                          // BR
		}
		else
		{
			tick(0x5234, 12);
			T.r[1] = T.r[2];
			fl::mov(T.r[1]);
			tick(0x5236, 12);
			T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x0038));
			tick(0x523A, 18);
			T.r[1] = fl::asl(T.r[1]);
			tick(0x523C, 12);
			T.r[1] = fl::add(T.r[1], T.r[0]);
			tick(0x523E, 12);
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0x5240, 12);
			T.r[0] = fl::add(T.r[0], 0x0220);
			tick(0x5244, 18);
		}
	}
	else
		tick(0x5244, 12);                              // BEQ
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0x5246, 12);
	T.r[1] = fl::asl(T.r[1]);
	tick(0x5248, 12);
	T.r[0] = peek(0x02E6);
	fl::mov(T.r[0]);
	tick(0x524C, 24);
	T.r[0] = fl::sub(T.r[0], peek((uint16_t)(0x6AC8 + T.r[1])));
	tick(0x5250, 27);
	T.r[1] = 0x0C00;
	fl::mov(T.r[1]);
	tick(0x5254, 18);
	poke(0x01D4, T.r[1]);
	fl::mov(T.r[1]);
	tick(0x5258, 27);
	poke(0x1402, T.r[1]);
	fl::mov(T.r[1]);
	tick(0x525C, 27);
	c_restore(1);
	return T.r[0];
}

// A7:4098 (assembly): move objects 0-3 by their velocities (x, y, height).  The fraction
// bytes (0x07FC/0x0800/0x0804 += 0x07F0/0x07F4/0x07F8) are added, but their carry is lost
// (SWAB clears it before the ADC), as in the original; g_fix_subpixel_carry adds it.
void move_objects_0_3()
{
	// fraction velocity, fraction, position (words), velocity (words) of x, y and height
	static const uint16_t parts[3][4] = {
		{0x07F0, 0x07FC, 0x06F0, 0x04F0},
		{0x07F4, 0x0800, 0x0730, 0x0570},
		{0x07F8, 0x0804, 0x06B0, 0x05B0},
	};
	// the condition codes are kept instruction by instruction: this runs deep in the frame's
	// work, where an interrupt pushes them onto the stack
	T.r[2] = 3;
	fl::mov(T.r[2]);
	tick(0x409C, 18);
	for (;;)
	{
		T.r[3] = T.r[2];
		fl::mov(T.r[3]);
		tick(0x409E, 12);
		T.r[3] = asl(T.r[3]);
		tick(0x40A0, 12);
		uint16_t pc = 0x40A0;
		for (const auto &p : parts)
		{
			T.r[0] = 0;
			fl::clr();
			tick((uint16_t)(pc + 2), 12);
			T.r[0] = peekb((uint16_t)(p[0] + T.r[2]));
			fl::movb((uint8_t)T.r[0]);
			tick((uint16_t)(pc + 6), 27);              // BISB
			T.r[0] = swab(T.r[0]);
			tick((uint16_t)(pc + 8), 12);
			T.r[1] = 0;
			fl::clr();
			tick((uint16_t)(pc + 10), 12);
			T.r[1] = peekb((uint16_t)(p[1] + T.r[2]));
			fl::movb((uint8_t)T.r[1]);
			tick((uint16_t)(pc + 14), 27);             // BISB
			T.r[1] = swab(T.r[1]);
			tick((uint16_t)(pc + 16), 12);
			const uint16_t carry = (uint16_t)(((uint32_t)T.r[0] + T.r[1]) >> 16);
			T.r[0] = fl::add(T.r[0], T.r[1]);
			tick((uint16_t)(pc + 18), 12);
			T.r[0] = swab(T.r[0]);
			tick((uint16_t)(pc + 20), 12);
			pokeb((uint16_t)(p[1] + T.r[2]), (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick((uint16_t)(pc + 24), 30);
			const uint16_t a = (uint16_t)(p[2] + T.r[3]);
			if (g_fix_subpixel_carry) poke(a, (uint16_t)(peek(a) + carry));
			poke(a, fl::adc(peek(a)));                 // the carry is clear (SWAB): adds nothing
			tick((uint16_t)(pc + 28), 30);
			poke(a, fl::add(peek(a), peek((uint16_t)(p[3] + T.r[3]))));
			pc = (uint16_t)(pc + 34);
			tick(pc, 45);
		}
		T.r[2] = fl::dec(T.r[2]);
		tick(0x4108, 12);
		if ((int16_t)T.r[2] < 0)
		{
			tick(0x410A, 12);                          // BPL
			break;
		}
		tick(0x409C, 12);
	}
	rts();
}

// A7:410C  project_speed(): the skater's speed 0x01B4 from the velocities projected on the
// slope: (2 * vy * 0x02E4 + vx * 0x02E2) * 3/4 + vz * 0x02E6 (F:80FA products, the long at
// 0x0204), taken >> 12.  0x0204/0x0206 keep the partial sums as in the original.
uint16_t project_speed(const Args &)
{
	c_save(1, 0x4110);
	push(peek(0x02E4));
	fl::mov(peek(T.r[6]));
	tick(0x4114, 36);
	push(peek(0x0570));
	fl::mov(peek(T.r[6]));
	tick(0x4118, 36);
	jsr(0x411C, 0x80FA);                               // mul32
	T.r[1] = peek(0x0204);
	fl::mov(T.r[1]);
	tick(0x4120, 24);
	T.r[2] = peek(0x0206);
	fl::mov(T.r[2]);
	tick(0x4124, 24);
	uint16_t c = (uint16_t)(T.r[1] >> 15);
	T.r[1] = fl::asl(T.r[1]);
	tick(0x4126, 12);                                  // ASL R1
	T.r[2] = (uint16_t)((T.r[2] << 1) | c);
	tick(0x4128, 12);                                  // ROL R2
	poke((uint16_t)(T.r[6] + 2), peek(0x02E2));
	fl::mov(peek((uint16_t)(T.r[6] + 2)));
	tick(0x412E, 42);
	poke(T.r[6], peek(0x04F0));
	fl::mov(peek(T.r[6]));
	tick(0x4132, 33);
	jsr(0x4136, 0x80FA);
	uint32_t s = (uint32_t)T.r[1] + peek(0x0204);
	T.r[1] = (uint16_t)s;
	fl::add((uint16_t)(T.r[1] - peek(0x0204)), peek(0x0204));
	tick(0x413A, 24);
	T.r[2] = (uint16_t)(T.r[2] + (s >> 16));
	tick(0x413C, 12);                                  // ADC R2
	T.r[2] = fl::add(T.r[2], peek(0x0206));
	tick(0x4140, 24);
	poke(0x0204, T.r[1]);
	fl::mov(T.r[1]);
	tick(0x4144, 27);
	poke(0x0206, T.r[2]);
	fl::mov(T.r[2]);
	tick(0x4148, 27);
	c = (uint16_t)(T.r[1] >> 15);
	T.r[1] = fl::asl(T.r[1]);
	tick(0x414A, 12);
	T.r[2] = (uint16_t)((T.r[2] << 1) | c);
	tick(0x414C, 12);
	s = (uint32_t)T.r[1] + peek(0x0204);
	T.r[1] = (uint16_t)s;
	fl::add((uint16_t)(T.r[1] - peek(0x0204)), peek(0x0204));
	tick(0x4150, 24);
	T.r[2] = (uint16_t)(T.r[2] + (s >> 16));
	tick(0x4152, 12);
	T.r[2] = fl::add(T.r[2], peek(0x0206));
	tick(0x4156, 24);
	for (uint16_t pc = 0x4156; pc < 0x415E; pc = (uint16_t)(pc + 4))
	{
		c = T.r[2] & 1;
		T.r[2] = asr(T.r[2], 1);
		tick((uint16_t)(pc + 2), 12);                  // ASR R2
		T.r[1] = (uint16_t)((T.r[1] >> 1) | (c << 15));
		tick((uint16_t)(pc + 4), 12);                  // ROR R1
	}
	poke((uint16_t)(T.r[6] + 2), peek(0x02E6));
	fl::mov(peek((uint16_t)(T.r[6] + 2)));
	tick(0x4164, 42);
	poke(T.r[6], peek(0x05B2));
	fl::mov(peek(T.r[6]));
	tick(0x4168, 33);
	jsr(0x416C, 0x80FA);
	s = (uint32_t)T.r[1] + peek(0x0204);
	T.r[1] = (uint16_t)s;
	fl::add((uint16_t)(T.r[1] - peek(0x0204)), peek(0x0204));
	tick(0x4170, 24);
	T.r[2] = (uint16_t)(T.r[2] + (s >> 16));
	tick(0x4172, 12);
	poke(0x0206, fl::add(peek(0x0206), T.r[2]));
	tick(0x4176, 27);
	const uint16_t v = pop();                          // MOV (SP)+,(SP)+
	poke(T.r[6], v);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	fl::mov(peek((uint16_t)(T.r[6] - 2)));
	tick(0x4178, 27);
	T.r[2] = sext8(peekb(0x0207));
	fl::movb((uint8_t)T.r[2]);
	tick(0x417C, 24);
	T.r[1] &= 0xFF00;
	fl::clr();
	tick(0x417E, 12);                                  // CLRB R1
	T.r[1] |= peekb(0x0206);
	fl::logicb((uint8_t)T.r[1]);
	tick(0x4182, 24);
	T.r[1] = (uint16_t)((T.r[1] >> 8) | (T.r[1] << 8));
	tick(0x4184, 12);                                  // SWAB R1
	for (uint16_t pc = 0x4184; pc < 0x4194; pc = (uint16_t)(pc + 4))
	{
		c = T.r[2] & 1;
		T.r[2] = asr(T.r[2], 1);
		tick((uint16_t)(pc + 2), 12);
		T.r[1] = (uint16_t)((T.r[1] >> 1) | (c << 15));
		tick((uint16_t)(pc + 4), 12);
	}
	poke(0x01B4, T.r[1]);
	tick(0x4198, 27);
	c_restore(1);
	return T.r[0];
}

// A7:4F98: direction (0-15) from object 7 to the skater (object 0, its height added to y),
// with the frame counter jittering the x difference
uint16_t direction_from_object7(const Args &)
{
	c_save(2, 0x4F9C);
	T.r[2] = peek(0x06F0);
	fl::mov(T.r[2]);
	tick(0x4FA0, 24);
	T.r[2] = fl::sub(T.r[2], peek(0x06FE));
	tick(0x4FA4, 24);
	T.r[1] = T.r[2];
	fl::mov(T.r[1]);
	tick(0x4FA6, 12);
	T.r[1] = fl::add(T.r[1], 0x0500);
	tick(0x4FAA, 18);
	T.r[0] = peek(0x01C4);
	fl::mov(T.r[0]);
	tick(0x4FAE, 24);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x01FF));
	tick(0x4FB2, 18);
	T.r[1] = fl::add(T.r[1], T.r[0]);
	tick(0x4FB4, 12);
	T.r[3] = peek(0x0730);
	fl::mov(T.r[3]);
	tick(0x4FB8, 24);
	T.r[3] = fl::sub(T.r[3], peek(0x073E));
	tick(0x4FBC, 24);
	T.r[3] = fl::add(T.r[3], peek(0x06B0));
	tick(0x4FC0, 24);
	T.r[0] = T.r[3];
	fl::mov(T.r[0]);
	tick(0x4FC2, 12);
	T.r[0] = fl::add(T.r[0], 0x0500);
	tick(0x4FC6, 18);
	fl::tst(T.r[1]);
	tick(0x4FC8, 12);                                  // TST R1
	if (T.r[1] & 0x8000)
	{
		tick(0x4FCE, 12);                              // BLT
		T.r[2] = T.r[1];
		fl::mov(T.r[2]);
		tick(0x4FD0, 12);
		T.r[2] = (uint16_t)-T.r[2];
		fl::neg((uint16_t)(-T.r[2]));
		tick(0x4FD2, 12);
	}
	else
	{
		tick(0x4FCA, 12);
		T.r[2] = T.r[1];
		fl::mov(T.r[2]);
		tick(0x4FCC, 12);
		tick(0x4FD2, 12);                              // BR
	}
	fl::cmp(T.r[2], 0x0200);
	tick(0x4FD6, 18);                                  // CMP R2,#0x0200
	bool negative;                                     // the flags tested by the BGE at 4FF6
	if ((int16_t)T.r[2] < 0x0200)
	{
		tick(0x4FD8, 12);                              // BGE
		T.r[2] = 0x0004;
		fl::mov(T.r[2]);
		tick(0x4FDC, 18);
		negative = (T.r[0] & 0x8000) != 0;
		fl::tst(T.r[0]);
		tick(0x4FDE, 12);                              // TST R0
		tick(0x4FF6, 12);                              // BR
	}
	else
	{
		tick(0x4FE0, 12);                              // BGE
		fl::tst(T.r[0]);
		tick(0x4FE2, 12);                              // TST R0
		if (T.r[0] & 0x8000)
		{
			tick(0x4FE8, 12);                          // BLT
			T.r[3] = T.r[0];
			fl::mov(T.r[3]);
			tick(0x4FEA, 12);
			T.r[3] = (uint16_t)-T.r[3];
			fl::neg((uint16_t)(-T.r[3]));
			tick(0x4FEC, 12);
		}
		else
		{
			tick(0x4FE4, 12);
			T.r[3] = T.r[0];
			fl::mov(T.r[3]);
			tick(0x4FE6, 12);
			tick(0x4FEC, 12);                          // BR
		}
		fl::cmp(T.r[3], 0x0200);
		tick(0x4FF0, 18);                              // CMP R3,#0x0200
		if ((int16_t)T.r[3] >= 0x0200)
		{
			tick(0x4FFE, 12);                          // BGE
			T.r[2] = 0x0002;
			fl::mov(T.r[2]);
			tick(0x5002, 18);
			fl::tst(T.r[1]);
			tick(0x5004, 12);                          // TST R1
			if (T.r[1] & 0x8000)
			{
				tick(0x5006, 12);
				T.r[2] = fl::logic((uint16_t)(T.r[2] | 0x0004));
				tick(0x500A, 18);
			}
			else
				tick(0x500A, 12);                      // BGE
			fl::tst(T.r[0]);
			tick(0x500C, 12);                          // TST R0
			if (T.r[0] & 0x8000)
			{
				tick(0x500E, 12);
				T.r[2] = (uint16_t)-T.r[2];
				fl::neg((uint16_t)(-T.r[2]));
				tick(0x5010, 12);
				T.r[2] = fl::add(T.r[2], 0x0010);
				tick(0x5014, 18);
			}
			else
				tick(0x5014, 12);                      // BGE
			T.r[0] = T.r[2];
			tick(0x5016, 12);
			c_restore(2);
			return T.r[0];
		}
		tick(0x4FF2, 12);
		T.r[2] = 0;
		fl::clr();
		tick(0x4FF4, 12);
		negative = (T.r[1] & 0x8000) != 0;
		fl::tst(T.r[1]);
		tick(0x4FF6, 12);                              // TST R1
	}
	if (negative)
	{
		tick(0x4FF8, 12);                              // BGE
		T.r[2] = fl::logic((uint16_t)(T.r[2] | 0x0008));
		tick(0x4FFC, 18);
		tick(0x5014, 12);                              // BR
	}
	else
		tick(0x5014, 12);
	T.r[0] = T.r[2];
	tick(0x5016, 12);
	c_restore(2);
	return T.r[0];
}

// A7:4ECC  hold_object(int i): object i is parked: its animation, byte 0x0770 and velocities
// go to the template 0x0E5E-0x0E69 (respawned later by A7:4F42), and it stops (0x04B0 bit
// 0x40).  Object 6 instead gets the animation 0x50FA in the template slot of object 5.
uint16_t hold_object(const Args &a)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x4ECE, 24);
	T.r[1] = a.u(0);
	fl::mov(T.r[1]);
	tick(0x4ED2, 27);
	fl::cmp(T.r[1], 0x0006);
	tick(0x4ED6, 18);                                  // CMP R1,#6
	if (T.r[1] == 6)
	{
		tick(0x4ED8, 12);
		poke(0x03BA, 0x50FA);
		fl::mov(0x50FA);
		tick(0x4EDE, 33);
		pokeb(0x0495, fl::logicb((uint8_t)(peekb(0x0495) | 0x18)));
		tick(0x4EE4, 33);
		poke(0x04FA, 0);
		fl::clr();
		tick(0x4EE8, 27);
		poke(0x057A, 0);
		fl::clr();
		tick(0x4EEC, 27);
		poke(0x043A, 0);
		fl::clr();
		tick(0x4EF0, 27);
		tick(0x4F24, 12);                              // BR
	}
	else
	{
		tick(0x4EF2, 12);                              // BNE
		pokeb(0x0E6A, (uint8_t)T.r[1]);
		fl::movb((uint8_t)T.r[1]);
		tick(0x4EF6, 27);
		pokeb(0x0E68, 0xB4);
		fl::movb(0xB4);
		tick(0x4EFC, 33);
		T.r[0] = T.r[1];
		fl::mov(T.r[0]);
		tick(0x4EFE, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x4F00, 12);
		poke(0x0E66, peek((uint16_t)(0x0430 + T.r[0])));
		fl::mov(peek(0x0E66));
		tick(0x4F06, 42);
		pokeb(0x0E69, peekb((uint16_t)(0x0770 + T.r[1])));
		fl::movb(peekb(0x0E69));
		tick(0x4F0C, 42);
		poke(0x0E5E, peek((uint16_t)(0x04F0 + T.r[0])));
		fl::mov(peek(0x0E5E));
		tick(0x4F12, 42);
		poke(0x0E60, peek((uint16_t)(0x0570 + T.r[0])));
		fl::mov(peek(0x0E60));
		tick(0x4F18, 42);
		poke(0x0E62, peek((uint16_t)(0x03B0 + T.r[0])));
		fl::mov(peek(0x0E62));
		tick(0x4F1E, 42);
		poke(0x0E64, peek((uint16_t)(0x03F0 + T.r[0])));
		fl::mov(peek(0x0E64));
		tick(0x4F24, 42);
	}
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0x4F26, 12);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x4F28, 12);
	poke((uint16_t)(0x0430 + T.r[0]), 0);
	fl::clr();
	tick(0x4F2C, 30);
	pokeb((uint16_t)(0x04B0 + T.r[1]), fl::logicb((uint8_t)(peekb((uint16_t)(0x04B0 + T.r[1])) | 0x40)));
	tick(0x4F32, 36);
	poke((uint16_t)(0x03F0 + T.r[0]), 0);
	fl::clr();
	tick(0x4F36, 30);
	poke((uint16_t)(0x04F0 + T.r[0]), 0);
	fl::clr();
	tick(0x4F3A, 30);
	poke((uint16_t)(0x0570 + T.r[0]), 0);
	fl::clr();
	tick(0x4F3E, 30);
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x4F40, 18);
	rts();
	return T.r[0];
}

// ---------------------------------------------------------------- messages
// Up to 8 message boxes are shown in the text layer: 0x0336 is the index of the last one,
// 0x0338/0x0348 hold their expiry time (compared with the counter 0x036A) and their box
// descriptor (word: text layer address, bytes: rows, columns).  Pending messages wait in
// 0x0362 (index 0x0358) with their durations in 0x035A.

// A7:4D30  message_box(int erase, box): draw the frame of box (tiles 0x110-0x117, inside
// 0x87, colour 0xE000) or erase it.  0x036C keeps a stack of flags (bit 0: the box address
// is odd, for which F:EAD4 draws/erases the text at 0x5DF6); erasing the last of those in
// Skate City also redraws the number 0x0244/0x0246 at 0x3746.  Bank B is 0x2800 meanwhile.
// Locals: the row count (low byte of the first word), the saved banks, the three tiles of
// an inner row.
uint16_t message_box(const Args &a)
{
	c_save(4, 0x4D34);
	T.r[6] = (uint16_t)(T.r[6] - 0x000C);
	const uint16_t sp = T.r[6];
	fl::sub((uint16_t)(T.r[6] + 0x000C), 0x000C);
	tick(0x4D38, 18);
	poke((uint16_t)(sp + 4), peek(0x01D2));
	fl::mov(peek((uint16_t)(T.r[6] + 4)));
	tick(0x4D3E, 42);
	poke((uint16_t)(sp + 2), peek(0x01D4));
	fl::mov(peek((uint16_t)(T.r[6] + 2)));
	tick(0x4D44, 42);
	T.r[0] = 0x2800;
	fl::mov(T.r[0]);
	tick(0x4D48, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x4D4C, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x4D50, 27);
	jsr(0x4D54, 0x8068);                               // select_video_bank0
	poke(0x0188, 0xE000);
	fl::mov(0xE000);
	tick(0x4D5A, 33);
	T.r[0] = a.u(1);
	fl::mov(T.r[0]);
	tick(0x4D5E, 27);
	T.r[1] = peek(T.r[0]);
	fl::mov(T.r[1]);
	tick(0x4D60, 18);
	T.r[1] = fl::add(T.r[1], 0x2000);
	tick(0x4D64, 18);
	T.r[1] = fl::logic((uint16_t)(T.r[1] & 0xFFFE));
	tick(0x4D68, 18);
	pokeb(sp, peekb((uint16_t)(T.r[0] + 2)));
	fl::movb(peekb(T.r[6]));
	tick(0x4D6C, 36);
	T.r[3] = sext8(peekb((uint16_t)(T.r[0] + 3)));
	fl::movb((uint8_t)T.r[3]);
	tick(0x4D70, 27);
	T.r[5] = 0;
	fl::clr();
	tick(0x4D72, 12);
	T.r[5] |= (uint8_t)T.r[3];
	fl::logicb((uint8_t)T.r[5]);
	tick(0x4D74, 12);
	T.r[5] = (uint16_t)(T.r[5] << 1);                  // the row width in bytes
	tick(0x4D76, 12);
	fl::tst(peek((uint16_t)(T.r[6] + 0x1A)));
	tick(0x4D7A, 27);                                  // TST 0x1A(SP)
	if (a.u(0))
	{
		tick(0x4D7C, 12);
		T.r[2] = 0;
		fl::clr();
		tick(0x4D7E, 12);
		T.r[2] = peekb(sp);
		fl::logicb((uint8_t)T.r[2]);
		tick(0x4D80, 18);
		fl::tst(T.r[2]);
		tick(0x4D82, 12);                              // TST R2
		if ((int16_t)T.r[2] > 0)
		{
			tick(0x4D84, 12);
			for (;;)
			{
				T.r[0] = 0;
				fl::clr();
				tick(0x4D86, 12);
				T.r[0] = (uint8_t)T.r[3];
				fl::logicb((uint8_t)T.r[0]);
				tick(0x4D88, 12);
				fl::tst(T.r[0]);
				tick(0x4D8A, 12);                      // TST R0
				if ((int16_t)T.r[0] > 0)
				{
					tick(0x4D8C, 12);
					for (;;)
					{
						poke(T.r[1], 0);
						T.r[1] = (uint16_t)(T.r[1] + 2);
						fl::clr();
						tick(0x4D8E, 21);
						if (!--T.r[0])
						{
							tick(0x4D90, 18);          // SOB
							break;
						}
						tick(0x4D8C, 18);
					}
				}
				else
					tick(0x4D90, 12);                  // BLE
				T.r[0] = T.r[1];
				fl::mov(T.r[0]);
				tick(0x4D92, 12);
				T.r[0] = fl::sub(T.r[0], T.r[5]);
				tick(0x4D94, 12);
				T.r[1] = T.r[0];
				fl::mov(T.r[1]);
				tick(0x4D96, 12);
				T.r[1] = fl::add(T.r[1], 0x0080);
				tick(0x4D9A, 18);
				if (!--T.r[2])
				{
					tick(0x4D9C, 18);                  // SOB
					break;
				}
				tick(0x4D84, 18);
			}
		}
		else
			tick(0x4D9C, 12);                          // BLE
		const bool last = peekb(0x036C) == 1;
		fl::cmpb(peekb(0x036C), 0x01);
		tick(0x4DA2, 30);                              // CMPB @#0x036C,#1
		if (last)
		{
			tick(0x4DA4, 12);
			const bool other = peekb(0x0F59) != 0;
			fl::tstb(peekb(0x0F59));
			tick(0x4DA8, 24);                          // TSTB @#0x0F59
			if (other)
				tick(0x4DDA, 12);                      // BNE
			else
			{
				tick(0x4DAA, 12);
				poke(0x0188, 0xA000);
				fl::mov(0xA000);
				tick(0x4DB0, 33);
				push(0x5DF6);
				fl::mov(0x5DF6);
				tick(0x4DB4, 30);
				push(0);
				fl::clr();
				tick(0x4DB6, 24);
				jsr(0x4DBA, 0xEAD4);                   // draw_tile_block(0, 0x5DF6)
				poke(0x0186, 0x3746);
				fl::mov(0x3746);
				tick(0x4DC0, 33);
				poke(T.r[6], peek(0x0244));
				fl::mov(peek(T.r[6]));
				tick(0x4DC4, 33);
				push(peek(0x0246));
				fl::mov(peek(T.r[6]));
				tick(0x4DC8, 36);
				push(0x0008);
				fl::mov(0x0008);
				tick(0x4DCC, 30);
				push(0);
				fl::clr();
				tick(0x4DCE, 24);
				push(0x0001);
				fl::mov(0x0001);
				tick(0x4DD2, 30);
				jsr(0x4DD6, 0xBA96);                   // print the number
				T.r[6] = fl::add(T.r[6], 0x000C);
				tick(0x4DDA, 18);
			}
		}
		else
			tick(0x4DDA, 12);                          // BNE
		T.r[0] = 0;
		fl::clr();
		tick(0x4DDC, 12);
		T.r[0] = peekb(0x036C);
		fl::logicb((uint8_t)T.r[0]);
		tick(0x4DE0, 24);
		T.r[0] = fl::asr(T.r[0]);
		tick(0x4DE2, 12);
		pokeb(0x036C, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x4DE6, 27);
		tick(0x4EAE, 12);                              // BR
	}
	else
	{
		tick(0x4DE8, 12);                              // BEQ
		T.r[2] = 0;
		fl::clr();
		tick(0x4DEA, 12);
		T.r[2] = peekb(0x036C);
		fl::logicb((uint8_t)T.r[2]);
		tick(0x4DEE, 24);
		T.r[2] = fl::asl(T.r[2]);
		tick(0x4DF0, 12);
		const bool text = (peek(T.r[0]) & 1) != 0;
		fl::logic((uint16_t)(0x0001 & peek(T.r[0])));
		tick(0x4DF4, 24);                              // BIT #1,(R0)
		if (text)
		{
			tick(0x4DF6, 12);
			pokeb(0x036C, (uint8_t)T.r[2]);
			fl::movb((uint8_t)T.r[2]);
			tick(0x4DFA, 27);
			pokeb(0x036C, fl::logicb((uint8_t)(peekb(0x036C) | 1)));
			tick(0x4E00, 33);
			push(0x5DF6);
			fl::mov(0x5DF6);
			tick(0x4E04, 30);
			push(0x0001);
			fl::mov(0x0001);
			tick(0x4E08, 30);
			jsr(0x4E0C, 0xEAD4);                       // draw_tile_block(1, 0x5DF6)
			T.r[6] = (uint16_t)(T.r[6] + 4);
			fl::cmp(peek((uint16_t)(T.r[6] - 4)), peek((uint16_t)(T.r[6] - 2)));
			tick(0x4E0E, 24);                          // CMP (SP)+,(SP)+
			tick(0x4E14, 12);                          // BR
		}
		else
		{
			tick(0x4E10, 12);                          // BEQ
			pokeb(0x036C, (uint8_t)T.r[2]);
			fl::movb((uint8_t)T.r[2]);
			tick(0x4E14, 27);
		}
		T.r[0] = peek(0x0188);
		fl::mov(T.r[0]);
		tick(0x4E18, 24);
		// a tile at (R1)+: the colour first, then the tile number added (BIS)
		auto corner = [&](uint16_t tile, uint16_t pc) {
			poke(T.r[1], T.r[0]);
			tick(pc, 21);
			poke(T.r[1], peek(T.r[1]) | tile);
			T.r[1] = (uint16_t)(T.r[1] + 2);
			tick((uint16_t)(pc + 4), 27);
		};
		corner(0x0110, 0x4E1A);
		T.r[4] = T.r[0];
		tick(0x4E20, 12);
		T.r[4] = fl::logic((uint16_t)(T.r[4] | 0x0111));
		tick(0x4E24, 18);
		T.r[2] = 0;
		fl::clr();
		tick(0x4E26, 12);
		T.r[2] = (uint8_t)T.r[3];
		fl::logicb((uint8_t)T.r[2]);
		tick(0x4E28, 12);
		T.r[2] = fl::sub(T.r[2], 2);
		tick(0x4E2C, 18);
		if ((int16_t)T.r[2] > 0)
		{
			tick(0x4E2E, 12);
			for (;;)
			{
				poke(T.r[1], T.r[4]);
				T.r[1] = (uint16_t)(T.r[1] + 2);
				fl::mov(T.r[4]);
				tick(0x4E30, 21);
				if (!--T.r[2])
				{
					tick(0x4E32, 18);                  // SOB
					break;
				}
				tick(0x4E2E, 18);
			}
		}
		else
			tick(0x4E32, 12);                          // BLE
		corner(0x0112, 0x4E34);
		T.r[4] = T.r[1];
		fl::mov(T.r[4]);
		tick(0x4E3A, 12);
		T.r[4] = fl::sub(T.r[4], T.r[5]);
		tick(0x4E3C, 12);
		T.r[1] = T.r[4];
		fl::mov(T.r[1]);
		tick(0x4E3E, 12);
		T.r[1] = fl::add(T.r[1], 0x0080);
		tick(0x4E42, 18);
		poke((uint16_t)(sp + 0x0A), T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4E46, 30);
		poke((uint16_t)(sp + 0x0A), fl::logic((uint16_t)(peek((uint16_t)(sp + 0x0A)) | 0x0113)));
		tick(0x4E4C, 36);
		poke((uint16_t)(sp + 6), T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4E50, 30);
		poke((uint16_t)(sp + 6), fl::logic((uint16_t)(peek((uint16_t)(sp + 6)) | 0x0087)));
		tick(0x4E56, 36);
		poke((uint16_t)(sp + 8), T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4E5A, 30);
		poke((uint16_t)(sp + 8), fl::logic((uint16_t)(peek((uint16_t)(sp + 8)) | 0x0114)));
		tick(0x4E60, 36);
		T.r[4] = 0;
		fl::clr();
		tick(0x4E62, 12);
		T.r[4] = peekb(sp);
		fl::logicb((uint8_t)T.r[4]);
		tick(0x4E64, 18);
		T.r[4] = fl::sub(T.r[4], 2);
		tick(0x4E68, 18);
		if ((int16_t)T.r[4] > 0)
		{
			tick(0x4E6A, 12);
			for (;;)
			{
				poke(T.r[1], peek((uint16_t)(sp + 0x0A)));
				T.r[1] = (uint16_t)(T.r[1] + 2);
				fl::mov(peek((uint16_t)(T.r[6] + 0x0A)));
				tick(0x4E6E, 36);
				T.r[2] = 0;
				fl::clr();
				tick(0x4E70, 12);
				T.r[2] = (uint8_t)T.r[3];
				fl::logicb((uint8_t)T.r[2]);
				tick(0x4E72, 12);
				T.r[2] = fl::sub(T.r[2], 2);
				tick(0x4E76, 18);
				if ((int16_t)T.r[2] > 0)
				{
					tick(0x4E78, 12);
					for (;;)
					{
						poke(T.r[1], peek((uint16_t)(sp + 6)));
						T.r[1] = (uint16_t)(T.r[1] + 2);
						fl::mov(peek((uint16_t)(T.r[6] + 6)));
						tick(0x4E7C, 36);
						if (!--T.r[2])
						{
							tick(0x4E7E, 18);          // SOB
							break;
						}
						tick(0x4E78, 18);
					}
				}
				else
					tick(0x4E7E, 12);                  // BLE
				poke(T.r[1], peek((uint16_t)(sp + 8)));
				T.r[1] = (uint16_t)(T.r[1] + 2);
				fl::mov(peek((uint16_t)(T.r[6] + 8)));
				tick(0x4E82, 36);
				T.r[2] = T.r[1];
				fl::mov(T.r[2]);
				tick(0x4E84, 12);
				T.r[2] = fl::sub(T.r[2], T.r[5]);
				tick(0x4E86, 12);
				T.r[1] = T.r[2];
				fl::mov(T.r[1]);
				tick(0x4E88, 12);
				T.r[1] = fl::add(T.r[1], 0x0080);
				tick(0x4E8C, 18);
				if (!--T.r[4])
				{
					tick(0x4E8E, 18);                  // SOB
					break;
				}
				tick(0x4E6A, 18);
			}
		}
		else
			tick(0x4E8E, 12);                          // BLE
		corner(0x0115, 0x4E90);
		T.r[4] = T.r[0];
		tick(0x4E96, 12);
		T.r[4] = fl::logic((uint16_t)(T.r[4] | 0x0116));
		tick(0x4E9A, 18);
		T.r[3] = fl::logic((uint16_t)(T.r[3] & 0x00FF));
		tick(0x4E9E, 18);
		T.r[3] = fl::sub(T.r[3], 2);
		tick(0x4EA2, 18);
		if ((int16_t)T.r[3] > 0)
		{
			tick(0x4EA4, 12);
			for (;;)
			{
				poke(T.r[1], T.r[4]);
				T.r[1] = (uint16_t)(T.r[1] + 2);
				fl::mov(T.r[4]);
				tick(0x4EA6, 21);
				if (!--T.r[3])
				{
					tick(0x4EA8, 18);                  // SOB
					break;
				}
				tick(0x4EA4, 18);
			}
		}
		else
			tick(0x4EA8, 12);                          // BLE
		corner(0x0117, 0x4EAA);
	}
	poke(0x01D2, peek((uint16_t)(sp + 4)));
	fl::mov(peek(0x01D2));
	tick(0x4EB4, 42);
	poke(0x01D4, peek((uint16_t)(sp + 2)));
	fl::mov(peek(0x01D4));
	tick(0x4EBA, 42);
	poke(0x1400, peek(0x01D2));
	fl::mov(peek(0x01D2));
	tick(0x4EC0, 39);
	poke(0x1402, peek(0x01D4));
	fl::mov(peek(0x01D4));
	tick(0x4EC6, 39);
	T.r[6] = fl::add(T.r[6], 0x000C);
	tick(0x4ECA, 18);
	c_restore(4);
	return T.r[0];
}

// A7:4B84 (assembly): count the message timer 0x036A and erase the messages whose time has
// come (shifting the later ones down).  As in the original, the entry after a removed one
// is skipped in this pass and the loop runs to the old count.  Banks are restored; the flags
// are those of comparing the two saved bank words.
void expire_messages()
{
	T.r[6] = (uint16_t)(T.r[6] - 4);                   // CMP -(SP),-(SP): the saved banks
	const uint16_t sp = T.r[6];
	fl::cmp(peek((uint16_t)(T.r[6] + 2)), peek(T.r[6]));
	tick(0x4B86, 30);
	T.r[5] = sext8(peekb(0x0336));
	fl::movb((uint8_t)T.r[5]);
	tick(0x4B8A, 24);
	if ((int16_t)T.r[5] >= 0)
	{
		tick(0x4B8C, 12);
		poke((uint16_t)(sp + 2), peek(0x01D2));
		fl::mov(peek((uint16_t)(T.r[6] + 2)));
		tick(0x4B92, 42);
		poke(sp, peek(0x01D4));
		fl::mov(peek(T.r[6]));
		tick(0x4B96, 33);
		poke(0x036A, fl::inc(peek(0x036A)));
		tick(0x4B9A, 27);
		T.r[4] = 0;
		fl::clr();
		tick(0x4B9C, 12);
		tick(0x4C0A, 12);                              // BR
		for (;;)
		{
			fl::cmp(T.r[4], T.r[5]);
			tick(0x4C0C, 12);                          // CMP R4,R5
			if ((int16_t)T.r[4] > (int16_t)T.r[5])
			{
				tick(0x4C0E, 12);                      // BLE
				break;
			}
			tick(0x4B9E, 12);
			T.r[0] = T.r[4];
			fl::mov(T.r[0]);
			tick(0x4BA0, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x4BA2, 12);
			const bool due = peek((uint16_t)(0x0338 + T.r[0])) == peek(0x036A);
			fl::cmp(peek((uint16_t)(T.r[0] + 0x0338)), peek(0x036A));
			tick(0x4BA8, 39);
			if (due)
			{
				tick(0x4BAA, 12);
				T.r[1] = 0x2800;
				fl::mov(T.r[1]);
				tick(0x4BAE, 18);
				poke(0x01D4, T.r[1]);
				fl::mov(T.r[1]);
				tick(0x4BB2, 27);
				poke(0x1402, T.r[1]);
				fl::mov(T.r[1]);
				tick(0x4BB6, 27);
				const bool tiles = (int16_t)peek(peek((uint16_t)(0x0348 + T.r[0]))) >= 0x4000;
				fl::cmp(peek(rw((uint16_t)(T.r[0] + 0x0348))), 0x4000);
				tick(0x4BBC, 39);
				if (tiles)
				{
					tick(0x4BBE, 12);
					push(peek(peek((uint16_t)(0x0348 + T.r[0]))));
					fl::mov(peek(T.r[6]));
					tick(0x4BC2, 45);
					push(0x0001);
					fl::mov(0x0001);
					tick(0x4BC6, 30);
					jsr(0x4BCA, 0xEAD4);               // draw_tile_block(1, block)
					tick(0x4BD8, 12);                  // BR
				}
				else
				{
					tick(0x4BCC, 12);                  // BLT
					push(peek((uint16_t)(0x0348 + T.r[0])));
					fl::mov(peek(T.r[6]));
					tick(0x4BD0, 39);
					push(0x0001);
					fl::mov(0x0001);
					tick(0x4BD4, 30);
					jsr(0x4BD8, 0x4D30);               // message_box(erase)
				}
				T.r[0] = sext8(peekb(0x0336));
				fl::movb((uint8_t)T.r[0]);
				tick(0x4BDC, 24);
				T.r[0] = fl::dec(T.r[0]);
				tick(0x4BDE, 12);
				pokeb(0x0336, (uint8_t)T.r[0]);
				fl::movb((uint8_t)T.r[0]);
				tick(0x4BE2, 27);
				T.r[3] = sext8((uint8_t)T.r[0]);
				fl::movb((uint8_t)T.r[3]);
				tick(0x4BE4, 12);
				if ((int16_t)T.r[3] >= 0)
				{
					tick(0x4BE6, 12);
					T.r[2] = T.r[4];
					fl::mov(T.r[2]);
					tick(0x4BE8, 12);
					T.r[2] = fl::dec(T.r[2]);
					tick(0x4BEA, 12);
					tick(0x4C00, 12);                  // BR
					for (;;)
					{
						T.r[2] = fl::inc(T.r[2]);
						tick(0x4C02, 12);
						fl::cmp(T.r[2], T.r[3]);
						tick(0x4C04, 12);              // CMP R2,R3
						if ((int16_t)T.r[2] > (int16_t)T.r[3])
						{
							tick(0x4C06, 12);          // BLE
							break;
						}
						tick(0x4BEC, 12);
						T.r[1] = T.r[2];
						fl::mov(T.r[1]);
						tick(0x4BEE, 12);
						T.r[1] = fl::asl(T.r[1]);
						tick(0x4BF0, 12);
						T.r[0] = T.r[2];
						fl::mov(T.r[0]);
						tick(0x4BF2, 12);
						T.r[0] = fl::asl(T.r[0]);
						tick(0x4BF4, 12);
						poke((uint16_t)(0x0348 + T.r[1]), peek((uint16_t)(0x034A + T.r[0])));
						tick(0x4BFA, 45);
						poke((uint16_t)(0x0338 + T.r[1]), peek((uint16_t)(0x033A + T.r[0])));
						tick(0x4C00, 45);
					}
				}
				else
					tick(0x4C06, 12);                  // BLT
				T.r[6] = (uint16_t)(T.r[6] + 4);
				fl::cmp(peek((uint16_t)(T.r[6] - 4)), peek((uint16_t)(T.r[6] - 2)));
				tick(0x4C08, 24);                      // CMP (SP)+,(SP)+: the arguments
			}
			else
				tick(0x4C08, 12);                      // BNE
			T.r[4] = fl::inc(T.r[4]);
			tick(0x4C0A, 12);
		}
		poke(0x01D2, peek((uint16_t)(sp + 2)));
		tick(0x4C14, 42);
		poke(0x01D4, peek(sp));
		fl::mov(peek(0x01D4));
		tick(0x4C18, 33);
		poke(0x1400, peek(0x01D2));
		fl::mov(peek(0x01D2));
		tick(0x4C1E, 39);
		poke(0x1402, peek(0x01D4));
		fl::mov(peek(0x01D4));
		tick(0x4C24, 39);
	}
	else
		tick(0x4C24, 12);                              // BLT
	T.r[6] = (uint16_t)(T.r[6] + 4);
	fl::cmp(peek(sp), peek((uint16_t)(sp + 2)));
	tick(0x4C26, 24);
	rts();
}

// A7:4C28 (assembly): show the next pending message (0x0362[0x0358]) in a new box: when all
// 8 are in use the oldest is removed first.  Text by F:A9FC; bits 0 of 0x036D / 0x036E ask
// for F:A8FE and the text table 0x5CD2 for this message.  The flags are those of the
// argument word left on the stack.
void show_next_message()
{
	T.r[0] = sext8(peekb(0x0358));
	fl::movb((uint8_t)T.r[0]);
	tick(0x4C2C, 24);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x4C2E, 12);
	const uint16_t msg = peek((uint16_t)(0x0362 + T.r[0]));
	fl::tst(msg);
	tick(0x4C32, 27);
	if (!msg)
	{
		tick(0x4D2E, 12);                              // BEQ
		rts();
		return;
	}
	tick(0x4C34, 12);
	const bool full = peekb(0x0336) == 7;
	fl::cmpb(peekb(0x0336), 0x07);
	tick(0x4C3A, 30);                                  // CMPB @#0x0336,#7
	if (full)
	{
		tick(0x4C3C, 12);
		T.r[3] = peek(0x01D2);
		fl::mov(T.r[3]);
		tick(0x4C40, 24);
		T.r[4] = peek(0x01D4);
		fl::mov(T.r[4]);
		tick(0x4C44, 24);
		T.r[0] = 0x2800;
		fl::mov(T.r[0]);
		tick(0x4C48, 18);
		poke(0x01D4, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4C4C, 27);
		poke(0x1402, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4C50, 27);
		const bool tiles = (int16_t)peek(peek(0x0348)) >= 0x4000;
		fl::cmp(peek(rw(0x0348)), 0x4000);
		tick(0x4C56, 39);                              // CMP @0x0348,#0x4000
		if (tiles)
		{
			tick(0x4C58, 12);
			push(peek(peek(0x0348)));
			fl::mov(peek(T.r[6]));
			tick(0x4C5C, 45);
			push(0x0001);
			fl::mov(0x0001);
			tick(0x4C60, 30);
			jsr(0x4C64, 0xEAD4);                       // draw_tile_block(1, block)
			tick(0x4C72, 12);                          // BR
		}
		else
		{
			tick(0x4C66, 12);                          // BLT
			push(peek(0x0348));
			fl::mov(peek(T.r[6]));
			tick(0x4C6A, 36);
			push(0x0001);
			fl::mov(0x0001);
			tick(0x4C6E, 30);
			jsr(0x4C72, 0x4D30);                       // message_box(erase)
		}
		T.r[2] = 0;
		fl::clr();
		tick(0x4C74, 12);
		for (;;)
		{
			T.r[1] = T.r[2];
			fl::mov(T.r[1]);
			tick(0x4C76, 12);
			T.r[1] = fl::asl(T.r[1]);
			tick(0x4C78, 12);
			T.r[0] = T.r[2];
			fl::mov(T.r[0]);
			tick(0x4C7A, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x4C7C, 12);
			poke((uint16_t)(0x0348 + T.r[1]), peek((uint16_t)(0x034A + T.r[0])));
			tick(0x4C82, 45);
			poke((uint16_t)(0x0338 + T.r[1]), peek((uint16_t)(0x033A + T.r[0])));
			tick(0x4C88, 45);
			T.r[2] = fl::inc(T.r[2]);
			tick(0x4C8A, 12);
			fl::cmp(T.r[2], 0x0006);
			tick(0x4C8E, 18);                          // CMP R2,#6
			if ((int16_t)T.r[2] > 6)
			{
				tick(0x4C90, 12);                      // BLE
				break;
			}
			tick(0x4C74, 12);
		}
		pokeb(0x0336, 6);
		tick(0x4C96, 33);
		poke(0x01D2, T.r[3]);
		fl::mov(T.r[3]);
		tick(0x4C9A, 27);
		poke(0x01D4, T.r[4]);
		fl::mov(T.r[4]);
		tick(0x4C9E, 27);
		poke(0x1400, peek(0x01D2));
		fl::mov(peek(0x01D2));
		tick(0x4CA4, 39);
		poke(0x1402, peek(0x01D4));
		fl::mov(peek(0x01D4));
		tick(0x4CAA, 39);
		T.r[6] = (uint16_t)(T.r[6] + 4);
		fl::cmp(peek((uint16_t)(T.r[6] - 4)), peek((uint16_t)(T.r[6] - 2)));
		tick(0x4CAC, 24);                              // CMP (SP)+,(SP)+
	}
	else
		tick(0x4CAC, 12);                              // BNE
	const bool font = (peekb(0x036D) & 1) != 0;
	fl::logicb((uint8_t)(0x01 & peekb(0x036D)));
	tick(0x4CB2, 30);                                  // BITB #1,@#0x036D
	if (font)
	{
		tick(0x4CB4, 12);
		jsr(0x4CB8, 0xA8FE);
	}
	else
		tick(0x4CB8, 12);                              // BEQ
	T.r[0] = 0;
	fl::clr();
	tick(0x4CBA, 12);
	T.r[0] = peekb(0x036D);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x4CBE, 24);
	T.r[0] = fl::asr(T.r[0]);
	tick(0x4CC0, 12);
	pokeb(0x036D, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x4CC4, 27);
	const bool table = (peek(0x036E) & 1) != 0;
	fl::logic((uint16_t)(0x0001 & peek(0x036E)));
	tick(0x4CCA, 30);                                  // BIT #1,@#0x036E
	if (table)
	{
		tick(0x4CCC, 12);
		poke(0x0184, 0x5CD2);
		fl::mov(0x5CD2);
		tick(0x4CD2, 33);
	}
	else
		tick(0x4CD2, 12);                              // BEQ
	T.r[0] = 0;
	fl::clr();
	tick(0x4CD4, 12);
	T.r[0] = peekb(0x036E);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x4CD8, 24);
	T.r[0] = fl::asr(T.r[0]);
	tick(0x4CDA, 12);
	pokeb(0x036E, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x4CDE, 27);
	T.r[0] = sext8(peekb(0x0358));
	fl::movb((uint8_t)T.r[0]);
	tick(0x4CE2, 24);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x4CE4, 12);
	push(peek((uint16_t)(0x0362 + T.r[0])));
	const uint16_t arg = T.r[6];
	fl::mov(peek(T.r[6]));
	tick(0x4CE8, 39);
	jsr(0x4CEC, 0xA9FC);                               // print the message
	poke(0x0184, 0x5AC8);
	fl::mov(0x5AC8);
	tick(0x4CF2, 33);
	T.r[0] = sext8(peekb(0x0336));
	fl::movb((uint8_t)T.r[0]);
	tick(0x4CF6, 24);
	T.r[0] = fl::inc(T.r[0]);
	tick(0x4CF8, 12);
	pokeb(0x0336, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x4CFC, 27);
	T.r[0] = sext8((uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x4CFE, 12);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x4D00, 12);
	T.r[1] = sext8(peekb(0x0358));
	fl::movb((uint8_t)T.r[1]);
	tick(0x4D04, 24);
	T.r[1] = fl::asl(T.r[1]);
	tick(0x4D06, 12);
	poke((uint16_t)(0x0348 + T.r[0]), peek((uint16_t)(0x0362 + T.r[1])));
	tick(0x4D0C, 45);
	T.r[2] = peek((uint16_t)(0x035A + T.r[1]));
	fl::mov(T.r[2]);
	tick(0x4D10, 27);
	T.r[2] = fl::add(T.r[2], peek(0x036A));
	tick(0x4D14, 24);
	poke((uint16_t)(0x0338 + T.r[0]), T.r[2]);
	fl::mov(T.r[2]);
	tick(0x4D18, 30);
	poke((uint16_t)(0x0362 + T.r[1]), 0);
	fl::clr();
	tick(0x4D1C, 30);
	T.r[0] = sext8(peekb(0x0358));
	fl::movb((uint8_t)T.r[0]);
	tick(0x4D20, 24);
	T.r[0] = fl::dec(T.r[0]);
	tick(0x4D22, 12);
	pokeb(0x0358, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x4D26, 27);
	T.r[0] = sext8((uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x4D28, 12);
	if (T.r[0] & 0x8000)
	{
		tick(0x4D2A, 12);                              // BGE
		poke(T.r[0], 0);
		fl::clr();
		tick(0x4D2C, 21);
	}
	else
		tick(0x4D2C, 12);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	fl::tst(peek(arg));
	tick(0x4D2E, 18);                                  // TST (SP)+
	rts();
}

// A7:5A7E  credit_messages(int line1, int line2): the credit / coin texts of the start and
// continue screens (F:8B04 prints message R0).  line1 = 0: "credits" (0x53, 0x5A for more
// than 4), the count (F:BA96) and a fraction tile pair for partial credits; otherwise the
// coin mode texts (0x54-0x57, 0x58, or 0x59 for free play).  Then 0x4F-0x52: by the credits
// against the price (0x5A46), or 0x52 when line2 = 0.
uint16_t credit_messages(const Args &a)
{
	c_save(2, 0x5A82);
	poke(0x0188, 0xE000);
	fl::mov(0xE000);
	tick(0x5A88, 33);
	fl::tst(peek((uint16_t)(T.r[6] + 0x0A)));
	tick(0x5A8C, 27);                                  // TST 0xA(SP)
	if (!a.u(0))
	{
		tick(0x5A8E, 12);
		T.r[0] = 0x0053;
		fl::mov(T.r[0]);
		tick(0x5A92, 18);
		jsr(0x5A96, 0x8B04);
		const bool many = peekb(0x01F9) > 4;
		fl::cmpb(peekb(0x01F9), 0x04);
		tick(0x5A9C, 30);                              // CMPB @#0x01F9,#4
		if (many)
		{
			tick(0x5A9E, 12);
			T.r[0] = 0x005A;
			fl::mov(T.r[0]);
			tick(0x5AA2, 18);
			jsr(0x5AA6, 0x8B04);
		}
		else
			tick(0x5AA6, 12);                          // BLOS
		poke(0x0186, fl::sub(peek(0x0186), 0x0018));
		tick(0x5AAC, 33);
		T.r[1] = 0;
		fl::clr();
		tick(0x5AAE, 12);
		T.r[1] = peekb(0x01F9);
		fl::logicb((uint8_t)T.r[1]);
		tick(0x5AB2, 24);
		T.r[1] = fl::asr(T.r[1]);
		tick(0x5AB4, 12);
		T.r[1] = fl::asr(T.r[1]);
		tick(0x5AB6, 12);
		push(T.r[1]);
		fl::mov(T.r[1]);
		tick(0x5AB8, 24);
		push(0);
		fl::clr();
		tick(0x5ABA, 24);
		push(0x0002);
		fl::mov(0x0002);
		tick(0x5ABE, 30);
		push(0x0001);
		fl::mov(0x0001);
		tick(0x5AC2, 30);
		push(0);
		fl::clr();
		tick(0x5AC4, 24);
		jsr(0x5AC8, 0xBA96);                           // print the count
		const bool part = (peekb(0x01F9) & 3) != 0;
		fl::logicb((uint8_t)(0x03 & peekb(0x01F9)));
		tick(0x5ACE, 30);                              // BITB #3,@#0x01F9
		if (part)
		{
			tick(0x5AD0, 12);
			T.r[0] = sext8(peekb(0x01FA));
			fl::movb((uint8_t)T.r[0]);
			tick(0x5AD4, 24);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0003));
			tick(0x5AD8, 18);
			T.r[1] = 0;
			fl::clr();
			tick(0x5ADA, 12);
			T.r[1] = peekb((uint16_t)(0x5A49 + T.r[0]));
			fl::logicb((uint8_t)T.r[1]);
			tick(0x5ADE, 27);
			T.r[0] = sext8(peekb(0x01F9));
			fl::movb((uint8_t)T.r[0]);
			tick(0x5AE2, 24);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0003));
			tick(0x5AE6, 18);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x5AE8, 12);
			T.r[0] = fl::add(T.r[0], T.r[1]);
			tick(0x5AEA, 12);
			T.r[1] = T.r[0];
			fl::mov(T.r[1]);
			tick(0x5AEC, 12);
			T.r[1] = fl::sub(T.r[1], 2);
			tick(0x5AF0, 18);
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0x5AF2, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x5AF4, 12);
			T.r[2] = peek((uint16_t)(0x5A4E + T.r[0]));
			fl::mov(T.r[2]);
			tick(0x5AF8, 27);
			T.r[2] = fl::logic((uint16_t)(T.r[2] | peek(0x0188)));
			tick(0x5AFC, 24);
			poke(peek(0x0186), T.r[2]);
			fl::mov(T.r[2]);
			tick(0x5B00, 36);
			T.r[0] = peek(0x0186);
			fl::mov(T.r[0]);
			tick(0x5B04, 24);
			T.r[2] = T.r[1];
			fl::mov(T.r[2]);
			tick(0x5B06, 12);
			T.r[2] = fl::asl(T.r[2]);
			tick(0x5B08, 12);
			T.r[3] = peek((uint16_t)(0x5A50 + T.r[2]));
			fl::mov(T.r[3]);
			tick(0x5B0C, 27);
			T.r[3] = fl::logic((uint16_t)(T.r[3] | peek(0x0188)));
			tick(0x5B10, 24);
			poke((uint16_t)(T.r[0] + 2), T.r[3]);
			fl::mov(T.r[3]);
			tick(0x5B14, 30);
		}
		else
			tick(0x5B14, 12);                          // BEQ
		T.r[6] = fl::add(T.r[6], 0x000A);
		tick(0x5B18, 18);
		tick(0x5B60, 12);                              // BR
	}
	else
	{
		tick(0x5B1A, 12);                              // BNE
		T.r[0] = 0;
		fl::clr();
		tick(0x5B1C, 12);
		T.r[0] = peekb(0x01FA);
		fl::logicb((uint8_t)T.r[0]);
		tick(0x5B20, 24);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00E0));
		tick(0x5B24, 18);
		fl::cmp(T.r[0], 0x00E0);
		tick(0x5B28, 18);                              // CMP R0,#0x00E0
		if (T.r[0] == 0x00E0)
		{
			tick(0x5B58, 12);                          // BEQ
			T.r[0] = 0x0059;
			fl::mov(T.r[0]);
			tick(0x5B5C, 18);
			jsr(0x5B60, 0x8B04);
		}
		else
		{
			tick(0x5B2A, 12);
			T.r[0] = 0;
			fl::clr();
			tick(0x5B2C, 12);
			T.r[0] = peekb(0x01F3);
			fl::logicb((uint8_t)T.r[0]);
			tick(0x5B30, 24);
			for (uint16_t pc = 0x5B32; pc <= 0x5B3C; pc = (uint16_t)(pc + 2))
			{
				T.r[0] = asr(T.r[0], 1);
				tick(pc, 12);
			}
			T.r[1] = T.r[0];
			tick(0x5B3E, 12);
			T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x0003));
			tick(0x5B42, 18);
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0x5B44, 12);
			T.r[0] = fl::add(T.r[0], 0x0054);
			tick(0x5B48, 18);
			jsr(0x5B4C, 0x8B04);
			fl::cmp(T.r[1], 0x0001);
			tick(0x5B50, 18);                          // CMP R1,#1
			if (T.r[1] == 1)
				tick(0x5B60, 12);                      // BEQ
			else
			{
				tick(0x5B52, 12);
				T.r[0] = 0x0058;
				fl::mov(T.r[0]);
				tick(0x5B56, 18);
				tick(0x5B5C, 12);                      // BR
				jsr(0x5B60, 0x8B04);
			}
		}
	}
	T.r[1] = 0;
	fl::clr();
	tick(0x5B62, 12);
	T.r[1] = peekb(0x01F9);
	fl::logicb((uint8_t)T.r[1]);
	tick(0x5B66, 24);
	fl::tst(peek((uint16_t)(T.r[6] + 0x0C)));
	tick(0x5B6A, 27);                                  // TST 0xC(SP)
	if (!a.u(1))
	{
		tick(0x5B6C, 12);
		T.r[1] = 0x0003;
		fl::mov(T.r[1]);
		tick(0x5B70, 18);
		tick(0x5BAC, 12);                              // BR
	}
	else
	{
		tick(0x5B72, 12);                              // BNE
		T.r[0] = 0;
		fl::clr();
		tick(0x5B74, 12);
		T.r[0] = peekb(0x01F3);
		fl::logicb((uint8_t)T.r[0]);
		tick(0x5B78, 24);
		for (uint16_t pc = 0x5B7A; pc <= 0x5B84; pc = (uint16_t)(pc + 2))
		{
			T.r[0] = asr(T.r[0], 1);
			tick(pc, 12);
		}
		T.r[0] &= 0x0003;
		tick(0x5B88, 18);
		T.r[2] = 0;
		fl::clr();
		tick(0x5B8A, 12);
		T.r[2] = peekb((uint16_t)(0x5A46 + T.r[0]));
		fl::logicb((uint8_t)T.r[2]);
		tick(0x5B8E, 27);
		T.r[2] = fl::asl(T.r[2]);
		tick(0x5B90, 12);
		fl::cmp(T.r[1], T.r[2]);
		tick(0x5B92, 12);                              // CMP R1,R2
		if ((int16_t)T.r[1] >= (int16_t)T.r[2])
		{
			tick(0x5B94, 12);                          // BLT
			T.r[1] = 0;
			fl::clr();
			tick(0x5B96, 12);
			tick(0x5BAC, 12);                          // BR
		}
		else
		{
			tick(0x5B98, 12);
			T.r[2] = 0;
			fl::clr();
			tick(0x5B9A, 12);
			T.r[2] = peekb((uint16_t)(0x5A46 + T.r[0]));
			fl::logicb((uint8_t)T.r[2]);
			tick(0x5B9E, 27);
			fl::cmp(T.r[1], T.r[2]);
			tick(0x5BA0, 12);                          // CMP R1,R2
			if ((int16_t)T.r[1] >= (int16_t)T.r[2])
			{
				tick(0x5BA2, 12);                      // BLT
				T.r[1] = 0x0001;
				fl::mov(T.r[1]);
				tick(0x5BA6, 18);
				tick(0x5BAC, 12);                      // BR
			}
			else
			{
				tick(0x5BA8, 12);
				T.r[1] = 0x0002;
				fl::mov(T.r[1]);
				tick(0x5BAC, 18);
			}
		}
	}
	T.r[1] = fl::add(T.r[1], 0x004F);
	tick(0x5BB0, 18);
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0x5BB2, 12);
	jsr(0x5BB6, 0x8B04);
	c_restore(2);
	return T.r[0];
}

// ---------------------------------------------------------------- game state

// A7:5018: the chaser (object 7).  Its timer 0x0EB4 counts down; while it is on screen it
// keeps the timer negative and is steered with the skater's velocity.  When the timer runs
// out it gets faster (0x0EB2, up to 7; 0x0F22 the delay from 0xFE0D) and F:E13A moves it
// towards the skater; the timer restarts at 5 (on screen) or 16-31 (random).
uint16_t chaser_step(const Args &)
{
	c_save(1, 0x501C);
	poke(0x043E, 0x0001);
	fl::mov(0x0001);
	tick(0x5022, 33);
	poke(0x0EB4, fl::dec(peek(0x0EB4)));
	tick(0x5026, 27);
	T.r[0] = 0;
	fl::clr();
	tick(0x5028, 12);
	T.r[1] = 0;
	fl::clr();
	tick(0x502A, 12);
	T.r[2] = 0;
	fl::clr();
	tick(0x502C, 12);
	bool visible = false;
	const uint16_t sx = peek(0x063E);
	fl::cmp(peek(0x063E), 0x8200);
	tick(0x5032, 30);                                  // CMP @#0x063E,#0x8200
	if (sx <= 0x8200)
		tick(0x503C, 12);                              // BLOS
	else
	{
		tick(0x5034, 12);
		visible = peek(0x063E) < 0xF000;
		fl::cmp(peek(0x063E), 0xF000);
		tick(0x503A, 30);                              // CMP @#0x063E,#0xF000
		tick(visible ? 0x504C : 0x503C, 12);           // BCS
	}
	if (!visible)
	{
		const uint16_t sy = peek(0x067E);
		fl::cmp(peek(0x067E), 0x1000);
		tick(0x5042, 30);                              // CMP @#0x067E,#0x1000
		if ((int16_t)sy >= 0x1000)
			tick(0x505E, 12);                          // BGE
		else
		{
			tick(0x5044, 12);
			visible = peek(0x067E) > 0x9000;
			fl::cmp(peek(0x067E), 0x9000);
			tick(0x504A, 30);                          // CMP @#0x067E,#0x9000
			tick(visible ? 0x504C : 0x505E, 12);       // BLOS
		}
	}
	if (visible)
	{
		T.r[1] = peek(0x04F0);
		fl::mov(T.r[1]);
		tick(0x5050, 24);
		T.r[2] = peek(0x0570);
		fl::mov(T.r[2]);
		tick(0x5054, 24);
		T.r[2] = fl::add(T.r[2], peek(0x05B0));
		tick(0x5058, 24);
		poke(0x0EB4, 0xFFFF);
		fl::mov(0xFFFF);
		tick(0x505E, 33);
	}
	const bool waiting = (int16_t)peek(0x0EB4) > 0;
	fl::tst(peek(0x0EB4));
	tick(0x5062, 24);                                  // TST @#0x0EB4
	if (waiting)
	{
		tick(0x50C6, 12);                              // BGT
		c_restore(1);
		return T.r[0];
	}
	tick(0x5064, 12);
	const bool delay = (int16_t)peek(0x0F22) > 0;
	fl::tst(peek(0x0F22));
	tick(0x5068, 24);                                  // TST @#0x0F22
	if (delay)
		tick(0x508C, 12);                              // BGT
	else
	{
		tick(0x506A, 12);
		const bool fastest = (int8_t)peekb(0x0EB2) > 6;
		fl::cmpb(peekb(0x0EB2), 0x06);
		tick(0x5070, 30);                              // CMPB @#0x0EB2,#6
		if (fastest)
			tick(0x507C, 12);                          // BGT
		else
		{
			tick(0x5072, 12);
			T.r[0] = sext8(peekb(0x0EB2));
			fl::movb((uint8_t)T.r[0]);
			tick(0x5076, 24);
			T.r[0] = fl::inc(T.r[0]);
			tick(0x5078, 12);
			pokeb(0x0EB2, (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0x507C, 27);
		}
		T.r[0] = 0;
		fl::clr();
		tick(0x507E, 12);
		T.r[0] = peekb(0x0F2E);
		fl::logicb((uint8_t)T.r[0]);
		tick(0x5082, 24);
		pokeb(0x0F22, rb((uint16_t)(0xFE0D + T.r[0])));
		fl::movb(peekb(0x0F22));
		tick(0x5088, 42);
		pokeb(0x0F23, 0);
		fl::clr();
		tick(0x508C, 27);
	}
	jsr(0x5090, 0x4F98);                               // direction_from_object7
	push(T.r[0]);
	fl::mov(T.r[0]);
	tick(0x5092, 24);
	T.r[0] = sext8(peekb(0x0EB2));
	fl::movb((uint8_t)T.r[0]);
	tick(0x5096, 24);
	push(T.r[0]);
	fl::mov(T.r[0]);
	tick(0x5098, 24);
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x509A, 24);
	push(T.r[2]);
	fl::mov(T.r[2]);
	tick(0x509C, 24);
	jsr(0x50A0, 0xE13A);                               // set_object7_motion
	const bool shown = (int16_t)peek(0x0EB4) < 0;
	fl::tst(peek(0x0EB4));
	tick(0x50A4, 24);                                  // TST @#0x0EB4
	if (shown)
	{
		tick(0x50A6, 12);                              // BGE
		poke(0x0EB4, 0x0005);
		fl::mov(0x0005);
		tick(0x50AC, 33);
		tick(0x50C2, 12);                              // BR
	}
	else
	{
		tick(0x50AE, 12);
		jsr(0x50B2, 0x9152);                           // next_random
		poke(0x0EB4, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x50B6, 27);
		poke(0x0EB4, fl::logic((uint16_t)(peek(0x0EB4) & 0x000F)));
		tick(0x50BC, 33);
		poke(0x0EB4, fl::add(peek(0x0EB4), 0x0010));
		tick(0x50C2, 33);
	}
	T.r[6] = fl::add(T.r[6], 8);
	tick(0x50C6, 18);
	c_restore(1);
	return T.r[0];
}

// A7:5248: buying (bit 0x10 of 0x0ED7): at the shop A7:5326, if the cash 0x0256 covers the
// price (tables 0xFE7C/0xFE70 by 0x0F31) the item count 0x025C+shop goes up, the price is
// paid (F:F002) and a sound plays; otherwise the "not enough" message.  The request is cleared.
uint16_t shop_purchase(const Args &)
{
	c_save(1, 0x524C);
	const bool buying = (peekb(0x0ED7) & 0x10) != 0;
	fl::logicb((uint8_t)(0x10 & peekb(0x0ED7)));
	tick(0x5252, 30);                                  // BITB #0x10,@#0x0ED7
	if (!buying)
	{
		tick(0x52FA, 12);                              // BEQ
		c_restore(1);
		return T.r[0];
	}
	tick(0x5254, 12);
	jsr(0x5258, 0x5326);                               // park_at_area: the shop
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0x525A, 12);
	pokeb(0x0ED8, fl::logicb((uint8_t)(peekb(0x0ED8) | rb((uint16_t)(0xFC74 + T.r[1])))));
	tick(0x5260, 42);
	T.r[0] = 0;
	fl::clr();
	tick(0x5262, 12);
	T.r[0] = peekb(0x0F31);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x5266, 24);
	T.r[0] = sext8(rb((uint16_t)(0xFE7C + T.r[0])));
	fl::movb((uint8_t)T.r[0]);
	tick(0x526A, 27);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
	tick(0x526E, 18);
	T.r[2] = 0;
	fl::clr();
	tick(0x5270, 12);
	T.r[2] = rb((uint16_t)(0xFE70 + T.r[0]));
	fl::logicb((uint8_t)T.r[2]);
	tick(0x5274, 27);
	const bool enough = (int16_t)peek(0x0256) >= (int16_t)T.r[2];
	fl::cmp(peek(0x0256), T.r[2]);
	tick(0x5278, 24);                                  // CMP @#0x0256,R2
	if (enough)
	{
		tick(0x527A, 12);                              // BLT
		T.r[0] = 0x025C;
		fl::mov(T.r[0]);
		tick(0x527E, 18);
		T.r[0] = fl::add(T.r[0], T.r[1]);
		tick(0x5280, 12);
		const bool none = (peekb(T.r[0]) & 0x80) != 0;
		fl::tstb(peekb(T.r[0]));
		tick(0x5282, 18);                              // TSTB (R0)
		if (none)
		{
			tick(0x5284, 12);                          // BGE
			pokeb(T.r[0], 0);
			fl::clr();
			tick(0x5286, 21);
		}
		else
			tick(0x5286, 12);
		pokeb(T.r[0], fl::incb(peekb(T.r[0])));
		tick(0x5288, 21);
		push(0x76C8);
		fl::mov(0x76C8);
		tick(0x528C, 30);
		push(0x00D2);
		fl::mov(0x00D2);
		tick(0x5290, 30);
		push(0x0001);
		fl::mov(0x0001);
		tick(0x5294, 30);
		jsr(0x5298, 0xA974);                           // the bought message
		T.r[2] = (uint16_t)-T.r[2];
		fl::neg((uint16_t)(-T.r[2]));
		tick(0x529A, 12);
		poke(T.r[6], T.r[2]);
		fl::mov(T.r[2]);
		tick(0x529C, 21);
		jsr(0x52A0, 0xF002);                           // add_cash(-price)
		poke(T.r[6], T.r[1]);
		fl::mov(T.r[1]);
		tick(0x52A2, 21);
		push(0);
		fl::clr();
		tick(0x52A4, 24);
		jsr(0x52A8, 0xD97C);
		jsr(0x52AC, 0x9152);                           // next_random
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0007));
		tick(0x52B0, 18);
		poke(T.r[6], 0);
		fl::clr();
		tick(0x52B2, 21);
		pokeb(T.r[6], rb((uint16_t)(0xFE1E + T.r[0])));
		fl::movb(peekb(T.r[6]));
		tick(0x52B6, 36);
		jsr(0x52BA, 0x8E78);                           // queue_sound
		jsr(0x52BE, 0x5602);                           // set_ticket_bits
		jsr(0x52C2, 0xD8D2);
		tick(0x52F0, 12);                              // BR
	}
	else
	{
		tick(0x52C4, 12);                              // BLT
		push(0x76D6);
		fl::mov(0x76D6);
		tick(0x52C8, 30);
		push(0x00F0);
		fl::mov(0x00F0);
		tick(0x52CC, 30);
		push(0x0001);
		fl::mov(0x0001);
		tick(0x52D0, 30);
		jsr(0x52D4, 0xA974);                           // the not-enough message
		poke(T.r[6], T.r[1]);
		fl::mov(T.r[1]);
		tick(0x52D6, 21);
		push(0x0001);
		fl::mov(0x0001);
		tick(0x52DA, 30);
		jsr(0x52DE, 0xD97C);
		jsr(0x52E2, 0x9152);                           // next_random
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0003));
		tick(0x52E6, 18);
		poke(T.r[6], 0);
		fl::clr();
		tick(0x52E8, 21);
		pokeb(T.r[6], rb((uint16_t)(0xFE26 + T.r[0])));
		fl::movb(peekb(T.r[6]));
		tick(0x52EC, 36);
		jsr(0x52F0, 0x8E78);                           // queue_sound
	}
	pokeb(0x0ED7, fl::logicb((uint8_t)(peekb(0x0ED7) & ~0x10)));
	tick(0x52F6, 33);
	T.r[6] = fl::add(T.r[6], 8);
	tick(0x52FA, 18);
	c_restore(1);
	return T.r[0];
}

// A7:43C0  insert_high_score(int first, long score): find the first of the 15 entries from
// first on (8 bytes at 0x0370: high word, low word, ...) below score and insert score there,
// moving the lower entries down.  Returns the place or -1.  The entry minus the score is
// worked out by F:80D0 (sub32) in two stack words.
uint16_t insert_high_score(const Args &a)
{
	c_save(4, 0x43C4);
	T.r[6] = (uint16_t)(T.r[6] - 4);                   // CMP -(SP),-(SP): the difference
	fl::cmp(peek((uint16_t)(T.r[6] + 2)), peek(T.r[6]));
	tick(0x43C6, 30);
	T.r[5] = 0xFFFF;
	fl::mov(T.r[5]);
	tick(0x43CA, 18);
	push(a.u(0));
	fl::mov(peek(T.r[6]));
	tick(0x43CE, 39);
	poke(T.r[6], (uint16_t)(peek(T.r[6]) + 0x000E));  // the last entry
	fl::add((uint16_t)(peek(T.r[6]) - 0x000E), 0x000E);
	tick(0x43D2, 27);
	push(a.u(0));
	fl::mov(peek(T.r[6]));
	tick(0x43D6, 39);
	poke(T.r[6], (uint16_t)(peek(T.r[6]) + T.r[5]));  // the entry, counted up from first - 1
	fl::add((uint16_t)(peek(T.r[6]) - T.r[5]), T.r[5]);
	tick(0x43D8, 21);
	const uint16_t sp = T.r[6];
	tick(0x4410, 12);                                  // BR
	bool found = false;
	for (;;)
	{
		poke(sp, fl::inc(peek(sp)));
		tick(0x4412, 21);
		const bool more = (int16_t)peek(sp) <= (int16_t)peek((uint16_t)(sp + 2));
		fl::cmp(peek(T.r[6]), peek((uint16_t)(T.r[6] + 2)));
		tick(0x4416, 33);                              // CMP (SP),2(SP)
		if (!more)
		{
			tick(0x4418, 12);                          // BLE
			break;
		}
		tick(0x43DA, 12);
		T.r[0] = peek(sp);
		fl::mov(T.r[0]);
		tick(0x43DC, 18);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x43DE, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x43E0, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x43E2, 12);
		poke((uint16_t)(sp + 4), peek((uint16_t)(0x0370 + T.r[0])));
		fl::mov(peek((uint16_t)(T.r[6] + 4)));
		tick(0x43E8, 45);
		poke((uint16_t)(sp + 6), peek((uint16_t)(0x0372 + T.r[0])));
		fl::mov(peek((uint16_t)(T.r[6] + 6)));
		tick(0x43EE, 45);
		T.r[1] = 0x0004;
		fl::mov(T.r[1]);
		tick(0x43F2, 18);
		T.r[1] = fl::add(T.r[1], T.r[6]);
		tick(0x43F4, 12);
		T.r[2] = 0x0006;
		fl::mov(T.r[2]);
		tick(0x43F8, 18);
		T.r[2] = fl::add(T.r[2], T.r[6]);
		tick(0x43FA, 12);
		T.r[3] = a.u(2);
		fl::mov(T.r[3]);
		tick(0x43FE, 27);
		T.r[4] = a.u(1);
		fl::mov(T.r[4]);
		tick(0x4402, 27);
		jsr(0x4406, 0x80D0);                           // sub32
		const bool below = (int16_t)peek((uint16_t)(sp + 4)) < 0;
		fl::tst(peek((uint16_t)(T.r[6] + 4)));
		tick(0x440A, 27);                              // TST 4(SP)
		if (below)
		{
			tick(0x440C, 12);                          // BGE
			T.r[5] = peek(sp);
			fl::mov(T.r[5]);
			tick(0x440E, 18);
			tick(0x441A, 12);                          // BR
			found = true;
			break;
		}
		tick(0x4410, 12);
	}
	if (!found)
		tick(0x441A, 12);                              // TST R5
	if (T.r[5] & 0x8000)
		tick(0x446E, 12);                              // BLT
	else
	{
		tick(0x441C, 12);
		T.r[3] = T.r[5];
		fl::mov(T.r[3]);
		tick(0x441E, 12);
		T.r[3] = fl::inc(T.r[3]);
		tick(0x4420, 12);
		T.r[2] = a.u(0);
		fl::mov(T.r[2]);
		tick(0x4424, 27);
		T.r[2] = fl::add(T.r[2], 0x000F);
		tick(0x4428, 18);
		tick(0x4454, 12);                              // BR
		for (;;)
		{
			T.r[2] = fl::dec(T.r[2]);
			tick(0x4456, 12);
			fl::cmp(T.r[2], T.r[3]);
			tick(0x4458, 12);                          // CMP R2,R3
			if ((int16_t)T.r[2] < (int16_t)T.r[3])
			{
				tick(0x445A, 12);                      // BGE
				break;
			}
			tick(0x442A, 12);
			T.r[0] = T.r[2];
			fl::mov(T.r[0]);
			tick(0x442C, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x442E, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x4430, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x4432, 12);
			T.r[1] = T.r[0];
			fl::mov(T.r[1]);
			tick(0x4434, 12);
			T.r[0] = T.r[2];
			fl::mov(T.r[0]);
			tick(0x4436, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x4438, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x443A, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x443C, 12);
			uint16_t pc = 0x443C;
			for (int k = 0; k < 8; k += 2)
			{
				poke((uint16_t)(0x0370 + k + T.r[1]), peek((uint16_t)(0x0368 + k + T.r[0])));
				pc = (uint16_t)(pc + 6);
				tick(pc, 45);
			}
		}
		T.r[0] = T.r[5];
		tick(0x445C, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x445E, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x4460, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x4462, 12);
		poke((uint16_t)(0x0370 + T.r[0]), a.u(2));
		fl::mov(peek((uint16_t)(T.r[6] + 0x1A)));
		tick(0x4468, 45);
		poke((uint16_t)(0x0372 + T.r[0]), a.u(1));
		fl::mov(peek((uint16_t)(T.r[6] + 0x18)));
		tick(0x446E, 45);
	}
	T.r[0] = T.r[5];
	fl::mov(T.r[0]);
	tick(0x4470, 12);
	T.r[6] = fl::add(T.r[6], 8);
	tick(0x4474, 18);
	c_restore(4);
	return T.r[0];
}

// A7:5684: the first object from 0x0994 down to 5 whose box (animation 0x03F0: x, y, width,
// height) overlaps the skater's (0x023A at 0x0630/0x0238) and whose shapes touch (A7:572A).
// Returns the object or 0.
uint16_t find_collision(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x5686, 24);
	push(T.r[2]);
	fl::mov(T.r[2]);
	tick(0x5688, 24);
	push(T.r[3]);
	fl::mov(T.r[3]);
	tick(0x568A, 24);
	push(T.r[4]);
	fl::mov(T.r[4]);
	tick(0x568C, 24);
	push(T.r[5]);
	fl::mov(T.r[5]);
	tick(0x568E, 24);
	T.r[1] = peek(0x023A);
	fl::mov(T.r[1]);
	tick(0x5692, 24);
	T.r[2] = sext8(peekb(0x0994));
	fl::movb((uint8_t)T.r[2]);
	tick(0x5696, 24);
	T.r[2] = fl::sub(T.r[2], 5);
	tick(0x569A, 18);
	T.r[2] = fl::asl(T.r[2]);
	tick(0x569C, 12);
	// the tests of one object, up to the BEQ after the call; false: on to 0x5716
	auto touches = [&]() -> bool {
		const uint16_t obj = peek((uint16_t)(0x03FA + T.r[2]));
		fl::tst(peek((uint16_t)(T.r[2] + 0x03FA)));
		tick(0x56A4, 27);                              // TST 0x3FA(R2)
		if (!obj)
		{
			tick(0x5716, 12);
			return false;
		}
		tick(0x56A6, 12);
		T.r[0] = peek((uint16_t)(0x03FA + T.r[2]));
		fl::mov(T.r[0]);
		tick(0x56AA, 27);
		T.r[3] = peek((uint16_t)(0x063A + T.r[2]));
		fl::mov(T.r[3]);
		tick(0x56AE, 27);
		T.r[3] = fl::add(T.r[3], peek(T.r[0]));
		tick(0x56B0, 18);
		T.r[5] = peek(0x0630);
		fl::mov(T.r[5]);
		tick(0x56B4, 24);
		T.r[5] = fl::add(T.r[5], peek(T.r[1]));
		tick(0x56B6, 18);
		bool less = (int16_t)T.r[3] < (int16_t)T.r[5];
		T.r[3] = fl::sub(T.r[3], T.r[5]);
		tick(0x56B8, 12);
		if (!less)
		{
			tick(0x56BA, 12);                          // BLT
			const bool near = T.r[3] < peek((uint16_t)(T.r[1] + 4));
			fl::cmp(T.r[3], peek((uint16_t)(T.r[1] + 0x0004)));
			tick(0x56BE, 27);                          // CMP R3,4(R1)
			if (!near)
			{
				tick(0x56C0, 12);                      // BCS
				tick(0x5716, 12);                      // BR
				return false;
			}
			tick(0x56CA, 12);
		}
		else
		{
			tick(0x56C2, 12);
			T.r[3] = (uint16_t)-T.r[3];
			fl::neg((uint16_t)(-T.r[3]));
			tick(0x56C4, 12);
			const bool near = T.r[3] < peek((uint16_t)(T.r[0] + 4));
			fl::cmp(T.r[3], peek((uint16_t)(T.r[0] + 0x0004)));
			tick(0x56C8, 27);                          // CMP R3,4(R0)
			if (!near)
			{
				tick(0x5716, 12);                      // BCC
				return false;
			}
			tick(0x56CA, 12);
		}
		T.r[3] = peek((uint16_t)(0x067A + T.r[2]));
		fl::mov(T.r[3]);
		tick(0x56CE, 27);
		T.r[3] = fl::add(T.r[3], peek((uint16_t)(T.r[0] + 2)));
		tick(0x56D2, 27);
		T.r[5] = peek(0x0238);
		fl::mov(T.r[5]);
		tick(0x56D6, 24);
		T.r[5] = fl::add(T.r[5], peek((uint16_t)(T.r[1] + 2)));
		tick(0x56DA, 27);
		less = (int16_t)T.r[3] < (int16_t)T.r[5];
		T.r[3] = fl::sub(T.r[3], T.r[5]);
		tick(0x56DC, 12);
		if (!less)
		{
			tick(0x56DE, 12);                          // BLT
			const bool near = T.r[3] < peek((uint16_t)(T.r[1] + 6));
			fl::cmp(T.r[3], peek((uint16_t)(T.r[1] + 0x0006)));
			tick(0x56E2, 27);                          // CMP R3,6(R1)
			if (!near)
			{
				tick(0x56E4, 12);                      // BCS
				tick(0x5716, 12);                      // BR
				return false;
			}
			tick(0x56F0, 12);
		}
		else
		{
			tick(0x56E6, 12);
			T.r[4] = T.r[3];
			fl::mov(T.r[4]);
			tick(0x56E8, 12);
			T.r[4] = (uint16_t)-T.r[4];
			fl::neg((uint16_t)(-T.r[4]));
			tick(0x56EA, 12);
			const bool near = T.r[4] < peek((uint16_t)(T.r[0] + 6));
			fl::cmp(T.r[4], peek((uint16_t)(T.r[0] + 0x0006)));
			tick(0x56EE, 27);                          // CMP R4,6(R0)
			if (!near)
			{
				tick(0x5716, 12);                      // BCC
				return false;
			}
			tick(0x56F0, 12);
		}
		// A7:572A shapes_touch(dx, slot + 10, slot, box)
		push(T.r[1]);
		fl::mov(T.r[1]);
		tick(0x56F2, 24);
		push(T.r[2]);
		fl::mov(T.r[2]);
		tick(0x56F4, 24);
		push(T.r[2]);
		fl::mov(T.r[2]);
		tick(0x56F6, 24);
		poke(T.r[6], fl::add(peek(T.r[6]), 0x000A));
		tick(0x56FA, 27);
		push(T.r[3]);
		fl::mov(T.r[3]);
		tick(0x56FC, 24);
		jsr(0x5700, 0x572A);
		const uint16_t v = pop();                      // MOV (SP)+,(SP)+
		poke(T.r[6], v);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::mov(peek((uint16_t)(T.r[6] - 2)));
		tick(0x5702, 27);
		T.r[2] = pop();
		fl::mov(T.r[2]);
		tick(0x5704, 18);
		T.r[1] = pop();
		fl::mov(T.r[1]);
		tick(0x5706, 18);
		fl::tst(T.r[0]);
		tick(0x5708, 12);                              // TST R0
		if (!T.r[0])
		{
			tick(0x5716, 12);                          // BEQ
			return false;
		}
		tick(0x570A, 12);
		return true;
	};
	for (;;)
	{
		fl::tst(T.r[2]);
		tick(0x569E, 12);                              // TST R2
		if ((int16_t)T.r[2] < 0)
		{
			tick(0x571C, 12);                          // BLT
			T.r[0] = 0;
			fl::clr();
			tick(0x571E, 12);
			break;
		}
		tick(0x56A0, 12);
		if (touches())
		{
			T.r[4] = T.r[2];
			tick(0x570C, 12);
			T.r[4] = fl::asr(T.r[4]);
			tick(0x570E, 12);
			T.r[0] = T.r[4];
			fl::mov(T.r[0]);
			tick(0x5710, 12);
			T.r[0] = fl::add(T.r[0], 5);
			tick(0x5714, 18);
			tick(0x571E, 12);                          // BR
			break;
		}
		T.r[2] = fl::sub(T.r[2], 2);
		tick(0x571A, 18);
		tick(0x569C, 12);                              // BR
	}
	T.r[5] = pop();
	fl::mov(T.r[5]);
	tick(0x5720, 18);
	T.r[4] = pop();
	fl::mov(T.r[4]);
	tick(0x5722, 18);
	T.r[3] = pop();
	fl::mov(T.r[3]);
	tick(0x5724, 18);
	T.r[2] = pop();
	fl::mov(T.r[2]);
	tick(0x5726, 18);
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x5728, 18);
	rts();
	return T.r[0];
}

// A7:50F8 (assembly): the skater after a fall (0x031F = 1): the timer 0x02FA runs down, then
// F:B602(2, 2) and sound 0x55 end the fall; on a slope (0x0327 bits 0-2) the skater turns to
// the slope's direction (0xFCD0, F:9B82), on flat ground he slows down by 20.  State 2 is
// not used by the game.  When the state is clear the kick state is reset (F:E238).
void fallen_skater()
{
	const uint8_t st = peekb(0x031F);
	T.r[1] = sext8(st);
	fl::movb(st);
	tick(0x50FC, 24);
	if (!st)
	{
		tick(0x51DC, 12);                              // BEQ
		rts();
		return;
	}
	tick(0x50FE, 12);
	const bool timing = peek(0x02FA) != 0;
	fl::tst(peek(0x02FA));
	tick(0x5102, 24);                                  // TST @#0x02FA
	if (timing)
	{
		tick(0x5104, 12);
		poke(0x02FA, fl::dec(peek(0x02FA)));
		tick(0x5108, 27);
	}
	else
		tick(0x5108, 12);                              // BEQ
	fl::cmp(T.r[1], 0x0001);
	tick(0x510C, 18);                                  // CMP R1,#1
	if (T.r[1] == 1)
	{
		tick(0x510E, 12);
		const bool fast = (int16_t)peek(0x02FC) >= 0x0380;
		fl::cmp(peek(0x02FC), 0x0380);
		tick(0x5114, 30);                              // CMP @#0x02FC,#0x0380
		bool end;
		if (fast)
		{
			tick(0x5116, 12);                          // BLT
			const bool left = peek(0x02FA) != 0;
			fl::tst(peek(0x02FA));
			tick(0x511A, 24);                          // TST @#0x02FA
			if (!left)
			{
				tick(0x5126, 12);                      // BEQ
				end = true;
			}
			else
			{
				tick(0x511C, 12);
				poke(0x02FA, fl::dec(peek(0x02FA)));
				tick(0x5120, 27);
				end = !peek(0x02FA);
				tick(0x5124, 24);                      // TST @#0x02FA
				tick(end ? 0x5126 : 0x5140, 12);       // BNE
			}
		}
		else
		{
			tick(0x5120, 12);                          // BLT
			end = !peek(0x02FA);
			tick(0x5124, 24);                          // TST @#0x02FA
			tick(end ? 0x5126 : 0x5140, 12);           // BNE
		}
		if (end)
		{
			push(0x0002);
			fl::mov(0x0002);
			tick(0x512A, 30);
			push(0x0002);
			fl::mov(0x0002);
			tick(0x512E, 30);
			jsr(0x5132, 0xB602);
			pokeb(0x031F, 0);
			fl::clr();
			tick(0x5136, 27);
			poke(T.r[6], 0x0055);
			fl::mov(0x0055);
			tick(0x513A, 27);
			jsr(0x513E, 0x8E78);                       // queue_sound
			T.r[6] = (uint16_t)(T.r[6] + 4);
			fl::cmp(peek((uint16_t)(T.r[6] - 4)), peek((uint16_t)(T.r[6] - 2)));
			tick(0x5140, 24);                          // CMP (SP)+,(SP)+
		}
		const bool slope = (peekb(0x0327) & 7) != 0;
		fl::logicb((uint8_t)(0x07 & peekb(0x0327)));
		tick(0x5146, 30);                              // BITB #7,@#0x0327
		if (slope)
		{
			tick(0x5168, 12);                          // BNE
			T.r[0] = 0;
			fl::clr();
			tick(0x516A, 12);
			T.r[0] = peekb(0x0327);
			fl::logicb((uint8_t)T.r[0]);
			tick(0x516E, 24);
			T.r[0] = fl::asr(T.r[0]);
			tick(0x5170, 12);
			T.r[0] = fl::asr(T.r[0]);
			tick(0x5172, 12);
			T.r[0] = fl::asr(T.r[0]);
			tick(0x5174, 12);
			T.r[0] = sext8(rb((uint16_t)(0xFCD0 + T.r[0])));
			fl::movb((uint8_t)T.r[0]);
			tick(0x5178, 27);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
			tick(0x517C, 18);
			T.r[2] = 0;
			fl::clr();
			tick(0x517E, 12);
			T.r[2] = peekb(0x031D);
			fl::logicb((uint8_t)T.r[2]);
			tick(0x5182, 24);
			fl::cmp(T.r[0], T.r[2]);
			tick(0x5184, 12);                          // CMP R0,R2
			if (T.r[0] == T.r[2])
				tick(0x5192, 12);                      // BEQ
			else
			{
				tick(0x5186, 12);
				pokeb(0x031D, (uint8_t)T.r[0]);
				fl::movb((uint8_t)T.r[0]);
				tick(0x518A, 27);
				pokeb(0x01F8, (uint8_t)T.r[0]);
				fl::movb((uint8_t)T.r[0]);
				tick(0x518E, 27);
				jsr(0x5192, 0x9B82);
			}
		}
		else
		{
			tick(0x5148, 12);
			const bool rolling = peekb(0x031E) != 0;
			fl::tstb(peekb(0x031E));
			tick(0x514C, 24);                          // TSTB @#0x031E
			if (rolling)
				tick(0x5192, 12);                      // BNE
			else
			{
				tick(0x514E, 12);
				const uint16_t v = (uint16_t)(peek(0x01B4) - 0x0014);
				poke(0x01B4, v);
				fl::sub((uint16_t)(peek(0x01B4) + 0x0014), 0x0014);
				tick(0x5154, 33);
				tick(0x5156, 18);                      // CCC CV
				if ((int16_t)v > 0)
					tick(0x5192, 12);                  // BGT
				else
				{
					tick(0x5158, 12);
					poke(0x01B4, 0);
					fl::clr();
					tick(0x515C, 27);
					push(0x0055);
					fl::mov(0x0055);
					tick(0x5160, 30);
					jsr(0x5164, 0x8E78);               // queue_sound
					T.r[6] = (uint16_t)(T.r[6] + 2);
					fl::tst(peek((uint16_t)(T.r[6] - 2)));
					tick(0x5166, 18);                  // TST (SP)+
					tick(0x5192, 12);                  // BR
				}
			}
		}
	}
	else
		tick(0x5192, 12);                              // BNE
	fl::cmp(T.r[1], 0x0002);
	tick(0x5196, 18);                                  // CMP R1,#2
	if (T.r[1] == 2)
	{
		tick(0x5198, 12);
		poke(0x01B4, 0);
		fl::clr();
		tick(0x519C, 27);
		const bool left = peek(0x02FA) != 0;
		fl::tst(peek(0x02FA));
		tick(0x51A0, 24);                              // TST @#0x02FA
		if (left)
			tick(0x51D2, 12);                          // BNE
		else
		{
			tick(0x51A2, 12);
			pokeb(0x031B, 0x3F);
			fl::movb(0x3F);
			tick(0x51A8, 33);
			pokeb(0x031F, 0);
			fl::clr();
			tick(0x51AC, 27);
			T.r[0] = sext8(peekb(0x0327));
			fl::movb((uint8_t)T.r[0]);
			tick(0x51B0, 24);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0007));
			tick(0x51B4, 18);
			fl::cmp(T.r[0], 0x0004);
			tick(0x51B8, 18);                          // CMP R0,#4
			if (T.r[0] < 4)
				tick(0x51D2, 12);                      // BCS
			else
			{
				tick(0x51BA, 12);
				pokeb(0x0325, 0x7F);
				fl::movb(0x7F);
				tick(0x51C0, 33);
				T.r[0] = 0;
				fl::clr();
				tick(0x51C2, 12);
				T.r[0] = peekb(0x0327);
				fl::logicb((uint8_t)T.r[0]);
				tick(0x51C6, 24);
				T.r[0] = fl::asr(T.r[0]);
				tick(0x51C8, 12);
				T.r[0] = fl::asr(T.r[0]);
				tick(0x51CA, 12);
				T.r[0] = fl::asr(T.r[0]);
				tick(0x51CC, 12);
				pokeb(0x031D, rb((uint16_t)(0xFCD0 + T.r[0])));
				fl::movb(peekb(0x031D));
				tick(0x51D2, 42);
			}
		}
	}
	else
		tick(0x51D2, 12);                              // BNE
	const uint8_t now = peekb(0x031F);
	fl::tstb(now);
	tick(0x51D6, 24);
	if (!now)
	{
		tick(0x51D8, 12);
		jsr(0x51DC, 0xE238);                           // reset_skater_state
	}
	else
		tick(0x51DC, 12);                              // BNE
	rts();
}

// A7:4B3E: every 128 frames ask the sound CPU for its status (command 9); a bit of 0-2 that
// stays set from the last answer (0x01A3) marks it as failing (0x01A2 = 1)
uint16_t poll_sound_status(const Args &)
{
	c_save(1, 0x4B42);
	const bool skip = (peekb(0x01C4) & 0x7F) != 0;
	fl::logicb((uint8_t)(0x7F & peekb(0x01C4)));
	tick(0x4B48, 30);                                  // BITB #0x7F,@#0x01C4
	if (skip)
	{
		tick(0x4B82, 12);                              // BNE
		c_restore(1);
		return T.r[0];
	}
	tick(0x4B4A, 12);
	jsr(0x4B4E, 0x8E6A);                               // set_flag_01a4
	push(0x0009);
	fl::mov(0x0009);
	tick(0x4B52, 30);
	jsr(0x4B56, 0x8EA2);                               // send_sound_command
	jsr(0x4B5A, 0x8ED2);                               // read_sound_response
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0x4B5C, 12);
	jsr(0x4B60, 0x8F62);
	jsr(0x4B64, 0x8E72);                               // clear_flag_01a4
	T.r[0] = 0;
	fl::clr();
	tick(0x4B66, 12);
	T.r[0] = peekb(0x01A3);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x4B6A, 24);
	T.r[2] = T.r[1];
	fl::mov(T.r[2]);
	tick(0x4B6C, 12);
	T.r[2] = (uint16_t)~T.r[2];
	fl::com((uint16_t)(~T.r[2]));
	tick(0x4B6E, 12);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & (uint16_t)~T.r[2]));
	tick(0x4B70, 12);
	fl::logic((uint16_t)(0x0007 & T.r[0]));
	tick(0x4B74, 18);                                  // BIT #7,R0
	if (T.r[0] & 7)
	{
		tick(0x4B76, 12);
		pokeb(0x01A2, 1);
		fl::movb(0x01);
		tick(0x4B7C, 33);
	}
	else
		tick(0x4B7C, 12);                              // BEQ
	pokeb(0x01A3, (uint8_t)T.r[1]);
	fl::movb((uint8_t)T.r[1]);
	tick(0x4B80, 27);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	fl::tst(peek((uint16_t)(T.r[6] - 2)));
	tick(0x4B82, 18);                                  // TST (SP)+
	c_restore(1);
	return T.r[0];
}

// ---------------------------------------------------------------- zone switches while scrolling

// A5:454A (assembly): Skate City zone switches.  When the scroll position (0x01AA, 0x01A8)
// enters another 512-pixel row or column (last ones in 0x0F52/0x0F54), the zone bytes of the
// cells along the new edge (8 x 8 table at 0x0F60; row 0x0232, column 0x0234 within the
// cell) are checked with A5:43F8-44EC and their own conditions, until one switches the
// tile bank (F:EEE6 counts it in 0x0236).  The zone byte's mode (bits 0-2) picks the test
// by a jump table; the modes that jump into the middle of an instruction or outside the
// routine (not used by the game) do no test here.  Registers end as the original leaves
// them; the flags are those of the final ADD to SP.
void scan_zone_switches()
{
	const uint16_t E = T.r[6];
	const uint16_t L0 = (uint16_t)(E - 8), L2 = (uint16_t)(E - 6), L4 = (uint16_t)(E - 4), L6 = (uint16_t)(E - 2);
	auto zone_at = [](uint16_t i) -> uint8_t { return peekb((uint16_t)(0x0F60 + i)); };
	auto finish = [&]() {                              // 485E: ADD #8,SP; RTS
		T.r[6] = fl::add(T.r[6], 0x0008);
		tick(0x4862, 18);
		rts();
	};
	auto pop_finish = [&]() {                          // 485C: TST (SP)+
		T.r[6] = (uint16_t)(T.r[6] + 2);
		tick(0x485E, 18);
		finish();
	};
	auto times8 = [](uint16_t pc, uint16_t &r) {       // three ASL r at pc
		for (int i = 0; i < 3; i++)
		{
			r = (uint16_t)(r << 1);
			pc = (uint16_t)(pc + 2);
			tick(pc, 12);
		}
	};
	auto times8_l6 = [&](uint16_t pc) {                // three ASL 6(SP) at pc
		for (int i = 0; i < 3; i++)
		{
			poke(L6, (uint16_t)(peek(L6) << 1));
			pc = (uint16_t)(pc + 4);
			tick(pc, 30);
		}
	};
	T.r[6] = L0;
	fl::sub((uint16_t)(T.r[6] + 0x0008), 0x0008);
	tick(0x454E, 18);                                  // SUB #8,SP
	T.r[0] = peek(0x01AA);
	fl::mov(T.r[0]);
	tick(0x4552, 24);
	T.r[2] = T.r[0];
	fl::mov(T.r[2]);
	tick(0x4554, 12);
	T.r[2] = fl::logic((uint16_t)(T.r[2] & (uint16_t)~0x01FF));
	tick(0x4558, 18);
	T.r[5] = 0;
	fl::clr();
	tick(0x455A, 12);
	const bool same_row = T.r[2] == peek(0x0F52);
	fl::cmp(T.r[2], peek(0x0F52));
	tick(0x455E, 24);                                  // CMP R2,@#0x0F52
	if (!same_row)
	{
		tick(0x4560, 12);                              // BEQ
		T.r[5] = fl::inc(T.r[5]);
		tick(0x4562, 12);
		tick(0x4576, 12);                              // BR
	}
	else
	{
		tick(0x4564, 12);
		T.r[1] = peek(0x01A8);
		fl::mov(T.r[1]);
		tick(0x4568, 24);
		T.r[1] = fl::logic((uint16_t)(T.r[1] & (uint16_t)~0x01FF));
		tick(0x456C, 18);
		const bool same_col = T.r[1] == peek(0x0F54);
		fl::cmp(T.r[1], peek(0x0F54));
		tick(0x4570, 24);                              // CMP R1,@#0x0F54
		if (same_col)
		{
			tick(0x4572, 12);                          // BNE
			tick(0x485E, 18);                          // JMP
			finish();
			return;
		}
		tick(0x4576, 12);
	}
	poke(0x0236, 0);
	fl::clr();
	tick(0x457A, 27);
	T.r[4] = T.r[0];
	fl::mov(T.r[4]);
	tick(0x457C, 12);
	T.r[4] = fl::asr(T.r[4]);
	tick(0x457E, 12);
	T.r[4] &= 0xFF00;
	fl::clr();
	tick(0x4580, 12);                                  // CLRB R4
	T.r[4] = (uint16_t)(T.r[4] >> 8);
	tick(0x4582, 12);                                  // SWAB R4
	T.r[4] = sext8((uint8_t)T.r[4]);
	fl::movb((uint8_t)T.r[4]);
	tick(0x4584, 12);                                  // MOVB R4,R4
	poke(0x0232, T.r[4]);
	fl::mov(T.r[4]);
	tick(0x4588, 27);
	poke(0x0232, fl::logic((uint16_t)(peek(0x0232) & 0x0007)));
	tick(0x458E, 33);
	T.r[1] = T.r[4];
	fl::mov(T.r[1]);
	tick(0x4590, 12);
	for (uint16_t pc = 0x4592; pc <= 0x4596; pc = (uint16_t)(pc + 2))
	{
		T.r[1] = asr(T.r[1], 1);
		tick(pc, 12);
	}
	T.r[4] = T.r[1];
	tick(0x4598, 12);
	T.r[4] = fl::logic((uint16_t)(T.r[4] & 0x0007));
	tick(0x459C, 18);
	T.r[3] = peek(0x01A8);
	fl::mov(T.r[3]);
	tick(0x45A0, 24);
	T.r[3] = fl::asr(T.r[3]);
	tick(0x45A2, 12);
	T.r[3] &= 0xFF00;
	fl::clr();
	tick(0x45A4, 12);                                  // CLRB R3
	T.r[3] = (uint16_t)(T.r[3] >> 8);
	tick(0x45A6, 12);                                  // SWAB R3
	T.r[3] = sext8((uint8_t)T.r[3]);
	fl::movb((uint8_t)T.r[3]);
	tick(0x45A8, 12);                                  // MOVB R3,R3
	poke(0x0234, T.r[3]);
	fl::mov(T.r[3]);
	tick(0x45AC, 27);
	poke(0x0234, fl::logic((uint16_t)(peek(0x0234) & 0x000F)));
	tick(0x45B2, 33);
	T.r[1] = T.r[3];
	fl::mov(T.r[1]);
	tick(0x45B4, 12);
	uint16_t carry = 0;
	for (uint16_t pc = 0x45B6; pc <= 0x45BC; pc = (uint16_t)(pc + 2))
	{
		carry = T.r[1] & 1;
		T.r[1] = asr(T.r[1], 1);
		tick(pc, 12);
	}
	T.r[3] = T.r[1];
	tick(0x45BE, 12);
	T.r[3] = fl::logic((uint16_t)(T.r[3] & 0x0007));
	tick(0x45C2, 18);
	const bool row_changed = (T.r[5] & 1) != 0;
	T.r[5] = (uint16_t)((carry << 15) | (T.r[5] >> 1));
	tick(0x45C4, 12);                                  // ROR R5 (the last ASR's carry in)
	if (!row_changed)
	{
		tick(0x45C6, 12);                              // BCS
		tick(0x4704, 18);                              // JMP
	}
	else
	{
		tick(0x45CA, 12);
		poke(L2, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x45CE, 30);
		poke(L2, fl::sub(peek(L2), peek(0x0F52)));
		tick(0x45D4, 42);
		poke(0x0F52, T.r[2]);
		fl::mov(T.r[2]);
		tick(0x45D8, 27);
		poke(L0, T.r[3]);
		fl::mov(T.r[3]);
		tick(0x45DA, 21);
		poke(L0, fl::add(peek(L0), 4));
		tick(0x45DE, 27);
		const bool up = (int16_t)peek(L2) < 0;
		fl::tst(peek((uint16_t)(T.r[6] + 2)));
		tick(0x45E2, 27);                              // TST 2(SP)
		if (up)
		{
			tick(0x45E4, 12);                          // BGE
			poke(L6, T.r[4]);
			fl::mov(T.r[4]);
			tick(0x45E8, 30);
			times8_l6(0x45E8);
			T.r[0] = T.r[3];
			fl::mov(T.r[0]);
			tick(0x45F6, 12);
			T.r[0] = fl::add(T.r[0], peek(L6));
			tick(0x45FA, 27);
			push(0);
			fl::clr();
			tick(0x45FC, 24);
			pokeb(T.r[6], zone_at(T.r[0]));
			fl::movb(peekb(T.r[6]));
			tick(0x4600, 36);
			jsr(0x4604, 0x43F8);                       // zone_switch_a
			const bool sw = peek(0x0236) != 0;
			fl::tst(peek(0x0236));
			tick(0x4608, 24);                          // TST @#0x0236
			if (sw)
			{
				tick(0x4660, 12);                      // BNE
				tick(0x485C, 18);                      // JMP
				pop_finish();
				return;
			}
			tick(0x460A, 12);
			T.r[2] = peek(L6);
			fl::mov(T.r[2]);
			tick(0x460E, 27);
			T.r[1] = 3;
			fl::mov(T.r[1]);
			tick(0x4612, 18);
			// modes 0-7: no test (0x463C), switch (0x4636), test 0x0232 < 7 (0x462E)
			static const uint16_t to[8] = {0x463C, 0x4636, 0x4636, 0x462E, 0x462E, 0x4636, 0x463C, 0x462E};
			for (;;)
			{
				T.r[0] = T.r[1];
				fl::mov(T.r[0]);
				tick(0x4614, 12);
				T.r[0] = fl::add(T.r[0], T.r[3]);
				tick(0x4616, 12);
				T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0007));
				tick(0x461A, 18);
				T.r[0] = fl::add(T.r[0], T.r[2]);
				tick(0x461C, 12);
				T.r[5] = 0;
				fl::clr();
				tick(0x461E, 12);
				T.r[5] = zone_at(T.r[0]);
				fl::logicb((uint8_t)T.r[5]);
				tick(0x4622, 27);
				T.r[0] = T.r[5];
				fl::mov(T.r[0]);
				tick(0x4624, 12);
				T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0007));
				tick(0x4628, 18);
				T.r[0] = fl::asl(T.r[0]);
				tick(0x462A, 12);
				uint16_t at = to[T.r[0] >> 1];
				tick(at, 27);                          // ADD 0x4864(R0),PC
				if (at == 0x462E)
				{
					const bool inside = (int16_t)peek(0x0232) < 7;
					fl::cmp(peek(0x0232), 0x0007);
					tick(0x4634, 30);                  // CMP @#0x0232,#7
					at = inside ? 0x4636 : 0x463C;
					tick(at, 12);                      // BGE
				}
				if (at == 0x4636)
				{
					poke(T.r[6], T.r[5]);
					fl::mov(T.r[5]);
					tick(0x4638, 21);
					jsr(0x463C, 0xEEE6);               // set_tile_bank
				}
				const bool done = peek(0x0236) != 0;
				fl::tst(peek(0x0236));
				tick(0x4640, 24);                      // TST @#0x0236
				if (done)
				{
					tick(0x4660, 12);                  // BNE
					tick(0x485C, 18);                  // JMP
					pop_finish();
					return;
				}
				tick(0x4642, 12);
				if (!--T.r[1])
				{
					tick(0x4644, 18);                  // SOB
					break;
				}
				tick(0x4612, 18);
			}
			T.r[0] = peek(L0);
			fl::mov(T.r[0]);
			tick(0x4648, 27);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0007));
			tick(0x464C, 18);
			T.r[0] = fl::add(T.r[0], peek(L6));
			tick(0x4650, 27);
			poke(T.r[6], 0);
			fl::clr();
			tick(0x4652, 21);
			pokeb(T.r[6], zone_at(T.r[0]));
			fl::movb(peekb(T.r[6]));
			tick(0x4656, 36);
			jsr(0x465A, 0x4442);                       // zone_switch_b
		}
		else
		{
			tick(0x4664, 12);                          // BGE
			const bool inner = peek(0x0232) != 0;
			fl::tst(peek(0x0232));
			tick(0x4668, 24);                          // TST @#0x0232
			if (inner)
			{
				tick(0x4670, 12);                      // BNE
				T.r[0] = 0x0006;
				fl::mov(T.r[0]);
				tick(0x4674, 18);
			}
			else
			{
				tick(0x466A, 12);
				T.r[0] = 0x0005;
				fl::mov(T.r[0]);
				tick(0x466E, 18);
				tick(0x4674, 12);                      // BR
			}
			T.r[0] = fl::add(T.r[0], T.r[4]);
			tick(0x4676, 12);
			T.r[1] = T.r[0];
			fl::mov(T.r[1]);
			tick(0x4678, 12);
			T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x0007));
			tick(0x467C, 18);
			poke(L6, T.r[1]);
			fl::mov(T.r[1]);
			tick(0x4680, 30);
			times8_l6(0x4680);
			T.r[0] = T.r[3];
			fl::mov(T.r[0]);
			tick(0x468E, 12);
			T.r[0] = fl::add(T.r[0], peek(L6));
			tick(0x4692, 27);
			push(0);
			fl::clr();
			tick(0x4694, 24);
			pokeb(T.r[6], zone_at(T.r[0]));
			fl::movb(peekb(T.r[6]));
			tick(0x4698, 36);
			jsr(0x469C, 0x4496);                       // zone_switch_c
			const bool sw = peek(0x0236) != 0;
			fl::tst(peek(0x0236));
			tick(0x46A0, 24);                          // TST @#0x0236
			if (sw)
			{
				tick(0x4660, 12);                      // BNE
				tick(0x485C, 18);                      // JMP
				pop_finish();
				return;
			}
			tick(0x46A2, 12);
			T.r[2] = peek(L6);
			fl::mov(T.r[2]);
			tick(0x46A6, 27);
			poke(L4, 0x0001);
			fl::mov(0x0001);
			tick(0x46AC, 36);
			// modes: no test (0x46D8), test 0x0232 != 1 (0x46CA), switch (0x46D2); mode 6
			// would enter the instruction at 0x46D8 and is taken as no test
			static const uint16_t to[8] = {0x46D8, 0x46CA, 0x46CA, 0x46D2, 0x46D2, 0x46D2, 0x46D8, 0x46D2};
			for (;;)
			{
				T.r[0] = T.r[3];
				fl::mov(T.r[0]);
				tick(0x46AE, 12);
				T.r[0] = fl::add(T.r[0], peek(L4));
				tick(0x46B2, 27);
				T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0007));
				tick(0x46B6, 18);
				T.r[0] = fl::add(T.r[0], T.r[2]);
				tick(0x46B8, 12);
				T.r[5] = 0;
				fl::clr();
				tick(0x46BA, 12);
				T.r[5] = zone_at(T.r[0]);
				fl::logicb((uint8_t)T.r[5]);
				tick(0x46BE, 27);
				T.r[0] = T.r[5];
				fl::mov(T.r[0]);
				tick(0x46C0, 12);
				T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0007));
				tick(0x46C4, 18);
				T.r[0] = fl::asl(T.r[0]);
				tick(0x46C6, 12);
				uint16_t at = to[T.r[0] >> 1];
				tick(at, 27);                          // ADD 0x4870(R0),PC
				if (at == 0x46CA)
				{
					const bool one = peek(0x0232) == 1;
					fl::cmp(peek(0x0232), 0x0001);
					tick(0x46D0, 30);                  // CMP @#0x0232,#1
					at = one ? 0x46D8 : 0x46D2;
					tick(at, 12);                      // BEQ
				}
				if (at == 0x46D2)
				{
					poke(T.r[6], T.r[5]);
					tick(0x46D4, 21);
					jsr(0x46D8, 0xEEE6);               // set_tile_bank
				}
				const bool done = peek(0x0236) != 0;
				fl::tst(peek(0x0236));
				tick(0x46DC, 24);                      // TST @#0x0236
				if (done)
				{
					tick(0x4660, 12);                  // BNE
					tick(0x485C, 18);                  // JMP
					pop_finish();
					return;
				}
				tick(0x46DE, 12);
				poke(L4, fl::inc(peek(L4)));
				tick(0x46E2, 30);
				const bool more = (int16_t)peek(L4) <= 3;
				fl::cmp(peek((uint16_t)(T.r[6] + 6)), 0x0003);
				tick(0x46E8, 33);                      // CMP 6(SP),#3
				if (!more)
				{
					tick(0x46EA, 12);                  // BLE
					break;
				}
				tick(0x46AC, 12);
			}
			T.r[0] = peek(L0);
			tick(0x46EE, 27);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0007));
			tick(0x46F2, 18);
			T.r[0] = fl::add(T.r[0], peek(L6));
			tick(0x46F6, 27);
			poke(T.r[6], 0);
			fl::clr();
			tick(0x46F8, 21);
			pokeb(T.r[6], zone_at(T.r[0]));
			fl::movb(peekb(T.r[6]));
			tick(0x46FC, 36);
			jsr(0x4700, 0x44EC);                       // zone_switch_d
			tick(0x465A, 12);                          // BR
		}
		const bool sw = peek(0x0236) != 0;
		fl::tst(peek(0x0236));
		tick(0x465E, 24);                              // TST @#0x0236
		if (sw)
		{
			tick(0x4660, 12);                          // BEQ
			tick(0x485C, 18);                          // JMP
			pop_finish();
			return;
		}
		tick(0x4702, 12);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(peek((uint16_t)(T.r[6] - 2)));
		tick(0x4704, 18);                              // TST (SP)+
	}
	T.r[0] = peek(0x01A8);
	fl::mov(T.r[0]);
	tick(0x4708, 24);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & (uint16_t)~0x01FF));
	tick(0x470C, 18);
	const bool same_col = T.r[0] == peek(0x0F54);
	fl::cmp(T.r[0], peek(0x0F54));
	tick(0x4710, 24);                                  // CMP R0,@#0x0F54
	if (same_col)
	{
		tick(0x4712, 12);                              // BNE
		tick(0x485E, 18);                              // JMP
		finish();
		return;
	}
	tick(0x4716, 12);
	T.r[5] = peek(0x01A8);
	fl::mov(T.r[5]);
	tick(0x471A, 24);
	T.r[5] = fl::sub(T.r[5], peek(0x0F54));
	tick(0x471E, 24);
	poke(0x0F54, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x4722, 27);
	poke(L6, T.r[4]);
	fl::mov(T.r[4]);
	tick(0x4726, 30);
	times8_l6(0x4726);
	fl::tst(T.r[5]);
	tick(0x4734, 12);                                  // TST R5
	if (T.r[5] & 0x8000)
	{
		tick(0x4736, 12);                              // BGE
		const bool down = (int16_t)peek(L2) > 0;
		fl::tst(peek((uint16_t)(T.r[6] + 2)));
		tick(0x473A, 27);                              // TST 2(SP)
		if (down)
			tick(0x4752, 12);                          // BGT
		else
		{
			tick(0x473C, 12);
			T.r[0] = T.r[4];
			fl::mov(T.r[0]);
			tick(0x473E, 12);
			times8(0x473E, T.r[0]);
			T.r[0] = fl::add(T.r[0], T.r[3]);
			tick(0x4746, 12);
			push(0);
			fl::clr();
			tick(0x4748, 24);
			pokeb(T.r[6], zone_at(T.r[0]));
			fl::movb(peekb(T.r[6]));
			tick(0x474C, 36);
			jsr(0x4750, 0x43F8);                       // zone_switch_a
			T.r[6] = (uint16_t)(T.r[6] + 2);
			fl::tst(peek((uint16_t)(T.r[6] - 2)));
			tick(0x4752, 18);                          // TST (SP)+
		}
		T.r[2] = T.r[3];
		fl::mov(T.r[2]);
		tick(0x4754, 12);
		T.r[2] = fl::add(T.r[2], peek(L6));
		tick(0x4758, 27);
		poke(L4, 0x0005);
		fl::mov(0x0005);
		tick(0x475E, 36);
		// modes: no test (0x478C), switch (0x4784), test 0x0234 < 14 (0x477C); mode 6
		// skips the test of 0x0236 (0x4792)
		static const uint16_t to[8] = {0x478C, 0x4784, 0x477C, 0x4784, 0x477C, 0x4784, 0x4792, 0x477C};
		for (;;)
		{
			T.r[0] = T.r[2];
			fl::mov(T.r[0]);
			tick(0x4760, 12);
			T.r[0] = fl::add(T.r[0], 8);
			tick(0x4764, 18);
			T.r[2] = T.r[0];
			fl::mov(T.r[2]);
			tick(0x4766, 12);
			T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x003F));
			tick(0x476A, 18);
			T.r[5] = 0;
			fl::clr();
			tick(0x476C, 12);
			T.r[5] = zone_at(T.r[2]);
			fl::logicb((uint8_t)T.r[5]);
			tick(0x4770, 27);
			T.r[0] = T.r[5];
			fl::mov(T.r[0]);
			tick(0x4772, 12);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0007));
			tick(0x4776, 18);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x4778, 12);
			uint16_t at = to[T.r[0] >> 1];
			tick(at, 27);                              // ADD 0x487C(R0),PC
			if (at == 0x477C)
			{
				const bool inside = (int16_t)peek(0x0234) < 14;
				fl::cmp(peek(0x0234), 0x000E);
				tick(0x4782, 30);                      // CMP @#0x0234,#14
				at = inside ? 0x4784 : 0x478C;
				tick(at, 12);                          // BGE
			}
			if (at == 0x4784)
			{
				push(T.r[5]);
				fl::mov(T.r[5]);
				tick(0x4786, 24);
				jsr(0x478A, 0xEEE6);                   // set_tile_bank
				T.r[6] = (uint16_t)(T.r[6] + 2);
				fl::tst(peek((uint16_t)(T.r[6] - 2)));
				tick(0x478C, 18);                      // TST (SP)+
				at = 0x478C;
			}
			if (at == 0x478C)
			{
				const bool done = peek(0x0236) != 0;
				fl::tst(peek(0x0236));
				tick(0x4790, 24);                      // TST @#0x0236
				if (done)
				{
					tick(0x485E, 12);                  // BNE
					finish();
					return;
				}
				tick(0x4792, 12);
			}
			poke(L4, (uint16_t)(peek(L4) - 1));
			const bool more = peek(L4) != 0;
			fl::dec((uint16_t)(peek((uint16_t)(T.r[6] + 4)) + 1));
			tick(0x4796, 30);                          // DEC 4(SP)
			if (!more)
			{
				tick(0x4798, 12);                      // BNE
				break;
			}
			tick(0x475E, 12);
		}
		const bool back = (int16_t)peek(L2) < 0;
		fl::tst(peek((uint16_t)(T.r[6] + 2)));
		tick(0x479C, 27);                              // TST 2(SP)
		if (back)
		{
			tick(0x485E, 12);                          // BLT
			finish();
			return;
		}
		tick(0x479E, 12);
		T.r[0] = T.r[4];
		fl::mov(T.r[0]);
		tick(0x47A0, 12);
		T.r[0] = fl::add(T.r[0], 6);
		tick(0x47A4, 18);
		times8(0x47A4, T.r[0]);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0038));
		tick(0x47AE, 18);
		T.r[0] = fl::add(T.r[0], T.r[3]);
		tick(0x47B0, 12);
		push(0);
		fl::clr();
		tick(0x47B2, 24);
		pokeb(T.r[6], zone_at(T.r[0]));
		fl::movb(peekb(T.r[6]));
		tick(0x47B6, 36);
		jsr(0x47BA, 0x4496);                           // zone_switch_c
		tick(0x485C, 12);                              // BR
		pop_finish();
		return;
	}
	tick(0x47BC, 12);                                  // BGE
	const bool inner = peek(0x0234) != 0;
	fl::tst(peek(0x0234));
	tick(0x47C0, 24);                                  // TST @#0x0234
	if (inner)
	{
		tick(0x47C8, 12);                              // BNE
		T.r[0] = 0x0004;
		fl::mov(T.r[0]);
		tick(0x47CC, 18);
	}
	else
	{
		tick(0x47C2, 12);
		T.r[0] = 0x0003;
		fl::mov(T.r[0]);
		tick(0x47C6, 18);
		tick(0x47CC, 12);                              // BR
	}
	T.r[0] = fl::add(T.r[0], T.r[3]);
	tick(0x47CE, 12);
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0x47D0, 12);
	T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x0007));
	tick(0x47D4, 18);
	const bool down = (int16_t)peek(L2) > 0;
	fl::tst(peek((uint16_t)(T.r[6] + 2)));
	tick(0x47D8, 27);                                  // TST 2(SP)
	if (down)
		tick(0x47F6, 12);                              // BGT
	else
	{
		tick(0x47DA, 12);
		T.r[0] = T.r[4];
		fl::mov(T.r[0]);
		tick(0x47DC, 12);
		times8(0x47DC, T.r[0]);
		T.r[0] = fl::add(T.r[0], T.r[1]);
		tick(0x47E4, 12);
		push(0);
		fl::clr();
		tick(0x47E6, 24);
		pokeb(T.r[6], zone_at(T.r[0]));
		fl::movb(peekb(T.r[6]));
		tick(0x47EA, 36);
		jsr(0x47EE, 0x4442);                           // zone_switch_b
		const bool sw = peek(0x0236) != 0;
		fl::tst(peek(0x0236));
		tick(0x47F2, 24);                              // TST @#0x0236
		if (sw)
		{
			tick(0x485C, 12);                          // BNE
			pop_finish();
			return;
		}
		tick(0x47F4, 12);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(peek((uint16_t)(T.r[6] - 2)));
		tick(0x47F6, 18);                              // TST (SP)+
	}
	T.r[2] = T.r[1];
	fl::mov(T.r[2]);
	tick(0x47F8, 12);
	T.r[2] = fl::add(T.r[2], peek(L6));
	tick(0x47FC, 27);
	T.r[3] = 0x0005;
	fl::mov(T.r[3]);
	tick(0x4800, 18);
	// modes: no test (0x4834), test 0x0234 > 2 or 0 (0x481E), switch (0x482C); modes 6 and 7
	// would jump outside the routine and are taken as no test
	static const uint16_t to[8] = {0x4834, 0x481E, 0x482C, 0x481E, 0x482C, 0x482C, 0x4834, 0x4834};
	for (;;)
	{
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0x4802, 12);
		T.r[0] = fl::add(T.r[0], 8);
		tick(0x4806, 18);
		T.r[2] = T.r[0];
		fl::mov(T.r[2]);
		tick(0x4808, 12);
		T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x003F));
		tick(0x480C, 18);
		T.r[5] = 0;
		fl::clr();
		tick(0x480E, 12);
		T.r[5] = zone_at(T.r[2]);
		fl::logicb((uint8_t)T.r[5]);
		tick(0x4812, 27);
		T.r[0] = T.r[5];
		fl::mov(T.r[0]);
		tick(0x4814, 12);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0007));
		tick(0x4818, 18);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x481A, 12);
		uint16_t at = to[T.r[0] >> 1];
		tick(at, 27);                                  // ADD 0x4888(R0),PC
		if (at == 0x481E)
		{
			const bool far = (int16_t)peek(0x0234) > 2;
			fl::cmp(peek(0x0234), 0x0002);
			tick(0x4824, 30);                          // CMP @#0x0234,#2
			if (far)
			{
				tick(0x482C, 12);                      // BGT
				at = 0x482C;
			}
			else
			{
				tick(0x4826, 12);
				const bool nz = peek(0x0234) != 0;
				fl::tst(peek(0x0234));
				tick(0x482A, 24);                      // TST @#0x0234
				at = nz ? 0x4834 : 0x482C;
				tick(at, 12);                          // BNE
			}
		}
		if (at == 0x482C)
		{
			push(T.r[5]);
			fl::mov(T.r[5]);
			tick(0x482E, 24);
			jsr(0x4832, 0xEEE6);                       // set_tile_bank
			T.r[6] = (uint16_t)(T.r[6] + 2);
			fl::tst(peek((uint16_t)(T.r[6] - 2)));
			tick(0x4834, 18);                          // TST (SP)+
		}
		const bool done = peek(0x0236) != 0;
		fl::tst(peek(0x0236));
		tick(0x4838, 24);                              // TST @#0x0236
		if (done)
		{
			tick(0x485E, 12);                          // BNE
			finish();
			return;
		}
		tick(0x483A, 12);
		if (!--T.r[3])
		{
			tick(0x483C, 18);                          // SOB
			break;
		}
		tick(0x4800, 18);
	}
	const bool back = (int16_t)peek(L2) < 0;
	fl::tst(peek((uint16_t)(T.r[6] + 2)));
	tick(0x4840, 27);                                  // TST 2(SP)
	if (back)
	{
		tick(0x485E, 12);                              // BLT
		finish();
		return;
	}
	tick(0x4842, 12);
	T.r[4] = fl::add(T.r[4], 6);
	tick(0x4846, 18);
	times8(0x4846, T.r[4]);
	T.r[4] = fl::logic((uint16_t)(T.r[4] & 0x0038));
	tick(0x4850, 18);
	T.r[4] = fl::add(T.r[4], T.r[1]);
	tick(0x4852, 12);
	push(0);
	fl::clr();
	tick(0x4854, 24);
	pokeb(T.r[6], zone_at(T.r[4]));
	fl::movb(peekb(T.r[6]));
	tick(0x4858, 36);
	jsr(0x485C, 0x44EC);                               // zone_switch_d
	pop_finish();
}

// ---------------------------------------------------------------- terrain events

// A7:538E: the terrain event 0x032A (1-25, jump table at 0x55AC) the skater has reached:
// shops and park entrances (1-8, 20-23), jumps (11/18: sound and F:B602 into the air),
// tickets (17), flags 0x0F27/0x0F28 (9/10), 0x0ED4 (13/14) and others.  Values above 25 jump
// into the next routine in the original and are not used; they do nothing here.  R1 is
// preserved; R0 is returned as the original leaves it.
uint16_t terrain_event(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x5390, 24);
	T.r[0] = sext8(peekb(0x032A));
	fl::movb((uint8_t)T.r[0]);
	tick(0x5394, 24);
	auto finish = [&]() {                              // 55A8: MOV (SP)+,R1; RTS
		T.r[1] = pop();
		fl::mov(T.r[1]);
		tick(0x55AA, 18);
		rts();
		return T.r[0];
	};
	if ((int16_t)T.r[0] <= 0)
	{
		tick(0x548C, 12);                              // BLE
		tick(0x5580, 12);                              // BR
		tick(0x55A8, 12);                              // BR
		return finish();
	}
	tick(0x5396, 12);
	const uint16_t ev = T.r[0];
	T.r[0] = fl::dec(T.r[0]);
	tick(0x5398, 12);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x539A, 12);
	static const uint16_t cases[25] = {
		0x539E, 0x539E, 0x539E, 0x539E, 0x5460, 0x5460, 0x5460, 0x5460, 0x547E, 0x5486,
		0x548E, 0x55A8, 0x54FA, 0x5502, 0x55A8, 0x550A, 0x5522, 0x548E, 0x555A, 0x5582,
		0x5582, 0x5582, 0x5582, 0x55A8, 0x55A8,
	};
	tick(ev <= 25 ? cases[ev - 1] : 0x55A8, 27);       // ADD 0x55AC(R0),PC
	switch (ev)
	{
	case 1: case 2: case 3: case 4:                    // park entrances
	{
		jsr(0x53A2, 0x5360);                           // park_at_gate
		pokeb(0x027A, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x53A6, 27);
		T.r[1] = 0;
		fl::clr();
		tick(0x53A8, 12);
		T.r[1] = (uint8_t)T.r[0];
		fl::logicb((uint8_t)T.r[1]);
		tick(0x53AA, 12);
		const bool open = (peekb(0x0EA4) & rb((uint16_t)(0xFC74 + T.r[1]))) != 0;
		fl::logicb((uint8_t)(peekb(0x0EA4) & peekb((uint16_t)(T.r[1] + 0xFC74))));
		tick(0x53B0, 39);                              // BITB @#0x0EA4,0xFC74(R1)
		if (open)
		{
			tick(0x548C, 12);                          // BNE
			tick(0x5580, 12);
			tick(0x55A8, 12);
			break;
		}
		tick(0x53B2, 12);
		T.r[0] = peek(0x0ED6);
		fl::mov(T.r[0]);
		tick(0x53B6, 24);
		T.r[0] |= peekb(0x0F29);
		fl::logicb((uint8_t)T.r[0]);
		tick(0x53BA, 24);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
		tick(0x53BE, 18);
		const bool done = (T.r[0] & rb((uint16_t)(0xFC74 + T.r[1]))) != 0;
		fl::logicb((uint8_t)((uint8_t)T.r[0] & peekb((uint16_t)(T.r[1] + 0xFC74))));
		tick(0x53C2, 27);                              // BITB R0,0xFC74(R1)
		if (done)
		{
			tick(0x548C, 12);                          // BNE
			tick(0x5580, 12);
			tick(0x55A8, 12);
			break;
		}
		tick(0x53C4, 12);
		bool other_park = false;
		const bool two = peekb(0x01F6) != 0;
		fl::tstb(peekb(0x01F6));
		tick(0x53C8, 24);                              // TSTB @#0x01F6
		if (two)
		{
			tick(0x53CA, 12);
			const bool one = peekb(0x0F2B) == 1;
			fl::cmpb(peekb(0x0F2B), 0x01);
			tick(0x53D0, 30);                          // CMPB @#0x0F2B,#1
			if (one)
			{
				tick(0x53D2, 12);
				T.r[0] = 0;
				fl::clr();
				tick(0x53D4, 12);
				T.r[0] = peekb(0x02B8);
				fl::logicb((uint8_t)T.r[0]);
				tick(0x53D8, 24);
				fl::cmp(T.r[1], T.r[0]);
				tick(0x53DA, 12);                      // CMP R1,R0
				other_park = T.r[1] != T.r[0];
				tick(other_park ? 0x53DC : 0x53F0, 12); // BEQ
			}
			else
				tick(0x53F0, 12);                      // BNE
		}
		else
			tick(0x53F0, 12);                          // BEQ
		if (other_park)
		{
			push(0x75F4);
			tick(0x53E0, 30);
			push(0x00B4);
			fl::mov(0x00B4);
			tick(0x53E4, 30);
			push(0x0001);
			fl::mov(0x0001);
			tick(0x53E8, 30);
			jsr(0x53EC, 0xA974);                       // message
			T.r[6] = (uint16_t)(T.r[6] + 2);
			fl::tst(peek((uint16_t)(T.r[6] - 2)));
			tick(0x53EE, 18);                          // TST (SP)+
			tick(0x5450, 12);                          // BR
		}
		else
		{
			const bool tickets = peekb(0x025B) != 0;
			fl::tstb(peekb(0x025B));
			tick(0x53F4, 24);                          // TSTB @#0x025B
			if (!tickets)
			{
				tick(0x53F6, 12);
				pokeb(0x0EA6, 1);
				fl::movb(0x01);
				tick(0x53FC, 33);
				poke(0x0EAA, 0x00B4);
				fl::mov(0x00B4);
				tick(0x5402, 33);
				push(0x0004);
				fl::mov(0x0004);
				tick(0x5406, 30);
				poke(T.r[6], fl::add(peek(T.r[6]), T.r[1]));
				tick(0x5408, 21);
				push(0x0001);
				fl::mov(0x0001);
				tick(0x540C, 30);
				jsr(0x5410, 0xD97C);
				jsr(0x5414, 0x9152);                   // next_random
				T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0003));
				tick(0x5418, 18);
				poke(T.r[6], 0);
				fl::clr();
				tick(0x541A, 21);
				pokeb(T.r[6], rb((uint16_t)(0xFE2A + T.r[0])));
				fl::movb(peekb(T.r[6]));
				tick(0x541E, 36);
				jsr(0x5422, 0x8E78);                   // queue_sound
				tick(0x5450, 12);                      // BR
			}
			else
			{
				tick(0x5424, 12);                      // BNE
				push(0xFFFF);
				fl::mov(0xFFFF);
				tick(0x5428, 30);
				jsr(0x542C, 0x72CA);
				pokeb(0x0279, fl::logicb((uint8_t)(peekb(0x0279) | 1)));
				tick(0x5432, 33);
				poke(T.r[6], 0);
				fl::clr();
				tick(0x5434, 21);
				pokeb(T.r[6], peekb(0x027A));
				fl::movb(peekb(T.r[6]));
				tick(0x5438, 33);
				poke(T.r[6], fl::add(peek(T.r[6]), 4));
				tick(0x543C, 27);
				push(0);
				fl::clr();
				tick(0x543E, 24);
				jsr(0x5442, 0xD97C);
				poke(T.r[6], 0);
				fl::clr();
				tick(0x5444, 21);
				pokeb(T.r[6], peekb(0x027A));
				fl::movb(peekb(T.r[6]));
				tick(0x5448, 33);
				jsr(0x544C, 0xDA02);
				jsr(0x5450, 0x50C8);
			}
		}
		T.r[0] = 0;
		fl::clr();
		tick(0x5452, 12);
		T.r[0] = peekb(0x027A);
		fl::logicb((uint8_t)T.r[0]);
		tick(0x5456, 24);
		pokeb(0x0ED6, fl::logicb((uint8_t)(peekb(0x0ED6) | rb((uint16_t)(0xFC74 + T.r[0])))));
		tick(0x545C, 42);
		T.r[6] = (uint16_t)(T.r[6] + 4);
		fl::cmp(peek((uint16_t)(T.r[6] - 4)), peek((uint16_t)(T.r[6] - 2)));
		tick(0x545E, 24);                              // CMP (SP)+,(SP)+
		tick(0x5558, 12);                              // BR
		tick(0x55A8, 12);                              // BR
		break;
	}
	case 5: case 6: case 7: case 8:                    // shops
	{
		T.r[1] = sext8(peekb(0x0ED7));
		fl::movb((uint8_t)T.r[1]);
		tick(0x5464, 24);
		T.r[1] |= peekb(0x0ED8);
		fl::logicb((uint8_t)T.r[1]);
		tick(0x5468, 24);
		T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x00FF));
		tick(0x546C, 18);
		jsr(0x5470, 0x5326);                           // park_at_area
		const bool bought = (T.r[1] & rb((uint16_t)(0xFC74 + T.r[0]))) != 0;
		fl::logicb((uint8_t)((uint8_t)T.r[1] & peekb((uint16_t)(T.r[0] + 0xFC74))));
		tick(0x5474, 27);                              // BITB R1,0xFC74(R0)
		if (bought)
			tick(0x5558, 12);                          // BNE
		else
		{
			tick(0x5476, 12);
			pokeb(0x0ED7, fl::logicb((uint8_t)(peekb(0x0ED7) | 0x10)));
			tick(0x547C, 33);
			tick(0x5558, 12);                          // BR
		}
		tick(0x55A8, 12);                              // BR
		break;
	}
	case 9:
		pokeb(0x0F27, 1);
		tick(0x5484, 33);
		tick(0x5580, 12);                              // BR
		tick(0x55A8, 12);                              // BR
		break;
	case 10:
		pokeb(0x0F28, 1);
		tick(0x548C, 33);
		tick(0x5580, 12);                              // BR
		tick(0x55A8, 12);                              // BR
		break;
	case 11: case 18:                                  // jumps
	{
		const bool air = peekb(0x01EA) != 0;
		fl::tstb(peekb(0x01EA));
		tick(0x5492, 24);                              // TSTB @#0x01EA
		if (!air)
		{
			tick(0x5494, 12);
			poke(0x0FB2, fl::inc(peek(0x0FB2)));
			tick(0x5498, 27);
			tick(0x54F4, 12);                          // BR
		}
		else
		{
			tick(0x549A, 12);                          // BNE
			const bool state5 = peekb(0x031F) == 5;
			fl::cmpb(peekb(0x031F), 0x05);
			tick(0x54A0, 30);                          // CMPB @#0x031F,#5
			if (state5)
				tick(0x54F4, 12);                      // BEQ
			else
			{
				tick(0x54A2, 12);
				push(0x0053);
				fl::mov(0x0053);
				tick(0x54A6, 30);
				jsr(0x54AA, 0x8E78);                   // queue_sound
				T.r[0] = 0x1C00;
				fl::mov(T.r[0]);
				tick(0x54AE, 18);
				poke(0x01D4, T.r[0]);
				fl::mov(T.r[0]);
				tick(0x54B2, 27);
				poke(0x1402, T.r[0]);
				fl::mov(T.r[0]);
				tick(0x54B6, 27);
				jsr(0x54BA, 0x78E4);
				const bool high = peekb(0x032A) == 0x12;
				fl::cmpb(peekb(0x032A), 0x12);
				tick(0x54C0, 30);                      // CMPB @#0x032A,#0x12
				if (high)
				{
					tick(0x54C2, 12);
					poke(T.r[6], 0x0069);
					fl::mov(0x0069);
					tick(0x54C6, 27);
					jsr(0x54CA, 0x8E78);
					poke(0x03B4, 0x57A8);
					fl::mov(0x57A8);
					tick(0x54D0, 33);
					tick(0x54E0, 12);                  // BR
				}
				else
				{
					tick(0x54D2, 12);                  // BNE
					poke(T.r[6], 0x006A);
					fl::mov(0x006A);
					tick(0x54D6, 27);
					jsr(0x54DA, 0x8E78);
					poke(0x03B4, 0x57AA);
					fl::mov(0x57AA);
					tick(0x54E0, 33);
				}
				poke(0x03B2, 0x50FA);
				fl::mov(0x50FA);
				tick(0x54E6, 33);
				poke(T.r[6], 0x0002);
				fl::mov(0x0002);
				tick(0x54EA, 27);
				push(0x0012);
				fl::mov(0x0012);
				tick(0x54EE, 30);
				jsr(0x54F2, 0xB602);
				T.r[6] = (uint16_t)(T.r[6] + 4);
				fl::cmp(peek((uint16_t)(T.r[6] - 4)), peek((uint16_t)(T.r[6] - 2)));
				tick(0x54F4, 24);                      // CMP (SP)+,(SP)+
			}
		}
		pokeb(0x032A, 0);
		fl::clr();
		tick(0x54F8, 27);
		tick(0x55A8, 12);                              // BR
		break;
	}
	case 13:
		poke(0x0ED4, 0x7800);
		tick(0x5500, 33);
		tick(0x55A8, 12);                              // BR
		break;
	case 14:
		poke(0x0ED4, 0x5800);
		tick(0x5508, 33);
		tick(0x55A8, 12);                              // BR
		break;
	case 16:
	{
		T.r[0] = peek(0x0F32);
		fl::mov(T.r[0]);
		tick(0x550E, 24);
		T.r[0] = fl::inc(T.r[0]);
		tick(0x5510, 12);
		const bool beyond = (int16_t)T.r[0] >= (int16_t)peek(0x099A);
		fl::cmp(T.r[0], peek(0x099A));
		tick(0x5514, 24);                              // CMP R0,@#0x099A
		if (beyond)
		{
			tick(0x55A8, 12);                          // BGE
			break;
		}
		tick(0x5516, 12);
		poke(0x0F32, peek(0x099A));
		fl::mov(peek(0x0F32));
		tick(0x551C, 39);
		jsr(0x5520, 0x7A9E);
		tick(0x55A8, 12);                              // BR
		break;
	}
	case 17:                                           // tickets
	{
		const bool air = peekb(0x01EA) != 0;
		fl::tstb(peekb(0x01EA));
		tick(0x5526, 24);                              // TSTB @#0x01EA
		if (!air)
		{
			tick(0x55A8, 12);                          // BEQ
			break;
		}
		tick(0x5528, 12);
		const bool late = peekb(0x018F) > 1;
		fl::cmpb(peekb(0x018F), 0x01);
		tick(0x552E, 30);                              // CMPB @#0x018F,#1
		if (late)
		{
			tick(0x55A8, 12);                          // BHI
			break;
		}
		tick(0x5530, 12);
		jsr(0x5534, 0x52FC);                           // difficulty_step
		T.r[0] = sext8(rb((uint16_t)(0xFC74 + T.r[0])));
		fl::movb((uint8_t)T.r[0]);
		tick(0x5538, 27);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
		tick(0x553C, 18);
		const bool have = (peekb(0x0F2A) & T.r[0]) != 0;
		fl::logicb((uint8_t)(peekb(0x0F2A) & (uint8_t)T.r[0]));
		tick(0x5540, 24);                              // BITB @#0x0F2A,R0
		if (have)
		{
			tick(0x55A8, 12);                          // BNE
			break;
		}
		tick(0x5542, 12);
		pokeb(0x0F36, 1);
		fl::movb(0x01);
		tick(0x5548, 33);
		T.r[1] = sext8(peekb(0x0F2A));
		fl::movb((uint8_t)T.r[1]);
		tick(0x554C, 24);
		T.r[1] = fl::logic((uint16_t)(T.r[1] | T.r[0]));
		tick(0x554E, 12);
		pokeb(0x0F2A, (uint8_t)T.r[1]);
		fl::movb((uint8_t)T.r[1]);
		tick(0x5552, 27);
		pokeb(0x0F2A, fl::logicb((uint8_t)(peekb(0x0F2A) | 0x80)));
		tick(0x5558, 33);
		tick(0x55A8, 12);                              // BR
		break;
	}
	case 19:
	{
		const bool air = peekb(0x01EA) != 0;
		fl::tstb(peekb(0x01EA));
		tick(0x555E, 24);                              // TSTB @#0x01EA
		if (!air)
		{
			tick(0x55A8, 12);                          // BEQ
			break;
		}
		tick(0x5560, 12);
		const bool phase = peekb(0x0F26) >= 5;
		fl::cmpb(peekb(0x0F26), 0x05);
		tick(0x5566, 30);                              // CMPB @#0x0F26,#5
		if (phase)
		{
			tick(0x55A8, 12);                          // BCC
			break;
		}
		tick(0x5568, 12);
		const bool state5 = peekb(0x031F) == 5;
		fl::cmpb(peekb(0x031F), 0x05);
		tick(0x556E, 30);                              // CMPB @#0x031F,#5
		if (state5)
		{
			tick(0x55A8, 12);                          // BEQ
			break;
		}
		tick(0x5570, 12);
		T.r[0] = 0x1C00;
		fl::mov(T.r[0]);
		tick(0x5574, 18);
		poke(0x01D4, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x5578, 27);
		poke(0x1402, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x557C, 27);
		jsr(0x5580, 0x78E4);
		tick(0x55A8, 12);                              // BR
		break;
	}
	case 20: case 21: case 22: case 23:                // park exits
	{
		jsr(0x5586, 0x5360);                           // park_at_gate
		pokeb(0x027A, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x558A, 27);
		T.r[0] = 0;
		fl::clr();
		tick(0x558C, 12);
		T.r[0] = peekb(0x027A);
		fl::logicb((uint8_t)T.r[0]);
		tick(0x5590, 24);
		const bool open = (peekb(0x0EA4) & rb((uint16_t)(0xFC74 + T.r[0]))) != 0;
		fl::logicb((uint8_t)(peekb(0x0EA4) & peekb((uint16_t)(T.r[0] + 0xFC74))));
		tick(0x5596, 39);                              // BITB @#0x0EA4,0xFC74(R0)
		if (!open)
		{
			tick(0x55A8, 12);                          // BEQ
			break;
		}
		tick(0x5598, 12);
		push(0x0053);
		fl::mov(0x0053);
		tick(0x559C, 30);
		jsr(0x55A0, 0x8E78);                           // queue_sound
		pokeb(0x0F26, 1);
		fl::movb(0x01);
		tick(0x55A6, 33);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(peek((uint16_t)(T.r[6] - 2)));
		tick(0x55A8, 18);                              // TST (SP)+
		break;
	}
	default:                                           // 12, 15, 24, 25 (and unused values)
		break;
	}
	return finish();
}

// ---------------------------------------------------------------- collisions

// A7:572A (assembly)  shapes_touch(int dy, int obj2, ...): pixel test of the skater's shape
// against object obj2 / 2 where their boxes overlap.  The shapes are 16-pixel-wide bit masks
// (one word per line) before the box descriptors (0x023A for the skater, 0x03F0[obj] for the
// object); B8:7DB8 shifts a mask word by R1 pixels.  0x0230 gets the number of overlapping
// lines, 0x0232 the horizontal shift, 0x0234/0x0236 the object's width/height in pixels / 256.
// Objects wider than 32 pixels are only partly handled, as in the original.  R0 = 1 when
// the shapes touch.  Registers end as the original leaves them.
void shapes_touch()
{
	// the locals (stack words below the return address E): L4 the skater's top, then the
	// direction flag; L2 the object's top, then a shift; L0 the overlap start, then the saved
	// R2; OX the object's left, then the saved R3; SX the skater's left, then a line counter;
	// S0 the x difference, then the line offset of the second half
	const uint16_t E = T.r[6];
	const uint16_t L0 = (uint16_t)(E - 6), L2 = (uint16_t)(E - 4), L4 = (uint16_t)(E - 2);
	const uint16_t OX = (uint16_t)(E - 8), SX = (uint16_t)(E - 10), S0 = (uint16_t)(E - 12);
	// CLRB a; SWAB a; BPL; COMB a+1 at p: the high byte of the word at a, sign-extended
	auto hi_byte = [](uint16_t a, uint16_t p) {
		pokeb(a, 0);
		tick((uint16_t)(p + 4), 27);
		const uint16_t w = peek(a);
		poke(a, (uint16_t)((w >> 8) | (w << 8)));
		const bool neg = (w & 0x8000) != 0;
		tick((uint16_t)(p + 8), 27);
		if (neg)
		{
			tick((uint16_t)(p + 10), 12);              // BPL
			pokeb((uint16_t)(a + 1), (uint8_t)~peekb((uint16_t)(a + 1)));
			tick((uint16_t)(p + 14), 27);
		}
		else
			tick((uint16_t)(p + 14), 12);
	};
	T.r[6] = L0;
	fl::sub((uint16_t)(T.r[6] + 0x0006), 0x0006);
	tick(0x572E, 18);                                  // SUB #6,SP
	T.r[0] = peek((uint16_t)(E + 4));
	fl::mov(T.r[0]);
	tick(0x5732, 27);
	T.r[2] = peek((uint16_t)(E + 2));
	fl::mov(T.r[2]);
	tick(0x5736, 27);
	T.r[1] = peek((uint16_t)(0x03F0 + T.r[0]));
	fl::mov(T.r[1]);
	tick(0x573A, 27);
	T.r[3] = peek((uint16_t)(0x0630 + T.r[0]));
	fl::mov(T.r[3]);
	tick(0x573E, 27);
	T.r[3] = fl::add(T.r[3], peek(T.r[1]));
	tick(0x5740, 18);
	push(T.r[3]);                                      // OX
	fl::mov(T.r[3]);
	tick(0x5742, 24);
	T.r[4] = peek(0x023A);
	fl::mov(T.r[4]);
	tick(0x5746, 24);
	push(peek(T.r[4]));                                // SX
	fl::mov(peek(T.r[6]));
	tick(0x5748, 30);
	poke(SX, fl::add(peek(SX), peek(0x0630)));
	tick(0x574C, 33);
	push(T.r[3]);                                      // S0
	fl::mov(T.r[3]);
	tick(0x574E, 24);
	poke(S0, fl::sub(peek(S0), peek(SX)));
	tick(0x5752, 36);
	T.r[3] = peek(S0);
	fl::mov(T.r[3]);
	tick(0x5754, 18);
	poke(0x0234, peek((uint16_t)(T.r[1] + 4)));
	fl::mov(peek(0x0234));
	tick(0x575A, 42);
	hi_byte(0x0234, 0x575A);
	poke(0x0236, peek((uint16_t)(T.r[1] + 6)));
	tick(0x576E, 42);
	hi_byte(0x0236, 0x576E);
	poke(0x0236, (uint16_t)(peek(0x0236) << 1));
	tick(0x5780, 27);
	poke(L4, peek((uint16_t)(T.r[4] + 2)));
	fl::mov(peek((uint16_t)(T.r[6] + 0x0A)));
	tick(0x5786, 45);
	poke(L4, fl::add(peek(L4), peek(0x0238)));
	tick(0x578C, 42);
	T.r[5] = peek(L4);
	fl::mov(T.r[5]);
	tick(0x5790, 27);
	T.r[5] = fl::add(T.r[5], peek((uint16_t)(T.r[4] + 6)));
	tick(0x5794, 27);
	push(peek((uint16_t)(0x0670 + T.r[0])));
	fl::mov(peek(T.r[6]));
	tick(0x5798, 39);
	poke(T.r[6], fl::add(peek(T.r[6]), peek((uint16_t)(T.r[1] + 2))));
	tick(0x579C, 36);
	const uint16_t top = pop();                        // MOV (SP)+,8(SP)
	poke(L2, top);
	fl::mov(peek((uint16_t)(T.r[6] + 8)));
	tick(0x57A0, 36);
	T.r[0] = peek(L2);
	fl::mov(T.r[0]);
	tick(0x57A4, 27);
	T.r[0] = fl::add(T.r[0], peek((uint16_t)(T.r[1] + 6)));
	tick(0x57A8, 27);
	fl::cmp(T.r[5], T.r[0]);
	tick(0x57AA, 12);                                  // CMP R5,R0
	if ((int16_t)T.r[5] < (int16_t)T.r[0])
	{
		tick(0x57AC, 12);                              // BGE
		T.r[0] = T.r[5];
		fl::mov(T.r[0]);
		tick(0x57AE, 12);
	}
	else
		tick(0x57AE, 12);
	poke(0x0230, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x57B2, 27);
	poke(L0, peek(L4));
	fl::mov(peek((uint16_t)(T.r[6] + 6)));
	tick(0x57B8, 45);
	T.r[0] = peek(L0);
	fl::mov(T.r[0]);
	tick(0x57BC, 27);
	T.r[0] = fl::sub(T.r[0], peek(L2));
	tick(0x57C0, 27);
	if (!(T.r[0] & 0x8000))
	{
		tick(0x57C2, 12);                              // BMI
		T.r[3] = 0xFFFE;
		fl::mov(T.r[3]);
		tick(0x57C6, 18);
		T.r[3] = fl::add(T.r[3], T.r[4]);
		tick(0x57C8, 12);
		poke(L4, 0);
		fl::clr();
		tick(0x57CC, 30);
		T.r[5] = T.r[0];
		fl::mov(T.r[5]);
		tick(0x57CE, 12);
		T.r[5] = fl::add(T.r[5], 0x0100);
		tick(0x57D2, 18);
		T.r[0] = T.r[5];
		fl::mov(T.r[0]);
		tick(0x57D4, 12);
		T.r[0] &= 0xFF00;
		fl::clr();
		tick(0x57D6, 12);                              // CLRB R0
		T.r[0] = (uint16_t)(T.r[0] >> 8);
		tick(0x57D8, 12);                              // SWAB R0
		T.r[0] = sext8((uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x57DA, 12);                              // MOVB R0,R0
		T.r[5] = T.r[0];
		fl::mov(T.r[5]);
		tick(0x57DC, 12);
		T.r[5] = fl::asl(T.r[5]);
		tick(0x57DE, 12);
		T.r[2] = T.r[1];
		fl::mov(T.r[2]);
		tick(0x57E0, 12);
		T.r[2] = fl::sub(T.r[2], T.r[5]);
		tick(0x57E2, 12);
		T.r[0] = peek(0x0230);
		fl::mov(T.r[0]);
		tick(0x57E6, 24);
		T.r[0] = fl::sub(T.r[0], peek(L0));
		tick(0x57EA, 27);
		T.r[0] = fl::add(T.r[0], 0x00C0);
		tick(0x57EE, 18);
		poke(0x0230, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x57F2, 27);
		hi_byte(0x0230, 0x57F2);
		poke(0x0232, peek(S0));
		fl::mov(peek(0x0232));
		tick(0x5804, 33);
		tick(0x5850, 12);                              // BR
	}
	else
	{
		tick(0x5806, 12);                              // BMI
		T.r[3] = 0xFFFE;
		fl::mov(T.r[3]);
		tick(0x580A, 18);
		T.r[3] = fl::add(T.r[3], T.r[1]);
		tick(0x580C, 12);
		poke(L4, 0x0001);
		fl::mov(0x0001);
		tick(0x5812, 36);
		T.r[5] = 0x0100;
		fl::mov(T.r[5]);
		tick(0x5816, 18);
		T.r[5] = fl::sub(T.r[5], T.r[0]);
		tick(0x5818, 12);
		T.r[0] = T.r[5];
		fl::mov(T.r[0]);
		tick(0x581A, 12);
		T.r[0] &= 0xFF00;
		fl::clr();
		tick(0x581C, 12);                              // CLRB R0
		T.r[0] = (uint16_t)(T.r[0] >> 8);
		tick(0x581E, 12);                              // SWAB R0
		T.r[0] = sext8((uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x5820, 12);                              // MOVB R0,R0
		T.r[0] = fl::asl(T.r[0]);
		tick(0x5822, 12);
		T.r[2] = T.r[4];
		fl::mov(T.r[2]);
		tick(0x5824, 12);
		T.r[2] = fl::sub(T.r[2], T.r[0]);
		tick(0x5826, 12);
		T.r[0] = peek(0x0230);
		fl::mov(T.r[0]);
		tick(0x582A, 24);
		T.r[0] = fl::sub(T.r[0], peek(L2));
		tick(0x582E, 27);
		T.r[0] = fl::add(T.r[0], 0x00C0);
		tick(0x5832, 18);
		poke(0x0230, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x5836, 27);
		hi_byte(0x0230, 0x5836);
		poke(0x0232, peek(SX));
		fl::mov(peek(0x0232));
		tick(0x584A, 42);
		poke(0x0232, fl::sub(peek(0x0232), peek(OX)));
		tick(0x5850, 42);
	}
	T.r[0] = peek(0x0232);
	fl::mov(T.r[0]);
	tick(0x5854, 24);
	T.r[4] = peek(0x0234);
	fl::mov(T.r[4]);
	tick(0x5858, 24);
	fl::cmp(T.r[4], 0x0010);
	tick(0x585C, 18);                                  // CMP R4,#0x10
	bool hit = false;
	if ((int16_t)T.r[4] <= 0x10)
	{
		tick(0x585E, 12);                              // BGT
		fl::tst(T.r[0]);
		tick(0x5860, 12);                              // TST R0
		if (T.r[0] & 0x8000)
		{
			tick(0x5862, 12);                          // BGE
			T.r[1] = T.r[0];
			fl::mov(T.r[1]);
			tick(0x5864, 12);
			T.r[1] = (uint16_t)-T.r[1];
			fl::neg((uint16_t)(-T.r[1]));
			tick(0x5866, 12);
			poke(0x0232, T.r[1]);
			fl::mov(T.r[1]);
			tick(0x586A, 27);
			hi_byte(0x0232, 0x586A);
			T.r[5] = peek(0x0230);
			tick(0x587C, 24);
			if ((int16_t)T.r[5] <= 0)
				tick(0x58C8, 12);                      // BLE
			else
			{
				tick(0x587E, 12);
				for (;;)
				{
					T.r[0] = peek(T.r[2]);
					fl::mov(T.r[0]);
					tick(0x5880, 18);
					T.r[1] = peek(0x0232);
					fl::mov(T.r[1]);
					tick(0x5884, 24);
					jsr(0x5888, 0x7DB8);               // shift the mask
					hit = (peek(T.r[3]) & T.r[0]) != 0;
					fl::logic((uint16_t)(peek(T.r[3]) & T.r[0]));
					tick(0x588A, 18);                  // BIT (R3),R0
					if (hit)
					{
						tick(0x595C, 12);              // BNE
						tick(0x5A2E, 12);              // BR
						break;
					}
					tick(0x588C, 12);
					T.r[3] = fl::sub(T.r[3], 2);
					tick(0x5890, 18);
					T.r[2] = fl::sub(T.r[2], 2);
					tick(0x5894, 18);
					if (!--T.r[5])
					{
						tick(0x5896, 18);              // SOB
						tick(0x58C8, 12);              // BR
						break;
					}
					tick(0x587E, 18);
				}
			}
		}
		else
		{
			tick(0x5898, 12);                          // BGE
			poke(0x0232, T.r[0]);
			fl::mov(T.r[0]);
			tick(0x589C, 27);
			hi_byte(0x0232, 0x589C);
			T.r[5] = peek(0x0230);
			fl::mov(T.r[5]);
			tick(0x58AE, 24);
			if ((int16_t)T.r[5] <= 0)
				tick(0x58C8, 12);                      // BLE
			else
			{
				tick(0x58B0, 12);
				for (;;)
				{
					T.r[0] = peek(T.r[3]);
					fl::mov(T.r[0]);
					tick(0x58B2, 18);
					T.r[1] = peek(0x0232);
					fl::mov(T.r[1]);
					tick(0x58B6, 24);
					jsr(0x58BA, 0x7DB8);
					hit = (T.r[0] & peek(T.r[2])) != 0;
					fl::logic((uint16_t)(T.r[0] & peek(T.r[2])));
					tick(0x58BC, 18);                  // BIT R0,(R2)
					if (hit)
					{
						tick(0x59B0, 12);              // BNE to the BNE at 0x59B0
						tick(0x5A2E, 12);
						break;
					}
					tick(0x58BE, 12);
					T.r[3] = fl::sub(T.r[3], 2);
					tick(0x58C2, 18);
					T.r[2] = fl::sub(T.r[2], 2);
					tick(0x58C6, 18);
					if (!--T.r[5])
					{
						tick(0x58C8, 18);              // SOB
						break;
					}
					tick(0x58B0, 18);
				}
			}
		}
		if (!hit)
			tick(0x5A3E, 18);                          // JMP @#0x5A3E
	}
	else
	{
		tick(0x58CC, 12);                              // BGT
		poke(OX, T.r[3]);
		fl::mov(T.r[3]);
		tick(0x58D0, 30);
		poke(L0, T.r[2]);
		fl::mov(T.r[2]);
		tick(0x58D4, 30);
		poke(0x0232, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x58D8, 27);
		hi_byte(0x0232, 0x58D8);
		T.r[0] = peek(0x0232);
		tick(0x58EA, 24);
		T.r[1] = T.r[4];
		fl::mov(T.r[1]);
		tick(0x58EC, 12);
		T.r[1] = fl::sub(T.r[1], 0x0010);
		tick(0x58F0, 18);
		const bool flip = peek(L4) != 0;
		fl::tst(peek((uint16_t)(T.r[6] + 0x0A)));
		tick(0x58F4, 27);                              // TST 0xA(SP)
		if (flip)
		{
			tick(0x58F6, 12);                          // BEQ
			T.r[5] = T.r[0];
			fl::mov(T.r[5]);
			tick(0x58F8, 12);
			T.r[5] = fl::sub(T.r[5], T.r[1]);
			tick(0x58FA, 12);
			fl::cmp(T.r[4], 0x0020);
			tick(0x58FE, 18);                          // CMP R4,#0x20
			if ((int16_t)T.r[4] > 0x20)
			{
				tick(0x5900, 12);                      // BLE
				poke(L2, T.r[5]);
				fl::mov(T.r[5]);
				tick(0x5904, 30);
				T.r[5] = fl::add(T.r[5], 0x0010);
				tick(0x5908, 18);
				tick(0x591C, 12);                      // BR
			}
			else
				tick(0x591C, 12);
		}
		else
		{
			tick(0x590A, 12);                          // BEQ
			T.r[5] = T.r[1];
			fl::mov(T.r[5]);
			tick(0x590C, 12);
			T.r[5] = fl::add(T.r[5], T.r[0]);
			tick(0x590E, 12);
			fl::cmp(T.r[4], 0x0020);
			tick(0x5912, 18);                          // CMP R4,#0x20
			if ((int16_t)T.r[4] > 0x20)
			{
				tick(0x5914, 12);                      // BLE
				poke(L2, T.r[5]);
				fl::mov(T.r[5]);
				tick(0x5918, 30);
				T.r[5] = fl::sub(T.r[5], 0x0010);
				tick(0x591C, 18);
			}
			else
				tick(0x591C, 12);
		}
		fl::tst(T.r[0]);
		tick(0x591E, 12);                              // TST R0
		if (T.r[0] & 0x8000)
		{
			tick(0x5920, 12);                          // BGE
			T.r[0] = (uint16_t)-T.r[0];
			fl::neg((uint16_t)(-T.r[0]));
			tick(0x5922, 12);
		}
		else
			tick(0x5922, 12);
		fl::cmp(T.r[0], 0x0010);
		tick(0x5926, 18);                              // CMP R0,#0x10
		if ((int16_t)T.r[0] < 0x10)
		{
			tick(0x5928, 12);                          // BGE
			fl::cmp(T.r[4], 0x0020);
			tick(0x592C, 18);                          // CMP R4,#0x20
			if ((int16_t)T.r[4] > 0x20)
			{
				tick(0x5934, 12);                      // BGT
				poke(S0, peek(0x0236));
				fl::mov(peek(T.r[6]));
				tick(0x5938, 33);
				poke(S0, (uint16_t)(peek(S0) << 1));
				tick(0x593A, 21);
			}
			else
			{
				tick(0x592E, 12);
				poke(S0, peek(0x0236));
				fl::mov(peek(T.r[6]));
				tick(0x5932, 33);
				tick(0x593A, 12);                      // BR
			}
			poke(SX, peek(0x0230));
			const bool lines = (int16_t)peek(SX) > 0;
			fl::mov(peek((uint16_t)(T.r[6] + 2)));
			tick(0x5940, 42);
			if (!lines)
				tick(0x597E, 12);                      // BLE
			else
			{
				tick(0x5942, 12);
				for (;;)
				{
					const bool f = peek(L4) != 0;
					fl::tst(peek((uint16_t)(T.r[6] + 0x0A)));
					tick(0x5946, 27);                  // TST 0xA(SP)
					if (!f)
					{
						tick(0x5948, 12);              // BNE
						T.r[0] = peek(T.r[3]);
						fl::mov(T.r[0]);
						tick(0x594A, 18);
						T.r[1] = peek(0x0232);
						fl::mov(T.r[1]);
						tick(0x594E, 24);
						jsr(0x5952, 0x7DB8);
						T.r[1] = T.r[0];
						fl::mov(T.r[1]);
						tick(0x5954, 12);
						T.r[0] = T.r[2];
						fl::mov(T.r[0]);
						tick(0x5956, 12);
						T.r[0] = fl::sub(T.r[0], peek(S0));
						tick(0x5958, 18);
						hit = (T.r[1] & peek(T.r[0])) != 0;
						fl::logic((uint16_t)(T.r[1] & peek(T.r[0])));
						tick(0x595A, 18);              // BIT R1,(R0)
					}
					else
					{
						tick(0x595E, 12);              // BNE
						T.r[4] = T.r[3];
						fl::mov(T.r[4]);
						tick(0x5960, 12);
						T.r[4] = fl::sub(T.r[4], peek(S0));
						tick(0x5962, 18);
						T.r[0] = peek(T.r[4]);
						fl::mov(T.r[0]);
						tick(0x5964, 18);
						T.r[1] = peek(0x0232);
						fl::mov(T.r[1]);
						tick(0x5968, 24);
						jsr(0x596C, 0x7DB8);
						hit = (T.r[0] & peek(T.r[2])) != 0;
						fl::logic((uint16_t)(T.r[0] & peek(T.r[2])));
						tick(0x596E, 18);              // BIT R0,(R2)
						tick(0x595A, 12);              // BR
					}
					if (hit)
					{
						tick(0x595C, 12);              // BEQ
						tick(0x5A2E, 12);              // BR
						break;
					}
					tick(0x5970, 12);
					T.r[3] = fl::sub(T.r[3], 2);
					tick(0x5974, 18);
					T.r[2] = fl::sub(T.r[2], 2);
					tick(0x5978, 18);
					poke(SX, (uint16_t)(peek(SX) - 1));
					const bool more = peek(SX) != 0;
					fl::dec((uint16_t)(peek((uint16_t)(T.r[6] + 2)) + 1));
					tick(0x597C, 30);                  // DEC 2(SP)
					if (!more)
					{
						tick(0x597E, 12);              // BNE
						break;
					}
					tick(0x5942, 12);
				}
			}
			if (!hit)
			{
				T.r[3] = peek(OX);
				fl::mov(T.r[3]);
				tick(0x5982, 27);
				T.r[2] = peek(L0);
				fl::mov(T.r[2]);
				tick(0x5986, 27);
			}
		}
		else
			tick(0x5986, 12);                          // BGE
		if (!hit)
		{
			fl::tst(T.r[5]);
			tick(0x5988, 12);                          // TST R5
			if (T.r[5] & 0x8000)
			{
				tick(0x598E, 12);                      // BLT
				T.r[0] = T.r[5];
				fl::mov(T.r[0]);
				tick(0x5990, 12);
				T.r[0] = (uint16_t)-T.r[0];
				fl::neg((uint16_t)(-T.r[0]));
				tick(0x5992, 12);
			}
			else
			{
				tick(0x598A, 12);
				T.r[0] = T.r[5];
				fl::mov(T.r[0]);
				tick(0x598C, 12);
				tick(0x5992, 12);                      // BR
			}
			fl::cmp(T.r[0], 0x0010);
			tick(0x5996, 18);                          // CMP R0,#0x10
			if ((int16_t)T.r[0] < 0x10)
			{
				tick(0x5998, 12);                      // BGE
				const bool wide = (int16_t)peek(0x0234) > 0x20;
				fl::cmp(peek(0x0234), 0x0020);
				tick(0x599E, 30);                      // CMP @#0x0234,#0x20
				if (!wide)
				{
					tick(0x59A0, 12);                  // BGT
					T.r[4] = peek(0x0230);
					fl::mov(T.r[4]);
					tick(0x59A4, 24);
					if ((int16_t)T.r[4] <= 0)
						tick(0x5A02, 12);              // BLE
					else
					{
						tick(0x59A6, 12);
						for (;;)
						{
							T.r[0] = peek(T.r[3]);
							fl::mov(T.r[0]);
							tick(0x59A8, 18);
							T.r[1] = T.r[5];
							fl::mov(T.r[1]);
							tick(0x59AA, 12);
							jsr(0x59AE, 0x7DB8);
							hit = (T.r[0] & peek(T.r[2])) != 0;
							fl::logic((uint16_t)(T.r[0] & peek(T.r[2])));
							tick(0x59B0, 18);          // BIT R0,(R2)
							if (hit)
							{
								tick(0x5A2E, 12);      // BNE
								break;
							}
							tick(0x59B2, 12);
							T.r[3] = fl::sub(T.r[3], 2);
							tick(0x59B6, 18);
							T.r[2] = fl::sub(T.r[2], 2);
							tick(0x59BA, 18);
							if (!--T.r[4])
							{
								tick(0x59BC, 18);      // SOB
								tick(0x5A02, 12);      // BR
								break;
							}
							tick(0x59A6, 18);
						}
					}
				}
				else
				{
					tick(0x59BE, 12);                  // BGT
					poke(SX, peek(0x0230));
					const bool lines = (int16_t)peek(SX) > 0;
					fl::mov(peek((uint16_t)(T.r[6] + 2)));
					tick(0x59C4, 42);
					if (!lines)
						tick(0x5A02, 12);              // BLE
					else
					{
						tick(0x59C6, 12);
						for (;;)
						{
							const bool f = peek(L4) != 0;
							fl::tst(peek((uint16_t)(T.r[6] + 0x0A)));
							tick(0x59CA, 27);          // TST 0xA(SP)
							if (!f)
							{
								tick(0x59CC, 12);      // BNE
								T.r[0] = peek(T.r[3]);
								fl::mov(T.r[0]);
								tick(0x59CE, 18);
								T.r[1] = T.r[5];
								fl::mov(T.r[1]);
								tick(0x59D0, 12);
								jsr(0x59D4, 0x7DB8);
								T.r[1] = T.r[0];
								fl::mov(T.r[1]);
								tick(0x59D6, 12);
								T.r[0] = T.r[2];
								fl::mov(T.r[0]);
								tick(0x59D8, 12);
								T.r[0] = fl::sub(T.r[0], peek(0x0236));
								tick(0x59DC, 24);
								hit = (T.r[1] & peek(T.r[0])) != 0;
								fl::logic((uint16_t)(T.r[1] & peek(T.r[0])));
								tick(0x59DE, 18);      // BIT R1,(R0)
							}
							else
							{
								tick(0x59E2, 12);      // BNE
								T.r[4] = T.r[3];
								fl::mov(T.r[4]);
								tick(0x59E4, 12);
								T.r[4] = fl::sub(T.r[4], peek(0x0236));
								tick(0x59E8, 24);
								T.r[0] = peek(T.r[4]);
								fl::mov(T.r[0]);
								tick(0x59EA, 18);
								T.r[1] = T.r[5];
								fl::mov(T.r[1]);
								tick(0x59EC, 12);
								jsr(0x59F0, 0x7DB8);
								hit = (T.r[0] & peek(T.r[2])) != 0;
								fl::logic((uint16_t)(T.r[0] & peek(T.r[2])));
								tick(0x59F2, 18);      // BIT R0,(R2)
								tick(0x59DE, 12);      // BR
							}
							if (hit)
							{
								tick(0x59E0, 12);      // BEQ
								tick(0x5A2E, 12);      // BR
								break;
							}
							tick(0x59F4, 12);
							T.r[3] = fl::sub(T.r[3], 2);
							tick(0x59F8, 18);
							T.r[2] = fl::sub(T.r[2], 2);
							tick(0x59FC, 18);
							poke(SX, (uint16_t)(peek(SX) - 1));
							const bool more = peek(SX) != 0;
							fl::dec((uint16_t)(peek((uint16_t)(T.r[6] + 2)) + 1));
							tick(0x5A00, 30);          // DEC 2(SP)
							if (!more)
							{
								tick(0x5A02, 12);      // BNE
								break;
							}
							tick(0x59C6, 12);
						}
					}
				}
				if (!hit)
				{
					T.r[3] = peek(OX);
					fl::mov(T.r[3]);
					tick(0x5A06, 27);
					T.r[2] = peek(L0);
					fl::mov(T.r[2]);
					tick(0x5A0A, 27);
				}
			}
			else
				tick(0x5A0A, 12);                      // BGE
		}
		if (!hit)
		{
			const bool more = (int16_t)peek(L2) > (int16_t)0xFFF0;
			fl::cmp(peek((uint16_t)(T.r[6] + 8)), 0xFFF0);
			tick(0x5A10, 33);                          // CMP 8(SP),#0xFFF0
			if (!more)
				tick(0x5A3E, 12);                      // BLE
			else
			{
				tick(0x5A12, 12);
				const bool wide = (int16_t)peek(0x0234) > 0x20;
				fl::cmp(peek(0x0234), 0x0020);
				tick(0x5A18, 30);                      // CMP @#0x0234,#0x20
				if (!wide)
					tick(0x5A3E, 12);                  // BLE
				else
				{
					tick(0x5A1A, 12);
					T.r[4] = peek(0x0230);
					fl::mov(T.r[4]);
					tick(0x5A1E, 24);
					if ((int16_t)T.r[4] <= 0)
						tick(0x5A3E, 12);              // BLE
					else
					{
						tick(0x5A20, 12);
						for (;;)
						{
							T.r[0] = peek(T.r[3]);
							fl::mov(T.r[0]);
							tick(0x5A22, 18);
							T.r[1] = peek(L2);
							fl::mov(T.r[1]);
							tick(0x5A26, 27);
							jsr(0x5A2A, 0x7DB8);
							hit = (T.r[0] & peek(T.r[2])) != 0;
							fl::logic((uint16_t)(T.r[0] & peek(T.r[2])));
							tick(0x5A2C, 18);          // BIT R0,(R2)
							if (hit)
							{
								tick(0x5A2E, 12);      // BEQ
								break;
							}
							tick(0x5A34, 12);
							T.r[3] = fl::sub(T.r[3], 2);
							tick(0x5A38, 18);
							T.r[2] = fl::sub(T.r[2], 2);
							tick(0x5A3C, 18);
							if (!--T.r[4])
							{
								tick(0x5A3E, 18);      // SOB
								break;
							}
							tick(0x5A20, 18);
						}
					}
				}
			}
		}
	}
	if (hit)
	{
		T.r[0] = 1;
		fl::mov(T.r[0]);
		tick(0x5A32, 18);
		tick(0x5A40, 12);                              // BR
	}
	else
	{
		T.r[0] = 0;
		tick(0x5A40, 12);
	}
	T.r[6] = fl::add(T.r[6], 0x000C);
	tick(0x5A44, 18);
	rts();
}

// ---------------------------------------------------------------- sound CPU exchanges
// These wait for the sound CPU (F:8ED2 read_sound_response polls), so the per-call tester
// mostly gives up on them; they are checked with whole runs (tools/compare_runs.py).

// A7:4B00  reset_sound_cpu(): up to 5 times: read the response port (and the byte it points
// to), hold the sound CPU in reset for 201 passes of a delay loop and wait for its 0xFF
// greeting (0x01A3 cleared then).  If it never answers the game is restarted (F:8000, not
// reached by the tests).
uint16_t reset_sound_cpu(const Args &)
{
	c_save(1, 0x4B04);
	pokeb(0x01A2, 0);
	fl::clr();
	tick(0x4B08, 27);
	T.r[1] = 5;
	fl::mov(T.r[1]);
	tick(0x4B0C, 18);
	for (;;)
	{
		T.r[0] = peek(0x1C00);
		fl::mov(T.r[0]);
		tick(0x4B10, 24);
		T.r[2] = 0;
		fl::clr();
		tick(0x4B12, 12);
		T.r[2] = peekb(T.r[0]);
		fl::logicb((uint8_t)T.r[2]);
		tick(0x4B14, 18);
		poke(0x15A0, 0x0001);                          // sound CPU held in reset
		fl::mov(0x0001);
		tick(0x4B1A, 33);
		T.r[0] = 0x00C9;
		fl::mov(T.r[0]);
		tick(0x4B1E, 18);
		for (;;)
		{
			T.r[2] = fl::inc(T.r[2]);
			tick(0x4B20, 12);
			T.r[0]--;
			if (!T.r[0])
			{
				tick(0x4B22, 18);                      // SOB
				break;
			}
			tick(0x4B1E, 18);
		}
		poke(0x15A0, 0);
		tick(0x4B26, 27);
		jsr(0x4B2A, 0x8ED2);                           // read_sound_response
		fl::cmp(T.r[0], 0x00FF);
		tick(0x4B2E, 18);                              // CMP R0,#0x00FF
		if (T.r[0] == 0x00FF)
		{
			tick(0x4B30, 12);
			pokeb(0x01A3, 0);
			fl::clr();
			tick(0x4B34, 27);
			c_restore(1);
			return T.r[0];
		}
		tick(0x4B36, 12);                              // BNE
		T.r[1]--;
		if (!T.r[1])
		{
			tick(0x4B38, 18);                          // SOB
			break;
		}
		tick(0x4B0C, 18);
	}
	// the reset: the game starts over inside this call (it does not come back)
	jsr(0x4B3C, 0x8000);
	c_restore(1);
	return T.r[0];
}

// A7:435A  read_switch_settings(): command 0x0B; the sound CPU answers with the coin
// switches 0x01FA / 0x01F3 / 0x01F4 and the credits (stored in 0x01F9; a coin sound 0x68 when
// bits 2-6 went up)
uint16_t read_switch_settings(const Args &)
{
	c_save(1, 0x435E);
	jsr(0x4362, 0x8E6A);                               // set_flag_01a4
	push(0x000B);
	fl::mov(0x000B);
	tick(0x4366, 30);
	jsr(0x436A, 0x8EA2);                               // send_sound_command
	jsr(0x436E, 0x8ED2);                               // read_sound_response
	jsr(0x4372, 0x8ED2);
	pokeb(0x01FA, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x4376, 27);
	jsr(0x437A, 0x8ED2);
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0x437C, 12);
	jsr(0x4380, 0x8ED2);
	pokeb(0x01F3, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x4384, 27);
	jsr(0x4388, 0x8ED2);
	pokeb(0x01F4, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x438C, 27);
	jsr(0x4390, 0x8F62);
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0x4392, 12);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x007C));
	tick(0x4396, 18);
	T.r[2] = 0;
	fl::clr();
	tick(0x4398, 12);
	T.r[2] = peekb(0x01F9);
	fl::logicb((uint8_t)T.r[2]);
	tick(0x439C, 24);
	T.r[2] = fl::logic((uint16_t)(T.r[2] & 0x007C));
	tick(0x43A0, 18);
	fl::cmp(T.r[0], T.r[2]);
	tick(0x43A2, 12);                                  // CMP R0,R2
	if ((int16_t)T.r[0] > (int16_t)T.r[2])
	{
		tick(0x43A4, 12);
		poke(T.r[6], 0x0068);
		fl::mov(0x0068);
		tick(0x43A8, 27);
		jsr(0x43AC, 0x8EA2);                           // the coin sound
	}
	else
		tick(0x43AC, 12);                              // BLE
	pokeb(0x01F9, (uint8_t)T.r[1]);
	fl::movb((uint8_t)T.r[1]);
	tick(0x43B0, 27);
	jsr(0x43B4, 0x8E72);                               // clear_flag_01a4
	T.r[6] = (uint16_t)(T.r[6] + 2);
	fl::tst(peek((uint16_t)(T.r[6] - 2)));
	tick(0x43B6, 18);                                  // TST (SP)+
	c_restore(1);
	return T.r[0];
}

// A7:4476  read_high_scores(): command 0x11: the 30 high score entries (8 bytes at 0x0370:
// score bytes, then three initials; the first 15 with a third score byte, the others with a
// one-bit flag) come from the sound CPU, which keeps them; 30 + 25 more bytes are skipped
uint16_t read_high_scores(const Args &)
{
	c_save(1, 0x447A);
	jsr(0x447E, 0x8E6A);                               // set_flag_01a4
	push(0x0011);
	fl::mov(0x0011);
	tick(0x4482, 30);
	jsr(0x4486, 0x8EA2);                               // send_sound_command
	T.r[2] = 0;
	fl::clr();
	tick(0x4488, 12);
	for (;;)
	{
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0x448A, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x448C, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x448E, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x4490, 12);
		T.r[1] = T.r[0];
		fl::mov(T.r[1]);
		tick(0x4492, 12);
		poke((uint16_t)(0x0370 + T.r[1]), 0);
		fl::clr();
		tick(0x4496, 30);
		fl::cmp(T.r[2], 0x000E);
		tick(0x449A, 18);                              // CMP R2,#14
		if ((int16_t)T.r[2] <= 14)
		{
			tick(0x449C, 12);
			jsr(0x44A0, 0x8ED2);                       // read_sound_response
			pokeb((uint16_t)(0x0370 + T.r[1]), (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0x44A4, 30);
			jsr(0x44A8, 0x8ED2);
			pokeb((uint16_t)(0x0373 + T.r[1]), (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0x44AC, 30);
			jsr(0x44B0, 0x8ED2);
			pokeb((uint16_t)(0x0372 + T.r[1]), (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0x44B4, 30);
			tick(0x44CC, 12);                          // BR
		}
		else
		{
			tick(0x44B6, 12);                          // BGT
			jsr(0x44BA, 0x8ED2);
			pokeb((uint16_t)(0x0372 + T.r[1]), (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0x44BE, 30);
			jsr(0x44C2, 0x8ED2);
			pokeb((uint16_t)(0x0373 + T.r[1]), (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0x44C6, 30);
			pokeb((uint16_t)(0x0373 + T.r[1]), fl::logicb((uint8_t)(peekb((uint16_t)(0x0373 + T.r[1])) & 0x01)));
			tick(0x44CC, 36);
		}
		jsr(0x44D0, 0x8ED2);
		pokeb((uint16_t)(0x0374 + T.r[1]), (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x44D4, 30);
		jsr(0x44D8, 0x8ED2);
		pokeb((uint16_t)(0x0375 + T.r[1]), (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x44DC, 30);
		poke((uint16_t)(0x0376 + T.r[1]), 0);
		fl::clr();
		tick(0x44E0, 30);
		jsr(0x44E4, 0x8ED2);
		pokeb((uint16_t)(0x0376 + T.r[1]), (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x44E8, 30);
		fl::cmp(T.r[2], 0x000E);
		tick(0x44EC, 18);                              // CMP R2,#14
		if (T.r[2] == 14)
		{
			tick(0x44EE, 12);
			T.r[1] = 0x001E;
			fl::mov(T.r[1]);
			tick(0x44F2, 18);
			for (;;)
			{
				jsr(0x44F6, 0x8ED2);                   // skipped
				T.r[1]--;
				if (!T.r[1])
				{
					tick(0x44F8, 18);                  // SOB
					break;
				}
				tick(0x44F2, 18);
			}
		}
		else
			tick(0x44F8, 12);                          // BNE
		T.r[2] = fl::inc(T.r[2]);
		tick(0x44FA, 12);
		fl::cmp(T.r[2], 0x001D);
		tick(0x44FE, 18);                              // CMP R2,#29
		if ((int16_t)T.r[2] > 0x1D)
		{
			tick(0x4500, 12);                          // BLE
			break;
		}
		tick(0x4488, 12);
	}
	T.r[1] = 0x0019;
	tick(0x4504, 18);
	for (;;)
	{
		jsr(0x4508, 0x8ED2);                           // skipped
		T.r[1]--;
		if (!T.r[1])
		{
			tick(0x450A, 18);                          // SOB
			break;
		}
		tick(0x4504, 18);
	}
	jsr(0x450E, 0x8F62);
	jsr(0x4512, 0x8E72);                               // clear_flag_01a4
	T.r[6] = (uint16_t)(T.r[6] + 2);
	fl::tst(peek((uint16_t)(T.r[6] - 2)));
	tick(0x4514, 18);                                  // TST (SP)+
	c_restore(1);
	return T.r[0];
}

// A7:4516  show_high_scores(): read the table (A7:4476), print the titles 0x7710/0x7718/0x771E
// (F:A9FC) and the 30 entries: initials with the small font, the score by F:BA96 (text
// layer positions from 0x7F7C, bank B 0x1C00).  Bank B ends as 0x3C00 (also left in R0).
uint16_t show_high_scores(const Args &)
{
	c_save(3, 0x451A);
	jsr(0x451E, 0x4476);                               // read_high_scores
	push(0x7710);
	fl::mov(0x7710);
	tick(0x4522, 30);
	jsr(0x4526, 0xA9FC);
	poke(T.r[6], 0x7718);
	fl::mov(0x7718);
	tick(0x452A, 27);
	jsr(0x452E, 0xA9FC);
	poke(T.r[6], 0x771E);
	fl::mov(0x771E);
	tick(0x4532, 27);
	jsr(0x4536, 0xA9FC);
	T.r[0] = 0x1C00;
	fl::mov(T.r[0]);
	tick(0x453A, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x453E, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x4542, 27);
	T.r[4] = 0;
	fl::clr();
	tick(0x4544, 12);
	for (;;)
	{
		T.r[0] = T.r[4];
		fl::mov(T.r[0]);
		tick(0x4546, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x4548, 12);
		poke(0x0186, peek((uint16_t)(0x7F7C + T.r[0])));
		fl::mov(peek(0x0186));
		tick(0x454E, 42);
		poke(0x0186, fl::add(peek(0x0186), 0x2000));
		tick(0x4554, 33);
		T.r[0] = T.r[4];
		fl::mov(T.r[0]);
		tick(0x4556, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x4558, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x455A, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x455C, 12);
		T.r[2] = T.r[0];
		fl::mov(T.r[2]);
		tick(0x455E, 12);
		T.r[3] = 0x0004;
		fl::mov(T.r[3]);
		tick(0x4562, 18);
		for (;;)
		{
			T.r[1] = T.r[3];
			fl::mov(T.r[1]);
			tick(0x4564, 12);
			T.r[1] = fl::add(T.r[1], T.r[2]);
			tick(0x4566, 12);
			T.r[0] = 0;
			fl::clr();
			tick(0x4568, 12);
			T.r[0] = peekb((uint16_t)(0x0370 + T.r[1]));
			fl::logicb((uint8_t)T.r[0]);
			tick(0x456C, 27);
			jsr(0x4570, 0x8C12);                       // put_small_char
			T.r[3] = fl::inc(T.r[3]);
			tick(0x4572, 12);
			fl::cmp(T.r[3], 0x0006);
			tick(0x4576, 18);                          // CMP R3,#6
			if ((int16_t)T.r[3] > 6)
			{
				tick(0x4578, 12);                      // BLE
				break;
			}
			tick(0x4562, 12);
		}
		poke(0x0186, (uint16_t)(peek(0x0186) + 6));
		tick(0x457E, 33);
		poke(T.r[6], peek((uint16_t)(0x0372 + T.r[2])));
		fl::mov(peek(T.r[6]));
		tick(0x4582, 36);
		push(peek((uint16_t)(0x0370 + T.r[2])));
		fl::mov(peek(T.r[6]));
		tick(0x4586, 39);
		push(0x0007);
		fl::mov(0x0007);
		tick(0x458A, 30);
		push(0x0001);
		fl::mov(0x0001);
		tick(0x458E, 30);
		push(0);
		fl::clr();
		tick(0x4590, 24);
		jsr(0x4594, 0xBA96);                           // print the score
		T.r[6] = fl::add(T.r[6], 8);
		tick(0x4598, 18);
		T.r[4] = fl::inc(T.r[4]);
		tick(0x459A, 12);
		fl::cmp(T.r[4], 0x001D);
		tick(0x459E, 18);                              // CMP R4,#29
		if ((int16_t)T.r[4] > 0x1D)
		{
			tick(0x45A0, 12);                          // BLE
			break;
		}
		tick(0x4544, 12);
	}
	T.r[0] = 0x3C00;
	tick(0x45A4, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x45A8, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x45AC, 27);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	fl::tst(peek((uint16_t)(T.r[6] - 2)));
	tick(0x45AE, 18);                                  // TST (SP)+
	c_restore(3);
	return T.r[0];
}

// A7:419A (assembly)  start_game(): for each player (0..0x01F6): starting time and ticket
// values from the coin settings (0x0FE3/0x0FE4/0x0FE5, tables 0x5212/0x523A), score and
// counters cleared, cash 100, countdown bytes 0x0274 from 0x7C7A (bank B 0x2800), a random
// byte for 0x0261 (and F:E730 with two players); then the common state, F:BA2E, F:CFD6, a
// fade to level 0, two vertical blanks with the palette loaders as their hook (0x0EB0), and
// 32 sound CPU exchanges F:910C.  Banks are restored; the flags are those of the final ADD.
void start_game()
{
	const uint16_t E = T.r[6];
	const uint16_t L = (uint16_t)(E - 2);              // the exchange counter
	T.r[6] = L;
	fl::tst(peek(T.r[6]));
	tick(0x419C, 21);                                  // TST -(SP)
	push(peek(0x01D2));
	fl::mov(peek(T.r[6]));
	tick(0x41A0, 36);
	push(peek(0x01D4));
	fl::mov(peek(T.r[6]));
	tick(0x41A4, 36);
	T.r[2] = 0;
	fl::clr();
	tick(0x41A6, 12);
	T.r[2] = peekb(0x01F6);
	fl::logicb((uint8_t)T.r[2]);
	tick(0x41AA, 24);
	T.r[1] = 0;
	fl::clr();
	tick(0x41AC, 12);
	tick(0x428C, 12);                                  // BR
	for (;;)
	{
		fl::cmp(T.r[1], T.r[2]);
		tick(0x428E, 12);                              // CMP R1,R2
		if ((int16_t)T.r[1] > (int16_t)T.r[2])
		{
			tick(0x4290, 12);                          // BLE
			break;
		}
		tick(0x41AE, 12);
		T.r[6] = (uint16_t)(T.r[6] - 2);               // MOVB @#0x01F3,-(SP)
		pokeb(T.r[6], peekb(0x01F3));
		fl::movb(peekb(T.r[6]));
		tick(0x41B2, 36);
		poke(T.r[6], fl::logic((uint16_t)(peek(T.r[6]) & 0x0003)));
		tick(0x41B6, 27);
		push(0x0005);
		fl::mov(0x0005);
		tick(0x41BA, 30);
		jsr(0x41BE, 0x80D8);                           // mul16
		pokeb(0x0FE3, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x41C2, 27);
		T.r[0] = 0;
		fl::clr();
		tick(0x41C4, 12);
		T.r[0] = peekb(0x01F3);
		fl::logicb((uint8_t)T.r[0]);
		tick(0x41C8, 24);
		T.r[0] = fl::asr(T.r[0]);
		tick(0x41CA, 12);
		T.r[0] = fl::asr(T.r[0]);
		tick(0x41CC, 12);
		poke(T.r[6], T.r[0]);
		fl::mov(T.r[0]);
		tick(0x41CE, 21);
		poke(T.r[6], fl::logic((uint16_t)(peek(T.r[6]) & 0x0003)));
		tick(0x41D2, 27);
		push(0x000B);
		fl::mov(0x000B);
		tick(0x41D6, 30);
		jsr(0x41DA, 0x80D8);                           // mul16
		pokeb(0x0FE4, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x41DE, 27);
		T.r[0] = sext8(peekb(0x01F4));
		fl::movb((uint8_t)T.r[0]);
		tick(0x41E2, 24);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0003));
		tick(0x41E6, 18);
		T.r[0] = sext8(peekb((uint16_t)(0x523A + T.r[0])));
		fl::movb((uint8_t)T.r[0]);
		tick(0x41EA, 27);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
		tick(0x41EE, 18);
		pokeb(0x025B, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x41F2, 27);
		pokeb(0x0FE5, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x41F6, 27);
		pokeb(0x025A, 0);
		fl::clr();
		tick(0x41FA, 27);
		T.r[0] = 0x3800;
		fl::mov(T.r[0]);
		tick(0x41FE, 18);
		poke(0x01D2, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4202, 27);
		poke(0x1400, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4206, 27);
		T.r[0] = 0;
		fl::clr();
		tick(0x4208, 12);
		T.r[0] = peekb(0x0FE3);
		fl::logicb((uint8_t)T.r[0]);
		tick(0x420C, 24);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x420E, 12);
		poke(0x0244, peek((uint16_t)(0x5212 + T.r[0])));
		fl::mov(peek(0x0244));
		tick(0x4214, 42);
		poke(0x0246, 0);
		fl::clr();
		tick(0x4218, 27);
		poke(0x0248, peek((uint16_t)(0x5212 + T.r[0])));
		fl::mov(peek(0x0248));
		tick(0x421E, 42);
		uint16_t pc = 0x421E;
		for (uint16_t a : {0x024A, 0x024C, 0x024E, 0x023E, 0x0240})
		{
			poke(a, 0);
			pc = (uint16_t)(pc + 4);
			tick(pc, 27);
		}
		poke(0x0256, 0x0064);
		tick(0x4238, 33);
		pokeb(0x0F30, 0);
		fl::clr();
		tick(0x423C, 27);
		pokeb(0x0F31, 0);
		fl::clr();
		tick(0x4240, 27);
		pokeb(0x027B, 1);
		fl::movb(0x01);
		tick(0x4246, 33);
		T.r[0] = 0x2800;
		fl::mov(T.r[0]);
		tick(0x424A, 18);
		poke(0x01D4, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x424E, 27);
		poke(0x1402, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4252, 27);
		T.r[0] = 4;
		fl::mov(T.r[0]);
		tick(0x4256, 18);
		for (;;)
		{
			pokeb((uint16_t)(0x0274 + T.r[0]), peekb((uint16_t)(0x7C7A + T.r[0])));
			tick(0x425C, 45);
			T.r[0] = fl::dec(T.r[0]);
			tick(0x425E, 12);
			if (T.r[0] & 0x8000)
			{
				tick(0x4260, 12);                      // BGE
				break;
			}
			tick(0x4256, 12);
		}
		pc = 0x4260;
		for (uint16_t a : {0x025F, 0x025C, 0x025E, 0x025D})
		{
			pokeb(a, 0);
			pc = (uint16_t)(pc + 4);
			tick(pc, 27);
		}
		pokeb((uint16_t)(0x0F3A + T.r[1]), 0);
		tick(0x4274, 30);
		jsr(0x4278, 0x9152);                           // next_random
		pokeb(0x0261, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x427C, 27);
		const bool two = peekb(0x01F6) != 0;
		fl::tstb(peekb(0x01F6));
		tick(0x4280, 24);                              // TSTB @#0x01F6
		if (two)
		{
			tick(0x4282, 12);
			jsr(0x4286, 0xE730);
		}
		else
			tick(0x4286, 12);                          // BEQ
		T.r[6] = fl::add(T.r[6], 6);
		tick(0x428A, 18);
		T.r[1] = fl::inc(T.r[1]);
		tick(0x428C, 12);
	}
	poke(0x020A, (uint16_t)(peek(0x020A) + 1));
	tick(0x4294, 27);
	uint16_t pc = 0x4294;
	for (uint16_t a : {0x0F29, 0x0F2A, 0x0ED6, 0x0EA4, 0x0F37, 0x0ED7, 0x0ED8, 0x0279, 0x0272})
	{
		pokeb(a, 0);
		pc = (uint16_t)(pc + 4);
		tick(pc, 27);
	}
	poke(0x01CE, 0);
	tick(0x42BC, 27);
	jsr(0x42C0, 0xBA2E);
	jsr(0x42C4, 0xCFD6);
	push(0);
	fl::clr();
	tick(0x42C6, 24);
	jsr(0x42CA, 0xDC24);                               // wait_fade(0)
	poke(0x0EB0, 0xDABA);
	fl::mov(0xDABA);
	tick(0x42D0, 33);
	jsr(0x42D4, 0x8D50);                               // wait_vblank
	poke(0x0EB0, 0xDA74);
	fl::mov(0xDA74);
	tick(0x42DA, 33);
	jsr(0x42DE, 0x8D50);
	jsr(0x42E2, 0x8E6A);                               // set_flag_01a4
	poke(L, 0x0020);
	fl::mov(0x0020);
	tick(0x42E8, 36);
	for (;;)
	{
		jsr(0x42EC, 0x910C);                           // read_sound_status
		poke(L, (uint16_t)(peek(L) - 1));
		const bool more = peek(L) != 0;
		fl::dec((uint16_t)(peek((uint16_t)(T.r[6] + 6)) + 1));
		tick(0x42F0, 30);                              // DEC 6(SP)
		if (!more)
		{
			tick(0x42F2, 12);                          // BNE
			break;
		}
		tick(0x42E8, 12);
	}
	jsr(0x42F6, 0x8E72);                               // clear_flag_01a4
	poke(0x01D2, peek((uint16_t)(E - 4)));
	fl::mov(peek(0x01D2));
	tick(0x42FC, 42);
	poke(0x01D4, peek((uint16_t)(E - 6)));
	fl::mov(peek(0x01D4));
	tick(0x4302, 42);
	poke(0x1400, peek(0x01D2));
	fl::mov(peek(0x01D2));
	tick(0x4308, 39);
	poke(0x1402, peek(0x01D4));
	fl::mov(peek(0x01D4));
	tick(0x430E, 39);
	T.r[6] = fl::add(T.r[6], 0x0008);
	tick(0x4312, 18);
	rts();
}
// ---------------------------------------------------------------- the downhill park (level 3)
// The course is a band of zones 0x0ED0 (0-5); 0x0ED2/0x0ED4 accumulate the skater's position
// along / across it from the velocities.

// A5:5844  slope_event(int t): 0x0222 = 1; on the ground with t = 15 the skater bounces back
// off the wall (velocities halved and reversed, direction mirrored at 4) when moving into it;
// otherwise t becomes the terrain 0x0329 and, if it changed (0x0328), the slope value of
// F:9E52 is applied (F:9EEE in the air, F:9E96 on the ground)
uint16_t slope_event(const Args &a)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x5846, 24);
	poke(0x0222, 0x0001);
	fl::mov(0x0001);
	tick(0x584C, 33);
	const bool air = peekb(0x01EA) != 0;
	fl::tstb(peekb(0x01EA));
	tick(0x5850, 24);                                  // TSTB @#0x01EA
	bool wall = false;
	if (air)
		tick(0x589A, 12);                              // BNE
	else
	{
		tick(0x5852, 12);
		wall = a.u(0) == 0x000F;
		fl::cmp(peek((uint16_t)(T.r[6] + 4)), 0x000F);
		tick(0x5858, 33);                              // CMP 4(SP),#15
		tick(wall ? 0x585A : 0x589A, 12);              // BNE
	}
	if (wall)
	{
		T.r[0] = peek(0x0570);
		tick(0x585E, 24);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x5860, 12);
		T.r[0] = fl::sub(T.r[0], peek(0x04F0));
		tick(0x5864, 24);
		tick(0x5866, 18);                              // CCC CV
		if ((T.r[0] & 0x8000) || !T.r[0])
			tick(0x58CC, 12);                          // BLE
		else
		{
			tick(0x5868, 12);
			T.r[0] = peek(0x04F0);
			fl::mov(T.r[0]);
			tick(0x586C, 24);
			T.r[0] = fl::asr(T.r[0]);
			tick(0x586E, 12);
			T.r[0] = (uint16_t)-T.r[0];
			fl::neg((uint16_t)(-T.r[0]));
			tick(0x5870, 12);
			poke(0x04F0, T.r[0]);
			fl::mov(T.r[0]);
			tick(0x5874, 27);
			T.r[0] = peek(0x0570);
			fl::mov(T.r[0]);
			tick(0x5878, 24);
			T.r[0] = fl::asr(T.r[0]);
			tick(0x587A, 12);
			T.r[0] = (uint16_t)-T.r[0];
			fl::neg((uint16_t)(-T.r[0]));
			tick(0x587C, 12);
			poke(0x0570, T.r[0]);
			fl::mov(T.r[0]);
			tick(0x5880, 27);
			T.r[0] = 0x0004;
			fl::mov(T.r[0]);
			tick(0x5884, 18);
			T.r[1] = sext8(peekb(0x031D));
			fl::movb((uint8_t)T.r[1]);
			tick(0x5888, 24);
			T.r[0] = fl::sub(T.r[0], T.r[1]);
			tick(0x588A, 12);
			T.r[1] = T.r[0];
			fl::mov(T.r[1]);
			tick(0x588C, 12);
			T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x000F));
			tick(0x5890, 18);
			pokeb(0x031D, 0);
			fl::clr();
			tick(0x5894, 27);
			pokeb(0x031D, fl::logicb((uint8_t)(peekb(0x031D) | T.r[1])));
			tick(0x5898, 27);
			tick(0x58CC, 12);                          // BR
		}
	}
	else
	{
		T.r[0] = a.u(0);
		tick(0x589E, 27);
		pokeb(0x0329, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x58A2, 27);
		T.r[1] = 0;
		fl::clr();
		tick(0x58A4, 12);
		T.r[1] = peekb(0x0328);
		fl::logicb((uint8_t)T.r[1]);
		tick(0x58A8, 24);
		fl::cmp(T.r[1], T.r[0]);
		tick(0x58AA, 12);                              // CMP R1,R0
		if (T.r[1] == T.r[0])
			tick(0x58CC, 12);                          // BEQ
		else
		{
			tick(0x58AC, 12);
			pokeb(0x0328, (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0x58B0, 27);
			push(T.r[0]);
			fl::mov(T.r[0]);
			tick(0x58B2, 24);
			jsr(0x58B6, 0x9E52);                       // slope_table_value
			const bool in_air = peekb(0x01EA) != 0;
			fl::tstb(peekb(0x01EA));
			tick(0x58BA, 24);                          // TSTB @#0x01EA
			if (in_air)
			{
				tick(0x58BC, 12);
				poke(T.r[6], T.r[0]);
				fl::mov(T.r[0]);
				tick(0x58BE, 21);
				jsr(0x58C2, 0x9EEE);
				tick(0x58CA, 12);                      // BR
			}
			else
			{
				tick(0x58C4, 12);                      // BEQ
				poke(T.r[6], T.r[0]);
				fl::mov(T.r[0]);
				tick(0x58C6, 21);
				jsr(0x58CA, 0x9E96);
			}
			T.r[6] = (uint16_t)(T.r[6] + 2);
			fl::tst(peek((uint16_t)(T.r[6] - 2)));
			tick(0x58CC, 18);                          // TST (SP)+
		}
	}
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x58CE, 18);
	rts();
	return T.r[0];
}

// A5:58D0 (assembly)  downhill_zone(): advance 0x0ED4/0x0ED2 by the velocities (x - 2y, -x - 2y,
// scaled 2:1 by the slope bytes 0x02E3/0x02E5), classify the position (R1: 0-2 inside the
// band by 0x0ED4, 3-6 beyond its ends) and move to the next zone from the table 0x7CD6
// (bank B 0x2C00); its upper bits select the zone's action (jump table 0x5A3E: start height
// 0x1500 and a push, F:9DE4, a terrain event A5:5844, or the finish at height 0x2800).
// 0x06B0 gets the zone's height from 0x7D06; 0x032E/0x032F are updated.  R0 = 1 beyond the
// band's ends.
void downhill_zone()
{
	T.r[0] = 0x2C00;
	fl::mov(T.r[0]);
	tick(0x58D4, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x58D8, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x58DC, 27);
	T.r[0] = sext8(peekb(0x02E5));
	fl::movb((uint8_t)T.r[0]);
	tick(0x58E0, 24);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x58E2, 12);
	T.r[1] = sext8(peekb(0x02E3));
	fl::movb((uint8_t)T.r[1]);
	tick(0x58E6, 24);
	T.r[1] = fl::add(T.r[1], T.r[0]);
	tick(0x58E8, 12);
	T.r[2] = T.r[1];
	fl::mov(T.r[2]);
	tick(0x58EA, 12);
	for (uint16_t pc = 0x58EC; pc <= 0x58F0; pc = (uint16_t)(pc + 2))
	{
		T.r[2] = (uint16_t)(T.r[2] << 1);
		tick(pc, 12);
	}
	T.r[3] = peek(0x0570);
	fl::mov(T.r[3]);
	tick(0x58F4, 24);
	T.r[3] = fl::asl(T.r[3]);
	tick(0x58F6, 12);
	T.r[1] = T.r[3];
	fl::mov(T.r[1]);
	tick(0x58F8, 12);
	T.r[1] = fl::add(T.r[1], peek(0x0ED4));
	tick(0x58FC, 24);
	T.r[1] = fl::add(T.r[1], peek(0x04F0));
	tick(0x5900, 24);
	poke(0x0ED4, T.r[1]);
	fl::mov(T.r[1]);
	tick(0x5904, 27);
	T.r[2] = fl::add(T.r[2], T.r[1]);
	tick(0x5906, 12);
	T.r[1] = sext8(peekb(0x02E3));
	fl::movb((uint8_t)T.r[1]);
	tick(0x590A, 24);
	T.r[0] = fl::sub(T.r[0], T.r[1]);
	tick(0x590C, 12);
	for (uint16_t pc = 0x590E; pc <= 0x5912; pc = (uint16_t)(pc + 2))
	{
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(pc, 12);
	}
	T.r[1] = T.r[3];
	fl::mov(T.r[1]);
	tick(0x5914, 12);
	T.r[1] = fl::add(T.r[1], peek(0x0ED2));
	tick(0x5918, 24);
	T.r[1] = fl::sub(T.r[1], peek(0x04F0));
	tick(0x591C, 24);
	poke(0x0ED2, T.r[1]);
	fl::mov(T.r[1]);
	tick(0x5920, 27);
	T.r[0] = fl::add(T.r[0], T.r[1]);
	tick(0x5922, 12);
	T.r[3] = 0;
	fl::clr();
	tick(0x5924, 12);
	fl::cmp(T.r[2], 0xB600);
	tick(0x5928, 18);                                  // CMP R2,#0xB600
	bool beyond = T.r[2] > 0xB600;
	if (beyond)
		tick(0x5952, 12);                              // BHI
	else
	{
		tick(0x592A, 12);
		T.r[3] = fl::inc(T.r[3]);
		tick(0x592C, 12);
		fl::cmp(T.r[0], 0x6C00);
		tick(0x5930, 18);                              // CMP R0,#0x6C00
		beyond = T.r[0] > 0x6C00;
		if (beyond)
			tick(0x5952, 12);                          // BHI
		else
		{
			tick(0x5932, 12);
			T.r[1] = 0;
			fl::clr();
			tick(0x5934, 12);
			fl::cmp(T.r[2], 0x7800);
			tick(0x5938, 18);                          // CMP R2,#0x7800
			bool inside = false;
			if (T.r[2] < 0x7800)
			{
				tick(0x593A, 12);                      // BCC
				fl::cmp(T.r[2], 0x3800);
				tick(0x593E, 18);                      // CMP R2,#0x3800
				inside = T.r[2] > 0x3800;
				tick(inside ? 0x597E : 0x5940, 12);    // BHI
			}
			else
				tick(0x5940, 12);
			if (!inside)
			{
				T.r[1] = fl::inc(T.r[1]);
				tick(0x5942, 12);
				fl::cmp(T.r[2], 0xA200);
				tick(0x5946, 18);                      // CMP R2,#0xA200
				if (T.r[2] < 0xA200)
				{
					tick(0x5948, 12);                  // BCC
					fl::cmp(T.r[2], 0x1400);
					tick(0x594C, 18);                  // CMP R2,#0x1400
					inside = T.r[2] > 0x1400;
					tick(inside ? 0x597E : 0x594E, 12); // BHI
				}
				else
					tick(0x594E, 12);
				if (!inside)
				{
					T.r[1] = fl::inc(T.r[1]);
					tick(0x5950, 12);
					tick(0x597E, 12);                  // BR
				}
			}
		}
	}
	if (beyond)
	{
		fl::cmp(T.r[0], 0xB000);
		tick(0x5956, 18);                              // CMP R0,#0xB000
		if (T.r[0] >= 0xB000)
		{
			tick(0x5958, 12);                          // BCS
			T.r[1] = 0x0003;
			fl::mov(T.r[1]);
			tick(0x595C, 18);
			tick(0x597E, 12);                          // BR
		}
		else
		{
			tick(0x595E, 12);
			fl::cmp(T.r[0], 0x6C00);
			tick(0x5962, 18);                          // CMP R0,#0x6C00
			bool five = false;
			if (T.r[0] > 0x6C00)
			{
				tick(0x5964, 12);                      // BLOS
				five = (T.r[3] & 1) != 0;              // ROR R3 (the carry is clear after the CMP)
				T.r[3] = (uint16_t)(T.r[3] >> 1);
				tick(0x5966, 12);
				if (five)
				{
					tick(0x5968, 12);                  // BCC
					T.r[1] = 0x0005;
					fl::mov(T.r[1]);
					tick(0x596C, 18);
					tick(0x597E, 12);                  // BR
				}
				else
					tick(0x596E, 12);
			}
			else
				tick(0x596E, 12);                      // BLOS
			if (!five)
			{
				fl::cmp(T.r[2], 0xBE00);
				tick(0x5972, 18);                      // CMP R2,#0xBE00
				if (T.r[2] >= 0xBE00)
				{
					tick(0x5974, 12);                  // BCS
					T.r[1] = 0x0004;
					fl::mov(T.r[1]);
					tick(0x5978, 18);
					tick(0x597E, 12);                  // BR
				}
				else
				{
					tick(0x597A, 12);
					T.r[1] = 0x0006;
					fl::mov(T.r[1]);
					tick(0x597E, 18);
				}
			}
		}
	}
	T.r[0] = 0;
	fl::clr();
	tick(0x5980, 12);
	T.r[0] = peekb(0x0ED0);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x5984, 24);
	for (uint16_t pc = 0x5986; pc <= 0x598A; pc = (uint16_t)(pc + 2))
	{
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(pc, 12);
	}
	T.r[0] = fl::add(T.r[0], T.r[1]);
	tick(0x598C, 12);
	T.r[3] = 0;
	fl::clr();
	tick(0x598E, 12);
	T.r[3] = peekb((uint16_t)(0x7CD6 + T.r[0]));
	fl::logicb((uint8_t)T.r[3]);
	tick(0x5992, 27);
	T.r[1] = T.r[3];
	fl::mov(T.r[1]);
	tick(0x5994, 12);
	T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x0007));
	tick(0x5998, 18);
	T.r[0] = 0;
	fl::clr();
	tick(0x599A, 12);
	T.r[0] = peekb(0x0ED0);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x599E, 24);
	fl::cmp(T.r[0], T.r[1]);
	tick(0x59A0, 12);                                  // CMP R0,R1
	if (T.r[0] != T.r[1])
	{
		tick(0x59A2, 12);                              // BEQ
		pokeb(0x0ED0, (uint8_t)T.r[1]);
		fl::movb((uint8_t)T.r[1]);
		tick(0x59A6, 27);
	}
	else
		tick(0x59A6, 12);
	T.r[0] = T.r[3];
	fl::mov(T.r[0]);
	tick(0x59A8, 12);
	for (uint16_t pc = 0x59AA; pc <= 0x59AE; pc = (uint16_t)(pc + 2))
	{
		T.r[0] = asr(T.r[0], 1);
		tick(pc, 12);
	}
	T.r[0] = (uint16_t)(T.r[0] << 1);
	tick(0x59B0, 12);
	// the zone's action (jump table 0x5A3E; actions above 8 are not used by the game and do
	// nothing here)
	static const uint16_t to[9] = {0x59FA, 0x59B4, 0x59FA, 0x59C2, 0x59C8, 0x59C8, 0x59C8, 0x59C8, 0x59D4};
	const uint16_t k = (uint16_t)(T.r[0] >> 1);
	const uint16_t at = k < 9 ? to[k] : 0x59FA;
	tick(at, 27);                                      // ADD 0x5A3E(R0),PC
	switch (at)
	{
	case 0x59B4:                                       // the start
		poke(0x06B0, 0x1500);
		tick(0x59BA, 33);
		poke(0x05B2, 0xFF80);
		fl::mov(0xFF80);
		tick(0x59C0, 33);
		tick(0x59FA, 12);                              // BR
		break;
	case 0x59C2:
		jsr(0x59C6, 0x9DE4);
		tick(0x59FA, 12);                              // BR
		break;
	case 0x59C8:                                       // a terrain event
		T.r[3] = (uint16_t)(T.r[3] - 0x0020);
		tick(0x59CC, 18);
		push(T.r[3]);
		fl::mov(T.r[3]);
		tick(0x59CE, 24);
		poke(T.r[6], fl::logic((uint16_t)(peek(T.r[6]) | 0x0007)));
		tick(0x59D2, 27);
		tick(0x59F4, 12);                              // BR
		jsr(0x59F8, 0x5844);                           // slope_event
		T.r[6] = (uint16_t)(T.r[6] + 2);
		tick(0x59FA, 18);                              // TST (SP)+
		break;
	case 0x59D4:                                       // the finish
	{
		const bool high = (int16_t)peek(0x06B2) >= 0x2800;
		fl::cmp(peek(0x06B2), 0x2800);
		tick(0x59DA, 30);                              // CMP @#0x06B2,#0x2800
		if (high)
		{
			tick(0x59DC, 12);                          // BLT
			pokeb(0x0328, 0x0F);
			fl::movb(0x0F);
			tick(0x59E2, 33);
			pokeb(0x031C, 3);
			fl::movb(0x03);
			tick(0x59E8, 33);
			pokeb(0x0ED0, 5);
			fl::movb(0x05);
			tick(0x59EE, 33);
			tick(0x59FA, 12);                          // BR
		}
		else
		{
			tick(0x59F0, 12);
			push(0x000F);
			fl::mov(0x000F);
			tick(0x59F4, 30);
			jsr(0x59F8, 0x5844);                       // slope_event
			T.r[6] = (uint16_t)(T.r[6] + 2);
			tick(0x59FA, 18);                          // TST (SP)+
		}
		break;
	}
	default:
		break;
	}
	T.r[0] = 0;
	fl::clr();
	tick(0x59FC, 12);
	T.r[0] = peekb(0x0ED0);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x5A00, 24);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x5A02, 12);
	const bool keep = peek((uint16_t)(0x7D06 + T.r[0])) == 0x0FFF;
	fl::cmp(peek((uint16_t)(T.r[0] + 0x7D06)), 0x0FFF);
	tick(0x5A08, 33);                                  // CMP 0x7D06(R0),#0x0FFF
	if (!keep)
	{
		tick(0x5A0A, 12);                              // BEQ
		poke(0x06B0, peek((uint16_t)(0x7D06 + T.r[0])));
		fl::mov(peek(0x06B0));
		tick(0x5A10, 42);
	}
	else
		tick(0x5A10, 12);
	fl::cmp(T.r[1], 0x0002);
	tick(0x5A14, 18);
	if ((int16_t)T.r[1] > 2)
	{
		tick(0x5A36, 12);                              // BGT
		T.r[4] = 1;
		fl::mov(T.r[4]);
		tick(0x5A3A, 18);
	}
	else
	{
		tick(0x5A16, 12);
		pokeb(0x032E, 0);
		fl::clr();
		tick(0x5A1A, 27);
		T.r[4] = 0;
		fl::clr();
		tick(0x5A1C, 12);
		fl::cmp(T.r[1], 0x0002);
		tick(0x5A20, 18);
		bool set = false;
		if (T.r[1] != 2)
		{
			tick(0x5A22, 12);                          // BEQ
			fl::cmp(T.r[2], 0x5800);
			tick(0x5A26, 18);
			set = !(T.r[2] > 0x5800);
			tick(set ? 0x5A28 : 0x5A30, 12);           // BHI
		}
		else
			tick(0x5A30, 12);
		if (set)
		{
			pokeb(0x032F, 1);
			fl::movb(0x01);
			tick(0x5A2E, 33);
			tick(0x5A3A, 12);                          // BR
		}
		else
		{
			pokeb(0x032F, 0);
			fl::clr();
			tick(0x5A34, 27);
			tick(0x5A3A, 12);                          // BR
		}
	}
	T.r[0] = T.r[4];
	fl::mov(T.r[0]);
	tick(0x5A3C, 12);
	rts();
}

// A5:5A50 (assembly)  downhill_height(): in zone 1 the height 0x06B0 follows the slope 0x02E7
// (capped at 0x1500), and the sprite table 0x7C48 (bank B 0x2C00) gives the skater's frame
// (+0x10 when 0x032F); high up (0x06B2 > 0x1800) 0x0ED4 is set to 0xA200 / 0x1400 and 0x0222
// cleared.  Zones 2 and 5 use frame 0x78, others keep 0x0304.  0x0302-0x0306 get the frame.
void downhill_height()
{
	T.r[0] = peek(0x0304);
	fl::mov(T.r[0]);
	tick(0x5A54, 24);
	T.r[2] = 0;
	fl::clr();
	tick(0x5A56, 12);
	T.r[2] = peekb(0x0ED0);
	fl::logicb((uint8_t)T.r[2]);
	tick(0x5A5A, 24);
	fl::cmp(T.r[2], 0x0001);
	tick(0x5A5E, 18);
	if (T.r[2] == 1)
	{
		tick(0x5A60, 12);
		T.r[1] = 0x2C00;
		fl::mov(T.r[1]);
		tick(0x5A64, 18);
		poke(0x01D4, T.r[1]);
		fl::mov(T.r[1]);
		tick(0x5A68, 27);
		poke(0x1402, T.r[1]);
		fl::mov(T.r[1]);
		tick(0x5A6C, 27);
		const bool high = (int16_t)peek(0x06B0) >= 0x1500;
		fl::cmp(peek(0x06B0), 0x1500);
		tick(0x5A72, 30);                              // CMP @#0x06B0,#0x1500
		if (high)
		{
			tick(0x5A74, 12);
			poke(0x06B0, 0x1500);
			fl::mov(0x1500);
			tick(0x5A7A, 33);
			T.r[0] = 0x0017;
			fl::mov(T.r[0]);
			tick(0x5A7E, 18);
			tick(0x5A9E, 12);                          // BR
		}
		else
		{
			tick(0x5A80, 12);                          // BLT
			T.r[1] = sext8(peekb(0x02E7));
			fl::movb((uint8_t)T.r[1]);
			tick(0x5A84, 24);
			if (T.r[1] & 0x8000)
			{
				tick(0x5A86, 12);                      // BGE
				T.r[1] = (uint16_t)-T.r[1];
				fl::neg((uint16_t)(-T.r[1]));
				tick(0x5A88, 12);
			}
			else
				tick(0x5A88, 12);
			T.r[1] = fl::asl(T.r[1]);
			tick(0x5A8A, 12);
			T.r[1] = fl::asl(T.r[1]);
			tick(0x5A8C, 12);
			T.r[1] = fl::asl(T.r[1]);
			tick(0x5A8E, 12);
			T.r[1] = fl::add(T.r[1], peek(0x06B0));
			tick(0x5A92, 24);
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0x5A94, 12);
			T.r[0] &= 0xFF00;
			fl::clr();
			tick(0x5A96, 12);                          // CLRB R0
			T.r[0] = (uint16_t)(T.r[0] >> 8);
			tick(0x5A98, 12);                          // SWAB R0
			T.r[0] = sext8((uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0x5A9A, 12);                          // MOVB R0,R0
			if (T.r[0] & 0x8000)
			{
				tick(0x5A9C, 12);                      // BGE
				T.r[0] = 0;
				fl::clr();
				tick(0x5A9E, 12);
			}
			else
				tick(0x5A9E, 12);
		}
		const bool alt = peekb(0x032F) != 0;
		fl::tstb(peekb(0x032F));
		tick(0x5AA2, 24);                              // TSTB @#0x032F
		if (alt)
		{
			tick(0x5AA8, 12);                          // BNE
			T.r[1] = 0x0010;
			fl::mov(T.r[1]);
			tick(0x5AAC, 18);
		}
		else
		{
			tick(0x5AA4, 12);
			T.r[1] = 0;
			fl::clr();
			tick(0x5AA6, 12);
			tick(0x5AAC, 12);                          // BR
		}
		T.r[2] = 0;
		fl::clr();
		tick(0x5AAE, 12);
		T.r[2] = peekb((uint16_t)(0x7C48 + T.r[0]));
		fl::logicb((uint8_t)T.r[2]);
		tick(0x5AB2, 27);
		T.r[1] = fl::logic((uint16_t)(T.r[1] | T.r[2]));
		tick(0x5AB4, 12);
		T.r[0] = T.r[1];
		fl::mov(T.r[0]);
		tick(0x5AB6, 12);
		const bool up = (int16_t)peek(0x06B2) > 0x1800;
		fl::cmp(peek(0x06B2), 0x1800);
		tick(0x5ABC, 30);                              // CMP @#0x06B2,#0x1800
		if (up)
		{
			tick(0x5ABE, 12);
			const bool alt2 = peekb(0x032F) != 0;
			fl::tstb(peekb(0x032F));
			tick(0x5AC2, 24);                          // TSTB @#0x032F
			if (alt2)
			{
				tick(0x5ACC, 12);                      // BNE
				poke(0x0ED4, 0x1400);
				fl::mov(0x1400);
				tick(0x5AD2, 33);
			}
			else
			{
				tick(0x5AC4, 12);
				poke(0x0ED4, 0xA200);
				fl::mov(0xA200);
				tick(0x5ACA, 33);
				tick(0x5AD2, 12);                      // BR
			}
		}
		else
			tick(0x5AD2, 12);                          // BLE
		poke(0x0222, 0);
		fl::clr();
		tick(0x5AD6, 27);
		tick(0x5AE8, 12);                              // BR
	}
	else
	{
		tick(0x5AD8, 12);                              // BNE
		fl::cmp(T.r[2], 0x0005);
		tick(0x5ADC, 18);
		bool frame78 = T.r[2] == 5;
		if (frame78)
			tick(0x5AE4, 12);                          // BEQ
		else
		{
			tick(0x5ADE, 12);
			fl::cmp(T.r[2], 0x0002);
			tick(0x5AE2, 18);
			frame78 = T.r[2] == 2;
			tick(frame78 ? 0x5AE4 : 0x5AE8, 12);       // BNE
		}
		if (frame78)
		{
			T.r[0] = 0x0078;
			fl::mov(T.r[0]);
			tick(0x5AE8, 18);
		}
	}
	poke(0x0306, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x5AEC, 27);
	poke(0x0304, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x5AF0, 27);
	poke(0x0302, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x5AF4, 27);
	rts();
}

// ---------------------------------------------------------------- tricks
// The trick moves 0x40-0x57 of 0x01EB (set up by A5:5264 on a ramp) play an animation from the
// table 0x01E0 (frames 0x01E2/0x01E4, step 0x01EE, 0x07B2) and change the skater's state.
// The test scenario 'tricks' sets the moves directly; A5:5264 itself is not reached by the
// tests.

// A5:5388  trick_facing(): 1 when the skater's direction 0x01F8 is within 2 of 6, else 0
uint16_t trick_facing(const Args &)
{
	T.r[0] = sext8(peekb(0x01F8));
	fl::movb((uint8_t)T.r[0]);
	tick(0x538C, 24);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0007));
	tick(0x5390, 18);
	T.r[0] = fl::sub(T.r[0], 6);
	tick(0x5394, 18);
	if (T.r[0] & 0x8000)
	{
		tick(0x5396, 12);                              // BPL
		T.r[0] = (uint16_t)-T.r[0];
		fl::neg((uint16_t)(-T.r[0]));
		tick(0x5398, 12);
	}
	else
		tick(0x5398, 12);
	fl::cmp(T.r[0], 0x0002);
	tick(0x539C, 18);                                  // CMP R0,#2
	if ((int16_t)T.r[0] > 2)
	{
		tick(0x53A4, 12);                              // BGT
		T.r[0] = 0;
		fl::clr();
		tick(0x53A6, 12);
	}
	else
	{
		tick(0x539E, 12);
		T.r[0] = 1;
		fl::mov(T.r[0]);
		tick(0x53A2, 18);
	}
	rts();
	return T.r[0];
}

// A5:543E  next_trick_frame(): advance the animation (0x01E2/0x01E4 += 2, 0x01EE + 1 per
// step) to the next entry of the table 0x01E0 + 0x07B2 that ends a sequence (negative byte);
// returns its address
uint16_t next_trick_frame(const Args &)
{
	T.r[0] = 0;
	fl::clr();
	tick(0x5440, 12);
	T.r[0] = peekb(0x07B2);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x5444, 24);
	T.r[0] = fl::add(T.r[0], peek(0x01E0));
	tick(0x5448, 24);
	T.r[0] = fl::dec(T.r[0]);
	tick(0x544A, 12);
	for (;;)
	{
		T.r[0] = fl::inc(T.r[0]);
		tick(0x544C, 12);
		poke(0x01E2, fl::add(peek(0x01E2), 2));
		tick(0x5452, 33);
		poke(0x01E4, fl::add(peek(0x01E4), 2));
		tick(0x5458, 33);
		pokeb(0x01EE, fl::incb(peekb(0x01EE)));
		tick(0x545C, 27);
		const bool last = (peekb(T.r[0]) & 0x80) != 0;
		fl::tstb(peekb(T.r[0]));
		tick(0x545E, 18);                              // TSTB (R0)
		if (last)
		{
			tick(0x5460, 12);                          // BGE
			break;
		}
		tick(0x544A, 12);
	}
	rts();
	return T.r[0];
}

// A5:53A8  trick_landing(int dir): unless already landing (0x01F0) or 0x0324 bit 2: when the
// direction dir (mirrored by 0x0327 bit 4) is more than 2 off the skater's 0x01F8, or the
// terrain is not a full slope, the landing starts: 0x01F0 = 4 (flat) or 7 (slope), the
// sprite flags 0x0771/0x0772 get bit 7 and, unless set, gravity 0x02EE = 12 with the vertical
// speed 0x05B2 = 0x01DE
uint16_t trick_landing(const Args &a)
{
	c_save(2, 0x53AC);
	T.r[2] = 0;
	fl::clr();
	tick(0x53AE, 12);
	const bool landing = peekb(0x01F0) != 0;
	fl::tstb(peekb(0x01F0));
	tick(0x53B2, 24);                                  // TSTB @#0x01F0
	if (landing)
		tick(0x53F4, 12);                              // BNE
	else
	{
		tick(0x53B4, 12);
		const bool held = (peekb(0x0324) & 0x04) != 0;
		fl::logicb((uint8_t)(0x04 & peekb(0x0324)));
		tick(0x53BA, 30);                              // BITB #4,@#0x0324
		if (held)
			tick(0x53F4, 12);                          // BNE
		else
		{
			tick(0x53BC, 12);
			T.r[1] = a.u(0);
			fl::mov(T.r[1]);
			tick(0x53C0, 27);
			T.r[1] = fl::logic((uint16_t)(T.r[1] & 0x000F));
			tick(0x53C4, 18);
			const bool mirror = (peekb(0x0327) & 0x10) != 0;
			fl::logicb((uint8_t)(0x10 & peekb(0x0327)));
			tick(0x53CA, 30);                          // BITB #0x10,@#0x0327
			if (mirror)
			{
				tick(0x53CC, 12);
				T.r[0] = 0x0008;
				fl::mov(T.r[0]);
				tick(0x53D0, 18);
				T.r[1] ^= T.r[0];
				fl::logic(T.r[1]);
				tick(0x53D2, 12);
			}
			else
				tick(0x53D2, 12);                      // BEQ
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0x53D4, 12);
			T.r[3] = sext8(peekb(0x01F8));
			fl::movb((uint8_t)T.r[3]);
			tick(0x53D8, 24);
			T.r[0] = fl::sub(T.r[0], T.r[3]);
			tick(0x53DA, 12);
			if (T.r[0] & 0x8000)
			{
				tick(0x53DC, 12);                      // BPL
				T.r[0] = (uint16_t)-T.r[0];
				fl::neg((uint16_t)(-T.r[0]));
				tick(0x53DE, 12);
			}
			else
				tick(0x53DE, 12);
			T.r[1] = T.r[0];
			fl::mov(T.r[1]);
			tick(0x53E0, 12);
			fl::cmp(T.r[1], 0x0008);
			tick(0x53E4, 18);                          // CMP R1,#8
			if ((int16_t)T.r[1] > 8)
			{
				tick(0x53E6, 12);                      // BLE
				T.r[1] = (uint16_t)-T.r[1];
				fl::neg((uint16_t)(-T.r[1]));
				tick(0x53E8, 12);
				T.r[1] = fl::add(T.r[1], 0x0010);
				tick(0x53EC, 18);
			}
			else
				tick(0x53EC, 12);
			fl::cmp(T.r[1], 0x0002);
			tick(0x53F0, 18);                          // CMP R1,#2
			if ((int16_t)T.r[1] > 2)
			{
				tick(0x53F2, 12);                      // BLE
				T.r[2] = fl::inc(T.r[2]);
				tick(0x53F4, 12);
			}
			else
				tick(0x53F4, 12);
		}
	}
	fl::tst(T.r[2]);
	tick(0x53F6, 12);                                  // TST R2
	if (!T.r[2])
	{
		tick(0x53F8, 12);                              // BNE
		T.r[0] = 0x0007;
		fl::mov(T.r[0]);
		tick(0x53FC, 18);
		T.r[0] = (uint16_t)(T.r[0] & ~peekb(0x0327));  // BICB @#0x0327,R0
		fl::logicb((uint8_t)T.r[0]);
		tick(0x5400, 24);
		if (!(uint8_t)T.r[0])
		{
			tick(0x543C, 12);                          // BEQ
			c_restore(2);
			return T.r[0];
		}
		tick(0x5402, 12);
	}
	else
		tick(0x5402, 12);                              // BNE
	const bool slope = (peekb(0x0327) & 7) != 0;
	fl::logicb((uint8_t)(0x07 & peekb(0x0327)));
	tick(0x5408, 30);                                  // BITB #7,@#0x0327
	if (slope)
	{
		tick(0x5412, 12);                              // BNE
		pokeb(0x01F0, 7);
		fl::movb(0x07);
		tick(0x5418, 33);
	}
	else
	{
		tick(0x540A, 12);
		pokeb(0x01F0, 4);
		fl::movb(0x04);
		tick(0x5410, 33);
		tick(0x5418, 12);                              // BR
	}
	T.r[0] = 0;
	fl::clr();
	tick(0x541A, 12);
	T.r[0] = peekb(0x0772);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x541E, 24);
	T.r[0] = fl::logic((uint16_t)(T.r[0] | 0x0080));
	tick(0x5422, 18);
	pokeb(0x0772, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x5426, 27);
	pokeb(0x0771, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x542A, 27);
	const bool gravity = peek(0x02EE) != 0;
	fl::tst(peek(0x02EE));
	tick(0x542E, 24);                                  // TST @#0x02EE
	if (gravity)
		tick(0x543C, 12);                              // BNE
	else
	{
		tick(0x5430, 12);
		poke(0x02EE, 0x000C);
		fl::mov(0x000C);
		tick(0x5436, 33);
		poke(0x05B2, peek(0x01DE));
		fl::mov(peek(0x05B2));
		tick(0x543C, 39);
	}
	c_restore(2);
	return T.r[0];
}

// A5:5462  trick_step(): show the current frame (0x03B2/0x03B4 from 0x01E2/0x01E4, step 0x01EE
// into 0x07B2), clear bit 7 of the sprite flags unless the frame ends a sequence, advance
// (A5:543E), check the landing (A5:53A8) when one was pending, else count 0x01F2; a queued
// move 0x01EF becomes the current one (0x01EB).  Returns 0x01EF.
uint16_t trick_step(const Args &)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x5464, 24);
	pokeb(0x07B1, 0);
	fl::clr();
	tick(0x5468, 27);
	poke(0x03B2, peek(0x01E2));
	fl::mov(peek(0x03B2));
	tick(0x546E, 39);
	poke(0x03B4, peek(0x01E4));
	fl::mov(peek(0x03B4));
	tick(0x5474, 39);
	pokeb(0x07B2, peekb(0x01EE));
	fl::movb(peekb(0x07B2));
	tick(0x547A, 39);
	T.r[0] = 0;
	fl::clr();
	tick(0x547C, 12);
	T.r[0] = peekb(0x07B2);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x5480, 24);
	T.r[0] = fl::add(T.r[0], peek(0x01E0));
	tick(0x5484, 24);
	T.r[1] = sext8(peekb(T.r[0]));
	fl::movb((uint8_t)T.r[1]);
	tick(0x5486, 18);
	if (!(T.r[1] & 0x8000))
	{
		tick(0x5488, 12);                              // BLT
		T.r[0] = peek(0x0772);
		fl::mov(T.r[0]);
		tick(0x548C, 24);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x007F));
		tick(0x5490, 18);
		pokeb(0x0772, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x5494, 27);
		pokeb(0x0771, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x5498, 27);
	}
	else
		tick(0x5498, 12);
	jsr(0x549C, 0x543E);                               // next_trick_frame
	const bool landing = peekb(0x01F0) != 0;
	fl::tstb(peekb(0x01F0));
	tick(0x54A0, 24);                                  // TSTB @#0x01F0
	bool count = true;
	if (landing)
	{
		tick(0x54A2, 12);
		pokeb(0x01F0, 0);
		fl::clr();
		tick(0x54A6, 27);
		push(T.r[1]);
		fl::mov(T.r[1]);
		tick(0x54A8, 24);
		jsr(0x54AC, 0x53A8);                           // trick_landing
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(peek((uint16_t)(T.r[6] - 2)));
		tick(0x54AE, 18);                              // TST (SP)+
		const bool again = peekb(0x01F0) != 0;
		fl::tstb(peekb(0x01F0));
		tick(0x54B2, 24);                              // TSTB @#0x01F0
		if (again)
		{
			tick(0x54B8, 12);                          // BNE
			count = false;
		}
		else
			tick(0x54B4, 12);
	}
	else
		tick(0x54B4, 12);                              // BEQ
	if (count)
	{
		pokeb(0x01F2, fl::incb(peekb(0x01F2)));
		tick(0x54B8, 27);
	}
	T.r[0] = 0;
	fl::clr();
	tick(0x54BA, 12);
	T.r[0] = peekb(0x01EF);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x54BE, 24);
	fl::tst(T.r[0]);
	tick(0x54C0, 12);                                  // TST R0
	if (T.r[0])
	{
		tick(0x54C2, 12);
		pokeb(0x01EB, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x54C6, 27);
		pokeb(0x01EF, 0);
		fl::clr();
		tick(0x54CA, 27);
	}
	else
		tick(0x54CA, 12);                              // BEQ
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x54CC, 18);
	rts();
	return T.r[0];
}

// A5:5264  start_trick(): on a ramp: the jump height class by the vertical speed 0x05B2
// (< 0x120, < 0x180, more; at the top the skater turns around: x velocity from y, objects
// 1-3 follow, A5:5166), the animation speed F:90A6(0x05B2, 0x525E[class]) clamped to 5-9
// in the sprite bytes 0x0771/0x0772/0x0791/0x0792, and the trick (move 0x01EB, next 0x01EF,
// landing 0x01F0, animation 0x01E0/0x01E2/0x01E4, frame 0x01F1) from the tables 0x7D12-0x7DAA
// (bank B 0x2C00) by class and direction.  Sound 0x4C.  Not reached by the tests.
uint16_t start_trick(const Args &)
{
	c_save(1, 0x5268);
	T.r[0] = 0x2C00;
	fl::mov(T.r[0]);
	tick(0x526C, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x5270, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x5274, 27);
	T.r[2] = peek(0x05B2);
	fl::mov(T.r[2]);
	tick(0x5278, 24);
	fl::cmp(T.r[2], 0x0120);
	tick(0x527C, 18);                                  // CMP R2,#0x0120
	if ((int16_t)T.r[2] < 0x0120)
	{
		tick(0x527E, 12);                              // BGE
		T.r[1] = 0;
		fl::clr();
		tick(0x5280, 12);
		tick(0x5292, 12);                              // BR
	}
	else
	{
		tick(0x5282, 12);
		fl::cmp(T.r[2], 0x0180);
		tick(0x5286, 18);                              // CMP R2,#0x0180
		if ((int16_t)T.r[2] < 0x0180)
		{
			tick(0x5288, 12);                          // BGE
			T.r[1] = 1;
			fl::mov(T.r[1]);
			tick(0x528C, 18);
			tick(0x5292, 12);                          // BR
		}
		else
		{
			tick(0x528E, 12);
			T.r[1] = 2;
			fl::mov(T.r[1]);
			tick(0x5292, 18);
		}
	}
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0x5294, 12);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x5296, 12);
	push(peek((uint16_t)(0x525E + T.r[0])));
	fl::mov(peek(T.r[6]));
	tick(0x529A, 39);
	push(T.r[2]);
	fl::mov(T.r[2]);
	tick(0x529C, 24);
	jsr(0x52A0, 0x90A6);                               // divide
	fl::cmp(T.r[0], 0x0009);
	tick(0x52A4, 18);                                  // CMP R0,#9
	if ((int16_t)T.r[0] > 9)
	{
		tick(0x52A6, 12);                              // BLE
		T.r[0] = 9;
		fl::mov(T.r[0]);
		tick(0x52AA, 18);
		tick(0x52B6, 12);                              // BR
	}
	else
	{
		tick(0x52AC, 12);
		fl::cmp(T.r[0], 0x0005);
		tick(0x52B0, 18);                              // CMP R0,#5
		if ((int16_t)T.r[0] < 5)
		{
			tick(0x52B2, 12);                          // BGE
			T.r[0] = 5;
			fl::mov(T.r[0]);
			tick(0x52B6, 18);
		}
		else
			tick(0x52B6, 12);
	}
	pokeb(0x0792, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x52BA, 27);
	pokeb(0x0791, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x52BE, 27);
	T.r[0] = fl::logic((uint16_t)(T.r[0] | 0x0080));
	tick(0x52C2, 18);
	pokeb(0x0772, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x52C6, 27);
	pokeb(0x0771, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x52CA, 27);
	pokeb(0x07B1, 0);
	fl::clr();
	tick(0x52CE, 27);
	fl::cmp(T.r[1], 0x0002);
	tick(0x52D2, 18);                                  // CMP R1,#2
	if (T.r[1] == 2)
	{
		tick(0x52D4, 12);
		pokeb(0x07F0, 0);
		fl::clr();
		tick(0x52D8, 27);
		pokeb(0x07F4, 0);
		fl::clr();
		tick(0x52DC, 27);
		poke(0x0570, asr(peek(0x0570), 1));
		tick(0x52E0, 27);
		T.r[0] = peek(0x0570);
		fl::mov(T.r[0]);
		tick(0x52E4, 24);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x52E6, 12);
		T.r[0] = (uint16_t)-T.r[0];
		fl::neg((uint16_t)(-T.r[0]));
		tick(0x52E8, 12);
		poke(0x04F0, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x52EC, 27);
		jsr(0x52F0, 0x5166);                           // copy_object0_velocity
	}
	else
		tick(0x52F0, 12);                              // BNE
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0x52F2, 12);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x52F4, 12);
	T.r[0] = fl::add(T.r[0], T.r[1]);
	tick(0x52F6, 12);
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0x52F8, 12);
	T.r[0] = sext8(peekb(0x031D));
	fl::movb((uint8_t)T.r[0]);
	tick(0x52FC, 24);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0007));
	tick(0x5300, 18);
	T.r[2] = sext8(peekb((uint16_t)(0x7D12 + T.r[0])));
	fl::movb((uint8_t)T.r[2]);
	tick(0x5304, 27);
	T.r[1] = fl::add(T.r[1], T.r[2]);
	tick(0x5306, 12);
	const bool flag = (peekb(0x0324) & 0x80) != 0;
	fl::tstb(peekb(0x0324));
	tick(0x530A, 24);                                  // TSTB @#0x0324
	if (flag)
	{
		tick(0x530C, 12);                              // BPL
		const bool low = (int16_t)peek(0x05B0) < 0x01A0;
		fl::cmp(peek(0x05B0), 0x01A0);
		tick(0x5312, 30);                              // CMP @#0x05B0,#0x01A0
		if (low)
		{
			tick(0x5314, 12);                          // BGE
			T.r[1] = fl::add(T.r[1], 9);
			tick(0x5318, 18);
		}
		else
			tick(0x5318, 12);
	}
	else
		tick(0x5318, 12);
	pokeb(0x01EB, peekb((uint16_t)(0x7D1A + T.r[1])));
	fl::movb(peekb(0x01EB));
	tick(0x531E, 42);
	T.r[0] = sext8(peekb((uint16_t)(0x7D1A + T.r[1])));
	fl::movb((uint8_t)T.r[0]);
	tick(0x5322, 27);
	T.r[0] = fl::inc(T.r[0]);
	tick(0x5324, 12);
	pokeb(0x01EF, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x5328, 27);
	pokeb(0x01F0, peekb((uint16_t)(0x7D2C + T.r[1])));
	fl::movb(peekb(0x01F0));
	tick(0x532E, 42);
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0x5330, 12);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x5332, 12);
	poke(0x01E0, peek((uint16_t)(0x7D3E + T.r[0])));
	fl::mov(peek(0x01E0));
	tick(0x5338, 42);
	const bool slope = (peekb(0x0327) & 0x38) != 0;
	fl::logicb((uint8_t)(0x38 & peekb(0x0327)));
	tick(0x533E, 30);                                  // BITB #0x38,@#0x0327
	if (slope)
	{
		tick(0x5340, 12);
		poke(0x0300, 0x1800);
		fl::mov(0x1800);
		tick(0x5346, 33);
		T.r[1] = fl::add(T.r[1], 0x0012);
		tick(0x534A, 18);
		tick(0x5352, 12);                              // BR
	}
	else
	{
		tick(0x534C, 12);                              // BEQ
		poke(0x0300, 0x1900);
		fl::mov(0x1900);
		tick(0x5352, 33);
	}
	pokeb(0x01F1, (uint8_t)T.r[1]);
	fl::movb((uint8_t)T.r[1]);
	tick(0x5356, 27);
	T.r[1] = fl::asl(T.r[1]);
	tick(0x5358, 12);
	poke(0x01E2, peek((uint16_t)(0x7D62 + T.r[1])));
	fl::mov(peek(0x01E2));
	tick(0x535E, 42);
	poke(0x01E4, peek((uint16_t)(0x7DAA + T.r[1])));
	fl::mov(peek(0x01E4));
	tick(0x5364, 42);
	pokeb(0x01EE, 0);
	fl::clr();
	tick(0x5368, 27);
	const bool move45 = peekb(0x01EB) == 0x45;
	fl::cmpb(peekb(0x01EB), 0x45);
	tick(0x536E, 30);                                  // CMPB @#0x01EB,#0x45
	if (move45)
	{
		tick(0x5370, 12);
		pokeb(0x0324, fl::logicb((uint8_t)(peekb(0x0324) | 0x40)));
		tick(0x5376, 33);
	}
	else
		tick(0x5376, 12);                              // BNE
	pokeb(0x0330, 0x40);
	fl::movb(0x40);
	tick(0x537C, 33);
	poke(T.r[6], 0x004C);
	fl::mov(0x004C);
	tick(0x5380, 27);
	jsr(0x5384, 0x8E78);                               // queue_sound
	T.r[6] = (uint16_t)(T.r[6] + 4);
	fl::cmp(peek((uint16_t)(T.r[6] - 4)), peek((uint16_t)(T.r[6] - 2)));
	tick(0x5386, 24);                                  // CMP (SP)+,(SP)+
	c_restore(1);
	return T.r[0];
}

// A5:54CE  trick_moves(): the trick state machine: check the landing of the current frame
// (A5:53A8), then the move 0x01EB: 0x40-0x45 (jump table 0x5814: rising, top, falling,
// landing frames, spin, ramp lip) and 0x46-0x57 (jump table 0x5820: transitions between
// them); finally the landing's sprites (F:E5D2 / F:E648 by 0x01F0, +0x10 in the lower half
// 0x0ED4 < 0x5C00).  Bank B 0x2C00.
uint16_t trick_moves(const Args &)
{
	c_save(1, 0x54D2);
	T.r[0] = 0x2C00;
	fl::mov(T.r[0]);
	tick(0x54D6, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x54DA, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x54DE, 27);
	T.r[1] = peek(0x01E0);
	fl::mov(T.r[1]);
	tick(0x54E2, 24);
	if (T.r[1])
	{
		tick(0x54E4, 12);                              // BEQ
		T.r[0] = 0;
		fl::clr();
		tick(0x54E6, 12);
		T.r[0] = peekb(0x07B2);
		fl::logicb((uint8_t)T.r[0]);
		tick(0x54EA, 24);
		T.r[0] = fl::add(T.r[0], T.r[1]);
		tick(0x54EC, 12);
		T.r[1] = sext8(peekb(T.r[0]));
		fl::movb((uint8_t)T.r[1]);
		tick(0x54EE, 18);
		const bool shown = peekb(0x07B1) != 0;
		fl::tstb(peekb(0x07B1));
		tick(0x54F2, 24);                              // TSTB @#0x07B1
		if (shown)
		{
			tick(0x54F4, 12);
			pokeb(0x07B1, 0);
			fl::clr();
			tick(0x54F8, 27);
			fl::tst(T.r[1]);
			tick(0x54FA, 12);                          // TST R1
			if (T.r[1] & 0x8000)
			{
				tick(0x54FC, 12);                      // BGE
				T.r[0] = 0;
				fl::clr();
				tick(0x54FE, 12);
				T.r[0] = peekb(0x0772);
				fl::logicb((uint8_t)T.r[0]);
				tick(0x5502, 24);
				T.r[0] = fl::logic((uint16_t)(T.r[0] | 0x0080));
				tick(0x5506, 18);
				pokeb(0x0772, (uint8_t)T.r[0]);
				fl::movb((uint8_t)T.r[0]);
				tick(0x550A, 27);
				pokeb(0x0771, (uint8_t)T.r[0]);
				fl::movb((uint8_t)T.r[0]);
				tick(0x550E, 27);
			}
			else
				tick(0x550E, 12);
		}
		else
			tick(0x550E, 12);                          // BEQ
		push(T.r[1]);
		fl::mov(T.r[1]);
		tick(0x5510, 24);
		jsr(0x5514, 0x53A8);                           // trick_landing
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(peek((uint16_t)(T.r[6] - 2)));
		tick(0x5516, 18);                              // TST (SP)+
	}
	else
		tick(0x5516, 12);                              // BEQ
	T.r[0] = sext8(peekb(0x01EB));
	fl::movb((uint8_t)T.r[0]);
	tick(0x551A, 24);
	fl::cmp(T.r[0], 0x0040);
	tick(0x551E, 18);                                  // CMP R0,#0x40
	bool in_range = false;
	if ((int16_t)T.r[0] >= 0x40)
	{
		tick(0x5520, 12);                              // BLT
		fl::cmp(T.r[0], 0x0045);
		tick(0x5524, 18);                              // CMP R0,#0x45
		in_range = (int16_t)T.r[0] <= 0x45;
		tick(in_range ? 0x5526 : 0x561A, 12);          // BGT
	}
	else
		tick(0x561A, 12);
	if (!in_range)
		tick(0x5644, 12);                              // 561A: BR
	else
	{
		T.r[0] = fl::sub(T.r[0], 0x40);
		tick(0x552A, 18);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x552C, 12);
		static const uint16_t cases[6] = {0x5530, 0x559A, 0x55B0, 0x55BC, 0x55FE, 0x561C};
		const int k = T.r[0] >> 1;
		tick(cases[k], 27);                            // ADD 0x5814(R0),PC
		switch (k)
		{
		case 0:                                        // rising
		{
			const bool below = (int16_t)peek(0x06B2) < (int16_t)peek(0x0300);
			tick(0x5536, 36);                          // CMP @#0x06B2,@#0x0300
			if (below)
			{
				tick(0x5632, 12);                      // BLT to the BLT at 0x5632
				tick(0x5644, 12);
				break;
			}
			tick(0x5538, 12);
			jsr(0x553C, 0x5462);                       // trick_step
			const bool landing = peekb(0x01F0) != 0;
			fl::tstb(peekb(0x01F0));
			tick(0x5540, 24);                          // TSTB @#0x01F0
			if (landing)
			{
				tick(0x5542, 12);
				pokeb(0x01EB, 0x45);
				fl::movb(0x45);
				tick(0x5548, 33);
				tick(0x5644, 12);                      // BR
				break;
			}
			tick(0x554A, 12);                          // BEQ
			const bool bounce = (peekb(0x0324) & 0x01) != 0;
			fl::logicb((uint8_t)(0x01 & peekb(0x0324)));
			tick(0x5550, 30);                          // BITB #1,@#0x0324
			if (!bounce)
			{
				tick(0x5644, 12);                      // BEQ
				break;
			}
			tick(0x5552, 12);
			poke(0x01DE, peek(0x05B2));
			fl::mov(peek(0x01DE));
			tick(0x5558, 39);
			poke(0x05B2, 0);
			fl::clr();
			tick(0x555C, 27);
			T.r[2] = peek(0x06F2);
			fl::mov(T.r[2]);
			tick(0x5560, 24);
			T.r[2] = fl::asr(T.r[2]);
			tick(0x5562, 12);
			T.r[2] = fl::add(T.r[2], peek(0x0732));
			tick(0x5566, 24);
			const bool full = peekb(0x0327) == 7;
			fl::cmpb(peekb(0x0327), 0x07);
			tick(0x556C, 30);                          // CMPB @#0x0327,#7
			if (full)
			{
				tick(0x556E, 12);
				T.r[0] = 0x2600;
				fl::mov(T.r[0]);
				tick(0x5572, 18);
				tick(0x5578, 12);                      // BR
			}
			else
			{
				tick(0x5574, 12);                      // BNE
				T.r[0] = 0xE000;
				fl::mov(T.r[0]);
				tick(0x5578, 18);
			}
			T.r[0] = fl::sub(T.r[0], T.r[2]);
			tick(0x557A, 12);
			T.r[0] = fl::add(T.r[0], peek(0x0300));
			tick(0x557E, 24);
			poke(0x06B2, T.r[0]);
			fl::mov(T.r[0]);
			tick(0x5582, 27);
			poke(0x06B2, fl::sub(peek(0x06B2), 0x0100));
			tick(0x5588, 33);
			poke(0x06B4, peek(0x06B2));
			fl::mov(peek(0x06B4));
			tick(0x558E, 39);
			poke(0x06B6, peek(0x06B2));
			fl::mov(peek(0x06B6));
			tick(0x5594, 39);
			poke(0x02EE, 0);
			fl::clr();
			tick(0x5598, 27);
			tick(0x5644, 12);                          // BR
			break;
		}
		case 1:                                        // top
		{
			pokeb(0x0324, (uint8_t)(peekb(0x0324) | 0x40));
			tick(0x55A0, 33);
			const bool above = (int16_t)peek(0x06B2) > (int16_t)peek(0x0300);
			fl::cmp(peek(0x06B2), peek(0x0300));
			tick(0x55A6, 36);                          // CMP @#0x06B2,@#0x0300
			if (above)
			{
				tick(0x5644, 12);                      // BGT
				break;
			}
			tick(0x55A8, 12);
			pokeb(0x01EB, 0x45);
			fl::movb(0x45);
			tick(0x55AE, 33);
			tick(0x55B6, 12);                          // BR
			jsr(0x55BA, 0x5462);                       // trick_step
			tick(0x5644, 12);                          // BR
			break;
		}
		case 2:                                        // falling
		{
			const bool rising = (int16_t)peek(0x05B2) > 0;
			fl::tst(peek(0x05B2));
			tick(0x55B4, 24);                          // TST @#0x05B2
			if (rising)
			{
				tick(0x5644, 12);                      // BGT
				break;
			}
			tick(0x55B6, 12);
			jsr(0x55BA, 0x5462);                       // trick_step
			tick(0x5644, 12);                          // BR
			break;
		}
		case 3:                                        // landing frames
		{
			T.r[2] = 0;
			fl::clr();
			tick(0x55BE, 12);
			T.r[2] = peekb(0x01EF);
			fl::logicb((uint8_t)T.r[2]);
			tick(0x55C2, 24);
			T.r[0] = 0;
			fl::clr();
			tick(0x55C4, 12);
			T.r[0] = peekb(0x07B2);
			fl::logicb((uint8_t)T.r[0]);
			tick(0x55C8, 24);
			fl::cmp(T.r[0], T.r[2]);
			tick(0x55CA, 12);                          // CMP R0,R2
			if (T.r[0] != T.r[2])
			{
				tick(0x5644, 12);                      // BNE
				break;
			}
			tick(0x55CC, 12);
			fl::cmp(T.r[2], 0x0004);
			tick(0x55D0, 18);                          // CMP R2,#4
			if (T.r[2] == 4)
				tick(0x55E0, 12);                      // BEQ
			else
			{
				tick(0x55D2, 12);
				T.r[0] = peek(0x01DE);
				fl::mov(T.r[0]);
				tick(0x55D6, 24);
				T.r[0] = (uint16_t)-T.r[0];
				fl::neg((uint16_t)(-T.r[0]));
				tick(0x55D8, 12);
				poke(0x05B4, T.r[0]);
				fl::mov(T.r[0]);
				tick(0x55DC, 27);
				poke(0x05B2, T.r[0]);
				fl::mov(T.r[0]);
				tick(0x55E0, 27);
			}
			poke(0x02EE, 0x000C);
			fl::mov(0x000C);
			tick(0x55E6, 33);
			pokeb(0x01EB, 0x45);
			fl::movb(0x45);
			tick(0x55EC, 33);
			fl::cmp(T.r[2], 0x0005);
			tick(0x55F0, 18);                          // CMP R2,#5
			if (T.r[2] != 5)
			{
				tick(0x5644, 12);                      // BNE
				break;
			}
			tick(0x55F2, 12);
			push(0x0059);
			fl::mov(0x0059);
			tick(0x55F6, 30);
			jsr(0x55FA, 0x8E78);                       // queue_sound
			T.r[6] = (uint16_t)(T.r[6] + 2);
			fl::tst(peek((uint16_t)(T.r[6] - 2)));
			tick(0x55FC, 18);                          // TST (SP)+
			tick(0x5644, 12);                          // BR
			break;
		}
		case 4:                                        // spin
		{
			poke(0x0432, (uint16_t)(peek(0x0432) - 1));
			const bool done = !peek(0x0432);
			fl::dec((uint16_t)(peek(0x0432) + 1));
			tick(0x5602, 27);                          // DEC @#0x0432
			if (done)
				tick(0x560A, 12);                      // BEQ
			else
			{
				tick(0x5604, 12);
				const bool landing = peekb(0x01F0) != 0;
				fl::tstb(peekb(0x01F0));
				tick(0x5608, 24);                      // TSTB @#0x01F0
				if (!landing)
				{
					tick(0x5644, 12);                  // BEQ
					break;
				}
				tick(0x560A, 12);
			}
			jsr(0x560E, 0x5462);                       // trick_step
			pokeb(0x01EF, 4);
			fl::movb(0x04);
			tick(0x5614, 33);
			pokeb(0x01EB, 0x43);
			fl::movb(0x43);
			tick(0x561A, 33);
			tick(0x5644, 12);                          // BR
			break;
		}
		case 5:                                        // ramp lip
		{
			const bool lip = (peekb(0x0324) & 0x08) != 0;
			fl::logicb((uint8_t)(0x08 & peekb(0x0324)));
			tick(0x5622, 30);                          // BITB #8,@#0x0324
			if (!lip)
			{
				tick(0x5644, 12);                      // BEQ
				break;
			}
			tick(0x5624, 12);
			const bool slow = (int16_t)peek(0x05B2) < 0x0140;
			fl::cmp(peek(0x05B2), 0x0140);
			tick(0x562A, 30);                          // CMP @#0x05B2,#0x0140
			if (slow)
			{
				tick(0x5644, 12);                      // BLT
				break;
			}
			tick(0x562C, 12);
			const bool low = (int16_t)peek(0x06B2) < 0x2400;
			fl::cmp(peek(0x06B2), 0x2400);
			tick(0x5632, 30);                          // CMP @#0x06B2,#0x2400
			if (low)
			{
				tick(0x5644, 12);                      // BLT
				break;
			}
			tick(0x5634, 12);
			pokeb(0x0324, fl::logicb((uint8_t)(peekb(0x0324) & ~0x08)));
			tick(0x563A, 33);
			jsr(0x563E, 0x5462);                       // trick_step
			pokeb(0x01F1, 0x12);
			fl::movb(0x12);
			tick(0x5644, 33);
			break;
		}
		}
	}
	// the shared ends of the transitions
	auto to_end = [](uint16_t) { tick(0x57E4, 12); };  // BR 0x57E4
	auto set_40 = [&]() { pokeb(0x01EB, 0x40); tick(0x578A, 33); to_end(0x578A); };
	auto set_43 = [&]() { pokeb(0x01EB, 0x43); tick(0x571E, 33); to_end(0x571E); };
	auto set_41 = [&]() { pokeb(0x01EB, 0x41); tick(0x57BA, 33); to_end(0x57BA); };
	auto set_45 = [&]() { pokeb(0x01EB, 0x45); tick(0x57E4, 33); };
	T.r[0] = sext8(peekb(0x01EB));
	fl::movb((uint8_t)T.r[0]);
	tick(0x5648, 24);
	fl::cmp(T.r[0], 0x0046);
	tick(0x564C, 18);                                  // CMP R0,#0x46
	bool second = false;
	if ((int16_t)T.r[0] >= 0x46)
	{
		tick(0x564E, 12);                              // BLT
		fl::cmp(T.r[0], 0x0057);
		tick(0x5652, 18);                              // CMP R0,#0x57
		second = (int16_t)T.r[0] <= 0x57;
		tick(second ? 0x5654 : 0x571E, 12);            // BGT
	}
	else
		tick(0x571E, 12);
	if (!second)
		to_end(0x571E);
	else
	{
		T.r[2] = T.r[0];
		fl::mov(T.r[2]);
		tick(0x5656, 12);
		T.r[2] = fl::sub(T.r[2], 0x0046);
		tick(0x565A, 18);
		T.r[2] = fl::asl(T.r[2]);
		tick(0x565C, 12);
		static const uint16_t cases[18] = {
			0x5660, 0x5668, 0x5694, 0x56BE, 0x56C6, 0x56F8, 0x57B4, 0x5700, 0x570E,
			0x5720, 0x572E, 0x5784, 0x578C, 0x57DE, 0x57A0, 0x57AE, 0x57BC, 0x57C4,
		};
		const int k = T.r[2] >> 1;
		tick(cases[k], 27);                            // ADD 0x5820(R2),PC
		switch (k)
		{
		case 0:
			pokeb(0x0324, (uint8_t)(peekb(0x0324) | 0x05));
			tick(0x5666, 33);
			tick(0x572C, 12);                          // BR
			tick(0x5784, 12);                          // BR
			set_40();
			break;
		case 1:
		{
			const bool five = peekb(0x07B2) == 5;
			tick(0x566E, 30);                          // CMPB @#0x07B2,#5
			if (!five)
			{
				tick(0x571E, 12);                      // BNE
				to_end(0x571E);
				break;
			}
			tick(0x5670, 12);
			T.r[1] = 0;
			fl::clr();
			tick(0x5672, 12);
			T.r[1] = peekb(0x0771);
			fl::logicb((uint8_t)T.r[1]);
			tick(0x5676, 24);
			T.r[1] = fl::asl(T.r[1]);
			tick(0x5678, 12);
			T.r[1] = fl::asl(T.r[1]);
			tick(0x567A, 12);
			pokeb(0x0791, (uint8_t)T.r[1]);
			fl::movb((uint8_t)T.r[1]);
			tick(0x567E, 27);
			pokeb(0x0771, (uint8_t)T.r[1]);
			fl::movb((uint8_t)T.r[1]);
			tick(0x5682, 27);
			pokeb(0x0792, (uint8_t)T.r[1]);
			fl::movb((uint8_t)T.r[1]);
			tick(0x5686, 27);
			pokeb(0x0772, (uint8_t)T.r[1]);
			fl::movb((uint8_t)T.r[1]);
			tick(0x568A, 27);
			T.r[2] = T.r[0];
			fl::mov(T.r[2]);
			tick(0x568C, 12);
			T.r[2] = fl::inc(T.r[2]);
			tick(0x568E, 12);
			pokeb(0x01EB, (uint8_t)T.r[2]);
			fl::movb((uint8_t)T.r[2]);
			tick(0x5692, 27);
			tick(0x578A, 12);                          // BR
			to_end(0x578A);
			break;
		}
		case 2:
		{
			const bool five = peekb(0x07B2) == 5;
			tick(0x569A, 30);                          // CMPB @#0x07B2,#5
			if (five)
			{
				tick(0x578A, 12);                      // BEQ
				to_end(0x578A);
				break;
			}
			tick(0x569C, 12);
			T.r[1] = 0;
			fl::clr();
			tick(0x569E, 12);
			T.r[1] = peekb(0x0771);
			fl::logicb((uint8_t)T.r[1]);
			tick(0x56A2, 24);
			T.r[1] = fl::asr(T.r[1]);
			tick(0x56A4, 12);
			T.r[1] = fl::asr(T.r[1]);
			tick(0x56A6, 12);
			pokeb(0x0791, (uint8_t)T.r[1]);
			fl::movb((uint8_t)T.r[1]);
			tick(0x56AA, 27);
			pokeb(0x0771, (uint8_t)T.r[1]);
			fl::movb((uint8_t)T.r[1]);
			tick(0x56AE, 27);
			pokeb(0x0792, (uint8_t)T.r[1]);
			fl::movb((uint8_t)T.r[1]);
			tick(0x56B2, 27);
			pokeb(0x0772, (uint8_t)T.r[1]);
			fl::movb((uint8_t)T.r[1]);
			tick(0x56B6, 27);
			pokeb(0x01EF, 9);
			fl::movb(0x09);
			tick(0x56BC, 33);
			tick(0x5718, 12);                          // BR
			set_43();
			break;
		}
		case 3:
			pokeb(0x0324, (uint8_t)(peekb(0x0324) | 0x01));
			tick(0x56C4, 33);
			tick(0x5784, 12);                          // BR
			set_40();
			break;
		case 4:
		{
			const bool frame = peekb(0x07B2) != 0;
			tick(0x56CA, 24);                          // TSTB @#0x07B2
			if (!frame)
			{
				tick(0x57BA, 12);                      // BEQ
				to_end(0x57BA);
				break;
			}
			tick(0x56CC, 12);
			T.r[0] = sext8(peekb(0x025F));
			fl::movb((uint8_t)T.r[0]);
			tick(0x56D0, 24);
			pokeb(0x05B2, peekb((uint16_t)(0x7C74 + T.r[0])));
			fl::movb(peekb(0x05B2));
			tick(0x56D6, 42);
			pokeb(0x05B3, 0);
			fl::clr();
			tick(0x56DA, 27);
			poke(0x05B2, fl::add(peek(0x05B2), peek(0x01DE)));
			tick(0x56E0, 39);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x56E2, 12);
			const bool over = (int16_t)peek(0x05B2) > (int16_t)peek((uint16_t)(0x7C78 + T.r[0]));
			fl::cmp(peek(0x05B2), peek((uint16_t)(T.r[0] + 0x7C78)));
			tick(0x56E8, 39);                          // CMP @#0x05B2,0x7C78(R0)
			if (over)
			{
				tick(0x56EA, 12);                      // BLE
				poke(0x05B2, peek((uint16_t)(0x7C78 + T.r[0])));
				fl::mov(peek(0x05B2));
				tick(0x56F0, 42);
			}
			else
				tick(0x56F0, 12);
			poke(0x02EE, 0x000C);
			fl::mov(0x000C);
			tick(0x56F6, 33);
			tick(0x57B4, 12);                          // BR
			set_41();
			break;
		}
		case 5:
			pokeb(0x01EB, 0x42);
			tick(0x56FE, 33);
			to_end(0x56FE);
			break;
		case 6:
			set_41();
			break;
		case 7:
			pokeb(0x0324, (uint8_t)(peekb(0x0324) | 0x01));
			tick(0x5706, 33);
			poke(0x0300, fl::add(peek(0x0300), 0x0100));
			tick(0x570C, 33);
			tick(0x5784, 12);                          // BR
			set_40();
			break;
		case 8:
			poke(0x01DE, 0);
			fl::clr();
			tick(0x5712, 27);
			pokeb(0x01EF, 5);
			fl::movb(0x05);
			tick(0x5718, 33);
			set_43();
			break;
		case 9:
			pokeb(0x0324, (uint8_t)(peekb(0x0324) | 0x05));
			tick(0x5726, 33);
			poke(0x0300, fl::add(peek(0x0300), 0x0200));
			tick(0x572C, 33);
			tick(0x5784, 12);                          // BR
			set_40();
			break;
		case 10:
			pokeb(0x0324, fl::logicb((uint8_t)(peekb(0x0324) & ~0x04)));
			tick(0x5734, 33);
			jsr(0x5738, 0x5388);                       // trick_facing
			T.r[1] = T.r[0];
			fl::mov(T.r[1]);
			tick(0x573A, 12);
			if (T.r[1])
			{
				tick(0x573C, 12);                      // BEQ
				poke(0x03B2, fl::add(peek(0x03B2), 0x000A));
				tick(0x5742, 33);
				poke(0x03B4, fl::add(peek(0x03B4), 0x000A));
				tick(0x5748, 33);
				poke(0x01E2, fl::add(peek(0x01E2), 0x000A));
				tick(0x574E, 33);
				poke(0x01E4, fl::add(peek(0x01E4), 0x000A));
				tick(0x5754, 33);
				poke(0x01E0, fl::add(peek(0x01E0), 0x0005));
				tick(0x575A, 33);
			}
			else
				tick(0x575A, 12);
			poke(0x0432, 0x0028);
			fl::mov(0x0028);
			tick(0x5760, 33);
			pokeb(0x01EB, 0x44);
			fl::movb(0x44);
			tick(0x5766, 33);
			T.r[0] = peek(0x04F0);
			fl::mov(T.r[0]);
			tick(0x576A, 24);
			T.r[0] = fl::asr(T.r[0]);
			tick(0x576C, 12);
			T.r[0] = fl::add(T.r[0], peek(0x04F0));
			tick(0x5770, 24);
			poke(0x04F0, T.r[0]);
			fl::mov(T.r[0]);
			tick(0x5774, 27);
			T.r[0] = peek(0x0570);
			fl::mov(T.r[0]);
			tick(0x5778, 24);
			T.r[0] = fl::asr(T.r[0]);
			tick(0x577A, 12);
			T.r[0] = fl::add(T.r[0], peek(0x0570));
			tick(0x577E, 24);
			poke(0x0570, T.r[0]);
			fl::mov(T.r[0]);
			tick(0x5782, 27);
			to_end(0x5782);
			break;
		case 11:
			set_40();
			break;
		case 12:
			pokeb(0x01EB, 0x41);
			tick(0x5792, 33);
			pokeb(0x01F0, 4);
			fl::movb(0x04);
			tick(0x5798, 33);
			pokeb(0x01EF, 0x53);
			fl::movb(0x53);
			tick(0x579E, 33);
			to_end(0x579E);
			break;
		case 13:
			set_45();
			break;
		case 14:
			pokeb(0x01EB, 0x42);
			tick(0x57A6, 33);
			poke(0x02EE, 0x0009);
			fl::mov(0x0009);
			tick(0x57AC, 33);
			to_end(0x57AC);
			break;
		case 15:
			pokeb(0x01EF, 0x56);
			tick(0x57B4, 33);
			set_41();
			break;
		case 16:
			poke(0x05B2, (uint16_t)(peek(0x05B2) + 0x0060));
			tick(0x57C2, 33);
			tick(0x57DE, 12);                          // BR
			set_45();
			break;
		case 17:
		{
			const bool full = peekb(0x0327) == 7;
			tick(0x57CA, 30);                          // CMPB @#0x0327,#7
			if (full)
			{
				tick(0x57CC, 12);
				const bool facing = peekb(0x01F8) == 2;
				fl::cmpb(peekb(0x01F8), 0x02);
				tick(0x57D2, 30);                      // CMPB @#0x01F8,#2
				if (facing)
				{
					tick(0x57D4, 12);
					pokeb(0x0324, fl::logicb((uint8_t)(peekb(0x0324) | 0x0C)));
					tick(0x57DA, 33);
				}
				else
					tick(0x57DA, 12);                  // BNE
			}
			else
				tick(0x57DA, 12);                      // BNE
			pokeb(0x01EF, 0);
			fl::clr();
			tick(0x57DE, 27);
			set_45();
			break;
		}
		}
	}
	T.r[0] = 0;
	fl::clr();
	tick(0x57E6, 12);
	T.r[0] = peekb(0x01F0);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x57EA, 24);
	fl::tst(T.r[0]);
	tick(0x57EC, 12);                                  // TST R0
	if ((int16_t)T.r[0] > 0)
	{
		tick(0x57EE, 12);                              // BLE
		T.r[1] = T.r[0];
		fl::mov(T.r[1]);
		tick(0x57F0, 12);
		const bool low = peek(0x0ED4) < 0x5C00;
		fl::cmp(peek(0x0ED4), 0x5C00);
		tick(0x57F6, 30);                              // CMP @#0x0ED4,#0x5C00
		if (low)
		{
			tick(0x57F8, 12);                          // BCC
			T.r[1] = fl::logic((uint16_t)(T.r[1] | 0x0010));
			tick(0x57FC, 18);
		}
		else
			tick(0x57FC, 12);
		push(T.r[1]);
		fl::mov(T.r[1]);
		tick(0x57FE, 24);
		jsr(0x5802, 0xE5D2);                           // skater_anim_a
		poke(0x03B2, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x5806, 27);
		poke(T.r[6], T.r[1]);
		fl::mov(T.r[1]);
		tick(0x5808, 21);
		jsr(0x580C, 0xE648);                           // skater_anim_b
		poke(0x03B4, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x5810, 27);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(peek((uint16_t)(T.r[6] - 2)));
		tick(0x5812, 18);                              // TST (SP)+
	}
	else
		tick(0x5812, 12);                              // BLE
	c_restore(1);
	return T.r[0];
}

// A7:4A66  velocity_direction(): the skater's direction 0x031D (0-15) from the velocity
// (0x04F0, 0x0570): F:90A6 divides 16 vy by vx and the ratio picks the sector
uint16_t velocity_direction(const Args &)
{
	c_save(3, 0x4A6A);
	T.r[4] = peek(0x0570);
	fl::mov(T.r[4]);
	tick(0x4A6E, 24);
	T.r[3] = peek(0x04F0);
	fl::mov(T.r[3]);
	tick(0x4A72, 24);
	if (!T.r[3])
	{
		tick(0x4A74, 12);                              // BNE
		fl::tst(T.r[4]);
		tick(0x4A76, 12);                              // TST R4
		if ((int16_t)T.r[4] > 0)
		{
			tick(0x4A78, 12);
			T.r[1] = 0x0004;
			fl::mov(T.r[1]);
			tick(0x4A7C, 18);
		}
		else
		{
			tick(0x4A7E, 12);                          // BLE
			T.r[1] = 0x000C;
			fl::mov(T.r[1]);
			tick(0x4A82, 18);
		}
		tick(0x4AFA, 12);                              // BR
	}
	else
	{
		tick(0x4A84, 12);
		push(T.r[4]);
		fl::mov(T.r[4]);
		tick(0x4A86, 24);
		for (uint16_t pc = 0x4A88; pc <= 0x4A8E; pc = (uint16_t)(pc + 2))
		{
			poke(T.r[6], (uint16_t)(peek(T.r[6]) << 1));
			tick(pc, 21);                              // ASL (SP)
		}
		push(T.r[3]);
		fl::mov(T.r[3]);
		tick(0x4A90, 24);
		jsr(0x4A94, 0x90A6);                           // divide
		T.r[2] = T.r[0];
		fl::mov(T.r[2]);
		tick(0x4A96, 12);
		if (T.r[0] & 0x8000)
		{
			tick(0x4A98, 12);                          // BGE
			T.r[0] = (uint16_t)-T.r[0];
			fl::neg((uint16_t)(-T.r[0]));
			tick(0x4A9A, 12);
		}
		else
			tick(0x4A9A, 12);
		if (!T.r[0])
		{
			tick(0x4A9C, 12);                          // BNE
			fl::tst(T.r[3]);
			tick(0x4A9E, 12);                          // TST R3
			if ((int16_t)T.r[3] > 0)
			{
				tick(0x4AAC, 12);                      // BGT
				T.r[1] = 0;
				tick(0x4AAE, 12);
			}
			else
			{
				tick(0x4AA0, 12);
				T.r[1] = 0x0008;
				fl::mov(T.r[1]);
				tick(0x4AA4, 18);
			}
			tick(0x4AD8, 12);                          // BR
		}
		else
		{
			tick(0x4AA6, 12);
			// the sector by the ratio: up to 2, 6, 13, 50, more
			static const uint16_t limits[4] = {2, 6, 13, 50};
			uint16_t pc = 0x4AA6;
			int k = 0;
			for (; k < 4; k++)
			{
				tick((uint16_t)(pc + 4), 18);          // CMP R0,#limit
				if ((int16_t)T.r[0] <= (int16_t)limits[k]) break;
				pc = (uint16_t)(pc + 10 + (k ? 2 : 0)); // BGT to the next CMP
				tick(pc, 12);
			}
			if (k == 0)
			{
				tick(0x4AAC, 12);                      // BGT
				T.r[1] = 0;
				tick(0x4AAE, 12);
			}
			else if (k < 4)
			{
				tick((uint16_t)(pc + 6), 12);          // BGT
				T.r[1] = (uint16_t)k;
				tick((uint16_t)(pc + 10), 18);
			}
			else
			{
				T.r[1] = 0x0004;
				tick(0x4AD8, 18);
			}
			if (k < 4) tick(0x4AD8, 12);               // BR
		}
		fl::tst(T.r[2]);
		tick(0x4ADA, 12);                              // TST R2
		if (T.r[2] & 0x8000)
		{
			tick(0x4ADC, 12);                          // BGE
			T.r[1] = (uint16_t)-T.r[1];
			fl::neg((uint16_t)(-T.r[1]));
			tick(0x4ADE, 12);
			T.r[1] = fl::add(T.r[1], 8);
			tick(0x4AE2, 18);
		}
		else
			tick(0x4AE2, 12);
		fl::tst(T.r[4]);
		tick(0x4AE4, 12);                              // TST R4
		if (T.r[4] & 0x8000)
		{
			tick(0x4AE6, 12);                          // BGE
			fl::tst(T.r[2]);
			tick(0x4AE8, 12);                          // TST R2
			if (T.r[2])
			{
				tick(0x4AEA, 12);                      // BEQ
				fl::cmp(T.r[1], 0x0008);
				tick(0x4AEE, 18);                      // CMP R1,#8
				if (T.r[1] == 8)
				{
					tick(0x4AF0, 12);                  // BNE
					T.r[1] = 0;
					fl::clr();
					tick(0x4AF2, 12);
					tick(0x4AF8, 12);                  // BR
				}
				else
				{
					tick(0x4AF4, 12);
					T.r[1] = fl::add(T.r[1], 8);
					tick(0x4AF8, 18);
				}
			}
			else
				tick(0x4AF8, 12);
		}
		else
			tick(0x4AF8, 12);
		T.r[6] = (uint16_t)(T.r[6] + 4);
		fl::cmp(peek((uint16_t)(T.r[6] - 4)), peek((uint16_t)(T.r[6] - 2)));
		tick(0x4AFA, 24);                              // CMP (SP)+,(SP)+
	}
	pokeb(0x031D, (uint8_t)T.r[1]);
	fl::movb((uint8_t)T.r[1]);
	tick(0x4AFE, 27);
	c_restore(3);
	return T.r[0];
}

// ---------------------------------------------------------------- the skater

// A5:4A2A (assembly)  skater_physics(): one frame of the skater's motion (bank B 0x2C00 for its
// tables).  The direction from the controls (0x031A from the table 0x6000 by 0x01C6) is
// approached through A5:4A0A unless held (0x031B); the slide state 0x031E (jump table 0x5160)
// keeps a minimum speed; on the ground the kick / carve state 0x0323 (0x02F2-0x02F6, the
// direction table 0x6000), in the air (0x01EA) the spin of the trick tables (0x0325/0x0326,
// 0x02F8, 0x7A08, F:90A6) or the landing (A5:4A0A, F:A2BE, F:A45A, F:E238, F:7E44); move 5
// sets the speed from 0x02E8 with the sounds; then the speed 0x01B4 by the slope (0x02EE),
// the friction tables 0x7BE8/0x7C28/0x7C38/0x7C64/0x7C68, the vertical speed (F:91A6, F:80FA,
// 0x05B0/0x05B2, 0xFCB0/0x7CD0) and the sprite priority bits of 0x0470-0x0473 (0x032F).
// Registers and flags end as the original leaves them.  Followed instruction by
// instruction; the labels are the original's addresses where paths join.
void skater_physics()
{
	bool b;                                            // a tested value (read before its tick)
	T.r[0] = 0x2C00;
	fl::mov(T.r[0]);
	tick(0x4A2E, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x4A32, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x4A36, 27);
	T.r[0] = peek(0x01C6);
	fl::mov(T.r[0]);
	tick(0x4A3A, 24);
	T.r[0] = fl::asr(T.r[0]);
	tick(0x4A3C, 12);
	pokeb(0x031A, peekb((uint16_t)(0x6000 + T.r[0])));
	fl::movb(peekb(0x031A));
	tick(0x4A42, 42);
	T.r[0] = sext8(peekb(0x031B));
	fl::movb((uint8_t)T.r[0]);
	tick(0x4A46, 24);
	if (T.r[0] & 0x8000)
		tick(0x4A92, 12);                              // BLT
	else
	{
		tick(0x4A48, 12);
		fl::cmp(T.r[0], 0x003F);
		tick(0x4A4C, 18);                              // CMP R0,#0x3F
		if ((int16_t)T.r[0] >= 0x003F)
		{
			tick(0x4A62, 12);                          // BGE
			T.r[2] = sext8(peekb(0x031A));
			fl::movb((uint8_t)T.r[2]);
			tick(0x4A66, 24);
			T.r[0] = sext8(peekb(0x01F8));
			fl::movb((uint8_t)T.r[0]);
			tick(0x4A6A, 24);
			fl::cmp(T.r[2], T.r[0]);
			tick(0x4A6C, 12);                          // CMP R2,R0
			if (T.r[2] == T.r[0])
			{
				tick(0x4A6E, 12);                      // BNE
				pokeb(0x031B, 0xFF);
				fl::movb(0xFF);
				tick(0x4A74, 33);
			}
			else
				tick(0x4A74, 12);
		}
		else
		{
			tick(0x4A4E, 12);
			b = peekb(0x01EA) == 1;
			fl::cmpb(peekb(0x01EA), 0x01);
			tick(0x4A54, 30);                          // CMPB @#0x01EA,#1
			if (!b)
			{
				tick(0x4A56, 12);                      // BEQ
				pokeb(0x031B, 0x3F);
				fl::movb(0x3F);
				tick(0x4A5C, 33);
			}
			else
				tick(0x4A5C, 12);
			T.r[2] = sext8(peekb(0x031B));
			fl::movb((uint8_t)T.r[2]);
			tick(0x4A60, 24);
			tick(0x4A74, 12);                          // BR
		}
		T.r[0] = sext8(peekb(0x01F8));
		fl::movb((uint8_t)T.r[0]);
		tick(0x4A78, 24);
		T.r[2] = fl::sub(T.r[2], T.r[0]);
		tick(0x4A7A, 12);
		jsr(0x4A7E, 0x4A0A);                           // fold_direction
		T.r[0] = sext8(peekb(0x01F8));
		fl::movb((uint8_t)T.r[0]);
		tick(0x4A82, 24);
		T.r[0] = fl::add(T.r[0], T.r[1]);
		tick(0x4A84, 12);
		T.r[3] = T.r[0];
		fl::mov(T.r[3]);
		tick(0x4A86, 12);
		T.r[3] = fl::logic((uint16_t)(T.r[3] & 0x000F));
		tick(0x4A8A, 18);
		pokeb(0x031A, 0);
		fl::clr();
		tick(0x4A8E, 27);
		pokeb(0x031A, fl::logicb((uint8_t)(peekb(0x031A) | T.r[3])));
		tick(0x4A92, 27);
	}
	T.r[3] = sext8(peekb(0x031E));
	fl::movb((uint8_t)T.r[3]);
	tick(0x4A96, 24);
	if (T.r[3])
	{
		tick(0x4A98, 12);                              // BEQ
		T.r[0] = peek(0x01B4);
		fl::mov(T.r[0]);
		tick(0x4A9C, 24);
		if (T.r[0] & 0x8000)
		{
			tick(0x4A9E, 12);                          // BGE
			T.r[0] = (uint16_t)-T.r[0];
			fl::neg((uint16_t)(-T.r[0]));
			tick(0x4AA0, 12);
		}
		else
			tick(0x4AA0, 12);
		fl::cmp(T.r[0], 0x0080);
		tick(0x4AA4, 18);                              // CMP R0,#0x80
		if ((int16_t)T.r[0] < 0x0080)
		{
			tick(0x4AA6, 12);                          // BGE
			pokeb(0x031C, 3);
			fl::movb(0x03);
			tick(0x4AAC, 33);
			b = (int16_t)peek(0x01B6) < 0;
			fl::tst(peek(0x01B6));
			tick(0x4AB0, 24);                          // TST @#0x01B6
			if (b)
			{
				tick(0x4ABA, 12);                      // BLT
				poke(0x01B4, 0xFEA0);
				fl::mov(0xFEA0);
				tick(0x4AC0, 33);
			}
			else
			{
				tick(0x4AB2, 12);
				poke(0x01B4, 0x0160);
				fl::mov(0x0160);
				tick(0x4AB8, 33);
				tick(0x4AC0, 12);                      // BR
			}
		}
		else
			tick(0x4AC0, 12);
		poke(0x01B6, peek(0x01B4));
		fl::mov(peek(0x01B6));
		tick(0x4AC6, 39);
	}
	else
		tick(0x4AC6, 12);
	T.r[0] = T.r[3];
	fl::mov(T.r[0]);
	tick(0x4AC8, 12);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x4ACA, 12);
	// the slide state (jump table 0x5160): 1 at 0x4DD0, 0 and 2 at 0x4ACE (others are not used)
	if (T.r[3] == 1)
	{
		tick(0x4DD0, 27);                              // ADD 0x5160(R0),PC
		goto at_4DD0;
	}
	fl::add((uint16_t)(T.r[7] - peek((uint16_t)(T.r[0] + 0x5160))), peek((uint16_t)(T.r[0] + 0x5160)));
	tick(0x4ACE, 27);
	fl::cmp(T.r[3], 0x0002);
	tick(0x4AD2, 18);                                  // CMP R3,#2
	if (T.r[3] == 2)
	{
		tick(0x4AD4, 12);                              // BNE
		b = peekb(0x0329) == peekb(0x0328);
		fl::cmpb(peekb(0x0329), peekb(0x0328));
		tick(0x4ADA, 36);                              // CMPB @#0x0329,@#0x0328
		if (!b)
		{
			tick(0x4ADC, 12);                          // BEQ
			T.r[0] = sext8(peekb(0x031C));
			fl::movb((uint8_t)T.r[0]);
			tick(0x4AE0, 24);
			T.r[0] = fl::dec(T.r[0]);
			tick(0x4AE2, 12);
			pokeb(0x031C, (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0x4AE6, 27);
			if (!(uint8_t)T.r[0])
			{
				tick(0x4AE8, 12);                      // BNE
				pokeb(0x031E, 0);
				fl::clr();
				tick(0x4AEC, 27);
				pokeb(0x0328, 0xFE);
				fl::movb(0xFE);
				tick(0x4AF2, 33);
			}
			else
				tick(0x4AF2, 12);
		}
		else
			tick(0x4AF2, 12);
	}
	else
		tick(0x4AF2, 12);                              // BNE
	b = peekb(0x031F) != 0;
	fl::tstb(peekb(0x031F));
	tick(0x4AF6, 24);                                  // TSTB @#0x031F
	if (b)
	{
		tick(0x4BA4, 12);                              // BNE
		tick(0x4E34, 18);                              // JMP
		goto at_4E34;
	}
	tick(0x4AF8, 12);
	b = peekb(0x01EA) != 0;
	fl::tstb(peekb(0x01EA));
	tick(0x4AFC, 24);                                  // TSTB @#0x01EA
	if (b)
	{
		tick(0x4BA8, 12);                              // BNE
		goto at_4BA8;
	}
	// on the ground: the kick / carve state 0x0323
	tick(0x4AFE, 12);
	T.r[5] = sext8(peekb(0x0323));
	fl::movb((uint8_t)T.r[5]);
	tick(0x4B02, 24);
	if (!T.r[5])
	{
		tick(0x4B04, 12);                              // BNE
		T.r[3] = peek(0x01C8);
		fl::mov(T.r[3]);
		tick(0x4B08, 24);
		T.r[3] = fl::asl(T.r[3]);
		tick(0x4B0A, 12);
		T.r[0] = sext8(peekb(0x025D));
		fl::movb((uint8_t)T.r[0]);
		tick(0x4B0E, 24);
		T.r[0] = sext8(peekb((uint16_t)(0x7CA0 + T.r[0])));
		fl::movb((uint8_t)T.r[0]);
		tick(0x4B12, 27);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
		tick(0x4B16, 18);
		fl::cmp(T.r[3], T.r[0]);
		tick(0x4B18, 12);                              // CMP R3,R0
		if ((int16_t)T.r[3] >= (int16_t)T.r[0])
		{
			tick(0x4B1A, 12);                          // BLT
			T.r[3] = T.r[5];
			fl::mov(T.r[3]);
			tick(0x4B1C, 12);
			T.r[3] = fl::inc(T.r[3]);
			tick(0x4B1E, 12);
			pokeb(0x0323, (uint8_t)T.r[3]);
			fl::movb((uint8_t)T.r[3]);
			tick(0x4B22, 27);
			b = (int16_t)peek(0x01CA) > 0;
			fl::tst(peek(0x01CA));
			tick(0x4B26, 24);                          // TST @#0x01CA
			if (!b)
			{
				tick(0x4B28, 12);                      // BGT
				T.r[0] = (uint16_t)-T.r[0];
				fl::neg((uint16_t)(-T.r[0]));
				tick(0x4B2A, 12);
			}
			else
				tick(0x4B2A, 12);
			pokeb(0x0321, (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0x4B2E, 27);
		}
		else
			tick(0x4B2E, 12);
		pokeb(0x01F8, peekb(0x031A));
		fl::movb(peekb(0x01F8));
		tick(0x4B34, 39);
		tick(0x4B94, 12);                              // BR
	}
	else
	{
		tick(0x4B36, 12);                              // BNE
		fl::cmp(T.r[5], 0x0001);
		tick(0x4B3A, 18);                              // CMP R5,#1
		if (T.r[5] == 1)
		{
			tick(0x4B3C, 12);                          // BNE
			T.r[4] = sext8(peekb(0x0321));
			fl::movb((uint8_t)T.r[4]);
			tick(0x4B40, 24);
			poke(0x02F4, fl::add(peek(0x02F4), T.r[4]));
			tick(0x4B44, 27);
			T.r[0] = peek(0x01CA);
			fl::mov(T.r[0]);
			tick(0x4B48, 24);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x4B4A, 12);
			T.r[0] = fl::add(T.r[0], peek(0x02F2));
			tick(0x4B4E, 24);
			poke(0x02F2, T.r[0]);
			fl::mov(T.r[0]);
			tick(0x4B52, 27);
			poke(0x02F6, fl::add(peek(0x02F6), T.r[4]));
			tick(0x4B56, 27);
			T.r[0] = peek(0x02F6);
			fl::mov(T.r[0]);
			tick(0x4B5A, 24);
			if (T.r[0] & 0x8000)
			{
				tick(0x4B5C, 12);                      // BGE
				poke(0x02F6, fl::add(peek(0x02F6), 0x0120));
				tick(0x4B62, 33);
				tick(0x4B70, 12);                      // BR
			}
			else
			{
				tick(0x4B64, 12);
				fl::cmp(T.r[0], 0x0120);
				tick(0x4B68, 18);                      // CMP R0,#0x0120
				if ((int16_t)T.r[0] >= 0x0120)
				{
					tick(0x4B6A, 12);                  // BLT
					poke(0x02F6, fl::sub(peek(0x02F6), 0x0120));
					tick(0x4B70, 33);
				}
				else
					tick(0x4B70, 12);
			}
			T.r[0] = peek(0x02F6);
			fl::mov(T.r[0]);
			tick(0x4B74, 24);
			T.r[0] = fl::asr(T.r[0]);
			tick(0x4B76, 12);
			T.r[0] = fl::asr(T.r[0]);
			tick(0x4B78, 12);
			pokeb(0x01F8, peekb((uint16_t)(0x6000 + T.r[0])));
			fl::movb(peekb(0x01F8));
			tick(0x4B7E, 42);
			T.r[2] = peek(0x02F4);
			fl::mov(T.r[2]);
			tick(0x4B82, 24);
			T.r[2] = fl::sub(T.r[2], peek(0x02F2));
			tick(0x4B86, 24);
			T.r[3] = T.r[4];
			fl::mov(T.r[3]);
			tick(0x4B88, 12);
			T.r[3] ^= T.r[2];
			fl::logic(T.r[3]);
			tick(0x4B8A, 12);
			if (T.r[3] & 0x8000)
				tick(0x4B94, 12);                      // BLT
			else
			{
				tick(0x4B8C, 12);
				T.r[0] = T.r[5];
				fl::mov(T.r[0]);
				tick(0x4B8E, 12);
				T.r[0] = fl::dec(T.r[0]);
				tick(0x4B90, 12);
				pokeb(0x0323, (uint8_t)T.r[0]);
				fl::movb((uint8_t)T.r[0]);
				tick(0x4B94, 27);
			}
		}
		else
			tick(0x4B94, 12);                          // BNE
	}
	T.r[0] = sext8(peekb(0x01F8));
	fl::movb((uint8_t)T.r[0]);
	tick(0x4B98, 24);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x000F));
	tick(0x4B9C, 18);
	pokeb(0x01F8, 0);
	fl::clr();
	tick(0x4BA0, 27);
	pokeb(0x01F8, fl::logicb((uint8_t)(peekb(0x01F8) | T.r[0])));
	tick(0x4BA4, 27);
	tick(0x4E34, 18);                                  // JMP
	goto at_4E34;

at_4BA8:                                               // in the air
	pokeb(0x01F8, peekb(0x031A));
	fl::movb(peekb(0x01F8));
	tick(0x4BAE, 39);
	T.r[0] = sext8(peekb(0x0327));
	fl::movb((uint8_t)T.r[0]);
	tick(0x4BB2, 24);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0007));
	tick(0x4BB6, 18);
	fl::cmp(T.r[0], 0x0005);
	tick(0x4BBA, 18);                                  // CMP R0,#5
	if (T.r[0] < 5)
	{
		tick(0x4BBC, 12);                              // BCC
		tick(0x4CD8, 18);                              // JMP
		goto at_4CD8;
	}
	tick(0x4BC0, 12);
	b = (peekb(0x0325) & 0x80) != 0;
	fl::tstb(peekb(0x0325));
	tick(0x4BC4, 24);                                  // TSTB @#0x0325
	if (!b)
	{
		tick(0x4C52, 12);                              // BGE
		goto at_4C52;
	}
	tick(0x4BC6, 12);
	jsr(0x4BCA, 0xE238);                               // reset_skater_state
	T.r[2] = sext8(peekb(0x031D));
	fl::movb((uint8_t)T.r[2]);
	tick(0x4BCE, 24);
	T.r[3] = peek(0x01B4);
	fl::mov(T.r[3]);
	tick(0x4BD2, 24);
	if (T.r[3] & 0x8000)
	{
		tick(0x4BD4, 12);                              // BGE
		T.r[1] = T.r[3];
		fl::mov(T.r[1]);
		tick(0x4BD6, 12);
		T.r[1] = (uint16_t)-T.r[1];
		fl::neg((uint16_t)(-T.r[1]));
		tick(0x4BD8, 12);
		T.r[0] = sext8(peekb(0x025F));
		fl::movb((uint8_t)T.r[0]);
		tick(0x4BDC, 24);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x4BDE, 12);
		poke(0x01B4, fl::add(peek(0x01B4), peek((uint16_t)(0x7C80 + T.r[0]))));
		tick(0x4BE4, 42);
		T.r[0] = 0x0008;
		fl::mov(T.r[0]);
		tick(0x4BE8, 18);
		T.r[2] ^= T.r[0];
		fl::logic(T.r[2]);
		tick(0x4BEA, 12);
		tick(0x4C04, 12);                              // BR
	}
	else
	{
		tick(0x4BEC, 12);
		T.r[1] = T.r[3];
		fl::mov(T.r[1]);
		tick(0x4BEE, 12);
		b = (int16_t)T.r[1] >= (int16_t)peek(0x02FC);
		fl::cmp(T.r[1], peek(0x02FC));
		tick(0x4BF2, 24);                              // CMP R1,@#0x02FC
		if (b)
			tick(0x4C04, 12);                          // BGE
		else
		{
			tick(0x4BF4, 12);
			T.r[0] = peek(0x02FC);
			fl::mov(T.r[0]);
			tick(0x4BF8, 24);
			T.r[0] = fl::sub(T.r[0], T.r[3]);
			tick(0x4BFA, 12);
			T.r[0] = fl::asr(T.r[0]);
			tick(0x4BFC, 12);
			T.r[0] = fl::add(T.r[0], peek(0x01B4));
			tick(0x4C00, 24);
			poke(0x01B4, T.r[0]);
			fl::mov(T.r[0]);
			tick(0x4C04, 27);
		}
	}
	T.r[0] = sext8(peekb(0x0327));
	fl::movb((uint8_t)T.r[0]);
	tick(0x4C08, 24);
	for (uint16_t pc = 0x4C0A; pc <= 0x4C0E; pc = (uint16_t)(pc + 2))
	{
		T.r[0] = asr(T.r[0], 1);
		tick(pc, 12);
	}
	T.r[0] = fl::asl(T.r[0]);
	tick(0x4C10, 12);
	poke(0x02F8, peek((uint16_t)(0x7A08 + T.r[0])));
	fl::mov(peek(0x02F8));
	tick(0x4C16, 42);
	T.r[0] = T.r[2];
	fl::mov(T.r[0]);
	tick(0x4C18, 12);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x4C1A, 12);
	T.r[0] = fl::add(T.r[0], peek(0x02F8));
	tick(0x4C1E, 24);
	poke(0x02F8, peek(T.r[0]));
	fl::mov(peek(0x02F8));
	tick(0x4C22, 33);
	b = peek(T.r[0]) == 0xFFFF;
	fl::cmp(peek(T.r[0]), 0xFFFF);
	tick(0x4C26, 24);                                  // CMP (R0),#-1
	if (b)
	{
		tick(0x4C28, 12);                              // BNE
		pokeb(0x0325, 0x7F);
		fl::movb(0x7F);
		tick(0x4C2E, 33);
		tick(0x4C52, 12);                              // BR
	}
	else
	{
		tick(0x4C30, 12);
		push(0x0580);
		fl::mov(0x0580);
		tick(0x4C34, 30);
		push(T.r[1]);
		fl::mov(T.r[1]);
		tick(0x4C36, 24);
		jsr(0x4C3A, 0x90A6);                           // divide
		pokeb(0x0325, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x4C3E, 27);
		T.r[0] = sext8((uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x4C40, 12);                              // MOVB R0,R0
		if (!T.r[0])
		{
			tick(0x4C42, 12);                          // BNE
			T.r[3] = T.r[0];
			fl::mov(T.r[3]);
			tick(0x4C44, 12);
			T.r[3] = fl::inc(T.r[3]);
			tick(0x4C46, 12);
			pokeb(0x0325, (uint8_t)T.r[3]);
			fl::movb((uint8_t)T.r[3]);
			tick(0x4C4A, 27);
		}
		else
			tick(0x4C4A, 12);
		pokeb(0x0326, peekb(0x0325));
		fl::movb(peekb(0x0326));
		tick(0x4C50, 39);
		T.r[6] = (uint16_t)(T.r[6] + 4);
		fl::cmp(peek((uint16_t)(T.r[6] - 4)), peek((uint16_t)(T.r[6] - 2)));
		tick(0x4C52, 24);                              // CMP (SP)+,(SP)+
	}
at_4C52:
	T.r[0] = sext8(peekb(0x0325));
	fl::movb((uint8_t)T.r[0]);
	tick(0x4C56, 24);
	fl::cmp(T.r[0], 0x007F);
	tick(0x4C5A, 18);                                  // CMP R0,#0x7F
	if (T.r[0] == 0x007F)
	{
		tick(0x4D48, 12);                              // BEQ
		tick(0x4DBE, 12);                              // BR
		goto at_4DBE;
	}
	tick(0x4C5C, 12);
	fl::cmp(T.r[0], 0x007E);
	tick(0x4C60, 18);                                  // CMP R0,#0x7E
	if (T.r[0] == 0x007E)
	{
		tick(0x4C62, 12);                              // BNE
		T.r[0] = sext8(peekb(0x0327));
		fl::movb((uint8_t)T.r[0]);
		tick(0x4C66, 24);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0018));
		tick(0x4C6A, 18);
		for (uint16_t pc = 0x4C6C; pc <= 0x4C70; pc = (uint16_t)(pc + 2))
		{
			T.r[0] = asr(T.r[0], 1);
			tick(pc, 12);
		}
		pokeb(0x031D, rb((uint16_t)(0xFCD0 + T.r[0])));
		tick(0x4C76, 42);
		b = (int16_t)peek(0x01B4) < 0;
		fl::tst(peek(0x01B4));
		tick(0x4C7A, 24);                              // TST @#0x01B4
		if (!b)
		{
			tick(0x4CA8, 12);                          // BGE
			goto at_4CA8;
		}
		tick(0x4C7C, 12);
		T.r[3] = 0x0008;
		fl::mov(T.r[3]);
		tick(0x4C80, 18);
		T.r[0] = sext8(rb((uint16_t)(0xFCD0 + T.r[0])));
		fl::movb((uint8_t)T.r[0]);
		tick(0x4C84, 27);
		T.r[3] ^= T.r[0];
		fl::logic(T.r[3]);
		tick(0x4C86, 12);
		pokeb(0x031D, (uint8_t)T.r[3]);
		fl::movb((uint8_t)T.r[3]);
		tick(0x4C8A, 27);
		tick(0x4CA8, 12);                              // BR
		goto at_4CA8;
	}
	tick(0x4C8C, 12);                                  // BNE
	T.r[3] = sext8(peekb(0x0326));
	fl::movb((uint8_t)T.r[3]);
	tick(0x4C90, 24);
	T.r[3] = fl::sub(T.r[3], T.r[0]);
	tick(0x4C92, 12);
	pokeb(0x0326, (uint8_t)T.r[3]);
	fl::movb((uint8_t)T.r[3]);
	tick(0x4C96, 27);
	for (;;)
	{
		T.r[3] = sext8(peekb(0x0326));
		fl::movb((uint8_t)T.r[3]);
		tick(0x4C9A, 24);
		if ((int16_t)T.r[3] > 0)
		{
			tick(0x4D98, 12);                          // BGT to the BNE at 0x4D98
			tick(0x4DBE, 12);
			goto at_4DBE;
		}
		tick(0x4C9C, 12);
		T.r[2] = sext8(peekb(peek(0x02F8)));
		fl::movb((uint8_t)T.r[2]);
		tick(0x4CA0, 33);
		poke(0x02F8, fl::inc(peek(0x02F8)));
		tick(0x4CA4, 27);
		fl::tst(T.r[2]);
		tick(0x4CA6, 12);                              // TST R2
		if (T.r[2] & 0x8000)
		{
			tick(0x4CA8, 12);                          // BGE
			goto at_4CA8;
		}
		tick(0x4CB0, 12);
		pokeb(0x031D, peekb(peek(0x02F8)));
		fl::movb(peekb(0x031D));
		tick(0x4CB6, 48);
		b = (int16_t)peek(0x01B4) < 0;
		fl::tst(peek(0x01B4));
		tick(0x4CBA, 24);                              // TST @#0x01B4
		if (b)
		{
			tick(0x4CBC, 12);                          // BGE
			T.r[4] = 0x0008;
			fl::mov(T.r[4]);
			tick(0x4CC0, 18);
			T.r[0] = sext8(peekb(0x031D));
			fl::movb((uint8_t)T.r[0]);
			tick(0x4CC4, 24);
			T.r[4] ^= T.r[0];
			fl::logic(T.r[4]);
			tick(0x4CC6, 12);
			pokeb(0x031D, (uint8_t)T.r[4]);
			fl::movb((uint8_t)T.r[4]);
			tick(0x4CCA, 27);
		}
		else
			tick(0x4CCA, 12);
		poke(0x02F8, fl::inc(peek(0x02F8)));
		tick(0x4CCE, 27);
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0x4CD0, 12);
		T.r[0] = fl::add(T.r[0], T.r[3]);
		tick(0x4CD2, 12);
		pokeb(0x0326, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x4CD6, 27);
		tick(0x4C96, 12);                              // BR
	}
at_4CA8:
	pokeb(0x0325, 0x7F);
	fl::movb(0x7F);
	tick(0x4CAE, 33);
	tick(0x4D7E, 12);                                  // BR
	tick(0x4DBE, 12);                                  // BR
	goto at_4DBE;

at_4CD8:                                               // the landing
	pokeb(0x0325, 0xFF);
	tick(0x4CDE, 33);
	pokeb(0x0324, 0);
	fl::clr();
	tick(0x4CE2, 27);
	T.r[2] = sext8(peekb(0x01F8));
	fl::movb((uint8_t)T.r[2]);
	tick(0x4CE6, 24);
	T.r[0] = sext8(peekb(0x031D));
	fl::movb((uint8_t)T.r[0]);
	tick(0x4CEA, 24);
	T.r[2] = fl::sub(T.r[2], T.r[0]);
	tick(0x4CEC, 12);
	jsr(0x4CF0, 0x4A0A);                               // fold_direction
	fl::cmp(T.r[2], 0x0004);
	tick(0x4CF4, 18);                                  // CMP R2,#4
	if ((int16_t)T.r[2] > 4)
	{
		tick(0x4CF6, 12);                              // BLE
		poke(0x01B4, (uint16_t)-peek(0x01B4));
		fl::neg((uint16_t)(-peek(0x01B4)));
		tick(0x4CFA, 27);
		T.r[3] = 0x0008;
		fl::mov(T.r[3]);
		tick(0x4CFE, 18);
		T.r[0] = sext8(peekb(0x031D));
		fl::movb((uint8_t)T.r[0]);
		tick(0x4D02, 24);
		T.r[3] ^= T.r[0];
		fl::logic(T.r[3]);
		tick(0x4D04, 12);
		pokeb(0x031D, (uint8_t)T.r[3]);
		fl::movb((uint8_t)T.r[3]);
		tick(0x4D08, 27);
	}
	else
		tick(0x4D08, 12);
	b = (int16_t)peek(0x01C8) > 3;
	fl::cmp(peek(0x01C8), 0x0003);
	tick(0x4D0E, 30);                                  // CMP @#0x01C8,#3
	if (b)
	{
		tick(0x4D10, 12);                              // BLE
		pokeb(0x0321, peekb(0x01CB));
		fl::movb(peekb(0x0321));
		tick(0x4D16, 39);
	}
	else
		tick(0x4D16, 12);
	b = peekb(0x01EA) == 2;
	fl::cmpb(peekb(0x01EA), 0x02);
	tick(0x4D1C, 30);                                  // CMPB @#0x01EA,#2
	if (b)
	{
		tick(0x4D80, 12);                              // BEQ
		T.r[1] = peek(0x01B4);
		fl::mov(T.r[1]);
		tick(0x4D84, 24);
		if (T.r[1] & 0x8000)
		{
			tick(0x4D86, 12);                          // BGE
			T.r[1] = (uint16_t)-T.r[1];
			fl::neg((uint16_t)(-T.r[1]));
			tick(0x4D88, 12);
		}
		else
			tick(0x4D88, 12);
		fl::cmp(T.r[1], 0x0080);
		tick(0x4D8C, 18);                              // CMP R1,#0x80
		if ((int16_t)T.r[1] < 0x0080)
		{
			tick(0x4D8E, 12);                          // BGE
			pokeb(0x031D, peekb(0x01F8));
			fl::movb(peekb(0x031D));
			tick(0x4D94, 39);
			tick(0x4D9A, 12);                          // BR
		}
		else
		{
			tick(0x4D96, 12);
			fl::tst(T.r[2]);
			tick(0x4D98, 12);                          // TST R2
			if (T.r[2])
			{
				tick(0x4DBE, 12);                      // BNE
				goto at_4DBE;
			}
			tick(0x4D9A, 12);
		}
		pokeb(0x01EA, 1);
		fl::movb(0x01);
		tick(0x4DA0, 33);
		jsr(0x4DA4, 0xE238);                           // reset_skater_state
		push(0x0058);
		fl::mov(0x0058);
		tick(0x4DA8, 30);
		jsr(0x4DAC, 0x8E78);                           // queue_sound
		T.r[0] = 0x3C00;
		fl::mov(T.r[0]);
		tick(0x4DB0, 18);
		poke(0x01D4, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4DB4, 27);
		poke(0x1402, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4DB8, 27);
		jsr(0x4DBC, 0x7E44);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(peek((uint16_t)(T.r[6] - 2)));
		tick(0x4DBE, 18);                              // TST (SP)+
		goto at_4DBE;
	}
	tick(0x4D1E, 12);
	b = (int16_t)peek(0x01C8) >= 8;
	fl::cmp(peek(0x01C8), 0x0008);
	tick(0x4D24, 30);                                  // CMP @#0x01C8,#8
	if (b)
	{
		tick(0x4DBE, 12);                              // BGE
		goto at_4DBE;
	}
	tick(0x4D26, 12);
	T.r[0] = sext8(peekb(0x01F8));
	fl::movb((uint8_t)T.r[0]);
	tick(0x4D2A, 24);
	push(T.r[0]);
	fl::mov(T.r[0]);
	tick(0x4D2C, 24);
	jsr(0x4D30, 0xA2BE);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	fl::tst(peek((uint16_t)(T.r[6] - 2)));
	tick(0x4D32, 18);                                  // TST (SP)+
	fl::tst(T.r[0]);
	tick(0x4D34, 12);                                  // TST R0
	if (T.r[0])
	{
		tick(0x4D36, 12);                              // BEQ
		T.r[0] = sext8(peekb(0x01F8));
		fl::movb((uint8_t)T.r[0]);
		tick(0x4D3A, 24);
		push(T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4D3C, 24);
		T.r[0] = sext8(peekb(0x0321));
		fl::movb((uint8_t)T.r[0]);
		tick(0x4D40, 24);
		push(T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4D42, 24);
		jsr(0x4D46, 0xA45A);
		T.r[6] = (uint16_t)(T.r[6] + 4);
		fl::cmp(peek((uint16_t)(T.r[6] - 4)), peek((uint16_t)(T.r[6] - 2)));
		tick(0x4D48, 24);                              // CMP (SP)+,(SP)+
		tick(0x4DBE, 12);                              // BR
		goto at_4DBE;
	}
	tick(0x4D4A, 12);
	T.r[0] = sext8(peekb(0x0331));
	fl::movb((uint8_t)T.r[0]);
	tick(0x4D4E, 24);
	T.r[3] = sext8(peekb(0x0322));
	fl::movb((uint8_t)T.r[3]);
	tick(0x4D52, 24);
	T.r[4] = 0;
	fl::clr();
	tick(0x4D54, 12);
	T.r[4] = peekb((uint16_t)(0x7CC0 + T.r[0]));
	fl::logicb((uint8_t)T.r[4]);
	tick(0x4D58, 27);
	T.r[3] = fl::add(T.r[3], T.r[4]);
	tick(0x4D5A, 12);
	pokeb(0x0322, (uint8_t)T.r[3]);
	fl::movb((uint8_t)T.r[3]);
	tick(0x4D5E, 27);
	T.r[0] = sext8((uint8_t)T.r[3]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x4D60, 12);
	if (T.r[0] & 0x8000)
	{
		tick(0x4D66, 12);                              // BLT
		T.r[3] = T.r[0];
		fl::mov(T.r[3]);
		tick(0x4D68, 12);
		T.r[3] = fl::logic((uint16_t)(T.r[3] & 0x007F));
		tick(0x4D6C, 18);
		pokeb(0x0322, 0);
		fl::clr();
		tick(0x4D70, 27);
		pokeb(0x0322, fl::logicb((uint8_t)(peekb(0x0322) | T.r[3])));
		tick(0x4D74, 27);
	}
	else
	{
		tick(0x4D62, 12);
		T.r[1] = 0;
		fl::clr();
		tick(0x4D64, 12);
		tick(0x4D74, 12);                              // BR
	}
	T.r[0] = sext8(peekb(0x031D));
	fl::movb((uint8_t)T.r[0]);
	tick(0x4D78, 24);
	T.r[0] = fl::add(T.r[0], T.r[1]);
	tick(0x4D7A, 12);
	pokeb(0x031D, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0x4D7E, 27);
	tick(0x4DBE, 12);                                  // BR

at_4DBE:
	T.r[0] = sext8(peekb(0x031D));
	fl::movb((uint8_t)T.r[0]);
	tick(0x4DC2, 24);
	T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x000F));
	tick(0x4DC6, 18);
	pokeb(0x031D, 0);
	fl::clr();
	tick(0x4DCA, 27);
	pokeb(0x031D, fl::logicb((uint8_t)(peekb(0x031D) | T.r[0])));
	tick(0x4DCE, 27);
	tick(0x4E34, 12);                                  // BR
	goto at_4E34;

at_4DD0:                                               // sliding
	b = peekb(0x01EB) != 0;
	fl::tstb(peekb(0x01EB));
	tick(0x4DD4, 24);                                  // TSTB @#0x01EB
	if (b)
	{
		tick(0x4DD6, 12);
		jsr(0x4DDA, 0xE238);                           // reset_skater_state
	}
	else
		tick(0x4DDA, 12);                              // BEQ
	b = (int8_t)peekb(0x0325) > 0;
	fl::tstb(peekb(0x0325));
	tick(0x4DDE, 24);                                  // TSTB @#0x0325
	if (b)
	{
		tick(0x4DE0, 12);                              // BLE
		pokeb(0x0325, 0x7E);
		fl::movb(0x7E);
		tick(0x4DE6, 33);
	}
	else
		tick(0x4DE6, 12);
	pokeb(0x01F8, peekb(0x031A));
	fl::movb(peekb(0x01F8));
	tick(0x4DEC, 39);
	b = peekb(0x01F8) == peekb(0x031B);
	fl::cmpb(peekb(0x01F8), peekb(0x031B));
	tick(0x4DF2, 36);                                  // CMPB @#0x01F8,@#0x031B
	if (b)
	{
		tick(0x4DF4, 12);                              // BNE
		b = peekb(0x0329) == peekb(0x0328);
		fl::cmpb(peekb(0x0329), peekb(0x0328));
		tick(0x4DFA, 36);                              // CMPB @#0x0329,@#0x0328
		if (!b)
		{
			tick(0x4DFC, 12);                          // BEQ
			T.r[0] = sext8(peekb(0x031C));
			fl::movb((uint8_t)T.r[0]);
			tick(0x4E00, 24);
			T.r[0] = fl::dec(T.r[0]);
			tick(0x4E02, 12);
			pokeb(0x031C, (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0x4E06, 27);
			if (!(uint8_t)T.r[0])
			{
				tick(0x4E08, 12);                      // BNE
				pokeb(0x031B, 0x3F);
				fl::movb(0x3F);
				tick(0x4E0E, 33);
				pokeb(0x031E, 0);
				fl::clr();
				tick(0x4E12, 27);
				pokeb(0x0328, 0xFE);
				fl::movb(0xFE);
				tick(0x4E18, 33);
			}
			else
				tick(0x4E18, 12);
		}
		else
			tick(0x4E18, 12);
	}
	else
		tick(0x4E18, 12);
	b = (peekb(0x031B) & 0x80) != 0;
	fl::tstb(peekb(0x031B));
	tick(0x4E1C, 24);                                  // TSTB @#0x031B
	if (b)
	{
		tick(0x4E1E, 12);                              // BGE
		pokeb(0x031E, 0);
		fl::clr();
		tick(0x4E22, 27);
		pokeb(0x0328, 0xFE);
		fl::movb(0xFE);
		tick(0x4E28, 33);
	}
	else
		tick(0x4E28, 12);
	b = peekb(0x01EA) != 0;
	fl::tstb(peekb(0x01EA));
	tick(0x4E2C, 24);                                  // TSTB @#0x01EA
	if (b)
	{
		tick(0x4E2E, 12);                              // BEQ
		pokeb(0x031D, peekb(0x01F8));
		fl::movb(peekb(0x031D));
		tick(0x4E34, 39);
	}
	else
		tick(0x4E34, 12);

at_4E34:
	b = peekb(0x01EB) == 5;
	fl::cmpb(peekb(0x01EB), 0x05);
	tick(0x4E3A, 30);                                  // CMPB @#0x01EB,#5
	if (b)
	{
		tick(0x4E3C, 12);                              // BNE
		T.r[1] = peek(0x02E8);
		fl::mov(T.r[1]);
		tick(0x4E40, 24);
		if (T.r[1] & 0x8000)
		{
			tick(0x4E4E, 12);                          // BLT
			fl::cmp(T.r[1], 0xFF80);
			tick(0x4E52, 18);                          // CMP R1,#-0x80
			if ((int16_t)T.r[1] >= -0x0080)
			{
				tick(0x4E54, 12);                      // BLT
				T.r[1] = fl::sub(T.r[1], 0x0100);
				tick(0x4E58, 18);
			}
			else
				tick(0x4E58, 12);
		}
		else
		{
			tick(0x4E42, 12);
			fl::cmp(T.r[1], 0x0080);
			tick(0x4E46, 18);                          // CMP R1,#0x80
			if ((int16_t)T.r[1] <= 0x0080)
			{
				tick(0x4E48, 12);                      // BGT
				T.r[1] = fl::add(T.r[1], 0x0100);
				tick(0x4E4C, 18);
				tick(0x4E58, 12);                      // BR
			}
			else
				tick(0x4E58, 12);
		}
		poke(0x01B4, T.r[1]);
		fl::mov(T.r[1]);
		tick(0x4E5C, 27);
		T.r[0] = 0x1C00;
		fl::mov(T.r[0]);
		tick(0x4E60, 18);
		poke(0x01D4, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4E64, 27);
		poke(0x1402, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4E68, 27);
		jsr(0x4E6C, 0x5B46);                           // rolling_sound
		jsr(0x4E70, 0x9FB4);
		T.r[0] = sext8(peekb(0x01EB));
		fl::movb((uint8_t)T.r[0]);
		tick(0x4E74, 24);
		T.r[0] = fl::inc(T.r[0]);
		tick(0x4E76, 12);
		pokeb(0x01EB, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0x4E7A, 27);
		T.r[0] = 0x2C00;
		fl::mov(T.r[0]);
		tick(0x4E7E, 18);
		poke(0x01D4, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4E82, 27);
		poke(0x1402, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4E86, 27);
		T.r[0] = sext8(peekb(0x025F));
		fl::movb((uint8_t)T.r[0]);
		tick(0x4E8A, 24);
		pokeb(0x05B2, peekb((uint16_t)(0x7C70 + T.r[0])));
		fl::movb(peekb(0x05B2));
		tick(0x4E90, 42);
		pokeb(0x05B3, 0);
		fl::clr();
		tick(0x4E94, 27);
	}
	else
		tick(0x4E94, 12);
	b = peekb(0x01EA) != 0;
	fl::tstb(peekb(0x01EA));
	tick(0x4E98, 24);                                  // TSTB @#0x01EA
	if (!b)
	{
		tick(0x4E9A, 12);                              // BNE
		tick(0x50E0, 18);                              // JMP
		goto at_50E0;
	}
	tick(0x4E9E, 12);
	jsr(0x4EA2, 0x9B82);
	b = peekb(0x031F) != 0;
	fl::tstb(peekb(0x031F));
	tick(0x4EA6, 24);                                  // TSTB @#0x031F
	if (b)
	{
		tick(0x4EA8, 12);                              // BEQ
		tick(0x502A, 18);                              // JMP
		goto at_502A;
	}
	tick(0x4EAC, 12);
	// the speed along the ground
	b = peekb(0x0EA0) != 0;
	fl::tstb(peekb(0x0EA0));
	tick(0x4EB0, 24);                                  // TSTB @#0x0EA0
	if (!b)
	{
		tick(0x4EB2, 12);                              // BNE
		poke(0x01B4, fl::add(peek(0x01B4), peek(0x02EE)));
		tick(0x4EB8, 39);
	}
	else
		tick(0x4EB8, 12);
	T.r[2] = 0;
	fl::clr();
	tick(0x4EBA, 12);
	T.r[3] = peek(0x01B4);
	fl::mov(T.r[3]);
	tick(0x4EBE, 24);
	T.r[0] = sext8(peekb(0x01EB));
	fl::movb((uint8_t)T.r[0]);
	tick(0x4EC2, 24);
	fl::cmp(T.r[0], 0x000A);
	tick(0x4EC6, 18);                                  // CMP R0,#10
	if (T.r[0] == 0x000A)
		tick(0x4ECE, 12);                              // BEQ
	else
	{
		tick(0x4EC8, 12);
		fl::cmp(T.r[0], 0x000B);
		tick(0x4ECC, 18);                              // CMP R0,#11
		if (T.r[0] != 0x000B)
		{
			tick(0x4EDE, 12);                          // BNE
			goto at_4EDE;
		}
		tick(0x4ECE, 12);
	}
	b = peekb(0x031E) != 0;
	fl::tstb(peekb(0x031E));
	tick(0x4ED2, 24);                                  // TSTB @#0x031E
	if (b)
	{
		tick(0x4EDE, 12);                              // BNE
		goto at_4EDE;
	}
	tick(0x4ED4, 12);
	fl::tst(T.r[3]);
	tick(0x4ED6, 12);                                  // TST R3
	if ((int16_t)T.r[3] <= 0)
	{
		tick(0x4EEE, 12);                              // BLE
		goto at_4EEE;
	}
	tick(0x4ED8, 12);
	T.r[2] = 0xFFF0;
	fl::mov(T.r[2]);
	tick(0x4EDC, 18);
	tick(0x4F54, 12);                                  // BR
	goto at_4F54;
at_4EDE:
	fl::tst(T.r[3]);
	tick(0x4EE0, 12);                                  // TST R3
	if (T.r[3] & 0x8000)
	{
		tick(0x4EE2, 12);                              // BGE
		fl::cmp(T.r[0], 0x0001);
		tick(0x4EE6, 18);                              // CMP R0,#1
		if (T.r[0] == 1)
		{
			tick(0x4EE8, 12);                          // BNE
			b = peekb(0x07B2) != 0;
			fl::tstb(peekb(0x07B2));
			tick(0x4EEC, 24);                          // TSTB @#0x07B2
			if (b)
			{
				tick(0x4EEE, 12);                      // BEQ
				goto at_4EEE;
			}
			tick(0x4EF4, 12);
		}
		else
			tick(0x4EF4, 12);
	}
	else
		tick(0x4EF4, 12);
	fl::cmp(T.r[0], 0x0002);
	tick(0x4EF8, 18);                                  // CMP R0,#2
	if (T.r[0] == 2)
	{
		tick(0x4EFA, 12);                              // BNE
		T.r[3] = T.r[0];
		fl::mov(T.r[3]);
		tick(0x4EFC, 12);
		T.r[3] = fl::inc(T.r[3]);
		tick(0x4EFE, 12);
		pokeb(0x01EB, (uint8_t)T.r[3]);
		fl::movb((uint8_t)T.r[3]);
		tick(0x4F02, 27);
		T.r[0] = 0x1C00;
		fl::mov(T.r[0]);
		tick(0x4F06, 18);
		poke(0x01D4, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4F0A, 27);
		poke(0x1402, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4F0E, 27);
		jsr(0x4F12, 0x5B46);                           // rolling_sound
		T.r[0] = 0x2C00;
		fl::mov(T.r[0]);
		tick(0x4F16, 18);
		poke(0x01D4, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4F1A, 27);
		poke(0x1402, T.r[0]);
		fl::mov(T.r[0]);
		tick(0x4F1E, 27);
		push(0x0052);
		fl::mov(0x0052);
		tick(0x4F22, 30);
		jsr(0x4F26, 0x8E78);                           // queue_sound
		b = (int16_t)peek(0x01B4) > 0;
		fl::tst(peek(0x01B4));
		tick(0x4F2A, 24);                              // TST @#0x01B4
		if (!b)
		{
			tick(0x4F2C, 12);                          // BGT
			poke(0x01B4, 0x0180);
			fl::mov(0x0180);
			tick(0x4F32, 33);
		}
		else
			tick(0x4F32, 12);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(peek((uint16_t)(T.r[6] - 2)));
		tick(0x4F34, 18);                              // TST (SP)+
	}
	else
		tick(0x4F34, 12);
	b = peekb(0x01EB) == 3;
	fl::cmpb(peekb(0x01EB), 0x03);
	tick(0x4F3A, 30);                                  // CMPB @#0x01EB,#3
	if (b)
	{
		tick(0x4F3C, 12);                              // BNE
		T.r[2] = peek(0x02FC);
		fl::mov(T.r[2]);
		tick(0x4F40, 24);
		T.r[2] = fl::sub(T.r[2], peek(0x01B4));
		tick(0x4F44, 24);
		if (T.r[2] & 0x8000)
		{
			tick(0x4F46, 12);                          // BPL
			T.r[2] = 0;
			fl::clr();
			tick(0x4F48, 12);
			tick(0x4F54, 12);                          // BR
		}
		else
		{
			tick(0x4F4A, 12);
			fl::cmp(T.r[2], 0x0050);
			tick(0x4F4E, 18);                          // CMP R2,#0x50
			if ((int16_t)T.r[2] > 0x0050)
			{
				tick(0x4F50, 12);                      // BLE
				T.r[2] = 0x0050;
				fl::mov(T.r[2]);
				tick(0x4F54, 18);
			}
			else
				tick(0x4F54, 12);
		}
	}
	else
		tick(0x4F54, 12);
	goto at_4F54;
at_4EEE:
	T.r[2] = 0x0010;
	fl::mov(T.r[2]);
	tick(0x4EF2, 18);
	tick(0x4F54, 12);                                  // BR
at_4F54:
	b = peek(0x01B4) != 0;
	fl::tst(peek(0x01B4));
	tick(0x4F58, 24);                                  // TST @#0x01B4
	if (!b)
	{
		tick(0x4FF6, 12);                              // BEQ
		goto at_4FF6;
	}
	tick(0x4F5A, 12);
	T.r[1] = 0;
	fl::clr();
	tick(0x4F5C, 12);
	b = (int16_t)peek(0x01B4) < 0;
	fl::tst(peek(0x01B4));
	tick(0x4F60, 24);                                  // TST @#0x01B4
	if (b)
	{
		tick(0x4F68, 12);                              // BLT
		T.r[0] = peek(0x01B4);
		fl::mov(T.r[0]);
		tick(0x4F6C, 24);
		T.r[0] = (uint16_t)-T.r[0];
		fl::neg((uint16_t)(-T.r[0]));
		tick(0x4F6E, 12);
		T.r[4] = T.r[0];
		fl::mov(T.r[4]);
		tick(0x4F70, 12);
	}
	else
	{
		tick(0x4F62, 12);
		T.r[4] = peek(0x01B4);
		fl::mov(T.r[4]);
		tick(0x4F66, 24);
		tick(0x4F70, 12);                              // BR
	}
	T.r[3] = sext8(peekb(0x025C));
	fl::movb((uint8_t)T.r[3]);
	tick(0x4F74, 24);
	T.r[5] = sext8(peekb(0x0327));
	fl::movb((uint8_t)T.r[5]);
	tick(0x4F78, 24);
	T.r[5] = fl::logic((uint16_t)(T.r[5] & 0x0007));
	tick(0x4F7C, 18);
	T.r[0] = T.r[5];
	fl::mov(T.r[0]);
	tick(0x4F7E, 12);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x4F80, 12);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x4F82, 12);
	T.r[0] = fl::add(T.r[0], T.r[3]);
	tick(0x4F84, 12);
	T.r[0] = fl::asl(T.r[0]);
	tick(0x4F86, 12);
	b = (int16_t)T.r[4] <= (int16_t)peek((uint16_t)(0x7BE8 + T.r[0]));
	fl::cmp(T.r[4], peek((uint16_t)(T.r[0] + 0x7BE8)));
	tick(0x4F8A, 27);                                  // CMP R4,0x7BE8(R0)
	if (b)
	{
		tick(0x4FDE, 12);                              // BLE
		goto at_4FDE;
	}
	tick(0x4F8C, 12);
	T.r[4] = peek(0x02EE);
	fl::mov(T.r[4]);
	tick(0x4F90, 24);
	T.r[0] = peek(0x01B4);
	fl::mov(T.r[0]);
	tick(0x4F94, 24);
	T.r[4] ^= T.r[0];
	fl::logic(T.r[4]);
	tick(0x4F96, 12);
	if (T.r[4] & 0x8000)
	{
		tick(0x4FDE, 12);                              // BLT
		goto at_4FDE;
	}
	tick(0x4F98, 12);
	T.r[0] = peek(0x02EE);
	fl::mov(T.r[0]);
	tick(0x4F9C, 24);
	if (T.r[0] & 0x8000)
	{
		tick(0x4F9E, 12);                              // BGE
		T.r[0] = (uint16_t)-T.r[0];
		fl::neg((uint16_t)(-T.r[0]));
		tick(0x4FA0, 12);
	}
	else
		tick(0x4FA0, 12);
	T.r[4] = T.r[0];
	fl::mov(T.r[4]);
	tick(0x4FA2, 12);
	b = peekb(0x0F59) == 3;
	fl::cmpb(peekb(0x0F59), 0x03);
	tick(0x4FA8, 30);                                  // CMPB @#0x0F59,#3
	if (b)
	{
		tick(0x4FBC, 12);                              // BEQ
		T.r[0] = T.r[3];
		fl::mov(T.r[0]);
		tick(0x4FBE, 12);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x4FC0, 12);
		b = (int16_t)peek(0x01B4) >= (int16_t)peek((uint16_t)(0x7C68 + T.r[0]));
		fl::cmp(peek(0x01B4), peek((uint16_t)(T.r[0] + 0x7C68)));
		tick(0x4FC6, 39);                              // CMP @#0x01B4,0x7C68(R0)
		if (b)
		{
			tick(0x4FD2, 12);                          // BGE
			T.r[0] = 0;
			tick(0x4FD4, 12);
		}
		else
		{
			tick(0x4FC8, 12);
			T.r[0] = T.r[5];
			fl::mov(T.r[0]);
			tick(0x4FCA, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x4FCC, 12);
			T.r[0] = peek((uint16_t)(0x7C38 + T.r[0]));
			fl::mov(T.r[0]);
			tick(0x4FD0, 27);
			tick(0x4FD4, 12);                          // BR
		}
	}
	else
	{
		tick(0x4FAA, 12);
		b = (peek(0x01C4) & 0x000F) != 0;
		fl::logic((uint16_t)(0x000F & peek(0x01C4)));
		tick(0x4FB0, 30);                              // BIT #15,@#0x01C4
		if (b)
		{
			tick(0x4FD2, 12);                          // BNE
			T.r[0] = 0;
			tick(0x4FD4, 12);
		}
		else
		{
			tick(0x4FB2, 12);
			T.r[0] = T.r[5];
			fl::mov(T.r[0]);
			tick(0x4FB4, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x4FB6, 12);
			T.r[0] = peek((uint16_t)(0x7C28 + T.r[0]));
			fl::mov(T.r[0]);
			tick(0x4FBA, 27);
			tick(0x4FD4, 12);                          // BR
		}
	}
	T.r[4] = fl::sub(T.r[4], T.r[0]);
	tick(0x4FD6, 12);
	T.r[1] = T.r[4];
	fl::mov(T.r[1]);
	tick(0x4FD8, 12);
	if (T.r[1] & 0x8000)
	{
		tick(0x4FDA, 12);                              // BGE
		T.r[1] = 0;
		fl::clr();
		tick(0x4FDC, 12);
		tick(0x4FEC, 12);                              // BR
	}
	else
		tick(0x4FEC, 12);
	goto at_4FEC;
at_4FDE:
	b = (peek(0x01C4) & 0x000F) != 0;
	fl::logic((uint16_t)(0x000F & peek(0x01C4)));
	tick(0x4FE4, 30);                                  // BIT #15,@#0x01C4
	if (!b)
	{
		tick(0x4FE6, 12);                              // BNE
		T.r[1] = 0;
		fl::clr();
		tick(0x4FE8, 12);
		T.r[1] = peekb((uint16_t)(0x7C64 + T.r[3]));
		fl::logicb((uint8_t)T.r[1]);
		tick(0x4FEC, 27);
	}
	else
		tick(0x4FEC, 12);
at_4FEC:
	b = (int16_t)peek(0x01B4) > 0;
	fl::tst(peek(0x01B4));
	tick(0x4FF0, 24);                                  // TST @#0x01B4
	if (b)
	{
		tick(0x4FF2, 12);                              // BLE
		T.r[1] = (uint16_t)-T.r[1];
		fl::neg((uint16_t)(-T.r[1]));
		tick(0x4FF4, 12);
	}
	else
		tick(0x4FF4, 12);
	T.r[2] = fl::add(T.r[2], T.r[1]);
	tick(0x4FF6, 12);
at_4FF6:
	b = (peekb(0x0327) & 7) != 0;
	fl::logicb((uint8_t)(0x07 & peekb(0x0327)));
	tick(0x4FFC, 30);                                  // BITB #7,@#0x0327
	if (b)
	{
		tick(0x5004, 12);                              // BNE
		T.r[1] = 0;
		fl::clr();
		tick(0x5006, 12);
	}
	else
	{
		tick(0x4FFE, 12);
		T.r[1] = 0x0080;
		fl::mov(T.r[1]);
		tick(0x5002, 18);
		tick(0x5006, 12);                              // BR
	}
	b = (int16_t)peek(0x01B4) < 0;
	fl::tst(peek(0x01B4));
	tick(0x500A, 24);                                  // TST @#0x01B4
	if (b)
	{
		tick(0x5018, 12);                              // BLT
		poke(0x01B4, fl::add(peek(0x01B4), T.r[2]));
		tick(0x501C, 27);
		T.r[3] = T.r[1];
		fl::mov(T.r[3]);
		tick(0x501E, 12);
		T.r[3] = (uint16_t)-T.r[3];
		fl::neg((uint16_t)(-T.r[3]));
		tick(0x5020, 12);
		b = (int16_t)peek(0x01B4) <= (int16_t)T.r[3];
		fl::cmp(peek(0x01B4), T.r[3]);
		tick(0x5024, 24);                              // CMP @#0x01B4,R3
		if (b)
		{
			tick(0x502A, 12);                          // BLE
			goto at_502A;
		}
		tick(0x5026, 12);
	}
	else
	{
		tick(0x500C, 12);
		poke(0x01B4, fl::add(peek(0x01B4), T.r[2]));
		tick(0x5010, 27);
		b = (int16_t)peek(0x01B4) >= (int16_t)T.r[1];
		fl::cmp(peek(0x01B4), T.r[1]);
		tick(0x5014, 24);                              // CMP @#0x01B4,R1
		if (b)
		{
			tick(0x502A, 12);                          // BGE
			goto at_502A;
		}
		tick(0x5016, 12);
		tick(0x5026, 12);                              // BR
	}
	poke(0x01B4, 0);
	fl::clr();
	tick(0x502A, 27);
at_502A:                                               // the vertical speed
	jsr(0x502E, 0x91A6);
	T.r[3] = sext8(peekb(0x0327));
	fl::movb((uint8_t)T.r[3]);
	tick(0x5032, 24);
	fl::logic((uint16_t)(0x0007 & T.r[3]));
	tick(0x5036, 18);                                  // BIT #7,R3
	if (T.r[3] & 7)
	{
		tick(0x50B8, 12);                              // BNE
		push(peek(0x01B4));
		fl::mov(peek(T.r[6]));
		tick(0x50BC, 36);
		push(peek(0x02E6));
		fl::mov(peek(T.r[6]));
		tick(0x50C0, 36);
		jsr(0x50C4, 0x80FA);                           // mul32
		pokeb(0x07F8, peekb(0x0205));
		fl::movb(peekb(0x07F8));
		tick(0x50CA, 39);
		poke(0x05B0, peek(0x0206));
		fl::mov(peek(0x05B0));
		tick(0x50D0, 39);
		T.r[6] = (uint16_t)(T.r[6] + 4);
		fl::cmp(peek((uint16_t)(T.r[6] - 4)), peek((uint16_t)(T.r[6] - 2)));
		tick(0x50D2, 24);                              // CMP (SP)+,(SP)+
	}
	else
	{
		tick(0x5038, 12);
		b = peekb(0x0EA0) != 0;
		fl::tstb(peekb(0x0EA0));
		tick(0x503C, 24);                              // TSTB @#0x0EA0
		if (b)
			tick(0x50D2, 12);                          // BNE
		else
		{
			tick(0x503E, 12);
			pokeb(0x07F0, 0);
			fl::clr();
			tick(0x5042, 27);
			pokeb(0x07F4, 0);
			fl::clr();
			tick(0x5046, 27);
			poke(0x04F0, fl::add(peek(0x04F0), 8));
			tick(0x504C, 33);
			poke(0x0570, fl::add(peek(0x0570), 8));
			tick(0x5052, 33);
			poke(0x04F0, fl::logic((uint16_t)(peek(0x04F0) & ~0x000F)));
			tick(0x5058, 33);
			poke(0x0570, fl::logic((uint16_t)(peek(0x0570) & ~0x000F)));
			tick(0x505E, 33);
			b = peek(0x01B4) != 0;
			fl::tst(peek(0x01B4));
			tick(0x5062, 24);                          // TST @#0x01B4
			bool flat = !b;
			if (b)
			{
				tick(0x5064, 12);                      // BEQ
				T.r[0] = T.r[3];
				fl::mov(T.r[0]);
				tick(0x5066, 12);
				for (uint16_t pc = 0x5068; pc <= 0x506C; pc = (uint16_t)(pc + 2))
				{
					T.r[0] = asr(T.r[0], 1);
					tick(pc, 12);
				}
				fl::cmp(T.r[0], 0x000F);
				tick(0x5070, 18);                      // CMP R0,#15
				flat = T.r[0] == 0x000F;
				tick(flat ? 0x50B2 : 0x5072, 12);      // BEQ
			}
			else
				tick(0x50B2, 12);
			if (flat)
			{
				poke(0x05B0, 0);
				fl::clr();
				tick(0x50B6, 27);
				tick(0x50D2, 12);                      // BR
			}
			else
			{
				pokeb(0x07F8, 0);
				fl::clr();
				tick(0x5076, 27);
				T.r[0] = T.r[3];
				fl::mov(T.r[0]);
				tick(0x5078, 12);
				for (uint16_t pc = 0x507A; pc <= 0x507E; pc = (uint16_t)(pc + 2))
				{
					T.r[0] = asr(T.r[0], 1);
					tick(pc, 12);
				}
				T.r[0] = (uint16_t)(T.r[0] << 1);
				tick(0x5080, 12);
				T.r[1] = rw((uint16_t)(0xFCB0 + T.r[0]));
				fl::mov(T.r[1]);
				tick(0x5084, 27);
				T.r[1] = fl::sub(T.r[1], peek(0x06B0));
				tick(0x5088, 24);
				T.r[0] = 0;
				fl::clr();
				tick(0x508A, 12);
				T.r[0] = peekb(0x0F59);
				fl::logicb((uint8_t)T.r[0]);
				tick(0x508E, 24);
				T.r[0] = sext8(peekb((uint16_t)(0x7CD0 + T.r[0])));
				fl::movb((uint8_t)T.r[0]);
				tick(0x5092, 27);
				T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
				tick(0x5096, 18);
				fl::tst(T.r[1]);
				tick(0x5098, 12);                      // TST R1
				if (T.r[1] & 0x8000)
				{
					tick(0x50A0, 12);                  // BLT
					T.r[3] = T.r[0];
					fl::mov(T.r[3]);
					tick(0x50A2, 12);
					T.r[3] = (uint16_t)-T.r[3];
					fl::neg((uint16_t)(-T.r[3]));
					tick(0x50A4, 12);
					fl::cmp(T.r[1], T.r[3]);
					tick(0x50A6, 12);                  // CMP R1,R3
					if ((int16_t)T.r[1] >= (int16_t)T.r[3])
						tick(0x50AC, 12);              // BGE
					else
					{
						tick(0x50A8, 12);
						T.r[0] = (uint16_t)-T.r[0];
						fl::neg((uint16_t)(-T.r[0]));
						tick(0x50AA, 12);
						T.r[1] = T.r[0];
						tick(0x50AC, 12);
					}
				}
				else
				{
					tick(0x509A, 12);
					fl::cmp(T.r[1], T.r[0]);
					tick(0x509C, 12);                  // CMP R1,R0
					if ((int16_t)T.r[1] <= (int16_t)T.r[0])
						tick(0x50AC, 12);              // BLE
					else
					{
						tick(0x509E, 12);
						tick(0x50AA, 12);              // BR
						T.r[1] = T.r[0];
						tick(0x50AC, 12);
					}
				}
				poke(0x05B0, T.r[1]);
				fl::mov(T.r[1]);
				tick(0x50B0, 27);
				tick(0x50D2, 12);                      // BR
			}
		}
	}
	poke(0x05B2, peek(0x05B0));
	fl::mov(peek(0x05B2));
	tick(0x50D8, 39);
	pokeb(0x07F9, peekb(0x07F8));
	fl::movb(peekb(0x07F9));
	tick(0x50DE, 39);
	tick(0x50FC, 12);                                  // BR
	goto at_50FC;
at_50E0:                                               // in the air: gravity
	poke(0x05B2, fl::sub(peek(0x05B2), peek(0x02EE)));
	tick(0x50E6, 39);
	b = (int16_t)peek(0x05B2) >= (int16_t)0xFE00;
	fl::cmp(peek(0x05B2), 0xFE00);
	tick(0x50EC, 30);                                  // CMP @#0x05B2,#-0x200
	if (!b)
	{
		tick(0x50EE, 12);                              // BGE
		b = peekb(0x0F59) == 3;
		fl::cmpb(peekb(0x0F59), 0x03);
		tick(0x50F4, 30);                              // CMPB @#0x0F59,#3
		if (!b)
		{
			tick(0x50F6, 12);                          // BEQ
			poke(0x05B2, 0xFE00);
			fl::mov(0xFE00);
			tick(0x50FC, 33);
		}
		else
			tick(0x50FC, 12);
	}
	else
		tick(0x50FC, 12);
at_50FC:                                               // the sprite priority bits
	b = peekb(0x032F) == 1;
	fl::cmpb(peekb(0x032F), 0x01);
	tick(0x5102, 30);                                  // CMPB @#0x032F,#1
	if (b)
	{
		tick(0x510A, 12);                              // BEQ
		T.r[2] = 0x0040;
		fl::mov(T.r[2]);
		tick(0x510E, 18);
	}
	else
	{
		tick(0x5104, 12);
		T.r[2] = 0x00C0;
		fl::mov(T.r[2]);
		tick(0x5108, 18);
		tick(0x510E, 12);                              // BR
	}
	uint16_t pc = 0x510E;
	for (uint16_t a = 0x0470; a < 0x0474; a++)
	{
		if (a & 1)
			T.r[0] = sext8(peekb(a));                  // MOVB
		else
			T.r[0] = peek(a);                          // MOV
		tick((uint16_t)(pc + 4), 24);
		T.r[0] &= 0x003F;
		tick((uint16_t)(pc + 8), 18);
		pokeb(a, 0);
		fl::clr();
		tick((uint16_t)(pc + 12), 27);
		if (a & 1)
			pokeb(a, (uint8_t)(peekb(a) | T.r[0]));    // BISB R0
		else
			poke(a, (uint16_t)(peek(a) | T.r[0]));     // BIS R0
		tick((uint16_t)(pc + 16), 27);
		const uint8_t v = (uint8_t)(peekb(a) | T.r[2]);
		pokeb(a, v);
		f_nzv(nzb(v));
		pc = (uint16_t)(pc + 20);
		tick(pc, 27);
	}
	rts();
}

// A7:45B0  enter_high_score(int first, long score): put score into the table of 15 entries
// from first (A7:43C0).  For a new entry the screen is cleared and the table shown (ranks,
// initials with the large font, scores by F:BA9A; the new place highlighted, colour 0x4000);
// for the main table (flag word bit 0: first is 0, its place kept in 0x0208) or when the
// score did not make it there (0x0208 = -1) the player enters three initials with the
// joystick (F:8D86 / F:8E2E; '?' deletes, 0x3F-0x5A wrap, 30 seconds counted down in the
// corner), else the initials of the main table's entry 0x0208 are copied.  The
// entry is sent to the sound CPU, which keeps the table (command 0x0F / 0x10 by the flag
// word, F:8F50 / F:8F62).  When the player's slot 0x0F3A is set and first is 0 a "game
// over" screen (texts 0x78F8 / 0x21 or 0x32, delay of 600 frames) comes first.  R0 ends as
// 0x3C00.
uint16_t enter_high_score(const Args &a)
{
	c_save(4, 0x45B4);
	T.r[6] = (uint16_t)(T.r[6] - 0x0010);
	const uint16_t S = T.r[6];
	// the locals: the entry's offset (place * 8), the letter (byte), the letters entered, the
	// seconds left (byte), the place in the shown part, the rank's position pointer (into
	// 0x7FB8), the last place shown, the flag word (1: first is 0)
	const uint16_t E8 = S, LET = (uint16_t)(S + 2), CNT = (uint16_t)(S + 4), SEC = (uint16_t)(S + 6);
	const uint16_t REL = (uint16_t)(S + 8), ROW = (uint16_t)(S + 0x0A), LAST = (uint16_t)(S + 0x0C);
	const uint16_t FLAG = (uint16_t)(S + 0x0E);
	fl::sub((uint16_t)(T.r[6] + 0x0010), 0x0010);
	tick(0x45B8, 18);                                  // SUB #0x10,SP
	T.r[0] = 0x1C00;
	fl::mov(T.r[0]);
	tick(0x45BC, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x45C0, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x45C4, 27);
	push(a.u(2));
	fl::mov(peek(T.r[6]));
	tick(0x45C8, 39);
	push(a.u(1));
	fl::mov(peek(T.r[6]));
	tick(0x45CC, 39);
	push(a.u(0));
	fl::mov(peek(T.r[6]));
	tick(0x45D0, 39);
	jsr(0x45D4, 0x43C0);                               // insert_high_score
	T.r[2] = T.r[0];
	fl::mov(T.r[2]);
	tick(0x45D6, 12);
	poke(0x0184, 0x5CD2);
	fl::mov(0x5CD2);
	tick(0x45DC, 33);
	T.r[0] = 0;
	fl::clr();
	tick(0x45DE, 12);
	T.r[0] = peekb(0x01F5);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x45E2, 24);
	bool b = peekb((uint16_t)(0x0F3A + T.r[0])) != 0;
	fl::tstb(peekb((uint16_t)(T.r[0] + 0x0F3A)));
	tick(0x45E6, 27);                                  // TSTB 0xF3A(R0)
	if (b)
	{
		tick(0x45E8, 12);                              // BEQ
		b = a.u(0) != 0;
		fl::tst(peek((uint16_t)(T.r[6] + 0x24)));
		tick(0x45EC, 27);                              // TST 0x24(SP)
		if (!b)
		{
			tick(0x45EE, 12);                          // BNE
			jsr(0x45F2, 0x8CE8);                       // clear_playfield
			jsr(0x45F6, 0x8D2C);
			poke(T.r[6], 0x0045);
			fl::mov(0x0045);
			tick(0x45FA, 27);
			jsr(0x45FE, 0x8E78);                       // queue_sound
			poke(0x0188, 0xE000);
			fl::mov(0xE000);
			tick(0x4604, 33);
			poke(T.r[6], 0x78F8);
			fl::mov(0x78F8);
			tick(0x4608, 27);
			jsr(0x460C, 0xA9FC);                       // "game over"
			b = peekb(0x01F6) != 0;
			fl::tstb(peekb(0x01F6));
			tick(0x4610, 24);                          // TSTB @#0x01F6
			if (!b)
			{
				tick(0x4612, 12);                      // BNE
				poke(0x0186, 0x275E);
				fl::mov(0x275E);
				tick(0x4618, 33);
				T.r[0] = 0x0021;
				fl::mov(T.r[0]);
				tick(0x461C, 18);
				jsr(0x4620, 0x8B52);                   // draw_large_char
				tick(0x463E, 12);                      // BR
			}
			else
			{
				tick(0x4622, 12);
				T.r[0] = 0x0032;
				fl::mov(T.r[0]);
				tick(0x4626, 18);
				jsr(0x462A, 0x8AB6);
				poke(0x0186, fl::sub(peek(0x0186), 8));
				tick(0x4630, 33);
				T.r[0] = 0;
				fl::clr();
				tick(0x4632, 12);
				T.r[0] = peekb(0x01F5);
				fl::logicb((uint8_t)T.r[0]);
				tick(0x4636, 24);
				T.r[0] = fl::add(T.r[0], 0x0031);
				tick(0x463A, 18);
				jsr(0x463E, 0x8C56);                   // draw_big_char: the player
			}
			poke(T.r[6], 0x0258);
			fl::mov(0x0258);
			tick(0x4642, 27);
			jsr(0x4646, 0x8D74);                       // wait_frames(600)
		}
		else
			tick(0x4646, 12);
	}
	else
		tick(0x4646, 12);                              // BEQ
	poke(FLAG, 0);
	fl::clr();
	tick(0x464A, 30);
	b = a.u(0) != 0;
	fl::tst(peek((uint16_t)(T.r[6] + 0x24)));
	tick(0x464E, 27);                                  // TST 0x24(SP)
	if (!b)
	{
		tick(0x4650, 12);                              // BNE
		poke(FLAG, fl::inc(peek(FLAG)));
		tick(0x4654, 30);
		poke(0x0208, T.r[2]);
		fl::mov(T.r[2]);
		tick(0x4658, 27);
	}
	else
		tick(0x4658, 12);
	fl::tst(T.r[2]);
	tick(0x465A, 12);                                  // TST R2
	if (T.r[2] & 0x8000)
	{
		tick(0x465C, 12);                              // BGE
		tick(0x4A4E, 18);                              // JMP: not a high score
	}
	else
	{
		tick(0x4660, 12);
		jsr(0x4664, 0x8CE8);                           // clear_playfield
		jsr(0x4668, 0x8D2C);
		poke(0x0188, 0xE000);
		fl::mov(0xE000);
		tick(0x466E, 33);
		poke(T.r[6], 0x5F1E);
		fl::mov(0x5F1E);
		tick(0x4672, 27);
		push(0);
		fl::clr();
		tick(0x4674, 24);
		jsr(0x4678, 0xEAD4);                           // draw_tile_block(0, 0x5F1E)
		b = (peek(FLAG) & 1) != 0;
		fl::logic((uint16_t)(0x0001 & peek((uint16_t)(T.r[6] + 0x16))));
		tick(0x467E, 33);                              // BIT #1,0x16(SP)
		if (b)
		{
			tick(0x4680, 12);
			T.r[0] = 0x0035;
			fl::mov(T.r[0]);
			tick(0x4684, 18);
			tick(0x468A, 12);                          // BR
		}
		else
		{
			tick(0x4686, 12);                          // BEQ
			T.r[0] = 0x0036;
			fl::mov(T.r[0]);
			tick(0x468A, 18);
		}
		jsr(0x468E, 0x8AB6);
		b = peekb(0x01F6) != 0;
		fl::tstb(peekb(0x01F6));
		tick(0x4692, 24);                              // TSTB @#0x01F6
		if (b)
		{
			tick(0x4694, 12);
			poke(T.r[6], 0x78D2);
			fl::mov(0x78D2);
			tick(0x4698, 27);
			jsr(0x469C, 0xA9FC);
		}
		else
			tick(0x469C, 12);                          // BEQ
		poke(LAST, a.u(0));
		fl::mov(peek((uint16_t)(T.r[6] + 0x14)));
		tick(0x46A2, 45);
		poke(LAST, fl::add(peek(LAST), 0x000E));
		tick(0x46A8, 36);
		T.r[4] = a.u(0);
		fl::mov(T.r[4]);
		tick(0x46AC, 27);
		T.r[4] = fl::dec(T.r[4]);
		tick(0x46AE, 12);
		tick(0x4756, 12);                              // BR
		// the table: rank, initials (spaces for none) and score of each entry
		for (;;)
		{
			T.r[4] = fl::inc(T.r[4]);
			tick(0x4758, 12);
			b = (int16_t)T.r[4] <= (int16_t)peek(LAST);
			fl::cmp(T.r[4], peek((uint16_t)(T.r[6] + 0x14)));
			tick(0x475C, 27);                          // CMP R4,0x14(SP)
			if (!b)
			{
				tick(0x475E, 12);                      // BLE
				break;
			}
			tick(0x46B0, 12);
			fl::cmp(T.r[4], T.r[2]);
			tick(0x46B2, 12);                          // CMP R4,R2
			if (T.r[4] == T.r[2])
			{
				tick(0x46B4, 12);                      // BNE
				poke(0x0188, 0x4000);
				fl::mov(0x4000);
				tick(0x46BA, 33);
				tick(0x46C2, 12);                      // BR
			}
			else
			{
				tick(0x46BC, 12);
				poke(0x0188, 0xE000);
				fl::mov(0xE000);
				tick(0x46C2, 33);
			}
			T.r[1] = T.r[4];
			fl::mov(T.r[1]);
			tick(0x46C4, 12);
			T.r[1] = fl::sub(T.r[1], a.u(0));
			tick(0x46C8, 27);
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0x46CA, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x46CC, 12);
			poke(0x0186, peek((uint16_t)(0x7FB8 + T.r[0])));
			fl::mov(peek(0x0186));
			tick(0x46D2, 42);
			poke(0x0186, fl::add(peek(0x0186), 0x1FF8));
			tick(0x46D8, 33);
			poke(T.r[6], T.r[1]);
			fl::mov(T.r[1]);
			tick(0x46DA, 21);
			poke(T.r[6], fl::inc(peek(T.r[6])));
			tick(0x46DC, 21);
			push(0);
			fl::clr();
			tick(0x46DE, 24);
			push(0x0002);
			fl::mov(0x0002);
			tick(0x46E2, 30);
			push(0);
			fl::clr();
			tick(0x46E4, 24);
			push(0);
			fl::clr();
			tick(0x46E6, 24);
			jsr(0x46EA, 0xBA9A);                       // the rank
			fl::cmp(T.r[1], 0x0008);
			tick(0x46EE, 18);                          // CMP R1,#8
			if ((int16_t)T.r[1] >= 8)
			{
				tick(0x46F0, 12);                      // BLT
				poke(0x0186, fl::add(peek(0x0186), 4));
				tick(0x46F6, 33);
			}
			else
				tick(0x46F6, 12);
			fl::cmp(T.r[4], T.r[2]);
			tick(0x46F8, 12);                          // CMP R4,R2
			if (T.r[4] == T.r[2])
			{
				tick(0x4728, 12);                      // BEQ
				poke(0x0186, fl::add(peek(0x0186), 0x000C));
				tick(0x472E, 33);
			}
			else
			{
				tick(0x46FA, 12);
				T.r[0] = T.r[4];
				fl::mov(T.r[0]);
				tick(0x46FC, 12);
				for (uint16_t pc = 0x46FE; pc <= 0x4702; pc = (uint16_t)(pc + 2))
				{
					T.r[0] = (uint16_t)(T.r[0] << 1);
					tick(pc, 12);
				}
				T.r[1] = T.r[0];
				tick(0x4704, 12);
				T.r[3] = 0x0004;
				fl::mov(T.r[3]);
				tick(0x4708, 18);
				for (;;)
				{
					T.r[0] = T.r[3];
					fl::mov(T.r[0]);
					tick(0x470A, 12);
					T.r[0] = fl::add(T.r[0], T.r[1]);
					tick(0x470C, 12);
					T.r[5] = sext8(peekb((uint16_t)(0x0370 + T.r[0])));
					fl::movb((uint8_t)T.r[5]);
					tick(0x4710, 27);
					if (!T.r[5])
					{
						tick(0x4712, 12);              // BNE
						T.r[5] = 0x0020;               // MOVB #0x20,R5
						fl::movb((uint8_t)T.r[5]);
						tick(0x4716, 18);
					}
					else
						tick(0x4716, 12);
					T.r[0] = 0;
					fl::clr();
					tick(0x4718, 12);
					T.r[0] = (uint8_t)T.r[5];
					fl::logicb((uint8_t)T.r[0]);
					tick(0x471A, 12);
					jsr(0x471E, 0x8B52);               // draw_large_char
					T.r[3] = fl::inc(T.r[3]);
					tick(0x4720, 12);
					fl::cmp(T.r[3], 0x0006);
					tick(0x4724, 18);                  // CMP R3,#6
					if ((int16_t)T.r[3] > 6)
					{
						tick(0x4726, 12);              // BLE
						break;
					}
					tick(0x4708, 12);
				}
				tick(0x472E, 12);                      // BR
			}
			poke(0x0186, fl::add(peek(0x0186), 4));
			tick(0x4734, 33);
			T.r[0] = T.r[4];
			fl::mov(T.r[0]);
			tick(0x4736, 12);
			for (uint16_t pc = 0x4738; pc <= 0x473C; pc = (uint16_t)(pc + 2))
			{
				T.r[0] = (uint16_t)(T.r[0] << 1);
				tick(pc, 12);
			}
			poke(T.r[6], peek((uint16_t)(0x0372 + T.r[0])));
			tick(0x4740, 36);
			push(peek((uint16_t)(0x0370 + T.r[0])));
			fl::mov(peek(T.r[6]));
			tick(0x4744, 39);
			push(0x0006);
			fl::mov(0x0006);
			tick(0x4748, 30);
			push(0x0001);
			fl::mov(0x0001);
			tick(0x474C, 30);
			push(peek(T.r[6]));
			fl::mov(peek(T.r[6]));
			tick(0x474E, 30);
			jsr(0x4752, 0xBA9A);                       // the score
			T.r[6] = fl::add(T.r[6], 0x0010);
			tick(0x4756, 18);
		}
		poke(0x0188, 0x4000);
		tick(0x4764, 33);
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0x4766, 12);
		for (uint16_t pc = 0x4768; pc <= 0x476C; pc = (uint16_t)(pc + 2))
		{
			T.r[0] = (uint16_t)(T.r[0] << 1);
			tick(pc, 12);
		}
		poke(E8, T.r[0]);
		tick(0x4770, 30);
		poke(REL, T.r[2]);
		fl::mov(T.r[2]);
		tick(0x4774, 30);
		poke(REL, fl::sub(peek(REL), a.u(0)));
		tick(0x477A, 45);
		T.r[0] = peek(REL);
		fl::mov(T.r[0]);
		tick(0x477E, 27);
		T.r[0] = fl::asl(T.r[0]);
		tick(0x4780, 12);
		poke(ROW, 0x7FB8);
		fl::mov(0x7FB8);
		tick(0x4786, 36);
		poke(ROW, fl::add(peek(ROW), T.r[0]));
		tick(0x478A, 30);
		b = peek(0x0208) == 0xFFFF;
		fl::cmp(peek(0x0208), 0xFFFF);
		tick(0x4790, 30);                              // CMP @#0x0208,#-1
		bool enter = b;
		if (!b)
		{
			tick(0x4792, 12);                          // BEQ
			enter = (peek(FLAG) & 1) != 0;
			fl::logic((uint16_t)(0x0001 & peek((uint16_t)(T.r[6] + 0x16))));
			tick(0x4798, 33);                          // BIT #1,0x16(SP)
			tick(enter ? 0x479E : 0x479A, 12);         // BNE
		}
		else
			tick(0x479E, 12);
		if (enter)
		{
			// the initials are entered
			T.r[0] = 0;
			fl::clr();
			tick(0x47A0, 12);
			T.r[0] = peekb(0x01F5);
			fl::logicb((uint8_t)T.r[0]);
			tick(0x47A4, 24);
			b = peekb((uint16_t)(0x0F3A + T.r[0])) != 0;
			fl::tstb(peekb((uint16_t)(T.r[0] + 0x0F3A)));
			tick(0x47A8, 27);                          // TSTB 0xF3A(R0)
			if (!b)
			{
				tick(0x47AA, 12);                      // BNE
				poke(T.r[6], 0x0045);
				fl::mov(0x0045);
				tick(0x47AE, 27);
				jsr(0x47B2, 0x8E78);                   // queue_sound
			}
			else
				tick(0x47B2, 12);
			poke(T.r[6], 0x78DC);
			fl::mov(0x78DC);
			tick(0x47B6, 27);
			jsr(0x47BA, 0xA9FC);
			poke(T.r[6], 0x78C8);
			fl::mov(0x78C8);
			tick(0x47BE, 27);
			jsr(0x47C2, 0xA9FC);
			poke(0x0188, 0x4000);
			fl::mov(0x4000);
			tick(0x47C8, 33);
			T.r[0] = peek(E8);
			fl::mov(T.r[0]);
			tick(0x47CC, 27);
			pokeb((uint16_t)(0x0374 + T.r[0]), 0x20);
			fl::movb(0x20);
			tick(0x47D2, 36);
			pokeb((uint16_t)(0x0375 + T.r[0]), 0x20);
			fl::movb(0x20);
			tick(0x47D8, 36);
			pokeb((uint16_t)(0x0376 + T.r[0]), 0x20);
			fl::movb(0x20);
			tick(0x47DE, 36);
			poke(CNT, 0);
			fl::clr();
			tick(0x47E2, 30);
			pokeb(LET, 0x41);
			fl::movb(0x41);
			tick(0x47E8, 36);
			pokeb(SEC, 0x1E);
			fl::movb(0x1E);
			tick(0x47EE, 36);
			jsr(0x47F2, 0x8D86);                       // read_controller
			jsr(0x47F6, 0x8E2E);
			for (;;)
			{
				b = (int16_t)peek(CNT) < 3;
				fl::cmp(peek((uint16_t)(T.r[6] + 0x0C)), 0x0003);
				tick(0x47FC, 33);                      // CMP 0xC(SP),#3
				if (!b)
				{
					tick(0x47FE, 12);                  // BLT
					break;
				}
				tick(0x4802, 12);
				b = (peekb(SEC) & 0x80) != 0;
				fl::tstb(peekb((uint16_t)(T.r[6] + 0x0E)));
				tick(0x4806, 27);                      // TSTB 0xE(SP)
				if (b)
				{
					tick(0x47FE, 12);                  // BLT: time is up
					break;
				}
				tick(0x4808, 12);
				jsr(0x480C, 0x8D50);                   // wait_vblank
				b = (peekb(0x01C4) & 0x3F) != 0;
				fl::logicb((uint8_t)(0x3F & peekb(0x01C4)));
				tick(0x4812, 30);                      // BITB #0x3F,@#0x01C4
				if (!b)
				{
					tick(0x4814, 12);                  // BNE: a second has passed
					T.r[0] = sext8(peekb(SEC));
					fl::movb((uint8_t)T.r[0]);
					tick(0x4818, 27);
					T.r[0] = fl::dec(T.r[0]);
					tick(0x481A, 12);
					pokeb(SEC, (uint8_t)T.r[0]);
					fl::movb((uint8_t)T.r[0]);
					tick(0x481E, 30);
					fl::cmpb((uint8_t)T.r[0], 0x0A);
					tick(0x4822, 18);                  // CMPB R0,#10
					if ((int8_t)T.r[0] < 10)
					{
						tick(0x4824, 12);              // BGE
						fl::tstb((uint8_t)T.r[0]);
						tick(0x4826, 12);              // TSTB R0
						if (!((int8_t)T.r[0] < 0))
						{
							tick(0x4828, 12);          // BLT
							poke(0x0188, 0xE000);
							fl::mov(0xE000);
							tick(0x482E, 33);
							fl::cmpb((uint8_t)T.r[0], 0x09);
							tick(0x4832, 18);          // CMPB R0,#9
							if ((uint8_t)T.r[0] == 9)
							{
								tick(0x4834, 12);      // BNE
								poke(T.r[6], 0x78E6);
								fl::mov(0x78E6);
								tick(0x4838, 27);
								push(0);
								fl::clr();
								tick(0x483A, 24);
								jsr(0x483E, 0x4D30);   // message_box(draw)
								T.r[6] = (uint16_t)(T.r[6] + 2);
								fl::tst(peek((uint16_t)(T.r[6] - 2)));
								tick(0x4840, 18);      // TST (SP)+
							}
							else
								tick(0x4840, 12);
							poke(0x0186, 0x20F0);
							fl::mov(0x20F0);
							tick(0x4846, 33);
							T.r[0] = sext8(peekb(SEC));
							fl::movb((uint8_t)T.r[0]);
							tick(0x484A, 27);
							T.r[0] = fl::add(T.r[0], 0x0030);
							tick(0x484E, 18);
							jsr(0x4852, 0x8C56);       // draw_big_char: the seconds
							poke(0x0188, 0x4000);
							fl::mov(0x4000);
							tick(0x4858, 33);
						}
						else
							tick(0x4858, 12);
					}
					else
						tick(0x4858, 12);
				}
				else
					tick(0x4858, 12);
				jsr(0x485C, 0x8D86);                   // read_controller
				jsr(0x4860, 0x8E2E);
				T.r[1] = 0;
				fl::clr();
				tick(0x4862, 12);
				T.r[1] = peekb(LET);
				fl::logicb((uint8_t)T.r[1]);
				tick(0x4866, 27);
				T.r[0] = fl::add(T.r[0], T.r[1]);
				tick(0x4868, 12);
				pokeb(LET, (uint8_t)T.r[0]);
				fl::movb((uint8_t)T.r[0]);
				tick(0x486C, 30);
				fl::cmpb((uint8_t)T.r[0], 0x3F);
				tick(0x4870, 18);                      // CMPB R0,#0x3F
				if ((uint8_t)T.r[0] < 0x3F)
				{
					tick(0x4872, 12);                  // BCC
					pokeb(LET, 0x5A);
					fl::movb(0x5A);
					tick(0x4878, 36);
				}
				else
					tick(0x4878, 12);
				b = peekb(LET) > 0x5A;
				fl::cmpb(peekb((uint16_t)(T.r[6] + 0x0A)), 0x5A);
				tick(0x487E, 33);                      // CMPB 0xA(SP),#0x5A
				if (b)
				{
					tick(0x4880, 12);                  // BLOS
					pokeb(LET, 0x3F);
					fl::movb(0x3F);
					tick(0x4886, 36);
				}
				else
					tick(0x4886, 12);
				T.r[1] = peek(CNT);
				fl::mov(T.r[1]);
				tick(0x488A, 27);
				T.r[1] = fl::asl(T.r[1]);
				tick(0x488C, 12);
				T.r[1] = fl::asl(T.r[1]);
				tick(0x488E, 12);
				T.r[0] = T.r[1];
				fl::mov(T.r[0]);
				tick(0x4890, 12);
				T.r[0] = fl::add(T.r[0], peek(peek(ROW)));
				tick(0x4894, 33);
				poke(0x0186, T.r[0]);
				fl::mov(T.r[0]);
				tick(0x4898, 27);
				poke(0x0186, fl::add(peek(0x0186), 0x2000));
				tick(0x489E, 33);
				b = (int16_t)peek(REL) >= 8;
				fl::cmp(peek((uint16_t)(T.r[6] + 0x10)), 0x0008);
				tick(0x48A4, 33);                      // CMP 0x10(SP),#8
				if (b)
				{
					tick(0x48A6, 12);                  // BLT
					poke(0x0186, fl::add(peek(0x0186), 4));
					tick(0x48AC, 33);
				}
				else
					tick(0x48AC, 12);
				T.r[2] = 0;
				fl::clr();
				tick(0x48AE, 12);
				b = peekb(LET) < 0x41;
				fl::cmpb(peekb((uint16_t)(T.r[6] + 0x0A)), 0x41);
				tick(0x48B4, 33);                      // CMPB 0xA(SP),#0x41
				if (b)
				{
					tick(0x48B6, 12);                  // BCC
					T.r[2] = fl::inc(T.r[2]);
					tick(0x48B8, 12);
					T.r[0] = 0;
					fl::clr();
					tick(0x48BA, 12);
					T.r[0] = peekb(LET);
					fl::logicb((uint8_t)T.r[0]);
					tick(0x48BE, 27);
					T.r[0] = fl::sub(T.r[0], 0x0020);
					tick(0x48C2, 18);
					tick(0x48CA, 12);                  // BR
				}
				else
				{
					tick(0x48C4, 12);
					T.r[0] = 0;
					fl::clr();
					tick(0x48C6, 12);
					T.r[0] = peekb(LET);
					fl::logicb((uint8_t)T.r[0]);
					tick(0x48CA, 27);
				}
				jsr(0x48CE, 0x8B52);                   // draw_large_char: the letter
				b = (peekb(0x0193) & 0x80) != 0;
				fl::tstb(peekb(0x0193));
				tick(0x48D2, 24);                      // TSTB @#0x0193
				if (!b)
				{
					tick(0x495A, 12);                  // BPL: no button
					pokeb(0x0190, 0);
					fl::clr();
					tick(0x495E, 27);
				}
				else
				{
					tick(0x48D4, 12);
					b = peekb(0x0190) != 0;
					fl::tstb(peekb(0x0190));
					tick(0x48D8, 24);                  // TSTB @#0x0190
					if (b)
					{
						tick(0x47F6, 12);              // BNE: still held
						continue;
					}
					tick(0x48DA, 12);
					const uint8_t letter = peekb(LET);
					const bool carry = letter < 0x3F;
					fl::cmpb(peekb((uint16_t)(T.r[6] + 0x0A)), 0x3F);
					tick(0x48E0, 33);                  // CMPB 0xA(SP),#0x3F
					if (letter == 0x3F)
					{
						// '?': delete the last letter
						tick(0x48E2, 12);              // BNE
						b = (int16_t)peek(CNT) > 0;
						fl::tst(peek((uint16_t)(T.r[6] + 0x0C)));
						tick(0x48E6, 27);              // TST 0xC(SP)
						if (b)
						{
							tick(0x48E8, 12);          // BLE
							pokeb(LET, 0x20);
							fl::movb(0x20);
							tick(0x48EE, 36);
							T.r[0] = T.r[1];
							fl::mov(T.r[0]);
							tick(0x48F0, 12);
							T.r[0] = fl::add(T.r[0], peek(peek(ROW)));
							tick(0x48F4, 33);
							poke(0x0186, T.r[0]);
							fl::mov(T.r[0]);
							tick(0x48F8, 27);
							poke(0x0186, fl::add(peek(0x0186), 0x2000));
							tick(0x48FE, 33);
							T.r[0] = peek(LAST);
							fl::mov(T.r[0]);
							tick(0x4902, 27);
							T.r[0] = fl::sub(T.r[0], a.u(0));
							tick(0x4906, 27);
							fl::cmp(T.r[0], 0x0008);
							tick(0x490A, 18);          // CMP R0,#8
							if ((int16_t)T.r[0] >= 8)
							{
								tick(0x490C, 12);      // BLT
								poke(0x0186, fl::add(peek(0x0186), 4));
								tick(0x4912, 33);
							}
							else
								tick(0x4912, 12);
							T.r[0] = 0;
							fl::clr();
							tick(0x4914, 12);
							T.r[0] = peekb(LET);
							fl::logicb((uint8_t)T.r[0]);
							tick(0x4918, 27);
							jsr(0x491C, 0x8B52);       // draw_large_char: a space
							T.r[0] = peek(E8);
							fl::mov(T.r[0]);
							tick(0x4920, 27);
							T.r[0] = fl::add(T.r[0], peek(CNT));
							tick(0x4924, 27);
							pokeb((uint16_t)(0x0374 + T.r[0]), peekb(LET));
							fl::movb(peekb((uint16_t)(T.r[6] + 0x0A)));
							tick(0x492A, 45);
							poke(CNT, fl::dec(peek(CNT)));
							tick(0x492E, 30);
							tick(0x494C, 12);          // BR
						}
						else
							tick(0x494C, 12);          // BLE
					}
					else
					{
						tick(0x4930, 12);              // BNE
						const bool space = (T.r[2] & 1) != 0;
						T.r[2] = (uint16_t)(((carry ? 1u : 0u) << 15) | (T.r[2] >> 1));
						tick(0x4932, 12);              // ROR R2
						if (space)
						{
							tick(0x4934, 12);          // BCC
							pokeb(LET, 0x20);
							fl::movb(0x20);
							tick(0x493A, 36);
						}
						else
							tick(0x493A, 12);
						T.r[0] = peek(E8);
						fl::mov(T.r[0]);
						tick(0x493E, 27);
						T.r[0] = fl::add(T.r[0], peek(CNT));
						tick(0x4942, 27);
						pokeb((uint16_t)(0x0374 + T.r[0]), peekb(LET));
						fl::movb(peekb((uint16_t)(T.r[6] + 0x0A)));
						tick(0x4948, 45);
						poke(CNT, fl::inc(peek(CNT)));
						tick(0x494C, 30);
					}
					pokeb(LET, 0x41);
					fl::movb(0x41);
					tick(0x4952, 36);
					pokeb(0x0190, 1);
					fl::movb(0x01);
					tick(0x4958, 33);
					tick(0x495E, 12);                  // BR
				}
				tick(0x47F6, 18);                      // JMP
			}
			tick(0x49B0, 18);                          // JMP
		}
		else
		{
			// shown again: the initials of the entry 0x0208 are copied
			tick(0x4962, 18);                          // JMP
			poke(0x0186, peek(peek(ROW)));
			fl::mov(peek(0x0186));
			tick(0x4968, 48);
			poke(0x0186, fl::add(peek(0x0186), 0x2000));
			tick(0x496E, 33);
			b = (int16_t)peek(REL) >= 8;
			fl::cmp(peek((uint16_t)(T.r[6] + 0x10)), 0x0008);
			tick(0x4974, 33);                          // CMP 0x10(SP),#8
			if (b)
			{
				tick(0x4976, 12);                      // BLT
				poke(0x0186, fl::add(peek(0x0186), 4));
				tick(0x497C, 33);
			}
			else
				tick(0x497C, 12);
			T.r[1] = 0;
			fl::clr();
			tick(0x497E, 12);
			for (;;)
			{
				T.r[2] = T.r[1];
				fl::mov(T.r[2]);
				tick(0x4980, 12);
				T.r[2] = fl::add(T.r[2], peek(E8));
				tick(0x4984, 27);
				T.r[0] = peek(0x0208);
				fl::mov(T.r[0]);
				tick(0x4988, 24);
				for (uint16_t pc = 0x498A; pc <= 0x498E; pc = (uint16_t)(pc + 2))
				{
					T.r[0] = (uint16_t)(T.r[0] << 1);
					tick(pc, 12);
				}
				T.r[0] = (uint16_t)(T.r[0] + T.r[1]);
				tick(0x4990, 12);
				T.r[0] = sext8(peekb((uint16_t)(0x0374 + T.r[0])));
				fl::movb((uint8_t)T.r[0]);
				tick(0x4994, 27);
				T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
				tick(0x4998, 18);
				pokeb((uint16_t)(0x0374 + T.r[2]), (uint8_t)T.r[0]);
				fl::movb((uint8_t)T.r[0]);
				tick(0x499C, 30);
				jsr(0x49A0, 0x8B52);                   // draw_large_char
				T.r[1] = fl::inc(T.r[1]);
				tick(0x49A2, 12);
				fl::cmp(T.r[1], 0x0002);
				tick(0x49A6, 18);                      // CMP R1,#2
				if ((int16_t)T.r[1] > 2)
				{
					tick(0x49A8, 12);                  // BLE
					break;
				}
				tick(0x497E, 12);
			}
			poke(T.r[6], 0x012C);
			tick(0x49AC, 27);
			jsr(0x49B0, 0x8D74);                       // wait_frames(300)
		}
		// the entry to the sound CPU
		jsr(0x49B4, 0x8E6A);                           // set_flag_01a4
		b = (peek(FLAG) & 1) != 0;
		fl::logic((uint16_t)(0x0001 & peek((uint16_t)(T.r[6] + 0x16))));
		tick(0x49BA, 33);                              // BIT #1,0x16(SP)
		if (b)
		{
			tick(0x49BC, 12);
			poke(T.r[6], 0x000F);
			fl::mov(0x000F);
			tick(0x49C0, 27);
			tick(0x49C6, 12);                          // BR
		}
		else
		{
			tick(0x49C2, 12);                          // BEQ
			poke(T.r[6], 0x0010);
			fl::mov(0x0010);
			tick(0x49C6, 27);
		}
		jsr(0x49CA, 0x8EA2);                           // send_sound_command
		jsr(0x49CE, 0x8F50);
		// one byte of the entry at 0x0370 + k: CLR (SP); MOV 8(SP),R0; MOVB k(R0),(SP)
		auto entry_byte = [&](uint16_t k, uint16_t pc) {
			poke(T.r[6], 0);
			tick((uint16_t)(pc + 2), 21);
			T.r[0] = peek(E8);
			tick((uint16_t)(pc + 6), 27);
			pokeb(T.r[6], peekb((uint16_t)(0x0370 + k + T.r[0])));
			tick((uint16_t)(pc + 10), 36);
		};
		b = (peek(FLAG) & 1) != 0;
		tick(0x49D4, 33);                              // BIT #1,0x16(SP)
		if (b)
		{
			tick(0x49D6, 12);
			entry_byte(0, 0x49D6);
			jsr(0x49E4, 0x8EA2);
			entry_byte(3, 0x49E4);
			jsr(0x49F2, 0x8EA2);
			entry_byte(2, 0x49F2);
			tick(0x4A16, 12);                          // BR
		}
		else
		{
			tick(0x49FE, 12);                          // BEQ
			entry_byte(2, 0x49FE);
			jsr(0x4A0C, 0x8EA2);
			entry_byte(3, 0x4A0C);
		}
		jsr(0x4A1A, 0x8EA2);
		entry_byte(4, 0x4A1A);
		jsr(0x4A28, 0x8EA2);
		entry_byte(5, 0x4A28);
		jsr(0x4A36, 0x8EA2);
		entry_byte(6, 0x4A36);
		jsr(0x4A44, 0x8EA2);
		jsr(0x4A48, 0x8F62);
		jsr(0x4A4C, 0x8E72);                           // clear_flag_01a4
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(peek((uint16_t)(T.r[6] - 2)));
		tick(0x4A4E, 18);                              // TST (SP)+
	}
	poke(0x0184, 0x5AC8);
	fl::mov(0x5AC8);
	tick(0x4A54, 33);
	T.r[0] = 0x3C00;
	fl::mov(T.r[0]);
	tick(0x4A58, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x4A5C, 27);
	poke(0x1402, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x4A60, 27);
	T.r[6] = fl::add(T.r[6], 0x0016);
	tick(0x4A64, 18);
	c_restore(4);
	return T.r[0];
}

// A7:5BB8 (assembly)  game_over_states(): the game states (0x018F) after a game; the code
// continues in bank B3 (0x6000 on), where states 0/1 (B3:62E6) and the others (B3:6062,
// B3:601E) are handled; those are reached with tail jumps, the stack as the original has it.
//   8: game over: with tickets left for a continue (0x027B against 0xFE9E by the coin mode)
//      the continue screen starts (F:E106, F:820C, A7:435A, sounds 0x70/0x76/0x53, texts
//      0x7708/0x5F06/0x76FC, B3:79D4; 0x0F1E = 60, state 9), else state 5.
//   9: the continue countdown: credits and price texts (F:8AB6, F:BA9A), each second
//      (0x0F1C) F:E78E asks for the button; pressed: tickets 0x027B + 1, time (F:F028),
//      B3:72CA, and when the hiscore goal 0x024C/0x024E is passed a new goal from 0x5212
//      (0x025A), interrupts F:81AA, state 0; time up: state 5.
//   5: the end: texts, 2 + 2 seconds, the cash 0x0256 counted into the score (F:EF24 by 10
//      and 100, B3:77B4) frame by frame, B3:6E92, the results sent to the sound CPU
//      (0x0E, parks 0x0F2E/0x0F2F), the leading player (F:E730), then for each player the
//      high score tables (A7:4476, A7:45B0 for the score and for the parks 0x0242), and
//      0x0F18 = -10 into B3:6000.
// Registers end as the B3 code leaves them.
void game_over_states()
{
	// The original jumps into the B3 code with its local word (and two argument words) still
	// above the return address; the B3 code pops them and returns to our caller.  tail_jump()
	// expects the return address on top of the stack, so the jump is made here the same way:
	// the B3 code runs from addr and returns to the caller's address; the return address is
	// then pushed back (the same word) for the return that ends this native routine.
	const uint16_t E = T.r[6];
	const uint16_t L = (uint16_t)(E - 2);              // the local word
	auto jump = [&](uint16_t addr) {
		const uint16_t ret = peek(E);
		T.r[7] = addr;
		shadow_push(ret);
		dispatch();
		shadow_pop();
		if (T.r[7] != ret) throw Unwound{};
		push(ret);
	};
	T.r[6] = L;
	fl::tst(peek(T.r[6]));
	tick(0x5BBA, 21);                                  // TST -(SP)
	T.r[0] = 0;
	fl::clr();
	tick(0x5BBC, 12);
	T.r[0] = peekb(0x018F);
	fl::logicb((uint8_t)T.r[0]);
	tick(0x5BC0, 24);
	fl::tst(T.r[0]);
	tick(0x5BC2, 12);                                  // TST R0
	if (T.r[0])
	{
		tick(0x5BC4, 12);                              // BEQ
		fl::cmp(T.r[0], 0x0001);
		tick(0x5BC8, 18);                              // CMP R0,#1
	}
	else
		tick(0x5BC8, 12);
	if (T.r[0] == 0 || T.r[0] == 1)
	{
		tick(0x5BCA, 12);                              // BNE
		tick(0x62E6, 18);                              // JMP: the game
		jump(0x62E6);
		return;
	}
	tick(0x5BCE, 12);
	bool b = peek(0x0F22) != 0;
	fl::tst(peek(0x0F22));
	tick(0x5BD2, 24);                                  // TST @#0x0F22
	if (b)
		tick(0x5C66, 12);                              // BNE
	else
	{
		tick(0x5BD4, 12);
		fl::cmp(T.r[0], 0x0008);
		tick(0x5BD8, 18);                              // CMP R0,#8
		if (T.r[0] != 8)
			tick(0x5C66, 12);                          // BNE
		else
		{
			// game over: the continue screen, or the end
			tick(0x5BDA, 12);
			jsr(0x5BDE, 0xA8FE);
			T.r[0] = 0;
			fl::clr();
			tick(0x5BE0, 12);
			T.r[0] = peekb(0x01F3);
			fl::logicb((uint8_t)T.r[0]);
			tick(0x5BE4, 24);
			for (uint16_t pc = 0x5BE6; pc <= 0x5BEC; pc = (uint16_t)(pc + 2))
			{
				T.r[0] = asr(T.r[0], 1);
				tick(pc, 12);
			}
			T.r[0] &= 0x0003;
			tick(0x5BF0, 18);
			b = peekb(0x027B) > rb((uint16_t)(0xFE9E + T.r[0]));
			fl::cmpb(peekb(0x027B), peekb((uint16_t)(T.r[0] + 0xFE9E)));
			tick(0x5BF6, 39);                          // CMPB @#0x027B,0xFE9E(R0)
			if (b)
			{
				tick(0x5C60, 12);                      // BHI: no continue
				pokeb(0x018F, 5);
				fl::movb(0x05);
				tick(0x5C66, 33);
			}
			else
			{
				tick(0x5BF8, 12);
				jsr(0x5BFC, 0xE106);
				jsr(0x5C00, 0x820C);                   // install_interrupts_b
				jsr(0x5C04, 0x435A);                   // read_switch_settings
				poke(0x01B4, 0);
				fl::clr();
				tick(0x5C08, 27);
				push(0x0070);
				fl::mov(0x0070);
				tick(0x5C0C, 30);
				jsr(0x5C10, 0x8E78);                   // queue_sound
				poke(T.r[6], 0x0076);
				fl::mov(0x0076);
				tick(0x5C14, 27);
				jsr(0x5C18, 0x8E78);
				poke(T.r[6], 0x0053);
				fl::mov(0x0053);
				tick(0x5C1C, 27);
				jsr(0x5C20, 0x8E78);
				poke(T.r[6], 0x7708);
				fl::mov(0x7708);
				tick(0x5C24, 27);
				jsr(0x5C28, 0xA9FC);
				poke(T.r[6], 0x5F06);
				fl::mov(0x5F06);
				tick(0x5C2C, 27);
				push(0x0001);
				fl::mov(0x0001);
				tick(0x5C30, 30);
				jsr(0x5C34, 0xEAD4);                   // draw_tile_block
				poke(0x0F1C, 0x000A);
				fl::mov(0x000A);
				tick(0x5C3A, 33);
				poke(T.r[6], 0x76FC);
				fl::mov(0x76FC);
				tick(0x5C3E, 27);
				push(0);
				fl::clr();
				tick(0x5C40, 24);
				jsr(0x5C44, 0x4D30);                   // message_box(draw)
				pokeb(0x0FA2, fl::logicb((uint8_t)(peekb(0x0FA2) | 0x10)));
				tick(0x5C4A, 33);
				jsr(0x5C4E, 0x79D4);
				pokeb(0x0F1E, 0x3C);
				fl::movb(0x3C);
				tick(0x5C54, 33);
				pokeb(0x018F, 9);
				fl::movb(0x09);
				tick(0x5C5A, 33);
				T.r[6] = fl::add(T.r[6], 6);
				tick(0x5C5E, 18);
				tick(0x5C66, 12);                      // BR
			}
		}
	}
	b = peekb(0x018F) == 9;
	fl::cmpb(peekb(0x018F), 0x09);
	tick(0x5C6C, 30);                                  // CMPB @#0x018F,#9
	if (!b)
	{
		tick(0x5C6E, 12);                              // BEQ
		tick(0x5E7C, 18);                              // JMP
	}
	else
	{
		// the continue countdown
		tick(0x5C72, 12);
		poke(0x0188, 0xE000);
		fl::mov(0xE000);
		tick(0x5C78, 33);
		b = peekb(0x0F1E) == 0x3C;
		fl::cmpb(peekb(0x0F1E), 0x3C);
		tick(0x5C7E, 30);                              // CMPB @#0x0F1E,#60
		if (!b)
			tick(0x5D7C, 12);                          // BNE
		else
		{
			tick(0x5C80, 12);
			b = (peek(0x0F1C) & 1) != 0;
			fl::logic((uint16_t)(0x0001 & peek(0x0F1C)));
			tick(0x5C86, 30);                          // BIT #1,@#0x0F1C
			if (b)
			{
				tick(0x5C88, 12);                      // BEQ
				T.r[0] = 0;
				fl::clr();
				tick(0x5C8A, 12);
				T.r[0] = peekb(0x01F3);
				fl::logicb((uint8_t)T.r[0]);
				tick(0x5C8E, 24);
				T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00C0));
				tick(0x5C92, 18);
				fl::cmp(T.r[0], 0x0080);
				tick(0x5C96, 18);                      // CMP R0,#0x80
				if (T.r[0] == 0x0080)
				{
					tick(0x5C98, 12);                  // BNE
					T.r[0] = 0x006C;
					fl::mov(T.r[0]);
					tick(0x5C9C, 18);
					tick(0x5CA2, 12);                  // BR
				}
				else
				{
					tick(0x5C9E, 12);
					T.r[0] = 0x006B;
					fl::mov(T.r[0]);
					tick(0x5CA2, 18);
				}
				jsr(0x5CA6, 0x8AB6);
				tick(0x5D7C, 12);                      // BR
			}
			else
			{
				tick(0x5CA8, 12);                      // BEQ
				jsr(0x5CAC, 0x435A);                   // read_switch_settings
				poke(L, 0);
				fl::clr();
				tick(0x5CAE, 21);
				pokeb(L, peekb(0x01F9));
				fl::movb(peekb(T.r[6]));
				tick(0x5CB2, 33);
				poke(L, asr(peek(L), 1));
				tick(0x5CB4, 21);
				poke(L, asr(peek(L), 1));
				tick(0x5CB6, 21);
				T.r[0] = 0x0066;
				fl::mov(T.r[0]);
				tick(0x5CBA, 18);
				jsr(0x5CBE, 0x8AB6);
				poke(0x0186, fl::sub(peek(0x0186), 0x002C));
				tick(0x5CC4, 33);
				push(peek(T.r[6]));
				fl::mov(peek(T.r[6]));
				tick(0x5CC6, 30);
				push(0);
				fl::clr();
				tick(0x5CC8, 24);
				push(0x0002);
				fl::mov(0x0002);
				tick(0x5CCC, 30);
				push(0x0001);
				fl::mov(0x0001);
				tick(0x5CD0, 30);
				push(0);
				fl::clr();
				tick(0x5CD2, 24);
				jsr(0x5CD6, 0xBA9A);                   // the credits
				b = (peekb(0x01F9) & 3) != 0;
				fl::logicb((uint8_t)(0x03 & peekb(0x01F9)));
				tick(0x5CDC, 30);                      // BITB #3,@#0x01F9
				if (b)
				{
					tick(0x5CDE, 12);                  // BEQ
					T.r[0] = sext8(peekb(0x01FA));
					fl::movb((uint8_t)T.r[0]);
					tick(0x5CE2, 24);
					T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0003));
					tick(0x5CE6, 18);
					pokeb(L, peekb((uint16_t)(0x5A49 + T.r[0])));
					fl::movb(peekb((uint16_t)(T.r[6] + 0x0A)));
					tick(0x5CEC, 45);
					pokeb((uint16_t)(L + 1), 0);
					fl::clr();
					tick(0x5CF0, 30);
					T.r[0] = sext8(peekb(0x01F9));
					fl::movb((uint8_t)T.r[0]);
					tick(0x5CF4, 24);
					T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0003));
					tick(0x5CF8, 18);
					T.r[0] = fl::asl(T.r[0]);
					tick(0x5CFA, 12);
					T.r[0] = fl::add(T.r[0], peek(L));
					tick(0x5CFE, 27);
					poke(L, T.r[0]);
					fl::mov(T.r[0]);
					tick(0x5D02, 30);
					poke(L, fl::sub(peek(L), 2));
					tick(0x5D08, 36);
					T.r[0] = peek(L);
					fl::mov(T.r[0]);
					tick(0x5D0C, 27);
					T.r[0] = fl::asl(T.r[0]);
					tick(0x5D0E, 12);
					T.r[1] = peek((uint16_t)(0x5A66 + T.r[0]));
					fl::mov(T.r[1]);
					tick(0x5D12, 27);
					T.r[1] = fl::logic((uint16_t)(T.r[1] | peek(0x0188)));
					tick(0x5D16, 24);
					poke(peek(0x0186), T.r[1]);
					fl::mov(T.r[1]);
					tick(0x5D1A, 36);
					T.r[0] = peek(0x0186);
					fl::mov(T.r[0]);
					tick(0x5D1E, 24);
					T.r[1] = peek(L);
					fl::mov(T.r[1]);
					tick(0x5D22, 27);
					T.r[1] = fl::asl(T.r[1]);
					tick(0x5D24, 12);
					T.r[2] = peek((uint16_t)(0x5A68 + T.r[1]));
					fl::mov(T.r[2]);
					tick(0x5D28, 27);
					T.r[2] = fl::logic((uint16_t)(T.r[2] | peek(0x0188)));
					tick(0x5D2C, 24);
					poke((uint16_t)(T.r[0] + 0x0080), T.r[2]);
					fl::mov(T.r[2]);
					tick(0x5D30, 30);
				}
				else
					tick(0x5D30, 12);
				b = peekb(0x01F9) != 0;
				fl::tstb(peekb(0x01F9));
				tick(0x5D34, 24);                      // TSTB @#0x01F9
				bool few = false;
				if (b)
				{
					tick(0x5D36, 12);                  // BEQ
					few = peekb(0x01F9) <= 4;
					fl::cmpb(peekb(0x01F9), 0x04);
					tick(0x5D3C, 30);                  // CMPB @#0x01F9,#4
					tick(few ? 0x5D3E : 0x5D44, 12);   // BHI
				}
				else
					tick(0x5D44, 12);
				if (few)
				{
					T.r[0] = 0x0069;
					tick(0x5D42, 18);
					tick(0x5D48, 12);                  // BR
				}
				else
				{
					T.r[0] = 0x0067;
					tick(0x5D48, 18);
				}
				jsr(0x5D4C, 0x8AB6);
				b = (int16_t)peek(L) > 0;
				fl::tst(peek((uint16_t)(T.r[6] + 0x0A)));
				tick(0x5D50, 27);                      // TST 0xA(SP)
				bool enough;
				if (b)
				{
					tick(0x5D52, 12);                  // BLE
					T.r[0] = 0;
					fl::clr();
					tick(0x5D54, 12);
					T.r[0] = peekb(0x01F3);
					fl::logicb((uint8_t)T.r[0]);
					tick(0x5D58, 24);
					T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0080));
					tick(0x5D5C, 18);
					fl::cmp(T.r[0], 0x012C);
					tick(0x5D60, 18);                  // CMP R0,#0x012C
					enough = T.r[0] != 0x012C;
					if (enough)
						tick(0x5D6A, 12);              // BNE
					else
						tick(0x5D62, 12);
				}
				else
				{
					tick(0x5D62, 12);                  // BLE
					enough = false;
				}
				if (!enough)
				{
					enough = !((int16_t)peek(L) < 2);
					fl::cmp(peek((uint16_t)(T.r[6] + 0x0A)), 0x0002);
					tick(0x5D68, 33);                  // CMP 0xA(SP),#2
					tick(enough ? 0x5D6A : 0x5D70, 12); // BLT
				}
				if (enough)
				{
					T.r[0] = 0x006A;
					tick(0x5D6E, 18);
					tick(0x5D74, 12);                  // BR
				}
				else
				{
					T.r[0] = 0x0068;
					tick(0x5D74, 18);
				}
				jsr(0x5D78, 0x8AB6);
				T.r[6] = fl::add(T.r[6], 0x000A);
				tick(0x5D7C, 18);
			}
		}
		b = peekb(0x0F1E) != 0;
		fl::tstb(peekb(0x0F1E));
		tick(0x5D80, 24);                              // TSTB @#0x0F1E
		if (!b)
		{
			// time up
			tick(0x5D82, 12);                          // BNE
			push(0x7708);
			fl::mov(0x7708);
			tick(0x5D86, 30);
			push(0x0001);
			fl::mov(0x0001);
			tick(0x5D8A, 30);
			jsr(0x5D8E, 0x4D30);                       // message_box(erase)
			poke(0x0F1C, 0);
			fl::clr();
			tick(0x5D92, 27);
			pokeb(0x0FA2, fl::logicb((uint8_t)(peekb(0x0FA2) | 0x10)));
			tick(0x5D98, 33);
			jsr(0x5D9C, 0x79D4);
			pokeb(0x018F, 5);
			fl::movb(0x05);
			tick(0x5DA2, 33);
			T.r[6] = (uint16_t)(T.r[6] + 4);
			fl::cmp(peek((uint16_t)(T.r[6] - 4)), peek((uint16_t)(T.r[6] - 2)));
			tick(0x5DA4, 24);                          // CMP (SP)+,(SP)+
			tick(0x5E7C, 12);                          // BR
		}
		else
		{
			tick(0x5DA6, 12);
			b = (int16_t)peek(0x0F1C) > 9;
			fl::cmp(peek(0x0F1C), 0x0009);
			tick(0x5DAC, 30);                          // CMP @#0x0F1C,#9
			if (b)
				tick(0x5E7C, 12);                      // BGT
			else
			{
				tick(0x5DAE, 12);
				jsr(0x5DB2, 0xE78E);                   // query_on_button_press
				fl::tst(T.r[0]);
				tick(0x5DB4, 12);                      // TST R0
				if (!T.r[0])
					tick(0x5E7C, 12);                  // BEQ
				else
				{
					// continue
					tick(0x5DB6, 12);
					poke(0x0F1C, 0);
					fl::clr();
					tick(0x5DBA, 27);
					pokeb(0x0F1E, 0);
					fl::clr();
					tick(0x5DBE, 27);
					pokeb(0x0FA2, fl::logicb((uint8_t)(peekb(0x0FA2) & ~0x10)));
					tick(0x5DC4, 33);
					pokeb(0x027B, fl::incb(peekb(0x027B)));
					tick(0x5DC8, 27);
					push(0x7708);
					fl::mov(0x7708);
					tick(0x5DCC, 30);
					push(0x0001);
					fl::mov(0x0001);
					tick(0x5DD0, 30);
					jsr(0x5DD4, 0x4D30);               // message_box(erase)
					poke(T.r[6], 0x76FC);
					fl::mov(0x76FC);
					tick(0x5DD8, 27);
					push(0x0001);
					fl::mov(0x0001);
					tick(0x5DDC, 30);
					jsr(0x5DE0, 0x4D30);
					poke(0x0188, 0x4000);
					fl::mov(0x4000);
					tick(0x5DE6, 33);
					poke(T.r[6], 0x5DEE);
					fl::mov(0x5DEE);
					tick(0x5DEA, 27);
					push(0);
					fl::clr();
					tick(0x5DEC, 24);
					jsr(0x5DF0, 0xEAD4);               // draw_tile_block
					poke(T.r[6], 0);
					fl::clr();
					tick(0x5DF2, 21);
					jsr(0x5DF6, 0xF028);               // add_time(0)
					b = peekb(0x025B) != 0;
					fl::tstb(peekb(0x025B));
					tick(0x5DFA, 24);                  // TSTB @#0x025B
					if (b)
						tick(0x5E70, 12);              // BNE
					else
					{
						tick(0x5DFC, 12);
						poke(T.r[6], 0x0001);
						fl::mov(0x0001);
						tick(0x5E00, 27);
						jsr(0x5E04, 0x72CA);
						T.r[1] = 0x024E;
						fl::mov(T.r[1]);
						tick(0x5E08, 18);
						T.r[2] = 0x024C;
						fl::mov(T.r[2]);
						tick(0x5E0C, 18);
						T.r[3] = peek(0x0240);
						fl::mov(T.r[3]);
						tick(0x5E10, 24);
						T.r[4] = peek(0x023E);
						fl::mov(T.r[4]);
						tick(0x5E14, 24);
						jsr(0x5E18, 0x80D0);           // sub32: the score left to the goal
						T.r[0] = peek(0x024E);
						fl::mov(T.r[0]);
						tick(0x5E1C, 24);
						bool passed;
						if (T.r[0] & 0x8000)
						{
							tick(0x5E26, 12);          // BLT
							passed = true;
						}
						else
						{
							tick(0x5E1E, 12);
							if (T.r[0])
							{
								tick(0x5E70, 12);      // BNE
								passed = false;
							}
							else
							{
								tick(0x5E20, 12);
								passed = !peek(0x024C);
								fl::tst(peek(0x024C));
								tick(0x5E24, 24);      // TST @#0x024C
								tick(passed ? 0x5E26 : 0x5E70, 12);   // BNE
							}
						}
						if (passed)
						{
							poke(0x024C, peek(0x0244));
							tick(0x5E2C, 39);
							poke(0x024E, peek(0x0246));
							fl::mov(peek(0x024E));
							tick(0x5E32, 39);
							pokeb(0x0FA2, fl::logicb((uint8_t)(peekb(0x0FA2) | 0x02)));
							tick(0x5E38, 33);
							const bool below = peekb(0x025A) < 4;
							fl::cmpb(peekb(0x025A), 0x04);
							tick(0x5E3E, 30);          // CMPB @#0x025A,#4
							pokeb(0x025A, (uint8_t)(peekb(0x025A) + (below ? 1 : 0)));
							tick(0x5E42, 27);          // ADCB @#0x025A
							T.r[0] = 0;
							fl::clr();
							tick(0x5E44, 12);
							T.r[0] = peekb(0x025A);
							fl::logicb((uint8_t)T.r[0]);
							tick(0x5E48, 24);
							T.r[1] = 0;
							fl::clr();
							tick(0x5E4A, 12);
							T.r[1] = peekb(0x0FE3);
							fl::logicb((uint8_t)T.r[1]);
							tick(0x5E4E, 24);
							T.r[0] = fl::add(T.r[0], T.r[1]);
							tick(0x5E50, 12);
							T.r[0] = fl::asl(T.r[0]);
							tick(0x5E52, 12);
							T.r[1] = 0x0246;
							fl::mov(T.r[1]);
							tick(0x5E56, 18);
							T.r[2] = 0x0244;
							fl::mov(T.r[2]);
							tick(0x5E5A, 18);
							T.r[3] = 0;
							fl::clr();
							tick(0x5E5C, 12);
							T.r[4] = peek((uint16_t)(0x5212 + T.r[0]));
							fl::mov(T.r[4]);
							tick(0x5E60, 27);
							jsr(0x5E64, 0x80C8);       // add32: the next goal
							poke(0x024A, peek(0x0246));
							fl::mov(peek(0x024A));
							tick(0x5E6A, 39);
							poke(0x0248, peek(0x0244));
							fl::mov(peek(0x0248));
							tick(0x5E70, 39);
						}
					}
					jsr(0x5E74, 0x81AA);               // install_interrupts_a
					pokeb(0x018F, 0);
					fl::clr();
					tick(0x5E78, 27);
					T.r[6] = fl::add(T.r[6], 8);
					tick(0x5E7C, 18);
				}
			}
		}
	}
	b = peekb(0x018F) == 5;
	fl::cmpb(peekb(0x018F), 0x05);
	tick(0x5E82, 30);                                  // CMPB @#0x018F,#5
	if (!b)
	{
		tick(0x5E84, 12);                              // BEQ
		tick(0x6062, 18);                              // JMP
		jump(0x6062);
		return;
	}
	// the end of the game
	tick(0x5E88, 12);
	push(0x7708);
	fl::mov(0x7708);
	tick(0x5E8C, 30);
	push(0x0001);
	fl::mov(0x0001);
	tick(0x5E90, 30);
	jsr(0x5E94, 0x4D30);                               // message_box(erase)
	poke(T.r[6], 0x0005);
	fl::mov(0x0005);
	tick(0x5E98, 27);
	jsr(0x5E9C, 0x8E78);                               // queue_sound
	jsr(0x5EA0, 0x820C);                               // install_interrupts_b
	poke(T.r[6], 0x76E0);
	fl::mov(0x76E0);
	tick(0x5EA4, 27);
	jsr(0x5EA8, 0xA9FC);
	b = peekb(0x018E) != 0;
	fl::tstb(peekb(0x018E));
	tick(0x5EAC, 24);                                  // TSTB @#0x018E
	if (b)
		tick(0x5ECE, 12);                              // BNE
	else
	{
		tick(0x5EAE, 12);
		T.r[0] = 0;
		fl::clr();
		tick(0x5EB0, 12);
		T.r[0] = peekb(0x01F3);
		fl::logicb((uint8_t)T.r[0]);
		tick(0x5EB4, 24);
		T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0030));
		tick(0x5EB8, 18);
		fl::cmp(T.r[0], 0x0010);
		tick(0x5EBC, 18);                              // CMP R0,#0x10
		if (T.r[0] == 0x0010)
			tick(0x5ECE, 12);                          // BEQ
		else
		{
			tick(0x5EBE, 12);
			poke(T.r[6], 0x78EC);
			fl::mov(0x78EC);
			tick(0x5EC2, 27);
			jsr(0x5EC6, 0xA9FC);
			poke(T.r[6], 0x0078);
			fl::mov(0x0078);
			tick(0x5ECA, 27);
			jsr(0x5ECE, 0x8D74);                       // wait 120 frames
		}
	}
	poke(T.r[6], 0x0078);
	fl::mov(0x0078);
	tick(0x5ED2, 27);
	jsr(0x5ED6, 0x8D74);                               // wait 120 frames
	b = (int16_t)peek(0x0256) > 0;
	fl::tst(peek(0x0256));
	tick(0x5EDA, 24);                                  // TST @#0x0256
	if (!b)
		tick(0x5F36, 12);                              // BLE
	else
	{
		// the cash into the score
		tick(0x5EDC, 12);
		poke(T.r[6], 0x78EC);
		fl::mov(0x78EC);
		tick(0x5EE0, 27);
		push(0x0001);
		fl::mov(0x0001);
		tick(0x5EE4, 30);
		jsr(0x5EE8, 0x4D30);                           // message_box(erase)
		poke(T.r[6], 0x76E8);
		fl::mov(0x76E8);
		tick(0x5EEC, 27);
		jsr(0x5EF0, 0xA9FC);
		for (;;)
		{
			b = peek(0x0256) != 0;
			fl::tst(peek(0x0256));
			tick(0x5EF4, 24);                          // TST @#0x0256
			if (!b)
			{
				tick(0x5F2C, 12);                      // BEQ
				break;
			}
			tick(0x5EF6, 12);
			poke(0x0256, fl::dec(peek(0x0256)));
			tick(0x5EFA, 27);
			poke(T.r[6], 0x000A);
			fl::mov(0x000A);
			tick(0x5EFE, 27);
			jsr(0x5F02, 0xEF24);                       // add_score
			b = (int16_t)peek(0x0256) > 0x000A;
			fl::cmp(peek(0x0256), 0x000A);
			tick(0x5F08, 30);                          // CMP @#0x0256,#10
			if (b)
			{
				tick(0x5F0A, 12);                      // BLE
				poke(0x0256, fl::sub(peek(0x0256), 0x000A));
				tick(0x5F10, 33);
				poke(T.r[6], 0x0064);
				fl::mov(0x0064);
				tick(0x5F14, 27);
				jsr(0x5F18, 0xEF24);
			}
			else
				tick(0x5F18, 12);
			pokeb(0x0FA2, 9);
			fl::movb(0x09);
			tick(0x5F1E, 33);
			jsr(0x5F22, 0x77B4);                       // update_status_display
			poke(T.r[6], 0x0001);
			fl::mov(0x0001);
			tick(0x5F26, 27);
			jsr(0x5F2A, 0x8D74);                       // wait a frame
			tick(0x5EF0, 12);                          // BR
		}
		poke(T.r[6], 0x003C);
		tick(0x5F30, 27);
		jsr(0x5F34, 0x8D74);                           // wait 60 frames
		T.r[6] = (uint16_t)(T.r[6] + 2);
		fl::tst(peek((uint16_t)(T.r[6] - 2)));
		tick(0x5F36, 18);                              // TST (SP)+
	}
	jsr(0x5F3A, 0x6E92);
	b = peekb(0x01F6) != 0;
	fl::tstb(peekb(0x01F6));
	tick(0x5F3E, 24);                                  // TSTB @#0x01F6
	if (b)
	{
		tick(0x5F40, 12);                              // BEQ
		b = peekb(0x0F2B) == 4;
		fl::cmpb(peekb(0x0F2B), 0x04);
		tick(0x5F46, 30);                              // CMPB @#0x0F2B,#4
		if (!b)
		{
			tick(0x601E, 12);                          // BNE
			jump(0x601E);
			return;
		}
		tick(0x5F48, 12);
	}
	else
		tick(0x5F48, 12);                              // BEQ
	// the results to the sound CPU
	jsr(0x5F4C, 0x8E6A);                               // set_flag_01a4
	poke(T.r[6], 0x000E);
	fl::mov(0x000E);
	tick(0x5F50, 27);
	jsr(0x5F54, 0x8EA2);                               // send_sound_command
	jsr(0x5F58, 0x8F50);
	for (uint16_t pc = 0x5F58; pc <= 0x5F74; pc = (uint16_t)(pc + 0x1C))
	{
		// the parks of player 1 / 2 (0x0F2E / 0x0F2F), the other's second
		const bool second = peekb(0x01F5) != 0;
		tick((uint16_t)(pc + 4), 24);                  // TSTB @#0x01F5
		const uint16_t first = pc == 0x5F58 ? 0x0F2E : 0x0F2F;
		const uint16_t other = pc == 0x5F58 ? 0x0F2F : 0x0F2E;
		if (second)
		{
			tick((uint16_t)(pc + 0x0E), 12);           // BNE
			T.r[0] = 0;
			tick((uint16_t)(pc + 0x10), 12);
			T.r[0] = peekb(other);
			tick((uint16_t)(pc + 0x14), 24);
		}
		else
		{
			tick((uint16_t)(pc + 6), 12);
			T.r[0] = 0;
			tick((uint16_t)(pc + 8), 12);
			T.r[0] = peekb(first);
			tick((uint16_t)(pc + 0x0C), 24);
			tick((uint16_t)(pc + 0x14), 12);           // BR
		}
		poke(T.r[6], T.r[0]);
		tick((uint16_t)(pc + 0x16), 21);
		poke(T.r[6], (uint16_t)(peek(T.r[6]) - 1));
		tick((uint16_t)(pc + 0x18), 21);
		jsr((uint16_t)(pc + 0x1C), 0x8EA2);
	}
	jsr(0x5F94, 0x8F62);
	jsr(0x5F98, 0x8E72);                               // clear_flag_01a4
	pokeb(0x018E, 1);
	fl::movb(0x01);
	tick(0x5F9E, 33);
	b = peekb(0x01F6) != 0;
	fl::tstb(peekb(0x01F6));
	tick(0x5FA2, 24);                                  // TSTB @#0x01F6
	if (b)
	{
		// the leading player
		tick(0x5FA4, 12);                              // BEQ
		bool second_leads = (int16_t)peek(0x027E) > (int16_t)peek(0x0240);
		const bool same = peek(0x027E) == peek(0x0240);
		fl::cmp(peek(0x027E), peek(0x0240));
		tick(0x5FAA, 36);                              // CMP @#0x027E,@#0x0240
		if (second_leads)
			tick(0x5FB6, 12);                          // BGT
		else
		{
			tick(0x5FAC, 12);
			if (!same)
				tick(0x5FBA, 12);                      // BNE
			else
			{
				tick(0x5FAE, 12);
				second_leads = (int16_t)peek(0x027C) > (int16_t)peek(0x023E);
				fl::cmp(peek(0x027C), peek(0x023E));
				tick(0x5FB4, 36);                      // CMP @#0x027C,@#0x023E
				tick(second_leads ? 0x5FB6 : 0x5FBA, 12);   // BLE
			}
		}
		if (second_leads)
			jsr(0x5FBA, 0xE730);
	}
	else
		tick(0x5FBA, 12);                              // BEQ
	// the high score tables of each player
	T.r[1] = 0;
	fl::clr();
	tick(0x5FBC, 12);
	T.r[1] = peekb(0x01F6);
	fl::logicb((uint8_t)T.r[1]);
	tick(0x5FC0, 24);
	fl::tst(T.r[1]);
	tick(0x5FC2, 12);                                  // TST R1
	if (T.r[1] & 0x8000)
		tick(0x5FF6, 12);                              // BLT
	else
	{
		tick(0x5FC4, 12);
		for (;;)
		{
			jsr(0x5FC8, 0x4476);                       // read_high_scores
			poke(T.r[6], peek(0x0240));
			fl::mov(peek(T.r[6]));
			tick(0x5FCC, 33);
			push(peek(0x023E));
			fl::mov(peek(T.r[6]));
			tick(0x5FD0, 36);
			push(0);
			fl::clr();
			tick(0x5FD2, 24);
			jsr(0x5FD6, 0x45B0);                       // enter_high_score(0, score)
			poke(T.r[6], 0);
			fl::clr();
			tick(0x5FD8, 21);
			push(peek(0x0242));
			fl::mov(peek(T.r[6]));
			tick(0x5FDC, 36);
			push(0x000F);
			fl::mov(0x000F);
			tick(0x5FE0, 30);
			jsr(0x5FE4, 0x45B0);                       // enter_high_score(15, parks)
			b = peekb(0x01F6) != 0;
			fl::tstb(peekb(0x01F6));
			tick(0x5FE8, 24);                          // TSTB @#0x01F6
			if (b)
			{
				tick(0x5FEA, 12);
				jsr(0x5FEE, 0xE730);                   // the other player
			}
			else
				tick(0x5FEE, 12);                      // BEQ
			T.r[6] = fl::add(T.r[6], 8);
			tick(0x5FF2, 18);
			T.r[1] = fl::dec(T.r[1]);
			tick(0x5FF4, 12);
			if (T.r[1] & 0x8000)
			{
				tick(0x5FF6, 12);                      // BGE
				break;
			}
			tick(0x5FC4, 12);
		}
	}
	poke(0x0F18, 0xFFF6);
	tick(0x5FFC, 33);
	pokeb(0x0F2B, 0);
	fl::clr();
	tick(0x6000, 27);                                  // into the B3 code
	jump(0x6000);
}

}

LOGIC_TABLE(banka,
	c_routine("A5", 0x438C, "step_map_column", step_map_column, 0, 0),
	asm_routine("A5", 0x42BC, "step_map_row", step_map_row, 0),
	asm_routine("A5", 0x41CE, "draw_map_strip", draw_map_strip, 0),
	c_routine("A5", 0x4000, "init_map_view", init_map_view, 0, 0),
	asm_routine("A5", 0x48CC, "update_map_slots", update_map_slots, 0),
	c_routine("A5", 0x5D08, "stop_effect_sound", stop_effect_sound, 0, 0),
	c_routine("A5", 0x5B46, "rolling_sound", rolling_sound, 0, 0),
	wait_routine("A5", 0x5BD2, "town_music", town_music, 0),
	c_routine("A5", 0x4984, "draw_text_block_3x3", draw_text_block_3x3, 0, 0),
	c_routine("A5", 0x51EE, "slope_height", slope_height, 0, 0),
	asm_routine("A7", 0x4098, "move_objects_0_3", move_objects_0_3, 0),
	c_routine("A7", 0x410C, "project_speed", project_speed, 0, 0),
	c_routine("A7", 0x4F98, "direction_from_object7", direction_from_object7, 0, 0),
	c_routine("A7", 0x4ECC, "hold_object", hold_object, 0, 0),
	c_routine("A7", 0x4D30, "message_box", message_box, 0, 0),
	asm_routine("A7", 0x4B84, "expire_messages", expire_messages, 0),
	asm_routine("A7", 0x4C28, "show_next_message", show_next_message, 0),
	c_routine("A7", 0x5A7E, "credit_messages", credit_messages, 0, 0),
	c_routine("A7", 0x5018, "chaser_step", chaser_step, 0, 0),
	c_routine("A7", 0x5248, "shop_purchase", shop_purchase, 0, 0),
	c_routine("A7", 0x43C0, "insert_high_score", insert_high_score, 0, 0),
	c_routine("A7", 0x5684, "find_collision", find_collision, 0, 0),
	asm_routine("A7", 0x50F8, "fallen_skater", fallen_skater, 0),
	wait_c_routine("A7", 0x4B3E, "poll_sound_status", poll_sound_status, 0),
	asm_routine("A5", 0x454A, "scan_zone_switches", scan_zone_switches, 0),
	c_routine("A7", 0x538E, "terrain_event", terrain_event, 0, 0),
	asm_routine("A7", 0x572A, "shapes_touch", shapes_touch, 0),
	wait_c_routine("A7", 0x4B00, "reset_sound_cpu", reset_sound_cpu, 0),
	wait_c_routine("A7", 0x435A, "read_switch_settings", read_switch_settings, 0),
	wait_c_routine("A7", 0x4476, "read_high_scores", read_high_scores, 0),
	wait_c_routine("A7", 0x4516, "show_high_scores", show_high_scores, 0),
	wait_routine("A7", 0x419A, "start_game", start_game, 0),
	c_routine("A5", 0x5844, "slope_event", slope_event, 0, 0),
	asm_routine("A5", 0x58D0, "downhill_zone", downhill_zone, 0),
	asm_routine("A5", 0x5A50, "downhill_height", downhill_height, 0),
	c_routine("A5", 0x5388, "trick_facing", trick_facing, 0, 0),
	c_routine("A5", 0x543E, "next_trick_frame", next_trick_frame, 0, 0),
	c_routine("A5", 0x53A8, "trick_landing", trick_landing, 0, 0),
	c_routine("A5", 0x5462, "trick_step", trick_step, 0, 0),
	c_routine("A5", 0x5264, "start_trick", start_trick, 0, 0),
	c_routine("A5", 0x54CE, "trick_moves", trick_moves, 0, 0),
	c_routine("A7", 0x4A66, "velocity_direction", velocity_direction, 0, 0),
	asm_routine("A5", 0x4A2A, "skater_physics", skater_physics, 0),
	wait_c_routine("A7", 0x45B0, "enter_high_score", enter_high_score, 0),
	wait_routine("A7", 0x5BB8, "game_over_states", game_over_states, 0),
)

}
