// Rewritten routines of the fixed ROM F:D000-FFFF: object slots and their spawning, the
// object scripts, palette fades, skater animation tables, score and the start buttons.
#include "logic.h"
#include <exception>

namespace logic {
namespace {

using namespace t11rt;

uint16_t sext8(uint8_t v) { return (uint16_t)(int16_t)(int8_t)v; }

// Cycles: every routine charges the original's exact cost on the path it takes (per
// straight-line part, see tools/insn_cycles.py); loops charge each pass (charge(), at the
// pass's address); the C routines' register-save helper charges its prologue and epilogue.
// Nothing is left to the table charge.
// Every charge is also a point where interrupts may come in (at the address of the routine
// running, kept in g_pc), so an interrupt falls between the same parts of the routine as in
// the original (before the part it falls in) instead of after the whole routine.
uint16_t g_pc = 0;
struct At {
	uint16_t saved;
	explicit At(uint16_t pc) : saved(g_pc) { g_pc = pc; }
	~At() { g_pc = saved; }
	At(const At &) = delete;
	At &operator=(const At &) = delete;
};
// Charging n cycles of straight-line code at pc.  The board runs in slices of 50 T-11 cycles
// and the original lets it run (and takes interrupts) after the instruction that ends a slice,
// so a part that ends a slice charges up to the slice's end, lets the board run there and
// then charges the rest (again for every further slice it spans): interrupts and the sound
// CPU then see the routine's time as in the original (to within an instruction), not the
// time after the whole part.  An interrupt made pending by the last part is taken first.
void charge(uint16_t pc, int n)
{
	if (g_atomic)                                      // difference tester: charge, count the step
	{
		tick(pc, n);
		return;
	}
	if (T.check_irqs) tick(pc, 0);
	while (T.icount <= n)
	{
		const int k = T.icount > 0 ? T.icount : 0;
		n -= k;
		tick(pc, k);
	}
	T.icount -= n;
}
void cy(int n) { charge(g_pc, n); }
// An instruction that reads or writes the board (I/O registers): charged after its access, as
// the original's instruction is, so that the board sees the access in the same time slice (and
// an interrupt it makes pending is taken right after it).
template <class F> void io(int n, F access)
{
	access();
	tick(g_pc, n);
}

// The C routines start with a register-save helper (JSR R1,@#F138 / F144 / F152 / F162): it
// pushes R1, R2..R(n+1) and its return address (F142 / F150 / F160 / F172), runs the body and
// restores the registers.  The frame is reproduced because callees can see the stack pointer
// (F:CE24 records the address of an argument in 0x09B2); callees with a register interface
// are given the registers the original holds, so they are put back here too (not when an
// interrupt handler returned elsewhere and the routine is abandoned: Unwound).
// the helpers' epilogues after the body's RTS (BR / MOV (SP)+ / RTS)
constexpr int OUT138 = 12 + 18 + 18 + 21, OUT144 = 12 + 18 * 3 + 21, OUT152 = 12 + 18 * 4 + 21, OUT162 = 18 * 5 + 21;
struct Helper {
	uint16_t r[6], sp;
	int unwinding, out;
	explicit Helper(int n)                             // n registers from R2 on (1..4)
	{
		static const int entry[] = {30 + 105, 30 + 129, 30 + 153, 30 + 177};
		static const int exit[] = {OUT138, OUT144, OUT152, OUT162};
		cy(entry[n - 1]);                              // JSR R1,@#helper and its prologue
		out = exit[n - 1];
		unwinding = std::uncaught_exceptions();
		static const uint16_t ret[] = {0xF142, 0xF150, 0xF160, 0xF172};
		for (int i = 1; i < 6; i++) r[i] = T.r[i];
		sp = T.r[6];
		push(T.r[1]);
		for (int i = 2; i < 2 + n; i++) push(T.r[i]);
		push(ret[n - 1]);
	}
	// the epilogue is charged here, at the end, where interrupts may come in as in the original
	~Helper() noexcept(false)
	{
		if (std::uncaught_exceptions() > unwinding) return;
		for (int i = 1; i < 6; i++) T.r[i] = r[i];
		T.r[6] = sp;
		cy(out);
	}
	Helper(const Helper &) = delete;
	Helper &operator=(const Helper &) = delete;
};
enum { F138 = 1, F144 = 2, F152 = 3, F162 = 4 };

// select both banks (shadows 0x01D2/0x01D4 and the bank registers)
void set_banks(uint16_t a, uint16_t b)
{
	poke(0x01D2, a); ww(0x1400, a);
	poke(0x01D4, b); ww(0x1402, b);
}

// put back banks saved earlier: shadows first, then the registers from the shadows
void restore_banks(uint16_t a, uint16_t b)
{
	poke(0x01D2, a);
	poke(0x01D4, b);
	ww(0x1400, peek(0x01D2));
	ww(0x1402, peek(0x01D4));
}

// F:D0F4: with banks 0x1000/0x1400, F:C3E6 runs for each slot 0x09AA = 0xAE..0 whose word in
// 0x0AF8 is negative; then object 4 is reset (F:B56E) if it is active.  Banks restored.
uint16_t update_marked_slots(const Args &)
{
	At here(0xD0F4);
	Helper h(F138);
	cy(225);                                           // F:D0F8-D11A
	const uint16_t a = peek(0x01D2), b = peek(0x01D4);
	T.r[1] = a; T.r[2] = b;
	set_banks(0x1000, 0x1400);
	poke(0x09AA, 0x00AE);
	uint16_t r0;
	for (;;)
	{
		r0 = peek(0x09AA);
		if ((int16_t)r0 < 0) break;
		r0 = (uint16_t)(r0 << 1);
		if ((int16_t)peek((uint16_t)(0x0AF8 + r0)) < 0)
		{
			T.r[0] = r0;
			cy(30);                                    // JSR
			call_from(0xD130, 0xC3E6, {});
		}
		poke(0x09AA, (uint16_t)(peek(0x09AA) - 1));
		charge(0xD11E, 36 + 51 + 39);                    // pass F:D11E-D134
	}
	cy(36 + 36);                                       // last test, F:D136-D13A
	if (peekb(0x0374))
	{
		T.r[0] = r0;
		cy(60);
		r0 = call_from(0xD144, 0xB56E, {0x0004});
		cy(18);
	}
	cy(153);                                           // F:D146-D15A
	restore_banks(a, b);
	return r0;
}

// F:D15C: F:CF5E(slot) for the slots 0xFE down to 0xAF whose type byte 0x09B8 is set
uint16_t update_high_slots(const Args &)
{
	At here(0xD15C);
	Helper h(F144);
	cy(90 + 30);                                       // F:D160-D16E, first test
	const uint16_t a = peek(0x01D2), b = peek(0x01D4);
	T.r[2] = a; T.r[3] = b;
	uint16_t r0 = 0x00FE;
	for (uint16_t i = 0x00FE; (int16_t)i >= 0x00AF; i--)
	{
		if (peekb((uint16_t)(0x09B8 + i)))
		{
			T.r[0] = r0; T.r[1] = i;
			cy(54);                                    // push, JSR
			r0 = call_from(0xD17C, 0xCF5E, {i});
			cy(18);
		}
		charge(0xD170, 39 + 12 + 30);                    // pass F:D170-D184
	}
	cy(153);
	restore_banks(a, b);
	return r0;
}

// F:D880: the eight entries of the table at 0x70C4 (bank B 0x0400) get a free slot each
// (F:CDFA find_other_slot(-1, value)) whose type 0x09B8 comes from 0x70D4, or from 0x70DC
// when the park of the entry (index / 2) is open (F:E6FE).  Returns the last slot.
uint16_t set_park_entry_types(const Args &)
{
	At here(0xD880);
	Helper h(F144);
	cy(30);
	const uint16_t open = call_from(0xD888, 0xE6FE, {});
	cy(156);
	T.r[1] = open;
	set_banks(0x0000, 0x0400);
	uint16_t r0 = 0;
	for (int16_t i = 7; i >= 0; i--)
	{
		T.r[3] = (uint16_t)i;
		T.r[2] = (uint16_t)(i << 1);
		cy(123);
		r0 = call_from(0xD8B2, 0xCDFA, {0xFFFF, peek((uint16_t)(0x70C4 + 2 * i))});
		const uint16_t dst = (uint16_t)(0x09B8 + r0);
		int c = 63 + 48;
		if (!(peekb((uint16_t)(0xFC74 + (i >> 1))) & (uint8_t)open))
		{
			c += 57;
			pokeb(dst, peekb((uint16_t)(0x70D4 + i)));
		}
		else
		{
			c += 45;
			pokeb(dst, peekb((uint16_t)(0x70DC + i)));
		}
		charge(0xD8A2, c);
	}
	cy(21);
	return r0;
}

// F:D8D2: the four entries of the table at 0x70AA (bank B 0x0400) get a slot (F:CDFA) whose
// type comes from 0x70BB, or 0x70BF when the ticket bit (0x0ED7) of the entry is set.  When
// no slot is found, the object of the slot found for value | 0x8000 is set up instead.
uint16_t set_ticket_entry_types(const Args &)
{
	At here(0xD8D2);
	Helper h(F162);
	cy(192);
	const uint16_t a = peek(0x01D2), b = peek(0x01D4);
	T.r[4] = a; T.r[5] = b;
	set_banks(0x0000, 0x0400);
	uint16_t r0 = 0;
	for (uint16_t i = 4; i >= 1; i--)
	{
		const uint16_t w = (uint16_t)(i << 1);
		T.r[1] = i; T.r[2] = w;
		push(peek((uint16_t)(0x70AA + w)));
		push(0xFFFF);
		cy(123);
		r0 = call_from(0xD906, 0xCDFA, {});
		int c = 24;
		const bool ticket = (peekb(0x0ED7) & peekb((uint16_t)(0xFC73 + i))) != 0;
		if ((int16_t)r0 >= 0)
		{
			c += 51 + 57;
			pokeb((uint16_t)(0x09B8 + r0), peekb((uint16_t)((ticket ? 0x70BF : 0x70BB) + i)));
		}
		else
		{
			poke(T.r[6], (uint16_t)(peek((uint16_t)(0x70AA + w)) | 0x8000));
			push(0xFFFF);
			cy(c + 123);
			c = 0;
			r0 = call_from(0xD932, 0xCDFA, {});
			c += 96;
			r0 = sext8(peekb((uint16_t)(0x09B8 + r0))) & 0x003F;
			if (!(peekb(0x0ED7) & peekb((uint16_t)(0xFC73 + i))))
			{
				c += 126;
				pokeb((uint16_t)(0x0370 + r0), peekb((uint16_t)(0x70BB + i)));
				poke((uint16_t)(0x03B0 + (uint16_t)(r0 << 1)), peek((uint16_t)(0x5B82 + w)));
			}
			else
			{
				c += 45;
				pokeb((uint16_t)(0x0370 + r0), peekb((uint16_t)(0x70BF + i)));
			}
			c += 54;
			pokeb((uint16_t)(0x0490 + r0), (uint8_t)(peekb((uint16_t)(0x0490 + r0)) | 0x18));
			T.r[6] = (uint16_t)(T.r[6] + 2);
		}
		T.r[6] = (uint16_t)(T.r[6] + 4);
		charge(0xD8F6, c + 42);
	}
	cy(153);
	restore_banks(a, b);
	return r0;
}

// F:D47C  start_object(int i): object i gets animation 0x6046[type] (type = 0x0370[i], bank B
// 0x0400), timers 0x0770/0x0790 = 8, flags 0x18 and its start sound (0x6567[type] through
// the table at 0xFDED); then F:BF18 runs for its slot (0x0390[i] into 0x09AA)
uint16_t start_object(const Args &a)
{
	At here(0xD47C);
	Helper h(F144);
	cy(462);
	set_banks(0x0000, 0x0400);
	const uint16_t i = a.u(0);
	pokeb((uint16_t)(0x0790 + i), 8);
	pokeb((uint16_t)(0x0770 + i), 8);
	const uint16_t type = peekb((uint16_t)(0x0370 + i));
	poke((uint16_t)(0x03B0 + (uint16_t)(i << 1)), peek((uint16_t)(0x6046 + (uint16_t)(type << 1))));
	pokeb((uint16_t)(0x0490 + i), (uint8_t)(peekb((uint16_t)(0x0490 + i)) | 0x18));
	const uint16_t snd = peekb((uint16_t)(0x6567 + type));
	T.r[0] = snd; T.r[1] = i; T.r[2] = type; T.r[3] = (uint16_t)(i << 1);
	if (snd)
	{
		cy(90);
		call_from(0xD4D2, 0x8E78, {peekb((uint16_t)(0xFDED + snd))});
		cy(18);
	}
	pokeb(0x09AA, peekb((uint16_t)(0x0390 + i)));
	pokeb(0x09AB, 0);
	cy(99);
	const uint16_t r0 = call_from(0xD4E2, 0xBF18, {});
	cy(21);
	return r0;
}

// F:DD5C: colour 0x1092 of the text palette.  On level 0 it comes from ROM (0x5ED2, bank
// 0x2000); on the other levels it is one of four colours by where the value 0x0F20 (0x0F34
// from 0x027A = 2 on) lies against the limits 0x0FDA / 0x0FDC / 0x0FDE (0: the lowest);
// nothing changes from 0x0F26 = 6 on.
uint16_t set_colour_1092(const Args &)
{
	At here(0xDD5C);
	Helper h(F138);
	cy(36);
	if (peekb(0x0F59))
	{
		cy(42);
		if (peekb(0x0F26) >= 6)
		{
			cy(21);
			return T.r[0];
		}
		cy(42);
		uint16_t v;
		if (peekb(0x027A) >= 2)
		{
			cy(24);
			v = peek(0x0F34);
		}
		else
		{
			cy(36);
			v = peek(0x0F20);
		}
		cy(12);
		uint16_t c;
		if (v == 0)
		{
			cy(18);
			c = 0x800F;
		}
		else if (cy(36), (int16_t)v >= (int16_t)peek(0x0FDE))
		{
			cy(30);
			c = 0xFF19;
		}
		else if (cy(36), (int16_t)v >= (int16_t)peek(0x0FDC))
		{
			cy(30);
			c = 0xEEFA;
		}
		else if (cy(36), (int16_t)v >= (int16_t)peek(0x0FDA))
		{
			cy(30);
			c = 0xC73A;
		}
		else
		{
			cy(18);
			c = 0x800F;
		}
		io(27, [&] { poke(0x1092, c); });
		cy(21);
		return c;
	}
	cy(192);                                           // F:DDB0-DDCC
	const uint16_t a = peek(0x01D2), b = peek(0x01D4);
	set_banks(0x2000, 0x2400);
	io(39, [] { poke(0x1092, peek(0x5ED2)); });
	cy(132 + 21);                                      // F:DDD6-DDEA
	restore_banks(a, b);
	return 0x2400;
}

// Object scripts.  Four byte streams (pointers 0x0EC2, counters 0x0ECA) drive the words listed
// at 0xFE06: x 0x0224 and y 0x0226 (channels 0 and 1: each frame += current byte * 8) and
// 0x03BA / 0x03BC (channels 2 and 3: on each new entry the word is set from the two bytes
// after the count, or the first adds twice its value when the count is non-zero).  An entry
// is a count byte (the time until the next entry); 0xFF n jumps back n bytes and loads the
// target word from the stream (channel 0 also adds the lane offset 0xFDB2[F:DDFE]).

// F:DE28: run the scripts for one frame; objects 5 and 6 follow (x 0x06FA/0x06FC,
// y 0x073A/0x073C)
uint16_t run_object_scripts(const Args &)
{
	At here(0xDE28);
	Helper h(F162);
	cy(255);
	T.r[6] = (uint16_t)(T.r[6] - 2);                   // TST -(SP): local
	set_banks(0x2400, 0x2800);
	poke(0x0224, peek(0x06FA));
	poke(0x0226, peek(0x073C));
	auto step = [](uint16_t ch) {
		cy(129);                                       // F:DEEE-DEFC
		const uint16_t w = (uint16_t)(ch << 1);
		const uint16_t d = (uint16_t)(sext8(peekb(peek((uint16_t)(0x0EC2 + w)))) << 3);
		const uint16_t target = peek((uint16_t)(0xFE06 + w));
		poke(target, (uint16_t)(peek(target) + d));
	};
	for (uint16_t ch = 0; ch <= 3; ch++)
	{
		cy(138);                                       // F:DE54-DE64
		const uint16_t count = (uint16_t)(0x0ECA + ch);
		pokeb(count, (uint8_t)(peekb(count) - 1));
		if (peekb(count))
		{
			cy(30);                                    // F:DEE8-DEEC
			if (ch <= 1) step(ch);
			cy(87 + 42);                               // F:DF00-DF08, F:DF0A-DF10
			continue;
		}
		do
		{
			cy(171);                                   // F:DE66-DE7E
			const uint16_t w = (uint16_t)(ch << 1);
			const uint16_t p = (uint16_t)(0x0EC2 + w);
			poke(p, (uint16_t)(peek(p) + 1));
			uint16_t r1 = peekb(peek(p));
			poke(p, (uint16_t)(peek(p) + 1));
			if (r1 == 0x00FF)
			{
				cy(174);
				poke(p, (uint16_t)(peek(p) - peekb(peek(p))));
				poke(peek((uint16_t)(0xFE06 + w)), peek(peek(p)));
				poke(p, (uint16_t)(peek(p) + 2));
				if (ch == 0)
				{
					cy(30);
					const uint16_t lane = call_from(0xDE9A, 0xDDFE, {});
					cy(66);
					poke(0x0224, (uint16_t)(peek(0x0224) + peek((uint16_t)(0xFDB2 + (uint16_t)(lane << 1)))));
				}
				else
				{
					cy(30);
					if (ch == 2)
					{
						cy(33);
						poke(0x03BA, (uint16_t)(peek(0x03BA) + 0x00A2));
					}
				}
				cy(66);
				r1 = peekb(peek(p));
				poke(p, (uint16_t)(peek(p) + 1));
			}
			cy(51);                                    // F:DEB8-DEBE
			pokeb(count, (uint8_t)r1);
			if (ch <= 1)
				step(ch);
			else
			{
				cy(90);                                // F:DEC0-DECA
				const uint16_t v = sext8(peekb(peek(p)));
				const uint16_t target = peek((uint16_t)(0xFE06 + w));
				if (peekb(count))
				{
					cy(33);
					poke(target, (uint16_t)(peek(target) + (uint16_t)(v << 1)));
				}
				else
				{
					cy(123);
					pokeb(target, (uint8_t)v);
					poke(p, (uint16_t)(peek(p) + 1));
					pokeb((uint16_t)(target + 1), peekb(peek(p)));
					if (ch == 2)
					{
						cy(45);
						poke(0x03BA, (uint16_t)(peek(0x03BA) + 0x00A2));
					}
				}
				cy(30);                                // F:DEE8-DEEC
			}
			cy(87);                                    // F:DF00-DF08
		} while (!peekb(count));
		cy(42);                                        // F:DF0A-DF10
	}
	cy(294);
	const uint16_t x = peek(0x0224);
	poke(0x06FA, x);
	poke(0x06FC, x);
	const uint16_t y = peek(0x0226);
	poke(0x073A, y);
	poke(0x073C, y);
	pokeb(0x0495, (uint8_t)(peekb(0x0495) | 0x18));
	pokeb(0x0496, (uint8_t)(peekb(0x0496) | 0x18));
	poke(0x043A, 0x0001);
	return y;
}

// F:DF40  start_object_scripts(int *y, int *x): pick the script set of the current slot
// (F:BE10; 0x09B6 indexes the bytes at 0x4000, bank 0x1000) and the park (0x0F2E -> 0xFDE3),
// start the four streams, place objects 5 and 6 and return their position through x and y;
// objects 5/6 are set up and F:B4B6(6) runs
uint16_t start_object_scripts(const Args &a)
{
	At here(0xDF40);
	Helper h(F144);
	cy(174);
	set_banks(0x1000, 0x1400);
	call_from(0xDF60, 0xBE10, {});
	cy(435 + 4 * 237 + 63);
	const uint16_t set = sext8(peekb((uint16_t)(0x4000 + peek(0x09B6)))) & 0x000F;
	set_banks(0x2400, 0x2800);
	const uint16_t base = peek((uint16_t)(0x4652 + (uint16_t)(set << 1)));
	const uint16_t park = (uint16_t)((sext8(peekb((uint16_t)(0xFDE3 + peekb(0x0F2E)))) & 0x00FF) << 3);
	uint16_t src = (uint16_t)(park + base - 8);
	for (uint16_t ch = 0; ch <= 3; ch++)
	{
		const uint16_t w = (uint16_t)(ch << 1);
		const uint16_t p = (uint16_t)(0x0EC2 + w);
		poke(p, peek(src));
		src = (uint16_t)(src + 2);
		poke(peek((uint16_t)(0xFE06 + w)), peek(peek(p)));
		poke(p, (uint16_t)(peek(p) + 1));
		pokeb((uint16_t)(0x0ECA + ch), 1);
	}
	poke(0x03BA, (uint16_t)(peek(0x03BA) + 0x00A2));
	const uint16_t lane = call_from(0xDFD6, 0xDDFE, {});
	cy(546);
	const uint16_t x = (uint16_t)(peek(0x0224) + peek((uint16_t)(0xFDB2 + (uint16_t)(lane << 1))));
	poke(0x06FA, x);
	poke(0x06FC, x);
	const uint16_t y = peek(0x0226);
	poke(0x073A, y);
	poke(0x073C, y);
	poke(a.u(1), x);
	poke(a.u(0), y);
	pokeb(0x0475, 0xF0);
	pokeb(0x0476, 0xF0);
	poke(0x03FA, 0);
	poke(0x03FC, 0x7114);
	pokeb(0x0376, 2);
	poke(0x043C, 0);
	poke(0x043A, 0x0001);
	T.r[0] = y; T.r[1] = x;
	const uint16_t r0 = call_from(0xE02A, 0xB4B6, {0x0006});
	cy(39);
	return r0;
}

// F:E036: in state 1 (0x018F) with 0x031F and 0x0377 clear, 0x0EB4 counts down; at 0
// F:CE24(0, 0, 4) and F:C4E4 run, object 7 is set up, 0x0F22 = 250, the counter 0x0EB3 steps
// (0..5) and A:5018 (bank 0x3800) runs with the same arguments
uint16_t countdown_0eb4(const Args &)
{
	At here(0xE036);
	cy(42);
	if (peekb(0x018F) != 1) return cy(21), T.r[0];
	cy(36);
	if (peekb(0x031F)) return cy(21), T.r[0];
	cy(36);
	if (peekb(0x0377)) return cy(21), T.r[0];
	cy(39);
	const uint16_t t = (uint16_t)(peek(0x0EB4) - 1);
	poke(0x0EB4, t);
	if (t) return cy(21), T.r[0];
	cy(108);
	push(0x0004);
	push(0x0000);
	push(0x0000);
	call_from(0xE05C, 0xCE24, {});
	cy(30);
	call_from(0xE060, 0xC4E4, {});
	cy(348);
	pokeb(0x0477, (uint8_t)(peekb(0x0477) | 0xC0));
	const uint8_t v = (uint8_t)(peekb(0x0777) & 0x7F);
	pokeb(0x0777, 0);
	pokeb(0x0777, v);
	pokeb(0x07D7, 0xFF);
	pokeb(0x0EB2, 1);
	poke(0x0EB4, 0);
	poke(0x0F22, 0x00FA);
	const uint8_t n = (uint8_t)(peekb(0x0EB3) + 1);
	pokeb(0x0EB3, n);
	if ((int8_t)n > 5)
	{
		cy(27);
		pokeb(0x0EB3, 0);
	}
	cy(174);
	set_banks(0x3800, 0x3C00);
	T.r[0] = 0x3C00;
	const uint16_t r0 = call_from(0xE0BC, 0x5018, {});
	cy(18 + 21);
	T.r[6] = (uint16_t)(T.r[6] + 6);
	return r0;
}

// F:E13A  set_object7_motion(int vx0, int vy0, int col, int row): object 7 velocity (0x04FE,
// 0x057E) from the 8-column tables at 0x4138 / 0x4238 (bank A 0x2400), doubled, plus vx0 /
// vy0; a new row (0x07D7) also picks its animation 0x03BE (0x5238, or 0x527E by 0xFE18
// [0x0EB3] for col >= 4, bank A 0) and the bytes 0x0777 / 0x0797 = 4.  Banks end as
// 0x3800 / 0x3C00.
uint16_t set_object7_motion(const Args &a)
{
	At here(0xE13A);
	Helper h(F138);
	cy(432);
	poke(0x01D2, 0x2400);
	ww(0x1400, 0x2400);
	const uint16_t row = a.u(3);
	const uint16_t idx = (uint16_t)((uint16_t)((row << 3) + a.u(2)) << 1);
	poke(0x04FE, (uint16_t)((uint16_t)(peek((uint16_t)(0x4138 + idx)) << 1) + a.u(1)));
	poke(0x057E, (uint16_t)((uint16_t)(peek((uint16_t)(0x4238 + idx)) << 1) + a.u(0)));
	if (peekb(0x07D7) != row)
	{
		cy(171);
		set_banks(0x0000, 0x0400);
		uint16_t r1 = 0;
		if ((int16_t)a.u(2) >= 4)
		{
			cy(63);
			r1 = peekb((uint16_t)(0xFE18 + sext8(peekb(0x0EB3))));
		}
		else
			cy(24);
		cy(30);
		if ((int16_t)r1 < 5)
		{
			cy(138);
			const uint16_t i = (uint16_t)((uint16_t)(((int16_t)row >> 1) + (r1 << 3)) << 1);
			poke(0x03BE, peek((uint16_t)(0x5238 + i)));
		}
		else
		{
			cy(54);
			poke(0x03BE, peek((uint16_t)(0x527E + (uint16_t)(r1 << 1))));
		}
		cy(99);
		pokeb(0x0797, 4);
		pokeb(0x0777, 4);
		pokeb(0x07D7, (uint8_t)row);
	}
	cy(165);
	set_banks(0x3800, 0x3C00);
	return 0x3C00;
}

// F:E5D2  skater_anim_a(int t): animation pointer for terrain t: 0x4F64 + 2 * 0x01F8 when
// t & 7 is 0, else 0x4C4C[t >> 3] + 0xFE2D[t & 7] + 2 * (0x01F8 & 7) (bank B 0x0400)
uint16_t skater_anim_a(const Args &a)
{
	At here(0xE5D2);
	Helper h(F162);
	cy(273);
	const uint16_t sa = peek(0x01D2), sb = peek(0x01D4);
	set_banks(0x0000, 0x0400);
	const uint16_t dir = sext8(peekb(0x01F8)) & 0x0007;
	const uint16_t t = a.u(0);
	uint16_t r;
	if (!(t & 7))
	{
		cy(66);
		r = (uint16_t)((uint16_t)(sext8(peekb(0x01F8)) << 1) + 0x4F64);
	}
	else
	{
		cy(192);
		const uint16_t row = peek((uint16_t)(0x4C4C + (uint16_t)(((int16_t)t >> 3) << 1)));
		r = (uint16_t)(row + peekb((uint16_t)(0xFE2D + (t & 7))) + (uint16_t)(dir << 1));
	}
	cy(165);
	restore_banks(sa, sb);
	return r;
}

// F:E648  skater_anim_b(int t): like F:E5D2 with the tables 0x4000, 0x4020 and 0xFE35; on
// terrain 2 / 10 (0x0327) at speed 0x01B4 >= 0x500 the tables 0x43E8 / 0x4408 are used
uint16_t skater_anim_b(const Args &a)
{
	At here(0xE648);
	Helper h(F162);
	cy(267);
	const uint16_t sa = peek(0x01D2), sb = peek(0x01D4);
	push(sb);
	set_banks(0x0000, 0x0400);
	const uint16_t dir = sext8(peekb(0x01F8));
	const uint16_t t = a.u(0);
	uint16_t r;
	if (!(t & 7))
	{
		cy(54);
		r = (uint16_t)((uint16_t)(dir << 1) + 0x4000);
	}
	else
	{
		const uint8_t terrain = peekb(0x0327);
		cy(54);
		bool fast = false;
		if (terrain == 2)
		{
			cy(24);
			fast = true;
		}
		else
		{
			cy(42);
			fast = terrain == 10;
		}
		if (fast)
		{
			cy(42);
			fast = (int16_t)peek(0x01B4) >= 0x0500;
		}
		if (fast)
		{
			cy(48 + 42);
			r = (uint16_t)((uint16_t)(dir << 1) + (terrain == 2 ? 0x43E8 : 0x4408));
		}
		else
		{
			cy(216);
			const uint16_t row = peek((uint16_t)(0x4020 + (uint16_t)(((int16_t)t >> 3) << 1)));
			r = (uint16_t)(row + peekb((uint16_t)(0xFE35 + (t & 7))) + (uint16_t)(dir << 1));
		}
	}
	cy(159);
	restore_banks(sa, sb);
	return r;
}

// F:EF24  add_score(int n): the score (long at 0x023E) grows by n; each time it reaches the
// next bonus threshold (long at 0x0244) a bonus is counted in 0x025A (up to 4), the next
// threshold is the step 0x5212[0x025A + 0x0FE3] (bank 0x3800) further, and, outside state 5,
// B:72CA(1) announces it
uint16_t add_score(const Args &a)
{
	At here(0xEF24);
	Helper h(F162);
	cy(210);
	const uint16_t sa = peek(0x01D2), sb = peek(0x01D4);
	push(sa);
	push(sb);
	pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) | 0x01));
	T.r[1] = 0x0240; T.r[2] = 0x023E; T.r[3] = 0; T.r[4] = a.u(0);
	call_from(0xEF48, 0x80C8, {});                                  // add32
	cy(78);
	poke(0x024A, peek(0x0246));
	poke(0x0248, peek(0x0244));
	uint16_t r0;
	for (;;)
	{
		cy(114);
		T.r[1] = 0x024A; T.r[2] = 0x0248; T.r[3] = peek(0x0240); T.r[4] = peek(0x023E);
		call_from(0xEF68, 0x80D0, {});                              // sub32: distance to the threshold
		cy(36);
		r0 = peek(0x024A);
		if ((int16_t)r0 > 0)
		{
			cy(12);
			break;
		}
		if (r0 == 0)
		{
			cy(12 + 36);
			if (peek(0x0248) != 0) break;
		}
		cy(435);
		pokeb(0x0FA2, (uint8_t)(peekb(0x0FA2) | 0x02));
		const uint8_t bonus = peekb(0x025A);
		pokeb(0x025A, (uint8_t)(bonus + (bonus < 4 ? 1 : 0)));
		set_banks(0x3800, 0x3C00);
		const uint16_t i = (uint16_t)((uint16_t)(peekb(0x025A) + peekb(0x0FE3)) << 1);
		T.r[0] = i;
		T.r[1] = 0x0246; T.r[2] = 0x0244; T.r[3] = 0; T.r[4] = peek((uint16_t)(0x5212 + i));
		call_from(0xEFC0, 0x80C8, {});                              // add32: next threshold
		cy(120);
		poke(0x024A, peek(0x0246));
		poke(0x0248, peek(0x0244));
		if (peekb(0x018F) == 5) continue;
		cy(84);
		T.r[5] = peek(0x0188);
		call_from(0xEFE0, 0x72CA, {0x0001});
		cy(57);
		poke(0x0188, T.r[5]);
	}
	cy(192);
	restore_banks(sa, sb);
	return r0;
}

