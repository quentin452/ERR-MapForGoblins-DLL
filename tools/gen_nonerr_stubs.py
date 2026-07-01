#!/usr/bin/env python3
"""Emit EMPTY stubs of the ERR-only generated files for a non-err profile, so the
DLL links. These tables (quest gates/steps/browser, region anchors, name regions,
model aliases) are ERR-only features the data pipeline does not generate off-ERR,
but goblin_inject/map_renderer/goblin_overlay reference their symbols unconditionally.
A COUNT=0 stub makes every consumer a no-op (loops/lower_bound over an empty range),
which is correct: those features are simply absent on the profile. The loot path is
untouched (it never reads these), so this does NOT affect loot static-vs-runtime
fidelity — loot markers come live (params) / from disk (MSB/EMEVD), not a bake.

Usage:  py gen_nonerr_stubs.py <profile>      # erte | convergence | vanilla
The .hpp is copied verbatim from src/generated/ (struct defs are profile-independent);
the .cpp is synthesized empty. Only writes a file if it is MISSING in the target dir.
"""
import re
import sys
from pathlib import Path

ERR_ONLY = [
    "goblin_quest_gates",
    "goblin_quest_steps",
    "goblin_region_anchors",
    "goblin_name_regions",
    "goblin_model_aliases",
]

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "src" / "generated"

# `extern <type...> <name>[];`  or  `extern <type...> <name>;`
EXTERN_RE = re.compile(r"extern\s+(.+?)\s+(\w+)\s*(\[\s*\])?\s*;")
# A free-function declaration: `<ret> <name>(<params>);` on its own line, no
# `extern`. These live in namespace `goblin` (not ::generated) and the overlay /
# quest_npc_layer reference them unconditionally, so the stub must DEFINE them too
# or the non-err DLL fails to link (e.g. quest_step_done). Comment lines (start
# with `/`) and the bracketed extern data decls are naturally excluded.
FUNC_RE = re.compile(
    r"^[ \t]*([A-Za-z_][\w:<>,*& ]*?[\w&*>])[ \t]+(\w+)[ \t]*\(([^;{}]*)\)[ \t]*;[ \t]*$",
    re.M,
)


def _default_return(ret: str) -> str:
    r = ret.strip()
    if r == "void":
        return ""
    if r.endswith("*"):
        return "    return nullptr;\n"
    if "bool" in r.split():
        return "    return false;\n"
    return "    return {};\n"  # int / size_t / struct value


def synth_cpp(name: str, hpp_text: str) -> str:
    defs = []
    for m in EXTERN_RE.finditer(hpp_text):
        typ, sym, arr = m.group(1).strip(), m.group(2), m.group(3)
        if arr:  # array: a size-1 dummy, never indexed (COUNT is 0)
            defs.append(f"    {typ} {sym}[1] = {{}};")
        elif "size_t" in typ or sym.upper().endswith("COUNT"):
            defs.append(f"    {typ} {sym} = 0;")
        else:  # scalar / struct value
            defs.append(f"    {typ} {sym} = {{}};")
    body = "\n".join(defs) if defs else "    // (no extern symbols)"
    out = (
        f"// AUTO-GENERATED empty stub (non-err profile) — see tools/gen_nonerr_stubs.py.\n"
        f"// {name} is an ERR-only feature; this empty table makes the DLL link and the\n"
        f"// feature a no-op on this profile. Do not hand-edit.\n"
        f'#include "{name}.hpp"\n\n'
        f"namespace goblin::generated\n{{\n{body}\n}}\n"
    )
    # No-op definitions for free functions (namespace goblin). Safe to call: the
    # data tables above are empty, so any caller hits a count==0 / not-found path.
    funcs = []
    for m in FUNC_RE.finditer(hpp_text):
        ret, fname, params = m.group(1).strip(), m.group(2), m.group(3).strip()
        funcs.append(f"{ret} {fname}({params})\n{{\n{_default_return(ret)}}}")
    if funcs:
        out += "\nnamespace goblin\n{\n" + "\n".join(funcs) + "\n}\n"
    return out


def main():
    if len(sys.argv) < 2:
        print("usage: gen_nonerr_stubs.py <profile>")
        return 2
    profile = sys.argv[1].strip().lower()
    if profile == "err":
        print("err uses the committed files; nothing to stub.")
        return 0
    dst = REPO / "src" / f"generated_{profile}"
    if not dst.is_dir():
        print(f"ERROR: {dst} does not exist (run the pipeline for '{profile}' first)")
        return 1

    wrote = 0
    for name in ERR_ONLY:
        src_hpp = SRC / f"{name}.hpp"
        if not src_hpp.exists():
            print(f"  skip {name}: no err .hpp to copy struct defs from")
            continue
        hpp_text = src_hpp.read_text(encoding="utf-8")
        dst_hpp, dst_cpp = dst / f"{name}.hpp", dst / f"{name}.cpp"
        # The .hpp is a VERBATIM copy of the ERR struct defs, which are
        # profile-independent -- it must track schema changes, so always refresh
        # it (a stale copy from before a schema migration silently breaks the
        # build, e.g. QuestStep gaining progress_flag/entity_id). The .cpp stub is
        # synthesized from the .hpp's extern decls below; refresh it in lockstep.
        if not dst_hpp.exists() or dst_hpp.read_text(encoding="utf-8") != hpp_text:
            dst_hpp.write_text(hpp_text, encoding="utf-8")
            wrote += 1
            print(f"  + {dst_hpp.relative_to(REPO)}")
        cpp_text = synth_cpp(name, hpp_text)
        if not dst_cpp.exists() or dst_cpp.read_text(encoding="utf-8") != cpp_text:
            dst_cpp.write_text(cpp_text, encoding="utf-8")
            wrote += 1
            print(f"  + {dst_cpp.relative_to(REPO)}")
    print(f"[gen_nonerr_stubs] {profile}: wrote {wrote} stub file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
