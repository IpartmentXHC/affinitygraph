# ClickHouse 手动测试教程（不使用 YBA）

面向**手动启动 ClickHouse + YCSB** 的压测场景：不用 YBA 编排，直接用
`affinity-run` 监督 ClickHouse 进程，用 YCSB 打负载。覆盖四个场景：

| 场景 | profile | 调度方式 | 预期行为 |
| --- | --- | --- | --- |
| 0 | 无 | baseline | 不启动 affinitygraph，直接启动数据库测基线吞吐（对照） |
| 1 | 无 | 动态 | runtime 自行采样、决策并迁移线程（`mode=active` 一步闭环） |
| 2 | 有 | 动态 | profile 先做初始放置，solver 再在其上增量微调 |
| 3 | 有 | 静态 | profile 放置后保持不动（`dynamic=false` + 可 `sample_interval_seconds=0` 停止采样） |

文中命令以 183 上的默认路径为准（`/home/xhc/...`、`/etc/affinitygraph/...`）；
换机器时替换成你的实际路径。四场景也可用 `tests/manual-scenarios.sh --db
clickhouse` 自动跑（见第 6 节）。Doris 版教程见 `docs/doris-manual-testing.md`，
两文结构相同，本章只强调 ClickHouse 差异。

## 0. 前置准备（四种场景通用）

```sh
# 1) 构建（make all 已包含用户态程序 + BPF 对象 build/affinitygraph.bpf.o）
cd ~/affinitygraph
make all CXX=/usr/bin/clang++-18 CLANG=/usr/bin/clang-18

# 2) 校准（每台机器一次，实测延迟；无实测数据也能生成，is_estimated 标记启发值）
sudo -A make calibrate
# 或把人工审核过的 CSV 放到 /etc/affinitygraph/calibration/hardware-node-edges.csv
# 183 上 ClickHouse 已冻结实测尺度 clickhouse-gate2-fixed-v2（见 1 节）

# 3) 检查内核/工具链/校准/BPF 是否就绪
sudo -A ./build/affinity-run preflight --config config/affinitygraph.toml \
  --bpf-object build/affinitygraph.bpf.o
# 期望输出 config: ok / cpu_envelope: ok / kernel / bpf: ok / pthread_uprobe: auto
```

要点：

- **CPU 信封优先级**：`--cpus` > `AFFINITY_CPUS` > TOML `resources.cpus` >
  `sched_getaffinity()`；信封必须落在启动时进程/cgroup 亲和掩码内，否则
  preflight 报 `cpu_envelope: fail`。
- **profile 路径优先级**：`--thread-profile` > `AFFINITY_THREAD_PROFILE` >
  TOML `runtime.thread_profile`。
- **默认以 `--user root` 监督**：`affinity-run --user root` 把 ClickHouse
  降权为 root 运行。**ClickHouse 要求进程用户与数据目录属主一致**：数据为
  root 属主时用默认（`CLICKHOUSE_RUN_USER=root`）；数据为 xhc 等非 root
  属主时，设 `CLICKHOUSE_RUN_USER=xhc` 或置空，否则报 Code: 430。
- **BPF 必须 ok**：`collector.required=true` 下 `bpf: fail` 会直接拒绝启动。

## 1. 拓扑与关键参数（183）

```sh
# CPU→NUMA node 映射（183：node2=64-95，node3=96-127）
lscpu -e=CPU,NODE,SOCKET | sort -k2 -n | less
# 本教程目标集合：node2+3 → 64-127
```

- ClickHouse MySQL 协议端口：9004（YCSB JDBC 连接用，user=default）
- 已冻结的 ClickHouse 校准尺度（`config/affinitygraph.toml` 同源）：

```toml
[calibration]
id = "clickhouse-gate2-fixed-v2"
activity_log_p95 = 2.4138804290562152
sync_log_p95 = 2.5591179487485345
share_log_p95 = 0.00894730347830295
```

- 参考静态 profile（`config/thread-profiles/clickhouse-threadpool-node2plus3.candidate.json`）：
  `clickhouse`×6、`ThreadPool`×38、`Common`×8 → `64-127`
- 三个场景各自独立的 socket/日志目录，互不干扰：
  `/tmp/affinitygraph-clickhouse-s{1,2,3}.sock`、
  `/var/log/affinitygraph-clickhouse-s{1,2,3}/runtime.jsonl`

## 2. 场景 0：baseline（对照基线）

