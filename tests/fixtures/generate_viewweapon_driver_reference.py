"""Execute the PAL/US view-weapon driver 0x8004EE0C over synthetic clip banks.

The complete driver executes unchanged: its substep loop, the key-boundary
state machine 0x8004F87C, the model-move driver 0x80050454 (Weapon_Animations),
the per-weapon frame driver 0x8004FEE8, the key reload and the hyperblaster
ramp 0x8004FC78, together with the dry pass's own auto-select 0x800506C4 and
neighbour search 0x80050758. After each call that has a held hand grenade, the
grenade's own think PrimaryGrenadeActive 0x8004A368 executes too, in its held
state: it is the only writer of the viewmodel+740 mark the cook arm reads.

The animation bank is synthetic (the retail bank's key COUNTS and key EVENTS, so
every frame-index arm and every move trigger sees its real clip shape, but
generated poses and durations) and is emitted into the header with the
results. The move records 0x8006D330 returns are synthetic too, emitted in
block-D units. Fire functions are scripted to return 1 (shot), 0 (spawned
nothing) or 2 (dry); a dry answer also empties that weapon's ammo slot, as a
real empty weapon has, and a hand grenade's 1 creates its held grenade the way
AnyoneFireGrenade 0x8004AA6C does. Sound, model lookup, matrix, rumble and
light services are recording hooks. No retail code or animation data is
exported; PAL and US must agree.
"""
import argparse
import hashlib
import json
import random
import struct
from pathlib import Path

from generate_combat_reference import signed
from generate_heap_reference import Form1Disc, LoaderMachine, NTSC_SEGMENT_SHA
from generate_resource_reference import SEGMENT_SHA

VM, PLAYER, CLIENT, INPUT = 0x801D0000, 0x801D0400, 0x801D0800, 0x801D0C00
BANK = 0x801D1000          # synthetic five-pointer blocks, then the keys
GRENADE = 0x801DC000       # the held grenade's entity record
MOVES = 0x801DD000         # synthetic move records, 0x40 per weapon
MODEL = 0x801DF000         # model N is MODEL + 0x100 * N (0x8006D008's answer)
FIRE_FAKE = 0x801E0000     # fire function N is FIRE_FAKE + 16 * N
DRIVER, GRENADE_THINK = 0x8004EE0C, 0x8004A368
DT, LEVEL_TIME = 0x800B2DB4, 0x800AEBAC
HALT, RESULT = 0x800B2B7C, 0x800B2B7E
CLIP_TABLE, FIRE_TABLE, AMMO_INDEX = 0x8009F59C, 0x8009D704, 0x8009DC5C
WINDOW, VIEWS, LIGHTS = 0x800D5C30, 0x800B2C2C, 0x800B2A28
# Sound handles the frame driver plays (0x800737A8's a0) and their codes.
SOUNDS = {0x800B2B38: 1, 0x800B2B3C: 2, 0x800B2B40: 3, 0x800B2B5C: 4}
# Return addresses of the four fire calls: the idle arm 0x8004FB30 and the
# machinegun, chaingun and hyperblaster arms of 0x8004FEE8.
SITES = {0x8004FB38: 0, 0x8004FFE8: 1, 0x80050214: 2, 0x800502E0: 3}
# The names 0x80050454 asks 0x8006D330 for (0x800ACD04 / 0x800ACD10 /
# 0x800ACCF8) and the synthetic move each weapon's model answers with, in
# block-D units: the loader scales them by five (0x8006C214), so the record
# the driver reads holds five times these.
MOVE_TABLE = {3: ('Fire', 0, 90), 6: ('Set', 0, 94), 7: ('Spin', 4, 60),
              8: ('Fire', 10, 70), 11: ('Fire', 0, 100)}

