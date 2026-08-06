# Validation gates

AffinityGraph must progress through modes by evidence, even though they use the
same runtime and dynamic solver:

1. **Observe:** audit embedded and Prism metric semantics before comparing
   values. Embedded cross-TID futex/VFS handoffs are not equivalent to Prism
   total wake/request counters, so the current comparison is warning-only.
   AffinityGraph's 30-second BPF loss must remain below 1%; throughput overhead
   must be at most 2%, mean runtime CPU below one core, and RSS below 256 MiB.
2. **Plan:** replay historical ClickHouse windows, verify relationship ranking,
   deterministic capacity constraints, migration budget, and a 128-TID solver
   P95 below one second. No affinity syscall may be issued.
3. **Active:** require at least 48-60 seconds of window confidence and the same
   materially active thread cohort for three consecutive solve windows before
   applying the latest dynamic placement. Small one-way startup growth is
   allowed, while removal or identity replacement resets confirmation. This is
   followed by 60 seconds dwell. Smoke
   action success must be at least 80%; formal action success must be at least
   95%, with restoration at 100%.

Every treatment starts a fresh ClickHouse and runs a 30-second warmup followed
by a 300-second measurement phase. Active must already have a committed plan at
measurement start and remain effective for at least 270 seconds. The formal
baseline is Linux default scheduling over four nodes and CPUs
`0-127`, with default memory policy. Baseline and active use `max_threads=32`,
global slots 128, identical data, YBA parameters, request ordering, and server
environment. C2T2, C4T6, and C5T16 each run five randomized pairs.

Acceptance requires every active workload at least 95% of baseline throughput,
positive gain in at least two workloads, and at least 3% weighted mean gain.
YBA throughput/P99 is used only for this offline acceptance, never as an online
feature. Strict singleton pinning has no established causal support yet and is
the primary experiment risk.
