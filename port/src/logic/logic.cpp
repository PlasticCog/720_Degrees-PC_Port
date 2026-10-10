// Rewritten game logic runtime, see logic.h.
#include "logic.h"
#include "board.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

extern const logic::Original t11_originals[];
extern const int t11_original_count;
extern const int t11_total_instructions;

namespace logic {

const Routine *routines_core(int &count);
const Routine *routines_small(int &count);
const Routine *routines_misc(int &count);
const Routine *routines_world(int &count);
const Routine *routines_system(int &count);
const Routine *routines_wait(int &count);
const Routine *routines_screen(int &count);
const Routine *routines_boot(int &count);
const Routine *routines_parks(int &count);
const Routine *routines_scores(int &count);
const Routine *routines_fhigh(int &count);
const Routine *routines_banka(int &count);
const Routine *routines_bankb(int &count);
const Routine *routines_flow(int &count);

bool g_enabled = true;
bool g_difftest = false;

namespace {

using namespace t11rt;

struct Slot {
	const Original *orig = nullptr;
	const Routine *r = nullptr;
	uint64_t calls = 0, tests = 0, bad = 0, timeouts = 0, unwound = 0;
	double orig_cycles = 0, native_cycles = 0;            // per tested call, callees included
	std::string first_bad;
};

// the routine tables of the routines_*.cpp files
const TableFn TABLES[] = { routines_core, routines_small, routines_misc, routines_world, routines_system, routines_wait, routines_screen, routines_boot, routines_parks, routines_scores, routines_fhigh, routines_banka, routines_bankb, routines_flow };

std::vector<Routine> &registry()
{
	static std::vector<Routine> r;
	if (r.empty())
		for (TableFn f : TABLES)
		{
			int n = 0;
			const Routine *t = f(n);
			r.insert(r.end(), t, t + n);
		}
	return r;
}

std::vector<Slot> g_slots;
bool g_inited = false;
int g_testing = 0;              // depth of difference tests in progress
bool g_original_side = false;   // the original side of a test runs without replacements

constexpr uint16_t RETURN_SENTINEL = 0xFFFE;

void init()
{
	g_slots.assign(t11_original_count, Slot{});
	for (int i = 0; i < t11_original_count; i++)
	{
		g_slots[i].orig = &t11_originals[i];
		for (const auto &r : registry())
			if (!strcmp(r.seg, t11_originals[i].seg) && r.addr == t11_originals[i].addr) g_slots[i].r = &r;
		if (!g_slots[i].r)
			fprintf(stderr, "logic: %s:%04X is listed in routines.txt but has no replacement\n", t11_originals[i].seg, t11_originals[i].addr);
	}
	for (const auto &r : registry())
	{
		bool listed = false;
		for (int i = 0; i < t11_original_count; i++)
			listed |= !strcmp(r.seg, t11_originals[i].seg) && r.addr == t11_originals[i].addr;
		if (!listed) fprintf(stderr, "logic: %s:%04X (%s) is not listed in routines.txt\n", r.seg, r.addr, r.name);
	}
	g_inited = true;
}

// a polling routine whose interrupt returned elsewhere stops; like the translated code, the
// frame that expects the new PC continues (or the original routine, if the PC is inside it)
void run_native(const Routine &r);
void run(Slot &s)
{
	try { run_native(*s.r); }
	catch (const Unwound &)
	{
		if (!s.unwound++) fprintf(stderr, "logic: %s:%04X %s was left at %04X by an interrupt\n", s.r->seg, s.r->addr, s.r->name, T.r[7]);
		if (!in_shadow(T.r[7])) s.orig->fn();
	}
}

void run_native(const Routine &r)
{
	if (r.c)
	{
		const Args a{T.r[6]};
		const uint16_t r0 = r.c(a);
		if (!(r.flags & VOID)) T.r[0] = r0;
		// the C register-save helpers return through "MOV (SP)+,R1": N/Z from R1, V clear
		T.psw = (uint8_t)((T.psw & ~(NF | ZF | VF)) | nzw(T.r[1]));
	}
	else
		r.asm_fn();
	T.r[7] = pop();
	T.icount -= r.cycles;
	if (r.flags & IRQ)
	{
		// RTI: the PSW comes back too, and a pending interrupt may be taken at once
		T.psw = (uint8_t)pop();
		T.check_irqs = true;
		sync_dyn();
	}
}

std::string fmt(const char *f, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, f);
	vsnprintf(buf, sizeof(buf), f, ap);
	va_end(ap);
	return buf;
}

// A rewritten routine called by another rewritten routine under test (on the replacement's
// side) is tested as well, with its own snapshots; the outer test then continues with the
// original's result of the inner call, as everywhere else.
struct Snapshots { Board::Snapshot b0, b1, b2; };
std::vector<std::unique_ptr<Snapshots>> g_snapshots;   // one set per nesting depth

void difftest(Slot &s)
{
	const bool outer = g_testing == 0;
	if ((int)g_snapshots.size() <= g_testing) g_snapshots.push_back(std::make_unique<Snapshots>());
	Snapshots &sn = *g_snapshots[g_testing];
	Board::Snapshot &b0 = sn.b0, &b1 = sn.b1, &b2 = sn.b2;
	g_testing++;
	const Ctx t0 = T;
	const int sh0 = shadow_top;
	const bool atomic0 = g_atomic;
	const uint16_t min_outer = g_min_sp;
	const uint32_t steps0 = outer ? 0 : g_atomic_steps;
	B->save(b0);
	const uint16_t args[4] = { rw(t0.r[6] + 2), rw(t0.r[6] + 4), rw(t0.r[6] + 6), rw(t0.r[6] + 8) };
	g_atomic = true;
	// a polling routine that has to wait is given up on quickly (the outermost test sets the
	// budget, nested tests share it)
	if (outer) g_atomic_limit = (s.r->flags & WAIT) ? 20000 : ATOMIC_STEP_LIMIT;

	// a call that waits for an interrupt cannot be compared: run it normally instead
	auto give_up = [&]() {
		g_original_side = false;
		B->load(b0);
		T = t0;
		shadow_top = sh0;
		g_atomic = atomic0;
		if (outer) g_atomic_limit = ATOMIC_STEP_LIMIT;
		g_min_sp = min_outer;
		s.timeouts++;
		// the routine now waits for real: what interrupts call meanwhile is tested as usual
		g_testing--;
		run(s);
	};

	// the original translation, without any replacements
	T.check_irqs = true;
	g_min_sp = 0xffff;
	g_atomic_steps = steps0;
	g_original_side = true;
	T.r[7] = s.orig->addr;
	try { s.orig->fn(); }
	catch (const AtomicTimeout &) { give_up(); return; }
	g_original_side = false;
	const Ctx t1 = T;
	const uint16_t min1 = g_min_sp;
	const int cycles1 = (t0.icount - t1.icount) - (t1.aborted - t0.aborted);
	B->save(b1);

	// the replacement, from the same state
	T = t0;
	T.check_irqs = true;
	shadow_top = sh0;
	B->load(b0);
	g_min_sp = 0xffff;
	g_atomic_steps = steps0;
	try { run_native(*s.r); }
	catch (const AtomicTimeout &) { give_up(); return; }
	const Ctx t2 = T;
	const int cycles2 = (t0.icount - t2.icount) - (t2.aborted - t0.aborted);
	const uint16_t min2 = g_min_sp;
	B->save(b2);

	// compare; stack memory below the final stack pointer is dead and not compared
	std::string d;
	int n = 0;
	auto note = [&](const std::string &x) { if (n++ < 8) d += (d.empty() ? "" : ", ") + x; };
	const bool asm_routine = (s.r->flags & ASM) != 0;
	for (int r = 0; r < 8; r++)
	{
		if (r == 0 && (s.r->flags & VOID)) continue;
		if (t1.r[r] != t2.r[r]) note(fmt("R%d %04X/%04X", r, t1.r[r], t2.r[r]));
	}
	if (asm_routine && (t1.psw & 15) != (t2.psw & 15)) note(fmt("flags %X/%X", t1.psw & 15, t2.psw & 15));
	const uint16_t dead_lo = std::min<uint16_t>({min1, min2, t0.r[6]}), dead_hi = t1.r[6];
	for (int a = 0; a < 0x1000; a++)
	{
		if (a >= dead_lo && a < dead_hi) continue;
		if (b1.ram[a] != b2.ram[a]) note(fmt("RAM %04X %02X/%02X", a, b1.ram[a], b2.ram[a]));
	}
	auto cmp16 = [&](const char *what, const uint16_t *x, const uint16_t *y, size_t count) {
		for (size_t i = 0; i < count; i++)
			if (x[i] != y[i]) note(fmt("%s[%zu] %04X/%04X", what, i, x[i], y[i]));
	};
	cmp16("palette", b1.palette.data(), b2.palette.data(), b1.palette.size());
	cmp16("alpha", b1.alpha.data(), b2.alpha.data(), b1.alpha.size());
	cmp16("mo", b1.mo.data(), b2.mo.data(), b1.mo.size());
	cmp16("pf_top", b1.pft.data(), b2.pft.data(), b1.pft.size());
	cmp16("pf_bottom", b1.pfb.data(), b2.pfb.data(), b1.pfb.size());
	if (b1.eeprom != b2.eeprom) note("eeprom");
	if (b1.bank_entry[0] != b2.bank_entry[0] || b1.bank_entry[1] != b2.bank_entry[1]) note(fmt("banks %d,%d/%d,%d", b1.bank_entry[0], b1.bank_entry[1], b2.bank_entry[0], b2.bank_entry[1]));
	if (b1.xscroll != b2.xscroll || b1.yscroll != b2.yscroll) note("scroll registers");
	if (b1.slap_current != b2.slap_current || b1.slap_state != b2.slap_state || b1.slap_armed != b2.slap_armed) note("slapstic");
	if (b1.soundlatch != b2.soundlatch || b1.soundlatch_pending != b2.soundlatch_pending) note("sound latch");
	if (b1.interrupt_enable != b2.interrupt_enable || b1.pending_int_enable != b2.pending_int_enable) note("interrupt enable");
	if (b1.video_int != b2.video_int || b1.scanline_int != b2.scanline_int) note("interrupt acknowledge");
	if (n)
	{
		s.bad++;
		if (s.first_bad.empty())
			s.first_bad = fmt("args %04X %04X %04X %04X: ", args[0], args[1], args[2], args[3]) + d + (n > 8 ? fmt(" (+%d more)", n - 8) : "");
	}
	s.tests++;
	s.orig_cycles += cycles1;
	s.native_cycles += cycles2;

	// continue exactly as the original did
	B->load(b1);
	T = t1;
	T.check_irqs = true;
	shadow_top = sh0;
	g_atomic = atomic0;
	if (outer) g_atomic_limit = ATOMIC_STEP_LIMIT;
	g_min_sp = std::min<uint16_t>({min_outer, min1, min2});
	g_testing--;
}

}

