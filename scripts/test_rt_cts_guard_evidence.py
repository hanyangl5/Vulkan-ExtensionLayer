#!/usr/bin/env python3
# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
"""Check that missing, rejected, or incomplete downstream proof cannot pass."""
import json
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import patch

from run_rt_cts import guard_evidence, run_environment


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

    def test_forced_dispatch_profile_does_not_leak_into_cts(self):
        with patch.dict("os.environ", {"VKRT_NO_RT_GUARD_DISPATCH_LIMITS": "2,2,64"}):
            for mode in ("native", "software-rt"):
                env, meta = run_environment(mode, None if mode == "native" else Path("fallback"))
                self.assertNotIn("VKRT_NO_RT_GUARD_DISPATCH_LIMITS", env)
                self.assertEqual(meta["unset_inherited"]["VKRT_NO_RT_GUARD_DISPATCH_LIMITS"], "2,2,64")


if __name__ == "__main__":
    unittest.main()
