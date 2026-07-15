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

"""Environment collection and reporting for the Nebula disk benchmark."""

import argparse
import datetime
import json
import math
import os
import platform
import re
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path


SCHEMA_VERSION = 1
CORE_PARAMETERS = (
    "preset",
    "size",
    "runtime",
    "ramp",
    "cooldown",
    "repeat",
    "jobs",
    "iodepth",
    "read_mix",
    "profiles",
    "profile_hash",
    "fio_version",
    "tool_version",
    "randseed",
)
NUMERIC_METRICS = (
    "iops",
    "bw_bytes_per_sec",
    "p50_latency_ns",
    "p95_latency_ns",
    "p99_latency_ns",
    "p99_9_latency_ns",
    "completion_p99_latency_ns",
    "completion_p99_9_latency_ns",
    "sync_p99_latency_ns",
    "sync_p99_9_latency_ns",
    "mean_latency_ns",
)
COMPARISON_METRICS = (
    ("iops", True, "IOPS"),
    ("bw_bytes_per_sec", True, "带宽"),
    ("p99_latency_ns", False, "P99 延迟"),
    ("p99_9_latency_ns", False, "P99.9 延迟"),
)
EXPECTED_DIRECTIONS = {
    "seq_write": ("write",),
    "seq_read": ("read",),
    "randread_8k_qd1": ("read",),
    "randread_8k": ("read",),
    "randrw_8k": ("read", "write"),
    "compaction": ("read", "write"),
    "raft_wal": ("write",),
    "wal_sync": ("write",),
}


def _utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z")


def _atomic_write(path, content):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    handle = tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", dir=str(path.parent), delete=False
    )
    try:
        with handle:
            handle.write(content)
        os.replace(handle.name, path)
    except BaseException:
        try:
            os.unlink(handle.name)
        except OSError:
            pass
        raise


def _write_json(path, value):
    _atomic_write(path, json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n")


def _read_json(path):
    with Path(path).open("r", encoding="utf-8") as stream:
        return json.load(stream)


def _run_command(command, parse_json=False):
    """Run a diagnostic command without making collection depend on it."""
    record = {"command": command}
    try:
        result = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            encoding="utf-8",
            errors="replace",
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError) as error:
        record.update({"available": False, "error": str(error)})
        return record

    record.update({"available": True, "returncode": result.returncode})
    stdout = result.stdout.strip()
    if parse_json and stdout:
        try:
            record["data"] = json.loads(stdout)
        except json.JSONDecodeError:
            record["text"] = stdout
    elif stdout:
        record["text"] = stdout
    if result.stderr.strip():
        record["error"] = result.stderr.strip()
    return record


def _os_release():
    values = {}
    try:
        with open("/etc/os-release", "r", encoding="utf-8") as stream:
            for line in stream:
                if "=" not in line:
                    continue
                key, value = line.rstrip().split("=", 1)
                if key in ("ID", "VERSION_ID", "PRETTY_NAME"):
                    values[key.lower()] = value.strip().strip('"')
    except OSError:
        pass
    return values


def _cpu_info():
    model = None
    sockets = set()
    cores = set()
    current = {}
    try:
        with open("/proc/cpuinfo", "r", encoding="utf-8", errors="replace") as stream:
            for raw_line in stream:
                line = raw_line.strip()
                if not line:
                    if current:
                        physical = current.get("physical id", "0")
                        core = current.get("core id", current.get("processor"))
                        sockets.add(physical)
                        if core is not None:
                            cores.add((physical, core))
                        current = {}
                    continue
                if ":" in line:
                    key, value = line.split(":", 1)
                    current[key.strip()] = value.strip()
                    if model is None and key.strip() in ("model name", "Hardware", "Processor"):
                        model = value.strip()
            if current:
                physical = current.get("physical id", "0")
                core = current.get("core id", current.get("processor"))
                sockets.add(physical)
                if core is not None:
                    cores.add((physical, core))
    except OSError:
        pass
    result = {"logical_cores": os.cpu_count(), "model": model}
    if sockets:
        result["sockets"] = len(sockets)
    if cores:
        result["physical_cores"] = len(cores)
    return result


def _memory_info():
    wanted = ("MemTotal", "MemAvailable", "MemFree", "Buffers", "Cached", "SwapTotal", "SwapFree")
    result = {}
    try:
        with open("/proc/meminfo", "r", encoding="utf-8") as stream:
            for line in stream:
                key, _, value = line.partition(":")
                if key not in wanted:
                    continue
                fields = value.split()
                if fields:
                    multiplier = 1024 if len(fields) > 1 and fields[1].lower() == "kb" else 1
                    result[key] = int(fields[0]) * multiplier
    except (OSError, ValueError):
        pass
    return result


