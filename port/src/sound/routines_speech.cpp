// Rewritten sound routines: speech (see sound.h).  A phrase is a run of bytes for the TMS5220
// (pointer 0x2B / 0x2C, count 0x2D / 0x2E), fed at the IRQs while the chip is ready; 0x2F is the
// state (0x80: just started, 1-0x7F: counting down, 0xFF: ending, 0: idle).  Phrases asked for
// while one plays wait in a queue of 8 (0x070C, read 0x070A, written 0x070B).
#include "sound.h"

namespace sound {
namespace {

using namespace s65rt;

void php_sei(uint16_t pc)
{
	cyc(3); push((uint8_t)(S.p | FB | FU)); step((uint16_t)(pc + 1));
	cyc(2); S.p |= FI; step((uint16_t)(pc + 2));
}
void plp(uint16_t pc)
{
	cyc(4); S.p = (uint8_t)((pull() | FU) & ~FB); step((uint16_t)(pc + 1));
}

// 5831: speech off: the queue emptied, the speech bit of 0x33 (bit 2) cleared, the chip reset
// (0x1872), and the state set as after a phrase of 0x20 bytes from 0x5863
void speech_init()
{
	php_sei(0x5831);
	cyc(2); S.y = 0x00; nz(S.y); step(0x5835);
	cyc(4); SR[0x70A] = S.y; step(0x5838);
	cyc(4); SR[0x70B] = S.y; step(0x583B);
	cyc(3); S.a = SR[0x33]; nz(S.a); step(0x583D);
	cyc(2); S.a &= 0xFB; nz(S.a); step(0x583F);
	cyc(3); SR[0x33] = S.a; step(0x5841);
	cyc(4); wr(0x187C, S.a); step(0x5844);
	cyc(4); wr(0x1872, S.y); step(0x5847);
	cyc(2); S.y--; nz(S.y); step(0x5848);              // DEY
	cyc(3); SR[0x30] = S.y; step(0x584A);
	cyc(2); S.a = 0x58; nz(S.a); step(0x584C);
	cyc(3); SR[0x2C] = S.a; step(0x584E);
	cyc(2); S.a = 0x63; nz(S.a); step(0x5850);
	cyc(3); SR[0x2B] = S.a; step(0x5852);
	cyc(2); S.a = 0x00; nz(S.a); step(0x5854);
	cyc(3); SR[0x2E] = S.a; step(0x5856);
	cyc(2); S.a = 0x20; nz(S.a); step(0x5858);
	cyc(3); SR[0x2D] = S.a; step(0x585A);
	cyc(4); S.a = SR[0x0000]; nz(S.a); step(0x585D);   // LDA $0000
	cyc(2); S.a = 0x80; nz(S.a); step(0x585F);
	cyc(3); SR[0x2F] = S.a; step(0x5861);
	plp(0x5861);
	rts();
}

// 5903: say phrase A: the speech bit of 0x33 by the phrase's 0x6258 bit 7 (to 0x187C), its
// bytes (pointer from 0xC1CD + 2 * 0x621C[A], count from 0xC24B + the same), state 0x80, and
// the mixer volumes 0x187A from the mask 0x28 and 0x29 lowered by the phrase's 0x6258 levels
void speak()
{
	php_sei(0x5903);
	cyc(3); push(S.a); step(0x5906);                   // PHA
	cyc(2); S.y = S.a; nz(S.y); step(0x5907);          // TAY
	cyc(4); S.a = rd(idx(0x6258, S.y)); nz(S.a); step(0x590A);
	if (!branch(0x590A, !(S.p & FN), 0x5912))
	{
		cyc(3); S.a = SR[0x33]; nz(S.a); step(0x590E);
		cyc(2); S.a |= 0x04; nz(S.a); step(0x5910);
		branch(0x5910, !(S.p & FZ), 0x5918);           // BNE (always; 0x33 itself unchanged)
	}
	else
	{
		cyc(3); S.a = SR[0x33]; nz(S.a); step(0x5914);
		cyc(2); S.a &= 0xFB; nz(S.a); step(0x5916);
		cyc(3); SR[0x33] = S.a; step(0x5918);
	}
	cyc(4); wr(0x187C, S.a); step(0x591B);
	cyc(4); S.a = pull(); nz(S.a); step(0x591C);       // PLA
	cyc(3); push(S.a); step(0x591D);                   // PHA
	cyc(2); S.y = S.a; nz(S.y); step(0x591E);          // TAY
	cyc(2); S.a = 0x00; nz(S.a); step(0x5920);
	cyc(3); SR[0x32] = S.a; step(0x5922);
	cyc(4); S.a = rd(idx(0x621C, S.y)); nz(S.a); step(0x5925);
	cyc(2); S.a = op_asl(S.a); step(0x5926);
	cyc(3); SR[0x31] = S.a; step(0x5928);
	cyc(3); push(S.a); step(0x5929);                   // PHA
	cyc(5); SR[0x32] = op_rol(SR[0x32]); step(0x592B);
	cyc(3); S.a = SR[0x32]; nz(S.a); step(0x592D);
	cyc(3); push(S.a); step(0x592E);                   // PHA
	// 0x31 / 0x32 + base, then a word from there into lo / hi
	auto table = [](uint16_t pc, uint8_t lo_base, uint8_t hi_base, uint8_t lo, uint8_t hi, bool clear_y) {
		cyc(2); S.a = lo_base; nz(S.a); step((uint16_t)(pc + 2));
		cyc(2); S.p &= ~FC; step((uint16_t)(pc + 3));  // CLC
		cyc(3); op_adc(SR[0x31]); step((uint16_t)(pc + 5));
		cyc(3); SR[0x31] = S.a; step((uint16_t)(pc + 7));
		cyc(2); S.a = hi_base; nz(S.a); step((uint16_t)(pc + 9));
		cyc(3); op_adc(SR[0x32]); step((uint16_t)(pc + 0x0B));
		cyc(3); SR[0x32] = S.a; step((uint16_t)(pc + 0x0D));
		uint16_t at = (uint16_t)(pc + 0x0D);
		if (clear_y)
		{
			cyc(2); S.y = 0x00; nz(S.y); step((uint16_t)(at + 2));
			at = (uint16_t)(at + 2);
		}
		cyc(5); S.a = rd(idx(zp16(0x31), S.y)); nz(S.a); step((uint16_t)(at + 2));   // LDA ($31),Y
		cyc(3); SR[lo] = S.a; step((uint16_t)(at + 4));
		cyc(2); S.y++; nz(S.y); step((uint16_t)(at + 5));   // INY
		cyc(5); S.a = rd(idx(zp16(0x31), S.y)); nz(S.a); step((uint16_t)(at + 7));
		cyc(3); SR[hi] = S.a; step((uint16_t)(at + 9));
	};
	table(0x592E, 0xCD, 0xC1, 0x2B, 0x2C, true);       // the bytes
	cyc(2); S.y--; nz(S.y); step(0x5947);              // DEY
	cyc(4); S.a = pull(); nz(S.a); step(0x5948);       // PLA
	cyc(3); SR[0x32] = S.a; step(0x594A);
	cyc(4); S.a = pull(); nz(S.a); step(0x594B);
	cyc(3); SR[0x31] = S.a; step(0x594D);
	table(0x594D, 0x4B, 0xC2, 0x2D, 0x2E, false);      // the count
	cyc(2); S.a = 0x80; nz(S.a); step(0x5965);
	cyc(3); SR[0x2F] = S.a; step(0x5967);
	cyc(4); S.a = pull(); nz(S.a); step(0x5968);       // PLA
	cyc(2); S.y = S.a; nz(S.y); step(0x5969);          // TAY
	cyc(2); S.a = 0x00; nz(S.a); step(0x596B);
	cyc(3); SR[0x2A] = S.a; step(0x596D);
	// the volumes
	cyc(4); S.a = rd(idx(0x6258, S.y)); nz(S.a); step(0x5970);
	cyc(2); S.a &= 0x0F; nz(S.a); step(0x5972);
	cyc(3); SR[0x32] = S.a; step(0x5974);
	cyc(3); push(S.a); step(0x5975);                   // PHA
	cyc(5); SR[0x32] = op_lsr(SR[0x32]); step(0x5977);
	cyc(3); S.a = SR[0x29]; nz(S.a); step(0x5979);
	cyc(2); S.a &= 0x07; nz(S.a); step(0x597B);
	cyc(2); S.p |= FC; step(0x597C);                   // SEC
	cyc(3); op_sbc(SR[0x32]); step(0x597E);
	if (!branch(0x597E, !(S.p & FN), 0x5982))
	{
		cyc(2); S.a = 0x00; nz(S.a); step(0x5982);
	}
	cyc(2); S.y = S.a; nz(S.y); step(0x5983);          // TAY
	cyc(4); S.a = pull(); nz(S.a); step(0x5984);       // PLA
	cyc(3); SR[0x32] = S.a; step(0x5986);
	cyc(5); SR[0x32] = op_lsr(SR[0x32]); step(0x5988);
	cyc(5); SR[0x32] = op_lsr(SR[0x32]); step(0x598A);
	cyc(3); S.a = SR[0x29]; nz(S.a); step(0x598C);
	cyc(2); S.a &= 0x18; nz(S.a); step(0x598E);
	for (uint16_t pc = 0x598F; pc <= 0x5991; pc++)
	{
		cyc(2); S.a = op_lsr(S.a); step(pc);
	}
	cyc(2); S.p |= FC; step(0x5992);                   // SEC
	cyc(3); op_sbc(SR[0x32]); step(0x5994);
	if (!branch(0x5994, !(S.p & FN), 0x5998))
	{
		cyc(2); S.a = 0x00; nz(S.a); step(0x5998);
	}
	for (uint16_t pc = 0x5999; pc <= 0x599B; pc++)
	{
		cyc(2); S.a = op_asl(S.a); step(pc);
	}
	cyc(3); SR[0x32] = S.a; step(0x599D);
	cyc(2); S.a = S.y; nz(S.a); step(0x599E);          // TYA
	cyc(3); S.a |= SR[0x32]; nz(S.a); step(0x59A0);
	cyc(3); S.a |= SR[0x28]; nz(S.a); step(0x59A2);
	cyc(4); wr(0x187A, S.a); step(0x59A5);
	plp(0x59A5);
	rts();
}

// 59A7: phrase A into the queue (not when it is full)
void queue_phrase()
{
	php_sei(0x59A7);
	cyc(4); S.y = SR[0x70B]; nz(S.y); step(0x59AC);
	cyc(2); S.y++; nz(S.y); step(0x59AD);
	cyc(2); op_cmp(S.y, 0x08); step(0x59AF);
	if (!branch(0x59AF, !(S.p & FC), 0x59B3))
	{
		cyc(2); S.y = 0x00; nz(S.y); step(0x59B3);
	}
	cyc(4); op_cmp(S.y, SR[0x70A]); step(0x59B6);
	if (!branch(0x59B6, S.p & FZ, 0x59BE))
	{
		cyc(4); SR[0x70B] = S.y; step(0x59BB);
		cyc(5); wr((uint16_t)(0x070C + S.y), S.a); step(0x59BE);
	}
	plp(0x59BE);
	rts();
}

// 58FC (type 11): say phrase A now, or after the one that plays
void cmd_speak()
{
	cyc(3); S.y = SR[0x2F]; nz(S.y); step(0x58FE);
	if (!branch(0x58FE, S.p & FZ, 0x5903))
	{
		cyc(3); step(0x59A7);                          // JMP $59A7
		queue_phrase();
		return;
	}
	speak();
}

// 5883 (from the IRQ): the speech.  When the chip is not ready (0x1840 bit 2): every other IRQ
// tick counted in 0x30, which at its end sends the reply 0x55.  Otherwise the next byte of the
// phrase to the chip (0x1870, 0x1873); after the last one the state counts down 0x11 IRQs
// (0x2A = 0x19), then 0x60 and 0xFF, and the next phrase of the queue starts.
void speech()
{
	cyc(4); S.a = rd(0x1840); nz(S.a); step(0x5886);
	cyc(2); S.a &= 0x04; nz(S.a); step(0x5888);
	if (!branch(0x5888, S.p & FZ, 0x58A1))
	{
		cyc(4); S.a = SR[0x0000]; nz(S.a); step(0x588D);   // LDA $0000: the tick
		cyc(2); S.a = op_lsr(S.a); step(0x588E);
		cyc(3); op_bit(SR[0x30]); step(0x5890);
		if (!branch(0x5890, S.p & FN, 0x5899))
		{
			cyc(3); S.a ^= SR[0x30]; nz(S.a); step(0x5894);
			if (branch(0x5894, !(S.p & FZ), 0x58FB))
			{
				rts();
				return;
			}
			jmp(0x4443);                               // reply_55
			return;
		}
		cyc(2); op_adc(0x10); step(0x589B);
		cyc(2); S.a &= 0x7F; nz(S.a); step(0x589D);
		cyc(3); SR[0x30] = S.a; step(0x589F);
		branch(0x589F, !(S.p & FN), 0x58FB);           // BPL (always)
		rts();
		return;
	}
	cyc(2); S.a = 0xFF; nz(S.a); step(0x58A3);
	cyc(3); SR[0x30] = S.a; step(0x58A5);
	cyc(4); wr(0x1872, S.a); step(0x58A8);
	cyc(2); S.a = 0xFF; nz(S.a); step(0x58AA);
	cyc(3); S.y = SR[0x2F]; nz(S.y); step(0x58AC);
	if (!branch(0x58AC, !(S.p & FZ), 0x58C8))
	{
		// idle: the next phrase of the queue
		cyc(4); S.y = SR[0x70A]; nz(S.y); step(0x58B1);
		cyc(4); op_cmp(S.y, SR[0x70B]); step(0x58B4);
		if (!branch(0x58B4, !(S.p & FZ), 0x58B8))
		{
			branch(0x58B6, S.p & FZ, 0x58F5);          // BEQ (always)
		}
		else
		{
			cyc(2); S.y++; nz(S.y); step(0x58B9);
			cyc(2); op_cmp(S.y, 0x08); step(0x58BB);
			if (!branch(0x58BB, !(S.p & FC), 0x58BF))
			{
				cyc(2); S.y = 0x00; nz(S.y); step(0x58BF);
			}
			cyc(4); SR[0x70A] = S.y; step(0x58C2);
			cyc(4); S.a = rd(idx(0x070C, S.y)); nz(S.a); step(0x58C5);
			cyc(3); step(0x5903);                      // JMP $5903
			speak();
			return;
		}
	}
	else
	{
		cyc(2); S.y++; nz(S.y); step(0x58C9);          // INY
		if (!branch(0x58C9, !(S.p & FZ), 0x58E5))
		{
			// playing: the next byte
			cyc(5); S.a = rd(idx(zp16(0x2B), S.y)); nz(S.a); step(0x58CD);   // LDA ($2B),Y
			cyc(5); SR[0x2B]++; nz(SR[0x2B]); step(0x58CF);
			if (!branch(0x58CF, !(S.p & FZ), 0x58D3))
			{
				cyc(5); SR[0x2C]++; nz(SR[0x2C]); step(0x58D3);
			}
			cyc(5); SR[0x2D]--; nz(SR[0x2D]); step(0x58D5);
			if (!branch(0x58D5, !(S.p & FZ), 0x58F5))
			{
				cyc(5); SR[0x2E]--; nz(SR[0x2E]); step(0x58D9);
				if (!branch(0x58D9, !(S.p & FN), 0x58F5))
				{
					// the last byte: the ending
					cyc(2); S.y = 0x19; nz(S.y); step(0x58DD);
					cyc(3); SR[0x2A] = S.y; step(0x58DF);
					cyc(2); S.y = 0x11; nz(S.y); step(0x58E1);
					cyc(3); SR[0x2F] = S.y; step(0x58E3);
					branch(0x58E3, !(S.p & FZ), 0x58F5);   // BNE (always)
				}
			}
		}
		else
		{
			cyc(2); op_cmp(S.y, 0x81); step(0x58E7);
			if (!branch(0x58E7, !(S.p & FZ), 0x58F1))
			{
				cyc(2); S.a = 0xFF; nz(S.a); step(0x58EB);
				cyc(3); SR[0x2F] = S.a; step(0x58ED);
				cyc(2); S.a = 0x60; nz(S.a); step(0x58EF);
				branch(0x58EF, !(S.p & FZ), 0x58F5);   // BNE (always)
			}
			else
			{
				cyc(5); SR[0x2F]--; nz(SR[0x2F]); step(0x58F3);
				cyc(2); S.a = 0x00; nz(S.a); step(0x58F5);
			}
		}
	}
	cyc(4); wr(0x1870, S.a); step(0x58F8);             // to the chip
	cyc(4); wr(0x1873, S.a); step(0x58FB);
	rts();
}

} // namespace

SOUND_TABLE(speech,
	routine(0x5831, "speech_init", speech_init),
	routine(0x5883, "speech", speech),
	routine(0x58FC, "cmd_speak", cmd_speak),
)

} // namespace sound
