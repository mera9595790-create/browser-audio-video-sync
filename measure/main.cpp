// avsync_measure.exe — standalone wireless audio delay measurement (system
// window, no browser involved).
//
// Method:
// - WASAPI shared-mode render on the default playback device: a tone burst is
//   spliced into a silence stream; the exact device position of the first tone
//   sample is captured via GetPosition+GetCurrentPadding, correlated with QPC.
// - WASAPI shared-mode capture on the default recording device: packets are
//   drained and tagged with device positions, correlated with QPC.
// - Tone onset is found with a per-frequency Goertzel scan (adaptive noise
//   floor, two-hop confirmation).
// - delay = onset(physical, QPC) - emit(physical, QPC), corrected by the
//   render/capture endpoint latencies (GetStreamLatency). The residual is the
//   extra output-path latency (e.g. Bluetooth codec) plus mic front-end bias.
//
// "Apply" writes the median into the shared-memory config (Local\AVSyncDelayConfig)
// used by avsync_hook.dll — takes effect immediately in all hooked browsers.

#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <cmath>
#include <vector>
#include <algorithm>
#include <atomic>
#include "../common/config.h"
#include "../common/manualmap.h"
#include "../common/embedded_dll.h"

// PKEY_Device_FriendlyName (avoid mingw header variance)
static const PROPERTYKEY PKEY_Device_FriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
};

// ---------------------------------------------------------------------------
// UI ids and shared state
// ---------------------------------------------------------------------------
static const int ID_FREQ  = 101;
static const int ID_START = 102;
static const int ID_APPLY = 103;
static const int ID_STATUS = 104;
static const int ID_CANVAS = 105;
static const int ID_NOTE   = 106;
static const int ID_CAPDEV = 107;
static const int ID_RENDDEV = 108;
static const int ID_LOOPBACK = 109;
static const int ID_CAL = 110;
static const int ID_CALVAL = 111;
static const int ID_VP_CHROM = 112;
static const int ID_VP_CHROM_RST = 113;
static const int ID_VP_FF = 114;
static const int ID_VP_FF_RST = 115;
static const int ID_CUR = 116;
static const int ID_REFRESH = 117;
static const int ID_MIC = 118;
static const int ID_MEDIAN = 119;
static const int ID_PREVIEW = 120;
static const int ID_DISABLE = 121;
static const int ID_AUTOSTART = 122;
static const int ID_LANG = 123;
// group boxes + static labels (for language switching)
static const int ID_GRP_VIDEO = 200;
static const int ID_GRP_DEVICES = 201;
static const int ID_GRP_MIC = 202;
static const int ID_GRP_TEST = 203;
static const int ID_GRP_RESULT = 204;
static const int ID_GRP_SYSTEM = 205;
static const int ID_L_CHROMIUM = 206;
static const int ID_L_FIREFOX = 207;
static const int ID_L_OWN = 208;
static const int ID_L_PLAYBACK = 209;
static const int ID_L_RECORDING = 210;
static const int ID_L_MICLAT = 211;
static const int ID_L_FREQ = 212;
static const int ID_L_MEDIAN = 213;
static const int ID_L_SETZERO = 214;
static const int ID_L_LANG = 215;
static const UINT WM_APP_STATUS = WM_APP + 1;
static const UINT WM_APP_TRAY = WM_APP + 2;

static HWND g_hwnd = nullptr;
static HWND g_hStatus = nullptr;
static HWND g_hCanvas = nullptr;
static HWND g_hCapCombo = nullptr;
static HWND g_hRendCombo = nullptr;
static HWND g_hLangCombo = nullptr;
static std::vector<std::wstring> g_capIds;
static std::vector<std::wstring> g_rendIds;
static CRITICAL_SECTION g_cs;
static std::atomic<bool> g_stopRequested{false};
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_calRunning{false};
static std::atomic<bool> g_watchStop{false};
static HANDLE g_watchThread = nullptr;

struct MeasureParams {
    std::wstring capId;
    std::wstring rendId;
    std::wstring capName;
    bool loopback = false;
};

struct DrawData {
    std::vector<float> samples;   // analysis window, decimated for draw
    std::vector<double> tms;      // per-sample time (ms relative to emit)
    double t0ms = 0;              // window start relative to emit (ms)
    double spanms = 1;            // window span (ms)
    double emitMs = 0;            // emit marker (ms, relative to t0)
    double heardMs = -1e9;        // onset marker (ms), very negative = none
    int freq = 0;
};
static DrawData g_draw;           // guarded by g_cs
static double g_medianMs = 0;     // guarded by g_cs
static bool g_hasMedian = false;
static double g_micCalMs = 0;     // guarded by g_cs; mic input-path latency
static bool g_hasMicCal = false;
static double g_vpChromMs = 277.0; // video path latency: Chromium (guarded by g_cs)
static double g_vpFfMs = 447.0;    // video path latency: Firefox (guarded by g_cs)
static NOTIFYICONDATAW g_nid = {};
static bool g_trayVisible = false;
static bool g_startMinimized = false;
static HICON g_appIcon = nullptr;
static HFONT g_uiFont = nullptr;

// ---------------------------------------------------------------------------
// WASAPI helpers
// ---------------------------------------------------------------------------

static IMMDevice* FindDeviceById(IMMDeviceEnumerator* en, EDataFlow flow, const std::wstring& id)
{
    if (id.empty()) return nullptr;
    IMMDeviceCollection* coll = nullptr;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll))) return nullptr;
    UINT n = 0;
    coll->GetCount(&n);
    IMMDevice* found = nullptr;
    for (UINT i = 0; i < n && !found; ++i) {
        IMMDevice* d = nullptr;
        if (FAILED(coll->Item(i, &d))) continue;
        LPWSTR did = nullptr;
        if (SUCCEEDED(d->GetId(&did)) && did && id == did) found = d; // keep reference
        if (did) CoTaskMemFree(did);
        if (!found) d->Release();
    }
    coll->Release();
    return found;
}

static void FillDeviceCombo(HWND combo, EDataFlow flow, ERole defRole, std::vector<std::wstring>& ids)
{
    ids.clear();
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&en))) return;

    std::wstring defId;
    {
        IMMDevice* def = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, defRole, &def))) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(def->GetId(&id)) && id) { defId = id; CoTaskMemFree(id); }
            def->Release();
        }
    }

    int defIdx = -1;
    IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll))) {
        UINT n = 0;
        coll->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(coll->Item(i, &d))) continue;
            wchar_t name[256] = L"?";
            IPropertyStore* ps = nullptr;
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
                    wcsncpy(name, v.pwszVal, 255);
                PropVariantClear(&v);
                ps->Release();
            }
            name[255] = 0;
            LPWSTR did = nullptr;
            if (SUCCEEDED(d->GetId(&did)) && did) { ids.push_back(did); CoTaskMemFree(did); }
            else ids.push_back(L"");
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)name);
            if (ids.back() == defId) defIdx = (int)ids.size() - 1;
            d->Release();
        }
        coll->Release();
    }
    en->Release();
    SendMessageW(combo, CB_SETCURSEL, defIdx >= 0 ? defIdx : 0, 0);
}

static void FillDeviceCombos()
{
    FillDeviceCombo(g_hRendCombo, eRender, eConsole, g_rendIds);
    FillDeviceCombo(g_hCapCombo, eCapture, eMultimedia, g_capIds);
}

static void LogStatus(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    SetWindowTextW(g_hStatus, buf);
}

struct StreamCtx {
    IAudioClient* client = nullptr;
    IAudioClock* clock = nullptr;
    WAVEFORMATEX* fmt = nullptr;
    UINT32 bufFrames = 0;
    REFERENCE_TIME latencyHns = 0;
    double sampleRate = 48000;
    int channels = 2;
    bool isFloat = true;
    int bits = 16;
    int blockAlign = 4;
};

static HRESULT InitStream(IMMDevice* dev, StreamCtx& s, REFERENCE_TIME bufHns, DWORD flags)
{
    HRESULT hr = dev->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&s.client);
    if (FAILED(hr)) return hr;
    hr = s.client->GetMixFormat(&s.fmt);
    if (FAILED(hr)) return hr;
    hr = s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufHns, 0, s.fmt, nullptr);
    if (FAILED(hr)) return hr;
    hr = s.client->GetBufferSize(&s.bufFrames);
    if (FAILED(hr)) return hr;
    s.client->GetStreamLatency(&s.latencyHns);
    s.sampleRate = (double)s.fmt->nSamplesPerSec;
    s.channels = s.fmt->nChannels ? s.fmt->nChannels : 2;
    s.blockAlign = s.fmt->nBlockAlign;
    WORD tag = s.fmt->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        auto ext = (WAVEFORMATEXTENSIBLE*)s.fmt;
        tag = ext->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT ? WAVE_FORMAT_IEEE_FLOAT : ext->SubFormat.Data1;
    }
    s.isFloat = (tag == WAVE_FORMAT_IEEE_FLOAT);
    s.bits = s.fmt->wBitsPerSample;
    return s.client->GetService(IID_IAudioClock, (void**)&s.clock);
}

static inline double FramesToHns(double frames, double rate) { return frames * 10000000.0 / rate; }
static inline double HnsToFrames(double hns, double rate) { return hns * rate / 10000000.0; }

// Read one interleaved sample as float, handling float and 16/24/32-bit PCM.
static float ReadSample(const BYTE* data, UINT32 frame, int ch, const StreamCtx& s)
{
    const BYTE* p = data + ((size_t)frame * s.channels + ch) * (s.bits / 8);
    if (s.isFloat) {
        float v; memcpy(&v, p, 4); return v;
    }
    switch (s.bits) {
    case 16: { short v; memcpy(&v, p, 2); return v / 32768.0f; }
    case 24: {
        int v = (int)p[0] | ((int)p[1] << 8) | ((int)p[2] << 16);
        if (v & 0x800000) v |= 0xFF000000; // sign-extend
        return v / 8388608.0f;
    }
    case 32: { int v; memcpy(&v, p, 4); return v / 2147483648.0f; }
    default: return 0.0f;
    }
}