不启动 affinitygraph，直接启动 ClickHouse，测量无干预的基线吞吐，作为
场景 1/2/3 的对照（脚本 `tests/manual-scenarios.sh --db clickhouse` 默认
包含该场景）。

```sh
# 直接启动 ClickHouse（等价于脚本场景 0 的命令；数据为 root 属主时用此命令）
sudo -A nohup /home/xhc/clickhouse/ClickHouse/build/programs/clickhouse server \
  --config-file /home/xhc/clickhouse/etc/config.xml \
  > /tmp/affinity-clickhouse-baseline.log 2>&1 &

# 等待 9004 端口就绪
until timeout 2 bash -c 'exec 3<>/dev/tcp/127.0.0.1/9004' 2>/dev/null; do sleep 5; done

# 然后执行 YCSB 测量（命令同 3.5 节），记录 baseline 吞吐

# 结束：停掉 ClickHouse
sudo -A pkill -x clickhouse; sudo -A pkill -x clckhouse-watch
```

- 运行用户：默认以 root 启动；若数据目录非 root 属主（如 183 的 xhc），脚本
  会按 `CLICKHOUSE_RUN_USER`（如 `CLICKHOUSE_RUN_USER=xhc`）经
  `runuser -u xhc` 启动，否则 ClickHouse 报 Code: 430。
- baseline 没有 runtime.jsonl / affinityctl，就绪判定是 9004 端口可连。

## 3. 场景 1：无 profile 文件的动态调度

让 runtime 全自动：采样 → 构建关系证据 → solver 决策 → 迁移线程。适用于没有
先验放置方案、想先看动态优化收益的场景。

### 3.1 写测试配置

```sh
sudo -A mkdir -p /etc/affinitygraph/targets
sudo -A tee /etc/affinitygraph/targets/clickhouse-s1.toml >/dev/null <<'EOF'
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
maximum_migrated_threads_ratio = 0.05
minimum_dwell_seconds = 60
maximum_threads_per_cpu = 4
log_directory = "/var/log/affinitygraph-clickhouse-s1"
socket_path = "/tmp/affinitygraph-clickhouse-s1.sock"

[resources]
calibration_path = "/etc/affinitygraph/calibration"

[collector]
required = true
pthread_uprobe = true

[calibration]
id = "clickhouse-gate2-fixed-v2"
activity_log_p95 = 2.4138804290562152
sync_log_p95 = 2.5591179487485345
share_log_p95 = 0.00894730347830295
EOF
```

> `mode="active"` 内部已包含“观察→计划→执行”的自动闭环。`dynamic` 不写
> （默认开启）＝solver 每 `solve_interval_seconds` 决策一次。
> `[calibration]` 使用冻结的 ClickHouse 实测尺度；换机器/负载时需重新
> 校准，不要沿用。

### 3.2 preflight 校验

```sh
sudo -A ./build/affinity-run preflight \
  --config /etc/affinitygraph/targets/clickhouse-s1.toml \
  --bpf-object build/affinitygraph.bpf.o
# 期望 thread_profile 行不出现（无 profile）；其余 ok
```

### 3.3 启动（后台运行）

```sh
cd ~/affinitygraph
sudo -A nohup ./build/affinity-run run \
  --config /etc/affinitygraph/targets/clickhouse-s1.toml \
  --bpf-object build/affinitygraph.bpf.o \
  -- /home/xhc/clickhouse/ClickHouse/build/programs/clickhouse server \
     --config-file /home/xhc/clickhouse/etc/config.xml \
  > /tmp/affinity-clickhouse-s1.log 2>&1 &
```

要点：

- 运行用户：默认 `--user root`（脚本用 `CLICKHOUSE_RUN_USER` 控制，默认
  root）；若数据目录非 root 属主，设 `CLICKHOUSE_RUN_USER=xhc` 或置空，否则
  ClickHouse 报 Code: 430。
- `clickhouse server` 前台运行，`affinity-run` 一直监督；进程退出时 runtime
  执行 restore 并退出。
- 也可在前台终端运行（去掉 `nohup ... &`），方便 Ctrl+C 收尾。

### 3.4 验证调度闭环

