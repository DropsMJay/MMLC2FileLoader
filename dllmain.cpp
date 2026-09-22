#include "pch.h"
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <MinHook.h>

// ============================================================================
// MMLC2 Mod Loader - .asi plugin (loaded by the Ultimate ASI Loader)
//
// Hooks the two resource-loading systems found in the collection:
//   - ResolveDiscResource (FUN_14021de90): fonts, system text, UI (.bin)
//   - ResolveGeneric      (FUN_14012df60): sprites, maps, menu screens,
//                          art gallery, MM7/MM8 data (.lzs, .bin, .PAC)
// When the game requests a resource (e.g. "illust/muse_rc7_002.lzs"), the
// loader checks whether "mods/illust/muse_rc7_002.lzs" exists next to the
// .exe. If it does, that file is served instead of the packed disc
// content. If not, the game proceeds normally (reads from the disc as
// usual).
// ============================================================================

// ----------------------------------------------------------------------
// CONFIG - toggle the debug features here
// ----------------------------------------------------------------------

// true = generates "mmlc2_all_resources_log.txt" with a deduplicated
// list of EVERY resource the game requested during the session - useful
// for discovering which files exist and can be modded.
static const bool DEBUG_LOG_ALL_FILES = false;

// true = generates "mmlc2_mod_redirects_log.txt" showing which files
// were actually REPLACED by a modded version (and which mod attempts
// failed to read) - useful for confirming a specific mod is being
// picked up by the game.
static const bool DEBUG_LOG_MOD_REDIRECTS = true;

// ----------------------------------------------------------------------
// Log 1: every resource seen (deduplicated)
// ----------------------------------------------------------------------

static FILE* g_allResourcesLog = nullptr;
static std::mutex g_allResourcesMutex;
static std::set<std::string> g_seenResources;
static int g_resourceCounter = 0;

static void LogResourceIfNew(const char* origin, const char* resourceName)
{
    if (!DEBUG_LOG_ALL_FILES || resourceName == nullptr) return;

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
    if (!DEBUG_LOG_MOD_REDIRECTS) return;

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
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash != nullptr) *(lastSlash + 1) = '\0';
    snprintf(outPath, outSize, "%smods\\%s", exePath, resourceName);
}

// ----------------------------------------------------------------------
// Hook for FUN_14021de90 (ResolveDiscResource) - fonts, UI, system text.
// Signature: undefined4 FUN_14021de90(longlong param_1)
// param_1 is used directly as char* (resource name).
//
// Redirect: writes the mod's bytes into the SAME global buffer the
// original function would use (DAT_1409ca7e8 + 0x629000) and returns
// the size read - exactly what the callers expect.
// ----------------------------------------------------------------------

typedef unsigned int(__fastcall* fnResolveDiscResource)(long long param_1);

static fnResolveDiscResource original_ResolveDiscResource = nullptr;

static const uintptr_t RVA_ResolveDiscResource = 0x21de90; // FUN_14021de90
static const uintptr_t RVA_PonteiroBaseBuffer = 0x9ca7e8; // DAT_1409ca7e8
static const uintptr_t OFFSET_BufferRecurso = 0x629000;

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
typedef void* (__fastcall* fnEngineAlloc)(long long size);

static fnResolveGeneric original_ResolveGeneric = nullptr;
static fnEngineAlloc g_engineAlloc = nullptr;

static const uintptr_t RVA_ResolveGeneric = 0x12df60; // FUN_14012df60
static const uintptr_t RVA_EngineAlloc = 0x13a3a0; // FUN_14013a3a0

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

    return original_ResolveGeneric(param_1, param_2, param_3);
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
    if (DEBUG_LOG_ALL_FILES)
        fopen_s(&g_allResourcesLog, "mmlc2_all_resources_log.txt", "w");

    if (DEBUG_LOG_MOD_REDIRECTS)
    {
        fopen_s(&g_modRedirectsLog, "mmlc2_mod_redirects_log.txt", "w");
        LogModRedirect("=== MMLC2 Mod Loader - redirect log ===\n");
    }

    if (MH_Initialize() != MH_OK) return;

    uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    g_engineAlloc = (fnEngineAlloc)(base + RVA_EngineAlloc);

    InstallOneHook(base, RVA_ResolveDiscResource,
        &Hooked_ResolveDiscResource, (void**)&original_ResolveDiscResource);

    InstallOneHook(base, RVA_ResolveGeneric,
        (void*)&Hooked_ResolveGeneric, (void**)&original_ResolveGeneric);

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