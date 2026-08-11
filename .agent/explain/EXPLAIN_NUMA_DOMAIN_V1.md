# AffinityGraph numa-domain-v1 只读解释报告

## 0. 10 行摘要

1. 当前生产 Selector 主路径是 `Runtime::maybe_solve()` -> `Runtime::solve_numa_domains()` -> `NumaDomainSolver::propose()`；`src/solver.cpp` 是旧 singleton 策略，本报告不把它当作主路径。
2. BPF 提供生命周期、futex/VFS 关系和 affinity 事件；runtime/runnable time 不是 BPF 事件，而是 `ProcCollector::sample()` 从 `/proc/<tgid>/task/<tid>/schedstat` 读取。
3. `GraphWindow::demands()` 对每个 TID 计算 `(runtime_delta + runqueue_delta) / wall_time`，再做 EWMA；代码没有独立的 runnable demand 字段。
4. 60 秒窗口中的 TID 按 `ThreadDemand.group` 聚成 family，关系边在 family 聚合后再做 family-level top-K，稳定 family 和跨 family pair 形成 domain。
5. domain demand 是成员 TID demand 的总和；单个 node 的容量是该 node 在线 CPU 数乘 `domain_capacity_ratio`，当前配置为 `0.80`。
6. `choose_nodes()` 枚举完整 NUMA node 子集，优先最少 node；因此低于单 node 容量的 domain 会优先保持单 node，关系延迟等只在 node 数相同时打破平局。
7. 已有单 node domain 的扩容候选条件是：无状态所需 node 数增加，且 `domain.demand / previous_mask_cpu_count > domain_expand_ratio`；当前没有 runnable-only 阈值。
8. 扩容候选连续确认 `domain_expand_confirmations` 次后标为 `expanded`，还要经过完整 domain+node signature 的 `domain_plan_confirmations` 次确认才会产生 ready plan；扩容判断本身没有 dwell 门槛。
9. 目标 mask 是所选 node 的全部在线 envelope CPU，再与每个线程的 `/proc` `Cpus_allowed_list` 相交；active 才交给 actuator 写入内核。
10. 当前反馈风险在代码中确实存在：绑定后 runqueue 增量可以抬高同一个 combined demand，进而影响容量和关系边；但仅凭现有 summary 不能把一次扩容唯一归因于 runnable，需关联 `thread_window` 和 `domain_details` 或增加分解日志。

## 1. 当前代码版本与解释范围

### 结论

本报告按当前 `affinitygraph` 仓库工作树中的源码解释，Git 基线为 `db6d4e2`（`fix: retain domains after recent evidence`）。主配置是 `config/affinitygraph.toml`，其中 `runtime.solver = "numa-domain-v1"`、`runtime.affinity_granularity = "numa_node_mask"`，配置文件本身的 `runtime.mode = "observe"`。

正式实验 summary 位于工作区根目录的 `experiments/doris/20260808-203309-affinitygraph-doris-random-v1-formal/summary/formal-result.json`。该 summary 报告的正式运行 `effective_mode` 为 `active`，并记录了单 node 初始 domain、`candidate_pass = false`；它不是当前源码配置的替代品。

### 解释范围

- 主路径：`src/domain_solver.cpp`、`src/runtime.cpp`、`src/graph.cpp`、`src/actuator.cpp`、`src/bpf_reader.cpp`、`src/collector.cpp`、`src/topology.cpp`、`src/affinity_run.cpp`。
- 输入采集：`bpf/affinitygraph.bpf.c`、`include/affinitygraph/bpf_events.h`。
- 配置解析：`src/config.cpp`、`config/affinitygraph.toml`。
- 测试覆盖：`tests/core_test.cpp`、`tests/bpf_lifecycle_test.cpp`、`tests/supervisor_test.sh` 及其小型 fixture。
- 离线只读 Selector 回放入口：`src/affinity_domain_replay.cpp`。它复用 `NumaDomainSolver`，不链接 runtime、BPF reader 或 actuator。

工作树已有用户改动：`.gitignore`、`Makefile`、`.agent/`、`config/ops.toml`、`scripts/`。本报告没有修改源码、BPF、测试、配置、脚本、部署文件或实验数据。

## 2. 主链路总览

主链路与代码中的真实职责如下：

```text
eBPF 采集
  -> BpfRingReader 消费 ringbuf / aggregate map
  -> Runtime 合并关系并由 ProcCollector 补充 /proc 线程采样
  -> GraphWindow 维护 60 秒线程/关系窗口和 family 输入
  -> NumaDomainSolver 生成/更新 family、domain 和 node plan
  -> mask_for_nodes 生成完整 NUMA node CPU mask
  -> Runtime 按 observe / plan / active 处理 proposal
  -> Actuator 应用 mask、回读、批次回滚、domain restore
```

### 2.1 eBPF 采集

当前模块：`bpf/affinitygraph.bpf.c`。

所属阶段：内核侧事件采集。

上游输入：tracked target、内核 task/futex/syscall/VFS 状态，以及 `pthread_create` uprobe。

当前职责：产生 task fork/exec/exit/rename、futex 关系、VFS 关系和成功的 `sched_setaffinity` 事件。futex/VFS 关系先写 aggregate map，生命周期和 affinity 写 ringbuf；`health` map 记录 emitted/dropped/suppressed 计数。

下游输出：`affinitygraph_bpf_event`，以及 `futex_aggregates`、`vfs_aggregates`、`health` map。

关键数据结构：`affinitygraph_bpf_event`、`affinitygraph_futex_key/value`、`affinitygraph_vfs_key/value`、`affinitygraph_bpf_health`。

日志：BPF 本身不写 `runtime.jsonl`；runtime 后续写 `bpf_health`、`thread_start`、`thread_exit`、`thread_name`、`application_affinity` 和 `relation_ingest_summary`。

你可以把它理解为：

- 输入：内核中的线程生命周期和线程间同步/资源交互。
- 处理：只采集关系和事件，不计算 NUMA capacity demand。
- 输出：runtime 可消费的关系/生命周期证据。
- 风险：BPF 丢失会使 `bpf_window_loss_ratio` 达到 gate，Selector 等待或失败；这不是 runnable demand 的来源。

### 2.2 `bpf_reader` 消费

当前模块：`affinitygraph/src/bpf_reader.cpp`：`BpfRingReader::drain()`、`drain_futex_aggregates()`、`drain_vfs_aggregates()`。

所属阶段：用户态 BPF 消费。

上游输入：ringbuf producer/consumer mmap、三个 BPF map fd。

当前职责：消费 ringbuf，批量 lookup-and-delete 关系 aggregate，记录 ring occupancy、lag、batch 和丢失相关统计。

下游输出：`Runtime::drain_bpf_once()` 收到 lifecycle event 和关系 aggregate event。

关键字段：`timestamp_ns`、`tid`、`peer_tid`、`start_time_ns`、`peer_start_time_ns`、`value_ns`、`resource`。

日志：间接进入 `bpf_health`、`relation_ingest_summary`；消费者自身不直接写 Selector 事件。

### 2.3 runtime 事件合并

