"""Nexus image for Quick Deposit's Quick Stack Station support, in the keep image's style
(tools/make_keep_image.py).
usage: python tools/make_station_image.py <station.png>
  station.png: a Quick Stack Station on the floor (708x580; a letter of the HUD at the left edge).
Writes dist/nexus/images/07-quick-stack-stations.jpg (1920x1080)."""
import math
import pathlib
import sys

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageFont

root = pathlib.Path(__file__).resolve().parent.parent
out = root / "dist" / "nexus" / "images"
GOLD = (232, 200, 130)
HOT = (255, 214, 140)
TXT = (230, 220, 200)
BOX = (22, 18, 14, 225)


def font(bold, size, italic=False):
    name = "georgiaz" if bold and italic else "georgiab" if bold else "georgiai" if italic else "georgia"
    return ImageFont.truetype(f"C:/Windows/Fonts/{name}.ttf", size)


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


def callout(im, x, y, title, sub, target=None, size=30, side=None, via=()):
    over = Image.new("RGBA", im.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    ft, fs = font(True, size), font(False, int(size * 0.74))
    lines = sub.split("\n") if sub else []
    w = int(max([d.textlength(title, font=ft)] + [d.textlength(s, font=fs) for s in lines]) + size * 1.2)
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
    return w, h


def shadowed_title(d, text, f, cx, y, fill):
    tw = d.textlength(text, font=f)
    d.text((cx - tw / 2 + 3, y + 4), text, font=f, fill=(0, 0, 0, 200))
    d.text((cx - tw / 2, y), text, font=f, fill=fill)


W, H = 1920, 1080

shot = Image.open(sys.argv[1]).convert("RGB").crop((30, 0, 708, 580))
shot = ImageEnhance.Contrast(shot).enhance(1.06)
shot = ImageEnhance.Color(shot).enhance(1.08).convert("RGBA")

# Backdrop: the shot, blurred and darkened.
s = max(W / shot.width, H / shot.height)
bg = shot.resize((int(shot.width * s) + 1, int(shot.height * s) + 1), Image.LANCZOS)
img = bg.crop(((bg.width - W) // 2, (bg.height - H) // 2, (bg.width - W) // 2 + W, (bg.height - H) // 2 + H))
img = img.filter(ImageFilter.GaussianBlur(28))
img.alpha_composite(Image.new("RGBA", (W, H), (10, 8, 14, 195)))

# The station, framed.
PH = 760
k = PH / shot.height
PW = round(shot.width * k)
PX, PY = 130, 210
panel = shot.resize((PW, PH), Image.LANCZOS)
shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
ImageDraw.Draw(shadow).rounded_rectangle((PX + 8, PY + 10, PX + PW + 8, PY + PH + 10), 14, fill=(0, 0, 0, 160))
img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(10)))
mask = Image.new("L", (PW, PH), 0)
ImageDraw.Draw(mask).rounded_rectangle((0, 0, PW, PH), 14, fill=255)
img.paste(panel, (PX, PY), mask)
ImageDraw.Draw(img).rounded_rectangle((PX, PY, PX + PW, PY + PH), 14, outline=GOLD + (240,), width=3)

d = ImageDraw.Draw(img)
shadowed_title(d, "QUICK STACK STATIONS", font(True, 66), W / 2, 40, HOT + (255,))
sub = "Quick Deposit  -  press E on a station: the same as G twice"
fs = font(False, 30, italic=True)
d.text((W / 2 - d.textlength(sub, font=fs) / 2, 126), sub, font=fs, fill=TXT + (240,))


def at(sx, sy):
    """A point of the (cropped) station shot on the canvas."""
    return PX + sx * k, PY + sy * k


X = PX + PW + 90
callout(img, X, 270, "Press E on the station", "a whole Quick Deposit for your base",
        target=at(330, 300), size=34, side="left")
callout(img, X, 450, "Your kept items stay", "stacks you marked with K\nnever leave your backpack", size=34)
callout(img, X, 660, "Sorted like G twice", "same item, same kind, chest names,\nthe chat report and the green glow",
        size=34)
callout(img, X, 870, "Works everywhere", "your world, friends' worlds and servers", size=30)

out.mkdir(parents=True, exist_ok=True)
img.convert("RGB").save(out / "07-quick-stack-stations.jpg", quality=92)
print(out / "07-quick-stack-stations.jpg", img.size)