WEAPONS = (1, 2, 4, 5, 9, 10)
ANIM_WEAPONS = (3, 6, 7, 8, 11)
DTS = ((1,), (12,), (30,), (12, 30, 1, 20, 12, 6, 30, 2))
MAIN_CALLS = {(1,): 72, (12,): 48, (30,): 30}
SCRIPTS = ((1,), (1, 0, 1, 1, 1, 2), (2, 1, 1))
# (weapon, dt pattern, script, quad, {main call: selected weapon}, held rule,
#  main calls or None)
SWITCHES = ((4, (12,), (1,), True, {10: 9, 30: 4}, 'early', None),
            (5, (30,), (1, 1, 0), False, {5: 1}, 'late', None),
            (9, DTS[3], (1,), True, {0: 10}, 'always', None),
            (10, (20,), (1, 2, 1), False, {3: 2, 8: 5}, 'early', None),
            (2, (1,), (1,), False, {2: 4}, 'never', None),
            (1, (12,), (1, 1, 2), True, {0: 9, 20: 1}, 'always', None))
# The moves: a fire clip that ends while its move still plays, a swap with a
# move on the record, a cooked grenade, a re-press past the cook position, a
# grenade whose spawn failed, and a one-tick BFG.
ANIM_CASES = ((8, (12,), (1,), False, {30: 4, 50: 8}, 'early', 80),
              (3, (30,), (1,), False, {12: 7}, 'always', 40),
              (11, (1,), (1,), False, {}, 'default', 72),
              (7, (20,), (1, 1, 2), True, {}, 'late', 48),
              (6, (12,), (1,), False, {}, 'cook', 100),
              (6, (30,), (1,), True, {}, 'cook', 50),
              (6, DTS[3], (1,), False, {}, 'cook', 90),
              (6, (12,), (1,), False, {}, 'retap', 100),
              (6, (12,), (0, 1), False, {}, 'cook', 100),
              (6, (20,), (1,), False, {40: 3}, 'cook', 80))


def held_at(rule, i, n):
    if rule == 'always':
        return True
    if rule == 'never':
        return False
    if rule == 'late':
        return i >= n // 3
    if rule == 'early':
        return i < n // 2
    if rule == 'cook':
        return i < (n * 7) // 10
    if rule == 'retap':
        return i < (n * 4) // 10 or (n * 4) // 10 + 2 <= i < (n * 7) // 10
    return i < (n * 3) // 5 or (n * 3) // 4 <= i < (n * 17) // 20


