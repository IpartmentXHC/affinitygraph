# AffinityGraph: Safe Rollout and Acceptance Stages

Create a publication-quality, vector-like academic validation workflow in a clean 16:9 landscape composition. Use a white background, dark charcoal text, thin precise arrows, and a restrained palette: teal for OBSERVE, amber for PLAN, green for ACTIVE, red only for blocked or failed paths. Typography must be crisp and readable. Avoid decoration, gradients, 3D effects, and dense prose.

The title is: **AffinityGraph eBPF Supervisor: Observe -> Plan -> Active**

Under the title show a narrow architecture banner:

**One long-running eBPF Supervisor | strict core BPF is mandatory | NO LD_PRELOAD**

The main body is a left-to-right gated rollout pipeline of exactly three large stage columns: **OBSERVE**, **PLAN**, **ACTIVE**. Every stage must visibly contain **INPUT -> PROCESS -> OUTPUT**, followed by a clearly separated **GATE** area and **FAILURE SEMANTICS** area. Between stages place a diamond review gate. Failed gates flow downward in red to a common stop/recovery lane. Successful gates flow right.

## Stage 1: OBSERVE - COLLECT ONLY

INPUT:
- eBPF tracepoints + optional uprobe
- BPF map / ring buffer
- `/proc` + topology

PROCESS:
- libbpf collection and thread reconciliation
- futex aggregation, VFS evidence, demand windows
- BPF health, rolling 30 s loss ratio, poll lag
- watchdog monitors Supervisor CPU/RSS and health-map

OUTPUT:
- trusted graph windows + health telemetry
- **actions = 0**
- large explicit badge: **NO sched_setaffinity**

GATE:
- every complete 30 s loss window `< 1%`
- no consecutive health-map read failure
- Supervisor CPU `< 1 core`, RSS `< 256 MiB`
- no hard failure
- formal observe overhead `<= 2%`

FAILURE SEMANTICS:
- **HARD FAILURE** if core BPF is unavailable or unhealthy
- block PLAN and ACTIVE
- no automatic continuation; human-reviewed recovery required

The diamond after this stage says **Observe gate passed?**. Its red branch goes to the recovery lane; its green branch proceeds to PLAN.

## Stage 2: PLAN - GENERATE ONLY

INPUT:
- accepted observe windows
- relationship + hardware graphs
- current placement and migration budget

PROCESS:
- solver: one-time node planning with demand + thread-count capacity
- whole initial plan confirmation and virtual budgeted pin batches
- incremental dirty frontier and bounded local move/swap
- per-action confirmation and per-thread dwell
- capacity and migration validation

OUTPUT:
- proposed per-TID CPU assignments
- isolated multi-window shadow placement
- objective values + confidence
- **actions = 0**
- large explicit badge: **NO sched_setaffinity**

GATE:
- plans exist and are deterministic
- capacity and migration checks pass
- BPF health remains good; rolling loss `< 1%`
- zero actuator actions

FAILURE SEMANTICS:
- invalid or unstable plan blocks ACTIVE
- BPF hard failure stops the workflow
- review and explicit recovery required

The diamond after this stage says **Plan gate + human review passed?**. Its red branch goes to the recovery lane; its green branch proceeds to ACTIVE.

## Stage 3: ACTIVE - VERIFIED EXECUTION

INPUT:
- confirmed plan
- live TIDs + valid CPU envelope
- saved application affinity masks

PROCESS:
- `sched_setaffinity` to singleton CPU masks
- per-TID verification
- watchdog + BPF health monitoring
- transactional rollback and restore

OUTPUT:
- effective active placement
- action + performance telemetry
- offline throughput / P99 statistics
- restore verification

GATE:
- measurement starts only when `effective_mode = active`
- committed and planned thread counts are greater than zero
- every planned singleton equals actual affinity
- action success: smoke `>= 80%`, formal `>= 95%`
- formal active measurement duration `>= 270 s`
- **restore = 100%**

FAILURE SEMANTICS:
- action batch failure -> rollback
- pause / cleanup / fault -> restore saved masks
- core BPF failure -> **HARD STOP**, block later rounds
- no automatic continuation; reviewed recovery required

Below ACTIVE, add a prominent green verification chain:

**effective_mode=active -> singleton masks verified -> measurement -> cleanup -> RESTORE 100%**

Across the bottom create a red common recovery lane:

**HARD FAILURE -> stop current/later plan-active work -> preserve diagnostics -> human review -> explicit recovery -> rerun gate**

Add a note beside the workflow, not as a fourth stage:

**Prism compatibility is warning-only; Prism is not run during formal performance treatments. Each treatment starts a clean ClickHouse: start -> 30 s warmup -> measurement -> cleanup.**

At the bottom, add a concise caption: **Figure 2 - How is the currently implemented AffinityGraph safely rolled out and accepted in stages?**

Accuracy constraints:
- Make the actuation boundary between PLAN and ACTIVE visually unmistakable.
- OBSERVE and PLAN never execute affinity changes. ACTIVE alone may execute.
- Spell all required technologies at least once: eBPF tracepoint, uprobe, libbpf, ring buffer, BPF map, /proc, solver, sched_setaffinity, restore, watchdog.
- Show BPF health, loss ratio, hard failure, and restore 100% clearly.
- Do not show LD_PRELOAD as a component. It appears only in the explicit exclusion label.
- Do not depict the obsolete architecture.
