# Doris 手动测试教程（不使用 YBA）

面向**手动启动 Doris + YCSB** 的压测场景：不用 YBA 编排，直接用
`affinity-run` 监督 Doris 进程，用 YCSB 打负载。覆盖四个场景：

| 场景 | profile | 调度方式 | 预期行为 |
| --- | --- | --- | --- |
| 0 | 无 | baseline | 不启动 affinitygraph，直接启动数据库测基线吞吐（对照） |
| 1 | 无 | 动态 | runtime 自行采样、决策并迁移线程（`mode=active` 一步闭环） |
| 2 | 有 | 动态 | profile 先做初始放置，solver 再在其上增量微调 |
| 3 | 有 | 静态 | profile 放置后保持不动（`dynamic=false` + 可 `sample_interval_seconds=0` 停止采样） |

文中命令以 183 上的默认路径为准（`/home/xhc/...`、`/etc/affinitygraph/...`）；
换机器时替换成你的实际路径。四场景也可用 `tests/manual-scenarios.sh` 自动跑
（见第 6 节）。

## 0. 前置准备（四种场景通用）

```sh
# 1) 构建（make all 已包含用户态程序 + BPF 对象 build/affinitygraph.bpf.o）
cd ~/affinitygraph
make all CXX=/usr/bin/clang++-18 CLANG=/usr/bin/clang-18

# 2) 校准（每台机器一次，实测延迟；无实测数据也能生成，is_estimated 标记启发值）
sudo -A make calibrate
# 或把人工审核过的 CSV 放到 /etc/affinitygraph/calibration/hardware-node-edges.csv

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
- **必须以 root 运行并带 `--user root`**：183 上 Doris 数据目录
  （`/home/xhc/doris/apache-doris-2.1.2-bin-arm64/{fe,be}`）是 root 属主，
  只有 root 能写；不带 `--user root` 或以非 root 启动时会报
  `unknown run user` / 目录权限错误。
- **BPF 必须 ok**：多进程 Doris 依赖 BPF 递归跟踪 fork/exec 发现全部线程，
  `collector.required=true` 下 `bpf: fail` 会直接拒绝启动。

## 1. 拓扑与关键参数（183）

```sh
# CPU→NUMA node 映射（183：node2=64-95，node3=96-127）
lscpu -e=CPU,NODE,SOCKET | sort -k2 -n | less
# 目标 CPU 集合：node2 → 64-95（本教程默认）
```

- Doris FE MySQL 协议端口：9030（YCSB JDBC 连接用）
- 三个场景各自独立的 socket/日志目录，互不干扰：
  `/tmp/affinitygraph-doris-s{1,2,3}.sock`、
  `/var/log/affinitygraph-doris-s{1,2,3}/runtime.jsonl`

## 2. 场景 0：baseline（对照基线）

不启动 affinitygraph，直接启动 Doris，测量无干预的基线吞吐，作为场景
1/2/3 的对照（脚本 `tests/manual-scenarios.sh --db doris` 默认包含该场景）。

```sh
# 直接启动 FE/BE（等价于脚本场景 0 的命令）
sudo -A nohup bash -c '/home/xhc/doris/apache-doris-2.1.2-bin-arm64/fe/bin/start_fe.sh --daemon \
                        && /home/xhc/doris/apache-doris-2.1.2-bin-arm64/be/bin/start_be.sh --daemon' \
  > /tmp/affinity-doris-baseline.log 2>&1 &

# 等待 FE 9030 端口就绪（Doris FE 启动较慢，约 1~2 分钟）
until timeout 2 bash -c 'exec 3<>/dev/tcp/127.0.0.1/9030' 2>/dev/null; do sleep 5; done

# 然后执行 YCSB 测量（命令同 3.5 节），记录 baseline 吞吐

