// avsync.exe — process watcher + manual-map injector + shared-memory config
// owner for avsync_hook.dll.
//
// Modes:
//   avsync.exe list                     list processes with command lines
//   avsync.exe inject --pid N [--dll P] one-shot manual-map injection
//   avsync.exe watch [--delay MS] [--interval MS] [--match SUBSTR]...
//                                       keep browsers' audio processes hooked
//   avsync.exe setdelay MS              change delay of a running watch session

#define INITGUID
#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <propsys.h>

// PKEY_Device_FriendlyName (defined manually to avoid mingw header variance)
static const PROPERTYKEY PKEY_Device_FriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
};
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include "../common/config.h"

// ---------------------------------------------------------------------------
// PIC loader stub: copied byte-for-byte into the target process and executed
// there via CreateRemoteThread. Constraints: no globals, no string literals,
// no CRT/library calls other than the two function pointers passed in ctx,
// no C++ constructs that emit helper calls. Kept adjacent to LoadDllStubEnd
// so (end - start) is the stub size (do NOT build with -ffunction-sections).
// ---------------------------------------------------------------------------

struct ManualInjectCtx {
    unsigned char*            ImageBase;
    IMAGE_NT_HEADERS64*       NtHeaders;
    IMAGE_BASE_RELOCATION*    BaseRelocation;
    IMAGE_IMPORT_DESCRIPTOR*  ImportDirectory;
    HMODULE (WINAPI* fnLoadLibraryA)(LPCSTR);
    FARPROC (WINAPI* fnGetProcAddress)(HMODULE, LPCSTR);
    volatile long*            PhaseOut;   // optional progress marker (diagnostics)
};

extern "C" DWORD WINAPI LoadDllStub(void* param)
{
    ManualInjectCtx* ctx = (ManualInjectCtx*)param;
    unsigned char* base = ctx->ImageBase;
    IMAGE_NT_HEADERS64* nt = ctx->NtHeaders;
    ULONGLONG delta = (ULONGLONG)base - (ULONGLONG)nt->OptionalHeader.ImageBase;
    if (ctx->PhaseOut) *ctx->PhaseOut = 1;

    // Relocations (BaseRelocation may be null when the image has no .reloc)
    IMAGE_BASE_RELOCATION* reloc = ctx->BaseRelocation;
    while (reloc && reloc->VirtualAddress) {
        if (reloc->SizeOfBlock < 8 || reloc->SizeOfBlock > 0x100000) break;
        unsigned count = (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        WORD* list = (WORD*)(reloc + 1);
        for (unsigned i = 0; i < count; ++i) {
            if (list[i]) {
                ULONGLONG* ptr = (ULONGLONG*)(base + reloc->VirtualAddress + (list[i] & 0xFFF));
                *ptr += delta;
            }
        }
        reloc = (IMAGE_BASE_RELOCATION*)((unsigned char*)reloc + reloc->SizeOfBlock);
    }
    if (ctx->PhaseOut) *ctx->PhaseOut = 2;

    // Imports
    IMAGE_IMPORT_DESCRIPTOR* imp = ctx->ImportDirectory;
    while (imp && imp->Name) {
        HMODULE mod = ctx->fnLoadLibraryA((LPCSTR)(base + imp->Name));
        if (mod) {
            ULONGLONG* firstThunk = (ULONGLONG*)(base + imp->FirstThunk);
            ULONGLONG* origThunk = imp->OriginalFirstThunk
                ? (ULONGLONG*)(base + imp->OriginalFirstThunk)
                : firstThunk;
            while (*origThunk) {
                if (*origThunk & IMAGE_ORDINAL_FLAG64) {
                    *firstThunk = (ULONGLONG)ctx->fnGetProcAddress(
                        mod, (LPCSTR)(ULONG_PTR)IMAGE_ORDINAL64(*origThunk));
                } else {
                    IMAGE_IMPORT_BY_NAME* ibn =
                        (IMAGE_IMPORT_BY_NAME*)(base + (*origThunk & 0xFFFFFFFF));
                    *firstThunk = (ULONGLONG)ctx->fnGetProcAddress(mod, (LPCSTR)ibn->Name);
                }
                ++firstThunk;
                ++origThunk;
            }
        }
        ++imp;
    }
    if (ctx->PhaseOut) *ctx->PhaseOut = 3;

    // Entry point (DllMain, built with -nostdlib: no CRT startup)
    if (nt->OptionalHeader.AddressOfEntryPoint) {
        typedef BOOL (WINAPI* DllEntry)(HMODULE, DWORD, LPVOID);
        DllEntry entry = (DllEntry)(base + nt->OptionalHeader.AddressOfEntryPoint);
        BOOL r = entry((HMODULE)base, DLL_PROCESS_ATTACH, nullptr);
        if (ctx->PhaseOut) *ctx->PhaseOut = 4;
        return r ? 0 : 1;
    }
    if (ctx->PhaseOut) *ctx->PhaseOut = 4;
    return 0;
}

extern "C" DWORD WINAPI LoadDllStubEnd(void* param) { (void)param; return 0; }

// ---------------------------------------------------------------------------
// Process helpers
// ---------------------------------------------------------------------------

struct MyUnicodeString { USHORT Length; USHORT MaximumLength; WCHAR* Buffer; };

static bool GetProcessCmdline(DWORD pid, std::wstring& out)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return false;
    bool ok = false;
    PROCESS_BASIC_INFORMATION pbi;
    memset(&pbi, 0, sizeof(pbi));
    ULONG retLen = 0;
    NTSTATUS st = NtQueryInformationProcess(h, ProcessBasicInformation, &pbi, sizeof(pbi), &retLen);
    if (st >= 0 && pbi.PebBaseAddress) {
        void* params = nullptr;
        // PEB->ProcessParameters at +0x20 (x64)
        if (ReadProcessMemory(h, (const char*)pbi.PebBaseAddress + 0x20, &params, sizeof(params), nullptr)) {
            MyUnicodeString us;
            memset(&us, 0, sizeof(us));
            // RTL_USER_PROCESS_PARAMETERS->CommandLine at +0x70 (x64)
            if (ReadProcessMemory(h, (const char*)params + 0x70, &us, sizeof(us), nullptr)
                && us.Buffer && us.Length >= 2) {
                std::wstring buf(us.Length / 2, L'\0');
                if (ReadProcessMemory(h, us.Buffer, &buf[0], us.Length, nullptr)) {
                    out = buf;
                    ok = true;
                }
            }
        }
    }
    CloseHandle(h);
    return ok;
}

