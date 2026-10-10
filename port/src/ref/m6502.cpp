// Reference NMOS 6502 core.  Documented opcodes with standard cycle counts
// (page-crossing penalties included).  Decimal mode implemented per NMOS behaviour.
#include "m6502.h"

namespace {
enum { FC = 0x01, FZ = 0x02, FI = 0x04, FD = 0x08, FB = 0x10, FU = 0x20, FV = 0x40, FN = 0x80 };
const uint8_t base_cycles[256] = {
	7,6,2,8,3,3,5,5,3,2,2,2,4,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
	6,6,2,8,3,3,5,5,4,2,2,2,4,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
	6,6,2,8,3,3,5,5,3,2,2,2,3,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
	6,6,2,8,3,3,5,5,4,2,2,2,5,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
	2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4, 2,6,2,6,4,4,4,4,2,5,2,5,5,5,5,5,
	2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4, 2,5,2,5,4,4,4,4,2,4,2,4,4,4,4,4,
	2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
	2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
};
}

void M6502::reset()
{
	a = x = y = 0;
	s = 0xfd;
	p = FI | FU;
	pc = rd16(0xfffc);
	m_nmi_pending = false;
}

void M6502::interrupt(uint16_t vector, bool brk)
{
	push(pc >> 8);
	push(pc & 0xff);
	push((p & ~FB) | FU | (brk ? FB : 0));
	p |= FI;
	pc = rd16(vector);
	m_cycles += 7;
}

int M6502::run(int cycles)
{
	m_cycles = 0;
	if (m_halted)
	{
		total_cycles += cycles;
		return cycles;
	}
	while (m_cycles < cycles)
	{
		if (m_nmi_pending)
		{
			m_nmi_pending = false;
			interrupt(0xfffa, false);
			continue;
		}
		if (m_irq && !(p & FI))
		{
			interrupt(0xfffe, false);
			continue;
		}
		if (insn_hook)
			insn_hook(hook_ctx, pc);
		step();
	}
	total_cycles += m_cycles;
	return m_cycles;
}

