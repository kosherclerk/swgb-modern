// Software emulation of the DirectDraw 1 subset used by the Genie engine.
#include "ddraw_impl.h"
#include "capture.h"
#include "zoom.h"
#include "floattext.h"
#include <emmintrin.h>
#include "presenter.h"
#include "window.h"
#include "renderer.h"
#include "video.h"
#include <algorithm>
#include <string.h>
#include <climits>

namespace swgbm {

DDState& State() {
    static DDState* s = [] {
        auto* st = new DDState();
        InitializeCriticalSectionAndSpinCount(&st->lock, 2000);
        return st;
    }();
    return *s;
}

struct Lock_ {
    Lock_() { EnterCriticalSection(&State().lock); }
    ~Lock_() { LeaveCriticalSection(&State().lock); }
};

static void NotifyPrimaryChanged(bool flip) {
    if (flip) Capture::OnGameFrame();
    Presenter::Get().FrameReady(flip);
}

// Copy one row to the screen surface, writing only the 16-byte blocks that
// differ; returns whether anything changed. One pass instead of a copy here
// plus a compare and a copy in the presenter (the engine re-sends mostly
// identical frames).
__attribute__((target("sse2"))) static bool CopyRowIfChanged(uint8_t* d, const uint8_t* s, int n) {
    bool changed = false;
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        __m128i a = _mm_loadu_si128((const __m128i*)(s + i)), b = _mm_loadu_si128((const __m128i*)(d + i));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(a, b)) != 0xFFFF) { _mm_storeu_si128((__m128i*)(d + i), a); changed = true; }
    }
    for (; i < n; i++) if (d[i] != s[i]) { d[i] = s[i]; changed = true; }
    return changed;
}

// Rows of the screen surface that a blit changed (exactly known).
static void MarkPrimaryRows(const std::vector<int>& rows, int h) {
    auto& st = State();
    EnterCriticalSection(&st.lock);
    if ((int)st.rowsChanged.size() != h) st.rowsChanged.assign(h, 0);
    for (int y : rows) { st.rowsChanged[y] = 1; st.changedY0 = std::min(st.changedY0, y); st.changedY1 = std::max(st.changedY1, y + 1); }
    InterlockedIncrement(&st.version);
    LeaveCriticalSection(&st.lock);
}

void MarkPrimaryDirty(int y0, int y1, bool palette) {
    auto& st = State(); // caller may or may not hold the (recursive) lock
    EnterCriticalSection(&st.lock);
    if (y1 > y0) { st.dirtyY0 = std::min(st.dirtyY0, y0); st.dirtyY1 = std::max(st.dirtyY1, y1); }
    if (palette) st.paletteDirty = true;
    InterlockedIncrement(&st.version);
    LeaveCriticalSection(&st.lock);
}

// Debug: heuristic stack scan for return addresses into the game's code.
static void LogStackScan(const char* tag) {
    static int budget = 4, skip = 400;
    static const bool enabled = CfgBool("Debug", "StackScan", false);   // read once, not per blit
    if (!enabled || skip-- > 0 || budget <= 0) return;
    budget--;
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    uintptr_t* sp = (uintptr_t*)__builtin_frame_address(0);
    char buf[2048];
    int n = snprintf(buf, sizeof(buf), "stack[%s]:", tag);
    for (int i = 0; i < 1024 && (void*)(sp + i) < tib->StackBase && n < 2000; i++) {
        uintptr_t v = sp[i];
        if (v > 0x401000 && v < 0x648000) {
            auto p = (const uint8_t*)v;
            if (p[-5] == 0xE8 || p[-2] == 0xFF || p[-3] == 0xFF || p[-6] == 0xFF) n += snprintf(buf + n, sizeof(buf) - n, " %06x", (unsigned)v);
        }
    }
    LOG("%s", buf);
}

// ------------------------------------------------------------------ palette
DDPalette::DDPalette(DWORD flags, const PALETTEENTRY* e) : flags_(flags) {
    memset(entries_, 0, sizeof(entries_));
    if (e) memcpy(entries_, e, sizeof(entries_));
    for (int i = 0; i < 256; i++)
        rgbq_[i] = {entries_[i].peBlue, entries_[i].peGreen, entries_[i].peRed, 0};
}
HRESULT DDPalette::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IDirectDrawPalette) { *ppv = this; AddRef(); return S_OK; }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG DDPalette::AddRef() { return InterlockedIncrement(&refs_); }
ULONG DDPalette::Release() {
    LONG r = InterlockedDecrement(&refs_);
    if (r == 0) {
        Lock_ l;
        if (State().primaryPal == this) State().primaryPal = nullptr;
        for (auto* s : State().surfaces) if (s->pal_ == this) s->pal_ = nullptr;
        delete this;
    }
    return r;
}
HRESULT DDPalette::GetCaps(LPDWORD c) { if (c) *c = flags_ | DDPCAPS_8BIT; return DD_OK; }
HRESULT DDPalette::GetEntries(DWORD, DWORD base, DWORD n, LPPALETTEENTRY out) {
    if (!out || base + n > 256) return DDERR_INVALIDPARAMS;
    memcpy(out, entries_ + base, n * sizeof(PALETTEENTRY));
    return DD_OK;
}
HRESULT DDPalette::Initialize(LPDIRECTDRAW, DWORD, LPPALETTEENTRY) { return DDERR_ALREADYINITIALIZED; }
HRESULT DDPalette::SetEntries(DWORD, DWORD base, DWORD n, LPPALETTEENTRY in) {
    if (!in || base + n > 256) return DDERR_INVALIDPARAMS;
    bool isPrimary;
    {
        Lock_ l;
        memcpy(entries_ + base, in, n * sizeof(PALETTEENTRY));
        for (DWORD i = base; i < base + n; i++)
            rgbq_[i] = {entries_[i].peBlue, entries_[i].peGreen, entries_[i].peRed, 0};
        isPrimary = State().primaryPal == this;
        for (auto* s : State().surfaces)
            if (s->pal_ == this || (!s->pal_ && isPrimary)) s->SyncColorTable();
    }
    if (isPrimary) { MarkPrimaryDirty(0, 0, true); NotifyPrimaryChanged(false); }
    return DD_OK;
}

