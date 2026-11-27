// Direct3D 12 backend. One frame in flight (the game produces at most one
// frame per present anyway), flip-model swapchain with 3 buffers.
#include "../renderer.h"
#include "d3d_common.h"
#include <d3d12.h>

namespace swgbm {
namespace {

constexpr UINT kBackBuffers = 3;

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br = {};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return br;
}

class D3D12Renderer : public Renderer {
public:
    const char* Name() const override { return "Direct3D 12"; }
    ~D3D12Renderer() override {
        WaitIdle();
        ReleaseBackBuffers();
        ReleaseFrame();
        SafeRelease(palTex_); SafeRelease(psoPal_); SafeRelease(psoScale_); SafeRelease(psoOverlay_); SafeRelease(rootSig_);
        SafeRelease(ovTex_);
        for (int i = 0; i < kInFlight; i++) { if (ovUpload_[i]) ovUpload_[i]->Unmap(0, nullptr); SafeRelease(ovUpload_[i]); }
        SafeRelease(list_); for (auto& a : alloc_) SafeRelease(a); SafeRelease(fence_); SafeRelease(rtvHeap_); SafeRelease(srvHeap_);
        SafeRelease(swap3_);
        if (fenceEvent_) CloseHandle(fenceEvent_);
        SafeRelease(queue_); SafeRelease(dev_);
    }

