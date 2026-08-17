# Doris 手动测试教程（不使用 YBA）

本教程面向**手动启动 Doris + YCSB** 的压测场景：不用 YBA 编排，直接用
`affinity-run` 监督 Doris 进程，用 YCSB 打负载。覆盖两种场景：

- **场景 A：带放置文件（thread profile）**——用已验证的静态放置画像，把指定
  线程（如 Doris 的 `brpc_light`、`Pipe_normal`）钉到指定 CPU 集合。
- **场景 B：无放置文件**——让 runtime 的动态 solver
  （`incremental-hotspot-v1`，默认）自行观察关系证据并迁移线程。

## 0. 前置准备（两种场景通用）

```sh
# 构建（make all 已包含用户态程序 + BPF 对象 build/affinitygraph.bpf.o）
make all CXX=/usr/bin/clang++-18 CLANG=/usr/bin/clang-18

# 校准（每台机器一次，sudo + 实测延迟；无实测数据时也能生成，但 is_estimated 标记启发值）
sudo -A make calibrate
# 或直接放置人工审核过的 CSV 到 /etc/affinitygraph/calibration/hardware-node-edges.csv

# 检查内核/工具链/校准是否就绪
sudo -A ./build/affinity-run preflight --config config/affinitygraph.toml \
  --bpf-object build/affinitygraph.bpf.o
# 期望输出中 bpf: ok；若为 fail/disabled，先解决 BPF 后再继续
```

准备一份测试专用配置 `/etc/affinitygraph/targets/doris.toml`（或直接改
`config/affinitygraph.toml` 并复制到 `/etc/affinitygraph/`）：

```toml
[runtime]
mode = "observe"                    # 场景 A 需切 active；场景 B 从 observe 起步
sample_interval_seconds = 1
graph_horizon_seconds = 60
solve_interval_seconds = 10
minimum_confidence = 0.8
proposal_confirmations = 3
solver = "incremental-hotspot-v1"   # 默认；numa-domain-v1 需配 [calibration] 关系尺度
affinity_granularity = "singleton_cpu"
maximum_managed_threads = 128
log_directory = "/var/log/affinitygraph-doris"   # 建议独立目录
socket_path = "/tmp/affinitygraph-doris.sock"    # 与 affinityctl --socket 对应

[resources]
# 省略 cpus 则自动继承进程/cgroup 亲和掩码；也可写死或靠 --cpus/AFFINITY_CPUS 覆盖
calibration_path = "/etc/affinitygraph/calibration"

[collector]
required = true                     # 生产/实验必须 true（需要 BPF）
pthread_uprobe = true

[calibration]
# 非 ClickHouse 负载必须替换为经评审的离线尺度，否则不要使用关系尺度相关特性
id = "doris-manual-test"
activity_log_p95 = 1.0
sync_log_p95 = 1.0
share_log_p95 = 1.0
```

要点：
- **CPU 信封优先级**：`--cpus` > `AFFINITY_CPUS` > TOML `resources.cpus` >
  `sched_getaffinity()`；信封必须落在启动时 cgroup/进程亲和掩码内，否则
  preflight 报 `cpu_envelope: fail`。
- **profile 路径优先级**：`--thread-profile` > `AFFINITY_THREAD_PROFILE` >
  TOML `runtime.thread_profile`。
- 本场景**必须有 BPF**（`collector.required=true` 且 preflight `bpf: ok`）：
  多进程的 Doris 依赖 fork/exec 跟踪发现全部线程；`required=false` 仅用于
  隔离降级测试。

## 1. 手动编排 Doris（两种场景相同）

`affinity-run run` 监督**一个长驻命令**，BPF 递归跟踪它的整棵进程树（包括
daemon 化、setsid 的后代）。Doris 的 FE/BE 会 daemon 化，所以用一段保持存活
的 wrapper 把它们包起来：

```sh
sudo -A ./build/affinity-run run \
  --config /etc/affinitygraph/targets/doris.toml \
  --bpf-object build/affinitygraph.bpf.o \
  --user doris \
  -- bash -c '/opt/doris/fe/bin/start_fe.sh --daemon \
               && /opt/doris/be/bin/start_be.sh --daemon \
               && wait'
```

- `--user doris`：supervisor 加载 BPF 后把子进程降权为 doris 用户；Doris 也
  需要以该用户写 FE/BE 目录。
- wrapper 不退出，`affinity-run` 就一直监督；wrapper 退出时 runtime 会执行
  restore 并退出。
