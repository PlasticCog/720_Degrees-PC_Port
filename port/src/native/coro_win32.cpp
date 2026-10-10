#include "coro.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace coro {

namespace {
struct Start { void (*fn)(void *); void *arg; };
void *g_main = nullptr;
void CALLBACK trampoline(void *p)
{
	Start s = *(Start *)p;
	delete (Start *)p;
	s.fn(s.arg);
	// coroutines never return; if one does, park it forever
	for (;;) SwitchToFiber(g_main);
}
}

void init_thread()
{
	if (g_main) return;
	g_main = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(nullptr);
}

void *create(void (*fn)(void *), void *arg, size_t stack_size)
{
	init_thread();
	return CreateFiber(stack_size, trampoline, new Start{fn, arg});
}

void destroy(void *fiber)
{
	if (fiber) DeleteFiber(fiber);
}

void switch_to(void *fiber)
{
	SwitchToFiber(fiber);
}

void *current()
{
	return GetCurrentFiber();
}

}
