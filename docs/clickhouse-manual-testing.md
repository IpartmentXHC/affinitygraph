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

profile 放置后**保持不动**，跳过 solver。静态模式（`sample_interval_seconds=0`）
按线程组名扫描：每发现一个匹配线程就绑定到目标 node；连续
`static_quiescence_seconds`（默认 30）秒无新匹配线程即判定初始绑定完成
（`sampling_stopped`），随后进入**低频续扫**（`static_scan_seconds`，默认每
30 秒一次），运行期新建的匹配线程仍会被自动绑定；设 `static_scan_seconds=0`
可彻底停止扫描。profile 不含 `count`，不依赖具体线程数（数据库按硬件自动
调整线程数也能覆盖）。

### 5.1 写测试配置（静态开关 + 零采样）

```sh
sudo -A tee /etc/affinitygraph/targets/clickhouse-s3.toml >/dev/null <<'EOF'
[runtime]
mode = "active"
dynamic = false
sample_interval_seconds = 0
static_quiescence_seconds = 30
static_scan_seconds = 30
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
>   `sampling: fail`）；`static_quiescence_seconds=30` 表示 30 秒内没有新的
>   匹配线程出现就判定初始绑定完成。线程池慢启动的库可调大（如 60）。
> - 绑定完成后默认**低频续扫**：`static_scan_seconds=30` 表示每 30 秒扫一次，
>   运行期新建的匹配线程（如 ClickHouse 按需扩的 `ThreadPool`）会被自动绑定；
>   想回到彻底停止（零开销）就设 `static_scan_seconds=0`。

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
9004，baseline 就绪判定端口）、`CLICKHOUSE_QUIESCENCE_SECONDS`（默认 30）、`CLICKHOUSE_STATIC_SCAN_SECONDS`（默认 30）、`TARGETS_DIR`、
`PROFILES_DIR`、`CALIBRATION_DIR`、`SUDO_ASKPASS`、`READY_TIMEOUT_SECONDS`
等。非 root 用户运行时自动经 `SUDO_ASKPASS + sudo -A` 提权；baseline 需以
非 root 运行数据库时自动经 `runuser -u <user>` 启动。

脚本只负责启动与调度状态校验；**正式 YCSB 测量请在另一终端按第 3.5 节手动
执行**。

### 6.1 在新机器上生成 profile（换硬件）

profile **不依赖具体线程数**（无 `count` 字段）：每条规则 = 线程组名白名单
（`comm`/`comm_prefix`）+ 一个目标 CPU 集合。新机器上不需要重新观测线程数量，
只需把旧模板里 `allowed_cpus` 与 `affinities[].cpus` 改成新机器目标 node 的
CPU 集合（`lscpu -e=CPU,NODE` 确认，必须落在资源信封内），其余保持不变。

```sh
# 1) 拷贝模板（ClickHouse 示例）
cp /etc/affinitygraph/profiles/clickhouse-threadpool-node2plus3.candidate.json \
   /etc/affinitygraph/profiles/clickhouse-newhost.candidate.json

# 2) 编辑为新机器目标 node 的 CPU 集合（示例 node2+3 = 0-63）：
#    "schema_version": 2
#    "allowed_cpus": "0-63"
#    "affinities": [{"cpus": "0-63"}]

# 3) preflight 校验（期望 thread_profile: ok (N placement rule(s), static)）
sudo -A ./build/affinity-run preflight \
  --config /etc/affinitygraph/targets/clickhouse-s3.toml \
  --thread-profile /etc/affinitygraph/profiles/clickhouse-newhost.candidate.json \
  --bpf-object build/affinitygraph.bpf.o
