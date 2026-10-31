"""Images for the gold service (CI-3, ADR-0135 §17), in the standard library only: PNG decoding and encoding, the
content digest, LDR-FLIP and SSIM.

- PNG: 8-bit greyscale, greyscale+alpha, RGB and RGBA, every filter, non-interlaced; anything else (palette, 16-bit,
  Adam7) is refused with the reason, never guessed. Pixels are RGBA8 rows.
- Digest: SHA-256 of `cerid-image/1`, the size and the RGBA8 pixels, so two encodings of the same picture (another
  compressor, another filter choice) are the same image.
- LDR-FLIP (Andersson et al., "FLIP: A Difference Evaluator for Alternating Images", HPG 2020), the algorithm of the
  reference implementation (NVlabs/flip, BSD-3-Clause) written again here: YCxCz with CSF filters (sums of
  Gaussians, edge-replicated borders), Hunt-adjusted CIELAB, HyAB distance with the paper's compression (qc 0.7,
  pc 0.4, pt 0.95), the edge and point feature difference (w 0.082, qf 0.5), at the declared pixels per degree.
  Every Gaussian here is separable, so a pass is one dimension at a time.
- SSIM (Wang et al. 2004) on luma with the 11-tap Gaussian window, sigma 1.5, K1 0.01, K2 0.03.

The perceptual metric the engine owns is IMG-ORACLE's (HDR-FLIP and a ColorVideoVDP-class metric in crd-hesap); when
it lands, this module is cross-checked against it, and the gold service may defer to it. The cross-check against
the reference implementation is a test that runs when CERID_FLIP_REFERENCE names a directory holding the reference
tool's output for a pair (scripts/test-ci-orchestrator.py, GoldImages); fetching that tool is a user-approved
download.
"""
from __future__ import annotations

import hashlib
import math
import struct
import zlib
from operator import mul

PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'
DEFAULT_PPD = 67.0  # FLIP's default: a 0.7 m wide 4K display seen from 0.7 m


class ImageError(ValueError):
    """An image that cannot be read: the reason says what is wrong."""


class Image:
    """width x height RGBA8 pixels, rows top to bottom."""

    def __init__(self, width, height, rgba):
        if len(rgba) != width * height * 4:
            raise ImageError('%dx%d needs %d bytes, got %d' % (width, height, width * height * 4, len(rgba)))
        self.width = width
        self.height = height
        self.rgba = bytes(rgba)

    def digest(self):
        return hashlib.sha256(b'cerid-image/1\0' + struct.pack('<II', self.width, self.height) + self.rgba).hexdigest()

    def channel(self, index):
        """One channel as floats in [0, 1]."""
        return [v / 255.0 for v in self.rgba[index::4]]


# --------------------------------------------------------------------------------------------- PNG

def decode_png(data, max_pixels=64 * 1024 * 1024):
    if not data.startswith(PNG_SIGNATURE):
        raise ImageError('not a PNG file')
    offset = len(PNG_SIGNATURE)
    header = None
    idat = []
    while offset + 8 <= len(data):
        length, kind = struct.unpack_from('>I4s', data, offset)
        body = data[offset + 8:offset + 8 + length]
        if len(body) != length or offset + 12 + length > len(data):
            raise ImageError('truncated PNG chunk %r' % kind)
        crc = struct.unpack_from('>I', data, offset + 8 + length)[0]
        if zlib.crc32(kind + body) != crc:
            raise ImageError('PNG chunk %r fails its CRC' % kind)
        offset += 12 + length
        if kind == b'IHDR':
            header = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat.append(body)
        elif kind == b'IEND':
            break
    if header is None:
        raise ImageError('PNG without IHDR')
    width, height, depth, colour, _, _, interlace = header
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(colour)
    if channels is None:
        raise ImageError('PNG colour type %d (palette) is not supported: write RGB or RGBA' % colour)
    if depth != 8:
        raise ImageError('PNG bit depth %d is not supported: write 8 bits per channel' % depth)
    if interlace:
        raise ImageError('interlaced PNG is not supported')
    if not width or not height or width * height > max_pixels:
        raise ImageError('PNG size %dx%d is out of range' % (width, height))
    try:
        raw = zlib.decompress(b''.join(idat))
    except zlib.error as error:
        raise ImageError('PNG data does not inflate: %s' % error) from None
    stride = width * channels
    if len(raw) != height * (stride + 1):
        raise ImageError('PNG data is %d bytes, expected %d' % (len(raw), height * (stride + 1)))
    rows = []
    previous = bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        unfilter(kind, line, previous, channels)
        rows.append(line)
        previous = line
    pixels = bytearray()
    for line in rows:
        if channels == 4:
            pixels += line
        elif channels == 3:
            out = bytearray(width * 4)
            out[0::4], out[1::4], out[2::4] = line[0::3], line[1::3], line[2::3]
            out[3::4] = b'\xff' * width
            pixels += out
        elif channels == 2:
            out = bytearray(width * 4)
            out[0::4] = out[1::4] = out[2::4] = line[0::2]
            out[3::4] = line[1::2]
            pixels += out
        else:
            out = bytearray(width * 4)
            out[0::4] = out[1::4] = out[2::4] = line
            out[3::4] = b'\xff' * width
            pixels += out
    return Image(width, height, pixels)


