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
   `d_i = clamp(EWMA_30s((run+rq)/dt),0,1)`. After confidence and BPF-health
   gates pass, `numa-domain-v1` aggregates placement families by normalized
   name and resolved start symbol. Demand, internal relationship weight,
   self-containment, and relative internal strength must remain above their
   thresholds for three solve windows before a family becomes a cohesive
   singleton anchor. A second path permits two families with sufficient demand
   and absolute internal evidence to seed a domain when their aggregated
   cross-family ratio is stable for three windows. This path does not lower the
   self-containment or relative-internal thresholds, and external-only handlers
   cannot seed a domain.
5. TID edges are aggregated by family before deterministic per-family
   heavy-hitter pruning. Stable cross-family seeds merge controlled families
   into domains. Each complete
   domain, up to 1024 threads, receives the smallest deterministic NUMA-node
   mask whose online CPUs provide 80% demand headroom. The mask is intersected
   with the resource and application envelope. Linux schedules within the
   mask; there are no steady-state CPU moves or swaps. Expansion and shrink use
   asymmetric utilization confirmation and a 300-second domain dwell.
6. Plan mode advances isolated shadow state. Active changes a complete mask
   batch only after whole-plan confirmation and actuator verification. A
   non-ESRCH failure restores every completed TID to its pre-batch mask. A new
   member already carrying its domain mask is recorded as inherited without a
   redundant syscall. The legacy singleton solver remains replayable.

The supervisor's own affinity syscalls originate outside tracked TGIDs, so the
BPF filter naturally distinguishes them from application declarations. A new
thread restores to its parent's saved application mask when its observed mask
was inherited from an AffinityGraph pin.

Domain node selection is lexicographic: sufficient capacity with the fewest
nodes, relationship latency, unmanaged sampled demand, initial migration count,
then node ID. PMU,
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
