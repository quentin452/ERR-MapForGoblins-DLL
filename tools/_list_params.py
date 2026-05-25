import sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pythonnet import load
load('coreclr')
from System.Reflection import Assembly
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
import SoulsFormats

bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(config.ERR_MOD_DIR / 'regulation.bin'))
seen = set()
for f in bnd.Files:
    n = str(f.Name)
    if not n.lower().endswith('.param'):
        continue
    base = n.split('\\')[-1].split('/')[-1]
    if base in seen:
        continue
    seen.add(base)
    nl = base.lower()
    if any(k in nl for k in ('world', 'area', 'place')):
        print(base)
