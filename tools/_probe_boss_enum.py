#!/usr/bin/env python3
"""Mod-agnostic boss enumeration: EMEVD 2003[011] x MSB placement x GameAreaParam x NpcName FMG.

Reproduces docs/re/cross_mod_boss_naming_re_findings.md on ANY install (vanilla, randomizer,
ERR, Convergence). Reads only the ACTIVE install's own files — no bake, no hardcoded table.

    python tools/_probe_boss_enum.py <game_data_dir> [out.json] [lang]

<game_data_dir> = a dir holding regulation.bin + event/ + map/MapStudio/ + msg/<lang>/
(the ERR/randomizer mod overlay dir, or a UXM-unpacked vanilla Game dir).

The chain:
  EMEVD 2003[011](state, entityId, hpBarSlot, nameId) -> per-ENCOUNTER boss identity + FMG name id
  MSB Parts.Enemies[EntityID]                          -> world position
  GameAreaParam[rowId == entityId].defeatBossFlagId    -> cleared/defeat flag (additive layer)
  NpcName.fmg[nameId]                                  -> display string
"""
import json
import os
import struct
import sys
import tempfile
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_all_items as E  # noqa: E402  (performs the .NET / SoulsFormats bootstrap)
from System import Array, Object  # noqa: E402
from System.IO import File as SysFile  # noqa: E402

BAND_LO, BAND_HI = 900000000, 910000000   # vanilla NpcName boss band
BOSS_BAR_OPCODE = (2003, 11)              # HandleBossHealthBar

_rd_emevd = E.asm.GetType("SoulsFormats.EMEVD").GetMethod(
    "Read", E.BindingFlags.Public | E.BindingFlags.Static | E.BindingFlags.FlattenHierarchy,
    None, E.Array[E.SysType]([E.SysType.GetType("System.String")]), None)

_seq = [0]


def _read(read_method, data, suffix):
    """extract_all_items._read_from_bytes reuses ONE temp path, and SoulsFormats keeps a mapped
    section open on it -> a second read in the same process fails. Use a fresh path per call."""
    _seq[0] += 1
    tmp = os.path.join(tempfile.gettempdir(), f"{os.getpid()}_{_seq[0]}_mfgboss{suffix}")
    SysFile.WriteAllBytes(tmp, data.ToArray() if hasattr(data, "ToArray") else data)
    return read_method.Invoke(None, Array[Object]([tmp]))


def emevd_boss_bars(src):
    """{(map, entityId): nameId} from every HandleBossHealthBar in the install's event/."""
    pairs = {}
    for p in sorted((src / "event").glob("*.emevd.dcx")):
        try:
            em = _read(_rd_emevd, E.SoulsFormats.DCX.Decompress(str(p)), ".emevd")
        except Exception:
            continue
        mp = p.name.split(".")[0]
        for ev in em.Events:
            for ins in ev.Instructions:
                if (int(ins.Bank), int(ins.ID)) != BOSS_BAR_OPCODE:
                    continue
                a = bytes(ins.ArgData)
                if len(a) < 16:
                    continue
                _state, entity, _slot, name_id = struct.unpack_from("<iiii", a, 0)
                if entity > 0 and BAND_LO <= name_id < BAND_HI:
                    pairs.setdefault((mp, entity), name_id)
    return pairs


def msb_enemy_positions(src):
    """{(map, entityId): (x, y, z, npcParamId, model)} for every placed enemy."""
    out = {}
    for mp in sorted((src / "map" / "MapStudio").glob("*.msb.dcx")):
        try:
            msb = _read(E._msbe_read, E.SoulsFormats.DCX.Decompress(str(mp)), ".msb")
        except Exception:
            continue
        name = mp.name.split(".")[0]
        for en in (getattr(msb.Parts, "Enemies", None) or []):
            try:
                eid = int(en.EntityID)
            except Exception:
                continue
            if eid > 0:
                pos = en.Position
                out[(name, eid)] = (float(pos.X), float(pos.Y), float(pos.Z),
                                    int(en.NPCParamID), str(en.ModelName))
    return out