// Write one interleaved sample from float, handling float and 16/24/32-bit PCM.
static void WriteSample(BYTE* data, UINT32 frame, int ch, const StreamCtx& s, float v)
{
    BYTE* p = data + ((size_t)frame * s.channels + ch) * (s.bits / 8);
    if (s.isFloat) { float f = v; memcpy(p, &f, 4); return; }
    switch (s.bits) {
    case 16: { short sv = (short)(v * 32767.0f); memcpy(p, &sv, 2); break; }
    case 24: {
        int iv = (int)(v * 8388607.0f);
        if (iv > 8388607) iv = 8388607;
        if (iv < -8388608) iv = -8388608;
        p[0] = (BYTE)(iv & 0xFF); p[1] = (BYTE)((iv >> 8) & 0xFF); p[2] = (BYTE)((iv >> 16) & 0xFF);
        break;
    }
    case 32: { int iv = (int)(v * 2147483647.0f); memcpy(p, &iv, 4); break; }
    default: break;
    }
}

static void LogToFile(const wchar_t* fmt, ...)
{
    FILE* f = _wfopen(L"C:\\Users\\Public\\avsync\\measure.log", L"a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fputws(L"\n", f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Goertzel detection
// ---------------------------------------------------------------------------

static double GoertzelPower(const float* data, size_t start, size_t len, double freq, double sr)
{
    const double coeff = 2.0 * cos(2.0 * 3.14159265358979323846 * freq / sr);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < len; ++i) {
        double s0 = data[start + i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

// ---------------------------------------------------------------------------
// Measurement session (worker thread)
// ---------------------------------------------------------------------------

struct CapturedPacket {
    double startQpc;          // wall-clock (QPC) of the FIRST frame, from packet read time + frame count
    std::vector<float> mono;  // mono mixdown
};

static DWORD WINAPI MeasureThread(LPVOID lp)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { LogStatus(L"CoInitialize failed %08lx", hr); g_running = false; delete (MeasureParams*)lp; return 1; }
    MeasureParams* params = (MeasureParams*)lp;

    IMMDeviceEnumerator* enumer = nullptr;
    IMMDevice *renderDev = nullptr, *captureDev = nullptr;
    StreamCtx rend, capt;
    IAudioRenderClient* rendClient = nullptr;
    IAudioCaptureClient* captClient = nullptr;

    auto fail = [&](const wchar_t* msg, HRESULT h) {
        LogStatus(L"%s (hr=%08lx)", msg, h);
    };

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&enumer);
    if (FAILED(hr)) { fail(L"enumerator", hr); goto done; }
    renderDev = FindDeviceById(enumer, eRender, params->rendId);
    if (!renderDev) { fail(L"playback device not found", E_FAIL); goto done; }
    captureDev = FindDeviceById(enumer, eCapture, params->capId);
    if (!captureDev && !params->loopback) { fail(L"recording device not found (pick a physical mic)", E_FAIL); goto done; }

    hr = InitStream(renderDev, rend, 3000000, 0); // 300 ms buffer
    if (FAILED(hr)) { fail(L"render init", hr); goto done; }
    if (params->loopback) {
        // Self-test: capture the render stream directly (no mic, no acoustics).
        hr = InitStream(renderDev, capt, 3000000, AUDCLNT_STREAMFLAGS_LOOPBACK);
        if (FAILED(hr)) { fail(L"loopback init", hr); goto done; }
    } else {
        hr = InitStream(captureDev, capt, 3000000, 0);
        if (FAILED(hr)) { fail(L"capture init", hr); goto done; }
    }

    wchar_t capFmt[96];
    _snwprintf_s(capFmt, _countof(capFmt), _TRUNCATE, L"%d Hz, %d ch, %d-bit %ls",
                 (int)capt.sampleRate, capt.channels, capt.bits, capt.isFloat ? L"float" : L"pcm");

    LogToFile(L"=== session start ===");
    LogToFile(L"render: %d Hz %d ch %d-bit isFloat=%d buf=%u lat=%.1fms | capt: %d Hz %d ch %d-bit isFloat=%d buf=%u lat=%.1fms loopback=%d",
              (int)rend.sampleRate, rend.channels, rend.bits, (int)rend.isFloat, rend.bufFrames,
              (double)rend.latencyHns / 10000.0,
              (int)capt.sampleRate, capt.channels, capt.bits, (int)capt.isFloat, capt.bufFrames,
              (double)capt.latencyHns / 10000.0, (int)params->loopback);

    hr = rend.client->GetService(IID_IAudioRenderClient, (void**)&rendClient);
    if (FAILED(hr)) { fail(L"render client", hr); goto done; }
    hr = capt.client->GetService(IID_IAudioCaptureClient, (void**)&captClient);
    if (FAILED(hr)) { fail(L"capture client", hr); goto done; }

    rend.client->Start();
    capt.client->Start();

    {
        LARGE_INTEGER qpf;
        QueryPerformanceFrequency(&qpf);
        const double qpcPerHns = (double)qpf.QuadPart / 10000000.0;

        std::vector<double> delays;         // all cycles (first = warm-up)
        std::vector<double> delaysNoWarm;   // for median
        int beepIndex = 0;

        while (!g_stopRequested) {
            int freq = (int)GetDlgItemInt(g_hwnd, ID_FREQ, nullptr, FALSE);
            if (freq < 100) freq = 100;
            if (freq > 8000) freq = 8000;

            // ---- splice the tone ----
            LARGE_INTEGER qpcAtTone;
            double toneStartHns = -1;
            double padHnsAtTone = 0;
            // capture QPC simultaneously with position measurement
            {
                UINT32 padding = 0;
                UINT64 pos = 0;
                rend.client->GetCurrentPadding(&padding);
                rend.clock->GetPosition(&pos, nullptr);
                QueryPerformanceCounter(&qpcAtTone);
                padHnsAtTone = FramesToHns((double)padding, rend.sampleRate);
                toneStartHns = FramesToHns((double)pos, rend.sampleRate) + padHnsAtTone;
            }
            UINT32 toneFrames = (UINT32)(0.25 * rend.sampleRate);
            {
                UINT32 padding = 0;
                rend.client->GetCurrentPadding(&padding);
                UINT32 freeFrames = rend.bufFrames > padding ? rend.bufFrames - padding : 0;
                if (freeFrames < toneFrames + 48) {
                    Sleep(20); // buffer too full; retry shortly (should not happen with 300ms buf)
                    continue;
                }
                BYTE* data = nullptr;
                if (FAILED(rendClient->GetBuffer(freeFrames, &data))) { Sleep(5); continue; }
                for (UINT32 f = 0; f < freeFrames; ++f) {
                    float v = 0.0f;
                    if (f < toneFrames)
                        v = (float)(0.9 * sin(2.0 * 3.14159265358979323846 * (double)freq * (double)f / rend.sampleRate));
                    for (int c = 0; c < rend.channels; ++c)
                        WriteSample(data, f, c, rend, v);
                }
                rendClient->ReleaseBuffer(freeFrames, 0);
            }
            // The render clock position `pos` is the DAC's current read
            // pointer; `padding` is the frames already queued ahead of it. Our
            // first tone sample is written at the back of the queue, so it
            // reaches the DAC exactly `padHnsAtTone` later. Do NOT add
            // GetStreamLatency on top: padding already accounts for the whole
            // queued delay, and double-counting makes the delay go negative.
            double emitQpc = (double)qpcAtTone.QuadPart
                           + padHnsAtTone * qpcPerHns;

            LogStatus(L"Beep #%d @ %d Hz — listening (%ls)", beepIndex + 1, freq, capFmt);

            // ---- record the analysis window ----
            const double windowHns = 25000000.0; // 2.5 s after emit
            std::vector<CapturedPacket> packets;
            double peak = 0;
            int packetCount = 0;
            int silentCount = 0;
            long totalSamples = 0;
            long zeroSamples = 0;
            double lastReadQpc = 0;
            LARGE_INTEGER qpcNow;
            double deadlineWall = 0;
            QueryPerformanceCounter(&qpcNow);
            deadlineWall = (double)qpcNow.QuadPart + 6.0 * qpf.QuadPart; // wall cap
            // Capture packet duration in QPC ticks (capture rate is constant).
            const double qpcPerFrame = (double)qpf.QuadPart / capt.sampleRate;

            while (!g_stopRequested) {
                QueryPerformanceCounter(&qpcNow);
                // drain capture
                UINT32 packetFrames = 0;
                while (SUCCEEDED(captClient->GetNextPacketSize(&packetFrames)) && packetFrames) {
                    BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
                    if (FAILED(captClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                    LARGE_INTEGER q; QueryPerformanceCounter(&q);
                    lastReadQpc = (double)q.QuadPart;
                    if (frames) {
                        CapturedPacket pk;
                        // Last frame of this packet was captured ~now (read time).
                        pk.startQpc = lastReadQpc - (double)(frames - 1) * qpcPerFrame;
                        pk.mono.resize(frames);
                        for (UINT32 f = 0; f < frames; ++f) {
                            float v = 0;
                            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                                v = ReadSample(data, f, 0, capt);
                                for (int c = 1; c < capt.channels; ++c) v += ReadSample(data, f, c, capt);
                                v /= capt.channels;
                            }
                            pk.mono[f] = v;
                            totalSamples++;
                            if (v == 0.0f) zeroSamples++;
                            double a = v < 0 ? -v : v;
                            if (a > peak) peak = a;
                        }
                        packets.push_back(std::move(pk));
                        packetCount++;
                        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) silentCount++;
                    }
                    captClient->ReleaseBuffer(frames);
                }
                // keep render topped with silence
                {
                    UINT32 padding = 0;
                    rend.client->GetCurrentPadding(&padding);
                    if (rend.bufFrames - padding > 240) {
                        BYTE* data = nullptr;
                        UINT32 n = rend.bufFrames - padding;
                        if (SUCCEEDED(rendClient->GetBuffer(n, &data))) {
                            memset(data, 0, (size_t)n * rend.blockAlign);
                            rendClient->ReleaseBuffer(n, 0);
                        }
                    }
                }
                // window end reached?
                if (lastReadQpc > 0 && (lastReadQpc - emitQpc) > windowHns * qpcPerHns) break;
                if ((double)qpcNow.QuadPart > deadlineWall) break;
                Sleep(5);
            }

            // ---- analyze ----
            size_t hop = std::max<size_t>(240, (size_t)(capt.sampleRate / 100)); // ~10 ms
            // Build ONE contiguous mono buffer across all packets (a packet can
            // be smaller than `hop` at high sample rates, so per-packet scanning
            // would never fit a Goertzel window).
            std::vector<float> buf;
            buf.reserve(totalSamples > 0 ? totalSamples : 1);
            for (auto& pk : packets) buf.insert(buf.end(), pk.mono.begin(), pk.mono.end());
            size_t N = buf.size();
            // Map a sample index to wall-clock QPC. Samples are ordered by
            // capture time; a packet's startQpc is the QPC of its first frame.
            auto idxToQpc = [&](size_t idx) -> double {
                size_t acc = 0;
                for (auto& pk : packets) {
                    if (idx < acc + pk.mono.size())
                        return pk.startQpc + (double)(idx - acc) * qpcPerFrame;
                    acc += pk.mono.size();
                }
                return 0;
            };
            std::vector<double> powers;
            double noiseMed = 0, threshold = 1.0;
            double onsetQpc = -1e18;
            double maxPower = 0;
            if (N >= 2 * hop) {
                for (size_t i = 0; i + hop <= N; i += hop)
                    powers.push_back(GoertzelPower(buf.data(), i, hop, freq, capt.sampleRate));
                std::sort(powers.begin(), powers.end());
                noiseMed = powers.empty() ? 0 : powers[powers.size() / 2];
                threshold = std::max(noiseMed * 5.0, 1.0);
                for (size_t i = 0; i + 2 * hop <= N; i += hop) {
                    double p1 = GoertzelPower(buf.data(), i, hop, freq, capt.sampleRate);
                    if (p1 > maxPower) maxPower = p1;
                    if (p1 > threshold) {
                        double p2 = GoertzelPower(buf.data(), i + hop, hop, freq, capt.sampleRate);
                        if (p2 > threshold * 0.3) {
                            onsetQpc = idxToQpc(i);
                            break;
                        }
                    }
                }
            }

            double delayMs = -1e9;
            if (onsetQpc > -1e17) {
                // The captured sample entered the buffer ~capture-latency after
                // it was acoustically produced; subtract that to get emit time.
                double onsetEmitQpc = onsetQpc - (double)capt.latencyHns * qpcPerHns;
                delayMs = (onsetEmitQpc - emitQpc) / qpcPerHns / 10000.0;
                // NOTE: delayMs is the RAW measured delay (output path + mic
                // front-end). Mic latency and video-path latency are subtracted
                // at Apply time from the editable fields, so the user can tweak
                // every term before committing.
            }

            // ---- update stats + drawing data ----
            {
                CRITICAL_SECTION* cs = &g_cs;
                EnterCriticalSection(cs);
                if (delayMs > -1e8) {
                    delays.push_back(delayMs);
                    if (beepIndex >= 1) delaysNoWarm.push_back(delayMs);
                    if (!delaysNoWarm.empty()) {
                        std::vector<double> s = delaysNoWarm;
                        std::sort(s.begin(), s.end());
                        g_medianMs = s.size() % 2 ? s[s.size() / 2] : (s[s.size() / 2 - 1] + s[s.size() / 2]) / 2;
                        g_hasMedian = true;
                    }
                }
                // build draw window: emit-300ms .. emit+2500ms
                DrawData dd;
                dd.freq = freq;
                dd.t0ms = -300;
                dd.spanms = 2800;
                dd.emitMs = 0;
                dd.heardMs = (onsetQpc > -1e17)
                    ? (onsetQpc - emitQpc) / qpcPerHns / 10000.0
                    : -1e9;
                double w0 = -0.3 * 1e7;   // -300 ms relative to emit, in hns
                double w1 = 2.5 * 1e7;    // +2500 ms
                for (auto& pk : packets) {
                    for (size_t i = 0; i < pk.mono.size(); i += 16) {
                        double tHns = (pk.startQpc + (double)i * qpcPerFrame - emitQpc) / qpcPerHns;
                        if (tHns < w0) continue;
                        if (tHns > w1) break;
                        dd.samples.push_back(pk.mono[i]);
                        dd.tms.push_back(tHns / 10000.0);
                        if (dd.samples.size() > 20000) break;
                    }
                    if (dd.samples.size() > 20000) break;
                }
                g_draw = std::move(dd);
                LeaveCriticalSection(cs);
            }

            {
                wchar_t txt[512];
                if (delayMs > -1e8) {
                    double med = 0;
                    EnterCriticalSection(&g_cs); med = g_medianMs; LeaveCriticalSection(&g_cs);
                    if (beepIndex == 0)
                        _snwprintf_s(txt, _countof(txt), _TRUNCATE,
                            L"Beep #1 @ %d Hz: %.1f ms  (warm-up, excluded)", freq, delayMs);
                    else if (g_hasMedian)
                        _snwprintf_s(txt, _countof(txt), _TRUNCATE,
                            L"Beep #%d @ %d Hz: %.1f ms   |   median %.1f ms (n=%zu)",
                            beepIndex + 1, freq, delayMs, med, delaysNoWarm.size());
                    else
                        _snwprintf_s(txt, _countof(txt), _TRUNCATE,
                            L"Beep #%d @ %d Hz: %.1f ms", beepIndex + 1, freq, delayMs);
                } else {
                    double nzPct = totalSamples ? 100.0 * (1.0 - (double)zeroSamples / totalSamples) : 0.0;
                    _snwprintf_s(txt, _countof(txt), _TRUNCATE,
                        L"Beep #%d @ %d Hz: not detected  (mic=[%ls] pkts=%d peak=%.4f nz=%.1f%% noise=%.2f maxp=%.2f)",
                        beepIndex + 1, freq, params->capName.c_str(), packetCount, peak, nzPct, noiseMed, maxPower);
                    if (packetCount == 0)
                        _snwprintf_s(txt, _countof(txt), _TRUNCATE,
                            L"Beep #%d: NO packets captured (pkts=0) - mic/device issue", beepIndex + 1);
                    else if (peak < 0.001)
                        _snwprintf_s(txt, _countof(txt), _TRUNCATE,
                            L"Beep #%d: captured %d pkts but silent (peak %.4f) - check mic privacy/mute",
                            beepIndex + 1, packetCount, peak);
                }
                SetWindowTextW(g_hStatus, txt);
                LogToFile(L"beep %d freq=%d pkts=%d peak=%.4f noise=%.2f thr=%.2f maxp=%.2f %ls",
                          beepIndex, freq, packetCount, peak, noiseMed, threshold, maxPower,
                          (delayMs > -1e8) ? L"detected" : L"none");
                LogToFile(L"  emitQpc-pad=%.3fms onsetQpc=%.3fms emitQpc=%.3fms delay=%.3fms",
                          padHnsAtTone / 10000.0,
                          (onsetQpc > -1e17 ? onsetQpc : 0) / qpcPerHns / 10000.0,
                          emitQpc / qpcPerHns / 10000.0, delayMs);
                if (!packets.empty()) {
                    wchar_t samp[256];
                    int o = _snwprintf_s(samp, _countof(samp), _TRUNCATE, L"  first8:");
                    size_t n = packets[0].mono.size() < 8 ? packets[0].mono.size() : 8;
                    for (size_t i = 0; i < n; ++i)
                        o += _snwprintf_s(samp + o, _countof(samp) - o, _TRUNCATE, L" %.3f", packets[0].mono[i]);
                    LogToFile(L"%ls", samp);
                }
                // raw dump of this window (mono float32) + sample rate, for offline analysis
                {
                    FILE* f = _wfopen(L"C:\\Users\\Public\\avsync\\capture_dump.f32", L"wb");
                    if (f) {
                        for (auto& pk : packets) fwrite(pk.mono.data(), sizeof(float), pk.mono.size(), f);
                        fclose(f);
                    }
                    FILE* m = _wfopen(L"C:\\Users\\Public\\avsync\\capture_meta.txt", L"w");
                    if (m) { fwprintf(m, L"%d\n", (int)capt.sampleRate); fclose(m); }
                }
            }
            InvalidateRect(g_hCanvas, nullptr, FALSE);

            beepIndex++;
            // keep the loop paced (~3 s per cycle handled by window above)
        }
    }

done:
    if (rendClient) { rend.client->Stop(); rendClient->Release(); }
    if (captClient) { capt.client->Stop(); captClient->Release(); }
    if (rend.clock) rend.clock->Release();
    if (capt.clock) capt.clock->Release();
    if (rend.client) rend.client->Release();
    if (capt.client) capt.client->Release();
    if (rend.fmt) CoTaskMemFree(rend.fmt);
    if (capt.fmt) CoTaskMemFree(capt.fmt);
    if (renderDev) renderDev->Release();
    if (captureDev) captureDev->Release();
    if (enumer) enumer->Release();
    CoUninitialize();
    g_running = false;
    PostMessageW(g_hwnd, WM_APP_STATUS, 0, 0);
    delete params;
    return 0;
}

// ---------------------------------------------------------------------------
// Microphone input-latency calibration (keyboard click method)
// ---------------------------------------------------------------------------
// Method: hold the mic against the keyboard, press SPACE. The key-down event
// gives a precise QPC timestamp (keyQpc). The key's mechanical click is a
// broadband impulse; we detect its onset in the mic stream (amplitude edge, not
// Goertzel) and take its QPC (onsetQpc). micLatency = onsetQpc - keyQpc, i.e.
// the mic input-path latency. This is later subtracted from the main A/V delay.

static DWORD WINAPI CalibrateThread(LPVOID lp)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { LogStatus(L"CoInitialize failed %08lx", hr); g_calRunning = false; PostMessageW(g_hwnd, WM_APP_STATUS, 0, 0); delete (MeasureParams*)lp; return 1; }
    MeasureParams* params = (MeasureParams*)lp;

    IMMDeviceEnumerator* enumer = nullptr;
    IMMDevice* captureDev = nullptr;
    StreamCtx capt;
    IAudioCaptureClient* captClient = nullptr;

    auto fail = [&](const wchar_t* msg, HRESULT h) {
        LogStatus(L"%s (hr=%08lx)", msg, h);
    };

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&enumer);
    if (FAILED(hr)) { fail(L"enumerator", hr); goto done; }
    captureDev = FindDeviceById(enumer, eCapture, params->capId);
    if (!captureDev) { fail(L"recording device not found (pick a physical mic)", E_FAIL); goto done; }
    hr = InitStream(captureDev, capt, 3000000, 0);
    if (FAILED(hr)) { fail(L"capture init", hr); goto done; }
    hr = capt.client->GetService(IID_IAudioCaptureClient, (void**)&captClient);
    if (FAILED(hr)) { fail(L"capture client", hr); goto done; }
    capt.client->Start();

    LogToFile(L"=== calibrate start ===");
    LogToFile(L"capt: %d Hz %d ch %d-bit isFloat=%d lat=%.1fms",
              (int)capt.sampleRate, capt.channels, capt.bits, (int)capt.isFloat,
              (double)capt.latencyHns / 10000.0);

    {
        LARGE_INTEGER qpf;
        QueryPerformanceFrequency(&qpf);
        const double qpcPerFrame = (double)qpf.QuadPart / capt.sampleRate;

        LogStatus(L"Calibrating: hold the mic against the keyboard, then press SPACEBAR...");

        // ---- wait for the SPACE key-down, capture the surrounding audio ----
        // Capture continuously; when a key-down is seen we mark keyQpc and keep
        // draining for ~1 s after it so the click is well inside the buffer.
        std::vector<CapturedPacket> packets;
        double keyQpc = 0;
        double lastReadQpc = 0;

        // pre-roll: discard leading packets until ~100 ms before the key so the
        // buffer starts just before the click (keeps memory small).
        auto drain = [&](std::vector<CapturedPacket>* out, double keyQ, double cutQpc, bool discardBefore) {
            UINT32 packetFrames = 0;
            while (SUCCEEDED(captClient->GetNextPacketSize(&packetFrames)) && packetFrames) {
                BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
                if (FAILED(captClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                LARGE_INTEGER q; QueryPerformanceCounter(&q);
                lastReadQpc = (double)q.QuadPart;
                if (frames) {
                    CapturedPacket pk;
                    pk.startQpc = lastReadQpc - (double)(frames - 1) * qpcPerFrame;
                    if (discardBefore && pk.startQpc < cutQpc) {
                        captClient->ReleaseBuffer(frames);
                        continue;
                    }
                    pk.mono.resize(frames);
                    for (UINT32 f = 0; f < frames; ++f) {
                        float v = 0;
                        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                            v = ReadSample(data, f, 0, capt);
                            for (int c = 1; c < capt.channels; ++c) v += ReadSample(data, f, c, capt);
                            v /= capt.channels;
                        }
                        pk.mono[f] = v;
                    }
                    out->push_back(std::move(pk));
                }
                captClient->ReleaseBuffer(frames);
            }
        };

        bool wasDown = false;
        while (!g_stopRequested) {
            drain(&packets, 0, 0, false);
            bool down = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
            if (down && !wasDown) {
                LARGE_INTEGER q; QueryPerformanceCounter(&q);
                keyQpc = (double)q.QuadPart;
                break;
            }
            wasDown = down;
            Sleep(2);
        }

        if (g_stopRequested || keyQpc == 0) { LogStatus(L"Calibration cancelled (no SPACE pressed)."); goto done; }

        // discard packets before ~50 ms prior to the key to bound memory, then
        // keep capturing until ~1 s after the key.
        std::vector<CapturedPacket> keep;
        double cutQpc = keyQpc - 0.05 * (double)qpf.QuadPart; // 50 ms before key
        for (auto& pk : packets) {
            if (pk.startQpc >= cutQpc) keep.push_back(std::move(pk));
        }
        packets = std::move(keep);

        while (!g_stopRequested) {
            drain(&packets, 0, 0, false);
            if (lastReadQpc > 0 && (lastReadQpc - keyQpc) > 1.0 * (double)qpf.QuadPart) break;
            Sleep(2);
        }

        // ---- build contiguous buffer, detect the click onset by amplitude ----
        std::vector<float> buf;
        size_t total = 0;
        for (auto& pk : packets) { buf.insert(buf.end(), pk.mono.begin(), pk.mono.end()); total += pk.mono.size(); }
        size_t N = buf.size();
        auto idxToQpc = [&](size_t idx) -> double {
            size_t acc = 0;
            for (auto& pk : packets) {
                if (idx < acc + pk.mono.size())
                    return pk.startQpc + (double)(idx - acc) * qpcPerFrame;
                acc += pk.mono.size();
            }
            return 0;
        };

        // noise floor = median abs over the whole buffer
        std::vector<float> ab(buf.size());
        for (size_t i = 0; i < N; ++i) ab[i] = fabsf(buf[i]);
        std::sort(ab.begin(), ab.end());
        double noise = ab.empty() ? 0 : ab[N / 2];
        double threshold = std::max(noise * 8.0, 0.05);

        double onsetQpc = 0;
        // search from ~5 ms before the key to 500 ms after it
        double loQpc = keyQpc - 0.005 * (double)qpf.QuadPart;
        double hiQpc = keyQpc + 0.5 * (double)qpf.QuadPart;
        for (size_t i = 0; i < N; ++i) {
            double t = idxToQpc(i);
            if (t < loQpc) continue;
            if (t > hiQpc) break;
            if (fabsf(buf[i]) > threshold) { onsetQpc = t; break; }
        }

        if (onsetQpc > 0) {
            double micLatMs = (onsetQpc - keyQpc) / (double)qpf.QuadPart * 1000.0;
            EnterCriticalSection(&g_cs);
            g_micCalMs = micLatMs;
            g_hasMicCal = true;
            LeaveCriticalSection(&g_cs);
            SetDlgItemInt(g_hwnd, ID_MIC, (UINT)(micLatMs + 0.5), FALSE);
            LogStatus(L"Calibration done: mic input latency %.1f ms.", micLatMs);
            LogToFile(L"calibrate: noise=%.4f thr=%.4f keyQpc=%.3fms onsetQpc=%.3fms lat=%.3fms",
                      noise, threshold, keyQpc / (double)qpf.QuadPart * 1000.0,
                      onsetQpc / (double)qpf.QuadPart * 1000.0, micLatMs);
        } else {
            LogStatus(L"Calibration failed: no click detected. Move the mic closer to the key.");
            LogToFile(L"calibrate: NO ONSET noise=%.4f thr=%.4f N=%zu", noise, threshold, N);
        }
    }

done:
    if (captClient) { capt.client->Stop(); captClient->Release(); }
    if (capt.clock) capt.clock->Release();
    if (capt.client) capt.client->Release();
    if (capt.fmt) CoTaskMemFree(capt.fmt);
    if (captureDev) captureDev->Release();
    if (enumer) enumer->Release();
    CoUninitialize();
    g_calRunning = false;
    PostMessageW(g_hwnd, WM_APP_STATUS, 0, 0);
    delete params;
    return 0;
}

// ---------------------------------------------------------------------------
// Config shared memory (create-if-missing, then write)
// ---------------------------------------------------------------------------

// Write the effective per-browser delays. globalMs is the fallback for any
// browser without a specific override (Chromium/Yandex use this); ffMs is the
// Firefox override (its own video path latency differs).
static bool WriteConfig(long globalMs, long ffMs)
{
    HANDLE h = OpenFileMappingW(FILE_MAP_WRITE, FALSE, AVSYNC_SHM_NAME);
    bool created = false;
    if (!h) {
        h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(AVSYNC_CONFIG), AVSYNC_SHM_NAME);
        created = true;
    }
    if (!h) return false;
    auto view = (AVSYNC_CONFIG*)MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, sizeof(AVSYNC_CONFIG));
    if (!view) { CloseHandle(h); return false; }
    if (created || view->magic != AVSYNC_MAGIC) {
        memset(view, 0, sizeof(*view));
        view->magic = AVSYNC_MAGIC;
        view->version = AVSYNC_VERSION;
        view->enabled = 1;
    }
    InterlockedExchange((volatile LONG*)&view->globalDelayMs, globalMs);
    // upsert the firefox.exe override
    unsigned idx = view->browserCount;
    for (unsigned i = 0; i < view->browserCount; ++i) {
        if (_wcsicmp(view->browsers[i].exe, L"firefox.exe") == 0) { idx = i; break; }
    }
    if (idx < AVSYNC_MAX_BROWSERS) {
        wcsncpy(view->browsers[idx].exe, L"firefox.exe", AVSYNC_EXE_LEN - 1);
        view->browsers[idx].exe[AVSYNC_EXE_LEN - 1] = 0;
        InterlockedExchange((volatile LONG*)&view->browsers[idx].delayMs, ffMs);
        if (idx == view->browserCount)
            InterlockedExchange((volatile LONG*)&view->browserCount, idx + 1);
    }
    UnmapViewOfFile(view);
    if (created) { /* leak intentionally: section owned by this process */ }
    else CloseHandle(h);
    return true;
}

