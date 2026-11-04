"""Camera Raw CS6 (7.0) joint Temperature + Tint transform -- reference
implementation in NumPy.

Recovered from "Camera Raw.8bi" (see README.md for the exact addresses).
This mirrors cr_temptint.h line by line; it is slower but easier to read.

    from cr_temptint import Transformer
    t = Transformer(temp=-100, tint=50)
    out = t.image(rgb_uint8)        # HxWx3 uint8 in, HxWx3 uint8 out

Sliders are the -100..100 positions of the Camera Raw Temperature / Tint
sliders for a rendered (JPEG/TIFF) file.
"""

import os
import re

import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))


def _load_tables():
    text = open(os.path.join(_HERE, "tone_tables.h"), "r", encoding="utf-8").read()

    def grab(name):
        m = re.search(name + r"\[1025\]\s*=\s*\{(.*?)\};", text, re.S)
        if not m:
            raise RuntimeError("table %s not found" % name)
        vals = re.findall(r"[-+]?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?f?", m.group(1))
        arr = np.array([float(v.rstrip("f")) for v in vals], dtype=np.float64)
        if arr.size != 1025:
            raise RuntimeError("table %s has %d entries" % (name, arr.size))
        return arr

    return grab("CR_TONE_FWD"), grab("CR_TONE_INV")


CR_TONE_FWD, CR_TONE_INV = _load_tables()

# ---------------------------------------------------------------- constants

K_SRGB_TO_PROPHOTO = np.array([
    [0.529299315089, 0.330050760771, 0.140649924280],
    [0.098412856595, 0.873484495895, 0.028102647454],
    [0.016846442812, 0.117682705085, 0.865470851982]])
K_PROPHOTO_TO_SRGB = np.array([
    [2.034289455709, -0.727308148960, -0.306981307112],
    [-0.228924753243, 1.231716583591, -0.002791830246],
    [-0.008469443817, -0.153326000538, 1.161795444488]])
K_PP_TO_XYZ = np.array([
    [0.7976749, 0.1351917, 0.0313534],
    [0.2880402, 0.7118741, 0.0000857],
    [0.0000000, 0.0000000, 0.8252099]])
K_XYZ_TO_PP = np.array([
    [1.345943366936, -0.255607518071, -0.051111838572],
    [-0.544598822462, 1.508167301812, 0.020535108440],
    [0.0, 0.0, 1.211812897543]])
K_BRADFORD = np.array([
    [0.8951, 0.2664, -0.1614],
    [-0.7502, 1.7135, 0.0367],
    [0.0389, -0.0685, 1.0296]])
K_BRADFORD_INV = np.linalg.inv(K_BRADFORD)
K_PCS_XYZ = np.array([0.9642956764, 1.0, 0.8251046025])
K_PCS_XY = np.array([0.3457, 0.3585])

# Robertson isotemperature table, VA 0x50003b90: mired, u, v, t
K_TEMP_TABLE = np.array([
    [0.0, 0.180060, 0.263520, -0.243410],
    [10.0, 0.180660, 0.265890, -0.254790],
    [20.0, 0.181330, 0.268460, -0.268760],
    [30.0, 0.182080, 0.271190, -0.285390],
    [40.0, 0.182930, 0.274070, -0.304700],
    [50.0, 0.183880, 0.277090, -0.326750],
    [60.0, 0.184940, 0.280210, -0.351560],
    [70.0, 0.186110, 0.283420, -0.379150],
    [80.0, 0.187400, 0.286680, -0.409550],
    [90.0, 0.188800, 0.289970, -0.442780],
    [100.0, 0.190320, 0.293260, -0.478880],
    [125.0, 0.194620, 0.301410, -0.582040],
    [150.0, 0.199620, 0.309210, -0.704710],
    [175.0, 0.205250, 0.316470, -0.849010],
    [200.0, 0.211420, 0.323120, -1.018200],
    [225.0, 0.218070, 0.329090, -1.216800],
    [250.0, 0.225110, 0.334390, -1.451200],
    [275.0, 0.232470, 0.339040, -1.729800],
    [300.0, 0.240100, 0.343080, -2.063700],
    [325.0, 0.247020, 0.346550, -2.468100],
    [350.0, 0.255910, 0.349510, -2.964100],
    [375.0, 0.264000, 0.352000, -3.581400],
    [400.0, 0.272180, 0.354070, -4.363300],
    [425.0, 0.280390, 0.355770, -5.376200],
    [450.0, 0.288630, 0.357140, -6.726200],
    [475.0, 0.296850, 0.358230, -8.595500],
    [500.0, 0.305050, 0.359070, -11.324000],
    [525.0, 0.313200, 0.359680, -15.628000],
    [550.0, 0.321290, 0.360110, -23.325000],
    [575.0, 0.329310, 0.360380, -40.770000],
    [600.0, 0.337240, 0.360510, -116.450000]])

