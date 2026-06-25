#!/usr/bin/env python3
"""Pin the EMEVD 64-bit binary layout for the runtime C++ parser (loot_emevd_drops).

Dumps the raw header of a real ERR .emevd.dcx + SoulsFormats ground truth (event count,
total instruction count, and the bank-2000 template-award tuples) and then runs a PURE-BYTES
parser, comparing the award list to SoulsFormats. When the pure-bytes parse matches, its
logic is the spec the C++ msbe::parse_emevd must reproduce.

Run: PYTHONUTF8=1 PYTHONPATH="C:/Users/iamacat/AppData/Local/Temp/mfg_aux" \
     MFG_PROFILE=err py -3.14 tools/probe_emevd_format.py
"""
import sys, io, os, struct, tempfile
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pathlib import Path
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
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)

TEMPLATE_EVENTS = {
    90005300:(8,16,20), 90005301:(8,16,20),
    90005860:(16,24,28), 90005861:(16,24,28), 90005880:(16,24,28),
    90005750:(8,16,20), 90005753:(8,16,20),
    90005774:(8,12,16), 90005792:(20,24,28),
    90005632:(8,16,20), 90005110:(8,20,24), 90005390:(8,28,32), 90005555:(8,12,16),
}


def sf_awards(raw):
    """Ground-truth award tuples via SoulsFormats: (eventId, entity, lot)."""
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid())+'_pe.tmp')
    SysFile.WriteAllBytes(tmp, raw)
    em = _emevd_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    nev = 0
    ninstr = 0
    out = []
    for ev in em.Events:
        nev += 1
        for ins in ev.Instructions:
            ninstr += 1
            if int(ins.Bank) != 2000:
                continue
            args = bytes(ins.ArgData)
            if len(args) < 8:
                continue
            eid = struct.unpack_from('<i', args, 4)[0]
            t = TEMPLATE_EVENTS.get(eid)
            if not t:
                continue
            eo, lo, ml = t
            if len(args) < ml:
                continue
            ent = struct.unpack_from('<i', args, eo)[0]
            lot = struct.unpack_from('<i', args, lo)[0]
            if lot > 0 and ent > 0:
                out.append((eid, ent, lot))
    return nev, ninstr, out


def hexdump(b, n=0x90):
    for off in range(0, min(n, len(b)), 16):
        chunk = b[off:off+16]
        h = ' '.join(f'{c:02x}' for c in chunk)
        print(f'  {off:04x}: {h}')


def header_ints(b):
    print('  -- int64 @ each 8-byte slot 0x00..0x80 --')
    for off in range(0, 0x88, 8):
        v = struct.unpack_from('<q', b, off)[0]
        plaus = 'offset?' if 0 < v < len(b) else ('count?' if 0 < v < 100000 else '')
        print(f'    0x{off:02x}: {v:>12}  (0x{v & 0xffffffffffffffff:x})  {plaus}')


def pure_parse(b):
    """Pure-bytes EMEVD parse with the pinned 64-bit layout (the C++ spec).
    Header: eventCount@0x10, eventsOffset@0x18, instrCount@0x20, instrTableOffset@0x28,
            argsOffset@0x78. Event stride 0x30 {id@0,instrCount@8,instrOffset@10}.
            Instruction stride 0x20 {bank@0,id@4,argLen@8,argOffset@10(int32)}."""
    if len(b) < 0x80 or b[:4] != b'EVD\x00':
        return []
    ev_count   = struct.unpack_from('<q', b, 0x10)[0]
    ev_off     = struct.unpack_from('<q', b, 0x18)[0]
    instr_tbl  = struct.unpack_from('<q', b, 0x28)[0]
    arg_off    = struct.unpack_from('<q', b, 0x78)[0]
    EVENT_SZ, INSTR_SZ = 0x30, 0x20
    awards = []
    for i in range(ev_count):
        e = ev_off + i * EVENT_SZ
        instr_count  = struct.unpack_from('<q', b, e + 0x08)[0]
        instr_offset = struct.unpack_from('<q', b, e + 0x10)[0]  # relative to instr table
        base = instr_tbl + instr_offset
        for j in range(instr_count):
            ins = base + j * INSTR_SZ
            bank = struct.unpack_from('<i', b, ins + 0)[0]
            if bank != 2000:
                continue
            alen = struct.unpack_from('<q', b, ins + 0x08)[0]
            aoff = struct.unpack_from('<i', b, ins + 0x10)[0]
            if alen < 8 or aoff < 0:
                continue
            a = arg_off + aoff
            args = b[a:a+alen]
            if len(args) < 8:
                continue
            eid = struct.unpack_from('<i', args, 4)[0]
            t = TEMPLATE_EVENTS.get(eid)
            if not t:
                continue
            eo, lo, ml = t
            if len(args) < ml:
                continue
            ent = struct.unpack_from('<i', args, eo)[0]
            lot = struct.unpack_from('<i', args, lo)[0]
            if lot > 0 and ent > 0:
                awards.append((eid, ent, lot))
    return awards


