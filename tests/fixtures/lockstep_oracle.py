"""Run the console's own game loop over an attract demo, natively.

The loader machine (generate_heap_reference.py) executes a level load; this
adds what a running game needs on top of it: a GTE (gte.py), the scratchpad
and I/O space, LWC2/SWC2, and the BIOS srand/rand pair with every call
recorded. It then enters a self-play level the way ProcessGame's state 10
does (GameSelfPlay, GameNewMission, srand(13), the level resolve, InitLevel)
and runs MainGameLoop's per-frame order: SwapAllButtons, which feeds the
recording and NewVcounts, then QuakeLoop. Drawing is stubbed at the GPU.

    python tests/fixtures/lockstep_oracle.py <PAL.cue> --frames 150 \
        --snapshot .agents/tmp/lockstep/base3.snap --out native.json [--probes]

The output is per record frame: every rand() call (its caller and result),
player one's origin and health, every creature's actor and edict (creatures),
the active-area mask, the closed doors and the entity box table. --snapshot
keeps the state after the level load for later runs, and --probes adds the
diagnostic events listed on OracleMachine.probe. It is a diagnostic, not a
fixture header; nothing from the disc is written.
"""
import argparse
import json
import pickle
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_heap_reference import Form1Disc, LoaderMachine, configure_level  # noqa: E402
from generate_combat_reference import signed  # noqa: E402
from gte import GTE  # noqa: E402

SCRATCH = 0x1F800000
SRAND, RAND = 0x80086068, 0x80089E28
GAME_SELF_PLAY, GAME_NEW_MISSION, LEVEL_RESOLVE = 0x80041300, 0x8003F37C, 0x8007C54C
INIT_LEVEL, AFTER_LOAD = 0x80070D84, 0x8007CA44
SWAP_ALL, QUAKE_LOOP = 0x80033030, 0x80038E70
NEWV, RATE = 0x800B2DB4, 0x800B2DD8
WINDOWS, PAD = 0x800D5C30, 0x800D5C30 + 0x2AC
OBJECTS = 0x800CBA28
FAKE_OT = 0x80400000


