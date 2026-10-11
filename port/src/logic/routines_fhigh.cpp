// Rewritten routines of the fixed ROM F:D000-FFFF: object slots and their spawning, the
// object scripts, palette fades, skater animation tables, score and the start buttons.
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

uint16_t sext8(uint8_t v) { return (uint16_t)(int16_t)(int8_t)v; }

// condition codes of the instructions the fl:: helpers do not cover (as the T-11 sets them)
uint16_t fl_asl(uint16_t v) { const uint16_t r = (uint16_t)(v << 1); f_nzvc(nzw(r) | (v >> 15)); f_shift(); return r; }
uint16_t fl_asr(uint16_t v) { const uint16_t r = (uint16_t)((int16_t)v >> 1); f_nzvc(nzw(r) | (v & 1)); f_shift(); return r; }
uint16_t fl_ror(uint16_t v) { const uint16_t r = (uint16_t)((C() << 15) | (v >> 1)); f_nzvc(nzw(r) | (v & 1)); f_shift(); return r; }
uint16_t fl_rol(uint16_t v) { const uint16_t r = (uint16_t)((v << 1) | C()); f_nzvc(nzw(r) | (v >> 15)); f_shift(); return r; }
uint16_t fl_neg(uint16_t v) { const uint16_t r = (uint16_t)-v; f_nzvc(nzw(r) | (r == 0x8000 ? VF : 0) | (r ? CF : 0)); return r; }
uint8_t fl_negb(uint8_t v) { const uint8_t r = (uint8_t)-v; f_nzvc(nzb(r) | (r == 0x80 ? VF : 0) | (r ? CF : 0)); return r; }
uint16_t fl_com(uint16_t v) { const uint16_t r = (uint16_t)~v; f_nzvc(nzw(r) | CF); return r; }
uint8_t fl_comb(uint8_t v) { const uint8_t r = (uint8_t)~v; f_nzvc(nzb(r) | CF); return r; }
uint16_t fl_swab(uint16_t v) { const uint16_t r = (uint16_t)((v << 8) | (v >> 8)); f_nzvc(nzb(r & 0xFF)); return r; }
uint16_t fl_logic(uint16_t r) { f_nzv(nzw(r)); return r; }   // BIC / BIS / XOR / BIT
uint8_t fl_logicb(uint8_t r) { f_nzv(nzb(r)); return r; }

// JSR PC,(Rn) at ret - 2 (27 cycles): as steps::jsr
void jsr_reg(uint16_t ret, uint16_t addr)
{
	push(ret);
	T.r[7] = addr;
	tick(addr, 27);
	shadow_push(ret);
	dispatch();
	shadow_pop();
	if (T.r[7] != ret) throw Unwound{};
}

// the stack word at SP + o (the original addresses its locals relative to the moving SP)
uint16_t sp_at(int o) { return (uint16_t)(T.r[6] + o); }

// MOV #a,R0; MOV R0,@#0x01D2; MOV R0,@#BANK_A at pc (bank_b: 0x01D4 / BANK_B), instruction by
// instruction
void bank_a(uint16_t pc, uint16_t a)
{
	T.r[0] = a;
	fl::mov(a);
	tick((uint16_t)(pc + 4), 18);
	poke(0x01D2, T.r[0]);
	tick((uint16_t)(pc + 8), 27);
	poke(0x1400, T.r[0]);
	tick((uint16_t)(pc + 0x0C), 27);
}
void bank_b(uint16_t pc, uint16_t b)
{
	T.r[0] = b;
	fl::mov(b);
	tick((uint16_t)(pc + 4), 18);
	poke(0x01D4, T.r[0]);
	tick((uint16_t)(pc + 8), 27);
	poke(0x1402, T.r[0]);
	tick((uint16_t)(pc + 0x0C), 27);
}
// CLR @#0x01D2; CLR @#BANK_A; MOV #0x0400,Rr; MOV Rr,@#0x01D4; MOV Rr,@#BANK_B at pc (the
// banks of the type tables)
void type_banks(uint16_t pc, int r)
{
	poke(0x01D2, 0);
	fl::clr();
	tick((uint16_t)(pc + 4), 27);
	poke(0x1400, 0);
	fl::clr();
	tick((uint16_t)(pc + 8), 27);
	T.r[r] = 0x0400;
	fl::mov(T.r[r]);
	tick((uint16_t)(pc + 0x0C), 18);
	poke(0x01D4, T.r[r]);
	tick((uint16_t)(pc + 0x10), 27);
	poke(0x1402, T.r[r]);
	tick((uint16_t)(pc + 0x14), 27);
}
// MOV Ra,@#0x01D2; MOV Rb,@#0x01D4; MOV @#0x01D2,@#BANK_A; MOV @#0x01D4,@#BANK_B at pc: the
// banks saved in registers put back
void banks_back(uint16_t pc, int ra, int rb)
{
	poke(0x01D2, T.r[ra]);
	fl::mov(T.r[ra]);
	tick((uint16_t)(pc + 4), 27);
	poke(0x01D4, T.r[rb]);
	fl::mov(T.r[rb]);
	tick((uint16_t)(pc + 8), 27);
	{
		const uint16_t v = peek(0x01D2);
		fl::mov(v);
		poke(0x1400, v);
	}
	tick((uint16_t)(pc + 0x0E), 39);
	{
		const uint16_t v = peek(0x01D4);
		fl::mov(v);
		poke(0x1402, v);
	}
	tick((uint16_t)(pc + 0x14), 39);
}
// MOV @#0x01D2,Ra; MOV @#0x01D4,Rb at pc: the banks saved in registers
void banks_save(uint16_t pc, int ra, int rb)
{
	T.r[ra] = peek(0x01D2);
	fl::mov(T.r[ra]);
	tick((uint16_t)(pc + 4), 24);
	T.r[rb] = peek(0x01D4);
	fl::mov(T.r[rb]);
	tick((uint16_t)(pc + 8), 24);
}
// TST (SP)+ at pc
void drop(uint16_t pc)
{
	fl::tst(rw(T.r[6]));
	T.r[6] = (uint16_t)(T.r[6] + 2);
	tick((uint16_t)(pc + 2), 18);
}

// F:D0F4: with banks 0x1000/0x1400, F:C3E6 runs for each slot 0x09AA = 0xAE..0 whose word in
// 0x0AF8 is negative; then object 4 is reset (F:B56E) if it is active.  Banks restored.
uint16_t update_marked_slots(const Args &)
{
	c_save(1, 0xD0F8);
	banks_save(0xD0F8, 1, 2);
	bank_a(0xD100, 0x1000);
	bank_b(0xD10C, 0x1400);
	poke(0x09AA, 0x00AE);
	fl::mov(0x00AE);
	tick(0xD11E, 33);
	for (;;)                                           // map objects 0xAE down to 0
	{
		T.r[0] = peek(0x09AA);
		fl::mov(T.r[0]);
		tick(0xD122, 24);
		if ((int16_t)T.r[0] < 0)
		{
			tick(0xD136, 12);                          // BLT
			break;
		}
		tick(0xD124, 12);
		T.r[0] = fl_asl(T.r[0]);
		tick(0xD126, 12);
		const uint16_t desc = peek((uint16_t)(0x0AF8 + T.r[0]));
		fl::tst(desc);
		tick(0xD12A, 27);
		if ((int16_t)desc >= 0)
			tick(0xD130, 12);                          // BGE: no object
		else
		{
			tick(0xD12C, 12);
			jsr(0xD130, 0xC3E6);                       // map_object_leaves
		}
		poke(0x09AA, fl::dec(peek(0x09AA)));
		tick(0xD134, 27);
		tick(0xD11E, 12);                              // BR
	}
	const uint8_t active = peekb(0x0374);              // object 4
	fl::tstb(active);
	tick(0xD13A, 24);
	if (!active)
		tick(0xD146, 12);                              // BEQ
	else
	{
		tick(0xD13C, 12);
		push(0x0004);
		fl::mov(0x0004);
		tick(0xD140, 30);
		jsr(0xD144, 0xB56E);                           // reset_object(4)
		drop(0xD144);
	}
	banks_back(0xD146, 1, 2);
	c_restore(1);
	return T.r[0];
}

// F:D15C: F:CF5E(slot) for the slots 0xFE down to 0xAF whose type byte 0x09B8 is set
uint16_t update_high_slots(const Args &)
{
	c_save(2, 0xD160);
	banks_save(0xD160, 2, 3);
	T.r[0] = 0x00FE;
	fl::mov(T.r[0]);
	tick(0xD16C, 18);
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0xD16E, 12);
	tick(0xD180, 12);                                  // BR
	for (;;)                                           // map objects 0xFE down to 0xAF
	{
		fl::cmp(T.r[1], 0x00AF);
		tick(0xD184, 18);
		if ((int16_t)T.r[1] < 0x00AF)
		{
			tick(0xD186, 12);                          // BGE not taken
			break;
		}
		tick(0xD170, 12);
		const uint8_t type = peekb((uint16_t)(0x09B8 + T.r[1]));
		fl::tstb(type);
		tick(0xD174, 27);
		if (!type)
			tick(0xD17E, 12);                          // BEQ
		else
		{
			tick(0xD176, 12);
			push(T.r[1]);
			fl::mov(T.r[1]);
			tick(0xD178, 24);
			jsr(0xD17C, 0xCF5E);                       // remove_map_object
			drop(0xD17C);
		}
		T.r[1] = fl::dec(T.r[1]);
		tick(0xD180, 12);
	}
	banks_back(0xD186, 2, 3);
	c_restore(2);
	return T.r[0];
}

// F:D880: the eight entries of the table at 0x70C4 (bank B 0x0400) get a free slot each
// (F:CDFA find_other_slot(-1, value)) whose type 0x09B8 comes from 0x70D4, or from 0x70DC
// when the park of the entry (index / 2) is open (F:E6FE).  Returns the last slot.
uint16_t set_park_entry_types(const Args &)
{
	c_save(2, 0xD884);
	jsr(0xD888, 0xE6FE);                               // the open parks' bits
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0xD88A, 12);
	type_banks(0xD88A, 0);
	T.r[3] = 0x0007;
	fl::mov(T.r[3]);
	tick(0xD8A2, 18);
	for (;;)                                           // entries 7 down to 0
	{
		T.r[2] = T.r[3];
		fl::mov(T.r[2]);
		tick(0xD8A4, 12);
		T.r[2] = fl_asl(T.r[2]);
		tick(0xD8A6, 12);
		{
			const uint16_t v = peek((uint16_t)(0x70C4 + T.r[2]));
			fl::mov(v);
			push(v);
		}
		tick(0xD8AA, 39);
		push(0xFFFF);
		fl::mov(0xFFFF);
		tick(0xD8AE, 30);
		jsr(0xD8B2, 0xCDFA);                           // find_other_slot(-1, value)
		T.r[2] = T.r[3];
		fl::mov(T.r[2]);
		tick(0xD8B4, 12);
		T.r[2] = fl_asr(T.r[2]);                       // the entry's park
		tick(0xD8B6, 12);
		const bool open = fl_logicb((uint8_t)((uint8_t)T.r[1] & peekb((uint16_t)(0xFC74 + T.r[2])))) != 0;
		tick(0xD8BA, 27);
		if (!open)
		{
			tick(0xD8BC, 12);
			const uint8_t type = peekb((uint16_t)(0x70D4 + T.r[3]));
			pokeb((uint16_t)(0x09B8 + T.r[0]), type);
			fl::movb(type);
			tick(0xD8C2, 45);
			tick(0xD8CA, 12);                          // BR
		}
		else
		{
			tick(0xD8C4, 12);                          // BNE
			const uint8_t type = peekb((uint16_t)(0x70DC + T.r[3]));
			pokeb((uint16_t)(0x09B8 + T.r[0]), type);
			fl::movb(type);
			tick(0xD8CA, 45);
		}
		const uint16_t a = pop(), b = pop();
		fl::cmp(a, b);                                 // CMP (SP)+,(SP)+
		tick(0xD8CC, 24);
		T.r[3] = fl::dec(T.r[3]);
		tick(0xD8CE, 12);
		if ((int16_t)T.r[3] < 0)
		{
			tick(0xD8D0, 12);                          // BGE not taken
			break;
		}
		tick(0xD8A2, 12);
	}
	c_restore(2);
	return T.r[0];
}

// F:D8D2: the four entries of the table at 0x70AA (bank B 0x0400) get a slot (F:CDFA) whose
// type comes from 0x70BB, or 0x70BF when the ticket bit (0x0ED7) of the entry is set.  When
// no slot is found, the object of the slot found for value | 0x8000 is set up instead.
uint16_t set_ticket_entry_types(const Args &)
{
	c_save(4, 0xD8D6);
	banks_save(0xD8D6, 4, 5);
	type_banks(0xD8DE, 0);
	T.r[1] = 0x0004;
	fl::mov(T.r[1]);
	tick(0xD8F6, 18);
	for (;;)                                           // tickets 4 down to 1
	{
		T.r[2] = T.r[1];
		fl::mov(T.r[2]);
		tick(0xD8F8, 12);
		T.r[2] = fl_asl(T.r[2]);
		tick(0xD8FA, 12);
		{
			const uint16_t v = peek((uint16_t)(0x70AA + T.r[2]));
			fl::mov(v);
			push(v);
		}
		tick(0xD8FE, 39);
		push(0xFFFF);
		fl::mov(0xFFFF);
		tick(0xD902, 30);
		jsr(0xD906, 0xCDFA);                           // find_other_slot(-1, value)
		fl::tst(T.r[0]);
		tick(0xD908, 12);
		if ((int16_t)T.r[0] >= 0)
		{
			// its map object's type: 0x70BB, or 0x70BF with the ticket held
			tick(0xD90A, 12);
			const bool held = fl_logicb((uint8_t)(peekb(0x0ED7) & peekb((uint16_t)(0xFC73 + T.r[1])))) != 0;
			tick(0xD910, 39);
			if (!held)
			{
				tick(0xD912, 12);
				const uint8_t type = peekb((uint16_t)(0x70BB + T.r[1]));
				pokeb((uint16_t)(0x09B8 + T.r[0]), type);
				fl::movb(type);
				tick(0xD918, 45);
				tick(0xD962, 12);                      // BR
			}
			else
			{
				tick(0xD91A, 12);                      // BNE
				const uint8_t type = peekb((uint16_t)(0x70BF + T.r[1]));
				pokeb((uint16_t)(0x09B8 + T.r[0]), type);
				fl::movb(type);
				tick(0xD920, 45);
				tick(0xD962, 12);                      // BR
			}
		}
		else
		{
			// no slot: the object of the slot found for value | 0x8000 is set up instead
			tick(0xD922, 12);                          // BLT
			{
				const uint16_t v = peek((uint16_t)(0x70AA + T.r[2]));
				ww(T.r[6], v);
				fl::mov(v);
			}
			tick(0xD926, 36);
			ww(T.r[6], fl_logic(rw(T.r[6]) | 0x8000));
			tick(0xD92A, 27);
			push(0xFFFF);
			fl::mov(0xFFFF);
			tick(0xD92E, 30);
			jsr(0xD932, 0xCDFA);                       // find_other_slot
			T.r[0] = sext8(peekb((uint16_t)(0x09B8 + T.r[0])));
			fl::movb((uint8_t)T.r[0]);
			tick(0xD936, 27);
			T.r[0] = fl_logic(T.r[0] & 0x003F);
			tick(0xD93A, 18);
			const bool held = fl_logicb((uint8_t)(peekb(0x0ED7) & peekb((uint16_t)(0xFC73 + T.r[1])))) != 0;
			tick(0xD940, 39);
			if (!held)
			{
				tick(0xD942, 12);
				const uint8_t type = peekb((uint16_t)(0x70BB + T.r[1]));
				pokeb((uint16_t)(0x0370 + T.r[0]), type);
				fl::movb(type);
				tick(0xD948, 45);
				T.r[3] = T.r[0];
				fl::mov(T.r[3]);
				tick(0xD94A, 12);
				T.r[3] = fl_asl(T.r[3]);
				tick(0xD94C, 12);
				{
					const uint16_t anim = peek((uint16_t)(0x5B82 + T.r[2]));
					poke((uint16_t)(0x03B0 + T.r[3]), anim);
					fl::mov(anim);
				}
				tick(0xD952, 45);
				tick(0xD95A, 12);                      // BR
			}
			else
			{
				tick(0xD954, 12);                      // BNE
				const uint8_t type = peekb((uint16_t)(0x70BF + T.r[1]));
				pokeb((uint16_t)(0x0370 + T.r[0]), type);
				fl::movb(type);
				tick(0xD95A, 45);
			}
			{
				const uint16_t flags = (uint16_t)(0x0490 + T.r[0]);
				pokeb(flags, fl_logicb((uint8_t)(peekb(flags) | 0x18)));
			}
			tick(0xD960, 36);
			drop(0xD960);
		}
		const uint16_t a = pop(), b = pop();
		fl::cmp(a, b);                                 // CMP (SP)+,(SP)+
		tick(0xD964, 24);
		T.r[1]--;                                      // SOB
		if (!T.r[1])
		{
			tick(0xD966, 18);
			break;
		}
		tick(0xD8F6, 18);
	}
	banks_back(0xD966, 4, 5);
	c_restore(4);
	return T.r[0];
}

// F:D47C  start_object(int i): object i gets animation 0x6046[type] (type = 0x0370[i], bank B
// 0x0400), timers 0x0770/0x0790 = 8, flags 0x18 and its start sound (0x6567[type] through
// the table at 0xFDED); then F:BF18 runs for its slot (0x0390[i] into 0x09AA)
uint16_t start_object(const Args &)
{
	c_save(2, 0xD480);
	type_banks(0xD480, 0);
	T.r[1] = rw(sp_at(0x0A));                          // the object
	fl::mov(T.r[1]);
	tick(0xD498, 27);
	T.r[0] = 0x0008;
	fl::mov(T.r[0]);
	tick(0xD49C, 18);
	pokeb((uint16_t)(0x0790 + T.r[1]), (uint8_t)T.r[0]);   // animation rate and counter
	fl::movb((uint8_t)T.r[0]);
	tick(0xD4A0, 30);
	pokeb((uint16_t)(0x0770 + T.r[1]), (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0xD4A4, 30);
	T.r[3] = T.r[1];
	fl::mov(T.r[3]);
	tick(0xD4A6, 12);
	T.r[3] = fl_asl(T.r[3]);
	tick(0xD4A8, 12);
	T.r[2] = 0;
	fl::clr();
	tick(0xD4AA, 12);
	T.r[2] = fl_logicb(peekb((uint16_t)(0x0370 + T.r[1])));   // its type
	tick(0xD4AE, 27);
	T.r[0] = T.r[2];
	fl::mov(T.r[0]);
	tick(0xD4B0, 12);
	T.r[0] = fl_asl(T.r[0]);
	tick(0xD4B2, 12);
	{
		const uint16_t anim = peek((uint16_t)(0x6046 + T.r[0]));   // its second animation
		poke((uint16_t)(0x03B0 + T.r[3]), anim);
		fl::mov(anim);
	}
	tick(0xD4B8, 45);
	{
		const uint16_t flags = (uint16_t)(0x0490 + T.r[1]);
		pokeb(flags, fl_logicb((uint8_t)(peekb(flags) | 0x18)));
	}
	tick(0xD4BE, 36);
	T.r[0] = 0;
	fl::clr();
	tick(0xD4C0, 12);
	T.r[0] = fl_logicb(peekb((uint16_t)(0x6567 + T.r[2])));   // its sound
	tick(0xD4C4, 27);
	fl::tst(T.r[0]);
	tick(0xD4C6, 12);
	if (!T.r[0])
		tick(0xD4D4, 12);                              // BEQ
	else
	{
		tick(0xD4C8, 12);
		push(0);
		fl::clr();
		tick(0xD4CA, 24);
		{
			const uint8_t cmd = peekb((uint16_t)(0xFDED + T.r[0]));
			pokeb(T.r[6], cmd);
			fl::movb(cmd);
		}
		tick(0xD4CE, 36);
		jsr(0xD4D2, 0x8E78);                           // queue_sound
		drop(0xD4D2);
	}
	{
		const uint8_t n = peekb((uint16_t)(0x0390 + T.r[1]));   // its map object
		pokeb(0x09AA, n);
		fl::movb(n);
	}
	tick(0xD4DA, 42);
	pokeb(0x09AB, 0);
	fl::clr();
	tick(0xD4DE, 27);
	jsr(0xD4E2, 0xBF18);                               // release_map_definition
	c_restore(2);
	return T.r[0];
}

// F:DD5C: colour 0x1092 of the text palette.  On level 0 it comes from ROM (0x5ED2, bank
// 0x2000); on the other levels it is one of four colours by where the value 0x0F20 (0x0F34
// from 0x027A = 2 on) lies against the limits 0x0FDA / 0x0FDC / 0x0FDE (0: the lowest);
// nothing changes from 0x0F26 = 6 on.
uint16_t set_colour_1092(const Args &)
{
	c_save(1, 0xDD60);
	const uint8_t level = peekb(0x0F59);
	fl::tstb(level);
	tick(0xDD64, 24);
	if (!level)
	{
		// Skate City: the colour from ROM
		tick(0xDDB0, 12);                              // BEQ
		banks_save(0xDDB0, 1, 2);
		bank_a(0xDDB8, 0x2000);
		bank_b(0xDDC4, 0x2400);
		{
			const uint16_t c = peek(0x5ED2);
			poke(0x1092, c);
			fl::mov(c);
		}
		tick(0xDDD6, 39);
		banks_back(0xDDD6, 1, 2);
	}
	else
	{
		tick(0xDD66, 12);
		const uint8_t stage = peekb(0x0F26);
		fl::cmpb(stage, 0x06);
		tick(0xDD6C, 30);
		if (stage >= 0x06)
			tick(0xDDEA, 12);                          // BCC: left as it is
		else
		{
			tick(0xDD6E, 12);
			const uint8_t round = peekb(0x027A);
			fl::cmpb(round, 0x02);
			tick(0xDD74, 30);
			if (round >= 0x02)
			{
				tick(0xDD7C, 12);                      // BCC
				T.r[0] = peek(0x0F34);
				fl::mov(T.r[0]);
				tick(0xDD80, 24);
			}
			else
			{
				tick(0xDD76, 12);
				T.r[0] = peek(0x0F20);
				fl::mov(T.r[0]);
				tick(0xDD7A, 24);
				tick(0xDD80, 12);                      // BR
			}
			// one of four colours by the value against the limits 0x0FDA / 0x0FDC / 0x0FDE
			if (!T.r[0])
			{
				tick(0xDDA6, 12);                      // BEQ
				T.r[0] = 0x800F;
				fl::mov(T.r[0]);
				tick(0xDDAA, 18);
			}
			else
			{
				tick(0xDD82, 12);
				fl::cmp(T.r[0], peek(0x0FDE));
				tick(0xDD86, 24);
				if ((int16_t)T.r[0] >= (int16_t)peek(0x0FDE))
				{
					tick(0xDD88, 12);
					T.r[0] = 0xFF19;
					fl::mov(T.r[0]);
					tick(0xDD8C, 18);
					tick(0xDDAA, 12);                  // BR
				}
				else
				{
					tick(0xDD8E, 12);                  // BLT
					fl::cmp(T.r[0], peek(0x0FDC));
					tick(0xDD92, 24);
					if ((int16_t)T.r[0] >= (int16_t)peek(0x0FDC))
					{
						tick(0xDD94, 12);
						T.r[0] = 0xEEFA;
						fl::mov(T.r[0]);
						tick(0xDD98, 18);
						tick(0xDDAA, 12);              // BR
					}
					else
					{
						tick(0xDD9A, 12);              // BLT
						fl::cmp(T.r[0], peek(0x0FDA));
						tick(0xDD9E, 24);
						if ((int16_t)T.r[0] >= (int16_t)peek(0x0FDA))
						{
							tick(0xDDA0, 12);
							T.r[0] = 0xC73A;
							fl::mov(T.r[0]);
							tick(0xDDA4, 18);
							tick(0xDDAA, 12);          // BR
						}
						else
						{
							tick(0xDDA6, 12);          // BLT
							T.r[0] = 0x800F;
							fl::mov(T.r[0]);
							tick(0xDDAA, 18);
						}
					}
				}
			}
			poke(0x1092, T.r[0]);
			fl::mov(T.r[0]);
			tick(0xDDAE, 27);
		}
	}
	c_restore(1);
	return T.r[0];
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
	// four byte streams (pointers 0x0EC2 + 2 * stream, counters 0x0ECA + stream) of (count,
	// value) entries; the table at 0xFE06 holds each stream's variable.  Streams 0 / 1 add
	// value * 8 to x / y every frame; streams 2 / 3 drive animation pointers.  0xFF n: jump
	// back n bytes and reload the variable.
	c_save(4, 0xDE2C);
	T.r[6] = (uint16_t)(T.r[6] - 2);                   // TST -(SP): the stream's counter
	fl::tst(rw(T.r[6]));
	tick(0xDE2E, 21);
	bank_a(0xDE2E, 0x2400);
	bank_b(0xDE3A, 0x2800);
	{
		const uint16_t x = peek(0x06FA);
		poke(0x0224, x);
		fl::mov(x);
	}
	tick(0xDE4C, 39);
	{
		const uint16_t y = peek(0x073C);
		poke(0x0226, y);
		fl::mov(y);
	}
	tick(0xDE52, 39);
	T.r[4] = 0;                                        // the stream
	fl::clr();
	tick(0xDE54, 12);
	auto counter_word = []() {                         // MOVB (R5),(SP); CLRB 1(SP); TST (SP)
		const uint8_t n = peekb(T.r[5]);
		pokeb(T.r[6], n);
		fl::movb(n);
	};
	uint16_t at = 0xDE54;
	for (bool running = true; running;)
	{
		switch (at)
		{
		case 0xDE54:                                   // the next stream counts down
			T.r[5] = 0x0ECA;
			fl::mov(T.r[5]);
			tick(0xDE58, 18);
			T.r[5] = fl::add(T.r[5], T.r[4]);
			tick(0xDE5A, 12);
			pokeb(T.r[5], fl::decb(peekb(T.r[5])));
			tick(0xDE5C, 21);
			counter_word();
			fl::movb(peekb(T.r[6]));
			tick(0xDE5E, 27);
			pokeb((uint16_t)(T.r[6] + 1), 0);
			fl::clr();
			tick(0xDE62, 30);
			fl::tst(rw(T.r[6]));
			tick(0xDE64, 18);
			if (rw(T.r[6]))
			{
				tick(0xDEE8, 12);                      // BNE: still counting
				at = 0xDEE8;
			}
			else
			{
				tick(0xDE66, 12);
				at = 0xDE66;
			}
			break;
		case 0xDE66:                                   // the stream's next entry
		{
			T.r[3] = T.r[4];
			fl::mov(T.r[3]);
			tick(0xDE68, 12);
			T.r[3] = fl_asl(T.r[3]);
			tick(0xDE6A, 12);
			T.r[2] = 0x0EC2;
			fl::mov(T.r[2]);
			tick(0xDE6E, 18);
			T.r[2] = fl::add(T.r[2], T.r[3]);          // its pointer
			tick(0xDE70, 12);
			poke(T.r[2], fl::inc(peek(T.r[2])));
			tick(0xDE72, 21);
			T.r[1] = 0;
			fl::clr();
			tick(0xDE74, 12);
			T.r[1] = fl_logicb(peekb(peek(T.r[2])));  // the count
			tick(0xDE78, 33);
			poke(T.r[2], fl::inc(peek(T.r[2])));
			tick(0xDE7A, 21);
			fl::cmp(T.r[1], 0x00FF);
			tick(0xDE7E, 18);
			if (T.r[1] != 0x00FF)
			{
				tick(0xDEB8, 12);                      // BNE
				at = 0xDEB8;
				break;
			}
			// 0xFF n: back n bytes, the variable reloaded from the word there
			tick(0xDE80, 12);
			T.r[0] = 0;
			fl::clr();
			tick(0xDE82, 12);
			T.r[0] = fl_logicb(peekb(peek(T.r[2])));
			tick(0xDE86, 33);
			poke(T.r[2], fl::sub(peek(T.r[2]), T.r[0]));
			tick(0xDE88, 21);
			{
				const uint16_t v = peek(peek(T.r[2]));
				poke(peek((uint16_t)(0xFE06 + T.r[3])), v);
				fl::mov(v);
			}
			tick(0xDE8E, 57);
			poke(T.r[2], fl::add(peek(T.r[2]), 0x0002));
			tick(0xDE92, 27);
			fl::tst(T.r[4]);
			tick(0xDE94, 12);
			if (!T.r[4])
			{
				// stream 0 (x): one of four lanes
				tick(0xDE96, 12);
				jsr(0xDE9A, 0xDDFE);                   // lane_choice
				T.r[0] = fl_asl(T.r[0]);
				tick(0xDE9C, 12);
				poke(0x0224, fl::add(peek(0x0224), peek((uint16_t)(0xFDB2 + T.r[0]))));
				tick(0xDEA2, 42);
				tick(0xDEB0, 12);                      // BR
			}
			else
			{
				tick(0xDEA4, 12);
				fl::cmp(T.r[4], 0x0002);
				tick(0xDEA8, 18);
				if (T.r[4] != 2)
					tick(0xDEB0, 12);                  // BNE
				else
				{
					tick(0xDEAA, 12);
					poke(0x03BA, fl::add(peek(0x03BA), 0x00A2));
					tick(0xDEB0, 33);
				}
			}
			T.r[1] = 0;
			fl::clr();
			tick(0xDEB2, 12);
			T.r[1] = fl_logicb(peekb(peek(T.r[2])));  // the count after the jump
			tick(0xDEB6, 33);
			poke(T.r[2], fl::inc(peek(T.r[2])));
			tick(0xDEB8, 21);
			at = 0xDEB8;
			break;
		}
		case 0xDEB8:
			pokeb(T.r[5], (uint8_t)T.r[1]);            // the new count
			fl::movb((uint8_t)T.r[1]);
			tick(0xDEBA, 21);
			fl::cmp(T.r[4], 0x0001);
			tick(0xDEBE, 18);
			if ((int16_t)T.r[4] <= 1)
			{
				tick(0xDEEE, 12);                      // BLE: streams 0 / 1
				at = 0xDEEE;
				break;
			}
			tick(0xDEC0, 12);
			T.r[1] = sext8(peekb(peek(T.r[2])));       // the value
			fl::movb((uint8_t)T.r[1]);
			tick(0xDEC4, 33);
			T.r[0] = peek((uint16_t)(0xFE06 + T.r[3])); // the variable
			fl::mov(T.r[0]);
			tick(0xDEC8, 27);
			{
				const uint8_t n = peekb(T.r[5]);
				fl::tstb(n);
				tick(0xDECA, 18);
				if (n)
				{
					// counting: the pointer advances by value words
					tick(0xDEE4, 12);                  // BNE
					T.r[1] = fl_asl(T.r[1]);
					tick(0xDEE6, 12);
					poke(T.r[0], fl::add(peek(T.r[0]), T.r[1]));
					tick(0xDEE8, 21);
					at = 0xDEE8;
					break;
				}
			}
			// count 0: the pointer is set outright
			tick(0xDECC, 12);
			pokeb(T.r[0], (uint8_t)T.r[1]);
			fl::movb((uint8_t)T.r[1]);
			tick(0xDECE, 21);
			poke(T.r[2], fl::inc(peek(T.r[2])));
			tick(0xDED0, 21);
			{
				const uint8_t hi = peekb(peek(T.r[2]));
				pokeb((uint16_t)(T.r[0] + 1), hi);
				fl::movb(hi);
			}
			tick(0xDED6, 51);
			fl::cmp(T.r[4], 0x0002);
			tick(0xDEDA, 18);
			if (T.r[4] != 2)
				tick(0xDEE8, 12);                      // BNE
			else
			{
				tick(0xDEDC, 12);
				poke(0x03BA, fl::add(peek(0x03BA), 0x00A2));
				tick(0xDEE2, 33);
				tick(0xDEE8, 12);                      // BR
			}
			at = 0xDEE8;
			break;
		case 0xDEE8:
			fl::cmp(T.r[4], 0x0001);
			tick(0xDEEC, 18);
			if ((int16_t)T.r[4] > 1)
			{
				tick(0xDF00, 12);                      // BGT
				at = 0xDF00;
				break;
			}
			tick(0xDEEE, 12);
			at = 0xDEEE;
			break;
		case 0xDEEE:                                   // streams 0 / 1: x / y += value * 8
			T.r[0] = T.r[4];
			fl::mov(T.r[0]);
			tick(0xDEF0, 12);
			T.r[0] = fl_asl(T.r[0]);
			tick(0xDEF2, 12);
			T.r[1] = sext8(peekb(peek((uint16_t)(0x0EC2 + T.r[0]))));
			fl::movb((uint8_t)T.r[1]);
			tick(0xDEF6, 33);
			T.r[1] = fl_asl(T.r[1]);
			tick(0xDEF8, 12);
			T.r[1] = fl_asl(T.r[1]);
			tick(0xDEFA, 12);
			T.r[1] = fl_asl(T.r[1]);
			tick(0xDEFC, 12);
			{
				const uint16_t v = peek((uint16_t)(0xFE06 + T.r[0]));
				poke(v, fl::add(peek(v), T.r[1]));
			}
			tick(0xDF00, 36);
			at = 0xDF00;
			break;
		case 0xDF00:
			counter_word();
			fl::movb(peekb(T.r[6]));
			tick(0xDF02, 27);
			pokeb((uint16_t)(T.r[6] + 1), 0);
			fl::clr();
			tick(0xDF06, 30);
			fl::tst(rw(T.r[6]));
			tick(0xDF08, 18);
			if (!rw(T.r[6]))
			{
				tick(0xDE66, 12);                      // BEQ: the next entry now
				at = 0xDE66;
				break;
			}
			tick(0xDF0A, 12);
			T.r[4] = fl::inc(T.r[4]);
			tick(0xDF0C, 12);
			fl::cmp(T.r[4], 0x0003);
			tick(0xDF10, 18);
			if ((int16_t)T.r[4] <= 3)
			{
				tick(0xDE54, 12);                      // BLE
				at = 0xDE54;
			}
			else
			{
				tick(0xDF12, 12);
				running = false;
			}
			break;
		}
	}
	// objects 5 and 6 follow
	T.r[0] = peek(0x0224);
	fl::mov(T.r[0]);
	tick(0xDF16, 24);
	poke(0x06FA, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDF1A, 27);
	poke(0x06FC, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDF1E, 27);
	T.r[0] = peek(0x0226);
	fl::mov(T.r[0]);
	tick(0xDF22, 24);
	poke(0x073A, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDF26, 27);
	poke(0x073C, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDF2A, 27);
	pokeb(0x0495, fl_logicb((uint8_t)(peekb(0x0495) | 0x18)));
	tick(0xDF30, 33);
	pokeb(0x0496, fl_logicb((uint8_t)(peekb(0x0496) | 0x18)));
	tick(0xDF36, 33);
	poke(0x043A, 0x0001);
	fl::mov(0x0001);
	tick(0xDF3C, 33);
	drop(0xDF3C);
	c_restore(4);
	return T.r[0];
}

// F:DF40  start_object_scripts(int *y, int *x): pick the script set of the current slot
// (F:BE10; 0x09B6 indexes the bytes at 0x4000, bank 0x1000) and the park (0x0F2E -> 0xFDE3),
// start the four streams, place objects 5 and 6 and return their position through x and y;
// objects 5/6 are set up and F:B4B6(6) runs
uint16_t start_object_scripts(const Args &)
{
	c_save(2, 0xDF44);
	bank_a(0xDF44, 0x1000);
	bank_b(0xDF50, 0x1400);
	jsr(0xDF60, 0xBE10);                               // locate_map_object
	T.r[0] = peek(0x09B6);
	fl::mov(T.r[0]);
	tick(0xDF64, 24);
	T.r[0] = sext8(peekb((uint16_t)(0x4000 + T.r[0])));
	fl::movb((uint8_t)T.r[0]);
	tick(0xDF68, 27);
	T.r[0] = fl_logic(T.r[0] & 0x000F);                // its script set
	tick(0xDF6C, 18);
	T.r[1] = 0x2400;
	fl::mov(T.r[1]);
	tick(0xDF70, 18);
	poke(0x01D2, T.r[1]);
	fl::mov(T.r[1]);
	tick(0xDF74, 27);
	poke(0x1400, T.r[1]);
	fl::mov(T.r[1]);
	tick(0xDF78, 27);
	T.r[1] = 0x2800;
	fl::mov(T.r[1]);
	tick(0xDF7C, 18);
	poke(0x01D4, T.r[1]);
	fl::mov(T.r[1]);
	tick(0xDF80, 27);
	poke(0x1402, T.r[1]);
	fl::mov(T.r[1]);
	tick(0xDF84, 27);
	T.r[1] = T.r[0];
	fl::mov(T.r[1]);
	tick(0xDF86, 12);
	T.r[1] = fl_asl(T.r[1]);
	tick(0xDF88, 12);
	T.r[0] = peek((uint16_t)(0x4652 + T.r[1]));
	fl::mov(T.r[0]);
	tick(0xDF8C, 27);
	T.r[1] = 0;
	fl::clr();
	tick(0xDF8E, 12);
	T.r[1] = fl_logicb(peekb(0x0F2E));
	tick(0xDF92, 24);
	T.r[1] = sext8(peekb((uint16_t)(0xFDE3 + T.r[1])));   // the stage's group
	fl::movb((uint8_t)T.r[1]);
	tick(0xDF96, 27);
	T.r[1] = fl_logic(T.r[1] & 0x00FF);
	tick(0xDF9A, 18);
	T.r[1] = fl_asl(T.r[1]);
	tick(0xDF9C, 12);
	T.r[1] = fl_asl(T.r[1]);
	tick(0xDF9E, 12);
	T.r[1] = fl_asl(T.r[1]);
	tick(0xDFA0, 12);
	T.r[1] = fl::add(T.r[1], T.r[0]);
	tick(0xDFA2, 12);
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0xDFA4, 12);
	T.r[0] = fl::sub(T.r[0], 0x0008);                  // its four stream addresses
	tick(0xDFA8, 18);
	T.r[3] = 0;
	fl::clr();
	tick(0xDFAA, 12);
	for (;;)                                           // the streams start
	{
		T.r[1] = T.r[3];
		fl::mov(T.r[1]);
		tick(0xDFAC, 12);
		T.r[1] = fl_asl(T.r[1]);
		tick(0xDFAE, 12);
		T.r[2] = 0x0EC2;
		fl::mov(T.r[2]);
		tick(0xDFB2, 18);
		T.r[2] = fl::add(T.r[2], T.r[1]);
		tick(0xDFB4, 12);
		{
			const uint16_t p = peek(T.r[0]);
			T.r[0] = (uint16_t)(T.r[0] + 2);
			poke(T.r[2], p);
			fl::mov(p);
		}
		tick(0xDFB6, 27);
		{
			const uint16_t v = peek(peek(T.r[2]));    // the variable's first value
			poke(peek((uint16_t)(0xFE06 + T.r[1])), v);
			fl::mov(v);
		}
		tick(0xDFBC, 57);
		poke(T.r[2], fl::inc(peek(T.r[2])));
		tick(0xDFBE, 21);
		pokeb((uint16_t)(0x0ECA + T.r[3]), 0x01);
		fl::movb(0x01);
		tick(0xDFC4, 36);
		T.r[3] = fl::inc(T.r[3]);
		tick(0xDFC6, 12);
		fl::cmp(T.r[3], 0x0003);
		tick(0xDFCA, 18);
		if ((int16_t)T.r[3] > 3)
		{
			tick(0xDFCC, 12);                          // BLE not taken
			break;
		}
		tick(0xDFAA, 12);
	}
	poke(0x03BA, fl::add(peek(0x03BA), 0x00A2));
	tick(0xDFD2, 33);
	jsr(0xDFD6, 0xDDFE);                               // lane_choice
	T.r[0] = fl_asl(T.r[0]);
	tick(0xDFD8, 12);
	T.r[1] = peek(0x0224);
	fl::mov(T.r[1]);
	tick(0xDFDC, 24);
	T.r[1] = fl::add(T.r[1], peek((uint16_t)(0xFDB2 + T.r[0])));   // x on its lane
	tick(0xDFE0, 27);
	poke(0x06FA, T.r[1]);
	fl::mov(T.r[1]);
	tick(0xDFE4, 27);
	poke(0x06FC, T.r[1]);
	fl::mov(T.r[1]);
	tick(0xDFE8, 27);
	T.r[0] = peek(0x0226);
	fl::mov(T.r[0]);
	tick(0xDFEC, 24);
	poke(0x073A, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDFF0, 27);
	poke(0x073C, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xDFF4, 27);
	poke(rw(sp_at(0x0C)), T.r[1]);                     // *x
	fl::mov(T.r[1]);
	tick(0xDFF8, 36);
	poke(rw(sp_at(0x0A)), T.r[0]);                     // *y
	fl::mov(T.r[0]);
	tick(0xDFFC, 36);
	pokeb(0x0475, 0xF0);                               // objects 5 / 6 set up
	fl::movb(0xF0);
	tick(0xE002, 33);
	pokeb(0x0476, 0xF0);
	fl::movb(0xF0);
	tick(0xE008, 33);
	poke(0x03FA, 0);
	fl::clr();
	tick(0xE00C, 27);
	poke(0x03FC, 0x7114);
	fl::mov(0x7114);
	tick(0xE012, 33);
	pokeb(0x0376, 0x02);
	fl::movb(0x02);
	tick(0xE018, 33);
	poke(0x043C, 0);
	fl::clr();
	tick(0xE01C, 27);
	poke(0x043A, 0x0001);
	fl::mov(0x0001);
	tick(0xE022, 33);
	push(0x0006);
	fl::mov(0x0006);
	tick(0xE026, 30);
	jsr(0xE02A, 0xB4B6);                               // show_object(6)
	drop(0xE02A);
	c_restore(2);
	return T.r[0];
}

