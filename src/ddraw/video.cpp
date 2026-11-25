// Intro video playback through the SWGB Modern renderer.
//
// The game's intros (xlogo1.avi, xintro.avi) are Indeo 5 AVIs. Originally an
// external player showed them in a separate window before the game started.
// Here they play inside the game window through the overlay layer, so they get
// the same scaling as the game, with audio through the mod's audio engine.
//
// Sources, in order of preference:
//   1. SWGBModern\hd\video\<name>.mp4  (AI-upscaled, if [Enhancements] HDVideos=1)
//   2. <name>.avi through Video for Windows (needs the Indeo 5 codec, ir50_32)
//   3. SWGBModern\video\<name>.mp4     (converted by the launcher with FFmpeg)
// MP4 files are decoded with Media Foundation.
#include "video.h"
#include "presenter.h"
#include "window.h"
#include <vfw.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <dsound.h>
#include <vector>
#include <string>

namespace swgbm {
namespace {

struct Clip {
    int w = 0, h = 0;
    double fps = 15;
    std::vector<std::vector<uint32_t>> frames;   // decoded lazily for AVI, streamed for MF
    WAVEFORMATEX wfx = {};
    std::vector<uint8_t> audio;
};

// Media Foundation is loaded on demand: Windows "N" editions may not have it,
// and ddraw.dll must still load there.
struct MF {
    HRESULT(WINAPI* Startup)(ULONG, DWORD) = nullptr;
    HRESULT(WINAPI* Shutdown)() = nullptr;
    HRESULT(WINAPI* CreateAttributes)(IMFAttributes**, UINT32) = nullptr;
    HRESULT(WINAPI* CreateMediaType)(IMFMediaType**) = nullptr;
    HRESULT(WINAPI* CreateWaveFormatEx)(IMFMediaType*, WAVEFORMATEX**, UINT32*, UINT32) = nullptr;
    HRESULT(WINAPI* CreateSourceReaderFromURL)(LPCWSTR, IMFAttributes*, IMFSourceReader**) = nullptr;
    bool Load() {
        HMODULE plat = LoadLibraryW(L"mfplat.dll"), rw = LoadLibraryW(L"mfreadwrite.dll");
        if (!plat || !rw) return false;
        Startup = (decltype(Startup))GetProcAddress(plat, "MFStartup");
        Shutdown = (decltype(Shutdown))GetProcAddress(plat, "MFShutdown");
        CreateAttributes = (decltype(CreateAttributes))GetProcAddress(plat, "MFCreateAttributes");
        CreateMediaType = (decltype(CreateMediaType))GetProcAddress(plat, "MFCreateMediaType");
        CreateWaveFormatEx = (decltype(CreateWaveFormatEx))GetProcAddress(plat, "MFCreateWaveFormatExFromMFMediaType");
        CreateSourceReaderFromURL = (decltype(CreateSourceReaderFromURL))GetProcAddress(rw, "MFCreateSourceReaderFromURL");
        return Startup && Shutdown && CreateAttributes && CreateMediaType && CreateWaveFormatEx && CreateSourceReaderFromURL;
    }
} g_mf;

bool FileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// ---------------------------------------------------------------- playback
class Player {
public:
    explicit Player(HWND hwnd) : hwnd_(hwnd) {}