struct ProcInfo { DWORD pid; std::wstring name; std::wstring cmdline; };

static std::vector<ProcInfo> ListProcesses()
{
    std::vector<ProcInfo> result;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcInfo pi;
            pi.pid = pe.th32ProcessID;
            pi.name = pe.szExeFile;
            GetProcessCmdline(pi.pid, pi.cmdline);
            result.push_back(pi);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return result;
}

static std::wstring ToLower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

// Chromium family: chrome.exe / msedge.exe / browser.exe (Yandex) audio service
static const wchar_t* CHROMIUM_AUDIO_PATTERN = L"--utility-sub-type=audio.mojom.audioservice";
// Firefox on Windows renders cubeb/WASAPI output in the MAIN process
// (verified via IAudioSessionManager2: the active audio session's process is
// firefox.exe with an empty command line). Legacy AudioIPC fallbacks kept for
// older builds.
static const wchar_t* FIREFOX_AUDIO_PATTERNS[] = { L"-audioserver", L"--audioipc" };

static bool IsAudioTarget(const ProcInfo& pi, const std::vector<std::wstring>& extra)
{
    std::wstring cl = ToLower(pi.cmdline);
    if (cl.empty()) return false;
    if (cl.find(CHROMIUM_AUDIO_PATTERN) != std::wstring::npos) return true;
    std::wstring nl = ToLower(pi.name);
    if (nl == L"firefox.exe") {
        // Audio output lives in the main process (no -contentproc flag).
        if (cl.find(L"-contentproc") == std::wstring::npos) return true;
        for (auto p : FIREFOX_AUDIO_PATTERNS)
            if (cl.find(p) != std::wstring::npos) return true;
    }
    for (auto& e : extra)
        if (!e.empty() && cl.find(ToLower(e)) != std::wstring::npos) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Manual-map injection
// ---------------------------------------------------------------------------

static bool ReadFileBytes(const wchar_t* path, std::vector<unsigned char>& out)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 64 * 1024 * 1024;
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD read = 0;
        ok = ReadFile(f, out.data(), (DWORD)out.size(), &read, nullptr) && read == out.size();
    }
    CloseHandle(f);
    return ok;
}

