// render720: run the native game headless and write screenshots + a WAV of its audio.
//   render720 --roms DIR --frames N [--out DIR] [--png-every K] [--script FILE] [--random SEED] [--wav FILE]
#include "nativemachine.h"
#include "soundchips.h"
#include "inputscript.h"
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
	std::string roms = "Game", out = ".", script, wav;
	int frames = 600, png_every = 0, seed = 0;
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
	}
	RomSet rs;
	std::string err;
	if (!rs.load(roms, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
	NativeMachine m(rs);
	m.reset();
	m.board.chips().audio_enabled = !wav.empty();
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
		inp.apply(f, m.board.input, snap);
		m.run_frame();
		if (!snap.empty() || (png_every && (f % png_every) == png_every - 1))
		{
			m.board.frame_rgb(rgb.data());
			char name[256];
			snprintf(name, sizeof(name), "%s/%s.png", out.c_str(), snap.empty() ? (std::string("n") + std::to_string(f + 1)).c_str() : snap.c_str());
			write_png(name, rgb.data(), 512, 384);
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
	printf("done: %d frames\n", frames);
	return 0;
}
