#!/usr/bin/env python3
"""OFFLINE PROTOTYPE of the mod-agnostic quest-NPC extractor.

Mines the ENGINE-STANDARD common-event templates that wire every quest NPC,
instead of hand-decoding each NPC:

  InitializeCommonEvent(0, 90005702, ENTITY, CONCLUDED_FLAG, REG_LO, REG_HI)
      90005702 = the shared NPC "_q99 concluded / died" handler.

Group the calls by (concluded_flag, coarse register): each group == one quest NPC,
its entity list == all that NPC's map placements. Then:
  * FINE register  = the progression state register -- the 2nd BatchSet(lo,hi,OFF)
    block inside the SAME resolver $Event() in common.emevd that owns the coarse
    register (Alexander 3665-3671, Boc 3945-3949, Thops 3805-3806).
  * NAME           = entity -> NPCParamID (MSB, only the maps that hold quest
    entities, via data/msb_entity_index.json) -> nameId (npc_name_id_map.json) ->
    text (npc_name_text_map.json). Cached to data/entity_name_cache.json.

This is what a RUNTIME version would do by parsing the ACTIVE install's .emevd.dcx
(via the DLL's dvdbnd reader) + reading MSB NPCParamID -- automatically correct for
any mod, NOT an ERR-frozen bake.

Usage: py extract_quest_npcs.py            # table (names from cache if present) + validate
       py extract_quest_npcs.py --names    # (re)build the entity->name cache via SoulsFormats
"""
import re, sys, io, json
from pathlib import Path
from collections import defaultdict, OrderedDict
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

REPO = Path(__file__).resolve().parent.parent
JS = Path(r'D:/tools/emevd_js/err')
NAME_CACHE = REPO / 'data' / 'entity_name_cache.json'

CALL = re.compile(r'Initialize(?:Common)?Event\(\s*\d+\s*,\s*90005702\s*,\s*([\d,\s\-]+?)\)')
BATCH = re.compile(r'BatchSet\w*EventFlags?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*OFF')
SETON = re.compile(r'Set\w*EventFlag\w*\(\s*(\d+)\s*,\s*ON')
EVHDR = re.compile(r'\$Event\(\s*(\d+)')


def extract_npcs():
    npc = OrderedDict()  # (concluded, lo, hi) -> {'entities': set}
    for p in sorted(JS.glob('*.emevd.dcx.js')):
        for line in p.read_text(encoding='utf-8', errors='replace').splitlines():
            for m in CALL.finditer(line):
                a = [int(x) for x in m.group(1).split(',') if x.strip().lstrip('-').isdigit()]
                if len(a) >= 4:
                    key = (a[1], a[2], a[3])  # concluded, coarse_lo, coarse_hi
                    npc.setdefault(key, {'entities': set()})['entities'].add(a[0])
    return npc


def fine_registers():
    """Map a coarse (lo,hi) -> fine (lo,hi) via the resolver $Event in common.emevd."""
    com = (JS / 'common.emevd.dcx.js').read_text(encoding='utf-8', errors='replace')
    events = []
    for chunk in re.split(r'(?=\$Event\()', com):
        m = EVHDR.match(chunk)
        if m:
            blocks = [(int(a), int(b)) for a, b in BATCH.findall(chunk)]
            events.append((int(m.group(1)), blocks, chunk))
    out = {}
    for evid, blocks, txt in events:
        uniq = list(dict.fromkeys(blocks))
        for (lo, hi) in uniq:
            # fine register = the block CONTIGUOUS just above the coarse one (same
            # resolver event): Alexander 3660-3663 -> 3665-3671 (gap 2), Boc 3940-3944
            # -> 3945-3949 (gap 1). Bound the gap so an unrelated higher block in a
            # shared event isn't mismatched (e.g. merchants' 4810-4813).
            cands = [b for b in uniq if 0 < b[0] - hi <= 8]
            fine = min(cands, key=lambda b: b[0]) if cands else None
            out[(lo, hi)] = (evid, fine)
    return out


