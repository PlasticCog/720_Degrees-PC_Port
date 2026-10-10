"""DEC T-11 (PDP-11 subset) disassembler used for reverse engineering 720 Degrees.

Produces structured instruction objects so the analysis / decompilation tools can
reason about control flow and operands.  Text output uses hex numbers.
"""

REGN = ['R0', 'R1', 'R2', 'R3', 'R4', 'R5', 'SP', 'PC']


class Operand:
    __slots__ = ('mode', 'reg', 'ext', 'addr', 'kind')

    def __init__(self, mode, reg, ext=None, addr=None):
        self.mode, self.reg, self.ext, self.addr = mode, reg, ext, addr
        # kind: 'reg','imm','abs','rel','reldef', or 'mem' (generic register based)
        if reg == 7 and mode == 2:
            self.kind = 'imm'
        elif reg == 7 and mode == 3:
            self.kind = 'abs'
        elif reg == 7 and mode == 6:
            self.kind = 'rel'
        elif reg == 7 and mode == 7:
            self.kind = 'reldef'
        elif mode == 0:
            self.kind = 'reg'
        else:
            self.kind = 'mem'

    def text(self, sym=None):
        r = REGN[self.reg]
        h = (lambda v: sym(v)) if sym else (lambda v: '0x%04X' % v)
        k = self.kind
        if k == 'imm':
            return '#0x%04X' % self.ext
        if k == 'abs':
            return '@#' + h(self.ext)
        if k == 'rel':
            return h(self.addr)
        if k == 'reldef':
            return '@' + h(self.addr)
        m = self.mode
        if m == 0:
            return r
        if m == 1:
            return '(%s)' % r
        if m == 2:
            return '(%s)+' % r
        if m == 3:
            return '@(%s)+' % r
        if m == 4:
            return '-(%s)' % r
        if m == 5:
            return '@-(%s)' % r
        x = self.ext
        if x >= 0xFF00:
            xs = '-0x%X' % (0x10000 - x)
        elif sym and x >= 0x100:
            xs = sym(x)
        else:
            xs = '0x%X' % x
        if m == 6:
            return '%s(%s)' % (xs, r)
        return '@%s(%s)' % (xs, r)


class Insn:
    __slots__ = ('addr', 'op', 'size', 'mn', 'ops', 'target', 'flow', 'byte', 'words')

    def __init__(self, addr, op):
        self.addr = addr
        self.op = op
        self.size = 2
        self.mn = '?'
        self.ops = []
        self.target = None   # static branch/jump/call target
        # flow: 'seq','br','cbr','jmp','ijmp','call','icall','ret','rti','trap','halt','wait','illegal','sob'
        self.flow = 'seq'
        self.byte = False
        self.words = [op]

    def text(self, sym=None):
        s = self.mn
        if self.ops:
            parts = []
            for o in self.ops:
                if isinstance(o, str):
                    if o.startswith('L') and sym and len(o) == 5:
                        parts.append(sym(int(o[1:], 16)))
                    else:
                        parts.append(o)
                else:
                    parts.append(o.text(sym))
            s += ' ' * max(1, 8 - len(self.mn)) + ','.join(parts)
        return s


DOUBLE = {1: 'MOV', 2: 'CMP', 3: 'BIT', 4: 'BIC', 5: 'BIS', 6: 'ADD',
          9: 'MOVB', 10: 'CMPB', 11: 'BITB', 12: 'BICB', 13: 'BISB', 14: 'SUB'}
SINGLE = {0o0050: 'CLR', 0o0051: 'COM', 0o0052: 'INC', 0o0053: 'DEC', 0o0054: 'NEG', 0o0055: 'ADC',
          0o0056: 'SBC', 0o0057: 'TST', 0o0060: 'ROR', 0o0061: 'ROL', 0o0062: 'ASR', 0o0063: 'ASL',
          0o0067: 'SXT', 0o0003: 'SWAB',
          0o1050: 'CLRB', 0o1051: 'COMB', 0o1052: 'INCB', 0o1053: 'DECB', 0o1054: 'NEGB', 0o1055: 'ADCB',
          0o1056: 'SBCB', 0o1057: 'TSTB', 0o1060: 'RORB', 0o1061: 'ROLB', 0o1062: 'ASRB', 0o1063: 'ASLB',
          0o1064: 'MTPS', 0o1067: 'MFPS'}
BRANCH = {0o000400: 'BR', 0o001000: 'BNE', 0o001400: 'BEQ', 0o002000: 'BGE', 0o002400: 'BLT',
          0o003000: 'BGT', 0o003400: 'BLE', 0o100000: 'BPL', 0o100400: 'BMI', 0o101000: 'BHI',
          0o101400: 'BLOS', 0o102000: 'BVC', 0o102400: 'BVS', 0o103000: 'BCC', 0o103400: 'BCS'}
WRITES_DEST = {'MOV', 'MOVB', 'BIC', 'BIS', 'ADD', 'SUB', 'BICB', 'BISB', 'CLR', 'COM', 'INC', 'DEC',
               'NEG', 'ADC', 'SBC', 'ROR', 'ROL', 'ASR', 'ASL', 'SXT', 'SWAB', 'CLRB', 'COMB', 'INCB',
               'DECB', 'NEGB', 'ADCB', 'SBCB', 'RORB', 'ROLB', 'ASRB', 'ASLB', 'MFPS', 'XOR'}