# 结束：停掉 Doris
sudo -A pkill -x java; sudo -A pkill -x doris_be
```

- 运行用户：默认以 root 启动（Doris 数据为 root 属主）；如需非 root，脚本会
  按 `DORIS_RUN_USER` 经 `runuser -u <user>` 启动。
- baseline 没有 runtime.jsonl / affinityctl，就绪判定是 FE 端口（9030）可连。

## 3. 场景 1：无 profile 文件的动态调度

让 runtime 全自动：采样 → 构建关系证据 → solver 决策 → 迁移线程，全程无人工
介入。适用于没有先验放置方案、想先看动态优化收益的场景。

### 3.1 写测试配置

```sh
sudo -A mkdir -p /etc/affinitygraph/targets
sudo -A tee /etc/affinitygraph/targets/doris-s1.toml >/dev/null <<'EOF'
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
log_directory = "/var/log/affinitygraph-doris-s1"
socket_path = "/tmp/affinitygraph-doris-s1.sock"

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

> `mode="active"` 内部已包含“观察→计划→执行”的自动闭环，不需要先 observe
> 再手动切 plan/active。`dynamic` 不写（默认开启）＝solver 每
> `solve_interval_seconds` 决策一次。`maximum_migrated_threads_ratio` /
> `minimum_dwell_seconds` 等旋钮控制调整幅度。

### 3.2 preflight 校验

```sh
sudo -A ./build/affinity-run preflight \
  --config /etc/affinitygraph/targets/doris-s1.toml \
  --bpf-object build/affinitygraph.bpf.o
# 期望 thread_profile 行不出现（无 profile）；其余 ok
```

### 3.3 启动（后台运行）

```sh
cd ~/affinitygraph
sudo -A nohup ./build/affinity-run run \
  --config /etc/affinitygraph/targets/doris-s1.toml \
  --bpf-object build/affinitygraph.bpf.o \
  --user root \
  -- bash -c '/home/xhc/doris/apache-doris-2.1.2-bin-arm64/fe/bin/start_fe.sh --daemon \
               && /home/xhc/doris/apache-doris-2.1.2-bin-arm64/be/bin/start_be.sh --daemon \
               && wait' \
  > /tmp/affinity-doris-s1.log 2>&1 &
```

要点：

- `affinity-run run` 监督 wrapper，wrapper 不退出就一直监督；wrapper 退出时
  runtime 执行 restore 并退出。
- 也可在前台终端运行（去掉 `nohup ... &`），方便 Ctrl+C 收尾。

### 3.4 验证调度闭环

```sh
# 等 Doris 起来并完成至少一个决策窗口（约 60~90s）
sleep 90

# 1) 日志：runtime_start → solve_window_end（outcome 是动态决策，不是 profile_static_hold）
sudo -A grep -E '"type":"(runtime_start|solve_window_end)"' \
  /var/log/affinitygraph-doris-s1/runtime.jsonl | tail -10

# 2) status：active_effective=true 表示 solver 已就绪并正在执行方案
sudo -A ./build/affinityctl status --socket /tmp/affinitygraph-doris-s1.sock | python3 -c '
import sys, json
d = json.load(sys.stdin)
for k in ("effective_mode","bpf","collector_degraded","target_tgids","threads",
          "pinned_threads","selector_ready","policy_armed","active_effective","solver_phase","paused"):
    print(f"{k}: {d.get(k)}")'
# 期望 pinned_threads > 0，active_effective=true，collector_degraded=false

# 3) 看每个窗口的实际决策
sudo -A grep '"type":"solve_window_end"' /var/log/affinitygraph-doris-s1/runtime.jsonl | tail -5
```

> 场景 1 的迁移可能较小（初始无先验，solver 需积累证据、受
> `minimum_confidence` / `proposal_confirmations` 约束）。要看收益，给足
> 时长（至少几个 `graph_horizon_seconds`）再压测。

### 3.5 YCSB 测量（另一终端，197 客户端）

```sh
# 197 客户端；workloada_doris 为纯读，连接 183 FE:9030（conf/db_183_doris.properties 已内置）
cd /home/xhc/ycsb-jdbc-binding-0.17.0
python2 bin/ycsb run jdbc -s \
  -P workloads/workloada_doris \
  -P conf/db_183_doris.properties \
  -cp lib/mysql-connector-java-8.0.28.jar \
  -p table=usertable -p threads=2 \
  -p operationcount=8000 -p status.interval=10
# 预热建议：2 个客户端 × 2 线程跑 60s 后再正式测量
# 结果取 [OVERALL] Throughput(ops/sec)
```