// ------------------------------------------------------------------ clipper
HRESULT DDClipper::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IDirectDrawClipper) { *ppv = this; AddRef(); return S_OK; }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG DDClipper::AddRef() { return InterlockedIncrement(&refs_); }
ULONG DDClipper::Release() {
    LONG r = InterlockedDecrement(&refs_);
    if (r == 0) {
        Lock_ l;
        for (auto* s : State().surfaces) if (s->clipper_ == this) s->clipper_ = nullptr;
        delete this;
    }
    return r;
}
HRESULT DDClipper::GetClipList(LPRECT, LPRGNDATA data, LPDWORD size) {
    // Report a single rect covering the game area.
    RECT r = {0, 0, State().modeW, State().modeH};
    DWORD need = sizeof(RGNDATAHEADER) + sizeof(RECT);
    if (!size) return DDERR_INVALIDPARAMS;
    if (!data) { *size = need; return DD_OK; }
    if (*size < need) { *size = need; return DDERR_REGIONTOOSMALL; }
    data->rdh.dwSize = sizeof(RGNDATAHEADER);
    data->rdh.iType = RDH_RECTANGLES;
    data->rdh.nCount = 1;
    data->rdh.nRgnSize = sizeof(RECT);
    data->rdh.rcBound = r;
    memcpy(data->Buffer, &r, sizeof(RECT));
    return DD_OK;
}
HRESULT DDClipper::GetHWnd(HWND* h) { if (h) *h = hwnd_; return DD_OK; }
HRESULT DDClipper::Initialize(LPDIRECTDRAW, DWORD) { return DDERR_ALREADYINITIALIZED; }
HRESULT DDClipper::IsClipListChanged(BOOL* b) { if (b) *b = FALSE; return DD_OK; }
HRESULT DDClipper::SetClipList(LPRGNDATA data, DWORD) {
    rects_.clear();
    if (data) {
        auto* r = (RECT*)data->Buffer;
        rects_.assign(r, r + data->rdh.nCount);
    }
    return DD_OK;
}
HRESULT DDClipper::SetHWnd(DWORD, HWND h) { hwnd_ = h; return DD_OK; }

// ------------------------------------------------------------------ surface
DDSurface::DDSurface(int w, int h, DWORD caps) : w_(w), h_(h), caps_(caps) {
    pitch_ = (w + 3) & ~3;
    struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } bmi = {};
    bmi.h.biSize = sizeof(BITMAPINFOHEADER);
    bmi.h.biWidth = pitch_;
    bmi.h.biHeight = -h; // top-down
    bmi.h.biPlanes = 1;
    bmi.h.biBitCount = 8;
    bmi.h.biCompression = BI_RGB;
    bmi.h.biClrUsed = 256;
    DDPalette* p = State().primaryPal;
    if (p) memcpy(bmi.c, p->rgbq_, sizeof(bmi.c));
    // The engine's sprite blitters read and write a few bytes outside the
    // surface (e.g. a 4-byte load/store for 3 pixels at the left edge of the
    // first row). Real DirectDraw memory has neighbours; a bare DIB starts on a
    // fresh page, so that byte faulted (crash in the shadow/colour-table blit).
    // The DIB therefore lives in a section with a page of padding in front of
    // the pixels and spare rows after them.
    const DWORD front = 4096, backRows = 4;
    const size_t bytes = (size_t)front + (size_t)pitch_ * ((h ? h : 1) + backRows);
    bmi.h.biHeight = -(LONG)((h ? h : 1) + backRows);
    dc_ = CreateCompatibleDC(nullptr);
    section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)((uint64_t)bytes >> 32), (DWORD)bytes, nullptr);
    if (section_) dib_ = CreateDIBSection(dc_, (BITMAPINFO*)&bmi, DIB_RGB_COLORS, (void**)&bits_, section_, front);
    if (dib_) {   // GDI maps the section from its start, so the padding is there
        MEMORY_BASIC_INFORMATION mi = {};
        if (!VirtualQuery(bits_ - 16, &mi, sizeof(mi)) || mi.State != MEM_COMMIT) {
            LOG("surface %dx%d: no memory before the pixels - using a plain DIB", w, h);
            DeleteObject(dib_); dib_ = nullptr;
        }
    }
    if (!dib_) {
        if (section_) { CloseHandle(section_); section_ = nullptr; }
        dib_ = CreateDIBSection(dc_, (BITMAPINFO*)&bmi, DIB_RGB_COLORS, (void**)&bits_, nullptr, 0);
    }
    if (!dib_) {
        LOG("CreateDIBSection failed %dx%d (err %lu) - falling back to heap", w, h, GetLastError());
        heap_ = (uint8_t*)calloc(bytes, 1);
        bits_ = heap_ + front;
    } else {
        oldBmp_ = SelectObject(dc_, dib_);
        memset(bits_, 0, (size_t)pitch_ * h);
    }
    Lock_ l;
    State().surfaces.push_back(this);
}

