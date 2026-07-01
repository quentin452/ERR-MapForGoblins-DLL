#!/usr/bin/env python3
"""RE probe 3 (Portal): find the EMEVD warp template that references AEG099_510 gate entities.

Pass 1: collect every AEG099_510 EntityID from the MSBs (the sending-gate asset, per _probe_portal_aeg).
Pass 2: scan EMEVD bank-2000 InitializeEvent calls (arg[1]@off4 = called template id; args off8+ = params).
        For each call whose params contain a gate entity id, tally (template_id, arg_offset).
The template with the most distinct gate-entity hits at a stable offset = the sending-gate warp template;
its total call count ≈ the real portal set. If NOTHING references them → portals are runtime/param-driven
(like graces from BonfireWarpParam) → need a live RPM read, not a static MSB/EMEVD pass.

Usage: py _probe_portal_emevd.py
"""
import sys, io, os, struct, tempfile, collections
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL)); clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats
ERR = config.require_err_mod_dir()
_st = SysType.GetType('System.String')
def _rd(tn):
    return asm.GetType(tn).GetMethod('Read', BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy,
                                     None, Array[SysType]([_st]), None)
_msbe = _rd('SoulsFormats.MSBE'); _emevd = _rd('SoulsFormats.EMEVD')
def rfb(rm, data, suf):
    t = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_pe' + suf)
    SysFile.WriteAllBytes(t, data.ToArray() if hasattr(data, 'ToArray') else data)
    r = rm.Invoke(None, Array[Object]([t])); os.unlink(t); return r

# ---- pass 1: AEG099_510 entity ids ----
gate_eids = set()
gate_meta = {}   # eid -> (area,gx,gz)
for mp in sorted((ERR/'map'/'MapStudio').glob('*.msb.dcx')):
    pt = mp.name.replace('.msb.dcx', '').split('_')
    if len(pt) < 4: continue
    area, gx, gz = int(pt[0][1:]), int(pt[1]), int(pt[2])
    try: msb = rfb(_msbe, SoulsFormats.DCX.Decompress(str(mp)), '.msb')
    except Exception: continue
    for p in msb.Parts.Assets:
        if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1: continue
        if str(p.ModelName) != 'AEG099_510': continue
        e = int(getattr(p, 'EntityID', 0) or 0)
        if e > 0:
            gate_eids.add(e); gate_meta.setdefault(e, (area, gx, gz))
print(f"# AEG099_510 gate entity ids: {len(gate_eids)}")

# ---- pass 2: EMEVD bank-2000 template calls referencing gate entities ----
# (template_id, arg_offset) -> set(gate eids hit); also total call count per template
tmpl_hits = collections.defaultdict(lambda: collections.defaultdict(set))
tmpl_calls = collections.Counter()
tmpl_all_args = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))  # tid->off->Counter(val)
for ep in sorted((ERR/'event').glob('*.emevd.dcx')):
    try: emevd = rfb(_emevd, SoulsFormats.DCX.Decompress(str(ep)), '.evd')
    except Exception: continue
    for evt in emevd.Events:
        for ins in evt.Instructions:
            if int(ins.Bank) != 2000: continue
            a = bytes(ins.ArgData) if ins.ArgData else b''
            if len(a) < 8: continue
            tid = struct.unpack_from('<i', a, 4)[0]
            tmpl_calls[tid] += 1
            for off in range(8, len(a) - 3, 4):
                v = struct.unpack_from('<i', a, off)[0]
                if v in gate_eids:
                    tmpl_hits[tid][off].add(v)

# ---- report ----
print("\n# templates whose params reference AEG099_510 entities (template, offset -> #distinct gates / #calls):")
ranked = []
for tid, offs in tmpl_hits.items():
    best_off = max(offs, key=lambda o: len(offs[o]))
    ranked.append((len(offs[best_off]), tid, best_off, tmpl_calls[tid]))
ranked.sort(reverse=True)
if not ranked:
    print("   *** NONE — no bank-2000 template references any AEG099_510 entity ***")
    print("   => portals are NOT statically bound in EMEVD templates. Likely runtime/param-driven")
    print("      (grace-style): the warp table is a live param → needs an RPM read, not a static pass.")
else:
    for nhit, tid, off, calls in ranked[:15]:
        print(f"   template {tid}: {nhit} distinct gates at arg-offset {off} (arg[{off//4}]) | {calls} total calls")
    top = ranked[0]
    print(f"\n# best candidate warp template: {top[1]} — {top[0]} gates matched at arg[{top[2]//4}], "
          f"{top[3]} total calls (≈ portal count if this is THE template)")