def unfilter(kind, line, previous, bpp):
    if kind == 0:
        return
    if kind == 1:
        for i in range(bpp, len(line)):
            line[i] = (line[i] + line[i - bpp]) & 0xFF
    elif kind == 2:
        for i in range(len(line)):
            line[i] = (line[i] + previous[i]) & 0xFF
    elif kind == 3:
        for i in range(len(line)):
            left = line[i - bpp] if i >= bpp else 0
            line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
    elif kind == 4:
        for i in range(len(line)):
            a = line[i - bpp] if i >= bpp else 0
            b = previous[i]
            c = previous[i - bpp] if i >= bpp else 0
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
    else:
        raise ImageError('PNG filter type %d is unknown' % kind)


def encode_png(image):
    """An RGBA8 PNG (filter 0 rows, zlib level 6)."""
    stride = image.width * 4
    raw = b''.join(b'\0' + image.rgba[y * stride:(y + 1) * stride] for y in range(image.height))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    return (PNG_SIGNATURE + chunk(b'IHDR', struct.pack('>IIBBBBB', image.width, image.height, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def downsample(image, factor):
    """Box-filtered by an integer factor (sizes rounded down; the remainder rows and columns are dropped)."""
    if factor <= 1:
        return image
    width, height = image.width // factor, image.height // factor
    if not width or not height:
        raise ImageError('%dx%d is too small to downsample by %d' % (image.width, image.height, factor))
    src = image.rgba
    out = bytearray(width * height * 4)
    area = factor * factor
    for y in range(height):
        for x in range(width):
            for c in range(4):
                total = 0
                for dy in range(factor):
                    row = ((y * factor + dy) * image.width + x * factor) * 4 + c
                    total += sum(src[row:row + factor * 4:4])
                out[(y * width + x) * 4 + c] = (total + area // 2) // area
    return Image(width, height, out)


# --------------------------------------------------------------------------------------------- convolution

def convolve_rows(plane, width, height, kernel):
    r = len(kernel) // 2
    out = [0.0] * (width * height)
    for y in range(height):
        row = plane[y * width:(y + 1) * width]
        padded = [row[0]] * r + row + [row[-1]] * r
        out[y * width:(y + 1) * width] = [sum(map(mul, kernel, padded[x:x + 2 * r + 1])) for x in range(width)]
    return out


def convolve_columns(plane, width, height, kernel):
    transposed = [plane[y * width + x] for x in range(width) for y in range(height)]
    done = convolve_rows(transposed, height, width, kernel)
    return [done[x * height + y] for y in range(height) for x in range(width)]


def separable(plane, width, height, along_x, along_y):
    return convolve_columns(convolve_rows(plane, width, height, along_x), width, height, along_y)


# --------------------------------------------------------------------------------------------- colour

RGB_TO_XYZ = ((10135552 / 24577794, 8788810 / 24577794, 4435075 / 24577794),
              (2613072 / 12288897, 8788810 / 12288897, 887015 / 12288897),
              (1425312 / 73733382, 8788810 / 73733382, 70074185 / 73733382))


def invert3(m):
    (a, b, c), (d, e, f), (g, h, i) = m
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return (((e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det),
            ((f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det),
            ((d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det))


XYZ_TO_RGB = invert3(RGB_TO_XYZ)
WHITE = tuple(sum(row) for row in RGB_TO_XYZ)  # D65: linear RGB (1, 1, 1)
SRGB_TO_LINEAR = [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in (v / 255.0 for v in range(256))]


def ycxcz_planes(image):
    """(Y, Cx, Cz) planes of an sRGB image (alpha ignored)."""
    lin = SRGB_TO_LINEAR
    rgba = image.rgba
    (m00, m01, m02), (m10, m11, m12), (m20, m21, m22) = RGB_TO_XYZ
    xn, yn, zn = WHITE
    ys, cxs, czs = [], [], []
    for i in range(0, len(rgba), 4):
        r, g, b = lin[rgba[i]], lin[rgba[i + 1]], lin[rgba[i + 2]]
        x = (m00 * r + m01 * g + m02 * b) / xn
        y = (m10 * r + m11 * g + m12 * b) / yn
        z = (m20 * r + m21 * g + m22 * b) / zn
        ys.append(116.0 * y - 16.0)
        cxs.append(500.0 * (x - y))
        czs.append(200.0 * (y - z))
    return ys, cxs, czs


def lab_f(t):
    return t ** (1.0 / 3.0) if t > 0.008856451679035631 else t / 0.12841854934601665 + 4.0 / 29.0


def hunt_lab(r, g, b):
    """Hunt-adjusted CIELAB of linear RGB (clamped to [0, 1])."""
    r, g, b = min(max(r, 0.0), 1.0), min(max(g, 0.0), 1.0), min(max(b, 0.0), 1.0)
    (m00, m01, m02), (m10, m11, m12), (m20, m21, m22) = RGB_TO_XYZ
    xn, yn, zn = WHITE
    fx = lab_f((m00 * r + m01 * g + m02 * b) / xn)
    fy = lab_f((m10 * r + m11 * g + m12 * b) / yn)
    fz = lab_f((m20 * r + m21 * g + m22 * b) / zn)
    lightness = 116.0 * fy - 16.0
    return lightness, 0.01 * lightness * 500.0 * (fx - fy), 0.01 * lightness * 200.0 * (fy - fz)


def hyab(p, q):
    return abs(p[0] - q[0]) + math.hypot(p[1] - q[1], p[2] - q[2])


QC, PC, PT, QF, FEATURE_W = 0.7, 0.4, 0.95, 0.5, 0.082
CMAX = hyab(hunt_lab(0.0, 1.0, 0.0), hunt_lab(0.0, 0.0, 1.0)) ** QC
CSF = {'a': ((1.0, 0.0047), (0.0, 1e-5)), 'rg': ((1.0, 0.0053), (0.0, 1e-5)), 'by': ((34.1, 0.04), (13.5, 0.025))}


def csf_filter(planes, width, height, ppd):
    """The three YCxCz planes filtered by FLIP's contrast sensitivity functions."""
    radius = int(math.ceil(3.0 * math.sqrt(0.04 / (2.0 * math.pi ** 2)) * ppd))
    delta = 1.0 / ppd
    out = []
    for plane, name in zip(planes, ('a', 'rg', 'by')):
        terms = []
        total = 0.0
        for a, b in CSF[name]:
            if a == 0.0:
                continue
            g = [math.exp(-math.pi ** 2 * (i * delta) ** 2 / b) for i in range(-radius, radius + 1)]
            weight = a * math.sqrt(math.pi / b)
            terms.append((weight, g))
            total += weight * sum(g) ** 2
        result = None
        for weight, g in terms:
            filtered = separable(plane, width, height, g, g)
            scale = weight / total
            result = [scale * v for v in filtered] if result is None else [
                acc + scale * v for acc, v in zip(result, filtered)]
        out.append(result)
    return out


def feature_kernels(ppd):
    sd = 0.5 * FEATURE_W * ppd
    radius = int(math.ceil(3.0 * sd))
    xs = range(-radius, radius + 1)
    g = [math.exp(-(x * x) / (2.0 * sd * sd)) for x in xs]
    smooth = [v / sum(g) for v in g]

    def balanced(values):
        positive = sum(v for v in values if v > 0)
        negative = -sum(v for v in values if v < 0)
        return [v / positive if v > 0 else v / negative if v < 0 else 0.0 for v in values]
    edge = balanced([-x * v for x, v in zip(xs, g)])
    point = balanced([(x * x / (sd * sd) - 1.0) * v for x, v in zip(xs, g)])
    return smooth, edge, point


def features(luma, width, height, ppd):
    """(edge magnitude, point magnitude) planes of the normalised achromatic channel."""
    smooth, edge, point = feature_kernels(ppd)
    along_x_smooth = convolve_rows(luma, width, height, smooth)
    along_x_edge = convolve_rows(luma, width, height, edge)
    along_x_point = convolve_rows(luma, width, height, point)
    edge_x = convolve_columns(along_x_edge, width, height, smooth)
    point_x = convolve_columns(along_x_point, width, height, smooth)
    edge_y = convolve_columns(along_x_smooth, width, height, edge)
    point_y = convolve_columns(along_x_smooth, width, height, point)
    return ([math.hypot(a, b) for a, b in zip(edge_x, edge_y)], [math.hypot(a, b) for a, b in zip(point_x, point_y)])


def flip(reference, test, ppd=DEFAULT_PPD):
    """The LDR-FLIP error map (floats in [0, 1], row-major) of two same-size sRGB images."""
    if (reference.width, reference.height) != (test.width, test.height):
        raise ImageError('the images differ in size: %dx%d and %dx%d' % (
            reference.width, reference.height, test.width, test.height))
    width, height = reference.width, reference.height
    colour = []
    luma = []
    for image in (reference, test):
        planes = ycxcz_planes(image)
        luma.append([(y + 16.0) / 116.0 for y in planes[0]])
        filtered = csf_filter(planes, width, height, ppd)
        (n00, n01, n02), (n10, n11, n12), (n20, n21, n22) = XYZ_TO_RGB
        xn, yn, zn = WHITE
        lab = []
        for y_, cx, cz in zip(*filtered):
            y = (y_ + 16.0) / 116.0
            x = cx / 500.0 + y
            z = y - cz / 200.0
            x, y, z = x * xn, y * yn, z * zn
            lab.append(hunt_lab(n00 * x + n01 * y + n02 * z, n10 * x + n11 * y + n12 * z,
                                n20 * x + n21 * y + n22 * z))
        colour.append(lab)
    threshold = PC * CMAX
    colour_error = []
    for p, q in zip(*colour):
        delta = hyab(p, q) ** QC
        colour_error.append(PT / threshold * delta if delta < threshold else
                            PT + (delta - threshold) / (CMAX - threshold) * (1.0 - PT))
    edges_ref, points_ref = features(luma[0], width, height, ppd)
    edges_test, points_test = features(luma[1], width, height, ppd)
    root2 = math.sqrt(2.0)
    out = []
    for i, error in enumerate(colour_error):
        feature = (max(abs(edges_ref[i] - edges_test[i]), abs(points_ref[i] - points_test[i])) / root2) ** QF
        out.append(min(1.0, error ** (1.0 - feature)))
    return out


def ssim(reference, test):
    """Mean SSIM of the luma (BT.601 weights on the stored sRGB values, in [0, 1])."""
    if (reference.width, reference.height) != (test.width, test.height):
        raise ImageError('the images differ in size')
    width, height = reference.width, reference.height

    def luma(image):
        p = image.rgba
        return [(0.299 * p[i] + 0.587 * p[i + 1] + 0.114 * p[i + 2]) / 255.0 for i in range(0, len(p), 4)]
    x, y = luma(reference), luma(test)
    g = [math.exp(-(i * i) / (2 * 1.5 * 1.5)) for i in range(-5, 6)]
    g = [v / sum(g) for v in g]

    def blur(plane):
        return separable(plane, width, height, g, g)
    mx, my = blur(x), blur(y)
    sxx = blur([a * a for a in x])
    syy = blur([b * b for b in y])
    sxy = blur([a * b for a, b in zip(x, y)])
    c1, c2 = 0.01 ** 2, 0.03 ** 2
    total = 0.0
    for i in range(width * height):
        vx, vy, cov = sxx[i] - mx[i] ** 2, syy[i] - my[i] ** 2, sxy[i] - mx[i] * my[i]
        total += ((2 * mx[i] * my[i] + c1) * (2 * cov + c2)) / ((mx[i] ** 2 + my[i] ** 2 + c1) * (vx + vy + c2))
    return total / (width * height)


def summarize(errors, pixel_threshold):
    """FLIP statistics: mean, max, the 99th percentile and the share of pixels above `pixel_threshold`."""
    ordered = sorted(errors)
    count = len(ordered)
    return {'flip_mean': sum(ordered) / count, 'flip_max': ordered[-1],
            'flip_p99': ordered[min(count - 1, int(0.99 * count))],
            'flip_area': sum(1 for e in errors if e > pixel_threshold) / count}


# Magma-like stops (black, purple, red, orange, yellow) for the difference map.
STOPS = ((0.0, (0, 0, 4)), (0.25, (80, 18, 123)), (0.5, (182, 54, 121)), (0.75, (251, 136, 97)),
         (1.0, (252, 253, 191)))


def heatmap(errors, width, height):
    out = bytearray(width * height * 4)
    for i, e in enumerate(errors):
        e = min(max(e, 0.0), 1.0)
        for (t0, c0), (t1, c1) in zip(STOPS, STOPS[1:]):
            if e <= t1:
                f = (e - t0) / (t1 - t0)
                out[i * 4:i * 4 + 3] = bytes(int(round(a + (b - a) * f)) for a, b in zip(c0, c1))
                break
        out[i * 4 + 3] = 255
    return Image(width, height, out)


def ppd_for(screen, kind, viewing):
    """Pixels per degree of a device with a `screen` (density in dpi) at its kind's viewing distance (metres); the
    FLIP default otherwise."""
    distance = (viewing or {}).get(kind)
    dpi = (screen or {}).get('density_dpi')
    if not distance or not dpi:
        return DEFAULT_PPD
    return dpi * (distance / 0.0254) * math.tan(math.pi / 180.0)
