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
	const uint16_t saved_a = peek(0x01D2), saved_b = peek(0x01D4);
	set_bank_b(0x2800);
	call(0x8068, {});                                  // select_video_bank0
	poke(0x0188, 0xE000);
	const uint16_t box = a.u(1);
	uint16_t p = (uint16_t)((peek(box) + 0x2000) & ~1);
	const uint16_t rows = peekb((uint16_t)(box + 2));
	const uint16_t cols = peekb((uint16_t)(box + 3));
	const uint16_t back = (uint16_t)(cols << 1);
	uint16_t r0;
	if (a.u(0))
	{
		for (uint16_t r = rows; r > 0; r--)
		{
			for (uint16_t c = cols; c > 0; c--)
			{
				poke(p, 0);
				p = (uint16_t)(p + 2);
			}
			p = (uint16_t)(p - back + 0x0080);
		}
		if (peekb(0x036C) == 1 && !peekb(0x0F59))
		{
			poke(0x0188, 0xA000);
			call(0xEAD4, {0x0000, 0x5DF6});
			poke(0x0186, 0x3746);
			call(0xBA96, {0x0001, 0x0000, 0x0008, peek(0x0246), peek(0x0244), 0x5DF6});
		}
		r0 = (uint16_t)(peekb(0x036C) >> 1);
		pokeb(0x036C, (uint8_t)r0);
	}
	else
	{
		const uint16_t flags = (uint16_t)(peekb(0x036C) << 1);
		pokeb(0x036C, (uint8_t)flags);
		if (peek(box) & 1)
		{
			pokeb(0x036C, (uint8_t)(peekb(0x036C) | 1));
			call(0xEAD4, {0x0001, 0x5DF6});
		}
		r0 = peek(0x0188);
		auto put = [&](uint16_t v) { poke(p, v); p = (uint16_t)(p + 2); };
		auto line = [&](uint16_t left, uint16_t mid, uint16_t right) {
			put(left);
			for (int16_t n = (int16_t)(cols - 2); n > 0; n--) put(mid);
			put(right);
		};
		line(r0 | 0x0110, r0 | 0x0111, r0 | 0x0112);
		p = (uint16_t)(p - back + 0x0080);
		for (int16_t n = (int16_t)(rows - 2); n > 0; n--)
		{
			line(r0 | 0x0113, r0 | 0x0087, r0 | 0x0114);
			p = (uint16_t)(p - back + 0x0080);
		}
		line(r0 | 0x0115, r0 | 0x0116, r0 | 0x0117);
	}
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
	T.r[6] = (uint16_t)(T.r[6] - 4);
	const uint16_t sp = T.r[6];
	T.r[5] = sext8(peekb(0x0336));
	if (!((int16_t)T.r[5] < 0))
	{
		poke((uint16_t)(sp + 2), peek(0x01D2));
		poke(sp, peek(0x01D4));
		poke(0x036A, (uint16_t)(peek(0x036A) + 1));
		for (T.r[4] = 0; (int16_t)T.r[4] <= (int16_t)T.r[5]; T.r[4]++)
		{
			T.r[0] = (uint16_t)(T.r[4] << 1);
			if (peek((uint16_t)(0x0338 + T.r[0])) != peek(0x036A)) continue;
			T.r[1] = 0x2800;
			set_bank_b(0x2800);
			const uint16_t box = peek((uint16_t)(0x0348 + T.r[0]));
			if ((int16_t)peek(box) >= 0x4000)
				call(0xEAD4, {0x0001, peek(box)});
			else
				call(0x4D30, {0x0001, box});           // message_box(erase)
			T.r[0] = (uint16_t)(sext8(peekb(0x0336)) - 1);
			pokeb(0x0336, (uint8_t)T.r[0]);
			T.r[3] = sext8((uint8_t)T.r[0]);
			if ((int16_t)T.r[3] < 0) continue;
			for (T.r[2] = T.r[4]; (int16_t)T.r[2] <= (int16_t)T.r[3]; T.r[2]++)
			{
				T.r[1] = (uint16_t)(T.r[2] << 1);
				T.r[0] = T.r[1];
				poke((uint16_t)(0x0348 + T.r[1]), peek((uint16_t)(0x034A + T.r[0])));
				poke((uint16_t)(0x0338 + T.r[1]), peek((uint16_t)(0x033A + T.r[0])));
			}
		}
		poke(0x01D2, peek((uint16_t)(sp + 2)));
		poke(0x01D4, peek(sp));
		poke(0x1400, peek(0x01D2));
		poke(0x1402, peek(0x01D4));
	}
	T.r[6] = (uint16_t)(T.r[6] + 4);
	fl::cmp(peek(sp), peek((uint16_t)(sp + 2)));
}