// F:E036: in state 1 (0x018F) with 0x031F and 0x0377 clear, 0x0EB4 counts down; at 0
// F:CE24(0, 0, 4) and F:C4E4 run, object 7 is set up, 0x0F22 = 250, the counter 0x0EB3 steps
// (0..5) and A:5018 (bank 0x3800) runs with the same arguments
uint16_t countdown_0eb4(const Args &)
{
	const uint8_t state = peekb(0x018F);
	fl::cmpb(state, 0x01);
	tick(0xE03C, 30);
	if (state != 1)
	{
		tick(0xE0C0, 12);                              // BNE
		rts();
		return T.r[0];
	}
	tick(0xE03E, 12);
	const uint8_t phase = peekb(0x031F);
	fl::tstb(phase);
	tick(0xE042, 24);
	if (phase)
	{
		tick(0xE0C0, 12);                              // BNE
		rts();
		return T.r[0];
	}
	tick(0xE044, 12);
	const uint8_t seven = peekb(0x0377);
	fl::tstb(seven);
	tick(0xE048, 24);
	if (seven)
	{
		tick(0xE0C0, 12);                              // BNE: the chaser is out
		rts();
		return T.r[0];
	}
	tick(0xE04A, 12);
	const uint16_t left = fl::dec(peek(0x0EB4));
	poke(0x0EB4, left);
	tick(0xE04E, 27);
	if (left)
	{
		tick(0xE0C0, 12);                              // BNE
		rts();
		return T.r[0];
	}
	// time's up: the chaser (type 4) comes out as object 7
	tick(0xE050, 12);
	push(0x0004);
	fl::mov(0x0004);
	tick(0xE054, 30);
	push(0);
	fl::clr();
	tick(0xE056, 24);
	push(0);
	fl::clr();
	tick(0xE058, 24);
	jsr(0xE05C, 0xCE24);                               // add_map_object(0, 0, 4)
	jsr(0xE060, 0xC4E4);                               // create_map_object
	pokeb(0x0477, fl_logicb((uint8_t)(peekb(0x0477) | 0xC0)));
	tick(0xE066, 33);
	T.r[0] = sext8(peekb(0x0777));
	fl::movb((uint8_t)T.r[0]);
	tick(0xE06A, 24);
	T.r[0] = fl_logic(T.r[0] & 0x007F);
	tick(0xE06E, 18);
	pokeb(0x0777, 0);
	fl::clr();
	tick(0xE072, 27);
	pokeb(0x0777, fl_logicb((uint8_t)(peekb(0x0777) | (uint8_t)T.r[0])));
	tick(0xE076, 27);
	pokeb(0x07D7, 0xFF);
	fl::movb(0xFF);
	tick(0xE07C, 33);
	pokeb(0x0EB2, 0x01);                               // speed level 1
	fl::movb(0x01);
	tick(0xE082, 33);
	poke(0x0EB4, 0);
	fl::clr();
	tick(0xE086, 27);
	poke(0x0F22, 0x00FA);                              // the speed-up clock
	fl::mov(0x00FA);
	tick(0xE08C, 33);
	T.r[0] = sext8(peekb(0x0EB3));                     // the shape counter steps
	fl::movb((uint8_t)T.r[0]);
	tick(0xE090, 24);
	T.r[0] = fl::inc(T.r[0]);
	tick(0xE092, 12);
	pokeb(0x0EB3, (uint8_t)T.r[0]);
	fl::movb((uint8_t)T.r[0]);
	tick(0xE096, 27);
	fl::cmpb((uint8_t)T.r[0], 0x05);
	tick(0xE09A, 18);
	if ((int8_t)T.r[0] <= 5)
		tick(0xE0A0, 12);                              // BLE
	else
	{
		tick(0xE09C, 12);
		pokeb(0x0EB3, 0);
		fl::clr();
		tick(0xE0A0, 27);
	}
	bank_a(0xE0A0, 0x3800);
	bank_b(0xE0AC, 0x3C00);
	jsr(0xE0BC, 0x5018);                               // chaser_step, with the same arguments
	T.r[6] = fl::add(T.r[6], 0x0006);
	tick(0xE0C0, 18);
	rts();
	return T.r[0];
}

// F:E13A  set_object7_motion(int vx0, int vy0, int col, int row): object 7 velocity (0x04FE,
// 0x057E) from the 8-column tables at 0x4138 / 0x4238 (bank A 0x2400), doubled, plus vx0 /
// vy0; a new row (0x07D7) also picks its animation 0x03BE (0x5238, or 0x527E by 0xFE18
// [0x0EB3] for col >= 4, bank A 0) and the bytes 0x0777 / 0x0797 = 4.  Banks end as
// 0x3800 / 0x3C00.
uint16_t set_object7_motion(const Args &)
{
	c_save(1, 0xE13E);
	bank_a(0xE13E, 0x2400);
	T.r[2] = rw(sp_at(0x0E));                          // the row (speed level)
	fl::mov(T.r[2]);
	tick(0xE14E, 27);
	T.r[0] = T.r[2];
	fl::mov(T.r[0]);
	tick(0xE150, 12);
	T.r[0] = fl_asl(T.r[0]);
	tick(0xE152, 12);
	T.r[0] = fl_asl(T.r[0]);
	tick(0xE154, 12);
	T.r[0] = fl_asl(T.r[0]);
	tick(0xE156, 12);
	T.r[0] = fl::add(T.r[0], rw(sp_at(0x0C)));         // + the column (direction)
	tick(0xE15A, 27);
	T.r[0] = fl_asl(T.r[0]);
	tick(0xE15C, 12);
	T.r[1] = peek((uint16_t)(0x4138 + T.r[0]));
	fl::mov(T.r[1]);
	tick(0xE160, 27);
	T.r[1] = fl_asl(T.r[1]);
	tick(0xE162, 12);
	T.r[1] = fl::add(T.r[1], rw(sp_at(0x0A)));
	tick(0xE166, 27);
	poke(0x04FE, T.r[1]);                              // object 7's x speed
	fl::mov(T.r[1]);
	tick(0xE16A, 27);
	T.r[0] = peek((uint16_t)(0x4238 + T.r[0]));
	fl::mov(T.r[0]);
	tick(0xE16E, 27);
	T.r[0] = fl_asl(T.r[0]);
	tick(0xE170, 12);
	T.r[0] = fl::add(T.r[0], rw(sp_at(0x08)));
	tick(0xE174, 27);
	poke(0x057E, T.r[0]);                              // and y speed
	fl::mov(T.r[0]);
	tick(0xE178, 27);
	T.r[0] = 0;
	fl::clr();
	tick(0xE17A, 12);
	T.r[0] = fl_logicb(peekb(0x07D7));
	tick(0xE17E, 24);
	fl::cmp(T.r[0], T.r[2]);
	tick(0xE180, 12);
	if (T.r[0] == T.r[2])
		tick(0xE1E0, 12);                              // BEQ: same row
	else
	{
		// a new row: its animation (from speed level 4 on the swarm's shapes, 0xFE18[0x0EB3])
		tick(0xE182, 12);
		type_banks(0xE182, 0);
		fl::cmp(rw(sp_at(0x0C)), 0x0004);
		tick(0xE19C, 33);
		if ((int16_t)rw(sp_at(0x0C)) >= 4)
		{
			tick(0xE1A2, 12);                          // BGE
			T.r[0] = sext8(peekb(0x0EB3));
			fl::movb((uint8_t)T.r[0]);
			tick(0xE1A6, 24);
			T.r[1] = 0;
			fl::clr();
			tick(0xE1A8, 12);
			T.r[1] = fl_logicb(peekb((uint16_t)(0xFE18 + T.r[0])));
			tick(0xE1AC, 27);
		}
		else
		{
			tick(0xE19E, 12);
			T.r[1] = 0;
			fl::clr();
			tick(0xE1A0, 12);
			tick(0xE1AC, 12);                          // BR
		}
		fl::cmp(T.r[1], 0x0005);
		tick(0xE1B0, 18);
		if ((int16_t)T.r[1] >= 5)
		{
			tick(0xE1C8, 12);                          // BGE
			T.r[1] = fl_asl(T.r[1]);
			tick(0xE1CA, 12);
			{
				const uint16_t anim = peek((uint16_t)(0x527E + T.r[1]));
				poke(0x03BE, anim);
				fl::mov(anim);
			}
			tick(0xE1D0, 42);
		}
		else
		{
			tick(0xE1B2, 12);
			T.r[0] = T.r[2];
			fl::mov(T.r[0]);
			tick(0xE1B4, 12);
			T.r[0] = fl_asr(T.r[0]);
			tick(0xE1B6, 12);
			T.r[1] = fl_asl(T.r[1]);
			tick(0xE1B8, 12);
			T.r[1] = fl_asl(T.r[1]);
			tick(0xE1BA, 12);
			T.r[1] = fl_asl(T.r[1]);
			tick(0xE1BC, 12);
			T.r[0] = fl::add(T.r[0], T.r[1]);
			tick(0xE1BE, 12);
			T.r[0] = fl_asl(T.r[0]);
			tick(0xE1C0, 12);
			{
				const uint16_t anim = peek((uint16_t)(0x5238 + T.r[0]));
				poke(0x03BE, anim);
				fl::mov(anim);
			}
			tick(0xE1C6, 42);
			tick(0xE1D0, 12);                          // BR
		}
		T.r[0] = 0x0004;
		fl::mov(T.r[0]);
		tick(0xE1D4, 18);
		pokeb(0x0797, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0xE1D8, 27);
		pokeb(0x0777, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0xE1DC, 27);
		pokeb(0x07D7, (uint8_t)T.r[2]);
		fl::movb((uint8_t)T.r[2]);
		tick(0xE1E0, 27);
	}
	bank_a(0xE1E0, 0x3800);
	bank_b(0xE1EC, 0x3C00);
	c_restore(1);
	return T.r[0];
}

// F:E5D2  skater_anim_a(int t): animation pointer for terrain t: 0x4F64 + 2 * 0x01F8 when
// t & 7 is 0, else 0x4C4C[t >> 3] + 0xFE2D[t & 7] + 2 * (0x01F8 & 7) (bank B 0x0400)
uint16_t skater_anim_a(const Args &)
{
	c_save(4, 0xE5D6);
	banks_save(0xE5D6, 3, 4);
	type_banks(0xE5DE, 0);
	T.r[0] = sext8(peekb(0x01F8));                     // the board's direction (of 8)
	fl::movb((uint8_t)T.r[0]);
	tick(0xE5F6, 24);
	T.r[0] = fl_logic(T.r[0] & 0x0007);
	tick(0xE5FA, 18);
	T.r[1] = rw(sp_at(0x0E));                          // the terrain
	fl::mov(T.r[1]);
	tick(0xE5FE, 27);
	const bool slope = fl_logic(T.r[1] & 0x0007) != 0;
	tick(0xE602, 18);
	if (!slope)
	{
		tick(0xE604, 12);
		T.r[2] = sext8(peekb(0x01F8));
		fl::movb((uint8_t)T.r[2]);
		tick(0xE608, 24);
		T.r[2] = fl_asl(T.r[2]);
		tick(0xE60A, 12);
		T.r[2] = fl::add(T.r[2], 0x4F64);
		tick(0xE60E, 18);
		tick(0xE630, 12);                              // BR
	}
	else
	{
		tick(0xE610, 12);                              // BNE
		T.r[2] = T.r[1];
		fl::mov(T.r[2]);
		tick(0xE612, 12);
		T.r[2] = fl_asr(T.r[2]);
		tick(0xE614, 12);
		T.r[2] = fl_asr(T.r[2]);
		tick(0xE616, 12);
		T.r[2] = fl_asr(T.r[2]);
		tick(0xE618, 12);
		T.r[2] = fl_asl(T.r[2]);
		tick(0xE61A, 12);
		T.r[1] = fl_logic(T.r[1] & 0x0007);
		tick(0xE61E, 18);
		T.r[2] = peek((uint16_t)(0x4C4C + T.r[2]));    // the height's table
		fl::mov(T.r[2]);
		tick(0xE622, 27);
		T.r[5] = 0;
		fl::clr();
		tick(0xE624, 12);
		T.r[5] = fl_logicb(peekb((uint16_t)(0xFE2D + T.r[1])));   // the slope's offset
		tick(0xE628, 27);
		T.r[2] = fl::add(T.r[2], T.r[5]);
		tick(0xE62A, 12);
		T.r[1] = T.r[0];
		fl::mov(T.r[1]);
		tick(0xE62C, 12);
		T.r[1] = fl_asl(T.r[1]);
		tick(0xE62E, 12);
		T.r[2] = fl::add(T.r[2], T.r[1]);
		tick(0xE630, 12);
	}
	T.r[0] = T.r[2];
	fl::mov(T.r[0]);
	tick(0xE632, 12);
	banks_back(0xE632, 3, 4);
	c_restore(4);
	return T.r[0];
}

// F:E648  skater_anim_b(int t): like F:E5D2 with the tables 0x4000, 0x4020 and 0xFE35; on
// terrain 2 / 10 (0x0327) at speed 0x01B4 >= 0x500 the tables 0x43E8 / 0x4408 are used
uint16_t skater_anim_b(const Args &)
{
	c_save(4, 0xE64C);
	T.r[5] = peek(0x01D2);                             // the banks, put back at the end
	fl::mov(T.r[5]);
	tick(0xE650, 24);
	{
		const uint16_t v = peek(0x01D4);
		fl::mov(v);
		push(v);
	}
	tick(0xE654, 36);
	type_banks(0xE654, 0);
	T.r[3] = sext8(peekb(0x01F8));                     // the board's direction
	fl::movb((uint8_t)T.r[3]);
	tick(0xE66C, 24);
	T.r[4] = rw(sp_at(0x10));                          // the terrain
	fl::mov(T.r[4]);
	tick(0xE670, 27);
	const bool slope = fl_logic(T.r[4] & 0x0007) != 0;
	tick(0xE674, 18);
	if (!slope)
	{
		tick(0xE676, 12);
		T.r[0] = T.r[3];
		fl::mov(T.r[0]);
		tick(0xE678, 12);
		T.r[0] = fl_asl(T.r[0]);
		tick(0xE67A, 12);
		T.r[0] = fl::add(T.r[0], 0x4000);
		tick(0xE67E, 18);
		tick(0xE6DA, 12);                              // BR
	}
	else
	{
		tick(0xE680, 12);                              // BNE
		T.r[2] = 0;
		fl::clr();
		tick(0xE682, 12);
		const uint8_t t2 = peekb(0x0327);
		fl::cmpb(t2, 0x02);
		tick(0xE688, 30);
		bool check_speed;
		if (t2 == 2)
		{
			tick(0xE68A, 12);
			T.r[2] = fl::inc(T.r[2]);
			tick(0xE68C, 12);
			tick(0xE696, 12);                          // BR
			check_speed = true;
		}
		else
		{
			tick(0xE68E, 12);                          // BNE
			const uint8_t t10 = peekb(0x0327);
			fl::cmpb(t10, 0x0A);
			tick(0xE694, 30);
			check_speed = t10 == 0x0A;
			tick(check_speed ? 0xE696 : 0xE6B6, 12);   // BNE
		}
		bool fast = false;
		if (check_speed)
		{
			const uint16_t speed = peek(0x01B4);
			fl::cmp(speed, 0x0500);
			tick(0xE69C, 30);
			fast = (int16_t)speed >= 0x0500;
			tick(fast ? 0xE69E : 0xE6B6, 12);          // BLT
		}
		if (fast)
		{
			// terrains 2 / 10 at speed: the tables 0x43E8 / 0x4408
			T.r[1] = T.r[3];
			fl::mov(T.r[1]);
			tick(0xE6A0, 12);
			T.r[1] = fl_asl(T.r[1]);
			tick(0xE6A2, 12);
			T.r[2] = fl_ror(T.r[2]);
			tick(0xE6A4, 12);
			if (C())
			{
				tick(0xE6A6, 12);
				T.r[0] = T.r[1];
				fl::mov(T.r[0]);
				tick(0xE6A8, 12);
				T.r[0] = fl::add(T.r[0], 0x43E8);
				tick(0xE6AC, 18);
				tick(0xE6DA, 12);                      // BR
			}
			else
			{
				tick(0xE6AE, 12);                      // BCC
				T.r[0] = T.r[1];
				fl::mov(T.r[0]);
				tick(0xE6B0, 12);
				T.r[0] = fl::add(T.r[0], 0x4408);
				tick(0xE6B4, 18);
				tick(0xE6DA, 12);                      // BR
			}
		}
		else
		{
			T.r[2] = T.r[4];
			fl::mov(T.r[2]);
			tick(0xE6B8, 12);
			T.r[2] = fl_asr(T.r[2]);
			tick(0xE6BA, 12);
			T.r[2] = fl_asr(T.r[2]);
			tick(0xE6BC, 12);
			T.r[2] = fl_asr(T.r[2]);
			tick(0xE6BE, 12);
			T.r[2] = fl_asl(T.r[2]);
			tick(0xE6C0, 12);
			T.r[1] = T.r[4];
			fl::mov(T.r[1]);
			tick(0xE6C2, 12);
			T.r[1] = fl_logic(T.r[1] & 0x0007);
			tick(0xE6C6, 18);
			T.r[2] = peek((uint16_t)(0x4020 + T.r[2]));
			fl::mov(T.r[2]);
			tick(0xE6CA, 27);
			T.r[4] = 0;
			fl::clr();
			tick(0xE6CC, 12);
			T.r[4] = fl_logicb(peekb((uint16_t)(0xFE35 + T.r[1])));
			tick(0xE6D0, 27);
			T.r[2] = fl::add(T.r[2], T.r[4]);
			tick(0xE6D2, 12);
			T.r[1] = T.r[3];
			fl::mov(T.r[1]);
			tick(0xE6D4, 12);
			T.r[1] = fl_asl(T.r[1]);
			tick(0xE6D6, 12);
			T.r[2] = fl::add(T.r[2], T.r[1]);
			tick(0xE6D8, 12);
			T.r[0] = T.r[2];
			fl::mov(T.r[0]);
			tick(0xE6DA, 12);
		}
	}
	poke(0x01D2, T.r[5]);
	fl::mov(T.r[5]);
	tick(0xE6DE, 27);
	{
		const uint16_t v = pop();
		fl::mov(v);
		poke(0x01D4, v);
	}
	tick(0xE6E2, 33);
	{
		const uint16_t v = peek(0x01D2);
		fl::mov(v);
		poke(0x1400, v);
	}
	tick(0xE6E8, 39);
	{
		const uint16_t v = peek(0x01D4);
		fl::mov(v);
		poke(0x1402, v);
	}
	tick(0xE6EE, 39);
	c_restore(4);
	return T.r[0];
}

// F:EF24  add_score(int n): the score (long at 0x023E) grows by n; each time it reaches the
// next bonus threshold (long at 0x0244) a bonus is counted in 0x025A (up to 4), the next
// threshold is the step 0x5212[0x025A + 0x0FE3] (bank 0x3800) further, and, outside state 5,
// B:72CA(1) announces it
uint16_t add_score(const Args &)
{
	c_save(4, 0xEF28);
	{
		const uint16_t v = peek(0x01D2);               // the banks, put back at the end
		fl::mov(v);
		push(v);
	}
	tick(0xEF2C, 36);
	{
		const uint16_t v = peek(0x01D4);
		fl::mov(v);
		push(v);
	}
	tick(0xEF30, 36);
	pokeb(0x0FA2, fl_logicb((uint8_t)(peekb(0x0FA2) | 0x01)));
	tick(0xEF36, 33);
	T.r[1] = 0x0240;                                   // the score (long at 0x023E) + n
	fl::mov(T.r[1]);
	tick(0xEF3A, 18);
	T.r[2] = 0x023E;
	fl::mov(T.r[2]);
	tick(0xEF3E, 18);
	T.r[3] = 0;
	fl::clr();
	tick(0xEF40, 12);
	T.r[4] = rw(sp_at(0x12));
	fl::mov(T.r[4]);
	tick(0xEF44, 27);
	jsr(0xEF48, 0x80C8);                               // add32
	{
		const uint16_t v = peek(0x0246);               // 0x0248: the bonus threshold (0x0244)
		fl::mov(v);
		poke(0x024A, v);
	}
	tick(0xEF4E, 39);
	{
		const uint16_t v = peek(0x0244);
		fl::mov(v);
		poke(0x0248, v);
	}
	tick(0xEF54, 39);
	for (;;)
	{
		T.r[1] = 0x024A;                               // less the score
		fl::mov(T.r[1]);
		tick(0xEF58, 18);
		T.r[2] = 0x0248;
		fl::mov(T.r[2]);
		tick(0xEF5C, 18);
		T.r[3] = peek(0x0240);
		fl::mov(T.r[3]);
		tick(0xEF60, 24);
		T.r[4] = peek(0x023E);
		fl::mov(T.r[4]);
		tick(0xEF64, 24);
		jsr(0xEF68, 0x80D0);                           // sub32
		T.r[0] = peek(0x024A);
		fl::mov(T.r[0]);
		tick(0xEF6C, 24);
		if ((int16_t)T.r[0] >= 0)
		{
			tick(0xEF6E, 12);
			if (T.r[0])
			{
				tick(0xEFE8, 12);                      // BNE: still below the threshold
				break;
			}
			tick(0xEF70, 12);
			const uint16_t low = peek(0x0248);
			fl::tst(low);
			tick(0xEF74, 24);
			if (low)
			{
				tick(0xEFE8, 12);                      // BNE
				break;
			}
			tick(0xEF76, 12);
		}
		else
			tick(0xEF76, 12);                          // BLT
		// a bonus: counted in 0x025A (up to 4), the next threshold further
		pokeb(0x0FA2, fl_logicb((uint8_t)(peekb(0x0FA2) | 0x02)));
		tick(0xEF7C, 33);
		fl::cmpb(peekb(0x025A), 0x04);
		tick(0xEF82, 30);
		{
			const uint8_t v = peekb(0x025A);           // ADCB: + 1 below 4
			const unsigned c = C();
			const uint8_t r = (uint8_t)(v + c);
			f_nzvc(nzb(r) | ((v == 0x7F && c) ? VF : 0) | ((v == 0xFF && c) ? CF : 0));
			pokeb(0x025A, r);
		}
		tick(0xEF86, 27);
		bank_a(0xEF86, 0x3800);
		bank_b(0xEF92, 0x3C00);
		T.r[0] = 0;
		fl::clr();
		tick(0xEFA0, 12);
		T.r[0] = fl_logicb(peekb(0x025A));
		tick(0xEFA4, 24);
		T.r[1] = 0;
		fl::clr();
		tick(0xEFA6, 12);
		T.r[1] = fl_logicb(peekb(0x0FE3));
		tick(0xEFAA, 24);
		T.r[0] = fl::add(T.r[0], T.r[1]);
		tick(0xEFAC, 12);
		T.r[0] = fl_asl(T.r[0]);
		tick(0xEFAE, 12);
		T.r[1] = 0x0246;
		fl::mov(T.r[1]);
		tick(0xEFB2, 18);
		T.r[2] = 0x0244;
		fl::mov(T.r[2]);
		tick(0xEFB6, 18);
		T.r[3] = 0;
		fl::clr();
		tick(0xEFB8, 12);
		T.r[4] = peek((uint16_t)(0x5212 + T.r[0]));    // the step
		fl::mov(T.r[4]);
		tick(0xEFBC, 27);
		jsr(0xEFC0, 0x80C8);                           // add32
		{
			const uint16_t v = peek(0x0246);
			fl::mov(v);
			poke(0x024A, v);
		}
		tick(0xEFC6, 39);
		{
			const uint16_t v = peek(0x0244);
			fl::mov(v);
			poke(0x0248, v);
		}
		tick(0xEFCC, 39);
		const uint8_t state = peekb(0x018F);
		fl::cmpb(state, 0x05);
		tick(0xEFD2, 30);
		if (state == 5)
		{
			tick(0xEF54, 12);                          // BEQ
			continue;
		}
		tick(0xEFD4, 12);
		T.r[5] = peek(0x0188);
		fl::mov(T.r[5]);
		tick(0xEFD8, 24);
		push(0x0001);
		fl::mov(0x0001);
		tick(0xEFDC, 30);
		jsr(0xEFE0, 0x72CA);                           // B:72CA(1): the announcement
		poke(0x0188, T.r[5]);
		fl::mov(T.r[5]);
		tick(0xEFE4, 27);
		drop(0xEFE4);
		tick(0xEF54, 12);                              // BR
	}
	{
		const uint16_t v = rw(sp_at(2));               // the banks back
		fl::mov(v);
		poke(0x01D2, v);
	}
	tick(0xEFEE, 42);
	{
		const uint16_t v = pop();
		fl::mov(v);
		poke(0x01D4, v);
	}
	tick(0xEFF2, 33);
	{
		const uint16_t v = peek(0x01D2);
		fl::mov(v);
		poke(0x1400, v);
	}
	tick(0xEFF8, 39);
	{
		const uint16_t v = peek(0x01D4);
		fl::mov(v);
		poke(0x1402, v);
	}
	tick(0xEFFE, 39);
	drop(0xEFFE);
	c_restore(4);
	return T.r[0];
}

