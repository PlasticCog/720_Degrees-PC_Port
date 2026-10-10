// Runtime for the translated main program: bus glue, interrupts, dispatch, time slicing.
#include "t11rt.h"
#include "coro.h"
#include <cstdio>
#include <cstdlib>

namespace t11rt {

Ctx T;
Board *B = nullptr;
uint8_t *RAM = nullptr;
const uint8_t *ROM = nullptr;
uint16_t shadow[256];
int shadow_top = 0;
uint16_t g_patch_town_alt_base = 0x6000;
uint16_t g_patch_park_odd_base = 0xA000;
uint16_t g_patch_gravity = 0x000C;
uint16_t g_patch_kick_speed = 0x0180;

void *g_game_fiber = nullptr;
void *g_host_fiber = nullptr;

// ---------------------------------------------------------------- bus
uint16_t bus_rw(uint32_t a)
{
	if (a >= 0x8000)
	{
		if (a < 0x8040) slap(a);
		return rom16(a);
	}
	return B->main_read16((uint16_t)a, 0xffff);
}

uint8_t bus_rb(uint32_t a)
{
	if (a >= 0x8000)
	{
		if (a < 0x8040) slap(a);
		return ROM[a];
	}
	return B->main_read8((uint16_t)a);
}

void bus_ww(uint32_t a, uint16_t v)
{
	if (a >= 0x8000)
	{
		if (a < 0x8040) slap(a);
		return;
	}
	B->main_write16((uint16_t)a, v, 0xffff);
}

void bus_wb(uint32_t a, uint8_t v)
{
	if (a >= 0x8000)
	{
		if (a < 0x8040) slap(a);
		return;
	}
	B->main_write8((uint16_t)a, v);
}

// ---------------------------------------------------------------- interrupts
namespace {
struct IrqEntry { uint8_t priority, vector; };
const IrqEntry irq_table[16] = {
	{ 0 << 5, 0000 }, { 4 << 5, 0070 }, { 4 << 5, 0064 }, { 4 << 5, 0060 },
	{ 5 << 5, 0134 }, { 5 << 5, 0130 }, { 5 << 5, 0124 }, { 5 << 5, 0120 },
	{ 6 << 5, 0114 }, { 6 << 5, 0110 }, { 6 << 5, 0104 }, { 6 << 5, 0100 },
	{ 7 << 5, 0154 }, { 7 << 5, 0150 }, { 7 << 5, 0144 }, { 7 << 5, 0140 },
};

void yield_to_host()
{
	coro::switch_to(g_host_fiber);
}

void take_interrupt(uint8_t vector)
{
	const uint16_t new_pc = rw(vector);
	const uint16_t new_psw = rw(vector + 2);
	const uint16_t ret = T.r[7];
	push(T.psw);
	push(ret);
	T.r[7] = new_pc;
	T.psw = (uint8_t)new_psw;
	T.icount -= 114;
	T.wait_state = false;
	if (T.icount <= 0) sync_slow(new_pc);
	shadow_push(ret);
	dispatch();
	shadow_pop();
}

void check_irqs()
{
	const IrqEntry &e = irq_table[T.cp_state & 15];
	if (e.priority > (T.psw & 0340))
		take_interrupt(e.vector);
}
}

bool g_atomic = false;
uint16_t g_min_sp = 0xffff;

bool sync_slow(uint16_t next)
{
	T.r[7] = next;
	if (g_atomic)
	{
		if (T.r[6] < g_min_sp) g_min_sp = T.r[6];
		return true;
	}
	for (;;)
	{
		if (T.check_irqs)
		{
			T.check_irqs = false;
			check_irqs();
		}
		if (T.icount > 0 && !T.wait_state) break;
		if (T.wait_state) T.icount = 0;
		yield_to_host();
		T.check_irqs = true;     // the CPU samples its interrupt lines at every slice start
	}
	return T.r[7] == next;
}

bool in_shadow(uint16_t pc)
{
	for (int i = shadow_top - 1; i >= 0; i--)
		if (shadow[i] == pc) return true;
	return false;
}

void dispatch()
{
	const uint16_t pc = T.r[7];
	const T11CodeSeg *seg = nullptr;
	if (pc >= 0x8000) seg = &t11_seg_fixed;
	else if (pc >= 0x4000)
	{
		const int w = (pc >> 13) & 1;
		const int e = B->bank_entry(w);
		seg = (e >= 0 && e < 64) ? t11_seg_bank[w][e] : nullptr;
	}
	if (!seg || (pc & 1)) fatal_unknown_pc(pc);
	const unsigned idx = (unsigned)(pc - seg->base) >> 1;
	if (idx >= seg->size / 2u || !seg->fn[idx]) fatal_unknown_pc(pc);
	seg->fn[idx]();
}

void trap_to(uint16_t vector, uint16_t next)
{
	push(T.psw);
	push(next);
	T.r[7] = rw(vector);
	T.psw = (uint8_t)rw(vector + 2);
	T.check_irqs = true;
	sync_slow(T.r[7]);
	shadow_push(next);
	dispatch();
	shadow_pop();
}

void halt_insn(uint16_t next)
{
	push(T.psw);
	push(next);
	T.r[7] = 0x8004;
	T.psw = 0340;
	T.check_irqs = true;
}

void fatal_unknown_pc(uint16_t pc)
{
	fprintf(stderr, "FATAL: translated main program reached untranslated code at PC=%04X (bank A=%d B=%d, SP=%04X)\n",
		pc, B ? B->bank_entry(0) : -1, B ? B->bank_entry(1) : -1, T.r[6]);
	fflush(stderr);
	abort();
}

// ---------------------------------------------------------------- fiber entry
void game_fiber_main(void *)
{
	check_irqs();
	for (;;)
	{
		shadow_top = 0;
		dispatch();
	}
}

} // namespace t11rt
