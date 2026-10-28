#!/usr/bin/env python3
# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
"""Run exact external Vulkan CTS cases with isolated, reproducible evidence.

Every case runs in a separate process. Native and fallback runs must use separate
output directories. No selection or filtering is inferred from --mode: use the
same case list for a native control and its corresponding fallback comparison.
--archive-dir is the CTS test-resource directory, not an output archive.

Example (PowerShell):
  python scripts/run_rt_cts.py --cts D:/Codes/VK-GL-CTS/build/external/vulkancts/modules/vulkan/Release/deqp-vk.exe `
    --archive-dir D:/Codes/VK-GL-CTS/external/vulkancts/data --cases cases.txt `
    --mode ray-query --layer-dir build-rq-goal/layers --output-dir build-rt-cts-evidence/rq-run

NotSupported, crashes, timeouts and missing results never count as passes. This
runner records evidence; a small passing slice is not Vulkan conformance.
"""

import argparse
import collections
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET


LAYERS = {
    "ray-query": "VK_LAYER_VKRT_SOFTWARE_RAY_QUERY",
    "software-rt": "VK_LAYER_VKRT_SOFTWARE_RAY_QUERY",
}
STATUSES = ("pass", "fail", "notSupported", "crash", "timeout", "noresult")
LAYER_ENV = (
    "VK_INSTANCE_LAYERS", "VK_LAYER_PATH", "VK_ADD_LAYER_PATH",
    "VK_LOADER_LAYERS_ENABLE", "VK_LOADER_LAYERS_ALLOW", "VK_LOADER_LAYERS_DISABLE",
    "ENABLE_VKRT_SOFTWARE_RAY_QUERY", "DISABLE_VKRT_SOFTWARE_RAY_QUERY",
)


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def hidden_process_options():
    if os.name == "nt":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = subprocess.SW_HIDE
        return {"creationflags": subprocess.CREATE_NO_WINDOW, "startupinfo": startup}
    return {"start_new_session": True}


def suppress_windows_error_dialogs():
    if os.name == "nt":
        # Child processes inherit this mode. A crashed CTS executable must not
        # leave an interactive Windows error dialog waiting behind the run.
        import ctypes
        kernel = ctypes.windll.kernel32
        kernel.SetErrorMode(kernel.GetErrorMode() | 0x0001 | 0x0002 | 0x8000)


def load_cases(path):
    cases = []
    for number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        case = line.split("#", 1)[0].strip()
        if not case:
            continue
        if not re.fullmatch(r"dEQP-VK\.[A-Za-z0-9_.-]+", case):
            raise ValueError(f"{path}:{number}: expected one exact dEQP-VK case, without wildcards")
        if case in cases:
            raise ValueError(f"{path}:{number}: duplicate case {case}")
        cases.append(case)
    if not cases:
        raise ValueError("case list is empty")
    return cases


