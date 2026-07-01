#pragma once

namespace goblin
{
// Dev RE probe (gated on config::debugLogging): scan EVERY loaded param table for a
// small set of hardcoded needle u32 values and log "[PARAMSCAN] table row +off = val"
// per hit. In-process replacement for a Cheat-Engine "search params for this id"
// session — works on Linux/Proton where the Windows RE tooling isn't available
// (docs/memory/tooling/linux-runtime-re-options.md, path 1). Current needles: the
// Group-2 ObjAct anchors (Smithing Table ABP 6250, Elevator ABP 5010) — edit
// kNeedles in the .cpp for the next hunt.
void param_needle_scan();
} // namespace goblin
