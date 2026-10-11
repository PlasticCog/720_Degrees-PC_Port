// Rewritten sound routines: the commands of the note data (see routines_music.cpp).  A byte of
// 0x80 or more where a note is expected is a command: 502E runs its handler from the table at
// 0x5080 (by an RTS) with A = the byte after it, X the slot and carry set; the handler reads any
// further bytes with 504C and returns carry set to go on with the next entry, clear to stop
// there.  The handlers are reached only by that RTS, so they are INNER entries.
//
// The opcodes 81, 86-8C, 90, 91, 96, 9A-9C, A8, AC, AD, AF, B1, B3, B4, B9 and BA (and 505E /
// 5074, reached from command type 12, which no command of this program has) are not used by
// any sound the test runs play: they follow the original as closely as the others, but no run
// has compared them.  Opcodes 92-95, 98 and A5 point at 471E inside the voice list player.
#include "ops.h"

namespace sound {
namespace {

using namespace s65rt;
using namespace op;

// ---- the command and its bytes

// 502E: command A (0xBB and up end the sound: 0x0228 = 0xFF, carry clear)
void note_command()
{
	cmp(imm(0xBB), 0x5030);
	if (!bcc(0x5030, 0x5039))
	{
		lda(imm(0xFF), 0x5034);
		sta(wabx(0x0228), 0x5037);
		clc(0x5038);
		rts();
		return;
	}
	iny(0x503A);
	stx(wzp(0x11), 0x503C);
	asl_a(0x503D);
	tax(0x503E);
	lda(abx(0x5081), 0x5041);
	pha(0x5042);
	lda(abx(0x5080), 0x5045);
	pha(0x5046);
	ldx(zp(0x11), 0x5048);
	lda(iy(0x06), 0x504A);
	sec(0x504B);
	rts_jump();
}

// 504C: the next byte of slot X's note data (the pointer 0x0240 / 0x0258 and Y one on)
void note_byte()
{
	lda(abx(0x0240), 0x504F);
	clc(0x5050);
	adc(imm(0x01), 0x5052);
	sta(wabx(0x0240), 0x5055);
	if (!bcc(0x5055, 0x505A)) inc(mabx(0x0258), 0x505A);
	iny(0x505B);
	lda(iy(0x06), 0x505D);
	rts();
}

// 505E (jumped to by 4461): with the interrupts off, the first slot of the list after slot X
// that plays sound 0x0708 gets command Y / 2 with A = 0x0709 (5074)
void kind_slots()
{
	php(0x505F);
	sei(0x5060);
	for (;;)
	{
		lda(abx(0x06C0), 0x5063);
		if (beq(0x5063, 0x5072)) break;
		tax(0x5066);
		dex(0x5067);
		lda(abx(0x0228), 0x506A);
		cmp(ab(0x0708), 0x506D);
		if (bne(0x506D, 0x5060)) continue;
		jsr(0x506F, 0x5074);
		break;
	}
	plp(0x5073);
	rts();
}

// 5074: the handler of command Y / 2 (by an RTS) with A = 0x0709
void kind_command()
{
	lda(aby(0x5081), 0x5077);
	pha(0x5078);
	lda(aby(0x5080), 0x507B);
	pha(0x507C);
	lda(ab(0x0709), 0x507F);
	rts_jump();
}

// the endings shared by the handlers
void clc_rts(uint16_t pc)                              // CLC, RTS at pc: stop
{
	clc((uint16_t)(pc + 1));
	rts();
}
void sec_rts(uint16_t pc)                              // SEC, RTS at pc: go on
{
	sec((uint16_t)(pc + 1));
	rts();
}

// ---- timing, volume, transposition

// 80: the tempo 0x0510 = A / 4
void op80_tempo()
{
	lsr_a(0x5179);
	lsr_a(0x517A);
	sta(wabx(0x0510), 0x517D);
	sec_rts(0x517D);
}

// 81: the tempo + A
void op81_tempo_add()
{
	clc(0x5170);
	adc(abx(0x0510), 0x5173);
	sta(wabx(0x0510), 0x5176);
	sec_rts(0x5176);
}

// 51A8: the volume 0x03A8 = A
void set_volume()
{
	sta(wabx(0x03A8), 0x51AB);
	sec_rts(0x51AB);
}

// 570D (A1, and 5186 on the YM2151): the levels 0x03C0 of the voice's operators that are not
// carriers (0x5798 by the connection) less A, within 0..0x7F
void opA1_levels_add()
{
	ldy(ab(0x06F5), 0x5710);
	cpy(imm(0x02), 0x5712);
	if (!beq(0x5712, 0x5715))
	{
		rts();
		return;
	}
	stx(wzp(0x11), 0x5717);
	eor(imm(0xFF), 0x5719);
	clc(0x571A);
	adc(imm(0x01), 0x571C);
	sta(wzp(0x0E), 0x571E);
	lda(abx(0x0438), 0x5721);
	and_(imm(0x07), 0x5723);
	tay(0x5724);
	lda(aby(0x5798), 0x5727);
	sta(wzp(0x0F), 0x5729);
	ldy(imm(0x03), 0x572B);
	do
	{
		asl(mzp(0x0F), 0x572D);
		if (!bcs(0x572D, 0x5741))
		{
			lda(abx(0x03C0), 0x5732);
			adc(zp(0x0E), 0x5734);
			if (!bpl(0x5734, 0x573E))
			{
				if (bvs(0x5736, 0x573C))
					lda(imm(0x7F), 0x573E);
				else
				{
					lda(imm(0x00), 0x573A);
					beq(0x573A, 0x573E);               // BEQ (always)
				}
			}
			sta(wabx(0x03C0), 0x5741);
		}
		txa(0x5742);
		clc(0x5743);
		adc(imm(0x18), 0x5745);
		tax(0x5746);
		dey(0x5747);
	} while (bpl(0x5747, 0x572B));
	ldx(zp(0x11), 0x574B);
	sec_rts(0x574B);
}

// 5186 (from the slide, and 83): the volume + A; on the YM2151 the operators' levels (570D)
void volume_add()
{
	ldy(ab(0x06F5), 0x5189);
	cpy(imm(0x02), 0x518B);
	if (!bne(0x518B, 0x5190))
	{
		jmp_to(0x570D);
		opA1_levels_add();
		return;
	}
	clc(0x5191);
	adc(abx(0x03A8), 0x5194);
	jmp_to(0x51A8);
	set_volume();
}

// the operators' levels 0x03C0 of slots X, X + 24, .. from the voice data at (0x0E), bytes 5,
// 11, 17 and 23 (LDA #3 at pc)
void voice_levels(uint16_t pc)
{
	lda(imm(0x03), (uint16_t)(pc + 2));
	sta(wzp(0x0C), (uint16_t)(pc + 4));
	ldy(imm(0x05), (uint16_t)(pc + 6));
	const uint16_t loop = (uint16_t)(pc + 6);
	do
	{
		lda(iy(0x0E), (uint16_t)(loop + 2));
		sta(wabx(0x03C0), (uint16_t)(loop + 5));
		tya((uint16_t)(loop + 6));
		clc((uint16_t)(loop + 7));
		adc(imm(0x06), (uint16_t)(loop + 9));
		tay((uint16_t)(loop + 0x0A));
		txa((uint16_t)(loop + 0x0B));
		clc((uint16_t)(loop + 0x0C));
		adc(imm(0x18), (uint16_t)(loop + 0x0E));
		tax((uint16_t)(loop + 0x0F));
		dec(mzp(0x0C), (uint16_t)(loop + 0x11));
	} while (bpl((uint16_t)(loop + 0x11), loop));
}

// 574D (82 on the YM2151): unless the sound is ending, the operators' levels from the voice
// again, then less 0x5788[A & 15] (570D)
void ym_levels()
{
	ldy(abx(0x0228), 0x5750);
	cpy(imm(0xFE), 0x5752);
	if (!bne(0x5752, 0x5756))
	{
		clc_rts(0x5754);
		return;
	}
	pha(0x5757);
	txa(0x5758);
	pha(0x5759);
	lda(abx(0x0450), 0x575C);
	sta(wzp(0x0E), 0x575E);
	lda(abx(0x0468), 0x5761);
	sta(wzp(0x0F), 0x5763);
	voice_levels(0x5763);
	pla(0x577D);
	tax(0x577E);
	pla(0x577F);
	and_(imm(0x0F), 0x5781);
	tay(0x5782);
	lda(aby(0x5788), 0x5785);
	jmp_to(0x570D);
	opA1_levels_add();
}

// 82: unless the sound is ending (0x0228 = 0xFE: carry clear), the volume = A (574D on the
// YM2151)
void op82_volume()
{
	ldy(abx(0x0228), 0x519A);
	cpy(imm(0xFE), 0x519C);
	if (beq(0x519C, 0x51AD))
	{
		clc_rts(0x51AD);
		return;
	}
	ldy(ab(0x06F5), 0x51A1);
	cpy(imm(0x02), 0x51A3);
	if (bne(0x51A3, 0x51A8))
	{
		set_volume();
		return;
	}
	jmp_to(0x574D);
	ym_levels();
}

// 83: unless the sound is ending, the volume + A (5186)
void op83_volume_add()
{
	ldy(abx(0x0228), 0x5182);
	cpy(imm(0xFE), 0x5184);
	if (beq(0x5184, 0x51AD))
	{
		clc_rts(0x51AD);
		return;
	}
	volume_add();
}

// 84: the transposition 0x0528 = A
void op84_transpose()
{
	sta(wabx(0x0528), 0x51B6);
	sec_rts(0x51B6);
}

// 85: the transposition + A
void op85_transpose_add()
{
	clc(0x51B0);
	adc(abx(0x0528), 0x51B3);
	op84_transpose();
}

// ---- envelopes and other settings

// 86: the pitch envelope's list 0x03F0 / 0x0408 = A and the next byte
void op86_pitch_envelope()
{
	sta(wabx(0x03F0), 0x5167);
	jsr(0x5167, 0x504C);
	sta(wabx(0x0408), 0x516D);
	sec_rts(0x516D);
}

// 87: the volume envelope's list 0x03C0 / 0x03D8 = A and the next byte
void op87_volume_envelope()
{
	sta(wabx(0x03C0), 0x515C);
	jsr(0x515C, 0x504C);
	sta(wabx(0x03D8), 0x5162);
	sec_rts(0x5162);
}

// 88: wait for the slots of group 0x0558: while another slot of the group (0x0540 not yet
// matching) still plays, the entry is read again next time (the pointer back by two, carry
// clear); when all have come, their 0x0540 = 0xFF
void op88_sync()
{
	lda(imm(0x00), 0x50F8);
	sta(wabx(0x02A0), 0x50FB);
	sta(wabx(0x02B8), 0x50FE);
	ldy(abx(0x0540), 0x5101);
	if (!beq(0x5101, 0x510B))
	{
		iny(0x5104);
		if (bne(0x5104, 0x513E)) goto l513E;
	l5106:
		sta(wabx(0x0540), 0x5109);
		sec_rts(0x5109);
		return;
	}
	lda(abx(0x0558), 0x510E);
	sta(wabx(0x0540), 0x5111);
	ldy(imm(0x17), 0x5113);
	do
	{
		cmp(aby(0x0558), 0x5116);
		if (!bne(0x5116, 0x5125))
		{
			cmp(aby(0x0540), 0x511B);
			if (!beq(0x511B, 0x5125))
			{
				lda(aby(0x0348), 0x5120);
				if (bne(0x5120, 0x513E)) goto l513E;
				lda(abx(0x0558), 0x5125);
			}
		}
		dey(0x5126);
	} while (bpl(0x5126, 0x5113));
	ldy(imm(0x17), 0x512A);
	do
	{
		cmp(aby(0x0558), 0x512D);
		if (!bne(0x512D, 0x5137))
		{
			lda(imm(0xFF), 0x5131);
			sta(waby(0x0540), 0x5134);
			lda(abx(0x0558), 0x5137);
		}
		dey(0x5138);
	} while (bpl(0x5138, 0x512A));
	lda(imm(0x00), 0x513C);
	beq(0x513C, 0x5106);                               // BEQ (always)
	goto l5106;
l513E:
	lda(abx(0x0240), 0x5141);
	cmp(imm(0x02), 0x5143);
	if (!bcs(0x5143, 0x5148)) dec(mabx(0x0258), 0x5148);
	dec(mabx(0x0240), 0x514B);
	dec(mabx(0x0240), 0x514E);
	clc_rts(0x514E);
}

// 89: the group 0x0558 = A, not come (0x0540 = 0)
void op89_sync_group()
{
	sta(wabx(0x0558), 0x5153);
	lda(imm(0x00), 0x5155);
	sta(wabx(0x0540), 0x5158);
	rts();
}

// 8A: the bits 0x0570 put into the volume = A
void op8A_volume_bits()
{
	sta(wabx(0x0570), 0x51BB);
	rts();
}

// 8B: bits A set in 0x0390 (on chips of type other than 0 only bits 0 and 3 of A, the rest
// from 0x06F6)
void op8B_bits_set()
{
	ldy(ab(0x06F5), 0x51BF);
	if (!beq(0x51BF, 0x51C9))
	{
		eor(ab(0x06F6), 0x51C4);
		and_(imm(0x09), 0x51C6);
		eor(ab(0x06F6), 0x51C9);
	}
	ora(abx(0x0390), 0x51CC);
	sta(wabx(0x0390), 0x51CF);
	rts();
}

// 9B: bits A cleared in 0x0378 (chip type 0), else bits 0 and 3 of A cleared in 0x0390
void op9B_bits_clear()
{
	eor(imm(0xFF), 0x51D2);
	ldy(ab(0x06F5), 0x51D5);
	if (!bne(0x51D5, 0x51DE))
	{
		and_(abx(0x0378), 0x51DA);
		sta(wabx(0x0378), 0x51DD);
		rts();
		return;
	}
	ora(imm(0xF6), 0x51E0);
	and_(abx(0x0390), 0x51E3);
	sta(wabx(0x0390), 0x51E6);
	rts();
}

// 8C: the slide's decay 0x0588 = A
void op8C_slide_decay()
{
	sta(wabx(0x0588), 0x51EA);
	rts();
}

// A4: unless the sound is ending, a slide: 0x0660 = the next byte, 0x0648 = A * 32, the time
// 0x0630 = A / 8 (0x0618 and the sum 0x0678 cleared)
void opA4_slide()
{
	sta(wzp(0x11), 0x5278);
	jsr(0x5278, 0x504C);
	ldy(abx(0x0228), 0x527E);
	cpy(imm(0xFE), 0x5280);
	if (beq(0x5280, 0x52A1))
	{
		clc_rts(0x52A1);
		return;
	}
	sta(wabx(0x0660), 0x5285);
	lda(zp(0x11), 0x5287);
	lsr_a(0x5288);
	lsr_a(0x5289);
	lsr_a(0x528A);
	sta(wabx(0x0630), 0x528D);
	lda(imm(0x00), 0x528F);
	sta(wabx(0x0618), 0x5292);
	sta(wabx(0x0678), 0x5295);
	lda(zp(0x11), 0x5297);
	for (uint16_t pc = 0x5298; pc <= 0x529C; pc++) asl_a(pc);
	sta(wabx(0x0648), 0x529F);
	sec_rts(0x529F);
}

// 97: the sound ends with a slide down (0x0228 = 0xFE)
void op97_fade()
{
	lda(imm(0x00), 0x5500);
	sta(wabx(0x0618), 0x5503);
	sta(wabx(0x0678), 0x5506);
	sta(wabx(0x0648), 0x5509);
	lda(imm(0xD0), 0x550B);
	sta(wabx(0x0660), 0x550E);
	lda(imm(0x02), 0x5510);
	sta(wabx(0x0630), 0x5513);
	lda(imm(0xFE), 0x5515);
	sta(wabx(0x0228), 0x5518);
	sec_rts(0x5518);
}

// A8: the envelope loop count 0x04F8 = A
void opA8_loops()
{
	sta(wabx(0x04F8), 0x570C);
	rts();
}

// ---- the priorities of a channel pair

// 90 / 9C: the slot's entry 0x06E9 (by 0x06EB) cleared, bits 0-1 of its priority 0x0348
// cleared (9C: set to 2), and the slot the first of its pair (0x06EB = 0, 0x06E9 = the
// priority)
void pair_first(uint16_t pc, bool second)
{
	ldy(ab(0x06EB), (uint16_t)(pc + 3));
	lda(imm(0x00), (uint16_t)(pc + 5));
	sta(waby(0x06E9), (uint16_t)(pc + 8));
	lda(abx(0x0348), (uint16_t)(pc + 0x0B));
	and_(imm(0xFC), (uint16_t)(pc + 0x0D));
	if (second)
	{
		ora(imm(0x02), (uint16_t)(pc + 0x0F));
		pc = (uint16_t)(pc + 2);
	}
	sta(wabx(0x0348), (uint16_t)(pc + 0x10));
	ldy(imm(0x00), (uint16_t)(pc + 0x12));
	sty(wab(0x06EB), (uint16_t)(pc + 0x15));
	sta(wab(0x06E9), (uint16_t)(pc + 0x18));
	rts();
}
void op90_pair_first() { pair_first(0x54D1, false); }
void op9C_pair_second() { pair_first(0x54B6, true); }

// 91: the second of the pair (0x06EB = 1), bits A set in 0x0348 and its level
void op91_pair_second()
{
	ldy(imm(0x01), 0x54EC);
	sty(wab(0x06EB), 0x54EF);
	ora(abx(0x0348), 0x54F2);
	sta(wabx(0x0348), 0x54F5);
	sta(waby(0x06E9), 0x54F8);
	rts();
}

// ---- flow of the note data

// 8D: a call: the address A and the next byte, the place to come back to kept in a node of the
// slot's list 0x05D0 (none free: on)
void op8D_call()
{
	sta(wzp(0x11), 0x51ED);
	jsr(0x51ED, 0x504C);
	sta(wzp(0x12), 0x51F2);
	jsr(0x51F2, 0x42CB);                               // node_take
	if (!beq(0x51F5, 0x5217))
	{
		pha(0x51F8);
		lda(abx(0x05D0), 0x51FB);
		sta(wiy(0x15), 0x51FD);
		pla(0x51FE);
		sta(wabx(0x05D0), 0x5201);
		iny(0x5202);
		lda(abx(0x0240), 0x5205);
		sta(wiy(0x15), 0x5207);
		iny(0x5208);
		lda(abx(0x0258), 0x520B);
		sta(wiy(0x15), 0x520D);
		lda(zp(0x11), 0x520F);
		sta(wabx(0x0240), 0x5212);
		lda(zp(0x12), 0x5214);
		sta(wabx(0x0258), 0x5217);
	}
	sec_rts(0x5217);
}

// 8E: a repeat A times from here: the place and the count kept in a node of the slot's list
// 0x05E8 (the count 0x0600)
void op8E_repeat()
{
	pha(0x521A);
	jsr(0x521A, 0x42CB);                               // node_take
	if (beq(0x521D, 0x5241))
	{
		pla(0x5242);
		sec_rts(0x5242);
		return;
	}
	pha(0x5220);
	lda(abx(0x05E8), 0x5223);
	sta(wiy(0x15), 0x5225);
	pla(0x5226);
	sta(wabx(0x05E8), 0x5229);
	iny(0x522A);
	lda(abx(0x0240), 0x522D);
	sta(wiy(0x15), 0x522F);
	iny(0x5230);
	lda(abx(0x0258), 0x5233);
	sta(wiy(0x15), 0x5235);
	iny(0x5236);
	lda(abx(0x0600), 0x5239);
	sta(wiy(0x15), 0x523B);
	pla(0x523C);
	sta(wabx(0x0600), 0x523F);
	sec_rts(0x523F);
}

// 8F: the end of a repeat: back to its place while the count lasts, else its node freed and
// the outer repeat's count back
void op8F_repeat_end()
{
	lda(abx(0x05E8), 0x5247);
	if (!beq(0x5247, 0x525D))
	{
		jsr(0x5249, 0x42DC);                           // node_addr
		dec(mabx(0x0600), 0x524F);
		if (beq(0x524F, 0x525F))
		{
			pha(0x5260);
			lda(iy(0x15), 0x5262);
			sta(wabx(0x05E8), 0x5265);
			lda(zp(0x14), 0x5267);
			sta(wiy(0x15), 0x5269);
			pla(0x526A);
			sta(wzp(0x14), 0x526C);
			iny(0x526D);
			iny(0x526E);
			iny(0x526F);
			lda(iy(0x15), 0x5271);
			sta(wabx(0x0600), 0x5274);
			sec_rts(0x5274);
			return;
		}
		iny(0x5252);
		lda(iy(0x15), 0x5254);
		sta(wabx(0x0240), 0x5257);
		iny(0x5258);
		lda(iy(0x15), 0x525A);
		sta(wabx(0x0258), 0x525D);
	}
	sec_rts(0x525D);
}

// 99: a jump to the address A and the next byte
void op99_jump()
{
	sta(wzp(0x11), 0x551C);
	jsr(0x551C, 0x504C);
	sta(wabx(0x0258), 0x5522);
	lda(zp(0x11), 0x5524);
	sta(wabx(0x0240), 0x5527);
	sec_rts(0x5527);
}

// 96: A to the main CPU (4445)
void op96_reply()
{
	jsr(0x54F9, 0x4445);                               // queue_reply
	sec_rts(0x54FC);
}

// 9A: phrase A spoken (58FC)
void op9A_speak()
{
	stx(wzp(0x11), 0x552B);
	tay(0x552C);
	jsr(0x552C, 0x58FC);                               // cmd_speak
	ldx(zp(0x11), 0x5531);
	sec_rts(0x5531);
}

// ---- the YM2151 voice and registers

// 5674: unless another slot follows (0x17), YM2151 register X = (0x0E),Y (Y on), kept at
// 0x0715 + X
void ym_data_reg()
{
	lda(zp(0x17), 0x5676);
	if (!bne(0x5676, 0x5687))
	{
		jsr(0x5678, 0x4FF5);                           // wait_ym
		stx(wab(0x1850), 0x567E);
		lda(iy(0x0E), 0x5680);
		iny(0x5681);
		sta(wab(0x1851), 0x5684);
		sta(wabx(0x0715), 0x5687);
	}
	rts();
}

// 558D (from the voice list, and 9D): unless another slot follows, on the YM2151 slot X's voice
// (at 0x0450 / 0x0468) to channel 0x0714: the key off, the registers 0x20, 0x28, 0x30, 0x38 from
// its bytes 0-3 and the operators' 0x40-0xE0 from the bytes after; 0x0378 / 0x0390 = the voice
// + 0x1C
void ym_voice()
{
	ldy(zp(0x17), 0x558F);
	if (!beq(0x558F, 0x5592))
	{
		rts();
		return;
	}
	ldy(ab(0x06F5), 0x5595);
	cpy(imm(0x02), 0x5597);
	if (!beq(0x5597, 0x559A))
	{
		rts();
		return;
	}
	stx(wzp(0x11), 0x559C);
	lda(abx(0x0450), 0x559F);
	sta(wzp(0x0E), 0x55A1);
	clc(0x55A2);
	adc(imm(0x1C), 0x55A4);
	sta(wabx(0x0378), 0x55A7);
	lda(abx(0x0468), 0x55AA);
	sta(wzp(0x0F), 0x55AC);
	adc(imm(0x00), 0x55AE);
	sta(wabx(0x0390), 0x55B1);
	ldx(imm(0x08), 0x55B3);
	jsr(0x55B3, 0x4FF5);                               // wait_ym
	stx(wab(0x1850), 0x55B9);
	lda(ab(0x0714), 0x55BC);
	sta(wab(0x1851), 0x55BF);
	sta(wabx(0x0715), 0x55C2);
	ldy(imm(0x00), 0x55C4);
	lda(imm(0x03), 0x55C6);
	sta(wzp(0x0C), 0x55C8);
	clc(0x55C9);
	lda(ab(0x0714), 0x55CC);
	adc(imm(0x20), 0x55CE);
	clc(0x55CF);
	do
	{
		tax(0x55D0);
		jsr(0x55D0, 0x4FF5);                           // wait_ym
		stx(wab(0x1850), 0x55D6);
		lda(iy(0x0E), 0x55D8);
		sta(wabx(0x0715), 0x55DB);
		sta(wab(0x1851), 0x55DE);
		txa(0x55DF);
		clc(0x55E0);
		adc(imm(0x08), 0x55E2);
		iny(0x55E3);
		dec(mzp(0x0C), 0x55E5);
	} while (bpl(0x55E5, 0x55CF));
	lda(imm(0x03), 0x55E9);
	sta(wzp(0x0C), 0x55EB);
	clc(0x55EC);
	lda(imm(0x40), 0x55EE);
	adc(ab(0x0714), 0x55F1);
	for (;;)
	{
		tax(0x55F2);
		jsr(0x55F2, 0x4FF5);                           // wait_ym
		stx(wab(0x1850), 0x55F8);
		lda(iy(0x0E), 0x55FA);
		sta(wabx(0x0715), 0x55FD);
		sta(wab(0x1851), 0x5600);
		iny(0x5601);
		txa(0x5602);
		clc(0x5603);
		adc(imm(0x20), 0x5605);
		if (bcc(0x5605, 0x55F1)) continue;
		adc(imm(0x47), 0x5609);
		dec(mzp(0x0C), 0x560B);
		if (!bpl(0x560B, 0x55F1)) break;
	}
	ldx(zp(0x11), 0x560F);
	sec_rts(0x560F);
}

// 9D: unless the sound is ending (then the byte skipped, carry clear), the voice at A and the
// next byte: its connection 0x0438, frequency 0x03A8, register 0x38, the operators' levels,
// then to the chip (558D)
void op9D_voice()
{
	sta(wzp(0x11), 0x5535);
	lda(abx(0x0228), 0x5538);
	cmp(imm(0xFE), 0x553A);
	if (!bne(0x553A, 0x5544))
	{
		inc(mabx(0x0228), 0x553F);
		jsr(0x553F, 0x504C);
		clc_rts(0x5542);
		return;
	}
	lda(zp(0x11), 0x5546);
	stx(wzp(0x11), 0x5548);
	sta(wzp(0x0E), 0x554A);
	sta(wabx(0x0450), 0x554D);
	jsr(0x554D, 0x504C);
	sta(wzp(0x0F), 0x5552);
	sta(wabx(0x0468), 0x5555);
	ldy(imm(0x00), 0x5557);
	lda(iy(0x0E), 0x5559);
	sta(wabx(0x0438), 0x555C);
	iny(0x555D);
	iny(0x555E);
	lda(iy(0x0E), 0x5560);
	sta(wabx(0x03A8), 0x5563);
	iny(0x5564);
	lda(iy(0x0E), 0x5566);
	pha(0x5567);
	clc(0x5568);
	lda(imm(0x38), 0x556A);
	adc(ab(0x0714), 0x556D);
	tay(0x556E);
	pla(0x556F);
	sta(waby(0x0715), 0x5572);
	voice_levels(0x5572);
	ldx(zp(0x11), 0x558D);
	ym_voice();
}

// the pointer (0x0E) = A + add and the next byte (carry kept across 504C)
void data_pointer(uint16_t pc, uint8_t add)
{
	clc((uint16_t)(pc + 1));
	adc(imm(add), (uint16_t)(pc + 3));
	sta(wzp(0x0E), (uint16_t)(pc + 5));
	php((uint16_t)(pc + 6));
	jsr((uint16_t)(pc + 6), 0x504C);
	plp((uint16_t)(pc + 0x0A));
	adc(imm(0x00), (uint16_t)(pc + 0x0C));
	sta(wzp(0x0F), (uint16_t)(pc + 0x0E));
}

// 9E: on the YM2151 the registers 0x18, 0x19 (twice), 0x1B (the LFO) and, after register 1 =
// 0, 0x0715 + 1 from the data at A + 0x24 and the next byte
void op9E_lfo()
{
	data_pointer(0x5611, 0x24);
	ldy(ab(0x06F5), 0x5622);
	cpy(imm(0x02), 0x5624);
	if (!bne(0x5624, 0x5651))
	{
		ldy(imm(0x00), 0x5628);
		ldx(imm(0x18), 0x562A);
		jsr(0x562A, 0x5674);                           // ym_data_reg
		ldx(imm(0x19), 0x562F);
		jsr(0x562F, 0x5674);
		ldx(imm(0x19), 0x5634);
		jsr(0x5634, 0x5674);
		ldx(imm(0x1B), 0x5639);
		jsr(0x5639, 0x5674);
		sec(0x563D);
		ldx(imm(0x01), 0x563F);
		jsr(0x563F, 0x4FF5);                           // wait_ym
		stx(wab(0x1850), 0x5645);
		lda(imm(0x00), 0x5647);
		sta(wab(0x1851), 0x564A);
		lda(iy(0x0E), 0x564C);
		sta(wabx(0x0715), 0x564F);
		ldx(zp(0x11), 0x5651);
	}
	sec_rts(0x5651);
}

// 9F: on the YM2151 register 0x0F (the noise) from the data at A + 0x29 and the next byte
void op9F_noise()
{
	data_pointer(0x5653, 0x29);
	ldy(ab(0x06F5), 0x5664);
	cpy(imm(0x02), 0x5666);
	if (!bne(0x5666, 0x5672))
	{
		sec(0x5669);
		ldy(imm(0x00), 0x566B);
		ldx(imm(0x0F), 0x566D);
		jsr(0x566D, 0x5674);                           // ym_data_reg
		ldx(zp(0x11), 0x5672);
	}
	sec_rts(0x5672);
}

// A0: unless another slot follows, on the YM2151 the output bits (bits 6-7 of the connection
// 0x0438) = A
void opA0_output()
{
	ldy(zp(0x17), 0x568A);
	if (!bne(0x568A, 0x56A5))
	{
		ldy(ab(0x06F5), 0x568F);
		cpy(imm(0x02), 0x5691);
		if (!bne(0x5691, 0x56A5))
		{
			for (uint16_t pc = 0x5694; pc <= 0x5699; pc++) asl_a(pc);
			sta(wzp(0x0E), 0x569B);
			lda(abx(0x0438), 0x569E);
			and_(imm(0x3F), 0x56A0);
			ora(zp(0x0E), 0x56A2);
			sta(wabx(0x0438), 0x56A5);
		}
	}
	sec_rts(0x56A5);
}

// A2: on the YM2151 the volume 0x0420 = -A
void opA2_ym_volume()
{
	ldy(ab(0x06F5), 0x56C6);
	cpy(imm(0x02), 0x56C8);
	if (!bne(0x56C8, 0x56D2))
	{
		eor(imm(0xFF), 0x56CC);
		clc(0x56CD);
		adc(imm(0x01), 0x56CF);
		sta(wabx(0x0420), 0x56D2);
	}
	sec_rts(0x56D2);
}

// A3: on the YM2151 the volume less A, within -0x80..0x7F
void opA3_ym_volume_add()
{
	ldy(ab(0x06F5), 0x56AA);
	cpy(imm(0x02), 0x56AC);
	if (!bne(0x56AC, 0x56D2))
	{
		eor(imm(0xFF), 0x56B0);
		clc(0x56B1);
		adc(imm(0x01), 0x56B3);
		clc(0x56B4);
		adc(abx(0x0420), 0x56B7);
		if (!bvc(0x56B7, 0x56CF))
		{
			if (bpl(0x56B9, 0x56BF))
			{
				lda(imm(0x80), 0x56C1);
				bne(0x56C1, 0x56CF);                   // BNE (always)
			}
			else
			{
				lda(imm(0x7F), 0x56BD);
				bne(0x56BD, 0x56CF);                   // BNE (always)
			}
		}
		sta(wabx(0x0420), 0x56D2);
	}
	sec_rts(0x56D2);
}

// A6: on the YM2151 the key fraction 0x03A8 = A * 4
void opA6_fraction()
{
	ldy(ab(0x06F5), 0x56FE);
	cpy(imm(0x02), 0x5700);
	if (bne(0x5700, 0x56D2))
	{
		sec_rts(0x56D2);
		return;
	}
	asl_a(0x5703);
	asl_a(0x5704);
	sta(wabx(0x03A8), 0x5707);
	sec_rts(0x5707);
}

// A7: on the YM2151 the signed A * 4 added to the key fraction and transposition (0x03A8 /
// 0x0528)
void opA7_bend()
{
	ldy(ab(0x06F5), 0x56D7);
	cpy(imm(0x02), 0x56D9);
	if (!bne(0x56D9, 0x56D2))
	{
		ldy(imm(0x00), 0x56DD);
		sty(wzp(0x10), 0x56DF);
		asl_a(0x56E0);
		if (!bcc(0x56E0, 0x56E6))
		{
			ldy(imm(0xFF), 0x56E4);
			sty(wzp(0x10), 0x56E6);
		}
		asl_a(0x56E7);
		rol(mzp(0x10), 0x56E9);
		clc(0x56EA);
		adc(abx(0x03A8), 0x56ED);
		sta(wabx(0x03A8), 0x56F0);
		lda(zp(0x10), 0x56F2);
		adc(abx(0x0528), 0x56F5);
		sta(wabx(0x0528), 0x56F8);
		jmp_to(0x56D2);
	}
	sec_rts(0x56D2);
}

// ---- the slot's variable 0x0690 (the last result also in 0x06A8)

// 52A7: the variable = A
void set_variable()
{
	sta(wabx(0x0690), 0x52AA);
	sta(wabx(0x06A8), 0x52AD);
	sec_rts(0x52AD);
}

// A9: + A
void opA9_add()
{
	clc(0x52A4);
	adc(abx(0x0690), 0x52A7);
	set_variable();
}

// AA: - A
void opAA_sub()
{
	sta(wzp(0x11), 0x52B1);
	lda(abx(0x0690), 0x52B4);
	sbc(zp(0x11), 0x52B6);
	jmp_to(0x52A7);
	set_variable();
}

// AB / AC / AD: AND / OR / EOR A
void opAB_and()
{
	and_(abx(0x0690), 0x52BC);
	jmp_to(0x52A7);
	set_variable();
}
void opAC_or()
{
	ora(abx(0x0690), 0x52C2);
	jmp_to(0x52A7);
	set_variable();
}
void opAD_eor()
{
	eor(abx(0x0690), 0x52C8);
	jmp_to(0x52A7);
	set_variable();
}

// B3 / B4 (at pc): shifted right / left by A (1-7; otherwise unchanged)
void shift(uint16_t pc, bool right)
{
	clc((uint16_t)(pc + 1));
	tay((uint16_t)(pc + 2));
	lda(abx(0x0690), (uint16_t)(pc + 5));
	// CPY #n, BEQ to the shift that leaves n of them
	static const uint8_t order[7] = {4, 2, 6, 7, 5, 3, 1};
	const uint16_t last = (uint16_t)(pc + 0x29);       // the last LSR / ASL
	uint16_t at = (uint16_t)(pc + 5);
	for (int i = 0; i < 7; i++)
	{
		const uint8_t n = order[i];
		cpy(imm(n), (uint16_t)(at + 2));
		const uint16_t target = (uint16_t)(last + 1 - n);
		if (beq((uint16_t)(at + 2), target))
		{
			for (uint16_t a = target; a <= last; a++)
				right ? lsr_a((uint16_t)(a + 1)) : asl_a((uint16_t)(a + 1));
			jmp_to(0x52A7);
			set_variable();
			return;
		}
		at = (uint16_t)(at + 4);
	}
	sec_rts((uint16_t)(pc + 0x21));
}
void opB3_shift_right() { shift(0x52CB, true); }
void opB4_shift_left() { shift(0x52F8, false); }

// AE: a jump to the variable-th address of the list from here (A and the next byte the first)
void opAE_jump_table()
{
	sta(wzp(0x11), 0x5327);
	lda(abx(0x0690), 0x532A);
	if (!bne(0x532A, 0x5331))
	{
		lda(zp(0x11), 0x532E);
		jmp_to(0x533F);
	}
	else
	{
		sta(wzp(0x10), 0x5333);
		do
		{
			jsr(0x5333, 0x504C);
			jsr(0x5336, 0x504C);
			sta(wzp(0x11), 0x533B);
			dec(mzp(0x10), 0x533D);
		} while (bne(0x533D, 0x5333));
	}
	jsr(0x533F, 0x504C);
	sta(wabx(0x0258), 0x5345);
	lda(zp(0x11), 0x5347);
	sta(wabx(0x0240), 0x534A);
	sec_rts(0x534A);
}

// AF: as AE, then the variable one on
void opAF_jump_table_next()
{
	sta(wzp(0x11), 0x534E);
	lda(abx(0x0690), 0x5351);
	if (!bne(0x5351, 0x5358))
	{
		lda(zp(0x11), 0x5355);
		jmp_to(0x5366);
	}
	else
	{
		sta(wzp(0x10), 0x535A);
		do
		{
			jsr(0x535A, 0x504C);
			jsr(0x535D, 0x504C);
			sta(wzp(0x11), 0x5362);
			dec(mzp(0x10), 0x5364);
		} while (bne(0x5364, 0x535A));
	}
	jsr(0x5366, 0x504C);
	sta(wabx(0x0258), 0x536C);
	lda(zp(0x11), 0x536E);
	sta(wabx(0x0240), 0x5371);
	inc(mabx(0x0690), 0x5374);
	lda(abx(0x0690), 0x5377);
	jmp_to(0x52A7);
	set_variable();
}

// 5449: the value of source A (Y kept): 0x16 and up the last result 0x06A8; 6-0x15 the bytes
// 0x18-0x27; 5 the input 0x180A; then on the YM2151 4 and up the key fraction 0x03A8, 3 the
// volume 0x0420, 2 the transposition, 1 the tempo; on the others 3 and 4 the last result, 2
// the transposition, 1 the tempo, 0 the volume 0x03A8
void source_value()
{
	sty(wzp(0x11), 0x544B);
	cmp(imm(0x16), 0x544D);
	if (bcs(0x544D, 0x54B0)) goto l54B0;
	cmp(imm(0x06), 0x5451);
	if (bcs(0x5451, 0x549B))
	{
		sbc(imm(0x06), 0x549D);
		tay(0x549E);
		lda(aby(0x0018), 0x54A1);
		pha(0x54A2);
		ldy(zp(0x11), 0x54A4);
		pla(0x54A5);
		rts();
		return;
	}
	cmp(imm(0x05), 0x5455);
	if (beq(0x5455, 0x54A6))
	{
		ldy(imm(0x0A), 0x54A8);
		lda(aby(0x1800), 0x54AB);
		pha(0x54AC);
		ldy(zp(0x11), 0x54AE);
		pla(0x54AF);
		rts();
		return;
	}
	ldy(ab(0x06F5), 0x545A);
	cpy(imm(0x02), 0x545C);
	if (bne(0x545C, 0x547D))
	{
		cmp(imm(0x03), 0x547F);
		if (bcs(0x547F, 0x54B0)) goto l54B0;
		cmp(imm(0x02), 0x5483);
		if (!bcc(0x5483, 0x548B)) goto l5485;
		cmp(imm(0x01), 0x548D);
		if (!bcc(0x548D, 0x5495)) goto l548F;
		ldy(zp(0x11), 0x5497);
		lda(abx(0x03A8), 0x549A);
		rts();
		return;
	}
	cmp(imm(0x04), 0x5460);
	if (!bcc(0x5460, 0x5468))
	{
		ldy(zp(0x11), 0x5464);
		lda(abx(0x03A8), 0x5467);
		rts();
		return;
	}
	cmp(imm(0x03), 0x546A);
	if (!bcc(0x546A, 0x5472))
	{
		ldy(zp(0x11), 0x546E);
		lda(abx(0x0420), 0x5471);
		rts();
		return;
	}
	cmp(imm(0x02), 0x5474);
	if (beq(0x5474, 0x5485)) goto l5485;
	cmp(imm(0x01), 0x5478);
	if (beq(0x5478, 0x548F)) goto l548F;
	ldy(zp(0x11), 0x547C);
	rts();
	return;
l5485:
	ldy(zp(0x11), 0x5487);
	lda(abx(0x0528), 0x548A);
	rts();
	return;
l548F:
	ldy(zp(0x11), 0x5491);
	lda(abx(0x0510), 0x5494);
	rts();
	return;
l54B0:
	ldy(zp(0x11), 0x54B2);
	lda(abx(0x06A8), 0x54B5);
	rts();
}

// B2: the variable = the value of source A
void opB2_load()
{
	jsr(0x5400, 0x5449);                               // source_value
	jmp_to(0x52A7);
	set_variable();
}

// BA: the last result 0x06A8 = the variable - A
void opBA_compare()
{
	sta(wzp(0x11), 0x540B);
	lda(abx(0x0690), 0x540E);
	sbc(zp(0x11), 0x5410);
	sta(wabx(0x06A8), 0x5413);
	sec_rts(0x5413);
}

// B9: the last result = the variable - the value of source A
void opB9_compare_source()
{
	jsr(0x5406, 0x5449);                               // source_value
	opBA_compare();
}

// B5-B8 (B5 at 5415): by the value of source A (equal / not equal / plus / minus), a jump to
// the address in the next two bytes, or past them
void branch_on(uint16_t pc)
{
	jsr(pc, 0x5449);                                   // source_value
	bool jump;
	const uint16_t b = (uint16_t)(pc + 3);
	switch (pc)
	{
	case 0x5415: jump = beq(b, 0x5437) || !bne((uint16_t)(b + 2), 0x542F); break;
	case 0x541C: jump = bne(b, 0x5437) || !beq((uint16_t)(b + 2), 0x542F); break;
	case 0x5423: jump = bpl(b, 0x5437) || !bmi((uint16_t)(b + 2), 0x542F); break;
	default: jump = bmi(b, 0x5437); break;             // B8 falls on to 542F
	}
	if (jump)
	{
		jsr(0x5437, 0x504C);
		sta(wzp(0x11), 0x543C);
		jsr(0x543C, 0x504C);
		sta(wabx(0x0258), 0x5442);
		lda(zp(0x11), 0x5444);
		sta(wabx(0x0240), 0x5447);
		sec_rts(0x5447);
		return;
	}
	jsr(0x542F, 0x504C);
	jsr(0x5432, 0x504C);
	sec_rts(0x5435);
}
void opB5_if_zero() { branch_on(0x5415); }
void opB6_if_nonzero() { branch_on(0x541C); }
void opB7_if_plus() { branch_on(0x5423); }
void opB8_if_minus() { branch_on(0x542A); }

// B0 (at 537A): the variable to destination A: 0x16 and up and 5 nothing, 6-0x15 the bytes
// 0x18-0x27; on the YM2151 4 and up the key fraction (A6), 3 the volume (A2); 2 the
// transposition (84), 1 the tempo (80), 0 the volume (82); on the other chips 3 and 4 nothing.
// B1 (at 53C7): the variable added the same way (A7, A3, 85, 81, 83), nothing from 5 up on the
// YM2151 and from 3 up on the others.
void store_to(bool add)
{
	if (!add)
	{
		cmp(imm(0x16), 0x537C);
		if (bcs(0x537C, 0x53C5))
		{
			sec_rts(0x53C5);
			return;
		}
		cmp(imm(0x06), 0x5380);
		if (!bcc(0x5380, 0x538E))
		{
			sbc(imm(0x06), 0x5384);
			tay(0x5385);
			lda(abx(0x0690), 0x5388);
			sta(waby(0x0018), 0x538B);
			jmp_to(0x53C5);
			sec_rts(0x53C5);
			return;
		}
		cmp(imm(0x05), 0x5390);
		if (beq(0x5390, 0x53C5))
		{
			sec_rts(0x53C5);
			return;
		}
		ldy(ab(0x06F5), 0x5395);
		cpy(imm(0x02), 0x5397);
		if (!bne(0x5397, 0x53AB))
		{
			tay(0x539A);
			lda(abx(0x0690), 0x539D);
			cpy(imm(0x04), 0x539F);
			if (!bcc(0x539F, 0x53A4))
			{
				jmp_to(0x56FB);
				opA6_fraction();
				return;
			}
			cpy(imm(0x03), 0x53A6);
			if (!bcc(0x53A6, 0x53B3))
			{
				jmp_to(0x56C3);
				opA2_ym_volume();
				return;
			}
		}
		else
		{
			cmp(imm(0x03), 0x53AD);
			if (bcs(0x53AD, 0x53C5))
			{
				sec_rts(0x53C5);
				return;
			}
			tay(0x53B0);
			lda(abx(0x0690), 0x53B3);
		}
		cpy(imm(0x02), 0x53B5);
		if (!bcc(0x53B5, 0x53BA))
		{
			jmp_to(0x51B3);
			op84_transpose();
			return;
		}
		cpy(imm(0x01), 0x53BC);
		if (!bcc(0x53BC, 0x53C1))
		{
			jmp_to(0x5178);
			op80_tempo();
			return;
		}
		sec(0x53C2);
		jmp_to(0x5197);
		op82_volume();
		return;
	}
	ldy(ab(0x06F5), 0x53CA);
	cpy(imm(0x02), 0x53CC);
	if (!bne(0x53CC, 0x53E4))
	{
		tay(0x53CF);
		cpy(imm(0x05), 0x53D1);
		if (bcs(0x53D1, 0x53FE))
		{
			sec_rts(0x53FE);
			return;
		}
		lda(abx(0x0690), 0x53D6);
		cpy(imm(0x04), 0x53D8);
		if (!bcc(0x53D8, 0x53DD))
		{
			jmp_to(0x56D4);
			opA7_bend();
			return;
		}
		cpy(imm(0x03), 0x53DF);
		if (!bcc(0x53DF, 0x53EC))
		{
			jmp_to(0x56A7);
			opA3_ym_volume_add();
			return;
		}
	}
	else
	{
		cmp(imm(0x03), 0x53E6);
		if (bcs(0x53E6, 0x53FE))
		{
			sec_rts(0x53FE);
			return;
		}
		tay(0x53E9);
		lda(abx(0x0690), 0x53EC);
	}
	cpy(imm(0x02), 0x53EE);
	if (!bcc(0x53EE, 0x53F3))
	{
		jmp_to(0x51AF);
		op85_transpose_add();
		return;
	}
	cpy(imm(0x01), 0x53F5);
	if (!bcc(0x53F5, 0x53FA))
	{
		jmp_to(0x516F);
		op81_tempo_add();
		return;
	}
	sec(0x53FB);
	jmp_to(0x517F);
	op83_volume_add();
}
void opB0_store() { store_to(false); }
void opB1_store_add() { store_to(true); }

} // namespace

SOUND_TABLE(notes,
	routine(0x502E, "note_command", note_command),
	routine(0x504C, "note_byte", note_byte),
	routine(0x505E, "kind_slots", kind_slots, INNER),
	routine(0x5074, "kind_command", kind_command),
	routine(0x50F6, "op88_sync", op88_sync, INNER),
	routine(0x5150, "op89_sync_group", op89_sync_group, INNER),
	routine(0x5159, "op87_volume_envelope", op87_volume_envelope, INNER),
	routine(0x5164, "op86_pitch_envelope", op86_pitch_envelope, INNER),
	routine(0x516F, "op81_tempo_add", op81_tempo_add, INNER),
	routine(0x5178, "op80_tempo", op80_tempo, INNER),
	routine(0x517F, "op83_volume_add", op83_volume_add, INNER),
	routine(0x5186, "volume_add", volume_add),
	routine(0x5197, "op82_volume", op82_volume, INNER),
	routine(0x51AF, "op85_transpose_add", op85_transpose_add, INNER),
	routine(0x51B3, "op84_transpose", op84_transpose, INNER),
	routine(0x51B8, "op8A_volume_bits", op8A_volume_bits, INNER),
	routine(0x51BC, "op8B_bits_set", op8B_bits_set, INNER),
	routine(0x51D0, "op9B_bits_clear", op9B_bits_clear, INNER),
	routine(0x51E7, "op8C_slide_decay", op8C_slide_decay, INNER),
	routine(0x51EB, "op8D_call", op8D_call, INNER),
	routine(0x5219, "op8E_repeat", op8E_repeat, INNER),
	routine(0x5244, "op8F_repeat_end", op8F_repeat_end, INNER),
	routine(0x5276, "opA4_slide", opA4_slide, INNER),
	routine(0x52A3, "opA9_add", opA9_add, INNER),
	routine(0x52AF, "opAA_sub", opAA_sub, INNER),
	routine(0x52B9, "opAB_and", opAB_and, INNER),
	routine(0x52BF, "opAC_or", opAC_or, INNER),
	routine(0x52C5, "opAD_eor", opAD_eor, INNER),
	routine(0x52CB, "opB3_shift_right", opB3_shift_right, INNER),
	routine(0x52F8, "opB4_shift_left", opB4_shift_left, INNER),
	routine(0x5325, "opAE_jump_table", opAE_jump_table, INNER),
	routine(0x534C, "opAF_jump_table_next", opAF_jump_table_next, INNER),
	routine(0x537A, "opB0_store", opB0_store, INNER),
	routine(0x53C7, "opB1_store_add", opB1_store_add, INNER),
	routine(0x5400, "opB2_load", opB2_load, INNER),
	routine(0x5406, "opB9_compare_source", opB9_compare_source, INNER),
	routine(0x5409, "opBA_compare", opBA_compare, INNER),
	routine(0x5415, "opB5_if_zero", opB5_if_zero, INNER),
	routine(0x541C, "opB6_if_nonzero", opB6_if_nonzero, INNER),
	routine(0x5423, "opB7_if_plus", opB7_if_plus, INNER),
	routine(0x542A, "opB8_if_minus", opB8_if_minus, INNER),
	routine(0x5449, "source_value", source_value),
	routine(0x54B6, "op9C_pair_second", op9C_pair_second, INNER),
	routine(0x54D1, "op90_pair_first", op90_pair_first, INNER),
	routine(0x54EA, "op91_pair_second", op91_pair_second, INNER),
	routine(0x54F9, "op96_reply", op96_reply, INNER),
	routine(0x54FE, "op97_fade", op97_fade, INNER),
	routine(0x551A, "op99_jump", op99_jump, INNER),
	routine(0x5529, "op9A_speak", op9A_speak, INNER),
	routine(0x5533, "op9D_voice", op9D_voice, INNER),
	routine(0x558D, "ym_voice", ym_voice),
	routine(0x5611, "op9E_lfo", op9E_lfo, INNER),
	routine(0x5653, "op9F_noise", op9F_noise, INNER),
	routine(0x5674, "ym_data_reg", ym_data_reg),
	routine(0x5688, "opA0_output", opA0_output, INNER),
	routine(0x56A7, "opA3_ym_volume_add", opA3_ym_volume_add, INNER),
	routine(0x56C3, "opA2_ym_volume", opA2_ym_volume, INNER),
	routine(0x56D4, "opA7_bend", opA7_bend, INNER),
	routine(0x56FB, "opA6_fraction", opA6_fraction, INNER),
	routine(0x5709, "opA8_loops", opA8_loops, INNER),
	routine(0x570D, "opA1_levels_add", opA1_levels_add, INNER),
)

} // namespace sound
