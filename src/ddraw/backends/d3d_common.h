// Helpers shared by the Direct3D 10/11/12 backends.
#pragma once
#include "../renderer.h"
#include <dxgi1_6.h>
#include <d3d11.h>
#include <d3dcompiler.h>

namespace swgbm {

template <class T> inline void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct ShaderBlobs {
    ID3DBlob* vs = nullptr;
    ID3DBlob* psPal = nullptr;
    ID3DBlob* psScale = nullptr;
    ID3DBlob* psOverlay = nullptr;
    ~ShaderBlobs() { SafeRelease(vs); SafeRelease(psPal); SafeRelease(psScale); SafeRelease(psOverlay); }
};

inline bool CompileHlsl(ShaderBlobs& out, const char* vsProfile, const char* psProfile) {
    static pD3DCompile compile = [] {
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!m) m = LoadLibraryW(L"d3dcompiler_43.dll");
        return m ? (pD3DCompile)GetProcAddress(m, "D3DCompile") : nullptr;
    }();
    if (!compile) { LOG("d3dcompiler not available"); return false; }
    auto one = [&](const char* entry, const char* profile, ID3DBlob** blob) {
        ID3DBlob* err = nullptr;
        HRESULT hr = compile(g_hlslSource, strlen(g_hlslSource), "swgbm.hlsl", nullptr, nullptr, entry, profile,
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &err);
        if (FAILED(hr)) {
            LOG("HLSL compile %s (%s) failed: %s", entry, profile, err ? (const char*)err->GetBufferPointer() : "?");
            SafeRelease(err);
            return false;
        }
        SafeRelease(err);
        return true;
    };
    return one("VSMain", vsProfile, &out.vs) && one("PSPalette", psProfile, &out.psPal) && one("PSScale", psProfile, &out.psScale) &&
           one("PSOverlay", psProfile, &out.psOverlay);
}

inline ScaleConstants MakeScaleConstants(const Frame& f, const Region& r, const PresentOptions& opt) {
    ScaleConstants c = {};
    c.srcSize[0] = (float)f.width; c.srcSize[1] = (float)f.height;
    c.srcSize[2] = 1.0f / f.width; c.srcSize[3] = 1.0f / f.height;
    c.dstSize[0] = (float)r.w; c.dstSize[1] = (float)r.h;
    c.dstSize[2] = 1.0f / (r.w ? r.w : 1); c.dstSize[3] = 1.0f / (r.h ? r.h : 1);
    c.filter = (int)r.filter;
    c.sharpness = opt.sharpness;
    c.uvRect[0] = r.u0; c.uvRect[1] = r.v0; c.uvRect[2] = r.u1 - r.u0; c.uvRect[3] = r.v1 - r.v0;
    return c;
}

// Flip-model swapchain helper. `device` is the D3D10/11 device or the D3D12
// command queue.
struct DxgiSwap {
    IDXGISwapChain1* swap = nullptr;
    IDXGIFactory2* factory = nullptr;
    HANDLE latencyWait = nullptr;   // frame-latency waitable object (flip model)
    HWND hwnd = nullptr;
    int w = 0, h = 0;
    UINT buffers = 2;
    bool tearing = false;
    bool flipModel = true;
    UINT flags = 0;

    ~DxgiSwap() { if (latencyWait) CloseHandle(latencyWait); SafeRelease(swap); SafeRelease(factory); }
    // The waitable object is a semaphore released once per completed present:
    // consume it at most once per Present() or skipped frames would stall.
    bool waited = false;
    void WaitForNextFrame() {
        if (latencyWait && !waited) { WaitForSingleObjectEx(latencyWait, 100, TRUE); waited = true; }
    }