- 另开一个终端启动 YCSB（它只是负载源，不归 affinity-run 管理）：

```sh
# 按你的 YCSB 客户端实际用法，目标是 Doris FE 的 MySQL 协议端口
./bin/ycsb run jdbc -P workloads/workloada \
  -p jdbc.url="jdbc:mysql://127.0.0.1:9030/db" -p jdbc.user=root \
  -p operationcount=100000 -threads 32
```

启动后先确认跟踪和健康：

```sh
sudo -A ./build/affinityctl status --socket /tmp/affinitygraph-doris.sock
# 关注 effective_mode、collector_degraded=false、target_tgids 数量（应覆盖 FE+BE 全部进程）
sudo -A ./build/affinityctl dump  --socket /tmp/affinitygraph-doris.sock
```

## 2. 场景 A：使用放置文件（静态放置实验）

适用：已有实验验证的放置方案（参考
`config/thread-profiles/doris-light-pipe-node2-*.candidate.json`，Doris
`brpc_light`/`Pipe_normal` → node2 CPU 集合），做复现/确认实验。

### 2.1 准备 profile

复制样例并改到你的机器上（**CPU 集合必须是本机信封内、且与实测拓扑一致**）：

```json
{
  "schema_version": 1,
  "profile_id": "doris-light-pipe-node2",
  "generated_at": "2026-08-17T00:00:00Z",
  "status": "candidate",
  "source": {"commit": "main", "experiment_id": "doris-manual-a", "test_id": "r1"},
  "applicability": {"description": "doris manual run"},
  "dynamic": {"enabled": false, "small_step_threads": 1,
              "large_change_ratio": 0.3, "large_step_threads": 4,
              "cooldown_seconds": 10},
  "placements": [
    {"id": "brpc-light", "match": {"comm": "brpc_light", "comm_prefix": null,
      "cgroup": null, "cgroup_prefix": null, "tid": null},
     "allowed_cpus": "64-95", "dynamic": false,
     "affinities": [{"cpus": "64-95", "count": 512}]},
    {"id": "pipe-normal", "match": {"comm": null, "comm_prefix": "Pipe_normal",
      "cgroup": null, "cgroup_prefix": null, "tid": null},
     "allowed_cpus": "64-95", "dynamic": false,
     "affinities": [{"cpus": "64-95", "count": 128}]}
  ]
}
```

- `match` 支持 `comm`/`comm_prefix`/`cgroup`/`cgroup_prefix`/`tid` 的任意组合；
  一个规则至少一个匹配字段。
- `affinities` 按 `count` 顺序分配实例（前 512 个 `brpc_light` → `64-95`，
  下一个亲和组给第 513+ 个）。
- `allowed_cpus` 与每个 `affinities[].cpus` 必须都在资源信封内，否则 preflight
  直接拒绝。
- `status` 只能是 `candidate` 或 `tested`。

### 2.2 preflight 校验 profile

```sh
sudo -A ./build/affinity-run preflight \
  --config /etc/affinitygraph/targets/doris.toml \
  --thread-profile /etc/affinitygraph/profiles/doris-light-pipe.json \
  --bpf-object build/affinitygraph.bpf.o
# 期望 thread_profile: ok (2 placement rule(s), static)
```

### 2.3 启动（active 模式才会真正落盘放置）

把配置里的 `mode` 改为 `active`，然后：

```sh
sudo -A ./build/affinity-run run \
  --config /etc/affinitygraph/targets/doris.toml \
  --thread-profile /etc/affinitygraph/profiles/doris-light-pipe.json \
  --profile-output /var/log/affinitygraph-doris/profiles/candidate-r1.json \
  --experiment-id doris-manual-a --test-id r1 \
  --bpf-object build/affinitygraph.bpf.o \
  --user doris \
  -- bash -c '/opt/doris/fe/bin/start_fe.sh --daemon \
               && /opt/doris/be/bin/start_be.sh --daemon \
               && wait'
```

> `dynamic.enabled=false` 时 runtime 只做“初始放置 + 静态保持”，跳过
> 图构建和 solver；`observe` 模式只记录不落盘，`plan` 只做 shadow。

### 2.4 验证放置生效

```sh
# runtime 日志：profile_load → profile_match（每个命中 tid）→ initial_affinity
grep -E '"type":"(profile_load|profile_match|initial_affinity)"' \
  /var/log/affinitygraph-doris/runtime.jsonl | tail -20

# status 里 active_effective 应为 true，且 collector_degraded=false
sudo -A ./build/affinityctl status --socket /tmp/affinitygraph-doris.sock
sudo -A ./build/affinityctl dump  --socket /tmp/affinitygraph-doris.sock
# dump 中查看目标 tid 的当前 mask 是否等于 profile 的 cpus（如 64-95）
```

