#pragma once
// DIAGNOSTIC (config diag_map_opens, default off): hook kernel32!CreateFileW and
// log every map .msb.dcx the GAME opens — full resolved OS path (post ME3/UXM
// redirect), open latency, and time since arming. Lets us compare WHERE the game
// actually reads maps from (loader-agnostic ground truth) vs the [LOOTDISK]
// ancestor-walk dir, and WHEN the first map open happens vs the init-time build.
// Throwaway experiment; install() is a no-op unless the flag is on.
namespace goblin::worldmap
{
void install_map_open_probe();
}
