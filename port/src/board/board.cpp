// Atari System 2 board support for 720 Degrees (see board.h).
#include "board.h"
#include "soundchips.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>

// ------------------------------------------------------------------ ROM loading

static bool read_file(const std::string &path, std::vector<uint8_t> &out, size_t expect, std::string &err)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) { err = "missing ROM file: " + path; return false; }
	out.assign(std::istreambuf_iterator<char>(f), {});
	if (expect && out.size() != expect) { err = "unexpected size: " + path; return false; }
	return true;
}

bool RomSet::load(const std::string &dir, std::string &err)
{
	auto p = [&](const char *n) { return dir + "/" + n; };
	std::vector<uint8_t> d;
	main.assign(0x90000, 0);
	auto load16 = [&](const char *n, size_t off, size_t size) {
		if (!read_file(p(n), d, size, err)) return false;
		for (size_t i = 0; i < d.size(); i++) main[off + i * 2] = d[i];
		return true;
	};
	if (!load16("136047-3126.7lm", 0x008000, 0x4000) || !load16("136047-3127.7mn", 0x008001, 0x4000) ||
		!load16("136047-3128.6fh", 0x010000, 0x10000) || !load16("136047-4131.6mn", 0x010001, 0x10000) ||
		!load16("136047-1129.6hj", 0x030000, 0x10000) || !load16("136047-1132.6p", 0x030001, 0x10000) ||
		!load16("136047-1130.6k", 0x050000, 0x10000) || !load16("136047-1133.6r", 0x050001, 0x10000))
		return false;

	sound.assign(0x10000, 0);
	const char *snd[3] = { "136047-2134.2a", "136047-1135.2b", "136047-2136.2cd" };
	for (int i = 0; i < 3; i++)
	{
		if (!read_file(p(snd[i]), d, 0x4000, err)) return false;
		memcpy(&sound[0x4000 + i * 0x4000], d.data(), 0x4000);
	}

	tiles.assign(0x40000, 0);
	const char *tl[8] = { "136047-1121.6a", "136047-1122.6b", "136047-1123.7a", "136047-1124.7b",
		"136047-1117.6c", "136047-1118.6d", "136047-1119.7d", "136047-1120.7c" };
	for (int i = 0; i < 8; i++)
	{
		if (!read_file(p(tl[i]), d, 0x8000, err)) return false;
		memcpy(&tiles[i * 0x8000], d.data(), 0x8000);
	}

	sprites.assign(0x100000, 0);
	struct { const char *n; size_t a, b; } sp[16] = {
		{"136047-1109.6t", 0x20000, 0x00000}, {"136047-1110.6sr", 0x28000, 0x08000},
		{"136047-1111.6p", 0x30000, 0x10000}, {"136047-1112.6n", 0x38000, 0x18000},
		{"136047-1113.6m", 0x60000, 0x40000}, {"136047-1114.6l", 0x68000, 0x48000},
		{"136047-1115.6kj", 0x70000, 0x50000}, {"136047-1116.6jh", 0x78000, 0x58000},
		{"136047-1101.5t", 0xa0000, 0x80000}, {"136047-1102.5sr", 0xa8000, 0x88000},
		{"136047-1103.5p", 0xb0000, 0x90000}, {"136047-1104.5n", 0xb8000, 0x98000},
		{"136047-1105.5m", 0xe0000, 0xc0000}, {"136047-1106.5l", 0xe8000, 0xc8000},
		{"136047-1107.5kj", 0xf0000, 0xd0000}, {"136047-1108.5jh", 0xf8000, 0xd8000} };
	for (auto &e : sp)
	{
		if (!read_file(p(e.n), d, 0x10000, err)) return false;
		memcpy(&sprites[e.a], d.data(), 0x8000);
		memcpy(&sprites[e.b], d.data() + 0x8000, 0x8000);
	}
	for (auto &b : sprites) b ^= 0xff;

	if (!read_file(p("136047-1125.4t"), chars, 0x4000, err)) return false;
	if (!read_file(p("720-eeprom.bin"), eeprom, 0x200, err)) return false;
	return true;
}

// ------------------------------------------------------------------ graphics decode

