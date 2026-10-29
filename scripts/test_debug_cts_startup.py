#!/usr/bin/env python3
# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
"""Check diagnostic replay metadata without launching a debugger or CTS."""
import hashlib
import json
from pathlib import Path
import runpy
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

from run_rt_cts import run_environment


class DiagnosticEnvironmentTests(unittest.TestCase):
    def capture(self, root, mode, profile, guarded):
        executable = root / "deqp-vk.exe"
        executable.write_bytes(b"CPU fixture: never executed")
        library = root / "fixture-layer.dll"
        library.write_bytes(b"CPU fixture: never loaded")
        original_log = root / "original.guard.jsonl"
        original_log.write_text("original evidence\n", encoding="utf-8")
        with patch.dict("os.environ", {"PATH": "saved-driver-path"}, clear=True):
            _, environment = run_environment(mode, Path("fallback") if mode != "native" else None,
                                             guard_dir=Path("guard") if guarded else None)
        environment["overrides"].update(profile)
        summary = {"mode": mode, "cts": str(executable), "cts_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                   "layer": ({"snapshot_library": str(library), "library_sha256": hashlib.sha256(library.read_bytes()).hexdigest()}
                             if mode != "native" else None),
                   "environment": environment, "no_native_rt_guard": {"name": "guard"} if guarded else None,
                   "cases": [{"case": "dEQP-VK.fixture", "command": [str(executable)], "cwd": str(root),
                              "stdout": "unused", "qpa": "unused", "no_native_rt_guard": {"log": str(original_log)}}]}
        summary_path = root / "source.json"
        summary_path.write_text(json.dumps(summary), encoding="utf-8")
        output = root / "capture"
        output.mkdir()
        observed = {}

        def capture_only(api, cdb, command, cwd, env, directory, timeout, trace):
            directory.mkdir()
            observed.update(env)
            return {"events": ["FIRST"]}

        # Preserve the active workspace when the test runs in an isolated overlay.
        capture = runpy.run_path(str(Path(__file__).absolute().parents[1] /
                                    "layers/raytracing/tests/debug_cts_startup.py"))["cts_runs"]
        inherited = {"VKRT_NO_RT_GUARD_LOG": str(original_log), "VKRT_NO_RT_GUARD_MASK_DISCOVERY": "garbage",
                     "VKRT_NO_RT_GUARD_ZERO_TIMESTAMPS": "garbage", "VKRT_NO_RT_GUARD_DISPATCH_LIMITS": "garbage",
                     "VKRT_NO_RT_GUARD_MEMORY_COUNT": "garbage", "PATH": "inherited-driver-path"}
        with patch.dict(capture.__globals__, {"run_one": capture_only}), patch.dict("os.environ", inherited, clear=True):
            capture(SimpleNamespace(cts_running=lambda: []),
                    SimpleNamespace(allow_cts_launch=True, summary=summary_path, case=["dEQP-VK.fixture"], repeats=1,
                                    cdb=Path("unused"), timeout=1, handle_trace=False), output)
        self.assertEqual(original_log.read_text(encoding="utf-8"), "original evidence\n")
        self.assertEqual(observed["PATH"], "saved-driver-path")
        record = json.loads((output / "01-01/capture.json").read_text(encoding="utf-8"))
        return observed, record, output / "01-01/guard.jsonl"

    def test_guard_profile_is_restored_and_evidence_log_is_fresh(self):
        for profile in ({"VKRT_NO_RT_GUARD_MASK_DISCOVERY": "1", "VKRT_NO_RT_GUARD_ZERO_TIMESTAMPS": "1"},
                        {"VKRT_NO_RT_GUARD_DISPATCH_LIMITS": "2,2,64"},
                        {"VKRT_NO_RT_GUARD_MEMORY_COUNT": "4096"}, {}):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as directory:
                observed, record, log = self.capture(Path(directory), "software-rt", profile, True)
                guard_values = {key: value for key, value in observed.items() if key.startswith("VKRT_NO_RT_GUARD_")}
                self.assertEqual(guard_values, {**profile, "VKRT_NO_RT_GUARD_LOG": str(log)})
                self.assertEqual(record["no_native_rt_guard_log"], str(log))

    def test_native_replay_has_no_inherited_guard_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            observed, record, _ = self.capture(Path(directory), "native", {}, False)
        self.assertFalse(any(key.startswith("VKRT_NO_RT_GUARD_") for key in observed))
        self.assertNotIn("VK_INSTANCE_LAYERS", observed)
        self.assertIsNone(record["no_native_rt_guard_log"])


if __name__ == "__main__":
    unittest.main()
