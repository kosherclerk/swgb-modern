// SWGB Modern - DirectSound replacement.
//
// The game mixes 11/22 kHz 16-bit mono effects and a streamed music buffer
// through DirectSound, which on Vista+ is a thin software layer with basic
// resampling. Here every buffer is mixed in 32-bit float at the output
// device's native rate through WASAPI (shared, event driven) using a
// band-limited polyphase windowed-sinc resampler, with DirectSound's exact
// volume (dB) and pan laws, then passed through a soft limiter instead of
// hard clipping. Output follows the Windows default device.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _USE_MATH_DEFINES
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <ks.h>
#include <ksmedia.h>
#include <math.h>
#include <vector>
#include <algorithm>
#include "../common/common.h"

using namespace swgbm;

namespace {

// ------------------------------------------------------------- resampler
// Polyphase windowed-sinc (Kaiser, beta 8). Cutoff follows the lower of the
// two Nyquist frequencies so upsampled game audio is free of imaging.
struct SincTable {
    static const int kTaps = 32, kPhases = 512;
    std::vector<float> coef; // [phase][tap]
    float cutoff = 1.0f;
    void Build(float fc) {
        cutoff = fc;
        coef.assign((size_t)kTaps * (kPhases + 1), 0.0f);
        auto bessel0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 32; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
        const double beta = 8.0, i0b = bessel0(beta);
        for (int p = 0; p <= kPhases; p++) {
            double frac = (double)p / kPhases;
            double sum = 0;
            for (int t = 0; t < kTaps; t++) {
                double x = (t - kTaps / 2 + 1) - frac; // distance from the interpolation point
                double s = x == 0 ? fc : sin(M_PI * fc * x) / (M_PI * x);
                double r = x / (kTaps / 2.0);
                double w = fabs(r) >= 1 ? 0 : bessel0(beta * sqrt(1 - r * r)) / i0b;
                coef[(size_t)p * kTaps + t] = (float)(s * w);
                sum += s * w;
            }
            for (int t = 0; t < kTaps; t++) coef[(size_t)p * kTaps + t] /= (float)sum; // unity DC gain
        }
    }
};

enum class Quality { Linear, Cubic, Sinc };

// ------------------------------------------------------------- engine
class Buffer;

struct Engine {
    CRITICAL_SECTION lock;
    std::vector<Buffer*> buffers;
    HANDLE thread = nullptr;
    volatile LONG running = 0;
    int rate = 48000, channels = 2;
    float master = 1.0f;          // primary buffer volume * user volume
    float userVolume = 1.0f;
    Quality quality = Quality::Sinc;
    bool limiter = true;
    std::vector<SincTable*> tables;
    volatile LONG framesMixed = 0;

    SincTable* TableFor(float fc) {
        // quantise cutoff to 1/64 to share tables
        float q = std::min(1.0f, std::max(0.05f, floorf(fc * 64) / 64));
        for (auto* t : tables) if (t->cutoff == q) return t;
        auto* t = new SincTable();
        t->Build(q * 0.97f); // small guard band
        t->cutoff = q;
        tables.push_back(t);
        return t;
    }
    void Start();
    void Run();
};
Engine g_eng;

class Buffer : public IDirectSoundBuffer, public IDirectSoundNotify {
public:
    Buffer(bool primary, const DSBUFFERDESC* d) : primary_(primary) {
        flags_ = d->dwFlags;
        if (!primary && d->lpwfxFormat) {
            fmt_ = *d->lpwfxFormat;
            fmt_.cbSize = 0;
            size_ = d->dwBufferBytes;
            data_.assign(size_, (fmt_.wBitsPerSample == 8) ? 0x80 : 0);
            freq_ = fmt_.nSamplesPerSec;
        } else {
            fmt_ = {WAVE_FORMAT_PCM, 2, 22050, 22050 * 4, 4, 16, 0};
        }
    }
    Buffer(const Buffer& src) : primary_(false) { // DuplicateSoundBuffer shares data
        flags_ = src.flags_; fmt_ = src.fmt_; size_ = src.size_; shared_ = const_cast<Buffer*>(&src);
        shared_->AddRef();
        freq_ = src.freq_; volume_ = src.volume_; pan_ = src.pan_;
    }
    ~Buffer() { if (shared_) shared_->Release(); }