当前模块：`affinitygraph/src/runtime.cpp`：`Runtime::drain_bpf_once()`、`consume_pending_bpf()`、`handle_bpf_event()`。

所属阶段：用户态事件归一化。

上游输入：BPF ringbuf/aggregate 输出和 lifecycle 表。

当前职责：生命周期事件更新 `lifecycle_`；futex/VFS 事件按 TID pair 和 generation 合并，在 `consume_pending_bpf()` 中换算成 `RelationObservation`；`numa-domain-v1` 保留所有 TID pair，family-level top-K 在 `domain_solver.cpp` 再做。

下游输出：`GraphWindow::observe_relation()` 的关系观察，以及线程身份、继承 mask、应用 mask。

关键字段：`RelationObservation.futex_per_second`、`shared_vfs_seconds`、`active_overlap`、`ThreadIdentity`、`allowed_cpus`。

日志：`relation_ingest_summary` 的 `input_records`、`input_pairs`、`retained_pairs`、`pruning_scope`、`weight_coverage`。

这里没有读取 runtime/runnable time。它们来自下一步 `/proc` 采样。

### 2.4 collector 补充线程 `/proc` 信息

当前模块：`affinitygraph/src/collector.cpp`：`ProcCollector::sample()`。

所属阶段：周期性线程采样。

上游输入：`/proc/<tgid>/task/<tid>/stat`、`schedstat`、`status`。

当前职责：读取线程身份和 generation、`recent_cpu`、状态、累计 `runtime_ns`、累计 `runqueue_ns`、上下文切换计数、`Cpus_allowed_list`。

下游输出：`ThreadSample`；同时 `numa_pages()` 读取 `numa_maps` 并只写 `numa_maps` 诊断日志。

关键字段：`ThreadSample.runtime_ns`、`ThreadSample.runqueue_ns`、`timestamp_ns`、`recent_cpu`、`allowed_cpus`。

日志：`thread_window` 记录窗口内的 `runtime_delta_ns`、`runqueue_delta_ns`、`current_cpu`、`allowed_cpus`、`sample_count`；`numa_maps` 记录页分布，但该页分布没有进入 `NumaDomainSolver` 的容量公式。

### 2.5 graph 窗口和 family 输入

当前模块：`affinitygraph/src/graph.cpp`：`GraphWindow::observe_threads()`、`observe_relation()`、`demands()`、`edges()`、`take_delta()`。

所属阶段：60 秒图窗口聚合。

上游输入：`ThreadSample` 和 `RelationObservation`。

当前职责：按 TID 保存线程历史，按 TID pair 保存关系历史，超过 `graph_horizon_seconds = 60` 的数据淘汰；输出线程 demand、normalized family key 和关系边。

下游输出：`Runtime::maybe_solve()` 取得 `demands()`、`thread_records()`、`edges()`，再送入 `solve_numa_domains()`。

关键数据结构：`threads_`、`relation_histories_`、`relation_aggregates_`、`ThreadDemand`、`RelationEdge`。

日志：`thread_window`、`relation_edge_summary`、`relation_edge`。

### 2.6 domain solver

当前模块：`affinitygraph/src/domain_solver.cpp`：`NumaDomainSolver::propose()`、`choose_nodes()`、`evaluate_nodes()`、`commit()`。

所属阶段：`numa-domain-v1` family/domain/NUMA node 规划。

上游输入：`ThreadDemand`、`RelationEdge`、每线程 `allowed_masks`、`HardwareGraph`、`NumaDomainOptions`。

当前职责：family gate、跨 family merge confirmation、domain component、capacity/node candidate、domain dwell、plan confirmation 和 per-TID mask proposal。

下游输出：`NumaDomainProposal`，其中有 `domains`、`planned_masks`、`actions`、`released_tids`、`ready`、`valid`。

日志：`selector_input`、`selector_output`、`plan`；domain 诊断嵌套在 `domain_details` 或 `domains`。

### 2.7 node mask 和 runtime gate

当前模块：`src/domain_solver.cpp` 的 `mask_for_nodes()` 以及 `src/runtime.cpp` 的 `solve_numa_domains()`、`maybe_solve()`。

所属阶段：把 node plan 变成可执行 proposal，并按模式决定动作。

上游输入：`target_nodes`、`HardwareGraph::cpus_in_node()`、每 TID `allowed_masks`、`Mode`。

当前职责：生成完整 node CPU mask；在 `observe` 中停止在观测；在 `plan` 中 commit 到 shadow state；在 `active` 中交给 actuator。

下游输出：`PlacementDelta.tid_to_mask` 或实际 `sched_setaffinity` 调用。

日志：`planned_masks`、`actuator_input`、`action`、`actuator_output`、`action_commit`、`domain_restore`。

### 2.8 actuator

当前模块：`affinitygraph/src/actuator.cpp`：`apply_delta()`、`Actuator::apply()`、`restore()`、`restore_all()`；内核后端是 `LinuxAffinityBackend::set()`。

所属阶段：active affinity 执行和恢复。

上游输入：已由 Selector 确认的 `PlacementDelta`、live TID 集合、保存的 restore mask。

当前职责：写 mask，内核回读比对，批次失败逆序回滚，释放 domain 时恢复策略介入前的 mask。

下游输出：`ApplyResult`、`RestoreResult` 和 `current_masks_`/solver commit。

日志：`actuator_input` 描述计划输入；`action` 和 `actuator_output` 描述应用结果；`domain_restore` 和 `runtime_stop` 描述恢复结果。

## 3. 模块职责表

| 模块 | 所属阶段 | 上游输入 | 当前职责 | 下游输出 | 关键日志 |
|---|---|---|---|---|---|
| `bpf/affinitygraph.bpf.c` | 内核采集 | task/futex/VFS/syscall | 生命周期、关系、affinity 事件 | ringbuf/map | 间接 `bpf_health` |
| `src/bpf_reader.cpp` | BPF 消费 | ringbuf、aggregate map | 解码、批量消费、lag 统计 | BPF events | `bpf_health` 间接 |
| `src/runtime.cpp:handle_bpf_event()` | 事件归一化 | BPF events | lifecycle、关系和应用 mask 状态 | graph / lifecycle | `thread_start`、`relation_ingest_summary` |
| `src/collector.cpp:ProcCollector::sample()` | 线程采样 | `/proc` | runtime、runqueue、CPU、允许 mask | `ThreadSample` | `thread_window` 间接 |
| `src/graph.cpp:GraphWindow` | 图窗口 | samples、relations | 60 秒 history、demand、edges、group | `ThreadDemand`、`RelationEdge` | `thread_window`、`relation_edge` |
| `src/domain_solver.cpp:NumaDomainSolver` | NUMA Selector | demand、edges、topology、allowed mask | family、domain、capacity、node 数、plan | `NumaDomainProposal` | `selector_input/output`、`plan` |
| `src/topology.cpp:HardwareGraph` | 拓扑 | CPU envelope、sysfs、calibration | CPU->node、node distance、node CPU 集合 | `HardwareGraph` | `topology_cpu`、`topology_edge` |
| `src/runtime.cpp:maybe_solve()` | 模式控制 | proposal、Mode | observe/plan/active 分支 | shadow commit 或 actuator input | `solve_window_*` |
| `src/actuator.cpp:apply_delta()` | affinity 执行 | per-TID CPU mask | set、回读、回滚、restore | `ApplyResult`/`RestoreResult` | `action`、`actuator_output` |

