"""Execute each level's native load and record what it puts in VRAM.

LoaderMachine (generate_heap_reference.py) runs the retail load path with GPU
transfers stubbed. Here LoadImage (0x80083648) is serviced instead: each
transfer's RECT and payload are written into a 1024x512 halfword VRAM model.
After the load, every uploaded rectangle is hashed (FNV-1a over its halfwords,
row-major, little-endian) and the header records the rectangles and hashes,
never the texels. test_vram_native compares the port's q2_vram_upload of the
same map against them.

This qualifies the level texture/CLUT path the loader drives (decompression,
placement, the CLUT STP fix-up), not GPU timing or any later VRAM writer.

    python tests/fixtures/generate_vram_reference.py <PAL.cue> <US.cue> \\
        --trace .agents/tmp/vram/native.json [--check]
"""
import argparse
import hashlib
import json
import struct
import time
from pathlib import Path

from generate_heap_reference import (Form1Disc, LoaderMachine, NTSC_SEGMENT_SHA,
                                     configure_level, select_mission)
from generate_resource_reference import SEGMENT_SHA

LEVEL_TABLE, LEVEL_ROWS, LEVEL_STRIDE = 0x8009C6C8, 52, 56


class VramMachine(LoaderMachine):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.vram = bytearray(1024 * 512 * 2)
        self.uploads = []

    def square_root0(self):
        # SquareRoot0 (0x8008A7E8), instruction for instruction: the
        # interpreter has no GTE, and this is the routine's only GTE use
        # (LZCS/LZCR, a leading-sign count). The table is the executable's own.
        a0 = self.r[4] & 0xFFFFFFFF
        value = a0 - (1 << 32) if a0 & 0x80000000 else a0
        bits = a0 if value >= 0 else (~a0) & 0xFFFFFFFF
        lzc = 32 if bits == 0 else 32 - bits.bit_length()
        if lzc == 32:
            return 0
        t2 = lzc & ~1
        t1 = (31 - t2) >> 1
        if t2 - 24 >= 0:
            t4 = (a0 << (t2 - 24)) & 0xFFFFFFFF
        else:
            t4 = (value >> (24 - t2)) & 0xFFFFFFFF
        t4 = (((t4 - 64) & 0xFFFFFFFF) << 1) & 0xFFFFFFFF
        t5 = self.read(self.address(0x800AA340) + t4, 2, sign=True) & 0xFFFFFFFF
        t5 = (t5 << (t1 & 31)) & 0xFFFFFFFF
        return t5 >> 12

    def intercept(self, pc):
        if self.canonical(pc) == 0x8008A7E8:  # SquareRoot0, serviced exactly
            self.r[2] = self.square_root0()
            key = f'{pc:08x}'
            self.hooks[key] = self.hooks.get(key, 0) + 1
            return True
        if self.canonical(pc) == 0x80083648:  # LoadImage(RECT *, u_long *)
            at = self.r[4] & 0x1FFFFFFF
            x, y, w, h = struct.unpack('<hhhh', bytes(self.mem[at:at + 8]))
            src = self.r[5] & 0x1FFFFFFF
            for row in range(h):
                for col in range(w):
                    vx, vy = x + col, y + row
                    if 0 <= vx < 1024 and 0 <= vy < 512:
                        o = (vy * 1024 + vx) * 2
                        s = src + (row * w + col) * 2
                        self.vram[o:o + 2] = self.mem[s:s + 2]
            self.uploads.append((x, y, w, h))
            self.r[2] = 0
            key = f'{pc:08x}'
            self.hooks[key] = self.hooks.get(key, 0) + 1
            return True
        return super().intercept(pc)

    def call(self, pc, *args):
        # A level whose module never returns (MAGDEMO's init loops forever on a
        # retail engine block) is reported, not executed to the global limit.
        pc = self.address(pc)
        self.registers()
        self.r[4:4 + len(args)] = args
        self.run(pc, limit=25000000)
        return self.r[2]

    def rect_hash(self, x, y, w, h):
        value = 2166136261
        for row in range(h):
            for col in range(w):
                o = ((y + row) * 1024 + x + col) * 2
                for byte in self.vram[o:o + 2]:
                    value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
        return value


