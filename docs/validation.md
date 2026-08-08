# Validation gates

AffinityGraph must progress through modes by evidence, even though they use the
same runtime and dynamic solver:

1. **Observe:** audit embedded and Prism metric semantics before comparing
   values. Embedded cross-TID futex/VFS handoffs are not equivalent to Prism
   total wake/request counters, so the current comparison is warning-only.
   AffinityGraph's 30-second BPF loss must remain below 1%; throughput overhead
   must be at most 2%, mean runtime CPU below one core, and RSS below 256 MiB.
2. **Plan:** replay Doris and ClickHouse windows, verify family selection,
   domain merging, full node masks, atomic 1024-thread bounds, capacity, and
   deterministic solver P95 below one second. No affinity syscall may be
   issued. A missing or unhealthy embedded BPF collector invalidates plan.
3. **Active:** require at least 48-60 seconds of window confidence, a healthy
   complete 30-second BPF window, and the same initial domain plan for three
   consecutive solve windows. Execute complete domain mask updates, verify all
   planned masks before measurement, and restore 100%. Smoke
   action success must be at least 80%; formal action success must be at least
   95%, with restoration at 100%.

Every treatment starts a fresh ClickHouse and runs a 30-second warmup followed
by a 300-second measurement phase. Measurement must wait until every eligible
TID in each managed domain has its verified normalized node mask and runtime reports
`active_effective=true`; active must then remain effective for at least 270
seconds. The formal
baseline is Linux default scheduling over four nodes and CPUs
`0-127`, with default memory policy. Baseline and active use `max_threads=32`,
global slots 128, identical data, YBA parameters, request ordering, and server
environment. C2T2, C4T6, and C5T16 each run five randomized pairs.

Acceptance requires every active workload at least 95% of baseline throughput,
positive gain in at least two workloads, and at least 3% weighted mean gain.
YBA throughput/P99 is used only for this offline acceptance, never as an online
feature. Three randomized baseline/active pairs per database are the minimum
performance conclusion; historical manual placement is mechanism evidence only.

## 2026-08-08 short positive-control smoke

The 183 host smoke used embedded BPF in every profile, two randomized rounds,
a 30-second warmup, and a 120-second measurement. It is selector evidence, not
a formal performance conclusion.

- Doris `brpc_light + Pipe_normal` over the unrestricted envelope improved
  throughput by 33.21% and 33.65% (mean 33.43%). Further partitioning against
  an all-thread one-node control was inconsistent (mean 1.43%, one of two
  wins).
- ClickHouse `ThreadPool:0-31` with other threads on `0-63` lost 4.27% and
  5.61% against the `0-63` two-node control (mean -4.94%). It is therefore a
  negative control; the selector must not force ThreadPool to satisfy an old
  oracle.
- Every run had zero BPF loss, zero AffinityGraph actions, valid mask checks,
  100% restore, and zero workload errors/timeouts. Maximum observed Supervisor
  average CPU was 0.745 core for Doris and 0.141 core for ClickHouse.

Runtime-log replay with the revised selector forms a single-node
`Pipe_normal + brpc_light` Doris domain without selecting `brpc_heavy` (640
threads, no truncation), while the ClickHouse replay forms no domain. Both
replays are deterministic; solve P95 is 10.1 ms for Doris and 0.4 ms for
ClickHouse on the replay host.
