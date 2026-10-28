#!/usr/bin/env python3
# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
"""Independent host controls for the explicit zero-timestamp guard profiles."""

import argparse
import copy
import json
from pathlib import Path
import subprocess

from no_native_rt_guard_audit import audit, audit_records


def must_reject(records, **expected):
    try:
        audit_records(records, **expected)
    except ValueError:
        return
    raise AssertionError(f"invalid guard evidence accepted with {expected}")


def main(executable, output):
    output.mkdir(parents=True, exist_ok=True)
    common = {"stdout": subprocess.PIPE, "stderr": subprocess.STDOUT, "timeout": 30}
    if hasattr(subprocess, "CREATE_NO_WINDOW"):
        common["creationflags"] = subprocess.CREATE_NO_WINDOW

    def run(name, mode, expected_exit=0):
        path = (output / f"{name}.jsonl").resolve()
        result = subprocess.run([str(executable), str(path), mode], **common)
        (output / f"{name}.stdout.txt").write_bytes(result.stdout)
        if result.returncode != expected_exit:
            raise AssertionError(f"{name}: exit {result.returncode}, expected {expected_exit}: "
                                 f"{result.stdout.decode(errors='replace')}")
        return path

    goods = {}
    for masked in (False, True):
        name = "zero-masked" if masked else "zero-native"
        mode = "--zero-timestamps-masked-control" if masked else "--zero-timestamps-control"
        path = run(name, mode)
        records = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
        expected = {"expected_zero_timestamps": True, "expected_discovery_mask": masked}
        result = audit(path, **expected)
        assert result["mode"] == ("mask-discovery+zero-timestamps" if masked else "zero-timestamps")
        assert result["rt_boundary_enforced"] is masked and result["zero_timestamp_version"] == 1
        assert result["queue_family_queries"] == 15 and result["zero_timestamp_families"] == 20
        assert result["timestamp_properties_queries"] == 3 and result["timestamp_write_attempts"] == 0
        assert result["event_signals"] == 3 and result["event_status_queries"] == 1
        assert result["shader_definitions"] == 1 and result["rejected"] == 0
        if not masked:
            assert "native RT is permitted" in result["qualification"]
        must_reject(records)
        must_reject(records, expected_discovery_mask=masked)
        must_reject(records, expected_zero_timestamps=True, expected_discovery_mask=not masked)
        must_reject(records, expected_zero_timestamps=True, expected_dispatch_limits=(9, 7, 3))
        goods[masked] = records

        for command, spelling in (("legacy", "timestamps"), ("core2", "timestamps2"), ("khr2", "timestamps2-khr")):
            fatal_mode = f"--zero-{spelling}{'-masked' if masked else ''}-fatal-control"
            fatal_path = run(f"{name}-{command}-fatal", fatal_mode, 86)
            fatal = [json.loads(line) for line in fatal_path.read_text(encoding="utf-8").splitlines()]
            rejects = [record for record in fatal if record["event"] == "reject"]
            assert len(rejects) == 1 and rejects[0]["kind"] == "zero_timestamp_write"
            assert rejects[0]["detail"] == ("vkCmdWriteTimestamp" if command == "legacy" else "vkCmdWriteTimestamp2")
            assert rejects[0]["timestamp_write_attempts"] == 1
            must_reject(fatal, **expected)

    path = run("zero-without-queue-observation", "--zero-timestamps-no-queues-control")
    no_queues = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
    must_reject(no_queues, expected_zero_timestamps=True)
    path = run("zero-forced-invalid", "--forced-zero-config-control")
    assert not path.exists(), "mixed forced/zero configuration must fail before a guard start record"

    path = run("ordinary", "--positive-control")
    ordinary = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
    assert audit(path)["mode"] == "execution-only"
    must_reject(ordinary, expected_zero_timestamps=True)

    mutation_count = 0
    for masked, records in goods.items():
        expected = {"expected_zero_timestamps": True, "expected_discovery_mask": masked}
        for field, value in (("schema_version", True), ("zero_timestamp_version", True), ("zero_timestamp_version", 2),
                             ("rt_boundary_enforced", int(masked)), ("rt_boundary_enforced", not masked),
                             ("timestamp_write_attempts", 1), ("timestamp_write_attempts", False),
                             ("queue_family_queries", 0), ("zero_timestamp_families", 0),
                             ("event_status_queries", -1), ("event_signals", True),
                             ("timestamp_properties_queries", 0)):
            bad = copy.deepcopy(records)
            bad[-1][field] = value
            must_reject(bad, **expected)
            mutation_count += 1
        for field in ("zero_timestamp_version", "rt_boundary_enforced", "queue_family_queries"):
            bad = copy.deepcopy(records)
            del bad[-1][field]
            must_reject(bad, **expected)
            mutation_count += 1
        bad = copy.deepcopy(records)
        bad[0]["event_signals"] = False
        must_reject(bad, **expected)
        mutation_count += 1
        bad = copy.deepcopy(records)
        bad[-1]["dispatch_limits"] = [9, 7, 3]
        must_reject(bad, **expected)
        mutation_count += 1
        bad = copy.deepcopy(records)
        for record in bad:
            record["mode"] = "mask-discovery" if masked else "execution-only"
        must_reject(bad, expected_discovery_mask=masked)
        mutation_count += 1
        must_reject(records, expected_zero_timestamps=1, expected_discovery_mask=masked)
        must_reject(records, expected_zero_timestamps=True, expected_discovery_mask=int(masked))
        mutation_count += 2
        reloaded = copy.deepcopy(records)
        for record in reloaded:
            record["load_id"] += "-reload"
        assert audit_records(records + reloaded, **expected)["loads"] == 2

    # Keep process identity constant so these specifically exercise profile
    # separation rather than merely rejecting unrelated process concatenation.
    mixed = copy.deepcopy(goods[True])
    for record in mixed:
        record["pid"] = goods[False][0]["pid"]
    must_reject(goods[False] + mixed, expected_zero_timestamps=True)
    must_reject(goods[False] + mixed, expected_zero_timestamps=True, expected_discovery_mask=True)
    disguised = copy.deepcopy(goods[False])
    for record in disguised:
        record["mode"] = "mask-discovery+zero-timestamps"
        record["discovery_mask_version"] = 1
    must_reject(disguised, expected_zero_timestamps=True, expected_discovery_mask=True)
    print(f"Zero-timestamp guard controls PASS: native and masked profiles; unchanged ordinary timestamps; "
          f"legacy/core2/KHR timestamp writes rejected in both profiles; zero queue observation, forced combination, "
          f"mixed/disguised profiles and {mutation_count} typed evidence mutations rejected")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    main(args.executable.resolve(strict=True), args.output.resolve())