// A7:4C28 (assembly): show the next pending message (0x0362[0x0358]) in a new box: when all
// 8 are in use the oldest is removed first.  Text by F:A9FC; bits 0 of 0x036D / 0x036E ask
// for F:A8FE and the text table 0x5CD2 for this message.  The flags are those of the
// argument word left on the stack.
void show_next_message()
{
	T.r[0] = (uint16_t)(sext8(peekb(0x0358)) << 1);
	const uint16_t msg = peek((uint16_t)(0x0362 + T.r[0]));
	fl::tst(msg);
	if (!msg) return;
	if (peekb(0x0336) == 7)
	{
		T.r[3] = peek(0x01D2);
		T.r[4] = peek(0x01D4);
		T.r[0] = 0x2800;
		set_bank_b(0x2800);
		const uint16_t box = peek(0x0348);
		if ((int16_t)peek(box) >= 0x4000)
			call(0xEAD4, {0x0001, peek(box)});
		else
			call(0x4D30, {0x0001, box});               // message_box(erase)
		for (T.r[2] = 0; (int16_t)T.r[2] <= 6; T.r[2]++)
		{
			T.r[1] = (uint16_t)(T.r[2] << 1);
			T.r[0] = T.r[1];
			poke((uint16_t)(0x0348 + T.r[1]), peek((uint16_t)(0x034A + T.r[0])));
			poke((uint16_t)(0x0338 + T.r[1]), peek((uint16_t)(0x033A + T.r[0])));
		}
		pokeb(0x0336, 6);
		poke(0x01D2, T.r[3]);
		poke(0x01D4, T.r[4]);
		poke(0x1400, peek(0x01D2));
		poke(0x1402, peek(0x01D4));
	}
	if (peekb(0x036D) & 1) call(0xA8FE, {});
	T.r[0] = (uint16_t)(peekb(0x036D) >> 1);
	pokeb(0x036D, (uint8_t)T.r[0]);
	if (peek(0x036E) & 1) poke(0x0184, 0x5CD2);
	T.r[0] = (uint16_t)(peekb(0x036E) >> 1);
	pokeb(0x036E, (uint8_t)T.r[0]);
	T.r[0] = (uint16_t)(sext8(peekb(0x0358)) << 1);
	push(peek((uint16_t)(0x0362 + T.r[0])));
	const uint16_t arg = T.r[6];
	call(0xA9FC, {});
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
	if ((int16_t)T.r[0] < 0) poke(T.r[0], 0);
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
	poke(0x0188, 0xE000);
	if (!a.u(0))
	{
		text(0x0053);
		if (peekb(0x01F9) > 4) text(0x005A);
		poke(0x0186, (uint16_t)(peek(0x0186) - 0x0018));
		call(0xBA96, {0x0000, 0x0001, 0x0002, 0x0000, (uint16_t)(peekb(0x01F9) >> 2)});
		if (peekb(0x01F9) & 3)
		{
			uint16_t r1 = rb((uint16_t)(0x5A49 + (sext8(peekb(0x01FA)) & 3)));
			r1 = (uint16_t)(((sext8(peekb(0x01F9)) & 3) << 1) + r1 - 2);
			const uint16_t at = peek(0x0186);
			poke(at, peek((uint16_t)(0x5A4E + (r1 << 1))) | peek(0x0188));
			poke((uint16_t)(peek(0x0186) + 2), peek((uint16_t)(0x5A50 + (r1 << 1))) | peek(0x0188));
		}
	}
	else if ((peekb(0x01FA) & 0xE0) == 0xE0)
		text(0x0059);
	else
	{
		const uint16_t m = (uint16_t)((peekb(0x01F3) >> 6) & 3);
		text((uint16_t)(m + 0x0054));
		if (m != 1) text(0x0058);
	}
	uint16_t n = peekb(0x01F9);
	if (!a.u(1))
		n = 3;
	else
	{
		const uint16_t price = rb((uint16_t)(0x5A46 + ((peekb(0x01F3) >> 6) & 3)));
		if ((int16_t)n >= (int16_t)(price << 1)) n = 0;
		else if ((int16_t)n >= (int16_t)price) n = 1;
		else n = 2;
	}
	return text((uint16_t)(n + 0x004F));
}