static inline int getbit(const std::vector<uint8_t> &r, size_t bitoff)
{
	return (r[bitoff >> 3] >> (7 - (bitoff & 7))) & 1;
}

void GfxSet::decode(const RomSet &roms)
{
	{   // playfield: 8x8, planes {0,4,half,half+4}, x {0-3,8-11}, 16 bits per row
		const size_t half = roms.tiles.size() / 2 * 8;
		const int xo[8] = {0, 1, 2, 3, 8, 9, 10, 11};
		const size_t po[4] = {0, 4, half, half + 4};
		tiles.assign(8192 * 64, 0);
		for (int t = 0; t < 8192; t++)
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 8; x++)
				{
					int v = 0;
					for (int pl = 0; pl < 4; pl++)
						v = (v << 1) | getbit(roms.tiles, (size_t)t * 128 + po[pl] + y * 16 + xo[x]);
					tiles[t * 64 + y * 8 + x] = (uint8_t)v;
				}
	}
	{   // motion objects: 16x16, planes {0,4,half,half+4}, x {0-3,8-11,16-19,24-27}, 32 bits per row
		const size_t half = roms.sprites.size() / 2 * 8;
		int xo[16];
		for (int i = 0; i < 16; i++) xo[i] = (i / 4) * 8 + (i % 4);
		const size_t po[4] = {0, 4, half, half + 4};
		sprites.assign(8192 * 256, 0);
		for (int t = 0; t < 8192; t++)
			for (int y = 0; y < 16; y++)
				for (int x = 0; x < 16; x++)
				{
					int v = 0;
					for (int pl = 0; pl < 4; pl++)
						v = (v << 1) | getbit(roms.sprites, (size_t)t * 512 + po[pl] + y * 32 + xo[x]);
					sprites[t * 256 + y * 16 + x] = (uint8_t)v;
				}
	}
	{   // alphanumerics: 8x8, 2 planes {0,4}
		const int xo[8] = {0, 1, 2, 3, 8, 9, 10, 11};
		chars.assign(1024 * 64, 0);
		for (int t = 0; t < 1024; t++)
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 8; x++)
					chars[t * 64 + y * 8 + x] = (uint8_t)((getbit(roms.chars, (size_t)t * 128 + y * 16 + xo[x]) << 1) |
						getbit(roms.chars, (size_t)t * 128 + 4 + y * 16 + xo[x]));
	}
}

// ------------------------------------------------------------------ board

Board::Board(const RomSet &roms, CpuLink &link) : m_roms(roms), m_link(link)
{
	m_gfx.decode(roms);
	m_chips = std::make_unique<SoundChips>();
	m_frame.assign(SCREEN_W * SCREEN_H, 0);
	m_mobuf.assign(SCREEN_W, 0xffff);
}

Board::~Board() = default;

static int bank_entry_from_data(uint16_t data)
{
	int n = ((data >> 10) & 077) ^ 3;
	auto b = [&](int i) { return (n >> i) & 1; };
	const int e = (b(5) << 5) | (b(4) << 4) | (b(1) << 3) | (b(0) << 2) | (b(3) << 1) | b(2);
	// Native extension for mods: the original program only ever writes bank values with the
	// low 10 bits clear; nonzero low bits select an extension group of 64 extra ROM pages
	// appended after the original 0x90000-byte region (see port/src/level/leveldata.h).
	return e + 64 * (data & 0x3ff);
}

void Board::reset()
{
	m_ram.fill(0);
	m_sram.fill(0);
	memcpy(m_eeprom.data(), m_roms.eeprom.data(), 0x200);
	m_interrupt_enable = 0;
	m_pending_int_enable = -1;
	m_scanline_int = m_video_int = m_p2portwr = m_p2portrd = false;
	m_soundlatch_pending = m_mainlatch_pending = false;
	m_bank_entry[0] = m_bank_entry[1] = 0;
	m_slap = Slapstic{};
	m_slap_armed = false;
	m_sound_reset = true;
	m_link.sound_set_reset(true);
	m_chips->reset();
	update_interrupts();
	scanline = 0;
	m_last_rendered = -1;
	frame_number = 0;
}