DDSurface::~DDSurface() {
    {
        Lock_ l;
        auto& v = State().surfaces;
        v.erase(std::remove(v.begin(), v.end(), this), v.end());
        if (State().primary == this) State().primary = nullptr;
    }
    if (pal_) pal_->Release();
    if (clipper_) clipper_->Release();
    if (dc_) { if (oldBmp_) SelectObject(dc_, oldBmp_); DeleteDC(dc_); }
    if (dib_) DeleteObject(dib_); else free(heap_);
    if (section_) CloseHandle(section_);
}

void DDSurface::SyncColorTable() {
    DDPalette* p = pal_ ? pal_ : State().primaryPal;
    if (p && dc_ && dib_) SetDIBColorTable(dc_, 0, 256, p->rgbq_);
}

void DDSurface::SwapContents(DDSurface* o) {
    std::swap(bits_, o->bits_);
    std::swap(dib_, o->dib_);
    std::swap(section_, o->section_);
    std::swap(heap_, o->heap_);
    std::swap(dc_, o->dc_);
    std::swap(oldBmp_, o->oldBmp_);
    std::swap(pitch_, o->pitch_);
}

DDSurface* SurfaceFromDC(HDC dc) {
    Lock_ l;
    for (auto* s : State().surfaces) if (s->dc_ == dc) return s;
    return nullptr;
}

HRESULT DDSurface::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IDirectDrawSurface) { *ppv = this; AddRef(); return S_OK; }
    LOG("Surface::QueryInterface unsupported {%08lx-...}", riid.Data1);
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG DDSurface::AddRef() { return InterlockedIncrement(&refs_); }
ULONG DDSurface::Release() {
    LONG r = InterlockedDecrement(&refs_);
    if (r == 0) {
        if (IsPrimary()) {
            // destroy implicit back buffers with the primary
            for (size_t i = 1; i < chain_.size(); i++) { chain_[i]->chainOwner_ = nullptr; delete chain_[i]; }
            chain_.clear();
        } else if (chainOwner_) {
            return 0; // back buffers are owned by the primary chain
        }
        delete this;
    }
    return r;
}

HRESULT DDSurface::AddAttachedSurface(LPDIRECTDRAWSURFACE s) {
    auto* o = (DDSurface*)s;
    if (!o) return DDERR_INVALIDPARAMS;
    o->AddRef();
    if (attached_) attached_->Release();
    attached_ = o;
    return DD_OK;
}
HRESULT DDSurface::AddOverlayDirtyRect(LPRECT) { return DDERR_UNSUPPORTED; }

static bool ClipBlt(RECT& d, RECT& s, int dw, int dh, int sw, int sh, bool stretch) {
    // Clip source to source bounds and destination to destination bounds,
    // adjusting the other rect proportionally.
    double sx = stretch ? (double)(s.right - s.left) / (d.right - d.left) : 1.0;
    double sy = stretch ? (double)(s.bottom - s.top) / (d.bottom - d.top) : 1.0;
    if (d.left < 0) { s.left += (LONG)(-d.left * sx); d.left = 0; }
    if (d.top < 0) { s.top += (LONG)(-d.top * sy); d.top = 0; }
    if (d.right > dw) { s.right -= (LONG)((d.right - dw) * sx); d.right = dw; }
    if (d.bottom > dh) { s.bottom -= (LONG)((d.bottom - dh) * sy); d.bottom = dh; }
    if (s.left < 0) { d.left += (LONG)(-s.left / sx); s.left = 0; }
    if (s.top < 0) { d.top += (LONG)(-s.top / sy); s.top = 0; }
    if (s.right > sw) { d.right -= (LONG)((s.right - sw) / sx); s.right = sw; }
    if (s.bottom > sh) { d.bottom -= (LONG)((s.bottom - sh) / sy); s.bottom = sh; }
    return d.right > d.left && d.bottom > d.top && s.right > s.left && s.bottom > s.top;
}

// ---- blit helpers (called with the surface lock held)

struct ColorKey { bool on = false; uint8_t lo = 0, hi = 0; };

// Plain 1:1 copy; an overlapping self-blit is handled by memmove + row order.
static void CopyRect(uint8_t* db, int dp, const RECT& d, const uint8_t* sb, int sp, const RECT& s, bool self) {
    const int dw = d.right - d.left, dh = d.bottom - d.top;
    const bool backwards = self && d.top > s.top;
    for (int i = 0; i < dh; i++) {
        int y = backwards ? dh - 1 - i : i;
        memmove(db + (size_t)(d.top + y) * dp + d.left, sb + (size_t)(s.top + y) * sp + s.left, dw);
    }
}

// 1:1 copy to the screen surface that writes only what changed and reports
// the changed rows (see CopyRowIfChanged).
// With the text layer, rows holding visible texts are copied with the texts
// replaced by their background (FloatText::Patch).
static std::vector<int>& CopyToScreen(uint8_t* db, int dp, const RECT& d, const uint8_t* sb, int sp, const RECT& s, bool patched) {
    static std::vector<int> rows;
    rows.clear();
    const int dw = d.right - d.left, dh = d.bottom - d.top;
    for (int y = 0; y < dh; y++) {
        const uint8_t* sr = sb + (size_t)(s.top + y) * sp + s.left;
        if (patched) sr = FloatText::Patch(d.top + y, sr, d.left, dw);
        if (CopyRowIfChanged(db + (size_t)(d.top + y) * dp + d.left, sr, dw)) rows.push_back(d.top + y);
    }
    return rows;
}

