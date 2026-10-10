// Cooperative coroutines for Linux and macOS (see coro.h; Windows uses coro_win32.cpp).
//
// The translated programs switch between the host and their coroutine a few hundred
// thousand times a second, so the switch must be cheap.  On x86-64 (System V ABI) and
// ARM64 (AAPCS64) a small assembly routine saves only the callee-saved registers and swaps
// stacks.  Other targets fall back to ucontext, which is slower (it also saves the signal
// mask with a system call) but portable.
#include "coro.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if (defined(__x86_64__) || defined(__aarch64__)) && !defined(CORO_FORCE_UCONTEXT)
#define CORO_ASM 1
#else
#define CORO_ASM 0
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700     // macOS only declares the ucontext functions with this
#endif
#include <ucontext.h>
#endif

#if defined(__APPLE__)
#define CORO_SYM(x) "_" #x
#else
#define CORO_SYM(x) #x
#endif

#if CORO_ASM
extern "C" void coro_swap(void **save_sp, void *load_sp);
extern "C" void coro_entry();

#if defined(__x86_64__)
// coro_swap(rdi = where to save the current stack pointer, rsi = stack to switch to)
// coro_entry: first activation of a coroutine; r12 = function, r13 = argument
asm(".text\n"
	".globl " CORO_SYM(coro_swap) "\n"
	".p2align 4\n"
	CORO_SYM(coro_swap) ":\n"
	"	pushq %rbp\n"
	"	pushq %rbx\n"
	"	pushq %r12\n"
	"	pushq %r13\n"
	"	pushq %r14\n"
	"	pushq %r15\n"
	"	movq %rsp, (%rdi)\n"
	"	movq %rsi, %rsp\n"
	"	popq %r15\n"
	"	popq %r14\n"
	"	popq %r13\n"
	"	popq %r12\n"
	"	popq %rbx\n"
	"	popq %rbp\n"
	"	ret\n"
	".globl " CORO_SYM(coro_entry) "\n"
	".p2align 4\n"
	CORO_SYM(coro_entry) ":\n"
	"	movq %r13, %rdi\n"
	"	andq $-16, %rsp\n"
	"	callq *%r12\n"
	"	ud2\n");
#else
// coro_swap(x0 = where to save the current stack pointer, x1 = stack to switch to)
// coro_entry: first activation of a coroutine; x19 = function, x20 = argument
asm(".text\n"
	".globl " CORO_SYM(coro_swap) "\n"
	".p2align 2\n"
	CORO_SYM(coro_swap) ":\n"
	"	sub sp, sp, #176\n"
	"	stp x19, x20, [sp, #0]\n"
	"	stp x21, x22, [sp, #16]\n"
	"	stp x23, x24, [sp, #32]\n"
	"	stp x25, x26, [sp, #48]\n"
	"	stp x27, x28, [sp, #64]\n"
	"	stp x29, x30, [sp, #80]\n"
	"	stp d8, d9, [sp, #96]\n"
	"	stp d10, d11, [sp, #112]\n"
	"	stp d12, d13, [sp, #128]\n"
	"	stp d14, d15, [sp, #144]\n"
	"	mov x9, sp\n"
	"	str x9, [x0]\n"
	"	mov sp, x1\n"
	"	ldp x19, x20, [sp, #0]\n"
	"	ldp x21, x22, [sp, #16]\n"
	"	ldp x23, x24, [sp, #32]\n"
	"	ldp x25, x26, [sp, #48]\n"
	"	ldp x27, x28, [sp, #64]\n"
	"	ldp x29, x30, [sp, #80]\n"
	"	ldp d8, d9, [sp, #96]\n"
	"	ldp d10, d11, [sp, #112]\n"
	"	ldp d12, d13, [sp, #128]\n"
	"	ldp d14, d15, [sp, #144]\n"
	"	add sp, sp, #176\n"
	"	ret\n"
	".globl " CORO_SYM(coro_entry) "\n"
	".p2align 2\n"
	CORO_SYM(coro_entry) ":\n"
	"	mov x0, x20\n"
	"	blr x19\n"
	"	brk #0\n");
#endif
#endif

namespace coro {

namespace {

struct Coroutine {
#if CORO_ASM
	void *sp = nullptr;
#else
	ucontext_t uc;
#endif
	void *stack = nullptr;
	void (*fn)(void *) = nullptr;
	void *arg = nullptr;
};

thread_local Coroutine g_thread;                // the thread's own context (the host)
thread_local Coroutine *g_current = nullptr;

// runs the coroutine's function; coroutines are not meant to return, but if one does it
// parks by switching back to the thread forever
void run(void *p)
{
	Coroutine *c = (Coroutine *)p;
	c->fn(c->arg);
	for (;;) switch_to(&g_thread);
}

#if !CORO_ASM
void run_ucontext(unsigned lo, unsigned hi)
{
	run((void *)(((uintptr_t)hi << 32) | (uintptr_t)lo));
}
#endif

}

void init_thread()
{
	if (!g_current) g_current = &g_thread;
}

void *create(void (*fn)(void *), void *arg, size_t stack_size)
{
	init_thread();
	Coroutine *c = new Coroutine;
	c->fn = fn;
	c->arg = arg;
	c->stack = malloc(stack_size);
	if (!c->stack) { delete c; return nullptr; }
#if CORO_ASM
	uintptr_t top = ((uintptr_t)c->stack + stack_size) & ~(uintptr_t)15;
#if defined(__x86_64__)
	// frame popped by coro_swap: r15 r14 r13 r12 rbx rbp, then the return address
	uintptr_t *frame = (uintptr_t *)(top - 8 * 8);
	memset(frame, 0, 8 * 8);
	frame[2] = (uintptr_t)c;                // r13: argument
	frame[3] = (uintptr_t)&run;             // r12: function
	frame[6] = (uintptr_t)&coro_entry;      // return address
	c->sp = frame;
#else
	// frame restored by coro_swap: x19..x28, x29, x30, d8..d15 (176 bytes)
	uintptr_t *frame = (uintptr_t *)(top - 176);
	memset(frame, 0, 176);
	frame[0] = (uintptr_t)&run;             // x19: function
	frame[1] = (uintptr_t)c;                // x20: argument
	frame[11] = (uintptr_t)&coro_entry;     // x30: return address
	c->sp = frame;
#endif
#else
	getcontext(&c->uc);
	c->uc.uc_stack.ss_sp = c->stack;
	c->uc.uc_stack.ss_size = stack_size;
	c->uc.uc_link = nullptr;
	const uintptr_t p = (uintptr_t)c;
	makecontext(&c->uc, (void (*)())run_ucontext, 2, (unsigned)(p & 0xffffffffu), (unsigned)(p >> 32));
#endif
	return c;
}

void destroy(void *fiber)
{
	Coroutine *c = (Coroutine *)fiber;
	if (!c || c == &g_thread) return;
	free(c->stack);
	delete c;
}

void switch_to(void *fiber)
{
	init_thread();
	Coroutine *to = (Coroutine *)fiber, *from = g_current;
	if (to == from) return;
	g_current = to;
#if CORO_ASM
	coro_swap(&from->sp, to->sp);
#else
	swapcontext(&from->uc, &to->uc);
#endif
}

void *current()
{
	init_thread();
	return g_current;
}

}
