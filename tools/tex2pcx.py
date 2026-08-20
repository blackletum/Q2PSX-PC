"""Cut the disc's atlas pages into individual PC-style textures, as Quake II PCX.

A texture on this disc is not a file. It is a rectangle of a 256x256 4bpp page,
sampled through one 16-entry CLUT, and the page holds dozens of unrelated
textures side by side. Exporting the page is therefore not exporting a texture:
resolve a whole page through one palette and only the parts that palette
belongs to are legible, which is why a page dump looks tinted and wrong.

So this works the way a PC texture set is laid out -- one texture, one file:

    <out>/textures/<map>/<name>.pcx   world surfaces
    <out>/models/<map>/<name>.pcx     model skin patches
    <out>/pics/<name>.pcx             standalone HUD, menu and screen art

WHERE THE RECTANGLES COME FROM. The geometry names them. `q2psx-inspect export`
writes each face's UVs into its OBJ as (u + 0.5) / 256, so the integer texel
corners come back exactly, and a face's four corners bound the rectangle it
samples. Collect those per (page, CLUT) and the page's layout falls out of its
own usage -- no grid is assumed, because the pages are not on one.

OVERLAPPING RECTANGLES ARE ONE TEXTURE. Different faces take different parts of
the same texture -- a full 64x64 here, the top 64x31 of it there -- so any two
rectangles that overlap are merged into their union and the merge is run to a
fixed point. What survives is the region the level actually samples: 43 of
BASE1's 59 world textures come out 64x64, which is what a Quake II texture is.

THE PALETTE IS QUAKE II'S, RETRIEVED, NOT INVENTED. `--palette` takes a pak
(any `pics/colormap.pcx` in it), a .pcx, or a raw 768-byte .lmp, read from the
user's own Quake II files at run time and never checked in. Each source colour
is matched to the nearest id colour by sum of squares over indices 0..254, the
way id's own tools do it, with no dithering. 255 is left out of the search
because Quake II reads it as transparent -- which is where the PlayStation's
transparent texel goes, so the one thing PCX cannot normally carry survives.

CLUT 0 IS SKIPPED. Palette index 0 is one of the sixteen reserved all-0x8000
CLUTs, so every texel it resolves is black; the nodes that bind it are sealing
geometry the console never draws (scene.h). Emitting them would be thousands of
black files.

Names are provenance, not archaeology: <page><clut>_<u>_<v>. Matching the crops
against the 2,118 .wal textures in Quake II's own pak0 was tried and dropped --
best and second-best score the same, because this art was redrawn for the
console rather than downscaled from the PC set.
"""
import argparse
import collections
import glob
import hashlib
import os
import struct
import sys
import zlib


# ---------------------------------------------------------------------------
# The Quake II palette, out of the user's own install
# ---------------------------------------------------------------------------
def palette_from_pcx(d):
    """The 768 bytes after the 0x0C marker at the tail of an 8bpp PCX."""
    if len(d) < 769 or d[0] != 0x0A or d[3] != 8:
        raise ValueError('not an 8bpp PCX')
    if d[-769] != 0x0C:
        raise ValueError('no palette marker at the tail')
    return bytearray(d[-768:])


