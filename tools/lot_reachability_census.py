#!/usr/bin/env python3
"""LOT REACHABILITY CENSUS — the "am I missing a pass?" checkup, independent of the bake.

`baked=0` only proves every marker we DRAW comes from disk/live, NOT that we draw every real item:
a lot missed by BOTH the bake and the disk passes is invisible to a baked-residual scan (the bake
missed it too — exactly the 3 Siofra goods 2009:00 recovered). So the oracle must be rebuilt from the
RAW source data, independent of any existing pass.

This tool enumerates the FULL notable-lot universe from the regulation and, for each lot, tests every
known placement mechanism in the real ERR files (MSB Treasure / Asset pickup / NpcParam-placed /
EMEVD any-bank / sequence-sibling). Two outputs:

  1. AWARD-INSTRUCTION CATALOG (the missing-pass detector, format-independent): every EMEVD
     (bank, instr-id[, template]) that ever carries an ItemLotParam id, with a count and a parsed=Y/N
     flag (does the runtime parser handle that instruction class?). Any UNPARSED row that carries
     notable lots is a candidate missing pass — this is how 2009:00 was found (scan ALL banks, not the
     bank-2000 template whitelist).
  2. PER-LOT REACHABILITY: every notable lot bucketed reached / unreached, and for each unreached one a
     NAMED blocker (missing-pass / emevd-no-anchor / npc-not-placed / shop / orphan / unreferenced).
     The goal is not "unreached = 0" (some lots are legitimately shop-only / quest-give / cut) but
     "every unreached lot has a named blocker, and none of them is 'missing-pass'."

Writes docs/lot_reachability.md (versioned baseline — git-diff it after a change to catch coverage
regressions, the mirror of docs/nobake_scoreboard.md). Run (Windows, see mapforgoblins-pipeline-setup):
  PYTHONUTF8=1 PYTHONPATH="C:/Users/iamacat/AppData/Local/Temp/mfg_aux" MFG_PROFILE=err \
      py -3.14 tools/lot_reachability_census.py
"""
import json
import os
import struct
import tempfile
from collections import Counter, defaultdict
from pathlib import Path

import extract_all_items as E
import config
from System import Array, Object
from System.IO import File as SysFile

# ── EMEDF (DarkScript3 instruction defs) — the precise award/reference discriminator ──────────────
# A lot id appearing in an instruction arg is only a real item placement when that arg is TYPED as an
# Item Lot (arg name contains "item lot"). Without this, coincidental numeric matches dominate — a
# ladder's disable-flag (2009:00 "Register Ladder") or any flag/entity that happens to equal a lot
# number. (That coincidence is exactly what a bad "2009 asset-lot pass" once mistook for a placement.)
EMEDF = json.load(open(Path(__file__).parent / "er-common.emedf.json", encoding="utf-8"))
INSTR = {}  # (bank, id) -> {"name", "lot_idxs": [arg indices typed Item Lot], "positions": bool}
for _cls in EMEDF["main_classes"]:
    for _ins in _cls["instrs"]:
        names = [a["name"].lower() for a in _ins["args"]]
        lot_idxs = [i for i, nm in enumerate(names) if "item lot" in nm]
        positions = any(("entity" in nm or "region" in nm or "point" in nm or "placement" in nm)
                        for nm in names)
        INSTR[(int(_cls["index"]), int(_ins["index"]))] = {
            "name": _ins["name"], "lot_idxs": lot_idxs, "positions": positions}

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "lot_reachability.md"
ERR = Path(config.require_err_mod_dir())
MSB_DIR = ERR / "map" / "MapStudio"
EVENT_DIR = ERR / "event"

# ── EMEVD coverage the RUNTIME parser implements (keep in lock-step with msbe_parser.cpp) ──────────
# bank-2000 RunCommonEvent templates the parser awards from (kEmevdTemplates) + the structural rules.
PARSED_TEMPLATES = {90005300, 90005301, 90005860, 90005861, 90005880, 90005750, 90005753,
                    90005774, 90005792, 90005632, 90005110, 90005390, 90005555, 90005500, 90005501}
PARSED_BOSS = {90005860, 90005861, 90005880}


