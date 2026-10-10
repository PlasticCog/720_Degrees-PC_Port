// System 2 audio chips, see soundchips.h
#include "soundchips.h"
#include "tms5220.h"
#include "ymfm_opm.h"
#include <algorithm>
#include <cstring>
#include <cmath>

// ============================================================================ POKEY

namespace {
enum : uint8_t {
	NOTPOLY5 = 0x80, POLY4 = 0x40, PURE = 0x20, VOLUME_ONLY = 0x10, VOLUME_MASK = 0x0f,
	POLY9 = 0x80, CH1_HICLK = 0x40, CH3_HICLK = 0x20, CH12_JOINED = 0x10, CH34_JOINED = 0x08,
	CH1_FILTER = 0x04, CH2_FILTER = 0x02, CLK_15KHZ = 0x01,
	SK_RESET = 0x03, SK_PADDLE = 0x04, SK_TWOTONE = 0x08,
};
constexpr int DIV_64 = 28, DIV_15 = 114;
constexpr int CLK_1 = 0, CLK_28 = 1, CLK_114 = 2;
}

Pokey::Pokey()
{
	auto init45 = [](uint32_t *poly, int size) {
		const int mask = (1 << size) - 1;
		uint32_t lfsr = 0;
		const int xorbit = size - 1;
		for (int i = 0; i < mask; i++)
		{
			lfsr = (lfsr << 1) | (~((lfsr >> 2) ^ (lfsr >> xorbit)) & 1);
			poly[i] = lfsr & mask;
		}
	};
	init45(m_poly4, 4);
	init45(m_poly5, 5);
	{
		uint32_t lfsr = 0x1ff;
		for (uint32_t i = 0; i < 0x1ff; i++)
		{
			const uint32_t in = (lfsr & 1) ^ ((lfsr >> 5) & 1);
			lfsr = (lfsr >> 1) | (in << 8);
			m_poly9[i] = lfsr;
		}
	}
	m_poly17.resize(0x1ffff);
	{
		uint32_t lfsr = 0x1ffff;
		for (uint32_t i = 0; i < 0x1ffff; i++)
		{
			const uint32_t in8 = ((lfsr >> 8) & 1) ^ ((lfsr >> 13) & 1);
			const uint32_t in = lfsr & 1;
			lfsr >>= 1;
			lfsr = (lfsr & 0xff7f) | (in8 << 7);
			lfsr = (in << 16) | lfsr;
			m_poly17[i] = lfsr;
		}
	}
	reset();
}

void Pokey::reset()
{
	for (auto &c : m_ch) c = Channel{};
	m_p4 = m_p5 = m_p9 = m_p17 = 0;
	m_clock_cnt[0] = m_clock_cnt[1] = m_clock_cnt[2] = 0;
	m_audctl = m_skctl = m_skstat = m_irqen = m_allpot = 0;
	m_irqst = 0x08;
	memset(m_potx, 0, sizeof(m_potx));
	m_pot_counter = 0;
	m_out_raw = 0;
	m_old_raw_inval = true;
	potgo();
}

void Pokey::potgo()
{
	m_allpot = 0;
	m_pot_counter = 0;
	for (auto &p : m_potx) p = 228;
}

void Pokey::process_channel(int ch)
{
	if ((m_ch[ch].audc & NOTPOLY5) || (m_poly5[m_p5] & 1))
	{
		if (m_ch[ch].audc & PURE) m_ch[ch].output ^= 1;
		else if (m_ch[ch].audc & POLY4) m_ch[ch].output = m_poly4[m_p4] & 1;
		else if (m_audctl & POLY9) m_ch[ch].output = m_poly9[m_p9] & 1;
		else m_ch[ch].output = m_poly17[m_p17] & 1;
		m_old_raw_inval = true;
	}
}