def _statvfs(path):
    resolved = str(Path(path).expanduser().resolve())
    result = {"path": str(path), "resolved_path": resolved}
    try:
        stat = os.statvfs(resolved)
        fragment_size = stat.f_frsize or stat.f_bsize
        result.update(
            {
                "block_size": stat.f_bsize,
                "fragment_size": fragment_size,
                "total_bytes": stat.f_blocks * fragment_size,
                "free_bytes": stat.f_bfree * fragment_size,
                "available_bytes": stat.f_bavail * fragment_size,
                "total_inodes": stat.f_files,
                "free_inodes": stat.f_ffree,
            }
        )
    except OSError as error:
        result["error"] = str(error)
    findmnt = _run_command(
        [
            "findmnt",
            "--json",
            "--target",
            resolved,
            "--output",
            "SOURCE,TARGET,FSTYPE,OPTIONS,SIZE,AVAIL,USE%",
        ],
        parse_json=True,
    )
    if findmnt.get("returncode") != 0:
        fallback = _run_command(
            [
                "findmnt",
                "--target",
                resolved,
                "--output",
                "SOURCE,TARGET,FSTYPE,OPTIONS",
            ]
        )
        fallback["previous_attempt"] = findmnt
        findmnt = fallback
    result["findmnt"] = findmnt
    return result


def _lsblk_info():
    modern = _run_command(
        [
            "lsblk",
            "--json",
            "--bytes",
            "--output",
            "NAME,KNAME,PATH,TYPE,SIZE,ROTA,MODEL,FSTYPE,MOUNTPOINTS,PKNAME,SCHED",
        ],
        parse_json=True,
    )
    if modern.get("returncode") == 0:
        return modern
    fallback = _run_command(
        [
            "lsblk",
            "--bytes",
            "--output",
            "NAME,KNAME,TYPE,SIZE,ROTA,FSTYPE,MOUNTPOINT",
        ]
    )
    fallback["previous_attempt"] = modern
    if fallback.get("returncode") == 0:
        return fallback
    plain = _run_command(["lsblk"])
    plain["previous_attempt"] = fallback
    return plain


def _nebula_storage_processes():
    processes = []
    proc = Path("/proc")
    if not proc.is_dir():
        return processes
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            raw = (entry / "cmdline").read_bytes()
            fields = [item.decode("utf-8", "replace") for item in raw.split(b"\0") if item]
        except OSError:
            continue
        if not fields or Path(fields[0]).name not in (
            "nebula-storaged",
            "nebula-metad",
            "nebula-standalone",
        ):
            continue
        item = {
            "pid": int(entry.name),
            "executable": fields[0],
            "role": Path(fields[0]).name,
        }
        for index, argument in enumerate(fields[1:]):
            for option in ("--flagfile", "--config"):
                if argument.startswith(option + "="):
                    item["config_file"] = argument.split("=", 1)[1]
                elif argument == option and index + 2 < len(fields):
                    item["config_file"] = fields[index + 2]
        processes.append(item)
    return sorted(processes, key=lambda item: item["pid"])


def _is_io_config_key(key):
    key = key.lower()
    exact = {
        "data_path",
        "wal_path",
        "rocksdb_wal_dir",
        "disable_page_cache",
        "num_compaction_threads",
        "minimum_reserved_bytes",
    }
    return (
        key in exact
        or key.startswith("wal_")
        or key.startswith("rocksdb_")
        or re.search(r"(^|_)(block|cache|compaction)(_|$)", key) is not None
    )


def _selected_nebula_config(path):
    result = {"path": str(Path(path).expanduser().resolve()), "selected_options": {}}
    try:
        lines = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as error:
        result["error"] = str(error)
        return result

    selected = result["selected_options"]
    for raw_line in lines:
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("--"):
            line = line[2:]
        if "=" in line:
            key, value = line.split("=", 1)
        else:
            fields = line.split(None, 1)
            if len(fields) != 2:
                continue
            key, value = fields
        key = key.strip()
        if _is_io_config_key(key):
            selected[key] = value.strip()
    return result


