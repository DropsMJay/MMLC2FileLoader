#define _CRT_SECURE_NO_WARNINGS
#include "pch.h"
// ============================================================================
// MMLC2 Mod Loader - .asi plugin (loaded by the Ultimate ASI Loader)
//
// Hooks the two resource-loading systems found in the collection:
//   - ResolveDiscResource (FUN_14021de90): fonts, system text, UI (.bin)
//   - ResolveGeneric      (FUN_14012df60): sprites, maps, menu screens,
//                          art gallery, MM7/MM8 data (.lzs, .bin, .PAC)
// When the game requests a resource (e.g. "illust/muse_rc7_002.lzs"), the
// loader checks whether "mods/illust/muse_rc7_002.lzs" exists next to
// the .exe. If it does, that file is served instead of the packed disc
// content. If not, the game proceeds normally (reads from the disc as
// usual).
//
// Debug logging is controlled by mmlc2_modloader.ini (created
// automatically next to the .exe on first run) - see LoadConfig() below.
// ============================================================================

#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <MinHook.h>

// ----------------------------------------------------------------------
// CONFIG - read from mmlc2_modloader.ini next to the .exe, so the debug
// features can be toggled without recompiling. If the .ini doesn't
// exist yet, one is created automatically with these defaults.
// ----------------------------------------------------------------------

static bool g_debugLogAllFiles = false;
static bool g_debugLogModRedirects = true;

// ----------------------------------------------------------------------
// Helper: writes <exe_folder> (with trailing backslash) into outPath
// ----------------------------------------------------------------------

static void GetExeFolder(char* outPath, size_t outSize)
{
    GetModuleFileNameA(nullptr, outPath, (DWORD)outSize);
    char* lastSlash = strrchr(outPath, '\\');
    if (lastSlash != nullptr) *(lastSlash + 1) = '\0';
}

static void LoadConfig()
{
    char exeFolder[MAX_PATH];
    GetExeFolder(exeFolder, sizeof(exeFolder));

    char iniPath[MAX_PATH];
    snprintf(iniPath, sizeof(iniPath), "%smmlc2_modloader.ini", exeFolder);

    // First run: create the .ini with default values so there's
    // something to edit.
    if (GetFileAttributesA(iniPath) == INVALID_FILE_ATTRIBUTES)
    {
        WritePrivateProfileStringA("Debug", "LogAllFiles", "0", iniPath);
        WritePrivateProfileStringA("Debug", "LogModRedirects", "1", iniPath);
    }

    g_debugLogAllFiles = GetPrivateProfileIntA("Debug", "LogAllFiles", 0, iniPath) != 0;
    g_debugLogModRedirects = GetPrivateProfileIntA("Debug", "LogModRedirects", 1, iniPath) != 0;
}

// ----------------------------------------------------------------------
// Log 1: every resource seen (deduplicated)
// ----------------------------------------------------------------------

static FILE* g_allResourcesLog = nullptr;
static std::mutex g_allResourcesMutex;
static std::set<std::string> g_seenResources;
static int g_resourceCounter = 0;

static void LogResourceIfNew(const char* origin, const char* resourceName)
{
    if (!g_debugLogAllFiles || resourceName == nullptr) return;

    std::lock_guard<std::mutex> lock(g_allResourcesMutex);
    if (g_seenResources.find(resourceName) != g_seenResources.end())
        return; // already seen, don't log it again

    g_seenResources.insert(resourceName);
    g_resourceCounter++;

    if (g_allResourcesLog != nullptr)
    {
        fprintf(g_allResourcesLog, "%04d [%s] %s\n", g_resourceCounter, origin, resourceName);
        fflush(g_allResourcesLog);
    }
}

// ----------------------------------------------------------------------
// Log 2: mod redirects (success or read failure)
// ----------------------------------------------------------------------

static FILE* g_modRedirectsLog = nullptr;
static std::mutex g_modRedirectsMutex;

static void LogModRedirect(const char* fmt, ...)
{
    if (!g_debugLogModRedirects) return;

    std::lock_guard<std::mutex> lock(g_modRedirectsMutex);
    if (!g_modRedirectsLog) return;

    va_list args;
    va_start(args, fmt);
    vfprintf(g_modRedirectsLog, fmt, args);
    va_end(args);
    fflush(g_modRedirectsLog);
}

// ----------------------------------------------------------------------
// Helper: builds the absolute path <exe_folder>\mods\<resource_name>
// ----------------------------------------------------------------------