K_TINT_ZERO = 9.562965


# ------------------------------------------------------------- slider maps

def kelvin_from_temperature_slider(slider):
    """FUN_4fd82436"""
    u = min(max(float(slider), -100.0), 100.0) / 100.0
    return 50000.0 / ((3.0 * u - 12.0) * u + 10.0)


def tint_from_tint_slider(slider):
    """FUN_4fd824f5"""
    t = min(max(float(slider), -100.0), 100.0) / 100.0
    f = t * 0.5 * (abs(t) + 1.0)
    return K_TINT_ZERO + (150.0 - f * K_TINT_ZERO) * f


def xy_from_temperature(temperature, tint):
    """FUN_4fbc1848 -- dng_temperature::Get_xy_coord()"""
    if temperature < 1.0:
        temperature = 1.0
    mired = 1.0e6 / temperature

    n = K_TEMP_TABLE.shape[0]
    i = 0
    while i != n - 2 and K_TEMP_TABLE[i + 1, 0] <= mired:
        i += 1
    a = K_TEMP_TABLE[i]
    b = K_TEMP_TABLE[i + 1]

    w0 = (b[0] - mired) / (b[0] - a[0])
    w1 = 1.0 - w0
    s0 = np.sqrt(1.0 + a[3] * a[3])
    s1 = np.sqrt(1.0 + b[3] * b[3])
    du = w0 / s0 + w1 / s1
    dv = (a[3] * w0) / s0 + (b[3] * w1) / s1
    ln = np.hypot(du, dv)
    du /= ln
    dv /= ln

    off = tint * (-1.0 / 3000.0)
    u = a[1] * w0 + b[1] * w1 + du * off
    v = a[2] * w0 + b[2] * w1 + dv * off
    den = u - 4.0 * v + 2.0
    return 1.5 * u / den, v / den


def white_xy_from_sliders(temp_slider, tint_slider):
    """FUN_4fd8269e, relative branch (what a JPEG/TIFF uses)"""
    temp_slider = min(max(float(temp_slider), -100.0), 100.0)
    tint_slider = min(max(float(tint_slider), -100.0), 100.0)
    if temp_slider == 0.0 and tint_slider == 0.0:
        return float(K_PCS_XY[0]), float(K_PCS_XY[1])
    return xy_from_temperature(kelvin_from_temperature_slider(temp_slider),
                               tint_from_tint_slider(tint_slider))


# ----------------------------------------------------------- colour spec

def _xy_to_xyz(x, y):
    return np.array([x / y, 1.0, (1.0 - x - y) / y])


def map_white_matrix(x, y):
    """FUN_4fb86134 -- dng_color_spec::MapWhiteMatrix (Bradford)"""
    w1 = _xy_to_xyz(*K_PCS_XY)
    w2 = _xy_to_xyz(x, y)
    c1 = K_BRADFORD @ w1
    c2 = K_BRADFORD @ w2
    d = np.empty(3)
    for i in range(3):
        s = max(c1[i], 0.0)
        t = max(c2[i], 0.0)
        r = t / s if s > 0.0 else 10.0
        d[i] = min(max(r, 0.1), 10.0)
    return K_BRADFORD_INV @ np.diag(d) @ K_BRADFORD


def camera_white(x, y):
    w = K_XYZ_TO_PP @ _xy_to_xyz(x, y)
    mx = w.max()
    return np.clip(w / mx, 0.001, 1.0)


def camera_to_pcs(x, y):
    q = K_XYZ_TO_PP @ map_white_matrix(x, y)
    v = q @ K_PCS_XYZ
    q = q / v.max()
    return np.linalg.inv(q)


K_PP_TO_XYZ = np.linalg.inv(K_XYZ_TO_PP)


def effective_white(x, y):
    """SetWhiteXY clamps fCameraWhite to [1e-3, 1] and builds the PCS<->camera
    matrix from that clamped vector.  Inside the ProPhoto gamut this returns the
    input unchanged; outside it the white lands on the gamut boundary."""
    w = K_XYZ_TO_PP @ _xy_to_xyz(x, y)
    mx = w.max()
    p = np.clip(w / mx, 0.001, 1.0)
    if np.array_equal(p, w / mx):
        return x, y
    w2 = K_PP_TO_XYZ @ p
    s = w2.sum()
    return w2[0] / s, w2[1] / s