def level_names(image, ntsc):
    m = LoaderMachine(image, None, ntsc)
    table = m.address(LEVEL_TABLE)
    names = []
    for i in range(LEVEL_ROWS):
        name = m.string(table + LEVEL_STRIDE * i + 12)
        if name and name not in names:
            names.append(name)
    return names


def execute(cue, ntsc):
    disc = Form1Disc(cue)
    exe = disc.read('/SLUS_007.57' if ntsc else '/SLES_015.34')
    image = exe[2048:2048 + struct.unpack_from('<I', exe, 0x1C)[0]]
    digest = hashlib.sha256(image).hexdigest()
    assert digest == (NTSC_SEGMENT_SHA if ntsc else SEGMENT_SHA)
    cases = []
    try:
        for name in level_names(image, ntsc):
            t0 = time.time()
            m = VramMachine(image, disc, ntsc)
            m.call(0x8006E8C0)
            try:
                row = configure_level(m, name, 1, False)
                select_mission(m, row, None, False)
                for fn in (0x80070C94, 0x80078A88, 0x8007146C):
                    m.call(fn)
            except Exception as exc:  # A row the isolated loader cannot run.
                cases.append({'map': name, 'error': str(exc)})
                print(f"{'US' if ntsc else 'PAL'} {name}: skipped ({exc})", flush=True)
                continue
            rects = [list(u) + [m.rect_hash(*u)] for u in m.uploads
                     if u[0] >= 0 and u[1] >= 0 and u[0] + u[2] <= 1024 and u[1] + u[3] <= 512]
            cases.append({'map': name, 'rects': rects})
            print(f"{'US' if ntsc else 'PAL'} {name}: {len(rects)} uploads ({time.time() - t0:.0f} s)", flush=True)
    finally:
        disc.stream.close()
    return {'region': 'US' if ntsc else 'PAL', 'sha256': digest, 'cases': cases}


def header(runs):
    lines = ['/* Native level VRAM uploads; see generate_vram_reference.py. */',
             '#ifndef Q2_VRAM_REFERENCE_H', '#define Q2_VRAM_REFERENCE_H',
             'typedef struct { short x, y, w, h; unsigned hash; } native_vram_rect;',
             'typedef struct { int ntsc; const char *map; int count;',
             '                 native_vram_rect rect[16]; } native_vram_case;',
             'static const native_vram_case native_vram[] = {']
    for run in runs:
        for case in run['cases']:
            if 'rects' not in case:
                continue
            rects = case['rects'][:16]
            body = ','.join('{%d,%d,%d,%d,%uu}' % tuple(r) for r in rects) or '{0,0,0,0,0u}'
            lines.append('    {%d,"%s",%d,{%s}},' % (int(run['region'] == 'US'), case['map'],
                                                     len(rects), body))
    return '\n'.join(lines + ['};', '#endif', ''])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pal', type=Path)
    parser.add_argument('us', type=Path)
    parser.add_argument('--trace', required=True, type=Path)
    parser.add_argument('--output', type=Path, default=Path(__file__).with_name('vram_reference.h'))
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    runs = [execute(args.pal, False), execute(args.us, True)]
    args.trace.parent.mkdir(parents=True, exist_ok=True)
    args.trace.write_text(json.dumps(runs, indent=2) + '\n', encoding='utf-8')
    text = header(runs)
    if args.check:
        assert args.output.read_text(encoding='utf-8') == text, 'stored VRAM fixture differs'
    else:
        args.output.write_text(text, encoding='utf-8')
    total = sum(len(c.get('rects', [])) for r in runs for c in r['cases'])
    print(f'{total} native uploads over {sum(len(r["cases"]) for r in runs)} loads')


if __name__ == '__main__':
    main()
