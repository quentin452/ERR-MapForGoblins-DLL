# FMG slot semantics + the `fmg_set` slot-419 freeze — RE findings

Status: **STATIC RESOLVED** (Ghidra, 2026-07-03). Answers the 4 questions in
`windows_fmg_slot_re_prompt.md`. One item (the exact byte trigger of the 419 freeze) is pinned to a
*structural* cause here and left with a 1-command live confirmation (`fmg_dump 419`) — but the fix does
not depend on that confirmation (see §3 + §5).

Tooling: Ghidra 12.1.2 headless `query.java` against `D:\ghidra_proj2\ER` (`eldenring.exe`,
imagebase `0x140000000`). Cross-referenced with the live repo read in
`windows_native_msg_getter_re_findings.md` (2026-06-30, ERR/Proton) and our own
`src/goblin_messages.cpp` (`patch_fmg_in_memory` / `inject_fmg_entries`).

---

## TL;DR

- **Inject custom item names at the BASE slot (GoodsName=10, WeaponName=11, ProtectorName=12,
  AccessoryName=13, …). It renders on the item** — the game's goods-name UI resolves
  `menu(111) → base(10) → dlc01(319) → dlc02(419)`, and for a *new* id the menu/DLC tiers are empty, so
  base 10 wins.
- **Never inject at a 3xx/4xx (DLC) or 11x (menu) slot.** Under ERR the DLC slots are 1-string stubs and
  the 11x tier is the menu layer; both are wrong-priority AND the DLC-stub layout is what freezes
  `patch_fmg_in_memory`.
- **The 419 freeze is in OUR rebuild, not the game.** The game *never* linearly walks an FMG group; it
  binary-searches and index-computes. `patch_fmg_in_memory` is the only consumer that expands groups and
  copies `[str_data_start, file_size)` — on a DLC-stub header those two assumptions blow up (see §3).
- **Two O(1) guards make any slot safe to *attempt*** (offset/size sanity + span-vs-stringcount), and the
  slot policy above makes the guards moot in practice.

---

## FMG-v2 format — confirmed from the game's own lookup

`FmgFile_lookup` = **`FUN_14266dc90`** (er+0x266dc90), tail-called by `GetMessage`
(`FUN_14266d3c0`, er+0x266d3c0). Decompiled:

```c
longlong FmgFile_lookup(u8 *fmg, u32 msgId) {
    u32 lo = 0, hi = *(u32*)(fmg + 0x0C) - 1;                 // hi = groupCount - 1
    if (msgId <  *(u32*)(fmg + 0x2C)) return 0;               // < group[0].first_id
    if (msgId >  *(u32*)(fmg + (hi+3)*0x10)) return 0;        // > group[hi].last_id
    do {
        u32 mid = (hi + lo) >> 1;
        u8 *g = fmg + mid*0x10;                               // group[mid], stride 0x10
        if (*(u32*)(g + 0x30) < msgId)      lo = mid + 1;     // group.last_id  < msgId
        else if (*(u32*)(g + 0x2C) <= msgId) {               // group.first_id <= msgId  => HIT
            u32 idx = (msgId - *(u32*)(g + 0x2C)) + *(u32*)(g + 0x28); // (msgId-first_id)+string_index
            u64 off = *(u64*)(*(u64*)(fmg + 0x18) + idx*8);   // offsets[idx]
            return off ? (longlong)(fmg + off) : 0;
        }
        else hi = mid - 1;
    } while (lo <= hi);
    return 0;
}
```

Header/group layout (verified — the DLL already matches this):

| off    | field                | note |
|--------|----------------------|------|
| `0x00` | version `0x00020000` | |
| `0x04` | fileSize             | |
| `0x0C` | groupCount           | |
| `0x10` | stringCount          | |
| `0x18` | stringOffsets ptr    | game fixes this up to an **absolute** pointer at load |
| `0x28` | group[0]             | **stride 0x10** = `{ i32 string_index@0, i32 first_id@4, i32 last_id@8, i32 pad@C }` |

> **Doc correction:** the prompt's anchor note said group stride `0xC`. It is **`0x10`** (16 bytes, with
> a 4-byte pad). Our `struct FmgGroup` (`goblin_messages.cpp:124`) is already 16 bytes, so the parse is
> correct — the `0xC` in the prompt was a note error, not a code bug.