void Board::update_interrupts()
{
	m_link.main_set_irq_lines((m_video_int ? 8 : 0) | (m_scanline_int ? 4 : 0) | (m_p2portwr ? 2 : 0) | (m_p2portrd ? 1 : 0));
}

void Board::apply_pending()
{
	if (m_pending_int_enable >= 0)
	{
		m_interrupt_enable = (uint8_t)m_pending_int_enable;
		m_pending_int_enable = -1;
	}
}

// ---- slapstic 137412-107 (MAME slapstic.cpp, 103-110 state machine)
void Board::slapstic_tap(uint32_t a)
{
	const int before = m_slap.state;
	auto in = [&](uint32_t mask, uint32_t value) { return (a & (0xfffffe00u | (mask << 1))) == (0x8000u | (value << 1)); };
	auto any = [&](uint32_t mask, uint32_t value) { return (a & (mask << 1)) == (value << 1); };
	const bool reset = (a & 0xfffffffeu) == 0x8000u;
	int &state = m_slap.state;
	switch (state)
	{
	case 0:
		if (reset) state = 1;
		break;
	case 1:
		if ((a & 0xfffffffeu) == 0x8030u) { m_slap.current = 0; state = 0; }
		else if ((a & 0xfffffffeu) == 0x8034u) { m_slap.current = 1; state = 0; }
		else if ((a & 0xfffffffeu) == 0x8038u) { m_slap.current = 2; state = 0; }
		else if ((a & 0xfffffffeu) == 0x803cu) { m_slap.current = 3; state = 0; }
		else if (any(0x007f, 0x006b)) state = 2;
		else if (in(0x3ff0, 0x00a0)) state = 5;
		break;
	case 2:
		if (reset) state = 1;
		else if (in(0x3fff, 0x3d52)) state = 3;
		else state = 1;
		break;
	case 3:
		if (reset) state = 1;
		else if (in(0x3ffc, 0x3d64)) { m_slap.loaded = (a >> 1) & 3; state = 4; }
		else state = 1;
		break;
	case 4:
		if (reset) state = 1;
		else if (in(0x3ff9, 0x0018)) { m_slap.current = m_slap.loaded; state = 0; }
		break;
	case 5:
		if (reset) state = 1;
		else if (in(0x3ff9, 0x0018)) { m_slap.loaded = m_slap.current; state = 6; }
		break;
	case 6: case 7:
	{
		const bool odd = state == 6;
		if (reset) { state = 1; break; }
		const uint32_t c0 = odd ? 0x00a0 : 0x00a3, s0 = odd ? 0x00a1 : 0x00a2;
		const uint32_t c1 = odd ? 0x00a2 : 0x00a1, s1 = odd ? 0x00a3 : 0x00a0;
		if (in(0x3ff3, c0)) { m_slap.loaded &= ~1; state = odd ? 7 : 6; }
		else if (in(0x3ff3, s0)) { m_slap.loaded |= 1; state = odd ? 7 : 6; }
		else if (in(0x3ff3, c1)) { m_slap.loaded &= ~2; state = odd ? 7 : 6; }
		else if (in(0x3ff3, s1)) { m_slap.loaded |= 2; state = odd ? 7 : 6; }
		else if (in(0x3ff8, 0x00b0)) { m_slap.current = m_slap.loaded; state = 0; }
		break;
	}
	}
	if (on_slapstic && before != state) on_slapstic(before, state, a, m_slap.current);
}

