// Rewritten routines: objects, terrain tables and the zone (tile bank) switches.
#include "logic.h"

namespace logic {
namespace {

using namespace t11rt;

uint16_t sext8(uint8_t v) { return (uint16_t)(int16_t)(int8_t)v; }

// A5:5166: objects 1-3 move with object 0: x/y velocities (0x04F0, 0x0570) and the bytes
// 0x07F0/0x07F4 are copied; the z velocity 0x05B0 and byte 0x07F8 are passed along the chain
// 1 -> 2 -> 3, taken from object 0 only when 0x031F == 5
uint16_t copy_object0_velocity(const Args &)
{
	for (int i = 3; i >= 1; i--)
	{
		poke((uint16_t)(0x04F0 + 2 * i), peek(0x04F0));
		pokeb((uint16_t)(0x07F0 + i), peekb(0x07F0));
		poke((uint16_t)(0x0570 + 2 * i), peek(0x0570));
		pokeb((uint16_t)(0x07F4 + i), peekb(0x07F4));
	}
	if (peekb(0x031F) == 5)
	{
		poke(0x05B2, peek(0x05B0));
		pokeb(0x07F9, peekb(0x07F8));
	}
	poke(0x05B4, peek(0x05B2));
	pokeb(0x07FA, peekb(0x07F9));
	poke(0x05B6, peek(0x05B4));
	pokeb(0x07FB, peekb(0x07FA));
	return 2;
}

// F:A2BE  direction_bit(int row): bit (row * 16 + direction 0x031D) of the bitmap at 0x7988
// (masks at 0xFC74); returns the masked byte
uint16_t direction_bit(const Args &a)
{
	const uint16_t i = (uint16_t)((a.u(0) << 4) + sext8(peekb(0x031D)));
	const uint16_t byte = sext8(peekb((uint16_t)(0x7988 + (uint16_t)((int16_t)i >> 3))));
	return byte & peekb((uint16_t)(0xFC74 + (i & 7)));
}

// F:9E52  slope_table_value(int t): signed byte from the 8 x 16 table at 0x7908 (bank B 0x2C00):
// row = bits 3-5 of t, column = the skater direction 0x031D (reversed when the speed 0x01B4
// is negative).  Bank B is restored afterwards.
uint16_t slope_table_value(const Args &a)
{
	const uint16_t saved = peek(0x01D4);
	poke(0x01D4, 0x2C00);
	poke(0x1402, 0x2C00);
	uint16_t dir = sext8(peekb(0x031D));
	if ((int16_t)peek(0x01B4) < 0) dir ^= 8;
	const uint16_t v = sext8(peekb((uint16_t)(0x7908 + ((a.u(0) & 0x38) << 1) + dir)));
	poke(0x01D4, saved);
	poke(0x1402, peek(0x01D4));
	return v;
}

// F:B56E  reset_object(int i): clear object slot i (inactive, no velocity, sprite off screen)
uint16_t reset_object(const Args &a)
{
	const uint16_t i = a.u(0), w = (uint16_t)(i * 2);
	pokeb((uint16_t)(0x0370 + i), 0);
	poke((uint16_t)(0x0430 + w), 0);
	pokeb((uint16_t)(0x0490 + i), peekb((uint16_t)(0x0490 + i)) & ~0x98);
	pokeb((uint16_t)(0x04B0 + i), peekb((uint16_t)(0x04B0 + i)) & ~0x40);
	poke((uint16_t)(0x0630 + w), 0x9000);
	poke((uint16_t)(0x0670 + w), 0);
	poke((uint16_t)(0x05F0 + w), 0x8200);
	poke((uint16_t)(0x06B0 + w), 0);
	pokeb((uint16_t)(0x0770 + i), 0);
	poke((uint16_t)(0x0570 + w), 0);
	poke((uint16_t)(0x04F0 + w), 0);
	poke((uint16_t)(0x05B0 + w), 0);
	poke((uint16_t)(0x03F0 + w), 0);
	return i;
}

// F:B5B8  alloc_object_slot(): first slot from 8 on whose 0x0490 flags (0x86) are clear;
// recorded in 0x0FFC, or, when 0x0196 is set, the slot in 0x0FFC is skipped instead.
// Returns the slot (claimed: flags 0x0490 = 0, 0x04B0 = 4) or -1 when none below 32 is free.
uint16_t alloc_object_slot(const Args &)
{
	uint16_t i = 8;
	if (!peekb(0x0196))
		for (;; i++)
		{
			pokeb(0x0FFC, (uint8_t)i);
			if (!(peekb((uint16_t)(0x0490 + i)) & 0x86)) break;
		}
	else
		for (;; i++)
		{
			if (sext8(peekb(0x0FFC)) == i) continue;
			if (!(peekb((uint16_t)(0x0490 + i)) & 0x86)) break;
		}
	if ((int16_t)i >= 0x20) return 0xFFFF;
	pokeb((uint16_t)(0x0490 + i), 0);
	pokeb((uint16_t)(0x04B0 + i), 4);
	return i;
}

// A7:4F42: if the object queued in 0x0E6A is ready (0x04B0 bit 0x40), start it from the
// template at 0x0E5E-0x0E69 and clear the queue (0x0E6A = 0xFF).  Returns the slot.
uint16_t spawn_queued_object(const Args &)
{
	const uint16_t i = peekb(0x0E6A), w = (uint16_t)(i * 2);
	if (!(peekb((uint16_t)(0x04B0 + i)) & 0x40)) return i;
	pokeb(0x0E68, 0);
	pokeb(0x0E6A, 0xFF);
	poke((uint16_t)(0x0430 + w), peek(0x0E66));
	pokeb((uint16_t)(0x0770 + i), peekb(0x0E69));
	poke((uint16_t)(0x04F0 + w), peek(0x0E5E));
	poke((uint16_t)(0x0570 + w), peek(0x0E60));
	poke((uint16_t)(0x03B0 + w), peek(0x0E62));
	poke((uint16_t)(0x03F0 + w), peek(0x0E64));
	pokeb((uint16_t)(0x0490 + i), peekb((uint16_t)(0x0490 + i)) | 0x18);
	pokeb((uint16_t)(0x04B0 + i), peekb((uint16_t)(0x04B0 + i)) & ~0x40);
	return i;
}

// F:E0C2: place object 7 relative to the scroll position (0x01A8, -0x01AA), on the side
// given by the signs of object 0 velocities.  Returns the y written.
uint16_t place_object7(const Args &)
{
	poke(0x06FE, peek(0x01A8));
	const uint16_t ny = (uint16_t)-peek(0x01AA);
	poke(0x073E, ny);
	poke(0x06FE, (uint16_t)(((int16_t)peek(0x04F0) < 0 ? 0x8E00 : 0xF200) + peek(0x01A8)));
	const uint16_t y = (uint16_t)(((int16_t)peek(0x0570) < 0 ? 0x6E00 : 0x1200) + ny);
	poke(0x073E, y);
	return y;
}

// Zone switches.  The park map is drawn with two tile banks; when the scroll position reaches
// a cell (column 0x0234 = 0..15, row 0x0232 = 0..7) whose zone byte asks for it, the second
// bank changes to that zone (F:EEE6 set_tile_bank).  The low 3 bits of the zone byte select
// a condition; the four routines are the checks for the four edges the scroll crosses.
// Modes 6 and 7 jump outside the original tables and are not used by the game; here they
// never switch.  R0 is returned as the original leaves it.

int16_t zone_col() { return (int16_t)peek(0x0234); }
int16_t zone_row() { return (int16_t)peek(0x0232); }

uint16_t switch_zone(uint16_t zone, uint16_t r0)
{
	T.r[0] = r0;
	return call(0xEEE6, {zone});
}

// A5:43F8
uint16_t zone_switch_a(const Args &a)
{
	const uint16_t z = a.u(0), mode = z & 7;
	uint16_t r0 = (uint16_t)(mode * 2);
	bool sw = false;
	switch (mode)
	{
	case 1: case 5: sw = true; break;
	case 2: sw = zone_col() < 14; break;
	case 3: sw = zone_row() < 7; break;
	case 4: r0 = (uint16_t)((int16_t)(uint16_t)(15 - zone_col()) >> 1); sw = zone_row() < (int16_t)r0; break;
	}
	return sw ? switch_zone(z, r0) : r0;
}

// A5:4442
uint16_t zone_switch_b(const Args &a)
{
	const uint16_t z = a.u(0), mode = z & 7;
	uint16_t r0 = (uint16_t)(mode * 2);
	bool sw = false;
	switch (mode)
	{
	case 1: sw = zone_col() != 1 && zone_col() != 0; break;
	case 2: case 5: sw = true; break;
	case 3: r0 = (uint16_t)(((zone_col() - 1) & 15) >> 1); sw = zone_row() < (int16_t)r0; break;
	case 4: sw = zone_row() < 7; break;
	}
	return sw ? switch_zone(z, r0) : r0;
}

// A5:4496
uint16_t zone_switch_c(const Args &a)
{
	const uint16_t z = a.u(0), mode = z & 7;
	uint16_t r0 = (uint16_t)(mode * 2);
	bool sw = false;
	switch (mode)
	{
	case 1: sw = zone_row() != 1; break;
	case 2: r0 = (uint16_t)(zone_col() >> 1); sw = (uint16_t)((zone_row() - 1) & 7) > r0; break;
	case 3: case 5: sw = true; break;
	case 4: sw = zone_col() < 14; break;
	}
	return sw ? switch_zone(z, r0) : r0;
}

// A5:44EC
uint16_t zone_switch_d(const Args &a)
{
	const uint16_t z = a.u(0), mode = z & 7;
	uint16_t r0 = (uint16_t)(mode * 2);
	bool sw = false;
	switch (mode)
	{
	case 1: r0 = (uint16_t)(((16 - zone_col()) & 15) >> 1); sw = (uint16_t)((zone_row() - 1) & 7) > r0; break;
	case 2: sw = zone_row() != 1; break;
	case 3: sw = zone_col() > 2; break;
	case 4: case 5: sw = true; break;
	}
	return sw ? switch_zone(z, r0) : r0;
}

}

LOGIC_TABLE(world,
	c_routine("A5", 0x5166, "copy_object0_velocity", copy_object0_velocity, 0, 911),
	c_routine("F", 0xA2BE, "direction_bit", direction_bit, 0, 492),
	c_routine("F", 0x9E52, "slope_table_value", slope_table_value, 0, 567),
	c_routine("F", 0xB56E, "reset_object", reset_object, 0, 528),
	c_routine("F", 0xB5B8, "alloc_object_slot", alloc_object_slot, 0, 818),
	c_routine("A7", 0x4F42, "spawn_queued_object", spawn_queued_object, 0, 576),
	c_routine("F", 0xE0C2, "place_object7", place_object7, 0, 387),
	c_routine("A5", 0x43F8, "zone_switch_a", zone_switch_a, 0, 161),
	c_routine("A5", 0x4442, "zone_switch_b", zone_switch_b, 0, 163),
	c_routine("A5", 0x4496, "zone_switch_c", zone_switch_c, 0, 443),
	c_routine("A5", 0x44EC, "zone_switch_d", zone_switch_d, 0, 542),
)

}