## 4. numa-domain-v1 决策链

### Q1：family 是如何形成的？

结论：family 不是由线程名白名单直接选出，而是由 `ThreadDemand.group` 形成；group 由 `GraphWindow::normalize_group()` 根据稳定的 comm 和创建入口生成。

证据：`GraphWindow::demands()` 将每个线程输出为 `{identity, group, demand, confidence, current_cpu}`。`normalize_group()` 会去掉末尾编号；若有 `start_symbol`，优先使用 symbol，避免 ASLR 地址造成 family 漂移。注释也明确 parent lineage 不进入 family key。

代码位置：`affinitygraph/src/graph.cpp:113` 附近 `normalize_group()`，`affinitygraph/src/graph.cpp:128` 附近 `demands()`；生命周期中的创建入口来自 `src/runtime.cpp:391` 的 `thread_started()` 和 `handle_bpf_event()`。

处理细节：

1. `Runtime::maybe_solve()` 取得当前 60 秒窗口的 `ThreadDemand`。
2. `NumaDomainSolver::propose()` 以 `by_family[thread.group]` 聚合 TID，并把成员 demand 相加。
3. 同 family 的 `RelationEdge.score` 累加到 `internal[name]`；不同 family 的边同时累加到两端 `external`，并累加到 `cross[minmax(a,b)]`。
4. 对每个 family 计算 `demand`、`internal_relation`、`external_relation`、`self_containment = internal / (internal + external)`、`relative_internal = internal / 全局最大 internal`。
5. 单 family cohesive gate 需要四项同时满足：`family_minimum_demand`、`family_minimum_internal_relation`、`family_minimum_self_containment`、`family_minimum_relative_internal`。当前配置分别是 `1.0`、`1.0`、`0.20`、`0.10`。
6. cohesive 证据连续满足 `family_stability_confirmations = 3` 次后成为 `cohesive_anchor`。
7. 跨 family 关系在 TID 证据聚合之后，按每 family 最多 `family_edges_per_family = 4` 条做 deterministic top-K。pair 需要两端绝对 demand/internal gate，以及 `cross_relation / min(internal_left, internal_right) >= domain_merge_ratio`，当前 ratio 为 `0.25`。
8. 跨 family pair 连续确认 `domain_stability_confirmations = 3` 次后成为 `cross_seed`。确认的 pair 用 union-find 合并 anchor，形成 family component。

是否会合并：会。已确认的跨 family pair 会进入同一个 component；`domain_id()` 用按字典序排列的 family 名称以 `+` 连接。正在确认的 pair 会设置 `cross_pending`，暂缓输出 singleton，避免先绑定后合并。

你可以把它理解为：

- 输入：TID 的稳定 group、TID demand、TID pair relation score。
- 处理：先完整聚合 TID 证据，再做 family gate 和跨 family confirmation。
- 输出：`FamilyMetric` 和已合并的 family component。
- 风险：demand 同时参与 family demand gate 和 relation edge 的 activity；runqueue 上升不仅可能影响 capacity，也可能间接改变 family/domain 证据。

### Q2：domain 是如何形成的？

结论：domain 是一个 family component 对应的 TID 集合，不是单一 NUMA node；它在当前 proposal 中可以先规划一个或多个 node，最终 `target_nodes` 才表达它使用几个 node。

证据：`NumaDomainSolver::propose()` 对每个 `components` 构造 `NumaDomain`，填入 `id`、`families`、`tids` 和成员 demand 总和，然后调用 `choose_nodes()`。

代码位置：`affinitygraph/src/domain_solver.cpp:353` 附近 component/domain 构造，`src/domain_solver.cpp:364` 附近 `NumaDomain` 初始化；状态字段在 `include/affinitygraph/core.hpp` 的 `NumaDomain` 和 `NumaDomainSolver` 中。

domain 生命周期：

- `domain_nodes_[domain.id]` 保存最近一次 commit 的 node 集合。
- `placement_` 保存最近一次实际确认/继承的 per-TID mask。
- `domains_` 保存最近一次已提交的 domain 诊断和成员集合。
- `expand_confirmations_`、`shrink_confirmations_` 保存 node 数变化候选的连续确认计数。
- `last_changed_ns_` 记录 node mask 最近 commit 变更时间。
- `last_confirmed_domain_evidence_ns_` 记录完整 family/domain 证据最近被确认的时间。
- `pending_signature_`、`global_plan_confirmation_` 对完整 domain+target node signature 做 plan confirmation。

`domain_minimum_dwell_seconds = 300` 有两种实际用途：

1. capacity 收缩必须距离最近 node change 足够久。
2. 已提交联合 domain 的完整关系证据刚确认后，如果当前窗口短暂丢边，dwell 内保留原 family 集合和 mask，输出 `held_domain_dwell`。

代码中未发现扩容使用 `minimum_dwell_ns` 的条件。扩容有自己的连续确认，但没有同等的扩容 dwell gate。

## 5. capacity demand 计算逻辑

### 5.1 demand 的真实公式

结论：当前 NUMA domain 使用的 demand 是每个 TID 的 combined CPU pressure，不是仅 runtime，也不是仅 runnable。

证据：`GraphWindow::demands()` 对连续样本计算：

```text
pressure_i = clamp((runtime_delta_i + runqueue_delta_i) / timestamp_delta_i, 0, 1)
ewma_1 = pressure_1
ewma_i = 0.0645 * pressure_i + 0.9355 * ewma_(i-1)
```

代码位置：`affinitygraph/src/graph.cpp:128-157` 附近 `GraphWindow::demands()`。

解释：

- `runtime_delta` 来自 `/proc/.../schedstat` 的第一个累计值。
- `runqueue_delta` 来自 `schedstat` 的第二个累计值，代码命名是 `runqueue_ns`，不是 `runnable_ns`。
- 分母是两个 sample 的 `timestamp_ns` 差，不是固定硬编码的 1 秒；采样周期由 `sample_interval_seconds` 控制。
- 历史只保留 `graph_horizon_seconds = 60` 秒，但 demand 的 EWMA 是按窗口内有效相邻样本顺序递推，没有另外的 peak、p95 或 EWMA 衰减配置。
- `confidence` 是有效 pressure interval 覆盖度：`pressure.size() / max(1, horizon_seconds - 1)`，上限为 1；在 `numa-domain-v1` 的 `propose()` 中没有用 `minimum_confidence` 过滤这些线程。

### 5.2 聚合层级和 capacity 比较

结论：capacity 比较在 domain 层发生，domain demand 是其所有成员 TID demand 的简单总和。

证据：`NumaDomainSolver::propose()` 在构造 domain 时执行 `domain.demand += member.demand`；`evaluate_nodes()` 计算：

```text
online_cpu_count = mask_for_nodes(hardware, candidate_nodes).size()
capacity_limit = online_cpu_count * capacity_ratio
capacity_headroom = capacity_limit - domain.demand
```

