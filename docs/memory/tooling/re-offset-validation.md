---
name: re-offset-validation
description: "How to pin a param/struct BYTE offset reliably before trusting it — empirical positive+negative cross-check, never hand-derived. Lesson from the isEnableRepick 0x3c bit5-vs-bit6 16k leak."
metadata: 
  node_type: memory
  type: feedback
---

**Never ship a field byte/bit offset derived by hand-computing the paramdef/struct packing.** Bit-packing is
subtle (bitfields, `dummy8` reserved bits that take NO slot, alignment) — a one-bit error reads an ADJACENT
field and looks plausible. This cost a 16k-marker leak: `isEnableRepick` was computed at byte 0x3c **bit 6**
but is really **bit 5**; bit 6 is `isBreakOnPickUp` (set on ~every breakable pickup) → the gather filter
matched ~18k pots/jars instead of ~1.4k gather nodes. (commits 102f4a0 / aeg_is_gather.)

**Validate EVERY offset before using it — the 4 checks:**
1. **Empirical raw bytes, not a derivation.** Read the actual bytes (regulation file via SoulsFormats, or live
   RPM) — don't trust a script that re-implements FromSoft packing. The byte layout is identical in the file
   and in live memory (the game loads rows verbatim), so a regulation read is valid for an RPM offset.
2. **Anchor on a KNOWN-GOOD offset.** Locate the row in the raw bytes via a field whose offset is already
   proven (e.g. `pickUpItemLotParamId @ +0xb8`, confirmed because `aeg_pickup_lot` returns correct lots), then
   read the candidate offset relative to that. Don't read a blind absolute offset.
3. **POSITIVE *and* NEGATIVE sample — mandatory.** A field=1 row ALONE can't disambiguate (many bits are 1).
   You need a row where the field is 0 to prove which bit flips. Cross-check the raw byte/bit against the
   library's PARSED cell value (SoulsFormats applies the paramdef correctly = ground truth) over both. The bit
   that is 1 for all field=1 rows AND 0 for all field=0 rows is the answer. (Recipe: tools/_probe_repick_bit.py
   pattern — search the binary for known field values, dump the candidate byte, diff gather vs pot rows.)
4. **Sanity-check the runtime OUTPUT.** After wiring it, verify the produced count/behaviour matches an
   independent expectation (here: disk gather should ≈ the bake's ~1455, not 6×=16800). A gross mismatch is a
   red flag that the offset (or logic) is wrong — investigate before declaring done.

**Shortcut when SoulsFormats is available:** its parsed cell value already IS the correct offset's value —
prefer reading the named cell over hand-deriving the byte position; only drop to raw offsets for the live C++
(RPM/`get_param<Raw…>`), and even then pin the offset via checks 1–3, not a computation. See
[[ghidra-re-tooling]], [[rpm-live-memory-tooling]].
