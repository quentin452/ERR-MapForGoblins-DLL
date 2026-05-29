"""Minimal minidump parser: extract exception record (code + faulting
address) and resolve which loaded module it falls in. No debugger needed.

Usage: py _parse_minidump.py <path-to-.dmp>
"""
import sys, struct, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

def describe_code(c):
    return {
        0xC0000005: 'ACCESS_VIOLATION',
        0xC00000FD: 'STACK_OVERFLOW',
        0x80000003: 'BREAKPOINT',
        0xC000001D: 'ILLEGAL_INSTRUCTION',
        0xC0000409: 'STACK_BUFFER_OVERRUN',
        0xE06D7363: 'C++ exception',
    }.get(c, 'unknown')

path = sys.argv[1] if len(sys.argv) > 1 else None
if not path:
    print("usage: _parse_minidump.py <dump>"); sys.exit(1)

with open(path, 'rb') as f:
    data = f.read()

# MINIDUMP_HEADER
sig, ver, nstreams, stream_rva = struct.unpack_from('<4sIII', data, 0)
if sig != b'MDMP':
    print(f'Not a minidump (sig={sig})'); sys.exit(1)
print(f'minidump: {nstreams} streams')

# Stream directory: array of MINIDUMP_DIRECTORY { u32 type, u32 size, u32 rva }
streams = {}
for i in range(nstreams):
    stype, ssize, srva = struct.unpack_from('<III', data, stream_rva + i * 12)
    streams[stype] = (ssize, srva)

# Stream type ids
EXCEPTION_STREAM = 6
MODULE_LIST = 4
SYSTEM_INFO = 7

# --- Module list ---
modules = []  # (base, size, name)
if MODULE_LIST in streams:
    _, rva = streams[MODULE_LIST]
    count = struct.unpack_from('<I', data, rva)[0]
    off = rva + 4
    for i in range(count):
        # MINIDUMP_MODULE: u64 BaseOfImage, u32 SizeOfImage, u32 CheckSum,
        # u32 TimeDateStamp, u32 ModuleNameRva, ... (total 108 bytes)
        base, size = struct.unpack_from('<QI', data, off)
        name_rva = struct.unpack_from('<I', data, off + 0x14)[0]
        # MINIDUMP_STRING: u32 length (bytes), then UTF-16LE
        slen = struct.unpack_from('<I', data, name_rva)[0]
        name = data[name_rva+4:name_rva+4+slen].decode('utf-16-le', errors='replace')
        modules.append((base, size, name))
        off += 108

def resolve(addr):
    for base, size, name in modules:
        if base <= addr < base + size:
            short = name.split('\\')[-1]
            return f'{short}+0x{addr-base:X}  (base 0x{base:X})'
    return f'0x{addr:X} (no module)'

# --- Exception stream ---
if EXCEPTION_STREAM in streams:
    _, rva = streams[EXCEPTION_STREAM]
    # MINIDUMP_EXCEPTION_STREAM: u32 ThreadId, u32 __align, then MINIDUMP_EXCEPTION
    thread_id = struct.unpack_from('<I', data, rva)[0]
    exc_rva = rva + 8
    # MINIDUMP_EXCEPTION: u32 ExceptionCode, u32 ExceptionFlags,
    # u64 ExceptionRecord, u64 ExceptionAddress, u32 NumberParameters, ...
    code, flags, rec, addr, nparams = struct.unpack_from('<IIQQI', data, exc_rva)
    params = []
    for i in range(min(nparams, 15)):
        p = struct.unpack_from('<Q', data, exc_rva + 0x20 + i * 8)[0]
        params.append(p)
    print(f'\n=== EXCEPTION (thread {thread_id}) ===')
    print(f'  code  = 0x{code:08X}  ({describe_code(code)})')
    print(f'  flags = 0x{flags:08X}')
    print(f'  faulting address = {resolve(addr)}')
    if code == 0xC0000005 and len(params) >= 2:
        rw = {0: 'read', 1: 'write', 8: 'execute'}.get(params[0], f'op{params[0]}')
        print(f'  access violation: {rw} at 0x{params[1]:X}')
    print(f'  params = {[hex(p) for p in params]}')
else:
    print('\nNo exception stream.')

print('\n=== Loaded modules (relevant) ===')
for base, size, name in modules:
    short = name.split('\\')[-1].lower()
    if any(k in short for k in ('eldenring', 'mapforgoblins', 'ersc', 'erquest')):
        print(f'  0x{base:012X} +0x{size:08X}  {name.split(chr(92))[-1]}')
