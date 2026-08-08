# AffinityGraph: eBPF Supervisor Technical Architecture

Create a publication-quality, vector-like academic systems architecture diagram in a clean 16:9 landscape composition. Use a white background, dark charcoal text, thin precise arrows, and a restrained palette: teal for observation, blue for collection, amber for planning, green for safe execution, red only for failures. Typography must be crisp and readable. Avoid decorative illustrations, gradients, 3D effects, and dense prose.

The title is: **AffinityGraph eBPF Supervisor: Four Technical Modules**

Immediately below the title, show a narrow architecture banner:

**Long-running affinity-run Supervisor | libbpf + CO-RE eBPF | Linux 6.12 ARM64**

At the right edge of this banner, show a clear crossed-out label: **NO LD_PRELOAD / NO pthread C shim / NO external collector daemon**. Do not depict the obsolete architecture anywhere else.

The main body is a left-to-right pipeline of exactly four large modules. Every module must visibly use the same three-row structure: **INPUT -> PROCESS -> OUTPUT**. Connect outputs to the next module's inputs with arrows. Add a slim feedback arrow from Execution back to Collection labeled **actual placement + action telemetry**.

## Module 1: THREAD LIFECYCLE

INPUT:
- `sched_process_fork / exec / exit`
- optional `pthread_create` uprobe
- `task_rename`
- `/proc/<pid>/task` reconciliation

PROCESS:
- eBPF tracepoints see pthread, raw clone, clone3, and descendants
- ARM64 uprobe captures optional `start_routine`
- stable identity: `(tgid, tid, generation/starttime)`
- rename is metadata; BPF coalesces per TID / 500 ms
- `/proc` reconciles pre-existing or missed threads

OUTPUT:
- lifecycle events + parent lineage
- stable thread identity
- final comm + start symbol metadata

Place a small callout inside this module: **Kernel tracepoints are authoritative; uprobe is optional enrichment.**

## Module 2: TRUSTED COLLECTION

INPUT:
- lifecycle, futex, selective VFS observations
- `/proc`: schedstat, state, CPU, context switches, allowed CPUs
- CPU / NUMA topology + calibration

PROCESS:
- libbpf loads and polls CO-RE programs
- BPF map aggregates cross-TID futex handoffs per window
- 4 MiB ring buffer transports bounded events; a dedicated worker drains every 50 ms
- health-map counts emitted / dropped / suppressed by event type
- compute rolling 30 s loss ratio
- Supervisor poll telemetry: batch size, occupancy, lag, CPU/RSS
- watchdog checks BPF health and host conditions

OUTPUT:
- trusted 60 s graph window
- per-TID demand + relation evidence
- BPF health + loss telemetry
- **hard failure** if core BPF is unhealthy

Visually show two parallel collection paths converging: **BPF maps / ring buffer** and **/proc reconciliation**. Show trust boundary gates: **loss ratio < 1%**, **health-map readable**, **Supervisor < 1 core**. A red arrow from a failed gate must go to a stop symbol labeled **HARD FAILURE: block plan/active; human recovery required**.

## Module 3: GRAPH + SOLVER

INPUT:
- stable TIDs and inferred groups
- demand `d_i = EWMA(run + runqueue)`
- futex/VFS relationship graph
- hardware latency graph, current placement, migration budget

PROCESS:
- one-time node-level planning with demand + thread-count capacity
- whole initial node-plan confirmation, then budgeted singleton pin batches
- incremental dirty frontier + dissatisfaction ranking
- bounded local move/swap solver (at most 64 exact candidates)
- per-action confirmation and per-thread dwell
- lexicographic objective: overload -> relationship latency -> migration cost

OUTPUT:
- per-TID singleton CPU plan
- objective components
- migration decisions and confidence
- committed placement in active / isolated shadow placement in plan

Place a small schematic in the module: a thread relationship graph maps through NUMA partitions to individual CPU boxes. Label the transition **solver**. Make clear this module generates a plan but does not itself change affinity.

## Module 4: SAFE EXECUTION

INPUT:
- confirmed singleton CPU plan
- live TIDs + cgroup/startup CPU envelope
- saved application affinity masks

PROCESS:
- active mode only: `sched_setaffinity(tid, singleton_mask)`
- verify every TID mask
- transactional batch rollback on failure
- vanished TID is a normal race
- watchdog monitors BPF and actuator health
- pause / cleanup / fault invokes restore

OUTPUT:
- committed active placement
- requested / committed / vanished / rollback telemetry
- verified singleton masks
- **restore result: required 100%**

Add a prominent safety bracket around this module: **Only ACTIVE may cross the actuation boundary**. Below it show: **observe = no execution | plan = no execution | active = verified execution**. A red fault path must go through **rollback / restore** to **saved masks**, ending at a green badge **RESTORE 100%**.

At the bottom, add a concise caption: **Figure 1 - What technical modules compose the currently implemented AffinityGraph eBPF Supervisor?**

Accuracy constraints:
- Spell all required technologies exactly: eBPF tracepoint, uprobe, libbpf, ring buffer, BPF map, /proc, solver, sched_setaffinity, restore, watchdog.
- Do not show LD_PRELOAD as a component. It appears only in the crossed-out exclusion label.
- Do not imply that a uprobe resolves symbols in kernel space; it captures the start routine address as optional metadata.
- Do not imply that process-level NUMA page distribution is per-thread locality.
- Avoid tiny paragraphs. Use compact labels and clear hierarchy while retaining every INPUT / PROCESS / OUTPUT row.