    uint8_t* Data() { return shared_ ? shared_->Data() : data_.data(); }

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDirectSoundBuffer) { *ppv = (IDirectSoundBuffer*)this; AddRef(); return S_OK; }
        if (riid == IID_IDirectSoundNotify && !primary_) { *ppv = (IDirectSoundNotify*)this; AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&refs_); }
    STDMETHOD_(ULONG, Release)() override {
        LONG r = InterlockedDecrement(&refs_);
        if (r == 0) {
            EnterCriticalSection(&g_eng.lock);
            auto& v = g_eng.buffers;
            v.erase(std::remove(v.begin(), v.end(), this), v.end());
            LeaveCriticalSection(&g_eng.lock);
            delete this;
        }
        return r;
    }
    // IDirectSoundBuffer
    STDMETHOD(GetCaps)(LPDSBCAPS c) override {
        if (!c) return DSERR_INVALIDPARAM;
        c->dwFlags = flags_ | DSBCAPS_LOCSOFTWARE;
        c->dwBufferBytes = primary_ ? 4096 : size_;
        c->dwUnlockTransferRate = 0;
        c->dwPlayCpuOverhead = 0;
        return DS_OK;
    }
    STDMETHOD(GetCurrentPosition)(LPDWORD play, LPDWORD write) override {
        if (primary_) { if (play) *play = 0; if (write) *write = 0; return DS_OK; }
        EnterCriticalSection(&g_eng.lock);
        DWORD p = (DWORD)pos_ * fmt_.nBlockAlign;
        LeaveCriticalSection(&g_eng.lock);
        if (p >= size_) p = 0;
        // write cursor: ~15 ms ahead of the play cursor (data already committed to the mix)
        DWORD lead = ((fmt_.nAvgBytesPerSec * 15 / 1000) / fmt_.nBlockAlign) * fmt_.nBlockAlign;
        if (play) *play = p;
        if (write) *write = playing_ ? (p + lead) % size_ : p;
        return DS_OK;
    }
    STDMETHOD(GetFormat)(LPWAVEFORMATEX f, DWORD sz, LPDWORD written) override {
        if (written) *written = sizeof(WAVEFORMATEX);
        if (f) memcpy(f, &fmt_, std::min<DWORD>(sz, sizeof(WAVEFORMATEX)));
        return DS_OK;
    }
    STDMETHOD(GetVolume)(LPLONG v) override { if (v) *v = volume_; return DS_OK; }
    STDMETHOD(GetPan)(LPLONG p) override { if (p) *p = pan_; return DS_OK; }
    STDMETHOD(GetFrequency)(LPDWORD f) override { if (f) *f = freq_; return DS_OK; }
    STDMETHOD(GetStatus)(LPDWORD s) override {
        if (s) *s = playing_ ? (DSBSTATUS_PLAYING | (looping_ ? DSBSTATUS_LOOPING : 0)) : 0;
        return DS_OK;
    }
    STDMETHOD(Initialize)(LPDIRECTSOUND, LPCDSBUFFERDESC) override { return DSERR_ALREADYINITIALIZED; }
    STDMETHOD(Lock)(DWORD off, DWORD bytes, LPVOID* p1, LPDWORD b1, LPVOID* p2, LPDWORD b2, DWORD flags) override {
        if (primary_) return DSERR_PRIOLEVELNEEDED;
        if (flags & DSBLOCK_FROMWRITECURSOR) { DWORD w; GetCurrentPosition(nullptr, &w); off = w; }
        if (flags & DSBLOCK_ENTIREBUFFER) bytes = size_;
        if (off >= size_ || bytes > size_) return DSERR_INVALIDPARAM;
        uint8_t* d = Data();
        *p1 = d + off;
        if (off + bytes <= size_) { *b1 = bytes; if (p2) *p2 = nullptr; if (b2) *b2 = 0; }
        else { *b1 = size_ - off; if (p2) *p2 = d; if (b2) *b2 = bytes - *b1; }
        return DS_OK;
    }
    STDMETHOD(Play)(DWORD, DWORD, DWORD flags) override {
        if (primary_) return DS_OK;
        EnterCriticalSection(&g_eng.lock);
        looping_ = (flags & DSBPLAY_LOOPING) != 0;
        if (!playing_) {
            playing_ = true;
            if (std::find(g_eng.buffers.begin(), g_eng.buffers.end(), this) == g_eng.buffers.end()) g_eng.buffers.push_back(this);
        }
        LeaveCriticalSection(&g_eng.lock);
        return DS_OK;
    }
    STDMETHOD(SetCurrentPosition)(DWORD p) override {
        if (primary_) return DS_OK;
        EnterCriticalSection(&g_eng.lock);
        pos_ = std::min<DWORD>(p, size_) / fmt_.nBlockAlign;
        frac_ = 0;
        LeaveCriticalSection(&g_eng.lock);
        return DS_OK;
    }
    STDMETHOD(SetFormat)(LPCWAVEFORMATEX f) override { if (primary_ && f) fmt_ = *f; return DS_OK; }
    STDMETHOD(SetVolume)(LONG v) override {
        volume_ = std::max<LONG>(DSBVOLUME_MIN, std::min<LONG>(DSBVOLUME_MAX, v));
        if (primary_) g_eng.master = Gain(volume_);
        return DS_OK;
    }
    STDMETHOD(SetPan)(LONG p) override { pan_ = std::max<LONG>(DSBPAN_LEFT, std::min<LONG>(DSBPAN_RIGHT, p)); return DS_OK; }
    STDMETHOD(SetFrequency)(DWORD f) override {
        freq_ = f == DSBFREQUENCY_ORIGINAL ? fmt_.nSamplesPerSec : std::max<DWORD>(100, std::min<DWORD>(200000, f));
        return DS_OK;
    }
    STDMETHOD(Stop)() override {
        EnterCriticalSection(&g_eng.lock);
        bool was = playing_;
        playing_ = false;
        LeaveCriticalSection(&g_eng.lock);
        if (was) FireStop();
        return DS_OK;
    }
    STDMETHOD(Unlock)(LPVOID, DWORD, LPVOID, DWORD) override { return DS_OK; }
    STDMETHOD(Restore)() override { return DS_OK; }
    // IDirectSoundNotify
    STDMETHOD(SetNotificationPositions)(DWORD n, LPCDSBPOSITIONNOTIFY p) override {
        EnterCriticalSection(&g_eng.lock);
        notify_.assign(p, p + n);
        LeaveCriticalSection(&g_eng.lock);
        return DS_OK;
    }

    static float Gain(LONG mB) { return mB <= DSBVOLUME_MIN ? 0.0f : powf(10.0f, mB / 2000.0f); }
    void FireStop() { for (auto& n : notify_) if (n.dwOffset == DSBPN_OFFSETSTOP) SetEvent(n.hEventNotify); }

    // Called by the mixer with g_eng.lock held: add `frames` output frames.
    void Mix(float* out, int frames, int outCh, int outRate) {
        if (!playing_ || size_ == 0) return;
        const uint8_t* d = Data();
        const int ch = fmt_.nChannels, bits = fmt_.wBitsPerSample;
        const uint32_t total = size_ / fmt_.nBlockAlign;
        if (total == 0) return;
        float g = Gain(volume_) * g_eng.master * g_eng.userVolume;
        float gl = g, gr = g;
        if (pan_ > 0) gl *= Gain(-pan_); else if (pan_ < 0) gr *= Gain(pan_);
        const double step = (double)freq_ / outRate;
        auto sample = [&](uint32_t i, int c) -> float {
            i %= total;
            if (bits == 16) return ((const int16_t*)d)[(size_t)i * ch + c] * (1.0f / 32768.0f);
            return (d[(size_t)i * ch + c] - 128) * (1.0f / 128.0f);
        };
        // Index access with loop/end awareness: past the end of a one-shot buffer is silence.
        auto at = [&](int64_t i, int c) -> float {
            if (i < 0) return looping_ ? sample((uint32_t)((i % total + total) % total), c) : 0.0f;
            if (i >= total && !looping_) return 0.0f;
            return sample((uint32_t)(i % total), c);
        };
        SincTable* tab = nullptr;
        if (g_eng.quality == Quality::Sinc) tab = g_eng.TableFor((float)std::min(1.0, 1.0 / step));
        uint32_t prevPos = pos_;
        for (int f = 0; f < frames; f++) {
            float s[2] = {0, 0};
            for (int c = 0; c < std::min(ch, 2); c++) {
                int64_t i = pos_;
                float t = (float)frac_;
                float v;
                if (step == 1.0 && frac_ == 0) v = at(i, c);
                else if (g_eng.quality == Quality::Linear) v = at(i, c) + (at(i + 1, c) - at(i, c)) * t;
                else if (g_eng.quality == Quality::Cubic) {
                    float y0 = at(i - 1, c), y1 = at(i, c), y2 = at(i + 1, c), y3 = at(i + 2, c);
                    float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3, b = y0 - 2.5f * y1 + 2 * y2 - 0.5f * y3, cc = -0.5f * y0 + 0.5f * y2;
                    v = ((a * t + b) * t + cc) * t + y1;
                } else {
                    int ph = (int)(t * SincTable::kPhases);
                    const float* k = &tab->coef[(size_t)ph * SincTable::kTaps];
                    int64_t base = i - SincTable::kTaps / 2 + 1;
                    v = 0;
                    for (int tp = 0; tp < SincTable::kTaps; tp++) v += k[tp] * at(base + tp, c);
                }
                s[c] = v;
            }
            if (ch == 1) s[1] = s[0];
            if (outCh >= 2) { out[f * outCh] += s[0] * gl; out[f * outCh + 1] += s[1] * gr; }
            else out[f] += (s[0] * gl + s[1] * gr) * 0.5f;
            frac_ += step;
            uint32_t adv = (uint32_t)frac_;
            frac_ -= adv;
            pos_ += adv;
            if (pos_ >= total) {
                if (looping_) pos_ %= total;
                else { pos_ = 0; frac_ = 0; playing_ = false; FireStop(); break; }
            }
        }
        // position notifications crossed during this chunk
        if (!notify_.empty()) {
            uint32_t a = prevPos * fmt_.nBlockAlign, b = pos_ * fmt_.nBlockAlign;
            for (auto& n : notify_) {
                if (n.dwOffset == DSBPN_OFFSETSTOP) continue;
                bool hit = a <= b ? (n.dwOffset >= a && n.dwOffset < b) : (n.dwOffset >= a || n.dwOffset < b);
                if (hit) SetEvent(n.hEventNotify);
            }
        }
    }

    bool primary_;
    LONG refs_ = 1;
    DWORD flags_ = 0;
    WAVEFORMATEX fmt_ = {};
    DWORD size_ = 0;
    std::vector<uint8_t> data_;
    Buffer* shared_ = nullptr;
    DWORD freq_ = 22050;
    LONG volume_ = 0, pan_ = 0;
    bool playing_ = false, looping_ = false;
    uint32_t pos_ = 0;
    double frac_ = 0;
    std::vector<DSBPOSITIONNOTIFY> notify_;
};

