#!/usr/bin/env python3
"""Compute the byte offset of a field (default: iconId) inside a PARAMDEF, by
replicating SoulsFormats' field-layout / bitfield-packing algorithm. Pure stdlib,
no .NET. This is the REFERENCE algorithm to port to C++ for live PARAMDEF reads
(goal: read iconId by field NAME at runtime, no hardcoded offsets, no offline bake).

Usage: paramdef_iconid_offset.py [field_name] [paramdef.xml ...]
"""
import re, sys, glob, os

# byte size + bit capacity per PARAMDEF DisplayType
SIZE = {  # type -> (bytes, bit_limit)
    's8': (1, 8), 'u8': (1, 8), 'dummy8': (1, 8),
    's16': (2, 16), 'u16': (2, 16),
    's32': (4, 32), 'u32': (4, 32), 'f32': (4, 32),
    'f64': (8, 64),
}

FIELD_RE = re.compile(r'Def="([^"]+)"')

def parse_def(defstr):
    """-> (type, name, array_len, bit_size). array_len=1 if scalar; bit_size=-1 if not a bitfield."""
    s = defstr.split('=')[0].strip()        # drop default value
    typ, rest = s.split(None, 1)
    rest = rest.strip()
    bit = -1
    m = re.search(r':\s*(\d+)$', rest)      # bitfield ":N"
    if m:
        bit = int(m.group(1)); rest = rest[:m.start()].strip()
    arr = 1
    m = re.search(r'\[\s*(\d+)\s*\]$', rest)  # array "[N]"
    if m:
        arr = int(m.group(1)); rest = rest[:m.start()].strip()
    name = rest
    return typ, name, arr, bit

def field_offset(xml_path, target):
    text = open(xml_path, encoding='utf-8').read()
    defs = FIELD_RE.findall(text)
    pos = 0
    bit_limit = 0          # capacity of the current bitfield storage unit (0 = none open)
    bit_used = 0           # bits consumed in the current unit
    for d in defs:
        typ, name, arr, bit = parse_def(d)
        if typ not in SIZE:
            return None, f"unknown type {typ!r} in {d!r}"
        nbytes, tlimit = SIZE[typ]
        if bit >= 0:
            # bitfield: pack into current unit if same capacity and it fits, else open new unit
            if bit_limit != tlimit or bit_used + bit > bit_limit:
                pos += 0 if bit_limit == 0 else 0   # prior unit's bytes already counted at open
                unit_off = pos
                pos += nbytes
                bit_limit = tlimit; bit_used = 0
            else:
                unit_off = pos - nbytes              # continue the just-opened unit
            if name == target:
                return unit_off, f"bit {bit_used} (bitfield)"
            bit_used += bit
        else:
            # plain field: close any open bitfield unit (its bytes already counted)
            bit_limit = 0; bit_used = 0
            if name == target:
                return pos, "u%d" % (nbytes * 8)
            pos += nbytes * arr
    return None, "field not found"

def main():
    args = sys.argv[1:]
    target = 'iconId'
    if args and not args[0].endswith('.xml') and not os.path.isdir(args[0]):
        target = args.pop(0)
    paths = []
    for a in args:
        paths += glob.glob(a)
    if not paths:
        base = os.path.join(os.path.dirname(__file__), 'paramdefs')
        for nm in ('EquipParamWeapon', 'EquipParamProtector', 'EquipParamAccessory',
                   'EquipParamGoods', 'EquipParamGem'):
            p = os.path.join(base, nm + '.xml')
            if os.path.exists(p):
                paths.append(p)
    for p in paths:
        off, note = field_offset(p, target)
        nm = os.path.splitext(os.path.basename(p))[0]
        if off is None:
            print(f"{nm:24s} {target}: -- ({note})")
        else:
            print(f"{nm:24s} {target}: 0x{off:X} ({off})  [{note}]")

if __name__ == '__main__':
    main()