void Pokey::step()
{
	if (m_skctl & SK_RESET)
	{
		if (++m_p4 == 0x0f) m_p4 = 0;
		if (++m_p5 == 0x1f) m_p5 = 0;
		if (++m_p9 == 0x1ff) m_p9 = 0;
		if (++m_p17 == 0x1ffff) m_p17 = 0;

		int trig[3] = {1, 0, 0};
		if (++m_clock_cnt[CLK_28] >= DIV_64) { m_clock_cnt[CLK_28] = 0; trig[CLK_28] = 1; }
		if (++m_clock_cnt[CLK_114] >= DIV_15) { m_clock_cnt[CLK_114] = 0; trig[CLK_114] = 1; }

		if ((m_audctl & CH1_HICLK) && trig[CLK_1])
			m_ch[0].inc_chan((m_audctl & CH12_JOINED) ? 7 : 4);
		const int base = (m_audctl & CLK_15KHZ) ? CLK_114 : CLK_28;
		if (!(m_audctl & CH1_HICLK) && trig[base])
			m_ch[0].inc_chan(1);
		if ((m_audctl & CH3_HICLK) && trig[CLK_1])
			m_ch[2].inc_chan((m_audctl & CH34_JOINED) ? 7 : 4);
		if (!(m_audctl & CH3_HICLK) && trig[base])
			m_ch[2].inc_chan(1);
		if (trig[base])
		{
			if (!(m_audctl & CH12_JOINED)) m_ch[1].inc_chan(1);
			if (!(m_audctl & CH34_JOINED)) m_ch[3].inc_chan(1);
		}
		if ((trig[CLK_114] || (m_skctl & SK_PADDLE)) && m_pot_counter < 228)
		{
			m_pot_counter++;
			for (int pot = 0; pot < 8; pot++)
				if (m_potx[pot] < m_pot_counter || m_pot_counter == 228)
					m_allpot |= (1 << pot);
		}
	}

	if (m_ch[2].check_borrow())
	{
		if (m_audctl & CH34_JOINED) m_ch[3].inc_chan(1);
		else m_ch[2].reset_channel();
		process_channel(2);
		if (m_audctl & CH1_FILTER) m_ch[0].sample(); else m_ch[0].filter_sample = 1;
		m_old_raw_inval = true;
	}
	if (m_ch[3].check_borrow())
	{
		if (m_audctl & CH34_JOINED) m_ch[2].reset_channel();
		m_ch[3].reset_channel();
		process_channel(3);
		if (m_audctl & CH2_FILTER) m_ch[1].sample(); else m_ch[1].filter_sample = 1;
		m_old_raw_inval = true;
	}
	if ((m_skctl & SK_TWOTONE) && m_ch[1].borrow_cnt == 1)
	{
		m_ch[0].reset_channel();
		m_old_raw_inval = true;
	}
	if (m_ch[0].check_borrow())
	{
		if (m_audctl & CH12_JOINED) m_ch[1].inc_chan(1);
		else m_ch[0].reset_channel();
		process_channel(0);
	}
	if (m_ch[1].check_borrow())
	{
		if (m_audctl & CH12_JOINED) m_ch[0].reset_channel();
		m_ch[1].reset_channel();
		process_channel(1);
	}
	if (m_old_raw_inval)
	{
		uint32_t sum = 0;
		for (int ch = 0; ch < 4; ch++)
			sum |= (((m_ch[ch].output ^ m_ch[ch].filter_sample) || (m_ch[ch].audc & VOLUME_ONLY)) ?
				((m_ch[ch].audc & VOLUME_MASK) << (ch * 4)) : 0);
		m_out_raw = sum;
		m_old_raw_inval = false;
	}
}

int Pokey::output() const
{
	int out = 0;
	for (int i = 0; i < 4; i++) out += (m_out_raw >> (4 * i)) & 0x0f;
	return out;
}

uint8_t Pokey::read(int offset, uint8_t allpot_input)
{
	switch (offset & 15)
	{
	case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
		return (m_allpot & (1 << (offset & 7))) ? m_potx[offset & 7] : m_pot_counter;
	case 0x08:
		if ((m_skctl & SK_RESET) == 0) return m_allpot;
		m_allpot = allpot_input;
		return allpot_input;
	case 0x09: return 0x09;
	case 0x0a:
		if (m_audctl & POLY9) return m_poly9[m_p9] & 0xff;
		return (m_poly17[m_p17] >> 8) & 0xff;
	case 0x0d: return 0x00;
	case 0x0e: return m_irqst ^ 0xff;
	case 0x0f: return m_skstat ^ 0xff;
	default: return 0xff;
	}
}