// Read the current config. Returns false if the section doesn't exist yet.
static bool ReadConfig(long* globalMs, long* ffMs, unsigned* enabled)
{
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, AVSYNC_SHM_NAME);
    if (!h) return false;
    auto view = (AVSYNC_CONFIG*)MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(AVSYNC_CONFIG));
    if (!view) { CloseHandle(h); return false; }
    bool ok = (view->magic == AVSYNC_MAGIC);
    if (ok) {
        if (globalMs) *globalMs = view->globalDelayMs;
        if (enabled) *enabled = view->enabled;
        if (ffMs) {
            *ffMs = -1;
            for (unsigned i = 0; i < view->browserCount; ++i) {
                if (_wcsicmp(view->browsers[i].exe, L"firefox.exe") == 0) { *ffMs = view->browsers[i].delayMs; break; }
            }
        }
    }
    UnmapViewOfFile(view);
    CloseHandle(h);
    return ok;
}

// ---------------------------------------------------------------------------
// Canvas drawing (waveform + ms scale + markers)
// ---------------------------------------------------------------------------

static LRESULT CALLBACK CanvasProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);
        RECT rc;
        GetClientRect(hw, &rc);
        int W = rc.right, H = rc.bottom;
        const int axisH = 26;
        int plotH = H - axisH;

        HBRUSH bg = CreateSolidBrush(RGB(235, 235, 235));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        RECT axrc = { 0, plotH, W, H };
        HBRUSH axbg = CreateSolidBrush(RGB(220, 220, 220));
        FillRect(dc, &axrc, axbg);
        DeleteObject(axbg);

        DrawData dd;
        EnterCriticalSection(&g_cs);
        dd = g_draw;
        LeaveCriticalSection(&g_cs);

        SetBkMode(dc, TRANSPARENT);

        auto msToX = [&](double ms) { return (int)((ms - dd.t0ms) / dd.spanms * W); };

        // waveform (drawn by per-sample time, so it aligns with the scale/markers)
        if (dd.samples.size() > 1 && dd.tms.size() == dd.samples.size()) {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(20, 20, 20));
            HGDIOBJ old = SelectObject(dc, pen);
            int px = msToX(dd.tms[0]);
            int py = plotH / 2 - (int)(dd.samples[0] * plotH * 0.45);
            for (size_t i = 1; i < dd.samples.size(); ++i) {
                int x = msToX(dd.tms[i]);
                int y = plotH / 2 - (int)(dd.samples[i] * plotH * 0.45);
                MoveToEx(dc, px, py, nullptr);
                LineTo(dc, x, y);
                px = x; py = y;
            }
            SelectObject(dc, old);
            DeleteObject(pen);
        }

        // ms scale anchored at emit (0 ms)
        HPEN axpen = CreatePen(PS_SOLID, 1, RGB(90, 90, 90));
        HGDIOBJ oldp = SelectObject(dc, axpen);
        HFONT fnt = CreateFontW(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Arial");
        HGDIOBJ oldf = SelectObject(dc, fnt);
        SetTextColor(dc, RGB(60, 60, 60));
        for (int ms = -200; ms <= 2500; ms += 100) {
            if (ms < dd.t0ms || ms > dd.t0ms + dd.spanms) continue;
            int x = msToX(ms);
            bool major = (ms % 500 == 0);
            MoveToEx(dc, x, plotH, nullptr);
            LineTo(dc, x, plotH + (major ? 9 : 5));
            if (major && x > 26 && x < W - 30) {
                wchar_t lab[32];
                _snwprintf_s(lab, _countof(lab), _TRUNCATE, ms > 0 ? L"+%d ms" : L"%d ms", ms);
                SetTextAlign(dc, TA_CENTER);
                TextOutW(dc, x, plotH + 10, lab, (int)wcslen(lab));
            }
        }
        MoveToEx(dc, 0, plotH, nullptr);
        LineTo(dc, W, plotH);

        // markers
        int ex = msToX(dd.emitMs);
        HPEN redpen = CreatePen(PS_SOLID, 2, RGB(210, 0, 0));
        SelectObject(dc, redpen);
        MoveToEx(dc, ex, 0, nullptr); LineTo(dc, ex, plotH);
        wchar_t rl[64];
        _snwprintf_s(rl, _countof(rl), _TRUNCATE, L"play %d Hz", dd.freq);
        SetTextAlign(dc, TA_LEFT);
        SetTextColor(dc, RGB(210, 0, 0));
        TextOutW(dc, ex + 5, 4, rl, (int)wcslen(rl));
        DeleteObject(redpen);

        if (dd.heardMs > -1e8) {
            int hx = msToX(dd.heardMs);
            HPEN bluepen = CreatePen(PS_SOLID, 2, RGB(0, 0, 210));
            SelectObject(dc, bluepen);
            MoveToEx(dc, hx, 0, nullptr); LineTo(dc, hx, plotH);
            SetTextColor(dc, RGB(0, 0, 210));
            TextOutW(dc, hx + 5, 22, L"heard", 5);
            DeleteObject(bluepen);

            // green bracket with measured delay
            HPEN grpen = CreatePen(PS_SOLID, 2, RGB(10, 125, 0));
            SelectObject(dc, grpen);
            int by = 52;
            MoveToEx(dc, ex, by, nullptr); LineTo(dc, hx, by);
            MoveToEx(dc, ex, by - 5, nullptr); LineTo(dc, ex, by + 5);
            MoveToEx(dc, hx, by - 5, nullptr); LineTo(dc, hx, by + 5);
            wchar_t dl[32];
            _snwprintf_s(dl, _countof(dl), _TRUNCATE, L"%.0f ms", dd.heardMs);
            HFONT bf = CreateFontW(16, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Arial");
            SelectObject(dc, bf);
            SIZE sz; GetTextExtentPoint32W(dc, dl, (int)wcslen(dl), &sz);
            int lx = std::min<int>(std::max<int>((ex + hx) / 2 - (int)sz.cx / 2, 2), W - (int)sz.cx - 4);
            RECT box = { lx - 3, by - 21, lx + sz.cx + 3, by - 3 };
            HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255));
            FillRect(dc, &box, wb);
            DeleteObject(wb);
            SetTextColor(dc, RGB(10, 125, 0));
            TextOutW(dc, lx, by - 20, dl, (int)wcslen(dl));
            DeleteObject(bf);
            DeleteObject(grpen);
        }

        SelectObject(dc, oldf);
        DeleteObject(fnt);
        SelectObject(dc, oldp);
        DeleteObject(axpen);
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Main window
// ---------------------------------------------------------------------------

