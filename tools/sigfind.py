"""Find CC patch sites in another Genie exe by masked byte signatures.
Signature = bytes [site-before, site+after) from CC, with rel32 operands of
E8/E9 calls/jumps and absolute addresses (0x4xxxxx-0x7xxxxx) wildcarded."""
import struct, sys

def load(path):
    d = open(path, 'rb').read()
    pe = struct.unpack('<I', d[0x3c:0x40])[0]
    nsec = struct.unpack('<H', d[pe+6:pe+8])[0]
    opt = struct.unpack('<H', d[pe+20:pe+22])[0]
    secs = []
    for i in range(nsec):
        o = pe+24+opt+i*40
        vs, va, rs, ra = struct.unpack('<IIII', d[o+8:o+24])
        secs.append((va, max(vs, rs), ra))
    return d, secs

def off(secs, va):
    rva = va - 0x400000
    for v, s, r in secs:
        if v <= rva < v+s: return r + rva - v
def va_of(secs, o):
    for v, s, r in secs:
        if r <= o < r+s: return 0x400000 + v + o - r

def mask(b):
    m = [True]*len(b)
    for i in range(len(b)-3):
        v = struct.unpack('<I', b[i:i+4])[0]
        if 0x401000 <= v < 0x800000:
            for k in range(4): m[i+k] = False
    for i in range(len(b)-4):
        if b[i] in (0xE8, 0xE9):
            for k in range(1, 5): m[i+k] = False
    return m

def find(cc, other, site, before=16, after=24):
    d1, s1 = cc; d2, s2 = other
    o = off(s1, site)
    sig = d1[o-before:o+after]
    m = mask(sig)
    # text section of other
    import re
    pat = b''.join(re.escape(bytes([sig[j]])) if m[j] else b'.' for j in range(len(sig)))
    rx = re.compile(pat, re.S)
    hits = []
    for v, s, r in s2[:2]:
        for mo in rx.finditer(d2, r, r+s):
            hits.append(va_of(s2, mo.start()+before))
            if len(hits) > 3: return hits
    return hits

if __name__ == '__main__':
    cc = load(sys.argv[1]); other = load(sys.argv[2])
    for a in sys.argv[3:]:
        site = int(a, 16)
        for b, af in [(16, 24), (10, 16), (24, 32), (6, 12)]:
            h = find(cc, other, site, b, af)
            if len(h) == 1: break
        print('%08x -> %s' % (site, ' '.join('%08x' % x for x in h) if h else 'NOT FOUND'))
