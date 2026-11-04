// ---------------------------------------------------------------------------
// Camera Raw 7.0 (CS6) -- joint Temperature + Tint transform
//
// Recovered from the shipped Photoshop Camera Raw plug-in:
//   32-bit: C:\Program Files (x86)\Common Files\Adobe\Plug-Ins\CS6\File Formats\
//           Camera Raw.8bi   sha256 b2f971020c3ec06f9622f1834c8c5b54c4ef7c692358e65dbfa848e1e5017ffe
//           (image base 0x4fad0000 -- all addresses below refer to this build)
//   64-bit: C:\Program Files\Common Files\Adobe\Plug-Ins\CS6\File Formats\
//           Camera Raw.8bi   sha256 04dff900d9784e86a4b2f7a5f0446147c2d628403de3a27a39fc39b109525a5d
//           (same constants: Robertson table, 9.562965, 150.0, 50000.0, 1/3000)
//
// ---------------------------------------------------------------------------
// The slider -> white point path (the part that this file adds)
// ---------------------------------------------------------------------------
//   FUN_4fcc6ee8  settings reader.  For a rendered file (JPEG/TIFF) ACR stores
//                 "IncrementalTemperature"/"IncrementalTint", i.e. the -100..+100
//                 relative sliders; for raw files it stores absolute
//                 "Temperature" (Kelvin) and "Tint".
//                 The relative pair is passed through with flag = 1, the
//                 absolute pair with flag = 0.
//   FUN_4fcc6ea7  stores the resulting dng_xy_coord; calls
//   FUN_4fd8269e  (slider pair, flag) -> dng_xy_coord
//                    if (temp == 0 && tint == 0)  return dng_xy_coord::kD50_xy
//                    if flag:  T    = kelvinFromTemperatureSlider(temp)
//                              tint = tintFromTintSlider(tint)
//                    else:     T    = clamp(temp, 2000, 50000)
//                              tint = clamp(tint, -150, 150)
//                    return dng_temperature(T, tint).Get_xy_coord()
//   FUN_4fd82436  relative Temperature slider (-100..100) -> Kelvin
//                    T = 50000 / (3u^2 - 12u + 10),  u = clamp(s,-100,100)/100
//                    (u = -100 -> 2000 K, u = 0 -> 5000 K, u = +100 -> 50000 K)
//   FUN_4fd824f5  relative Tint slider (-100..100) -> dng_temperature tint
//                    f    = 0.5 * t * (|t| + 1),      t = clamp(tint,-100,100)/100
//                    tint = 9.562965 + (150 - 9.562965 * f) * f
//                    (tint(-100) = -150, tint(0) = 9.562965, tint(+100) = +150)
//                 9.562965 is the tint that puts the 5000 K Robertson point on
//                 D50, so slider (0,0) is exactly the identity.
//   FUN_4fbc1848  dng_temperature::Get_xy_coord()
//                    Robertson isotemperature table at VA 0x50003b90
//                    ({mired,u,v,t}, 31 entries), linear interpolation, then the
//                    tint is applied along the *unit normal* of the
//                    isotemperature line:
//                        s_i     = sqrt(1 + t_i^2)
//                        du      = w0/s0 + w1/s1
//                        dv      = t0*w0/s0 + t1*w1/s1
//                        (du,dv) /= |(du,dv)|
//                        off     = tint * (-1/3000)
//                        u = u0*w0 + u1*w1 + du*off
//                        v = v0*w0 + v1*w1 + dv*off
//                        x = 1.5 u / (u - 4v + 2),  y = v / (u - 4v + 2)
//   FUN_4fb8685d  dng_color_spec::SetWhiteXY(): stores the xy, rebuilds
//                 fCameraToPCS (Bradford-adapted, cone ratios pinned to [0.1,10])
//                 and fCameraWhite = clamp(XYZ->ProPhoto * whiteXYZ / max, 1e-3, 1)
//   FUN_4fc04df9  cr_stage_ABCtoRGB::Init(): the 3x3
//                 M = (XYZ->ProPhoto) * fCameraToPCS * diag(fCameraWhite)
//                 plus the gains round(4096 / fCameraWhite) (1/4096 lattice) and
//                 the exposure a11 = max gain.
//   FUN_4fd45e70  the per-pixel white balance kernel (see wbKernel below).
//
// The colour management around it (linear sRGB -> ProPhoto, the two 1025-entry
// RGBTone tables, the exposure split with the highlight rolloff) is the same
// machinery that the earlier temperature-only reconstruction established.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "tone_tables.h"

