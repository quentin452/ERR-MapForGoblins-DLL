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
#include "goblin_inject.hpp"   // get_player_world_pos (worldPos for the by-id spawn helper)

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

    // ── native by-id spawn helper capture (RE the NEXT wall: the raw registrar drain FAULTS) ──────────────
    // FUN_1406d4e80(blockStreamerState, uint aegId, float* worldPos) is the engine's own "request AEG by id
    // at position" call (resolves the player's block, builds L"AEG###_###", worldPos-blockOrigin, then
    // registrar). We can't derive blockStreamerState statically, so CAPTURE it: hook FUN_1406d4e80 and log
    // its live args + how param_1 relates to the singleton, while the streamer legitimately calls it (warp →
    // block load). RE: docs/re/windows_geom_spawn_thread_re_findings.md "NEXT wall".
    constexpr uintptr_t BYID_SPAWN_RVA = 0x6d4e80;   // FUN_1406d4e80(state, aegId, worldPos)
    using ByIdFn = void *(__fastcall *)(void *state, uint32_t aegId, float *worldPos);
    ByIdFn g_byid_orig = nullptr;
    std::atomic<bool> g_byid_cap_installed{false};
    ByIdFn g_byid_fn = nullptr;   // resolved FUN_1406d4e80 for the drain to CALL (not the capture trampoline)

    // SEH-guarded call of the by-id helper FUN_1406d4e80(state, aegId, worldPos). out = returned handle
    // (0 = engine declined); returns false on a fault (bad state pointer).
    __declspec(noinline) bool call_byid(ByIdFn fn, void *state, uint32_t aegId, float *wp, void *&out)
    {
        __try { out = fn(state, aegId, wp); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { out = nullptr; return false; }
    }

    // Parse "AEG###_###" -> aegId (grp1*1000 + grp2), matching the engine's swprintf(L"AEG%03u_%03u",
    // aegId/1000, aegId%1000). Returns false if the name isn't in that shape.
    bool parse_aeg_id(const wchar_t *name, uint32_t &id_out)
    {
        if (!name) return false;
        // expect A E G d d d _ d d d
        if (name[0] != L'A' || name[1] != L'E' || name[2] != L'G') return false;
        uint32_t g1 = 0, g2 = 0;
        for (int k = 3; k < 6; ++k) { if (name[k] < L'0' || name[k] > L'9') return false; g1 = g1 * 10 + (name[k] - L'0'); }
        if (name[6] != L'_') return false;
        for (int k = 7; k < 10; ++k) { if (name[k] < L'0' || name[k] > L'9') return false; g2 = g2 * 10 + (name[k] - L'0'); }
        id_out = g1 * 1000 + g2;
        return true;
    }

    // The by-id helper FUN_1406d4e80 does NOT fire on normal streaming (live 2026-07-05 — a warp produced 0
    // [CAP4e80] lines). Normal proximity streaming calls the registrar FUN_1406a5080 DIRECTLY. So capture
    // the REGISTRAR's legit args instead: this reveals what param_1 actually is when the engine calls it
    // (block vs reqMgr) — the crux of why our raw reqMgr call faults.
    EnsureAssetReqFn g_reg_orig = nullptr;
    std::atomic<bool> g_reg_cap_installed{false};

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
    // (game main-update) thread. p1step = FUN_1406d31f0's param_1 (= DAT_143d69ba8 singleton per the RE).
    // The RAW registrar FUN_1406a5080(reqMgr, name) FAULTS even here (same TID as the streamer, correct
    // arg) — it needs per-call block/desc context. So drive the by-id helper FUN_1406d4e80(state, aegId,
    // worldPos), which resolves the player's block itself. `state` isn't statically known, so try candidate
    // pointers derived from p1step/singleton (SEH-guarded) and log which one the engine accepts.
    char __fastcall hk_step(void *p1step, void *time, uint8_t p3, uint8_t p4, uint8_t p5)
    {
        char ret = g_step_orig(p1step, time, p3, p4, p5);
        static std::atomic<uint64_t> fires{0};
        if (fires.fetch_add(1, std::memory_order_relaxed) == 0)
            spdlog::info("[SPAWNASSET] hk_step FIRED (first) — TID={} p1step={:p} (FUN_1406d31f0 thread)",
                         GetCurrentThreadId(), p1step);
        std::wstring name;
        {
            std::lock_guard<std::mutex> lk(g_qmtx);
            if (g_queue.empty()) return ret;
            name = std::move(g_queue.front());
            g_queue.pop_front();
        }
        // Drive the by-id helper FUN_1406d4e80(state=p1step, aegId, worldPos). state = FUN_1406d31f0's
        // param_1 was proven live (2026-07-05): the FIRST candidate, engine returns a nonzero handle, no
        // fault (the raw registrar faulted here — it needs the block context this helper builds).
        uint32_t aegId = 0;
        if (!parse_aeg_id(name.c_str(), aegId))
        {
            spdlog::warn("[SPAWNASSET] step: name not AEG###_### — skipping by-id drain");
            return ret;
        }
        if (!g_byid_fn) { spdlog::warn("[SPAWNASSET] step: FUN_1406d4e80 unresolved"); return ret; }
        float px = 0, py = 0, pz = 0;
        bool gotpos = goblin::get_player_world_pos(px, py, pz);
        float wp[3] = {px, py, pz};
        void *h = nullptr;
        bool ok = call_byid(g_byid_fn, p1step, aegId, gotpos ? wp : nullptr, h);
        spdlog::info("[SPAWNASSET] step serviced via FUN_1406d4e80 -> ok={} handle={:p} aegId={} "
                     "pos=({:.1f},{:.1f},{:.1f}) TID={}",
                     ok, h, aegId, wp[0], wp[1], wp[2], GetCurrentThreadId());
        return ret;
    }

    // Capture detour on FUN_1406d4e80: log the live (state, aegId, worldPos) + how `state` relates to the
    // singleton, so the drain can reproduce the call. Pass-through (read-only observation).
    void *__fastcall hk_byid_capture(void *state, uint32_t aegId, float *worldPos)
    {
        static std::atomic<int> n{0};
        int i = n.fetch_add(1, std::memory_order_relaxed);
        if (i < 16)
        {
            uint64_t sing = 0, mgr = 0;
            resolve_mgr(sing, mgr);
            uint64_t s1e8 = 0, ds1e8 = 0;
            if (sing) { s1e8 = sing + 0x1e8; rpm((const void *)s1e8, &ds1e8, sizeof(ds1e8)); }
            float wp[3] = {0, 0, 0};
            bool gotwp = worldPos && rpm(worldPos, wp, sizeof(wp));
            spdlog::info("[SPAWNASSET][CAP4e80] #{} state={:p} aegId={} (AEG{:03}_{:03}) worldPos={} "
                         "({:.1f},{:.1f},{:.1f}) | singleton=0x{:x} reqMgr=0x{:x} sing+0x1e8=0x{:x} "
                         "*(sing+0x1e8)=0x{:x}",
                         i, state, aegId, aegId / 1000, aegId % 1000, (void *)worldPos,
                         gotwp ? wp[0] : 0.f, gotwp ? wp[1] : 0.f, gotwp ? wp[2] : 0.f,
                         (unsigned long long)sing, (unsigned long long)mgr, (unsigned long long)s1e8,
                         (unsigned long long)ds1e8);
        }
        return g_byid_orig(state, aegId, worldPos);
    }

    // Capture detour on the registrar FUN_1406a5080(param_1, name): log param_1 + the L"AEG..." name it is
    // legitimately called with, and how param_1 relates to the singleton/reqMgr. Pass-through.
    void *__fastcall hk_reg_capture(void *p1, const wchar_t *name)
    {
        static std::atomic<int> n{0};
        int i = n.fetch_add(1, std::memory_order_relaxed);
        if (i < 24)
        {
            uint64_t sing = 0, mgr = 0;
            resolve_mgr(sing, mgr);
            // narrow the wide name for logging (ASCII AEG names)
            char nm[32] = {};
            if (name)
                for (int k = 0; k < 31 && name[k]; ++k) nm[k] = (char)name[k];
            spdlog::info("[SPAWNASSET][CAPREG] #{} TID={} param_1={:p} name='{}' | reqMgr=0x{:x} "
                         "p1==reqMgr:{} p1-reqMgr=0x{:x}",
                         i, GetCurrentThreadId(), p1, nm, (unsigned long long)mgr,
                         ((uint64_t)p1 == mgr), (unsigned long long)((uint64_t)p1 - mgr));
        }
        return g_reg_orig(p1, name);
    }

    bool ensure_hook()
    {
        if (g_hook_installed.load()) return true;
        uintptr_t er = (uintptr_t)GetModuleHandleA("eldenring.exe");
        if (!er) return false;
        g_ensure_fn = (EnsureAssetReqFn)goblin::sig::resolve_func_aob(
            goblin::sig::ENSURE_ASSET_REQUEST_FN, er, ENSURE_ASSET_REQUEST_RVA, "ENSURE_ASSET_REQUEST");
        if (!g_ensure_fn) return false;
        g_byid_fn = (ByIdFn)(er + BYID_SPAWN_RVA);   // FUN_1406d4e80 by-id spawn helper (RVA-direct)
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
    bool arm_byid_capture()
    {
        if (g_byid_cap_installed.load()) return true;
        uintptr_t er = (uintptr_t)GetModuleHandleA("eldenring.exe");
        if (!er) return false;
        void *target = (void *)(er + BYID_SPAWN_RVA);   // RVA-direct (no unique AOB yet)
        try
        {
            modutils::hook_now(target, reinterpret_cast<void *>(&hk_byid_capture),
                               reinterpret_cast<void **>(&g_byid_orig));
        }
        catch (const std::exception &e)
        {
            spdlog::error("[SPAWNASSET] byid capture hook failed: {}", e.what());
            return false;
        }
        g_byid_cap_installed = true;
        spdlog::info("[SPAWNASSET] byid capture hooked FUN_1406d4e80 @ {:p} (rva er+{:#x}) — warp to trigger "
                     "block streaming; see [CAP4e80] lines", target, BYID_SPAWN_RVA);
        return true;
    }

    bool arm_registrar_capture()
    {
        if (g_reg_cap_installed.load()) return true;
        uintptr_t er = (uintptr_t)GetModuleHandleA("eldenring.exe");
        if (!er) return false;
        void *target = goblin::sig::resolve_func_aob(
            goblin::sig::ENSURE_ASSET_REQUEST_FN, er, ENSURE_ASSET_REQUEST_RVA, "ENSURE_ASSET_REQUEST");
        if (!target) return false;
        try
        {
            modutils::hook_now(target, reinterpret_cast<void *>(&hk_reg_capture),
                               reinterpret_cast<void **>(&g_reg_orig));
        }
        catch (const std::exception &e)
        {
            spdlog::error("[SPAWNASSET] registrar capture hook failed: {}", e.what());
            return false;
        }
        g_reg_cap_installed = true;
        spdlog::info("[SPAWNASSET] registrar capture hooked FUN_1406a5080 @ {:p} — warp/move to trigger; see "
                     "[CAPREG] lines", target);
        return true;
    }

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
