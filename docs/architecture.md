# Architecture

AffinityGraph is a per-target Linux thread-placement supervisor. `affinity-run`
loads and attaches the CO-RE object, forks the database behind a startup
barrier, registers its TGID, drops both processes to the database account, and
keeps collection and policy code outside the target address space. It uses no
`LD_PRELOAD`, pthread interposition, or system-wide policy daemon.

In active mode only, the dropped supervisor retains `CAP_SYS_NICE` as its sole
permitted and effective capability. This is required to change affinity when a
database executable has file capabilities and is therefore a privileged-exec
target. The database child does not inherit `CAP_SYS_NICE`; observe and plan
supervisors retain no capability.

The runtime has four paths:

1. `sched_process_fork`, `sched_process_exec`, and `sched_process_exit` are the
   authoritative lifecycle source for pthread, raw clone, clone3, descendants,
   and re-exec. `task_rename` plus `/proc/<tgid>/task` reconciliation supplies
   names and missed state. Identity is `(tgid, tid, generation)`; mutable comm
   values are metadata only.
2. An optional entry/return uprobe on the mapped `pthread_create` symbol reads
   the ARM64 x2 start-routine address and correlates it with the next thread
   fork from that parent TID. The supervisor resolves the user address through
   `/proc/<pid>/maps`; raw clone and missed probes legitimately have no start
   routine. Uprobe failure never changes lifecycle correctness.
3. `/proc` provides per-TID schedstat, state, recent CPU, context switches, and
   allowed CPUs. The reduced CO-RE program reports futex handoffs, selective
   shared inode access, and successful application `sched_setaffinity` calls.
   Process `numa_maps` remains process-scoped and is never presented as
   per-thread locality.
4. A 60-second graph computes
   `d_i = clamp(EWMA_30s((run+rq)/dt),0,1)`, then performs deterministic
   capacity-first NUMA partitioning and LPT singleton placement. Confidence,
   confirmation, dwell, and migration limits guard action. Confirmation means
   that the materially active `(tgid, tid, generation)` cohort remains eligible
   for three solve windows; a cohort of at least 20 threads may grow by at most
   five percent while all existing identities remain present. Removal, identity
   replacement, or larger growth resets confirmation. The latest dynamic
   placement is then applied. CPU assignments are intentionally excluded because an unpinned
   thread's sampled current CPU changes under the default scheduler. A
   non-ESRCH batch failure rolls back earlier calls.

The supervisor's own affinity syscalls originate outside tracked TGIDs, so the
BPF filter naturally distinguishes them from application declarations. A new
thread restores to its parent's saved application mask when its observed mask
was inherited from an AffinityGraph pin.

The solver objective remains lexicographic: CPU/node overload,
relationship-weighted hardware latency, then active migration cost. PMU,
uncore, SPE, application throughput, latency, and YBA profiles are excluded
from online decisions.

## Current boundary

The BPF program represents a futex handoff with the most recently observed
waiter and VFS sharing with consecutive accesses to the same selected inode.
Tetragon and Tracee inform process-generation and descendant tracking semantics;
they are not runtime dependencies. A target-host Prism run documents metric
compatibility before activation. Current Prism totals and embedded attributed
handoffs have different semantics, so their delta is warning-only.
AffinityGraph's own 30-second ring-loss window must remain below one percent.
