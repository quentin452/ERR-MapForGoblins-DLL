#!/usr/bin/env python3
"""Measure the FLAG axis of the progression graph (companion to _probe_progression_gates.py).

_probe_progression_gates answered the ITEM axis: 19 distinct item requirements gate the world,
stable across ERR and Convergence -> flat and derivable. This asks the other half, the one that
gives the graph its DEPTH: how many distinct EVENT FLAGS gate *traversal* (a door/lift enabling,
a barrier vanishing, a warp firing)? And how many of those are boss-clear flags, i.e. how much of
the depth is the boss chain?

Method, all derived from the ACTIVE install (no bake, no table, no hardcoded bank:id — the
instructions are located by NAME in the EMEDF, so a mod that renumbers nothing still resolves):
  * traversal actions = Set ObjAct State / Change Asset Enable State / the Warp family
  * an event containing one is "a traversal event"; every Event-Flag test inside it is a gate
  * caller-supplied flag ids are resolved through InitializeEvent/InitializeCommonEvent args
  * boss flags = WorldMapPointParam field-boss rows (textId2 == 5100) -> clearedEventFlagId

Run from tools/ (cwd needs oo2core):
  MFG_PROFILE=err py -3 _probe_flag_gates.py
"""
import sys, io, os, json, struct, tempfile, re
from collections import Counter, defaultdict
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
import extract_all_items as E

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str_type = SysType.GetType('System.String')
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)


def load_emevd(path):
    data = SoulsFormats.DCX.Decompress(str(path)).ToArray()
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_mfg_flag.tmp')
    SysFile.WriteAllBytes(tmp, data)
    e = _emevd_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return e


SIZE = {0: 1, 1: 2, 2: 4, 3: 1, 4: 2, 5: 4, 6: 4, 8: 4}
FMT = {0: 'B', 1: 'H', 2: 'I', 3: 'b', 4: 'h', 5: 'i', 6: 'f', 8: 'I'}

# ── EMEDF: locate instructions by NAME, keep the byte span of the arg we want ──
EMEDF = json.load(open(config.TOOLS_DIR / 'er-common.emedf.json', encoding='utf-8'))
FLAG_TEST_RE = re.compile(r'^(IF|SKIP IF|END IF|GOTO IF|WAIT For) Event Flag$')
TRAVERSAL_NAMES = {
    'Set ObjAct State',                 # doors, lifts, ladders: the interaction itself
    'Change Asset Enable State',        # barriers / fog-wall assets appearing or vanishing
    'Warp Player',
    'Play Cutscene to Player and Warp',
    'Play Cutscene to Player and Warp with Weather and Time',
    'Play Cutscene to Player and Warp with Stable Position Update',
}
# A flag TESTED by an event that also SETS it is bookkeeping ("this door is already open"),
# one per instance — a consequence, not a prerequisite. Only externally-set flags are real
# graph edges, so the writes have to be subtracted.
WRITE_NAMES = {'Set Event Flag', 'Set Network-Connected Event Flag'}
# WorldMapPointParam's clearedEventFlagId is the MAP-ICON flag, NOT the kill flag the EMEVD
# actually sets and tests (Godrick: 510010 vs 10000800) — classifying gates against it
# undercounts the boss chain badly. Derive the kill flags instead: any flag SET by an event
# that also announces a boss/miniboss defeat, or tests a character's death.
DEATH_NAMES = {'Handle Boss Defeat and Display Banner', 'Handle Miniboss Defeat',
               'IF Character Dead/Alive'}
DEATH_INSTR = {}     # (bank, idx) -> name
FLAG_TESTS = {}      # (bank, idx) -> (name, span_of_flag_arg)
FLAG_WRITES = {}     # (bank, idx) -> span_of_flag_arg
TRAVERSAL = {}       # (bank, idx) -> name


