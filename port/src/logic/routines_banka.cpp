// Rewritten routines of the banked segments A5 and A7: the Skate City map view, object
// motion, sounds driven by the game state, and the message boxes of the text layer.
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

// cycles of the original's instructions on the path taken (tools/insn_cycles.py), so that each
// call costs exactly what the original's does; the table charge is only the common epilogue
inline void cyc(int n) { T.icount -= n; }
// the same for a block of the original starting at pc, letting interrupts in there as the
// original would within the block (once per block)
inline void blk(uint16_t pc, int n) { tick(pc, n); }
// select bank B (shadow 0x01D4 and the hardware register)
void set_bank_b(uint16_t v)
{
	poke(0x01D4, v);
	poke(0x1402, v);
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
	tick(0x4390, 27);                                  // TST 2(SP)
	if (a.s(0) > 0)
	{
		tick(0x4392, 12);
		T.r[0] = 0;
		tick(0x4394, 12);
		T.r[0] = peekb(0x0F44);
		tick(0x4398, 24);
		T.r[0] = (uint16_t)(T.r[0] + 0x0020);
		tick(0x439C, 18);
		pokeb(0x0F44, (uint8_t)T.r[0]);
		tick(0x43A0, 27);
		poke(0x0F46, (uint16_t)(peek(0x0F46) + 2));
		tick(0x43A6, 33);
		T.r[0] = 0;
		tick(0x43A8, 12);
		T.r[0] = peekb(0x0F44);
		tick(0x43AC, 24);
		T.r[0] = (uint16_t)(T.r[0] + 0x00A0);
		tick(0x43B0, 18);
		pokeb(0x0F45, (uint8_t)T.r[0]);
		tick(0x43B4, 27);
		poke(0x0F48, peek(0x0F46));
		tick(0x43BA, 39);
		poke(0x0F48, (uint16_t)(peek(0x0F48) + 0x000A));
		tick(0x43C0, 33);
		tick(0x43E0, 12);                              // BR
	}
	else
	{
		tick(0x43C2, 12);                              // BLE
		T.r[0] = 0;
		tick(0x43C4, 12);
		T.r[0] = peekb(0x0F44);
		tick(0x43C8, 24);
		T.r[0] = (uint16_t)(T.r[0] - 0x0020);
		tick(0x43CC, 18);
		pokeb(0x0F44, (uint8_t)T.r[0]);
		tick(0x43D0, 27);
		poke(0x0F46, (uint16_t)(peek(0x0F46) - 2));
		tick(0x43D6, 33);
		pokeb(0x0F45, (uint8_t)T.r[0]);
		tick(0x43DA, 27);
		poke(0x0F48, peek(0x0F46));
		tick(0x43E0, 39);
	}
	poke(0x0F42, peek(0x0F40));
	tick(0x43E6, 39);
	pokeb(0x0F5D, 8);
	tick(0x43EC, 33);
	pokeb(0x0F5C, 0);
	tick(0x43F0, 27);
	poke(0x0F4A, 0xF800);
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
	tick(0x42C0, 27);                                  // TST 2(SP)
	if (dir > 0)
	{
		tick(0x42C2, 12);
		poke(0x0F40, (uint16_t)(peek(0x0F40) - 0x0800));
		tick(0x42C8, 33);
		T.r[1] = 0;
		tick(0x42CA, 12);
		T.r[1] = peekb(0x0F5A);
		tick(0x42CE, 24);
		const bool odd = (peek(0x0F40) & 0x0800) != 0;
		tick(0x42D4, 30);
		if (odd)
		{
			tick(0x42D6, 12);
			poke(0x0F46, (uint16_t)(peek(0x0F46) - T.r[1]));
			tick(0x42DA, 27);
			T.r[0] = sext8(peekb(0x0F5B));
			tick(0x42DE, 24);
			T.r[0]--;
			tick(0x42E0, 12);
			pokeb(0x0F5B, (uint8_t)T.r[0]);
			tick(0x42E4, 27);
			if ((int8_t)T.r[0] < 0)                    // BGE on the byte stored
			{
				tick(0x42E6, 12);
				pokeb(0x0F5B, 0x17);
				tick(0x42EC, 33);
				T.r[0] = peek(0x0F46);
				tick(0x42F0, 24);
				T.r[0] = (uint16_t)(T.r[0] - 0x0180);
				tick(0x42F4, 18);
				poke(0x0F46, 0x0A6C);
				tick(0x42FA, 33);
				poke(0x0F46, (uint16_t)(peek(0x0F46) + T.r[0]));
				tick(0x42FE, 27);
			}
			else
				tick(0x42FE, 12);
			T.r[0] = T.r[1];
			tick(0x4300, 12);
			T.r[0] = (uint16_t)(T.r[0] << 1);
			tick(0x4302, 12);
			T.r[0] = (uint16_t)(T.r[0] + T.r[1]);
			tick(0x4304, 12);
			tick(0x430C, 12);                          // BR
		}
		else
		{
			tick(0x4306, 12);                          // BEQ
			T.r[0] = T.r[1];
			tick(0x4308, 12);
			T.r[0] = (uint16_t)(T.r[0] << 1);
			tick(0x430A, 12);
			T.r[0] = (uint16_t)(T.r[0] << 1);
			tick(0x430C, 12);
		}
		poke(0x0F48, peek(0x0F46));
		tick(0x4312, 39);
		poke(0x0F48, (uint16_t)(peek(0x0F48) - T.r[0]));
		tick(0x4316, 27);
		poke(0x0F42, peek(0x0F40));
		tick(0x431C, 39);
		poke(0x0F42, (uint16_t)(peek(0x0F42) - 0x3800));
		tick(0x4322, 33);
		tick(0x4374, 12);                              // BR
	}
	else
	{
		tick(0x4324, 12);                              // BLE
		poke(0x0F40, (uint16_t)(peek(0x0F40) + 0x0800));
		tick(0x432A, 33);
		const bool odd = (peek(0x0F40) & 0x0800) != 0;
		tick(0x4330, 30);
		if (!odd)
		{
			tick(0x4332, 12);
			T.r[0] = 0;
			tick(0x4334, 12);
			T.r[0] = peekb(0x0F5A);
			tick(0x4338, 24);
			poke(0x0F46, (uint16_t)(peek(0x0F46) + T.r[0]));
			tick(0x433C, 27);
			T.r[0] = sext8(peekb(0x0F5B));
			tick(0x4340, 24);
			T.r[0]++;
			tick(0x4342, 12);
			pokeb(0x0F5B, (uint8_t)T.r[0]);
			tick(0x4346, 27);
			T.r[0] = peek(0x099E);
			tick(0x434A, 24);
			T.r[0] = (uint16_t)(T.r[0] << 1);
			tick(0x434C, 12);
			T.r[1] = sext8(peekb(0x0F5B));
			tick(0x4350, 24);
			tick(0x4352, 12);                          // CMP R1,R0
			if (T.r[1] == T.r[0])
			{
				tick(0x4354, 12);
				pokeb(0x0F5B, 0);
				tick(0x4358, 27);
				T.r[0] = peek(0x0F46);
				tick(0x435C, 24);
				T.r[0] = (uint16_t)(T.r[0] - 0x0A80);
				tick(0x4360, 18);
				T.r[0] = (uint16_t)(T.r[0] + 0x0194);
				tick(0x4364, 18);
				poke(0x0F46, T.r[0]);
				tick(0x4368, 27);
			}
			else
				tick(0x4368, 12);                      // BNE
		}
		else
			tick(0x4368, 12);                          // BNE
		poke(0x0F42, peek(0x0F40));
		tick(0x436E, 39);
		poke(0x0F48, peek(0x0F46));
		tick(0x4374, 39);
	}
	pokeb(0x0F45, peekb(0x0F44));
	tick(0x437A, 39);
	pokeb(0x0F5C, 0x20);
	tick(0x4380, 33);
	poke(0x0F4A, 0);
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
	tick(0x41D0, 24);
	poke(local, peekb(0x0F5D));                        // MOVB (sign extension is not done
	tick(0x41D4, 33);                                  // for a memory destination)
	for (;;)
	{
		tick(0x41D6, 18);                              // TST (SP)
		if (!peek(local))
		{
			tick(0x4254, 12);                          // BEQ
			break;
		}
		tick(0x41D8, 12);
		do
		{
			pokeb(0x0F5D, (uint8_t)(peekb(0x0F5D) - 1));
			tick(0x41DC, 27);
			poke(0x0F42, peek(0x0F42) & ~0xC001);
			tick(0x41E2, 33);
			jsr(0x41E6, 0xE82E);                       // unpack_map_block
			poke(0x0F42, (uint16_t)(peek(0x0F42) + peek(0x0F4A)));
			tick(0x41EC, 39);
			T.r[0] = peek(0x0F42);
			tick(0x41F0, 24);
			T.r[1] = peek(0x0F4A);
			tick(0x41F4, 24);
			T.r[1] = (uint16_t)~T.r[1];
			tick(0x41F6, 12);
			T.r[0] &= (uint16_t)~T.r[1];
			tick(0x41F8, 12);
			tick(0x41FC, 18);                          // BIT #0x0800,R0
			if (T.r[0] & 0x0800)
			{
				tick(0x41FE, 12);
				T.r[0] = 0;
				tick(0x4200, 12);
				T.r[0] = peekb(0x0F5A);
				tick(0x4204, 24);
				poke(0x0F48, (uint16_t)(peek(0x0F48) - T.r[0]));
				tick(0x4208, 27);
				poke(0x0F56, (uint16_t)(peek(0x0F56) - 6));
				tick(0x420E, 33);
			}
			else
				tick(0x420E, 12);
			T.r[0] = 0;
			tick(0x4210, 12);
			T.r[0] = peekb(0x0F45);
			tick(0x4214, 24);
			T.r[1] = sext8(peekb(0x0F5C));
			tick(0x4218, 24);
			T.r[0] = (uint16_t)(T.r[0] + T.r[1]);
			tick(0x421A, 12);
			pokeb(0x0F45, (uint8_t)T.r[0]);
			tick(0x421E, 27);
			tick(0x4220, 12);                          // TSTB R1
			if ((uint8_t)T.r[1])
			{
				tick(0x4222, 12);
				poke(0x0F48, (uint16_t)(peek(0x0F48) + 2));
				tick(0x4228, 33);
				T.r[0] = 0;
				tick(0x422A, 12);
				T.r[0] = peekb(0x0F58);
				tick(0x422E, 24);
				tick(0x4230, 12);                      // TST R0
				if (T.r[0])
				{
					tick(0x4232, 12);
					T.r[1] = T.r[0];
					tick(0x4234, 12);
					T.r[1] = (uint16_t)(T.r[1] << 1);
					tick(0x4236, 12);
					pokeb(0x0F58, (uint8_t)T.r[1]);
					tick(0x423A, 27);
					if (!(uint8_t)T.r[1])
					{
						tick(0x423C, 12);
						pokeb(0x0F58, (uint8_t)(peekb(0x0F58) + 1));
						tick(0x4240, 27);
						poke(0x0F56, (uint16_t)(peek(0x0F56) + 1));
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
			tick(0x4246, 21);
			poke(local, peekb(0x0F5D));
			tick(0x424A, 33);
			tick(0x424C, 18);                          // TST (SP)
			const bool more = peek(local) != 0;
			tick(more ? 0x41D8 : 0x424E, 12);          // BNE
			if (!more) break;
		} while (true);
		pokeb(0x0F5E, (uint8_t)(peekb(0x0F5E) + 1));
		tick(0x4252, 27);
		tick(0x41D4, 12);                              // BR 41D4
	}
	T.r[0] = peek(0x01AA);
	tick(0x4258, 24);
	T.r[0] = (uint16_t)(T.r[0] + 0x0800);
	tick(0x425C, 18);
	T.r[0] &= (uint16_t)~0x0FFF;
	tick(0x4260, 18);
	poke(local, peek(0x0F3E));
	tick(0x4264, 33);
	poke(local, (uint16_t)(peek(local) - T.r[0]));
	tick(0x4266, 21);
	if (peek(local))
	{
		tick(0x4268, 12);
		push(peek(local));                             // MOV (SP),-(SP): the argument
		tick(0x426A, 30);
		jsr(0x426E, 0x42BC);                           // step_map_row
		tick(0x4272, 27);                              // TST 2(SP)
		if ((int16_t)peek(local) >= 0)
		{
			tick(0x427C, 12);
			poke(0x0F3E, (uint16_t)(peek(0x0F3E) - 0x1000));
			tick(0x4282, 33);
		}
		else
		{
			tick(0x4274, 12);
			poke(0x0F3E, (uint16_t)(peek(0x0F3E) + 0x1000));
			tick(0x427A, 33);
		}
		tick(0x42B2, 12);                              // BR 42B2
	}
	else
	{
		tick(0x4284, 12);
		T.r[0] = peek(0x01A8);
		tick(0x4288, 24);
		T.r[0] = (uint16_t)(T.r[0] + 0x1000);
		tick(0x428C, 18);
		T.r[0] &= (uint16_t)~0x1FFF;
		tick(0x4290, 18);
		T.r[0] = (uint16_t)(T.r[0] - peek(0x0F3C));
		tick(0x4294, 24);
		poke(local, T.r[0]);
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
		tick(0x429A, 24);
		jsr(0x429E, 0x438C);                           // step_map_column
		tick(0x42A2, 27);                              // TST 2(SP)
		if ((int16_t)peek(local) > 0)
		{
			tick(0x42A4, 12);
			poke(0x0F3C, (uint16_t)(peek(0x0F3C) + 0x2000));
			tick(0x42AA, 33);
			tick(0x42B2, 12);                          // BR 42B2
		}
		else
		{
			tick(0x42AC, 12);
			poke(0x0F3C, (uint16_t)(peek(0x0F3C) - 0x2000));
			tick(0x42B2, 33);
		}
	}
	jsr(0x42B6, 0x4894);                               // town_alt_cell_bit
	T.r[6] = (uint16_t)(T.r[6] + 2);                   // TST (SP)+: the argument
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
	tick(0x4006, 21);
	pokeb(0x0F59, a.b(2));
	tick(0x400C, 42);
	c = a.u(1) < 4;
	poke((uint16_t)(a.sp + 4), (uint16_t)(a.u(1) - 4));
	tick(0x4012, 36);
	T.r[2] = a.u(1);
	tick(0x4016, 27);
	pokeb(0x0F5B, (uint8_t)T.r[2]);
	tick(0x401A, 27);
	T.r[3] = a.u(0);
	tick(0x401E, 27);
	T.r[0] = T.r[3];
	tick(0x4020, 12);
	for (uint16_t pc = 0x4022; pc <= 0x4028; pc = (uint16_t)(pc + 2))
	{
		T.r[0] = ror(T.r[0]);
		tick(pc, 12);
	}
	T.r[0] &= 0xE000;
	tick(0x402C, 18);
	T.r[1] = T.r[0];
	tick(0x402E, 12);
	c = (T.r[1] + 0x2000u) >> 16;
	T.r[1] = (uint16_t)(T.r[1] + 0x2000);
	tick(0x4032, 18);
	poke(0x01A8, T.r[1]);
	tick(0x4036, 27);
	poke(0x01AC, T.r[1]);
	tick(0x403A, 27);
	T.r[0] = T.r[2];
	tick(0x403C, 12);
	for (uint16_t pc = 0x403E; pc <= 0x4044; pc = (uint16_t)(pc + 2))
	{
		T.r[0] = ror(T.r[0]);
		tick(pc, 12);
	}
	T.r[0] &= 0xE000;
	tick(0x4048, 18);
	T.r[1] = T.r[0];
	tick(0x404A, 12);
	T.r[1] = (uint16_t)(T.r[1] + 0x1000);
	tick(0x404E, 18);
	poke(0x01AA, T.r[1]);
	tick(0x4052, 27);
	poke(0x01AE, T.r[1]);
	tick(0x4056, 27);
	poke(0x0F3C, peek(0x01A8));
	tick(0x405C, 39);
	poke(0x0F3C, peek(0x0F3C) & 0xE000);
	tick(0x4062, 33);
	poke(0x0F3E, T.r[1]);
	tick(0x4066, 27);
	poke(0x0F3E, peek(0x0F3E) & 0xF000);
	tick(0x406C, 33);
	poke(0x01B0, 0);
	tick(0x4070, 27);
	poke(0x01B2, 0);
	tick(0x4074, 27);
	T.r[1] = 0;
	tick(0x4076, 12);
	T.r[1] = a.b(2);
	tick(0x407A, 27);
	pokeb(0x01CC, rb((uint16_t)(0xFEC2 + T.r[1])));
	tick(0x4080, 42);
	pokeb(0x01CD, 0);
	tick(0x4084, 27);
	pokeb(0x01CE, rb((uint16_t)(0xFECA + T.r[1])));
	tick(0x408A, 42);
	pokeb(0x01CF, 0);
	tick(0x408E, 27);
	T.r[0] = peek(0x01CC);
	tick(0x4092, 24);
	T.r[0] = (uint16_t)(T.r[0] << 1);
	tick(0x4094, 12);
	poke(0x01D6, rw((uint16_t)(0xFED2 + T.r[0])));
	tick(0x409A, 42);
	T.r[0] = peek(0x01CE);
	tick(0x409E, 24);
	T.r[0] = (uint16_t)(T.r[0] << 1);
	tick(0x40A0, 12);
	poke(0x01D8, rw((uint16_t)(0xFED2 + T.r[0])));
	tick(0x40A6, 42);
	T.r[0] = T.r[1];
	tick(0x40A8, 12);
	T.r[0] = (uint16_t)(T.r[0] << 1);
	tick(0x40AA, 12);
	poke(0x0F46, peek((uint16_t)(0x7F8C + T.r[0])));
	tick(0x40B0, 42);
	tick(0x40B2, 12);                                  // TST R1
	bool town = false;
	if ((int16_t)T.r[1] > 0)
	{
		tick(0x40B4, 12);
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
		tick(0x40C0, 12);
		T.r[1] = rb((uint16_t)(0xFE8F + T.r[0]));
		tick(0x40C4, 27);
		T.r[1] = (uint16_t)(T.r[1] + peek(0x0F46));
		tick(0x40C8, 24);
		pokeb(0x0F5A, peekb((uint16_t)(T.r[1] + 0x000C)));
		tick(0x40CE, 42);
		jsr(0x40D2, 0x90BA);
		T.r[0] = sext8(rb((uint16_t)(0xFE8F + T.r[0])));
		tick(0x40D6, 27);
		T.r[0] &= 0x00FF;
		tick(0x40DA, 18);
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(0x40DC, 12);
		T.r[0] = (uint16_t)(T.r[0] + peek(0x0F46));
		tick(0x40E0, 24);
		poke(0x0F46, peek(T.r[0]));
		tick(0x40E4, 33);
		tick(0x40EC, 12);                              // BR
	}
	else
	{
		pokeb(0x0F5A, peekb((uint16_t)(0x7F9C + T.r[1])));
		tick(0x40EC, 42);
	}
	push(T.r[2]);
	tick(0x40EE, 24);
	push(0);
	tick(0x40F0, 24);
	pokeb(T.r[6], peekb(0x0F5A));
	tick(0x40F4, 33);
	jsr(0x40F8, 0x80D8);                               // mul16
	T.r[0] = (uint16_t)(T.r[0] + peek(0x0F46));
	tick(0x40FC, 24);
	T.r[1] = T.r[3];
	tick(0x40FE, 12);
	T.r[1] = (uint16_t)(T.r[1] << 1);
	tick(0x4100, 12);
	T.r[0] = (uint16_t)(T.r[0] + T.r[1]);
	tick(0x4102, 12);
	poke(0x0F46, T.r[0]);
	tick(0x4106, 27);
	T.r[0] = T.r[2];
	tick(0x4108, 12);
	T.r[0] = (uint16_t)((T.r[0] >> 8) | (T.r[0] << 8));
	tick(0x410A, 12);                                  // SWAB
	T.r[0] &= 0xFF00;
	tick(0x410C, 12);                                  // CLRB
	for (uint16_t pc = 0x410E; pc <= 0x4114; pc = (uint16_t)(pc + 2))
	{
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(pc, 12);
	}
	poke(0x0F40, T.r[0]);
	tick(0x4118, 27);
	poke(0x0F40, (uint16_t)(peek(0x0F40) - 0x0800));
	tick(0x411E, 33);
	T.r[0] = T.r[3];
	tick(0x4120, 12);
	for (uint16_t pc = 0x4122; pc <= 0x412A; pc = (uint16_t)(pc + 2))
	{
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(pc, 12);
	}
	pokeb(0x0F44, (uint8_t)T.r[0]);
	tick(0x412E, 27);
	poke(0x099A, T.r[2]);
	tick(0x4132, 27);
	poke(0x099A, asr(peek(0x099A), 1));
	tick(0x4136, 27);
	T.r[0] = T.r[3];
	tick(0x4138, 12);
	T.r[0]--;
	tick(0x413A, 12);
	poke(0x0996, T.r[0]);
	tick(0x413E, 27);
	poke(0x0996, asr(peek(0x0996), 1));
	tick(0x4142, 27);
	poke(0x099C, peek(0x099A));
	tick(0x4148, 39);
	poke(0x099C, (uint16_t)(peek(0x099C) + 3));
	tick(0x414E, 33);
	for (uint16_t pc = 0x4152; pc <= 0x415E; pc = (uint16_t)(pc + 4))
	{
		poke(0x0996, (uint16_t)(peek(0x0996) << 1));
		tick(pc, 27);
	}
	poke(0x0998, peek(0x0996));
	tick(0x4164, 39);
	poke(0x0998, (uint16_t)(peek(0x0998) + 0x0040));
	tick(0x416A, 33);
	T.r[0] = 0;
	tick(0x416C, 12);
	T.r[0] = peekb(0x0F59);
	tick(0x4170, 24);
	T.r[0] = (uint16_t)(T.r[0] << 1);
	tick(0x4172, 12);
	poke(0x0F4E, rw((uint16_t)(0xFF54 + T.r[0])));
	tick(0x4178, 42);
	const bool other = peekb(0x0F59) != 0;
	tick(0x417C, 24);                                  // TSTB @#0x0F59
	if (other)
	{
		tick(0x4186, 12);                              // BNE
		poke(0x099E, 0x0FFF);
		tick(0x418C, 33);
	}
	else
	{
		tick(0x417E, 12);
		poke(0x099E, 0x000C);
		tick(0x4184, 33);
		tick(0x418C, 12);                              // BR
	}
	pokeb(0x0E9B, 0);
	tick(0x4190, 27);
	poke(0x09A0, peek(0x01A8));
	tick(0x4196, 39);
	poke(0x09A0, peek(0x09A0) & 0xC000);
	tick(0x419C, 33);
	poke(0x09A2, peek(0x01AA));
	tick(0x41A2, 39);
	poke(0x09A2, peek(0x09A2) & 0xC000);
	tick(0x41A8, 33);
	pokeb(0x0F58, 0);
	tick(0x41AC, 27);
	poke(counter, 0x0008);
	tick(0x41B2, 36);
	for (;;)
	{
		poke(T.r[6], 0xFFFF);
		tick(0x41B6, 27);
		jsr(0x41BA, 0x42BC);                           // step_map_row(-1)
		jsr(0x41BE, 0x4894);                           // town_alt_cell_bit
		jsr(0x41C2, 0x41CE);                           // draw_map_strip
		poke(counter, (uint16_t)(peek(counter) - 1));
		const bool more = peek(counter) != 0;
		tick(0x41C6, 30);                              // DEC 4(SP)
		if (!more)
		{
			tick(0x41C8, 12);                          // BNE
			break;
		}
		tick(0x41B2, 12);
	}
	T.r[6] = (uint16_t)(T.r[6] + 6);
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
	tick(0x48CE, 21);
	pokeb(0x0E99, 0);
	tick(0x48D2, 27);
	poke(0x09AA, peek(0x09B0));
	tick(0x48D8, 39);
	T.r[4] = peek(0x099A);
	tick(0x48DC, 24);
	T.r[2] = peek(0x099C);
	tick(0x48E0, 24);
	T.r[3] = peek(0x0996);
	tick(0x48E4, 24);
	T.r[1] = peek(0x0998);
	tick(0x48E8, 24);
	tick(0x48EA, 12);                                  // TST R4
	if ((int16_t)T.r[4] < 0)
	{
		tick(0x48EC, 12);
		tick(0x48F0, 18);                              // CMP R3,#0x00C0
		if ((int16_t)T.r[3] >= 0x00C0)
		{
			tick(0x48F2, 12);
			T.r[4] = (uint16_t)(T.r[4] + 0x000C);
			tick(0x48F6, 18);
			T.r[2] = (uint16_t)(T.r[2] + 0x000C);
			tick(0x48FA, 18);
			T.r[3] = (uint16_t)(T.r[3] - 0x0050);
			tick(0x48FE, 18);
			T.r[1] = (uint16_t)(T.r[1] - 0x0050);
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
		tick(0x4906, 24);
		push(0);
		tick(0x4908, 24);
		pokeb(T.r[6], peekb(0x0E98));
		tick(0x490C, 33);
		const bool done = (int16_t)T.r[0] < (int16_t)pop();
		tick(0x490E, 18);                              // CMP R0,(SP)+
		if (done)
		{
			tick(0x4958, 12);                          // BLT
			break;
		}
		tick(0x4910, 12);
		const bool used = peekb((uint16_t)(0x09B8 + T.r[0])) != 0;
		tick(0x4914, 27);
		if (used)
		{
			tick(0x4916, 12);
			poke(local, 0);
			tick(0x4918, 21);
			T.r[0] = (uint16_t)(T.r[0] << 1);
			tick(0x491A, 12);
			T.r[0] = peek((uint16_t)(0x0AF8 + T.r[0]));
			tick(0x491E, 27);
			T.r[5] = T.r[0];
			tick(0x4920, 12);
			T.r[5] &= 0x000F;
			tick(0x4924, 18);
			tick(0x4926, 12);                          // CMP R5,R4
			if ((int16_t)T.r[5] < (int16_t)T.r[4])
				tick(0x493C, 12);                      // BLT
			else
			{
				tick(0x4928, 12);
				tick(0x492A, 12);                      // CMP R5,R2
				if ((int16_t)T.r[5] > (int16_t)T.r[2])
					tick(0x493C, 12);                  // BGT
				else
				{
					tick(0x492C, 12);
					T.r[5] = T.r[0];
					tick(0x492E, 12);
					T.r[5] &= 0x01F0;
					tick(0x4932, 18);
					tick(0x4934, 12);                  // CMP R5,R3
					if ((int16_t)T.r[5] < (int16_t)T.r[3])
						tick(0x493C, 12);              // BLT
					else
					{
						tick(0x4936, 12);
						tick(0x4938, 12);              // CMP R5,R1
						if ((int16_t)T.r[5] >= (int16_t)T.r[1])
							tick(0x493C, 12);          // BGE
						else
						{
							tick(0x493A, 12);
							poke(local, (uint16_t)(peek(local) + 1));
							tick(0x493C, 21);
						}
					}
				}
			}
			const bool inside = peek(local) != 0;
			tick(0x493E, 18);                          // TST (SP)
			if (!inside)
			{
				tick(0x4940, 12);
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
		poke(0x09AA, (uint16_t)(peek(0x09AA) - 1));
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
	tick(0x5D0A, 24);
	T.r[0] = 0x0400;
	tick(0x5D0E, 18);
	poke(0x01D4, T.r[0]);
	tick(0x5D12, 27);
	poke(0x1402, T.r[0]);
	tick(0x5D16, 27);
	T.r[0] = a.u(0);
	tick(0x5D1A, 27);
	T.r[0] = sext8(peekb((uint16_t)(0x6567 + T.r[0])));
	tick(0x5D1E, 27);
	T.r[0] &= 0x00FF;
	tick(0x5D22, 18);
	if (T.r[0])
	{
		tick(0x5D24, 12);
		T.r[1] = 0;
		tick(0x5D26, 12);
		T.r[1] = peekb(0x01A6);
		tick(0x5D2A, 24);
		tick(0x5D2C, 12);                              // CMP R1,R0
		bool found = true;
		if (T.r[1] == T.r[0])
		{
			tick(0x5D2E, 12);
			pokeb(0x01A6, 0);
			tick(0x5D32, 27);
			push(0x0070);
			tick(0x5D36, 30);
			tick(0x5D4A, 12);                          // BR
		}
		else
		{
			tick(0x5D38, 12);                          // BNE
			T.r[1] = 0;
			tick(0x5D3A, 12);
			T.r[1] = peekb(0x01A7);
			tick(0x5D3E, 24);
			tick(0x5D40, 12);                          // CMP R1,R0
			if (T.r[1] == T.r[0])
			{
				tick(0x5D42, 12);
				pokeb(0x01A7, 0);
				tick(0x5D46, 27);
				push(0x0076);
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
			tick(0x5D50, 18);                          // TST (SP)+
		}
	}
	else
		tick(0x5D50, 12);                              // BEQ
	T.r[1] = pop();
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
	tick(0x5B4E, 24);                                  // TSTB @#0x031F
	if (fallen)
	{
		tick(0x5BD0, 12);                              // BNE
		c_restore(2);
		return T.r[0];
	}
	tick(0x5B50, 12);
	const bool other = peekb(0x0330) != 0;
	tick(0x5B54, 24);                                  // TSTB @#0x0330
	if (other)
	{
		tick(0x5BD0, 12);                              // BNE
		c_restore(2);
		return T.r[0];
	}
	tick(0x5B56, 12);
	T.r[2] = peek(0x01B4);
	tick(0x5B5A, 24);
	T.r[3] = 0;
	tick(0x5B5C, 12);
	tick(0x5B5E, 12);                                  // TST R2
	if (T.r[2] & 0x8000)
	{
		tick(0x5B66, 12);                              // BLT
		T.r[0] = T.r[2];
		tick(0x5B68, 12);
		T.r[0] = (uint16_t)-T.r[0];
		tick(0x5B6A, 12);
	}
	else
	{
		tick(0x5B60, 12);
		T.r[3]++;
		tick(0x5B62, 12);
		T.r[0] = T.r[2];
		tick(0x5B64, 12);
		tick(0x5B6A, 12);                              // BR
	}
	T.r[0] = (uint16_t)(T.r[0] - peek(0x01DC));
	tick(0x5B6E, 24);
	if (T.r[0] & 0x8000)
	{
		tick(0x5B74, 12);                              // BMI
		T.r[1] = T.r[0];
		tick(0x5B76, 12);
		T.r[1] = (uint16_t)-T.r[1];
		tick(0x5B78, 12);
	}
	else
	{
		tick(0x5B70, 12);
		T.r[1] = T.r[0];
		tick(0x5B72, 12);
		tick(0x5B78, 12);                              // BR
	}
	tick(0x5B7C, 18);                                  // CMP R1,#0x0032
	if ((int16_t)T.r[1] < 0x0032)
	{
		tick(0x5B7E, 12);                              // BGE
		T.r[0] = 0;
		tick(0x5B80, 12);
	}
	else
		tick(0x5B80, 12);
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
		tick(0x5B8A, 24);                              // TST @#0x01DC
		if (up)
		{
			tick(0x5B92, 12);                          // BGT
			push(0x0048);
			tick(0x5B96, 30);
		}
		else
		{
			tick(0x5B8C, 12);
			push(0x0046);
			tick(0x5B90, 30);
			tick(0x5B96, 12);                          // BR
		}
		jsr(0x5B9A, 0x8E78);                           // queue_sound
		poke(0x01DC, (uint16_t)(peek(0x01DC) + 0x001B));
		tick(0x5BA0, 33);
		tick(0x5BCE, 12);                              // BR
	}
	else
	{
		tick(0x5BA2, 12);                              // BLE
		poke(0x01DC, (uint16_t)(peek(0x01DC) - 0x001B));
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
			tick(0x5BAE, 12);
		}
		T.r[0] = T.r[2];
		tick(0x5BB0, 12);
		tick(0x5BB4, 18);                              // CMP R0,#0x0080
		bool silence = false;
		if ((int16_t)T.r[0] < 0x0080)
		{
			tick(0x5BB6, 12);                          // BGE
			silence = peek(0x01DC) != 0;
			tick(0x5BBA, 24);                          // TST @#0x01DC
			tick(silence ? 0x5BBC : 0x5BC6, 12);       // BEQ
		}
		else
			tick(0x5BC6, 12);                          // BGE
		if (silence)
		{
			poke(0x01DC, 0);
			tick(0x5BC0, 27);
			push(0x0053);
			tick(0x5BC4, 30);
			tick(0x5BCA, 12);                          // BR
		}
		else
		{
			push(0x0049);
			tick(0x5BCA, 30);
		}
		jsr(0x5BCE, 0x8E78);                           // queue_sound
	}
	T.r[6] = (uint16_t)(T.r[6] + 2);
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
	cyc(36);
	const uint8_t level = peekb(0x0F59);
	fl::tstb(level);
	if (level)
	{
		cyc(21);
		return;
	}
	cyc(60);
	T.r[1] = peek(0x0996);
	T.r[0] = peek(0x099A);
	if ((int16_t)T.r[0] < 0)
	{
		cyc(36);
		T.r[1] = (uint16_t)(T.r[1] + 0x0050);
		T.r[0] = (uint16_t)(T.r[0] + 0x000C);
	}
	cyc(42);
	T.r[2] = (uint16_t)(T.r[1] - 0x0090);
	if (T.r[2] & 0x8000)
	{
		cyc(12);
		T.r[2] = (uint16_t)-T.r[2];
	}
	cyc(102);
	T.r[1] = asr(T.r[2], 4);
	T.r[2] = (uint16_t)(T.r[0] - 0x0005);
	if (T.r[2] & 0x8000)
	{
		cyc(12);
		T.r[2] = (uint16_t)-T.r[2];
	}
	cyc(48);
	T.r[0] = (uint16_t)(T.r[2] << 1);
	if (!((int16_t)T.r[1] > (int16_t)T.r[0]))
	{
		cyc(12);
		T.r[1] = T.r[0];
	}
	cyc(30);
	if ((int16_t)T.r[1] > 0x000D)
	{
		cyc(18);
		T.r[1] = 0x000D;
	}
	cyc(60);
	T.r[0] = peekb(0x01E6);
	if (T.r[1] != T.r[0])
	{
		cyc(90);
		call(0x8E78, {peekb((uint16_t)(0x5AF6 + T.r[1]))});
		cyc(45);
		pokeb(0x01E6, (uint8_t)T.r[1]);
	}
	cyc(36);
	const uint8_t frame = peekb(0x01C4);
	fl::tstb(frame);
	if (frame)
	{
		cyc(21);
		return;
	}
	tick(0x5C40, 18);                                  // interrupts may still come in here
	T.r[2] = 0x00E0;
	T.psw = (uint8_t)((T.psw & 0x10) | 0xE0);         // MTPS: interrupts off
	T.check_irqs = true;
	cyc(24 + 30 + 30);
	T.r[6] = (uint16_t)(T.r[6] - 2);
	const uint16_t arg = T.r[6];
	poke(arg, 0x000D);
	call(0x8EA2, {});                                  // send_sound_command(0x0D)
	cyc(30);
	call(0x8ED2, {});                                  // read_sound_response
	cyc(42);
	T.r[1] = T.r[0];
	call(0x8F62, {});
	cyc(12 + 30);
	T.r[2] = 0;
	T.psw = (uint8_t)(T.psw & 0x10);                   // MTPS #0
	T.check_irqs = true;
	tick(0x5C5A, 0);                                   // pending interrupts come in now
	cyc(12 + 12);
	if (!T.r[1])
	{
		cyc(36);
		if (!peekb(0x01DA))
		{
			cyc(33);
			pokeb(0x01DA, 0xFF);
		}
		cyc(30);
		call(0x9152, {});                              // next_random
		for (;;)
		{
			cyc(105);
			T.r[1] = T.r[0] & 0x0007;
			T.r[0] = (uint16_t)(T.r[1] << 1);
			if (peekb(0x01DA) & rb((uint16_t)(0xFC7C + T.r[0]))) break;
			cyc(36);
			T.r[0] = (uint16_t)(T.r[1] + 1);
		}
		cyc(162);
		T.r[0] = (uint16_t)(T.r[1] << 1);
		pokeb(0x01DA, peekb(0x01DA) & ~rb((uint16_t)(0xFC7C + T.r[0])));
		T.r[1] = rb((uint16_t)(0x5B04 + T.r[1]));
		poke(arg, T.r[1]);
		call(0x8E78, {});                              // queue_sound(tune)
		cyc(36);
		if (!peekb(0x01E7))
		{
			cyc(36);
			if ((int16_t)peek(0x0B78) < 0)
			{
				cyc(87);
				poke(arg, 0x0040);
				call(0xC98E, {0x0005});
				cyc(18);
			}
			cyc(27);
			pokeb(0x01E7, (uint8_t)(peekb(0x01E7) + 1));
		}
	}
	cyc(18 + 21);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	fl::tst(peek(arg));
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
	tick(0x4990, 27);
	T.r[1] = (uint16_t)(T.r[1] << 1);
	tick(0x4992, 12);
	push(a.u(1));
	tick(0x4996, 39);
	push(0x0180);
	tick(0x499A, 30);
	jsr(0x499E, 0x80D8);                               // mul16
	T.r[0] = (uint16_t)(T.r[0] + peek((uint16_t)(0x4980 + T.r[1])));
	tick(0x49A2, 27);
	poke(0x0186, T.r[0]);
	tick(0x49A6, 27);
	poke(0x0186, (uint16_t)(peek(0x0186) + 0x1E80));
	tick(0x49AC, 33);
	tick(0x49B0, 27);                                  // TST 0x10(SP)
	if (a.u(0))
	{
		tick(0x49B2, 12);
		poke((uint16_t)(a.sp + 2), 0x0009);
		tick(0x49B8, 36);
	}
	else
		tick(0x49B8, 12);                              // BEQ
	T.r[3] = a.u(0);
	tick(0x49BC, 27);
	T.r[3] = (uint16_t)(T.r[3] + 6);
	tick(0x49C0, 18);
	T.r[2] = a.u(0);
	tick(0x49C4, 27);
	tick(0x4A02, 12);                                  // BR
	for (;;)
	{
		tick(0x4A04, 12);                              // CMP R2,R3
		if ((int16_t)T.r[2] > (int16_t)T.r[3])
		{
			tick(0x4A06, 12);                          // BLE
			break;
		}
		tick(0x49C6, 12);
		T.r[0] = peek(0x0186);
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
		T.r[2] = (uint16_t)(T.r[2] + 3);
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
	tick(0x51F6, 18);
	poke(0x01D4, T.r[0]);
	tick(0x51FA, 27);
	poke(0x1402, T.r[0]);
	tick(0x51FE, 27);
	T.r[0] = sext8(peekb(0x031D));
	tick(0x5202, 24);
	T.r[2] = a.u(0);
	tick(0x5206, 27);
	tick(0x520A, 18);                                  // BIT #7,R2
	if (T.r[2] & 7)
	{
		tick(0x520C, 12);
		T.r[1] = 0x0007;
		tick(0x5210, 18);
		T.r[1] &= (uint16_t)~T.r[2];
		tick(0x5212, 12);
		if (T.r[1])
		{
			tick(0x5214, 12);                          // BEQ
			T.r[1] = T.r[2];
			tick(0x5216, 12);
			T.r[1] = asr(T.r[1], 1);
			tick(0x5218, 12);
			T.r[1] = asr(T.r[1], 1);
			tick(0x521A, 12);
			T.r[1] = asr(T.r[1], 1);
			tick(0x521C, 12);
			T.r[1] = (uint16_t)(T.r[1] << 1);
			tick(0x521E, 12);
			T.r[2] &= 0x0007;
			tick(0x5222, 18);
			T.r[2] = (uint16_t)(T.r[2] << 1);
			tick(0x5224, 12);
			T.r[2] = (uint16_t)(T.r[2] << 1);
			tick(0x5226, 12);
			T.r[2] = (uint16_t)(T.r[2] << 1);
			tick(0x5228, 12);
			T.r[2] = (uint16_t)(T.r[2] << 1);
			tick(0x522A, 12);
			T.r[2] = (uint16_t)(T.r[2] + peek((uint16_t)(0x79E8 + T.r[1])));
			tick(0x522E, 27);
			T.r[2] = (uint16_t)(T.r[2] + T.r[0]);
			tick(0x5230, 12);
			T.r[0] = T.r[2];
			tick(0x5232, 12);
			tick(0x5244, 12);                          // BR
		}
		else
		{
			tick(0x5234, 12);
			T.r[1] = T.r[2];
			tick(0x5236, 12);
			T.r[1] &= 0x0038;
			tick(0x523A, 18);
			T.r[1] = (uint16_t)(T.r[1] << 1);
			tick(0x523C, 12);
			T.r[1] = (uint16_t)(T.r[1] + T.r[0]);
			tick(0x523E, 12);
			T.r[0] = T.r[1];
			tick(0x5240, 12);
			T.r[0] = (uint16_t)(T.r[0] + 0x0220);
			tick(0x5244, 18);
		}
	}
	else
		tick(0x5244, 12);                              // BEQ
	T.r[1] = T.r[0];
	tick(0x5246, 12);
	T.r[1] = (uint16_t)(T.r[1] << 1);
	tick(0x5248, 12);
	T.r[0] = peek(0x02E6);
	tick(0x524C, 24);
	T.r[0] = (uint16_t)(T.r[0] - peek((uint16_t)(0x6AC8 + T.r[1])));
	tick(0x5250, 27);
	T.r[1] = 0x0C00;
	tick(0x5254, 18);
	poke(0x01D4, T.r[1]);
	tick(0x5258, 27);
	poke(0x1402, T.r[1]);
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
	T.r[2] = 3;
	tick(0x409C, 18);
	for (;;)
	{
		T.r[3] = T.r[2];
		tick(0x409E, 12);
		T.r[3] = (uint16_t)(T.r[3] << 1);
		tick(0x40A0, 12);
		uint16_t pc = 0x40A0;
		for (const auto &p : parts)
		{
			T.r[0] = 0;
			tick((uint16_t)(pc + 2), 12);
			T.r[0] = peekb((uint16_t)(p[0] + T.r[2]));
			tick((uint16_t)(pc + 6), 27);
			T.r[0] = (uint16_t)((T.r[0] >> 8) | (T.r[0] << 8));
			tick((uint16_t)(pc + 8), 12);              // SWAB
			T.r[1] = 0;
			tick((uint16_t)(pc + 10), 12);
			T.r[1] = peekb((uint16_t)(p[1] + T.r[2]));
			tick((uint16_t)(pc + 14), 27);
			T.r[1] = (uint16_t)((T.r[1] >> 8) | (T.r[1] << 8));
			tick((uint16_t)(pc + 16), 12);
			const uint16_t carry = (uint16_t)(((uint32_t)T.r[0] + T.r[1]) >> 16);
			T.r[0] = (uint16_t)(T.r[0] + T.r[1]);
			tick((uint16_t)(pc + 18), 12);
			T.r[0] = (uint16_t)((T.r[0] >> 8) | (T.r[0] << 8));
			tick((uint16_t)(pc + 20), 12);
			pokeb((uint16_t)(p[1] + T.r[2]), (uint8_t)T.r[0]);
			tick((uint16_t)(pc + 24), 30);
			const uint16_t a = (uint16_t)(p[2] + T.r[3]);
			if (g_fix_subpixel_carry) poke(a, (uint16_t)(peek(a) + carry));
			tick((uint16_t)(pc + 28), 30);             // ADC (the carry is clear: adds nothing)
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
	tick(0x4114, 36);
	push(peek(0x0570));
	tick(0x4118, 36);
	jsr(0x411C, 0x80FA);                               // mul32
	T.r[1] = peek(0x0204);
	tick(0x4120, 24);
	T.r[2] = peek(0x0206);
	tick(0x4124, 24);
	uint16_t c = (uint16_t)(T.r[1] >> 15);
	T.r[1] = (uint16_t)(T.r[1] << 1);
	tick(0x4126, 12);                                  // ASL R1
	T.r[2] = (uint16_t)((T.r[2] << 1) | c);
	tick(0x4128, 12);                                  // ROL R2
	poke((uint16_t)(T.r[6] + 2), peek(0x02E2));
	tick(0x412E, 42);
	poke(T.r[6], peek(0x04F0));
	tick(0x4132, 33);
	jsr(0x4136, 0x80FA);
	uint32_t s = (uint32_t)T.r[1] + peek(0x0204);
	T.r[1] = (uint16_t)s;
	tick(0x413A, 24);
	T.r[2] = (uint16_t)(T.r[2] + (s >> 16));
	tick(0x413C, 12);                                  // ADC R2
	T.r[2] = (uint16_t)(T.r[2] + peek(0x0206));
	tick(0x4140, 24);
	poke(0x0204, T.r[1]);
	tick(0x4144, 27);
	poke(0x0206, T.r[2]);
	tick(0x4148, 27);
	c = (uint16_t)(T.r[1] >> 15);
	T.r[1] = (uint16_t)(T.r[1] << 1);
	tick(0x414A, 12);
	T.r[2] = (uint16_t)((T.r[2] << 1) | c);
	tick(0x414C, 12);
	s = (uint32_t)T.r[1] + peek(0x0204);
	T.r[1] = (uint16_t)s;
	tick(0x4150, 24);
	T.r[2] = (uint16_t)(T.r[2] + (s >> 16));
	tick(0x4152, 12);
	T.r[2] = (uint16_t)(T.r[2] + peek(0x0206));
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
	tick(0x4164, 42);
	poke(T.r[6], peek(0x05B2));
	tick(0x4168, 33);
	jsr(0x416C, 0x80FA);
	s = (uint32_t)T.r[1] + peek(0x0204);
	T.r[1] = (uint16_t)s;
	tick(0x4170, 24);
	T.r[2] = (uint16_t)(T.r[2] + (s >> 16));
	tick(0x4172, 12);
	poke(0x0206, (uint16_t)(peek(0x0206) + T.r[2]));
	tick(0x4176, 27);
	const uint16_t v = pop();                          // MOV (SP)+,(SP)+
	poke(T.r[6], v);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	tick(0x4178, 27);
	T.r[2] = sext8(peekb(0x0207));
	tick(0x417C, 24);
	T.r[1] &= 0xFF00;
	tick(0x417E, 12);                                  // CLRB R1
	T.r[1] |= peekb(0x0206);
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
	tick(0x4FA0, 24);
	T.r[2] = (uint16_t)(T.r[2] - peek(0x06FE));
	tick(0x4FA4, 24);
	T.r[1] = T.r[2];
	tick(0x4FA6, 12);
	T.r[1] = (uint16_t)(T.r[1] + 0x0500);
	tick(0x4FAA, 18);
	T.r[0] = peek(0x01C4);
	tick(0x4FAE, 24);
	T.r[0] &= 0x01FF;
	tick(0x4FB2, 18);
	T.r[1] = (uint16_t)(T.r[1] + T.r[0]);
	tick(0x4FB4, 12);
	T.r[3] = peek(0x0730);
	tick(0x4FB8, 24);
	T.r[3] = (uint16_t)(T.r[3] - peek(0x073E));
	tick(0x4FBC, 24);
	T.r[3] = (uint16_t)(T.r[3] + peek(0x06B0));
	tick(0x4FC0, 24);
	T.r[0] = T.r[3];
	tick(0x4FC2, 12);
	T.r[0] = (uint16_t)(T.r[0] + 0x0500);
	tick(0x4FC6, 18);
	tick(0x4FC8, 12);                                  // TST R1
	if (T.r[1] & 0x8000)
	{
		tick(0x4FCE, 12);                              // BLT
		T.r[2] = T.r[1];
		tick(0x4FD0, 12);
		T.r[2] = (uint16_t)-T.r[2];
		tick(0x4FD2, 12);
	}
	else
	{
		tick(0x4FCA, 12);
		T.r[2] = T.r[1];
		tick(0x4FCC, 12);
		tick(0x4FD2, 12);                              // BR
	}
	tick(0x4FD6, 18);                                  // CMP R2,#0x0200
	bool negative;                                     // the flags tested by the BGE at 4FF6
	if ((int16_t)T.r[2] < 0x0200)
	{
		tick(0x4FD8, 12);                              // BGE
		T.r[2] = 0x0004;
		tick(0x4FDC, 18);
		negative = (T.r[0] & 0x8000) != 0;
		tick(0x4FDE, 12);                              // TST R0
		tick(0x4FF6, 12);                              // BR
	}
	else
	{
		tick(0x4FE0, 12);                              // BGE
		tick(0x4FE2, 12);                              // TST R0
		if (T.r[0] & 0x8000)
		{
			tick(0x4FE8, 12);                          // BLT
			T.r[3] = T.r[0];
			tick(0x4FEA, 12);
			T.r[3] = (uint16_t)-T.r[3];
			tick(0x4FEC, 12);
		}
		else
		{
			tick(0x4FE4, 12);
			T.r[3] = T.r[0];
			tick(0x4FE6, 12);
			tick(0x4FEC, 12);                          // BR
		}
		tick(0x4FF0, 18);                              // CMP R3,#0x0200
		if ((int16_t)T.r[3] >= 0x0200)
		{
			tick(0x4FFE, 12);                          // BGE
			T.r[2] = 0x0002;
			tick(0x5002, 18);
			tick(0x5004, 12);                          // TST R1
			if (T.r[1] & 0x8000)
			{
				tick(0x5006, 12);
				T.r[2] |= 0x0004;
				tick(0x500A, 18);
			}
			else
				tick(0x500A, 12);                      // BGE
			tick(0x500C, 12);                          // TST R0
			if (T.r[0] & 0x8000)
			{
				tick(0x500E, 12);
				T.r[2] = (uint16_t)-T.r[2];
				tick(0x5010, 12);
				T.r[2] = (uint16_t)(T.r[2] + 0x0010);
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
		tick(0x4FF4, 12);
		negative = (T.r[1] & 0x8000) != 0;
		tick(0x4FF6, 12);                              // TST R1
	}
	if (negative)
	{
		tick(0x4FF8, 12);                              // BGE
		T.r[2] |= 0x0008;
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
	tick(0x4ECE, 24);
	T.r[1] = a.u(0);
	tick(0x4ED2, 27);
	tick(0x4ED6, 18);                                  // CMP R1,#6
	if (T.r[1] == 6)
	{
		tick(0x4ED8, 12);
		poke(0x03BA, 0x50FA);
		tick(0x4EDE, 33);
		pokeb(0x0495, peekb(0x0495) | 0x18);
		tick(0x4EE4, 33);
		poke(0x04FA, 0);
		tick(0x4EE8, 27);
		poke(0x057A, 0);
		tick(0x4EEC, 27);
		poke(0x043A, 0);
		tick(0x4EF0, 27);
		tick(0x4F24, 12);                              // BR
	}
	else
	{
		tick(0x4EF2, 12);                              // BNE
		pokeb(0x0E6A, (uint8_t)T.r[1]);
		tick(0x4EF6, 27);
		pokeb(0x0E68, 0xB4);
		tick(0x4EFC, 33);
		T.r[0] = T.r[1];
		tick(0x4EFE, 12);
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(0x4F00, 12);
		poke(0x0E66, peek((uint16_t)(0x0430 + T.r[0])));
		tick(0x4F06, 42);
		pokeb(0x0E69, peekb((uint16_t)(0x0770 + T.r[1])));
		tick(0x4F0C, 42);
		poke(0x0E5E, peek((uint16_t)(0x04F0 + T.r[0])));
		tick(0x4F12, 42);
		poke(0x0E60, peek((uint16_t)(0x0570 + T.r[0])));
		tick(0x4F18, 42);
		poke(0x0E62, peek((uint16_t)(0x03B0 + T.r[0])));
		tick(0x4F1E, 42);
		poke(0x0E64, peek((uint16_t)(0x03F0 + T.r[0])));
		tick(0x4F24, 42);
	}
	T.r[0] = T.r[1];
	tick(0x4F26, 12);
	T.r[0] = (uint16_t)(T.r[0] << 1);
	tick(0x4F28, 12);
	poke((uint16_t)(0x0430 + T.r[0]), 0);
	tick(0x4F2C, 30);
	pokeb((uint16_t)(0x04B0 + T.r[1]), peekb((uint16_t)(0x04B0 + T.r[1])) | 0x40);
	tick(0x4F32, 36);
	poke((uint16_t)(0x03F0 + T.r[0]), 0);
	tick(0x4F36, 30);
	poke((uint16_t)(0x04F0 + T.r[0]), 0);
	tick(0x4F3A, 30);
	poke((uint16_t)(0x0570 + T.r[0]), 0);
	tick(0x4F3E, 30);
	T.r[1] = pop();
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
	tick(0x4D38, 18);
	poke((uint16_t)(sp + 4), peek(0x01D2));
	tick(0x4D3E, 42);
	poke((uint16_t)(sp + 2), peek(0x01D4));
	tick(0x4D44, 42);
	T.r[0] = 0x2800;
	tick(0x4D48, 18);
	poke(0x01D4, T.r[0]);
	tick(0x4D4C, 27);
	poke(0x1402, T.r[0]);
	tick(0x4D50, 27);
	jsr(0x4D54, 0x8068);                               // select_video_bank0
	poke(0x0188, 0xE000);
	tick(0x4D5A, 33);
	T.r[0] = a.u(1);
	tick(0x4D5E, 27);
	T.r[1] = peek(T.r[0]);
	tick(0x4D60, 18);
	T.r[1] = (uint16_t)(T.r[1] + 0x2000);
	tick(0x4D64, 18);
	T.r[1] &= 0xFFFE;
	tick(0x4D68, 18);
	pokeb(sp, peekb((uint16_t)(T.r[0] + 2)));
	tick(0x4D6C, 36);
	T.r[3] = sext8(peekb((uint16_t)(T.r[0] + 3)));
	tick(0x4D70, 27);
	T.r[5] = 0;
	tick(0x4D72, 12);
	T.r[5] |= (uint8_t)T.r[3];
	tick(0x4D74, 12);
	T.r[5] = (uint16_t)(T.r[5] << 1);                  // the row width in bytes
	tick(0x4D76, 12);
	tick(0x4D7A, 27);                                  // TST 0x1A(SP)
	if (a.u(0))
	{
		tick(0x4D7C, 12);
		T.r[2] = 0;
		tick(0x4D7E, 12);
		T.r[2] = peekb(sp);
		tick(0x4D80, 18);
		tick(0x4D82, 12);                              // TST R2
		if ((int16_t)T.r[2] > 0)
		{
			tick(0x4D84, 12);
			for (;;)
			{
				T.r[0] = 0;
				tick(0x4D86, 12);
				T.r[0] = (uint8_t)T.r[3];
				tick(0x4D88, 12);
				tick(0x4D8A, 12);                      // TST R0
				if ((int16_t)T.r[0] > 0)
				{
					tick(0x4D8C, 12);
					for (;;)
					{
						poke(T.r[1], 0);
						T.r[1] = (uint16_t)(T.r[1] + 2);
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
				tick(0x4D92, 12);
				T.r[0] = (uint16_t)(T.r[0] - T.r[5]);
				tick(0x4D94, 12);
				T.r[1] = T.r[0];
				tick(0x4D96, 12);
				T.r[1] = (uint16_t)(T.r[1] + 0x0080);
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
		tick(0x4DA2, 30);                              // CMPB @#0x036C,#1
		if (last)
		{
			tick(0x4DA4, 12);
			const bool other = peekb(0x0F59) != 0;
			tick(0x4DA8, 24);                          // TSTB @#0x0F59
			if (other)
				tick(0x4DDA, 12);                      // BNE
			else
			{
				tick(0x4DAA, 12);
				poke(0x0188, 0xA000);
				tick(0x4DB0, 33);
				push(0x5DF6);
				tick(0x4DB4, 30);
				push(0);
				tick(0x4DB6, 24);
				jsr(0x4DBA, 0xEAD4);                   // draw_tile_block(0, 0x5DF6)
				poke(0x0186, 0x3746);
				tick(0x4DC0, 33);
				poke(T.r[6], peek(0x0244));
				tick(0x4DC4, 33);
				push(peek(0x0246));
				tick(0x4DC8, 36);
				push(0x0008);
				tick(0x4DCC, 30);
				push(0);
				tick(0x4DCE, 24);
				push(0x0001);
				tick(0x4DD2, 30);
				jsr(0x4DD6, 0xBA96);                   // print the number
				T.r[6] = (uint16_t)(T.r[6] + 0x000C);
				tick(0x4DDA, 18);
			}
		}
		else
			tick(0x4DDA, 12);                          // BNE
		T.r[0] = 0;
		tick(0x4DDC, 12);
		T.r[0] = peekb(0x036C);
		tick(0x4DE0, 24);
		T.r[0] = asr(T.r[0], 1);
		tick(0x4DE2, 12);
		pokeb(0x036C, (uint8_t)T.r[0]);
		tick(0x4DE6, 27);
		tick(0x4EAE, 12);                              // BR
	}
	else
	{
		tick(0x4DE8, 12);                              // BEQ
		T.r[2] = 0;
		tick(0x4DEA, 12);
		T.r[2] = peekb(0x036C);
		tick(0x4DEE, 24);
		T.r[2] = (uint16_t)(T.r[2] << 1);
		tick(0x4DF0, 12);
		const bool text = (peek(T.r[0]) & 1) != 0;
		tick(0x4DF4, 24);                              // BIT #1,(R0)
		if (text)
		{
			tick(0x4DF6, 12);
			pokeb(0x036C, (uint8_t)T.r[2]);
			tick(0x4DFA, 27);
			pokeb(0x036C, (uint8_t)(peekb(0x036C) | 1));
			tick(0x4E00, 33);
			push(0x5DF6);
			tick(0x4E04, 30);
			push(0x0001);
			tick(0x4E08, 30);
			jsr(0x4E0C, 0xEAD4);                       // draw_tile_block(1, 0x5DF6)
			T.r[6] = (uint16_t)(T.r[6] + 4);
			tick(0x4E0E, 24);                          // CMP (SP)+,(SP)+
			tick(0x4E14, 12);                          // BR
		}
		else
		{
			tick(0x4E10, 12);                          // BEQ
			pokeb(0x036C, (uint8_t)T.r[2]);
			tick(0x4E14, 27);
		}
		T.r[0] = peek(0x0188);
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
		T.r[4] |= 0x0111;
		tick(0x4E24, 18);
		T.r[2] = 0;
		tick(0x4E26, 12);
		T.r[2] = (uint8_t)T.r[3];
		tick(0x4E28, 12);
		T.r[2] = (uint16_t)(T.r[2] - 2);
		tick(0x4E2C, 18);
		if ((int16_t)T.r[2] > 0)
		{
			tick(0x4E2E, 12);
			for (;;)
			{
				poke(T.r[1], T.r[4]);
				T.r[1] = (uint16_t)(T.r[1] + 2);
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
		tick(0x4E3A, 12);
		T.r[4] = (uint16_t)(T.r[4] - T.r[5]);
		tick(0x4E3C, 12);
		T.r[1] = T.r[4];
		tick(0x4E3E, 12);
		T.r[1] = (uint16_t)(T.r[1] + 0x0080);
		tick(0x4E42, 18);
		poke((uint16_t)(sp + 0x0A), T.r[0]);
		tick(0x4E46, 30);
		poke((uint16_t)(sp + 0x0A), peek((uint16_t)(sp + 0x0A)) | 0x0113);
		tick(0x4E4C, 36);
		poke((uint16_t)(sp + 6), T.r[0]);
		tick(0x4E50, 30);
		poke((uint16_t)(sp + 6), peek((uint16_t)(sp + 6)) | 0x0087);
		tick(0x4E56, 36);
		poke((uint16_t)(sp + 8), T.r[0]);
		tick(0x4E5A, 30);
		poke((uint16_t)(sp + 8), peek((uint16_t)(sp + 8)) | 0x0114);
		tick(0x4E60, 36);
		T.r[4] = 0;
		tick(0x4E62, 12);
		T.r[4] = peekb(sp);
		tick(0x4E64, 18);
		T.r[4] = (uint16_t)(T.r[4] - 2);
		tick(0x4E68, 18);
		if ((int16_t)T.r[4] > 0)
		{
			tick(0x4E6A, 12);
			for (;;)
			{
				poke(T.r[1], peek((uint16_t)(sp + 0x0A)));
				T.r[1] = (uint16_t)(T.r[1] + 2);
				tick(0x4E6E, 36);
				T.r[2] = 0;
				tick(0x4E70, 12);
				T.r[2] = (uint8_t)T.r[3];
				tick(0x4E72, 12);
				T.r[2] = (uint16_t)(T.r[2] - 2);
				tick(0x4E76, 18);
				if ((int16_t)T.r[2] > 0)
				{
					tick(0x4E78, 12);
					for (;;)
					{
						poke(T.r[1], peek((uint16_t)(sp + 6)));
						T.r[1] = (uint16_t)(T.r[1] + 2);
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
				tick(0x4E82, 36);
				T.r[2] = T.r[1];
				tick(0x4E84, 12);
				T.r[2] = (uint16_t)(T.r[2] - T.r[5]);
				tick(0x4E86, 12);
				T.r[1] = T.r[2];
				tick(0x4E88, 12);
				T.r[1] = (uint16_t)(T.r[1] + 0x0080);
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
		T.r[4] |= 0x0116;
		tick(0x4E9A, 18);
		T.r[3] &= 0x00FF;
		tick(0x4E9E, 18);
		T.r[3] = (uint16_t)(T.r[3] - 2);
		tick(0x4EA2, 18);
		if ((int16_t)T.r[3] > 0)
		{
			tick(0x4EA4, 12);
			for (;;)
			{
				poke(T.r[1], T.r[4]);
				T.r[1] = (uint16_t)(T.r[1] + 2);
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
	tick(0x4EB4, 42);
	poke(0x01D4, peek((uint16_t)(sp + 2)));
	tick(0x4EBA, 42);
	poke(0x1400, peek(0x01D2));
	tick(0x4EC0, 39);
	poke(0x1402, peek(0x01D4));
	tick(0x4EC6, 39);
	T.r[6] = (uint16_t)(T.r[6] + 0x000C);
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
	tick(0x4B86, 30);
	T.r[5] = sext8(peekb(0x0336));
	tick(0x4B8A, 24);
	if ((int16_t)T.r[5] >= 0)
	{
		tick(0x4B8C, 12);
		poke((uint16_t)(sp + 2), peek(0x01D2));
		tick(0x4B92, 42);
		poke(sp, peek(0x01D4));
		tick(0x4B96, 33);
		poke(0x036A, (uint16_t)(peek(0x036A) + 1));
		tick(0x4B9A, 27);
		T.r[4] = 0;
		tick(0x4B9C, 12);
		tick(0x4C0A, 12);                              // BR
		for (;;)
		{
			tick(0x4C0C, 12);                          // CMP R4,R5
			if ((int16_t)T.r[4] > (int16_t)T.r[5])
			{
				tick(0x4C0E, 12);                      // BLE
				break;
			}
			tick(0x4B9E, 12);
			T.r[0] = T.r[4];
			tick(0x4BA0, 12);
			T.r[0] = (uint16_t)(T.r[0] << 1);
			tick(0x4BA2, 12);
			const bool due = peek((uint16_t)(0x0338 + T.r[0])) == peek(0x036A);
			tick(0x4BA8, 39);
			if (due)
			{
				tick(0x4BAA, 12);
				T.r[1] = 0x2800;
				tick(0x4BAE, 18);
				poke(0x01D4, T.r[1]);
				tick(0x4BB2, 27);
				poke(0x1402, T.r[1]);
				tick(0x4BB6, 27);
				const bool tiles = (int16_t)peek(peek((uint16_t)(0x0348 + T.r[0]))) >= 0x4000;
				tick(0x4BBC, 39);
				if (tiles)
				{
					tick(0x4BBE, 12);
					push(peek(peek((uint16_t)(0x0348 + T.r[0]))));
					tick(0x4BC2, 45);
					push(0x0001);
					tick(0x4BC6, 30);
					jsr(0x4BCA, 0xEAD4);               // draw_tile_block(1, block)
					tick(0x4BD8, 12);                  // BR
				}
				else
				{
					tick(0x4BCC, 12);                  // BLT
					push(peek((uint16_t)(0x0348 + T.r[0])));
					tick(0x4BD0, 39);
					push(0x0001);
					tick(0x4BD4, 30);
					jsr(0x4BD8, 0x4D30);               // message_box(erase)
				}
				T.r[0] = sext8(peekb(0x0336));
				tick(0x4BDC, 24);
				T.r[0]--;
				tick(0x4BDE, 12);
				pokeb(0x0336, (uint8_t)T.r[0]);
				tick(0x4BE2, 27);
				T.r[3] = sext8((uint8_t)T.r[0]);
				tick(0x4BE4, 12);
				if ((int16_t)T.r[3] >= 0)
				{
					tick(0x4BE6, 12);
					T.r[2] = T.r[4];
					tick(0x4BE8, 12);
					T.r[2]--;
					tick(0x4BEA, 12);
					tick(0x4C00, 12);                  // BR
					for (;;)
					{
						T.r[2]++;
						tick(0x4C02, 12);
						tick(0x4C04, 12);              // CMP R2,R3
						if ((int16_t)T.r[2] > (int16_t)T.r[3])
						{
							tick(0x4C06, 12);          // BLE
							break;
						}
						tick(0x4BEC, 12);
						T.r[1] = T.r[2];
						tick(0x4BEE, 12);
						T.r[1] = (uint16_t)(T.r[1] << 1);
						tick(0x4BF0, 12);
						T.r[0] = T.r[2];
						tick(0x4BF2, 12);
						T.r[0] = (uint16_t)(T.r[0] << 1);
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
				tick(0x4C08, 24);                      // CMP (SP)+,(SP)+: the arguments
			}
			else
				tick(0x4C08, 12);                      // BNE
			T.r[4]++;
			tick(0x4C0A, 12);
		}
		poke(0x01D2, peek((uint16_t)(sp + 2)));
		tick(0x4C14, 42);
		poke(0x01D4, peek(sp));
		tick(0x4C18, 33);
		poke(0x1400, peek(0x01D2));
		tick(0x4C1E, 39);
		poke(0x1402, peek(0x01D4));
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
	tick(0x4C2C, 24);
	T.r[0] = (uint16_t)(T.r[0] << 1);
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
	tick(0x4C3A, 30);                                  // CMPB @#0x0336,#7
	if (full)
	{
		tick(0x4C3C, 12);
		T.r[3] = peek(0x01D2);
		tick(0x4C40, 24);
		T.r[4] = peek(0x01D4);
		tick(0x4C44, 24);
		T.r[0] = 0x2800;
		tick(0x4C48, 18);
		poke(0x01D4, T.r[0]);
		tick(0x4C4C, 27);
		poke(0x1402, T.r[0]);
		tick(0x4C50, 27);
		const bool tiles = (int16_t)peek(peek(0x0348)) >= 0x4000;
		tick(0x4C56, 39);                              // CMP @0x0348,#0x4000
		if (tiles)
		{
			tick(0x4C58, 12);
			push(peek(peek(0x0348)));
			tick(0x4C5C, 45);
			push(0x0001);
			tick(0x4C60, 30);
			jsr(0x4C64, 0xEAD4);                       // draw_tile_block(1, block)
			tick(0x4C72, 12);                          // BR
		}
		else
		{
			tick(0x4C66, 12);                          // BLT
			push(peek(0x0348));
			tick(0x4C6A, 36);
			push(0x0001);
			tick(0x4C6E, 30);
			jsr(0x4C72, 0x4D30);                       // message_box(erase)
		}
		T.r[2] = 0;
		tick(0x4C74, 12);
		for (;;)
		{
			T.r[1] = T.r[2];
			tick(0x4C76, 12);
			T.r[1] = (uint16_t)(T.r[1] << 1);
			tick(0x4C78, 12);
			T.r[0] = T.r[2];
			tick(0x4C7A, 12);
			T.r[0] = (uint16_t)(T.r[0] << 1);
			tick(0x4C7C, 12);
			poke((uint16_t)(0x0348 + T.r[1]), peek((uint16_t)(0x034A + T.r[0])));
			tick(0x4C82, 45);
			poke((uint16_t)(0x0338 + T.r[1]), peek((uint16_t)(0x033A + T.r[0])));
			tick(0x4C88, 45);
			T.r[2]++;
			tick(0x4C8A, 12);
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
		tick(0x4C9A, 27);
		poke(0x01D4, T.r[4]);
		tick(0x4C9E, 27);
		poke(0x1400, peek(0x01D2));
		tick(0x4CA4, 39);
		poke(0x1402, peek(0x01D4));
		tick(0x4CAA, 39);
		T.r[6] = (uint16_t)(T.r[6] + 4);
		tick(0x4CAC, 24);                              // CMP (SP)+,(SP)+
	}
	else
		tick(0x4CAC, 12);                              // BNE
	const bool font = (peekb(0x036D) & 1) != 0;
	tick(0x4CB2, 30);                                  // BITB #1,@#0x036D
	if (font)
	{
		tick(0x4CB4, 12);
		jsr(0x4CB8, 0xA8FE);
	}
	else
		tick(0x4CB8, 12);                              // BEQ
	T.r[0] = 0;
	tick(0x4CBA, 12);
	T.r[0] = peekb(0x036D);
	tick(0x4CBE, 24);
	T.r[0] = asr(T.r[0], 1);
	tick(0x4CC0, 12);
	pokeb(0x036D, (uint8_t)T.r[0]);
	tick(0x4CC4, 27);
	const bool table = (peek(0x036E) & 1) != 0;
	tick(0x4CCA, 30);                                  // BIT #1,@#0x036E
	if (table)
	{
		tick(0x4CCC, 12);
		poke(0x0184, 0x5CD2);
		tick(0x4CD2, 33);
	}
	else
		tick(0x4CD2, 12);                              // BEQ
	T.r[0] = 0;
	tick(0x4CD4, 12);
	T.r[0] = peekb(0x036E);
	tick(0x4CD8, 24);
	T.r[0] = asr(T.r[0], 1);
	tick(0x4CDA, 12);
	pokeb(0x036E, (uint8_t)T.r[0]);
	tick(0x4CDE, 27);
	T.r[0] = sext8(peekb(0x0358));
	tick(0x4CE2, 24);
	T.r[0] = (uint16_t)(T.r[0] << 1);
	tick(0x4CE4, 12);
	push(peek((uint16_t)(0x0362 + T.r[0])));
	const uint16_t arg = T.r[6];
	tick(0x4CE8, 39);
	jsr(0x4CEC, 0xA9FC);                               // print the message
	poke(0x0184, 0x5AC8);
	tick(0x4CF2, 33);
	T.r[0] = sext8(peekb(0x0336));
	tick(0x4CF6, 24);
	T.r[0]++;
	tick(0x4CF8, 12);
	pokeb(0x0336, (uint8_t)T.r[0]);
	tick(0x4CFC, 27);
	T.r[0] = sext8((uint8_t)T.r[0]);
	tick(0x4CFE, 12);
	T.r[0] = (uint16_t)(T.r[0] << 1);
	tick(0x4D00, 12);
	T.r[1] = sext8(peekb(0x0358));
	tick(0x4D04, 24);
	T.r[1] = (uint16_t)(T.r[1] << 1);
	tick(0x4D06, 12);
	poke((uint16_t)(0x0348 + T.r[0]), peek((uint16_t)(0x0362 + T.r[1])));
	tick(0x4D0C, 45);
	T.r[2] = peek((uint16_t)(0x035A + T.r[1]));
	tick(0x4D10, 27);
	T.r[2] = (uint16_t)(T.r[2] + peek(0x036A));
	tick(0x4D14, 24);
	poke((uint16_t)(0x0338 + T.r[0]), T.r[2]);
	tick(0x4D18, 30);
	poke((uint16_t)(0x0362 + T.r[1]), 0);
	tick(0x4D1C, 30);
	T.r[0] = sext8(peekb(0x0358));
	tick(0x4D20, 24);
	T.r[0]--;
	tick(0x4D22, 12);
	pokeb(0x0358, (uint8_t)T.r[0]);
	tick(0x4D26, 27);
	T.r[0] = sext8((uint8_t)T.r[0]);
	tick(0x4D28, 12);
	if (T.r[0] & 0x8000)
	{
		tick(0x4D2A, 12);                              // BGE
		poke(T.r[0], 0);
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
	tick(0x5A88, 33);
	tick(0x5A8C, 27);                                  // TST 0xA(SP)
	if (!a.u(0))
	{
		tick(0x5A8E, 12);
		T.r[0] = 0x0053;
		tick(0x5A92, 18);
		jsr(0x5A96, 0x8B04);
		const bool many = peekb(0x01F9) > 4;
		tick(0x5A9C, 30);                              // CMPB @#0x01F9,#4
		if (many)
		{
			tick(0x5A9E, 12);
			T.r[0] = 0x005A;
			tick(0x5AA2, 18);
			jsr(0x5AA6, 0x8B04);
		}
		else
			tick(0x5AA6, 12);                          // BLOS
		poke(0x0186, (uint16_t)(peek(0x0186) - 0x0018));
		tick(0x5AAC, 33);
		T.r[1] = 0;
		tick(0x5AAE, 12);
		T.r[1] = peekb(0x01F9);
		tick(0x5AB2, 24);
		T.r[1] = asr(T.r[1], 1);
		tick(0x5AB4, 12);
		T.r[1] = asr(T.r[1], 1);
		tick(0x5AB6, 12);
		push(T.r[1]);
		tick(0x5AB8, 24);
		push(0);
		tick(0x5ABA, 24);
		push(0x0002);
		tick(0x5ABE, 30);
		push(0x0001);
		tick(0x5AC2, 30);
		push(0);
		tick(0x5AC4, 24);
		jsr(0x5AC8, 0xBA96);                           // print the count
		const bool part = (peekb(0x01F9) & 3) != 0;
		tick(0x5ACE, 30);                              // BITB #3,@#0x01F9
		if (part)
		{
			tick(0x5AD0, 12);
			T.r[0] = sext8(peekb(0x01FA));
			tick(0x5AD4, 24);
			T.r[0] &= 0x0003;
			tick(0x5AD8, 18);
			T.r[1] = 0;
			tick(0x5ADA, 12);
			T.r[1] = peekb((uint16_t)(0x5A49 + T.r[0]));
			tick(0x5ADE, 27);
			T.r[0] = sext8(peekb(0x01F9));
			tick(0x5AE2, 24);
			T.r[0] &= 0x0003;
			tick(0x5AE6, 18);
			T.r[0] = (uint16_t)(T.r[0] << 1);
			tick(0x5AE8, 12);
			T.r[0] = (uint16_t)(T.r[0] + T.r[1]);
			tick(0x5AEA, 12);
			T.r[1] = T.r[0];
			tick(0x5AEC, 12);
			T.r[1] = (uint16_t)(T.r[1] - 2);
			tick(0x5AF0, 18);
			T.r[0] = T.r[1];
			tick(0x5AF2, 12);
			T.r[0] = (uint16_t)(T.r[0] << 1);
			tick(0x5AF4, 12);
			T.r[2] = peek((uint16_t)(0x5A4E + T.r[0]));
			tick(0x5AF8, 27);
			T.r[2] |= peek(0x0188);
			tick(0x5AFC, 24);
			poke(peek(0x0186), T.r[2]);
			tick(0x5B00, 36);
			T.r[0] = peek(0x0186);
			tick(0x5B04, 24);
			T.r[2] = T.r[1];
			tick(0x5B06, 12);
			T.r[2] = (uint16_t)(T.r[2] << 1);
			tick(0x5B08, 12);
			T.r[3] = peek((uint16_t)(0x5A50 + T.r[2]));
			tick(0x5B0C, 27);
			T.r[3] |= peek(0x0188);
			tick(0x5B10, 24);
			poke((uint16_t)(T.r[0] + 2), T.r[3]);
			tick(0x5B14, 30);
		}
		else
			tick(0x5B14, 12);                          // BEQ
		T.r[6] = (uint16_t)(T.r[6] + 0x000A);
		tick(0x5B18, 18);
		tick(0x5B60, 12);                              // BR
	}
	else
	{
		tick(0x5B1A, 12);                              // BNE
		T.r[0] = 0;
		tick(0x5B1C, 12);
		T.r[0] = peekb(0x01FA);
		tick(0x5B20, 24);
		T.r[0] &= 0x00E0;
		tick(0x5B24, 18);
		tick(0x5B28, 18);                              // CMP R0,#0x00E0
		if (T.r[0] == 0x00E0)
		{
			tick(0x5B58, 12);                          // BEQ
			T.r[0] = 0x0059;
			tick(0x5B5C, 18);
			jsr(0x5B60, 0x8B04);
		}
		else
		{
			tick(0x5B2A, 12);
			T.r[0] = 0;
			tick(0x5B2C, 12);
			T.r[0] = peekb(0x01F3);
			tick(0x5B30, 24);
			for (uint16_t pc = 0x5B32; pc <= 0x5B3C; pc = (uint16_t)(pc + 2))
			{
				T.r[0] = asr(T.r[0], 1);
				tick(pc, 12);
			}
			T.r[1] = T.r[0];
			tick(0x5B3E, 12);
			T.r[1] &= 0x0003;
			tick(0x5B42, 18);
			T.r[0] = T.r[1];
			tick(0x5B44, 12);
			T.r[0] = (uint16_t)(T.r[0] + 0x0054);
			tick(0x5B48, 18);
			jsr(0x5B4C, 0x8B04);
			tick(0x5B50, 18);                          // CMP R1,#1
			if (T.r[1] == 1)
				tick(0x5B60, 12);                      // BEQ
			else
			{
				tick(0x5B52, 12);
				T.r[0] = 0x0058;
				tick(0x5B56, 18);
				tick(0x5B5C, 12);                      // BR
				jsr(0x5B60, 0x8B04);
			}
		}
	}
	T.r[1] = 0;
	tick(0x5B62, 12);
	T.r[1] = peekb(0x01F9);
	tick(0x5B66, 24);
	tick(0x5B6A, 27);                                  // TST 0xC(SP)
	if (!a.u(1))
	{
		tick(0x5B6C, 12);
		T.r[1] = 0x0003;
		tick(0x5B70, 18);
		tick(0x5BAC, 12);                              // BR
	}
	else
	{
		tick(0x5B72, 12);                              // BNE
		T.r[0] = 0;
		tick(0x5B74, 12);
		T.r[0] = peekb(0x01F3);
		tick(0x5B78, 24);
		for (uint16_t pc = 0x5B7A; pc <= 0x5B84; pc = (uint16_t)(pc + 2))
		{
			T.r[0] = asr(T.r[0], 1);
			tick(pc, 12);
		}
		T.r[0] &= 0x0003;
		tick(0x5B88, 18);
		T.r[2] = 0;
		tick(0x5B8A, 12);
		T.r[2] = peekb((uint16_t)(0x5A46 + T.r[0]));
		tick(0x5B8E, 27);
		T.r[2] = (uint16_t)(T.r[2] << 1);
		tick(0x5B90, 12);
		tick(0x5B92, 12);                              // CMP R1,R2
		if ((int16_t)T.r[1] >= (int16_t)T.r[2])
		{
			tick(0x5B94, 12);                          // BLT
			T.r[1] = 0;
			tick(0x5B96, 12);
			tick(0x5BAC, 12);                          // BR
		}
		else
		{
			tick(0x5B98, 12);
			T.r[2] = 0;
			tick(0x5B9A, 12);
			T.r[2] = peekb((uint16_t)(0x5A46 + T.r[0]));
			tick(0x5B9E, 27);
			tick(0x5BA0, 12);                          // CMP R1,R2
			if ((int16_t)T.r[1] >= (int16_t)T.r[2])
			{
				tick(0x5BA2, 12);                      // BLT
				T.r[1] = 0x0001;
				tick(0x5BA6, 18);
				tick(0x5BAC, 12);                      // BR
			}
			else
			{
				tick(0x5BA8, 12);
				T.r[1] = 0x0002;
				tick(0x5BAC, 18);
			}
		}
	}
	T.r[1] = (uint16_t)(T.r[1] + 0x004F);
	tick(0x5BB0, 18);
	T.r[0] = T.r[1];
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
	tick(0x5022, 33);
	poke(0x0EB4, (uint16_t)(peek(0x0EB4) - 1));
	tick(0x5026, 27);
	T.r[0] = 0;
	tick(0x5028, 12);
	T.r[1] = 0;
	tick(0x502A, 12);
	T.r[2] = 0;
	tick(0x502C, 12);
	bool visible = false;
	const uint16_t sx = peek(0x063E);
	tick(0x5032, 30);                                  // CMP @#0x063E,#0x8200
	if (sx <= 0x8200)
		tick(0x503C, 12);                              // BLOS
	else
	{
		tick(0x5034, 12);
		visible = peek(0x063E) < 0xF000;
		tick(0x503A, 30);                              // CMP @#0x063E,#0xF000
		tick(visible ? 0x504C : 0x503C, 12);           // BCS
	}
	if (!visible)
	{
		const uint16_t sy = peek(0x067E);
		tick(0x5042, 30);                              // CMP @#0x067E,#0x1000
		if ((int16_t)sy >= 0x1000)
			tick(0x505E, 12);                          // BGE
		else
		{
			tick(0x5044, 12);
			visible = peek(0x067E) > 0x9000;
			tick(0x504A, 30);                          // CMP @#0x067E,#0x9000
			tick(visible ? 0x504C : 0x505E, 12);       // BLOS
		}
	}
	if (visible)
	{
		T.r[1] = peek(0x04F0);
		tick(0x5050, 24);
		T.r[2] = peek(0x0570);
		tick(0x5054, 24);
		T.r[2] = (uint16_t)(T.r[2] + peek(0x05B0));
		tick(0x5058, 24);
		poke(0x0EB4, 0xFFFF);
		tick(0x505E, 33);
	}
	const bool waiting = (int16_t)peek(0x0EB4) > 0;
	tick(0x5062, 24);                                  // TST @#0x0EB4
	if (waiting)
	{
		tick(0x50C6, 12);                              // BGT
		c_restore(1);
		return T.r[0];
	}
	tick(0x5064, 12);
	const bool delay = (int16_t)peek(0x0F22) > 0;
	tick(0x5068, 24);                                  // TST @#0x0F22
	if (delay)
		tick(0x508C, 12);                              // BGT
	else
	{
		tick(0x506A, 12);
		const bool fastest = (int8_t)peekb(0x0EB2) > 6;
		tick(0x5070, 30);                              // CMPB @#0x0EB2,#6
		if (fastest)
			tick(0x507C, 12);                          // BGT
		else
		{
			tick(0x5072, 12);
			T.r[0] = sext8(peekb(0x0EB2));
			tick(0x5076, 24);
			T.r[0]++;
			tick(0x5078, 12);
			pokeb(0x0EB2, (uint8_t)T.r[0]);
			tick(0x507C, 27);
		}
		T.r[0] = 0;
		tick(0x507E, 12);
		T.r[0] = peekb(0x0F2E);
		tick(0x5082, 24);
		pokeb(0x0F22, rb((uint16_t)(0xFE0D + T.r[0])));
		tick(0x5088, 42);
		pokeb(0x0F23, 0);
		tick(0x508C, 27);
	}
	jsr(0x5090, 0x4F98);                               // direction_from_object7
	push(T.r[0]);
	tick(0x5092, 24);
	T.r[0] = sext8(peekb(0x0EB2));
	tick(0x5096, 24);
	push(T.r[0]);
	tick(0x5098, 24);
	push(T.r[1]);
	tick(0x509A, 24);
	push(T.r[2]);
	tick(0x509C, 24);
	jsr(0x50A0, 0xE13A);                               // set_object7_motion
	const bool shown = (int16_t)peek(0x0EB4) < 0;
	tick(0x50A4, 24);                                  // TST @#0x0EB4
	if (shown)
	{
		tick(0x50A6, 12);                              // BGE
		poke(0x0EB4, 0x0005);
		tick(0x50AC, 33);
		tick(0x50C2, 12);                              // BR
	}
	else
	{
		tick(0x50AE, 12);
		jsr(0x50B2, 0x9152);                           // next_random
		poke(0x0EB4, T.r[0]);
		tick(0x50B6, 27);
		poke(0x0EB4, peek(0x0EB4) & 0x000F);
		tick(0x50BC, 33);
		poke(0x0EB4, (uint16_t)(peek(0x0EB4) + 0x0010));
		tick(0x50C2, 33);
	}
	T.r[6] = (uint16_t)(T.r[6] + 8);
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
	tick(0x525A, 12);
	pokeb(0x0ED8, (uint8_t)(peekb(0x0ED8) | rb((uint16_t)(0xFC74 + T.r[1]))));
	tick(0x5260, 42);
	T.r[0] = 0;
	tick(0x5262, 12);
	T.r[0] = peekb(0x0F31);
	tick(0x5266, 24);
	T.r[0] = sext8(rb((uint16_t)(0xFE7C + T.r[0])));
	tick(0x526A, 27);
	T.r[0] &= 0x00FF;
	tick(0x526E, 18);
	T.r[2] = 0;
	tick(0x5270, 12);
	T.r[2] = rb((uint16_t)(0xFE70 + T.r[0]));
	tick(0x5274, 27);
	const bool enough = (int16_t)peek(0x0256) >= (int16_t)T.r[2];
	tick(0x5278, 24);                                  // CMP @#0x0256,R2
	if (enough)
	{
		tick(0x527A, 12);                              // BLT
		T.r[0] = 0x025C;
		tick(0x527E, 18);
		T.r[0] = (uint16_t)(T.r[0] + T.r[1]);
		tick(0x5280, 12);
		const bool none = (peekb(T.r[0]) & 0x80) != 0;
		tick(0x5282, 18);                              // TSTB (R0)
		if (none)
		{
			tick(0x5284, 12);                          // BGE
			pokeb(T.r[0], 0);
			tick(0x5286, 21);
		}
		else
			tick(0x5286, 12);
		pokeb(T.r[0], (uint8_t)(peekb(T.r[0]) + 1));
		tick(0x5288, 21);
		push(0x76C8);
		tick(0x528C, 30);
		push(0x00D2);
		tick(0x5290, 30);
		push(0x0001);
		tick(0x5294, 30);
		jsr(0x5298, 0xA974);                           // the bought message
		T.r[2] = (uint16_t)-T.r[2];
		tick(0x529A, 12);
		poke(T.r[6], T.r[2]);
		tick(0x529C, 21);
		jsr(0x52A0, 0xF002);                           // add_cash(-price)
		poke(T.r[6], T.r[1]);
		tick(0x52A2, 21);
		push(0);
		tick(0x52A4, 24);
		jsr(0x52A8, 0xD97C);
		jsr(0x52AC, 0x9152);                           // next_random
		T.r[0] &= 0x0007;
		tick(0x52B0, 18);
		poke(T.r[6], 0);
		tick(0x52B2, 21);
		pokeb(T.r[6], rb((uint16_t)(0xFE1E + T.r[0])));
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
		tick(0x52C8, 30);
		push(0x00F0);
		tick(0x52CC, 30);
		push(0x0001);
		tick(0x52D0, 30);
		jsr(0x52D4, 0xA974);                           // the not-enough message
		poke(T.r[6], T.r[1]);
		tick(0x52D6, 21);
		push(0x0001);
		tick(0x52DA, 30);
		jsr(0x52DE, 0xD97C);
		jsr(0x52E2, 0x9152);                           // next_random
		T.r[0] &= 0x0003;
		tick(0x52E6, 18);
		poke(T.r[6], 0);
		tick(0x52E8, 21);
		pokeb(T.r[6], rb((uint16_t)(0xFE26 + T.r[0])));
		tick(0x52EC, 36);
		jsr(0x52F0, 0x8E78);                           // queue_sound
	}
	pokeb(0x0ED7, peekb(0x0ED7) & ~0x10);
	tick(0x52F6, 33);
	T.r[6] = (uint16_t)(T.r[6] + 8);
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
	tick(0x43C6, 30);
	T.r[5] = 0xFFFF;
	tick(0x43CA, 18);
	push(a.u(0));
	tick(0x43CE, 39);
	poke(T.r[6], (uint16_t)(peek(T.r[6]) + 0x000E));  // the last entry
	tick(0x43D2, 27);
	push(a.u(0));
	tick(0x43D6, 39);
	poke(T.r[6], (uint16_t)(peek(T.r[6]) + T.r[5]));  // the entry, counted up from first - 1
	tick(0x43D8, 21);
	const uint16_t sp = T.r[6];
	tick(0x4410, 12);                                  // BR
	bool found = false;
	for (;;)
	{
		poke(sp, (uint16_t)(peek(sp) + 1));
		tick(0x4412, 21);
		const bool more = (int16_t)peek(sp) <= (int16_t)peek((uint16_t)(sp + 2));
		tick(0x4416, 33);                              // CMP (SP),2(SP)
		if (!more)
		{
			tick(0x4418, 12);                          // BLE
			break;
		}
		tick(0x43DA, 12);
		T.r[0] = peek(sp);
		tick(0x43DC, 18);
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(0x43DE, 12);
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(0x43E0, 12);
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(0x43E2, 12);
		poke((uint16_t)(sp + 4), peek((uint16_t)(0x0370 + T.r[0])));
		tick(0x43E8, 45);
		poke((uint16_t)(sp + 6), peek((uint16_t)(0x0372 + T.r[0])));
		tick(0x43EE, 45);
		T.r[1] = 0x0004;
		tick(0x43F2, 18);
		T.r[1] = (uint16_t)(T.r[1] + T.r[6]);
		tick(0x43F4, 12);
		T.r[2] = 0x0006;
		tick(0x43F8, 18);
		T.r[2] = (uint16_t)(T.r[2] + T.r[6]);
		tick(0x43FA, 12);
		T.r[3] = a.u(2);
		tick(0x43FE, 27);
		T.r[4] = a.u(1);
		tick(0x4402, 27);
		jsr(0x4406, 0x80D0);                           // sub32
		const bool below = (int16_t)peek((uint16_t)(sp + 4)) < 0;
		tick(0x440A, 27);                              // TST 4(SP)
		if (below)
		{
			tick(0x440C, 12);                          // BGE
			T.r[5] = peek(sp);
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
		tick(0x441E, 12);
		T.r[3]++;
		tick(0x4420, 12);
		T.r[2] = a.u(0);
		tick(0x4424, 27);
		T.r[2] = (uint16_t)(T.r[2] + 0x000F);
		tick(0x4428, 18);
		tick(0x4454, 12);                              // BR
		for (;;)
		{
			T.r[2]--;
			tick(0x4456, 12);
			tick(0x4458, 12);                          // CMP R2,R3
			if ((int16_t)T.r[2] < (int16_t)T.r[3])
			{
				tick(0x445A, 12);                      // BGE
				break;
			}
			tick(0x442A, 12);
			T.r[0] = T.r[2];
			tick(0x442C, 12);
			T.r[0] = (uint16_t)(T.r[0] << 1);
			tick(0x442E, 12);
			T.r[0] = (uint16_t)(T.r[0] << 1);
			tick(0x4430, 12);
			T.r[0] = (uint16_t)(T.r[0] << 1);
			tick(0x4432, 12);
			T.r[1] = T.r[0];
			tick(0x4434, 12);
			T.r[0] = T.r[2];
			tick(0x4436, 12);
			T.r[0] = (uint16_t)(T.r[0] << 1);
			tick(0x4438, 12);
			T.r[0] = (uint16_t)(T.r[0] << 1);
			tick(0x443A, 12);
			T.r[0] = (uint16_t)(T.r[0] << 1);
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
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(0x445E, 12);
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(0x4460, 12);
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(0x4462, 12);
		poke((uint16_t)(0x0370 + T.r[0]), a.u(2));
		tick(0x4468, 45);
		poke((uint16_t)(0x0372 + T.r[0]), a.u(1));
		tick(0x446E, 45);
	}
	T.r[0] = T.r[5];
	tick(0x4470, 12);
	T.r[6] = (uint16_t)(T.r[6] + 8);
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
	tick(0x5686, 24);
	push(T.r[2]);
	tick(0x5688, 24);
	push(T.r[3]);
	tick(0x568A, 24);
	push(T.r[4]);
	tick(0x568C, 24);
	push(T.r[5]);
	tick(0x568E, 24);
	T.r[1] = peek(0x023A);
	tick(0x5692, 24);
	T.r[2] = sext8(peekb(0x0994));
	tick(0x5696, 24);
	T.r[2] = (uint16_t)(T.r[2] - 5);
	tick(0x569A, 18);
	T.r[2] = (uint16_t)(T.r[2] << 1);
	tick(0x569C, 12);
	// the tests of one object, up to the BEQ after the call; false: on to 0x5716
	auto touches = [&]() -> bool {
		const uint16_t obj = peek((uint16_t)(0x03FA + T.r[2]));
		tick(0x56A4, 27);                              // TST 0x3FA(R2)
		if (!obj)
		{
			tick(0x5716, 12);
			return false;
		}
		tick(0x56A6, 12);
		T.r[0] = peek((uint16_t)(0x03FA + T.r[2]));
		tick(0x56AA, 27);
		T.r[3] = peek((uint16_t)(0x063A + T.r[2]));
		tick(0x56AE, 27);
		T.r[3] = (uint16_t)(T.r[3] + peek(T.r[0]));
		tick(0x56B0, 18);
		T.r[5] = peek(0x0630);
		tick(0x56B4, 24);
		T.r[5] = (uint16_t)(T.r[5] + peek(T.r[1]));
		tick(0x56B6, 18);
		bool less = (int16_t)T.r[3] < (int16_t)T.r[5];
		T.r[3] = (uint16_t)(T.r[3] - T.r[5]);
		tick(0x56B8, 12);
		if (!less)
		{
			tick(0x56BA, 12);                          // BLT
			const bool near = T.r[3] < peek((uint16_t)(T.r[1] + 4));
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
			tick(0x56C4, 12);
			const bool near = T.r[3] < peek((uint16_t)(T.r[0] + 4));
			tick(0x56C8, 27);                          // CMP R3,4(R0)
			if (!near)
			{
				tick(0x5716, 12);                      // BCC
				return false;
			}
			tick(0x56CA, 12);
		}
		T.r[3] = peek((uint16_t)(0x067A + T.r[2]));
		tick(0x56CE, 27);
		T.r[3] = (uint16_t)(T.r[3] + peek((uint16_t)(T.r[0] + 2)));
		tick(0x56D2, 27);
		T.r[5] = peek(0x0238);
		tick(0x56D6, 24);
		T.r[5] = (uint16_t)(T.r[5] + peek((uint16_t)(T.r[1] + 2)));
		tick(0x56DA, 27);
		less = (int16_t)T.r[3] < (int16_t)T.r[5];
		T.r[3] = (uint16_t)(T.r[3] - T.r[5]);
		tick(0x56DC, 12);
		if (!less)
		{
			tick(0x56DE, 12);                          // BLT
			const bool near = T.r[3] < peek((uint16_t)(T.r[1] + 6));
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
			tick(0x56E8, 12);
			T.r[4] = (uint16_t)-T.r[4];
			tick(0x56EA, 12);
			const bool near = T.r[4] < peek((uint16_t)(T.r[0] + 6));
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
		tick(0x56F2, 24);
		push(T.r[2]);
		tick(0x56F4, 24);
		push(T.r[2]);
		tick(0x56F6, 24);
		poke(T.r[6], (uint16_t)(peek(T.r[6]) + 0x000A));
		tick(0x56FA, 27);
		push(T.r[3]);
		tick(0x56FC, 24);
		jsr(0x5700, 0x572A);
		const uint16_t v = pop();                      // MOV (SP)+,(SP)+
		poke(T.r[6], v);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		tick(0x5702, 27);
		T.r[2] = pop();
		tick(0x5704, 18);
		T.r[1] = pop();
		tick(0x5706, 18);
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
		tick(0x569E, 12);                              // TST R2
		if ((int16_t)T.r[2] < 0)
		{
			tick(0x571C, 12);                          // BLT
			T.r[0] = 0;
			tick(0x571E, 12);
			break;
		}
		tick(0x56A0, 12);
		if (touches())
		{
			T.r[4] = T.r[2];
			tick(0x570C, 12);
			T.r[4] = asr(T.r[4], 1);
			tick(0x570E, 12);
			T.r[0] = T.r[4];
			tick(0x5710, 12);
			T.r[0] = (uint16_t)(T.r[0] + 5);
			tick(0x5714, 18);
			tick(0x571E, 12);                          // BR
			break;
		}
		T.r[2] = (uint16_t)(T.r[2] - 2);
		tick(0x571A, 18);
		tick(0x569C, 12);                              // BR
	}
	T.r[5] = pop();
	tick(0x5720, 18);
	T.r[4] = pop();
	tick(0x5722, 18);
	T.r[3] = pop();
	tick(0x5724, 18);
	T.r[2] = pop();
	tick(0x5726, 18);
	T.r[1] = pop();
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
	tick(0x5102, 24);                                  // TST @#0x02FA
	if (timing)
	{
		tick(0x5104, 12);
		poke(0x02FA, (uint16_t)(peek(0x02FA) - 1));
		tick(0x5108, 27);
	}
	else
		tick(0x5108, 12);                              // BEQ
	tick(0x510C, 18);                                  // CMP R1,#1
	if (T.r[1] == 1)
	{
		tick(0x510E, 12);
		const bool fast = (int16_t)peek(0x02FC) >= 0x0380;
		tick(0x5114, 30);                              // CMP @#0x02FC,#0x0380
		bool end;
		if (fast)
		{
			tick(0x5116, 12);                          // BLT
			const bool left = peek(0x02FA) != 0;
			tick(0x511A, 24);                          // TST @#0x02FA
			if (!left)
			{
				tick(0x5126, 12);                      // BEQ
				end = true;
			}
			else
			{
				tick(0x511C, 12);
				poke(0x02FA, (uint16_t)(peek(0x02FA) - 1));
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
			tick(0x512A, 30);
			push(0x0002);
			tick(0x512E, 30);
			jsr(0x5132, 0xB602);
			pokeb(0x031F, 0);
			tick(0x5136, 27);
			poke(T.r[6], 0x0055);
			tick(0x513A, 27);
			jsr(0x513E, 0x8E78);                       // queue_sound
			T.r[6] = (uint16_t)(T.r[6] + 4);
			tick(0x5140, 24);                          // CMP (SP)+,(SP)+
		}
		const bool slope = (peekb(0x0327) & 7) != 0;
		tick(0x5146, 30);                              // BITB #7,@#0x0327
		if (slope)
		{
			tick(0x5168, 12);                          // BNE
			T.r[0] = 0;
			tick(0x516A, 12);
			T.r[0] = peekb(0x0327);
			tick(0x516E, 24);
			T.r[0] = asr(T.r[0], 1);
			tick(0x5170, 12);
			T.r[0] = asr(T.r[0], 1);
			tick(0x5172, 12);
			T.r[0] = asr(T.r[0], 1);
			tick(0x5174, 12);
			T.r[0] = sext8(rb((uint16_t)(0xFCD0 + T.r[0])));
			tick(0x5178, 27);
			T.r[0] &= 0x00FF;
			tick(0x517C, 18);
			T.r[2] = 0;
			tick(0x517E, 12);
			T.r[2] = peekb(0x031D);
			tick(0x5182, 24);
			tick(0x5184, 12);                          // CMP R0,R2
			if (T.r[0] == T.r[2])
				tick(0x5192, 12);                      // BEQ
			else
			{
				tick(0x5186, 12);
				pokeb(0x031D, (uint8_t)T.r[0]);
				tick(0x518A, 27);
				pokeb(0x01F8, (uint8_t)T.r[0]);
				tick(0x518E, 27);
				jsr(0x5192, 0x9B82);
			}
		}
		else
		{
			tick(0x5148, 12);
			const bool rolling = peekb(0x031E) != 0;
			tick(0x514C, 24);                          // TSTB @#0x031E
			if (rolling)
				tick(0x5192, 12);                      // BNE
			else
			{
				tick(0x514E, 12);
				const uint16_t v = (uint16_t)(peek(0x01B4) - 0x0014);
				poke(0x01B4, v);
				tick(0x5154, 33);
				tick(0x5156, 18);                      // CCC CV
				if ((int16_t)v > 0)
					tick(0x5192, 12);                  // BGT
				else
				{
					tick(0x5158, 12);
					poke(0x01B4, 0);
					tick(0x515C, 27);
					push(0x0055);
					tick(0x5160, 30);
					jsr(0x5164, 0x8E78);               // queue_sound
					T.r[6] = (uint16_t)(T.r[6] + 2);
					tick(0x5166, 18);                  // TST (SP)+
					tick(0x5192, 12);                  // BR
				}
			}
		}
	}
	else
		tick(0x5192, 12);                              // BNE
	tick(0x5196, 18);                                  // CMP R1,#2
	if (T.r[1] == 2)
	{
		tick(0x5198, 12);
		poke(0x01B4, 0);
		tick(0x519C, 27);
		const bool left = peek(0x02FA) != 0;
		tick(0x51A0, 24);                              // TST @#0x02FA
		if (left)
			tick(0x51D2, 12);                          // BNE
		else
		{
			tick(0x51A2, 12);
			pokeb(0x031B, 0x3F);
			tick(0x51A8, 33);
			pokeb(0x031F, 0);
			tick(0x51AC, 27);
			T.r[0] = sext8(peekb(0x0327));
			tick(0x51B0, 24);
			T.r[0] &= 0x0007;
			tick(0x51B4, 18);
			tick(0x51B8, 18);                          // CMP R0,#4
			if (T.r[0] < 4)
				tick(0x51D2, 12);                      // BCS
			else
			{
				tick(0x51BA, 12);
				pokeb(0x0325, 0x7F);
				tick(0x51C0, 33);
				T.r[0] = 0;
				tick(0x51C2, 12);
				T.r[0] = peekb(0x0327);
				tick(0x51C6, 24);
				T.r[0] = asr(T.r[0], 1);
				tick(0x51C8, 12);
				T.r[0] = asr(T.r[0], 1);
				tick(0x51CA, 12);
				T.r[0] = asr(T.r[0], 1);
				tick(0x51CC, 12);
				pokeb(0x031D, rb((uint16_t)(0xFCD0 + T.r[0])));
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
	tick(0x4B52, 30);
	jsr(0x4B56, 0x8EA2);                               // send_sound_command
	jsr(0x4B5A, 0x8ED2);                               // read_sound_response
	T.r[1] = T.r[0];
	tick(0x4B5C, 12);
	jsr(0x4B60, 0x8F62);
	jsr(0x4B64, 0x8E72);                               // clear_flag_01a4
	T.r[0] = 0;
	tick(0x4B66, 12);
	T.r[0] = peekb(0x01A3);
	tick(0x4B6A, 24);
	T.r[2] = T.r[1];
	tick(0x4B6C, 12);
	T.r[2] = (uint16_t)~T.r[2];
	tick(0x4B6E, 12);
	T.r[0] &= (uint16_t)~T.r[2];
	tick(0x4B70, 12);
	tick(0x4B74, 18);                                  // BIT #7,R0
	if (T.r[0] & 7)
	{
		tick(0x4B76, 12);
		pokeb(0x01A2, 1);
		tick(0x4B7C, 33);
	}
	else
		tick(0x4B7C, 12);                              // BEQ
	pokeb(0x01A3, (uint8_t)T.r[1]);
	tick(0x4B80, 27);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	tick(0x4B82, 18);                                  // TST (SP)+
	c_restore(1);
	return T.r[0];
}

// ---------------------------------------------------------------- zone switches while scrolling

// A5:454A (assembly): Skate City zone switches.  When the scroll position (0x01AA, 0x01A8)
// enters another 512-pixel row or column (last ones in 0x0F52/0x0F54), the zone bytes of the
// cells along the new edge (8 x 8 table at 0x0F60; row 0x0232, column 0x0234 within the
// cell) are checked with A5:43F8-44EC and their own conditions, until one switches the
// tile bank (F:EEE6 counts it in 0x0236).  The modes of the zone byte that jump into the
// middle of an instruction or outside the routine (not used by the game) do nothing here.
// Registers end as the original leaves them; the flags are those of the final ADD to SP.
void scan_zone_switches()
{
	const uint16_t E = T.r[6];
	const uint16_t L0 = (uint16_t)(E - 8), L2 = (uint16_t)(E - 6), L4 = (uint16_t)(E - 4), L6 = (uint16_t)(E - 2);
	T.r[6] = L0;
	auto finish = [&]() { cyc(39); T.r[6] = fl::add(T.r[6], 0x0008); };   // ADD #8,SP; RTS
	auto finish_pop = [&](int extra) { cyc(extra + 18); T.r[6] = (uint16_t)(T.r[6] + 2); finish(); };
	auto zone_at = [&](uint16_t i) -> uint16_t { return peekb((uint16_t)(0x0F60 + i)); };
	auto set_bank = [&]() { T.r[0] = call(0xEEE6, {}); };   // set_tile_bank(stack word)
	cyc(120);
	T.r[0] = peek(0x01AA);
	T.r[2] = T.r[0] & ~0x01FF;
	T.r[5] = 0;
	if (T.r[2] != peek(0x0F52))
	{
		cyc(24);
		T.r[5] = 1;
	}
	else
	{
		cyc(78);
		T.r[1] = peek(0x01A8) & ~0x01FF;
		if (T.r[1] == peek(0x0F54))
		{
			cyc(18);
			finish();
			return;
		}
	}
	cyc(471);
	poke(0x0236, 0);
	T.r[4] = asr(T.r[0], 9);
	poke(0x0232, T.r[4] & 7);
	T.r[1] = asr(T.r[4], 3);
	T.r[4] = T.r[1] & 7;
	T.r[3] = asr(peek(0x01A8), 9);
	poke(0x0234, T.r[3] & 0x000F);
	T.r[1] = asr(T.r[3], 4);
	const bool carry = (T.r[3] >> 3) & 1;             // shifted out by the last ASR
	T.r[3] = T.r[1] & 7;
	const bool row_changed = T.r[5] & 1;
	T.r[5] = (uint16_t)((carry ? 0x8000 : 0) | (T.r[5] >> 1));   // ROR
	if (row_changed)
	{
		cyc(186);
		poke(L2, (uint16_t)(T.r[0] - peek(0x0F52)));
		poke(0x0F52, T.r[2]);
		poke(L0, (uint16_t)(T.r[3] + 4));
		if ((int16_t)peek(L2) < 0)
		{
			cyc(249);
			poke(L6, (uint16_t)(T.r[4] << 3));
			T.r[0] = (uint16_t)(T.r[3] + peek(L6));
			push(zone_at(T.r[0]));
			T.r[0] = call(0x43F8, {});                 // zone_switch_a
			cyc(36);
			if (peek(0x0236)) { finish_pop(18); return; }
			cyc(45);
			T.r[2] = peek(L6);
			T.r[1] = 3;
			do
			{
				cyc(162);
				T.r[0] = (uint16_t)(((T.r[1] + T.r[3]) & 7) + T.r[2]);
				T.r[5] = zone_at(T.r[0]);
				T.r[0] = (uint16_t)((T.r[5] & 7) << 1);
				bool sw = false;
				switch (T.r[5] & 7)
				{
				case 1: case 2: case 5: sw = true; break;
				case 3: case 4: case 7: cyc(42); sw = (int16_t)peek(0x0232) < 7; break;
				default: break;                        // 0, 6: no switch
				}
				if (sw)
				{
					cyc(51);
					poke(T.r[6], T.r[5]);
					set_bank();
				}
				cyc(36);
				if (peek(0x0236)) { finish_pop(18); return; }
				cyc(18);
			} while (--T.r[1]);
			cyc(159);
			T.r[0] = (uint16_t)((peek(L0) & 7) + peek(L6));
			poke(T.r[6], zone_at(T.r[0]));
			T.r[0] = call(0x4442, {});                 // zone_switch_b
		}
		else
		{
			cyc(36);
			T.r[0] = peek(0x0232) ? 6 : 5;
			cyc(peek(0x0232) ? 18 : 30);
			cyc(291);
			T.r[0] = (uint16_t)(T.r[0] + T.r[4]);
			T.r[1] = T.r[0] & 7;
			poke(L6, (uint16_t)(T.r[1] << 3));
			T.r[0] = (uint16_t)(T.r[3] + peek(L6));
			push(zone_at(T.r[0]));
			T.r[0] = call(0x4496, {});                 // zone_switch_c
			cyc(36);
			if (peek(0x0236)) { finish_pop(18); return; }
			cyc(63);
			T.r[2] = peek(L6);
			poke(L4, 1);
			do
			{
				cyc(177);
				T.r[0] = (uint16_t)(((T.r[3] + peek(L4)) & 7) + T.r[2]);
				T.r[5] = zone_at(T.r[0]);
				T.r[0] = (uint16_t)((T.r[5] & 7) << 1);
				bool sw = false;
				switch (T.r[5] & 7)
				{
				case 1: case 2: cyc(42); sw = peek(0x0232) != 1; break;
				case 3: case 4: case 5: case 7: sw = true; break;
				default: break;                        // 0, and 6 (enters an instruction): no switch
				}
				if (sw)
				{
					cyc(51);
					poke(T.r[6], T.r[5]);
					set_bank();
				}
				cyc(36);
				if (peek(0x0236)) { finish_pop(18); return; }
				cyc(75);
				poke(L4, (uint16_t)(peek(L4) + 1));
			} while ((int16_t)peek(L4) <= 3);
			cyc(159);
			T.r[0] = (uint16_t)((peek(L0) & 7) + peek(L6));
			poke(T.r[6], zone_at(T.r[0]));
			T.r[0] = call(0x44EC, {});                 // zone_switch_d
			cyc(12);
		}
		cyc(36);
		if (peek(0x0236)) { finish_pop(18); return; }
		cyc(18);
		T.r[6] = (uint16_t)(T.r[6] + 2);
	}
	else
		cyc(18);                                       // JMP 4704
	cyc(78);
	T.r[0] = peek(0x01A8) & ~0x01FF;
	if (T.r[0] == peek(0x0F54))
	{
		cyc(18);
		finish();
		return;
	}
	cyc(219);
	T.r[5] = (uint16_t)(peek(0x01A8) - peek(0x0F54));
	poke(0x0F54, T.r[0]);
	poke(L6, (uint16_t)(T.r[4] << 3));
	if ((int16_t)T.r[5] < 0)
	{
		cyc(39);
		if (!((int16_t)peek(L2) > 0))
		{
			cyc(84 + 66);
			T.r[0] = (uint16_t)((T.r[4] << 3) + T.r[3]);
			push(zone_at(T.r[0]));
			T.r[0] = call(0x43F8, {});                 // zone_switch_a
			cyc(18);
			T.r[6] = (uint16_t)(T.r[6] + 2);
		}
		cyc(75);
		T.r[2] = (uint16_t)(T.r[3] + peek(L6));
		poke(L4, 5);
		do
		{
			cyc(168);
			T.r[0] = (uint16_t)(T.r[2] + 8);
			T.r[2] = T.r[0] & 0x003F;
			T.r[5] = zone_at(T.r[2]);
			T.r[0] = (uint16_t)((T.r[5] & 7) << 1);
			const uint16_t mode = T.r[5] & 7;
			bool sw = false;
			switch (mode)
			{
			case 1: case 3: case 5: sw = true; break;
			case 2: case 4: case 7: cyc(42); sw = (int16_t)peek(0x0234) < 14; break;
			default: break;                            // 0: no switch, 6: also skips the check
			}
			if (sw)
			{
				cyc(54);
				push(T.r[5]);
				set_bank();
				cyc(18);
				T.r[6] = (uint16_t)(T.r[6] + 2);
			}
			if (mode != 6)
			{
				cyc(36);
				if (peek(0x0236)) { finish(); return; }
			}
			cyc(42);
			poke(L4, (uint16_t)(peek(L4) - 1));
		} while (peek(L4));
		cyc(39);
		if ((int16_t)peek(L2) < 0) { finish(); return; }
		cyc(186);
		T.r[0] = (uint16_t)((((T.r[4] + 6) << 3) & 0x0038) + T.r[3]);
		push(zone_at(T.r[0]));
		T.r[0] = call(0x4496, {});                     // zone_switch_c
		finish_pop(12);
		return;
	}
	cyc(36);
	cyc(peek(0x0234) ? 18 : 30);
	cyc(81);
	T.r[0] = (uint16_t)((peek(0x0234) ? 4 : 3) + T.r[3]);
	T.r[1] = T.r[0] & 7;
	if (!((int16_t)peek(L2) > 0))
	{
		cyc(150);
		T.r[0] = (uint16_t)((T.r[4] << 3) + T.r[1]);
		push(zone_at(T.r[0]));
		T.r[0] = call(0x4442, {});                     // zone_switch_b
		cyc(36);
		if (peek(0x0236)) { finish_pop(0); return; }
		cyc(18);
		T.r[6] = (uint16_t)(T.r[6] + 2);
	}
	cyc(57);
	T.r[2] = (uint16_t)(T.r[1] + peek(L6));
	T.r[3] = 5;
	do
	{
		cyc(168);
		T.r[0] = (uint16_t)(T.r[2] + 8);
		T.r[2] = T.r[0] & 0x003F;
		T.r[5] = zone_at(T.r[2]);
		T.r[0] = (uint16_t)((T.r[5] & 7) << 1);
		bool sw = false;
		switch (T.r[5] & 7)
		{
		case 1: case 3:
			cyc(42);
			if ((int16_t)peek(0x0234) > 2) sw = true;
			else
			{
				cyc(36);
				sw = peek(0x0234) == 0;
			}
			break;
		case 2: case 4: case 5: sw = true; break;
		default: break;                                // 0, and 6/7 (outside the routine): no switch
		}
		if (sw)
		{
			cyc(54);
			push(T.r[5]);
			set_bank();
			cyc(18);
			T.r[6] = (uint16_t)(T.r[6] + 2);
		}
		cyc(36);
		if (peek(0x0236)) { finish(); return; }
		cyc(18);
	} while (--T.r[3]);
	cyc(39);
	if ((int16_t)peek(L2) < 0) { finish(); return; }
	cyc(174);
	T.r[4] = (uint16_t)((((T.r[4] + 6) << 3) & 0x0038) + T.r[1]);
	push(zone_at(T.r[4]));
	T.r[0] = call(0x44EC, {});                         // zone_switch_d
	finish_pop(0);
}

// ---------------------------------------------------------------- terrain events

// A7:538E: the terrain event 0x032A (1-25, jump table at 0x55AC) the skater has reached:
// shops and park entrances (1-8, 20-23), jumps (11/18: sound and F:B602 into the air),
// tickets (17), flags 0x0F27/0x0F28 (9/10), 0x0ED4 (13/14) and others.  Values above 25 jump
// into the next routine in the original and are not used; they do nothing here.  R1 is
// preserved; R0 is returned as the original leaves it.
uint16_t terrain_event(const Args &)
{
	const uint16_t saved_r1 = T.r[1];
	cyc(60);
	T.r[0] = sext8(peekb(0x032A));
	if ((int16_t)T.r[0] <= 0)
	{
		cyc(24);
		return T.r[0];
	}
	cyc(51);
	const uint16_t ev = T.r[0];
	T.r[0] = (uint16_t)((T.r[0] - 1) << 1);
	switch (ev)
	{
	case 1: case 2: case 3: case 4:
	{
		cyc(30);
		T.r[0] = call(0x5360, {});
		cyc(102);
		pokeb(0x027A, (uint8_t)T.r[0]);
		T.r[1] = T.r[0] & 0x00FF;
		if (peekb(0x0EA4) & rb((uint16_t)(0xFC74 + T.r[1])))
		{
			cyc(24);
			break;
		}
		cyc(105);
		T.r[0] = (peek(0x0ED6) | peekb(0x0F29)) & 0x00FF;
		if (T.r[0] & rb((uint16_t)(0xFC74 + T.r[1])))
		{
			cyc(24);
			break;
		}
		bool other_park = false;
		cyc(36);
		if (peekb(0x01F6))
		{
			cyc(42);
			if (peekb(0x0F2B) == 1)
			{
				cyc(60);
				T.r[0] = peekb(0x02B8);
				other_park = T.r[1] != T.r[0];
			}
		}
		if (other_park)
		{
			cyc(120);
			push(0x75F4);
			push(0x00B4);
			push(0x0001);
			T.r[0] = call(0xA974, {});
			cyc(30);
			T.r[6] = (uint16_t)(T.r[6] + 2);
		}
		else
		{
			cyc(36);
			if (!peekb(0x025B))
			{
				cyc(177);
				pokeb(0x0EA6, 1);
				poke(0x0EAA, 0x00B4);
				push((uint16_t)(T.r[1] + 4));
				push(0x0001);
				T.r[0] = call(0xD97C, {});
				cyc(30);
				T.r[0] = call(0x9152, {}) & 0x0003;    // next_random
				cyc(105);
				poke(T.r[6], rb((uint16_t)(0xFE2A + T.r[0])));
				T.r[0] = call(0x8E78, {});             // queue_sound
				cyc(12);
			}
			else
			{
				cyc(60);
				push(0xFFFF);
				T.r[0] = call(0x72CA, {});
				cyc(168);
				pokeb(0x0279, (uint8_t)(peekb(0x0279) | 1));
				poke(T.r[6], (uint16_t)(peekb(0x027A) + 4));
				push(0x0000);
				T.r[0] = call(0xD97C, {});
				cyc(84);
				poke(T.r[6], peekb(0x027A));
				T.r[0] = call(0xDA02, {});
				cyc(30);
				T.r[0] = call(0x50C8, {});
			}
		}
		cyc(114 + 12);
		T.r[0] = peekb(0x027A);
		pokeb(0x0ED6, (uint8_t)(peekb(0x0ED6) | rb((uint16_t)(0xFC74 + T.r[0]))));
		T.r[6] = (uint16_t)(T.r[6] + 4);
		break;
	}
	case 5: case 6: case 7: case 8:
		cyc(96);
		T.r[1] = (sext8(peekb(0x0ED7)) | peekb(0x0ED8)) & 0x00FF;
		T.r[0] = call(0x5326, {});
		cyc(39);
		if (!(T.r[1] & rb((uint16_t)(0xFC74 + T.r[0]))))
		{
			cyc(45);
			pokeb(0x0ED7, (uint8_t)(peekb(0x0ED7) | 0x10));
		}
		cyc(12);
		break;
	case 9:
		cyc(57);
		pokeb(0x0F27, 1);
		break;
	case 10:
		cyc(57);
		pokeb(0x0F28, 1);
		break;
	case 11: case 18:
		cyc(36);
		if (!peekb(0x01EA))
		{
			cyc(39);
			poke(0x0FB2, (uint16_t)(peek(0x0FB2) + 1));
		}
		else
		{
			cyc(42);
			if (peekb(0x031F) != 5)
			{
				cyc(60);
				push(0x0053);
				T.r[0] = call(0x8E78, {});             // queue_sound
				cyc(102);
				T.r[0] = 0x1C00;
				set_bank_b(0x1C00);
				T.r[0] = call(0x78E4, {});
				cyc(42);
				if (peekb(0x032A) == 0x12)
				{
					cyc(57);
					poke(T.r[6], 0x0069);
					T.r[0] = call(0x8E78, {});
					cyc(45);
					poke(0x03B4, 0x57A8);
				}
				else
				{
					cyc(57);
					poke(T.r[6], 0x006A);
					T.r[0] = call(0x8E78, {});
					cyc(33);
					poke(0x03B4, 0x57AA);
				}
				cyc(120);
				poke(0x03B2, 0x50FA);
				poke(T.r[6], 0x0002);
				push(0x0012);
				T.r[0] = call(0xB602, {});
				cyc(24);
				T.r[6] = (uint16_t)(T.r[6] + 4);
			}
		}
		cyc(39);
		pokeb(0x032A, 0);
		break;
	case 13:
		cyc(45);
		poke(0x0ED4, 0x7800);
		break;
	case 14:
		cyc(45);
		poke(0x0ED4, 0x5800);
		break;
	case 16:
		cyc(72);
		T.r[0] = (uint16_t)(peek(0x0F32) + 1);
		if ((int16_t)T.r[0] < (int16_t)peek(0x099A))
		{
			cyc(69);
			poke(0x0F32, peek(0x099A));
			T.r[0] = call(0x7A9E, {});
			cyc(12);
		}
		break;
	case 17:
		cyc(36);
		if (!peekb(0x01EA)) break;
		cyc(42);
		if (peekb(0x018F) > 1) break;
		cyc(30);
		T.r[0] = call(0x52FC, {});                     // difficulty_step
		cyc(81);
		T.r[0] = rb((uint16_t)(0xFC74 + T.r[0]));
		if (peekb(0x0F2A) & T.r[0]) break;
		cyc(129 + 12);
		pokeb(0x0F36, 1);
		T.r[1] = sext8(peekb(0x0F2A)) | T.r[0];
		pokeb(0x0F2A, (uint8_t)T.r[1]);
		pokeb(0x0F2A, (uint8_t)(peekb(0x0F2A) | 0x80));
		break;
	case 19:
		cyc(36);
		if (!peekb(0x01EA)) break;
		cyc(42);
		if (peekb(0x0F26) >= 5) break;
		cyc(42);
		if (peekb(0x031F) == 5) break;
		cyc(102);
		T.r[0] = 0x1C00;
		set_bank_b(0x1C00);
		T.r[0] = call(0x78E4, {});
		cyc(12);
		break;
	case 20: case 21: case 22: case 23:
		cyc(30);
		T.r[0] = call(0x5360, {});
		cyc(114);
		pokeb(0x027A, (uint8_t)T.r[0]);
		T.r[0] = peekb(0x027A);
		if (!(peekb(0x0EA4) & rb((uint16_t)(0xFC74 + T.r[0])))) break;
		cyc(60);
		push(0x0053);
		T.r[0] = call(0x8E78, {});
		cyc(51);
		pokeb(0x0F26, 1);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		break;
	default:                                               // 12, 15, 24, 25 and unused values
		break;
	}
	T.r[1] = saved_r1;
	return T.r[0];
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
	const uint16_t E = T.r[6];
	const uint16_t L0 = (uint16_t)(E - 6), L2 = (uint16_t)(E - 4), L4 = (uint16_t)(E - 2);
	const uint16_t OX = (uint16_t)(E - 8), SX = (uint16_t)(E - 10), S0 = (uint16_t)(E - 12);
	T.r[6] = L0;
	auto shift = [&](uint16_t mask, uint16_t n) { T.r[0] = mask; T.r[1] = n; call(0x7DB8, {}); };
	auto byte_shift = [&](uint16_t a, uint16_t v, int cost) {   // MOV/CLRB/SWAB/BPL [COMB]
		cyc(cost);
		if (v & 0x8000) cyc(27);
		poke(a, asr(v, 8));
	};
	cyc(333);
	T.r[0] = peek((uint16_t)(E + 4));
	T.r[2] = peek((uint16_t)(E + 2));
	T.r[1] = peek((uint16_t)(0x03F0 + T.r[0]));
	T.r[3] = (uint16_t)(peek((uint16_t)(0x0630 + T.r[0])) + peek(T.r[1]));
	push(T.r[3]);                                      // OX: object left
	T.r[4] = peek(0x023A);
	push(peek(T.r[4]));
	poke(SX, (uint16_t)(peek(SX) + peek(0x0630)));     // SX: skater left
	push(T.r[3]);
	poke(S0, (uint16_t)(peek(S0) - peek(SX)));
	T.r[3] = peek(S0);
	byte_shift(0x0234, peek((uint16_t)(T.r[1] + 4)), 108);
	byte_shift(0x0236, peek((uint16_t)(T.r[1] + 6)), 108);
	poke(0x0236, (uint16_t)(peek(0x0236) << 1));
	cyc(357);
	poke(L4, (uint16_t)(peek((uint16_t)(T.r[4] + 2)) + peek(0x0238)));   // skater top
	T.r[5] = (uint16_t)(peek(L4) + peek((uint16_t)(T.r[4] + 6)));
	const uint16_t oy = (uint16_t)(peek((uint16_t)(0x0670 + T.r[0])) + peek((uint16_t)(T.r[1] + 2)));
	ww((uint16_t)(E - 14), oy);                        // pushed and popped
	poke(L2, oy);                                      // object top
	T.r[0] = (uint16_t)(peek(L2) + peek((uint16_t)(T.r[1] + 6)));
	if (!((int16_t)T.r[5] >= (int16_t)T.r[0]))
	{
		cyc(12);
		T.r[0] = T.r[5];
	}
	cyc(138);
	poke(0x0230, T.r[0]);
	poke(L0, peek(L4));
	T.r[0] = (uint16_t)(peek(L0) - peek(L2));
	if (!(T.r[0] & 0x8000))
	{
		T.r[3] = (uint16_t)(T.r[4] - 2);
		poke(L4, 0);
		T.r[5] = (uint16_t)(T.r[0] + 0x0100);
		T.r[0] = asr(T.r[5], 8);
		T.r[5] = (uint16_t)(T.r[0] << 1);
		T.r[2] = (uint16_t)(T.r[1] - T.r[5]);
		T.r[0] = (uint16_t)(peek(0x0230) - peek(L0) + 0x00C0);
		byte_shift(0x0230, T.r[0], 348);
		cyc(45);
		poke(0x0232, peek(S0));
	}
	else
	{
		T.r[3] = (uint16_t)(T.r[1] - 2);
		poke(L4, 1);
		T.r[5] = (uint16_t)(0x0100 - T.r[0]);
		T.r[0] = (uint16_t)(asr(T.r[5], 8) << 1);
		T.r[2] = (uint16_t)(T.r[4] - T.r[0]);
		T.r[0] = (uint16_t)(peek(0x0230) - peek(L2) + 0x00C0);
		byte_shift(0x0230, T.r[0], 342);
		cyc(84);
		poke(0x0232, (uint16_t)(peek(SX) - peek(OX)));
	}
	cyc(78);
	T.r[0] = peek(0x0232);
	T.r[4] = peek(0x0234);
	bool hit = false;
	if (!((int16_t)T.r[4] > 0x10))
	{
		cyc(24);
		const bool left = (int16_t)T.r[0] < 0;
		if (left)
		{
			T.r[1] = (uint16_t)-T.r[0];
			byte_shift(0x0232, T.r[1], 117);
		}
		else
			byte_shift(0x0232, T.r[0], 93);
		cyc(36);
		T.r[5] = peek(0x0230);
		if ((int16_t)T.r[5] > 0)
			do
			{
				cyc(72);
				if (left)
				{
					shift(peek(T.r[2]), peek(0x0232));
					cyc(30);
					hit = (peek(T.r[3]) & T.r[0]) != 0;
				}
				else
				{
					shift(peek(T.r[3]), peek(0x0232));
					cyc(30);
					hit = (T.r[0] & peek(T.r[2])) != 0;
				}
				if (hit)
				{
					cyc(12);                           // 595C BR / 59B0 BNE
					break;
				}
				cyc(54);
				T.r[3] = (uint16_t)(T.r[3] - 2);
				T.r[2] = (uint16_t)(T.r[2] - 2);
			} while (--T.r[5]);
		if (!hit) cyc((left && (int16_t)peek(0x0230) > 0 ? 12 : 0) + 18);   // BR 58C8 / JMP 5A3E
	}
	else
	{
		poke(OX, T.r[3]);
		poke(L0, T.r[2]);
		byte_shift(0x0232, T.r[0], 153);
		cyc(93);
		T.r[0] = peek(0x0232);
		T.r[1] = (uint16_t)(T.r[4] - 0x0010);
		if (peek(L4))
		{
			cyc(54);
			T.r[5] = (uint16_t)(T.r[0] - T.r[1]);
			if ((int16_t)T.r[4] > 0x20)
			{
				cyc(60);
				poke(L2, T.r[5]);
				T.r[5] = (uint16_t)(T.r[5] + 0x0010);
			}
		}
		else
		{
			cyc(54);
			T.r[5] = (uint16_t)(T.r[1] + T.r[0]);
			if ((int16_t)T.r[4] > 0x20)
			{
				cyc(48);
				poke(L2, T.r[5]);
				T.r[5] = (uint16_t)(T.r[5] - 0x0010);
			}
		}
		cyc(24);
		if ((int16_t)T.r[0] < 0)
		{
			cyc(12);
			T.r[0] = (uint16_t)-T.r[0];
		}
		cyc(30);
		if ((int16_t)T.r[0] < 0x10)
		{
			cyc(30);
			if ((int16_t)T.r[4] > 0x20)
			{
				cyc(54);
				poke(S0, (uint16_t)(peek(0x0236) << 1));
			}
			else
			{
				cyc(45);
				poke(S0, peek(0x0236));
			}
			cyc(54);
			poke(SX, peek(0x0230));
			if ((int16_t)peek(SX) > 0)
				do
				{
					cyc(39);
					if (peek(L4))
					{
						cyc(102);
						T.r[4] = (uint16_t)(T.r[3] - peek(S0));
						shift(peek(T.r[4]), peek(0x0232));
						cyc(30);
						hit = (T.r[0] & peek(T.r[2])) != 0;
					}
					else
					{
						cyc(72);
						shift(peek(T.r[3]), peek(0x0232));
						cyc(60);
						T.r[1] = T.r[0];
						T.r[0] = (uint16_t)(T.r[2] - peek(S0));
						hit = (T.r[1] & peek(T.r[0])) != 0;
					}
					cyc(12);                           // 595A BEQ
					if (hit)
					{
						cyc(12);
						break;
					}
					cyc(78);
					T.r[3] = (uint16_t)(T.r[3] - 2);
					T.r[2] = (uint16_t)(T.r[2] - 2);
					poke(SX, (uint16_t)(peek(SX) - 1));
				} while (peek(SX));
			if (!hit)
			{
				cyc(54);
				T.r[3] = peek(OX);
				T.r[2] = peek(L0);
			}
		}
		if (!hit)
		{
			cyc(24 + 24 + 30);
			T.r[0] = (int16_t)T.r[5] < 0 ? (uint16_t)-T.r[5] : T.r[5];
			if ((int16_t)T.r[0] < 0x10)
			{
				cyc(42);
				if ((int16_t)peek(0x0234) > 0x20)
				{
					cyc(54);
					poke(SX, peek(0x0230));
					if ((int16_t)peek(SX) > 0)
						do
						{
							cyc(39);
							if (peek(L4))
							{
								cyc(96);
								T.r[4] = (uint16_t)(T.r[3] - peek(0x0236));
								shift(peek(T.r[4]), T.r[5]);
								cyc(30);
								hit = (T.r[0] & peek(T.r[2])) != 0;
							}
							else
							{
								cyc(60);
								shift(peek(T.r[3]), T.r[5]);
								cyc(66);
								T.r[1] = T.r[0];
								T.r[0] = (uint16_t)(T.r[2] - peek(0x0236));
								hit = (T.r[1] & peek(T.r[0])) != 0;
							}
							cyc(12);                   // 59DE BEQ
							if (hit)
							{
								cyc(12);
								break;
							}
							cyc(78);
							T.r[3] = (uint16_t)(T.r[3] - 2);
							T.r[2] = (uint16_t)(T.r[2] - 2);
							poke(SX, (uint16_t)(peek(SX) - 1));
						} while (peek(SX));
				}
				else
				{
					cyc(36);
					T.r[4] = peek(0x0230);
					if ((int16_t)T.r[4] > 0)
					{
						do
						{
							cyc(60);
							shift(peek(T.r[3]), T.r[5]);
							cyc(30);
							hit = (T.r[0] & peek(T.r[2])) != 0;
							if (hit) break;
							cyc(54);
							T.r[3] = (uint16_t)(T.r[3] - 2);
							T.r[2] = (uint16_t)(T.r[2] - 2);
						} while (--T.r[4]);
						if (!hit) cyc(12);             // BR 5A02
					}
				}
				if (!hit)
				{
					cyc(54);
					T.r[3] = peek(OX);
					T.r[2] = peek(L0);
				}
			}
		}
		if (!hit)
		{
			cyc(45);
			if ((int16_t)peek(L2) > (int16_t)0xFFF0)
			{
				cyc(42);
				if ((int16_t)peek(0x0234) > 0x20)
				{
					cyc(36);
					T.r[4] = peek(0x0230);
					if ((int16_t)T.r[4] > 0)
						do
						{
							cyc(75);
							shift(peek(T.r[3]), peek(L2));
							cyc(30);
							hit = (T.r[0] & peek(T.r[2])) != 0;
							if (hit) break;
							cyc(54);
							T.r[3] = (uint16_t)(T.r[3] - 2);
							T.r[2] = (uint16_t)(T.r[2] - 2);
						} while (--T.r[4]);
				}
			}
		}
	}
	if (hit)
	{
		cyc(30);                                       // MOV #1,R0; BR
		T.r[0] = 1;
	}
	else
	{
		cyc(12);                                       // CLR R0
		T.r[0] = 0;
	}
	T.r[6] = fl::add(T.r[6], 0x000C);
}

// ---------------------------------------------------------------- sound CPU exchanges
// These wait for the sound CPU (F:8ED2 read_sound_response polls), so the per-call tester
// gives up on them; they are checked with whole runs (tools/compare_runs.py).

uint16_t sound_byte() { return call(0x8ED2, {}); }    // read_sound_response
uint8_t sound_byte_after(int cycles) { cyc(cycles); return (uint8_t)sound_byte(); }

// A7:4B00  reset_sound_cpu(): up to 5 times: read the response port (and the byte it points
// to), hold the sound CPU in reset for 201 passes of a delay loop and wait for its 0xFF
// greeting (0x01A3 cleared then).  If it never answers the game is restarted (F:8000, not
// reached by the tests).
uint16_t reset_sound_cpu(const Args &)
{
	c_save(1, 0x4B04);
	pokeb(0x01A2, 0);
	tick(0x4B08, 27);
	T.r[1] = 5;
	tick(0x4B0C, 18);
	for (;;)
	{
		T.r[0] = peek(0x1C00);
		tick(0x4B10, 24);
		T.r[2] = 0;
		tick(0x4B12, 12);
		T.r[2] = peekb(T.r[0]);
		tick(0x4B14, 18);
		poke(0x15A0, 0x0001);                          // sound CPU held in reset
		tick(0x4B1A, 33);
		T.r[0] = 0x00C9;
		tick(0x4B1E, 18);
		for (;;)
		{
			T.r[2]++;
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
		tick(0x4B2E, 18);                              // CMP R0,#0x00FF
		if (T.r[0] == 0x00FF)
		{
			tick(0x4B30, 12);
			pokeb(0x01A3, 0);
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
	tick(0x4366, 30);
	jsr(0x436A, 0x8EA2);                               // send_sound_command
	jsr(0x436E, 0x8ED2);                               // read_sound_response
	jsr(0x4372, 0x8ED2);
	pokeb(0x01FA, (uint8_t)T.r[0]);
	tick(0x4376, 27);
	jsr(0x437A, 0x8ED2);
	T.r[1] = T.r[0];
	tick(0x437C, 12);
	jsr(0x4380, 0x8ED2);
	pokeb(0x01F3, (uint8_t)T.r[0]);
	tick(0x4384, 27);
	jsr(0x4388, 0x8ED2);
	pokeb(0x01F4, (uint8_t)T.r[0]);
	tick(0x438C, 27);
	jsr(0x4390, 0x8F62);
	T.r[0] = T.r[1];
	tick(0x4392, 12);
	T.r[0] &= 0x007C;
	tick(0x4396, 18);
	T.r[2] = 0;
	tick(0x4398, 12);
	T.r[2] = peekb(0x01F9);
	tick(0x439C, 24);
	T.r[2] &= 0x007C;
	tick(0x43A0, 18);
	tick(0x43A2, 12);                                  // CMP R0,R2
	if ((int16_t)T.r[0] > (int16_t)T.r[2])
	{
		tick(0x43A4, 12);
		poke(T.r[6], 0x0068);
		tick(0x43A8, 27);
		jsr(0x43AC, 0x8EA2);                           // the coin sound
	}
	else
		tick(0x43AC, 12);                              // BLE
	pokeb(0x01F9, (uint8_t)T.r[1]);
	tick(0x43B0, 27);
	jsr(0x43B4, 0x8E72);                               // clear_flag_01a4
	T.r[6] = (uint16_t)(T.r[6] + 2);
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
	tick(0x4482, 30);
	jsr(0x4486, 0x8EA2);                               // send_sound_command
	T.r[2] = 0;
	tick(0x4488, 12);
	for (;;)
	{
		T.r[0] = T.r[2];
		tick(0x448A, 12);
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(0x448C, 12);
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(0x448E, 12);
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(0x4490, 12);
		T.r[1] = T.r[0];
		tick(0x4492, 12);
		poke((uint16_t)(0x0370 + T.r[1]), 0);
		tick(0x4496, 30);
		tick(0x449A, 18);                              // CMP R2,#14
		if ((int16_t)T.r[2] <= 14)
		{
			tick(0x449C, 12);
			jsr(0x44A0, 0x8ED2);                       // read_sound_response
			pokeb((uint16_t)(0x0370 + T.r[1]), (uint8_t)T.r[0]);
			tick(0x44A4, 30);
			jsr(0x44A8, 0x8ED2);
			pokeb((uint16_t)(0x0373 + T.r[1]), (uint8_t)T.r[0]);
			tick(0x44AC, 30);
			jsr(0x44B0, 0x8ED2);
			pokeb((uint16_t)(0x0372 + T.r[1]), (uint8_t)T.r[0]);
			tick(0x44B4, 30);
			tick(0x44CC, 12);                          // BR
		}
		else
		{
			tick(0x44B6, 12);                          // BGT
			jsr(0x44BA, 0x8ED2);
			pokeb((uint16_t)(0x0372 + T.r[1]), (uint8_t)T.r[0]);
			tick(0x44BE, 30);
			jsr(0x44C2, 0x8ED2);
			pokeb((uint16_t)(0x0373 + T.r[1]), (uint8_t)T.r[0]);
			tick(0x44C6, 30);
			pokeb((uint16_t)(0x0373 + T.r[1]), peekb((uint16_t)(0x0373 + T.r[1])) & 0x01);
			tick(0x44CC, 36);
		}
		jsr(0x44D0, 0x8ED2);
		pokeb((uint16_t)(0x0374 + T.r[1]), (uint8_t)T.r[0]);
		tick(0x44D4, 30);
		jsr(0x44D8, 0x8ED2);
		pokeb((uint16_t)(0x0375 + T.r[1]), (uint8_t)T.r[0]);
		tick(0x44DC, 30);
		poke((uint16_t)(0x0376 + T.r[1]), 0);
		tick(0x44E0, 30);
		jsr(0x44E4, 0x8ED2);
		pokeb((uint16_t)(0x0376 + T.r[1]), (uint8_t)T.r[0]);
		tick(0x44E8, 30);
		tick(0x44EC, 18);                              // CMP R2,#14
		if (T.r[2] == 14)
		{
			tick(0x44EE, 12);
			T.r[1] = 0x001E;
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
		T.r[2]++;
		tick(0x44FA, 12);
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
	tick(0x4522, 30);
	jsr(0x4526, 0xA9FC);
	poke(T.r[6], 0x7718);
	tick(0x452A, 27);
	jsr(0x452E, 0xA9FC);
	poke(T.r[6], 0x771E);
	tick(0x4532, 27);
	jsr(0x4536, 0xA9FC);
	T.r[0] = 0x1C00;
	tick(0x453A, 18);
	poke(0x01D4, T.r[0]);
	tick(0x453E, 27);
	poke(0x1402, T.r[0]);
	tick(0x4542, 27);
	T.r[4] = 0;
	tick(0x4544, 12);
	for (;;)
	{
		T.r[0] = T.r[4];
		tick(0x4546, 12);
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(0x4548, 12);
		poke(0x0186, peek((uint16_t)(0x7F7C + T.r[0])));
		tick(0x454E, 42);
		poke(0x0186, (uint16_t)(peek(0x0186) + 0x2000));
		tick(0x4554, 33);
		T.r[0] = T.r[4];
		tick(0x4556, 12);
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(0x4558, 12);
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(0x455A, 12);
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(0x455C, 12);
		T.r[2] = T.r[0];
		tick(0x455E, 12);
		T.r[3] = 0x0004;
		tick(0x4562, 18);
		for (;;)
		{
			T.r[1] = T.r[3];
			tick(0x4564, 12);
			T.r[1] = (uint16_t)(T.r[1] + T.r[2]);
			tick(0x4566, 12);
			T.r[0] = 0;
			tick(0x4568, 12);
			T.r[0] = peekb((uint16_t)(0x0370 + T.r[1]));
			tick(0x456C, 27);
			jsr(0x4570, 0x8C12);                       // put_small_char
			T.r[3]++;
			tick(0x4572, 12);
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
		tick(0x4582, 36);
		push(peek((uint16_t)(0x0370 + T.r[2])));
		tick(0x4586, 39);
		push(0x0007);
		tick(0x458A, 30);
		push(0x0001);
		tick(0x458E, 30);
		push(0);
		tick(0x4590, 24);
		jsr(0x4594, 0xBA96);                           // print the score
		T.r[6] = (uint16_t)(T.r[6] + 8);
		tick(0x4598, 18);
		T.r[4]++;
		tick(0x459A, 12);
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
	tick(0x45A8, 27);
	poke(0x1402, T.r[0]);
	tick(0x45AC, 27);
	T.r[6] = (uint16_t)(T.r[6] + 2);
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
	const uint16_t L = (uint16_t)(E - 2);
	cyc(153);
	push(peek(L));                                     // TST -(SP): the counter slot
	push(peek(0x01D2));
	push(peek(0x01D4));
	T.r[2] = peekb(0x01F6);
	for (T.r[1] = 0;; T.r[1]++)
	{
		cyc(24);
		if (!((int16_t)T.r[1] <= (int16_t)T.r[2])) break;
		cyc(123);
		push(peekb(0x01F3) & 3);
		push(0x0005);
		T.r[0] = call(0x80D8, {});                     // mul16
		cyc(195);
		pokeb(0x0FE3, (uint8_t)T.r[0]);
		T.r[0] = (uint16_t)(peekb(0x01F3) >> 2);
		poke(T.r[6], T.r[0] & 3);
		push(0x000B);
		T.r[0] = call(0x80D8, {});                     // mul16
		cyc(771);
		pokeb(0x0FE4, (uint8_t)T.r[0]);
		T.r[0] = sext8(peekb(0x01F4)) & 3;
		T.r[0] = peekb((uint16_t)(0x523A + T.r[0]));
		pokeb(0x025B, (uint8_t)T.r[0]);
		pokeb(0x0FE5, (uint8_t)T.r[0]);
		pokeb(0x025A, 0);
		T.r[0] = 0x3800;
		poke(0x01D2, 0x3800);
		poke(0x1400, 0x3800);
		T.r[0] = (uint16_t)(peekb(0x0FE3) << 1);
		poke(0x0244, peek((uint16_t)(0x5212 + T.r[0])));
		poke(0x0246, 0);
		poke(0x0248, peek((uint16_t)(0x5212 + T.r[0])));
		poke(0x024A, 0);
		poke(0x024C, 0);
		poke(0x024E, 0);
		poke(0x023E, 0);
		poke(0x0240, 0);
		poke(0x0256, 0x0064);
		pokeb(0x0F30, 0);
		pokeb(0x0F31, 0);
		pokeb(0x027B, 1);
		T.r[0] = 0x2800;
		set_bank_b(0x2800);
		for (T.r[0] = 4; (int16_t)T.r[0] >= 0; T.r[0]--)
		{
			cyc(69);
			pokeb((uint16_t)(0x0274 + T.r[0]), peekb((uint16_t)(0x7C7A + T.r[0])));
		}
		cyc(168);
		pokeb(0x025F, 0);
		pokeb(0x025C, 0);
		pokeb(0x025E, 0);
		pokeb(0x025D, 0);
		pokeb((uint16_t)(0x0F3A + T.r[1]), 0);
		T.r[0] = call(0x9152, {});                     // next_random
		cyc(63);
		pokeb(0x0261, (uint8_t)T.r[0]);
		if (peekb(0x01F6))
		{
			cyc(30);
			call(0xE730, {});
		}
		cyc(30);
		T.r[6] = (uint16_t)(T.r[6] + 6);
	}
	cyc(327);
	poke(0x020A, (uint16_t)(peek(0x020A) + 1));
	for (uint16_t a : {0x0F29, 0x0F2A, 0x0ED6, 0x0EA4, 0x0F37, 0x0ED7, 0x0ED8, 0x0279, 0x0272}) pokeb(a, 0);
	poke(0x01CE, 0);
	call(0xBA2E, {});
	cyc(30);
	call(0xCFD6, {});
	cyc(54);
	push(0x0000);
	call(0xDC24, {});                                  // wait_fade(0)
	cyc(63);
	poke(0x0EB0, 0xDABA);
	call(0x8D50, {});                                  // wait_vblank
	cyc(63);
	poke(0x0EB0, 0xDA74);
	call(0x8D50, {});
	cyc(30);
	call(0x8E6A, {});                                  // set_flag_01a4
	cyc(36);
	poke(L, 0x0020);
	do
	{
		cyc(30);
		call(0x910C, {});
		cyc(42);
		poke(L, (uint16_t)(peek(L) - 1));
	} while (peek(L));
	cyc(30);
	call(0x8E72, {});                                  // clear_flag_01a4
	cyc(201);                                          // banks restored, ADD #8,SP, RTS
	poke(0x01D2, peek((uint16_t)(E - 4)));
	poke(0x01D4, peek((uint16_t)(E - 6)));
	poke(0x1400, peek(0x01D2));
	poke(0x1402, peek(0x01D4));
	T.r[6] = fl::add(T.r[6], 0x0008);
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
	tick(0x5846, 24);
	poke(0x0222, 0x0001);
	tick(0x584C, 33);
	const bool air = peekb(0x01EA) != 0;
	tick(0x5850, 24);                                  // TSTB @#0x01EA
	bool wall = false;
	if (air)
		tick(0x589A, 12);                              // BNE
	else
	{
		tick(0x5852, 12);
		wall = a.u(0) == 0x000F;
		tick(0x5858, 33);                              // CMP 4(SP),#15
		tick(wall ? 0x585A : 0x589A, 12);              // BNE
	}
	if (wall)
	{
		T.r[0] = peek(0x0570);
		tick(0x585E, 24);
		T.r[0] = (uint16_t)(T.r[0] << 1);
		tick(0x5860, 12);
		T.r[0] = (uint16_t)(T.r[0] - peek(0x04F0));
		tick(0x5864, 24);
		tick(0x5866, 18);                              // CCC CV
		if ((T.r[0] & 0x8000) || !T.r[0])
			tick(0x58CC, 12);                          // BLE
		else
		{
			tick(0x5868, 12);
			T.r[0] = peek(0x04F0);
			tick(0x586C, 24);
			T.r[0] = asr(T.r[0], 1);
			tick(0x586E, 12);
			T.r[0] = (uint16_t)-T.r[0];
			tick(0x5870, 12);
			poke(0x04F0, T.r[0]);
			tick(0x5874, 27);
			T.r[0] = peek(0x0570);
			tick(0x5878, 24);
			T.r[0] = asr(T.r[0], 1);
			tick(0x587A, 12);
			T.r[0] = (uint16_t)-T.r[0];
			tick(0x587C, 12);
			poke(0x0570, T.r[0]);
			tick(0x5880, 27);
			T.r[0] = 0x0004;
			tick(0x5884, 18);
			T.r[1] = sext8(peekb(0x031D));
			tick(0x5888, 24);
			T.r[0] = (uint16_t)(T.r[0] - T.r[1]);
			tick(0x588A, 12);
			T.r[1] = T.r[0];
			tick(0x588C, 12);
			T.r[1] &= 0x000F;
			tick(0x5890, 18);
			pokeb(0x031D, 0);
			tick(0x5894, 27);
			pokeb(0x031D, (uint8_t)(peekb(0x031D) | T.r[1]));
			tick(0x5898, 27);
			tick(0x58CC, 12);                          // BR
		}
	}
	else
	{
		T.r[0] = a.u(0);
		tick(0x589E, 27);
		pokeb(0x0329, (uint8_t)T.r[0]);
		tick(0x58A2, 27);
		T.r[1] = 0;
		tick(0x58A4, 12);
		T.r[1] = peekb(0x0328);
		tick(0x58A8, 24);
		tick(0x58AA, 12);                              // CMP R1,R0
		if (T.r[1] == T.r[0])
			tick(0x58CC, 12);                          // BEQ
		else
		{
			tick(0x58AC, 12);
			pokeb(0x0328, (uint8_t)T.r[0]);
			tick(0x58B0, 27);
			push(T.r[0]);
			tick(0x58B2, 24);
			jsr(0x58B6, 0x9E52);                       // slope_table_value
			const bool in_air = peekb(0x01EA) != 0;
			tick(0x58BA, 24);                          // TSTB @#0x01EA
			if (in_air)
			{
				tick(0x58BC, 12);
				poke(T.r[6], T.r[0]);
				tick(0x58BE, 21);
				jsr(0x58C2, 0x9EEE);
				tick(0x58CA, 12);                      // BR
			}
			else
			{
				tick(0x58C4, 12);                      // BEQ
				poke(T.r[6], T.r[0]);
				tick(0x58C6, 21);
				jsr(0x58CA, 0x9E96);
			}
			T.r[6] = (uint16_t)(T.r[6] + 2);
			tick(0x58CC, 18);                          // TST (SP)+
		}
	}
	T.r[1] = pop();
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
	cyc(540);
	T.r[0] = 0x2C00;
	set_bank_b(0x2C00);
	T.r[0] = (uint16_t)(sext8(peekb(0x02E5)) << 1);
	T.r[1] = (uint16_t)(sext8(peekb(0x02E3)) + T.r[0]);
	T.r[2] = (uint16_t)(T.r[1] << 3);
	T.r[3] = (uint16_t)(peek(0x0570) << 1);
	T.r[1] = (uint16_t)(T.r[3] + peek(0x0ED4) + peek(0x04F0));
	poke(0x0ED4, T.r[1]);
	T.r[2] = (uint16_t)(T.r[2] + T.r[1]);
	T.r[1] = sext8(peekb(0x02E3));
	T.r[0] = (uint16_t)((uint16_t)(T.r[0] - T.r[1]) << 3);
	T.r[1] = (uint16_t)(T.r[3] + peek(0x0ED2) - peek(0x04F0));
	poke(0x0ED2, T.r[1]);
	T.r[0] = (uint16_t)(T.r[0] + T.r[1]);
	T.r[3] = 0;
	bool beyond = T.r[2] > 0xB600;
	if (!beyond)
	{
		cyc(42);
		T.r[3] = 1;
		beyond = T.r[0] > 0x6C00;
		if (!beyond)
		{
			cyc(42);
			T.r[1] = 0;
			bool inside = false;
			if (T.r[2] < 0x7800)
			{
				cyc(30);
				inside = T.r[2] > 0x3800;
			}
			if (!inside)
			{
				cyc(42);
				T.r[1] = 1;
				if (T.r[2] < 0xA200)
				{
					cyc(30);
					inside = T.r[2] > 0x1400;
				}
				if (!inside)
				{
					cyc(24);
					T.r[1] = 2;
				}
			}
		}
	}
	if (beyond)
	{
		cyc(30);
		if (T.r[0] >= 0xB000)
		{
			cyc(30);
			T.r[1] = 3;
		}
		else
		{
			cyc(30);
			bool five = false;
			if (T.r[0] > 0x6C00)
			{
				cyc(24);
				five = T.r[3] & 1;                     // ROR R3 (C clear after the CMP)
				T.r[3] = (uint16_t)(T.r[3] >> 1);
				if (five)
				{
					cyc(30);
					T.r[1] = 5;
				}
			}
			if (!five)
			{
				cyc(30);
				if (T.r[2] >= 0xBE00)
				{
					cyc(30);
					T.r[1] = 4;
				}
				else
				{
					cyc(18);
					T.r[1] = 6;
				}
			}
		}
	}
	cyc(213);
	T.r[0] = (uint16_t)((peekb(0x0ED0) << 3) + T.r[1]);
	T.r[3] = peekb((uint16_t)(0x7CD6 + T.r[0]));
	T.r[1] = T.r[3] & 7;
	T.r[0] = peekb(0x0ED0);
	if (T.r[0] != T.r[1])
	{
		cyc(27);
		pokeb(0x0ED0, (uint8_t)T.r[1]);
	}
	cyc(87);
	T.r[0] = (uint16_t)(asr(T.r[3], 3) << 1);
	switch (T.r[3] >> 3)
	{
	case 1:
		cyc(78);
		poke(0x06B0, 0x1500);
		poke(0x05B2, 0xFF80);
		break;
	case 3:
		cyc(30);
		T.r[0] = call(0x9DE4, {});
		cyc(12);
		break;
	case 4: case 5: case 6: case 7:
		cyc(81 + 30);
		T.r[3] = (uint16_t)(T.r[3] - 0x0020);
		push(T.r[3] | 7);
		T.r[0] = call(0x5844, {});                     // slope_event
		cyc(18);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		break;
	case 8:
		cyc(42);
		if ((int16_t)peek(0x06B2) >= 0x2800)
		{
			cyc(111);
			pokeb(0x0328, 0x0F);
			pokeb(0x031C, 3);
			pokeb(0x0ED0, 5);
		}
		else
		{
			cyc(30 + 30);
			push(0x000F);
			T.r[0] = call(0x5844, {});                 // slope_event
			cyc(18);
			T.r[6] = (uint16_t)(T.r[6] + 2);
		}
		break;
	default:                                           // 0, 2: nothing
		break;
	}
	cyc(93);
	T.r[0] = (uint16_t)(peekb(0x0ED0) << 1);
	if (peek((uint16_t)(0x7D06 + T.r[0])) != 0x0FFF)
	{
		cyc(42);
		poke(0x06B0, peek((uint16_t)(0x7D06 + T.r[0])));
	}
	cyc(30);
	fl::cmp(T.r[1], 0x0002);
	if ((int16_t)T.r[1] > 2)
	{
		cyc(18);
		T.r[4] = 1;
	}
	else
	{
		cyc(69);
		pokeb(0x032E, 0);
		T.r[4] = 0;
		fl::cmp(T.r[1], 0x0002);
		bool set = false;
		if (T.r[1] != 2)
		{
			cyc(30);
			fl::cmp(T.r[2], 0x5800);
			set = !(T.r[2] > 0x5800);
		}
		if (set)
		{
			cyc(45);
			pokeb(0x032F, 1);
		}
		else
		{
			cyc(39);
			pokeb(0x032F, 0);
			fl::clr();
		}
	}
	cyc(33);
	T.r[0] = T.r[4];
	fl::mov(T.r[0]);
}

// A5:5A50 (assembly)  downhill_height(): in zone 1 the height 0x06B0 follows the slope 0x02E7
// (capped at 0x1500), and the sprite table 0x7C48 (bank B 0x2C00) gives the skater's frame
// (+0x10 when 0x032F); high up (0x06B2 > 0x1800) 0x0ED4 is set to 0xA200 / 0x1400 and 0x0222
// cleared.  Zones 2 and 5 use frame 0x78, others keep 0x0304.  0x0302-0x0306 get the frame.
void downhill_height()
{
	T.r[0] = peek(0x0304);
	tick(0x5A54, 24);
	T.r[2] = 0;
	tick(0x5A56, 12);
	T.r[2] = peekb(0x0ED0);
	tick(0x5A5A, 24);
	fl::cmp(T.r[2], 0x0001);
	tick(0x5A5E, 18);
	if (T.r[2] == 1)
	{
		tick(0x5A60, 12);
		T.r[1] = 0x2C00;
		tick(0x5A64, 18);
		poke(0x01D4, T.r[1]);
		tick(0x5A68, 27);
		poke(0x1402, T.r[1]);
		tick(0x5A6C, 27);
		const bool high = (int16_t)peek(0x06B0) >= 0x1500;
		tick(0x5A72, 30);                              // CMP @#0x06B0,#0x1500
		if (high)
		{
			tick(0x5A74, 12);
			poke(0x06B0, 0x1500);
			tick(0x5A7A, 33);
			T.r[0] = 0x0017;
			tick(0x5A7E, 18);
			tick(0x5A9E, 12);                          // BR
		}
		else
		{
			tick(0x5A80, 12);                          // BLT
			T.r[1] = sext8(peekb(0x02E7));
			tick(0x5A84, 24);
			if (T.r[1] & 0x8000)
			{
				tick(0x5A86, 12);                      // BGE
				T.r[1] = (uint16_t)-T.r[1];
				tick(0x5A88, 12);
			}
			else
				tick(0x5A88, 12);
			T.r[1] = (uint16_t)(T.r[1] << 1);
			tick(0x5A8A, 12);
			T.r[1] = (uint16_t)(T.r[1] << 1);
			tick(0x5A8C, 12);
			T.r[1] = (uint16_t)(T.r[1] << 1);
			tick(0x5A8E, 12);
			T.r[1] = (uint16_t)(T.r[1] + peek(0x06B0));
			tick(0x5A92, 24);
			T.r[0] = T.r[1];
			tick(0x5A94, 12);
			T.r[0] &= 0xFF00;
			tick(0x5A96, 12);                          // CLRB R0
			T.r[0] = (uint16_t)(T.r[0] >> 8);
			tick(0x5A98, 12);                          // SWAB R0
			T.r[0] = sext8((uint8_t)T.r[0]);
			tick(0x5A9A, 12);                          // MOVB R0,R0
			if (T.r[0] & 0x8000)
			{
				tick(0x5A9C, 12);                      // BGE
				T.r[0] = 0;
				tick(0x5A9E, 12);
			}
			else
				tick(0x5A9E, 12);
		}
		const bool alt = peekb(0x032F) != 0;
		tick(0x5AA2, 24);                              // TSTB @#0x032F
		if (alt)
		{
			tick(0x5AA8, 12);                          // BNE
			T.r[1] = 0x0010;
			tick(0x5AAC, 18);
		}
		else
		{
			tick(0x5AA4, 12);
			T.r[1] = 0;
			tick(0x5AA6, 12);
			tick(0x5AAC, 12);                          // BR
		}
		T.r[2] = 0;
		tick(0x5AAE, 12);
		T.r[2] = peekb((uint16_t)(0x7C48 + T.r[0]));
		tick(0x5AB2, 27);
		T.r[1] |= T.r[2];
		tick(0x5AB4, 12);
		T.r[0] = T.r[1];
		tick(0x5AB6, 12);
		const bool up = (int16_t)peek(0x06B2) > 0x1800;
		tick(0x5ABC, 30);                              // CMP @#0x06B2,#0x1800
		if (up)
		{
			tick(0x5ABE, 12);
			const bool alt2 = peekb(0x032F) != 0;
			tick(0x5AC2, 24);                          // TSTB @#0x032F
			if (alt2)
			{
				tick(0x5ACC, 12);                      // BNE
				poke(0x0ED4, 0x1400);
				tick(0x5AD2, 33);
			}
			else
			{
				tick(0x5AC4, 12);
				poke(0x0ED4, 0xA200);
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
			tick(0x5AE8, 18);
		}
	}
	poke(0x0306, T.r[0]);
	tick(0x5AEC, 27);
	poke(0x0304, T.r[0]);
	tick(0x5AF0, 27);
	poke(0x0302, T.r[0]);
	fl::mov(T.r[0]);
	tick(0x5AF4, 27);
	rts();
}

// ---------------------------------------------------------------- tricks (not reached by the tests)
// The trick moves 0x40-0x57 of 0x01EB (set up by A5:5264 on a ramp) play an animation from the
// table 0x01E0 (frames 0x01E2/0x01E4, step 0x01EE, 0x07B2) and change the skater's state.
// The game never sets them up in the test scenarios; these routines were written from the
// disassembly and are not reached by the tests.

// A5:5388  trick_facing(): 1 when the skater's direction 0x01F8 is within 2 of 6, else 0
uint16_t trick_facing(const Args &)
{
	T.r[0] = sext8(peekb(0x01F8));
	tick(0x538C, 24);
	T.r[0] &= 0x0007;
	tick(0x5390, 18);
	T.r[0] = (uint16_t)(T.r[0] - 6);
	tick(0x5394, 18);
	if (T.r[0] & 0x8000)
	{
		tick(0x5396, 12);                              // BPL
		T.r[0] = (uint16_t)-T.r[0];
		tick(0x5398, 12);
	}
	else
		tick(0x5398, 12);
	tick(0x539C, 18);                                  // CMP R0,#2
	if ((int16_t)T.r[0] > 2)
	{
		tick(0x53A4, 12);                              // BGT
		T.r[0] = 0;
		tick(0x53A6, 12);
	}
	else
	{
		tick(0x539E, 12);
		T.r[0] = 1;
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
	tick(0x5440, 12);
	T.r[0] = peekb(0x07B2);
	tick(0x5444, 24);
	T.r[0] = (uint16_t)(T.r[0] + peek(0x01E0));
	tick(0x5448, 24);
	T.r[0]--;
	tick(0x544A, 12);
	for (;;)
	{
		T.r[0]++;
		tick(0x544C, 12);
		poke(0x01E2, (uint16_t)(peek(0x01E2) + 2));
		tick(0x5452, 33);
		poke(0x01E4, (uint16_t)(peek(0x01E4) + 2));
		tick(0x5458, 33);
		pokeb(0x01EE, (uint8_t)(peekb(0x01EE) + 1));
		tick(0x545C, 27);
		const bool last = (peekb(T.r[0]) & 0x80) != 0;
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
	cyc(159 + 48);
	uint16_t r2 = 0;
	if (!peekb(0x01F0))
	{
		cyc(42);
		if (!(peekb(0x0324) & 0x04))
		{
			cyc(87);
			uint16_t r1 = a.u(0) & 0x000F;
			if (peekb(0x0327) & 0x10)
			{
				cyc(30);
				r1 ^= 0x0008;
			}
			cyc(60);
			uint16_t r0 = (uint16_t)(r1 - sext8(peekb(0x01F8)));
			if (r0 & 0x8000)
			{
				cyc(12);
				r0 = (uint16_t)-r0;
			}
			cyc(42);
			r1 = r0;
			if ((int16_t)r1 > 8)
			{
				cyc(30);
				r1 = (uint16_t)(0x0010 - r1);
			}
			cyc(30);
			if ((int16_t)r1 > 2)
			{
				cyc(12);
				r2++;
			}
		}
	}
	cyc(24);
	if (!r2)
	{
		cyc(54);
		const uint16_t r0 = 7 & ~peekb(0x0327);
		if (!(uint8_t)r0)
		{
			cyc(21);
			return r0;
		}
	}
	cyc(42);
	if (peekb(0x0327) & 7)
	{
		cyc(33);
		pokeb(0x01F0, 7);
	}
	else
	{
		cyc(45);
		pokeb(0x01F0, 4);
	}
	cyc(144);
	const uint16_t r0 = (uint16_t)(peekb(0x0772) | 0x0080);
	pokeb(0x0772, (uint8_t)r0);
	pokeb(0x0771, (uint8_t)r0);
	if (!peek(0x02EE))
	{
		cyc(72);
		poke(0x02EE, 0x000C);
		poke(0x05B2, peek(0x01DE));
	}
	cyc(21);
	return r0;
}

// A5:5462  trick_step(): show the current frame (0x03B2/0x03B4 from 0x01E2/0x01E4, step 0x01EE
// into 0x07B2), clear bit 7 of the sprite flags unless the frame ends a sequence, advance
// (A5:543E), check the landing (A5:53A8) when one was pending, else count 0x01F2; a queued
// move 0x01EF becomes the current one (0x01EB).  Returns 0x01EF.
uint16_t trick_step(const Args &)
{
	push(T.r[1]);
	tick(0x5464, 24);
	pokeb(0x07B1, 0);
	tick(0x5468, 27);
	poke(0x03B2, peek(0x01E2));
	tick(0x546E, 39);
	poke(0x03B4, peek(0x01E4));
	tick(0x5474, 39);
	pokeb(0x07B2, peekb(0x01EE));
	tick(0x547A, 39);
	T.r[0] = 0;
	tick(0x547C, 12);
	T.r[0] = peekb(0x07B2);
	tick(0x5480, 24);
	T.r[0] = (uint16_t)(T.r[0] + peek(0x01E0));
	tick(0x5484, 24);
	T.r[1] = sext8(peekb(T.r[0]));
	tick(0x5486, 18);
	if (!(T.r[1] & 0x8000))
	{
		tick(0x5488, 12);                              // BLT
		T.r[0] = peek(0x0772);
		tick(0x548C, 24);
		T.r[0] &= 0x007F;
		tick(0x5490, 18);
		pokeb(0x0772, (uint8_t)T.r[0]);
		tick(0x5494, 27);
		pokeb(0x0771, (uint8_t)T.r[0]);
		tick(0x5498, 27);
	}
	else
		tick(0x5498, 12);
	jsr(0x549C, 0x543E);                               // next_trick_frame
	const bool landing = peekb(0x01F0) != 0;
	tick(0x54A0, 24);                                  // TSTB @#0x01F0
	bool count = true;
	if (landing)
	{
		tick(0x54A2, 12);
		pokeb(0x01F0, 0);
		tick(0x54A6, 27);
		push(T.r[1]);
		tick(0x54A8, 24);
		jsr(0x54AC, 0x53A8);                           // trick_landing
		T.r[6] = (uint16_t)(T.r[6] + 2);
		tick(0x54AE, 18);                              // TST (SP)+
		const bool again = peekb(0x01F0) != 0;
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
		pokeb(0x01F2, (uint8_t)(peekb(0x01F2) + 1));
		tick(0x54B8, 27);
	}
	T.r[0] = 0;
	tick(0x54BA, 12);
	T.r[0] = peekb(0x01EF);
	tick(0x54BE, 24);
	tick(0x54C0, 12);                                  // TST R0
	if (T.r[0])
	{
		tick(0x54C2, 12);
		pokeb(0x01EB, (uint8_t)T.r[0]);
		tick(0x54C6, 27);
		pokeb(0x01EF, 0);
		tick(0x54CA, 27);
	}
	else
		tick(0x54CA, 12);                              // BEQ
	T.r[1] = pop();
	tick(0x54CC, 18);
	rts();
	return T.r[0];
}

// A5:5264  start_trick(): on a ramp: the jump height class by the vertical speed 0x05B2
// (< 0x120, < 0x180, more; at the top the skater turns around: x velocity from y, objects
// 1-3 follow, A5:5166), the animation speed F:90A6(0x05B2, 0x525E[class]) clamped to 5-9
// in the sprite bytes 0x0771/0x0772/0x0791/0x0792, and the trick (move 0x01EB, next 0x01EF,
// landing 0x01F0, animation 0x01E0/0x01E2/0x01E4, frame 0x01F1) from the tables 0x7D12-0x7DAA
// (bank B 0x2C00) by class and direction.  Sound 0x4C.
uint16_t start_trick(const Args &)
{
	cyc(135 + 126);
	T.r[0] = 0x2C00;
	set_bank_b(0x2C00);
	const uint16_t r2 = peek(0x05B2);
	uint16_t r1;
	if ((int16_t)r2 < 0x0120)
	{
		cyc(24);
		r1 = 0;
	}
	else
	{
		cyc(30);
		if ((int16_t)r2 < 0x0180)
		{
			cyc(30);
			r1 = 1;
		}
		else
		{
			cyc(18);
			r1 = 2;
		}
	}
	cyc(117);
	push(peek((uint16_t)(0x525E + (r1 << 1))));
	push(r2);
	uint16_t r0 = call(0x90A6, {});                    // divide
	cyc(30);
	if ((int16_t)r0 > 9)
	{
		cyc(30);
		r0 = 9;
	}
	else
	{
		cyc(30);
		if ((int16_t)r0 < 5)
		{
			cyc(18);
			r0 = 5;
		}
	}
	cyc(183);
	pokeb(0x0792, (uint8_t)r0);
	pokeb(0x0791, (uint8_t)r0);
	r0 |= 0x0080;
	pokeb(0x0772, (uint8_t)r0);
	pokeb(0x0771, (uint8_t)r0);
	pokeb(0x07B1, 0);
	if (r1 == 2)
	{
		cyc(186);
		pokeb(0x07F0, 0);
		pokeb(0x07F4, 0);
		poke(0x0570, asr(peek(0x0570), 1));
		poke(0x04F0, (uint16_t)-(uint16_t)(peek(0x0570) << 1));
		r0 = call(0x5166, {});                         // copy_object0_velocity
	}
	cyc(165);
	r1 = (uint16_t)(r1 * 3 + sext8(peekb((uint16_t)(0x7D12 + (sext8(peekb(0x031D)) & 7)))));
	if (peekb(0x0324) & 0x80)
	{
		cyc(42);
		if ((int16_t)peek(0x05B0) < 0x01A0)
		{
			cyc(18);
			r1 = (uint16_t)(r1 + 9);
		}
	}
	cyc(258);
	pokeb(0x01EB, peekb((uint16_t)(0x7D1A + r1)));
	pokeb(0x01EF, (uint8_t)(sext8(peekb((uint16_t)(0x7D1A + r1))) + 1));
	pokeb(0x01F0, peekb((uint16_t)(0x7D2C + r1)));
	r0 = (uint16_t)(r1 << 1);
	poke(0x01E0, peek((uint16_t)(0x7D3E + r0)));
	if (peekb(0x0327) & 0x38)
	{
		cyc(63);
		poke(0x0300, 0x1800);
		r1 = (uint16_t)(r1 + 0x0012);
	}
	else
	{
		cyc(33);
		poke(0x0300, 0x1900);
	}
	cyc(192);
	pokeb(0x01F1, (uint8_t)r1);
	poke(0x01E2, peek((uint16_t)(0x7D62 + (r1 << 1))));
	poke(0x01E4, peek((uint16_t)(0x7DAA + (r1 << 1))));
	pokeb(0x01EE, 0);
	if (peekb(0x01EB) == 0x45)
	{
		cyc(33);
		pokeb(0x0324, (uint8_t)(peekb(0x0324) | 0x40));
	}
	cyc(90);
	pokeb(0x0330, 0x40);
	poke(T.r[6], 0x004C);
	T.r[0] = r0;
	r0 = call(0x8E78, {});                             // queue_sound
	cyc(45);
	T.r[6] = (uint16_t)(T.r[6] + 4);
	return r0;
}

// A5:54CE  trick_moves(): the trick state machine: check the landing of the current frame
// (A5:53A8), then the move 0x01EB: 0x40-0x45 (jump table 0x5814: rising, top, falling,
// landing frames, spin, ramp lip) and 0x46-0x57 (jump table 0x5820: transitions between
// them); finally the landing's sprites (F:E5D2 / F:E648 by 0x01F0, +0x10 in the lower half
// 0x0ED4 < 0x5C00).  Bank B 0x2C00.
uint16_t trick_moves(const Args &)
{
	cyc(135 + 108);
	set_bank_b(0x2C00);
	if (peek(0x01E0))
	{
		cyc(102);
		const uint16_t r1 = sext8(peekb((uint16_t)(peekb(0x07B2) + peek(0x01E0))));
		if (peekb(0x07B1))
		{
			cyc(51);
			pokeb(0x07B1, 0);
			if (r1 & 0x8000)
			{
				cyc(108);
				const uint16_t r0 = (uint16_t)(peekb(0x0772) | 0x0080);
				pokeb(0x0772, (uint8_t)r0);
				pokeb(0x0771, (uint8_t)r0);
			}
		}
		cyc(54);
		call(0x53A8, {r1});                            // trick_landing
		cyc(18);
	}
	cyc(54);
	uint16_t move = sext8(peekb(0x01EB));
	if ((int16_t)move >= 0x40)
	{
		cyc(30);
		if ((int16_t)move <= 0x45)
		{
			cyc(57);
			switch (move - 0x40)
			{
			case 0:                                    // rising
				cyc(48);
				if ((int16_t)peek(0x06B2) < (int16_t)peek(0x0300))
				{
					cyc(12);                           // the BLT at 0x5632
					break;
				}
				cyc(30);
				call(0x5462, {});                      // trick_step
				cyc(36);
				if (peekb(0x01F0))
				{
					cyc(45);
					pokeb(0x01EB, 0x45);
					break;
				}
				cyc(42);
				if (!(peekb(0x0324) & 0x01)) break;
				{
					cyc(168);
					poke(0x01DE, peek(0x05B2));
					poke(0x05B2, 0);
					const uint16_t r2 = (uint16_t)(asr(peek(0x06F2), 1) + peek(0x0732));
					uint16_t r0;
					if (peekb(0x0327) == 7)
					{
						cyc(30);
						r0 = 0x2600;
					}
					else
					{
						cyc(18);
						r0 = 0xE000;
					}
					cyc(213);
					poke(0x06B2, (uint16_t)(r0 - r2 + peek(0x0300)));
					poke(0x06B2, (uint16_t)(peek(0x06B2) - 0x0100));
					poke(0x06B4, peek(0x06B2));
					poke(0x06B6, peek(0x06B2));
					poke(0x02EE, 0);
				}
				break;
			case 1:                                    // top
				cyc(81);
				pokeb(0x0324, (uint8_t)(peekb(0x0324) | 0x40));
				if ((int16_t)peek(0x06B2) > (int16_t)peek(0x0300)) break;
				cyc(45 + 30);
				pokeb(0x01EB, 0x45);
				call(0x5462, {});                      // trick_step
				cyc(12);
				break;
			case 2:                                    // falling
				cyc(36);
				if ((int16_t)peek(0x05B2) > 0) break;
				cyc(30);
				call(0x5462, {});                      // trick_step
				cyc(12);
				break;
			case 3:                                    // landing frames
			{
				cyc(96);
				const uint16_t r2 = peekb(0x01EF);
				if (peekb(0x07B2) != r2) break;
				cyc(30);
				if (r2 != 4)
				{
					cyc(90);
					const uint16_t r0 = (uint16_t)-peek(0x01DE);
					poke(0x05B4, r0);
					poke(0x05B2, r0);
				}
				cyc(96);
				poke(0x02EE, 0x000C);
				pokeb(0x01EB, 0x45);
				if (r2 == 5)
				{
					cyc(60);
					call(0x8E78, {0x0059});            // queue_sound
					cyc(30);
				}
				break;
			}
			case 4:                                    // spin
				cyc(39);
				poke(0x0432, (uint16_t)(peek(0x0432) - 1));
				if (peek(0x0432))
				{
					cyc(36);
					if (!peekb(0x01F0)) break;
				}
				cyc(30);
				call(0x5462, {});                      // trick_step
				cyc(66 + 12);
				pokeb(0x01EF, 4);
				pokeb(0x01EB, 0x43);
				break;
			case 5:                                    // ramp lip
				cyc(42);
				if (!(peekb(0x0324) & 0x08)) break;
				cyc(42);
				if ((int16_t)peek(0x05B2) < 0x0140) break;
				cyc(30 + 12);
				if ((int16_t)peek(0x06B2) < 0x2400) break;
				cyc(63);
				pokeb(0x0324, (uint8_t)(peekb(0x0324) & ~0x08));
				call(0x5462, {});                      // trick_step
				cyc(33);
				pokeb(0x01F1, 0x12);
				break;
			}
		}
		else
			cyc(12);                                   // BR 5644 from 561A
	}
	else
		cyc(12);                                       // BR 5644 from 561A
	cyc(54);
	move = sext8(peekb(0x01EB));
	bool second = false;
	if ((int16_t)move >= 0x46)
	{
		cyc(30);
		second = (int16_t)move <= 0x57;
	}
	if (!second)
		cyc(12);                                       // 571E: BR 57E4
	else
	{
		cyc(69);
		switch (move - 0x46)                           // costs include the branches to 0x57E4
		{
		case 0:
			cyc(102);
			pokeb(0x0324, (uint8_t)(peekb(0x0324) | 0x05));
			pokeb(0x01EB, 0x40);
			break;
		case 1:
			cyc(42);
			if (peekb(0x07B2) != 5)
			{
				cyc(12);
				break;
			}
			{
				cyc(231 + 12);
				const uint16_t r1 = (uint16_t)(peekb(0x0771) << 2);
				pokeb(0x0791, (uint8_t)r1);
				pokeb(0x0771, (uint8_t)r1);
				pokeb(0x0792, (uint8_t)r1);
				pokeb(0x0772, (uint8_t)r1);
				pokeb(0x01EB, (uint8_t)(move + 1));
			}
			break;
		case 2:
			cyc(42);
			if (peekb(0x07B2) == 5)
			{
				cyc(12);
				break;
			}
			{
				cyc(213 + 33 + 12);
				const uint16_t r1 = (uint16_t)(peekb(0x0771) >> 2);
				pokeb(0x0791, (uint8_t)r1);
				pokeb(0x0771, (uint8_t)r1);
				pokeb(0x0792, (uint8_t)r1);
				pokeb(0x0772, (uint8_t)r1);
				pokeb(0x01EF, 9);
				pokeb(0x01EB, 0x43);
			}
			break;
		case 3:
			cyc(90);
			pokeb(0x0324, (uint8_t)(peekb(0x0324) | 0x01));
			pokeb(0x01EB, 0x40);
			break;
		case 4:
			cyc(36);
			if (!peekb(0x07B2))
			{
				cyc(12);
				break;
			}
			{
				cyc(195);
				const uint16_t r0 = sext8(peekb(0x025F));
				poke(0x05B2, peekb((uint16_t)(0x7C74 + r0)));
				poke(0x05B2, (uint16_t)(peek(0x05B2) + peek(0x01DE)));
				const uint16_t limit = peek((uint16_t)(0x7C78 + (r0 << 1)));
				if ((int16_t)peek(0x05B2) > (int16_t)limit)
				{
					cyc(42);
					poke(0x05B2, limit);
				}
				cyc(45 + 33 + 12);
				poke(0x02EE, 0x000C);
				pokeb(0x01EB, 0x41);
			}
			break;
		case 5:
			cyc(45);
			pokeb(0x01EB, 0x42);
			break;
		case 6:
			cyc(45);
			pokeb(0x01EB, 0x41);
			break;
		case 7:
			cyc(123);
			pokeb(0x0324, (uint8_t)(peekb(0x0324) | 0x01));
			poke(0x0300, (uint16_t)(peek(0x0300) + 0x0100));
			pokeb(0x01EB, 0x40);
			break;
		case 8:
			cyc(105);
			poke(0x01DE, 0);
			pokeb(0x01EF, 5);
			pokeb(0x01EB, 0x43);
			break;
		case 9:
			cyc(123);
			pokeb(0x0324, (uint8_t)(peekb(0x0324) | 0x05));
			poke(0x0300, (uint16_t)(peek(0x0300) + 0x0200));
			pokeb(0x01EB, 0x40);
			break;
		case 10:
			cyc(63);
			pokeb(0x0324, (uint8_t)(peekb(0x0324) & ~0x04));
			if (call(0x5388, {}))                      // trick_facing
			{
				cyc(24 + 165);
				poke(0x03B2, (uint16_t)(peek(0x03B2) + 0x000A));
				poke(0x03B4, (uint16_t)(peek(0x03B4) + 0x000A));
				poke(0x01E2, (uint16_t)(peek(0x01E2) + 0x000A));
				poke(0x01E4, (uint16_t)(peek(0x01E4) + 0x000A));
				poke(0x01E0, (uint16_t)(peek(0x01E0) + 0x0005));
			}
			else
				cyc(24);
			cyc(252);
			poke(0x0432, 0x0028);
			pokeb(0x01EB, 0x44);
			poke(0x04F0, (uint16_t)(asr(peek(0x04F0), 1) + peek(0x04F0)));
			poke(0x0570, (uint16_t)(asr(peek(0x0570), 1) + peek(0x0570)));
			break;
		case 11:
			cyc(45);
			pokeb(0x01EB, 0x40);
			break;
		case 12:
			cyc(111);
			pokeb(0x01EB, 0x41);
			pokeb(0x01F0, 4);
			pokeb(0x01EF, 0x53);
			break;
		case 13:
			cyc(33);
			pokeb(0x01EB, 0x45);
			break;
		case 14:
			cyc(78);
			pokeb(0x01EB, 0x42);
			poke(0x02EE, 0x0009);
			break;
		case 15:
			cyc(78);
			pokeb(0x01EF, 0x56);
			pokeb(0x01EB, 0x41);
			break;
		case 16:
			cyc(78);
			poke(0x05B2, (uint16_t)(peek(0x05B2) + 0x0060));
			pokeb(0x01EB, 0x45);
			break;
		case 17:
			cyc(42);
			if (peekb(0x0327) == 7)
			{
				cyc(42);
				if (peekb(0x01F8) == 2)
				{
					cyc(33);
					pokeb(0x0324, (uint8_t)(peekb(0x0324) | 0x0C));
				}
			}
			cyc(27 + 33);
			pokeb(0x01EF, 0);
			pokeb(0x01EB, 0x45);
			break;
		}
	}
	cyc(60);
	uint16_t r0 = peekb(0x01F0);
	if ((int16_t)r0 > 0)
	{
		cyc(54);
		uint16_t r1 = r0;
		if (peek(0x0ED4) < 0x5C00)
		{
			cyc(18);
			r1 |= 0x0010;
		}
		cyc(54);
		r0 = call(0xE5D2, {r1});
		cyc(78);
		poke(0x03B2, r0);
		r0 = call(0xE648, {r1});
		cyc(45);
		poke(0x03B4, r0);
	}
	cyc(21);
	return r0;
}

// A7:4A66  velocity_direction(): the skater's direction 0x031D (0-15) from the velocity
// (0x04F0, 0x0570): F:90A6 divides 16 vy by vx and the ratio picks the sector
uint16_t velocity_direction(const Args &)
{
	c_save(3, 0x4A6A);
	T.r[4] = peek(0x0570);
	tick(0x4A6E, 24);
	T.r[3] = peek(0x04F0);
	tick(0x4A72, 24);
	if (!T.r[3])
	{
		tick(0x4A74, 12);                              // BNE
		tick(0x4A76, 12);                              // TST R4
		if ((int16_t)T.r[4] > 0)
		{
			tick(0x4A78, 12);
			T.r[1] = 0x0004;
			tick(0x4A7C, 18);
		}
		else
		{
			tick(0x4A7E, 12);                          // BLE
			T.r[1] = 0x000C;
			tick(0x4A82, 18);
		}
		tick(0x4AFA, 12);                              // BR
	}
	else
	{
		tick(0x4A84, 12);
		push(T.r[4]);
		tick(0x4A86, 24);
		for (uint16_t pc = 0x4A88; pc <= 0x4A8E; pc = (uint16_t)(pc + 2))
		{
			poke(T.r[6], (uint16_t)(peek(T.r[6]) << 1));
			tick(pc, 21);                              // ASL (SP)
		}
		push(T.r[3]);
		tick(0x4A90, 24);
		jsr(0x4A94, 0x90A6);                           // divide
		T.r[2] = T.r[0];
		tick(0x4A96, 12);
		if (T.r[0] & 0x8000)
		{
			tick(0x4A98, 12);                          // BGE
			T.r[0] = (uint16_t)-T.r[0];
			tick(0x4A9A, 12);
		}
		else
			tick(0x4A9A, 12);
		if (!T.r[0])
		{
			tick(0x4A9C, 12);                          // BNE
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
		tick(0x4ADA, 12);                              // TST R2
		if (T.r[2] & 0x8000)
		{
			tick(0x4ADC, 12);                          // BGE
			T.r[1] = (uint16_t)-T.r[1];
			tick(0x4ADE, 12);
			T.r[1] = (uint16_t)(T.r[1] + 8);
			tick(0x4AE2, 18);
		}
		else
			tick(0x4AE2, 12);
		tick(0x4AE4, 12);                              // TST R4
		if (T.r[4] & 0x8000)
		{
			tick(0x4AE6, 12);                          // BGE
			tick(0x4AE8, 12);                          // TST R2
			if (T.r[2])
			{
				tick(0x4AEA, 12);                      // BEQ
				tick(0x4AEE, 18);                      // CMP R1,#8
				if (T.r[1] == 8)
				{
					tick(0x4AF0, 12);                  // BNE
					T.r[1] = 0;
					tick(0x4AF2, 12);
					tick(0x4AF8, 12);                  // BR
				}
				else
				{
					tick(0x4AF4, 12);
					T.r[1] = (uint16_t)(T.r[1] + 8);
					tick(0x4AF8, 18);
				}
			}
			else
				tick(0x4AF8, 12);
		}
		else
			tick(0x4AF8, 12);
		T.r[6] = (uint16_t)(T.r[6] + 4);
		tick(0x4AFA, 24);                              // CMP (SP)+,(SP)+
	}
	pokeb(0x031D, (uint8_t)T.r[1]);
	tick(0x4AFE, 27);
	c_restore(3);
	return T.r[0];
}

// ---------------------------------------------------------------- the skater

// A5:4A2A (assembly)  skater_physics(): the skater's frame step.
//   Steering: the joystick direction 0x031A (from the table 0x6000, bank B 0x2C00, by 0x01C6)
//   is turned towards (A5:4A0A); 0x031B holds a forced direction for 0x031C frames.
//   0x031E (jump table 0x5160): 1 = landing (0x0325/0x0328 cleanup), 0/2 = rolling:
//     on the ground the kick counter 0x0323 / 0x0321 and the turn accumulators 0x02F2-0x02F6
//     give the direction 0x01F8; in the air the slope 0x0327 decides: a ramp (type 5-7) runs
//     the spin sequence of 0x02F8 (0x0325/0x0326, F:90A6), a slope turns the skater to its
//     direction and lets him fall back (F:A2BE / F:A45A / 0x0322, or sound 0x58, B3:7E44 and
//     0x01EA = 1 when he is too slow).
//   Kick (0x01EB = 5): speed from 0x02E8, A5:5B46, F:9FB4 into the air.
//   In the air: F:9B82, gravity 0x02EE, kick phases 0x0A/0x0B/1/2/3 adjust the speed 0x01B4
//   with the friction tables 0x7BE8/0x7C28/0x7C38/0x7C64/0x7C68 (0x0F59 = 3 downhill), it stops
//   at the slope's limit; F:91A6; on flat ground the velocities are rounded and the vertical
//   speed 0x05B0 follows the height table 0xFCB0 (limit 0x7CD0) or F:80FA on slopes.
//   Falling: 0x05B2 -= gravity down to -0x200 (not downhill).
//   Finally the sprite flags 0x0470-0x0473 get 0x40 / 0xC0 by 0x032F.
// Registers end as the original leaves them.
void skater_physics()
{
	auto tbl = [](uint16_t a) { return peek(a); };
	blk(0x4A2A, 186);
	T.r[0] = 0x2C00;
	set_bank_b(0x2C00);
	T.r[0] = asr(peek(0x01C6), 1);
	pokeb(0x031A, peekb((uint16_t)(0x6000 + T.r[0])));
	T.r[0] = sext8(peekb(0x031B));
	if (!(T.r[0] & 0x8000))
	{
		blk(0x4A48, 30);
		if ((int16_t)T.r[0] < 0x003F)
		{
			blk(0x4A4E, 42);
			if (peekb(0x01EA) != 1)
			{
				blk(0x4A56, 33);
				pokeb(0x031B, 0x3F);
			}
			blk(0x4A5C, 36);
			T.r[2] = sext8(peekb(0x031B));
		}
		else
		{
			blk(0x4A62, 72);
			T.r[2] = sext8(peekb(0x031A));
			T.r[0] = sext8(peekb(0x01F8));
			if (T.r[2] == T.r[0])
			{
				blk(0x4A6E, 33);
				pokeb(0x031B, 0xFF);
			}
		}
		blk(0x4A74, 66);
		T.r[0] = sext8(peekb(0x01F8));
		T.r[2] = (uint16_t)(T.r[2] - T.r[0]);
		call(0x4A0A, {});                              // fold_direction
		blk(0x4A7E, 120);
		T.r[0] = (uint16_t)(sext8(peekb(0x01F8)) + T.r[1]);
		T.r[3] = T.r[0] & 0x000F;
		pokeb(0x031A, (uint8_t)T.r[3]);
	}
	blk(0x4A92, 36);
	T.r[3] = sext8(peekb(0x031E));
	if (T.r[3])
	{
		blk(0x4A98, 36);
		T.r[0] = peek(0x01B4);
		if (T.r[0] & 0x8000)
		{
			blk(0x4A9E, 12);
			T.r[0] = (uint16_t)-T.r[0];
		}
		blk(0x4AA0, 30);
		if ((int16_t)T.r[0] < 0x0080)
		{
			blk(0x4AA6, 69);
			pokeb(0x031C, 3);
			if ((int16_t)peek(0x01B6) < 0)
			{
				blk(0x4ABA, 33);
				poke(0x01B4, 0xFEA0);
			}
			else
			{
				blk(0x4AB2, 45);
				poke(0x01B4, 0x0160);
			}
		}
		blk(0x4AC0, 39);
		poke(0x01B6, peek(0x01B4));
	}
	blk(0x4AC6, 51);
	T.r[0] = (uint16_t)(T.r[3] << 1);
	if (T.r[3] == 1)
		goto landing;                                  // 0x4DD0
	// T.r[3] 0 or 2 (other values are not used: they jump outside the routine)
	blk(0x4ACE, 30);
	if (T.r[3] == 2)
	{
		blk(0x4AD4, 48);
		if (peekb(0x0329) != peekb(0x0328))
		{
			blk(0x4ADC, 75);
			T.r[0] = (uint16_t)(sext8(peekb(0x031C)) - 1);
			pokeb(0x031C, (uint8_t)T.r[0]);
			if (!(uint8_t)T.r[0])
			{
				blk(0x4AE8, 60);
				pokeb(0x031E, 0);
				pokeb(0x0328, 0xFE);
			}
		}
	}
	blk(0x4AF2, 36);
	if (peekb(0x031F))
	{
		blk(0x4BA4, 18);
		goto kick;                                     // JMP 0x4E34
	}
	blk(0x4AF8, 36);
	if (!peekb(0x01EA))
	{
		// on the ground: kicks and turns
		blk(0x4AFE, 36);
		T.r[5] = sext8(peekb(0x0323));
		if (!T.r[5])
		{
			blk(0x4B04, 129);
			T.r[3] = (uint16_t)(peek(0x01C8) << 1);
			T.r[0] = peekb((uint16_t)(0x7CA0 + sext8(peekb(0x025D))));
			if (!((int16_t)T.r[3] < (int16_t)T.r[0]))
			{
				blk(0x4B1A, 87);
				T.r[3] = (uint16_t)(T.r[5] + 1);
				pokeb(0x0323, (uint8_t)T.r[3]);
				if (!((int16_t)peek(0x01CA) > 0))
				{
					blk(0x4B28, 12);
					T.r[0] = (uint16_t)-T.r[0];
				}
				blk(0x4B2A, 27);
				pokeb(0x0321, (uint8_t)T.r[0]);
			}
			blk(0x4B2E, 51);
			pokeb(0x01F8, peekb(0x031A));
		}
		else
		{
			blk(0x4B36, 30);
			if (T.r[5] == 1)
			{
				blk(0x4B3C, 201);
				T.r[4] = sext8(peekb(0x0321));
				poke(0x02F4, (uint16_t)(peek(0x02F4) + T.r[4]));
				T.r[0] = (uint16_t)((peek(0x01CA) << 1) + peek(0x02F2));
				poke(0x02F2, T.r[0]);
				poke(0x02F6, (uint16_t)(peek(0x02F6) + T.r[4]));
				T.r[0] = peek(0x02F6);
				if (T.r[0] & 0x8000)
				{
					blk(0x4B5C, 45);
					poke(0x02F6, (uint16_t)(peek(0x02F6) + 0x0120));
				}
				else
				{
					blk(0x4B64, 30);
					if (!((int16_t)T.r[0] < 0x0120))
					{
						blk(0x4B6A, 33);
						poke(0x02F6, (uint16_t)(peek(0x02F6) - 0x0120));
					}
				}
				blk(0x4B70, 174);
				T.r[0] = asr(peek(0x02F6), 2);
				pokeb(0x01F8, peekb((uint16_t)(0x6000 + T.r[0])));
				T.r[2] = (uint16_t)(peek(0x02F4) - peek(0x02F2));
				T.r[3] = (uint16_t)(T.r[4] ^ T.r[2]);
				if (!(T.r[3] & 0x8000))
				{
					blk(0x4B8C, 51);
					T.r[0] = (uint16_t)(T.r[5] - 1);
					pokeb(0x0323, (uint8_t)T.r[0]);
				}
			}
		}
		blk(0x4B94, 96 + 18);                          // and JMP 0x4E34
		T.r[0] = sext8(peekb(0x01F8)) & 0x000F;
		pokeb(0x01F8, (uint8_t)T.r[0]);
		goto kick;
	}
	// in the air
	blk(0x4BA8, 111);
	pokeb(0x01F8, peekb(0x031A));
	T.r[0] = sext8(peekb(0x0327)) & 0x0007;
	if (T.r[0] < 5)
	{
		blk(0x4BBC, 18);
		goto slope;                                    // JMP 0x4CD8
	}
	// a ramp: the spin sequence
	blk(0x4BC0, 36);
	if (peekb(0x0325) & 0x80)
	{
		blk(0x4BC6, 30);
		call(0xE238, {});                              // reset_skater_state
		blk(0x4BCA, 60);
		T.r[2] = sext8(peekb(0x031D));
		T.r[3] = peek(0x01B4);
		if (T.r[3] & 0x8000)
		{
			blk(0x4BD4, 144);
			T.r[1] = (uint16_t)-T.r[3];
			T.r[0] = (uint16_t)(sext8(peekb(0x025F)) << 1);
			poke(0x01B4, (uint16_t)(peek(0x01B4) + tbl((uint16_t)(0x7C80 + T.r[0]))));
			T.r[0] = 0x0008;
			T.r[2] ^= T.r[0];
		}
		else
		{
			blk(0x4BEC, 48);
			T.r[1] = T.r[3];
			if ((int16_t)T.r[1] < (int16_t)peek(0x02FC))
			{
				blk(0x4BF4, 99);
				T.r[0] = (uint16_t)(asr((uint16_t)(peek(0x02FC) - T.r[3]), 1) + peek(0x01B4));
				poke(0x01B4, T.r[0]);
			}
		}
		blk(0x4C04, 231);
		T.r[0] = (uint16_t)(asr(sext8(peekb(0x0327)), 3) << 1);
		poke(0x02F8, tbl((uint16_t)(0x7A08 + T.r[0])));
		T.r[0] = (uint16_t)((T.r[2] << 1) + peek(0x02F8));
		poke(0x02F8, peek(T.r[0]));
		if (peek(T.r[0]) == 0xFFFF)
		{
			blk(0x4C28, 45);
			pokeb(0x0325, 0x7F);
		}
		else
		{
			blk(0x4C30, 84);
			push(0x0580);
			push(T.r[1]);
			T.r[0] = call(0x90A6, {});                 // divide
			blk(0x4C3A, 51);
			pokeb(0x0325, (uint8_t)T.r[0]);
			T.r[0] = sext8((uint8_t)T.r[0]);
			if (!T.r[0])
			{
				blk(0x4C42, 51);
				T.r[3] = (uint16_t)(T.r[0] + 1);
				pokeb(0x0325, (uint8_t)T.r[3]);
			}
			blk(0x4C4A, 63);
			pokeb(0x0326, peekb(0x0325));
			T.r[6] = (uint16_t)(T.r[6] + 4);
		}
	}
	blk(0x4C52, 54);
	T.r[0] = sext8(peekb(0x0325));
	if (T.r[0] == 0x007F)
	{
		blk(0x4D48, 12);
		goto direction;                                // 0x4DBE
	}
	blk(0x4C5C, 30);
	if (T.r[0] == 0x007E)
	{
		blk(0x4C62, 156);
		T.r[0] = (uint16_t)((sext8(peekb(0x0327)) & 0x0018) >> 3);
		pokeb(0x031D, rb((uint16_t)(0xFCD0 + T.r[0])));
		if ((int16_t)peek(0x01B4) < 0)
		{
			blk(0x4C7C, 96);
			T.r[3] = 0x0008;
			T.r[0] = sext8(rb((uint16_t)(0xFCD0 + T.r[0])));
			T.r[3] ^= T.r[0];
			pokeb(0x031D, (uint8_t)T.r[3]);
		}
		blk(0x4CA8, 45 + 12);
		pokeb(0x0325, 0x7F);
		goto direction;
	}
	blk(0x4C8C, 63);
	T.r[3] = (uint16_t)(sext8(peekb(0x0326)) - T.r[0]);
	pokeb(0x0326, (uint8_t)T.r[3]);
	for (;;)
	{
		blk(0x4C96, 36);
		T.r[3] = sext8(peekb(0x0326));
		if ((int16_t)T.r[3] > 0)
		{
			blk(0x4D98, 12);
			goto direction;
		}
		blk(0x4C9C, 84);
		T.r[2] = sext8(peekb(peek(0x02F8)));
		poke(0x02F8, (uint16_t)(peek(0x02F8) + 1));
		if (T.r[2] & 0x8000)
		{
			blk(0x4CA8, 45 + 12);
			pokeb(0x0325, 0x7F);
			goto direction;
		}
		blk(0x4CB0, 84);
		pokeb(0x031D, peekb(peek(0x02F8)));
		if ((int16_t)peek(0x01B4) < 0)
		{
			blk(0x4CBC, 81);
			T.r[4] = 0x0008;
			T.r[0] = sext8(peekb(0x031D));
			T.r[4] ^= T.r[0];
			pokeb(0x031D, (uint8_t)T.r[4]);
		}
		blk(0x4CCA, 90);
		poke(0x02F8, (uint16_t)(peek(0x02F8) + 1));
		T.r[0] = (uint16_t)(T.r[2] + T.r[3]);
		pokeb(0x0326, (uint8_t)T.r[0]);
	}
slope:
	// in the air over a slope (type 0-4)
	blk(0x4CD8, 150);
	pokeb(0x0325, 0xFF);
	pokeb(0x0324, 0);
	T.r[2] = sext8(peekb(0x01F8));
	T.r[0] = sext8(peekb(0x031D));
	T.r[2] = (uint16_t)(T.r[2] - T.r[0]);
	call(0x4A0A, {});                                  // fold_direction
	blk(0x4CF0, 30);
	if ((int16_t)T.r[2] > 4)
	{
		blk(0x4CF6, 108);
		poke(0x01B4, (uint16_t)-peek(0x01B4));
		T.r[3] = 0x0008;
		T.r[0] = sext8(peekb(0x031D));
		T.r[3] ^= T.r[0];
		pokeb(0x031D, (uint8_t)T.r[3]);
	}
	blk(0x4D08, 42);
	if ((int16_t)peek(0x01C8) > 3)
	{
		blk(0x4D10, 39);
		pokeb(0x0321, peekb(0x01CB));
	}
	blk(0x4D16, 42);
	if (peekb(0x01EA) == 2)
	{
		blk(0x4D80, 36);
		T.r[1] = peek(0x01B4);
		if (T.r[1] & 0x8000)
		{
			blk(0x4D86, 12);
			T.r[1] = (uint16_t)-T.r[1];
		}
		blk(0x4D88, 30);
		if ((int16_t)T.r[1] < 0x0080)
		{
			blk(0x4D8E, 51);
			pokeb(0x031D, peekb(0x01F8));
		}
		else
		{
			blk(0x4D96, 24);                           // TST R2; BNE
			if (T.r[2]) goto direction;
		}
		blk(0x4D9A, 63);
		pokeb(0x01EA, 1);
		call(0xE238, {});                              // reset_skater_state
		blk(0x4DA4, 60);
		push(0x0058);
		call(0x8E78, {});                              // queue_sound
		blk(0x4DAC, 102);
		T.r[0] = 0x3C00;
		set_bank_b(0x3C00);
		call(0x7E44, {});
		blk(0x4DBC, 18);
		T.r[6] = (uint16_t)(T.r[6] + 2);
		goto direction;
	}
	blk(0x4D1E, 42);
	if ((int16_t)peek(0x01C8) >= 8) goto direction;
	blk(0x4D26, 78);
	T.r[0] = sext8(peekb(0x01F8));
	push(T.r[0]);
	T.r[0] = call(0xA2BE, {});                         // direction_bit
	blk(0x4D30, 42);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	if (T.r[0])
	{
		blk(0x4D36, 126);
		T.r[0] = sext8(peekb(0x01F8));
		push(T.r[0]);
		T.r[0] = sext8(peekb(0x0321));
		push(T.r[0]);
		call(0xA45A, {});
		blk(0x4D46, 24 + 12);
		T.r[6] = (uint16_t)(T.r[6] + 4);
		goto direction;
	}
	blk(0x4D4A, 150);
	T.r[0] = sext8(peekb(0x0331));
	T.r[3] = sext8(peekb(0x0322));
	T.r[4] = rb((uint16_t)(0x7CC0 + T.r[0]));
	T.r[3] = (uint16_t)(T.r[3] + T.r[4]);
	pokeb(0x0322, (uint8_t)T.r[3]);
	T.r[0] = sext8((uint8_t)T.r[3]);
	if (T.r[0] & 0x8000)
	{
		blk(0x4D66, 84);
		T.r[3] = T.r[0] & 0x007F;
		pokeb(0x0322, (uint8_t)T.r[3]);
	}
	else
	{
		blk(0x4D62, 24);
		T.r[1] = 0;
	}
	blk(0x4D74, 63 + 12);
	T.r[0] = (uint16_t)(sext8(peekb(0x031D)) + T.r[1]);
	pokeb(0x031D, (uint8_t)T.r[0]);
direction:
	blk(0x4DBE, 108);
	T.r[0] = sext8(peekb(0x031D)) & 0x000F;
	pokeb(0x031D, (uint8_t)T.r[0]);
	goto kick;
landing:
	blk(0x4DD0, 36);
	if (peekb(0x01EB))
	{
		blk(0x4DD6, 30);
		call(0xE238, {});                              // reset_skater_state
	}
	blk(0x4DDA, 36);
	if ((int8_t)peekb(0x0325) > 0)
	{
		blk(0x4DE0, 33);
		pokeb(0x0325, 0x7E);
	}
	blk(0x4DE6, 87);
	pokeb(0x01F8, peekb(0x031A));
	if (peekb(0x01F8) == peekb(0x031B))
	{
		blk(0x4DF4, 48);
		if (peekb(0x0329) != peekb(0x0328))
		{
			blk(0x4DFC, 75);
			T.r[0] = (uint16_t)(sext8(peekb(0x031C)) - 1);
			pokeb(0x031C, (uint8_t)T.r[0]);
			if (!(uint8_t)T.r[0])
			{
				blk(0x4E08, 93);
				pokeb(0x031B, 0x3F);
				pokeb(0x031E, 0);
				pokeb(0x0328, 0xFE);
			}
		}
	}
	blk(0x4E18, 36);
	if (peekb(0x031B) & 0x80)
	{
		blk(0x4E1E, 60);
		pokeb(0x031E, 0);
		pokeb(0x0328, 0xFE);
	}
	blk(0x4E28, 36);
	if (peekb(0x01EA))
	{
		blk(0x4E2E, 39);
		pokeb(0x031D, peekb(0x01F8));
	}
kick:
	blk(0x4E34, 42);
	if (peekb(0x01EB) == 5)
	{
		blk(0x4E3C, 36);
		T.r[1] = peek(0x02E8);
		if (T.r[1] & 0x8000)
		{
			blk(0x4E4E, 30);
			if (!((int16_t)T.r[1] < (int16_t)0xFF80))
			{
				blk(0x4E54, 18);
				T.r[1] = (uint16_t)(T.r[1] - 0x0100);
			}
		}
		else
		{
			blk(0x4E42, 30);
			if (!((int16_t)T.r[1] > 0x0080))
			{
				blk(0x4E48, 30);
				T.r[1] = (uint16_t)(T.r[1] + 0x0100);
			}
		}
		blk(0x4E58, 129);
		poke(0x01B4, T.r[1]);
		T.r[0] = 0x1C00;
		set_bank_b(0x1C00);
		call(0x5B46, {});                              // rolling_sound
		blk(0x4E6C, 30);
		call(0x9FB4, {});                              // skater_take_off
		blk(0x4E70, 228);
		T.r[0] = (uint16_t)(sext8(peekb(0x01EB)) + 1);
		pokeb(0x01EB, (uint8_t)T.r[0]);
		T.r[0] = 0x2C00;
		set_bank_b(0x2C00);
		T.r[0] = sext8(peekb(0x025F));
		poke(0x05B2, peekb((uint16_t)(0x7C70 + T.r[0])));
	}
	blk(0x4E94, 36);
	if (!peekb(0x01EA))
	{
		blk(0x4E9A, 18);
		goto falling;                                  // JMP 0x50E0
	}
	blk(0x4E9E, 30);
	call(0x9B82, {});                                  // skater_motion_params
	blk(0x4EA2, 36);
	if (peekb(0x031F))
	{
		blk(0x4EA8, 18);
		goto velocity;                                 // JMP 0x502A
	}
	blk(0x4EAC, 36);
	if (!peekb(0x0EA0))
	{
		blk(0x4EB2, 39);
		poke(0x01B4, (uint16_t)(peek(0x01B4) + peek(0x02EE)));
	}
	// the push of the kick phases
	blk(0x4EB8, 90);
	T.r[2] = 0;
	T.r[3] = peek(0x01B4);
	T.r[0] = sext8(peekb(0x01EB));
	{
		bool phase_ab = T.r[0] == 0x000A;
		if (!phase_ab)
		{
			blk(0x4EC8, 30);
			phase_ab = T.r[0] == 0x000B;
		}
		bool done = false;
		if (phase_ab)
		{
			blk(0x4ECE, 36);
			if (!peekb(0x031E))
			{
				blk(0x4ED4, 24);
				if ((int16_t)T.r[3] > 0)
				{
					blk(0x4ED8, 30);
					T.r[2] = 0xFFF0;
				}
				else
				{
					blk(0x4EEE, 30);
					T.r[2] = 0x0010;
				}
				done = true;
			}
		}
		if (!done)
		{
			blk(0x4EDE, 24);
			bool push_back = false;
			if (T.r[3] & 0x8000)
			{
				blk(0x4EE2, 30);
				if (T.r[0] == 1)
				{
					blk(0x4EE8, 36);
					push_back = peekb(0x07B2) != 0;
				}
			}
			if (push_back)
			{
				blk(0x4EEE, 30);
				T.r[2] = 0x0010;
			}
			else
			{
				blk(0x4EF4, 30);
				if (T.r[0] == 2)
				{
					blk(0x4EFA, 153);
					T.r[3] = (uint16_t)(T.r[0] + 1);
					pokeb(0x01EB, (uint8_t)T.r[3]);
					T.r[0] = 0x1C00;
					set_bank_b(0x1C00);
					call(0x5B46, {});                  // rolling_sound
					blk(0x4F12, 132);
					T.r[0] = 0x2C00;
					set_bank_b(0x2C00);
					push(0x0052);
					call(0x8E78, {});                  // queue_sound
					blk(0x4F26, 36);
					if (!((int16_t)peek(0x01B4) > 0))
					{
						blk(0x4F2C, 33);
						poke(0x01B4, 0x0180);
					}
					blk(0x4F32, 18);
					T.r[6] = (uint16_t)(T.r[6] + 2);
				}
				blk(0x4F34, 42);
				if (peekb(0x01EB) == 3)
				{
					blk(0x4F3C, 60);
					T.r[2] = (uint16_t)(peek(0x02FC) - peek(0x01B4));
					if (T.r[2] & 0x8000)
					{
						blk(0x4F46, 24);
						T.r[2] = 0;
					}
					else
					{
						blk(0x4F4A, 30);
						if ((int16_t)T.r[2] > 0x0050)
						{
							blk(0x4F50, 18);
							T.r[2] = 0x0050;
						}
					}
				}
			}
		}
	}
	// friction
	blk(0x4F54, 36);
	if (peek(0x01B4))
	{
		blk(0x4F5A, 48);
		T.r[1] = 0;
		if ((int16_t)peek(0x01B4) < 0)
		{
			blk(0x4F68, 48);
			T.r[0] = (uint16_t)-peek(0x01B4);
			T.r[4] = T.r[0];
		}
		else
		{
			blk(0x4F62, 36);
			T.r[4] = peek(0x01B4);
		}
		blk(0x4F70, 165);
		T.r[3] = sext8(peekb(0x025C));
		T.r[5] = sext8(peekb(0x0327)) & 0x0007;
		T.r[0] = (uint16_t)(((T.r[5] << 2) + T.r[3]) << 1);
		bool table = false;
		if ((int16_t)T.r[4] > (int16_t)tbl((uint16_t)(0x7BE8 + T.r[0])))
		{
			blk(0x4F8C, 72);
			T.r[4] = peek(0x02EE);
			T.r[0] = peek(0x01B4);
			T.r[4] ^= T.r[0];
			if (!(T.r[4] & 0x8000))
			{
				table = true;
				blk(0x4F98, 36);
				T.r[0] = peek(0x02EE);
				if (T.r[0] & 0x8000)
				{
					blk(0x4F9E, 12);
					T.r[0] = (uint16_t)-T.r[0];
				}
				blk(0x4FA0, 54);
				T.r[4] = T.r[0];
				bool zero = false;
				if (peekb(0x0F59) == 3)
				{
					blk(0x4FBC, 75);
					T.r[0] = (uint16_t)(T.r[3] << 1);
					if ((int16_t)peek(0x01B4) >= (int16_t)tbl((uint16_t)(0x7C68 + T.r[0])))
						zero = true;
					else
					{
						blk(0x4FC8, 63);
						T.r[0] = tbl((uint16_t)(0x7C38 + (T.r[5] << 1)));
					}
				}
				else
				{
					blk(0x4FAA, 42);
					if (peek(0x01C4) & 0x000F)
						zero = true;
					else
					{
						blk(0x4FB2, 63);
						T.r[0] = tbl((uint16_t)(0x7C28 + (T.r[5] << 1)));
					}
				}
				if (zero)
				{
					blk(0x4FD2, 12);
					T.r[0] = 0;
				}
				blk(0x4FD4, 36);
				T.r[4] = (uint16_t)(T.r[4] - T.r[0]);
				T.r[1] = T.r[4];
				if (T.r[1] & 0x8000)
				{
					blk(0x4FDA, 24);
					T.r[1] = 0;
				}
			}
		}
		if (!table)
		{
			blk(0x4FDE, 42);
			if (!(peek(0x01C4) & 0x000F))
			{
				blk(0x4FE6, 39);
				T.r[1] = rb((uint16_t)(0x7C64 + T.r[3]));
			}
		}
		blk(0x4FEC, 36);
		if ((int16_t)peek(0x01B4) > 0)
		{
			blk(0x4FF2, 12);
			T.r[1] = (uint16_t)-T.r[1];
		}
		blk(0x4FF4, 12);
		T.r[2] = (uint16_t)(T.r[2] + T.r[1]);
	}
	// the speed limit of the slope
	blk(0x4FF6, 42);
	if (peekb(0x0327) & 7)
	{
		blk(0x5004, 12);
		T.r[1] = 0;
	}
	else
	{
		blk(0x4FFE, 30);
		T.r[1] = 0x0080;
	}
	blk(0x5006, 36);
	if ((int16_t)peek(0x01B4) < 0)
	{
		blk(0x5018, 87);
		poke(0x01B4, (uint16_t)(peek(0x01B4) + T.r[2]));
		T.r[3] = (uint16_t)-T.r[1];
		if (!((int16_t)peek(0x01B4) <= (int16_t)T.r[3]))
		{
			blk(0x5026, 27);
			poke(0x01B4, 0);
		}
	}
	else
	{
		blk(0x500C, 63);
		poke(0x01B4, (uint16_t)(peek(0x01B4) + T.r[2]));
		if (!((int16_t)peek(0x01B4) >= (int16_t)T.r[1]))
		{
			blk(0x5016, 12 + 27);
			poke(0x01B4, 0);
		}
	}
velocity:
	blk(0x502A, 30);
	call(0x91A6, {});                                  // skater_velocity
	blk(0x502E, 54);
	T.r[3] = sext8(peekb(0x0327));
	if (T.r[3] & 7)
	{
		blk(0x50B8, 102);
		push(peek(0x01B4));
		push(peek(0x02E6));
		call(0x80FA, {});                              // mul32
		blk(0x50C4, 102);
		pokeb(0x07F8, peekb(0x0205));
		poke(0x05B0, peek(0x0206));
		T.r[6] = (uint16_t)(T.r[6] + 4);
	}
	else
	{
		blk(0x5038, 36);
		if (!peekb(0x0EA0))
		{
			blk(0x503E, 222);
			pokeb(0x07F0, 0);
			pokeb(0x07F4, 0);
			poke(0x04F0, (uint16_t)(peek(0x04F0) + 8));
			poke(0x0570, (uint16_t)(peek(0x0570) + 8));
			poke(0x04F0, peek(0x04F0) & ~0x000F);
			poke(0x0570, peek(0x0570) & ~0x000F);
			bool level = !peek(0x01B4);
			if (!level)
			{
				blk(0x5064, 78);
				T.r[0] = asr(T.r[3], 3);
				level = T.r[0] == 0x000F;
			}
			if (level)
			{
				blk(0x50B2, 39);
				poke(0x05B0, 0);
			}
			else
			{
				blk(0x5072, 243);
				pokeb(0x07F8, 0);
				T.r[0] = (uint16_t)(asr(T.r[3], 3) << 1);
				T.r[1] = (uint16_t)(rw((uint16_t)(0xFCB0 + T.r[0])) - peek(0x06B0));
				T.r[0] = rb((uint16_t)(0x7CD0 + peekb(0x0F59)));
				if (T.r[1] & 0x8000)
				{
					blk(0x50A0, 48);
					T.r[3] = (uint16_t)-T.r[0];
					if (!((int16_t)T.r[1] >= (int16_t)T.r[3]))
					{
						blk(0x50A8, 12 + 12);
						T.r[0] = (uint16_t)-T.r[0];
						T.r[1] = T.r[0];
					}
				}
				else
				{
					blk(0x509A, 24);
					if (!((int16_t)T.r[1] <= (int16_t)T.r[0]))
					{
						blk(0x509E, 12 + 12);
						T.r[1] = T.r[0];
					}
				}
				blk(0x50AC, 39);
				poke(0x05B0, T.r[1]);
			}
		}
	}
	blk(0x50D2, 90);
	poke(0x05B2, peek(0x05B0));
	pokeb(0x07F9, peekb(0x07F8));
	goto sprites;
falling:
	blk(0x50E0, 81);
	poke(0x05B2, (uint16_t)(peek(0x05B2) - peek(0x02EE)));
	if ((int16_t)peek(0x05B2) < (int16_t)0xFE00)
	{
		blk(0x50EE, 42);
		if (peekb(0x0F59) != 3)
		{
			blk(0x50F6, 33);
			poke(0x05B2, 0xFE00);
		}
	}
sprites:
	blk(0x50FC, 42);
	if (peekb(0x032F) == 1)
	{
		blk(0x510A, 18);
		T.r[2] = 0x0040;
	}
	else
	{
		blk(0x5104, 30);
		T.r[2] = 0x00C0;
	}
	blk(0x510E, 513);
	uint8_t last = 0;
	for (uint16_t a = 0x0470; a <= 0x0473; a++)
	{
		T.r[0] = ((a & 1) ? sext8(peekb(a)) : peek(a)) & 0x003F;
		last = (uint8_t)(T.r[0] | (uint8_t)T.r[2]);
		pokeb(a, last);
	}
	f_nzvc(nzb(last));
}

// A7:45B0  enter_high_score(int first, long score): put score into the table of 15 entries
// from first (A7:43C0).  For a new entry the screen is cleared and the table shown (ranks,
// initials with the large font, scores by F:BA9A; the new place highlighted, colour 0x4000);
// unless the entry is only being shown again (0x0208 = -1 or bit 0 of the flag word set) the
// player enters three initials with the joystick (F:8D86 / F:8E2E; '?' deletes, 0x3F-0x5A
// wrap, 30 seconds counted down in the corner), else the previous initials are copied.  The
// entry is sent to the sound CPU, which keeps the table (command 0x0F / 0x10 by the flag
// word, F:8F50 / F:8F62).  When the player's slot 0x0F3A is set and first is 0 a "game
// over" screen (texts 0x78F8 / 0x21 or 0x32, delay of 600 frames) comes first.
// The routine's locals are on the stack as in the original (SP-relative offsets below are the
// original's).  R0 ends as 0x3C00.
uint16_t enter_high_score(const Args &)
{
	uint16_t saved[6];
	for (int i = 1; i <= 5; i++) saved[i] = T.r[i];
	auto at = [](int off) { return (uint16_t)(T.r[6] + off); };
	auto pop2 = []() { T.r[6] = (uint16_t)(T.r[6] + 2); };
	blk(0x45B4, 207 + 237);
	T.r[6] = (uint16_t)(T.r[6] - 0x0C);                // the frame of F:F162 (R1-R5, its return)
	T.r[6] = (uint16_t)(T.r[6] - 0x10);
	T.r[0] = 0x1C00;
	set_bank_b(0x1C00);
	for (int i = 0; i < 3; i++) push(peek(at(0x22)));
	T.r[0] = call(0x43C0, {});                         // insert_high_score
	blk(0x45D4, 120);
	const uint16_t place = T.r[0];
	T.r[2] = place;
	poke(0x0184, 0x5CD2);
	T.r[0] = peekb(0x01F5);
	if (peekb((uint16_t)(0x0F3A + T.r[0])))
	{
		blk(0x45E8, 39);
		if (!peek(at(0x24)))
		{
			blk(0x45EE, 30);
			call(0x8CE8, {});                          // clear_playfield
			blk(0x45F2, 30);
			call(0x8D2C, {});                          // clear_playfield_ram
			blk(0x45F6, 57);
			poke(at(0), 0x0045);
			call(0x8E78, {});                          // queue_sound
			blk(0x45FE, 90);
			poke(0x0188, 0xE000);
			poke(at(0), 0x78F8);
			call(0xA9FC, {});
			blk(0x460C, 36);
			if (!peekb(0x01F6))
			{
				blk(0x4612, 81);
				poke(0x0186, 0x275E);
				T.r[0] = 0x0021;
				call(0x8B52, {});                      // draw_large_char
				blk(0x4620, 12);
			}
			else
			{
				blk(0x4622, 48);
				T.r[0] = 0x0032;
				call(0x8AB6, {});
				blk(0x462A, 117);
				poke(0x0186, (uint16_t)(peek(0x0186) - 8));
				T.r[0] = (uint16_t)(peekb(0x01F5) + 0x0031);
				call(0x8C56, {});                      // draw_big_char
			}
			blk(0x463E, 57);
			poke(at(0), 0x0258);
			call(0x8D74, {});                          // wait 600 frames
		}
	}
	blk(0x4646, 69);
	poke(at(0x14), 0);
	if (!peek(at(0x24)))
	{
		blk(0x4650, 57);
		poke(at(0x14), (uint16_t)(peek(at(0x14)) + 1));
		poke(0x0208, place);
	}
	blk(0x4658, 24);
	if (place & 0x8000)
		blk(0x465C, 18);                               // JMP 0x4A4E
	else
	{
	// the table
	blk(0x4660, 30);
	call(0x8CE8, {});                                  // clear_playfield
	blk(0x4664, 30);
	call(0x8D2C, {});                                  // clear_playfield_ram
	blk(0x4668, 114);
	poke(0x0188, 0xE000);
	poke(at(0), 0x5F1E);
	push(0x0000);
	call(0xEAD4, {});                                  // draw_tile_block(0, 0x5F1E)
	blk(0x4678, 45);
	if (peek(at(0x16)) & 1)
	{
		blk(0x4680, 30);
		T.r[0] = 0x0035;
	}
	else
	{
		blk(0x4686, 18);
		T.r[0] = 0x0036;
	}
	blk(0x468A, 30);
	call(0x8AB6, {});
	blk(0x468E, 36);
	if (peekb(0x01F6))
	{
		blk(0x4694, 57);
		poke(at(0), 0x78D2);
		call(0xA9FC, {});
	}
	blk(0x469C, 132);
	poke(at(0x14), (uint16_t)(peek(at(0x26)) + 0x000E));
	T.r[4] = (uint16_t)(peek(at(0x26)) - 1);
	for (;;)
	{
		blk(0x4756, 51);
		T.r[4]++;
		if (!((int16_t)T.r[4] <= (int16_t)peek(at(0x14)))) break;
		blk(0x46B0, 24);
		if (T.r[4] == T.r[2])
		{
			blk(0x46B4, 45);
			poke(0x0188, 0x4000);
		}
		else
		{
			blk(0x46BC, 33);
			poke(0x0188, 0xE000);
		}
		blk(0x46C2, 312);
		T.r[1] = (uint16_t)(T.r[4] - peek(at(0x26)));
		T.r[0] = (uint16_t)(T.r[1] << 1);
		poke(0x0186, (uint16_t)(peek((uint16_t)(0x7FB8 + T.r[0])) + 0x1FF8));
		poke(at(0), (uint16_t)(T.r[1] + 1));
		push(0x0000);
		push(0x0002);
		push(0x0000);
		push(0x0000);
		T.r[0] = call(0xBA9A, {});                     // the rank
		blk(0x46EA, 30);
		if ((int16_t)T.r[1] >= 8)
		{
			blk(0x46F0, 33);
			poke(0x0186, (uint16_t)(peek(0x0186) + 4));
		}
		blk(0x46F6, 24);
		if (T.r[4] == T.r[2])
		{
			blk(0x4728, 33);
			poke(0x0186, (uint16_t)(peek(0x0186) + 0x000C));
		}
		else
		{
			blk(0x46FA, 78);
			T.r[0] = (uint16_t)(T.r[4] << 3);
			T.r[1] = T.r[0];
			for (T.r[3] = 4;;)
			{
				blk(0x4708, 63);
				T.r[0] = (uint16_t)(T.r[3] + T.r[1]);
				T.r[5] = sext8(peekb((uint16_t)(0x0370 + T.r[0])));
				if (!T.r[5])
				{
					blk(0x4712, 18);
					T.r[5] = 0x0020;
				}
				blk(0x4716, 54);
				T.r[0] = T.r[5] & 0x00FF;
				call(0x8B52, {});                      // draw_large_char (initial)
				blk(0x471E, 42);
				T.r[3]++;
				if (!((int16_t)T.r[3] <= 6)) break;
			}
			blk(0x4726, 12);
		}
		blk(0x472E, 276);
		poke(0x0186, (uint16_t)(peek(0x0186) + 4));
		T.r[0] = (uint16_t)(T.r[4] << 3);
		poke(at(0), peek((uint16_t)(0x0372 + T.r[0])));
		push(peek((uint16_t)(0x0370 + T.r[0])));
		push(0x0006);
		push(0x0001);
		push(peek(at(0)));
		T.r[0] = call(0xBA9A, {});                     // the score
		blk(0x4752, 18);
		T.r[6] = (uint16_t)(T.r[6] + 0x0010);
	}
	blk(0x475E, 333);
	poke(0x0188, 0x4000);
	T.r[0] = (uint16_t)(T.r[2] << 3);
	poke(at(0x08), T.r[0]);                            // offset of the new entry
	poke(at(0x10), (uint16_t)(T.r[2] - peek(at(0x26))));   // its place on the screen
	T.r[0] = (uint16_t)(peek(at(0x10)) << 1);
	poke(at(0x12), (uint16_t)(0x7FB8 + T.r[0]));       // its screen position
	bool enter = peek(0x0208) == 0xFFFF;
	if (!enter)
	{
		blk(0x4792, 45);
		enter = peek(at(0x16)) & 1;
	}
	if (enter)
	{
		blk(0x479E, 75);
		T.r[0] = peekb(0x01F5);
		if (!peekb((uint16_t)(0x0F3A + T.r[0])))
		{
			blk(0x47AA, 57);
			poke(at(0), 0x0045);
			call(0x8E78, {});                          // queue_sound
		}
		blk(0x47B2, 57);
		poke(at(0), 0x78DC);
		call(0xA9FC, {});
		blk(0x47BA, 57);
		poke(at(0), 0x78C8);
		call(0xA9FC, {});
		blk(0x47C2, 300);
		poke(0x0188, 0x4000);
		T.r[0] = peek(at(0x08));
		for (int k = 4; k <= 6; k++) pokeb((uint16_t)(0x0370 + k + T.r[0]), 0x20);
		poke(at(0x0C), 0);                             // initial being entered
		pokeb(at(0x0A), 0x41);                         // the letter
		pokeb(at(0x0E), 0x1E);                         // seconds left
		call(0x8D86, {});
		blk(0x47F2, 30);
		call(0x8E2E, {});
		for (;;)
		{
			blk(0x47F6, 45);
			if ((int16_t)peek(at(0x0C)) >= 3)
			{
				blk(0x47FE, 18);
				break;
			}
			blk(0x4802, 39);
			if (peekb(at(0x0E)) & 0x80)
			{
				blk(0x47FE, 18);
				break;
			}
			blk(0x4808, 30);
			call(0x8D50, {});                          // wait_vblank
			blk(0x480C, 42);
			if (!(peekb(0x01C4) & 0x3F))
			{
				blk(0x4814, 99);
				T.r[0] = (uint16_t)(sext8(peekb(at(0x0E))) - 1);
				pokeb(at(0x0E), (uint8_t)T.r[0]);
				if ((int8_t)T.r[0] < 0x0A)
				{
					blk(0x4824, 24);
					if (!((int8_t)T.r[0] < 0))
					{
						blk(0x4828, 63);
						poke(0x0188, 0xE000);
						if ((uint8_t)T.r[0] == 9)
						{
							blk(0x4834, 81);
							poke(at(0), 0x78E6);
							push(0x0000);
							call(0x4D30, {});          // message_box
							blk(0x483E, 18);
							pop2();
						}
						blk(0x4840, 108);
						poke(0x0186, 0x20F0);
						T.r[0] = (uint16_t)(sext8(peekb(at(0x0E))) + 0x0030);
						call(0x8C56, {});              // draw_big_char
						blk(0x4852, 33);
						poke(0x0188, 0x4000);
					}
				}
			}
			blk(0x4858, 30);
			call(0x8D86, {});
			blk(0x485C, 30);
			T.r[0] = call(0x8E2E, {});                 // joystick step
			blk(0x4860, 111);
			T.r[1] = peekb(at(0x0A));
			T.r[0] = (uint16_t)(T.r[0] + T.r[1]);
			pokeb(at(0x0A), (uint8_t)T.r[0]);
			if ((uint8_t)T.r[0] < 0x3F)
			{
				blk(0x4872, 36);
				pokeb(at(0x0A), 0x5A);
			}
			blk(0x4878, 45);
			if (peekb(at(0x0A)) > 0x5A)
			{
				blk(0x4880, 36);
				pokeb(at(0x0A), 0x3F);
			}
			blk(0x4886, 201);
			T.r[1] = (uint16_t)(peek(at(0x0C)) << 2);
			T.r[0] = (uint16_t)(T.r[1] + peek(peek(at(0x12))));
			poke(0x0186, (uint16_t)(T.r[0] + 0x2000));
			if ((int16_t)peek(at(0x10)) >= 8)
			{
				blk(0x48A6, 33);
				poke(0x0186, (uint16_t)(peek(0x0186) + 4));
			}
			blk(0x48AC, 57);
			T.r[2] = 0;
			if (peekb(at(0x0A)) < 0x41)
			{
				blk(0x48B6, 81);
				T.r[2]++;
				T.r[0] = (uint16_t)(peekb(at(0x0A)) - 0x0020);
			}
			else
			{
				blk(0x48C4, 39);
				T.r[0] = peekb(at(0x0A));
			}
			blk(0x48CA, 30);
			call(0x8B52, {});                          // draw_large_char: the letter
			blk(0x48CE, 36);
			if (!(peekb(0x0193) & 0x80))
			{
				blk(0x495A, 27 + 18);
				pokeb(0x0190, 0);
				continue;
			}
			blk(0x48D4, 36);
			if (peekb(0x0190)) continue;               // button still held
			blk(0x48DA, 45);
			if (peekb(at(0x0A)) == 0x3F)
			{
				// delete the last initial
				blk(0x48E2, 39);
				if ((int16_t)peek(at(0x0C)) > 0)
				{
					blk(0x48E8, 225);
					pokeb(at(0x0A), 0x20);
					T.r[0] = (uint16_t)(T.r[1] + peek(peek(at(0x12))));
					poke(0x0186, (uint16_t)(T.r[0] + 0x2000));
					T.r[0] = (uint16_t)(peek(at(0x14)) - peek(at(0x26)));
					if (!((int16_t)T.r[0] < 8))
					{
						blk(0x490C, 33);
						poke(0x0186, (uint16_t)(peek(0x0186) + 4));
					}
					blk(0x4912, 69);
					T.r[0] = peekb(at(0x0A));
					call(0x8B52, {});                  // draw_large_char: blank
					blk(0x491C, 141);
					T.r[0] = (uint16_t)(peek(at(0x08)) + peek(at(0x0C)));
					pokeb((uint16_t)(0x0374 + T.r[0]), peekb(at(0x0A)));
					poke(at(0x0C), (uint16_t)(peek(at(0x0C)) - 1));
				}
			}
			else
			{
				blk(0x4930, 24);
				if (T.r[2] & 1)
				{
					blk(0x4934, 36);
					pokeb(at(0x0A), 0x20);
				}
				T.r[2] >>= 1;
				blk(0x493A, 129);
				T.r[0] = (uint16_t)(peek(at(0x08)) + peek(at(0x0C)));
				pokeb((uint16_t)(0x0374 + T.r[0]), peekb(at(0x0A)));
				poke(at(0x0C), (uint16_t)(peek(at(0x0C)) + 1));
			}
			blk(0x494C, 81 + 18);
			pokeb(at(0x0A), 0x41);
			pokeb(0x0190, 1);
		}
	}
	else
	{
		// shown again: the initials of the earlier entry 0x0208
		blk(0x479A, 18 + 126);
		poke(0x0186, (uint16_t)(peek(peek(at(0x12))) + 0x2000));
		if ((int16_t)peek(at(0x10)) >= 8)
		{
			blk(0x4976, 33);
			poke(0x0186, (uint16_t)(peek(0x0186) + 4));
		}
		blk(0x497C, 12);
		for (T.r[1] = 0;;)
		{
			blk(0x497E, 216);
			T.r[2] = (uint16_t)(T.r[1] + peek(at(0x08)));
			T.r[0] = (uint16_t)((peek(0x0208) << 3) + T.r[1]);
			T.r[0] = peekb((uint16_t)(0x0374 + T.r[0]));
			pokeb((uint16_t)(0x0374 + T.r[2]), (uint8_t)T.r[0]);
			call(0x8B52, {});                          // draw_large_char
			blk(0x49A0, 42);
			T.r[1]++;
			if (!((int16_t)T.r[1] <= 2)) break;
		}
		blk(0x49A8, 57);
		poke(at(0), 0x012C);
		call(0x8D74, {});                              // wait 300 frames
	}
	// to the sound CPU
	{
		blk(0x49B0, 30);
		call(0x8E6A, {});                              // set_flag_01a4
		const bool flag = peek(at(0x16)) & 1;
		blk(0x49B4, 45);
		if (flag)
		{
			blk(0x49BC, 39);
			poke(at(0), 0x000F);
		}
		else
		{
			blk(0x49C2, 27);
			poke(at(0), 0x0010);
		}
		blk(0x49C6, 30);
		call(0x8EA2, {});                              // send_sound_command
		blk(0x49CA, 30);
		call(0x8F50, {});
		blk(0x49CE, 45);
		auto send = [&](uint16_t pc, int cost, uint16_t a) {
			blk(pc, cost);
			poke(at(0), 0);
			pokeb(at(0), peekb((uint16_t)(a + peek(at(0x08)))));
			call(0x8EA2, {});
		};
		if (flag)
		{
			send(0x49D6, 114, 0x0370);
			send(0x49E4, 114, 0x0373);
			send(0x49F2, 96 + 30, 0x0372);
		}
		else
		{
			send(0x49FE, 114, 0x0372);
			send(0x4A0C, 84 + 30, 0x0373);
		}
		send(0x4A1A, 114, 0x0374);
		send(0x4A28, 114, 0x0375);
		send(0x4A36, 114, 0x0376);
		blk(0x4A44, 30);
		call(0x8F62, {});
		blk(0x4A48, 30);
		call(0x8E72, {});                              // clear_flag_01a4
		blk(0x4A4C, 18);
		pop2();
	}
	}
	blk(0x4A4E, 162 - 18);
	poke(0x0184, 0x5AC8);
	T.r[0] = 0x3C00;
	set_bank_b(0x3C00);
	T.r[6] = (uint16_t)(T.r[6] + 0x0016 + 0x000C);
	for (int i = 1; i <= 5; i++) T.r[i] = saved[i];
	return 0x3C00;
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
	auto at = [](int off) { return (uint16_t)(T.r[6] + off); };
	auto pop = [](int n) { T.r[6] = (uint16_t)(T.r[6] + n); };
	// The original jumps into the B3 code with its local word (and two argument words) still
	// above the return address; the B3 code pops them and returns to our caller.  tail_jump()
	// expects the return address on top of the stack, so the jump is made here the same way:
	// the B3 code runs from addr and returns to the caller's address; the return address is
	// then pushed back (the same word) for the return that ends this native routine.
	const uint16_t entry_sp = T.r[6];
	auto jump = [&](uint16_t addr) {
		const uint16_t ret = peek(entry_sp);
		T.r[7] = addr;
		shadow_push(ret);
		dispatch();
		shadow_pop();
		if (T.r[7] != ret) throw Unwound{};
		push(ret);
	};
	// this part runs every frame: interrupts may come in after each instruction, as in the
	// original
	T.r[6] = (uint16_t)(T.r[6] - 2);                   // TST -(SP): the local word
	tick(0x5BBA, 21);
	T.r[0] = 0;
	tick(0x5BBC, 12);
	T.r[0] = peekb(0x018F);
	tick(0x5BC0, 24);
	tick(0x5BC2, 12);
	if (T.r[0])
	{
		tick(0x5BC4, 12);
		tick(0x5BC8, 18);
	}
	else
		tick(0x5BC8, 12);
	if (T.r[0] == 0 || T.r[0] == 1)
	{
		tick(0x5BCA, 12);
		tick(0x62E6, 18);
		jump(0x62E6);
		return;
	}
	tick(0x5BCE, 12);
	blk(0x5BCE, 36);
	if (!peek(0x0F22))
	{
		blk(0x5BD4, 30);
		if (T.r[0] == 8)
		{
			blk(0x5BDA, 30);
			T.r[0] = call(0xA8FE, {});
			blk(0x5BDE, 153);
			T.r[0] = (uint16_t)((peekb(0x01F3) >> 4) & 3);
			if (peekb(0x027B) > rb((uint16_t)(0xFE9E + T.r[0])))
			{
				blk(0x5C60, 33);
				pokeb(0x018F, 5);
			}
			else
			{
				// the continue screen
				blk(0x5BF8, 30);
				call(0xE106, {});
				blk(0x5BFC, 30);
				call(0x820C, {});                      // install_interrupts_b
				blk(0x5C00, 30);
				call(0x435A, {});                      // read_switch_settings
				blk(0x5C04, 87);
				poke(0x01B4, 0);
				push(0x0070);
				call(0x8E78, {});                      // queue_sound
				blk(0x5C10, 57);
				poke(at(0), 0x0076);
				call(0x8E78, {});
				blk(0x5C18, 57);
				poke(at(0), 0x0053);
				call(0x8E78, {});
				blk(0x5C20, 57);
				poke(at(0), 0x7708);
				call(0xA9FC, {});
				blk(0x5C28, 87);
				poke(at(0), 0x5F06);
				push(0x0001);
				call(0xEAD4, {});
				blk(0x5C34, 114);
				poke(0x0F1C, 0x000A);
				poke(at(0), 0x76FC);
				push(0x0000);
				call(0x4D30, {});                      // message_box(draw)
				blk(0x5C44, 63);
				pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) | 0x10));
				call(0x79D4, {});
				blk(0x5C4E, 96);
				pokeb(0x0F1E, 0x3C);
				pokeb(0x018F, 9);
				pop(6);
			}
		}
	}
	blk(0x5C66, 42);
	if (peekb(0x018F) == 9)
	{
		// the continue countdown
		blk(0x5C72, 75);
		poke(0x0188, 0xE000);
		if (peekb(0x0F1E) == 0x3C)
		{
			blk(0x5C80, 42);
			if (peek(0x0F1C) & 1)
			{
				blk(0x5C88, 84);
				T.r[0] = peekb(0x01F3) & 0x00C0;
				if (T.r[0] == 0x0080)
				{
					blk(0x5C98, 30);
					T.r[0] = 0x006C;
				}
				else
				{
					blk(0x5C9E, 18);
					T.r[0] = 0x006B;
				}
				blk(0x5CA2, 30);
				call(0x8AB6, {});
				blk(0x5CA6, 12);
			}
			else
			{
				blk(0x5CA8, 30);
				call(0x435A, {});                      // read_switch_settings
				blk(0x5CAC, 144);
				poke(at(0), 0);
				pokeb(at(0), peekb(0x01F9));
				poke(at(0), asr(peek(at(0)), 2));
				T.r[0] = 0x0066;
				call(0x8AB6, {});
				blk(0x5CBE, 201);
				poke(0x0186, (uint16_t)(peek(0x0186) - 0x002C));
				push(peek(at(0)));
				push(0x0000);
				push(0x0002);
				push(0x0001);
				push(0x0000);
				call(0xBA9A, {});
				blk(0x5CD6, 42);
				if (peekb(0x01F9) & 3)
				{
					blk(0x5CDE, 534);
					T.r[0] = sext8(peekb(0x01FA)) & 3;
					pokeb(at(0x0A), peekb((uint16_t)(0x5A49 + T.r[0])));
					pokeb(at(0x0B), 0);
					T.r[0] = (uint16_t)((sext8(peekb(0x01F9)) & 3) << 1);
					T.r[0] = (uint16_t)(T.r[0] + peek(at(0x0A)));
					poke(at(0x0A), T.r[0]);
					poke(at(0x0A), (uint16_t)(peek(at(0x0A)) - 2));
					T.r[0] = (uint16_t)(peek(at(0x0A)) << 1);
					T.r[1] = peek((uint16_t)(0x5A66 + T.r[0])) | peek(0x0188);
					poke(peek(0x0186), T.r[1]);
					T.r[0] = peek(0x0186);
					T.r[1] = (uint16_t)(peek(at(0x0A)) << 1);
					T.r[2] = peek((uint16_t)(0x5A68 + T.r[1])) | peek(0x0188);
					poke((uint16_t)(T.r[0] + 0x0080), T.r[2]);
				}
				blk(0x5D30, 36);
				bool few = false;
				if (peekb(0x01F9))
				{
					blk(0x5D36, 42);
					few = peekb(0x01F9) <= 4;
				}
				if (few)
				{
					blk(0x5D3E, 30);
					T.r[0] = 0x0069;
				}
				else
				{
					blk(0x5D44, 18);
					T.r[0] = 0x0067;
				}
				blk(0x5D48, 30);
				call(0x8AB6, {});
				blk(0x5D4C, 39);
				bool enough;
				if ((int16_t)peek(at(0x0A)) > 0)
				{
					blk(0x5D52, 84);
					enough = (peekb(0x01F3) & 0x80) != 0x012C;
					if (!enough)
					{
						blk(0x5D62, 45);
						enough = !((int16_t)peek(at(0x0A)) < 2);
					}
				}
				else
				{
					blk(0x5D62, 45);
					enough = !((int16_t)peek(at(0x0A)) < 2);
				}
				if (enough)
				{
					blk(0x5D6A, 30);
					T.r[0] = 0x006A;
				}
				else
				{
					blk(0x5D70, 18);
					T.r[0] = 0x0068;
				}
				blk(0x5D74, 30);
				call(0x8AB6, {});
				blk(0x5D78, 18);
				pop(0x0A);
			}
		}
		blk(0x5D7C, 36);
		if (!peekb(0x0F1E))
		{
			// time up
			blk(0x5D82, 90);
			push(0x7708);
			push(0x0001);
			call(0x4D30, {});                          // message_box(erase)
			blk(0x5D8E, 90);
			poke(0x0F1C, 0);
			pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) | 0x10));
			call(0x79D4, {});
			blk(0x5D9C, 69);
			pokeb(0x018F, 5);
			pop(4);
		}
		else
		{
			blk(0x5DA6, 42);
			if (!((int16_t)peek(0x0F1C) > 9))
			{
				blk(0x5DAE, 30);
				T.r[0] = call(0xE78E, {});
				blk(0x5DB2, 24);
				if (T.r[0])
				{
					// continue
					blk(0x5DB6, 204);
					poke(0x0F1C, 0);
					pokeb(0x0F1E, 0);
					pokeb(0x0FA2, peekb(0x0FA2) & ~0x10);
					pokeb(0x027B, (uint8_t)(peekb(0x027B) + 1));
					push(0x7708);
					push(0x0001);
					call(0x4D30, {});                  // message_box(erase)
					blk(0x5DD4, 87);
					poke(at(0), 0x76FC);
					push(0x0001);
					call(0x4D30, {});
					blk(0x5DE0, 114);
					poke(0x0188, 0x4000);
					poke(at(0), 0x5DEE);
					push(0x0000);
					call(0xEAD4, {});
					blk(0x5DF0, 51);
					poke(at(0), 0);
					call(0xF028, {});                  // add_time(0)
					blk(0x5DF6, 36);
					if (!peekb(0x025B))
					{
						blk(0x5DFC, 57);
						poke(at(0), 0x0001);
						call(0x72CA, {});
						blk(0x5E04, 114);
						T.r[1] = 0x024E;
						T.r[2] = 0x024C;
						T.r[3] = peek(0x0240);
						T.r[4] = peek(0x023E);
						call(0x80D0, {});              // sub32: score left to the goal
						blk(0x5E18, 36);
						T.r[0] = peek(0x024E);
						bool passed = (T.r[0] & 0x8000) != 0;
						if (!passed)
						{
							blk(0x5E1E, 12);
							if (!T.r[0])
							{
								blk(0x5E20, 36);
								passed = !peek(0x024C);
							}
						}
						if (passed)
						{
							blk(0x5E26, 369);
							poke(0x024C, peek(0x0244));
							poke(0x024E, peek(0x0246));
							pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) | 0x02));
							if (peekb(0x025A) < 4) pokeb(0x025A, (uint8_t)(peekb(0x025A) + 1));
							T.r[0] = (uint16_t)((peekb(0x025A) + peekb(0x0FE3)) << 1);
							T.r[1] = 0x0246;
							T.r[2] = 0x0244;
							T.r[3] = 0;
							T.r[4] = peek((uint16_t)(0x5212 + T.r[0]));
							call(0x80C8, {});          // add32: the next goal
							blk(0x5E64, 78);
							poke(0x024A, peek(0x0246));
							poke(0x0248, peek(0x0244));
						}
					}
					blk(0x5E70, 30);
					T.r[0] = call(0x81AA, {});         // install_interrupts_a
					blk(0x5E74, 45);
					pokeb(0x018F, 0);
					pop(8);
				}
			}
		}
	}
	else
		blk(0x5C6E, 18);                               // JMP 0x5E7C
	blk(0x5E7C, 42);
	if (peekb(0x018F) != 5)
	{
		blk(0x5E84, 18);
		jump(0x6062);
		return;
	}
	// the end of the game
	blk(0x5E88, 90);
	push(0x7708);
	push(0x0001);
	call(0x4D30, {});                                  // message_box(erase)
	blk(0x5E94, 57);
	poke(at(0), 0x0005);
	call(0x8E78, {});                                  // queue_sound
	blk(0x5E9C, 30);
	call(0x820C, {});                                  // install_interrupts_b
	blk(0x5EA0, 57);
	poke(at(0), 0x76E0);
	call(0xA9FC, {});
	blk(0x5EA8, 36);
	if (!peekb(0x018E))
	{
		blk(0x5EAE, 84);
		if ((peekb(0x01F3) & 0x30) != 0x10)
		{
			blk(0x5EBE, 57);
			poke(at(0), 0x78EC);
			call(0xA9FC, {});
			blk(0x5EC6, 57);
			poke(at(0), 0x0078);
			call(0x8D74, {});                          // wait 120 frames
		}
	}
	blk(0x5ECE, 57);
	poke(at(0), 0x0078);
	call(0x8D74, {});                                  // wait 120 frames
	blk(0x5ED6, 36);
	if ((int16_t)peek(0x0256) > 0)
	{
		// the cash into the score
		blk(0x5EDC, 87);
		poke(at(0), 0x78EC);
		push(0x0001);
		call(0x4D30, {});                              // message_box(erase)
		blk(0x5EE8, 57);
		poke(at(0), 0x76E8);
		call(0xA9FC, {});
		for (;;)
		{
			blk(0x5EF0, 36);
			if (!peek(0x0256)) break;
			blk(0x5EF6, 84);
			poke(0x0256, (uint16_t)(peek(0x0256) - 1));
			poke(at(0), 0x000A);
			call(0xEF24, {});                          // add_score
			blk(0x5F02, 42);
			if ((int16_t)peek(0x0256) > 0x000A)
			{
				blk(0x5F0A, 90);
				poke(0x0256, (uint16_t)(peek(0x0256) - 0x000A));
				poke(at(0), 0x0064);
				call(0xEF24, {});
			}
			blk(0x5F18, 63);
			pokeb(0x0FA2, 9);
			call(0x77B4, {});                          // update_status_display
			blk(0x5F22, 57);
			poke(at(0), 0x0001);
			call(0x8D74, {});                          // wait a frame
			blk(0x5F2A, 12);
		}
		blk(0x5F2C, 57);
		poke(at(0), 0x003C);
		call(0x8D74, {});                              // wait 60 frames
		blk(0x5F34, 18);
		pop(2);
	}
	blk(0x5F36, 30);
	call(0x6E92, {});
	blk(0x5F3A, 36);
	if (peekb(0x01F6))
	{
		blk(0x5F40, 42);
		if (peekb(0x0F2B) != 4)
		{
			jump(0x601E);
			return;
		}
	}
	// the results to the sound CPU
	blk(0x5F48, 30);
	call(0x8E6A, {});                                  // set_flag_01a4
	blk(0x5F4C, 57);
	poke(at(0), 0x000E);
	call(0x8EA2, {});                                  // send_sound_command
	blk(0x5F54, 30);
	call(0x8F50, {});
	blk(0x5F58, 36);
	if (peekb(0x01F5))
	{
		blk(0x5F66, 36);
		T.r[0] = peekb(0x0F2F);
	}
	else
	{
		blk(0x5F5E, 48);
		T.r[0] = peekb(0x0F2E);
	}
	blk(0x5F6C, 72);
	poke(at(0), (uint16_t)(T.r[0] - 1));
	call(0x8EA2, {});
	blk(0x5F74, 36);
	if (peekb(0x01F5))
	{
		blk(0x5F82, 36);
		T.r[0] = peekb(0x0F2E);
	}
	else
	{
		blk(0x5F7A, 48);
		T.r[0] = peekb(0x0F2F);
	}
	blk(0x5F88, 72);
	poke(at(0), (uint16_t)(T.r[0] - 1));
	call(0x8EA2, {});
	blk(0x5F90, 30);
	call(0x8F62, {});
	blk(0x5F94, 30);
	call(0x8E72, {});                                  // clear_flag_01a4
	blk(0x5F98, 69);
	pokeb(0x018E, 1);
	if (peekb(0x01F6))
	{
		blk(0x5FA4, 48);
		bool second_leads = (int16_t)peek(0x027E) > (int16_t)peek(0x0240);
		if (!second_leads)
		{
			blk(0x5FAC, 12);
			if (peek(0x027E) == peek(0x0240))
			{
				blk(0x5FAE, 48);
				second_leads = !((int16_t)peek(0x027C) <= (int16_t)peek(0x023E));
			}
		}
		if (second_leads)
		{
			blk(0x5FB6, 30);
			call(0xE730, {});
		}
	}
	// the high score tables of each player
	blk(0x5FBA, 60);
	T.r[1] = peekb(0x01F6);
	if (!(T.r[1] & 0x8000))
	{
		do
		{
			blk(0x5FC4, 30);
			call(0x4476, {});                          // read_high_scores
			blk(0x5FC8, 123);
			poke(at(0), peek(0x0240));
			push(peek(0x023E));
			push(0x0000);
			call(0x45B0, {});                          // enter_high_score(0, score)
			blk(0x5FD6, 117);
			poke(at(0), 0);
			push(peek(0x0242));
			push(0x000F);
			call(0x45B0, {});                          // enter_high_score(15, parks)
			blk(0x5FE4, 36);
			if (peekb(0x01F6))
			{
				blk(0x5FEA, 30);
				call(0xE730, {});
			}
			blk(0x5FEE, 42);
			pop(8);
			T.r[1]--;
		} while (!(T.r[1] & 0x8000));
	}
	blk(0x5FF6, 60);
	poke(0x0F18, 0xFFF6);
	pokeb(0x0F2B, 0);
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
	c_routine("A7", 0x538E, "terrain_event", terrain_event, 0, 39),
	asm_routine("A7", 0x572A, "shapes_touch", shapes_touch, 39),
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
	c_routine("A5", 0x53A8, "trick_landing", trick_landing, 0, 87),
	c_routine("A5", 0x5462, "trick_step", trick_step, 0, 0),
	c_routine("A5", 0x5264, "start_trick", start_trick, 0, 69),
	c_routine("A5", 0x54CE, "trick_moves", trick_moves, 0, 69),
	c_routine("A7", 0x4A66, "velocity_direction", velocity_direction, 0, 0),
	asm_routine("A5", 0x4A2A, "skater_physics", skater_physics, 0),
	wait_c_routine("A7", 0x45B0, "enter_high_score", enter_high_score, 111),
	wait_routine("A7", 0x5BB8, "game_over_states", game_over_states, 0),
)

}
