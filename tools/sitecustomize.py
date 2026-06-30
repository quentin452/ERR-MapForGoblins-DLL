# Auto-imported by CPython at startup whenever `tools/` is on sys.path[0]
# (i.e. every `py tools\<script>.py` run in the build pipeline).
#
# WHY: the SoulsFormats (.NET) readers write a temp .bnd/.msb, read it, then the
# Python helpers immediately os.unlink() it. On Windows the just-read temp is still
# briefly locked (the .NET reader/AV holds a handle), so the unlink raises
# PermissionError (WinError 5) and aborts an otherwise-successful extraction stage.
# The data is already in memory by then; a leaked temp is harmless. Make unlink
# retry-then-swallow process-wide so no stage dies on cleanup.
import os
import time

_orig_unlink = os.unlink
_orig_remove = os.remove


def _tolerant_unlink(path, *args, **kwargs):
    last = None
    for _ in range(12):
        try:
            return _orig_unlink(path, *args, **kwargs)
        except FileNotFoundError:
            return None
        except PermissionError as e:
            last = e
            time.sleep(0.05)
    # Give up quietly — a leaked temp file never justifies failing the build.
    return None


os.unlink = _tolerant_unlink
os.remove = _tolerant_unlink
