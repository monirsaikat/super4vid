"""Generate the placeholder app icon: assets/icon.ico (16/32/48/256 px).

A rounded square in the accent color #4F8CFF with a white circle. Requires Pillow.
Usage: python tools/make_icon.py
"""
from pathlib import Path

from PIL import Image, ImageDraw

ACCENT = (0x4F, 0x8C, 0xFF, 255)
WHITE = (255, 255, 255, 255)
SIZES = [16, 32, 48, 256]
SUPERSAMPLE = 4


def render(size: int) -> Image.Image:
    # Draw large and downsample so the edges are antialiased at every size.
    big = size * SUPERSAMPLE
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    margin = big // 16
    d.rounded_rectangle([margin, margin, big - margin - 1, big - margin - 1], radius=big // 5, fill=ACCENT)
    r = big * 0.22
    c = big / 2
    d.ellipse([c - r, c - r, c + r, c + r], fill=WHITE)
    return img.resize((size, size), Image.LANCZOS)


def main() -> None:
    out = Path(__file__).resolve().parent.parent / "assets" / "icon.ico"
    out.parent.mkdir(parents=True, exist_ok=True)
    images = [render(s) for s in SIZES]
    # Pillow writes one entry per size; it resamples from the largest image, so pass ours explicitly.
    images[-1].save(out, format="ICO", sizes=[(s, s) for s in SIZES], append_images=images[:-1])
    print(f"wrote {out} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
