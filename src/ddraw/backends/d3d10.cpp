// Direct3D 10.1 backend (for DX10-class GPUs / users who want the DX10 path).
#include "../renderer.h"
#include "d3d_common.h"
#include <d3d10_1.h>

namespace swgbm {
namespace {

class D3D10Renderer : public Renderer {
public:
    const char* Name() const override { return "Direct3D 10.1"; }
    ~D3D10Renderer() override {
        if (dev_) dev_->ClearState();
        SafeRelease(cb_); SafeRelease(samp_); SafeRelease(vs_); SafeRelease(psPal_); SafeRelease(psScale_);
        SafeRelease(idxSrv_); SafeRelease(idxTex_); SafeRelease(palSrv_); SafeRelease(palTex_);
        SafeRelease(frameRtv_); SafeRelease(frameSrv_); SafeRelease(frameTex_); SafeRelease(bbRtv_);
        SafeRelease(dev_);
    }

    bool Init(HWND hwnd) override {
        HMODULE m = LoadLibraryW(L"d3d10_1.dll");
        if (!m) return false;
        typedef HRESULT(WINAPI * PFN)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, D3D10_FEATURE_LEVEL1, UINT, ID3D10Device1**);
        auto create = (PFN)GetProcAddress(m, "D3D10CreateDevice1");
        if (!create) return false;
        HRESULT hr = create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, D3D10_CREATE_DEVICE_BGRA_SUPPORT,
                            D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION, &dev_);
        if (FAILED(hr)) hr = create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, D3D10_CREATE_DEVICE_BGRA_SUPPORT,
                                    D3D10_FEATURE_LEVEL_10_0, D3D10_1_SDK_VERSION, &dev_);
        if (FAILED(hr)) { LOG("D3D10CreateDevice1 failed %08lx", hr); return false; }
        if (!dxgi_.CreateSwapChain(dev_, hwnd)) return false;

        ShaderBlobs sb;
        if (!CompileHlsl(sb, "vs_4_0", "ps_4_0")) return false;
        if (FAILED(dev_->CreateVertexShader(sb.vs->GetBufferPointer(), sb.vs->GetBufferSize(), &vs_))) return false;
        if (FAILED(dev_->CreatePixelShader(sb.psPal->GetBufferPointer(), sb.psPal->GetBufferSize(), &psPal_))) return false;
        if (FAILED(dev_->CreatePixelShader(sb.psScale->GetBufferPointer(), sb.psScale->GetBufferSize(), &psScale_))) return false;