```sh
# 等 ClickHouse 起来并完成至少一个决策窗口（约 60~90s）
sleep 90

# 1) 日志：runtime_start → solve_window_end（outcome 是动态决策，不是 profile_static_hold）
sudo -A grep -E '"type":"(runtime_start|solve_window_end)"' \
  /var/log/affinitygraph-clickhouse-s1/runtime.jsonl | tail -10

# 2) status：active_effective=true 表示 solver 已就绪并正在执行方案
sudo -A ./build/affinityctl status --socket /tmp/affinitygraph-clickhouse-s1.sock | python3 -c '
import sys, json
d = json.load(sys.stdin)
for k in ("effective_mode","bpf","collector_degraded","target_tgids","threads",
          "pinned_threads","selector_ready","policy_armed","active_effective","solver_phase","paused"):
    print(f"{k}: {d.get(k)}")'
# 期望 pinned_threads > 0，active_effective=true，collector_degraded=false

# 3) 看每个窗口的实际决策
sudo -A grep '"type":"solve_window_end"' /var/log/affinitygraph-clickhouse-s1/runtime.jsonl | tail -5
```

> 场景 1 的迁移可能较小（初始无先验，solver 需积累证据、受
> `minimum_confidence` / `proposal_confirmations` 约束）。要看收益，给足
> 时长（至少几个 `graph_horizon_seconds`）再压测。

### 3.5 YCSB 测量（另一终端，197 客户端）

```sh
# 197 客户端；workloada_clickhouse_numa_read 为纯读，连接 183:9004
# （conf/db_183_clickhouse.properties 已内置 jdbc:mysql://192.168.70.183:9004/ycsb）
cd /home/xhc/ycsb-jdbc-binding-0.17.0
python2 bin/ycsb run jdbc -s \
  -P workloads/workloada_clickhouse_numa_read \
  -P conf/db_183_clickhouse.properties \
  -cp lib/mysql-connector-java-8.0.28.jar \
  -p table=usertable -p threads=2 \
  -p operationcount=8000 -p status.interval=10
# 预热建议：2 个客户端 × 2 线程跑 60s 后再正式测量
# 结果取 [OVERALL] Throughput(ops/sec)
```

### 3.6 停止场景 1

```sh
# 1) pause：同步恢复被迁移线程的掩码
sudo -A ./build/affinityctl pause --socket /tmp/affinitygraph-clickhouse-s1.sock

# 2) 停 supervisor（clickhouse server 随之退出，runtime 收尾 restore）
sudo -A pkill -TERM -x affinity-run

# 3) 核对收尾日志
sudo -A grep -E '"type":"(pause|runtime_stop)"' \
  /var/log/affinitygraph-clickhouse-s1/runtime.jsonl | tail

# 4) 清理 ClickHouse 进程（如需；注意进程名被内核截断为 clckhouse-watch）
sudo -A pkill -x clickhouse; sudo -A pkill -x clckhouse-watch
```

## 4. 场景 2：有 profile 文件的动态调度

先用已验证的放置画像做**初始放置**，再由 solver 在其上做**小范围动态微调**
——即“初始方案 + 增量调整”。

### 4.1 准备动态 profile（复制 + 打开顶层 dynamic）

```sh
# 参考 profile 顶层 dynamic.enabled=false（静态）
sudo -A grep -n '"enabled"' /etc/affinitygraph/profiles/clickhouse-threadpool-node2plus3.candidate.json | head -1

# 生成场景 2 专用副本（顶层 dynamic 打开为 true，placement 级不动）
sudo -A mkdir -p /etc/affinitygraph/profiles
sudo -A python3 - <<'PY'
import json
src = "/etc/affinitygraph/profiles/clickhouse-threadpool-node2plus3.candidate.json"
dst = "/etc/affinitygraph/profiles/clickhouse-s2.dynamic.json"
d = json.load(open(src))
d["dynamic"]["enabled"] = True
d["profile_id"] = d.get("profile_id", "dynamic") + "-s2-dynamic"
open(dst, "w").write(json.dumps(d, indent=2, ensure_ascii=False) + "\n")
PY

# 确认顶层 dynamic 已打开
sudo -A grep -n '"enabled"' /etc/affinitygraph/profiles/clickhouse-s2.dynamic.json | head -1
```

### 4.2 写测试配置

```sh
sudo -A tee /etc/affinitygraph/targets/clickhouse-s2.toml >/dev/null <<'EOF'
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
maximum_migrated_threads_ratio = 0.05
minimum_dwell_seconds = 60
maximum_threads_per_cpu = 4
log_directory = "/var/log/affinitygraph-clickhouse-s2"
socket_path = "/tmp/affinitygraph-clickhouse-s2.sock"

[resources]
calibration_path = "/etc/affinitygraph/calibration"

[collector]
required = true
pthread_uprobe = true

[calibration]
id = "clickhouse-gate2-fixed-v2"
activity_log_p95 = 2.4138804290562152
sync_log_p95 = 2.5591179487485345
share_log_p95 = 0.00894730347830295
EOF
```

