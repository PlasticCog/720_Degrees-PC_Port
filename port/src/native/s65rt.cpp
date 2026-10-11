// Runtime for the translated sound program + the SoundCpu implementation that runs it.
#include "s65rt.h"
#include "soundcpu.h"
#include "coro.h"
#include <cstdio>
#include <cstdlib>

namespace s65rt {

Ctx S;
Board *SB = nullptr;
uint8_t *SR = nullptr;
const uint8_t *SROM = nullptr;
uint16_t sshadow[256];
int sshadow_top = 0;
static void *g_sound_fiber = nullptr;
static void *g_host = nullptr;

bool sin_shadow(uint16_t pc)
{
	for (int i = sshadow_top - 1; i >= 0; i--)
		if (sshadow[i] == pc) return true;
	return false;
}

void sdispatch()
{
	const uint16_t pc = S.pc;
	if (pc < 0x4000 || !s65_table[pc - 0x4000])
	{
		fprintf(stderr, "FATAL: translated sound program reached untranslated code at PC=%04X\n", pc);
		fflush(stderr);
		abort();
	}
	s65_table[pc - 0x4000]();
}

static void take_int(uint16_t vector, bool brk)
{
	const uint16_t ret = S.pc;
	spush((uint8_t)(ret >> 8));
	spush((uint8_t)ret);
	spush((uint8_t)((S.p & ~FB) | FU | (brk ? FB : 0)));
	S.p |= FI;
	S.pc = (uint16_t)(SROM[vector] | (SROM[vector + 1] << 8));
	S.cyc += 7;
	sync6_slow(S.pc);
	sshadow_push(ret);
	sdispatch();
	sshadow_pop();
}

bool sync6_slow(uint16_t next)
{
	S.pc = next;
	for (;;)
	{
		if (S.cyc >= S.end) { coro::switch_to(g_host); continue; }
		if (S.nmi_pending) { S.nmi_pending = false; take_int(0xfffa, false); continue; }
		if (S.irq && !(S.p & FI)) { take_int(0xfffe, false); continue; }
		break;
	}
	return S.pc == next;
}

void sbrk(uint16_t ret)
{
	S.pc = ret;
	take_int(0xfffe, true);
}

static void sound_root(void *)
{
	if (S.cyc >= S.end || S.nmi_pending || (S.irq && !(S.p & FI))) sync6_slow(S.pc);
	for (;;)
	{
		sshadow_top = 0;
		sdispatch();
	}
}

} // namespace s65rt

namespace {
using namespace s65rt;
class NativeSound : public SoundCpu {
public:
	explicit NativeSound(Board &b) : m_board(b) {}
	~NativeSound() override { coro::destroy(m_fiber); }
	void bind()
	{
		SB = &m_board;
		SR = m_board.sound_ram();
		SROM = m_board.roms().sound.data();
	}
	void reset() override
	{
		bind();
		S = Ctx{};
		S.s = 0xfd;
		S.p = FI | FU;
		m_halted = true;
	}
	int run(int cycles) override
	{
		if (m_halted) { S.cyc = 0; return cycles; }
		bind();
		S.cyc = 0;
		S.end = cycles;
		g_host = coro::current();
		coro::switch_to(m_fiber);
		return S.cyc;
	}
	int progress() const override { return S.cyc; }
	void set_nmi(bool s) override
	{
		if (s && !S.nmi_line) S.nmi_pending = true;
		S.nmi_line = s;
	}
	void nmi_state(bool &line, bool &pending) const override { line = S.nmi_line; pending = S.nmi_pending; }
	void restore_nmi(bool line, bool pending) override { S.nmi_line = line; S.nmi_pending = pending; }
	void set_irq(bool s) override { S.irq = s; }
	void set_reset(bool held) override
	{
		m_halted = held;
		if (!held)
		{
			bind();
			S.a = S.x = S.y = 0;
			S.s = 0xfd;
			S.p = FI | FU;
			S.pc = (uint16_t)(SROM[0xfffc] | (SROM[0xfffd] << 8));
			S.nmi_pending = false;
			if (m_fiber) coro::destroy(m_fiber);
			m_fiber = coro::create(sound_root, nullptr, 1 << 20);
			g_sound_fiber = m_fiber;
		}
	}
	uint16_t pc() const override { return S.pc; }
private:
	Board &m_board;
	void *m_fiber = nullptr;
	bool m_halted = true;
};
}

std::unique_ptr<SoundCpu> make_sound_cpu(Board &board)
{
	return std::make_unique<NativeSound>(board);
}