    // Returns false when the user asked to skip all remaining videos.
    template <class NextFrame>
    bool Run(int w, int h, double fps, NextFrame next, const WAVEFORMATEX* wfx, const std::vector<uint8_t>* audio) {
        int gw, gh;
        Presenter::Get().GameSize(gw, gh);
        // fit the video into the current game frame, keeping its aspect ratio
        double s = std::min((double)gw / w, (double)gh / h);
        float rw = (float)(w * s), rh = (float)(h * s);
        float rx = (gw - rw) / 2, ry = (gh - rh) / 2;
        StartAudio(wfx, audio);
        LARGE_INTEGER freq, t0;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        std::vector<uint32_t> px((size_t)w * h);
        int shown = -1;
        bool keepGoing = true;
        for (;;) {
            // input: any key/click skips this video, Esc skips all
            MSG m;
            bool skip = false;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                if (m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN || m.message == WM_LBUTTONDOWN || m.message == WM_RBUTTONDOWN) {
                    skip = true;
                    if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) keepGoing = false;
                    continue;
                }
                if (m.message == WM_KEYUP || m.message == WM_CHAR || m.message == WM_LBUTTONUP || m.message == WM_RBUTTONUP) continue;
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            if (skip) break;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double t = (double)(now.QuadPart - t0.QuadPart) / freq.QuadPart;
            int want = (int)(t * fps);
            if (want > shown) {
                int got = next(want, px.data());
                if (got < 0) break; // end of clip
                shown = want;
                Presenter::Get().SetOverlay(px.data(), w, h, rx, ry, rw, rh, true);
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 4, QS_ALLINPUT);
        }
        StopAudio();
        Presenter::Get().ClearOverlay();
        return keepGoing;
    }

private:
    void StartAudio(const WAVEFORMATEX* wfx, const std::vector<uint8_t>* audio) {
        if (!wfx || !audio || audio->empty()) return;
        HMODULE ds = GetModuleHandleW(L"dsound.dll");
        if (!ds) ds = LoadLibraryW(L"dsound.dll"); // our dsound.dll when installed, else Windows'
        auto create = ds ? (HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN))GetProcAddress(ds, "DirectSoundCreate") : nullptr;
        if (!create || FAILED(create(nullptr, &dsound_, nullptr))) return;
        dsound_->SetCooperativeLevel(hwnd_, DSSCL_NORMAL);
        DSBUFFERDESC d = {sizeof(d)};
        d.dwFlags = DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLVOLUME;
        d.dwBufferBytes = (DWORD)audio->size();
        WAVEFORMATEX f = *wfx;
        d.lpwfxFormat = &f;
        if (FAILED(dsound_->CreateSoundBuffer(&d, &buf_, nullptr))) return;
        void *p1, *p2;
        DWORD b1, b2;
        if (SUCCEEDED(buf_->Lock(0, d.dwBufferBytes, &p1, &b1, &p2, &b2, 0))) {
            memcpy(p1, audio->data(), b1);
            if (p2) memcpy(p2, audio->data() + b1, b2);
            buf_->Unlock(p1, b1, p2, b2);
        }
        buf_->Play(0, 0, 0);
    }
    void StopAudio() {
        if (buf_) { buf_->Stop(); buf_->Release(); buf_ = nullptr; }
        if (dsound_) { dsound_->Release(); dsound_ = nullptr; }
    }
    HWND hwnd_;
    IDirectSound* dsound_ = nullptr;
    IDirectSoundBuffer* buf_ = nullptr;
};

