# Offline Replay

`affinity-replay` reuses `HardwareGraph` and `Solver`, but links neither the BPF
reader nor collector, runtime, actuator, control socket, or Linux affinity
backend. It consumes a complete `affinitygraph.snapshot.v1` file and a reviewed
`affinitygraph.strategy.v1` TOML file:

```bash
affinity-replay --snapshot snapshot.json \
  --strategy strategies/legacy-v1.toml --output result.json
```

`legacy-v1` keeps the production ordering, four FM passes, assignments, and
the overload, relationship, and migration objective components. Non-legacy
strategies can use only the parser's allowlisted parameters. Unknown keys and
out-of-range safety values fail closed.

The result separates cross-CPU relationship-weighted latency from same-CPU
edge weight. It reports hard gates and comparison metrics but no weighted
total score. The checked-in strategies are research inputs, not active
policies. Advancing one requires offline comparison, human Pareto review,
plan-mode dry run, and a separately approved smoke.

Stateful strategies use a sequence manifest whose frames reference timestamped
snapshot files. Paths are resolved relative to the manifest:

```bash
affinity-replay --sequence sequence.json \
  --strategy strategies/incremental-hotspot-v1.toml --output sequence-result.json
```

`affinitygraph.replay-sequence-result.v1` reports each frame's solver phase,
dirty/candidate counts, action budget, shadow generation, effective state, and
solve time. The runner repeats the complete sequence to check determinism and
reports solve-time P95; it links no collector, BPF reader, actuator, `/proc`,
or affinity backend.

`affinity-domain-replay` reconstructs `numa-domain-v1` inputs from a runtime
JSONL log. It replays only windows that originally emitted a NUMA-domain plan,
preserves topology, allowed masks, complete family membership, and relation
edges, then repeats the sequence to check determinism:

```bash
affinity-domain-replay --runtime-log runtime.jsonl \
  --config config/affinitygraph.toml --output domain-result.json
```

`affinitygraph.domain-replay-result.v1` reports per-window cohesive and
cross-seeded anchors, domain families, thread count, demand, target nodes, and
solve time, plus sequence solve-time P95. This tool is also read-only and links
no runtime, BPF reader, collector, actuator, control socket, or affinity
backend.