def decode(rw, addr):
    """rw(a) returns the 16-bit word at address a (a even)."""
    op = rw(addr)
    ins = Insn(addr, op)
    pc = [(addr + 2) & 0xffff]

    def operand(mode, reg):
        if reg == 7 and mode in (2, 3):
            ext = rw(pc[0])
            ins.words.append(ext)
            pc[0] = (pc[0] + 2) & 0xffff
            return Operand(mode, reg, ext)
        if mode in (6, 7):
            ext = rw(pc[0])
            ins.words.append(ext)
            pc[0] = (pc[0] + 2) & 0xffff
            a = (ext + pc[0]) & 0xffff if reg == 7 else None
            return Operand(mode, reg, ext, a)
        return Operand(mode, reg)

    def done():
        ins.size = (pc[0] - addr) & 0xffff
        return ins

    key = (op >> 12) & 0o17
    if key in DOUBLE:
        ins.mn = DOUBLE[key]
        ins.byte = key >= 9 and key != 14
        s = operand((op >> 9) & 7, (op >> 6) & 7)
        d = operand((op >> 3) & 7, op & 7)
        ins.ops = [s, d]
        if ins.mn in WRITES_DEST and d.mode == 0 and d.reg == 7:
            ins.flow = 'ijmp'
        return done()
    if (op & 0o177000) == 0o074000:
        ins.mn = 'XOR'
        d = operand((op >> 3) & 7, op & 7)
        ins.ops = [REGN[(op >> 6) & 7], d]
        if d.mode == 0 and d.reg == 7:
            ins.flow = 'ijmp'
        return done()
    if (op & 0o177000) == 0o077000:
        ins.mn = 'SOB'
        r = (op >> 6) & 7
        ins.target = (addr + 2 - 2 * (op & 0o77)) & 0xffff
        ins.ops = [REGN[r], 'L%04X' % ins.target]
        ins.flow = 'sob'
        return ins
    if (op & 0o177000) == 0o004000:
        r = (op >> 6) & 7
        d = operand((op >> 3) & 7, op & 7)
        ins.mn = 'JSR'
        ins.ops = [REGN[r], d]
        if d.mode == 0:
            ins.mn = 'ILL4'
            ins.flow = 'illegal'
        elif d.kind == 'rel':
            ins.target = d.addr
            ins.flow = 'call'
        elif d.kind == 'abs':
            ins.target = d.ext
            ins.flow = 'call'
        else:
            ins.flow = 'icall'
        return done()
    if (op & 0o177700) == 0o000100:
        d = operand((op >> 3) & 7, op & 7)
        ins.mn = 'JMP'
        ins.ops = [d]
        if d.mode == 0:
            ins.mn = 'ILL4'
            ins.flow = 'illegal'
        elif d.kind == 'rel':
            ins.target = d.addr
            ins.flow = 'jmp'
        elif d.kind == 'abs':
            ins.target = d.ext
            ins.flow = 'jmp'
        else:
            ins.flow = 'ijmp'
        return done()
    if (op & 0o177770) == 0o000200:
        ins.mn = 'RTS'
        ins.ops = [REGN[op & 7]]
        ins.flow = 'ret'
        return ins
    if (op & 0o177740) == 0o000240:
        bits = op & 0o17
        setc = op & 0o20
        if bits == 0:
            ins.mn = 'NOP'
        else:
            names = ''.join(n for n, b in (('C', 1), ('V', 2), ('Z', 4), ('N', 8)) if bits & b)
            if len(names) == 1:
                ins.mn = ('SE' if setc else 'CL') + names
            else:
                ins.mn = ('SCC' if setc else 'CCC')
                if bits != 15:
                    ins.ops = [names]
        return ins
    if op < 8:
        ins.mn = ['HALT', 'WAIT', 'RTI', 'BPT', 'IOT', 'RESET', 'RTT', 'MFPT'][op]
        ins.flow = {0: 'halt', 1: 'wait', 2: 'rti', 3: 'trap', 4: 'trap', 6: 'rti'}.get(op, 'seq')
        return ins
    bkey = op & 0o177400
    if bkey in BRANCH:
        ins.mn = BRANCH[bkey]
        off = op & 0xff
        if off & 0x80:
            off -= 0x100
        ins.target = (addr + 2 + 2 * off) & 0xffff
        ins.ops = ['L%04X' % ins.target]
        ins.flow = 'br' if ins.mn == 'BR' else 'cbr'
        return ins
    if bkey == 0o104000:
        ins.mn = 'EMT'
        ins.ops = ['0x%02X' % (op & 0xff)]
        ins.flow = 'trap'
        return ins
    if bkey == 0o104400:
        ins.mn = 'TRAP'
        ins.ops = ['0x%02X' % (op & 0xff)]
        ins.flow = 'trap'
        return ins
    sk = op >> 6
    if sk in SINGLE:
        ins.mn = SINGLE[sk]
        ins.byte = sk >= 0o1000
        d = operand((op >> 3) & 7, op & 7)
        ins.ops = [d]
        if d.mode == 0 and d.reg == 7 and ins.mn not in ('TST', 'TSTB', 'MTPS'):
            ins.flow = 'ijmp'
        return done()
    ins.mn = '.WORD'
    ins.ops = ['0x%04X' % op]
    ins.flow = 'illegal'
    return ins