// F:F064  start_object4(int kind): object 4 appears next to the skater (0x0400 right and 0x0300
// down, 0x0800 more on terrain 5-7) with kind (0..9; -1 on screen 7, negative: nothing) in
// 0x0394; if it was not active, it is activated (type 0x53) and F:B4B6(4) runs.  Returns kind.
uint16_t start_object4(const Args &a)
{
	At here(0xF064);
	Helper h(F138);
	const uint16_t arg = (uint16_t)(a.sp + 2);
	cy(42);
	if (peekb(0x0F59) == 7)
	{
		cy(36);
		poke(arg, 0xFFFF);
	}
	cy(39);
	if ((int16_t)peek(arg) < 0) return cy(48), peek(arg);
	cy(30);
	if ((int16_t)peek(arg) > 9)
	{
		cy(36);
		poke(arg, 0x0009);
	}
	cy(282);
	poke(0x0438, 0x001E);
	poke(0x0638, 0x8040);
	poke(0x06F8, (uint16_t)(peek(0x06F0) + 0x0400));
	poke(0x0738, (uint16_t)(peek(0x0730) + 0x0300));
	if ((sext8(peekb(0x0327)) & 7) >= 5)
	{
		cy(33);
		poke(0x0738, (uint16_t)(peek(0x0738) + 0x0800));
	}
	cy(423);
	poke(0x06B8, peek(0x06B0));
	poke(0x03B8, 0x576A);
	pokeb(0x0394, peekb(arg));
	pokeb(0x0794, 3);
	pokeb(0x0774, 3);
	pokeb(0x07B4, 0);
	poke(0x03F8, 0);
	pokeb(0x0474, (uint8_t)(peekb(0x0470) & 0xC0));
	pokeb(0x0474, (uint8_t)(peekb(0x0474) | 0x20));
	pokeb(0x0494, (uint8_t)(peekb(0x0494) | 0x18));
	if (!peekb(0x0374))
	{
		cy(141);
		const uint16_t sa = peek(0x01D2), sb = peek(0x01D4);
		T.r[1] = sa; T.r[2] = sb;
		pokeb(0x0374, 0x53);
		T.r[0] = peekb(0x0470) & 0xC0;
		call_from(0xF11C, 0xB4B6, {0x0004});
		cy(150);
		restore_banks(sa, sb);
	}
	cy(48);
	return peek(arg);
}

// F:D19C: set up the slots of a level: object queue state, entries (F:D8D2, F:D880),
// slots 0-3 (F:CF5E), the four fixed objects (F:CE24 from 0x70E4/0x70E8, bank B 0x0400);
// for a park (0x0F2E) above the last one set up (0x0E94) also the slots 4-0xAE whose type
// has bit 0 of 0x6438 clear, the park's objects (0x708C and the list at 0x6B9E) and the
// object counters 0x0E7E-0x0E86 (total 0x0E93) from 0x68D0; finally A:48CC (bank 0x1800)
uint16_t setup_level_slots(const Args &)
{
	At here(0xD19C);
	Helper h(F162);
	cy(321);
	const uint16_t sa = peek(0x01D2), sb = peek(0x01D4);
	push(sa);
	push(sb);
	pokeb(0x0E98, 0x40);
	poke(0x0E96, 0x020E);
	poke(0x09AC, 0);
	pokeb(0x0E6B, 0xFF);
	pokeb(0x0E6C, 5);
	pokeb(0x0E68, 0);
	pokeb(0x0E6A, 0xFF);
	call_from(0xD1D2, 0xD8D2, {});
	cy(30);
	call_from(0xD1D6, 0xD880, {});
	cy(18);
	for (int16_t i = 3; i >= 0; i--)
	{
		if (peekb((uint16_t)(0x09B8 + i)))
		{
			T.r[1] = (uint16_t)i;
			cy(39 + 54);                               // push, JSR
			call_from(0xD1E6, 0xCF5E, {(uint16_t)i});
			cy(18);
		}
		else
			cy(39);
		charge(0xD1DA, 24);                              // rest of the pass F:D1DA-D1EA
	}
	cy(144);
	set_banks(0x0000, 0x0400);
	for (int16_t i = 3; i >= 0; i--)
	{
		T.r[1] = (uint16_t)i;
		T.r[0] = (uint16_t)(i << 1);
		const uint16_t b = peekb((uint16_t)(0x70E4 + i));
		cy(177);
		call_from(0xD218, 0xCE24, {0, peek((uint16_t)(0x70E8 + 2 * i)), b});
		charge(0xD204, 42);                              // rest of the pass F:D204-D21E
	}
	cy(84);
	const int16_t done = (int16_t)sext8(peekb(0x0E94));
	if ((int16_t)peekb(0x0F2E) > done)
	{
		cy(18);
		for (int16_t i = 0x00AE; i >= 4; i--)
		{
			const uint16_t t = sext8(peekb((uint16_t)(0x09B8 + i)));
			if (t)
			{
				set_banks(0x0000, 0x0400);
				cy(39 + 183);                          // F:D232-D254
				if (!(peekb((uint16_t)(0x6438 + t)) & 0x01))
				{
					T.r[1] = t; T.r[2] = (uint16_t)i; T.r[3] = t;
					cy(54);                            // push, JSR
					call_from(0xD25C, 0xCF5E, {(uint16_t)i});
					cy(18);
				}
			}
			else
				cy(39);
			charge(0xD232, 42);                          // rest of the pass F:D232-D264
		}
		cy(291);
		set_banks(0x0000, 0x0400);
		uint16_t p = (uint16_t)((uint16_t)(peekb((uint16_t)(0xFDE3 + peekb(0x0F2E))) << 3) + 0x708C - 8);
		for (int n = 4; n >= 1; n--)
		{
			T.r[1] = p; T.r[2] = (uint16_t)n;
			cy(114);
			call_from(0xD2A4, 0xCE24, {0, peek(p), 0x0001});
			p = (uint16_t)(p + 2);
			charge(0xD298, 54);                          // rest of the pass F:D298-D2AC
		}
		cy(75);
		uint16_t q = peek((uint16_t)(0x6B9E + (uint16_t)(peekb(0x0F2E) << 1)));
		for (;;)
		{
			cy(36);                                    // F:D2BA-D2BE
			if (peek(q) == 0xFFFF) break;
			T.r[1] = q;
			const uint16_t w = peek(q);
			cy(123);
			call_from(0xD2CC, 0xCE24, {0, peek((uint16_t)(q + 2)), w});
			q = (uint16_t)(q + 4);
			charge(0xD2BA, 48);                          // rest of the pass F:D2C0-D2D4
		}
		cy(45);
		pokeb(0x0E93, 0);
		for (uint16_t i = 0x14; i >= 1; i--)
		{
			pokeb((uint16_t)(0x0E7D + i), 0);
			charge(0xD2DE, 48);                          // pass F:D2DE-D2E2
		}
		cy(66);
		const uint16_t table = (uint16_t)(0x68D0 + (uint16_t)(peekb(0x0F2E) << 1));
		for (uint16_t i = 9; i >= 1; i--)
		{
			const uint8_t v = peekb((uint16_t)(i + peek(table) - 1));
			pokeb((uint16_t)(0x0E7D + i), v);
			pokeb(0x0E93, (uint8_t)(peekb(0x0E93) + v));
			charge(0xD2F0, 189);                         // pass F:D2F0-D30A
		}
		cy(39);
		pokeb(0x0E94, peekb(0x0F2E));
	}
	cy(174);
	set_banks(0x1800, 0x1C00);
	T.r[0] = 0x1C00;
	const uint16_t r0 = call_from(0xD32E, 0x48CC, {});
	cy(192);
	restore_banks(sa, sb);
	return r0;
}