// Refresh the "current settings" label from shared memory.
static void RefreshCurrentConfig()
{
    long globalMs = -1, ffMs = -1; unsigned enabled = 0;
    bool ok = ReadConfig(&globalMs, &ffMs, &enabled);
    wchar_t t[512];
    if (!ok) {
        _snwprintf_s(t, _countof(t), _TRUNCATE,
                     L"Current config: not set yet (no hook/watch running).");
    } else {
        wchar_t ff[32];
        if (ffMs < 0) wcscpy_s(ff, L"—"); else _snwprintf_s(ff, _countof(ff), _TRUNCATE, L"%ld ms", ffMs);
        _snwprintf_s(t, _countof(t), _TRUNCATE,
                     L"Current config: %ls   global = %ld ms   firefox = %ls",
                     enabled ? L"ON" : L"OFF", globalMs, ff);
    }
    SetWindowTextW(GetDlgItem(g_hwnd, ID_CUR), t);
}

static double ReadEditDbl(HWND hw, int id, double defval)
{
    BOOL ok = FALSE;
    UINT v = GetDlgItemInt(hw, id, &ok, FALSE);
    return ok ? (double)v : defval;
}

// Recompute and show the would-be per-browser delays from the editable fields:
//   delay = median − mic_latency − video_path_latency(browser)
static void UpdatePreview()
{
    HWND hw = g_hwnd;
    double median = ReadEditDbl(hw, ID_MEDIAN, 0.0);
    double mic = ReadEditDbl(hw, ID_MIC, 0.0);
    double vpC = ReadEditDbl(hw, ID_VP_CHROM, 277.0);
    double vpF = ReadEditDbl(hw, ID_VP_FF, 447.0);
    long g = (long)(median - mic - vpC + 0.5);
    long f = (long)(median - mic - vpF + 0.5);
    wchar_t t[384];
    _snwprintf_s(t, _countof(t), _TRUNCATE,
                 L"Will apply: global = %ld ms · firefox = %ld ms    (median %.0f − mic %.0f − video path)",
                 g, f, median, mic);
    SetWindowTextW(GetDlgItem(hw, ID_PREVIEW), t);
}

