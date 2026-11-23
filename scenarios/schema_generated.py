#!/usr/bin/env python3
"""Fails if src/codeline/generated/ differs from what schema/ generates now.

Fix a failure by running:  python3 tools/gen_appliance_tables.py
"""

import filecmp
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHECKED_IN = os.path.join(ROOT, "src", "codeline", "generated")
GENERATOR = os.path.join(ROOT, "tools", "gen_appliance_tables.py")


def main():
    with tempfile.TemporaryDirectory() as fresh:
        subprocess.run([sys.executable, GENERATOR, "--out", fresh], check=True)
        expected = sorted(os.listdir(fresh))
        actual = sorted(f for f in os.listdir(CHECKED_IN) if not f.startswith("."))
        stale = [f for f in expected if f not in actual or not filecmp.cmp(
            os.path.join(fresh, f), os.path.join(CHECKED_IN, f), shallow=False)]
        extra = [f for f in actual if f not in expected]
    for f in stale:
        print(f"  FAIL  src/codeline/generated/{f} is out of date with schema/")
    for f in extra:
        print(f"  FAIL  src/codeline/generated/{f} is not produced by the generator")
    if stale or extra:
        print("  run: python3 tools/gen_appliance_tables.py")
        return 1
    print(f"  ok    {len(expected)} generated files match schema/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
