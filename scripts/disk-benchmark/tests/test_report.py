#!/usr/bin/env python3
# Copyright (c) 2026 vesoft inc. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import importlib.util
import json
import math
import tempfile
import unittest
from pathlib import Path


REPORT_PATH = Path(__file__).resolve().parents[1] / "lib" / "report.py"
SPEC = importlib.util.spec_from_file_location("disk_benchmark_report", REPORT_PATH)
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)


def latency(unit, p99, mean=None):
    return {
        "clat_{}".format(unit): {
            "mean": p99 / 2 if mean is None else mean,
            "percentile": {"99.000000": p99},
        }
    }


def direction(iops, bandwidth, unit="ns", p99=1000, use_bw_bytes=True):
    result = {
        "total_ios": max(int(iops), 1),
        "io_bytes": max(int(bandwidth), 1),
        "iops": iops,
    }
    result["bw_bytes" if use_bw_bytes else "bw"] = bandwidth
    result.update(latency(unit, p99))
    return result


def zero_direction():
    return {
        "total_ios": 0,
        "io_bytes": 0,
        "iops": 0,
        "bw_bytes": 0,
        "clat_ns": {"mean": 0, "percentile": {"99.000000": 0}},
    }


class ReportTest(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)

    def tearDown(self):
        self.temp_dir.cleanup()

    def write_json(self, relative_path, value):
        path = self.root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def write_fio(self, relative_path, jobs):
        return self.write_json(relative_path, {"fio version": "fio-3.35", "jobs": jobs})

    def manifest(self, run_dir, **overrides):
        parameters = {
            "preset": "standard",
            "size": "8G",
            "runtime": 60,
            "ramp": 10,
            "cooldown": 5,
            "repeat": 3,
            "jobs": 4,
            "iodepth": 8,
            "read_mix": 70,
            "profiles": ["randrw_8k"],
            "profile_hash": "test-profile-hash",
            "fio_version": "fio-3.35",
            "tool_version": "1.0",
            "randseed": 12345,
            "target": "/data",
            "wal": "/wal",
        }
        parameters.update(overrides)
        run_dir.mkdir(parents=True, exist_ok=True)
        (run_dir / "manifest.json").write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "label": run_dir.name,
                    "status": "complete",
                    "parameters": parameters,
                }
            ),
            encoding="utf-8",
        )

    def test_bandwidth_latency_units_and_sync_latency(self):
        path = self.write_fio(
            "units.json",
            [
                {
                    "read": direction(10, 4096, unit="ns", p99=5000, use_bw_bytes=True),
                    "write": zero_direction(),
                    "sync": {"total_ios": 0, "lat_ns": {}},
                },
                {
                    "read": zero_direction(),
                    "write": direction(20, 10, unit="us", p99=7, use_bw_bytes=False),
                    "sync": {
                        "total_ios": 20,
                        "lat_ms": {
                            "mean": 0.1,
                            "percentile": {"99.000000": 0.2},
                        },
                    },
                },
            ],
        )

        parsed = report.parse_fio_result(path, "wal_sync", 1)
        read = parsed["directions"]["read"]
        write = parsed["directions"]["write"]
        self.assertEqual(4096, read["bw_bytes_per_sec"])
        self.assertEqual(5000, read["p99_latency_ns"])
        self.assertEqual(10 * 1024, write["bw_bytes_per_sec"])
        self.assertEqual(7000, write["completion_p99_latency_ns"])
        self.assertEqual(200000, write["sync_p99_latency_ns"])
        self.assertEqual(200000, write["p99_latency_ns"])
        self.assertEqual("sync", write["latency_source"])

        millisecond = report._latency(latency("ms", 1.25), completion=True)
        self.assertEqual(1250000, millisecond["p99_ns"])

        tails = report._latency(
            {
                "clat_us": {
                    "mean": 4,
                    "percentile": {
                        "50.000000": 2,
                        "95.000000": 5,
                        "99.000000": 8,
                        "99.900000": 13,
                    },
                }
            }
        )
        self.assertEqual(2000, tails["p50_ns"])
        self.assertEqual(5000, tails["p95_ns"])
        self.assertEqual(8000, tails["p99_ns"])
        self.assertEqual(13000, tails["p99_9_ns"])

    def test_summarize_median_cv_and_both_directions(self):
        run_dir = self.root / "run-a"
        self.manifest(run_dir)
        for round_number, iops in enumerate((100, 200, 300), 1):
            path = run_dir / "raw" / "round-{:02d}".format(round_number) / "randrw_8k.json"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(
                json.dumps(
                    {
                        "jobs": [
                            {
                                "read": direction(iops, iops * 1024, p99=iops * 10),
                                "write": direction(iops / 2, iops * 512, p99=iops * 20),
                                "sync": {"total_ios": 0, "lat_ns": {}},
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )

        summary = report.summarize_run(run_dir)
        self.assertEqual({"read", "write"}, set(summary["profiles"]["randrw_8k"]))
        read = summary["profiles"]["randrw_8k"]["read"]
        stats = read["metrics"]["iops"]
        self.assertEqual(3, read["round_count"])
        self.assertEqual(200, stats["median"])
        self.assertEqual(100, stats["min"])
        self.assertEqual(300, stats["max"])
        self.assertTrue(math.isclose(stats["cv_percent"], 40.8248290463863))
        self.assertTrue(any("CV" in warning for warning in summary["warnings"]))
        self.assertTrue((run_dir / "summary.json").is_file())
        markdown = (run_dir / "summary.md").read_text(encoding="utf-8")
        self.assertIn("randrw_8k", markdown)
        self.assertIn("CV", markdown)

    def test_zero_io_direction_is_not_reported(self):
        path = self.write_fio(
            "seq_write.json",
            [
                {
                    "read": zero_direction(),
                    "write": direction(50, 8192, p99=8000),
                    "sync": {"total_ios": 0, "lat_ns": {}},
                }
            ],
        )
        parsed = report.parse_fio_result(path)
        self.assertNotIn("read", parsed["directions"])
        self.assertIn("write", parsed["directions"])

    def test_comparison_metric_direction(self):
        higher = report._compare_metric(100, 120, True, 10)
        lower = report._compare_metric(100, 80, False, 10)
        slower = report._compare_metric(100, 120, False, 10)
        self.assertEqual("improved", higher["status"])
        self.assertEqual(20, higher["performance_change_percent"])
        self.assertEqual("improved", lower["status"])
        self.assertEqual(20, lower["performance_change_percent"])
        self.assertEqual("regressed", slower["status"])

    def test_compare_detects_core_parameter_mismatch(self):
        baseline_dir = self.root / "baseline"
        candidate_dir = self.root / "candidate"
        output_dir = self.root / "comparison"
        self.manifest(baseline_dir)
        self.manifest(candidate_dir, jobs=8, profile_hash="different-profile-hash")
        summary_template = {
            "schema_version": 1,
            "profiles": {
                "randrw_8k": {
                    "read": {
                        "metrics": {
                            "iops": {"median": 100},
                            "bw_bytes_per_sec": {"median": 1048576},
                            "p99_latency_ns": {"median": 10000},
                        }
                    }
                }
            },
        }
        for run_dir in (baseline_dir, candidate_dir):
            summary = dict(summary_template)
            summary["manifest"] = json.loads(
                (run_dir / "manifest.json").read_text(encoding="utf-8")
            )
            summary["parameters"] = summary["manifest"]["parameters"]
            (run_dir / "summary.json").write_text(json.dumps(summary), encoding="utf-8")

        comparison = report.compare_runs(baseline_dir, candidate_dir, output_dir)
        self.assertFalse(comparison["comparable"])
        self.assertEqual("jobs", comparison["parameter_mismatches"][0]["parameter"])
        self.assertIn(
            "profile_hash",
            {item["parameter"] for item in comparison["parameter_mismatches"]},
        )
        self.assertTrue((output_dir / "comparison.json").is_file())
        self.assertIn("参数不一致", (output_dir / "comparison.md").read_text(encoding="utf-8"))
        exit_code = report.main(
            [
                "compare",
                "--baseline",
                str(baseline_dir),
                "--candidate",
                str(candidate_dir),
                "--output-dir",
                str(output_dir),
            ]
        )
        self.assertEqual(0, exit_code)

    def test_compare_rejects_incomplete_or_partial_runs(self):
        baseline_dir = self.root / "baseline-validity"
        candidate_dir = self.root / "candidate-validity"
        output_dir = self.root / "comparison-validity"
        self.manifest(baseline_dir)
        self.manifest(candidate_dir)
        for run_dir, warnings in ((baseline_dir, []), (candidate_dir, ["缺少第 3 轮"])):
            manifest = json.loads(
                (run_dir / "manifest.json").read_text(encoding="utf-8")
            )
            summary = {
                "schema_version": 1,
                "manifest": manifest,
                "parameters": manifest["parameters"],
                "profiles": {},
                "warnings": warnings,
            }
            (run_dir / "summary.json").write_text(json.dumps(summary), encoding="utf-8")

        candidate_manifest = json.loads(
            (candidate_dir / "manifest.json").read_text(encoding="utf-8")
        )
        candidate_manifest["status"] = "failed"
        (candidate_dir / "manifest.json").write_text(
            json.dumps(candidate_manifest), encoding="utf-8"
        )

        comparison = report.compare_runs(baseline_dir, candidate_dir, output_dir)
        self.assertFalse(comparison["comparable"])
        self.assertEqual(2, len(comparison["validity_issues"]))
        markdown = (output_dir / "comparison.md").read_text(encoding="utf-8")
        self.assertIn("结果完整性问题", markdown)
        self.assertIn("failed", markdown)

    def test_selected_config_does_not_leak_unrelated_options(self):
        config = self.root / "nebula-storaged.conf"
        config.write_text(
            "--data_path=/data\n"
            "--rocksdb_block_cache=4096\n"
            "--disable_page_cache=true\n"
            "--minimum_reserved_bytes=1073741824\n"
            "--local_ip=192.0.2.1\n"
            "--meta_server_addrs=secret.example:9559\n",
            encoding="utf-8",
        )
        selected = report._selected_nebula_config(config)["selected_options"]
        self.assertEqual("/data", selected["data_path"])
        self.assertIn("rocksdb_block_cache", selected)
        self.assertIn("disable_page_cache", selected)
        self.assertIn("minimum_reserved_bytes", selected)
        self.assertNotIn("local_ip", selected)
        self.assertNotIn("meta_server_addrs", selected)


if __name__ == "__main__":
    unittest.main()
