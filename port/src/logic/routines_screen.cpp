// Rewritten routines: text layer, video memory and the display interrupts.  They follow the
// original instruction by instruction: each read and write happens where the original's does
// and after every instruction its cycles are charged and interrupts may come in (tick at the
// next instruction's address; see routines_core.cpp).  The final RTI of the interrupt
// handler is charged in the table.
#include "logic.h"

namespace logic {
namespace steps {                                      // defined in routines_core.cpp
void rts();
void jsr(uint16_t ret, uint16_t addr);
void c_save(int n, uint16_t body);
void c_restore(int n);
}

namespace {

using namespace t11rt;
using namespace steps;


// F:8B52 (character code in R0): draw a character of the large font (2 x 2 tiles) at the text
// cursor 0x0186 with the colour bits 0x0188 and advance it; 0 only advances.  Digits are
// drawn as by F:8C56 (the original branches into its body), ',' is two tiles, other codes use
// the 4-byte tile groups at 0x4000: symbols through the table at 0x8C86 ('-' and '=' in the
// second palette), letters from 0x41 on in order.  R1-R3 are preserved (register-save helper
// F144).  Text is drawn while the interrupts come and go (the play interrupt draws with the
// same cursor and switches the video bank), so this follows the original step by step: the
// registers, the stack and every write are as the original's at each instruction, and
// interrupts are let in after every instruction (tick).
void draw_large_char()
{
	fl::tst(T.r[0]);
	tick(0x8B54, 12);                                  // TST R0
	if (T.r[0] == 0)
	{
		tick(0x8B56, 12);                              // BNE
		poke(0x0186, fl::add(peek(0x0186), 4));
		tick(0x8B5C, 33);                              // ADD #4,@#0x0186
		rts();
		return;
	}
	tick(0x8B5E, 12);                                  // BNE
	// JSR R1,@#F144: R1, R2, R3 go on the stack, the epilogue's address F150 below them
	const uint16_t saved_r1 = T.r[1];
	push(T.r[1]);
	T.r[1] = 0x8B62;
	tick(0xF144, 30);
	push(T.r[2]);
	fl::mov(T.r[2]);
	tick(0xF146, 24);
	push(T.r[3]);
	fl::mov(T.r[3]);
	tick(0xF148, 24);
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xF14A, 24);
	T.r[1] = saved_r1;
	fl::mov(T.r[1]);
	tick(0xF14E, 27);
	ww(T.r[6], 0xF150);                                // JSR PC,@(SP)+
	tick(0x8B62, 30);