```

要点：

- 运行时按线程组名扫描，每发现一个匹配线程就绑定到目标 CPU 集合；30 秒
  （`static_quiescence_seconds`）无新匹配线程即判定初始绑定完成，之后低频
  续扫（`static_scan_seconds`，默认每 30 秒一次）继续绑定运行期新建线程，
  所以无需知道/配置线程数。
- 生成的 profile 为静态（`dynamic.enabled=false`）；需要动态微调时把顶层
  `dynamic.enabled` 改为 `true` 再按场景 2 使用。
- 新机器如有不同数据库版本导致线程组名变化，先用方案一观察实际 comm，
  再调整匹配规则。



## 7. 大表构建、NUMA 探针与正式 A/B（ClickHouse 大表 + 扫描/聚合负载）

背景：第 2/3/5 节用的是 105 MiB 小表点查（`workloada_clickhouse_numa_read`），
服务端成本被 JDBC 客户端开销淹没，测不到 NUMA 收益。本节切换到 ≤10GB 大表 +
扫描/聚合负载：先用受控四臂探针判定「扫描/聚合是否受 CPU 亲和性影响」（Gate），
通过后再做正式 A/B。

### 7.1 大表 usertable_big（键格式与 YCSB 逐字节一致）

`tests/ch-bigtable-build.sh` 生成 `ycsb.usertable_big`：`YCSB_KEY String` 主键 +
`field0..field9 String`，`MergeTree PRIMARY KEY YCSB_KEY ORDER BY YCSB_KEY
SETTINGS index_granularity=8192`。键 = `"user" + %019d(Math.abs(fnvhash64(keynum)))`，
与 YCSB `CoreWorkload.buildKeyName` + `Utils.fnvhash64`（FNV-1a 64：offset
`0xCBF29CE484222325`、prime `1099511628211`、8 字节小端、末尾 `Math.abs`）
逐字节一致，所以**不需要跑 YCSB load**，纯 SQL `INSERT ... SELECT numbers(N)`
即可。

```sh
cd ~/affinitygraph
tests/ch-bigtable-build.sh --dry-run   # 先看生成的 SQL（FNV WITH 链 + randomPrintableASCII(100)）
tests/ch-bigtable-build.sh             # 默认 12M 行；建完查 count() 与 system.parts.bytes_on_disk
tests/ch-bigtable-build.sh --verify    # 抽样 5 键 vs Python 参考 + 1000 随机键 IN 点查 count()=1000
tests/ch-bigtable-build.sh --drop      # 重建（默认已存在且行数一致时跳过插入）
```

- 环境变量：`CH_CLIENT/CH_HOST/CH_PORT`（native 9000）、`CH_DATABASE/CH_TABLE`、
  `BIG_TABLE_ROWS`（默认 12000000）、`BIG_TABLE_MAX_GB`（默认 10）。
- 表磁盘占用（`system.parts.bytes_on_disk`）超过 `BIG_TABLE_MAX_GB` 时脚本打印
  建议行数并非零退出，**不自动重建**。
- 12M 行原始约 12GB，压缩后预计 6–9GB；超限就按提示调小 `BIG_TABLE_ROWS`。

### 7.2 四臂 NUMA 探针

`tests/ch-numa-probe.sh` 自动跑四臂（脚本会接管 ClickHouse 生命周期：每臂
stop→start→就绪→探针→stop，结束后恢复原启动方式）：

| 臂 | CPU 亲和 | 内存 | 说明 |
| --- | --- | --- | --- |
| A | 自然分布 | 自然 | 现状基线，无任何干预 |
| B | 不 pin | membind node2 | 仅约束用户态内存；静态模式无 profile 时 affinity-run 拒绝启动，故直接 `numactl` 包 CH |
| C | pin node2+3（64-127） | membind node2 | 本地访问：`clickhouse-threadpool-node2plus3.candidate.json` |
| D | pin node0/1（0-63） | membind node2 | 远端访问：探针专用副本（cpus→0-63，仅 /tmp，不进正式候选） |

```sh
cd ~/affinitygraph
tests/ch-numa-probe.sh --dry-run        # 只打印将执行的命令
tests/ch-numa-probe.sh                  # A→B→C→D；A 臂后交互暂停检查（PROBE_PAUSE_AFTER_A=0 关）
tests/ch-numa-probe.sh --arm C          # 只跑某一臂（排障用）
```

探针负载（183 本机 HTTP keep-alive，优先 python3，缺则 bash `/dev/tcp` 降级）：
先预热 page cache，然后

- `Q1` 点查（命中键）：`SELECT count(), sum(length(field0..9)) WHERE YCSB_KEY = ?`
- `Q2-<t>` 范围聚合：`SELECT count(), sum(length(field0..9)) WHERE YCSB_KEY >= ? AND < ?`
  （覆盖约 1M/5M/10M 行；键界在显示键空间按 `L = t·2^63/N` 反推）
- `Q3-<t>` 服务端纯 scan：`SELECT count() FROM (SELECT YCSB_KEY FROM t LIMIT ?)`
  （只传一列，避免客户端传输稀释服务端成本）

输出每臂 p50/p95 与 C vs D 差值；原始记录在
`/tmp/ch-numa-probe/probe-{A,B,C,D}.tsv`。可调：`PROBE_CONCURRENCY`（默认 16）、
`PROBE_ITERATIONS`（20）、`PROBE_Q2_ROWS`、`PROBE_Q3_LIMITS`、`PROBE_TABLE`、
`PROBE_EXPECT_ROWS` 等。

> Q2 键界说明：`Math.abs` 使显示键折叠到 `[0, 2^63)`，键在显示空间的密度是
> `N/2^63`，覆盖 t 行的范围长度 `L = t·2^63/N`（不是 `t·2^64/N`）。

### 7.3 Gate 判定

同一数据布局下，C（本地）vs D（远端）服务端 p50 差 ≥10% 且 p95 同向
（Q2/Q3 任一命中即算；方向不敏感，C 更快或 D 更快都算）：

- 命中 →「扫描/聚合受 CPU 亲和性影响、有研究必要」→ 进入 7.4 正式 A/B，
  最终 workload 保留 `scan=0.10`；
- 未命中 → 输出「该负载形态无研究必要」，不加 scan（或仅 read）。

### 7.4 正式 A/B（read 0.90 + scan 0.10）

workload 模板 `tests/workloads/clickhouse_numa_scan.properties`：
`readproportion=0.90`、`scanproportion=0.10`、`scanlengthdistribution=uniform`、
`maxscanlength=1000`、`requestdistribution=zipfian`、`zeropadding=19`（**必须**，
YCSB 默认 `zeropadding=1` 与表键格式不一致）。

```sh
# 前置: cp tests/ycsb-bench.conf.example tests/ycsb-bench.conf，并把 CLIENT_HOSTS 留空
#       （CLIENTS 由 --clients 覆盖 → 197 个 local 进程，见 3.5 节说明）
# 每臂（baseline / C / D）各 3 轮；EXTRA_YCSB_ARGS 叠加模板 + recordcount
EXTRA_YCSB_ARGS="-P /home/xhc/affinitygraph/tests/workloads/clickhouse_numa_scan.properties -p recordcount=12000000" \
  tests/run-ycsb-bench.sh --config tests/ycsb-bench.conf --clients 197 --rounds 3 --tag numa-scan-C