def arg_span(args, want):
    off = None
    cur = 0
    for a in args:
        sz = SIZE[a['type']]
        cur = (cur + sz - 1) // sz * sz
        if a['name'] == want:
            off = (cur, sz, a['type'])
        cur += sz
    return off


for mc in EMEDF['main_classes']:
    for ins in mc['instrs']:
        key = (mc['index'], ins['index'])
        if FLAG_TEST_RE.match(ins['name']):
            sp = arg_span(ins['args'], 'Target Event Flag ID')
            if sp:
                FLAG_TESTS[key] = (ins['name'], sp)
        elif ins['name'] in TRAVERSAL_NAMES:
            TRAVERSAL[key] = ins['name']
        elif ins['name'] in WRITE_NAMES:
            sp = arg_span(ins['args'], 'Target Event Flag ID')
            if sp:
                FLAG_WRITES[key] = sp
        if ins['name'] in DEATH_NAMES:
            DEATH_INSTR[key] = ins['name']


def main():
    src = config.require_err_mod_dir()
    files = sorted((Path(src) / 'event').glob('*.emevd.dcx'))
    print(f'profile={config.PROFILE}  src={src}')
    print(f'{len(files)} EMEVDs; {len(FLAG_TESTS)} flag-test instrs, {len(TRAVERSAL)} traversal instrs')

    gate_flags = Counter()             # flag -> times it gates a traversal event (external)
    self_gated = Counter()             # tested AND set by the same event = bookkeeping
    maps_of = defaultdict(set)
    trav_events = 0
    trav_by_kind = Counter()
    trav_events_with_no_flag = 0
    death_flags = set()                # flags set by a boss-defeat / character-death event
    pending = []                       # caller-supplied flag ids
    callers = defaultdict(list)
    all_flag_tests = 0                 # every flag test anywhere, for scale

    for p in files:
        try:
            emevd = load_emevd(p)
        except Exception:
            continue
        map_name = p.name.replace('.emevd.dcx', '')
        for evt in emevd.Events:
            evt_id = int(evt.ID)
            par = defaultdict(list)
            for prm in evt.Parameters:
                par[int(prm.InstructionIndex)].append(
                    (int(prm.TargetStartByte), int(prm.ByteCount), int(prm.SourceStartByte)))
            insts = list(evt.Instructions)
            kinds = set()
            flags_here, pend_here, written = [], [], set()
            is_death_evt = False
            for i, inst in enumerate(insts):
                key = (int(inst.Bank), int(inst.ID))
                ab = bytes(inst.ArgData) if inst.ArgData else b''
                if key in ((2000, 0), (2000, 6)):
                    if len(ab) >= 8:
                        callee = struct.unpack_from('<I', ab, 4)[0]
                        callers[('__common__' if key == (2000, 6) else map_name, callee)].append(ab)
                    continue
                if key in DEATH_INSTR:
                    is_death_evt = True
                if key in TRAVERSAL:
                    kinds.add(TRAVERSAL[key])
                elif key in FLAG_WRITES:
                    off, sz, ty = FLAG_WRITES[key]
                    if off + sz <= len(ab):
                        written.add(struct.unpack_from('<' + FMT[ty], ab, off)[0])
                elif key in FLAG_TESTS:
                    all_flag_tests += 1
                    _, (off, sz, ty) = FLAG_TESTS[key]
                    if off + sz > len(ab):
                        continue
                    srcs = [s2 for s, n, s2 in par.get(i, ())
                            if off < s + n and s < off + sz]
                    if srcs:
                        pend_here.append(srcs[0])
                    else:
                        v = struct.unpack_from('<' + FMT[ty], ab, off)[0]
                        if v > 0:
                            flags_here.append(v)
            if is_death_evt:
                death_flags.update(f for f in written if f > 0)
            if not kinds:
                continue
            trav_events += 1
            for k in kinds:
                trav_by_kind[k] += 1
            if not flags_here and not pend_here:
                trav_events_with_no_flag += 1
            for f in flags_here:
                if f in written:
                    self_gated[f] += 1     # bookkeeping: set by the very event that reads it
                    continue
                gate_flags[f] += 1
                maps_of[f].add(map_name)
            for s in pend_here:
                pending.append({'map': map_name, 'event': evt_id, 'src': s})

    # resolve caller-supplied flag ids (initializer args = [slot, eventId, params...])
    resolved = unresolved = 0
    for g in pending:
        blobs = list(callers.get((g['map'], g['event']), []))
        blobs += callers.get(('__common__', g['event']), [])
        got = False
        for ab in blobs:
            off = 8 + g['src']
            if off + 4 > len(ab):
                continue
            v = struct.unpack_from('<I', ab, off)[0]
            if 0 < v < 2_000_000_000:
                gate_flags[v] += 1
                maps_of[v].add(g['map'])
                got = True
        resolved += got
        unresolved += (not got)

    # ── boss-clear flags, from the loaded regulation ──
    reg = SoulsFormats.SFUtil.DecryptERRegulation(str(Path(src) / 'regulation.bin'))
    wmp = E.param_to_dict(E.read_param(reg, 'WorldMapPointParam', E.load_paramdefs()),
                          {'textId2', 'clearedEventFlagId'})
    boss_flags = {int(r['clearedEventFlagId']) for r in wmp.values()
                  if int(r['textId2']) == 5100 and int(r['clearedEventFlagId']) > 0}

    print(f'\nflag tests anywhere in the game      : {all_flag_tests}   (the noise floor)')
    print(f'traversal events (door/asset/warp)   : {trav_events}')
    print('  by action: ' + ', '.join(f'{k}={n}' for k, n in trav_by_kind.most_common()))
    print(f'  ...of those, gated by NO flag      : {trav_events_with_no_flag}'
          f'  ({100*trav_events_with_no_flag//max(trav_events,1)}% ungated = free edges)')
    print(f'  caller-supplied flag id resolved   : {resolved}  (unresolved {unresolved})')
    print(f'  self-gated (tested+set by the same event, = bookkeeping, dropped): {len(self_gated)}')
    print(f'\n★ DISTINCT EXTERNAL FLAGS GATING TRAVERSAL : {len(gate_flags)}')
    print(f'  boss-clear flags in the game       : {len(boss_flags)}')
    print(f'  ...that gate traversal             : {len(set(gate_flags) & boss_flags)}'
          f'   (WMP icon-flag family — the WRONG one, see DEATH below)')
    print(f'\nflags set by a boss-defeat/death event : {len(death_flags)}')
    inter = set(gate_flags) & death_flags
    print(f'  ...that gate traversal                : {len(inter)}   <-- BOSS-CHAIN DEPTH')

    # A per-instance door-state flag shows up once, in one map. The head of the
    # distribution is where the shared, structural gates live.
    tail = [f for f, n in gate_flags.items() if n == 1 and len(maps_of[f]) == 1]
    print(f'\ngate distribution: {len(tail)} of {len(gate_flags)} appear ONCE in ONE map'
          f' (per-instance state) -> structural head = {len(gate_flags) - len(tail)}')

    print(f'\ntop traversal gates\n{"flag":>12} {"uses":>5} {"maps":>5}  kind')
    for f, n in gate_flags.most_common(30):
        kind = 'DEATH' if f in death_flags else ('wmp-boss' if f in boss_flags else '')
        print(f'{f:>12} {n:>5} {len(maps_of[f]):>5}  {kind}')

    out = Path(tempfile.gettempdir()) / f'flag_gates_{config.PROFILE}.json'
    out.write_text(json.dumps(
        {'gates': {str(f): {'uses': n, 'maps': sorted(maps_of[f]), 'boss': f in boss_flags}
                   for f, n in gate_flags.items()}}, indent=1), encoding='utf-8')
    print(f'\nfull list -> {out}')


if __name__ == '__main__':
    main()
