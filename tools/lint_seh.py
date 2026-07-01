#!/usr/bin/env python3
"""Tripwire for the clang-cl SEH-elision hazard.

clang-cl silently ELIDES a `__try` that directly wraps a raw load/store (it
proves a plain deref "can't fault" — UB reasoning), even with noinline on the
containing function. The ONLY reliable shape is `__try` around an opaque CALL
(noinline body / fn pointer / cross-module). History + the shipped patterns:
docs/memory/tooling/clang-cl-seh-noinline.md, docs/plans/clang_only_toolchain_plan.md.

This lint flags any `__try { ... }` block whose body contains a textual raw
deref. Heuristic by design — the goal is to make the silent regression LOUD at
review time, not to be a proof. Run from the repo root:

    python3 tools/lint_seh.py            # exit 1 on findings
"""
import re
import sys
from pathlib import Path

# Textual signs of a raw memory access inside a guarded block. Function CALLS
# are fine (clang keeps the SEH frame around opaque calls) — these patterns
# specifically catch dereference expressions.
DEREF = re.compile(
    r"\*\s*reinterpret_cast"      # *reinterpret_cast<T*>(p)
    r"|=\s*\*\s*\w"               # x = *p
    r"|\*\s*\(\s*\w+\s*\*\s*\)"   # *(T*)p
)


def strip_comments(text: str) -> str:
    """Blank out // and /* */ comments (and string literals) so a `__try` QUOTED in
    a comment doesn't get scanned as code — newlines are preserved to keep line
    numbers stable."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append('"' + " " * (j - i - 2) + '"' if j - i >= 2 else text[i:j])
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def try_blocks(text: str):
    """Yield (line_no, block_text) for each __try { ... } body."""
    text = strip_comments(text)
    for m in re.finditer(r"__try\b", text):
        brace = text.find("{", m.end())
        if brace < 0:
            continue
        depth, i = 0, brace
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        yield text.count("\n", 0, m.start()) + 1, text[brace : i + 1]


def main() -> int:
    findings = []
    for p in sorted(Path("src").rglob("*.cpp")) + sorted(Path("src").rglob("*.hpp")):
        if "generated" in p.parts[1] if len(p.parts) > 1 else False:
            continue
        text = p.read_text(errors="replace")
        for line, body in try_blocks(text):
            hit = DEREF.search(body)
            if hit:
                findings.append(f"{p}:{line}: __try body contains a raw deref "
                                f"(`{hit.group(0).strip()}`) — clang-cl may ELIDE this guard. "
                                f"Move the access into a __declspec(noinline) body function.")
    for f in findings:
        print(f)
    if findings:
        print(f"\n{len(findings)} finding(s). See docs/memory/tooling/clang-cl-seh-noinline.md.")
        return 1
    print("lint_seh: all __try blocks wrap calls only — OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