	const uint16_t c = T.r[0] & 0x007F;
	T.r[0] = c;
	fl::logic(T.r[0]);
	tick(0x8B66, 18);                                  // BIC #0xFF80,R0
	T.r[3] = peek(0x0188);
	fl::mov(T.r[3]);
	tick(0x8B6A, 24);                                  // MOV @#0x0188,R3
	fl::cmp(T.r[0], 0x0030);
	tick(0x8B6E, 18);                                  // CMP R0,#0x30
	bool carry = false;                                // C as the epilogue leaves it
	if (c >= 0x30)
	{
		tick(0x8B70, 12);                              // BLT
		fl::cmp(T.r[0], 0x0039);
		tick(0x8B74, 18);                              // CMP R0,#0x39
		if (c <= 0x39)
		{
			tick(0x8C5A, 12);                          // BLE into the body of F:8C56
			T.r[0] = (uint16_t)(c - 0x30);
			fl::sub((uint16_t)(T.r[0] + 0x0030), 0x0030);
			tick(0x8C5E, 18);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x8C60, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0x8C62, 12);
			T.r[0] = fl::logic((uint16_t)(T.r[0] | peek(0x0188)));
			tick(0x8C66, 24);
			T.r[1] = peek(0x0186);
			fl::mov(T.r[1]);
			tick(0x8C6A, 24);
			T.r[0] = fl::add(T.r[0], peek(0x018A));
			tick(0x8C6E, 24);
			ww(T.r[1], T.r[0]);
			T.r[1] += 2;
			fl::mov(T.r[0]);
			tick(0x8C70, 21);
			T.r[0] = fl::inc(T.r[0]);
			tick(0x8C72, 12);
			ww(T.r[1], T.r[0]);
			T.r[1] += 2;
			fl::mov(T.r[0]);
			tick(0x8C74, 21);
			poke(0x0186, T.r[1]);
			fl::mov(T.r[1]);
			tick(0x8C78, 27);
			carry = T.r[1] + 0x007C > 0xFFFF;
			T.r[1] = fl::add(T.r[1], 0x007C);
			tick(0x8C7C, 18);
			T.r[0] = fl::inc(T.r[0]);
			tick(0x8C7E, 12);
			ww(T.r[1], T.r[0]);
			T.r[1] += 2;
			fl::mov(T.r[0]);
			tick(0x8C80, 21);
			T.r[0] = fl::inc(T.r[0]);
			tick(0x8C82, 12);
			ww(T.r[1], T.r[0]);
			fl::mov(T.r[0]);
			tick(0x8C84, 21);
			goto epilogue;
		}
	}
	tick(0x8B76, 12);                                  // BLT / BLE
	fl::cmp(T.r[0], 0x002C);
	tick(0x8B7A, 18);                                  // CMP R0,#0x2C
	if (c == 0x2C)
	{
		tick(0x8B7C, 12);                              // BNE
		T.r[1] = peek(0x0186);
		fl::mov(T.r[1]);
		tick(0x8B80, 24);
		T.r[2] = 0x0087;
		fl::mov(T.r[2]);
		tick(0x8B84, 18);
		T.r[2] = fl::logic((uint16_t)(T.r[2] | T.r[3]));
		tick(0x8B86, 12);
		ww(T.r[1], T.r[2]);
		T.r[1] += 2;
		fl::mov(T.r[2]);
		tick(0x8B88, 21);
		poke(0x0186, T.r[1]);
		fl::mov(T.r[1]);
		tick(0x8B8C, 27);
		carry = T.r[1] + 0x007E > 0xFFFF;
		T.r[1] = fl::add(T.r[1], 0x007E);
		tick(0x8B90, 18);
		T.r[2] = 0x007D;
		fl::mov(T.r[2]);
		tick(0x8B94, 18);
		T.r[2] = fl::logic((uint16_t)(T.r[2] | T.r[3]));
		tick(0x8B96, 12);
		ww(T.r[1], T.r[2]);
		fl::mov(T.r[2]);
		tick(0x8B98, 21);
		goto epilogue;
	}
	tick(0x8B9A, 12);                                  // BNE
	fl::cmp(T.r[0], 0x001F);
	tick(0x8B9E, 18);                                  // CMP R0,#0x1F
	if (c == 0x1F)
	{
		tick(0x8BA0, 12);                              // BNE
		T.r[0] = 0x0074;
		fl::mov(T.r[0]);
		tick(0x8BA4, 18);
		tick(0x8BE0, 12);                              // BR
	}
	else
	{
		tick(0x8BA6, 12);                              // BNE
		fl::cmp(T.r[0], 0x002D);
		tick(0x8BAA, 18);                              // CMP R0,#0x2D
		if (c == 0x2D)
		{
			tick(0x8BAC, 12);                          // BNE
			T.r[0] = 0x0084;
			fl::mov(T.r[0]);
			tick(0x8BB0, 18);
			tick(0x8BBE, 12);                          // BR
			T.r[3] = fl::logic((uint16_t)(T.r[3] | 0x0100));
			tick(0x8BC2, 18);
			tick(0x8BE0, 12);                          // BR
		}
		else
		{
			tick(0x8BB2, 12);                          // BNE
			fl::cmp(T.r[0], 0x003D);
			tick(0x8BB6, 18);                          // CMP R0,#0x3D
			if (c == 0x3D)
			{
				tick(0x8BB8, 12);                      // BNE
				T.r[3] = fl::logic((uint16_t)(T.r[3] | 0x0100));
				tick(0x8BBC, 18);
			}
			tick(0x8BC4, 12);                          // BNE / BR
			fl::cmp(T.r[0], 0x0040);
			tick(0x8BC8, 18);                          // CMP R0,#0x40
			if ((int16_t)c > 0x40)
			{
				tick(0x8BD8, 12);                      // BGT
				T.r[0] = (uint16_t)(c - 0x41);
				fl::sub((uint16_t)(T.r[0] + 0x0041), 0x0041);
				tick(0x8BDC, 18);
				T.r[0] = fl::asl(T.r[0]);
				tick(0x8BDE, 12);
				T.r[0] = fl::asl(T.r[0]);
				tick(0x8BE0, 12);
			}
			else
			{
				tick(0x8BCA, 12);                      // BGT
				T.r[0] = (uint16_t)(c - 0x20);
				fl::sub((uint16_t)(T.r[0] + 0x0020), 0x0020);
				tick(0x8BCE, 18);
				T.r[0] = (uint16_t)(int16_t)(int8_t)rb((uint16_t)(0x8C86 + T.r[0]));
				fl::movb((uint8_t)T.r[0]);
				tick(0x8BD2, 27);
				T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x00FF));
				tick(0x8BD6, 18);
				tick(0x8BE0, 12);                      // BR
			}
		}
	}
	// the tile group at 0x4000 + R0, written with the colour bits R3
	T.r[0] = fl::add(T.r[0], 0x4000);
	tick(0x8BE4, 18);
	T.r[1] = peek(0x0186);
	fl::mov(T.r[1]);
	tick(0x8BE8, 24);
	T.r[2] = 0;
	fl::clr();
	tick(0x8BEA, 12);
	T.r[2] |= rb(T.r[0]++);
	fl::logicb((uint8_t)T.r[2]);
	tick(0x8BEC, 18);
	T.r[2] = fl::logic((uint16_t)(T.r[2] | T.r[3]));
	tick(0x8BEE, 12);
	ww(T.r[1], T.r[2]);
	T.r[1] += 2;
	fl::mov(T.r[2]);
	tick(0x8BF0, 21);
	T.r[2] &= 0xFF00;
	fl::clr();
	tick(0x8BF2, 12);
	T.r[2] |= rb(T.r[0]++);
	fl::logicb((uint8_t)T.r[2]);
	tick(0x8BF4, 18);
	T.r[2] = fl::logic((uint16_t)(T.r[2] | T.r[3]));
	tick(0x8BF6, 12);
	ww(T.r[1], T.r[2]);
	T.r[1] += 2;
	fl::mov(T.r[2]);
	tick(0x8BF8, 21);
	poke(0x0186, T.r[1]);
	fl::mov(T.r[1]);
	tick(0x8BFC, 27);
	T.r[1] = fl::add(T.r[1], 0x007C);
	tick(0x8C00, 18);
	T.r[2] &= 0xFF00;                                  // CLRB R2 (C clear from here on)
	fl::clr();
	tick(0x8C02, 12);
	T.r[2] |= rb(T.r[0]++);
	fl::logicb((uint8_t)T.r[2]);
	tick(0x8C04, 18);
	T.r[2] = fl::logic((uint16_t)(T.r[2] | T.r[3]));
	tick(0x8C06, 12);
	ww(T.r[1], T.r[2]);
	T.r[1] += 2;
	fl::mov(T.r[2]);
	tick(0x8C08, 21);
	T.r[2] &= 0xFF00;
	fl::clr();
	tick(0x8C0A, 12);
	T.r[2] |= rb(T.r[0]);
	fl::logicb((uint8_t)T.r[2]);
	tick(0x8C0C, 18);
	T.r[2] = fl::logic((uint16_t)(T.r[2] | T.r[3]));
	tick(0x8C0E, 12);
	ww(T.r[1], T.r[2]);
	fl::mov(T.r[2]);
	tick(0x8C10, 21);

