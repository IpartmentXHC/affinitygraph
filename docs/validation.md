# Validation gates

AffinityGraph must progress through modes by evidence, even though they use the
same runtime and dynamic solver:

1. **Observe:** compare embedded task/futex/VFS counters with the pinned Prism
   collector. Key counter error and BPF loss must each be below 1%; throughput
   overhead must be at most 2%, mean runtime CPU below one core, and RSS below
   256 MiB.
2. **Plan:** replay historical ClickHouse windows, verify relationship ranking,
   deterministic capacity constraints, migration budget, and a 128-TID solver
   P95 below one second. No affinity syscall may be issued.
3. **Active:** require at least 48-60 seconds of window confidence, three equal
   proposals, and 60 seconds dwell. Affinity action success must be at least
   99.9% and restoration 100% before performance acceptance.

The formal baseline is Linux default scheduling over four nodes and CPUs
`0-127`, with default memory policy. Baseline and active use `max_threads=32`,
global slots 128, identical data, YBA parameters, request ordering, and server
environment. C2T2, C4T6, and C5T16 each run five randomized pairs.

Acceptance requires every active workload at least 95% of baseline throughput,
positive gain in at least two workloads, and at least 3% weighted mean gain.
YBA throughput/P99 is used only for this offline acceptance, never as an online
feature. Strict singleton pinning has no established causal support yet and is
the primary experiment risk.

