"""Convert sim/out/*.ppm to PNG and build a contact sheet (needs Pillow)."""
import glob, os, sys
from PIL import Image
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "out")
files = sorted(glob.glob(os.path.join(out, "*.ppm")))
imgs = []
for f in files:
    im = Image.open(f)
    im.save(f[:-4] + ".png")
    imgs.append((os.path.basename(f)[:-4], im))
print(len(imgs), "frames")