epilogue:
	// RTS into F150, BR F176, MOV (SP)+,R3 / R2 / R1, RTS
	pop();
	tick(0xF150, 21);
	tick(0xF176, 12);
	T.r[3] = pop();
	fl::mov(T.r[3]);
	tick(0xF178, 18);
	T.r[2] = pop();
	fl::mov(T.r[2]);
	tick(0xF17A, 18);
	T.r[1] = pop();
	f_nzvc(nzw(T.r[1]) | (carry ? CF : 0));
	tick(0xF17C, 18);
	rts();
}

// F:8D2C / F:8D3E (assembly): clear video RAM window 0 from 0x2000 to 0x37FF (playfield)
// or from 0x3800 to 0x3FFF (text layer and sprites); step by step, interrupts come in all the
// time meanwhile
void clear_video(uint16_t pc, uint16_t from, uint16_t last)
{
	jsr((uint16_t)(pc + 4), 0x8068);                   // select_video_bank0
	T.r[0] = from;
	fl::mov(T.r[0]);
	tick((uint16_t)(pc + 8), 18);
	const uint16_t loop = (uint16_t)(pc + 8);
	for (;;)
	{
		poke(T.r[0], 0);
		fl::clr();
		T.r[0] += 2;
		tick((uint16_t)(loop + 2), 21);                // CLR (R0)+
		fl::cmp(T.r[0], last);
		tick((uint16_t)(loop + 6), 18);
		if (T.r[0] > last) break;
		tick(loop, 12);                                // BLOS
	}
	tick((uint16_t)(loop + 8), 12);
	rts();
}
void clear_playfield_ram() { clear_video(0x8D2C, 0x2000, 0x37FE); }
void clear_text_ram() { clear_video(0x8D3E, 0x3800, 0x3FFE); }

