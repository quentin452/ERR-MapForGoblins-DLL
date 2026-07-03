#include "goblin_geom_move.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <spdlog/spdlog.h>

#include "goblin_collected.hpp"  // first_live_geom_instance / list_live_geom_instances
#include "goblin_inject.hpp"      // get_player_world_pos (nearest-to-player selection)

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

    // Persistence-probe state (single held instance; present-thread only, no lock needed).
    static void *g_held = nullptr;
    static float g_held_before[3] = {};

    MoveResult move_hold(float dx, float dy, float dz)
    {
        MoveResult r;
        uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
        if (!base) { std::snprintf(r.err, sizeof(r.err), "eldenring.exe base not found"); return r; }
        void *inst = goblin::collected::first_live_geom_instance();
        if (!inst) { std::snprintf(r.err, sizeof(r.err), "no live geom instance"); return r; }
        r.inst = (uint64_t)inst;
        rpm(inst, &r.vtable, 8);

        constexpr ptrdiff_t CACHE_MAT = 0x220;
        float m[16] = {};
        auto getter = (GetterFn)(base + GETTER_RVA);
        if (!call_getter(getter, inst, m) || !finite3(m))
        {
            std::snprintf(r.err, sizeof(r.err), "getter faulted / non-finite");
            return r;
        }
        float cache[16] = {};
        if (!rpm((char *)inst + CACHE_MAT, cache, sizeof(cache)) || !finite3(cache))
        {
            std::snprintf(r.err, sizeof(r.err), "cache matrix unreadable");
            return r;
        }
        r.before[0] = cache[12]; r.before[1] = cache[13]; r.before[2] = cache[14];

        void **vtbl = *(void ***)inst;
        SetterFn setter = (SetterFn)vtbl[SETTER_VSLOT];
        float moved_mat[16];
        memcpy(moved_mat, m, sizeof(moved_mat));
        moved_mat[12] = r.before[0] + dx; moved_mat[13] = r.before[1] + dy; moved_mat[14] = r.before[2] + dz;
        if (!call_setter(setter, inst, moved_mat)) { std::snprintf(r.err, sizeof(r.err), "setter faulted"); return r; }
        rpm((char *)inst + CACHE_MAT, cache, sizeof(cache));
        r.moved[0] = cache[12]; r.moved[1] = cache[13]; r.moved[2] = cache[14];

        g_held = inst;
        g_held_before[0] = r.before[0]; g_held_before[1] = r.before[1]; g_held_before[2] = r.before[2];
        r.ok = true;
        spdlog::info("[GEOMMOVE] HOLD inst={:#x} before=({:.2f},{:.2f},{:.2f}) moved=({:.2f},{:.2f},{:.2f})",
                     r.inst, r.before[0], r.before[1], r.before[2], r.moved[0], r.moved[1], r.moved[2]);
        return r;
    }

    MoveResult move_near(float dx, float dy, float dz, float &dist)
    {
        dist = -1.0f;
        MoveResult r;
        uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
        if (!base) { std::snprintf(r.err, sizeof(r.err), "eldenring.exe base not found"); return r; }
        float px = 0, py = 0, pz = 0;
        if (!goblin::get_player_world_pos(px, py, pz))
        {
            std::snprintf(r.err, sizeof(r.err), "player pos unresolved (in-world?)");
            return r;
        }
        // Enumerate loaded geom instances; pick the one whose +0x220 translation is nearest the player.
        static void *insts[4096];
        size_t cnt = goblin::collected::list_live_geom_instances(insts, 4096);
        if (!cnt) { std::snprintf(r.err, sizeof(r.err), "no live geom instances"); return r; }
        void *best = nullptr;
        float best_d2 = 1e30f;
        float best_m[16] = {};
        for (size_t i = 0; i < cnt; i++)
        {
            float cache[16] = {};
            if (!rpm((char *)insts[i] + 0x220, cache, sizeof(cache)) || !finite3(cache)) continue;
            float ddx = cache[12] - px, ddy = cache[13] - py, ddz = cache[14] - pz;
            float d2 = ddx * ddx + ddy * ddy + ddz * ddz;
            if (d2 < best_d2) { best_d2 = d2; best = insts[i]; memcpy(best_m, cache, sizeof(best_m)); }
        }
        if (!best) { std::snprintf(r.err, sizeof(r.err), "no readable instance near player"); return r; }
        dist = std::sqrt(best_d2);
        r.inst = (uint64_t)best;
        rpm(best, &r.vtable, 8);
        r.before[0] = best_m[12]; r.before[1] = best_m[13]; r.before[2] = best_m[14];

        // Build the moved matrix from the getter (preserves rotation/scale), translation += delta.
        auto getter = (GetterFn)(base + GETTER_RVA);
        float m[16] = {};
        if (!call_getter(getter, best, m) || !finite3(m)) memcpy(m, best_m, sizeof(m));
        float moved_mat[16];
        memcpy(moved_mat, m, sizeof(moved_mat));
        moved_mat[12] = r.before[0] + dx; moved_mat[13] = r.before[1] + dy; moved_mat[14] = r.before[2] + dz;
        void **vtbl = *(void ***)best;
        SetterFn setter = (SetterFn)vtbl[SETTER_VSLOT];
        if (!call_setter(setter, best, moved_mat)) { std::snprintf(r.err, sizeof(r.err), "setter faulted"); return r; }
        float cache[16] = {};
        rpm((char *)best + 0x220, cache, sizeof(cache));
        r.moved[0] = cache[12]; r.moved[1] = cache[13]; r.moved[2] = cache[14];

        g_held = best;
        g_held_before[0] = r.before[0]; g_held_before[1] = r.before[1]; g_held_before[2] = r.before[2];
        r.ok = true;
        spdlog::info("[GEOMMOVE] NEAR inst={:#x} dist={:.2f} before=({:.2f},{:.2f},{:.2f}) moved=({:.2f},"
                     "{:.2f},{:.2f})", r.inst, dist, r.before[0], r.before[1], r.before[2], r.moved[0],
                     r.moved[1], r.moved[2]);
        return r;
    }

    MoveResult read_held()
    {
        MoveResult r;
        if (!g_held) { std::snprintf(r.err, sizeof(r.err), "nothing held (call move_hold first)"); return r; }
        r.inst = (uint64_t)g_held;
        r.before[0] = g_held_before[0]; r.before[1] = g_held_before[1]; r.before[2] = g_held_before[2];
        float cache[16] = {};
        if (!rpm((char *)g_held + 0x220, cache, sizeof(cache)) || !finite3(cache))
        {
            std::snprintf(r.err, sizeof(r.err), "held instance no longer readable");
            return r;
        }
        r.moved[0] = cache[12]; r.moved[1] = cache[13]; r.moved[2] = cache[14];
        r.restored[0] = cache[12]; r.restored[1] = cache[13]; r.restored[2] = cache[14];  // current == moved
        r.ok = true;
        return r;
    }

    // Diagnostic: count loaded geom instances + histogram their vtables (classes). Answers "why did
    // some objects move and others not" — move_all only touches CSWorldGeomIns-family instances (AEG
    // assets), never map parts (walls/terrain), and is capped; LOD gives an asset >1 instance. Returns
    // a summary string in .err (reused), .inst = total count.
    // Read-only recon for the ADD-new-placement route 1: dump a live geom instance's header + its
    // CSMsbParts record (inst+0x10) so we know exactly what a cloned record must satisfy for the
    // Dynamic ctor FUN_1406b9880 (rec+0x124, rec+0x18b, model refs). Logs hex to [GEOMDUMP]; safe.
    static void dump_hex(const char *tag, const uint8_t *b, size_t n)
    {
        char line[8 + 16 * 3 + 2];
        for (size_t off = 0; off < n; off += 16)
        {
            int p = std::snprintf(line, sizeof(line), "%03zx:", off);
            for (size_t i = 0; i < 16 && off + i < n; i++)
                p += std::snprintf(line + p, sizeof(line) - p, " %02x", b[off + i]);
            spdlog::info("[GEOMDUMP] {} {}", tag, line);
        }
    }
    MoveResult geom_dump()
    {
        MoveResult r;
        void *inst = goblin::collected::first_live_geom_instance();
        if (!inst) { std::snprintf(r.err, sizeof(r.err), "no live geom instance"); return r; }
        r.inst = (uint64_t)inst;
        uint8_t hdr[0x60] = {};
        rpm(inst, hdr, sizeof(hdr));
        uint64_t vt = 0, parts_rec = 0;
        memcpy(&vt, hdr + 0x00, 8);
        memcpy(&parts_rec, hdr + 0x10, 8);  // CSMsbParts record ptr
        r.vtable = vt;
        spdlog::info("[GEOMDUMP] inst={:#x} vt={:#x} partsRec={:#x}", (uint64_t)inst, vt, parts_rec);
        dump_hex("inst", hdr, sizeof(hdr));
        if (parts_rec)
        {
            uint8_t rec[0x200] = {};
            if (rpm((void *)parts_rec, rec, sizeof(rec)))
            {
                uint64_t rvt = 0;
                memcpy(&rvt, rec + 0x00, 8);
                spdlog::info("[GEOMDUMP] partsRec vt={:#x}  @+0x124={:#x} @+0x18b(byte)={:#x}", rvt,
                             *(uint32_t *)(rec + 0x124), rec[0x18b]);
                dump_hex("rec", rec, sizeof(rec));
            }
            else
                spdlog::info("[GEOMDUMP] partsRec unreadable");
        }
        std::snprintf(r.err, sizeof(r.err), "inst=%#llx vt=%#llx partsRec=%#llx (dump -> [GEOMDUMP] log)",
                      (unsigned long long)(uint64_t)inst, (unsigned long long)vt,
                      (unsigned long long)parts_rec);
        r.ok = true;
        return r;
    }

    MoveResult geom_stats()
    {
        MoveResult r;
        uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
        static void *insts[20000];
        size_t cnt = goblin::collected::list_live_geom_instances(insts, 20000);
        r.inst = cnt;
        std::vector<std::pair<uint64_t, int>> hist;  // vtable RVA -> count
        for (size_t i = 0; i < cnt; i++)
        {
            uint64_t vt = 0;
            if (!rpm(insts[i], &vt, 8) || !vt) continue;
            uint64_t rva = vt - base;
            bool seen = false;
            for (auto &e : hist) if (e.first == rva) { e.second++; seen = true; break; }
            if (!seen && hist.size() < 64) hist.push_back({rva, 1});
        }
        std::sort(hist.begin(), hist.end(), [](auto &a, auto &b) { return a.second > b.second; });
        spdlog::info("[GEOMMOVE] STATS total={} distinctClasses={} (move_all cap=20000-list/4096-move)",
                     cnt, hist.size());
        std::string s;
        char b[64];
        for (size_t i = 0; i < hist.size() && i < 6; i++)
        {
            std::snprintf(b, sizeof(b), "vt+%#llx=%d ", (unsigned long long)hist[i].first, hist[i].second);
            s += b;
            spdlog::info("[GEOMMOVE]   class vt+{:#x} count={}", hist[i].first, hist[i].second);
        }
        std::snprintf(r.err, sizeof(r.err), "total=%zu classes=%zu top: %s", cnt, hist.size(), s.c_str());
        r.ok = true;
        return r;
    }

    MoveResult move_all(float dx, float dy, float dz)
    {
        MoveResult r;
        uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
        if (!base) { std::snprintf(r.err, sizeof(r.err), "eldenring.exe base not found"); return r; }
        // Move ALL loaded instances (there are ~17k in an open area — the old 4096 cap left most
        // unmoved, which looked like "the world half-duplicated"; it was just the cap).
        static void *insts[20000];
        size_t cnt = goblin::collected::list_live_geom_instances(insts, 20000);
        if (!cnt) { std::snprintf(r.err, sizeof(r.err), "no live geom instances"); return r; }
        auto getter = (GetterFn)(base + GETTER_RVA);
        int moved = 0;
        for (size_t i = 0; i < cnt; i++)
        {
            void *inst = insts[i];
            float m[16] = {};
            if (!call_getter(getter, inst, m) || !finite3(m)) continue;
            m[12] += dx; m[13] += dy; m[14] += dz;  // uniform per-instance delta (own block-local frame)
            void **vtbl = *(void ***)inst;
            SetterFn setter = (SetterFn)vtbl[SETTER_VSLOT];
            if (call_setter(setter, inst, m)) ++moved;
        }
        r.inst = (uint64_t)moved;  // reuse .inst as the moved count
        r.ok = true;
        spdlog::info("[GEOMMOVE] ALL moved {}/{} instances by ({},{},{})", moved, cnt, dx, dy, dz);
        return r;
    }

    MoveResult restore_held()
    {
        MoveResult r;
        if (!g_held) { std::snprintf(r.err, sizeof(r.err), "nothing held"); return r; }
        uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
        r.inst = (uint64_t)g_held;
        r.before[0] = g_held_before[0]; r.before[1] = g_held_before[1]; r.before[2] = g_held_before[2];
        auto getter = (GetterFn)(base + GETTER_RVA);
        float m[16] = {};
        if (!call_getter(getter, g_held, m) || !finite3(m))
        {
            if (!rpm((char *)g_held + 0x220, m, sizeof(m)) || !finite3(m))
            { std::snprintf(r.err, sizeof(r.err), "held instance unreadable"); return r; }
        }
        m[12] = g_held_before[0]; m[13] = g_held_before[1]; m[14] = g_held_before[2];
        void **vtbl = *(void ***)g_held;
        SetterFn setter = (SetterFn)vtbl[SETTER_VSLOT];
        if (!call_setter(setter, g_held, m)) { std::snprintf(r.err, sizeof(r.err), "setter faulted"); return r; }
        float cache[16] = {};
        rpm((char *)g_held + 0x220, cache, sizeof(cache));
        r.moved[0] = cache[12]; r.moved[1] = cache[13]; r.moved[2] = cache[14];
        r.ok = true;
        return r;
    }
}
