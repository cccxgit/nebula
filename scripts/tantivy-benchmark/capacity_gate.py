#!/usr/bin/env python3
"""Conservative go/no-go assessment from a measured 1m-or-larger pilot.

This only reads measurements and /proc; it never starts services or imports data.
Passing permits the next bounded stage, not an unattended jump to 100m records.
"""
import argparse
import json
from pathlib import Path
import shutil
import time

GIB = 1024 ** 3


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--pilot", required=True)
    p.add_argument("--path", default="/home/sch/nebula-data/tantivy")
    p.add_argument("--download-metadata", default="/home/sch/nebula-data/tantivy/msmarco_v2_passage.tar.download.json")
    p.add_argument("--target-rows", type=int, default=100000000)
    p.add_argument("--time-budget-hours", type=float, default=0)
    p.add_argument("--output", required=True)
    args = p.parse_args()
    pilot = json.loads(Path(args.pilot).read_text())
    required = ["measured_rows", "graph_delta_bytes", "index_delta_bytes", "normalized_file_bytes",
                "index_visible_seconds", "peak_service_pss_bytes", "max_verified_query_hits",
                "all_query_hit_counts_verified", "error_free", "no_oom_or_swap_storm"]
    missing = [key for key in required if pilot.get(key) is None]
    if missing:
        raise ValueError("Missing measured inputs; do not substitute estimates: " + ", ".join(missing))
    n = pilot["measured_rows"]
    if n < 1000000 or args.target_rows < n:
        raise ValueError("Pilot must contain at least 1m measured rows, target must not be smaller")
    for key in required[:7]:
        if not isinstance(pilot[key], (int, float)) or pilot[key] < 0:
            raise ValueError("Invalid measured value: " + key)
    memory = {}
    for line in Path('/proc/meminfo').read_text().splitlines():
        key, value = line.split(':', 1)
        memory[key] = int(value.split()[0]) * 1024
    download = json.loads(Path(args.download_metadata).read_text())
    archive_remaining = max(0, int(download['remote']['Content-Length']) - download.get('downloaded_bytes', 0))
    ratio = args.target_rows / n
    graph_final = pilot['graph_delta_bytes'] * ratio
    index_final = pilot['index_delta_bytes'] * ratio
    # The measured graph/index deltas already include all THREE storaged replicas.
    # Do not multiply them by three again.
    additional_data = (graph_final + index_final) * (1 - 1 / ratio) if ratio > 1 else 0
    normalized_remaining = pilot['normalized_file_bytes'] * (ratio - 1)
    uncertainty = additional_data * .50
    merge_headroom = index_final * .50
    fixed_reserve = 40 * GIB
    required_free = additional_data + uncertainty + merge_headroom + normalized_remaining + archive_remaining + fixed_reserve
    free = shutil.disk_usage(args.path).free
    projected_hours = pilot['index_visible_seconds'] * ratio * 1.5 / 3600
    projected_hits = pilot['max_verified_query_hits'] * ratio
    checks = {
        'pilot_error_free': pilot['error_free'] is True,
        'pilot_no_oom_or_swap_storm': pilot['no_oom_or_swap_storm'] is True,
        'current_available_memory_at_least_8_GiB': memory['MemAvailable'] >= 8 * GIB,
        'pilot_peak_service_pss_at_most_half_RAM': pilot['peak_service_pss_bytes'] <= memory['MemTotal'] * .5,
        'disk_has_50pct_growth_margin_merge_headroom_and_40GiB_reserve': free >= required_free,
        'complete_query_hit_counts_verified': pilot['all_query_hit_counts_verified'] is True,
        'projected_query_hits_at_most_100k': projected_hits <= 100000,
    }
    if args.time_budget_hours:
        checks['projected_elapsed_fits_declared_time_budget'] = projected_hours <= args.time_budget_hours
    next_stage = min(args.target_rows, max(n + 1, min(n * 5, n + 10000000)))
    report = {
        'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        'kind': 'conservative_estimate_from_measured_pilot_not_large_scale_result',
        'pilot': args.pilot, 'pilot_rows': n, 'target_rows': args.target_rows,
        'checks': checks, 'passes_all_gates': all(checks.values()),
        'permitted_next_stage_rows_if_passed': next_stage,
        'measured_current_free_bytes': free, 'estimated_required_free_bytes': required_free,
        'estimated_final_graph_bytes': graph_final, 'estimated_final_index_bytes': index_final,
        'reserved_growth_uncertainty_bytes': uncertainty, 'reserved_merge_bytes': merge_headroom,
        'remaining_archive_download_bytes': archive_remaining,
        'estimated_remaining_normalized_file_bytes': normalized_remaining,
        'estimated_index_visible_hours_with_50pct_margin': projected_hours,
        'estimated_max_query_hits': projected_hits,
        'notes': [
            '1m-to-100m extrapolation is not evidence of fit; repeat measurement at 5m and 10m, then increment by at most 10m.',
            'Stop scheduling new writes if disk free <40GiB or MemAvailable <8GiB; do not kill unrelated desktop processes.',
            'Current prototype can collect all search hits before LIMIT; high-hit queries must not run at 100m without bounded collection.',
            'A positive outcome does not establish crash recovery, production readiness, or a 100m benchmark result.'
        ]}
    target = Path(args.output)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if not report['passes_all_gates']:
        raise SystemExit(2)


if __name__ == '__main__':
    main()
