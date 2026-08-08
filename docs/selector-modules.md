# Selector 算法模块与日志

本文对应 `AGENT.md` 中的六个算法模块，说明线上调用链、主要源文件、输入输出
以及用于快速排障的结构化日志。`numa-domain-v1` 不读取吞吐、P99 或 workload
名称白名单；在线关系数据只来自 embedded eBPF。

## 调用链

```text
embedded BPF + /proc samples
        |
        v
Runtime::consume_pending_bpf / reconcile_and_sample
        |
        v
GraphWindow::demands + GraphWindow::edges
        |
        v
Runtime::solve_numa_domains
        |
        v
NumaDomainSolver::propose
        |
        +--> family evidence --> relational seed --> domain lifecycle
        |                                      --> capacity/node planner
        v
PlacementDelta (tid -> CPU mask)
        |
        v
Actuator::apply / restore
```

## 模块与源文件

| 模块 | 主要源文件 | 输入 | 输出 |
|---|---|---|---|
| Family Evidence Aggregator | `src/runtime.cpp`, `src/graph.cpp`, `src/domain_solver.cpp` | BPF pair、线程采样、规范化 family | `D_g/I_g/X_g/S_g/P_g`、family gate |
| Relational Seed Selector | `src/domain_solver.cpp` | family 指标、聚合后的 `X_gh` | cohesive anchor、cross seed、pair confirmation |
| Domain Lifecycle Controller | `src/domain_solver.cpp`, `include/affinitygraph/core.hpp` | anchor/seed、历史 signature、确认状态 | domain family 集、ready、release、expand/shrink 状态 |
| Capacity And Node Planner | `src/domain_solver.cpp`, `src/topology.cpp` | domain demand、NUMA 拓扑、背景 demand、历史 node | target nodes、完整 node mask、容量余量、node decision |
| Atomic Membership And Actuation | `src/actuator.cpp`, `src/runtime.cpp` | `PlacementDelta`、live TID、原始/应用 mask | commit/rollback/restore 结果、实际生效 placement |
| Causal Evaluation And Genericity | `src/affinity_domain_replay.cpp`, `src/affinity_replay.cpp`, `tests/core_test.cpp`, `tests/fixtures/` | runtime JSONL、匿名 fixture、策略配置 | deterministic replay、正负例、solve P95 |

公共数据结构位于 `include/affinitygraph/core.hpp`，配置解析位于
`src/config.cpp`，默认门槛位于 `config/affinitygraph.toml`。BPF 事件格式和采集
分别位于 `include/affinitygraph/bpf_events.h`、`bpf/affinitygraph.bpf.c` 和
`src/bpf_reader.cpp`。

## 日志事件

所有事件使用相同 `window_id` 关联。快速排障时优先看下面四类事件；
`thread_window` 和 `relation_edge` 用于需要逐 TID 重建时的深挖。

### `selector_input`

在 BPF gate 判断和 solver 调用之前记录，回答“本窗口实际输入了什么”：

- `threads`、`families`、`relation_edges`
- `total_demand`、`total_relation`
- `allowed_masks`、`empty_allowed_masks`
- `online_cpus`、`online_nodes`、`current_managed_threads`
- BPF reader/health/window/loss 及 `bpf_gate_passed`
- `thresholds`：family、pair、confirmation、容量和扩缩容门槛

BPF gate 不通过时仍会输出该事件，随后产生 `evaluated=false` 的
`selector_output`，因此首次失败窗口不会丢失上下文。

### `selector_output`

记录 solver 的完整可解释输出：

- 输入 family/pair 数、聚合后 top-K pair 数
- cohesive eligible/anchor、cross-pending/cross-seed family 状态
- qualified/confirmed pair 数
- domain、受管 family/thread、planned mask、继承、释放和迁移数量
- `family_metrics`：每个 family 的原始指标、四个 gate 和确认状态
- `family_pairs`：`X_gh`、分母、merge ratio、绝对证据 gate 和确认状态
- `domain_details`：旧/新 node、mask、demand、容量余量、背景 demand、首次
  迁移数、expand/shrink confirmation 和 `node_decision`

`node_decision` 的值包括：

- `initial`：首次选择 node，或该 domain 尚无已 commit 的历史 node
- `stable`：无状态最优解与历史 node 一致
- `held_existing`：候选 node 变化但策略保持历史 node
- `held_expand_pending` / `held_shrink_pending`：等待确认或 dwell
- `held_domain_dwell`：关系或 demand 短时消失，但已提交 family domain 仍在
  300 秒最小 dwell 内；保持原 node mask，避免阶段切换时释放后重绑
- `expanded` / `shrunk`：扩缩容条件已确认

`cross_pending=true` 表示 family 已出现满足绝对门槛的强跨组候选，但 pair
尚未完成连续三个窗口确认。pending 期间 cohesive anchor 不会抢先形成
singleton domain；候选短暂消失时，只保留与完整 plan 确认等长的 acquisition
grace，pair confirmation 仍会归零，因此不会降低关系稳定门槛。

### `actuator_input` / `actuator_output`

只在 active 模式产生。input 记录待恢复 TID、目标 mask 和强制迁移；output
区分 restore/apply 阶段，记录 requested/applied/committed/vanished/rollback、
错误码和回滚结果。selector 输出正确但内核未生效时，应从这两条事件定位，
不要把 actuator 故障误判为 selector 错误。

### 兼容日志

- `plan`：保留原有消费者所需字段，并新增 `family_pairs` 和 node 诊断字段。
- `action`、`domain_restore`、`action_commit`：保留执行和恢复生命周期。
- `relation_ingest_summary.pruning_scope=family_solver`：确认 NUMA-domain 路径
  未在 TID 层提前剪枝。

## 快速查看命令

查看每个窗口的输入和输出摘要：

```bash
jq -c 'select(.type=="selector_input" or .type=="selector_output") |
  del(.family_metrics,.family_pairs,.domain_details)' runtime.jsonl
```

查看 family 卡在哪个 gate：

```bash
jq -c 'select(.type=="selector_output") |
  .window_id as $w | .family_metrics[] |
  {window_id:$w,name,demand,internal_relation,self_containment,
   relative_internal,demand_eligible,internal_relation_eligible,
   self_containment_eligible,relative_internal_eligible,
   cohesive_eligible,cohesive_anchor,cross_pending,cross_seed}' runtime.jsonl
```

查看跨组 seed 的建立过程：

```bash
jq -c 'select(.type=="selector_output") |
  .window_id as $w | .family_pairs[] |
  {window_id:$w,left,right,cross_relation,denominator,merge_ratio,
   endpoints_eligible,qualifies,confirmation,confirmed}' runtime.jsonl
```

查看 node 选择和容量余量：

```bash
jq -c 'select(.type=="selector_output") |
  .window_id as $w | .domain_details[] |
  {window_id:$w,id,demand,previous_nodes,target_nodes,target_mask,
   capacity_limit,capacity_headroom,background_demand,initial_migrations,
   node_decision,confirmation}' runtime.jsonl
```

## 不变量

- family 聚合先于 heavy-hitter 剪枝。
- `S_g/P_g` 是 cohesive 特征，不是 cross seed 的统一硬门槛。
- cross seed 双方必须满足 demand 和绝对组内关系门槛。
- pending cross seed 暂缓 singleton；首次 active placement 必须是非空且完整
  确认的 domain，空计划不能报告 `active_effective=true`。
- domain 成员是完整 family；超限整域无约束，不做部分绑定。
- 输出是完整 NUMA-node mask，不生成稳态 CPU move/swap。
- plan 不调用 affinity；active 必须事务执行并在退出时 100% restore。