void M6502::step()
{
	const uint8_t op = rd(pc++);
	m_cycles += base_cycles[op];
	uint16_t ad = 0;
	uint8_t v;

	auto setnz = [&](uint8_t r) { p = (p & ~(FN | FZ)) | (r & FN) | (r ? 0 : FZ); };
	auto imm = [&]() { ad = pc++; };
	auto zp = [&]() { ad = rd(pc++); };
	auto zpx = [&]() { ad = (rd(pc++) + x) & 0xff; };
	auto zpy = [&]() { ad = (rd(pc++) + y) & 0xff; };
	auto abs_ = [&]() { ad = rd16(pc); pc += 2; };
	auto absx = [&](bool pen) { uint16_t b = rd16(pc); pc += 2; ad = b + x; if (pen && ((b ^ ad) & 0xff00)) m_cycles++; };
	auto absy = [&](bool pen) { uint16_t b = rd16(pc); pc += 2; ad = b + y; if (pen && ((b ^ ad) & 0xff00)) m_cycles++; };
	auto indx = [&]() { uint8_t z = rd(pc++) + x; ad = rd(z) | (rd(uint8_t(z + 1)) << 8); };
	auto indy = [&](bool pen) { uint8_t z = rd(pc++); uint16_t b = rd(z) | (rd(uint8_t(z + 1)) << 8); ad = b + y; if (pen && ((b ^ ad) & 0xff00)) m_cycles++; };
	auto branch = [&](bool c) {
		int8_t off = (int8_t)rd(pc++);
		if (c) { uint16_t n = pc + off; m_cycles += 1 + (((n ^ pc) & 0xff00) ? 1 : 0); pc = n; }
	};
	auto adc = [&](uint8_t m) {
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
			a = (lo & 0x0f) + (hi & 0xf0);
		}
		else
		{
			int sum = a + m + (p & FC);
			p &= ~(FV | FC);
			if (~(a ^ m) & (a ^ sum) & 0x80) p |= FV;
			if (sum & 0x100) p |= FC;
			a = sum;
			setnz(a);
		}
	};
	auto sbc = [&](uint8_t m) {
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
			a = (lo & 0x0f) | (hi & 0xf0);
		}
		else
		{
			int diff = a - m - ((p & FC) ? 0 : 1);
			p &= ~(FV | FC);
			if ((a ^ m) & (a ^ diff) & 0x80) p |= FV;
			if (!(diff & 0x100)) p |= FC;
			a = diff;
			setnz(a);
		}
	};
	auto cmp = [&](uint8_t r, uint8_t m) { int d = r - m; p = (p & ~FC) | (d >= 0 ? FC : 0); setnz(uint8_t(d)); };
	auto asl = [&](uint8_t m) { p = (p & ~FC) | (m >> 7); m <<= 1; setnz(m); return m; };
	auto lsr = [&](uint8_t m) { p = (p & ~FC) | (m & 1); m >>= 1; setnz(m); return m; };
	auto rol = [&](uint8_t m) { uint8_t c = p & FC; p = (p & ~FC) | (m >> 7); m = (m << 1) | c; setnz(m); return m; };
	auto ror = [&](uint8_t m) { uint8_t c = p & FC; p = (p & ~FC) | (m & 1); m = (m >> 1) | (c << 7); setnz(m); return m; };
	auto bit = [&](uint8_t m) { p = (p & ~(FN | FV | FZ)) | (m & (FN | FV)) | ((a & m) ? 0 : FZ); };
	auto rmw = [&](uint8_t (*f)(M6502 &, uint8_t)) { (void)f; };
	(void)rmw;

	switch (op)
	{
	// LDA
	case 0xa9: imm(); a = rd(ad); setnz(a); break;
	case 0xa5: zp(); a = rd(ad); setnz(a); break;
	case 0xb5: zpx(); a = rd(ad); setnz(a); break;
	case 0xad: abs_(); a = rd(ad); setnz(a); break;
	case 0xbd: absx(true); a = rd(ad); setnz(a); break;
	case 0xb9: absy(true); a = rd(ad); setnz(a); break;
	case 0xa1: indx(); a = rd(ad); setnz(a); break;
	case 0xb1: indy(true); a = rd(ad); setnz(a); break;
	// LDX
	case 0xa2: imm(); x = rd(ad); setnz(x); break;
	case 0xa6: zp(); x = rd(ad); setnz(x); break;
	case 0xb6: zpy(); x = rd(ad); setnz(x); break;
	case 0xae: abs_(); x = rd(ad); setnz(x); break;
	case 0xbe: absy(true); x = rd(ad); setnz(x); break;
	// LDY
	case 0xa0: imm(); y = rd(ad); setnz(y); break;
	case 0xa4: zp(); y = rd(ad); setnz(y); break;
	case 0xb4: zpx(); y = rd(ad); setnz(y); break;
	case 0xac: abs_(); y = rd(ad); setnz(y); break;
	case 0xbc: absx(true); y = rd(ad); setnz(y); break;
	// STA
	case 0x85: zp(); wr(ad, a); break;
	case 0x95: zpx(); wr(ad, a); break;
	case 0x8d: abs_(); wr(ad, a); break;
	case 0x9d: absx(false); wr(ad, a); break;
	case 0x99: absy(false); wr(ad, a); break;
	case 0x81: indx(); wr(ad, a); break;
	case 0x91: indy(false); wr(ad, a); break;
	// STX / STY
	case 0x86: zp(); wr(ad, x); break;
	case 0x96: zpy(); wr(ad, x); break;
	case 0x8e: abs_(); wr(ad, x); break;
	case 0x84: zp(); wr(ad, y); break;
	case 0x94: zpx(); wr(ad, y); break;
	case 0x8c: abs_(); wr(ad, y); break;
	// transfers
	case 0xaa: x = a; setnz(x); break;
	case 0xa8: y = a; setnz(y); break;
	case 0x8a: a = x; setnz(a); break;
	case 0x98: a = y; setnz(a); break;
	case 0xba: x = s; setnz(x); break;
	case 0x9a: s = x; break;
	// stack
	case 0x48: push(a); break;
	case 0x68: a = pull(); setnz(a); break;
	case 0x08: push(p | FB | FU); break;
	case 0x28: p = pull() | FU; p &= ~FB; break;
	// logic
	case 0x29: imm(); a &= rd(ad); setnz(a); break;
	case 0x25: zp(); a &= rd(ad); setnz(a); break;
	case 0x35: zpx(); a &= rd(ad); setnz(a); break;
	case 0x2d: abs_(); a &= rd(ad); setnz(a); break;
	case 0x3d: absx(true); a &= rd(ad); setnz(a); break;
	case 0x39: absy(true); a &= rd(ad); setnz(a); break;
	case 0x21: indx(); a &= rd(ad); setnz(a); break;
	case 0x31: indy(true); a &= rd(ad); setnz(a); break;
	case 0x09: imm(); a |= rd(ad); setnz(a); break;
	case 0x05: zp(); a |= rd(ad); setnz(a); break;
	case 0x15: zpx(); a |= rd(ad); setnz(a); break;
	case 0x0d: abs_(); a |= rd(ad); setnz(a); break;
	case 0x1d: absx(true); a |= rd(ad); setnz(a); break;
	case 0x19: absy(true); a |= rd(ad); setnz(a); break;
	case 0x01: indx(); a |= rd(ad); setnz(a); break;
	case 0x11: indy(true); a |= rd(ad); setnz(a); break;
	case 0x49: imm(); a ^= rd(ad); setnz(a); break;
	case 0x45: zp(); a ^= rd(ad); setnz(a); break;
	case 0x55: zpx(); a ^= rd(ad); setnz(a); break;
	case 0x4d: abs_(); a ^= rd(ad); setnz(a); break;
	case 0x5d: absx(true); a ^= rd(ad); setnz(a); break;
	case 0x59: absy(true); a ^= rd(ad); setnz(a); break;
	case 0x41: indx(); a ^= rd(ad); setnz(a); break;
	case 0x51: indy(true); a ^= rd(ad); setnz(a); break;
	case 0x24: zp(); bit(rd(ad)); break;
	case 0x2c: abs_(); bit(rd(ad)); break;
	// arithmetic
	case 0x69: imm(); adc(rd(ad)); break;
	case 0x65: zp(); adc(rd(ad)); break;
	case 0x75: zpx(); adc(rd(ad)); break;
	case 0x6d: abs_(); adc(rd(ad)); break;
	case 0x7d: absx(true); adc(rd(ad)); break;
	case 0x79: absy(true); adc(rd(ad)); break;
	case 0x61: indx(); adc(rd(ad)); break;
	case 0x71: indy(true); adc(rd(ad)); break;
	case 0xe9: imm(); sbc(rd(ad)); break;
	case 0xe5: zp(); sbc(rd(ad)); break;
	case 0xf5: zpx(); sbc(rd(ad)); break;
	case 0xed: abs_(); sbc(rd(ad)); break;
	case 0xfd: absx(true); sbc(rd(ad)); break;
	case 0xf9: absy(true); sbc(rd(ad)); break;
	case 0xe1: indx(); sbc(rd(ad)); break;
	case 0xf1: indy(true); sbc(rd(ad)); break;
	case 0xc9: imm(); cmp(a, rd(ad)); break;
	case 0xc5: zp(); cmp(a, rd(ad)); break;
	case 0xd5: zpx(); cmp(a, rd(ad)); break;
	case 0xcd: abs_(); cmp(a, rd(ad)); break;
	case 0xdd: absx(true); cmp(a, rd(ad)); break;
	case 0xd9: absy(true); cmp(a, rd(ad)); break;
	case 0xc1: indx(); cmp(a, rd(ad)); break;
	case 0xd1: indy(true); cmp(a, rd(ad)); break;
	case 0xe0: imm(); cmp(x, rd(ad)); break;
	case 0xe4: zp(); cmp(x, rd(ad)); break;
	case 0xec: abs_(); cmp(x, rd(ad)); break;
	case 0xc0: imm(); cmp(y, rd(ad)); break;
	case 0xc4: zp(); cmp(y, rd(ad)); break;
	case 0xcc: abs_(); cmp(y, rd(ad)); break;
	// inc/dec
	case 0xe6: zp(); v = rd(ad) + 1; wr(ad, v); setnz(v); break;
	case 0xf6: zpx(); v = rd(ad) + 1; wr(ad, v); setnz(v); break;
	case 0xee: abs_(); v = rd(ad) + 1; wr(ad, v); setnz(v); break;
	case 0xfe: absx(false); v = rd(ad) + 1; wr(ad, v); setnz(v); break;
	case 0xc6: zp(); v = rd(ad) - 1; wr(ad, v); setnz(v); break;
	case 0xd6: zpx(); v = rd(ad) - 1; wr(ad, v); setnz(v); break;
	case 0xce: abs_(); v = rd(ad) - 1; wr(ad, v); setnz(v); break;
	case 0xde: absx(false); v = rd(ad) - 1; wr(ad, v); setnz(v); break;
	case 0xe8: x++; setnz(x); break;
	case 0xc8: y++; setnz(y); break;
	case 0xca: x--; setnz(x); break;
	case 0x88: y--; setnz(y); break;
	// shifts
	case 0x0a: a = asl(a); break;
	case 0x06: zp(); wr(ad, asl(rd(ad))); break;
	case 0x16: zpx(); wr(ad, asl(rd(ad))); break;
	case 0x0e: abs_(); wr(ad, asl(rd(ad))); break;
	case 0x1e: absx(false); wr(ad, asl(rd(ad))); break;
	case 0x4a: a = lsr(a); break;
	case 0x46: zp(); wr(ad, lsr(rd(ad))); break;
	case 0x56: zpx(); wr(ad, lsr(rd(ad))); break;
	case 0x4e: abs_(); wr(ad, lsr(rd(ad))); break;
	case 0x5e: absx(false); wr(ad, lsr(rd(ad))); break;
	case 0x2a: a = rol(a); break;
	case 0x26: zp(); wr(ad, rol(rd(ad))); break;
	case 0x36: zpx(); wr(ad, rol(rd(ad))); break;
	case 0x2e: abs_(); wr(ad, rol(rd(ad))); break;
	case 0x3e: absx(false); wr(ad, rol(rd(ad))); break;
	case 0x6a: a = ror(a); break;
	case 0x66: zp(); wr(ad, ror(rd(ad))); break;
	case 0x76: zpx(); wr(ad, ror(rd(ad))); break;
	case 0x6e: abs_(); wr(ad, ror(rd(ad))); break;
	case 0x7e: absx(false); wr(ad, ror(rd(ad))); break;
	// jumps
	case 0x4c: pc = rd16(pc); break;
	case 0x6c: { uint16_t t = rd16(pc); pc = rd(t) | (rd((t & 0xff00) | ((t + 1) & 0xff)) << 8); break; }
	case 0x20: { uint16_t t = rd16(pc); pc++; push(pc >> 8); push(pc & 0xff); pc = t; break; }
	case 0x60: pc = pull(); pc |= pull() << 8; pc++; break;
	case 0x40: p = (pull() | FU) & ~FB; pc = pull(); pc |= pull() << 8; break;
	case 0x00: pc++; interrupt(0xfffe, true); m_cycles -= 7; break;
	// branches
	case 0x10: branch(!(p & FN)); break;
	case 0x30: branch(p & FN); break;
	case 0x50: branch(!(p & FV)); break;
	case 0x70: branch(p & FV); break;
	case 0x90: branch(!(p & FC)); break;
	case 0xb0: branch(p & FC); break;
	case 0xd0: branch(!(p & FZ)); break;
	case 0xf0: branch(p & FZ); break;
	// flags
	case 0x18: p &= ~FC; break;
	case 0x38: p |= FC; break;
	case 0x58: p &= ~FI; break;
	case 0x78: p |= FI; break;
	case 0xb8: p &= ~FV; break;
	case 0xd8: p &= ~FD; break;
	case 0xf8: p |= FD; break;
	case 0xea: break;
	default:
		// undocumented opcode: treat as 1-byte NOP (should not occur in the 720 sound program)
		break;
	}
}
