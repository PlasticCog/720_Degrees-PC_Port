// render720: run the native game headless and write screenshots + a WAV of its audio.
//   render720 --roms DIR --frames N [--out DIR] [--png-every K] [--script FILE] [--random SEED] [--wav FILE]
#include "nativemachine.h"
#include "soundchips.h"
#include "inputscript.h"
#include "modloader.h"
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
	std::string roms = "Game", out = ".", script, wav, mod, sound_log;
	int frames = 600, png_every = 0, seed = 0, stats_every = 0;
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
		else if (a == "--audio-stats") stats_every = atoi(next().c_str());   // print chip levels every N frames
	}
	RomSet rs;
	std::string err;
	if (!rs.load(roms, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
	if (!mod.empty() && !apply_mod_file(rs, mod, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
	NativeMachine m(rs);
	m.reset();
	m.board.chips().audio_enabled = !wav.empty() || stats_every > 0;
	uint32_t sound_cmds = 0;
	FILE *slog = sound_log.empty() ? nullptr : fopen(sound_log.c_str(), "w");
	int cur_frame = 0;
	if (stats_every || slog)
		m.board.on_sound_command = [&](uint8_t c) { sound_cmds++; if (slog) fprintf(slog, "%d %02X\n", cur_frame, c); };
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
		inp.apply(f, m.board.input, snap);
		m.run_frame();
		if (!snap.empty() || (png_every && (f % png_every) == png_every - 1))
		{
			m.board.frame_rgb(rgb.data());
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
	printf("done: %d frames\n", frames);
	return 0;
}
