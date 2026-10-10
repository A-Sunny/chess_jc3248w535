import math, sys
from PIL import Image

W = H = 36
SS = 4  # supersampling per axis

# ---------- shape primitives (return f(x,y)->bool) ----------
def circle(cx, cy, r):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r

def ellipse(cx, cy, rx, ry):
    return lambda x, y: ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1

def rect(x0, y0, x1, y1):
    return lambda x, y: x0 <= x <= x1 and y0 <= y <= y1

def poly(pts):
    def f(x, y):
        inside = False
        n = len(pts)
        j = n - 1
        for i in range(n):
            xi, yi = pts[i]; xj, yj = pts[j]
            if (yi > y) != (yj > y):
                if x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                    inside = not inside
            j = i
        return inside
    return f

def line(x0, y0, x1, y1, w):
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy
    def f(x, y):
        t = max(0, min(1, ((x - x0) * dx + (y - y0) * dy) / L2)) if L2 else 0
        px, py = x0 + t * dx, y0 + t * dy
        return (x - px) ** 2 + (y - py) ** 2 <= (w / 2) ** 2
    return f

def union(*fs):
    return lambda x, y: any(f(x, y) for f in fs)

# ---------- piece designs (36x36 grid, y down) ----------
def base(y0=28.5):
    return union(rect(7, y0 + 1.5, 29, 33.5), rect(9, y0 - 1.5, 27, y0 + 2))

PIECES = {}

# pawn
PIECES[1] = (
    union(circle(18, 10.5, 5.3), rect(15.5, 14, 20.5, 17.5), ellipse(18, 17.5, 6.8, 1.9),
          poly([(15, 18), (21, 18), (25.5, 28), (10.5, 28)]), rect(8, 28, 28, 33.5)),
    union(line(11, 28, 25, 28, 0.9)))

# knight (faces left)
knight_pts = [(9, 28), (10, 24), (13, 20.5), (12.5, 18.8), (8.5, 21), (5.5, 20.5), (4.3, 17.5),
              (5.3, 14.3), (9, 10.5), (13, 6.8), (15.2, 5), (16.8, 2.2), (18.8, 4.8), (21.5, 4),
              (23, 7), (25.5, 6.5), (26, 10), (28.5, 10.8), (28, 15.5), (30, 17), (28.5, 21),
              (28.5, 28)]
PIECES[2] = (
    union(poly(knight_pts), rect(7, 28, 29, 33.5)),
    union(circle(12.8, 10.6, 1.3), circle(6.6, 16.8, 0.9), line(5.8, 19.2, 9.5, 19.4, 0.8),
          line(11, 28, 26, 28, 0.9), line(14, 13, 17, 18, 0.9)))

# bishop
PIECES[3] = (
    union(circle(18, 4.5, 2.3), ellipse(18, 13, 6.2, 8.6),
          poly([(13.5, 19), (22.5, 19), (25.5, 28), (10.5, 28)]), rect(10.5, 20.5, 25.5, 23.5),
          rect(8, 28, 28, 33.5)),
    union(line(15.5, 16, 20, 8.5, 1.1), line(13, 22, 23, 22, 0.9), line(11, 28, 25, 28, 0.9)))

# rook
PIECES[4] = (
    union(rect(8, 5, 13, 12), rect(15.5, 5, 20.5, 12), rect(23, 5, 28, 12), rect(9, 11, 27, 14.5),
          poly([(11.5, 14), (24.5, 14), (25.5, 27), (10.5, 27)]), rect(7, 27, 29, 33.5)),
    union(line(10, 14.5, 26, 14.5, 0.9), line(10, 27, 26, 27, 0.9)))

# queen
queen_crown = poly([(10, 26), (7.5, 10), (11, 16), (12.5, 7), (15.5, 16), (18, 5), (20.5, 16),
                    (23.5, 7), (25, 16), (28.5, 10), (26, 26)])
PIECES[5] = (
    union(queen_crown, circle(7.5, 9, 2.2), circle(12.5, 6.3, 2.2), circle(18, 4.3, 2.4),
          circle(23.5, 6.3, 2.2), circle(28.5, 9, 2.2),
          poly([(10, 25), (26, 25), (28, 29), (8, 29)]), rect(7, 29, 29, 33.5)),
    union(line(11, 22, 25, 22, 0.9), line(9, 29, 27, 29, 0.9)))