// F:B9D4 (interrupt, 32V during play): count the quarter frame in 0x0191; at the first and
// the fourth the sprite list link is rebuilt (F:B1C8) with 0x0993 = 1 / 0.  Other interrupts
// are allowed meanwhile (PSW 0); video bank 0 is selected and the previous bank restored.
// Step by step like the original, as the other interrupts may come in at any point.
void irq_32v_play()
{
	push(T.r[0]);
	fl::mov(T.r[0]);
	tick(0xB9D6, 24);
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xB9D8, 24);
	push(T.r[2]);
	fl::mov(T.r[2]);
	tick(0xB9DA, 24);
	push(T.r[3]);
	fl::mov(T.r[3]);
	tick(0xB9DC, 24);
	push(T.r[4]);
	fl::mov(T.r[4]);
	tick(0xB9DE, 24);
	push(T.r[5]);
	fl::mov(T.r[5]);
	tick(0xB9E0, 24);
	poke(0x15C0, 0);                                   // acknowledge
	fl::clr();
	tick(0xB9E4, 27);
	pokeb(0x0191, fl::incb(peekb(0x0191)));
	tick(0xB9E8, 27);
	T.r[0] = 0;
	fl::clr();
	tick(0xB9EA, 12);
	T.psw &= 0x10;                                     // MTPS #0: other interrupts may come in
	T.check_irqs = true;
	tick(0xB9EE, 30);
	push(peek(0x01D0));
	fl::mov(peek(T.r[6]));
	tick(0xB9F2, 36);
	jsr(0xB9F6, 0x8068);                               // select_video_bank0
	const bool first = peekb(0x0191) == 1;
	fl::cmpb(peekb(0x0191), 0x01);
	tick(0xB9FC, 30);                                  // CMPB @#0x0191,#1
	if (first)
	{
		tick(0xB9FE, 12);
		pokeb(0x0993, 1);
		fl::movb(0x01);
		tick(0xBA04, 33);
		tick(0xBA12, 12);                              // BR
		jsr(0xBA16, 0xB1C8);                           // sprite_link
	}
	else
	{
		tick(0xBA06, 12);                              // BNE
		const bool fourth = peekb(0x0191) == 4;
		fl::cmpb(peekb(0x0191), 0x04);
		tick(0xBA0C, 30);                              // CMPB @#0x0191,#4
		if (fourth)
		{
			tick(0xBA0E, 12);
			pokeb(0x0993, 0);
			fl::clr();
			tick(0xBA12, 27);
			jsr(0xBA16, 0xB1C8);
		}
		else
			tick(0xBA16, 12);                          // BNE
	}
	poke(0x01D0, rw(T.r[6]));
	fl::mov(peek(0x01D0));
	tick(0xBA1A, 33);
	jsr(0xBA1E, 0x8096);                               // reselect_video_bank
	T.r[6] += 2;
	fl::tst(peek((uint16_t)(T.r[6] - 2)));
	tick(0xBA20, 18);                                  // TST (SP)+
	T.r[5] = pop();
	fl::mov(T.r[5]);
	tick(0xBA22, 18);
	T.r[4] = pop();
	fl::mov(T.r[4]);
	tick(0xBA24, 18);
	T.r[3] = pop();
	fl::mov(T.r[3]);
	tick(0xBA26, 18);
	T.r[2] = pop();
	fl::mov(T.r[2]);
	tick(0xBA28, 18);
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0xBA2A, 18);
	T.r[0] = pop();
	fl::mov(T.r[0]);
	tick(0xBA2C, 18);                                  // RTI in the table
}


