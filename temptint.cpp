// temptint -- Camera Raw CS6 joint Temperature/Tint transform.
//
//   temptint params  <Temp> <Tint>
//   temptint apply   <in> <out> <Temp> <Tint>
//   temptint compare <in> <ref> <Temp> <Tint> [stride]
//   temptint validate <root> [stride] [imageFilter]
//
// Temp and Tint are the -100..100 slider positions of the Camera Raw
// "Temperature" and "Tint" sliders as they appear for a rendered file
// (JPEG/TIFF): -100..+100 relative to the as-shot white balance.
//
// Image I/O uses Windows Imaging Component (JPEG, PNG, TIFF, BMP).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cr_temptint.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace {

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "%s failed: 0x%08lx\n", what, static_cast<unsigned long>(hr));
        std::exit(1);
    }
}

std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    w.resize(n - 1);
    return w;
}

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;
};

bool loadImage(const std::string& path, Image* img) {
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
        return false;
    const std::wstring wpath = widen(path);
    if (FAILED(factory->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnLoad, &decoder))) {
        factory->Release();
        return false;
    }
    decoder->GetFrame(0, &frame);
    factory->CreateFormatConverter(&conv);
    conv->Initialize(frame, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                     WICBitmapPaletteTypeCustom);
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    img->w = static_cast<int>(w);
    img->h = static_cast<int>(h);
    const UINT rowBytes = w * 3;
    std::vector<uint8_t> bgr(static_cast<size_t>(rowBytes) * h);
    conv->CopyPixels(nullptr, rowBytes, static_cast<UINT>(bgr.size()), bgr.data());
    img->rgb.resize(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < img->rgb.size(); i += 3) {
        img->rgb[i + 0] = bgr[i + 2];
        img->rgb[i + 1] = bgr[i + 1];
        img->rgb[i + 2] = bgr[i + 0];
    }
    conv->Release();
    frame->Release();
    decoder->Release();
    factory->Release();
    return true;
}

bool saveImage(const std::string& path, const Image& img) {
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&factory)), "CoCreateInstance(WIC)");
    const std::wstring wpath = widen(path);
    check(factory->CreateStream(&stream), "CreateStream");
    check(stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE), "InitializeFromFilename");
    GUID container = GUID_ContainerFormatPng;
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        std::string ext = path.substr(dot);
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".jpg" || ext == ".jpeg") container = GUID_ContainerFormatJpeg;
        else if (ext == ".tif" || ext == ".tiff") container = GUID_ContainerFormatTiff;
        else if (ext == ".bmp") container = GUID_ContainerFormatBmp;
    }
    check(factory->CreateEncoder(container, nullptr, &encoder), "CreateEncoder");
    check(encoder->Initialize(stream, WICBitmapEncoderNoCache), "encoder Initialize");
    check(encoder->CreateNewFrame(&frame, &props), "CreateNewFrame");
    if (props) {
        PROPBAG2 opt = {};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 1.0f;
        props->Write(1, &opt, &v);
        VariantClear(&v);
    }
    check(frame->Initialize(props), "frame Initialize");
    check(frame->SetSize(img.w, img.h), "SetSize");
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    check(frame->SetPixelFormat(&fmt), "SetPixelFormat");
    std::vector<uint8_t> bgr(img.rgb.size());
    for (size_t i = 0; i < img.rgb.size(); i += 3) {
        bgr[i + 0] = img.rgb[i + 2];
        bgr[i + 1] = img.rgb[i + 1];
        bgr[i + 2] = img.rgb[i + 0];
    }
    const UINT stride = static_cast<UINT>(img.w) * 3;
    check(frame->WritePixels(img.h, stride, static_cast<UINT>(bgr.size()), bgr.data()), "WritePixels");
    check(frame->Commit(), "frame Commit");
    check(encoder->Commit(), "encoder Commit");
    if (props) props->Release();
    frame->Release();
    encoder->Release();
    stream->Release();
    factory->Release();
    return true;
}