// ---------------------------------------------------------------------------
// UI language (RU default, EN optional)
// ---------------------------------------------------------------------------
enum class Lang { RU, EN };
static Lang g_lang = Lang::RU;

struct StrPair { const wchar_t* ru; const wchar_t* en; };
static const wchar_t* T(StrPair p) { return g_lang == Lang::EN ? p.en : p.ru; }
#define SP(ru, en) StrPair{ L##ru, L##en }

static StrPair STR_VIDEO_PATH = SP("Задержка видеотракта", "Video path latency");
static StrPair STR_CHROMIUM   = SP("Chromium / Яндекс:", "Chromium / Yandex:");
static StrPair STR_FIREFOX    = SP("Firefox:", "Firefox:");
static StrPair STR_OWN_DELAY  = SP("собственная задержка вывода", "browser's own output delay");
static StrPair STR_RESET      = SP("Сброс", "Reset");
static StrPair STR_DEVICES    = SP("Аудиоустройства", "Audio devices");
static StrPair STR_PLAYBACK   = SP("Воспроизведение:", "Playback:");
static StrPair STR_RECORDING  = SP("Запись:", "Recording:");
static StrPair STR_MIC_CAL    = SP("Калибровка микрофона", "Microphone calibration");
static StrPair STR_CALIBRATE  = SP("Калибровка (микрофон у клавиатуры, ПРОБЕЛ)", "Calibrate (mic near keyboard, press SPACE)");
static StrPair STR_MIC_LAT    = SP("Задержка входа микрофона:", "Mic input latency:");
static StrPair STR_TEST       = SP("Тест задержки", "Delay test");
static StrPair STR_FREQ       = SP("Частота тона (Гц):", "Tone frequency (Hz):");
static StrPair STR_START      = SP("Запустить тест", "Start Test");
static StrPair STR_STOP       = SP("Остановить тест", "Stop Test");
static StrPair STR_LOOPBACK   = SP("Самотест: loopback (без микрофона)", "Self-test: loopback (no mic)");
static StrPair STR_RESULT     = SP("Результат", "Result");
static StrPair STR_MEDIAN     = SP("Измеренная задержка (медиана):", "Measured delay (median):");
static StrPair STR_APPLY      = SP("Применить", "Apply");
static StrPair STR_DISABLE    = SP("Отключить", "Disable");
static StrPair STR_SETZERO    = SP("задержка 0 (прозрачный режим)", "sets delay to 0 (pass-through)");
static StrPair STR_REFRESH    = SP("Обновить", "Refresh");
static StrPair STR_SYSTEM     = SP("Система", "System");
static StrPair STR_AUTOSTART  = SP("Автозапуск при входе в Windows (свёрнуто в трей)", "Autostart at Windows startup (minimized to tray)");
static StrPair STR_LANG       = SP("Язык:", "Language:");
static StrPair STR_WINTITLE   = SP("Синхронизация звука видео", "Audio-Video Sync");
static StrPair STR_TRAY_TIP   = SP("Синхронизация звука видео", "Audio-Video Sync");
static StrPair STR_RESTORE    = SP("Восстановить", "Restore");
static StrPair STR_EXIT       = SP("Выход", "Exit");
static StrPair STR_CLOSE_TITLE= SP("AV Sync Measure", "AV Sync Measure");
static StrPair STR_CLOSE_MSG  = SP("Отключить задержку и выйти?\n\nДа — отключить задержку (0 мс) и закрыть\nНет — свернуть в трей",
                                   "Disable the delay and exit?\n\nYes — disable the delay (0 ms) and close\nNo — minimize to tray");

