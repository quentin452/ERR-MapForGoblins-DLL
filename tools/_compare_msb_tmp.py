#!/usr/bin/env python3
import os, sys, json, math
sys.path.insert(0, "G:\\Games\\Elden Ring\\ERR saves piece\\MapForGoblins\\tools")

from pythonnet import load
load("coreclr")
import clr
from System import Array, Object, Type as SysType
from System.Reflection import Assembly, BindingFlags

import config
import shutil
import tempfile

dll_path = "G:\\Games\\Elden Ring\\ERR saves piece\\MapForGoblins\\tools\\lib\\Andre.SoulsFormats.dll"
asm = Assembly.LoadFrom(dll_path)
clr.AddReference(dll_path)
import SoulsFormats

_str_type = SysType.GetType("System.String")
_msb_read = asm.GetType("SoulsFormats.MSBE").GetMethod(
    "Read", BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)

src = "G:\\Steam\\steamapps\\common\\ELDEN RING\\Game\\oo2core_6_win64.dll"
for d in [config.LIB_DIR, tempfile.gettempdir()]:
    dst = os.path.join(str(d), "oo2core_6_win64.dll")
    if not os.path.exists(dst):
        shutil.copy2(src, dst)

TARGET_PARTS = {"AEG463_840_9003", "AEG463_600_9002"}
TARGET_POS = (-60.96, 27.75, -54.56)
POS_RADIUS = 5.0

def dist_3d(p1, p2):
    return math.sqrt((p1[0]-p2[0])**2 + (p1[1]-p2[1])**2 + (p1[2]-p2[2])**2)

def extract_entries(msb):
    results = {}
    
    # Parts.Assets
    if hasattr(msb, 'Parts') and hasattr(msb.Parts, 'Assets'):
        for entry in msb.Parts.Assets:
            if hasattr(entry, 'Name'):
                name = str(entry.Name)
                if name in TARGET_PARTS:
                    d = {"collection": "Parts.Assets", "name": name}
                    if hasattr(entry, 'Position'):
                        d["pos"] = (float(entry.Position.X), float(entry.Position.Y), float(entry.Position.Z))
                    if hasattr(entry, 'EntityID'):
                        d["EntityID"] = int(entry.EntityID)
                    if hasattr(entry, 'InstanceID'):
                        d["InstanceID"] = int(entry.InstanceID)
                    results[name] = d
    
    # Check Events and Regions for references
    if hasattr(msb, 'Events'):
        for event_group_name in dir(msb.Events):
            if event_group_name.startswith('_'):
                continue
            group = getattr(msb.Events, event_group_name)
            if not hasattr(group, 'Count'):
                continue
            
            for event in group:
                # Treasure events
                if hasattr(event, 'PartName') and hasattr(event, 'PartName'):
                    pn = str(event.PartName)
                    if pn in TARGET_PARTS:
                        key = f"{event_group_name}_{str(event.Name) if hasattr(event, 'Name') else 'unknown'}"
                        results[key] = {
                            "collection": f"Events.{event_group_name}",
                            "name": str(event.Name) if hasattr(event, 'Name') else None,
                            "PartName": pn,
                            "EventID": int(event.EventID) if hasattr(event, 'EventID') else None,
                        }
    
    return results

print("Loading ERR MSB...")
err_path = "G:\\Games\\ERRv2.1.2.3\\ERRv2.2.1.2-541-2-2-1-2-1773056755\\ERRv2.2.1.2\\mod\\map\\MapStudio\\m60_41_35_00.msb.dcx"
err_msb = _msb_read.Invoke(None, Array[Object]([err_path]))
err_entries = extract_entries(err_msb)

print("Loading Vanilla MSB...")
vanilla_path = "G:\\Steam\\steamapps\\common\\ELDEN RING\\Game\\map\\MapStudio\\m60_41_35_00.msb.dcx"
vanilla_msb = _msb_read.Invoke(None, Array[Object]([vanilla_path]))
vanilla_entries = extract_entries(vanilla_msb)

print("\n=== ERR ENTRIES ===")
print(json.dumps(err_entries, indent=2, default=str))

print("\n=== VANILLA ENTRIES ===")
print(json.dumps(vanilla_entries, indent=2, default=str))

print("\n=== DIFFERENCES ===")
for key in err_entries:
    if key not in vanilla_entries:
        print(f"ERR only: {key}")
        print(json.dumps(err_entries[key], indent=2, default=str))
