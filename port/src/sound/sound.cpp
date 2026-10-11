// Rewritten sound program runtime, see sound.h.
#include "sound.h"
#include "logic.h"
#include <array>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#ifndef D720_NO_SOUND_TRANSLATION
extern const s65rt::S65Original s65_originals[];
extern const int s65_original_count;
extern const int s65_total_instructions;
#endif

namespace sound {

const Routine *sound_core(int &count);
const Routine *sound_main(int &count);
const Routine *sound_command(int &count);

namespace {

using namespace s65rt;

struct Slot {
	const S65Original *orig = nullptr;
	const Routine *r = nullptr;
	uint64_t calls = 0, tests = 0, bad = 0, timeouts = 0, unwound = 0, step_bad = 0;
	double orig_cycles = 0, native_cycles = 0;            // per tested call, callees included
	std::string first_bad, first_step;
};

struct Table { const char *name; TableFn fn; };
const Table TABLES[] = {
	{"core", sound_core}, {"main", sound_main}, {"command", sound_command},
};

std::vector<Routine> &registry()
{
	static std::vector<Routine> r;
	if (r.empty())
		for (const Table &tb : TABLES)
		{
			int n = 0;
			const Routine *t = tb.fn(n);
			r.insert(r.end(), t, t + n);
		}
	return r;
}

std::vector<Slot> g_slots;
std::unordered_map<uint16_t, int> g_entries;             // entry address -> slot
bool g_inited = false;
bool g_testing = false;                                  // a difference test is running
bool g_original_side = false;                            // its original side: no replacements

void init()
{
#ifdef D720_NO_SOUND_TRANSLATION
	g_slots.assign(registry().size(), Slot{});
	for (size_t i = 0; i < registry().size(); i++) g_slots[i].r = &registry()[i];
#else
	g_slots.assign(s65_original_count, Slot{});
	for (int i = 0; i < s65_original_count; i++)
	{
		g_slots[i].orig = &s65_originals[i];
		for (const auto &r : registry())
			if (r.addr == s65_originals[i].addr) g_slots[i].r = &r;
		if (!g_slots[i].r)
			fprintf(stderr, "sound: %04X is listed in routines.txt but has no replacement\n", s65_originals[i].addr);
	}
	for (const auto &r : registry())
	{
		bool listed = false;
		for (int i = 0; i < s65_original_count; i++) listed |= r.addr == s65_originals[i].addr;
		if (!listed && !(r.flags & INNER)) fprintf(stderr, "sound: %04X (%s) is not listed in routines.txt\n", r.addr, r.name);
	}
#endif
	g_entries.clear();
	for (size_t i = 0; i < g_slots.size(); i++)
		if (g_slots[i].r) g_entries[g_slots[i].r->addr] = (int)i;
	g_reset_hook6 = []() { g_testing = g_original_side = false; };
	g_inited = true;
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

// a routine left by an interrupt: like the translated code, the frame that expects the new PC
// continues (or the original routine, if the PC is inside it; without the translation only a
// routine that starts there can)
void run(Slot &s)
{
	try { s.r->fn(); }
	catch (const Unwound &)
	{
		if (!s.unwound++) fprintf(stderr, "sound: %04X %s was left at %04X by an interrupt\n", s.r->addr, s.r->name, S.pc);
#ifdef D720_NO_SOUND_TRANSLATION
		if (!sin_shadow(S.pc)) sdispatch();
#else
		if (!sin_shadow(S.pc)) s.orig->fn();
#endif
	}
}

#ifndef D720_NO_SOUND_TRANSLATION
std::string step_text(const std::vector<uint64_t> &v, size_t i)
{
	if (i >= v.size()) return "(none)";
	const uint64_t x = v[i];
	return fmt("%04X s %02X p %02X cyc %02X a %02X x %02X y %02X", (unsigned)(x >> 48), (unsigned)(x >> 40) & 0xff,
		(unsigned)(x >> 32) & 0xff, (unsigned)(x >> 24) & 0xff, (unsigned)(x >> 16) & 0xff, (unsigned)(x >> 8) & 0xff,
		(unsigned)x & 0xff);
}

// Run the call twice from the same state, without time slices or interrupts: the original
// translation (its accesses to the chips and latches done for real and recorded), then the
// replacement (the original's reads replayed, its writes only recorded); compare and continue
// with the original's result.  A call that does not finish within the step limit waits for an
// interrupt: it is run for real instead (the chips have seen the original's writes up to there).
// The call ends where it returns to its caller (an interrupt handler: to the interrupted
// code), also when it got there through an RTS used as a jump (an address pushed before), which
// the translation leaves to the dispatcher.
void difftest(Slot &s)
{
	static std::array<uint8_t, 0x1000> r0, r1, r2;
	static std::vector<uint64_t> o, w;
	static std::vector<IoEvent> io_o, io_w;
	const Ctx c0 = S;
	memcpy(r0.data(), SR, 0x1000);
	auto stacked = [&](int k) { return SR[0x100 | (uint8_t)(c0.s + k)]; };
	const uint16_t ret = (s.r->flags & RTI)       ? (uint16_t)(stacked(2) | stacked(3) << 8)
	                   : (s.r->flags & NMI_END) ? (uint16_t)(stacked(5) | stacked(6) << 8)
	                                            : (uint16_t)((stacked(1) | stacked(2) << 8) + 1);
	auto to_return = [&]() {
		while (S.pc != ret) sdispatch();
	};
	const int sh0 = sshadow_top;
	g_testing = true;
	g_atomic6 = true;
	g_io_cyc0 = S.cyc;
	auto start = [&]() {
		S = c0;
		S.end = INT_MIN;                               // every step goes through sync6_slow
		memcpy(SR, r0.data(), 0x1000);
		sshadow_top = sh0;
		g_steps6 = 0;
		g_step_limit6 = 1000000;
		g_io_events.clear();
	};
	auto finish = [&]() {
		g_atomic6 = false;
		g_trace6 = nullptr;
		g_io_mode = 0;
		g_original_side = false;
		g_testing = false;
	};

	// the original
	start();
	o.clear();
	g_trace6 = &o;
	g_io_mode = 1;
	g_original_side = true;
	g_continue6 = true;                                // a wait: the original goes on for real
	g_timed_out6 = false;
	g_io_effects = false;
	g_end6 = c0.end;
	try
	{
		s.orig->fn();
		to_return();
	}
	catch (const Timeout6 &)
	{
		// it waits before it has done anything to the chips or latches: from the start again,
		// for real
		g_continue6 = false;
		S = c0;
		memcpy(SR, r0.data(), 0x1000);
		sshadow_top = sh0;
		finish();
		s.timeouts++;
		run(s);
		return;
	}
	g_continue6 = false;
	if (g_timed_out6)
	{
		// it waited for the main CPU or an interrupt: not compared, the run goes on from there
		g_timed_out6 = false;
		sshadow_top = sh0;
		finish();
		s.timeouts++;
		return;
	}
	g_original_side = false;
	io_o.swap(g_io_events);
	const Ctx c1 = S;
	memcpy(r1.data(), SR, 0x1000);

	// the replacement
	start();
	w.clear();
	g_trace6 = &w;
	g_io_mode = 2;
	g_io_replay = &io_o;
	g_io_replay_pos = 0;
	std::string d;
	int n = 0;
	auto note = [&](const std::string &x) { if (n++ < 8) d += (d.empty() ? "" : ", ") + x; };
	try
	{
		s.r->fn();
		to_return();
	}
	catch (const Timeout6 &) { note("did not finish"); }
	catch (const Unwound &) { note(fmt("left at %04X", S.pc)); }
	io_w.swap(g_io_events);
	const Ctx c2 = S;
	memcpy(r2.data(), SR, 0x1000);

	if (c1.a != c2.a) note(fmt("A %02X/%02X", c1.a, c2.a));
	if (c1.x != c2.x) note(fmt("X %02X/%02X", c1.x, c2.x));
	if (c1.y != c2.y) note(fmt("Y %02X/%02X", c1.y, c2.y));
	if (c1.s != c2.s) note(fmt("S %02X/%02X", c1.s, c2.s));
	if (c1.p != c2.p) note(fmt("P %02X/%02X", c1.p, c2.p));
	if (c1.pc != c2.pc) note(fmt("PC %04X/%04X", c1.pc, c2.pc));
	if (c1.cyc != c2.cyc) note(fmt("cycles %d/%d", c1.cyc - c0.cyc, c2.cyc - c0.cyc));
	for (int a = 0; a < 0x1000; a++)
		if (r1[a] != r2[a]) note(fmt("RAM %03X %02X/%02X", a, r1[a], r2[a]));
	const size_t m = std::min(io_o.size(), io_w.size());
	for (size_t i = 0; i < m; i++)
	{
		const IoEvent &x = io_o[i], &y = io_w[i];
		if (x.addr != y.addr || x.data != y.data || x.write != y.write || x.cyc != y.cyc)
		{
			note(fmt("access %zu: %c%04X %02X at %d / %c%04X %02X at %d", i, x.write ? 'W' : 'R', x.addr, x.data, x.cyc,
				y.write ? 'W' : 'R', y.addr, y.data, y.cyc));
			break;
		}
	}
	if (io_o.size() != io_w.size()) note(fmt("accesses %zu/%zu", io_o.size(), io_w.size()));
	if (n)
	{
		s.bad++;
		if (s.first_bad.empty())
			s.first_bad = fmt("A %02X X %02X Y %02X S %02X: ", c0.a, c0.x, c0.y, c0.s) + d + (n > 8 ? fmt(" (+%d more)", n - 8) : "");
	}
	size_t k = 0;
	const size_t ms = std::min(o.size(), w.size());
	while (k < ms && o[k] == w[k]) k++;
	if (k < ms || o.size() != w.size())
	{
		s.step_bad++;
		if (s.first_step.empty())
			s.first_step = fmt("A %02X X %02X Y %02X, step %zu of %zu/%zu", c0.a, c0.x, c0.y, k, o.size(), w.size()) +
				(k ? fmt(" after %04X", (unsigned)(o[k - 1] >> 48)) : std::string()) + ": original " + step_text(o, k) +
				", rewrite " + step_text(w, k);
	}
	s.tests++;
	s.orig_cycles += c1.cyc - c0.cyc;
	s.native_cycles += c2.cyc - c0.cyc;

	// continue as the original did
	S = c1;
	S.end = c0.end;
	memcpy(SR, r1.data(), 0x1000);
	sshadow_top = sh0;
	finish();
}
#endif

} // namespace

void jsr(uint16_t pc, uint16_t target)
{
	cyc(6);
	const uint16_t ret = (uint16_t)(pc + 2), next = (uint16_t)(pc + 3);
	const uint8_t s0 = S.s;
	push((uint8_t)(ret >> 8));
	push((uint8_t)ret);
	step(target);
	S.pc = target;
	sshadow_push(next);
	sdispatch();
	// an RTS used as a jump inside the callee comes back here with the PC it jumped to (the
	// translation leaves that to its dispatcher): while the return address is still on the
	// stack, the callee goes on there
	while (S.pc != next && S.s < s0) sdispatch();
	sshadow_pop();
	if (S.pc != next) throw Unwound{};
}

void rts()
{
	cyc(6);
	uint16_t pc = pull();
	pc = (uint16_t)(pc | pull() << 8);
	S.pc = (uint16_t)(pc + 1);
	sync6_dyn();
}

void rti()
{
	cyc(6);
	S.p = (uint8_t)((pull() | FU) & ~FB);
	uint16_t pc = pull();
	S.pc = (uint16_t)(pc | pull() << 8);
	sync6_dyn();
}

void jmp(uint16_t target)
{
	cyc(3);
	step(target);
	S.pc = target;
	sdispatch();
}

void rts_jump()
{
	rts();
	sdispatch();
}

bool enter(int index)
{
	if (!g_inited) init();
	if (!logic::g_enabled || g_original_side || index < 0 || index >= (int)g_slots.size()) return false;
	Slot &s = g_slots[index];
	if (!s.r) return false;
	s.calls++;
#ifndef D720_NO_SOUND_TRANSLATION
	if (logic::g_difftest && !g_testing && !(s.r->flags & NOTEST))
		difftest(s);
	else
#endif
		run(s);
	return true;
}

bool run_entry(uint16_t pc)
{
	if (!g_inited) init();
	const auto it = g_entries.find(pc);
	if (it == g_entries.end()) return false;
	Slot &s = g_slots[it->second];
	s.calls++;
	run(s);
	return true;
}

std::string report(bool all)
{
	if (!g_inited) init();
	int routines = 0, insns = 0;
	for (const auto &s : g_slots)
		if (s.r)
		{
			routines++;
			if (s.orig) insns += s.orig->size;
		}
#ifdef D720_NO_SOUND_TRANSLATION
	std::string out = fmt("rewritten sound routines: %d\n", routines);
#else
	std::string out = fmt("rewritten sound routines: %d, %d of %d instructions (%.1f%%)\n", routines, insns,
		s65_total_instructions, 100.0 * insns / (s65_total_instructions ? s65_total_instructions : 1));
#endif
	for (const auto &s : g_slots)
	{
		if (!s.r || (!all && !s.calls)) continue;
		out += fmt("  S:%04X %-22s calls %8llu", s.r->addr, s.r->name, (unsigned long long)s.calls);
		if (s.tests)
			out += fmt("  tested %8llu  mismatches %llu  original %.0f cycles/call (charged 0) native %.0f", (unsigned long long)s.tests,
				(unsigned long long)s.bad, s.orig_cycles / s.tests, s.native_cycles / s.tests);
		if (s.timeouts) out += fmt("  waits (not compared) %llu", (unsigned long long)s.timeouts);
		if (s.unwound) out += fmt("  left by an interrupt %llu", (unsigned long long)s.unwound);
		if (s.step_bad) out += fmt("  steps differ in %llu", (unsigned long long)s.step_bad);
		out += "\n";
		if (!s.first_bad.empty()) out += "      first mismatch: " + s.first_bad + "\n";
		if (!s.first_step.empty()) out += "      first step difference: " + s.first_step + "\n";
	}
	return out;
}

} // namespace sound
