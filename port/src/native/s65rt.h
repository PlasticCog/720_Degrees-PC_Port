// Runtime support for the statically translated 720 Degrees sound program
// (6502 code converted to C++ by tools/m6502_to_cpp.py).  Semantics match the
// reference interpreter exactly (flags, decimal mode, cycles, interrupt timing).
#pragma once
#include <cstdint>
#include "board.h"

namespace s65rt {

enum : uint8_t { FC = 0x01, FZ = 0x02, FI = 0x04, FD = 0x08, FB = 0x10, FU = 0x20, FV = 0x40, FN = 0x80 };

struct Ctx {
	uint8_t a, x, y, s, p;
	uint16_t pc;
	int32_t cyc;          // cycles consumed in the current run()
	int32_t end;          // cycle budget of the current run()
	bool irq;             // IRQ line level
	bool nmi_line, nmi_pending;
};
extern Ctx S;
extern Board *SB;
extern uint8_t *SR;              // sound RAM (4K, mirrored)
extern const uint8_t *SROM;      // 64K sound region (ROM from 0x4000)

inline uint8_t sio_rd(uint16_t a) { return SB->sound_read(a); }
inline void sio_wr(uint16_t a, uint8_t v) { SB->sound_write(a, v); }
inline uint8_t srd(uint16_t a)
{
	if ((a & 0xdfff) < 0x1000) return SR[a & 0xfff];
	if (a >= 0x4000) return SROM[a];
	return SB->sound_read(a);
}
inline void swr(uint16_t a, uint8_t v)
{
	if ((a & 0xdfff) < 0x1000) { SR[a & 0xfff] = v; return; }
	if (a >= 0x4000) return;
	SB->sound_write(a, v);
}
inline void spush(uint8_t v) { SR[0x100 | S.s] = v; S.s--; }
inline uint8_t spull() { S.s++; return SR[0x100 | S.s]; }

inline void setnz(uint8_t r) { S.p = (uint8_t)((S.p & ~(FN | FZ)) | (r & FN) | (r ? 0 : FZ)); }
inline void op_adc(uint8_t m)
{
	uint8_t &a = S.a, &p = S.p;
	if (p & FD)
	{
		int c = p & FC;
		int lo = (a & 0x0f) + (m & 0x0f) + c;
		int hi = (a & 0xf0) + (m & 0xf0);
		p &= ~(FV | FC | FN | FZ);
		if (!((a + m + c) & 0xff)) p |= FZ;
		if (lo > 0x09) { hi += 0x10; lo += 0x06; }
		if (hi & 0x80) p |= FN;
		if (~(a ^ m) & (a ^ hi) & 0x80) p |= FV;
		if (hi > 0x90) hi += 0x60;
		if (hi & 0xff00) p |= FC;
		a = (uint8_t)((lo & 0x0f) + (hi & 0xf0));
	}
	else
	{
		int sum = a + m + (p & FC);
		p &= ~(FV | FC);
		if (~(a ^ m) & (a ^ sum) & 0x80) p |= FV;
		if (sum & 0x100) p |= FC;
		a = (uint8_t)sum;
		setnz(a);
	}
}
inline void op_sbc(uint8_t m)
{
	uint8_t &a = S.a, &p = S.p;
	if (p & FD)
	{
		int c = (p & FC) ? 0 : 1;
		int diff = a - m - c;
		int lo = (a & 0x0f) - (m & 0x0f) - c;
		int hi = (a & 0xf0) - (m & 0xf0);
		p &= ~(FV | FC | FZ | FN);
		if (lo & 0x10) { lo -= 6; hi--; }
		if ((a ^ m) & (a ^ diff) & 0x80) p |= FV;
		if (hi & 0x0100) hi -= 0x60;
		if (!(diff & 0xff00)) p |= FC;
		if (!(diff & 0xff)) p |= FZ;
		if (diff & 0x80) p |= FN;
		a = (uint8_t)((lo & 0x0f) | (hi & 0xf0));
	}
	else
	{
		int diff = a - m - ((p & FC) ? 0 : 1);
		p &= ~(FV | FC);
		if ((a ^ m) & (a ^ diff) & 0x80) p |= FV;
		if (!(diff & 0x100)) p |= FC;
		a = (uint8_t)diff;
		setnz(a);
	}
}
inline void op_cmp(uint8_t r, uint8_t m) { int d = r - m; S.p = (uint8_t)((S.p & ~FC) | (d >= 0 ? FC : 0)); setnz((uint8_t)d); }
inline uint8_t op_asl(uint8_t m) { S.p = (uint8_t)((S.p & ~FC) | (m >> 7)); m <<= 1; setnz(m); return m; }
inline uint8_t op_lsr(uint8_t m) { S.p = (uint8_t)((S.p & ~FC) | (m & 1)); m >>= 1; setnz(m); return m; }
inline uint8_t op_rol(uint8_t m) { uint8_t c = S.p & FC; S.p = (uint8_t)((S.p & ~FC) | (m >> 7)); m = (uint8_t)((m << 1) | c); setnz(m); return m; }
inline uint8_t op_ror(uint8_t m) { uint8_t c = S.p & FC; S.p = (uint8_t)((S.p & ~FC) | (m & 1)); m = (uint8_t)((m >> 1) | (c << 7)); setnz(m); return m; }
inline void op_bit(uint8_t m) { S.p = (uint8_t)((S.p & ~(FN | FV | FZ)) | (m & (FN | FV)) | ((S.a & m) ? 0 : FZ)); }

// checkpoint after every instruction: end of slice / pending interrupt
bool sync6_slow(uint16_t next);
inline bool sync6(uint16_t next)
{
	if (S.cyc < S.end && !S.nmi_pending && !(S.irq && !(S.p & FI))) return true;
	return sync6_slow(next);
}
inline void sync6_dyn() { if (S.cyc >= S.end || S.nmi_pending || (S.irq && !(S.p & FI))) sync6_slow(S.pc); }

void sdispatch();
extern uint16_t sshadow[256];
extern int sshadow_top;
inline void sshadow_push(uint16_t pc) { sshadow[sshadow_top++ & 255] = pc; }
inline void sshadow_pop() { sshadow_top--; }
bool sin_shadow(uint16_t pc);
void sbrk(uint16_t ret);

} // namespace s65rt

using s65fn = void (*)();
extern const s65fn s65_table[0xC000];
