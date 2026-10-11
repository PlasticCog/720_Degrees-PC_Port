// Rewritten routines: arithmetic helpers, video bank selection, sound command queue and
// the playfield tile bank.  RAM addresses are the original program's variables.
//
// Timing.  The rewritten routines follow the original instruction by instruction: after
// each instruction its cycles are charged and the board may run on and interrupts come in at
// the next instruction's address (tick, logic.h), so that every read and write happens in
// the same time slice as the original's and interrupts land at the same instructions.  The
// stack is kept as the original's (register saves, return addresses), as the interrupts
// push onto it.  The step helpers for that are defined here (logic::steps) and used by the
// other routines_*.cpp files of this group.
#include "logic.h"

namespace logic {
namespace steps {

using namespace t11rt;

// RTS: its cycles, then interrupts may come in at the return address as after the original's
// RTS (with the return address off the stack, as there); the routines using it charge nothing
// in their table entry.  The return address goes back on the stack afterwards for logic.cpp,
// which pops it when the routine returns.
void rts()
{
	const uint16_t ret = pop();
	tick(ret, 21);
	push(ret);
}

// JSR PC,@#addr at ret - 4: the return address goes on the stack, the JSR's cycles are
// charged and interrupts may come in at addr; addr (rewritten or not) returns to ret
void jsr(uint16_t ret, uint16_t addr)
{
	push(ret);
	T.r[7] = addr;
	tick(addr, 30);
	shadow_push(ret);
	dispatch();
	shadow_pop();
	if (T.r[7] != ret) throw Unwound{};
}

// JSR R1,@#F138 / F144 / F152 / F162 at body - 4: the register save of a C routine for R2 up
// to R(n+1) (n = 1..4): R1 and those registers go on the stack, then the address of the
// helper's epilogue, and the helper jumps to the body
void c_save(int n, uint16_t body)
{
	static const uint16_t helper[] = {0, 0xF138, 0xF144, 0xF152, 0xF162};
	uint16_t pc = helper[n];
	const uint16_t r1 = T.r[1];
	push(T.r[1]);
	T.r[1] = body;
	tick(pc, 30);                                      // JSR R1,@#helper
	for (int i = 2; i <= n + 1; i++)
	{
		push(T.r[i]);
		fl::mov(T.r[i]);
		pc += 2;
		tick(pc, 24);                                  // MOV Ri,-(SP)
	}
	push(T.r[1]);
	fl::mov(T.r[1]);
	pc += 2;
	tick(pc, 24);                                      // MOV R1,-(SP)
	T.r[1] = r1;
	fl::mov(r1);
	pc += 4;
	tick(pc, 27);                                      // MOV k(SP),R1
	ww(T.r[6], (uint16_t)(pc + 2));
	tick(body, 30);                                    // JSR PC,@(SP)+
}

// MOV #0x000E,R1 at pc and the two loops after it (F:81B8, F:821A, F:F3BA): the eight vectors
// listed at 0xFC4E get handler h1 with PSW 0xE0, the four 32V vectors listed at 0xFC5E
// handler h2 with PSW 0xC0 (MOV #h,@list(R1); MOV list(R1),R0; MOV #psw,2(R0)); R0 ends as
// the last vector's address
void vector_loops(uint16_t pc, uint16_t h1, uint16_t h2)
{
	for (int k = 0; k < 2; k++)
	{
		const uint16_t list = k ? 0xFC5E : 0xFC4E, h = k ? h2 : h1, psw = k ? 0x00C0 : 0x00E0;
		T.r[1] = k ? 0x0006 : 0x000E;
		fl::mov(T.r[1]);
		const uint16_t loop = (uint16_t)(pc + 4);
		tick(loop, 18);                                // MOV #n,R1
		for (;;)
		{
			poke(peek((uint16_t)(list + T.r[1])), h);
			fl::mov(h);
			tick((uint16_t)(loop + 6), 42);
			T.r[0] = peek((uint16_t)(list + T.r[1]));
			fl::mov(T.r[0]);
			tick((uint16_t)(loop + 0x0A), 27);
			poke((uint16_t)(T.r[0] + 2), psw);
			fl::mov(psw);
			tick((uint16_t)(loop + 0x10), 36);
			T.r[1] = fl::sub(T.r[1], 0x0002);
			tick((uint16_t)(loop + 0x14), 18);
			if ((int16_t)T.r[1] < 0)
			{
				tick((uint16_t)(loop + 0x16), 12);     // BPL not taken
				break;
			}
			tick(loop, 12);
		}
		pc = (uint16_t)(loop + 0x16);
	}
}

// the body's RTS into the register save helper, the helper's epilogue (F172-F17A: the saved
// registers back) and its RTS
void c_restore(int n)
{
	const uint16_t back = pop();
	tick(back, 21);                                    // RTS PC of the body
	uint16_t pc = 0xF172;
	if (n < 4)
	{
		pc = (uint16_t)(0xF17A - 2 * n);
		tick(pc, 12);                                  // BR
	}
	for (int i = n + 1; i >= 2; i--)
	{
		T.r[i] = pop();
		fl::mov(T.r[i]);
		pc += 2;
		tick(pc, 18);                                  // MOV (SP)+,Ri
	}
	T.r[1] = pop();
	f_nzv(nzw(T.r[1]));
	tick(0xF17C, 18);                                  // MOV (SP)+,R1
	rts();
}

}

namespace {

using namespace t11rt;
using namespace steps;

// F:8078 (register interface): select the video RAM window for playfield address R0.
// The video MMU is switched through the slapstic: a read of 0x8000 arms it, a read of
// 0x8038 / 0x803C selects the bank (0x803C for the half of the playfield with bit 13 set).
void select_video_bank()
{
	const bool upper = (T.r[0] & 0x2000) != 0;
	fl::logic((uint16_t)(0x2000 & T.r[0]));
	tick(0x807C, 18);                                  // BIT #0x2000,R0
	if (upper)
	{
		tick(0x8084, 12);                              // BNE
		T.r[0] = 0x003C;
		fl::mov(T.r[0]);
		tick(0x8088, 18);
	}
	else
	{
		tick(0x807E, 12);
		T.r[0] = 0x0038;
		fl::mov(T.r[0]);
		tick(0x8082, 18);
		tick(0x8088, 12);                              // BR
	}
	ww(0x01D0, T.r[0]);                                // current bank, as seen by the game
	fl::mov(T.r[0]);
	tick(0x808C, 27);
	rw(0x8000);
	fl::tst(peek(0x8000));
	tick(0x8090, 24);
	const uint16_t v = rw(0x8000 + T.r[0]);
	T.psw = (uint8_t)((T.psw & ~(NF | ZF | VF | CF)) | nzw(v));
	tick(0x8094, 27);
	rts();
}

// F:80D8  int mul16(int a, int b): product modulo 2^16, shifting and adding until b is
// shifted out or a runs out of bits
uint16_t mul16(const Args &a)
{
	c_save(1, 0x80DC);
	T.r[2] = 0;
	fl::clr();
	tick(0x80DE, 12);
	T.r[1] = a.u(1);
	fl::mov(T.r[1]);
	tick(0x80E2, 27);
	T.r[0] = a.u(0);
	fl::mov(T.r[0]);
	tick(0x80E6, 27);
	for (;;)
	{
		fl::logic((uint16_t)(0x0001 & T.r[0]));
		tick(0x80EA, 18);                              // BIT #1,R0
		if (T.r[0] & 1)
		{
			tick(0x80EC, 12);
			T.r[2] = fl::add(T.r[2], T.r[1]);
			tick(0x80EE, 12);
		}
		else
			tick(0x80EE, 12);                          // BEQ
		T.r[1] = fl::asl(T.r[1]);
		tick(0x80F0, 12);
		if (!T.r[1])
		{
			tick(0x80F6, 12);                          // BEQ
			break;
		}
		tick(0x80F2, 12);
		T.r[0] = fl::asr(T.r[0]);
		tick(0x80F4, 12);
		if (!T.r[0])
		{
			tick(0x80F6, 12);
			break;
		}
		tick(0x80E6, 12);                              // BNE
	}
	T.r[0] = T.r[2];
	fl::mov(T.r[0]);
	tick(0x80F8, 12);
	c_restore(1);
	return T.r[0];
}

// F:80FA  mul32(int a, int b): signed 32-bit product into the long at 0x0204 (shift and add,
// until a runs out of bits or b is shifted out).  R0 is what is left of a.
uint16_t mul32(const Args &a)
{
	c_save(3, 0x80FE);
	T.r[2] = 0;
	fl::clr();
	tick(0x8100, 12);
	T.r[3] = 0;
	fl::clr();
	tick(0x8102, 12);
	T.r[4] = 0;
	fl::clr();
	tick(0x8104, 12);
	T.r[1] = a.u(1);
	fl::mov(T.r[1]);
	tick(0x8108, 27);
	if ((int16_t)T.r[1] < 0)
	{
		tick(0x810A, 12);
		T.r[4] = fl::dec(T.r[4]);                                      // the high word of b
		tick(0x810C, 12);
	}
	else
		tick(0x810C, 12);                              // BPL
	T.r[0] = a.u(0);
	fl::mov(T.r[0]);
	tick(0x8110, 27);
	for (;;)
	{
		fl::logic((uint16_t)(0x0001 & T.r[0]));
		tick(0x8114, 18);                              // BIT #1,R0
		if (T.r[0] & 1)
		{
			tick(0x8116, 12);
			T.r[2] = fl::add(T.r[2], T.r[1]);
			tick(0x8118, 12);                          // ADD R1,R2
			T.r[3] = fl::adc(T.r[3]);
			tick(0x811A, 12);                          // ADC R3
			T.r[3] = fl::add(T.r[3], T.r[4]);
			tick(0x811C, 12);
		}
		else
			tick(0x811C, 12);                          // BEQ
		T.r[0] = fl::asr(T.r[0]);
		tick(0x811E, 12);
		if (!T.r[0])
		{
			tick(0x812A, 12);                          // BEQ
			break;
		}
		tick(0x8120, 12);
		T.r[1] = fl::asl(T.r[1]);
		tick(0x8122, 12);                              // ASL R1
		T.r[4] = fl::rol(T.r[4]);
		tick(0x8124, 12);                              // ROL R4
		if (T.r[4])
		{
			tick(0x8110, 12);                          // BNE
			continue;
		}
		tick(0x8126, 12);
		fl::tst(T.r[1]);
		tick(0x8128, 12);                              // TST R1
		if (T.r[1])
		{
			tick(0x8110, 12);                          // BNE
			continue;
		}
		tick(0x812A, 12);
		break;
	}
	poke(0x0204, T.r[2]);
	fl::mov(T.r[2]);
	tick(0x812E, 27);
	poke(0x0206, T.r[3]);
	fl::mov(T.r[3]);
	tick(0x8132, 27);
	c_restore(3);
	return T.r[0];
}

// F:8E78  void queue_sound(int command): append to the 8-entry sound command ring at 0x0198
// (write index at 0x01A1).  Nothing is queued while the game is in state 4 (0x018F).
uint16_t queue_sound(const Args &a)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0x8E7A, 24);
	const bool off = peekb(0x018F) == 4;
	fl::cmpb(peekb(0x018F), 0x04);
	tick(0x8E80, 30);
	if (off)
		tick(0x8E9E, 12);                              // BEQ; R0 untouched
	else
	{
		tick(0x8E82, 12);
		T.r[0] = 0;
		fl::clr();
		tick(0x8E84, 12);
		T.r[0] = peekb(0x01A1);
		fl::logicb((uint8_t)T.r[0]);
		tick(0x8E88, 24);
		pokeb((uint16_t)(0x0198 + T.r[0]), a.b(0));
		fl::movb(peekb((uint16_t)(T.r[6] + 4)));
		tick(0x8E8E, 45);
		T.r[0] = fl::inc(T.r[0]);
		tick(0x8E90, 12);
		T.r[1] = T.r[0];
		fl::mov(T.r[1]);
		tick(0x8E92, 12);
		T.r[1] = fl::logic((uint16_t)(T.r[1] & 7));
		tick(0x8E96, 18);
		pokeb(0x01A1, 0);
		fl::clr();
		tick(0x8E9A, 27);
		pokeb(0x01A1, fl::logicb((uint8_t)(peekb(0x01A1) | T.r[1])));
		tick(0x8E9E, 27);
	}
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0x8EA0, 18);
	rts();
	return T.r[0];
}

