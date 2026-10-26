#!/usr/bin/env python3
# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
"""Run exact external CTS cases against an immutable experimental layer snapshot."""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cts", type=Path, required=True)
    parser.add_argument("--archive-dir", type=Path, required=True)
    parser.add_argument("--cases", type=Path, required=True)
    parser.add_argument("--layer-dir", type=Path, required=True)
    parser.add_argument("--validation-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--backend", choices=("native", "experimental-software"), required=True)
    parser.add_argument("--validation-order", choices=("above", "below"), default="above")
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--cts-revision", default=None)
    args = parser.parse_args()
    cts = args.cts.resolve(strict=True)
    archive = args.archive_dir.resolve(strict=True)
    case_file = args.cases.resolve(strict=True)
    case_list = [line.strip() for line in case_file.read_text().splitlines() if line.strip() and not line.lstrip().startswith("#")]
    if not case_list or len(case_list) != len(set(case_list)) or args.timeout <= 0:
        parser.error("cases must be nonempty and unique; timeout must be positive")
    manifest_source = (args.layer_dir / "VkLayer_VEL_opacity_micromap.json").resolve(strict=True)
    manifest = json.loads(manifest_source.read_text())
    library_source = (manifest_source.parent / manifest["layer"]["library_path"]).resolve(strict=True)
    validation = (args.validation_dir / "VkLayer_khronos_validation.json").resolve(strict=True)
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    snapshot = output / "layer_snapshot"
    snapshot.mkdir()
    shutil.copy2(library_source, snapshot / library_source.name)
    manifest["layer"]["library_path"] = str(snapshot / library_source.name)
    (snapshot / manifest_source.name).write_text(json.dumps(manifest, indent=2) + "\n")
    env = dict(os.environ)
    for variable in ("VK_INSTANCE_LAYERS", "VK_LAYER_PATH", "VK_ADD_LAYER_PATH", "VK_LAYER_ENABLES",
                     "VK_LOADER_LAYERS_ENABLE", "VK_VEL_OMM_EXPERIMENTAL_SOFTWARE"):
        env.pop(variable, None)
    layers = ["VK_LAYER_KHRONOS_validation", "VK_LAYER_VEL_opacity_micromap"]
    if args.validation_order == "below":
        layers.reverse()
    recorded = {"VK_INSTANCE_LAYERS": ";".join(layers), "VK_LAYER_PATH": os.pathsep.join((str(snapshot), str(validation.parent))),
                "VK_LAYER_VALIDATE_SYNC": "1", "VK_LOADER_LAYERS_DISABLE": "~implicit~"}
    if args.backend == "experimental-software":
        recorded["VK_VEL_OMM_EXPERIMENTAL_SOFTWARE"] = "1"
    env.update(recorded)
    summary = {"schema_version": 1, "started_at_utc": datetime.now(timezone.utc).isoformat(), "state": "running",
               "backend_requested": args.backend, "validation_order": args.validation_order,
               "cts": str(cts), "cts_sha256": sha(cts), "cts_revision_declared": args.cts_revision,
               "case_list": str(case_file), "case_list_sha256": sha(case_file), "selected_cases": case_list,
               "manifest_sha256": sha(snapshot / manifest_source.name), "library_sha256": sha(snapshot / library_source.name),
               "validation_manifest": str(validation), "validation_manifest_sha256": sha(validation),
               "environment": recorded, "timeout_seconds": args.timeout, "cases": [],
               "note": "Native mode is pass-through control, never a software OMM result. Experimental mode is incomplete."}

    def save():
        summary["counts"] = dict(Counter(case["status"] for case in summary["cases"]))
        temporary = output / "summary.tmp.json"
        temporary.write_text(json.dumps(summary, indent=2) + "\n")
        temporary.replace(output / "summary.json")

    save()
    for index, name in enumerate(case_list, 1):
        stem = f"{index:04d}-{hashlib.sha256(name.encode()).hexdigest()[:12]}"
        qpa, stdout = output / (stem + ".qpa"), output / (stem + ".stdout.txt")
        command = [str(cts), "--deqp-case=" + name, "--deqp-visibility=hidden", "--deqp-archive-dir=" + str(archive),
                   "--deqp-shadercache=disable", "--deqp-watchdog=enable", "--deqp-crashhandler=enable",
                   "--deqp-watchdog-total-time-limit=" + str(args.timeout),
                   "--deqp-watchdog-interval-time-limit=" + str(max(1, args.timeout // 2)),
                   "--deqp-log-filename=" + str(qpa), "--deqp-vk-validation=enable", "--deqp-vk-print-validation-errors"]
        with stdout.open("wb") as stream:
            process = subprocess.Popen(command, cwd=cts.parent, env=env, stdout=stream, stderr=subprocess.STDOUT)
            try:
                returncode = process.wait(timeout=args.timeout + 5)
                status = None
            except subprocess.TimeoutExpired:
                process.kill()
                returncode = process.wait()
                status = "Timeout"
        text = qpa.read_text(errors="replace") if qpa.exists() else ""
        match = re.search(r'<Result\b[^>]*\bStatusCode="([^"]+)"', text)
        cts_status = match.group(1) if match else None
        diagnostic = bool(re.search(r"Validation Error|SYNC-HAZARD", stdout.read_text(errors="replace"), re.IGNORECASE))
        if status is None:
            status = cts_status or ("Crash" if returncode else "NoResult")
            if returncode and status in ("Pass", "NotSupported"):
                status = "ProcessError"
            if diagnostic and status == "Pass":
                status = "ValidationError"
        summary["cases"].append({"name": name, "status": status, "cts_status": cts_status, "returncode": returncode,
                                 "validation_error": diagnostic, "command": command, "stdout_sha256": sha(stdout),
                                 "qpa_sha256": sha(qpa) if qpa.exists() else None})
        save()
        print(f"[{index}/{len(case_list)}] {status}: {name}", flush=True)
    summary["state"] = "completed"
    summary["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
    save()
    print(summary["counts"], flush=True)
    return int(any(case["status"] not in ("Pass", "NotSupported") for case in summary["cases"]))


if __name__ == "__main__":
    raise SystemExit(main())
