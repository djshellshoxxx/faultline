"""Generates Assets/icon.png — a dark, high-contrast ECG-style app icon
matching the plugin's medical/anatomical look. Pure stdlib, no Pillow."""
import struct, zlib, math, os

SIZE = 512
BG      = (0x06, 0x08, 0x0a, 255)
BORDER  = (0x0f, 0x6d, 0x5c, 255)
GREEN   = (0x2f, 0xf0, 0xc8, 255)
RED     = (0xff, 0x3b, 0x3b, 255)

px = [[list(BG) for _ in range(SIZE)] for _ in range(SIZE)]

def set_px(x, y, c, a=1.0):
    if 0 <= x < SIZE and 0 <= y < SIZE:
        dst = px[y][x]
        for i in range(3):
            dst[i] = int(dst[i] * (1 - a) + c[i] * a)
        dst[3] = 255

def thick_line(x0, y0, x1, y1, c, w):
    steps = int(max(abs(x1 - x0), abs(y1 - y0))) * 2 + 1
    for s in range(steps + 1):
        t = s / steps
        x = x0 + (x1 - x0) * t
        y = y0 + (y1 - y0) * t
        r = w / 2
        for dx in range(-int(r) - 1, int(r) + 2):
            for dy in range(-int(r) - 1, int(r) + 2):
                d = math.hypot(dx, dy)
                if d <= r:
                    a = 1.0 if d <= r - 1 else max(0.0, r - d)
                    set_px(int(x) + dx, int(y) + dy, c, a)

# ---- background fill with a sharp inner border (no rounding: matches "sharp edges") ---
margin = 18
for y in range(SIZE):
    for x in range(SIZE):
        on_border = (margin - 3 <= x < margin or SIZE - margin <= x < SIZE - margin + 3 or
                     margin - 3 <= y < margin or SIZE - margin <= y < SIZE - margin + 3)
        if on_border:
            px[y][x] = list(BORDER)

# ---- ECG / heart-monitor trace across the middle ----------------------------
mid = SIZE * 0.56
amp = SIZE * 0.30
pts = [
    (0.06, 0.0), (0.24, 0.0), (0.30, 0.10), (0.36, -0.95),
    (0.40, 1.0), (0.45, -0.35), (0.52, 0.05), (0.60, 0.0),
    (0.78, 0.0), (0.84, -0.18), (0.90, 0.0), (0.97, 0.0),
]
for i in range(len(pts) - 1):
    x0, y0 = pts[i]
    x1, y1 = pts[i + 1]
    thick_line(x0 * SIZE, mid - y0 * amp, x1 * SIZE, mid - y1 * amp, GREEN, SIZE / 42)

# faint glow duplicate underneath (draw first would be better, but a soft pass ok)
# small red transient tick + dot to echo the plugin's transient markers
thick_line(0.40 * SIZE, mid - 1.0 * amp - SIZE * 0.05, 0.40 * SIZE, mid - 1.0 * amp - SIZE * 0.14, RED, SIZE / 60)

# corner scalpel-style diagonal accent (bottom-right), echoing the "surgeon" theme
thick_line(SIZE * 0.74, SIZE * 0.80, SIZE * 0.88, SIZE * 0.66, GREEN, SIZE / 70)

def write_png(path, size, pixels):
    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))
    raw = bytearray()
    for row in pixels:
        raw.append(0)  # filter type None
        for r, g, b, a in row:
            raw += bytes((r, g, b, a))
    compressed = zlib.compress(bytes(raw), 9)
    ihdr = struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', ihdr))
        f.write(chunk(b'IDAT', compressed))
        f.write(chunk(b'IEND', b''))

def downscale(pixels, factor):
    n = SIZE // factor
    out = []
    for y in range(n):
        row = []
        for x in range(n):
            rs = gs = bs = as_ = 0
            for dy in range(factor):
                for dx in range(factor):
                    p = pixels[y * factor + dy][x * factor + dx]
                    rs += p[0]; gs += p[1]; bs += p[2]; as_ += p[3]
            cnt = factor * factor
            row.append((rs // cnt, gs // cnt, bs // cnt, as_ // cnt))
        out.append(row)
    return out

out_dir = os.path.join(os.path.dirname(__file__), '..', 'Assets')
os.makedirs(out_dir, exist_ok=True)
write_png(os.path.join(out_dir, 'icon.png'), SIZE, px)
small = downscale(px, 4)  # 128x128
write_png(os.path.join(out_dir, 'icon_small.png'), SIZE // 4, small)
print('wrote icon.png (512) and icon_small.png (128) to', out_dir)