# king
PIECES[6] = (
    union(rect(16.5, 0.8, 19.5, 9), rect(14, 3, 22, 6), ellipse(18, 15.5, 8.2, 6.5),
          poly([(11.5, 17), (24.5, 17), (26.5, 29), (9.5, 29)]), rect(7, 29, 29, 33.5)),
    union(line(11, 22, 25, 22, 0.9), line(9, 29, 27, 29, 0.9), line(18, 10, 18, 15, 0.9)))

# ---------- rasterise ----------
def raster(body, detail):
    sil = [[False] * W for _ in range(H)]
    det = [[False] * W for _ in range(H)]
    for py in range(H):
        for px in range(W):
            cb = cd = 0
            for j in range(SS):
                for i in range(SS):
                    x = px + (i + 0.5) / SS
                    y = py + (j + 0.5) / SS
                    if body(x, y): cb += 1
                    if detail(x, y): cd += 1
            sil[py][px] = cb >= SS * SS / 2
            det[py][px] = cd >= SS * SS / 2
    code = [[0] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            if not sil[y][x]:
                continue
            edge = False
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if not (0 <= nx < W and 0 <= ny < H) or not sil[ny][nx]:
                    edge = True
            if edge: code[y][x] = 1
            elif det[y][x]: code[y][x] = 3
            else: code[y][x] = 2
    return code

def pack(code):
    stride = (W * 2 + 7) // 8
    out = []
    for y in range(H):
        row = [0] * stride
        for x in range(W):
            row[x // 4] |= (code[y][x] & 3) << ((x % 4) * 2)
        out += row
    return out, stride

sprites = {t: raster(*PIECES[t]) for t in range(1, 7)}

# ---------- C header ----------
names = {1: 'pawn', 2: 'knight', 3: 'bishop', 4: 'rook', 5: 'queen', 6: 'king'}
lines = ["#pragma once", "#include <stdint.h>", "",
         "// 36x36 chess piece sprites, 2 bits per pixel (generated by gen_pieces.py)",
         "// pixel codes: 0 = transparent, 1 = outline, 2 = fill, 3 = detail",
         f"#define PIECE_W {W}", f"#define PIECE_H {H}", f"#define PIECE_STRIDE {(W*2+7)//8}", "",
         "static const uint8_t PIECE_BMP[6][PIECE_H * PIECE_STRIDE] = {"]
for t in range(1, 7):
    data, stride = pack(sprites[t])
    lines.append(f"  {{ // {names[t]}")
    for y in range(H):
        row = data[y * stride:(y + 1) * stride]
        lines.append("    " + ",".join(f"0x{b:02X}" for b in row) + ("," if not (y == H - 1) else ""))
    lines.append("  }" + ("," if t < 6 else ""))
lines.append("};")
open(sys.argv[1], "w").write("\n".join(lines) + "\n")

# ---------- preview ----------
LIGHT = (240, 217, 181); DARK = (181, 136, 99)
WC = {1: (20, 20, 20), 2: (250, 250, 245), 3: (20, 20, 20)}
BC = {1: (12, 12, 14), 2: (60, 60, 68), 3: (230, 230, 230)}
SC = 5; TILE = 40
img = Image.new("RGB", (6 * TILE * SC, 4 * TILE * SC))
px = img.load()
combos = [(WC, LIGHT), (BC, DARK), (WC, DARK), (BC, LIGHT)]
for r, (pal, bg) in enumerate(combos):
    for t in range(1, 7):
        ox = (t - 1) * TILE * SC; oy = r * TILE * SC
        for yy in range(TILE * SC):
            for xx in range(TILE * SC):
                px[ox + xx, oy + yy] = bg
        for y in range(H):
            for x in range(W):
                c = sprites[t][y][x]
                if c:
                    for dy in range(SC):
                        for dx in range(SC):
                            px[ox + (x + 2) * SC + dx, oy + (y + 2) * SC + dy] = pal[c]
img.save(sys.argv[2])
print("done")