def collect_environment(
    output,
    label,
    target_dir,
    wal_target_dir,
    fio_bin,
    nebula_config=None,
    nebula_home=None,
):
    fio = _run_command([fio_bin, "--version"])
    fio_version = fio.get("text", "").splitlines()
    if fio_version:
        fio["version"] = fio_version[0]
    load_average = None
    try:
        load_average = dict(zip(("1m", "5m", "15m"), os.getloadavg()))
    except (AttributeError, OSError):
        pass
    processes = _nebula_storage_processes()
    report = {
        "schema_version": SCHEMA_VERSION,
        "collected_at": _utc_now(),
        "label": label,
        "system": {
            "hostname": platform.node(),
            "platform": platform.platform(),
            "kernel": platform.release(),
            "machine": platform.machine(),
            "os_release": _os_release(),
            "cpu": _cpu_info(),
            "memory_bytes": _memory_info(),
            "load_average": load_average,
        },
        "fio": fio,
        "filesystems": {
            "target": _statvfs(target_dir),
            "wal_target": _statvfs(wal_target_dir),
        },
        "block_devices": _lsblk_info(),
        "nebula_storage_processes": {
            "active": bool(processes),
            "count": len(processes),
            "processes": processes,
        },
    }
    if nebula_config:
        report["nebula_io_config"] = _selected_nebula_config(nebula_config)
    if nebula_home:
        report["nebula_home"] = str(Path(nebula_home).expanduser().resolve())
    _write_json(output, report)
    return report


def _normalize_profiles(values):
    if isinstance(values, str):
        values = [values]
    profiles = []
    for value in values or []:
        for profile in re.split(r"[,\s]+", value.strip()):
            if profile and profile not in profiles:
                profiles.append(profile)
    return profiles


def init_run(
    output,
    label,
    preset,
    size,
    runtime,
    ramp,
    repeat,
    jobs,
    iodepth,
    profiles,
    target,
    wal,
    tool_version,
    read_mix=70,
    fio_version=None,
    randseed=1,
    cooldown=0,
    profile_hash=None,
):
    parameters = {
        "preset": preset,
        "size": size,
        "runtime": runtime,
        "ramp": ramp,
        "cooldown": cooldown,
        "repeat": repeat,
        "jobs": jobs,
        "iodepth": iodepth,
        "read_mix": read_mix,
        "profiles": _normalize_profiles(profiles),
        "profile_hash": profile_hash,
        "target": target,
        "wal": wal,
        "fio_version": fio_version,
        "tool_version": tool_version,
        "randseed": randseed,
    }
    manifest = {
        "schema_version": SCHEMA_VERSION,
        "label": label,
        "status": "running",
        "started_at": _utc_now(),
        "finished_at": None,
        "parameters": parameters,
    }
    _write_json(output, manifest)
    return manifest


def finish_run(manifest_path, status):
    manifest = _read_json(manifest_path)
    manifest["status"] = status
    manifest["finished_at"] = _utc_now()
    _write_json(manifest_path, manifest)
    return manifest


def _number(value):
    if isinstance(value, bool):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def _percentile(container, percentile=99.0):
    if not isinstance(container, dict):
        return None
    values = container.get("percentile")
    if not isinstance(values, dict) or not values:
        return None
    for key, value in values.items():
        key_number = _number(key)
        value_number = _number(value)
        if (
            key_number is not None
            and value_number is not None
            and math.isclose(key_number, percentile, rel_tol=0.0, abs_tol=0.0001)
        ):
            return value_number
    return None


def _latency(section, completion=True):
    if not isinstance(section, dict):
        return {
            "p50_ns": None,
            "p95_ns": None,
            "p99_ns": None,
            "p99_9_ns": None,
            "mean_ns": None,
            "source": None,
        }
    prefixes = ("clat", "lat") if completion else ("lat", "clat")
    for prefix in prefixes:
        for suffix, multiplier in (("ns", 1.0), ("us", 1000.0), ("ms", 1000000.0)):
            key = prefix + "_" + suffix
            value = section.get(key)
            if not isinstance(value, dict):
                continue
            mean = _number(value.get("mean"))
            return {
                "p50_ns": _scaled_percentile(value, 50.0, multiplier),
                "p95_ns": _scaled_percentile(value, 95.0, multiplier),
                "p99_ns": _scaled_percentile(value, 99.0, multiplier),
                "p99_9_ns": _scaled_percentile(value, 99.9, multiplier),
                "mean_ns": mean * multiplier if mean is not None else None,
                "source": key,
            }
    return {
        "p50_ns": None,
        "p95_ns": None,
        "p99_ns": None,
        "p99_9_ns": None,
        "mean_ns": None,
        "source": None,
    }


def _scaled_percentile(container, percentile, multiplier):
    value = _percentile(container, percentile)
    return value * multiplier if value is not None else None


def _bandwidth_bytes(section):
    value = _number(section.get("bw_bytes"))
    if value is not None:
        return value
    value = _number(section.get("bw"))
    return value * 1024.0 if value is not None else 0.0


def _direction_is_active(section):
    if not isinstance(section, dict):
        return False
    for key in ("total_ios", "io_bytes", "io_kbytes", "iops", "bw_bytes", "bw"):
        value = _number(section.get(key))
        if value is not None and value > 0:
            return True
    return False