class DSound : public IDirectSound {
public:
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDirectSound) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&refs_); }
    STDMETHOD_(ULONG, Release)() override { LONG r = InterlockedDecrement(&refs_); if (!r) delete this; return r; }
    STDMETHOD(CreateSoundBuffer)(LPCDSBUFFERDESC d, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN) override {
        if (!d || !out) return DSERR_INVALIDPARAM;
        bool prim = (d->dwFlags & DSBCAPS_PRIMARYBUFFER) != 0;
        if (!prim) {
            if (!d->lpwfxFormat || d->dwBufferBytes < 4 || d->dwBufferBytes > 0x0FFFFFFF) return DSERR_INVALIDPARAM;
            auto* f = d->lpwfxFormat;
            if (f->wFormatTag != WAVE_FORMAT_PCM || (f->wBitsPerSample != 8 && f->wBitsPerSample != 16) || f->nChannels < 1 || f->nChannels > 2)
            {
                LOG("audio: rejected buffer format tag %u, %u bit, %u ch, %u Hz", f->wFormatTag, f->wBitsPerSample, f->nChannels, (unsigned)f->nSamplesPerSec);
                return DSERR_BADFORMAT;
            }
        }
        *out = new Buffer(prim, d);
        return DS_OK;
    }
    STDMETHOD(GetCaps)(LPDSCAPS c) override {
        if (!c) return DSERR_INVALIDPARAM;
        DWORD sz = c->dwSize;
        memset(c, 0, std::min<DWORD>(sz, sizeof(DSCAPS)));
        c->dwSize = sz;
        c->dwFlags = DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT |
                     DSCAPS_SECONDARYMONO | DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARY8BIT | DSCAPS_SECONDARY16BIT | DSCAPS_CONTINUOUSRATE;
        c->dwMinSecondarySampleRate = 100;
        c->dwMaxSecondarySampleRate = 200000;
        c->dwMaxHwMixingAllBuffers = c->dwMaxHwMixingStaticBuffers = c->dwMaxHwMixingStreamingBuffers = 0;
        return DS_OK;
    }
    STDMETHOD(DuplicateSoundBuffer)(LPDIRECTSOUNDBUFFER src, LPDIRECTSOUNDBUFFER* out) override {
        if (!src || !out) return DSERR_INVALIDPARAM;
        *out = new Buffer(*(Buffer*)src);
        return DS_OK;
    }
    STDMETHOD(SetCooperativeLevel)(HWND, DWORD) override { return DS_OK; }
    STDMETHOD(Compact)() override { return DS_OK; }
    STDMETHOD(GetSpeakerConfig)(LPDWORD c) override { if (c) *c = DSSPEAKER_STEREO; return DS_OK; }
    STDMETHOD(SetSpeakerConfig)(DWORD) override { return DS_OK; }
    STDMETHOD(Initialize)(LPCGUID) override { return DS_OK; }
    LONG refs_ = 1;
};