void printParams(double t, double n) {
    const cr::Params p = cr::makeParams(t, n);
    std::printf("Temperature slider %g, Tint slider %g\n", t, n);
    std::printf("  white xy       = %.9f %.9f\n", p.whiteX, p.whiteY);
    std::printf("  kelvin         = %.3f\n", cr::kelvinFromTemperatureSlider(t));
    std::printf("  dng tint       = %.6f\n", cr::tintFromTintSlider(n));
    std::printf("  fCameraWhite   = %.9f %.9f %.9f\n", p.p[0], p.p[1], p.p[2]);
    std::printf("  gains(int)     = %d %d %d  (1/4096)\n", p.gainInt[0], p.gainInt[1], p.gainInt[2]);
    std::printf("  gains          = %.9f %.9f %.9f\n", p.gains[0], p.gains[1], p.gains[2]);
    std::printf("  total          = %.9f  ordinary = %.9f  highlight = %.9f\n", p.total, p.ordinary,
                p.highlight);
    for (int r = 0; r < 3; ++r)
        std::printf("  M[%d]           = %+.9f %+.9f %+.9f\n", r, p.matrix[r][0], p.matrix[r][1],
                    p.matrix[r][2]);
}

static std::string sliderTag(int v) {
    char b[32];
    if (v > 0) std::snprintf(b, sizeof(b), "+%d", v);
    else std::snprintf(b, sizeof(b), "%d", v);
    return std::string(b);
}

