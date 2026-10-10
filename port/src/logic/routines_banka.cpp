// Rewritten routines of the banked segments A5 and A7: the Skate City map view, object
// motion, sounds driven by the game state, and the message boxes of the text layer.
#include "logic.h"

namespace logic {
namespace {

using namespace t11rt;

uint16_t sext8(uint8_t v) { return (uint16_t)(int16_t)(int8_t)v; }
uint16_t asr(uint16_t v, int n) { return (uint16_t)((int16_t)v >> n); }

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
	if (a.s(0) > 0)
	{
		r0 = (uint16_t)(peekb(0x0F44) + 0x20);
		pokeb(0x0F44, (uint8_t)r0);
		poke(0x0F46, (uint16_t)(peek(0x0F46) + 2));
		r0 = (uint16_t)(peekb(0x0F44) + 0xA0);
		pokeb(0x0F45, (uint8_t)r0);
		poke(0x0F48, (uint16_t)(peek(0x0F46) + 10));
	}
	else
	{
		r0 = (uint16_t)(peekb(0x0F44) - 0x20);
		pokeb(0x0F44, (uint8_t)r0);
		poke(0x0F46, (uint16_t)(peek(0x0F46) - 2));
		pokeb(0x0F45, (uint8_t)r0);
		poke(0x0F48, peek(0x0F46));
	}
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
	if (dir > 0)
	{
		poke(0x0F40, (uint16_t)(peek(0x0F40) - 0x0800));
		T.r[1] = peekb(0x0F5A);
		if (peek(0x0F40) & 0x0800)
		{
			poke(0x0F46, (uint16_t)(peek(0x0F46) - T.r[1]));
			T.r[0] = (uint16_t)(sext8(peekb(0x0F5B)) - 1);
			pokeb(0x0F5B, (uint8_t)T.r[0]);
			if ((int8_t)T.r[0] < 0)
			{
				pokeb(0x0F5B, 0x17);
				T.r[0] = (uint16_t)(peek(0x0F46) - 0x0180);
				poke(0x0F46, (uint16_t)(0x0A6C + T.r[0]));
			}
			T.r[0] = (uint16_t)(T.r[1] * 3);
		}
		else
			T.r[0] = (uint16_t)(T.r[1] << 2);
		poke(0x0F48, (uint16_t)(peek(0x0F46) - T.r[0]));
		poke(0x0F42, (uint16_t)(peek(0x0F40) - 0x3800));
	}
	else
	{
		poke(0x0F40, (uint16_t)(peek(0x0F40) + 0x0800));
		if (!(peek(0x0F40) & 0x0800))
		{
			T.r[0] = peekb(0x0F5A);
			poke(0x0F46, (uint16_t)(peek(0x0F46) + T.r[0]));
			T.r[0] = (uint16_t)(sext8(peekb(0x0F5B)) + 1);
			pokeb(0x0F5B, (uint8_t)T.r[0]);
			T.r[0] = (uint16_t)(peek(0x099E) << 1);
			T.r[1] = sext8(peekb(0x0F5B));
			if (T.r[1] == T.r[0])
			{
				pokeb(0x0F5B, 0);
				T.r[0] = (uint16_t)(peek(0x0F46) - 0x0A80 + 0x0194);
				poke(0x0F46, T.r[0]);
			}
		}
		poke(0x0F42, peek(0x0F40));
		poke(0x0F48, peek(0x0F46));
	}
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
	T.r[6] = (uint16_t)(T.r[6] - 2);
	const uint16_t local = T.r[6];
	poke(local, 0);
	pokeb(local, peekb(0x0F5D));
	if (peek(local))
	{
		do
		{
			pokeb(0x0F5D, (uint8_t)(peekb(0x0F5D) - 1));
			poke(0x0F42, peek(0x0F42) & ~0xC001);
			call(0xE82E, {});
			poke(0x0F42, (uint16_t)(peek(0x0F42) + peek(0x0F4A)));
			T.r[1] = (uint16_t)~peek(0x0F4A);
			T.r[0] = peek(0x0F42) & ~T.r[1];
			if (T.r[0] & 0x0800)
			{
				T.r[0] = peekb(0x0F5A);
				poke(0x0F48, (uint16_t)(peek(0x0F48) - T.r[0]));
				poke(0x0F56, (uint16_t)(peek(0x0F56) - 6));
			}
			T.r[1] = sext8(peekb(0x0F5C));
			T.r[0] = (uint16_t)(peekb(0x0F45) + T.r[1]);
			pokeb(0x0F45, (uint8_t)T.r[0]);
			if ((uint8_t)T.r[1])
			{
				poke(0x0F48, (uint16_t)(peek(0x0F48) + 2));
				T.r[0] = peekb(0x0F58);
				if (T.r[0])
				{
					T.r[1] = (uint16_t)(T.r[0] << 1);
					pokeb(0x0F58, (uint8_t)T.r[1]);
					if (!(uint8_t)T.r[1])
					{
						pokeb(0x0F58, (uint8_t)(peekb(0x0F58) + 1));
						poke(0x0F56, (uint16_t)(peek(0x0F56) + 1));
					}
				}
			}
			poke(local, 0);
			pokeb(local, peekb(0x0F5D));
		} while (peek(local));
		pokeb(0x0F5E, (uint8_t)(peekb(0x0F5E) + 1));
	}
	T.r[0] = (uint16_t)((peek(0x01AA) + 0x0800) & ~0x0FFF);
	poke(local, (uint16_t)(peek(0x0F3E) - T.r[0]));
	if (peek(local))
	{
		call(0x42BC, {peek(local)});
		if ((int16_t)peek(local) >= 0) poke(0x0F3E, (uint16_t)(peek(0x0F3E) - 0x1000));
		else poke(0x0F3E, (uint16_t)(peek(0x0F3E) + 0x1000));
	}
	else
	{
		T.r[0] = (uint16_t)(((peek(0x01A8) + 0x1000) & ~0x1FFF) - peek(0x0F3C));
		poke(local, T.r[0]);
		if (!T.r[0])
		{
			T.r[6] = (uint16_t)(T.r[6] + 2);
			fl::tst(peek(local));
			return;
		}
		call(0x438C, {T.r[0]});
		if ((int16_t)peek(local) > 0) poke(0x0F3C, (uint16_t)(peek(0x0F3C) + 0x2000));
		else poke(0x0F3C, (uint16_t)(peek(0x0F3C) - 0x2000));
	}
	call(0x4894, {});                                  // town_alt_cell_bit
	T.r[6] = (uint16_t)(T.r[6] + 2);
	fl::tst(peek(local));
}

// A5:4000  init_map_view(int col, int row, int level): set the scroll position and the map
// pointers for the cell (col, row - 4) of level, then draw the 8 strips around it.  The
// row argument is left decremented on the stack, as the original does.
uint16_t init_map_view(const Args &a)
{
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
	if ((int16_t)level > 0 && (int16_t)level < 5)
	{
		uint16_t r0 = call(0x90BA, {});
		const uint16_t r1 = (uint16_t)(peekb((uint16_t)(0xFE8F + r0)) + peek(0x0F46));
		pokeb(0x0F5A, peekb((uint16_t)(r1 + 0x000C)));
		r0 = call(0x90BA, {});
		r0 = (uint16_t)((peekb((uint16_t)(0xFE8F + r0)) << 1) + peek(0x0F46));
		poke(0x0F46, peek(r0));
	}
	else
		pokeb(0x0F5A, peekb((uint16_t)(0x7F9C + level)));
	poke(0x0F46, (uint16_t)(peekb(0x0F5A) * row + peek(0x0F46) + (col << 1)));   // mul16
	poke(0x0F40, (uint16_t)(((row & 0x0F) << 12) - 0x0800));
	pokeb(0x0F44, (uint8_t)(col << 5));
	poke(0x099A, asr(row, 1));
	poke(0x099C, (uint16_t)(peek(0x099A) + 3));
	poke(0x0996, (uint16_t)(asr((uint16_t)(col - 1), 1) << 4));
	poke(0x0998, (uint16_t)(peek(0x0996) + 0x0040));
	poke(0x0F4E, peek((uint16_t)(0xFF54 + (peekb(0x0F59) << 1))));
	poke(0x099E, peekb(0x0F59) ? 0x0FFF : 0x000C);
	pokeb(0x0E9B, 0);
	poke(0x09A0, peek(0x01A8) & 0xC000);
	poke(0x09A2, peek(0x01AA) & 0xC000);
	pokeb(0x0F58, 0);
	T.r[0] = (uint16_t)(peekb(0x0F59) << 1);
	T.r[1] = (uint16_t)(col << 1);
	T.r[2] = row;
	T.r[3] = col;
	for (int n = 8; n > 0; n--)
	{
		call(0x42BC, {0xFFFF});                        // step_map_row
		call(0x4894, {});                              // town_alt_cell_bit
		call(0x41CE, {});                              // draw_map_strip
	}
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
	T.r[6] = (uint16_t)(T.r[6] - 2);
	const uint16_t local = T.r[6];
	pokeb(0x0E99, 0);
	poke(0x09AA, peek(0x09B0));
	T.r[4] = peek(0x099A);
	T.r[2] = peek(0x099C);
	T.r[3] = peek(0x0996);
	T.r[1] = peek(0x0998);
	if ((int16_t)T.r[4] < 0 && (int16_t)T.r[3] >= 0x00C0)
	{
		T.r[4] = (uint16_t)(T.r[4] + 0x000C);
		T.r[2] = (uint16_t)(T.r[2] + 0x000C);
		T.r[3] = (uint16_t)(T.r[3] - 0x0050);
		T.r[1] = (uint16_t)(T.r[1] - 0x0050);
	}
	for (;;)
	{
		T.r[0] = peek(0x09AA);
		const uint16_t last = peekb(0x0E98);
		if ((int16_t)T.r[0] < (int16_t)last) break;
		if (peekb((uint16_t)(0x09B8 + T.r[0])))
		{
			poke(local, 0);
			T.r[0] = peek((uint16_t)(0x0AF8 + (T.r[0] << 1)));
			T.r[5] = T.r[0] & 0x000F;
			if ((int16_t)T.r[5] >= (int16_t)T.r[4] && (int16_t)T.r[5] <= (int16_t)T.r[2])
			{
				T.r[5] = T.r[0] & 0x01F0;
				if ((int16_t)T.r[5] >= (int16_t)T.r[3] && (int16_t)T.r[5] < (int16_t)T.r[1])
					poke(local, (uint16_t)(peek(local) + 1));
			}
			if (!peek(local))
			{
				if ((int16_t)T.r[0] < 0) call(0xC3E6, {});
			}
			else if ((int16_t)T.r[0] >= 0)
				call(0xC4E4, {});
		}
		poke(0x09AA, (uint16_t)(peek(0x09AA) - 1));
	}
	T.r[6] = (uint16_t)(T.r[6] + 2);
	fl::tst(peek(local));
}

// ---------------------------------------------------------------- sounds

// A5:5D08  stop_effect_sound(int n): if sound effect n (table 0x6567, bank B 0x0400) plays on
// an effect channel (0x01A6 / 0x01A7), free the channel and queue its stop command
uint16_t stop_effect_sound(const Args &a)
{
	set_bank_b(0x0400);
	const uint16_t s = peekb((uint16_t)(0x6567 + a.u(0)));
	if (!s) return 0;
	uint16_t cmd;
	if (peekb(0x01A6) == s)
	{
		pokeb(0x01A6, 0);
		cmd = 0x0070;
	}
	else if (peekb(0x01A7) == s)
	{
		pokeb(0x01A7, 0);
		cmd = 0x0076;
	}
	else
		return s;
	T.r[0] = s;
	return call(0x8E78, {cmd});
}

// A5:5B46: rolling sound: when the skater's speed differs from the sound's level 0x01DC by
// at least 50, the level steps by 27 towards it with the matching sound command (0x46/0x48
// up, 0x49 down, 0x53 to silence below speed 128).  Not while 0x031F or 0x0330 is set.
uint16_t rolling_sound(const Args &)
{
	if (peekb(0x031F) || peekb(0x0330)) return T.r[0];
	const uint16_t speed = peek(0x01B4);
	const uint16_t mag = (int16_t)speed >= 0 ? speed : (uint16_t)-speed;
	uint16_t r0 = (uint16_t)(mag - peek(0x01DC));
	const uint16_t diff = (r0 & 0x8000) ? (uint16_t)-r0 : r0;
	if ((int16_t)diff < 0x0032) r0 = 0;
	if (!r0) return 0;
	if ((int16_t)r0 > 0)
	{
		const uint16_t cmd = (int16_t)peek(0x01DC) > 0 ? 0x0048 : 0x0046;
		T.r[0] = r0;
		const uint16_t res = call(0x8E78, {cmd});
		poke(0x01DC, (uint16_t)(peek(0x01DC) + 0x001B));
		return res;
	}
	poke(0x01DC, (uint16_t)(peek(0x01DC) - 0x001B));
	uint16_t cmd = 0x0049;
	if ((int16_t)mag < 0x0080 && peek(0x01DC))
	{
		poke(0x01DC, 0);
		cmd = 0x0053;
	}
	T.r[0] = mag;
	return call(0x8E78, {cmd});
}

// A5:5BD2 (assembly): Skate City music.  The music level 0x01E6 (0-13, commands from 0x5AF6)
// follows the distance of the view (0x0996, 0x099A) from the cell column 9, row 5.  Every
// 256 frames the sound CPU is asked whether a tune is playing (command 0x0D, interrupts off
// meanwhile); if not, a random tune not played yet (bits of 0x01DA, commands at 0x5B04)
// starts, the first time with F:C98E(5, 0x40) when 0x0B78 is negative.
void town_music()
{
	const uint8_t level = peekb(0x0F59);
	fl::tstb(level);
	if (level) return;
	T.r[1] = peek(0x0996);
	T.r[0] = peek(0x099A);
	if ((int16_t)T.r[0] < 0)
	{
		T.r[1] = (uint16_t)(T.r[1] + 0x0050);
		T.r[0] = (uint16_t)(T.r[0] + 0x000C);
	}
	T.r[2] = (uint16_t)(T.r[1] - 0x0090);
	if (T.r[2] & 0x8000) T.r[2] = (uint16_t)-T.r[2];
	T.r[1] = asr(T.r[2], 4);
	T.r[2] = (uint16_t)(T.r[0] - 0x0005);
	if (T.r[2] & 0x8000) T.r[2] = (uint16_t)-T.r[2];
	T.r[0] = (uint16_t)(T.r[2] << 1);
	if (!((int16_t)T.r[1] > (int16_t)T.r[0])) T.r[1] = T.r[0];
	if ((int16_t)T.r[1] > 0x000D) T.r[1] = 0x000D;
	T.r[0] = peekb(0x01E6);
	if (T.r[1] != T.r[0])
	{
		call(0x8E78, {peekb((uint16_t)(0x5AF6 + T.r[1]))});
		pokeb(0x01E6, (uint8_t)T.r[1]);
	}
	const uint8_t frame = peekb(0x01C4);
	fl::tstb(frame);
	if (frame) return;
	T.r[2] = 0x00E0;
	T.psw = (uint8_t)((T.psw & 0x10) | 0xE0);         // MTPS: interrupts off
	T.check_irqs = true;
	T.r[6] = (uint16_t)(T.r[6] - 2);
	const uint16_t arg = T.r[6];
	poke(arg, 0x000D);
	call(0x8EA2, {});                                  // send_sound_command(0x0D)
	call(0x8ED2, {});                                  // read_sound_response
	T.r[1] = T.r[0];
	call(0x8F62, {});
	T.r[2] = 0;
	T.psw = (uint8_t)(T.psw & 0x10);                   // MTPS #0
	T.check_irqs = true;
	if (!T.r[1])
	{
		if (!peekb(0x01DA)) pokeb(0x01DA, 0xFF);
		call(0x9152, {});                              // next_random
		for (;;)
		{
			T.r[1] = T.r[0] & 0x0007;
			T.r[0] = (uint16_t)(T.r[1] << 1);
			if (peekb(0x01DA) & rb((uint16_t)(0xFC7C + T.r[0]))) break;
			T.r[0] = (uint16_t)(T.r[1] + 1);
		}
		T.r[0] = (uint16_t)(T.r[1] << 1);
		pokeb(0x01DA, peekb(0x01DA) & ~rb((uint16_t)(0xFC7C + T.r[0])));
		T.r[1] = rb((uint16_t)(0x5B04 + T.r[1]));
		poke(arg, T.r[1]);
		call(0x8E78, {});                              // queue_sound(tune)
		if (!peekb(0x01E7))
		{
			if ((int16_t)peek(0x0B78) < 0)
			{
				poke(arg, 0x0040);
				call(0xC98E, {0x0005});
			}
			pokeb(0x01E7, (uint8_t)(peekb(0x01E7) + 1));
		}
	}
	T.r[6] = (uint16_t)(T.r[6] + 2);
	fl::tst(peek(arg));
}

// ---------------------------------------------------------------- objects

// A5:4984  draw_tile_block(int kind, int row, int col): draw a 3 x 3 block of text tiles
// (table 0x495C: kind 0, or any other kind as 9; colour 0x0188) at row * 0x180 plus the
// column offset 0x4980[col].  A non-zero kind is written back as 9 into the argument.
uint16_t draw_tile_block(const Args &a)
{
	call(0x8068, {});                                  // select_video_bank0
	uint16_t r0 = (uint16_t)(a.u(1) * 0x0180 + peek((uint16_t)(0x4980 + (a.u(2) << 1))));
	poke(0x0186, r0);
	poke(0x0186, (uint16_t)(peek(0x0186) + 0x1E80));
	if (a.u(0)) poke((uint16_t)(a.sp + 2), 0x0009);
	const uint16_t first = a.u(0), last = (uint16_t)(first + 6);
	for (uint16_t i = first; (int16_t)i <= (int16_t)last; i = (uint16_t)(i + 3))
	{
		r0 = peek(0x0186);
		for (int j = 0; j < 3; j++)
			poke((uint16_t)(r0 + 2 * j), peek((uint16_t)(0x495C + (i << 1) + 2 * j)) | peek(0x0188));
		poke(0x0186, (uint16_t)(peek(0x0186) + 0x0080));
	}
	return r0;
}

// A5:51EE  slope_height(int t): 0x02E6 minus the height table entry 0x6AC8 (bank B 0x2C00)
// for the skater direction 0x031D and the terrain t: low bits 0 use the direction alone, 7
// the row (bits 3-5) of a 16-entry block at 0x220, others the row table at 0x79E8.  Bank B
// ends as 0x0C00.
uint16_t slope_height(const Args &a)
{
	set_bank_b(0x2C00);
	uint16_t r0 = sext8(peekb(0x031D));
	const uint16_t t = a.u(0);
	if (t & 7)
	{
		if ((t & 7) == 7)
			r0 = (uint16_t)(((t & 0x0038) << 1) + r0 + 0x0220);
		else
			r0 = (uint16_t)(((t & 7) << 4) + peek((uint16_t)(0x79E8 + (asr(t, 3) << 1))) + r0);
	}
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
	const uint16_t dx = (uint16_t)(peek(0x06F0) - peek(0x06FE) + 0x0500 + (peek(0x01C4) & 0x01FF));
	const uint16_t dy = (uint16_t)(peek(0x0730) - peek(0x073E) + peek(0x06B0) + 0x0500);
	const uint16_t ax = (int16_t)dx < 0 ? (uint16_t)-dx : dx;
	uint16_t r;
	if ((int16_t)ax < 0x0200)
		r = (int16_t)dy < 0 ? 0x000C : 0x0004;
	else
	{
		const uint16_t ay = (int16_t)dy < 0 ? (uint16_t)-dy : dy;
		if ((int16_t)ay < 0x0200)
			r = (int16_t)dx < 0 ? 0x0008 : 0x0000;
		else
		{
			r = (int16_t)dx < 0 ? 0x0006 : 0x0002;
			if ((int16_t)dy < 0) r = (uint16_t)(0x0010 - r);
		}
	}
	return r;
}

// A7:4ECC  hold_object(int i): object i is parked: its animation, byte 0x0770 and velocities
// go to the template 0x0E5E-0x0E69 (respawned later by A7:4F42), and it stops (0x04B0 bit
// 0x40).  Object 6 instead gets the animation 0x50FA in the template slot of object 5.
uint16_t hold_object(const Args &a)
{
	const uint16_t i = a.u(0), w = (uint16_t)(i << 1);
	if (i == 6)
	{
		poke(0x03BA, 0x50FA);
		pokeb(0x0495, peekb(0x0495) | 0x18);
		poke(0x04FA, 0);
		poke(0x057A, 0);
		poke(0x043A, 0);
	}
	else
	{
		pokeb(0x0E6A, (uint8_t)i);
		pokeb(0x0E68, 0xB4);
		poke(0x0E66, peek((uint16_t)(0x0430 + w)));
		pokeb(0x0E69, peekb((uint16_t)(0x0770 + i)));
		poke(0x0E5E, peek((uint16_t)(0x04F0 + w)));
		poke(0x0E60, peek((uint16_t)(0x0570 + w)));
		poke(0x0E62, peek((uint16_t)(0x03B0 + w)));
		poke(0x0E64, peek((uint16_t)(0x03F0 + w)));
	}
	poke((uint16_t)(0x0430 + w), 0);
	pokeb((uint16_t)(0x04B0 + i), peekb((uint16_t)(0x04B0 + i)) | 0x40);
	poke((uint16_t)(0x03F0 + w), 0);
	poke((uint16_t)(0x04F0 + w), 0);
	poke((uint16_t)(0x0570 + w), 0);
	return w;
}

}