def template_parsed(eventId):
    """Does the runtime emevd pass award from a bank-2000 init invoking `eventId`?"""
    if eventId in PARSED_TEMPLATES:
        return True
    if eventId >= 1_000_000_000:       # per-tile enemy-death award (eventId >= 1e9)
        return True
    if eventId == 1200:                # event-1200 boss-drop mechanism (B)
        return True
    return False


# Non-bank-2000 instruction classes the parser handles: (bank, id) -> label.
PARSED_DIRECT_INSTR = {(2009, 0): "asset-lot place (2009:00)"}

print("=== loading regulation ===")
reg = E.SoulsFormats.SFUtil.DecryptERRegulation(str(ERR / "regulation.bin"))
pdefs = E.load_paramdefs()

LOTF = {"getItemFlagId"}
for i in range(1, 9):
    LOTF.update([f"lotItemId0{i}", f"lotItemCategory0{i}"])
ilm = E.param_to_dict(E.read_param(reg, "ItemLotParam_map", pdefs), LOTF)
ile = E.param_to_dict(E.read_param(reg, "ItemLotParam_enemy", pdefs), LOTF)
ALL_LOTS = set(ilm) | set(ile)   # every ItemLotParam row id (for the instruction-arg catalog)


def slot1(r):
    for s in range(1, 9):
        iid, cat = r.get(f"lotItemId0{s}", 0), r.get(f"lotItemCategory0{s}", 0)
        if iid > 0 and cat > 0:
            return iid, cat
    return 0, 0


def is_notable(r):
    """A lot that SHOULD be a map marker: a real slot-1 item + a one-time obtain flag. (Flag!=0 is the
    offline proxy for resolve_loot_flag!=0; some flagged-repeatable enemy drops slip in but they are
    almost all reached as placed enemies, so they don't pollute the unreached signal.)"""
    iid, cat = slot1(r)
    return iid > 0 and cat > 0 and r.get("getItemFlagId", 0) != 0


# Notable universe, per table (lt 1 = map, 2 = enemy).
notable = {}   # lot -> (lt, itemId, cat, flag)
for lt, tbl in ((1, ilm), (2, ile)):
    for lot, r in tbl.items():
        if is_notable(r):
            iid, cat = slot1(r)
            notable.setdefault(lot, (lt, iid, cat, r.get("getItemFlagId", 0)))
print(f"  notable universe: {len(notable)} lots ({sum(1 for v in notable.values() if v[0]==1)} map, "
      f"{sum(1 for v in notable.values() if v[0]==2)} enemy)")

# ── NpcParam: lot -> placed? (filled after the MSB scan) ───────────────────────────────────────────
npc = E.param_to_dict(E.read_param(reg, "NpcParam", pdefs), {"itemLotId_map", "itemLotId_enemy"})
npc_to_lots = defaultdict(set)   # npcId -> {lots}
npc_lotrefs = set()
for nid, f in npc.items():
    for k in ("itemLotId_map", "itemLotId_enemy"):
        if f.get(k, 0) > 0:
            npc_to_lots[nid].add(f[k])
            npc_lotrefs.add(f[k])

# ── AEG asset pickups: aegRow -> lot ───────────────────────────────────────────────────────────────
aeg = E.param_to_dict(E.read_param(reg, "AssetEnvironmentGeometryParam", pdefs), {"pickUpItemLotParamId"})
aegrow_to_lot = {rid: f.get("pickUpItemLotParamId", 0) for rid, f in aeg.items()}

# ── Shop (∞ / finite) keyed by (equipType, equipId) ────────────────────────────────────────────────
shop = E.param_to_dict(E.read_param(reg, "ShopLineupParam", pdefs), {"equipId", "equipType", "sellQuantity"})
CAT2ET = {2: 0, 3: 1, 4: 2, 1: 3}  # lotItemCategory -> ShopLineupParam.equipType
sold_inf, sold_any = set(), set()
for f in shop.values():
    k = (f.get("equipType", -99), f.get("equipId", 0))
    sold_any.add(k)
    if f.get("sellQuantity", 0) == -1:
        sold_inf.add(k)