// Colour-keyed, stretched and/or mirrored copy. The source column of every
// destination column is computed once per blit (no per-pixel division).
static void CopyGeneral(DDSurface* dst, const RECT& d, DDSurface* src, const RECT& s, bool stretch, bool mirLR, bool mirUD, ColorKey key) {
    static std::vector<uint8_t> tmp;
    static std::vector<int> cols;
    const int dw = d.right - d.left, dh = d.bottom - d.top, sw = s.right - s.left, sh = s.bottom - s.top;
    const uint8_t* sb = src->bits_;
    const int sp = src->pitch_;
    if (src == dst) {   // overlapping: work from a copy of the source rows
        tmp.assign(src->bits_ + (size_t)s.top * sp, src->bits_ + (size_t)s.bottom * sp);
        sb = tmp.data() - (size_t)s.top * sp;
    }
    cols.resize(dw);
    for (int x = 0; x < dw; x++) {
        int sx = stretch ? s.left + (int)((int64_t)x * sw / dw) : s.left + x;
        cols[x] = mirLR ? s.right - 1 - (sx - s.left) : sx;
    }
    const bool plainCols = !stretch && !mirLR;
    for (int y = 0; y < dh; y++) {
        int sy = stretch ? s.top + (int)((int64_t)y * sh / dh) : s.top + y;
        if (mirUD) sy = s.bottom - 1 - (sy - s.top);
        const uint8_t* srow = sb + (size_t)sy * sp;
        uint8_t* drow = dst->bits_ + (size_t)(d.top + y) * dst->pitch_ + d.left;
        if (!key.on) {
            if (plainCols) memcpy(drow, srow + s.left, dw);
            else for (int x = 0; x < dw; x++) drow[x] = srow[cols[x]];
        } else if (plainCols && key.lo == key.hi) {
            const uint8_t* sr = srow + s.left;
            for (int x = 0; x < dw; x++) if (sr[x] != key.lo) drow[x] = sr[x];
        } else {
            for (int x = 0; x < dw; x++) {
                uint8_t v = srow[cols[x]];
                if (v < key.lo || v > key.hi) drow[x] = v;
            }
        }
    }
}

HRESULT DDSurface::Blt(LPRECT dstRect, LPDIRECTDRAWSURFACE srcI, LPRECT srcRect, DWORD flags, LPDDBLTFX fx) {
    auto* src = (DDSurface*)srcI;
    RECT d = dstRect ? *dstRect : RECT{0, 0, w_, h_};
    const std::vector<int>* exactRows = nullptr;   // screen blits: the rows that changed
    {
        Lock_ l;
        if (flags & DDBLT_COLORFILL) {
            uint8_t c = fx ? (uint8_t)fx->dwFillColor : 0;
            RECT s = d;
            if (!ClipBlt(d, s, w_, h_, w_, h_, false)) return DD_OK;
            for (LONG y = d.top; y < d.bottom; y++) memset(bits_ + (size_t)y * pitch_ + d.left, c, d.right - d.left);
        } else if (src) {
            RECT s = srcRect ? *srcRect : RECT{0, 0, src->w_, src->h_};
            bool stretch = (s.right - s.left) != (d.right - d.left) || (s.bottom - s.top) != (d.bottom - d.top);
            if (!ClipBlt(d, s, w_, h_, src->w_, src->h_, stretch)) return DD_OK;
            ColorKey key;
            if (flags & DDBLT_KEYSRCOVERRIDE && fx) {
                key = {true, (uint8_t)fx->ddckSrcColorkey.dwColorSpaceLowValue, (uint8_t)fx->ddckSrcColorkey.dwColorSpaceHighValue};
            } else if ((flags & DDBLT_KEYSRC) && src->hasSrcKey_) {
                key = {true, (uint8_t)src->srcKeyLo_, (uint8_t)src->srcKeyHi_};
            }
            if (key.hi < key.lo) key.hi = key.lo;
            bool mirLR = fx && (flags & DDBLT_DDFX) && (fx->dwDDFX & DDBLTFX_MIRRORLEFTRIGHT);
            bool mirUD = fx && (flags & DDBLT_DDFX) && (fx->dwDDFX & DDBLTFX_MIRRORUPDOWN);
            bool plain = !stretch && !key.on && !mirLR && !mirUD;
            if (plain && IsPrimary() && src != this) {
                // texts go to the text layer instead (HD text, zoom)
                bool patched = FloatText::Prepare(src, d, s, w_, h_);
                exactRows = &CopyToScreen(bits_, pitch_, d, src->bits_, src->pitch_, s, patched);
            }
            else {
                // texts drawn elsewhere move along with 1:1 copies (text layer)
                bool moving = !stretch && !mirLR && !mirUD && !IsPrimary() && FloatText::TransferBegin(src, this, d, s, key.on, key.lo, key.hi);
                if (plain) CopyRect(bits_, pitch_, d, src->bits_, src->pitch_, s, src == this);
                else CopyGeneral(this, d, src, s, stretch, mirLR, mirUD, key);
                if (moving) FloatText::TransferEnd(this);
            }
        } else {
            return DDERR_INVALIDPARAMS;
        }
    }
    // Genie does not page-flip: it composes into an offscreen surface and blits
    // it to the primary. A blit covering most of the primary is a frame
    // boundary, treated like Flip() for pacing.
    if (IsPrimary()) {
        bool frame = (int64_t)(d.right - d.left) * (d.bottom - d.top) * 2 >= (int64_t)w_ * h_;
        if (h_ > 600) LogStackScan(frame ? "blt-frame" : "blt-partial");
        if (exactRows) { if (!exactRows->empty()) MarkPrimaryRows(*exactRows, h_); }
        else MarkPrimaryDirty(d.top, d.bottom, false);
        NotifyPrimaryChanged(frame);   // also for unchanged frames: paces the game loop
    }
    return DD_OK;
}