// F:E78E  query_on_button_press(): on a new press of a button (0x0193 bits 0xC0) while 0x0190
// is clear, query the sound CPU (F:8E6A, command 10, F:8F50; then 1 / 2 for button 0x80 /
// 0x40, or 0x80 | (0x0F2E - 1) in state 9; five answer bytes, F:8F62, F:8E72).  A first
// answer with bit 7 set returns 1 and sets the state 0x018F to 6 / 7 (in state 9 it needs
// answer bit 6 instead).  Without a press 0x0190 is cleared.  Waits for the sound CPU, so the
// cycles are charged here.
uint16_t query_on_button_press(const Args &)
{
	At here(0xE78E);
	Helper h(F138);
	h.out += 21;                                       // every path ends with the body's RTS
	cy(30 + 12);                                  // BITB, BEQ
	if (!(peekb(0x0193) & 0xC0))
	{
		cy(27 + 12);
		pokeb(0x0190, 0);
		return 0;
	}
	cy(24 + 12);
	if (peekb(0x0190))
	{
		cy(12);
		return 0;
	}
	uint16_t r1 = T.r[1];
	cy(30);
	call_from(0xE7A4, 0x8E6A, {});
	cy(30 + 30);
	push(0x000A);
	call_from(0xE7AC, 0x8EA2, {});
	cy(30);
	call_from(0xE7B0, 0x8F50, {});
	cy(30 + 12);
	if (peekb(0x018F) == 9)
	{
		cy(12 + 24 + 12 + 21 + 27 + 12);
		T.r[0] = (uint16_t)(peekb(0x0F2E) - 1);
		poke(T.r[6], (uint16_t)(T.r[0] | 0x0080));
	}
	else
	{
		cy(24 + 12);
		if (peekb(0x0193) & 0x80)
		{
			cy(18 + 27 + 12);
			r1 = 6;
			poke(T.r[6], 0x0001);
		}
		else
		{
			cy(18 + 27);
			r1 = 7;
			poke(T.r[6], 0x0002);
		}
	}
	T.r[1] = r1;
	cy(30);
	call_from(0xE7E4, 0x8EA2, {});
	cy(30);
	const uint16_t answer = call_from(0xE7E8, 0x8ED2, {});
	T.r[2] = answer;
	cy(12);
	for (int i = 0; i < 4; i++)
	{
		cy(30);
		call_from((uint16_t)(0xE7EE + 4 * i), 0x8ED2, {});
	}
	cy(30);
	call_from(0xE7FE, 0x8F62, {});
	cy(30);
	call_from(0xE802, 0x8E72, {});
	T.r[6] = (uint16_t)(T.r[6] + 2);
	cy(12 + 12);
	if (!(answer & 0x80))
	{
		cy(18 + 12 + 12);
		return 0;
	}
	cy(30 + 12);
	if (peekb(0x018F) != 9)
	{
		cy(27 + 12);
		pokeb(0x018F, (uint8_t)r1);
	}
	else
	{
		cy(18 + 12);
		if (!(answer & 0x40))
		{
			cy(18 + 12 + 12);
			return 0;
		}
	}
	cy(18 + 18);
	return 1;
}

// ---------------------------------------------------------------- assembly interface

// F:D35E (registers): place one waiting object.  The byte counters 0x0E7E-0x0E90 (total
// 0x0E93) count the objects still to place, by kind 19..1.  For the first kind with objects
// left, a random cell (0x0E92 += F:9152, masked with 0xFD95[kind]) of the kind's list 0x6948
// (bank B 0x0400) is tried: kinds 1-3 need its column bits (>> 4 & 3) to differ from the
// tile bank 0x01CE - 1, the others a cell outside the window 0x099A-0x099C / 0x0996-0x0998.
// The object comes from F:CE24(flag, cell, type) (kinds up to 10: type from the park's list
// 0x6862, flag 1; others 0x6935, flag 0); when it is placed (R0 = 0) the counters drop by 1.
void spawn_counted_object()
{
	At here(0xD35E);
	const uint16_t sp0 = T.r[6];
	cy(54);                                            // F:D35E-D366
	T.r[6] = (uint16_t)(sp0 - 8);                       // SUB #8,SP: locals
	uint16_t &r0 = T.r[0], &r1 = T.r[1], &r2 = T.r[2], &r3 = T.r[3], &r4 = T.r[4], &r5 = T.r[5];
	if (!peekb(0x0E93))
		cy(18);                                        // JMP
	else
	{
		cy(18);
		for (r4 = 0x0013;;)
		{
			int c = 93;                                // F:D370-D37A
			const uint16_t counter = (uint16_t)(0x0E7D + r4);
			if (peekb(counter))
			{
				cy(c + 186);
				c = 0;
				set_banks(0x0000, 0x0400);
				r0 = 0x0400;
				call_from(0xD398, 0x9152, {});                      // next_random
				c += 291;                              // F:D398-D3C6
				r5 = peekb(0x0E92);
				r0 = (uint16_t)(r0 + r5);
				pokeb(0x0E92, (uint8_t)r0);
				r1 = peek(0x0E92);
				r0 = (uint16_t)~sext8(peekb((uint16_t)(0xFD95 + r4)));
				r1 &= (uint16_t)~r0;
				r1 &= 0x00FF;
				r0 = (uint16_t)(r4 << 1);
				r5 = (uint16_t)((uint16_t)(r1 << 1) + peek((uint16_t)(0x6948 + r0)));
				r3 = peek(r5);
				bool skip = false;
				if ((int16_t)r4 > 3)
				{
					c += 66;
					r2 = r3 & 0x000F;
					if ((int16_t)r2 >= (int16_t)peek(0x099A))
					{
						c += 36;
						if ((int16_t)r2 <= (int16_t)peek(0x099C))
						{
							c += 66;
							r2 = r3 & 0x01F0;
							if ((int16_t)r2 >= (int16_t)peek(0x0996))
							{
								c += 36;
								skip = (int16_t)r2 < (int16_t)peek(0x0998);
							}
						}
					}
				}
				else
				{
					c += 207;
					r5 = (uint16_t)((int16_t)r1 >> 4);
					r0 = (uint16_t)(peek(0x01CE) - 1);
					skip = (r5 & 3) == r0;
					if (skip) c += 12;
				}
				if (skip) c += 30;                     // F:D40A
				c += 39;                               // F:D40E-D412
				if (!skip)
				{
					c += 30;                           // F:D414-D418
					uint16_t flag, type;
					if ((int16_t)r4 > 10)
					{
						c += 105;
						type = peekb((uint16_t)(0x6935 + r4));
						flag = 0;
					}
					else
					{
						c += 210;
						r0 = (uint16_t)(peek((uint16_t)(0x6862 + (uint16_t)(peekb(0x0F2E) << 1))) + r4);
						type = peekb((uint16_t)(r0 - 1));
						flag = 1;
					}
					cy(c + 132);
					c = 0;
					call_from(0xD456, 0xCE24, {flag, r3, type});
					c += 42;                           // F:D456-D45C
					if (r0 == 0)
					{
						cy(c + 111);
						pokeb(counter, (uint8_t)(peekb(counter) - 1));
						r0 = (uint16_t)(sext8(peekb(0x0E93)) - 1);
						pokeb(0x0E93, (uint8_t)r0);
						break;
					}
				}
			}
			c += 24;                                   // F:D46E-D470
			r4 = (uint16_t)(r4 - 1);
			if (r4 == 0)
			{
				cy(c);
				break;
			}
			charge(0xD370, c + 18);                      // JMP back
		}
	}
	cy(39);                                            // F:D476-D47A
	T.r[6] = sp0;
	fl::add((uint16_t)(sp0 - 8), 8);                    // ADD #8,SP
}

// F:D4E4 (registers): position of the skater (object 0) relative to object 0x0EA0.  R5 = the
// object's shape record (table 0x4354 through 0xFDBA[0x07D0[obj]], bank A 0x2400), R4 =
// column (x distance + 0x0A00 - record[-4]: high byte / 2) also in 0x0E9E, R3 = height
// (record[-2] - (y + z distance + 0x0300)) also in 0x0E9C; when positive, R3 becomes the
// row's offset byte from 0x4338.  R1 is preserved.
void offset_to_object_0ea0()
{
	At here(0xD4E4);
	cy(696);                                           // F:D4E4-D552
	const uint16_t saved_r1 = T.r[1];
	set_banks(0x2400, 0x2800);
	uint16_t r1 = sext8(peekb(0x0EA0));
	uint16_t r0 = peekb((uint16_t)(0x07D0 + r1));
	uint16_t r5 = (uint16_t)(sext8(peekb((uint16_t)(0xFDBA + r0))) + 0x4354);
	r5 = peek(r5);
	r1 = (uint16_t)(r1 << 1);
	r0 = (uint16_t)(peek(0x06F0) - peek((uint16_t)(0x06F0 + r1)));
	uint16_t r4 = (uint16_t)(r0 + 0x0A00 - peek((uint16_t)(r5 - 4)));
	r0 = sext8((uint8_t)(r4 >> 8));
	r4 = (uint16_t)((int16_t)r0 >> 1);
	r0 = (uint16_t)(peek(0x0730) + peek(0x06B0) - peek((uint16_t)(0x0730 + r1)));
	uint16_t r3 = (uint16_t)-(uint16_t)(r0 + 0x0300);
	r3 = fl::add(r3, peek((uint16_t)(r5 - 2)));
	poke(0x0E9E, r4);
	poke(0x0E9C, r3);
	if ((int16_t)r3 > 0)
	{
		cy(87);
		r0 = (uint16_t)((int16_t)sext8((uint8_t)(r3 >> 8)) >> 1);
		r3 = peekb((uint16_t)(0x4338 + r0));
		T.psw &= ~CF;                                  // CLR R3
	}
	cy(39);
	T.r[0] = r0; T.r[3] = r3; T.r[4] = r4; T.r[5] = r5;
	T.r[1] = saved_r1;
	fl::mov(saved_r1);
}

// F:D566 (registers): the skater against object -0x0EA0 (0x0EA0 negative: just reached).
// F:AD70 (R0 = 0, R1 = -obj) gives a side bit kept on the stack; 0x0EA0 is made positive
// (copy in 0x0EA1, 0x0EA2 = 5) and F:D4E4 locates the skater.  Outside the shape 0x0EA0 is cleared.
// Otherwise the shape's height bytes under the skater are followed up (rows of 0x12 bytes):
// at or above the result the skater is let go (0x0EA0 cleared, direction 4 / 12 by the side,
// speed 0x01B4 from 0x7C90[0x025D] in bank B 0x2C00, then F:A2F0); below it its height
// 0x06B0 is set and F:D778 runs.
void contact_object_0ea0()
{
	At here(0xD566);
	cy(225);                                           // F:D566-D584
	T.r[6] = (uint16_t)(T.r[6] - 2);                     // TST -(SP): local for the side
	set_banks(0x0000, 0x0400);
	T.r[1] = (uint16_t)-sext8(peekb(0x0EA0));
	T.r[0] = 0;
	call_from(0xD588, 0xAD70, {});
	cy(150);
	ww(T.r[6], T.r[0]);
	pokeb(0x0EA0, (uint8_t)-peekb(0x0EA0));
	pokeb(0x0EA1, peekb(0x0EA0));
	pokeb(0x0EA2, 5);
	call_from(0xD59E, 0xD4E4, {});
	cy(36);
	T.r[0] = T.r[4] | T.r[3];
	if ((int16_t)T.r[0] < 0)
	{
		cy(39);
		pokeb(0x0EA0, 0);
	}
	else
	{
		cy(90);
		T.r[0] = peek(0x06B0);
		T.r[1] = (uint16_t)(T.r[3] + T.r[5] + T.r[4]);
		if (peekb(T.r[1]))
			for (;;)
			{
				cy(156);
				T.r[3] = (uint16_t)(T.r[3] - 0x0012);
				T.r[0] = (uint16_t)(T.r[0] + 0x0200);
				T.r[1] = (uint16_t)(T.r[3] + T.r[5] + T.r[4]);
				T.r[2] = peekb(T.r[1]) & 0x007F;
				if (rw(T.r[6]) & 1)
				{
					cy(24);
					if (T.r[2] != 0)
					{
						cy(24);
						if ((int16_t)T.r[3] > 0) continue;
					}
					cy(30);
					T.r[0] = (uint16_t)(T.r[0] - 0x0100);
					break;
				}
				cy(42);
				T.r[1] = 7 & (uint16_t)~T.r[2];
				if (T.r[1] != 0) break;
			}
		cy(36);
		if ((int16_t)T.r[0] >= (int16_t)peek(0x06B2))
		{
			cy(63);
			pokeb(0x0EA0, 0);
			if (rw(T.r[6]) & 1)
			{
				cy(45);
				pokeb(0x031D, 4);
			}
			else
			{
				cy(33);
				pokeb(0x031D, 0x0C);
			}
			cy(180);
			T.r[0] = 0x2C00;
			poke(0x01D4, 0x2C00);
			ww(0x1402, 0x2C00);
			T.r[0] = (uint16_t)(sext8(peekb(0x025D)) << 1);
			poke(0x01B4, peek((uint16_t)(0x7C90 + T.r[0])));
			call_from(0xD624, 0xA2F0, {});
			cy(12);
		}
		else
		{
			cy(57);
			poke(0x06B0, T.r[0]);
			call_from(0xD62E, 0xD778, {});
		}
	}
	cy(39);
	const uint16_t side = rw(T.r[6]);
	T.r[6] = (uint16_t)(T.r[6] + 2);
	fl::tst(side);
}

// F:DB4E (registers): sprite palette (0x1000-0x107F) = the colours at 0x7EEC-0x7FAB (bank B
// 0x0400; the first 16 from 0x7F0C-0x7F2B of bank 0x1C00 when 0x01F5 is set) minus
// sext(0x0EA7), each group of 16 in reverse order.  Banks restored.
void fade_sprite_palette()
{
	At here(0xDB4E);
	cy(258);                                           // F:DB4E-DB72
	push(peek(0x01D2));
	push(peek(0x01D4));
	set_banks(0x0000, 0x0400);
	T.r[0] = 0x0400;
	T.r[3] = sext8(peekb(0x0EA7));
	T.r[2] = 0x7FAC;
	for (int16_t n = 0x2F; n >= 0; n--)
	{
		charge(0xDB76, 21 + 12 + 12 + 12 + 18 + 12);     // pass F:DB76-DB82
		T.r[2] = (uint16_t)(T.r[2] - 2);
		T.r[1] = (uint16_t)(peek(T.r[2]) - T.r[3]);
		T.r[4] = (uint16_t)(((uint16_t)n << 1) ^ 0x001E);
		T.r[0] = 0x001E;
		io(30, [] { poke((uint16_t)(0x1020 + T.r[4]), T.r[1]); });
		cy(12 + 12);                                     // DEC R5; BGE
	}
	cy(36);
	if (peekb(0x01F5))
	{
		cy(162);
		set_banks(0x1800, 0x1C00);
		T.r[0] = 0x1C00;
		T.r[2] = 0x7F4C;
	}
	cy(18);
	for (int16_t n = 0x0F; n >= 0; n--)
	{
		charge(0xDBB2, 21 + 12 + 12 + 12 + 18 + 12);     // pass F:DBB2-DBBE
		T.r[2] = (uint16_t)(T.r[2] - 2);
		T.r[1] = (uint16_t)(peek(T.r[2]) - T.r[3]);
		T.r[4] = (uint16_t)(((uint16_t)n << 1) ^ 0x001E);
		T.r[0] = 0x001E;
		io(30, [] { poke((uint16_t)(0x1000 + T.r[4]), T.r[1]); });
		cy(12 + 12);                                     // DEC R5; BGE
	}
	cy(192);
	T.r[5] = 0xFFFF;
	poke(0x01D2, rw((uint16_t)(T.r[6] + 2)));
	poke(0x01D4, pop());
	ww(0x1400, peek(0x01D2));
	ww(0x1402, peek(0x01D4));
	fl::tst(pop());
}

// F:DBE2 (registers): one step of the palette fade towards 0x0EA8 (negative: none): on odd
// frames the level 0x0EA7 moves one step and the sprite palette is redone (F:DB4E); on even
// frames a reached target is cleared (0xFF) and the playfield palette is redone (F:DB00)
void step_palette_fade()
{
	At here(0xDBE2);
	cy(36);
	const uint8_t target = peekb(0x0EA8);
	fl::tstb(target);
	if (target & 0x80)
	{
		cy(21);
		return;
	}
	cy(66);
	T.r[0] = sext8(peekb(0x0EA7));
	T.r[1] = sext8(peekb(0x0EA8));
	if (peek(0x01C4) & 1)
	{
		cy(48);
		if ((int16_t)T.r[1] > (int16_t)T.r[0])
		{
			cy(36);
			T.r[1] = (uint16_t)(T.r[0] + 1);
		}
		else
		{
			cy(24);
			T.r[1] = (uint16_t)(T.r[0] - 1);
		}
		cy(57);
		pokeb(0x0EA7, (uint8_t)T.r[1]);
		call_from(0xDC0E, 0xDB4E, {});
		cy(21);
		return;
	}
	cy(48);
	if (T.r[1] == T.r[0])
	{
		cy(33);
		pokeb(0x0EA8, 0xFF);
	}
	cy(30);
	call_from(0xDC22, 0xDB00, {});
	cy(21);
}