# ── MSB pass: treasure bases, placed (enabled) NPC ids, placed asset pickup lots, entity->pos ───────
print("=== scanning map/MapStudio ===")
treasure_bases = set()
placed_npc_ids = set()
placed_asset_lots = set()   # lots placed via an asset whose AEG pickUpItemLotParamId == lot
ent_pos = {}                # EntityID -> (tile, kind)  (positionable: enemy/asset, _00)
for mp in sorted(MSB_DIR.glob("*.msb.dcx")):
    stem = mp.name.replace(".msb.dcx", "")
    if stem.endswith("_99"):
        continue
    info = E.parse_map_name(mp.name)
    if not info or info.get("p3") == 99:
        continue
    try:
        msb = E._read_from_bytes(E._msbe_read, E.SoulsFormats.DCX.Decompress(str(mp)), ".msb")
    except Exception:
        continue
    for t in msb.Events.Treasures:
        try:
            il = int(t.ItemLotID)
        except Exception:
            continue
        if il > 0:
            treasure_bases.add(il)
    for p in msb.Parts.Enemies:
        if int(getattr(p, "GameEditionDisable", 0) or 0) == 1:
            continue
        try:
            nid = int(p.NPCParamID)
        except Exception:
            nid = 0
        if nid:
            placed_npc_ids.add(nid)
        try:
            eid = int(p.EntityID)
        except Exception:
            eid = 0
        if eid and eid not in ent_pos:
            ent_pos[eid] = (stem, "enemy")
    for p in msb.Parts.Assets:
        try:
            eid = int(p.EntityID)
        except Exception:
            eid = 0
        if eid and eid not in ent_pos:
            ent_pos[eid] = (stem, "asset")
        model = str(p.ModelName)
        if model.startswith("AEG"):
            try:
                a, b = model[3:].split("_")[:2]
                lot = aegrow_to_lot.get(int(a) * 1000 + int(b), 0)
            except Exception:
                lot = 0
            if lot:
                placed_asset_lots.add(lot)

placed_npc_lots = set()
for nid in placed_npc_ids:
    placed_npc_lots |= npc_to_lots.get(nid, set())

# ── EMEVD pass: EMEDF-precise award catalog + per-lot anchor resolvability ───────────────────────────
# Two distinct mechanisms:
#   • DIRECT item-lot instructions (non bank-2000): the lot sits in an arg TYPED "Item Lot" (EMEDF).
#     Only `positions=True` ones (2003:34 Spawn Snuggly) are markers; the rest (2003:04/36 Award) are
#     positionless gives to the player → correctly NOT markers.
#   • bank-2000 RunCommonEvent template inits: the lot is awarded by the TEMPLATE body, not a typed
#     arg, so we track by invoked template id (arg1) and whether the runtime parses that template.
print("=== scanning event/*.emevd (EMEDF-typed) ===")
_emevd_read = E.asm.GetType("SoulsFormats.EMEVD").GetMethod(
    "Read", E.BindingFlags.Public | E.BindingFlags.Static | E.BindingFlags.FlattenHierarchy,
    None, E.Array[E.SysType]([E.SysType.GetType("System.String")]), None)
ENT_MIN = 10000   # below this an int arg is a literal (flag/count), not a placeable entity id

direct_cat = defaultdict(lambda: {"lots": set(), "notable": set()})   # (bank,id) -> item-lot instr
tmpl_cat = defaultdict(lambda: {"lots": set(), "notable": set()})     # template eventId -> bank-2000
lot_direct = defaultdict(set)    # notable lot -> {(bank,id)} direct item-lot instrs awarding it
lot_tmpl = defaultdict(set)      # notable lot -> {template} bank-2000 inits carrying it
lot_tmpl_anchor = set()          # notable lot carried by a bank-2000 init with a positionable entity
for p in sorted(EVENT_DIR.glob("*.emevd.dcx")):
    try:
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + "_lrc.tmp")
        SysFile.WriteAllBytes(tmp, E.SoulsFormats.DCX.Decompress(str(p)).ToArray())
        emevd = _emevd_read.Invoke(None, Array[Object]([tmp]))
        os.unlink(tmp)
    except Exception:
        continue
    for ev in emevd.Events:
        for ins in ev.Instructions:
            bank = int(ins.Bank)
            iid = int(ins.ID)
            ab = bytes(ins.ArgData)
            n = len(ab) // 4
            if n < 1:
                continue
            ints = [struct.unpack_from("<i", ab, k * 4)[0] for k in range(n)]
            spec = INSTR.get((bank, iid))
            if bank == 2000:
                # template init: lot is awarded by the body; track by template id (arg1) + anchors.
                if n < 2:
                    continue
                template = ints[1]
                hit = [v for v in ints if v in notable]
                if not hit:
                    continue
                pos_ents = [v for v in ints if v >= ENT_MIN and v in ent_pos]
                for v in set(hit):
                    tmpl_cat[template]["lots"].add(v)
                    tmpl_cat[template]["notable"].add(v)
                    lot_tmpl[v].add(template)
                    if pos_ents:
                        lot_tmpl_anchor.add(v)
            elif spec and spec["lot_idxs"]:
                # DIRECT item-lot instruction: only the EMEDF item-lot arg(s) count (no coincidences).
                for idx in spec["lot_idxs"]:
                    if idx >= n:
                        continue
                    v = ints[idx]
                    if v not in ALL_LOTS:
                        continue
                    direct_cat[(bank, iid)]["lots"].add(v)
                    if v in notable:
                        direct_cat[(bank, iid)]["notable"].add(v)
                        lot_direct[v].add((bank, iid))


