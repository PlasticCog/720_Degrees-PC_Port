// ref720: headless reference runner used during reverse engineering.
//   ref720 --roms DIR --frames N [--out DIR] [--png-every K] [--script FILE]
//          [--coverage FILE] [--sound-coverage FILE] [--ramdump-every K] [--random SEED]
//          [--poke FRAME:ADDR:VALUE]  (main RAM word, hex address/value; e.g. enter a park:
//                                      --poke 1200:027A:0101 --poke 1200:0F26:0001)
// Script lines: "<frame> <input> <value>"  e.g.  "200 coin1 1", "206 coin1 0", "400 rotate 3"
#include "refmachine.h"
#include "inputscript.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
	std::string roms = "Game", out = "out", script, coverage, sndcov;
	int frames = 600, png_every = 0, ram_every = 0, random_play = 0, trace_from = -1, break_limit = 20;
	std::vector<uint16_t> breaks;
	std::vector<std::pair<uint16_t, uint16_t>> watches;     // --watch A[-B]: log writes to main RAM
	int watch_from = 0, watch_to = 1 << 30;
	struct Poke { int frame; uint16_t addr, value; };
	std::vector<Poke> pokes;
	for (int i = 1; i < argc; i++)
	{
		std::string a = argv[i];
		auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
		if (a == "--roms") roms = next();
		else if (a == "--frames") frames = atoi(next().c_str());
		else if (a == "--out") out = next();
		else if (a == "--png-every") png_every = atoi(next().c_str());
		else if (a == "--ramdump-every") ram_every = atoi(next().c_str());
		else if (a == "--script") script = next();
		else if (a == "--coverage") coverage = next();
		else if (a == "--sound-coverage") sndcov = next();
		else if (a == "--random") random_play = atoi(next().c_str());
		else if (a == "--poke")
		{
			const std::string v = next();
			const size_t c1 = v.find(':'), c2 = v.find(':', c1 + 1);
			pokes.push_back({atoi(v.substr(0, c1).c_str()), (uint16_t)strtol(v.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 16),
				(uint16_t)strtol(v.substr(c2 + 1).c_str(), nullptr, 16)});
		}
		else if (a == "--trace-mem") trace_from = atoi(next().c_str());
		else if (a == "--break") breaks.push_back((uint16_t)strtol(next().c_str(), nullptr, 16));
		else if (a == "--break-limit") break_limit = atoi(next().c_str());
		else if (a == "--watch")
		{
			const std::string v = next();
			const size_t dash = v.find('-');
			const uint16_t lo = (uint16_t)strtol(v.substr(0, dash).c_str(), nullptr, 16);
			watches.push_back({lo, dash == std::string::npos ? lo : (uint16_t)strtol(v.substr(dash + 1).c_str(), nullptr, 16)});
		}
		else if (a == "--watch-frames")
		{
			const std::string v = next();
			const size_t dash = v.find('-');
			watch_from = atoi(v.substr(0, dash).c_str());
			if (dash != std::string::npos) watch_to = atoi(v.substr(dash + 1).c_str());
		}
	}

	RomSet rs;
	std::string err;
	if (!rs.load(roms, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
	RefMachine m(rs);
	m.reset();
	m.board.chips().audio_enabled = false;

	InputScript inp;
	if (!script.empty() && !inp.load(script)) { fprintf(stderr, "cannot read script %s\n", script.c_str()); return 1; }
	inp.random_seed = random_play;

	// coverage: key = pc | (bank+1)<<16 (bank only for code in the banked windows)
	std::map<std::pair<int, int>, int> irqs;
	bool irq_next = false;
	std::set<uint32_t> cov;
	std::set<uint64_t> edges;
	std::set<uint16_t> scov;
	std::set<uint32_t> sedges;
	uint32_t prev_key = 0xffffffff;
	uint16_t sprev = 0;
	struct Ctx { RefMachine *m; std::function<void(uint16_t)> fn; } ctx{&m, nullptr};
	int break_hits = 0;
	if (!coverage.empty() || !breaks.empty())
	{
		m.t11.hook_ctx = &ctx;
		ctx.fn = [&](uint16_t pc) {
			for (uint16_t b : breaks)
				if (b == pc && break_hits < break_limit)
				{
					break_hits++;
					const auto &r = m.t11.r;
					printf("BREAK %04X frame=%llu line=%d bankA=%d bankB=%d vmmu=%d R0=%04X R1=%04X R2=%04X R3=%04X R4=%04X R5=%04X SP=%04X  stack:",
						pc, (unsigned long long)m.board.frame_number, m.board.scanline, m.board.bank_entry(0), m.board.bank_entry(1),
						m.board.vmmu_bank(), r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
					for (int i = 0; i < 10; i++) printf(" %04X", m.board.main_read16((uint16_t)(r[6] + 2 * i), 0xffff));
					printf("\n");
				}
			if (coverage.empty()) return;
			uint32_t key = pc;
			if (pc >= 0x4000 && pc < 0x6000) key |= (uint32_t)(m.board.bank_entry(0) + 1) << 16;
			else if (pc >= 0x6000 && pc < 0x8000) key |= (uint32_t)(m.board.bank_entry(1) + 1) << 16;
			cov.insert(key);
			if (prev_key != 0xffffffff)
			{
				const int d = (int)pc - (int)(prev_key & 0xffff);
				if ((d != 2 && d != 4 && d != 6) && !irq_next)
					edges.insert(((uint64_t)prev_key << 32) | key);
			}
			prev_key = key;
			irq_next = false;
		};
		m.t11.insn_hook = [](void *c, uint16_t pc) { ((Ctx *)c)->fn(pc); };
		static std::function<void(uint8_t, uint16_t)> irqfn;
		irqfn = [&](uint8_t v, uint16_t pc) { irqs[{v, pc}]++; irq_next = true; };
		m.t11.irq_hook = [](void *, uint8_t v, uint16_t pc) { irqfn(v, pc); };
	}
	static std::function<void(uint16_t)> sfn;
	if (!sndcov.empty())
	{
		sfn = [&](uint16_t pc) {
			scov.insert(pc);
			const int d = (int)pc - (int)sprev;
			if (d < 1 || d > 3) sedges.insert(((uint32_t)sprev << 16) | pc);
			sprev = pc;
		};
		m.m6502.insn_hook = [](void *, uint16_t pc) { sfn(pc); };
	}
	std::map<int, int> cmdcount;
	m.board.on_sound_command = [&](uint8_t c) { cmdcount[c]++; };
	std::map<std::string, int> slaplog;
	m.board.on_slapstic = [&](int from, int to, uint32_t addr, int bank) {
		char k[128];
		snprintf(k, sizeof(k), "%d->%d addr=%04X pc=%04X bank=%d", from, to, addr, m.t11.ppc, bank);
		slaplog[k]++;
	};

	// memory tracing: who writes the playfield, who reads banked ROM data
	std::map<uint32_t, uint64_t> pf_writers;                 // ppc -> count
	std::map<uint32_t, uint64_t> pf_readers, attr_readers, odd_bank_writes;
	std::map<uint64_t, uint64_t> rom_readers;                // (ppc, window, bank, addr>>8) -> count
	bool tracing = false;
	int cur_frame = 0;
	m.on_data = [&](uint16_t a, bool wr, bool word, uint16_t v) {
		if (wr && !watches.empty() && cur_frame >= watch_from && cur_frame <= watch_to)
			for (const auto &w : watches)
				if (a >= w.first && a <= w.second)
					printf("WRITE frame %d line %d pc %04X bankA %d bankB %d [%04X] = %04X%s R0=%04X R1=%04X R2=%04X R3=%04X R4=%04X R5=%04X\n",
						cur_frame, m.board.scanline, m.t11.ppc, m.board.bank_entry(0), m.board.bank_entry(1), a, v, word ? "" : " (byte)",
						m.t11.r[0], m.t11.r[1], m.t11.r[2], m.t11.r[3], m.t11.r[4], m.t11.r[5]);
		if (!tracing) return;
		const uint16_t pc = m.t11.ppc;
		if (wr && a >= 0x2000 && a < 0x4000 && (m.board.vmmu_bank() == 2 || m.board.vmmu_bank() == 3))
			pf_writers[pc]++;
		if (!wr && a >= 0x2000 && a < 0x4000 && (m.board.vmmu_bank() == 2 || m.board.vmmu_bank() == 3))
			pf_readers[pc]++;
		if (!wr && a >= 0x0F60 && a < 0x0FA0)
			attr_readers[pc]++;
		if (wr && a >= 0x1400 && a < 0x1480 && (v & 0x03ff))
			odd_bank_writes[pc]++;
		if (!wr && a >= 0x4000 && a < 0x8000)
		{
			const int w = (a >> 13) & 1;
			rom_readers[((uint64_t)pc << 32) | ((uint64_t)w << 24) | ((uint64_t)m.board.bank_entry(w) << 16) | (a & 0xff00)]++;
		}
	};
	std::vector<uint32_t> rgb(Board::SCREEN_W * Board::SCREEN_H);
	for (int f = 0; f < frames; f++)
	{
		std::string snap;
		if (trace_from >= 0 && f >= trace_from) tracing = true;
		cur_frame = f;
		for (const auto &pk : pokes)
			if (pk.frame == f && pk.addr < 0x0fff)
			{
				m.board.main_ram()[pk.addr] = (uint8_t)pk.value;
				m.board.main_ram()[pk.addr + 1] = (uint8_t)(pk.value >> 8);
			}
		inp.apply(f, m.board.input, snap);
		if (!snap.empty())
		{
			m.board.frame_rgb(rgb.data());
			write_png(out + "/" + snap + ".png", rgb.data(), 512, 384);
			// full state dump for offline analysis: main RAM, palette, PF top/bottom, MO, alpha, scroll
			if (FILE *fp = fopen((out + "/" + snap + ".state").c_str(), "wb"))
			{
				fwrite(m.board.main_ram(), 1, 0x1000, fp);
				fwrite(m.board.palette_ram(), 2, 0x100, fp);
				fwrite(m.board.pf_top(), 2, 0x1000, fp);
				fwrite(m.board.pf_bottom(), 2, 0x1000, fp);
				fwrite(m.board.mo_ram(), 2, 0x400, fp);
				fwrite(m.board.alpha_ram(), 2, 0xc00, fp);
				uint16_t regs[4] = { m.board.xscroll(), m.board.yscroll(), (uint16_t)m.board.bank_entry(0), (uint16_t)m.board.bank_entry(1) };
				fwrite(regs, 2, 4, fp);
				fclose(fp);
			}
		}
		m.run_frame();
		if (png_every && (f % png_every) == png_every - 1)
		{
			m.board.frame_rgb(rgb.data());
			char name[512];
			snprintf(name, sizeof(name), "%s/f%05d.png", out.c_str(), f + 1);
			write_png(name, rgb.data(), 512, 384);
		}
		if (ram_every && (f % ram_every) == ram_every - 1)
		{
			char name[512];
			snprintf(name, sizeof(name), "%s/ram%05d.bin", out.c_str(), f + 1);
			if (FILE *fp = fopen(name, "wb")) { fwrite(m.board.main_ram(), 1, 0x1000, fp); fclose(fp); }
		}
	}
	printf("frames=%d t11 insns=%llu cycles=%llu pc=%04X  6502 pc=%04X\n", frames,
		(unsigned long long)m.t11.total_insns, (unsigned long long)m.t11.total_cycles, m.t11.r[7], m.m6502.pc);
	if (trace_from >= 0)
	{
		printf("playfield writers (pc: count):\n");
		for (auto &kv : pf_writers) printf("  %04X: %llu\n", kv.first, (unsigned long long)kv.second);
		printf("bank register writes with low bits set: %zu sites", odd_bank_writes.size());
		for (auto &kv : odd_bank_writes) printf(" %04X:%llu", kv.first, (unsigned long long)kv.second);
		printf("playfield readers (pc: count):\n");
		for (auto &kv : pf_readers) printf("  %04X: %llu\n", kv.first, (unsigned long long)kv.second);
		printf("block attribute table (0F60-0F9F) readers:\n");
		for (auto &kv : attr_readers) printf("  %04X: %llu\n", kv.first, (unsigned long long)kv.second);
		printf("banked ROM data readers (pc window bank page: count):\n");
		for (auto &kv : rom_readers)
			printf("  pc=%04X win=%c bank=%2d page=%04X: %llu\n", (unsigned)(kv.first >> 32), ((kv.first >> 24) & 1) ? 'B' : 'A',
				(int)((kv.first >> 16) & 0xff), (unsigned)(kv.first & 0xff00), (unsigned long long)kv.second);
	}
	printf("interrupts taken:\n");
	for (auto &kv : irqs) printf("  vector %03o -> %04X  x%d\n", kv.first.first, kv.first.second, kv.second);
	printf("slapstic transitions:\n");
	for (auto &kv : slaplog) printf("  %-48s x%d\n", kv.first.c_str(), kv.second);
	printf("sound commands:");
	for (auto &kv : cmdcount) printf(" %02X:%d", kv.first, kv.second);
	printf("\n");
	if (!coverage.empty())
	{
		FILE *fp = fopen(coverage.c_str(), "w");
		for (uint32_t k : cov) fprintf(fp, "%04X %d\n", k & 0xffff, (int)(k >> 16) - 1);
		fclose(fp);
		fp = fopen((coverage + ".edges").c_str(), "w");
		for (uint64_t e : edges)
		{
			const uint32_t a = (uint32_t)(e >> 32), b = (uint32_t)(e & 0xffffffff);
			fprintf(fp, "%04X %d %04X %d\n", a & 0xffff, (int)(a >> 16) - 1, b & 0xffff, (int)(b >> 16) - 1);
		}
		fclose(fp);
		fp = fopen((coverage + ".irq").c_str(), "w");
		for (auto &kv : irqs) fprintf(fp, "%03o %04X %d\n", kv.first.first, kv.first.second, kv.second);
		fclose(fp);
		printf("main coverage: %zu instructions, %zu edges\n", cov.size(), edges.size());
	}
	if (!sndcov.empty())
	{
		FILE *fp = fopen(sndcov.c_str(), "w");
		for (uint16_t k : scov) fprintf(fp, "%04X\n", k);
		fclose(fp);
		fp = fopen((sndcov + ".edges").c_str(), "w");
		for (uint32_t e : sedges) fprintf(fp, "%04X %04X\n", e >> 16, e & 0xffff);
		fclose(fp);
		printf("sound coverage: %zu instructions\n", scov.size());
	}
	return 0;
}