// ---- main CPU memory map
uint16_t Board::main_read16(uint16_t a, uint16_t mask)
{
	if (a < 0x1000) return m_ram[a] | (m_ram[a + 1] << 8);
	if (a < 0x1400) return m_palette[(a >> 1) & 0xff];
	if (a < 0x1800) return 0xffff;          // ADC (unused by 720) and write-only registers
	if (a < 0x1c00)
	{
		return 0x7f0f | (m_mainlatch_pending ? 0x10 : 0) | (m_soundlatch_pending ? 0x20 : 0) |
			(input.button2 ? 0 : 0x40) | (input.button1 ? 0 : 0x80) | (input.test ? 0 : 0x8000);
	}
	if (a < 0x2000)
	{
		// response from the sound CPU; reading clears its interrupt and the latch
		m_p2portwr = false;
		update_interrupts();
		m_mainlatch_pending = false;
		return 0xff00 | m_mainlatch;
	}
	if (a < 0x4000)
	{
		const int off = (a - 0x2000) >> 1;
		switch (m_slap.current)
		{
		case 0: return a < 0x3800 ? m_alpha[off] : m_mo[off - 0xc00];
		case 2: return m_pft[off];
		case 3: return m_pfb[off];
		default: return 0xffff;
		}
	}
	if (a < 0x8000)
	{
		const size_t o = banked_rom_offset((a >> 13) & 1, a);
		if (o + 1 >= m_roms.main.size()) return 0;
		return m_roms.main[o] | (m_roms.main[o + 1] << 8);
	}
	return m_roms.main[a] | (m_roms.main[a + 1] << 8);
}

void Board::main_write16(uint16_t a, uint16_t data, uint16_t mask)
{
	auto combine = [&](uint16_t &d) { d = (d & ~mask) | (data & mask); };
	const bool lo = (mask & 0x00ff) != 0;
	if (a < 0x1000)
	{
		if (mask & 0x00ff) m_ram[a] = data & 0xff;
		if (mask & 0xff00) m_ram[a + 1] = data >> 8;
		return;
	}
	if (a < 0x1400) { combine(m_palette[(a >> 1) & 0xff]); return; }
	if (a < 0x1480) { m_bank_entry[(a >> 1) & 1] = bank_entry_from_data(data); return; }
	if (a < 0x1580) return;     // ADC start / unmapped
	if (a < 0x15a0) { if (lo) { m_p2portrd = false; update_interrupts(); } return; }
	if (a < 0x15c0)
	{
		if (lo)
		{
			const bool rst = data & 1;
			if (rst != m_sound_reset)
			{
				m_sound_reset = rst;
				m_link.sound_set_reset(rst);
				if (rst) m_chips->reset();
			}
		}
		return;
	}
	if (a < 0x15e0) { if (lo) { m_scanline_int = false; update_interrupts(); } return; }
	if (a < 0x1600) { if (lo) { m_video_int = false; update_interrupts(); } return; }
	if (a < 0x1680) { if (lo) { m_pending_int_enable = data & 0xff; m_link.main_end_timeslice(); } return; }
	if (a < 0x1700)
	{
		if (lo)
		{
			m_soundlatch = data & 0xff;
			m_soundlatch_pending = true;
			m_link.sound_set_nmi(true);
			if (on_sound_command) on_sound_command(m_soundlatch);
			m_link.main_end_timeslice();
		}
		return;
	}
	if (a < 0x1780)
	{
		const uint16_t n = (m_xscroll & ~mask) | (data & mask);
		if (n != m_xscroll) update_partial(scanline);
		m_pf_scrollx = n >> 6;
		m_pf_tile_bank[0] = n & 0x0f;
		m_xscroll = n;
		return;
	}
	if (a < 0x1800)
	{
		const uint16_t n = (m_yscroll & ~mask) | (data & mask);
		if (n != m_yscroll) update_partial(scanline);
		if (!(n & 0x10)) m_pf_scrolly = (n >> 6) - scanline;
		else m_pending_yscroll = n >> 6;
		m_pf_tile_bank[1] = n & 0x0f;
		m_yscroll = n;
		return;
	}
	if (a < 0x2000) return;     // watchdog
	if (a < 0x4000)
	{
		const int off = (a - 0x2000) >> 1;
		switch (m_slap.current)
		{
		case 0:
			if (a < 0x3800) combine(m_alpha[off]);
			else
			{
				if (off - 0xc00 == 3) update_partial(scanline);
				combine(m_mo[off - 0xc00]);
			}
			break;
		case 2: combine(m_pft[off]); break;
		case 3: combine(m_pfb[off]); break;
		default: break;
		}
	}
}

// ---- sound CPU memory map
uint8_t Board::leta_r(int offset)
{
	if (offset == 0) return input.leta_center;
	if (offset == 1) return input.leta_rotate;
	return 0xff;
}