def build_name_cache():
    import config
    from pythonnet import load; load('coreclr')
    import clr; clr.AddReference(str(config.SOULSFORMATS_DLL))
    from System.Reflection import Assembly, BindingFlags
    from System import Array, Type as SysType, Object
    from System.IO import File as SysFile
    import SoulsFormats, os, tempfile
    asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
    _str = SysType.GetType('System.String')
    _msbe = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
        BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy, None, Array[SysType]([_str]), None)
    _seq = [0]
    def read_msb(path):
        _seq[0] += 1
        t = os.path.join(tempfile.gettempdir(), '%d_qx%d.msb' % (os.getpid(), _seq[0]))
        SysFile.WriteAllBytes(t, SoulsFormats.DCX.Decompress(str(path)).ToArray())
        m = _msbe.Invoke(None, Array[Object]([t]))
        try: os.unlink(t)
        except OSError: pass
        return m

    g = config.require_game_dir()
    idx = json.load(open(REPO / 'data' / 'msb_entity_index.json', encoding='utf-8'))
    param2nid = {int(k): int(v) for k, v in json.load(open(REPO / 'data' / 'npc_name_id_map.json', encoding='utf-8')).items()}
    nid2text = json.load(open(REPO / 'data' / 'npc_name_text_map.json', encoding='utf-8'))

    wanted = {e for info in extract_npcs().values() for e in info['entities']}
    by_map = defaultdict(set)
    for e in wanted:
        info = idx.get(str(e))
        if info: by_map[info['map']].add(e)
    ent2name = {}
    for mp, ents in sorted(by_map.items()):
        path = g / 'map' / 'MapStudio' / (mp + '.msb.dcx')
        if not path.exists(): continue
        try: msb = read_msb(path)
        except Exception: continue
        for part in msb.Parts.Enemies:
            eid = int(getattr(part, 'EntityID', 0) or 0)
            if eid in ents:
                param = int(getattr(part, 'NPCParamID', 0) or 0)
                nid = param2nid.get(param, 0)
                ent2name[str(eid)] = nid2text.get(str(nid), '') or ('param%d' % param)
    NAME_CACHE.write_text(json.dumps(ent2name, ensure_ascii=False, indent=0), encoding='utf-8')
    print('wrote %d entity names -> %s' % (len(ent2name), NAME_CACHE.relative_to(REPO)))


def main():
    if '--names' in sys.argv:
        build_name_cache(); return
    npc = extract_npcs()
    fine = fine_registers()
    names = json.load(open(NAME_CACHE, encoding='utf-8')) if NAME_CACHE.exists() else {}

    def name_of(info):
        for e in sorted(info['entities']):
            if names.get(str(e)): return names[str(e)]
        return '?'

    print('scanned %d EMEVD files | quest NPCs: %d | names: %s\n' % (
        len(list(JS.glob('*.emevd.dcx.js'))), len(npc), 'cached' if names else 'run --names'))
    print('%-26s %9s %11s %11s %5s' % ('name', 'concluded', 'coarse', 'fine(prog)', '#pl'))
    print('-' * 72)
    rows = sorted(npc.items(), key=lambda kv: name_of(kv[1]).lower())
    for (concluded, lo, hi), info in rows:
        evid, f = fine.get((lo, hi), (None, None))
        fs = '%d-%d' % (f[0], f[1]) if f else '-'
        print('%-26s %9d %6d-%-4d %11s %5d' % (name_of(info)[:26], concluded, lo, hi, fs, len(info['entities'])))

    print('\n=== validation vs manual mappings ===')
    for name, key in {'Boc': (3943, 3940, 3944), 'Thops': (3803, 3800, 3803), 'Alexander': (3663, 3660, 3663)}.items():
        info = npc.get(key); evid, f = fine.get(key[1:], (None, None))
        ok = 'OK' if info else 'MISSING'
        print('  %-10s concluded=%d coarse=%d-%d fine=%s -> %s (%d placements)' % (
            name, key[0], key[1], key[2], ('%d-%d' % f if f else '-'), ok, len(info['entities']) if info else 0))


if __name__ == '__main__':
    main()