def colour_spec(x, y):
    p = camera_white(x, y)
    m4 = K_XYZ_TO_PP @ camera_to_pcs(x, y)
    return p, m4 * p[None, :]


# --------------------------------------------------------------- helpers

def _lround(v):
    """C++ std::lround (round half away from zero)"""
    return np.sign(v) * np.floor(np.abs(v) + 0.5)


def _curve_lut(table):
    """curveFixed() for every signed-16-bit input value."""
    v = np.arange(-32768, 32768, dtype=np.float64)
    n = np.clip((v + 32768.0) / 65535.0, 0.0, 1.0)
    s = n * 1024.0
    i = np.floor(s).astype(np.int64)
    i = np.clip(i, 0, 1023)
    f = s - i
    val = table[i] * (1.0 - f) + table[i + 1] * f
    return _lround(val * 65535.0) - 32768


def _rgb_tone(v, table, lut):
    """RGBTone: hue-preserving, signed 16-bit quantised (rgbToneFixed)."""
    fixed = _lround(v * 65535.0).astype(np.int64) - 32768
    order = np.argsort(fixed, axis=-1)
    lo = np.take_along_axis(fixed, order[..., 0:1], axis=-1)
    mid = np.take_along_axis(fixed, order[..., 1:2], axis=-1)
    hi = np.take_along_axis(fixed, order[..., 2:3], axis=-1)

    rlo = lut[np.clip(lo + 32768, 0, 65535)]
    rhi = lut[np.clip(hi + 32768, 0, 65535)]
    same = hi == lo
    den = np.where(same, 1, hi - lo)
    ratio = np.where(same, 0.0, (mid - lo) / den)
    rmid = _lround(rlo + (rhi - rlo) * ratio)
    rmid = np.where(same, rlo, rmid)

    res = np.empty(fixed.shape, dtype=np.float64)
    np.put_along_axis(res, order[..., 0:1], rlo.astype(np.float64), axis=-1)
    np.put_along_axis(res, order[..., 2:3], rhi.astype(np.float64), axis=-1)
    np.put_along_axis(res, order[..., 1:2], rmid.astype(np.float64), axis=-1)
    return (res + 32768.0) / 65535.0


def _brightness_part(value, amount):
    gain = 2.0 ** (amount / 2.2)
    toe = max(1.0 / ((gain - 1.0) * 12.0 + 1.0), 0.1)
    split = 0.5 / gain
    out = gain * value
    span = 1.0 - split
    t = np.clip((value - split) / span, 0.0, None)
    omt = 1.0 - t
    hi = t * t * (3.0 - 2.0 * t - omt * toe * span) + \
        (gain * span * t + 0.5 * (1.0 + 2.0 * t)) * omt * omt
    return np.where(value < split, out, hi)


def _exposure_lut(highlight):
    x = np.arange(4097, dtype=np.float64) / 4096.0
    enc = x ** (1.0 / 2.2)
    c = _brightness_part(enc, min(max(highlight, 0.0), 2.0))
    if highlight > 2.0:
        c = _brightness_part(c, min(highlight - 2.0, 2.0))
    return np.power(np.maximum(c, 0.0), 2.2)


def srgb_to_linear(v):
    return np.where(v > 0.040449936, ((v + 0.055) / 1.055) ** 2.4, v / 12.92)


def linear_to_srgb(v):
    v = np.clip(v, 0.0, 1.0)
    return np.where(v > 0.0031306684, 1.055 * np.power(v, 1.0 / 2.4) - 0.055, v * 12.92)


# ------------------------------------------------------------- the transform

