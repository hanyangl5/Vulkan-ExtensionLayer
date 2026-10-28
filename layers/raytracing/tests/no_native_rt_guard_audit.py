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


def audit_records(records, require_shaders=True, expected_dispatch_limits=None, expected_discovery_mask=False,
                  expected_zero_timestamps=False, expected_memory_count=False):
    if type(expected_memory_count) is not bool:
        raise ValueError("expected memory-count profile must be an explicit boolean")
    if type(expected_discovery_mask) is not bool or type(expected_zero_timestamps) is not bool:
        raise ValueError("expected discovery/zero-timestamp profiles must be explicit booleans")
    if not records or not isinstance(records[0], dict) or records[0].get("event") != "start":
        raise ValueError("guard configuration/start record is missing")
    counter_keys = ("checked", "rejected", "devices", "shader_definitions")
    forced = expected_dispatch_limits is not None
    expected = dispatch_limits(expected_dispatch_limits) if forced else None
    if forced and (expected_discovery_mask or expected_zero_timestamps or expected_memory_count):
        raise ValueError("discovery/zero-timestamp and artificial dispatch are distinct profiles")
    mode = "forced-dispatch-limits" if forced else "mask-discovery" if expected_discovery_mask else "execution-only"
    if expected_zero_timestamps:
        mode = "mask-discovery+zero-timestamps" if expected_discovery_mask else "zero-timestamps"
    zero_counters = ("queue_family_queries", "zero_timestamp_families", "timestamp_write_attempts",
                     "timestamp_properties_queries", "event_signals", "event_status_queries")
    if expected_zero_timestamps:
        counter_keys += zero_counters
    memory_counters = ("memory_properties", "memory_attempts", "memory_allocations", "memory_frees", "memory_peak")
    if expected_memory_count:
        counter_keys += memory_counters
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
        if not isinstance(record, dict) or type(record.get("schema_version")) is not int or record["schema_version"] != 2:
            raise ValueError("guard record schema/identity is missing or unsupported")
        if record.get("pid") != process or type(record.get("pid")) is not int:
            raise ValueError("guard log mixes different process identities")
        if record.get("mode") != mode:
            raise ValueError("guard mode is not the explicitly expected execution/dispatch profile")
        if expected_discovery_mask and (type(record.get("discovery_mask_version")) is not int or
                                        record["discovery_mask_version"] != 1):
            raise ValueError("masked discovery lacks complete feature/property/extension/format/proc masking")
        if not expected_discovery_mask and "discovery_mask_version" in record:
            raise ValueError("masked-discovery fields require an explicit masked profile")
        if expected_zero_timestamps:
            if type(record.get("zero_timestamp_version")) is not int or record["zero_timestamp_version"] != 1:
                raise ValueError("zero-timestamp injection lacks a supported typed profile version")
            if type(record.get("rt_boundary_enforced")) is not bool or record["rt_boundary_enforced"] != expected_discovery_mask:
                raise ValueError("zero-timestamp native/RT rejection boundary differs from the explicitly expected profile")
        elif any(key in record for key in ("zero_timestamp_version", "rt_boundary_enforced", *zero_counters)):
            raise ValueError("zero-timestamp injection needs an explicit dedicated audit")
        if expected_memory_count:
            if (type(record.get("memory_count_version")) is not int or record["memory_count_version"] != 1 or
                    type(record.get("memory_count_limit")) is not int or record["memory_count_limit"] != 4096):
                raise ValueError("memory-count injection lacks the explicit 4096-allocation profile")
            live = record.get("memory_live")
            allocations, frees = record.get("memory_allocations"), record.get("memory_frees")
            peak = record.get("memory_peak")
            attempts = record.get("memory_attempts")
            if (any(type(value) is not int or value < 0 for value in (live, allocations, frees, peak, attempts)) or
                    frees > allocations or allocations > attempts or allocations - frees != live or peak > 4096 or
                    (allocations and not peak)):
                raise ValueError("memory-count allocation/free accounting is inconsistent")
            if record.get("event") == "summary" and active_instances == 1 and live:
                raise ValueError("memory-count instance completed with live native allocations")
        elif any(key in record for key in ("memory_count_version", "memory_count_limit", "memory_live", *memory_counters)):
            raise ValueError("memory-count injection needs an explicit dedicated audit")
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
            if any(type(record.get(key)) is not int or record[key] != 0 for key in counter_keys):
                raise ValueError("guard load start counters are not zero")
            if expected_memory_count and record["memory_live"]:
                raise ValueError("guard memory-count start is not empty")
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
        if expected_zero_timestamps and record["timestamp_write_attempts"]:
            raise ValueError("zero-timestamp execution attempted a forbidden timestamp write")
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
    if expected_zero_timestamps and not any(segment["queue_family_queries"] and segment["zero_timestamp_families"]
                                           for segment in executing):
        raise ValueError("no executing load segment observed zero-timestamp queue-family properties")
    if expected_zero_timestamps and not any(segment["timestamp_properties_queries"] for segment in executing):
        raise ValueError("no executing load segment observed zero-timestamp device properties")
    if expected_memory_count and not any(segment["memory_properties"] and segment["memory_allocations"]
                                         for segment in executing):
        raise ValueError("no executing load segment observed memory-count properties and allocations")
    totals = {key: sum(segment[key] for segment in segments) for key in ("instances", *counter_keys)}
    result = {"mode": mode, "pid": process, "loads": len(segments), "segments": segments, **totals}
    if forced:
        result["dispatch_limits"] = list(expected)
        result["dispatch_max"] = [max(s["dispatch_max"][axis] for s in segments) for axis in range(3)]
        result["qualification"] = "artificial limits; not an ordinary Vulkan mandatory-limit proof"
    if expected_discovery_mask:
        result["discovery_mask_version"] = 1
        result["qualification"] = "masked discovery; not physical no-RT or complete mandatory qualification"
    if expected_zero_timestamps:
        result["zero_timestamp_version"] = 1
        result["rt_boundary_enforced"] = expected_discovery_mask
        result["qualification"] = ("zero timestamps with masked RT boundary; not physical no-RT or complete mandatory qualification"
                                   if expected_discovery_mask else
                                   "zero-timestamp native control; native RT is permitted, not a no-native-RT boundary proof")
    if expected_memory_count:
        result["memory_count_version"] = 1
        result["memory_count_limit"] = 4096
        result["memory_peak"] = max(segment["memory_peak"] for segment in segments)
        result["memory_qualification"] = "injected native allocation-count boundary; not complete public memory delivery"
    return result