HRESULT DDSurface::BltBatch(LPDDBLTBATCH, DWORD, DWORD) { return DDERR_UNSUPPORTED; }

HRESULT DDSurface::BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE src, LPRECT srcRect, DWORD flags) {
    auto* s = (DDSurface*)src;
    if (!s) return DDERR_INVALIDPARAMS;
    RECT sr = srcRect ? *srcRect : RECT{0, 0, s->w_, s->h_};
    RECT dr = {(LONG)x, (LONG)y, (LONG)x + (sr.right - sr.left), (LONG)y + (sr.bottom - sr.top)};
    DDBLTFX fx = {};
    fx.dwSize = sizeof(fx);
    DWORD bf = 0;
    if (flags & DDBLTFAST_SRCCOLORKEY) bf |= DDBLT_KEYSRC;
    return Blt(&dr, src, &sr, bf, &fx);
}

HRESULT DDSurface::DeleteAttachedSurface(DWORD, LPDIRECTDRAWSURFACE s) {
    if (attached_ && (!s || (DDSurface*)s == attached_) && !attached_->chainOwner_) {
        attached_->Release();
        attached_ = nullptr;
    }
    return DD_OK;
}
HRESULT DDSurface::EnumAttachedSurfaces(LPVOID ctx, LPDDENUMSURFACESCALLBACK cb) {
    if (attached_ && cb) {
        DDSURFACEDESC d;
        attached_->FillDesc(&d);
        attached_->AddRef();
        cb((LPDIRECTDRAWSURFACE)attached_, &d, ctx);
    }
    return DD_OK;
}
HRESULT DDSurface::EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK) { return DDERR_UNSUPPORTED; }

HRESULT DDSurface::Flip(LPDIRECTDRAWSURFACE, DWORD) {
    if (!IsPrimary() || chain_.size() < 2) return DDERR_NOTFLIPPABLE;
    {
        Lock_ l;
        // rotate contents: front <- back1 <- back2 ... <- front
        for (size_t i = 0; i + 1 < chain_.size(); i++) chain_[i]->SwapContents(chain_[i + 1]);
        for (auto* s : chain_) s->SyncColorTable();
    }
    MarkPrimaryDirty(0, h_, false);
    NotifyPrimaryChanged(true);
    return DD_OK;
}

