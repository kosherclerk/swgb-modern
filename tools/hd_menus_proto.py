"""Prototype of the launcher's HD menu generator (the launcher has the C# port).
Usage: hd_menus_proto.py <Game dir> <work dir>  -> writes <work>/src/<crc>.png"""
import os, struct, sys, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def drs(path):
    f = open(path, 'rb').read()
    ntab = struct.unpack('<i', f[0x4c:0x50])[0]
    out = {}
    for t in range(ntab):
        ext, off, num = struct.unpack('<4sii', f[0x54 + t * 12:0x60 + t * 12])
        ext = ext[::-1].decode().strip()
        for i in range(num):
            rid, o, s = struct.unpack('<iii', f[off + i * 12:off + i * 12 + 12])
            out[(ext, rid)] = f[o:o + s]
    return out


def slp_frame0(d):
    cmdo, outo, palo, props, w, h, hx, hy = struct.unpack('<IIIIiiii', d[32:64])
    img = [[-1] * w for _ in range(h)]
    for y in range(h):
        l, r = struct.unpack('<hh', d[outo + y * 4:outo + y * 4 + 4])
        if l == -32768 or r == -32768:
            continue
        p = struct.unpack('<I', d[cmdo + y * 4:cmdo + y * 4 + 4])[0]
        x = l
        while True:
            c = d[p]; p += 1
            lo2, lo4 = c & 3, c & 0xF
            if c == 0x0F: break
            if lo2 == 0:
                n = c >> 2; img[y][x:x + n] = list(d[p:p + n]); p += n; x += n
            elif lo2 == 1:
                x += c >> 2
            elif lo4 == 2:
                n = ((c & 0xF0) << 4) + d[p]; p += 1; img[y][x:x + n] = list(d[p:p + n]); p += n; x += n
            elif lo4 == 3:
                x += ((c & 0xF0) << 4) + d[p]; p += 1
            elif lo4 in (6, 7, 0xA, 0xB):
                n = c >> 4
                if n == 0: n = d[p]; p += 1
                if lo4 == 6: img[y][x:x + n] = [v + 16 for v in d[p:p + n]]; p += n
                elif lo4 == 7: img[y][x:x + n] = [d[p]] * n; p += 1
                elif lo4 == 0xA: img[y][x:x + n] = [d[p] + 16] * n; p += 1
                x += n
            elif lo4 == 0xE:
                if c in (0x4E, 0x6E): x += 1
                elif c in (0x5E, 0x7E): x += d[p]; p += 1
            else:
                raise ValueError('cmd %x' % c)
    return w, h, img


def palette(txt):
    L = txt.decode('latin1').replace('\r', '').split('\n')
    n = int(L[2])
    return [tuple(map(int, L[3 + i].split()[:3])) for i in range(n)]


def png_rgba(path, w, h, rows):
    raw = b''.join(b'\0' + bytes(r) for r in rows)
    def ch(t, b):
        c = struct.pack('>I', len(b)) + t + b
        return c + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))


def main():
    game, work = sys.argv[1], sys.argv[2]
    os.makedirs(os.path.join(work, 'src'), exist_ok=True)
    archives = [drs(os.path.join(game, 'Data', n)) for n in ('INTERFAC.DRS', 'interfac_x1.drs') if os.path.exists(os.path.join(game, 'Data', n))]
    allres = {}
    for a in archives: allres.update({k: v for k, v in a.items()})
    done = set()
    for a in archives:
        for (ext, rid), data in a.items():
            if ext != 'bina':
                continue
            txt = data.decode('latin1', 'replace')
            if 'background1_files' not in txt:
                continue
            fields = dict((ln.split()[0], ln.split()[1:]) for ln in txt.replace('\r', '').split('\n') if ln.split())
            bg = fields.get('background1_files', [])
            pal = fields.get('palette_file', [])
            if len(bg) < 3 or bg[2] == '-1' or len(pal) < 2:
                continue
            slp_id, pal_id = int(bg[2]), int(pal[1])
            for arch in archives:
                sd = arch.get(('slp', slp_id))
                pd = allres.get(('bina', pal_id))
                if not sd or not pd:
                    continue
                crc = zlib.crc32(sd) & 0xffffffff
                if crc in done:
                    continue
                w, h, img = slp_frame0(sd)
                if w < 320:
                    continue
                P = palette(pd)
                rows = []
                for r in img:
                    b = bytearray()
                    for v in r:
                        b += bytes(P[v]) + b'\xff' if v >= 0 else b'\0\0\0\0'
                    rows.append(b)
                png_rgba(os.path.join(work, 'src', '%08x.png' % crc), w, h, rows)
                done.add(crc)
                print('%08x slp %d pal %d %dx%d' % (crc, slp_id, pal_id, w, h))


main()
