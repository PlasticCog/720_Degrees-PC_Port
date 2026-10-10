// TMS5220C LPC speech synthesizer.
// Port of the relevant parts of MAME's tms5220.cpp (BSD-3-Clause; Frank Palazzolo,
// Aaron Giles, Jonathan Gevaryahu, Raphael Nabet, Couriersud, Michael Zapf), restricted
// to the configuration used by Atari System 2: TMS5220C, speak-external mode fed through
// the data latch, /RS tied high, "true timing" /WS strobes, no VSM attached.
#include "tms5220.h"
#include <algorithm>
#include <cstring>

namespace {
constexpr unsigned FIFO_SIZE = 16;
constexpr uint8_t NOCOMMAND = 0xff;
const uint8_t reload_table[4] = {0, 2, 4, 6};

const unsigned short energytable[16] = {0, 1, 2, 3, 4, 6, 8, 11, 16, 23, 33, 47, 63, 85, 114, 0};
const unsigned short pitchtable[64] = {
	0, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
	30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 44, 46, 48,
	50, 52, 53, 56, 58, 60, 62, 65, 68, 70, 72, 76, 78, 80, 84, 86,
	91, 94, 98, 101, 105, 109, 114, 118, 122, 127, 132, 137, 142, 148, 153, 159};
const int kbits[10] = {5, 5, 4, 4, 4, 4, 4, 3, 3, 3};
const int ktable[10][32] = {
	{-501, -498, -497, -495, -493, -491, -488, -482, -478, -474, -469, -464, -459, -452, -445, -437,
	 -412, -380, -339, -288, -227, -158, -81, -1, 80, 157, 226, 287, 337, 379, 411, 436},
	{-328, -303, -274, -244, -211, -175, -138, -99, -59, -18, 24, 64, 105, 143, 180, 215,
	 248, 278, 306, 331, 354, 374, 392, 408, 422, 435, 445, 455, 463, 470, 476, 506},
	{-441, -387, -333, -279, -225, -171, -117, -63, -9, 45, 98, 152, 206, 260, 314, 368},
	{-328, -273, -217, -161, -106, -50, 5, 61, 116, 172, 228, 283, 339, 394, 450, 506},
	{-328, -282, -235, -189, -142, -96, -50, -3, 43, 90, 136, 182, 229, 275, 322, 368},
	{-256, -212, -168, -123, -79, -35, 10, 54, 98, 143, 187, 232, 276, 320, 365, 409},
	{-308, -260, -212, -164, -117, -69, -21, 27, 75, 122, 170, 218, 266, 314, 361, 409},
	{-256, -161, -66, 29, 124, 219, 314, 409},
	{-256, -176, -96, -15, 65, 146, 226, 307},
	{-205, -132, -59, 14, 87, 160, 234, 307}};
const int16_t chirptable[52] = {
	0x00, 0x03, 0x0f, 0x28, 0x4c, 0x6c, 0x71, 0x50, 0x25, 0x26, 0x4c, 0x44, 0x1a, 0x32, 0x3b, 0x13,
	0x37, 0x1a, 0x25, 0x1f, 0x1d, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
const int8_t interp_coeff[8] = {0, 3, 3, 3, 2, 2, 1, 1};
}

struct Tms5220::Impl {
	uint8_t fifo[FIFO_SIZE] = {};
	uint8_t fifo_head = 0, fifo_tail = 0, fifo_count = 0, fifo_bits_taken = 0;
	bool previous_talk_status = false, SPEN = false, DDIS = false, TALK = false, TALKD = false;
	bool buffer_low = true, buffer_empty = true, irq_pin = false, ready_pin = false;
	uint8_t new_frame_energy_idx = 0, new_frame_pitch_idx = 0, new_frame_k_idx[10] = {};
	int32_t current_energy = 0, current_pitch = 0, current_k[10] = {};
	int32_t previous_energy = 0;
	uint8_t subcycle = 0, subc_reload = 1, PC = 0, IP = 0;
	bool inhibit = true, uv_zpar = false, zpar = false, pitch_zero = false;
	uint8_t c_variant_rate = 0;
	bool OLDE = true, OLDP = true;
	int32_t pitch_count = 0;
	int32_t u[11] = {}, x[10] = {};
	uint16_t RNG = 0x1fff;
	int16_t excitation_data = 0;
	uint8_t command_register = NOCOMMAND;
	bool data_latched = false, io_ready = true;
	uint8_t rs_ws = 0x03, write_latch = 0;
	int ready_timer = -1;                 // in TMS clocks; -1 = idle