// ---------------------------------------------------------------- game state

// A7:5018: the chaser (object 7).  Its timer 0x0EB4 counts down; while it is on screen it
// keeps the timer negative and is steered with the skater's velocity.  When the timer runs
// out it gets faster (0x0EB2, up to 7; 0x0F22 the delay from 0xFE0D) and F:E13A moves it
// towards the skater; the timer restarts at 5 (on screen) or 16-31 (random).
uint16_t chaser_step(const Args &)
{
	poke(0x043E, 0x0001);
	poke(0x0EB4, (uint16_t)(peek(0x0EB4) - 1));
	uint16_t r1 = 0, r2 = 0;
	const uint16_t sx = peek(0x063E), sy = peek(0x067E);
	bool visible = sx > 0x8200 && sx < 0xF000;
	if (!visible) visible = (int16_t)sy < 0x1000 && sy > 0x9000;
	if (visible)
	{
		r1 = peek(0x04F0);
		r2 = (uint16_t)(peek(0x0570) + peek(0x05B0));
		poke(0x0EB4, 0xFFFF);
	}
	if ((int16_t)peek(0x0EB4) > 0) return 0;
	if ((int16_t)peek(0x0F22) <= 0)
	{
		if ((int8_t)peekb(0x0EB2) <= 6) pokeb(0x0EB2, (uint8_t)(peekb(0x0EB2) + 1));
		pokeb(0x0F22, rb((uint16_t)(0xFE0D + peekb(0x0F2E))));
		pokeb(0x0F23, 0);
	}
	const uint16_t dir = call(0x4F98, {});             // direction_from_object7
	uint16_t r0 = call(0xE13A, {r2, r1, sext8(peekb(0x0EB2)), dir});
	if ((int16_t)peek(0x0EB4) < 0)
		poke(0x0EB4, 0x0005);
	else
	{
		r0 = call(0x9152, {});                         // next_random
		poke(0x0EB4, (uint16_t)((r0 & 0x000F) + 0x0010));
	}
	return r0;
}

// A7:5248: buying (bit 0x10 of 0x0ED7): at the shop A7:5326, if the cash 0x0256 covers the
// price (tables 0xFE7C/0xFE70 by 0x0F31) the item count 0x025C+shop goes up, the price is
// paid (F:F002) and a sound plays; otherwise the "not enough" message.  The request is cleared.
uint16_t shop_purchase(const Args &)
{
	if (!(peekb(0x0ED7) & 0x10)) return T.r[0];
	const uint16_t shop = call(0x5326, {});
	pokeb(0x0ED8, (uint8_t)(peekb(0x0ED8) | rb((uint16_t)(0xFC74 + shop))));
	const uint16_t price = rb((uint16_t)(0xFE70 + rb((uint16_t)(0xFE7C + peekb(0x0F31)))));
	if ((int16_t)peek(0x0256) >= (int16_t)price)
	{
		const uint16_t item = (uint16_t)(0x025C + shop);
		if ((int8_t)peekb(item) < 0) pokeb(item, 0);
		pokeb(item, (uint8_t)(peekb(item) + 1));
		push(0x76C8);
		push(0x00D2);
		push(0x0001);
		call(0xA974, {});
		poke(T.r[6], (uint16_t)-price);
		call(0xF002, {});
		poke(T.r[6], shop);
		push(0x0000);
		call(0xD97C, {});
		T.r[0] = call(0x9152, {}) & 0x0007;
		poke(T.r[6], rb((uint16_t)(0xFE1E + T.r[0])));
		call(0x8E78, {});
		call(0x5602, {});
		call(0xD8D2, {});
	}
	else
	{
		push(0x76D6);
		push(0x00F0);
		push(0x0001);
		call(0xA974, {});
		poke(T.r[6], shop);
		push(0x0001);
		call(0xD97C, {});
		T.r[0] = call(0x9152, {}) & 0x0003;
		poke(T.r[6], rb((uint16_t)(0xFE26 + T.r[0])));
		call(0x8E78, {});
	}
	pokeb(0x0ED7, peekb(0x0ED7) & ~0x10);
	T.r[6] = (uint16_t)(T.r[6] + 8);
	return T.r[0];
}

