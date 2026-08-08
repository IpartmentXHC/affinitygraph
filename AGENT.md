# 当前任务目标：以 Doris 为目标研究 Selector

## 总目标

当前阶段以 Doris 为唯一优化目标。先利用已有 embedded-eBPF plan 日志、
离线 replay 和短时正控制实验，找出 selector 未能稳定表达的优化机会，
形成边界清晰、可独立验证的算法模块。完成 Doris 机制验证后，再判断这些
机制是否是跨 workload 的通用优化点，以及是否应进入默认通用策略。

当前阶段不追求 ClickHouse 正收益，不运行长时间正式实验，也不通过降低
`P_g`、降低 `S_g` 或加入 Doris 线程名白名单来满足既定 oracle。

## 已有证据

- 所有短时 profile 使用相同的 embedded BPF。实验均为两轮、30 秒
  warmup、120 秒 measurement，BPF loss 为 0，mask 校验和 restore 均通过。
- Doris `light_pipe_unlimited` 相对 BPF-on unrestricted 两轮提升分别为
  33.21% 和 33.65%，平均 33.43%。selector 存在较大的优化空间。
- Doris `light_pipe_limited` 相对 all-thread one-node 平均仅提升 1.43%，
  且一胜一负。当前证据支持“把关键执行域收敛到一个 NUMA node”，不支持
  “必须把其余线程隔离到另一个 node”。
- `Pipe_normal` 在 40 个 replay 窗口中的 37 个满足完整 cohesive gate。
  `brpc_light` 在 30 个窗口满足 demand 和绝对组内关系门槛，但只有 1 个
  窗口满足完整 cohesive gate；实际主要失败项是相对组内强度 `P_g`。
- 修正后的无名称 replay 在连续 26 个窗口形成
  `Pipe_normal + brpc_light` domain，包含完整 640 个线程并排除
  `brpc_heavy`。稳态 domain demand 约为 22.37-23.88 CPU，适合一个
  32-CPU node 的 80% 容量门槛。
- ClickHouse `ThreadPool:0-31` 相对 `0-63` two-node 平均下降 4.94%。
  ClickHouse 当前作为负控，防止 selector 为旧 oracle 过拟合。

这些结果是机制和方向证据，不是正式性能结论。两轮短测不能证明跨时段
稳定性、最佳目标 node，也不能证明该机制可以推广到其他数据库。

## 当前研究问题

1. Doris 的主要收益来自关键 family 的关系局部性、总体负载的单节点收敛，
   还是两者的组合？
2. `Pipe_normal + brpc_light` 是否必须共同受管；仅控制其中一个 family
   能否复现收益？
3. 收益是否与具体 node ID 无关，还是受到非受管 demand、IRQ 或内存位置
   的影响？
4. 关系 seed 在启动、稳态和 workload 结束阶段如何 acquire、hold、release，
   才能避免暂态证据波动造成 domain churn？
5. 80% 容量门槛在 Doris 约 23 CPU demand 时是否提供足够 headroom，扩缩容
   是否应使用预测 demand、峰值或置信区间？
6. 哪些特征可以匿名化后在其他 workload 上保持选择能力，哪些只是 Doris
   的局部机制？

## 算法模块

### 1. Family Evidence Aggregator

- 保留完整 TID 关系证据，先按规范化名称和 start symbol 聚合 family，再做
  deterministic per-family heavy-hitter 剪枝。
- 输出 `D_g`、`I_g`、`X_g`、`S_g`、`P_g`、pair weight `X_gh`、覆盖率、
  稳定性和缺失原因。
- eBPF 是唯一在线关系来源；不得使用吞吐、P99、YBA 标签或线程名白名单。

### 2. Relational Seed Selector

- 保留 cohesive singleton 路径。
- 允许双方 demand 和绝对组内证据达标的强跨组边直接建立 seed domain，
  `S_g` 和 `P_g` 仅作为关系类型特征，不作为所有 seed 的统一硬门槛。
- 外部边很强但没有绝对组内证据的 handler/heavy family 必须保持排除。
- 不降低当前 `S_g=0.20`、`P_g=0.10`。

### 3. Domain Lifecycle Controller

- acquire、hold、release 分离计数；建立 domain 需要连续确认，短时关系下降
  不立即拆域，退出和恢复仍必须有界且可解释。
- domain family 集、目标 node 和 mask 分别维护 signature，普通权重变化不得
  触发 node 或 CPU 级 churn。
- 新成员继承当前 domain mask；family 消失或策略退出时 100% restore。

### 4. Capacity And Node Planner

- 使用完整 domain demand 选择满足 headroom 的最少 node 集，输出容量余量和
  扩缩容原因。
- 在 node 数相同时依次比较跨 domain 关系延迟、非受管 demand、首次迁移和
  node ID；已稳定的目标 node 具有 dwell/stickiness。
- Doris 当前先验证单 node 机制，不假设 `64-95` 是唯一正确 node。

### 5. Atomic Membership And Actuation

- 控制完整 family，不做 top-64 截断；超过 1024 线程时整域无约束并令 plan
  无效。
- family 全体线程使用完整 NUMA-node mask，与资源和应用声明 mask 取交集。
- 继续保证事务回滚、mask 规范化校验、无 CPU singleton move/swap 和 100%
  restore。

### 6. Causal Evaluation And Genericity

- Doris discovery 对照至少包含 unrestricted、all-thread one-node、pair-only
  one-node、单 family one-node，以及不同目标 node。所有 profile 使用同一
  embedded BPF 和相同 envelope。
- 先使用离线 replay 和 plan 验证选择正确性，再做短时 active smoke；当前
  阶段不进入正式长实验。
- 将 family 名称匿名化，用 Doris 正例、ClickHouse 负例和后续 workload 做
  leave-one-workload-out 检查。只有不依赖名称、阈值不针对单一 workload、
  并在至少一个独立 workload 上复现机制时，才提出通用策略候选。

## 当前阶段交付物

1. 生成 Doris family/pair 的逐窗口证据表，明确每次 acquire/hold/release 和
   node 选择的原因。
2. 为上述六个模块定义输入、输出、状态和不变量，并补匿名化 fixture。
3. 用已有 Doris 日志验证 `Pipe_normal + brpc_light`，用 ClickHouse 日志验证
   不产生错误 domain，结果必须 deterministic 且 solver P95 小于 1 秒。
4. 设计并执行必要的短时 Doris 因果 smoke，区分 family 关系局部性与单节点
   容量收敛；不扩大为正式长实验。
5. 基于跨 workload 证据提交“Doris 专用机制、可复用模块、通用策略候选”
   三层结论，不提前把 Doris 结果写成通用策略。

## 安全与停止条件

- BPF 加载、健康或 loss gate 失败时停止，不降级到其他在线关系来源。
- mask 校验、事务回滚或 restore 失败时停止 active 实验。
- 新算法未通过 replay 和 plan 前不得执行 affinity。
- 不修改 Doris/ClickHouse 配置、内存策略或 `0-127` 资源 envelope，除非任务
  明确批准新的因果对照。