def main():
    moddir = config.require_err_mod_dir()
    emevd_dir = moddir / 'event'
    # Pick a representative file with known template awards (m60 overworld is rich).
    files = sorted(emevd_dir.glob('*.emevd.dcx'))
    sample = None
    for p in files:
        if 'm60_42_36' in p.name or 'm60' in p.name:
            sample = p
            break
    sample = sample or files[0]
    print(f'Sample: {sample.name}')
    raw = bytes(SoulsFormats.DCX.Decompress(str(sample)).ToArray())
    print(f'Raw size: {len(raw)} bytes, magic={raw[:4]!r}')
    hexdump(raw)
    header_ints(raw)

    nev, ninstr, awards = sf_awards(raw)
    print(f'\nSoulsFormats: {nev} events, {ninstr} instructions, {len(awards)} template awards')

    # ── Validate pure-bytes parse vs SoulsFormats over ALL ERR event files ──
    print('\n=== Validating pure-bytes parse vs SoulsFormats over all event files ===')
    tot_sf = tot_pure = 0
    mismatches = 0
    for p in files:
        try:
            r = bytes(SoulsFormats.DCX.Decompress(str(p)).ToArray())
        except Exception:
            continue
        _, _, sf = sf_awards(r)
        pur = pure_parse(r)
        tot_sf += len(sf)
        tot_pure += len(pur)
        if sorted(sf) != sorted(pur):
            mismatches += 1
            if mismatches <= 8:
                print(f'  MISMATCH {p.name}: SF={len(sf)} pure={len(pur)}')
                only_sf = sorted(set(sf) - set(pur))[:4]
                only_pu = sorted(set(pur) - set(sf))[:4]
                if only_sf: print(f'      only SF:   {only_sf}')
                if only_pu: print(f'      only pure: {only_pu}')
    print(f'\nTOTAL: SoulsFormats={tot_sf} awards, pure-bytes={tot_pure} awards, '
          f'{mismatches} file mismatches')
    print('RESULT:', 'MATCH — layout pinned, port to C++' if mismatches == 0
          else 'NEEDS WORK — fix offsets')

    # ── Mirror the RUNTIME build_disk_emevd_markers chain end-to-end ──
    # parse (pure-bytes) -> dedup by (entity,lot) -> join EntityID to an MSB Enemy part's
    # position -> count markers + how many baked LootSource::Emevd lots get covered.
    print('\n=== Runtime chain reproduction (what loot_emevd_drops will do) ===')
    import json
    allaw = []   # (eventId, entity, lot) across all files
    for p in files:
        try:
            r = bytes(SoulsFormats.DCX.Decompress(str(p)).ToArray())
        except Exception:
            continue
        allaw += pure_parse(r)
    # entity -> any MSB enemy part (position anchor); first wins
    _msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod(
        'Read', BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
        None, Array[SysType]([_str]), None)
    want = {a[1] for a in allaw}
    ent_pos = set()
    for mp in sorted((moddir/'map'/'MapStudio').glob('*.msb.dcx')):
        if mp.name.endswith('_99.msb.dcx'):
            continue
        try:
            tmp = os.path.join(tempfile.gettempdir(), str(os.getpid())+'_pm.tmp')
            SysFile.WriteAllBytes(tmp, SoulsFormats.DCX.Decompress(str(mp)).ToArray())
            msb = _msbe_read.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
        except Exception:
            continue
        for q in (getattr(msb.Parts, 'Enemies', []) or []):
            if int(getattr(q, 'GameEditionDisable', 0) or 0) == 1:
                continue
            eid = int(getattr(q, 'EntityID', 0) or 0)
            if eid in want:
                ent_pos.add(eid)
    # dedup by (entity,lot), keep those whose entity has a position
    seen = set(); markers = []
    for ev, ent, lot in allaw:
        if ent not in ent_pos:
            continue
        k = (ent, lot)
        if k in seen:
            continue
        seen.add(k); markers.append((ent, lot))
    emevd_lots = {lot for _, lot in markers}
    baked = json.load(open('.scratch/entries.json'))
    baked_emevd_lots = {e['lotId'] for e in baked if e.get('src') == 'Emevd'}
    covered = baked_emevd_lots & emevd_lots
    print(f'  parsed awards: {len(allaw)}; unique (entity,lot) with a position: {len(markers)}')
    print(f'  distinct lots placed: {len(emevd_lots)}')
    print(f'  baked Emevd lots: {len(baked_emevd_lots)}; COVERED by EMEVD pass: {len(covered)} '
          f'({len(baked_emevd_lots)-len(covered)} stay baked)')

    # ── Validate mechanism B (event-1200) pure-bytes == the C++ parse_emevd_full path ──
    # Mirrors loot_disk::load_emevd_awards: pure-bytes scan of RunEvent(2000:00,callee=1200)
    # -> flag->lot (common.emevd only) + SetEventFlag(2003:66/69,state=1) events with their
    # entity candidates; join candidates to the MSB enemy entity set (boss-preferred).
    print('\n=== Validate mechanism B (event-1200) pure-bytes reproduction ===')
    def parse_full(b):
        if len(b) < 0x80 or b[:4] != b'EVD\x00': return [], []
        ev_count = struct.unpack_from('<q', b, 0x10)[0]
        ev_off   = struct.unpack_from('<q', b, 0x18)[0]
        instr_tbl= struct.unpack_from('<q', b, 0x28)[0]
        arg_off  = struct.unpack_from('<q', b, 0x78)[0]
        run1200 = []   # (flag, lot)
        setters = []   # (flags[], candidates[])
        for i in range(ev_count):
            e = ev_off + i*0x30
            ic = struct.unpack_from('<q', b, e+0x08)[0]
            io = struct.unpack_from('<q', b, e+0x10)[0]
            base = instr_tbl + io
            flags=[]; cand=[]
            for j in range(ic):
                ins = base + j*0x20
                bank = struct.unpack_from('<i', b, ins+0)[0]
                iid  = struct.unpack_from('<i', b, ins+4)[0]
                alen = struct.unpack_from('<q', b, ins+8)[0]
                aoff = struct.unpack_from('<i', b, ins+0x10)[0]
                if aoff < 0 or alen < 4: continue
                a = arg_off + aoff
                for k in range(0, alen-3, 4):
                    v = struct.unpack_from('<I', b, a+k)[0]
                    if 1000 <= v <= 0x7fffffff: cand.append(v)
                if alen < 8: continue
                eid = struct.unpack_from('<i', b, a+4)[0]
                if bank == 2000 and iid == 0 and eid == 1200 and alen >= 16:
                    fl = struct.unpack_from('<i', b, a+8)[0]; lo = struct.unpack_from('<i', b, a+12)[0]
                    if fl > 0 and lo > 0: run1200.append((fl, lo))
                elif bank == 2003 and iid in (66, 69) and alen >= 12:
                    fl = struct.unpack_from('<i', b, a+4)[0]; st = struct.unpack_from('<i', b, a+8)[0]
                    if st == 1 and fl > 0: flags.append(fl)
            if flags: setters.append((flags, cand))
        return run1200, setters
    flag_to_lot = {}; all_setters = []
    for p in files:
        try: r = bytes(SoulsFormats.DCX.Decompress(str(p)).ToArray())
        except Exception: continue
        run1200, setters = parse_full(r)
        if p.name == 'common.emevd.dcx':
            for fl, lo in run1200: flag_to_lot[fl] = lo
        all_setters += setters
    # all MSB enemy entity ids (the runtime's knownEntities)
    known = set(ent_pos)  # ent_pos already collected above is only award-referenced; rebuild full set
    known = set()
    for mp in sorted((moddir/'map'/'MapStudio').glob('*.msb.dcx')):
        if mp.name.endswith('_99.msb.dcx'): continue
        try:
            tmp = os.path.join(tempfile.gettempdir(), str(os.getpid())+'_pk.tmp')
            SysFile.WriteAllBytes(tmp, SoulsFormats.DCX.Decompress(str(mp)).ToArray())
            msb = _msbe_read.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
        except Exception: continue
        for q in (getattr(msb.Parts, 'Enemies', []) or []):
            if int(getattr(q, 'GameEditionDisable', 0) or 0) == 1: continue
            eid = int(getattr(q, 'EntityID', 0) or 0)
            if eid > 0: known.add(eid)
    seenB = set(); ev1200 = []
    for flags, cand in all_setters:
        for fl in flags:
            if fl not in flag_to_lot: continue
            lot = flag_to_lot[fl]
            boss = sorted(c for c in cand if c in known and 800 <= c % 1000 <= 899)
            other = sorted(c for c in cand if c in known)
            chosen = boss[0] if boss else (other[0] if other else 0)
            if not chosen: continue
            k = (chosen, lot)
            if k in seenB: continue
            seenB.add(k); ev1200.append((chosen, lot))
    ev1200_lots = {l for _, l in ev1200}
    print(f'  pure-bytes event-1200: {len(ev1200)} unique (entity,lot); {len(ev1200_lots)} distinct lots')
    print(f'  of the baked Emevd residual, covered by event-1200: '
          f'{len((baked_emevd_lots - emevd_lots) & ev1200_lots)} '
          f'(datamine SoulsFormats reference = 55)')

if __name__ == '__main__':
    main()