### 3.6 停止场景 1

```sh
# 1) pause：同步恢复被迁移线程的掩码
sudo -A ./build/affinityctl pause --socket /tmp/affinitygraph-doris-s1.sock

# 2) 停 supervisor（wrapper 随之退出，runtime 收尾 restore）
sudo -A pkill -TERM -x affinity-run

# 3) 核对收尾日志
sudo -A grep -E '"type":"(pause|runtime_stop)"' \
  /var/log/affinitygraph-doris-s1/runtime.jsonl | tail

# 4) 清理 Doris 进程（如需）
sudo -A pkill -x java; sudo -A pkill -x doris_be
```

## 4. 场景 2：有 profile 文件的动态调度

先用已验证的放置画像做**初始放置**，再由 solver 在其上做**小范围动态微调**
——即“初始方案 + 增量调整”。

### 4.1 准备动态 profile（复制 + 打开顶层 dynamic）

```sh
# 参考 profile 已存在（顶层 dynamic.enabled=true）
sudo -A grep -n '"enabled"' /etc/affinitygraph/profiles/doris-node2-dynamic.candidate.json | head -1

# 生成场景 2 专用副本（只改顶层 dynamic，placement 级不动）
sudo -A mkdir -p /etc/affinitygraph/profiles
sudo -A python3 - <<'PY'
import json
src = "/etc/affinitygraph/profiles/doris-node2-dynamic.candidate.json"
dst = "/etc/affinitygraph/profiles/doris-s2.dynamic.json"
d = json.load(open(src))
d["dynamic"]["enabled"] = True
d["profile_id"] = d.get("profile_id", "dynamic") + "-s2-dynamic"
open(dst, "w").write(json.dumps(d, indent=2, ensure_ascii=False) + "\n")
PY

# 确认顶层 dynamic 已打开
sudo -A grep -n '"enabled"' /etc/affinitygraph/profiles/doris-s2.dynamic.json | head -1
```

> 若用 `sed -i 's/"enabled": false/"enabled": true/'`，只会命中第一个匹配
> （即顶层 dynamic 节），效果相同；但 python 方式更明确、可重复。

### 4.2 写测试配置

```sh
sudo -A tee /etc/affinitygraph/targets/doris-s2.toml >/dev/null <<'EOF'
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
log_directory = "/var/log/affinitygraph-doris-s2"
socket_path = "/tmp/affinitygraph-doris-s2.sock"

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

> 与场景 1 的唯一差异是启动时带 `--thread-profile`；配置本身可以复用。

### 4.3 preflight 校验 profile

```sh
sudo -A ./build/affinity-run preflight \
  --config /etc/affinitygraph/targets/doris-s2.toml \
  --thread-profile /etc/affinitygraph/profiles/doris-s2.dynamic.json \
  --bpf-object build/affinitygraph.bpf.o
# 期望 thread_profile: ok (N placement rule(s), dynamic)
```

### 4.4 启动

```sh
cd ~/affinitygraph
sudo -A nohup ./build/affinity-run run \
  --config /etc/affinitygraph/targets/doris-s2.toml \
  --thread-profile /etc/affinitygraph/profiles/doris-s2.dynamic.json \
  --bpf-object build/affinitygraph.bpf.o \
  --user root \
  -- bash -c '/home/xhc/doris/apache-doris-2.1.2-bin-arm64/fe/bin/start_fe.sh --daemon \
               && /home/xhc/doris/apache-doris-2.1.2-bin-arm64/be/bin/start_be.sh --daemon \
               && wait' \
  > /tmp/affinity-doris-s2.log 2>&1 &
```

### 4.5 验证：初始放置 + 动态微调

```sh
# 等 Doris 起来并完成初始放置 + 至少一个决策窗口
sleep 90