class DriverMachine(LoaderMachine):
    def __init__(self, image, ntsc):
        super().__init__(image, None, ntsc)
        self.script, self.cursor = (1,), 0
        self.grenade = 0
        self.begin_call()

    def begin_call(self):
        self.substep, self.fires, self.selects, self.sounds = 0, [], [], []
        self.quads = self.models = self.rand_calls = self.primes = 0
        self.display_t = None

    def s16(self, address):
        return signed(self.read(address, 2), 16)

    def s32(self, address):
        return signed(self.read(address, 4))

    def intercept(self, pc):
        if FIRE_FAKE <= pc < FIRE_FAKE + 12 * 16:
            weapon = (pc - FIRE_FAKE) // 16
            site = SITES[self.canonical(self.r[31])]
            if self.r[4] != PLAYER:
                raise ValueError('fire function called without the owner')
            answer = self.script[self.cursor % len(self.script)]
            self.cursor += 1
            # 0x80050200: the chaingun's rounds are viewmodel+44 at the call.
            rounds = self.s32(VM + 44) if site == 2 else 1
            self.fires.append([self.substep, site, self.read(VM + 72, 4),
                               self.read(VM + 76, 4), self.s32(VM + 80),
                               rounds, answer])
            if answer == 2:  # A dry weapon is an empty ammo slot.
                index = self.read(self.address(AMMO_INDEX) + 4 * weapon, 4)
                self.write(CLIENT + 108 + 2 * index, 2, 0)
            if weapon == 6 and answer == 1:
                # QPlayerFireGrenade 0x8004EBDC -> AnyoneFireGrenade 0x8004AA6C:
                # state 1 (0x8004AC00), charge 4096 (0x8004AC08), the owner at
                # +740 (0x8004AC10) and a fuse at +244 (0x8004AC20), long
                # enough here that it never expires in the hand.
                self.put(GRENADE, bytes(0x300))
                self.write(GRENADE + 72, 4, 1)
                self.write(GRENADE + 76, 4, 4096)
                self.write(GRENADE + 740, 4, PLAYER)
                self.write(GRENADE + 244, 2, 30000)
                self.grenade = 1
            self.r[2] = answer
            key = f'fire-{site}'
            self.hooks[key] = self.hooks.get(key, 0) + 1
            return True
        pal = self.canonical(pc)
        if pal == 0x8004F87C:      # One state-machine pass per substep.
            self.substep += 1
            return False
        if pal == 0x8004FB68:      # After 0x800506C4, the dry pass's auto-select.
            self.selects.append(self.s16(CLIENT + 102))
            return False
        if pal == 0x80089E28:
            self.rand_calls += 1
            return super().intercept(pc)
        if pal in (0x800739B8, 0x800739E4):   # Is the voice still sounding: no.
            self.r[2] = 0
        elif pal == 0x80073734:               # itm_damage3 at the player.
            self.quads += 1
        elif pal == 0x800737A8:               # A frame-driver weapon sound.
            self.sounds.append([self.substep, SOUNDS[self.sound_of(self.r[4])]])
        elif pal == 0x80073924:               # Stop a voice.
            pass
        elif pal == 0x8006D008:               # Model lookup on a swap.
            self.models += 1
            self.r[2] = MODEL + 0x100 * self.s16(VM + 212)
        elif pal == 0x8006D330:               # A move by name, in the model.
            weapon = (self.r[4] - MODEL) // 0x100
            name = struct.pack('<III', self.r[5], self.r[6], self.r[7]).split(b'\0')[0]
            move = MOVE_TABLE.get(weapon)
            if not move or move[0].encode() != name:
                raise ValueError(f'unexpected move {name!r} on model {weapon}')
            self.r[2] = MOVES + 0x40 * weapon
        elif pal == 0x8007270C and self.canonical(self.r[31]) == 0x8004A4A8:
            self.primes += 1                  # Grenade3's prime sample.
        elif pal == 0x80040800:               # The release's pad rumble.
            pass
        elif pal == 0x8006D43C:               # Patch the barrel key; no state here.
            pass
        elif pal in (0x80089E38, 0x8008A8B8, 0x8006FC1C):
            pass                              # Matrix output only.
        elif pal == 0x80038260:               # The kick: none in these cases.
            self.put(self.r[4], bytes(6))
        elif pal == 0x8006FD38:               # The interpolated translation.
            self.display_t = [self.s32(self.r[5] + 4 * k) for k in range(3)]
        elif pal in (0x8007270C, 0x8007012C):
            raise ValueError(f'unexpected service {pal:08x}')
        else:
            return super().intercept(pc)
        key = f'{pal:08x}'
        self.hooks[key] = self.hooks.get(key, 0) + 1
        return True

    def sound_of(self, handle):
        for address, code in SOUNDS.items():
            if self.read(self.address(address), 4) == handle:
                return address
        raise ValueError(f'unknown sound handle {handle:08x}')


