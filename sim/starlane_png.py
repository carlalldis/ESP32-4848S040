"""Convert sim/out/starlane/*.ppm to PNG and build a contact sheet (needs Pillow)."""
import glob
import os
import sys

from PIL import Image

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "out", "starlane")
files = sorted(glob.glob(os.path.join(out, "*.ppm")))
thumbs = []
for f in files:
    im = Image.open(f)
    im.save(f[:-4] + ".png")
    thumbs.append(im.resize((240, 240), Image.NEAREST))
print(len(files), "frames")
if thumbs:
    cols = 6
    rows = (len(thumbs) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * 244, rows * 244), (40, 40, 40))
    for i, t in enumerate(thumbs):
        sheet.paste(t, ((i % cols) * 244 + 2, (i // cols) * 244 + 2))
    sheet.save(os.path.join(out, "contact_sheet.png"))