**The load-bearing structural fact:** the game does **binary search + arithmetic indexing
`(msgId - first_id) + string_index`** and *never iterates `first_id..last_id`*. Consequently, in a
well-formed FMG **each group is a contiguous run** (ids `first_id..last_id` map 1:1 onto consecutive
string slots), so **`Σ(last_id − first_id + 1) == stringCount`**. Sparse ids are encoded as *more
groups*, not as one wide group. This equality is the invariant we exploit in §5.

---

## Q1 — MsgRepositoryImp layout + slot index space

Confirmed (matches `windows_native_msg_getter_re_findings.md`): `GetMessage(repo, group=0, fmgId, msgId)`
with `repo+0x08 = base_array`, `repo+0x10 = groupCount(==1)`, `repo+0x14 = fmgCount(==512 on ERR)`.
`fmgId` **is the physical slot** — no separate FMG-category enum; the slot number IS the identity.
`base_array[0]` is the single (base-language) sub-array of 512 FMG pointers; `sub[slot]` is the buffer.

Slot identities (from the item-name wrappers in Q2 + the merged item/menu msgbnd numbering):

| category   | menu tier | **base** | dlc01 | dlc02 |
|------------|-----------|----------|-------|-------|
| Goods      | 111 `0x6f`| **10**   | 319   | 419   |
| Weapon     | 115 `0x73`| **11**   | 310   | 410   |
| Protector  | 117 `0x75`| **12**   | 313   | 413   |
| Accessory  | (11x)     | **13**   | 316   | 416   |
| PlaceName  | —         | **19**   | 329   | 429   |

Under ERR the loader **folds DLC strings into the base slot and stubs the vanilla DLC slots to 1 string**
(live read: slot 10 = 8984 strings; 319/410/419 = 1-string stubs). So on ERR the base slot already holds
base+DLC merged content, and the 3xx/4xx slots are degenerate stubs.

---

## Q2 — Item-name resolution path (which slot renders)

The inventory/menu goods-name resolver is **`FUN_140d10680`** (er+0xd10680), one of a family of ~30 tiny
wrappers at er+0xd0fe20 … er+0xd11388, each a fixed fallback chain over `GetMessage`:

```c
// FUN_140d10680  — GoodsName
s = GetMessage(repo,0, 0x6f, id);        // 111  menu/summary tier   (tried FIRST)
if(!s) s = GetMessage(repo,0, 0x0a, id); //  10  BASE GoodsName
if(!s) s = GetMessage(repo,0,0x13f, id); // 319  dlc01
if(!s)     GetMessage(repo,0,0x1a3, id); // 419  dlc02
```

Order is universal (Weapon `115→11→310→410`, Protector `117→12→313→413`, …): **`menu(11x) → base(1x) →
dlc01(31x) → dlc02(41x)`**.

**Consequence for injection:**
- A **new** custom id is absent from the menu(111) and DLC(319/419) FMGs, so the chain falls through to
  **base slot 10 → our injected string renders on the item.** This is why `fmg_set 10 <id>` both reads
  back via `GetMessage(10,id)` *and* shows on the item.
- Two corrections to the existing mod chains (`decode_textid`, `goblin_messages.cpp:44-77`): the game
  order is **menu-first then base**, and it includes a **menu tier (111/115/117…) the mod chains omit**.
  The mod's `{419,319,10}` is reversed (DLC-first) and misses 111. Harmless for *new* ids (111/DLC empty),
  but for read-back parity the decode chain should mirror the game: `{111,10,319,419}` for Goods, etc.
  - **Caveat (live-verify):** if a custom id *reuses* an existing goods id that already has a **111**
    entry, injecting at 10 would be shadowed by 111. New ids in a free range are safe. Whether 111 is
    broadly populated on ERR is a content question — confirm with a live `GetMessage(repo,0,111,<id>)`
    probe if id-reuse is ever needed.

---

## Q3 — Why slot 419 freezes `patch_fmg_in_memory`

