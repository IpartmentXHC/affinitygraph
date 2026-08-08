# AffinityGraph

AffinityGraph is a dynamic NUMA-domain affinity runtime for Linux 6.12/ARM64
NUMA database servers. It observes only black-box system data online, embeds a
reduced Prism-derived eBPF collector in a per-target supervisor, and never reads
throughput, P99, or YBA profiles while making decisions.

```sh
make -j4 test
build/affinity-run preflight --config config/affinitygraph.toml \
  --bpf-object build/affinitygraph.bpf.o
```

The default example is `observe`, intentionally. Read
`docs/operations.md` and `docs/validation.md` before enabling `active`.

Main artifacts are `build/affinity-run`, `build/affinityctl`, the CO-RE object
from `make bpf`, the offline single-window/sequence `build/affinity-replay`,
the runtime-log NUMA-domain replay tool `build/affinity-domain-replay`,
the systemd unit under
`deploy/`, and JSONL lifecycle/metric/plan/action logs.

The default `numa-domain-v1` policy discovers stable worker families from
normalized names, start symbols, demand, and embedded-eBPF relationship
evidence. Related anchors share the smallest NUMA-node mask that provides 80%
capacity headroom; Linux schedules freely within that mask. The previous
`incremental-hotspot-v1` singleton policy remains available for replay.
Observe never solves, plan advances only shadow state, and active commits only
transactionally verified mask deltas.

Selector 模块到源文件的映射、输入输出字段和快速日志查询见
[`docs/selector-modules.md`](docs/selector-modules.md)。