// ------------------------------------------------------------- output
DWORD WINAPI MixThread(void*) { g_eng.Run(); return 0; }

void Engine::Start() {
    if (InterlockedCompareExchange(&running, 1, 0) != 0) return;
    std::string q = CfgStr("Audio", "Resampler", "sinc");
    quality = q == "linear" ? Quality::Linear : q == "cubic" ? Quality::Cubic : Quality::Sinc;
    userVolume = std::max(0, std::min(200, CfgInt("Audio", "Volume", 100))) / 100.0f;
    limiter = CfgBool("Audio", "Limiter", true);
    thread = CreateThread(nullptr, 0, MixThread, nullptr, 0, nullptr);
}

void Engine::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIdx = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);
    std::vector<float> mix;
    while (running) {
        IMMDeviceEnumerator* en = nullptr;
        IMMDevice* dev = nullptr;
        IAudioClient* ac = nullptr;
        IAudioRenderClient* rc = nullptr;
        WAVEFORMATEX* mf = nullptr;
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) &&
                  SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
                  SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) &&
                  SUCCEEDED(ac->GetMixFormat(&mf));
        bool isFloat = false;
        if (ok) {
            channels = mf->nChannels;
            rate = mf->nSamplesPerSec;
            isFloat = mf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                      (mf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((WAVEFORMATEXTENSIBLE*)mf)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
            REFERENCE_TIME period = 100000; // 10 ms
            int latencyMs = CfgInt("Audio", "LatencyMs", 20);
            ok = SUCCEEDED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, (REFERENCE_TIME)latencyMs * 10000,
                                          0, mf, nullptr)) &&
                 SUCCEEDED(ac->SetEventHandle(ev)) && SUCCEEDED(ac->GetService(__uuidof(IAudioRenderClient), (void**)&rc)) &&
                 SUCCEEDED(ac->Start());
            (void)period;
            LOG("audio: WASAPI %d Hz, %d ch, %s, resampler %s", rate, channels, isFloat ? "float" : "pcm16",
                quality == Quality::Sinc ? "sinc" : quality == Quality::Cubic ? "cubic" : "linear");
        }
        UINT32 bufFrames = 0;
        if (ok) ac->GetBufferSize(&bufFrames);
        while (ok && running) {
            if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
            UINT32 pad = 0;
            if (FAILED(ac->GetCurrentPadding(&pad))) { ok = false; break; }
            UINT32 n = bufFrames - pad;
            if (!n) continue;
            BYTE* out = nullptr;
            HRESULT hr = rc->GetBuffer(n, &out);
            if (FAILED(hr)) { ok = false; break; }
            mix.assign((size_t)n * channels, 0.0f);
            EnterCriticalSection(&lock);
            for (size_t i = 0; i < buffers.size(); i++) {
                Buffer* b = buffers[i];
                b->Mix(mix.data(), (int)n, std::min(channels, 2) == 2 ? channels : 1, rate);
            }
            LeaveCriticalSection(&lock);
            // soft limiter: transparent below -1 dBFS, smooth knee above
            for (auto& s : mix) {
                if (limiter) {
                    float a = fabsf(s);
                    if (a > 0.89f) s = copysignf(0.89f + 0.11f * tanhf((a - 0.89f) / 0.11f), s);
                } else {
                    s = std::max(-1.0f, std::min(1.0f, s));
                }
            }
            if (isFloat) memcpy(out, mix.data(), mix.size() * 4);
            else {
                int16_t* o = (int16_t*)out;
                for (size_t i = 0; i < mix.size(); i++) o[i] = (int16_t)lrintf(std::max(-1.0f, std::min(1.0f, mix[i])) * 32767.0f);
            }
            rc->ReleaseBuffer(n, 0);
            InterlockedExchangeAdd(&framesMixed, (LONG)n);
            if (LogEnabled()) {
                static DWORD lastStat = GetTickCount();
                static float peak = 0;
                static int maxVoices = 0;
                for (auto s : mix) peak = std::max(peak, fabsf(s));
                int playingNow = 0;
                EnterCriticalSection(&lock);
                for (auto* b : buffers) playingNow += b->playing_ ? 1 : 0;
                size_t total = buffers.size();
                LeaveCriticalSection(&lock);
                maxVoices = std::max(maxVoices, playingNow);
                if (GetTickCount() - lastStat > 5000) {
                    LOG("audio: %zu buffers known, max %d voices, peak %.2f dBFS", total, maxVoices, peak > 0 ? 20 * log10f(peak) : -120.0f);
                    lastStat = GetTickCount(); peak = 0; maxVoices = 0;
                }
            }
        }
        if (ac) ac->Stop();
        if (rc) rc->Release();
        if (ac) ac->Release();
        if (mf) CoTaskMemFree(mf);
        if (dev) dev->Release();
        if (en) en->Release();
        CloseHandle(ev);
        if (running) { LOG("audio: device lost or unavailable, retrying"); Sleep(500); }
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

} // namespace