// ---------------------------------------------------------------------------
// App icon (gramophone), drawn into a 32x32 DIB and made an HICON.
// ---------------------------------------------------------------------------
static HICON MakeAppIcon()
{
    const int S = 32;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = -S; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { DeleteDC(dc); return nullptr; }
    HGDIOBJ old = SelectObject(dc, bmp);
    RECT r = { 0, 0, S, S };
    // transparent background
    HBRUSH tbr = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &r, tbr); DeleteObject(tbr);
    // transparent mask trick: clear alpha by zeroing bits first
    if (bits) memset(bits, 0, S * S * 4);

    auto px = [&](int x, int y, COLORREF c, BYTE a = 255) {
        if (x < 0 || y < 0 || x >= S || y >= S) return;
        BYTE* p = (BYTE*)bits + (y * S + x) * 4;
        p[0] = GetBValue(c); p[1] = GetGValue(c); p[2] = GetRValue(c); p[3] = a;
    };
    auto circle = [&](int cx, int cy, int rad, COLORREF c) {
        for (int y = cy - rad; y <= cy + rad; ++y)
            for (int x = cx - rad; x <= cx + rad; ++x)
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= rad * rad)
                    px(x, y, c);
    };

    // Gramophone (mechanical record player): a large flared horn feeding into
    // a wooden base box with a turntable/record on top and a crank handle.
    COLORREF brass = RGB(176, 128, 44);
    COLORREF brassDark = RGB(120, 84, 26);
    COLORREF wood = RGB(96, 60, 28);
    COLORREF woodDark = RGB(60, 36, 14);
    COLORREF record = RGB(24, 24, 24);

    // --- horn: flared trapezoid opening to the upper-right ---
    for (int y = 3; y <= 24; ++y) {
        float t = (y - 3) / 21.0f;               // 0 at top, 1 at neck
        int x0 = 16 + (int)(-8 * t);             // left edge flares left as it rises
        int x1 = 24 + (int)(4 * t);              // right edge
        for (int x = x0; x <= x1; ++x) px(x, y, brass);
    }
    // horn mouth outline (darker rim at the top opening)
    for (int x = 6; x <= 27; ++x) { px(x, 3, brassDark); px(x, 4, brassDark); }

    // --- base box (wood) ---
    for (int y = 22; y <= 28; ++y)
        for (int x = 5; x <= 26; ++x) px(x, y, (y >= 26 ? woodDark : wood));

    // --- turntable / record on top of the base ---
    circle(10, 23, 5, record);          // record
    px(10, 23, brass);                  // spindle
    // record groove
    circle(10, 23, 3, RGB(40, 40, 40));
    px(10, 23, brass);

    // --- crank handle on the right side ---
    for (int x = 24; x <= 29; ++x) px(x, 20, brassDark);
    px(29, 19, brassDark); px(30, 20, brassDark);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = bmp;
    ii.hbmMask = CreateBitmap(S, S, 1, 1, nullptr);
    HICON icon = CreateIconIndirect(&ii);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    return icon;
}

