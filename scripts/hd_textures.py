"""Build an HD texture pack from your own copy of the game.

python scripts/hd_textures.py <dump dir> <pack dir> [--scale 2] [--max N]

The textures come from the game you own (dump them by playing with
RBREW_TEXTURE_DUMP=<dump dir>, see HD-Textures.bat / HD-Textures.sh); the results
stay on your PC in Game/textures. They are derived from the game's files, so they are never
uploaded or committed - everyone builds their own pack with this script.

- Colour textures (BC1/BC2/BC3) are upscaled 4x by Real-ESRGAN (x4plus), resized
  to --scale and written as RGBA8 with a full mip chain.
- Grey-scale masks the game tints itself (BC4: loading screen camo, dots, icons)
  get sharp, smooth edges: each grey level is upscaled as its own region and
  re-sharpened, then encoded as BC4 again.
- Normal maps, data maps and flat textures are left alone.

Needs Python 3 with torch, numpy and pillow (the script offers to install
them). An NVIDIA graphics card with CUDA makes it many times faster than the
CPU (about 4 minutes per 1024x1024 texture on the CPU).
"""
import argparse
import hashlib
import io
import struct
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MODEL_URL = 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth'
MODEL_SHA256 = '4fa0d38905f75ac06eb49a7951b426670021be3018265fd191d2125df9d682f1'
MODEL_PATH = ROOT / 'build' / 'upscale' / 'models' / 'RealESRGAN_x4plus.pth'


def ensure_packages():
    missing = []
    # Compatible ranges, not 'latest': a new major version cannot slip in unnoticed.
    wanted = {'numpy': 'numpy>=2,<3', 'PIL': 'pillow>=10,<13', 'torch': 'torch>=2.2,<3'}
    for name in ('numpy', 'PIL', 'torch'):
        try:
            __import__(name)
        except ImportError:
            missing.append(wanted[name])
    if not missing:
        return
    print('Missing Python packages:', ' '.join(missing))
    answer = input('Install them now with pip (internet, about 200 MB-2 GB)? [y/N] ').strip().lower()
    if answer not in ('y', 'yes', 'j', 'ja'):
        sys.exit('Install them with:  python -m pip install ' + ' '.join(missing))
    # --only-binary: ready-made wheels only; a source package would run its build script on this PC.
    subprocess.check_call([sys.executable, '-m', 'pip', 'install', '--only-binary=:all:', *missing])


ensure_packages()
import numpy as np                     # noqa: E402
import torch                           # noqa: E402
from torch import nn                   # noqa: E402
from torch.nn import functional as F   # noqa: E402
from PIL import Image                  # noqa: E402

# --- Real-ESRGAN x4plus network (RRDBNet, same parameter names as basicsr) ---

