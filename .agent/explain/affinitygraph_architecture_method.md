# AffinityGraph detailed architecture figure specification

Create a publication-quality academic architecture diagram for the Linux NUMA-aware thread relationship analysis and dynamic affinity runtime named AffinityGraph. The figure must explain the actual `numa-domain-v1` code path, not a generic scheduler.

Use a clean landscape layout on a white background, suitable for a two-column paper figure. Use four visually distinct horizontal bands with thin borders and restrained colors: Observation Plane, Graph Representation Plane, NUMA Decision Plane, and Execution and Safety Plane. Use solid arrows for the forward data path, dashed arrows for feedback and control, and small numbered stage markers. Use compact rectangular modules with short labels, readable typography, consistent alignment, and no decorative illustrations, gradients, 3D effects, or large empty hero areas.

The top Observation Plane must show two parallel input sources:

1. A Kernel eBPF Collector block labeled `bpf/affinitygraph.bpf.c` with sublabels `task fork/exec/exit/rename`, `futex relation`, `VFS relation`, `successful sched_setaffinity`, and maps `events`, `futex_aggregates`, `vfs_aggregates`, `health`.
2. A Proc Sampling block labeled `src/collector.cpp: ProcCollector::sample()` with sublabels `/proc/<tgid>/task/<tid>/stat`, `schedstat: runtime_ns + runqueue_ns`, `recent_cpu`, `Cpus_allowed_list`, and `context switches`.

Both sources must point into a user-space `src/runtime.cpp` block labeled `BpfRingReader + Runtime event ingestion`, with sublabels `drain ringbuf/maps`, `lifecycle_`, `RelationObservation`, `generation check`, and `allowed_masks`. Make it visually clear that runnable time comes from `/proc/schedstat`, not from eBPF.

The Graph Representation Plane must contain:

1. `GraphWindow::observe_threads()` and `observe_relation()`.
2. A large `60 s graph window` block with `thread history per TID`, `relation history per TID pair`, and horizon eviction.
3. A prominent equation block: `pressure = clamp((runtime_delta + runqueue_delta) / wall_time, 0, 1)` and `EWMA = 0.0645 * pressure + 0.9355 * previous`.
4. An output block `ThreadDemand` with fields `identity`, `group`, `demand`, `confidence`, `current_cpu`.
5. A `RelationEdge` block with `activity`, `sync`, `share`, `stability`, and `score`; show that `activity` depends on thread demand.
6. A family formation block labeled `GraphWindow::normalize_group()` and `by_family[group]`, with the note `stable comm + start symbol; no name whitelist`.

The NUMA Decision Plane must be the visual center of the figure and show the actual sequential stages inside `src/domain_solver.cpp: NumaDomainSolver::propose()`:

1. `Family metrics`: aggregate TID demand and edge scores into `internal_relation`, `external_relation`, `self_containment`, and `relative_internal`; show the four family gates and `family_stability_confirmations = 3`.
2. `Cross-family merge`: aggregate cross-family relation, family-level top-K (`family_edges_per_family = 4`), `merge_ratio >= 0.25`, `domain_stability_confirmations = 3`, then union-find components.
3. `Domain`: a set of families and TIDs, with `domain.demand = sum(member.demand)` and `maximum_threads_per_domain = 1024`.
4. A highlighted `Capacity And Node Planner` block labeled `choose_nodes() + evaluate_nodes()` with the equations `capacity_limit = online_cpu_count * 0.80` and `capacity_headroom = capacity_limit - domain.demand`. Show exhaustive NUMA node subset enumeration, then ranking by `minimum node count -> relation_latency -> background_demand -> initial_migrations -> node ID`.
5. A separate highlighted expansion decision block labeled `existing domain lifecycle` with `utilization = demand / previous_mask_cpu_count`, `expand if required_nodes grow AND utilization > 0.90`, `expand confirmations = 3`, `shrink if utilization < 0.55`, `shrink confirmations = 6`, and a red warning label `No runnable-only threshold; expansion uses combined demand`. Clearly show that expansion itself has no dwell condition, while shrink and evidence retention use `domain_minimum_dwell_seconds = 300`.
6. A domain state block with `domain_nodes_`, `placement_`, `domains_`, `expand_confirmations_`, `shrink_confirmations_`, `last_changed_ns_`, `last_confirmed_domain_evidence_ns_`, and `global_plan_confirmation_`.

The Node Mask and Execution band must show:

1. `mask_for_nodes()` converting `target_nodes` to all online CPUs in the selected NUMA nodes.
2. Per-thread intersection with `/proc Cpus_allowed_list`, producing `PlacementDelta.tid_to_mask` and `planned_masks`.
3. A mode gate with three labeled branches: `observe: collect/log only; returns before NUMA solve`, `plan: solve + shadow commit; no kernel write`, and `active: solve + actuator apply`.
4. `src/actuator.cpp: Actuator` with `sched_setaffinity`, exact readback, batch transaction, reverse rollback, `restore_masks_`, domain restore, and runtime-stop restore.
5. Logs around the path: `thread_window`, `selector_input`, `selector_output`, `plan`, `actuator_input`, `action`, `actuator_output`, `action_commit`, `domain_restore`.

Add a dashed feedback arrow from `active affinity mask` back to `ProcCollector::sample()` labeled `binding may raise runqueue_delta`, then through `combined demand` to the planner. Add a warning callout in this loop: `runtime and runnable are currently coupled; code cannot distinguish true capacity shortage from binding-induced queueing`.

Add a small side callout near mask planning: `allowed_masks constrain final per-TID masks but do not reduce choose_nodes() online_cpu_count`, and another callout near topology: `HardwareGraph: CPU -> NUMA node, node distance; numa_maps is logged but not used by planner`.

Use exact code identifiers in monospace-like labels wherever possible. Keep all text inside boxes, avoid overlap, and maintain a clear left-to-right flow. The final image should read as a detailed architecture figure for a systems/OS research paper, with the capacity/runnable/node-mask decision chain visually dominant.
