"""Minimal .env loader for MapForGoblins tooling.

Loads ``.env`` then ``.env.local`` into ``os.environ`` WITHOUT overriding
variables already present in the real process environment, so the effective
precedence is:

    real shell env  >  .env.local  >  .env

Both files are searched in the repo root AND in ``tools/`` (the same dir as
``config.ini``); when a key appears in more than one, ``tools/`` wins over the
repo root and ``.env.local`` wins over ``.env``.

``.env`` is a committed template (placeholders, empty values); ``.env.local``
holds the real per-machine paths and is gitignored. Empty values are ignored,
so any var left blank simply falls through to ``tools/config.ini`` and then to
each tool's built-in default — nothing breaks if neither file is present.

``config.py`` calls :func:`load` at import time, so every tool sees the values,
and every child subprocess inherits them via ``os.environ``.

Format: plain ``KEY=VALUE`` lines. Blank lines and ``#`` comments are skipped,
an optional leading ``export`` is stripped, and matched surrounding single or
double quotes around the value are removed. No variable interpolation.
"""
import os
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
PROJECT_DIR = TOOLS_DIR.parent
# Lowest-priority dir first; later entries override earlier ones.
_SEARCH_DIRS = (PROJECT_DIR, TOOLS_DIR)


def _parse(path):
    """Parse one KEY=VALUE file into a dict (missing/unreadable file -> {})."""
    vals = {}
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return vals
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[len("export "):].lstrip()
        if "=" not in line:
            continue
        key, _, val = line.partition("=")
        key = key.strip()
        if not key:
            continue
        val = val.strip()
        if len(val) >= 2 and val[0] == val[-1] and val[0] in ("'", '"'):
            val = val[1:-1]
        vals[key] = val
    return vals


def load(search_dirs=_SEARCH_DIRS):
    """Load .env then .env.local into os.environ.

    .env.local overrides .env, and tools/ overrides the repo root; none
    override a var already in the real environment. Empty values are skipped.
    Returns the merged dict that was sourced from the files (for debugging)."""
    merged = {}
    for name in (".env", ".env.local"):
        for base in search_dirs:
            merged.update(_parse(Path(base) / name))
    for key, val in merged.items():
        if val == "":
            continue
        if key not in os.environ:  # real shell environment wins
            os.environ[key] = val
    return merged