**Not the group-expansion loop** (which is why the reverted group-span guard didn't help). The game's
format permits — and the DLL handles — sparse groups fine; and on a 1-string stub the expansion loop runs
at most once. The freeze is in the **string-data copy sizing**, which assumes a *contiguous, header-first*
FMG that a DLC stub violates:

```c
// patch_fmg_in_memory, goblin_messages.cpp:162-176, 234-239
raw = *(u64*)(fmg+0x18);
if (raw > 0x1000000) { off_ptr = (u8*)raw; off_rel = off_ptr - fmg; }   // game fixed-up ABS pointer
else                 { off_rel = raw;       off_ptr = fmg + off_rel; }
...
str_data_start = off_rel + string_cnt*8;                 // assumes offsets array sits right after header
str_data_len   = orig_file_size - str_data_start;        // size_t — UNDERFLOWS if start > fileSize
new_str_data.resize(str_data_len);                       // multi-GB / SIZE_MAX resize
memcpy(new_str_data.data(), fmg + str_data_start, str_data_len);
```

On a **base** slot the fixed-up offsets array lives immediately after the header inside the same buffer,
so `off_rel` is small and `str_data_start < fileSize` — all fine (PlaceName slot 19 rebuilds every boot).
On a **DLC stub** the header is tiny (`fileSize` ~0x30–0x50) but its fixed-up `+0x18` pointer does **not**
sit at `header + groups + offsets` inside the stub — it can point into a shared/relocated region, making
`off_rel = off_ptr - fmg` large (or, if below `fmg`, wrap to a huge `u64`). Then `str_data_start` exceeds
`fileSize`, `str_data_len = fileSize - str_data_start` **underflows to a multi-gigabyte / near-`SIZE_MAX`
value**, and `new_str_data.resize()/memcpy` page-thrash the whole address space on the **present thread**
→ "never returns, no frames, game dead." This matches the symptom (a stall, not an immediate crash) and
explains why a *group-span* guard was irrelevant.

*(Secondary, same family: `parse loop` reads `orig_offsets[si]` through the far `off_ptr`; benign next to
the resize, but also covered by the offset guard below.)*

**Live confirmation (optional, 1 command):** add an `fmg_dump <slot>` RPC that prints `fmg[0x00..0x40]`
for `sub[419]` — expect `fileSize` small, `+0x18` a pointer whose `−fmg` delta is large/negative and a
`stringCount==1`. Not required for the fix.

---

## Q4 / §5 — Correct injectable slot + O(1) hazard rejection

**Injectable base slot per category** (item-name path reads it for new ids, and it is a full contiguous
FMG that rebuilds safely): Goods **10**, Weapon **11**, Protector **12**, Accessory **13**, PlaceName
**19**. Never 3xx/4xx (DLC stubs) or 11x (menu tier).

**O(1) invariants to reject a hazardous slot before the heavy rebuild** (add to `inject_fmg_entries` /
top of `patch_fmg_in_memory`; today's guard only checks `ver==0x00020000 && string_cnt<=200000`, which a
stub passes):

1. **Offset/size sanity (kills the freeze directly):**
   ```
   require 0 < off_rel < orig_file_size
   require off_rel + string_cnt*8   <= orig_file_size          // offsets array fits
   require str_data_start           <= orig_file_size          // no underflow
   ```
   Any failure → refuse the slot (fast error), never resize/memcpy.

2. **Span-vs-stringcount invariant (defends future wide-group cases):**
   ```
   span = Σ over groups (last_id - first_id + 1)
   require span == string_cnt          // well-formed FMG (see format §)
     (or, tolerant: span <= string_cnt + K, small K; reject if span explodes)
   ```
   O(groupCount); a stub with a wide bogus group is caught in O(1).

3. **Slot policy (the real fix):** only accept the base name slots; reject `slot >= 300` and the 11x menu
   tier outright with a clear message. Inject names at slot 10 (Goods) etc., where render resolves.

With (1)+(3), `fmg_set 419` returns an instant error instead of freezing, and custom names inject at the
slot the item-name UI actually renders.

---

## Anchors (er-relative RVAs, imagebase 0x140000000, this ERR build)

- `GetMessage`      = `FUN_14266d3c0` (er+0x266d3c0)
- `FmgFile_lookup`  = `FUN_14266dc90` (er+0x266dc90)  — binary search, the format authority
- Goods-name UI resolver = `FUN_140d10680` (er+0xd10680); family er+0xd0fe20 … er+0xd11388
- DLL side: `patch_fmg_in_memory` `src/goblin_messages.cpp:138`; `inject_fmg_entries` `:544`;
  `struct FmgGroup` `:124`; `decode_textid` chains `:44`.