static bool InjectDll(DWORD pid, const std::vector<unsigned char>& dll)
{
    if (dll.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    const unsigned char* data = dll.data();
    auto dos = (const IMAGE_DOS_HEADER*)data;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (const IMAGE_NT_HEADERS64*)(data + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        !(nt->FileHeader.Characteristics & IMAGE_FILE_DLL)) {
        wprintf(L"[%lu] not a x64 DLL image\n", pid);
        return false;
    }

    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc) { wprintf(L"[%lu] OpenProcess failed (err %lu)\n", pid, GetLastError()); return false; }

    bool ok = false;
    void* image = VirtualAllocEx(hProc, nullptr, nt->OptionalHeader.SizeOfImage,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    void* stubMem = nullptr;
    if (!image) { wprintf(L"[%lu] VirtualAllocEx image failed (err %lu)\n", pid, GetLastError()); goto cleanup; }

    {
        // headers
        SIZE_T written = 0;
        if (!WriteProcessMemory(hProc, image, data, nt->OptionalHeader.SizeOfHeaders, &written)) goto cleanup;
        // sections
        auto sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (!sec->SizeOfRawData) continue;
            if (sec->PointerToRawData + sec->SizeOfRawData > dll.size()) goto cleanup;
            if (!WriteProcessMemory(hProc, (unsigned char*)image + sec->VirtualAddress,
                                    data + sec->PointerToRawData, sec->SizeOfRawData, &written))
                goto cleanup;
        }
        // section protections (no RWX anywhere: write first, then protect)
        sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            DWORD prot = PAGE_READONLY;
            if (sec->Characteristics & IMAGE_SCN_CNT_CODE) prot = PAGE_EXECUTE_READ;
            else if (sec->Characteristics & IMAGE_SCN_MEM_WRITE) prot = PAGE_READWRITE;
            DWORD old;
            VirtualProtectEx(hProc, (unsigned char*)image + sec->VirtualAddress,
                             sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData,
                             prot, &old);
        }
        // The IAT often lives in a read-only section (.rdata); the OS loader
        // makes it writable while resolving imports - we must too.
        {
            DWORD iatRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress;
            if (iatRva) {
                sec = IMAGE_FIRST_SECTION(nt);
                for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
                    DWORD end = sec->VirtualAddress + (sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData);
                    if (iatRva >= sec->VirtualAddress && iatRva < end) {
                        DWORD old;
                        VirtualProtectEx(hProc, (unsigned char*)image + sec->VirtualAddress,
                                         sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData,
                                         PAGE_READWRITE, &old);
                        break;
                    }
                }
            }
        }

        // stub + context: separate pages! VirtualProtect works page-granular,
        // so the RX stub page must not share a page with the writable ctx/phase.
        SIZE_T stubSize = (SIZE_T)((uintptr_t)&LoadDllStubEnd - (uintptr_t)&LoadDllStub);
        if (stubSize == 0 || stubSize > 3800) { wprintf(L"stub size suspicious: %zu\n", stubSize); goto cleanup; }
        stubMem = VirtualAllocEx(hProc, nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!stubMem) goto cleanup;

        ManualInjectCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ImageBase        = (unsigned char*)image;
        ctx.NtHeaders        = (IMAGE_NT_HEADERS64*)((unsigned char*)image + dos->e_lfanew);
        ctx.BaseRelocation   = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress
            ? (IMAGE_BASE_RELOCATION*)((unsigned char*)image +
                nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress)
            : nullptr;
        ctx.ImportDirectory  = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress
            ? (IMAGE_IMPORT_DESCRIPTOR*)((unsigned char*)image +
                nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress)
            : nullptr;
        ctx.fnLoadLibraryA   = LoadLibraryA;   // kernel32 base is identical across processes (per boot)
        ctx.fnGetProcAddress = GetProcAddress;
        long* remotePhase = (long*)((unsigned char*)stubMem + 4096 + sizeof(ManualInjectCtx));
        ctx.PhaseOut = remotePhase;

        SIZE_T written2 = 0;
        unsigned char* stubParams = (unsigned char*)stubMem + 4096; // ctx on the second (RW) page
        if (!WriteProcessMemory(hProc, stubMem, (void*)&LoadDllStub, stubSize, &written2)) { wprintf(L"[%lu] write stub failed (err %lu)\n", pid, GetLastError()); goto cleanup; }
        if (!WriteProcessMemory(hProc, stubParams, &ctx, sizeof(ctx), &written2)) { wprintf(L"[%lu] write ctx failed (err %lu)\n", pid, GetLastError()); goto cleanup; }
        DWORD old;
        if (!VirtualProtectEx(hProc, stubMem, 4096, PAGE_EXECUTE_READ, &old)) { wprintf(L"[%lu] protect stub failed (err %lu)\n", pid, GetLastError()); goto cleanup; }

        HANDLE ht = CreateRemoteThread(hProc, nullptr, 0,
                                       (LPTHREAD_START_ROUTINE)stubMem, stubParams, 0, nullptr);
        if (!ht) { wprintf(L"[%lu] CreateRemoteThread failed (err %lu)\n", pid, GetLastError()); goto cleanup; }
        WaitForSingleObject(ht, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeThread(ht, &exitCode);
        CloseHandle(ht);
        long phaseVal = -1; SIZE_T rd = 0;
        ReadProcessMemory(hProc, remotePhase, &phaseVal, sizeof(phaseVal), &rd);
        if (exitCode != 0) { wprintf(L"[%lu] remote loader exit code %lu (phase %ld)\n", pid, exitCode, rd ? phaseVal : -9); goto cleanup; }

        VirtualFreeEx(hProc, stubMem, 0, MEM_RELEASE);
        stubMem = nullptr;
        ok = true;
        wprintf(L"[%lu] injected OK\n", pid);
    }

