// Rewritten sound routines: the reset, the power-on tests and the main loop (see sound.h).
#include "sound.h"

namespace sound {
namespace {

using namespace s65rt;

// 4157: a bad RAM byte in page 0: 0x10 to the main CPU, and stop
[[noreturn]] void hang_bad_ram()
{
	cyc(2); S.a = 0x10; nz(S.a); step(0x4159);
	cyc(4); wr(0x1874, S.a); step(0x415C);             // STA $1874
	for (;;)
	{
		cyc(3); step(0x415C);                          // JMP $415C
	}
}

// 4142: a bad RAM byte in page X (from the RAM test): page 1 stops (4157), pages 2-7 set 0x02
// to 0x10, pages 8-15 set bit 3 of 0x02.  A is kept.
void bad_ram_page()
{
	cyc(2); op_cmp(S.x, 0x01); step(0x4144);           // CPX #1
	if (branch(0x4144, S.p & FZ, 0x4157))
		hang_bad_ram();
	cyc(3); push(S.a); step(0x4147);                   // PHA
	cyc(2); op_cmp(S.x, 0x08); step(0x4149);           // CPX #8
	if (!branch(0x4149, S.p & FC, 0x414F))
	{
		cyc(2); S.a = 0x10; nz(S.a); step(0x414D);
		branch(0x414D, !(S.p & FZ), 0x4153);           // BNE (always)
	}
	else
	{
		cyc(3); S.a = SR[0x02]; nz(S.a); step(0x4151);
		cyc(2); S.a |= 0x08; nz(S.a); step(0x4153);
	}
	cyc(3); SR[0x02] = S.a; step(0x4155);
	cyc(4); S.a = pull(); nz(S.a); step(0x4156);       // PLA
	rts();
}

// 415F: the checksum of 64 pages of ROM from page X (X ends 64 higher): the bytes add up to
// 0xFF (ADC with the carry cleared before each), else the error bit A is set in 0x02
void rom_checksum()
{
	cyc(3); SR[0x11] = S.a; step(0x4161);              // the error bit
	cyc(2); S.a = 0x3F; nz(S.a); step(0x4163);
	cyc(3); SR[0x10] = S.a; step(0x4165);
	cyc(2); S.a = 0x00; nz(S.a); step(0x4167);
	cyc(3); SR[0x0E] = S.a; step(0x4169);
	do
	{
		cyc(2); S.y = 0x00; nz(S.y); step(0x416B);
		cyc(3); SR[0x0F] = S.x; step(0x416D);          // the page
		do
		{
			cyc(2); S.p &= ~FC; step(0x416E);          // CLC
			cyc(5); op_adc(rd(idx(zp16(0x0E), S.y))); step(0x4170);   // ADC ($0E),Y
			cyc(2); S.y++; nz(S.y); step(0x4171);      // INY
		} while (branch(0x4171, !(S.p & FZ), 0x416D));
		cyc(2); S.x++; nz(S.x); step(0x4174);          // INX
		cyc(5); SR[0x10]--; nz(SR[0x10]); step(0x4176);   // DEC $10
	} while (branch(0x4176, !(S.p & FN), 0x4169));
	cyc(2); op_cmp(S.a, 0xFF); step(0x417A);
	if (!branch(0x417A, S.p & FZ, 0x4182))
	{
		cyc(3); S.a = SR[0x02]; nz(S.a); step(0x417E);
		cyc(3); S.a |= SR[0x11]; nz(S.a); step(0x4180);
		cyc(3); SR[0x02] = S.a; step(0x4182);
	}
	rts();
}

// 40EA: the start of the sound program proper (also after a BRK, see the IRQ): the chips set
// up (5831, the mask 0x28 / 0x29 and 0x187A, 41EB) and the main loop, which never ends: a reply
// waiting in 0x0214 (16 bytes, read 0x0224, written 0x0225) goes to the main CPU when its latch
// is free (0x1840 bit 1), and a command from the main CPU waiting in 0x0200 (16 bytes, read
// 0x0210, written 0x0211 by the NMI) is done through 4333.  Bit 0 of 0x02 is cleared on each
// pass.
[[noreturn]] void restart()
{
	jsr(0x40EA, 0x5831);
	cyc(2); S.a = 0xFF; nz(S.a); step(0x40EF);
	cyc(2); S.a &= 0xE0; nz(S.a); step(0x40F1);
	cyc(3); SR[0x28] = S.a; step(0x40F3);
	cyc(2); S.a = 0xE0; nz(S.a); step(0x40F5);
	cyc(2); S.a ^= 0xFF; nz(S.a); step(0x40F7);
	cyc(2); S.a &= 0xFF; nz(S.a); step(0x40F9);
	cyc(3); SR[0x29] = S.a; step(0x40FB);
	cyc(4); wr(0x187A, S.a); step(0x40FE);
	jsr(0x40FE, 0x41EB);
	cyc(2); S.p &= ~FI; step(0x4102);                  // CLI

	for (;;)
	{
		cyc(3); S.a = SR[0x02]; nz(S.a); step(0x4104);
		cyc(2); S.a &= 0xFE; nz(S.a); step(0x4106);
		cyc(3); SR[0x02] = S.a; step(0x4108);
		// a reply for the main CPU, when its latch is free
		cyc(2); S.a = 0x02; nz(S.a); step(0x410A);
		cyc(4); op_bit(rd(0x1840)); step(0x410D);
		if (!branch(0x410D, !(S.p & FZ), 0x4127))
		{
			cyc(4); S.y = SR[0x224]; nz(S.y); step(0x4112);
			cyc(4); op_cmp(S.y, SR[0x225]); step(0x4115);
			if (!branch(0x4115, S.p & FZ, 0x4127))
			{
				cyc(4); S.a = SR[idx(0x0214, S.y)]; nz(S.a); step(0x411A);   // LDA $0214,Y
				cyc(4); wr(0x1874, S.a); step(0x411D);
				cyc(2); S.y++; nz(S.y); step(0x411E);
				cyc(2); op_cmp(S.y, 0x10); step(0x4120);
				if (!branch(0x4120, !(S.p & FC), 0x4124))
				{
					cyc(2); S.y = 0x00; nz(S.y); step(0x4124);
				}
				cyc(4); SR[0x224] = S.y; step(0x4127);
			}
		}
		// a command from the main CPU
		cyc(4); S.x = SR[0x210]; nz(S.x); step(0x412A);
		cyc(4); op_cmp(S.x, SR[0x211]); step(0x412D);
		if (!branch(0x412D, S.p & FZ, 0x413F))
		{
			cyc(2); S.x++; nz(S.x); step(0x4130);
			cyc(2); op_cmp(S.x, 0x10); step(0x4132);
			if (!branch(0x4132, !(S.p & FC), 0x4136))
			{
				cyc(2); S.x = 0x00; nz(S.x); step(0x4136);
			}
			cyc(4); SR[0x210] = S.x; step(0x4139);
			cyc(4); S.y = SR[idx(0x0200, S.x)]; nz(S.y); step(0x413C);   // LDY $0200,X
			jsr(0x413C, 0x4333);                       // do_command
		}
		cyc(3); step(0x4102);                          // JMP $4102
	}
}

// 4000: the reset.  Unless the self-test bit of 0x1840 (bit 2) is set, which clears page 0
// instead: the RAM test (page 0 through $0000,Y, a bad byte stops at 4157; pages 1-15 through
// ($0E),Y, a bad byte goes to 4142), the ROM checksums (4000-7FFF, 8000-BFFF, C000-FFFF: bits
// 7, 6, 5 of 0x02) and the periodic IRQ timed (its handler sets 0x00 within 65536 polls, else
// bit 2 of 0x02).  Then F568, the queues emptied, 0xFF to the main CPU, and on at 40EA.
void reset()
{
	cyc(2); S.p |= FI; step(0x4001);                   // SEI
	cyc(2); S.p &= ~FD; step(0x4002);                  // CLD
	cyc(2); S.x = 0xFF; nz(S.x); step(0x4004);
	cyc(2); S.s = S.x; step(0x4005);                   // TXS
	for (uint16_t pc = 0x4005; pc < 0x4014; pc += 5)   // 0xFF, 0x00, 0xFF to 0x187E
	{
		cyc(2); S.a = pc == 0x400A ? 0x00 : 0xFF; nz(S.a); step((uint16_t)(pc + 2));
		cyc(4); wr(0x187E, S.a); step((uint16_t)(pc + 5));
	}
	cyc(2); S.x = 0x00; nz(S.x); step(0x4016);
	cyc(4); S.a = rd(0x1840); nz(S.a); step(0x4019);
	cyc(2); S.a &= 0x04; nz(S.a); step(0x401B);
	if (!branch(0x401B, S.p & FZ, 0x402A))
	{
		// no tests: page 0 cleared
		cyc(2); S.a = 0x00; nz(S.a); step(0x401F);
		do
		{
			cyc(4); SR[S.x] = S.a; step(0x4021);       // STA $00,X
			cyc(2); S.x++; nz(S.x); step(0x4022);
		} while (branch(0x4022, !(S.p & FZ), 0x401F));
		jsr(0x4024, 0x4183);                           // ack_irq
		cyc(3); step(0x40C6);                          // JMP $40C6
		goto setup;
	}

	// page 0: each byte takes 0x01, 0x02 .. 0x80 and their complements
	cyc(2); S.y = 0x00; nz(S.y); step(0x402C);
	cyc(2); S.a = 0xFF; nz(S.a); step(0x402E);
	cyc(2); S.a = 0x01; nz(S.a); step(0x4030);
	do
	{
		for (;;)
		{
			cyc(5); SR[S.y] = S.a; step(0x4033);       // STA $0000,Y
			cyc(4); op_cmp(S.a, SR[S.y]); step(0x4036);   // CMP $0000,Y
			if (!branch(0x4036, S.p & FZ, 0x403B))
			{
				cyc(3); step(0x4157);                  // JMP $4157
				hang_bad_ram();
			}
			cyc(2); S.a ^= 0xFF; nz(S.a); step(0x403D);   // EOR #$FF
			cyc(5); SR[S.y] = S.a; step(0x4040);
			cyc(4); op_cmp(S.a, SR[S.y]); step(0x4043);
			if (!branch(0x4043, S.p & FZ, 0x4048))
			{
				cyc(3); step(0x4157);
				hang_bad_ram();
			}
			cyc(2); S.a ^= 0xFF; nz(S.a); step(0x404A);
			cyc(2); S.p &= ~FC; step(0x404B);          // CLC
			cyc(2); S.a = op_rol(S.a); step(0x404C);
			if (!branch(0x404C, !(S.p & FC), 0x4030))
				break;
		}
		cyc(5); SR[S.y] = S.a; step(0x4051);           // STA $0000,Y: 0 left
		cyc(2); S.a = op_rol(S.a); step(0x4052);
		cyc(2); S.y++; nz(S.y); step(0x4053);
	} while (branch(0x4053, !(S.p & FZ), 0x4030));
	cyc(2); S.a = 0x00; nz(S.a); step(0x4057);
	cyc(3); SR[0x0E] = S.a; step(0x4059);
	cyc(2); S.a = 0x00; nz(S.a); step(0x405B);
	cyc(3); SR[0x02] = S.a; step(0x405D);              // no errors yet

	// pages 1-15 the same through ($0E),Y
	for (;;)
	{
		cyc(2); S.x++; nz(S.x); step(0x405E);          // INX
		cyc(2); op_cmp(S.x, 0x10); step(0x4060);
		if (!branch(0x4060, !(S.p & FZ), 0x4065))
		{
			cyc(3); step(0x408D);                      // JMP $408D
			break;
		}
		cyc(3); SR[0x0F] = S.x; step(0x4067);
		cyc(2); S.y = 0x00; nz(S.y); step(0x4069);
		cyc(2); S.a = 0x01; nz(S.a); step(0x406B);
		do
		{
			for (;;)
			{
				cyc(6); wr((uint16_t)(zp16(0x0E) + S.y), S.a); step(0x406D);   // STA ($0E),Y
				cyc(5); op_cmp(S.a, rd(idx(zp16(0x0E), S.y))); step(0x406F);
				if (!branch(0x406F, S.p & FZ, 0x4074))
					jsr(0x4071, 0x4142);               // bad_ram_page
				cyc(2); S.a ^= 0xFF; nz(S.a); step(0x4076);
				cyc(6); wr((uint16_t)(zp16(0x0E) + S.y), S.a); step(0x4078);
				cyc(5); op_cmp(S.a, rd(idx(zp16(0x0E), S.y))); step(0x407A);
				if (!branch(0x407A, S.p & FZ, 0x407F))
					jsr(0x407C, 0x4142);
				cyc(2); S.a ^= 0xFF; nz(S.a); step(0x4081);
				cyc(2); S.p &= ~FC; step(0x4082);
				cyc(2); S.a = op_rol(S.a); step(0x4083);
				if (!branch(0x4083, !(S.p & FC), 0x406B))
					break;
			}
			cyc(6); wr((uint16_t)(zp16(0x0E) + S.y), S.a); step(0x4087);
			cyc(2); S.a = op_rol(S.a); step(0x4088);
			cyc(2); S.y++; nz(S.y); step(0x4089);
		} while (branch(0x4089, !(S.p & FZ), 0x406B));
		branch(0x408B, S.p & FZ, 0x405D);              // BEQ (always)
	}

	// the ROM checksums
	cyc(2); S.x = 0xFF; nz(S.x); step(0x408F);
	cyc(2); S.s = S.x; step(0x4090);                   // TXS
	cyc(2); S.x = 0x40; nz(S.x); step(0x4092);
	cyc(2); S.a = 0x80; nz(S.a); step(0x4094);
	jsr(0x4094, 0x415F);                               // rom_checksum
	cyc(2); S.a = 0x40; nz(S.a); step(0x4099);
	jsr(0x4099, 0x415F);
	cyc(2); S.a = 0x20; nz(S.a); step(0x409E);
	jsr(0x409E, 0x415F);

	// the periodic IRQ: its handler sets 0x00
	cyc(2); S.a = 0xFF; nz(S.a); step(0x40A3);
	cyc(3); SR[0x01] = S.a; step(0x40A5);
	jsr(0x40A5, 0x4183);                               // ack_irq
	cyc(2); S.a = 0x00; nz(S.a); step(0x40AA);
	cyc(3); SR[0x00] = S.a; step(0x40AC);
	cyc(3); SR[0x0E] = S.a; step(0x40AE);
	cyc(3); SR[0x0F] = S.a; step(0x40B0);
	cyc(2); S.p &= ~FI; step(0x40B1);                  // CLI
	for (;;)
	{
		cyc(3); S.a = SR[0x00]; nz(S.a); step(0x40B3);
		if (branch(0x40B3, !(S.p & FZ), 0x40C6))
			break;
		for (uint16_t pc = 0x40B6; pc <= 0x40B8; pc++)
		{
			cyc(2); step(pc);                          // NOP
		}
		cyc(5); SR[0x0E]++; nz(SR[0x0E]); step(0x40BA);
		if (branch(0x40BA, !(S.p & FZ), 0x40B1))
			continue;
		cyc(5); SR[0x0F]++; nz(SR[0x0F]); step(0x40BE);
		if (branch(0x40BE, !(S.p & FZ), 0x40B1))
			continue;
		cyc(3); S.a = SR[0x02]; nz(S.a); step(0x40C2); // no IRQ
		cyc(2); S.a |= 0x04; nz(S.a); step(0x40C4);
		cyc(3); SR[0x02] = S.a; step(0x40C6);
		break;
	}

setup:
	cyc(2); S.p |= FI; step(0x40C7);                   // SEI
	cyc(2); S.a = 0x00; nz(S.a); step(0x40C9);
	cyc(3); SR[0x01] = S.a; step(0x40CB);
	jsr(0x40CB, 0xF568);
	cyc(2); S.x = 0xFF; nz(S.x); step(0x40D0);
	cyc(4); SR[0x213] = S.x; step(0x40D3);
	cyc(2); S.x++; nz(S.x); step(0x40D4);              // INX: 0
	cyc(3); SR[0x13] = S.x; step(0x40D6);
	cyc(4); SR[0x224] = S.x; step(0x40D9);             // the queues
	cyc(4); SR[0x225] = S.x; step(0x40DC);
	cyc(4); SR[0x210] = S.x; step(0x40DF);
	cyc(4); SR[0x211] = S.x; step(0x40E2);
	cyc(4); S.a = rd(0x1860); nz(S.a); step(0x40E5);   // the main CPU's latch read
	cyc(2); S.x = 0xFF; nz(S.x); step(0x40E7);
	cyc(4); wr(0x1874, S.x); step(0x40EA);             // 0xFF to the main CPU
	restart();
}

// 41C8: the work of a periodic IRQ: 5883 three times, the players of channels 0 and 2 (5012)
// on even ticks of 0x00, channel 1 on odd ones, then 5883 again
void irq_work()
{
	jsr(0x41C8, 0x5883);
	jsr(0x41CB, 0x5883);
	jsr(0x41CE, 0x5883);
	cyc(3); S.a = SR[0x00]; nz(S.a); step(0x41D3);
	cyc(2); S.a = op_lsr(S.a); step(0x41D4);
	if (!branch(0x41D4, !(S.p & FC), 0x41DE))
	{
		cyc(2); S.x = 0x01; nz(S.x); step(0x41D8);
		jsr(0x41D8, 0x5012);
		jmp(0x5883);                                   // JMP $5883
		return;
	}
	cyc(2); S.x = 0x00; nz(S.x); step(0x41E0);
	jsr(0x41E0, 0x5012);
	cyc(2); S.x = 0x02; nz(S.x); step(0x41E5);
	jsr(0x41E5, 0x5012);
	jmp(0x5883);
}

// 4187: the periodic IRQ: acknowledged, bit 2 of 0x02 cleared and the tick 0x00 counted.  While
// 0x01 is set (the reset's timing) that is all; a BRK restarts at 40EA.  Otherwise the
// countdown 0x2A, at 0 the mask 0x29 goes to 0x187A; then 41C8 and F14D.  A, X, Y kept.
void irq()
{
	cyc(3); push(S.a); step(0x4188);                   // PHA
	cyc(2); S.a = S.x; nz(S.a); step(0x4189);          // TXA
	cyc(3); push(S.a); step(0x418A);                   // PHA
	cyc(2); S.p &= ~FD; step(0x418B);                  // CLD
	cyc(4); wr(0x1878, S.a); step(0x418E);             // the IRQ acknowledged
	cyc(3); S.a = SR[0x02]; nz(S.a); step(0x4190);
	cyc(2); S.a &= 0xFB; nz(S.a); step(0x4192);
	cyc(3); SR[0x02] = S.a; step(0x4194);
	cyc(3); S.a = SR[0x01]; nz(S.a); step(0x4196);
	if (!branch(0x4196, S.p & FZ, 0x419D))
	{
		cyc(5); SR[0x00]++; nz(SR[0x00]); step(0x419A);   // INC $00
		cyc(3); step(0x41C4);                          // JMP $41C4
	}
	else
	{
		cyc(2); S.x = S.s; nz(S.x); step(0x419E);      // TSX
		cyc(4); S.a = rd(idx(0x0103, S.x)); nz(S.a); step(0x41A1);   // the stacked P
		cyc(2); S.a &= 0x10; nz(S.a); step(0x41A3);
		if (!branch(0x41A3, S.p & FZ, 0x41AB))
		{
			cyc(2); S.x = 0xFF; nz(S.x); step(0x41A7); // a BRK: start again
			cyc(2); S.s = S.x; step(0x41A8);           // TXS
			cyc(3); step(0x40EA);                      // JMP $40EA
			leave_to(0x40EA);
		}
		cyc(2); S.a = S.y; nz(S.a); step(0x41AC);      // TYA
		cyc(3); push(S.a); step(0x41AD);               // PHA
		cyc(5); SR[0x00]++; nz(SR[0x00]); step(0x41AF);
		cyc(3); S.a = SR[0x2A]; nz(S.a); step(0x41B1);
		if (!branch(0x41B1, S.p & FZ, 0x41BC))
		{
			cyc(5); SR[0x2A]--; nz(SR[0x2A]); step(0x41B5);   // DEC $2A
			if (!branch(0x41B5, !(S.p & FZ), 0x41BC))
			{
				cyc(3); S.a = SR[0x29]; nz(S.a); step(0x41B9);
				cyc(4); wr(0x187A, S.a); step(0x41BC);
			}
		}
		jsr(0x41BC, 0x41C8);                           // irq_work
		jsr(0x41BF, 0xF14D);
		cyc(4); S.a = pull(); nz(S.a); step(0x41C3);   // PLA
		cyc(2); S.y = S.a; nz(S.y); step(0x41C4);      // TAY
	}
	cyc(4); S.a = pull(); nz(S.a); step(0x41C5);       // PLA
	cyc(2); S.x = S.a; nz(S.x); step(0x41C6);          // TAX
	cyc(4); S.a = pull(); nz(S.a); step(0x41C7);       // PLA
	rti();
}

// 41EB: the sound reset, with interrupts off: the node list (429A), the queues and the channel
// tables cleared; for each of the three players (2, 1, 0; tables 0x57A0 / 0x57A3 / 0x57A6): a
// player without chip (0x57A6 = 0) gets its block at 0x08 cleared (offset 8 and 15, 16, 17 =
// 0, 1, 2), the YM2151 one has all eight channels keyed off (register 8) and the busy flag 0x0D
// and bit 1 of 0x02 cleared
void sound_reset()
{
	cyc(3); push((uint8_t)(S.p | FB | FU)); step(0x41EC);   // PHP
	cyc(2); S.p |= FI; step(0x41ED);                   // SEI
	jsr(0x41ED, 0x429A);                               // node_list
	cyc(2); S.a = 0x00; nz(S.a); step(0x41F2);
	static const uint16_t cleared[5] = {0x0224, 0x0225, 0x0226, 0x070A, 0x070B};
	for (int i = 0; i < 5; i++)
	{
		cyc(4); SR[cleared[i]] = S.a; step((uint16_t)(0x41F5 + 3 * i));
	}
	cyc(2); S.x = 0x27; nz(S.x); step(0x4203);
	do
	{
		cyc(5); SR[0x06C0 + S.x] = S.a; step(0x4206);   // STA $06C0,X
		cyc(2); S.x--; nz(S.x); step(0x4207);
	} while (branch(0x4207, !(S.p & FN), 0x4203));
	cyc(2); S.x = 0x17; nz(S.x); step(0x420B);
	do
	{
		static const uint16_t table[5] = {0x0348, 0x0270, 0x0288, 0x03A8, 0x0570};
		for (int i = 0; i < 5; i++)
		{
			cyc(5); SR[table[i] + S.x] = S.a; step((uint16_t)(0x420E + 3 * i));
		}
		cyc(2); S.x--; nz(S.x); step(0x421B);
	} while (branch(0x421B, !(S.p & FN), 0x420B));
	cyc(2); S.x = 0x02; nz(S.x); step(0x421F);
	for (;;)
	{
		cyc(4); S.a = rd(idx(0x57A0, S.x)); nz(S.a); step(0x4222);
		cyc(3); SR[0x08] = S.a; step(0x4224);
		cyc(4); S.a = rd(idx(0x57A3, S.x)); nz(S.a); step(0x4227);
		cyc(3); SR[0x09] = S.a; step(0x4229);
		cyc(4); S.a = rd(idx(0x57A6, S.x)); nz(S.a); step(0x422C);
		if (!branch(0x422C, !(S.p & FZ), 0x4263))
		{
			cyc(2); S.y = 0x08; nz(S.y); step(0x4230);
			cyc(6); wr((uint16_t)(zp16(0x08) + S.y), S.a); step(0x4232);   // STA ($08),Y
			cyc(3); SR[0x11] = S.x; step(0x4234);
			cyc(2); S.p &= ~FC; step(0x4235);          // CLC
			cyc(3); S.a = SR[0x08]; nz(S.a); step(0x4237);
			cyc(3); SR[0x12] = S.a; step(0x4239);
			cyc(2); S.a = 0x0F; nz(S.a); step(0x423B);
			cyc(3); op_adc(SR[0x08]); step(0x423D);
			cyc(3); SR[0x08] = S.a; step(0x423F);
			cyc(2); S.x = 0x00; nz(S.x); step(0x4241);
			cyc(2); S.a = S.x; nz(S.a); step(0x4242);  // TXA
			cyc(6); wr(zp16((uint8_t)(0x08 + S.x)), S.a); step(0x4244);   // STA ($08,X)
			do
			{
				cyc(2); op_adc(0x01); step(0x4246);
				cyc(2); op_cmp(S.a, 0x03); step(0x4248);
			} while (branch(0x4248, !(S.p & FZ), 0x4244));
			cyc(6); wr(zp16((uint8_t)(0x08 + S.x)), S.a); step(0x424C);
			cyc(3); S.x = SR[0x11]; nz(S.x); step(0x424E);
			cyc(3); S.a = SR[0x12]; nz(S.a); step(0x4250);
			cyc(3); SR[0x08] = S.a; step(0x4252);
			cyc(2); S.y = 0x08; nz(S.y); step(0x4254);
			cyc(2); S.a = 0x00; nz(S.a); step(0x4256);
			cyc(6); wr((uint16_t)(zp16(0x08) + S.y), S.a); step(0x4258);
			cyc(2); S.a = 0x00; nz(S.a); step(0x425A);
			cyc(2); S.y = 0x08; nz(S.y); step(0x425C);
			do
			{
				cyc(2); S.y--; nz(S.y); step(0x425D);  // DEY
				cyc(6); wr((uint16_t)(zp16(0x08) + S.y), S.a); step(0x425F);
			} while (branch(0x425F, !(S.p & FZ), 0x425C));
			branch(0x4261, S.p & FZ, 0x4292);          // BEQ (always)
		}
		else
		{
			jsr(0x4263, 0x4FF5);                       // wait_ym
			cyc(2); S.y = 0x08; nz(S.y); step(0x4268);
			cyc(4); wr(0x1850, S.y); step(0x426B);     // register 8: key on / off
			cyc(2); S.a = 0x00; nz(S.a); step(0x426D);
			cyc(3); SR[0x0D] = S.a; step(0x426F);
			jsr(0x426F, 0x4FF5);
			cyc(4); wr(0x1850, S.y); step(0x4275);
			cyc(3); SR[0x0D] = S.a; step(0x4277);
			jsr(0x4277, 0x4FF5);
			cyc(2); S.y = 0x07; nz(S.y); step(0x427C);
			do
			{
				cyc(3); SR[0x0D] = S.a; step(0x427E);
				jsr(0x427E, 0x4FF5);
				cyc(4); wr(0x1851, S.y); step(0x4284); // channel Y off
				cyc(2); S.y--; nz(S.y); step(0x4285);
			} while (branch(0x4285, !(S.p & FN), 0x427C));
			cyc(3); SR[0x0D] = S.a; step(0x4289);
			cyc(3); S.a = SR[0x02]; nz(S.a); step(0x428B);
			cyc(2); S.a &= 0xFD; nz(S.a); step(0x428D);
			cyc(3); SR[0x02] = S.a; step(0x428F);
			jsr(0x428F, 0x4FF5);
		}
		cyc(2); S.x--; nz(S.x); step(0x4293);          // DEX
		if (branch(0x4293, S.p & FN, 0x4298))
			break;
		cyc(3); step(0x421F);                          // JMP $421F
	}
	cyc(4); S.p = (uint8_t)((pull() | FU) & ~FB); step(0x4299);   // PLP
	rts();
}

} // namespace

SOUND_TABLE(main,
	routine(0x4000, "reset", reset, NOTEST),
	routine(0x40EA, "restart", restart, NOTEST | INNER),
	routine(0x4142, "bad_ram_page", bad_ram_page),
	routine(0x415F, "rom_checksum", rom_checksum),
	routine(0x4187, "irq", irq, RTI),
	routine(0x41C8, "irq_work", irq_work),
	routine(0x41EB, "sound_reset", sound_reset),
)

} // namespace sound