uint8_t Board::sound_read(uint16_t a)
{
	if (a >= 0x4000) return m_roms.sound[a];
	if ((a & 0xdfff) < 0x1000) return m_sram[a & 0xfff];
	const uint16_t c = a & 0xd9ff;
	if (c >= 0x1000 && c < 0x1200) return m_eeprom[a & 0x1ff];
	if ((a & 0xd87f) >= 0x1800 && (a & 0xd87f) < 0x1880)
	{
		const int o = a & 0x7f;
		if (o < 0x10) return m_chips->pokey_read(0, o & 0xf, input.dsw0);
		if (o < 0x20) return leta_r(o & 3);
		if (o < 0x30) return 0xff;
		if (o < 0x40) return m_chips->pokey_read(1, o & 0xf, input.dsw1);
		if (o < 0x50)
			return (m_soundlatch_pending ? 1 : 0) | (m_mainlatch_pending ? 2 : 0) | (m_chips->tms_readyq() ? 4 : 0) |
				(input.test ? 0 : 0x10) | (input.service_coin ? 0 : 0x20) | (input.coin1 ? 0 : 0x40) | (input.coin2 ? 0 : 0x80);
		if (o < 0x60) return m_chips->ym_read(o & 1);
		if (o < 0x70)
		{
			m_p2portrd = (m_interrupt_enable & 1) != 0;
			update_interrupts();
			m_soundlatch_pending = false;
			m_link.sound_set_nmi(false);
			return m_soundlatch;
		}
		return 0xff;
	}
	return 0xff;
}

void Board::sound_write(uint16_t a, uint8_t data)
{
	if (a >= 0x4000) return;
	if ((a & 0xdfff) < 0x1000) { m_sram[a & 0xfff] = data; return; }
	const uint16_t c = a & 0xd9ff;
	if (c >= 0x1000 && c < 0x1200) { m_eeprom[a & 0x1ff] = data; return; }
	if ((a & 0xd87f) >= 0x1800 && (a & 0xd87f) < 0x1880)
	{
		const int o = a & 0x7f;
		if (o < 0x10) { m_chips->pokey_write(0, o & 0xf, data); return; }
		if (o >= 0x30 && o < 0x40) { m_chips->pokey_write(1, o & 0xf, data); return; }
		if (o >= 0x50 && o < 0x60) { m_chips->ym_write(o & 1, data); return; }
		if (o >= 0x70)
		{
			switch (o & 0x0e)
			{
			case 0x00: m_chips->tms_data_w(data); break;
			case 0x02: m_chips->tms_wsq_w(1 - (o & 1)); break;
			case 0x04:
				m_p2portwr = (m_interrupt_enable & 2) != 0;
				update_interrupts();
				m_mainlatch = data;
				m_mainlatch_pending = true;
				break;
			case 0x06: break;                                    // coin counters
			case 0x08: m_link.sound_set_irq(false); break;       // IRQ acknowledge
			case 0x0a: m_chips->mixer_w(data); break;
			case 0x0c: m_chips->switch_w(data); break;
			case 0x0e: m_chips->sndrst_w(data); break;
			}
		}
	}
}

void Board::sound_periodic_irq()
{
	m_link.sound_set_irq(true);
}

// ------------------------------------------------------------------ timing / video

void Board::begin_line(int line)
{
	scanline = line;
	if (line == 0)
	{
		m_last_rendered = -1;
		if (m_pending_yscroll >= 0) { m_pf_scrolly = m_pending_yscroll; m_pending_yscroll = -1; }
	}
	if ((line % 64) == 0)
	{
		if (line > 0 && line < SCREEN_H) update_partial(line - 1);   // atarimo periodic force update
		m_scanline_int = (m_interrupt_enable & 4) != 0;
		update_interrupts();
	}
	if (line == SCREEN_H)
	{
		update_partial(SCREEN_H - 1);
		m_video_int = (m_interrupt_enable & 8) != 0;
		update_interrupts();
		frame_number++;
	}
}

