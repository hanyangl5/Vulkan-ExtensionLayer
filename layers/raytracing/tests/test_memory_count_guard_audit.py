#!/usr/bin/env python3
# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
"""Exercise the native allocation boundary and audit corrupted evidence."""
import argparse
import copy
import json
from pathlib import Path
import subprocess

from no_native_rt_guard_audit import audit, audit_records


def main(executable, output):
    output.mkdir(parents=True, exist_ok=True)
    good = output / "positive.jsonl"
    fatal = output / "overflow.jsonl"
    options = {"stdout": subprocess.PIPE, "stderr": subprocess.STDOUT, "timeout": 30}
    if hasattr(subprocess, "CREATE_NO_WINDOW"):
        options["creationflags"] = subprocess.CREATE_NO_WINDOW
    for name, path, arguments, expected_exit in (("positive", good, [], 0), ("overflow", fatal, ["--overflow"], 86)):
        result = subprocess.run([str(executable), str(path), *arguments], **options)
        (output / f"{name}.stdout.txt").write_bytes(result.stdout)
        assert result.returncode == expected_exit, result.stdout.decode(errors="replace")
    result = audit(good, require_shaders=False, expected_memory_count=True)
    assert result["memory_peak"] == result["memory_count_limit"] == 4096
    assert result["memory_allocations"] == result["memory_frees"] == 4097
    assert result["memory_attempts"] == 4098 and result["memory_properties"] == 3
    records = [json.loads(line) for line in good.read_text().splitlines()]

    def rejects(values, **kwargs):
        try:
            audit_records(values, require_shaders=False, **kwargs)
        except ValueError:
            return
        raise AssertionError("corrupt or unexpected memory profile was accepted")

    rejects(records)
    rejects(records, expected_memory_count=1)
    rejects(records, expected_memory_count=True, expected_dispatch_limits=(1, 1, 1))
    mutations = 0
    for key, value in (("memory_count_version", True), ("memory_count_version", 2), ("memory_count_limit", 4095),
                       ("memory_count_limit", True), ("memory_properties", 0), ("memory_peak", 4097),
                       ("memory_live", 1), ("memory_live", False), ("memory_allocations", 4098),
                       ("memory_frees", 4098), ("memory_attempts", False), ("memory_peak", -1),
                       ("memory_attempts", 0), ("memory_peak", 0)):
        changed = copy.deepcopy(records)
        changed[-1][key] = value
        rejects(changed, expected_memory_count=True)
        mutations += 1
    for key in ("memory_count_version", "memory_peak", "memory_live", "memory_allocations"):
        changed = copy.deepcopy(records)
        del changed[-1][key]
        rejects(changed, expected_memory_count=True)
        mutations += 1
    bad = [json.loads(line) for line in fatal.read_text().splitlines()]
    assert bad[-1]["event"] == "reject" and bad[-1]["kind"] == "memory_count_overflow"
    assert bad[-1]["memory_live"] == bad[-1]["memory_peak"] == 4096
    rejects(bad, expected_memory_count=True)
    print(f"Memory-count guard PASS: full 4096, failure rollback, free/retry, overflow rejection, {mutations} mutations")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    main(args.executable.resolve(), args.output.resolve())
