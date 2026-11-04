# Temperature / Tint Algorithm Reconstruction

This task use more than **5_000_000_000** tokens.
The complete pixel-processing algorithm for Temperature / Tint adjustments has been reverse-engineered from Photoshop Camera Raw 7.0 (CS6).

| File | Description |
|---|---|
| `cr_temptint.h` | Core algorithm (C++17, no third-party dependencies) |
| `temptint.cpp` | Command-line tool: `params` / `apply` / `compare` / `validate` |
| `cr_temptint.py` | NumPy reference implementation of the same algorithm, for readability and line-by-line comparison |
| `tone_tables.h` | Two 1025-entry RGBTone tables extracted from the plugin |
| `build.bat` | VS2017 build script (`cl /O2`) |

Simple Run:
```
python cr_temptint.py colorbox.jpg colorbox_out.png -100 0
```


## 1. Key Finding: Sliders → White Point

For **non-raw (JPEG/TIFF)** images, Camera Raw stores
`crs:IncrementalTemperature` / `crs:IncrementalTint` (relative sliders from −100 to +100),
rather than the absolute `Temperature` (K) / `Tint` used for raw images. The plugin's
processing chain is:

```
FUN_4fcc6ee8  Read settings: IncrementalTemperature + IncrementalTint → flag = 1
FUN_4fcc6ea7  Store dng_xy_coord
FUN_4fd8269e  (temp, tint, flag) → dng_xy_coord
                Relative branch (flag=1):
                    T    = kelvinFromTemperatureSlider(temp)
                    tint = tintFromTintSlider(tint)
                Absolute branch (flag=0): T = clamp(T,2000,50000), tint = clamp(tint,-150,150)
                Then xy = dng_temperature(T, tint).Get_xy_coord()
FUN_4fbc1848  dng_temperature::Get_xy_coord() (Robertson isotherm table)
```

### 1.1 Temperature Slider → Kelvin (`FUN_4fd82436`)

```
u = clamp(Temp, -100, 100) / 100
T = 50000 / (3u² − 12u + 10)
```

| Temp | −100 | −50 | 0 | +50 | +100 |
|---|---|---|---|---|---|
| Kelvin | 2000 K | 3333.3 K | 5000 K | 7407.4 K | 50000 K |

### 1.2 Tint Slider → dng tint (`FUN_4fd824f5`)

```
t = clamp(Tint, -100, 100) / 100
f = 0.5 · t · (|t| + 1)
tint = 9.562965 + (150 − 9.562965·f) · f
```

`Tint = 0 → 9.562965`, and `±100 → ±150`. The value 9.562965 is not arbitrary:
it is precisely the tint that shifts the **5000 K Robertson point to D50**. Thus,
slider values (0,0) correspond exactly to
`dng_xy_coord::kD50_xy = (0.3457, 0.3585)`, producing an identity transform
(the code returns D50 directly for (0,0)).

### 1.3 Computing White Point xy from (T, tint) (`FUN_4fbc1848`)

The Robertson isotherm table is at VA `0x50003b90`, with 31 entries of `{mired, u, v, t}`:

```
mired = 1e6 / T
i = first index satisfying table[i+1].mired > mired (capped at 29)
w0 = (m[i+1] − mired) / (m[i+1] − m[i]);   w1 = 1 − w0
s0 = √(1+t0²),  s1 = √(1+t1²)
du = w0/s0 + w1/s1
dv = t0·w0/s0 + t1·w1/s1
(du,dv) /= |(du,dv)|                       ← Unit vector normal to the isotherm
off = tint · (−1/3000)
u = u0·w0 + u1·w1 + du·off
v = v0·w0 + v1·w1 + dv·off
x = 1.5u / (u − 4v + 2)
y = v   / (u − 4v + 2)
```

> Note: Tint shifts along the **isotherm normal**, normalized by `√(1+t²)` using
> the slope in the `t` column. The earlier temperature-only version used
> `u += off·(−v); v += off·u`, which is only an approximation and deviates at the warm end.

Verification: `Get_xy_coord(5000 K, 9.562965)` returns `(0.345721, 0.358526)`,
matching D50 `(0.3457, 0.3585)` within 1e-4. The white point function is continuous
at (0,0).


## 2. White Point → Color Matrix (dng_color_spec)

```
FUN_4fb8685d  dng_color_spec::SetWhiteXY()
FUN_4fb86134  dng_color_spec::MapWhiteMatrix()   Bradford, cone response ratios clamped to [0.1, 10]
FUN_4fc04df9  cr_stage_ABCtoRGB::Init()
```

```
P  = fCameraWhite = clamp( XYZ→ProPhoto · xy2XYZ(x,y) / max(...), 1e-3, 1 )
C  = fCameraToPCS = inverse( XYZ→ProPhoto · MapWhiteMatrix(x,y) / max(...) )
M  = (XYZ→ProPhoto) · C · diag(P)
gainInt[c] = min(round(4096 / P[c]), 65535)          (1/4096 quantization grid)
total      = log2(max gain)                          Exposure
```