uint32_t Board::palette_rgb(int index) const
{
	static const int ZB = 115, Z3 = 78, Z2 = 37, Z1 = 17, Z0 = 9;
	static const int intensity_table[16] = {
		0, ZB + Z0, ZB + Z1, ZB + Z1 + Z0, ZB + Z2, ZB + Z2 + Z0, ZB + Z2 + Z1, ZB + Z2 + Z1 + Z0,
		ZB + Z3, ZB + Z3 + Z0, ZB + Z3 + Z1, ZB + Z3 + Z1 + Z0, ZB + Z3 + Z2, ZB + Z3 + Z2 + Z0, ZB + Z3 + Z2 + Z1, ZB + Z3 + Z2 + Z1 + Z0 };
	static const int color_table[16] = { 0x0, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xe, 0xf, 0xf };
	const uint16_t raw = m_palette[index & 0xff];
	const int i = intensity_table[raw & 15];
	const int r = (color_table[(raw >> 12) & 15] * i) >> 4;
	const int g = (color_table[(raw >> 8) & 15] * i) >> 4;
	const int b = (color_table[(raw >> 4) & 15] * i) >> 4;
	return 0xff000000u | (r << 16) | (g << 8) | b;
}

void Board::frame_rgb(uint32_t *out) const
{
	uint32_t pal[256];
	for (int i = 0; i < 256; i++) pal[i] = palette_rgb(i);
	for (int i = 0; i < SCREEN_W * SCREEN_H; i++) out[i] = pal[m_frame[i] & 0xff];
}

void Board::update_partial(int line)
{
	if (line >= SCREEN_H) line = SCREEN_H - 1;
	while (m_last_rendered < line)
		render_line(++m_last_rendered);
}

void Board::render_line(int y)
{
	uint16_t *dst = &m_frame[y * SCREEN_W];
	uint8_t pri[SCREEN_W];

	// playfield: 128x64 tiles, 1024x512 pixels, wraps
	const int ty = (y + m_pf_scrolly) & 511;
	for (int x = 0; x < SCREEN_W; x++)
	{
		const int tx = (x + m_pf_scrollx) & 1023;
		const int index = (ty >> 3) * 128 + (tx >> 3);
		const uint16_t data = index < 4096 ? m_pft[index] : m_pfb[index & 4095];
		const int code = ((m_pf_tile_bank[(data >> 10) & 1] << 10) | (data & 0x3ff)) & 8191;
		const int color = (data >> 11) & 7;
		dst[x] = (uint16_t)(128 + color * 16 + m_gfx.tiles[code * 64 + (ty & 7) * 8 + (tx & 7)]);
		pri[x] = (~data >> 14) & 3;
		if (debug.capture_tiles) m_dbg_tiles[y * SCREEN_W + x] = (uint16_t)code;
	}
	if (debug.hide_pf)
		for (int x = 0; x < SCREEN_W; x++) dst[x] = 0;

	// motion objects: linked list from entry 0, drawn in list order (MAME atarimo semantics)
	std::fill(m_mobuf.begin(), m_mobuf.end(), 0xffff);
	{
		uint8_t visited[256] = {0};
		int link = 0;
		for (int i = 0; i < 1024 && !visited[link]; i++)
		{
			const uint16_t *e = &m_mo[link * 4];
			visited[link] = 1;
			const int nlink = (e[3] >> 3) & 0xff;
			int code = (e[1] & 0x7ff) | ((e[0] & 7) << 11);
			const int color = ((e[3] >> 12) & 3) * 16 | (((e[3] >> 14) & 3) << 12);
			int xpos = (e[2] >> 6) & 0x3ff;
			int ypos = -((e[0] >> 6) & 0x1ff);
			const int hflip = (e[1] >> 14) & 1;
			const int height = ((e[1] >> 11) & 7) + 1;
			ypos -= height << 4;
			if (e[1] & 0x8000) xpos = m_mo_last_xpos + 16;
			m_mo_last_xpos = xpos;
			xpos &= 1023;
			ypos &= 511;
			if (xpos >= 640) xpos -= 1024;
			if (ypos >= VTOTAL) ypos -= 512;
			for (int t = 0, sy = ypos; t < height; t++, sy += 16, code++)
			{
				if (y < sy || y >= sy + 16) continue;
				if (xpos <= -16 || xpos > SCREEN_W - 1) continue;
				const uint8_t *src = &m_gfx.sprites[(code & 8191) * 256 + (y - sy) * 16];
				for (int px = 0; px < 16; px++)
				{
					const int sx = xpos + px;
					if (sx < 0 || sx >= SCREEN_W) continue;
					const int pen = src[hflip ? 15 - px : px];
					if (pen != 15) m_mobuf[sx] = (uint16_t)(color + pen);
				}
			}
			link = nlink;
		}
	}
	for (int x = 0; x < SCREEN_W && !debug.hide_mo; x++)
	{
		const uint16_t mo = m_mobuf[x];
		if (mo == 0xffff) continue;
		if (((mo >> 12) + pri[x]) & 2)
		{
			if (!(dst[x] & 0x08)) dst[x] = mo & 0x0fff;
		}
		else
			dst[x] = mo & 0x0fff;
	}

	// alphanumerics on top, pen 0 transparent
	for (int x = 0; x < SCREEN_W && !debug.hide_alpha; x++)
	{
		const uint16_t data = m_alpha[(y >> 3) * 64 + (x >> 3)];
		const int pen = m_gfx.chars[(data & 0x3ff) * 64 + (y & 7) * 8 + (x & 7)];
		if (pen) dst[x] = (uint16_t)(64 + ((data >> 13) & 7) * 4 + pen);
	}
}

