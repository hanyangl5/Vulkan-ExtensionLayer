# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
"""Verify pinned source bytes without network access or a Git checkout."""
import hashlib
import json
from pathlib import Path
import sys


def verify(root):
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    failures = []
    for entry in manifest["files"]:
        relative = Path(entry["local"])
        if relative.is_absolute() or ".." in relative.parts:
            failures.append(f"Invalid manifest path: {relative}")
            continue
        path = root / relative
        if not path.is_file():
            failures.append(f"Missing source: {relative}")
            continue
        data = path.read_bytes()
        # Match Git's blob object IDs, including files without final newlines.
        blob = b"blob " + str(len(data)).encode("ascii") + b"\0" + data
        digest = hashlib.sha1(blob).hexdigest()
        if digest != entry["sha"] or len(data) != entry["size"]:
            failures.append(f"Pinned source differs: {relative}")
    expected = {x["local"] for x in manifest["files"]}
    for source in manifest["sources"]:
        for path in (root / source["name"]).rglob("*"):
            if path.is_file() and path.relative_to(root).as_posix() not in expected:
                failures.append(f"Unrecorded reference file: {path.relative_to(root)}")
    return failures, len(manifest["files"])


if __name__ == "__main__":
    errors, count = verify(Path(__file__).resolve().parent)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        sys.exit(1)
    print(f"Verified {count} pinned reference files; this does not verify port completeness")
