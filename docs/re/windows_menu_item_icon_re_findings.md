# RE findings — how ER resolves an ITEM to its menu icon (inventory/equipment)

Answers `docs/re/windows_menu_item_icon_re_prompt.md`. Static Ghidra RE (`D:\ghidra_proj2\ER`,
scripts `re_v78..v80`) + a paramdef offset recompute (`D:\ghidra_scripts\paramoff.py`).
App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Read-only.

---

## 0. TL;DR

The probe's zero matches were an **offset bug, not a wrong id space.** The hand-summed iconId
offsets were each ~3 bytes too high (bitfield packing). With the corrected offsets, item →
`EquipParam.iconId` is direct, and `MENU_FL_<digits>` is the gfx's per-`iconId` atlas image (ERR
naming) — so re-running the verify should match `40147` to a real item row.

- **Ask #1 — authoritative iconId offsets** (from the paramdef, correct packing — the probe's were
  all `+3` too high, Gem `+1`):

  | param | field | **offset** | probe used (wrong) | type |
  |---|---|---|---|---|
  | EquipParamWeapon | `iconId` | **`0xBF`** | 0xc2 | u16 |
  | EquipParamProtector | `iconIdM` | **`0xA7`** | 0xaa | u16 |
  | EquipParamProtector | `iconIdF` | **`0xA9`** | — | u16 |
  | EquipParamAccessory | `iconId` | **`0x27`** | 0x2a | u16 |
  | EquipParamGoods | `iconId` | **`0x31`** | 0x34 | u16 |
  | EquipParamGem | `iconId` | **`0x05`** | 0x6 | u16 |
  | MagicParam | `iconId` | `0x15` | — | s16 |

- **Ask #2 — format site**: `FUN_14073d9d0` (`0x73d9d0`) picks `format = (&PTR_MENU_ItemIcon_%05d)
  [iconRef.type]` from the array at **`0x3b34c28`** and `vswprintf`s it (`FUN_14013a1f0` `0x13a1f0`
  → `FUN_14013a220`) with the icon-ref id. Its one caller / icon-symbol resolver is
  **`FUN_14074bcc0`** (`0x74bcc0`). `iconRef = { u8 type @+0 ; s32 id @+4 }`. §2.
- **Ask #3 — `MENU_FL` id space**: `MENU_FL_*` is a **separate** family from `MENU_ItemIcon_%05d`.
  `MENU_FL_<digits>` = the gfx's per-`iconId` atlas image name (ERR's high-range iconIds, e.g.
  40147); `MENU_FL_Affinity_*` / `MENU_FL_Evaluation_*` = **named CB_imgTag glyphs** embedded in FMG
  text via `<img src='img://…'>` (resolver `FUN_140d1a7c0`), NOT per-item icons. §3.
- **Ask #4 — iconId→rect**: **GFx-only.** No C++/param rect table; the sub-rect is the gfx
  sprite-sheet layout, surfaced per-draw on the `CSTextureImage` (the chain you already solved).
  Capture it at draw, or FFDEC. §4.

---

## 1. The icon symbol pipeline (deliverable #2)

```
inventory item  ──(item type → EquipParam row)──►  iconId (u16, §0 table)
   │
   ▼  build an icon-ref { u8 type ; s32 id@+4 }   (type: 0=ItemIcon, 1=PropertyIcon, 2=StatusIcon)
