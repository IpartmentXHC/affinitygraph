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
`brpc_light`/`Pipe_normal` → node2 CPU 集合），做复现/确认实验。以下命令
按顺序逐条执行（默认在 affinitygraph 仓库根目录、sudo 用 `-A`）。

### 2.1 前置确认

```sh
# 1) 构建产物与 BPF 对象就绪
ls -l build/affinity-run build/affinitygraph.bpf.o

# 2) 基础 preflight 通过（bpf: ok）
sudo -A ./build/affinity-run preflight \
  --config config/affinitygraph.toml \
  --bpf-object build/affinitygraph.bpf.o
```

### 2.2 确认拓扑，确定目标 CPU 集合

```sh
# 查看 CPU→NUMA node 映射，选定目标 node 的 CPU 列表（示例 node2 → 64-95）
lscpu -e=CPU,NODE,SOCKET | sort -k2 -n | less
# 或用 numactl（未安装则跳过）
numactl --hardware
```

### 2.3 停掉旧 Doris，准备目录

```sh
# affinity-run 必须自己拉起进程树，所以先停掉已在运行的 Doris
/opt/doris/fe/bin/stop_fe.sh || true
/opt/doris/be/bin/stop_be.sh || true

sudo -A mkdir -p /etc/affinitygraph/targets /etc/affinitygraph/profiles
sudo -A mkdir -p /var/log/affinitygraph-doris/profiles
```

### 2.4 写测试配置

```sh
sudo -A tee /etc/affinitygraph/targets/doris.toml >/dev/null <<'EOF'
[runtime]
mode = "active"                    # 静态放置只有在 active 才会真正落盘
dynamic = false                    # 只做静态放置，跳过 solver（无需改 profile）
sample_interval_seconds = 1        # 0 = 静止后停止采样（需 static，见下文说明）
graph_horizon_seconds = 60
solve_interval_seconds = 10
minimum_confidence = 0.8
proposal_confirmations = 3
solver = "incremental-hotspot-v1"  # 静态保持会跳过 solver，值不影响
static_quiescent_windows = 3       # sample=0 时连续无新命中 N 窗后停采样
affinity_granularity = "singleton_cpu"
maximum_managed_threads = 128
log_directory = "/var/log/affinitygraph-doris"
socket_path = "/tmp/affinitygraph-doris.sock"

[resources]
calibration_path = "/etc/affinitygraph/calibration"

[collector]
required = true
pthread_uprobe = true

[calibration]
id = "doris-manual-test"
activity_log_p95 = 1.0
sync_log_p95 = 1.0
share_log_p95 = 1.0
EOF
```

> `dynamic = false` 是静态开关：运行时强制静态，即使 profile 里
> `dynamic.enabled=true` 也会被覆盖，无需手工改 profile 文件。缺省（不写该
> 键）时仍由 profile 的 `dynamic.enabled` 决定。
>
> 采样频率：`sample_interval_seconds` 可调大（如 30~60s，静态模式下低频轮询，
> 新线程仍会被放置）；设 `0` 则持续采样直到 profile 连续
> `static_quiescent_windows`（默认 3）个窗口无新命中后彻底停止轮询
> （`sampling_stopped`，此时只保留 control socket）。`sample_interval_seconds=0`
> 仅静态模式合法，否则 preflight/启动会报 `sampling: fail`。

### 2.5 写放置文件 profile

```sh
sudo -A tee /etc/affinitygraph/profiles/doris-static.json >/dev/null <<'EOF'
{
  "schema_version": 1,
  "profile_id": "doris-manual-a-r1",
  "generated_at": "2026-08-17T00:00:00Z",
  "status": "candidate",
  "source": {"commit": "main", "experiment_id": "doris-manual-a", "test_id": "r1"},
  "applicability": {"description": "doris static placement r1"},
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
EOF
```

