# Final presentation handoff

Final deliverable: `docs/tantivy/nebula-tantivy-competition-final-20261105-v2.pptx`.
Private previews and finalization receipt: `task-logs/tantivy-presentation/final-20261105-v2/`.

Eight slides preserve the established design. Native editable tables are on slides 2, 6 and 7; slide 3 has the editable architecture diagram and explicitly labels Tantivy as an in-process static library. Slide 2 records the million-record native query and whole-service absolute observations, with the 54/77/2 semantic differences and different fulltext replication conditions. Slide 7 reports the 112.883x same-MATCH C1 on/off result and five-million absolute measurements, with unequal 800/80 samples and 3/1 warmups visibly disclosed. No ES speedup, memory saving, machine downgrade, 1m-to-5m scaling ratio or 100m execution is claimed.

The five prototype suites total 93 distinct checks. Current main and native grammar evidence is `tantivy-demo/verified-final/report.json` and `tantivy-benchmark/regression-tantivy-final/regression.json` under `task-logs/`. Lifecycle reruns are counted once. `tantivy_demo_v3`, `ft_regression_final` and `ft_bench_5m` remain available, and guide queries were independently smoke-tested. Restarting storage triggers full backfill; process liveness alone is not readiness.

The measured slide source is `performance-slide.measured.json`. Do not deliver the old prototype, v1 or private source files as the final deck. The artifact operation marker was already recorded once for this authoring operation. No services or compiler were started by presentation work. Final package and visual QA records remain private.

Use this exact directive once in the final user response:

:codex-file-citation{path="/home/sch/nebula/nebula-3.6-tantivy/docs/tantivy/nebula-tantivy-competition-final-20261105-v2.pptx" purpose="output"}
