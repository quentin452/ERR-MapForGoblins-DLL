"""After a game update, check which known AOBs still resolve in the new
eldenring.exe and report their NEW RVAs. Pure byte scan of the original
MSVC .text (lowest-VA .text). For RIP-relative-loading AOBs, also resolve
the referenced slot RVA.
"""
import sys, io, struct, re
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import pefile

import config  # resolves GAME_DIR from env (.env / .env.local) or config.ini

# eldenring.exe to scan, resolved in order:
#   1. CLI arg          2. GAME_DIR/eldenring.exe (env or config.ini)
#   3. legacy hardcoded default (kept so the script still runs unconfigured)
EXE = (sys.argv[1] if len(sys.argv) > 1
       else str(config.GAME_DIR / 'eldenring.exe') if config.GAME_DIR
       else r'G:\Steam\steamapps\common\ELDEN RING\Game\eldenring.exe')
IMG = 0x140000000
pe = pefile.PE(EXE, fast_load=True)
texts = [s for s in pe.sections if s.Name.rstrip(b'\x00') == b'.text']
texts.sort(key=lambda s: s.VirtualAddress)
t = texts[0]
base_off = t.PointerToRawData
code = pe.__data__[t.PointerToRawData : t.PointerToRawData + t.SizeOfRawData]
TBEG = t.VirtualAddress
print(f'.text RVA 0x{TBEG:X}, size 0x{t.Misc_VirtualSize:X}, exe imgsize 0x{pe.OPTIONAL_HEADER.SizeOfImage:X}')

# (label, AOB string with ?? wildcards, optional rip slot {disp_off, instr_len})
AOBS = [
    ('trampoline ShowTutorialPopup (was 0x80DA50)',
     '48 8B 05 ?? ?? ?? ?? 8B D1 48 85 C0 74 17 48 8B 88 80 00 00 00 48 85 C9', None),
    ('outer ShowTutorialPopup (was 0x7EE630)',
     '48 8B C4 44 88 40 18 89 50 10 55 56 57 41 56 41 57 48 8D 68 A1 48 81 EC', None),
    ('MsgRepository::Lookup (was 0x266D3B0)',
     '3B 51 10 73 ?? 44 3B 41 14 73 ?? 48 8B 41 08 8B D2 48 8B 0C D0', None),
    ('CSMenuMan slot (AOB, modutils)',
     '48 8B 05 ?? ?? ?? ?? 33 DB 48 89 74 24', (3, 7)),
    ('CSFeMan slot (AOB, modutils)',
     '48 8B 05 ?? ?? ?? ?? 48 85 C0 74 11 8B 80 3C 65 00 00', (3, 7)),
    ('FeSystemAnnounce enqueue (was 0x841C50)',
     '48 89 54 24 10 48 83 EC 28 48 8B 41 38 48 FF C0 48 83 F8 0A 73 15 48 83 C1 10 48 8D 54 24 38 E8', None),
]

def to_re(aob):
    parts = aob.split()
    out = b''
    for p in parts:
        out += b'.' if p == '??' else re.escape(bytes([int(p, 16)]))
    return re.compile(out, re.DOTALL)

for label, aob, slot in AOBS:
    rx = to_re(aob)
    hits = [m.start() for m in rx.finditer(code)]
    if not hits:
        print(f'  [MISS] {label}: AOB not found (changed)')
        continue
    if len(hits) > 1:
        print(f'  [WARN] {label}: {len(hits)} matches (ambiguous)')
    off = hits[0]
    rva = TBEG + off
    msg = f'  [OK]   {label}: RVA 0x{rva:X}'
    if slot:
        disp_off, instr_len = slot
        disp = struct.unpack_from('<i', code, off + disp_off)[0]
        slot_rva = rva + instr_len + disp
        msg += f'  -> slot RVA 0x{slot_rva:X}'
    print(msg)
