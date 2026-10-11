// Runtime support for the statically translated 720 Degrees main program (T-11 code
// converted to C++ by tools/t11_to_cpp.py).  The generated functions operate on the
// register context T and the board memory map; flag and bus semantics match the
// original CPU exactly so the translated program behaves like the arcade program,
// including where interrupts land.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include "board.h"

namespace t11rt {

struct Ctx {
	uint16_t r[8];
	uint8_t psw;
	int32_t icount;        // cycles left in the current time slice
	int32_t aborted;       // cycles given back by main_end_timeslice()
	bool check_irqs;
	bool wait_state;
	uint8_t cp_state;
};
extern Ctx T;
extern Board *B;
extern uint8_t *RAM;
extern const uint8_t *ROM;     // 0x90000 main region (fixed ROM at 0x8000-0xFFFF)

// ---------------------------------------------------------------- memory
uint16_t bus_rw(uint32_t a);
uint8_t bus_rb(uint32_t a);
void bus_ww(uint32_t a, uint16_t v);
void bus_wb(uint32_t a, uint8_t v);

inline uint16_t ram16(uint32_t a) { return (uint16_t)(RAM[a] | (RAM[a + 1] << 8)); }
inline void ram16w(uint32_t a, uint32_t v) { RAM[a] = (uint8_t)v; RAM[a + 1] = (uint8_t)(v >> 8); }
inline uint16_t rom16(uint32_t a) { return (uint16_t)(ROM[a] | (ROM[a + 1] << 8)); }

inline uint16_t rw(uint32_t a) { a &= 0xfffe; return a < 0x1000 ? ram16(a) : bus_rw(a); }
inline uint8_t rb(uint32_t a) { a &= 0xffff; return a < 0x1000 ? RAM[a] : bus_rb(a); }
inline void ww(uint32_t a, uint32_t v) { a &= 0xfffe; if (a < 0x1000) ram16w(a, v); else bus_ww(a, (uint16_t)v); }
inline void wb(uint32_t a, uint32_t v) { a &= 0xffff; if (a < 0x1000) RAM[a] = (uint8_t)v; else bus_wb(a, (uint8_t)v); }
inline void slap(uint32_t a) { B->slapstic_simple((uint16_t)a); }

inline void push(uint32_t v) { T.r[6] -= 2; ww(T.r[6], v); }
inline uint16_t pop() { uint16_t v = rw(T.r[6]); T.r[6] += 2; return v; }

// ---------------------------------------------------------------- flags (MAME t11ops macros)
enum { CF = 1, VF = 2, ZF = 4, NF = 8 };
inline int nzw(int r) { return ((r >> 12) & 8) | (((r & 0xffff) == 0) << 2); }
inline int nzb(int r) { return ((r >> 4) & 8) | (((r & 0xff) == 0) << 2); }
inline int vw(int s, int d, int r) { return ((s ^ d ^ r ^ (r >> 1)) >> 14) & 2; }
inline int vb(int s, int d, int r) { return ((s ^ d ^ r ^ (r >> 1)) >> 6) & 2; }
inline int cw(int r) { return (r >> 16) & 1; }
inline int cb(int r) { return (r >> 8) & 1; }
inline void f_nzv(int nz) { T.psw = (uint8_t)((T.psw & ~(NF | ZF | VF)) | nz); }             // CLR_NZV; SET NZ
inline void f_nzvc(int f) { T.psw = (uint8_t)((T.psw & ~(NF | ZF | VF | CF)) | f); }         // CLR_NZVC; SET ...
inline void f_shift() { T.psw |= ((T.psw << 1) ^ (T.psw >> 2)) & 2; }                        // V = N ^ C
inline int C() { return T.psw & CF; }
inline bool fN() { return (T.psw & NF) != 0; }
inline bool fZ() { return (T.psw & ZF) != 0; }
inline bool fV() { return (T.psw & VF) != 0; }
inline bool fC() { return (T.psw & CF) != 0; }

// ---------------------------------------------------------------- control
// slow path of the per-instruction checkpoint: interrupts, end of time slice
bool sync_slow(uint16_t next);
// atomic mode (difference tester): code runs to completion without taking interrupts or
// ending its time slice; the lowest stack pointer reached is recorded
extern bool g_atomic;
extern uint16_t g_min_sp;
// code that waits for an interrupt cannot finish in atomic mode: after this many
// instructions AtomicTimeout is thrown and the difference tester gives up on that call
extern uint32_t g_atomic_steps, g_atomic_limit;
constexpr uint32_t ATOMIC_STEP_LIMIT = 4000000;
struct AtomicTimeout {};
// atomic mode also appends every instruction boundary (next PC << 32 | SP << 16 | PSW) to the
// active traces, so the difference tester can compare the rewritten routines' steps with the
// original's
constexpr int MAX_TRACES = 8;
extern std::vector<uint64_t> *g_traces[MAX_TRACES];
extern int g_trace_count;
inline bool sync(uint16_t next)
{
	if (T.icount > 0 && !T.check_irqs) return true;
	return sync_slow(next);
}
inline void sync_dyn() { if (T.icount <= 0 || T.check_irqs) sync_slow(T.r[7]); }

// run the code at T.r[7] until it returns (RTS/RTI) — dispatches by address + current bank
void dispatch();
// expected-return shadow stack (lets mismatched returns unwind to the right C++ frame)
extern uint16_t shadow[256];
extern int shadow_top;
inline void shadow_push(uint16_t pc) { shadow[shadow_top++ & 255] = pc; }
inline void shadow_pop() { shadow_top--; }
bool in_shadow(uint16_t pc);
// hardware trap/EMT/TRAP: push PSW+PC, load vector, run handler
void trap_to(uint16_t vector, uint16_t next);
void halt_insn(uint16_t next);
[[noreturn]] void fatal_unknown_pc(uint16_t pc);

} // namespace t11rt

// patch points: immediates in the translated code that mods and the debug menu may change
// (F:E8B6 MOV #0x6000,R2 - town alternate block bank; F:E8B0 MOV #0xA000,R2 - park odd-pointer bank;
//  F:A068 MOV #12,@#0x02EE - gravity while airborne; A5:4F2C MOV #0x180,@#0x01B4 - kick start speed)
namespace t11rt { extern uint16_t g_patch_town_alt_base, g_patch_park_odd_base, g_patch_gravity, g_patch_kick_speed; }

// generated dispatch tables
using t11fn = void (*)();
struct T11CodeSeg { uint16_t base, size; const t11fn *fn; };   // fn[(pc-base)/2]
extern const T11CodeSeg t11_seg_fixed;
extern const T11CodeSeg *t11_seg_bank[2][64];                  // [window A/B][bank]
