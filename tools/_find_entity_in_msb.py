"""Search MSB files for the entity IDs from 90005724 callers."""
import sys, io, os, tempfile
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pythonnet import load
load('coreclr')
from System.Reflection import Assembly
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
import SoulsFormats
_str = SysType.GetType('System.String')
_msbcls = asm.GetType('SoulsFormats.MSBE')
_msbr = _msbcls.BaseType.GetMethod('Read', Array[SysType]([_str]))

TARGETS = {1041320290, 1043330290, 1041325291, 1043335291}
MSB_DIR = config.ERR_MOD_DIR / 'map/mapstudio'
if not MSB_DIR.exists():
    MSB_DIR = config.ERR_MOD_DIR / 'mapstudio'
print(f"MSB dir: {MSB_DIR}")

for path in sorted(MSB_DIR.glob('*.msb.dcx')):
    tmp = os.path.join(tempfile.gettempdir(), '_m.msb')
    SysFile.WriteAllBytes(tmp, SoulsFormats.DCX.Decompress(str(path)).ToArray())
    msb = _msbr.Invoke(None, Array[Object]([tmp]))
    print(f"\n=== {path.name} ===")
    # Walk all part collections
    for kind in ('Enemies', 'Assets', 'DummyAssets', 'MapPieces', 'Players',
                 'DummyEnemies', 'Collisions'):
        try: coll = getattr(msb.Parts, kind, None)
        except: coll = None
        if coll is None: continue
        for p in coll:
            try: eid = int(p.EntityID)
            except: continue
            if eid in TARGETS:
                print(f"  [{kind}] name={p.Name} model={getattr(p,'ModelName','?')} "
                      f"entityID={eid} pos=({float(p.Position.X):.1f},{float(p.Position.Y):.1f},{float(p.Position.Z):.1f})")
    # Also check events
    for ekind in ('Treasures', 'PatrolInfo', 'Generators', 'ObjActs', 'MapOffsets',
                  'PlatoonInfo', 'PseudoMultiplayers', 'Navigation', 'NPCSummonInfo'):
        try: coll = getattr(msb.Events, ekind, None)
        except: coll = None
        if coll is None: continue
        for ev in coll:
            try: eid = int(getattr(ev, 'EntityID', 0))
            except: eid = 0
            if eid in TARGETS:
                print(f"  [Events.{ekind}] name={ev.Name} entityID={eid}")
    os.unlink(tmp)
