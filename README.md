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
`build/affinitygraph.bpf.o` (from `make all` or `make bpf`), the offline
single-window/sequence `build/affinity-replay`,
the runtime-log NUMA-domain replay tool `build/affinity-domain-replay`,
the systemd unit under
`deploy/`, and JSONL lifecycle/metric/plan/action logs.

The shipped `config/affinitygraph.toml` defaults to the `incremental-hotspot-v1`
singleton policy; the `numa-domain-v1` policy (family evidence and NUMA-node
masks) remains available by configuration. The JSON thread-profile path can
optionally pin selected threads to explicit CPU lists on start, including a
static-hold mode for placement experiments. Observe never solves, plan
advances only shadow state, and active commits only transactionally verified
mask deltas.

Selector 模块到源文件的映射、输入输出字段和快速日志查询见
[`docs/selector-modules.md`](docs/selector-modules.md)。

不使用 YBA、手动启动 Doris + YCSB 的两种压测方式（带放置文件 / 动态
solver）见 [`docs/doris-manual-testing.md`](docs/doris-manual-testing.md)。
