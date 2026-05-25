"""Scan eldenring.exe memory for AEG463_840 instances. Mirrors goblin_collected.cpp logic."""
import ctypes
from ctypes import wintypes, c_uint64, c_uint32, c_uint16, c_uint8, c_int32, byref, sizeof, create_string_buffer
import struct

PID = 31100
BASE = 0x7ff70ced0000
RVA_WORLD_GEOM_MAN = 0x3D69BA8
RVA_GEOM_FLAG = 0x3D69D18

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400

k32 = ctypes.windll.kernel32
ReadProcessMemory = k32.ReadProcessMemory
ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
ReadProcessMemory.restype = wintypes.BOOL

h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, PID)
if not h:
    print('OpenProcess failed', ctypes.get_last_error())
    raise SystemExit

def rd(addr, n):
    buf = (ctypes.c_ubyte * n)()
    read = ctypes.c_size_t(0)
    if not ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, byref(read)) or read.value != n:
        return None
    return bytes(buf)

def rd_ptr(addr):
    b = rd(addr, 8)
    if b is None: return None
    return struct.unpack('<Q', b)[0]

def rd_u32(addr):
    b = rd(addr, 4)
    if b is None: return None
    return struct.unpack('<I', b)[0]

def rd_u16(addr):
    b = rd(addr, 2)
    if b is None: return None
    return struct.unpack('<H', b)[0]

def rd_u8(addr):
    b = rd(addr, 1)
    return b[0] if b else None

def rd_f32(addr):
    b = rd(addr, 4)
    if b is None: return None
    return struct.unpack('<f', b)[0]

# Read CSWorldGeomMan pointer
wgm = rd_ptr(BASE + RVA_WORLD_GEOM_MAN)
print(f'CSWorldGeomMan: 0x{wgm:x}')

tree_head = rd_ptr(wgm + 0x18 + 0x08)
tree_size = rd_ptr(wgm + 0x18 + 0x10)
print(f'tree_head=0x{tree_head:x} size={tree_size}')

def get_left(node): return rd_ptr(node + 0)
def get_parent(node): return rd_ptr(node + 8)
def get_right(node): return rd_ptr(node + 0x10)
def get_is_nil(node):
    v = rd_u8(node + 0x19)
    return v != 0 if v is not None else True

root = rd_ptr(tree_head + 8)  # parent-of-head = root

def min_node(node):
    while node and not get_is_nil(node):
        left = get_left(node)
        if not left or get_is_nil(left): break
        node = left
    return node

def next_node(node):
    r = get_right(node)
    if r and not get_is_nil(r):
        return min_node(r)
    p = get_parent(node)
    while p and not get_is_nil(p) and node == get_right(p):
        node = p
        p = get_parent(p)
    return p

# walk inorder
current = min_node(root)
nodes_visited = 0
found_463_840 = []

while current and current != tree_head and not get_is_nil(current) and nodes_visited < 500:
    nodes_visited += 1
    block_id = rd_u32(current + 0x20)
    block_data = rd_ptr(current + 0x28)
    if block_data:
        # geom_ins vector at BlockData+0x288
        vec_begin = rd_ptr(block_data + 0x288 + 8)
        vec_end = rd_ptr(block_data + 0x288 + 0x10)
        if vec_begin and vec_end and vec_end > vec_begin:
            count = (vec_end - vec_begin) // 8
            if count > 10000: count = 10000
            tile_aeg463_840 = []
            for i in range(count):
                geom_ins = rd_ptr(vec_begin + i*8)
                if not geom_ins: continue
                msb_part_ptr = rd_ptr(geom_ins + 0x18 + 0x18 + 0x18)
                if not msb_part_ptr: continue
                name_ptr = rd_ptr(msb_part_ptr)
                if not name_ptr: continue
                name_buf = rd(name_ptr, 64)
                if not name_buf: continue
                # wide char to narrow
                narrow = ''
                for c in range(0, 62, 2):
                    ch = name_buf[c]
                    if ch == 0: break
                    narrow += chr(ch)
                if 'AEG463_840' in narrow:
                    px = rd_f32(msb_part_ptr + 0x20)
                    py = rd_f32(msb_part_ptr + 0x24)
                    pz = rd_f32(msb_part_ptr + 0x28)
                    f263 = rd_u8(geom_ins + 0x263)
                    f26B = rd_u8(geom_ins + 0x26B)
                    alive = bool((f263 & 0x02) and not (f26B & 0x10)) if f263 is not None else None
                    tile_aeg463_840.append((narrow, (px, py, pz), f263, f26B, alive))
            if tile_aeg463_840:
                # decode tile_id
                area = (block_id >> 24) & 0xFF
                gx = (block_id >> 16) & 0xFF
                gz = (block_id >> 8) & 0xFF
                print(f'\ntile m{area:02d}_{gx:02d}_{gz:02d}_00 (block_id=0x{block_id:x}, count={count}):')
                for name, pos, f263, f26B, alive in tile_aeg463_840:
                    f263s = f'0x{f263:02x}' if f263 is not None else 'None'
                    f26Bs = f'0x{f26B:02x}' if f26B is not None else 'None'
                    print(f'  {name}  pos=({pos[0]:.2f},{pos[1]:.2f},{pos[2]:.2f})  +0x263={f263s} +0x26B={f26Bs}  alive={alive}')
                    found_463_840.append((block_id, name))
    current = next_node(current)

