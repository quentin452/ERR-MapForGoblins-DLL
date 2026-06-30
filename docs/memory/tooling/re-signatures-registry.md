---
name: re-signatures-registry
description: All AOB signatures + image RVAs centralized in src/re_signatures.hpp with a startup PASS/FAIL health check — first place to look when an ER update breaks resolution
metadata: 
  node_type: memory
  type: reference
---

All ~18 AOB signatures and the 3 genuine image RVAs the MapForGoblins DLL resolves at
runtime live in **`src/re_signatures.hpp`** (`namespace goblin::sig`), each a named constant
with a re-find hint. Centralized 2026-06-20 (commit b80bd7e); call sites across
markers/kindling/debug_events/collected/messages/inject/params/probe reference
`goblin::sig::NAME` instead of inline byte strings (dedups IsEventFlag×3, EventFlagMan
slot×3, WorldGeomMan×2, CSMenuMan×2 — one fix per game update, not N).

**After an ELDEN RING / ERR update breaks something:** launch, then grep the DLL log for
`[SIG]` — `resolve_all_signatures()` (run in `setup_mod`) logs `PASS <name> -> <addr>` or
`FAIL <name>` for every signature. A FAIL names exactly which AOB to re-find; fix it in the
one header. (Struct field offsets like `dialog+0xA88`, `point+0x80` are NOT centralized —
they're inline at their use sites; low churn, tightly coupled.)

Key flag signatures (the memory get/write path): `IS_EVENT_FLAG` (reader, unique),
`EVENT_FLAG_MAN_SLOT` (singleton, +{{3,7}}), `SET_EVENT_FLAG` (writer = EventFlag_C1).

**SET_EVENT_FLAG had a sibling-collision bug (fixed, commit bf75aef) — the health check
caught it on day 1.** A sibling fn shares the whole prologue + `8B 12 48 8B F1 85 D2`, so the
22-byte prologue AOB matched 2 fns and Pattern16 picked either across sessions (0x1405d2110
decoy vs 0x1405d2240 real → writer would hit the wrong fn ~half the time). They diverge right
after: REAL has short `74` (jz), DECOY has `0F 84` (jz near). The fixed AOB ends at `…85 D2 74`
= unique, pins the real entry 0x1405d2240. The full Hexinton AOB (`…85 D2 0F 84`) actually
described the DECOY. Lesson: a non-unique AOB is a latent wrong-function bug; the [SIG] log
shows address drift across sessions when one exists. See [[mapforgoblins-map-open-freeze]],
[[overlay-rendered-markers]].

**`GETMESSAGE` — WIRED + VERIFIED LIVE on ERR** (2026-06-30, see
`docs/re/windows_native_msg_getter_re_findings.md`, landed `a64f4e1`): the native loot-name getter
`wchar_t* GetMessage(repo, group, fmgId, msgId)` @ RVA `0x266d3c0`. `lookup_text` resolves names through
it on demand; log confirms `[SIG] PASS GETMESSAGE` (unique) + `GetMessage resolved at …d3c0` (= match−5)
and user-confirmed labels render. **Caveat — anchor the AOB on the interior, not the prologue:** ERR
hooks this function (live entry = MinHook `E9` trampoline), so the prologue bytes are clobbered at
runtime. Interior AOB `44 3B 41 14 73 ?? 48 8B 41 08 8B D2 48 8B 0C D0 48 85 C9 74 ?? 41 8B C0 48 8B
0C C1 48 85 C9 74 ?? 41 8B D1 E9`, **entry = match − 5**. This is a new general rule for the registry:
hookable engine functions need interior anchors. See [[rpm-live-memory-tooling]], [[ghidra-re-tooling]].