cleanup:
    if (stubMem) VirtualFreeEx(hProc, stubMem, 0, MEM_RELEASE);
    // note: on failure the image allocation is intentionally left (hard to
    // know how far initialization got); watch mode will not retry same pid
    CloseHandle(hProc);
    return ok;
}

// ---------------------------------------------------------------------------
// Diagnostics: in-process self-map with SEH attribution, and a sleep target
// ---------------------------------------------------------------------------

static DWORD g_excCode = 0;
static void* g_excAddr = nullptr;
static unsigned char* g_dbgImage = nullptr;
static unsigned long g_dbgImageSize = 0;
static unsigned char* g_dbgStub = nullptr;

// Vectored handler: logs and attributes the exception before the process
// dies (SEH __try is unavailable on clang windows-gnu).
static LONG WINAPI VehHandler(PEXCEPTION_POINTERS ep)
{
    g_excCode = ep->ExceptionRecord->ExceptionCode;
    g_excAddr = ep->ExceptionRecord->ExceptionAddress;
    wprintf(L"EXCEPTION %08lx at %p\n", g_excCode, g_excAddr);
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)g_excAddr, &m)) {
        wchar_t name[512];
        GetModuleFileNameW(m, name, 512);
        wprintf(L"  in module: %ls (+0x%llx)\n", name,
                (unsigned long long)((unsigned char*)g_excAddr - (unsigned char*)m));
    } else if (g_dbgImage && (uintptr_t)g_excAddr >= (uintptr_t)g_dbgImage &&
               (uintptr_t)g_excAddr < (uintptr_t)g_dbgImage + g_dbgImageSize) {
        wprintf(L"  in mapped image (+0x%llx)\n",
                (unsigned long long)((unsigned char*)g_excAddr - g_dbgImage));
    } else if (g_dbgStub && (uintptr_t)g_excAddr >= (uintptr_t)g_dbgStub &&
               (uintptr_t)g_excAddr < (uintptr_t)g_dbgStub + 8192) {
        wprintf(L"  in stub (+0x%llx)\n", (unsigned long long)((unsigned char*)g_excAddr - g_dbgStub));
    } else {
        wprintf(L"  unknown location\n");
    }
    fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD RunStubDirect(void* stubFn, ManualInjectCtx* ctx)
{
    return ((DWORD (WINAPI*)(void*))stubFn)(ctx);
}