def _aggregate_job_direction(jobs, direction):
    samples = []
    for job in jobs:
        section = job.get(direction)
        if not _direction_is_active(section):
            continue
        latency = _latency(section, completion=True)
        total_ios = _number(section.get("total_ios")) or 0.0
        sample = {
            "iops": _number(section.get("iops")) or 0.0,
            "bw_bytes_per_sec": _bandwidth_bytes(section),
            "io_bytes": _number(section.get("io_bytes")) or 0.0,
            "total_ios": total_ios,
            "completion_p50_latency_ns": latency["p50_ns"],
            "completion_p95_latency_ns": latency["p95_ns"],
            "completion_p99_latency_ns": latency["p99_ns"],
            "completion_p99_9_latency_ns": latency["p99_9_ns"],
            "mean_latency_ns": latency["mean_ns"],
            "latency_source": latency["source"],
            "sync_p50_latency_ns": None,
            "sync_p95_latency_ns": None,
            "sync_p99_latency_ns": None,
            "sync_p99_9_latency_ns": None,
        }
        if direction == "write":
            sync = job.get("sync")
            sync_latency = _latency(sync, completion=False)
            sync_ios = _number(sync.get("total_ios")) if isinstance(sync, dict) else None
            if sync_latency["p99_ns"] is not None and (sync_ios is None or sync_ios > 0):
                sample["sync_p50_latency_ns"] = sync_latency["p50_ns"]
                sample["sync_p95_latency_ns"] = sync_latency["p95_ns"]
                sample["sync_p99_latency_ns"] = sync_latency["p99_ns"]
                sample["sync_p99_9_latency_ns"] = sync_latency["p99_9_ns"]
        samples.append(sample)
    if not samples:
        return None

    def maximum(key):
        values = [sample[key] for sample in samples if sample.get(key) is not None]
        return max(values) if values else None

    def weighted_mean(key):
        values = [sample for sample in samples if sample.get(key) is not None]
        if not values:
            return None
        weights = [sample["total_ios"] for sample in values]
        if sum(weights) > 0:
            return sum(sample[key] * weight for sample, weight in zip(values, weights)) / sum(weights)
        return sum(sample[key] for sample in values) / len(values)

    combined_latency = {}
    for name in ("p50", "p95", "p99", "p99_9"):
        sync_value = maximum("sync_{}_latency_ns".format(name))
        completion_value = maximum("completion_{}_latency_ns".format(name))
        combined_latency[name] = sync_value if sync_value is not None else completion_value
    sources = sorted({sample["latency_source"] for sample in samples if sample["latency_source"]})
    return {
        "iops": sum(sample["iops"] for sample in samples),
        "bw_bytes_per_sec": sum(sample["bw_bytes_per_sec"] for sample in samples),
        "io_bytes": sum(sample["io_bytes"] for sample in samples),
        "total_ios": sum(sample["total_ios"] for sample in samples),
        "p50_latency_ns": combined_latency["p50"],
        "p95_latency_ns": combined_latency["p95"],
        "p99_latency_ns": combined_latency["p99"],
        "p99_9_latency_ns": combined_latency["p99_9"],
        "completion_p99_latency_ns": maximum("completion_p99_latency_ns"),
        "completion_p99_9_latency_ns": maximum("completion_p99_9_latency_ns"),
        "sync_p99_latency_ns": maximum("sync_p99_latency_ns"),
        "sync_p99_9_latency_ns": maximum("sync_p99_9_latency_ns"),
        "mean_latency_ns": weighted_mean("mean_latency_ns"),
        "latency_source": (
            "sync"
            if maximum("sync_p99_latency_ns") is not None
            else ",".join(sources) or None
        ),
    }


def parse_fio_result(path, profile=None, round_number=None):
    data = _read_json(path)
    jobs = data.get("jobs")
    if not isinstance(jobs, list) or not jobs:
        raise ValueError("fio JSON does not contain a non-empty jobs array: {}".format(path))
    result = {
        "profile": profile or Path(path).stem,
        "round": round_number,
        "directions": {},
    }
    for direction in ("read", "write"):
        metrics = _aggregate_job_direction(jobs, direction)
        if metrics is not None:
            result["directions"][direction] = metrics
    if not result["directions"]:
        raise ValueError("fio JSON contains no completed read or write I/O: {}".format(path))
    return result


def summarize_values(values):
    numbers = [_number(value) for value in values]
    numbers = [value for value in numbers if value is not None]
    if not numbers:
        return None
    mean = statistics.fmean(numbers)
    cv = 0.0 if len(numbers) < 2 or mean == 0 else statistics.pstdev(numbers) / abs(mean) * 100.0
    return {
        "median": statistics.median(numbers),
        "min": min(numbers),
        "max": max(numbers),
        "cv_percent": cv,
        "samples": len(numbers),
    }


