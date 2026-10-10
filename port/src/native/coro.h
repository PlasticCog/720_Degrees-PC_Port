// Minimal cooperative coroutines (Win32 fibers) used to run the translated programs:
// each program runs on its own fiber and yields back to the frame scheduler when its
// time slice is used up.
#pragma once
#include <cstddef>

namespace coro {
void init_thread();                                   // convert calling thread to a fiber (once)
void *create(void (*fn)(void *), void *arg, size_t stack_size);
void destroy(void *fiber);
void switch_to(void *fiber);
void *current();
}
