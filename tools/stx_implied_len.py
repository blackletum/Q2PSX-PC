"""What the frame headers alone say the AC codes must average.

For a 320x192 frame the bitstream is 1440 blocks, each a 10-bit DC and a 2-bit
EOB plus its (run, level) pairs, and every pair costs its code length plus one
sign bit. So

    bits_used  =  1440 * 12  +  sum over pairs of (code_len + 1)

and if `bs_num_codes` is `round_up_32(1440 + pairs)` then the pair count is
known to within the rounding. Dividing gives the mean code length the disc
requires — computed from two header fields, with no decoder involved at all.
"""
import struct, sys

SECTOR = 2048
PAY = 2016


def frames(path):
    d = open(path, 'rb').read()
    n = len(d) // SECTOR
    i = 0
    while i < n:
        s = d[i * SECTOR:(i + 1) * SECTOR]
        if i % 8 == 7 or len(s) < 32:
            i += 1
            continue
        magic, sub, ci, cc = struct.unpack('<HHHH', s[0:8])
        num, size = struct.unpack('<II', s[8:16])
        w, h = struct.unpack('<HH', s[16:20])
        nc, bsm, qs, ver = struct.unpack('<HHHH', s[20:28])
        if magic != 0x0160 or sub != 0x8001 or ci != 0:
            i += 1
            continue
        yield num, size, nc, qs, cc
        i += 1


for path in sys.argv[1:]:
    tot = 0
    rows = []
    for num, size, nc, qs, cc in frames(path):
        pairs = nc - 1440
        if pairs <= 0:
            continue
        bits = (size - 8) * 8
        ac_bits = bits - 1440 * 12
        if ac_bits <= 0:
            continue
        rows.append((ac_bits / pairs, pairs, qs))
        tot += 1

    if not rows:
        print(path, 'no AC frames')
        continue

    rows.sort()
    mean = sum(r[0] for r in rows) / len(rows)
    print('%-14s %5d AC frames   mean (len+1) = %.2f   median %.2f   '
          'range %.2f..%.2f'
          % (path.split('/')[-1], tot, mean, rows[len(rows) // 2][0],
             rows[0][0], rows[-1][0]))