bool enter(int index)
{
	if (!g_inited) init();
	if (!g_enabled || g_original_side || index < 0 || index >= (int)g_slots.size()) return false;
	Slot &s = g_slots[index];
	if (!s.r) return false;
	s.calls++;
	if (g_difftest)
		difftest(s);
	else
		run(s);
	return true;
}

uint16_t call(uint16_t addr, std::initializer_list<uint16_t> args)
{
	for (auto it = std::rbegin(args); it != std::rend(args); ++it) push(*it);
	push(RETURN_SENTINEL);
	T.r[7] = addr;
	shadow_push(RETURN_SENTINEL);
	dispatch();
	shadow_pop();
	if (T.r[7] != RETURN_SENTINEL)
	{
		fprintf(stderr, "logic::call(%04X) did not return normally (PC=%04X)\n", addr, T.r[7]);
		fatal_unknown_pc(T.r[7]);
	}
	T.r[6] = (uint16_t)(T.r[6] + 2 * args.size());
	return T.r[0];
}

void reset_stats()
{
	for (auto &s : g_slots) { s.calls = s.tests = s.bad = s.timeouts = s.unwound = 0; s.orig_cycles = s.native_cycles = 0; s.first_bad.clear(); }
}

int rewritten_count()
{
	if (!g_inited) init();
	int n = 0;
	for (const auto &s : g_slots) n += s.r != nullptr;
	return n;
}