def snapshot_layer(source_dir, output_dir, mode):
    expected = LAYERS[mode]
    matches = []
    for path in sorted(source_dir.glob("*.json")):
        try:
            raw = path.read_bytes()
            manifest = json.loads(raw)
        except (OSError, UnicodeError, json.JSONDecodeError):
            continue
        if isinstance(manifest, dict) and isinstance(manifest.get("layer"), dict) and manifest["layer"].get("name") == expected:
            matches.append((path, raw, manifest))
    if len(matches) != 1:
        raise ValueError(f"expected exactly one {expected} manifest in {source_dir}; found {len(matches)}")
    source_manifest, manifest_bytes, manifest = matches[0]
    if not isinstance(manifest["layer"].get("library_path"), str):
        raise ValueError(f"invalid library_path in {source_manifest}")
    library = Path(manifest["layer"]["library_path"])
    if not library.is_absolute():
        library = source_manifest.parent / library
    library = library.resolve(strict=True)
    snapshot = output_dir / "layer_snapshot"
    snapshot.mkdir()
    target = snapshot / library.name
    for _ in range(3):
        before = sha256(library)
        shutil.copy2(library, target)
        frozen_hash = sha256(target)
        if frozen_hash == before == sha256(library):
            break
    else:
        raise ValueError("layer library changed while being snapshotted; rerun after the build finishes")
    if source_manifest.read_bytes() != manifest_bytes:
        raise ValueError("layer manifest changed while being snapshotted; rerun after the build finishes")
    # Windows LoadLibraryEx can reject a relative path with error 87 when the
    # loader requests DLL-load-directory search semantics. Resolve the frozen
    # library explicitly so the snapshot works independently of process CWD.
    manifest["layer"]["library_path"] = target.resolve().as_posix()
    frozen_manifest = snapshot / source_manifest.name
    frozen_manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return snapshot, {
        "name": expected,
        "source_manifest": str(source_manifest),
        "source_manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
        "source_library": str(library),
        "snapshot_manifest": str(frozen_manifest),
        "snapshot_manifest_sha256": sha256(frozen_manifest),
        "snapshot_library": str(target),
        "library_sha256": frozen_hash,
        "snapshot_scope": "manifest_and_layer_library",
    }


def run_environment(mode, snapshot, validation_dir=None):
    env = os.environ.copy()
    removed = {key: env.pop(key) for key in LAYER_ENV if key in env}
    overrides = {"VK_LOADER_LAYERS_DISABLE": "~implicit~"}
    if mode != "native":
        overrides.update(VK_INSTANCE_LAYERS=LAYERS[mode], VK_LAYER_PATH=str(snapshot))
        if mode in ("ray-query", "software-rt"):
            overrides["ENABLE_VKRT_SOFTWARE_RAY_QUERY"] = "1"
    if validation_dir:
        overrides["VK_LAYER_PATH"] = os.pathsep.join(
            [str(path) for path in (snapshot, validation_dir) if path is not None])
    env.update(overrides)
    # Keep reproducibility-relevant driver/loader inputs without dumping API
    # keys, credentials or unrelated inherited process environment variables.
    prefixes = ("VK_", "DEQP_", "VEL_", "MESA_", "NVIDIA_", "__GL_")
    relevant = {key: value for key, value in env.items() if key.startswith(prefixes) or key.upper() == "PATH"}
    return env, {"overrides": overrides, "unset_inherited": removed, "effective_driver_environment": relevant}


def qpa_result(path, case):
    if not path.is_file():
        return None, "QPA log was not created", {}
    text = path.read_text(encoding="utf-8", errors="replace")
    session = {}
    for key, value in re.findall(r"(?m)^#sessionInfo\s+(\S+)\s+([^\r\n]*)", text):
        session[key] = value
    records = re.findall(
        r"(?ms)^#beginTestCaseResult\s+([^\r\n]+)\r?\n(.*?)^#endTestCaseResult\s*$", text
    )
    if len(records) != 1 or records[0][0].strip() != case:
        return None, f"expected one complete result for {case}; found {len(records)} complete records", session
    try:
        root = ET.fromstring(records[0][1].strip())
    except ET.ParseError as error:
        return None, f"invalid QPA test-result XML: {error}", session
    results = root.findall("./Result")
    if root.tag != "TestCaseResult" or root.get("CasePath") != case or len(results) != 1:
        return None, "QPA result case identity or Result element does not match", session
    result = results[0]
    return result.get("StatusCode"), "".join(result.itertext()).strip(), session


def classify(result, reason, exit_code, timed_out, launch_error):
    if timed_out:
        return "timeout", "outer per-case timeout exceeded; process killed"
    if launch_error:
        return "noresult", launch_error
    if result in ("Crash", "Terminated"):
        return "crash", reason
    if result in ("Timeout", "WatchdogTimeout"):
        return "timeout", reason
    if result in ("Fail", "InternalError", "ResourceError", "QualityWarning", "CompatibilityWarning"):
        return "fail", reason
    if exit_code != 0:
        return "crash", f"process exited with code {exit_code}; QPA status={result!r}; {reason}"
    if result == "Pass":
        return "pass", reason
    if result == "NotSupported":
        return "notSupported", reason
    return "noresult", reason or f"unrecognized or missing QPA result: {result!r}"


