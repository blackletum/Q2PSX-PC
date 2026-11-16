"""Execute the PAL/US rail ring: rcossin, RotateS and the Y negation.

Each case builds the shooter's matrix at object+0x2C0 with the retail RotMatrix
(0x80089E38) from the angles at +0xE6, then runs AnyoneFireInstantRail's ring
loop (0x8004917C) from 0x80049678 to its exit at 0x80049758 unchanged: the
rcossin table lookup (0x800A5430), RotateS (0x8006FC1C) on (cos, sin, 0) in
place, the Y negation (0x800496C4), the ring's `>> 2` added to the segment
offsets and the `>> 10` drift. The ring vector is read back at 0x800496D0,
after the negated Y is stored and before the shifts; the offsets and drifts
are read after the loop and checked against it here. The loop's registers and
two stack words are set as 0x80049660..0x80049674 leave them; the per-segment offsets
start at zero. No CPU-side hooks are needed; nothing reaches the GTE. No
original code is exported.
"""
import argparse
import hashlib
import json
import random
import struct
from pathlib import Path

from generate_combat_reference import signed
from generate_heap_reference import Form1Disc, NTSC_SEGMENT_SHA
from generate_resource_reference import SEGMENT_SHA
from generate_toss_reference import TossMachine

SHOOTER, RECORD, OFFSETS = 0x801D0000, 0x801D0400, 0x801D0800
FRAME = 0x801FFE00

# (per, turns): single player 30 and one turn; multiplayer 15, one turn with
# one or two windows and two with three or four (0x800493C8..0x800493F8).
LAYOUTS = [(30, 1), (15, 1), (15, 2)]


class RingMachine(TossMachine):
    def intercept(self, pc):
        pal = self.canonical(pc)
        if pal == 0x80089E38:
            return False  # The toss fixture stubs RotMatrix; run it here.
        if pal == 0x800496D0:
            at = self.r[16] + 84
            self.rings.append([signed(self.read(at + k * 2, 2), 16)
                               for k in range(3)])
            return False
        return super().intercept(pc)


def execute(cue, ntsc):
    disc = Form1Disc(cue)
    try:
        exe = disc.read('/SLUS_007.57' if ntsc else '/SLES_015.34')
    finally:
        disc.stream.close()
    image = exe[2048:2048 + struct.unpack_from('<I', exe, 0x1C)[0]]
    digest = hashlib.sha256(image).hexdigest()
    assert digest == (NTSC_SEGMENT_SHA if ntsc else SEGMENT_SHA)
    m = RingMachine(image, ntsc)
    rng = random.Random(0x800496C4)
    rows = []
    for i in range(384):
        if i < 48:
            # Level and pitched views first, then the strafe roll.
            angles = [((i // 8) - 3) * 160, (i % 8) * 512, 0]
        elif i < 192:
            angles = [rng.randrange(-1024, 1025), rng.randrange(-32768, 32768), 0]
        else:
            angles = [rng.randrange(-1024, 1025), rng.randrange(-32768, 32768),
                      rng.randrange(-400, 401)]
        per, turns = LAYOUTS[i % len(LAYOUTS)]
        astep = (turns << 12) // per
        m.put(SHOOTER, bytes(0x300))
        m.put(RECORD, bytes(0x200))
        m.put(OFFSETS, bytes(0x100))
        for k in range(3):
            m.write(SHOOTER + 0xE6 + k * 2, 2, angles[k])
        m.call(0x80089E38, SHOOTER + 0xE6, SHOOTER + 0x2C0)
        matrix = [signed(m.read(SHOOTER + 0x2C0 + k * 2, 2), 16) for k in range(9)]

        m.registers()
        m.r[29] = FRAME
        m.write(FRAME + 56, 4, OFFSETS)   # s1: the segment offsets
        m.write(FRAME + 60, 4, astep)     # t7: the angle step
        m.write(FRAME + 104, 4, SHOOTER)  # t6: the shooter
        m.r[18] = m.r[16] = RECORD        # s2, and s0 walks from it
        m.r[17] = OFFSETS                 # s1, 0x8004966C's load of sp+56
        m.r[21] = 84                      # s5, 0x80049670
        m.r[20] = per                     # s4
        m.r[19] = m.r[23] = 0             # s3 = s7 = 0, the first angle
        m.rings = []
        m.run(m.address(0x80049678), stop=m.address(0x80049758), limit=200000)
        assert len(m.rings) == per, 'the ring loop ran short'
        for n, ring in enumerate(m.rings):
            pos = [signed(m.read(OFFSETS + n * 6 + k * 2, 2), 16) for k in range(3)]
            vel = [signed(m.read(RECORD + 84 + n * 6 + k * 2, 2), 16) for k in range(3)]
            assert pos == [v >> 2 for v in ring] and vel == [v >> 10 for v in ring]
        rows.append({'angles': angles, 'per': per, 'astep': astep,
                     'matrix': matrix, 'ring': m.rings})
    return {'region': 'US' if ntsc else 'PAL', 'sha256': digest, 'cases': rows}


def vec(values):
    return '{' + ','.join(map(str, values)) + '}'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('pal', type=Path)
    p.add_argument('us', type=Path)
    p.add_argument('--trace', required=True, type=Path)
    p.add_argument('--output', type=Path,
                   default=Path(__file__).with_name('rail_reference.h'))
    p.add_argument('--check', action='store_true')
    args = p.parse_args()
    runs = [execute(args.pal, False), execute(args.us, True)]
    assert runs[0]['cases'] == runs[1]['cases'], 'PAL/US rail ring differs'
    lines = ['/* Native PAL/US rail ring. See generate_rail_reference.py. */',
             '#ifndef Q2_RAIL_REFERENCE_H', '#define Q2_RAIL_REFERENCE_H',
             'typedef struct {',
             '    int angles[3], per, astep, matrix[9];',
             '    short ring[30][3];',
             '} native_rail_case;',
             'static const native_rail_case native_rail[] = {']
    for c in runs[0]['cases']:
        rings = c['ring'] + [[0, 0, 0]] * (30 - c['per'])
        lines.append('    {' + ','.join((vec(c['angles']), str(c['per']),
                     str(c['astep']), vec(c['matrix']),
                     '{' + ','.join(vec(r) for r in rings) + '}')) + '},')
    text = '\n'.join(lines + ['};', '#endif', ''])
    if args.check:
        assert args.output.read_text(encoding='utf-8') == text
    else:
        args.output.write_text(text, encoding='utf-8')
    args.trace.parent.mkdir(parents=True, exist_ok=True)
    args.trace.write_text(json.dumps(runs, indent=2) + '\n', encoding='utf-8')
    print(f"Native rail ring: {len(runs[0]['cases'])} cases per region; PAL/US agree")


if __name__ == '__main__':
    main()
