// Rewritten sound routines: the commands from the main CPU (see sound.h).  The NMI takes each
// command byte from the latch: most go into the queue at 0x0200 for the main loop, which does
// them through 4333 by their type (0x5D58) and parameter (0x5E11); some are done at once by the
// NMI (0x5C9F positive: a handler of the table at 0x5ED2, which ends the NMI).
#include "sound.h"

namespace sound {
namespace {

using namespace s65rt;

// PHP; SEI at pc, PLP at pc
void php_sei(uint16_t pc)
{
	cyc(3); push((uint8_t)(S.p | FB | FU)); step((uint16_t)(pc + 1));
	cyc(2); S.p |= FI; step((uint16_t)(pc + 2));
}
void plp(uint16_t pc)
{
	cyc(4); S.p = (uint8_t)((pull() | FU) & ~FB); step((uint16_t)(pc + 1));
}

// 4333: command Y: unless Y >= 0xB9 or its type (0x5D58) >= 15, on to the handler of the type
// (table 0x4638, by an RTS) with the command's parameter (0x5E11) in A and Y the command
void do_command()
{
	cyc(2); op_cmp(S.y, 0xB9); step(0x4335);           // CPY #$B9
	if (branch(0x4335, S.p & FC, 0x434B))
	{
		rts();
		return;
	}
	cyc(4); S.a = rd(idx(0x5D58, S.y)); nz(S.a); step(0x433A);
	cyc(2); op_cmp(S.a, 0x0F); step(0x433C);
	if (branch(0x433C, S.p & FC, 0x434B))
	{
		rts();
		return;
	}
	cyc(2); S.a = op_asl(S.a); step(0x433F);
	cyc(2); S.x = S.a; nz(S.x); step(0x4340);          // TAX
	cyc(4); S.a = rd(idx(0x4639, S.x)); nz(S.a); step(0x4343);
	cyc(3); push(S.a); step(0x4344);                   // PHA
	cyc(4); S.a = rd(idx(0x4638, S.x)); nz(S.a); step(0x4347);
	cyc(3); push(S.a); step(0x4348);                   // PHA
	cyc(4); S.a = rd(idx(0x5E11, S.y)); nz(S.a); step(0x434B);   // the parameter
	rts_jump();
}

// 434C (type 0): 0x13 = A * 4
void cmd_set_13()
{
	cyc(2); S.a = op_asl(S.a); step(0x434D);
	cyc(2); S.a = op_asl(S.a); step(0x434E);
	cyc(3); SR[0x13] = S.a; step(0x4350);
	rts();
}

// 4351 (type 1) / 435E (type 2): the byte 0x18 + (0x6294[A] & 15) set to / increased by 0x62B8[A]
void set_or_add_18(uint16_t pc, bool add)
{
	cyc(2); S.y = S.a; nz(S.y); step((uint16_t)(pc + 1));   // TAY
	cyc(4); S.a = rd(idx(0x6294, S.y)); nz(S.a); step((uint16_t)(pc + 4));
	cyc(2); S.a &= 0x0F; nz(S.a); step((uint16_t)(pc + 6));
	cyc(2); S.x = S.a; nz(S.x); step((uint16_t)(pc + 7));   // TAX
	cyc(4); S.a = rd(idx(0x62B8, S.y)); nz(S.a); step((uint16_t)(pc + 0x0A));
	uint16_t at = (uint16_t)(pc + 0x0A);
	if (add)
	{
		cyc(2); S.p &= ~FC; step((uint16_t)(at + 1));  // CLC
		cyc(4); op_adc(SR[(uint8_t)(0x18 + S.x)]); step((uint16_t)(at + 3));
		at = (uint16_t)(at + 3);
	}
	cyc(4); SR[(uint8_t)(0x18 + S.x)] = S.a; step((uint16_t)(at + 2));   // STA $18,X
	rts();
}
void cmd_set_18() { set_or_add_18(0x4351, false); }
void cmd_add_18() { set_or_add_18(0x435E, true); }

// 436E (type 3): on to the handler A of the table at 0x5ECA (by an RTS)
void cmd_table()
{
	cyc(2); S.a = op_asl(S.a); step(0x436F);
	cyc(2); S.y = S.a; nz(S.y); step(0x4370);          // TAY
	cyc(4); S.a = rd(idx(0x5ECB, S.y)); nz(S.a); step(0x4373);
	cyc(3); push(S.a); step(0x4374);
	cyc(4); S.a = rd(idx(0x5ECA, S.y)); nz(S.a); step(0x4377);
	cyc(3); push(S.a); step(0x4378);
	rts_jump();
}

// 4379 (type 4): the notes whose 0x0348 / 4 is A are stopped (0x0228 = 0xFF)
void cmd_stop_class()
{
	php_sei(0x4379);
	cyc(3); SR[0x11] = S.a; step(0x437D);
	cyc(2); S.y = 0x17; nz(S.y); step(0x437F);
	do
	{
		cyc(4); S.a = rd(idx(0x0348, S.y)); nz(S.a); step(0x4382);
		cyc(2); S.a = op_lsr(S.a); step(0x4383);
		cyc(2); S.a = op_lsr(S.a); step(0x4384);
		cyc(3); S.a ^= SR[0x11]; nz(S.a); step(0x4386);
		if (!branch(0x4386, !(S.p & FZ), 0x438D))
		{
			cyc(2); S.a = 0xFF; nz(S.a); step(0x438A);
			cyc(5); wr((uint16_t)(0x0228 + S.y), S.a); step(0x438D);
		}
		cyc(2); S.y--; nz(S.y); step(0x438E);
	} while (branch(0x438E, !(S.p & FN), 0x437F));
	plp(0x4390);
	rts();
}

// 4392 (type 5): the notes of the sound command A (type 7, its parameter) are stopped
void cmd_stop_sound()
{
	php_sei(0x4392);
	cyc(2); S.y = S.a; nz(S.y); step(0x4395);          // TAY
	cyc(4); S.a = rd(idx(0x5D58, S.y)); nz(S.a); step(0x4398);
	cyc(2); op_cmp(S.a, 0x07); step(0x439A);
	if (!branch(0x439A, !(S.p & FZ), 0x43B2))
	{
		cyc(4); S.a = rd(idx(0x5E11, S.y)); nz(S.a); step(0x439F);
		cyc(3); SR[0x11] = S.a; step(0x43A1);
		cyc(2); S.y = 0x17; nz(S.y); step(0x43A3);
		do
		{
			cyc(4); op_cmp(S.a, rd(idx(0x0228, S.y))); step(0x43A6);
			if (!branch(0x43A6, !(S.p & FZ), 0x43AF))
			{
				cyc(2); S.a = 0xFF; nz(S.a); step(0x43AA);
				cyc(5); wr((uint16_t)(0x0228 + S.y), S.a); step(0x43AD);
				cyc(3); S.a = SR[0x11]; nz(S.a); step(0x43AF);
			}
			cyc(2); S.y--; nz(S.y); step(0x43B0);
		} while (branch(0x43B0, !(S.p & FN), 0x43A3));
	}
	plp(0x43B2);
	rts();
}

// 43B4 (type 6): the notes of channel A (high nibble: player, 0x57A9; low: channel) stopped
void cmd_stop_channel()
{
	cyc(3); SR[0x11] = S.a; step(0x43B6);
	for (uint16_t pc = 0x43B7; pc <= 0x43BA; pc++)
	{
		cyc(2); S.a = op_lsr(S.a); step(pc);
	}
	cyc(2); S.y = S.a; nz(S.y); step(0x43BB);          // TAY
	cyc(3); S.a = SR[0x11]; nz(S.a); step(0x43BD);
	cyc(2); S.a &= 0x0F; nz(S.a); step(0x43BF);
	cyc(2); S.p &= ~FC; step(0x43C0);                  // CLC
	cyc(4); op_adc(rd(idx(0x57A9, S.y))); step(0x43C3);
	cyc(2); S.y = S.a; nz(S.y); step(0x43C4);          // TAY
	php_sei(0x43C4);
	cyc(4); S.a = rd(idx(0x06C0, S.y)); nz(S.a); step(0x43C9);
	if (!branch(0x43C9, S.p & FZ, 0x43D7))
	{
		do
		{
			cyc(2); S.y = S.a; nz(S.y); step(0x43CC);  // TAY
			cyc(2); S.y--; nz(S.y); step(0x43CD);      // DEY
			cyc(2); S.a = 0xFF; nz(S.a); step(0x43CF);
			cyc(5); wr((uint16_t)(0x0228 + S.y), S.a); step(0x43D2);
			cyc(4); S.a = rd(idx(0x06C0, S.y)); nz(S.a); step(0x43D5);
		} while (branch(0x43D5, !(S.p & FZ), 0x43CB));
	}
	plp(0x43D7);
	rts();
}

// the release of note Y at pc (LDA #0 .. STA $0228,Y): 0x0618 / 0x0678 / 0x0648 = 0, 0x0660 =
// 0xD0, 0x0630 = 2, 0x0228 = 0xFE
void release(uint16_t pc)
{
	cyc(2); S.a = 0x00; nz(S.a); step((uint16_t)(pc + 2));
	static const uint16_t zero[3] = {0x0618, 0x0678, 0x0648};
	for (int i = 0; i < 3; i++)
	{
		cyc(5); wr((uint16_t)(zero[i] + S.y), S.a); step((uint16_t)(pc + 5 + 3 * i));
	}
	static const struct { uint8_t v; uint16_t a; } set[3] = {{0xD0, 0x0660}, {0x02, 0x0630}, {0xFE, 0x0228}};
	uint16_t at = (uint16_t)(pc + 0x0B);
	for (const auto &s : set)
	{
		cyc(2); S.a = s.v; nz(S.a); step((uint16_t)(at + 2));
		cyc(5); wr((uint16_t)(s.a + S.y), S.a); step((uint16_t)(at + 5));
		at = (uint16_t)(at + 5);
	}
}

// 43D9 (type 9): the notes of the sound command A (type 7) released
void cmd_release_sound()
{
	php_sei(0x43D9);
	cyc(2); S.y = S.a; nz(S.y); step(0x43DC);          // TAY
	cyc(4); S.a = rd(idx(0x5D58, S.y)); nz(S.a); step(0x43DF);
	cyc(2); op_cmp(S.a, 0x07); step(0x43E1);
	if (!branch(0x43E1, !(S.p & FZ), 0x440E))
	{
		cyc(4); S.a = rd(idx(0x5E11, S.y)); nz(S.a); step(0x43E6);
		cyc(3); SR[0x11] = S.a; step(0x43E8);
		cyc(2); S.y = 0x17; nz(S.y); step(0x43EA);
		do
		{
			cyc(4); op_cmp(S.a, rd(idx(0x0228, S.y))); step(0x43ED);
			if (!branch(0x43ED, !(S.p & FZ), 0x440B))
			{
				release(0x43EF);
				cyc(3); S.a = SR[0x11]; nz(S.a); step(0x440B);
			}
			cyc(2); S.y--; nz(S.y); step(0x440C);
		} while (branch(0x440C, !(S.p & FN), 0x43EA));
	}
	plp(0x440E);
	rts();
}

// 4410 (type 10): the notes whose 0x0348 / 4 is A released
void cmd_release_class()
{
	php_sei(0x4410);
	cyc(3); SR[0x11] = S.a; step(0x4414);
	cyc(2); S.y = 0x17; nz(S.y); step(0x4416);
	do
	{
		cyc(4); S.a = rd(idx(0x0348, S.y)); nz(S.a); step(0x4419);
		cyc(2); S.a = op_lsr(S.a); step(0x441A);
		cyc(2); S.a = op_lsr(S.a); step(0x441B);
		cyc(3); S.a ^= SR[0x11]; nz(S.a); step(0x441D);
		if (!branch(0x441D, !(S.p & FZ), 0x4439))
			release(0x441F);
		cyc(2); S.y--; nz(S.y); step(0x443A);
	} while (branch(0x443A, !(S.p & FN), 0x4416));
	plp(0x443C);
	rts();
}

// 4445 (type 8): A into the queue of replies for the main CPU (0x0214, written at 0x0225); when
// it is full, 0x0226 = 0x80 instead
void queue_reply()
{
	cyc(4); S.y = SR[0x225]; nz(S.y); step(0x4448);
	cyc(5); wr((uint16_t)(0x0214 + S.y), S.a); step(0x444B);
	cyc(2); S.y++; nz(S.y); step(0x444C);
	cyc(2); op_cmp(S.y, 0x10); step(0x444E);
	if (!branch(0x444E, !(S.p & FC), 0x4452))
	{
		cyc(2); S.y = 0x00; nz(S.y); step(0x4452);
	}
	cyc(4); op_cmp(S.y, SR[0x224]); step(0x4455);
	if (branch(0x4455, S.p & FZ, 0x445B))
	{
		cyc(2); S.a = 0x80; nz(S.a); step(0x445D);
		cyc(4); SR[0x226] = S.a; step(0x4460);
		rts();
		return;
	}
	cyc(4); SR[0x225] = S.y; step(0x445A);
	rts();
}

// 4443: the reply 0x55 (jumped to from 5883)
void reply_55()
{
	cyc(2); S.a = 0x55; nz(S.a); step(0x4445);
	queue_reply();
}

// 4461 (type 12): the entry A of the table at 0x62DC: an entry 0x3B or more, and the kinds 8-9,
// 0x0D-0x11, 0x16-0x18 do nothing; otherwise 0x0709 and (for a sound command, type 7) 0x0708
// are set and 505E goes on with X = 0x62DF + 0x18 and Y = the kind * 2
void cmd_62dc()
{
	cyc(2); S.x = S.a; nz(S.x); step(0x4462);          // TAX
	cyc(4); S.a = rd(idx(0x62DC, S.x)); nz(S.a); step(0x4465);
	static const struct { uint8_t v; bool skip; } bounds[7] = {
		{0x3B, true}, {0x08, false}, {0x0A, true}, {0x0D, false}, {0x12, true}, {0x16, false}, {0x19, true},
	};
	// CMP #v at pc: BCS skips (the first) / BCC goes on at 4481 / BCC skips
	uint16_t pc = 0x4465;
	bool go = false;
	for (int i = 0; i < 7; i++)
	{
		cyc(2); op_cmp(S.a, bounds[i].v); step((uint16_t)(pc + 2));
		const bool below = !(S.p & FC);
		if (i == 0)
		{
			if (branch((uint16_t)(pc + 2), !below, 0x44A7))
			{
				rts();
				return;
			}
		}
		else if (bounds[i].skip)
		{
			if (branch((uint16_t)(pc + 2), below, 0x44A7))
			{
				rts();
				return;
			}
		}
		else if (branch((uint16_t)(pc + 2), below, 0x4481))
		{
			go = true;
			break;
		}
		pc = (uint16_t)(pc + 4);
	}
	(void)go;                                          // 0x19 and up: on at 4481 as well
	cyc(2); S.a = op_asl(S.a); step(0x4482);
	cyc(2); S.y = S.a; nz(S.y); step(0x4483);          // TAY
	cyc(4); S.a = rd(idx(0x62DF, S.x)); nz(S.a); step(0x4486);
	cyc(2); S.p &= ~FC; step(0x4487);                  // CLC
	cyc(2); op_adc(0x18); step(0x4489);
	cyc(3); push(S.a); step(0x448A);                   // PHA
	cyc(4); S.a = rd(idx(0x62DD, S.x)); nz(S.a); step(0x448D);
	cyc(4); SR[0x709] = S.a; step(0x4490);
	cyc(4); S.a = rd(idx(0x62DE, S.x)); nz(S.a); step(0x4493);
	cyc(2); S.x = S.a; nz(S.x); step(0x4494);          // TAX
	cyc(4); S.a = rd(idx(0x5D58, S.x)); nz(S.a); step(0x4497);
	cyc(2); op_cmp(S.a, 0x07); step(0x4499);
	if (branch(0x4499, !(S.p & FZ), 0x44A6))
	{
		cyc(4); S.a = pull(); nz(S.a); step(0x44A7);   // PLA
		rts();
		return;
	}
	cyc(4); S.a = rd(idx(0x5E11, S.x)); nz(S.a); step(0x449E);
	cyc(4); SR[0x708] = S.a; step(0x44A1);
	cyc(4); S.a = pull(); nz(S.a); step(0x44A2);       // PLA
	cyc(2); S.x = S.a; nz(S.x); step(0x44A3);          // TAX
	jmp(0x505E);
}

// 44CD: A to the main CPU's latch (0x1874), waiting until it is free before and after
void send_main()
{
	cyc(3); push(S.a); step(0x44CE);                   // PHA
	cyc(2); S.a = 0x02; nz(S.a); step(0x44D0);
	do
	{
		cyc(4); S.a &= rd(0x1840); nz(S.a); step(0x44D3);   // AND $1840
	} while (branch(0x44D3, !(S.p & FZ), 0x44D0));
	cyc(4); S.a = pull(); nz(S.a); step(0x44D6);       // PLA
	cyc(3); push(S.a); step(0x44D7);                   // PHA
	cyc(4); wr(0x1874, S.a); step(0x44DA);
	cyc(2); S.a = 0x02; nz(S.a); step(0x44DC);
	do
	{
		cyc(4); S.a &= rd(0x1840); nz(S.a); step(0x44DF);
	} while (branch(0x44DF, !(S.p & FZ), 0x44DC));
	cyc(4); S.a = pull(); nz(S.a); step(0x44E2);       // PLA
	rts();
}

// 44BD: the end of an NMI command: 0x55 to the main CPU, the registers back (X, Y, A), out of
// the data mode (0x0213 = 0xFF), RTI
void nmi_done()
{
	cyc(2); S.a = 0x55; nz(S.a); step(0x44BF);
	jsr(0x44BF, 0x44CD);                               // send_main
	cyc(4); S.a = pull(); nz(S.a); step(0x44C3);       // PLA
	cyc(2); S.x = S.a; nz(S.x); step(0x44C4);          // TAX
	cyc(4); S.a = pull(); nz(S.a); step(0x44C5);
	cyc(2); S.y = S.a; nz(S.y); step(0x44C6);          // TAY
	cyc(2); S.a = 0xFF; nz(S.a); step(0x44C8);
	cyc(4); SR[0x213] = S.a; step(0x44CB);
	cyc(4); S.a = pull(); nz(S.a); step(0x44CC);
	rti();
}

// 44A8 (NMI command): the error bits 0x02 to the main CPU (then bits 0 and 2 set), then 0x55
void nmi_status()
{
	cyc(3); S.a = SR[0x02]; nz(S.a); step(0x44AA);
	jsr(0x44AA, 0x44CD);                               // send_main
	cyc(3); S.a = SR[0x02]; nz(S.a); step(0x44AF);
	cyc(2); S.a |= 0x04; nz(S.a); step(0x44B1);
	cyc(2); S.a |= 0x01; nz(S.a); step(0x44B3);
	cyc(3); SR[0x02] = S.a; step(0x44B5);
	cyc(3); step(0x44BD);                              // JMP $44BD
	nmi_done();
}

// 44B8 (NMI command): 0xB9 to the main CPU, then 0x55
void nmi_b9()
{
	cyc(2); S.a = 0xB9; nz(S.a); step(0x44BA);
	jsr(0x44BA, 0x44CD);                               // send_main
	nmi_done();
}

// 44E3 (type 7): start the sound A (0x03): its notes (from 0x5EFA[A], chained through 0x6190)
// each take a free slot of the 24 (0x0348 = 0) or the one of the lowest priority below theirs
// in the same voice list (0x06C0 + 0x5FEC + 0x18); a sound without 0x5F2D does not start while
// it still plays.  The slot gets its note data (0x6078 / 0x6178 pointer into 0x0240 / 0x0258),
// priority (0x5F60), the defaults and its place in the voice list (sorted by priority, a slot
// of the same priority replaced, 42FE).  SEC when a note found no slot.
void cmd_start_sound()
{
	cyc(3); SR[0x03] = S.a; step(0x44E5);
	cyc(2); S.y = S.a; nz(S.y); step(0x44E6);          // TAY
	cyc(4); S.x = rd(idx(0x5EFA, S.y)); nz(S.x); step(0x44E9);
	cyc(4); S.a = rd(idx(0x5F2D, S.y)); nz(S.a); step(0x44EC);
	if (!branch(0x44EC, !(S.p & FZ), 0x4502))
	{
		// not again while it plays
		cyc(3); S.a = SR[0x03]; nz(S.a); step(0x44F0);
		cyc(2); S.y = 0x17; nz(S.y); step(0x44F2);
		do
		{
			cyc(4); op_cmp(S.a, rd(idx(0x0228, S.y))); step(0x44F5);
			if (!branch(0x44F5, !(S.p & FZ), 0x44FF))
			{
				cyc(4); S.a = rd(idx(0x0348, S.y)); nz(S.a); step(0x44FA);
				if (!branch(0x44FA, S.p & FZ, 0x44FD))
				{
					rts();
					return;
				}
				cyc(3); S.a = SR[0x03]; nz(S.a); step(0x44FF);
			}
			cyc(2); S.y--; nz(S.y); step(0x4500);
		} while (branch(0x4500, !(S.p & FN), 0x44F2));
	}
	for (;;)
	{
		// note X: a free slot Y
		cyc(4); SR[0x227] = S.x; step(0x4505);
		cyc(2); S.y = 0x17; nz(S.y); step(0x4507);
		bool found = false;
		do
		{
			cyc(4); S.a = rd(idx(0x0348, S.y)); nz(S.a); step(0x450A);
			if (branch(0x450A, S.p & FZ, 0x4537))
			{
				found = true;
				break;
			}
			cyc(2); S.y--; nz(S.y); step(0x450D);
		} while (branch(0x450D, !(S.p & FN), 0x4507));
		if (!found)
		{
			// or the first slot of the voice list, if its priority is lower
			cyc(4); S.y = rd(idx(0x5F60, S.x)); nz(S.y); step(0x4512);
			cyc(4); S.a = rd(idx(0x5FEC, S.x)); nz(S.a); step(0x4515);
			cyc(2); S.p &= ~FC; step(0x4516);          // CLC
			cyc(2); op_adc(0x18); step(0x4518);
			cyc(2); S.x = S.a; nz(S.x); step(0x4519);  // TAX
			cyc(2); S.a = S.y; nz(S.a); step(0x451A);  // TYA
			php_sei(0x451A);
			cyc(4); S.y = rd(idx(0x06C0, S.x)); nz(S.y); step(0x451F);
			bool take = false;
			if (!branch(0x451F, S.p & FZ, 0x452A))
			{
				cyc(2); S.y--; nz(S.y); step(0x4522);  // DEY
				cyc(2); S.a = op_asl(S.a); step(0x4523);
				cyc(2); S.p |= FC; step(0x4524);       // SEC
				cyc(2); S.a = op_rol(S.a); step(0x4525);
				cyc(4); op_cmp(S.a, rd(idx(0x0348, S.y))); step(0x4528);
				take = branch(0x4528, S.p & FC, 0x452D);
			}
			if (!take)
			{
				plp(0x452A);
				cyc(2); S.p |= FC; step(0x452C);       // SEC: no slot
				rts();
				return;
			}
			cyc(4); S.a = rd(idx(0x06C0, S.y)); nz(S.a); step(0x4530);
			cyc(5); wr((uint16_t)(0x06C0 + S.x), S.a); step(0x4533);
			plp(0x4533);
			cyc(4); S.x = SR[0x227]; nz(S.x); step(0x4537);
		}
		// the slot set up
		static const struct { uint8_t v; uint16_t a; } first[4] = {
			{0x07, 0x03A8}, {0x10, 0x0510}, {0xA0, 0x0570}, {0xFF, 0x0378},
		};
		uint16_t pc = 0x4537;
		for (const auto &f : first)
		{
			cyc(2); S.a = f.v; nz(S.a); step((uint16_t)(pc + 2));
			cyc(5); wr((uint16_t)(f.a + S.y), S.a); step((uint16_t)(pc + 5));
			pc = (uint16_t)(pc + 5);
		}
		cyc(2); S.a = 0x00; nz(S.a); step(0x454D);
		static const uint16_t zero[25] = {
			0x0390, 0x06C0, 0x0528, 0x02A0, 0x02B8, 0x0588, 0x0600, 0x05A0, 0x05B8, 0x05D0, 0x05E8, 0x02D0, 0x02E8,
			0x0300, 0x0618, 0x0630, 0x0690, 0x06A8, 0x0558, 0x0540, 0x0480, 0x04F8, 0x0420, 0x0468, 0x0360,
		};
		for (int i = 0; i < 25; i++)
		{
			cyc(5); wr((uint16_t)(zero[i] + S.y), S.a); step((uint16_t)(0x4550 + 3 * i));
		}
		cyc(2); S.a = 0xC0; nz(S.a); step(0x459A);
		cyc(5); wr((uint16_t)(0x03C0 + S.y), S.a); step(0x459D);
		cyc(5); wr((uint16_t)(0x03F0 + S.y), S.a); step(0x45A0);
		cyc(2); S.a = 0x59; nz(S.a); step(0x45A2);
		cyc(5); wr((uint16_t)(0x03D8 + S.y), S.a); step(0x45A5);
		cyc(5); wr((uint16_t)(0x0408 + S.y), S.a); step(0x45A8);
		cyc(4); S.a = rd(idx(0x5F60, S.x)); nz(S.a); step(0x45AB);   // the priority
		cyc(2); S.a = op_asl(S.a); step(0x45AC);
		cyc(2); S.p |= FC; step(0x45AD);               // SEC
		cyc(2); S.a = op_rol(S.a); step(0x45AE);
		cyc(5); wr((uint16_t)(0x0348 + S.y), S.a); step(0x45B1);
		cyc(4); S.a = rd(idx(0x5FEC, S.x)); nz(S.a); step(0x45B4);   // the voice list
		cyc(2); S.p &= ~FC; step(0x45B5);              // CLC
		cyc(2); op_adc(0x18); step(0x45B7);
		cyc(4); SR[0x6E8] = S.a; step(0x45BA);
		cyc(2); S.a = S.x; nz(S.a); step(0x45BB);      // TXA
		cyc(2); S.a = op_asl(S.a); step(0x45BC);
		cyc(2); S.x = S.a; nz(S.x); step(0x45BD);      // TAX
		// the note data pointer from 0x6078 (0x6178 for notes 0x80 on)
		const bool high = (S.p & FC) != 0;
		if (branch(0x45BD, high, 0x45C4))
		{
			cyc(4); S.a = rd(idx(0x6178, S.x)); nz(S.a); step(0x45C7);
		}
		else
		{
			cyc(4); S.a = rd(idx(0x6078, S.x)); nz(S.a); step(0x45C2);
			branch(0x45C2, !(S.p & FC), 0x45C7);       // BCC (always)
		}
		cyc(5); wr((uint16_t)(0x0240 + S.y), S.a); step(0x45CA);
		if (branch(0x45CA, S.p & FC, 0x45D1))
		{
			cyc(4); S.a = rd(idx(0x6179, S.x)); nz(S.a); step(0x45D4);
		}
		else
		{
			cyc(4); S.a = rd(idx(0x6079, S.x)); nz(S.a); step(0x45CF);
			branch(0x45CF, !(S.p & FC), 0x45D4);
		}
		cyc(5); wr((uint16_t)(0x0258 + S.y), S.a); step(0x45D7);
		cyc(4); S.x = SR[0x6E8]; nz(S.x); step(0x45DA);
		php_sei(0x45DA);
		// its place in the voice list: before the first of a lower priority
		for (;;)
		{
			cyc(4); SR[0x6E8] = S.x; step(0x45DF);
			cyc(4); S.a = rd(idx(0x06C0, S.x)); nz(S.a); step(0x45E2);
			if (branch(0x45E2, S.p & FZ, 0x4607))
				break;
			cyc(2); S.x = S.a; nz(S.x); step(0x45E5);  // TAX
			cyc(2); S.x--; nz(S.x); step(0x45E6);      // DEX
			cyc(4); S.a = rd(idx(0x0348, S.x)); nz(S.a); step(0x45E9);
			cyc(2); S.a |= 0x01; nz(S.a); step(0x45EB);
			cyc(4); op_cmp(S.a, rd(idx(0x0348, S.y))); step(0x45EE);
			if (branch(0x45EE, !(S.p & FC), 0x45DC))
				continue;
			if (branch(0x45F0, S.p & FZ, 0x45F6))
			{
				// the same priority: that slot replaced
				jsr(0x45F6, 0x42FE);                   // node_free
				cyc(2); S.a = 0x00; nz(S.a); step(0x45FB);
				cyc(5); wr((uint16_t)(0x0348 + S.x), S.a); step(0x45FE);
				cyc(4); S.a = rd(idx(0x06C0, S.x)); nz(S.a); step(0x4601);
			}
			else
			{
				cyc(2); S.x++; nz(S.x); step(0x45F3);  // INX
				cyc(2); S.a = S.x; nz(S.a); step(0x45F4);   // TXA
				branch(0x45F4, S.p & FC, 0x4601);      // BCS (always)
			}
			cyc(5); wr((uint16_t)(0x06C0 + S.y), S.a); step(0x4604);
			cyc(4); S.x = SR[0x6E8]; nz(S.x); step(0x4607);
			break;
		}
		cyc(3); S.a = SR[0x03]; nz(S.a); step(0x4609);
		cyc(5); wr((uint16_t)(0x0228 + S.y), S.a); step(0x460C);   // the sound playing
		cyc(2); S.y++; nz(S.y); step(0x460D);
		cyc(2); S.a = S.y; nz(S.a); step(0x460E);      // TYA
		cyc(5); wr((uint16_t)(0x06C0 + S.x), S.a); step(0x4611);
		plp(0x4611);
		cyc(4); S.y = SR[0x227]; nz(S.y); step(0x4615);
		cyc(4); S.x = rd(idx(0x6190, S.y)); nz(S.x); step(0x4618);   // the next note
		if (branch(0x4618, S.p & FZ, 0x461D))
			break;
		cyc(3); step(0x4502);                          // JMP $4502
	}
	rts();
}

// 461D (type 14): nothing
void cmd_none()
{
	rts();
}

// 461E (type 13): the IRQ mask A: 0x28 = A & 0xE0, 0x29 = the rest, to 0x187A unless 0x2F
// has bits of it
void cmd_mask()
{
	cyc(3); push(S.a); step(0x461F);                   // PHA
	cyc(3); SR[0x28] = S.a; step(0x4621);
	cyc(2); S.a = 0xE0; nz(S.a); step(0x4623);
	cyc(2); S.a ^= 0xFF; nz(S.a); step(0x4625);
	cyc(3); S.a &= SR[0x28]; nz(S.a); step(0x4627);
	cyc(3); SR[0x29] = S.a; step(0x4629);
	cyc(4); S.a = pull(); nz(S.a); step(0x462A);       // PLA
	cyc(2); S.a &= 0xE0; nz(S.a); step(0x462C);
	cyc(3); SR[0x28] = S.a; step(0x462E);
	cyc(3); S.a = SR[0x29]; nz(S.a); step(0x4630);
	cyc(3); op_bit(SR[0x2F]); step(0x4632);
	if (!branch(0x4632, !(S.p & FZ), 0x4637))
	{
		cyc(4); wr(0x187A, S.a); step(0x4637);
	}
	rts();
}

// 57AC: the NMI, a byte from the main CPU in the latch (0x1860), once it has read the last
// reply.  In the data mode (0x0213 = 0) the byte is stored at (0x04) + 0x0212 (0x0212 counts
// on, the pointer moves on by 0x80 when it passes 0x7F); 0x0213 other than 0xFF drops it.  A
// command goes into the queue at 0x0200 (the oldest dropped when it is full; 0xB9 for a bad
// one), or, for 0x5C9F[command] < 0x14, is done at once by the handler of the table at 0x5ED2
// (which ends the NMI).
void nmi()
{
	cyc(3); push(S.a); step(0x57AD);                   // PHA
	do
	{
		cyc(2); S.a = 0x02; nz(S.a); step(0x57AF);
		cyc(4); op_bit(rd(0x1840)); step(0x57B2);      // the last reply read?
	} while (branch(0x57B2, !(S.p & FZ), 0x57AD));
	cyc(2); S.a = S.y; nz(S.a); step(0x57B5);          // TYA
	cyc(3); push(S.a); step(0x57B6);                   // PHA
	cyc(4); S.y = SR[0x213]; nz(S.y); step(0x57B9);
	if (!branch(0x57B9, !(S.p & FZ), 0x57DD))
	{
		// the data mode
		cyc(2); S.p &= ~FC; step(0x57BC);              // CLC
		cyc(4); S.y = SR[0x212]; nz(S.y); step(0x57BF);
		cyc(2); S.a = S.y; nz(S.a); step(0x57C0);      // TYA
		if (!branch(0x57C0, !(S.p & FN), 0x57D0))
		{
			cyc(2); S.a ^= 0x80; nz(S.a); step(0x57C4);
			cyc(2); S.y = S.a; nz(S.y); step(0x57C5); // TAY
			cyc(3); S.a = SR[0x04]; nz(S.a); step(0x57C7);
			cyc(2); S.a ^= 0x80; nz(S.a); step(0x57C9);
			cyc(3); SR[0x04] = S.a; step(0x57CB);
			if (!branch(0x57CB, S.p & FN, 0x57CF))
			{
				cyc(5); SR[0x05]++; nz(SR[0x05]); step(0x57CF);
			}
			cyc(2); S.a = S.y; nz(S.a); step(0x57D0); // TYA
		}
		cyc(2); op_adc(0x01); step(0x57D2);
		cyc(4); SR[0x212] = S.a; step(0x57D5);
		cyc(4); S.a = rd(0x1860); nz(S.a); step(0x57D8);   // the byte
		cyc(6); wr((uint16_t)(zp16(0x04) + S.y), S.a); step(0x57DA);   // STA ($04),Y
		cyc(3); step(0x581E);                          // JMP $581E
	}
	else
	{
		cyc(2); S.y++; nz(S.y); step(0x57DE);          // INY
		if (!branch(0x57DE, S.p & FZ, 0x57E6))
		{
			cyc(4); S.a = rd(0x1860); nz(S.a); step(0x57E3);   // dropped
			cyc(3); step(0x581E);                      // JMP $581E
		}
		else
		{
			// a command
			cyc(2); S.a = S.x; nz(S.a); step(0x57E7);  // TXA
			cyc(3); push(S.a); step(0x57E8);           // PHA
			cyc(4); S.y = SR[0x211]; nz(S.y); step(0x57EB);
			cyc(2); S.y++; nz(S.y); step(0x57EC);
			cyc(2); op_cmp(S.y, 0x10); step(0x57EE);
			if (!branch(0x57EE, !(S.p & FC), 0x57F2))
			{
				cyc(2); S.y = 0x00; nz(S.y); step(0x57F2);
			}
			cyc(4); SR[0x211] = S.y; step(0x57F5);
			cyc(4); op_cmp(S.y, SR[0x210]); step(0x57F8);
			if (!branch(0x57F8, !(S.p & FZ), 0x5807))
			{
				// full: the oldest dropped
				cyc(4); S.x = SR[0x210]; nz(S.x); step(0x57FD);
				cyc(2); S.x++; nz(S.x); step(0x57FE);
				cyc(2); op_cmp(S.x, 0x10); step(0x5800);
				if (!branch(0x5800, !(S.p & FC), 0x5804))
				{
					cyc(2); S.x = 0x00; nz(S.x); step(0x5804);
				}
				cyc(4); SR[0x210] = S.x; step(0x5807);
			}
			cyc(2); S.a = 0xB9; nz(S.a); step(0x5809);
			cyc(5); wr((uint16_t)(0x0200 + S.y), S.a); step(0x580C);
			cyc(4); S.x = rd(0x1860); nz(S.x); step(0x580F);   // the command
			cyc(2); op_cmp(S.x, 0xB9); step(0x5811);
			if (!branch(0x5811, S.p & FC, 0x581C))
			{
				cyc(4); S.a = rd(idx(0x5C9F, S.x)); nz(S.a); step(0x5816);
				if (branch(0x5816, !(S.p & FN), 0x5822))
				{
					cyc(2); op_cmp(S.a, 0x14); step(0x5824);
					if (!branch(0x5824, S.p & FC, 0x581C))
					{
						// done at once
						cyc(2); S.a = op_asl(S.a); step(0x5827);
						cyc(2); S.y = S.a; nz(S.y); step(0x5828);   // TAY
						cyc(4); S.a = rd(idx(0x5ED3, S.y)); nz(S.a); step(0x582B);
						cyc(3); push(S.a); step(0x582C);
						cyc(4); S.a = rd(idx(0x5ED2, S.y)); nz(S.a); step(0x582F);
						cyc(3); push(S.a); step(0x5830);
						rts_jump();
						return;
					}
				}
				else
				{
					cyc(2); S.a = S.x; nz(S.a); step(0x5819);   // TXA
					cyc(5); wr((uint16_t)(0x0200 + S.y), S.a); step(0x581C);
				}
			}
			cyc(4); S.a = pull(); nz(S.a); step(0x581D);   // PLA
			cyc(2); S.x = S.a; nz(S.x); step(0x581E);  // TAX
		}
	}
	cyc(4); S.a = pull(); nz(S.a); step(0x581F);       // PLA
	cyc(2); S.y = S.a; nz(S.y); step(0x5820);          // TAY
	cyc(4); S.a = pull(); nz(S.a); step(0x5821);       // PLA
	rti();
}

} // namespace

SOUND_TABLE(command,
	routine(0x4333, "do_command", do_command),
	routine(0x434C, "cmd_set_13", cmd_set_13),
	routine(0x4351, "cmd_set_18", cmd_set_18),
	routine(0x435E, "cmd_add_18", cmd_add_18),
	routine(0x436E, "cmd_table", cmd_table),
	routine(0x4379, "cmd_stop_class", cmd_stop_class),
	routine(0x4392, "cmd_stop_sound", cmd_stop_sound),
	routine(0x43B4, "cmd_stop_channel", cmd_stop_channel),
	routine(0x43D9, "cmd_release_sound", cmd_release_sound),
	routine(0x4410, "cmd_release_class", cmd_release_class),
	routine(0x4443, "reply_55", reply_55, INNER),
	routine(0x4445, "queue_reply", queue_reply),
	routine(0x4461, "cmd_62dc", cmd_62dc),
	routine(0x44A8, "nmi_status", nmi_status, NMI_END),
	routine(0x44B8, "nmi_b9", nmi_b9, NMI_END),
	routine(0x44CD, "send_main", send_main),
	routine(0x44E3, "cmd_start_sound", cmd_start_sound),
	routine(0x461D, "cmd_none", cmd_none),
	routine(0x461E, "cmd_mask", cmd_mask),
	routine(0x57AC, "nmi", nmi, RTI),
)

} // namespace sound
