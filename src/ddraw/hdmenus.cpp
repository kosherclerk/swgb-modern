// HD menu backgrounds.
//
// The menus run at 800x600 and their art exists only at that size. The
// launcher upscales each menu background once with an AI upscaler and saves it
// as SWGBModern\hd\menu\<crc32 of the original SLP>.jpg (or .png). When the game loads a
// sprite whose CRC has an HD version, we keep the original (as palette
// indices) and the HD image. Each frame we detect which known background is on
// screen and composite the HD image through the overlay layer - but only where
// the screen still shows the original background pixel, so buttons, hover
// highlights and text drawn by the game stay on top. The mask is sampled
// bilinearly, which gives soft edges at the HD resolution.
#include "hdmenus.h"
#include "slp.h"
#include <wincodec.h>
#include <vector>
#include <map>
#include <memory>
#include <algorithm>
#include <thread>
#include <math.h>
#include <climits>

namespace swgbm {
namespace {

struct Bg {
    uint32_t crc = 0;
    int w = 0, h = 0, x = 0, y = 0;   // original size and position (from hotspot)
    std::vector<int16_t> idx;         // original palette indices (-1 transparent)
    int hw = 0, hh = 0;               // HD size
    std::vector<uint32_t> hd;         // HD pixels, BGRA (straight alpha)
};

CRITICAL_SECTION g_lock;
bool g_enabled = false;
std::wstring g_dir;
std::map<uint32_t, std::shared_ptr<Bg>> g_bgs;   // crc -> background
std::map<uint32_t, bool> g_checked;              // crc -> has HD file
std::shared_ptr<Bg> g_active;
bool g_forceFull = true;
std::vector<uint8_t> g_mask;   // mask of the active background as last composited

uint32_t Crc32(const uint8_t* d, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool LoadPng(const std::wstring& path, int& w, int& h, std::vector<uint32_t>& out) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f)) &&
        SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        UINT uw = 0, uh = 0;
        cv->GetSize(&uw, &uh);
        w = (int)uw; h = (int)uh;
        out.resize((size_t)w * h);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)(out.size() * 4), (BYTE*)out.data()));
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    if (f) f->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

} // namespace

void HdMenus::Init() {
    InitializeCriticalSection(&g_lock);
    g_enabled = CfgBool("Enhancements", "HDMenus", false);
    g_dir = GameDir() + L"\\SWGBModern\\hd\\menu\\";
    if (g_enabled) LOG("hd menus: enabled (%ls)", g_dir.c_str());
}

void HdMenus::OnSlpLoaded(const void* data, size_t size) {
    if (!g_enabled || size < 64) return;
    uint32_t crc = Crc32((const uint8_t*)data, size);
    EnterCriticalSection(&g_lock);
    bool seen = g_checked.count(crc) != 0;
    if (!seen) g_checked[crc] = false;
    LeaveCriticalSection(&g_lock);
    if (seen) return;
    // the downloadable HD pack uses JPEG (the original background's own
    // transparency mask is applied at runtime); locally made files may be PNG
    std::wstring path;
    for (const wchar_t* ext : {L"jpg", L"png"}) {
        wchar_t name[20];
        swprintf(name, 20, L"%08x.%ls", crc, ext);
        if (GetFileAttributesW((g_dir + name).c_str()) != INVALID_FILE_ATTRIBUTES) { path = g_dir + name; break; }
    }
    if (path.empty()) return;
    std::vector<SlpFrame> fr;
    if (!SlpDecode((const uint8_t*)data, size, fr) || fr.empty() || fr[0].w < 320) return;
    auto bg = std::make_shared<Bg>();
    bg->crc = crc;
    bg->w = fr[0].w; bg->h = fr[0].h; bg->x = -fr[0].hx; bg->y = -fr[0].hy;
    bg->idx = std::move(fr[0].px);
    if (!LoadPng(path, bg->hw, bg->hh, bg->hd)) { LOG("hd menus: cannot read %ls", path.c_str()); return; }
    EnterCriticalSection(&g_lock);
    g_bgs[crc] = bg;
    g_checked[crc] = true;
    LeaveCriticalSection(&g_lock);
    LOG("hd menus: background %08x (%dx%d) -> HD %dx%d", crc, bg->w, bg->h, bg->hw, bg->hh);
}