代码位置：`affinitygraph/src/domain_solver.cpp:88-137` 的 `NodeChoice/evaluate_nodes()`，以及 `src/domain_solver.cpp:367-374` 的 domain demand 聚合。

当前配置：`domain_capacity_ratio = 0.80`；`domain_expand_ratio = 0.90`；`domain_shrink_ratio = 0.55`。

重要边界：

- `background_demand` 只用于候选 node 的排序诊断，没有加到 `capacity_headroom` 中。
- `relation_latency` 只用于同样 node 数候选间的排序，不改变 capacity_limit。
- `numa_pages()` 输出的内存页分布不进入 domain demand 或 capacity 公式。
- `allowed_masks` 不参与 `choose_nodes()` 的 `online_cpu_count` 计算；它只在 node 选择完成后与 `domain.target_mask` 相交。因此 cgroup/application allowed mask 可能使线程实际可用 CPU 少于 capacity planner 看到的 envelope CPU，当前代码只在最终交集为空时把 proposal 标为 invalid。
- 当前代码没有 separate `runtime_demand`、`runnable_demand`、CPU utilization、runqueue length 或 memory bandwidth demand 字段。
- capacity 的“容量”是 envelope 中 online CPU 数乘 ratio，不是硬件内存容量，也不是由校准 CSV 的 STREAM 值计算出来。

你可以把它理解为：

- 输入：每线程 combined demand、domain 成员、hardware CPU->node 映射。
- 处理：成员 demand 求和，与候选完整 node mask 的 `0.80 * CPU 数` 比较。
- 输出：`capacity_limit`、`capacity_headroom`、无状态所需 node 集合。
- 风险：runqueue 上升和真正 runtime 上升在这个比较中没有区分；同一 combined demand 也会被用于关系 activity。

## 6. runnable time 参与决策的方式

### 6.1 采集和进入 demand

结论：runnable time 通过 `/proc` `schedstat` 的 `runqueue_ns` 进入，而不是通过 BPF map/event 进入。

证据：`ProcCollector::sample()` 执行 `schedstat >> sample.runtime_ns >> sample.runqueue_ns`；`GraphWindow::demands()` 把相邻样本的两个增量直接相加。

代码位置：`affinitygraph/src/collector.cpp:20-63` 附近 `ProcCollector::sample()`；`src/graph.cpp:128-157` 附近 `GraphWindow::demands()`。

### 6.2 是否有 runnable 专用扩容阈值

结论：代码中没有发现 runnable time 专用阈值、runnable 专用确认计数或 runtime/runnable 分权重。

代码中的实际判断只有：

```text
required_nodes = choose_nodes(domain.demand, capacity_ratio)
utilization = domain.demand / previous_mask_cpu_count
expand = required_nodes.size() > previous_nodes.size()
         && utilization > expand_ratio
```

这里的 `domain.demand` 已经包含 runtime 和 runqueue。`domain_expand_ratio = 0.90` 不是 runnable 阈值，而是 combined demand 相对于已有 node CPU 数的阈值。

### 6.3 单 node 绑定后的反馈路径

结论：代码结构允许该反馈路径发生，但“本次 workload 中 runqueue 上升的实际因果量”必须由日志或回放确认。

证据路径：

1. active 下 `Actuator::apply()` 改变线程 affinity mask。
2. 下一轮 `ProcCollector::sample()` 继续读取这些 TID 的 `schedstat`。
3. 如果绑定后的 `runqueue_ns` 增量提高，`GraphWindow::demands()` 的 combined `demand` 会提高。
4. `GraphWindow::edges()` 还用 `sqrt(demand[from] * demand[to])` 计算 `activity`，所以 demand 变化可能同时改变 relation score/family 证据。
5. domain planner 重新计算 `required_nodes` 和 `utilization`；满足扩容候选条件并连续确认后，target mask 可能变成两个 node。

代码中没有区分“真实 CPU capacity 不足”和“affinity 绑定造成的排队升高”。当前只使用 combined demand、候选 node capacity、expand/shrink confirmation 和部分 dwell 机制。

不确定点：现有 `thread_window` 有 runtime/runqueue delta，但没有把 EWMA 拆成两个贡献，也没有记录上一轮和本轮的 `utilization`/`required_nodes`。因此仅凭一个 `domain_details.node_decision = expanded` 不能数学上证明扩容完全由 runnable 触发。

## 7. 单 node 到双 node 扩容条件

### 7.1 决策表

| 条件 | 当前值来源 | 判断逻辑 | 结果 | 代码位置 | 日志字段 |
|---|---|---|---|---|---|
| 单 node capacity 可容纳 | `domain.demand`、`HardwareGraph::cpus_in_node()` | 候选 node `capacity_headroom >= 0`；单 node 容量为 CPU 数 `* 0.80` | `choose_nodes()` 可选单 node，且排序优先少 node | `src/domain_solver.cpp:evaluate_nodes()`、`choose_nodes()` | `capacity_limit`、`capacity_headroom`、`target_nodes` |
| demand 超过单 node 的 80% capacity | combined `domain.demand` | 单 node 候选被 `capacity_headroom < 0` 排除，通常 `required_nodes.size()` 增至 2 | 对初始 domain 直接选择双 node；对已有 domain 还要经过下一行的 expand gate | `src/domain_solver.cpp:143-188` | `domain_details.capacity_headroom`、`target_nodes` |
| 已有 domain 的扩容候选 | 当前 demand、`domain_nodes_[id]` | `required_nodes.size() > previous.size()` 且 `domain.demand / previous_mask.size() > 0.90` | `expand = true`，计数递增 | `src/domain_solver.cpp:394-412` 附近 | `previous_nodes`、`expand_confirmation`、`node_decision` |
| runnable time 超过独立阈值 | 代码未提供 | 未发现 `runnable_threshold` 或 `runqueue` 单独判断 | 代码中没有此条件；runqueue 只能通过 combined demand 影响上一行 | `src/graph.cpp:128`、`src/domain_solver.cpp:394` | 当前只有 `runqueue_delta_ns`，没有 runnable threshold |
| 连续扩容确认 | `expand_confirmations_[domain.id]` | `expand` 为真时递增，否则清零；达到 `domain_expand_confirmations = 3` | `domain.node_decision = "expanded"` | `src/domain_solver.cpp:401-418` | `expand_confirmation`、`node_decision` |
| domain minimum dwell | `last_changed_ns_`、`last_confirmed_domain_evidence_ns_` | **扩容分支没有 dwell 判断**；dwell 用于 shrink 和关系证据保持 | 扩容不等待 300 秒；收缩或关系短暂缺失才受 dwell 约束 | `src/domain_solver.cpp:394-418`、`src/domain_solver.cpp:514-574` | 间接 `previous_nodes`、`node_decision = held_domain_dwell`；无剩余时间字段 |
| 完整 plan confirmation | `plan_signature()`、`global_plan_confirmation_` | domain id、target node 集合、valid 状态连续相同达到 `domain_plan_confirmations = 3` | `proposal.ready = true`，才生成 actions | `src/domain_solver.cpp:575-645` | domain `confirmation`、`selector_output.ready` |
| plan 模式 | `Config.mode` | proposal ready 后 `domain_solver_.commit()`，不调用 actuator | shadow state 更新，不写内核 affinity | `src/runtime.cpp:1099-1111` | `shadow_commit`、`solve_window_end.outcome=shadow_committed` |
| active 模式 | `Config.mode` | proposal ready 后先 restore released TID，再 `actuator_.apply()` | 写入 target mask；成功后 commit | `src/runtime.cpp:1117-1195` | `actuator_input`、`domain_restore`、`action`、`actuator_output`、`action_commit` |

