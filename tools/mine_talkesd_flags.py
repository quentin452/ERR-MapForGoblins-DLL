#!/usr/bin/env python3
"""TalkESD death/loss-flag miner (pipeline v1) for NPCs that have NO EMEVD death
handler (the DLC followers). For each NPC name: resolve entity + TalkID from the
MSBs, locate its t<TalkID>.esd in the talkesdbnds, decode every int literal in
the state-machine (command args + condition evaluators), and report candidate
event-flag ids the NPC's talk references (the "dead/concluded" flag is among them).

EzState arg/evaluator bytecode: a 4-byte int literal is `0x82` + int32 LE. We
harvest those; large ids in flag ranges are candidate flags, ids matching the
NPC's own entity-namespace are highlighted. Candidates still need save-diff /
runtime confirmation -- this narrows ~the whole flag space to a handful.

Usage: py mine_talkesd_flags.py            # default DLC followers + Patches(validate)
       py mine_talkesd_flags.py Ansbach Moore ...
"""
import sys, io, os, tempfile, re, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from collections import defaultdict
from pathlib import Path
from pythonnet import load; load('coreclr')
import clr; clr.AddReference(str(config.SOULSFORMATS_DLL))
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
import SoulsFormats
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str = SysType.GetType('System.String')
def rd(tn):
    return asm.GetType(tn).GetMethod('Read', BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy, None, Array[SysType]([_str]), None)
_param, _bnd4, _fmg, _msbe, _esd = (rd('SoulsFormats.PARAM'), rd('SoulsFormats.BND4'),
                                    rd('SoulsFormats.FMG'), rd('SoulsFormats.MSBE'), rd('SoulsFormats.ESD'))
def frombytes(m, d, s):
    t = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_mt' + s)
    SysFile.WriteAllBytes(t, d.ToArray() if hasattr(d, 'ToArray') else d)
    r = m.Invoke(None, Array[Object]([t])); os.unlink(t); return r
def to_bytes(b):
    return bytes(b.ToArray() if hasattr(b, 'ToArray') else b)

DEFAULT = ['Leda', 'Ansbach', 'Moore', 'Thiollier', 'Dane', 'Freyja', 'Hornsent',
           'Queelign', 'Igon', 'Jolan', 'Patches']  # Patches = validation anchor (flag 3683)

def load_paramdefs():
    d = {}
    for x in config.PARAMDEF_DIR.glob('*.xml'):
        try:
            pd = SoulsFormats.PARAMDEF.XmlDeserialize(str(x), False)
            if pd and pd.ParamType: d[str(pd.ParamType)] = pd
        except Exception: pass
    return d
def read_param(bnd, name, pds):
    for f in bnd.Files:
        if name in str(f.Name):
            p = frombytes(_param, f.Bytes, '.param')
            pt = str(p.ParamType) if p.ParamType else ''
            if pt in pds: p.ApplyParamdef(pds[pt])
            return p
    raise KeyError(name)

def decode_int_literals(buf):
    """Yield every 0x82 int32-LE literal in an EzState bytecode buffer."""
    i, n = 0, len(buf)
    out = []
    while i < n:
        b = buf[i]
        if b == 0x82 and i + 5 <= n:
            out.append(struct.unpack('<i', buf[i+1:i+5])[0]); i += 5
        else:
            i += 1
    return out

def harvest_esd_ints(esd):
    """All int literals across every command arg + condition evaluator."""
    ints = []
    for gkv in esd.StateGroups:
        for skv in gkv.Value:
            st = skv.Value
            for cln in ('EntryCommands', 'ExitCommands', 'WhileCommands'):
                for cmd in (getattr(st, cln, None) or []):
                    for a in cmd.Arguments:
                        ints += decode_int_literals(to_bytes(a))
            for cond in (getattr(st, 'Conditions', None) or []):
                ev = getattr(cond, 'Evaluator', None)
                if ev: ints += decode_int_literals(to_bytes(ev))
    return ints