// F:DC3C (registers; run from 0x0EB0 by F:DDEC): load palette block 0x0EAC from colour set
// 0x0EAE (bank B 0x1C00): blocks 0-7 are 4 text colours (0x1080 + 8 * block, from 0x7F6C),
// 8-15 16 playfield colours (0x1100, from 0x7EAC), 16 on 16 sprite colours (0x1000,
// reversed, from 0x7F0C).  Banks restored.
void load_palette_block()
{
	At here(0xDC3C);
	cy(282);                                           // F:DC3C-DC68
	uint16_t *r = T.r;
	r[5] = peek(0x01D2);
	push(peek(0x01D4));
	r[1] = peek(0x0EAC);
	r[0] = peek(0x0EAE);
	r[2] = 0x1800;
	poke(0x01D2, 0x1800);
	ww(0x1400, 0x1800);
	r[2] = 0x1C00;
	poke(0x01D4, 0x1C00);
	ww(0x1402, 0x1C00);
	if ((int16_t)r[1] <= 7)
	{
		cy(90);
		r[3] = (uint16_t)(r[1] << 2);
		r[4] = (uint16_t)(r[0] << 2);
		for (int16_t n = 3; n >= 0; n--)
		{
			charge(0xDC7A, 12 * 6);                      // pass F:DC7A-DC84
			r[1] = (uint16_t)((uint16_t)(n + r[3]) << 1);
			r[0] = (uint16_t)((uint16_t)(n + r[4]) << 1);
			io(45, [] { poke((uint16_t)(0x1080 + T.r[1]), peek((uint16_t)(0x7F6C + T.r[0]))); });
			cy(12 + 12);                                 // DEC R2; BGE
		}
		cy(12);
	}
	else
	{
		cy(90 + 96);
		r[4] = (uint16_t)(r[0] << 4);
		const bool pf = (int16_t)r[1] <= 15;
		r[3] = (uint16_t)((uint16_t)(r[1] - (pf ? 8 : 16)) << 4);
		for (int16_t n = 15; n >= 0; n--)
		{
			if (pf)
				charge(0xDCB4, 12 * 6);                  // pass F:DCB4-DCBE
			else
				charge(0xDCDE, 12 * 3 + 18 + 12 * 4);    // pass F:DCDE-DCEE
			r[1] = (uint16_t)((uint16_t)(n + r[3]) << 1);
			if (!pf) r[1] ^= 0x001E;
			r[0] = (uint16_t)((uint16_t)(n + r[4]) << 1);
			if (pf)
			{
				io(45, [] { poke((uint16_t)(0x1100 + T.r[1]), peek((uint16_t)(0x7EAC + T.r[0]))); });
				cy(12 + 12);                             // DEC R2; BGE
			}
			else
			{
				io(45, [] { poke((uint16_t)(0x1000 + T.r[1]), peek((uint16_t)(0x7F0C + T.r[0]))); });
				cy(12 + 12);
			}
		}
		if (pf) cy(12);
	}
	cy(159);
	r[2] = 0xFFFF;
	poke(0x01D2, r[5]);
	poke(0x01D4, pop());
	ww(0x1400, peek(0x01D2));
	const uint16_t v = peek(0x01D4);
	ww(0x1402, v);
	T.psw = (uint8_t)((T.psw & ~CF) | ((r[4] >> 15) & 1));      // C of the last ASL R0
	fl::mov(v);
}

// F:DDEC (registers): run the palette job queued in 0x0EB0 (a routine address) and clear it
void run_palette_job()
{
	At here(0xDDEC);
	cy(36);
	T.r[0] = peek(0x0EB0);
	fl::mov(T.r[0]);
	if (!T.r[0])
	{
		cy(21);
		return;
	}
	cy(27);                                            // JSR PC,(R0)
	call_from(0xDDF4, T.r[0], {});
	cy(27 + 21);
	poke(0x0EB0, 0);
	fl::clr();
}

// F:E254 (registers): the skater's phase 0x01EB.  After F:9236, a press of button 0x40
// (0x0193) sets 0x0332 (2, or 1 when already set; terrain bit 4 also sets 0x0324 bit 7).
// With 0x0320 clear and 0x0330 != 1 phase 1 starts (0x0320 = 0x0330 = 1, timers from 0xFE3E
// [0x0331]; from 0x01EA = 2 first: speed made positive, direction from 0x01F8, 0x01EA = 1,
// F:E238, sound 0x58, B:7E44).  Otherwise 0x0332 = 2 with 0x01EA = 1 on terrain classes 0-2
// (not on level 3) starts phase 6 (z speed 0x05B2 > 0x30, raised by 0x7C70[0x025F]) or 4
// (speed kept in 0x02E8 and stopped).  Then the animation pointers 0x03B0-0x03B6 are set
// from the terrain (F:E5D2, F:E648) and the phase steps (jump table at 0xFE54, phases 0-11).
void update_skater_phase()
{
	At here(0xE254);
	uint16_t *r = T.r;
	cy(30);
	call_from(0xE258, 0x9236, {});
	cy(162);                                           // F:E258-E270
	set_banks(0x0000, 0x0400);
	r[0] = 0x0400;
	if (!peekb(0x031F))
	{
		cy(42);
		if (peekb(0x0193) & 0x40)
		{
			cy(36);
			if (peekb(0x0332))
			{
				cy(33);
				pokeb(0x0332, 1);
			}
			else
			{
				cy(45);
				pokeb(0x0332, 2);
			}
			cy(42);
			if (peekb(0x0327) & 4)
			{
				cy(45);
				pokeb(0x0324, (uint8_t)(peekb(0x0324) | 0x80));
			}
		}
		else
		{
			cy(27);
			pokeb(0x0332, 0);
		}
		cy(36);
		bool start = !peekb(0x0320);
		if (start)
		{
			cy(42);
			start = peekb(0x0330) != 1;
		}
		if (start)
		{
			cy(42);
			if (peekb(0x01EA) == 2)
			{
				cy(36);
				if ((int16_t)peek(0x01B4) < 0)
				{
					cy(27);
					poke(0x01B4, (uint16_t)-peek(0x01B4));
				}
				cy(102);
				pokeb(0x031D, peekb(0x01F8));
				pokeb(0x01EA, 1);
				call_from(0xE2D2, 0xE238, {});                      // reset_skater_state
				cy(60);
				push(0x0058);
				call_from(0xE2DA, 0x8E78, {});                      // queue_sound(0x58)
				cy(174);
				set_banks(0x3800, 0x3C00);
				r[0] = 0x3C00;
				call_from(0xE2F6, 0x7E44, {});
				cy(144);
				set_banks(0x0000, 0x0400);
				r[0] = 0x0400;
				r[6] = (uint16_t)(r[6] + 2);
			}
			cy(243);
			pokeb(0x0320, 1);
			pokeb(0x01EB, 1);
			pokeb(0x0330, 1);
			pokeb(0x07B2, 0);
			r[0] = sext8(peekb((uint16_t)(0xFE3E + sext8(peekb(0x0331)))));
			pokeb(0x0792, (uint8_t)r[0]);
			pokeb(0x0772, (uint8_t)r[0]);
		}
		else
		{
			cy(42);
			if (peekb(0x0332) == 2)
			{
				cy(30 + 12);
				if (peekb(0x01EA) == 1)
				{
					cy(72);
					r[0] = sext8(peekb(0x0327)) & 7;
					bool kick = r[0] < 3;
					if (kick)
					{
						cy(42);
						kick = peekb(0x0F59) != 3;
					}
					if (kick)
					{
						cy(30);
						call_from(0xE35E, 0xE238, {});              // reset_skater_state
						cy(87);
						r[0] = 4;
						pokeb(0x0330, 4);
						if ((int16_t)peek(0x05B2) > 0x0030)
						{
							cy(96);
							pokeb(0x01EB, 6);
							pokeb(0x07B1, 1);
							call_from(0xE37E, 0x9FB4, {});
							cy(246);
							r[0] = 0x2C00;
							poke(0x01D4, 0x2C00);
							ww(0x1402, 0x2C00);
							r[0] = sext8(peekb(0x025F));
							r[1] = peekb((uint16_t)(0x7C70 + r[0]));
							poke(0x05B2, (uint16_t)(peek(0x05B2) + r[1]));
							r[0] = 0x0400;
							poke(0x01D4, 0x0400);
							ww(0x1402, 0x0400);
						}
						else
						{
							cy(162);
							pokeb(0x01EB, 4);
							pokeb(0x07B1, 0);
							r[1] = peek(0x01B4);
							poke(0x02E8, r[1]);
							poke(0x01B4, 0);
							if ((int16_t)r[1] < 0)
							{
								cy(12);
								r[1] = (uint16_t)-r[1];
							}
							cy(30);
							if ((int16_t)r[1] >= 0x01C0)
							{
								cy(18);
								r[1] = 2;
							}
							else
							{
								cy(30);
								r[1] = 8;
							}
							cy(108);
							pokeb(0x0791, (uint8_t)r[1]);
							pokeb(0x0771, (uint8_t)r[1]);
							pokeb(0x0792, (uint8_t)r[1]);
							pokeb(0x0772, (uint8_t)r[1]);
						}
					}
				}
			}
		}
	}
	else
		cy(12);                                        // F:E342
	const uint8_t state = peekb(0x031F);
	cy(42);
	if (state != 1) cy(30);
	cy(12);
	if (state != 1 && state != 5)
	{
		cy(195);
		r[1] = sext8(peekb(0x01F8)) & 7;
		r[0] = (uint16_t)(r[1] << 1);
		poke(0x03F0, peek((uint16_t)(0x70F0 + r[0])));
		push(peekb(0x0327));
		call_from(0xE412, 0xE5D2, {});                              // skater_anim_a(terrain)
		cy(54);
		r[2] = r[0];
		bool still = peekb(0x01EA) == 2;
		if (!still)
		{
			cy(54);
			r[0] = 7 & (uint16_t)~peekb(0x0327);
			still = r[0] == 0;
			if (!still) cy(39);
		}
		if (still) cy(33);
		poke(0x03B0, still ? 0x50FA : r[2]);
		cy(54);
		r[3] = sext8(peekb(0x01EB));
		if (r[3] & 0x0040)
		{
			cy(18 + 102);
			r[0] = 0x1800;
			poke(0x01D2, 0x1800);
			ww(0x1400, 0x1800);
			call_from(0xE5AC, 0x54CE, {});
		}
		else
		{
			cy(51);
			r[0] = (uint16_t)(r[3] << 1);
			auto next_phase = [r]() {
				cy(90);                                // F:E4FE-E50A
				r[0] = (uint16_t)(r[3] + 1);
				pokeb(0x01EB, (uint8_t)r[0]);
				pokeb(0x07B1, (uint8_t)(peekb(0x07B1) + 1));
			};
			switch (r[3])
			{
			case 0:
			{
				cy(42);
				bool flat = !(peekb(0x0327) & 7);
				if (flat)
				{
					cy(42);
					flat = (int16_t)peek(0x01C8) >= 8;
				}
				if (flat)
				{
					cy(84);
					r[0] = (uint16_t)(((int16_t)sext8(peekb(0x01F8)) >> 1) << 1);
					const bool back = (peekb(0x0321) & 0x80) != 0;
					cy(back ? 60 : 72);
					r[1] = (uint16_t)(r[0] + (back ? 0x42B0 : 0x42C0));
					r[2] = (uint16_t)(r[0] + (back ? 0x4E1C : 0x4E2C));
					cy(39);
					poke(0x03B0, r[2]);
				}
				else
				{
					cy(84);
					ww(r[6], peekb(0x0327));
					call_from(0xE490, 0xE648, {});                  // skater_anim_b(terrain)
					cy(12);
					r[1] = r[0];
				}
				cy(66);
				poke(0x03B2, r[2]);
				poke(0x03B4, r[1]);
				break;
			}
			case 1:
				cy(42);
				if (peekb(0x07B2) == 2)
				{
					cy(12 + 63);
					r[0] = (uint16_t)(r[3] + 1);
					pokeb(0x01EB, (uint8_t)r[0]);
				}
				break;
			case 2: case 3:
				cy(42 + 12);
				if (peekb(0x07B2) == 3)
				{
					cy(30);
					call_from(0xE518, 0xE238, {});
					cy(12);
				}
				break;
			case 4:
				cy(36);
				if (peekb(0x07B1))
				{
					cy(30);
					call_from(0xE4B8, 0xE238, {});
					cy(78);
					pokeb(0x0330, 4);
					pokeb(0x01EB, 5);
				}
				else
				{
					cy(45);
					poke(0x03B0, 0x50FA);
				}
				break;
			case 5: case 6:
				cy(60);
				r[0] = (uint16_t)(peek(0x05B2) - peek(0x05B0));
				if (r[0] & 0x8000)
				{
					cy(12);
					next_phase();
				}
				break;
			case 7:
				cy(78);
				r[0] = (uint16_t)(peek(0x06B2) - peek(0x06B0));
				if ((int16_t)r[0] <= 0x0600)
				{
					cy(12);
					next_phase();
				}
				break;
			case 8:
				cy(42);
				if (peekb(0x01EA) == 1)
				{
					cy(72);
					r[0] = 0x0014;
					pokeb(0x0791, 0x14);
					pokeb(0x0771, 0x14);
					next_phase();
				}
				break;
			case 9:
				cy(30 + 12);
				if (peekb(0x0791) == 2)
				{
					cy(30);
					call_from(0xE518, 0xE238, {});
					cy(12);
				}
				break;
			case 10:
				cy(42);
				if (peekb(0x07B1) == 6)
				{
					cy(54 + 63);
					pokeb(0x0791, 0);
					pokeb(0x0771, 0);
					r[0] = (uint16_t)(r[3] + 1);
					pokeb(0x01EB, (uint8_t)r[0]);
				}
				break;
			case 11:
				cy(12);
				r[1] = 0;
				break;
			default:                                   // the table has no entries for these
				break;
			}
			cy(42);
			if (peekb(0x0330) == 4)
			{
				cy(90);
				r[1] = (uint16_t)(peekb(0x07B1) << 1);
				if ((int8_t)peekb(0x01EB) > 8)
				{
					cy(39);
					poke(0x03B2, peek(0x03B0));
				}
				else
				{
					cy(117);
					r[0] = (uint16_t)(sext8(peekb(0x01F8)) << 1);
					poke(0x03B2, peek((uint16_t)(0x4F44 + r[0])));
					poke(0x03B2, (uint16_t)(peek(0x03B2) + r[1]));
				}
				cy(90 + 39);
				r[0] = (uint16_t)(sext8(peekb(0x01F8)) << 1);
				poke(0x03B4, peek((uint16_t)(0x43C8 + r[0])));
				poke(0x03B4, (uint16_t)(peek(0x03B4) + r[1]));
			}
			else
			{
				cy(42);
				if (peekb(0x0330) == 1)
				{
					cy(165 + 39);
					poke(0x03B2, peek(0x03B0));
					r[1] = (uint16_t)(peekb(0x07B2) << 1);
					r[0] = (uint16_t)(sext8(peekb(0x01F8)) << 1);
					poke(0x03B4, peek((uint16_t)(0x4330 + r[0])));
					poke(0x03B4, (uint16_t)(peek(0x03B4) + r[1]));
				}
			}
		}
		cy(18);
		r[6] = (uint16_t)(r[6] + 2);                     // TST (SP)+
	}
	else
		cy(18);                                        // JMP F:E5AE
	cy(36);
	if (peekb(0x025D))
	{
		cy(42);
		if (peekb(0x031F) != 5)
		{
			cy(93);
			poke(0x03B6, peek(0x03B4));
			poke(0x03B6, fl::add(peek(0x03B6), 0x061E));
			return;
		}
	}
	cy(54);
	poke(0x03B6, 0x50FA);
	f_nzvc(0);                                         // C clear from the TSTB / CMPB before
}

// ---------------------------------------------------------------- packed tile blocks

// Packed tile runs (F:EAD4 and F:E82E).  A run starts with a byte: bits 0-5 the number of
// tiles (0: 65536), bits 6/7 the mode, then the tile's low byte (0x0F4C); mode 2 also has
// the high byte (0x0F4D).  The following tiles of the run: mode 0 a new low byte each, mode 1
// the same tile, mode 2 two new bytes each, mode 3 the tile number + 1 each.  The mode is
// kept in 0x0F5F (0-3).
uint8_t run_mode(uint8_t head)
{
	const bool b6 = (head & 0x40) != 0;
	return (head & 0x80) ? (b6 ? 3 : 2) : (b6 ? 1 : 0);
}

// cycles of the original tile pass (F:EB50 / F:E974) up to the mode dispatch: a new run
// (reading the head byte, by mode) or the next tile of the run (by mode)
int run_head_cycles(uint8_t head)
{
	static const int mode_part[4] = {24 + 39, 24 + 45, 30 + 45 + 57, 30 + 45};
	return 24 + 162 + ((head & 0x40) ? 12 : 0) + 24 + mode_part[run_mode(head)];
}
int run_next_cycles(uint8_t mode)
{
	static const int mode_part[4] = {69, 0, 90, 27};
	return 87 + (mode < 4 ? mode_part[mode] : 0);
}

// F:EAD4  draw_tile_block(int erase, int *block): block = {packed data, video address (low 7
// bits: column * 2), rows, columns}; the pointer argument is advanced past the header.  Tiles
// get the colour bits 0x0188 (or are cleared when erase is set); tile 0x149 is transparent.
// Video bank 0 is selected; the banks are restored.
uint16_t draw_tile_block(const Args &a)
{
	At here(0xEAD4);
	Helper h(F162);
	cy(276);                                           // F:EAD8-EB00
	const uint16_t sa = peek(0x01D2), sb = peek(0x01D4);
	T.r[6] = (uint16_t)(T.r[6] - 0x000C);                // SUB #0xC,SP: locals, banks saved there
	ww((uint16_t)(T.r[6] + 0x000A), sa);
	ww((uint16_t)(T.r[6] + 0x0008), sb);
	set_banks(0x1800, 0x1C00);
	T.r[0] = 0x1C00;
	call_from(0xEB04, 0x8068, {});                                  // select_video_bank0
	cy(483);                                           // F:EB04-EB48
	const uint16_t slot = (uint16_t)(a.sp + 4);
	uint16_t r3 = peek(peek(slot));
	poke(slot, (uint16_t)(peek(slot) + 2));
	uint16_t r2 = peek(peek(slot)) & (uint16_t)~0x007F;
	uint16_t r1 = peek(peek(slot)) & 0x007E;
	poke(slot, (uint16_t)(peek(slot) + 2));
	uint16_t rows = peek(peek(slot));
	poke(slot, (uint16_t)(peek(slot) + 2));
	const uint16_t cols = peek(peek(slot));
	uint16_t r0 = 0;
	poke(0x0F4C, 0);
	const uint16_t back = (uint16_t)(cols << 1);
	if ((int16_t)rows > 0)
		do
		{
			uint16_t n = cols;
			if ((int16_t)n > 0)
				do
				{
					int cycles;                            // the original pass F:EB50-EBF2
					if (r0 == 0)
					{
						const uint8_t head = peekb(r3++);
						cycles = run_head_cycles(head);
						pokeb(0x0F5F, head);
						r0 = sext8(head) & 0x003F;
						pokeb(0x0F4C, peekb(r3++));
						const uint8_t mode = run_mode(peekb(0x0F5F));
						pokeb(0x0F5F, mode);
						if (mode == 2) pokeb(0x0F4D, peekb(r3++));
					}
					else
					{
						cycles = run_next_cycles(peekb(0x0F5F));
						switch (peekb(0x0F5F))
						{
						case 0: pokeb(0x0F4C, peekb(r3++)); break;
						case 1: break;
						case 2: pokeb(0x0F4C, peekb(r3++)); pokeb(0x0F4D, peekb(r3++)); break;
						case 3: poke(0x0F4C, (uint16_t)(peek(0x0F4C) + 1)); break;
						default: break;                    // not set by the run heads
						}
					}
					cycles += 72 + 63;
					if ((peek(0x0F4C) & 0x03FF) != 0x0149)
					{
						const uint16_t at = (uint16_t)((r1 & 0x00FF) | r2);
						if (!a.u(0))
						{
							cycles += 39 + 120;
							poke(0x0F4C, (uint16_t)(peek(0x0F4C) | peek(0x0188)));
							poke(at, peek(0x0F4C));
						}
						else
						{
							cycles += 39 + 57;
							poke(at, 0);
						}
					}
					r1 = (uint16_t)(r1 + 2);
					r0 = (uint16_t)(r0 - 1);
					charge(0xEB50, cycles);                  // interrupts may come in between tiles
				} while (--n != 0);
			r1 = (uint16_t)(r1 - back);
			r2 = (uint16_t)(r2 + 0x0080);
			charge(0xEB4A, 48 + 87);                         // row: F:EB4A-EB4E, F:EBF4-EC00
		} while (--rows != 0);
	cy(201);                                           // F:EC02-EC1E
	restore_banks(sa, sb);
	return r0;
}