### 4.3 preflight 校验 profile

```sh
sudo -A ./build/affinity-run preflight \
  --config /etc/affinitygraph/targets/clickhouse-s2.toml \
  --thread-profile /etc/affinitygraph/profiles/clickhouse-s2.dynamic.json \
  --bpf-object build/affinitygraph.bpf.o
# 期望 thread_profile: ok (3 placement rule(s), dynamic)
```

### 4.4 启动

```sh
cd ~/affinitygraph
sudo -A nohup ./build/affinity-run run \
  --config /etc/affinitygraph/targets/clickhouse-s2.toml \
  --thread-profile /etc/affinitygraph/profiles/clickhouse-s2.dynamic.json \
  --bpf-object build/affinitygraph.bpf.o \
  -- /home/xhc/clickhouse/ClickHouse/build/programs/clickhouse server \
     --config-file /home/xhc/clickhouse/etc/config.xml \
  > /tmp/affinity-clickhouse-s2.log 2>&1 &
```

### 4.5 验证：初始放置 + 动态微调

```sh
# 等 ClickHouse 起来并完成初始放置 + 至少一个决策窗口
sleep 90

# 1) 三连：profile_load(success) → profile_match(逐线程命中) → initial_affinity(committed≥1)
sudo -A grep -E '"type":"(profile_load|profile_match|initial_affinity)"' \
  /var/log/affinitygraph-clickhouse-s2/runtime.jsonl | tail -15

# 2) 决策窗口 outcome（应出现动态 outcome，不是 profile_static_hold）
sudo -A grep '"type":"solve_window_end"' /var/log/affinitygraph-clickhouse-s2/runtime.jsonl | tail -5

# 3) 抽查被放置线程的实际掩码（从 profile_match 挑一个 tid）
TID=$(sudo -A grep '"type":"profile_match"' /var/log/affinitygraph-clickhouse-s2/runtime.jsonl | tail -1 | sed -E 's/.*"tid":([0-9]+).*/\1/')
sudo -A sh -c "grep Cpus_allowed_list /proc/$TID/status"
# 期望 Cpus_allowed_list: 64-127

# 4) status 关键字段
sudo -A ./build/affinityctl status --socket /tmp/affinitygraph-clickhouse-s2.sock | python3 -c '
import sys, json
d = json.load(sys.stdin)
for k in ("effective_mode","target_tgids","threads","pinned_threads",
          "active_effective","solver_phase","paused"):
    print(f"{k}: {d.get(k)}")'
```

### 4.6 YCSB 测量 + 停止

同 3.5（socket/日志换成 `clickhouse-s2`），测量结束后：

```sh
sudo -A ./build/affinityctl pause --socket /tmp/affinitygraph-clickhouse-s2.sock
sudo -A pkill -TERM -x affinity-run
sudo -A grep -E '"type":"(pause|runtime_stop)"' \
  /var/log/affinitygraph-clickhouse-s2/runtime.jsonl | tail
sudo -A pkill -x clickhouse; sudo -A pkill -x clckhouse-watch
```

## 5. 场景 3：有 profile 文件的静态调度

profile 放置后**保持不动**，跳过 solver。183 实测表明静态模式下可以
`sample_interval_seconds=0`：持续采样到 profile 连续
`static_quiescent_windows` 个窗口无新命中后彻底停止轮询（`sampling_stopped`），
采样开销趋近于零。

### 5.1 写测试配置（静态开关 + 零采样）

```sh
sudo -A tee /etc/affinitygraph/targets/clickhouse-s3.toml >/dev/null <<'EOF'
[runtime]
mode = "active"
dynamic = false
sample_interval_seconds = 0
static_quiescent_windows = 60
graph_horizon_seconds = 60
solve_interval_seconds = 10
minimum_confidence = 0.8
proposal_confirmations = 3
initial_proposal_confirmations = 1
solver = "incremental-hotspot-v1"
affinity_granularity = "singleton_cpu"
maximum_managed_threads = 128
maximum_migrated_threads_ratio = 0.05
minimum_dwell_seconds = 60
maximum_threads_per_cpu = 4
log_directory = "/var/log/affinitygraph-clickhouse-s3"
socket_path = "/tmp/affinitygraph-clickhouse-s3.sock"

[resources]
calibration_path = "/etc/affinitygraph/calibration"

[collector]
required = true
pthread_uprobe = true

[calibration]
id = "clickhouse-gate2-fixed-v2"
activity_log_p95 = 2.4138804290562152
sync_log_p95 = 2.5591179487485345
share_log_p95 = 0.00894730347830295
EOF
```

