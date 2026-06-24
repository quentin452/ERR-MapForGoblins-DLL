#!/usr/bin/env python3
"""Pin the EntityGroupIDs offset inside the MSBE part entity sub-struct (ptr @
part+0x60). Scans DummyAssets across a few areas, finds ones whose SoulsFormats
EntityGroupIDs has a non-zero entry, and locates that value in the raw struct."""
import sys, io, os, tempfile, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pythonnet import load
load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
import SoulsFormats

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str = SysType.GetType('System.String')
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)

def rd32(b, o): return struct.unpack_from('<I', b, o)[0]
def rd64(b, o): return struct.unpack_from('<Q', b, o)[0]

def walk_sections(buf):
    secs, po = [], rd32(buf, 0x08)
    for _ in range(6):
        entries = rd32(buf, po + 4) - 1
        entry_arr = po + 0x10
        secs.append((entries, entry_arr))
        po = rd64(buf, entry_arr + entries * 8)
    return secs

def raw_part_by_name(buf):
    secs = walk_sections(buf)
    pt_entries, pt_arr = secs[5]
    out = {}
    for i in range(pt_entries):
        pe = rd64(buf, pt_arr + i * 8)
        nm_off = pe + rd64(buf, pe + 0x00)
        s, o = bytearray(), nm_off
        while o + 1 < len(buf):
            c = buf[o] | (buf[o + 1] << 8)
            if c == 0: break
            s.append(c if c < 0x80 else ord('?')); o += 2
        out[s.decode('ascii', 'replace')] = pe
    return out

def read_msb(path):
    data = bytes(SoulsFormats.DCX.Decompress(str(path)).ToArray())
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_pg.msb')
    SysFile.WriteAllBytes(tmp, data)
    msb = _msbe_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return msb, data

def groups(p):
    g = getattr(p, 'EntityGroupIDs', None)
    try: return [int(x) for x in g] if g is not None else []
    except Exception: return []

def main():
    msb_dir = config.require_err_mod_dir() / 'map' / 'MapStudio'
    files = sorted(msb_dir.glob('m1*_00.msb.dcx'))[:40] + \
            sorted(msb_dir.glob('m60_*_00.msb.dcx'))[:30]
    found = 0
    for path in files:
        try: msb, buf = read_msb(path)
        except Exception: continue
        raw = raw_part_by_name(buf)
        for cat in ('DummyAssets', 'Assets', 'Enemies'):
            col = getattr(msb.Parts, cat, None)
            if not col: continue
            for p in col:
                gs = groups(p)
                nz = [g for g in gs if g not in (0, -1)]
                if not nz: continue
                name = str(p.Name)
                pe = raw.get(name)
                if pe is None: continue
                ent = pe + rd64(buf, pe + 0x60)
                # dump entity struct +0x00..0x40
                vals = [rd32(buf, ent + k) for k in range(0, 0x40, 4)]
                # find each non-zero group's offset within the struct
                locs = {}
                for g in nz:
                    for k in range(0, 0x80, 4):
                        if ent + k + 4 <= len(buf) and rd32(buf, ent + k) == (g & 0xffffffff):
                            locs[g] = k; break
                eid = int(getattr(p, 'EntityID', 0) or 0)
                print(f"\n{path.name} {cat} {name} eid={eid} groups={gs}")
                print("  entity struct +0x00..0x3c: " +
                      ' '.join('%d' % v for v in vals))
                print(f"  group value offsets within struct: {locs}")
                found += 1
                if found >= 8:
                    return
    if not found:
        print("no parts with non-zero EntityGroupIDs found in the sampled tiles")

if __name__ == '__main__':
    main()