### 7.2 当前代码的关键阈值关系

对已有单 node domain，代码有两个不同的比例：

1. `capacity_ratio = 0.80` 决定无状态 `choose_nodes()` 哪些 node 子集可行。
2. `expand_ratio = 0.90` 决定从已有 node 集合扩容的候选是否足够强。

因此，已提交单 node 的典型路径是：

```text
combined domain demand
  -> 单 node 在 0.80 capacity 下不可行
  -> required_nodes = 两个 node
  -> combined demand / 已有单 node CPU 数 > 0.90
  -> 连续 3 次 expand confirmation
  -> 连续 3 次完整 plan signature confirmation
  -> target_nodes 变成两个 node
```

这里存在一个需要 Modify Agent 特别关注的边界：如果单 node 已经不满足 `0.80`，但 `demand / previous_cpu_count` 尚未超过 `0.90`，`choose_nodes()` 会给出更大的 `required_nodes`，而 lifecycle 分支会保留旧 node，输出 `held_existing` 或 `held_expand_pending`，重新评估后 `capacity_headroom` 可能为负。代码没有把这一状态标成 invalid，也没有立即扩容。

### 7.3 domain dwell 对扩容问题的实际作用

结论：当前 `domain_minimum_dwell_seconds = 300` 不能阻止上述扩容候选本身。

证据：`shrink` 条件包含 `now_ns - last_changed_ns_[domain.id] >= minimum_dwell_ns`；同一段的 `expand` 条件没有该表达式。另一个 dwell 分支只在候选 domain 不再完整覆盖已提交 family 时保留旧 domain。

不应把 `domain.confirmation` 和 `expand_confirmation` 混为一谈：前者是完整 plan signature confirmation，后者是 node 扩容候选 confirmation。

## 8. node mask 生成逻辑

### 8.1 node 选择

当前模块：`affinitygraph/src/domain_solver.cpp`：`choose_nodes()`、`evaluate_nodes()`。

所属阶段：capacity 可行候选和具体 NUMA node 选择。

上游输入：`HardwareGraph` 的 online CPU/node、domain demand、domain 成员当前 CPU、其他已放置 domain、跨 family relation score。

当前职责：枚举 hardware 中所有 node 子集，过滤 capacity 不足的候选，并按以下字典序选择：

1. node 数最少。
2. `relation_latency` 最小。
3. `background_demand` 最小。
4. `initial_migrations` 最少。
5. node ID 字典序最小。

下游输出：`NumaDomain.target_nodes`。

代码位置：`src/domain_solver.cpp:143-188` 的 `choose_nodes()`，`src/domain_solver.cpp:97-137` 的 `evaluate_nodes()`。

结论：选择具体 node 时考虑了 CPU 数量和 `HardwareGraph.node_distance`；没有使用 numa page locality、内存容量、STREAM calibration 或单 CPU latency 作为 capacity 条件。`numa_maps` 虽然被采样和记录，但没有传给 planner。

### 8.2 从 target node 到 per-TID mask

当前模块：`src/domain_solver.cpp:42` 附近 `mask_for_nodes()`，以及 `propose()` 中的 allowed-mask intersection。

处理：

1. `mask_for_nodes()` 对每个 `target_node` 调用 `hardware.cpus_in_node(node)`，收集该 node 的所有 online CPU，排序去重。
2. 这得到 domain 级完整 node mask，并写入 `domain.target_mask`。
3. 对每个成员 TID，从 `allowed_masks[tid]` 取得当前 `/proc` `Cpus_allowed_list`。
4. 实际目标是 `intersect_masks(domain.target_mask, allowed_masks[tid])`；没有 allowed entry 时使用完整 domain mask。
5. 任何成员的交集为空，整个 proposal 标记 `empty_application_mask_intersection`，并清空 `delta.tid_to_mask`，不会只执行部分 domain。

容量边界：`allowed_masks` 只影响第 3-5 步，不回流到第 1 步的 `online_cpu_count` 和 `capacity_limit`。所以非空但很窄的 per-TID allowed mask 可能产生比 planner 预期更小的实际执行 mask；这是当前实现的 UNKNOWN 风险点，现有代码没有单独的“有效可用 CPU capacity”诊断。

结论：domain 内线程先共享同一个 node-level target，但最终 per-TID mask 可能因为各自 application/cgroup allowed mask 不同而不同；代码不会把 mask 强行扩大到 allowed mask 之外。

日志：`domain_details.target_nodes`、`domain_details.target_mask`、`plan.planned_masks`、`selector_output.domain_details` 和 `domain_actions.target_mask`。

## 9. observe / plan / active 行为差异

### observe

当前模块：`src/runtime.cpp:1199` 附近 `maybe_solve()`。

结论：observe 会采样并生成图/线程窗口/关系日志，但对当前 runtime 主循环而言，不会调用 `solve_numa_domains()`。

证据：`maybe_solve()` 先记录 `thread_window` 和 `relation_edge`，随后遇到 `if (config_.mode == Mode::Observe)` 直接写 `solve_window_end.outcome = "observed"` 并 return；NUMA Selector 分支在其后。

结果：当前 `config/affinitygraph.toml` 的 observe 模式不会产生本轮 `selector_input`、`selector_output` 或 `plan`，也不会执行 mask。它不是“Selector 计算后只不执行”的模式，而是这条 runtime 路径上提前停止在观测。

### plan

当前模块：`src/runtime.cpp:878-1111` 的 NUMA 分支。

结论：plan 会进入 `NumaDomainSolver`，等待 BPF gate、family/domain/plan confirmations；proposal ready 后调用 `domain_solver_.commit()`，但不调用 `Actuator`。

结果：`current_masks_` 和 solver 内部 placement 是 shadow/计划状态，日志通常出现 `plan`、`shadow_commit`，不应解释为内核已经接受 mask。

### active

当前模块：`src/runtime.cpp:1117-1195`，以及 `src/affinity_run.cpp:454-464`。

结论：active 才真正写 affinity。`affinity_run` 在 active 下做 affinity authority 检查；runtime 在 proposal ready 后先处理 released TID 的 restore，再调用 `Actuator::apply()`。

结果：只有 `action`/`actuator_output.success = true` 且之后 `action_commit`，才表示 runtime 将成功写入的 TID 计入 solver placement。应用失败会 discard proposal；domain restore 失败会触发 fatal path。

### 模式切换和 preflight