# 1) 三连：profile_load(success) → profile_match(逐线程命中) → initial_affinity(committed≥1)
sudo -A grep -E '"type":"(profile_load|profile_match|initial_affinity)"' \
  /var/log/affinitygraph-doris-s2/runtime.jsonl | tail -15

# 2) 决策窗口 outcome（应出现动态 outcome，不是 profile_static_hold）
sudo -A grep '"type":"solve_window_end"' /var/log/affinitygraph-doris-s2/runtime.jsonl | tail -5

# 3) 抽查被放置线程的实际掩码（从 profile_match 挑一个 tid）
TID=$(sudo -A grep '"type":"profile_match"' /var/log/affinitygraph-doris-s2/runtime.jsonl | tail -1 | sed -E 's/.*"tid":([0-9]+).*/\1/')
sudo -A sh -c "grep Cpus_allowed_list /proc/$TID/status"
# 期望 Cpus_allowed_list: 64-95

# 4) status 关键字段
sudo -A ./build/affinityctl status --socket /tmp/affinitygraph-doris-s2.sock | python3 -c '
import sys, json
d = json.load(sys.stdin)
for k in ("effective_mode","target_tgids","threads","pinned_threads",
          "active_effective","solver_phase","paused"):
    print(f"{k}: {d.get(k)}")'
```

### 4.6 YCSB 测量 + 停止

同 3.5（socket/日志换成 `doris-s2`），测量结束后：

```sh
sudo -A ./build/affinityctl pause --socket /tmp/affinitygraph-doris-s2.sock
sudo -A pkill -TERM -x affinity-run
sudo -A grep -E '"type":"(pause|runtime_stop)"' \
  /var/log/affinitygraph-doris-s2/runtime.jsonl | tail
sudo -A pkill -x java; sudo -A pkill -x doris_be
```

## 5. 场景 3：有 profile 文件的静态调度

profile 放置后**保持不动**，跳过 solver。静态模式（`sample_interval_seconds=0`）
按线程组名扫描：每发现一个匹配线程就绑定到目标 node；连续
`static_quiescence_seconds`（默认 30）秒无新匹配线程即判定绑定完成并停止轮询
（`sampling_stopped`），采样开销趋近于零。profile 不含 `count`，不依赖具体
线程数（数据库按硬件自动调整线程数也能覆盖）。

### 5.1 写测试配置（静态开关 + 零采样）

```sh
sudo -A tee /etc/affinitygraph/targets/doris-s3.toml >/dev/null <<'EOF'
[runtime]
mode = "active"
dynamic = false
sample_interval_seconds = 0
static_quiescence_seconds = 30
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
log_directory = "/var/log/affinitygraph-doris-s3"
socket_path = "/tmp/affinitygraph-doris-s3.sock"

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

> - `dynamic = false` 是**运行时静态开关**：即使 profile 里
>   `dynamic.enabled=true` 也会被覆盖；不写该键时由 profile 的
>   `dynamic.enabled` 决定。
> - `sample_interval_seconds=0` **仅静态模式合法**（动态模式会
>   `sampling: fail`）；`static_quiescence_seconds=30` 表示 30 秒内没有新的
>   匹配线程出现就判定绑定完成并停采样。线程池慢启动的库可调大（如 60）。
> - 绑定完成后采样停止，之后新建的匹配线程**不再自动绑定**；如工作负载
>   运行期会创建新线程，请改用场景 2（动态）或设 `sample_interval_seconds>0`。

### 5.2 preflight

```sh
sudo -A ./build/affinity-run preflight \
  --config /etc/affinitygraph/targets/doris-s3.toml \
  --thread-profile /etc/affinitygraph/profiles/doris-node2-dynamic.candidate.json \
  --bpf-object build/affinitygraph.bpf.o
# 期望 thread_profile: ok (N placement rule(s), static)
```

### 5.3 启动