struct Stats {
    double sum = 0.0;
    long long n = 0;
    int worst = 0;
    long long over2 = 0, over4 = 0;
    long long hist[8] = {0};
    void add(int d) {
        sum += d;
        ++n;
        if (d > worst) worst = d;
        if (d > 2) ++over2;
        if (d > 4) ++over4;
        if (d < 8) ++hist[d];
    }
    void print(const char* tag) const {
        if (!n) { std::printf("%s no data\n", tag); return; }
        std::printf("%s mean=%.3f max=%d over2=%.2f%% over4=%.2f%%  |d| 0..7:", tag, sum / n, worst,
                    100.0 * over2 / n, 100.0 * over4 / n);
        for (int i = 0; i < 8; ++i) std::printf(" %.1f", 100.0 * hist[i] / n);
        std::printf("\n");
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage:\n"
                     "  temptint params  <Temp> <Tint>\n"
                     "  temptint apply   <in> <out> <Temp> <Tint>\n"
                     "  temptint compare <in> <ref> <Temp> <Tint> [stride]\n"
                     "  temptint validate <root> [stride] [filter]\n");
        return 1;
    }
    const std::string mode = argv[1];
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (mode == "params") {
        if (argc < 4) return 1;
        printParams(std::atof(argv[2]), std::atof(argv[3]));
        return 0;
    }
    if (mode == "apply") {
        if (argc < 6) return 1;
        Image in, out;
        if (!loadImage(argv[2], &in)) { std::fprintf(stderr, "decode failed\n"); return 1; }
        out.w = in.w; out.h = in.h; out.rgb.resize(in.rgb.size());
        const cr::Transformer t(std::atof(argv[4]), std::atof(argv[5]));
        t.buffer(in.rgb.data(), out.rgb.data(), static_cast<size_t>(in.w) * in.h);
        return saveImage(argv[3], out) ? 0 : 1;
    }
    if (mode == "compare") {
        if (argc < 6) return 1;
        Image in, ref;
        if (!loadImage(argv[2], &in) || !loadImage(argv[3], &ref)) return 1;
        if (in.w != ref.w || in.h != ref.h) { std::fprintf(stderr, "size mismatch\n"); return 1; }
        const int stride = argc > 6 ? std::atoi(argv[6]) : 1;
        const cr::Transformer t(std::atof(argv[4]), std::atof(argv[5]));
        const size_t pixels = static_cast<size_t>(in.w) * static_cast<size_t>(in.h);
        Stats st;
        for (size_t i = 0; i < pixels; i += static_cast<size_t>(stride)) {
            uint8_t out[3];
            t.pixel(in.rgb.data() + 3 * i, out);
            for (int c = 0; c < 3; ++c)
                st.add(std::abs(static_cast<int>(out[c]) - static_cast<int>(ref.rgb[3 * i + c])));
        }
        st.print("compare");
        return st.worst <= 4 ? 0 : 2;
    }
    if (mode == "validate") {
        if (argc < 3) return 1;
        const std::string root = argv[2];
        const int stride = argc > 3 ? std::atoi(argv[3]) : 8;
        std::vector<std::string> names;
        {
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA((root + "\\orig\\*.jpg").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) { std::fprintf(stderr, "cannot list\n"); return 1; }
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
            std::sort(names.begin(), names.end());
        }
        if (argc > 4) {
            const std::string filter = argv[4];
            std::vector<std::string> keep;
            for (const std::string& n : names)
                if (n.find(filter) != std::string::npos) keep.push_back(n);
            if (!keep.empty()) names = keep;
        }
        std::vector<std::pair<int, int>> sliders;
        {
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA((root + "\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                    const std::string d = fd.cFileName;
                    if (d == "orig" || d == "." || d == "..") continue;
                    int a = 0, b = 0;
                    if (std::sscanf(d.c_str(), "%d %d", &a, &b) == 2) sliders.emplace_back(a, b);
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
            std::sort(sliders.begin(), sliders.end());
        }
        Stats grandFlat;
        std::printf("%-10s %s\n", "sliders", "flat-pixel error (reference vs reconstruction)");
        for (const auto& s : sliders) {
            Stats flatStats;
            char tag[64];
            std::snprintf(tag, sizeof(tag), "%+d %+d", s.first, s.second);
            const std::string dtag = sliderTag(s.first) + " " + sliderTag(s.second);
            const std::string dir = root + "\\" + dtag;
            const cr::Transformer t(s.first, s.second);
            for (const std::string& name : names) {
                Image in, ref;
                if (!loadImage(root + "\\orig\\" + name, &in)) continue;
                if (!loadImage(dir + "\\" + name, &ref)) continue;
                if (in.w != ref.w || in.h != ref.h) continue;
                const size_t pixels = static_cast<size_t>(in.w) * static_cast<size_t>(in.h);
                for (size_t i = 0; i < pixels; i += static_cast<size_t>(stride)) {
                    uint8_t out[3];
                    t.pixel(in.rgb.data() + 3 * i, out);
                    const int px = static_cast<int>(i % static_cast<size_t>(in.w));
                    const int py = static_cast<int>(i / static_cast<size_t>(in.w));
                    bool flat = px > 1 && py > 1 && px + 2 < in.w && py + 2 < in.h;
                    for (int dy = -2; dy <= 2 && flat; ++dy) {
                        for (int dx = -2; dx <= 2 && flat; ++dx) {
                            const size_t j = static_cast<size_t>(py + dy) * in.w + (px + dx);
                            for (int c = 0; c < 3; ++c)
                                if (std::abs(static_cast<int>(in.rgb[3 * j + c]) -
                                             static_cast<int>(in.rgb[3 * i + c])) > 1) { flat = false; break; }
                        }
                    }
                    if (!flat) continue;
                    for (int c = 0; c < 3; ++c)
                        flatStats.add(std::abs(static_cast<int>(out[c]) - static_cast<int>(ref.rgb[3 * i + c])));
                }
            }
            std::printf("%-10s ", tag);
            flatStats.print("");
            grandFlat.sum += flatStats.sum;
            grandFlat.n += flatStats.n;
            grandFlat.worst = std::max(grandFlat.worst, flatStats.worst);
            grandFlat.over2 += flatStats.over2;
            grandFlat.over4 += flatStats.over4;
            for (int i = 0; i < 8; ++i) grandFlat.hist[i] += flatStats.hist[i];
            std::fflush(stdout);
        }
        std::printf("\n");
        grandFlat.print("ALL FLAT ");
        return 0;
    }
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 1;
}