	bool talk_status() const { return SPEN || TALKD; }
	bool new_frame_stop_flag() const { return new_frame_energy_idx == 0x0f; }
	bool new_frame_silence_flag() const { return new_frame_energy_idx == 0; }
	bool new_frame_unvoiced_flag() const { return new_frame_pitch_idx == 0; }

	void reset()
	{
		memset(fifo, 0, sizeof(fifo));
		fifo_head = fifo_tail = fifo_count = fifo_bits_taken = 0;
		SPEN = DDIS = TALK = TALKD = previous_talk_status = irq_pin = ready_pin = false;
		buffer_empty = buffer_low = true;
		command_register = NOCOMMAND;
		data_latched = false;
		new_frame_energy_idx = 0; current_energy = previous_energy = 0;
		new_frame_pitch_idx = 0; current_pitch = 0;
		zpar = uv_zpar = false;
		memset(new_frame_k_idx, 0, sizeof(new_frame_k_idx));
		memset(current_k, 0, sizeof(current_k));
		inhibit = true;
		subcycle = c_variant_rate = 0; pitch_count = 0; PC = 0;
		subc_reload = 1;
		OLDE = OLDP = true;
		IP = reload_table[c_variant_rate & 3];
		RNG = 0x1fff;
		memset(u, 0, sizeof(u));
		memset(x, 0, sizeof(x));
	}

	void update_fifo_status_and_ints()
	{
		if (fifo_count <= 8) buffer_low = true; else buffer_low = false;
		if (fifo_count == 0)
		{
			buffer_empty = true;
			if (DDIS) TALK = SPEN = false;
		}
		else
			buffer_empty = false;
		if (previous_talk_status && !talk_status())
		{
			DDIS = false;
			previous_talk_status = false;
			if (command_register != NOCOMMAND)
			{
				process_command(command_register);
				if (data_latched) ready_timer = 16;
				else io_ready = true;
			}
		}
		previous_talk_status = talk_status();
	}

	int read_bits(int count)
	{
		int val = 0;
		if (DDIS)
		{
			while (count--)
			{
				val = (val << 1) | ((fifo[fifo_head] >> fifo_bits_taken) & 1);
				if (++fifo_bits_taken >= 8)
				{
					fifo_count--;
					fifo[fifo_head] = 0;
					fifo_head = (fifo_head + 1) % FIFO_SIZE;
					fifo_bits_taken = 0;
					update_fifo_status_and_ints();
				}
			}
		}
		else
			val = (1 << count) - 1;    // no VSM attached: reads float high
		return val;
	}

	void zero_new_frame()
	{
		zpar = true; uv_zpar = true; OLDE = true; OLDP = true;
		new_frame_energy_idx = 0; new_frame_pitch_idx = 0;
		for (int i = 0; i < 4; i++) new_frame_k_idx[i] = 0;
		for (int i = 4; i < 7; i++) new_frame_k_idx[i] = 0xf;
		for (int i = 7; i < 10; i++) new_frame_k_idx[i] = 0x7;
	}

	void process_command(uint8_t cmd)
	{
		command_register = cmd;
		switch (cmd & 0x70)
		{
		case 0x10: if (!talk_status()) { read_bits(8); command_register = NOCOMMAND; } break;
		case 0x00: case 0x20: c_variant_rate = cmd & 0x0f; command_register = NOCOMMAND; break;
		case 0x30: if (!talk_status()) command_register = NOCOMMAND; break;
		case 0x40: if (!talk_status()) command_register = NOCOMMAND; break;
		case 0x50:
			SPEN = true; TALK = true; DDIS = false;
			zero_new_frame();
			command_register = NOCOMMAND;
			break;
		case 0x60:
			memset(fifo, 0, sizeof(fifo));
			fifo_head = fifo_tail = fifo_count = fifo_bits_taken = 0;
			DDIS = true;
			zero_new_frame();
			command_register = NOCOMMAND;
			break;
		case 0x70:
			reset();
			command_register = NOCOMMAND;
			break;
		}
		update_fifo_status_and_ints();
	}