static int CmdSelfTest(const std::wstring& dllPath)
{
    std::vector<unsigned char> dll;
    if (!ReadFileBytes(dllPath.c_str(), dll)) { wprintf(L"cannot read %ls\n", dllPath.c_str()); return 2; }
    auto dos = (const IMAGE_DOS_HEADER*)dll.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { wprintf(L"bad dos magic\n"); return 2; }
    auto nt = (const IMAGE_NT_HEADERS64*)(dll.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { wprintf(L"bad nt signature\n"); return 2; }

    unsigned char* image = (unsigned char*)VirtualAlloc(nullptr, nt->OptionalHeader.SizeOfImage,
                                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!image) { wprintf(L"alloc image failed\n"); return 3; }
    memcpy(image, dll.data(), nt->OptionalHeader.SizeOfHeaders);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!sec->SizeOfRawData) continue;
        memcpy(image + sec->VirtualAddress, dll.data() + sec->PointerToRawData, sec->SizeOfRawData);
    }
    // Apply section protections (DEP: RW pages are not executable)
    sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        DWORD prot = PAGE_READONLY;
        if (sec->Characteristics & IMAGE_SCN_CNT_CODE) prot = PAGE_EXECUTE_READ;
        else if (sec->Characteristics & IMAGE_SCN_MEM_WRITE) prot = PAGE_READWRITE;
        SIZE_T region = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        if (!region) continue;
        DWORD old;
        VirtualProtect(image + sec->VirtualAddress, region, prot, &old);
    }
    // IAT section must be writable while the stub resolves imports
    {
        DWORD iatRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress;
        if (iatRva) {
            sec = IMAGE_FIRST_SECTION(nt);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
                DWORD end = sec->VirtualAddress + (sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData);
                if (iatRva >= sec->VirtualAddress && iatRva < end) {
                    SIZE_T region = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
                    DWORD old;
                    VirtualProtect(image + sec->VirtualAddress, region, PAGE_READWRITE, &old);
                    break;
                }
            }
        }
    }

    unsigned char* stubMem = (unsigned char*)VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE,
                                                          PAGE_EXECUTE_READWRITE);
    if (!stubMem) { wprintf(L"alloc stub failed\n"); return 3; }
    SIZE_T stubSize = (SIZE_T)((uintptr_t)&LoadDllStubEnd - (uintptr_t)&LoadDllStub);
    memcpy(stubMem, (const void*)&LoadDllStub, stubSize);

    static long phase = 0;
    ManualInjectCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ImageBase        = image;
    ctx.NtHeaders        = (IMAGE_NT_HEADERS64*)(image + dos->e_lfanew);
    ctx.BaseRelocation   = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress
        ? (IMAGE_BASE_RELOCATION*)(image + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress)
        : nullptr;
    ctx.ImportDirectory  = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress
        ? (IMAGE_IMPORT_DESCRIPTOR*)(image + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress)
        : nullptr;
    ctx.fnLoadLibraryA   = LoadLibraryA;
    ctx.fnGetProcAddress = GetProcAddress;
    ctx.PhaseOut         = &phase;

    g_excCode = 0; g_excAddr = nullptr;
    g_dbgImage = image;
    g_dbgImageSize = nt->OptionalHeader.SizeOfImage;
    g_dbgStub = stubMem;
    AddVectoredExceptionHandler(1, VehHandler);
    DWORD r = RunStubDirect(stubMem, &ctx);
    wprintf(L"stub ret %lu, phase %ld\n", r, phase);
    if (g_excCode) {
        wprintf(L"selftest FAILED (exception details above)\n");
        return 4;
    }
    wprintf(L"selftest OK: loader path works in-process\n");
    return 0;
}

static LONG CrashLogFilter(EXCEPTION_POINTERS* ep)
{
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE m = nullptr;
    wchar_t name[512] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &m))
        GetModuleFileNameW(m, name, 512);
    FILE* f = _wfopen(L"C:\\Users\\Public\\avsync_crash.log", L"a");
    if (f) {
        fwprintf(f, L"pid %lu EXC %08lx addr %p module %ls\n",
                 GetCurrentProcessId(), ep->ExceptionRecord->ExceptionCode, addr, name);
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

static int CmdSleep(int secs)
{
    SetUnhandledExceptionFilter(CrashLogFilter);
    wprintf(L"sleeping pid %lu for %d s\n", GetCurrentProcessId(), secs);
    Sleep((DWORD)secs * 1000);
    return 0;
}

// ---------------------------------------------------------------------------
// Shared-memory config
// ---------------------------------------------------------------------------

static HANDLE  g_shm = nullptr;
static AVSYNC_CONFIG* g_shmView = nullptr;

static bool CreateConfig(long delayMs)
{
    g_shm = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                               0, sizeof(AVSYNC_CONFIG), AVSYNC_SHM_NAME);
    if (!g_shm) return false;
    g_shmView = (AVSYNC_CONFIG*)MapViewOfFile(g_shm, FILE_MAP_WRITE, 0, 0, sizeof(AVSYNC_CONFIG));
    if (!g_shmView) { CloseHandle(g_shm); g_shm = nullptr; return false; }
    memset(g_shmView, 0, sizeof(*g_shmView));
    g_shmView->magic = AVSYNC_MAGIC;
    g_shmView->version = AVSYNC_VERSION;
    g_shmView->enabled = 1;
    g_shmView->globalDelayMs = delayMs;
    return true;
}

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