// F:EEE6  void set_tile_bank(int zone): the playfield's second tile bank becomes zone >> 3.
// 0x01D6 / 0x01D8 hold the terrain table bases for the two banks (table at 0xFED2);
// 0x0236 counts changes so that the screen is redrawn.
uint16_t set_tile_bank(const Args &a)
{
	push(T.r[1]);
	fl::mov(T.r[1]);
	tick(0xEEE8, 24);
	poke(0x01CE, a.u(0));
	fl::mov(peek(0x01CE));
	tick(0xEEEE, 42);
	for (uint16_t pc = 0xEEF2; pc <= 0xEEFA; pc += 4)
	{
		poke(0x01CE, fl::asr(peek(0x01CE)));
		tick(pc, 27);                                  // ASR @#0x01CE
	}
	T.r[1] = peek(0x01CE);
	fl::mov(T.r[1]);
	tick(0xEEFE, 24);
	T.r[1] = fl::asl(T.r[1]);
	tick(0xEF00, 12);
	const bool same = peek(0x01D8) == peek((uint16_t)(0xFED2 + T.r[1]));
	fl::cmp(peek(0x01D8), peek((uint16_t)(T.r[1] + 0xFED2)));
	tick(0xEF06, 39);
	if (same)
		tick(0xEF1E, 12);                              // BEQ; R0 untouched
	else
	{
		tick(0xEF08, 12);
		T.r[0] = peek(0x01CC);
		fl::mov(T.r[0]);
		tick(0xEF0C, 24);
		T.r[0] = fl::asl(T.r[0]);
		tick(0xEF0E, 12);
		poke(0x01D6, peek((uint16_t)(0xFED2 + T.r[0])));
		fl::mov(peek(0x01D6));
		tick(0xEF14, 42);
		poke(0x01D8, peek((uint16_t)(0xFED2 + T.r[1])));
		fl::mov(peek(0x01D8));
		tick(0xEF1A, 42);
		poke(0x0236, fl::inc(peek(0x0236)));
		tick(0xEF1E, 27);
	}
	T.r[1] = pop();
	fl::mov(T.r[1]);
	tick(0xEF20, 18);
	rts();
	return T.r[0];
}

}

LOGIC_TABLE(core,
	asm_routine("F", 0x8078, "select_video_bank", select_video_bank, 0),
	c_routine("F", 0x80D8, "mul16", mul16, 0, 0),
	c_routine("F", 0x80FA, "mul32", mul32, 0, 0),
	c_routine("F", 0x8E78, "queue_sound", queue_sound, 0, 0),
	c_routine("F", 0xEEE6, "set_tile_bank", set_tile_bank, 0, 0),
)

}
