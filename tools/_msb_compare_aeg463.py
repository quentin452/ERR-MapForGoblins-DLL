#!/usr/bin/env python3
"""Direct MSB comparison for AEG463_840_9003 and AEG463_600_9002"""
import sys, io, json, os, tempfile, math
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
sys.path.insert(0, "G:\\Games\\Elden Ring\\ERR saves piece\\MapForGoblins\\tools")

import config
from pathlib import Path
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

_str_type = SysType.GetType('System.String')
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)

def rfb(rm, data, suf='.bin'):
    tmp = os.path.join(tempfile.gettempdir(), '_msb_tmp' + suf)
    if hasattr(data, 'ToArray'):
        SysFile.WriteAllBytes(tmp, data.ToArray())
    else:
        SysFile.WriteAllBytes(tmp, data)
    r = rm.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return r

def read_msb(path):
    return rfb(_msbe_read, SoulsFormats.DCX.Decompress(str(path)), '.msb')

def convert_value(val):
    if val is None:
        return None
    if isinstance(val, (int, float, bool, str)):
        return val
    try:
        t = str(val.GetType().Name)
    except:
        return str(val)
    if t in ('Int32', 'UInt32', 'Int64', 'UInt64', 'Int16', 'UInt16', 'Byte', 'SByte'):
        return int(val)
    if t in ('Single', 'Double'):
        return round(float(val), 4)
    if t == 'Boolean':
        return bool(val)
    if t == 'Vector3':
        return {'X': round(float(val.X), 2), 'Y': round(float(val.Y), 2), 'Z': round(float(val.Z), 2)}
    if t == 'String':
        return str(val)
    return str(val)

def get_props(obj):
    t = obj.GetType()
    flags = BindingFlags.Public | BindingFlags.Instance | BindingFlags.FlattenHierarchy
    props = t.GetProperties(flags)
    result = {}
    for prop in props:
        name = str(prop.Name)
        try:
            val = prop.GetValue(obj)
            result[name] = convert_value(val)
        except:
            pass
    return result

TARGET_NAMES = {"AEG463_840_9003", "AEG463_600_9002"}
TARGET_POS = (-60.96, 27.75, -54.56)
RADIUS = 5.0

def dist_3d(p1, p2):
    return math.sqrt((p1[0]-p2[0])**2 + (p1[1]-p2[1])**2 + (p1[2]-p2[2])**2)

def extract_from_msb(msb, map_name):
    entries = {}
    
    # Check Parts.Assets
    if hasattr(msb, 'Parts') and hasattr(msb.Parts, 'Assets'):
        for part in msb.Parts.Assets:
            name = str(part.Name) if hasattr(part, 'Name') else None
            is_target_name = name in TARGET_NAMES
            
            pos = None
            is_nearby = False
            if hasattr(part, 'Position'):
                pos = (float(part.Position.X), float(part.Position.Y), float(part.Position.Z))
                is_nearby = dist_3d(pos, TARGET_POS) <= RADIUS
            
            if is_target_name or is_nearby:
                key = f"Parts.Assets:{name}"
                props = get_props(part)
                entries[key] = {"collection": "Parts.Assets", **props}
    
    # Check Events
    if hasattr(msb, 'Events'):
        for ev_type_name in dir(msb.Events):
            if ev_type_name.startswith('_'): continue
            try:
                ev_type = getattr(msb.Events, ev_type_name)
                if not hasattr(ev_type, '__iter__'): continue
                for event in ev_type:
                    props = get_props(event)
                    match = False
                    
                    # Check for name reference
                    for pname in TARGET_NAMES:
                        if any(pname in str(v) for v in props.values() if isinstance(v, str)):
                            match = True
                            break
                    
                    # Check position
                    if hasattr(event, 'Position'):
                        pos = (float(event.Position.X), float(event.Position.Y), float(event.Position.Z))
                        if dist_3d(pos, TARGET_POS) <= RADIUS:
                            match = True
                    
                    if match:
                        ev_name = str(event.Name) if hasattr(event, 'Name') else 'unknown'
                        key = f"Events.{ev_type_name}:{ev_name}"
                        entries[key] = {"collection": f"Events.{ev_type_name}", **props}
            except:
                pass
    
    # Check Regions
    if hasattr(msb, 'Regions'):
        for reg_type_name in dir(msb.Regions):
            if reg_type_name.startswith('_'): continue
            try:
                reg_type = getattr(msb.Regions, reg_type_name)
                if not hasattr(reg_type, '__iter__'): continue
                for region in reg_type:
                    match = False
                    if hasattr(region, 'Position'):
                        pos = (float(region.Position.X), float(region.Position.Y), float(region.Position.Z))
                        if dist_3d(pos, TARGET_POS) <= RADIUS:
                            match = True
                    
                    if match:
                        reg_name = str(region.Name) if hasattr(region, 'Name') else 'unknown'
                        key = f"Regions.{reg_type_name}:{reg_name}"
                        props = get_props(region)
                        entries[key] = {"collection": f"Regions.{reg_type_name}", **props}
            except:
                pass
    
    return entries

print("=" * 80)
print("MSB COMPARISON: AEG463_840_9003 & AEG463_600_9002")
print("=" * 80)

err_path = "G:\\Games\\ERRv2.1.2.3\\ERRv2.2.1.2-541-2-2-1-2-1773056755\\ERRv2.2.1.2\\mod\\map\\MapStudio\\m60_41_35_00.msb.dcx"
vanilla_path = "G:\\Steam\\steamapps\\common\\ELDEN RING\\Game\\map\\MapStudio\\m60_41_35_00.msb.dcx"

print(f"\nLoading ERR MSB from {err_path}...")
err_msb = read_msb(err_path)
err_entries = extract_from_msb(err_msb, "m60_41_35_00")

print(f"Loading Vanilla MSB from {vanilla_path}...")
vanilla_msb = read_msb(vanilla_path)
vanilla_entries = extract_from_msb(vanilla_msb, "m60_41_35_00")

print(f"\n=== ERR ENTRIES (target or nearby) ===")
if err_entries:
    for k, v in sorted(err_entries.items()):
        print(f"\n{k}:")
        for pk in sorted(v.keys()):
            if pk != 'collection':
                pv = v[pk]
                if isinstance(pv, (dict, list)):
                    print(f"  {pk}: {json.dumps(pv)}")
                else:
                    print(f"  {pk}: {pv}")
else:
    print("  (none found)")

print(f"\n=== VANILLA ENTRIES (target or nearby) ===")
if vanilla_entries:
    for k, v in sorted(vanilla_entries.items()):
        print(f"\n{k}:")
        for pk in sorted(v.keys()):
            if pk != 'collection':
                pv = v[pk]
                if isinstance(pv, (dict, list)):
                    print(f"  {pk}: {json.dumps(pv)}")
                else:
                    print(f"  {pk}: {pv}")
else:
    print("  (none found)")

print(f"\n=== DIFFERENCES (ERR only, not in vanilla) ===")
err_only = set(err_entries.keys()) - set(vanilla_entries.keys())
if err_only:
    for k in sorted(err_only):
        print(f"\n{k}:")
        v = err_entries[k]
        for pk in sorted(v.keys()):
            if pk != 'collection':
                pv = v[pk]
                if isinstance(pv, (dict, list)):
                    print(f"  {pk}: {json.dumps(pv)}")
                else:
                    print(f"  {pk}: {pv}")
else:
    print("  (none - entities are identical in both versions)")

print("\n" + "=" * 80)
print("ANALYSIS COMPLETE")
print("=" * 80)