// ---------------------------------------------------------------------------
// Autostart (registry Run key) + system tray helpers
// ---------------------------------------------------------------------------
static bool IsAutostartEnabled()
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[1024]; DWORD sz = sizeof(buf);
    LONG r = RegGetValueW(k, nullptr, L"AVSyncMeasure", RRF_RT_REG_SZ, nullptr, buf, &sz);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static bool SetAutostart(bool on)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    bool ok = true;
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        wchar_t val[MAX_PATH + 32];
        _snwprintf_s(val, _countof(val), _TRUNCATE, L"\"%ls\" --minimized", exe);
        ok = (RegSetValueExW(k, L"AVSyncMeasure", 0, REG_SZ,
                             (const BYTE*)val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
    } else {
        ok = (RegDeleteValueW(k, L"AVSyncMeasure") == ERROR_SUCCESS ||
              RegDeleteValueW(k, L"AVSyncMeasure") == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(k);
    return ok;
}

static void AddTrayIcon(HWND hw)
{
    if (g_trayVisible) return;
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hw;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_appIcon ? g_appIcon : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, T(STR_TRAY_TIP));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_trayVisible = true;
}

static void RemoveTrayIcon()
{
    if (!g_trayVisible) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayVisible = false;
}

static void RestoreFromTray(HWND hw)
{
    ShowWindow(hw, SW_RESTORE);
    ShowWindow(hw, SW_SHOW);
    SetForegroundWindow(hw);
}

// Create the standard Windows UI font (Segoe UI, from NONCLIENTMETRICS).
static HFONT MakeUiFont()
{
    NONCLIENTMETRICSW ncm;
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        return nullptr;
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

static BOOL CALLBACK SetFontEnumProc(HWND hw, LPARAM lParam)
{
    SendMessageW(hw, WM_SETFONT, (WPARAM)lParam, TRUE);
    return TRUE;
}

static void ApplyUiFont(HWND hw)
{
    if (!g_uiFont) return;
    EnumChildWindows(hw, SetFontEnumProc, (LPARAM)g_uiFont);
    InvalidateRect(hw, nullptr, TRUE);
}

// Apply the current UI language to all labels, groups and buttons.
static void ApplyLanguage(){
    HWND hw = g_hwnd;
    SetWindowTextW(GetDlgItem(hw, ID_GRP_VIDEO), T(STR_VIDEO_PATH));
    SetWindowTextW(GetDlgItem(hw, ID_L_CHROMIUM), T(STR_CHROMIUM));
    SetWindowTextW(GetDlgItem(hw, ID_L_FIREFOX), T(STR_FIREFOX));
    SetWindowTextW(GetDlgItem(hw, ID_L_OWN), T(STR_OWN_DELAY));
    SetWindowTextW(GetDlgItem(hw, ID_VP_CHROM_RST), T(STR_RESET));
    SetWindowTextW(GetDlgItem(hw, ID_VP_FF_RST), T(STR_RESET));
    SetWindowTextW(GetDlgItem(hw, ID_GRP_DEVICES), T(STR_DEVICES));
    SetWindowTextW(GetDlgItem(hw, ID_L_PLAYBACK), T(STR_PLAYBACK));
    SetWindowTextW(GetDlgItem(hw, ID_L_RECORDING), T(STR_RECORDING));
    SetWindowTextW(GetDlgItem(hw, ID_GRP_MIC), T(STR_MIC_CAL));
    SetWindowTextW(GetDlgItem(hw, ID_CAL), T(STR_CALIBRATE));
    SetWindowTextW(GetDlgItem(hw, ID_L_MICLAT), T(STR_MIC_LAT));
    SetWindowTextW(GetDlgItem(hw, ID_GRP_TEST), T(STR_TEST));
    SetWindowTextW(GetDlgItem(hw, ID_L_FREQ), T(STR_FREQ));
    SetWindowTextW(GetDlgItem(hw, ID_LOOPBACK), T(STR_LOOPBACK));
    SetWindowTextW(GetDlgItem(hw, ID_GRP_RESULT), T(STR_RESULT));
    SetWindowTextW(GetDlgItem(hw, ID_L_MEDIAN), T(STR_MEDIAN));
    SetWindowTextW(GetDlgItem(hw, ID_APPLY), T(STR_APPLY));
    SetWindowTextW(GetDlgItem(hw, ID_DISABLE), T(STR_DISABLE));
    SetWindowTextW(GetDlgItem(hw, ID_L_SETZERO), T(STR_SETZERO));
    SetWindowTextW(GetDlgItem(hw, ID_REFRESH), T(STR_REFRESH));
    SetWindowTextW(GetDlgItem(hw, ID_GRP_SYSTEM), T(STR_SYSTEM));
    SetWindowTextW(GetDlgItem(hw, ID_AUTOSTART), T(STR_AUTOSTART));
    SetWindowTextW(GetDlgItem(hw, ID_L_LANG), T(STR_LANG));
    SetWindowTextW(hw, T(STR_WINTITLE));
    SetWindowTextW(GetDlgItem(hw, ID_START), g_running ? T(STR_STOP) : T(STR_START));
    if (g_trayVisible) {
        wcscpy_s(g_nid.szTip, T(STR_TRAY_TIP));
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
}

// ---------------------------------------------------------------------------
// Watcher: inject the embedded hook DLL into browser audio processes.
// Runs for the lifetime of the GUI, so a single exe is fully self-contained.
// ---------------------------------------------------------------------------
static DWORD WINAPI WatchThread(LPVOID)
{
    EnableDebugPrivilege();
    std::vector<unsigned char> dll(g_embedded_hook_dll, g_embedded_hook_dll + g_embedded_hook_dll_len);
    std::set<DWORD> injected;
    while (!g_watchStop) {
        auto procs = ListProcesses();
        std::set<DWORD> alive;
        std::vector<std::wstring> extra;
        for (auto& p : procs) {
            if (!IsAudioTarget(p, extra)) continue;
            alive.insert(p.pid);
            if (injected.count(p.pid)) continue;
            if (InjectDll(p.pid, dll)) {
                LogToFile(L"watch: injected pid %lu (%ls)", p.pid, p.name.c_str());
            } else {
                LogToFile(L"watch: inject FAILED pid %lu (%ls)", p.pid, p.name.c_str());
            }
            injected.insert(p.pid); // don't hammer failing targets
        }
        for (auto it = injected.begin(); it != injected.end(); )
            it = alive.count(*it) ? std::next(it) : injected.erase(it);
        Sleep(800);
    }
    return 0;
}

static LRESULT CALLBACK WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hw;

        // ============ Group 1: Video path latency ============
        CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                        12, 8, 800, 60, hw, (HMENU)(INT_PTR)ID_GRP_VIDEO, nullptr, nullptr);

        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                        22, 34, 140, 22, hw, (HMENU)(INT_PTR)ID_L_CHROMIUM, nullptr, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"277", WS_CHILD | WS_VISIBLE | ES_NUMBER,
                        168, 30, 60, 26, hw, (HMENU)(INT_PTR)ID_VP_CHROM, nullptr, nullptr);
        CreateWindowExW(0, L"STATIC", L"ms", WS_CHILD | WS_VISIBLE, 234, 34, 22, 20, hw, nullptr, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE,
                        262, 30, 54, 26, hw, (HMENU)(INT_PTR)ID_VP_CHROM_RST, nullptr, nullptr);

        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                        336, 34, 70, 22, hw, (HMENU)(INT_PTR)ID_L_FIREFOX, nullptr, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"447", WS_CHILD | WS_VISIBLE | ES_NUMBER,
                        412, 30, 60, 26, hw, (HMENU)(INT_PTR)ID_VP_FF, nullptr, nullptr);
        CreateWindowExW(0, L"STATIC", L"ms", WS_CHILD | WS_VISIBLE, 478, 34, 22, 20, hw, nullptr, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE,
                        506, 30, 54, 26, hw, (HMENU)(INT_PTR)ID_VP_FF_RST, nullptr, nullptr);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                        580, 34, 220, 22, hw, (HMENU)(INT_PTR)ID_L_OWN, nullptr, nullptr);

        // ============ Group 2: Devices ============
        CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                        12, 76, 800, 58, hw, (HMENU)(INT_PTR)ID_GRP_DEVICES, nullptr, nullptr);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                        22, 100, 110, 20, hw, (HMENU)(INT_PTR)ID_L_PLAYBACK, nullptr, nullptr);
        g_hRendCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                        138, 96, 260, 220, hw, (HMENU)(INT_PTR)ID_RENDDEV, nullptr, nullptr);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                        414, 100, 110, 20, hw, (HMENU)(INT_PTR)ID_L_RECORDING, nullptr, nullptr);
        g_hCapCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                        530, 96, 268, 220, hw, (HMENU)(INT_PTR)ID_CAPDEV, nullptr, nullptr);

        // ============ Group 3: Microphone calibration ============
        CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                        12, 142, 800, 58, hw, (HMENU)(INT_PTR)ID_GRP_MIC, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE,
                        22, 162, 360, 26, hw, (HMENU)(INT_PTR)ID_CAL, nullptr, nullptr);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                        394, 166, 180, 20, hw, (HMENU)(INT_PTR)ID_L_MICLAT, nullptr, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER,
                        580, 162, 64, 26, hw, (HMENU)(INT_PTR)ID_MIC, nullptr, nullptr);
        CreateWindowExW(0, L"STATIC", L"ms", WS_CHILD | WS_VISIBLE, 650, 166, 22, 20, hw, nullptr, nullptr, nullptr);

        // ============ Group 4: Delay test ============
        CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                        12, 208, 800, 238, hw, (HMENU)(INT_PTR)ID_GRP_TEST, nullptr, nullptr);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                        22, 232, 152, 22, hw, (HMENU)(INT_PTR)ID_L_FREQ, nullptr, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1000", WS_CHILD | WS_VISIBLE | ES_NUMBER,
                        180, 228, 70, 26, hw, (HMENU)(INT_PTR)ID_FREQ, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        264, 226, 110, 30, hw, (HMENU)(INT_PTR)ID_START, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                        390, 232, 210, 22, hw, (HMENU)(INT_PTR)ID_LOOPBACK, nullptr, nullptr);

        g_hStatus = CreateWindowExW(0, L"STATIC", L"",
                                    WS_CHILD | WS_VISIBLE, 22, 262, 780, 34, hw, (HMENU)(INT_PTR)ID_STATUS, nullptr, nullptr);
        g_hCanvas = CreateWindowExW(WS_EX_CLIENTEDGE, L"AVSYNC_CANVAS", L"", WS_CHILD | WS_VISIBLE,
                                    22, 300, 780, 136, hw, (HMENU)(INT_PTR)ID_CANVAS, nullptr, nullptr);

        // ============ Group 5: Result ============
        CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                        12, 454, 800, 128, hw, (HMENU)(INT_PTR)ID_GRP_RESULT, nullptr, nullptr);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                        22, 482, 220, 22, hw, (HMENU)(INT_PTR)ID_L_MEDIAN, nullptr, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER,
                        248, 478, 70, 26, hw, (HMENU)(INT_PTR)ID_MEDIAN, nullptr, nullptr);
        CreateWindowExW(0, L"STATIC", L"ms", WS_CHILD | WS_VISIBLE, 324, 482, 22, 20, hw, nullptr, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        358, 476, 90, 30, hw, (HMENU)(INT_PTR)ID_APPLY, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE,
                        456, 476, 90, 30, hw, (HMENU)(INT_PTR)ID_DISABLE, nullptr, nullptr);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                        556, 480, 250, 22, hw, (HMENU)(INT_PTR)ID_L_SETZERO, nullptr, nullptr);

        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                        22, 514, 780, 24, hw, (HMENU)(INT_PTR)ID_PREVIEW, nullptr, nullptr);

        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                        22, 548, 650, 20, hw, (HMENU)(INT_PTR)ID_CUR, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE,
                        684, 546, 116, 24, hw, (HMENU)(INT_PTR)ID_REFRESH, nullptr, nullptr);

        // ============ Group 6: System ============
        CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                        12, 590, 800, 50, hw, (HMENU)(INT_PTR)ID_GRP_SYSTEM, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                        22, 606, 380, 22, hw, (HMENU)(INT_PTR)ID_AUTOSTART, nullptr, nullptr);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                        470, 608, 70, 20, hw, (HMENU)(INT_PTR)ID_L_LANG, nullptr, nullptr);
        g_hLangCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                        546, 604, 130, 220, hw, (HMENU)(INT_PTR)ID_LANG, nullptr, nullptr);
        SendMessageW(g_hLangCombo, CB_ADDSTRING, 0, (LPARAM)L"Русский");
        SendMessageW(g_hLangCombo, CB_ADDSTRING, 0, (LPARAM)L"English");
        SendMessageW(g_hLangCombo, CB_SETCURSEL, g_lang == Lang::EN ? 1 : 0, 0);

        SendMessageW(GetDlgItem(hw, ID_AUTOSTART), BM_SETCHECK,
                     IsAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);

        FillDeviceCombos();
        ApplyLanguage();
        ApplyUiFont(hw);
        RefreshCurrentConfig();
        UpdatePreview();
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        WORD code = HIWORD(wp);
        // Live preview: recompute would-be delays as the user edits fields.
        if (code == EN_CHANGE &&
            (id == ID_MEDIAN || id == ID_MIC || id == ID_VP_CHROM || id == ID_VP_FF)) {
            UpdatePreview();
            return 0;
        }
        if (id == ID_START) {
            if (g_running) {
                g_stopRequested = true;
                EnableWindow((HWND)lp, FALSE);
                SetWindowTextW((HWND)lp, L"Stopping...");
            } else {
                g_stopRequested = false;
                g_running = true;
                SetWindowTextW((HWND)lp, T(STR_STOP));
                auto* params = new MeasureParams;
                int rsel = (int)SendMessageW(g_hRendCombo, CB_GETCURSEL, 0, 0);
                int csel = (int)SendMessageW(g_hCapCombo, CB_GETCURSEL, 0, 0);
                params->rendId = (rsel >= 0 && rsel < (int)g_rendIds.size()) ? g_rendIds[rsel] : L"";
                params->capId = (csel >= 0 && csel < (int)g_capIds.size()) ? g_capIds[csel] : L"";
                params->capName.clear();
                if (csel >= 0) {
                    wchar_t buf[256];
                    int len = (int)SendMessageW(g_hCapCombo, CB_GETLBTEXTLEN, csel, 0);
                    if (len > 0 && len < 256) { SendMessageW(g_hCapCombo, CB_GETLBTEXT, csel, (LPARAM)buf); buf[len] = 0; params->capName = buf; }
                }
                params->loopback = (SendMessageW(GetDlgItem(hw, ID_LOOPBACK), BM_GETCHECK, 0, 0) == BST_CHECKED);
                HANDLE t = CreateThread(nullptr, 0, MeasureThread, params, 0, nullptr);
                if (t) CloseHandle(t);
            }
        } else if (id == ID_CAL) {
            if (g_calRunning) {
                g_stopRequested = true;
            } else {
                if (g_running) {
                    MessageBoxW(hw, L"Stop the main test before calibrating.", L"AV Sync Measure", MB_ICONINFORMATION);
                    break;
                }
                g_stopRequested = false;
                g_calRunning = true;
                auto* params = new MeasureParams;
                int csel = (int)SendMessageW(g_hCapCombo, CB_GETCURSEL, 0, 0);
                params->capId = (csel >= 0 && csel < (int)g_capIds.size()) ? g_capIds[csel] : L"";
                params->capName.clear();
                if (csel >= 0) {
                    wchar_t buf[256];
                    int len = (int)SendMessageW(g_hCapCombo, CB_GETLBTEXTLEN, csel, 0);
                    if (len > 0 && len < 256) { SendMessageW(g_hCapCombo, CB_GETLBTEXT, csel, (LPARAM)buf); buf[len] = 0; params->capName = buf; }
                }
                HANDLE t = CreateThread(nullptr, 0, CalibrateThread, params, 0, nullptr);
                if (t) CloseHandle(t);
            }
        } else if (id == ID_APPLY) {
            double median = ReadEditDbl(hw, ID_MEDIAN, -1.0);
            if (median < 0) {
                MessageBoxW(hw, L"No measured delay yet - run the test, then Stop it to fill the median.",
                            L"AV Sync Measure", MB_ICONINFORMATION);
                break;
            }
            double mic = ReadEditDbl(hw, ID_MIC, 0.0);
            double vpChrom = ReadEditDbl(hw, ID_VP_CHROM, 277.0);
            double vpFf = ReadEditDbl(hw, ID_VP_FF, 447.0);
            long globalMs = (long)(median - mic - vpChrom + 0.5);
            long ffMs = (long)(median - mic - vpFf + 0.5);
            if (WriteConfig(globalMs, ffMs)) {
                EnterCriticalSection(&g_cs); g_vpChromMs = vpChrom; g_vpFfMs = vpFf; LeaveCriticalSection(&g_cs);
                wchar_t t[256];
                _snwprintf_s(t, _countof(t), _TRUNCATE,
                    L"Applied: global = %ld ms (Chromium/Yandex), firefox = %ld ms.", globalMs, ffMs);
                SetWindowTextW(g_hStatus, t);
                RefreshCurrentConfig();
                UpdatePreview();
            } else {
                MessageBoxW(hw, L"Could not open/create the shared config.", L"AV Sync Measure", MB_ICONERROR);
            }
        } else if (id == ID_VP_CHROM_RST) {
            SetDlgItemInt(hw, ID_VP_CHROM, 277, FALSE);
            EnterCriticalSection(&g_cs); g_vpChromMs = 277.0; LeaveCriticalSection(&g_cs);
            UpdatePreview();
        } else if (id == ID_VP_FF_RST) {
            SetDlgItemInt(hw, ID_VP_FF, 447, FALSE);
            EnterCriticalSection(&g_cs); g_vpFfMs = 447.0; LeaveCriticalSection(&g_cs);
            UpdatePreview();
        } else if (id == ID_REFRESH) {
            RefreshCurrentConfig();
        } else if (id == ID_DISABLE) {
            if (WriteConfig(0, 0)) {
                SetWindowTextW(g_hStatus, L"Disabled: global = 0 ms, firefox = 0 ms (pass-through).");
                RefreshCurrentConfig();
            } else {
                MessageBoxW(hw, L"Could not open/create the shared config.", L"AV Sync Measure", MB_ICONERROR);
            }
        } else if (id == ID_AUTOSTART) {
            bool on = (SendMessageW(GetDlgItem(hw, ID_AUTOSTART), BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (!SetAutostart(on)) {
                MessageBoxW(hw, L"Could not update the autostart registry entry.", L"AV Sync Measure", MB_ICONERROR);
                SendMessageW(GetDlgItem(hw, ID_AUTOSTART), BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0);
            }
        } else if (id == ID_LANG) {
            if (code == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(g_hLangCombo, CB_GETCURSEL, 0, 0);
                g_lang = (sel == 1) ? Lang::EN : Lang::RU;
                ApplyLanguage();
                RefreshCurrentConfig();
            }
        }
        return 0;
    }
    case WM_APP_STATUS:
        if (!g_running) {
            HWND btn = GetDlgItem(hw, ID_START);
            EnableWindow(btn, TRUE);
            SetWindowTextW(btn, T(STR_START));
            // Fill the editable median field from the finished run's median.
            double med; bool has;
            EnterCriticalSection(&g_cs); med = g_medianMs; has = g_hasMedian; LeaveCriticalSection(&g_cs);
            if (has) {
                SetDlgItemInt(hw, ID_MEDIAN, (UINT)(med + 0.5), FALSE);
                UpdatePreview();
            }
        }
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            AddTrayIcon(hw);
            ShowWindow(hw, SW_HIDE);
            return 0;
        }
        return 0;
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) {
            RestoreFromTray(hw);
        } else if (LOWORD(lp) == WM_RBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, T(STR_RESTORE));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 2, T(STR_EXIT));
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(hw);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hw, nullptr);
            DestroyMenu(menu);
            if (cmd == 1) RestoreFromTray(hw);
            else if (cmd == 2) DestroyWindow(hw);
        }
        return 0;
    case WM_CLOSE: {
        // Ask: disable the delay and exit, or minimize to tray.
        int r = MessageBoxW(hw, T(STR_CLOSE_MSG), T(STR_CLOSE_TITLE),
                            MB_YESNO | MB_ICONQUESTION);
        if (r == IDYES) {
            WriteConfig(0, 0); // disable delay
            DestroyWindow(hw);
        } else {
            // minimize to tray
            AddTrayIcon(hw);
            ShowWindow(hw, SW_HIDE);
        }
        return 0;
    }
    case WM_DESTROY:
        g_stopRequested = true;
        g_watchStop = true;
        if (g_watchThread) { WaitForSingleObject(g_watchThread, 1500); CloseHandle(g_watchThread); g_watchThread = nullptr; }
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    InitializeCriticalSection(&g_cs);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_appIcon = MakeAppIcon();
    g_uiFont = MakeUiFont();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = CanvasProc;
    wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AVSYNC_CANVAS";
    RegisterClassW(&wc);

    WNDCLASSW wm = {};
    wm.lpfnWndProc = WndProc;
    wm.hInstance = hInst;
    wm.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wm.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wm.hIcon = g_appIcon ? g_appIcon : LoadIconW(nullptr, IDI_APPLICATION);
    wm.lpszClassName = L"AVSYNC_MEASURE";
    RegisterClassW(&wm);

    HWND hw = CreateWindowExW(0, L"AVSYNC_MEASURE", T(STR_WINTITLE),
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 840, 700,
                              nullptr, nullptr, hInst, nullptr);
    // Start minimized to the tray if launched with --minimized (autostart).
    g_startMinimized = false;
    {
        int n = 0;
        LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &n);
        for (int i = 1; i < n; ++i) {
            if (_wcsicmp(args[i], L"--minimized") == 0) g_startMinimized = true;
        }
        if (args) LocalFree(args);
    }

    // Tray icon is always present (even when the window is shown).
    AddTrayIcon(hw);
    if (g_startMinimized) {
        // keep window hidden; do not ShowWindow
    } else {
        ShowWindow(hw, SW_SHOW);
        UpdateWindow(hw);
    }

    // Start the browser watcher (embedded DLL injection).
    g_watchStop = false;
    g_watchThread = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    DeleteCriticalSection(&g_cs);
    if (g_uiFont) DeleteObject(g_uiFont);
    if (g_appIcon) DestroyIcon(g_appIcon);
    CoUninitialize();
    return 0;
}
