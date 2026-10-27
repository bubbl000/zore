"""Generate the ZoreView app icon from the repository's logo.jpg.

Run from anywhere:  python tools/make_icon.py

Outputs
  src/app/zoreview.ico   multi-size icon embedded by src/app/version.rc
  ../../logo.png          512 px transparent master (docs / store listings)

The logo.jpg artwork is a lime mark on a black square; this adds a 22% rounded
corner mask so the icon reads as an app tile on light and dark taskbars.
"""
from PIL import Image, ImageDraw
import os

here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.dirname(here)                 # .../new
src_path = os.path.join(repo, "assets", "logo.jpg")
if not os.path.exists(src_path):
    # Development checkout: the source artwork also lives one level above.
    src_path = os.path.join(os.path.dirname(repo), "logo.jpg")
if not os.path.exists(src_path):
    raise SystemExit("assets/logo.jpg not found")

src = Image.open(src_path).convert("RGB")
# 4x upsample keeps the diagonal edges clean when the ICO writer downsizes.
big = src.resize((1024, 1024), Image.LANCZOS)

mask = Image.new("L", (1024, 1024), 0)
ImageDraw.Draw(mask).rounded_rectangle([0, 0, 1023, 1023],
                                       radius=int(1024 * 0.22), fill=255)
master = big.convert("RGBA")
master.putalpha(mask)

master.resize((512, 512), Image.LANCZOS).save(os.path.join(repo, "assets", "logo.png"))
ico_path = os.path.join(repo, "src", "app", "zoreview.ico")
master.save(ico_path, format="ICO",
            sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48),
                   (64, 64), (96, 96), (128, 128), (256, 256)])
logo_png = os.path.join(repo, "src", "app", "app_logo.png")
master.resize((64, 64), Image.LANCZOS).save(logo_png)
print("wrote", ico_path, os.path.getsize(ico_path), "bytes")
print("wrote", os.path.join(repo, "assets", "logo.png"))
print("wrote", logo_png, os.path.getsize(logo_png), "bytes")