```sh
cd ~/affinitygraph
sudo -A nohup ./build/affinity-run run \
  --config /etc/affinitygraph/targets/doris-s3.toml \
  --thread-profile /etc/affinitygraph/profiles/doris-node2-dynamic.candidate.json \
  --bpf-object build/affinitygraph.bpf.o \
  --user root \
  -- bash -c '/home/xhc/doris/apache-doris-2.1.2-bin-arm64/fe/bin/start_fe.sh --daemon \
               && /home/xhc/doris/apache-doris-2.1.2-bin-arm64/be/bin/start_be.sh --daemon \
               && wait' \
  > /tmp/affinity-doris-s3.log 2>&1 &
```

### 5.4 验证静态保持 + 采样停止

```sh
# 等初始放置完成并进入静态保持（Doris 建议 3~5 分钟）
sleep 240

# 1) 三连 + 决策窗口 outcome=profile_static_hold
sudo -A grep -E '"type":"(profile_load|initial_affinity)"' \
  /var/log/affinitygraph-doris-s3/runtime.jsonl | tail -5
sudo -A grep '"type":"solve_window_end"' /var/log/affinitygraph-doris-s3/runtime.jsonl | tail -3
# 期望 outcome 均为 "profile_static_hold"

# 2) 采样停止事件
sudo -A grep '"type":"sampling_stopped"' /var/log/affinitygraph-doris-s3/runtime.jsonl | tail -2

# 3) 抽查掩码（profile_match 最近 tid）
TID=$(sudo -A grep '"type":"profile_match"' /var/log/affinitygraph-doris-s3/runtime.jsonl | tail -1 | sed -E 's/.*"tid":([0-9]+).*/\1/')
sudo -A sh -c "grep Cpus_allowed_list /proc/$TID/status"

# 4) status：active_effective=false 是设计行为（静态保持不走 solver）
sudo -A ./build/affinityctl status --socket /tmp/affinitygraph-doris-s3.sock | python3 -c '
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

同 3.5（socket/日志换成 `doris-s3`）。测量期间可观察日志确认无迁移
（`solve_window_end` 恒为 `profile_static_hold`）。结束：

```sh
sudo -A ./build/affinityctl pause --socket /tmp/affinitygraph-doris-s3.sock
sudo -A pkill -TERM -x affinity-run
sudo -A grep -E '"type":"(pause|runtime_stop)"' \
  /var/log/affinitygraph-doris-s3/runtime.jsonl | tail
sudo -A pkill -x java; sudo -A pkill -x doris_be
```

## 6. 自动化脚本

`tests/manual-scenarios.sh` 自动完成四个场景（0=baseline，1-3=affinitygraph）：
场景 0 直接启动数据库测基线；场景 1-3 按场景生成独立 toml、生成/选用
profile、启动 affinity-run、轮询就绪标记、打印 status 校验、停止与清理。

```sh
# 四场景全跑（交互：每个场景就绪后回车继续 / q 退出）
cd ~/affinitygraph
tests/manual-scenarios.sh --db doris

# 只跑某个场景（0=baseline，1=无 profile 动态，2=有 profile 动态，3=静态）
tests/manual-scenarios.sh --db doris --scenario 0
tests/manual-scenarios.sh --db doris --scenario 2

# 无人值守：就绪后自动等待 60s 再进入下一场景
AUTO_NEXT_SECONDS=60 tests/manual-scenarios.sh --db doris