HRESULT DDSurface::GetAttachedSurface(LPDDSCAPS caps, LPDIRECTDRAWSURFACE* out) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (attached_) {
        attached_->AddRef();
        *out = attached_;
        return DD_OK;
    }
    *out = nullptr;
    LOG("GetAttachedSurface: none (caps %08lx)", caps ? caps->dwCaps : 0);
    return DDERR_NOTFOUND;
}
HRESULT DDSurface::GetBltStatus(DWORD) { return DD_OK; }
HRESULT DDSurface::GetCaps(LPDDSCAPS c) { if (!c) return DDERR_INVALIDPARAMS; c->dwCaps = caps_; return DD_OK; }
HRESULT DDSurface::GetClipper(LPDIRECTDRAWCLIPPER* c) {
    if (!c) return DDERR_INVALIDPARAMS;
    *c = clipper_;
    if (!clipper_) return DDERR_NOCLIPPERATTACHED;
    clipper_->AddRef();
    return DD_OK;
}
HRESULT DDSurface::GetColorKey(DWORD flags, LPDDCOLORKEY k) {
    if (!k) return DDERR_INVALIDPARAMS;
    if ((flags & DDCKEY_SRCBLT) && hasSrcKey_) { k->dwColorSpaceLowValue = srcKeyLo_; k->dwColorSpaceHighValue = srcKeyHi_; return DD_OK; }
    return DDERR_NOCOLORKEY;
}
HRESULT DDSurface::GetDC(HDC* out) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (!dc_) return DDERR_CANTCREATEDC;
    SyncColorTable();
    dcOut_++;
    *out = dc_;
    return DD_OK;
}
HRESULT DDSurface::GetFlipStatus(DWORD) { return DD_OK; }
HRESULT DDSurface::GetOverlayPosition(LPLONG, LPLONG) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT DDSurface::GetPalette(LPDIRECTDRAWPALETTE* p) {
    if (!p) return DDERR_INVALIDPARAMS;
    *p = pal_;
    if (!pal_) return DDERR_NOPALETTEATTACHED;
    pal_->AddRef();
    return DD_OK;
}
static void Fill8bpp(DDPIXELFORMAT* pf) {
    memset(pf, 0, sizeof(*pf));
    pf->dwSize = sizeof(DDPIXELFORMAT);
    pf->dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
    pf->dwRGBBitCount = 8;
}
HRESULT DDSurface::GetPixelFormat(LPDDPIXELFORMAT pf) { if (!pf) return DDERR_INVALIDPARAMS; Fill8bpp(pf); return DD_OK; }
void DDSurface::FillDesc(DDSURFACEDESC* d) const {
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(DDSURFACEDESC);
    d->dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    d->dwWidth = w_;
    d->dwHeight = h_;
    d->lPitch = pitch_;
    d->ddsCaps.dwCaps = caps_;
    if (hasSrcKey_) {
        d->dwFlags |= DDSD_CKSRCBLT;
        d->ddckCKSrcBlt.dwColorSpaceLowValue = srcKeyLo_;
        d->ddckCKSrcBlt.dwColorSpaceHighValue = srcKeyHi_;
    }
    if (chain_.size() > 1) { d->dwFlags |= DDSD_BACKBUFFERCOUNT; d->dwBackBufferCount = (DWORD)chain_.size() - 1; }
    Fill8bpp(&d->ddpfPixelFormat);
}
HRESULT DDSurface::GetSurfaceDesc(LPDDSURFACEDESC d) {
    if (!d) return DDERR_INVALIDPARAMS;
    DWORD sz = d->dwSize ? d->dwSize : sizeof(DDSURFACEDESC);
    DDSURFACEDESC t;
    FillDesc(&t);
    memcpy(d, &t, std::min<DWORD>(sz, sizeof(t)));
    d->dwSize = sz;
    return DD_OK;
}
HRESULT DDSurface::Initialize(LPDIRECTDRAW, LPDDSURFACEDESC) { return DDERR_ALREADYINITIALIZED; }
HRESULT DDSurface::IsLost() { return DD_OK; }
HRESULT DDSurface::Lock(LPRECT r, LPDDSURFACEDESC d, DWORD, HANDLE) {
    if (!d) return DDERR_INVALIDPARAMS;
    DWORD sz = d->dwSize ? d->dwSize : sizeof(DDSURFACEDESC);
    DDSURFACEDESC t;
    FillDesc(&t);
    t.dwFlags |= DDSD_LPSURFACE;
    t.lpSurface = bits_ + (r ? r->top * pitch_ + r->left : 0);
    memcpy(d, &t, std::min<DWORD>(sz, sizeof(t)));
    d->dwSize = sz;
    return DD_OK;
}
HRESULT DDSurface::ReleaseDC(HDC) {
    GdiFlush();
    if (dcOut_ > 0) dcOut_--;
    if (IsPrimary()) { MarkPrimaryDirty(0, h_, false); NotifyPrimaryChanged(false); }
    return DD_OK;
}
HRESULT DDSurface::Restore() { return DD_OK; }
HRESULT DDSurface::SetClipper(LPDIRECTDRAWCLIPPER c) {
    if (c) c->AddRef();
    if (clipper_) clipper_->Release();
    clipper_ = (DDClipper*)c;
    return DD_OK;
}
HRESULT DDSurface::SetColorKey(DWORD flags, LPDDCOLORKEY k) {
    if (flags & DDCKEY_SRCBLT) {
        if (k) { hasSrcKey_ = true; srcKeyLo_ = k->dwColorSpaceLowValue; srcKeyHi_ = (flags & DDCKEY_COLORSPACE) ? k->dwColorSpaceHighValue : k->dwColorSpaceLowValue; }
        else hasSrcKey_ = false;
    }
    return DD_OK;
}
HRESULT DDSurface::SetOverlayPosition(LONG, LONG) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT DDSurface::SetPalette(LPDIRECTDRAWPALETTE p) {
    {
        Lock_ l;
        if (p) p->AddRef();
        if (pal_) pal_->Release();
        pal_ = (DDPalette*)p;
        if (IsPrimary()) {
            State().primaryPal = pal_;
            for (auto* s : State().surfaces) s->SyncColorTable();
        } else {
            SyncColorTable();
        }
    }
    if (IsPrimary()) { MarkPrimaryDirty(0, h_, true); NotifyPrimaryChanged(false); }
    return DD_OK;
}
HRESULT DDSurface::Unlock(LPVOID) {
    if (IsPrimary()) { MarkPrimaryDirty(0, h_, false); NotifyPrimaryChanged(false); }
    return DD_OK;
}
HRESULT DDSurface::UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDOVERLAYFX) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT DDSurface::UpdateOverlayDisplay(DWORD) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT DDSurface::UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE) { return DDERR_NOTAOVERLAYSURFACE; }

bool SnapshotPrimary(Frame& out) {
    Lock_ l;
    auto& st = State();
    DDSurface* p = st.primary;
    if (!p || !p->bits_) return false;
    bool resized = out.width != p->w_ || out.height != p->h_;
    out.width = p->w_;
    out.height = p->h_;
    int y0 = std::max(0, st.dirtyY0), y1 = std::min(p->h_, st.dirtyY1);
    if (resized) { out.pixels.assign((size_t)p->w_ * p->h_, 0); y0 = 0; y1 = p->h_; }
    // The engine re-blits the whole back buffer even when nothing moved, so
    // compare with the previous snapshot and keep only rows that really changed.
    // Rows a screen blit reported as changed are copied directly.
    int c0 = INT_MAX, c1 = 0;
    bool exact = (int)st.rowsChanged.size() == p->h_;
    int e0 = exact ? std::max(0, st.changedY0) : INT_MAX, e1 = exact ? std::min(p->h_, st.changedY1) : 0;
    for (int y = std::min(y0, e0); y < std::max(y1, e1); y++) {
        uint8_t* dst = out.pixels.data() + (size_t)y * p->w_;
        const uint8_t* src = p->bits_ + (size_t)y * p->pitch_;
        bool known = exact && y >= e0 && y < e1 && st.rowsChanged[y];
        if (known) st.rowsChanged[y] = 0;
        else if (y < y0 || y >= y1 || (!resized && memcmp(dst, src, p->w_) == 0)) continue;
        memcpy(dst, src, p->w_);
        c0 = std::min(c0, y); c1 = y + 1;
    }
    st.changedY0 = 1 << 30; st.changedY1 = 0;
    out.dirtyY0 = c0 < c1 ? c0 : 0;
    out.dirtyY1 = c0 < c1 ? c1 : 0;
    out.paletteChanged = st.paletteDirty || resized;
    if (out.paletteChanged) {
        DDPalette* pal = st.primaryPal;
        for (int i = 0; i < 256; i++)
            out.palette[i] = pal ? (pal->entries_[i].peRed << 16) | (pal->entries_[i].peGreen << 8) | pal->entries_[i].peBlue : i * 0x010101;
    }
    st.dirtyY0 = 1 << 30;
    st.dirtyY1 = 0;
    st.paletteDirty = false;
    return true;
}