static void BuildModPath(const char* resourceName, char* outPath, size_t outSize)
{
    char exeFolder[MAX_PATH];
    GetExeFolder(exeFolder, sizeof(exeFolder));
    snprintf(outPath, outSize, "%smods\\%s", exeFolder, resourceName);
}

// ----------------------------------------------------------------------
// Hook for FUN_14021de90 (ResolveDiscResource) - fonts, UI, system text.
// Signature: undefined4 FUN_14021de90(long long param_1)
// param_1 is used directly as char* (resource name).
//
// Redirect: writes the mod's bytes into the SAME global buffer the
// original function would use (DAT_1409ca7e8 + 0x629000) and returns
// the size read - exactly what the callers expect.
// ----------------------------------------------------------------------

typedef unsigned int(__fastcall* fnResolveDiscResource)(long long param_1);

static fnResolveDiscResource original_ResolveDiscResource = nullptr;

static const uintptr_t RVA_ResolveDiscResource = 0x21de90; // FUN_14021de90
static const uintptr_t RVA_PonteiroBaseBuffer  = 0x9ca7e8; // DAT_1409ca7e8
static const uintptr_t OFFSET_BufferRecurso    = 0x629000;

static bool TryReadModFile_Buffer(const char* resourceName, unsigned int* outSize)
{
    char modPath[MAX_PATH];
    BuildModPath(resourceName, modPath, sizeof(modPath));

    HANDLE hFile = CreateFileA(modPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false; // no mod for this resource - fall through to normal flow

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0)
    {
        CloseHandle(hFile);
        LogModRedirect("[FAIL] \"%s\" exists but has an invalid size\n", modPath);
        return false;
    }

    uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    void* basePtrAddr = (void*)(base + RVA_PonteiroBaseBuffer);
    uintptr_t bufferBase = *(uintptr_t*)basePtrAddr;
    void* buffer = (void*)(bufferBase + OFFSET_BufferRecurso);

    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, buffer, (DWORD)fileSize.QuadPart, &bytesRead, nullptr);
    CloseHandle(hFile);

    if (!ok || bytesRead != (DWORD)fileSize.QuadPart)
    {
        LogModRedirect("[FAIL] \"%s\" error while reading (read=%lu, expected=%lld)\n",
                        modPath, bytesRead, (long long)fileSize.QuadPart);
        return false;
    }

    LogModRedirect("[OK] \"%s\" <- \"%s\" (%lu bytes)\n", resourceName, modPath, bytesRead);
    *outSize = bytesRead;
    return true;
}

static unsigned int __fastcall Hooked_ResolveDiscResource(long long param_1)
{
    const char* resourceName = (param_1 != 0) ? (const char*)param_1 : nullptr;

    LogResourceIfNew("ResolveDiscResource", resourceName);

    if (resourceName != nullptr)
    {
        unsigned int modSize = 0;
        if (TryReadModFile_Buffer(resourceName, &modSize))
            return modSize;
    }

    return original_ResolveDiscResource(param_1);
}

// ----------------------------------------------------------------------
// Hook for FUN_14012df60 (ResolveGeneric) - sprites, maps, menus, art
// gallery, MM7/MM8 data (.lzs, .bin, .PAC).
// Signature: LPVOID FUN_14012df60(char *param_1, LPVOID param_2, DWORD *param_3)
// param_1 = path (usually prefixed with "DISC::")
// param_2 = caller-provided buffer, or nullptr (function allocates one)
// param_3 = output pointer for the size read (can be nullptr)
//
// We only intercept the param_2 == nullptr case - the only one we can
// safely redirect without knowing in advance the size of a caller-
// provided buffer. Memory is allocated using the SAME allocator the
// engine uses (FUN_14013a3a0) to stay compatible with whoever frees it
// afterwards.
// ----------------------------------------------------------------------

typedef LPVOID(__fastcall* fnResolveGeneric)(char* param_1, LPVOID param_2, DWORD* param_3);
typedef void*(__fastcall* fnEngineAlloc)(long long size);

static fnResolveGeneric original_ResolveGeneric = nullptr;
static fnEngineAlloc g_engineAlloc = nullptr;

static const uintptr_t RVA_ResolveGeneric = 0x12df60; // FUN_14012df60
static const uintptr_t RVA_EngineAlloc    = 0x13a3a0; // FUN_14013a3a0

