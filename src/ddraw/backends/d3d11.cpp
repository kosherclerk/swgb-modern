// Direct3D 11 backend (feature level 10_0+). DXGI flip-model swapchain.
#include "../renderer.h"
#include "d3d_common.h"
#include <d3d11.h>

namespace swgbm {
namespace {

class D3D11Renderer : public Renderer {
public:
    const char* Name() const override { return "Direct3D 11"; }
    ~D3D11Renderer() override {
        if (ctx_) ctx_->ClearState();
        SafeRelease(cb_); SafeRelease(samp_); SafeRelease(vs_); SafeRelease(psPal_); SafeRelease(psScale_);
        SafeRelease(idxSrv_); SafeRelease(idxTex_); SafeRelease(palSrv_); SafeRelease(palTex_);
        SafeRelease(frameRtv_); SafeRelease(frameSrv_); SafeRelease(frameTex_); SafeRelease(bbRtv_);
        SafeRelease(swap_); SafeRelease(ctx_); SafeRelease(dev_);
    }

    bool Init(HWND hwnd) override {
        hwnd_ = hwnd;
        HMODULE m = LoadLibraryW(L"d3d11.dll");
        if (!m) return false;
        auto create = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(m, "D3D11CreateDevice");
        if (!create) return false;
        D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, 4,
                            D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
        if (FAILED(hr)) hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls + 1, 3,
                                    D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
        if (FAILED(hr)) { LOG("D3D11CreateDevice failed %08lx", hr); return false; }
        if (!dxgi_.CreateSwapChain(dev_, hwnd)) return false;
        swap_ = dxgi_.swap;
        swap_->AddRef();

        ShaderBlobs sb;
        if (!CompileHlsl(sb, "vs_4_0", "ps_4_0")) return false;
        if (FAILED(dev_->CreateVertexShader(sb.vs->GetBufferPointer(), sb.vs->GetBufferSize(), nullptr, &vs_))) return false;
        if (FAILED(dev_->CreatePixelShader(sb.psPal->GetBufferPointer(), sb.psPal->GetBufferSize(), nullptr, &psPal_))) return false;
        if (FAILED(dev_->CreatePixelShader(sb.psScale->GetBufferPointer(), sb.psScale->GetBufferSize(), nullptr, &psScale_))) return false;

        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev_->CreateSamplerState(&sd, &samp_);
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(ScaleConstants);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev_->CreateBuffer(&bd, nullptr, &cb_);

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = 256; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &palTex_))) return false;
        dev_->CreateShaderResourceView(palTex_, nullptr, &palSrv_);
        return true;
    }

    bool Present(const Frame& f, const Viewport& vp, int cw, int ch, const PresentOptions& opt) override {
        if (cw <= 0 || ch <= 0) return true;
        if (f.width != fw_ || f.height != fh_) { if (!CreateFrameResources(f.width, f.height)) return false; }
        if (dxgi_.NeedsResize(cw, ch)) {
            SafeRelease(bbRtv_);
            ctx_->OMSetRenderTargets(0, nullptr, nullptr);
            if (!dxgi_.Resize(cw, ch)) return false;
        }
        if (!bbRtv_) {
            ID3D11Texture2D* bb = nullptr;
            if (FAILED(swap_->GetBuffer(0, IID_ID3D11Texture2D, (void**)&bb))) return false;
            dev_->CreateRenderTargetView(bb, nullptr, &bbRtv_);
            bb->Release();
        }
        // upload
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(ctx_->Map(idxTex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            for (int y = 0; y < f.height; y++) memcpy((uint8_t*)ms.pData + y * ms.RowPitch, f.pixels.data() + (size_t)y * f.width, f.width);
            ctx_->Unmap(idxTex_, 0);
        }
        if (SUCCEEDED(ctx_->Map(palTex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            memcpy(ms.pData, f.palette, 1024);
            ctx_->Unmap(palTex_, 0);
        }
        ScaleConstants sc = MakeScaleConstants(f, vp, opt);
        if (SUCCEEDED(ctx_->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, &sc, sizeof(sc)); ctx_->Unmap(cb_, 0); }

        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->IASetInputLayout(nullptr);
        ctx_->VSSetShader(vs_, nullptr, 0);
        // pass 1: palette resolve
        ID3D11ShaderResourceView* null3[3] = {};
        ctx_->PSSetShaderResources(0, 3, null3);
        ctx_->OMSetRenderTargets(1, &frameRtv_, nullptr);
        D3D11_VIEWPORT v1 = {0, 0, (float)f.width, (float)f.height, 0, 1};
        ctx_->RSSetViewports(1, &v1);
        ID3D11ShaderResourceView* s1[2] = {idxSrv_, palSrv_};
        ctx_->PSSetShaderResources(0, 2, s1);
        ctx_->PSSetShader(psPal_, nullptr, 0);
        ctx_->Draw(3, 0);
        // pass 2: scale
        ctx_->PSSetShaderResources(0, 3, null3);
        ctx_->OMSetRenderTargets(1, &bbRtv_, nullptr);
        float black[4] = {0, 0, 0, 1};
        ctx_->ClearRenderTargetView(bbRtv_, black);
        D3D11_VIEWPORT v2 = {(float)vp.x, (float)vp.y, (float)vp.w, (float)vp.h, 0, 1};
        ctx_->RSSetViewports(1, &v2);
        ctx_->PSSetShaderResources(2, 1, &frameSrv_);
        ctx_->PSSetSamplers(0, 1, &samp_);
        ctx_->PSSetConstantBuffers(0, 1, &cb_);
        ctx_->PSSetShader(psScale_, nullptr, 0);
        ctx_->Draw(3, 0);
        return dxgi_.Present(opt.vsync);
    }

private:
    bool CreateFrameResources(int w, int h) {
        SafeRelease(idxSrv_); SafeRelease(idxTex_); SafeRelease(frameRtv_); SafeRelease(frameSrv_); SafeRelease(frameTex_);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R8_UINT; td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &idxTex_))) return false;
        dev_->CreateShaderResourceView(idxTex_, nullptr, &idxSrv_);
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.Usage = D3D11_USAGE_DEFAULT; td.CPUAccessFlags = 0;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &frameTex_))) return false;
        dev_->CreateShaderResourceView(frameTex_, nullptr, &frameSrv_);
        dev_->CreateRenderTargetView(frameTex_, nullptr, &frameRtv_);
        fw_ = w; fh_ = h;
        return true;
    }

    HWND hwnd_ = nullptr;
    DxgiSwap dxgi_;
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGISwapChain* swap_ = nullptr;
    ID3D11RenderTargetView* bbRtv_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* psPal_ = nullptr;
    ID3D11PixelShader* psScale_ = nullptr;
    ID3D11SamplerState* samp_ = nullptr;
    ID3D11Buffer* cb_ = nullptr;
    ID3D11Texture2D* idxTex_ = nullptr;
    ID3D11ShaderResourceView* idxSrv_ = nullptr;
    ID3D11Texture2D* palTex_ = nullptr;
    ID3D11ShaderResourceView* palSrv_ = nullptr;
    ID3D11Texture2D* frameTex_ = nullptr;
    ID3D11ShaderResourceView* frameSrv_ = nullptr;
    ID3D11RenderTargetView* frameRtv_ = nullptr;
    int fw_ = 0, fh_ = 0;
};

} // namespace

std::unique_ptr<Renderer> CreateD3D11Renderer() { return std::make_unique<D3D11Renderer>(); }

} // namespace swgbm