    bool Init(HWND hwnd) override {
        HMODULE m = LoadLibraryW(L"d3d12.dll");
        if (!m) return false;
        auto create = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(m, "D3D12CreateDevice");
        serialize_ = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)GetProcAddress(m, "D3D12SerializeRootSignature");
        if (!create || !serialize_) return false;
        HRESULT hr = create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device, (void**)&dev_);
        if (FAILED(hr)) { LOG("D3D12CreateDevice failed %08lx", hr); return false; }
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev_->CreateCommandQueue(&qd, IID_ID3D12CommandQueue, (void**)&queue_))) return false;
        if (!dxgi_.CreateSwapChain(queue_, hwnd, kBackBuffers, true)) return false;
        if (FAILED(dxgi_.swap->QueryInterface(IID_IDXGISwapChain3, (void**)&swap3_))) return false;

        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = kBackBuffers + 1;
        if (FAILED(dev_->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap, (void**)&rtvHeap_))) return false;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 5;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev_->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap, (void**)&srvHeap_))) return false;
        rtvInc_ = dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        srvInc_ = dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        for (auto& a : alloc_)
            if (FAILED(dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator, (void**)&a))) return false;
        if (FAILED(dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_[0], nullptr, IID_ID3D12GraphicsCommandList, (void**)&list_))) return false;
        list_->Close();
        if (FAILED(dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, (void**)&fence_))) return false;
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        if (!CreatePipeline()) return false;
        // palette texture (persistent)
        palTex_ = CreateTex(256, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (!palTex_) return false;
        WriteSrv(1, palTex_, DXGI_FORMAT_B8G8R8A8_UNORM);
        return AcquireBackBuffers();
    }

    bool Present(const Frame& f, const Viewport& vp, int cw, int ch, const PresentOptions& opt) override {
        if (cw <= 0 || ch <= 0) return true;
        if (f.width != fw_ || f.height != fh_) { WaitIdle(); if (!CreateFrame(f.width, f.height)) return false; }
        if (dxgi_.NeedsResize(cw, ch)) {
            WaitIdle();
            ReleaseBackBuffers();
            if (!dxgi_.Resize(cw, ch) || !AcquireBackBuffers()) return false;
        }
        // two frames in flight: wait only for the frame that used this slot
        cur_ = (cur_ + 1) % kInFlight;
        if (slotFence_[cur_] && fence_->GetCompletedValue() < slotFence_[cur_]) {
            fence_->SetEventOnCompletion(slotFence_[cur_], fenceEvent_);
            WaitForSingleObject(fenceEvent_, 2000);
        }
        int y0 = allDirty_ ? 0 : f.dirtyY0, y1 = allDirty_ ? f.height : f.dirtyY1;
        bool pal = allDirty_ || f.paletteChanged;
        uint8_t* up = uploadPtr_[cur_];
        for (int y = y0; y < y1; y++) memcpy(up + idxFoot_.Offset + (size_t)y * idxFoot_.Footprint.RowPitch, f.pixels.data() + (size_t)y * f.width, f.width);
        if (pal) memcpy(up + palFoot_.Offset, f.palette, 1024);

        alloc_[cur_]->Reset();
        list_->Reset(alloc_[cur_], nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.pResource = upload_[cur_];
        if (y1 > y0) {
            D3D12_RESOURCE_BARRIER b = Transition(idxTex_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            list_->ResourceBarrier(1, &b);
            dst.pResource = idxTex_; src.PlacedFootprint = idxFoot_;
            D3D12_BOX box = {0, (UINT)y0, 0, (UINT)f.width, (UINT)y1, 1};
            list_->CopyTextureRegion(&dst, 0, (UINT)y0, 0, &src, &box);
            b = Transition(idxTex_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            list_->ResourceBarrier(1, &b);
        }
        if (pal) {
            D3D12_RESOURCE_BARRIER b = Transition(palTex_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            list_->ResourceBarrier(1, &b);
            dst.pResource = palTex_; src.PlacedFootprint = palFoot_;
            list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            b = Transition(palTex_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            list_->ResourceBarrier(1, &b);
        }
        allDirty_ = false;
        D3D12_RESOURCE_BARRIER b2 = Transition(frameTex_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list_->ResourceBarrier(1, &b2);

        list_->SetGraphicsRootSignature(rootSig_);
        list_->SetDescriptorHeaps(1, &srvHeap_);
        list_->SetGraphicsRootDescriptorTable(0, srvHeap_->GetGPUDescriptorHandleForHeapStart());
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        // pass 1
        D3D12_CPU_DESCRIPTOR_HANDLE frameRtv = Rtv(kBackBuffers);
        list_->OMSetRenderTargets(1, &frameRtv, FALSE, nullptr);
        D3D12_VIEWPORT v1 = {0, 0, (float)f.width, (float)f.height, 0, 1};
        D3D12_RECT s1 = {0, 0, f.width, f.height};
        list_->RSSetViewports(1, &v1);
        list_->RSSetScissorRects(1, &s1);
        list_->SetPipelineState(psoPal_);
        list_->DrawInstanced(3, 1, 0, 0);
        // pass 2
        UINT bi = swap3_->GetCurrentBackBufferIndex();
        D3D12_RESOURCE_BARRIER b3[2] = {Transition(frameTex_, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                                        Transition(bb_[bi], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET)};
        list_->ResourceBarrier(2, b3);
        D3D12_CPU_DESCRIPTOR_HANDLE bbRtv = Rtv(bi);
        list_->OMSetRenderTargets(1, &bbRtv, FALSE, nullptr);
        float black[4] = {0, 0, 0, 1};
        list_->ClearRenderTargetView(bbRtv, black, 0, nullptr);
        D3D12_VIEWPORT v2 = {(float)vp.x, (float)vp.y, (float)vp.w, (float)vp.h, 0, 1};
        D3D12_RECT s2 = {vp.x, vp.y, vp.x + vp.w, vp.y + vp.h};
        list_->RSSetViewports(1, &v2);
        list_->RSSetScissorRects(1, &s2);
        ScaleConstants sc = MakeScaleConstants(f, vp, opt);
        list_->SetGraphicsRoot32BitConstants(1, sizeof(sc) / 4, &sc, 0);
        list_->SetPipelineState(psoScale_);
        if (!(overlay_ && overlay_->visible && overlay_->hideFrame)) list_->DrawInstanced(3, 1, 0, 0);
        DrawOverlay(f, vp);
        D3D12_RESOURCE_BARRIER b4 = Transition(bb_[bi], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        list_->ResourceBarrier(1, &b4);
        if (FAILED(list_->Close())) return false;
        ID3D12CommandList* lists[] = {list_};
        queue_->ExecuteCommandLists(1, lists);
        bool ok = dxgi_.Present(opt.vsync);
        slotFence_[cur_] = ++fenceValue_;
        queue_->Signal(fence_, slotFence_[cur_]);
        return ok;
    }
    void WaitForNextFrame() override { dxgi_.WaitForNextFrame(); }
    // AMD's 32-bit D3D12 driver busy-polls when the queue goes idle; a steady
    // present cadence keeps it parked (measured: menu CPU 100% -> see log).
    bool SkipIdenticalFrames() const override { return CfgBool("Display", "D3D12SteadyPresent", true) ? false : true; }

private:
    // Overlay: default-heap texture updated from a per-frame upload buffer.
    void DrawOverlay(const Frame& f, const Viewport& vp) {
        const Overlay* o = overlay_;
        if (!o || !o->visible || o->width <= 0 || o->height <= 0) return;
        bool full = false;
        if (o->width != ovW_ || o->height != ovH_) {
            WaitIdle();
            SafeRelease(ovTex_);
            for (int i = 0; i < kInFlight; i++) { if (ovUpload_[i]) ovUpload_[i]->Unmap(0, nullptr); SafeRelease(ovUpload_[i]); ovPtr_[i] = nullptr; }
            ovTex_ = CreateTex(o->width, o->height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (!ovTex_) return;
            WriteSrv(3, ovTex_, DXGI_FORMAT_B8G8R8A8_UNORM);
            D3D12_RESOURCE_DESC desc = ovTex_->GetDesc();
            UINT64 sz = 0;
            dev_->GetCopyableFootprints(&desc, 0, 1, 0, &ovFoot_, nullptr, nullptr, &sz);
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd = {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = sz; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
            bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            for (int i = 0; i < kInFlight; i++) {
                if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_ID3D12Resource, (void**)&ovUpload_[i]))) return;
                D3D12_RANGE none = {0, 0};
                ovUpload_[i]->Map(0, &none, (void**)&ovPtr_[i]);
            }
            ovW_ = o->width; ovH_ = o->height; full = true;
        }
        int y0 = full ? 0 : o->dirtyY0, y1 = full ? o->height : o->dirtyY1;
        if (y1 > y0) {
            for (int y = y0; y < y1; y++)
                memcpy(ovPtr_[cur_] + ovFoot_.Offset + (size_t)y * ovFoot_.Footprint.RowPitch, o->pixels.data() + (size_t)y * o->width, (size_t)o->width * 4);
            D3D12_RESOURCE_BARRIER b = Transition(ovTex_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            list_->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.pResource = ovTex_;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.pResource = ovUpload_[cur_]; src.PlacedFootprint = ovFoot_;
            D3D12_BOX box = {0, (UINT)y0, 0, (UINT)o->width, (UINT)y1, 1};
            list_->CopyTextureRegion(&dst, 0, (UINT)y0, 0, &src, &box);
            b = Transition(ovTex_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            list_->ResourceBarrier(1, &b);
            // copies need the render target re-bound? no: RT binding survives barriers on other resources
        }
        float x, y, w, h;
        OverlayRect(*o, f, vp, x, y, w, h);
        D3D12_VIEWPORT v = {x, y, w, h, 0, 1};
        D3D12_RECT sr = {(LONG)x, (LONG)y, (LONG)(x + w + 0.5f), (LONG)(y + h + 0.5f)};
        list_->RSSetViewports(1, &v);
        list_->RSSetScissorRects(1, &sr);
        list_->SetPipelineState(psoOverlay_);
        list_->DrawInstanced(3, 1, 0, 0);
    }

    void WaitIdle() {
        if (!queue_ || !fence_) return;
        UINT64 v = ++fenceValue_;
        queue_->Signal(fence_, v);
        if (fence_->GetCompletedValue() < v) {
            fence_->SetEventOnCompletion(v, fenceEvent_);
            WaitForSingleObject(fenceEvent_, 2000);
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * rtvInc_;
        return h;
    }
    void WriteSrv(UINT slot, ID3D12Resource* r, DXGI_FORMAT fmt) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = fmt;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)slot * srvInc_;
        dev_->CreateShaderResourceView(r, &sd, h);
    }
    ID3D12Resource* CreateTex(int w, int h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = fmt; rd.SampleDesc.Count = 1; rd.Flags = flags;
        ID3D12Resource* r = nullptr;
        D3D12_CLEAR_VALUE cv = {};
        cv.Format = fmt;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state,
                                                 (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? &cv : nullptr,
                                                 IID_ID3D12Resource, (void**)&r)))
            return nullptr;
        return r;
    }
    void ReleaseFrame() {
        SafeRelease(idxTex_); SafeRelease(frameTex_);
        for (int i = 0; i < kInFlight; i++) { if (upload_[i]) upload_[i]->Unmap(0, nullptr); SafeRelease(upload_[i]); uploadPtr_[i] = nullptr; }
    }
    bool CreateFrame(int w, int h) {
        ReleaseFrame();
        idxTex_ = CreateTex(w, h, DXGI_FORMAT_R8_UINT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        frameTex_ = CreateTex(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (!idxTex_ || !frameTex_) return false;
        WriteSrv(0, idxTex_, DXGI_FORMAT_R8_UINT);
        WriteSrv(2, frameTex_, DXGI_FORMAT_R8G8B8A8_UNORM);
        dev_->CreateRenderTargetView(frameTex_, nullptr, Rtv(kBackBuffers));
        // upload buffer: index footprint followed by palette footprint
        D3D12_RESOURCE_DESC idesc = idxTex_->GetDesc(), pdesc = palTex_->GetDesc();
        UINT64 isz = 0, psz = 0;
        dev_->GetCopyableFootprints(&idesc, 0, 1, 0, &idxFoot_, nullptr, nullptr, &isz);
        UINT64 palOff = (isz + 511) & ~511ull;
        dev_->GetCopyableFootprints(&pdesc, 0, 1, palOff, &palFoot_, nullptr, nullptr, &psz);
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = palOff + psz; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (int i = 0; i < kInFlight; i++) {
            if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_ID3D12Resource, (void**)&upload_[i])))
                return false;
            D3D12_RANGE none = {0, 0};
            if (FAILED(upload_[i]->Map(0, &none, (void**)&uploadPtr_[i]))) return false; // persistently mapped
        }
        allDirty_ = true;
        fw_ = w; fh_ = h;
        return true;
    }
    void ReleaseBackBuffers() { for (auto& b : bb_) SafeRelease(b); }
    bool AcquireBackBuffers() {
        for (UINT i = 0; i < kBackBuffers; i++) {
            if (FAILED(dxgi_.swap->GetBuffer(i, IID_ID3D12Resource, (void**)&bb_[i]))) return false;
            dev_->CreateRenderTargetView(bb_[i], nullptr, Rtv(i));
        }
        return true;
    }
    bool CreatePipeline() {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 4;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &range;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.Num32BitValues = sizeof(ScaleConstants) / 4;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC ss = {};
        ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.MaxLOD = D3D12_FLOAT32_MAX;
        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 2; rs.pParameters = params;
        rs.NumStaticSamplers = 1; rs.pStaticSamplers = &ss;
        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        if (FAILED(serialize_(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
            LOG("D3D12 root signature: %s", err ? (const char*)err->GetBufferPointer() : "?");
            SafeRelease(err);
            return false;
        }
        HRESULT hr = dev_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_ID3D12RootSignature, (void**)&rootSig_);
        SafeRelease(blob); SafeRelease(err);
        if (FAILED(hr)) return false;

        ShaderBlobs sb;
        if (!CompileHlsl(sb, "vs_5_0", "ps_5_0")) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rootSig_;
        pd.VS = {sb.vs->GetBufferPointer(), sb.vs->GetBufferSize()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.SampleDesc.Count = 1;
        pd.PS = {sb.psPal->GetBufferPointer(), sb.psPal->GetBufferSize()};
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (FAILED(dev_->CreateGraphicsPipelineState(&pd, IID_ID3D12PipelineState, (void**)&psoPal_))) return false;
        pd.PS = {sb.psScale->GetBufferPointer(), sb.psScale->GetBufferSize()};
        pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (FAILED(dev_->CreateGraphicsPipelineState(&pd, IID_ID3D12PipelineState, (void**)&psoScale_))) return false;
        pd.PS = {sb.psOverlay->GetBufferPointer(), sb.psOverlay->GetBufferSize()};
        auto& bt = pd.BlendState.RenderTarget[0];
        bt.BlendEnable = TRUE; bt.SrcBlend = D3D12_BLEND_ONE; bt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; bt.BlendOp = D3D12_BLEND_OP_ADD;
        bt.SrcBlendAlpha = D3D12_BLEND_ONE; bt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; bt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        bt.LogicOp = D3D12_LOGIC_OP_NOOP;
        if (FAILED(dev_->CreateGraphicsPipelineState(&pd, IID_ID3D12PipelineState, (void**)&psoOverlay_))) return false;
        return true;
    }

    DxgiSwap dxgi_;
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize_ = nullptr;
    ID3D12Device* dev_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    IDXGISwapChain3* swap3_ = nullptr;
    ID3D12DescriptorHeap* rtvHeap_ = nullptr;
    ID3D12DescriptorHeap* srvHeap_ = nullptr;
    UINT rtvInc_ = 0, srvInc_ = 0;
    static const int kInFlight = 2;
    ID3D12CommandAllocator* alloc_[kInFlight] = {};
    UINT64 slotFence_[kInFlight] = {};
    int cur_ = 0;
    bool allDirty_ = true;
    ID3D12GraphicsCommandList* list_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceValue_ = 0;
    ID3D12RootSignature* rootSig_ = nullptr;
    ID3D12PipelineState* psoPal_ = nullptr;
    ID3D12PipelineState* psoScale_ = nullptr;
    ID3D12PipelineState* psoOverlay_ = nullptr;
    ID3D12Resource* ovTex_ = nullptr;
    ID3D12Resource* ovUpload_[2] = {};
    uint8_t* ovPtr_[2] = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT ovFoot_ = {};
    int ovW_ = 0, ovH_ = 0;
    ID3D12Resource* bb_[kBackBuffers] = {};
    ID3D12Resource* idxTex_ = nullptr;
    ID3D12Resource* palTex_ = nullptr;
    ID3D12Resource* frameTex_ = nullptr;
    ID3D12Resource* upload_[kInFlight] = {};
    uint8_t* uploadPtr_[kInFlight] = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT idxFoot_ = {}, palFoot_ = {};
    int fw_ = 0, fh_ = 0;
};

} // namespace

std::unique_ptr<Renderer> CreateD3D12Renderer() { return std::make_unique<D3D12Renderer>(); }

} // namespace swgbm
