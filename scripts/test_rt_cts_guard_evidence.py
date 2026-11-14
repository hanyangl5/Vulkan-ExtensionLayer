#!/usr/bin/env python3
# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
"""Check that missing, rejected, or incomplete downstream proof cannot pass."""
import json
import contextlib
import io
from pathlib import Path
import runpy
import sys
import tempfile
import unittest
from unittest.mock import patch

from run_rt_cts import classify, guard_evidence, main, qpa_result, run_environment


class RunnerTimeoutTests(unittest.TestCase):
    case = "dEQP-VK.ray_tracing_pipeline.example"

    def test_terminated_qpa_timeout_matches_selected_case(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "case.qpa"
            for result in ("Timeout", "WatchdogTimeout"):
                with self.subTest(result=result):
                    log.write_text(f'#sessionInfo releaseName test\n#beginTestCaseResult {self.case}\n'
                                   f'<TestCaseResult CasePath="{self.case}">\n'
                                   f'#terminateTestCaseResult {result}\n', encoding="utf-8")
                    status, reason, session = qpa_result(log, self.case)
                    self.assertEqual(status, result)
                    self.assertEqual(classify(status, reason, 3221226505, False, None)[0], "timeout")
                    self.assertEqual(session, {"releaseName": "test"})

    def test_timeout_marker_does_not_hide_wrong_case_or_other_crash(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "case.qpa"
            for text in (f"#beginTestCaseResult {self.case}.other\n#terminateTestCaseResult Timeout\n",
                         f"#beginTestCaseResult {self.case}\n#terminateTestCaseResult Crash\n",
                         f"#terminateTestCaseResult Timeout\n#beginTestCaseResult {self.case}\n"):
                with self.subTest(text=text):
                    log.write_text(text, encoding="utf-8")
                    result, reason, _ = qpa_result(log, self.case)
                    self.assertIsNone(result)
                    self.assertEqual(classify(result, reason, 1, False, None)[0], "crash")
            self.assertEqual(classify("Pass", "OK", 1, False, None)[0], "crash")

    def test_watchdog_defaults_override_and_summary(self):
        for timeout, requested, effective in ((90, None, 30), (12.25, None, 13),
                                               (180, 60, 60), (180, 120, 120), (90, 0.25, 1)):
            with self.subTest(timeout=timeout, requested=requested), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                cts, cases, output = root / "fake-cts", root / "cases.txt", root / "evidence"
                cts.write_bytes(b"fake CTS executable; no process is launched")
                cases.write_text(self.case + "\n", encoding="utf-8")
                argv = ["run_rt_cts.py", "--cts", str(cts), "--cases", str(cases), "--mode", "native",
                        "--archive-dir", str(root), "--output-dir", str(output), "--timeout", str(timeout)]
                if requested is not None:
                    argv += ["--watchdog-interval", str(requested)]

                class WatchdogProcess:
                    def wait(self, timeout):
                        return 3

                def launch(command, **kwargs):
                    qpa = Path(next(arg.split("=", 1)[1] for arg in command if arg.startswith("--deqp-log-filename=")))
                    qpa.write_text(f"#beginTestCaseResult {self.case}\n#terminateTestCaseResult Timeout\n", encoding="utf-8")
                    kwargs["stdout"].write(b"FATAL ERROR: Watchdog timer timeout for touch interval\n")
                    return WatchdogProcess()

                with patch.object(sys, "argv", argv), patch("run_rt_cts.subprocess.Popen", side_effect=launch), \
                        patch("run_rt_cts.suppress_windows_error_dialogs"), contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(main(), 1)
                summary = json.loads((output / "summary.json").read_text(encoding="utf-8"))
                self.assertEqual(summary["watchdog_interval_seconds"], effective)
                self.assertEqual(summary["watchdog_interval_requested_seconds"], requested)
                self.assertEqual(summary["counts"]["timeout"], 1)
                self.assertEqual(summary["counts"]["pass"], 0)
                self.assertIn(f"--deqp-watchdog-interval-time-limit={effective}", summary["cases"][0]["command"])

    def test_invalid_watchdog_interval_rejected(self):
        for value in ("0", "-1", "nan", "inf", "-inf"):
            with self.subTest(value=value):
                argv = ["run_rt_cts.py", "--cts", "unused", "--cases", "unused", "--mode", "native",
                        "--archive-dir", "unused", "--output-dir", "unused", "--watchdog-interval=" + value]
                error = io.StringIO()
                with patch.object(sys, "argv", argv), patch("run_rt_cts.execute") as execute, \
                        contextlib.redirect_stderr(error), self.assertRaises(SystemExit) as failure:
                    main()
                self.assertEqual(failure.exception.code, 2)
                self.assertIn("--watchdog-interval must be positive and finite", error.getvalue())
                execute.assert_not_called()


class GuardEvidenceTests(unittest.TestCase):
    def test_zero_timestamps_require_explicit_injection_and_audit(self):
        with patch.dict("os.environ", {"VKRT_NO_RT_GUARD_ZERO_TIMESTAMPS": "1"}):
            for mode in ("native", "software-rt"):
                env, meta = run_environment(mode, None if mode == "native" else Path("fallback"))
                self.assertNotIn("VKRT_NO_RT_GUARD_ZERO_TIMESTAMPS", env)
                self.assertEqual(meta["unset_inherited"]["VKRT_NO_RT_GUARD_ZERO_TIMESTAMPS"], "1")
        env, _ = run_environment("software-rt", Path("fallback"), guard_dir=Path("guard"),
                                 mask_native_rt_discovery=True, zero_timestamps=True)
        self.assertEqual(env["VKRT_NO_RT_GUARD_ZERO_TIMESTAMPS"], "1")
        with self.assertRaises(ValueError):
            run_environment("software-rt", Path("fallback"), zero_timestamps=True)
        with self.assertRaises(ValueError):
            run_environment("native", None, guard_dir=Path("guard"), zero_timestamps=True)
        done = dict(schema_version=2, pid=123, load_id="zero-load", event="summary",
                    mode="mask-discovery+zero-timestamps", devices=1, shader_definitions=3, checked=12, rejected=0,
                    discovery_mask_version=1, zero_timestamp_version=1, rt_boundary_enforced=True,
                    queue_family_queries=2, zero_timestamp_families=1, timestamp_properties_queries=1,
                    timestamp_write_attempts=0, event_signals=2, event_status_queries=1)
        counters = {key: 0 for key in ("devices", "shader_definitions", "checked", "queue_family_queries",
                                      "zero_timestamp_families", "timestamp_properties_queries", "event_signals",
                                      "event_status_queries")}
        records = [dict(done, **counters, event="start"), dict(done, event="instance"), done]
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "zero.jsonl"
            log.write_text("".join(json.dumps(record) + "\n" for record in records), encoding="utf-8")
            evidence, error = guard_evidence(log, expected_discovery_mask=True, expected_zero_timestamps=True)
            self.assertTrue(evidence["verified"], error)
            self.assertFalse(guard_evidence(log, expected_discovery_mask=True)[0]["verified"])
            self.assertFalse(guard_evidence(log, expected_zero_timestamps=True)[0]["verified"])

    def test_masked_discovery_requires_explicit_complete_profile(self):
        audit = runpy.run_path(str(Path(__file__).resolve().parents[1] / "layers/raytracing/tests/no_native_rt_guard_audit.py"))["audit_records"]
        done = dict(schema_version=2, pid=123, load_id="masked-load", event="summary", mode="mask-discovery", devices=1,
                    shader_definitions=3, checked=12, rejected=0, discovery_mask_version=1)
        records = [dict(done, event="start", devices=0, shader_definitions=0, checked=0),
                   dict(done, event="instance"), done]
        self.assertEqual(audit(records, expected_discovery_mask=True)["mode"], "mask-discovery")
        with self.assertRaises(ValueError):
            audit(records)
        for version in (None, 0, True, "1"):
            changed = [dict(r, discovery_mask_version=version) for r in records]
            with self.subTest(version=version), self.assertRaises(ValueError):
                audit(changed, expected_discovery_mask=True)
        with self.assertRaises(ValueError):
            audit(records, expected_discovery_mask=True, expected_dispatch_limits=(2, 2, 64))
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "mask.jsonl"
            log.write_text("".join(json.dumps(r) + "\n" for r in records), encoding="utf-8")
            self.assertTrue(guard_evidence(log, expected_discovery_mask=True)[0]["verified"])
            self.assertFalse(guard_evidence(log)[0]["verified"])
        env, meta = run_environment("software-rt", Path("fallback"), guard_dir=Path("guard"), mask_native_rt_discovery=True)
        self.assertEqual(env["VKRT_NO_RT_GUARD_MASK_DISCOVERY"], "1")
        self.assertEqual(meta["overrides"]["VKRT_NO_RT_GUARD_MASK_DISCOVERY"], "1")
        with self.assertRaises(ValueError):
            run_environment("native", None, guard_dir=Path("guard"), mask_native_rt_discovery=True)
        with self.assertRaises(ValueError):
            run_environment("software-rt", Path("fallback"), mask_native_rt_discovery=True)

    def test_required_execution_evidence(self):
        completed = dict(schema_version=2, pid=123, load_id="test-load-1", event="summary", mode="execution-only", devices=1,
                         shader_definitions=3, checked=12, rejected=0)
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "guard.jsonl"
            evidence, error = guard_evidence(log)
            self.assertFalse(evidence["verified"])
            self.assertIsNotNone(error)
            for records, valid in [
                ([dict(completed, event="instance"), completed], True),
                ([completed], False),
                ([], False),
                ([dict(completed, event="device")], False),
                ([dict(completed, event="instance"), dict(completed, devices=0)], False),
                ([dict(completed, event="instance"), dict(completed, shader_definitions=0)], False),
                ([dict(completed, event="instance"), dict(completed, checked=0)], False),
                ([dict(completed, event="instance"), dict(completed, rejected=1)], False),
                ([dict(completed, event="instance"), dict(completed, event="reject"), completed], False),
                ([dict(completed, event="instance"), dict(completed, mode="mask-discovery")], False),
            ]:
                with self.subTest(records=records):
                    start = dict(completed, event="start", devices=0, shader_definitions=0, checked=0)
                    log.write_text("".join(json.dumps(r) + "\n" for r in [start, *records]), encoding="utf-8")
                    evidence, error = guard_evidence(log)
                    self.assertEqual(evidence["verified"], valid)
                    self.assertEqual(error is None, valid)
                    self.assertEqual(evidence["reject_records"], sum(record["event"] == "reject" for record in records))
            log.write_text('{"event":', encoding="utf-8")
            self.assertIsNotNone(guard_evidence(log)[1])
            log.write_text('{}\n', encoding="utf-8")
            self.assertIsNotNone(guard_evidence(log)[1])

    def test_layer_order_and_native_isolation(self):
        import os
        env, _ = run_environment("software-rt", Path("fallback"), Path("validation"), Path("guard"))
        self.assertEqual(env["VK_INSTANCE_LAYERS"].split(os.pathsep),
                         ["VK_LAYER_VKRT_SOFTWARE_RAY_QUERY", "VK_LAYER_VKRT_NO_NATIVE_RT_GUARD"])
        self.assertEqual(env["VK_LAYER_PATH"].split(os.pathsep), ["fallback", "guard", "validation"])
        env, _ = run_environment("native", None)
        self.assertNotIn("VK_INSTANCE_LAYERS", env)
        self.assertNotIn("VKRT_NO_RT_GUARD_LOG", env)
        self.assertNotIn("VKRT_NO_RT_GUARD_MASK_DISCOVERY", env)

    def test_validation_layer_path_without_rejection_guard(self):
        import os
        for mode, snapshot, expected in (
                ("native", None, ["validation"]),
                ("software-rt", Path("fallback"), ["fallback", "validation"])):
            with self.subTest(mode=mode):
                env, meta = run_environment(mode, snapshot, validation_dir=Path("validation"))
                self.assertEqual(env["VK_LAYER_PATH"].split(os.pathsep), expected)
                self.assertEqual(meta["overrides"]["VK_LAYER_PATH"], env["VK_LAYER_PATH"])
                self.assertEqual(meta["effective_driver_environment"]["VK_LAYER_PATH"], env["VK_LAYER_PATH"])

    def test_reproducibility_environment_records_driver_inputs(self):
        inputs = {"VK_DRIVER_FILES": "driver.json", "DEQP_LOG_LEVEL": "debug", "VEL_BUILD": "local",
                  "MESA_VK_DEVICE_SELECT": "1", "NVIDIA_PROFILE": "test", "__GL_SYNC_TO_VBLANK": "0",
                  "PATH": "audit-bin", "UNRELATED_SETTING": "ignored", "API_TOKEN": "not-for-evidence"}
        with patch.dict("os.environ", inputs, clear=True):
            env, meta = run_environment("native", None)
        recorded = meta["effective_driver_environment"]
        for key in inputs.keys() - {"UNRELATED_SETTING", "API_TOKEN"}:
            with self.subTest(key=key):
                self.assertEqual(recorded[key], env[key])
        self.assertEqual(recorded["VK_LOADER_LAYERS_DISABLE"], env["VK_LOADER_LAYERS_DISABLE"])
        self.assertNotIn("UNRELATED_SETTING", recorded)
        self.assertNotIn("API_TOKEN", recorded)

    def test_forced_dispatch_profile_does_not_leak_into_cts(self):
        with patch.dict("os.environ", {"VKRT_NO_RT_GUARD_DISPATCH_LIMITS": "2,2,64", "VKRT_NO_RT_GUARD_MEMORY_COUNT": "4096"}):
            for mode in ("native", "software-rt"):
                env, meta = run_environment(mode, None if mode == "native" else Path("fallback"))
                self.assertNotIn("VKRT_NO_RT_GUARD_DISPATCH_LIMITS", env)
                self.assertEqual(meta["unset_inherited"]["VKRT_NO_RT_GUARD_DISPATCH_LIMITS"], "2,2,64")
                self.assertNotIn("VKRT_NO_RT_GUARD_MEMORY_COUNT", env)
                self.assertEqual(meta["unset_inherited"]["VKRT_NO_RT_GUARD_MEMORY_COUNT"], "4096")


if __name__ == "__main__":
    unittest.main()
