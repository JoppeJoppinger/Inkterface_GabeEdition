"""Render README preview images for each display view.

Portraits and GabeCube are decoded from the firmware bitmaps exactly as the panel draws
them. Statistics and Now Playing are illustrative renderings of the same layouts with
sample data. Requires Pillow. Run from the repository root:
    python scripts/render_previews.py
"""
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "previews"
FW = ROOT / "firmware" / "src"
W, H = 648, 480
MONO = "C:/Windows/Fonts/consola.ttf"
MONO_BOLD = "C:/Windows/Fonts/consolab.ttf"


def font(size, bold=False):
    return ImageFont.truetype(MONO_BOLD if bold else MONO, size)


def decode_xbm(header, name):
    text = (FW / header).read_text()
    body = re.search(name + r"\[\]\s*=\s*\{(.*?)\};", text, re.S).group(1)
    data = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})", body))
    img = Image.new("1", (W, H), 1)
    px = img.load()
    stride = (W + 7) // 8
    for y in range(H):
        for x in range(W):
            if (data[y * stride + x // 8] >> (x % 8)) & 1:
                px[x, y] = 0
    return img


def rounded(d, box, width=2):
    for i in range(width):
        d.rounded_rectangle(
            (box[0] + i, box[1] + i, box[2] - i, box[3] - i), radius=4, outline=0
        )


def statistics():
    img = Image.new("1", (W, H), 1)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((5, 5, 105, 105), radius=3, fill=0)
    d.ellipse((24, 24, 86, 86), fill=1)
    d.ellipse((32, 32, 78, 78), fill=0)
    d.text((120, 15), "steammachine", font=font(30, True), fill=0)
    d.text((120, 50), "SteamOS 3.8 - Steam 2026-10", font=font(18), fill=0)
    d.text((120, 80), "Game: idle", font=font(18), fill=0)

    def discrete(x, y, title, value):
        rounded(d, (x, y, x + 208, y + 25))
        d.text((x + 8, y + 4), title, font=font(18, True), fill=0)
        d.text((x + 200 - d.textlength(value, font=font(18, True)), y + 4), value,
               font=font(18, True), fill=0)

    def spark(x, y, title, value, series):
        rounded(d, (x, y, x + 208, y + 89))
        d.line((x, y + 26, x + 208, y + 26), fill=0)
        d.text((x + 8, y + 4), title, font=font(18, True), fill=0)
        d.text((x + 200 - d.textlength(value, font=font(18, True)), y + 4), value,
               font=font(18, True), fill=0)
        pts = [(x + 10 + i * 189 / (len(series) - 1), y + 74 - v * 34)
               for i, v in enumerate(series)]
        for dx, dy in ((0, 0), (0, 1), (0, -1), (1, 0), (-1, 0)):
            d.line([(px + dx, py + dy) for px, py in pts], fill=0)

    for i, (t, v) in enumerate([("OS", "3.8.2"), ("BIOS", "F7B"), ("STEAM", "Stable")]):
        discrete(5 + i * 214, 115, t, v)
    rows = [
        [("CPU", "62C", [.3, .35, .5, .45, .6, .55, .7, .65, .8, .75]),
         ("GPU", "58C", [.4, .45, .4, .5, .6, .7, .65, .6, .7, .72]),
         ("FAN", "1850", [.2, .2, .3, .35, .4, .4, .5, .55, .5, .5])],
        [("CPU", "34%", [.2, .5, .3, .6, .4, .8, .5, .6, .35, .4]),
         ("GPU", "97%", [.8, .9, .95, .9, .97, .95, .9, .96, .97, .97]),
         ("MEM", "41%", [.4, .4, .41, .42, .41, .43, .42, .41, .41, .41])],
        [("DISK", "63%", [.62, .62, .62, .63, .63, .63, .63, .63, .63, .63]),
         ("NET", "2.4MB", [.1, .2, .6, .3, .8, .5, .2, .4, .3, .2]),
         ("LOAD", "0.58", [.3, .4, .5, .45, .55, .6, .5, .55, .58, .58])],
    ]
    y = 146
    for row in rows:
        for i, (t, v, s) in enumerate(row):
            spark(5 + i * 214, y, t, v, s)
        y += 95
    d.text((5, H - 24), "4.05 V", font=font(11), fill=0)
    d.text((5, H - 12), "INKTF-3158C8 IFv01", font=font(11), fill=0)
    return img


def now_playing():
    img = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(img)
    cx = 18
    cover = decode_xbm("gabe_cube.h", "GABE_CUBE_BITS").convert("L").resize((260, 193))
    cy = 18 + (430 - 193) // 2
    img.paste(cover, (cx, cy))
    d.rectangle((cx - 2, cy - 2, cx + 260, cy + 193), outline=0)
    tx = 18 + 260 + 18
    d.text((tx, 18), "EXAMPLE GAME:", font=font(30, True), fill=0)
    d.text((tx, 56), "SAMPLE EDITION", font=font(30, True), fill=0)
    d.text((tx, 200), "PLAYTIME", font=font(17, True), fill=0)
    d.text((tx, 224), "42.5 HRS", font=font(25, True), fill=0)
    d.text((tx, 290), "ACHIEVEMENTS", font=font(17, True), fill=0)
    d.text((tx, 314), "18 / 40", font=font(25, True), fill=0)
    bar_w = W - tx - 18
    d.rectangle((tx, 350, tx + bar_w - 1, 370), outline=0)
    d.rectangle((tx + 3, 353, tx + 3 + int(bar_w * 18 / 40) - 6, 367), fill=0)
    d.text((W - 18 - d.textlength("NOW PLAYING", font=font(14)), H - 38),
           "NOW PLAYING", font=font(14), fill=0)
    return img.convert("1", dither=Image.FLOYDSTEINBERG)


def save(img, name):
    img.convert("1").save(OUT / name, optimize=True)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for i in range(5):
        save(decode_xbm("gabe_portrait.h", f"GABE_PORTRAIT_{i}"), f"portrait-{i + 1}.png")
    save(decode_xbm("gabe_cube.h", "GABE_CUBE_BITS"), "gabecube.png")
    save(statistics(), "statistics.png")
    save(now_playing(), "now-playing.png")


if __name__ == "__main__":
    main()