> - `dynamic = false` 是**运行时静态开关**：即使 profile 里
>   `dynamic.enabled=true` 也会被覆盖；不写该键时由 profile 的
>   `dynamic.enabled` 决定。
> - `sample_interval_seconds=0` **仅静态模式合法**（动态模式会
>   `sampling: fail`）；`static_quiescent_windows=60` 表示连续 60 个窗口
>   无新命中后停采样（ClickHouse 线程稳定比 Doris 快，183 实测 60 足够；
>   Doris 版用 180）。
> - 想保留低频轮询（新线程仍会被放置）可设 `sample_interval_seconds=30~60`
>   并去掉 quiescent 逻辑。

### 5.2 preflight

```sh
sudo -A ./build/affinity-run preflight \
  --config /etc/affinitygraph/targets/clickhouse-s3.toml \
  --thread-profile /etc/affinitygraph/profiles/clickhouse-threadpool-node2plus3.candidate.json \
  --bpf-object build/affinitygraph.bpf.o
# 期望 thread_profile: ok (3 placement rule(s), static)
```

### 5.3 启动

```sh
cd ~/affinitygraph
sudo -A nohup ./build/affinity-run run \
  --config /etc/affinitygraph/targets/clickhouse-s3.toml \
  --thread-profile /etc/affinitygraph/profiles/clickhouse-threadpool-node2plus3.candidate.json \
  --bpf-object build/affinitygraph.bpf.o \
  -- /home/xhc/clickhouse/ClickHouse/build/programs/clickhouse server \
     --config-file /home/xhc/clickhouse/etc/config.xml \
  > /tmp/affinity-clickhouse-s3.log 2>&1 &
```

### 5.4 验证静态保持 + 采样停止

```sh
# 等初始放置完成并进入静态保持（ClickHouse 建议 2~3 分钟）
sleep 180

# 1) 三连 + 决策窗口 outcome=profile_static_hold
sudo -A grep -E '"type":"(profile_load|initial_affinity)"' \
  /var/log/affinitygraph-clickhouse-s3/runtime.jsonl | tail -5
sudo -A grep '"type":"solve_window_end"' /var/log/affinitygraph-clickhouse-s3/runtime.jsonl | tail -3
# 期望 outcome 均为 "profile_static_hold"

# 2) 采样停止事件
sudo -A grep '"type":"sampling_stopped"' /var/log/affinitygraph-clickhouse-s3/runtime.jsonl | tail -2

# 3) 抽查掩码（profile_match 最近 tid）
TID=$(sudo -A grep '"type":"profile_match"' /var/log/affinitygraph-clickhouse-s3/runtime.jsonl | tail -1 | sed -E 's/.*"tid":([0-9]+).*/\1/')
sudo -A sh -c "grep Cpus_allowed_list /proc/$TID/status"

# 4) status：active_effective=false 是设计行为（静态保持不走 solver）
sudo -A ./build/affinityctl status --socket /tmp/affinitygraph-clickhouse-s3.sock | python3 -c '
import sys, json
d = json.load(sys.stdin)
for k in ("effective_mode","bpf","collector_degraded","threads","pinned_threads",
          "active_effective","solver_phase","paused","fatal_error"):
    print(f"{k}: {d.get(k)}")'
```

> **不要用 `active_effective` 作为静态场景的测量门禁**——静态保持提前返回、
> `selector_ready` 不置位是设计行为。以 `initial_affinity success:true` +
> 实际掩码 + `profile_static_hold` 为准。

### 5.5 YCSB 测量 + 停止

同 3.5（socket/日志换成 `clickhouse-s3`）。测量期间可观察日志确认无迁移
（`solve_window_end` 恒为 `profile_static_hold`）。结束：

```sh
sudo -A ./build/affinityctl pause --socket /tmp/affinitygraph-clickhouse-s3.sock
sudo -A pkill -TERM -x affinity-run
sudo -A grep -E '"type":"(pause|runtime_stop)"' \
  /var/log/affinitygraph-clickhouse-s3/runtime.jsonl | tail
sudo -A pkill -x clickhouse; sudo -A pkill -x clckhouse-watch
```