def main():
    needles = [a.lower() for a in sys.argv[1:]] or [d.lower() for d in DEFAULT]
    g = config.require_game_dir(); pds = load_paramdefs()
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(g / 'regulation.bin'))
    np = read_param(bnd, 'NpcParam', pds)
    npc_name_id = {}
    for row in np.Rows:
        for c in row.Cells:
            if str(c.Def.InternalName) == 'nameId':
                try: npc_name_id[int(row.ID)] = int(str(c.Value))
                except: pass
                break
    names = {}
    for mb in ('item.msgbnd.dcx', 'item_dlc01.msgbnd.dcx', 'item_dlc02.msgbnd.dcx'):
        p = g / 'msg' / 'engus' / mb
        if not p.exists(): continue
        b = frombytes(_bnd4, SoulsFormats.DCX.Decompress(str(p)), '.bnd')
        for f in b.Files:
            base = str(f.Name).replace(chr(92), '/').rsplit('/', 1)[-1]
            if base == 'NpcName.fmg' or re.match(r'NpcName_dlc\d+\.fmg$', base, re.I):
                fmg = frombytes(_fmg, f.Bytes, '.fmg')
                for e in fmg.Entries:
                    t = str(e.Text) if e.Text else ''
                    if t and t != '[ERROR]': names[int(e.ID)] = t

    # 1) scan MSBs -> (name -> set of (entity, talkId))
    print("Scanning MSBs for target NPCs (entity + TalkID)...", flush=True)
    npc_hits = defaultdict(set)
    for path in sorted((g / 'map' / 'MapStudio').glob('*.msb.dcx')):
        try: msb = frombytes(_msbe, SoulsFormats.DCX.Decompress(str(path)), '.msb')
        except Exception: continue
        for e in msb.Parts.Enemies:
            ent = int(getattr(e, 'EntityID', 0) or 0)
            if ent <= 0: continue
            nm = names.get(npc_name_id.get(int(getattr(e, 'NPCParamID', 0) or 0), 0), '')
            if nm and any(nd in nm.lower() for nd in needles):
                tid = int(getattr(e, 'TalkID', 0) or 0)
                npc_hits[nm].add((ent, tid))

    # 2) index every t<id>.esd across all talkesdbnds
    print("Indexing talk ESDs...", flush=True)
    esd_index = {}  # talkId -> (esd object, source bnd name)
    for path in sorted((g / 'script' / 'talk').glob('*.talkesdbnd.dcx')):
        try: tb = frombytes(_bnd4, SoulsFormats.DCX.Decompress(str(path)), '.bnd')
        except Exception: continue
        for f in tb.Files:
            base = str(f.Name).replace(chr(92), '/').rsplit('/', 1)[-1]
            m = re.match(r't(\d+)\.esd$', base)
            if m:
                esd_index[int(m.group(1))] = (f, path.name)

    # 3) per NPC: decode candidate flags
    FLAG_LO, FLAG_HI = 1000, 90000000           # plausible event-flag range (excl. 9-digit entity/gen)
    for nm in sorted(npc_hits):
        print(f"\n=== {nm} ===")
        for ent, tid in sorted(npc_hits[nm]):
            ns = ent // 1000  # entity namespace prefix
            entry = esd_index.get(tid)
            if not entry:
                print(f"  entity {ent}  talkId {tid}: no t{tid}.esd found")
                continue
            f, src = entry
            try:
                esd = frombytes(_esd, f.Bytes, '.esd')
                ints = harvest_esd_ints(esd)
            except Exception as e:
                print(f"  entity {ent}  talkId {tid}: ESD parse fail ({e})")
                continue
            flags = sorted({v for v in ints if FLAG_LO <= v <= FLAG_HI})
            # highlight ids sharing the NPC's entity namespace (likely its own quest flags)
            own = [v for v in flags if v // 1000 == ns or str(v).startswith(str(ns)[:5])]
            print(f"  entity {ent}  talkId {tid}  (t{tid}.esd @ {src})")
            print(f"    candidate flags ({len(flags)}): {flags}")
            if own:
                print(f"    namespace-matched (strongest): {own}")

if __name__ == '__main__':
    main()
