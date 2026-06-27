#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Embedded "find out what accesses this address", filtered to the GAME's code.
//
// WHY: the offset source-of-truth refactor reads a param field offset live from the game's own
// access instruction (modutils::resolve_field_offset). Authoring that AOB needs the read SITE.
// Cheat Engine's find-what-accesses works (it cracked goodsType) but is drowned when several mods
// read the same field (sortGroupId: 6 param-mods loaded) and static scans are too noisy (AEG: 6507
// lookalike [reg+0xb8] reads). This probe sets a HARDWARE breakpoint (DR0) on a LIVE param row+
// offset and, in a vectored handler, logs ONLY the accessing instruction whose RIP is inside
// eldenring.exe — every mod read (MapForGoblins/reforged/…) is skipped automatically. One log line
// ([FWA]) gives the RIP + a byte window; feed it to D:\ghidra_scripts\offset_resolver.py `author`.
//
// Dev-only, off by default. EAC must be bypassed (offline/ERSC) — it writes debug registers.
namespace goblin::field_probe
{
    // spec = "ParamName:rowId:offset[:len[:rw]]"  (len 1|2|4|8 default 4; rw r|w|rw default r).
    //   e.g. "AssetEnvironmentGeometryParam:99821:0xb8:4:r"  or  "EquipParamGoods:1000:0x72:1:r".
    // Resolves the live row via get_param, arms DR0 on every current thread, installs the VEH.
    // Logs the resolved address + arms; the [FWA] hit line appears once the game reads the field.
    //
    // RAW-ADDRESS mode — spec = "@geom[:offset[:len[:rw]]]" (defaults +0x263, len 1, read):
    //   the watched address isn't a param row but a LIVE heap object the static INI can't name
    //   (ASLR): a CSWorldGeomIns alive byte, for the "find the native collected getter" RE. The
    //   probe DEFERS arming; a geom subsystem (goblin_collected's WGM walk) polls geom_arm_pending()
    //   and calls arm_raw() with the first tracked alive instance's address. Then approach/reload
    //   that asset in-game and the [FWA] hit RIP is the game's own collected/alive read site.
    void initialize(const std::string &spec);

    // Arm DR0 on a RAW absolute address resolved at runtime. Reusable by any subsystem holding a
    // live address the INI can't express. No-op (returns false) if already armed or bounds fail.
    // `label` names the target in the [FWA] log.
    bool arm_raw(std::uintptr_t addr, int len, bool write_only, const char *label);

    // True while initialize() parsed an "@geom" spec but no live address has been fed yet. Out-params
    // carry the requested offset/len/rw to add to a resolved CSWorldGeomIns*. The caller resolves a
    // live instance and calls arm_raw(instance + off, len, write_only, ...). One-shot (arm clears it).
    bool geom_arm_pending(std::ptrdiff_t &off, int &len, bool &write_only);
}
