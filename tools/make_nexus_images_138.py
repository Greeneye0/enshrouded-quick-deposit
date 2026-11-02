"""Nexus images for Quick Deposit 1.3.8, in the keep image's style (dark brown boxes, gold outline,
Georgia, dashed gold arrows; tools/make_keep_image.py).
usage: python tools/make_nexus_images_138.py <glow.webp>
  glow.webp: chests lit green after a Quick Stack Station deposit, seen through other chests
  (1625x923; the HUD at the top left and the compass are cropped off).
Writes dist/nexus/images/01-title.jpg (1280x640) and 06-see-where-it-went.jpg (1920x1080)."""
import math
import pathlib
import sys

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageFont

root = pathlib.Path(__file__).resolve().parent.parent
out = root / "dist" / "nexus" / "images"
GOLD = (232, 200, 130)
HOT = (255, 214, 140)
TXT = (230, 220, 200)
GREEN = (120, 255, 110)
BOX = (22, 18, 14, 225)


def font(bold, size, italic=False):
    name = "georgiaz" if bold and italic else "georgiab" if bold else "georgiai" if italic else "georgia"
    return ImageFont.truetype(f"C:/Windows/Fonts/{name}.ttf", size)


def sans(size):
    return ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", size)


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


shot = Image.open(sys.argv[1]).convert("RGB")
CROP = (150, 80, 1625, 910)  # no HUD, no compass; 16:9
shot = shot.crop(CROP)
shot = ImageEnhance.Contrast(shot).enhance(1.05)
shot = ImageEnhance.Color(shot).enhance(1.05).convert("RGBA")


def src(x, y, k):
    """A point of the original 1625x923 shot on a canvas scaled by k."""
    return (x - CROP[0]) * k, (y - CROP[1]) * k


# --- 06: see where it went (1920x1080) ---------------------------------------------------------
W, H = 1920, 1080
k = W / shot.width
img = shot.resize((W, round(shot.height * k)), Image.LANCZOS).crop((0, 0, W, H))
# A dark band behind the title.
band = Image.new("RGBA", (W, H), (0, 0, 0, 0))
bd = ImageDraw.Draw(band)
for y in range(230):
    bd.line((0, y, W, y), fill=(10, 8, 14, int(215 * (1 - y / 230) ** 1.3)))
img.alpha_composite(band)
d = ImageDraw.Draw(img)
shadowed_title(d, "SEE WHERE IT WENT", font(True, 66), W / 2, 34, HOT + (255,))
sub = "Quick Deposit  -  the chests that got something glow green, even through walls"
fs = font(False, 30, italic=True)
d.text((W / 2 - d.textlength(sub, font=fs) / 2, 120), sub, font=fs, fill=TXT + (240,))

callout(img, 120, 760, "Seen through the chests in front",
        "a green silhouette, drawn over everything", target=src(480, 530, k), size=32, side="top")
callout(img, 1030, 300, "Filled chests glow green",
        "the game's own outline, pulsing", target=src(880, 500, k), size=32, side="bottom")
callout(img, 1180, 800, "Quick Stack Stations too",
        "press E: a whole Quick Deposit,\nyour kept items stay", size=30)
callout(img, 1180, 640, "10 seconds",
        "after you close the inventory (glowMs)", size=28)

out.mkdir(parents=True, exist_ok=True)
img.convert("RGB").save(out / "06-see-where-it-went.jpg", quality=92)
print(out / "06-see-where-it-went.jpg", img.size)

# --- 01: title card (1280x640) -----------------------------------------------------------------
W, H = 1280, 640
bg = shot.resize((round(shot.width * H / shot.height), H), Image.LANCZOS)
bg = bg.crop(((bg.width - W) // 2, 0, (bg.width - W) // 2 + W, H)).filter(ImageFilter.GaussianBlur(40))
card = bg.copy()
card.alpha_composite(Image.new("RGBA", (W, H), (12, 10, 26, 228)))
d = ImageDraw.Draw(card)
d.text((74, 92), "QUICK DEPOSIT", font=font(True, 62), fill=(240, 232, 220, 255))
d.text((76, 186), "for  ENSHROUDED", font=font(False, 26), fill=GOLD + (255,))
d.line((74, 240, 560, 240), fill=GOLD + (200,), width=2)
d.text((74, 262), "Press G twice.", font=font(True, 38), fill=(240, 232, 220, 255))
d.text((76, 312), "Your backpack sorts itself into your chests.", font=font(False, 26), fill=(205, 200, 215, 255))
bullets = ["Same item, same kind, then chest name",
           "Filled chests glow green, even through walls",
           "Quick Stack Stations do the same",
           "Hotbar, gear and kept items never move",
           "Servers and friends' worlds, 15 languages"]
for i, b in enumerate(bullets):
    y = 378 + i * 44
    d.ellipse((80, y + 12, 92, y + 24), fill=GOLD + (255,))
    d.text((106, y), b, font=sans(24), fill=(230, 226, 232, 255))
# The glow shot, framed.
PW, PH = 560, 315
px, py = W - PW - 50, (H - PH) // 2
sx0, sy0 = src(370, 300, 1)
panel = shot.crop((sx0, sy0, sx0 + 960, sy0 + 540)).resize((PW, PH), Image.LANCZOS)
shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
ImageDraw.Draw(shadow).rounded_rectangle((px + 8, py + 10, px + PW + 8, py + PH + 10), 14, fill=(0, 0, 0, 160))
card.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(10)))
mask = Image.new("L", (PW, PH), 0)
ImageDraw.Draw(mask).rounded_rectangle((0, 0, PW, PH), 14, fill=255)
card.paste(panel, (px, py), mask)
ImageDraw.Draw(card).rounded_rectangle((px, py, px + PW, py + PH), 14, outline=GOLD + (240,), width=3)
card.convert("RGB").save(out / "01-title.jpg", quality=92)
print(out / "01-title.jpg", card.size)