class OracleMachine(LoaderMachine):
    def __init__(self, image, disc):
        super().__init__(image, disc, False)
        # Room past the console's 2 MB for the stand-in ordering tables the
        # draw links into (FAKE_OT); nothing the game owns lives up there.
        self.mem.extend(bytes(6 * 1024 * 1024))
        self.gte = GTE()
        self.scratch = bytearray(1024)
        self.rng_state = 0
        self.rand_log = []
        # Device work with no bearing on the simulation: CD audio.
        self.stubs = {0x80071B94: 0,   # RestartXA
                      0x800892A8: 0,   # VSync: the interpreter has no vertical blank
                      0x800B007C: 0,   # the model draw's OT insertion (0x80069980)
                      0x80032DCC: 0}   # SnapShotPadViolation: a pad is plugged in, or
                                       # GameCheckPaused pauses the run (0x8003E858)
        self.count = 0
        self.probes = False

    # --------------------------------------------------------------- memory
    def read(self, address, size, sign=False):
        a = address & 0x1FFFFFFF
        if a >= 0x1F800000:
            if a < 0x1F800400:
                o = a - 0x1F800000
                return int.from_bytes(self.scratch[o:o + size], 'little', signed=sign) & 0xFFFFFFFF
            if 0x1F801C00 <= a < 0x1F801E00:
                return super().read(address, size, sign)
            if a == 0x1F801814:            # GPUSTAT: idle and ready for everything
                return 0x1C000000
            return 0
        return super().read(address, size, sign)

    def write(self, address, size, value):
        a = address & 0x1FFFFFFF
        if getattr(self, 'watch', None) and a < self.watch[1] and a + size > self.watch[0]:  # scratch watch
            print('WATCH', hex(getattr(self, 'cur_pc', 0)), hex(a), size, value & ((1 << (8 * size)) - 1), flush=True)
        if a >= 0x1F800000:
            if a < 0x1F800400:
                o = a - 0x1F800000
                self.scratch[o:o + size] = (value & ((1 << (size * 8)) - 1)).to_bytes(size, 'little')
                return
            if 0x1F801C00 <= a < 0x1F801E00:
                return super().write(address, size, value)
            return
        super().write(address, size, value)

    def put(self, address, data):
        a = address & 0x1FFFFFFF
        if a >= 0x1F800000:
            o = a - 0x1F800000
            if o + len(data) > len(self.scratch):
                raise ValueError('Write beyond the scratchpad')
            self.scratch[o:o + len(data)] = data
            return
        super().put(address, data)

    def string(self, address):
        a = address & 0x1FFFFFFF
        if a >= 0x1F800000:
            o = a - 0x1F800000
            end = self.scratch.find(0, o)
            if end < 0:
                raise ValueError(f'Unterminated string at {a:08x}')
            return bytes(self.scratch[o:end]).decode('ascii', errors='backslashreplace')
        return super().string(address)

    def get(self, address, size):
        a = address & 0x1FFFFFFF
        if a >= 0x1F800000:
            o = a - 0x1F800000
            return bytes(self.scratch[o:o + size])
        return bytes(self.mem[a:a + size])

    # ------------------------------------------------------------ intercept
    def intercept(self, pc):
        if pc == SRAND:
            self.rng_state = self.r[4] & 0xFFFFFFFF
            self.rand_log.append(('srand', self.r[31], self.r[4]))
            return True
        if pc == RAND:
            self.rng_state = (self.rng_state * 0x41C64E6D + 0x3039) & 0xFFFFFFFF
            self.r[2] = (self.rng_state >> 16) & 0x7FFF
            self.rand_log.append(('rand', self.r[31], self.r[2]))
            return True
        if pc == 0x8007270C:
            self.sound_variation()
        if self.probes:
            self.probe(pc)
        stub = self.stubs.get(pc)
        if stub is not None:
            self.r[2] = stub
            return True
        r = self.r
        if pc == 0x8008A0D8:  # A(1Ah) strncpy
            src = self.string(r[5]).encode('ascii', 'replace')[:r[6]]
            self.put(r[4], src + bytes(r[6] - len(src)))
            r[2] = r[4]
            return True
        if pc == 0x80095798:  # A(18h) strncmp
            a, b, n = self.string(r[4])[:r[6]], self.string(r[5])[:r[6]], r[6]
            r[2] = next((ord(x) - ord(y) for x, y in zip(a + '\0', b + '\0') if x != y), 0) & 0xFFFFFFFF
            return True
        if pc in (0x8008A7B8, 0x8008A0C8):  # A(1Eh) strchr / A(1Fh) strrchr
            s, ch = self.string(r[4]), chr(r[5] & 0xFF)
            i = s.find(ch) if pc == 0x8008A7B8 else s.rfind(ch)
            if ch == '\0':
                i = len(s)
            r[2] = (r[4] + i) & 0xFFFFFFFF if i >= 0 else 0
            return True
        if pc == 0x8008AA68 and r[31] != self.address(0x8006E9AC):  # A(24h) strstr
            i = self.string(r[4]).find(self.string(r[5]))
            r[2] = (r[4] + i) & 0xFFFFFFFF if i >= 0 else 0
            return True
        if pc == 0x80091268:  # A(28h) bzero
            self.put(r[4], bytes(r[5]))
            return True
        if pc == 0x80094BE8:  # A(2Eh) memchr
            a = r[4] & 0x1FFFFFFF
            i = self.mem.find(bytes([r[5] & 0xFF]), a, a + r[6])
            r[2] = (r[4] + (i - a)) & 0xFFFFFFFF if i >= 0 else 0
            return True
        if pc == 0x80082940:  # A(44h) FlushCache
            return True
        if pc in (0x8008AD9C, 0x8008ADAC):  # memcpy / backward move, either space
            self.put(r[4], self.get(r[5], r[6]))
            r[2] = r[4]
            return True
        return super().intercept(pc)

    def sound_variation(self):
        """The randomising sound start's two draws, which this machine never reaches.

        0x8007270C returns before them when its sample is not in the bank
        table (0x80072784) or no voice is free (0x800740DC). The loader stubs
        the sample upload (0x800724EC), so here every sample is missing and no
        sound ever draws, where a console with its samples loaded and one of
        24 voices free does: rand() at 0x800727BC for the pitch when the
        request's flags (+8) have bit 0, and at 0x8007281C for the volume
        when they have bit 1, each `lower + rand() % (upper - lower)`. This
        makes those draws as the console would and stores their results.
        """
        idx = signed(self.r[4])
        if idx in (-1, 0):
            return
        rec = 0x800DFDE4 + 12 * idx
        flags = self.read(rec + 8, 4)
        for bit, hi, lo, out, ra in ((1, 4, 5, 1, 0x800727C4), (2, 6, 7, 2, 0x80072824)):
            if not flags & bit:
                continue
            span = self.read(rec + hi, 1) - self.read(rec + lo, 1)
            self.rng_state = (self.rng_state * 0x41C64E6D + 0x3039) & 0xFFFFFFFF
            v = (self.rng_state >> 16) & 0x7FFF
            self.rand_log.append(('rand', ra, v))
            if span:
                m = abs(v) % abs(span)
                self.write(rec + out, 1, (self.read(rec + lo, 1) + m) & 0xFF)

    # -------------------------------------------------------------- probes
    def probe(self, pc):
        """The diagnostic events --probes adds to each frame's 'rand' list.

        Each is (kind, address, values); compare.py and frame.py read only
        the 'rand' entries. gibs: the smash and ThrowGibs with the parent's
        motion; sub/subret: the box trace's hull walk, entity sweep and body
        query; trace/traced: the box trace with its start cell and result;
        bottom: M_CheckBottom; pmv/pmvret: player one's stepped move,
        separation and slide; feed: an actor pulled back over its edict;
        turnchk/turnhandler, stepdir/stepret: SV_StepDirection; toss: a
        body part's flags, spin, position and halfwords +0xC0..+0xEE; vw:
        the weapon driver; start/go/ai_stand: a creature's start.
        """
        if pc in (0x8007CEB4, 0x8005A3D4):  # the smash and ThrowGibs: the parent's motion
            a = self.r[4]
            self.rand_log.append(('gibs', pc, [a, signed(self.r[5]), signed(self.r[6]), signed(self.r[7])] +
                                  [signed(self.read(a + 0x54 + 4 * i, 4)) for i in range(3)] +
                                  [signed(self.read(a + 0xE0 + 2 * i, 2), 16) for i in range(3)] +
                                  [signed(self.read(a + 0x2F8 + 2 * i, 2), 16) for i in range(3)]))
        if pc in (0x80044C44, 0x80053C58, 0x800544EC):  # the box trace's three passes
            self.sub_ret = getattr(self, 'sub_ret', [])
            self.sub_ret.append((self.r[31], pc))
            self.rand_log.append(('sub', pc, [self.r[4], self.r[5], self.r[6], self.r[7]] +
                                  [signed(self.read(self.r[5] + 4 * i, 4)) for i in range(3)] +
                                  [signed(self.read(self.r[6] + 4 * i, 4)) for i in range(3)]))
        elif getattr(self, 'sub_ret', None) and pc == self.sub_ret[-1][0]:
            _, fn = self.sub_ret.pop()
            extra = []
            if fn == 0x80053C58 and self.r[2] == 0:
                idx = self.read(0x800CBA26, 2)
                box = 0x800CAE10 + 64 * idx
                extra = [idx, self.read(box + 54, 1), self.read(box + 55, 1)] + [signed(self.read(box + 4 * i, 4)) for i in range(6)] + [hex(self.read(box + 56 + 4 * i, 4)) for i in range(2)]
            self.rand_log.append(('subret', fn, [signed(self.r[2])] + [signed(self.read(0x800D4F90 + 4 * i, 4)) for i in range(3)] + extra))
        if pc == 0x8005BD3C:  # the box trace: start, end, start cell
            self.trace_ret = getattr(self, 'trace_ret', [])
            self.trace_ret.append(self.r[31])
            sp = self.r[29]
            self.rand_log.append(('trace', self.r[31], [signed(self.read(self.r[4] + 4 * i, 4)) for i in range(3)] +
                                  [signed(self.read(self.r[7] + 4 * i, 4)) for i in range(3)] +
                                  [signed(self.read(sp + 24, 2), 16)]))
        elif getattr(self, 'trace_ret', None) and pc == self.trace_ret[-1]:
            self.trace_ret.pop()
            res = self.read(0x800AEAE8, 4)
            self.rand_log.append(('traced', pc, [signed(self.read(res + 4 * i, 4)) for i in range(6)] +
                                  [signed(self.read(0x800C9002, 2), 16)]))
        if pc == 0x8005FB24:  # M_CheckBottom
            self.cb_ret = self.r[31]
        elif getattr(self, 'cb_ret', None) and pc == self.cb_ret:
            self.cb_ret = None
            self.rand_log.append(('bottom', pc, signed(self.r[2])))
        if pc in (0x8004583C, 0x80051258, 0x80045144) and self.r[4] == self.read(((self.read(0x800B2FD0, 4) + 324) & 0xFFFFFFFF) + 36, 4):
            a = self.r[4]
            self.pmv = getattr(self, 'pmv', [])
            self.pmv.append((self.r[31], pc))
            self.rand_log.append(('pmv', pc, [hex(self.r[5]), hex(self.r[6]), hex(self.r[7]), hex(self.read(a + 152, 4))] + [signed(self.read(a + 84 + 4 * i, 4)) for i in range(3)] + [signed(self.read(a + 236 + 2 * i, 2), 16) for i in range(3)]))
        elif getattr(self, 'pmv', None) and pc == self.pmv[-1][0]:
            _, fn = self.pmv.pop()
            a = self.read(((self.read(0x800B2FD0, 4) + 324) & 0xFFFFFFFF) + 36, 4)
            self.rand_log.append(('pmvret', fn, [hex(self.r[2])] + [signed(self.read(a + 84 + 4 * i, 4)) for i in range(3)] + [signed(self.read(a + 236 + 2 * i, 2), 16) for i in range(3)]))
        if pc == 0x8005C980:  # the actor's position fed back into the edict
            e = self.r[4]
            a = self.read(e + 36, 4)
            self.rand_log.append(('feed', e, [self.r[5]] + [signed(self.read(e + 4 * i, 4)) for i in range(3)] + [signed(self.read(a + 84 + 4 * i, 4)) for i in range(3)] + [signed(self.read(e + 20, 2), 16), signed(self.read(a + 218, 2), 16), signed(self.read(a + 232, 2), 16), self.r[31]]))
        if pc == 0x800604BC:  # SV_StepDirection's turned-far-enough test
            e = self.r[16]
            self.rand_log.append(('turnchk', e, [signed(self.read(e + 20, 2), 16), signed(self.read(e + 142, 2), 16), signed(self.read(e + 140, 2), 16), signed(self.read(e + 320, 4))] + [signed(self.read(e + 4 * i, 4)) for i in range(3)]))
        if pc == 0x80060408:  # its turn handler, +0x108
            self.rand_log.append(('turnhandler', self.r[2], [signed(self.r[7])]))
        if pc == 0x80060334:  # SV_StepDirection(actor, yaw, dist)
            self.step_ret = getattr(self, 'step_ret', [])
            self.step_ret.append(self.r[31])
            self.rand_log.append(('stepdir', self.r[31], [signed(self.r[5]), signed(self.r[6])]))
        elif getattr(self, 'step_ret', None) and pc == self.step_ret[-1]:
            self.step_ret.pop()
            self.rand_log.append(('stepret', pc, signed(self.r[2])))
        if pc == 0x80046B98:  # the toss / tumble: its object's flags
            self.rand_log.append(('toss', self.r[4], [self.read(self.r[4] + 0x98, 4), self.read(self.r[4] + 0xE6, 2), self.read(0x800AE8B4, 4)] + [signed(self.read(self.r[4] + 0x54 + 4 * i, 4)) for i in range(3)] + [signed(self.read(self.r[4] + o, 2), 16) for o in range(0xC0, 0xF0, 2)]))
        if pc == 0x80065C5C:  # SortData's region test: the area and its packed rectangle
            self.rand_log.append(('region', self.r[16], [signed(self.read(self.r[29] + 78, 2), 16)] +
                                  [signed(self.read(self.r[16] + 2 * i, 2), 16) for i in range(4)] +
                                  [signed(self.read(self.read(0x800B2C24, 4) + 4 * i, 4)) for i in range(3)]))
        if pc in (0x800689F4, 0x800578D8):  # area stamp (player, area) / the creature activity test
            self.rand_log.append(('stamp' if pc == 0x800689F4 else 'activity', self.r[31], [self.r[4], self.r[5]]))
        if pc == 0x801FB724:  # BASE3's Soldier fire, after attachment_local_offset: the local muzzle and its inputs
            o, sp = self.r[17], self.r[29]
            self.rand_log.append(('muzzle', o, [signed(self.read(sp + 144 + 2 * i, 2), 16) for i in range(3)] +
                                  [signed(self.read(o + 256, 2), 16), hex(self.read(o + 176, 4))] +
                                  [signed(self.read(0x801FD7E0 + 2 * i, 2), 16) for i in range(4)] +
                                  [signed(self.read(o + 164 + 4 * i, 4)) for i in range(3)] +
                                  [signed(self.read(o + 704 + 2 * i, 2), 16) for i in range(9)] +
                                  [signed(self.read(o + 0xE6 + 2 * i, 2), 16) for i in range(3)]))
        if pc == 0x8004874C:  # the bullet: shooter object, its edict's frame and yaw, the start and the dir
            o = self.r[4]
            e = self.read(o + 748, 4) if o else 0
            self.rand_log.append(('bullet', o, [signed(self.read(e + 56, 2), 16) if e else 0, signed(self.read(e + 20, 2), 16) if e else 0,
                                                signed(self.read(o + 0xE8, 2), 16)] +
                                  [signed(self.read(self.r[5] + 4 * i, 4)) for i in range(3)] +
                                  [signed(self.read(self.r[6] + 2 * i, 2), 16) for i in range(3)]))
        if pc == 0x8003870C:  # the live camera's roll store: entity +230..234 and view +46..50
            v, e = self.r[16], self.r[5]
            self.rand_log.append(('camera', v, [signed(self.read(e + 230 + 2 * i, 2), 16) for i in range(3)] +
                                  [signed(self.read(v + 46 + 2 * i, 2), 16) for i in range(3)]))
        if pc == 0x80054BD4 and self.r[31] == 0x8004895C:  # GenMissileHitPlane from the bullet: record, plane, point, dir
            a0, a1, a2, a3 = self.r[4], self.r[5], self.r[6], self.r[7]
            self.rand_log.append(('hitplane', a0, [signed(self.read(a0 + 4 * i, 4)) for i in range(3)] +
                                  [signed(self.read(a1 + 2 * i, 2), 16) for i in range(6)] +
                                  [signed(self.read(a2 + 4 * i, 4)) for i in range(3)] +
                                  [signed(self.read(a3 + 2 * i, 2), 16) for i in range(3)]))
        if pc == 0x80058204:  # the knockback just stored: the victim's +0x2F8..+0x2FC and sp+40..48
            v = self.r[18]
            self.rand_log.append(('knock', v, [signed(self.read(v + 760 + 2 * i, 2), 16) for i in range(3)] +
                                  [signed(self.read(self.r[29] + 40 + 4 * i, 4)) for i in range(3)]))
        if pc == 0x80057D54:  # the damage function: caller, a0..a3 and the stack arguments
            self.rand_log.append(('damage', self.r[31], [hex(self.r[4]), hex(self.r[5]), hex(self.r[6]), hex(self.r[7])] +
                                  [signed(self.read(self.r[29] + 16 + 4 * i, 4)) for i in range(6)]))
        if pc in (0x8005E838, 0x8005E968, 0x8005E9CC):  # ai_run's three pursuit traces, returned
            tr = self.read(0x800AEAE8, 4)
            self.rand_log.append(('pursuit', pc, [signed(self.read(tr + 8, 4)), signed(self.r[21]), signed(self.r[19]),
                                                  signed(self.r[20]), signed(self.r[18])] +
                                  [signed(self.read(tr + 12 + 4 * i, 4)) for i in range(3)]))
        if pc in (0x80045144, 0x80053C58):  # player one's move (obj, delta) / its entity sweep (obj, want)
            pl = self.read(((self.read(0x800B2FD0, 4) + 324) & 0xFFFFFFFF) + 36, 4)
            if self.r[4] in (pl, pl + 0x54):
                self.mv_ret = getattr(self, 'mv_ret', [])
                self.mv_ret.append((self.r[31], pc, self.r[5], self.r[4]))
                vec = ([signed(self.read(self.r[5] + 2 * i, 2), 16) for i in range(3)] if pc == 0x80045144
                       else [signed(self.read(self.r[5] + 4 * i, 4)) for i in range(3)])
                self.rand_log.append(('mv' if pc == 0x80045144 else 'esw', pc,
                                      [signed(self.read(self.r[4] + 4 * i, 4)) for i in range(3)] + vec))
        elif getattr(self, 'mv_ret', None) and pc == self.mv_ret[-1][0]:
            _, fn, a1, pl = self.mv_ret.pop()
            self.rand_log.append(('mvret' if fn == 0x80045144 else 'eswret', signed(self.r[2]),
                                  [signed(self.read(pl + 4 * i, 4)) for i in range(3)] +
                                  ([signed(self.read(a1 + 4 * i, 4)) for i in range(3)] if fn == 0x80053C58 else [])))
        if pc in (0x8006087C, 0x80060544):  # M_MoveToGoal(self, dist) / SV_NewChaseDir(self, goal, dist)
            e = self.r[4]
            g = self.read(e + 132, 4) if pc == 0x8006087C else self.r[5]
            self.rand_log.append(('mtg' if pc == 0x8006087C else 'ncd', e,
                                  [signed(self.read(e, 4)), signed(self.read(e + 8, 4)), signed(self.read(e + 142, 2), 16),
                                   signed(self.r[5] if pc == 0x8006087C else self.r[6]), hex(g)] +
                                  ([signed(self.read(g, 4)), signed(self.read(g + 8, 4))] if g else [])))
        if pc == 0x800565C4:  # spawn_entity_from_placement(record): the record as spawned
            a = self.r[4]
            self.rand_log.append(('placement', a, [signed(self.read(a + 4 * i, 4)) for i in range(4)] +
                                  [hex(self.read(a + 16 + 2 * i, 2)) for i in range(4)]))
        if pc == 0x8005D334:  # FindTarget(self): the level's alert fields on entry
            self.ft_ret = getattr(self, 'ft_ret', [])
            self.ft_ret.append(self.r[31])
            e = self.r[4]
            self.rand_log.append(('findtarget', e, [hex(self.read(0x800E46D8 + 4 * i, 4)) for i in range(9)] +
                                  [hex(self.read(e + 28, 4)), hex(self.read(e + 228, 4)), signed(self.read(e + 308, 4)), signed(self.read(e + 268, 4))]))
        elif getattr(self, 'ft_ret', None) and pc == self.ft_ret[-1]:
            self.ft_ret.pop()
            self.rand_log.append(('found', pc, signed(self.r[2])))
        if pc == 0x8002B96C:  # the CREBATCH primitive: who raised it
            self.rand_log.append(('crebatch', self.r[31], [self.r[4], self.r[5], self.r[6]]))
        if pc == 0x8004EE0C:  # PrimaryWeaponMovement: the driver record on entry
            v = self.r[4]
            self.rand_log.append(('vw', self.read(v + 72, 4), [self.read(v + 76, 4), signed(self.read(v + 80, 4)), signed(self.read(v + 216, 2), 16)]))
        if pc in (0x800619E0, 0x80061BA4, 0x8005CDA8):  # monster_start / _go / ai_stand
            self.rand_log.append(({0x800619E0: 'start', 0x80061BA4: 'go', 0x8005CDA8: 'ai_stand'}[pc], self.r[31], self.r[4]))

    # ----------------------------------------------------------------- run
    def run(self, entry, stop=0, limit=2000):
        r, mem = self.r, self.mem
        pc, next_pc = entry, entry + 4
        gte = self.gte
        for _ in range(limit):
            if pc == stop:
                return signed(r[2], 16)
            if self.intercept(pc):
                if self.load:
                    r[self.load[0]] = self.load[1]
                    self.load = None
                pc = r[31]
                next_pc = pc + 4
                continue
            self.count += 1
            self.cur_pc = pc
            word = self.read(pc, 4)
            op = word >> 26
            rs, rt, rd = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
            imm = word & 0xFFFF
            if imm & 0x8000:
                imm -= 0x10000
            a, b = r[rs], r[rt]
            wr = wv = None
            pending = None
            destination = next_pc + 4
            if op == 0:
                fn, sh = word & 63, (word >> 6) & 31
                if fn == 0: wr, wv = rd, b << sh
                elif fn == 2: wr, wv = rd, b >> sh
                elif fn == 3: wr, wv = rd, signed(b) >> sh
                elif fn == 4: wr, wv = rd, b << (a & 31)
                elif fn == 6: wr, wv = rd, b >> (a & 31)
                elif fn == 7: wr, wv = rd, signed(b) >> (a & 31)
                elif fn == 8: destination = a
                elif fn == 9: destination, wr, wv = a, rd, pc + 8
                elif fn == 12 or fn == 13:
                    raise ValueError(f'syscall/break at {pc:08x}')
                elif fn == 16: wr, wv = rd, self.hi
                elif fn == 17: self.hi = a
                elif fn == 18: wr, wv = rd, self.lo
                elif fn == 19: self.lo = a
                elif fn == 24:
                    p = signed(a) * signed(b)
                    self.hi, self.lo = (p >> 32) & 0xFFFFFFFF, p & 0xFFFFFFFF
                elif fn == 25:
                    p = a * b
                    self.hi, self.lo = (p >> 32) & 0xFFFFFFFF, p & 0xFFFFFFFF
                elif fn == 26:
                    sa, sb = signed(a), signed(b)
                    if sb == 0:
                        self.lo = 1 if sa < 0 else 0xFFFFFFFF
                        self.hi = a
                    else:
                        q = abs(sa) // abs(sb)
                        if (sa < 0) != (sb < 0): q = -q
                        self.lo, self.hi = q & 0xFFFFFFFF, (sa - q * sb) & 0xFFFFFFFF
                elif fn == 27:
                    if b == 0:
                        self.lo, self.hi = 0xFFFFFFFF, a
                    else:
                        self.lo, self.hi = a // b, a % b
                elif fn in (32, 33): wr, wv = rd, a + b
                elif fn in (34, 35): wr, wv = rd, a - b
                elif fn == 36: wr, wv = rd, a & b
                elif fn == 37: wr, wv = rd, a | b
                elif fn == 38: wr, wv = rd, a ^ b
                elif fn == 39: wr, wv = rd, ~(a | b)
                elif fn == 42: wr, wv = rd, int(signed(a) < signed(b))
                elif fn == 43: wr, wv = rd, int(a < b)
                else: raise ValueError(f'Unsupported SPECIAL at {pc:08x}: {word:08x}')
            elif op == 1:
                if rt in (0, 16):
                    if signed(a) < 0: destination = pc + 4 + imm * 4
                elif rt in (1, 17):
                    if signed(a) >= 0: destination = pc + 4 + imm * 4
                if rt in (16, 17): wr, wv = 31, pc + 8
            elif op == 2 or op == 3:
                destination = ((pc + 4) & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
                if op == 3: wr, wv = 31, pc + 8
            elif op == 4:
                if a == b: destination = pc + 4 + imm * 4
            elif op == 5:
                if a != b: destination = pc + 4 + imm * 4
            elif op == 6:
                if signed(a) <= 0: destination = pc + 4 + imm * 4
            elif op == 7:
                if signed(a) > 0: destination = pc + 4 + imm * 4
            elif op == 8 or op == 9: wr, wv = rt, a + imm
            elif op == 10: wr, wv = rt, int(signed(a) < imm)
            elif op == 11: wr, wv = rt, int(a < (imm & 0xFFFFFFFF))
            elif op == 12: wr, wv = rt, a & (word & 0xFFFF)
            elif op == 13: wr, wv = rt, a | (word & 0xFFFF)
            elif op == 14: wr, wv = rt, a ^ (word & 0xFFFF)
            elif op == 15: wr, wv = rt, (word & 0xFFFF) << 16
            elif op == 16:  # COP0: mfc0/mtc0/rfe, harmless here
                if rs == 0: pending = (rt, 0)
            elif op == 18:
                if word & (1 << 25):
                    gte.command(word)
                elif rs == 0:
                    pending = (rt, gte.read_data(rd))
                elif rs == 2:
                    pending = (rt, gte.read_ctrl(rd))
                elif rs == 4:
                    gte.write_data(rd, b)
                elif rs == 6:
                    gte.write_ctrl(rd, b)
                else:
                    raise ValueError(f'COP2 form at {pc:08x}: {word:08x}')
            elif op in (32, 33, 35, 36, 37):
                size = 1 if op in (32, 36) else 2 if op in (33, 37) else 4
                pending = (rt, self.read(a + imm, size, op in (32, 33)))
            elif op in (34, 38):
                address = a + imm
                memory = self.read(address & ~3, 4)
                prior = self.load[1] if self.load and self.load[0] == rt else b
                shift = ((3 - (address & 3)) if op == 34 else (address & 3)) * 8
                mask = ((1 << shift) - 1) if op == 34 else (0xFFFFFFFF << (32 - shift)) & 0xFFFFFFFF
                value = (memory << shift) if op == 34 else (memory >> shift)
                pending = (rt, ((prior & mask) | value) & 0xFFFFFFFF)
            elif op == 40: self.write(a + imm, 1, b)
            elif op == 41: self.write(a + imm, 2, b)
            elif op == 43: self.write(a + imm, 4, b)
            elif op == 42:
                address = a + imm
                self.write(address & ~3, (address & 3) + 1, b >> ((3 - (address & 3)) * 8))
            elif op == 46:
                address = a + imm
                self.write(address, 4 - (address & 3), b)
            elif op == 50:  # LWC2
                gte.write_data(rt, self.read(a + imm, 4))
            elif op == 58:  # SWC2
                self.write(a + imm, 4, gte.read_data(rt))
            else:
                raise ValueError(f'Unsupported opcode at {pc:08x}: {word:08x}')
            if self.load:
                r[self.load[0]] = self.load[1]
            if wr:
                r[wr] = wv & 0xFFFFFFFF
            r[0] = 0
            self.load = pending
            pc, next_pc = next_pc, destination
        raise ValueError(f'Native routine exceeded {limit} instructions at {pc:08x}')

    def call(self, pc, *args, limit=200000000):
        self.registers()
        self.r[4:4 + len(args)] = list(args)
        self.run(pc, limit=limit)
        return self.r[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('pal', type=Path)
    ap.add_argument('--frames', type=int, default=20)
    ap.add_argument('--out', type=Path)
    ap.add_argument('--snapshot', type=Path)
    ap.add_argument('--probes', action='store_true', help='log the diagnostic events (OracleMachine.probe)')
    args = ap.parse_args()

    disc = Form1Disc(args.pal)
    exe = disc.read('/SLES_015.34')
    image = exe[2048:2048 + struct.unpack_from('<I', exe, 0x1C)[0]]
    m = OracleMachine(image, disc)
    m.probes = args.probes
    if __import__('os').environ.get('ORACLE_WATCH'):
        lo, hi = (int(x, 16) for x in __import__('os').environ['ORACLE_WATCH'].split(':'))
        m.watch = (lo & 0x1FFFFFFF, hi & 0x1FFFFFFF)
    t0 = time.time()
    snap = args.snapshot
    if snap and snap.exists():
        state = pickle.loads(snap.read_bytes())
        m.mem[:len(state['mem'])] = state['mem']
        m.scratch[:] = state['scratch']
        m.gte.d, m.gte.c, m.gte.flag = state['gte']
        m.rng_state, m.hi, m.lo = state['rng'], state['hi'], state['lo']
        boot_globals(m)                    # a snapshot from before it was set
        print('resumed from', snap, flush=True)
    else:
        load(m, t0)
        if snap:
            snap.parent.mkdir(parents=True, exist_ok=True)
            snap.write_bytes(pickle.dumps({'mem': bytes(m.mem), 'scratch': bytes(m.scratch),
                                           'gte': (m.gte.d, m.gte.c, m.gte.flag),
                                           'rng': m.rng_state, 'hi': m.hi, 'lo': m.lo}))
    frames = []
    try:
        run_frames(m, args.frames, frames, t0)
    except Exception as exc:
        print(f'stopped: {exc} at pc {getattr(m, "cur_pc", 0):08x} ra {m.r[31]:08x} '
              f'frame {len(frames) + 1}', flush=True)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(frames, indent=1))


def boot_globals(m):
    """What the boot's main (0x80018D04) sets before any game state runs.

    0x80018D84..0x80018D98 point two globals at the 324-byte scratch record
    0x800DDDCC. 0x800B2DFC is the one ai_run takes as its tempgoal: it
    clears it with G_InitEdict (0x8005F804) and makes it the goalentity
    while it pursues a last sighting (0x8005E61C..0x8005E640). Left at 0, as
    the loader leaves it, the pursuit handed M_MoveToGoal a NULL goal, so
    SV_NewChaseDir returned at once (0x80060570) where the console steps.
    """
    m.write(0x800B2DFC, 4, 0x800DDDCC)
    m.write(0x800B2DB0, 4, 0x800DDDCC)


def load(m, t0):
    boot_globals(m)
    m.call(0x8006E8C0)                     # heap init
    m.write(RATE, 4, 6)                    # PalNtscRate, PAL
    m.call(GAME_SELF_PLAY)
    print('GameSelfPlay ok', flush=True)
    m.call(GAME_NEW_MISSION)
    print('GameNewMission ok', flush=True)
    m.rng_state = 13
    name = bytes(m.mem[0x0E46C0:0x0E46CC])
    print('level name', name, 'arrival', bytes(m.mem[0x0E4680:0x0E468C]), flush=True)
    # The level resolve (0x8007C54C) waits on hardware; configure_level writes
    # what it leaves behind, as the loader fixtures do. Then the arrival
    # GameSelfPlay chose (0x800E4680) goes where the load reads it.
    row = configure_level(m, 'BASE3', 1, False)
    m.put(0x800C8CD0, bytes(m.mem[0x0E4680:0x0E468C]))
    # The resolve's tail (0x8007C81C..0x8007C828): the row's two load hooks.
    m.write(0x800B2F6C, 4, m.read(row + 24, 4))
    m.write(0x800B2F70, 4, m.read(row + 28, 4))
    # LoadLevel (0x8007CA44) copies the resolved path from 0x800E468C, then
    # runs 0x80071B94, InitLevel and 0x80077454, as TestIt's caller does.
    m.put(0x800E468C, m.string(0x800DDDA8).encode('ascii') + b'\0')
    m.call(AFTER_LOAD, limit=60000000)
    print('LoadLevel ok', time.time() - t0, 's', m.count, 'instructions', flush=True)


def creatures(m):
    """Every creature edict (SVF_MONSTER, +0x40 & 4) from g_edicts[2] on.

    Each entry: edict index, the actor's area byte (+0x9E), its suspend bit
    (+0x10C bit 9), the actor's X and Z (+0x54, +0x5C), the edict's own X and
    Z, ideal yaw (+0x8E) and yaw (+0x14), then the actor's separation radius
    (+0x90), hit radius (+0x94) and height (+0x96), then the edict's tick
    phase pair (+0x2C, +0x2D) and its own clock (+0x34). An edict whose actor
    is not yet linked reads zeros for the actor's fields.
    """
    base = m.read(0x800B2FD0, 4)
    out = []
    for i in range(2, 64):
        e = (base + 324 * i) & 0xFFFFFFFF
        if not m.read(e + 0x40, 4) & 4:
            continue
        a = m.read(e + 0x24, 4)
        actor = (lambda o, n: (signed(m.read(a + o, n), 8 * n) if n > 1 else m.read(a + o, 1))) if a else (lambda o, n: 0)
        out.append([i, actor(158, 1), (m.read(a + 268, 4) >> 9 & 1) if a else 0,
                    actor(84, 4), actor(92, 4),
                    signed(m.read(e, 4)), signed(m.read(e + 8, 4)),
                    signed(m.read(e + 0x8E, 2), 16), signed(m.read(e + 0x14, 2), 16),
                    actor(0x90, 2), actor(0x94, 2), actor(0x96, 2),
                    m.read(e + 0x2C, 1), m.read(e + 0x2D, 1), signed(m.read(e + 0x34, 4))])
    return out


def run_frames(m, count, frames, t0):
    for f in range(count):
        m.rand_log = []
        m.write(NEWV, 4, 12)
        m.write(PAD + 4, 4, 0)
        # MainGameLoop's own frame setup (0x80018384..0x80018398): swap the
        # draw buffers, which resets the packet pointers, then clear this
        # buffer's ordering table.
        m.call(0x80018214)                         # DoubleTrebleBuffer
        m.call(0x800837C0, m.read(0x800B2754, 4) + 10984, 217)   # ClearOTag
        m.call(SWAP_ALL)
        newv = m.read(NEWV, 4)
        # The level clock. The VSync callback (0x80019044) adds the rate to
        # 0x800B29D4 every field, and MainGameLoop copies it into
        # 0x800AEBAC at the top of the frame (0x800183A0..0x800183B0). The
        # interpreter has no vertical blank, so it adds the recorded frame's
        # fields itself; nothing between that copy and SwapAllButtons reads
        # the clock. Frozen at 0, every kick, caption and powerup deadline
        # held still, and the view plane kept a landing kick for ever.
        clock = (m.read(0x800B29D4, 4) + newv) & 0xFFFFFFFF
        m.write(0x800B29D4, 4, clock)
        m.write(0x800AEBAC, 4, clock)
        buttons = m.read(PAD + 12, 4)
        fn0 = m.read(0x800E46DC, 4)
        m.call(QUAKE_LOOP)
        ent = (m.read(0x800B2FD0, 4) + 324) & 0xFFFFFFFF   # g_edicts[1], QuakeThinkGeneral
        link = m.read(ent + 0x24, 4)
        player = [signed(m.read(link + 0x54 + 4 * i, 4)) for i in range(3)] + [signed(m.read(link + 0x108, 2), 16)]
        frames.append({'frame': f + 1, 'buttons': buttons, 'newv': newv,
                       'framenum': [fn0, m.read(0x800E46DC, 4)], 'player': player,
                       'mask': [m.read(0x800DD944 + 4 * i, 4) for i in range(3)],
                       'view_area': m.read(WINDOWS + 0x9E, 2),
                       'closed': [n for n in range(400) if m.read(m.read(0x800B2C3C, 4) + 52 * n + 8, 4) & 0x8000],
                       'cre': creatures(m),
                       'camera': [signed(m.read(WINDOWS + 4 * i, 4)) for i in range(3)] +
                                 [signed(m.read(WINDOWS + 12 + 2 * i, 2), 16) for i in range(3)],
                       # Areas player one's view drew this frame: 0x800689F4's stamp
                       # (0x800B2DE4); the next frame's creatures test them (0x800689C0).
                       'seen': [a for a in range(96) if m.read(0x800D8D78 + 64 * a, 2) == m.read(0x800B2DE4, 2)],
                       'boxes': [[i] + [signed(m.read(0x800CAE10 + 64 * i + 4 * j, 4)) for j in range(6)] + [m.read(0x800CAE10 + 64 * i + 55, 1)]
                                 for i in range(48) if m.read(0x800CAE10 + 64 * i + 54, 1)],
                       'rand': [(k, ra, v) for k, ra, v in m.rand_log]})
        print(f'frame {f + 1} btn {buttons:04x} newv {newv} rand {len(m.rand_log)} framenum {fn0} player {player} '
              f'({m.count} instr, {time.time() - t0:.0f} s)', flush=True)


if __name__ == '__main__':
    main()