class Transformer:
    def __init__(self, temp, tint):
        self.temp = temp
        self.tint = tint
        x, y = white_xy_from_sliders(temp, tint)
        x, y = effective_white(x, y)
        self.white_xy = (x, y)
        self.p, m = colour_spec(x, y)
        self.matrix = m
        g = np.minimum(np.round(4096.0 / self.p), 65535.0)
        self.gain_int = g.astype(np.int64)
        self.gains = g / 4096.0
        self.total = float(np.log2(self.gains.max()))
        self.ordinary = 0.0
        self.highlight = self.total
        self._lut_fwd = _curve_lut(CR_TONE_FWD)
        self._lut_inv = _curve_lut(CR_TONE_INV)
        self._exp_lut = _exposure_lut(self.highlight)

    # ---------------------------------------------------------------- core
    def _wb_kernel(self, scene):
        order = np.argsort(-self.gains, kind="stable")
        i1, i2, i3 = order
        a11 = np.float32(self.gains[i1])
        a12 = np.float32(self.gains[i2])
        a13 = np.float32(self.gains[i3])
        ratio = np.float32(np.float32(1.0) / a11) * a12
        s3 = np.float32(np.float32(1.0) / a11) * a13
        q = a13 * np.float32(0.25)

        z = scene[..., i1].astype(np.float32)
        x = scene[..., i2].astype(np.float32) * ratio
        y = scene[..., i3].astype(np.float32) * s3
        big = np.maximum(z, x)

        wz = np.maximum((z - q) * (np.float32(1.0) / (np.float32(1.0) - q)), np.float32(0.0))
        qx = q * ratio
        wx = np.maximum((x - qx) * (np.float32(1.0) / (ratio - qx)), np.float32(0.0))
        qy = q * s3
        wy = np.maximum((y - qy) * (np.float32(1.0) / (s3 - qy)), np.float32(0.0))

        z2 = z + (big - z) * wz * wz
        x2 = x + (big - x) * wx * wx
        yt = np.maximum(np.float32(0.5) * (x2 + z2), y)
        y2 = y + (yt - y) * wy * wy

        out = np.empty(scene.shape, dtype=np.float64)
        out[..., 0] = z2.astype(np.float64) * self.matrix[0, i1] + \
            x2.astype(np.float64) * self.matrix[0, i2] + \
            y2.astype(np.float64) * self.matrix[0, i3]
        out[..., 1] = z2.astype(np.float64) * self.matrix[1, i1] + \
            x2.astype(np.float64) * self.matrix[1, i2] + \
            y2.astype(np.float64) * self.matrix[1, i3]
        out[..., 2] = z2.astype(np.float64) * self.matrix[2, i1] + \
            x2.astype(np.float64) * self.matrix[2, i2] + \
            y2.astype(np.float64) * self.matrix[2, i3]
        return np.clip(out, 0.0, 1.0)

    def _exposure(self, v):
        gain = np.float32(2.0 ** self.ordinary)
        s = (v.astype(np.float32) * gain).astype(np.float64)
        order = np.argsort(s, axis=-1)
        lo = np.take_along_axis(s, order[..., 0:1], axis=-1)
        mid = np.take_along_axis(s, order[..., 1:2], axis=-1)
        hi = np.take_along_axis(s, order[..., 2:3], axis=-1)

        def lut(u):
            u = np.clip(u, 0.0, 1.0) * 4096.0
            i = np.clip(np.floor(u).astype(np.int64), 0, 4095)
            f = u - i
            return self._exp_lut[i] * (1.0 - f) + self._exp_lut[i + 1] * f

        cl = lut(lo)
        ch = lut(hi)
        den = hi - lo
        r = np.where(den != 0.0, (mid - lo) / np.where(den == 0.0, 1.0, den), 0.0)
        cm = cl + (ch - cl) * r
        same = np.broadcast_to((hi == lo), lo.shape)
        cm = np.where(same, cl, cm)
        out = np.empty(s.shape, dtype=np.float64)
        np.put_along_axis(out, order[..., 0:1], cl, axis=-1)
        np.put_along_axis(out, order[..., 2:3], ch, axis=-1)
        np.put_along_axis(out, order[..., 1:2], cm, axis=-1)
        return out

    # ------------------------------------------------------------- public
    def image(self, rgb):
        """rgb: HxWx3 uint8 -> HxWx3 uint8"""
        v = np.asarray(rgb, dtype=np.float64) / 255.0
        lin = srgb_to_linear(v)
        pp = lin @ K_SRGB_TO_PROPHOTO.T
        scene = _rgb_tone(pp, CR_TONE_INV, self._lut_inv)
        adjusted = self._wb_kernel(scene)
        exposed = self._exposure(adjusted)
        disp = _rgb_tone(exposed, CR_TONE_FWD, self._lut_fwd)
        lin_out = disp @ K_PROPHOTO_TO_SRGB.T
        out = linear_to_srgb(lin_out) * 255.0
        return _lround(np.clip(out, 0.0, 255.0)).astype(np.uint8)


if __name__ == "__main__":
    import sys

    if len(sys.argv) < 5:
        print("usage: python cr_temptint.py <in> <out> <Temp> <Tint>")
        raise SystemExit(1)
    from PIL import Image

    src, dst, temp, tint = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
    im = Image.open(src).convert("RGB")
    res = Transformer(temp, tint).image(np.asarray(im))
    Image.fromarray(res).save(dst)
    print("wrote", dst, res.shape)
