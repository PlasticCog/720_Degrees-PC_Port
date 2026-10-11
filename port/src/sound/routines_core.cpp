// Rewritten sound routines: small helpers (see sound.h).  Each instruction of the original is a
// line: its cycles, its action and flags, and the step to the next instruction.
#include "sound.h"

namespace sound {
namespace {

using namespace s65rt;

// 4183: acknowledge the periodic IRQ (any write to 0x1878)
void ack_irq()
{
	cyc(4); wr(0x1878, S.a); step(0x4186);             // STA $1878
	rts();
}

// 4FF5: wait until the YM2151 is not busy (status bit 7 at 0x1851), at most 255 polls; when it
// stays busy, 0x0D is set (no more waiting from then on) and bit 1 of 0x02.  A is kept; nothing
// to wait for while 0x0D is negative.
void wait_ym()
{
	cyc(3); op_bit(SR[0x0D]); step(0x4FF7);            // BIT $0D
	if (branch(0x4FF7, S.p & FN, 0x5011))
	{
		rts();
		return;
	}
	cyc(3); push(S.a); step(0x4FFA);                   // PHA
	cyc(2); S.a = 0; nz(S.a); step(0x4FFC);            // LDA #0
	cyc(2); S.p &= ~FC; step(0x4FFD);                  // CLC
	for (;;)
	{
		cyc(4); op_bit(rd(0x1851)); step(0x5000);      // BIT $1851
		if (branch(0x5000, !(S.p & FN), 0x5010))
			break;
		cyc(2); op_adc(0x01); step(0x5004);            // ADC #1
		cyc(2); op_cmp(S.a, 0xFF); step(0x5006);       // CMP #$FF
		if (!branch(0x5006, !(S.p & FZ), 0x4FFD))
		{
			cyc(3); SR[0x0D] = S.a; step(0x500A);      // STA $0D: busy for good
			cyc(3); S.a = SR[0x02]; nz(S.a); step(0x500C);
			cyc(2); S.a |= 0x02; nz(S.a); step(0x500E);
			cyc(3); SR[0x02] = S.a; step(0x5010);
			break;
		}
	}
	cyc(4); S.a = pull(); nz(S.a); step(0x5011);       // PLA
	rts();
}

// F47B: the counter 0x8C, 0 .. 0x15 and round again
void count_8c()
{
	cyc(5); SR[0x8C]++; nz(SR[0x8C]); step(0xF47D);    // INC $8C
	cyc(3); S.a = SR[0x8C]; nz(S.a); step(0xF47F);
	cyc(2); op_cmp(S.a, 0x16); step(0xF481);
	if (!branch(0xF481, !(S.p & FC), 0xF487))
	{
		cyc(2); S.a = 0; nz(S.a); step(0xF485);
		cyc(3); SR[0x8C] = S.a; step(0xF487);
	}
	rts();
}

// F488: A * 10 (A * 2 kept in 0x65; the carry of the ADC as the shifts leave it)
void times10()
{
	cyc(2); S.a = op_asl(S.a); step(0xF489);
	cyc(3); SR[0x65] = S.a; step(0xF48B);
	cyc(2); S.a = op_asl(S.a); step(0xF48C);
	cyc(2); S.a = op_asl(S.a); step(0xF48D);
	cyc(3); op_adc(SR[0x65]); step(0xF48F);
	rts();
}

// F490: the pointer 0x8D/0x8E = 0x1000 + A * 32
void ptr_1000()
{
	cyc(2); S.x = 0; nz(S.x); step(0xF492);            // LDX #0
	cyc(3); SR[0x65] = S.x; step(0xF494);
	cyc(2); S.a = op_asl(S.a); step(0xF495);
	cyc(2); S.a = op_asl(S.a); step(0xF496);
	cyc(2); S.a = op_asl(S.a); step(0xF497);
	cyc(5); SR[0x65] = op_rol(SR[0x65]); step(0xF499);
	cyc(2); S.a = op_asl(S.a); step(0xF49A);
	cyc(5); SR[0x65] = op_rol(SR[0x65]); step(0xF49C);
	cyc(2); S.p &= ~FC; step(0xF49D);                  // CLC
	cyc(2); op_adc(0x00); step(0xF49F);
	cyc(3); SR[0x8D] = S.a; step(0xF4A1);
	cyc(3); S.a = SR[0x65]; nz(S.a); step(0xF4A3);
	cyc(2); op_adc(0x10); step(0xF4A5);
	cyc(3); SR[0x8E] = S.a; step(0xF4A7);
	rts();
}

// FE28: a delay of about 0xBB * 8 * 4 loop passes (A and X kept)
void delay()
{
	cyc(3); push(S.a); step(0xFE29);                   // PHA
	cyc(2); S.a = S.x; nz(S.a); step(0xFE2A);          // TXA
	cyc(3); push(S.a); step(0xFE2B);                   // PHA
	cyc(2); S.a = 0x0F; nz(S.a); step(0xFE2D);
	for (;;)
	{
		cyc(3); push(S.a); step(0xFE2E);               // PHA
		cyc(2); S.p |= FC; step(0xFE2F);               // SEC
		cyc(2); S.a = 0x08; nz(S.a); step(0xFE31);
		for (;;)
		{
			cyc(2); S.x = 0xBB; nz(S.x); step(0xFE33);
			for (;;)
			{
				cyc(2); S.x--; nz(S.x); step(0xFE34);  // DEX
				if (!branch(0xFE34, !(S.p & FZ), 0xFE33))
					break;
			}
			cyc(2); op_sbc(0x01); step(0xFE38);
			if (!branch(0xFE38, !(S.p & FZ), 0xFE31))
				break;
		}
		cyc(4); S.a = pull(); nz(S.a); step(0xFE3B);   // PLA
		cyc(2); S.a = op_lsr(S.a); step(0xFE3C);
		if (!branch(0xFE3C, !(S.p & FZ), 0xFE2D))
			break;
	}
	cyc(4); S.a = pull(); nz(S.a); step(0xFE3F);       // PLA
	cyc(2); S.x = S.a; nz(S.x); step(0xFE40);          // TAX
	cyc(4); S.a = pull(); nz(S.a); step(0xFE41);       // PLA
	rts();
}

// FE1D: A into 0x1160 + Y and 0x0C48 + Y, Y + 1, then the delay
void store_and_delay()
{
	cyc(5); wr((uint16_t)(0x1160 + S.y), S.a); step(0xFE20);   // STA $1160,Y
	cyc(5); wr((uint16_t)(0x0C48 + S.y), S.a); step(0xFE23);   // STA $0C48,Y
	cyc(2); S.y++; nz(S.y); step(0xFE24);              // INY
	jsr(0xFE24, 0xFE28);                               // delay
	rts();
}

// The note nodes: 4-byte nodes from 0x0815 on (node n at 0x0815 + 4 * (n - 1)), linked through
// their first byte; 0x14 is the first free one.

// 42DC: the address of node A in 0x15 / 0x16 (A - 1 in 0x0E, Y = 0); A kept
void node_addr()
{
	cyc(3); push(S.a); step(0x42DD);                   // PHA
	cyc(2); S.y = S.a; nz(S.y); step(0x42DE);          // TAY
	cyc(2); S.y--; nz(S.y); step(0x42DF);              // DEY
	cyc(3); SR[0x15] = S.y; step(0x42E1);
	cyc(3); SR[0x0E] = S.y; step(0x42E3);
	cyc(2); S.y = 0x00; nz(S.y); step(0x42E5);
	cyc(3); SR[0x16] = S.y; step(0x42E7);
	cyc(5); SR[0x15] = op_asl(SR[0x15]); step(0x42E9);
	cyc(5); SR[0x16] = op_rol(SR[0x16]); step(0x42EB);
	cyc(5); SR[0x15] = op_asl(SR[0x15]); step(0x42ED);
	cyc(5); SR[0x16] = op_rol(SR[0x16]); step(0x42EF);
	cyc(2); S.a = 0x15; nz(S.a); step(0x42F1);
	cyc(2); S.p &= ~FC; step(0x42F2);                  // CLC
	cyc(3); op_adc(SR[0x15]); step(0x42F4);
	cyc(3); SR[0x15] = S.a; step(0x42F6);
	cyc(2); S.a = 0x08; nz(S.a); step(0x42F8);
	cyc(3); op_adc(SR[0x16]); step(0x42FA);
	cyc(3); SR[0x16] = S.a; step(0x42FC);
	cyc(4); S.a = pull(); nz(S.a); step(0x42FD);       // PLA
	rts();
}

// 429A: all nodes free: node n links to n + 1, up to 0xC8 or the end of page 0x0A; the last
// one ends the list
void node_list()
{
	cyc(2); S.a = 0x01; nz(S.a); step(0x429C);
	cyc(3); SR[0x14] = S.a; step(0x429E);
	jsr(0x429E, 0x42DC);                               // node_addr
	cyc(2); S.a = 0x02; nz(S.a); step(0x42A3);
	for (;;)
	{
		cyc(6); wr((uint16_t)(zp16(0x15) + S.y), S.a); step(0x42A5);   // STA ($15),Y
		cyc(2); S.x = S.a; nz(S.x); step(0x42A6);      // TAX
		cyc(2); S.x++; nz(S.x); step(0x42A7);          // INX
		cyc(3); S.a = SR[0x15]; nz(S.a); step(0x42A9);
		cyc(2); S.p &= ~FC; step(0x42AA);              // CLC
		cyc(2); op_adc(0x04); step(0x42AC);
		cyc(3); SR[0x15] = S.a; step(0x42AE);
		if (!branch(0x42AE, !(S.p & FC), 0x42B8))
		{
			cyc(5); SR[0x16]++; nz(SR[0x16]); step(0x42B2);   // INC $16
			cyc(2); S.a = 0x0B; nz(S.a); step(0x42B4);
			cyc(3); op_cmp(S.a, SR[0x16]); step(0x42B6);
			if (branch(0x42B6, S.p & FZ, 0x42BD))
				break;
		}
		cyc(2); S.a = S.x; nz(S.a); step(0x42B9);      // TXA
		cyc(2); op_cmp(S.a, 0xC8); step(0x42BB);
		if (!branch(0x42BB, !(S.p & FC), 0x42A3))
			break;
	}
	cyc(5); SR[0x16]--; nz(SR[0x16]); step(0x42BF);    // DEC $16: back to the last node
	cyc(3); S.a = SR[0x15]; nz(S.a); step(0x42C1);
	cyc(2); S.p |= FC; step(0x42C2);                   // SEC
	cyc(2); op_sbc(0x04); step(0x42C4);
	cyc(3); SR[0x15] = S.a; step(0x42C6);
	cyc(2); S.a = 0x00; nz(S.a); step(0x42C8);
	cyc(6); wr((uint16_t)(zp16(0x15) + S.y), S.a); step(0x42CA);
	rts();
}

// 42CB: take the first free node: A = its number (0x15 / 0x16 its address), the list goes on
// with the one it links to.  With none left behind it A = 0, and the RTS goes astray (the
// original pushes A without pulling it).
void node_take()
{
	cyc(3); S.a = SR[0x14]; nz(S.a); step(0x42CD);
	jsr(0x42CD, 0x42DC);                               // node_addr
	cyc(3); push(S.a); step(0x42D1);                   // PHA
	cyc(5); S.a = rd(idx(zp16(0x15), S.y)); nz(S.a); step(0x42D3);   // LDA ($15),Y
	if (branch(0x42D3, S.p & FZ, 0x42D9))
	{
		cyc(2); S.a = 0x00; nz(S.a); step(0x42DB);
		rts();
		return;
	}
	cyc(3); SR[0x14] = S.a; step(0x42D7);
	cyc(4); S.a = pull(); nz(S.a); step(0x42D8);       // PLA
	rts();
}

// 42FE: the two node chains of channel X (0x05E8 and 0x05D0) given back to the free list; Y kept
void node_free()
{
	cyc(2); S.a = S.y; nz(S.a); step(0x42FF);          // TYA
	cyc(3); push(S.a); step(0x4300);                   // PHA
	static const struct { uint16_t chain, pc; } part[2] = {{0x05E8, 0x4300}, {0x05D0, 0x4318}};
	for (const auto &c : part)
	{
		for (;;)
		{
			cyc(4); S.a = rd(idx(c.chain, S.x)); nz(S.a); step((uint16_t)(c.pc + 3));
			if (branch((uint16_t)(c.pc + 3), S.p & FZ, (uint16_t)(c.pc + 0x18)))
				break;
			jsr((uint16_t)(c.pc + 5), 0x42DC);         // node_addr
			cyc(3); push(S.a); step((uint16_t)(c.pc + 9));   // PHA
			cyc(5); S.a = rd(idx(zp16(0x15), S.y)); nz(S.a); step((uint16_t)(c.pc + 0x0B));
			cyc(5); wr((uint16_t)(c.chain + S.x), S.a); step((uint16_t)(c.pc + 0x0E));
			cyc(3); S.a = SR[0x14]; nz(S.a); step((uint16_t)(c.pc + 0x10));
			cyc(6); wr((uint16_t)(zp16(0x15) + S.y), S.a); step((uint16_t)(c.pc + 0x12));
			cyc(4); S.a = pull(); nz(S.a); step((uint16_t)(c.pc + 0x13));   // PLA
			cyc(3); SR[0x14] = S.a; step((uint16_t)(c.pc + 0x15));
			cyc(3); step(c.pc);                        // JMP
		}
	}
	cyc(4); S.a = pull(); nz(S.a); step(0x4331);       // PLA
	cyc(2); S.y = S.a; nz(S.y); step(0x4332);          // TAY
	rts();
}

} // namespace

SOUND_TABLE(core,
	routine(0x429A, "node_list", node_list),
	routine(0x42CB, "node_take", node_take),
	routine(0x42DC, "node_addr", node_addr),
	routine(0x42FE, "node_free", node_free),
	routine(0x4183, "ack_irq", ack_irq),
	routine(0x4FF5, "wait_ym", wait_ym),
	routine(0xF47B, "count_8c", count_8c),
	routine(0xF488, "times10", times10),
	routine(0xF490, "ptr_1000", ptr_1000),
	routine(0xFE1D, "store_and_delay", store_and_delay),
	routine(0xFE28, "delay", delay),
)

} // namespace sound
