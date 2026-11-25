"""Image comparison for the renderer test matrix (no third-party modules).

cmp.py ref.bmp test.bmp [--scale N]   ->  prints mean abs diff and % of differing pixels
With --scale N, ref (game-resolution frame dump) is nearest-upscaled by N and
compared against the centred region of the test screenshot.
"""
import struct, sys


def read_bmp(path):
    d = open(path, 'rb').read()
    off = struct.unpack('<I', d[10:14])[0]
    w, h = struct.unpack('<ii', d[18:26])
    bpp = struct.unpack('<H', d[28:30])[0]
    bottom_up = h > 0
    h = abs(h)
    bpr = (w * bpp // 8 + 3) & ~3
    rows = []
    for y in range(h):
        ry = h - 1 - y if bottom_up else y
        rows.append(d[off + ry * bpr: off + ry * bpr + w * bpp // 8])
    return w, h, bpp // 8, rows


def main():
    ref, test = sys.argv[1], sys.argv[2]
    scale = int(sys.argv[sys.argv.index('--scale') + 1]) if '--scale' in sys.argv else 0
    try:
        rw, rh, rb, rrows = read_bmp(ref)
        tw, th, tb, trows = read_bmp(test)
    except Exception as e:
        print('unreadable (%s)' % e); return
    if scale and (rw * scale > tw or rh * scale > th or len(rrows[-1]) < rw * rb):
        print('size mismatch (ref %dx%d)' % (rw, rh)); return
    if scale:
        ox, oy = (tw - rw * scale) // 2, (th - rh * scale) // 2
        w, h = rw * scale, rh * scale
    else:
        ox = oy = 0
        w, h = min(rw, tw), min(rh, th)
    total = 0
    bad = 0
    n = 0
    step = 2  # sample every 2nd pixel in each direction (speed)
    for y in range(0, h, step):
        ry = y // scale if scale else y
        rrow, trow = rrows[ry], trows[oy + y]
        for x in range(0, w, step):
            rx = x // scale if scale else x
            r = rrow[rx * rb: rx * rb + 3]
            t = trow[(ox + x) * tb: (ox + x) * tb + 3]
            dsum = abs(r[0] - t[0]) + abs(r[1] - t[1]) + abs(r[2] - t[2])
            total += dsum
            bad += dsum > 30
            n += 1
    print('mean_abs=%.2f bad_pct=%.3f' % (total / (3 * n), 100.0 * bad / n))


main()
