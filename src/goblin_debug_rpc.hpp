#pragma once

// Phase 3 of docs/plans/overlay_hot_reload_playwright_plan.md: dev-only debug RPC.
//
// A TCP loopback listener (127.0.0.1:<debug_rpc_port>, ini [Debug], empty = disabled — the
// existing config-gate pattern) lets an external driver (tools/mfg_rpc.py) script the REAL running
// game: screenshot the framebuffer, flip the F1 panel, set config values, trigger an overlay
// hot reload (split build) — the observe half of the AI iterate loop (Phase 4).
//
// TCP loopback, NOT a named pipe, on purpose: the live game runs under Proton on the Linux box,
// and Wine's loopback IS the host's loopback — a Python driver on Linux reaches 127.0.0.1 into the
// Wine process directly, which a Windows named pipe can't do. Works identically on real Windows.
//
// Threading: the listener thread only parses lines and queues them; every command EXECUTES on the
// present thread (pump(), called from hk_present after the overlay draw so screenshots include it),
// then the listener is woken to send the one-line reply. Commands time out (10s) if no frames are
// presenting. One client at a time; line protocol, replies start "ok" or "err".
//
// Commands: ping | status | open_f1 <0|1|toggle> | set <ini_key> <value> | screenshot <path> |
// reload_overlay | pause <0|1|toggle> (goblin_pause frame-step flip; also `paused=` in status —
// lets a driver clear a pause before scripting; rendering/RPC keep running while paused). Input injection (listener-thread, SendInput — drives menus/save-load/map hover
// for the Phase 4 loop): key <name> [hold_ms] | mouse_move <x> <y> | mouse_click [left|right]
// [<x> <y>] — coordinates in game-window CLIENT pixels (same space as screenshots).

#include <string>
#include <vector>

struct IDXGISwapChain3;

namespace goblin::debug_rpc
{
    // Start the listener thread if debug_rpc_port is set (call after load_config). No-op when the
    // port is empty/invalid. Never fails the mod — a bind error just logs and disables the RPC.
    void initialize();

    // Drain queued commands on the present thread. Called once per frame from hk_present, after
    // the overlay's own draw is submitted (so `screenshot` captures game + overlay). Cheap no-op
    // (one relaxed atomic read) when the RPC is disabled or idle.
    void pump(IDXGISwapChain3 *swapchain);

    // Live feed for the on-screen HUD (bottom-right corner): the last few commands + their
    // results, newest last; entries fade out (dropped after ~6s). Empty when the RPC is
    // disabled or idle — the HUD then draws nothing.
    std::vector<std::string> recent_commands();
}
