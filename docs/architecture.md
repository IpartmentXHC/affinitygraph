# Architecture

AffinityGraph is an in-process Linux thread-placement runtime. `affinity-run`
performs the privileged CO-RE load, inserts its own PID into the BPF filter,
keeps the BPF object descriptors across `exec`, drops to the database account,
and preloads `libaffinitygraph.so`. There is no collector or policy daemon.

The shared library has four paths:

1. pthread interposition records parent/child TIDs, start routine addresses,
   creation time, names, inherited masks, application affinity declarations,
   and all cleanup exits. `/proc/self/task` reconciliation covers preloaded
   threads, raw clone/clone3, missed events, and TID reuse.
2. `/proc` provides per-TID schedstat, state, recent CPU, context switches, and
   allowed CPUs. The reduced CO-RE program reports fork/exit, futex handoffs,
   and selective shared inode access into a one MiB ring buffer. Process
   `numa_maps` is intentionally not attributed to individual threads.
3. A 60-second window computes `d_i = clamp(EWMA_30s((run+rq)/dt),0,1)` and
   retains the activity, synchronization, sharing, stability, and final score
   dimensions separately. The example freezes ClickHouse gate2 fixed-v2 P95
   scales (`2.4138804290562152`, `2.5591179487485345`, and
   `0.00894730347830295`); another database must supply a reviewed calibration
   ID and all three values. Names are normalized generically; start routine and
   lineage disambiguate same-name pools. Neither names nor VFS sharing are
   interpreted as hardware cache sharing.
4. A deterministic capacity-first partition assigns TIDs to NUMA nodes, then
   LPT with relationship-latency cost assigns singleton CPUs. Confirmation,
   confidence, dwell, and active-thread migration limits guard execution.
   Every batch verifies each syscall and rolls back earlier actions on a
   non-ESRCH failure.

The solver objective is lexicographic: CPU/node overload, relationship-weighted
hardware latency, then active migration cost. This preserves all useful graph
edges; no minimum spanning tree is used. PMU, uncore, SPE, application
throughput, latency, and YBA profiles are excluded from online decisions.

## Current boundary

The reduced BPF program represents a futex handoff with the most recently
observed waiter for an address and VFS sharing with consecutive accesses to the
same inode. It is deliberately smaller than Prism's full sampled maps. A target
host comparison against the pinned Prism collector is an activation gate; the
runtime must remain in observe mode until the counter delta is below one
percent and ring loss is below one percent.
