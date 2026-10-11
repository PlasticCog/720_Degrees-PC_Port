// Rewritten routines of the banked segments A5 and A7: the Skate City map view, object
// motion, sounds driven by the game state, and the message boxes of the text layer.
#include "logic.h"

namespace logic {
namespace {

using namespace t11rt;

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
	uint16_t r0;
	cyc(39);
	if (a.s(0) > 0)
	{
		cyc(279);
		r0 = (uint16_t)(peekb(0x0F44) + 0x20);
		pokeb(0x0F44, (uint8_t)r0);
		poke(0x0F46, (uint16_t)(peek(0x0F46) + 2));
		r0 = (uint16_t)(peekb(0x0F44) + 0xA0);
		pokeb(0x0F45, (uint8_t)r0);
		poke(0x0F48, (uint16_t)(peek(0x0F46) + 10));
	}
	else
	{
		cyc(180);
		r0 = (uint16_t)(peekb(0x0F44) - 0x20);
		pokeb(0x0F44, (uint8_t)r0);
		poke(0x0F46, (uint16_t)(peek(0x0F46) - 2));
		pokeb(0x0F45, (uint8_t)r0);
		poke(0x0F48, peek(0x0F46));
	}
	cyc(153);
	poke(0x0F42, peek(0x0F40));
	pokeb(0x0F5D, 8);
	pokeb(0x0F5C, 0);
	poke(0x0F4A, 0xF800);
	return r0;
}

// A5:42BC (assembly)  step_map_row(int dir): move the row strip one cell up (dir > 0) or down
// and set up a horizontal strip of 6 cells.  A row wraps around the map (0x0F5B counts it,
// 0x099E rows); R0/R1 are left as the computation leaves them.
void step_map_row()
{
	const int16_t dir = (int16_t)rw((uint16_t)(T.r[6] + 2));
	cyc(39);
	if (dir > 0)
	{
		cyc(111);
		poke(0x0F40, (uint16_t)(peek(0x0F40) - 0x0800));
		T.r[1] = peekb(0x0F5A);
		if (peek(0x0F40) & 0x0800)
		{
			cyc(102);
			poke(0x0F46, (uint16_t)(peek(0x0F46) - T.r[1]));
			T.r[0] = (uint16_t)(sext8(peekb(0x0F5B)) - 1);
			pokeb(0x0F5B, (uint8_t)T.r[0]);
			if ((int8_t)T.r[0] < 0)
			{
				cyc(135);
				pokeb(0x0F5B, 0x17);
				T.r[0] = (uint16_t)(peek(0x0F46) - 0x0180);
				poke(0x0F46, (uint16_t)(0x0A6C + T.r[0]));
			}
			cyc(48);
			T.r[0] = (uint16_t)(T.r[1] * 3);
		}
		else
		{
			cyc(36);
			T.r[0] = (uint16_t)(T.r[1] << 2);
		}
		cyc(150);
		poke(0x0F48, (uint16_t)(peek(0x0F46) - T.r[0]));
		poke(0x0F42, (uint16_t)(peek(0x0F40) - 0x3800));
	}
	else
	{
		cyc(75);
		poke(0x0F40, (uint16_t)(peek(0x0F40) + 0x0800));
		if (!(peek(0x0F40) & 0x0800))
		{
			cyc(210);
			T.r[0] = peekb(0x0F5A);
			poke(0x0F46, (uint16_t)(peek(0x0F46) + T.r[0]));
			T.r[0] = (uint16_t)(sext8(peekb(0x0F5B)) + 1);
			pokeb(0x0F5B, (uint8_t)T.r[0]);
			T.r[0] = (uint16_t)(peek(0x099E) << 1);
			T.r[1] = sext8(peekb(0x0F5B));
			if (T.r[1] == T.r[0])
			{
				cyc(114);
				pokeb(0x0F5B, 0);
				T.r[0] = (uint16_t)(peek(0x0F46) - 0x0A80 + 0x0194);
				poke(0x0F46, T.r[0]);
			}
		}
		cyc(78);
		poke(0x0F42, peek(0x0F40));
		poke(0x0F48, peek(0x0F46));
	}
	cyc(153);
	pokeb(0x0F45, peekb(0x0F44));
	pokeb(0x0F5C, 0x20);
	poke(0x0F4A, 0);
	pokeb(0x0F5D, 6);
	f_nzvc(0);
}

// A5:41CE (assembly): draw the cells of the pending strip (F:E82E draws one cell), then start
// the next strip if the scroll position has moved a cell away from the drawn area (0x0F3E
// rows, 0x0F3C columns).  Registers end as the callees leave them; the flags are those of the
// final stack word (the row or column difference).
void draw_map_strip()
{
	blk(0x41CE, 57);
	T.r[6] = (uint16_t)(T.r[6] - 2);
	const uint16_t local = T.r[6];
	poke(local, 0);
	pokeb(local, peekb(0x0F5D));
	blk(0x41D4, 30);                                           // 41D4: TST (SP)
	if (peek(local))
	{
		do
		{
			blk(0x41D8, 90);
			pokeb(0x0F5D, (uint8_t)(peekb(0x0F5D) - 1));
			poke(0x0F42, peek(0x0F42) & ~0xC001);
			call(0xE82E, {});
			blk(0x41E6, 141);
			poke(0x0F42, (uint16_t)(peek(0x0F42) + peek(0x0F4A)));
			T.r[1] = (uint16_t)~peek(0x0F4A);
			T.r[0] = peek(0x0F42) & ~T.r[1];
			if (T.r[0] & 0x0800)
			{
				blk(0x41FE, 96);
				T.r[0] = peekb(0x0F5A);
				poke(0x0F48, (uint16_t)(peek(0x0F48) - T.r[0]));
				poke(0x0F56, (uint16_t)(peek(0x0F56) - 6));
			}
			blk(0x420E, 123);
			T.r[1] = sext8(peekb(0x0F5C));
			T.r[0] = (uint16_t)(peekb(0x0F45) + T.r[1]);
			pokeb(0x0F45, (uint8_t)T.r[0]);
			if ((uint8_t)T.r[1])
			{
				blk(0x4222, 93);
				poke(0x0F48, (uint16_t)(peek(0x0F48) + 2));
				T.r[0] = peekb(0x0F58);
				if (T.r[0])
				{
					blk(0x4232, 63);
					T.r[1] = (uint16_t)(T.r[0] << 1);
					pokeb(0x0F58, (uint8_t)T.r[1]);
					if (!(uint8_t)T.r[1])
					{
						blk(0x423C, 54);
						pokeb(0x0F58, (uint8_t)(peekb(0x0F58) + 1));
						poke(0x0F56, (uint16_t)(peek(0x0F56) + 1));
					}
				}
			}
			blk(0x4244, 84);
			poke(local, 0);
			pokeb(local, peekb(0x0F5D));
		} while (peek(local));
		blk(0x424E, 39 + 30);                                  // 424E: INCB, BR; 41D4: TST (SP)
		pokeb(0x0F5E, (uint8_t)(peekb(0x0F5E) + 1));
	}
	blk(0x4254, 126);
	T.r[0] = (uint16_t)((peek(0x01AA) + 0x0800) & ~0x0FFF);
	poke(local, (uint16_t)(peek(0x0F3E) - T.r[0]));
	if (peek(local))
	{
		blk(0x4268, 60);
		call(0x42BC, {peek(local)});                   // step_map_row
		blk(0x426E, 39 + 45);
		if ((int16_t)peek(local) >= 0) poke(0x0F3E, (uint16_t)(peek(0x0F3E) - 0x1000));
		else poke(0x0F3E, (uint16_t)(peek(0x0F3E) + 0x1000));
	}
	else
	{
		blk(0x4284, 117);
		T.r[0] = (uint16_t)(((peek(0x01A8) + 0x1000) & ~0x1FFF) - peek(0x0F3C));
		poke(local, T.r[0]);
		if (!T.r[0])
		{
			blk(0x42B8, 39);
			T.r[6] = (uint16_t)(T.r[6] + 2);
			fl::tst(peek(local));
			return;
		}
		blk(0x4298, 54);
		call(0x438C, {T.r[0]});                        // step_map_column
		blk(0x429E, 39);
		if ((int16_t)peek(local) > 0)
		{
			blk(0x42A4, 45);
			poke(0x0F3C, (uint16_t)(peek(0x0F3C) + 0x2000));
		}
		else
		{
			blk(0x42AC, 33);
			poke(0x0F3C, (uint16_t)(peek(0x0F3C) - 0x2000));
		}
	}
	blk(0x42B2, 30);
	call(0x4894, {});                                  // town_alt_cell_bit
	blk(0x42B6, 18 + 39);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	fl::tst(peek(local));
}

// A5:4000  init_map_view(int col, int row, int level): set the scroll position and the map
// pointers for the cell (col, row - 4) of level, then draw the 8 strips around it.  The
// row argument is left decremented on the stack, as the original does.
uint16_t init_map_view(const Args &a)
{
	cyc(207 + 1113);                                   // F162 prologue, 4004-40B2
	uint16_t saved[6];
	for (int i = 1; i <= 5; i++) saved[i] = T.r[i];
	const uint16_t col = a.u(0);
	pokeb(0x0F59, a.b(2));
	poke((uint16_t)(a.sp + 4), (uint16_t)(a.u(1) - 4));
	const uint16_t row = a.u(1);
	pokeb(0x0F5B, (uint8_t)row);
	const uint16_t x = (uint16_t)(((col & 7) << 13) + 0x2000);
	poke(0x01A8, x);
	poke(0x01AC, x);
	const uint16_t y = (uint16_t)(((row & 7) << 13) + 0x1000);
	poke(0x01AA, y);
	poke(0x01AE, y);
	poke(0x0F3C, peek(0x01A8) & 0xE000);
	poke(0x0F3E, y & 0xF000);
	poke(0x01B0, 0);
	poke(0x01B2, 0);
	const uint16_t level = a.b(2);
	poke(0x01CC, rb((uint16_t)(0xFEC2 + level)));
	poke(0x01CE, rb((uint16_t)(0xFECA + level)));
	poke(0x01D6, peek((uint16_t)(0xFED2 + (peek(0x01CC) << 1))));
	poke(0x01D8, peek((uint16_t)(0xFED2 + (peek(0x01CE) << 1))));
	poke(0x0F46, peek((uint16_t)(0x7F8C + (level << 1))));
	T.r[6] = (uint16_t)(T.r[6] - 2);                   // TST -(SP): the strip counter
	const uint16_t counter = T.r[6];
	if (level > 0) cyc(30);
	if (level > 0 && level < 5)
	{
		cyc(30);
		uint16_t r0 = call(0x90BA, {});
		cyc(135);
		const uint16_t r1 = (uint16_t)(peekb((uint16_t)(0xFE8F + r0)) + peek(0x0F46));
		pokeb(0x0F5A, peekb((uint16_t)(r1 + 0x000C)));
		r0 = call(0x90BA, {});
		cyc(126);
		r0 = (uint16_t)((peekb((uint16_t)(0xFE8F + r0)) << 1) + peek(0x0F46));
		poke(0x0F46, peek(r0));
	}
	else
	{
		cyc(42);
		pokeb(0x0F5A, peekb((uint16_t)(0x7F9C + level)));
	}
	cyc(111);
	push(row);
	push(peekb(0x0F5A));
	const uint16_t product = call(0x80D8, {});         // mul16
	cyc(840);
	poke(0x0F46, (uint16_t)(product + peek(0x0F46) + (col << 1)));
	poke(0x0F40, (uint16_t)(((row & 0x0F) << 12) - 0x0800));
	pokeb(0x0F44, (uint8_t)(col << 5));
	poke(0x099A, asr(row, 1));
	poke(0x099C, (uint16_t)(peek(0x099A) + 3));
	poke(0x0996, (uint16_t)(asr((uint16_t)(col - 1), 1) << 4));
	poke(0x0998, (uint16_t)(peek(0x0996) + 0x0040));
	poke(0x0F4E, peek((uint16_t)(0xFF54 + (peekb(0x0F59) << 1))));
	if (peekb(0x0F59))
	{
		cyc(33);
		poke(0x099E, 0x0FFF);
	}
	else
	{
		cyc(45);
		poke(0x099E, 0x000C);
	}
	cyc(234);
	pokeb(0x0E9B, 0);
	poke(0x09A0, peek(0x01A8) & 0xC000);
	poke(0x09A2, peek(0x01AA) & 0xC000);
	pokeb(0x0F58, 0);
	poke(counter, 0x0008);
	T.r[0] = (uint16_t)(peekb(0x0F59) << 1);
	T.r[1] = (uint16_t)(col << 1);
	T.r[2] = row;
	T.r[3] = col;
	do
	{
		cyc(57);
		poke(T.r[6], 0xFFFF);
		call(0x42BC, {});                              // step_map_row(-1)
		cyc(30);
		call(0x4894, {});                              // town_alt_cell_bit
		cyc(30);
		call(0x41CE, {});                              // draw_map_strip
		cyc(42);
		poke(counter, (uint16_t)(peek(counter) - 1));
	} while (peek(counter));
	cyc(39);
	T.r[6] = (uint16_t)(T.r[6] + 6);
	const uint16_t r0 = T.r[0];
	for (int i = 1; i <= 5; i++) T.r[i] = saved[i];
	return r0;
}

// A5:48CC (assembly): the map slots from 0x09B0 down to 0x0E98: a slot whose cell (0x0AF8:
// column in bits 0-3, row in bits 4-8) is inside the view (columns 0x099A-0x099C, rows
// 0x0996-0x0998) is activated (F:C4E4) unless it is active (bit 15), one outside is
// deactivated (F:C3E6).  The flag word on the stack is passed to the callees.
void update_map_slots()
{
	cyc(207);
	T.r[6] = (uint16_t)(T.r[6] - 2);
	const uint16_t local = T.r[6];
	pokeb(0x0E99, 0);
	poke(0x09AA, peek(0x09B0));
	T.r[4] = peek(0x099A);
	T.r[2] = peek(0x099C);
	T.r[3] = peek(0x0996);
	T.r[1] = peek(0x0998);
	if ((int16_t)T.r[4] < 0)
	{
		cyc(30);
		if ((int16_t)T.r[3] >= 0x00C0)
		{
			cyc(72);
			T.r[4] = (uint16_t)(T.r[4] + 0x000C);
			T.r[2] = (uint16_t)(T.r[2] + 0x000C);
			T.r[3] = (uint16_t)(T.r[3] - 0x0050);
			T.r[1] = (uint16_t)(T.r[1] - 0x0050);
		}
	}
	// one pass of the loop at 0x4902 costs 111 (test of the slot number) + 39 (in use?)
	// + 39 (next slot) cycles, an occupied slot 114 + 30 + 24 more plus the range tests
	// and the JSR to the callee
	for (;;)
	{
		T.r[0] = peek(0x09AA);
		const uint16_t last = peekb(0x0E98);
		if ((int16_t)T.r[0] < (int16_t)last) break;
		int pass = 111 + 39 + 39;
		if (peekb((uint16_t)(0x09B8 + T.r[0])))
		{
			pass += 114 + 30 + 24;
			poke(local, 0);
			T.r[0] = peek((uint16_t)(0x0AF8 + (T.r[0] << 1)));
			T.r[5] = T.r[0] & 0x000F;
			if ((int16_t)T.r[5] >= (int16_t)T.r[4])
			{
				pass += 24;
				if ((int16_t)T.r[5] <= (int16_t)T.r[2])
				{
					pass += 54;
					T.r[5] = T.r[0] & 0x01F0;
					if ((int16_t)T.r[5] >= (int16_t)T.r[3])
					{
						pass += 24;
						if ((int16_t)T.r[5] < (int16_t)T.r[1])
						{
							pass += 21;
							poke(local, (uint16_t)(peek(local) + 1));
						}
					}
				}
			}
			if (!peek(local))
			{
				if ((int16_t)T.r[0] < 0)
				{
					pass += 30 + 12;
					call(0xC3E6, {});
				}
			}
			else if ((int16_t)T.r[0] >= 0)
			{
				pass += 30;
				call(0xC4E4, {});
			}
		}
		poke(0x09AA, (uint16_t)(peek(0x09AA) - 1));
		tick(0x4902, pass);
	}
	cyc(111 + 18 + 21);                                // last test, TST (SP)+, RTS
	T.r[6] = (uint16_t)(T.r[6] + 2);
	fl::tst(peek(local));
}

// ---------------------------------------------------------------- sounds

// A5:5D08  stop_effect_sound(int n): if sound effect n (table 0x6567, bank B 0x0400) plays on
// an effect channel (0x01A6 / 0x01A7), free the channel and queue its stop command
uint16_t stop_effect_sound(const Args &a)
{
	cyc(180);
	set_bank_b(0x0400);
	const uint16_t s = peekb((uint16_t)(0x6567 + a.u(0)));
	if (!s)
	{
		cyc(39);
		return 0;
	}
	uint16_t cmd;
	cyc(60);
	if (peekb(0x01A6) == s)
	{
		cyc(69);
		pokeb(0x01A6, 0);
		cmd = 0x0070;
	}
	else
	{
		cyc(60);
		if (peekb(0x01A7) != s)
		{
			cyc(39);
			return s;
		}
		cyc(57);
		pokeb(0x01A7, 0);
		cmd = 0x0076;
	}
	cyc(30);
	T.r[0] = s;
	const uint16_t r0 = call(0x8E78, {cmd});
	cyc(18 + 39);
	return r0;
}

// A5:5B46: rolling sound: when the skater's speed differs from the sound's level 0x01DC by
// at least 50, the level steps by 27 towards it with the matching sound command (0x46/0x48
// up, 0x49 down, 0x53 to silence below speed 128).  Not while 0x031F or 0x0330 is set.
uint16_t rolling_sound(const Args &)
{
	cyc(159 + 36);
	if (peekb(0x031F))
	{
		cyc(21);
		return T.r[0];
	}
	cyc(36);
	if (peekb(0x0330))
	{
		cyc(21);
		return T.r[0];
	}
	cyc(60);
	const uint16_t speed = peek(0x01B4);
	const bool forward = (int16_t)speed >= 0;
	cyc(forward ? 36 : 24);
	const uint16_t mag = forward ? speed : (uint16_t)-speed;
	uint16_t r0 = (uint16_t)(mag - peek(0x01DC));
	cyc(36 + 24 + 30);
	const uint16_t diff = (r0 & 0x8000) ? (uint16_t)-r0 : r0;
	if ((int16_t)diff < 0x0032)
	{
		cyc(12);
		r0 = 0;
	}
	cyc(24);
	if (!r0)
	{
		cyc(21);
		return 0;
	}
	cyc(12);
	uint16_t cmd;
	if ((int16_t)r0 > 0)
	{
		cyc(36);
		if ((int16_t)peek(0x01DC) > 0)
		{
			cyc(30);
			cmd = 0x0048;
		}
		else
		{
			cyc(42);
			cmd = 0x0046;
		}
		cyc(30);
		T.r[0] = r0;
		const uint16_t res = call(0x8E78, {cmd});
		cyc(45 + 18 + 21);
		poke(0x01DC, (uint16_t)(peek(0x01DC) + 0x001B));
		return res;
	}
	cyc(57);
	poke(0x01DC, (uint16_t)(peek(0x01DC) - 0x001B));
	if (!forward) cyc(12);
	cyc(42);
	cmd = 0x0049;
	if ((int16_t)mag < 0x0080)
	{
		cyc(36);
		if (peek(0x01DC))
		{
			cyc(69);
			poke(0x01DC, 0);
			cmd = 0x0053;
		}
		else
			cyc(30);
	}
	else
		cyc(30);
	cyc(30);
	T.r[0] = mag;
	const uint16_t res = call(0x8E78, {cmd});
	cyc(18 + 21);
	return res;
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
	cyc(102);
	T.r[2] = 0x00E0;
	T.psw = (uint8_t)((T.psw & 0x10) | 0xE0);         // MTPS: interrupts off
	T.check_irqs = true;
	T.r[6] = (uint16_t)(T.r[6] - 2);
	const uint16_t arg = T.r[6];
	poke(arg, 0x000D);
	call(0x8EA2, {});                                  // send_sound_command(0x0D)
	cyc(30);
	call(0x8ED2, {});                                  // read_sound_response
	cyc(42);
	T.r[1] = T.r[0];
	call(0x8F62, {});
	cyc(66);
	T.r[2] = 0;
	T.psw = (uint8_t)(T.psw & 0x10);                   // MTPS #0
	T.check_irqs = true;
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
	cyc(183 + 30);
	call(0x8068, {});                                  // select_video_bank0
	cyc(138);
	push(a.u(1));
	push(0x0180);
	uint16_t r0 = (uint16_t)(call(0x80D8, {}) + peek((uint16_t)(0x4980 + (a.u(2) << 1))));   // mul16
	cyc(126);
	poke(0x0186, r0);
	poke(0x0186, (uint16_t)(peek(0x0186) + 0x1E80));
	if (a.u(0))
	{
		cyc(36);
		poke((uint16_t)(a.sp + 2), 0x0009);
	}
	cyc(84);
	const uint16_t first = a.u(0), last = (uint16_t)(first + 6);
	for (uint16_t i = first;; i = (uint16_t)(i + 3))
	{
		cyc(24);
		if (!((int16_t)i <= (int16_t)last)) break;
		cyc(381);
		r0 = peek(0x0186);
		for (int j = 0; j < 3; j++)
			poke((uint16_t)(r0 + 2 * j), peek((uint16_t)(0x495C + (i << 1) + 2 * j)) | peek(0x0188));
		poke(0x0186, (uint16_t)(peek(0x0186) + 0x0080));
	}
	cyc(45);
	T.r[6] = (uint16_t)(T.r[6] + 4);
	return r0;
}

// A5:51EE  slope_height(int t): 0x02E6 minus the height table entry 0x6AC8 (bank B 0x2C00)
// for the skater direction 0x031D and the terrain t: low bits 0 use the direction alone, 7
// the row (bits 3-5) of a 16-entry block at 0x220, others the row table at 0x79E8.  Bank B
// ends as 0x0C00.
uint16_t slope_height(const Args &a)
{
	cyc(135 + 153);
	set_bank_b(0x2C00);
	uint16_t r0 = sext8(peekb(0x031D));
	const uint16_t t = a.u(0);
	if (t & 7)
	{
		cyc(42);
		if ((t & 7) == 7)
		{
			cyc(84);
			r0 = (uint16_t)(((t & 0x0038) << 1) + r0 + 0x0220);
		}
		else
		{
			cyc(189);
			r0 = (uint16_t)(((t & 7) << 4) + peek((uint16_t)(0x79E8 + (asr(t, 3) << 1))) + r0);
		}
	}
	cyc(168);
	r0 = (uint16_t)(peek(0x02E6) - peek((uint16_t)(0x6AC8 + (r0 << 1))));
	set_bank_b(0x0C00);
	return r0;
}

// A7:4098 (assembly): move objects 0-3 by their velocities (x, y, height).  The fraction
// bytes (0x07FC/0x0800/0x0804 += 0x07F0/0x07F4/0x07F8) are added, but their carry is lost
// (SWAB clears it before the ADC), as in the original.
void move_objects_0_3()
{
	static const uint16_t parts[3][4] = {
		{0x07F0, 0x07FC, 0x06F0, 0x04F0},
		{0x07F4, 0x0800, 0x0730, 0x0570},
		{0x07F8, 0x0804, 0x06B0, 0x05B0},
	};
	for (uint16_t i = 3;; i--)
	{
		const uint16_t w = (uint16_t)(i << 1);
		for (const auto &p : parts)
		{
			T.r[0] = (uint16_t)(peekb((uint16_t)(p[0] + i)) << 8);
			T.r[1] = (uint16_t)(peekb((uint16_t)(p[1] + i)) << 8);
			T.r[0] = (uint16_t)(T.r[0] + T.r[1]);
			T.r[0] = (uint16_t)((T.r[0] >> 8) | (T.r[0] << 8));
			pokeb((uint16_t)(p[1] + i), (uint8_t)T.r[0]);
			const uint16_t a = (uint16_t)(p[2] + w);
			poke(a, fl::add(peek(a), peek((uint16_t)(p[3] + w))));
		}
		T.r[2] = fl::dec(i);
		T.r[3] = w;
		if (i == 0) break;
	}
}

// A7:410C: the skater's speed 0x01B4 = the velocity projected on the direction vector
// (0x02E2, 0x02E4, 0x02E6): (3 * (2 vy*dy + vx*dx) / 4 + vz1*dz) >> 12, with 32-bit products
// from F:80FA mul32 (long at 0x0204).  Returns the R0 of the last mul32.
uint32_t long_0204() { return (uint32_t)peek(0x0206) << 16 | peek(0x0204); }
uint16_t project_speed(const Args &)
{
	call(0x80FA, {peek(0x0570), peek(0x02E4)});
	uint32_t s = long_0204() << 1;
	call(0x80FA, {peek(0x04F0), peek(0x02E2)});
	s += long_0204();
	poke(0x0204, (uint16_t)s);
	poke(0x0206, (uint16_t)(s >> 16));
	s = (uint32_t)((int32_t)(s * 3) >> 2);
	const uint16_t r0 = call(0x80FA, {peek(0x05B2), peek(0x02E6)});
	const uint32_t t = s + long_0204();
	poke(0x0206, (uint16_t)(t >> 16));
	poke(0x01B4, (uint16_t)((int32_t)t >> 12));
	return r0;
}

// A7:4F98: direction (0-15) from object 7 to the skater (object 0, its height added to y),
// with the frame counter jittering the x difference
uint16_t direction_from_object7(const Args &)
{
	cyc(159 + 258 + 24 + 30);
	const uint16_t dx = (uint16_t)(peek(0x06F0) - peek(0x06FE) + 0x0500 + (peek(0x01C4) & 0x01FF));
	const uint16_t dy = (uint16_t)(peek(0x0730) - peek(0x073E) + peek(0x06B0) + 0x0500);
	const uint16_t ax = (int16_t)dx < 0 ? (uint16_t)-dx : dx;
	uint16_t r;
	if ((int16_t)ax < 0x0200)
	{
		cyc(42 + 12);
		r = 0x0004;
		if ((int16_t)dy < 0)
		{
			cyc(30);
			r = 0x000C;
		}
	}
	else
	{
		cyc(24 + 24 + 30);
		const uint16_t ay = (int16_t)dy < 0 ? (uint16_t)-dy : dy;
		if ((int16_t)ay < 0x0200)
		{
			cyc(24 + 12);
			r = 0x0000;
			if ((int16_t)dx < 0)
			{
				cyc(30);
				r = 0x0008;
			}
		}
		else
		{
			cyc(42);
			r = 0x0002;
			if ((int16_t)dx < 0)
			{
				cyc(18);
				r = 0x0006;
			}
			cyc(24);
			if ((int16_t)dy < 0)
			{
				cyc(30);
				r = (uint16_t)(0x0010 - r);
			}
		}
	}
	cyc(33);
	return r;
}

// A7:4ECC  hold_object(int i): object i is parked: its animation, byte 0x0770 and velocities
// go to the template 0x0E5E-0x0E69 (respawned later by A7:4F42), and it stops (0x04B0 bit
// 0x40).  Object 6 instead gets the animation 0x50FA in the template slot of object 5.
uint16_t hold_object(const Args &a)
{
	cyc(81);
	const uint16_t i = a.u(0), w = (uint16_t)(i << 1);
	if (i == 6)
	{
		cyc(159);
		poke(0x03BA, 0x50FA);
		pokeb(0x0495, peekb(0x0495) | 0x18);
		poke(0x04FA, 0);
		poke(0x057A, 0);
		poke(0x043A, 0);
	}
	else
	{
		cyc(336);
		pokeb(0x0E6A, (uint8_t)i);
		pokeb(0x0E68, 0xB4);
		poke(0x0E66, peek((uint16_t)(0x0430 + w)));
		pokeb(0x0E69, peekb((uint16_t)(0x0770 + i)));
		poke(0x0E5E, peek((uint16_t)(0x04F0 + w)));
		poke(0x0E60, peek((uint16_t)(0x0570 + w)));
		poke(0x0E62, peek((uint16_t)(0x03B0 + w)));
		poke(0x0E64, peek((uint16_t)(0x03F0 + w)));
	}
	cyc(219);
	poke((uint16_t)(0x0430 + w), 0);
	pokeb((uint16_t)(0x04B0 + i), peekb((uint16_t)(0x04B0 + i)) | 0x40);
	poke((uint16_t)(0x03F0 + w), 0);
	poke((uint16_t)(0x04F0 + w), 0);
	poke((uint16_t)(0x0570 + w), 0);
	return w;
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
uint16_t message_box(const Args &a)
{
	cyc(207 + 204);
	const uint16_t saved_a = peek(0x01D2), saved_b = peek(0x01D4);
	set_bank_b(0x2800);
	call(0x8068, {});                                  // select_video_bank0
	cyc(252);
	poke(0x0188, 0xE000);
	const uint16_t box = a.u(1);
	uint16_t p = (uint16_t)((peek(box) + 0x2000) & ~1);
	const uint16_t rows = peekb((uint16_t)(box + 2));
	const uint16_t cols = peekb((uint16_t)(box + 3));
	const uint16_t back = (uint16_t)(cols << 1);
	uint16_t r0;
	if (a.u(0))
	{
		cyc(54);
		for (uint16_t r = rows; r > 0; r--)
		{
			for (uint16_t c = cols; c > 0; c--)
			{
				poke(p, 0);
				p = (uint16_t)(p + 2);
			}
			p = (uint16_t)(p - back + 0x0080);
			tick(0x4D84, 120 + 39 * cols);             // one row of the loop at 0x4D84
		}
		cyc(42);
		if (peekb(0x036C) == 1)
		{
			cyc(36);
			if (!peekb(0x0F59))
			{
				cyc(117);
				poke(0x0188, 0xA000);
				call(0xEAD4, {0x0000, 0x5DF6});
				cyc(216);
				poke(0x0186, 0x3746);
				call(0xBA96, {0x0001, 0x0000, 0x0008, peek(0x0246), peek(0x0244), 0x5DF6});
				cyc(18);
			}
		}
		cyc(87);
		r0 = (uint16_t)(peekb(0x036C) >> 1);
		pokeb(0x036C, (uint8_t)r0);
	}
	else
	{
		cyc(84);
		const uint16_t flags = (uint16_t)(peekb(0x036C) << 1);
		pokeb(0x036C, (uint8_t)flags);
		if (peek(box) & 1)
		{
			cyc(150);
			pokeb(0x036C, (uint8_t)(peekb(0x036C) | 1));
			call(0xEAD4, {0x0001, 0x5DF6});
			cyc(36);
		}
		else
			cyc(27);
		r0 = peek(0x0188);
		const int16_t inner = (int16_t)(cols - 2);
		auto put = [&](uint16_t v) { poke(p, v); p = (uint16_t)(p + 2); };
		auto line = [&](uint16_t left, uint16_t mid, uint16_t right) {
			put(left);
			for (int16_t n = inner; n > 0; n--) put(mid);
			put(right);
		};
		cyc(156 + (inner > 0 ? 39 * inner : 0) + 360);
		line(r0 | 0x0110, r0 | 0x0111, r0 | 0x0112);
		p = (uint16_t)(p - back + 0x0080);
		for (int16_t n = (int16_t)(rows - 2); n > 0; n--)
		{
			line(r0 | 0x0113, r0 | 0x0087, r0 | 0x0114);
			p = (uint16_t)(p - back + 0x0080);
			tick(0x4E6A, 198 + (inner > 0 ? 54 * inner : 0));   // one row of the loop at 0x4E6A
		}
		cyc(126 + (inner > 0 ? 39 * inner : 0) + 48);
		line(r0 | 0x0115, r0 | 0x0116, r0 | 0x0117);
	}
	cyc(201);
	poke(0x01D2, saved_a);
	poke(0x01D4, saved_b);
	poke(0x1400, peek(0x01D2));
	poke(0x1402, peek(0x01D4));
	return r0;
}

// A7:4B84 (assembly): count the message timer 0x036A and erase the messages whose time has
// come (shifting the later ones down).  As in the original, the entry after a removed one
// is skipped in this pass and the loop runs to the old count.  Banks are restored; the flags
// are those of comparing the two saved bank words.
void expire_messages()
{
	cyc(66);
	T.r[6] = (uint16_t)(T.r[6] - 4);
	const uint16_t sp = T.r[6];
	T.r[5] = sext8(peekb(0x0336));
	if (!((int16_t)T.r[5] < 0))
	{
		cyc(126);
		poke((uint16_t)(sp + 2), peek(0x01D2));
		poke(sp, peek(0x01D4));
		poke(0x036A, (uint16_t)(peek(0x036A) + 1));
		for (T.r[4] = 0;; T.r[4]++)
		{
			cyc(24);                                   // 4C0A: CMP R4,R5
			if (!((int16_t)T.r[4] <= (int16_t)T.r[5])) break;
			cyc(75);
			T.r[0] = (uint16_t)(T.r[4] << 1);
			if (peek((uint16_t)(0x0338 + T.r[0])) != peek(0x036A))
			{
				cyc(12);                               // INC R4
				continue;
			}
			cyc(123);
			T.r[1] = 0x2800;
			set_bank_b(0x2800);
			const uint16_t box = peek((uint16_t)(0x0348 + T.r[0]));
			if ((int16_t)peek(box) >= 0x4000)
			{
				cyc(105);
				call(0xEAD4, {0x0001, peek(box)});
				cyc(12);
			}
			else
			{
				cyc(99);
				call(0x4D30, {0x0001, box});           // message_box(erase)
			}
			cyc(87);
			T.r[0] = (uint16_t)(sext8(peekb(0x0336)) - 1);
			pokeb(0x0336, (uint8_t)T.r[0]);
			T.r[3] = sext8((uint8_t)T.r[0]);
			if ((int16_t)T.r[3] >= 0)
			{
				cyc(36);
				for (T.r[2] = T.r[4];; T.r[2]++)
				{
					cyc(36);                           // 4C00: INC R2, CMP, BLE
					if (!((int16_t)T.r[2] <= (int16_t)T.r[3])) break;
					cyc(138);
					T.r[1] = (uint16_t)(T.r[2] << 1);
					T.r[0] = T.r[1];
					poke((uint16_t)(0x0348 + T.r[1]), peek((uint16_t)(0x034A + T.r[0])));
					poke((uint16_t)(0x0338 + T.r[1]), peek((uint16_t)(0x033A + T.r[0])));
				}
			}
			cyc(24 + 12);                              // CMP (SP)+,(SP)+; INC R4
		}
		cyc(153);
		poke(0x01D2, peek((uint16_t)(sp + 2)));
		poke(0x01D4, peek(sp));
		poke(0x1400, peek(0x01D2));
		poke(0x1402, peek(0x01D4));
	}
	cyc(45);
	T.r[6] = (uint16_t)(T.r[6] + 4);
	fl::cmp(peek(sp), peek((uint16_t)(sp + 2)));
}

// A7:4C28 (assembly): show the next pending message (0x0362[0x0358]) in a new box: when all
// 8 are in use the oldest is removed first.  Text by F:A9FC; bits 0 of 0x036D / 0x036E ask
// for F:A8FE and the text table 0x5CD2 for this message.  The flags are those of the
// argument word left on the stack.
void show_next_message()
{
	cyc(75);
	T.r[0] = (uint16_t)(sext8(peekb(0x0358)) << 1);
	const uint16_t msg = peek((uint16_t)(0x0362 + T.r[0]));
	fl::tst(msg);
	if (!msg)
	{
		cyc(21);
		return;
	}
	cyc(42);
	if (peekb(0x0336) == 7)
	{
		cyc(171);
		T.r[3] = peek(0x01D2);
		T.r[4] = peek(0x01D4);
		T.r[0] = 0x2800;
		set_bank_b(0x2800);
		const uint16_t box = peek(0x0348);
		if ((int16_t)peek(box) >= 0x4000)
		{
			cyc(105);
			call(0xEAD4, {0x0001, peek(box)});
			cyc(12);
		}
		else
		{
			cyc(96);
			call(0x4D30, {0x0001, box});               // message_box(erase)
		}
		cyc(12);
		for (T.r[2] = 0; (int16_t)T.r[2] <= 6;)
		{
			cyc(180);
			T.r[1] = (uint16_t)(T.r[2] << 1);
			T.r[0] = T.r[1];
			poke((uint16_t)(0x0348 + T.r[1]), peek((uint16_t)(0x034A + T.r[0])));
			poke((uint16_t)(0x0338 + T.r[1]), peek((uint16_t)(0x033A + T.r[0])));
			T.r[2]++;
		}
		cyc(189);
		pokeb(0x0336, 6);
		poke(0x01D2, T.r[3]);
		poke(0x01D4, T.r[4]);
		poke(0x1400, peek(0x01D2));
		poke(0x1402, peek(0x01D4));
	}
	cyc(42);
	if (peekb(0x036D) & 1)
	{
		cyc(30);
		call(0xA8FE, {});
	}
	cyc(117);
	T.r[0] = (uint16_t)(peekb(0x036D) >> 1);
	pokeb(0x036D, (uint8_t)T.r[0]);
	if (peek(0x036E) & 1)
	{
		cyc(33);
		poke(0x0184, 0x5CD2);
	}
	cyc(180);
	T.r[0] = (uint16_t)(peekb(0x036E) >> 1);
	pokeb(0x036E, (uint8_t)T.r[0]);
	T.r[0] = (uint16_t)(sext8(peekb(0x0358)) << 1);
	push(peek((uint16_t)(0x0362 + T.r[0])));
	const uint16_t arg = T.r[6];
	call(0xA9FC, {});
	cyc(399);
	poke(0x0184, 0x5AC8);
	T.r[0] = (uint16_t)(sext8(peekb(0x0336)) + 1);
	pokeb(0x0336, (uint8_t)T.r[0]);
	T.r[0] = (uint16_t)(sext8((uint8_t)T.r[0]) << 1);
	T.r[1] = (uint16_t)(sext8(peekb(0x0358)) << 1);
	poke((uint16_t)(0x0348 + T.r[0]), peek((uint16_t)(0x0362 + T.r[1])));
	T.r[2] = (uint16_t)(peek((uint16_t)(0x035A + T.r[1])) + peek(0x036A));
	poke((uint16_t)(0x0338 + T.r[0]), T.r[2]);
	poke((uint16_t)(0x0362 + T.r[1]), 0);
	T.r[0] = (uint16_t)(sext8(peekb(0x0358)) - 1);
	pokeb(0x0358, (uint8_t)T.r[0]);
	T.r[0] = sext8((uint8_t)T.r[0]);
	if ((int16_t)T.r[0] < 0)
	{
		cyc(21);
		poke(T.r[0], 0);
	}
	cyc(18 + 21);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	fl::tst(peek(arg));
}

// A7:5A7E  credit_messages(int mode, int show_price): the credit/coin texts (F:8B04, colour
// 0xE000).  Mode 0: texts 0x53 (and 0x5A beyond 4 credits) with the credit count 0x01F9 / 4;
// otherwise the coin mode text from the switches 0x01F3 / 0x01FA.  Then text 0x4F-0x52 by the
// credits against the price table 0x5A46.
uint16_t text(uint16_t n)
{
	T.r[0] = n;
	return call(0x8B04, {});
}
uint16_t credit_messages(const Args &a)
{
	cyc(159 + 72);
	poke(0x0188, 0xE000);
	if (!a.u(0))
	{
		cyc(48);
		text(0x0053);
		cyc(42);
		if (peekb(0x01F9) > 4)
		{
			cyc(48);
			text(0x005A);
		}
		cyc(255);
		poke(0x0186, (uint16_t)(peek(0x0186) - 0x0018));
		call(0xBA96, {0x0000, 0x0001, 0x0002, 0x0000, (uint16_t)(peekb(0x01F9) >> 2)});
		cyc(42);
		if (peekb(0x01F9) & 3)
		{
			cyc(417);
			uint16_t r1 = rb((uint16_t)(0x5A49 + (sext8(peekb(0x01FA)) & 3)));
			r1 = (uint16_t)(((sext8(peekb(0x01F9)) & 3) << 1) + r1 - 2);
			const uint16_t at = peek(0x0186);
			poke(at, peek((uint16_t)(0x5A4E + (r1 << 1))) | peek(0x0188));
			poke((uint16_t)(peek(0x0186) + 2), peek((uint16_t)(0x5A50 + (r1 << 1))) | peek(0x0188));
		}
		cyc(30);
	}
	else
	{
		cyc(84);
		if ((peekb(0x01FA) & 0xE0) == 0xE0)
		{
			cyc(18 + 30);
			text(0x0059);
		}
		else
		{
			cyc(198);
			const uint16_t m = (uint16_t)((peekb(0x01F3) >> 6) & 3);
			text((uint16_t)(m + 0x0054));
			cyc(30);
			if (m != 1)
			{
				cyc(30 + 30);
				text(0x0058);
			}
		}
	}
	cyc(75);
	uint16_t n = peekb(0x01F9);
	if (!a.u(1))
	{
		cyc(30);
		n = 3;
	}
	else
	{
		cyc(201);
		const uint16_t price = rb((uint16_t)(0x5A46 + ((peekb(0x01F3) >> 6) & 3)));
		if ((int16_t)n >= (int16_t)(price << 1))
		{
			cyc(24);
			n = 0;
		}
		else
		{
			cyc(63);
			if ((int16_t)n >= (int16_t)price)
			{
				cyc(30);
				n = 1;
			}
			else
			{
				cyc(18);
				n = 2;
			}
		}
	}
	cyc(60);
	const uint16_t r0 = text((uint16_t)(n + 0x004F));
	cyc(21);
	return r0;
}

// ---------------------------------------------------------------- game state

// A7:5018: the chaser (object 7).  Its timer 0x0EB4 counts down; while it is on screen it
// keeps the timer negative and is steered with the skater's velocity.  When the timer runs
// out it gets faster (0x0EB2, up to 7; 0x0F22 the delay from 0xFE0D) and F:E13A moves it
// towards the skater; the timer restarts at 5 (on screen) or 16-31 (random).
uint16_t chaser_step(const Args &)
{
	cyc(135 + 138);
	poke(0x043E, 0x0001);
	poke(0x0EB4, (uint16_t)(peek(0x0EB4) - 1));
	uint16_t r1 = 0, r2 = 0;
	const uint16_t sx = peek(0x063E), sy = peek(0x067E);
	bool visible = false;
	if (sx > 0x8200)
	{
		cyc(42);
		visible = sx < 0xF000;
	}
	if (!visible)
	{
		cyc(42);
		if ((int16_t)sy < 0x1000)
		{
			cyc(42);
			visible = sy > 0x9000;
		}
	}
	if (visible)
	{
		cyc(105);
		r1 = peek(0x04F0);
		r2 = (uint16_t)(peek(0x0570) + peek(0x05B0));
		poke(0x0EB4, 0xFFFF);
	}
	cyc(36);
	if ((int16_t)peek(0x0EB4) > 0)
	{
		cyc(21);
		return 0;
	}
	cyc(36);
	if ((int16_t)peek(0x0F22) <= 0)
	{
		cyc(42);
		if ((int8_t)peekb(0x0EB2) <= 6)
		{
			cyc(63);
			pokeb(0x0EB2, (uint8_t)(peekb(0x0EB2) + 1));
		}
		cyc(105);
		pokeb(0x0F22, rb((uint16_t)(0xFE0D + peekb(0x0F2E))));
		pokeb(0x0F23, 0);
	}
	cyc(30);
	const uint16_t dir = call(0x4F98, {});             // direction_from_object7
	cyc(150);
	uint16_t r0 = call(0xE13A, {r2, r1, sext8(peekb(0x0EB2)), dir});
	cyc(36);
	if ((int16_t)peek(0x0EB4) < 0)
	{
		cyc(45);
		poke(0x0EB4, 0x0005);
	}
	else
	{
		cyc(30);
		r0 = call(0x9152, {});                         // next_random
		cyc(93);
		poke(0x0EB4, (uint16_t)((r0 & 0x000F) + 0x0010));
	}
	cyc(18 + 21);
	return r0;
}

// A7:5248: buying (bit 0x10 of 0x0ED7): at the shop A7:5326, if the cash 0x0256 covers the
// price (tables 0xFE7C/0xFE70 by 0x0F31) the item count 0x025C+shop goes up, the price is
// paid (F:F002) and a sound plays; otherwise the "not enough" message.  The request is cleared.
uint16_t shop_purchase(const Args &)
{
	cyc(135 + 42);
	if (!(peekb(0x0ED7) & 0x10))
	{
		cyc(21);
		return T.r[0];
	}
	cyc(30);
	const uint16_t shop = call(0x5326, {});
	cyc(210);
	pokeb(0x0ED8, (uint8_t)(peekb(0x0ED8) | rb((uint16_t)(0xFC74 + shop))));
	const uint16_t price = rb((uint16_t)(0xFE70 + rb((uint16_t)(0xFE7C + peekb(0x0F31)))));
	if ((int16_t)peek(0x0256) >= (int16_t)price)
	{
		cyc(60);
		const uint16_t item = (uint16_t)(0x025C + shop);
		if ((int8_t)peekb(item) < 0)
		{
			cyc(21);
			pokeb(item, 0);
		}
		cyc(141);
		pokeb(item, (uint8_t)(peekb(item) + 1));
		push(0x76C8);
		push(0x00D2);
		push(0x0001);
		call(0xA974, {});
		cyc(63);
		poke(T.r[6], (uint16_t)-price);
		call(0xF002, {});
		cyc(75);
		poke(T.r[6], shop);
		push(0x0000);
		call(0xD97C, {});
		cyc(30);
		T.r[0] = call(0x9152, {}) & 0x0007;
		cyc(105);
		poke(T.r[6], rb((uint16_t)(0xFE1E + T.r[0])));
		call(0x8E78, {});
		cyc(30);
		call(0x5602, {});
		cyc(30);
		call(0xD8D2, {});
		cyc(12);
	}
	else
	{
		cyc(120);
		push(0x76D6);
		push(0x00F0);
		push(0x0001);
		call(0xA974, {});
		cyc(81);
		poke(T.r[6], shop);
		push(0x0001);
		call(0xD97C, {});
		cyc(30);
		T.r[0] = call(0x9152, {}) & 0x0003;
		cyc(105);
		poke(T.r[6], rb((uint16_t)(0xFE26 + T.r[0])));
		call(0x8E78, {});
	}
	cyc(51 + 21);
	pokeb(0x0ED7, peekb(0x0ED7) & ~0x10);
	T.r[6] = (uint16_t)(T.r[6] + 8);
	return T.r[0];
}

// A7:43C0  insert_high_score(int first, long score): find the first of the 15 entries from
// first on (8 bytes at 0x0370: high word, low word, ...) below score and insert score there,
// moving the lower entries down.  Returns the place or -1.
uint16_t insert_high_score(const Args &a)
{
	cyc(207 + 186 + 66);                               // prologue, setup, first test of the loop
	const uint16_t first = a.u(0), lo = a.u(1), hi = a.u(2);
	const uint32_t score = (uint32_t)hi << 16 | lo;
	uint16_t place = 0xFFFF;
	for (uint16_t i = first; (int16_t)i <= (int16_t)(uint16_t)(first + 14); i = (uint16_t)(i + 1))
	{
		const uint16_t e = (uint16_t)(0x0370 + (i << 3));
		const uint32_t v = (uint32_t)peek(e) << 16 | peek((uint16_t)(e + 2));
		if ((int16_t)((v - score) >> 16) < 0)
		{
			cyc(288 + 84 + 39 + 30);                   // the pass that finds it, F:80D0 sub32 included
			place = i;
			break;
		}
		tick(0x4410, 477);                             // one pass, F:80D0 sub32 included
	}
	if (place == 0xFFFF) cyc(12);                      // TST R5 after the loop
	cyc(12);                                           // BLT
	if ((int16_t)place < 0)
	{
		cyc(51);
		return place;
	}
	cyc(81 + 36);
	for (uint16_t j = (uint16_t)(first + 14); (int16_t)j >= (int16_t)(uint16_t)(place + 1); j = (uint16_t)(j - 1))
	{
		for (int k = 0; k < 8; k += 2)
			poke((uint16_t)(0x0370 + (j << 3) + k), peek((uint16_t)(0x0368 + (j << 3) + k)));
		tick(0x4454, 324);                             // one entry moved by the loop at 0x4454
	}
	cyc(138 + 51);
	poke((uint16_t)(0x0370 + (place << 3)), hi);
	poke((uint16_t)(0x0372 + (place << 3)), lo);
	return place;
}

// A7:5684: the first object from 0x0994 down to 5 whose box (animation 0x03F0: x, y, width,
// height) overlaps the skater's (0x023A at 0x0630/0x0238) and whose shapes touch (A7:572A).
// Returns the object or 0.
uint16_t find_collision(const Args &)
{
	cyc(198);
	uint16_t saved[6];
	for (int i = 1; i <= 5; i++) saved[i] = T.r[i];
	const uint16_t box = peek(0x023A);
	uint16_t result = 0;
	for (uint16_t r2 = (uint16_t)((sext8(peekb(0x0994)) - 5) << 1);; r2 = (uint16_t)(r2 - 2))
	{
		if ((int16_t)r2 < 0)
		{
			cyc(24 + 12);                              // TST R2 / BLT, CLR R0
			break;
		}
		int pass = 24 + 39 + 30;                       // tests of the loop and the object, SUB/BR
		const uint16_t p = peek((uint16_t)(0x03FA + r2));
		if (!p)
		{
			tick(0x569C, pass);
			continue;
		}
		pass += 138;
		uint16_t a1 = (uint16_t)(peek((uint16_t)(0x063A + r2)) + peek(p));
		uint16_t b1 = (uint16_t)(peek(0x0630) + peek(box));
		uint16_t d = (uint16_t)(a1 - b1);
		if ((int16_t)a1 >= (int16_t)b1)
		{
			pass += 39;
			if (d >= peek((uint16_t)(box + 4)))
			{
				tick(0x569C, pass + 12);
				continue;
			}
		}
		else
		{
			pass += 51;
			if ((uint16_t)-d >= peek((uint16_t)(p + 4)))
			{
				tick(0x569C, pass);
				continue;
			}
		}
		pass += 129;
		a1 = (uint16_t)(peek((uint16_t)(0x067A + r2)) + peek((uint16_t)(p + 2)));
		b1 = (uint16_t)(peek(0x0238) + peek((uint16_t)(box + 2)));
		d = (uint16_t)(a1 - b1);
		if ((int16_t)a1 >= (int16_t)b1)
		{
			pass += 39;
			if (d >= peek((uint16_t)(box + 6)))
			{
				tick(0x569C, pass + 12);
				continue;
			}
		}
		else
		{
			pass += 63;
			if ((uint16_t)-d >= peek((uint16_t)(p + 6)))
			{
				tick(0x569C, pass);
				continue;
			}
		}
		cyc(pass - 30 + 153);                          // this pass so far, the call
		const uint16_t touch = call(0x572A, {d, (uint16_t)(r2 + 10), r2, box});
		cyc(87);
		if (touch)
		{
			cyc(66);
			result = (uint16_t)((r2 >> 1) + 5);
			break;
		}
		tick(0x569C, 30);
	}
	cyc(111);
	for (int i = 1; i <= 5; i++) T.r[i] = saved[i];
	return result;
}

// A7:50F8 (assembly): the skater after a fall (0x031F = 1): the timer 0x02FA runs down, then
// F:B602(2, 2) and sound 0x55 end the fall; on a slope (0x0327 bits 0-2) the skater turns to
// the slope's direction (0xFCD0, F:9B82), on flat ground he slows down by 20.  State 2 is
// not used by the game.  When the state is clear the kick state is reset (F:E238).
void fallen_skater()
{
	cyc(36);
	const uint8_t st = peekb(0x031F);
	T.r[1] = sext8(st);
	fl::movb(st);
	if (!st)
	{
		cyc(21);
		return;
	}
	cyc(36);
	if (peek(0x02FA))
	{
		cyc(27);
		poke(0x02FA, (uint16_t)(peek(0x02FA) - 1));
	}
	cyc(30);
	if (T.r[1] == 1)
	{
		cyc(42);
		bool end;
		if ((int16_t)peek(0x02FC) >= 0x0380)
		{
			cyc(36);
			if (!peek(0x02FA))
				end = true;
			else
			{
				cyc(27 + 36);
				poke(0x02FA, (uint16_t)(peek(0x02FA) - 1));
				end = !peek(0x02FA);
			}
		}
		else
		{
			cyc(36);
			end = !peek(0x02FA);
		}
		if (end)
		{
			cyc(90);
			call(0xB602, {0x0002, 0x0002});
			cyc(84);
			pokeb(0x031F, 0);
			call(0x8E78, {0x0055});
			cyc(24);
		}
		cyc(42);
		if (peekb(0x0327) & 7)
		{
			cyc(177);
			T.r[0] = rb((uint16_t)(0xFCD0 + (peekb(0x0327) >> 3)));
			T.r[2] = peekb(0x031D);
			if (T.r[0] != T.r[2])
			{
				cyc(84);
				pokeb(0x031D, (uint8_t)T.r[0]);
				pokeb(0x01F8, (uint8_t)T.r[0]);
				call(0x9B82, {});
			}
		}
		else
		{
			cyc(36);
			if (!peekb(0x031E))
			{
				cyc(63);
				const uint16_t v = (uint16_t)(peek(0x01B4) - 0x0014);
				poke(0x01B4, v);
				if (!((int16_t)v > 0))
				{
					cyc(87);
					poke(0x01B4, 0);
					call(0x8E78, {0x0055});
					cyc(30);
				}
			}
		}
	}
	cyc(30);
	if (T.r[1] == 2)
	{
		cyc(63);
		poke(0x01B4, 0);
		if (!peek(0x02FA))
		{
			cyc(132);
			pokeb(0x031B, 0x3F);
			pokeb(0x031F, 0);
			T.r[0] = sext8(peekb(0x0327)) & 0x0007;
			if (T.r[0] >= 4)
			{
				cyc(147);
				pokeb(0x0325, 0x7F);
				T.r[0] = (uint16_t)(peekb(0x0327) >> 3);
				pokeb(0x031D, rb((uint16_t)(0xFCD0 + T.r[0])));
			}
		}
	}
	cyc(36);
	const uint8_t now = peekb(0x031F);
	fl::tstb(now);
	if (!now)
	{
		cyc(30);
		call(0xE238, {});                             // reset_skater_state
	}
	cyc(21);
}

// A7:4B3E: every 128 frames ask the sound CPU for its status (command 9); a bit of 0-2 that
// stays set from the last answer (0x01A3) marks it as failing (0x01A2 = 1)
uint16_t poll_sound_status(const Args &)
{
	cyc(135 + 42);
	if (peekb(0x01C4) & 0x7F)
	{
		cyc(21);
		return T.r[0];
	}
	cyc(30);
	call(0x8E6A, {});                                  // set_flag_01a4
	cyc(60);
	push(0x0009);
	call(0x8EA2, {});                                  // send_sound_command
	cyc(30);
	const uint16_t status = call(0x8ED2, {});          // read_sound_response
	cyc(42);
	call(0x8F62, {});
	cyc(30);
	call(0x8E72, {});                                  // clear_flag_01a4
	cyc(102);
	const uint16_t r0 = peekb(0x01A3) & status;
	if (r0 & 7)
	{
		cyc(33);
		pokeb(0x01A2, 1);
	}
	cyc(45 + 21);
	pokeb(0x01A3, (uint8_t)status);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	return r0;
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
// greeting (0x01A3 cleared then).  If it never answers the game is restarted (F:8000).
uint16_t reset_sound_cpu(const Args &)
{
	cyc(135 + 45);
	pokeb(0x01A2, 0);
	for (int n = 5; n > 0; n--)
	{
		cyc(105);
		const uint16_t r0 = peek(0x1C00);
		(void)peekb(r0);
		(void)peek(0x15A0);
		poke(0x15A0, 0x0001);
		for (int i = 0xC9; i > 0; i--) tick(0x4B1E, 12 + 18);   // INC R2 / SOB delay
		cyc(57);
		(void)peek(0x15A0);
		poke(0x15A0, 0);
		const uint16_t r = sound_byte();
		cyc(30);
		if (r == 0x00FF)
		{
			cyc(48);
			pokeb(0x01A3, 0);
			return r;
		}
		cyc(18);
	}
	cyc(30);
	const uint16_t r0 = call_from(0x4B3C, 0x8000, {});  // reset (does not come back)
	cyc(21);
	return r0;
}

// A7:435A  read_switch_settings(): command 0x0B; the sound CPU answers with 6 bytes: the coin
// switches 0x01FA / 0x01F3 / 0x01F4 and the credits (stored in 0x01F9; a coin sound 0x68 when
// bits 2-6 went up)
uint16_t read_switch_settings(const Args &)
{
	cyc(135 + 30);
	call(0x8E6A, {});                                  // set_flag_01a4
	cyc(60);
	push(0x000B);
	call(0x8EA2, {});                                  // send_sound_command
	sound_byte_after(30);
	const uint8_t coins_a = sound_byte_after(30);
	cyc(57);
	pokeb(0x01FA, coins_a);
	const uint16_t credits = sound_byte();
	const uint8_t coins_b = sound_byte_after(42);
	cyc(57);
	pokeb(0x01F3, coins_b);
	const uint8_t coins_c = sound_byte();
	cyc(57);
	pokeb(0x01F4, coins_c);
	call(0x8F62, {});
	cyc(108);
	T.r[0] = credits & 0x007C;
	if ((int16_t)T.r[0] > (int16_t)(peekb(0x01F9) & 0x007C))
	{
		cyc(57);
		poke(T.r[6], 0x0068);
		call(0x8EA2, {});
	}
	cyc(57);
	pokeb(0x01F9, (uint8_t)credits);
	call(0x8E72, {});                                  // clear_flag_01a4
	cyc(39);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	return T.r[0];
}

// A7:4476  read_high_scores(): command 0x11: the 30 high score entries (8 bytes at 0x0370:
// score bytes, then three initials; the first 15 with a third score byte, the others with a
// one-bit flag) come from the sound CPU, which keeps them; 30 + 25 more bytes are skipped
uint16_t read_high_scores(const Args &)
{
	cyc(135 + 30);
	call(0x8E6A, {});                                  // set_flag_01a4
	cyc(60);
	push(0x0011);
	call(0x8EA2, {});                                  // send_sound_command
	cyc(12);
	for (uint16_t i = 0; (int16_t)i <= 0x1D; i++)
	{
		cyc(120);
		const uint16_t e = (uint16_t)(0x0370 + (i << 3));
		poke(e, 0);
		if ((int16_t)i <= 14)
		{
			pokeb(e, sound_byte_after(30));
			pokeb((uint16_t)(e + 3), sound_byte_after(60));
			pokeb((uint16_t)(e + 2), sound_byte_after(60));
			cyc(42);
		}
		else
		{
			pokeb((uint16_t)(e + 2), sound_byte_after(30));
			pokeb((uint16_t)(e + 3), sound_byte_after(60));
			cyc(66);
			pokeb((uint16_t)(e + 3), peekb((uint16_t)(e + 3)) & 0x01);
		}
		pokeb((uint16_t)(e + 4), sound_byte_after(30));
		pokeb((uint16_t)(e + 5), sound_byte_after(60));
		cyc(90);
		poke((uint16_t)(e + 6), 0);
		pokeb((uint16_t)(e + 6), (uint8_t)sound_byte());
		cyc(60);
		if (i == 14)
		{
			cyc(18);
			for (int n = 30; n > 0; n--)
			{
				sound_byte_after(30);
				cyc(18);
			}
		}
		cyc(42);
	}
	cyc(18);
	for (int n = 25; n > 0; n--)
	{
		sound_byte_after(30);
		cyc(18);
	}
	cyc(30);
	call(0x8F62, {});
	cyc(30);
	call(0x8E72, {});                                  // clear_flag_01a4
	cyc(39);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	return T.r[0];
}

// A7:4516  show_high_scores(): read the table (A7:4476), print the titles 0x7710/0x7718/0x771E
// (F:A9FC) and the 30 entries: initials with the small font, the score by F:BA96 (text
// layer positions from 0x7F7C, bank B 0x1C00).  Bank B ends as 0x3C00 (also left in R0).
uint16_t show_high_scores(const Args &)
{
	cyc(183 + 30);
	call(0x4476, {});                                  // read_high_scores
	cyc(60);
	push(0x7710);
	call(0xA9FC, {});
	cyc(57);
	poke(T.r[6], 0x7718);
	call(0xA9FC, {});
	cyc(57);
	poke(T.r[6], 0x771E);
	call(0xA9FC, {});
	cyc(84);
	set_bank_b(0x1C00);
	for (uint16_t i = 0; (int16_t)i <= 0x1D; i++)
	{
		cyc(177);
		poke(0x0186, (uint16_t)(peek((uint16_t)(0x7F7C + (i << 1))) + 0x2000));
		const uint16_t e = (uint16_t)(i << 3);
		for (uint16_t k = 4; k <= 6; k++)
		{
			cyc(93);
			T.r[0] = peekb((uint16_t)(0x0370 + e + k));
			call(0x8C12, {});                          // put_small_char
			cyc(42);
		}
		cyc(222);
		poke(0x0186, (uint16_t)(peek(0x0186) + 6));
		poke(T.r[6], peek((uint16_t)(0x0372 + e)));
		push(peek((uint16_t)(0x0370 + e)));
		push(0x0007);
		push(0x0001);
		push(0x0000);
		call(0xBA96, {});
		cyc(60);
		T.r[6] = (uint16_t)(T.r[6] + 8);
	}
	cyc(111);
	set_bank_b(0x3C00);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	return 0x3C00;
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
	cyc(93);
	poke(0x0222, 0x0001);
	if (!peekb(0x01EA))
	{
		cyc(45);
		if (a.u(0) == 0x000F)
		{
			cyc(90);
			const uint16_t r0 = (uint16_t)((peek(0x0570) << 1) - peek(0x04F0));
			if ((r0 & 0x8000) || !r0)                  // BLE after CCC CV
			{
				cyc(39);
				return r0;
			}
			cyc(300 + 39);
			poke(0x04F0, (uint16_t)-asr(peek(0x04F0), 1));
			poke(0x0570, (uint16_t)-asr(peek(0x0570), 1));
			const uint16_t d = (uint16_t)(0x0004 - sext8(peekb(0x031D)));
			pokeb(0x031D, 0);
			pokeb(0x031D, (uint8_t)(d & 0x000F));
			return d;
		}
	}
	cyc(114);
	const uint16_t t = a.u(0);
	pokeb(0x0329, (uint8_t)t);
	if (peekb(0x0328) == t)
	{
		cyc(39);
		return t;
	}
	cyc(81);
	pokeb(0x0328, (uint8_t)t);
	push(t);
	uint16_t r0 = call(0x9E52, {});                    // slope_table_value
	cyc(36);
	poke(T.r[6], r0);
	if (peekb(0x01EA))
	{
		cyc(51);
		r0 = call(0x9EEE, {});
		cyc(12);
	}
	else
	{
		cyc(51);
		r0 = call(0x9E96, {});
	}
	cyc(18 + 39);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	return r0;
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
	cyc(90);
	T.r[0] = peek(0x0304);
	T.r[2] = peekb(0x0ED0);
	fl::cmp(T.r[2], 0x0001);
	if (T.r[2] == 1)
	{
		cyc(114);
		T.r[1] = 0x2C00;
		set_bank_b(0x2C00);
		if ((int16_t)peek(0x06B0) >= 0x1500)
		{
			cyc(63);
			poke(0x06B0, 0x1500);
			T.r[0] = 0x0017;
		}
		else
		{
			cyc(36);
			T.r[1] = sext8(peekb(0x02E7));
			if (T.r[1] & 0x8000)
			{
				cyc(12);
				T.r[1] = (uint16_t)-T.r[1];
			}
			cyc(120);
			T.r[1] = (uint16_t)((T.r[1] << 3) + peek(0x06B0));
			T.r[0] = asr(T.r[1], 8);
			if (T.r[0] & 0x8000)
			{
				cyc(12);
				T.r[0] = 0;
			}
		}
		cyc(36);
		if (peekb(0x032F))
		{
			cyc(18);
			T.r[1] = 0x0010;
		}
		else
		{
			cyc(24);
			T.r[1] = 0;
		}
		cyc(105);
		T.r[2] = peekb((uint16_t)(0x7C48 + T.r[0]));
		T.r[1] |= T.r[2];
		T.r[0] = T.r[1];
		if ((int16_t)peek(0x06B2) > 0x1800)
		{
			cyc(36);
			if (peekb(0x032F))
			{
				cyc(33);
				poke(0x0ED4, 0x1400);
			}
			else
			{
				cyc(45);
				poke(0x0ED4, 0xA200);
			}
		}
		cyc(39);
		poke(0x0222, 0);
		fl::clr();
	}
	else
	{
		cyc(30);
		fl::cmp(T.r[2], 0x0005);
		bool frame78 = T.r[2] == 5;
		if (!frame78)
		{
			cyc(30);
			fl::cmp(T.r[2], 0x0002);
			frame78 = T.r[2] == 2;
		}
		if (frame78)
		{
			cyc(18);
			T.r[0] = 0x0078;
		}
	}
	cyc(102);
	poke(0x0306, T.r[0]);
	poke(0x0304, T.r[0]);
	poke(0x0302, T.r[0]);
	fl::mov(T.r[0]);
}

// ---------------------------------------------------------------- tricks (not reached by the tests)
// The trick moves 0x40-0x57 of 0x01EB (set up by A5:5264 on a ramp) play an animation from the
// table 0x01E0 (frames 0x01E2/0x01E4, step 0x01EE, 0x07B2) and change the skater's state.
// The game never sets them up in the test scenarios; these routines were written from the
// disassembly and are not reached by the tests.

// A5:5388  trick_facing(): 1 when the skater's direction 0x01F8 is within 2 of 6, else 0
uint16_t trick_facing(const Args &)
{
	cyc(72);
	uint16_t r0 = (uint16_t)((sext8(peekb(0x01F8)) & 7) - 6);
	if (r0 & 0x8000)
	{
		cyc(12);
		r0 = (uint16_t)-r0;
	}
	cyc(30);
	if ((int16_t)r0 > 2)
	{
		cyc(33);
		return 0;
	}
	cyc(39);
	return 1;
}

// A5:543E  next_trick_frame(): advance the animation (0x01E2/0x01E4 += 2, 0x01EE + 1 per
// step) to the next entry of the table 0x01E0 + 0x07B2 that ends a sequence (negative byte);
// returns its address
uint16_t next_trick_frame(const Args &)
{
	cyc(72);
	uint16_t r0 = (uint16_t)(peekb(0x07B2) + peek(0x01E0) - 1);
	do
	{
		cyc(135);
		r0 = (uint16_t)(r0 + 1);
		poke(0x01E2, (uint16_t)(peek(0x01E2) + 2));
		poke(0x01E4, (uint16_t)(peek(0x01E4) + 2));
		pokeb(0x01EE, (uint8_t)(peekb(0x01EE) + 1));
	} while (!(peekb(r0) & 0x80));
	cyc(21);
	return r0;
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
	cyc(258);
	pokeb(0x07B1, 0);
	poke(0x03B2, peek(0x01E2));
	poke(0x03B4, peek(0x01E4));
	pokeb(0x07B2, peekb(0x01EE));
	const uint16_t r1 = sext8(peekb((uint16_t)(peekb(0x07B2) + peek(0x01E0))));
	if (!(r1 & 0x8000))
	{
		cyc(96);
		const uint16_t r0 = peek(0x0772) & 0x007F;
		pokeb(0x0772, (uint8_t)r0);
		pokeb(0x0771, (uint8_t)r0);
	}
	cyc(30);
	call(0x543E, {});                                  // next_trick_frame
	cyc(36);
	bool count = true;
	if (peekb(0x01F0))
	{
		cyc(81);
		pokeb(0x01F0, 0);
		call(0x53A8, {r1});                            // trick_landing
		cyc(54);
		count = !peekb(0x01F0);
	}
	if (count)
	{
		cyc(27);
		pokeb(0x01F2, (uint8_t)(peekb(0x01F2) + 1));
	}
	cyc(60);
	const uint16_t r0 = peekb(0x01EF);
	if (r0)
	{
		cyc(54);
		pokeb(0x01EB, (uint8_t)r0);
		pokeb(0x01EF, 0);
	}
	cyc(39);
	return r0;
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
	cyc(183 + 60);
	const uint16_t r4 = peek(0x0570), r3 = peek(0x04F0);
	uint16_t r1, r0 = T.r[0];
	if (!r3)
	{
		cyc(24 + 30);
		r1 = (int16_t)r4 > 0 ? 0x0004 : 0x000C;
	}
	else
	{
		cyc(162);
		r0 = call(0x90A6, {r3, (uint16_t)(r4 << 4)});  // divide
		const uint16_t r2 = r0;
		cyc(24);
		if (r0 & 0x8000)
		{
			cyc(12);
			r0 = (uint16_t)-r0;
		}
		cyc(12);
		if (!r0)
		{
			cyc(24);
			if ((int16_t)r3 > 0)
			{
				cyc(24);
				r1 = 0;
			}
			else
			{
				cyc(30);
				r1 = 8;
			}
		}
		else
		{
			cyc(30);
			if ((int16_t)r0 <= 2)
			{
				cyc(24);
				r1 = 0;
			}
			else
			{
				cyc(30);
				if ((int16_t)r0 <= 6)
				{
					cyc(30);
					r1 = 1;
				}
				else
				{
					cyc(30);
					if ((int16_t)r0 <= 13)
					{
						cyc(30);
						r1 = 2;
					}
					else
					{
						cyc(30);
						if ((int16_t)r0 <= 50)
						{
							cyc(30);
							r1 = 3;
						}
						else
						{
							cyc(18);
							r1 = 4;
						}
					}
				}
			}
		}
		cyc(24);
		if (r2 & 0x8000)
		{
			cyc(30);
			r1 = (uint16_t)(0x0008 - r1);
		}
		cyc(24);
		if (r4 & 0x8000)
		{
			cyc(24);
			if (r2)
			{
				cyc(30);
				if (r1 == 8)
				{
					cyc(24);
					r1 = 0;
				}
				else
				{
					cyc(18);
					r1 = (uint16_t)(r1 + 8);
				}
			}
		}
		cyc(24);
	}
	cyc(48);
	pokeb(0x031D, (uint8_t)r1);
	return r0;
}

}

LOGIC_TABLE(banka,
	c_routine("A5", 0x438C, "step_map_column", step_map_column, 0, 0),
	asm_routine("A5", 0x42BC, "step_map_row", step_map_row, 0),
	asm_routine("A5", 0x41CE, "draw_map_strip", draw_map_strip, 0),
	c_routine("A5", 0x4000, "init_map_view", init_map_view, 0, 111),
	asm_routine("A5", 0x48CC, "update_map_slots", update_map_slots, 0),
	c_routine("A5", 0x5D08, "stop_effect_sound", stop_effect_sound, 0, 0),
	c_routine("A5", 0x5B46, "rolling_sound", rolling_sound, 0, 87),
	wait_routine("A5", 0x5BD2, "town_music", town_music, 0),
	c_routine("A5", 0x4984, "draw_text_block_3x3", draw_text_block_3x3, 0, 105),
	c_routine("A5", 0x51EE, "slope_height", slope_height, 0, 69),
	asm_routine("A7", 0x4098, "move_objects_0_3", move_objects_0_3, 3003),
	c_routine("A7", 0x410C, "project_speed", project_speed, 0, 1140),
	c_routine("A7", 0x4F98, "direction_from_object7", direction_from_object7, 0, 87),
	c_routine("A7", 0x4ECC, "hold_object", hold_object, 0, 0),
	c_routine("A7", 0x4D30, "message_box", message_box, 0, 111),
	asm_routine("A7", 0x4B84, "expire_messages", expire_messages, 0),
	asm_routine("A7", 0x4C28, "show_next_message", show_next_message, 0),
	c_routine("A7", 0x5A7E, "credit_messages", credit_messages, 0, 87),
	c_routine("A7", 0x5018, "chaser_step", chaser_step, 0, 69),
	c_routine("A7", 0x5248, "shop_purchase", shop_purchase, 0, 69),
	c_routine("A7", 0x43C0, "insert_high_score", insert_high_score, 0, 111),
	c_routine("A7", 0x5684, "find_collision", find_collision, 0, 0),
	asm_routine("A7", 0x50F8, "fallen_skater", fallen_skater, 0),
	wait_c_routine("A7", 0x4B3E, "poll_sound_status", poll_sound_status, 69),
	asm_routine("A5", 0x454A, "scan_zone_switches", scan_zone_switches, 0),
	c_routine("A7", 0x538E, "terrain_event", terrain_event, 0, 39),
	asm_routine("A7", 0x572A, "shapes_touch", shapes_touch, 39),
	wait_c_routine("A7", 0x4B00, "reset_sound_cpu", reset_sound_cpu, 69),
	wait_c_routine("A7", 0x435A, "read_switch_settings", read_switch_settings, 69),
	wait_c_routine("A7", 0x4476, "read_high_scores", read_high_scores, 69),
	wait_c_routine("A7", 0x4516, "show_high_scores", show_high_scores, 105),
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
	c_routine("A7", 0x4A66, "velocity_direction", velocity_direction, 0, 105),
)

}
