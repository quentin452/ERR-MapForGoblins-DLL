#pragma once
// DIAGNOSTIC (config diag_map_opens, default off): hook kernel32!CreateFileW and
// log every map .msb.dcx the GAME opens — full resolved OS path (post ME3/UXM
// redirect), open latency, and time since arming. Lets us compare WHERE the game
// actually reads maps from (loader-agnostic ground truth) vs the [LOOTDISK]
// ancestor-walk dir, and WHEN the first map open happens vs the init-time build.
// Throwaway experiment; install() is a no-op unless the flag is on.
//
// BOOT I/O PROFILE (config diag_boot_io, default off): the same observer widened
// to EVERY file open, hooked live immediately (not queued for enable_hooks) so it
// covers the whole boot — including the ~8s regulation/param wait. [BOOTIO] lines
// carry +ms-since-arming + per-open latency; correlate with the init-phase log
// timestamps to see what the startup actually waits on. Read-only.
namespace goblin::worldmap
{
// Idempotent — safe to call from both the early boot-io arm point and the normal
// post-from_params site; the first effective call installs the hook.
void install_map_open_probe();
}
