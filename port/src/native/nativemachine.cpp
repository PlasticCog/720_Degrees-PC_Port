#include "nativemachine.h"
#include "t11rt.h"
#include "coro.h"

namespace t11rt {
extern void *g_game_fiber;
extern void *g_host_fiber;
void game_fiber_main(void *);
}

NativeMachine::NativeMachine(const RomSet &roms) : board(roms, *this)
{
	sound = make_sound_cpu(board);
	board.chips().now = [this]() { return sched.sound_slice_start + (uint64_t)sound->progress(); };
	coro::init_thread();
}

NativeMachine::~NativeMachine()
{
	coro::destroy(m_fiber);
}

void NativeMachine::main_set_irq_lines(uint8_t cp)
{
	using namespace t11rt;
	if (cp != T.cp_state)
	{
		T.cp_state = cp;
		T.check_irqs = true;
	}
}

void NativeMachine::main_end_timeslice()
{
	using namespace t11rt;
	if (T.icount > 0)
	{
		T.aborted += T.icount;
		T.icount = 0;
	}
}

void NativeMachine::reset()
{
	using namespace t11rt;
	sched = SchedState{};
	B = &board;
	RAM = board.main_ram();
	ROM = board.main_rom();
	T = Ctx{};
	T.r[6] = 0376;
	T.r[7] = 0x8000;
	T.psw = 0340;
	shadow_top = 0;
	board.reset();
	sound->reset();
	if (m_fiber) coro::destroy(m_fiber);
	m_fiber = coro::create(game_fiber_main, nullptr, 4 << 20);
	g_game_fiber = m_fiber;
	g_host_fiber = coro::current();
}

namespace {
struct MainRunner {
	int run(int cycles)
	{
		using namespace t11rt;
		T.icount = cycles;
		T.aborted = 0;
		coro::switch_to(g_game_fiber);
		return cycles - T.icount - T.aborted;
	}
};
struct SoundRunner {
	SoundCpu &s;
	int run(int n) { return s.run(n); }
};
}

void NativeMachine::run_frame()
{
	using namespace t11rt;
	B = &board;
	RAM = board.main_ram();
	ROM = board.main_rom();
	g_host_fiber = coro::current();
	MainRunner mr;
	SoundRunner sr{*sound};
	::run_frame(board, mr, sr, sched);
}