	void data_write(uint8_t data)
	{
		const bool old_buffer_low = buffer_low;
		if (DDIS)
		{
			if (fifo_count < FIFO_SIZE)
			{
				fifo[fifo_tail] = data;
				fifo_tail = (fifo_tail + 1) % FIFO_SIZE;
				fifo_count++;
				update_fifo_status_and_ints();
				if (!SPEN && old_buffer_low && !buffer_low)
				{
					zero_new_frame();
					SPEN = true;
					TALK = true;
				}
			}
			data_latched = false;
		}
		else
			process_command(data);
		if (!data_latched) io_ready = true;
	}

	void set_io_ready()
	{
		switch (rs_ws)
		{
		case 0x02:
			if (fifo_count >= FIFO_SIZE && DDIS) { ready_timer = 16; return; }
			if (command_register == NOCOMMAND)
			{
				data_latched = false;
				data_write(write_latch);
			}
			break;
		default:
			io_ready = true;
			break;
		}
	}

	void parse_frame()
	{
		uv_zpar = zpar = false;
		if (c_variant_rate & 0x04) IP = reload_table[read_bits(2)];
		else IP = reload_table[c_variant_rate & 3];
		update_fifo_status_and_ints();
		if (DDIS && buffer_empty) return;
		new_frame_energy_idx = (uint8_t)read_bits(4);
		update_fifo_status_and_ints();
		if (DDIS && buffer_empty) return;
		if (new_frame_energy_idx == 0 || new_frame_energy_idx == 15) return;
		const int rep_flag = read_bits(1);
		new_frame_pitch_idx = (uint8_t)read_bits(6);
		uv_zpar = new_frame_unvoiced_flag();
		update_fifo_status_and_ints();
		if (DDIS && buffer_empty) return;
		if (rep_flag) return;
		for (int i = 0; i < 4; i++)
		{
			new_frame_k_idx[i] = (uint8_t)read_bits(kbits[i]);
			update_fifo_status_and_ints();
			if (DDIS && buffer_empty) return;
		}
		if (new_frame_pitch_idx == 0) return;
		for (int i = 4; i < 10; i++)
		{
			new_frame_k_idx[i] = (uint8_t)read_bits(kbits[i]);
			update_fifo_status_and_ints();
			if (DDIS && buffer_empty) return;
		}
	}

	static int32_t matrix_multiply(int32_t a, int32_t b)
	{
		while (a > 511) a -= 1024;
		while (a < -512) a += 1024;
		while (b > 16383) b -= 32768;
		while (b < -16384) b += 32768;
		return (a * b) >> 9;
	}

	int32_t lattice_filter()
	{
		u[10] = matrix_multiply(previous_energy, excitation_data << 6);
		for (int i = 9; i >= 0; i--)
			u[i] = u[i + 1] - matrix_multiply(current_k[i], x[i]);
		for (int i = 9; i >= 1; i--)
			x[i] = x[i - 1] + matrix_multiply(current_k[i - 1], u[i - 1]);
		x[0] = u[0];
		previous_energy = current_energy;
		return u[0];
	}

	static int16_t clip_analog(int16_t c)
	{
		if (c > 2047) c = 2047;
		else if (c < -2048) c = -2048;
		c &= ~0xf;
		return (int16_t)((c << 4) | ((c & 0x7f0) >> 3) | ((c & 0x400) >> 10));
	}