def synthetic_bank(m):
    """The retail key counts and events, synthetic poses and durations."""
    table = m.address(CLIP_TABLE)
    counts, events = [], []
    for w in range(12):
        block = m.read(table + 4 * w, 4)
        ptrs = [m.read(block + 4 * s, 4) for s in range(5)]
        counts.append([(ptrs[s + 1] - ptrs[s]) // 20 for s in range(4)])
        events.append([[signed(m.read(ptrs[s] + 20 * k + 16, 2), 16)
                        for k in range(counts[w][s])] for s in range(4)])
    assert counts[0] == counts[1] and all(c > 0 for row in counts for c in row)
    rng = random.Random(0x4EE0C)
    keys, clips = [], []
    for w in range(1, 12):
        row = []
        for s in range(4):
            first = len(keys)
            for k in range(counts[w][s]):
                jitter = int(s == 2 and k % 4 == 1)
                duration = rng.choice((2, 3, 5)) if jitter else \
                    rng.choice((5, 10, 12, 15, 20, 25, 30, 40, 60))
                if w == 6 and s == 1:
                    # The retail grenade's three fire keys run 550 ticks; the
                    # arm must reach the cook position before they run out.
                    duration = rng.choice((150, 180, 200, 240))
                keys.append([rng.randrange(-2048, 2048) for _ in range(3)] +
                            [rng.randrange(-300, 300) for _ in range(3)] +
                            [duration, jitter, events[w][s][k]])
            row.append([first, counts[w][s]])
        clips.append(row)
    clips.insert(0, clips[0])   # Slot 0 aliases slot 1, as the table does.
    blocks, data = BANK, BANK + 12 * 20
    for w in range(12):
        for s in range(4):
            m.write(blocks + w * 20 + 4 * s, 4, data + 20 * clips[w][s][0])
        m.write(blocks + w * 20 + 16, 4, data + 20 * (clips[w][3][0] + clips[w][3][1]))
        m.write(table + 4 * w, 4, blocks + w * 20)
    for i, key in enumerate(keys + [[0] * 9]):   # A pad key after the last clip.
        m.put(data + 20 * i, struct.pack('<9hh', *key, 0))
    for w, (name, start, end) in MOVE_TABLE.items():
        m.put(MOVES + 0x40 * w, name.encode().ljust(12, b'\0') +
              struct.pack('<HHHH', start * 5, end * 5, end * 5, 1))
    return counts, keys, clips


def seed(m, weapon, quad, rng_seed):
    """The constructor's writes (0x8004F708..0x8004F81C), the owner and client."""
    m.put(VM, bytes(0x300))
    m.put(PLAYER, bytes(0x400))
    m.put(CLIENT, bytes(0x200))
    m.put(INPUT, bytes(16))
    m.write(PLAYER + 12, 4, CLIENT)          # 0x8004EE50 / 0x8004F8BC
    m.write(PLAYER + 244, 2, 1)              # live owner (0x8004A3A8)
    m.write(PLAYER + 252, 2, 4096)           # copied at 0x8004EE70
    m.write(PLAYER + 264, 2, 100)            # health (0x8004A398)
    m.write(CLIENT, 4, m.address(WINDOW))    # player index 0 (0x8004F8C4)
    m.write(CLIENT + 40, 4, PLAYER)          # 0x800506E0: owner of the client
    m.write(CLIENT + 98, 2, weapon)          # the weapon in hand (0x8004FF40)
    m.write(CLIENT + 102, 2, weapon)         # the selection (0x8004FAB8)
    m.write(CLIENT + 104, 4, 0xFFFFFFFF)     # every weapon owned (0x800506FC)
    for k in range(6):
        m.write(CLIENT + 108 + 2 * k, 2, 200)   # ammo (0x800501E8, 0x80050728)
    m.write(CLIENT + 152, 2, 4096)           # the barrel ramp at rest
    m.write(CLIENT + 168, 4, VM)             # the view weapon (0x8004A3C0)
    m.write(CLIENT + 172, 4, 0x7FFFFFFF if quad else 0)   # 0x8004F8E8
    m.write(VM + 16, 4, MODEL + 0x100 * weapon)
    m.write(VM + 32, 4, FIRE_FAKE + 16 * weapon)  # 0x8004F81C
    m.write(VM + 44, 4, 1)                   # 0x8004F7C0
    m.write(VM + 48, 4, 1)                   # 0x8004F7B4
    m.write(VM + 68, 4, PLAYER)              # 0x8004F774
    m.write(VM + 72, 4, 3)                   # 0x8004F7A4: LOWER
    m.write(VM + 212, 2, weapon)             # 0x8004F798
    m.write(VM + 214, 2, weapon)             # 0x8004F7C4
    m.write(VM + 218, 2, 1)                  # 0x8004F7B0 (s1 = 1, 0x8004F748)
    # +258, +695, +736 and +740 are zero (0x8004F7E4..0x8004F7FC).
    m.write(m.address(HALT), 2, 0)
    m.write(m.address(RESULT), 2, 0)
    m.write(m.address(LEVEL_TIME), 4, 1000)
    m.rng_state = rng_seed
    m.grenade = 0


def execute(cue, ntsc):
    disc = Form1Disc(cue)
    try:
        exe = disc.read('/SLUS_007.57' if ntsc else '/SLES_015.34')
    finally:
        disc.stream.close()
    image = exe[2048:2048 + struct.unpack_from('<I', exe, 0x1C)[0]]
    digest = hashlib.sha256(image).hexdigest()
    assert digest == (NTSC_SEGMENT_SHA if ntsc else SEGMENT_SHA)
    m = DriverMachine(image, ntsc)
    counts, keys, clips = synthetic_bank(m)
    for w in range(12):
        m.write(m.address(FIRE_TABLE) + 4 * w, 4, FIRE_FAKE + 16 * w)
    for address in SOUNDS:
        m.write(m.address(address), 4, 0x5000 + SOUNDS[address])
    m.write(m.address(0x800B28B0), 4, 0x5010)
    m.write(m.address(VIEWS), 4, 0)
    m.write(m.address(LIGHTS), 2, 0)

    plans = []
    for weapon in WEAPONS:
        for dts in DTS:
            for script in SCRIPTS:
                plans.append((weapon, dts, script, False, {}, 'default', None))
    plans += list(SWITCHES)
    for weapon in ANIM_WEAPONS:
        for dts in DTS[1:]:
            for script in SCRIPTS:
                plans.append((weapon, dts, script, False, {}, 'default', None))
    plans += list(ANIM_CASES)
    sequences, calls = [], []
    for index, (weapon, dts, script, quad, selects, rule, n) in enumerate(plans):
        rng_seed = (0x1234 + 7919 * index) & 0xFFFFFFFF
        seed(m, weapon, quad, rng_seed)
        m.script, m.cursor = script, 0
        n = n or MAIN_CALLS.get(dts, 48)
        steps = [(30, False, -1)] * 10
        steps += [(dts[i % len(dts)], held_at(rule, i, n), selects.get(i, -1))
                  for i in range(n)]
        countdown = 0
        sequences.append({'weapon': weapon, 'quad': int(quad), 'seed': rng_seed,
                          'script': list(script), 'first': len(calls),
                          'count': len(steps)})
        for dt, held, select in steps:
            # The cycle routine 0x8004ECB4 runs first in the player think
            # (0x8003A8E0, before the driver call at 0x8003AD98): a selection
            # arms the 70-tick countdown (0x8004ECF0) and leaves +214 for the
            # next frame; otherwise the countdown falls by dt (0x8004ED54,
            # clamped at -1 by 0x8004ED70) and +214 follows the selection.
            if select >= 0:
                m.write(CLIENT + 102, 2, select)
                countdown = 70
            else:
                countdown = signed((countdown & 0xFF) - dt, 8)
                if countdown < -1:
                    countdown = -1
                if m.s16(CLIENT + 102) != m.s16(VM + 214):
                    m.write(VM + 214, 2, m.read(CLIENT + 102, 2))
            m.write(CLIENT + 222, 1, countdown)
            m.write(INPUT + 11, 1, int(held))
            m.write(m.address(DT), 4, dt)
            m.begin_call()
            returned = signed(m.call(DRIVER, VM, INPUT), 16)
            assert returned == m.s16(m.address(RESULT))
            if m.display_t is None:
                raise ValueError('the driver did not reach its placement')
            # The held grenade thinks after the player in the same frame (the
            # client's order, and the one the driver's +740 read assumes).
            # Only its held state is executed: once 0x8004A4D0 has thrown it
            # (state 2) its flight is outside this fixture.
            released, charge = 0, 0
            if m.grenade:
                before = m.s32(GRENADE + 76)
                m.call(GRENADE_THINK, GRENADE)
                charge = m.s32(GRENADE + 76) - before
                if m.read(GRENADE + 72, 4) != 1:
                    released, m.grenade = 1, 0
            base = [m.s16(VM + off) for off in (224, 226, 228, 236, 238, 240)]
            record = m.read(VM + 736, 4)
            calls.append({
                'dt': dt, 'held': int(held), 'select': select,
                'countdown': countdown, 'weapon': m.s16(VM + 212),
                # The selection the pass compares against (0x8004FAB8); +214,
                # which the swap reads, follows it a frame later on a press.
                'pending': m.s16(CLIENT + 102), 'state': m.read(VM + 72, 4),
                'frame': m.read(VM + 76, 4), 'total': m.s32(VM + 48),
                'left': m.s32(VM + 80), 'latch': m.s16(VM + 216),
                'accum': m.s32(VM + 52), 'rate': m.s32(VM + 44),
                'cache': m.s16(VM + 218), 'result': returned,
                'halt': m.s16(m.address(HALT)), 'base': base,
                'display': [m.s16(VM + off) for off in (230, 232, 234)] + m.display_t,
                'ramp': m.s16(CLIENT + 152), 'rng': m.rng_state,
                'rand_calls': m.rand_calls, 'substeps': m.substep,
                'fires': m.fires, 'selects': m.selects,
                'quad': m.quads, 'models': m.models,
                'sound': m.sounds[-1][1] if m.sounds else 0,
                'sounds': m.sounds,
                'anim_pos': m.s16(VM + 256), 'anim_flags': m.read(VM + 258, 2),
                'move_end': m.read(record + 14, 2) if record else -1,
                'mark': m.s32(VM + 740), 'primes': m.primes,
                'released': released, 'charge': charge, 'live': m.grenade})
    return {'region': 'US' if ntsc else 'PAL', 'sha256': digest,
            'counts': counts, 'keys': keys, 'clips': clips,
            'sequences': sequences, 'calls': calls, 'hooks': m.hooks}


def header(run):
    lines = ['/* Generated by generate_viewweapon_driver_reference.py from complete',
             ' * PAL/US executions of the view-weapon driver 0x8004EE0C over a',
             ' * synthetic bank with the retail key counts. Numeric results only. */',
             '#ifndef Q2_VIEWWEAPON_DRIVER_REFERENCE_H',
             '#define Q2_VIEWWEAPON_DRIVER_REFERENCE_H',
             '/* r[3], t[3], duration, jitter, event */',
             'static const short native_vw_keys[][9] = {']
    lines += ['    {' + ','.join(map(str, k)) + '},' for k in run['keys']]
    lines += ['};', '/* [weapon][state] = { first key, count } */',
              'static const int native_vw_clips[12][4][2] = {']
    lines += ['    {' + ','.join('{%d,%d}' % tuple(c) for c in row) + '},'
              for row in run['clips']]
    lines += ['};', '/* [weapon] = { start, end } of the model\'s move, block-D units;',
              ' * the name is the one 0x80050454 asks for. -1 when it asks none. */',
              'static const int native_vw_moves[12][2] = {']
    lines += ['    {%d,%d},' % (MOVE_TABLE[w][1:] if w in MOVE_TABLE else (-1, -1))
              for w in range(12)]
    lines += ['};',
              'typedef struct { int weapon, quad; unsigned seed; int script[6], script_len,',
              '                 first, count; } native_vw_sequence;',
              'static const native_vw_sequence native_vw_sequences[] = {']
    for s in run['sequences']:
        script = s['script'] + [0] * (6 - len(s['script']))
        lines.append('    {%d,%d,%uu,{%s},%d,%d,%d},' % (
            s['weapon'], s['quad'], s['seed'], ','.join(map(str, script)),
            len(s['script']), s['first'], s['count']))
    lines += ['};',
              'typedef struct {',
              '    int dt, held, select, countdown;',
              '    int weapon, pending, state, frame, total, left, latch, accum, rate,',
              '        cache, result, halt, base[6], display[6], ramp;',
              '    unsigned rng; int rand_calls, substeps, quad, models, sound;',
              '    int fire_first, fire_count, select_first, select_count;',
              '    /* +256, +258, the playing record\'s end (-1 none), +740, and the',
              '     * held grenade think\'s primes, release, charge and liveness */',
              '    int anim_pos, anim_flags, move_end, mark, primes, released,',
              '        charge, live;',
              '} native_vw_call;',
              'static const native_vw_call native_vw_calls[] = {']
    fires, selects = [], []
    for c in run['calls']:
        values = [c['dt'], c['held'], c['select'], c['countdown'], c['weapon'], c['pending'],
                  c['state'], c['frame'], c['total'], c['left'], c['latch'],
                  c['accum'], c['rate'], c['cache'], c['result'], c['halt']]
        text = ','.join(map(str, values))
        text += ',{' + ','.join(map(str, c['base'])) + '}'
        text += ',{' + ','.join(map(str, c['display'])) + '}'
        text += ',%d,%uu,%d,%d,%d,%d,%d' % (c['ramp'], c['rng'], c['rand_calls'],
                                           c['substeps'], c['quad'], c['models'],
                                           c['sound'])
        text += ',%d,%d,%d,%d' % (len(fires), len(c['fires']), len(selects),
                                  len(c['selects']))
        text += ',' + ','.join(str(c[k]) for k in ('anim_pos', 'anim_flags', 'move_end',
                                                    'mark', 'primes', 'released',
                                                    'charge', 'live'))
        fires += c['fires']
        selects += c['selects']
        lines.append('    {' + text + '},')
    lines += ['};',
              '/* substep (1-based), site (0 idle 0x8004FB30, 1 machinegun 0x8004FFE0,',
              ' * 2 chaingun 0x8005020C, 3 hyperblaster 0x800502D8), state, frame, left',
              ' * at the call, rounds, the scripted answer */',
              'static const int native_vw_fires[][7] = {']
    lines += ['    {' + ','.join(map(str, f)) + '},' for f in fires or [[0] * 7]]
    lines += ['};', '/* What 0x800506C4 selected on each dry pass, in order. */',
              'static const int native_vw_selects[] = {']
    lines += ['    ' + ','.join(map(str, selects[i:i + 16])) + ','
              for i in range(0, len(selects), 16)] or ['    0']
    lines += ['};',
              '#define NATIVE_VW_FIRES %d' % len(fires),
              '#define NATIVE_VW_SELECTS %d' % len(selects),
              '#endif', '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pal', type=Path)
    parser.add_argument('us', type=Path)
    parser.add_argument('--trace', required=True, type=Path)
    parser.add_argument('--output', type=Path,
                        default=Path(__file__).with_name('viewweapon_driver_reference.h'))
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    runs = [execute(args.pal, False), execute(args.us, True)]
    for key in ('counts', 'keys', 'clips', 'sequences', 'calls'):
        assert runs[0][key] == runs[1][key], f'PAL/US view-weapon driver {key} differ'
    text = header(runs[0])
    if args.check:
        assert args.output.read_text(encoding='utf-8') == text, \
            'stored view-weapon driver fixture differs from native execution'
    else:
        args.output.write_text(text, encoding='utf-8')
    args.trace.parent.mkdir(parents=True, exist_ok=True)
    args.trace.write_text(json.dumps([{k: v for k, v in r.items() if k != 'calls'} |
                                      {'calls': len(r['calls'])} for r in runs],
                                     indent=2) + '\n', encoding='utf-8')
    calls = runs[0]['calls']
    print(f"Native view-weapon driver: {len(runs[0]['sequences'])} sequences, "
          f"{len(calls)} calls, {sum(len(c['fires']) for c in calls)} fire calls, "
          f"{sum(c['models'] for c in calls)} swaps, "
          f"{sum(1 for c in calls if c['move_end'] >= 0)} calls with a move on the record, "
          f"{sum(c['primes'] for c in calls)} primes, {sum(c['released'] for c in calls)} "
          f"releases per region; PAL/US agree")


if __name__ == '__main__':
    main()
