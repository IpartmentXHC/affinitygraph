# AffinityGraph

AffinityGraph is a dynamic singleton-CPU affinity runtime for Linux 6.6/ARM64
NUMA database servers. It observes only black-box system data online, embeds a
reduced Prism-derived eBPF collector in the target process, and never reads
throughput, P99, or YBA profiles while making decisions.

```sh
make -j4 test
build/affinity-run preflight --config config/affinitygraph.toml \
  --bpf-object build/affinitygraph.bpf.o
```

The default example is `observe`, intentionally. Read
`docs/operations.md` and `docs/validation.md` before enabling `active`.

Main artifacts are `build/libaffinitygraph.so`, `build/affinity-run`,
`build/affinityctl`, the CO-RE object from `make bpf`, the systemd unit under
`deploy/`, and JSONL lifecycle/metric/plan/action logs.