class ResidualDenseBlock(nn.Module):
    def __init__(self, nf=64, gc=32):
        super().__init__()
        self.conv1 = nn.Conv2d(nf, gc, 3, 1, 1)
        self.conv2 = nn.Conv2d(nf + gc, gc, 3, 1, 1)
        self.conv3 = nn.Conv2d(nf + 2 * gc, gc, 3, 1, 1)
        self.conv4 = nn.Conv2d(nf + 3 * gc, gc, 3, 1, 1)
        self.conv5 = nn.Conv2d(nf + 4 * gc, nf, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(0.2, inplace=True)

    def forward(self, x):
        x1 = self.lrelu(self.conv1(x))
        x2 = self.lrelu(self.conv2(torch.cat((x, x1), 1)))
        x3 = self.lrelu(self.conv3(torch.cat((x, x1, x2), 1)))
        x4 = self.lrelu(self.conv4(torch.cat((x, x1, x2, x3), 1)))
        x5 = self.conv5(torch.cat((x, x1, x2, x3, x4), 1))
        return x5 * 0.2 + x


class RRDB(nn.Module):
    def __init__(self, nf, gc=32):
        super().__init__()
        self.rdb1, self.rdb2, self.rdb3 = ResidualDenseBlock(nf, gc), ResidualDenseBlock(nf, gc), ResidualDenseBlock(nf, gc)

    def forward(self, x):
        return self.rdb3(self.rdb2(self.rdb1(x))) * 0.2 + x


class RRDBNet(nn.Module):
    def __init__(self, nf=64, blocks=23, gc=32):
        super().__init__()
        self.conv_first = nn.Conv2d(3, nf, 3, 1, 1)
        self.body = nn.Sequential(*[RRDB(nf, gc) for _ in range(blocks)])
        self.conv_body = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_up1 = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_up2 = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_hr = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_last = nn.Conv2d(nf, 3, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(0.2, inplace=True)

    def forward(self, x):
        feat = self.conv_first(x)
        feat = feat + self.conv_body(self.body(feat))
        feat = self.lrelu(self.conv_up1(F.interpolate(feat, scale_factor=2, mode='nearest')))
        feat = self.lrelu(self.conv_up2(F.interpolate(feat, scale_factor=2, mode='nearest')))
        return self.conv_last(self.lrelu(self.conv_hr(feat)))


def load_model(device):
    if not MODEL_PATH.exists():
        print(f'Downloading the Real-ESRGAN model (~64 MB) from {MODEL_URL}')
        MODEL_PATH.parent.mkdir(parents=True, exist_ok=True)
        part = MODEL_PATH.with_suffix('.part')
        urllib.request.urlretrieve(MODEL_URL, part)
        digest = hashlib.sha256(part.read_bytes()).hexdigest()
        if digest != MODEL_SHA256:
            part.unlink()
            sys.exit(f'The downloaded model has the wrong checksum ({digest}); not used.')
        part.rename(MODEL_PATH)
    net = RRDBNet()
    state = torch.load(MODEL_PATH, map_location='cpu', weights_only=True)
    net.load_state_dict(state.get('params_ema', state.get('params', state)), strict=True)
    net.eval().to(device)
    if device.type == 'cuda':
        net.half()
    return net


def upscale4(net, device, rgb, tile=256, pad=12):
    """4x in tiles (bounded memory)."""
    x = torch.from_numpy(np.asarray(rgb, np.float32).transpose(2, 0, 1) / 255).unsqueeze(0)
    _, _, h, w = x.shape
    out = torch.zeros(1, 3, h * 4, w * 4)
    dtype = torch.float16 if device.type == 'cuda' else torch.float32
    with torch.no_grad():
        for y0 in range(0, h, tile):
            for x0 in range(0, w, tile):
                ya, yb = max(y0 - pad, 0), min(y0 + tile + pad, h)
                xa, xb = max(x0 - pad, 0), min(x0 + tile + pad, w)
                r = net(x[:, :, ya:yb, xa:xb].to(device, dtype)).float().cpu()
                cy, cx = (y0 - ya) * 4, (x0 - xa) * 4
                th, tw = min(tile, h - y0) * 4, min(tile, w - x0) * 4
                out[:, :, y0 * 4:y0 * 4 + th, x0 * 4:x0 * 4 + tw] = r[:, :, cy:cy + th, cx:cx + tw]
    return Image.fromarray((out.squeeze(0).clamp(0, 1).numpy().transpose(1, 2, 0) * 255 + 0.5).astype(np.uint8))


# --- DDS reading/writing ---

COLOUR = {71: 28, 72: 29, 74: 28, 75: 29, 77: 28, 78: 29}   # BC format -> RGBA8 (same sRGB-ness)
UNORM = {72: 71, 75: 74, 78: 77}                             # PIL reads only the UNORM codes


def dxgi_of(path):
    data = path.read_bytes()
    return struct.unpack_from('<I', data, 128)[0] if len(data) > 148 and data[84:88] == b'DX10' else None


def read_image(path):
    data = bytearray(path.read_bytes())
    dx = struct.unpack_from('<I', data, 128)[0]
    if dx in UNORM:
        struct.pack_into('<I', data, 128, UNORM[dx])
    image = Image.open(io.BytesIO(bytes(data)))
    image.load()
    return image


def kind(rgba):
    a = np.asarray(rgba.resize((64, 64)), np.float32) / 255
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    if abs(r.mean() - 0.5) < 0.12 and abs(g.mean() - 0.5) < 0.12 and b.mean() > 0.7:
        return 'normal map'
    if b.mean() < 0.05 and (r.std() > 0.15 or g.std() > 0.15):
        return 'data map'
    if a[..., :3].std() < 0.015:
        return 'flat'
    return 'colour'


def dds_header(w, h, levels, linear_size, block):
    header = [0] * 31
    header[0] = 124
    header[1] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | (0x80000 if block else 0x8)
    header[2], header[3], header[4] = h, w, linear_size
    header[6] = levels
    header[18], header[19] = 32, 0x4
    header[20] = struct.unpack('<I', b'DX10')[0]
    header[26] = 0x1000 | 0x8 | 0x400000
    return b'DDS ' + struct.pack('<31I', *header)


def write_rgba(path, levels, dx):
    w, h = levels[0].size
    with open(path, 'wb') as f:
        f.write(dds_header(w, h, len(levels), w * 4, False))
        f.write(struct.pack('<5I', dx, 3, 0, 1, 0))
        for level in levels:
            f.write(level.tobytes())


def encode_bc4(level):
    h, w = level.shape
    ph, pw = (h + 3) // 4 * 4, (w + 3) // 4 * 4
    v = np.pad(np.round(level * 255.0).astype(np.int32), ((0, ph - h), (0, pw - w)), mode='edge')
    blocks = v.reshape(ph // 4, 4, pw // 4, 4).transpose(0, 2, 1, 3).reshape(-1, 16)
    r0, r1 = blocks.max(axis=1), blocks.min(axis=1)
    palette = np.stack([r0, r1] + [((7 - k) * r0 + k * r1) // 7 for k in range(1, 7)], axis=1)
    index = np.abs(blocks[:, :, None] - palette[:, None, :]).argmin(axis=2).astype(np.uint64)
    index[r0 == r1] = 0
    bits = np.zeros(len(blocks), dtype=np.uint64)
    for i in range(16):
        bits |= index[:, i] << np.uint64(3 * i)
    out = np.zeros((len(blocks), 8), dtype=np.uint8)
    out[:, 0], out[:, 1] = r0, r1
    for b in range(6):
        out[:, 2 + b] = (bits >> np.uint64(8 * b)) & np.uint64(0xFF)
    return out.tobytes()


def write_bc4(path, levels):
    h, w = levels[0].shape
    payload = [encode_bc4(level) for level in levels]
    with open(path, 'wb') as f:
        f.write(dds_header(w, h, len(levels), len(payload[0]), True))
        f.write(struct.pack('<5I', 80, 3, 0, 1, 0))
        for data in payload:
            f.write(data)


# --- the two kinds of texture ---

def mask_levels(mask):
    hist, _ = np.histogram(mask, bins=32, range=(0.0, 1.0))
    peaks = [i for i in range(32) if hist[i] > mask.size * 0.03 and hist[i] >= hist[max(i - 1, 0)] and hist[i] >= hist[min(i + 1, 31)]]
    centres = sorted({(i + 0.5) / 32 for i in peaks} | {0.0, 1.0}) if peaks else [0.0, 1.0]
    merged = [centres[0]]
    for c in centres[1:]:
        if c - merged[-1] > 0.08:
            merged.append(c)
    return merged


def is_flat_mask(mask):
    """A shape mask: most pixels sit on a few grey levels (not a gradient or noise)."""
    hist, _ = np.histogram(mask, bins=32, range=(0.0, 1.0))
    return np.sort(hist)[-4:].sum() > 0.85 * mask.size and mask.std() > 0.05


def hd_mask(mask, scale):
    h, w = mask.shape
    size = (w * scale, h * scale)
    centres = mask_levels(mask)
    v = np.clip(mask, centres[0], centres[-1])
    shares = np.zeros((len(centres),) + mask.shape, dtype=np.float32)
    for i, (lo, hi) in enumerate(zip(centres, centres[1:])):
        inside = (v >= lo) & (v <= hi)
        t = (v - lo) / (hi - lo)
        shares[i] += np.where(inside, 1.0 - t, 0.0)
        shares[i + 1] += np.where(inside, t, 0.0)
    shares /= np.maximum(shares.sum(axis=0), 1e-6)
    weights = []
    for k in range(len(centres)):
        soft = np.clip(np.asarray(Image.fromarray(shares[k]).resize(size, Image.BICUBIC), dtype=np.float32), 0.0, 1.0)
        t = np.clip((soft - 0.5) * scale + 0.5, 0.0, 1.0)
        weights.append(t * t * (3.0 - 2.0 * t) + 1e-4)
    weights = np.stack(weights)
    hd = np.clip((weights * np.array(centres)[:, None, None]).sum(axis=0) / weights.sum(axis=0), 0.0, 1.0).astype(np.float32)
    levels = [hd]
    while max(levels[-1].shape) > 1:
        lh, lw = max(1, levels[-1].shape[0] // 2), max(1, levels[-1].shape[1] // 2)
        levels.append(np.asarray(Image.fromarray(levels[-1]).resize((lw, lh), Image.BOX), dtype=np.float32))
    return levels


def hd_colour(net, device, rgba, scale):
    w, h = rgba.size
    big = upscale4(net, device, rgba.convert('RGB'))
    size = (w * scale, h * scale)
    rgb = big.resize(size, Image.LANCZOS) if scale != 4 else big
    level = Image.merge('RGBA', (*rgb.split(), rgba.getchannel('A').resize(size, Image.BICUBIC)))
    levels = [level]
    while level.size != (1, 1):
        level = level.resize((max(1, level.size[0] // 2), max(1, level.size[1] // 2)), Image.LANCZOS)
        levels.append(level)
    return levels


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump', type=Path)
    ap.add_argument('pack', type=Path)
    ap.add_argument('--scale', type=int, default=2, choices=(2, 4))
    ap.add_argument('--max', type=int, default=0, help='at most this many colour textures (0 = all)')
    ap.add_argument('--min', type=int, default=64, help='smallest side to upscale')
    args = ap.parse_args()
    files = sorted(args.dump.glob('*.dds'), key=lambda p: p.stat().st_size, reverse=True)
    if not files:
        sys.exit(f'No dumped textures in {args.dump}. Play the game with RBREW_TEXTURE_DUMP set first (HD-Textures.bat / HD-Textures.sh).')
    args.pack.mkdir(parents=True, exist_ok=True)
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f'{len(files)} dumped textures; upscaling on the {"graphics card (CUDA)" if device.type == "cuda" else "CPU (slow)"}')
    net = None
    done = masks = skipped = 0
    started = time.time()
    for n, path in enumerate(files, 1):
        target = args.pack / path.name
        if target.exists():
            continue
        dx = dxgi_of(path)
        try:
            if dx == 80:   # grey-scale mask
                mask = np.asarray(read_image(path).convert('L'), dtype=np.float32) / 255.0
                if min(mask.shape) < 32 or not is_flat_mask(mask):
                    skipped += 1; continue
                write_bc4(target, hd_mask(mask, 4))
                masks += 1
                continue
            if dx not in COLOUR or (args.max and done >= args.max):
                skipped += 1; continue
            rgba = read_image(path).convert('RGBA')
            if min(rgba.size) < args.min or kind(rgba) != 'colour':
                skipped += 1; continue
            if net is None:
                net = load_model(device)
            t0 = time.time()
            write_rgba(target, hd_colour(net, device, rgba, args.scale), COLOUR[dx])
            done += 1
            print(f'[{n}/{len(files)}] {path.name}: {rgba.size[0]}x{rgba.size[1]} -> x{args.scale} in {time.time() - t0:.0f} s')
        except Exception as error:   # one bad file must not stop the pack
            print(f'{path.name}: skipped ({error})')
            skipped += 1
    print(f'Done in {(time.time() - started) / 60:.1f} min: {done} colour textures, {masks} masks, {skipped} skipped -> {args.pack}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