> `64-95` 只是示例，必须替换为 2.2 确认的本机 node CPU 列表。约束：
> `allowed_cpus` 与每个 `affinities[].cpus` 都必须在资源信封内（preflight
> 会拒绝越界）；`status` 只能是 `candidate`/`tested`；`profile_id`、
> `generated_at` 必填。

### 2.6 preflight 校验 profile

```sh
sudo -A ./build/affinity-run preflight \
  --config /etc/affinitygraph/targets/doris.toml \
  --thread-profile /etc/affinitygraph/profiles/doris-static.json \
  --bpf-object build/affinitygraph.bpf.o

# 期望输出：
#   config: ok
#   thread_profile: ok (2 placement rule(s), static)
#   cpu_envelope: ok (0-127)
#   kernel: ...
#   bpf: ok
#   pthread_uprobe: auto
```

### 2.7 启动（终端 1：保持前台运行）

```sh
sudo -A ./build/affinity-run run \
  --config /etc/affinitygraph/targets/doris.toml \
  --thread-profile /etc/affinitygraph/profiles/doris-static.json \
  --profile-output /var/log/affinitygraph-doris/profiles/doris-r1.candidate.json \
  --experiment-id doris-manual-a --test-id r1 \
  --bpf-object build/affinitygraph.bpf.o \
  --user doris \
  -- bash -c '/opt/doris/fe/bin/start_fe.sh --daemon \
               && /opt/doris/be/bin/start_be.sh --daemon \
               && wait'
```

`dynamic.enabled=false` 时 runtime 只做“初始放置 + 静态保持”，跳过图构建和
solver；wrapper 不退出，supervisor 就一直监督。

### 2.8 验证放置生效（终端 2）

```sh
# 等待 Doris FE/BE 起来并完成采样
sleep 20

# 1) runtime 日志三连：profile_load → profile_match → initial_affinity
sudo -A grep -E '"type":"(profile_load|profile_match|initial_affinity)"' \
  /var/log/affinitygraph-doris/runtime.jsonl | tail -30

# 期望：
#   profile_load  ... "success":true
#   profile_match ... "rule_id":"brpc-light" "target_cpus":"64-95"（每个命中线程一条）
#   initial_affinity ... "requested":N,"committed":N,"success":true

# 2) supervisor 健康（bpf 生效、未降级、未暂停）
sudo -A ./build/affinityctl status --socket /tmp/affinitygraph-doris.sock
# 关注 "bpf":true、"collector_degraded":false、"paused":false、"target_tgids" 覆盖 FE+BE

# 3) 抽查内核实际掩码（从 profile_match 里挑几个 tid）
TID=12345   # 替换为 profile_match 输出的某个 tid
sudo -A sh -c "grep Cpus_allowed_list /proc/$TID/status"
# 期望 Cpus_allowed_list: 64-95
```

> **注意**：静态 profile 下 `active_effective` 恒为 `false`（静态保持提前返回、
> `selector_ready` 不会置位，这是设计行为），不要用它作为测量门禁；以
> `initial_affinity success:true` + 实际掩码为准。

### 2.9 YCSB 测量（终端 3）

```sh
# 代表命令，按你的 YCSB 客户端/表结构调整；目标是 Doris FE 的 MySQL 端口
./bin/ycsb run jdbc -P workloads/workloada \
  -p jdbc.url="jdbc:mysql://127.0.0.1:9030/testdb" \
  -p jdbc.user=root -p jdbc.password='' \
  -p operationcount=1000000 -threads 32
```

测量期间可随时在终端 2 查看 `status`/日志确认放置未漂移。

### 2.10 结束实验并导出 candidate