void Pokey::write(int offset, uint8_t data)
{
	switch (offset & 15)
	{
	case 0x00: m_ch[0].audf = data; break;
	case 0x01: m_ch[0].audc = data; m_old_raw_inval = true; break;
	case 0x02: m_ch[1].audf = data; break;
	case 0x03: m_ch[1].audc = data; m_old_raw_inval = true; break;
	case 0x04: m_ch[2].audf = data; break;
	case 0x05: m_ch[2].audc = data; m_old_raw_inval = true; break;
	case 0x06: m_ch[3].audf = data; break;
	case 0x07: m_ch[3].audc = data; m_old_raw_inval = true; break;
	case 0x08:
		if (data == m_audctl) return;
		m_audctl = data;
		m_old_raw_inval = true;
		break;
	case 0x09:  // STIMER
		for (int i = 0; i < 4; i++)
		{
			m_ch[i].reset_channel();
			m_ch[i].output = 0;
			m_ch[i].filter_sample = (i < 2 ? 1 : 0);
		}
		m_old_raw_inval = true;
		break;
	case 0x0a: m_skstat &= ~(0x80 | 0x20 | 0x40); break;
	case 0x0b: if (m_skctl & SK_RESET) potgo(); break;
	case 0x0d: break;
	case 0x0e:
		if (m_irqst & ~data) m_irqst &= (0x08 | data);
		m_irqen = data;
		break;
	case 0x0f:
		if (data == m_skctl) return;
		m_skctl = data;
		if (!(data & SK_RESET))
		{
			m_irqst &= 0x08; m_irqen = 0;
			m_skstat &= ~(0x80 | 0x20 | 0x40);
			m_p9 = m_p17 = m_p4 = m_p5 = 0;
			m_clock_cnt[0] = m_clock_cnt[1] = m_clock_cnt[2] = 0;
		}
		m_old_raw_inval = true;
		break;
	}
}

// ============================================================================ YM2151 (ymfm)

class Ym2151Host : public ymfm::ymfm_interface {
public:
	Ym2151Host() : chip(*this) { chip.reset(); }
	ymfm::ym2151 chip;
	uint64_t now = 0;                       // in YM clocks (2 per sound CPU cycle)
	uint64_t timer_end[2] = {UINT64_MAX, UINT64_MAX};
	uint64_t busy_end = 0;

	void ymfm_set_timer(uint32_t tnum, int32_t duration) override
	{
		timer_end[tnum] = duration < 0 ? UINT64_MAX : now + (uint64_t)duration;
	}
	void ymfm_set_busy_end(uint32_t clocks) override { busy_end = now + clocks; }
	bool ymfm_is_busy() override { return now < busy_end; }

	void advance_to(uint64_t t)
	{
		for (;;)
		{
			const int first = timer_end[0] <= timer_end[1] ? 0 : 1;
			if (timer_end[first] > t) break;
			now = timer_end[first];
			timer_end[first] = UINT64_MAX;
			m_engine->engine_timer_expired(first);
		}
		now = t;
	}
};

// ============================================================================ mixer

SoundChips::SoundChips() : m_ym(std::make_unique<Ym2151Host>()), m_tms(std::make_unique<Tms5220>()) {}
SoundChips::~SoundChips() = default;

void SoundChips::reset()
{
	m_pokey[0].reset();
	m_pokey[1].reset();
	m_ym->chip.reset();
	m_tms->reset();
	mixer_w(0);
}

void SoundChips::produce_sample()
{
	ymfm::ym2151::output_data out;
	m_ym->chip.generate(&out, 1);
	const double tms = m_tms->sample_at_rate(output_rate());
	double l = 0, r = 0;
	// MAME routes: YM2151 L/R 0.60, POKEY1 left 1.35, POKEY2 right 1.35, TMS5220 both 0.75
	l += out.data[0] / 32768.0 * 0.60 * m_gain_ym;
	r += out.data[1] / 32768.0 * 0.60 * m_gain_ym;
	const double pg = (32767.0 / 11 / 4) / 32768.0 / CYCLES_PER_SAMPLE;
	l += std::min(32767.0, m_pokey_acc[0] * (32767.0 / 11 / 4) / CYCLES_PER_SAMPLE) / 32768.0 * 1.35 * m_gain_pokey;
	r += std::min(32767.0, m_pokey_acc[1] * (32767.0 / 11 / 4) / CYCLES_PER_SAMPLE) / 32768.0 * 1.35 * m_gain_pokey;
	(void)pg;
	l += tms * 0.75 * m_gain_tms;
	r += tms * 0.75 * m_gain_tms;
	{
		const double y = (out.data[0] + out.data[1]) / 65536.0 * 0.60 * m_gain_ym;
		const double p0 = m_pokey_acc[0] * (32767.0 / 11 / 4) / CYCLES_PER_SAMPLE / 32768.0 * 1.35 * m_gain_pokey;
		const double p1 = m_pokey_acc[1] * (32767.0 / 11 / 4) / CYCLES_PER_SAMPLE / 32768.0 * 1.35 * m_gain_pokey;
		const double t = tms * 0.75 * m_gain_tms;
		stats.ym += y * y; stats.pokey[0] += p0 * p0; stats.pokey[1] += p1 * p1; stats.tms += t * t;
		stats.samples++;
	}
	m_pokey_acc[0] = m_pokey_acc[1] = 0;
	auto clip = [](double v) { v *= 32767.0; return (int16_t)std::clamp(v, -32768.0, 32767.0); };
	if (m_audio.size() < 2 * 48000)   // cap backlog at ~1s if nobody drains it
	{
		m_audio.push_back(clip(l));
		m_audio.push_back(clip(r));
	}
}