bool HdMenus::Compose(const Frame& f, Overlay& out) {
    if (!g_enabled) return false;
    EnterCriticalSection(&g_lock);
    std::vector<std::shared_ptr<Bg>> list;
    for (auto& kv : g_bgs) list.push_back(kv.second);
    LeaveCriticalSection(&g_lock);
    if (list.empty()) return false;

    // Which background is on screen? Sample a grid and count exact matches.
    bool frameChanged = f.Dirty() || g_forceFull;
    std::shared_ptr<Bg> best = g_active;
    if (frameChanged) {
        double bestScore = 0;
        best = nullptr;
        for (auto& b : list) {
            if (b->x < 0 || b->y < 0 || b->x + b->w > f.width || b->y + b->h > f.height) continue;
            int hits = 0, total = 0;
            for (int sy = 0; sy < 30; sy++) {
                for (int sx = 0; sx < 40; sx++) {
                    int x = (sx * 2 + 1) * b->w / 80, y = (sy * 2 + 1) * b->h / 60;
                    int16_t v = b->idx[(size_t)y * b->w + x];
                    if (v < 0) continue;
                    total++;
                    hits += f.pixels[(size_t)(b->y + y) * f.width + b->x + x] == (uint8_t)v;
                }
            }
            double score = total ? (double)hits / total : 0;
            if (score > bestScore) { bestScore = score; best = b; }
        }
        if (bestScore < 0.5) best = nullptr;
    }
    if (best != g_active) { g_active = best; g_forceFull = true; g_mask.clear(); }
    if (!g_active) { g_forceFull = true; return false; }
    if (!frameChanged) { out.dirtyY0 = out.dirtyY1 = 0; return true; }

    Bg& b = *g_active;
    bool full = g_forceFull || out.width != b.hw || out.height != b.hh;
    if (out.width != b.hw || out.height != b.hh) out.pixels.assign((size_t)b.hw * b.hh, 0);
    out.width = b.hw; out.height = b.hh;
    out.x = (float)b.x; out.y = (float)b.y; out.w = (float)b.w; out.h = (float)b.h;
    out.visible = true; out.hideFrame = false;
    // game rows the presenter reported as changed
    int gy0 = full ? 0 : std::max(0, f.dirtyY0 - b.y), gy1 = full ? b.h : std::min(b.h, f.dirtyY1 - b.y);
    // mask(game pixel) = 1 where the screen still shows the original
    // background. The HD result depends only on the mask (not the palette), so
    // only rows whose mask actually changed are recomposited.
    if (g_mask.size() != (size_t)b.w * b.h) { g_mask.assign((size_t)b.w * b.h, 0); full = true; gy0 = 0; gy1 = b.h; }
    int cy0 = INT_MAX, cy1 = -1;
    for (int y = gy0; y < gy1; y++) {
        const uint8_t* fr = f.pixels.data() + (size_t)(b.y + y) * f.width + b.x;
        const int16_t* br = b.idx.data() + (size_t)y * b.w;
        uint8_t* m = g_mask.data() + (size_t)y * b.w;
        bool rowChanged = false;
        for (int x = 0; x < b.w; x++) {
            uint8_t v = (br[x] >= 0 && fr[x] == (uint8_t)br[x]) ? 255 : 0;
            if (v != m[x]) { m[x] = v; rowChanged = true; }
        }
        if (rowChanged) { cy0 = std::min(cy0, y); cy1 = std::max(cy1, y + 1); }
    }
    if (full) { cy0 = 0; cy1 = b.h; }
    if (cy1 <= cy0) { out.dirtyY0 = out.dirtyY1 = 0; g_forceFull = false; return true; }
    // HD rows affected (+1 game row margin for the bilinear mask)
    int hy0 = std::max(0, (int)((int64_t)(cy0 - 1) * b.hh / b.h) - 2), hy1 = std::min(b.hh, (int)((int64_t)(cy1 + 1) * b.hh / b.h) + 2);
    const std::vector<uint8_t>& mask = g_mask;
    const float sx = (float)b.w / b.hw, sy = (float)b.h / b.hh;
    // per-column sampling positions (same for every row)
    std::vector<int> tx0(b.hw), tx1(b.hw);
    std::vector<float> tfx(b.hw);
    for (int X = 0; X < b.hw; X++) {
        float gx = (X + 0.5f) * sx - 0.5f;
        int x0 = std::max(0, std::min(b.w - 1, (int)floorf(gx)));
        tx0[X] = x0; tx1[X] = std::min(b.w - 1, x0 + 1);
        tfx[X] = std::max(0.0f, std::min(1.0f, gx - x0));
    }
    auto work = [&](int r0, int r1) {
        for (int Y = r0; Y < r1; Y++) {
            float gy = (Y + 0.5f) * sy - 0.5f;
            int y0 = std::max(0, std::min(b.h - 1, (int)floorf(gy))), y1 = std::min(b.h - 1, y0 + 1);
            float fy = std::max(0.0f, std::min(1.0f, gy - y0));
            const uint8_t* m0 = mask.data() + (size_t)y0 * b.w;
            const uint8_t* m1 = mask.data() + (size_t)y1 * b.w;
            const uint32_t* src = b.hd.data() + (size_t)Y * b.hw;
            uint32_t* dst = out.pixels.data() + (size_t)Y * b.hw;
            for (int X = 0; X < b.hw; X++) {
                int x0 = tx0[X], x1 = tx1[X];
                uint32_t c = src[X];
                int s4 = m0[x0] + m0[x1] + m1[x0] + m1[x1];
                if (s4 == 0) { dst[X] = 0; continue; }
                if (s4 == 4 * 255 && (c >> 24) == 255) { dst[X] = c; continue; }
                float fx = tfx[X];
                float a = ((m0[x0] * (1 - fx) + m0[x1] * fx) * (1 - fy) + (m1[x0] * (1 - fx) + m1[x1] * fx) * fy) / 255.0f;
                a *= (c >> 24) / 255.0f;
                uint32_t A = (uint32_t)(a * 255 + 0.5f);
                uint32_t R = (uint32_t)(((c >> 16) & 0xFF) * a + 0.5f), G = (uint32_t)(((c >> 8) & 0xFF) * a + 0.5f),
                         B = (uint32_t)((c & 0xFF) * a + 0.5f);
                dst[X] = (A << 24) | (R << 16) | (G << 8) | B;
            }
        }
    };
    int rows = hy1 - hy0;
    unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    if (rows < 128 || nt == 1) work(hy0, hy1);
    else {
        std::vector<std::thread> th;
        int chunk = (rows + (int)nt - 1) / (int)nt;
        for (unsigned i = 0; i < nt; i++) {
            int a = hy0 + (int)i * chunk, e = std::min(hy1, a + chunk);
            if (a < e) th.emplace_back(work, a, e);
        }
        for (auto& t : th) t.join();
    }
    out.dirtyY0 = full ? 0 : hy0;
    out.dirtyY1 = full ? b.hh : hy1;
    g_forceFull = false;
    return true;
}

void HdMenus::Invalidate() { g_forceFull = true; }

} // namespace swgbm
