// The 6502's instructions as steps, for the rewritten sound routines (see sound.h).
//
// An operand is read by its addressing mode, which charges the instruction's cycles first (as
// the 6502 takes them before the access; an indexed read one more across a page):
//   imm(v)  zp(z)  zpx(z)  zpy(z)  ab(a)  abx(a)  aby(a)  ix(z) = (z,X)  iy(z) = (z),Y
// and an address to write or to change likewise:
//   wzp wzpx wab wabx waby wix wiy     (stores)
//   mzp mzpx mab mabx                   (INC, DEC, shifts in memory)
// The instruction then does its work, sets the flags and steps to the next address, e.g.
//   lda(zp(0x10), 0x4012);   sta(wabx(0x0240), 0x4015);   inc(mzp(0x8C), 0x4017);
#pragma once
#include "sound.h"

namespace sound {
namespace op {

using namespace s65rt;

// operands
inline uint8_t imm(uint8_t v) { cyc(2); return v; }
inline uint8_t zp(uint8_t z) { cyc(3); return SR[z]; }
inline uint8_t zpx(uint8_t z) { cyc(4); return SR[(uint8_t)(z + S.x)]; }
inline uint8_t zpy(uint8_t z) { cyc(4); return SR[(uint8_t)(z + S.y)]; }
inline uint8_t ab(uint16_t a) { cyc(4); return rd(a); }
inline uint8_t abx(uint16_t a) { cyc(4); return rd(idx(a, S.x)); }
inline uint8_t aby(uint16_t a) { cyc(4); return rd(idx(a, S.y)); }
inline uint8_t ix(uint8_t z) { cyc(6); return rd(zp16((uint8_t)(z + S.x))); }
inline uint8_t iy(uint8_t z) { cyc(5); return rd(idx(zp16(z), S.y)); }
// store addresses
inline uint16_t wzp(uint8_t z) { cyc(3); return z; }
inline uint16_t wzpx(uint8_t z) { cyc(4); return (uint8_t)(z + S.x); }
inline uint16_t wzpy(uint8_t z) { cyc(4); return (uint8_t)(z + S.y); }
inline uint16_t wab(uint16_t a) { cyc(4); return a; }
inline uint16_t wabx(uint16_t a) { cyc(5); return (uint16_t)(a + S.x); }
inline uint16_t waby(uint16_t a) { cyc(5); return (uint16_t)(a + S.y); }
inline uint16_t wix(uint8_t z) { cyc(6); return zp16((uint8_t)(z + S.x)); }
inline uint16_t wiy(uint8_t z) { cyc(6); return (uint16_t)(zp16(z) + S.y); }
// read-modify-write addresses
inline uint16_t mzp(uint8_t z) { cyc(5); return z; }
inline uint16_t mzpx(uint8_t z) { cyc(6); return (uint8_t)(z + S.x); }
inline uint16_t mab(uint16_t a) { cyc(6); return a; }
inline uint16_t mabx(uint16_t a) { cyc(7); return (uint16_t)(a + S.x); }

// loads, stores, arithmetic
inline void lda(uint8_t v, uint16_t next) { S.a = v; nz(S.a); step(next); }
inline void ldx(uint8_t v, uint16_t next) { S.x = v; nz(S.x); step(next); }
inline void ldy(uint8_t v, uint16_t next) { S.y = v; nz(S.y); step(next); }
inline void sta(uint16_t a, uint16_t next) { wr(a, S.a); step(next); }
inline void stx(uint16_t a, uint16_t next) { wr(a, S.x); step(next); }
inline void sty(uint16_t a, uint16_t next) { wr(a, S.y); step(next); }
inline void adc(uint8_t v, uint16_t next) { op_adc(v); step(next); }
inline void sbc(uint8_t v, uint16_t next) { op_sbc(v); step(next); }
inline void and_(uint8_t v, uint16_t next) { S.a &= v; nz(S.a); step(next); }
inline void ora(uint8_t v, uint16_t next) { S.a |= v; nz(S.a); step(next); }
inline void eor(uint8_t v, uint16_t next) { S.a ^= v; nz(S.a); step(next); }
inline void cmp(uint8_t v, uint16_t next) { op_cmp(S.a, v); step(next); }
inline void cpx(uint8_t v, uint16_t next) { op_cmp(S.x, v); step(next); }
inline void cpy(uint8_t v, uint16_t next) { op_cmp(S.y, v); step(next); }
inline void bit(uint8_t v, uint16_t next) { op_bit(v); step(next); }
// memory changed in place
inline void inc(uint16_t a, uint16_t next) { const uint8_t v = (uint8_t)(rd(a) + 1); wr(a, v); nz(v); step(next); }
inline void dec(uint16_t a, uint16_t next) { const uint8_t v = (uint8_t)(rd(a) - 1); wr(a, v); nz(v); step(next); }
inline void asl(uint16_t a, uint16_t next) { const uint8_t v = op_asl(rd(a)); wr(a, v); step(next); }
inline void lsr(uint16_t a, uint16_t next) { const uint8_t v = op_lsr(rd(a)); wr(a, v); step(next); }
inline void rol(uint16_t a, uint16_t next) { const uint8_t v = op_rol(rd(a)); wr(a, v); step(next); }
inline void ror(uint16_t a, uint16_t next) { const uint8_t v = op_ror(rd(a)); wr(a, v); step(next); }
// the accumulator shifted
inline void asl_a(uint16_t next) { cyc(2); S.a = op_asl(S.a); step(next); }
inline void lsr_a(uint16_t next) { cyc(2); S.a = op_lsr(S.a); step(next); }
inline void rol_a(uint16_t next) { cyc(2); S.a = op_rol(S.a); step(next); }
inline void ror_a(uint16_t next) { cyc(2); S.a = op_ror(S.a); step(next); }
// registers
inline void inx(uint16_t next) { cyc(2); S.x++; nz(S.x); step(next); }
inline void iny(uint16_t next) { cyc(2); S.y++; nz(S.y); step(next); }
inline void dex(uint16_t next) { cyc(2); S.x--; nz(S.x); step(next); }
inline void dey(uint16_t next) { cyc(2); S.y--; nz(S.y); step(next); }
inline void tax(uint16_t next) { cyc(2); S.x = S.a; nz(S.x); step(next); }
inline void tay(uint16_t next) { cyc(2); S.y = S.a; nz(S.y); step(next); }
inline void txa(uint16_t next) { cyc(2); S.a = S.x; nz(S.a); step(next); }
inline void tya(uint16_t next) { cyc(2); S.a = S.y; nz(S.a); step(next); }
inline void tsx(uint16_t next) { cyc(2); S.x = S.s; nz(S.x); step(next); }
inline void txs(uint16_t next) { cyc(2); S.s = S.x; step(next); }
// the stack and the flags
inline void pha(uint16_t next) { cyc(3); push(S.a); step(next); }
inline void pla(uint16_t next) { cyc(4); S.a = pull(); nz(S.a); step(next); }
inline void php(uint16_t next) { cyc(3); push((uint8_t)(S.p | FB | FU)); step(next); }
inline void plp(uint16_t next) { cyc(4); S.p = (uint8_t)((pull() | FU) & ~FB); step(next); }
inline void clc(uint16_t next) { cyc(2); S.p &= ~FC; step(next); }
inline void sec(uint16_t next) { cyc(2); S.p |= FC; step(next); }
inline void cli(uint16_t next) { cyc(2); S.p &= ~FI; step(next); }
inline void sei(uint16_t next) { cyc(2); S.p |= FI; step(next); }
inline void cld(uint16_t next) { cyc(2); S.p &= ~FD; step(next); }
inline void clv(uint16_t next) { cyc(2); S.p &= ~FV; step(next); }
inline void nop(uint16_t next) { cyc(2); step(next); }
// branches at pc: true when taken
inline bool beq(uint16_t pc, uint16_t target) { return branch(pc, (S.p & FZ) != 0, target); }
inline bool bne(uint16_t pc, uint16_t target) { return branch(pc, !(S.p & FZ), target); }
inline bool bcs(uint16_t pc, uint16_t target) { return branch(pc, (S.p & FC) != 0, target); }
inline bool bcc(uint16_t pc, uint16_t target) { return branch(pc, !(S.p & FC), target); }
inline bool bmi(uint16_t pc, uint16_t target) { return branch(pc, (S.p & FN) != 0, target); }
inline bool bpl(uint16_t pc, uint16_t target) { return branch(pc, !(S.p & FN), target); }
inline bool bvs(uint16_t pc, uint16_t target) { return branch(pc, (S.p & FV) != 0, target); }
inline bool bvc(uint16_t pc, uint16_t target) { return branch(pc, !(S.p & FV), target); }
// JMP within the routine
inline void jmp_to(uint16_t target) { cyc(3); step(target); }

} // namespace op
} // namespace sound
