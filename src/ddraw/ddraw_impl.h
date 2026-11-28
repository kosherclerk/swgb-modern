#pragma once
#include "../common/common.h"
#include <ddraw.h>
#include <vector>

namespace swgbm {

class DDPalette;
class DDClipper;

// Emulated 8-bit DirectDraw surface backed by a DIB section so the game can
// draw text into it with GDI (GetDC).
class DDSurface : public IDirectDrawSurface {
public:
    DDSurface(int w, int h, DWORD caps);
    virtual ~DDSurface();

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    // IDirectDrawSurface
    STDMETHOD(AddAttachedSurface)(LPDIRECTDRAWSURFACE) override;
    STDMETHOD(AddOverlayDirtyRect)(LPRECT) override;
    STDMETHOD(Blt)(LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDBLTFX) override;
    STDMETHOD(BltBatch)(LPDDBLTBATCH, DWORD, DWORD) override;
    STDMETHOD(BltFast)(DWORD, DWORD, LPDIRECTDRAWSURFACE, LPRECT, DWORD) override;
    STDMETHOD(DeleteAttachedSurface)(DWORD, LPDIRECTDRAWSURFACE) override;
    STDMETHOD(EnumAttachedSurfaces)(LPVOID, LPDDENUMSURFACESCALLBACK) override;
    STDMETHOD(EnumOverlayZOrders)(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK) override;
    STDMETHOD(Flip)(LPDIRECTDRAWSURFACE, DWORD) override;
    STDMETHOD(GetAttachedSurface)(LPDDSCAPS, LPDIRECTDRAWSURFACE*) override;
    STDMETHOD(GetBltStatus)(DWORD) override;
    STDMETHOD(GetCaps)(LPDDSCAPS) override;
    STDMETHOD(GetClipper)(LPDIRECTDRAWCLIPPER*) override;
    STDMETHOD(GetColorKey)(DWORD, LPDDCOLORKEY) override;
    STDMETHOD(GetDC)(HDC*) override;
    STDMETHOD(GetFlipStatus)(DWORD) override;
    STDMETHOD(GetOverlayPosition)(LPLONG, LPLONG) override;
    STDMETHOD(GetPalette)(LPDIRECTDRAWPALETTE*) override;
    STDMETHOD(GetPixelFormat)(LPDDPIXELFORMAT) override;
    STDMETHOD(GetSurfaceDesc)(LPDDSURFACEDESC) override;
    STDMETHOD(Initialize)(LPDIRECTDRAW, LPDDSURFACEDESC) override;
    STDMETHOD(IsLost)() override;
    STDMETHOD(Lock)(LPRECT, LPDDSURFACEDESC, DWORD, HANDLE) override;
    STDMETHOD(ReleaseDC)(HDC) override;
    STDMETHOD(Restore)() override;
    STDMETHOD(SetClipper)(LPDIRECTDRAWCLIPPER) override;
    STDMETHOD(SetColorKey)(DWORD, LPDDCOLORKEY) override;
    STDMETHOD(SetOverlayPosition)(LONG, LONG) override;
    STDMETHOD(SetPalette)(LPDIRECTDRAWPALETTE) override;
    STDMETHOD(Unlock)(LPVOID) override;
    STDMETHOD(UpdateOverlay)(LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDOVERLAYFX) override;
    STDMETHOD(UpdateOverlayDisplay)(DWORD) override;
    STDMETHOD(UpdateOverlayZOrder)(DWORD, LPDIRECTDRAWSURFACE) override;

    // internals
    void FillDesc(DDSURFACEDESC* d) const;
    void SyncColorTable();
    bool IsPrimary() const { return (caps_ & DDSCAPS_PRIMARYSURFACE) != 0; }
    void SwapContents(DDSurface* other);

    LONG refs_ = 1;
    int w_, h_, pitch_;
    DWORD caps_;
    uint8_t* bits_ = nullptr;
    HBITMAP dib_ = nullptr;
    HANDLE section_ = nullptr;   // memory behind the DIB (padded, see the constructor)
    uint8_t* heap_ = nullptr;    // fallback allocation (padded)
    HGDIOBJ oldBmp_ = nullptr;
    HDC dc_ = nullptr;
    int dcOut_ = 0;
    DDPalette* pal_ = nullptr;
    DDClipper* clipper_ = nullptr;
    bool hasSrcKey_ = false;
    DWORD srcKeyLo_ = 0, srcKeyHi_ = 0;
    DDSurface* attached_ = nullptr;   // next surface in the flip chain (back buffer)
    DDSurface* chainOwner_ = nullptr; // primary that owns this back buffer
    std::vector<DDSurface*> chain_;   // primary: [front, back1, back2...]
};

class DDPalette : public IDirectDrawPalette {
public:
    explicit DDPalette(DWORD flags, const PALETTEENTRY* entries);
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(GetCaps)(LPDWORD) override;
    STDMETHOD(GetEntries)(DWORD, DWORD, DWORD, LPPALETTEENTRY) override;
    STDMETHOD(Initialize)(LPDIRECTDRAW, DWORD, LPPALETTEENTRY) override;
    STDMETHOD(SetEntries)(DWORD, DWORD, DWORD, LPPALETTEENTRY) override;