- mode 由启动时 `load_config()` 解析，代码中没有把 observe/plan/active 作为运行时控制 socket 的切换命令；控制 socket 的 pause/resume 只暂停或恢复工作。
- BPF gate 在 `solve_numa_domains()` 中要求 reader 存在、health 有效、至少约 30 秒 BPF health window ready、loss ratio 小于 1%。
- `affinity-run preflight` 检查 config、CPU envelope、calibration、BTF/libbpf/CO-RE 可用性；它不做 domain plan，也不执行 affinity mask。
- 正式 summary 的 `effective_mode` 为 `active`；当前仓库主配置仍是 `observe`，两者不能混写。

## 10. 反馈回路分析

### 反馈回路分析

1. **代码中是否确实存在该路径？**

   结论：结构路径确实存在，但一次实际扩容是否主要由 runnable 造成仍是 UNKNOWN。

   证据：active mask 由 `Actuator::apply()` 改变；后续 `/proc` `schedstat` 采样进入 `GraphWindow::demands()`；combined demand 再进入 `NumaDomainSolver::propose()`。此外 demand 还进入 `GraphWindow::edges()` 的 activity 分量。

   代码位置：`src/actuator.cpp:107`、`src/collector.cpp:20`、`src/graph.cpp:128`、`src/domain_solver.cpp:192`。

2. **哪个函数把 runnable time 转成 demand？**

   `GraphWindow::demands()`。实际字段是 `ThreadSample.runqueue_ns`，实际输出字段是 `ThreadDemand.demand`；`runqueue_delta_ns` 只在 `ThreadWindowRecord`/`thread_window` 日志中保留诊断值。

3. **哪个函数把 demand 转成扩容决策？**

   `choose_nodes()` 先用 `evaluate_nodes()` 的 `capacity_headroom` 找到无状态可行 node 集；`NumaDomainSolver::propose()` 随后用 `required_nodes.size()`、旧 `domain_nodes_` 和 `utilization > options.expand_ratio` 设置 `expand`，并维护 `expand_confirmations_`。

4. **是否有机制区分“真实容量不足”和“绑定后正常排队升高”？**

   结论：没有发现这种区分机制。

   现有机制只有 combined demand、`capacity_ratio`、`expand_ratio`、连续 confirmation，以及主要用于 shrink/关系保持的 dwell。没有 runtime-only baseline、runnable-only threshold、unbind counterfactual、CPU utilization 单独证据或 memory pressure 证据。

5. **当前机制为什么会把适合单 node 的 domain 错误扩到双 node？**

   当单 node affinity 造成 `runqueue_delta_ns` 增长时，它会与 `runtime_delta_ns` 相加，提高 TID EWMA 和 domain demand；如果 combined demand 使无状态候选需要两个 node，且相对旧单 node CPU 数超过 `0.90`，连续三次 expand confirmation 后，planner 没有“关系仍适合单 node”的保护条件，会把 target node 集合改成两个。完整 plan confirmation 后，active actuator 会对变更 TID 写双 node mask，可能增加迁移和跨 node 运行。

   代码能够证明前半段决策链，不能单独证明吞吐/P99 退化的全部原因。正式实验 summary 记录的单次双 node 负 uplift 和 P99 退化属于实验事实，应与逐窗口 runtime log 对齐。

6. **哪些日志字段可以验证这个反馈？**

   按同一 `window_id` 和同一 domain id 对齐：

   - `thread_window.runtime_delta_ns` 与 `thread_window.runqueue_delta_ns`：判断 demand 上升来自哪个累计量。
   - `thread_window.demand` 与 `selector_input.total_demand`：确认输入 scalar 是否上升。
   - `selector_output.domain_details.previous_nodes`、`target_nodes`、`capacity_limit`、`capacity_headroom`：确认 node 数变化和容量证据。
   - `expand_confirmation`、`node_decision`：确认是否经过连续扩容候选。
   - `plan.domains`、`planned_masks`、`action.actions.target_nodes/target_mask`：确认计划是否真的从单 node 变成双 node。
   - `action`、`actuator_output`、`action_commit`：确认 active 是否实际应用。

   但现有日志没有拆出 EWMA 的 runtime/runnable 两个贡献，也没有记录未被采用的 `required_nodes` 候选。因此“runnable-only trigger”需要额外回放或新增诊断字段确认。

## 11. 可用于验证的日志字段

### 日志验证方案

| 日志字段 | 来源文件 | 含义 | 如何证明扩容是由 runnable time 触发 |
|---|---|---|---|
| `thread_window.group`、`tid`、`starttime` | `src/runtime.cpp:1219` | 逐 TID 身份和 family 输入 | 先确认比较的是同一 TID generation 和同一 domain 成员 |
| `thread_window.runtime_delta_ns` | `src/runtime.cpp:1219` | 60 秒线程 history 当前首尾 runtime 增量 | 与下一行的 runqueue 增量并列比较；若 runtime 基本不变而 runqueue 明显升高，支持 runnable 归因 |
| `thread_window.runqueue_delta_ns` | `src/runtime.cpp:1219` | `schedstat` runqueue 累计量的窗口增量 | 这是当前代码中唯一直接暴露 runnable time 的字段；它本身不是独立 demand |
| `thread_window.demand`、`confidence` | `src/runtime.cpp:1219`、`GraphWindow::demands()` | combined demand EWMA 和覆盖度 | 对齐前后窗口，确认 runqueue 增量是否伴随 combined demand 上升；不能单独拆分 EWMA 贡献 |
| `selector_input.total_demand`、`threads`、`families` | `src/runtime.cpp:933` | 本轮进入 NUMA Selector 的总输入摘要 | 验证 domain planner 收到的总 demand 是否在绑定后升高 |
| `family_metrics[].demand` | `src/runtime.cpp:1019` 的 `family_metrics_json()` | family 成员 demand 总和 | 确认升高的 demand 是否落在被扩容 domain 的 family |
| `domain_details.demand` | `src/runtime.cpp:1019` 的 `domains_json()` | domain 成员 demand 总和 | 直接比较扩容前后 capacity 输入 |
| `capacity_limit`、`capacity_headroom` | `domains_json()` | 候选最终 mask 的 CPU capacity 和剩余量 | 确认单 node 是否在 `0.80` capacity 下不可行；headroom 不是 runnable 专用证据 |
| `previous_nodes`、`target_nodes`、`target_mask` | `domains_json()` | 已提交 node 集和本轮最终 node/mask | `previous_nodes` 为 1 个 node、`target_nodes` 为 2 个 node 才能证明最终计划扩容 |
| `expand_confirmation` | `domains_json()` | 当前 domain 的扩容候选连续计数 | 达到配置的 3 才支持 `node_decision=expanded` 的确认链 |
| `shrink_confirmation` | `domains_json()` | 收缩候选连续计数 | 排除把 shrink/dwell 事件误读成 expand |
| `node_decision` | `domains_json()` | `initial`、`stable`、`held_expand_pending`、`expanded`、`held_domain_dwell` 等最终解释 | `expanded` 说明 planner 采用扩容；`held_expand_pending` 说明候选尚未采用 |
| `domain_details.confirmation` | `domains_json()` | 完整 domain+node plan signature confirmation | 确认 proposal 是否达到 `ready`，不要与 `expand_confirmation` 混淆 |
| `selector_output.ready`、`valid`、`invalid_reason` | `src/runtime.cpp:1019` | Selector proposal 是否可执行 | 排除 empty mask、最大线程数或 BPF gate 导致的非扩容结果 |
| `plan.planned_masks`、`actions[].target_nodes` | `src/runtime.cpp:1054` | 计划输出和实际 action 目标 | 证明 Selector 输出了双 node mask，而非只改变了 family 证据 |
| `actuator_input` | `src/runtime.cpp:1118` | active 写入前的 mask/action 输入 | 证明该双 node 计划进入 actuator |
| `action.success`、`requested`、`committed`、`forced_migrations` | `src/runtime.cpp:1155` | affinity apply 结果 | 证明双 node 计划实际写成功及迁移规模 |
| `actuator_output.rollback_success` | `src/runtime.cpp:1164` | 失败批次是否成功回滚 | 区分 planner 扩容和 actuator 执行失败 |
| `domain_restore`、`restore_*` | `src/runtime.cpp:1130`、析构函数 | domain release 或停止时 restore 结果 | 确认旧 domain 成员是否恢复原 mask |
| `bpf_health.window_loss_ratio`、`bpf_window_ready` | `src/runtime.cpp:1608` | BPF gate 的 30 秒窗口健康度 | 排除关系输入缺失导致的 proposal 等待；它不证明容量扩容原因 |

