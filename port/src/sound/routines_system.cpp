// Rewritten sound routines: the board's other jobs (see sound.h, ops.h): the coin inputs and
// credits, the play clocks, the counter inputs at 0x1810 and the EEPROM (0x1000-0x11FF), where
// the statistics live as 16-byte blocks with check bits, written one byte per IRQ.
//
// The EEPROM holds 22 slots of 32 bytes (0x1000 + slot * 32, see F490); 0x0D4C + slot is the
// record (high nibble) and sequence number (low nibble) in each, 0x0D62 + record the latest
// sequence of each of the 11 records, 0x0D6D + record * 10 their data.  A record to write is
// queued at 0x0DEB (0x87 / 0x88 the ends of the queue) with its data copied to 0x0DF7.
#include "ops.h"

namespace sound {
namespace {

using namespace s65rt;
using namespace op;

// ---- coins

// F00E: the coin inputs (0x1840 bits 7, 6, 5 for X = 2, 1, 0), debounced by the counts 0x53 + X;
// each coin adds to the coin units 0x4A and 0x4F (by the mech settings in 0x49) and to the coin
// counter pulses 0x4C + X.  Coin units become credits 0x48 (bonus units by 0x49 bits 5-7, the
// units per credit by bits 0-1); then the coin counters' pulses run down, a new pulse is
// counted in the statistics 0x0D6D (0x86 = 0xFF: to be saved) and counters 1 and 2 go out to
// 0x1876 (bits 1 and 0).
void coin_inputs()
{
	ldy(imm(0x00), 0xF010);
	ldx(imm(0x02), 0xF012);
lF012:
	lda(ab(0x1840), 0xF015);
	cpx(imm(0x01), 0xF017);
	{
		int shifts;                                    // the coin's bit into the carry
		if (beq(0xF017, 0xF01C))
			shifts = 2;
		else if (bcs(0xF019, 0xF01D))
			shifts = 1;
		else
			shifts = 3;
		for (uint16_t pc = (uint16_t)(0xF01E - shifts); pc < 0xF01E; pc++) asl_a((uint16_t)(pc + 1));
	}
	lda(zpx(0x53), 0xF020);
	and_(imm(0x1F), 0xF022);
	if (bcs(0xF022, 0xF03C)) goto lF03C;
	// the input low: the count down (every frame from 0x1B up, below that every eighth)
	if (beq(0xF024, 0xF037)) goto lF037;
	cmp(imm(0x1B), 0xF028);
	if (!bcs(0xF028, 0xF035))
	{
		pha(0xF02B);
		lda(ab(0x0000), 0xF02E);
		and_(imm(0x07), 0xF030);
		cmp(imm(0x07), 0xF032);
		pla(0xF033);
		if (bcc(0xF033, 0xF037)) goto lF037;
	}
	sbc(imm(0x01), 0xF037);
lF037:
	sta(wzpx(0x53), 0xF039);
	jmp_to(0xF07A);
	goto lF07A;
lF03C:
	// the input high: below 0x1B the count up by 0x20; a coin when that carries out past 0
	cmp(imm(0x1B), 0xF03E);
	if (!bcs(0xF03E, 0xF049))
	{
		lda(zpx(0x53), 0xF042);
		adc(imm(0x20), 0xF044);
		if (bcc(0xF044, 0xF037)) goto lF037;
		if (!beq(0xF046, 0xF049)) clc(0xF049);
	}
	lda(imm(0x1F), 0xF04B);
	sta(wzpx(0x53), 0xF04D);
	if (bcs(0xF04D, 0xF07A)) goto lF07A;
	// a coin: its units (mech 0: 1; mech 1: 2 with 0x49 bit 4; mech 2: by 0x49 bits 2-3)
	iny(0xF050);
	lda(imm(0x00), 0xF052);
	cpx(imm(0x01), 0xF054);
	if (!bcc(0xF054, 0xF06C))
	{
		if (beq(0xF056, 0xF064))
		{
			lda(zp(0x49), 0xF066);
			and_(imm(0x10), 0xF068);
			if (!beq(0xF068, 0xF06C)) lda(imm(0x01), 0xF06C);
		}
		else
		{
			lda(zp(0x49), 0xF05A);
			and_(imm(0x0C), 0xF05C);
			lsr_a(0xF05D);
			lsr_a(0xF05E);
			if (!beq(0xF05E, 0xF06C))
			{
				adc(imm(0x02), 0xF062);
				bne(0xF062, 0xF06C);                   // BNE (always)
			}
		}
	}
	sec(0xF06D);
	pha(0xF06E);
	adc(zp(0x4A), 0xF070);
	sta(wzp(0x4A), 0xF072);
	pla(0xF073);
	sec(0xF074);
	adc(zp(0x4F), 0xF076);
	sta(wzp(0x4F), 0xF078);
	inc(mzpx(0x4C), 0xF07A);
lF07A:
	dex(0xF07B);
	if (bpl(0xF07B, 0xF012)) goto lF012;

	// credits from the coin units
	tya(0xF07E);
	if (beq(0xF07E, 0xF0C1)) goto lF0C1;
	lda(zp(0x49), 0xF082);
	for (uint16_t pc = 0xF083; pc <= 0xF087; pc++) lsr_a(pc);
	eor(imm(0x07), 0xF089);
	if (beq(0xF089, 0xF0BD)) goto lF0BD;
	tay(0xF08C);
	lda(zp(0x4A), 0xF08E);
lF08E:
	sec(0xF08F);
lF08F:
	sbc(aby(0xF0A0), 0xF092);
	if (!bmi(0xF092, 0xF0A8))
	{
		// the bonus units
		sta(wzp(0x4A), 0xF096);
		inc(mzp(0x4B), 0xF098);
		cpy(imm(0x04), 0xF09A);
		if (bne(0xF09A, 0xF08E)) goto lF08E;
		inc(mzp(0x4B), 0xF09E);
		bcs(0xF09E, 0xF08F);                           // BCS (always: Y = 4)
		goto lF08F;
	}
lF0A8:
	lda(zp(0x49), 0xF0AA);
	and_(imm(0x03), 0xF0AC);
	eor(imm(0xFF), 0xF0AE);
	clc(0xF0AF);
	adc(zp(0x4F), 0xF0B1);
	if (!bcs(0xF0B1, 0xF0BB))
	{
		adc(zp(0x4B), 0xF0B5);
		if (bmi(0xF0B5, 0xF0C1)) goto lF0C1;
		sta(wzp(0x4B), 0xF0B9);
		lda(imm(0x00), 0xF0BB);
	}
	inc(mzp(0x48), 0xF0BD);
lF0BD:
	sta(wzp(0x4F), 0xF0BF);
	if (bne(0xF0BF, 0xF0A8)) goto lF0A8;

lF0C1:
	// the coin counters' pulses
	lda(ab(0x0000), 0xF0C4);
	lsr_a(0xF0C5);
	if (!bcs(0xF0C5, 0xF0F6))
	{
		lda(zp(0x4C), 0xF0C9);
		ora(zp(0x4D), 0xF0CB);
		ora(zp(0x4E), 0xF0CD);
		if (!beq(0xF0CD, 0xF0F6))
		{
			cmp(imm(0x10), 0xF0D1);
			if (!bcc(0xF0D1, 0xF0E6))
			{
				ldx(imm(0x02), 0xF0D5);
				do
				{
					lda(zpx(0x4C), 0xF0D7);
					if (!beq(0xF0D7, 0xF0E1))
					{
						cmp(imm(0x10), 0xF0DB);
						if (!bcc(0xF0DB, 0xF0E1))
						{
							adc(imm(0xEF), 0xF0DF);
							sta(wzpx(0x4C), 0xF0E1);
						}
					}
					dex(0xF0E2);
				} while (bpl(0xF0E2, 0xF0D5));
				bmi(0xF0E4, 0xF0F6);                   // BMI (always)
			}
			else
			{
				ldx(imm(0x02), 0xF0E8);
				do
				{
					lda(zpx(0x4C), 0xF0EA);
					if (!beq(0xF0EA, 0xF0F3))
					{
						clc(0xF0ED);
						adc(imm(0xEF), 0xF0EF);
						sta(wzpx(0x4C), 0xF0F1);
						if (bmi(0xF0F1, 0xF0F6)) break;
					}
					dex(0xF0F4);
				} while (bpl(0xF0F4, 0xF0E8));
			}
		}
	}
	ldx(imm(0x02), 0xF0F8);
	do
	{
		lda(zpx(0x4C), 0xF0FA);
		if (!bpl(0xF0FA, 0xF11C))
		{
			lda(zpx(0x57), 0xF0FE);
			if (!bmi(0xF0FE, 0xF11C))
			{
				// a pulse started: counted in the statistics (16 bits at 0x0D6D + 0xF14A[X])
				ldy(abx(0xF14A), 0xF103);
				lda(aby(0x0D6D), 0xF106);
				clc(0xF107);
				adc(imm(0x01), 0xF109);
				sta(waby(0x0D6D), 0xF10C);
				iny(0xF10D);
				if (!bcc(0xF10D, 0xF117))
				{
					lda(aby(0x0D6D), 0xF112);
					adc(imm(0x00), 0xF114);
					sta(waby(0x0D6D), 0xF117);
				}
				lda(imm(0xFF), 0xF119);
				sta(wab(0x0086), 0xF11C);
			}
		}
		lda(zpx(0x4C), 0xF11E);
		sta(wzpx(0x57), 0xF120);
		dex(0xF121);
	} while (bpl(0xF121, 0xF0F8));
	lda(zp(0x4D), 0xF125);
	rol_a(0xF126);
	lda(zp(0x5A), 0xF128);
	if (!bcc(0xF128, 0xF12F))
	{
		ora(imm(0x02), 0xF12C);
		jmp_to(0xF131);
	}
	else
		and_(imm(0xFD), 0xF131);
	sta(wzp(0x5A), 0xF133);
	sta(wab(0x1876), 0xF136);
	lda(zp(0x4E), 0xF138);
	rol_a(0xF139);
	lda(zp(0x5A), 0xF13B);
	if (!bcc(0xF13B, 0xF142))
	{
		ora(imm(0x01), 0xF13F);
		jmp_to(0xF144);
	}
	else
		and_(imm(0xFE), 0xF144);
	sta(wzp(0x5A), 0xF146);
	sta(wab(0x1876), 0xF149);
	rts();
}

// F15B: unless 0x66 = 0x20, the coins (F00E) with the credits 0x48 kept at most 0x1F (6 when
// the settings 0x49 bits 5-7 are all set)
void coins()
{
	lda(zp(0x66), 0xF15D);
	cmp(imm(0x20), 0xF15F);
	if (!bne(0xF15F, 0xF162))
	{
		rts();
		return;
	}
	jsr(0xF162, 0xF00E);                               // coin_inputs
	ldx(imm(0x00), 0xF167);
	lda(imm(0x1F), 0xF169);
	do
	{
		cmp(zpx(0x48), 0xF16B);
		if (!bcs(0xF16B, 0xF16F)) sta(wzpx(0x48), 0xF16F);
		dex(0xF170);
	} while (bpl(0xF170, 0xF169));
	lda(zp(0x49), 0xF174);
	and_(imm(0xE0), 0xF176);
	cmp(imm(0xE0), 0xF178);
	if (!bne(0xF178, 0xF17E))
	{
		lda(imm(0x06), 0xF17C);
		sta(wzp(0x48), 0xF17E);
	}
	rts();
}

// ---- clocks

// F1F4: clock X one second on (seconds 0x6D + X, minutes 0x6F + X)
void clock_tick()
{
	clc(0xF1F5);
	lda(zpx(0x6D), 0xF1F7);
	adc(imm(0x01), 0xF1F9);
	cmp(imm(0x3C), 0xF1FB);
	if (!bne(0xF1FB, 0xF205))
	{
		lda(zpx(0x6F), 0xF1FF);
		adc(imm(0x00), 0xF201);
		sta(wzpx(0x6F), 0xF203);
		lda(imm(0x00), 0xF205);
	}
	sta(wzpx(0x6D), 0xF207);
	rts();
}

// F17F: every 250 IRQs (a second): during a game (0x66 bit 7) the clocks of the players in
// 0x68 (bits 0-1): X, X + 4 and X + 8 (X + 12 when 0x69 + X is set); the power-on clock 0x7D-0x80
// (seconds, minutes, then 16 bits of hours).  Every IRQ: 0x92 counts down, and 0x85 too, at 0
// the coin units 0x4A / 0x4B cleared.
void play_clocks()
{
	dec(mzp(0x5B), 0xF181);
	if (!bne(0xF181, 0xF1DF))
	{
		lda(imm(0xFA), 0xF185);
		sta(wzp(0x5B), 0xF187);
		bit(zp(0x66), 0xF189);
		if (!bpl(0xF189, 0xF1BA))
		{
			ldx(imm(0x00), 0xF18D);
			lda(zp(0x68), 0xF18F);
			do
			{
				lsr_a(0xF190);
				if (!bcc(0xF190, 0xF1B5))
				{
					pha(0xF193);
					txa(0xF194);
					pha(0xF195);
					jsr(0xF195, 0xF1F4);                   // clock_tick
					txa(0xF199);
					ora(imm(0x04), 0xF19B);
					tax(0xF19C);
					jsr(0xF19C, 0xF1F4);
					pla(0xF1A0);
					tax(0xF1A1);
					pha(0xF1A2);
					lda(zpx(0x69), 0xF1A4);
					if (beq(0xF1A4, 0xF1AB))
					{
						txa(0xF1AC);
						ora(imm(0x08), 0xF1AE);
					}
					else
					{
						txa(0xF1A7);
						ora(imm(0x0C), 0xF1A9);
						bne(0xF1A9, 0xF1AE);               // BNE (always)
					}
					tax(0xF1AF);
					jsr(0xF1AF, 0xF1F4);
					pla(0xF1B3);
					tax(0xF1B4);
					pla(0xF1B5);
				}
				inx(0xF1B6);
				cpx(imm(0x02), 0xF1B8);
			} while (bne(0xF1B8, 0xF18F));
		}
		clc(0xF1BB);
		lda(zp(0x80), 0xF1BD);
		adc(imm(0x01), 0xF1BF);
		cmp(imm(0x3C), 0xF1C1);
		if (!bne(0xF1C1, 0xF1DD))
		{
			lda(zp(0x7F), 0xF1C5);
			adc(imm(0x00), 0xF1C7);
			cmp(imm(0x3C), 0xF1C9);
			if (!bne(0xF1C9, 0xF1D9))
			{
				lda(zp(0x7E), 0xF1CD);
				adc(imm(0x00), 0xF1CF);
				sta(wzp(0x7E), 0xF1D1);
				lda(zp(0x7D), 0xF1D3);
				adc(imm(0x00), 0xF1D5);
				sta(wzp(0x7D), 0xF1D7);
				lda(imm(0x00), 0xF1D9);
			}
			sta(wzp(0x7F), 0xF1DB);
			lda(imm(0x00), 0xF1DD);
		}
		sta(wzp(0x80), 0xF1DF);
	}
	lda(zp(0x92), 0xF1E1);
	if (!beq(0xF1E1, 0xF1E5)) dec(mzp(0x92), 0xF1E5);
	lda(zp(0x85), 0xF1E7);
	if (!beq(0xF1E7, 0xF1F3))
	{
		dec(mzp(0x85), 0xF1EB);
		if (!bne(0xF1EB, 0xF1F3))
		{
			lda(imm(0x00), 0xF1EF);
			sta(wzp(0x4A), 0xF1F1);
			sta(wzp(0x4B), 0xF1F3);
		}
	}
	rts();
}

// ---- the EEPROM

// F376: A, the byte at position X of the block 0x0DDB, into its check bytes: 0x0DDC, 0x0DDD,
// 0x0DDF, 0x0DE3 for the bits 0-3 of X, and 0x0DDB (A and X kept)
void eeprom_parity()
{
	pha(0xF377);
	stx(wzp(0x63), 0xF379);
	static const uint16_t check[4] = {0x0DDC, 0x0DDD, 0x0DDF, 0x0DE3};
	uint16_t pc = 0xF379;
	for (int i = 0; i < 4; i++)
	{
		lsr(mzp(0x63), (uint16_t)(pc + 2));
		if (!bcc((uint16_t)(pc + 2), (uint16_t)(pc + 0x0C)))
		{
			pla((uint16_t)(pc + 5));
			pha((uint16_t)(pc + 6));
			eor(ab(check[i]), (uint16_t)(pc + 9));
			sta(wab(check[i]), (uint16_t)(pc + 0x0C));
		}
		pc = (uint16_t)(pc + 0x0C);
	}
	pla(0xF3AA);
	eor(ab(0x0DDB), 0xF3AD);
	sta(wab(0x0DDB), 0xF3B0);
	rts();
}

// F303: the block 0x0DDB for record 0x8F from its data at 0x0DF7 + X * 10: the record and its
// next sequence number at position 0xF370 (15), the ten data bytes at the positions of 0xF366,
// the check bytes (F376) and the overall check in 0x0DDB
void eeprom_encode()
{
	stx(wzp(0x62), 0xF305);
	lda(imm(0x00), 0xF307);
	sta(wab(0x0DDB), 0xF30A);
	sta(wab(0x0DDC), 0xF30D);
	sta(wab(0x0DDD), 0xF310);
	sta(wab(0x0DDF), 0xF313);
	sta(wab(0x0DE3), 0xF316);
	ldy(zp(0x8F), 0xF318);
	tya(0xF319);
	for (uint16_t pc = 0xF31A; pc <= 0xF31D; pc++) asl_a(pc);
	sta(wzp(0x63), 0xF31F);
	lda(aby(0x0D62), 0xF322);
	clc(0xF323);
	adc(imm(0x01), 0xF325);
	and_(imm(0x0F), 0xF327);
	ora(zp(0x63), 0xF329);
	ldx(ab(0xF370), 0xF32C);
	sta(wabx(0x0DDB), 0xF32F);
	jsr(0xF32F, 0xF376);                               // eeprom_parity
	lda(zp(0x62), 0xF334);
	jsr(0xF334, 0xF488);                               // times10
	tay(0xF338);
	ldx(imm(0x00), 0xF33A);
	do
	{
		lda(abx(0xF366), 0xF33D);
		stx(wzp(0x62), 0xF33F);
		tax(0xF340);
		lda(aby(0x0DF7), 0xF343);
		sta(wabx(0x0DDB), 0xF346);
		jsr(0xF346, 0xF376);                           // eeprom_parity
		ldx(zp(0x62), 0xF34B);
		iny(0xF34C);
		inx(0xF34D);
		cpx(imm(0x0A), 0xF34F);
	} while (bne(0xF34F, 0xF33A));
	lda(ab(0x0DDB), 0xF354);
	eor(ab(0x0DDC), 0xF357);
	eor(ab(0x0DDD), 0xF35A);
	eor(ab(0x0DDF), 0xF35D);
	eor(ab(0x0DE3), 0xF360);
	eor(imm(0xFF), 0xF362);
	sta(wab(0x0DDB), 0xF365);
	rts();
}

// F4A8: X = the slot holding the oldest copy of record 0x8F (its sequence number less than 8
// behind the others'), 0xFF if none
void eeprom_oldest()
{
	ldx(imm(0x15), 0xF4AA);
	lda(imm(0xFF), 0xF4AC);
	sta(wzp(0x64), 0xF4AE);
	do
	{
		lda(abx(0x0D4C), 0xF4B1);
		cmp(imm(0xFF), 0xF4B3);
		if (!beq(0xF4B3, 0xF4E3))
		{
			sta(wzp(0x63), 0xF4B7);
			for (uint16_t pc = 0xF4B8; pc <= 0xF4BB; pc++) lsr_a(pc);
			cmp(zp(0x8F), 0xF4BD);
			if (!bne(0xF4BD, 0xF4E3))
			{
				ldy(zp(0x64), 0xF4C1);
				cpy(imm(0xFF), 0xF4C3);
				bool take = true;
				if (!beq(0xF4C3, 0xF4E1))
				{
					lda(zp(0x63), 0xF4C7);
					and_(imm(0x0F), 0xF4C9);
					sta(wzp(0x63), 0xF4CB);
					lda(aby(0x0D4C), 0xF4CE);
					and_(imm(0x0F), 0xF4D0);
					sec(0xF4D1);
					sbc(zp(0x63), 0xF4D3);
					if (!bcs(0xF4D3, 0xF4DD))
					{
						eor(imm(0xFF), 0xF4D7);
						cmp(imm(0x08), 0xF4D9);
						if (!bcs(0xF4D9, 0xF4E1))
						{
							bcc(0xF4DB, 0xF4E3);           // BCC (always)
							take = false;
						}
					}
					else
					{
						cmp(imm(0x08), 0xF4DF);
						take = !bcs(0xF4DF, 0xF4E3);
					}
				}
				if (take) stx(wzp(0x64), 0xF4E3);
			}
		}
		dex(0xF4E4);
	} while (bpl(0xF4E4, 0xF4AE));
	ldx(zp(0x64), 0xF4E8);
	rts();
}

// F233: one step of writing to the EEPROM (carry set when a byte was written): the block 0x0DDB
// goes from its last byte (0x89) down into the slot at (0x8D), each byte read back and written
// again up to 10 times (then the slot is dropped and the block goes to another); with no block
// under way the next queued record (0x0DEB) is encoded (F303) into the slot of its oldest copy,
// or (with 6 or more copies, or none) the next of the slots in turn (0x8C) not holding a
// record's latest copy
void eeprom_write_step()
{
	ldy(zp(0x89), 0xF235);
	cpy(imm(0xFF), 0xF237);
	if (beq(0xF237, 0xF26C)) goto lF26C;
	lda(aby(0x0DDB), 0xF23C);
	cmp(iy(0x8D), 0xF23E);
	if (!beq(0xF23E, 0xF25A))
	{
		inc(mzp(0x8A), 0xF242);
		lda(zp(0x8A), 0xF244);
		cmp(imm(0x0A), 0xF246);
		if (bcc(0xF246, 0xF265)) goto lF265;
		ldx(zp(0x8B), 0xF24A);
		lda(abx(0x0D4C), 0xF24D);
		for (uint16_t pc = 0xF24E; pc <= 0xF251; pc++) lsr_a(pc);
		sta(wzp(0x8F), 0xF253);
		lda(imm(0xFF), 0xF255);
		sta(wabx(0x0D4C), 0xF258);
		bne(0xF258, 0xF28F);                           // BNE (always)
		goto lF28F;
	}
	dey(0xF25B);
	sty(wzp(0x89), 0xF25D);
	cpy(imm(0xFF), 0xF25F);
	if (beq(0xF25F, 0xF26C)) goto lF26C;
	lda(imm(0x00), 0xF263);
	sta(wzp(0x8A), 0xF265);
lF265:
	lda(aby(0x0DDB), 0xF268);
	sta(wiy(0x8D), 0xF26A);
	sec(0xF26B);
	rts();
	return;

lF26C:
	ldx(zp(0x88), 0xF26E);
	cpx(zp(0x87), 0xF270);
	if (!bne(0xF270, 0xF274))
	{
		clc(0xF273);
		rts();
		return;
	}
	lda(abx(0x0DEB), 0xF277);
	sta(wzp(0x8F), 0xF279);
	jsr(0xF279, 0xF303);                               // eeprom_encode
	lda(imm(0x0F), 0xF27E);
	sta(wzp(0x89), 0xF280);
	lda(imm(0x00), 0xF282);
	sta(wzp(0x8A), 0xF284);
	ldx(zp(0x88), 0xF286);
	inx(0xF287);
	cpx(imm(0x0C), 0xF289);
	if (!bcc(0xF289, 0xF28D)) ldx(imm(0x00), 0xF28D);
	stx(wzp(0x88), 0xF28F);
lF28F:
	jsr(0xF28F, 0xF4A8);                               // eeprom_oldest
	cpx(imm(0xFF), 0xF294);
	if (!beq(0xF294, 0xF2B5))
	{
		ldy(zp(0x8F), 0xF298);
		lda(abx(0x0D4C), 0xF29B);
		and_(imm(0x0F), 0xF29D);
		sta(wzp(0x64), 0xF29F);
		lda(aby(0x0D62), 0xF2A2);
		sec(0xF2A3);
		sbc(zp(0x64), 0xF2A5);
		if (!bcs(0xF2A5, 0xF2AE))
		{
			lda(aby(0x0D62), 0xF2AA);
			ora(imm(0x10), 0xF2AC);
			sbc(zp(0x64), 0xF2AE);
		}
		cmp(imm(0x06), 0xF2B0);
		if (!bcc(0xF2B0, 0xF2B5))
		{
			jmp_to(0xF2DE);
			goto lF2DE;
		}
	}
lF2B5:
	ldy(zp(0x8C), 0xF2B7);
	lda(aby(0x0D4C), 0xF2BA);
	cmp(imm(0xFF), 0xF2BC);
	if (!beq(0xF2BC, 0xF2D7))
	{
		and_(imm(0x0F), 0xF2C0);
		sta(wzp(0x64), 0xF2C2);
		lda(aby(0x0D4C), 0xF2C5);
		for (uint16_t pc = 0xF2C6; pc <= 0xF2C9; pc++) lsr_a(pc);
		tax(0xF2CA);
		lda(abx(0x0D62), 0xF2CD);
		cmp(zp(0x64), 0xF2CF);
		if (!bne(0xF2CF, 0xF2D7))
		{
			jsr(0xF2D1, 0xF47B);                       // count_8c
			jmp_to(0xF2B5);
			goto lF2B5;
		}
	}
	tya(0xF2D8);
	pha(0xF2D9);
	jsr(0xF2D9, 0xF47B);                               // count_8c
	pla(0xF2DD);
	tax(0xF2DE);
lF2DE:
	ldy(ab(0xF370), 0xF2E1);
	lda(aby(0x0DDB), 0xF2E4);
	sta(wabx(0x0D4C), 0xF2E7);
	txa(0xF2E8);
	sta(wzp(0x8B), 0xF2EA);
	jsr(0xF2EA, 0xF490);                               // ptr_1000
	ldy(zp(0x8F), 0xF2EF);
	lda(aby(0x0D62), 0xF2F2);
	clc(0xF2F3);
	adc(imm(0x01), 0xF2F5);
	and_(imm(0x0F), 0xF2F7);
	sta(waby(0x0D62), 0xF2FA);
	ldy(zp(0x89), 0xF2FC);
	lda(aby(0x0DDB), 0xF2FF);
	sta(wiy(0x8D), 0xF301);
	sec(0xF302);
	rts();
}

// F208: every third IRQ (0x92) an EEPROM step (F233); when it wrote nothing, a byte of the
// queue 0x0C20 (value, then offset; 0x91 / 0x90 its ends) goes to 0x1160 + offset
void eeprom_writer()
{
	lda(zp(0x92), 0xF20A);
	if (!beq(0xF20A, 0xF20D))
	{
		rts();
		return;
	}
	jsr(0xF20D, 0xF233);                               // eeprom_write_step
	if (!bcs(0xF210, 0xF22E))
	{
		ldx(zp(0x91), 0xF214);
		cpx(zp(0x90), 0xF216);
		if (!beq(0xF216, 0xF22E))
		{
			lda(abx(0x0C20), 0xF21B);
			inx(0xF21C);
			pha(0xF21D);
			lda(abx(0x0C20), 0xF220);
			inx(0xF221);
			tay(0xF222);
			pla(0xF223);
			sta(waby(0x1160), 0xF226);
			cpx(imm(0x28), 0xF228);
			if (!bcc(0xF228, 0xF22C)) ldx(imm(0x00), 0xF22C);
			stx(wzp(0x91), 0xF22E);
		}
	}
	lda(imm(0x03), 0xF230);
	sta(wzp(0x92), 0xF232);
	rts();
}

// ---- the counter inputs

// F53B: the counters at 0x1810 / 0x1811: the change since last time (0xA4 + X) added to the 16
// bits at 0x94 + X / 0x98 + X (signed); 0x98 = 0x95 while 0xA4 is not 0
void counter_inputs()
{
	ldx(imm(0x01), 0xF53D);
	do
	{
		sec(0xF53E);
		lda(abx(0x1810), 0xF541);
		pha(0xF542);
		sbc(zpx(0xA4), 0xF544);
		if (bmi(0xF544, 0xF55E))
		{
			clc(0xF55F);
			adc(zpx(0x94), 0xF561);
			if (!bcs(0xF561, 0xF54D))
			{
				dec(mzpx(0x98), 0xF565);
				jmp_to(0xF54D);
			}
		}
		else
		{
			clc(0xF547);
			adc(zpx(0x94), 0xF549);
			if (!bcc(0xF549, 0xF54D)) inc(mzpx(0x98), 0xF54D);
		}
		sta(wzpx(0x94), 0xF54F);
		pla(0xF550);
		sta(wzpx(0xA4), 0xF552);
		dex(0xF553);
	} while (bpl(0xF553, 0xF53D));
	lda(zp(0xA4), 0xF557);
	if (!beq(0xF557, 0xF55D))
	{
		lda(zp(0x95), 0xF55B);
		sta(wzp(0x98), 0xF55D);
	}
	rts();
}

// F3B1: slot X of the EEPROM read into 0x0DDB and checked: its ten data bytes to 0x0E6F and
// its record / sequence byte to 0x0E79 and 0x0D4C + X, with a single wrong bit put right (by
// the check bytes, 0xF46B); an error that cannot be put right: 0xFF.
// Returns A = the record / sequence byte.
void eeprom_read()
{
	txa(0xF3B2);
	pha(0xF3B3);
	jsr(0xF3B3, 0xF490);                               // ptr_1000
	ldy(imm(0x00), 0xF3B8);
	do
	{
		lda(iy(0x8D), 0xF3BA);
		sta(waby(0x0DDB), 0xF3BD);
		iny(0xF3BE);
		cpy(imm(0x10), 0xF3C0);
	} while (bne(0xF3C0, 0xF3B8));
	lda(ab(0x0DDB), 0xF3C5);
	eor(imm(0xFF), 0xF3C7);
	eor(ab(0x0DDC), 0xF3CA);
	eor(ab(0x0DDD), 0xF3CD);
	eor(ab(0x0DDF), 0xF3D0);
	eor(ab(0x0DE3), 0xF3D3);
	sta(wab(0x0DDB), 0xF3D6);
	ldy(imm(0x00), 0xF3D8);
	do
	{
		lda(aby(0xF366), 0xF3DB);
		tax(0xF3DC);
		lda(abx(0x0DDB), 0xF3DF);
		sta(waby(0x0E6F), 0xF3E2);
		jsr(0xF3E2, 0xF376);                           // eeprom_parity
		iny(0xF3E6);
		cpy(imm(0x0B), 0xF3E8);
	} while (bne(0xF3E8, 0xF3D8));
	// what is left in the check bytes: 0 when all is well
	lda(ab(0x0DDB), 0xF3ED);
	ora(ab(0x0DDC), 0xF3F0);
	ora(ab(0x0DDD), 0xF3F3);
	ora(ab(0x0DDF), 0xF3F6);
	ora(ab(0x0DE3), 0xF3F9);
	if (beq(0xF3F9, 0xF457)) goto lF457;
	lda(ab(0x0DDB), 0xF3FE);
	if (beq(0xF3FE, 0xF464)) goto lF464;
	lda(imm(0x01), 0xF402);
	sta(wzp(0x63), 0xF404);
lF404:
	{
		// for each bit 0x63: the wrong position from the check bytes
		lda(imm(0x00), 0xF406);
		sta(wzp(0x62), 0xF408);
		static const struct { uint16_t check; uint8_t bit; } pos[4] = {
			{0x0DE3, 0x08}, {0x0DDF, 0x04}, {0x0DDD, 0x02}, {0x0DDC, 0x01},
		};
		uint16_t pc = 0xF408;
		for (int i = 0; i < 4; i++)
		{
			lda(ab(pos[i].check), (uint16_t)(pc + 3));
			and_(zp(0x63), (uint16_t)(pc + 5));
			if (!beq((uint16_t)(pc + 5), (uint16_t)(pc + 0x0D)))
			{
				lda(imm(pos[i].bit), (uint16_t)(pc + 9));
				ora(zp(0x62), (uint16_t)(pc + 0x0B));
				sta(wzp(0x62), (uint16_t)(pc + 0x0D));
			}
			pc = (uint16_t)(pc + 0x0D);
		}
	}
	lda(ab(0x0DDB), 0xF43F);
	and_(zp(0x63), 0xF441);
	if (!beq(0xF441, 0xF453))
	{
		ldx(zp(0x62), 0xF445);
		lda(abx(0xF46B), 0xF448);
		if (bmi(0xF448, 0xF460))
		{
			lda(zp(0x62), 0xF462);
			if (!beq(0xF462, 0xF453)) goto lF464;
		}
		else
		{
			tax(0xF44B);
			lda(abx(0x0E6F), 0xF44E);
			eor(zp(0x63), 0xF450);
			sta(wabx(0x0E6F), 0xF453);
		}
	}
	asl(mzp(0x63), 0xF455);
	if (bcc(0xF455, 0xF404)) goto lF404;
lF457:
	pla(0xF458);
	tax(0xF459);
	lda(ab(0x0E79), 0xF45C);
	sta(wabx(0x0D4C), 0xF45F);
	rts();
	return;
lF464:
	lda(imm(0xFF), 0xF466);
	sta(wab(0x0E79), 0xF469);
	bne(0xF469, 0xF457);                               // BNE (always)
	goto lF457;
}

// F4E9: record A queued for the EEPROM (0x0DEB, unless it is queued already and not the one
// being written) with its data 0x0D6D + A * 10 copied to its place 0x0DF7 + queue slot * 10
// (A, X and Y kept)
void eeprom_queue()
{
	sta(wzp(0x61), 0xF4EB);
	txa(0xF4EC);
	pha(0xF4ED);
	tya(0xF4EE);
	pha(0xF4EF);
	ldx(zp(0x88), 0xF4F1);
	cpx(zp(0x87), 0xF4F3);
	if (!beq(0xF4F3, 0xF50B))
	{
		do
		{
			lda(abx(0x0DEB), 0xF4F8);
			cmp(zp(0x61), 0xF4FA);
			if (!bne(0xF4FA, 0xF500))
			{
				cpx(zp(0x88), 0xF4FE);
				if (bne(0xF4FE, 0xF51B)) goto lF51B;
			}
			inx(0xF501);
			cpx(imm(0x0C), 0xF503);
			if (!bcc(0xF503, 0xF507)) ldx(imm(0x00), 0xF507);
			cpx(zp(0x87), 0xF509);
		} while (bne(0xF509, 0xF4F5));
	}
	lda(zp(0x61), 0xF50D);
	sta(wabx(0x0DEB), 0xF510);
	txa(0xF511);
	inx(0xF512);
	cpx(imm(0x0C), 0xF514);
	if (!bcc(0xF514, 0xF518)) ldx(imm(0x00), 0xF518);
	stx(wzp(0x87), 0xF51A);
	tax(0xF51B);
lF51B:
	lda(zp(0x61), 0xF51D);
	jsr(0xF51D, 0xF488);                               // times10
	tay(0xF521);
	txa(0xF522);
	jsr(0xF522, 0xF488);
	tax(0xF526);
	lda(imm(0x09), 0xF528);
	sta(wzp(0x61), 0xF52A);
	do
	{
		lda(aby(0x0D6D), 0xF52D);
		sta(wabx(0x0DF7), 0xF530);
		inx(0xF531);
		iny(0xF532);
		dec(mzp(0x61), 0xF534);
	} while (bpl(0xF534, 0xF52A));
	pla(0xF537);
	tay(0xF538);
	pla(0xF539);
	tax(0xF53A);
	rts();
}

// ---- letters and times

// FAC6: a letter (0x41-0x5E) as 0-0x1D, anything else 0x1F
void letter_code()
{
	cmp(imm(0x5F), 0xFAC8);
	if (!bcs(0xFAC8, 0xFACF))
	{
		sec(0xFACB);
		sbc(imm(0x41), 0xFACD);
		if (bcs(0xFACD, 0xFAD1))
		{
			rts();
			return;
		}
	}
	lda(imm(0x1F), 0xFAD1);
	rts();
}

// FAD2: the letter of code A (bits 0-4; past 0x5E a space)
void code_letter()
{
	and_(imm(0x1F), 0xFAD4);
	clc(0xFAD5);
	adc(imm(0x41), 0xFAD7);
	cmp(imm(0x5F), 0xFAD9);
	if (!bcc(0xFAD9, 0xFADD)) lda(imm(0x20), 0xFADD);
	rts();
}

// FB2A: the statistic Y (16 bits at 0x0D6D + Y) one up
void stat_inc()
{
	lda(aby(0x0D6D), 0xFB2D);
	clc(0xFB2E);
	adc(imm(0x01), 0xFB30);
	sta(waby(0x0D6D), 0xFB33);
	if (!bcc(0xFB33, 0xFB3D))
	{
		lda(aby(0x0D6E), 0xFB38);
		adc(imm(0x00), 0xFB3A);
		sta(waby(0x0D6E), 0xFB3D);
	}
	rts();
}

// FB3E: the record holding statistic Y (Y / 10) queued for the EEPROM (F4E9); X kept
void stat_queue()
{
	txa(0xFB3F);
	pha(0xFB40);
	sty(wzp(0x60), 0xFB42);
	ldx(imm(0x00), 0xFB44);
	lda(imm(0x0A), 0xFB46);
	do
	{
		cmp(zp(0x60), 0xFB48);
		if (!beq(0xFB48, 0xFB4C) && bcs(0xFB4A, 0xFB54)) break;
		clc(0xFB4D);
		adc(imm(0x0A), 0xFB4F);
		inx(0xFB50);
		cpx(imm(0x0A), 0xFB52);
	} while (bne(0xFB52, 0xFB46));
	txa(0xFB55);
	jsr(0xFB55, 0xF4E9);                               // eeprom_queue
	pla(0xFB59);
	tax(0xFB5A);
	rts();
}

// FB5B: the time kept in the statistics at Y (hours: 16 bits and two bits, minutes 6 bits,
// seconds 6 bits) into 0x7D + X (hours high, low), 0x7F + X (minutes) and 0x80 + X (seconds)
void time_unpack()
{
	lda(aby(0x0D6F), 0xFB5E);
	and_(imm(0x3F), 0xFB60);
	sta(wzpx(0x80), 0xFB62);
	lda(aby(0x0D6F), 0xFB65);
	sta(wzpx(0x7F), 0xFB67);
	lda(aby(0x0D6D), 0xFB6A);
	sta(wzpx(0x7D), 0xFB6C);
	lda(aby(0x0D6E), 0xFB6F);
	for (uint16_t pc = 0xFB6F; pc < 0xFB83; pc = (uint16_t)(pc + 5))
	{
		lsr(mzpx(0x7D), (uint16_t)(pc + 2));
		ror_a((uint16_t)(pc + 3));
		ror(mzpx(0x7F), (uint16_t)(pc + 5));
	}
	lsr(mzpx(0x7F), 0xFB85);
	lsr(mzpx(0x7F), 0xFB87);
	sta(wzpx(0x7E), 0xFB89);
	rts();
}

// FB8A: the time at 0x7D + Y into the statistics at X (FB5B the other way)
void time_pack()
{
	lda(aby(0x007D), 0xFB8D);
	sta(wabx(0x0D6D), 0xFB90);
	lda(aby(0x007E), 0xFB93);
	sta(wabx(0x0D6E), 0xFB96);
	lda(aby(0x007F), 0xFB99);
	asl_a(0xFB9A);
	asl_a(0xFB9B);
	for (uint16_t pc = 0xFB9B; pc < 0xFBB7; pc = (uint16_t)(pc + 7))
	{
		asl_a((uint16_t)(pc + 1));
		rol(mabx(0x0D6E), (uint16_t)(pc + 4));
		rol(mabx(0x0D6D), (uint16_t)(pc + 7));
	}
	ora(aby(0x0080), 0xFBBA);
	sta(wabx(0x0D6F), 0xFBBD);
	rts();
}

// FBBE: the time at 0x7D + X to the main CPU (0x7E, 0x7D, 0x7F, 0x80)
void send_time()
{
	static const uint8_t order[4] = {0x7E, 0x7D, 0x7F, 0x80};
	for (int i = 0; i < 4; i++)
	{
		const uint16_t pc = (uint16_t)(0xFBBE + 5 * i);
		lda(zpx(order[i]), (uint16_t)(pc + 2));
		jsr((uint16_t)(pc + 2), 0x44CD);               // send_main
	}
	rts();
}

// FBD3: clock X (seconds 0x6D + X, minutes 0x6F + X) added to the time 0x81-0x84
void time_add()
{
	lda(zp(0x84), 0xFBD5);
	clc(0xFBD6);
	adc(zpx(0x6D), 0xFBD8);
	cmp(imm(0x3C), 0xFBDA);
	if (!bcc(0xFBDA, 0xFBDF))
	{
		sbc(imm(0x3C), 0xFBDE);
		sec(0xFBDF);
	}
	sta(wzp(0x84), 0xFBE1);
	lda(zp(0x83), 0xFBE3);
	adc(zpx(0x6F), 0xFBE5);
	cmp(imm(0x3C), 0xFBE7);
	if (!bcc(0xFBE7, 0xFBEC))
	{
		sbc(imm(0x3C), 0xFBEB);
		sec(0xFBEC);
	}
	sta(wzp(0x83), 0xFBEE);
	if (!bcc(0xFBEE, 0xFBFE))
	{
		lda(zp(0x82), 0xFBF2);
		adc(imm(0x00), 0xFBF4);
		sta(wzp(0x82), 0xFBF6);
		if (!bcc(0xFBF6, 0xFBFE))
		{
			lda(zp(0x81), 0xFBFA);
			adc(imm(0x00), 0xFBFC);
			sta(wzp(0x81), 0xFBFE);
		}
	}
	rts();
}

// FC54: the play-time statistic Y + (clock A's minutes, rounded to half minutes: 0-0x1F, 0x20
// up to 23 minutes, then 0x22) one up and its record queued
void time_histogram()
{
	tax(0xFC55);
	sty(wzp(0x5E), 0xFC57);
	lda(zpx(0x6F), 0xFC59);
	sta(wzp(0x5D), 0xFC5B);
	lda(zpx(0x6D), 0xFC5D);
	cmp(imm(0x1E), 0xFC5F);
	rol(mzp(0x5D), 0xFC61);
	asl(mzp(0x5D), 0xFC63);
	lda(zp(0x5D), 0xFC65);
	cmp(imm(0x20), 0xFC67);
	if (!bcc(0xFC67, 0xFC73))
	{
		cmp(imm(0x2E), 0xFC6B);
		if (bcc(0xFC6B, 0xFC71))
			lda(imm(0x20), 0xFC73);
		else
		{
			lda(imm(0x22), 0xFC6F);
			bne(0xFC6F, 0xFC73);                       // BNE (always)
		}
	}
	clc(0xFC74);
	adc(zp(0x5E), 0xFC76);
	tay(0xFC77);
	jsr(0xFC77, 0xFB2A);                               // stat_inc
	jsr(0xFC7A, 0xFB3E);                               // stat_queue
	rts();
}

// the statistics of one clock of player X (pushed): the total at Y (in 0x81) plus the clock
// (X | add) back at Y (LDY #y at pc)
void player_clock(uint16_t pc, uint8_t y, uint8_t add)
{
	ldy(imm(y), (uint16_t)(pc + 2));
	ldx(imm(0x04), (uint16_t)(pc + 4));
	jsr((uint16_t)(pc + 4), 0xFB5B);                   // time_unpack
	pla((uint16_t)(pc + 8));
	pha((uint16_t)(pc + 9));
	if (add)
	{
		ora(imm(add), (uint16_t)(pc + 0x0B));
		pc = (uint16_t)(pc + 2);
	}
	tax((uint16_t)(pc + 0x0A));
	jsr((uint16_t)(pc + 0x0A), 0xFBD3);                // time_add
	ldx(imm(y), (uint16_t)(pc + 0x0F));
	ldy(imm(0x04), (uint16_t)(pc + 0x11));
	jsr((uint16_t)(pc + 0x11), 0xFB8A);                // time_pack
}

// FBFF: player X's clocks (X, X + 8, X + 12) added to the play times in the statistics (0x11,
// 0x18, 0x1B), and the histogram of its game time (0x1E, or 0x46 after a continue) one up
void player_times()
{
	txa(0xFC00);
	pha(0xFC01);
	player_clock(0xFC01, 0x11, 0x00);
	player_clock(0xFC15, 0x18, 0x08);
	player_clock(0xFC2B, 0x1B, 0x0C);
	pla(0xFC42);
	tax(0xFC43);
	lda(zpx(0x69), 0xFC45);
	if (beq(0xFC45, 0xFC4B))
		ldy(imm(0x1E), 0xFC4D);
	else
	{
		ldy(imm(0x46), 0xFC49);
		bne(0xFC49, 0xFC4D);                           // BNE (always)
	}
	txa(0xFC4E);
	ora(imm(0x04), 0xFC50);
	jsr(0xFC50, 0xFC54);                               // time_histogram
	rts();
}

// FB01: the end of a game: the power-on time saved (statistics 0), the game counted (one or
// two players: 0x0A / 0x0C), the players' times (FBFF) and records 1 and 2 queued
void end_of_game()
{
	ldy(imm(0x00), 0xFB03);
	ldx(imm(0x0E), 0xFB05);
	jsr(0xFB05, 0xFB8A);                               // time_pack
	ldy(imm(0x0A), 0xFB0A);
	lda(zp(0x67), 0xFB0C);
	cmp(imm(0x02), 0xFB0E);
	if (!bne(0xFB0E, 0xFB17))
	{
		ldx(imm(0x01), 0xFB12);
		jsr(0xFB12, 0xFBFF);                           // player_times
		ldy(imm(0x0C), 0xFB17);
	}
	jsr(0xFB17, 0xFB2A);                               // stat_inc
	ldx(imm(0x00), 0xFB1C);
	jsr(0xFB1C, 0xFBFF);
	lda(imm(0x01), 0xFB21);
	jsr(0xFB21, 0xF4E9);                               // eeprom_queue
	lda(imm(0x02), 0xFB26);
	jsr(0xFB26, 0xF4E9);
	rts();
}

// ---- the high scores (0x0C48 on, kept in the EEPROM at 0x1160 on)

// FC7E: A into the score tables at 0x0C48 + Y and queued for the EEPROM (0x0C20: value,
// offset); Y one on, X kept
void score_store()
{
	stx(wzp(0x61), 0xFC80);
	sta(waby(0x0C48), 0xFC83);
	ldx(zp(0x90), 0xFC85);
	sta(wabx(0x0C20), 0xFC88);
	inx(0xFC89);
	tya(0xFC8A);
	sta(wabx(0x0C20), 0xFC8D);
	inx(0xFC8E);
	cpx(imm(0x28), 0xFC90);
	if (!bcc(0xFC90, 0xFC94)) ldx(imm(0x00), 0xFC94);
	stx(wzp(0x90), 0xFC96);
	iny(0xFC97);
	ldx(zp(0x61), 0xFC99);
	rts();
}

// three letters of the table at X on (letter codes, FAC6) packed into 0x5C (the first, the
// second's top bits) and 0x5D (the second's low bits, the third) (LDA t,X at pc)
void pack_letters(uint16_t pc, uint16_t table)
{
	lda(abx(table), (uint16_t)(pc + 3));
	inx((uint16_t)(pc + 4));
	jsr((uint16_t)(pc + 4), 0xFAC6);                   // letter_code
	sta(wzp(0x5C), (uint16_t)(pc + 9));
	lda(abx(table), (uint16_t)(pc + 0x0C));
	inx((uint16_t)(pc + 0x0D));
	jsr((uint16_t)(pc + 0x0D), 0xFAC6);
	for (uint16_t a = (uint16_t)(pc + 0x11); a <= (uint16_t)(pc + 0x14); a++) asl_a(a);
	rol(mzp(0x5C), (uint16_t)(pc + 0x16));
	asl_a((uint16_t)(pc + 0x17));
	rol(mzp(0x5C), (uint16_t)(pc + 0x19));
	sta(wzp(0x5D), (uint16_t)(pc + 0x1B));
	lda(abx(table), (uint16_t)(pc + 0x1E));
	inx((uint16_t)(pc + 0x1F));
	jsr((uint16_t)(pc + 0x1F), 0xFAC6);
	ora(zp(0x5D), (uint16_t)(pc + 0x24));
	sta(wzp(0x5D), (uint16_t)(pc + 0x26));
}

// FD7F: the high score tables back to their defaults (0xFE42: 20 of three score bytes and three
// letters; 0xFEBA: 20 of a byte and three letters, its bit 0 into the first letter byte's bit
// 7), through FE1D into 0x1160 / 0x0C48; 0x0D75 / 0x0D76 = 0x07D0
void default_scores()
{
	lda(imm(0x07), 0xFD81);
	sta(wab(0x0D76), 0xFD84);
	lda(imm(0xD0), 0xFD86);
	sta(wab(0x0D75), 0xFD89);
	ldx(imm(0x00), 0xFD8B);
	ldy(imm(0x00), 0xFD8D);
	do
	{
		for (uint16_t pc = 0xFD8D; pc < 0xFDA2; pc = (uint16_t)(pc + 7))
		{
			lda(abx(0xFE42), (uint16_t)(pc + 3));
			inx((uint16_t)(pc + 4));
			jsr((uint16_t)(pc + 4), 0xFE1D);           // store_and_delay
		}
		pack_letters(0xFDA2, 0xFE42);
		lda(zp(0x5C), 0xFDCA);
		jsr(0xFDCA, 0xFE1D);
		lda(zp(0x5D), 0xFDCF);
		jsr(0xFDCF, 0xFE1D);
		cpy(imm(0x64), 0xFDD4);
	} while (bne(0xFDD4, 0xFD8D));
	ldx(imm(0x00), 0xFDD8);
	do
	{
		lda(imm(0x00), 0xFDDA);
		sta(wzp(0x5E), 0xFDDC);
		lda(abx(0xFEBA), 0xFDDF);
		lsr_a(0xFDE0);
		ror(mzp(0x5E), 0xFDE2);
		inx(0xFDE3);
		jsr(0xFDE3, 0xFE1D);
		pack_letters(0xFDE6, 0xFEBA);
		lda(zp(0x5C), 0xFE0E);
		ora(zp(0x5E), 0xFE10);
		jsr(0xFE10, 0xFE1D);
		lda(zp(0x5D), 0xFE15);
		jsr(0xFE15, 0xFE1D);
		cpy(imm(0xA0), 0xFE1A);
	} while (bne(0xFE1A, 0xFDD8));
	rts();
}

// FADE: A bytes from the main CPU: the NMI's data mode into 0x0C00 (0x04 / 0x05 the pointer,
// 0x0212 the count, 0x0213 = 0), 0xAA to the main CPU, then waiting until they have come
void receive_data()
{
	sta(wzp(0x5C), 0xFAE0);
	lda(imm(0x0C), 0xFAE2);
	sta(wab(0x0005), 0xFAE5);
	lda(imm(0x00), 0xFAE7);
	sta(wab(0x0004), 0xFAEA);
	lda(imm(0x00), 0xFAEC);
	sta(wab(0x0212), 0xFAEF);
	lda(imm(0x00), 0xFAF1);
	sta(wab(0x0213), 0xFAF4);
	lda(imm(0xAA), 0xFAF6);
	jsr(0xFAF6, 0x44CD);                               // send_main
	do
	{
		lda(ab(0x0212), 0xFAFC);
		cmp(zp(0x5C), 0xFAFE);
	} while (bne(0xFAFE, 0xFAF9));
	rts();
}

// ---- power on

// F568 (from the reset): the score tables from the EEPROM (0x1160 on into 0x0C48 on); the
// POKEYs' serial control; the statistics from the EEPROM: each slot read (F3B1), the newest
// good copy of each record into 0x0D6D (its sequence in 0x0D62); the power-on time from them
// (FB5B); the settings from the POKEYs' pot inputs (0x49 and 0x93); the game state, coin and
// counter values cleared; with the statistics never set (0x0D81 bit 7 clear and 0x0D75 / 0x0D76
// = 0), the default scores (FD7F) and record 0 queued
void system_init()
{
	ldx(imm(0xA1), 0xF56A);
	do
	{
		lda(abx(0x115F), 0xF56D);
		sta(wabx(0x0C47), 0xF570);
		dex(0xF571);
	} while (bne(0xF571, 0xF56A));
	lda(imm(0x07), 0xF575);
	sta(wab(0x180F), 0xF578);
	sta(wab(0x183F), 0xF57B);
	lda(imm(0xFF), 0xF57D);
	sta(wzp(0x89), 0xF57F);
	ldx(imm(0x0B), 0xF581);
	lda(imm(0xFF), 0xF583);
	do
	{
		sta(wabx(0x0D62), 0xF586);
		dex(0xF587);
	} while (bpl(0xF587, 0xF583));
	ldx(imm(0x6D), 0xF58B);
	lda(imm(0x00), 0xF58D);
	do
	{
		sta(wabx(0x0D6D), 0xF590);
		dex(0xF591);
	} while (bpl(0xF591, 0xF58D));
	tax(0xF594);
	sta(wzp(0x87), 0xF596);
	sta(wzp(0x88), 0xF598);
	sta(wzp(0x8A), 0xF59A);
	sta(wzp(0x8C), 0xF59C);
	do
	{
		jsr(0xF59C, 0xF3B1);                           // eeprom_read
		cmp(imm(0xFF), 0xF5A1);
		if (beq(0xF5A1, 0xF5E7)) goto lF5E7;
		sta(wzp(0x62), 0xF5A5);
		for (uint16_t pc = 0xF5A6; pc <= 0xF5A9; pc++) lsr_a(pc);
		sta(wzp(0x63), 0xF5AB);
		tay(0xF5AC);
		lda(zp(0x62), 0xF5AE);
		and_(imm(0x0F), 0xF5B0);
		sta(wzp(0x62), 0xF5B2);
		lda(aby(0x0D62), 0xF5B5);
		cmp(imm(0xFF), 0xF5B7);
		if (!beq(0xF5B7, 0xF5CA))
		{
			// newer than the copy found before (by less than 8)?
			sec(0xF5BA);
			sbc(zp(0x62), 0xF5BC);
			if (!bcs(0xF5BC, 0xF5C6))
			{
				eor(imm(0xFF), 0xF5C0);
				cmp(imm(0x08), 0xF5C2);
				if (bcs(0xF5C2, 0xF5E7)) goto lF5E7;
				bcc(0xF5C4, 0xF5CA);                   // BCC (always)
			}
			else
			{
				cmp(imm(0x08), 0xF5C8);
				if (bcc(0xF5C8, 0xF5E7)) goto lF5E7;
			}
		}
		lda(zp(0x62), 0xF5CC);
		sta(waby(0x0D62), 0xF5CF);
		stx(wzp(0x62), 0xF5D1);
		lda(zp(0x63), 0xF5D3);
		jsr(0xF5D3, 0xF488);                           // times10
		tax(0xF5D7);
		ldy(imm(0x00), 0xF5D9);
		do
		{
			lda(aby(0x0E6F), 0xF5DC);
			sta(wabx(0x0D6D), 0xF5DF);
			iny(0xF5E0);
			inx(0xF5E1);
			cpy(imm(0x0A), 0xF5E3);
		} while (bne(0xF5E3, 0xF5D9));
		ldx(zp(0x62), 0xF5E7);
	lF5E7:
		inx(0xF5E8);
		cpx(imm(0x16), 0xF5EA);
	} while (bne(0xF5EA, 0xF59C));
	ldy(imm(0x0E), 0xF5EE);
	ldx(imm(0x00), 0xF5F0);
	jsr(0xF5F0, 0xFB5B);                               // time_unpack
	lda(imm(0x00), 0xF5F5);
	sta(wab(0x180B), 0xF5F8);
	sta(wab(0x183B), 0xF5FB);
	ldx(imm(0x02), 0xF5FD);
	do
		dex(0xF5FE);
	while (bpl(0xF5FE, 0xF5FD));
	lda(ab(0x1808), 0xF603);
	sta(wzp(0x49), 0xF605);
	lda(ab(0x1838), 0xF608);
	sta(wzp(0x93), 0xF60A);
	lda(imm(0x00), 0xF60C);
	static const uint8_t cleared[7] = {0x66, 0x68, 0x67, 0x69, 0x6A, 0x90, 0x91};
	for (int i = 0; i < 7; i++) sta(wzp(cleared[i]), (uint16_t)(0xF60E + 2 * i));
	lda(imm(0x00), 0xF61C);
	ldx(imm(0x07), 0xF61E);
	do
	{
		sta(wzpx(0x94), 0xF620);
		sta(wzpx(0x9C), 0xF622);
		dex(0xF623);
	} while (bpl(0xF623, 0xF61E));
	lda(imm(0x0C), 0xF627);
	sta(wab(0x187C), 0xF62A);
	lda(imm(0x00), 0xF62C);
	sta(wzp(0x33), 0xF62E);
	ldx(imm(0x03), 0xF630);
	do
	{
		lda(abx(0x1810), 0xF633);
		sta(wzpx(0xA4), 0xF635);
		dex(0xF636);
	} while (bpl(0xF636, 0xF630));
	lda(ab(0x0D81), 0xF63B);
	if (!bmi(0xF63B, 0xF64D))
	{
		lda(ab(0x0D75), 0xF640);
		ora(ab(0x0D76), 0xF643);
		if (!bne(0xF643, 0xF64D))
		{
			jsr(0xF645, 0xFD7F);                       // default_scores
			lda(imm(0x00), 0xF64A);
			jsr(0xF64A, 0xF4E9);                       // eeprom_queue
		}
	}
	rts();
}

// ---- the main CPU's commands (table 0x5ECA, run in the NMI; each ends at 44BD)

// F703 (command 7, and after 6): the state to the main CPU: 0x66 | 0x67 (0xC0 becomes 0x80 after
// it), the settings read again from the POKEYs' pots (0x49), the credits and coin units, 0x93,
// 0x0D81
void cmd_status()
{
	lda(imm(0x07), 0xF705);
	sta(wab(0x180F), 0xF708);
	sta(wab(0x183F), 0xF70B);
	lda(zp(0x66), 0xF70D);
	ora(zp(0x67), 0xF70F);
	jsr(0xF70F, 0x44CD);                               // send_main
	lda(zp(0x66), 0xF714);
	cmp(imm(0xC0), 0xF716);
	if (!bne(0xF716, 0xF71C))
	{
		lda(imm(0x80), 0xF71A);
		sta(wzp(0x66), 0xF71C);
	}
	lda(imm(0x00), 0xF71E);
	sta(wab(0x180B), 0xF721);
	sta(wab(0x183B), 0xF724);
	ldx(imm(0x02), 0xF726);
	do
		dex(0xF727);
	while (bne(0xF727, 0xF726));
	lda(ab(0x1838), 0xF72C);
	sta(wzp(0x93), 0xF72E);
	lda(ab(0x1808), 0xF731);
	sta(wzp(0x49), 0xF733);
	jsr(0xF733, 0x44CD);
	lda(zp(0x48), 0xF738);
	asl_a(0xF739);
	asl_a(0xF73A);
	eor(zp(0x4F), 0xF73C);
	and_(imm(0xFC), 0xF73E);
	eor(zp(0x4F), 0xF740);
	jsr(0xF740, 0x44CD);
	lda(zp(0x93), 0xF745);
	jsr(0xF745, 0x44CD);
	lda(ab(0x0D81), 0xF74B);
	jsr(0xF74B, 0x44CD);
	jmp(0x44BD);                                       // nmi_done
}

// F64E (command 6): a game starts (one byte: the players, bit 7 a continue) when the credits
// 0x48 cover its price (0xF6F7 by the players and 0x93 bits 6-7; a continue 0xF6FF by 0x93
// bits 6-7): the clocks cleared, 0x68 = 1, 0x66 = 0x80 (0xC0 for a continue, which counts the
// continues (statistic 6) and the time histogram of the player's game); a game started after a
// counted coin (0x86) takes one off statistic 8 and queues record 0; then F703
void cmd_game_start()
{
	lda(imm(0x01), 0xF650);
	jsr(0xF650, 0xFADE);                               // receive_data
	lda(ab(0x0C00), 0xF656);
	if (bmi(0xF656, 0xF6B4)) goto lF6B4;
	sta(wzp(0x67), 0xF65A);
	sta(wzp(0x5D), 0xF65C);
	dec(mzp(0x5D), 0xF65E);
	lda(zp(0x93), 0xF660);
	and_(imm(0xC0), 0xF662);
	asl_a(0xF663);
	rol(mzp(0x5D), 0xF665);
	asl_a(0xF666);
	rol(mzp(0x5D), 0xF668);
	ldy(zp(0x5D), 0xF66A);
	lda(zp(0x48), 0xF66C);
	sec(0xF66D);
	sbc(aby(0xF6F7), 0xF670);
	if (!bcs(0xF670, 0xF675))
	{
		jmp_to(0xF703);
		cmd_status();
		return;
	}
	sta(wzp(0x48), 0xF677);
	lda(imm(0x00), 0xF679);
	ldx(imm(0x10), 0xF67B);
	do
	{
		sta(wzpx(0x6D), 0xF67D);
		dex(0xF67E);
	} while (bpl(0xF67E, 0xF67B));
	sta(wzp(0x69), 0xF682);
	sta(wzp(0x6A), 0xF684);
	lda(imm(0x01), 0xF686);
	sta(wzp(0x68), 0xF688);
	lda(imm(0x80), 0xF68A);
	sta(wzp(0x66), 0xF68C);
lF68C:
	lda(imm(0xFF), 0xF68E);
	sta(wzp(0x85), 0xF690);
	lda(zp(0x86), 0xF692);
	if (!beq(0xF692, 0xF6B1))
	{
		ldy(imm(0x08), 0xF696);
		lda(aby(0x0D6D), 0xF699);
		sec(0xF69A);
		sbc(imm(0x01), 0xF69C);
		sta(waby(0x0D6D), 0xF69F);
		if (!bcs(0xF69F, 0xF6AA))
		{
			iny(0xF6A2);
			lda(aby(0x0D6D), 0xF6A5);
			sbc(imm(0x00), 0xF6A7);
			sta(waby(0x0D6D), 0xF6AA);
		}
		lda(imm(0x00), 0xF6AC);
		sta(wzp(0x86), 0xF6AE);
		jsr(0xF6AE, 0xF4E9);                           // eeprom_queue
	}
lF6B1:
	jmp_to(0xF703);
	cmd_status();
	return;

lF6B4:
	// a continue
	lda(zp(0x93), 0xF6B6);
	for (uint16_t pc = 0xF6B7; pc <= 0xF6BC; pc++) lsr_a(pc);
	tay(0xF6BD);
	lda(zp(0x48), 0xF6BF);
	sec(0xF6C0);
	sbc(aby(0xF6FF), 0xF6C3);
	if (bcc(0xF6C3, 0xF6B1)) goto lF6B1;
	sta(wzp(0x48), 0xF6C7);
	lda(imm(0xC0), 0xF6C9);
	sta(wzp(0x66), 0xF6CB);
	ldy(imm(0x06), 0xF6CD);
	jsr(0xF6CD, 0xFB2A);                               // stat_inc
	lda(imm(0xFF), 0xF6D2);
	sta(wzp(0x86), 0xF6D4);
	ldx(zp(0x68), 0xF6D6);
	dex(0xF6D7);
	lda(zpx(0x69), 0xF6D9);
	if (beq(0xF6D9, 0xF6DF))
		ldy(imm(0x1E), 0xF6E1);
	else
	{
		ldy(imm(0x46), 0xF6DD);
		bne(0xF6DD, 0xF6E1);                           // BNE (always)
	}
	txa(0xF6E2);
	pha(0xF6E3);
	ora(imm(0x04), 0xF6E5);
	jsr(0xF6E5, 0xFC54);                               // time_histogram
	lda(imm(0x00), 0xF6EA);
	sta(wzpx(0x6D), 0xF6EC);
	sta(wzpx(0x6F), 0xF6EE);
	pla(0xF6EF);
	tax(0xF6F0);
	lda(imm(0xFF), 0xF6F2);
	sta(wzpx(0x69), 0xF6F4);
	jmp_to(0xF68C);
	goto lF68C;
}

// F751 (command 0x17): one byte into 0x0D81 (record 2), queued for the EEPROM
void cmd_set_option()
{
	lda(imm(0x01), 0xF753);
	jsr(0xF753, 0xFADE);                               // receive_data
	lda(ab(0x0C00), 0xF759);
	sta(wab(0x0D81), 0xF75C);
	lda(imm(0x02), 0xF75E);
	jsr(0xF75E, 0xF4E9);                               // eeprom_queue
	jmp(0x44BD);                                       // nmi_done
}

// F764 (command 0x13): the counters' moves since last time (0x94 / 0x98 less 0x9C / 0xA0, the
// second then the first) to the main CPU
void cmd_counter_moves()
{
	ldx(imm(0x01), 0xF766);
	do
	{
		sec(0xF767);
		lda(zpx(0x94), 0xF769);
		sbc(zpx(0x9C), 0xF76B);
		php(0xF76C);
		jsr(0xF76C, 0x44CD);                           // send_main
		plp(0xF770);
		lda(zpx(0x98), 0xF772);
		sbc(zpx(0xA0), 0xF774);
		jsr(0xF774, 0x44CD);
		lda(zpx(0x94), 0xF779);
		sta(wzpx(0x9C), 0xF77B);
		lda(zpx(0x98), 0xF77D);
		sta(wzpx(0xA0), 0xF77F);
		dex(0xF780);
	} while (bpl(0xF780, 0xF766));
	jmp(0x44BD);                                       // nmi_done
}

// F785 (command 0x15): the mixer 0x187C from one byte (bits 2-4, the rest from 0x33, bits 2-3
// inverted)
void cmd_mixer()
{
	lda(imm(0x01), 0xF787);
	jsr(0xF787, 0xFADE);                               // receive_data
	lda(ab(0x0C00), 0xF78D);
	eor(zp(0x33), 0xF78F);
	and_(imm(0x1C), 0xF791);
	eor(zp(0x33), 0xF793);
	eor(imm(0x0C), 0xF795);
	sta(wab(0x187C), 0xF798);
	jmp(0x44BD);                                       // nmi_done
}

// F79B (command 0x0A): a game over (two bytes): its statistics (FB01), 0x66-0x68 cleared
void cmd_game_over()
{
	lda(imm(0x02), 0xF79D);
	jsr(0xF79D, 0xFADE);                               // receive_data
	jsr(0xF7A0, 0xFB01);                               // end_of_game
	lda(imm(0x00), 0xF7A5);
	sta(wzp(0x66), 0xF7A7);
	sta(wzp(0x67), 0xF7A9);
	sta(wzp(0x68), 0xF7AB);
	jmp(0x44BD);                                       // nmi_done
}

// F7AE (command 9): 0x26 to the main CPU
void cmd_send_26()
{
	lda(zp(0x26), 0xF7B0);
	jsr(0xF7B0, 0x44CD);                               // send_main
	jmp(0x44BD);                                       // nmi_done
}

// F7B6 (command 0x14): two of the POKEY's random numbers (0x180A) to the main CPU
void cmd_random()
{
	ldx(imm(0x01), 0xF7B8);
	do
	{
		lda(ab(0x180A), 0xF7BB);
		jsr(0xF7BB, 0x44CD);                           // send_main
		dex(0xF7BF);
	} while (bpl(0xF7BF, 0xF7B8));
	jmp(0x44BD);                                       // nmi_done
}

// F7C4 (command 0x16): statistic 0x42 (byte 0) or 0x44 (otherwise) one up and queued
void cmd_count()
{
	lda(imm(0x01), 0xF7C6);
	jsr(0xF7C6, 0xFADE);                               // receive_data
	lda(ab(0x0C00), 0xF7CC);
	if (!beq(0xF7CC, 0xF7D0)) lda(imm(0x02), 0xF7D0);
	clc(0xF7D1);
	adc(imm(0x42), 0xF7D3);
	tay(0xF7D4);
	jsr(0xF7D4, 0xFB2A);                               // stat_inc
	jsr(0xF7D7, 0xFB3E);                               // stat_queue
	jmp(0x44BD);                                       // nmi_done
}

// 0x68 (the players' clocks that run) from byte 0 (bits 0-1, at least 1) (LDA $0C00 at F816)
void set_players()
{
	lda(ab(0x0C00), 0xF819);
	and_(imm(0x03), 0xF81B);
	if (!bne(0xF81B, 0xF81F)) lda(imm(0x01), 0xF81F);
	sta(wzp(0x68), 0xF821);
	jmp(0x44BD);                                       // nmi_done
}

// F811 (command 8): the players' clocks (one byte)
void cmd_players()
{
	lda(imm(0x01), 0xF813);
	jsr(0xF813, 0xFADE);                               // receive_data
	set_players();
}

// F7DD (command 0x0F): three bytes: statistic 0x16 one up and record 2 queued; byte 1 adds
// credits (bits 2-7) and coin units (bits 0-1), byte 2 sets 0x67 (bits 0-1) and 0x66 (bits
// 6-7), byte 0 the players' clocks
void cmd_resume()
{
	lda(imm(0x03), 0xF7DF);
	jsr(0xF7DF, 0xFADE);                               // receive_data
	ldy(imm(0x16), 0xF7E4);
	jsr(0xF7E4, 0xFB2A);                               // stat_inc
	lda(imm(0x02), 0xF7E9);
	jsr(0xF7E9, 0xF4E9);                               // eeprom_queue
	lda(ab(0x0C01), 0xF7EF);
	lsr_a(0xF7F0);
	lsr_a(0xF7F1);
	clc(0xF7F2);
	adc(zp(0x48), 0xF7F4);
	sta(wzp(0x48), 0xF7F6);
	lda(ab(0x0C01), 0xF7F9);
	and_(imm(0x03), 0xF7FB);
	clc(0xF7FC);
	adc(zp(0x4F), 0xF7FE);
	sta(wzp(0x4F), 0xF800);
	lda(ab(0x0C02), 0xF803);
	and_(imm(0x03), 0xF805);
	sta(wzp(0x67), 0xF807);
	lda(ab(0x0C02), 0xF80A);
	and_(imm(0xC0), 0xF80C);
	sta(wzp(0x66), 0xF80E);
	jmp_to(0xF816);
	set_players();
}

// F824 (command 0x0E): the byte at the address in two bytes to the main CPU
void cmd_peek()
{
	lda(imm(0x02), 0xF826);
	jsr(0xF826, 0xFADE);                               // receive_data
	lda(ab(0x0C00), 0xF82C);
	sta(wzp(0x5C), 0xF82E);
	lda(ab(0x0C01), 0xF831);
	sta(wzp(0x5D), 0xF833);
	ldy(imm(0x00), 0xF835);
	lda(iy(0x5C), 0xF837);
	jsr(0xF837, 0x44CD);                               // send_main
	jmp(0x44BD);                                       // nmi_done
}

// a pass over the score entries at base (size bytes apart, compared on their first n bytes)
// from entry Y down to entry 0x5F: 0x5C = the offset of the highest (else the lowest) of them
// and the one at 0x5C before (LDX $5C at pc; the end tested with CMP $5F or CMP #0)
void best_entry(uint16_t pc, uint16_t base, int n, uint8_t size, bool highest, bool cmp_zp)
{
	const uint16_t c = (uint16_t)(pc + 4 + 0x0A * (n - 1) + 6);
	do
	{
		ldx(zp(0x5C), (uint16_t)(pc + 2));
		sty(wzp(0x5D), (uint16_t)(pc + 4));
		uint16_t a = (uint16_t)(pc + 4);
		for (int k = 0;; k++)
		{
			lda(highest ? abx(base) : aby(base), (uint16_t)(a + 3));
			cmp(highest ? aby(base) : abx(base), (uint16_t)(a + 6));
			if (k == n - 1 || bne((uint16_t)(a + 6), c)) break;
			inx((uint16_t)(a + 9));
			iny((uint16_t)(a + 0x0A));
			a = (uint16_t)(a + 0x0A);
		}
		if (!bcs(c, (uint16_t)(c + 6)))
		{
			lda(zp(0x5D), (uint16_t)(c + 4));
			sta(wzp(0x5C), (uint16_t)(c + 6));
		}
		lda(zp(0x5D), (uint16_t)(c + 8));
		sec((uint16_t)(c + 9));
		sbc(imm(size), (uint16_t)(c + 0x0B));
		tay((uint16_t)(c + 0x0C));
		if (cmp_zp)
			cmp(zp(0x5F), (uint16_t)(c + 0x0E));
		else
			cmp(imm(0x00), (uint16_t)(c + 0x0E));
	} while (bne((uint16_t)(c + 0x0E), pc));
}

// with 0x0D76 = 0 and 0x0D75 at most 0xC8: 0x0D75 = 0xC8 and record 1 queued (LDA $0D76 at pc)
void high_score_seen(uint16_t pc, uint16_t skip)
{
	lda(ab(0x0D76), (uint16_t)(pc + 3));
	if (bne((uint16_t)(pc + 3), skip)) return;
	lda(imm(0xC8), (uint16_t)(pc + 7));
	cmp(ab(0x0D75), (uint16_t)(pc + 0x0A));
	if (bcc((uint16_t)(pc + 0x0A), skip)) return;
	sta(wab(0x0D75), (uint16_t)(pc + 0x0F));
	lda(imm(0x01), (uint16_t)(pc + 0x11));
	jsr((uint16_t)(pc + 0x11), 0xF4E9);                // eeprom_queue
}

// F83D (command 0x0B): a score of the first table (six bytes: three of score, three letters)
// takes the place of the table's lowest (20 entries of 5 bytes at 0x0C48) if not below it
void cmd_high_score()
{
	lda(imm(0x06), 0xF83F);
	jsr(0xF83F, 0xFADE);                               // receive_data
	lda(imm(0x00), 0xF844);
	sta(wzp(0x5C), 0xF846);
	ldy(imm(0x5F), 0xF848);
	best_entry(0xF848, 0x0C48, 3, 5, false, false);
	ldx(zp(0x5C), 0xF878);
	lda(ab(0x0C00), 0xF87B);
	cmp(abx(0x0C48), 0xF87E);
	if (!bne(0xF87E, 0xF890))
	{
		inx(0xF881);
		lda(ab(0x0C01), 0xF884);
		cmp(abx(0x0C48), 0xF887);
		if (!bne(0xF887, 0xF890))
		{
			inx(0xF88A);
			lda(ab(0x0C02), 0xF88D);
			cmp(abx(0x0C48), 0xF890);
		}
	}
	if (!bcc(0xF890, 0xF8DE))
	{
		high_score_seen(0xF892, 0xF8A6);
		// the letters packed into two bytes
		lda(ab(0x0C03), 0xF8A9);
		jsr(0xF8A9, 0xFAC6);                           // letter_code
		sta(wab(0x0C03), 0xF8AF);
		lda(ab(0x0C04), 0xF8B2);
		jsr(0xF8B2, 0xFAC6);
		for (uint16_t pc = 0xF8B6; pc <= 0xF8B9; pc++) asl_a(pc);
		rol(mab(0x0C03), 0xF8BC);
		asl_a(0xF8BD);
		rol(mab(0x0C03), 0xF8C0);
		sta(wab(0x0C04), 0xF8C3);
		lda(ab(0x0C05), 0xF8C6);
		jsr(0xF8C6, 0xFAC6);
		ora(ab(0x0C04), 0xF8CC);
		sta(wab(0x0C04), 0xF8CF);
		ldx(imm(0x00), 0xF8D1);
		ldy(zp(0x5C), 0xF8D3);
		do
		{
			lda(abx(0x0C00), 0xF8D6);
			jsr(0xF8D6, 0xFC7E);                       // score_store
			inx(0xF8DA);
			cpx(imm(0x05), 0xF8DC);
		} while (bne(0xF8DC, 0xF8D3));
	}
	jmp(0x44BD);                                       // nmi_done
}

// F8E1 (command 0x0C): an entry of the second table (five bytes: a 16-bit value, shifted down
// one, and three letters) takes the place of the table's lowest (20 entries of 3 bytes at
// 0x0CAC) if not below it
void cmd_high_score2()
{
	lda(imm(0x05), 0xF8E3);
	jsr(0xF8E3, 0xFADE);                               // receive_data
	lsr(mab(0x0C01), 0xF8E9);
	ror(mab(0x0C00), 0xF8EC);
	ror(mab(0x0C01), 0xF8EF);
	lda(imm(0x00), 0xF8F1);
	sta(wzp(0x5C), 0xF8F3);
	ldy(imm(0x39), 0xF8F5);
	best_entry(0xF8F5, 0x0CAC, 2, 3, false, false);
	ldx(zp(0x5C), 0xF91B);
	lda(ab(0x0C00), 0xF91E);
	cmp(abx(0x0CAC), 0xF921);
	if (!bne(0xF921, 0xF92C))
	{
		inx(0xF924);
		lda(ab(0x0C01), 0xF927);
		ora(imm(0x7F), 0xF929);
		cmp(abx(0x0CAC), 0xF92C);
	}
	if (!bcc(0xF92C, 0xF98B))
	{
		high_score_seen(0xF92E, 0xF942);
		lda(ab(0x0C01), 0xF945);
		and_(imm(0x80), 0xF947);
		lsr_a(0xF948);
		lsr_a(0xF949);
		sta(wab(0x0C01), 0xF94C);
		lda(ab(0x0C02), 0xF94F);
		jsr(0xF94F, 0xFAC6);                           // letter_code
		ora(ab(0x0C01), 0xF955);
		sta(wab(0x0C01), 0xF958);
		lda(ab(0x0C03), 0xF95B);
		jsr(0xF95B, 0xFAC6);
		for (uint16_t pc = 0xF95F; pc <= 0xF962; pc++) asl_a(pc);
		rol(mab(0x0C01), 0xF965);
		asl_a(0xF966);
		rol(mab(0x0C01), 0xF969);
		sta(wab(0x0C02), 0xF96C);
		lda(ab(0x0C04), 0xF96F);
		jsr(0xF96F, 0xFAC6);
		ora(ab(0x0C02), 0xF975);
		sta(wab(0x0C02), 0xF978);
		ldx(imm(0x00), 0xF97A);
		lda(zp(0x5C), 0xF97C);
		clc(0xF97D);
		adc(imm(0x64), 0xF97F);
		tay(0xF980);
		do
		{
			lda(abx(0x0C00), 0xF983);
			jsr(0xF983, 0xFC7E);                       // score_store
			inx(0xF987);
			cpx(imm(0x03), 0xF989);
		} while (bne(0xF989, 0xF980));
	}
	jmp(0x44BD);                                       // nmi_done
}

// the letters of an entry at 0x0C00 back from their codes (FAD2): the code bytes at high /
// low (high bits 0-1 and low bits 5-7 the second) into the bytes at out + 2, out + 1 and out
// (LDA low at pc)
void unpack_letters(uint16_t pc, uint16_t low, uint16_t high, uint16_t out)
{
	lda(ab(low), (uint16_t)(pc + 3));
	jsr((uint16_t)(pc + 3), 0xFAD2);                   // code_letter
	sta(wab((uint16_t)(out + 2)), (uint16_t)(pc + 9));
	lda(ab(low), (uint16_t)(pc + 0x0C));
	lsr(mab(high), (uint16_t)(pc + 0x0F));
	ror_a((uint16_t)(pc + 0x10));
	lsr(mab(high), (uint16_t)(pc + 0x13));
	ror_a((uint16_t)(pc + 0x14));
	for (uint16_t a = (uint16_t)(pc + 0x15); a <= (uint16_t)(pc + 0x17); a++) lsr_a(a);
	jsr((uint16_t)(pc + 0x17), 0xFAD2);
	sta(wab((uint16_t)(out + 1)), (uint16_t)(pc + 0x1D));
	lda(ab(high), (uint16_t)(pc + 0x20));
	jsr((uint16_t)(pc + 0x20), 0xFAD2);
	sta(wab(out), (uint16_t)(pc + 0x26));
}

// F98E (command 0x0D): both score tables to the main CPU, highest first, from a copy at 0x0CE8:
// the first's 20 entries as three score bytes and three letters, the second's as two bytes and
// three letters
void cmd_send_scores()
{
	ldy(imm(0x64), 0xF990);
	do
	{
		lda(aby(0x0C47), 0xF993);
		sta(waby(0x0CE7), 0xF996);
		dey(0xF997);
	} while (bne(0xF997, 0xF990));
	lda(imm(0x00), 0xF99B);
	sta(wzp(0x5F), 0xF99D);
	lda(imm(0x14), 0xF99F);
	sta(wzp(0x5E), 0xF9A1);
	for (;;)
	{
		lda(zp(0x5F), 0xF9A3);
		sta(wzp(0x5C), 0xF9A5);
		clc(0xF9A6);
		adc(imm(0x5F), 0xF9A8);
		tay(0xF9A9);
		best_entry(0xF9A9, 0x0CE8, 3, 5, true, true);
		ldy(zp(0x5C), 0xF9D9);
		ldx(imm(0x00), 0xF9DB);
		do
		{
			lda(aby(0x0CE8), 0xF9DE);
			sta(wabx(0x0C00), 0xF9E1);
			lda(imm(0x00), 0xF9E3);
			sta(waby(0x0CE8), 0xF9E6);
			iny(0xF9E7);
			inx(0xF9E8);
			cpx(imm(0x05), 0xF9EA);
		} while (bne(0xF9EA, 0xF9DB));
		unpack_letters(0xF9EC, 0x0C04, 0x0C03, 0x0C03);
		ldx(imm(0x00), 0xFA14);
		do
		{
			lda(abx(0x0C00), 0xFA17);
			jsr(0xFA17, 0x44CD);                       // send_main
			inx(0xFA1B);
			cpx(imm(0x06), 0xFA1D);
		} while (bne(0xFA1D, 0xFA14));
		dec(mzp(0x5E), 0xFA21);
		if (beq(0xFA21, 0xFA26)) break;
		jmp_to(0xF9A1);
	}
	ldy(imm(0x3C), 0xFA28);
	do
	{
		lda(aby(0x0CAB), 0xFA2B);
		sta(waby(0x0CE7), 0xFA2E);
		dey(0xFA2F);
	} while (bne(0xFA2F, 0xFA28));
	lda(imm(0x00), 0xFA33);
	sta(wzp(0x5F), 0xFA35);
	lda(imm(0x14), 0xFA37);
	sta(wzp(0x5E), 0xFA39);
	for (;;)
	{
		lda(zp(0x5F), 0xFA3B);
		sta(wzp(0x5C), 0xFA3D);
		clc(0xFA3E);
		adc(imm(0x39), 0xFA40);
		tay(0xFA41);
		best_entry(0xFA41, 0x0CE8, 2, 3, true, true);
		ldy(zp(0x5C), 0xFA67);
		ldx(imm(0x00), 0xFA69);
		do
		{
			lda(aby(0x0CE8), 0xFA6C);
			sta(wabx(0x0C00), 0xFA6F);
			lda(imm(0x00), 0xFA71);
			sta(waby(0x0CE8), 0xFA74);
			iny(0xFA75);
			inx(0xFA76);
			cpx(imm(0x03), 0xFA78);
		} while (bne(0xFA78, 0xFA69));
		unpack_letters(0xFA7A, 0x0C02, 0x0C01, 0x0C02);
		lda(ab(0x0C01), 0xFAA3);
		asl_a(0xFAA4);
		asl_a(0xFAA5);
		and_(imm(0x80), 0xFAA7);
		asl_a(0xFAA8);
		rol(mab(0x0C00), 0xFAAB);
		rol_a(0xFAAC);
		sta(wab(0x0C01), 0xFAAF);
		ldx(imm(0x00), 0xFAB1);
		do
		{
			lda(abx(0x0C00), 0xFAB4);
			jsr(0xFAB4, 0x44CD);                       // send_main
			inx(0xFAB8);
			cpx(imm(0x05), 0xFABA);
		} while (bne(0xFABA, 0xFAB1));
		dec(mzp(0x5E), 0xFABE);
		if (beq(0xFABE, 0xFAC3)) break;
		jmp_to(0xFA39);
	}
	jmp(0x44BD);                                       // nmi_done
}

// FC9A (command 0x10): the play times to the main CPU: the statistics 2-7 and 0x0A-0x0D, the
// times at 0x11 (then the power-on time), 0x18 and 0x1B
void cmd_send_times()
{
	ldy(imm(0x00), 0xFC9C);
	do
	{
		lda(aby(0x0D6F), 0xFC9F);
		jsr(0xFC9F, 0x44CD);                           // send_main
		iny(0xFCA3);
		cpy(imm(0x06), 0xFCA5);
	} while (bne(0xFCA5, 0xFC9C));
	ldy(imm(0x00), 0xFCA9);
	do
	{
		lda(aby(0x0D77), 0xFCAC);
		jsr(0xFCAC, 0x44CD);
		iny(0xFCB0);
		cpy(imm(0x04), 0xFCB2);
	} while (bne(0xFCB2, 0xFCA9));
	ldy(imm(0x11), 0xFCB6);
	ldx(imm(0x04), 0xFCB8);
	jsr(0xFCB8, 0xFB5B);                               // time_unpack
	jsr(0xFCBB, 0xFBBE);                               // send_time
	ldx(imm(0x00), 0xFCC0);
	jsr(0xFCC0, 0xFBBE);
	ldy(imm(0x18), 0xFCC5);
	ldx(imm(0x04), 0xFCC7);
	jsr(0xFCC7, 0xFB5B);
	jsr(0xFCCA, 0xFBBE);
	ldy(imm(0x1B), 0xFCCF);
	ldx(imm(0x04), 0xFCD1);
	jsr(0xFCD1, 0xFB5B);
	jsr(0xFCD4, 0xFBBE);
	jmp(0x44BD);                                       // nmi_done
}

// FCDA (command 0x11): the statistics from 0x14 on (0x0D81-0x0DDA) to the main CPU
void cmd_send_stats()
{
	ldy(imm(0x14), 0xFCDC);
	do
	{
		lda(aby(0x0D6D), 0xFCDF);
		jsr(0xFCDF, 0x44CD);                           // send_main
		iny(0xFCE3);
		cpy(imm(0x6E), 0xFCE5);
	} while (bne(0xFCE5, 0xFCDC));
	jmp(0x44BD);                                       // nmi_done
}

// FCEA (command 0x12): one byte: bit 0 the default scores (FD7F); bits 1-2 the statistics
// cleared (all but 8-9 and 0x14) and written to the EEPROM with every slot's sequence started
// again, the slots past the records erased
void cmd_clear()
{
	lda(imm(0x01), 0xFCEC);
	jsr(0xFCEC, 0xFADE);                               // receive_data
	lsr(mab(0x0C00), 0xFCF2);
	if (!bcc(0xFCF2, 0xFCF7)) jsr(0xFCF4, 0xFD7F);    // default_scores
	lda(ab(0x0C00), 0xFCFA);
	and_(imm(0x03), 0xFCFC);
	if (!beq(0xFCFC, 0xFD7C))
	{
		ldx(imm(0x00), 0xFD00);
		txa(0xFD01);
		do
		{
			sta(wabx(0x0D6D), 0xFD04);
			sta(wabx(0x0DF7), 0xFD07);
			inx(0xFD08);
			cpx(imm(0x08), 0xFD0A);
		} while (bne(0xFD0A, 0xFD01));
		lda(abx(0x0D6D), 0xFD0F);
		sta(wabx(0x0DF7), 0xFD12);
		inx(0xFD13);
		lda(abx(0x0D6D), 0xFD16);
		sta(wabx(0x0DF7), 0xFD19);
		inx(0xFD1A);
		lda(imm(0x00), 0xFD1C);
		do
		{
			sta(wabx(0x0D6D), 0xFD1F);
			sta(wabx(0x0DF7), 0xFD22);
			inx(0xFD23);
			cpx(imm(0x14), 0xFD25);
		} while (bne(0xFD25, 0xFD1C));
		lda(abx(0x0D6D), 0xFD2A);
		sta(wabx(0x0DF7), 0xFD2D);
		inx(0xFD2E);
		lda(imm(0x00), 0xFD30);
		do
		{
			sta(wabx(0x0D6D), 0xFD33);
			sta(wabx(0x0DF7), 0xFD36);
			inx(0xFD37);
			cpx(imm(0x6E), 0xFD39);
		} while (bne(0xFD39, 0xFD30));
		ldx(imm(0x00), 0xFD3D);
		do
		{
			lda(imm(0xFF), 0xFD3F);
			sta(wabx(0x0D62), 0xFD42);
			stx(wzp(0x8F), 0xFD44);
			jsr(0xFD44, 0xF303);                       // eeprom_encode
			lda(zp(0x8F), 0xFD49);
			jsr(0xFD49, 0xF490);                       // ptr_1000
			ldx(zp(0x8F), 0xFD4E);
			ldy(imm(0x00), 0xFD50);
			do
			{
				lda(aby(0x0DDB), 0xFD53);
				sta(wiy(0x8D), 0xFD55);
				jsr(0xFD55, 0xFE28);                   // delay
				iny(0xFD59);
				cpy(imm(0x10), 0xFD5B);
			} while (bne(0xFD5B, 0xFD50));
			inx(0xFD5E);
			cpx(imm(0x0B), 0xFD60);
		} while (bne(0xFD60, 0xFD3D));
		do
		{
			txa(0xFD63);
			pha(0xFD64);
			jsr(0xFD64, 0xF490);                       // ptr_1000
			pla(0xFD68);
			tax(0xFD69);
			ldy(imm(0x00), 0xFD6B);
			do
			{
				lda(imm(0xFF), 0xFD6D);
				sta(wiy(0x8D), 0xFD6F);
				jsr(0xFD6F, 0xFE28);                   // delay
				iny(0xFD73);
				cpy(imm(0x10), 0xFD75);
			} while (bne(0xFD75, 0xFD6B));
			inx(0xFD78);
			cpx(imm(0x16), 0xFD7A);
		} while (bne(0xFD7A, 0xFD62));
	}
	jmp(0x44BD);                                       // nmi_done
}

// F14D (from the IRQ): the clocks, the coins, the EEPROM and the counter inputs; carry clear
void irq_system()
{
	jsr(0xF14D, 0xF17F);                               // play_clocks
	jsr(0xF150, 0xF15B);                               // coins
	jsr(0xF153, 0xF208);                               // eeprom_writer
	jsr(0xF156, 0xF53B);                               // counter_inputs
	clc(0xF15A);
	rts();
}

} // namespace

SOUND_TABLE(system,
	routine(0xF00E, "coin_inputs", coin_inputs),
	routine(0xF14D, "irq_system", irq_system),
	routine(0xF15B, "coins", coins),
	routine(0xF17F, "play_clocks", play_clocks),
	routine(0xF1F4, "clock_tick", clock_tick),
	routine(0xF208, "eeprom_writer", eeprom_writer),
	routine(0xF233, "eeprom_write_step", eeprom_write_step),
	routine(0xF303, "eeprom_encode", eeprom_encode),
	routine(0xF376, "eeprom_parity", eeprom_parity),
	routine(0xF3B1, "eeprom_read", eeprom_read),
	routine(0xF4A8, "eeprom_oldest", eeprom_oldest),
	routine(0xF4E9, "eeprom_queue", eeprom_queue),
	routine(0xF53B, "counter_inputs", counter_inputs),
	routine(0xF568, "system_init", system_init),
	routine(0xF64E, "cmd_game_start", cmd_game_start, NMI_END),
	routine(0xF703, "cmd_status", cmd_status, NMI_END),
	routine(0xF751, "cmd_set_option", cmd_set_option, NMI_END),
	routine(0xF764, "cmd_counter_moves", cmd_counter_moves, NMI_END),
	routine(0xF785, "cmd_mixer", cmd_mixer, NMI_END),
	routine(0xF79B, "cmd_game_over", cmd_game_over, NMI_END),
	routine(0xF7AE, "cmd_send_26", cmd_send_26, NMI_END),
	routine(0xF7B6, "cmd_random", cmd_random, NMI_END),
	routine(0xF7C4, "cmd_count", cmd_count, NMI_END),
	routine(0xF7DD, "cmd_resume", cmd_resume, NMI_END),
	routine(0xF811, "cmd_players", cmd_players, NMI_END),
	routine(0xF824, "cmd_peek", cmd_peek, NMI_END),
	routine(0xF83D, "cmd_high_score", cmd_high_score, NMI_END),
	routine(0xF8E1, "cmd_high_score2", cmd_high_score2, NMI_END),
	routine(0xF98E, "cmd_send_scores", cmd_send_scores, NMI_END),
	routine(0xFAC6, "letter_code", letter_code),
	routine(0xFAD2, "code_letter", code_letter),
	routine(0xFADE, "receive_data", receive_data),
	routine(0xFB01, "end_of_game", end_of_game),
	routine(0xFB2A, "stat_inc", stat_inc),
	routine(0xFB3E, "stat_queue", stat_queue),
	routine(0xFB5B, "time_unpack", time_unpack),
	routine(0xFB8A, "time_pack", time_pack),
	routine(0xFBBE, "send_time", send_time),
	routine(0xFBD3, "time_add", time_add),
	routine(0xFBFF, "player_times", player_times),
	routine(0xFC54, "time_histogram", time_histogram),
	routine(0xFC7E, "score_store", score_store),
	routine(0xFC9A, "cmd_send_times", cmd_send_times, NMI_END),
	routine(0xFCDA, "cmd_send_stats", cmd_send_stats, NMI_END),
	routine(0xFCEA, "cmd_clear", cmd_clear, NMI_END),
	routine(0xFD7F, "default_scores", default_scores),
)

} // namespace sound
