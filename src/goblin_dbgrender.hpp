#pragma once
// goblin_dbgrender — throwaway probe for greybox job #2a (windows_debug_render_flag_re_findings.md).
// Reads/writes ER's debug-draw gate DAT_143d85b18 (er+0x3d85b18 — the CSDbgDispStep run gate) to test
// whether flipping it makes the engine draw its own debug primitives / collision. READ FIRST: the gate is
// tested as a qword (`test rax,rax`), so it may be a POINTER (writing 1 would crash on deref) rather than a
// bool — inspect the value before writing.
#include <string>
#include <cstdint>

namespace goblin::dbgrender
{
    std::string probe_read();          // dump DAT_143d85b18 + the live hknpWorld ptr, classified
    bool probe_write(uint64_t value);  // SEH write DAT_143d85b18 (guarded)
    uint64_t last_read();

    std::string dump(uintptr_t rva, size_t len);   // hex dump er+rva (read-only; inspect a struct)
    std::string find_hkdbg();                       // locate the CSHkDebugDisp instance + dump its head
    // Flip byte at er+rva to `val` (SEH). For the bounded enable hunt: only flip currently-0 bytes.
    bool poke8(uintptr_t rva, uint8_t val);
    uint8_t peek8(uintptr_t rva);
}