// F:F064  start_object4(int kind): object 4 appears next to the skater (0x0400 right and 0x0300
// down, 0x0800 more on terrain 5-7) with kind (0..9; -1 on screen 7, negative: nothing) in
// 0x0394; if it was not active, it is activated (type 0x53) and F:B4B6(4) runs.  Returns kind.
uint16_t start_object4(const Args &)
{
	c_save(1, 0xF068);
	const uint8_t level = peekb(0x0F59);
	fl::cmpb(level, 0x07);
	tick(0xF06E, 30);
	if (level != 7)
		tick(0xF076, 12);                              // BNE
	else
	{
		tick(0xF070, 12);
		ww(sp_at(8), 0xFFFF);                          // kind -1 on screen 7
		fl::mov(0xFFFF);
		tick(0xF076, 36);
	}
	T.r[0] = rw(sp_at(8));
	fl::mov(T.r[0]);
	tick(0xF07A, 27);
	if ((int16_t)T.r[0] < 0)
		tick(0xF132, 12);                              // BLT: nothing
	else
	{
		tick(0xF07C, 12);
		fl::cmp(T.r[0], 0x0009);
		tick(0xF080, 18);
		if ((int16_t)T.r[0] <= 9)
			tick(0xF088, 12);                          // BLE
		else
		{
			tick(0xF082, 12);
			ww(sp_at(8), 0x0009);
			fl::mov(0x0009);
			tick(0xF088, 36);
		}
		poke(0x0438, 0x001E);
		fl::mov(0x001E);
		tick(0xF08E, 33);
		poke(0x0638, 0x8040);
		fl::mov(0x8040);
		tick(0xF094, 33);
		{
			const uint16_t v = peek(0x06F0);           // next to the skater
			fl::mov(v);
			poke(0x06F8, v);
		}
		tick(0xF09A, 39);
		poke(0x06F8, fl::add(peek(0x06F8), 0x0400));
		tick(0xF0A0, 33);
		{
			const uint16_t v = peek(0x0730);
			fl::mov(v);
			poke(0x0738, v);
		}
		tick(0xF0A6, 39);
		poke(0x0738, fl::add(peek(0x0738), 0x0300));
		tick(0xF0AC, 33);
		T.r[0] = sext8(peekb(0x0327));
		fl::movb((uint8_t)T.r[0]);
		tick(0xF0B0, 24);
		T.r[0] = fl_logic(T.r[0] & 0x0007);
		tick(0xF0B4, 18);
		fl::cmp(T.r[0], 0x0005);
		tick(0xF0B8, 18);
		if (T.r[0] < 5)
			tick(0xF0C0, 12);                          // BCS
		else
		{
			tick(0xF0BA, 12);                          // terrain 5-7
			poke(0x0738, fl::add(peek(0x0738), 0x0800));
			tick(0xF0C0, 33);
		}
		{
			const uint16_t v = peek(0x06B0);
			fl::mov(v);
			poke(0x06B8, v);
		}
		tick(0xF0C6, 39);
		poke(0x03B8, 0x576A);
		fl::mov(0x576A);
		tick(0xF0CC, 33);
		{
			const uint8_t v = peekb(sp_at(8));         // the kind
			fl::movb(v);
			pokeb(0x0394, v);
		}
		tick(0xF0D2, 42);
		T.r[0] = 0x0003;
		fl::mov(T.r[0]);
		tick(0xF0D6, 18);
		pokeb(0x0794, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0xF0DA, 27);
		pokeb(0x0774, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0xF0DE, 27);
		pokeb(0x07B4, 0);
		fl::clr();
		tick(0xF0E2, 27);
		poke(0x03F8, 0);
		fl::clr();
		tick(0xF0E6, 27);
		T.r[0] = 0;
		fl::clr();
		tick(0xF0E8, 12);
		T.r[0] = fl_logicb(peekb(0x0470));
		tick(0xF0EC, 24);
		T.r[0] = fl_logic(T.r[0] & 0x00C0);
		tick(0xF0F0, 18);
		pokeb(0x0474, (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0xF0F4, 27);
		pokeb(0x0474, fl_logicb((uint8_t)(peekb(0x0474) | 0x20)));
		tick(0xF0FA, 33);
		pokeb(0x0494, fl_logicb((uint8_t)(peekb(0x0494) | 0x18)));
		tick(0xF100, 33);
		const uint8_t active = peekb(0x0374);
		fl::tstb(active);
		tick(0xF104, 24);
		if (active)
			tick(0xF132, 12);                          // BNE
		else
		{
			tick(0xF106, 12);
			banks_save(0xF106, 1, 2);
			pokeb(0x0374, 0x53);                       // activated
			fl::movb(0x53);
			tick(0xF114, 33);
			push(0x0004);
			fl::mov(0x0004);
			tick(0xF118, 30);
			jsr(0xF11C, 0xB4B6);                       // show_object(4)
			banks_back(0xF11C, 1, 2);
			drop(0xF130);
		}
	}
	T.r[0] = rw(sp_at(8));
	fl::mov(T.r[0]);
	tick(0xF136, 27);
	c_restore(1);
	return T.r[0];
}

// F:D19C: set up the slots of a level: object queue state, entries (F:D8D2, F:D880),
// slots 0-3 (F:CF5E), the four fixed objects (F:CE24 from 0x70E4/0x70E8, bank B 0x0400);
// for a park (0x0F2E) above the last one set up (0x0E94) also the slots 4-0xAE whose type
// has bit 0 of 0x6438 clear, the park's objects (0x708C and the list at 0x6B9E) and the
// object counters 0x0E7E-0x0E86 (total 0x0E93) from 0x68D0; finally A:48CC (bank 0x1800)
uint16_t setup_level_slots(const Args &)
{
	c_save(4, 0xD1A0);
	{
		const uint16_t v = peek(0x01D2);               // the banks, put back at the end
		fl::mov(v);
		push(v);
	}
	tick(0xD1A4, 36);
	{
		const uint16_t v = peek(0x01D4);
		fl::mov(v);
		push(v);
	}
	tick(0xD1A8, 36);
	pokeb(0x0E98, 0x40);                               // the object queue state
	fl::movb(0x40);
	tick(0xD1AE, 33);
	poke(0x0E96, 0x020E);
	fl::mov(0x020E);
	tick(0xD1B4, 33);
	poke(0x09AC, 0);
	fl::clr();
	tick(0xD1B8, 27);
	pokeb(0x0E6B, 0xFF);
	fl::movb(0xFF);
	tick(0xD1BE, 33);
	pokeb(0x0E6C, 0x05);
	fl::movb(0x05);
	tick(0xD1C4, 33);
	pokeb(0x0E68, 0);
	fl::clr();
	tick(0xD1C8, 27);
	pokeb(0x0E6A, 0xFF);
	fl::movb(0xFF);
	tick(0xD1CE, 33);
	jsr(0xD1D2, 0xD8D2);                               // set_ticket_entry_types
	jsr(0xD1D6, 0xD880);                               // set_park_entry_types
	T.r[1] = 0x0003;
	fl::mov(T.r[1]);
	tick(0xD1DA, 18);
	for (;;)                                           // map objects 3..0 removed
	{
		const uint8_t type = peekb((uint16_t)(0x09B8 + T.r[1]));
		fl::tstb(type);
		tick(0xD1DE, 27);
		if (!type)
			tick(0xD1E8, 12);                          // BEQ
		else
		{
			tick(0xD1E0, 12);
			push(T.r[1]);
			fl::mov(T.r[1]);
			tick(0xD1E2, 24);
			jsr(0xD1E6, 0xCF5E);                       // remove_map_object
			drop(0xD1E6);
		}
		T.r[1] = fl::dec(T.r[1]);
		tick(0xD1EA, 12);
		if ((int16_t)T.r[1] < 0)
		{
			tick(0xD1EC, 12);                          // BGE not taken
			break;
		}
		tick(0xD1DA, 12);
	}
	type_banks(0xD1EC, 0);
	T.r[1] = 0x0003;
	fl::mov(T.r[1]);
	tick(0xD204, 18);
	for (;;)                                           // the four fixed objects (park gates)
	{
		push(0);
		fl::clr();
		tick(0xD206, 24);
		{
			const uint8_t type = peekb((uint16_t)(0x70E4 + T.r[1]));
			pokeb(T.r[6], type);
			fl::movb(type);
		}
		tick(0xD20A, 36);
		T.r[0] = T.r[1];
		fl::mov(T.r[0]);
		tick(0xD20C, 12);
		T.r[0] = fl_asl(T.r[0]);
		tick(0xD20E, 12);
		{
			const uint16_t desc = peek((uint16_t)(0x70E8 + T.r[0]));
			fl::mov(desc);
			push(desc);
		}
		tick(0xD212, 39);
		push(0);
		fl::clr();
		tick(0xD214, 24);
		jsr(0xD218, 0xCE24);                           // add_map_object(0, desc, type)
		T.r[6] = fl::add(T.r[6], 0x0006);
		tick(0xD21C, 18);
		T.r[1] = fl::dec(T.r[1]);
		tick(0xD21E, 12);
		if ((int16_t)T.r[1] < 0)
		{
			tick(0xD220, 12);                          // BGE not taken
			break;
		}
		tick(0xD204, 12);
	}
	T.r[1] = sext8(peekb(0x0E94));                     // the last stage set up
	fl::movb((uint8_t)T.r[1]);
	tick(0xD224, 24);
	T.r[0] = 0;
	fl::clr();
	tick(0xD226, 12);
	T.r[0] = fl_logicb(peekb(0x0F2E));                 // the town stage
	tick(0xD22A, 24);
	fl::cmp(T.r[0], T.r[1]);
	tick(0xD22C, 12);
	if ((int16_t)T.r[0] <= (int16_t)T.r[1])
		tick(0xD312, 12);                              // BLE
	else
	{
		tick(0xD22E, 12);
		T.r[2] = 0x00AE;
		fl::mov(T.r[2]);
		tick(0xD232, 18);
		for (;;)                                       // map objects 0xAE..4 of temporary types go
		{
			T.r[3] = sext8(peekb((uint16_t)(0x09B8 + T.r[2])));
			fl::movb((uint8_t)T.r[3]);
			tick(0xD236, 27);
			if (!T.r[3])
				tick(0xD25E, 12);                      // BEQ
			else
			{
				tick(0xD238, 12);
				type_banks(0xD238, 0);
				T.r[1] = T.r[3];
				fl::mov(T.r[1]);
				tick(0xD24E, 12);
				const bool keep = fl_logicb((uint8_t)(peekb((uint16_t)(0x6438 + T.r[1])) & 0x01)) != 0;
				tick(0xD254, 33);
				if (keep)
					tick(0xD25E, 12);                  // BNE
				else
				{
					tick(0xD256, 12);
					push(T.r[2]);
					fl::mov(T.r[2]);
					tick(0xD258, 24);
					jsr(0xD25C, 0xCF5E);               // remove_map_object
					drop(0xD25C);
				}
			}
			T.r[2] = fl::dec(T.r[2]);
			tick(0xD260, 12);
			fl::cmp(T.r[2], 0x0004);
			tick(0xD264, 18);
			if ((int16_t)T.r[2] < 4)
			{
				tick(0xD266, 12);                      // BGE not taken
				break;
			}
			tick(0xD232, 12);
		}
		type_banks(0xD266, 0);
		T.r[0] = 0;
		fl::clr();
		tick(0xD27C, 12);
		T.r[0] = fl_logicb(peekb(0x0F2E));
		tick(0xD280, 24);
		T.r[1] = 0;
		fl::clr();
		tick(0xD282, 12);
		T.r[1] = fl_logicb(peekb((uint16_t)(0xFDE3 + T.r[0])));   // the stage's skater group
		tick(0xD286, 27);
		T.r[1] = fl_asl(T.r[1]);
		tick(0xD288, 12);
		T.r[1] = fl_asl(T.r[1]);
		tick(0xD28A, 12);
		T.r[1] = fl_asl(T.r[1]);
		tick(0xD28C, 12);
		T.r[1] = fl::add(T.r[1], 0x708C);
		tick(0xD290, 18);
		T.r[1] = fl::sub(T.r[1], 0x0008);
		tick(0xD294, 18);
		T.r[2] = 0x0004;
		fl::mov(T.r[2]);
		tick(0xD298, 18);
		for (;;)                                       // its four scripted skaters (type 1)
		{
			push(0x0001);
			fl::mov(0x0001);
			tick(0xD29C, 30);
			{
				const uint16_t desc = peek(T.r[1]);
				fl::mov(desc);
				push(desc);
			}
			tick(0xD29E, 30);
			push(0);
			fl::clr();
			tick(0xD2A0, 24);
			jsr(0xD2A4, 0xCE24);                       // add_map_object(0, desc, 1)
			T.r[1] = fl::add(T.r[1], 0x0002);
			tick(0xD2A8, 18);
			T.r[6] = fl::add(T.r[6], 0x0006);
			tick(0xD2AC, 18);
			T.r[2]--;                                  // SOB
			if (!T.r[2])
			{
				tick(0xD2AE, 18);
				break;
			}
			tick(0xD298, 18);
		}
		T.r[0] = 0;
		fl::clr();
		tick(0xD2B0, 12);
		T.r[0] = fl_logicb(peekb(0x0F2E));
		tick(0xD2B4, 24);
		T.r[0] = fl_asl(T.r[0]);
		tick(0xD2B6, 12);
		T.r[1] = peek((uint16_t)(0x6B9E + T.r[0]));    // the stage's list (type, descriptor)
		fl::mov(T.r[1]);
		tick(0xD2BA, 27);
		for (;;)
		{
			const uint16_t type = peek(T.r[1]);
			fl::cmp(type, 0xFFFF);
			tick(0xD2BE, 24);
			if (type == 0xFFFF)
			{
				tick(0xD2D6, 12);                      // BEQ
				break;
			}
			tick(0xD2C0, 12);
			push(type);
			fl::mov(type);
			tick(0xD2C2, 30);
			{
				const uint16_t desc = peek((uint16_t)(T.r[1] + 2));
				fl::mov(desc);
				push(desc);
			}
			tick(0xD2C6, 39);
			push(0);
			fl::clr();
			tick(0xD2C8, 24);
			jsr(0xD2CC, 0xCE24);                       // add_map_object(0, desc, type)
			T.r[1] = fl::add(T.r[1], 0x0004);
			tick(0xD2D0, 18);
			T.r[6] = fl::add(T.r[6], 0x0006);
			tick(0xD2D4, 18);
			tick(0xD2BA, 12);                          // BR
		}
		pokeb(0x0E93, 0);                              // the object counters, from 0x68D0
		fl::clr();
		tick(0xD2DA, 27);
		T.r[0] = 0x0014;
		fl::mov(T.r[0]);
		tick(0xD2DE, 18);
		for (;;)
		{
			pokeb((uint16_t)(0x0E7D + T.r[0]), 0);
			fl::clr();
			tick(0xD2E2, 30);
			T.r[0]--;                                  // SOB
			if (!T.r[0])
			{
				tick(0xD2E4, 18);
				break;
			}
			tick(0xD2DE, 18);
		}
		T.r[3] = 0;
		fl::clr();
		tick(0xD2E6, 12);
		T.r[3] = fl_logicb(peekb(0x0F2E));
		tick(0xD2EA, 24);
		T.r[3] = fl_asl(T.r[3]);
		tick(0xD2EC, 12);
		T.r[2] = 0x0009;
		fl::mov(T.r[2]);
		tick(0xD2F0, 18);
		for (;;)
		{
			T.r[1] = T.r[2];
			fl::mov(T.r[1]);
			tick(0xD2F2, 12);
			T.r[1] = fl::add(T.r[1], peek((uint16_t)(0x68D0 + T.r[3])));
			tick(0xD2F6, 27);
			T.r[0] = 0;
			fl::clr();
			tick(0xD2F8, 12);
			T.r[0] = fl_logicb(peekb((uint16_t)(T.r[1] - 1)));
			tick(0xD2FC, 27);
			pokeb((uint16_t)(0x0E7D + T.r[2]), (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0xD300, 30);
			T.r[1] = sext8(peekb(0x0E93));
			fl::movb((uint8_t)T.r[1]);
			tick(0xD304, 24);
			T.r[1] = fl::add(T.r[1], T.r[0]);
			tick(0xD306, 12);
			pokeb(0x0E93, (uint8_t)T.r[1]);            // the total
			fl::movb((uint8_t)T.r[1]);
			tick(0xD30A, 27);
			T.r[2]--;                                  // SOB
			if (!T.r[2])
			{
				tick(0xD30C, 18);
				break;
			}
			tick(0xD2F0, 18);
		}
		{
			const uint8_t stage = peekb(0x0F2E);
			pokeb(0x0E94, stage);                      // set up
			fl::movb(stage);
		}
		tick(0xD312, 39);
	}
	bank_a(0xD312, 0x1800);
	bank_b(0xD31E, 0x1C00);
	jsr(0xD32E, 0x48CC);                               // update_map_slots
	{
		const uint16_t v = rw(sp_at(2));
		fl::mov(v);
		poke(0x01D2, v);
	}
	tick(0xD334, 42);
	{
		const uint16_t v = pop();
		fl::mov(v);
		poke(0x01D4, v);
	}
	tick(0xD338, 33);
	{
		const uint16_t v = peek(0x01D2);
		fl::mov(v);
		poke(0x1400, v);
	}
	tick(0xD33E, 39);
	{
		const uint16_t v = peek(0x01D4);
		fl::mov(v);
		poke(0x1402, v);
	}
	tick(0xD344, 39);
	drop(0xD344);
	c_restore(4);
	return T.r[0];
}

// F:E78E  query_on_button_press(): on a new press of a button (0x0193 bits 0xC0) while 0x0190
// is clear, query the sound CPU (F:8E6A, command 10, F:8F50; then 1 / 2 for button 0x80 /
// 0x40, or 0x80 | (0x0F2E - 1) in state 9; five answer bytes, F:8F62, F:8E72).  A first
// answer with bit 7 set returns 1 and sets the state 0x018F to 6 / 7 (in state 9 it needs
// answer bit 6 instead).  Without a press 0x0190 is cleared.  Waits for the sound CPU, so the
// cycles are charged here.
uint16_t query_on_button_press(const Args &)
{
	c_save(1, 0xE792);
	const bool pressed = fl_logicb((uint8_t)(peekb(0x0193) & 0xC0)) != 0;
	tick(0xE798, 30);
	if (!pressed)
	{
		tick(0xE826, 12);                              // BEQ
		pokeb(0x0190, 0);
		fl::clr();
		tick(0xE82A, 27);
		T.r[0] = 0;
		fl::clr();
		tick(0xE82C, 12);
		c_restore(1);
		return T.r[0];
	}
	tick(0xE79A, 12);
	const uint8_t busy = peekb(0x0190);
	fl::tstb(busy);
	tick(0xE79E, 24);
	if (busy)
	{
		tick(0xE82A, 12);                              // BNE
		T.r[0] = 0;
		fl::clr();
		tick(0xE82C, 12);
		c_restore(1);
		return T.r[0];
	}
	tick(0xE7A0, 12);
	jsr(0xE7A4, 0x8E6A);                               // set_flag_01a4
	push(0x000A);
	fl::mov(0x000A);
	tick(0xE7A8, 30);
	jsr(0xE7AC, 0x8EA2);                               // send_sound_command(10)
	jsr(0xE7B0, 0x8F50);                               // expect_answer_aa
	const uint8_t state = peekb(0x018F);
	fl::cmpb(state, 0x09);
	tick(0xE7B6, 30);
	if (state == 9)
	{
		tick(0xE7B8, 12);
		T.r[0] = 0;
		fl::clr();
		tick(0xE7BA, 12);
		T.r[0] = fl_logicb(peekb(0x0F2E));
		tick(0xE7BE, 24);
		T.r[0] = fl::dec(T.r[0]);
		tick(0xE7C0, 12);
		ww(T.r[6], T.r[0]);
		fl::mov(T.r[0]);
		tick(0xE7C2, 21);
		ww(T.r[6], fl_logic(rw(T.r[6]) | 0x0080));
		tick(0xE7C6, 27);
		tick(0xE7E0, 12);                              // BR
	}
	else
	{
		tick(0xE7C8, 12);                              // BNE
		const uint8_t buttons = peekb(0x0193);
		fl::tstb(buttons);
		tick(0xE7CC, 24);
		if (!(buttons & 0x80))
		{
			tick(0xE7D8, 12);                          // BPL: button 0x40
			T.r[1] = 0x0007;
			fl::mov(T.r[1]);
			tick(0xE7DC, 18);
			ww(T.r[6], 0x0002);
			fl::mov(0x0002);
			tick(0xE7E0, 27);
		}
		else
		{
			tick(0xE7CE, 12);                          // button 0x80
			T.r[1] = 0x0006;
			fl::mov(T.r[1]);
			tick(0xE7D2, 18);
			ww(T.r[6], 0x0001);
			fl::mov(0x0001);
			tick(0xE7D6, 27);
			tick(0xE7E0, 12);                          // BR
		}
	}
	jsr(0xE7E4, 0x8EA2);                               // send_sound_command
	jsr(0xE7E8, 0x8ED2);                               // read_sound_response: the answer
	T.r[2] = T.r[0];
	fl::mov(T.r[2]);
	tick(0xE7EA, 12);
	jsr(0xE7EE, 0x8ED2);                               // and four more bytes
	jsr(0xE7F2, 0x8ED2);
	jsr(0xE7F6, 0x8ED2);
	jsr(0xE7FA, 0x8ED2);
	jsr(0xE7FE, 0x8F62);                               // expect_answer_55
	jsr(0xE802, 0x8E72);                               // clear_flag_01a4
	fl::tstb((uint8_t)T.r[2]);
	tick(0xE804, 12);
	bool yes = false;
	if (!(T.r[2] & 0x80))
		tick(0xE822, 12);                              // BPL
	else
	{
		tick(0xE806, 12);
		const uint8_t state2 = peekb(0x018F);
		fl::cmpb(state2, 0x09);
		tick(0xE80C, 30);
		if (state2 == 9)
		{
			tick(0xE814, 12);                          // BEQ
			yes = fl_logic(T.r[2] & 0x0040) != 0;
			tick(0xE818, 18);
			tick(yes ? 0xE81A : 0xE822, 12);           // BEQ
		}
		else
		{
			tick(0xE80E, 12);
			pokeb(0x018F, (uint8_t)T.r[1]);            // the new state
			fl::movb((uint8_t)T.r[1]);
			tick(0xE812, 27);
			tick(0xE81A, 12);                          // BR
			yes = true;
		}
	}
	if (yes)
	{
		drop(0xE81A);
		T.r[0] = 0x0001;
		fl::mov(T.r[0]);
		tick(0xE820, 18);
		c_restore(1);
		return T.r[0];
	}
	drop(0xE822);
	tick(0xE82A, 12);                                  // BR
	T.r[0] = 0;
	fl::clr();
	tick(0xE82C, 12);
	c_restore(1);
	return T.r[0];
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
	// locals: (SP) the kind's counter, 2(SP) the flag for F:CE24, 4(SP) the type, 6(SP) refused
	T.r[6] = fl::sub(T.r[6], 0x0008);
	tick(0xD362, 18);
	const uint8_t total = peekb(0x0E93);
	fl::tstb(total);
	tick(0xD366, 24);
	if (!total)
	{
		tick(0xD368, 12);                              // BNE not taken
		tick(0xD476, 18);                              // JMP
	}
	else
	{
		tick(0xD36C, 12);
		T.r[4] = 0x0013;                               // kinds 19 down to 1
		fl::mov(T.r[4]);
		tick(0xD370, 18);
		for (;;)
		{
			ww(T.r[6], 0x0E7D);
			fl::mov(0x0E7D);
			tick(0xD374, 27);
			ww(T.r[6], fl::add(rw(T.r[6]), T.r[4]));
			tick(0xD376, 21);
			const uint8_t left = peekb(rw(T.r[6]));
			fl::tstb(left);
			tick(0xD37A, 33);
			bool placed = false;
			if (!left)
				tick(0xD46E, 12);                      // BEQ: none of this kind
			else
			{
				tick(0xD37C, 12);
				type_banks(0xD37C, 0);
				ww(sp_at(6), 0);
				fl::clr();
				tick(0xD394, 30);
				jsr(0xD398, 0x9152);                   // next_random
				T.r[5] = 0;
				fl::clr();
				tick(0xD39A, 12);
				T.r[5] = fl_logicb(peekb(0x0E92));
				tick(0xD39E, 24);
				T.r[0] = fl::add(T.r[0], T.r[5]);
				tick(0xD3A0, 12);
				pokeb(0x0E92, (uint8_t)T.r[0]);
				fl::movb((uint8_t)T.r[0]);
				tick(0xD3A4, 27);
				T.r[1] = peek(0x0E92);
				fl::mov(T.r[1]);
				tick(0xD3A8, 24);
				T.r[0] = sext8(peekb((uint16_t)(0xFD95 + T.r[4])));   // the kind's mask
				fl::movb((uint8_t)T.r[0]);
				tick(0xD3AC, 27);
				T.r[0] = fl_com(T.r[0]);
				tick(0xD3AE, 12);
				T.r[1] = fl_logic((uint16_t)(T.r[1] & ~T.r[0]));
				tick(0xD3B0, 12);
				T.r[1] = fl_logic(T.r[1] & 0x00FF);
				tick(0xD3B4, 18);
				T.r[0] = T.r[4];
				fl::mov(T.r[0]);
				tick(0xD3B6, 12);
				T.r[0] = fl_asl(T.r[0]);
				tick(0xD3B8, 12);
				T.r[5] = T.r[1];
				fl::mov(T.r[5]);
				tick(0xD3BA, 12);
				T.r[5] = fl_asl(T.r[5]);
				tick(0xD3BC, 12);
				T.r[5] = fl::add(T.r[5], peek((uint16_t)(0x6948 + T.r[0])));
				tick(0xD3C0, 27);
				T.r[3] = peek(T.r[5]);                 // the candidate cell
				fl::mov(T.r[3]);
				tick(0xD3C2, 18);
				fl::cmp(T.r[4], 0x0003);
				tick(0xD3C6, 18);
				bool refused;
				if ((int16_t)T.r[4] > 3)
				{
					// outside the window 0x099A-0x099C (rows) / 0x0996-0x0998 (columns)
					tick(0xD3E6, 12);                  // BGT
					T.r[2] = T.r[3];
					fl::mov(T.r[2]);
					tick(0xD3E8, 12);
					T.r[2] = fl_logic(T.r[2] & 0x000F);
					tick(0xD3EC, 18);
					fl::cmp(T.r[2], peek(0x099A));
					tick(0xD3F0, 24);
					refused = false;
					if ((int16_t)T.r[2] < (int16_t)peek(0x099A))
						tick(0xD40E, 12);              // BLT
					else
					{
						tick(0xD3F2, 12);
						fl::cmp(T.r[2], peek(0x099C));
						tick(0xD3F6, 24);
						if ((int16_t)T.r[2] > (int16_t)peek(0x099C))
							tick(0xD40E, 12);          // BGT
						else
						{
							tick(0xD3F8, 12);
							T.r[2] = T.r[3];
							fl::mov(T.r[2]);
							tick(0xD3FA, 12);
							T.r[2] = fl_logic(T.r[2] & 0x01F0);
							tick(0xD3FE, 18);
							fl::cmp(T.r[2], peek(0x0996));
							tick(0xD402, 24);
							if ((int16_t)T.r[2] < (int16_t)peek(0x0996))
								tick(0xD40E, 12);      // BLT
							else
							{
								tick(0xD404, 12);
								fl::cmp(T.r[2], peek(0x0998));
								tick(0xD408, 24);
								refused = (int16_t)T.r[2] < (int16_t)peek(0x0998);
								tick(refused ? 0xD40A : 0xD40E, 12);   // BGE
							}
						}
					}
				}
				else
				{
					// kinds 1-3: column bits 4-5 of the cell other than the tile bank 0x01CE - 1
					tick(0xD3C8, 12);
					T.r[5] = T.r[1];
					fl::mov(T.r[5]);
					tick(0xD3CA, 12);
					T.r[5] = fl_asr(T.r[5]);
					tick(0xD3CC, 12);
					T.r[5] = fl_asr(T.r[5]);
					tick(0xD3CE, 12);
					T.r[5] = fl_asr(T.r[5]);
					tick(0xD3D0, 12);
					T.r[5] = fl_asr(T.r[5]);
					tick(0xD3D2, 12);
					T.r[0] = peek(0x01CE);
					fl::mov(T.r[0]);
					tick(0xD3D6, 24);
					T.r[0] = fl::dec(T.r[0]);
					tick(0xD3D8, 12);
					push(T.r[0]);
					fl::mov(T.r[0]);
					tick(0xD3DA, 24);
					push(T.r[5]);
					fl::mov(T.r[5]);
					tick(0xD3DC, 24);
					ww(T.r[6], fl_logic(rw(T.r[6]) & 0x0003));
					tick(0xD3E0, 27);
					const uint16_t a = pop(), b = pop();
					fl::cmp(a, b);
					tick(0xD3E2, 24);
					refused = a == b;
					if (!refused)
						tick(0xD40E, 12);              // BNE
					else
					{
						tick(0xD3E4, 12);
						tick(0xD40A, 12);              // BR
					}
				}
				if (refused)
				{
					ww(sp_at(6), fl::inc(rw(sp_at(6))));
					tick(0xD40E, 30);
				}
				const uint16_t no = rw(sp_at(6));
				fl::tst(no);
				tick(0xD412, 27);
				if (no)
					tick(0xD46E, 12);                  // BNE
				else
				{
					tick(0xD414, 12);
					fl::cmp(T.r[4], 0x000A);
					tick(0xD418, 18);
					if ((int16_t)T.r[4] > 0x0A)
					{
						// kinds 11-19: a fixed type (0x6935), flag 0
						tick(0xD43A, 12);              // BGT
						{
							const uint8_t type = peekb((uint16_t)(0x6935 + T.r[4]));
							pokeb(sp_at(4), type);
							fl::movb(type);
						}
						tick(0xD440, 45);
						pokeb(sp_at(5), 0);
						fl::clr();
						tick(0xD444, 30);
						ww(sp_at(2), 0);
						fl::clr();
						tick(0xD448, 30);
					}
					else
					{
						// kinds 1-10: the type for the stage (0x6862), flag 1
						tick(0xD41A, 12);
						T.r[0] = 0;
						fl::clr();
						tick(0xD41C, 12);
						T.r[0] = fl_logicb(peekb(0x0F2E));
						tick(0xD420, 24);
						T.r[0] = fl_asl(T.r[0]);
						tick(0xD422, 12);
						T.r[0] = peek((uint16_t)(0x6862 + T.r[0]));
						fl::mov(T.r[0]);
						tick(0xD426, 27);
						T.r[0] = fl::add(T.r[0], T.r[4]);
						tick(0xD428, 12);
						{
							const uint8_t type = peekb((uint16_t)(T.r[0] - 1));
							pokeb(sp_at(4), type);
							fl::movb(type);
						}
						tick(0xD42E, 45);
						pokeb(sp_at(5), 0);
						fl::clr();
						tick(0xD432, 30);
						ww(sp_at(2), 0x0001);
						fl::mov(0x0001);
						tick(0xD438, 36);
						tick(0xD448, 12);              // BR
					}
					{
						const uint16_t type = rw(sp_at(4));
						fl::mov(type);
						push(type);
					}
					tick(0xD44C, 39);
					push(T.r[3]);
					fl::mov(T.r[3]);
					tick(0xD44E, 24);
					{
						const uint16_t flag = rw(sp_at(6));
						fl::mov(flag);
						push(flag);
					}
					tick(0xD452, 39);
					jsr(0xD456, 0xCE24);               // add_map_object(flag, cell, type)
					T.r[6] = fl::add(T.r[6], 0x0006);
					tick(0xD45A, 18);
					fl::tst(T.r[0]);
					tick(0xD45C, 12);
					if (T.r[0])
						tick(0xD46E, 12);              // BNE: not placed
					else
					{
						// placed: one less of the kind and in total
						tick(0xD45E, 12);
						{
							const uint16_t p = rw(T.r[6]);
							pokeb(p, fl::decb(peekb(p)));
						}
						tick(0xD462, 36);
						T.r[0] = sext8(peekb(0x0E93));
						fl::movb((uint8_t)T.r[0]);
						tick(0xD466, 24);
						T.r[0] = fl::dec(T.r[0]);
						tick(0xD468, 12);
						pokeb(0x0E93, (uint8_t)T.r[0]);
						fl::movb((uint8_t)T.r[0]);
						tick(0xD46C, 27);
						tick(0xD476, 12);              // BR
						placed = true;
					}
				}
			}
			if (placed)
				break;
			T.r[4] = fl::dec(T.r[4]);
			tick(0xD470, 12);
			if (!T.r[4])
			{
				tick(0xD476, 12);                      // BEQ
				break;
			}
			tick(0xD472, 12);
			tick(0xD370, 18);                          // JMP
		}
	}
	T.r[6] = fl::add(T.r[6], 0x0008);
	tick(0xD47A, 18);
	rts();
}

// F:D4E4 (registers): position of the skater (object 0) relative to object 0x0EA0.  R5 = the
// object's shape record (table 0x4354 through 0xFDBA[0x07D0[obj]], bank A 0x2400), R4 =
// column (x distance + 0x0A00 - record[-4]: high byte / 2) also in 0x0E9E, R3 = height
// (record[-2] - (y + z distance + 0x0300)) also in 0x0E9C; when positive, R3 becomes the
// row's offset byte from 0x4338.  R1 is preserved.
void offset_to_object_0ea0()
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xD4E6, 24);
	bank_a(0xD4E6, 0x2400);
	bank_b(0xD4F2, 0x2800);
	T.r[1] = sext8(peekb(0x0EA0));                     // the object
	fl::movb((uint8_t)T.r[1]);
	tick(0xD502, 24);
	T.r[0] = 0;
	fl::clr();
	tick(0xD504, 12);
	T.r[0] = fl_logicb(peekb((uint16_t)(0x07D0 + T.r[1])));   // its direction
	tick(0xD508, 27);
	T.r[5] = sext8(peekb((uint16_t)(0xFDBA + T.r[0])));
	fl::movb((uint8_t)T.r[5]);
	tick(0xD50C, 27);
	T.r[5] = fl::add(T.r[5], 0x4354);
	tick(0xD510, 18);
	T.r[5] = peek(T.r[5]);                             // its shape record
	fl::mov(T.r[5]);
	tick(0xD512, 18);
	T.r[1] = fl_asl(T.r[1]);
	tick(0xD514, 12);
	T.r[0] = peek(0x06F0);
	fl::mov(T.r[0]);
	tick(0xD518, 24);
	T.r[0] = fl::sub(T.r[0], peek((uint16_t)(0x06F0 + T.r[1])));   // x distance
	tick(0xD51C, 27);
	T.r[4] = T.r[0];
	fl::mov(T.r[4]);
	tick(0xD51E, 12);
	T.r[4] = fl::add(T.r[4], 0x0A00);
	tick(0xD522, 18);
	T.r[4] = fl::sub(T.r[4], peek((uint16_t)(T.r[5] - 4)));
	tick(0xD526, 27);
	T.r[0] = T.r[4];
	fl::mov(T.r[0]);
	tick(0xD528, 12);
	T.r[0] &= 0xFF00;                                  // CLRB R0
	fl::clr();
	tick(0xD52A, 12);
	T.r[0] = fl_swab(T.r[0]);
	tick(0xD52C, 12);
	T.r[0] = sext8((uint8_t)T.r[0]);                   // MOVB R0,R0
	fl::movb((uint8_t)T.r[0]);
	tick(0xD52E, 12);
	T.r[4] = T.r[0];
	fl::mov(T.r[4]);
	tick(0xD530, 12);
	T.r[4] = fl_asr(T.r[4]);                           // the column
	tick(0xD532, 12);
	T.r[0] = peek(0x0730);
	fl::mov(T.r[0]);
	tick(0xD536, 24);
	T.r[0] = fl::add(T.r[0], peek(0x06B0));
	tick(0xD53A, 24);
	T.r[0] = fl::sub(T.r[0], peek((uint16_t)(0x0730 + T.r[1])));   // y + z distance
	tick(0xD53E, 27);
	T.r[3] = T.r[0];
	fl::mov(T.r[3]);
	tick(0xD540, 12);
	T.r[3] = fl::add(T.r[3], 0x0300);
	tick(0xD544, 18);
	T.r[3] = fl_neg(T.r[3]);
	tick(0xD546, 12);
	T.r[3] = fl::add(T.r[3], peek((uint16_t)(T.r[5] - 2)));   // the height
	tick(0xD54A, 27);
	poke(0x0E9E, T.r[4]);
	fl::mov(T.r[4]);
	tick(0xD54E, 27);
	poke(0x0E9C, T.r[3]);
	fl::mov(T.r[3]);
	tick(0xD552, 27);
	if ((int16_t)T.r[3] <= 0)
		tick(0xD562, 12);                              // BLE
	else
	{
		// the row's offset byte
		tick(0xD554, 12);
		T.r[0] = T.r[3];
		fl::mov(T.r[0]);
		tick(0xD556, 12);
		T.r[0] = fl_swab(T.r[0]);
		tick(0xD558, 12);
		T.r[0] = sext8((uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0xD55A, 12);
		T.r[0] = fl_asr(T.r[0]);
		tick(0xD55C, 12);
		T.r[3] = 0;
		fl::clr();
		tick(0xD55E, 12);
		T.r[3] = fl_logicb(peekb((uint16_t)(0x4338 + T.r[0])));
		tick(0xD562, 27);
	}
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0xD564, 18);
	rts();
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
	T.r[6] = (uint16_t)(T.r[6] - 2);                   // TST -(SP): the side bit
	fl::tst(rw(T.r[6]));
	tick(0xD568, 21);
	type_banks(0xD568, 0);
	T.r[1] = sext8(peekb(0x0EA0));
	fl::movb((uint8_t)T.r[1]);
	tick(0xD580, 24);
	T.r[1] = fl_neg(T.r[1]);                           // the object (0x0EA0 is negative here)
	tick(0xD582, 12);
	T.r[0] = 0;
	fl::clr();
	tick(0xD584, 12);
	jsr(0xD588, 0xAD70);                               // sprite_in_front
	ww(T.r[6], T.r[0]);
	fl::mov(T.r[0]);
	tick(0xD58A, 21);
	pokeb(0x0EA0, fl_negb(peekb(0x0EA0)));
	tick(0xD58E, 27);
	{
		const uint8_t v = peekb(0x0EA0);
		pokeb(0x0EA1, v);
		fl::movb(v);
	}
	tick(0xD594, 39);
	pokeb(0x0EA2, 0x05);
	fl::movb(0x05);
	tick(0xD59A, 33);
	jsr(0xD59E, 0xD4E4);                               // offset_to_object_0ea0
	T.r[0] = T.r[4];
	fl::mov(T.r[0]);
	tick(0xD5A0, 12);
	T.r[0] = fl_logic(T.r[0] | T.r[3]);
	tick(0xD5A2, 12);
	if ((int16_t)T.r[0] < 0)
	{
		// outside the shape
		tick(0xD5A4, 12);
		pokeb(0x0EA0, 0);
		fl::clr();
		tick(0xD5A8, 27);
		tick(0xD62E, 12);                              // BR
	}
	else
	{
		tick(0xD5AA, 12);                              // BGE
		T.r[0] = peek(0x06B0);
		fl::mov(T.r[0]);
		tick(0xD5AE, 24);
		T.r[1] = T.r[3];
		fl::mov(T.r[1]);
		tick(0xD5B0, 12);
		T.r[1] = fl::add(T.r[1], T.r[5]);
		tick(0xD5B2, 12);
		T.r[1] = fl::add(T.r[1], T.r[4]);
		tick(0xD5B4, 12);
		const uint8_t first = peekb(T.r[1]);
		fl::tstb(first);
		tick(0xD5B6, 18);
		if (!first)
			tick(0xD5EA, 12);                          // BEQ
		else
		{
			// up the shape's rows (0x12 bytes) while there is something under the skater
			tick(0xD5B8, 12);
			for (;;)
			{
				T.r[3] = fl::sub(T.r[3], 0x0012);
				tick(0xD5BC, 18);
				T.r[0] = fl::add(T.r[0], 0x0200);
				tick(0xD5C0, 18);
				T.r[1] = T.r[3];
				fl::mov(T.r[1]);
				tick(0xD5C2, 12);
				T.r[1] = fl::add(T.r[1], T.r[5]);
				tick(0xD5C4, 12);
				T.r[1] = fl::add(T.r[1], T.r[4]);
				tick(0xD5C6, 12);
				T.r[2] = 0;
				fl::clr();
				tick(0xD5C8, 12);
				T.r[2] = fl_logicb(peekb(T.r[1]));
				tick(0xD5CA, 18);
				T.r[2] = fl_logic(T.r[2] & 0x007F);
				tick(0xD5CE, 18);
				const bool side = fl_logic(rw(T.r[6]) & 0x0001) != 0;
				tick(0xD5D2, 24);
				if (side)
				{
					tick(0xD5D4, 12);
					fl::tst(T.r[2]);
					tick(0xD5D6, 12);
					bool more = false;
					if (!T.r[2])
						tick(0xD5DC, 12);              // BEQ
					else
					{
						tick(0xD5D8, 12);
						fl::tst(T.r[3]);
						tick(0xD5DA, 12);
						more = (int16_t)T.r[3] > 0;
						tick(more ? 0xD5B8 : 0xD5DC, 12);   // BGT
					}
					if (more)
						continue;
					T.r[0] = fl::sub(T.r[0], 0x0100);
					tick(0xD5E0, 18);
					tick(0xD5EA, 12);                  // BR
					break;
				}
				tick(0xD5E2, 12);                      // BEQ
				T.r[1] = 0x0007;
				fl::mov(T.r[1]);
				tick(0xD5E6, 18);
				T.r[1] = fl_logic((uint16_t)(T.r[1] & ~T.r[2]));
				tick(0xD5E8, 12);
				if (T.r[1])
				{
					tick(0xD5EA, 12);                  // BEQ not taken
					break;
				}
				tick(0xD5B8, 12);
			}
		}
		fl::cmp(T.r[0], peek(0x06B2));
		tick(0xD5EE, 24);
		if ((int16_t)T.r[0] < (int16_t)peek(0x06B2))
		{
			// below the skater's body: it stands on the object at that height
			tick(0xD626, 12);                          // BLT
			poke(0x06B0, T.r[0]);
			fl::mov(T.r[0]);
			tick(0xD62A, 27);
			jsr(0xD62E, 0xD778);                       // update_object_0ea0
		}
		else
		{
			// let go: facing away from the object's side, at the board's top speed, landed
			tick(0xD5F0, 12);
			pokeb(0x0EA0, 0);
			fl::clr();
			tick(0xD5F4, 27);
			const bool side = fl_logic(rw(T.r[6]) & 0x0001) != 0;
			tick(0xD5F8, 24);
			if (side)
			{
				tick(0xD5FA, 12);
				pokeb(0x031D, 0x04);
				fl::movb(0x04);
				tick(0xD600, 33);
				tick(0xD608, 12);                      // BR
			}
			else
			{
				tick(0xD602, 12);                      // BEQ
				pokeb(0x031D, 0x0C);
				fl::movb(0x0C);
				tick(0xD608, 33);
			}
			bank_b(0xD608, 0x2C00);
			T.r[0] = sext8(peekb(0x025D));
			fl::movb((uint8_t)T.r[0]);
			tick(0xD618, 24);
			T.r[0] = fl_asl(T.r[0]);
			tick(0xD61A, 12);
			{
				const uint16_t speed = peek((uint16_t)(0x7C90 + T.r[0]));
				poke(0x01B4, speed);
				fl::mov(speed);
			}
			tick(0xD620, 42);
			jsr(0xD624, 0xA2F0);                       // skater_land
			tick(0xD62E, 12);                          // BR
		}
	}
	fl::tst(rw(T.r[6]));
	T.r[6] = (uint16_t)(T.r[6] + 2);
	tick(0xD630, 18);
	rts();
}