FUN_14074bcc0 (0x74bcc0)  — the icon-symbol resolver (1331B; the icon-ref's caller)
   │
   ▼  FUN_14073d9d0 (0x73d9d0):  fmt = (&PTR_MENU_ItemIcon_%05d_143b34c28)[ iconRef.type ];
   ▼  FUN_14013a1f0/220 (vswprintf):  L"MENU_ItemIcon_%05d" % id   →  e.g. "MENU_ItemIcon_40147"
   │
   ▼  + "_ptl" suffix (FUN_140d64490 formats L"%s_ptl")  →  the GFx image symbol
   ▼  gfx resolves the symbol → its sprite → imported atlas image (ERR: "MENU_FL_<id>")
   ▼  CS::CSScaleformImageCreator::CreateImage → CS::CSTextureImage (rect +0x74/+0x78/+0x7c/+0x80)
```

The **format-string array base = `0x3b34c28`** (`MENU_ItemIcon_%05d` @ idx0, `MENU_PropertyIcon_%05d`
@ idx1, `MENU_StatusIcon_%05d` @ idx2; neighbours at `0x3b34c08..0x3b34c20` = `MENU_Load_%05d` /
`MENU_DummyTutorial` / `MENU_Tuto_%05d`). The icon **type** picks the format; the **id** is the
`%05d`. For a normal inventory item the type is `ItemIcon` and the id is the item's `EquipParam.iconId`.

**Worked example:** weapon row → `iconId = [row + 0xBF]` (u16) → icon-ref `{type 0, id=iconId}` →
`MENU_ItemIcon_%05d` → `"MENU_ItemIcon_<iconId>"` → gfx image `MENU_FL_<iconId>` → `CSTextureImage`
with the atlas sub-rect. (The full item→icon-ref hop lives in the menu item-data layer inside
`FUN_14074bcc0`; the id it sprintf's is the item's iconId.)

## 2. Why the probe read garbage — the offset fix (deliverable #1)

ER paramdefs are byte-packed with bitfields packing per underlying type; the hand-sum mis-counted a
bitfield/padding run and landed `+3` high on all four equip params (`+1` on Gem). Recomputed straight
from `tools/paramdefs/*.xml` with correct bitfield packing (`paramoff.py`) → the §0 table. The probe
read `iconId+3`, i.e. into the next field → Weapon happened to look 0–999 (it overlapped a small
field), Protector/Accessory/Goods read `0`/`65535`, Gem read garbage. **Fix `verify_equip_iconids`
to the §0 offsets and re-run** — expect `{40144,40147,40172}` to match real ERR item rows.

## 3. `MENU_FL` vs `MENU_ItemIcon` (deliverable #3)

Two distinct systems share the `MENU_*` prefix:
- **`MENU_ItemIcon_%05d` / `MENU_PropertyIcon` / `MENU_StatusIcon`** — the **numeric, per-item icon**
  formats (array @ `0x3b34c28`), keyed by `iconId`. This is the inventory/equipment item icon.
- **`MENU_FL_*`** — the **CB_imgTag** family: image tags embedded in localized menu TEXT via
  `<img src='img://%s' …>` (formats at `0x2a95414`/`0x2a9ab14`). Resolver `FUN_140d1a7c0` (`0xd1a7c0`)
  maps a tag id (`<0x230`, stride-`0x28` table, symbol name @entry`+0x10`) to a `MENU_FL_*` symbol —
  e.g. `MENU_FL_Affinity_Frenzied`, `MENU_FL_Evaluation_1..5` (rune/affinity/evaluation glyphs). These
  are **not** per-item icons.

The live-captured `MENU_FL_<NNNNN>` (40147 …) for inventory items is the **gfx atlas image name**:
ERR's `.gfx` names each per-`iconId` sprite's imported image `MENU_FL_<iconId>` (high iconId range).
So `40147` **is an item iconId** (ERR's range), reached via the `MENU_ItemIcon_%05d` request that the
gfx maps onto its `MENU_FL_<id>` atlas cell. (Confirm by the §2 re-verify: 40147 should hit an
EquipParam row at the corrected offset.) `MENU_FL_<digits>` = iconId-keyed; `MENU_FL_<name>` =
named img-tag glyph.

## 4. iconId → rect (deliverable #4) — GFx-only

There is **no C++/param table** mapping iconId → atlas sub-rect. The format pipeline (§1) produces a
symbol *name*; the name→sprite→sub-rect resolution is entirely inside the GFx movie (the SWF
sprite-sheet layout — the FFDEC-readable part). At runtime the only place the rect appears is on the
per-draw `CS::CSTextureImage` (`+0x74/+0x78/+0x7c/+0x80`, sheet `+0x84/+0x88`), already captured by the
`CreateImage` hook (`windows_runtime_icon_textures_followup_re_findings.md`). So: **iconId→rect = capture
at draw** (the inventory draws each visible item's icon → CreateImage fires with name `MENU_FL_<id>` +
rect), or parse the `.gfx` with FFDEC. Unlike the worldmap, the inventory **does** draw all categories'
items, so every needed iconId's rect is capturable by opening the menu and scrolling.

## 5. Plan to draw the 6 categories from the live inventory atlas

1. Map each MFG item → `iconId` via the §0 EquipParam offsets (live `get_param`).
2. The CreateImage hook already logs `MENU_FL_<iconId>` + rect + the 2048×2048 atlas `CSTextureImage`;
   on a menu-open frame read its resource via the solved chain (`img+0x10 → +0x70 → ID3D12Resource`).
3. `CopyTextureRegion` the rect into our SRV (or one-shot readback→PNG→bake), keyed by iconId.
   Scroll the inventory once to draw + capture every needed category's icons.

## 6. Handles

- format getter `FUN_14073d9d0` `0x73d9d0`; resolver `FUN_14074bcc0` `0x74bcc0`; vswprintf
  `FUN_14013a1f0` `0x13a1f0` → `FUN_14013a220`. format array base **`0x3b34c28`** (ItemIcon/Property/
  Status @ +0/+8/+0x10). icon-ref `{u8 type@+0, s32 id@+4}`.
- CB_imgTag resolver `FUN_140d1a7c0` `0xd1a7c0`; tag table stride `0x28`, symbol @entry`+0x10`;
  `MENU_FL_*` strings `~0x2bae718`/`0x2bb…`; `img://%s` formats `0x2a95414`/`0x2a9ab14`.
- iconId offsets §0 (paramdef-derived; resolve final via the corrected `verify_equip_iconids`).
- iconId→rect: GFx-only → the CreateImage/CSTextureImage capture (followup findings) + atlas resource
  chain. Offsets version-specific; the paramdef offsets track the regulation, not the exe build.
```
