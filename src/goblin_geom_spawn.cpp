#include "goblin_geom_spawn.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>

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

    // ── deferred spawn queue, drained on the GAME's MAIN-UPDATE thread via a per-frame step hook ─────────
    // present-thread call = deadlock (lock inversion); worker-thread call = FAULT (registrar needs the game's
    // own thread context). So drain inside a hooked per-frame main-update step (the streamer's own thread).
    // SOLVED (windows_geom_spawn_thread_re_findings.md): the reqMgr per-frame update FUN_1406d31f0
    // (er+0x6d31f0) is called every frame in-world by the world-geom update FUN_140623410 on the
    // registrar's OWN thread, and RECEIVES the reqMgr as param_1. Hooking it drains exactly on that
    // thread+context. (Earlier guesses FUN_140699170 / FUN_14069a550 fired conditionally or not at all.)
    // AOB-resolved (STREAMER_STEP_FN) with this RVA as fallback.
    constexpr uintptr_t STREAMER_STEP_RVA = 0x6d31f0;   // FUN_1406d31f0 (reqMgr per-frame update)
    std::deque<std::wstring> g_queue;
    std::mutex g_qmtx;
    std::atomic<bool> g_hook_installed{false};
    EnsureAssetReqFn g_ensure_fn = nullptr;
    // char FUN_1406d31f0(reqMgr, FD4Time*, u8, u8, u8) — full ABI so the trampoline forwards every arg
    // (incl. the 5th, stack-passed) and returns the engine's al.
    using StepFn = char(__fastcall *)(void *, void *, uint8_t, uint8_t, uint8_t);
    StepFn g_step_orig = nullptr;

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

    // Per-frame reqMgr-update detour (FUN_1406d31f0): run orig, then drain ONE queued spawn on THIS
    // (game main-update) thread — the registrar's own context. reqMgr arrives here as param_1, but we
    // still resolve it via the singleton to keep the direct-call path identical.
    char __fastcall hk_step(void *reqMgr, void *time, uint8_t p3, uint8_t p4, uint8_t p5)
    {
        char ret = g_step_orig(reqMgr, time, p3, p4, p5);
        static std::atomic<uint64_t> fires{0};
        if (fires.fetch_add(1, std::memory_order_relaxed) == 0)
            spdlog::info("[SPAWNASSET] hk_step FIRED (first) — main-update thread reached");
        std::wstring name;
        {
            std::lock_guard<std::mutex> lk(g_qmtx);
            if (g_queue.empty()) return ret;
            name = std::move(g_queue.front());
            g_queue.pop_front();
        }
        uint64_t sing = 0, mgr = 0;
        if (resolve_mgr(sing, mgr)[0] || !mgr || !g_ensure_fn) return ret;
        void *req = nullptr;
        bool ok = call_ensure(g_ensure_fn, (void *)mgr, name.c_str(), req);
        spdlog::info("[SPAWNASSET] step serviced -> ok={} req={:p} (reqMgr=0x{:x})", ok, req,
                     (unsigned long long)mgr);
        return ret;
    }

    bool ensure_hook()
    {
        if (g_hook_installed.load()) return true;
        uintptr_t er = (uintptr_t)GetModuleHandleA("eldenring.exe");
        if (!er) return false;
        g_ensure_fn = (EnsureAssetReqFn)goblin::sig::resolve_func_aob(
            goblin::sig::ENSURE_ASSET_REQUEST_FN, er, ENSURE_ASSET_REQUEST_RVA, "ENSURE_ASSET_REQUEST");
        if (!g_ensure_fn) return false;
        // Hook by RVA directly: the FUN_1406d31f0 prologue AOB is NOT unique (matches 3 sites, first is a
        // different function) — AOB-first would hook the wrong one (live 2026-07-05). RVA is correct for
        // THIS build; re-find a unique AOB before shipping (rva_aob_hardening_backlog.md). Same posture as
        // goblin_geom_move (RVA call, no AOB yet).
        void *step_target = (void *)(er + STREAMER_STEP_RVA);
        try
        {
            // hook_now (immediate MH_EnableHook), NOT hook() — this installs LAZILY at first spawn, long
            // after enable_hooks()/MH_ApplyQueued already ran at DLL init, so a QUEUED enable would never
            // apply and the detour would never fire (live 2026-07-05: hooked but no hk_step).
            modutils::hook_now(step_target, reinterpret_cast<void *>(&hk_step),
                               reinterpret_cast<void **>(&g_step_orig));
        }
        catch (const std::exception &e) { spdlog::error("[SPAWNASSET] step hook failed: {}", e.what()); return false; }
        g_hook_installed = true;
        spdlog::info("[SPAWNASSET] reqMgr per-frame update (FUN_1406d31f0) hooked @ {:p} (rva er+{:#x})",
                     step_target, STREAMER_STEP_RVA);
        return true;
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

        // widen "AEG099_090" -> L"AEG099_090"
        std::wstring wname(r.name, r.name + std::strlen(r.name));

        // DEFAULT (pivot-2 refined): QUEUE the request for the MAIN-THREAD step hook to service — the direct
        // call on the present/RPC thread deadlocks (lock inversion with the streamer). `force` still fires the
        // direct call (diagnostic; WILL hang).
        if (!force)
        {
            if (!ensure_hook())
            {
                std::snprintf(r.err, sizeof(r.err), "step hook / registrar-fn init failed");
                return r;
            }
            {
                std::lock_guard<std::mutex> lk(g_qmtx);
                g_queue.push_back(std::move(wname));
            }
            r.ok = true;   // queued; the worker thread services it off the present thread
            spdlog::info("[SPAWNASSET] '{}' QUEUED for main-update-step spawn (reqMgr=0x{:x})", r.name, r.reqMgr);
            return r;
        }
        spdlog::warn("[SPAWNASSET] force: DIRECT call FUN_1406a5080('{}') — EXPECTED TO HANG (lock inversion).", r.name);

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
