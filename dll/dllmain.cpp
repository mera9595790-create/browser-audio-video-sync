// avsync_hook.dll — injected into browser audio processes (manual mapping).
//
// Patches the process-wide IAudioClock vtable so that GetPosition() reports a
// position lagging by the configured delay. The browser's own A/V sync then
// delays the video to match, compensating Bluetooth/Wireless audio latency.
//
// Constraints:
// - Chromium child processes only allow LoadLibrary of Microsoft-signed
//   modules: we import kernel32/ole32/ucrtbase only (all MS-signed). Our own
//   module is loaded via manual mapping, never LoadLibrary.
// - COM must not be initialized under the loader lock: DllMain only spawns a
//   thread; the thread waits for DllMain to return before doing COM work.

// COM GUID instantiation for this translation unit (mingw-w64: without
// INITGUID the CLSID_*/IID_* are extern refs that libuuid may not provide)
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include "../common/config.h"

// Built with -nostdlib: the entry point is DllMain itself (no CRT startup),
// so the only imports are kernel32 and ole32 — both Microsoft-signed and
// always loadable, including inside Chromium sandboxed processes.

static volatile LONG        g_attached = 0;
static AVSYNC_CONFIG*       g_config   = nullptr;   // mapped view (read-only)
static IMMDeviceEnumerator* g_enumerator = nullptr; // kept alive
static IMMDevice*           g_device   = nullptr;
static IAudioClient*        g_client   = nullptr;
static IAudioClock*         g_clock    = nullptr;

// IAudioClock vtable: [0]QueryInterface [1]AddRef [2]Release
//                     [3]GetFrequency   [4]GetPosition
typedef HRESULT (WINAPI* FnGetPosition)(IAudioClock*, UINT64*, UINT64*);
static FnGetPosition g_origGetPosition = nullptr;

static wchar_t g_exeName[AVSYNC_EXE_LEN] = { 0 };

// --- per-browser delay resolution (nostdlib: no CRT string helpers) ---
static wchar_t LowerW(wchar_t c)
{
    if (c >= L'A' && c <= L'Z') return c + (L'a' - L'A');
    return c;
}

static int StrEqI(const wchar_t* a, const wchar_t* b)
{
    for (;; ++a, ++b) {
        wchar_t ca = LowerW(*a);
        wchar_t cb = LowerW(*b);
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}

static void ResolveExeName()
{
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return;
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    int i = 0;
    for (; base[i] && i < AVSYNC_EXE_LEN - 1; ++i) g_exeName[i] = LowerW(base[i]);
    g_exeName[i] = 0;
}

static long ResolveDelayMs()
{
    AVSYNC_CONFIG* c = g_config;
    if (!c || c->magic != AVSYNC_MAGIC || !c->enabled) return 0;
    if (g_exeName[0] && c->version >= 2 && c->browserCount) {
        unsigned n = c->browserCount < AVSYNC_MAX_BROWSERS ? c->browserCount : AVSYNC_MAX_BROWSERS;
        for (unsigned i = 0; i < n; ++i)
            if (StrEqI(g_exeName, c->browsers[i].exe)) return c->browsers[i].delayMs;
    }
    return c->globalDelayMs;
}

static HRESULT WINAPI Hook_GetPosition(IAudioClock* This, UINT64* pu64Position,
                                       UINT64* pu64QPCPosition)
{
    HRESULT hr = g_origGetPosition(This, pu64Position, pu64QPCPosition);
    if (SUCCEEDED(hr) && pu64Position) {
        long delayMs = ResolveDelayMs();
        if (delayMs > 0) {
            UINT64 delayHns = (UINT64)delayMs * 10000ull;
            *pu64Position = (*pu64Position > delayHns) ? (*pu64Position - delayHns) : 0;
        }
    }
    return hr;
}

static void TryMapConfig()
{
    if (g_config) return;
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, AVSYNC_SHM_NAME);
    if (!h) return; // owner (injector/configurator) not running yet
    void* view = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(AVSYNC_CONFIG));
    if (view && ((AVSYNC_CONFIG*)view)->magic == AVSYNC_MAGIC) {
        g_config = (AVSYNC_CONFIG*)view;
    } else {
        if (view) UnmapViewOfFile(view);
        CloseHandle(h);
    }
}

static DWORD WINAPI InitThread(LPVOID)
{
    // Wait until DllMain returned (COM under loader lock risks deadlock)
    while (!InterlockedCompareExchange(&g_attached, 1, 1)) Sleep(25);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return 1;

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                          IID_IMMDeviceEnumerator, (void**)&g_enumerator);
    if (FAILED(hr)) return 2;

    hr = g_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g_device);
    if (FAILED(hr)) return 3;

    hr = g_device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&g_client);
    if (FAILED(hr)) return 4;

    WAVEFORMATEX* mix = nullptr;
    hr = g_client->GetMixFormat(&mix);
    if (FAILED(hr)) return 5;

    hr = g_client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000 /*100 ms*/,
                              0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) return 6;

    hr = g_client->GetService(IID_IAudioClock, (void**)&g_clock);
    if (FAILED(hr)) return 7;

    void** vtable = *(void***)g_clock;

    // Idempotency guard: GetPosition normally lives inside a loaded system
    // module (mmdevapi/combase). If its address does not belong to any
    // module, another manually-mapped copy already hooked this process.
    HMODULE ownerMod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)vtable[4], &ownerMod)) {
        return 0;
    }

    // Patch the shared vtable — affects every IAudioClock instance in this
    // process, including ones the browser created before we arrived.
    DWORD oldProt = 0;
    if (!VirtualProtect(vtable, sizeof(void*) * 6, PAGE_READWRITE, &oldProt))
        return 8;
    g_origGetPosition = (FnGetPosition)vtable[4];
    vtable[4] = (void*)&Hook_GetPosition;
    DWORD tmp;
    VirtualProtect(vtable, sizeof(void*) * 6, oldProt, &tmp);

    // Keep trying to attach the config (owner may start later)
    for (;;) {
        TryMapConfig();
        if (g_config) break;
        Sleep(2000);
    }
    return 0;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        ResolveExeName();
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
        InterlockedExchange(&g_attached, 1);
    }
    return TRUE;
}
