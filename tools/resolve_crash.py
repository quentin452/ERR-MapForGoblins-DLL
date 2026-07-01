#!/usr/bin/env python3
"""Symbolize a MapForGoblins crash triage (.txt) offline.

The in-game handler already tries to symbolize via dbghelp (needs the .pdb
deployed next to the DLL); this script is the fallback/offline path — run it on
any MapForGoblins_crash_<pid>.txt with the MATCHING build's DLL+PDB pair.

Usage:
    py tools/resolve_crash.py <crash.txt> [--dll build-linux/MapForGoblins.dll]

Requires llvm-symbolizer on PATH and MapForGoblins.pdb next to the given DLL.
eldenring.exe frames have no PDB — those stay raw (Ghidra path, see
docs/memory/tooling/ghidra-re-tooling.md).
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

FAULT_RE = re.compile(r"fault_base\s*=\s*0x[0-9a-fA-F]+\s*\(\+(0x[0-9a-fA-F]+)\)")
FAULT_MOD_RE = re.compile(r"fault_module\s*=\s*(.+)")
STACK_RE = re.compile(r"^(\s*\[\+0x[0-9a-fA-F]+\]\s*)MapForGoblins\.dll \+(0x[0-9a-fA-F]+)")


def symbolize(dll: Path, rvas: list[str]) -> dict[str, str]:
    """rva string -> 'name file:line' via one llvm-symbolizer batch call."""
    if not rvas:
        return {}
    proc = subprocess.run(
        ["llvm-symbolizer", f"--obj={dll}", "--relative-address",
         "--output-style=LLVM", *rvas],
        capture_output=True, text=True, check=True)
    out: dict[str, str] = {}
    # LLVM style: blocks of "name\nfile:line:col" separated by blank lines,
    # in input order (inlined frames stack extra pairs — keep them all).
    blocks = [b.strip() for b in proc.stdout.strip().split("\n\n")]
    for rva, block in zip(rvas, blocks):
        lines = block.splitlines()
        pretty = " <- ".join(
            f"{lines[i]} ({lines[i + 1]})" for i in range(0, len(lines) - 1, 2))
        out[rva] = pretty or "?"
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("crash_txt", type=Path)
    ap.add_argument("--dll", type=Path,
                    default=Path("build-linux/MapForGoblins.dll"))
    args = ap.parse_args()
    text = args.crash_txt.read_text(errors="replace").splitlines()

    # Collect every MapForGoblins RVA (fault address if it's ours + stack lines).
    rvas: list[str] = []
    fault_is_ours = any("MapForGoblins" in (FAULT_MOD_RE.search(l).group(1) if FAULT_MOD_RE.search(l) else "")
                        for l in text)
    fault_rva = None
    for l in text:
        if (m := FAULT_RE.search(l)) and fault_is_ours:
            fault_rva = m.group(1)
            rvas.append(fault_rva)
        elif m := STACK_RE.match(l):
            rvas.append(m.group(2))
    syms = symbolize(args.dll, list(dict.fromkeys(rvas)))

    for l in text:
        if fault_rva and (m := FAULT_RE.search(l)):
            print(f"{l}    ; {syms.get(fault_rva, '?')}")
        elif m := STACK_RE.match(l):
            print(f"{l.rstrip()}    ; {syms.get(m.group(2), '?')}")
        else:
            print(l)
    return 0


if __name__ == "__main__":
    sys.exit(main())
