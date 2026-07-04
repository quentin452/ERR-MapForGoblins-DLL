#!/usr/bin/env python3
# Offline minidump stack walker for the freeze / load-stall / crash dumps the DLL watchdogs write
# (logs/MapForGoblins_{freeze,load_stall,crash}_<pid>.dmp). No dbghelp / Windows needed — runs on
# Linux. Prints each thread's RIP + probable return-address RVAs into eldenring.exe (ER) and
# MapForGoblins.dll (DLL). ER RVAs cross-ref directly to Ghidra FUN_140xxxxxx (imagebase 0x140000000
# → RVA = FUN - 0x140000000). Usage: python3 tools/parse_minidump.py <dump.dmp>
# See docs/memory/tooling/offline-minidump-analysis.md.
import struct, sys

f = open(sys.argv[1], 'rb').read()
sig, ver, nstreams, dirrva = struct.unpack_from('<IIII', f, 0)
assert sig == 0x504d444d, hex(sig)

streams = {}
for i in range(nstreams):
    st, ds, rva = struct.unpack_from('<III', f, dirrva + i*12)
    streams[st] = (ds, rva)

def mdstring(rva):
    ln = struct.unpack_from('<I', f, rva)[0]
    return f[rva+4:rva+4+ln].decode('utf-16le', 'replace')

# Module list (type 4)
mods = []  # (base, size, name)
ds, rva = streams[4]
cnt = struct.unpack_from('<I', f, rva)[0]
p = rva + 4
for i in range(cnt):
    base, size, chk, tds, nrva = struct.unpack_from('<QIIII', f, p)
    name = mdstring(nrva)
    mods.append((base, size, name))
    p += 108

def modof(addr):
    for base, size, name in mods:
        if base <= addr < base+size:
            return (name, addr-base)
    return (None, 0)

er = next((b for b,s,n in mods if n.lower().endswith('eldenring.exe')), None)
dll = next((b for b,s,n in mods if 'mapforgoblins' in n.lower()), None)
print("eldenring.exe base = %#x" % er if er else "NO eldenring.exe")
print("MapForGoblins base = %#x" % dll if dll else "NO dll")
print("modules:", len(mods))

# Thread list (type 3)
ds, rva = streams[3]
cnt = struct.unpack_from('<I', f, rva)[0]
p = rva + 4
print("\nthreads:", cnt)
for i in range(cnt):
    tid, susp, pc, pri, teb, stkstart, stkds, stkrva, ctxds, ctxrva = struct.unpack_from('<IIIIQQIIII', f, p)
    p += 48
    rip = struct.unpack_from('<Q', f, ctxrva + 0xF8)[0]
    rsp = struct.unpack_from('<Q', f, ctxrva + 0x98)[0]
    ripm = modof(rip)
    # only print threads with a resolvable RIP or ER stack content
    stack = f[stkrva:stkrva+stkds]
    # walk stack qwords for probable return addrs into ER / dll
    frames = []
    off = 0
    if rsp >= stkstart and rsp < stkstart+stkds:
        off = rsp - stkstart
    seen = 0
    for a in range(off, len(stack)-8, 8):
        val = struct.unpack_from('<Q', stack, a)[0]
        m, r = modof(val)
        if m and (m.lower().endswith('eldenring.exe') or 'mapforgoblins' in m.lower()):
            tag = 'ER' if m.lower().endswith('eldenring.exe') else 'DLL'
            frames.append("%s+%#x" % (tag, r))
            seen += 1
            if seen >= 30: break
    if ripm[0] or frames:
        print("\n-- thread %#x  RIP=%s+%#x  RSP=%#x" % (
            tid, ripm[0] or '?', ripm[1], rsp))
        # dedupe consecutive
        out=[]
        for fr in frames:
            if not out or out[-1]!=fr: out.append(fr)
        print("   stack:", " ".join(out[:24]))
