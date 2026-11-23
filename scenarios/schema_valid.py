#!/usr/bin/env python3
"""Fails if a cluster or appliance file under schema/ breaks its JSON Schema.

The shapes are in schema/meta/. Run through uv (`make check` does):
    uv run python scenarios/schema_valid.py
"""

import json
import sys
from pathlib import Path

from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parent.parent
KINDS = {"clusters": "cluster.schema.json", "appliances": "appliance.schema.json", "rulebooks": "rulebook.schema.json"}


def main():
    failures = 0
    for folder, meta_name in KINDS.items():
        meta = json.loads((ROOT / "schema" / "meta" / meta_name).read_text())
        Draft202012Validator.check_schema(meta)
        validator = Draft202012Validator(meta)
        for path in sorted((ROOT / "schema" / folder).glob("*.json")):
            errors = sorted(validator.iter_errors(json.loads(path.read_text())), key=lambda e: list(e.path))
            for e in errors:
                where = "/".join(str(p) for p in e.path) or "(top)"
                print(f"  FAIL  {path.relative_to(ROOT)}: {where}: {e.message}")
            if not errors:
                print(f"  ok    {path.relative_to(ROOT)}")
            failures += len(errors)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