### 2.1 Handling Out-of-Gamut White Points (a Critical Step)

`SetWhiteXY` clamps `fCameraWhite` to `[1e-3, 1]`. **The matrix is then derived
from this clamped vector**, rather than the original xy. Within the gamut, the two
are equivalent (the transform is a no-op). Once the white point falls outside the
ProPhoto gamut (very low temperature + large positive tint), the effective white
point is pulled to the **gamut boundary**:

```
w   = XYZ→ProPhoto · xy2XYZ(x, y)
P   = clamp(w / max(w), 1e-3, 1)
If P == w/max(w)  →  White point unchanged
Otherwise        →  w2 = ProPhoto→XYZ · P
                    x' = w2.x / (w2.x+w2.y+w2.z)
                    y' = w2.y / (w2.x+w2.y+w2.z)     ← Use this white point for all subsequent calculations
```

This step reduces the errors for `−60/+80`, `−100/+50`, and `−100/+100` from
1.86 / 2.03 / 5.85 to **0.85 / 0.92 / 1.78**, with **no effect at all** on the
21 in-gamut combinations.


## 3. Main Per-Pixel Loop (`FUN_4fd45e70` + `FUN_4fd45db0`)

```
Sort by gain in descending order → i1,i2,i3
a11=gain[i1], a12=gain[i2], a13=gain[i3] (a13 is always 1 because P is normalized by its maximum)
ratio = a12/a11, s3 = a13/a11 = 1/a11, q = a13·0.25

z = scene[i1]
x = scene[i2] · ratio
y = scene[i3] · s3
big = max(z, x)
wz = max((z − q)/(1 − q), 0)
z2 = z + (big − z)·wz²
wx = max((x − q·ratio)/(ratio − q·ratio), 0)
x2 = x + (big − x)·wx²
wy = max((y − q·s3)/(s3 − q·s3), 0)
yt = max(0.5·(x2 + z2), y)
y2 = y + (yt − y)·wy²
out[c] = clamp(z2·M[c][i1] + x2·M[c][i2] + y2·M[c][i3], 0, 1)
```

## 4. Surrounding Pipeline (Reused from the Earlier Temperature-Only Reconstruction, Verified Pixel by Pixel)

```
sRGB8 → linear sRGB → linear ProPhoto (D50)
      → Inverse RGBTone table CR_TONE_INV  (1025 entries, 16-bit fixed-point, hue-preserving)
      → White balance kernel above
      → Exposure/highlight roll-off ExposureLut(total) (cr_curve_brightness_part)
      → Forward RGBTone table CR_TONE_FWD
      → linear ProPhoto → linear sRGB → sRGB8
```



## 5. Reverse-Engineering Evidence (Reproducible)

REA (rea-agents 6.8.0) + Ghidra 12.1.3, targeting the 32-bit build listed above:

| Address | Purpose |
|---|---|
| `0x4fcc6ee8` | Read settings (Temperature/Tint vs IncrementalTemperature/IncrementalTint) |
| `0x4fcc6ea7` | Store white point |
| `0x4fd8269e` | (slider, slider, flag) → xy, including ±100 clamping and the (0,0)→D50 shortcut |
| `0x4fd82436` | Relative temperature slider → Kelvin `50000/(3u²−12u+10)` |
| `0x4fd8247e` | Inverse function (Kelvin → slider) |
| `0x4fd824f5` | Relative tint slider → dng tint (9.562965, 150) |
| `0x4fd8254d` | Inverse function |
| `0x4fbc1848` | dng_temperature::Get_xy_coord() (Robertson + normal offset) |
| `0x4fb8685d` | dng_color_spec::SetWhiteXY() |
| `0x4fb86134` | dng_color_spec::MapWhiteMatrix() (Bradford, [0.1,10]) |
| `0x4fc04df9` | cr_stage_ABCtoRGB::Init() (P, matrix, gains, exposure) |
| `0x4fd45e70` | Per-pixel white balance kernel |
| `0x4fd45db0` | Highlight recovery (inlined within the kernel) |
| `0x4fc01efe` | Kernel forwarding function (22 parameters) |
| `0x4fc07556` | ABCtoRGB Apply (assembles 22 arguments) |
| `0x50003b90` | Robertson isotherm table (31×4 doubles) |
| `0x530420` / `0x531430` | CR_TONE_FWD / CR_TONE_INV (1025 floats) |
| `0x50001424` | 1025-entry inverse table used by the earlier temperature-only version |

The key details established from the x87 assembly of `0x4fd45e70` (normal
normalization in `Get_xy_coord`, `q=a13*0.25`, and `1/a11` scaling) are all
implemented in `cr_temptint.h`.