def template_parsed_key(template):
    return template_parsed(template)


def emevd_reached(lot):
    """Markers from EMEVD come ONLY through a PARSED bank-2000 template with a resolvable anchor
    (the direct item-lot instructions are positionless gives, except the unused 2003:34)."""
    if lot not in lot_tmpl_anchor:
        return False
    return any(template_parsed(t) for t in lot_tmpl.get(lot, ()))


# ── sequence-sibling reachability: lot = base+k of a covered base, contiguous chain (runtime walk) ──
covered_bases = treasure_bases | placed_npc_lots | placed_asset_lots
# also any lot directly emevd-reached is a base the sibling walk can extend from
covered_bases |= {l for l in notable if emevd_reached(l)}
SIB_MAX = 50


def row_exists(lot, lt):
    return lot in (ile if lt == 2 else ilm)


def sibling_reached(lot, lt):
    tbl = ile if lt == 2 else ilm
    for k in range(1, SIB_MAX + 1):
        base = lot - k
        if base in covered_bases:
            # contiguous chain base+1..lot must all exist in this table (runtime breaks on a gap)
            if all((base + j) in tbl for j in range(1, k + 1)):
                return base
        # a chain also ends at the next base; stop scanning further back past one
    return None


# ── classify every notable lot ──────────────────────────────────────────────────────────────────────
print("=== classifying ===")
reached_by = Counter()
unreached = []          # (lot, lt, itemId, cat, blocker, detail)
unreached_by = Counter()
for lot, (lt, iid, cat, flag) in notable.items():
    if lot in treasure_bases:
        reached_by["treasure"] += 1
        continue
    if lot in placed_asset_lots:
        reached_by["asset-pickup"] += 1
        continue
    if lot in placed_npc_lots:
        reached_by["npc-placed"] += 1
        continue
    if emevd_reached(lot):
        reached_by["emevd"] += 1
        continue
    sib = sibling_reached(lot, lt)
    if sib is not None:
        reached_by["sibling"] += 1
        continue

    # ── unreached: name the blocker (in priority order) ──
    direct = lot_direct.get(lot, set())                 # EMEDF item-lot instructions awarding it
    pos_direct = [k for k in direct if INSTR[k]["positions"]]   # position-bearing (markerable)
    give_direct = [k for k in direct if not INSTR[k]["positions"]]  # positionless give
    tmpls = lot_tmpl.get(lot, set())
    unparsed_tmpls = [t for t in tmpls if not template_parsed(t)]
    shop_key = (CAT2ET.get(cat), iid)
    if pos_direct:
        # a position-bearing item-lot instruction we don't parse → a REAL missing pass (2003:34 etc.)
        blocker = "missing-pass"
        detail = "direct " + ",".join(f"{b}:{i:02d}({INSTR[(b,i)]['name']})" for b, i in sorted(pos_direct))
    elif lot in lot_tmpl_anchor and unparsed_tmpls:
        # carried by an UNPARSED bank-2000 template with a resolvable anchor. NOT a confirmed gap:
        # template inits pass untyped args, so the lot may be a coincidental param (most are treasure-
        # glow/enable templates whose item is already a treasure). A candidate to spot-check by
        # decompiling the template — only a real one becomes a missing-pass.
        blocker = "emevd-template?"
        detail = "unparsed bank-2000 template(s) " + ",".join(f"ev{t}" for t in sorted(unparsed_tmpls)[:4]) + \
                 " carry it + an anchor (decompile to confirm it AWARDS the lot vs references it)"
    elif give_direct:
        blocker = "scripted-give"
        detail = "awarded only by " + ",".join(f"{INSTR[(b,i)]['name']}" for b, i in sorted(give_direct)) + \
                 " (no world position → correctly not a marker)"
    elif shop_key in sold_inf:
        blocker = "shop-inf"
        detail = "sold ∞ in ShopLineupParam (merchant give, no world spot)"
    elif shop_key in sold_any:
        blocker = "shop-finite"
        detail = "sold (finite) in ShopLineupParam"
    elif tmpls:
        blocker = "emevd-no-anchor"
        detail = f"in bank-2000 init(s) {sorted(tmpls)[:4]} but no positionable MSB entity co-located"
    elif lt == 2 and lot not in npc_lotrefs:
        blocker = "orphan-enemy"
        detail = "ItemLotParam_enemy row with NO NpcParam reference (bake mislabel / scripted)"
    elif lot in npc_lotrefs:
        blocker = "npc-not-placed"
        detail = "NpcParam refs lot but the npc is not placed in any enabled MSB enemy"
    else:
        blocker = "unreferenced"
        detail = "no MSB Treasure/Asset, NpcParam, item-lot instruction, or shop reference " \
                 "(cut/unused row, or a give the data doesn't model)"
    unreached.append((lot, lt, iid, cat, blocker, detail))
    unreached_by[blocker] += 1

