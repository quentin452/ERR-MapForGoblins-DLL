#!/usr/bin/env python3
"""Scout a SAFE game-thread per-frame hook for the heightfield cast (Track D2.2).

Companion to docs/plans/heightfield_relief_plan.md. The cast (FUN_140c70360) must run on the
GAME thread during gameplay, with a CLEAN (non-torn) ctx = *(*(er+0x3d76060)+0x98). The blocker
is finding a hook site that is (1) per-frame, (2) game-thread, (3) with a KNOWN signature so the
detour doesn't crash. This script DE-RISKS that RE from Python over the live debug-RPC — it does
NOT hook (hooking is an in-DLL detour; RPM can't install one). It only reads/observes:

  slot     Pure-RPM proof that the ctx slot (inst+0x98) is WRITTEN every frame + catch torn reads.
           inst = *(er+0x3d76060); the writer of inst+0x98 is BY CONSTRUCTION a per-frame
           game-thread fn that produces the clean ctx -> the ideal hook site.
  fwa      Arm a HW find-what-accesses WRITE bp on inst+0x98 -> the game's own writer RIP + caller
           stack land in logs/MapForGoblins.log as [FWA] lines. That RIP's function = the candidate.
  disasm   mem_dump a prologue (a candidate RIP or any er+RVA) + disassemble it (capstone) and
           SUMMARISE the signature: which int-arg regs (rcx/rdx/r8/r9) + xmm args are used, and
           whether it reads INCOMING STACK args (>4 args -> a mismatched detour CRASHES). Kills the
           "arg count uncertain" risk from commit fc60903 before any in-DLL hook is written.

Port resolves --port -> $MFG_RPC_PORT -> 38700 (same as tools/mfg.py). er_base resolves
--er-base -> the `er_base` RPC verb (if the DLL exposes it) -> error with instructions.

Examples:
    python tools/hf_hook_scout.py slot --samples 120        # confirm per-frame + torn reads
    python tools/hf_hook_scout.py fwa                        # arm write-watch, then play in-world
    python tools/hf_hook_scout.py fwa --log '<mod>/logs/MapForGoblins.log'   # + tail for [FWA]
    python tools/hf_hook_scout.py disasm er+0x3f13c0        # dissect a candidate's signature
    python tools/hf_hook_scout.py disasm er+0xc70360 --len 96   # the cast fn prologue
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mfg_rpc import Rpc  # noqa: E402

# --- anchors (ERR 2.2.9.6 dev box; docs/plans/heightfield_relief_plan.md "Anchors") ---------------
PHYSWORLD_SLOT_RVA = 0x3D76060  # CS::PhysWorld FD4Singleton static slot -> holds the instance ptr
CTX_FIELD_OFF = 0x98            # inst+0x98 = the world-holder ptr the game WRITES every frame (torn)
CASTRAY_RVA = 0xC70360         # the ray-cast primitive (reference prologue)


# ---------------------------------------------------------------------------------- RPM helpers ---
def resolve_er_base(rpc, override):
    if override is not None:
        return override
    reply = rpc.cmd("er_base")
    m = re.search(r"0x[0-9a-fA-F]+", reply)
    if reply.startswith("ok") and m:
        return int(m.group(0), 16)
    sys.exit(
        "error: could not resolve er_base.\n"
        f"  the DLL replied: {reply!r}\n"
        "  Either the `er_base` RPC verb isn't in this build, or you're not in-world.\n"
        "  Fix: pass --er-base 0x<base> (eldenring.exe module base — read it from a prior\n"
        "  [FWA] line's absolute addr, from Cheat Engine, or add the tiny `er_base` verb)."
    )


def mem_read(rpc, abs_addr, length):
    """mem_dump <abs> <len> -> bytes. Reply: 'ok 0xADDR: xx xx ...'."""
    reply = rpc.cmd(f"mem_dump {abs_addr:#x} {length}")
    if not reply.startswith("ok"):
        raise RuntimeError(f"mem_dump {abs_addr:#x} {length} -> {reply}")
    hexpart = reply.split(":", 1)[1]
    return bytes(int(b, 16) for b in hexpart.split())


def read_u64(rpc, abs_addr):
    return int.from_bytes(mem_read(rpc, abs_addr, 8), "little")


def resolve_ctx_slot(rpc, er_base):
    """Return (inst, ctx_slot_addr). ctx_slot_addr = inst+0x98 is the per-frame write target."""
    inst = read_u64(rpc, er_base + PHYSWORLD_SLOT_RVA)
    if inst == 0:
        sys.exit("error: PhysWorld instance is null (*(er+0x3d76060)==0) — not in-world yet?")
    return inst, inst + CTX_FIELD_OFF


# ------------------------------------------------------------------------------------ sub: slot ---
def cmd_slot(rpc, er_base, args):
    inst, slot = resolve_ctx_slot(rpc, er_base)
    print(f"PhysWorld inst = {inst:#x}   ctx slot (inst+0x98) = {slot:#x}")
    print(f"polling {args.samples} samples @ {args.interval*1000:.0f}ms — walk/turn in-world...\n")
    seen, changes, torn, prev = {}, 0, 0, None
    for i in range(args.samples):
        v = read_u64(rpc, slot)
        seen[v] = seen.get(v, 0) + 1
        hi = v >> 32
        is_torn = hi not in (0, 1) or v == 0  # clean heap ptr has hi dword 0/tiny; 0x1_xxxxxxxx = stitched
        if is_torn:
            torn += 1
        if prev is not None and v != prev:
            changes += 1
        prev = v
        if i < 8 or is_torn:
            print(f"  [{i:3}] ctx={v:#018x}{'   <- TORN?' if is_torn else ''}")
        time.sleep(args.interval)
    print(f"\n{'='*60}")
    print(f"distinct values : {len(seen)}   changes across samples: {changes}/{args.samples-1}")
    print(f"torn-looking    : {torn}/{args.samples}")
    if changes > args.samples * 0.3:
        print("=> slot IS written frequently -> its writer is a per-frame game-thread fn.")
        print("   That writer = the ideal hook site (clean ctx by construction). Next: `fwa`.")
    else:
        print("=> slot rarely changed. Either you were static, or off-thread reads mostly caught")
        print("   the same value. Move around and re-run; still flat => reconsider the target.")
    if torn:
        print(f"   {torn} torn read(s) confirm the doc: off-thread reads race the writer -> the cast")
        print("   MUST run on the game thread, not from RPC/present.")


# ------------------------------------------------------------------------------------- sub: fwa ---
def cmd_fwa(rpc, er_base, args):
    inst, slot = resolve_ctx_slot(rpc, er_base)
    rw = "r" if args.read else "w"
    print(f"arming FWA on ctx slot {slot:#x} (len 8, {rw}) — inst+0x98, the per-frame writer.")
    print("WARNING: this is a HOT per-frame target -> expect a brief VEH burst; the handler collects")
    print("         distinct sites then self-disarms (goblin_field_probe.cpp).")
    reply = rpc.cmd(f"mem_fwa {slot:#x} 8 {rw}")
    print(f"  -> {reply}")
    if not reply.startswith("ok"):
        return
    print("\nNow: be IN-WORLD in GAMEPLAY (map closed) and move for a second or two.")
    print("The accessing RIP + caller ret-addrs are logged as [FWA] lines (er-relative) to")
    print("logs/MapForGoblins.log. Take the caller RIP and feed it to: hf_hook_scout.py disasm er+0x<rip>")
    if args.log:
        _tail_fwa(args.log, args.watch)


def _tail_fwa(path, seconds):
    if not os.path.exists(path):
        print(f"\n(--log {path} not found; check the [FWA] lines yourself.)")
        return
    print(f"\ntailing {path} for [FWA] for {seconds}s...")
    deadline = time.monotonic() + seconds
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        f.seek(0, os.SEEK_END)
        while time.monotonic() < deadline:
            line = f.readline()
            if not line:
                time.sleep(0.2)
                continue
            if "[FWA]" in line:
                print("  " + line.rstrip())


# ---------------------------------------------------------------------------------- sub: disasm ---
_ARG_FAMILIES = {
    "rcx": {"rcx", "ecx", "cx", "cl", "ch"},
    "rdx": {"rdx", "edx", "dx", "dl", "dh"},
    "r8": {"r8", "r8d", "r8w", "r8b"},
    "r9": {"r9", "r9d", "r9w", "r9b"},
    "xmm0": {"xmm0"}, "xmm1": {"xmm1"}, "xmm2": {"xmm2"}, "xmm3": {"xmm3"},
}
_INT_ORDER = ["rcx", "rdx", "r8", "r9"]


def _family_of(reg):
    reg = reg.lower()
    for fam, members in _ARG_FAMILIES.items():
        if reg in members:
            return fam
    return None


def cmd_disasm(rpc, er_base, args):
    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64
        from capstone import x86 as _x86  # noqa: F401  (ensure the x86 module is present)
    except ImportError:
        sys.exit("error: capstone not installed. Run:  pip install capstone")

    abs_addr = _parse_target(args.target, er_base)
    code = mem_read(rpc, abs_addr, args.len)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    print(f"disasm {args.target}  (abs {abs_addr:#x}, er+0x{abs_addr - er_base:x}, {args.len} bytes)\n")
    written, used_arg, stack_args = set(), {}, []  # used_arg[family] = insn index of first arg-read
    rsp_adj = 0  # cumulative prologue push/sub so we can locate INCOMING stack args
    for idx, insn in enumerate(md.disasm(code, abs_addr)):
        note = ""
        try:
            regs_read, regs_written = insn.regs_access()
        except Exception:
            regs_read, regs_written = (), ()
        read_fams = set(filter(None, (_family_of(insn.reg_name(r)) for r in regs_read)))
        for fam in read_fams:
            if fam not in written and fam not in used_arg:
                used_arg[fam] = idx
                note += f"  <- reads arg {fam}"
        for r in regs_written:
            fam = _family_of(insn.reg_name(r))
            if fam:
                written.add(fam)

        # track prologue stack adjustment + flag reads of incoming stack args
        if insn.mnemonic == "push":
            rsp_adj += 8
        elif insn.mnemonic == "sub" and insn.op_str.startswith("rsp,"):
            rsp_adj += _imm(insn.op_str)
        elif insn.mnemonic == "add" and insn.op_str.startswith("rsp,"):
            rsp_adj -= _imm(insn.op_str)
        for disp in _stack_mem_reads(insn):
            # incoming arg5 sits at [rsp + rsp_adj + 0x28] (ret + 32B shadow); +8 per further arg
            base = rsp_adj + 0x28
            if disp >= base and (disp - base) % 8 == 0:
                argno = 5 + (disp - base) // 8
                note += f"  <- STACK arg #{argno} ([rsp+{disp:#x}])"
                stack_args.append(argno)

        # the plan's tell-tale virtual dispatch
        if insn.mnemonic in ("mov", "call") and re.search(r"\[r[a-z0-9]+ \+ 0x[0-9a-f]+\]", insn.op_str):
            if insn.mnemonic == "call":
                note += "  <- virtual call [reg+off]"
        print(f"  +{insn.address-abs_addr:03x}  {insn.mnemonic:<7} {insn.op_str}{note}")
        if insn.mnemonic in ("ret", "int3") and idx > 2:
            break

    print(f"\n{'='*60}\nSIGNATURE SUMMARY")
    ints = [f for f in _INT_ORDER if f in used_arg]
    xmms = sorted(f for f in used_arg if f.startswith("xmm"))
    print(f"  int-arg regs used : {', '.join(ints) or '(none)'}"
          + (f"   => >= {len(ints)} integer arg(s)" if ints else ""))
    print(f"  float-arg regs    : {', '.join(xmms) or '(none)'}")
    if stack_args:
        print(f"  INCOMING STACK args detected: #{', #'.join(str(a) for a in sorted(set(stack_args)))}")
        print("  !! >4 args -> the detour MUST match the full ABI or it CRASHES (commit fc60903 risk b).")
    else:
        print("  no incoming stack-arg reads seen in this window -> likely <=4 args (register-only).")
    print("  Verify against the plan's sig `(rcx=obj, rdx=out-vec, r8d, xmm3)` before writing a detour.")

    if args.aob:
        _emit_aob(code, abs_addr, er_base)


def _emit_aob(code, abs_addr, er_base, max_bytes=40, min_insns=6):
    """Build a copy-paste AOB from a FUNC prologue: literal opcode bytes, with rip-relative
    displacements and rel32 branch targets wildcarded (??) since those move per build/ASLR. A FUNC
    target needs NO relative_offsets — a base match IS the function address. Stops at the first ret,
    or once >=max_bytes over >=min_insns (long enough to be unique)."""
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_GRP_JUMP, CS_GRP_CALL
    from capstone.x86 import X86_OP_MEM
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    toks, nbytes, ninsns, imm64 = [], 0, 0, False
    for insn in md.disasm(code, abs_addr):
        b = insn.bytes
        enc = insn.encoding
        wild = set()
        rip_rel = any(op.type == X86_OP_MEM and op.mem.base and insn.reg_name(op.mem.base) == "rip"
                      for op in insn.operands)
        if rip_rel and enc.disp_size:
            wild.update(range(enc.disp_offset, enc.disp_offset + enc.disp_size))
        if (insn.group(CS_GRP_JUMP) or insn.group(CS_GRP_CALL)) and enc.imm_size:
            wild.update(range(enc.imm_offset, enc.imm_offset + enc.imm_size))
        if enc.imm_size == 8:  # movabs imm64: often an absolute address (ASLR) — flag, don't trust
            imm64 = True
        toks += ["??" if k in wild else f"{b[k]:02X}" for k in range(len(b))]
        nbytes += len(b); ninsns += 1
        if insn.mnemonic in ("ret", "int3") and ninsns > 2:
            break
        if nbytes >= max_bytes and ninsns >= min_insns:
            break
    literals = sum(1 for t in toks if t != "??")
    print(f"\n{'='*60}\nAOB (er+0x{abs_addr - er_base:x}, FUNC — no relative_offsets)")
    print(f"  \"{' '.join(toks)}\"")
    print(f"  {len(toks)} bytes, {literals} literal / {len(toks) - literals} wildcard")
    if imm64:
        print("  !! a movabs imm64 is kept LITERAL — if it's an absolute address it will break the")
        print("     AOB on another build; inspect the disasm and wildcard it by hand if so.")
    print("  Next: paste into src/re_signatures.hpp, run the [SIG] health check for PASS + UNIQUE.")


def _imm(op_str):
    m = re.search(r"0x[0-9a-fA-F]+|\d+", op_str.split(",", 1)[1])
    return int(m.group(0), 0) if m else 0


def _stack_mem_reads(insn):
    """Yield displacements of [rsp/rbp + disp] operands that are READ (candidate stack args)."""
    out = []
    try:
        from capstone.x86 import X86_OP_MEM, X86_AC_READ
        for op in insn.operands:
            if op.type != X86_OP_MEM:
                continue
            base = insn.reg_name(op.mem.base) if op.mem.base else ""
            if base in ("rsp", "rbp") and op.mem.disp > 0:
                # only count reads (writes to [rsp+..] are locals/spills, not incoming args)
                if not hasattr(op, "access") or (op.access & X86_AC_READ):
                    out.append(op.mem.disp)
    except Exception:
        pass
    return out


def _parse_target(tok, er_base):
    tok = tok.strip().lower()
    if tok.startswith("er+"):
        return er_base + int(tok[3:], 16)
    val = int(tok, 16) if tok.startswith("0x") else int(tok, 0)
    return val if val > 0x10000000 else er_base + val  # small value => treat as an RVA


# ------------------------------------------------------------------------------------------ main --
def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int,
                    default=int(os.environ.get("MFG_RPC_PORT", "38700")),
                    help="debug_rpc_port (default: $MFG_RPC_PORT or 38700)")
    ap.add_argument("--er-base", type=lambda s: int(s, 0), default=None,
                    help="eldenring.exe module base (hex). Default: ask the `er_base` RPC verb.")
    sub = ap.add_subparsers(dest="sub", required=True)

    p = sub.add_parser("slot", help="pure-RPM: prove ctx slot is written per-frame + catch torn reads")
    p.add_argument("--samples", type=int, default=60)
    p.add_argument("--interval", type=float, default=0.05)

    p = sub.add_parser("fwa", help="arm FWA write-watch on the ctx slot -> writer RIP in [FWA] log")
    p.add_argument("--read", action="store_true", help="watch reads instead of writes")
    p.add_argument("--log", default=None, help="path to MapForGoblins.log to tail for [FWA] lines")
    p.add_argument("--watch", type=float, default=8.0, help="seconds to tail the log")

    p = sub.add_parser("disasm", help="dump+disassemble a prologue and summarise its signature")
    p.add_argument("target", help="er+0x<rva>, a small 0x<rva>, or an absolute 0x<addr>")
    p.add_argument("--len", type=int, default=128, help="bytes to disassemble (<=256)")
    p.add_argument("--aob", action="store_true",
                   help="also emit a copy-paste AOB (rip-rel disps + rel32 branch targets wildcarded) "
                        "for docs/re/rva_aob_hardening_backlog.md")

    args = ap.parse_args()
    with Rpc(args.port) as rpc:
        er_base = resolve_er_base(rpc, args.er_base)
        {"slot": cmd_slot, "fwa": cmd_fwa, "disasm": cmd_disasm}[args.sub](rpc, er_base, args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