### 建议新增日志字段（仅方案，不修改代码）

当前缺少以下直接证据：

- 每 TID 的 `runtime_pressure`、`runqueue_pressure` 和最终 combined `demand`，或至少 domain/family 两个来源的聚合值。
- `required_nodes`（`choose_nodes()` 的无状态结果）与最终保留的 `target_nodes`；现在只能看到最终值，看不到 `held_expand_pending` 时原本想要几个 node。
- `previous_cpu_count`、`utilization`、`expand_candidate`、`expand_confirmed`。
- `last_changed_ns`、evidence dwell 剩余时间和当前 `global_plan_confirmation_` 的原始值。
- 一条明确的 `node_decision_reason`，区分 capacity candidate、expand confirmation、shrink dwell、domain evidence dwell 和 mask intersection。

这些字段应作为诊断字段，不应让日志重新计算一套与 solver 不一致的公式；最好直接从 `evaluate_nodes()`/`propose()` 使用的值输出。

## 12. 当前测试缺口

| 场景 | 当前是否覆盖 | 缺失点 | 建议测试入口 |
|---|---|---|---|
| 1. 单 node domain 在 runnable time 升高时不被错误扩展到双 node | 未覆盖 | `test_identity_reuse_and_demand()` 只验证一次 combined demand；没有先 commit 单 node、再只改变 runqueue、再经过 expand/plan confirmations 的序列 | `tests/core_test.cpp`：新增 `NumaDomainSolver` lifecycle fixture；优先匿名 TID，不涉及 Doris 名称 |
| 2. 真实容量不足时仍然可以扩容 | 部分覆盖 | `test_numa_domain_capacity_atomicity_and_envelope()` 覆盖初始 demand 需要双 node，但没有覆盖已有单 node -> 双 node 的连续确认和 active action 链 | `tests/core_test.cpp`：在同一 domain 先低 demand commit，再提高 combined demand，验证 `expand_confirmation`、`expanded`、双 node mask |
| 3. domain dwell 能抑制震荡 | 部分覆盖 | `test_numa_domain_capacity_dwell_uses_node_change_time()` 覆盖收缩 dwell；`test_numa_domain_dwell_holds_transient_relation_gap()` 覆盖关系证据保持；没有扩容候选与收缩交替的长序列，也暴露了扩容没有 dwell gate | `tests/core_test.cpp`：增加 expand/shrink 交替时间序列，并明确检查 `last_changed_ns` 语义 |
| 4. plan 确认不会误触发 active 行为 | 未覆盖 NUMA runtime 端到端 | core 测试直接调用 solver，`supervisor_test.sh` 主要验证 observe/degraded；没有 fake backend 注入 Runtime plan 模式并断言无 `set` | `tests/core_test.cpp` 可先覆盖 proposal/commit 语义；`tests/supervisor_test.sh` 或单独 supervisor fixture 覆盖 plan 不写 affinity |
| 5. mask 扩缩容可以 restore | 部分覆盖 | `test_transactional_rollback()`、`test_inherited_restore_mask()`、`test_selective_restore_releases_cooled_thread()` 覆盖 Actuator；未覆盖 NUMA domain release/expand 后的完整 `domain_restore` 和失败路径 | `tests/core_test.cpp`：FakeBackend + `PlacementDelta.tid_to_mask`；Runtime active 集成则由 `tests/supervisor_test.sh` 扩展 |
| BPF lifecycle 和关系输入 | 已覆盖采集生命周期，不等于 planner | `tests/bpf_lifecycle_test.cpp` 覆盖线程事件、rename/exit/clone 等行为，但不验证 `runqueue_ns` 到 capacity 的决策 | `tests/bpf_lifecycle_test.cpp` 保持采集边界；planner 用匿名 replay fixture |
| 离线 selector determinism | 已有入口 | `src/affinity_domain_replay.cpp` 可从 runtime JSONL 读取 `thread_window`/`relation_edge`/topology 并重复 replay；现有小型 snapshot 不含 runqueue 分解和扩容序列 | `tests/fixtures/` 增加脱敏序列，调用已有 domain replay 入口；不要把 runtime log 全量提交 |

## 13. 给 Modify Agent 的建议

以下是小范围方案，不是正式 patch；不硬编码 Doris 线程名，不降低 Selector 阈值，也不直接关闭扩容。

### 建议 1

目标：让 capacity planner 能区分 `runtime` 和 `runqueue` 对 combined demand 的贡献。

涉及文件：`affinitygraph/src/collector.cpp`、`affinitygraph/src/graph.cpp`、`affinitygraph/include/affinitygraph/core.hpp`、`affinitygraph/src/domain_solver.cpp`。

涉及函数：`ProcCollector::sample()`、`GraphWindow::demands()`、`NumaDomainSolver::propose()`/`evaluate_nodes()`。

修改思路：保留现有 combined `ThreadDemand.demand` 作为 family/relationship 兼容输入，同时以最小附加诊断/规划字段保留 runtime 与 runqueue 的独立窗口贡献；capacity expand gate 先使用 reviewed 的 runtime/capacity evidence，runnable 作为辅助证据或单独确认条件，避免其单独把 domain 推过扩容线。具体权重和条件应由 fixture/replay 先固定，不在代码中写 workload 名称。

需要新增测试：combined demand 与两个分量的 fixture；runtime 不变、runqueue 上升时不扩容；runtime 同时上升且单 node capacity 确实不足时仍扩容。

风险：改变 domain demand 语义会影响 family gate、relation activity 和历史 replay；应先用日志字段验证，再修改实际 gate，并保留旧字段兼容。

### 建议 2

目标：修正 `capacity_ratio = 0.80` 与 `expand_ratio = 0.90` 之间的生命周期边界。

涉及文件：`affinitygraph/src/domain_solver.cpp`，必要时只同步 `include/affinitygraph/core.hpp` 的 proposal 诊断字段。

涉及函数：`choose_nodes()`、`evaluate_nodes()`、`NumaDomainSolver::propose()`。