// A7:43C0  insert_high_score(int first, long score): find the first of the 15 entries from
// first on (8 bytes at 0x0370: high word, low word, ...) below score and insert score there,
// moving the lower entries down.  Returns the place or -1.
uint16_t insert_high_score(const Args &a)
{
	const uint16_t first = a.u(0), lo = a.u(1), hi = a.u(2);
	const uint32_t score = (uint32_t)hi << 16 | lo;
	uint16_t place = 0xFFFF;
	for (uint16_t i = first; (int16_t)i <= (int16_t)(uint16_t)(first + 14); i = (uint16_t)(i + 1))
	{
		const uint16_t e = (uint16_t)(0x0370 + (i << 3));
		const uint32_t v = (uint32_t)peek(e) << 16 | peek((uint16_t)(e + 2));
		if ((int16_t)((v - score) >> 16) < 0)
		{
			place = i;
			break;
		}
	}
	if ((int16_t)place < 0) return place;
	for (uint16_t j = (uint16_t)(first + 14); (int16_t)j >= (int16_t)(uint16_t)(place + 1); j = (uint16_t)(j - 1))
		for (int k = 0; k < 8; k += 2)
			poke((uint16_t)(0x0370 + (j << 3) + k), peek((uint16_t)(0x0368 + (j << 3) + k)));
	poke((uint16_t)(0x0370 + (place << 3)), hi);
	poke((uint16_t)(0x0372 + (place << 3)), lo);
	return place;
}

// A7:5684: the first object from 0x0994 down to 5 whose box (animation 0x03F0: x, y, width,
// height) overlaps the skater's (0x023A at 0x0630/0x0238) and whose shapes touch (A7:572A).
// Returns the object or 0.
uint16_t find_collision(const Args &)
{
	uint16_t saved[6];
	for (int i = 1; i <= 5; i++) saved[i] = T.r[i];
	const uint16_t box = peek(0x023A);
	uint16_t result = 0;
	for (uint16_t r2 = (uint16_t)((sext8(peekb(0x0994)) - 5) << 1); (int16_t)r2 >= 0; r2 = (uint16_t)(r2 - 2))
	{
		const uint16_t p = peek((uint16_t)(0x03FA + r2));
		if (!p) continue;
		uint16_t a1 = (uint16_t)(peek((uint16_t)(0x063A + r2)) + peek(p));
		uint16_t b1 = (uint16_t)(peek(0x0630) + peek(box));
		uint16_t d = (uint16_t)(a1 - b1);
		if ((int16_t)a1 >= (int16_t)b1)
		{
			if (d >= peek((uint16_t)(box + 4))) continue;
		}
		else if ((uint16_t)-d >= peek((uint16_t)(p + 4)))
			continue;
		a1 = (uint16_t)(peek((uint16_t)(0x067A + r2)) + peek((uint16_t)(p + 2)));
		b1 = (uint16_t)(peek(0x0238) + peek((uint16_t)(box + 2)));
		d = (uint16_t)(a1 - b1);
		if ((int16_t)a1 >= (int16_t)b1)
		{
			if (d >= peek((uint16_t)(box + 6))) continue;
		}
		else if ((uint16_t)-d >= peek((uint16_t)(p + 6)))
			continue;
		if (call(0x572A, {d, (uint16_t)(r2 + 10), r2, box}))
		{
			result = (uint16_t)((r2 >> 1) + 5);
			break;
		}
	}
	for (int i = 1; i <= 5; i++) T.r[i] = saved[i];
	return result;
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
	if (!st) return;
	if (peek(0x02FA)) poke(0x02FA, (uint16_t)(peek(0x02FA) - 1));
	if (T.r[1] == 1)
	{
		bool end;
		if ((int16_t)peek(0x02FC) >= 0x0380)
		{
			if (!peek(0x02FA))
				end = true;
			else
			{
				poke(0x02FA, (uint16_t)(peek(0x02FA) - 1));
				end = !peek(0x02FA);
			}
		}
		else
			end = !peek(0x02FA);
		if (end)
		{
			call(0xB602, {0x0002, 0x0002});
			pokeb(0x031F, 0);
			call(0x8E78, {0x0055});
		}
		if (peekb(0x0327) & 7)
		{
			T.r[0] = rb((uint16_t)(0xFCD0 + (peekb(0x0327) >> 3)));
			T.r[2] = peekb(0x031D);
			if (T.r[0] != T.r[2])
			{
				pokeb(0x031D, (uint8_t)T.r[0]);
				pokeb(0x01F8, (uint8_t)T.r[0]);
				call(0x9B82, {});
			}
		}
		else if (!peekb(0x031E))
		{
			const uint16_t v = (uint16_t)(peek(0x01B4) - 0x0014);
			poke(0x01B4, v);
			if (!((int16_t)v > 0))
			{
				poke(0x01B4, 0);
				call(0x8E78, {0x0055});
			}
		}
	}
	if (T.r[1] == 2)
	{
		poke(0x01B4, 0);
		if (!peek(0x02FA))
		{
			pokeb(0x031B, 0x3F);
			pokeb(0x031F, 0);
			T.r[0] = sext8(peekb(0x0327)) & 0x0007;
			if (T.r[0] >= 4)
			{
				pokeb(0x0325, 0x7F);
				T.r[0] = (uint16_t)(peekb(0x0327) >> 3);
				pokeb(0x031D, rb((uint16_t)(0xFCD0 + T.r[0])));
			}
		}
	}
	const uint8_t now = peekb(0x031F);
	fl::tstb(now);
	if (!now) call(0xE238, {});                       // reset_skater_state
}

