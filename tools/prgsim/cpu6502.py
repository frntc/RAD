# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""
A cycle-inexact NMOS 6502 core, enough to run a C64 .prg on a PC.

Written for one job: desk-checking the gpu64 API conformance suite
(tests/gpu64_test_*.prg) against a reference model of the API,
so a test that asserts the wrong value is found on a PC and not on the
bench. Bench access is the scarce resource; this is not.

Scope, deliberately:
  - the documented NMOS instruction set, no undocumented opcodes
  - no cycle counting, no IRQ/NMI, no VIC, no CIA
  - memory is a flat 64K bytearray with a hook for the IO2 window

Anything the suite does not need is missing on purpose.
"""

FLAG_C = 0x01
FLAG_Z = 0x02
FLAG_I = 0x04
FLAG_D = 0x08
FLAG_B = 0x10
FLAG_U = 0x20
FLAG_V = 0x40
FLAG_N = 0x80


class Cpu6502:
    def __init__(self, read, write):
        self.read = read
        self.write = write
        self.a = self.x = self.y = 0
        self.sp = 0xFD
        self.pc = 0
        self.p = FLAG_U | FLAG_I
        self.cycles = 0
        self.halted = False

    # --- helpers -------------------------------------------------------
    def rd(self, addr):
        return self.read(addr & 0xFFFF) & 0xFF

    def wr(self, addr, val):
        self.write(addr & 0xFFFF, val & 0xFF)

    def rd16(self, addr):
        return self.rd(addr) | (self.rd(addr + 1) << 8)

    def rd16zp(self, addr):
        # zero page wraps within the page
        return self.rd(addr & 0xFF) | (self.rd((addr + 1) & 0xFF) << 8)

    def push(self, v):
        self.wr(0x100 + self.sp, v)
        self.sp = (self.sp - 1) & 0xFF

    def pop(self):
        self.sp = (self.sp + 1) & 0xFF
        return self.rd(0x100 + self.sp)

    def setzn(self, v):
        v &= 0xFF
        self.p = (self.p & ~(FLAG_Z | FLAG_N)) | (FLAG_Z if v == 0 else 0) | (v & FLAG_N)
        return v

    def setflag(self, mask, on):
        if on:
            self.p |= mask
        else:
            self.p &= ~mask

    # --- addressing ----------------------------------------------------
    def am_imm(self):
        a = self.pc
        self.pc = (self.pc + 1) & 0xFFFF
        return a

    def am_zp(self):
        a = self.rd(self.pc)
        self.pc = (self.pc + 1) & 0xFFFF
        return a

    def am_zpx(self):
        a = (self.rd(self.pc) + self.x) & 0xFF
        self.pc = (self.pc + 1) & 0xFFFF
        return a

    def am_zpy(self):
        a = (self.rd(self.pc) + self.y) & 0xFF
        self.pc = (self.pc + 1) & 0xFFFF
        return a

    def am_abs(self):
        a = self.rd16(self.pc)
        self.pc = (self.pc + 2) & 0xFFFF
        return a

    def am_absx(self):
        a = (self.rd16(self.pc) + self.x) & 0xFFFF
        self.pc = (self.pc + 2) & 0xFFFF
        return a

    def am_absy(self):
        a = (self.rd16(self.pc) + self.y) & 0xFFFF
        self.pc = (self.pc + 2) & 0xFFFF
        return a

    def am_indx(self):
        z = (self.rd(self.pc) + self.x) & 0xFF
        self.pc = (self.pc + 1) & 0xFFFF
        return self.rd16zp(z)

    def am_indy(self):
        z = self.rd(self.pc)
        self.pc = (self.pc + 1) & 0xFFFF
        return (self.rd16zp(z) + self.y) & 0xFFFF

    def am_rel(self):
        off = self.rd(self.pc)
        self.pc = (self.pc + 1) & 0xFFFF
        if off & 0x80:
            off -= 256
        return (self.pc + off) & 0xFFFF

    # --- operations ----------------------------------------------------
    def op_adc(self, m):
        a = self.a
        c = 1 if self.p & FLAG_C else 0
        if self.p & FLAG_D:
            lo = (a & 0x0F) + (m & 0x0F) + c
            hi = (a >> 4) + (m >> 4)
            if lo > 9:
                lo += 6
                hi += 1
            self.setflag(FLAG_V, False)
            if hi > 9:
                hi += 6
            self.setflag(FLAG_C, hi > 15)
            r = ((hi & 0x0F) << 4) | (lo & 0x0F)
            self.setzn(r)
            self.a = r
        else:
            r = a + m + c
            self.setflag(FLAG_C, r > 0xFF)
            self.setflag(FLAG_V, bool((~(a ^ m) & (a ^ r)) & 0x80))
            self.a = self.setzn(r)

    def op_sbc(self, m):
        if self.p & FLAG_D:
            a = self.a
            c = 1 if self.p & FLAG_C else 0
            lo = (a & 0x0F) - (m & 0x0F) - (1 - c)
            hi = (a >> 4) - (m >> 4)
            if lo & 0x10:
                lo -= 6
                hi -= 1
            if hi & 0x10:
                hi -= 6
            r = a - m - (1 - c)
            self.setflag(FLAG_C, r >= 0)
            self.setflag(FLAG_V, bool(((a ^ m) & (a ^ (r & 0xFF))) & 0x80))
            self.setzn(r & 0xFF)
            self.a = ((hi & 0x0F) << 4) | (lo & 0x0F)
        else:
            self.op_adc(m ^ 0xFF)

    def op_cmp(self, reg, m):
        r = (reg - m) & 0x1FF
        self.setflag(FLAG_C, reg >= m)
        self.setzn(r & 0xFF)

    def branch(self, cond):
        t = self.am_rel()
        if cond:
            self.pc = t

    # --- run -----------------------------------------------------------
    def step(self):
        op = self.rd(self.pc)
        self.pc = (self.pc + 1) & 0xFFFF
        self.cycles += 1
        f = OPS.get(op)
        if f is None:
            raise RuntimeError("unimplemented opcode $%02X at $%04X" % (op, (self.pc - 1) & 0xFFFF))
        f(self)


def _build():
    o = {}

    def define(code, fn):
        o[code] = fn

    # LDA
    for code, am in ((0xA9, 'imm'), (0xA5, 'zp'), (0xB5, 'zpx'), (0xAD, 'abs'),
                     (0xBD, 'absx'), (0xB9, 'absy'), (0xA1, 'indx'), (0xB1, 'indy')):
        define(code, (lambda am: lambda c: setattr(c, 'a', c.setzn(c.rd(getattr(c, 'am_' + am)()))))(am))
    # LDX
    for code, am in ((0xA2, 'imm'), (0xA6, 'zp'), (0xB6, 'zpy'), (0xAE, 'abs'), (0xBE, 'absy')):
        define(code, (lambda am: lambda c: setattr(c, 'x', c.setzn(c.rd(getattr(c, 'am_' + am)()))))(am))
    # LDY
    for code, am in ((0xA0, 'imm'), (0xA4, 'zp'), (0xB4, 'zpx'), (0xAC, 'abs'), (0xBC, 'absx')):
        define(code, (lambda am: lambda c: setattr(c, 'y', c.setzn(c.rd(getattr(c, 'am_' + am)()))))(am))
    # STA / STX / STY
    for code, am in ((0x85, 'zp'), (0x95, 'zpx'), (0x8D, 'abs'), (0x9D, 'absx'),
                     (0x99, 'absy'), (0x81, 'indx'), (0x91, 'indy')):
        define(code, (lambda am: lambda c: c.wr(getattr(c, 'am_' + am)(), c.a))(am))
    for code, am in ((0x86, 'zp'), (0x96, 'zpy'), (0x8E, 'abs')):
        define(code, (lambda am: lambda c: c.wr(getattr(c, 'am_' + am)(), c.x))(am))
    for code, am in ((0x84, 'zp'), (0x94, 'zpx'), (0x8C, 'abs')):
        define(code, (lambda am: lambda c: c.wr(getattr(c, 'am_' + am)(), c.y))(am))

    # ADC / SBC / AND / ORA / EOR / CMP
    for code, am in ((0x69, 'imm'), (0x65, 'zp'), (0x75, 'zpx'), (0x6D, 'abs'),
                     (0x7D, 'absx'), (0x79, 'absy'), (0x61, 'indx'), (0x71, 'indy')):
        define(code, (lambda am: lambda c: c.op_adc(c.rd(getattr(c, 'am_' + am)())))(am))
    for code, am in ((0xE9, 'imm'), (0xE5, 'zp'), (0xF5, 'zpx'), (0xED, 'abs'),
                     (0xFD, 'absx'), (0xF9, 'absy'), (0xE1, 'indx'), (0xF1, 'indy')):
        define(code, (lambda am: lambda c: c.op_sbc(c.rd(getattr(c, 'am_' + am)())))(am))
    for code, am in ((0x29, 'imm'), (0x25, 'zp'), (0x35, 'zpx'), (0x2D, 'abs'),
                     (0x3D, 'absx'), (0x39, 'absy'), (0x21, 'indx'), (0x31, 'indy')):
        define(code, (lambda am: lambda c: setattr(c, 'a', c.setzn(c.a & c.rd(getattr(c, 'am_' + am)()))))(am))
    for code, am in ((0x09, 'imm'), (0x05, 'zp'), (0x15, 'zpx'), (0x0D, 'abs'),
                     (0x1D, 'absx'), (0x19, 'absy'), (0x01, 'indx'), (0x11, 'indy')):
        define(code, (lambda am: lambda c: setattr(c, 'a', c.setzn(c.a | c.rd(getattr(c, 'am_' + am)()))))(am))
    for code, am in ((0x49, 'imm'), (0x45, 'zp'), (0x55, 'zpx'), (0x4D, 'abs'),
                     (0x5D, 'absx'), (0x59, 'absy'), (0x41, 'indx'), (0x51, 'indy')):
        define(code, (lambda am: lambda c: setattr(c, 'a', c.setzn(c.a ^ c.rd(getattr(c, 'am_' + am)()))))(am))
    for code, am in ((0xC9, 'imm'), (0xC5, 'zp'), (0xD5, 'zpx'), (0xCD, 'abs'),
                     (0xDD, 'absx'), (0xD9, 'absy'), (0xC1, 'indx'), (0xD1, 'indy')):
        define(code, (lambda am: lambda c: c.op_cmp(c.a, c.rd(getattr(c, 'am_' + am)())))(am))
    for code, am in ((0xE0, 'imm'), (0xE4, 'zp'), (0xEC, 'abs')):
        define(code, (lambda am: lambda c: c.op_cmp(c.x, c.rd(getattr(c, 'am_' + am)())))(am))
    for code, am in ((0xC0, 'imm'), (0xC4, 'zp'), (0xCC, 'abs')):
        define(code, (lambda am: lambda c: c.op_cmp(c.y, c.rd(getattr(c, 'am_' + am)())))(am))

    # BIT
    def _bit(am):
        def f(c):
            m = c.rd(getattr(c, 'am_' + am)())
            c.setflag(FLAG_Z, (c.a & m) == 0)
            c.setflag(FLAG_N, bool(m & 0x80))
            c.setflag(FLAG_V, bool(m & 0x40))
        return f
    define(0x24, _bit('zp'))
    define(0x2C, _bit('abs'))

    # shifts / rotates, accumulator and memory
    def _asl_v(c, m):
        c.setflag(FLAG_C, bool(m & 0x80))
        return c.setzn((m << 1) & 0xFF)

    def _lsr_v(c, m):
        c.setflag(FLAG_C, bool(m & 0x01))
        return c.setzn(m >> 1)

    def _rol_v(c, m):
        old = 1 if c.p & FLAG_C else 0
        c.setflag(FLAG_C, bool(m & 0x80))
        return c.setzn(((m << 1) | old) & 0xFF)

    def _ror_v(c, m):
        old = 0x80 if c.p & FLAG_C else 0
        c.setflag(FLAG_C, bool(m & 0x01))
        return c.setzn((m >> 1) | old)

    for base, fn in ((0x0A, _asl_v), (0x4A, _lsr_v), (0x2A, _rol_v), (0x6A, _ror_v)):
        define(base, (lambda fn: lambda c: setattr(c, 'a', fn(c, c.a)))(fn))
    for fn, table in ((_asl_v, ((0x06, 'zp'), (0x16, 'zpx'), (0x0E, 'abs'), (0x1E, 'absx'))),
                      (_lsr_v, ((0x46, 'zp'), (0x56, 'zpx'), (0x4E, 'abs'), (0x5E, 'absx'))),
                      (_rol_v, ((0x26, 'zp'), (0x36, 'zpx'), (0x2E, 'abs'), (0x3E, 'absx'))),
                      (_ror_v, ((0x66, 'zp'), (0x76, 'zpx'), (0x6E, 'abs'), (0x7E, 'absx')))):
        for code, am in table:
            def mk(fn, am):
                def f(c):
                    a = getattr(c, 'am_' + am)()
                    c.wr(a, fn(c, c.rd(a)))
                return f
            define(code, mk(fn, am))

    # INC / DEC
    for code, am in ((0xE6, 'zp'), (0xF6, 'zpx'), (0xEE, 'abs'), (0xFE, 'absx')):
        def mkinc(am):
            def f(c):
                a = getattr(c, 'am_' + am)()
                c.wr(a, c.setzn(c.rd(a) + 1))
            return f
        define(code, mkinc(am))
    for code, am in ((0xC6, 'zp'), (0xD6, 'zpx'), (0xCE, 'abs'), (0xDE, 'absx')):
        def mkdec(am):
            def f(c):
                a = getattr(c, 'am_' + am)()
                c.wr(a, c.setzn(c.rd(a) - 1))
            return f
        define(code, mkdec(am))

    # register transfers and inc/dec
    define(0xAA, lambda c: setattr(c, 'x', c.setzn(c.a)))
    define(0xA8, lambda c: setattr(c, 'y', c.setzn(c.a)))
    define(0x8A, lambda c: setattr(c, 'a', c.setzn(c.x)))
    define(0x98, lambda c: setattr(c, 'a', c.setzn(c.y)))
    define(0xBA, lambda c: setattr(c, 'x', c.setzn(c.sp)))
    define(0x9A, lambda c: setattr(c, 'sp', c.x))
    define(0xE8, lambda c: setattr(c, 'x', c.setzn(c.x + 1)))
    define(0xCA, lambda c: setattr(c, 'x', c.setzn(c.x - 1)))
    define(0xC8, lambda c: setattr(c, 'y', c.setzn(c.y + 1)))
    define(0x88, lambda c: setattr(c, 'y', c.setzn(c.y - 1)))

    # stack
    define(0x48, lambda c: c.push(c.a))
    define(0x68, lambda c: setattr(c, 'a', c.setzn(c.pop())))
    define(0x08, lambda c: c.push(c.p | FLAG_B | FLAG_U))
    define(0x28, lambda c: setattr(c, 'p', (c.pop() | FLAG_U) & ~FLAG_B))

    # flags
    define(0x18, lambda c: c.setflag(FLAG_C, False))
    define(0x38, lambda c: c.setflag(FLAG_C, True))
    define(0x58, lambda c: c.setflag(FLAG_I, False))
    define(0x78, lambda c: c.setflag(FLAG_I, True))
    define(0xB8, lambda c: c.setflag(FLAG_V, False))
    define(0xD8, lambda c: c.setflag(FLAG_D, False))
    define(0xF8, lambda c: c.setflag(FLAG_D, True))

    # branches
    define(0x10, lambda c: c.branch(not (c.p & FLAG_N)))
    define(0x30, lambda c: c.branch(bool(c.p & FLAG_N)))
    define(0x50, lambda c: c.branch(not (c.p & FLAG_V)))
    define(0x70, lambda c: c.branch(bool(c.p & FLAG_V)))
    define(0x90, lambda c: c.branch(not (c.p & FLAG_C)))
    define(0xB0, lambda c: c.branch(bool(c.p & FLAG_C)))
    define(0xD0, lambda c: c.branch(not (c.p & FLAG_Z)))
    define(0xF0, lambda c: c.branch(bool(c.p & FLAG_Z)))

    # jumps and calls
    def _jmp_abs(c):
        c.pc = c.rd16(c.pc)

    def _jmp_ind(c):
        a = c.rd16(c.pc)
        # the NMOS page-wrap bug, faithfully: the harness relies on JMP (SRC)
        # in zero page, which never straddles a page, but a test could.
        lo = c.rd(a)
        hi = c.rd((a & 0xFF00) | ((a + 1) & 0xFF))
        c.pc = lo | (hi << 8)

    def _jsr(c):
        t = c.rd16(c.pc)
        r = (c.pc + 1) & 0xFFFF
        c.push(r >> 8)
        c.push(r & 0xFF)
        c.pc = t

    def _rts(c):
        lo = c.pop()
        hi = c.pop()
        c.pc = ((hi << 8) | lo) + 1 & 0xFFFF

    def _rti(c):
        c.p = (c.pop() | FLAG_U) & ~FLAG_B
        lo = c.pop()
        hi = c.pop()
        c.pc = (hi << 8) | lo

    define(0x4C, _jmp_abs)
    define(0x6C, _jmp_ind)
    define(0x20, _jsr)
    define(0x60, _rts)
    define(0x40, _rti)
    define(0xEA, lambda c: None)
    define(0x00, lambda c: setattr(c, 'halted', True))
    return o


OPS = _build()
