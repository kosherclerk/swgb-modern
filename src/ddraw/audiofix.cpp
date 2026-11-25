// Virtual mixer: the game drives its music/sound sliders through the Windows
// mixer API (mixerSetControlDetails), which on Vista+ changes system/endpoint
// volume. We give it a private virtual mixer instead:
//   - CD audio line  -> GOG's ogg CD emulation (auxSetVolume in win32.dll/winmm)
//   - wave/speakers  -> our dsound.dll master volume (if loaded)
#include "audiofix.h"
#include "hook.h"
#include <mmsystem.h>

namespace swgbm {
namespace {

struct Line { DWORD type; DWORD value; };
Line g_lines[8];
int g_nLines = 0;
const char* g_mmDll = "WINMM.dll";

Line* LineFor(DWORD id) {
    for (int i = 0; i < g_nLines; i++) if (g_lines[i].type == id) return &g_lines[i];
    if (g_nLines < 8) { g_lines[g_nLines] = {id, 0xFFFF}; return &g_lines[g_nLines++]; }
    return &g_lines[0];
}

void Apply(Line* l) {
    DWORD v = l->value & 0xFFFF;
    if (l->type == MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC) {
        HMODULE m = GetModuleHandleA(g_mmDll);
        auto aux = m ? (MMRESULT(WINAPI*)(UINT, DWORD))GetProcAddress(m, "auxSetVolume") : nullptr;
        if (aux) aux(0, v | (v << 16));
        LOG("virtual mixer: CD music volume %lu%%", v * 100 / 65535);
    } else {
        HMODULE ds = GetModuleHandleW(L"dsound.dll");
        auto set = ds ? (void(WINAPI*)(float))GetProcAddress(ds, "SWGBM_SetVolume") : nullptr;
        if (set) set(v / 65535.0f);
        LOG("virtual mixer: line %08lx volume %lu%%", l->type, v * 100 / 65535);
    }
}

UINT WINAPI H_mixerGetNumDevs() { return 1; }
MMRESULT WINAPI H_mixerOpen(LPHMIXER h, UINT, DWORD_PTR, DWORD_PTR, DWORD) { if (h) *h = (HMIXER)(uintptr_t)0x5157; return MMSYSERR_NOERROR; }
MMRESULT WINAPI H_mixerClose(HMIXER) { return MMSYSERR_NOERROR; }

MMRESULT WINAPI H_mixerGetLineInfoA(HMIXEROBJ, LPMIXERLINEA ml, DWORD flags) {
    if (!ml) return MMSYSERR_INVALPARAM;
    DWORD type = MIXERLINE_COMPONENTTYPE_DST_SPEAKERS;
    switch (flags & MIXER_GETLINEINFOF_QUERYMASK) {
    case MIXER_GETLINEINFOF_COMPONENTTYPE: type = ml->dwComponentType; break;
    case MIXER_GETLINEINFOF_LINEID: type = ml->dwLineID; break;
    case MIXER_GETLINEINFOF_SOURCE: type = ml->dwSource == 0 ? MIXERLINE_COMPONENTTYPE_SRC_WAVEOUT : MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC; break;
    default: break;
    }
    LineFor(type);
    DWORD sz = ml->cbStruct;
    memset(((BYTE*)ml) + sizeof(DWORD), 0, sz - sizeof(DWORD));
    ml->dwLineID = type;
    ml->dwComponentType = type;
    ml->cChannels = 2;
    ml->cConnections = type == MIXERLINE_COMPONENTTYPE_DST_SPEAKERS ? 2 : 0;
    ml->cControls = 1;
    ml->fdwLine = MIXERLINE_LINEF_ACTIVE;
    ml->Target.dwType = type == MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC ? MIXERLINE_TARGETTYPE_AUX : MIXERLINE_TARGETTYPE_WAVEOUT;
    lstrcpynA(ml->szShortName, "SWGB", MIXER_SHORT_NAME_CHARS);
    lstrcpynA(ml->szName, "SWGB Modern virtual line", MIXER_LONG_NAME_CHARS);
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI H_mixerGetLineControlsA(HMIXEROBJ, LPMIXERLINECONTROLSA mc, DWORD flags) {
    if (!mc || !mc->pamxctrl) return MMSYSERR_INVALPARAM;
    DWORD id = (flags & MIXER_GETLINECONTROLSF_QUERYMASK) == MIXER_GETLINECONTROLSF_ONEBYID ? mc->dwControlID : mc->dwLineID;
    MIXERCONTROLA* c = mc->pamxctrl;
    memset(c, 0, sizeof(*c));
    c->cbStruct = sizeof(*c);
    c->dwControlID = id;
    c->dwControlType = MIXERCONTROL_CONTROLTYPE_VOLUME;
    c->cMultipleItems = 0;
    c->Bounds.dwMinimum = 0;
    c->Bounds.dwMaximum = 0xFFFF;
    c->Metrics.cSteps = 0xFFFF;
    lstrcpynA(c->szName, "Volume", MIXER_LONG_NAME_CHARS);
    mc->cControls = 1;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI H_mixerGetControlDetailsA(HMIXEROBJ, LPMIXERCONTROLDETAILS d, DWORD) {
    if (!d || !d->paDetails) return MMSYSERR_INVALPARAM;
    Line* l = LineFor(d->dwControlID);
    auto* v = (MIXERCONTROLDETAILS_UNSIGNED*)d->paDetails;
    for (DWORD i = 0; i < (d->cChannels ? d->cChannels : 1); i++) v[i].dwValue = l->value;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI H_mixerSetControlDetails(HMIXEROBJ, LPMIXERCONTROLDETAILS d, DWORD) {
    if (!d || !d->paDetails) return MMSYSERR_INVALPARAM;
    Line* l = LineFor(d->dwControlID);
    l->value = ((MIXERCONTROLDETAILS_UNSIGNED*)d->paDetails)[0].dwValue;
    Apply(l);
    return MMSYSERR_NOERROR;
}

} // namespace

void AudioFix::Install() {
    if (!CfgBool("Fixes", "IsolateVolume", true)) return;
    // Clone Campaigns (GOG) imports winmm through GOG's "WIN32.dll" proxy.
    for (const char* dll : {"WIN32.dll", "WINMM.dll"}) {
        struct { const char* n; void* f; } hk[] = {
            {"mixerGetNumDevs", (void*)H_mixerGetNumDevs}, {"mixerOpen", (void*)H_mixerOpen},
            {"mixerClose", (void*)H_mixerClose}, {"mixerGetLineInfoA", (void*)H_mixerGetLineInfoA},
            {"mixerGetLineControlsA", (void*)H_mixerGetLineControlsA},
            {"mixerGetControlDetailsA", (void*)H_mixerGetControlDetailsA},
            {"mixerSetControlDetails", (void*)H_mixerSetControlDetails},
        };
        int n = 0;
        for (auto& h : hk) n += HookIAT(nullptr, dll, h.n, h.f) != nullptr;
        if (n) { g_mmDll = dll; LOG("virtual mixer: %d mixer functions redirected (%s)", n, dll); }
    }
}

} // namespace swgbm
