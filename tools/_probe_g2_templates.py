#!/usr/bin/env python3
"""RE recon (Group 2): map the common EMEVD templates (90005xxx / 90006xxx) to the ASSET MODELS they
bind, so elevator / smithing-table / hidden-passage / etc. templates can be spotted the same way Portal
was (template 90005605 -> AEG099_510). For each bank-2000 template call, resolve each arg that is an
entity to its MSB Asset/Enemy model, and tally template -> {model: count}. Prints the templates whose
bound models cluster on a single AEG model (the clean, wire-able ones).

Usage: py _probe_g2_templates.py
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
def _rd(tn): return asm.GetType(tn).GetMethod('Read', BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy, None, Array[SysType]([_st]), None)
_msbe = _rd('SoulsFormats.MSBE'); _emevd = _rd('SoulsFormats.EMEVD')
def rfb(rm, data, suf):
    t = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_g2' + suf)
    SysFile.WriteAllBytes(t, data.ToArray() if hasattr(data, 'ToArray') else data)
    r = rm.Invoke(None, Array[Object]([t])); os.unlink(t); return r

# entity -> model (Assets + Enemies)
ent_model = {}
for mp in sorted((ERR/'map'/'MapStudio').glob('*.msb.dcx')):
    try: msb = rfb(_msbe, SoulsFormats.DCX.Decompress(str(mp)), '.msb')
    except Exception: continue
    for coll in (msb.Parts.Assets, msb.Parts.Enemies):
        for p in coll:
            e = int(getattr(p, 'EntityID', 0) or 0)
            if e > 0: ent_model.setdefault(e, str(p.ModelName))
print(f"# entity->model map: {len(ent_model)} placed entities")

# scan EMEVD: template -> Counter(model) over args that resolve to a placed entity; + call count
tmpl_models = collections.defaultdict(collections.Counter)
tmpl_calls = collections.Counter()
tmpl_entcount = collections.defaultdict(set)
for ep in sorted((ERR/'event').glob('*.emevd.dcx')):
    try: emevd = rfb(_emevd, SoulsFormats.DCX.Decompress(str(ep)), '.evd')
    except Exception: continue
    for evt in emevd.Events:
        for ins in evt.Instructions:
            if int(ins.Bank) != 2000: continue
            a = bytes(ins.ArgData) if ins.ArgData else b''
            if len(a) < 8: continue
            tid = struct.unpack_from('<i', a, 4)[0]
            if not (90005000 <= tid <= 90006999): continue   # shared common templates only
            tmpl_calls[tid] += 1
            seen_model = None
            for off in range(8, len(a) - 3, 4):
                v = struct.unpack_from('<i', a, off)[0]
                m = ent_model.get(v)
                if m:
                    if seen_model != m:  # count each distinct model once per call
                        tmpl_models[tid][m] += 1; seen_model = m
                    tmpl_entcount[tid].add(v)

# report: templates dominated by ONE AEG model (clean wire), excluding already-wired ones
WIRED = {90005605}  # Portal
KNOWN = {90005683:'HeroTomb', 90005792:'HostileNPC', 90006051:'Seal', 90005570:'Gesture',
         90005702:'QuestNPC', 90005632:'Painting', 90005633:'Painting'}
print("\n# common templates by dominant bound model (candidate Group-2 features):")
rows = []
for tid, mc in tmpl_models.items():
    if not mc: continue
    top_model, top_n = mc.most_common(1)[0]
    total = sum(mc.values())
    purity = top_n / total
    rows.append((len(tmpl_entcount[tid]), purity, tid, top_model, top_n, total, tmpl_calls[tid], mc))
rows.sort(reverse=True)
for nent, purity, tid, tm, tn, total, calls, mc in rows:
    if nent < 3: continue
    tag = f" [{KNOWN[tid]}]" if tid in KNOWN else (" [PORTAL-wired]" if tid in WIRED else "")
    models = ', '.join(f'{m}:{c}' for m, c in mc.most_common(5))
    print(f"   tmpl {tid}{tag}: {nent} entities, {calls} calls, top {tm} ({tn}/{total}, {purity*100:.0f}% pure) | {models}")
