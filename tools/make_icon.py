"""Generates src/app.ico (a push pin) at all sizes Windows uses.

Usage: python tools/make_icon.py   (requires Pillow)
"""
from pathlib import Path

from PIL import Image, ImageDraw

S = 1024  # drawing canvas; downscaled for each icon size
RED = (229, 57, 53, 255)
RED_DARK = (183, 28, 28, 255)
HIGHLIGHT = (255, 255, 255, 110)
NEEDLE = (120, 130, 140, 255)


def draw_pin() -> Image.Image:
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = S // 2
    # needle
    d.polygon([(cx - 22, 600), (cx + 22, 600), (cx + 4, 960), (cx - 4, 960)], fill=NEEDLE)
    # base plate
    d.rounded_rectangle([cx - 250, 520, cx + 250, 640], radius=60, fill=RED_DARK)
    # body (tapered)
    d.polygon([(cx - 150, 250), (cx + 150, 250), (cx + 200, 560), (cx - 200, 560)], fill=RED)
    # head cap
    d.rounded_rectangle([cx - 210, 90, cx + 210, 280], radius=80, fill=RED)
    d.rounded_rectangle([cx - 210, 230, cx + 210, 290], radius=30, fill=RED_DARK)
    # highlight
    d.rounded_rectangle([cx - 150, 120, cx - 80, 250], radius=35, fill=HIGHLIGHT)
    d.polygon([(cx - 120, 320), (cx - 70, 320), (cx - 90, 520), (cx - 150, 520)], fill=HIGHLIGHT)
    # tilt like a pin stuck into a board
    return img.rotate(-35, resample=Image.BICUBIC, center=(cx, S // 2))


def main() -> None:
    base = draw_pin()
    sizes = [16, 20, 24, 32, 40, 48, 64, 256]
    out = Path(__file__).resolve().parent.parent / "src" / "app.ico"
    base.resize((256, 256), Image.LANCZOS).save(out, sizes=[(s, s) for s in sizes])
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