// F:EC20  draw_map_object(int i): object record i at 0x4B58 (bank 0x8000): {type, shape,
// column, row}.  The shape gives the size (0x4BCB columns x 0x4BC0 rows), the bank base
// (0x4BD6) and the frame table (0x4BE2); the frame comes from the type: 0-3 F:90BA with
// 0x027A = type, 4-7 the ticket count 0x0258[type] (0-2), 8-11 0xFE7C[0x0F31] - 1; objects
// of types 4-11 whose ticket bit (0x0ED7) is set are not drawn.  The frame's tiles are drawn
// into the playfield at the map position (0x0F42/0x0F45), keeping the colour bits, inside
// the playfield half of 0x0F42; zero tiles are skipped (the column does not advance).
uint16_t draw_map_object(const Args &a)
{
	At here(0xEC20);
	Helper h(F162);
	cy(420);                                           // F:EC24-EC5C
	T.r[6] = (uint16_t)(T.r[6] - 8);                   // CMP -(SP),-(SP) and two pushed sizes
	poke(0x01D2, 0x8000);
	ww(0x1400, 0x8000);
	uint16_t r0 = (uint16_t)((uint16_t)(a.u(0) << 2) + 0x4B58);
	uint16_t r3 = r0;
	const uint16_t type = peekb(r3++);
	uint16_t r1 = peekb(r3);
	uint16_t rows = peekb((uint16_t)(0x4BC0 + r1));
	const uint16_t cols = peekb((uint16_t)(0x4BCB + r1));
	uint16_t r5;
	if (peekb((uint16_t)(0x4BD6 + r1)))
	{
		cy(18);
		r5 = 0x6000;
	}
	else
	{
		cy(30);
		r5 = 0x4000;
	}
	cy(324);                                           // F:EC68-EC96
	r0 = (uint16_t)(r1 << 1);
	r1 = peek((uint16_t)(0x4BE2 + r0));
	r3++;
	r0 = (uint16_t)((uint16_t)(peekb(r3++) << 1) + peekb(0x0F45));
	const uint16_t col0 = r0;
	r0 = (uint16_t)((uint16_t)(peekb(r3) << 8) + peek(0x0F42));
	uint16_t r2 = r0;
	r0 = (uint16_t)(type << 1);
	if (type <= 3)
	{
		cy(129);
		const uint8_t saved = peekb(0x027A);
		pokeb(0x027A, (uint8_t)type);
		T.r[0] = r0; T.r[1] = r1; T.r[2] = r2; T.r[3] = r3; T.r[4] = type; T.r[5] = r5;
		r0 = call_from(0xECAC, 0x90BA, {});
		cy(78);
		r3 = (uint16_t)(r0 - 1);
		pokeb(0x027A, saved);
	}
	else if (type <= 7)
	{
		cy(51);
		if (peekb(0x0ED7) & peekb((uint16_t)(0xFC70 + type))) return cy(39), r0;
		cy(39);
		r3 = sext8(peekb((uint16_t)(0x0258 + type)));
		if ((int16_t)r3 < 0)
			cy(24);
		else
		{
			cy(30);
			if ((int16_t)r3 >= 3) cy(24);
		}
		if ((int16_t)r3 < 0 || (int16_t)r3 >= 3) r3 = 0;
	}
	else if (type <= 11)
	{
		cy(51);
		if (peekb(0x0ED7) & peekb((uint16_t)(0xFC6C + type))) return cy(39), r0;
		cy(87);
		r0 = peekb(0x0F31);
		r3 = (uint16_t)(peekb((uint16_t)(0xFE7C + r0)) - 1);
	}
	else
		return cy(39), r0;                             // the jump table has no entries for these
	cy(393);                                           // F:ECE6-ED28
	r0 = (uint16_t)((uint16_t)(r3 << 1) + r1);
	r3 = peek((uint16_t)(0x4000 + r0));
	r0 = (uint16_t)(((int16_t)r3 >> 3) & 0x1C00);
	r5 = (uint16_t)(r0 + r5);
	poke(0x01D2, r5);
	poke(0x01D4, (uint16_t)(r5 + 0x0400));
	ww(0x1400, peek(0x01D2));
	ww(0x1402, peek(0x01D4));
	r3 = (uint16_t)((r3 & 0x1FFF) + 0x4000);
	const uint16_t half = peek(0x0F42) & 0x0800;
	if (half)
	{
		cy(48);
		r2 = (uint16_t)(r2 - 0x0800);
		r0 = (uint16_t)(cols << 1);
		for (;;)
		{
			cy(30);
			if (r2 & 0x0800) break;
			cy(72);
			r2 = (uint16_t)(r2 + 0x0100);
			rows--;
			r3 = (uint16_t)(r3 + r0);
		}
	}
	cy(42);
	T.r[0] = r2;
	call_from(0xED4A, 0x8078, {});                                  // select_video_bank
	cy(99);
	r0 = r2 & 0x1F00;
	r2 = (uint16_t)(r0 + 0x2000);
	if ((int16_t)rows > 0)
		do
		{
			cy(54);
			if ((r2 & 0x0800) != half) break;
			cy(75);
			r1 = col0;
			uint16_t n = cols;
			if ((int16_t)n > 0)
				do
				{
					int c = 30 + 42;
					r0 = peek(r3);
					r3 = (uint16_t)(r3 + 2);
					if (r0)
					{
						c += 30 + 159;
						if ((int16_t)r0 >= 0x0800)
						{
							c += 36;
							r0 = (uint16_t)((r0 & 0x03FF) + 0x0400);
						}
						const uint16_t at = (uint16_t)((r1 & 0x00FF) | r2);
						r0 |= peek(at) & 0xF800;
						poke(at, r0);
						r1 = (uint16_t)(r1 + 2);
					}
					charge(0xED70, c);
				} while (--n != 0);
			r2 = (uint16_t)(r2 + 0x0100);
			cy(60);
		} while (--rows != 0);
	cy(39);
	return r0;
}

// F:E82E (registers): unpack the 8 x 16 tile block of the map cell 0x0F48 into the playfield
// at 0x0F42 (row 0x0F43, column 0x0F45; video bank by F:8078).  The cell's block number is in
// the level's map (bank base 0xFEB2[level]); the block itself is in the bank from 0xFEA2
// [level] (0x6000 for the town's alternate cells flagged at 0x0F56 / 0x0F58, 0xA000 for odd
// blocks in parks 1-4).  The block's first byte (bit 7: it has objects) or, in the right
// half of the playfield, its byte 1 is the cell's terrain byte (0x0F60 + row / column); byte
// 2 then skips ahead.  Tiles 0-15 with a non-zero entry in the tile table at 0x0F4E become
// that entry plus a random variant (0xFEE2, index 0x0FA0).  The objects are listed backwards
// before the block and drawn by F:EC20.  The banks end as 0x1800 / 0x3C00.
void unpack_map_block()
{
	At here(0xE82E);
	uint16_t *r = T.r;
	const uint16_t sp0 = r[6];
	cy(48);
	r[6] = (uint16_t)(sp0 - 0x000A);                    // SUB #0xA,SP: locals
	call_from(0xE836, 0x9152, {});                                  // next_random
	cy(105);
	r[0] = (uint16_t)(r[0] + peek(0x0FA0));
	poke(0x0FA0, r[0]);
	r[0] = peek(0x0F42);
	call_from(0xE846, 0x8078, {});                                  // select_video_bank
	cy(522);                                           // F:E846-E89A
	r[4] = peek(0x0F48);
	r[0] = (uint16_t)(((int16_t)r[4] >> 3) & 0x1C00);
	r[5] = peekb(0x0F59);
	r[3] = (uint16_t)(r[5] << 1);
	r[0] = (uint16_t)(r[0] + peek((uint16_t)(0xFEB2 + r[3])));
	r[2] = r[0];
	poke(0x01D2, r[2]);
	r[0] = (uint16_t)(r[0] + 0x0400);
	poke(0x01D4, r[0]);
	ww(0x1400, peek(0x01D2));
	ww(0x1402, peek(0x01D4));
	r[4] = (uint16_t)((r[4] & 0x1FFF) + 0x4000);
	r[4] = peek(r[4]);
	r[1] = 0x2C00;
	poke(0x01D4, 0x2C00);
	ww(0x1402, 0x2C00);
	if (peekb(peek(0x0F56)) & peekb(0x0F58))
	{
		cy(18);
		r[2] = g_patch_town_alt_base;                  // F:E8B6
	}
	else
	{
		cy(51);
		r[2] = peek((uint16_t)(0xFEA2 + r[3]));
		if ((int16_t)r[5] > 0)
		{
			cy(30);
			if ((int16_t)r[5] <= 4)
			{
				cy(30);
				if (r[4] & 1)
				{
					cy(30);
					r[2] = g_patch_park_odd_base;      // F:E8B0
				}
			}
		}
	}
	cy(54);
	r[0] = peek(0x0F42);
	call_from(0xE8C2, 0x8078, {});
	cy(348);                                           // F:E8C2-E8FA
	r[0] = (uint16_t)((((int16_t)r[4] >> 3) & 0x1C00) + r[2]);
	r[2] = r[0];
	poke(0x01D2, r[2]);
	r[0] = (uint16_t)(r[0] + 0x0400);
	poke(0x01D4, r[0]);
	ww(0x1400, peek(0x01D2));
	ww(0x1402, peek(0x01D4));
	r[4] = (uint16_t)((r[4] & 0x1FFF) + 0x4000);
	r[0] = sext8(peekb(r[4]));
	const bool objects = (r[0] & 0x80) != 0;
	if (objects) cy(90);
	const uint16_t block = r[4], bank = r[2];
	cy(153);                                           // F:E908-E920
	pokeb(0x0F51, peekb(0x0F43));
	pokeb(0x0F51, (uint8_t)(peekb(0x0F51) | 0x20));
	pokeb(0x0F50, peekb(0x0F45));
	if (peek(0x0F42) & 0x0800)
	{
		cy(102);
		r[1] = peekb((uint16_t)(r[4] + 1));
		r[3] = peekb((uint16_t)(r[4] + 2));
		r[4] = (uint16_t)(r[4] + r[3]);
	}
	else
	{
		cy(36);
		r[1] = sext8(peekb(r[4])) & 0x007F;
	}
	cy(324);                                           // F:E938-E96A
	r[0] = (uint16_t)(peekb(0x0F45) >> 3);
	r[5] = sext8((uint8_t)(peek(0x0F42) >> 8)) & 0x0038;
	r[0] = (uint16_t)(r[0] >> 2);
	r[5] = (uint16_t)(r[5] + r[0]);
	pokeb((uint16_t)(0x0F60 + r[5]), (uint8_t)r[1]);
	r[3] = 0;
	poke(0x0F4C, 0);
	r[2] = 0;
	r[0] = 0;
	r[4] = (uint16_t)(r[4] + 3);
	for (int row = 8; row > 0; row--)
	{
		for (int col = 16; col > 0; col--)
		{
			bool repeat = false;                       // mode 1: the same tile again
			int cycles;                                // the original pass F:E974-EA46
			if (r[3] == 0)
			{
				const uint8_t head = peekb(r[4]++);
				cycles = run_head_cycles(head);
				r[3] = sext8(head);
				pokeb(0x0F5F, head);
				r[3] &= 0x003F;
				pokeb(0x0F4C, peekb(r[4]++));
				r[1] = sext8(peekb(0x0F5F));
				r[5] = (r[1] & 0x40) ? 1 : 0;
				if (!(r[1] & 0x80)) r[5] = 0;          // ROR R5 (C clear after TST R1)
				const uint8_t mode = run_mode((uint8_t)r[1]);
				pokeb(0x0F5F, mode);
				if (mode == 2) pokeb(0x0F4D, peekb(r[4]++));
			}
			else
			{
				r[1] = (uint16_t)(sext8(peekb(0x0F5F)) << 1);
				cycles = run_next_cycles(peekb(0x0F5F));
				switch (peekb(0x0F5F))
				{
				case 0: pokeb(0x0F4C, peekb(r[4]++)); break;
				case 1: repeat = true; break;
				case 2: pokeb(0x0F4C, peekb(r[4]++)); pokeb(0x0F4D, peekb(r[4]++)); break;
				case 3: poke(0x0F4C, (uint16_t)(peek(0x0F4C) + 1)); break;
				default: break;                        // not set by the run heads
				}
			}
			bool plain;
			if (repeat)
			{
				cycles += 24 + 24;
				plain = r[2] == 0;                     // the variant computed last time
			}
			else
			{
				cycles += 12 + 24 + 84;
				r[0] = 0;
				r[2] = peek(0x0F4C) & 0x07FF;
				if ((int16_t)r[2] > 0x000F)
				{
					cycles += 36;
					r[2] = 0;
					plain = true;
				}
				else
				{
					cycles += 78;
					r[1] = (uint16_t)((uint16_t)(r[2] << 1) + peek(0x0F4E));
					r[2] = peek(r[1]);
					if (r[2] == 0)
						plain = true;
					else
					{
						cycles += 78;
						r[1] = peek(0x0F4C) & 0xF800;
						r[2] |= r[1];
						plain = false;
					}
				}
			}
			cycles += (plain ? 24 : 171) + 144;
			if (plain)
				r[1] = peek(0x0F4C);
			else
			{
				r[1] = (uint16_t)(peek(0x0FA0) + 1);
				poke(0x0FA0, r[1]);
				poke(0x0FA0, peek(0x0FA0) & 0x001F);
				r[5] = peek(0x0FA0);
				r[1] = (uint16_t)(sext8(peekb((uint16_t)(0xFEE2 + r[5]))) + r[2]);
			}
			poke(peek(0x0F50), r[1]);
			pokeb(0x0F50, (uint8_t)(peekb(0x0F50) + 1));
			pokeb(0x0F50, (uint8_t)(peekb(0x0F50) + 1));
			r[3] = (uint16_t)(r[3] - 1);
			charge(0xE974, cycles);                      // interrupts may come in between tiles
		}
		r[1] = (uint16_t)(peekb(0x0F50) - 0x0020);
		pokeb(0x0F50, (uint8_t)r[1]);
		pokeb(0x0F51, (uint8_t)(peekb(0x0F51) + 1));
		charge(0xE96E, 36 + 141);                        // row: F:E96E, F:EA48-EA5C
	}
	cy(45);                                            // F:EA5E-EA64
	if (objects)
	{
		cy(273);
		r[4] = (uint16_t)(block + 0x1FFF);
		const uint16_t ba = (uint16_t)(bank - 0x0400);
		poke(0x01D2, ba);
		r[1] = (uint16_t)(ba + 0x0400);
		poke(0x01D4, r[1]);
		ww(0x1400, peek(0x01D2));
		ww(0x1402, peek(0x01D4));
		do
		{
			r[0] = sext8(peekb(r[4]));
			cy(72);
			call_from(0xEA9A, 0xEC20, {r[0]});                      // draw_map_object
			poke(0x01D2, ba);
			poke(0x01D4, r[1]);
			ww(0x1400, peek(0x01D2));
			ww(0x1402, peek(0x01D4));
			r[4] = (uint16_t)(r[4] - 1);
			charge(0xEA92, 198);                         // rest of the pass F:EA9A-EAB4
		} while (!(peekb(r[4]) & 0x80));
	}
	cy(183);                                           // F:EAB6-EAD2
	r[0] = 0x1800;
	poke(0x01D2, 0x1800);
	ww(0x1400, 0x1800);
	r[0] = 0x3C00;
	poke(0x01D4, 0x3C00);
	ww(0x1402, 0x3C00);
	r[6] = sp0;
	fl::add((uint16_t)(sp0 - 0x000A), 0x000A);          // ADD #0xA,SP
}

// ---------------------------------------------------------------- round 2

uint16_t asr(uint16_t v, int n) { return (uint16_t)((int16_t)v >> n); }

// MTPS: priority and condition codes (not the T bit); a pending interrupt is taken at the next
// charge, as after the instruction in the original
void mtps(uint8_t v)
{
	T.psw = (uint8_t)((T.psw & ~0xEF) | (v & 0xEF));
	T.check_irqs = true;
}

