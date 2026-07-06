#include "goblin_coop.hpp"
#include "goblin_inject.hpp"   // goblin::get_world_chr_man_ptr / get_local_player_ptr

#include <cstdint>
#include <windows.h>

//
// Co-op player enumeration (docs/re/coop_player_list_re_prompt.md "SOLVED", 2026-07-06).
// The session player array lives in WorldChrMan's player ChrSet (ChrSet#0 @ WCM+0x10EE0):
//   array ptr = *(WCM + 0x10EF8)        (ChrSet field +0x18 = ChrIns* array)
//   capacity  = *(int*)(WCM + 0x10EF0)  (= 6: local + up to 5 buddies = ER co-op session cap)
//   slot[i]   = ((ChrIns**)array)[i]
// A slot is a real player iff its vtable == the LOCAL player's vtable (both CS::PlayerIns) —
// build-independent, no hardcoded RVA (RTTI filter without an RVA). Solo-validated live: exactly
// slot[0] == LocalPlayer matches; junk slots (e.g. 0x404) and nulls are rejected by the vtable
// check. (WCM+0x10EF8 is the old "LocalPlayerOffset 0x10EF8" long noted DEAD — it was the player
// ARRAY, never the local player.)
//

namespace
{
    constexpr uintptr_t kPlayerArrOff = 0x10EF8; // ChrSet#0 (WCM+0x10EE0) array field +0x18
    constexpr uintptr_t kPlayerCapOff = 0x10EF0; // ChrSet#0 count/capacity field +0x10
    constexpr int kCapClamp = 16;                // sanity clamp (real session cap = 6)

    // POD result filled inside the SEH frame; caller reads it outside.
    struct CoopProbe
    {
        int count;              // PlayerIns entries (local + buddies)
        int others;             // buddy ChrIns* written to `out` (may be < count-1 if `out` full)
        bool ok;
    };

    // Raw derefs live in a noinline body: a __try wrapped directly around raw loads gets ELIDED
    // by clang-cl (docs/memory/tooling/clang-cl-seh-noinline.md) — same shape as
    // goblin_world_position.cpp's probes. `out`/`out_cap` optional (collect buddy ChrIns*).
    __declspec(noinline) void probe_body(uint8_t *wcm, uint8_t *lp, void **out, int out_cap,
                                         CoopProbe *pr)
    {
        void *lp_vt = *reinterpret_cast<void **>(lp);                 // CS::PlayerIns vtable
        auto **arr = *reinterpret_cast<void ***>(wcm + kPlayerArrOff);
        int cap = *reinterpret_cast<int *>(wcm + kPlayerCapOff);
        if (!arr || cap <= 0) return;
        if (cap > kCapClamp) cap = kCapClamp;
        int n = 0, others = 0;
        for (int i = 0; i < cap; ++i)
        {
            void *e = arr[i];
            // Plausible heap pointer only — reject null AND junk (e.g. the live 0x404 seen in an
            // unused slot). Dereferencing 0x404 would fault and abort the whole SEH probe, so
            // pre-filter before reading the vtable.
            auto ev = reinterpret_cast<uintptr_t>(e);
            if (ev < 0x10000 || ev > 0x7FFFFFFFFFFFull) continue;
            if (*reinterpret_cast<void **>(e) != lp_vt) continue;       // not a live PlayerIns
            ++n;
            if (reinterpret_cast<uint8_t *>(e) == lp) continue;        // skip the local player
            if (out && others < out_cap) out[others] = e;
            ++others;
        }
        pr->count = n;
        pr->others = others;
        pr->ok = true;
    }

    void probe_seh(uint8_t *wcm, uint8_t *lp, void **out, int out_cap, CoopProbe *pr)
    {
        pr->count = 0; pr->others = 0; pr->ok = false;
        __try { probe_body(wcm, lp, out, out_cap, pr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { pr->ok = false; }
    }

    bool resolve(uint8_t *&wcm, uint8_t *&lp)
    {
        wcm = reinterpret_cast<uint8_t *>(goblin::get_world_chr_man_ptr());
        lp  = reinterpret_cast<uint8_t *>(goblin::get_local_player_ptr());
        return wcm && lp;
    }
}

int goblin::coop::player_count()
{
    uint8_t *wcm, *lp;
    if (!resolve(wcm, lp)) return 0;
    CoopProbe pr{};
    probe_seh(wcm, lp, nullptr, 0, &pr);
    return pr.ok ? pr.count : 0;
}

bool goblin::coop::others_present()
{
    return goblin::coop::player_count() > 1;
}

std::vector<void *> goblin::coop::other_players()
{
    uint8_t *wcm, *lp;
    if (!resolve(wcm, lp)) return {};
    void *buf[kCapClamp] = {};
    CoopProbe pr{};
    probe_seh(wcm, lp, buf, kCapClamp, &pr);
    if (!pr.ok) return {};
    int n = pr.others < kCapClamp ? pr.others : kCapClamp;
    return std::vector<void *>(buf, buf + n);
}

std::vector<goblin::coop::CoopMarker> goblin::coop::markers()
{
    std::vector<CoopMarker> out;
    for (void *chr : other_players())
    {
        int area = 0, group = 0;
        float wx = 0.f, wz = 0.f;
        if (goblin::get_chr_map_pos(chr, area, wx, wz, &group))
            out.push_back({wx, wz, group});
    }
    return out;
}
