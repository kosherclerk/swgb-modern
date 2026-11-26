"""terrain_mosaic.py <slp id> <order rc|cr> <out.bmp> [axis down|up] [repeat]
Assembles an n x n terrain SLP (97x49 isometric tiles) into one isometric mosaic
with the in-game palette 50500. SWGB layout: order cr, axis up (tile (i,j) uses
frame j*n+i; map axis i runs up-right). repeat=3 gives wrap-around context for
upscaling the texture as one seamless image."""
import os, struct, sys, math
sys.path.insert(0, r'C:\Users\user\Desktop\claude\swgb\ModSource\tools')
src = open(r'C:\Users\user\Desktop\claude\swgb\ModSource\tools\hd_menus_proto.py').read().replace('\nmain()\n', '\n')
ns = {'__file__': 'x'}
exec(compile(src, 'proto', 'exec'), ns)
drs, slp_frame0, palette = ns['drs'], ns['slp_frame0'], ns['palette']
G = r'C:\Users\user\Desktop\claude\swgb\Game\Data'
rid, order, out = int(sys.argv[1]), sys.argv[2], sys.argv[3]
axis = sys.argv[4] if len(sys.argv) > 4 else 'down'
rep = int(sys.argv[5]) if len(sys.argv) > 5 else 1
res = {}
for n in ('TERRAIN.DRS', 'terrain_x1.drs', 'INTERFAC.DRS'):
    res.update(drs(os.path.join(G, n)))
d = res[('slp', rid)]
pal = palette(res[('bina', 50500)])
nf = struct.unpack_from('<i', d, 4)[0]
n = int(round(math.sqrt(nf)))

def frame(k):
    # decode frame k by pointing the frame-0 decoder at frame k's header
    hdr = d[32 + k * 32: 64 + k * 32]
    dd = d[:32] + hdr + d[64:]
    return slp_frame0(dd)

N = n * rep
W, H = 48 * 2 * N + 2, 24 * 2 * N + 2
img = [[(255, 0, 255)] * W for _ in range(H)]
ox = 48 * N
for i in range(N):
    for j in range(N):
        k = (i % n) * n + (j % n) if order == 'rc' else (j % n) * n + (i % n)
        w, h, px = frame(k)
        if axis == 'down': sx, sy = ox + (j - i) * 48 - 48, (i + j) * 24
        else: sx, sy = (i + j) * 48, H // 2 - 24 + (j - i) * 24
        for y in range(h):
            for x in range(w):
                v = px[y][x]
                if v >= 0 and 0 <= sy + y < H and 0 <= sx + x < W:
                    img[sy + y][sx + x] = pal[v]
st = (W * 3 + 3) & ~3
rows = [bytes(sum(([c[2], c[1], c[0]] for c in r), [])) + b'\0' * (st - W * 3) for r in reversed(img)]
hdr = struct.pack('<2sIHHI', b'BM', 54 + st * H, 0, 0, 54) + struct.pack('<IiiHHIIiiII', 40, W, H, 1, 24, 0, st * H, 0, 0, 0, 0)
open(out, 'wb').write(hdr + b''.join(rows))
print('n =', n, 'size', W, H)