// F:D052  spawn_level_objects(): in the town (0x0F59 = 1) the map objects of the park level
// (F:90BA; table 0xFE8F) are added: for each pair of words (x, y) from 0x6738[level] up to
// 0x672C[level] (bank B 0x0400) F:CE24 add_map_object(1, y, x) and add_map_object(1, y, 0x19);
// 0x0E95 counts them, 0x0E96 = 0x18, the first slot 0x0E98 = 0x09AC = 0xAF.  Banks restored.
uint16_t spawn_level_objects(const Args &)
{
	At here(0xD052);
	Helper h(F152);
	cy(24 + 24 + 33 + 30 + 12);                        // F:D056-D06A
	T.r[3] = peek(0x01D2);
	T.r[4] = peek(0x01D4);
	pokeb(0x0E98, 0xAF);
	if (peekb(0x0F59) == 1)
	{
		cy(27 + 27 + 18 + 27 + 27 + 33 + 27 + 27 + 39 + 30);   // F:D06C-D094
		poke(0x01D2, 0);
		ww(0x1400, 0);
		T.r[0] = 0x0400;
		poke(0x01D4, 0x0400);
		ww(0x1402, 0x0400);
		pokeb(0x0E96, 0x18);
		pokeb(0x0E95, 0);
		poke(0x09AC, 0);
		pokeb(0x09AC, peekb(0x0E98));
		call_from(0xD098, 0x90BA, {});
		cy(12 + 27 + 12 + 12 + 27 + 27);               // F:D098-D0A6
		T.r[2] = peekb((uint16_t)(0xFE8F + T.r[0]));
		T.r[0] = (uint16_t)(T.r[2] << 1);
		T.r[1] = peek((uint16_t)(0x672C + T.r[0]));
		T.r[2] = peek((uint16_t)(0x6738 + T.r[0]));
		do
		{
			charge(0xD0AA, 24 + 12 + 27 + 30 + 39 + 30 + 30);   // F:D0AA-D0BE
			T.r[0] = (uint16_t)(sext8(peekb(0x0E95)) + 1);
			pokeb(0x0E95, (uint8_t)T.r[0]);
			push(peek(T.r[2]));
			push(peek((uint16_t)(T.r[2] + 2)));
			push(0x0001);
			call_from(0xD0C2, 0xCE24, {});
			cy(27 + 39 + 30 + 30);                     // F:D0C2-D0CE
			poke(T.r[6], 0x0019);
			push(peek((uint16_t)(T.r[2] + 2)));
			push(0x0001);
			call_from(0xD0D2, 0xCE24, {});
			cy(18 + 18 + 12 + 12);                     // F:D0D2-D0DC
			T.r[2] = (uint16_t)(T.r[2] + 4);
			T.r[6] = (uint16_t)(T.r[6] + 0x000A);
		} while (T.r[2] != T.r[1]);
	}
	cy(27 + 27 + 39 + 39 + 21);                        // F:D0DE-D0F2
	restore_banks(T.r[3], T.r[4]);
	return T.r[0];
}

// F:D632 (registers): the skater on object 0x0EA0.  It is let go when bit 6 is set and F:D838
// finds it facing the object, or when the copy 0x0EA1 is clear: 0x0304 = 0x18, height 0x06B0
// = 0, F:D778, F:9FB4 (take off, 0x02E8 cleared) when 0x01EA is set, F:A090 (touch down)
// when 0x06B2 <= 0x06B0, and 0x0EA0 is cleared.  Otherwise, with bit 6 clear, F:D4E4 locates
// it: inside the shape (column below 0x12, height not negative) the shape's byte there (bank
// B 0x2800, low 7 bits) goes to 0x0304 unless it is 0 or has bits 0-2 all set (0xF0 then).
void on_object_0ea0()
{
	At here(0xD632);
	cy(30 + 12);                                       // BIT #0x40,@#0x0EA0; BEQ
	uint16_t bit = peek(0x0EA0) & 0x0040;
	fl::mov(bit);
	bool let_go = false, tested = false;
	if (bit)
	{
		cy(30);
		call_from(0xD63E, 0xD838, {});
		cy(12 + 12);                                   // ROR R0; BCC
		const bool c = (T.r[0] & 1) != 0;
		T.r[0] = (uint16_t)((fC() ? 0x8000 : 0) | (T.r[0] >> 1));
		f_nzvc(nzw(T.r[0]) | (c ? CF : 0));
		f_shift();
		let_go = !c;
		tested = let_go;
	}
	if (!tested)
	{
		cy(24 + 12);                                   // TSTB @#0x0EA1; BNE
		const uint8_t copy = peekb(0x0EA1);
		fl::tstb(copy);
		let_go = copy == 0;
	}
	if (let_go)
	{
		cy(33 + 27 + 30);                              // F:D648-D652
		poke(0x0304, 0x0018);
		poke(0x06B0, 0);
		call_from(0xD656, 0xD778, {});
		cy(24 + 12);
		if (peekb(0x01EA))
		{
			cy(27 + 30);
			poke(0x02E8, 0);
			call_from(0xD664, 0x9FB4, {});
		}
		cy(36 + 12);
		if ((int16_t)peek(0x06B2) <= (int16_t)peek(0x06B0))
		{
			cy(30);
			call_from(0xD670, 0xA090, {});
		}
		cy(27 + 21);
		pokeb(0x0EA0, 0);
		fl::clr();
		return;
	}
	cy(30 + 12);                                       // F:D676 BIT #0x40,@#0x0EA0; BNE
	bit = peek(0x0EA0) & 0x0040;
	fl::mov(bit);
	if (bit)
	{
		cy(21);
		return;
	}
	cy(30);
	call_from(0xD682, 0xD4E4, {});
	cy(18 + 12);                                       // CMP R4,#0x12; BCC
	fl::cmp(T.r[4], 0x0012);
	bool inside = false;
	if (T.r[4] < 0x0012)
	{
		cy(12 + 12);                                   // TST R3; BGE
		fl::tst(T.r[3]);
		inside = (int16_t)T.r[3] >= 0;
	}
	if (!inside)
	{
		cy(12 + 12);                                   // CLR R1; BR
		T.r[1] = 0;
		fl::clr();
	}
	else
	{
		cy(18 + 27 + 27 + 12 + 12 + 18 + 18);          // F:D690-D6A2
		T.r[0] = 0x2800;
		poke(0x01D4, 0x2800);
		ww(0x1402, 0x2800);
		T.r[5] = fl::add(T.r[5], T.r[3]);
		T.r[5] = fl::add(T.r[5], T.r[4]);
		T.r[1] = sext8(peekb(T.r[5])) & 0x007F;
		fl::mov(T.r[1]);                               // BIC #0xFF80,R1 (C from the ADD)
	}
	cy(12);                                            // F:D6A6 BEQ
	if (T.r[1])
	{
		cy(18 + 12 + 12);                              // MOV #7,R0; BIC R1,R0; BEQ
		T.r[0] = 7 & (uint16_t)~T.r[1];
		fl::mov(T.r[0]);
		if (T.r[0])
		{
			cy(27 + 21);
			poke(0x0304, T.r[1]);
			fl::mov(T.r[1]);
			return;
		}
	}
	cy(33 + 21);
	poke(0x0304, 0x00F0);
	fl::mov(0x00F0);
}

// F:D6BE  slope_of_object_0ea0(): unless 0x0EA0 has bit 6 set, the speed 0x05B0 changes by
// the slope of the object under the skater: 0x79A8[(0x0327 & 3) * 8 + 0x65CC[type]] (bank B
// 0x2C00; the type's class from bank B 0x0400), negated or dropped as the table 0x79F8[the
// direction bits 3-4 of 0x0327 / 2 + 0x07D0[obj] / 4] says (-1 / 0 / 1, through the jump
// offsets at 0xFDF8).  Leaves bank A 0 and bank B 0x2C00 selected; returns the change.
uint16_t slope_of_object_0ea0(const Args &)
{
	At here(0xD6BE);
	Helper h(F138);
	cy(24 + 18 + 12);                                  // F:D6C2-D6CA
	T.r[2] = sext8(peekb(0x0EA0));
	if (T.r[2] & 0x0040)
	{
		cy(21);
		fl::mov(T.r[2] & 0x0040);                      // BIT #0x40,R2
		return T.r[0];
	}
	cy(636);                                           // F:D6CC-D732
	poke(0x01D2, 0);
	ww(0x1400, 0);
	poke(0x01D4, 0x0400);
	ww(0x1402, 0x0400);
	uint16_t r0 = peekb((uint16_t)(0x0370 + T.r[2]));
	r0 = peekb((uint16_t)(0x65CC + r0));
	poke(0x01D4, 0x2C00);
	ww(0x1402, 0x2C00);
	uint16_t r1 = (uint16_t)(((((uint16_t)sext8(peekb(0x0327)) & 3) << 3) + r0) << 1);
	r0 = peek((uint16_t)(0x79A8 + r1));
	r1 = (uint16_t)((peekb(0x0327) & 0x18) >> 1);
	T.r[2] = (uint16_t)(peekb((uint16_t)(0x07D0 + T.r[2])) >> 2);
	r1 = (uint16_t)(r1 + T.r[2]);
	r1 = (uint16_t)((sext8(peekb((uint16_t)(0x79F8 + r1))) + 1) << 1);
	T.r[1] = r1;
	// ADD 0xFDF8(R1),PC
	const uint16_t target = (uint16_t)(0xD736 + peek((uint16_t)(0xFDF8 + r1)));
	switch (target)
	{
	case 0xD736:                                       // NEG R0; BR
		cy(12 + 12);
		r0 = (uint16_t)-r0;
		break;
	case 0xD738:                                       // BR
		cy(12);
		break;
	case 0xD73A:                                       // CLR R0
		cy(12);
		r0 = 0;
		break;
	case 0xD73C:
		break;
	default:
		fatal_unknown_pc(target);
	}
	cy(27 + 21);
	T.r[0] = r0;
	poke(0x05B0, fl::add(peek(0x05B0), r0));
	return r0;
}

// F:D742 (registers): the skater moves on object 0x0EA0: with banks 0x3800 / 0x3C00, A7:4A66,
// F:9B82 (motion parameters) and A7:410C (projected speed) with the speed 0x05B2 held at 0.
// R1 is preserved.
void move_on_object_0ea0()
{
	At here(0xD742);
	cy(198);                                           // F:D742-D75C
	push(T.r[1]);
	set_banks(0x3800, 0x3C00);
	T.r[0] = 0x3C00;
	call_from(0xD760, 0x4A66, {});
	cy(30);
	call_from(0xD764, 0x9B82, {});
	cy(24 + 27 + 30);
	T.r[1] = peek(0x05B2);
	poke(0x05B2, 0);
	call_from(0xD770, 0x410C, {});
	cy(27 + 18 + 21);
	poke(0x05B2, T.r[1]);
	T.r[1] = pop();
	fl::mov(T.r[1]);
}

// F:D778 (registers): object 0x0EA0 follows the skater's height (0x0670 = 0x0730 + 0x01AA +
// 0x06B0) and its piece count is queued (F:B602 queue_piece_count(type, obj)); banks 0 / 0x0400.
void update_object_0ea0()
{
	At here(0xD778);
	cy(381);                                           // F:D778-D7AC
	poke(0x01D2, 0);
	ww(0x1400, 0);
	T.r[0] = 0x0400;
	poke(0x01D4, 0x0400);
	ww(0x1402, 0x0400);
	poke(0x0670, (uint16_t)(peek(0x0730) + peek(0x01AA) + peek(0x06B0)));
	T.r[0] = peek(0x0EA0) & 0x003F;
	push(T.r[0]);
	push(0);
	pokeb(T.r[6], peekb((uint16_t)(0x0370 + T.r[0])));
	call_from(0xD7B0, 0xB602, {});
	cy(24 + 21);
	const uint16_t a = pop(), b = pop();
	fl::cmp(a, b);                                     // CMP (SP)+,(SP)+
}

// F:D838  facing_object_0ea0(): 1 when the skater is on the ground (0x01EA) and its direction
// 0x031D (turned by 8 when the speed 0x01B4 is negative) is within 2 of the object's 0x07D0
// [0x0EA1] (of 16 directions), else 0.  The carry is left as the original's (F:D632 uses it).
uint16_t facing_object_0ea0(const Args &)
{
	At here(0xD838);
	Helper h(F138);
	cy(24 + 12);
	if (!peekb(0x01EA))
	{
		cy(12 + 21);
		fl::clr();
		return 0;
	}
	cy(12 + 24 + 24 + 12);                             // F:D842-D84C
	uint16_t r1 = peekb(0x031D);
	if ((int16_t)peek(0x01B4) < 0)
	{
		cy(18 + 12);
		r1 ^= 0x0008;
	}
	cy(24 + 12 + 27 + 12 + 12);                        // F:D854-D860
	const uint16_t r0 = sext8(peekb(0x0EA1));
	r1 = (uint16_t)(r1 - peekb((uint16_t)(0x07D0 + r0)));
	if ((int16_t)r1 < 0)
	{
		cy(12);
		r1 = (uint16_t)-r1;
	}
	cy(18 + 12);
	if ((int16_t)r1 > 8)
	{
		cy(12 + 18);
		r1 = (uint16_t)(0x0010 - r1);
	}
	cy(18 + 12);
	fl::cmp(r1, 0x0002);
	if ((int16_t)r1 > 2)
	{
		cy(12 + 21);
		fl::clr();
		return 0;
	}
	cy(18 + 21);
	fl::mov(1);
	return 1;
}

// F:E730  swap_players(): the 0x3E bytes of player state at 0x023E change places with those
// at 0x027C and the player 0x01F5 toggles; after the next vertical blank (F:8D50) 0x0FB4-0x0FD3
// are cleared and the sound CPU is told: command 12, F:8F50, command player + 1, between
// F:8E6A and F:8E72 (flag 0x01A4) and F:8F62.
uint16_t swap_players(const Args &)
{
	At here(0xE730);
	Helper h(F138);
	cy(18 + 18);                                       // F:E734-E738
	for (uint16_t a = 0x023E; a != 0x027C; a++)
	{
		cy(18 + 27 + 21 + 18 + 12);                    // pass F:E73C-E746
		T.r[0] = sext8(peekb(a));
		pokeb(a, peekb((uint16_t)(a + 0x3E)));
		pokeb((uint16_t)(a + 0x3E), (uint8_t)T.r[0]);
	}
	cy(18 + 12 + 24 + 12 + 27 + 30);                   // F:E748-E758
	T.r[0] = peekb(0x01F5);
	T.r[1] = (uint16_t)(T.r[0] ^ 1);
	T.r[2] = 0x027C;
	pokeb(0x01F5, (uint8_t)T.r[1]);
	call_from(0xE75C, 0x8D50, {});
	cy(18);
	for (T.r[0] = 0x0020; T.r[0]; T.r[0]--)
	{
		cy(30 + 18);                                   // pass F:E760-E764
		pokeb((uint16_t)(0x0FB3 + T.r[0]), 0);
	}
	cy(30);
	call_from(0xE76A, 0x8E6A, {});
	cy(30 + 30);
	push(0x000C);
	call_from(0xE772, 0x8EA2, {});
	cy(30);
	call_from(0xE776, 0x8F50, {});
	cy(21 + 33 + 21 + 30);                             // F:E776-E77E
	poke(T.r[6], (uint16_t)(peekb(0x01F5) + 1));
	call_from(0xE782, 0x8EA2, {});
	cy(30);
	call_from(0xE786, 0x8F62, {});
	cy(30);
	call_from(0xE78A, 0x8E72, {});
	cy(18 + 21);
	fl::tst(pop());
	return T.r[0];
}

// F:EDB2  show_screen_objects(n): the palettes are loaded over two vertical blanks (jobs
// F:DC3C 0x10 / 0, 0x11 / 2 in 0x0EB0 / 0x0EAC / 0x0EAE) and a third: block 0x0F (0 with
// n, else 1) or, for one of the players in a game (0x0F2B 1 or 2, 0x018F not 5), F:DABA.
// Then A?:4000(0, 3, n + 5) in bank A 0x1800, 0x01AE / 0x01AC = 0 / 0x2000, and the objects
// 0..0xFF2A[n] are set up from the tables at 0x7D94 (bank B 0x0400: type, x, y, z; frames 3
// for the first four, 4 after; flags 0x80 / 0x90) and shown (F:B4B6); F:8270 installs the
// 32V vectors and banks 0x3800 / 0x3C00 are selected.
uint16_t show_screen_objects(const Args &a)
{
	At here(0xEDB2);
	Helper h(F144);
	cy(33 + 33 + 27 + 30);                             // F:EDB6-EDC6
	poke(0x0EB0, 0xDC3C);
	poke(0x0EAC, 0x0010);
	poke(0x0EAE, 0);
	call_from(0xEDCA, 0x8D50, {});
	cy(33 + 33 + 33 + 30);                             // F:EDCA-EDDC
	poke(0x0EB0, 0xDC3C);
	poke(0x0EAC, 0x0011);
	poke(0x0EAE, 0x0002);
	call_from(0xEDE0, 0x8D50, {});
	cy(27 + 12);
	T.r[1] = a.u(0);
	if (T.r[1])
	{
		cy(33 + 27 + 12 + 33 + 30);                    // F:EDE6-EDF0, EE1E-EE24
		poke(0x0EAC, 0x000F);
		poke(0x0EAE, 0);
		poke(0x0EB0, 0xDC3C);
	}
	else
	{
		cy(30 + 12);
		bool other = peekb(0x0F2B) != 1;
		if (other)
		{
			cy(30 + 12);
			other = peekb(0x0F2B) != 2;
		}
		if (!other)
		{
			cy(30 + 12);
			other = peekb(0x018F) == 5;
		}
		if (!other)
		{
			cy(33 + 12 + 30);                          // F:EE0A-EE10, EE24
			poke(0x0EB0, 0xDABA);
		}
		else
		{
			cy(33 + 33 + 33 + 30);                     // F:EE12-EE24
			poke(0x0EAC, 0x000F);
			poke(0x0EAE, 0x0001);
			poke(0x0EB0, 0xDC3C);
		}
	}
	call_from(0xEE28, 0x8D50, {});
	cy(18 + 27 + 27 + 24 + 27 + 30 + 24 + 30);         // F:EE28-EE40
	T.r[0] = 0x1800;
	poke(0x01D2, 0x1800);
	ww(0x1400, 0x1800);
	push((uint16_t)(T.r[1] + 5));
	push(0x0003);
	push(0);
	call_from(0xEE44, 0x4000, {});
	cy(27 + 33 + 27 + 27 + 18 + 27 + 27 + 27 + 18 + 12 + 12);   // F:EE44-EE6C
	poke(0x01AE, 0);
	poke(0x01AC, 0x2000);
	poke(0x01D2, 0);
	ww(0x1400, 0);
	T.r[0] = 0x0400;
	poke(0x01D4, 0x0400);
	ww(0x1402, 0x0400);
	T.r[1] = peekb((uint16_t)(T.r[1] - 0x00D6));
	T.r[2] = 0;
	for (;;)
	{
		cy(12 + 12);                                   // F:EEC0 CMP R2,R1; BLE
		if ((int16_t)T.r[2] > (int16_t)T.r[1]) break;
		const uint16_t r2 = T.r[2];
		cy(45 + 12 + 12 + 45 + 45 + 45 + 12 + 18 + 12);   // F:EE6E-EE90
		pokeb((uint16_t)(0x0370 + r2), peekb((uint16_t)(0x7D94 + r2)));
		T.r[0] = (uint16_t)(r2 << 1);
		poke((uint16_t)(0x03B0 + T.r[0]), peek((uint16_t)(0x7D9A + T.r[0])));
		poke((uint16_t)(0x06F0 + T.r[0]), peek((uint16_t)(0x7DA4 + T.r[0])));
		poke((uint16_t)(0x0730 + T.r[0]), peek((uint16_t)(0x7DAE + T.r[0])));
		const bool first = (int16_t)r2 < 4;
		const bool c = r2 < 4;                         // carry of CMP R2,#4
		if (first)
		{
			cy(12 + 18 + 12);
			T.r[0] = 3;
		}
		else
		{
			cy(18);
			T.r[0] = 4;
		}
		cy(30 + 30 + 12 + 12);                         // F:EE9E-EEA8
		pokeb((uint16_t)(0x0790 + r2), (uint8_t)T.r[0]);
		pokeb((uint16_t)(0x0770 + r2), (uint8_t)T.r[0]);
		// ROR R3 (R3 = 1 for the first four)
		T.r[3] = (uint16_t)((c ? 0x8000 : 0) | ((first ? 1 : 0) >> 1));
		f_nzvc(nzw(T.r[3]) | (first ? CF : 0));
		f_shift();
		if (first)
		{
			cy(36 + 12);
			pokeb((uint16_t)(0x0470 + r2), 0x80);
		}
		else
		{
			cy(36);
			pokeb((uint16_t)(0x0470 + r2), 0x90);
		}
		cy(21 + 30);
		poke(T.r[6], r2);
		call_from(0xEEBE, 0xB4B6, {});
		cy(12);
		T.r[2]++;
	}
	cy(30);
	call_from(0xEEC8, 0x8270, {});
	cy(18 + 27 + 27 + 18 + 27 + 27 + 18 + 21);         // F:EEC8-EEE4
	T.r[0] = 0x3800;
	poke(0x01D2, 0x3800);
	ww(0x1400, 0x3800);
	T.r[0] = 0x3C00;
	poke(0x01D4, 0x3C00);
	ww(0x1402, 0x3C00);
	T.r[6] = fl::add(T.r[6], 0x0006);
	return T.r[0];
}