// F:DB4E (registers): sprite palette (0x1000-0x107F) = the colours at 0x7EEC-0x7FAB (bank B
// 0x0400; the first 16 from 0x7F0C-0x7F2B of bank 0x1C00 when 0x01F5 is set) minus
// sext(0x0EA7), each group of 16 in reverse order.  Banks restored.
void fade_sprite_palette()
{
	{
		const uint16_t v = peek(0x01D2);               // the banks, put back at the end
		fl::mov(v);
		push(v);
	}
	tick(0xDB52, 36);
	{
		const uint16_t v = peek(0x01D4);
		fl::mov(v);
		push(v);
	}
	tick(0xDB56, 36);
	type_banks(0xDB56, 0);
	T.r[3] = sext8(peekb(0x0EA7));                     // the fade level
	fl::movb((uint8_t)T.r[3]);
	tick(0xDB6E, 24);
	T.r[2] = 0x7FAC;
	fl::mov(T.r[2]);
	tick(0xDB72, 18);
	T.r[5] = 0x002F;
	fl::mov(T.r[5]);
	tick(0xDB76, 18);
	for (;;)                                           // sprite colours 0x10-0x3F
	{
		T.r[2] = (uint16_t)(T.r[2] - 2);
		T.r[1] = peek(T.r[2]);
		fl::mov(T.r[1]);
		tick(0xDB78, 21);
		T.r[1] = fl::sub(T.r[1], T.r[3]);
		tick(0xDB7A, 12);
		T.r[4] = T.r[5];
		fl::mov(T.r[4]);
		tick(0xDB7C, 12);
		T.r[4] = fl_asl(T.r[4]);
		tick(0xDB7E, 12);
		T.r[0] = 0x001E;
		fl::mov(T.r[0]);
		tick(0xDB82, 18);
		T.r[4] = fl_logic(T.r[4] ^ T.r[0]);            // each group of 16 reversed
		tick(0xDB84, 12);
		poke((uint16_t)(0x1020 + T.r[4]), T.r[1]);
		fl::mov(T.r[1]);
		tick(0xDB88, 30);
		T.r[5] = fl::dec(T.r[5]);
		tick(0xDB8A, 12);
		if ((int16_t)T.r[5] < 0)
		{
			tick(0xDB8C, 12);                          // BGE not taken
			break;
		}
		tick(0xDB76, 12);
	}
	const uint8_t player2 = peekb(0x01F5);
	fl::tstb(player2);
	tick(0xDB90, 24);
	if (!player2)
		tick(0xDBAE, 12);                              // BEQ: on from where the loop stopped
	else
	{
		// player 2's skater colours
		tick(0xDB92, 12);
		bank_a(0xDB92, 0x1800);
		bank_b(0xDB9E, 0x1C00);
		T.r[2] = 0x7F4C;
		fl::mov(T.r[2]);
		tick(0xDBAE, 18);
	}
	T.r[5] = 0x000F;
	fl::mov(T.r[5]);
	tick(0xDBB2, 18);
	for (;;)                                           // sprite colours 0-0x0F
	{
		T.r[2] = (uint16_t)(T.r[2] - 2);
		T.r[1] = peek(T.r[2]);
		fl::mov(T.r[1]);
		tick(0xDBB4, 21);
		T.r[1] = fl::sub(T.r[1], T.r[3]);
		tick(0xDBB6, 12);
		T.r[4] = T.r[5];
		fl::mov(T.r[4]);
		tick(0xDBB8, 12);
		T.r[4] = fl_asl(T.r[4]);
		tick(0xDBBA, 12);
		T.r[0] = 0x001E;
		fl::mov(T.r[0]);
		tick(0xDBBE, 18);
		T.r[4] = fl_logic(T.r[4] ^ T.r[0]);
		tick(0xDBC0, 12);
		poke((uint16_t)(0x1000 + T.r[4]), T.r[1]);
		fl::mov(T.r[1]);
		tick(0xDBC4, 30);
		T.r[5] = fl::dec(T.r[5]);
		tick(0xDBC6, 12);
		if ((int16_t)T.r[5] < 0)
		{
			tick(0xDBC8, 12);                          // BGE not taken
			break;
		}
		tick(0xDBB2, 12);
	}
	{
		const uint16_t v = rw(sp_at(2));
		fl::mov(v);
		poke(0x01D2, v);
	}
	tick(0xDBCE, 42);
	{
		const uint16_t v = pop();
		fl::mov(v);
		poke(0x01D4, v);
	}
	tick(0xDBD2, 33);
	{
		const uint16_t v = peek(0x01D2);
		fl::mov(v);
		poke(0x1400, v);
	}
	tick(0xDBD8, 39);
	{
		const uint16_t v = peek(0x01D4);
		fl::mov(v);
		poke(0x1402, v);
	}
	tick(0xDBDE, 39);
	drop(0xDBDE);
	rts();
}

// F:DBE2 (registers): one step of the palette fade towards 0x0EA8 (negative: none): on odd
// frames the level 0x0EA7 moves one step and the sprite palette is redone (F:DB4E); on even
// frames a reached target is cleared (0xFF) and the playfield palette is redone (F:DB00)
void step_palette_fade()
{
	const uint8_t target = peekb(0x0EA8);
	fl::tstb(target);
	tick(0xDBE6, 24);
	if (target & 0x80)
	{
		tick(0xDC22, 12);                              // BLT: no fade
		rts();
		return;
	}
	tick(0xDBE8, 12);
	T.r[0] = sext8(peekb(0x0EA7));                     // the level now
	fl::movb((uint8_t)T.r[0]);
	tick(0xDBEC, 24);
	const bool odd = fl_logic(peek(0x01C4) & 0x0001) != 0;
	tick(0xDBF2, 30);
	if (odd)
	{
		// odd frames: one step towards the target, sprites redone
		tick(0xDBF4, 12);
		T.r[1] = sext8(peekb(0x0EA8));
		fl::movb((uint8_t)T.r[1]);
		tick(0xDBF8, 24);
		fl::cmp(T.r[1], T.r[0]);
		tick(0xDBFA, 12);
		if ((int16_t)T.r[1] <= (int16_t)T.r[0])
		{
			tick(0xDC02, 12);                          // BLE
			T.r[1] = T.r[0];
			fl::mov(T.r[1]);
			tick(0xDC04, 12);
			T.r[1] = fl::dec(T.r[1]);
			tick(0xDC06, 12);
		}
		else
		{
			tick(0xDBFC, 12);
			T.r[1] = T.r[0];
			fl::mov(T.r[1]);
			tick(0xDBFE, 12);
			T.r[1] = fl::inc(T.r[1]);
			tick(0xDC00, 12);
			tick(0xDC06, 12);                          // BR
		}
		pokeb(0x0EA7, (uint8_t)T.r[1]);
		fl::movb((uint8_t)T.r[1]);
		tick(0xDC0A, 27);
		jsr(0xDC0E, 0xDB4E);                           // fade_sprite_palette
		rts();
		return;
	}
	// even frames: a reached target is cleared, the playfield redone
	tick(0xDC10, 12);                                  // BEQ
	T.r[1] = sext8(peekb(0x0EA8));
	fl::movb((uint8_t)T.r[1]);
	tick(0xDC14, 24);
	fl::cmp(T.r[1], T.r[0]);
	tick(0xDC16, 12);
	if (T.r[1] != T.r[0])
		tick(0xDC1E, 12);                              // BNE
	else
	{
		tick(0xDC18, 12);
		pokeb(0x0EA8, 0xFF);
		fl::movb(0xFF);
		tick(0xDC1E, 33);
	}
	jsr(0xDC22, 0xDB00);                               // fade_playfield_palette
	rts();
}

// F:DC3C (registers; run from 0x0EB0 by F:DDEC): load palette block 0x0EAC from colour set
// 0x0EAE (bank B 0x1C00): blocks 0-7 are 4 text colours (0x1080 + 8 * block, from 0x7F6C),
// 8-15 16 playfield colours (0x1100, from 0x7EAC), 16 on 16 sprite colours (0x1000,
// reversed, from 0x7F0C).  Banks restored.
void load_palette_block()
{
	T.r[5] = peek(0x01D2);                             // the banks, put back at the end
	fl::mov(T.r[5]);
	tick(0xDC40, 24);
	{
		const uint16_t v = peek(0x01D4);
		fl::mov(v);
		push(v);
	}
	tick(0xDC44, 36);
	T.r[1] = peek(0x0EAC);                             // the block
	fl::mov(T.r[1]);
	tick(0xDC48, 24);
	T.r[0] = peek(0x0EAE);                             // the colour set
	fl::mov(T.r[0]);
	tick(0xDC4C, 24);
	T.r[2] = 0x1800;
	fl::mov(T.r[2]);
	tick(0xDC50, 18);
	poke(0x01D2, T.r[2]);
	fl::mov(T.r[2]);
	tick(0xDC54, 27);
	poke(0x1400, T.r[2]);
	fl::mov(T.r[2]);
	tick(0xDC58, 27);
	T.r[2] = 0x1C00;
	fl::mov(T.r[2]);
	tick(0xDC5C, 18);
	poke(0x01D4, T.r[2]);
	fl::mov(T.r[2]);
	tick(0xDC60, 27);
	poke(0x1402, T.r[2]);
	fl::mov(T.r[2]);
	tick(0xDC64, 27);
	fl::cmp(T.r[1], 0x0007);
	tick(0xDC68, 18);
	if ((int16_t)T.r[1] <= 7)
	{
		// blocks 0-7: four text colours
		tick(0xDC6A, 12);                              // BGT not taken
		T.r[3] = T.r[1];
		fl::mov(T.r[3]);
		tick(0xDC6C, 12);
		T.r[3] = fl_asl(T.r[3]);
		tick(0xDC6E, 12);
		T.r[3] = fl_asl(T.r[3]);
		tick(0xDC70, 12);
		T.r[4] = T.r[0];
		fl::mov(T.r[4]);
		tick(0xDC72, 12);
		T.r[4] = fl_asl(T.r[4]);
		tick(0xDC74, 12);
		T.r[4] = fl_asl(T.r[4]);
		tick(0xDC76, 12);
		T.r[2] = 0x0003;
		fl::mov(T.r[2]);
		tick(0xDC7A, 18);
		for (;;)
		{
			T.r[1] = T.r[2];
			fl::mov(T.r[1]);
			tick(0xDC7C, 12);
			T.r[1] = fl::add(T.r[1], T.r[3]);
			tick(0xDC7E, 12);
			T.r[1] = fl_asl(T.r[1]);
			tick(0xDC80, 12);
			T.r[0] = T.r[2];
			fl::mov(T.r[0]);
			tick(0xDC82, 12);
			T.r[0] = fl::add(T.r[0], T.r[4]);
			tick(0xDC84, 12);
			T.r[0] = fl_asl(T.r[0]);
			tick(0xDC86, 12);
			{
				const uint16_t c = peek((uint16_t)(0x7F6C + T.r[0]));
				poke((uint16_t)(0x1080 + T.r[1]), c);
				fl::mov(c);
			}
			tick(0xDC8C, 45);
			T.r[2] = fl::dec(T.r[2]);
			tick(0xDC8E, 12);
			if ((int16_t)T.r[2] < 0)
			{
				tick(0xDC90, 12);                      // BGE not taken
				break;
			}
			tick(0xDC7A, 12);
		}
		tick(0xDCFA, 12);                              // BR
	}
	else
	{
		tick(0xDC92, 12);                              // BGT
		T.r[4] = T.r[0];
		fl::mov(T.r[4]);
		tick(0xDC94, 12);
		T.r[4] = fl_asl(T.r[4]);
		tick(0xDC96, 12);
		T.r[4] = fl_asl(T.r[4]);
		tick(0xDC98, 12);
		T.r[4] = fl_asl(T.r[4]);
		tick(0xDC9A, 12);
		T.r[4] = fl_asl(T.r[4]);
		tick(0xDC9C, 12);
		fl::cmp(T.r[1], 0x000F);
		tick(0xDCA0, 18);
		if ((int16_t)T.r[1] <= 0x0F)
		{
			// blocks 8-15: sixteen playfield colours
			tick(0xDCA2, 12);                          // BGT not taken
			T.r[3] = T.r[1];
			fl::mov(T.r[3]);
			tick(0xDCA4, 12);
			T.r[3] = fl::sub(T.r[3], 0x0008);
			tick(0xDCA8, 18);
			T.r[3] = fl_asl(T.r[3]);
			tick(0xDCAA, 12);
			T.r[3] = fl_asl(T.r[3]);
			tick(0xDCAC, 12);
			T.r[3] = fl_asl(T.r[3]);
			tick(0xDCAE, 12);
			T.r[3] = fl_asl(T.r[3]);
			tick(0xDCB0, 12);
			T.r[2] = 0x000F;
			fl::mov(T.r[2]);
			tick(0xDCB4, 18);
			for (;;)
			{
				T.r[1] = T.r[2];
				fl::mov(T.r[1]);
				tick(0xDCB6, 12);
				T.r[1] = fl::add(T.r[1], T.r[3]);
				tick(0xDCB8, 12);
				T.r[1] = fl_asl(T.r[1]);
				tick(0xDCBA, 12);
				T.r[0] = T.r[2];
				fl::mov(T.r[0]);
				tick(0xDCBC, 12);
				T.r[0] = fl::add(T.r[0], T.r[4]);
				tick(0xDCBE, 12);
				T.r[0] = fl_asl(T.r[0]);
				tick(0xDCC0, 12);
				{
					const uint16_t c = peek((uint16_t)(0x7EAC + T.r[0]));
					poke((uint16_t)(0x1100 + T.r[1]), c);
					fl::mov(c);
				}
				tick(0xDCC6, 45);
				T.r[2] = fl::dec(T.r[2]);
				tick(0xDCC8, 12);
				if ((int16_t)T.r[2] < 0)
				{
					tick(0xDCCA, 12);                  // BGE not taken
					break;
				}
				tick(0xDCB4, 12);
			}
			tick(0xDCFA, 12);                          // BR
		}
		else
		{
			// blocks from 16: sixteen sprite colours, reversed
			tick(0xDCCC, 12);                          // BGT
			T.r[3] = T.r[1];
			fl::mov(T.r[3]);
			tick(0xDCCE, 12);
			T.r[3] = fl::sub(T.r[3], 0x0010);
			tick(0xDCD2, 18);
			T.r[3] = fl_asl(T.r[3]);
			tick(0xDCD4, 12);
			T.r[3] = fl_asl(T.r[3]);
			tick(0xDCD6, 12);
			T.r[3] = fl_asl(T.r[3]);
			tick(0xDCD8, 12);
			T.r[3] = fl_asl(T.r[3]);
			tick(0xDCDA, 12);
			T.r[2] = 0x000F;
			fl::mov(T.r[2]);
			tick(0xDCDE, 18);
			for (;;)
			{
				T.r[1] = T.r[2];
				fl::mov(T.r[1]);
				tick(0xDCE0, 12);
				T.r[1] = fl::add(T.r[1], T.r[3]);
				tick(0xDCE2, 12);
				T.r[1] = fl_asl(T.r[1]);
				tick(0xDCE4, 12);
				T.r[0] = 0x001E;
				fl::mov(T.r[0]);
				tick(0xDCE8, 18);
				T.r[1] = fl_logic(T.r[1] ^ T.r[0]);
				tick(0xDCEA, 12);
				T.r[0] = T.r[2];
				fl::mov(T.r[0]);
				tick(0xDCEC, 12);
				T.r[0] = fl::add(T.r[0], T.r[4]);
				tick(0xDCEE, 12);
				T.r[0] = fl_asl(T.r[0]);
				tick(0xDCF0, 12);
				{
					const uint16_t c = peek((uint16_t)(0x7F0C + T.r[0]));
					poke((uint16_t)(0x1000 + T.r[1]), c);
					fl::mov(c);
				}
				tick(0xDCF6, 45);
				T.r[2] = fl::dec(T.r[2]);
				tick(0xDCF8, 12);
				if ((int16_t)T.r[2] < 0)
				{
					tick(0xDCFA, 12);                  // BGE not taken
					break;
				}
				tick(0xDCDE, 12);
			}
		}
	}
	poke(0x01D2, T.r[5]);
	fl::mov(T.r[5]);
	tick(0xDCFE, 27);
	{
		const uint16_t v = pop();
		fl::mov(v);
		poke(0x01D4, v);
	}
	tick(0xDD02, 33);
	{
		const uint16_t v = peek(0x01D2);
		fl::mov(v);
		poke(0x1400, v);
	}
	tick(0xDD08, 39);
	{
		const uint16_t v = peek(0x01D4);
		fl::mov(v);
		poke(0x1402, v);
	}
	tick(0xDD0E, 39);
	rts();
}