// ------------------------------------------------------------------ ddraw
DDraw::DDraw() {}
HRESULT DDraw::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IDirectDraw) { *ppv = this; AddRef(); return S_OK; }
    LOG("DDraw::QueryInterface unsupported {%08lx-...}", riid.Data1);
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG DDraw::AddRef() { return InterlockedIncrement(&refs_); }
ULONG DDraw::Release() {
    LONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return r;
}
HRESULT DDraw::Compact() { return DD_OK; }
HRESULT DDraw::CreateClipper(DWORD, LPDIRECTDRAWCLIPPER* out, IUnknown*) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = new DDClipper();
    return DD_OK;
}
HRESULT DDraw::CreatePalette(DWORD flags, LPPALETTEENTRY e, LPDIRECTDRAWPALETTE* out, IUnknown*) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = new DDPalette(flags, e);
    LOG("CreatePalette flags=%08lx", flags);
    return DD_OK;
}
HRESULT DDraw::CreateSurface(LPDDSURFACEDESC d, LPDIRECTDRAWSURFACE* out, IUnknown*) {
    if (!d || !out) return DDERR_INVALIDPARAMS;
    DWORD caps = (d->dwFlags & DDSD_CAPS) ? d->ddsCaps.dwCaps : 0;
    auto& st = State();
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        int backs = (d->dwFlags & DDSD_BACKBUFFERCOUNT) ? (int)d->dwBackBufferCount : 0;
        auto* prim = new DDSurface(st.modeW, st.modeH, caps | DDSCAPS_VISIBLE | DDSCAPS_FRONTBUFFER | DDSCAPS_VIDEOMEMORY);
        prim->chain_.push_back(prim);
        DDSurface* prev = prim;
        for (int i = 0; i < backs; i++) {
            DWORD bc = (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE)) | DDSCAPS_VIDEOMEMORY;
            if (i == 0) bc |= DDSCAPS_BACKBUFFER;
            auto* b = new DDSurface(st.modeW, st.modeH, bc);
            b->chainOwner_ = prim;
            prim->chain_.push_back(b);
            prev->attached_ = b;
            prev = b;
        }
        {
            Lock_ l;
            st.primary = prim;
            st.dirtyY0 = 0; st.dirtyY1 = 1 << 30; st.paletteDirty = true;
        }
        LOG("CreateSurface primary %dx%d backbuffers=%d caps=%08lx", st.modeW, st.modeH, backs, caps);
        *out = prim;
        return DD_OK;
    }
    if (!(d->dwFlags & DDSD_WIDTH) || !(d->dwFlags & DDSD_HEIGHT)) return DDERR_INVALIDPARAMS;
    if ((d->dwFlags & DDSD_PIXELFORMAT) && d->ddpfPixelFormat.dwRGBBitCount != 8) {
        LOG("CreateSurface: non-8bpp format requested (%lu bpp) - unsupported", d->ddpfPixelFormat.dwRGBBitCount);
        return DDERR_INVALIDPIXELFORMAT;
    }
    if (!(caps & (DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY))) caps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
    auto* s = new DDSurface((int)d->dwWidth, (int)d->dwHeight, caps | DDSCAPS_OFFSCREENPLAIN);
    if (d->dwFlags & DDSD_CKSRCBLT) {
        s->hasSrcKey_ = true;
        s->srcKeyLo_ = d->ddckCKSrcBlt.dwColorSpaceLowValue;
        s->srcKeyHi_ = d->ddckCKSrcBlt.dwColorSpaceHighValue;
    }
    *out = s;
    return DD_OK;
}
HRESULT DDraw::DuplicateSurface(LPDIRECTDRAWSURFACE src, LPDIRECTDRAWSURFACE* out) {
    auto* s = (DDSurface*)src;
    if (!s || !out) return DDERR_INVALIDPARAMS;
    auto* d = new DDSurface(s->w_, s->h_, s->caps_ & ~DDSCAPS_PRIMARYSURFACE);
    memcpy(d->bits_, s->bits_, (size_t)s->pitch_ * s->h_);
    d->hasSrcKey_ = s->hasSrcKey_; d->srcKeyLo_ = s->srcKeyLo_; d->srcKeyHi_ = s->srcKeyHi_;
    *out = d;
    return DD_OK;
}

static void ModeDesc(DDSURFACEDESC* d, int w, int h, int bpp, int hz) {
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
    d->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    d->dwWidth = w;
    d->dwHeight = h;
    d->lPitch = w * bpp / 8;
    d->dwRefreshRate = hz;
    d->ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    d->ddpfPixelFormat.dwRGBBitCount = bpp;
    if (bpp == 8) d->ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
    else {
        d->ddpfPixelFormat.dwFlags = DDPF_RGB;
        if (bpp == 16) { d->ddpfPixelFormat.dwRBitMask = 0xF800; d->ddpfPixelFormat.dwGBitMask = 0x07E0; d->ddpfPixelFormat.dwBBitMask = 0x001F; }
        else { d->ddpfPixelFormat.dwRBitMask = 0xFF0000; d->ddpfPixelFormat.dwGBitMask = 0xFF00; d->ddpfPixelFormat.dwBBitMask = 0xFF; }
    }
}