def palette_from_pak(path):
    with open(path, 'rb') as f:
        magic, ofs, ln = struct.unpack('<4sii', f.read(12))
        if magic != b'PACK':
            raise ValueError('%s: not a Quake II pak' % path)
        f.seek(ofs)
        d = f.read(ln)
        for i in range(ln // 64):
            e = d[i * 64:(i + 1) * 64]
            name = e[:56].split(b'\0')[0].decode('latin-1').lower()
            if name.replace('\\', '/') == 'pics/colormap.pcx':
                pos, sz = struct.unpack('<ii', e[56:64])
                f.seek(pos)
                return palette_from_pcx(f.read(sz))
    raise ValueError('%s: no pics/colormap.pcx inside' % path)


def load_palette(path):
    if os.path.splitext(path)[1].lower() == '.pak':
        return palette_from_pak(path)
    d = open(path, 'rb').read()
    return bytearray(d) if len(d) == 768 else palette_from_pcx(d)


PAK_GUESSES = [
    'C:/Program Files (x86)/Steam/steamapps/common/Quake 2/baseq2/pak0.pak',
    'C:/Program Files (x86)/Steam/steamapps/common/Quake II/baseq2/pak0.pak',
    'C:/quake2/baseq2/pak0.pak',
    os.path.expanduser('~/.local/share/Steam/steamapps/common/Quake 2/'
                       'baseq2/pak0.pak'),
    '/usr/share/games/quake2/baseq2/pak0.pak',
]


def find_palette():
    for p in PAK_GUESSES:
        if os.path.isfile(p):
            return p
    return None


class Matcher(object):
    """id's BestColor: nearest by sum of squares over 0..254, because 255 is
    the transparent index and must never be chosen for a colour."""

    def __init__(self, pal):
        self.tri = [(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2])
                    for i in range(255)]
        self.cache = {}
        self.err_sum = 0.0
        self.err_n = 0
        self.err_max = 0

    def best(self, rgb):
        hit = self.cache.get(rgb)
        if hit is None:
            r, g, b = rgb
            bi, bd = 0, 1 << 30
            for i, (pr, pg, pb) in enumerate(self.tri):
                dr, dg, db = r - pr, g - pg, b - pb
                d = dr * dr + dg * dg + db * db
                if d < bd:
                    bi, bd = i, d
                    if not d:
                        break
            hit = self.cache[rgb] = (bi, bd)
        self.err_sum += hit[1] ** 0.5
        self.err_n += 1
        if hit[1] > self.err_max:
            self.err_max = hit[1]
        return hit[0]


# ---------------------------------------------------------------------------
# PNG in
# ---------------------------------------------------------------------------
PNG_SIG = b'\x89PNG\r\n\x1a\n'


def read_png8(path):
    """-> (w, h, indices, palette bytes, alpha list or None)."""
    d = open(path, 'rb').read()
    if d[:8] != PNG_SIG:
        raise ValueError('%s: not a PNG' % path)
    idat = bytearray()
    pal = bytearray(768)
    alpha = None
    w = h = depth = ctype = None
    o = 8
    while o + 8 <= len(d):
        ln, typ = struct.unpack('>I4s', d[o:o + 8])
        body = d[o + 8:o + 8 + ln]
        if typ == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
        elif typ == b'PLTE':
            pal[:ln] = body
        elif typ == b'tRNS':
            alpha = list(body)
        elif typ == b'IDAT':
            idat += body
        elif typ == b'IEND':
            break
        o += 12 + ln
    if (depth, ctype) != (8, 3):
        raise ValueError('%s: want 8-bit indexed, got depth %s colour type %s'
                         % (path, depth, ctype))
    return w, h, unfilter(zlib.decompress(bytes(idat)), w, h), pal, alpha


def unfilter(raw, w, h):
    """Undo the PNG row filters. One byte per pixel, so `a` is the byte left."""
    out = bytearray(w * h)
    prev = bytearray(w)
    o = 0
    for y in range(h):
        ft = raw[o]
        row = bytearray(raw[o + 1:o + 1 + w])
        o += 1 + w
        if ft == 1:
            for x in range(1, w):
                row[x] = (row[x] + row[x - 1]) & 0xFF
        elif ft == 2:
            for x in range(w):
                row[x] = (row[x] + prev[x]) & 0xFF
        elif ft == 3:
            for x in range(w):
                a = row[x - 1] if x else 0
                row[x] = (row[x] + ((a + prev[x]) >> 1)) & 0xFF
        elif ft == 4:
            for x in range(w):
                a = row[x - 1] if x else 0
                c = prev[x - 1] if x else 0
                b = prev[x]
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                row[x] = (row[x] + pr) & 0xFF
        elif ft != 0:
            raise ValueError('bad filter %d on row %d' % (ft, y))
        out[y * w:(y + 1) * w] = row
        prev = row
    return out


# ---------------------------------------------------------------------------
# PCX out
# ---------------------------------------------------------------------------
def rle(row):
    """PCX run encoding. A lone byte >= 0xC0 still needs the count prefix, or a
    decoder reads the pixel as one."""
    out = bytearray()
    i, n = 0, len(row)
    while i < n:
        b = row[i]
        run = 1
        while i + run < n and row[i + run] == b and run < 63:
            run += 1
        if run > 1 or b >= 0xC0:
            out.append(0xC0 | run)
        out.append(b)
        i += run
    return out