extern "C" {

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSectionAndSpinCount(&g_eng.lock, 1000);
    }
    return TRUE;
}

typedef HRESULT(WINAPI* DSCreateFn)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);
static DSCreateFn SystemDirectSoundCreate() {
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH); // SysWOW64 for this 32-bit process
    wcscat_s(path, L"\\dsound.dll");
    HMODULE m = LoadLibraryW(path);
    return m ? (DSCreateFn)GetProcAddress(m, "DirectSoundCreate") : nullptr;
}

HRESULT WINAPI DirectSoundCreate(LPCGUID guid, LPDIRECTSOUND* out, LPUNKNOWN outer) {
    if (!CfgBool("Audio", "Enabled", true)) {
        // Audio engine disabled in the launcher: hand over to Windows' DirectSound.
        static DSCreateFn sys = SystemDirectSoundCreate();
        LOG("audio: SWGB Modern engine disabled, using system DirectSound");
        return sys ? sys(guid, out, outer) : DSERR_NODRIVER;
    }
    if (!out) return DSERR_INVALIDPARAM;
    g_eng.Start();
    *out = new DSound();
    LOG("DirectSoundCreate");
    return DS_OK;
}

HRESULT WINAPI DirectSoundEnumerateA(LPDSENUMCALLBACKA cb, LPVOID ctx) {
    if (cb) cb(nullptr, (LPSTR) "Primary Sound Driver", (LPSTR) "", ctx);
    return DS_OK;
}
HRESULT WINAPI DirectSoundEnumerateW(LPDSENUMCALLBACKW cb, LPVOID ctx) {
    if (cb) cb(nullptr, (LPWSTR)L"Primary Sound Driver", (LPWSTR)L"", ctx);
    return DS_OK;
}
HRESULT WINAPI DllCanUnloadNow() { return S_FALSE; }
HRESULT WINAPI DllGetClassObject(REFCLSID, REFIID, LPVOID* p) { if (p) *p = nullptr; return CLASS_E_CLASSNOTAVAILABLE; }

// Volume control for the launcher-independent in-game hook (ddraw.dll routes
// the game's mixer* calls here instead of touching the Windows mixer).
void WINAPI SWGBM_SetVolume(float v) { g_eng.userVolume = std::max(0.0f, std::min(2.0f, v)); }

} // extern "C"