修改思路：明确区分“无状态 required node set”和“已有 domain 是否允许采用该 set”。当旧单 node 在 capacity candidate 上已经不可行时，不要产生 `capacity_headroom < 0` 但继续保留旧 mask 的静默状态；改为输出可解释的 pending/invalid 状态，或按经过确认的容量证据进入扩容。保留最少 node、关系延迟、background demand 的排序逻辑。

需要新增测试：demand 落在 `0.80` 与 `0.90` 之间、刚超过 `0.90`、以及连续确认中途回落的三个边界。

风险：可能改变已有 domain 的 action 时机；必须用 `node_decision`、`required_nodes`、headroom 回放验证，不要用调低阈值规避。

### 建议 3

目标：避免短暂绑定后排队波动直接完成扩容。

涉及文件：`affinitygraph/src/domain_solver.cpp`、`affinitygraph/config/affinitygraph.toml`（由 Modify Agent 决定是否引入已评审配置项）。

涉及函数：`NumaDomainSolver::propose()` 中 expand confirmation 分支。

修改思路：在现有 `domain_expand_confirmations` 之上增加与 node 最近变更时间一致的扩容保护，或要求连续窗口内容量证据和关系/domain 成员同时稳定；该保护应是可配置、可回滚的，不应直接禁用 expansion。注意只改变扩容采用条件，不改变 family Selector 阈值。

需要新增测试：单 node commit 后的短暂 runqueue spike、持续真实 capacity pressure、expand 后立即回落三组序列。

风险：过长保护会延迟真实扩容；必须与建议 2 的 capacity evidence 一起测，不应只靠 dwell 延迟掩盖问题。

### 建议 4

目标：先让每一次扩容都能被审计为“什么候选、什么阈值、什么确认”触发。

涉及文件：`affinitygraph/src/domain_solver.cpp`、`affinitygraph/src/runtime.cpp`。

涉及函数：`evaluate_nodes()`、`NumaDomainSolver::propose()`、`Runtime::solve_numa_domains()` 的 `domains_json()`/selector log 路径。

修改思路：直接输出 solver 已使用的 `required_nodes`、old/new CPU count、utilization、expand candidate/confirmed、runtime/runqueue 分量和 dwell 状态；不要由日志消费者猜测公式。

需要新增测试：日志 fixture 校验字段与 proposal 中的数值一致，尤其是 held pending 不应伪装成 expanded。

风险：日志体积增加；应只输出 domain 聚合和必要的 per-TID 分量，并保持现有 `window_id` 关联。

### 建议 5

目标：建立可重复的匿名回归场景，再决定通用性。

涉及文件：`affinitygraph/tests/core_test.cpp`、`affinitygraph/tests/fixtures/`、已有 `src/affinity_domain_replay.cpp` 入口。

涉及函数：优先测试 `GraphWindow::demands()` 和 `NumaDomainSolver::propose()`，不需要启动数据库。

修改思路：使用匿名 family/TID 和两 node synthetic topology，构造低 runqueue 单 node、绑定后 runqueue 升高、真实 runtime/capacity 超载、回落恢复四段 sequence。验证 selector 的 planned target、confirmation、dwell、mask 和 restore。

需要新增测试：至少覆盖本报告第 12 节前五行；fixture 只保留脱敏 topology、demand 分量和 relation evidence，不提交完整 benchmark/runtime 数据。

风险：fixture 若只模拟 combined demand，仍无法验证 runnable 反馈；必须保留 runtime/runqueue 分量或等价的可解释输入。

## 14. UNKNOWN 与待确认问题

- UNKNOWN：正式实验某个具体双 node placement 的逐窗口 `runtime.jsonl` 是否同时满足“runtime 基本不变、runqueue 增长、`node_decision=expanded`、三次 expand confirmation”。当前只读到了 formal summary，没有用完整 runtime log 做因果归因。
- UNKNOWN：该正式实验的每个 double-node window 是否发生在同一个 domain id、同一个 family member set 上；summary 中的部分 `active_domains` 是采样点状态，不能替代逐窗口日志。
- UNKNOWN：实际硬件上每个 node 的 online CPU 数、node distance 和 calibration 是否在所有实验窗口保持不变；源码只读启动时 topology。
- UNKNOWN：`numa_maps` 记录的内存页分布是否与双 node 退化相关；当前 planner 不使用它，因此需要实验分析而不是代码推断。
- UNKNOWN：当前正式实验使用的具体 config 文件路径；summary 证明 effective mode 为 active，但不提供完整启动命令和每窗口 domain log。
- 代码中未发现 runnable 专用阈值、runnable 专用 confirmation、runtime/runnable 分权重或 capacity-only counterfactual。
- 代码中未发现扩容 dwell 条件；现有 `domain_minimum_dwell_seconds` 主要约束 shrink 和 domain evidence hold。
- restore 的成功定义是 backend set 后精确回读成功，或 TID 已退出（`ESRCH`）被计为 vanished；`restore_all()` 在返回后会清空 acted/restore 状态，即使存在非零 `failed`，运行时需要依赖日志/上层故障处理确认后续策略。不要把 `restored=true` 与“所有仍存活 TID 都已恢复”混为一谈。

## 15. 推荐阅读顺序

1. `affinitygraph/config/affinitygraph.toml`：先确认 `solver`、`affinity_granularity`、capacity/expand/shrink/confirmation/dwell 配置。
2. `affinitygraph/include/affinitygraph/core.hpp`：查看 `ThreadSample`、`ThreadDemand`、`NumaDomain`、`NumaDomainOptions`、`NumaDomainProposal` 的边界。
3. `affinitygraph/src/collector.cpp`：确认 runtime 和 runqueue 的原始来源。
4. `affinitygraph/src/graph.cpp`：阅读 60 秒淘汰、demand EWMA、group normalization、relation edge score。
5. `affinitygraph/src/runtime.cpp:1199` 附近：理解 solve window、observe early return 和输入日志。
6. `affinitygraph/src/runtime.cpp:878` 附近：理解 NUMA options、BPF gate、selector/plan/action 日志和模式分支。
7. `affinitygraph/src/domain_solver.cpp:192` 附近：按 family gate -> pair confirmation -> component -> domain demand 的顺序读。
8. `affinitygraph/src/domain_solver.cpp:143` 附近：阅读 `choose_nodes()`/`evaluate_nodes()` 的 capacity 和 node 排序。
9. `affinitygraph/src/domain_solver.cpp:394` 附近：阅读已有 domain 的 expand/shrink confirmation 与 dwell；这是当前最高优先级修改点。
10. `affinitygraph/src/domain_solver.cpp:42` 和 `src/actuator.cpp`：确认 node mask、allowed-mask intersection、apply/readback/rollback/restore。
11. `affinitygraph/tests/core_test.cpp:242` 之后的 NUMA domain fixture：先看 family/merge，再看 dwell、capacity atomicity 和 envelope。
12. `affinitygraph/src/affinity_domain_replay.cpp`：最后用真实 runtime JSONL 的 `thread_window`、`relation_edge`、`plan` 做离线确定性复核，不启动 runtime，不改变 affinity。
