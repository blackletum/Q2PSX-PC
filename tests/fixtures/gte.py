"""A PlayStation GTE (COP2) for the fixture interpreters.

Written from the documented behaviour of the geometry transformation engine:
32 data and 32 control registers, the command set with its sf/lm/mx/v/cv
fields, saturation into IR/SXY/SZ/OTZ/RGB with the FLAG bits they set, and the
unsigned Newton-Raphson divide RTPS/RTPT use. It is the arithmetic the logic
paths call (MVMVA, RTPS/RTPT, SQR, OP, GPF/GPL, NCLIP, AVSZ, the lighting set)
so that native code which rotates or projects a vector gets the console's
numbers. Rendering correctness beyond that is not claimed.
"""


def _s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def _s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def _unr_table():
    t = []
    for i in range(0x101):
        v = max(0, (0x40000 // (i + 0x100) + 1) // 2 - 0x101)
        t.append(v)
    return t


UNR = _unr_table()


class GTE:
    def __init__(self):
        self.d = [0] * 32   # data registers, raw 32-bit
        self.c = [0] * 32   # control registers, raw 32-bit
        self.flag = 0

    # ----------------------------------------------------------- register io
    def read_data(self, i):
        if i in (1, 3, 5, 8, 9, 10, 11):          # VZ0..2, IR0..3: sign extended
            return _s16(self.d[i]) & 0xFFFFFFFF
        if i in (7, 16, 17, 18, 19):              # OTZ, SZ0..3: zero extended
            return self.d[i] & 0xFFFF
        if i == 15:                               # SXYP mirrors SXY2
            return self.d[14]
        if i in (28, 29):                         # IRGB / ORGB from IR1..3
            def c5(v):
                v = _s16(v) >> 7
                return 0 if v < 0 else 0x1F if v > 0x1F else v
            return c5(self.d[9]) | (c5(self.d[10]) << 5) | (c5(self.d[11]) << 10)
        if i == 31:                               # LZCR
            return self.d[31]
        return self.d[i] & 0xFFFFFFFF

    def write_data(self, i, v):
        v &= 0xFFFFFFFF
        if i == 15:                               # SXYP pushes the FIFO
            self.d[12], self.d[13], self.d[14] = self.d[13], self.d[14], v
            return
        if i == 28:                               # IRGB expands into IR1..3
            self.d[9] = (v & 0x1F) << 7
            self.d[10] = ((v >> 5) & 0x1F) << 7
            self.d[11] = ((v >> 10) & 0x1F) << 7
            self.d[28] = v
            return
        if i in (29, 31):                         # ORGB, LZCR are read-only
            return
        if i == 30:                               # LZCS sets LZCR
            self.d[30] = v
            s = _s32(v)
            bits = v if s >= 0 else (~v) & 0xFFFFFFFF
            self.d[31] = 32 if bits == 0 else 32 - bits.bit_length()
            return
        self.d[i] = v

    def read_ctrl(self, i):
        if i in (4, 12, 20, 26, 27, 29, 30):      # RT33, L33, LB3, H, DQA, ZSF3/4
            return _s16(self.c[i]) & 0xFFFFFFFF   # H too: the read sign-extends
        if i == 31:
            f = self.flag & 0x7FFFF000
            if f & 0x7F87E000:
                f |= 0x80000000
            return f
        return self.c[i] & 0xFFFFFFFF

    def write_ctrl(self, i, v):
        v &= 0xFFFFFFFF
        if i == 31:
            self.flag = v & 0x7FFFF000
            return
        self.c[i] = v

    # -------------------------------------------------------------- helpers
    def _mat(self, base):
        c = self.c
        return [[_s16(c[base]), _s16(c[base] >> 16), _s16(c[base + 1])],
                [_s16(c[base + 1] >> 16), _s16(c[base + 2]), _s16(c[base + 2] >> 16)],
                [_s16(c[base + 3]), _s16(c[base + 3] >> 16), _s16(c[base + 4])]]

    def _vec(self, n):
        d = self.d
        if n == 3:
            return [_s16(d[9]), _s16(d[10]), _s16(d[11])]
        return [_s16(d[n * 2]), _s16(d[n * 2] >> 16), _s16(d[n * 2 + 1])]

    def _tr(self, base):
        return [_s32(self.c[base]), _s32(self.c[base + 1]), _s32(self.c[base + 2])]

    def _mac(self, n, v):
        # 44-bit overflow flags for MAC1..3, 32-bit for MAC0
        if n == 0:
            if v > 0x7FFFFFFF: self.flag |= 1 << 16
            if v < -0x80000000: self.flag |= 1 << 15
            self.d[24] = v & 0xFFFFFFFF
            return v
        if v > 0x7FFFFFFFFFF: self.flag |= 1 << (31 - n)
        if v < -0x80000000000: self.flag |= 1 << (28 - n)
        # sign-extend to 44 bits
        v &= (1 << 44) - 1
        if v & (1 << 43):
            v -= 1 << 44
        return v

    def _set_mac(self, n, v, sf):
        v = self._mac(n, v)
        r = v >> (12 * sf)
        self.d[24 + n] = r & 0xFFFFFFFF
        return r

    def _ir(self, n, v, lm):
        lo = 0 if lm else -0x8000
        if v > 0x7FFF:
            self.flag |= 1 << (25 - n); v = 0x7FFF
        elif v < lo:
            self.flag |= 1 << (25 - n); v = lo
        self.d[8 + n] = v & 0xFFFF
        return v

    def _ir0(self, v):
        if v > 0x1000: self.flag |= 1 << 12; v = 0x1000
        elif v < 0: self.flag |= 1 << 12; v = 0
        self.d[8] = v & 0xFFFF
        return v

    def _push_sz(self, v):
        if v > 0xFFFF: self.flag |= 1 << 18; v = 0xFFFF
        elif v < 0: self.flag |= 1 << 18; v = 0
        self.d[16], self.d[17], self.d[18], self.d[19] = self.d[17], self.d[18], self.d[19], v
        return v

    def _push_sxy(self, x, y):
        if x > 0x3FF: self.flag |= 1 << 14; x = 0x3FF
        elif x < -0x400: self.flag |= 1 << 14; x = -0x400
        if y > 0x3FF: self.flag |= 1 << 13; y = 0x3FF
        elif y < -0x400: self.flag |= 1 << 13; y = -0x400
        self.d[12], self.d[13] = self.d[13], self.d[14]
        self.d[14] = (x & 0xFFFF) | ((y & 0xFFFF) << 16)

    def _divide(self, h, sz3):
        if h < sz3 * 2:
            z = 16 - sz3.bit_length()             # leading zeros of the 16-bit SZ3
            n = (h << z) & 0xFFFFFFFF
            d = (sz3 << z) & 0xFFFF
            u = UNR[(d - 0x7FC0) >> 7] + 0x101
            d = (0x2000080 - d * u) >> 8
            d = (0x0000080 + d * u) >> 8
            r = min(0x1FFFF, (n * d + 0x8000) >> 16)
            return r
        self.flag |= 1 << 17
        return 0x1FFFF

    def _push_rgb(self, r, g, b):
        def cl(v, bit):
            if v < 0: self.flag |= 1 << bit; return 0
            if v > 0xFF: self.flag |= 1 << bit; return 0xFF
            return v
        code = (self.d[6] >> 24) & 0xFF
        self.d[20], self.d[21] = self.d[21], self.d[22]
        self.d[22] = cl(r, 21) | (cl(g, 20) << 8) | (cl(b, 19) << 16) | (code << 24)

    # ------------------------------------------------------------- commands
    def _rtp(self, n, sf, lm, last):
        rt = self._mat(0)
        tr = self._tr(5)
        v = self._vec(n)
        macs = []
        for i in range(3):
            acc = (tr[i] << 12) + rt[i][0] * v[0] + rt[i][1] * v[1] + rt[i][2] * v[2]
            macs.append(self._set_mac(i + 1, acc, sf))
        self._ir(1, macs[0], lm)
        self._ir(2, macs[1], lm)
        # IR3 saturation flag quirk: flag tested on MAC3>>12 regardless of sf
        mac3 = macs[2]
        lo = 0 if lm else -0x8000
        v3 = mac3
        if v3 > 0x7FFF: v3 = 0x7FFF
        elif v3 < lo: v3 = lo
        shifted = self._mac(3, (tr[2] << 12) + rt[2][0] * v[0] + rt[2][1] * v[1] + rt[2][2] * v[2]) >> 12
        if shifted > 0x7FFF or shifted < -0x8000:
            self.flag |= 1 << 22
        self.d[11] = v3 & 0xFFFF
        sz = self._push_sz(shifted)
        h = self.c[26] & 0xFFFF
        q = self._divide(h, sz)
        ofx, ofy = _s32(self.c[24]), _s32(self.c[25])
        x = self._mac(0, q * _s16(self.d[9]) + ofx) >> 16
        y = self._mac(0, q * _s16(self.d[10]) + ofy) >> 16
        self._push_sxy(x, y)
        if last:
            dqa, dqb = _s16(self.c[27]), _s32(self.c[28])
            mac0 = self._mac(0, q * dqa + dqb)
            self._ir0(mac0 >> 12)

    def _mvmva(self, sf, lm, mx, vv, cv):
        m = self._mat(0) if mx == 0 else self._mat(8) if mx == 1 else self._mat(16) if mx == 2 else None
        if m is None:  # garbage matrix, as documented
            r = _s16(self.d[6] & 0xFF) << 4
            m = [[-(r), r, _s16(self.d[8])], [_s16(self.c[1]), _s16(self.c[1]), _s16(self.c[1])],
                 [_s16(self.c[2]), _s16(self.c[2]), _s16(self.c[2])]]
        v = self._vec(vv)
        t = self._tr(5) if cv == 0 else self._tr(13) if cv == 1 else self._tr(21) if cv == 2 else [0, 0, 0]
        if cv == 2:  # far colour bug: only the last two products accumulate
            for i in range(3):
                self._ir(i + 1, self._mac(i + 1, (t[i] << 12) + m[i][0] * v[0]) >> (12 * sf), False)
                acc = m[i][1] * v[1] + m[i][2] * v[2]
                self._ir(i + 1, self._set_mac(i + 1, acc, sf), lm)
            return
        for i in range(3):
            acc = (t[i] << 12) + m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2]
            self._ir(i + 1, self._set_mac(i + 1, acc, sf), lm)

    def command(self, word):
        sf = (word >> 19) & 1
        lm = (word >> 10) & 1
        mx, vv, cv = (word >> 17) & 3, (word >> 15) & 3, (word >> 13) & 3
        op = word & 0x3F
        self.flag = 0
        d = self.d
        if op == 0x01:
            self._rtp(0, sf, lm, True)
        elif op == 0x30:
            self._rtp(0, sf, lm, False)
            self._rtp(1, sf, lm, False)
            self._rtp(2, sf, lm, True)
        elif op == 0x12:
            self._mvmva(sf, lm, mx, vv, cv)
        elif op == 0x06:  # NCLIP
            def xy(r): return _s16(r), _s16(r >> 16)
            (x0, y0), (x1, y1), (x2, y2) = xy(d[12]), xy(d[13]), xy(d[14])
            self._mac(0, x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1)
        elif op == 0x0C:  # OP
            r = self._mat(0)
            d1, d2, d3 = r[0][0], r[1][1], r[2][2]
            i1, i2, i3 = _s16(d[9]), _s16(d[10]), _s16(d[11])
            self._ir(1, self._set_mac(1, i3 * d2 - i2 * d3, sf), lm)
            self._ir(2, self._set_mac(2, i1 * d3 - i3 * d1, sf), lm)
            self._ir(3, self._set_mac(3, i2 * d1 - i1 * d2, sf), lm)
        elif op == 0x28:  # SQR
            for i in range(3):
                v = _s16(d[9 + i])
                self._ir(i + 1, self._set_mac(i + 1, v * v, sf), lm)
        elif op == 0x3D:  # GPF
            ir0 = _s16(d[8])
            vals = [self._set_mac(i + 1, ir0 * _s16(d[9 + i]), sf) for i in range(3)]
            for i in range(3): self._ir(i + 1, vals[i], lm)
            self._push_rgb(vals[0] >> 4, vals[1] >> 4, vals[2] >> 4)
        elif op == 0x3E:  # GPL
            ir0 = _s16(d[8])
            vals = [self._set_mac(i + 1, (_s32(d[25 + i]) << (12 * sf)) + ir0 * _s16(d[9 + i]), sf)
                    for i in range(3)]
            for i in range(3): self._ir(i + 1, vals[i], lm)
            self._push_rgb(vals[0] >> 4, vals[1] >> 4, vals[2] >> 4)
        elif op == 0x2D:  # AVSZ3
            zsf3 = _s16(self.c[29])
            mac0 = self._mac(0, zsf3 * (d[17] + d[18] + d[19]))
            self._otz(mac0 >> 12)
        elif op == 0x2E:  # AVSZ4
            zsf4 = _s16(self.c[30])
            mac0 = self._mac(0, zsf4 * (d[16] + d[17] + d[18] + d[19]))
            self._otz(mac0 >> 12)
        elif op in (0x10, 0x11, 0x13, 0x14, 0x16, 0x1B, 0x1C, 0x1E, 0x20, 0x29, 0x2A, 0x3F):
            # Lighting/colour commands: results feed only drawing. Keep the
            # FIFO moving so code that pops RGB stays consistent.
            self._push_rgb(0, 0, 0)
        else:
            raise ValueError(f'GTE command {op:02x} not modelled')

    def _otz(self, v):
        if v > 0xFFFF: self.flag |= 1 << 18; v = 0xFFFF
        elif v < 0: self.flag |= 1 << 18; v = 0
        self.d[7] = v
