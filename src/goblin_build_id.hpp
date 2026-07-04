#pragma once
//
// ELDEN RING build fingerprint.
//
// Every fixed RVA / AOB signature in re_signatures.hpp (+ the RVA-hardening backlog) is pinned to ONE
// game build. When Steam updates eldenring.exe under our feet, those resolutions silently point at the
// wrong bytes. Logging the exe's version at boot (next to the AOB PASS/FAIL health check) and exposing
// it over the debug-RPC (`er_version`) makes "am I on the build these signatures were derived from?"
// verifiable at a glance — both from the log and live. Pair with docs/re/patch_diff_maintenance.md.
//
// The version comes from eldenring.exe's VS_FIXEDFILEINFO. The version resource lives in the PE resource
// directory, which VMProtect / Steam-DRM leave intact (only code sections are packed), so this reads the
// real game version off the on-disk exe. Empty string on any failure (module/resource absent).

#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>
#include <winver.h>  // GetFileVersionInfo* / VerQueryValue / VS_FIXEDFILEINFO (needs version.lib)

namespace goblin
{
    inline std::string er_exe_version()
    {
        HMODULE er = GetModuleHandleW(L"eldenring.exe");
        if (!er) return {};
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(er, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return {};

        DWORD handle = 0;
        DWORD sz = GetFileVersionInfoSizeW(path, &handle);
        if (sz == 0) return {};
        std::vector<unsigned char> buf(sz);
        if (!GetFileVersionInfoW(path, handle, sz, buf.data())) return {};

        VS_FIXEDFILEINFO *ffi = nullptr;
        UINT len = 0;
        if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void **>(&ffi), &len) || !ffi || len == 0)
            return {};

        char out[48];
        std::snprintf(out, sizeof(out), "%u.%u.%u.%u",
                      static_cast<unsigned>(HIWORD(ffi->dwFileVersionMS)),
                      static_cast<unsigned>(LOWORD(ffi->dwFileVersionMS)),
                      static_cast<unsigned>(HIWORD(ffi->dwFileVersionLS)),
                      static_cast<unsigned>(LOWORD(ffi->dwFileVersionLS)));
        return out;
    }
}