int instructions_rewritten()
{
	if (!g_inited) init();
	int n = 0;
	for (const auto &s : g_slots) if (s.r) n += s.orig->size;
	return n;
}

int instructions_total() { return t11_total_instructions; }

uint64_t mismatches()
{
	uint64_t n = 0;
	for (const auto &s : g_slots) n += s.bad;
	return n;
}

std::string report(bool all)
{
	if (!g_inited) init();
	std::string out = fmt("rewritten routines: %d, %d of %d instructions (%.1f%%)\n", rewritten_count(), instructions_rewritten(),
		instructions_total(), 100.0 * instructions_rewritten() / std::max(1, instructions_total()));
	for (const auto &s : g_slots)
	{
		if (!s.r || (!all && !s.calls)) continue;
		out += fmt("  %s:%04X %-22s calls %8llu", s.r->seg, s.r->addr, s.r->name, (unsigned long long)s.calls);
		if (s.tests)
			out += fmt("  tested %8llu  mismatches %llu  original %.0f cycles/call (charged %d)",
				(unsigned long long)s.tests, (unsigned long long)s.bad, s.orig_cycles / s.tests, s.r->cycles);
		if (s.tests) out += fmt(" native %.0f", s.native_cycles / s.tests);
		if (s.timeouts) out += fmt("  waits (not compared) %llu", (unsigned long long)s.timeouts);
		if (s.unwound) out += fmt("  left by an interrupt %llu", (unsigned long long)s.unwound);
		out += "\n";
		if (!s.first_bad.empty()) out += "      first mismatch: " + s.first_bad + "\n";
	}
	return out;
}

}
