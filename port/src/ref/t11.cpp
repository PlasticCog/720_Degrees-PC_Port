// Reference DEC T-11 core, see t11.h.  Semantics mirror MAME's t11ops.hxx macros exactly,
// including the T-11 specific read-before-write bus sequence for MOV/MOVB/CLR/CLRB/SXT.
#include "t11.h"
#include "t11_table.inc"

namespace {
constexpr int CFLAG = 1, VFLAG = 2, ZFLAG = 4, NFLAG = 8;

struct IrqEntry { uint8_t priority; uint8_t vector; };
const IrqEntry irq_table[16] = {
	{ 0 << 5, 0000 }, { 4 << 5, 0070 }, { 4 << 5, 0064 }, { 4 << 5, 0060 },
	{ 5 << 5, 0134 }, { 5 << 5, 0130 }, { 5 << 5, 0124 }, { 5 << 5, 0120 },
	{ 6 << 5, 0114 }, { 6 << 5, 0110 }, { 6 << 5, 0104 }, { 6 << 5, 0100 },
	{ 7 << 5, 0154 }, { 7 << 5, 0150 }, { 7 << 5, 0144 }, { 7 << 5, 0140 },
};
}

void T11::reset(uint16_t initial_pc)
{
	m_initial_pc = initial_pc;
	r[6] = 0376;
	r[7] = initial_pc;
	psw = 0340;
	m_wait_state = false;
	m_trace_trap = false;
	m_check_irqs = false;
}

void T11::set_cp_state(uint8_t state)
{
	if (state != m_cp_state)
	{
		m_cp_state = state;
		m_check_irqs = true;
	}
}

void T11::take_interrupt(uint8_t vector)
{
	int new_pc = rword(vector);
	int new_psw = rword(vector + 2);
	if (irq_hook) irq_hook(hook_ctx, vector, new_pc);
	push(psw);
	push(r[7]);
	r[7] = new_pc;
	psw = new_psw;
	m_icount -= 114;
	m_wait_state = false;
}

void T11::check_irqs()
{
	const IrqEntry &irq = irq_table[m_cp_state & 15];
	if (irq.priority > (psw & 0340))
		take_interrupt(irq.vector);   // non-vectored: VEC line never asserted on System 2
}

void T11::trap_to(uint16_t vector)
{
	push(psw);
	push(r[7]);
	r[7] = rword(vector);
	psw = rword(vector + 2);
	m_check_irqs = true;
}

int T11::run(int cycles)
{
	m_icount = cycles;
	m_aborted = 0;
	check_irqs();
	if (m_wait_state)
	{
		m_icount = 0;
		total_cycles += cycles;
		return cycles;
	}
	while (m_icount > 0)
	{
		ppc = r[7];
		if (insn_hook)
			insn_hook(hook_ctx, ppc);
		execute_one();
		total_insns++;
		if (m_check_irqs || m_trace_trap || (psw & 0x10))
		{
			m_check_irqs = false;
			check_irqs();
		}
	}
	int used = cycles - m_icount - m_aborted;
	total_cycles += used;
	return used;
}

// effective address computation (MAKE_EAB_xx / MAKE_EAW_xx). 'opfetch' selects
// the ROPCODE() variant MAME uses for @#abs operands in source/dest macros.
int T11::ea_byte(int mode, int reg)
{
	int ea;
	switch (mode)
	{
	case 1: return r[reg];
	case 2: ea = r[reg]; r[reg] += (reg < 6 ? 1 : 2); return ea;
	case 3: ea = r[reg]; r[reg] += 2; return rword(ea);
	case 4: r[reg] -= (reg < 6 ? 1 : 2); return r[reg];
	case 5: r[reg] -= 2; ea = r[reg]; return rword(ea);
	case 6: ea = fetch(); return (ea + r[reg]) & 0xffff;
	default: ea = fetch(); ea = (ea + r[reg]) & 0xffff; return rword(ea);
	}
}

int T11::ea_word(int mode, int reg)
{
	int ea;
	switch (mode)
	{
	case 1: return r[reg];
	case 2: ea = r[reg]; r[reg] += 2; return ea;
	case 3: ea = r[reg]; r[reg] += 2; return rword(ea);
	case 4: r[reg] -= 2; return r[reg];
	case 5: r[reg] -= 2; ea = r[reg]; return rword(ea);
	case 6: ea = fetch(); return (ea + r[reg]) & 0xffff;
	default: ea = fetch(); ea = (ea + r[reg]) & 0xffff; return rword(ea);
	}
}

