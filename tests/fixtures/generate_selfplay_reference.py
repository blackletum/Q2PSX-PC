"""Execute PAL/US SelfPlayOn and SwapAllButtons over the three attract recordings.

SelfPlayOn (0x80033120, US 0x8003311C) runs unchanged on the module address
each level's init passes it, with its four settings resets hooked. Each frame
then runs the complete SwapAllButtons (0x80033030, US 0x8003302C) — SwapButtons
for all four players and the NoInputTime test — with NewVcounts preset to the
frame MainGameLoop would otherwise give it (two fields) and ChrisCount advanced
by one afterwards, as the frame routine does. The DualShock motor call is a
hook, and so is NoInputTimeOutRoutine, which only counts its calls.

Each recording is played until the routine fires. Three exits are executed on
BASE3 as well: a real pad press, player one's entity at zero health, and a
NoInputTime of 20 over a synthetic zero-button recording. The level's LevelBin
is read from the user's disc into native RAM at 0x80100000; the recordings need
no relocation. Only counts and FNV-1a hashes of the per-frame (buttons,
NewVcounts) pairs are written, no retail data.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from generate_actor_light_reference import chunks
from generate_heap_reference import Form1Disc, LoaderMachine, NTSC_SEGMENT_SHA
from generate_resource_reference import SEGMENT_SHA

SELFPLAYON, SWAPALL, QUAKE_END = 0x80033120, 0x80033030, 0x80040EA4
HOOKS = {0x8001BDA8: 'player', 0x8001FA50: 'sound', 0x8001FA18: 'video',
         0x8001C29C: 'install', 0x8007CCFC: 'shock'}
WINDOWS, STRIDE = 0x800D5C30, 0x310
PAD = WINDOWS + 0x2AC
NEWV, RATE, CHRIS = 0x800B2DB4, 0x800B2DD8, 0x800B2DE4
LAST, NOINPUT, ROUTINE = 0x800B2874, 0x800B2878, 0x800B2884
INSELF, MULTI, RUNMODE = 0x800B2A24, 0x800AEBCC, 0x800B2AA8
ROUTINE_FAKE, MODULE, ENTITY = 0x801E0000, 0x80100000, 0x801D0000
MAPS = (('BASE3', 0x8B4), ('JAIL2', 0x788), ('WASTE2', 0x7F4))
CHECKPOINT = 25
LIMIT = 4000


class SelfPlayMachine(LoaderMachine):
    def __init__(self, image, ntsc):
        super().__init__(image, None, ntsc)
        self.counts = {name: 0 for name in HOOKS.values()}
        self.fired = 0

    def intercept(self, pc):
        if pc == ROUTINE_FAKE:
            self.fired += 1
            return True
        if pc < 0x80018000:
            raise ValueError(f'Unhooked BIOS call {self.r[9]:02x}, return {self.r[31]:08x}')
        canon = self.canonical(pc)
        if canon in HOOKS:
            self.counts[HOOKS[canon]] += 1
            return True
        return False


def fnv(h, value):
    for byte in value.to_bytes(4, 'little'):
        h = ((h ^ byte) * 0x01000193) & 0xFFFFFFFF
    return h


def play(image, ntsc, module, record, real_at=None, dead_at=None,
         no_input=900, synthetic=None):
    m = SelfPlayMachine(image, ntsc)
    rate = 5 if ntsc else 6
    m.put(MODULE, module)
    if synthetic is not None:
        m.put(MODULE + record, b''.join(struct.pack('<H', w) for w in synthetic))
    m.write(m.address(RATE), 4, rate)
    m.write(m.address(RUNMODE), 4, 0)
    m.write(m.address(MULTI), 4, 0)
    m.write(m.address(CHRIS), 4, 0)
    m.write(m.address(LAST), 4, 0)
    m.write(m.address(INSELF), 4, 1)                 # GameSelfPlay
    m.call(SELFPLAYON, 0, MODULE + record, m.address(QUAKE_END))
    m.write(m.address(NOINPUT), 4, no_input)         # *engine+0x6C
    m.write(m.address(ROUTINE), 4, ROUTINE_FAKE)     # engine+0x70
    pad = m.address(PAD)
    m.write(ENTITY + 0x108, 2, 45)
    m.write(m.address(WINDOWS) + 0x120, 4, ENTITY if dead_at is not None else 0)
    h, checkpoints, frames, units, pressed = 0x811C9DC5, [], 0, 0, 0
    first = None
    while frames < LIMIT:
        m.write(m.address(NEWV), 4, 2 * rate)        # MainGameLoop's own frame
        m.write(pad + 4, 4, 0x40 if real_at is not None and frames >= real_at else 0)
        if dead_at is not None and frames >= dead_at:
            m.write(ENTITY + 0x108, 2, 0)
        m.call(SWAPALL)
        buttons = m.read(pad + 12, 4)
        newv = m.read(m.address(NEWV), 4)
        mode = m.read(pad + 28, 2)
        if m.fired:
            first = {'frame': frames, 'buttons': buttons, 'mode': mode,
                     'inself': m.read(m.address(INSELF), 4),
                     'no_input': m.read(m.address(NOINPUT), 4),
                     'cursor': m.read(pad + 32, 4) - MODULE - record}
            break
        h = fnv(fnv(h, buttons), newv)
        units += newv
        pressed += int(buttons != 0)
        frames += 1
        if frames % CHECKPOINT == 0:
            checkpoints.append(h)
        m.write(m.address(CHRIS), 4, m.read(m.address(CHRIS), 4) + 1)
    if first is None:
        raise ValueError('the routine never fired')
    return {'frames': frames, 'hash': h, 'units': units, 'pressed': pressed,
            'checkpoints': checkpoints, 'exit': first, 'hooks': m.counts}


def execute(cue, ntsc):
    disc = Form1Disc(cue)
    try:
        exe = disc.read('/SLUS_007.57' if ntsc else '/SLES_015.34')
        modules = {name: chunks(disc.read(f'/Q2DATA/LEVELS/{name}/COMMON.DAT'))['LevelBin']
                   for name, _ in MAPS}
    finally:
        disc.stream.close()
    image = exe[2048:2048 + struct.unpack_from('<I', exe, 0x1C)[0]]
    digest = hashlib.sha256(image).hexdigest()
    assert digest == (NTSC_SEGMENT_SHA if ntsc else SEGMENT_SHA)
    runs = []
    for name, record in MAPS:
        run = play(image, ntsc, modules[name], record)
        run.update(map=name, record=record)
        runs.append(run)
    base3 = modules['BASE3']
    exits = [
        dict(kind=1, at=200, **play(image, ntsc, base3, 0x8B4, real_at=200)['exit']),
        dict(kind=2, at=150, **play(image, ntsc, base3, 0x8B4, dead_at=150)['exit']),
        dict(kind=3, at=20, **play(image, ntsc, base3, 0x8B4, no_input=20,
                                   synthetic=[0x0001] * 40 + [0xFFFF])['exit']),
    ]
    return {'region': 'US' if ntsc else 'PAL', 'sha256': digest,
            'runs': runs, 'exits': exits}


def header(results):
    lines = ['/* Generated by generate_selfplay_reference.py from complete PAL/US',
             ' * executions of SelfPlayOn and SwapAllButtons over the three attract',
             ' * recordings. Counts and FNV-1a hashes of (buttons, NewVcounts) only. */',
             '#ifndef Q2_SELFPLAY_REFERENCE_H',
             '#define Q2_SELFPLAY_REFERENCE_H',
             '#define NATIVE_SP_CHECKPOINT %d' % CHECKPOINT,
             'typedef struct {',
             '    int ntsc; const char *map; unsigned record;',
             '    /* frames played before the routine fired, then the hash, units and',
             '     * button frames over them; the cursor where it stopped */',
             '    unsigned frames, hash, units, pressed, cursor;',
             '    unsigned checkpoints[64]; unsigned checkpoint_count;',
             '} native_sp_run;',
             'static const native_sp_run native_sp_runs[] = {']
    for r in results:
        ntsc = int(r['region'] == 'US')
        for run in r['runs']:
            cps = run['checkpoints']
            assert len(cps) <= 64
            lines.append('    {%d,"%s",0x%X,%d,0x%08Xu,%d,%d,%d,{%s},%d},' % (
                ntsc, run['map'], run['record'], run['frames'], run['hash'],
                run['units'], run['pressed'], run['exit']['cursor'],
                ','.join('0x%08Xu' % c for c in cps), len(cps)))
    lines += ['};',
              '/* kind 1 = real pad from frame `at`, 2 = dead from `at`, 3 = NoInputTime',
              ' * 20 over zero-button words. `frame` is the frame the routine fired on;',
              ' * the rest is the state SwapAllButtons left. */',
              'typedef struct { int ntsc, kind, at, frame; unsigned buttons, mode, inself,',
              '                 no_input, cursor; } native_sp_exit;',
              'static const native_sp_exit native_sp_exits[] = {']
    for r in results:
        ntsc = int(r['region'] == 'US')
        for e in r['exits']:
            lines.append('    {%d,%d,%d,%d,%d,%d,%d,%d,%d},' % (
                ntsc, e['kind'], e['at'], e['frame'], e['buttons'], e['mode'],
                e['inself'], e['no_input'], e['cursor']))
    lines += ['};', '#endif', '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pal', type=Path)
    parser.add_argument('us', type=Path)
    parser.add_argument('--trace', type=Path)
    parser.add_argument('--output', type=Path,
                        default=Path(__file__).with_name('selfplay_reference.h'))
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    results = [execute(args.pal, False), execute(args.us, True)]
    text = header(results)
    if args.check:
        assert args.output.read_text(encoding='utf-8') == text, \
            'stored self-play fixture differs from native execution'
    else:
        args.output.write_text(text, encoding='utf-8')
    if args.trace:
        args.trace.parent.mkdir(parents=True, exist_ok=True)
        args.trace.write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
    for r in results:
        print(r['region'], ', '.join('%s %d frames' % (run['map'], run['frames'])
                                     for run in r['runs']),
              '; exits at', [e['frame'] for e in r['exits']])


if __name__ == '__main__':
    main()