	int16_t process_one()
	{
		int16_t out;
		if (TALKD)
		{
			if (IP == 0 && PC == 12 && subcycle == 1)
			{
				IP = reload_table[c_variant_rate & 3];
				parse_frame();
				if (new_frame_stop_flag())
				{
					TALK = SPEN = false;
					update_fifo_status_and_ints();
				}
				if ((!OLDP && new_frame_unvoiced_flag()) || (OLDP && !new_frame_unvoiced_flag()) ||
					(OLDE && !new_frame_silence_flag()) || (OLDP && new_frame_silence_flag()))
					inhibit = true;
				else
					inhibit = false;
			}
			else
			{
				const bool inhibit_state = inhibit && IP != 0;
				if (subcycle == 2)
				{
					const int sh = interp_coeff[IP];
					switch (PC)
					{
					case 0:
						if (IP == 0) pitch_zero = false;
						current_energy = (current_energy + (((energytable[new_frame_energy_idx] - current_energy) * (1 - inhibit_state)) >> sh)) * (1 - zpar);
						break;
					case 1:
						current_pitch = (current_pitch + (((pitchtable[new_frame_pitch_idx] - current_pitch) * (1 - inhibit_state)) >> sh)) * (1 - zpar);
						break;
					case 12:
						break;
					default:
					{
						const int k = PC - 2;
						current_k[k] = (current_k[k] + (((ktable[k][new_frame_k_idx[k]] - current_k[k]) * (1 - inhibit_state)) >> sh)) * (1 - ((k < 4) ? zpar : uv_zpar));
						break;
					}
					}
				}
			}

			if (OLDP)
				excitation_data = (RNG & 1) ? ~0x3f : 0x40;
			else
				excitation_data = (int8_t)chirptable[pitch_count >= 51 ? 51 : pitch_count];
			for (int i = 0; i < 20; i++)
			{
				const int bitout = ((RNG >> 12) & 1) ^ ((RNG >> 3) & 1) ^ ((RNG >> 2) & 1) ^ (RNG & 1);
				RNG = (uint16_t)((RNG << 1) | bitout);
			}
			int32_t s = lattice_filter();
			while (s > 16383) s -= 32768;
			while (s < -16384) s += 32768;
			out = clip_analog((int16_t)s);

			subcycle++;
			if (subcycle == 2 && PC == 12)
			{
				if (IP == 7 && inhibit) pitch_zero = true;
				if (IP == 7)
				{
					OLDE = new_frame_silence_flag();
					OLDP = new_frame_unvoiced_flag();
					TALKD = TALK;
					update_fifo_status_and_ints();
					if (!TALK && SPEN) TALK = true;
				}
				subcycle = subc_reload;
				PC = 0;
				IP = (IP + 1) & 7;
			}
			else if (subcycle == 3)
			{
				subcycle = subc_reload;
				PC++;
			}
			pitch_count++;
			if (pitch_count >= current_pitch || pitch_zero) pitch_count = 0;
			pitch_count &= 0x1ff;
		}
		else
		{
			subcycle++;
			if (subcycle == 2 && PC == 12)
			{
				if (IP == 7)
				{
					TALKD = TALK;
					update_fifo_status_and_ints();
					if (!TALK && SPEN) TALK = true;
				}
				subcycle = subc_reload;
				PC = 0;
				IP = (IP + 1) & 7;
			}
			else if (subcycle == 3)
			{
				subcycle = subc_reload;
				PC++;
			}
			out = -1;
		}
		return out;
	}

	// time keeping
	double clock_pos = 0;          // fractional TMS clocks pending
	double sample_pos = 0;         // fractional TMS samples pending (clock/80)
	int16_t last_sample = 0;
	std::vector<int16_t> pending;
};

Tms5220::Tms5220() : m(new Impl) { reset(); }
Tms5220::~Tms5220() { delete m; }

void Tms5220::reset()
{
	m->reset();
	m->io_ready = true;
	m->rs_ws = 0x02 | (m_wsq & 1);
}

void Tms5220::wsq_w(int state)
{
	state &= 1;
	m_wsq = state;
	const uint8_t new_val = 0x02 | state;      // /RS tied high on System 2
	if (new_val == m->rs_ws) return;
	m->rs_ws = new_val;
	if (new_val == 3) return;                  // high impedance
	// high to low: /READY goes inactive, write serviced after 16 clocks
	m->io_ready = false;
	m->ready_timer = 16;
}

void Tms5220::write_data(uint8_t data) { m->data_write(data); }

bool Tms5220::readyq() const
{
	return !m->io_ready;
}

void Tms5220::advance(int sound_cycles)
{
	// sound CPU clock -> TMS clock
	const double tms_clocks = sound_cycles * (m_clock / (14318181.0 / 8.0));
	m->clock_pos += tms_clocks;
	while (m->clock_pos >= 1.0)
	{
		m->clock_pos -= 1.0;
		if (m->ready_timer > 0 && --m->ready_timer == 0)
		{
			m->ready_timer = -1;
			if (m->rs_ws == 0x02) m->write_latch = m_latch;
			m->set_io_ready();
		}
		m->sample_pos += 1.0 / 80.0;
		if (m->sample_pos >= 1.0)
		{
			m->sample_pos -= 1.0;
			m->last_sample = m->process_one();
		}
	}
}

double Tms5220::sample_at_rate(double)
{
	return m->last_sample / 32768.0;
}

void Tms5220::data_w(uint8_t data)
{
	m_latch = data;
	m->write_latch = data;
	m->data_latched = true;
}