// F:DDEC (registers): run the palette job queued in 0x0EB0 (a routine address) and clear it
void run_palette_job()
{
	T.r[0] = peek(0x0EB0);
	fl::mov(T.r[0]);
	tick(0xDDF0, 24);
	if (!T.r[0])
		tick(0xDDF8, 12);                              // BEQ: none queued
	else
	{
		tick(0xDDF2, 12);
		jsr_reg(0xDDF4, T.r[0]);                       // JSR PC,(R0)
		poke(0x0EB0, 0);
		fl::clr();
		tick(0xDDF8, 27);
	}
	rts();
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
	jsr(0xE258, 0x9236);                               // update_push
	type_banks(0xE258, 0);
	const uint8_t mode = peekb(0x031F);
	fl::tstb(mode);
	tick(0xE270, 24);
	if (mode)
	{
		// BNE onto the BNE at E342, which branches again with these flags
		tick(0xE342, 12);
		tick(0xE3E2, 12);
	}
	else
	{
		tick(0xE272, 12);
		// the jump button (0x40) just pressed: 0x0332 = 2 (1 when already set)
		const bool pressed = fl_logicb((uint8_t)(peekb(0x0193) & 0x40)) != 0;
		tick(0xE278, 30);
		if (!pressed)
		{
			tick(0xE29E, 12);                          // BEQ
			pokeb(0x0332, 0);
			fl::clr();
			tick(0xE2A2, 27);
		}
		else
		{
			tick(0xE27A, 12);
			const uint8_t held = peekb(0x0332);
			fl::tstb(held);
			tick(0xE27E, 24);
			if (held)
			{
				tick(0xE288, 12);                      // BNE
				pokeb(0x0332, 0x01);
				fl::movb(0x01);
				tick(0xE28E, 33);
			}
			else
			{
				tick(0xE280, 12);
				pokeb(0x0332, 0x02);
				fl::movb(0x02);
				tick(0xE286, 33);
				tick(0xE28E, 12);                      // BR
			}
			const bool bit4 = fl_logicb((uint8_t)(peekb(0x0327) & 0x04)) != 0;
			tick(0xE294, 30);
			if (!bit4)
				tick(0xE2A2, 12);                      // BEQ
			else
			{
				tick(0xE296, 12);
				pokeb(0x0324, fl_logicb((uint8_t)(peekb(0x0324) | 0x80)));
				tick(0xE29C, 33);
				tick(0xE2A2, 12);                      // BR
			}
		}
		const uint8_t kicking = peekb(0x0320);
		fl::tstb(kicking);
		tick(0xE2A6, 24);
		bool kick = false;
		if (kicking)
			tick(0xE334, 12);                          // BNE
		else
		{
			tick(0xE2A8, 12);
			const uint8_t phase = peekb(0x0330);
			fl::cmpb(phase, 0x01);
			tick(0xE2AE, 30);
			kick = phase != 1;
			tick(kick ? 0xE2B0 : 0xE334, 12);          // BEQ
		}
		if (kick)
		{
			// phase 1, the kick (from the ramp (0x01EA = 2) first back to the ground)
			const uint8_t ramp = peekb(0x01EA);
			fl::cmpb(ramp, 0x02);
			tick(0xE2B6, 30);
			if (ramp != 2)
				tick(0xE30C, 12);                      // BNE
			else
			{
				tick(0xE2B8, 12);
				const uint16_t speed = peek(0x01B4);
				fl::tst(speed);
				tick(0xE2BC, 24);
				if ((int16_t)speed >= 0)
					tick(0xE2C2, 12);                  // BGE
				else
				{
					tick(0xE2BE, 12);
					poke(0x01B4, fl_neg(peek(0x01B4)));
					tick(0xE2C2, 27);
				}
				{
					const uint8_t dir = peekb(0x01F8);
					pokeb(0x031D, dir);
					fl::movb(dir);
				}
				tick(0xE2C8, 39);
				pokeb(0x01EA, 0x01);
				fl::movb(0x01);
				tick(0xE2CE, 33);
				jsr(0xE2D2, 0xE238);                   // reset_skater_state
				push(0x0058);
				fl::mov(0x0058);
				tick(0xE2D6, 30);
				jsr(0xE2DA, 0x8E78);                   // queue_sound(0x58)
				bank_a(0xE2DA, 0x3800);
				bank_b(0xE2E6, 0x3C00);
				jsr(0xE2F6, 0x7E44);
				type_banks(0xE2F6, 0);
				drop(0xE30A);
			}
			pokeb(0x0320, 0x01);
			fl::movb(0x01);
			tick(0xE312, 33);
			pokeb(0x01EB, 0x01);
			fl::movb(0x01);
			tick(0xE318, 33);
			pokeb(0x0330, 0x01);
			fl::movb(0x01);
			tick(0xE31E, 33);
			pokeb(0x07B2, 0);
			fl::clr();
			tick(0xE322, 27);
			T.r[0] = sext8(peekb(0x0331));
			fl::movb((uint8_t)T.r[0]);
			tick(0xE326, 24);
			T.r[0] = sext8(peekb((uint16_t)(0xFE3E + T.r[0])));   // its timer
			fl::movb((uint8_t)T.r[0]);
			tick(0xE32A, 27);
			pokeb(0x0792, (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0xE32E, 27);
			pokeb(0x0772, (uint8_t)T.r[0]);
			fl::movb((uint8_t)T.r[0]);
			tick(0xE332, 27);
			tick(0xE3E2, 12);                          // BR
		}
		else
		{
			// a jump (0x0332 = 2) from the ground on terrain classes 0-2, not in park 3
			const uint8_t jump = peekb(0x0332);
			fl::cmpb(jump, 0x02);
			tick(0xE33A, 30);
			bool go = false;
			if (jump != 2)
				tick(0xE3E2, 12);                      // BNE
			else
			{
				tick(0xE33C, 12);
				const uint8_t ramp = peekb(0x01EA);
				fl::cmpb(ramp, 0x01);
				tick(0xE342, 30);
				if (ramp != 1)
					tick(0xE3E2, 12);                  // BNE
				else
				{
					tick(0xE344, 12);
					T.r[0] = sext8(peekb(0x0327));
					fl::movb((uint8_t)T.r[0]);
					tick(0xE348, 24);
					T.r[0] = fl_logic(T.r[0] & 0x0007);
					tick(0xE34C, 18);
					fl::cmp(T.r[0], 0x0003);
					tick(0xE350, 18);
					if (T.r[0] >= 3)
						tick(0xE3E2, 12);              // BCC
					else
					{
						tick(0xE352, 12);
						const uint8_t level = peekb(0x0F59);
						fl::cmpb(level, 0x03);
						tick(0xE358, 30);
						go = level != 3;
						tick(go ? 0xE35A : 0xE3E2, 12);   // BEQ
					}
				}
			}
			if (go)
			{
				jsr(0xE35E, 0xE238);                   // reset_skater_state
				T.r[0] = 0x0004;
				fl::mov(T.r[0]);
				tick(0xE362, 18);
				pokeb(0x0330, (uint8_t)T.r[0]);
				fl::movb((uint8_t)T.r[0]);
				tick(0xE366, 27);
				fl::cmp(peek(0x05B2), 0x0030);
				tick(0xE36C, 30);
				if ((int16_t)peek(0x05B2) > 0x30)
				{
					// already rising: phase 6, take off with the shoes' extra lift
					tick(0xE36E, 12);                  // BLE not taken
					pokeb(0x01EB, 0x06);
					fl::movb(0x06);
					tick(0xE374, 33);
					pokeb(0x07B1, 0x01);
					fl::movb(0x01);
					tick(0xE37A, 33);
					jsr(0xE37E, 0x9FB4);               // skater_take_off
					bank_b(0xE37E, 0x2C00);
					T.r[0] = sext8(peekb(0x025F));
					fl::movb((uint8_t)T.r[0]);
					tick(0xE38E, 24);
					T.r[1] = 0;
					fl::clr();
					tick(0xE390, 12);
					T.r[1] = fl_logicb(peekb((uint16_t)(0x7C70 + T.r[0])));
					tick(0xE394, 27);
					poke(0x05B2, fl::add(peek(0x05B2), T.r[1]));
					tick(0xE398, 27);
					bank_b(0xE398, 0x0400);
					tick(0xE3E2, 12);                  // BR
				}
				else
				{
					// phase 4: crouch, the speed kept in 0x02E8
					tick(0xE3A6, 12);                  // BLE
					pokeb(0x01EB, 0x04);
					fl::movb(0x04);
					tick(0xE3AC, 33);
					pokeb(0x07B1, 0);
					fl::clr();
					tick(0xE3B0, 27);
					T.r[1] = peek(0x01B4);
					fl::mov(T.r[1]);
					tick(0xE3B4, 24);
					poke(0x02E8, T.r[1]);
					fl::mov(T.r[1]);
					tick(0xE3B8, 27);
					poke(0x01B4, 0);
					fl::clr();
					tick(0xE3BC, 27);
					fl::tst(T.r[1]);
					tick(0xE3BE, 12);
					if ((int16_t)T.r[1] >= 0)
						tick(0xE3C2, 12);              // BGE
					else
					{
						tick(0xE3C0, 12);
						T.r[1] = fl_neg(T.r[1]);
						tick(0xE3C2, 12);
					}
					fl::cmp(T.r[1], 0x01C0);
					tick(0xE3C6, 18);
					if ((int16_t)T.r[1] >= 0x01C0)
					{
						tick(0xE3CE, 12);              // BGE
						T.r[1] = 0x0002;
						fl::mov(T.r[1]);
						tick(0xE3D2, 18);
					}
					else
					{
						tick(0xE3C8, 12);
						T.r[1] = 0x0008;
						fl::mov(T.r[1]);
						tick(0xE3CC, 18);
						tick(0xE3D2, 12);              // BR
					}
					pokeb(0x0791, (uint8_t)T.r[1]);
					fl::movb((uint8_t)T.r[1]);
					tick(0xE3D6, 27);
					pokeb(0x0771, (uint8_t)T.r[1]);
					fl::movb((uint8_t)T.r[1]);
					tick(0xE3DA, 27);
					pokeb(0x0792, (uint8_t)T.r[1]);
					fl::movb((uint8_t)T.r[1]);
					tick(0xE3DE, 27);
					pokeb(0x0772, (uint8_t)T.r[1]);
					fl::movb((uint8_t)T.r[1]);
					tick(0xE3E2, 27);
				}
			}
		}
	}
	// the animation pointers, unless in mode 1 or 5
	bool animate;
	{
		const uint8_t m1 = peekb(0x031F);
		fl::cmpb(m1, 0x01);
		tick(0xE3E8, 30);
		if (m1 == 1)
		{
			tick(0xE3F0, 12);                          // BEQ onto the BNE at E3F0 (not taken)
			tick(0xE3F2, 12);
			animate = false;
		}
		else
		{
			tick(0xE3EA, 12);
			const uint8_t m5 = peekb(0x031F);
			fl::cmpb(m5, 0x05);
			tick(0xE3F0, 30);
			animate = m5 != 5;
			tick(animate ? 0xE3F6 : 0xE3F2, 12);       // BNE
		}
	}
	if (!animate)
		tick(0xE5AE, 18);                              // JMP
	else
	{
		T.r[1] = sext8(peekb(0x01F8));                 // the board's direction (of 8)
		fl::movb((uint8_t)T.r[1]);
		tick(0xE3FA, 24);
		T.r[1] = fl_logic(T.r[1] & 0x0007);
		tick(0xE3FE, 18);
		T.r[0] = T.r[1];
		fl::mov(T.r[0]);
		tick(0xE400, 12);
		T.r[0] = fl_asl(T.r[0]);
		tick(0xE402, 12);
		{
			const uint16_t box = peek((uint16_t)(0x70F0 + T.r[0]));
			poke(0x03F0, box);
			fl::mov(box);
		}
		tick(0xE408, 42);
		push(0);
		fl::clr();
		tick(0xE40A, 24);
		{
			const uint8_t terrain = peekb(0x0327);
			pokeb(T.r[6], terrain);
			fl::movb(terrain);
		}
		tick(0xE40E, 33);
		jsr(0xE412, 0xE5D2);                           // skater_anim_a(terrain)
		T.r[2] = T.r[0];
		fl::mov(T.r[2]);
		tick(0xE414, 12);
		const uint8_t ramp = peekb(0x01EA);
		fl::cmpb(ramp, 0x02);
		tick(0xE41A, 30);
		bool blank = true;
		if (ramp == 2)
			tick(0xE42C, 12);                          // BEQ
		else
		{
			tick(0xE41C, 12);
			T.r[0] = 0x0007;
			fl::mov(T.r[0]);
			tick(0xE420, 18);
			T.r[0] = (uint16_t)((T.r[0] & 0xFF00) | fl_logicb((uint8_t)(T.r[0] & ~peekb(0x0327))));
			tick(0xE424, 24);
			if (!(T.r[0] & 0xFF))
				tick(0xE42C, 12);                      // BEQ
			else
			{
				tick(0xE426, 12);
				poke(0x03B0, T.r[2]);
				fl::mov(T.r[2]);
				tick(0xE42A, 27);
				tick(0xE432, 12);                      // BR
				blank = false;
			}
		}
		if (blank)
		{
			poke(0x03B0, 0x50FA);
			fl::mov(0x50FA);
			tick(0xE432, 33);
		}
		T.r[3] = sext8(peekb(0x01EB));                 // the phase
		fl::movb((uint8_t)T.r[3]);
		tick(0xE436, 24);
		const bool trick = fl_logic(T.r[3] & 0x0040) != 0;
		tick(0xE43A, 18);
		if (trick)
		{
			tick(0xE43C, 12);
			tick(0xE59C, 18);                          // JMP
			T.r[0] = 0x1800;
			fl::mov(T.r[0]);
			tick(0xE5A0, 18);
			poke(0x01D2, T.r[0]);
			fl::mov(T.r[0]);
			tick(0xE5A4, 27);
			poke(0x1400, T.r[0]);
			fl::mov(T.r[0]);
			tick(0xE5A8, 27);
			jsr(0xE5AC, 0x54CE);                       // trick_moves
		}
		else
		{
			tick(0xE440, 12);
			T.r[0] = T.r[3];
			fl::mov(T.r[0]);
			tick(0xE442, 12);
			T.r[0] = fl_asl(T.r[0]);
			tick(0xE444, 12);
			uint16_t at = fl::add(0xE448, peek((uint16_t)(0xFE54 + T.r[0])));   // ADD 0xFE54(R0),PC
			tick(at, 27);
			auto e52a = [&]() {                        // E52A: the next phase
				T.r[0] = T.r[3];
				fl::mov(T.r[0]);
				tick(0xE52C, 12);
				T.r[0] = fl::inc(T.r[0]);
				tick(0xE52E, 12);
				pokeb(0x01EB, (uint8_t)T.r[0]);
				fl::movb((uint8_t)T.r[0]);
				tick(0xE532, 27);
				tick(0xE536, 12);                      // BR
			};
			auto e4fe = [&]() {                        // E4FE: the next phase, 0x07B1 counted
				T.r[0] = T.r[3];
				fl::mov(T.r[0]);
				tick(0xE500, 12);
				T.r[0] = fl::inc(T.r[0]);
				tick(0xE502, 12);
				pokeb(0x01EB, (uint8_t)T.r[0]);
				fl::movb((uint8_t)T.r[0]);
				tick(0xE506, 27);
				pokeb(0x07B1, fl::incb(peekb(0x07B1)));
				tick(0xE50A, 27);
				tick(0xE536, 12);                      // BR
			};
			auto e512 = [&]() {                        // E512: BNE E536, else F:E238
				if (!fZ())
					tick(0xE536, 12);
				else
				{
					tick(0xE514, 12);
					jsr(0xE518, 0xE238);               // reset_skater_state
					tick(0xE536, 12);                  // BR
				}
			};
			switch (at)
			{
			case 0xE448:                               // phase 0: riding
			{
				const bool slope = fl_logicb((uint8_t)(peekb(0x0327) & 0x07)) != 0;
				tick(0xE44E, 30);
				bool cruising = false;
				if (slope)
					tick(0xE486, 12);                  // BNE
				else
				{
					tick(0xE450, 12);
					fl::cmp(peek(0x01C8), 0x0008);
					tick(0xE456, 30);
					cruising = (int16_t)peek(0x01C8) >= 8;
					tick(cruising ? 0xE458 : 0xE486, 12);   // BLT
				}
				if (cruising)
				{
					T.r[0] = sext8(peekb(0x01F8));
					fl::movb((uint8_t)T.r[0]);
					tick(0xE45C, 24);
					T.r[0] = fl_asr(T.r[0]);
					tick(0xE45E, 12);
					T.r[0] = fl_asl(T.r[0]);
					tick(0xE460, 12);
					const uint8_t back = peekb(0x0321);
					fl::tstb(back);
					tick(0xE464, 24);
					if (back & 0x80)
					{
						tick(0xE474, 12);              // BLT
						T.r[1] = T.r[0];
						fl::mov(T.r[1]);
						tick(0xE476, 12);
						T.r[1] = fl::add(T.r[1], 0x42B0);
						tick(0xE47A, 18);
						T.r[2] = T.r[0];
						fl::mov(T.r[2]);
						tick(0xE47C, 12);
						T.r[2] = fl::add(T.r[2], 0x4E1C);
						tick(0xE480, 18);
					}
					else
					{
						tick(0xE466, 12);
						T.r[1] = T.r[0];
						fl::mov(T.r[1]);
						tick(0xE468, 12);
						T.r[1] = fl::add(T.r[1], 0x42C0);
						tick(0xE46C, 18);
						T.r[2] = T.r[0];
						fl::mov(T.r[2]);
						tick(0xE46E, 12);
						T.r[2] = fl::add(T.r[2], 0x4E2C);
						tick(0xE472, 18);
						tick(0xE480, 12);              // BR
					}
					poke(0x03B0, T.r[2]);
					fl::mov(T.r[2]);
					tick(0xE484, 27);
					tick(0xE492, 12);                  // BR
				}
				else
				{
					ww(T.r[6], 0);
					fl::clr();
					tick(0xE488, 21);
					{
						const uint8_t terrain = peekb(0x0327);
						pokeb(T.r[6], terrain);
						fl::movb(terrain);
					}
					tick(0xE48C, 33);
					jsr(0xE490, 0xE648);               // skater_anim_b(terrain)
					T.r[1] = T.r[0];
					fl::mov(T.r[1]);
					tick(0xE492, 12);
				}
				poke(0x03B2, T.r[2]);
				fl::mov(T.r[2]);
				tick(0xE496, 27);
				poke(0x03B4, T.r[1]);
				fl::mov(T.r[1]);
				tick(0xE49A, 27);
				tick(0xE536, 12);                      // BR
				break;
			}
			case 0xE49C:                               // phase 1
			{
				const uint8_t step = peekb(0x07B2);
				fl::cmpb(step, 0x02);
				tick(0xE4A2, 30);
				if (step != 2)
					tick(0xE536, 12);                  // BNE
				else
				{
					tick(0xE4A4, 12);
					tick(0xE52A, 12);                  // BR
					e52a();
				}
				break;
			}
			case 0xE4A6:                               // phases 2 and 3
			{
				const uint8_t step = peekb(0x07B2);
				fl::cmpb(step, 0x03);
				tick(0xE4AC, 30);
				tick(0xE512, 12);                      // BR
				e512();
				break;
			}
			case 0xE4AE:                               // phase 4: the crouch
			{
				const uint8_t n = peekb(0x07B1);
				fl::tstb(n);
				tick(0xE4B2, 24);
				if (!n)
				{
					tick(0xE4C6, 12);                  // BEQ
					poke(0x03B0, 0x50FA);
					fl::mov(0x50FA);
					tick(0xE4CC, 33);
					tick(0xE536, 12);                  // BR
				}
				else
				{
					tick(0xE4B4, 12);
					jsr(0xE4B8, 0xE238);               // reset_skater_state
					pokeb(0x0330, 0x04);
					fl::movb(0x04);
					tick(0xE4BE, 33);
					pokeb(0x01EB, 0x05);
					fl::movb(0x05);
					tick(0xE4C4, 33);
					tick(0xE536, 12);                  // BR
				}
				break;
			}
			case 0xE4CE:                               // phases 5 and 6: until falling
				T.r[0] = peek(0x05B2);
				fl::mov(T.r[0]);
				tick(0xE4D2, 24);
				T.r[0] = fl::sub(T.r[0], peek(0x05B0));
				tick(0xE4D6, 24);
				if (!fN())
					tick(0xE536, 12);                  // BPL
				else
				{
					tick(0xE4D8, 12);
					tick(0xE4FE, 12);                  // BR
					e4fe();
				}
				break;
			case 0xE4DA:                               // phase 7: until near the ground point
				T.r[0] = peek(0x06B2);
				fl::mov(T.r[0]);
				tick(0xE4DE, 24);
				T.r[0] = fl::sub(T.r[0], peek(0x06B0));
				tick(0xE4E2, 24);
				fl::cmp(T.r[0], 0x0600);
				tick(0xE4E6, 18);
				if ((int16_t)T.r[0] > 0x0600)
					tick(0xE536, 12);                  // BGT
				else
				{
					tick(0xE4E8, 12);
					tick(0xE4FE, 12);                  // BR
					e4fe();
				}
				break;
			case 0xE4EA:                               // phase 8: landed
			{
				const uint8_t ramp2 = peekb(0x01EA);
				fl::cmpb(ramp2, 0x01);
				tick(0xE4F0, 30);
				if (ramp2 != 1)
					tick(0xE536, 12);                  // BNE
				else
				{
					tick(0xE4F2, 12);
					T.r[0] = 0x0014;
					fl::mov(T.r[0]);
					tick(0xE4F6, 18);
					pokeb(0x0791, (uint8_t)T.r[0]);
					fl::movb((uint8_t)T.r[0]);
					tick(0xE4FA, 27);
					pokeb(0x0771, (uint8_t)T.r[0]);
					fl::movb((uint8_t)T.r[0]);
					tick(0xE4FE, 27);
					e4fe();
				}
				break;
			}
			case 0xE50C:                               // phase 9
			{
				const uint8_t rate = peekb(0x0791);
				fl::cmpb(rate, 0x02);
				tick(0xE512, 30);
				e512();
				break;
			}
			case 0xE51A:                               // phase 10
			{
				const uint8_t n = peekb(0x07B1);
				fl::cmpb(n, 0x06);
				tick(0xE520, 30);
				if (n != 6)
					tick(0xE536, 12);                  // BNE
				else
				{
					tick(0xE522, 12);
					pokeb(0x0791, 0);
					fl::clr();
					tick(0xE526, 27);
					pokeb(0x0771, 0);
					fl::clr();
					tick(0xE52A, 27);
					e52a();
				}
				break;
			}
			default:                                   // 0xE534, phase 11
				T.r[1] = 0;
				fl::clr();
				tick(0xE536, 12);
				break;
			}
			// the body and helmet animations for the kick (0x0330 = 4) or jump (1) phases
			const uint8_t kind = peekb(0x0330);
			fl::cmpb(kind, 0x04);
			tick(0xE53C, 30);
			if (kind == 4)
			{
				tick(0xE53E, 12);
				T.r[1] = 0;
				fl::clr();
				tick(0xE540, 12);
				T.r[1] = fl_logicb(peekb(0x07B1));
				tick(0xE544, 24);
				T.r[1] = fl_asl(T.r[1]);
				tick(0xE546, 12);
				const uint8_t phase = peekb(0x01EB);
				fl::cmpb(phase, 0x08);
				tick(0xE54C, 30);
				if ((int8_t)phase > 8)
				{
					tick(0xE560, 12);                  // BGT
					{
						const uint16_t v = peek(0x03B0);
						poke(0x03B2, v);
						fl::mov(v);
					}
					tick(0xE566, 39);
				}
				else
				{
					tick(0xE54E, 12);
					T.r[0] = sext8(peekb(0x01F8));
					fl::movb((uint8_t)T.r[0]);
					tick(0xE552, 24);
					T.r[0] = fl_asl(T.r[0]);
					tick(0xE554, 12);
					{
						const uint16_t v = peek((uint16_t)(0x4F44 + T.r[0]));
						poke(0x03B2, v);
						fl::mov(v);
					}
					tick(0xE55A, 42);
					poke(0x03B2, fl::add(peek(0x03B2), T.r[1]));
					tick(0xE55E, 27);
					tick(0xE566, 12);                  // BR
				}
				T.r[0] = sext8(peekb(0x01F8));
				fl::movb((uint8_t)T.r[0]);
				tick(0xE56A, 24);
				T.r[0] = fl_asl(T.r[0]);
				tick(0xE56C, 12);
				{
					const uint16_t v = peek((uint16_t)(0x43C8 + T.r[0]));
					poke(0x03B4, v);
					fl::mov(v);
				}
				tick(0xE572, 42);
				tick(0xE596, 12);                      // BR
				poke(0x03B4, fl::add(peek(0x03B4), T.r[1]));
				tick(0xE59A, 27);
				tick(0xE5AC, 12);                      // BR
			}
			else
			{
				tick(0xE574, 12);
				const uint8_t kind1 = peekb(0x0330);
				fl::cmpb(kind1, 0x01);
				tick(0xE57A, 30);
				if (kind1 != 1)
					tick(0xE5AC, 12);                  // BNE
				else
				{
					tick(0xE57C, 12);
					{
						const uint16_t v = peek(0x03B0);
						poke(0x03B2, v);
						fl::mov(v);
					}
					tick(0xE582, 39);
					T.r[1] = 0;
					fl::clr();
					tick(0xE584, 12);
					T.r[1] = fl_logicb(peekb(0x07B2));
					tick(0xE588, 24);
					T.r[1] = fl_asl(T.r[1]);
					tick(0xE58A, 12);
					T.r[0] = sext8(peekb(0x01F8));
					fl::movb((uint8_t)T.r[0]);
					tick(0xE58E, 24);
					T.r[0] = fl_asl(T.r[0]);
					tick(0xE590, 12);
					{
						const uint16_t v = peek((uint16_t)(0x4330 + T.r[0]));
						poke(0x03B4, v);
						fl::mov(v);
					}
					tick(0xE596, 42);
					poke(0x03B4, fl::add(peek(0x03B4), T.r[1]));
					tick(0xE59A, 27);
					tick(0xE5AC, 12);                  // BR
				}
			}
		}
		drop(0xE5AC);
	}
	// the helmet's animation
	const uint8_t helmet = peekb(0x025D);
	fl::tstb(helmet);
	tick(0xE5B2, 24);
	if (helmet)
	{
		tick(0xE5B4, 12);
		const uint8_t m5 = peekb(0x031F);
		fl::cmpb(m5, 0x05);
		tick(0xE5BA, 30);
		if (m5 != 5)
		{
			tick(0xE5BC, 12);
			{
				const uint16_t v = peek(0x03B4);
				poke(0x03B6, v);
				fl::mov(v);
			}
			tick(0xE5C2, 39);
			poke(0x03B6, fl::add(peek(0x03B6), 0x061E));
			tick(0xE5C8, 33);
			rts();
			return;
		}
		tick(0xE5CA, 12);                              // BEQ
	}
	else
		tick(0xE5CA, 12);                              // BEQ
	poke(0x03B6, 0x50FA);
	fl::mov(0x50FA);
	tick(0xE5D0, 33);
	rts();
}

// ---------------------------------------------------------------- packed tile blocks

// Packed tile runs (F:EAD4 and F:E82E).  A run starts with a byte: bits 0-5 the number of
// tiles (0: 65536), bits 6/7 the mode, then the tile's low byte (0x0F4C); mode 2 also has
// the high byte (0x0F4D).  The following tiles of the run: mode 0 a new low byte each, mode 1
// the same tile, mode 2 two new bytes each, mode 3 the tile number + 1 each.  The mode (the
// head byte's bits 7-6) is kept in 0x0F5F (0-3).

// F:EAD4  draw_tile_block(int erase, int *block): block = {packed data, video address (low 7
// bits: column * 2), rows, columns}; the pointer argument is advanced past the header.  Tiles
// get the colour bits 0x0188 (or are cleared when erase is set); tile 0x149 is transparent.
// Video bank 0 is selected; the banks are restored.
uint16_t draw_tile_block(const Args &)
{
	c_save(4, 0xEAD8);
	T.r[6] = fl::sub(T.r[6], 0x000C);                  // SUB #0xC,SP: locals
	tick(0xEADC, 18);
	{
		const uint16_t v = peek(0x01D2);               // 0xA(SP), 0x8(SP): the banks
		fl::mov(v);
		ww(sp_at(0x0A), v);
	}
	tick(0xEAE2, 42);
	{
		const uint16_t v = peek(0x01D4);
		fl::mov(v);
		ww(sp_at(0x08), v);
	}
	tick(0xEAE8, 42);
	bank_a(0xEAE8, 0x1800);
	bank_b(0xEAF4, 0x1C00);
	jsr(0xEB04, 0x8068);                               // select_video_bank0
	// the header, through the pointer argument 0x1C(SP), which is advanced past it
	T.r[3] = peek(rw(sp_at(0x1C)));                    // the packed tiles
	fl::mov(T.r[3]);
	tick(0xEB08, 33);
	ww(sp_at(0x1C), fl::add(rw(sp_at(0x1C)), 0x0002));
	tick(0xEB0E, 36);
	T.r[2] = peek(rw(sp_at(0x1C)));                    // the video address: row
	fl::mov(T.r[2]);
	tick(0xEB12, 33);
	T.r[2] = fl_logic(T.r[2] & 0xFF80);
	tick(0xEB16, 18);
	T.r[1] = peek(rw(sp_at(0x1C)));                    // and column
	fl::mov(T.r[1]);
	tick(0xEB1A, 33);
	T.r[1] = fl_logic(T.r[1] & 0x007E);
	tick(0xEB1E, 18);
	ww(sp_at(0x1C), fl::add(rw(sp_at(0x1C)), 0x0002));
	tick(0xEB24, 36);
	T.r[4] = peek(rw(sp_at(0x1C)));                    // rows
	fl::mov(T.r[4]);
	tick(0xEB28, 33);
	ww(sp_at(0x1C), fl::add(rw(sp_at(0x1C)), 0x0002));
	tick(0xEB2E, 36);
	{
		const uint16_t v = peek(rw(sp_at(0x1C)));      // 6(SP): columns
		fl::mov(v);
		ww(sp_at(6), v);
	}
	tick(0xEB34, 51);
	T.r[0] = 0;
	fl::clr();
	tick(0xEB36, 12);
	poke(0x0F4C, 0);
	fl::clr();
	tick(0xEB3A, 27);
	{
		const uint16_t v = rw(sp_at(6));               // 4(SP): the row's bytes
		fl::mov(v);
		ww(sp_at(4), v);
	}
	tick(0xEB40, 45);
	ww(sp_at(4), fl_asl(rw(sp_at(4))));
	tick(0xEB44, 30);
	ww(sp_at(2), T.r[4]);                              // 2(SP): rows left
	fl::mov(T.r[4]);
	tick(0xEB48, 30);
	if ((int16_t)T.r[4] <= 0)
		tick(0xEC02, 12);                              // BLE
	else
	{
		tick(0xEB4A, 12);
		for (;;)
		{
			const uint16_t cols = rw(sp_at(6));        // (SP): columns left
			fl::mov(cols);
			ww(T.r[6], cols);
			tick(0xEB4E, 36);
			if ((int16_t)cols <= 0)
				tick(0xEBF4, 12);                      // BLE
			else
			{
				tick(0xEB50, 12);
				for (;;)
				{
					fl::tst(T.r[0]);
					tick(0xEB52, 12);
					uint16_t to;
					if (T.r[0] == 0)
					{
						// a new run (see the packed tile runs)
						tick(0xEB54, 12);
						T.r[0] = sext8(peekb(T.r[3]));
						fl::movb((uint8_t)T.r[0]);
						T.r[3]++;
						tick(0xEB56, 18);
						pokeb(0x0F5F, (uint8_t)T.r[0]);
						fl::movb((uint8_t)T.r[0]);
						tick(0xEB5A, 27);
						T.r[0] = fl_logic(T.r[0] & 0x003F);
						tick(0xEB5E, 18);
						{
							const uint8_t v = peekb(T.r[3]);
							fl::movb(v);
							pokeb(0x0F4C, v);
							T.r[3]++;
						}
						tick(0xEB62, 33);
						T.r[4] = sext8(peekb(0x0F5F));
						fl::movb((uint8_t)T.r[4]);
						tick(0xEB66, 24);
						T.r[5] = 0;
						fl::clr();
						tick(0xEB68, 12);
						const bool b6 = fl_logic(T.r[4] & 0x0040) != 0;
						tick(0xEB6C, 18);
						if (!b6)
							tick(0xEB70, 12);          // BEQ
						else
						{
							tick(0xEB6E, 12);
							T.r[5] = fl::inc(T.r[5]);
							tick(0xEB70, 12);
						}
						fl::tst(T.r[4]);
						tick(0xEB72, 12);
						if ((int16_t)T.r[4] >= 0)
						{
							tick(0xEB8A, 12);          // BGE
							T.r[5] = fl_ror(T.r[5]);
							tick(0xEB8C, 12);
							if (!C())
							{
								tick(0xEB96, 12);      // BCC: mode 0
								pokeb(0x0F5F, 0);
								fl::clr();
								tick(0xEB9A, 27);
								tick(0xEBBC, 12);      // BR
							}
							else
							{
								tick(0xEB8E, 12);      // mode 1
								pokeb(0x0F5F, 0x01);
								fl::movb(0x01);
								tick(0xEB94, 33);
								tick(0xEBBC, 12);      // BR
							}
							to = 0xEBBC;
						}
						else
						{
							tick(0xEB74, 12);
							const bool b0 = fl_logic(T.r[5] & 0x0001) != 0;
							tick(0xEB78, 18);
							if (!b0)
							{
								tick(0xEB82, 12);      // BEQ: mode 2, the high byte too
								pokeb(0x0F5F, 0x02);
								fl::movb(0x02);
								tick(0xEB88, 33);
								tick(0xEBB0, 12);      // BR
								to = 0xEBB0;
							}
							else
							{
								tick(0xEB7A, 12);      // mode 3
								pokeb(0x0F5F, 0x03);
								fl::movb(0x03);
								tick(0xEB80, 33);
								tick(0xEBBC, 12);      // BR
								to = 0xEBBC;
							}
						}
					}
					else
					{
						// the next tile of the run, by its mode (jump table at 0xFF0A)
						tick(0xEB9C, 12);              // BNE
						T.r[4] = sext8(peekb(0x0F5F));
						fl::movb((uint8_t)T.r[4]);
						tick(0xEBA0, 24);
						T.r[4] = fl_asl(T.r[4]);
						tick(0xEBA2, 12);
						const uint16_t off = peek((uint16_t)(0xFF0A + T.r[4]));
						to = fl::add(0xEBA6, off);
						tick(to, 27);
					}
					switch (to)
					{
					case 0xEBA6:                       // mode 0: a new low byte
						{
							const uint8_t v = peekb(T.r[3]);
							fl::movb(v);
							pokeb(0x0F4C, v);
						}
						tick(0xEBAA, 33);
						tick(0xEBB4, 12);              // BR
						T.r[3] = fl::inc(T.r[3]);
						tick(0xEBB6, 12);
						tick(0xEBBC, 12);              // BR
						to = 0xEBBC;
						break;
					case 0xEBAC:                       // mode 2: two new bytes
						{
							const uint8_t v = peekb(T.r[3]);
							fl::movb(v);
							pokeb(0x0F4C, v);
							T.r[3]++;
						}
						tick(0xEBB0, 33);
						to = 0xEBB0;
						break;
					case 0xEBB8:                       // mode 3: the next tile number
						poke(0x0F4C, fl::inc(peek(0x0F4C)));
						tick(0xEBBC, 27);
						to = 0xEBBC;
						break;
					case 0xEBB0:
					case 0xEBBC:                       // mode 1: the same tile again
						break;
					default:
						leave_to(to);
					}
					if (to == 0xEBB0)
					{
						{
							const uint8_t v = peekb(T.r[3]);
							fl::movb(v);
							pokeb(0x0F4D, v);
						}
						tick(0xEBB4, 33);
						T.r[3] = fl::inc(T.r[3]);
						tick(0xEBB6, 12);
						tick(0xEBBC, 12);              // BR
					}
					T.r[4] = peek(0x0F4C);
					fl::mov(T.r[4]);
					tick(0xEBC0, 24);
					T.r[4] = fl_logic(T.r[4] & 0x03FF);
					tick(0xEBC4, 18);
					fl::cmp(T.r[4], 0x0149);
					tick(0xEBC8, 18);
					if (T.r[4] == 0x0149)
						tick(0xEBEA, 12);              // BEQ: tile 0x149 is transparent
					else
					{
						tick(0xEBCA, 12);
						const uint16_t erase = rw(sp_at(0x1A));
						fl::tst(erase);
						tick(0xEBCE, 27);
						if (erase)
						{
							tick(0xEBE2, 12);          // BNE
							T.r[4] = 0;
							fl::clr();
							tick(0xEBE4, 12);
							T.r[4] = fl_logicb((uint8_t)T.r[1]);
							tick(0xEBE6, 12);
							T.r[4] = fl_logic(T.r[4] | T.r[2]);
							tick(0xEBE8, 12);
							poke(T.r[4], 0);
							fl::clr();
							tick(0xEBEA, 21);
						}
						else
						{
							tick(0xEBD0, 12);
							poke(0x0F4C, fl_logic(peek(0x0F4C) | peek(0x0188)));   // the colour bits
							tick(0xEBD6, 39);
							T.r[4] = 0;
							fl::clr();
							tick(0xEBD8, 12);
							T.r[4] = fl_logicb((uint8_t)T.r[1]);
							tick(0xEBDA, 12);
							T.r[4] = fl_logic(T.r[4] | T.r[2]);
							tick(0xEBDC, 12);
							{
								const uint16_t v = peek(0x0F4C);
								fl::mov(v);
								poke(T.r[4], v);
							}
							tick(0xEBE0, 33);
							tick(0xEBEA, 12);          // BR
						}
					}
					T.r[1] = fl::add(T.r[1], 0x0002);
					tick(0xEBEE, 18);
					T.r[0] = fl::dec(T.r[0]);
					tick(0xEBF0, 12);
					const uint16_t left = fl::dec(rw(T.r[6]));
					ww(T.r[6], left);
					tick(0xEBF2, 21);
					if (!left)
					{
						tick(0xEBF4, 12);              // BNE not taken
						break;
					}
					tick(0xEB50, 12);
				}
			}
			T.r[1] = fl::sub(T.r[1], rw(sp_at(4)));    // the next row
			tick(0xEBF8, 27);
			T.r[2] = fl::add(T.r[2], 0x0080);
			tick(0xEBFC, 18);
			const uint16_t rows = fl::dec(rw(sp_at(2)));
			ww(sp_at(2), rows);
			tick(0xEC00, 30);
			if (!rows)
			{
				tick(0xEC02, 12);                      // BNE not taken
				break;
			}
			tick(0xEB4A, 12);
		}
	}
	{
		const uint16_t v = rw(sp_at(0x0A));            // the banks back
		fl::mov(v);
		poke(0x01D2, v);
	}
	tick(0xEC08, 42);
	{
		const uint16_t v = rw(sp_at(0x08));
		fl::mov(v);
		poke(0x01D4, v);
	}
	tick(0xEC0E, 42);
	{
		const uint16_t v = peek(0x01D2);
		fl::mov(v);
		poke(0x1400, v);
	}
	tick(0xEC14, 39);
	{
		const uint16_t v = peek(0x01D4);
		fl::mov(v);
		poke(0x1402, v);
	}
	tick(0xEC1A, 39);
	T.r[6] = fl::add(T.r[6], 0x000C);
	tick(0xEC1E, 18);
	c_restore(4);
	return T.r[0];
}

// F:EC20  draw_map_object(int i): object record i at 0x4B58 (bank 0x8000): {type, shape,
// column, row}.  The shape gives the size (0x4BCB columns x 0x4BC0 rows), the bank base
// (0x4BD6) and the frame table (0x4BE2); the frame comes from the type: 0-3 F:90BA with
// 0x027A = type, 4-7 the ticket count 0x0258[type] (0-2), 8-11 0xFE7C[0x0F31] - 1; objects
// of types 4-11 whose ticket bit (0x0ED7) is set are not drawn.  The frame's tiles are drawn
// into the playfield at the map position (0x0F42/0x0F45), keeping the colour bits, inside
// the playfield half of 0x0F42; zero tiles are skipped (the column does not advance).
uint16_t draw_map_object(const Args &)
{
	c_save(4, 0xEC24);
	{
		const uint16_t s = rw((uint16_t)(T.r[6] - 2)); // CMP -(SP),-(SP): two locals
		const uint16_t d = rw((uint16_t)(T.r[6] - 4));
		T.r[6] = (uint16_t)(T.r[6] - 4);
		fl::cmp(s, d);
	}
	tick(0xEC26, 30);
	bank_a(0xEC26, 0x8000);
	T.r[0] = rw(sp_at(0x12));                          // the object record i
	fl::mov(T.r[0]);
	tick(0xEC36, 27);
	T.r[0] = fl_asl(T.r[0]);
	tick(0xEC38, 12);
	T.r[0] = fl_asl(T.r[0]);
	tick(0xEC3A, 12);
	T.r[0] = fl::add(T.r[0], 0x4B58);
	tick(0xEC3E, 18);
	T.r[3] = T.r[0];
	fl::mov(T.r[3]);
	tick(0xEC40, 12);
	T.r[4] = 0;
	fl::clr();
	tick(0xEC42, 12);
	T.r[4] = fl_logicb(peekb(T.r[3]));                 // the type
	T.r[3]++;
	tick(0xEC44, 18);
	T.r[1] = 0;
	fl::clr();
	tick(0xEC46, 12);
	T.r[1] = fl_logicb(peekb(T.r[3]));                 // the shape
	tick(0xEC48, 18);
	{
		const uint8_t v = peekb((uint16_t)(0x4BC0 + T.r[1]));   // 2(SP): rows
		T.r[6] = (uint16_t)(T.r[6] - 2);
		pokeb(T.r[6], v);
		fl::movb(v);
	}
	tick(0xEC4C, 39);
	pokeb(sp_at(1), 0);
	fl::clr();
	tick(0xEC50, 30);
	{
		const uint8_t v = peekb((uint16_t)(0x4BCB + T.r[1]));   // (SP): columns
		T.r[6] = (uint16_t)(T.r[6] - 2);
		pokeb(T.r[6], v);
		fl::movb(v);
	}
	tick(0xEC54, 39);
	pokeb(sp_at(1), 0);
	fl::clr();
	tick(0xEC58, 30);
	const uint8_t high = peekb((uint16_t)(0x4BD6 + T.r[1]));
	fl::tstb(high);
	tick(0xEC5C, 27);
	if (high)
	{
		tick(0xEC64, 12);                              // BNE
		T.r[5] = 0x6000;
		fl::mov(T.r[5]);
		tick(0xEC68, 18);
	}
	else
	{
		tick(0xEC5E, 12);
		T.r[5] = 0x4000;
		fl::mov(T.r[5]);
		tick(0xEC62, 18);
		tick(0xEC68, 12);                              // BR
	}
	T.r[0] = T.r[1];
	fl::mov(T.r[0]);
	tick(0xEC6A, 12);
	T.r[0] = fl_asl(T.r[0]);
	tick(0xEC6C, 12);
	T.r[1] = peek((uint16_t)(0x4BE2 + T.r[0]));        // the frame table
	fl::mov(T.r[1]);
	tick(0xEC70, 27);
	T.r[3] = fl::inc(T.r[3]);
	tick(0xEC72, 12);
	T.r[0] = 0;
	fl::clr();
	tick(0xEC74, 12);
	T.r[0] = fl_logicb(peekb(T.r[3]));                 // the column
	T.r[3]++;
	tick(0xEC76, 18);
	T.r[0] = fl_asl(T.r[0]);
	tick(0xEC78, 12);
	T.r[2] = 0;
	fl::clr();
	tick(0xEC7A, 12);
	T.r[2] = fl_logicb(peekb(0x0F45));
	tick(0xEC7E, 24);
	T.r[0] = fl::add(T.r[0], T.r[2]);
	tick(0xEC80, 12);
	ww(sp_at(6), T.r[0]);                              // 6(SP): the first column's byte
	fl::mov(T.r[0]);
	tick(0xEC84, 30);
	T.r[0] = 0;
	fl::clr();
	tick(0xEC86, 12);
	T.r[0] = fl_logicb(peekb(T.r[3]));                 // the row
	tick(0xEC88, 18);
	T.r[0] = fl_swab(T.r[0]);
	tick(0xEC8A, 12);
	T.r[0] &= 0xFF00;                                  // CLRB
	fl::clr();
	tick(0xEC8C, 12);
	T.r[0] = fl::add(T.r[0], peek(0x0F42));
	tick(0xEC90, 24);
	T.r[2] = T.r[0];
	fl::mov(T.r[2]);
	tick(0xEC92, 12);
	T.r[0] = T.r[4];
	fl::mov(T.r[0]);
	tick(0xEC94, 12);
	T.r[0] = fl_asl(T.r[0]);
	tick(0xEC96, 12);
	// the frame, by the type (jump table at 0xFF12)
	const uint16_t off = peek((uint16_t)(0xFF12 + T.r[0]));
	const uint16_t to = fl::add(0xEC9A, off);
	tick(to, 27);
	bool shown = true;                                 // false: a ticket already taken
	switch (to)
	{
	case 0xEC9A:                                       // types 0-3: F:90BA with 0x027A = type
		ww(sp_at(4), 0);
		fl::clr();
		tick(0xEC9E, 30);
		{
			const uint8_t v = peekb(0x027A);
			fl::movb(v);
			pokeb(sp_at(4), v);
		}
		tick(0xECA4, 42);
		pokeb(0x027A, (uint8_t)T.r[4]);
		fl::movb((uint8_t)T.r[4]);
		tick(0xECA8, 27);
		jsr(0xECAC, 0x90BA);
		T.r[3] = T.r[0];
		fl::mov(T.r[3]);
		tick(0xECAE, 12);
		T.r[3] = fl::dec(T.r[3]);
		tick(0xECB0, 12);
		{
			const uint8_t v = peekb(sp_at(4));
			fl::movb(v);
			pokeb(0x027A, v);
		}
		tick(0xECB6, 42);
		tick(0xECE6, 12);                              // BR
		break;
	case 0xECB8:                                       // types 4-7: the ticket count 0x0258[type]
		{
			const bool taken = fl_logicb((uint8_t)(peekb(0x0ED7) & peekb((uint16_t)(0xFC70 + T.r[4])))) != 0;
			tick(0xECBE, 39);
			if (taken)
			{
				tick(0xEDAC, 12);                      // BNE
				shown = false;
				break;
			}
		}
		tick(0xECC0, 12);
		T.r[3] = sext8(peekb((uint16_t)(0x0258 + T.r[4])));
		fl::movb((uint8_t)T.r[3]);
		tick(0xECC4, 27);
		if ((int16_t)T.r[3] < 0)
			tick(0xECCC, 12);                          // BLT
		else
		{
			tick(0xECC6, 12);
			fl::cmp(T.r[3], 0x0003);
			tick(0xECCA, 18);
			if ((int16_t)T.r[3] < 3)
			{
				tick(0xECE6, 12);                      // BLT
				break;
			}
			tick(0xECCC, 12);
		}
		T.r[3] = 0;
		fl::clr();
		tick(0xECCE, 12);
		tick(0xECE6, 12);                              // BR
		break;
	case 0xECD0:                                       // types 8-11: 0xFE7C[0x0F31] - 1
		{
			const bool taken = fl_logicb((uint8_t)(peekb(0x0ED7) & peekb((uint16_t)(0xFC6C + T.r[4])))) != 0;
			tick(0xECD6, 39);
			if (taken)
			{
				tick(0xEDAC, 12);                      // BNE
				shown = false;
				break;
			}
		}
		tick(0xECD8, 12);
		T.r[0] = 0;
		fl::clr();
		tick(0xECDA, 12);
		T.r[0] = fl_logicb(peekb(0x0F31));
		tick(0xECDE, 24);
		T.r[3] = 0;
		fl::clr();
		tick(0xECE0, 12);
		T.r[3] = fl_logicb(peekb((uint16_t)(0xFE7C + T.r[0])));
		tick(0xECE4, 27);
		T.r[3] = fl::dec(T.r[3]);
		tick(0xECE6, 12);
		break;
	default:                                           // types 12-15 jump into other code
		leave_to(to);
	}
	if (shown)
	{
		T.r[0] = T.r[3];
		fl::mov(T.r[0]);
		tick(0xECE8, 12);
		T.r[0] = fl_asl(T.r[0]);
		tick(0xECEA, 12);
		T.r[0] = fl::add(T.r[0], T.r[1]);
		tick(0xECEC, 12);
		T.r[3] = peek((uint16_t)(0x4000 + T.r[0]));    // the frame: its bank and address
		fl::mov(T.r[3]);
		tick(0xECF0, 27);
		T.r[0] = T.r[3];
		fl::mov(T.r[0]);
		tick(0xECF2, 12);
		T.r[0] = fl_asr(T.r[0]);
		tick(0xECF4, 12);
		T.r[0] = fl_asr(T.r[0]);
		tick(0xECF6, 12);
		T.r[0] = fl_asr(T.r[0]);
		tick(0xECF8, 12);
		T.r[0] = fl_logic(T.r[0] & 0x1C00);
		tick(0xECFC, 18);
		T.r[0] = fl::add(T.r[0], T.r[5]);
		tick(0xECFE, 12);
		T.r[5] = T.r[0];
		fl::mov(T.r[5]);
		tick(0xED00, 12);
		poke(0x01D2, T.r[5]);
		fl::mov(T.r[5]);
		tick(0xED04, 27);
		T.r[0] = fl::add(T.r[0], 0x0400);
		tick(0xED08, 18);
		poke(0x01D4, T.r[0]);
		fl::mov(T.r[0]);
		tick(0xED0C, 27);
		{
			const uint16_t v = peek(0x01D2);
			fl::mov(v);
			poke(0x1400, v);
		}
		tick(0xED12, 39);
		{
			const uint16_t v = peek(0x01D4);
			fl::mov(v);
			poke(0x1402, v);
		}
		tick(0xED18, 39);
		T.r[3] = fl_logic(T.r[3] & 0x1FFF);
		tick(0xED1C, 18);
		T.r[3] = fl::add(T.r[3], 0x4000);
		tick(0xED20, 18);
		T.r[4] = peek(0x0F42);                         // the playfield half
		fl::mov(T.r[4]);
		tick(0xED24, 24);
		T.r[4] = fl_logic(T.r[4] & 0x0800);
		tick(0xED28, 18);
		if (!T.r[4])
			tick(0xED44, 12);                          // BEQ
		else
		{
			// the right half: the rows above it are skipped
			tick(0xED2A, 12);
			T.r[2] = fl::sub(T.r[2], 0x0800);
			tick(0xED2E, 18);
			T.r[0] = rw(T.r[6]);
			fl::mov(T.r[0]);
			tick(0xED30, 18);
			T.r[0] = fl_asl(T.r[0]);
			tick(0xED32, 12);
			for (;;)
			{
				const bool in = fl_logic(T.r[2] & 0x0800) != 0;
				tick(0xED36, 18);
				if (in)
				{
					tick(0xED44, 12);                  // BNE
					break;
				}
				tick(0xED38, 12);
				T.r[2] = fl::add(T.r[2], 0x0100);
				tick(0xED3C, 18);
				ww(sp_at(2), fl::dec(rw(sp_at(2))));
				tick(0xED40, 30);
				T.r[3] = fl::add(T.r[3], T.r[0]);
				tick(0xED42, 12);
				tick(0xED32, 12);                      // BR
			}
		}
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0xED46, 12);
		jsr(0xED4A, 0x8078);                           // select_video_bank
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0xED4C, 12);
		T.r[0] = fl_logic(T.r[0] & 0x1F00);
		tick(0xED50, 18);
		T.r[2] = T.r[0];
		fl::mov(T.r[2]);
		tick(0xED52, 12);
		T.r[2] = fl::add(T.r[2], 0x2000);
		tick(0xED56, 18);
		const uint16_t rows = rw(sp_at(2));
		fl::tst(rows);
		tick(0xED5A, 27);
		if ((int16_t)rows <= 0)
			tick(0xEDAC, 12);                          // BLE
		else
		{
			tick(0xED5C, 12);
			for (;;)
			{
				T.r[5] = T.r[2];
				fl::mov(T.r[5]);
				tick(0xED5E, 12);
				T.r[5] = fl_logic(T.r[5] & 0x0800);
				tick(0xED62, 18);
				fl::cmp(T.r[5], T.r[4]);
				tick(0xED64, 12);
				if (T.r[5] != T.r[4])
				{
					tick(0xEDAC, 12);                  // BNE: out of the playfield half
					break;
				}
				tick(0xED66, 12);
				T.r[1] = rw(sp_at(6));
				fl::mov(T.r[1]);
				tick(0xED6A, 27);
				const uint16_t cols = rw(T.r[6]);      // 4(SP): columns left
				fl::mov(cols);
				ww(sp_at(4), cols);
				tick(0xED6E, 36);
				if ((int16_t)cols <= 0)
					tick(0xEDA2, 12);                  // BLE
				else
				{
					tick(0xED70, 12);
					for (;;)
					{
						T.r[0] = peek(T.r[3]);
						fl::mov(T.r[0]);
						T.r[3] = (uint16_t)(T.r[3] + 2);
						tick(0xED72, 18);
						if (!T.r[0])
							tick(0xED9C, 12);          // BEQ: no tile
						else
						{
							tick(0xED74, 12);
							fl::cmp(T.r[0], 0x0800);
							tick(0xED78, 18);
							if ((int16_t)T.r[0] < 0x0800)
								tick(0xED82, 12);      // BLT
							else
							{
								tick(0xED7A, 12);
								T.r[0] = fl_logic(T.r[0] & 0x03FF);
								tick(0xED7E, 18);
								T.r[0] = fl::add(T.r[0], 0x0400);
								tick(0xED82, 18);
							}
							T.r[5] = 0;
							fl::clr();
							tick(0xED84, 12);
							T.r[5] = fl_logicb((uint8_t)T.r[1]);
							tick(0xED86, 12);
							T.r[5] = fl_logic(T.r[5] | T.r[2]);
							tick(0xED88, 12);
							T.r[5] = peek(T.r[5]);     // keeping the colour bits
							fl::mov(T.r[5]);
							tick(0xED8A, 18);
							T.r[5] = fl_logic(T.r[5] & 0xF800);
							tick(0xED8E, 18);
							T.r[0] = fl_logic(T.r[0] | T.r[5]);
							tick(0xED90, 12);
							T.r[5] = 0;
							fl::clr();
							tick(0xED92, 12);
							T.r[5] = fl_logicb((uint8_t)T.r[1]);
							tick(0xED94, 12);
							T.r[5] = fl_logic(T.r[5] | T.r[2]);
							tick(0xED96, 12);
							poke(T.r[5], T.r[0]);
							fl::mov(T.r[0]);
							tick(0xED98, 21);
							T.r[1] = fl::add(T.r[1], 0x0002);
							tick(0xED9C, 18);
						}
						const uint16_t left = fl::dec(rw(sp_at(4)));
						ww(sp_at(4), left);
						tick(0xEDA0, 30);
						if (!left)
						{
							tick(0xEDA2, 12);          // BNE not taken
							break;
						}
						tick(0xED70, 12);
					}
				}
				T.r[2] = fl::add(T.r[2], 0x0100);
				tick(0xEDA6, 18);
				const uint16_t more = fl::dec(rw(sp_at(2)));
				ww(sp_at(2), more);
				tick(0xEDAA, 30);
				if (!more)
				{
					tick(0xEDAC, 12);                  // BNE not taken
					break;
				}
				tick(0xED5C, 12);
			}
		}
	}
	T.r[6] = fl::add(T.r[6], 0x0008);
	tick(0xEDB0, 18);
	c_restore(4);
	return T.r[0];
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
	T.r[6] = fl::sub(T.r[6], 0x000A);                  // SUB #0xA,SP: locals
	tick(0xE832, 18);
	jsr(0xE836, 0x9152);                               // next_random
	T.r[0] = fl::add(T.r[0], peek(0x0FA0));
	tick(0xE83A, 24);
	poke(0x0FA0, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xE83E, 27);
	T.r[0] = peek(0x0F42);
	fl::mov(T.r[0]);
	tick(0xE842, 24);
	jsr(0xE846, 0x8078);                               // select_video_bank
	// the map: the cell's block number
	T.r[4] = peek(0x0F48);
	fl::mov(T.r[4]);
	tick(0xE84A, 24);
	T.r[0] = T.r[4];
	fl::mov(T.r[0]);
	tick(0xE84C, 12);
	T.r[0] = fl_asr(T.r[0]);
	tick(0xE84E, 12);
	T.r[0] = fl_asr(T.r[0]);
	tick(0xE850, 12);
	T.r[0] = fl_asr(T.r[0]);
	tick(0xE852, 12);
	T.r[0] = fl_logic(T.r[0] & 0x1C00);
	tick(0xE856, 18);
	T.r[5] = 0;
	fl::clr();
	tick(0xE858, 12);
	T.r[5] = fl_logicb(peekb(0x0F59));                 // the level
	tick(0xE85C, 24);
	T.r[3] = T.r[5];
	fl::mov(T.r[3]);
	tick(0xE85E, 12);
	T.r[3] = fl_asl(T.r[3]);
	tick(0xE860, 12);
	T.r[0] = fl::add(T.r[0], peek((uint16_t)(0xFEB2 + T.r[3])));
	tick(0xE864, 27);
	T.r[2] = T.r[0];
	fl::mov(T.r[2]);
	tick(0xE866, 12);
	poke(0x01D2, T.r[2]);
	fl::mov(T.r[2]);
	tick(0xE86A, 27);
	T.r[0] = fl::add(T.r[0], 0x0400);
	tick(0xE86E, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xE872, 27);
	{
		const uint16_t v = peek(0x01D2);
		fl::mov(v);
		poke(0x1400, v);
	}
	tick(0xE878, 39);
	{
		const uint16_t v = peek(0x01D4);
		fl::mov(v);
		poke(0x1402, v);
	}
	tick(0xE87E, 39);
	T.r[4] = fl_logic(T.r[4] & 0x1FFF);
	tick(0xE882, 18);
	T.r[4] = fl::add(T.r[4], 0x4000);
	tick(0xE886, 18);
	T.r[4] = peek(T.r[4]);
	fl::mov(T.r[4]);
	tick(0xE888, 18);
	T.r[1] = 0x2C00;
	fl::mov(T.r[1]);
	tick(0xE88C, 18);
	poke(0x01D4, T.r[1]);
	fl::mov(T.r[1]);
	tick(0xE890, 27);
	poke(0x1402, T.r[1]);
	fl::mov(T.r[1]);
	tick(0xE894, 27);
	// the block's bank
	const bool alt = fl_logicb((uint8_t)(peekb(peek(0x0F56)) & peekb(0x0F58))) != 0;
	tick(0xE89A, 45);
	if (alt)
	{
		tick(0xE8B6, 12);                              // BNE: the town's alternate cells
		T.r[2] = g_patch_town_alt_base;
		fl::mov(T.r[2]);
		tick(0xE8BA, 18);
	}
	else
	{
		tick(0xE89C, 12);
		T.r[2] = peek((uint16_t)(0xFEA2 + T.r[3]));
		fl::mov(T.r[2]);
		tick(0xE8A0, 27);
		fl::tst(T.r[5]);
		tick(0xE8A2, 12);
		if ((int16_t)T.r[5] <= 0)
			tick(0xE8BA, 12);                          // BLE
		else
		{
			tick(0xE8A4, 12);
			fl::cmp(T.r[5], 0x0004);
			tick(0xE8A8, 18);
			if ((int16_t)T.r[5] > 4)
				tick(0xE8BA, 12);                      // BGT
			else
			{
				tick(0xE8AA, 12);
				const bool odd = fl_logic(T.r[4] & 0x0001) != 0;
				tick(0xE8AE, 18);
				if (!odd)
					tick(0xE8BA, 12);                  // BEQ
				else
				{
					tick(0xE8B0, 12);                  // parks 1-4: odd blocks
					T.r[2] = g_patch_park_odd_base;
					fl::mov(T.r[2]);
					tick(0xE8B4, 18);
					tick(0xE8BA, 12);                  // BR
				}
			}
		}
	}
	T.r[0] = peek(0x0F42);
	fl::mov(T.r[0]);
	tick(0xE8BE, 24);
	jsr(0xE8C2, 0x8078);                               // select_video_bank
	T.r[0] = T.r[4];
	fl::mov(T.r[0]);
	tick(0xE8C4, 12);
	T.r[0] = fl_asr(T.r[0]);
	tick(0xE8C6, 12);
	T.r[0] = fl_asr(T.r[0]);
	tick(0xE8C8, 12);
	T.r[0] = fl_asr(T.r[0]);
	tick(0xE8CA, 12);
	T.r[0] = fl_logic(T.r[0] & 0x1C00);
	tick(0xE8CE, 18);
	T.r[0] = fl::add(T.r[0], T.r[2]);
	tick(0xE8D0, 12);
	T.r[2] = T.r[0];
	fl::mov(T.r[2]);
	tick(0xE8D2, 12);
	poke(0x01D2, T.r[2]);
	fl::mov(T.r[2]);
	tick(0xE8D6, 27);
	T.r[0] = fl::add(T.r[0], 0x0400);
	tick(0xE8DA, 18);
	poke(0x01D4, T.r[0]);
	fl::mov(T.r[0]);
	tick(0xE8DE, 27);
	{
		const uint16_t v = peek(0x01D2);
		fl::mov(v);
		poke(0x1400, v);
	}
	tick(0xE8E4, 39);
	{
		const uint16_t v = peek(0x01D4);
		fl::mov(v);
		poke(0x1402, v);
	}
	tick(0xE8EA, 39);
	T.r[4] = fl_logic(T.r[4] & 0x1FFF);
	tick(0xE8EE, 18);
	T.r[4] = fl::add(T.r[4], 0x4000);                  // the block
	tick(0xE8F2, 18);
	T.r[0] = sext8(peekb(T.r[4]));
	fl::movb((uint8_t)T.r[0]);
	tick(0xE8F4, 18);
	ww(sp_at(8), 0);                                   // 8(SP): the block has objects
	fl::clr();
	tick(0xE8F8, 30);
	fl::tstb((uint8_t)T.r[0]);
	tick(0xE8FA, 12);
	if (!(T.r[0] & 0x80))
		tick(0xE908, 12);                              // BGE
	else
	{
		tick(0xE8FC, 12);
		ww(sp_at(8), fl::inc(rw(sp_at(8))));
		tick(0xE900, 30);
		ww(sp_at(6), T.r[4]);                          // 6(SP), 4(SP): the block and its bank
		fl::mov(T.r[4]);
		tick(0xE904, 30);
		ww(sp_at(4), T.r[2]);
		fl::mov(T.r[2]);
		tick(0xE908, 30);
	}
	// 0x0F50: the video address (row 0x0F43, column 0x0F45)
	{
		const uint8_t v = peekb(0x0F43);
		fl::movb(v);
		pokeb(0x0F51, v);
	}
	tick(0xE90E, 39);
	pokeb(0x0F51, fl_logicb((uint8_t)(peekb(0x0F51) | 0x20)));
	tick(0xE914, 33);
	{
		const uint8_t v = peekb(0x0F45);
		fl::movb(v);
		pokeb(0x0F50, v);
	}
	tick(0xE91A, 39);
	// the cell's terrain byte
	const bool right = fl_logic(peek(0x0F42) & 0x0800) != 0;
	tick(0xE920, 30);
	if (!right)
	{
		tick(0xE932, 12);                              // BEQ
		T.r[1] = sext8(peekb(T.r[4]));
		fl::movb((uint8_t)T.r[1]);
		tick(0xE934, 18);
		T.r[1] = fl_logic(T.r[1] & 0x007F);
		tick(0xE938, 18);
	}
	else
	{
		tick(0xE922, 12);
		T.r[1] = 0;
		fl::clr();
		tick(0xE924, 12);
		T.r[1] = fl_logicb(peekb((uint16_t)(T.r[4] + 1)));
		tick(0xE928, 27);
		T.r[3] = 0;
		fl::clr();
		tick(0xE92A, 12);
		T.r[3] = fl_logicb(peekb((uint16_t)(T.r[4] + 2)));
		tick(0xE92E, 27);
		T.r[4] = fl::add(T.r[4], T.r[3]);
		tick(0xE930, 12);
		tick(0xE938, 12);                              // BR
	}
	T.r[0] = 0;
	fl::clr();
	tick(0xE93A, 12);
	T.r[0] = fl_logicb(peekb(0x0F45));
	tick(0xE93E, 24);
	T.r[0] = fl_asr(T.r[0]);
	tick(0xE940, 12);
	T.r[0] = fl_asr(T.r[0]);
	tick(0xE942, 12);
	T.r[0] = fl_asr(T.r[0]);
	tick(0xE944, 12);
	T.r[5] = peek(0x0F42);
	fl::mov(T.r[5]);
	tick(0xE948, 24);
	T.r[5] &= 0xFF00;                                  // CLRB
	fl::clr();
	tick(0xE94A, 12);
	T.r[5] = fl_swab(T.r[5]);
	tick(0xE94C, 12);
	T.r[5] = sext8((uint8_t)T.r[5]);
	fl::movb((uint8_t)T.r[5]);
	tick(0xE94E, 12);
	T.r[5] = fl_logic(T.r[5] & 0x0038);
	tick(0xE952, 18);
	T.r[0] = fl_asr(T.r[0]);
	tick(0xE954, 12);
	T.r[0] = fl_asr(T.r[0]);
	tick(0xE956, 12);
	T.r[5] = fl::add(T.r[5], T.r[0]);
	tick(0xE958, 12);
	pokeb((uint16_t)(0x0F60 + T.r[5]), (uint8_t)T.r[1]);
	fl::movb((uint8_t)T.r[1]);
	tick(0xE95C, 30);
	T.r[3] = 0;
	fl::clr();
	tick(0xE95E, 12);
	poke(0x0F4C, 0);
	fl::clr();
	tick(0xE962, 27);
	T.r[2] = 0;
	fl::clr();
	tick(0xE964, 12);
	T.r[0] = 0;
	fl::clr();
	tick(0xE966, 12);
	T.r[4] = fl::add(T.r[4], 0x0003);                  // the packed tiles
	tick(0xE96A, 18);
	ww(T.r[6], 0x0008);                                // (SP): 8 rows
	fl::mov(0x0008);
	tick(0xE96E, 27);
	for (;;)
	{
		ww(sp_at(2), 0x0010);                          // 2(SP): 16 columns
		fl::mov(0x0010);
		tick(0xE974, 36);
		for (;;)
		{
			fl::tst(T.r[3]);
			tick(0xE976, 12);
			uint16_t to;
			if (T.r[3] == 0)
			{
				// a new run (see the packed tile runs)
				tick(0xE978, 12);
				T.r[3] = sext8(peekb(T.r[4]));
				fl::movb((uint8_t)T.r[3]);
				T.r[4]++;
				tick(0xE97A, 18);
				pokeb(0x0F5F, (uint8_t)T.r[3]);
				fl::movb((uint8_t)T.r[3]);
				tick(0xE97E, 27);
				T.r[3] = fl_logic(T.r[3] & 0x003F);
				tick(0xE982, 18);
				{
					const uint8_t v = peekb(T.r[4]);
					fl::movb(v);
					pokeb(0x0F4C, v);
					T.r[4]++;
				}
				tick(0xE986, 33);
				T.r[1] = sext8(peekb(0x0F5F));
				fl::movb((uint8_t)T.r[1]);
				tick(0xE98A, 24);
				T.r[5] = 0;
				fl::clr();
				tick(0xE98C, 12);
				const bool b6 = fl_logic(T.r[1] & 0x0040) != 0;
				tick(0xE990, 18);
				if (!b6)
					tick(0xE994, 12);                  // BEQ
				else
				{
					tick(0xE992, 12);
					T.r[5] = fl::inc(T.r[5]);
					tick(0xE994, 12);
				}
				fl::tst(T.r[1]);
				tick(0xE996, 12);
				if ((int16_t)T.r[1] >= 0)
				{
					tick(0xE9AE, 12);                  // BGE
					T.r[5] = fl_ror(T.r[5]);
					tick(0xE9B0, 12);
					if (!C())
					{
						tick(0xE9BA, 12);              // BCC: mode 0
						pokeb(0x0F5F, 0);
						fl::clr();
						tick(0xE9BE, 27);
						tick(0xE9E0, 12);              // BR
					}
					else
					{
						tick(0xE9B2, 12);              // mode 1
						pokeb(0x0F5F, 0x01);
						fl::movb(0x01);
						tick(0xE9B8, 33);
						tick(0xE9E0, 12);              // BR
					}
					to = 0xE9E0;
				}
				else
				{
					tick(0xE998, 12);
					const bool b0 = fl_logic(T.r[5] & 0x0001) != 0;
					tick(0xE99C, 18);
					if (!b0)
					{
						tick(0xE9A6, 12);              // BEQ: mode 2, the high byte too
						pokeb(0x0F5F, 0x02);
						fl::movb(0x02);
						tick(0xE9AC, 33);
						tick(0xE9D4, 12);              // BR
						to = 0xE9D4;
					}
					else
					{
						tick(0xE99E, 12);              // mode 3
						pokeb(0x0F5F, 0x03);
						fl::movb(0x03);
						tick(0xE9A4, 33);
						tick(0xE9E0, 12);              // BR
						to = 0xE9E0;
					}
				}
			}
			else
			{
				// the next tile of the run, by its mode (jump table at 0xFF02)
				tick(0xE9C0, 12);                      // BNE
				T.r[1] = sext8(peekb(0x0F5F));
				fl::movb((uint8_t)T.r[1]);
				tick(0xE9C4, 24);
				T.r[1] = fl_asl(T.r[1]);
				tick(0xE9C6, 12);
				const uint16_t off = peek((uint16_t)(0xFF02 + T.r[1]));
				to = fl::add(0xE9CA, off);
				tick(to, 27);
			}
			switch (to)
			{
			case 0xE9CA:                               // mode 0: a new low byte
				{
					const uint8_t v = peekb(T.r[4]);
					fl::movb(v);
					pokeb(0x0F4C, v);
				}
				tick(0xE9CE, 33);
				tick(0xE9D8, 12);                      // BR
				T.r[4] = fl::inc(T.r[4]);
				tick(0xE9DA, 12);
				tick(0xE9E0, 12);                      // BR
				to = 0xE9E0;
				break;
			case 0xE9D0:                               // mode 2: two new bytes
				{
					const uint8_t v = peekb(T.r[4]);
					fl::movb(v);
					pokeb(0x0F4C, v);
					T.r[4]++;
				}
				tick(0xE9D4, 33);
				to = 0xE9D4;
				break;
			case 0xE9DC:                               // mode 3: the next tile number
				poke(0x0F4C, fl::inc(peek(0x0F4C)));
				tick(0xE9E0, 27);
				to = 0xE9E0;
				break;
			case 0xE9D4:
			case 0xE9E0:
			case 0xE9E2:                               // mode 1: the same tile again
				break;
			default:
				leave_to(to);
			}
			if (to == 0xE9D4)
			{
				{
					const uint8_t v = peekb(T.r[4]);
					fl::movb(v);
					pokeb(0x0F4D, v);
				}
				tick(0xE9D8, 33);
				T.r[4] = fl::inc(T.r[4]);
				tick(0xE9DA, 12);
				tick(0xE9E0, 12);                      // BR
				to = 0xE9E0;
			}
			if (to == 0xE9E0)
			{
				T.r[0] = fl::inc(T.r[0]);
				tick(0xE9E2, 12);
			}
			fl::tst(T.r[0]);
			tick(0xE9E4, 12);
			bool variant;                              // Z clear at F:EA12: a tile with variants
			bool to_ea30 = false;                      // the BEQ at F:EA00 taken
			if (T.r[0] == 0)
			{
				tick(0xEA10, 12);                      // BEQ: the variant computed last time
				fl::tst(T.r[2]);
				tick(0xEA12, 12);
				variant = T.r[2] != 0;
			}
			else
			{
				tick(0xE9E6, 12);
				T.r[0] = 0;
				fl::clr();
				tick(0xE9E8, 12);
				T.r[2] = peek(0x0F4C);
				fl::mov(T.r[2]);
				tick(0xE9EC, 24);
				T.r[2] = fl_logic(T.r[2] & 0x07FF);
				tick(0xE9F0, 18);
				fl::cmp(T.r[2], 0x000F);
				tick(0xE9F4, 18);
				if ((int16_t)T.r[2] > 0x000F)
				{
					tick(0xEA0E, 12);                  // BGT
					T.r[2] = 0;
					fl::clr();
					tick(0xEA10, 12);
					fl::tst(T.r[2]);
					tick(0xEA12, 12);
					variant = false;
				}
				else
				{
					tick(0xE9F6, 12);
					T.r[1] = T.r[2];
					fl::mov(T.r[1]);
					tick(0xE9F8, 12);
					T.r[1] = fl_asl(T.r[1]);
					tick(0xE9FA, 12);
					T.r[1] = fl::add(T.r[1], peek(0x0F4E));   // the tile table
					tick(0xE9FE, 24);
					T.r[2] = peek(T.r[1]);
					fl::mov(T.r[2]);
					tick(0xEA00, 18);
					if (T.r[2] == 0)
					{
						tick(0xEA30, 12);              // BEQ
						variant = false;
						to_ea30 = true;
					}
					else
					{
						tick(0xEA02, 12);
						T.r[1] = peek(0x0F4C);
						fl::mov(T.r[1]);
						tick(0xEA06, 24);
						T.r[1] = fl_logic(T.r[1] & 0xF800);
						tick(0xEA0A, 18);
						T.r[2] = fl_logic(T.r[2] | T.r[1]);
						tick(0xEA0C, 12);
						tick(0xEA12, 12);              // BR
						variant = T.r[2] != 0;
					}
				}
			}
			if (!to_ea30)
				tick(variant ? 0xEA14 : 0xEA30, 12);   // BEQ at F:EA12
			if (variant)
			{
				T.r[1] = peek(0x0FA0);                 // the next of the 32 random variants
				fl::mov(T.r[1]);
				tick(0xEA18, 24);
				T.r[1] = fl::inc(T.r[1]);
				tick(0xEA1A, 12);
				poke(0x0FA0, T.r[1]);
				fl::mov(T.r[1]);
				tick(0xEA1E, 27);
				poke(0x0FA0, fl_logic(peek(0x0FA0) & 0x001F));
				tick(0xEA24, 33);
				T.r[5] = peek(0x0FA0);
				fl::mov(T.r[5]);
				tick(0xEA28, 24);
				T.r[1] = sext8(peekb((uint16_t)(0xFEE2 + T.r[5])));
				fl::movb((uint8_t)T.r[1]);
				tick(0xEA2C, 27);
				T.r[1] = fl::add(T.r[1], T.r[2]);
				tick(0xEA2E, 12);
				tick(0xEA34, 12);                      // BR
			}
			else
			{
				T.r[1] = peek(0x0F4C);
				fl::mov(T.r[1]);
				tick(0xEA34, 24);
			}
			poke(peek(0x0F50), T.r[1]);                // the tile into the playfield
			fl::mov(T.r[1]);
			tick(0xEA38, 36);
			pokeb(0x0F50, fl::incb(peekb(0x0F50)));
			tick(0xEA3C, 27);
			pokeb(0x0F50, fl::incb(peekb(0x0F50)));
			tick(0xEA40, 27);
			T.r[3] = fl::dec(T.r[3]);
			tick(0xEA42, 12);
			const uint16_t cols = fl::dec(rw(sp_at(2)));
			ww(sp_at(2), cols);
			tick(0xEA46, 30);
			if (!cols)
			{
				tick(0xEA48, 12);                      // BNE not taken
				break;
			}
			tick(0xE974, 12);
		}
		T.r[1] = 0;                                    // the next row
		fl::clr();
		tick(0xEA4A, 12);
		T.r[1] = fl_logicb(peekb(0x0F50));
		tick(0xEA4E, 24);
		T.r[1] = fl::sub(T.r[1], 0x0020);
		tick(0xEA52, 18);
		pokeb(0x0F50, (uint8_t)T.r[1]);
		fl::movb((uint8_t)T.r[1]);
		tick(0xEA56, 27);
		pokeb(0x0F51, fl::incb(peekb(0x0F51)));
		tick(0xEA5A, 27);
		const uint16_t rows = fl::dec(rw(T.r[6]));
		ww(T.r[6], rows);
		tick(0xEA5C, 21);
		if (!rows)
		{
			tick(0xEA5E, 12);                          // BNE not taken
			break;
		}
		tick(0xE96E, 12);
	}
	const bool objects = fl_logic(rw(sp_at(8)) & 0x0001) != 0;
	tick(0xEA64, 33);
	if (!objects)
		tick(0xEAB6, 12);                              // BEQ
	else
	{
		// the objects, listed backwards before the block (bit 7: the last), each drawn by
		// F:EC20 with the block's bank - 0x0400
		tick(0xEA66, 12);
		T.r[4] = rw(sp_at(6));
		fl::mov(T.r[4]);
		tick(0xEA6A, 27);
		T.r[4] = fl::add(T.r[4], 0x1FFF);
		tick(0xEA6E, 18);
		ww(sp_at(4), fl::sub(rw(sp_at(4)), 0x0400));
		tick(0xEA74, 36);
		{
			const uint16_t v = rw(sp_at(4));
			fl::mov(v);
			poke(0x01D2, v);
		}
		tick(0xEA7A, 42);
		T.r[1] = rw(sp_at(4));
		fl::mov(T.r[1]);
		tick(0xEA7E, 27);
		T.r[1] = fl::add(T.r[1], 0x0400);
		tick(0xEA82, 18);
		poke(0x01D4, T.r[1]);
		fl::mov(T.r[1]);
		tick(0xEA86, 27);
		{
			const uint16_t v = peek(0x01D2);
			fl::mov(v);
			poke(0x1400, v);
		}
		tick(0xEA8C, 39);
		{
			const uint16_t v = peek(0x01D4);
			fl::mov(v);
			poke(0x1402, v);
		}
		tick(0xEA92, 39);
		for (;;)
		{
			T.r[0] = sext8(peekb(T.r[4]));
			fl::movb((uint8_t)T.r[0]);
			tick(0xEA94, 18);
			push(T.r[0]);
			fl::mov(T.r[0]);
			tick(0xEA96, 24);
			jsr(0xEA9A, 0xEC20);                       // draw_map_object
			{
				const uint16_t v = rw(sp_at(6));
				fl::mov(v);
				poke(0x01D2, v);
			}
			tick(0xEAA0, 42);
			poke(0x01D4, T.r[1]);
			fl::mov(T.r[1]);
			tick(0xEAA4, 27);
			{
				const uint16_t v = peek(0x01D2);
				fl::mov(v);
				poke(0x1400, v);
			}
			tick(0xEAAA, 39);
			{
				const uint16_t v = peek(0x01D4);
				fl::mov(v);
				poke(0x1402, v);
			}
			tick(0xEAB0, 39);
			drop(0xEAB0);
			T.r[4]--;
			const uint8_t next = peekb(T.r[4]);
			fl::tstb(next);
			tick(0xEAB4, 21);
			if (next & 0x80)
			{
				tick(0xEAB6, 12);                      // BGE not taken
				break;
			}
			tick(0xEA92, 12);
		}
	}
	bank_a(0xEAB6, 0x1800);
	bank_b(0xEAC2, 0x3C00);
	T.r[6] = fl::add(T.r[6], 0x000A);
	tick(0xEAD2, 18);
	rts();
}