```sh
# 1) 先 pause，同步恢复掩码
sudo -A ./build/affinityctl pause --socket /tmp/affinitygraph-doris.sock
# 输出中 "restore_requested":N,"restore_restored":N 应相等

# 2) 回到终端 1 按 Ctrl+C（supervisor 收到信号后转发给 wrapper，收尾退出）
#    或另开终端: sudo -A kill -TERM <affinity-run 的 pid>

# 3) 核对收尾日志：runtime_stop 的 restore 计数 + profile_export
sudo -A grep -E '"type":"(pause|runtime_stop|profile_export)"' \
  /var/log/affinitygraph-doris/runtime.jsonl | tail

# 4) 查看导出的 candidate（generated_at 已刷新为结束时间）
sudo -A head -8 /var/log/affinitygraph-doris/profiles/doris-r1.candidate.json
```

### 2.11 进阶：profile 初始放置 + active 动态微调

纯静态版（2.5 的 `dynamic.enabled=false`）会让 solver 直接
`profile_static_hold` 跳过。把 profile 顶层 `dynamic.enabled` 改为 `true`
后，profile 的初始放置照常落地（`initial_affinity`），solver 再在其之上做
增量调整 —— 即"初始方案 + 小范围动态调整"。

> 反方向强制静态不需要改 profile：在 `[runtime]` 写 `dynamic = false` 即可
> 覆盖 profile 里的 `dynamic.enabled=true`（2.4 的静态配置就是这么用的）。

注意两点现状：
- placement 级 `dynamic` 字段与 dynamic 节的
  `small_step_threads/large_change_ratio/large_step_threads/cooldown_seconds`
  目前只解析与导出，solver 尚未消费；
- `incremental-hotspot-v1` 不读 profile 的 `allowed_cpus`，理论上可把线程
  迁到信封内其他 node；实际"调整幅度"由 TOML 旋钮控制：
  `maximum_migrated_threads_ratio`（每窗口迁移比例）、
  `proposal_confirmations`（连续一致才执行）、`minimum_dwell_seconds`
  （迁移后驻留）、`maximum_threads_per_cpu`（每 CPU 线程上限）。

```sh
# 1) 复制 profile 并打开顶层 dynamic（sed 只命中顶层 "enabled"，不影响
#    placement 级 "dynamic": false）
sudo -A mkdir -p /etc/affinitygraph/profiles
sudo -A cp config/thread-profiles/doris-light-pipe-node2-c4t4.candidate.json \
  /etc/affinitygraph/profiles/doris-node2-dynamic.candidate.json
sudo -A sed -i 's/"enabled": false/"enabled": true/' \
  /etc/affinitygraph/profiles/doris-node2-dynamic.candidate.json
sudo -A grep -n '"enabled"' \
  /etc/affinitygraph/profiles/doris-node2-dynamic.candidate.json

# 2) 写配置：mode="active" + 收紧动态幅度的旋钮
sudo -A tee /etc/affinitygraph/targets/doris.toml >/dev/null <<'EOF'
[runtime]
mode = "active"
sample_interval_seconds = 1
graph_horizon_seconds = 60
solve_interval_seconds = 10
minimum_confidence = 0.8
proposal_confirmations = 3
initial_proposal_confirmations = 1
solver = "incremental-hotspot-v1"
affinity_granularity = "singleton_cpu"
maximum_managed_threads = 128
maximum_migrated_threads_ratio = 0.05   # 每窗口最多动 5% 受管线程
minimum_dwell_seconds = 60              # 迁移后至少驻留 60s
maximum_threads_per_cpu = 4             # 每 CPU 线程上限（按需）
log_directory = "/var/log/affinitygraph-doris"
socket_path = "/tmp/affinitygraph-doris.sock"

[resources]
calibration_path = "/etc/affinitygraph/calibration"

[collector]
required = true
pthread_uprobe = true

[calibration]
id = "doris-manual-test"
activity_log_p95 = 1.0
sync_log_p95 = 1.0
share_log_p95 = 1.0
EOF

# 3) preflight：期望 thread_profile: ok (2 placement rule(s), dynamic)
sudo -A ./build/affinity-run preflight \
  --config /etc/affinitygraph/targets/doris.toml \
  --thread-profile /etc/affinitygraph/profiles/doris-node2-dynamic.candidate.json \
  --bpf-object build/affinitygraph.bpf.o

# 4) 启动（终端 1）
sudo -A ./build/affinity-run run \
  --config /etc/affinitygraph/targets/doris.toml \
  --thread-profile /etc/affinitygraph/profiles/doris-node2-dynamic.candidate.json \
  --profile-output /etc/affinitygraph/profiles/doris-node2-dynamic.export.json \
  --experiment-id 20260817-doris-profile-dynamic \
  --test-id hybrid-active-v1 \
  --bpf-object build/affinitygraph.bpf.o \
  -- bash -c '/opt/doris/fe/bin/start_fe.sh --daemon \
               && /opt/doris/be/bin/start_be.sh --daemon \
               && wait'

# 5) 终端 3：YCSB 负载（同 2.9）

# 6) 终端 2：验证"先初始放置、后动态微调"
sleep 30
sudo -A grep -E '"type":"(profile_load|profile_match|initial_affinity|plan|action|action_commit|solve_window_end)"' \
  /var/log/affinitygraph-doris/runtime.jsonl | tail -60
# 期望：profile_match → initial_affinity committed（初始放置）；
# 之后 plan confirmation 递增 → ready，solve_window_end outcome:"action_committed"
# （动态微调，迁移量受 maximum_migrated_threads_ratio 限制）

sudo -A ./build/affinityctl status --socket /tmp/affinitygraph-doris.sock
# dynamic.enabled=true 时 selector_ready 会置位，期望 active_effective:true
```