// F:DD10 (assembly): while 0x0EA9 counts down (every fourth frame) the colours 0x11C2-0x11C6
// rotate through the four at 0x7A9A; at 0 they are restored from 0x7E6C.  Step by step.
void flash_text_colours()
{
	const uint8_t n = peekb(0x0EA9);
	fl::tstb(n);
	tick(0xDD14, 24);
	if (!n)
	{
		tick(0xDD5A, 12);                              // BEQ
		rts();
		return;
	}
	tick(0xDD16, 12);
	const uint16_t phase = peek(0x01C4) & 0x0003;
	fl::mov(phase);
	tick(0xDD1C, 30);
	if (phase)
	{
		tick(0xDD5A, 12);                              // BNE
		rts();
		return;
	}
	tick(0xDD1E, 12);
	pokeb(0x0EA9, fl::decb(peekb(0x0EA9)));
	tick(0xDD22, 27);
	T.r[3] = 0;
	fl::clr();
	tick(0xDD24, 12);
	T.r[3] = peekb(0x0EA9);
	fl::logicb((uint8_t)T.r[3]);
	tick(0xDD28, 24);
	fl::tst(T.r[3]);
	tick(0xDD2A, 12);                                  // TST R3
	if (T.r[3] == 0)
	{
		tick(0xDD2C, 12);
		T.r[1] = 3;
		fl::mov(T.r[1]);
		tick(0xDD30, 18);
		for (;;)
		{
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0xDD32, 12);
			T.r[0] = fl::asl(T.r[0]);
			tick(0xDD34, 12);
			const uint16_t v = rw((uint16_t)(0x7E6C + T.r[0]));
			poke((uint16_t)(0x11C0 + T.r[0]), v);
			f_nzvc(nzw(v));                            // C is clear from the ASL
			tick(0xDD3A, 45);
			if (--T.r[1] == 0) break;
			tick(0xDD30, 18);                          // SOB
		}
		tick(0xDD3C, 18);
	}
	else
	{
		tick(0xDD3E, 12);                              // BNE
		T.r[1] = 3;
		fl::mov(T.r[1]);
		tick(0xDD42, 18);
		for (;;)
		{
			T.r[2] = T.r[1];
			fl::mov(T.r[2]);
			tick(0xDD44, 12);
			T.r[2] = fl::asl(T.r[2]);
			tick(0xDD46, 12);
			T.r[0] = T.r[3];
			fl::mov(T.r[0]);
			tick(0xDD48, 12);
			T.r[0] = fl::add(T.r[0], T.r[1]);
			tick(0xDD4A, 12);
			T.r[0] = fl::dec(T.r[0]);
			tick(0xDD4C, 12);
			T.r[0] = fl::logic((uint16_t)(T.r[0] & 0x0003));
			tick(0xDD50, 18);
			T.r[0] = fl::asl(T.r[0]);
			tick(0xDD52, 12);
			const uint16_t v = rw((uint16_t)(0x7A9A + T.r[0]));
			poke((uint16_t)(0x11C0 + T.r[2]), v);
			f_nzvc(nzw(v));
			tick(0xDD58, 45);
			if (--T.r[1] == 0) break;
			tick(0xDD42, 18);                          // SOB
		}
		tick(0xDD5A, 18);
	}
	rts();
}

}

LOGIC_TABLE(screen,
	asm_routine("F", 0x8B52, "draw_large_char", draw_large_char, 0),
	asm_routine("F", 0x8D2C, "clear_playfield_ram", clear_playfield_ram, 0),
	asm_routine("F", 0x8D3E, "clear_text_ram", clear_text_ram, 0),
	irq_routine("F", 0xB9D4, "irq_32v_play", irq_32v_play, 24),
	asm_routine("F", 0xDD10, "flash_text_colours", flash_text_colours, 0),
)

}
