# Windows RE findings — the ER game-data SERIALIZE (sidecar Phase-2 strip bracket) — FOUND

Follow-on to `windows_save_function_rpm_re_findings.md` (§Ghidra) and `linux_save_function_re_findings.md`
(§PM#4). Those established that the earlier serialize candidates were misidentified. This pins the **real**
game-data serialize in Ghidra with clean function boundaries. **Variant A is now solved (statically);
the only thing left is a live observer sanity-check.**

## Tool: `find_serialize.java` (committed `tools/ghidra/`)

The offline capstone CC-scan mislabels ER function entries (mid-function `CC` bytes → `<no function>`).
Ghidra has the real boundaries, so the hunt moved there. `find_serialize.java`:
1. resolves the `GameDataMan` static slot from its accessor AOB (`er+0x3d5df38`),
2. collects every function that references it (459 — Ghidra's real call/data model),
3. scores each by how heavily it calls into the `DLIO::DLOutputStream` code region (`0x1edc000..0x1ee1000`)
   — the save serialize both **reads GameDataMan** and **writes a DLOutputStream**; SaveLoad2's write
   session does neither (it copies a pre-serialized buffer), so this intersection is the serialize.

Ranked output put two functions at the top; decompiling them nailed the answer.

## The answer — `FUN_14067dc00` @ RVA `0x67dc00` = the whole-slot save serialize

The strip/reinject **bracket target**. It serializes ONE complete save slot into a caller-supplied
buffer:
- `FUN_141ede5e0(&strm, param_2, param_3)` constructs a **`DLIO::DLOutputStream`** over the out-buffer
  (`param_2`, size `param_3`).
- writes a 0x10-byte header, then a second 0x10 header from GameMan (`DAT_143d69918+0x88`),
- **`FUN_140257f20(&ctx, &strm)`** = the player game-data serialize (see below — reads GameDataMan
  live, incl. the inventory chain),
- then ~11 more section serializers to the same stream (`FUN_1405b5c60`, `FUN_14067e290`,
  `FUN_14067eaa0`, `FUN_14067e9a0`, `FUN_1401cd510`, `FUN_140647420`, `FUN_140643560`, `FUN_140258640/
  6f0/5e0/6a0` …), then a 0x80 trailer,
- finally **seeks to 0 and re-writes the header with the final size** (`FUN_141ede770(&strm,0,0)` +
  rewrite) — an unmistakable **serialize/write** signature (you never rewrite a size header on load).

Properties that make it the ideal bracket:
- **Save-specific:** uses `DLOutputStream` write primitives directly (`FUN_141ede700` = the bounds+write
  primitive). No shared read/write path → **no save-vs-load discrimination needed** (unlike its sub
  `FUN_140257f20`, which is direction-generic).
- **Synchronous, direct-called** (4 callers = the different save triggers: `FUN_14067b750`,
  `FUN_14067b940`, `FUN_14067e150`, `FUN_14067a230`) — NOT vtable-dispatched, so a trampoline hook holds.
- **Entry is before all game-data serialization** (the inventory is read inside `FUN_140257f20`, called
  after entry) and **exit is after the whole buffer is written** → strip@entry / reinject@exit leaves
  the item out of the serialized bytes, atomically, with no autosave window.

**Convention:** `bool FUN_14067dc00(rcx = save_ctx /*+0xb5e..+0xcb2 save-manager fields*/, rdx = out_buffer,
r8d = buf_size, r9 = &out_written_size)`.

**AOB (unique in .text):**
```
40 55 53 56 57 48 8D 6C 24 A8 48 81 EC 58 01 00 00 48 C7 45 A0 FE FF FF FF
```
Added to `re_signatures.hpp` as `SERIALIZE_FN` (replaces the wrong `0x2573c0`).

## Supporting — `FUN_140257f20` @ RVA `0x257f20` = the player-data serialize

Called by `FUN_14067dc00`. Reads `GameDataMan` (`DAT_143d5df38`) 20× and writes each field/sub-object to
the stream via `(*(param_2+0x18))(param_2, buf, len)` (stream **Write** — the sibling `FUN_140258410`
computes a value *then* passes it to `+0x18`, proving write). It delegates to each sub-object's
`serialize(stream)` vtable method `(*(sub+8))(sub, stream)`, incl. `PlayerGameData` (`DAT_143d5df38[1]`)
— whose chain (`+0x2B0` EquipGameData) is the inventory. This is the **direction-generic reflection-style
serializer** (save writes via DLOutputStream, load reads via DLInputStream — the SAME function), which is
the grain of truth behind PM#3's "shared save+load walker" — but PM#3 pointed at the wrong address
(`0x1ede700`, a stream primitive). If ever hooked directly, discriminate by `param_2`'s vtable
(`DLOutputStream` = save). AOB `40 55 56 57 41 56 48 8B EC 48 83 EC 38 48 8B 05 ?? ?? ?? ??` (unique).

## Deliverable status vs. the sidecar Phase-2 goal

1. **Strip/reinject bracket fn + AOB** — ✅ `FUN_14067dc00` `0x67dc00`, unique AOB, `SERIALIZE_FN` updated.
2. **Call convention** — ✅ `rcx=save_ctx, rdx=out_buffer, r8d=size, r9=&out_written`.
3. **Pre-serialize ordering** — ✅ by construction (Ghidra call order: header → player/inventory →
   sections → trailer; entry precedes the inventory read inside `FUN_140257f20`).
4. **Once-per-save / thread** — ⚠ **the one remaining check**: hook `0x67dc00` READ-ONLY (observer) and
   confirm it fires on a real save (grace-rest / quit) and note its thread, before flipping
   `kItemStripReinjectWired`. This is cheap with the shipped in-DLL observer infra on ERR/Proton — and it
   directly closes the PM#2 failure mode (a candidate that never fired). Also re-verify `[SIG]` on the
   ERR deploy build (AOB from Windows App 2.6.x).

Net: the save serialize is **found and statically airtight** (GameData→DLOutputStream, save-specific,
synchronous, brackets the inventory). Variant A (clean vanilla save) is unblocked; wire
strip(entry)/reinject(exit) on `SERIALIZE_FN` + the observer confirmation → done. Variant B remains the
zero-RE fallback.
