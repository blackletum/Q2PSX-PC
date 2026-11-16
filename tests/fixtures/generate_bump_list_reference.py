"""Execute the PAL/US actor list: its swap, its appends and its order.

Two halves. The first runs InitEveryLevel's list setup (0x80055288) and then
scripted frames of InitialiseBumpListFrame (0x8005525C) followed by
AddCreToNextBumpList (0x800552B4) calls, reading the list every query sees
(0x800B2B90..0x800B2B98) after each swap. The objects are stand-in addresses:
object 0 is the player, who appends first each frame as slot 0 does, and
object k is creature handle k - 1. Some frames append past the 32-pointer cap
and some append one object twice, as a player corpse does.

The second runs LineIntersectCreCylinder (0x800544EC) on two-body lists, the
same two bodies in both orders, with the start often inside both cylinders.
The order is what the port's lists got wrong; the outputs say which body each
order stops in and where.

Everything executes unchanged; nothing is hooked except what the toss fixture
already services. Generated output holds numbers only, never native code.
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

OBJECTS = [0x801D2000 + 0x300 * k for k in range(41)]
START, END, OUT, LIST = 0x801D0000, 0x801D0020, 0x801D0040, 0x801D0080
BODIES = [0x801D1000, 0x801D1400]
# Far from the port test's player at the origin, so only the two bodies count.
FIELD = (120000, 0, -90000)


class BumpMachine(TossMachine):
    def intercept(self, pc):
        if self.canonical(pc) == 0x800544EC:
            return False  # The toss fixture answers "nothing"; run it here.
        return super().intercept(pc)

    def read_list(self):
        base = self.read(self.address(0x800B2B90), 4)
        end = self.read(self.address(0x800B2B98), 4)
        assert (end - base) % 4 == 0 and 0 <= end - base <= 128
        return [OBJECTS.index(self.read(base + 4 * i, 4))
                for i in range((end - base) // 4)]

    def query(self, order):
        for j, body in enumerate(order):
            self.write(LIST + 4 * j, 4, BODIES[body])
        self.registers()
        self.r[29] = 0x801FC000
        self.r[4:8] = [START, END, 0, OUT]
        self.write(self.r[29] + 16, 4, LIST)
        self.write(self.r[29] + 20, 4, LIST + 4 * len(order))
        self.run(self.address(0x800544EC), limit=20000)
        assert self.r[29] == 0x801FC000
        return (BODIES.index(self.r[2]) if self.r[2] else -1), self.vector(OUT)


def frames_script():
    rng = random.Random(0x5525C)
    frames = []
    counts = [0, 1, 3, 31, 32, 33, 40, 5, 0, 12, 2, 31]
    counts += [rng.randrange(0, 41) for _ in range(12)]
    for f, count in enumerate(counts):
        appends = [0]  # the player, slot 0, first
        appends += [rng.randrange(1, 41) for _ in range(count)]
        if f % 3 == 1:
            appends.insert(1, 0)  # a dying player's second append
        frames.append(appends)
    return frames


def execute(cue, ntsc):
    disc = Form1Disc(cue)
    try:
        exe = disc.read('/SLUS_007.57' if ntsc else '/SLES_015.34')
    finally:
        disc.stream.close()
    image = exe[2048:2048 + struct.unpack_from('<I', exe, 0x1C)[0]]
    digest = hashlib.sha256(image).hexdigest()
    assert digest == (NTSC_SEGMENT_SHA if ntsc else SEGMENT_SHA)
    m = BumpMachine(image, ntsc)

    m.call(0x80055288)
    frames = []
    for appends in frames_script():
        m.call(0x8005525C)
        read = m.read_list()
        for k in appends:
            m.call(0x800552B4, OBJECTS[k])
        frames.append({'read': read, 'appends': appends})

    rng = random.Random(0x544EC)
    orders, changed, picked = [], 0, 0
    for i in range(1024):
        start = [FIELD[k] + rng.randrange(-3000, 3001) for k in range(3)]
        delta = [rng.randrange(-3000, 3001) for _ in range(3)]
        if i % 4 == 0:
            delta[1] = 0
        bodies = []
        for j, address in enumerate(BODIES):
            radius = rng.choice((124, 286, 429, 590, rng.randrange(60, 700)))
            height = rng.choice((143, 572, 600, 918, rng.randrange(100, 1200)))
            if i % 2 == 0:
                # The start inside the cylinder: within its radius across and
                # inside its slab (origin.y + 286 down to that minus height).
                centre = [start[0] + rng.randrange(-radius // 2, radius // 2 + 1),
                          start[1] - 286 + rng.randrange(1, max(2, height - 1)),
                          start[2] + rng.randrange(-radius // 2, radius // 2 + 1)]
            else:
                along = rng.randrange(256, 3841)
                centre = [start[k] + delta[k] * along // 4096 +
                          rng.randrange(-radius, radius + 1) for k in range(3)]
            bodies.append([centre, radius, height])
            m.put(address, bytes(0x400))
            m.put_vector(address + 0x54, centre)
            m.write(address + 0x94, 2, radius)
            m.write(address + 0x96, 2, height)
        m.put_vector(START, start)
        m.put_vector(END, [a + b for a, b in zip(start, delta)])
        ab = m.query([0, 1])
        ba = m.query([1, 0])
        if ab[0] < 0 and ba[0] < 0:
            continue
        changed += ab != ba
        picked += ab[0] != ba[0]
        orders.append({'start': start, 'delta': delta, 'bodies': bodies,
                       'ab': list(ab), 'ba': list(ba)})
    return {'region': 'US' if ntsc else 'PAL', 'sha256': digest,
            'frames': frames, 'orders': orders,
            'order_changed': changed, 'order_picked': picked}


def vec(values):
    return '{' + ','.join(map(str, values)) + '}'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('pal', type=Path)
    p.add_argument('us', type=Path)
    p.add_argument('--trace', required=True, type=Path)
    p.add_argument('--output', type=Path,
                   default=Path(__file__).with_name('bump_list_reference.h'))
    p.add_argument('--check', action='store_true')
    args = p.parse_args()
    runs = [execute(args.pal, False), execute(args.us, True)]
    for key in ('frames', 'orders'):
        assert runs[0][key] == runs[1][key], f'PAL/US {key} differ'
    run = runs[0]
    lines = ['/* Native PAL/US actor list. See generate_bump_list_reference.py. */',
             '#ifndef Q2_BUMP_LIST_REFERENCE_H', '#define Q2_BUMP_LIST_REFERENCE_H',
             '/* Object 0 is the player; object k is creature handle k - 1. */',
             'typedef struct {',
             '    int append_count, appends[48];',
             '    int read_count, read[32];',
             '} native_bump_frame;',
             'static const native_bump_frame native_bump_frames[] = {']
    for f in run['frames']:
        lines.append('    {%d,%s,%d,%s},' % (len(f['appends']), vec(f['appends'] or [0]),
                                             len(f['read']), vec(f['read'] or [0])))
    lines += ['};',
              'typedef struct {',
              '    int start[3], delta[3];',
              '    int centre[2][3], radius[2], height[2];',
              '    int hit_ab, point_ab[3], hit_ba, point_ba[3];',
              '} native_bump_order;',
              'static const native_bump_order native_bump_orders[] = {']
    for c in run['orders']:
        b = c['bodies']
        lines.append('    {' + ','.join((
            vec(c['start']), vec(c['delta']),
            '{' + vec(b[0][0]) + ',' + vec(b[1][0]) + '}',
            vec([b[0][1], b[1][1]]), vec([b[0][2], b[1][2]]),
            str(c['ab'][0]), vec(c['ab'][1]), str(c['ba'][0]), vec(c['ba'][1]))) + '},')
    lines += ['};',
              '#define NATIVE_BUMP_ORDER_CHANGED %d' % run['order_changed'],
              '#define NATIVE_BUMP_ORDER_PICKED %d' % run['order_picked'],
              '#endif', '']
    text = '\n'.join(lines)
    if args.check:
        assert args.output.read_text(encoding='utf-8') == text
    else:
        args.output.write_text(text, encoding='utf-8')
    args.trace.write_text(json.dumps(runs, indent=2) + '\n', encoding='utf-8')
    print(f"Native actor list: {len(run['frames'])} frames, "
          f"{len(run['orders'])} two-body crossings, {run['order_changed']} "
          f"land elsewhere in the other order, {run['order_picked']} pick "
          f"another body; PAL/US agree")


if __name__ == '__main__':
    main()
