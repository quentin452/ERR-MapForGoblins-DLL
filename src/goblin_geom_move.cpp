#include "goblin_geom_move.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <spdlog/spdlog.h>

#include "goblin_collected.hpp"  // first_live_geom_instance — proven live CSWorldGeomIns source

namespace
{
    // er-relative anchors for THIS ERR build (docs/re/windows_msb_placement_write_re_findings.md).
    constexpr uintptr_t GETTER_RVA = 0x6c46e0;  // FUN_1406c46e0(module=inst+0x18, out mat4x4[16])
    constexpr int SETTER_VSLOT = 26;            // vtable[0xd0] = SetWorldMatrix(self, const float[16])

    using GetterFn = void(__fastcall *)(void *module, float *out_mat16);
    using SetterFn = void(__fastcall *)(void *self, const float *mat16);

    // SEH-guarded, NOINLINE calls into the engine. clang-cl elides __try around an inlined call, so the
    // call MUST be its own noinline function for the guard to survive (see docs/memory clang-cl-seh).
    __declspec(noinline) bool call_getter(GetterFn fn, void *inst, float *out)
    {
        __try { fn((char *)inst + 0x18, out); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    __declspec(noinline) bool call_setter(SetterFn fn, void *inst, const float *mat)
    {
        __try { fn(inst, mat); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool rpm(const void *addr, void *out, size_t n)
    {
        SIZE_T got = 0;
        return ReadProcessMemory(GetCurrentProcess(), addr, out, n, &got) && got == n;
    }

    bool finite3(const float *m) { return std::isfinite(m[12]) && std::isfinite(m[13]) && std::isfinite(m[14]); }
}

namespace goblin::geom_move
{
    MoveResult move_first(float dx, float dy, float dz)
    {
        MoveResult r;
        uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
        if (!base) { std::snprintf(r.err, sizeof(r.err), "eldenring.exe base not found"); return r; }

        void *inst = goblin::collected::first_live_geom_instance();
        if (!inst) { std::snprintf(r.err, sizeof(r.err), "no live geom instance (in-world + tiles loaded?)"); return r; }
        r.inst = (uint64_t)inst;
        uint64_t vt = 0;
        rpm(inst, &vt, 8);  // record the instance's vtable
        r.vtable = vt;

        auto getter = (GetterFn)(base + GETTER_RVA);

        // Read the current world matrix (engine getter — no offset guessing).
        float m[16] = {};
        if (!call_getter(getter, inst, m))
        {
            std::snprintf(r.err, sizeof(r.err), "getter faulted on inst %p", inst);
            return r;
        }
        if (!finite3(m))
        {
            std::snprintf(r.err, sizeof(r.err), "non-finite translation — wrong offset/inst, aborting vcall");
            return r;
        }
        // The SETTER writes the engine's cached WORLD matrix at inst+0x220 (what physics/render use),
        // NOT the +0x18 source pose module the getter rebuilds from — so verify via the cached matrix,
        // not the getter (confirmed live 2026-07-03: a +Δ setter call moves +0x220's translation by Δ,
        // touching exactly the one float; the getter still shows the module's original value).
        constexpr ptrdiff_t CACHE_MAT = 0x220;  // cached world matrix (row-major; translation @ 12,13,14)
        float cache[16] = {};
        if (rpm((char *)inst + CACHE_MAT, cache, sizeof(cache)) && finite3(cache))
        {
            r.before[0] = cache[12]; r.before[1] = cache[13]; r.before[2] = cache[14];
        }
        else
        {
            r.before[0] = m[12]; r.before[1] = m[13]; r.before[2] = m[14];  // fallback to the module
        }

        // Resolve the virtual setter (vtable[0xd0] = slot 26).
        void **vtbl = *(void ***)inst;
        SetterFn setter = (SetterFn)vtbl[SETTER_VSLOT];

        // Move by the delta (full matrix from the getter preserves rotation/scale; translation += delta).
        float moved_mat[16];
        memcpy(moved_mat, m, sizeof(moved_mat));
        moved_mat[12] = r.before[0] + dx; moved_mat[13] = r.before[1] + dy; moved_mat[14] = r.before[2] + dz;
        if (!call_setter(setter, inst, moved_mat))
        {
            std::snprintf(r.err, sizeof(r.err), "setter faulted (move)");
            return r;
        }
        rpm((char *)inst + CACHE_MAT, cache, sizeof(cache));
        r.moved[0] = cache[12]; r.moved[1] = cache[13]; r.moved[2] = cache[14];

        // Restore the original transform (leave the world unchanged).
        float restore_mat[16];
        memcpy(restore_mat, m, sizeof(restore_mat));
        restore_mat[12] = r.before[0]; restore_mat[13] = r.before[1]; restore_mat[14] = r.before[2];
        call_setter(setter, inst, restore_mat);
        rpm((char *)inst + CACHE_MAT, cache, sizeof(cache));
        r.restored[0] = cache[12]; r.restored[1] = cache[13]; r.restored[2] = cache[14];

        r.ok = true;
        spdlog::info("[GEOMMOVE] inst={:#x} vt={:#x} before=({:.2f},{:.2f},{:.2f}) moved=({:.2f},{:.2f},"
                     "{:.2f}) restored=({:.2f},{:.2f},{:.2f})", r.inst, r.vtable, r.before[0], r.before[1],
                     r.before[2], r.moved[0], r.moved[1], r.moved[2], r.restored[0], r.restored[1],
                     r.restored[2]);
        return r;
    }
}