// ---------------------------------------------------------------- round 2

// MTPS: priority and condition codes (not the T bit); a pending interrupt is taken at the next
// tick, as after the instruction in the original
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
	c_save(3, 0xD056);
	banks_save(0xD056, 3, 4);
	pokeb(0x0E98, 0xAF);                               // the first slot
	fl::movb(0xAF);
	tick(0xD064, 33);
	const uint8_t level = peekb(0x0F59);
	fl::cmpb(level, 1);
	tick(0xD06A, 30);
	if (level != 1)
		tick(0xD0DE, 12);                              // BNE: not the town
	else
	{
		tick(0xD06C, 12);
		type_banks(0xD06C, 0);
		pokeb(0x0E96, 0x18);
		fl::movb(0x18);
		tick(0xD086, 33);
		pokeb(0x0E95, 0);
		fl::clr();
		tick(0xD08A, 27);
		poke(0x09AC, 0);
		fl::clr();
		tick(0xD08E, 27);
		{
			const uint8_t v = peekb(0x0E98);
			pokeb(0x09AC, v);
			fl::movb(v);
		}
		tick(0xD094, 39);
		jsr(0xD098, 0x90BA);                           // park_level
		T.r[2] = 0;
		fl::clr();
		tick(0xD09A, 12);
		T.r[2] = fl_logicb(peekb((uint16_t)(0xFE8F + T.r[0])));   // the layout
		tick(0xD09E, 27);
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0xD0A0, 12);
		T.r[0] = fl_asl(T.r[0]);
		tick(0xD0A2, 12);
		T.r[1] = peek((uint16_t)(0x672C + T.r[0]));    // the end of its list
		fl::mov(T.r[1]);
		tick(0xD0A6, 27);
		T.r[2] = peek((uint16_t)(0x6738 + T.r[0]));    // its (type, descriptor) pairs
		fl::mov(T.r[2]);
		tick(0xD0AA, 27);
		for (;;)
		{
			T.r[0] = sext8(peekb(0x0E95));
			fl::movb((uint8_t)T.r[0]);
			tick(0xD0AE, 24);
			T.r[0] = fl::inc(T.r[0]);
			tick(0xD0B0, 12);
			pokeb(0x0E95, (uint8_t)T.r[0]);            // counted
			fl::movb((uint8_t)T.r[0]);
			tick(0xD0B4, 27);
			{
				const uint16_t type = peek(T.r[2]);
				fl::mov(type);
				push(type);
			}
			tick(0xD0B6, 30);
			{
				const uint16_t desc = peek((uint16_t)(T.r[2] + 2));
				fl::mov(desc);
				push(desc);
			}
			tick(0xD0BA, 39);
			push(0x0001);
			fl::mov(0x0001);
			tick(0xD0BE, 30);
			jsr(0xD0C2, 0xCE24);                       // add_map_object(1, desc, type)
			ww(T.r[6], 0x0019);                        // and a type 0x19 object at the same place
			fl::mov(0x0019);
			tick(0xD0C6, 27);
			{
				const uint16_t desc = peek((uint16_t)(T.r[2] + 2));
				fl::mov(desc);
				push(desc);
			}
			tick(0xD0CA, 39);
			push(0x0001);
			fl::mov(0x0001);
			tick(0xD0CE, 30);
			jsr(0xD0D2, 0xCE24);                       // add_map_object(1, desc, 0x19)
			T.r[2] = fl::add(T.r[2], 0x0004);
			tick(0xD0D6, 18);
			T.r[6] = fl::add(T.r[6], 0x000A);
			tick(0xD0DA, 18);
			fl::cmp(T.r[2], T.r[1]);
			tick(0xD0DC, 12);
			if (T.r[2] == T.r[1])
			{
				tick(0xD0DE, 12);                      // BNE not taken
				break;
			}
			tick(0xD0AA, 12);
		}
	}
	banks_back(0xD0DE, 3, 4);
	c_restore(3);
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
	bool let_go = false;
	const bool fixed = fl_logic(peek(0x0EA0) & 0x0040) != 0;
	tick(0xD638, 30);
	bool check_copy = true;
	if (fixed)
	{
		tick(0xD63A, 12);
		jsr(0xD63E, 0xD838);                           // facing_object_0ea0
		T.r[0] = fl_ror(T.r[0]);
		tick(0xD640, 12);
		if (!fC())
		{
			tick(0xD648, 12);                          // BCC: facing it, let go
			let_go = true;
			check_copy = false;
		}
		else
			tick(0xD642, 12);
	}
	else
		tick(0xD642, 12);                              // BEQ
	if (check_copy)
	{
		const uint8_t copy = peekb(0x0EA1);
		fl::tstb(copy);
		tick(0xD646, 24);
		let_go = !copy;
		tick(copy ? 0xD676 : 0xD648, 12);              // BNE
	}
	if (let_go)
	{
		poke(0x0304, 0x0018);
		fl::mov(0x0018);
		tick(0xD64E, 33);
		poke(0x06B0, 0);
		fl::clr();
		tick(0xD652, 27);
		jsr(0xD656, 0xD778);                           // update_object_0ea0
		const uint8_t ramp = peekb(0x01EA);
		fl::tstb(ramp);
		tick(0xD65A, 24);
		if (!ramp)
			tick(0xD664, 12);                          // BEQ
		else
		{
			tick(0xD65C, 12);
			poke(0x02E8, 0);
			fl::clr();
			tick(0xD660, 27);
			jsr(0xD664, 0x9FB4);                       // skater_take_off
		}
		fl::cmp(peek(0x06B2), peek(0x06B0));
		tick(0xD66A, 36);
		if ((int16_t)peek(0x06B2) > (int16_t)peek(0x06B0))
			tick(0xD670, 12);                          // BGT
		else
		{
			tick(0xD66C, 12);
			jsr(0xD670, 0xA090);                       // skater_touch_down
		}
		pokeb(0x0EA0, 0);
		fl::clr();
		tick(0xD674, 27);
		rts();
		return;
	}
	const bool fixed2 = fl_logic(peek(0x0EA0) & 0x0040) != 0;
	tick(0xD67C, 30);
	if (fixed2)
	{
		tick(0xD6BC, 12);                              // BNE
		rts();
		return;
	}
	tick(0xD67E, 12);
	jsr(0xD682, 0xD4E4);                               // offset_to_object_0ea0
	fl::cmp(T.r[4], 0x0012);
	tick(0xD686, 18);
	bool inside = false;
	if (T.r[4] >= 0x0012)
		tick(0xD68C, 12);                              // BCC: beside the shape
	else
	{
		tick(0xD688, 12);
		fl::tst(T.r[3]);
		tick(0xD68A, 12);
		inside = (int16_t)T.r[3] >= 0;
		tick(inside ? 0xD690 : 0xD68C, 12);            // BGE
	}
	if (!inside)
	{
		T.r[1] = 0;
		fl::clr();
		tick(0xD68E, 12);
		tick(0xD6A6, 12);                              // BR
	}
	else
	{
		bank_b(0xD690, 0x2800);
		T.r[5] = fl::add(T.r[5], T.r[3]);
		tick(0xD69E, 12);
		T.r[5] = fl::add(T.r[5], T.r[4]);
		tick(0xD6A0, 12);
		T.r[1] = sext8(peekb(T.r[5]));                 // the shape's byte under the skater
		fl::movb((uint8_t)T.r[1]);
		tick(0xD6A2, 18);
		T.r[1] = fl_logic(T.r[1] & 0x007F);
		tick(0xD6A6, 18);
	}
	if (T.r[1])
	{
		tick(0xD6A8, 12);                              // BEQ not taken
		T.r[0] = 0x0007;
		fl::mov(T.r[0]);
		tick(0xD6AC, 18);
		T.r[0] = fl_logic((uint16_t)(T.r[0] & ~T.r[1]));
		tick(0xD6AE, 12);
		if (T.r[0])
		{
			tick(0xD6B0, 12);
			poke(0x0304, T.r[1]);
			fl::mov(T.r[1]);
			tick(0xD6B4, 27);
			rts();
			return;
		}
		tick(0xD6B6, 12);                              // BEQ
	}
	else
		tick(0xD6B6, 12);                              // BEQ
	poke(0x0304, 0x00F0);
	fl::mov(0x00F0);
	tick(0xD6BC, 33);
	rts();
}

