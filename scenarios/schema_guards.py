#!/usr/bin/env python3
"""The generator refuses a schema that would let an appliance be configured
into a state it cannot handle. Each case edits a scratch copy of schema/ and
expects tools/gen_appliance_tables.py to fail with the named reason.

    lever.inputs max 40     a channel count wider than the BitVector (phantom SW33)
    button.inputs max 33    the same on another cluster
    mast.heads max 4        a head count no rulebook has a STOP row for
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GENERATOR = os.path.join(ROOT, "tools", "gen_appliance_tables.py")


def set_attr(schema, cluster, name, key, value):
    path = os.path.join(schema, "clusters", f"{cluster}.json")
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    next(a for a in doc["attributes"] if a["name"] == name)[key] = value
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f)


CASES = [
    ("unchanged schema generates", None, None),
    ("lever.inputs above the BitVector width", ("lever", "inputs", "max", 40), "exceeds the BitVector width"),
    ("button.inputs above the BitVector width", ("button", "inputs", "max", 33), "exceeds the BitVector width"),
    ("mast.heads beyond every rulebook's STOP rows", ("mast", "heads", "max", 4), "no STOP row for 4 heads"),
]


def main():
    failures = 0
    for title, edit, reason in CASES:
        with tempfile.TemporaryDirectory() as tmp:
            schema = os.path.join(tmp, "schema")
            shutil.copytree(os.path.join(ROOT, "schema"), schema)
            if edit:
                set_attr(schema, *edit)
            run = subprocess.run([sys.executable, GENERATOR, "--schema", schema, "--out", os.path.join(tmp, "out")],
                                 capture_output=True, text=True)
            ok = run.returncode == 0 if reason is None else (run.returncode != 0 and reason in run.stderr)
            print(f"  {'ok  ' if ok else 'FAIL'}  {title}")
            if not ok:
                print(f"        exit {run.returncode}: {run.stderr.strip()}")
                failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
