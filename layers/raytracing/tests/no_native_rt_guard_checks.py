#!/usr/bin/env python3
# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
"""Host-only positive, negative and fatal controls for the downstream guard."""

import argparse
import copy
import json
from pathlib import Path
import subprocess
import tempfile

from no_native_rt_guard_audit import audit, audit_records


def main(executable, output):
    output.mkdir(parents=True, exist_ok=True)
    common = {"stdout": subprocess.PIPE, "stderr": subprocess.STDOUT, "timeout": 30}
    if hasattr(subprocess, "CREATE_NO_WINDOW"):
        common["creationflags"] = subprocess.CREATE_NO_WINDOW
    for name, mode, expected in (("positive", "--positive-control", 0), ("negative", None, 0), ("fatal", "--fatal-control", 86)):
        log = (output / f"{name}.jsonl").resolve()
        result = subprocess.run([str(executable), str(log)] + ([mode] if mode else []), **common)
        (output / f"{name}.stdout.txt").write_bytes(result.stdout)
        if result.returncode != expected:
            raise AssertionError(f"{name} returned {result.returncode}, expected {expected}: {result.stdout.decode(errors='replace')}")
        if name == "positive":
            good = audit(log)
            assert good["devices"] == 1 and good["shader_definitions"] == 1 and good["rejected"] == 0
        else:
            records = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]
            rejects = [r for r in records if r["event"] == "reject"]
            assert len(rejects) == (1 if name == "fatal" else 13)
            if name == "fatal":
                assert rejects[0]["kind"] == "native_entrypoint" and rejects[0]["detail"] == "vkCmdTraceRaysKHR"
            try:
                audit(log)
            except ValueError:
                pass
            else:
                raise AssertionError(f"{name} rejection log incorrectly counted as successful boundary evidence")

    records = [json.loads(line) for line in (output / "positive.jsonl").read_text(encoding="utf-8").splitlines()]
    discovery = []
    for source, event in ((records[0], "start"), (records[1], "instance"), (records[-1], "summary")):
        record = copy.deepcopy(source)
        record["load_id"] = "host-control-discovery-load"
        record["event"] = event
        for key in ("checked", "rejected", "devices", "shader_definitions"):
            record[key] = 0
        discovery.append(record)
    reloaded = audit_records(discovery + records)
    assert reloaded["loads"] == 2 and reloaded["instances"] == 2 and reloaded["devices"] == 1
    mutations = [records[:-1], records + records, []]
    mask = copy.deepcopy(records)
    mask[0]["mode"] = "mask-discovery"
    mutations.append(mask)
    no_shader = copy.deepcopy(records)
    no_shader[-1]["shader_definitions"] = 0
    mutations.append(no_shader)
    hidden_reject = copy.deepcopy(records)
    hidden_reject[-1]["rejected"] = 1
    mutations.append(hidden_reject)
    other_process = copy.deepcopy(discovery)
    for record in other_process:
        record["pid"] += 1
    mutations.append(other_process + records)
    mutations.append(discovery[:-1] + records)
    wrong_load = copy.deepcopy(records)
    wrong_load[1]["load_id"] = "missing-start-load"
    mutations.append(wrong_load)
    decreasing = copy.deepcopy(records)
    decreasing[-1]["checked"] = 0
    mutations.append(decreasing)
    no_pid = copy.deepcopy(records)
    no_pid[0].pop("pid")
    mutations.append(no_pid)
    mutations.append(discovery)
    mutations.append(records + [records[-1]])
    with tempfile.TemporaryDirectory(prefix="audit-controls-", dir=output) as temporary:
        for index, mutated in enumerate(mutations):
            path = Path(temporary) / f"mutation-{index}.jsonl"
            path.write_text("".join(json.dumps(r) + "\n" for r in mutated), encoding="utf-8")
            try:
                audit(path)
            except ValueError:
                pass
            else:
                raise AssertionError(f"audit accepted invalid evidence mutation {index}")

    expected_limits = (9, 7, 3)
    forced_cases = (("forced", "--forced-dispatch-control", 0, None),
                    ("forced-empty", "--forced-no-dispatch-control", 0, None),
                    ("forced-oversize", "--forced-dispatch-fatal-control", 86, "dispatch_limits"),
                    ("forced-native", "--forced-native-limit-fatal-control", 86, "dispatch_limits"),
                    ("forced-base", "--forced-base-fatal-control", 86, "dispatch_limits"),
                    ("forced-indirect", "--forced-indirect-fatal-control", 86, "uninspectable_dispatch"),
                    ("forced-invalid", "--invalid-dispatch-config-control", 0, None),
                    ("forced-mask", "--forced-mask-config-control", 0, None))
    for name, mode, expected_exit, reject_kind in forced_cases:
        path = (output / f"{name}.jsonl").resolve()
        result = subprocess.run([str(executable), str(path), mode], **common)
        (output / f"{name}.stdout.txt").write_bytes(result.stdout)
        if result.returncode != expected_exit:
            raise AssertionError(f"{name} returned {result.returncode}, expected {expected_exit}: {result.stdout.decode(errors='replace')}")
        if name in ("forced-invalid", "forced-mask"):
            assert not path.exists(), "invalid configuration must fail before starting a guard instance"
            continue
        forced_records = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
        try:
            audit(path)
        except ValueError:
            pass
        else:
            raise AssertionError(f"default auditor accepted artificial dispatch limits in {name}")
        if name == "forced":
            forced_good = audit(path, expected_dispatch_limits=expected_limits)
            assert forced_good["mode"] == "forced-dispatch-limits" and forced_good["dispatches"] == 2
            assert forced_good["multidimensional_dispatches"] == 2 and forced_good["dispatch_max"] == [8, 5, 3]
            assert forced_good["dispatch_limits"] == list(expected_limits)
            good_forced_records = forced_records
        else:
            if reject_kind:
                rejects = [r for r in forced_records if r["event"] == "reject"]
                assert len(rejects) == 1 and rejects[0]["kind"] == reject_kind
            try:
                audit(path, expected_dispatch_limits=expected_limits)
            except ValueError:
                pass
            else:
                raise AssertionError(f"dedicated auditor accepted rejected or dispatch-free evidence {name}")

    forced_discovery = copy.deepcopy(discovery)
    for record in forced_discovery:
        record.update(pid=good_forced_records[0]["pid"], mode="forced-dispatch-limits", dispatch_limits=list(expected_limits),
                      dispatches=0, multidimensional_dispatches=0, dispatch_max=[0, 0, 0])
    assert audit_records(forced_discovery + good_forced_records, expected_dispatch_limits=expected_limits)["loads"] == 2
    forced_mutations = []
    for change in ("limits", "empty", "max", "multi", "bool", "mode"):
        bad = copy.deepcopy(good_forced_records)
        if change == "limits":
            bad[-1]["dispatch_limits"][2] = 4
        elif change == "empty":
            bad[-1]["dispatches"] = bad[-1]["multidimensional_dispatches"] = 0
        elif change == "max":
            bad[-1]["dispatch_max"][0] = 10
        elif change == "multi":
            bad[-1]["multidimensional_dispatches"] = 3
        elif change == "bool":
            bad[-1]["dispatches"] = True
        else:
            bad[-1]["mode"] = "execution-only"
        forced_mutations.append(bad)
    for index, bad in enumerate(forced_mutations):
        try:
            audit_records(bad, expected_dispatch_limits=expected_limits)
        except ValueError:
            pass
        else:
            raise AssertionError(f"dedicated auditor accepted forced-profile mutation {index}")
    disguised = copy.deepcopy(good_forced_records)
    for record in disguised:
        record["mode"] = "execution-only"
    try:
        audit_records(disguised)
    except ValueError:
        pass
    else:
        raise AssertionError("default auditor accepted forced fields relabeled as execution-only")
    print(f"No-native-RT guard controls PASS: positive execution, discovery/reload segments, 13 blocked attacks, fatal void-call exit86, {len(mutations)} invalid-evidence rejections; forced-limit property/dispatch/base/indirect/configuration controls and {len(forced_mutations)} dedicated-audit mutations")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    main(args.executable.resolve(strict=True), args.output.resolve())
