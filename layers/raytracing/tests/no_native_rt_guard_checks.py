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
    print(f"No-native-RT guard controls PASS: positive execution, discovery/reload segments, 13 blocked attacks, fatal void-call exit86, {len(mutations)} invalid-evidence rejections")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    main(args.executable.resolve(strict=True), args.output.resolve())