def _parameters_from_manifest(manifest):
    parameters = manifest.get("parameters")
    return parameters if isinstance(parameters, dict) else manifest


def _summary_markdown(summary):
    manifest = summary.get("manifest", {})
    parameters = summary.get("parameters", {})
    lines = [
        "# Nebula Graph 磁盘基准测试汇总",
        "",
        "- 环境标签：{}".format(manifest.get("label", "未知")),
        "- 运行状态：{}".format(manifest.get("status", "未知")),
        "- 生成时间：{}".format(summary["generated_at"]),
        "- 测试参数：preset={}, size={}, runtime={}s, ramp={}s, cooldown={}s, repeat={}, jobs={}, iodepth={}, read_mix={}%".format(
            parameters.get("preset", "-"),
            parameters.get("size", "-"),
            parameters.get("runtime", "-"),
            parameters.get("ramp", "-"),
            parameters.get("cooldown", "-"),
            parameters.get("repeat", "-"),
            parameters.get("jobs", "-"),
            parameters.get("iodepth", "-"),
            parameters.get("read_mix", "-"),
        ),
        "",
        "## 汇总指标",
        "",
        "| Profile | 方向 | 轮次 | IOPS 中位数 (min-max, CV) | 带宽 MiB/s 中位数 (min-max, CV) | P50 延迟 us | P99 延迟 us (min-max, CV) | P99.9 延迟 us |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]

    def stat_text(stat, scale=1.0):
        if not stat:
            return "-"
        return "{:.2f} ({:.2f}-{:.2f}, {:.2f}%)".format(
            stat["median"] / scale,
            stat["min"] / scale,
            stat["max"] / scale,
            stat["cv_percent"],
        )

    for profile, directions in sorted(summary["profiles"].items()):
        for direction, result in sorted(directions.items()):
            metrics = result["metrics"]
            lines.append(
                "| {} | {} | {} | {} | {} | {} | {} | {} |".format(
                    profile,
                    "读" if direction == "read" else "写",
                    result["round_count"],
                    stat_text(metrics.get("iops")),
                    stat_text(metrics.get("bw_bytes_per_sec"), 1024.0 * 1024.0),
                    stat_text(metrics.get("p50_latency_ns"), 1000.0),
                    stat_text(metrics.get("p99_latency_ns"), 1000.0),
                    stat_text(metrics.get("p99_9_latency_ns"), 1000.0),
                )
            )
    lines.extend(
        [
            "",
            "> CV 为各轮结果的总体标准差除以均值。CV 较高时应先排查后台负载、缓存状态和设备抖动，再比较环境差异。",
        ]
    )
    if summary.get("warnings"):
        lines.extend(["", "## 警告", ""])
        lines.extend("- {}".format(warning) for warning in summary["warnings"])
    return "\n".join(lines) + "\n"


def summarize_run(run_dir):
    run_dir = Path(run_dir).expanduser().resolve()
    manifest_path = run_dir / "manifest.json"
    manifest = _read_json(manifest_path) if manifest_path.exists() else {}
    warnings = []
    environment_path = run_dir / "environment.json"
    if environment_path.exists():
        try:
            environment = _read_json(environment_path)
            processes = environment.get("nebula_storage_processes") or environment.get(
                "nebula_storaged"
            )
            if isinstance(processes, dict) and processes.get("active"):
                warnings.append("环境采集时检测到活动的 Nebula 存储进程，结果包含在线 I/O 干扰")
        except (OSError, json.JSONDecodeError) as error:
            warnings.append("无法读取 environment.json: {}".format(error))
    rounds = {}
    raw_dir = run_dir / "raw"
    for path in sorted(raw_dir.glob("round-*/*.json")):
        match = re.fullmatch(r"round-(\d+)", path.parent.name)
        if not match:
            continue
        round_number = int(match.group(1))
        profile = path.stem
        try:
            parsed = parse_fio_result(path, profile, round_number)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            warnings.append(str(error))
            continue
        for direction, metrics in parsed["directions"].items():
            rounds.setdefault(profile, {}).setdefault(direction, []).append(
                dict({"round": round_number}, **metrics)
            )
    if not rounds:
        raise ValueError("no valid fio result files found below {}".format(raw_dir))

    parameters = _parameters_from_manifest(manifest)
    expected_profiles = _normalize_profiles(parameters.get("profiles", []))
    expected_repeat = _number(parameters.get("repeat"))
    for profile in expected_profiles:
        if profile not in rounds:
            warnings.append("缺少 profile {} 的有效结果".format(profile))
            continue
        for direction in EXPECTED_DIRECTIONS.get(profile, ()):
            if direction not in rounds[profile]:
                warnings.append(
                    "profile {} 缺少 {} 方向的有效结果".format(profile, direction)
                )

    profiles = {}
    for profile, directions in sorted(rounds.items()):
        profiles[profile] = {}
        for direction, samples in sorted(directions.items()):
            samples.sort(key=lambda item: item["round"])
            if expected_repeat is not None and len(samples) != int(expected_repeat):
                warnings.append(
                    "profile {} {} 方向期望 {} 轮，实际 {} 轮".format(
                        profile, direction, int(expected_repeat), len(samples)
                    )
                )
            metrics = {}
            for metric in NUMERIC_METRICS:
                stat = summarize_values([sample.get(metric) for sample in samples])
                if stat is not None:
                    metrics[metric] = stat
            for metric in ("iops", "bw_bytes_per_sec", "p99_latency_ns", "p99_9_latency_ns"):
                stat = metrics.get(metric)
                if stat and stat["samples"] >= 3 and stat["cv_percent"] > 10.0:
                    warnings.append(
                        "profile {} {} 指标 {} 的 CV {:.2f}% 超过 10%".format(
                            profile, direction, metric, stat["cv_percent"]
                        )
                    )
            profiles[profile][direction] = {
                "round_count": len(samples),
                "rounds": samples,
                "metrics": metrics,
            }

    summary = {
        "schema_version": SCHEMA_VERSION,
        "generated_at": _utc_now(),
        "run_dir": str(run_dir),
        "manifest": manifest,
        "parameters": parameters,
        "profiles": profiles,
        "warnings": warnings,
    }
    _write_json(run_dir / "summary.json", summary)
    _atomic_write(run_dir / "summary.md", _summary_markdown(summary))
    return summary


def _load_summary(path):
    path = Path(path).expanduser().resolve()
    if path.is_file():
        summary = _read_json(path)
        run_dir = path.parent
    else:
        run_dir = path
        summary_path = run_dir / "summary.json"
        summary = _read_json(summary_path) if summary_path.exists() else summarize_run(run_dir)

    manifest_path = run_dir / "manifest.json"
    if manifest_path.exists():
        current_manifest = _read_json(manifest_path)
        summary = dict(summary)
        summary["manifest"] = current_manifest
        summary["parameters"] = _parameters_from_manifest(current_manifest)
    return summary


def _canonical_parameter(value, key):
    if key == "profiles":
        return _normalize_profiles(value if isinstance(value, list) else [str(value or "")])
    if key in (
        "runtime",
        "ramp",
        "cooldown",
        "repeat",
        "jobs",
        "iodepth",
        "read_mix",
        "randseed",
    ):
        number = _number(value)
        return number if number is not None else value
    return value


def _parameter_mismatches(baseline, candidate):
    baseline_parameters = baseline.get("parameters") or _parameters_from_manifest(
        baseline.get("manifest", {})
    )
    candidate_parameters = candidate.get("parameters") or _parameters_from_manifest(
        candidate.get("manifest", {})
    )
    mismatches = []
    for key in CORE_PARAMETERS:
        baseline_value = _canonical_parameter(baseline_parameters.get(key), key)
        candidate_value = _canonical_parameter(candidate_parameters.get(key), key)
        if baseline_value != candidate_value:
            mismatches.append(
                {"parameter": key, "baseline": baseline_value, "candidate": candidate_value}
            )
    return mismatches


def _validity_issues(summary, role):
    issues = []
    manifest = summary.get("manifest") or {}
    status = manifest.get("status")
    if status != "complete":
        issues.append(
            {
                "role": role,
                "type": "run_status",
                "detail": "运行状态为 {}，期望 complete".format(status or "missing"),
            }
        )
    for warning in summary.get("warnings") or []:
        issues.append({"role": role, "type": "summary_warning", "detail": str(warning)})
    return issues


def _compare_metric(baseline, candidate, higher_is_better, threshold):
    baseline_value = _number(baseline)
    candidate_value = _number(candidate)
    result = {
        "baseline": baseline_value,
        "candidate": candidate_value,
        "higher_is_better": higher_is_better,
        "threshold_percent": threshold,
    }
    if baseline_value is None or candidate_value is None:
        result.update({"change_percent": None, "performance_change_percent": None, "status": "missing_data"})
        return result
    if baseline_value == 0:
        if candidate_value == 0:
            result.update({"change_percent": 0.0, "performance_change_percent": 0.0, "status": "stable"})
        else:
            result.update({"change_percent": None, "performance_change_percent": None, "status": "not_comparable"})
        return result
    change = (candidate_value - baseline_value) / abs(baseline_value) * 100.0
    performance_change = change if higher_is_better else -change
    if performance_change >= threshold:
        status = "improved"
    elif performance_change <= -threshold:
        status = "regressed"
    else:
        status = "stable"
    result.update(
        {
            "change_percent": change,
            "performance_change_percent": performance_change,
            "status": status,
        }
    )
    return result


def _comparison_markdown(comparison):
    lines = [
        "# Nebula Graph 磁盘基准测试对比",
        "",
        "- 基准环境：{}".format(comparison["baseline_label"]),
        "- 候选环境：{}".format(comparison["candidate_label"]),
        "- 变化判定阈值：{:.2f}%".format(comparison["threshold_percent"]),
        "- 参数可比：{}".format("是" if comparison["comparable"] else "否"),
        "",
    ]
    if comparison["parameter_mismatches"]:
        lines.extend(
            [
                "## 参数不一致",
                "",
                "| 参数 | 基准环境 | 候选环境 |",
                "|---|---|---|",
            ]
        )
        for mismatch in comparison["parameter_mismatches"]:
            lines.append(
                "| {} | `{}` | `{}` |".format(
                    mismatch["parameter"], mismatch["baseline"], mismatch["candidate"]
                )
            )
        lines.extend(
            [
                "",
                "> 核心参数不一致，结果不能直接用于环境性能结论。请使用相同参数重新测试。",
                "",
            ]
        )
    if comparison["validity_issues"]:
        lines.extend(
            [
                "## 结果完整性问题",
                "",
                "| 环境 | 类型 | 说明 |",
                "|---|---|---|",
            ]
        )
        role_names = {"baseline": "基准", "candidate": "候选"}
        for issue in comparison["validity_issues"]:
            lines.append(
                "| {} | {} | {} |".format(
                    role_names.get(issue["role"], issue["role"]),
                    issue["type"],
                    issue["detail"].replace("|", "\\|"),
                )
            )
        lines.extend(
            [
                "",
                "> 运行未完整结束或汇总存在缺失数据，当前结果不能用于性能结论。",
                "",
            ]
        )
    lines.extend(
        [
            "## 分项对比",
            "",
            "| Profile | 方向 | 指标 | 基准中位数 | 候选中位数 | 性能变化 | 判定 |",
            "|---|---:|---|---:|---:|---:|---|",
        ]
    )
    status_names = {
        "improved": "提升",
        "regressed": "回退",
        "stable": "持平",
        "missing_data": "数据缺失",
        "not_comparable": "无法计算",
    }
    metric_names = {key: name for key, _, name in COMPARISON_METRICS}
    for profile, directions in sorted(comparison["profiles"].items()):
        for direction, metrics in sorted(directions.items()):
            for metric, result in metrics.items():
                performance = result.get("performance_change_percent")
                performance_text = "-" if performance is None else "{:+.2f}%".format(performance)
                lines.append(
                    "| {} | {} | {} | {} | {} | {} | {} |".format(
                        profile,
                        "读" if direction == "read" else "写",
                        metric_names.get(metric, metric),
                        _format_comparison_value(metric, result.get("baseline")),
                        _format_comparison_value(metric, result.get("candidate")),
                        performance_text,
                        status_names.get(result["status"], result["status"]),
                    )
                )
    lines.extend(
        [
            "",
            "> 性能变化已按指标方向归一：IOPS/带宽越高越好，延迟越低越好。本报告不生成跨 profile 总分；不同负载应分别判断。",
        ]
    )
    return "\n".join(lines) + "\n"


def _format_comparison_value(metric, value):
    if value is None:
        return "-"
    if metric == "bw_bytes_per_sec":
        return "{:.2f} MiB/s".format(value / (1024.0 * 1024.0))
    if metric in ("p99_latency_ns", "p99_9_latency_ns"):
        return "{:.2f} us".format(value / 1000.0)
    return "{:.2f}".format(value)


def compare_runs(baseline_path, candidate_path, output_dir, threshold_percent=10.0):
    if threshold_percent < 0:
        raise ValueError("threshold-percent must be non-negative")
    baseline = _load_summary(baseline_path)
    candidate = _load_summary(candidate_path)
    mismatches = _parameter_mismatches(baseline, candidate)
    validity_issues = _validity_issues(baseline, "baseline") + _validity_issues(
        candidate, "candidate"
    )
    profiles = {}
    all_profiles = sorted(set(baseline.get("profiles", {})) | set(candidate.get("profiles", {})))
    for profile in all_profiles:
        profiles[profile] = {}
        baseline_directions = baseline.get("profiles", {}).get(profile, {})
        candidate_directions = candidate.get("profiles", {}).get(profile, {})
        directions = sorted(set(baseline_directions) | set(candidate_directions))
        for direction in directions:
            profiles[profile][direction] = {}
            baseline_metrics = baseline_directions.get(direction, {}).get("metrics", {})
            candidate_metrics = candidate_directions.get(direction, {}).get("metrics", {})
            for metric, higher_is_better, _ in COMPARISON_METRICS:
                baseline_stat = baseline_metrics.get(metric) or {}
                candidate_stat = candidate_metrics.get(metric) or {}
                profiles[profile][direction][metric] = _compare_metric(
                    baseline_stat.get("median"),
                    candidate_stat.get("median"),
                    higher_is_better,
                    float(threshold_percent),
                )
    comparison = {
        "schema_version": SCHEMA_VERSION,
        "generated_at": _utc_now(),
        "baseline_label": baseline.get("manifest", {}).get(
            "label", baseline.get("label", "baseline")
        ),
        "candidate_label": candidate.get("manifest", {}).get(
            "label", candidate.get("label", "candidate")
        ),
        "threshold_percent": float(threshold_percent),
        "comparable": not mismatches and not validity_issues,
        "parameter_mismatches": mismatches,
        "validity_issues": validity_issues,
        "profiles": profiles,
        "note": "No aggregate score is calculated; interpret every profile independently.",
    }
    output_dir = Path(output_dir).expanduser().resolve()
    _write_json(output_dir / "comparison.json", comparison)
    _atomic_write(output_dir / "comparison.md", _comparison_markdown(comparison))
    return comparison


def _parser():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    collect = subparsers.add_parser("collect-env", help="collect host and storage environment")
    collect.add_argument("--output", required=True)
    collect.add_argument("--label", required=True)
    collect.add_argument("--target-dir", required=True)
    collect.add_argument("--wal-target-dir", required=True)
    collect.add_argument("--fio-bin", required=True)
    collect.add_argument("--nebula-config")
    collect.add_argument("--nebula-home")

    initialize = subparsers.add_parser("init-run", help="create a run manifest")
    initialize.add_argument("--output", required=True)
    initialize.add_argument("--label", required=True)
    initialize.add_argument("--preset", required=True)
    initialize.add_argument("--size", required=True)
    initialize.add_argument("--runtime", required=True, type=int)
    initialize.add_argument("--ramp", required=True, type=int)
    initialize.add_argument("--cooldown", required=True, type=int)
    initialize.add_argument("--repeat", required=True, type=int)
    initialize.add_argument("--jobs", required=True, type=int)
    initialize.add_argument("--iodepth", required=True, type=int)
    initialize.add_argument("--read-mix", type=int, default=70)
    initialize.add_argument("--profiles", required=True, nargs="+")
    initialize.add_argument("--target", "--target-dir", dest="target", required=True)
    initialize.add_argument("--wal", "--wal-target-dir", dest="wal", required=True)
    initialize.add_argument("--tool-version", required=True)
    initialize.add_argument("--fio-version")
    initialize.add_argument("--randseed", type=int, default=1)
    initialize.add_argument("--profile-hash", required=True)

    finish = subparsers.add_parser("finish-run", help="finish a run manifest")
    finish.add_argument("--manifest", required=True)
    finish.add_argument("--status", choices=("complete", "failed"), required=True)

    summarize = subparsers.add_parser("summarize", help="summarize fio JSON files")
    summarize.add_argument("--run-dir", required=True)

    compare = subparsers.add_parser("compare", help="compare two benchmark summaries")
    compare.add_argument("--baseline", required=True)
    compare.add_argument("--candidate", required=True)
    compare.add_argument("--output-dir", required=True)
    compare.add_argument("--threshold-percent", type=float, default=10.0)
    return parser


def main(argv=None):
    args = _parser().parse_args(argv)
    try:
        if args.command == "collect-env":
            collect_environment(
                args.output,
                args.label,
                args.target_dir,
                args.wal_target_dir,
                args.fio_bin,
                args.nebula_config,
                args.nebula_home,
            )
        elif args.command == "init-run":
            init_run(
                args.output,
                args.label,
                args.preset,
                args.size,
                args.runtime,
                args.ramp,
                args.repeat,
                args.jobs,
                args.iodepth,
                args.profiles,
                args.target,
                args.wal,
                args.tool_version,
                args.read_mix,
                args.fio_version,
                args.randseed,
                args.cooldown,
                args.profile_hash,
            )
        elif args.command == "finish-run":
            finish_run(args.manifest, args.status)
        elif args.command == "summarize":
            summarize_run(args.run_dir)
        elif args.command == "compare":
            compare_runs(
                args.baseline, args.candidate, args.output_dir, args.threshold_percent
            )
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print("report.py: {}".format(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