// F:D6BE  slope_of_object_0ea0(): unless 0x0EA0 has bit 6 set, the speed 0x05B0 changes by
// the slope of the object under the skater: 0x79A8[(0x0327 & 3) * 8 + 0x65CC[type]] (bank B
// 0x2C00; the type's class from bank B 0x0400), negated or dropped as the table 0x79F8[the
// direction bits 3-4 of 0x0327 / 2 + 0x07D0[obj] / 4] says (-1 / 0 / 1, through the jump
// offsets at 0xFDF8).  Leaves bank A 0 and bank B 0x2C00 selected; returns the change.
uint16_t slope_of_object_0ea0(const Args &)
{
	c_save(1, 0xD6C2);
	T.r[2] = sext8(peekb(0x0EA0));
	fl::movb((uint8_t)T.r[2]);
	tick(0xD6C6, 24);
	const bool fixed = fl_logic(T.r[2] & 0x0040) != 0;
	tick(0xD6CA, 18);
	if (fixed)
		tick(0xD740, 12);                              // BNE
	else
	{
		tick(0xD6CC, 12);
		type_banks(0xD6CC, 0);
		T.r[0] = 0;
		fl::clr();
		tick(0xD6E2, 12);
		T.r[0] = fl_logicb(peekb((uint16_t)(0x0370 + T.r[2])));   // the object's type
		tick(0xD6E6, 27);
		T.r[0] = sext8(peekb((uint16_t)(0x65CC + T.r[0])));       // its class
		fl::movb((uint8_t)T.r[0]);
		tick(0xD6EA, 27);
		T.r[0] = fl_logic(T.r[0] & 0x00FF);
		tick(0xD6EE, 18);
		T.r[1] = 0x2C00;
		fl::mov(T.r[1]);
		tick(0xD6F2, 18);
		poke(0x01D4, T.r[1]);
		fl::mov(T.r[1]);
		tick(0xD6F6, 27);
		poke(0x1402, T.r[1]);
		fl::mov(T.r[1]);
		tick(0xD6FA, 27);
		T.r[1] = sext8(peekb(0x0327));                 // the terrain's slope type
		fl::movb((uint8_t)T.r[1]);
		tick(0xD6FE, 24);
		T.r[1] = fl_logic(T.r[1] & 0x0003);
		tick(0xD702, 18);
		T.r[1] = fl_asl(T.r[1]);
		tick(0xD704, 12);
		T.r[1] = fl_asl(T.r[1]);
		tick(0xD706, 12);
		T.r[1] = fl_asl(T.r[1]);
		tick(0xD708, 12);
		T.r[1] = fl::add(T.r[1], T.r[0]);
		tick(0xD70A, 12);
		T.r[1] = fl_asl(T.r[1]);
		tick(0xD70C, 12);
		T.r[0] = peek((uint16_t)(0x79A8 + T.r[1]));    // the change of speed
		fl::mov(T.r[0]);
		tick(0xD710, 27);
		T.r[1] = 0;
		fl::clr();
		tick(0xD712, 12);
		T.r[1] = fl_logicb(peekb(0x0327));
		tick(0xD716, 24);
		T.r[1] = fl_logic(T.r[1] & 0x0018);            // the slope's direction bits
		tick(0xD71A, 18);
		T.r[1] = fl_asr(T.r[1]);
		tick(0xD71C, 12);
		T.r[2] = sext8(peekb((uint16_t)(0x07D0 + T.r[2])));   // the object's direction
		fl::movb((uint8_t)T.r[2]);
		tick(0xD720, 27);
		T.r[2] = fl_logic(T.r[2] & 0x00FF);
		tick(0xD724, 18);
		T.r[2] = fl_asr(T.r[2]);
		tick(0xD726, 12);
		T.r[2] = fl_asr(T.r[2]);
		tick(0xD728, 12);
		T.r[1] = fl::add(T.r[1], T.r[2]);
		tick(0xD72A, 12);
		T.r[1] = sext8(peekb((uint16_t)(0x79F8 + T.r[1])));   // -1 / 0 / 1
		fl::movb((uint8_t)T.r[1]);
		tick(0xD72E, 27);
		T.r[1] = fl::inc(T.r[1]);
		tick(0xD730, 12);
		T.r[1] = fl_asl(T.r[1]);
		tick(0xD732, 12);
		const uint16_t to = fl::add(0xD736, peek((uint16_t)(0xFDF8 + T.r[1])));
		tick(to, 27);                                  // ADD 0xFDF8(R1),PC
		if (to == 0xD736)
		{
			T.r[0] = fl_neg(T.r[0]);                   // against the slope
			tick(0xD738, 12);
			tick(0xD73C, 12);                          // BR
		}
		else if (to == 0xD738)
			tick(0xD73C, 12);                          // BR: with it
		else if (to == 0xD73A)
		{
			T.r[0] = 0;                                // across it
			fl::clr();
			tick(0xD73C, 12);
		}
		poke(0x05B0, fl::add(peek(0x05B0), T.r[0]));
		tick(0xD740, 27);
	}
	c_restore(1);
	return T.r[0];
}

// F:D742 (registers): the skater moves on object 0x0EA0: with banks 0x3800 / 0x3C00, A7:4A66,
// F:9B82 (motion parameters) and A7:410C (projected speed) with the speed 0x05B2 held at 0.
// R1 is preserved.
void move_on_object_0ea0()
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xD744, 24);
	bank_a(0xD744, 0x3800);
	bank_b(0xD750, 0x3C00);
	jsr(0xD760, 0x4A66);
	jsr(0xD764, 0x9B82);                               // skater_motion_params
	T.r[1] = peek(0x05B2);                             // the height speed, held at 0 meanwhile
	fl::mov(T.r[1]);
	tick(0xD768, 24);
	poke(0x05B2, 0);
	fl::clr();
	tick(0xD76C, 27);
	jsr(0xD770, 0x410C);                               // project_speed
	poke(0x05B2, T.r[1]);
	fl::mov(T.r[1]);
	tick(0xD774, 27);
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0xD776, 18);
	rts();
}

// F:D778 (registers): object 0x0EA0 follows the skater's height (0x0670 = 0x0730 + 0x01AA +
// 0x06B0) and its piece count is queued (F:B602 queue_piece_count(type, obj)); banks 0 / 0x0400.
void update_object_0ea0()
{
	type_banks(0xD778, 0);
	T.r[0] = peek(0x0730);
	fl::mov(T.r[0]);
	tick(0xD790, 24);
	T.r[0] = fl::add(T.r[0], peek(0x01AA));
	tick(0xD794, 24);
	T.r[0] = fl::add(T.r[0], peek(0x06B0));
	tick(0xD798, 24);
	poke(0x0670, T.r[0]);                              // its screen y at the skater's height
	fl::mov(T.r[0]);
	tick(0xD79C, 27);
	T.r[0] = peek(0x0EA0);
	fl::mov(T.r[0]);
	tick(0xD7A0, 24);
	T.r[0] = fl_logic(T.r[0] & 0x003F);
	tick(0xD7A4, 18);
	push(T.r[0]);
	fl::mov(T.r[0]);
	tick(0xD7A6, 24);
	push(0);
	fl::clr();
	tick(0xD7A8, 24);
	{
		const uint8_t type = peekb((uint16_t)(0x0370 + T.r[0]));
		pokeb(T.r[6], type);
		fl::movb(type);
	}
	tick(0xD7AC, 36);
	jsr(0xD7B0, 0xB602);                               // queue_piece_count(type, obj)
	const uint16_t a = pop(), b = pop();
	fl::cmp(a, b);                                     // CMP (SP)+,(SP)+
	tick(0xD7B2, 24);
	rts();
}

// F:D838  facing_object_0ea0(): 1 when the skater is on the ground (0x01EA) and its direction
// 0x031D (turned by 8 when the speed 0x01B4 is negative) is within 2 of the object's 0x07D0
// [0x0EA1] (of 16 directions), else 0.  The carry is left as the original's (F:D632 uses it).
uint16_t facing_object_0ea0(const Args &)
{
	c_save(1, 0xD83C);
	const uint8_t ramp = peekb(0x01EA);
	fl::tstb(ramp);
	tick(0xD840, 24);
	bool facing = false;
	if (!ramp)
		tick(0xD87C, 12);                              // BEQ
	else
	{
		tick(0xD842, 12);
		T.r[1] = 0;
		fl::clr();
		tick(0xD844, 12);
		T.r[1] = fl_logicb(peekb(0x031D));             // the skater's direction
		tick(0xD848, 24);
		const uint16_t speed = peek(0x01B4);
		fl::tst(speed);
		tick(0xD84C, 24);
		if ((int16_t)speed >= 0)
			tick(0xD854, 12);                          // BGE
		else
		{
			// rolling backwards: the other way
			tick(0xD84E, 12);
			T.r[0] = 0x0008;
			fl::mov(T.r[0]);
			tick(0xD852, 18);
			T.r[1] = fl_logic(T.r[1] ^ T.r[0]);
			tick(0xD854, 12);
		}
		T.r[0] = sext8(peekb(0x0EA1));
		fl::movb((uint8_t)T.r[0]);
		tick(0xD858, 24);
		T.r[2] = 0;
		fl::clr();
		tick(0xD85A, 12);
		T.r[2] = fl_logicb(peekb((uint16_t)(0x07D0 + T.r[0])));   // the object's direction
		tick(0xD85E, 27);
		T.r[1] = fl::sub(T.r[1], T.r[2]);
		tick(0xD860, 12);
		if (!fN())
			tick(0xD864, 12);                          // BPL
		else
		{
			tick(0xD862, 12);
			T.r[1] = fl_neg(T.r[1]);
			tick(0xD864, 12);
		}
		fl::cmp(T.r[1], 0x0008);
		tick(0xD868, 18);
		if ((int16_t)T.r[1] <= 8)
			tick(0xD870, 12);                          // BLE
		else
		{
			tick(0xD86A, 12);
			T.r[1] = fl_neg(T.r[1]);
			tick(0xD86C, 12);
			T.r[1] = fl::add(T.r[1], 0x0010);
			tick(0xD870, 18);
		}
		fl::cmp(T.r[1], 0x0002);
		tick(0xD874, 18);
		facing = (int16_t)T.r[1] <= 2;
		tick(facing ? 0xD876 : 0xD87C, 12);            // BGT
	}
	if (facing)
	{
		T.r[0] = 0x0001;
		fl::mov(T.r[0]);
		tick(0xD87A, 18);
	}
	else
	{
		T.r[0] = 0;
		fl::clr();
		tick(0xD87E, 12);
	}
	c_restore(1);
	return T.r[0];
}

// F:E730  swap_players(): the 0x3E bytes of player state at 0x023E change places with those
// at 0x027C and the player 0x01F5 toggles; after the next vertical blank (F:8D50) 0x0FB4-0x0FD3
// are cleared and the sound CPU is told: command 12, F:8F50, command player + 1, between
// F:8E6A and F:8E72 (flag 0x01A4) and F:8F62.
uint16_t swap_players(const Args &)
{
	c_save(1, 0xE734);
	T.r[2] = 0x023E;
	fl::mov(T.r[2]);
	tick(0xE738, 18);
	T.r[1] = 0x027C;
	fl::mov(T.r[1]);
	tick(0xE73C, 18);
	for (;;)                                           // the two players' state change places
	{
		T.r[0] = sext8(peekb(T.r[2]));
		fl::movb((uint8_t)T.r[0]);
		tick(0xE73E, 18);
		{
			const uint8_t v = peekb(T.r[1]);
			pokeb(T.r[2], v);
			fl::movb(v);
		}
		T.r[2]++;
		tick(0xE740, 27);
		pokeb(T.r[1], (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		T.r[1]++;
		tick(0xE742, 21);
		fl::cmp(T.r[2], 0x027C);
		tick(0xE746, 18);
		if (T.r[2] == 0x027C)
		{
			tick(0xE748, 12);                          // BNE not taken
			break;
		}
		tick(0xE73C, 12);
	}
	T.r[1] = 0x0001;
	fl::mov(T.r[1]);
	tick(0xE74C, 18);
	T.r[0] = 0;
	fl::clr();
	tick(0xE74E, 12);
	T.r[0] = fl_logicb(peekb(0x01F5));
	tick(0xE752, 24);
	T.r[1] = fl_logic(T.r[1] ^ T.r[0]);
	tick(0xE754, 12);
	pokeb(0x01F5, (uint8_t)T.r[1]);                    // the other player
	fl::movb((uint8_t)T.r[1]);
	tick(0xE758, 27);
	jsr(0xE75C, 0x8D50);                               // wait_vblank
	T.r[0] = 0x0020;
	fl::mov(T.r[0]);
	tick(0xE760, 18);
	for (;;)                                           // 0x0FB4-0x0FD3 cleared
	{
		pokeb((uint16_t)(0x0FB3 + T.r[0]), 0);
		fl::clr();
		tick(0xE764, 30);
		T.r[0]--;                                      // SOB
		if (!T.r[0])
		{
			tick(0xE766, 18);
			break;
		}
		tick(0xE760, 18);
	}
	jsr(0xE76A, 0x8E6A);                               // set_flag_01a4
	push(0x000C);
	fl::mov(0x000C);
	tick(0xE76E, 30);
	jsr(0xE772, 0x8EA2);                               // send_sound_command(12)
	jsr(0xE776, 0x8F50);                               // expect_answer_aa
	ww(T.r[6], 0);
	fl::clr();
	tick(0xE778, 21);
	{
		const uint8_t player = peekb(0x01F5);
		pokeb(T.r[6], player);
		fl::movb(player);
	}
	tick(0xE77C, 33);
	ww(T.r[6], fl::inc(rw(T.r[6])));
	tick(0xE77E, 21);
	jsr(0xE782, 0x8EA2);                               // send_sound_command(player + 1)
	jsr(0xE786, 0x8F62);                               // expect_answer_55
	jsr(0xE78A, 0x8E72);                               // clear_flag_01a4
	drop(0xE78A);
	c_restore(1);
	return T.r[0];
}

// F:EDB2  show_screen_objects(n): the palettes are loaded over two vertical blanks (jobs
// F:DC3C 0x10 / 0, 0x11 / 2 in 0x0EB0 / 0x0EAC / 0x0EAE) and a third: block 0x0F (0 with
// n, else 1) or, for one of the players in a game (0x0F2B 1 or 2, 0x018F not 5), F:DABA.
// Then A?:4000(0, 3, n + 5) in bank A 0x1800, 0x01AE / 0x01AC = 0 / 0x2000, and the objects
// 0..0xFF2A[n] are set up from the tables at 0x7D94 (bank B 0x0400: type, x, y, z; frames 3
// for the first four, 4 after; flags 0x80 / 0x90) and shown (F:B4B6); F:8270 installs the
// 32V vectors and banks 0x3800 / 0x3C00 are selected.
uint16_t show_screen_objects(const Args &)
{
	c_save(2, 0xEDB6);
	// the palettes, one job per vertical blank
	poke(0x0EB0, 0xDC3C);
	fl::mov(0xDC3C);
	tick(0xEDBC, 33);
	poke(0x0EAC, 0x0010);
	fl::mov(0x0010);
	tick(0xEDC2, 33);
	poke(0x0EAE, 0);
	fl::clr();
	tick(0xEDC6, 27);
	jsr(0xEDCA, 0x8D50);                               // wait_vblank
	poke(0x0EB0, 0xDC3C);
	fl::mov(0xDC3C);
	tick(0xEDD0, 33);
	poke(0x0EAC, 0x0011);
	fl::mov(0x0011);
	tick(0xEDD6, 33);
	poke(0x0EAE, 0x0002);
	fl::mov(0x0002);
	tick(0xEDDC, 33);
	jsr(0xEDE0, 0x8D50);
	T.r[1] = rw(sp_at(0x0A));                          // n
	fl::mov(T.r[1]);
	tick(0xEDE4, 27);
	if (T.r[1])
	{
		tick(0xEDE6, 12);
		poke(0x0EAC, 0x000F);
		fl::mov(0x000F);
		tick(0xEDEC, 33);
		poke(0x0EAE, 0);
		fl::clr();
		tick(0xEDF0, 27);
		tick(0xEE1E, 12);                              // BR
		poke(0x0EB0, 0xDC3C);
		fl::mov(0xDC3C);
		tick(0xEE24, 33);
	}
	else
	{
		tick(0xEDF2, 12);                              // BEQ
		const uint8_t who = peekb(0x0F2B);
		fl::cmpb(who, 0x01);
		tick(0xEDF8, 30);
		bool player = who == 1;
		if (player)
			tick(0xEE02, 12);                          // BEQ
		else
		{
			tick(0xEDFA, 12);
			const uint8_t who2 = peekb(0x0F2B);
			fl::cmpb(who2, 0x02);
			tick(0xEE00, 30);
			player = who2 == 2;
			tick(player ? 0xEE02 : 0xEE12, 12);        // BNE
		}
		if (player)
		{
			const uint8_t state = peekb(0x018F);
			fl::cmpb(state, 0x05);
			tick(0xEE08, 30);
			player = state != 5;
			tick(player ? 0xEE0A : 0xEE12, 12);        // BEQ
		}
		if (player)
		{
			poke(0x0EB0, 0xDABA);                      // a player in a game: F:DABA
			fl::mov(0xDABA);
			tick(0xEE10, 33);
			tick(0xEE24, 12);                          // BR
		}
		else
		{
			poke(0x0EAC, 0x000F);
			fl::mov(0x000F);
			tick(0xEE18, 33);
			poke(0x0EAE, 0x0001);
			fl::mov(0x0001);
			tick(0xEE1E, 33);
			poke(0x0EB0, 0xDC3C);
			fl::mov(0xDC3C);
			tick(0xEE24, 33);
		}
	}
	jsr(0xEE28, 0x8D50);
	bank_a(0xEE28, 0x1800);
	push(T.r[1]);                                      // A?:4000(0, 3, n + 5)
	fl::mov(T.r[1]);
	tick(0xEE36, 24);
	ww(T.r[6], fl::add(rw(T.r[6]), 0x0005));
	tick(0xEE3A, 27);
	push(0x0003);
	fl::mov(0x0003);
	tick(0xEE3E, 30);
	push(0);
	fl::clr();
	tick(0xEE40, 24);
	jsr(0xEE44, 0x4000);
	poke(0x01AE, 0);
	fl::clr();
	tick(0xEE48, 27);
	poke(0x01AC, 0x2000);
	fl::mov(0x2000);
	tick(0xEE4E, 33);
	type_banks(0xEE4E, 0);
	T.r[1] = sext8(peekb((uint16_t)(0xFF2A + T.r[1])));   // the last object
	fl::movb((uint8_t)T.r[1]);
	tick(0xEE66, 27);
	T.r[1] = fl_logic(T.r[1] & 0x00FF);
	tick(0xEE6A, 18);
	T.r[2] = 0;
	fl::clr();
	tick(0xEE6C, 12);
	tick(0xEEC0, 12);                                  // BR
	for (;;)
	{
		fl::cmp(T.r[2], T.r[1]);
		tick(0xEEC2, 12);
		if ((int16_t)T.r[2] > (int16_t)T.r[1])
		{
			tick(0xEEC4, 12);                          // BLE not taken
			break;
		}
		tick(0xEE6E, 12);
		// object R2 from the tables at 0x7D94: type, x, y, z
		{
			const uint8_t v = peekb((uint16_t)(0x7D94 + T.r[2]));
			fl::movb(v);
			pokeb((uint16_t)(0x0370 + T.r[2]), v);
		}
		tick(0xEE74, 45);
		T.r[0] = T.r[2];
		fl::mov(T.r[0]);
		tick(0xEE76, 12);
		T.r[0] = fl_asl(T.r[0]);
		tick(0xEE78, 12);
		{
			const uint16_t v = peek((uint16_t)(0x7D9A + T.r[0]));
			fl::mov(v);
			poke((uint16_t)(0x03B0 + T.r[0]), v);
		}
		tick(0xEE7E, 45);
		{
			const uint16_t v = peek((uint16_t)(0x7DA4 + T.r[0]));
			fl::mov(v);
			poke((uint16_t)(0x06F0 + T.r[0]), v);
		}
		tick(0xEE84, 45);
		{
			const uint16_t v = peek((uint16_t)(0x7DAE + T.r[0]));
			fl::mov(v);
			poke((uint16_t)(0x0730 + T.r[0]), v);
		}
		tick(0xEE8A, 45);
		T.r[3] = 0;
		fl::clr();
		tick(0xEE8C, 12);
		fl::cmp(T.r[2], 0x0004);
		tick(0xEE90, 18);
		if ((int16_t)T.r[2] >= 4)
		{
			tick(0xEE9A, 12);                          // BGE
			T.r[0] = 0x0004;
			fl::mov(T.r[0]);
			tick(0xEE9E, 18);
		}
		else
		{
			tick(0xEE92, 12);
			T.r[3] = fl::inc(T.r[3]);
			tick(0xEE94, 12);
			T.r[0] = 0x0003;
			fl::mov(T.r[0]);
			tick(0xEE98, 18);
			tick(0xEE9E, 12);                          // BR
		}
		pokeb((uint16_t)(0x0790 + T.r[2]), (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0xEEA2, 30);
		pokeb((uint16_t)(0x0770 + T.r[2]), (uint8_t)T.r[0]);
		fl::movb((uint8_t)T.r[0]);
		tick(0xEEA6, 30);
		T.r[3] = fl_ror(T.r[3]);                       // C from the CMP: the first four
		tick(0xEEA8, 12);
		if (!C())
		{
			tick(0xEEB2, 12);                          // BCC
			pokeb((uint16_t)(0x0470 + T.r[2]), 0x90);
			fl::movb(0x90);
			tick(0xEEB8, 36);
		}
		else
		{
			tick(0xEEAA, 12);
			pokeb((uint16_t)(0x0470 + T.r[2]), 0x80);
			fl::movb(0x80);
			tick(0xEEB0, 36);
			tick(0xEEB8, 12);                          // BR
		}
		ww(T.r[6], T.r[2]);
		fl::mov(T.r[2]);
		tick(0xEEBA, 21);
		jsr(0xEEBE, 0xB4B6);                           // show_object
		T.r[2] = fl::inc(T.r[2]);
		tick(0xEEC0, 12);
	}
	jsr(0xEEC8, 0x8270);                               // the 32V vectors
	bank_a(0xEEC8, 0x3800);
	bank_b(0xEED4, 0x3C00);
	T.r[6] = fl::add(T.r[6], 0x0006);
	tick(0xEEE4, 18);
	c_restore(2);
	return T.r[0];
}

// F:F372 (registers): the test switch was turned on in the game: the self test's interrupt
// vectors (F:F3A8), interrupts acknowledged and enabled, 0x01CE / 0x01CC cleared, 0x018A = 1,
// the screens cleared (F:8D3E, F:8CE8) and set up (F:F40C); the self test (F:F43C) does not
// come back.
void enter_selftest()
{
	jsr(0xF376, 0xF3A8);                               // install_selftest_interrupts
	poke(0x15E0, 0);                                   // acknowledge the interrupts
	fl::clr();
	tick(0xF37A, 27);
	poke(0x15C0, 0);
	fl::clr();
	tick(0xF37E, 27);
	poke(0x1580, 0);
	fl::clr();
	tick(0xF382, 27);
	T.r[0] = 0;
	fl::clr();
	tick(0xF384, 12);
	mtps(0);                                           // and enabled
	tick(0xF388, 30);
	poke(0x01CE, 0);
	fl::clr();
	tick(0xF38C, 27);
	poke(0x01CC, 0);
	fl::clr();
	tick(0xF390, 27);
	poke(0x018A, 0x0001);
	fl::mov(0x0001);
	tick(0xF396, 33);
	jsr(0xF39A, 0x8D3E);                               // clear_text_ram
	jsr(0xF39E, 0x8CE8);                               // clear_playfield
	jsr(0xF3A2, 0xF40C);                               // init_display_b6
	jsr(0xF3A6, 0xF43C);                               // selftest_loop: does not come back
	rts();
}

// F:F40A: BR . - waits for an interrupt that continues elsewhere (the reset)
void wait_forever()
{
	for (;;)
		tick(0xF40A, 12);                              // BR .
}

// F:F43C: the self test's screens, over and over: sound (F:F5D8), controls (F:FB5E), the
// tests in bank B 0x2800 (0x7388, 0x74E4, 0x706A, 0x713E, 0x72B0), the scroll screen (F:F90E)
// and the playfield palette (F:DABA); F:F40C sets the display up again each time.
void selftest_loop()
{
	for (;;)
	{
		jsr(0xF440, 0xF5D8);                           // selftest_sound_screen
		jsr(0xF444, 0xFB5E);                           // selftest_controls_screen
		bank_b(0xF444, 0x2800);
		jsr(0xF454, 0x7388);
		jsr(0xF458, 0x74E4);
		jsr(0xF45C, 0x8D2C);
		jsr(0xF460, 0x706A);
		jsr(0xF464, 0xF90E);                           // selftest_scroll_screen
		bank_b(0xF464, 0x2800);
		jsr(0xF474, 0x713E);
		jsr(0xF478, 0xDABA);
		jsr(0xF47C, 0x72B0);
		jsr(0xF480, 0xF40C);                           // init_display_b6
		tick(0xF43C, 12);                              // BR F:F43C
	}
}

// F:F482 (registers): "BAD " and the number argument (F:BA9A, three digits) after a failed
// checksum; 0x0FE8 = 1.
void print_bad()
{
	static const uint8_t text[] = {0x42, 0x41, 0x44, 0x20};   // "BAD "
	for (int i = 0; i < 4; i++)
	{
		T.r[0] = text[i];
		fl::mov(T.r[0]);
		tick((uint16_t)(0xF486 + 8 * i), 18);
		jsr((uint16_t)(0xF48A + 8 * i), 0x8B52);       // draw_large_char
	}
	{
		const uint16_t v = rw(sp_at(2));               // print_number_large(0, 3, 3, 0, n)
		fl::mov(v);
		push(v);
	}
	tick(0xF4A6, 39);
	push(0);
	fl::clr();
	tick(0xF4A8, 24);
	push(0x0003);
	fl::mov(0x0003);
	tick(0xF4AC, 30);
	{
		const uint16_t v = rw(T.r[6]);
		fl::mov(v);
		push(v);
	}
	tick(0xF4AE, 30);
	push(0);
	fl::clr();
	tick(0xF4B0, 24);
	jsr(0xF4B4, 0xBA9A);
	pokeb(0x0FE8, 0x01);
	fl::movb(0x01);
	tick(0xF4BA, 33);
	T.r[6] = fl::add(T.r[6], 0x000A);
	tick(0xF4BE, 18);
	rts();
}

// F:F4D2 (registers): the ROM checksums of the self test.  The fixed ROM 0x8000-0xFFFF (bank
// A 0x3800) adds its even and odd bytes separately: both sums' low bytes must be 0 (message
// 0x0F / 0x14, "BAD n" F:F482 or OK F:F4C0).  Then three groups of eight banks (bank A n,
// bank B n + 0x0400, n = 0, 0x0800, ... 0xB800), 0x4000-0x7FFF each, against the bytes at
// 0xFF3C / 0xFF40 (messages 0x10-0x12 / 0x15-0x17).
void selftest_rom_checksums()
{
	// the verdict on a sum in Rr: BEQ at ok - 2 to ok (OK: F:F4C0) or "BAD n" (F:F482) from
	// bad, both on at end
	auto verdict = [](bool good, int r, uint16_t bad, uint16_t ok, uint16_t end) {
		if (good)
		{
			tick(ok, 12);                              // BEQ
			jsr(end, 0xF4C0);                          // print_ok
		}
		else
		{
			tick(bad, 12);
			push(T.r[r]);
			fl::mov(T.r[r]);
			tick((uint16_t)(bad + 2), 24);
			jsr((uint16_t)(bad + 6), 0xF482);          // print_bad(sum)
			drop((uint16_t)(bad + 6));
			tick(end, 12);                             // BR
		}
	};
	// the fixed ROM 0x8000-0xFFFF: even and odd bytes
	bank_a(0xF4D2, 0x3800);
	T.r[2] = 0;
	fl::clr();
	tick(0xF4E0, 12);
	T.r[1] = 0;
	fl::clr();
	tick(0xF4E2, 12);
	T.r[0] = 0x8000;
	fl::mov(T.r[0]);
	tick(0xF4E6, 18);
	for (;;)
	{
		T.r[3] = 0;
		fl::clr();
		tick(0xF4E8, 12);
		T.r[3] = fl_logicb(peekb(T.r[0]));
		T.r[0]++;
		tick(0xF4EA, 18);
		T.r[2] = fl::add(T.r[2], T.r[3]);
		tick(0xF4EC, 12);
		T.r[3] = 0;
		fl::clr();
		tick(0xF4EE, 12);
		T.r[3] = fl_logicb(peekb(T.r[0]));
		T.r[0]++;
		tick(0xF4F0, 18);
		T.r[1] = fl::add(T.r[1], T.r[3]);
		tick(0xF4F2, 12);
		fl::tst(T.r[0]);
		tick(0xF4F4, 12);
		if (!T.r[0])
		{
			tick(0xF4F6, 12);                          // BNE not taken
			break;
		}
		tick(0xF4E6, 12);
	}
	T.r[0] = 0x000F;
	fl::mov(T.r[0]);
	tick(0xF4FA, 18);
	jsr(0xF4FE, 0x8AB6);                               // message 0x0F
	T.r[2] = fl_logic(T.r[2] & 0x00FF);
	tick(0xF502, 18);
	verdict(T.r[2] == 0, 2, 0xF504, 0xF50E, 0xF512);
	T.r[0] = 0x0014;
	fl::mov(T.r[0]);
	tick(0xF516, 18);
	jsr(0xF51A, 0x8AB6);                               // message 0x14
	T.r[1] = fl_logic(T.r[1] & 0x00FF);
	tick(0xF51E, 18);
	verdict(T.r[1] == 0, 1, 0xF520, 0xF52A, 0xF52E);
	// three groups of eight banks (A n, B n + 0x0400), 0x4000-0x7FFF each
	T.r[5] = 0;
	fl::clr();
	tick(0xF530, 12);
	T.r[3] = 0x0001;
	fl::mov(T.r[3]);
	tick(0xF534, 18);
	for (;;)
	{
		T.r[2] = 0;
		fl::clr();
		tick(0xF536, 12);
		T.r[1] = 0;
		fl::clr();
		tick(0xF538, 12);
		T.r[4] = 0x0008;
		fl::mov(T.r[4]);
		tick(0xF53C, 18);
		for (;;)
		{
			poke(0x01D2, T.r[5]);
			fl::mov(T.r[5]);
			tick(0xF540, 27);
			T.r[0] = T.r[5];
			fl::mov(T.r[0]);
			tick(0xF542, 12);
			T.r[0] = fl::add(T.r[0], 0x0400);
			tick(0xF546, 18);
			poke(0x01D4, T.r[0]);
			fl::mov(T.r[0]);
			tick(0xF54A, 27);
			{
				const uint16_t v = peek(0x01D2);
				fl::mov(v);
				poke(0x1400, v);
			}
			tick(0xF550, 39);
			{
				const uint16_t v = peek(0x01D4);
				fl::mov(v);
				poke(0x1402, v);
			}
			tick(0xF556, 39);
			T.r[0] = 0x4000;
			fl::mov(T.r[0]);
			tick(0xF55A, 18);
			for (;;)
			{
				// MOVB (R0)+,-(SP); CLRB 1(SP); ADD (SP)+,R2 (then R1)
				for (int k = 2; k >= 1; k--)
				{
					const uint16_t pc = (uint16_t)(0xF55A + 8 * (2 - k));
					{
						const uint8_t v = peekb(T.r[0]);
						T.r[0]++;
						T.r[6] = (uint16_t)(T.r[6] - 2);
						pokeb(T.r[6], v);
						fl::movb(v);
					}
					tick((uint16_t)(pc + 2), 30);
					pokeb(sp_at(1), 0);
					fl::clr();
					tick((uint16_t)(pc + 6), 30);
					T.r[k] = fl::add(T.r[k], pop());
					tick((uint16_t)(pc + 8), 18);
				}
				fl::cmp(T.r[0], 0x7FFE);
				tick(0xF56E, 18);
				if (T.r[0] > 0x7FFE)
				{
					tick(0xF570, 12);                  // BLOS not taken
					break;
				}
				tick(0xF55A, 12);
			}
			T.r[5] = fl::add(T.r[5], 0x0800);
			tick(0xF574, 18);
			if (--T.r[4] == 0)                         // SOB
			{
				tick(0xF576, 18);
				break;
			}
			tick(0xF53C, 18);
		}
		T.r[2] = fl_logic(T.r[2] & 0x00FF);
		tick(0xF57A, 18);
		T.r[0] = T.r[3];
		fl::mov(T.r[0]);
		tick(0xF57C, 12);
		T.r[0] = fl::add(T.r[0], 0x000F);
		tick(0xF580, 18);
		jsr(0xF584, 0x8AB6);                           // message 0x0F + group
		bank_a(0xF584, 0x3800);
		T.r[0] = 0;
		fl::clr();
		tick(0xF592, 12);
		T.r[0] = fl_logicb(peekb((uint16_t)(0xFF3B + T.r[3])));
		tick(0xF596, 27);
		fl::cmp(T.r[2], T.r[0]);
		tick(0xF598, 12);
		verdict(T.r[2] == T.r[0], 2, 0xF59A, 0xF5A4, 0xF5A8);
		T.r[1] = fl_logic(T.r[1] & 0x00FF);
		tick(0xF5AC, 18);
		T.r[0] = T.r[3];
		fl::mov(T.r[0]);
		tick(0xF5AE, 12);
		T.r[0] = fl::add(T.r[0], 0x0014);
		tick(0xF5B2, 18);
		jsr(0xF5B6, 0x8AB6);                           // message 0x14 + group
		T.r[0] = 0;
		fl::clr();
		tick(0xF5B8, 12);
		T.r[0] = fl_logicb(peekb((uint16_t)(0xFF3F + T.r[3])));
		tick(0xF5BC, 27);
		fl::cmp(T.r[1], T.r[0]);
		tick(0xF5BE, 12);
		verdict(T.r[1] == T.r[0], 1, 0xF5C0, 0xF5CA, 0xF5CE);
		T.r[3] = fl::inc(T.r[3]);
		tick(0xF5D0, 12);
		fl::cmp(T.r[3], 0x0003);
		tick(0xF5D4, 18);
		if ((int16_t)T.r[3] > 3)
		{
			tick(0xF5D6, 12);                          // BLE not taken
			break;
		}
		tick(0xF534, 12);
	}
	rts();
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
	// F:BA9A(0, 3, 3, 0, (SP) - sub) with the value at k(SP) put in (SP) at pc: MOV k(SP),(SP);
	// SUB #sub,(SP); CLR -(SP); MOV #3,-(SP); MOV (SP),-(SP); CLR -(SP); JSR (the four pushed
	// words stay on the stack)
	auto number = [](uint16_t pc, int k, uint16_t sub) {
		{
			const uint16_t v = rw(sp_at(k));
			fl::mov(v);
			ww(T.r[6], v);
		}
		tick((uint16_t)(pc + 4), 36);
		ww(T.r[6], fl::sub(rw(T.r[6]), sub));
		tick((uint16_t)(pc + 8), 27);
		push(0);
		fl::clr();
		tick((uint16_t)(pc + 0x0A), 24);
		push(0x0003);
		fl::mov(0x0003);
		tick((uint16_t)(pc + 0x0E), 30);
		{
			const uint16_t v = rw(T.r[6]);
			fl::mov(v);
			push(v);
		}
		tick((uint16_t)(pc + 0x10), 30);
		push(0);
		fl::clr();
		tick((uint16_t)(pc + 0x12), 24);
		jsr((uint16_t)(pc + 0x16), 0xBA9A);            // print_number_large
	};
	// MOV #msg,R0; JSR PC,@#F:8AB6 at pc
	auto message = [](uint16_t pc, uint16_t msg) {
		T.r[0] = msg;
		fl::mov(msg);
		tick((uint16_t)(pc + 4), 18);
		jsr((uint16_t)(pc + 8), 0x8AB6);
	};
	// MOV #v,(SP); JSR PC,@#F:8EA2 at pc: a sound command
	auto command = [](uint16_t pc, uint16_t v) {
		ww(T.r[6], v);
		fl::mov(v);
		tick((uint16_t)(pc + 4), 27);
		jsr((uint16_t)(pc + 8), 0x8EA2);               // send_sound_command
	};
	// frame after F:F60C: (SP) argument, 2(SP) the answer, 4(SP) the volume, 6(SP) the
	// highest volume, 8(SP) tries left; the volume screen runs 16 bytes lower
	uint16_t at = 0xF5D8;
	uint16_t tst = 0;                                  // the word the BLE at F:F61E tests
	for (;;)
	{
		switch (at)
		{
		case 0xF5D8:
			T.r[6] = fl::sub(T.r[6], 0x0008);
			tick(0xF5DC, 18);
			jsr(0xF5E0, 0x8CE8);                       // clear_playfield
			jsr(0xF5E4, 0x8D2C);
			jsr(0xF5E8, 0x8E2E);
			poke(0x01AE, 0x0011);
			fl::mov(0x0011);
			tick(0xF5EE, 33);
			poke(0x01AC, 0);
			fl::clr();
			tick(0xF5F2, 27);
			poke(0x1100, 0x0051);                      // PAL_PF
			fl::mov(0x0051);
			tick(0xF5F8, 33);
			poke(0x0188, 0xE000);
			fl::mov(0xE000);
			tick(0xF5FE, 33);
			poke(0x0184, 0x5C04);
			fl::mov(0x5C04);
			tick(0xF604, 33);
			message(0xF604, 0x0021);
			push(0x003C);
			fl::mov(0x003C);
			tick(0xF610, 30);
			jsr(0xF614, 0x8D74);
			ww(sp_at(8), 0x000A);                      // ten tries
			fl::mov(0x000A);
			tick(0xF61A, 36);
			at = 0xF61A;
			break;
		case 0xF61A:
			tst = rw(sp_at(8));
			fl::tst(tst);
			tick(0xF61E, 27);
			at = 0xF61E;
			break;
		case 0xF61E:
			if ((int16_t)tst <= 0)
			{
				tick(0xF688, 12);                      // BLE
				at = 0xF688;
				break;
			}
			tick(0xF620, 12);
			// reset the sound CPU at a quiet moment
			jsr(0xF624, 0x8D50);                       // wait_vblank
			{
				const bool busy = fl_logicb((uint8_t)(peekb(0x01C4) & 0x3F)) != 0;
				tick(0xF62A, 30);
				if (busy)
				{
					tick(0xF61A, 12);                  // BNE
					at = 0xF61A;
					break;
				}
			}
			tick(0xF62C, 12);
			poke(0x15A0, 0x0001);                      // SOUND_RESET
			fl::mov(0x0001);
			tick(0xF632, 33);
			jsr(0xF636, 0x8134);
			poke(0x15A0, 0);
			fl::clr();
			tick(0xF63A, 27);
			pokeb(0x01A2, 0);
			fl::clr();
			tick(0xF63E, 27);
			ww(sp_at(2), 0);
			fl::clr();
			tick(0xF642, 30);
			{
				const uint8_t v = peekb(0x1C00);       // SOUND_RESPONSE
				fl::movb(v);
				pokeb(sp_at(2), v);
			}
			tick(0xF648, 42);
			jsr(0xF64C, 0x8ED2);                       // read_sound_response
			ww(sp_at(2), T.r[0]);
			fl::mov(T.r[0]);
			tick(0xF650, 30);
			fl::cmp(T.r[0], 0x00FF);
			tick(0xF654, 18);
			if (T.r[0] == 0x00FF)
			{
				tick(0xF656, 12);                      // BNE not taken: no answer
				tst = 0xFFFF;
				ww(sp_at(8), tst);
				fl::mov(tst);
				tick(0xF65C, 36);
				tick(0xF61E, 12);                      // BR
				at = 0xF61E;
				break;
			}
			tick(0xF65E, 12);
			ww(sp_at(8), fl::dec(rw(sp_at(8))));
			tick(0xF662, 30);
			message(0xF662, 0x0022);                   // the tries so far
			ww(T.r[6], 0x000A);
			fl::mov(0x000A);
			tick(0xF66E, 27);
			ww(T.r[6], fl::sub(rw(T.r[6]), rw(sp_at(8))));
			tick(0xF672, 36);
			push(0);
			fl::clr();
			tick(0xF674, 24);
			push(0x0002);
			fl::mov(0x0002);
			tick(0xF678, 30);
			push(0x0001);
			fl::mov(0x0001);
			tick(0xF67C, 30);
			push(0);
			fl::clr();
			tick(0xF67E, 24);
			jsr(0xF682, 0xBA9A);                       // print_number_large
			T.r[6] = fl::add(T.r[6], 0x0008);
			tick(0xF686, 18);
			tick(0xF61A, 12);                          // BR
			at = 0xF61A;
			break;
		case 0xF688:
			{
				const uint16_t left = rw(sp_at(8));
				fl::tst(left);
				tick(0xF68C, 27);
				if (!left)
				{
					tick(0xF68E, 12);                  // BNE not taken: no sound CPU
					message(0xF68E, 0x0023);
					tick(0xF888, 18);                  // JMP
					at = 0xF888;
					break;
				}
			}
			tick(0xF69A, 12);
			message(0xF69A, 0x00A3);
			jsr(0xF6A6, 0x8D50);
			jsr(0xF6AA, 0x8D50);
			command(0xF6AA, 0x0009);                   // its self test
			jsr(0xF6B6, 0x8ED2);
			ww(sp_at(2), T.r[0]);
			fl::mov(T.r[0]);
			tick(0xF6BA, 30);
			jsr(0xF6BE, 0x8F62);                       // expect_answer_55
			message(0xF6BE, 0x001F);
			{
				const bool failed = fl_logic(rw(sp_at(2)) & 0x001F) != 0;
				tick(0xF6CC, 33);
				if (failed)
				{
					tick(0xF6CE, 12);
					tick(0xF86E, 18);                  // JMP
					at = 0xF86E;
					break;
				}
			}
			tick(0xF6D2, 12);                          // BEQ
			T.r[1] = 0;
			fl::clr();
			tick(0xF6D4, 12);
			at = 0xF6D4;
			break;
		case 0xF6D4:                                   // the four test sounds
			ww(T.r[6], 0);
			fl::clr();
			tick(0xF6D6, 21);
			{
				const uint8_t v = peekb((uint16_t)(0xFF48 + T.r[1]));
				fl::movb(v);
				pokeb(T.r[6], v);
			}
			tick(0xF6DA, 36);
			jsr(0xF6DE, 0x8EA2);
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0xF6E0, 12);
			T.r[0] = fl::add(T.r[0], 0x0029);
			tick(0xF6E4, 18);
			jsr(0xF6E8, 0x8AB6);
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0xF6EA, 12);
			T.r[0] = fl_asl(T.r[0]);
			tick(0xF6EC, 12);
			T.r[2] = peek((uint16_t)(0xFF4C + T.r[0]));   // frames
			fl::mov(T.r[2]);
			tick(0xF6F0, 27);
			if ((int16_t)T.r[2] <= 0)
			{
				tick(0xF70A, 12);                      // BLE
				at = 0xF70A;
				break;
			}
			tick(0xF6F2, 12);
			at = 0xF6F2;
			break;
		case 0xF6F2:
			jsr(0xF6F6, 0x8D50);
			{
				const uint8_t buttons = peekb(0x0193);
				fl::tstb(buttons);
				tick(0xF6FA, 24);
				if (buttons & 0x80)
				{
					tick(0xF6FC, 12);                  // a button ends the screen
					command(0xF6FC, 0x0005);
					tick(0xF88C, 18);                  // JMP
					at = 0xF88C;
					break;
				}
			}
			tick(0xF708, 12);                          // BPL
			if (--T.r[2] != 0)                         // SOB
			{
				tick(0xF6F2, 18);
				break;
			}
			tick(0xF70A, 18);
			at = 0xF70A;
			break;
		case 0xF70A:
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0xF70C, 12);
			T.r[0] = fl::add(T.r[0], 0x00A9);
			tick(0xF710, 18);
			jsr(0xF714, 0x8AB6);
			command(0xF714, 0x0005);
			T.r[1] = fl::inc(T.r[1]);
			tick(0xF71E, 12);
			fl::cmp(T.r[1], 0x0003);
			tick(0xF722, 18);
			if ((int16_t)T.r[1] <= 3)
			{
				tick(0xF6D4, 12);                      // BLE
				at = 0xF6D4;
				break;
			}
			tick(0xF724, 12);
			// the volume screen
			command(0xF724, 0x0008);                   // the highest volume
			jsr(0xF730, 0x8ED2);
			ww(sp_at(6), T.r[0]);
			fl::mov(T.r[0]);
			tick(0xF734, 30);
			jsr(0xF738, 0x8F62);
			message(0xF738, 0x0024);
			number(0xF740, 6, 0x002F);
			ww(sp_at(0x0C), 0x0030);                   // the volume (4(SP) of the frame)
			fl::mov(0x0030);
			tick(0xF75C, 36);
			message(0xF75C, 0x0025);
			number(0xF764, 0x0C, 0x0030);
			message(0xF77A, 0x0026);
			message(0xF782, 0x0027);
			message(0xF78A, 0x002D);
			at = 0xF792;
			break;
		case 0xF792:
			{
				const uint8_t buttons = peekb(0x0193);
				fl::tstb(buttons);
				tick(0xF796, 24);
				if (buttons & 0x80)
				{
					tick(0xF868, 12);                  // BMI: the end
					at = 0xF868;
					break;
				}
			}
			tick(0xF798, 12);
			jsr(0xF79C, 0x8D50);
			{
				const bool skip = fl_logicb((uint8_t)(peekb(0x01C4) & 0x07)) != 0;
				tick(0xF7A2, 30);
				if (skip)
				{
					tick(0xF7F4, 12);                  // BNE
					at = 0xF7F4;
					break;
				}
			}
			tick(0xF7A4, 12);
			// every 8 frames: the coin inputs
			command(0xF7A4, 0x0012);
			jsr(0xF7B0, 0x8F50);                       // expect_answer_aa
			command(0xF7B0, 0x0040);
			command(0xF7B8, 0x0018);
			jsr(0xF7C4, 0x8ED2);
			ww(sp_at(0x12), T.r[0]);
			fl::mov(T.r[0]);
			tick(0xF7C8, 30);
			jsr(0xF7CC, 0x8F62);
			T.r[1] = 0;
			fl::clr();
			tick(0xF7CE, 12);
			at = 0xF7CE;
			break;
		case 0xF7CE:
			T.r[0] = T.r[1];
			fl::mov(T.r[0]);
			tick(0xF7D0, 12);
			T.r[0] = fl_asl(T.r[0]);
			tick(0xF7D2, 12);
			{
				const bool set = fl_logic(rw(sp_at(0x12)) & peek((uint16_t)(0xFF44 + T.r[0]))) != 0;
				tick(0xF7D8, 42);
				if (set)
				{
					tick(0xF7E2, 12);                  // BNE
					T.r[0] = T.r[1];
					fl::mov(T.r[0]);
					tick(0xF7E4, 12);
					T.r[0] = fl::add(T.r[0], 0x00AE);
					tick(0xF7E8, 18);
				}
				else
				{
					tick(0xF7DA, 12);
					T.r[0] = T.r[1];
					fl::mov(T.r[0]);
					tick(0xF7DC, 12);
					T.r[0] = fl::add(T.r[0], 0x002E);
					tick(0xF7E0, 18);
					tick(0xF7E8, 12);                  // BR
				}
			}
			jsr(0xF7EC, 0x8AB6);
			T.r[1] = fl::inc(T.r[1]);
			tick(0xF7EE, 12);
			fl::cmp(T.r[1], 0x0001);
			tick(0xF7F2, 18);
			if ((int16_t)T.r[1] <= 1)
			{
				tick(0xF7CE, 12);                      // BLE
				break;
			}
			tick(0xF7F4, 12);
			at = 0xF7F4;
			break;
		case 0xF7F4:
			// the rotary controller sets the volume
			{
				const uint16_t v = rw(sp_at(0x14));
				fl::mov(v);
				ww(sp_at(0x18), v);                    // the volume before
			}
			tick(0xF7FA, 45);
			jsr(0xF7FE, 0x8D86);
			jsr(0xF802, 0x8E2E);
			T.r[0] = fl::add(T.r[0], rw(sp_at(0x14)));
			tick(0xF806, 27);
			ww(sp_at(0x14), T.r[0]);
			fl::mov(T.r[0]);
			tick(0xF80A, 30);
			fl::cmp(T.r[0], 0x0030);
			tick(0xF80E, 18);
			if ((int16_t)T.r[0] >= 0x0030)
				tick(0xF816, 12);                      // BGE
			else
			{
				tick(0xF810, 12);                      // below 0x30: the highest
				const uint16_t v = rw(sp_at(0x16));
				fl::mov(v);
				ww(sp_at(0x14), v);
				tick(0xF816, 45);
			}
			{
				const uint16_t vol = rw(sp_at(0x14));
				const uint16_t top = rw(sp_at(0x16));
				fl::cmp(vol, top);
				tick(0xF81C, 42);
				if ((int16_t)vol <= (int16_t)top)
					tick(0xF824, 12);                  // BLE
				else
				{
					tick(0xF81E, 12);                  // above it: 0x30
					ww(sp_at(0x14), 0x0030);
					fl::mov(0x0030);
					tick(0xF824, 36);
				}
			}
			{
				const uint16_t before = rw(sp_at(0x18));
				const uint16_t vol = rw(sp_at(0x14));
				fl::cmp(before, vol);
				tick(0xF82A, 42);
				if (before == vol)
				{
					tick(0xF856, 12);                  // BEQ
					at = 0xF856;
					break;
				}
			}
			tick(0xF82C, 12);
			command(0xF82C, 0x0005);
			message(0xF834, 0x0025);
			number(0xF83C, 0x14, 0x0030);
			T.r[6] = fl::add(T.r[6], 0x0008);
			tick(0xF856, 18);
			at = 0xF856;
			break;
		case 0xF856:
			{
				const bool play = fl_logicb((uint8_t)(peekb(0x0193) & 0x40)) != 0;
				tick(0xF85C, 30);
				if (!play)
				{
					tick(0xF792, 12);                  // BEQ
					at = 0xF792;
					break;
				}
			}
			tick(0xF85E, 12);                          // button 1 plays it
			{
				const uint16_t v = rw(sp_at(0x14));
				fl::mov(v);
				ww(T.r[6], v);
			}
			tick(0xF862, 36);
			jsr(0xF866, 0x8EA2);
			tick(0xF792, 12);                          // BR
			at = 0xF792;
			break;
		case 0xF868:
			T.r[6] = fl::add(T.r[6], 0x0010);
			tick(0xF86C, 18);
			tick(0xF88C, 12);                          // BR
			at = 0xF88C;
			break;
		case 0xF86E:                                   // the failed parts
			T.r[1] = 0x0007;
			fl::mov(T.r[1]);
			tick(0xF872, 18);
			for (;;)
			{
				ww(sp_at(2), fl_asr(rw(sp_at(2))));
				tick(0xF876, 30);
				const bool bad = fl_logic(rw(sp_at(2)) & 0x0001) != 0;
				tick(0xF87C, 33);
				if (!bad)
					tick(0xF886, 12);                  // BEQ
				else
				{
					tick(0xF87E, 12);
					message(0xF87E, 0x0028);
				}
				if (--T.r[1] == 0)                     // SOB
				{
					tick(0xF888, 18);
					break;
				}
				tick(0xF872, 18);
			}
			at = 0xF888;
			break;
		case 0xF888:
			jsr(0xF88C, 0x8D60);                       // wait for a button
			at = 0xF88C;
			break;
		case 0xF88C:
			pokeb(0x0FE6, 0);
			fl::clr();
			tick(0xF890, 27);
			T.r[6] = fl::add(T.r[6], 0x000A);
			tick(0xF894, 18);
			rts();
			return;
		}
	}
}

