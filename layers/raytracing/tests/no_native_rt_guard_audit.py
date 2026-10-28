#!/usr/bin/env python3
# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
"""Check a single process's downstream guard log alongside its test result.

This audits boundary evidence only. A passing CTS/test exit and QPA result must
be verified independently. Rejections, missing summaries and mask-discovery
cannot count as a successful execution-only no-native-RT boundary run.
"""

import argparse
import json
from pathlib import Path


def audit_records(records, require_shaders=True):
    if not records or not isinstance(records[0], dict) or records[0].get("event") != "start":
        raise ValueError("guard configuration/start record is missing")
    counter_keys = ("checked", "rejected", "devices", "shader_definitions")
    process = records[0].get("pid")
    if type(process) is not int or process <= 0:
        raise ValueError("guard process identity is missing or invalid")
    segments = []
    seen_loads = set()
    current = None
    active_instances = 0
    last_event = None

    def finish_segment():
        if not current or not current["instances"] or active_instances or last_event != "summary":
            raise ValueError("not every instance completed in a load segment; terminal summary is missing")
        segments.append(dict(current))

    for record in records:
        if not isinstance(record, dict) or record.get("schema_version") != 2:
            raise ValueError("guard record schema/identity is missing or unsupported")
        if record.get("pid") != process or type(record.get("pid")) is not int:
            raise ValueError("guard log mixes different process identities")
        if record.get("mode") != "execution-only":
            raise ValueError("mask-discovery is a separate support-discovery observation")
        load = record.get("load_id")
        if not isinstance(load, str) or not load or len(load) > 128:
            raise ValueError("guard load identity is missing or invalid")
        event = record.get("event")
        if event not in ("start", "instance", "device", "summary", "reject"):
            raise ValueError("unknown guard event")
        if event == "start":
            if current is not None:
                finish_segment()
            if load in seen_loads:
                raise ValueError("guard load identity repeated; concatenated evidence is invalid")
            seen_loads.add(load)
            current = {"load_id": load, "instances": 0, **{key: 0 for key in counter_keys}}
            active_instances = 0
            if any(record.get(key) != 0 for key in counter_keys):
                raise ValueError("guard load start counters are not zero")
        elif current is None or load != current["load_id"]:
            raise ValueError("guard event has no matching active load start")
        for key in counter_keys:
            value = record.get(key)
            if type(value) is not int or value < current[key]:
                raise ValueError(f"invalid or decreasing {key} counter in load segment")
            current[key] = value
        if event == "reject" or record["rejected"]:
            raise ValueError(f"native boundary rejected {record.get('kind')}: {record.get('detail')}")
        if event == "instance":
            current["instances"] += 1
            active_instances += 1
        elif event == "summary":
            if not active_instances:
                raise ValueError("instance summary has no matching created instance")
            active_instances -= 1
        elif event == "device" and not active_instances:
            raise ValueError("device created outside a live instance")
        last_event = event
    finish_segment()
    executing = [segment for segment in segments if segment["devices"] and segment["checked"]]
    if not executing:
        raise ValueError("no downstream device/execution boundary was exercised")
    if require_shaders and not any(segment["shader_definitions"] for segment in executing):
        raise ValueError("no executing load segment has an inspected shader definition")
    totals = {key: sum(segment[key] for segment in segments) for key in ("instances", *counter_keys)}
    return {"mode": "execution-only", "pid": process, "loads": len(segments), "segments": segments, **totals}


def audit(path, require_shaders=True):
    records = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    return audit_records(records, require_shaders=require_shaders)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--allow-no-shaders", action="store_true", help="device-only controls; does not establish shader execution")
    args = parser.parse_args()
    try:
        result = audit(args.log, require_shaders=not args.allow_no_shaders)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"Guard evidence FAIL: {error}\n")
    print(json.dumps(result, sort_keys=True))