// A7:4B3E: every 128 frames ask the sound CPU for its status (command 9); a bit of 0-2 that
// stays set from the last answer (0x01A3) marks it as failing (0x01A2 = 1)
uint16_t poll_sound_status(const Args &)
{
	if (peekb(0x01C4) & 0x7F) return T.r[0];
	call(0x8E6A, {});                                  // set_flag_01a4
	call(0x8EA2, {0x0009});                            // send_sound_command
	const uint16_t status = call(0x8ED2, {});          // read_sound_response
	call(0x8F62, {});
	call(0x8E72, {});                                  // clear_flag_01a4
	const uint16_t r0 = peekb(0x01A3) & status;
	if (r0 & 7) pokeb(0x01A2, 1);
	pokeb(0x01A3, (uint8_t)status);
	return r0;
}

}

LOGIC_TABLE(banka,
	c_routine("A5", 0x438C, "step_map_column", step_map_column, 0, 438),
	asm_routine("A5", 0x42BC, "step_map_row", step_map_row, 464),
	asm_routine("A5", 0x41CE, "draw_map_strip", draw_map_strip, 379),
	c_routine("A5", 0x4000, "init_map_view", init_map_view, 0, 2993035),
	asm_routine("A5", 0x48CC, "update_map_slots", update_map_slots, 30363),
	c_routine("A5", 0x5D08, "stop_effect_sound", stop_effect_sound, 0, 311),
	c_routine("A5", 0x5B46, "rolling_sound", rolling_sound, 0, 439),
	wait_routine("A5", 0x5BD2, "town_music", town_music, 420),
	c_routine("A5", 0x4984, "draw_tile_block", draw_tile_block, 0, 3117),
	c_routine("A5", 0x51EE, "slope_height", slope_height, 0, 643),
	asm_routine("A7", 0x4098, "move_objects_0_3", move_objects_0_3, 3003),
	c_routine("A7", 0x410C, "project_speed", project_speed, 0, 6390),
	c_routine("A7", 0x4F98, "direction_from_object7", direction_from_object7, 0, 762),
	c_routine("A7", 0x4ECC, "hold_object", hold_object, 0, 636),
	c_routine("A7", 0x4D30, "message_box", message_box, 0, 10547),
	asm_routine("A7", 0x4B84, "expire_messages", expire_messages, 166),
	asm_routine("A7", 0x4C28, "show_next_message", show_next_message, 160),
	c_routine("A7", 0x5A7E, "credit_messages", credit_messages, 0, 23113),
	c_routine("A7", 0x5018, "chaser_step", chaser_step, 0, 614),
	c_routine("A7", 0x5248, "shop_purchase", shop_purchase, 0, 267),
	c_routine("A7", 0x43C0, "insert_high_score", insert_high_score, 0, 7800),
	c_routine("A7", 0x5684, "find_collision", find_collision, 0, 2730),
	asm_routine("A7", 0x50F8, "fallen_skater", fallen_skater, 99),
	wait_c_routine("A7", 0x4B3E, "poll_sound_status", poll_sound_status, 267),
)

}
