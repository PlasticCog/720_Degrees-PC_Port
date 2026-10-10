// verify720: lockstep comparison of the native (translated) game against the reference
// model.  Both are fed the same inputs; after every frame all RAM, video memory, scroll
// and bank registers and the main CPU registers must be identical.
//   verify720 --roms DIR --frames N [--script FILE] [--random SEED] [--out DIR]
#include "refmachine.h"
#include "nativemachine.h"
#include "t11rt.h"
#include "s65rt.h"
#include "inputscript.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static bool cmp_region(const char *name, const void *a, const void *b, size_t bytes, int frame)
{
	if (!memcmp(a, b, bytes)) return true;
	const uint8_t *pa = (const uint8_t *)a, *pb = (const uint8_t *)b;
	size_t first = 0;
	while (pa[first] == pb[first]) first++;
	int count = 0;
	for (size_t i = 0; i < bytes; i++) count += pa[i] != pb[i];
	printf("frame %d: %s differs at offset 0x%04zX (ref %02X, native %02X), %d bytes differ\n",
		frame, name, first, pa[first], pb[first], count);
	return false;
}

int main(int argc, char **argv)
{
	std::string roms = "Game", out = ".", script;
	int frames = 600, seed = 0;
	for (int i = 1; i < argc; i++)
	{
		std::string a = argv[i];
		auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
		if (a == "--roms") roms = next();
		else if (a == "--frames") frames = atoi(next().c_str());
		else if (a == "--script") script = next();
		else if (a == "--random") seed = atoi(next().c_str());
		else if (a == "--out") out = next();
	}
	RomSet rs;
	std::string err;
	if (!rs.load(roms, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }

	RefMachine ref(rs);
	ref.simple_slapstic = true;
	NativeMachine nat(rs);
	ref.reset();
	nat.reset();
	ref.board.chips().audio_enabled = false;
	nat.board.chips().audio_enabled = false;

	InputScript in_ref, in_nat;
	if (!script.empty()) { in_ref.load(script); in_nat.load(script); }
	in_ref.random_seed = in_nat.random_seed = seed;

	std::vector<uint32_t> rgb(512 * 384);
	for (int f = 0; f < frames; f++)
	{
		std::string snap;
		in_ref.apply(f, ref.board.input, snap);
		in_nat.apply(f, nat.board.input, snap);
		ref.run_frame();
		nat.run_frame();

		bool ok = true;
		ok &= cmp_region("main RAM", ref.board.main_ram(), nat.board.main_ram(), 0x1000, f);
		ok &= cmp_region("palette", ref.board.palette_ram(), nat.board.palette_ram(), 0x200, f);
		ok &= cmp_region("alpha RAM", ref.board.alpha_ram(), nat.board.alpha_ram(), 0x1800, f);
		ok &= cmp_region("MO RAM", ref.board.mo_ram(), nat.board.mo_ram(), 0x800, f);
		ok &= cmp_region("PF top", ref.board.pf_top(), nat.board.pf_top(), 0x2000, f);
		ok &= cmp_region("PF bottom", ref.board.pf_bottom(), nat.board.pf_bottom(), 0x2000, f);
		ok &= cmp_region("sound RAM", ref.board.sound_ram(), nat.board.sound_ram(), 0x1000, f);
		ok &= cmp_region("frame", ref.board.frame_indices(), nat.board.frame_indices(), 512 * 384 * 2, f);
		using t11rt::T;
		if (memcmp(ref.t11.r, T.r, sizeof(T.r)) || ref.t11.psw != T.psw)
		{
			printf("frame %d: CPU state differs\n  ref:    R0=%04X R1=%04X R2=%04X R3=%04X R4=%04X R5=%04X SP=%04X PC=%04X PSW=%03o\n"
				"  native: R0=%04X R1=%04X R2=%04X R3=%04X R4=%04X R5=%04X SP=%04X PC=%04X PSW=%03o\n", f,
				ref.t11.r[0], ref.t11.r[1], ref.t11.r[2], ref.t11.r[3], ref.t11.r[4], ref.t11.r[5], ref.t11.r[6], ref.t11.r[7], ref.t11.psw,
				T.r[0], T.r[1], T.r[2], T.r[3], T.r[4], T.r[5], T.r[6], T.r[7], T.psw);
			ok = false;
		}
		{
			const auto &S = s65rt::S;
			const auto &M = ref.m6502;
			if (M.a != S.a || M.x != S.x || M.y != S.y || M.s != S.s || M.p != S.p || M.pc != S.pc)
			{
				printf("frame %d: sound CPU state differs\n  ref:    A=%02X X=%02X Y=%02X S=%02X P=%02X PC=%04X\n"
					"  native: A=%02X X=%02X Y=%02X S=%02X P=%02X PC=%04X\n",
					f, M.a, M.x, M.y, M.s, M.p, M.pc, S.a, S.x, S.y, S.s, S.p, S.pc);
				ok = false;
			}
		}
		if (ref.board.xscroll() != nat.board.xscroll() || ref.board.yscroll() != nat.board.yscroll() ||
			ref.board.bank_entry(0) != nat.board.bank_entry(0) || ref.board.bank_entry(1) != nat.board.bank_entry(1) ||
			ref.board.vmmu_bank() != nat.board.vmmu_bank())
		{
			printf("frame %d: scroll/bank registers differ\n", f);
			ok = false;
		}
		if (!ok)
		{
			ref.board.frame_rgb(rgb.data());
			write_png(out + "/mismatch_ref.png", rgb.data(), 512, 384);
			nat.board.frame_rgb(rgb.data());
			write_png(out + "/mismatch_native.png", rgb.data(), 512, 384);
			printf("FAILED at frame %d\n", f);
			return 2;
		}
		if ((f % 1000) == 999) { printf("frame %d ok\n", f + 1); fflush(stdout); }
	}
	printf("PASS: %d frames identical\n", frames);
	return 0;
}