// ---------------------------------------------------------------- AVI (VfW)
bool PlayAvi(HWND hwnd, const std::wstring& path, bool& keepGoing) {
    AVIFileInit();
    PAVISTREAM vs = nullptr, as = nullptr;
    if (AVIStreamOpenFromFileW(&vs, path.c_str(), streamtypeVIDEO, 0, OF_READ, nullptr) != 0) { AVIFileExit(); return false; }
    AVISTREAMINFOW vi = {};
    AVIStreamInfoW(vs, &vi, sizeof(vi));
    int w = vi.rcFrame.right - vi.rcFrame.left, h = vi.rcFrame.bottom - vi.rcFrame.top;
    double fps = vi.dwScale ? (double)vi.dwRate / vi.dwScale : 15.0;
    LONG first = AVIStreamStart(vs), count = AVIStreamLength(vs);
    BITMAPINFOHEADER want = {sizeof(want), w, h, 1, 32, BI_RGB};
    PGETFRAME gf = AVIStreamGetFrameOpen(vs, &want);
    if (!gf) gf = AVIStreamGetFrameOpen(vs, nullptr); // codec's default output format
    if (!gf) {
        LOG("video: %ls - no decoder for this AVI (Indeo 5 codec missing?)", path.c_str());
        AVIStreamRelease(vs);
        AVIFileExit();
        return false;
    }
    WAVEFORMATEX wfx = {};
    std::vector<uint8_t> audio;
    if (AVIStreamOpenFromFileW(&as, path.c_str(), streamtypeAUDIO, 0, OF_READ, nullptr) == 0) {
        LONG fsz = sizeof(wfx);
        AVIStreamReadFormat(as, AVIStreamStart(as), &wfx, &fsz);
        if (wfx.wFormatTag == WAVE_FORMAT_PCM) {
            LONG bytes = 0, samples = 0;
            AVIStreamRead(as, AVIStreamStart(as), AVIStreamLength(as), nullptr, 0, &bytes, &samples);
            audio.resize(bytes);
            AVIStreamRead(as, AVIStreamStart(as), AVIStreamLength(as), audio.data(), bytes, nullptr, nullptr);
        } else {
            LOG("video: %ls - audio format %u not PCM, playing without sound", path.c_str(), wfx.wFormatTag);
        }
        AVIStreamRelease(as);
    }
    LOG("video: playing %ls (%dx%d, %.1f fps, %ld frames, VfW)", path.c_str(), w, h, fps, count);
    Player pl(hwnd);
    keepGoing = pl.Run(w, h, fps, [&](int i, uint32_t* out) -> int {
        if (i >= count) return -1;
        auto* bih = (BITMAPINFOHEADER*)AVIStreamGetFrame(gf, first + i);
        if (!bih) return 0;
        const uint8_t* bits = (const uint8_t*)bih + bih->biSize + bih->biClrUsed * 4;
        int bpp = bih->biBitCount, stride = ((bih->biWidth * bpp + 31) / 32) * 4;
        bool bottomUp = bih->biHeight > 0;
        for (int y = 0; y < h; y++) {
            const uint8_t* row = bits + (size_t)(bottomUp ? h - 1 - y : y) * stride;
            uint32_t* o = out + (size_t)y * w;
            for (int x = 0; x < w; x++) {
                if (bpp == 32) o[x] = ((const uint32_t*)row)[x] | 0xFF000000;
                else o[x] = 0xFF000000 | (row[x * 3 + 2] << 16) | (row[x * 3 + 1] << 8) | row[x * 3];
            }
        }
        return 1;
    }, wfx.nChannels ? &wfx : nullptr, &audio);
    AVIStreamGetFrameClose(gf);
    AVIStreamRelease(vs);
    AVIFileExit();
    return true;
}

