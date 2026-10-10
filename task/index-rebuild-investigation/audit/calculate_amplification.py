#!/usr/bin/env python3
"""Exact first-snapshot arithmetic; extrapolation, not a multi-day benchmark."""
from pathlib import Path
import csv
import json

OUT = Path(__file__).resolve().parent

def metrics(n, key_bytes, batch, burst):
    # Modify op, empty TTL value: put index key + remove (16-byte op header + key).
    d = 2 * key_bytes + 16
    q = batch // d + 1
    first = max(q, burst // d + 1)
    if n >= q:
        calls = n - q + 2  # Every prefix q..N, then final duplicate full batch.
        row_copies = (n * (n + 1) - (q - 1) * q) // 2 + n
    else:
        calls, row_copies = 1, n
    forced_sleeps = max(0, n - first + 1) + int(n * d > burst)
    return dict(n=n, key_bytes=key_bytes, operation_payload_bytes=d,
                batch_bytes=batch, burst_bytes=burst, first_flush_record=q,
                first_over_burst_record=first,
                first_snapshot_append_calls=calls,
                repeated_logical_row_copies=row_copies,
                first_snapshot_lowlevel_ops=2*row_copies,
                first_snapshot_payload_bytes=d*row_copies,
                first_snapshot_encoded_bytes=(d+18)*row_copies+13*calls,
                forced_sleep_seconds_min=forced_sleeps,
                forced_sleep_days_min=round(forced_sleeps/86400,6),
                repetition_factor=round(row_copies/n,6))

cases = []
for batch, burst, label in [(131072,524288,"flags_default"),
                             (1048576,4194304,"config_template")]:
    for key_bytes, kind in [(24,"tag_int_vid8"),(40,"edge_int_vid8"),(1040,"tag_4xstring256_vid8")]:
        for n in [2200,8250,200000,250000]:
            cases.append(dict(profile=label, kind=kind, **metrics(n,key_bytes,batch,burst)))
with (OUT/"amplification.csv").open("w") as out:
    writer=csv.DictWriter(out,fieldnames=cases[0].keys());writer.writeheader();writer.writerows(cases)

# Cross-check sums with recorded native harness cases, excluding its two final empty passes.
assert metrics(2200,24,131072,524288)["first_snapshot_encoded_bytes"] + 26 == 26662183
assert metrics(8250,24,131072,524288)["first_snapshot_encoded_bytes"] + 26 == 2619607483
assert metrics(8250,24,131072,524288)["forced_sleep_seconds_min"] == 59

selected=[x for x in cases if (x['n'] in (200000,250000))]
(OUT/"amplification.log").write_text("ANALYTIC EXTRAPOLATION ONLY; actual encoded totals cross-checked with native source harness.\n" + json.dumps(selected,indent=2)+"\n")
print((OUT/"amplification.log").read_text())
