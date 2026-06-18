"""Make the project's `tools/` scripts importable from the test suite.

The pure generator logic lives in tools/*.py (no package __init__), so we add
the parent tools directory to sys.path here. Tests stay hermetic: they exercise
the join/filter/grouping helpers with tiny inline fixtures — no game data, no
regulation.bin, no generated files on disk.
"""
import os
import sys

TOOLS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TOOLS_DIR)
