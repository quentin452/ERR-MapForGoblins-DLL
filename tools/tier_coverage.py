#!/usr/bin/env python3
"""Characterize each MSB tile tier (_00/_01/_02/_10/_11/_12/_99): how much UNIQUE content
(enemy EntityIDs + treasure lotIds not present in any _00 tile) each holds. Tells us whether
non-_00 tiers are LOD-duplicates (skip) or real content the _00-only parser misses."""
import sys, io, os, tempfile
from collections import defaultdict
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats
from pathlib import Path

_str=SysType.GetType('System.String')
_read=asm.GetType('SoulsFormats.MSBE').GetMethod('Read',BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy,None,Array[SysType]([_str]),None)
def rfb(d):
    tmp=os.path.join(tempfile.gettempdir(),str(os.getpid())+'_t.msb'); SysFile.WriteAllBytes(tmp,d.ToArray()); r=_read.Invoke(None,Array[Object]([tmp])); os.unlink(tmp); return r

M=Path(r"D:\DOWNLOAD\ERRv2.2.9.6-541-2-2-9-6-1780861369\ERRv2.2.9.6\mod\map\MapStudio")
# Pass 1: collect _00 entityIds + treasure part presence
ent_by_tier=defaultdict(set)   # tier -> set of enemy EntityIDs
tre_by_tier=defaultdict(int)   # tier -> positioned-treasure count
files_by_tier=defaultdict(int)
for p in sorted(M.glob('*.msb.dcx')):
    parts=p.name.replace('.msb.dcx','').split('_')
    if len(parts)<4: continue
    tier=parts[3]
    files_by_tier[tier]+=1
    try: msb=rfb(SoulsFormats.DCX.Decompress(str(p)))
    except: continue
    for e in msb.Parts.Enemies:
        try:
            eid=int(getattr(e,'EntityID',0) or 0)
            if eid>0: ent_by_tier[tier].add(eid)
        except: pass
    for t in (getattr(msb.Events,'Treasures',None) or []):
        tre_by_tier[tier]+=1

ent00=ent_by_tier['00']
print(f"{'tier':5} {'files':>6} {'enemyEIDs':>10} {'EIDs∉_00':>9} {'treasures':>10}")
for tier in sorted(files_by_tier):
    eids=ent_by_tier[tier]
    uniq=len(eids - ent00) if tier!='00' else 0
    print(f"{tier:5} {files_by_tier[tier]:>6} {len(eids):>10} {uniq:>9} {tre_by_tier[tier]:>10}")