# ── write docs/lot_reachability.md ───────────────────────────────────────────────────────────────────
total = len(notable)
nreached = sum(reached_by.values())
lines = []
A = lines.append
A("# Lot reachability census — am I missing a pass?")
A("")
A("**`baked=0` does not prove completeness.** A lot missed by BOTH the bake and the disk passes is "
  "invisible to a baked-residual scan. This census rebuilds the oracle from the RAW regulation + MSB + "
  "EMEVD, independent of any pass, and asks of every notable lot: *is it reachable, and if not, why?* "
  "Regenerate + `git diff` this file to catch coverage regressions (the mirror of "
  "`docs/nobake_scoreboard.md`). Source: `tools/lot_reachability_census.py` (ERR profile).")
A("")
A(f"- **Notable universe**: {total} lots (a real slot-1 item + a one-time obtain flag).")
A(f"- **Reached**: {nreached} ({100*nreached//max(1,total)}%) · **Unreached**: {len(unreached)}.")
A("- The goal is NOT unreached=0 — some lots are legitimately shop-only / quest-give / cut. The goal "
  "is **every unreached lot has a named blocker, and none is `missing-pass`**.")
A("")
A("## 1. Item-instruction catalog — the missing-pass detector (EMEDF-typed)")
A("")
A("Every EMEVD instruction that places/awards an item lot, identified PRECISELY: the lot must sit in an "
  "arg the EMEDF types as *Item Lot* (not a coincidental numeric match — that is how a ladder's disable-"
  "flag in `2009:00 Register Ladder` was once mistaken for a placement). Only `positions=yes` "
  "instructions are markers; `positions=no` ones (`Award Item Lot` etc.) give to the player with no "
  "world spot → correctly not markers. **A `positions=yes` instruction the parser does NOT handle is a "
  "real missing pass.**")
A("")
A("| bank:id | instruction | positions | lots | notable | parsed? |")
A("|---|---|---|--:|--:|---|")
for (b, i), d in sorted(direct_cat.items(), key=lambda x: (-len(x[1]["notable"]), x[0])):
    sp = INSTR[(b, i)]
    parsed = (b, i) in PARSED_DIRECT_INSTR
    pos = "yes" if sp["positions"] else "no (give)"
    pflag = "✅ yes" if parsed else ("⚠️ **NO** (FIX)" if (sp["positions"] and d["notable"]) else "n/a (give)")
    A(f"| {b}:{i:02d} | {sp['name']} | {pos} | {len(d['lots'])} | {len(d['notable'])} | {pflag} |")
