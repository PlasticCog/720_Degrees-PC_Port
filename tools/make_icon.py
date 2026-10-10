"""Draw the program icon (original artwork, in the spirit of the title screen: a skateboard deck
seen from above with a "720°" logo).  Needs Pillow.

    python tools/make_icon.py

Writes port/res/icon.ico (16-256 px, used by the Windows executables), port/res/icon.png
(256 px) and port/src/app/icon_rgba.h (64 x 64 RGBA, set as the window icon on Linux/macOS).
"""
import math
import os
import random

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
N = 1024                                     # drawing size, scaled down at the end

ORANGE = (247, 150, 30, 255)
RED = (226, 38, 28, 255)
DARK = (40, 16, 8, 255)
YELLOW = (255, 214, 32, 255)
GREEN = (40, 196, 64, 255)
STEEL = (150, 158, 172, 255)
GRIP = [(226, 38, 28), (40, 196, 64), (40, 80, 230), (255, 214, 32), (240, 240, 240), (247, 150, 30)]


def stadium(draw, box, fill, outline=None, width=0):
    x0, y0, x1, y1 = box
    draw.rounded_rectangle(box, radius=(y1 - y0) // 2, fill=fill, outline=outline, width=width)


def deck_layer():
    """The board, drawn horizontally (nose right), to be rotated."""
    img = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    top, bottom = 330, 694
    # wheels and trucks first: the deck covers their inner halves
    for x in (215, 809):
        d.rounded_rectangle((x - 70, top - 92, x + 70, bottom + 92), radius=22, fill=STEEL, outline=DARK, width=12)
        for y0, y1 in ((top - 150, top - 30), (bottom + 30, bottom + 150)):
            d.rounded_rectangle((x - 62, y0, x + 62, y1), radius=34, fill=GREEN, outline=DARK, width=12)
            d.rectangle((x - 20, y0 + 10, x + 20, y1 - 10), fill=RED)
    # the deck: orange with a red band and speckled grip tape at both ends, like the title
    mask = Image.new('L', (N, N), 0)
    stadium(ImageDraw.Draw(mask), (40, top, N - 40, bottom), 255)
    art = Image.new('RGBA', (N, N), ORANGE)
    a = ImageDraw.Draw(art)
    a.polygon([(560, top - 10), (760, top - 10), (640, bottom + 10), (440, bottom + 10)], fill=RED)
    rnd = random.Random(720)
    for _ in range(5000):
        x = rnd.choice([rnd.uniform(40, 330), rnd.uniform(760, N - 40)])
        y = rnd.uniform(top, bottom)
        c = rnd.choice(GRIP)
        r = rnd.uniform(5, 10)
        a.ellipse((x - r, y - r, x + r, y + r), fill=c + (255,))
    img.paste(art, (0, 0), mask)
    stadium(d, (40, top, N - 40, bottom), None, DARK, 16)
    return img


def logo_mask():
    """Blocky italic "720" and a degree ring, white on black."""
    m = Image.new('L', (N, N), 0)
    d = ImageDraw.Draw(m)
    W, H, S = 172, 262, 56                   # digit width, height, stroke
    gap = 38
    x0 = (N - (3 * W + 2 * gap + 90)) // 2
    y0 = (N - H) // 2
    skew = 0.28

    def poly(pts, dx):
        d.polygon([(x0 + dx + x + (H - y) * skew, y0 + y) for x, y in pts], fill=255)

    def rect(x, y, w, h, dx):
        poly([(x, y), (x + w, y), (x + w, y + h), (x, y + h)], dx)

    # 7
    rect(0, 0, W, S, 0)
    poly([(W - S, S), (W, S), (W * 0.55, H), (W * 0.55 - S, H)], 0)
    # 2
    dx = W + gap
    rect(0, 0, W, S, dx)
    rect(W - S, 0, S, H // 2, dx)
    rect(0, H // 2 - S // 2, W, S, dx)
    rect(0, H // 2 - S // 2, S, H // 2 + S // 2, dx)
    rect(0, H - S, W, S, dx)
    # 0
    dx = 2 * (W + gap)
    rect(0, 0, W, S, dx)
    rect(0, H - S, W, S, dx)
    rect(0, 0, S, H, dx)
    rect(W - S, 0, S, H, dx)
    # degree ring
    cx, cy, r = x0 + 3 * W + 2 * gap + 36 + H * skew, y0 + 28, 39
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
    d.ellipse((cx - r + 23, cy - r + 23, cx + r - 23, cy + r - 23), fill=0)
    return m


def compose():
    base = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    deck = deck_layer().rotate(40, resample=Image.BICUBIC, center=(N / 2, N / 2))
    base.alpha_composite(deck)
    m = logo_mask().rotate(10, resample=Image.BICUBIC, center=(N / 2, N / 2))
    outline = m.filter(ImageFilter.MaxFilter(41))
    shadow = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    shadow.paste((10, 4, 2, 200), (0, 0), outline.filter(ImageFilter.GaussianBlur(10)))
    base.alpha_composite(shadow, (18, 22))
    red = Image.new('RGBA', (N, N), RED)
    base.paste(red, (0, 0), outline)
    yellow = Image.new('RGBA', (N, N), YELLOW)
    base.paste(yellow, (0, 0), m)
    # a lighter top edge on the letters
    hi = m.filter(ImageFilter.MinFilter(9))
    hi = Image.composite(Image.new('L', (N, N), 0), hi, hi.transform((N, N), Image.AFFINE, (1, 0, 0, 0, 1, 10)))
    base.paste(Image.new('RGBA', (N, N), (255, 246, 170, 255)), (0, 0), hi)
    return base


def main():
    art = compose()
    res = os.path.join(ROOT, 'port', 'res')
    os.makedirs(res, exist_ok=True)
    sizes = [16, 24, 32, 48, 64, 128, 256]
    imgs = {s: art.resize((s, s), Image.LANCZOS) for s in sizes}
    for s in (16, 24, 32):
        imgs[s] = imgs[s].filter(ImageFilter.UnsharpMask(radius=1, percent=80, threshold=2))
    imgs[256].save(os.path.join(res, 'icon.ico'), format='ICO', sizes=[(s, s) for s in sizes],
                   append_images=[imgs[s] for s in sizes if s != 256])
    imgs[256].save(os.path.join(res, 'icon.png'))
    px = imgs[64].tobytes()
    lines = ['// Program icon, 64 x 64 RGBA (generated by tools/make_icon.py; original artwork).',
             '#pragma once',
             '#include <cstdint>',
             'constexpr int ICON_W = 64, ICON_H = 64;',
             'static const uint8_t ICON_RGBA[64 * 64 * 4] = {']
    for i in range(0, len(px), 32):
        lines.append('\t' + ', '.join('%d' % b for b in px[i:i + 32]) + ',')
    lines.append('};')
    open(os.path.join(ROOT, 'port', 'src', 'app', 'icon_rgba.h'), 'w', newline='\n').write('\n'.join(lines) + '\n')
    print('icon written:', ', '.join('%d' % s for s in sizes))


if __name__ == '__main__':
    main()