// F:F372 (registers): the test switch was turned on in the game: the self test's interrupt
// vectors (F:F3A8), interrupts acknowledged and enabled, 0x01CE / 0x01CC cleared, 0x018A = 1,
// the screens cleared (F:8D3E, F:8CE8) and set up (F:F40C); the self test (F:F43C) does not
// come back.
void enter_selftest()
{
	At here(0xF372);
	cy(30);
	call_from(0xF376, 0xF3A8, {});
	io(27, [] { poke(0x15E0, 0); });                   // acknowledge the interrupts
	io(27, [] { poke(0x15C0, 0); });
	io(27, [] { poke(0x1580, 0); });
	cy(12);
	T.r[0] = 0;
	fl::clr();
	io(30, [] { mtps(0); });
	cy(27);
	poke(0x01CE, 0);
	cy(27);
	poke(0x01CC, 0);
	cy(33);
	poke(0x018A, 0x0001);
	fl::mov(0x0001);
	cy(30);
	call_from(0xF39A, 0x8D3E, {});
	cy(30);
	call_from(0xF39E, 0x8CE8, {});
	cy(30);
	call_from(0xF3A2, 0xF40C, {});
	cy(30);
	call_from(0xF3A6, 0xF43C, {});
	cy(21);
}

// F:F40A: BR . - waits for an interrupt that continues elsewhere (the reset)
void wait_forever()
{
	At here(0xF40A);
	for (;;)
		charge(0xF40A, 12);
}

// F:F43C: the self test's screens, over and over: sound (F:F5D8), controls (F:FB5E), the
// tests in bank B 0x2800 (0x7388, 0x74E4, 0x706A, 0x713E, 0x72B0), the scroll screen (F:F90E)
// and the playfield palette (F:DABA); F:F40C sets the display up again each time.
void selftest_loop()
{
	At here(0xF43C);
	for (;;)
	{
		cy(30);
		call_from(0xF440, 0xF5D8, {});
		cy(30);
		call_from(0xF444, 0xFB5E, {});
		cy(18 + 27 + 27 + 30);
		T.r[0] = 0x2800;
		poke(0x01D4, 0x2800);
		ww(0x1402, 0x2800);
		call_from(0xF454, 0x7388, {});
		cy(30);
		call_from(0xF458, 0x74E4, {});
		cy(30);
		call_from(0xF45C, 0x8D2C, {});
		cy(30);
		call_from(0xF460, 0x706A, {});
		cy(30);
		call_from(0xF464, 0xF90E, {});
		cy(18 + 27 + 27 + 30);
		T.r[0] = 0x2800;
		poke(0x01D4, 0x2800);
		ww(0x1402, 0x2800);
		call_from(0xF474, 0x713E, {});
		cy(30);
		call_from(0xF478, 0xDABA, {});
		cy(30);
		call_from(0xF47C, 0x72B0, {});
		cy(30);
		call_from(0xF480, 0xF40C, {});
		cy(12);                                        // BR F:F43C
	}
}

// F:F482 (registers): "BAD " and the number argument (F:BA9A, three digits) after a failed
// checksum; 0x0FE8 = 1.
void print_bad()
{
	At here(0xF482);
	static const uint8_t text[] = {0x42, 0x41, 0x44, 0x20};
	for (int i = 0; i < 4; i++)
	{
		cy(18 + 30);
		T.r[0] = text[i];
		call_from((uint16_t)(0xF48A + 8 * i), 0x8B52, {});
	}
	cy(39 + 24 + 30 + 30 + 24 + 30);                   // F:F4A2-F4B0
	push(peek((uint16_t)(T.r[6] + 2)));
	push(0);
	push(0x0003);
	push(0x0003);
	push(0);
	call_from(0xF4B4, 0xBA9A, {});
	cy(33 + 18 + 21);
	pokeb(0x0FE8, 1);
	T.r[6] = fl::add(T.r[6], 0x000A);
}

// F:F4D2 (registers): the ROM checksums of the self test.  The fixed ROM 0x8000-0xFFFF (bank
// A 0x3800) adds its even and odd bytes separately: both sums' low bytes must be 0 (message
// 0x0F / 0x14, "BAD n" F:F482 or OK F:F4C0).  Then three groups of eight banks (bank A n,
// bank B n + 0x0400, n = 0, 0x0800, ... 0xB800), 0x4000-0x7FFF each, against the bytes at
// 0xFF3C / 0xFF40 (messages 0x10-0x12 / 0x15-0x17).
void selftest_rom_checksums()
{
	At here(0xF4D2);
	cy(18 + 27 + 27 + 12 + 12 + 18);                   // F:F4D2-F4E2
	T.r[0] = 0x3800;
	poke(0x01D2, 0x3800);
	ww(0x1400, 0x3800);
	uint16_t r0 = 0x8000, r1 = 0, r2 = 0, r3 = 0;
	do
	{
		charge(0xF4E6, 12 + 18 + 12 + 12 + 18 + 12 + 12 + 12);   // pass F:F4E6-F4F4
		r3 = peekb(r0++);
		r2 = (uint16_t)(r2 + r3);
		r3 = peekb(r0++);
		r1 = (uint16_t)(r1 + r3);
	} while (r0);
	T.r[1] = r1;
	T.r[2] = r2;
	T.r[3] = r3;
	// the result of a sum: "BAD n" or OK
	auto verdict = [&](uint16_t bad, uint16_t ret_bad, uint16_t ret_ok) {
		if (bad)
		{
			cy(24 + 30);
			push(T.r[bad]);
			call_from(ret_bad, 0xF482, {});
			cy(18 + 12);
			T.r[6] = (uint16_t)(T.r[6] + 2);
		}
		else
		{
			cy(30);
			call_from(ret_ok, 0xF4C0, {});
		}
	};
	cy(18 + 30);
	T.r[0] = 0x000F;
	call_from(0xF4FE, 0x8AB6, {});
	cy(18 + 12);
	T.r[2] &= 0x00FF;
	verdict(T.r[2] ? 2 : 0, 0xF50A, 0xF512);
	cy(18 + 30);
	T.r[0] = 0x0014;
	call_from(0xF51A, 0x8AB6, {});
	cy(18 + 12);
	T.r[1] &= 0x00FF;
	verdict(T.r[1] ? 1 : 0, 0xF526, 0xF52E);
	cy(12 + 18);
	T.r[5] = 0;
	T.r[3] = 1;
	for (;;)
	{
		cy(12 + 12 + 18);                              // F:F534-F538
		T.r[2] = 0;
		T.r[1] = 0;
		T.r[4] = 8;
		do
		{
			charge(0xF53C, 27 + 12 + 18 + 27 + 39 + 39 + 18);   // F:F53C-F556
			poke(0x01D2, T.r[5]);
			T.r[0] = (uint16_t)(T.r[5] + 0x0400);
			poke(0x01D4, T.r[0]);
			ww(0x1400, peek(0x01D2));
			ww(0x1402, peek(0x01D4));
			T.r[0] = 0x4000;
			do
			{
				charge(0xF55A, 30 + 30 + 18 + 30 + 30 + 18 + 18 + 12);   // pass F:F55A-F56E
				for (int k = 2; k >= 1; k--)           // MOVB (R0)+,-(SP); CLRB 1(SP); ADD (SP)+,Rk
				{
					T.r[6] = (uint16_t)(T.r[6] - 2);
					pokeb(T.r[6], peekb(T.r[0]++));
					pokeb((uint16_t)(T.r[6] + 1), 0);
					T.r[k] = (uint16_t)(T.r[k] + peek(T.r[6]));
					T.r[6] = (uint16_t)(T.r[6] + 2);
				}
			} while (T.r[0] <= 0x7FFE);
			cy(18 + 18);                               // ADD #0x800,R5; SOB
			T.r[5] = (uint16_t)(T.r[5] + 0x0800);
		} while (--T.r[4]);
		cy(18 + 12 + 18 + 30);                         // F:F576-F580
		T.r[2] &= 0x00FF;
		T.r[0] = (uint16_t)(T.r[3] + 0x000F);
		call_from(0xF584, 0x8AB6, {});
		cy(18 + 27 + 27 + 12 + 27 + 12 + 12);          // F:F584-F598
		T.r[0] = 0x3800;
		poke(0x01D2, 0x3800);
		ww(0x1400, 0x3800);
		T.r[0] = peekb((uint16_t)(0xFF3B + T.r[3]));
		verdict(T.r[2] != T.r[0] ? 2 : 0, 0xF5A0, 0xF5A8);
		cy(18 + 12 + 18 + 30);                         // F:F5A8-F5B2
		T.r[1] &= 0x00FF;
		T.r[0] = (uint16_t)(T.r[3] + 0x0014);
		call_from(0xF5B6, 0x8AB6, {});
		cy(12 + 27 + 12 + 12);                         // F:F5B6-F5BE
		T.r[0] = peekb((uint16_t)(0xFF3F + T.r[3]));
		verdict(T.r[1] != T.r[0] ? 1 : 0, 0xF5C6, 0xF5CE);
		cy(12 + 18 + 12);                              // INC R3; CMP R3,#3; BLE
		T.r[3]++;
		fl::cmp(T.r[3], 0x0003);
		if ((int16_t)T.r[3] > 3) break;
	}
	cy(21);
}

// F:F5D8 (registers): the self test's sound screen.  The sound CPU is reset (0x15A0, F:8134)
// at a quiet moment (0x01C4 & 0x3F = 0) until it answers anything but 0xFF, up to ten tries
// (message 0x22 counts them; none: message 0x23, wait for a button).  Its self test result
// (command 9): bits 0-4 set list the failed parts (message 0x28 each, then wait for a
// button).  Otherwise the four test sounds (0xFF48[i], waiting 0xFF4C[i] frames, a button
// ends the screen), then the volume screen: command 8 gives the highest volume, the rotary
// controller (F:8D86 / F:8E2E) sets it (0x30 up, wrapping), shown with F:BA9A, button 1
// plays it, the other button ends; every 8 frames the coin inputs are shown (0x12, 0x40,
// 0x18: message 0x2E / 0xAE + i for bits 0xFF44[i]).
void selftest_sound_screen()
{
	At here(0xF5D8);
	uint16_t &sp = T.r[6];
	auto print = [](uint16_t msg, uint16_t ret) {
		cy(18 + 30);
		T.r[0] = msg;
		call_from(ret, 0x8AB6, {});
	};
	auto number = [&](uint16_t value, uint16_t a, uint16_t b, uint16_t ret) {   // F:BA9A(0, b, a, 0, value)
		cy(36 + 27 + 24 + 30 + 30 + 24 + 30);
		poke(sp, value);
		push(0);
		push(a);
		push(b);
		push(0);
		call_from(ret, 0xBA9A, {});
	};
	auto leave = [&]() {                               // F:F88C
		cy(27 + 18 + 21);
		pokeb(0x0FE6, 0);
		sp = fl::add(sp, 0x000A);
	};
	auto wait_and_leave = [&]() {                      // F:F888
		cy(30);
		call_from(0xF88C, 0x8D60, {});
		leave();
	};
	cy(18 + 30);
	sp = (uint16_t)(sp - 8);
	call_from(0xF5E0, 0x8CE8, {});
	cy(30);
	call_from(0xF5E4, 0x8D2C, {});
	cy(30);
	call_from(0xF5E8, 0x8E2E, {});
	cy(33 + 27);                                       // F:F5E8-F5EE
	poke(0x01AE, 0x0011);
	poke(0x01AC, 0);
	io(33, [] { poke(0x1100, 0x0051); });
	cy(33 + 33);                                       // F:F5F8-F5FE
	poke(0x0188, 0xE000);
	poke(0x0184, 0x5C04);
	print(0x0021, 0xF60C);
	cy(30 + 30);
	push(0x003C);
	call_from(0xF614, 0x8D74, {});
	// frame: (SP) argument, 2(SP) answer, 4(SP) volume, 6(SP) highest volume, 8(SP) tries left
	cy(36);
	poke((uint16_t)(sp + 8), 0x000A);
	for (;;)
	{
		cy(27 + 12);                                   // F:F61A TST 8(SP); BLE
		if ((int16_t)peek((uint16_t)(sp + 8)) <= 0) break;
		cy(30);
		call_from(0xF624, 0x8D50, {});
		cy(30 + 12);
		if (peekb(0x01C4) & 0x3F) continue;
		io(33, [] { poke(0x15A0, 0x0001); });          // the sound CPU held in reset
		cy(30);
		call_from(0xF636, 0x8134, {});
		io(27, [] { poke(0x15A0, 0); });               // and let go
		cy(27 + 30);
		pokeb(0x01A2, 0);
		poke((uint16_t)(sp + 2), 0);
		io(42, [&] { pokeb((uint16_t)(sp + 2), peekb(0x1C00)); });
		cy(30);
		call_from(0xF64C, 0x8ED2, {});
		cy(30 + 18 + 12);
		poke((uint16_t)(sp + 2), T.r[0]);
		if (T.r[0] == 0x00FF)
		{
			cy(36 + 12 + 12);                          // MOV #-1,8(SP); BR F:F61E; BLE
			poke((uint16_t)(sp + 8), 0xFFFF);
			break;
		}
		cy(30);
		poke((uint16_t)(sp + 8), (uint16_t)(peek((uint16_t)(sp + 8)) - 1));
		print(0x0022, 0xF66A);
		number((uint16_t)(0x000A - peek((uint16_t)(sp + 8))), 0x0002, 0x0001, 0xF682);
		cy(18 + 12);
		sp = (uint16_t)(sp + 8);
	}
	cy(27 + 12);                                       // F:F688 TST 8(SP); BNE
	if (!peek((uint16_t)(sp + 8)))
	{
		print(0x0023, 0xF696);
		cy(18);                                        // JMP F:F888
		wait_and_leave();
		return;
	}
	print(0x00A3, 0xF6A2);
	cy(30);
	call_from(0xF6A6, 0x8D50, {});
	cy(30);
	call_from(0xF6AA, 0x8D50, {});
	cy(27 + 30);
	poke(sp, 0x0009);
	call_from(0xF6B2, 0x8EA2, {});
	cy(30);
	call_from(0xF6B6, 0x8ED2, {});
	cy(30 + 30);
	poke((uint16_t)(sp + 2), T.r[0]);
	call_from(0xF6BE, 0x8F62, {});
	print(0x001F, 0xF6C6);
	cy(33 + 12);
	if (peek((uint16_t)(sp + 2)) & 0x001F)
	{
		cy(18 + 18);                                   // JMP F:F86E; MOV #7,R1
		T.r[1] = 7;
		do
		{
			charge(0xF872, 30 + 33 + 12);              // ASR 2(SP); BIT #1,2(SP); BEQ
			poke((uint16_t)(sp + 2), asr(peek((uint16_t)(sp + 2)), 1));
			if (peek((uint16_t)(sp + 2)) & 1)
				print(0x0028, 0xF886);
			cy(18);                                    // SOB
		} while (--T.r[1]);
		wait_and_leave();
		return;
	}
	cy(12);
	T.r[1] = 0;
	do
	{
		charge(0xF6D4, 21 + 36 + 30);                  // CLR (SP); MOVB -0xB8(R1),(SP); JSR
		poke(sp, 0);
		pokeb(sp, peekb((uint16_t)(0xFF48 + T.r[1])));
		call_from(0xF6DE, 0x8EA2, {});
		cy(12);
		print((uint16_t)(T.r[1] + 0x0029), 0xF6E8);
		cy(12 + 12 + 27 + 12);                         // F:F6E8-F6F0
		T.r[0] = (uint16_t)(T.r[1] << 1);
		T.r[2] = peek((uint16_t)(0xFF4C + T.r[0]));
		if ((int16_t)T.r[2] > 0)
			do
			{
				cy(30);
				call_from(0xF6F6, 0x8D50, {});
				cy(24 + 12);
				if (peekb(0x0193) & 0x80)
				{
					cy(27 + 30);
					poke(sp, 0x0005);
					call_from(0xF704, 0x8EA2, {});
					cy(18);                            // JMP F:F88C
					leave();
					return;
				}
				cy(18);                                // SOB
			} while (--T.r[2]);
		cy(12);
		print((uint16_t)(T.r[1] + 0x00A9), 0xF714);
		cy(27 + 30);
		poke(sp, 0x0005);
		call_from(0xF71C, 0x8EA2, {});
		cy(12 + 18 + 12);                              // INC R1; CMP R1,#3; BLE
		T.r[1]++;
	} while ((int16_t)T.r[1] <= 3);
	cy(27 + 30);
	poke(sp, 0x0008);
	call_from(0xF72C, 0x8EA2, {});
	cy(30);
	call_from(0xF730, 0x8ED2, {});
	cy(30 + 30);
	poke((uint16_t)(sp + 6), T.r[0]);
	call_from(0xF738, 0x8F62, {});
	print(0x0024, 0xF740);
	number((uint16_t)(peek((uint16_t)(sp + 6)) - 0x002F), 0x0003, 0x0003, 0xF756);
	// the four words stay on the stack: the frame is at 8(SP), later at 0x10(SP)
	cy(36);
	poke((uint16_t)(sp + 0x0C), 0x0030);
	print(0x0025, 0xF764);
	number((uint16_t)(peek((uint16_t)(sp + 0x0C)) - 0x0030), 0x0003, 0x0003, 0xF77A);
	print(0x0026, 0xF782);
	print(0x0027, 0xF78A);
	print(0x002D, 0xF792);
	// 0x12(SP) coin bits, 0x14(SP) volume, 0x16(SP) highest volume, 0x18(SP) volume before
	for (;;)
	{
		cy(24 + 12);                                   // F:F792 TSTB @#0x0193; BMI
		if (peekb(0x0193) & 0x80)
		{
			cy(18 + 12);                               // ADD #0x10,SP; BR F:F88C
			sp = (uint16_t)(sp + 0x10);
			leave();
			return;
		}
		cy(30);
		call_from(0xF79C, 0x8D50, {});
		cy(30 + 12);
		if (!(peekb(0x01C4) & 0x07))
		{
			cy(27 + 30);
			poke(sp, 0x0012);
			call_from(0xF7AC, 0x8EA2, {});
			cy(30);
			call_from(0xF7B0, 0x8F50, {});
			cy(27 + 30);
			poke(sp, 0x0040);
			call_from(0xF7B8, 0x8EA2, {});
			cy(27 + 30);
			poke(sp, 0x0018);
			call_from(0xF7C0, 0x8EA2, {});
			cy(30);
			call_from(0xF7C4, 0x8ED2, {});
			cy(30 + 30);
			poke((uint16_t)(sp + 0x12), T.r[0]);
			call_from(0xF7CC, 0x8F62, {});
			cy(12);
			T.r[1] = 0;
			do
			{
				charge(0xF7CE, 12 + 12 + 42 + 12);     // MOV R1,R0; ASL R0; BIT; BNE
				T.r[0] = (uint16_t)(T.r[1] << 1);
				if (!(peek((uint16_t)(sp + 0x12)) & peek((uint16_t)(0xFF44 + T.r[0]))))
				{
					cy(12 + 18 + 12);
					T.r[0] = (uint16_t)(T.r[1] + 0x002E);
				}
				else
				{
					cy(12 + 18);
					T.r[0] = (uint16_t)(T.r[1] + 0x00AE);
				}
				cy(30);
				call_from(0xF7EC, 0x8AB6, {});
				cy(12 + 18 + 12);                      // INC R1; CMP R1,#1; BLE
				T.r[1]++;
			} while ((int16_t)T.r[1] <= 1);
		}
		cy(45 + 30);                                   // F:F7F4-F7FA
		poke((uint16_t)(sp + 0x18), peek((uint16_t)(sp + 0x14)));
		call_from(0xF7FE, 0x8D86, {});
		cy(30);
		call_from(0xF802, 0x8E2E, {});
		cy(27 + 30 + 18 + 12);                         // F:F802-F80E
		T.r[0] = (uint16_t)(T.r[0] + peek((uint16_t)(sp + 0x14)));
		poke((uint16_t)(sp + 0x14), T.r[0]);
		if ((int16_t)T.r[0] < 0x0030)
		{
			cy(45);
			poke((uint16_t)(sp + 0x14), peek((uint16_t)(sp + 0x16)));
		}
		cy(42 + 12);
		if ((int16_t)peek((uint16_t)(sp + 0x14)) > (int16_t)peek((uint16_t)(sp + 0x16)))
		{
			cy(36);
			poke((uint16_t)(sp + 0x14), 0x0030);
		}
		cy(42 + 12);
		if (peek((uint16_t)(sp + 0x18)) != peek((uint16_t)(sp + 0x14)))
		{
			cy(27 + 30);
			poke(sp, 0x0005);
			call_from(0xF834, 0x8EA2, {});
			print(0x0025, 0xF83C);
			number((uint16_t)(peek((uint16_t)(sp + 0x14)) - 0x0030), 0x0003, 0x0003, 0xF852);
			cy(18);
			sp = (uint16_t)(sp + 8);
		}
		cy(30 + 12);
		if (!(peekb(0x0193) & 0x40)) continue;
		cy(36 + 30);
		poke(sp, peek((uint16_t)(sp + 0x14)));
		call_from(0xF866, 0x8EA2, {});
		cy(12);
	}
}