# baseline 与 D 臂同样各跑一遍（tag 区分）
```

`run-ycsb-bench.sh` 汇总端到端吞吐 mean/stddev（辅助口径）；服务端耗时用
`system.query_log` 交叉验证（config.xml 需 `log_queries` 开启，默认开）：

```sql
SELECT quantile(0.50)(query_duration_ms) AS p50, quantile(0.95)(query_duration_ms) AS p95,
       count() AS n
FROM system.query_log
WHERE event_time > now() - INTERVAL 20 MINUTE
  AND query LIKE '%usertable_big%' AND type = 'QueryFinish';
```

最终报告以服务端 p50 差为主口径、端到端吞吐为辅助口径。若 Gate 显示仅
Q2/Q3 聚合敏感而 JDBC scan 形态被传输稀释，则补一轮 7.2 探针作为代表性负载
的正式对照（记录服务端耗时分布）。

### 7.5 恢复

探针/正式 A/B 结束后脚本自动恢复；手动恢复（杀掉 membind/affinity-run 实例后
用原命令重启，`SELECT 1` 轮询就绪）：

```sh
sudo -A pkill -x affinity-run; sudo -A pkill -x clickhouse; sudo -A pkill -x clckhouse-watch
sudo -A nohup /home/xhc/clickhouse/ClickHouse/build/programs/clickhouse server \
  --config-file /home/xhc/clickhouse/etc/config.xml \
  > /tmp/affinity-clickhouse-baseline.log 2>&1 &
until timeout 2 bash -c 'exec 3<>/dev/tcp/127.0.0.1/9004' 2>/dev/null; do sleep 5; done
sudo -A /home/xhc/clickhouse/ClickHouse/build/programs/clickhouse client --query "SELECT 1"
```

`usertable_big` 默认保留供复测；不再需要时：

```sh
sudo -A /home/xhc/clickhouse/ClickHouse/build/programs/clickhouse client \
  --query 'DROP TABLE IF EXISTS ycsb.usertable_big'
