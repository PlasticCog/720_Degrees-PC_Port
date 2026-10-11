// render720: run the native game headless and write screenshots + a WAV of its audio.
//   render720 --roms DIR --frames N [--out DIR] [--png-every K] [--script FILE] [--random SEED] [--wav FILE]
//             [--ram-dump FILE] [--pal-dump FILE] [--hide-pf] [--hide-alpha] [--poke FRAME:ADDR:VALUE]
//             [--extras NAME,NAME...] [--dev-input FROM:TO:BITS]   (extras.h; BITS 1 freeze, 4 step)
//             [--cheat no-water,no-bushes]
#include "nativemachine.h"
#include "soundchips.h"
#include "inputscript.h"
#include <map>
#include "modloader.h"
#include "logic.h"
#include "sound.h"
#include "extras.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

static void write_wav(const std::string &path, const std::vector<int16_t> &s, int rate)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return;
	auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
	auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
	const uint32_t bytes = (uint32_t)(s.size() * 2);
	fwrite("RIFF", 1, 4, f); w32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
	w32(16); w16(1); w16(2); w32(rate); w32(rate * 4); w16(4); w16(16);
	fwrite("data", 1, 4, f); w32(bytes);
	fwrite(s.data(), 2, s.size(), f);
	fclose(f);
}