def write_pcx8(path, w, h, px, pal):
    hdr = bytearray(128)
    hdr[0] = 0x0A                              # ZSoft
    hdr[1] = 5                                 # v3.0: palette at the tail
    hdr[2] = 1                                 # RLE
    hdr[3] = 8                                 # bits per pixel per plane
    struct.pack_into('<HHHH', hdr, 4, 0, 0, w - 1, h - 1)
    struct.pack_into('<HH', hdr, 12, 72, 72)
    # hdr[16:64] is the EGA colormap: unused at 8bpp, and left zero so that no
    # decoder can prefer it over the real palette at the end of the file.
    hdr[65] = 1                                # colour planes
    struct.pack_into('<H', hdr, 66, w + (w & 1))   # bytes per line, even
    struct.pack_into('<H', hdr, 68, 1)         # palette type: colour
    struct.pack_into('<HH', hdr, 70, w, h)

    body = bytearray()
    for y in range(h):
        row = px[y * w:(y + 1) * w]
        if w & 1:                              # pad odd rows to an even count
            row = row + row[-1:]
        body += rle(row)

    with open(path, 'wb') as fp:
        fp.write(hdr)
        fp.write(body)
        fp.write(b'\x0C')
        fp.write(pal)


# ---------------------------------------------------------------------------
# The atlas, read back out of the geometry
# ---------------------------------------------------------------------------
def obj_rects(path):
    """Yield (material, u0, v0, u1, v1) for every textured face in an OBJ.

    The exporter writes `vt (u + 0.5) / 256` with v flipped, so the integer
    texel each corner names comes back exactly. Four corners bound the
    rectangle the face samples."""
    mat = None
    cur = []
    for line in open(path, 'r', errors='replace'):
        if line.startswith('vt '):
            _, a, b = line.split()
            cur.append((int(round(float(a) * 256.0 - 0.5)),
                        int(round((1.0 - float(b)) * 256.0 - 0.5))))
            if len(cur) > 4:
                del cur[:-4]
        elif line.startswith('f '):
            if '/' in line and len(cur) == 4:
                us = [p[0] for p in cur]
                vs = [p[1] for p in cur]
                yield mat, min(us), min(vs), max(us), max(vs)
            cur = []
        elif line.startswith('usemtl '):
            mat = line.split()[1]


def merge_rects(rects):
    """Any two rectangles that overlap are one texture: union them, to a fixed
    point. Touching edges do not merge -- only real overlap does."""
    rs = list(rects)
    changed = True
    while changed:
        changed = False
        out = []
        for r in rs:
            for i, k in enumerate(out):
                if (min(k[2], r[2]) >= max(k[0], r[0]) and
                        min(k[3], r[3]) >= max(k[1], r[1])):
                    out[i] = (min(k[0], r[0]), min(k[1], r[1]),
                              max(k[2], r[2]), max(k[3], r[3]))
                    changed = True
                    break
            else:
                out.append(r)
        rs = out
    return sorted(rs)


def crop(px, pw, r):
    u0, v0, u1, v1 = r
    w, h = u1 - u0 + 1, v1 - v0 + 1
    out = bytearray(w * h)
    for y in range(h):
        s = (v0 + y) * pw + u0
        out[y * w:(y + 1) * w] = px[s:s + w]
    return w, h, out


def remap(px, pal, alpha, m):
    """Source indices -> Quake II indices, through a 256-byte table."""
    lut = bytearray(256)
    for i in set(px):
        if alpha and i < len(alpha) and alpha[i] != 255:
            lut[i] = 255                       # the console's transparent texel
        else:
            lut[i] = m.best((pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]))
    return px.translate(bytes(lut))


def pic_name(stem):
    """img_09_frontend.lbm_c0 -> frontend_c0, the way a PC pic is named."""
    s = stem
    if s.startswith('img_'):
        s = s[4:]
        if '_' in s and s.split('_', 1)[0].isdigit():
            s = s.split('_', 1)[1]
    return s.replace('.lbm', '').replace('.', '_').lower()


