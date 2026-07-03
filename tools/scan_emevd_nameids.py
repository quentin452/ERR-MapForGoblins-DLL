#!/usr/bin/env python3
"""Scan emevd.dcx files for boss-bar name ids (windows_enemy_name_runtime_source RE).

Confirms the vanilla linkage EMEVD HandleBossHealthBar(entity, slot, nameId) -> NpcName:
looks for the raw LE u32 of known NpcName boss ids inside each decompressed emevd,
in both the vanilla install and the ERR overlay.
"""
import sys, io, struct, ctypes
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config

oodle = ctypes.cdll.LoadLibrary(str(config.require_oo2core()))
dec = oodle.OodleLZ_Decompress
dec.restype = ctypes.c_int
dec.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int,
                ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p,
                ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                ctypes.c_int, ctypes.c_int]

def dcx_decompress(data):
    usz = struct.unpack('>I', data[0x1C:0x20])[0]
    csz = struct.unpack('>I', data[0x20:0x24])[0]
    out = ctypes.create_string_buffer(usz)
    n = dec(data[0x4C:0x4C+csz], csz, out, usz, 0, 0, 0, None, 0, None, None, None, 0, 3)
    return bytes(out)[:n]

TARGETS = {
    903251600: 'vanilla NpcName "Tree Sentinel"',
    903251601: 'vanilla NpcName "Tree Sentinel" #2',
    903250600: 'vanilla NpcName "Draconic Tree Sentinel"',
    904311000: 'NpcName "Soldier of Godrick"',
    5763010:   'ERR "Field Boss: Tree Sentinel" (NpcName+ActionButtonText)',
}
NEEDLES = {v: struct.pack('<I', v) for v in TARGETS}

def scan(tag, evdir):
    if not evdir.exists():
        print(f'[{tag}] no event dir at {evdir}'); return
    files = sorted(evdir.glob('*.emevd.dcx'))
    print(f'[{tag}] scanning {len(files)} emevd.dcx in {evdir}')
    for p in files:
        try:
            raw = dcx_decompress(p.read_bytes())
        except Exception as e:
            print(f'[{tag}]  {p.name}: decompress failed ({e})'); continue
        for val, needle in NEEDLES.items():
            off = raw.find(needle)
            if off >= 0:
                n = raw.count(needle)
                print(f'[{tag}]  {p.name}: {val} ({TARGETS[val]}) x{n} first@0x{off:X}')

scan('vanilla', config.GAME_DIR / 'event')
if config.ERR_MOD_DIR and config.ERR_MOD_DIR != config.GAME_DIR:
    scan('err', config.ERR_MOD_DIR / 'event')
