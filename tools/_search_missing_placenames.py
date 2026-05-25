import sys, io, os, tempfile, re
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
_fcls = asm.GetType('SoulsFormats.FMG')
_fr = _fcls.BaseType.GetMethod('Read', Array[SysType]([_str]))
_bcls = asm.GetType('SoulsFormats.BND4')
_br = _bcls.BaseType.GetMethod('Read', Array[SysType]([_str]))

targets = {61003, 62003, 62004, 64003, 64004, 64005,
           6800, 6810, 6820, 6830, 6840, 6850, 6900, 6920, 6940, 6970}
for src in [config.ERR_MOD_DIR/'msg/engus', config.GAME_DIR/'msg/engus']:
    print(f"\n=== {src} ===")
    for p in sorted(src.glob('*.msgbnd.dcx')):
        bnd = _br.Invoke(None, Array[Object]([str(p)]))
        for f in bnd.Files:
            full = str(f.Name)
            if not full.lower().endswith('.fmg'): continue
            tmp = os.path.join(tempfile.gettempdir(),'_f.fmg')
            SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
            try: fmg = _fr.Invoke(None, Array[Object]([tmp]))
            except Exception: continue
            hits = []
            for e in fmg.Entries:
                eid = int(e.ID)
                if eid in targets:
                    t = str(e.Text) if e.Text else ''
                    if t and t != '[ERROR]':
                        hits.append((eid, re.sub(r'<[^>]+>','',t)))
            if hits:
                fname = full.rsplit('\\', 1)[-1]
                print(f"  {p.name}/{fname}:")
                for eid, t in hits:
                    print(f"    id={eid}: {t!r}")
