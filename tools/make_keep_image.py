"""Nexus image for Quick Deposit's keep marks: the backpack before and after G G, with callouts
in the Embervale Teleport style (dark brown boxes, gold outline, Georgia, dashed gold arrows).
usage: python tools/make_keep_image.py <before.webp> <after.webp>
Writes dist/nexus/images/05-keep-in-backpack.jpg (1920x1080)."""
import math
import pathlib
import sys

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageFont

root = pathlib.Path(__file__).resolve().parent.parent
out = root / "dist" / "nexus" / "images"
GOLD = (232, 200, 130)
HOT = (255, 214, 140)
TXT = (230, 220, 200)
TEAL = (120, 230, 200)
BOX = (22, 18, 14, 225)
W, H = 1920, 1080


def font(bold, size, italic=False):
    name = "georgiaz" if bold and italic else "georgiab" if bold else "georgiai" if italic else "georgia"
    return ImageFont.truetype(f"C:/Windows/Fonts/{name}.ttf", size)


def load(path):
    im = Image.open(path).convert("RGB")
    im = im.crop((0, 0, im.width, 1200))  # without the key-hint footer
    im = ImageEnhance.Contrast(im).enhance(1.06)
    return ImageEnhance.Color(im).enhance(1.06).convert("RGBA")


def dashed(d, pts, width=4):
    for n in range(len(pts) - 1):
        (ax, ay), (bx, by) = pts[n], pts[n + 1]
        dist = math.hypot(bx - ax, by - ay)
        ux, uy = (bx - ax) / dist, (by - ay) / dist
        end = dist - (20 if n == len(pts) - 2 else 0)
        t = 0.0
        while t < end:
            b = min(t + 14, end)
            d.line((ax + ux * t, ay + uy * t, ax + ux * b, ay + uy * b), fill=GOLD + (240,), width=width)
            t += 23
    tx, ty = pts[-1]
    px, py = -uy, ux
    base = (tx - ux * 24, ty - uy * 24)
    d.polygon([(tx, ty), (base[0] + px * 11, base[1] + py * 11), (base[0] - px * 11, base[1] - py * 11)],
              fill=GOLD + (250,))


def callout(im, x, y, title, sub, target=None, size=30, width=None, side=None, via=()):
    over = Image.new("RGBA", im.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    ft, fs = font(True, size), font(False, int(size * 0.74))
    lines = sub.split("\n") if sub else []
    w = width or int(max([d.textlength(title, font=ft)] + [d.textlength(s, font=fs) for s in lines]) + size * 1.2)
    h = int(size * (1.55 + 1.0 * len(lines)))
    d.rounded_rectangle((x, y, x + w, y + h), radius=int(size * 0.35), fill=BOX, outline=GOLD + (235,), width=3)
    d.text((x + size * 0.55, y + size * 0.28), title, font=ft, fill=HOT + (255,))
    for i, s in enumerate(lines):
        d.text((x + size * 0.57, y + size * (1.42 + i * 1.0)), s, font=fs, fill=TXT + (240,))
    if target:
        side = side or ("right" if target[0] > x + w else "left" if target[0] < x else "bottom")
        start = {"right": (x + w, y + h / 2), "left": (x, y + h / 2), "bottom": (x + w / 2, y + h),
                 "top": (x + w / 2, y)}[side]
        dashed(d, [start, *via, target])
    im.alpha_composite(over)


def ring(im, box, color=GOLD, width=4):
    over = Image.new("RGBA", im.size, (0, 0, 0, 0))
    ImageDraw.Draw(over).rounded_rectangle(box, radius=10, outline=color + (245,), width=width)
    im.alpha_composite(over)


before, after = load(sys.argv[1]), load(sys.argv[2])

# Backdrop: the before shot, blurred and darkened.
s = max(W / before.width, H / before.height)
bg = before.resize((int(before.width * s) + 1, int(before.height * s) + 1), Image.LANCZOS)
y0 = (bg.height - H) // 2
img = bg.crop((0, y0, W, y0 + H)).filter(ImageFilter.GaussianBlur(24))
img.alpha_composite(Image.new("RGBA", (W, H), (10, 8, 14, 190)))

# The two backpacks.
PH = 790
k = PH / before.height
PW = round(before.width * k)
LX, RX, PY = 150, W - 150 - PW, 172
for shot, x in ((before, LX), (after, RX)):
    panel = shot.resize((PW, PH), Image.LANCZOS)
    shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((x + 8, PY + 10, x + PW + 8, PY + PH + 10), 12, fill=(0, 0, 0, 150))
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(10)))
    img.alpha_composite(panel, (x, PY))


def at(shot_x, sx, sy):
    """A point in a source screenshot, on the canvas."""
    return shot_x + sx * k, PY + sy * k


d = ImageDraw.Draw(img)
title = "KEEP ITEMS IN YOUR BACKPACK"
ft = font(True, 62)
tw = d.textlength(title, font=ft)
d.text((W / 2 - tw / 2 + 3, 40 + 4), title, font=ft, fill=(0, 0, 0, 200))
d.text((W / 2 - tw / 2, 40), title, font=ft, fill=HOT + (255,))
fs = font(False, 28, italic=True)
sub = "Quick Deposit  -  hover a stack and press K"
d.text((W / 2 - d.textlength(sub, font=fs) / 2, 118), sub, font=fs, fill=TXT + (235,))

# Step labels over each panel.
for x, label in ((LX, "1.  Mark with K"), (RX, "2.  Press G twice")):
    f = font(True, 34)
    lw = d.textlength(label, font=f)
    d.text((x + PW / 2 - lw / 2, PY + PH + 12), label, font=f, fill=HOT + (255,))

# Middle arrow.
mx0, mx1, my = LX + PW + 50, RX - 50, PY + PH * 0.42
d.line((mx0, my, mx1 - 30, my), fill=GOLD + (255,), width=10)
d.polygon([(mx1, my), (mx1 - 44, my - 30), (mx1 - 44, my + 30)], fill=GOLD + (255,))
f = font(True, 34)
gg = "G  G"
d.text(((mx0 + mx1) / 2 - d.textlength(gg, font=f) / 2, my - 62), gg, font=f, fill=HOT + (255,))

# Before: the marked stacks (source pixels of the before shot).
ring(img, (*at(LX, 382, 412), *at(LX, 760, 507)), TEAL, 4)   # arrows 57, 33, 191 + coins 913
ring(img, (*at(LX, 760, 320), *at(LX, 853, 413)), TEAL, 4)   # arrow 15
callout(img, *at(LX, 150, 640), "Marked stacks", "teal border: these stay",
        target=at(LX, 560, 512), size=30, side="top")
callout(img, *at(LX, 150, 800), "K again to unmark", "split a stack to keep only part", size=26)

# After: what stayed.
ring(img, (*at(RX, 470, 226), *at(RX, 846, 318)), TEAL, 4)
ring(img, (*at(RX, 659, 414), *at(RX, 752, 507)), TEAL, 4)
callout(img, *at(RX, 120, 640), "Kept: still in your backpack",
        "your chat lists what stayed", target=at(RX, 560, 322), size=30, side="top")
callout(img, *at(RX, 120, 800), "Everything else", "sorted into your chests", size=26)
callout(img, LX + PW + 40, *at(0, 0, 1010)[1:], "Hotbar", "never moves", target=at(RX, 90, 1040),
        size=28, width=RX - LX - PW - 80)

out.mkdir(parents=True, exist_ok=True)
img.convert("RGB").save(out / "05-keep-in-backpack.jpg", quality=92)
print(out / "05-keep-in-backpack.jpg", img.size)