结束实验与导出同 2.10。

### 2.12 场景 A 排障

- `profile_load "success":false`：profile 路径/信封/status 非法，看 `error` 字段。
- `profile_match "outcome":"empty_allowed_intersection"`：该线程的
  `allowed_cpus` 与 profile `cpus` 交集为空，检查信封与 cgroup 掩码。
- `initial_affinity "success":false`：看 `rollback_success` 与
  `runtime_start` 的 `affinity_capability`（active 需要保留 CAP_SYS_NICE）。
- `collector_degraded=true`：BPF 未生效，回到 2.1/2.6 检查。

## 3. 场景 B：无放置文件（动态 solver）

适用：冷启动探索，让 runtime 自己基于关系证据做放置。
按 observe → plan → active 三段推进，每次切换**都要重启 supervisor**（
affinity-run 必须自己拉起 Doris）。注意：Ctrl+C 停掉 supervisor 后，
Doris 的 FE/BE daemon（`setsid` 脱离进程组）**仍然存活**，切换模式前要
手动 `stop_fe.sh`/`stop_be.sh`。

> 真实环境不需要三段推进：`mode = "active"` 内部已经包含采样 → 计算 →
> 执行 的自动闭环（见 3.6）。observe/plan 只用于离线验证与干跑，正式
> 部署直接跳到 3.6。

### 3.1 前置确认与配置（observe 起步）

```sh
# 1) 构建产物与 BPF 就绪（同场景 A 2.1）
ls -l build/affinity-run build/affinitygraph.bpf.o
sudo -A ./build/affinity-run preflight \
  --config config/affinitygraph.toml \
  --bpf-object build/affinitygraph.bpf.o

# 2) 停掉旧 Doris（如正在运行）
/opt/doris/fe/bin/stop_fe.sh || true
/opt/doris/be/bin/stop_be.sh || true

sudo -A mkdir -p /etc/affinitygraph/targets
sudo -A mkdir -p /var/log/affinitygraph-doris
```

