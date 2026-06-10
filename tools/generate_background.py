"""Generate CUE SAMPLER background: orange/white gradient with soft cloud washes,
matching the cuesampler.com homepage look."""
import numpy as np
from PIL import Image

W, H = 2876, 1768  # 2x editor size (1438x884)
rng = np.random.default_rng(7)

yy, xx = np.mgrid[0:H, 0:W].astype(np.float64)
u = xx / W   # 0..1 left->right
v = yy / H   # 0..1 top->bottom

def lerp(a, b, t):
    return a + (b - a) * t

# ---- base vertical gradient (color stops sampled from the website) ----
stops = [
    (0.00, (238,  82,  44)),   # saturated coral orange, top
    (0.45, (241, 100,  64)),   # main coral
    (0.80, (245, 132,  98)),   # softer coral
    (1.00, (247, 162, 130)),   # warm salmon, bottom
]
base = np.zeros((H, W, 3))
for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
    mask = (v >= p0) & (v <= p1)
    t = np.clip((v - p0) / (p1 - p0), 0, 1)
    t = t * t * (3 - 2 * t)  # smoothstep between stops
    for ch in range(3):
        seg = lerp(c0[ch], c1[ch], t)
        base[..., ch] = np.where(mask, seg, base[..., ch])

# ---- cloud layer 1: smooth value-noise field ----
def smooth_noise(gw, gh, size):
    g = rng.random((gh, gw))
    img = Image.fromarray((g * 255).astype(np.uint8))
    img = img.resize(size, Image.BICUBIC)
    return np.asarray(img).astype(np.float64) / 255.0

noise = (smooth_noise(9, 5, (W, H)) * 0.6
         + smooth_noise(17, 9, (W, H)) * 0.3
         + smooth_noise(33, 17, (W, H)) * 0.1)
noise = np.clip((noise - 0.45) / 0.55, 0, 1) ** 1.6

# ---- cloud layer 2: hand-placed soft blobs (echoing the site's washes) ----
def blob(cx, cy, sx, sy, amp, rot=0.0):
    dx, dy = xx - cx * W, yy - cy * H
    if rot:
        c, s = np.cos(rot), np.sin(rot)
        dx, dy = dx * c - dy * s, dx * s + dy * c
    return amp * np.exp(-((dx / (sx * W)) ** 2 + (dy / (sy * H)) ** 2))

blobs = (
    blob(0.06, 0.10, 0.26, 0.50, 0.26)          # top-left wash
    + blob(0.88, 0.38, 0.28, 0.55, 0.24, 0.4)   # right-center wash
    + blob(0.35, 1.00, 0.42, 0.42, 0.26)        # bottom-center glow
    + blob(0.72, 1.08, 0.46, 0.40, 0.20)        # bottom-right glow
    + blob(0.45, 0.30, 0.20, 0.35, 0.10, -0.5)  # faint mid accent
)

# whiteness ramp: clouds read a little stronger toward the bottom, like the site
ramp = 0.45 + 0.45 * v
alpha = np.clip(noise * 0.26 * ramp + blobs * ramp, 0, 0.48)

white = np.array([255.0, 253.0, 250.0])
out = base * (1 - alpha[..., None]) + white * alpha[..., None]

# ---- fine grain to prevent gradient banding ----
out += rng.normal(0, 1.1, (H, W, 1))

out = np.clip(out, 0, 255).astype(np.uint8)
img = Image.fromarray(out)
img.save("/Users/jerryvolpe/Documents/SAMPLERv3/assets/cue_background.png", optimize=True)

# Pre-blurred copy for the frosted-glass panels: low-res + heavy gaussian blur.
# Panels draw a slice of this behind their white tint to fake backdrop blur;
# the upscale at draw time adds further softness, so quarter-res is plenty.
from PIL import ImageFilter
blur = img.resize((719, 442), Image.LANCZOS).filter(ImageFilter.GaussianBlur(16))
blur.save("/Users/jerryvolpe/Documents/SAMPLERv3/assets/cue_background_blur.png", optimize=True)

# small preview for inspection
img.resize((1438, 884), Image.LANCZOS).save("/tmp/bg_preview.png")
print("done")
