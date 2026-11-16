"""Execute the PAL/US player muzzle composition: RotMatrix, RotateS and the
blaster's own arithmetic around them.

Each case builds the player's matrix at object+0x2C0 with the retail RotMatrix
(0x80089E38) from the angles at +0xE6, then runs QPlayerFireBlaster
(0x8004BFBC) from its entry until the muzzle point is complete on its stack at
0x8004C078 (the jal that follows). Everything in between executes unchanged:
the offset load from 0x800AE9C4, the Y negation (0x8004C01C), RotateS
(0x8006FC1C), the rotated Y subtracted from the origin (0x8004C04C) and the
eye height (+286 - object+0xF6). The offset is the table's own for the first
cases and is then rewritten in place, so the same instructions carry every
other weapon's offset and the hand grenade's release offset. No CPU-side hooks
are needed; nothing reaches the GTE. No original code is exported.
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

PLAYER, CLIENT = 0x801D0000, 0x801D0400
ORIGIN = [1000, -2000, 3000]
VIEW_HEIGHT = 150

# The disc's muzzle triples (weapontables.c) and Grenade3's release literal
# (0x800AE980), in the (right, down, forward) form the fire functions load.
OFFSETS = [(80, 56, 250), (76, 80, 250), (96, 72, 250), (72, 80, 250),
           (104, 128, 250), (80, 40, 200), (40, 80, 100), (90, 140, 250),
           (40, 96, 256), (80, -50, 200)]


class Done(Exception):
    pass


class MuzzleMachine(TossMachine):
    def intercept(self, pc):
        if self.canonical(pc) == 0x80089E38:
            return False  # The toss fixture stubs RotMatrix; run it here.
        if self.canonical(pc) == 0x8004C078:
            sp = self.r[29]
            self.point = [signed(self.read(sp + 40 + k * 4, 4)) for k in range(3)]
            raise Done
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
    m = MuzzleMachine(image, ntsc)
    table = m.address(0x800AE9C4)
    assert [signed(m.read(table + k * 2, 2), 16) for k in range(3)] == [80, 56, 250]
    rng = random.Random(0x8004C01C)
    rows = []
    for i in range(1536):
        if i < 64:
            # Level and pitched views first, every weapon's offset.
            angles = [((i // 8) - 4) * 128, (i % 8) * 512, 0]
        elif i < 512:
            angles = [rng.randrange(-1024, 1025), rng.randrange(-32768, 32768), 0]
        else:
            # The strafe roll (0x8003AB18) with everything else arbitrary.
            angles = [rng.randrange(-1024, 1025), rng.randrange(-32768, 32768),
                      rng.randrange(-400, 401)]
        if i % 4 == 3:
            offset = [rng.randrange(-512, 513) for _ in range(3)]
        else:
            offset = list(OFFSETS[i % len(OFFSETS)])
        m.put(PLAYER, bytes(0x300))
        m.put(CLIENT, bytes(0x100))
        m.write(PLAYER + 12, 4, CLIENT)
        m.put_vector(PLAYER + 0x54, ORIGIN)
        m.write(PLAYER + 0xF6, 2, VIEW_HEIGHT)
        for k in range(3):
            m.write(PLAYER + 0xE6 + k * 2, 2, angles[k])
            m.write(table + k * 2, 2, offset[k])
        m.call(0x80089E38, PLAYER + 0xE6, PLAYER + 0x2C0)
        matrix = [signed(m.read(PLAYER + 0x2C0 + k * 2, 2), 16) for k in range(9)]
        m.point = None
        try:
            m.call(0x8004BFBC, PLAYER)
        except Done:
            pass
        assert m.point is not None, 'the blaster never reached its muzzle'
        eye = [ORIGIN[0], ORIGIN[1] + 286 - VIEW_HEIGHT, ORIGIN[2]]
        delta = [m.point[k] - eye[k] for k in range(3)]
        rows.append({'angles': angles, 'offset': offset, 'matrix': matrix,
                     'delta': delta})
    return {'region': 'US' if ntsc else 'PAL', 'sha256': digest, 'cases': rows}


def vec(values):
    return '{' + ','.join(map(str, values)) + '}'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('pal', type=Path)
    p.add_argument('us', type=Path)
    p.add_argument('--trace', required=True, type=Path)
    p.add_argument('--output', type=Path,
                   default=Path(__file__).with_name('muzzle_reference.h'))
    p.add_argument('--check', action='store_true')
    args = p.parse_args()
    runs = [execute(args.pal, False), execute(args.us, True)]
    assert runs[0]['cases'] == runs[1]['cases'], 'PAL/US muzzle differs'
    lines = ['/* Native PAL/US player muzzle. See generate_muzzle_reference.py. */',
             '#ifndef Q2_MUZZLE_REFERENCE_H', '#define Q2_MUZZLE_REFERENCE_H',
             'typedef struct {',
             '    int angles[3], offset[3], matrix[9], delta[3];',
             '} native_muzzle_case;',
             'static const native_muzzle_case native_muzzle[] = {']
    for c in runs[0]['cases']:
        lines.append('    {' + ','.join((vec(c['angles']), vec(c['offset']),
                     vec(c['matrix']), vec(c['delta']))) + '},')
    text = '\n'.join(lines + ['};', '#endif', ''])
    if args.check:
        assert args.output.read_text(encoding='utf-8') == text
    else:
        args.output.write_text(text, encoding='utf-8')
    args.trace.write_text(json.dumps(runs, indent=2) + '\n', encoding='utf-8')
    print(f"Native muzzle: {len(runs[0]['cases'])} cases per region; PAL/US agree")


if __name__ == '__main__':
    main()