// F:FA00  selftest_center_rotary(): unless the controller position 0x01C6 is already in
// 0x20-0x28, asks for it to be centred (messages 0 and 7) and waits until it has stood still
// there for six frames (button 1 shown: message 0x0B / 0x8B; 0x0FE6, the next screen, ends
// early), then messages 0x80, 0x87, 1, two seconds, 0x81.
uint16_t selftest_center_rotary(const Args &)
{
	// MOV #m,R0; JSR PC,@#F:8AB6 at pc
	auto message = [](uint16_t pc, uint16_t m) {
		T.r[0] = m;
		fl::mov(m);
		tick((uint16_t)(pc + 4), 18);
		jsr((uint16_t)(pc + 8), 0x8AB6);
	};
	c_save(4, 0xFA04);
	T.r[6] = (uint16_t)(T.r[6] - 2);                   // TST -(SP): (SP) frames left
	fl::tst(rw(T.r[6]));
	tick(0xFA06, 21);
	bool centred;
	{
		const uint16_t pos = peek(0x01C6);
		fl::cmp(pos, 0x0020);
		tick(0xFA0C, 30);
		if ((int16_t)pos < 0x0020)
		{
			tick(0xFA16, 12);                          // BLT
			centred = false;
		}
		else
		{
			tick(0xFA0E, 12);
			const uint16_t pos2 = peek(0x01C6);
			fl::cmp(pos2, 0x0028);
			tick(0xFA14, 30);
			centred = (int16_t)pos2 <= 0x0028;
			tick(centred ? 0xFA9A : 0xFA16, 12);       // BLE
		}
	}
	if (!centred)
	{
		message(0xFA16, 0x0000);
		message(0xFA1E, 0x0007);
		ww(T.r[6], 0x0006);
		fl::mov(0x0006);
		tick(0xFA2A, 27);
		bool next = false;                             // the next screen asked for (0x0FE6)
		for (;;)
		{
			jsr(0xFA2E, 0x8D50);                       // wait_vblank
			jsr(0xFA32, 0x8D86);
			// six frames standing still in the centre (0x20-0x28)
			bool still = false;
			{
				const uint16_t moving = peek(0x01CA);
				fl::tst(moving);
				tick(0xFA36, 24);
				if (moving)
					tick(0xFA4C, 12);                  // BNE
				else
				{
					tick(0xFA38, 12);
					const uint16_t pos = peek(0x01C6);
					fl::cmp(pos, 0x0020);
					tick(0xFA3E, 30);
					if ((int16_t)pos < 0x0020)
						tick(0xFA4C, 12);              // BLT
					else
					{
						tick(0xFA40, 12);
						const uint16_t pos2 = peek(0x01C6);
						fl::cmp(pos2, 0x0028);
						tick(0xFA46, 30);
						still = (int16_t)pos2 <= 0x0028;
						tick(still ? 0xFA48 : 0xFA4C, 12);   // BGT
					}
				}
			}
			if (still)
			{
				ww(T.r[6], fl::dec(rw(T.r[6])));
				tick(0xFA4A, 21);
				tick(0xFA50, 12);                      // BR
			}
			else
			{
				ww(T.r[6], 0x0006);
				fl::mov(0x0006);
				tick(0xFA50, 27);
			}
			// button 1
			const bool pressed = fl_logicb((uint8_t)(peekb(0x0193) & 0x40)) != 0;
			tick(0xFA56, 30);
			if (pressed)
			{
				tick(0xFA58, 12);
				T.r[0] = 0x000B;
				fl::mov(T.r[0]);
				tick(0xFA5C, 18);
				tick(0xFA62, 12);                      // BR
			}
			else
			{
				tick(0xFA5E, 12);                      // BEQ
				T.r[0] = 0x008B;
				fl::mov(T.r[0]);
				tick(0xFA62, 18);
			}
			jsr(0xFA66, 0x8AB6);
			const uint8_t asked = peekb(0x0FE6);
			fl::tstb(asked);
			tick(0xFA6A, 24);
			if (asked)
			{
				tick(0xFA9A, 12);                      // BNE
				next = true;
				break;
			}
			tick(0xFA6C, 12);
			const uint16_t left = rw(T.r[6]);
			fl::tst(left);
			tick(0xFA6E, 18);
			if (left)
			{
				tick(0xFA2A, 12);                      // BNE
				continue;
			}
			tick(0xFA70, 12);
			break;
		}
		if (!next)
		{
			message(0xFA70, 0x0080);
			message(0xFA78, 0x0087);
			message(0xFA80, 0x0001);
			push(0x0078);                              // two seconds
			fl::mov(0x0078);
			tick(0xFA8C, 30);
			jsr(0xFA90, 0x8D74);
			message(0xFA90, 0x0081);
			drop(0xFA98);
		}
	}
	drop(0xFA9A);
	c_restore(4);
	return T.r[0];
}

// F:FA9E (registers): marks sector n of the rotary test: the four characters at 0x7798[n * 4]
// + 0x2082 (bank B 0x2800; the playfield selected by F:8068) get the colour 0x0188 | 0x87.
// R1 is preserved.
void mark_rotary_sector()
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xFAA0, 24);
	jsr(0xFAA4, 0x8068);                               // select_video_bank0
	bank_b(0xFAA4, 0x2800);
	T.r[0] = rw(sp_at(4));                             // the sector
	fl::mov(T.r[0]);
	tick(0xFAB4, 27);
	T.r[0] = fl_asl(T.r[0]);
	tick(0xFAB6, 12);
	T.r[0] = fl_asl(T.r[0]);
	tick(0xFAB8, 12);
	T.r[0] = fl_asl(T.r[0]);
	tick(0xFABA, 12);
	T.r[0] = fl::add(T.r[0], 0x7798);
	tick(0xFABE, 18);
	T.r[0] = peek(T.r[0]);                             // its place on the screen
	fl::mov(T.r[0]);
	tick(0xFAC0, 18);
	T.r[0] = fl::add(T.r[0], 0x2082);
	tick(0xFAC4, 18);
	T.r[1] = peek(0x0188);
	fl::mov(T.r[1]);
	tick(0xFAC8, 24);
	T.r[1] = fl_logic(T.r[1] | 0x0087);                // a 2 x 2 block of tile 0x87
	tick(0xFACC, 18);
	poke((uint16_t)(T.r[0] + 0x82), T.r[1]);
	fl::mov(T.r[1]);
	tick(0xFAD0, 30);
	poke((uint16_t)(T.r[0] + 0x80), T.r[1]);
	fl::mov(T.r[1]);
	tick(0xFAD4, 30);
	poke((uint16_t)(T.r[0] + 2), T.r[1]);
	fl::mov(T.r[1]);
	tick(0xFAD8, 30);
	poke(T.r[0], T.r[1]);
	fl::mov(T.r[1]);
	tick(0xFADA, 21);
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0xFADC, 18);
	rts();
}

// F:FADE  selftest_rotate_sectors(): every frame the controller's sector ((0x01C6 + 4) / 9,
// F:90A6) is marked (F:FA9E) the first time it is reached, until all sectors (bits 0xFC7C[n])
// are done (message 5, two seconds) or the next screen is asked for (0x0FE6).  Button 1 is
// shown as in F:FA00.
uint16_t selftest_rotate_sectors(const Args &)
{
	c_save(4, 0xFAE2);
	{
		const uint16_t s = rw((uint16_t)(T.r[6] - 2)); // CMP -(SP),-(SP): (SP) the sector,
		const uint16_t d = rw((uint16_t)(T.r[6] - 4)); // 2(SP) the sectors reached
		T.r[6] = (uint16_t)(T.r[6] - 4);
		fl::cmp(s, d);
	}
	tick(0xFAE4, 30);
	ww(sp_at(2), 0);
	fl::clr();
	tick(0xFAE8, 30);
	T.r[0] = 0;
	fl::clr();
	tick(0xFAEA, 12);
	for (;;)
	{
		jsr(0xFAEE, 0x8D50);                           // wait_vblank
		jsr(0xFAF2, 0x8D86);
		{
			const uint16_t v = peek(0x01C6);           // the sector: (0x01C6 + 4) / 9
			fl::mov(v);
			push(v);
		}
		tick(0xFAF6, 36);
		ww(T.r[6], fl::add(rw(T.r[6]), 0x0004));
		tick(0xFAFA, 27);
		push(0x0009);
		fl::mov(0x0009);
		tick(0xFAFE, 30);
		jsr(0xFB02, 0x90A6);
		ww(sp_at(4), T.r[0]);
		fl::mov(T.r[0]);
		tick(0xFB06, 30);
		T.r[1] = T.r[0];
		fl::mov(T.r[1]);
		tick(0xFB08, 12);
		T.r[1] = fl_asl(T.r[1]);
		tick(0xFB0A, 12);
		const bool reached = fl_logic(rw(sp_at(6)) & peek((uint16_t)(0xFC7C + T.r[1]))) != 0;
		tick(0xFB10, 42);
		if (reached)
			tick(0xFB1E, 12);                          // BNE
		else
		{
			tick(0xFB12, 12);                          // the first time: marked
			ww(T.r[6], T.r[0]);
			fl::mov(T.r[0]);
			tick(0xFB14, 21);
			jsr(0xFB18, 0xFA9E);                       // mark_rotary_sector
			ww(sp_at(6), fl_logic(rw(sp_at(6)) | peek((uint16_t)(0xFC7C + T.r[1]))));
			tick(0xFB1E, 45);
		}
		// button 1
		const bool pressed = fl_logicb((uint8_t)(peekb(0x0193) & 0x40)) != 0;
		tick(0xFB24, 30);
		if (pressed)
		{
			tick(0xFB26, 12);
			T.r[0] = 0x000B;
			fl::mov(T.r[0]);
			tick(0xFB2A, 18);
			tick(0xFB30, 12);                          // BR
		}
		else
		{
			tick(0xFB2C, 12);                          // BEQ
			T.r[0] = 0x008B;
			fl::mov(T.r[0]);
			tick(0xFB30, 18);
		}
		jsr(0xFB34, 0x8AB6);
		const uint8_t asked = peekb(0x0FE6);
		fl::tstb(asked);
		tick(0xFB38, 24);
		// CMP (SP)+,(SP)+ (the arguments) after the branch
		auto drop2 = [](uint16_t next) {
			const uint16_t s = pop();
			const uint16_t d = pop();
			fl::cmp(s, d);
			tick(next, 24);
		};
		if (asked)
		{
			tick(0xFB3A, 12);                          // the next screen
			drop2(0xFB3C);
			tick(0xFB5A, 12);                          // BR
			break;
		}
		tick(0xFB3E, 12);                              // BEQ
		drop2(0xFB40);
		const uint16_t done = rw(sp_at(2));
		fl::cmp(done, 0xFFFF);
		tick(0xFB46, 33);
		if (done != 0xFFFF)
		{
			tick(0xFAEA, 12);                          // BNE
			continue;
		}
		// all sectors reached
		tick(0xFB48, 12);
		T.r[0] = 0x0005;
		fl::mov(T.r[0]);
		tick(0xFB4C, 18);
		jsr(0xFB50, 0x8AB6);
		push(0x0078);
		fl::mov(0x0078);
		tick(0xFB54, 30);
		jsr(0xFB58, 0x8D74);
		drop(0xFB58);
		break;
	}
	{
		const uint16_t s = pop();                      // CMP (SP)+,(SP)+: the locals
		const uint16_t d = pop();
		fl::cmp(s, d);
	}
	tick(0xFB5C, 24);
	c_restore(4);
	return T.r[0];
}

// F:FB5E  selftest_controls_screen(): the items screen (F:F98E) with messages 2, 3 and 8 shows
// button 1 (message 0x0B / 0x8B) until the next screen is asked for (0x0FE6) or the rotary
// controller passes its centre (0x02BE, set by F:8D86).  Then (message 9, two seconds, until it
// has stood still for six frames) the controller tests repeat: centring (F:FA00), clockwise
// (messages 4, 0x0A) and anticlockwise (4, 6) through all sectors (F:FADE).
uint16_t selftest_controls_screen(const Args &)
{
	// MOV #m,R0; JSR PC,@#F:8AB6 at pc
	auto message = [](uint16_t pc, uint16_t m) {
		T.r[0] = m;
		fl::mov(m);
		tick((uint16_t)(pc + 4), 18);
		jsr((uint16_t)(pc + 8), 0x8AB6);
	};
	c_save(4, 0xFB62);
	T.r[6] = (uint16_t)(T.r[6] - 2);                   // TST -(SP): (SP) the argument
	fl::tst(rw(T.r[6]));
	tick(0xFB64, 21);
	jsr(0xFB68, 0xF98E);                               // the items screen
	pokeb(0x02BE, 0);
	fl::clr();
	tick(0xFB6C, 27);
	message(0xFB6C, 0x0002);
	message(0xFB74, 0x0003);
	message(0xFB7C, 0x0008);
	// button 1 until the controller passes its centre (0x02BE) or the next screen
	bool next = false;
	for (;;)
	{
		const uint8_t passed = peekb(0x02BE);
		fl::tstb(passed);
		tick(0xFB88, 24);
		if (passed)
		{
			tick(0xFBB4, 12);                          // BNE
			break;
		}
		tick(0xFB8A, 12);
		jsr(0xFB8E, 0x8D50);                           // wait_vblank
		jsr(0xFB92, 0x8D86);
		const bool pressed = fl_logicb((uint8_t)(peekb(0x0193) & 0x40)) != 0;
		tick(0xFB98, 30);
		if (pressed)
		{
			tick(0xFB9A, 12);
			T.r[0] = 0x000B;
			fl::mov(T.r[0]);
			tick(0xFB9E, 18);
			tick(0xFBA4, 12);                          // BR
		}
		else
		{
			tick(0xFBA0, 12);                          // BEQ
			T.r[0] = 0x008B;
			fl::mov(T.r[0]);
			tick(0xFBA4, 18);
		}
		jsr(0xFBA8, 0x8AB6);
		const uint8_t asked = peekb(0x0FE6);
		fl::tstb(asked);
		tick(0xFBAC, 24);
		if (!asked)
		{
			tick(0xFB84, 12);                          // BEQ
			continue;
		}
		tick(0xFBAE, 12);
		pokeb(0x0FE6, 0);
		fl::clr();
		tick(0xFBB2, 27);
		tick(0xFC4A, 12);                              // BR
		next = true;
		break;
	}
	if (!next)
	{
		// message 9, two seconds, until the controller stands still for six frames
		message(0xFBB4, 0x0009);
		push(0x0078);
		fl::mov(0x0078);
		tick(0xFBC0, 30);
		jsr(0xFBC4, 0x8D74);
		ww(sp_at(2), 0x0006);
		fl::mov(0x0006);
		tick(0xFBCA, 36);
		bool ended = false;
		for (;;)
		{
			jsr(0xFBCE, 0x8D50);
			jsr(0xFBD2, 0x8D86);
			const uint16_t moving = peek(0x01CA);
			fl::tst(moving);
			tick(0xFBD6, 24);
			if (moving)
			{
				tick(0xFBDE, 12);                      // BNE
				ww(sp_at(2), 0x0006);
				fl::mov(0x0006);
				tick(0xFBE4, 36);
			}
			else
			{
				tick(0xFBD8, 12);
				ww(sp_at(2), fl::dec(rw(sp_at(2))));
				tick(0xFBDC, 30);
				tick(0xFBE4, 12);                      // BR
			}
			const uint8_t asked = peekb(0x0FE6);
			fl::tstb(asked);
			tick(0xFBE8, 24);
			if (asked)
			{
				tick(0xFC44, 12);                      // BNE
				ended = true;
				break;
			}
			tick(0xFBEA, 12);
			const uint16_t left = rw(sp_at(2));
			fl::tst(left);
			tick(0xFBEE, 27);
			if (!left)
			{
				tick(0xFBF0, 12);                      // BNE not taken
				break;
			}
			tick(0xFBCA, 12);
		}
		// the controller tests, over and over: centring, clockwise and anticlockwise
		while (!ended)
		{
			jsr(0xFBF4, 0xF98E);
			jsr(0xFBF8, 0x8D86);
			jsr(0xFBFC, 0xFA00);                       // selftest_center_rotary
			message(0xFBFC, 0x0004);
			message(0xFC04, 0x000A);
			ww(T.r[6], 0x0001);
			fl::mov(0x0001);
			tick(0xFC10, 27);
			jsr(0xFC14, 0xFADE);                       // selftest_rotate_sectors(1)
			{
				const uint8_t asked = peekb(0x0FE6);
				fl::tstb(asked);
				tick(0xFC18, 24);
				if (asked)
				{
					tick(0xFC44, 12);                  // BNE
					break;
				}
			}
			tick(0xFC1A, 12);
			jsr(0xFC1E, 0xF98E);
			jsr(0xFC22, 0x8D86);
			jsr(0xFC26, 0xFA00);
			message(0xFC26, 0x0004);
			message(0xFC2E, 0x0006);
			ww(T.r[6], 0xFFFF);
			fl::mov(0xFFFF);
			tick(0xFC3A, 27);
			jsr(0xFC3E, 0xFADE);                       // selftest_rotate_sectors(-1)
			const uint8_t asked = peekb(0x0FE6);
			fl::tstb(asked);
			tick(0xFC42, 24);
			if (asked)
			{
				tick(0xFC44, 12);                      // BEQ not taken
				break;
			}
			tick(0xFBF0, 12);
		}
		pokeb(0x0FE6, 0);
		fl::clr();
		tick(0xFC48, 27);
		drop(0xFC48);
	}
	drop(0xFC4A);
	c_restore(4);
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