## 6. 自动化脚本

`tests/manual-scenarios.sh` 自动完成四个场景（0=baseline，1-3=affinitygraph）：
场景 0 直接启动数据库测基线；场景 1-3 按场景生成独立 toml、生成/选用
profile、启动 affinity-run、轮询就绪标记、打印 status 校验、停止与清理。

```sh
# 四场景全跑（交互：每个场景就绪后回车继续 / q 退出）
cd ~/affinitygraph
tests/manual-scenarios.sh --db clickhouse

# 只跑某个场景（0=baseline，1=无 profile 动态，2=有 profile 动态，3=静态）
tests/manual-scenarios.sh --db clickhouse --scenario 0
tests/manual-scenarios.sh --db clickhouse --scenario 3

# 无人值守：就绪后自动等待 60s 再进入下一场景
AUTO_NEXT_SECONDS=60 tests/manual-scenarios.sh --db clickhouse

# 只打印将要执行的命令，不实际执行
tests/manual-scenarios.sh --db clickhouse --dry-run
```

脚本顶部 CONFIG 块是 183 默认值，可用同名环境变量覆盖：
`CLICKHOUSE_BIN`、`CLICKHOUSE_CONFIG`、`CLICKHOUSE_PROFILE`、
`CLICKHOUSE_CPUS`、`CLICKHOUSE_RUN_USER`（默认 root，设空则不带 `--user`；
数据属主非 root 时设实际用户，如 `xhc`）、`CLICKHOUSE_READY_PORT`（默认
9004，baseline 就绪判定端口）、`CLICKHOUSE_QUIESCENT_WINDOWS`、`TARGETS_DIR`、
`PROFILES_DIR`、`CALIBRATION_DIR`、`SUDO_ASKPASS`、`READY_TIMEOUT_SECONDS`
等。非 root 用户运行时自动经 `SUDO_ASKPASS + sudo -A` 提权；baseline 需以
非 root 运行数据库时自动经 `runuser -u <user>` 启动。

脚本只负责启动与调度状态校验；**正式 YCSB 测量请在另一终端按第 3.5 节手动
执行**。

## 7. 日志速查

```sh
# runtime 全量事件
sudo -A grep '"type"' /var/log/affinitygraph-clickhouse-sN/runtime.jsonl | tail -30

# 只关心某几类
sudo -A grep -E '"type":"(profile_load|profile_match|initial_affinity)"' /var/log/affinitygraph-clickhouse-sN/runtime.jsonl
sudo -A grep -E '"type":"(solve_window_end|action|action_commit|sampling_stopped)"' /var/log/affinitygraph-clickhouse-sN/runtime.jsonl

# supervisor 启动日志（启动失败先看这里）
sudo -A tail -30 /tmp/affinity-clickhouse-sN.log
```

## 8. FAQ

- **`--user root` 与数据属主？** ClickHouse 要求进程用户与数据目录属主一致。
  数据为 root 属主时默认 `--user root` 正确；数据为 xhc 等非 root 属主时，
  设 `CLICKHOUSE_RUN_USER=xhc` 或置空，否则 ClickHouse 报 Code: 430。
- **`active_effective=false` 是不是没生效？** 静态场景（场景 3）这是设计行为；
  动态场景（1/2）它应为 true，若为 false 检查 `solver_phase`、`pinned_threads`、
  决策窗口日志。
- **`sampling_stopped` 后还能放置新线程吗？** 不能，采样已停；如有新进程
  需要重新评估，请调大 `static_quiescent_windows` 或用
  `sample_interval_seconds>0`。
- **为什么场景 1 迁移很少？** solver 需要跨窗口积累证据，且受
  `minimum_confidence`/`proposal_confirmations`/`maximum_migrated_threads_ratio`
  约束；给足时长（≥ 数个 `graph_horizon_seconds`）再测量。
- **校准尺度可以直接复用吗？** `clickhouse-gate2-fixed-v2` 只适用于 183 的
  C2T2 纯读 ClickHouse 负载；换机器/负载必须重新 `make calibrate` 或人工
  评审，否则关系尺度不可信。
- **换机器怎么用？** 把 CONFIG 块路径改成你的实际路径；信封 CPU 集合按
  `lscpu -e=CPU,NODE` 确认；校准重新 `make calibrate`。