print(f'\nVisited {nodes_visited} tiles, found {len(found_463_840)} AEG463_840 instances')

# Also dump GEOF entries for any AEG463 model
print('\n=== GEOF (GeomFlagSaveDataManager) ===')
gf_ptr = rd_ptr(BASE + RVA_GEOM_FLAG)
print(f'gf_ptr=0x{gf_ptr:x}')

tiles_found = 0
consecutive_empty = 0
for off in range(0x08, 0x20000, 16):
    id_val = rd_ptr(gf_ptr + off)
    ptr_val = rd_ptr(gf_ptr + off + 8)
    if id_val is None or ptr_val is None: break
    if id_val == 0 and ptr_val == 0:
        consecutive_empty += 1
        if consecutive_empty > 256: break
        continue
    consecutive_empty = 0
    tile_id = id_val & 0xFFFFFFFF
    area = (tile_id >> 24) & 0xFF
    if area < 0x0A or area > 0x3D: continue
    if ptr_val < 0x10000 or ptr_val > 0x7FFFFFFFFFFF: continue
    tiles_found += 1

    # Read 16-byte header
    header = rd(ptr_val, 16)
    if not header: continue
    countA = struct.unpack('<I', header[8:12])[0]
    countB = struct.unpack('<I', header[0:4])[0]
    if 0 < countA < 100000:
        count = countA
        entries_start = ptr_val + 16
    elif 0 < countB < 100000:
        count = countB
        entries_start = ptr_val + 8
    else: continue

    aeg463_in_tile = []
    for ei in range(count):
        entry = rd(entries_start + ei*8, 8)
        if not entry: break
        entry_flags = entry[1]
        geom_idx = entry[2] | (entry[3] << 8)
        model_hash = struct.unpack('<I', entry[4:8])[0]
        # AEG463_840 model_id = 10000000 + 463*1000 + 840 = 10463840
        if 10463000 <= model_hash <= 10463999:
            aeg463_in_tile.append((entry_flags, geom_idx, model_hash))
    if aeg463_in_tile:
        gx = (tile_id >> 16) & 0xFF
        gz = (tile_id >> 8) & 0xFF
        print(f'  tile m{area:02d}_{gx:02d}_{gz:02d}_00 (id=0x{tile_id:x}): {len(aeg463_in_tile)} AEG463 entries')
        for fl, gi, mh in aeg463_in_tile:
            slot = (gi - 0) * 2 + (1 if fl & 0x80 else 0)  # GEOM_IDX_MIN unknown; print raw
            model_num = mh - 10000000
            group = model_num // 1000
            num = model_num % 1000
            print(f'    AEG{group:03d}_{num:03d} flags=0x{fl:02x} geom_idx={gi} (raw_slot={(gi-0)*2 + (1 if fl & 0x80 else 0)})')
print(f'GEOF: scanned {tiles_found} tiles total')