# 只打印将要执行的命令，不实际执行
tests/manual-scenarios.sh --db doris --dry-run
```

脚本顶部 CONFIG 块是 183 默认值，可用同名环境变量覆盖：
`DORIS_HOME`、`DORIS_FE_START`、`DORIS_BE_START`、`DORIS_PROFILE`、
`DORIS_CPUS`、`DORIS_RUN_USER`（默认 root，设空则不带 `--user`）、
`DORIS_READY_PORT`（默认 9030，baseline 就绪判定端口）、
`DORIS_QUIESCENCE_SECONDS`（默认 30）、`TARGETS_DIR`、`PROFILES_DIR`、`CALIBRATION_DIR`、
`SUDO_ASKPASS`、`READY_TIMEOUT_SECONDS` 等。
非 root 用户运行时自动经 `SUDO_ASKPASS + sudo -A` 提权；baseline 需以非 root
运行数据库时自动经 `runuser -u <user>` 启动。

脚本只负责启动与调度状态校验；**正式 YCSB 测量请在另一终端按第 3.5 节手动
执行**。

### 6.1 在新机器上生成 profile（换硬件）

profile **不依赖具体线程数**（无 `count` 字段）：每条规则 = 线程组名白名单
（`comm`/`comm_prefix`）+ 一个目标 CPU 集合。新机器上不需要重新观测线程数量，
只需把旧模板里 `allowed_cpus` 与 `affinities[].cpus` 改成新机器目标 node 的
CPU 集合（`lscpu -e=CPU,NODE` 确认，必须落在资源信封内），其余保持不变。

```sh
# 1) 拷贝模板（Doris 示例）
cp /etc/affinitygraph/profiles/doris-node2-dynamic.candidate.json \
   /etc/affinitygraph/profiles/doris-newhost.candidate.json

# 2) 编辑为新机器目标 node 的 CPU 集合（示例 node2 = 0-31）：
#    "schema_version": 2
#    "allowed_cpus": "0-31"
#    "affinities": [{"cpus": "0-31"}]

# 3) preflight 校验（期望 thread_profile: ok (N placement rule(s), static)）
sudo -A ./build/affinity-run preflight \
  --config /etc/affinitygraph/targets/doris-s3.toml \
  --thread-profile /etc/affinitygraph/profiles/doris-newhost.candidate.json \
  --bpf-object build/affinitygraph.bpf.o
```

要点：

- 运行时按线程组名扫描，每发现一个匹配线程就绑定到目标 CPU 集合；30 秒
  （`static_quiescence_seconds`）无新匹配线程即判定绑定完成并停止扫描，
  所以无需知道/配置线程数。
- 生成的 profile 为静态（`dynamic.enabled=false`）；需要动态微调时把顶层
  `dynamic.enabled` 改为 `true` 再按场景 2 使用。
- 新机器如有不同数据库版本导致线程组名变化，先用方案一观察实际 comm，
  再调整匹配规则。



## 7. 日志速查

```sh
# runtime 全量事件
sudo -A grep '"type"' /var/log/affinitygraph-doris-sN/runtime.jsonl | tail -30

# 只关心某几类
sudo -A grep -E '"type":"(profile_load|profile_match|initial_affinity)"' /var/log/affinitygraph-doris-sN/runtime.jsonl
sudo -A grep -E '"type":"(solve_window_end|action|action_commit|sampling_stopped)"' /var/log/affinitygraph-doris-sN/runtime.jsonl

# supervisor 启动日志（启动失败先看这里）
sudo -A tail -30 /tmp/affinity-doris-sN.log
```

## 8. FAQ

- **为什么必须 `--user root`？** 183 的 Doris 数据目录是 root 属主，FE/BE
  启动脚本需要写权限；`affinity-run --user root` 即保持 root 身份监督。
- **`active_effective=false` 是不是没生效？** 静态场景（场景 3）这是设计行为；
  动态场景（1/2）它应为 true，若为 false 检查 `solver_phase`、`pinned_threads`、
  决策窗口日志。
- **`sampling_stopped` 后还能放置新线程吗？** 不能，采样已停；绑定完成后
  新建的匹配线程不再自动绑定。如工作负载运行期会创建新线程，请调大
  `static_quiescence_seconds`、改用场景 2（动态），或设
  `sample_interval_seconds>0`。
- **为什么场景 1 迁移很少？** solver 需要跨窗口积累证据，且受
  `minimum_confidence`/`proposal_confirmations`/`maximum_migrated_threads_ratio`
  约束；给足时长（≥ 数个 `graph_horizon_seconds`）再测量。
- **换机器怎么用？** 把 CONFIG 块路径改成你的实际路径；信封 CPU 集合按
  `lscpu -e=CPU,NODE` 确认；校准重新 `make calibrate`。