void T11::execute_one()
{
	const int op = fetch();
	const auto &ent = t11_table[op >> 3];
	const int sm = (op >> 9) & 7, sr = (op >> 6) & 7, dm = (op >> 3) & 7, dr = op & 7;
	int source = 0, dest = 0, result = 0, ea = 0;

	// operand helpers following MAME macro semantics
	auto get_sw = [&]() {
		if (sm == 0) source = r[sr];
		else if (sr == 7 && sm == 2) source = fetch();
		else if (sr == 7 && sm == 3) source = rword(fetch());
		else source = rword(ea_word(sm, sr));
	};
	auto get_sb = [&]() {
		if (sm == 0) source = r[sr] & 0xff;
		else if (sr == 7 && sm == 2) source = fetch();     // full word, as MAME does
		else if (sr == 7 && sm == 3) source = rbyte(fetch());
		else source = rbyte(ea_byte(sm, sr));
	};
	auto get_dw = [&]() {
		if (dm == 0) dest = r[dr];
		else if (dr == 7 && dm == 3) { ea = fetch(); dest = rword(ea); }
		else { ea = ea_word(dm, dr); dest = rword(ea); }
	};
	auto get_db = [&]() {
		if (dm == 0) dest = r[dr] & 0xff;
		else if (dr == 7 && dm == 3) { ea = fetch(); dest = rbyte(ea); }
		else { ea = ea_byte(dm, dr); dest = rbyte(ea); }
	};
	auto put_dw = [&](int v) { if (dm == 0) r[dr] = v; else wword(ea, v); };
	auto put_db = [&](int v) { if (dm == 0) r[dr] = (r[dr] & 0xff00) | (v & 0xff); else wbyte(ea, v); };
	// T-11 read-before-write store (PUT_DWT / PUT_DBT); register destination is a plain write
	auto put_dwt = [&](int v) {
		if (dm == 0) { r[dr] = v; return; }
		if (dr == 7 && dm == 3) ea = fetch(); else ea = ea_word(dm, dr);
		rword(ea); wword(ea, v);
	};
	auto put_dbt = [&](int v) {
		if (dr == 7 && dm == 3) ea = fetch(); else ea = ea_byte(dm, dr);
		rbyte(ea); wbyte(ea, v);
	};
	auto put_db_plain = [&](int v) {   // PUT_DB_xx (no read)
		if (dr == 7 && dm == 3) ea = fetch(); else ea = ea_byte(dm, dr);
		wbyte(ea, v);
	};

	auto clr_nzv = [&]() { psw &= ~(NFLAG | ZFLAG | VFLAG); };
	auto clr_nzvc = [&]() { psw &= ~(NFLAG | ZFLAG | VFLAG | CFLAG); };
	auto setb_nz = [&]() { psw |= (result >> 4) & 0x08; psw |= ((result & 0xff) == 0) << 2; };
	auto setw_nz = [&]() { psw |= (result >> 12) & 0x08; psw |= ((result & 0xffff) == 0) << 2; };
	auto setb_v = [&]() { psw |= ((source ^ dest ^ result ^ (result >> 1)) >> 6) & 0x02; };
	auto setw_v = [&]() { psw |= ((source ^ dest ^ result ^ (result >> 1)) >> 14) & 0x02; };
	auto setb_c = [&]() { psw |= (result >> 8) & 0x01; };
	auto setw_c = [&]() { psw |= (result >> 16) & 0x01; };
	auto shift_v = [&]() { psw |= ((psw << 1) ^ (psw >> 2)) & 2; };
	auto illegal = [&](uint16_t vec) { m_icount -= 48; trap_to(vec); };
	auto branch = [&](bool c) { if (c) r[7] += 2 * (int8_t)(op & 0xff); };
	const int N = (psw & NFLAG) != 0, Z = (psw & ZFLAG) != 0, V = (psw & VFLAG) != 0, C = psw & CFLAG;

	switch (ent.kind)
	{
	case K_OP_0000:
		switch (op & 0x3f)
		{
		case 0x00: // HALT
			m_icount -= 48;
			push(psw); push(r[7]);
			r[7] = m_initial_pc + 4; psw = 0340;
			m_check_irqs = true;
			break;
		case 0x01: m_icount = 0; m_wait_state = true; break;     // WAIT
		case 0x02: m_icount -= 24; r[7] = pop(); psw = pop(); if (psw & 0x10) m_trace_trap = true; m_check_irqs = true; break; // RTI
		case 0x03: m_icount -= 48; trap_to(014); break;           // BPT
		case 0x04: m_icount -= 48; trap_to(020); break;           // IOT
		case 0x05: m_icount -= 110; break;                         // RESET
		case 0x06: m_icount -= 33; r[7] = pop(); psw = pop(); m_check_irqs = true; break; // RTT
		case 0x07: r[0] = (r[0] & 0xff00) | 4; break;             // MFPT
		default: illegal(010); break;
		}
		return;
	case K_OP_0001: case K_MARK: case K_ILLEGAL: case K_MUL: case K_DIV: case K_ASH: case K_ASHC:
		illegal(010);
		return;
	case K_ILLEGAL4:
		illegal(004);
		return;
	default:
		break;
	}

	m_icount -= ent.cycles;

	switch (ent.kind)
	{
	// ---- double operand, word
	case K_MOV:
		get_sw(); clr_nzv(); result = source; setw_nz(); put_dwt(result); break;
	case K_CMP:
		get_sw(); get_dw(); clr_nzvc(); result = source - dest; setw_nz(); setw_v(); setw_c(); break;
	case K_BIT:
		get_sw(); get_dw(); clr_nzv(); result = dest & source; setw_nz(); break;
	case K_BIC:
		get_sw(); get_dw(); clr_nzv(); result = dest & ~source; setw_nz(); put_dw(result); break;
	case K_BIS:
		get_sw(); get_dw(); clr_nzv(); result = dest | source; setw_nz(); put_dw(result); break;
	case K_ADD:
		get_sw(); get_dw(); clr_nzvc(); result = dest + source; setw_nz(); setw_v(); setw_c(); put_dw(result); break;
	case K_SUB:
		get_sw(); get_dw(); clr_nzvc(); result = dest - source; setw_nz(); setw_v(); setw_c(); put_dw(result); break;
	// ---- double operand, byte
	case K_MOVB:
		get_sb(); clr_nzv(); result = source; setb_nz();
		if (dm == 0) r[dr] = (uint16_t)(int16_t)(int8_t)result;
		else put_dbt(result);
		break;
	case K_CMPB:
		get_sb(); get_db(); clr_nzvc(); result = source - dest; setb_nz(); setb_v(); setb_c(); break;
	case K_BITB:
		get_sb(); get_db(); clr_nzv(); result = dest & source; setb_nz(); break;
	case K_BICB:
		get_sb(); get_db(); clr_nzv(); result = dest & ~source; setb_nz(); put_db(result); break;
	case K_BISB:
		get_sb(); get_db(); clr_nzv(); result = dest | source; setb_nz(); put_db(result); break;
	case K_XOR:
		source = r[sr]; get_dw(); clr_nzv(); result = dest ^ source; setw_nz(); put_dw(result); break;

	// ---- single operand, word
	case K_CLR:
		if (dm == 0) r[dr] = 0; else put_dwt(0);
		clr_nzvc(); psw |= ZFLAG; break;
	case K_COM:
		get_dw(); clr_nzvc(); result = ~dest; setw_nz(); psw |= CFLAG; put_dw(result); break;
	case K_INC:
		get_dw(); clr_nzv(); result = dest + 1; setw_nz(); if (dest == 0x7fff) psw |= VFLAG; put_dw(result); break;
	case K_DEC:
		get_dw(); clr_nzv(); result = dest - 1; setw_nz(); if (dest == 0x8000) psw |= VFLAG; put_dw(result); break;
	case K_NEG:
		get_dw(); clr_nzvc(); result = -dest; setw_nz(); if (dest == 0x8000) psw |= VFLAG; if (result) psw |= CFLAG; put_dw(result); break;
	case K_ADC:
		source = C; get_dw(); clr_nzvc(); result = dest + source; setw_nz(); setw_v(); setw_c(); put_dw(result); break;
	case K_SBC:
		source = C; get_dw(); clr_nzvc(); result = dest - source; setw_nz(); setw_v(); setw_c(); put_dw(result); break;
	case K_TST:
		get_dw(); clr_nzvc(); result = dest; setw_nz(); break;
	case K_ROR:
		get_dw(); result = (dest >> 1) | (C << 15); clr_nzvc(); setw_nz(); psw |= dest & 1; shift_v(); put_dw(result); break;
	case K_ROL:
		get_dw(); result = (dest << 1) | C; clr_nzvc(); setw_nz(); psw |= (dest >> 15) & 1; shift_v(); put_dw(result); break;
	case K_ASR:
		get_dw(); clr_nzvc(); result = (dest >> 1) | (dest & 0x8000); setw_nz(); psw |= dest & 1; shift_v(); put_dw(result); break;
	case K_ASL:
		get_dw(); clr_nzvc(); result = dest << 1; setw_nz(); psw |= (dest >> 15) & 1; shift_v(); put_dw(result); break;
	case K_SWAB:
		get_dw(); clr_nzvc(); result = ((dest >> 8) & 0xff) + (dest << 8); setb_nz(); put_dw(result); break;
	case K_SXT:
		psw &= ~(ZFLAG | VFLAG);
		if (N) result = -1; else { result = 0; psw |= ZFLAG; }
		if (dm == 0) r[dr] = result; else put_dwt(result);
		break;

	// ---- single operand, byte
	case K_CLRB:
		if (dm == 0) r[dr] &= 0xff00; else put_dbt(0);
		clr_nzvc(); psw |= ZFLAG; break;
	case K_COMB:
		get_db(); clr_nzvc(); result = ~dest; setb_nz(); psw |= CFLAG; put_db(result); break;
	case K_INCB:
		get_db(); clr_nzv(); result = dest + 1; setb_nz(); if (dest == 0x7f) psw |= VFLAG; put_db(result); break;
	case K_DECB:
		get_db(); clr_nzv(); result = dest - 1; setb_nz(); if (dest == 0x80) psw |= VFLAG; put_db(result); break;
	case K_NEGB:
		get_db(); clr_nzvc(); result = -dest; setb_nz(); if (dest == 0x80) psw |= VFLAG; if (result) psw |= CFLAG; put_db(result); break;
	case K_ADCB:
		source = C; get_db(); clr_nzvc(); result = dest + source; setb_nz(); setb_v(); setb_c(); put_db(result); break;
	case K_SBCB:
		source = C; get_db(); clr_nzvc(); result = dest - source; setb_nz(); setb_v(); setb_c(); put_db(result); break;
	case K_TSTB:
		get_db(); clr_nzvc(); result = dest; setb_nz(); break;
	case K_RORB:
		get_db(); result = (dest >> 1) | (C << 7); clr_nzvc(); setb_nz(); psw |= dest & 1; shift_v(); put_db(result); break;
	case K_ROLB:
		get_db(); result = (dest << 1) | C; clr_nzvc(); setb_nz(); psw |= (dest >> 7) & 1; shift_v(); put_db(result); break;
	case K_ASRB:
		get_db(); clr_nzvc(); result = (dest >> 1) | (dest & 0x80); setb_nz(); psw |= dest & 1; shift_v(); put_db(result); break;
	case K_ASLB:
		get_db(); clr_nzvc(); result = dest << 1; setb_nz(); psw |= (dest >> 7) & 1; shift_v(); put_db(result); break;
	case K_MTPS:
		get_db(); psw = (psw & ~0xef) | (dest & 0xef); m_check_irqs = true; break;
	case K_MFPS:
		result = psw; clr_nzv(); setb_nz();
		if (dm == 0) r[dr] = (uint16_t)(int16_t)(int8_t)result;
		else put_db_plain(result);
		break;

	// ---- control flow
	case K_JMP:
		r[7] = ea_word(dm, dr); break;
	case K_JSR:
		ea = ea_word(dm, dr); push(r[sr]); r[sr] = r[7]; r[7] = ea; break;
	case K_RTS:
		r[7] = r[dr]; r[dr] = pop(); break;
	case K_CCC: psw &= ~(op & 15); break;
	case K_SCC: psw |= (op & 15); break;
	case K_BR:   branch(true); break;
	case K_BNE:  branch(!Z); break;
	case K_BEQ:  branch(Z); break;
	case K_BGE:  branch(!(N ^ V)); break;
	case K_BLT:  branch(N ^ V); break;
	case K_BGT:  branch(!Z && !(N ^ V)); break;
	case K_BLE:  branch(Z || (N ^ V)); break;
	case K_BPL:  branch(!N); break;
	case K_BMI:  branch(N); break;
	case K_BHI:  branch(!C && !Z); break;
	case K_BLOS: branch(C || Z); break;
	case K_BVC:  branch(!V); break;
	case K_BVS:  branch(V); break;
	case K_BCC:  branch(!C); break;
	case K_BCS:  branch(C); break;
	case K_SOB:
		source = r[sr] - 1; r[sr] = source;
		if (source) r[7] -= 2 * (op & 0x3f);
		break;
	case K_EMT:  trap_to(030); break;
	case K_TRAP: trap_to(034); break;
	default:
		illegal(010);
		break;
	}
}