确认 `active_effective=true` 后再开始 YCSB 正式测量。

### 2.5 结束与导出

```sh
# 先 pause 同步恢复掩码，再停 supervisor
sudo -A ./build/affinityctl pause --socket /tmp/affinitygraph-doris.sock
# 日志应出现 restore 记录
grep '"type":"restore' /var/log/affinitygraph-doris/runtime.jsonl | tail

# 正常退出（SIGTERM wrapper）后 runtime 导出 candidate 到 --profile-output
grep '"type":"profile_export"' /var/log/affinitygraph-doris/runtime.jsonl
```

## 3. 场景 B：无放置文件（动态 solver）

适用：冷启动探索，让 runtime 自己基于关系证据做放置。

### 3.1 observe（先看证据，不动手）

配置 `mode = "observe"`，按第 1 节启动（**不带** `--thread-profile`），跑
YCSB 负载。此时 runtime 只采样/建图/记录，不迁移任何线程：

```sh
grep -E '"type":"(solve_window_end|plan|family|domain)"' \
  /var/log/affinitygraph-doris/runtime.jsonl | tail -30
# observe 模式 solve 输出 outcome:"observed"，不会提交
```

### 3.2 plan（shadow 验证）

`mode = "plan"` 重启（或改配置后重启），观察 solver 的 shadow 决策：
`solve_window_end`/`plan` 记录会给出目标放置；`affinityctl dump` 中
`planned_threads` 不为 0 时表示有影子计划。此模式不执行任何 `sched_setaffinity`。

### 3.3 active（真正迁移）

`mode = "active"` 重启。runtime 会按 `incremental-hotspot-v1`
（默认 singleton_cpu 粒度）自动迁移热点线程：

```sh
# 看每次 action：迁移数量、committed、success
grep -E '"type":"(action|plan|solve_window_end|collector_health)"' \
  /var/log/affinitygraph-doris/runtime.jsonl | tail -40

sudo -A ./build/affinityctl status --socket /tmp/affinitygraph-doris.sock
sudo -A ./build/affinityctl dump  --socket /tmp/affinitygraph-doris.sock
```

- 需要 `affinity_capability=true`（保留 CAP_SYS_NICE），否则 active 无法
  迁移；`runtime_start` 日志里有该字段。
- 停止前同样先 `affinityctl pause`，然后确认 restore 日志：
  `"restore_requested"`/`"restore_restored"` 数量一致且 100%。

## 4. 结果与日志速查

| 文件/命令 | 内容 |
| --- | --- |
| `log_directory/runtime.jsonl` | 全量事件：runtime_start/topology_cpu/profile_*/thread_start/relation/plan/action/restore |
| `affinityctl status --socket ...` | effective_mode、collector_degraded、target_tgids、active_effective、planned_threads |
| `affinityctl dump --socket ...` | 每个受管 tid 的当前/计划掩码、family |
| `affinityctl pause/resume --socket ...` | 同步恢复/恢复运行 |

## 5. 常见问题

- **`bpf: fail (compiled CO-RE object is missing)`**：先 `make all` 生成
  `build/affinitygraph.bpf.o`，并确认 preflight 与 make 在同一目录执行（相对
  路径依赖 CWD），或改用绝对路径。
- **`cpu_envelope: fail`**：配置的信封超出了启动时 cgroup/进程亲和掩码；
  用 `--cpus` 或 `AFFINITY_CPUS` 收窄，或调整启动掩码。
- **`collector_degraded=true`**：BPF 未生效（未传 `--bpf-object`、非 root、
  libbpf 缺失等）；profile 场景必须修复。
- **profile 规则全部未命中**：确认 `comm` 与 `/proc/<pid>/task/<tid>/comm`
  完全一致（Doris 线程名如 `brpc_light`、`Pipe_normal`）；或改用
  `comm_prefix`/`cgroup_prefix`。
- **放置后线程不在目标 CPU**：先看 `profile_match` 是否 `empty_allowed_intersection`
  （信封与 `allowed_cpus` 交集为空），再看 `initial_affinity` 的
  `committed`/`success`。
- **暂停后未恢复**：`affinityctl pause` 应在停止 supervisor 前执行；异常退出
  时以 `restore_all` 兜底，检查 `runtime_stop` 的 restore 计数。