A("")
mp_instr = [(k, d) for k, d in direct_cat.items() if INSTR[k]["positions"] and d["notable"]
            and k not in PARSED_DIRECT_INSTR]
if mp_instr:
    A(f"**🔴 {len(mp_instr)} position-bearing item instruction(s) carry notable lots and are NOT parsed "
      "— real missing pass(es):**")
    for (b, i), d in mp_instr:
        A(f"- `{b}:{i:02d}` **{INSTR[(b,i)]['name']}** — {len(d['notable'])} notable lots, e.g. {sorted(d['notable'])[:6]}")
    A("")
else:
    A("**✅ Every position-bearing item instruction is parsed (or unused) — no direct-instruction "
      "missing pass.**")
    A("")
A("### bank-2000 RunCommonEvent templates carrying notable lots")
A("")
A("Templates award via their body (no typed arg here), so this is by invoked template id. A template "
  "the parser handles = covered; an unparsed one carrying notable lots with a resolvable anchor is a "
  "candidate (but many are treasure-glow/enable templates whose lot is already placed by the MSB "
  "Treasure pass — confirm via the per-lot reachability, which marks those `reached:treasure`).")
A("")
A("| template | lots | notable | parsed? |")
A("|---|--:|--:|---|")
for t, d in sorted(tmpl_cat.items(), key=lambda x: (template_parsed(x[0]), -len(x[1]["notable"])))[:40]:
    pflag = "✅ yes" if template_parsed(t) else ("⚠️ no" if d["notable"] else "—")
    A(f"| ev{t} | {len(d['lots'])} | {len(d['notable'])} | {pflag} |")
A("")
A("## 2. Reached, by mechanism")
A("")
A("| mechanism | lots |")
A("|---|--:|")
for m, n in reached_by.most_common():
    A(f"| {m} | {n} |")
A(f"| **total reached** | **{nreached}** |")
A("")
A("## 3. Unreached notable lots, by named blocker")
A("")
A("`missing-pass` = a position-bearing item instruction we don't parse (FIX). `emevd-template?` = "
  "carried by an unparsed bank-2000 template + an anchor — a CANDIDATE to decompile (most are treasure-"
  "glow templates referencing an already-placed lot, NOT a gap). `scripted-give` = Award-Item-Lot to "
  "the player, no world position (correctly not a marker). `shop-*` = merchant give. `npc-not-placed` = "
  "the drop's enemy isn't placed. `orphan-enemy` = `_enemy` row with no NpcParam. `unreferenced` = "
  "cut/unused row.")
A("")
A("| blocker | count | kind |")
A("|---|--:|---|")
KIND = {"missing-pass": "🔴 FIX", "emevd-template?": "🟡 candidate (decompile)",
        "scripted-give": "✅ no position", "emevd-no-anchor": "🟡 RE anchor", "npc-not-placed": "🟡 RE",
        "shop-inf": "✅ correct", "shop-finite": "✅ correct", "orphan-enemy": "🟡 investigate",
        "unreferenced": "✅ likely cut/unused"}
for b, n in unreached_by.most_common():
    A(f"| `{b}` | {n} | {KIND.get(b,'?')} |")
A("")
mp = [u for u in unreached if u[4] == "missing-pass"]
if mp:
    A(f"### 🔴 {len(mp)} `missing-pass` lots — a parser gap, FIX:")
    A("")
    for lot, lt, iid, cat, blk, detail in sorted(mp)[:40]:
        A(f"- lot `{lot}` (lt{lt}, item {iid}) — {detail}")
    A("")
A("### Sample unreached per other blocker (first 8)")
A("")
by_b = defaultdict(list)
for u in unreached:
    by_b[u[4]].append(u)
for b in sorted(by_b):
    if b == "missing-pass":
        continue
    sample = sorted(by_b[b])[:8]
    A(f"- **`{b}`** ({len(by_b[b])}): " + ", ".join(f"{lot}(lt{lt})" for lot, lt, _, _, _, _ in sample))
A("")

OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
print(f"\nwrote {OUT}")
print(f"  notable={total} reached={nreached} unreached={len(unreached)}")
print(f"  position-bearing-unparsed-instr={len(mp_instr)}  missing-pass-lots={len(mp)}")
for b, n in unreached_by.most_common():
    print(f"    {b:<18} {n}")