static LPVOID __fastcall Hooked_ResolveGeneric(char* param_1, LPVOID param_2, DWORD* param_3)
{
    if (param_1 != nullptr)
    {
        const char* nameForLog = param_1;
        if (strncmp(nameForLog, "DISC::", 6) == 0) nameForLog += 6;
        LogResourceIfNew("ResolveGeneric", nameForLog);
    }

    if (param_1 != nullptr && param_2 == nullptr)
    {
        const char* resourceName = param_1;
        if (strncmp(resourceName, "DISC::", 6) == 0)
            resourceName += 6;

        char modPath[MAX_PATH];
        BuildModPath(resourceName, modPath, sizeof(modPath));

        HANDLE hFile = CreateFileA(modPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER fileSize;
            if (GetFileSizeEx(hFile, &fileSize) && fileSize.QuadPart > 0 && g_engineAlloc != nullptr)
            {
                // NOTE: no extra size prefix is added here. FUN_14000a910
                // (the original pack-read function) returns a plain data
                // buffer with no header of its own - the size is passed
                // out via a separate out-param when the caller provides
                // one. When callers omit that out-param (e.g. the async
                // CTArcLoader chain), they instead read a 4-byte size
                // field that's already part of the .lzs container FORMAT
                // ITSELF (embedded by the file, not added by the engine).
                // Since our mod files are real repacked .lzs containers,
                // that header is already present in the bytes we read -
                // adding another one here would double it up and break
                // decompression.
                void* buffer = g_engineAlloc(fileSize.QuadPart);
                if (buffer != nullptr)
                {
                    DWORD bytesRead = 0;
                    BOOL ok = ReadFile(hFile, buffer, (DWORD)fileSize.QuadPart, &bytesRead, nullptr);
                    CloseHandle(hFile);

                    if (ok && bytesRead == (DWORD)fileSize.QuadPart)
                    {
                        if (param_3 != nullptr) *param_3 = bytesRead;
                        LogModRedirect("[OK] \"%s\" <- \"%s\" (%lu bytes)\n",
                                       resourceName, modPath, bytesRead);
                        return buffer;
                    }
                    LogModRedirect("[FAIL] \"%s\" error while reading\n", modPath);
                }
            }
            else
            {
                CloseHandle(hFile);
            }
        }
    }

    // Buffer-provided case (param_2 != nullptr): the caller already
    // reserved its own destination buffer, so we can't just allocate a
    // fresh one and hand it back. Two allocation patterns were observed
    // for this call shape:
    //
    // - A growing "bump" arena (e.g. FUN_1402cfcc0, caller RVA 0x2cfe37):
    //   safe to exceed the original size, because the caller recomputes
    //   the NEXT resource's slot based on the actual size we report back
    //   through *param_3 - it just shifts forward. The only real limit is
    //   the arena's total remaining capacity, which we can't know, but in
    //   practice there's headroom for modest size increases (fonts,
    //   small data tables).
    //
    // - A reused scratch buffer shared across multiple resources (e.g.
    //   FUN_1402cf9d0 / rm10 sprite/* files, caller RVA 0x2cfaf6): this
    //   one is genuinely fixed-capacity - writing past it corrupts
    //   adjacent memory. Any caller NOT explicitly whitelisted below is
    //   treated this way, conservatively.
    //
    // We only allow growth for whitelisted caller RVAs known to use the
    // safe bump-arena pattern. Anything else keeps the strict "mod must
    // fit within the size the original call actually used" rule.
    static const uintptr_t SAFE_TO_GROW_CALLER_RVAS[] = {
        0x2cfe37, // FUN_1402cfcc0 - rm10/bin/media/bin/* bump arena
    };

    if (param_1 != nullptr && param_2 != nullptr)
    {
        const char* resourceName = param_1;
        if (strncmp(resourceName, "DISC::", 6) == 0)
            resourceName += 6;

        void* returnAddr = _ReturnAddress();
        uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
        uintptr_t callerRva = (uintptr_t)returnAddr - base;

        bool safeToGrow = false;
        for (uintptr_t whitelisted : SAFE_TO_GROW_CALLER_RVAS)
        {
            if (callerRva == whitelisted) { safeToGrow = true; break; }
        }

        LPVOID result = original_ResolveGeneric(param_1, param_2, param_3);

        if (result != nullptr && param_3 != nullptr)
        {
            DWORD originalSize = *param_3;

            char modPath[MAX_PATH];
            BuildModPath(resourceName, modPath, sizeof(modPath));

            HANDLE hFile = CreateFileA(modPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE)
            {
                LARGE_INTEGER modSize;
                if (GetFileSizeEx(hFile, &modSize) && modSize.QuadPart > 0)
                {
                    bool fits = safeToGrow || ((DWORD)modSize.QuadPart <= originalSize);

                    if (fits)
                    {
                        DWORD bytesRead = 0;
                        BOOL ok = ReadFile(hFile, param_2, (DWORD)modSize.QuadPart, &bytesRead, nullptr);
                        if (ok && bytesRead == (DWORD)modSize.QuadPart)
                        {
                            *param_3 = bytesRead;
                            LogModRedirect(
                                "[OK-BUFFERED] \"%s\" <- \"%s\" (%lu bytes, original was %lu%s)\n",
                                resourceName, modPath, bytesRead, originalSize,
                                safeToGrow ? ", growth allowed for this caller" : "");
                        }
                        else
                        {
                            LogModRedirect("[FAIL] \"%s\" error while reading (buffered)\n", modPath);
                        }
                    }
                    else
                    {
                        LogModRedirect(
                            "[FAIL] \"%s\" mod is %lld bytes, larger than the %lu bytes reserved for "
                            "this resource, and its caller (RVA 0x%llX) is not known to safely support "
                            "growth - skipped\n",
                            modPath, (long long)modSize.QuadPart, originalSize,
                            (unsigned long long)callerRva);
                    }
                }
                CloseHandle(hFile);
            }
        }

        return result;
    }

    return original_ResolveGeneric(param_1, param_2, param_3);
}