    bool CreateSwapChain(IUnknown* device, HWND wnd, UINT bufferCount = 2, bool requireFlip = false) {
        bool allowTearing = CfgBool("Display", "AllowTearing", true);
        hwnd = wnd;
        buffers = bufferCount;
        HMODULE m = LoadLibraryW(L"dxgi.dll");
        auto cf = m ? (HRESULT(WINAPI*)(UINT, REFIID, void**))GetProcAddress(m, "CreateDXGIFactory2") : nullptr;
        if (cf) cf(0, IID_IDXGIFactory2, (void**)&factory);
        if (!factory) {
            auto cf1 = m ? (HRESULT(WINAPI*)(REFIID, void**))GetProcAddress(m, "CreateDXGIFactory1") : nullptr;
            if (cf1) cf1(IID_IDXGIFactory2, (void**)&factory);
        }
        if (!factory) { LOG("DXGI: no IDXGIFactory2"); return false; }
        IDXGIFactory5* f5 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(IID_IDXGIFactory5, (void**)&f5))) {
            BOOL allow = FALSE;
            if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) tearing = allow != 0 && allowTearing;
            f5->Release();
        }
        RECT rc;
        GetClientRect(wnd, &rc);
        w = rc.right > 0 ? rc.right : 1;
        h = rc.bottom > 0 ? rc.bottom : 1;
        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width = w; d.Height = h;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = buffers;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        flags = (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0) | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        d.Flags = flags;
        HRESULT hr = factory->CreateSwapChainForHwnd(device, wnd, &d, nullptr, nullptr, &swap);
        if (FAILED(hr)) { d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; hr = factory->CreateSwapChainForHwnd(device, wnd, &d, nullptr, nullptr, &swap); }
        if (FAILED(hr)) { // older systems: no waitable object
            d.Flags = flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
            d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            hr = factory->CreateSwapChainForHwnd(device, wnd, &d, nullptr, nullptr, &swap);
        }
        if (FAILED(hr) && !requireFlip) {
            d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; d.BufferCount = 1; d.Flags = flags = 0; tearing = false; flipModel = false;
            hr = factory->CreateSwapChainForHwnd(device, wnd, &d, nullptr, nullptr, &swap);
        }
        if (FAILED(hr)) { LOG("DXGI CreateSwapChainForHwnd failed %08lx", hr); return false; }
        factory->MakeWindowAssociation(wnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        if (flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
            IDXGISwapChain2* s2 = nullptr;
            if (SUCCEEDED(swap->QueryInterface(IID_IDXGISwapChain2, (void**)&s2))) {
                s2->SetMaximumFrameLatency(1);
                latencyWait = s2->GetFrameLatencyWaitableObject();
                s2->Release();
            }
        }
        LOG("DXGI swapchain %dx%d flip=%d tearing=%d low-latency=%d", w, h, flipModel, tearing, latencyWait != nullptr);
        return true;
    }
    bool NeedsResize(int cw, int ch) const { return cw != w || ch != h; }
    bool Resize(int cw, int ch) {
        HRESULT hr = swap->ResizeBuffers(0, cw, ch, DXGI_FORMAT_UNKNOWN, flags);
        if (FAILED(hr)) { LOG("ResizeBuffers failed %08lx", hr); return false; }
        w = cw; h = ch;
        return true;
    }
    bool Present(bool vsync) {
        UINT pf = (!vsync && tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
        HRESULT hr = swap->Present(vsync ? 1 : 0, pf);
        waited = false;
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            // the removal reason tells a driver reset (TDR, GPU busy elsewhere) from a real fault
            HRESULT why = hr;
            IUnknown* dev = nullptr;
            ID3D11Device* d11 = nullptr;
            if (SUCCEEDED(swap->GetDevice(__uuidof(IUnknown), (void**)&dev)) && dev) {
                if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device), (void**)&d11)) && d11) { why = d11->GetDeviceRemovedReason(); d11->Release(); }
                dev->Release();
            }
            LOG("DXGI device lost %08lx (reason %08lx); recreating the renderer", hr, why);
            return false;
        }
        return true;
    }
};

} // namespace swgbm
