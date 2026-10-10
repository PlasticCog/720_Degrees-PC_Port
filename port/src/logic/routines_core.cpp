// Rewritten routines: arithmetic helpers, video bank selection, sound command queue and
// the playfield tile bank.  RAM addresses are the original program's variables.
#include "logic.h"

namespace logic {
namespace {

using namespace t11rt;

// F:8078 (register interface): select the video RAM window for playfield address R0.
// The video MMU is switched through the slapstic: a read of 0x8000 arms it, a read of
// 0x8038 / 0x803C selects the bank (0x803C for the half of the playfield with bit 13 set).
void select_video_bank()
{
	T.r[0] = (T.r[0] & 0x2000) ? 0x003C : 0x0038;
	ww(0x01D0, T.r[0]);                                // current bank, as seen by the game
	rw(0x8000);
	const uint16_t v = rw(0x8000 + T.r[0]);
	T.psw = (uint8_t)((T.psw & ~(NF | ZF | VF | CF)) | nzw(v));
}

// F:80D8  int mul16(int a, int b): product modulo 2^16
uint16_t mul16(const Args &a)
{
	return (uint16_t)(a.u(0) * a.u(1));
}

// F:80FA  mul32(int a, int b): signed 32-bit product into the long at 0x0204.  R0 is what
// is left of a when the shift-and-add loop stops (0, or a >> n once b is shifted out).
uint16_t mul32(const Args &a)
{
	const int32_t p = (int32_t)a.s(0) * (int32_t)a.s(1);
	poke(0x0204, (uint16_t)p);
	poke(0x0206, (uint16_t)((uint32_t)p >> 16));
	int16_t r0 = a.s(0);
	uint32_t m = (uint32_t)(int32_t)a.s(1);
	for (;;)
	{
		r0 = (int16_t)(r0 >> 1);
		if (r0 == 0) break;
		m <<= 1;
		if (m == 0) break;
	}
	return (uint16_t)r0;
}

// F:8E78  void queue_sound(int command): append to the 8-entry sound command ring at 0x0198
// (write index at 0x01A1).  Nothing is queued while the game is in state 4 (0x018F).
uint16_t queue_sound(const Args &a)
{
	if (peekb(0x018F) == 4) return T.r[0];             // R0 untouched
	const uint8_t i = peekb(0x01A1);
	pokeb((uint16_t)(0x0198 + i), a.b(0));
	pokeb(0x01A1, (uint8_t)((i + 1) & 7));
	return (uint16_t)(i + 1);
}

// F:EEE6  void set_tile_bank(int zone): the playfield's second tile bank becomes zone >> 3.
// 0x01D6 / 0x01D8 hold the terrain table bases for the two banks (table at 0xFED2);
// 0x0236 counts changes so that the screen is redrawn.
uint16_t set_tile_bank(const Args &a)
{
	const int16_t bank = (int16_t)(a.s(0) >> 3);
	poke(0x01CE, (uint16_t)bank);
	const uint16_t base1 = peek((uint16_t)(0xFED2 + bank * 2));
	if (peek(0x01D8) == base1) return T.r[0];          // R0 untouched
	const uint16_t r0 = (uint16_t)(peek(0x01CC) * 2);
	poke(0x01D6, peek((uint16_t)(0xFED2 + r0)));
	poke(0x01D8, base1);
	poke(0x0236, (uint16_t)(peek(0x0236) + 1));
	return r0;
}

}

LOGIC_TABLE(core,
	asm_routine("F", 0x8078, "select_video_bank", select_video_bank, 153),
	c_routine("F", 0x80D8, "mul16", mul16, 0, 836),
	c_routine("F", 0x80FA, "mul32", mul32, 0, 2009),
	c_routine("F", 0x8E78, "queue_sound", queue_sound, 0, 282),
	c_routine("F", 0xEEE6, "set_tile_bank", set_tile_bank, 0, 273),
)

}