// F:FA00  selftest_center_rotary(): unless the controller position 0x01C6 is already in
// 0x20-0x28, asks for it to be centred (messages 0 and 7) and waits until it has stood still
// there for six frames (button 1 shown: message 0x0B / 0x8B; 0x0FE6, the next screen, ends
// early), then messages 0x80, 0x87, 1, two seconds, 0x81.
uint16_t selftest_center_rotary(const Args &)
{
	At here(0xFA00);
	Helper h(F162);
	uint16_t &sp = T.r[6];
	auto print = [](uint16_t msg, uint16_t ret) {
		cy(18 + 30);
		T.r[0] = msg;
		call_from(ret, 0x8AB6, {});
	};
	cy(21 + 30 + 12);                                  // TST -(SP); CMP @#0x01C6,#0x20; BLT
	sp = (uint16_t)(sp - 2);
	if ((int16_t)peek(0x01C6) >= 0x20)
	{
		cy(30 + 12);
		if ((int16_t)peek(0x01C6) <= 0x28)
		{
			cy(18 + 21);
			sp = (uint16_t)(sp + 2);
			return T.r[0];
		}
	}
	print(0x0000, 0xFA1E);
	print(0x0007, 0xFA26);
	cy(27);
	poke(sp, 0x0006);
	for (;;)
	{
		cy(30);
		call_from(0xFA2E, 0x8D50, {});
		cy(30);
		call_from(0xFA32, 0x8D86, {});
		cy(24 + 12);
		bool still = false;
		if (!peek(0x01CA))
		{
			cy(30 + 12);
			if ((int16_t)peek(0x01C6) >= 0x20)
			{
				cy(30 + 12);
				still = (int16_t)peek(0x01C6) <= 0x28;
			}
		}
		if (still)
		{
			cy(21 + 12);
			poke(sp, (uint16_t)(peek(sp) - 1));
		}
		else
		{
			cy(27);
			poke(sp, 0x0006);
		}
		cy(30 + 12);
		if (peekb(0x0193) & 0x40)
		{
			cy(18 + 12);
			T.r[0] = 0x000B;
		}
		else
		{
			cy(18);
			T.r[0] = 0x008B;
		}
		cy(30);
		call_from(0xFA66, 0x8AB6, {});
		cy(24 + 12);
		if (peekb(0x0FE6))
		{
			cy(18 + 21);
			sp = (uint16_t)(sp + 2);
			return T.r[0];
		}
		cy(18 + 12);
		if (!peek(sp)) break;
	}
	print(0x0080, 0xFA78);
	print(0x0087, 0xFA80);
	print(0x0001, 0xFA88);
	cy(30 + 30);
	push(0x0078);
	call_from(0xFA90, 0x8D74, {});
	print(0x0081, 0xFA98);
	cy(18 + 18 + 21);
	sp = (uint16_t)(sp + 4);
	return T.r[0];
}

// F:FA9E (registers): marks sector n of the rotary test: the four characters at 0x7798[n * 4]
// + 0x2082 (bank B 0x2800; the playfield selected by F:8068) get the colour 0x0188 | 0x87.
// R1 is preserved.
void mark_rotary_sector()
{
	At here(0xFA9E);
	cy(24 + 30);
	push(T.r[1]);
	call_from(0xFAA4, 0x8068, {});
	cy(381);                                           // F:FAA4-FADC
	T.r[0] = 0x2800;
	poke(0x01D4, 0x2800);
	ww(0x1402, 0x2800);
	uint16_t r0 = peek((uint16_t)(T.r[6] + 4));
	r0 = (uint16_t)(peek((uint16_t)((r0 << 3) + 0x7798)) + 0x2082);
	const uint16_t r1 = peek(0x0188) | 0x0087;
	poke((uint16_t)(r0 + 0x82), r1);
	poke((uint16_t)(r0 + 0x80), r1);
	poke((uint16_t)(r0 + 2), r1);
	poke(r0, r1);
	T.r[0] = r0;
	T.r[1] = pop();
	fl::mov(T.r[1]);
}

// F:FADE  selftest_rotate_sectors(): every frame the controller's sector ((0x01C6 + 4) / 9,
// F:90A6) is marked (F:FA9E) the first time it is reached, until all sectors (bits 0xFC7C[n])
// are done (message 5, two seconds) or the next screen is asked for (0x0FE6).  Button 1 is
// shown as in F:FA00.
uint16_t selftest_rotate_sectors(const Args &)
{
	At here(0xFADE);
	Helper h(F162);
	uint16_t &sp = T.r[6];
	cy(30 + 30 + 12);                                  // CMP -(SP),-(SP); CLR 2(SP); CLR R0
	sp = (uint16_t)(sp - 4);
	poke((uint16_t)(sp + 2), 0);
	T.r[0] = 0;
	for (;;)
	{
		cy(30);
		call_from(0xFAEE, 0x8D50, {});
		cy(30);
		call_from(0xFAF2, 0x8D86, {});
		cy(36 + 27 + 30 + 30);                         // F:FAF2-FAFE
		push(peek(0x01C6));
		poke(sp, (uint16_t)(peek(sp) + 4));
		push(0x0009);
		call_from(0xFB02, 0x90A6, {});
		cy(30 + 12 + 12 + 42 + 12);                    // F:FB02-FB10
		poke((uint16_t)(sp + 4), T.r[0]);
		T.r[1] = (uint16_t)(T.r[0] << 1);
		if (!(peek((uint16_t)(sp + 6)) & peek((uint16_t)(0xFC7C + T.r[1]))))
		{
			cy(21 + 30);
			poke(sp, T.r[0]);
			call_from(0xFB18, 0xFA9E, {});
			cy(45);
			poke((uint16_t)(sp + 6), peek((uint16_t)(sp + 6)) | peek((uint16_t)(0xFC7C + T.r[1])));
		}
		cy(30 + 12);
		if (peekb(0x0193) & 0x40)
		{
			cy(18 + 12);
			T.r[0] = 0x000B;
		}
		else
		{
			cy(18);
			T.r[0] = 0x008B;
		}
		cy(30);
		call_from(0xFB34, 0x8AB6, {});
		cy(24 + 12);
		if (peekb(0x0FE6))
		{
			cy(24 + 12 + 24 + 21);
			sp = (uint16_t)(sp + 8);
			return T.r[0];
		}
		cy(24 + 33 + 12);
		sp = (uint16_t)(sp + 4);
		if (peek((uint16_t)(sp + 2)) == 0xFFFF) break;
	}
	cy(18 + 30);
	T.r[0] = 0x0005;
	call_from(0xFB50, 0x8AB6, {});
	cy(30 + 30);
	push(0x0078);
	call_from(0xFB58, 0x8D74, {});
	cy(18 + 24 + 21);
	sp = (uint16_t)(sp + 6);
	return T.r[0];
}

// F:FB5E  selftest_controls_screen(): the items screen (F:F98E) with messages 2, 3 and 8 shows
// button 1 (message 0x0B / 0x8B) until the next screen is asked for (0x0FE6) or the rotary
// controller passes its centre (0x02BE, set by F:8D86).  Then (message 9, two seconds, until it
// has stood still for six frames) the controller tests repeat: centring (F:FA00), clockwise
// (messages 4, 0x0A) and anticlockwise (4, 6) through all sectors (F:FADE).
uint16_t selftest_controls_screen(const Args &)
{
	At here(0xFB5E);
	Helper h(F162);
	uint16_t &sp = T.r[6];
	auto print = [](uint16_t msg, uint16_t ret) {
		cy(18 + 30);
		T.r[0] = msg;
		call_from(ret, 0x8AB6, {});
	};
	cy(21 + 30);
	sp = (uint16_t)(sp - 2);                           // TST -(SP)
	call_from(0xFB68, 0xF98E, {});
	cy(27);
	pokeb(0x02BE, 0);
	print(0x0002, 0xFB74);
	print(0x0003, 0xFB7C);
	print(0x0008, 0xFB84);
	for (;;)
	{
		cy(24 + 12);                                   // F:FB84 TSTB @#0x02BE; BNE
		if (peekb(0x02BE)) break;
		cy(30);
		call_from(0xFB8E, 0x8D50, {});
		cy(30);
		call_from(0xFB92, 0x8D86, {});
		cy(30 + 12);
		if (peekb(0x0193) & 0x40)
		{
			cy(18 + 12);
			T.r[0] = 0x000B;
		}
		else
		{
			cy(18);
			T.r[0] = 0x008B;
		}
		cy(30);
		call_from(0xFBA8, 0x8AB6, {});
		cy(24 + 12);
		if (peekb(0x0FE6))
		{
			cy(27 + 12 + 18 + 21);                     // CLRB @#0x0FE6; BR F:FC4A; TST (SP)+; RTS
			pokeb(0x0FE6, 0);
			sp = (uint16_t)(sp + 2);
			return T.r[0];
		}
	}
	print(0x0009, 0xFBBC);
	cy(30 + 30);
	push(0x0078);
	call_from(0xFBC4, 0x8D74, {});
	cy(36);
	poke((uint16_t)(sp + 2), 0x0006);
	bool next = false;
	for (;;)
	{
		cy(30);
		call_from(0xFBCE, 0x8D50, {});
		cy(30);
		call_from(0xFBD2, 0x8D86, {});
		cy(24 + 12);
		if (!peek(0x01CA))
		{
			cy(30 + 12);
			poke((uint16_t)(sp + 2), (uint16_t)(peek((uint16_t)(sp + 2)) - 1));
		}
		else
		{
			cy(36);
			poke((uint16_t)(sp + 2), 0x0006);
		}
		cy(24 + 12);
		if (peekb(0x0FE6))
		{
			next = true;
			break;
		}
		cy(27 + 12);
		if (!peek((uint16_t)(sp + 2))) break;
	}
	while (!next)
	{
		static const uint16_t rets[2][8] = {
			{0xFBF4, 0xFBF8, 0xFBFC, 0xFC04, 0xFC0C, 0xFC14},
			{0xFC1E, 0xFC22, 0xFC26, 0xFC2E, 0xFC36, 0xFC3E},
		};
		for (int dir = 0; dir < 2 && !next; dir++)
		{
			const uint16_t *ret = rets[dir];
			cy(30);
			call_from(ret[0], 0xF98E, {});
			cy(30);
			call_from(ret[1], 0x8D86, {});
			cy(30);
			call_from(ret[2], 0xFA00, {});
			print(0x0004, ret[3]);
			print(dir ? 0x0006 : 0x000A, ret[4]);
			cy(27 + 30);
			poke(sp, dir ? 0xFFFF : 0x0001);
			call_from(ret[5], 0xFADE, {});
			cy(24 + 12);
			next = peekb(0x0FE6) != 0;
		}
	}
	cy(27 + 18 + 18 + 21);                             // F:FC44-FC4C
	pokeb(0x0FE6, 0);
	sp = (uint16_t)(sp + 4);
	return T.r[0];
}

}

LOGIC_TABLE(fhigh,
	c_routine("F", 0xD0F4, "update_marked_slots", update_marked_slots, 0, 0),
	c_routine("F", 0xD15C, "update_high_slots", update_high_slots, 0, 0),
	c_routine("F", 0xD880, "set_park_entry_types", set_park_entry_types, 0, 0),
	c_routine("F", 0xD8D2, "set_ticket_entry_types", set_ticket_entry_types, 0, 0),
	c_routine("F", 0xD47C, "start_object", start_object, 0, 0),
	c_routine("F", 0xDD5C, "set_colour_1092", set_colour_1092, 0, 0),
	c_routine("F", 0xDE28, "run_object_scripts", run_object_scripts, 0, 0),
	c_routine("F", 0xDF40, "start_object_scripts", start_object_scripts, 0, 0),
	c_routine("F", 0xE036, "countdown_0eb4", countdown_0eb4, 0, 0),
	c_routine("F", 0xE13A, "set_object7_motion", set_object7_motion, 0, 0),
	c_routine("F", 0xE5D2, "skater_anim_a", skater_anim_a, 0, 0),
	c_routine("F", 0xE648, "skater_anim_b", skater_anim_b, 0, 0),
	c_routine("F", 0xEF24, "add_score", add_score, 0, 0),
	c_routine("F", 0xF064, "start_object4", start_object4, 0, 0),
	c_routine("F", 0xD19C, "setup_level_slots", setup_level_slots, 0, 0),
	wait_c_routine("F", 0xE78E, "query_on_button_press", query_on_button_press, 0),
	asm_routine("F", 0xD35E, "spawn_counted_object", spawn_counted_object, 0),
	asm_routine("F", 0xD4E4, "offset_to_object_0ea0", offset_to_object_0ea0, 0),
	asm_routine("F", 0xD566, "contact_object_0ea0", contact_object_0ea0, 0),
	asm_routine("F", 0xDB4E, "fade_sprite_palette", fade_sprite_palette, 0),
	asm_routine("F", 0xDBE2, "step_palette_fade", step_palette_fade, 0),
	asm_routine("F", 0xDC3C, "load_palette_block", load_palette_block, 0),
	asm_routine("F", 0xDDEC, "run_palette_job", run_palette_job, 0),
	asm_routine("F", 0xE254, "update_skater_phase", update_skater_phase, 0),
	c_routine("F", 0xEAD4, "draw_tile_block", draw_tile_block, 0, 0),
	c_routine("F", 0xEC20, "draw_map_object", draw_map_object, 0, 0),
	asm_routine("F", 0xE82E, "unpack_map_block", unpack_map_block, 0),
	c_routine("F", 0xD052, "spawn_level_objects", spawn_level_objects, 0, 0),
	asm_routine("F", 0xD632, "on_object_0ea0", on_object_0ea0, 0),
	c_routine("F", 0xD6BE, "slope_of_object_0ea0", slope_of_object_0ea0, 0, 0),
	asm_routine("F", 0xD742, "move_on_object_0ea0", move_on_object_0ea0, 0),
	asm_routine("F", 0xD778, "update_object_0ea0", update_object_0ea0, 0),
	c_routine("F", 0xD838, "facing_object_0ea0", facing_object_0ea0, 0, 0),
	wait_c_routine("F", 0xE730, "swap_players", swap_players, 0),
	wait_c_routine("F", 0xEDB2, "show_screen_objects", show_screen_objects, 0),
	wait_routine("F", 0xF372, "enter_selftest", enter_selftest, 0),
	wait_routine("F", 0xF40A, "wait_forever", wait_forever, 0),
	wait_routine("F", 0xF43C, "selftest_loop", selftest_loop, 0),
	asm_routine("F", 0xF482, "print_bad", print_bad, 0),
	asm_routine("F", 0xF4D2, "selftest_rom_checksums", selftest_rom_checksums, 0),
	wait_routine("F", 0xF5D8, "selftest_sound_screen", selftest_sound_screen, 0),
	wait_c_routine("F", 0xFA00, "selftest_center_rotary", selftest_center_rotary, 0),
	asm_routine("F", 0xFA9E, "mark_rotary_sector", mark_rotary_sector, 0),
	wait_c_routine("F", 0xFADE, "selftest_rotate_sectors", selftest_rotate_sectors, 0),
	wait_c_routine("F", 0xFB5E, "selftest_controls_screen", selftest_controls_screen, 0),
)

}
