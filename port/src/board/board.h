// Atari System 2 board support for 720 Degrees, shared by the native game and the
// development reference model.  Implements the memory-mapped hardware the game program
// talks to: palette, video RAM (alpha/MO/playfield), scroll registers, ROM banking,
// interrupt latches, CPU-to-CPU latches, EEPROM, controls and the sound chip ports.
// Behaviour follows MAME's atarisy2 driver (BSD-3-Clause, Aaron Giles).
#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <string>
#include <functional>
#include <memory>

struct RomSet {
	std::vector<uint8_t> main;      // 0x90000 'maincpu' region
	std::vector<uint8_t> sound;     // 0x10000
	std::vector<uint8_t> tiles;     // 0x40000
	std::vector<uint8_t> sprites;   // 0x100000 (inverted as on the board)
	std::vector<uint8_t> chars;     // 0x4000
	std::vector<uint8_t> eeprom;    // 0x200 default contents
	bool load(const std::string &dir, std::string &error);
};

// decoded graphics, one byte per pixel
struct GfxSet {
	std::vector<uint8_t> tiles;     // 8192 x (8x8)   4bpp playfield tiles
	std::vector<uint8_t> sprites;   // 8192 x (16x16) 4bpp motion objects
	std::vector<uint8_t> chars;     // 1024 x (8x8)   2bpp alphanumerics
	void decode(const RomSet &roms);
};

struct Inputs {
	bool coin1 = false, coin2 = false, service_coin = false, test = false;
	bool button1 = false, button2 = false;      // pressed = true
	bool dev_freeze = false, dev_step = false;  // IN0 bits 0 / 2: not wired on the cabinet; the game
	                                            // reads them as a developer freeze / frame step
	uint8_t leta_center = 0, leta_rotate = 0;   // 720 controller counters
	uint8_t dsw0 = 0x00, dsw1 = 0x55;           // default DIP switches (MAME defaults)
};

class SoundChips;

// Interface back to whatever executes the two programs (interpreter or translated code)
class CpuLink {
public:
	virtual ~CpuLink() = default;
	virtual void main_set_irq_lines(uint8_t cp) = 0;   // T-11 CP3..CP0
	virtual void main_end_timeslice() = 0;             // MAME 'synchronize' semantics
	virtual void sound_set_nmi(bool state) = 0;
	virtual void sound_set_irq(bool state) = 0;
	virtual void sound_set_reset(bool held) = 0;
};

class Board {
public:
	static constexpr int SCREEN_W = 512, SCREEN_H = 384, VTOTAL = 416;
	static constexpr double MAIN_CLOCK = 10000000.0;               // T-11 (MAME cycles)
	static constexpr double SOUND_CLOCK = 14318181.0 / 8.0;       // 6502
	static constexpr double LINE_TIME = 640.0 / 16000000.0;       // 40 us

	Board(const RomSet &roms, CpuLink &link);
	~Board();
	void reset();

	// ---- main CPU (T-11) bus
	uint16_t main_read16(uint16_t addr, uint16_t mask);
	void main_write16(uint16_t addr, uint16_t data, uint16_t mask);
	uint8_t main_read8(uint16_t addr)
	{
		uint16_t v = main_read16(addr & 0xfffe, (addr & 1) ? 0xff00 : 0x00ff);
		return (addr & 1) ? uint8_t(v >> 8) : uint8_t(v);
	}
	void main_write8(uint16_t addr, uint8_t data)
	{
		if (addr & 1) main_write16(addr & 0xfffe, uint16_t(data << 8), 0xff00);
		else main_write16(addr, data, 0x00ff);
	}
	// slapstic 137412-107: full state machine (reference) driven by every bus access
	void slapstic_tap(uint32_t addr);
	// native simplified model: only reset (0x8000) and bank-select accesses matter
	void slapstic_simple(uint16_t addr)
	{
		if ((addr & 0xfffe) == 0x8000) m_slap_armed = true;
		else if (m_slap_armed && addr >= 0x8030 && addr < 0x803e && !(addr & 2))
		{
			m_slap.current = (addr - 0x8030) >> 2;
			m_slap_armed = false;
			m_slap.state = 0;
		}
	}
	int vmmu_bank() const { return m_slap.current; }

	// ---- sound CPU (6502) bus
	uint8_t sound_read(uint16_t addr);
	void sound_write(uint16_t addr, uint8_t data);

	// ---- timing, called by the scheduler
	void begin_line(int line);                 // 32V / VBLANK / partial video updates
	void sound_periodic_irq();                 // 244 Hz timer
	void apply_pending();                      // deferred register writes ('synchronize')
	int scanline = 0;
	uint64_t frame_number = 0;

	// ---- video output
	const uint16_t *frame_indices() const { return m_frame.data(); }
	void frame_rgb(uint32_t *out) const;       // 512x384 ARGB8888
	uint32_t palette_rgb(int index) const;                 // current palette RAM
	static uint32_t raw_rgb(uint16_t raw);                 // one palette word as ARGB
	void finish_frame() { update_partial(SCREEN_H - 1); }

	// ---- audio
	SoundChips &chips() { return *m_chips; }

	Inputs input;

