#!/usr/bin/env python3
# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
"""Check a single process's downstream guard log alongside its test result.

This audits boundary evidence only. A passing CTS/test exit and QPA result must
be verified independently. Rejections, missing summaries and mask-discovery
cannot count as a successful execution-only no-native-RT boundary run. Artificial
dispatch-limit runs require a separate explicit expected profile and never count
as ordinary Vulkan mandatory-limit or execution-only qualification.
"""

import argparse
import json
from pathlib import Path


def dispatch_limits(value):
    if isinstance(value, str):
        parts = value.split(",")
        if len(parts) != 3 or any(not p or not p.isascii() or not p.isdecimal() for p in parts):
            raise ValueError("expected dispatch limits must contain three positive decimal uint32 values")
        value = tuple(int(p) for p in parts)
    if not isinstance(value, (tuple, list)) or len(value) != 3 or any(type(v) is not int or not 0 < v <= 0xffffffff for v in value):
        raise ValueError("expected dispatch limits must contain three positive decimal uint32 values")
    return tuple(value)


def audit_records(records, require_shaders=True, expected_dispatch_limits=None):
    if not records or not isinstance(records[0], dict) or records[0].get("event") != "start":
        raise ValueError("guard configuration/start record is missing")
    counter_keys = ("checked", "rejected", "devices", "shader_definitions")
    forced = expected_dispatch_limits is not None
    expected = dispatch_limits(expected_dispatch_limits) if forced else None
    mode = "forced-dispatch-limits" if forced else "execution-only"
    if forced:
        counter_keys += ("dispatches", "multidimensional_dispatches")
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
        if record.get("mode") != mode:
            raise ValueError("guard mode is not the explicitly expected execution/dispatch profile")
        if forced:
            if dispatch_limits(record.get("dispatch_limits")) != expected:
                raise ValueError("guard dispatch limits differ from the explicitly expected test profile")
        elif any(key in record for key in ("dispatch_limits", "dispatches", "dispatch_max", "multidimensional_dispatches")):
            raise ValueError("artificial dispatch-limit evidence needs an explicit dedicated audit")
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
            if forced:
                current["dispatch_max"] = [0, 0, 0]
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
        if forced:
            maximum = record.get("dispatch_max")
            if not isinstance(maximum, list) or len(maximum) != 3 or any(
                    type(value) is not int or value < current["dispatch_max"][axis] or value > expected[axis]
                    for axis, value in enumerate(maximum)):
                raise ValueError("invalid or decreasing observed dispatch maximum")
            if event == "start" and any(maximum):
                raise ValueError("guard load start dispatch maximum is not zero")
            if record["multidimensional_dispatches"] > record["dispatches"]:
                raise ValueError("multidimensional dispatch count exceeds checked dispatch count")
            if record["multidimensional_dispatches"] and maximum[1] <= 1 and maximum[2] <= 1:
                raise ValueError("multidimensional dispatch count has no matching observed dimensions")
            current["dispatch_max"] = list(maximum)
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
    if forced and not any(segment["dispatches"] for segment in executing):
        raise ValueError("no executing load segment checked a direct dispatch against the artificial limits")
    totals = {key: sum(segment[key] for segment in segments) for key in ("instances", *counter_keys)}
    result = {"mode": mode, "pid": process, "loads": len(segments), "segments": segments, **totals}
    if forced:
        result["dispatch_limits"] = list(expected)
        result["dispatch_max"] = [max(s["dispatch_max"][axis] for s in segments) for axis in range(3)]
        result["qualification"] = "artificial limits; not an ordinary Vulkan mandatory-limit proof"
    return result


def audit(path, require_shaders=True, expected_dispatch_limits=None):
    records = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    return audit_records(records, require_shaders=require_shaders, expected_dispatch_limits=expected_dispatch_limits)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--allow-no-shaders", action="store_true", help="device-only controls; does not establish shader execution")
    parser.add_argument("--expected-dispatch-limits", help="explicit artificial X,Y,Z test profile; never ordinary conformance proof")
    args = parser.parse_args()
    try:
        result = audit(args.log, require_shaders=not args.allow_no_shaders, expected_dispatch_limits=args.expected_dispatch_limits)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"Guard evidence FAIL: {error}\n")
    print(json.dumps(result, sort_keys=True))