int main(int argc, char **argv)
{
	std::string roms = "Game", out = ".", script, wav, mod, sound_log, ram_dump, palette_log, in0_log, sio_log, pal_dump,
		sound_ram_dump, draw_ground, pf_dump, mo_log;
	bool hide_pf = false, hide_alpha = false;
	int frames = 600, png_every = 0, seed = 0, stats_every = 0;
	struct Poke { int frame; uint16_t addr, value; };
	std::vector<Poke> pokes;
	std::string extras;
	struct DevInput { int from, to, bits; };
	std::vector<DevInput> dev_inputs;
	for (int i = 1; i < argc; i++)
	{
		std::string a = argv[i];
		auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
		if (a == "--roms") roms = next();
		else if (a == "--frames") frames = atoi(next().c_str());
		else if (a == "--out") out = next();
		else if (a == "--png-every") png_every = atoi(next().c_str());
		else if (a == "--script") script = next();
		else if (a == "--random") seed = atoi(next().c_str());
		else if (a == "--wav") wav = next();
		else if (a == "--mod") mod = next();
		else if (a == "--sound-log") sound_log = next();   // "frame command" per sound command
		else if (a == "--palette-log") palette_log = next(); // "frame scanline addr value" per palette write
		else if (a == "--in0-log") in0_log = next();
		else if (a == "--sound-io-log") sio_log = next();   // sound CPU I/O writes with the time in seconds       // changes of the sound status bits as the main CPU reads them
		else if (a == "--ram-dump") ram_dump = next();     // per frame: main RAM, MO RAM, x/y scroll
		else if (a == "--sound-ram-dump") sound_ram_dump = next();   // per frame: the sound CPU's RAM
		else if (a == "--pal-dump") pal_dump = next();     // per frame: the 256 palette RAM words
		else if (a == "--hide-pf") hide_pf = true;         // screenshots without the playfield
		else if (a == "--hide-alpha") hide_alpha = true;   // screenshots without the text layer
		else if (a == "--difftest") logic::g_difftest = true;   // compare rewritten routines with the originals
		else if (a == "--no-rewrite") logic::g_enabled = false; // run only the translated original
		else if (a == "--cheat")                           // no-water, no-bushes (the debug menu's cheats)
		{
			const std::string c = next();
			logic::g_cheat_no_water |= c.find("no-water") != std::string::npos;
			logic::g_cheat_no_bushes |= c.find("no-bushes") != std::string::npos;
		}
		else if (a == "--draw-ground") draw_ground = next();   // water, bushes: drawn as ground only (play unchanged)
		else if (a == "--pf-dump") pf_dump = next();
		else if (a == "--mo-log") mo_log = next();         // per frame: the visible sprite list entries       // with each screenshot: the playfield words, scroll, tile banks
		else if (a == "--translated") logic::use_translation(next());   // these rewritten routines run translated
		else if (a == "--poke")                            // FRAME:ADDR:VALUE (hex addr/value), main RAM word
		{
			const std::string v = next();
			const size_t c1 = v.find(':'), c2 = v.find(':', c1 + 1);
			pokes.push_back({atoi(v.substr(0, c1).c_str()), (uint16_t)strtol(v.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 16),
				(uint16_t)strtol(v.substr(c2 + 1).c_str(), nullptr, 16)});
		}
		else if (a == "--extras") extras = next();       // switch these extras on (extras.h names)
		else if (a == "--dev-input")                       // FROM:TO:BITS hold the hidden inputs
		{
			const std::string v = next();
			const size_t c1 = v.find(':'), c2 = v.find(':', c1 + 1);
			dev_inputs.push_back({atoi(v.substr(0, c1).c_str()), atoi(v.substr(c1 + 1, c2 - c1 - 1).c_str()),
				atoi(v.substr(c2 + 1).c_str())});
		}
		else if (a == "--audio-stats") stats_every = atoi(next().c_str());   // print chip levels every N frames
	}
	RomSet rs;
	std::string err;
	if (!rs.load(roms, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
	if (!mod.empty() && !apply_mod_file(rs, mod, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
	Extras ex;
	if (!extras.empty())
	{
		std::string text = extras;
		for (char &c : text) if (c == ',') c = '\n';
		std::string lines;
		size_t p = 0;
		while (p < text.size())
		{
			size_t e = text.find('\n', p);
			if (e == std::string::npos) e = text.size();
			lines += text.substr(p, e - p) + " 1\n";
			p = e + 1;
		}
		ex.s.load(lines);
		ex.apply(rs);
		if (!ex.problems.empty()) fprintf(stderr, "%s", ex.problems.c_str());
	}
	NativeMachine m(rs);
	m.reset();
	m.board.chips().audio_enabled = !wav.empty() || stats_every > 0;
	uint32_t sound_cmds = 0;
	FILE *slog = sound_log.empty() ? nullptr : fopen(sound_log.c_str(), "w");
	int cur_frame = 0;
	FILE *rdump = ram_dump.empty() ? nullptr : fopen(ram_dump.c_str(), "wb");
	FILE *sdump = sound_ram_dump.empty() ? nullptr : fopen(sound_ram_dump.c_str(), "wb");
	FILE *pdump = pal_dump.empty() ? nullptr : fopen(pal_dump.c_str(), "wb");
	m.board.debug.hide_pf = hide_pf;
	m.board.debug.hide_alpha = hide_alpha;
	m.board.debug.ground_water = logic::g_cheat_no_water;
	m.board.debug.ground_bushes = logic::g_cheat_no_bushes;
	m.board.debug.ground_water |= draw_ground.find("water") != std::string::npos;
	m.board.debug.ground_bushes |= draw_ground.find("bushes") != std::string::npos;
	FILE *pfdump = pf_dump.empty() ? nullptr : fopen(pf_dump.c_str(), "wb");
	FILE *molog = mo_log.empty() ? nullptr : fopen(mo_log.c_str(), "w");
	if (pfdump)                                        // first the tile tables: terrain, event bytes per code
	{
		uint8_t t[2][8192];
		for (int c = 0; c < 8192; c++) { t[0][c] = m.board.tile_terrain(c); t[1][c] = m.board.tile_event(c); }
		fwrite(t, 1, sizeof(t), pfdump);
	}
	if (stats_every || slog)
		m.board.on_sound_command = [&](uint8_t c) {
			sound_cmds++;
			if (slog) fprintf(slog, "%d %02X %llu\n", cur_frame, c, (unsigned long long)main_cycles_now());
		};
	FILE *ilog = in0_log.empty() ? nullptr : fopen(in0_log.c_str(), "w");
	uint16_t last_in0 = 0xffff;
	if (ilog)
		m.board.on_in0_read = [&](uint16_t v) {
			if ((v & 0x30) != last_in0)
			{
				last_in0 = v & 0x30;
				fprintf(ilog, "%d %02X %llu\n", cur_frame, last_in0, (unsigned long long)main_cycles_now());
			}
		};
	FILE *sio = sio_log.empty() ? nullptr : fopen(sio_log.c_str(), "w");
	std::map<uint16_t, int> last_sread;
	if (sio)
	{
		m.board.on_sound_cpu_io_write = [&](uint16_t a, uint8_t d) {
			fprintf(sio, "%d %04X %02X %.9f%c", cur_frame, a, d, (double)m.board.chips().now() / Board::SOUND_CLOCK, 10);
		};
		m.board.on_sound_cpu_io_read = [&](uint16_t a, uint8_t d) {
			auto it = last_sread.find(a);
			if (it != last_sread.end() && it->second == d) return;
			last_sread[a] = d;
			fprintf(sio, "%d R%04X %02X %.9f%c", cur_frame, a, d, (double)m.board.chips().now() / Board::SOUND_CLOCK, 10);
		};
	}
	FILE *plog = palette_log.empty() ? nullptr : fopen(palette_log.c_str(), "w");
	if (plog)
		m.board.on_palette_write = [&](uint16_t a, uint16_t d, uint16_t) {
			fprintf(plog, "%d %d %llu %04X %04X\n", cur_frame, m.board.scanline, (unsigned long long)main_cycles_now(), a, d);
		};
	InputScript inp;
	if (!script.empty()) inp.load(script);
	inp.random_seed = seed;
	std::vector<uint32_t> rgb(512 * 384);
	std::vector<int16_t> audio, buf(16384);
	double sumsq = 0;
	int peak = 0;
	for (int f = 0; f < frames; f++)
	{
		std::string snap;
		cur_frame = f;
		for (const auto &pk : pokes)
			if (pk.frame == f && pk.addr < 0x0fff)
			{
				m.board.main_ram()[pk.addr] = (uint8_t)pk.value;
				m.board.main_ram()[pk.addr + 1] = (uint8_t)(pk.value >> 8);
			}
		inp.apply(f, m.board.input, snap);
		m.board.input.dev_freeze = m.board.input.dev_step = false;
		for (const auto &d : dev_inputs)
			if (f >= d.from && f < d.to)
			{
				m.board.input.dev_freeze |= (d.bits & 1) != 0;
				m.board.input.dev_step |= (d.bits & 4) != 0;
			}
		m.run_frame();
		if (rdump)
		{
			fwrite(m.board.main_ram(), 1, 0x1000, rdump);
			fwrite(m.board.mo_ram(), 2, 0x400, rdump);
			const uint16_t sc[2] = { m.board.xscroll(), m.board.yscroll() };
			fwrite(sc, 2, 2, rdump);
		}
		if (pdump) fwrite(m.board.palette_ram(), 2, 0x100, pdump);
		if (molog)
		{
			// frame entry x y height code color empty(1: every pixel transparent)
			const uint16_t *mo = m.board.mo_ram();
			bool visited[256] = {};
			int link = 0, last_x = 0;
			for (int n = 0; n < 256 && !visited[link]; n++)
			{
				const uint16_t *e = &mo[link * 4];
				visited[link] = true;
				const int code = (e[1] & 0x7ff) | ((e[0] & 7) << 11);
				int xpos = (e[2] >> 6) & 0x3ff, ypos = -((e[0] >> 6) & 0x1ff);
				const int height = ((e[1] >> 11) & 7) + 1;
				ypos -= height << 4;
				if (e[1] & 0x8000) xpos = last_x + 16;
				last_x = xpos;
				xpos &= 1023;
				ypos &= 511;
				if (xpos >= 640) xpos -= 1024;
				if (ypos >= Board::VTOTAL) ypos -= 512;
				const int idx = link;
				link = (e[3] >> 3) & 0xff;
				if (xpos <= -16 || xpos >= Board::SCREEN_W || ypos >= Board::SCREEN_H || ypos + height * 16 <= 0) continue;
				bool empty = true;
				for (int t = 0; t < height && empty; t++)
					for (int k = 0; k < 256; k++)
						if (m.board.gfx().sprites[((code + t) & 8191) * 256 + k] != 15) { empty = false; break; }
				fprintf(molog, "%d %d %d %d %d %d %d %d\n", f + 1, idx, xpos, ypos, height, code, (e[3] >> 12) & 15, empty ? 1 : 0);
			}
		}
		if (sdump) fwrite(m.board.sound_ram(), 1, 0x1000, sdump);
		if (!snap.empty() || (png_every && (f % png_every) == png_every - 1))
		{
			m.board.frame_rgb(rgb.data());
			if (pfdump)
			{
				const int32_t head[4] = { f + 1, m.board.pf_scrollx(), m.board.pf_scrolly(), m.board.pf_tile_bank(0) | m.board.pf_tile_bank(1) << 8 };
				fwrite(head, 4, 4, pfdump);
				fwrite(m.board.pf_top(), 2, 0x1000, pfdump);
				fwrite(m.board.pf_bottom(), 2, 0x1000, pfdump);
			}
			char name[256];
			snprintf(name, sizeof(name), "%s/%s.png", out.c_str(), snap.empty() ? (std::string("n") + std::to_string(f + 1)).c_str() : snap.c_str());
			write_png(name, rgb.data(), 512, 384);
		}
		if (stats_every && (f % stats_every) == stats_every - 1)
		{
			auto &st = m.board.chips().stats;
			auto db = [&](double e) { return e > 0 ? 10 * std::log10(e / std::max<uint64_t>(1, st.samples)) : -999.0; };
			printf("frames %5d-%5d  level dB: ym %6.1f  pokey %6.1f %6.1f  tms %6.1f   writes: ym %5u pokey %5u %5u tms %4u  sound cmds %u\n",
				f + 1 - stats_every, f, db(st.ym), db(st.pokey[0]), db(st.pokey[1]), db(st.tms),
				st.ym_writes, st.pokey_writes[0], st.pokey_writes[1], st.tms_writes, sound_cmds);
			st = SoundChips::Stats();
			sound_cmds = 0;
		}
		size_t n;
		while ((n = m.board.chips().take_samples(buf.data(), buf.size() / 2)) > 0)
			for (size_t i = 0; i < n * 2; i++)
			{
				audio.push_back(buf[i]);
				sumsq += (double)buf[i] * buf[i];
				peak = std::max(peak, std::abs((int)buf[i]));
			}
	}
	if (!wav.empty())
	{
		write_wav(wav, audio, (int)std::lround(SoundChips::output_rate()));
		printf("audio: %zu frames, rms %.1f, peak %d\n", audio.size() / 2, std::sqrt(sumsq / std::max<size_t>(1, audio.size())), peak);
	}
	if (slog) fclose(slog);
	if (rdump) fclose(rdump);
	if (sdump) fclose(sdump);
	if (pdump) fclose(pdump);
	if (logic::g_difftest || logic::g_enabled)
	{
		printf("%s", logic::report().c_str());
		printf("%s", sound::report().c_str());
	}
	printf("done: %d frames\n", frames);
	return 0;
}
