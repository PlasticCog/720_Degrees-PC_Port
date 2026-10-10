// Self-test for the coroutine backends (coro_win32.cpp / coro_posix.cpp); runs in CI on
// Windows, Linux and macOS.  Exits non-zero on failure.
#include "coro.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

void *g_host = nullptr, *g_a = nullptr, *g_b = nullptr;
volatile long g_counter_a = 0, g_counter_b = 0;
volatile double g_sum = 0;

int deep(int n) { volatile char pad[256]; pad[0] = (char)n; return n ? deep(n - 1) + pad[0] : 0; }

void fiber_a(void *arg)
{
	const long limit = (long)(intptr_t)arg;
	double local = 1.5;                                  // must survive switches
	const int d = deep(2000);                            // uses ~600 KB of this stack
	for (long i = 0; i < limit; i++)
	{
		g_counter_a = g_counter_a + 1;
		local = local * 1.0000001;
		coro::switch_to(g_host);
	}
	g_sum = local + d;
	for (;;) coro::switch_to(g_host);
}

void fiber_b(void *)
{
	for (;;)
	{
		g_counter_b = g_counter_b + 2;
		coro::switch_to(g_host);
	}
}

}

int main()
{
	coro::init_thread();
	g_host = coro::current();
	const long N = 2000000;
	g_a = coro::create(fiber_a, (void *)(intptr_t)N, 4 << 20);
	g_b = coro::create(fiber_b, nullptr, 1 << 20);
	if (!g_a || !g_b) { fprintf(stderr, "create failed\n"); return 1; }

	double host_float = 3.25;
	const auto t0 = std::chrono::steady_clock::now();
	for (long i = 0; i < N; i++)
	{
		coro::switch_to(g_a);                           // like the game program
		coro::switch_to(g_b);                           // like the sound program
		host_float += 0.5;
		if (coro::current() != g_host) { fprintf(stderr, "current() wrong at %ld\n", i); return 1; }
	}
	coro::switch_to(g_a);                               // lets fiber_a finish its loop
	const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

	int fails = 0;
	if (g_counter_a != N) { fprintf(stderr, "counter a %ld, expected %ld\n", (long)g_counter_a, N); fails++; }
	if (g_counter_b != 2 * N) { fprintf(stderr, "counter b %ld, expected %ld\n", (long)g_counter_b, 2 * N); fails++; }
	if (host_float != 3.25 + 0.5 * N) { fprintf(stderr, "host float corrupted: %f\n", host_float); fails++; }
	const double expect = 1.5 * std::pow(1.0000001, (double)N) + deep(2000);
	if (std::fabs(g_sum - expect) > 1e-6 * expect) { fprintf(stderr, "fiber float corrupted: %f vs %f\n", (double)g_sum, expect); fails++; }
	coro::destroy(g_a);
	coro::destroy(g_b);
	const double ns = secs * 1e9 / (4.0 * N);
	printf("coro_test: %s, %.1f ns per switch\n", fails ? "FAILED" : "ok", ns);
	return fails ? 1 : 0;
}