LOGIC_TABLE(banka,
	c_routine("A5", 0x438C, "step_map_column", step_map_column, 0, 438),
	asm_routine("A5", 0x42BC, "step_map_row", step_map_row, 464),
	asm_routine("A5", 0x41CE, "draw_map_strip", draw_map_strip, 383),
	c_routine("A5", 0x4000, "init_map_view", init_map_view, 0, 2982195),
	asm_routine("A5", 0x48CC, "update_map_slots", update_map_slots, 30961),
	c_routine("A5", 0x5D08, "stop_effect_sound", stop_effect_sound, 0, 306),
	c_routine("A5", 0x5B46, "rolling_sound", rolling_sound, 0, 452),
	wait_routine("A5", 0x5BD2, "town_music", town_music, 420),
	c_routine("A5", 0x4984, "draw_tile_block", draw_tile_block, 0, 3117),
	c_routine("A5", 0x51EE, "slope_height", slope_height, 0, 612),
	asm_routine("A7", 0x4098, "move_objects_0_3", move_objects_0_3, 3003),
	c_routine("A7", 0x410C, "project_speed", project_speed, 0, 6703),
	c_routine("A7", 0x4F98, "direction_from_object7", direction_from_object7, 0, 762),
	c_routine("A7", 0x4ECC, "hold_object", hold_object, 0, 636),
)

}
