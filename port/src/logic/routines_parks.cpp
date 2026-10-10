// Rewritten routines: parks, tickets, cash and the town position checks.
#include "logic.h"

namespace logic {
namespace {

using namespace t11rt;

struct KeepRegs {
	int n;
	uint16_t r[6];
	explicit KeepRegs(int count) : n(count) { for (int i = 1; i <= n; i++) r[i] = T.r[i]; }
	~KeepRegs() { for (int i = 1; i <= n; i++) T.r[i] = r[i]; }
};

// B3:7F80: clear the bytes 0x0FB4-0x0FD7
uint16_t clear_0fb4(const Args &)
{
	for (int i = 0x24; i >= 1; i--) pokeb((uint16_t)(0x0FB3 + i), 0);
	return 0;
}

// A7:5602: bit i of 0x0ED7 (masks at 0xFC74) is set when the byte 0x025B+i (i = 1..4) is
// positive (A7:55DE does the same against a limit)
uint16_t set_ticket_bits(const Args &)
{
	for (int i = 4; i >= 1; i--)
	{
		const uint8_t m = peekb((uint16_t)(0xFC73 + i));
		if ((int8_t)peekb((uint16_t)(0x025B + i)) > 0)
			pokeb(0x0ED7, peekb(0x0ED7) | m);
		else
			pokeb(0x0ED7, peekb(0x0ED7) & ~m);
	}
	return 0;
}

// F:F002  add_cash(int n): cash 0x0256 += n, kept within 0..9999 (0x0FA2 bit 3: redraw)
uint16_t add_cash(const Args &a)
{
	pokeb(0x0FA2, peekb(0x0FA2) | 0x08);
	poke(0x0256, (uint16_t)(peek(0x0256) + a.u(0)));
	if ((int16_t)peek(0x0256) < 0) poke(0x0256, 0);
	if ((int16_t)peek(0x0256) > 9999) poke(0x0256, 9999);
	pokeb(0x0ED8, 0);
	return T.r[0];
}

// B3:65D6  add_park_counter(int n): in parks 0 and 1 (0x027A) 0x0F20 += 4n, otherwise
// 0x0F1C += n; returns the new value
uint16_t add_park_counter(const Args &a)
{
	uint16_t v;
	if (peekb(0x027A) < 2)
	{
		v = (uint16_t)((a.u(0) << 2) + peek(0x0F20));
		poke(0x0F20, v);
	}
	else
	{
		v = (uint16_t)(peek(0x0F1C) + a.u(0));
		poke(0x0F1C, v);
	}
	return v;
}

// A7:5360: park (0-3) whose gate is at the town position 0x0996 / 0x099A
uint16_t park_at_gate(const Args &)
{
	const bool east = (int16_t)peek(0x0996) > 0x00B0;
	if ((int16_t)peek(0x099A) > 4) return east ? 0 : 3;
	return east ? 2 : 1;
}

// A7:5326: park (0-3) whose area contains the town position 0x0996 / 0x099A
uint16_t park_at_area(const Args &)
{
	const int16_t x = (int16_t)peek(0x0996), y = (int16_t)peek(0x099A);
	if (x < 0x0050) return 0;
	if (x > 0x0100) return 3;
	if (y > 6) return 2;
	if (y >= 0) return 1;
	return x > 0x00D0 ? 2 : 1;
}

// F:CDD0  find_slot_with(int except, int value): highest slot i <= 0x09AE, other than except,
// whose word 0x0AF8[i] equals value; -1 if none
uint16_t find_slot_with(const Args &a)
{
	for (int16_t i = (int16_t)peek(0x09AE); i >= 0; i--)
		if (peek((uint16_t)(0x0AF8 + 2 * i)) == a.u(1) && (uint16_t)i != a.u(0)) return (uint16_t)i;
	return 0xFFFF;
}

// F:E106: when 0x0377 is set, F:CF5E(0x0397); the banks are restored afterwards
uint16_t call_cf5e_keep_banks(const Args &)
{
	KeepRegs keep(2);
	const uint16_t a = peek(0x01D2), b = peek(0x01D4);
	uint16_t r0 = T.r[0];
	if (peekb(0x0377)) r0 = call(0xCF5E, {peekb(0x0397)});
	poke(0x01D2, a);
	poke(0x01D4, b);
	poke(0x1400, peek(0x01D2));
	poke(0x1402, peek(0x01D4));
	return r0;
}

// A7:50C8: when 0x0377 is set, F:E13A(0, 0, 2, A7:4F98() ^ 8) and 0x0EB4 = 250
uint16_t a7_50c8(const Args &)
{
	KeepRegs keep(1);
	if (!peekb(0x0377)) return T.r[0];
	const uint16_t d = (uint16_t)(call(0x4F98, {}) ^ 8);
	const uint16_t r0 = call(0xE13A, {0, 0, 2, d});
	poke(0x0EB4, 0x00FA);
	return r0;
}


// F:DA02  open_park_gate(int park): sound 0x62; the gate object of the park (slot whose
// 0x0AF8 word is the park's entry at 0x70E8 in bank B 0x0400, with bit 15) starts its
// animation (0x0430 = 1); the park is marked open in 0x0EA4.  Banks are restored.
uint16_t open_park_gate(const Args &a)
{
	KeepRegs keep(3);
	const uint16_t bank_a = peek(0x01D2), bank_b = peek(0x01D4);
	poke(0x01D2, 0);
	poke(0x1400, 0);
	poke(0x01D4, 0x0400);
	poke(0x1402, 0x0400);
	T.r[0] = 0x0400;
	call(0x8E78, {0x0062});                            // queue_sound
	const uint16_t park = a.u(0);
	const uint16_t id = (uint16_t)(peek((uint16_t)(0x70E8 + park * 2)) | 0x8000);
	uint16_t r0 = call(0xCDD0, {0xFFFF, id});          // find_slot_with
	if ((int16_t)r0 >= 0)
	{
		r0 = (uint16_t)((uint16_t)(int16_t)(int8_t)peekb((uint16_t)(0x09B8 + r0)) & 0x3F);
		r0 = (uint16_t)(r0 * 2);
		poke((uint16_t)(0x0430 + r0), 0x0001);
	}
	pokeb(0x0EA4, peekb(0x0EA4) | peekb((uint16_t)(0xFC74 + park)));
	poke(0x01D2, bank_a);
	poke(0x01D4, bank_b);
	poke(0x1400, peek(0x01D2));
	poke(0x1402, peek(0x01D4));
	return r0;
}

// B3:6E38: medal level of the value 0x0258 against the thresholds 0x0FDA/0x0FDC/0x0FDE
// (3 -> 0xC0, 2 -> 0x80, 1 -> 0x40, none -> 0) into the byte 0x0262 + 0x0F31; bank A 0x3800.
// Returns 0x0F31 when a level was reached, else 0x3800.
uint16_t set_medal_level(const Args &)
{
	const int16_t v = (int16_t)peek(0x0258);
	if (v > 0)
		for (int i = 3; i >= 1; i--)
			if (v >= (int16_t)peek((uint16_t)(0x0FD8 + 2 * i)))
			{
				const uint16_t slot = peekb(0x0F31);
				pokeb((uint16_t)(0x0262 + slot), (uint8_t)(i << 6));
				poke(0x01D2, 0x3800);
				poke(0x1400, 0x3800);
				return slot;
			}
	pokeb((uint16_t)(0x0262 + peekb(0x0F31)), 0);
	poke(0x01D2, 0x3800);
	poke(0x1400, 0x3800);
	return 0x3800;
}


// F:D97C  set_gate_animation(int closing, int gate): the gate object (slot whose 0x0AF8 word is
// the gate's entry at 0x70AC in bank B 0x0400, with bit 15) gets animation 0x03B0 from the
// table 0x5B8C (or 0x5BAC when closing), one of two variants chosen at random.  Banks are
// restored.
uint16_t set_gate_animation(const Args &a)
{
	KeepRegs keep(3);
	const uint16_t bank_a = peek(0x01D2), bank_b = peek(0x01D4);
	poke(0x01D2, 0);
	poke(0x1400, 0);
	T.r[0] = 0x0400;
	poke(0x01D4, 0x0400);
	poke(0x1402, 0x0400);
	const uint16_t gate = a.u(1);
	const uint16_t id = (uint16_t)(peek((uint16_t)(0x70AC + gate * 2)) | 0x8000);
	uint16_t r0 = call(0xCDFA, {0xFFFF, id});          // find_other_slot
	if ((int16_t)r0 >= 0)
	{
		const uint16_t obj = (uint16_t)((uint16_t)(int16_t)(int8_t)peekb((uint16_t)(0x09B8 + r0)) & 0x3F);
		r0 = (call(0x9152, {}) & 1) ? 8 : 0;           // next_random
		r0 = (uint16_t)((r0 + gate) * 2);
		const uint16_t table = a.u(0) == 0 ? 0x5B8C : 0x5BAC;
		poke((uint16_t)(0x03B0 + obj * 2), peek((uint16_t)(table + r0)));
	}
	poke(0x01D2, bank_a);
	poke(0x01D4, bank_b);
	poke(0x1400, peek(0x01D2));
	poke(0x1402, peek(0x01D4));
	return r0;
}

}

LOGIC_TABLE(parks,
	c_routine("B3", 0x7F80, "clear_0fb4", clear_0fb4, 0, 1767),
	c_routine("A7", 0x5602, "set_ticket_bits", set_ticket_bits, 0, 447),
	c_routine("F", 0xF002, "add_cash", add_cash, 0, 177),
	c_routine("B3", 0x65D6, "add_park_counter", add_park_counter, 0, 153),
	c_routine("A7", 0x5360, "park_at_gate", park_at_gate, 0, 121),
	c_routine("A7", 0x5326, "park_at_area", park_at_area, 0, 201),
	c_routine("F", 0xCDD0, "find_slot_with", find_slot_with, 0, 2769),
	c_routine("F", 0xE106, "call_cf5e_keep_banks", call_cf5e_keep_banks, 0, 441),
	c_routine("A7", 0x50C8, "a7_50c8", a7_50c8, 0, 99),
	c_routine("F", 0xDA02, "open_park_gate", open_park_gate, 0, 897),
	c_routine("B3", 0x6E38, "set_medal_level", set_medal_level, 0, 548),
	c_routine("F", 0xD97C, "set_gate_animation", set_gate_animation, 0, 786),
)

}
