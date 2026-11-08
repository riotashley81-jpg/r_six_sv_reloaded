"""Builds the Vita LiveArea images in vita/sce_sys from the game's own art.

    python vita/make_livearea.py ICON BACKGROUND

ICON is the APK's launcher icon (apk/xperia-play/res/drawable/icon.png) and
BACKGROUND a screenshot of the title splash (any size; it is cropped to fit).
The LiveArea only accepts 8-bit palettised PNGs, so every image is quantised.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sce_sys")


def fit(im, w, h):
    """Scale to cover w x h, then centre-crop."""
    s = max(w / im.width, h / im.height)
    im = im.resize((round(im.width * s), round(im.height * s)), Image.LANCZOS)
    x, y = (im.width - w) // 2, (im.height - h) // 2
    return im.crop((x, y, x + w, y + h))


def save8(im, path):
    im.convert("RGB").quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG).save(path, optimize=True)
    print("wrote", os.path.relpath(path), im.size)


def main(icon_path, bg_path):
    os.makedirs(os.path.join(OUT, "livearea", "contents"), exist_ok=True)

    # bubble: the launcher icon, its rounded corners filled with black (the
    # LiveArea masks the bubble to a circle anyway)
    icon = Image.open(icon_path).convert("RGBA")
    flat = Image.new("RGBA", icon.size, (0, 0, 0, 255))
    flat.alpha_composite(icon)
    save8(fit(flat, 128, 128), os.path.join(OUT, "icon0.png"))

    # background: the splash, darkened towards the bottom where the LiveArea
    # draws its buttons
    bg = fit(Image.open(bg_path).convert("RGB"), 840, 500)
    shade = Image.new("L", bg.size, 0)
    d = ImageDraw.Draw(shade)
    for y in range(bg.height):
        d.line([(0, y), (bg.width, y)], fill=int(150 * max(0, (y - 300) / 200)))
    bg = Image.composite(Image.new("RGB", bg.size, (0, 0, 0)), bg, shade)
    save8(bg, os.path.join(OUT, "livearea", "contents", "bg.png"))

    # full-screen background (pic0), shown behind the app's info and while it boots
    save8(fit(Image.open(bg_path).convert("RGB"), 960, 544), os.path.join(OUT, "pic0.png"))

    # startup gate: the splash again, small
    save8(fit(Image.open(bg_path).convert("RGB").filter(ImageFilter.SHARPEN), 280, 158),
          os.path.join(OUT, "livearea", "contents", "startup.png"))

    with open(os.path.join(OUT, "livearea", "contents", "template.xml"), "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                '<livearea style="a1" format-ver="01.00" content-rev="1">\n'
                '  <livearea-background>\n    <image>bg.png</image>\n  </livearea-background>\n'
                '  <gate>\n    <startup-image>startup.png</startup-image>\n  </gate>\n'
                '</livearea>\n')
    print("wrote", os.path.relpath(os.path.join(OUT, "livearea", "contents", "template.xml")))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
