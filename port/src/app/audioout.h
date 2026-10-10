// Audio output for the game: feeds the sound board's samples to an SDL audio stream.
//
// The game makes samples at the arcade rate while the main loop is paced by the system
// clock and vsync, so audio arrives in frame-sized bursts with some jitter, and the two
// clocks drift apart slowly.  To play without gaps the stream keeps a cushion of queued
// audio (TARGET_MS).  A small resampling correction of at most +/-0.5 %, too small to hear,
// keeps the cushion near its target; if the queue still runs dry it is refilled with silence.
#pragma once
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdint>
#include <vector>

class AudioOut {
public:
	static constexpr double TARGET_MS = 60.0;      // cushion kept queued
	static constexpr double HIGH_MS = 150.0;       // above this, new audio is dropped until back near target
	static constexpr double MAX_CORRECTION = 0.005;

	bool open(int rate)
	{
		m_rate = rate;
		const SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, rate };
		m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
		if (!m_stream) return false;
		SDL_ResumeAudioStreamDevice(m_stream);
		return true;
	}
	void close()
	{
		if (m_stream) SDL_DestroyAudioStream(m_stream);
		m_stream = nullptr;
	}
	bool active() const { return m_stream != nullptr; }

	// call once per emulated frame with the samples that frame produced (stereo frames)
	void push(const int16_t *samples, size_t frames)
	{
		if (!m_stream) return;
		double q = queued_ms();
		if (m_primed)
		{
			stats.checks++;
			stats.sum_ms += q;
			stats.min_ms = std::min(stats.min_ms, q);
			if (q <= 0) stats.dry++;
		}
		if (!m_primed || q <= 0)
		{
			// (re)start with a cushion of silence
			std::vector<int16_t> zero((size_t)(m_rate * TARGET_MS / 1000.0) * 2, 0);
			SDL_PutAudioStreamData(m_stream, zero.data(), (int)(zero.size() * sizeof(int16_t)));
			q += TARGET_MS;
			m_avg_ms = TARGET_MS;
			m_primed = true;
		}
		// a burst (e.g. the device starting late) can leave far too much queued, which would
		// add lasting delay: drop audio until the queue is back near the target
		if (q > HIGH_MS) m_draining = true;
		if (m_draining && q < TARGET_MS * 1.25) m_draining = false;
		if (m_draining) stats.dropped++;
		else if (frames) SDL_PutAudioStreamData(m_stream, samples, (int)(frames * 2 * sizeof(int16_t)));

		// smooth the queue level (the device takes data in chunks), then steer towards the target
		m_avg_ms += (q - m_avg_ms) * 0.05;
		const double err = (m_avg_ms - TARGET_MS) / TARGET_MS;
		const double ratio = 1.0 + std::clamp(err * MAX_CORRECTION, -MAX_CORRECTION, MAX_CORRECTION);
		SDL_SetAudioStreamFrequencyRatio(m_stream, (float)ratio);
	}

	// after a pause the queue drains; start again with a fresh cushion
	void restart() { if (m_stream) SDL_ClearAudioStream(m_stream); m_primed = m_draining = false; }

	struct Stats { uint64_t checks = 0, dry = 0, dropped = 0; double sum_ms = 0, min_ms = 1e9; } stats;

private:
	SDL_AudioStream *m_stream = nullptr;
	int m_rate = 0;
	bool m_primed = false, m_draining = false;
	double m_avg_ms = TARGET_MS;

	double queued_ms() const
	{
		return SDL_GetAudioStreamQueued(m_stream) / (2.0 * sizeof(int16_t)) * 1000.0 / m_rate;
	}
};