    LONG refs_ = 1;
    DWORD flags_;
    PALETTEENTRY entries_[256];
    RGBQUAD rgbq_[256];
};

class DDClipper : public IDirectDrawClipper {
public:
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(GetClipList)(LPRECT, LPRGNDATA, LPDWORD) override;
    STDMETHOD(GetHWnd)(HWND*) override;
    STDMETHOD(Initialize)(LPDIRECTDRAW, DWORD) override;
    STDMETHOD(IsClipListChanged)(BOOL*) override;
    STDMETHOD(SetClipList)(LPRGNDATA, DWORD) override;
    STDMETHOD(SetHWnd)(DWORD, HWND) override;
    LONG refs_ = 1;
    HWND hwnd_ = nullptr;
    std::vector<RECT> rects_;
};

class DDraw : public IDirectDraw {
public:
    DDraw();
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Compact)() override;
    STDMETHOD(CreateClipper)(DWORD, LPDIRECTDRAWCLIPPER*, IUnknown*) override;
    STDMETHOD(CreatePalette)(DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE*, IUnknown*) override;
    STDMETHOD(CreateSurface)(LPDDSURFACEDESC, LPDIRECTDRAWSURFACE*, IUnknown*) override;
    STDMETHOD(DuplicateSurface)(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE*) override;
    STDMETHOD(EnumDisplayModes)(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMMODESCALLBACK) override;
    STDMETHOD(EnumSurfaces)(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) override;
    STDMETHOD(FlipToGDISurface)() override;
    STDMETHOD(GetCaps)(LPDDCAPS, LPDDCAPS) override;
    STDMETHOD(GetDisplayMode)(LPDDSURFACEDESC) override;
    STDMETHOD(GetFourCCCodes)(LPDWORD, LPDWORD) override;
    STDMETHOD(GetGDISurface)(LPDIRECTDRAWSURFACE*) override;
    STDMETHOD(GetMonitorFrequency)(LPDWORD) override;
    STDMETHOD(GetScanLine)(LPDWORD) override;
    STDMETHOD(GetVerticalBlankStatus)(LPBOOL) override;
    STDMETHOD(Initialize)(GUID*) override;
    STDMETHOD(RestoreDisplayMode)() override;
    STDMETHOD(SetCooperativeLevel)(HWND, DWORD) override;
    STDMETHOD(SetDisplayMode)(DWORD, DWORD, DWORD) override;
    STDMETHOD(WaitForVerticalBlank)(DWORD, HANDLE) override;

    HRESULT SetMode(DWORD w, DWORD h, DWORD bpp);
    LONG refs_ = 1;
};

// Global emulation state
struct DDState {
    CRITICAL_SECTION lock;      // guards primary contents/palette for the presenter
    HWND hwnd = nullptr;
    int modeW = 800, modeH = 600, modeBpp = 8;
    DDSurface* primary = nullptr;
    std::vector<DDSurface*> surfaces;
    DDPalette* primaryPal = nullptr;
    // change tracking for the presenter (guarded by lock)
    int dirtyY0 = 0, dirtyY1 = 1 << 30;      // rows to compare with the last snapshot
    std::vector<uint8_t> rowsChanged;        // rows known to have changed (no compare needed)
    int changedY0 = 1 << 30, changedY1 = 0;  // range of rowsChanged that may be set
    bool paletteDirty = true;
    volatile LONG version = 0;
};
DDState& State();

// Copy the current front buffer (+palette) for presentation. Returns false if
// there is nothing to show yet.
// Copies changed rows of the front buffer (+palette) into `out` and sets its
// dirty range. Returns false if there is no primary surface yet.
bool SnapshotPrimary(struct Frame& out);
DDSurface* SurfaceFromDC(HDC dc);
void MarkPrimaryDirty(int y0, int y1, bool palette);

} // namespace swgbm