```sh
sudo -A tee /etc/affinitygraph/targets/doris.toml >/dev/null <<'EOF'
[runtime]
mode = "observe"                   # 先 observe；后续手动改为 plan / active
sample_interval_seconds = 1
graph_horizon_seconds = 60
solve_interval_seconds = 10
minimum_confidence = 0.8
proposal_confirmations = 3
solver = "incremental-hotspot-v1"
affinity_granularity = "singleton_cpu"
maximum_managed_threads = 128      # Doris 线程多时按需调大
log_directory = "/var/log/affinitygraph-doris"
socket_path = "/tmp/affinitygraph-doris.sock"

[resources]
calibration_path = "/etc/affinitygraph/calibration"

[collector]
required = true
pthread_uprobe = true

[calibration]
# 关系证据归一化会用到这些尺度（graph.cpp），正式实验请替换为
# 经评审的离线尺度，不要沿用 ClickHouse 默认值
id = "doris-manual-test"
activity_log_p95 = 1.0
sync_log_p95 = 1.0
share_log_p95 = 1.0
EOF
```

### 3.2 启动 observe（终端 1，**不带** --thread-profile）

```sh
sudo -A ./build/affinity-run run \
  --config /etc/affinitygraph/targets/doris.toml \
  --bpf-object build/affinitygraph.bpf.o \
  --user doris \
  -- bash -c '/opt/doris/fe/bin/start_fe.sh --daemon \
               && /opt/doris/be/bin/start_be.sh --daemon \
               && wait'
```

### 3.3 观察证据（终端 2 + 终端 3 打 YCSB 负载）

```sh
# 终端 3：起 YCSB 负载（同场景 A 2.9）
./bin/ycsb run jdbc -P workloads/workloada \
  -p jdbc.url="jdbc:mysql://127.0.0.1:9030/testdb" \
  -p jdbc.user=root -p jdbc.password='' \
  -p operationcount=1000000 -threads 32
```

```sh
# 终端 2：等待窗口建立，观察关系证据
sleep 30
sudo -A grep -E '"type":"(solve_window_begin|selector_input|selector_output|relation_edge_summary|solve_window_end)"' \
  /var/log/affinitygraph-doris/runtime.jsonl | tail -40
# observe 模式 solve_window_end 应输出 "outcome":"observed"，不提交任何放置

sudo -A ./build/affinityctl status --socket /tmp/affinitygraph-doris.sock
# 关注 "bpf":true、"collector_degraded":false、"target_tgids" 覆盖 FE+BE
```

### 3.4 切换 plan（shadow 验证，不执行迁移）

```sh
# 1) 终端 1 按 Ctrl+C 停 supervisor，然后停 Doris
/opt/doris/fe/bin/stop_fe.sh || true
/opt/doris/be/bin/stop_be.sh || true

# 2) 改 mode
sudo -A sed -i 's/^mode = .*/mode = "plan"/' /etc/affinitygraph/targets/doris.toml
sudo -A grep '^mode' /etc/affinitygraph/targets/doris.toml

# 3) 终端 1 重新启动（同 3.2 命令），终端 3 重新打负载
```

```sh
# 终端 2：观察 shadow 决策
sleep 30
sudo -A grep -E '"type":"(plan|shadow_commit|solve_window_end)"' \
  /var/log/affinitygraph-doris/runtime.jsonl | tail -40
# plan 模式期望 outcome:"planned" / "shadow_committed"，无 action 日志

sudo -A ./build/affinityctl status --socket /tmp/affinitygraph-doris.sock
# "planned_assignments"/"planned_masks" 非空 = 有影子计划；
# action_requested/action_committed 应保持 0
```

### 3.5 切换 active（真正迁移）

```sh
# 1) 终端 1 Ctrl+C → 停 Doris → 改 mode
/opt/doris/fe/bin/stop_fe.sh || true
/opt/doris/be/bin/stop_be.sh || true
sudo -A sed -i 's/^mode = .*/mode = "active"/' /etc/affinitygraph/targets/doris.toml
sudo -A grep '^mode' /etc/affinitygraph/targets/doris.toml

# 2) 终端 1 重新启动（同 3.2 命令），终端 3 重新打负载
```

