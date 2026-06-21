# RE findings — icon manager + container, runtime-corrected

Answers `docs/re/windows_suppress_native_pins_runtime_re_prompt.md`. The static
`CSWorldMapPointMan=[er+0x3D6E9B0]` / map `+0x398` were reported "don't hold at runtime" — this RE
shows the **static is correct**; the `[PINSET]` probe read the wrong field. Static Ghidra
(`find_iconmgr.java/2/3`) on the render-walk + its caller. App 2.6.2.0 / ERR 2.2.9.6.

---

## 0. TL;DR — the static was right; the probe read `_Mysize` at the wrong offset

- The render walk `MOV RDI,[R15+0x398]` (`er+0xa8397e`) is inside the reconcile **`FUN_140a832a0`**.
  Its caller `FUN_140623410` loads the manager at **`0x140624296: MOV RCX,[0x143d6e9b0]`** and calls
  the reconcile — so **R15 = the manager = `[er+0x3D6E9B0]`. CONFIRMED, the static is correct.**
- `[mgr+0x398]` is the std::map's **`_Myhead` (a pointer)**, NOT the size. The `[PINSET]` probe read
  "size" at `+0x398` (so it saw a pointer / 0) and its `{head,size}` scan missed the real pair. The
  MSVC `std::map` `_Tree` sits at **`mgr+0x390`**: `_Myproxy`@`+0x390`, **`_Myhead`@`+0x398`**,
  **`_Mysize`@`+0x3A0`**. Read size at **`+0x3A0`**.

So nothing needs re-finding — re-probe with the right offsets (below). The map being "empty" in the
old probe was a wrong-offset read (and/or the map not built at that instant), not a wrong manager.

---

## 1. Container layout (corrected, from the RB-tree walk)

```
mgr        = [er_base + 0x3D6E9B0]              ; CSWorldMapPointMan (confirmed at the call site)
_Tree      = mgr + 0x390                        ; MSVC std::map<int id, CSWorldMapPointIns*>
_Myhead    = [mgr + 0x398]                      ; sentinel/nil node (NON-null even when empty)
_Mysize    = *(size_t*)(mgr + 0x3A0)            ; element count  ← read THIS, not +0x398
root       = [_Myhead + 0x8]                    ; _Myhead->Parent
node: Left@+0x0  Parent@+0x8  Right@+0x10  isNil/color@+0x19  key(int)@+0x20  value(CSWorldMapPointIns*)@+0x28
```
Walk (in-order or DFS via Left/Right, stop at nodes whose `+0x19` nil-byte is set / == `_Myhead`).

## 2. Re-probe recipe (do this live, map OPEN)
```
mgr  = [er+0x3D6E9B0]
size = *(uint64*)(mgr + 0x3A0)            // expect > 0 with pins on screen
head = [mgr + 0x398]
for each node (DFS from [head+8], skip nil): id = *(int*)(node+0x20); ins = [node+0x28]
    type/source = read CSWorldMapPointIns fields on `ins` (e.g. ins+0x30 = id; probe a type byte)
```
If `size > 0` and ids look like WorldMapPointParam ids → confirmed. Report ids/types to answer the
original §3 (do graces+categories+player-markers+objectives share this map, or are markers/objectives
separate). If `size == 0` while native pins are visible, the icons are a *different* manager — then
fall to §3.

## 3. Sibling manager (lead, if §2 size==0)
Right before the icon reconcile, `FUN_140623410` also does
`0x140624231: MOV RCX,[0x143d6f558]; … CALL FUN_140aba1a0` — a **second manager `[er+0x3D6F558]`**
updated in the same map-tick. If `[er+0x3D6E9B0]+0x398` is genuinely empty when pins show, walk
`[er+0x3D6F558]` (same `+0x390` std::map shape) — likely the player-marker / objective system.

## 4. Suppression (unchanged recommendation)
Still prefer the **show-predicate filter** over clearing the tree: the per-row predicate (vt[1]
`FUN_140a81450`) runs BEFORE insert, and the mod already hides points via `WorldMapPointParam.areaNo
→ 99`. Extend that flip to all category+grace rows → they never enter `mgr+0x398`; fog, player dot,
player-markers, objectives untouched. If clearing is needed instead, call the game's `std::map` clear
(don't hand-free nodes); the manager is now correctly resolved (`[er+0x3D6E9B0]`).

## 5. Offsets / RVAs
- icon manager `[er+0x3D6E9B0]`; load site `er+0x624296` (`MOV RCX,[0x143d6e9b0]`).
- std::map `_Tree`@`mgr+0x390`: `_Myhead`@`+0x398`, **`_Mysize`@`+0x3A0`**; node key`+0x20`/value`+0x28`.
- reconcile `FUN_140a832a0` (render walk `[R15+0x398]`@`er+0xa8397e`); caller `FUN_140623410`
  (also drives player-pos mgr `[er+0x3D69BA8]`).
- sibling manager `[er+0x3D6F558]` via `FUN_140aba1a0` (`er+0x62426f`).
- show-predicate vt[1] `FUN_140a81450`; CSWorldMapPointIns ctor `FUN_140a811e0`.

Scripts: `find_iconmgr.java`, `find_iconmgr2.java`, `find_iconmgr3.java`, `read_pinmap.py`
(re-run live with `_Mysize` at `+0x3A0`).