        D3D10_SAMPLER_DESC sd = {};
        sd.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D10_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D10_FLOAT32_MAX;
        dev_->CreateSamplerState(&sd, &samp_);
        D3D10_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(ScaleConstants);
        bd.Usage = D3D10_USAGE_DYNAMIC;
        bd.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
        dev_->CreateBuffer(&bd, nullptr, &cb_);
        D3D10_TEXTURE2D_DESC td = {};
        td.Width = 256; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D10_USAGE_DYNAMIC; td.BindFlags = D3D10_BIND_SHADER_RESOURCE; td.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &palTex_))) return false;
        dev_->CreateShaderResourceView(palTex_, nullptr, &palSrv_);
        return true;
    }

    bool Present(const Frame& f, const Viewport& vp, int cw, int ch, const PresentOptions& opt) override {
        if (cw <= 0 || ch <= 0) return true;
        if (f.width != fw_ || f.height != fh_) { if (!CreateFrameResources(f.width, f.height)) return false; }
        if (dxgi_.NeedsResize(cw, ch)) {
            SafeRelease(bbRtv_);
            dev_->OMSetRenderTargets(0, nullptr, nullptr);
            if (!dxgi_.Resize(cw, ch)) return false;
        }
        if (!bbRtv_) {
            ID3D10Texture2D* bb = nullptr;
            if (FAILED(dxgi_.swap->GetBuffer(0, IID_ID3D10Texture2D, (void**)&bb))) return false;
            dev_->CreateRenderTargetView(bb, nullptr, &bbRtv_);
            bb->Release();
        }
        D3D10_MAPPED_TEXTURE2D mt;
        if (SUCCEEDED(idxTex_->Map(0, D3D10_MAP_WRITE_DISCARD, 0, &mt))) {
            for (int y = 0; y < f.height; y++) memcpy((uint8_t*)mt.pData + y * mt.RowPitch, f.pixels.data() + (size_t)y * f.width, f.width);
            idxTex_->Unmap(0);
        }
        if (SUCCEEDED(palTex_->Map(0, D3D10_MAP_WRITE_DISCARD, 0, &mt))) { memcpy(mt.pData, f.palette, 1024); palTex_->Unmap(0); }
        ScaleConstants sc = MakeScaleConstants(f, vp, opt);
        void* p;
        if (SUCCEEDED(cb_->Map(D3D10_MAP_WRITE_DISCARD, 0, &p))) { memcpy(p, &sc, sizeof(sc)); cb_->Unmap(); }

        dev_->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        dev_->IASetInputLayout(nullptr);
        dev_->VSSetShader(vs_);
        ID3D10ShaderResourceView* null3[3] = {};
        dev_->PSSetShaderResources(0, 3, null3);
        dev_->OMSetRenderTargets(1, &frameRtv_, nullptr);
        D3D10_VIEWPORT v1 = {0, 0, (UINT)f.width, (UINT)f.height, 0, 1};
        dev_->RSSetViewports(1, &v1);
        ID3D10ShaderResourceView* s1[2] = {idxSrv_, palSrv_};
        dev_->PSSetShaderResources(0, 2, s1);
        dev_->PSSetShader(psPal_);
        dev_->Draw(3, 0);
        dev_->PSSetShaderResources(0, 3, null3);
        dev_->OMSetRenderTargets(1, &bbRtv_, nullptr);
        float black[4] = {0, 0, 0, 1};
        dev_->ClearRenderTargetView(bbRtv_, black);
        D3D10_VIEWPORT v2 = {vp.x, vp.y, (UINT)vp.w, (UINT)vp.h, 0, 1};
        dev_->RSSetViewports(1, &v2);
        dev_->PSSetShaderResources(2, 1, &frameSrv_);
        dev_->PSSetSamplers(0, 1, &samp_);
        dev_->PSSetConstantBuffers(0, 1, &cb_);
        dev_->PSSetShader(psScale_);
        dev_->Draw(3, 0);
        return dxgi_.Present(opt.vsync);
    }

private:
    bool CreateFrameResources(int w, int h) {
        SafeRelease(idxSrv_); SafeRelease(idxTex_); SafeRelease(frameRtv_); SafeRelease(frameSrv_); SafeRelease(frameTex_);
        D3D10_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R8_UINT; td.Usage = D3D10_USAGE_DYNAMIC;
        td.BindFlags = D3D10_BIND_SHADER_RESOURCE; td.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &idxTex_))) return false;
        dev_->CreateShaderResourceView(idxTex_, nullptr, &idxSrv_);
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.Usage = D3D10_USAGE_DEFAULT; td.CPUAccessFlags = 0;
        td.BindFlags = D3D10_BIND_SHADER_RESOURCE | D3D10_BIND_RENDER_TARGET;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &frameTex_))) return false;
        dev_->CreateShaderResourceView(frameTex_, nullptr, &frameSrv_);
        dev_->CreateRenderTargetView(frameTex_, nullptr, &frameRtv_);
        fw_ = w; fh_ = h;
        return true;
    }

    DxgiSwap dxgi_;
    ID3D10Device1* dev_ = nullptr;
    ID3D10RenderTargetView* bbRtv_ = nullptr;
    ID3D10VertexShader* vs_ = nullptr;
    ID3D10PixelShader* psPal_ = nullptr;
    ID3D10PixelShader* psScale_ = nullptr;
    ID3D10SamplerState* samp_ = nullptr;
    ID3D10Buffer* cb_ = nullptr;
    ID3D10Texture2D* idxTex_ = nullptr;
    ID3D10ShaderResourceView* idxSrv_ = nullptr;
    ID3D10Texture2D* palTex_ = nullptr;
    ID3D10ShaderResourceView* palSrv_ = nullptr;
    ID3D10Texture2D* frameTex_ = nullptr;
    ID3D10ShaderResourceView* frameSrv_ = nullptr;
    ID3D10RenderTargetView* frameRtv_ = nullptr;
    int fw_ = 0, fh_ = 0;
};

} // namespace

std::unique_ptr<Renderer> CreateD3D10Renderer() { return std::make_unique<D3D10Renderer>(); }

} // namespace swgbm