namespace cr {

// ---------------------------------------------------------------- constants

// linear sRGB (D50 adapted) <-> linear ProPhoto, as used by the plug-in
constexpr double kSrgbToProPhoto[3][3] = {
    { 0.529299315089,  0.330050760771,  0.140649924280},
    { 0.098412856595,  0.873484495895,  0.028102647454},
    { 0.016846442812,  0.117682705085,  0.865470851982},
};
constexpr double kProPhotoToSrgb[3][3] = {
    { 2.034289455709, -0.727308148960, -0.306981307112},
    {-0.228924753243,  1.231716583591, -0.002791830246},
    {-0.008469443817, -0.153326000538,  1.161795444488},
};

// dng_space_ProPhoto and its inverse (FUN_4fb85863)
constexpr double kPpToXyz[3][3] = {
    {0.7976749, 0.1351917, 0.0313534},
    {0.2880402, 0.7118741, 0.0000857},
    {0.0000000, 0.0000000, 0.8252099},
};
constexpr double kXyzToPp[3][3] = {
    {+1.345943366936, -0.255607518071, -0.051111838572},
    {-0.544598822462, +1.508167301812, +0.020535108440},
    {+0.000000000000, +0.000000000000, +1.211812897543},
};
constexpr double kBradford[3][3] = {
    { 0.8951,  0.2664, -0.1614},
    {-0.7502,  1.7135,  0.0367},
    { 0.0389, -0.0685,  1.0296},
};
constexpr double kBradfordInv[3][3] = {
    {+0.986992905467, -0.147054256421, +0.159962651664},
    {+0.432305269723, +0.518360271537, +0.049291228213},
    {-0.008528664575, +0.040042821654, +0.968486695788},
};
constexpr double kPcsXyz[3] = {0.9642956764, 1.0, 0.8251046025};  // XYtoXYZ(D50)
constexpr double kPcsXy[2] = {0.3457, 0.3585};                    // dng_xy_coord::kD50_xy

inline void matMul3(const double a[3][3], const double b[3][3], double out[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
}

inline void matVec3(const double m[3][3], const double v[3], double out[3]) {
    for (int i = 0; i < 3; ++i)
        out[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
}

inline void matInv3(const double a[3][3], double out[3][3]) {
    const double c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1];
    const double c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2];
    const double c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
    const double det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
    const double inv = 1.0 / det;
    out[0][0] = c00 * inv;
    out[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * inv;
    out[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * inv;
    out[1][0] = c01 * inv;
    out[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * inv;
    out[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * inv;
    out[2][0] = c02 * inv;
    out[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * inv;
    out[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * inv;
}

inline void xyToXyz(double x, double y, double out[3]) {
    out[0] = x / y;
    out[1] = 1.0;
    out[2] = (1.0 - x - y) / y;
}

// ------------------------------------------------------ dng_temperature ----

// Robertson isotemperature table, read from VA 0x50003b90: { mired, u, v, t }.
struct TempPoint { double mired, u, v, t; };
constexpr TempPoint kTempTable[31] = {
    {   0.0, 0.180060, 0.263520,   -0.243410},
    {  10.0, 0.180660, 0.265890,   -0.254790},
    {  20.0, 0.181330, 0.268460,   -0.268760},
    {  30.0, 0.182080, 0.271190,   -0.285390},
    {  40.0, 0.182930, 0.274070,   -0.304700},
    {  50.0, 0.183880, 0.277090,   -0.326750},
    {  60.0, 0.184940, 0.280210,   -0.351560},
    {  70.0, 0.186110, 0.283420,   -0.379150},
    {  80.0, 0.187400, 0.286680,   -0.409550},
    {  90.0, 0.188800, 0.289970,   -0.442780},
    { 100.0, 0.190320, 0.293260,   -0.478880},
    { 125.0, 0.194620, 0.301410,   -0.582040},
    { 150.0, 0.199620, 0.309210,   -0.704710},
    { 175.0, 0.205250, 0.316470,   -0.849010},
    { 200.0, 0.211420, 0.323120,   -1.018200},
    { 225.0, 0.218070, 0.329090,   -1.216800},
    { 250.0, 0.225110, 0.334390,   -1.451200},
    { 275.0, 0.232470, 0.339040,   -1.729800},
    { 300.0, 0.240100, 0.343080,   -2.063700},
    { 325.0, 0.247020, 0.346550,   -2.468100},
    { 350.0, 0.255910, 0.349510,   -2.964100},
    { 375.0, 0.264000, 0.352000,   -3.581400},
    { 400.0, 0.272180, 0.354070,   -4.363300},
    { 425.0, 0.280390, 0.355770,   -5.376200},
    { 450.0, 0.288630, 0.357140,   -6.726200},
    { 475.0, 0.296850, 0.358230,   -8.595500},
    { 500.0, 0.305050, 0.359070,  -11.324000},
    { 525.0, 0.313200, 0.359680,  -15.628000},
    { 550.0, 0.321290, 0.360110,  -23.325000},
    { 575.0, 0.329310, 0.360380,  -40.770000},
    { 600.0, 0.337240, 0.360510, -116.450000},
};
constexpr int kTempTableCount = 31;

constexpr double kTintZero = 9.562965;   // tint of D50 about the 5000 K point

// FUN_4fd82436: relative Temperature slider (-100..100) -> Kelvin.
inline double kelvinFromTemperatureSlider(double slider) {
    double u = std::min(std::max(slider, -100.0), 100.0) / 100.0;
    return 50000.0 / ((3.0 * u - 12.0) * u + 10.0);
}

// FUN_4fd8247e: Kelvin -> relative Temperature slider.
inline double temperatureSliderFromKelvin(double kelvin) {
    double q = 50000.0 / kelvin;
    q = std::min(std::max(q, 1.0), 25.0);
    const double s = (12.0 - std::sqrt(24.0 + 12.0 * q)) / 6.0;
    return std::min(std::max(s * 100.0, -100.0), 100.0);
}

// FUN_4fd824f5: relative Tint slider (-100..100) -> dng_temperature tint.
inline double tintFromTintSlider(double slider) {
    double t = std::min(std::max(slider, -100.0), 100.0) / 100.0;
    const double f = t * 0.5 * (std::fabs(t) + 1.0);
    return kTintZero + (150.0 - f * kTintZero) * f;
}

// FUN_4fd8254d: dng tint -> relative Tint slider.
inline double tintSliderFromTint(double tint) {
    tint = std::min(std::max(tint, -150.0), 150.0);
    const double g = (tint - 150.0) / -19.12593;
    if (g > 0.0) return (std::sqrt(g) - 1.0) * 50.0;
    if (g < 0.0) return (std::sqrt(-g) - 1.0) * -50.0;
    return 0.0;
}

// FUN_4fbc1848: dng_temperature::Get_xy_coord().
inline void xyFromTemperature(double temperature, double tint, double* outX, double* outY) {
    if (temperature < 1.0) temperature = 1.0;
    const double mired = 1.0e6 / temperature;

    int i = 0;
    while (i != kTempTableCount - 2 && kTempTable[i + 1].mired <= mired) ++i;

    const TempPoint& a = kTempTable[i];
    const TempPoint& b = kTempTable[i + 1];
    const double w0 = (b.mired - mired) / (b.mired - a.mired);
    const double w1 = 1.0 - w0;

    // Unit normal of the interpolated isotemperature line.
    const double s0 = std::sqrt(1.0 + a.t * a.t);
    const double s1 = std::sqrt(1.0 + b.t * b.t);
    double du = w0 / s0 + w1 / s1;
    double dv = (a.t * w0) / s0 + (b.t * w1) / s1;
    const double len = std::sqrt(du * du + dv * dv);
    du /= len;
    dv /= len;

    const double off = tint * (-1.0 / 3000.0);
    const double u = a.u * w0 + b.u * w1 + du * off;
    const double v = a.v * w0 + b.v * w1 + dv * off;

    const double den = u - 4.0 * v + 2.0;
    *outX = (1.5 * u) / den;
    *outY = v / den;
}

// FUN_4fd8269e, relative branch (flag != 0): the sliders of a rendered file.
inline void whiteXyFromSliders(double tempSlider, double tintSlider, double* px, double* py) {
    tempSlider = std::min(std::max(tempSlider, -100.0), 100.0);
    tintSlider = std::min(std::max(tintSlider, -100.0), 100.0);
    if (tempSlider == 0.0 && tintSlider == 0.0) {
        *px = kPcsXy[0];
        *py = kPcsXy[1];
        return;
    }
    xyFromTemperature(kelvinFromTemperatureSlider(tempSlider),
                      tintFromTintSlider(tintSlider), px, py);
}

// FUN_4fd8269e, absolute branch (flag == 0): raw files, Kelvin + tint.
inline void whiteXyFromAbsolute(double kelvin, double tint, double* px, double* py) {
    kelvin = std::min(std::max(kelvin, 2000.0), 50000.0);
    tint = std::min(std::max(tint, -150.0), 150.0);
    xyFromTemperature(kelvin, tint, px, py);
}

// ------------------------------------------------------ dng_color_spec ----

// dng_color_spec::MapWhiteMatrix (FUN_4fb86134): Bradford adaptation from the
// PCS white to `x,y`, cone ratios pinned to [0.1, 10].
inline void mapWhiteMatrix(double x, double y, double out[3][3]) {
    double w1[3], w2[3], c1[3], c2[3], d[3];
    xyToXyz(kPcsXy[0], kPcsXy[1], w1);
    xyToXyz(x, y, w2);
    matVec3(kBradford, w1, c1);
    matVec3(kBradford, w2, c2);
    for (int i = 0; i < 3; ++i) {
        const double s = std::max(c1[i], 0.0);
        const double t = std::max(c2[i], 0.0);
        const double r = s > 0.0 ? t / s : 10.0;
        d[i] = std::min(std::max(r, 0.1), 10.0);
    }
    double a[3][3] = {{d[0], 0.0, 0.0}, {0.0, d[1], 0.0}, {0.0, 0.0, d[2]}};
    double tmp[3][3];
    matMul3(kBradfordInv, a, tmp);
    matMul3(tmp, kBradford, out);
}

// fCameraWhite (colour spec +0x540).
inline void cameraWhite(double x, double y, double p[3]) {
    double xyz[3], w[3];
    xyToXyz(x, y, xyz);
    matVec3(kXyzToPp, xyz, w);
    const double mx = std::max(w[0], std::max(w[1], w[2]));
    for (int i = 0; i < 3; ++i)
        p[i] = std::min(std::max(w[i] / mx, 0.001), 1.0);
}

// SetWhiteXY stores fCameraWhite clamped to [1e-3, 1] and then derives the
// PCS <-> camera matrix from the *clamped* vector, not from the raw xy.  For
// every white inside the ProPhoto gamut the two are identical; for a white
// outside it (a very warm Temperature combined with a strong positive Tint)
// the clamped vector is the only thing the plug-in ever sees, and the effective
// white ends up on the gamut boundary.
inline void effectiveWhite(double x, double y, double* ox, double* oy) {
    double xyz[3], w[3], p[3], w2[3];
    xyToXyz(x, y, xyz);
    matVec3(kXyzToPp, xyz, w);
    const double mx = std::max(w[0], std::max(w[1], w[2]));
    for (int i = 0; i < 3; ++i) p[i] = std::min(std::max(w[i] / mx, 0.001), 1.0);
    if (p[0] == w[0] / mx && p[1] == w[1] / mx && p[2] == w[2] / mx) {
        *ox = x;   // in gamut: nothing to do
        *oy = y;
        return;
    }
    matVec3(kPpToXyz, p, w2);
    const double s = w2[0] + w2[1] + w2[2];
    *ox = w2[0] / s;
    *oy = w2[1] / s;
}

// fCameraToPCS = inverse(fPCStoCamera) (colour spec +0x570).
inline void cameraToPcs(double x, double y, double out[3][3]) {
    double mw[3][3], q[3][3], v[3];
    mapWhiteMatrix(x, y, mw);
    matMul3(kXyzToPp, mw, q);
    matVec3(q, kPcsXyz, v);
    const double mx = std::max(v[0], std::max(v[1], v[2]));
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) q[i][j] /= mx;
    matInv3(q, out);
}

// ------------------------------------------------------------ small helpers

inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

inline double srgbToLinear(double v) {
    return v > 0.040449936 ? std::pow((v + 0.055) / 1.055, 2.4) : v / 12.92;
}

inline double linearToSrgb(double v) {
    v = clamp01(v);
    return v > 0.0031306684 ? 1.055 * std::pow(v, 1.0 / 2.4) - 0.055 : v * 12.92;
}

inline double tableEval(const float* table, double x) {
    const double s = clamp01(x) * 1024.0;
    int i = static_cast<int>(s);
    if (i > 1023) i = 1023;
    const double f = s - i;
    return table[i] * (1.0 - f) + table[i + 1] * f;
}

inline void matApply(const double m[3][3], const double in[3], double out[3]) {
    for (int i = 0; i < 3; ++i)
        out[i] = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2];
}

// Hue-preserving RGBTone with the plug-in's signed 16-bit quantisation points.
inline void rgbToneFixed(const double v[3], const float* table, double out[3]) {
    int fixed[3];
    for (int c = 0; c < 3; ++c)
        fixed[c] = static_cast<int>(std::lround(v[c] * 65535.0)) - 32768;

    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [&](int a, int b) { return fixed[a] < fixed[b]; });
    const int lo = order[0], mid = order[1], hi = order[2];
    auto curveFixed = [&](int value) {
        const double n = clamp01((value + 32768.0) / 65535.0);
        return static_cast<int>(std::lround(tableEval(table, n) * 65535.0)) - 32768;
    };
    int result[3];
    result[lo] = curveFixed(fixed[lo]);
    result[hi] = curveFixed(fixed[hi]);
    if (fixed[hi] == fixed[lo]) {
        result[mid] = result[lo];
    } else {
        const double r = static_cast<double>(fixed[mid] - fixed[lo]) /
                         static_cast<double>(fixed[hi] - fixed[lo]);
        result[mid] = static_cast<int>(std::lround(result[lo] + (result[hi] - result[lo]) * r));
    }
    for (int c = 0; c < 3; ++c) out[c] = (result[c] + 32768.0) / 65535.0;
}

// -------------------------------------------------------------- WB kernel

// FUN_4fd45e70 + FUN_4fd45db0: highlight recovery followed by the 3x3.
inline void wbKernel(const double scene[3], const double gains[3],
                     const double M[3][3], double out[3]) {
    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [&](int a, int b) { return gains[a] > gains[b]; });
    const int i1 = order[0], i2 = order[1], i3 = order[2];
    const float a11 = static_cast<float>(gains[i1]);
    const float a12 = static_cast<float>(gains[i2]);
    const float a13 = static_cast<float>(gains[i3]);
    const float ratio = (1.0f / a11) * a12;
    const float s3 = (1.0f / a11) * a13;
    const float q = a13 * 0.25f;      // a13 is 1.0 because fCameraWhite is max-normalised

    const float z = static_cast<float>(scene[i1]);
    const float x = static_cast<float>(scene[i2]) * ratio;
    const float y = static_cast<float>(scene[i3]) * s3;
    const float big = std::max(z, x);

    const float wz = std::max((z - q) * (1.0f / (1.0f - q)), 0.0f);
    const float qx = q * ratio;
    const float wx = std::max((x - qx) * (1.0f / (ratio - qx)), 0.0f);
    const float qy = q * s3;
    const float wy = std::max((y - qy) * (1.0f / (s3 - qy)), 0.0f);

    const float z2 = z + (big - z) * wz * wz;
    const float x2 = x + (big - x) * wx * wx;
    const float yt = std::max(0.5f * (x2 + z2), y);
    const float y2 = y + (yt - y) * wy * wy;

    for (int c = 0; c < 3; ++c) {
        const double v = static_cast<double>(z2) * M[c][i1] +
                         static_cast<double>(x2) * M[c][i2] +
                         static_cast<double>(y2) * M[c][i3];
        out[c] = clamp01(v);
    }
}

// ----------------------------------------------------- exposure / rolloff

// cr_curve_brightness_part::Evaluate
inline double brightnessPart(double value, double amount) {
    const double gain = std::pow(2.0, amount / 2.2);
    const double toe = std::max(1.0 / ((gain - 1.0) * 12.0 + 1.0), 0.1);
    const double split = 0.5 / gain;
    if (value < split) return gain * value;
    const double span = 1.0 - split;
    const double t = (value - split) / span;
    const double omt = 1.0 - t;
    return t * t * (3.0 - 2.0 * t - omt * toe * span) +
           (gain * span * t + 0.5 * (1.0 + 2.0 * t)) * omt * omt;
}

struct ExposureLut {
    float table[4097];
    explicit ExposureLut(double highlight) {
        for (int i = 0; i <= 4096; ++i) {
            const double x = i / 4096.0;
            const double enc = std::pow(x, 1.0 / 2.2);
            double c = brightnessPart(enc, std::min(std::max(highlight, 0.0), 2.0));
            if (highlight > 2.0) c = brightnessPart(c, std::min(highlight - 2.0, 2.0));
            table[i] = static_cast<float>(std::pow(std::max(c, 0.0), 2.2));
        }
    }
    double operator()(double v) const {
        v = std::min(std::max(v, 0.0), 1.0);
        const double s = v * 4096.0;
        int i = static_cast<int>(s);
        if (i > 4095) i = 4095;
        const double f = s - i;
        return table[i] * (1.0 - f) + table[i + 1] * f;
    }
};

inline void exposureTone(const double v[3], double exposure, double highlight,
                         const ExposureLut& lut, double out[3]) {
    const float gain = static_cast<float>(std::pow(2.0, exposure));
    float scaled[3];
    for (int c = 0; c < 3; ++c) scaled[c] = static_cast<float>(v[c]) * gain;
    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [&](int a, int b) { return scaled[a] < scaled[b]; });
    const int lo = order[0], mid = order[1], hi = order[2];
    const double cl = lut(scaled[lo]);
    const double ch = lut(scaled[hi]);
    if (lo == hi) {
        out[0] = out[1] = out[2] = cl;
        return;
    }
    const float den = scaled[hi] - scaled[lo];
    const float r = den != 0.0f ? (scaled[mid] - scaled[lo]) / den : 0.0f;
    out[lo] = cl;
    out[hi] = ch;
    out[mid] = cl + (ch - cl) * r;
}

// --------------------------------------------------------------- pipeline

struct Params {
    double gains[3];        // round(4096 / fCameraWhite) / 4096
    double matrix[3][3];    // (XYZ->ProPhoto) * fCameraToPCS * diag(fCameraWhite)
    double total;           // log2(max gain)
    double ordinary;        // exposure applied to every channel
    double highlight;       // total - ordinary, drives the rolloff
    int gainInt[3];         // the integers Init() stores
    double p[3];            // fCameraWhite
    double whiteX, whiteY;
};

// cr_curve_exposure::UpdateSplit (VA 0x4fc1197f).
inline void splitExposure(double total, double statDelta, double compensation,
                          double* ordinary, double* highlight) {
    *ordinary = 0.0;
    *highlight = total;
    if (total > 0.0) {
        double cap = statDelta - 1.0;
        if (cap < 0.0) cap = 0.0;
        *ordinary = std::min(cap, total);
        *highlight = std::max(total - *ordinary, 0.0);
    }
    if (compensation > 0.0) {
        const double move = std::min(*highlight, compensation);
        *highlight -= move;
        *ordinary += move;
    } else if (compensation < 0.0) {
        const double move = std::min(*ordinary, -compensation);
        *ordinary -= move;
        *highlight += move;
    }
}

// For a rendered sRGB file the plug-in's split leaves everything in the
// highlight rolloff (stat21 - stat205 <= 1 -> cap 0).
constexpr double kStatDelta = 0.0;
constexpr double kExposureCompensation = 0.0;

inline Params makeParams(double tempSlider, double tintSlider) {
    Params p;
    double x, y;
    whiteXyFromSliders(tempSlider, tintSlider, &x, &y);
    effectiveWhite(x, y, &x, &y);       // SetWhiteXY works from the clamped
    p.whiteX = x;                       // fCameraWhite, not from the raw xy
    p.whiteY = y;

    double P[3];
    cameraWhite(x, y, P);
    double b[3][3], m4[3][3];
    cameraToPcs(x, y, b);
    matMul3(kXyzToPp, b, m4);

    for (int c = 0; c < 3; ++c) {
        p.p[c] = P[c];
        const double g = std::min(std::round(4096.0 / P[c]), 65535.0);
        p.gainInt[c] = static_cast<int>(g);
        p.gains[c] = g / 4096.0;
        for (int r = 0; r < 3; ++r) p.matrix[r][c] = m4[r][c] * P[c];
    }

    p.total = std::log2(std::max(p.gains[0], std::max(p.gains[1], p.gains[2])));
    splitExposure(p.total, kStatDelta, kExposureCompensation, &p.ordinary, &p.highlight);
    return p;
}

inline void sceneFromPixel(const uint8_t in[3], double scene[3]) {
    double lin[3], pp[3];
    for (int c = 0; c < 3; ++c) lin[c] = srgbToLinear(in[c] / 255.0);
    matApply(kSrgbToProPhoto, lin, pp);
    rgbToneFixed(pp, CR_TONE_INV, scene);
}

inline void pixelFromScene(const double scene[3], const Params& p, double exposure,
                           const ExposureLut& lut, uint8_t out[3]) {
    double exposed[3], disp[3], lin[3];
    exposureTone(scene, exposure, p.highlight, lut, exposed);
    rgbToneFixed(exposed, CR_TONE_FWD, disp);
    matApply(kProPhotoToSrgb, disp, lin);
    for (int c = 0; c < 3; ++c) {
        const double v = linearToSrgb(lin[c]) * 255.0;
        out[c] = static_cast<uint8_t>(std::lround(std::min(std::max(v, 0.0), 255.0)));
    }
}

class Transformer {
public:
    Transformer(double tempSlider, double tintSlider)
        : p_(makeParams(tempSlider, tintSlider)), lut_(p_.highlight) {}

    void pixel(const uint8_t in[3], uint8_t out[3]) const {
        double scene[3], adjusted[3];
        sceneFromPixel(in, scene);
        wbKernel(scene, p_.gains, p_.matrix, adjusted);
        pixelFromScene(adjusted, p_, p_.ordinary, lut_, out);
    }

    void buffer(const uint8_t* src, uint8_t* dst, size_t n) const {
        for (size_t i = 0; i < n; ++i) pixel(src + 3 * i, dst + 3 * i);
    }

    const Params& params() const { return p_; }

private:
    Params p_;
    ExposureLut lut_;
};

}  // namespace cr