```sh
# 终端 2：等待策略武装并观察迁移动作
sleep 30
sudo -A grep -E '"type":"(action|action_commit|actuator_output|solve_window_end)"' \
  /var/log/affinitygraph-doris/runtime.jsonl | tail -40
# active 期望 solve_window_end outcome:"action_committed"，action/action_commit 记录迁移

sudo -A ./build/affinityctl status --socket /tmp/affinitygraph-doris.sock
# 期望 "policy_armed":true、"active_effective":true（需 selector_ready 且 BPF 健康
# 窗口 loss<1%）；"pinned_threads":N、"action_requested"=="action_committed"
```

```sh
# 抽查内核实际掩码（从 action/plan 日志挑一个受管 tid）
TID=12345
sudo -A sh -c "grep Cpus_allowed_list /proc/$TID/status"
```

### 3.6 一步到位 active（真实环境，推荐）

直接从 active 起步：runtime 每 `sample_interval_seconds`（默认 1s）采样，
每 `solve_interval_seconds`（默认 10s）计算一次方案并执行。前 1~3 分钟是
证据积累期（`graph_horizon_seconds`=60s + `proposal_confirmations`=3），
`plan` 日志 `confirmation` 连续达成后才出现 `outcome:"action_committed"`。

```sh
# 0) 构建产物与 BPF 就绪（若已 make all 可跳过）
cd /data/kunpeng-affinity/affinitygraph
make all CXX=/usr/bin/clang++-18 CLANG=/usr/bin/clang-18
ls -l build/affinity-run build/affinitygraph.bpf.o

# 1) 停掉旧 Doris，建目录
/opt/doris/fe/bin/stop_fe.sh || true
/opt/doris/be/bin/stop_be.sh || true
sudo -A mkdir -p /etc/affinitygraph/targets /var/log/affinitygraph-doris
```

```sh
# 2) 写配置：mode 直接为 active（动态 solver）
sudo -A tee /etc/affinitygraph/targets/doris.toml >/dev/null <<'EOF'
[runtime]
mode = "active"                    # 采样+计算+执行 一步到位
sample_interval_seconds = 1
graph_horizon_seconds = 60
solve_interval_seconds = 10
minimum_confidence = 0.8
proposal_confirmations = 3
initial_proposal_confirmations = 1
solver = "incremental-hotspot-v1"
affinity_granularity = "singleton_cpu"
maximum_managed_threads = 128      # Doris 线程多时按需调大
active_demand_threshold = 0.05
inactive_demand_threshold = 0.0
log_directory = "/var/log/affinitygraph-doris"
socket_path = "/tmp/affinitygraph-doris.sock"

[resources]
calibration_path = "/etc/affinitygraph/calibration"

[collector]
required = true
pthread_uprobe = true

[calibration]
id = "doris-manual-test"
activity_log_p95 = 1.0
sync_log_p95 = 1.0
share_log_p95 = 1.0
EOF
sudo -A grep -E '^(mode|solve_interval_seconds|maximum_managed_threads)' \
  /etc/affinitygraph/targets/doris.toml
```

```sh
# 3) preflight 校验（期望 config/cpu_envelope/kernel/bpf 全 ok）
sudo -A ./build/affinity-run preflight \
  --config /etc/affinitygraph/targets/doris.toml \
  --bpf-object build/affinitygraph.bpf.o
```

```sh
# 4) 终端 1：启动 active（不带 --thread-profile，让动态 solver 自己放）
sudo -A ./build/affinity-run run \
  --config /etc/affinitygraph/targets/doris.toml \
  --bpf-object build/affinitygraph.bpf.o \
  --user doris \
  -- bash -c '/opt/doris/fe/bin/start_fe.sh --daemon \
               && /opt/doris/be/bin/start_be.sh --daemon \
               && wait'
```