// ---------------------------------------------------------------- MP4 (Media Foundation)
bool PlayMp4(HWND hwnd, const std::wstring& path, bool& keepGoing) {
    static bool mfOk = g_mf.Load();
    if (!mfOk) { LOG("video: Media Foundation not available (Windows N without Media Feature Pack?)"); return false; }
    if (FAILED(g_mf.Startup(MF_VERSION, MFSTARTUP_LITE))) return false;
    bool ok = false;
    IMFAttributes* attr = nullptr;
    g_mf.CreateAttributes(&attr, 1);
    attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    IMFSourceReader* rd = nullptr;
    IMFSourceReader* ard = nullptr;
    if (SUCCEEDED(g_mf.CreateSourceReaderFromURL(path.c_str(), attr, &rd))) {
        IMFMediaType* t = nullptr;
        g_mf.CreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        HRESULT hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t);
        t->Release();
        IMFMediaType* cur = nullptr;
        UINT32 w = 0, h = 0, num = 15, den = 1;
        if (SUCCEEDED(hr) && SUCCEEDED(rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
            MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
            MFGetAttributeRatio(cur, MF_MT_FRAME_RATE, &num, &den);
            cur->Release();
        }
        // audio: decode everything up front (intros are short) with a second reader
        WAVEFORMATEX wfx = {};
        std::vector<uint8_t> audio;
        if (SUCCEEDED(g_mf.CreateSourceReaderFromURL(path.c_str(), nullptr, &ard))) {
            IMFMediaType* at = nullptr;
            g_mf.CreateMediaType(&at);
            at->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            at->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            at->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            if (SUCCEEDED(ard->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, at))) {
                IMFMediaType* ac = nullptr;
                if (SUCCEEDED(ard->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &ac))) {
                    WAVEFORMATEX* pw = nullptr;
                    UINT32 sz = 0;
                    if (SUCCEEDED(g_mf.CreateWaveFormatEx(ac, &pw, &sz, 0))) { wfx = *pw; wfx.cbSize = 0; CoTaskMemFree(pw); }
                    ac->Release();
                }
                for (;;) {
                    DWORD flags = 0;
                    IMFSample* smp = nullptr;
                    if (FAILED(ard->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &smp)) ||
                        (flags & MF_SOURCE_READERF_ENDOFSTREAM)) { if (smp) smp->Release(); break; }
                    if (!smp) continue;
                    IMFMediaBuffer* mb = nullptr;
                    if (SUCCEEDED(smp->ConvertToContiguousBuffer(&mb))) {
                        BYTE* p;
                        DWORD len;
                        if (SUCCEEDED(mb->Lock(&p, nullptr, &len))) { audio.insert(audio.end(), p, p + len); mb->Unlock(); }
                        mb->Release();
                    }
                    smp->Release();
                }
            }
            at->Release();
        }
        if (w && h) {
            double fps = den ? (double)num / den : 15.0;
            LOG("video: playing %ls (%ux%u, %.2f fps, Media Foundation)", path.c_str(), w, h, fps);
            Player pl(hwnd);
            int decoded = -1;
            std::vector<uint32_t> last((size_t)w * h, 0xFF000000);
            keepGoing = pl.Run((int)w, (int)h, fps, [&](int want, uint32_t* out) -> int {
                // sequential decode up to the wanted frame (drops late frames)
                while (decoded < want) {
                    DWORD flags = 0;
                    IMFSample* smp = nullptr;
                    if (FAILED(rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, nullptr, &smp)) ||
                        (flags & MF_SOURCE_READERF_ENDOFSTREAM)) { if (smp) smp->Release(); return -1; }
                    if (!smp) continue;
                    decoded++;
                    if (decoded == want) {
                        IMFMediaBuffer* mb = nullptr;
                        if (SUCCEEDED(smp->ConvertToContiguousBuffer(&mb))) {
                            BYTE* p;
                            DWORD len;
                            if (SUCCEEDED(mb->Lock(&p, nullptr, &len)) && len >= w * h * 4) {
                                const uint32_t* s = (const uint32_t*)p;
                                for (size_t i = 0, n = (size_t)w * h; i < n; i++) last[i] = s[i] | 0xFF000000;
                                mb->Unlock();
                            }
                            mb->Release();
                        }
                    }
                    smp->Release();
                }
                memcpy(out, last.data(), last.size() * 4);
                return 1;
            }, wfx.nChannels ? &wfx : nullptr, &audio);
            ok = true;
        }
    }
    if (ard) ard->Release();
    if (rd) rd->Release();
    if (attr) attr->Release();
    g_mf.Shutdown();
    return ok;
}

} // namespace

void Video::PlayIntros(HWND hwnd) {
    static bool done = false;
    if (done || !hwnd) return;
    done = true;
    if (!CfgBool("Video", "PlayIntro", true)) return;
    // The game has not shown its window yet at this point of startup.
    ShowWindow(hwnd, SW_SHOW);
    Window::Relayout();
    SetForegroundWindow(hwnd);
    std::string list = CfgStr("Video", "Intros", "xlogo1,xintro");
    bool hd = CfgBool("Enhancements", "HDVideos", false);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    size_t a = 0;
    while (a <= list.size()) {
        size_t b = list.find(',', a);
        std::string n = list.substr(a, b == std::string::npos ? std::string::npos : b - a);
        a = b == std::string::npos ? list.size() + 1 : b + 1;
        while (!n.empty() && n[0] == ' ') n.erase(0, 1);
        if (n.empty()) continue;
        std::wstring name(n.begin(), n.end());
        std::wstring dir = GameDir();
        bool keepGoing = true, played = false;
        std::wstring hdPath = dir + L"\\SWGBModern\\hd\\video\\" + name + L".mp4";
        std::wstring conv = dir + L"\\SWGBModern\\video\\" + name + L".mp4";
        if (hd && FileExists(hdPath)) played = PlayMp4(hwnd, hdPath, keepGoing);
        if (!played && FileExists(dir + L"\\" + name + L".avi")) played = PlayAvi(hwnd, dir + L"\\" + name + L".avi", keepGoing);
        if (!played && FileExists(conv)) played = PlayMp4(hwnd, conv, keepGoing);
        if (!played) LOG("video: %ls could not be played (no decoder; the launcher can convert it)", name.c_str());
        if (!keepGoing) break;
    }
}

} // namespace swgbm