static void EnableDebugPrivilege()
{
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    tp.Privileges[0].Luid.LowPart = 20; // SE_DEBUG_NAME
    tp.Privileges[0].Luid.HighPart = 0;
    AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
    CloseHandle(token);
}

static std::wstring DefaultDllPath()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    size_t pos = path.find_last_of(L"\\/");
    path = (pos == std::wstring::npos) ? L"" : path.substr(0, pos + 1);
    return path + L"avsync_hook.dll";
}

static int CmdList()
{
    auto procs = ListProcesses();
    for (auto& p : procs) {
        if (p.cmdline.empty()) continue;
        std::wstring cl = p.cmdline;
        if (cl.length() > 220) cl = cl.substr(0, 220) + L"...";
        wprintf(L"%-8lu %-28ls %ls\n", p.pid, p.name.c_str(), cl.c_str());
    }
    return 0;
}

static int CmdDevices()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { wprintf(L"CoInitializeEx failed %08lx\n", hr); return 2; }
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&en))) { wprintf(L"enumerator failed\n"); CoUninitialize(); return 2; }

    auto stateName = [](DWORD st) -> const wchar_t* {
        switch (st) {
        case DEVICE_STATE_ACTIVE: return L"active";
        case DEVICE_STATE_DISABLED: return L"disabled";
        case DEVICE_STATE_UNPLUGGED: return L"unplugged";
        case DEVICE_STATE_NOTPRESENT: return L"notpresent";
        default: return L"?";
        }
    };
    auto friendlyName = [](IMMDevice* d, wchar_t* buf, int cap) {
        buf[0] = 0;
        IPropertyStore* ps = nullptr;
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
                wcsncpy(buf, v.pwszVal, cap - 1);
            PropVariantClear(&v);
            ps->Release();
        }
        buf[cap - 1] = 0;
    };

    for (int pass = 0; pass < 2; ++pass) {
        EDataFlow flow = pass == 0 ? eRender : eCapture;
        IMMDeviceCollection* coll = nullptr;
        if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED | DEVICE_STATE_NOTPRESENT | DEVICE_STATE_UNPLUGGED, &coll))) continue;
        UINT n = 0;
        coll->GetCount(&n);
        wprintf(L"%s endpoints (%u):\n", pass == 0 ? L"render" : L"capture", n);
        IMMDevice* def = nullptr;
        en->GetDefaultAudioEndpoint(flow, flow == eRender ? eConsole : eMultimedia, &def);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(coll->Item(i, &d))) continue;
            DWORD st = 0;
            d->GetState(&st);
            wchar_t name[256];
            friendlyName(d, name, 256);
            wprintf(L"  [%-10ls] %ls%s\n", stateName(st), name, (def && d == def) ? L"   <default>" : L"");
            d->Release();
        }
        if (def) def->Release();
        coll->Release();
    }
    en->Release();
    CoUninitialize();
    return 0;
}

// Enumerate active audio sessions on the default render endpoint: shows which
// process actually holds the audio endpoint (i.e. where cubeb/WASAPI output
// lives), which is what we need to inject into.
static const wchar_t* SessionStateName(AudioSessionState s)
{
    switch (s) {
    case AudioSessionStateActive:  return L"active";
    case AudioSessionStateExpired: return L"expired";
    default: return L"inactive";
    }
}