	// ---- state access (debugging, verification, editor)
	uint8_t *main_ram() { return m_ram.data(); }
	uint16_t *palette_ram() { return m_palette.data(); }
	uint16_t *alpha_ram() { return m_alpha.data(); }
	uint16_t *mo_ram() { return m_mo.data(); }
	uint16_t *pf_top() { return m_pft.data(); }
	uint16_t *pf_bottom() { return m_pfb.data(); }
	uint8_t *sound_ram() { return m_sram.data(); }
	uint8_t *eeprom() { return m_eeprom.data(); }
	uint16_t xscroll() const { return m_xscroll; }
	uint16_t yscroll() const { return m_yscroll; }
	int bank_entry(int i) const { return m_bank_entry[i]; }
	const RomSet &roms() const { return m_roms; }
	const GfxSet &gfx() const { return m_gfx; }
	const uint8_t *main_rom() const { return m_roms.main.data(); }
	size_t banked_rom_offset(int window, uint16_t addr) const
	{
		return 0x10000 + (size_t)m_bank_entry[window] * 0x2000 + (addr & 0x1fff);
	}

	// ---- debug menu support
	struct Debug {
		bool hide_pf = false, hide_mo = false, hide_alpha = false;
		bool capture_tiles = false;            // record the playfield tile code under every pixel
	} debug;
	// playfield tile code (bank-resolved, 0..8191) under each pixel of the last frame
	const uint16_t *debug_tile_codes() const { return m_dbg_tiles.data(); }
	int pf_tile_bank(int i) const { return m_pf_tile_bank[i & 1]; }
	// send a command to the sound CPU as if the main program wrote the sound latch;
	// returns false while the previous command has not been read yet
	bool debug_send_sound(uint8_t cmd);

	// full state seen by the main CPU (memory, video RAM, registers, latches), for the
	// difference tester of the rewritten routines (port/src/logic)
	struct Snapshot {
		std::array<uint8_t, 0x1000> ram;
		std::array<uint16_t, 0x100> palette;
		std::array<uint16_t, 0xc00> alpha;
		std::array<uint16_t, 0x400> mo;
		std::array<uint16_t, 0x1000> pft, pfb;
		std::array<uint8_t, 0x200> eeprom;
		int slap_state, slap_current, slap_loaded;
		bool slap_armed;
		int bank_entry[2];
		uint16_t xscroll, yscroll;
		int pf_scrollx, pf_scrolly, pf_tile_bank[2], pending_yscroll;
		uint8_t interrupt_enable;
		int pending_int_enable;
		bool scanline_int, video_int, p2portwr, p2portrd;
		uint8_t soundlatch, mainlatch;
		bool soundlatch_pending, mainlatch_pending, sound_reset;
		int last_rendered, mo_last_xpos, scanline;
	};
	void save(Snapshot &s) const;
	void load(const Snapshot &s);

	// hooks for tooling
	std::function<void(uint8_t)> on_sound_command;
	std::function<void(uint16_t addr, uint16_t data, uint16_t mask)> on_palette_write;   // tooling
	std::function<void(uint16_t value)> on_in0_read;                                     // tooling
	std::function<void(uint16_t addr, uint8_t data)> on_sound_cpu_io_write;              // tooling: 6502 writes 0x1800-0x1fff
	std::function<void(uint16_t addr, uint8_t data)> on_sound_cpu_io_read;               // tooling: 6502 reads 0x1800-0x187f
	std::function<void(int from, int to, uint32_t addr, int bank)> on_slapstic;

	struct Slapstic { int state = 0, current = 3, loaded = 0; } m_slap;

private:
	const RomSet &m_roms;
	CpuLink &m_link;
	GfxSet m_gfx;
	std::unique_ptr<SoundChips> m_chips;

	std::array<uint8_t, 0x1000> m_ram{};
	std::array<uint16_t, 0x100> m_palette{};
	std::array<uint16_t, 0x100> m_shown_palette{};      // palette as the vertical blank began (frame_rgb)
	std::array<uint16_t, 0xc00> m_alpha{};
	std::array<uint16_t, 0x400> m_mo{};
	std::array<uint16_t, 0x1000> m_pft{}, m_pfb{};
	std::array<uint8_t, 0x1000> m_sram{};
	std::array<uint8_t, 0x200> m_eeprom{};
	bool m_slap_armed = false;

	int m_bank_entry[2] = {0, 0};
	uint16_t m_xscroll = 0, m_yscroll = 0;
	int m_pf_scrollx = 0, m_pf_scrolly = 0, m_pf_tile_bank[2] = {0, 0};
	int m_pending_yscroll = -1;

	uint8_t m_interrupt_enable = 0;
	int m_pending_int_enable = -1;
	bool m_scanline_int = false, m_video_int = false, m_p2portwr = false, m_p2portrd = false;
	uint8_t m_soundlatch = 0, m_mainlatch = 0;
	bool m_soundlatch_pending = false, m_mainlatch_pending = false;
	bool m_sound_reset = true;
	bool m_sound_irq_restart = false;
public:
	// the sound CPU just left reset: its periodic interrupt starts one period later (MAME's
	// periodic interrupts restart when the CPU is reset); taken by the scheduler
	bool take_sound_irq_restart() { const bool r = m_sound_irq_restart; m_sound_irq_restart = false; return r; }
private:

	int m_last_rendered = -1;
	int m_mo_last_xpos = 0;
	std::vector<uint16_t> m_frame;
	std::vector<uint16_t> m_mobuf;
	std::vector<uint16_t> m_dbg_tiles = std::vector<uint16_t>(SCREEN_W * SCREEN_H, 0);

	void update_interrupts();
	void update_partial(int line);
	void render_line(int y);
	uint8_t leta_r(int offset);
};

bool write_png(const std::string &path, const uint32_t *argb, int w, int h);