def write_summary(path, summary):
    counts = collections.Counter(item["status"] for item in summary["cases"])
    summary["counts"] = {status: counts[status] for status in STATUSES}
    summary["updated_at_utc"] = utc_now()
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def execute(args):
    cts = args.cts.resolve(strict=True)
    archive = args.archive_dir.resolve(strict=True)
    if not cts.is_file() or not archive.is_dir():
        raise ValueError("--cts must be a file and --archive-dir must be the CTS resource directory")
    cases = load_cases(args.cases)
    output = args.output_dir.resolve()
    if output.exists() and any(output.iterdir()):
        raise ValueError("--output-dir must be new or empty; preserve prior run evidence in a separate directory")
    output.mkdir(parents=True, exist_ok=True)
    snapshot, layer = (None, None)
    if args.mode != "native":
        snapshot, layer = snapshot_layer(args.layer_dir.resolve(strict=True), output, args.mode)
    validation = None
    validation_dir = args.validation_layer_dir.resolve(strict=True) if args.validation_layer_dir else None
    if validation_dir:
        manifest_path = validation_dir / "VkLayer_khronos_validation.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if manifest.get("layer", {}).get("name") != "VK_LAYER_KHRONOS_validation":
            raise ValueError(f"expected Khronos validation manifest in {validation_dir}")
        library = Path(manifest["layer"]["library_path"])
        if not library.is_absolute():
            library = validation_dir / library
        validation = {"manifest": str(manifest_path), "manifest_sha256": sha256(manifest_path),
                      "library": str(library.resolve(strict=True)), "library_sha256": sha256(library),
                      "scope": "original SDK installation, hashed but not copied"}
    env, env_record = run_environment(args.mode, snapshot, validation_dir)
    device_info = None
    if args.device_info_file:
        device_source = args.device_info_file.resolve(strict=True)
        device_target = output / "device_info.txt"
        shutil.copy2(device_source, device_target)
        device_info = {"source": str(device_source), "snapshot": str(device_target), "sha256": sha256(device_target),
                       "note": "User-supplied device report, not independently validated by this runner."}
    binary_stat = (cts.stat().st_size, cts.stat().st_mtime_ns)
    summary = {
        "schema_version": 1,
        "started_at_utc": utc_now(),
        "state": "running",
        "mode": args.mode,
        "cts": str(cts),
        "cts_sha256": sha256(cts),
        "cts_binary_size": binary_stat[0],
        "cts_revision_declared": args.cts_revision,
        "resource_archive": str(archive),
        "case_list": str(args.cases.resolve()),
        "case_list_sha256": sha256(args.cases),
        "selected_cases": cases,
        "layer": layer,
        "validation": validation,
        "layer_revision_declared": args.layer_revision,
        "device_info": device_info,
        "environment": env_record,
        "timeout_seconds": args.timeout,
        "cases": [],
        "note": "Each case is isolated. NotSupported/noresult/crash/timeout are not passes; this slice is not conformance.",
    }
    summary_path = output / "summary.json"
    write_summary(summary_path, summary)
    suppress_windows_error_dialogs()
    for ordinal, case in enumerate(cases, 1):
        stem = f"{ordinal:04d}-{case.split('.')[-1][:40]}-{hashlib.sha256(case.encode()).hexdigest()[:10]}"
        qpa = output / (stem + ".qpa")
        stdout = output / (stem + ".stdout.txt")
        watchdog = max(1, math.ceil(args.timeout))
        command = [str(cts), f"--deqp-case={case}", "--deqp-visibility=hidden", f"--deqp-archive-dir={archive}",
                   "--deqp-shadercache=disable", "--deqp-watchdog=enable", "--deqp-crashhandler=enable",
                   f"--deqp-watchdog-total-time-limit={watchdog}",
                   f"--deqp-watchdog-interval-time-limit={min(30, watchdog)}", f"--deqp-log-filename={qpa}"]
        if validation_dir:
            command += ["--deqp-vk-validation=enable", "--deqp-vk-print-validation-errors"]
        print(f"[{ordinal}/{len(cases)}] {args.mode}: {case}", flush=True)
        start = time.monotonic()
        timed_out, launch_error, exit_code = False, None, None
        if (cts.stat().st_size, cts.stat().st_mtime_ns) != binary_stat:
            launch_error = "CTS binary changed after run metadata was captured; case was not executed"
            stdout.write_text(launch_error + "\n", encoding="utf-8")
        else:
            with stdout.open("wb") as log:
                try:
                    process = subprocess.Popen(command, cwd=output, env=env, stdin=subprocess.DEVNULL,
                                               stdout=log, stderr=subprocess.STDOUT, shell=False, **hidden_process_options())
                    try:
                        exit_code = process.wait(timeout=args.timeout)
                    except subprocess.TimeoutExpired:
                        timed_out = True
                        if os.name == "nt":
                            process.kill()
                        else:
                            os.killpg(process.pid, signal.SIGKILL)
                        exit_code = process.wait()
                except OSError as error:
                    launch_error = f"process launch/termination failed: {error}"
                    log.write((launch_error + "\n").encode())
        result, reason, session = qpa_result(qpa, case)
        status, reason = classify(result, reason, exit_code, timed_out, launch_error)
        entry = {"case": case, "status": status, "result": result, "reason": reason, "exit_code": exit_code,
                 "elapsed_seconds": round(time.monotonic() - start, 6), "command": command, "cwd": str(output),
                 "stdout": str(stdout), "qpa": str(qpa), "session_info": session}
        summary["cases"].append(entry)
        write_summary(summary_path, summary)
        print(f"  {status} ({entry['elapsed_seconds']:.2f}s): {reason}", flush=True)
    summary["state"] = "completed"
    summary["finished_at_utc"] = utc_now()
    write_summary(summary_path, summary)
    print(json.dumps(summary["counts"], sort_keys=True), flush=True)
    print(f"Evidence: {summary_path}", flush=True)
    return 1 if any(item["status"] in ("fail", "crash", "timeout", "noresult") for item in summary["cases"]) else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cts", required=True, type=Path)
    parser.add_argument("--cases", required=True, type=Path, help="one exact CTS case per line; # comments allowed")
    parser.add_argument("--mode", required=True, choices=("native", "ray-query", "software-rt"))
    parser.add_argument("--layer-dir", type=Path, help="directory containing the chosen fallback manifest")
    parser.add_argument("--output-dir", required=True, type=Path, help="new or empty evidence directory")
    parser.add_argument("--archive-dir", required=True, type=Path, help="CTS test-resource directory")
    parser.add_argument("--timeout", type=float, default=90, help="outer timeout per exact case, seconds")
    parser.add_argument("--cts-revision", help="declared CTS source revision; binary SHA and QPA release are recorded separately")
    parser.add_argument("--layer-revision", help="unvalidated revision/build label for the snapshotted fallback library")
    parser.add_argument("--device-info-file", type=Path, help="snapshot a user-supplied device report beside the logs")
    parser.add_argument("--validation-layer-dir", type=Path,
                        help="enable CTS Vulkan validation using this SDK Khronos validation manifest directory")
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    if args.mode != "native" and args.layer_dir is None:
        parser.error("fallback modes require --layer-dir")
    if args.mode == "native" and args.layer_dir is not None:
        parser.error("native control cannot use --layer-dir")
    try:
        return execute(args)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(2, f"runner error: {error}\n")


if __name__ == "__main__":
    sys.exit(main())