```

### 7.6 实测记录（183，2026-08-24）

本节在 183 上按 7.1–7.3 实际执行一遍后的结果与注意事项（供复测对照）。

**建表与校验**

- 12M 行建出后 `system.parts.bytes_on_disk = 11.51GB`，超过 10GB 上限，脚本按
  提示非零退出；改 `BIG_TABLE_ROWS=9900000` 重建：**9,900,000 行、9.50GB**。
- `tests/ch-bigtable-build.sh --verify`（`BIG_TABLE_ROWS=9900000`）通过：抽样
  5 个键与 Python 参考逐字节一致；1000 个随机键 `IN` 点查 `count()=1000`
  （读取命中率 100%）。`ycsb.usertable_big` 保留在 183 供复测。

**四臂探针结果**（每臂 20 次迭代、并发 16；p50 为服务端耗时 ms，来自
`/tmp/ch-numa-probe/probe-{A,B,C,D}.tsv`）

| qid | A（自然） | B（membind） | C（pin node2+3） | D（pin node0/1） |
| --- | --- | --- | --- | --- |
| Q1 | 206.4 | 150.0 | 162.9 | 184.1 |
| Q2-1M | 1367.2 | 5568.7 | 5222.0 | 5740.9 |
| Q2-5M | 6073.6 | 25860.7 | 23447.3 | 24968.0 |
| Q2-10M | 10982.1 | 46300.0 | 45331.4 | 47971.3 |
| Q3-1M | 88.2 | 389.9 | 303.6 | 328.2 |
| Q3-5M | 164.1 | 831.2 | 603.9 | 651.4 |
| Q3-10M | 251.9 | 1089.3 | 795.6 | 851.9 |

绑定有效性：C 臂 333 条 `profile_match` 全部 `target_cpus:"64-127"`
（末次 `initial_affinity` committed=14）；D 臂 305 条全部 `"0-63"`
（committed=2），静态绑定生效。

**Gate 结论：未命中 → 不进入正式 A/B**

C vs D（同为 membind，仅 CPU pin 不同）Q2/Q3 服务端 p50 差分别为
9.9% / 6.5% / 5.8%（Q2-1M/5M/10M）与 8.1% / 7.9% / 7.1%（Q3-1M/5M/10M），
全部 <10%，p95 也未同向显著（最大 21.9% 仅在 Q3-1M，p50 未达阈值）→ 按 7.3
判定「**该负载形态无研究必要（扫描/聚合不受显著 CPU 亲和性影响）**」，最终
workload 不加 scan（或仅 read），7.4 的正式 A/B 与 197 客户端测量不执行。

**183 特有注意事项（复测/换机对照）**

- multi-call 二进制必须带子命令：`CH_CLIENT="/home/xhc/clickhouse/ClickHouse/build/programs/clickhouse client"`，
  裸 `--host` 会报 Unrecognized option。
- CH 以非 root 用户 `xhc` 运行（数据属主 xhc）：root 直接启动报 Code: 430，
  需 `CLICKHOUSE_RUN_USER=xhc`（脚本已自动经 `runuser -u xhc --` 启动）。
- `numactl` 不认 node 名字：membind 必须写数字，`PROBE_MEMBIND_NODE=2`。
- 183 的 `system.query_log` 未启用（表不存在），7.4 的 query_log 服务端耗时
  交叉验证在 183 不可用；复测若需要，先开 `log_queries` 并重启 CH。
- Q1 p50 偏大（与 Q2/Q3 重查询混跑排队所致；空闲时点查约 0.1–0.2ms），
  Q1 不参与 Gate。Q2-10M 实际覆盖 9,801,110 行（`t>N` 时夹到 99% 键界）。

## 8. 日志速查

```sh
# runtime 全量事件
sudo -A grep '"type"' /var/log/affinitygraph-clickhouse-sN/runtime.jsonl | tail -30

# 只关心某几类
sudo -A grep -E '"type":"(profile_load|profile_match|initial_affinity)"' /var/log/affinitygraph-clickhouse-sN/runtime.jsonl
sudo -A grep -E '"type":"(solve_window_end|action|action_commit|sampling_stopped)"' /var/log/affinitygraph-clickhouse-sN/runtime.jsonl

# supervisor 启动日志（启动失败先看这里）
sudo -A tail -30 /tmp/affinity-clickhouse-sN.log
```

## 9. FAQ

- **`--user root` 与数据属主？** ClickHouse 要求进程用户与数据目录属主一致。
  数据为 root 属主时默认 `--user root` 正确；数据为 xhc 等非 root 属主时，
  设 `CLICKHOUSE_RUN_USER=xhc` 或置空，否则 ClickHouse 报 Code: 430。
- **`active_effective=false` 是不是没生效？** 静态场景（场景 3）这是设计行为；
  动态场景（1/2）它应为 true，若为 false 检查 `solver_phase`、`pinned_threads`、
  决策窗口日志。
- **`sampling_stopped` 后还能放置新线程吗？** 默认可以：绑定完成后进入低频
  续扫（`static_scan_seconds`，默认每 30 秒一次），运行期新建的匹配线程会被
  自动绑定；设 `static_scan_seconds=0` 才彻底停止。
- **为什么场景 1 迁移很少？** solver 需要跨窗口积累证据，且受
  `minimum_confidence`/`proposal_confirmations`/`maximum_migrated_threads_ratio`
  约束；给足时长（≥ 数个 `graph_horizon_seconds`）再测量。
- **校准尺度可以直接复用吗？** `clickhouse-gate2-fixed-v2` 只适用于 183 的
  C2T2 纯读 ClickHouse 负载；换机器/负载必须重新 `make calibrate` 或人工
  评审，否则关系尺度不可信。
- **换机器怎么用？** 把 CONFIG 块路径改成你的实际路径；信封 CPU 集合按
  `lscpu -e=CPU,NODE` 确认；校准重新 `make calibrate`。