```sh
# 5) 终端 3：起 YCSB 负载（正式测量，持续打）
./bin/ycsb run jdbc -P workloads/workloada \
  -p jdbc.url="jdbc:mysql://127.0.0.1:9030/testdb" \
  -p jdbc.user=root -p jdbc.password='' \
  -p operationcount=1000000 -threads 32
```

```sh
# 6) 终端 2：等 1~3 分钟证据积累，观察自动闭环
sleep 30
sudo -A ./build/affinityctl status --socket /tmp/affinitygraph-doris.sock
# 期望 effective_mode:"active"、bpf:true、collector_degraded:false、
# 证据足够后 active_effective:true、planned_threads 非空

sudo -A grep -E '"type":"(solve_window_begin|plan|action|action_commit|active_cohort|solve_window_end)"' \
  /var/log/affinitygraph-doris/runtime.jsonl | tail -60
# 期望 plan 日志 confirmation 递增 → 1，然后 solve_window_end
# outcome:"action_committed"；action/action_commit 记录每次迁移
```

```sh
# 7) 抽查内核实际掩码（从 action/plan 日志挑一个受管 tid）
TID=12345
sudo -A sh -c "grep Cpus_allowed_list /proc/$TID/status"
```

### 3.7 结束实验

```sh
# 1) 先 pause，同步恢复全部掩码
sudo -A ./build/affinityctl pause --socket /tmp/affinitygraph-doris.sock
# "restore_requested":N,"restore_restored":N 应相等

# 2) 终端 1 Ctrl+C 收尾
# 3) 核对 runtime_stop 的 restore 计数
sudo -A grep -E '"type":"(pause|runtime_stop)"' \
  /var/log/affinitygraph-doris/runtime.jsonl | tail

# 4) 停掉未被监督的 Doris daemon（可选，按实验需要）
/opt/doris/fe/bin/stop_fe.sh || true
/opt/doris/be/bin/stop_be.sh || true
```

### 3.8 场景 B 排障

- `solve_window_end outcome:"waiting_bpf_health"`：BPF 健康窗口未就绪
  （loss 需 <1%），等 `bpf_window_ready:true` 再看。
- `policy_armed/active_effective` 一直 false：检查 `selector_ready`、
  `paused`、`fatal_error`、`affinity_capability`（active 需保留 CAP_SYS_NICE）。
- `outcome:"action_failed"`/`"actions_vanished"`：迁移失败/线程已消失，看
  `actuator_output` 与 `action` 日志的错误字段。
- 迁移数量为 0：`maximum_managed_threads` 太小或 demand 未达
  `active_demand_threshold`，看 `active_cohort` 日志。

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
- **stderr 出现 `libbpf: elf: ambiguous match for 'pthread_create'`**：glibc
  ≥ 2.34 合并 libpthread 后 `pthread_create` 在 `libc.so.6` 里有多个版本别名，
  libbpf 按符号名挂 uprobe 会解析失败。已改为 dlsym 解析地址 + ELF offset
  挂载（`src/affinity_run.cpp`），重新 `make all` 后 `status` 的
  `pthread_uprobe` 应显示 `attached:... (resolved offset)`；旧的
  `(symbol name)` 表示回退到了符号名路径，需检查 libc 路径一致性。
- **preflight 报 `sampling: fail`**：`sample_interval_seconds=0` 要求静态
  profile（`runtime.dynamic=false` 或 profile `dynamic.enabled=false`），
  否则无法在静止后停止采样；去掉该键或改成低频值（如 30）即可。
- **`status` 里 `sampling_stopped:true`**：静态 + `sample_interval_seconds=0`
  时，profile 连续 `static_quiescent_windows` 个窗口无新命中后停止轮询
  （日志出现 `sampling_stopped` 事件）。这是终态：`resume` 不会恢复采样，
  需要新线程被放置时请用低频值或重启进程。
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
