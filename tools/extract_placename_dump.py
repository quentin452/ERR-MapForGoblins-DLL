#!/usr/bin/env python3
"""Dump the PlaceName FMG (English) to data/PlaceName_engus.json.

Output (into config.DATA_DIR, profile-aware): {id: text} for every non-empty
PlaceName entry across base+DLC layers of item_dlc02.msgbnd.

Consumed by generate_location_overrides.py (hybrid location naming validates
every candidate PlaceName id against this dump so no marker ever points at a
missing FMG entry) and extract_map_name_regions.py.
"""
import sys
import io
import os
import json
import tempfile
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

import config
from pythonnet import load as _pyload
_pyload('coreclr')
from System.Reflection import Assembly
from System import Array, Type as SysType, Object
from System.IO import File as SysFile

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
import SoulsFormats

_str = SysType.GetType('System.String')
_fr = asm.GetType('SoulsFormats.FMG').BaseType.GetMethod('Read', Array[SysType]([_str]))
_br = asm.GetType('SoulsFormats.BND4').BaseType.GetMethod('Read', Array[SysType]([_str]))


def main():
    src = config.require_err_mod_dir()
    out = config.DATA_DIR
    out.mkdir(parents=True, exist_ok=True)

    names = {}
    msg = _br.Invoke(None, Array[Object]([str(src / 'msg' / 'engus' / 'item_dlc02.msgbnd.dcx')]))
    for f in msg.Files:
        nm = os.path.basename(str(f.Name))
        if not nm.startswith('PlaceName'):
            continue
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_pn.fmg')
        SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
        fmg = _fr.Invoke(None, Array[Object]([tmp]))
        for e in fmg.Entries:
            t = str(e.Text) if e.Text else ''
            if t and t != '[ERROR]':
                names.setdefault(int(e.ID), t)

    with open(out / 'PlaceName_engus.json', 'w', encoding='utf-8') as f:
        json.dump({str(k): names[k] for k in sorted(names)}, f, ensure_ascii=False, indent=2)
    print(f'  PlaceName_engus: {len(names)} entries')


if __name__ == '__main__':
    main()