README = """\
Quake II PSX textures, cut into PC-style files
==============================================

Produced by tools/tex2pcx.py from the `q2psx-inspect export` tree. One texture
per file, laid out the way a PC Quake II texture set is:

  textures/<map>/<name>.pcx   world surfaces      %(world)5d files
  models/<map>/<name>.pcx     model skin patches  %(model)5d files
  pics/<name>.pcx             HUD, menu, screens  %(pic)5d files

Every map on the disc carries its own copy of the HUD and menu art, so pics are
pooled into one directory and deduplicated by content, the way baseq2 holds
them once. Textures and model patches stay under the map they belong to.

textures.csv lists every file with the page, CLUT, rectangle and face count it
came from.

Why the pages had to be cut up
------------------------------
A texture on this disc is a RECTANGLE OF A PAGE, not a file. The console samples
a 256x256 4bpp page through one 16-entry CLUT, and one page holds dozens of
unrelated textures packed side by side. Resolving a whole page through a single
palette leaves only the parts that palette belongs to legible and tints
everything else, which is why a page dump looks wrong no matter how good the
palette conversion is.

The geometry names the rectangles. Every face's UVs are in the exported OBJ as
(u + 0.5) / 256, so the integer texel corners come back exactly, and the four
corners of a face bound the region it samples. Rectangles that overlap are the
same texture seen by different faces -- a full 64x64 here, its top 64x31 there
-- so they are merged into their union until nothing more merges. The page's
layout comes out of its own usage; no grid is assumed, because these pages are
not on one.

%(sizes)s

Palette
-------
EVERY FILE IS IN QUAKE II'S PALETTE -- the 256-colour table id ships as
pics/colormap.pcx, read out of
  %(source)s
and written into each file's 768-byte tail block. The console's own palettes are
16-entry CLUTs and no Quake II tool can read one.

Each source colour is matched to the nearest Quake II colour by sum of squares
in RGB across indices 0..254, which is what id's own tools do, and the index
bytes run through that table. No dithering, for the same reason. Mean error of
a matched colour is %(mean).1f and the worst is %(worst).1f, on the 0..441 scale
of a distance in 8-bit RGB -- small because this art already sits inside id's
gamut, where an arbitrary 15-bit colour would average 52.

Index 255 is left out of the search because Quake II reads it as transparent.
That is where the PlayStation's transparent texel goes: a CLUT entry of zero is
the hardware's see-through rather than black, so the transparency PCX normally
cannot express survives by the engine's own convention.

Not here
--------
CLUT index 0 is skipped. It is one of the sixteen reserved all-0x8000 palettes,
so every texel through it is black, and the nodes that bind it are sealing
geometry the console never draws. %(void)d page/CLUT pairs were dropped for it.

Names are provenance -- <page><clut>_<u>_<v> -- not the original names. Matching
the crops against the 2,118 .wal textures in Quake II's own pak0 was tried and
dropped: best and second-best score the same, because this art was redrawn for
the console rather than downscaled from the PC set.

Format
------
8 bits per pixel, one plane, version 5, RLE, palette after a 0x0C marker at the
end -- what Quake II's LoadPCX accepts.
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--src', default='.tmp')
    ap.add_argument('--out', default='.tmp/texture-convert')
    ap.add_argument('--palette', default=None, metavar='PATH',
                    help='pak0.pak, colormap.pcx or a 768-byte .lmp')
    args = ap.parse_args()

    pal_path = args.palette or find_palette()
    if not pal_path:
        sys.exit('no Quake II palette found -- pass --palette pointing at '
                 'baseq2/pak0.pak, pics/colormap.pcx or a 768-byte .lmp')
    q2pal = load_palette(pal_path)
    print('palette: %s' % pal_path)

    src = os.path.abspath(args.src)
    out = os.path.abspath(args.out)
    maps = sorted(d for d in os.listdir(src)
                  if os.path.isdir(os.path.join(src, d, 'textures')))
    if not maps:
        sys.exit('%s holds no <MAP>/textures -- run `q2psx-inspect export` '
                 'first' % src)

    m = Matcher(q2pal)
    counts = collections.Counter()
    sizes = collections.Counter()
    void = set()
    dupes = [0]
    rows = []
    pic_seen = {}
    pic_names = {}

    for mi, mp in enumerate(maps):
        tdir = os.path.join(src, mp, 'textures')
        low = mp.lower()

        wanted = {'textures': collections.defaultdict(collections.Counter),
                  'models': collections.defaultdict(collections.Counter)}
        for kind, pats in (('textures', ['*_ZONE*.obj']),
                           ('models', ['models/*.obj'])):
            for pat in pats:
                for f in glob.glob(os.path.join(src, mp, pat)):
                    for mat, u0, v0, u1, v1 in obj_rects(f):
                        if mat is None:
                            continue
                        if mat.endswith('_c000'):
                            void.add((mp, mat))
                            continue
                        wanted[kind][mat][(u0, v0, u1, v1)] += 1

        pages = {}
        seen = {}

        def emit(kind, name, w, h, px, key, flat=False):
            """flat: one pooled directory, the way baseq2 holds its pics."""
            d = os.path.join(out, kind) if flat else os.path.join(out, kind, low)
            os.makedirs(d, exist_ok=True)
            write_pcx8(os.path.join(d, name + '.pcx'), w, h, px, q2pal)
            counts[kind] += 1
            sizes[(w, h)] += 1
            rows.append(key)

        for kind in ('textures', 'models'):
            for mat in sorted(wanted[kind]):
                hits = wanted[kind][mat]
                if mat not in pages:
                    p = os.path.join(tdir, mat + '.png')
                    if not os.path.isfile(p):
                        pages[mat] = None
                    else:
                        pw, ph, ppx, ppal, palpha = read_png8(p)
                        pages[mat] = (pw, ph, ppx, ppal, palpha)
                if pages[mat] is None:
                    continue
                pw, ph, ppx, ppal, palpha = pages[mat]
                for r in merge_rects(hits.keys()):
                    faces = sum(c for rr, c in hits.items()
                                if min(rr[2], r[2]) >= max(rr[0], r[0]) and
                                min(rr[3], r[3]) >= max(rr[1], r[1]))
                    w, h, sub = crop(ppx, pw, r)
                    sub = remap(sub, ppal, palpha, m)
                    dig = hashlib.sha1(bytes([w & 255, h & 255]) + bytes(sub))
                    dig = dig.hexdigest()
                    if dig in seen:
                        dupes[0] += 1
                        continue
                    name = '%s_%03d_%03d' % (mat.replace('_', ''), r[0], r[1])
                    seen[dig] = name
                    emit(kind, name, w, h, sub,
                         (low, kind, name, mat, r[0], r[1], w, h, faces, dig))

        for p in sorted(glob.glob(os.path.join(tdir, 'img_*.png'))):
            stem = os.path.basename(p)[:-4]
            w, h, px, pal, alpha = read_png8(p)
            px = remap(px, pal, alpha, m)
            dig = hashlib.sha1(bytes([w & 255, h & 255]) + bytes(px)).hexdigest()
            if dig in pic_seen:
                dupes[0] += 1
                continue
            # Maps do not agree on what `screena` or `chars_c0` looks like, so
            # a name that is already taken by different art gets the map that
            # brought this one, then a number if even that is not enough.
            name = base = pic_name(stem)
            if name in pic_names and pic_names[name] != dig:
                name = base = '%s_%s' % (base, low)
            k = 2
            while name in pic_names and pic_names[name] != dig:
                name = '%s_%d' % (base, k)
                k += 1
            pic_names[name] = dig
            pic_seen[dig] = name
            emit('pics', name, w, h, px,
                 (low, 'pics', name, stem, 0, 0, w, h, 0, dig), flat=True)

        print('  [%2d/%d] %-9s world %4d  models %4d  pics %3d'
              % (mi + 1, len(maps), mp, counts['textures'], counts['models'],
                 counts['pics']), flush=True)

    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, 'textures.csv'), 'w') as fp:
        fp.write('map,kind,name,source,u,v,width,height,faces,sha1\n')
        for r in rows:
            fp.write(','.join(str(x) for x in r) + '\n')

    top = sizes.most_common(12)
    wide = max(len('%dx%d' % k) for k, _ in top)
    tbl = ['Sizes that came out, most common first:', '']
    tbl += ['  %-*s  %5d' % (wide, '%dx%d' % k, v) for k, v in top]
    mean = m.err_sum / m.err_n if m.err_n else 0.0
    with open(os.path.join(out, 'README.txt'), 'w') as fp:
        fp.write(README % {'world': counts['textures'],
                           'model': counts['models'], 'pic': counts['pics'],
                           'source': pal_path, 'mean': mean,
                           'worst': m.err_max ** 0.5, 'void': len(void),
                           'sizes': '\n'.join(tbl)})

    print('\n%d textures -> %s' % (sum(counts.values()), out))
    print('  world %d, model %d, pics %d'
          % (counts['textures'], counts['models'], counts['pics']))
    print('  %d identical crops collapsed, %d CLUT-0 pairs skipped'
          % (dupes[0], len(void)))
    print('  colour match: mean %.1f, worst %.1f' % (mean, m.err_max ** 0.5))
    print('  sizes: ' + ', '.join('%dx%d x%d' % (k[0], k[1], v)
                                  for k, v in top[:6]))


if __name__ == '__main__':
    main()
