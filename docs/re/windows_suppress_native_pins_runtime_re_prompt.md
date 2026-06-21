# RE follow-up — find the REAL world-map icon manager + container at RUNTIME

The static answer (`windows_suppress_native_pins_re_findings.md`) gave
`CSWorldMapPointMan = [er_base+0x3D6E9B0]`, built-icon set = `std::map<int, CSWorldMapPointIns*>`
at `+0x398`. **Those offsets DO NOT hold at runtime** — we verified in-process. We need the real
manager + container resolved the way the player-pos was: a runtime pointer-scan / find-what-accesses,
not a static guess. App 2.6.2.0 / ERR 2.2.9.6, ERSC + ERR, our DLL is in-process.

## What our in-game `[PINSET]` probe found (decisive)

We resolve `er_base = GetModuleHandle("eldenring.exe")` (ASLR-correct; other readers use it fine) and,
while the world map is OPEN, walked the candidate manager:
- `mgr0 = [er_base + 0x3D6E9B0]` = a **heap** pointer (e.g. `0xfdb25800`, `0x188287400` across runs —
  varies, so it IS a live object) — BUT scanning `mgr0+0x300 .. +0x600` for any `{non-null head,
  size 1..20000}` std::map-shaped pair found **ZERO candidates**, and `[mgr0+0x398]` size read **0**.
- `mgr1 = [er_base + 0x3D6E9D8]` = `0x143d6e9f0` = **`er_base + 0x3d6e9f0`** = a pointer back into the
  module's static data (a self-reference near the slot), i.e. **not** an object instance. So
  `0x3D6E9D8` is not the getter-result slot we want.

Conclusion: either `0x3D6E9B0` is the wrong static (mgr0 is some other live object with no icon map in
that window), or the icon container is reached by a different deref / offset than `+0x398`. Native
pins ARE visible on the open map, so the container is populated — somewhere we haven't found.

## What we need (runtime, not static)

1. **The authoritative access is the render walk** `[R15 + 0x398]` at `eldenring.exe+0xa8397e`
   (`marker_affine_hook_re_findings.md`), where R15 = the icon manager. **Break there at runtime**
   (x64dbg / CE) with the world map open and report:
   - the concrete value of **R15** (the manager instance address),
   - **R15 − er_base**? It won't be static (heap), so instead: how is R15 *obtained* a few
     instructions earlier — which static slot / getter call / deref chain produces it? Give the
     `module+RVA` of that static and the deref chain (so we can resolve it in-process like
     `[[er+X]+Y]…`).
   - the std::map at `[R15+0x398]`: confirm `_Myhead`/`_Mysize` layout + the node key(+0x20)/value
     (+0x28); read `_Mysize` live (should be > 0 with pins on screen).
2. **OR pointer-scan from a visible pin.** Pick a known on-screen native pin (e.g. a specific grace),
   find its `CSWorldMapPointIns` (CE "find what accesses" the icon while hovering / its id), then
   pointer-scan back to a stable `eldenring.exe+RVA → offsets` path that reaches the manager + its
   `+0x398` map. Give that path.
3. **Confirm what's IN the map at runtime** (the original §5 question we couldn't answer because our
   offsets were wrong): walk it and report the ids/types — do graces, category pins, player-placed
   markers, and objective beacons all share it, or are markers/objectives a separate manager? This
   decides clear-all vs selective filter.

## Goal / how we'll use it

Same as before: once we can reach the real `+0x398` map (or the per-row show-predicate
`FUN_140a81450`), we suppress the native category+grace pins (overlay draws them) while keeping the
player dot, fog, player-markers, objectives. Implementation will be in our DLL; we just need the
runtime-stable resolution path (static base + deref offsets, or an AOB on the render walk / predicate).

## Known-good reference (for resolution style)
The player position was solved exactly this way and works: `WorldChrMan = [er_base+0x3D65F88]`,
`LocalPlayer = [WCM+0x1E508]`, pos `+0x6C0`. We want the icon manager expressed in the same
`[er_base+RVA] + offsets` form, runtime-verified.
