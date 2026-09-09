// manualmap.h — shared manual-map injector + process enumeration for the
// hook DLL. Included by avsync_measure.exe (GUI) so a single exe can both
// watch/inject the embedded DLL and configure delays. Logging is silent here;
// callers get bool results and log themselves.
//
// The PIC loader stub (LoadDllStub..LoadDllStubEnd) must stay byte-for-byte
// adjacent in the final image — keep them in this one header, in order, and do
// NOT compile the including TU with -ffunction-sections.
#pragma once

#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include "config.h"

struct ManualInjectCtx {
    unsigned char*            ImageBase;
    IMAGE_NT_HEADERS64*       NtHeaders;
    IMAGE_BASE_RELOCATION*    BaseRelocation;
    IMAGE_IMPORT_DESCRIPTOR*  ImportDirectory;
    HMODULE (WINAPI* fnLoadLibraryA)(LPCSTR);
    FARPROC (WINAPI* fnGetProcAddress)(HMODULE, LPCSTR);
    volatile long*            PhaseOut;
};

extern "C" DWORD WINAPI LoadDllStub(void* param)
{
    ManualInjectCtx* ctx = (ManualInjectCtx*)param;
    unsigned char* base = ctx->ImageBase;
    IMAGE_NT_HEADERS64* nt = ctx->NtHeaders;
    ULONGLONG delta = (ULONGLONG)base - (ULONGLONG)nt->OptionalHeader.ImageBase;
    if (ctx->PhaseOut) *ctx->PhaseOut = 1;

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

    IMAGE_IMPORT_DESCRIPTOR* imp = ctx->ImportDirectory;
    while (imp && imp->Name) {
        HMODULE mod = ctx->fnLoadLibraryA((LPCSTR)(base + imp->Name));
        if (mod) {
            ULONGLONG* firstThunk = (ULONGLONG*)(base + imp->FirstThunk);
            ULONGLONG* origThunk = imp->OriginalFirstThunk
                ? (ULONGLONG*)(base + imp->OriginalFirstThunk) : firstThunk;
            while (*origThunk) {
                if (*origThunk & IMAGE_ORDINAL_FLAG64) {
                    *firstThunk = (ULONGLONG)ctx->fnGetProcAddress(mod, (LPCSTR)(ULONG_PTR)IMAGE_ORDINAL64(*origThunk));
                } else {
                    IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + (*origThunk & 0xFFFFFFFF));
                    *firstThunk = (ULONGLONG)ctx->fnGetProcAddress(mod, (LPCSTR)ibn->Name);
                }
                ++firstThunk; ++origThunk;
            }
        }
        ++imp;
    }
    if (ctx->PhaseOut) *ctx->PhaseOut = 3;

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
// Process enumeration + audio-target detection
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
        if (ReadProcessMemory(h, (const char*)pbi.PebBaseAddress + 0x20, &params, sizeof(params), nullptr)) {
            MyUnicodeString us;
            memset(&us, 0, sizeof(us));
            if (ReadProcessMemory(h, (const char*)params + 0x70, &us, sizeof(us), nullptr)
                && us.Buffer && us.Length >= 2) {
                std::wstring buf(us.Length / 2, L'\0');
                if (ReadProcessMemory(h, us.Buffer, &buf[0], us.Length, nullptr)) {
                    out = buf; ok = true;
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

static std::wstring ToLowerW(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

static const wchar_t* CHROMIUM_AUDIO_PATTERN = L"--utility-sub-type=audio.mojom.audioservice";
static const wchar_t* FIREFOX_AUDIO_PATTERNS[] = { L"-audioserver", L"--audioipc" };

static bool IsAudioTarget(const ProcInfo& pi, const std::vector<std::wstring>& extra)
{
    std::wstring cl = ToLowerW(pi.cmdline);
    if (cl.empty()) return false;
    if (cl.find(CHROMIUM_AUDIO_PATTERN) != std::wstring::npos) return true;
    std::wstring nl = ToLowerW(pi.name);
    if (nl == L"firefox.exe") {
        if (cl.find(L"-contentproc") == std::wstring::npos) return true;
        for (auto p : FIREFOX_AUDIO_PATTERNS)
            if (cl.find(p) != std::wstring::npos) return true;
    }
    for (auto& e : extra)
        if (!e.empty() && cl.find(ToLowerW(e)) != std::wstring::npos) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Manual-map injection (silent)
// ---------------------------------------------------------------------------

static bool InjectDll(DWORD pid, const std::vector<unsigned char>& dll)
{
    if (dll.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    const unsigned char* data = dll.data();
    auto dos = (const IMAGE_DOS_HEADER*)data;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (const IMAGE_NT_HEADERS64*)(data + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        !(nt->FileHeader.Characteristics & IMAGE_FILE_DLL))
        return false;

    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc) return false;

    bool ok = false;
    void* image = VirtualAllocEx(hProc, nullptr, nt->OptionalHeader.SizeOfImage,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    void* stubMem = nullptr;
    if (!image) goto cleanup;

    {
        SIZE_T written = 0;
        if (!WriteProcessMemory(hProc, image, data, nt->OptionalHeader.SizeOfHeaders, &written)) goto cleanup;
        auto sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (!sec->SizeOfRawData) continue;
            if (sec->PointerToRawData + sec->SizeOfRawData > dll.size()) goto cleanup;
            if (!WriteProcessMemory(hProc, (unsigned char*)image + sec->VirtualAddress,
                                    data + sec->PointerToRawData, sec->SizeOfRawData, &written))
                goto cleanup;
        }
        sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            DWORD prot = PAGE_READONLY;
            if (sec->Characteristics & IMAGE_SCN_CNT_CODE) prot = PAGE_EXECUTE_READ;
            else if (sec->Characteristics & IMAGE_SCN_MEM_WRITE) prot = PAGE_READWRITE;
            DWORD old;
            VirtualProtectEx(hProc, (unsigned char*)image + sec->VirtualAddress,
                             sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData, prot, &old);
        }
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

        SIZE_T stubSize = (SIZE_T)((uintptr_t)&LoadDllStubEnd - (uintptr_t)&LoadDllStub);
        if (stubSize == 0 || stubSize > 3800) goto cleanup;
        stubMem = VirtualAllocEx(hProc, nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!stubMem) goto cleanup;

        ManualInjectCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ImageBase        = (unsigned char*)image;
        ctx.NtHeaders        = (IMAGE_NT_HEADERS64*)((unsigned char*)image + dos->e_lfanew);
        ctx.BaseRelocation   = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress
            ? (IMAGE_BASE_RELOCATION*)((unsigned char*)image + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress) : nullptr;
        ctx.ImportDirectory  = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress
            ? (IMAGE_IMPORT_DESCRIPTOR*)((unsigned char*)image + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress) : nullptr;
        ctx.fnLoadLibraryA   = LoadLibraryA;
        ctx.fnGetProcAddress = GetProcAddress;
        long* remotePhase = (long*)((unsigned char*)stubMem + 4096 + sizeof(ManualInjectCtx));
        ctx.PhaseOut = remotePhase;

        SIZE_T written2 = 0;
        unsigned char* stubParams = (unsigned char*)stubMem + 4096;
        if (!WriteProcessMemory(hProc, stubMem, (void*)&LoadDllStub, stubSize, &written2)) goto cleanup;
        if (!WriteProcessMemory(hProc, stubParams, &ctx, sizeof(ctx), &written2)) goto cleanup;
        DWORD old;
        if (!VirtualProtectEx(hProc, stubMem, 4096, PAGE_EXECUTE_READ, &old)) goto cleanup;

        HANDLE ht = CreateRemoteThread(hProc, nullptr, 0, (LPTHREAD_START_ROUTINE)stubMem, stubParams, 0, nullptr);
        if (!ht) goto cleanup;
        WaitForSingleObject(ht, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeThread(ht, &exitCode);
        CloseHandle(ht);
        if (exitCode != 0) goto cleanup;

        VirtualFreeEx(hProc, stubMem, 0, MEM_RELEASE);
        stubMem = nullptr;
        ok = true;
    }

cleanup:
    if (stubMem) VirtualFreeEx(hProc, stubMem, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return ok;
}

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