static int CmdSessions()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { wprintf(L"CoInitializeEx failed %08lx\n", hr); return 2; }
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&en))) { wprintf(L"enumerator failed\n"); CoUninitialize(); return 2; }
    IMMDevice* dev = nullptr;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) { wprintf(L"no render endpoint\n"); en->Release(); CoUninitialize(); return 2; }
    IAudioSessionManager2* mgr = nullptr;
    if (FAILED(dev->Activate(IID_IAudioSessionManager2, CLSCTX_ALL, nullptr, (void**)&mgr))) { wprintf(L"session manager failed\n"); dev->Release(); en->Release(); CoUninitialize(); return 2; }
    IAudioSessionEnumerator* sess = nullptr;
    if (FAILED(mgr->GetSessionEnumerator(&sess))) { wprintf(L"enumerator failed\n"); mgr->Release(); dev->Release(); en->Release(); CoUninitialize(); return 2; }
    int count = 0;
    sess->GetCount(&count);
    wprintf(L"%d audio sessions on default render endpoint:\n", count);
    for (int i = 0; i < count; ++i) {
        IAudioSessionControl* c = nullptr;
        if (FAILED(sess->GetSession(i, &c))) continue;
        IAudioSessionControl2* c2 = nullptr;
        DWORD pid = 0;
        AudioSessionState st = AudioSessionStateInactive;
        if (SUCCEEDED(c->QueryInterface(IID_IAudioSessionControl2, (void**)&c2))) {
            c2->GetProcessId(&pid);
            c2->GetState(&st);
        }
        wchar_t name[MAX_PATH] = L"?";
        if (pid) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (h) {
                DWORD sz = MAX_PATH;
                if (!QueryFullProcessImageNameW(h, 0, name, &sz)) wcscpy_s(name, L"<unknown>");
                CloseHandle(h);
            }
        }
        wprintf(L"  pid %-6lu  %-8ls  %ls\n", pid, SessionStateName(st), name);
        if (c2) c2->Release();
        c->Release();
    }
    sess->Release(); mgr->Release(); dev->Release(); en->Release();
    CoUninitialize();
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    // Unbuffered stdout: watch mode is often run with redirected output
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        wprintf(L"Usage:\n"
                L"  avsync.exe list\n"
                L"  avsync.exe inject --pid N [--dll PATH]\n"
                L"  avsync.exe watch [--delay MS] [--interval MS] [--match SUBSTR]...\n"
                L"  avsync.exe setdelay MS\n"
                L"  avsync.exe selftest [--dll PATH]\n"
                L"  avsync.exe sleep [SECONDS]\n"
                L"  avsync.exe sessions\n"
                L"  avsync.exe devices\n");
        return 1;
    }
    std::wstring mode = argv[1];
    EnableDebugPrivilege();

    if (mode == L"list") return CmdList();

    if (mode == L"sessions") return CmdSessions();

    if (mode == L"devices") return CmdDevices();

    if (mode == L"getdelay") {
        HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, AVSYNC_SHM_NAME);
        if (!h) { wprintf(L"config not found (is watch running?)\n"); return 2; }
        auto view = (AVSYNC_CONFIG*)MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(AVSYNC_CONFIG));
        if (!view || view->magic != AVSYNC_MAGIC) { wprintf(L"bad config\n"); if (view) UnmapViewOfFile(view); CloseHandle(h); return 2; }
        wprintf(L"globalDelayMs = %ld (enabled=%u, version=%u)\n", view->globalDelayMs, view->enabled, view->version);
        for (unsigned i = 0; i < view->browserCount && i < AVSYNC_MAX_BROWSERS; ++i)
            wprintf(L"  %-20ls = %ld ms\n", view->browsers[i].exe, view->browsers[i].delayMs);
        UnmapViewOfFile(view);
        CloseHandle(h);
        return 0;
    }

    if (mode == L"selftest") {
        std::wstring dllPath = DefaultDllPath();
        for (int i = 2; i < argc; ++i) {
            std::wstring a = argv[i];
            if (a == L"--dll" && i + 1 < argc) dllPath = argv[++i];
        }
        return CmdSelfTest(dllPath);
    }

    if (mode == L"sleep") {
        int secs = argc > 2 ? _wtoi(argv[2]) : 30;
        return CmdSleep(secs);
    }

    if (mode == L"setdelay") {
        if (argc < 3) { wprintf(L"setdelay MS [--exe NAME]\n"); return 1; }
        long ms = _wtol(argv[2]);
        std::wstring exe;
        for (int i = 3; i < argc; ++i) {
            std::wstring a = argv[i];
            if (a == L"--exe" && i + 1 < argc) exe = argv[++i];
        }
        HANDLE h = OpenFileMappingW(FILE_MAP_WRITE, FALSE, AVSYNC_SHM_NAME);
        if (!h) { wprintf(L"config not found - is 'avsync.exe watch' running?\n"); return 2; }
        auto view = (AVSYNC_CONFIG*)MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, sizeof(AVSYNC_CONFIG));
        if (!view || view->magic != AVSYNC_MAGIC) { if (view) UnmapViewOfFile(view); CloseHandle(h); wprintf(L"bad config\n"); return 2; }
        if (!exe.empty()) {
            std::transform(exe.begin(), exe.end(), exe.begin(), ::towlower);
            unsigned idx = view->browserCount;
            for (unsigned i = 0; i < view->browserCount; ++i) {
                if (_wcsicmp(view->browsers[i].exe, exe.c_str()) == 0) { idx = i; break; }
            }
            if (idx >= AVSYNC_MAX_BROWSERS) { UnmapViewOfFile(view); CloseHandle(h); wprintf(L"too many browser overrides\n"); return 2; }
            wcsncpy(view->browsers[idx].exe, exe.c_str(), AVSYNC_EXE_LEN - 1);
            view->browsers[idx].exe[AVSYNC_EXE_LEN - 1] = 0;
            InterlockedExchange((volatile LONG*)&view->browsers[idx].delayMs, ms);
            if (idx == view->browserCount)
                InterlockedExchange((volatile LONG*)&view->browserCount, idx + 1);
            wprintf(L"%ls delay = %ld ms\n", exe.c_str(), ms);
        } else {
            InterlockedExchange((volatile LONG*)&view->globalDelayMs, ms);
            wprintf(L"global delay = %ld ms\n", ms);
        }
        UnmapViewOfFile(view);
        CloseHandle(h);
        return 0;
    }

    if (mode == L"inject") {
        DWORD pid = 0;
        std::wstring dllPath = DefaultDllPath();
        for (int i = 2; i < argc; ++i) {
            std::wstring a = argv[i];
            if (a == L"--pid" && i + 1 < argc) pid = (DWORD)_wtol(argv[++i]);
            else if (a == L"--dll" && i + 1 < argc) dllPath = argv[++i];
        }
        if (!pid) { wprintf(L"--pid required\n"); return 1; }
        std::vector<unsigned char> dll;
        if (!ReadFileBytes(dllPath.c_str(), dll)) { wprintf(L"cannot read %ls\n", dllPath.c_str()); return 2; }
        return InjectDll(pid, dll) ? 0 : 3;
    }

    if (mode == L"watch") {
        long delayMs = 200;
        DWORD intervalMs = 500;
        std::wstring dllPath = DefaultDllPath();
        std::vector<std::wstring> extra;
        for (int i = 2; i < argc; ++i) {
            std::wstring a = argv[i];
            if (a == L"--delay" && i + 1 < argc) delayMs = _wtol(argv[++i]);
            else if (a == L"--interval" && i + 1 < argc) intervalMs = (DWORD)_wtol(argv[++i]);
            else if (a == L"--dll" && i + 1 < argc) dllPath = argv[++i];
            else if (a == L"--match" && i + 1 < argc) extra.push_back(argv[++i]);
        }
        if (!CreateConfig(delayMs)) { wprintf(L"cannot create config shm (err %lu)\n", GetLastError()); return 2; }
        wprintf(L"watching, global delay %ld ms, shm ready\n", delayMs);
        wprintf(L"press Ctrl+C to stop\n");

        std::vector<unsigned char> dll;
        if (!ReadFileBytes(dllPath.c_str(), dll)) { wprintf(L"cannot read %ls\n", dllPath.c_str()); return 2; }

        std::set<DWORD> injected;
        for (;;) {
            auto procs = ListProcesses();
            std::set<DWORD> alive;
            for (auto& p : procs) {
                if (!IsAudioTarget(p, extra)) continue;
                alive.insert(p.pid);
                if (injected.count(p.pid)) continue;
                wprintf(L"target pid %lu (%ls)\n", p.pid, p.name.c_str());
                if (InjectDll(p.pid, dll)) injected.insert(p.pid);
                else injected.insert(p.pid); // don't hammer failing targets
            }
            // forget pids that disappeared
            for (auto it = injected.begin(); it != injected.end(); )
                it = alive.count(*it) ? std::next(it) : injected.erase(it);
            Sleep(intervalMs);
        }
    }

    wprintf(L"unknown mode\n");
    return 1;
}