void SoundChips::sync(uint64_t cycle)
{
	while (m_cycle < cycle)
	{
		m_pokey[0].step();
		m_pokey[1].step();
		m_cycle++;
		m_tms->advance(1);
		if (audio_enabled)
		{
			m_pokey_acc[0] += m_pokey[0].output();
			m_pokey_acc[1] += m_pokey[1].output();
		}
		if (++m_sub >= CYCLES_PER_SAMPLE)
		{
			m_sub = 0;
			m_ym->advance_to(m_cycle * 2);
			if (audio_enabled) produce_sample();
			else
			{
				ymfm::ym2151::output_data out;
				m_ym->chip.generate(&out, 1);
			}
		}
	}
	m_ym->advance_to(m_cycle * 2);
}

size_t SoundChips::take_samples(int16_t *dst, size_t max_frames)
{
	const size_t n = std::min(max_frames, m_audio.size() / 2);
	memcpy(dst, m_audio.data(), n * 2 * sizeof(int16_t));
	m_audio.erase(m_audio.begin(), m_audio.begin() + n * 2);
	return n;
}

uint8_t SoundChips::pokey_read(int which, int offset, uint8_t allpot)
{
	if (now) sync(now());
	return m_pokey[which].read(offset, allpot);
}

void SoundChips::pokey_write(int which, int offset, uint8_t data)
{
	if (now) sync(now());
	stats.pokey_writes[which & 1]++;
	m_pokey[which].write(offset, data);
}

uint8_t SoundChips::ym_read(int offset)
{
	if (now) sync(now());
	return m_ym->chip.read(offset);
}

void SoundChips::ym_write(int offset, uint8_t data)
{
	if (now) sync(now());
	stats.ym_writes++;
	m_ym->chip.write(offset, data);
}

bool SoundChips::tms_readyq()
{
	if (now) sync(now());
	return m_tms->readyq();
}

void SoundChips::tms_data_w(uint8_t data)
{
	if (now) sync(now());
	stats.tms_writes++;
	m_tms->data_w(data);
}

void SoundChips::tms_wsq_w(int state)
{
	if (now) sync(now());
	m_tms->wsq_w(state);
}

void SoundChips::mixer_w(uint8_t data)
{
	auto gain3 = [](uint8_t d, double r0, double r1, double r2) {
		const double rtop = 1.0 / (1.0 / 100 + 1.0 / 100);
		double rbott = 0;
		if (!(d & 1)) rbott += 1.0 / r0;
		if (!(d & 2)) rbott += 1.0 / r1;
		if (!(d & 4)) rbott += 1.0 / r2;
		return rbott == 0 ? 1.0 : ((1.0 / rbott) / (rtop + (1.0 / rbott)));
	};
	m_gain_ym = gain3(data & 7, 100, 47, 22);
	{
		const double rtop = 1.0 / (1.0 / 100 + 1.0 / 100);
		double rbott = 0;
		if (!(data & 0x08)) rbott += 1.0 / 47;
		if (!(data & 0x10)) rbott += 1.0 / 22;
		m_gain_pokey = rbott == 0 ? 1.0 : ((1.0 / rbott) / (rtop + (1.0 / rbott)));
	}
	m_gain_tms = gain3((data >> 5) & 7, 100, 47, 22);
}

void SoundChips::switch_w(uint8_t data)
{
	// bit 5 selects the TMS5220 clock divider (MASTER/4/divider/2)
	const int divider = 16 - (12 | ((data >> 5) & 1));
	m_tms->set_clock(20000000.0 / 4 / divider / 2);
}

void SoundChips::sndrst_w(uint8_t data)
{
	const uint8_t state = data & 1;
	if (state == m_sndrst) return;
	m_sndrst = state;
	if (state == 0)
		m_ym->chip.reset();          // YM2151 held in reset while low
	else
	{
		m_tms->reset();
		mixer_w(0);
	}
}