// ----------------------------------------------------------------------
// Hook for FUN_14000a910 - the low-level pack-read function shared by
// EVERY resource-loading system found so far (ResolveDiscResource,
// ResolveGeneric, and the per-room clones like FUN_1402cfeb0 used for
// rm10 data and possibly audio indices/headers). Hooking here catches
// mods for resources that never reach our higher-level hooks, without
// needing to find and hook every clone individually.
//
// Signature (from Ghidra's own analysis of the function body, x64
// __fastcall convention): FUN_14000a910(param_1 unused/context,
// param_2 = path string, param_3 = destination buffer or 0 (auto-
// allocate), param_4 = int* output size, can be null).
//
// Same dual-mode redirect logic as ResolveGeneric: auto-allocate case
// is handled directly; buffer-provided case runs the original first and
// only overwrites if the mod fits (or the caller is whitelisted to grow).
// ----------------------------------------------------------------------

typedef long long(__fastcall* fnEngineDiscRead)(
    unsigned long long param_1, const char* param_2, long long param_3, int* param_4);

static fnEngineDiscRead original_EngineDiscRead = nullptr;
static const uintptr_t RVA_EngineDiscRead = 0xa910; // FUN_14000a910

static long long __fastcall Hooked_EngineDiscRead(
    unsigned long long param_1, const char* param_2, long long param_3, int* param_4)
{
    if (param_2 != nullptr)
    {
        const char* nameForLog = param_2;
        if (strncmp(nameForLog, "DISC::", 6) == 0) nameForLog += 6;
        LogResourceIfNew("EngineDiscRead", nameForLog);
    }

    if (param_2 != nullptr && param_3 == 0)
    {
        const char* resourceName = param_2;
        if (strncmp(resourceName, "DISC::", 6) == 0)
            resourceName += 6;

        char modPath[MAX_PATH];
        BuildModPath(resourceName, modPath, sizeof(modPath));

        HANDLE hFile = CreateFileA(modPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER fileSize;
            if (GetFileSizeEx(hFile, &fileSize) && fileSize.QuadPart > 0 && g_engineAlloc != nullptr)
            {
                void* buffer = g_engineAlloc(fileSize.QuadPart);
                if (buffer != nullptr)
                {
                    DWORD bytesRead = 0;
                    BOOL ok = ReadFile(hFile, buffer, (DWORD)fileSize.QuadPart, &bytesRead, nullptr);
                    CloseHandle(hFile);

                    if (ok && bytesRead == (DWORD)fileSize.QuadPart)
                    {
                        if (param_4 != nullptr) *param_4 = (int)bytesRead;
                        LogModRedirect("[OK-LOWLEVEL] \"%s\" <- \"%s\" (%lu bytes)\n",
                                       resourceName, modPath, bytesRead);
                        return (long long)buffer;
                    }
                    LogModRedirect("[FAIL] \"%s\" error while reading (low-level)\n", modPath);
                }
            }
            else
            {
                CloseHandle(hFile);
            }
        }
    }
    else if (param_2 != nullptr && param_3 != 0)
    {
        const char* resourceName = param_2;
        if (strncmp(resourceName, "DISC::", 6) == 0)
            resourceName += 6;

        long long result = original_EngineDiscRead(param_1, param_2, param_3, param_4);

        if (result != 0 && param_4 != nullptr)
        {
            int originalSize = *param_4;

            char modPath[MAX_PATH];
            BuildModPath(resourceName, modPath, sizeof(modPath));

            HANDLE hFile = CreateFileA(modPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE)
            {
                LARGE_INTEGER modSize;
                if (GetFileSizeEx(hFile, &modSize) && modSize.QuadPart > 0 &&
                    (int)modSize.QuadPart <= originalSize)
                {
                    DWORD bytesRead = 0;
                    BOOL ok = ReadFile(hFile, (LPVOID)param_3, (DWORD)modSize.QuadPart, &bytesRead, nullptr);
                    if (ok && bytesRead == (DWORD)modSize.QuadPart)
                    {
                        *param_4 = (int)bytesRead;
                        LogModRedirect(
                            "[OK-LOWLEVEL-BUFFERED] \"%s\" <- \"%s\" (%lu bytes, original was %d)\n",
                            resourceName, modPath, bytesRead, originalSize);
                    }
                    else
                    {
                        LogModRedirect("[FAIL] \"%s\" error while reading (low-level buffered)\n", modPath);
                    }
                }
                CloseHandle(hFile);
            }
        }

        return result;
    }

    return original_EngineDiscRead(param_1, param_2, param_3, param_4);
}

