#include "goblin_pause.hpp"

#include "modutils.hpp"
#include "re_signatures.hpp"

#include <spdlog/spdlog.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <mutex>

namespace goblin::pause
{
    // ── Character-update freeze (ER's own cutscene freeze) ───────────────────────────────────────
    // FUN_1405f4d40 = CS::CSEventUtility::SetDisableAllChrUpdate(char). Freezes EVERY ChrIns (player +
    // enemies + NPCs) in pose while render/UI/input keep running; resume is INSTANT even after minutes
    // (a disabled chr isn't updated → no dt accumulates → nothing to catch up). Supersedes the dead
    // FUN_140623410 timescale hook (RET-tested = zero effect live) AND the branch-flip pause (whose resume
    // hitch grew with duration). MUST be CALLED — poking the flag [WorldChrManDbg+0x8] is a no-op; the
    // per-chr propagation happens inside via a ChrFinder. docs/re/game_timestep_freeze_re_findings.md.
    namespace
    {
        using DisableAllChrUpdateFn = void(__fastcall *)(char);  // 1 = freeze, 0 = resume
        DisableAllChrUpdateFn g_disable_chr = nullptr;
        bool g_chr_tried = false;
        bool g_chr_frozen = false;

        DisableAllChrUpdateFn resolve_chr_freeze()
        {
            if (!g_chr_tried)
            {
                g_chr_tried = true;
                g_disable_chr = reinterpret_cast<DisableAllChrUpdateFn>(
                    modutils::scan<void>({.aob = goblin::sig::SET_DISABLE_ALL_CHR_UPDATE}));
                spdlog::info("[CHRFREEZE] SetDisableAllChrUpdate {}",
                             g_disable_chr ? "resolved" : "NOT found — freeze disabled");
            }
            return g_disable_chr;
        }

        // Player-exempt (enemies-only) theory: per-chr effective-disable = [WorldChrManDbg+0x8] XOR
        // [ChrIns+0x531]&1 (FUN_1405ed590). Setting the player's +0x531 bit0=1 before a freeze(1) call
        // makes the player read active (1 XOR 1 = 0) while enemies freeze (1 XOR 0 = 1). LocalPlayer =
        // [[er+0x3d65f88]+0x1e508]. SEH-guarded; best-effort (untested theory — safe if it no-ops).
        void set_player_exempt_bit(bool exempt)
        {
            __try
            {
                auto er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
                if (!er) return;
                auto wcm = *reinterpret_cast<uint8_t **>(er + 0x3d65f88);
                if (!wcm) return;
                auto player = *reinterpret_cast<uint8_t **>(wcm + 0x1e508);
                if (!player) return;
                uint8_t &b = player[0x531];
                b = exempt ? (b | 1) : (b & ~1);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    bool chr_freeze_available() { return resolve_chr_freeze() != nullptr; }
    bool chr_frozen() { return g_chr_frozen; }

    void set_chr_freeze(bool freeze, bool enemies_only)
    {
        auto fn = resolve_chr_freeze();
        if (!fn) return;
        if (freeze && enemies_only) set_player_exempt_bit(true);
        fn(freeze ? 1 : 0);
        if (!freeze) set_player_exempt_bit(false);  // clear the exempt bit on resume
        g_chr_frozen = freeze;
        spdlog::info("[CHRFREEZE] {} ({})", freeze ? "FROZEN" : "resumed",
                     enemies_only ? "enemies-only" : "all chrs");
    }

    // ── Pause API (F1 "Pause game" / pauseOnOpen / `pause` verb / `paused=` status) ───────────────
    // Now backed by the chr-update freeze, NOT the old frame-step branch flip (removed): the branch flip
    // paused the whole sim but its resume hitch grew with duration; chr-freeze is ER's own cutscene freeze
    // (instant resume). The visible difference is theoretical (non-chr world sim keeps ticking under freeze),
    // so the pause toggle uses all-chr freeze. game_timestep_freeze_re_findings.md "SOLVED".
    bool available() { return chr_freeze_available(); }
    bool paused() { return chr_frozen(); }

    namespace { unsigned g_freeze_mask = 0; }

    void request_freeze(unsigned reason, bool on)
    {
        unsigned m = on ? (g_freeze_mask | reason) : (g_freeze_mask & ~reason);
        if (m == g_freeze_mask) return;
        bool was = g_freeze_mask != 0, now = m != 0;
        g_freeze_mask = m;
        if (was != now) set_chr_freeze(now, false);  // apply only on the 0↔nonzero edge
    }

    void set_paused(bool want) { request_freeze(FREEZE_MANUAL, want); }
}