HRESULT DDraw::EnumDisplayModes(DWORD, LPDDSURFACEDESC filter, LPVOID ctx, LPDDENUMMODESCALLBACK cb) {
    if (!cb) return DDERR_INVALIDPARAMS;
    std::vector<std::pair<int, int>> modes = {{640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 720},
                                              {1280, 1024}, {1366, 768}, {1600, 900}, {1600, 1200}, {1920, 1080}};
    int cw = Zoom::InternalW(), ch = Zoom::InternalH();
    if (cw <= 0 || ch <= 0) { cw = CfgInt("Display", "GameWidth", 0); ch = CfgInt("Display", "GameHeight", 0); }
    if (cw > 0 && ch > 0) modes.push_back({cw, ch});
    for (int bpp : {8, 16, 32}) {
        for (auto& m : modes) {
            DDSURFACEDESC d;
            ModeDesc(&d, m.first, m.second, bpp, 60);
            if (filter) {
                if ((filter->dwFlags & DDSD_WIDTH) && filter->dwWidth != d.dwWidth) continue;
                if ((filter->dwFlags & DDSD_HEIGHT) && filter->dwHeight != d.dwHeight) continue;
                if ((filter->dwFlags & DDSD_PIXELFORMAT) && filter->ddpfPixelFormat.dwRGBBitCount != (DWORD)bpp) continue;
            }
            if (cb(&d, ctx) == DDENUMRET_CANCEL) return DD_OK;
        }
    }
    return DD_OK;
}
HRESULT DDraw::EnumSurfaces(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) { return DD_OK; }
HRESULT DDraw::FlipToGDISurface() { return DD_OK; }
HRESULT DDraw::GetCaps(LPDDCAPS hal, LPDDCAPS hel) {
    DDCAPS c;
    memset(&c, 0, sizeof(c));
    c.dwSize = sizeof(c);
    c.dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_PALETTE | DDCAPS_CANBLTSYSMEM;
    c.dwCaps2 = DDCAPS2_WIDESURFACES | DDCAPS2_NOPAGELOCKREQUIRED;
    c.dwCKeyCaps = DDCKEYCAPS_SRCBLT;
    c.dwFXCaps = DDFXCAPS_BLTMIRRORLEFTRIGHT | DDFXCAPS_BLTMIRRORUPDOWN | DDFXCAPS_BLTSTRETCHX | DDFXCAPS_BLTSTRETCHY;
    c.dwPalCaps = DDPCAPS_8BIT | DDPCAPS_PRIMARYSURFACE;
    c.dwVidMemTotal = 128u << 20;
    c.dwVidMemFree = 120u << 20;
    c.ddsCaps.dwCaps = DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER | DDSCAPS_OFFSCREENPLAIN |
                       DDSCAPS_PALETTE | DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY;
    for (LPDDCAPS t : {hal, hel}) {
        if (!t) continue;
        DWORD sz = t->dwSize ? t->dwSize : sizeof(DDCAPS);
        memcpy(t, &c, std::min<DWORD>(sz, sizeof(c)));
        t->dwSize = sz;
    }
    return DD_OK;
}
HRESULT DDraw::GetDisplayMode(LPDDSURFACEDESC d) {
    if (!d) return DDERR_INVALIDPARAMS;
    DWORD sz = d->dwSize ? d->dwSize : sizeof(DDSURFACEDESC);
    DDSURFACEDESC t;
    ModeDesc(&t, State().modeW, State().modeH, State().modeBpp, 60);
    memcpy(d, &t, std::min<DWORD>(sz, sizeof(t)));
    d->dwSize = sz;
    return DD_OK;
}
HRESULT DDraw::GetFourCCCodes(LPDWORD n, LPDWORD) { if (n) *n = 0; return DD_OK; }
HRESULT DDraw::GetGDISurface(LPDIRECTDRAWSURFACE* out) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = State().primary;
    if (!*out) return DDERR_NOTFOUND;
    State().primary->AddRef();
    return DD_OK;
}
HRESULT DDraw::GetMonitorFrequency(LPDWORD f) { if (f) *f = 60; return DD_OK; }
HRESULT DDraw::GetScanLine(LPDWORD l) { if (l) *l = 0; return DDERR_VERTICALBLANKINPROGRESS; }
HRESULT DDraw::GetVerticalBlankStatus(LPBOOL b) { if (b) *b = TRUE; return DD_OK; }
HRESULT DDraw::Initialize(GUID*) { return DDERR_ALREADYINITIALIZED; }
HRESULT DDraw::RestoreDisplayMode() { return DD_OK; }
HRESULT DDraw::SetCooperativeLevel(HWND hwnd, DWORD flags) {
    LOG("SetCooperativeLevel hwnd=%p flags=%08lx", hwnd, flags);
    if (hwnd && hwnd != State().hwnd) {
        State().hwnd = hwnd;
        Window::Attach(hwnd);
    }
    return DD_OK;
}
HRESULT DDraw::SetMode(DWORD w, DWORD h, DWORD bpp) {
    LOG("SetDisplayMode %lux%lux%lu", w, h, bpp);
    if (bpp != 8) LOG("  warning: non-8bpp mode requested; surfaces stay 8bpp");
    State().modeW = (int)w;
    State().modeH = (int)h;
    State().modeBpp = (int)bpp;
    Window::SetGameSize((int)w, (int)h);
    Presenter::Get().Start(State().hwnd);
    // First mode set happens during startup, before the menu: play the intros.
    Video::PlayIntros(State().hwnd);
    return DD_OK;
}
HRESULT DDraw::SetDisplayMode(DWORD w, DWORD h, DWORD bpp) { return SetMode(w, h, bpp); }
HRESULT DDraw::WaitForVerticalBlank(DWORD, HANDLE) { return DD_OK; }

} // namespace swgbm