def game_area_flags(src):
    """{rowId: defeatBossFlagId} — rowId is the boss ENTITY id (verified live 2026-07-27)."""
    reg = E.SoulsFormats.SFUtil.DecryptERRegulation(str(src / "regulation.bin"))
    pdefs = E.load_paramdefs()
    for f in reg.Files:
        if str(f.Name).split("\\")[-1] != "GameAreaParam.param":
            continue
        param = _read(E._param_read, f.Bytes, ".param")
        param.ApplyParamdef(pdefs[str(param.ParamType)])
        return {int(r.ID): int({str(c.Def.InternalName): c.Value for c in r.Cells}
                               .get("defeatBossFlagId", 0)) for r in param.Rows}
    return {}


def npc_names(src, lang):
    """{fmgId: text} from every NpcName*.fmg in the install's msg/<lang>/."""
    texts = {}
    for c in sorted((src / "msg" / lang).glob("*.msgbnd.dcx")):
        try:
            bnd = _read(E._bnd4_read, E.SoulsFormats.DCX.Decompress(str(c)), ".msgbnd")
        except Exception:
            continue
        for f in bnd.Files:
            if "NpcName" not in str(f.Name).split("\\")[-1]:
                continue
            for entry in _read(E._fmg_read, f.Bytes, ".fmg").Entries:
                t = str(entry.Text) if entry.Text is not None else ""
                if t:
                    texts.setdefault(int(entry.ID), t)
    return texts


def main():
    src = Path(sys.argv[1])
    out_path = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    lang = sys.argv[3] if len(sys.argv) > 3 else "engus"

    bars = emevd_boss_bars(src)
    print(f"EMEVD 2003[011]: {len(bars)} boss bars, {len(set(bars.values()))} distinct nameIds")
    pos = msb_enemy_positions(src)
    print(f"MSB: {len(pos)} placed enemies with an entity id")
    flags = game_area_flags(src)
    print(f"GameAreaParam: {len(flags)} rows")
    names = npc_names(src, lang)
    print(f"NpcName ({lang}): {len(names)} ids")

    bosses, orphans = [], []
    for (mp, entity), name_id in sorted(bars.items()):
        hit = pos.get((mp, entity))
        if hit is None:
            # a boss bar can be raised from a map that is not the one the entity is placed in
            cands = [k for k in pos if k[1] == entity]
            hit = pos[cands[0]] if len(cands) == 1 else None
        if hit is None:
            orphans.append((mp, entity, name_id))
            continue
        x, y, z, npc, model = hit
        bosses.append(dict(map=mp, entity=entity, nameId=name_id, name=names.get(name_id),
                           x=round(x, 1), y=round(y, 1), z=round(z, 1),
                           npcParamId=npc, model=model, defeatFlag=flags.get(entity)))

    named = sum(1 for b in bosses if b["name"])
    print(f"\nJOINED {len(bosses)}/{len(bars)} with a position, {named}/{len(bosses)} with a name")
    print(f"  no MSB placement: {len(orphans)} {orphans[:5]}")
    print(f"  no GameAreaParam row: {sum(1 for b in bosses if b['defeatFlag'] is None)}")
    areas = defaultdict(int)
    for b in bosses:
        areas[b["map"][1:3]] += 1
    print(f"  per area: {dict(sorted(areas.items()))}")

    dups = [(n, c) for n, c in Counter(b["name"] for b in bosses).most_common() if c > 1]
    print(f"  names shared by >1 encounter: {len(dups)} {dups[:6]}")
    print("\n  sample:")
    for b in bosses[:10]:
        print(f"    {b['map']:22s} e={b['entity']:<11} {str(b['name'])[:38]:40s} "
              f"({b['x']},{b['y']},{b['z']}) flag={b['defeatFlag']}")

    if out_path:
        out_path.write_text(json.dumps(bosses, indent=1, ensure_ascii=False), encoding="utf-8")
        print(f"\nwrote {out_path}")


main()