// ------------------------------------------------------------------ PNG writer (stored deflate)

static uint32_t crc_table[256];
static void crc_init()
{
	for (uint32_t n = 0; n < 256; n++)
	{
		uint32_t c = n;
		for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
}
static uint32_t crc32_buf(const uint8_t *buf, size_t len, uint32_t c = 0xffffffffu)
{
	for (size_t i = 0; i < len; i++) c = crc_table[(c ^ buf[i]) & 0xff] ^ (c >> 8);
	return c;
}

bool write_png(const std::string &path, const uint32_t *argb, int w, int h)
{
	crc_init();
	std::vector<uint8_t> raw;
	raw.reserve((size_t)(w * 3 + 1) * h);
	for (int y = 0; y < h; y++)
	{
		raw.push_back(0);
		for (int x = 0; x < w; x++)
		{
			const uint32_t p = argb[y * w + x];
			raw.push_back((p >> 16) & 0xff);
			raw.push_back((p >> 8) & 0xff);
			raw.push_back(p & 0xff);
		}
	}
	std::vector<uint8_t> z = {0x78, 0x01};
	uint32_t a = 1, b = 0;
	for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
	for (size_t pos = 0; pos < raw.size();)
	{
		const size_t n = std::min<size_t>(65535, raw.size() - pos);
		z.push_back(pos + n == raw.size() ? 1 : 0);
		z.push_back(n & 0xff); z.push_back((n >> 8) & 0xff);
		z.push_back(~n & 0xff); z.push_back((~n >> 8) & 0xff);
		z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
		pos += n;
	}
	const uint32_t ad = (b << 16) | a;
	z.push_back(ad >> 24); z.push_back(ad >> 16); z.push_back(ad >> 8); z.push_back(ad);

	std::ofstream f(path, std::ios::binary);
	if (!f) return false;
	auto be32 = [&](uint32_t v) { uint8_t t[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; f.write((char *)t, 4); };
	auto chunk = [&](const char *type, const std::vector<uint8_t> &d) {
		be32((uint32_t)d.size());
		std::vector<uint8_t> td(type, type + 4);
		td.insert(td.end(), d.begin(), d.end());
		f.write((char *)td.data(), td.size());
		be32(crc32_buf(td.data(), td.size()) ^ 0xffffffffu);
	};
	const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
	f.write((const char *)sig, 8);
	chunk("IHDR", {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
		uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0});
	chunk("IDAT", z);
	chunk("IEND", {});
	return true;
}

bool Board::debug_send_sound(uint8_t cmd)
{
	if (m_soundlatch_pending) return false;
	m_soundlatch = cmd;
	m_soundlatch_pending = true;
	m_link.sound_set_nmi(true);
	if (on_sound_command) on_sound_command(cmd);
	return true;
}