def audit(path, require_shaders=True, expected_dispatch_limits=None, expected_discovery_mask=False, expected_zero_timestamps=False,
          expected_memory_count=False):
    records = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    return audit_records(records, require_shaders=require_shaders, expected_dispatch_limits=expected_dispatch_limits,
                         expected_discovery_mask=expected_discovery_mask, expected_zero_timestamps=expected_zero_timestamps,
                         expected_memory_count=expected_memory_count)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--allow-no-shaders", action="store_true", help="device-only controls; does not establish shader execution")
    parser.add_argument("--expected-dispatch-limits", help="explicit artificial X,Y,Z test profile; never ordinary conformance proof")
    parser.add_argument("--expected-discovery-mask", action="store_true", help="explicit masked feature/property/format/proc profile")
    parser.add_argument("--expected-zero-timestamps", action="store_true",
                        help="explicit zero-timestamp injection; alone permits native RT, may combine with discovery mask")
    parser.add_argument("--expected-memory-count", action="store_true", help="explicit native allocation-count limit of 4096")
    args = parser.parse_args()
    try:
        result = audit(args.log, require_shaders=not args.allow_no_shaders, expected_dispatch_limits=args.expected_dispatch_limits,
                       expected_discovery_mask=args.expected_discovery_mask, expected_zero_timestamps=args.expected_zero_timestamps,
                       expected_memory_count=args.expected_memory_count)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"Guard evidence FAIL: {error}\n")
    print(json.dumps(result, sort_keys=True))
