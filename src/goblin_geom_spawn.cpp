#include "goblin_geom_spawn.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <spdlog/spdlog.h>

#include "modutils.hpp"
#include "re_signatures.hpp"

namespace
{
    // er-relative anchor for THIS build (docs/re/windows_geom_spawn_pivot2_re_findings.md). Dev probe →
    // RVA call like goblin_geom_move (the FUN_ has no AOB yet); the reqMgr singleton IS AOB-resolved.
    constexpr uintptr_t ENSURE_ASSET_REQUEST_RVA = 0x6a5080;  // FUN_1406a5080(reqMgr, wchar_t* name)
    constexpr uintptr_t REQ_MGR_OFFSET = 0x30;                // reqMgr = *(singleton + 0x30)

    using EnsureAssetReqFn = void *(__fastcall *)(void *mgr, const wchar_t *name);

    bool rpm(const void *addr, void *out, size_t n)
    {
        SIZE_T got = 0;
        return ReadProcessMemory(GetCurrentProcess(), addr, out, n, &got) && got == n;
    }

    // SEH-guarded, NOINLINE engine call (clang-cl elides __try around an inlined call — see
    // docs/memory clang-cl-seh). Returns the request pointer, or (via ok=false) a fault.
    __declspec(noinline) bool call_ensure(EnsureAssetReqFn fn, void *mgr, const wchar_t *name, void *&out)
    {
        __try { out = fn(mgr, name); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { out = nullptr; return false; }
    }

    // Resolve reqMgr = *(void**)(DAT_143d69ba8 + 0x30). Tries the primary AOB, then the backup (both load
    // the SAME singleton). Fills singleton/reqMgr; returns an error string ("" on success).
    const char *resolve_mgr(uint64_t &singleton_out, uint64_t &reqmgr_out)
    {
        singleton_out = reqmgr_out = 0;
        void **slot = modutils::scan<void *>({.aob = goblin::sig::GEOM_REQ_MGR, .relative_offsets = {{3, 7}}});
        if (!slot)
            slot = modutils::scan<void *>({.aob = goblin::sig::GEOM_REQ_MGR_BACKUP, .relative_offsets = {{3, 7}}});
        if (!slot) return "GEOM_REQ_MGR AOB not found (game update?)";

        void *singleton = nullptr;
        if (!rpm(slot, &singleton, sizeof(singleton)) || !singleton)
            return "reqMgr singleton null (not in-world yet?)";
        singleton_out = (uint64_t)singleton;

        void *reqMgr = nullptr;
        if (!rpm((const uint8_t *)singleton + REQ_MGR_OFFSET, &reqMgr, sizeof(reqMgr)) || !reqMgr)
            return "reqMgr (*[singleton+0x30]) null";
        reqmgr_out = (uint64_t)reqMgr;
        return "";
    }
}

namespace goblin::geom_spawn
{
    SpawnResult resolve_req_mgr()
    {
        SpawnResult r;
        const char *e = resolve_mgr(r.singleton, r.reqMgr);
        if (e[0]) { std::snprintf(r.err, sizeof(r.err), "%s", e); return r; }
        r.ok = true;
        spdlog::info("[SPAWNASSET] reqMgr resolved: singleton=0x{:x} reqMgr=0x{:x}", r.singleton, r.reqMgr);
        return r;
    }

    SpawnResult spawn_asset(const char *aegName, bool force)
    {
        SpawnResult r;
        if (!aegName || !aegName[0]) { std::snprintf(r.err, sizeof(r.err), "empty asset name"); return r; }
        std::snprintf(r.name, sizeof(r.name), "%s", aegName);

        uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
        if (!base) { std::snprintf(r.err, sizeof(r.err), "eldenring.exe base not found"); return r; }

        const char *e = resolve_mgr(r.singleton, r.reqMgr);
        if (e[0]) { std::snprintf(r.err, sizeof(r.err), "%s", e); return r; }

        // Default: read-only resolve, DO NOT call — the direct call deadlocks the present thread (see the
        // header note / the pivot-2 findings). `force` fires it anyway (for a future main-thread hook).
        if (!force)
        {
            r.ok = true;  // resolve succeeded; req stays 0 (no call made)
            spdlog::warn("[SPAWNASSET] '{}' resolve OK (reqMgr=0x{:x}) — direct call SKIPPED (deadlocks "
                         "present thread; pass force). See windows_geom_spawn_pivot2_re_findings.md.",
                         r.name, r.reqMgr);
            return r;
        }
        spdlog::warn("[SPAWNASSET] force: calling FUN_1406a5080('{}') — EXPECTED TO HANG (lock inversion).", r.name);

        // widen "AEG099_090" -> L"AEG099_090"
        std::wstring wname(r.name, r.name + std::strlen(r.name));

        EnsureAssetReqFn fn = (EnsureAssetReqFn)goblin::sig::resolve_func_aob(
            goblin::sig::ENSURE_ASSET_REQUEST_FN, base, ENSURE_ASSET_REQUEST_RVA, "ENSURE_ASSET_REQUEST");
        void *req = nullptr;
        if (!call_ensure(fn, (void *)r.reqMgr, wname.c_str(), req))
        {
            std::snprintf(r.err, sizeof(r.err), "FUN_1406a5080 faulted (reqMgr=0x%llx name=%s)",
                          (unsigned long long)r.reqMgr, r.name);
            return r;
        }
        r.req = (uint64_t)req;
        // req==0 is a VALID engine answer: the registrar's gate (mgr[0x185] / slot / id alloc) declined,
        // or the name isn't a known asset. ok reflects "the call ran"; the driver inspects .req.
        r.ok = true;
        spdlog::info("[SPAWNASSET] request '{}' -> req=0x{:x} (reqMgr=0x{:x}){}", r.name, r.req, r.reqMgr,
                     r.req ? "" : " [gate/unknown-asset — req=0]");
        return r;
    }
}