// ----------------------------------------------------------------------
// Hook for FUN_140140030 - audio system (.xwb wave banks, used by
// several games in the collection). Used only for cataloging
// (LogResourceIfNew); does not implement audio redirection.
// ----------------------------------------------------------------------

typedef unsigned long long(__fastcall* fnLoadMM8Audio)(
    unsigned long long param_1, char* param_2, unsigned int param_3, unsigned long long param_4);

static fnLoadMM8Audio original_LoadMM8Audio = nullptr;
static const uintptr_t RVA_LoadMM8Audio = 0x140030; // FUN_140140030

static unsigned long long __fastcall Hooked_LoadMM8Audio(
    unsigned long long param_1, char* param_2, unsigned int param_3, unsigned long long param_4)
{
    LogResourceIfNew("LoadMM8Audio", param_2);
    return original_LoadMM8Audio(param_1, param_2, param_3, param_4);
}

// ----------------------------------------------------------------------
// Hook installation via MinHook
// ----------------------------------------------------------------------

static bool InstallOneHook(uintptr_t base, uintptr_t rva, void* detour, void** original)
{
    void* target = (void*)(base + rva);
    if (MH_CreateHook(target, detour, original) != MH_OK) return false;
    if (MH_EnableHook(target) != MH_OK) return false;
    return true;
}

static void InstallHooks()
{
    LoadConfig();

    char exeFolder[MAX_PATH];
    GetExeFolder(exeFolder, sizeof(exeFolder));

    if (g_debugLogAllFiles)
    {
        char logPath[MAX_PATH];
        snprintf(logPath, sizeof(logPath), "%smmlc2_all_resources_log.txt", exeFolder);
        fopen_s(&g_allResourcesLog, logPath, "w");
    }

    if (g_debugLogModRedirects)
    {
        char logPath[MAX_PATH];
        snprintf(logPath, sizeof(logPath), "%smmlc2_mod_redirects_log.txt", exeFolder);
        fopen_s(&g_modRedirectsLog, logPath, "w");
        LogModRedirect("=== MMLC2 Mod Loader - redirect log ===\n");
    }

    if (MH_Initialize() != MH_OK) return;

    uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    g_engineAlloc = (fnEngineAlloc)(base + RVA_EngineAlloc);

    InstallOneHook(base, RVA_ResolveDiscResource,
                   &Hooked_ResolveDiscResource, (void**)&original_ResolveDiscResource);

    InstallOneHook(base, RVA_ResolveGeneric,
                   (void*)&Hooked_ResolveGeneric, (void**)&original_ResolveGeneric);

    InstallOneHook(base, RVA_EngineDiscRead,
                   (void*)&Hooked_EngineDiscRead, (void**)&original_EngineDiscRead);

    InstallOneHook(base, RVA_LoadMM8Audio,
                   &Hooked_LoadMM8Audio, (void**)&original_LoadMM8Audio);
}

// ----------------------------------------------------------------------
// Entry point (called by the ASI Loader when it loads this .asi)
// ----------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        InstallHooks();
        break;
    case DLL_PROCESS_DETACH:
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        if (g_allResourcesLog) fclose(g_allResourcesLog);
        if (g_modRedirectsLog) fclose(g_modRedirectsLog);
        break;
    }
    return TRUE;
}
