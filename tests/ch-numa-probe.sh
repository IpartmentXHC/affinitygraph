#!/usr/bin/env bash
# ClickHouse 大表 + NUMA 敏感负载四臂探针（183）
#
# 目的: 在 ≤10GB 大表（tests/ch-bigtable-build.sh 生成的 ycsb.usertable_big）
#       上判定「扫描/聚合是否受 CPU 亲和性影响」，据此决定是否进入正式 YCSB A/B。
#
# 四臂（同一数据布局，仅 CPU/内存放置不同）:
#   A 现状    : 无 numactl、无 affinitygraph（自然分布）
#   B membind : numactl --membind=$PROBE_MEMBIND_NODE，无 affinitygraph（不 pin）。
#               说明: 静态模式（dynamic=false, sample_interval_seconds=0）在无
#               profile 时会被 affinity-run 拒绝启动（runtime 校验），故 B 臂直接
#               用 numactl 包 CH，等价于"只约束内存、不干预 CPU 亲和"。
#   C 本地    : membind=node2 + affinity-run 静态 profile 把热线程 pin 到
#               node2+3（64-127，与内存同 node，本地访问）
#   D 远端    : membind=node2 + 探针专用 profile 副本 pin 到 node0/1（0-63，
#               与内存异 node，远端访问）；副本只放 /tmp，不进正式候选目录
#
# 负载（183 本机 HTTP keep-alive；优先 python3，缺则 bash /dev/tcp 降级）:
#   先预热 page cache；然后:
#     Q1        点查（命中键）: SELECT count(), sum(length(field0..9)) WHERE YCSB_KEY = ?
#     Q2-<t>    范围聚合: SELECT count(), sum(length(field0..9)) WHERE YCSB_KEY >= ? AND < ?
#               （键界按显示键空间 [0,2^63) 密度 N/2^63 反推，覆盖 t≈1M/5M/10M 行）
#     Q3-<t>    服务端纯 scan: SELECT count() FROM (SELECT YCSB_KEY FROM t LIMIT ?)
#               （只传一列、不传 10 个字段，避免客户端传输稀释服务端成本）
#
# Gate（已确认口径）: 同一数据布局下，C（本地）vs D（远端）的 Q2/Q3 服务端
#   p50 差 ≥10% 且 p95 同向 → 判定「扫描/聚合受 CPU 亲和性影响、有研究必要」，
#   进入正式 A/B（Q2/Q3 任一命中即保留 scan=0.10）；否则输出「该负载形态无研究
#   必要」结论。脚本结束自动恢复原启动方式（无 numactl / 无 affinitygraph）。
#
# 用法:
#   tests/ch-numa-probe.sh [--arm A|B|C|D|A,B,C,D] [--dry-run] [--help]
#
# 环境变量（均有默认值）:
#   路径类: REPO/BUILD/BIN/BPF_OBJECT/TARGETS_DIR/PROFILES_DIR/CALIBRATION_DIR/
#           SUDO_ASKPASS/CLICKHOUSE_BIN/CLICKHOUSE_CONFIG/
#           CLICKHOUSE_PROFILE/CLICKHOUSE_RUN_USER(默认 root, 设空则不带 --user)
#   探针类: PROBE_HTTP_HOST(127.0.0.1)/PROBE_HTTP_PORT(8123)/PROBE_TABLE/
#           PROBE_EXPECT_ROWS(12000000)/PROBE_CONCURRENCY(16)/
#           PROBE_ITERATIONS(20)/PROBE_WARMUP_ITERATIONS(3)/
#           PROBE_PREWARM_RUNS(2)/PROBE_TIMEOUT_SECONDS(120)/
#           PROBE_Q2_ROWS("1000000 5000000 10000000")/
#           PROBE_Q3_LIMITS("1000000 5000000 10000000")/PROBE_Q1_KEYNUM(42)
#   调度类: PROBE_QUIESCENCE_SECONDS(30)/PROBE_STATIC_SCAN_SECONDS(30)/
#           READY_TIMEOUT_SECONDS(300)
#   输出类: PROBE_OUT_DIR(/tmp/ch-numa-probe)/PROBE_PAUSE_AFTER_A(1=交互暂停)/
#           PROBE_D_PROFILE_OUT(/tmp/clickhouse-threadpool-node01.probe.json)/
#           PROBE_D_CPUS(0-63, D 臂目标 cpus)
set -u

# ============ CONFIG（183 默认值，可用同名环境变量覆盖） ============
REPO=${REPO:-/home/xhc/affinitygraph}
BUILD=${BUILD:-$REPO/build}
BIN=${BIN:-$BUILD/affinity-run}
BPF_OBJECT=${BPF_OBJECT:-$BUILD/affinitygraph.bpf.o}

TARGETS_DIR=${TARGETS_DIR:-/etc/affinitygraph/targets}
PROFILES_DIR=${PROFILES_DIR:-/etc/affinitygraph/profiles}
CALIBRATION_DIR=${CALIBRATION_DIR:-/etc/affinitygraph/calibration}
SUDO_ASKPASS=${SUDO_ASKPASS:-/home/xhc/ExperScript/doris-bench/askpass.sh}

CLICKHOUSE_BIN=${CLICKHOUSE_BIN:-/home/xhc/clickhouse/ClickHouse/build/programs/clickhouse}
CLICKHOUSE_CONFIG=${CLICKHOUSE_CONFIG:-/home/xhc/clickhouse/etc/config.xml}
CLICKHOUSE_PROFILE=${CLICKHOUSE_PROFILE:-$PROFILES_DIR/clickhouse-threadpool-node2plus3.candidate.json}
CLICKHOUSE_RUN_USER=${CLICKHOUSE_RUN_USER-root}

PROBE_HTTP_HOST=${PROBE_HTTP_HOST:-127.0.0.1}
PROBE_HTTP_PORT=${PROBE_HTTP_PORT:-8123}
PROBE_TABLE=${PROBE_TABLE:-ycsb.usertable_big}
PROBE_EXPECT_ROWS=${PROBE_EXPECT_ROWS:-12000000}
PROBE_CONCURRENCY=${PROBE_CONCURRENCY:-16}
PROBE_ITERATIONS=${PROBE_ITERATIONS:-20}
PROBE_WARMUP_ITERATIONS=${PROBE_WARMUP_ITERATIONS:-3}
PROBE_PREWARM_RUNS=${PROBE_PREWARM_RUNS:-2}
PROBE_TIMEOUT_SECONDS=${PROBE_TIMEOUT_SECONDS:-120}
PROBE_Q2_ROWS=${PROBE_Q2_ROWS:-"1000000 5000000 10000000"}
PROBE_Q3_LIMITS=${PROBE_Q3_LIMITS:-"1000000 5000000 10000000"}
PROBE_Q1_KEYNUM=${PROBE_Q1_KEYNUM:-42}
PROBE_QUIESCENCE_SECONDS=${PROBE_QUIESCENCE_SECONDS:-30}
PROBE_STATIC_SCAN_SECONDS=${PROBE_STATIC_SCAN_SECONDS:-30}
PROBE_OUT_DIR=${PROBE_OUT_DIR:-/tmp/ch-numa-probe}
PROBE_PAUSE_AFTER_A=${PROBE_PAUSE_AFTER_A:-1}
PROBE_D_PROFILE_OUT=${PROBE_D_PROFILE_OUT:-/tmp/clickhouse-threadpool-node01.probe.json}
PROBE_D_CPUS=${PROBE_D_CPUS:-0-63}
PROBE_MEMBIND_NODE=${PROBE_MEMBIND_NODE:-2}   # numactl --membind 用节点号（部分 libnuma 不接受 node2 写法）

READY_TIMEOUT_SECONDS=${READY_TIMEOUT_SECONDS:-300}
DRY_RUN=0
ARMS="A B C D"

info() { printf '[INFO] %s\n' "$*"; }
warn() { printf '[WARN] %s\n' "$*"; }
die()  { printf '[FAIL] %s\n' "$*" >&2; exit 2; }

usage() { sed -n '2,44p' "$0"; exit 0; }

while [ "$#" -gt 0 ]; do
  case "$1" in
    --arm) ARMS=$2; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage ;;
    *) die "未知参数: $1（--help 查看用法）" ;;
  esac
done

for a in $ARMS; do
  case "$a" in A|B|C|D) ;; *) die "--arm 仅支持 A|B|C|D，当前: '$a'" ;; esac
done

# ============ 通用工具 ============
as_root() {
  if [ "$(id -u)" -eq 0 ]; then "$@"; else SUDO_ASKPASS="$SUDO_ASKPASS" sudo -A "$@"; fi
}

run_as_root() {
  if [ "$DRY_RUN" = 1 ]; then
    printf '  + [sudo] %s\n' "$*"
  else
    as_root "$@"
  fi
}

port_ready() { # $1=host $2=port
  timeout 2 bash -c "exec 3<>/dev/tcp/$1/$2" 2>/dev/null
}

cleanup_leftovers() {
  info "清理残留进程（affinity-run / clickhouse / clckhouse-watch）..."
  run_as_root pkill -x affinity-run 2>/dev/null || true
  [ "$DRY_RUN" = 1 ] || sleep 2
  run_as_root pkill -x clickhouse 2>/dev/null || true
  run_as_root pkill -x clckhouse-watch 2>/dev/null || true
  [ "$DRY_RUN" = 1 ] || sleep 3
}

ch_launch() { # $1=log [$2...=前置命令]; 以 CLICKHOUSE_RUN_USER 指定的用户启动 CH
  local log=$1
  shift
  if [ -n "$CLICKHOUSE_RUN_USER" ] && [ "$CLICKHOUSE_RUN_USER" != root ]; then
    run_as_root nohup runuser -u "$CLICKHOUSE_RUN_USER" -- "$@" "$CLICKHOUSE_BIN" server --config-file "$CLICKHOUSE_CONFIG" > "$log" 2>&1 &
  else
    run_as_root nohup "$@" "$CLICKHOUSE_BIN" server --config-file "$CLICKHOUSE_CONFIG" > "$log" 2>&1 &
  fi
}

ch_launch_echo() { # $1=log [$2...=前置命令]; dry-run 打印启动命令
  local log=$1
  shift
  local pre_cmd=""
  if [ -n "$CLICKHOUSE_RUN_USER" ] && [ "$CLICKHOUSE_RUN_USER" != root ]; then
    pre_cmd="runuser -u $CLICKHOUSE_RUN_USER --"
  fi
  if [ "$#" -gt 0 ]; then
    printf '  + nohup %s %s %s server --config-file %s > %s 2>&1 &\n' "$pre_cmd" "$*" "$CLICKHOUSE_BIN" "$CLICKHOUSE_CONFIG" "$log"
  else
    printf '  + nohup %s %s server --config-file %s > %s 2>&1 &\n' "$pre_cmd" "$CLICKHOUSE_BIN" "$CLICKHOUSE_CONFIG" "$log"
  fi
}

# ============ C/D 臂配置与启动 ============
write_probe_toml() { # $1=arm(C|D); 全局 PROBE_TOML
  local arm=$1
  PROBE_TOML="$TARGETS_DIR/clickhouse-probe-${arm}.toml"
  if [ "$DRY_RUN" = 1 ]; then
    info "将生成配置 $PROBE_TOML（静态: dynamic=false, sample_interval_seconds=0）"
    return 0
  fi
  run_as_root mkdir -p "$TARGETS_DIR"
  run_as_root tee "$PROBE_TOML" >/dev/null <<EOF
[runtime]
mode = "active"
dynamic = false
sample_interval_seconds = 0
static_quiescence_seconds = $PROBE_QUIESCENCE_SECONDS
static_scan_seconds = $PROBE_STATIC_SCAN_SECONDS
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
log_directory = "/var/log/affinitygraph-clickhouse-probe-${arm}"
socket_path = "/tmp/affinitygraph-clickhouse-probe-${arm}.sock"

[resources]
calibration_path = "$CALIBRATION_DIR"

[collector]
required = true
pthread_uprobe = true

[calibration]
id = "clickhouse-gate2-fixed-v2"
activity_log_p95 = 2.4138804290562152
sync_log_p95 = 2.5591179487485345
share_log_p95 = 0.00894730347830295
EOF
  info "已生成 $PROBE_TOML"
}

gen_probe_d_profile() { # $1=src $2=dst; 生成探针专用 D 臂副本; 全局 PROBE_D_PROFILE
  local src=$1 dst=$2
  PROBE_D_PROFILE=$dst
  if [ "$DRY_RUN" = 1 ]; then
    info "将生成探针专用 D 臂 profile $dst（全部 allowed_cpus/affinities[].cpus → $PROBE_D_CPUS，profile_id/placement id 改名；仅探针用，不进正式候选目录）"
    return 0
  fi
  python3 - "$src" "$dst" "$PROBE_D_CPUS" <<'PY' || die "D 臂 profile 生成失败: $src"
import json
import sys

src, dst, cpus = sys.argv[1], sys.argv[2], sys.argv[3]
d = json.load(open(src))
d["profile_id"] = d.get("profile_id", "clickhouse-threadpool") + "-node01-probe"
for p in d.get("placements", []):
    p["id"] = p.get("id", "rule") + "-node01-probe"
    p["allowed_cpus"] = cpus
    for a in p.get("affinities", []):
        a["cpus"] = cpus
with open(dst, "w") as f:
    json.dump(d, f, indent=2, ensure_ascii=False)
    f.write("\n")
PY
  info "已生成 $PROBE_D_PROFILE"
}

arm_launch() { # $1=arm; 0=启动成功
  local arm=$1
  local log="/tmp/affinity-clickhouse-probe-${arm}.log"
  case "$arm" in
    A)
      info "启动 A 臂（自然分布，无 numactl / 无 affinitygraph）"
      if [ "$DRY_RUN" = 1 ]; then
        ch_launch_echo "$log"
        return 0
      fi
      run_as_root rm -f "$log"
      ch_launch "$log"
      ;;
    B)
      info "启动 B 臂（numactl --membind=$PROBE_MEMBIND_NODE，无 affinitygraph）"
      command -v numactl >/dev/null 2>&1 || die "缺少 numactl"
      if [ "$DRY_RUN" = 1 ]; then
        ch_launch_echo "$log" numactl --membind=$PROBE_MEMBIND_NODE
        return 0
      fi
      run_as_root rm -f "$log"
      ch_launch "$log" numactl --membind=$PROBE_MEMBIND_NODE
      ;;
    C|D)
      command -v numactl >/dev/null 2>&1 || die "缺少 numactl"
      write_probe_toml "$arm"
      if [ "$arm" = C ]; then
        PROFILE="$CLICKHOUSE_PROFILE"
      else
        gen_probe_d_profile "$CLICKHOUSE_PROFILE" "$PROBE_D_PROFILE_OUT"
        PROFILE="$PROBE_D_PROFILE"
      fi
      info "启动 $arm 臂（numactl --membind=$PROBE_MEMBIND_NODE + affinity-run 静态 profile $PROFILE）"
      local args=("$BIN" run --config "$PROBE_TOML")
      args+=(--thread-profile "$PROFILE")
      args+=(--bpf-object "$BPF_OBJECT")
      if [ -n "$CLICKHOUSE_RUN_USER" ]; then args+=(--user "$CLICKHOUSE_RUN_USER"); fi
      args+=(-- "$CLICKHOUSE_BIN" server --config-file "$CLICKHOUSE_CONFIG")
      info "affinity-run 命令: numactl --membind=$PROBE_MEMBIND_NODE ${args[*]}"
      if [ "$DRY_RUN" = 1 ]; then
        printf '  + nohup numactl --membind=$PROBE_MEMBIND_NODE %s > %s 2>&1 &\n' "${args[*]}" "$log"
        return 0
      fi
      [ -x "$BIN" ] || die "缺少 $BIN，先 make all"
      [ -f "$BPF_OBJECT" ] || die "缺少 $BPF_OBJECT，先 make all"
      run_as_root rm -rf "/var/log/affinitygraph-clickhouse-probe-${arm}"
      run_as_root nohup numactl --membind=$PROBE_MEMBIND_NODE "${args[@]}" > "$log" 2>&1 &
      sleep 3
      ;;
  esac
  if [ "$DRY_RUN" = 1 ]; then return 0; fi
  sleep 2
  if ! pgrep -x clickhouse >/dev/null 2>&1; then
    warn "未发现 clickhouse 进程，启动日志 tail:"
    as_root tail -20 "$log" 2>/dev/null || true
    return 1
  fi
  return 0
}

check_marker() { # $1=arm $2=marker
  local arm=$1 m=$2
  local log="/var/log/affinitygraph-clickhouse-probe-${arm}/runtime.jsonl"
  case "$m" in
    profile_load)
      as_root sh -c "grep '\"type\":\"profile_load\"' '$log' 2>/dev/null | tail -1 | grep -q '\"success\":true'" ;;
    initial_affinity)
      as_root sh -c "grep '\"type\":\"initial_affinity\"' '$log' 2>/dev/null | tail -1 | grep -qE '\"committed\":[1-9]'" ;;
    sampling_stopped)
      as_root grep -q '"type":"sampling_stopped"' "$log" 2>/dev/null ;;
    *) return 1 ;;
  esac
}

wait_arm_ready() { # $1=arm; 0=就绪
  local arm=$1 i
  info "等待 $arm 臂 HTTP 端口 $PROBE_HTTP_PORT 就绪（最长 ${READY_TIMEOUT_SECONDS}s）..."
  for i in $(seq 1 "$READY_TIMEOUT_SECONDS"); do
    if port_ready "$PROBE_HTTP_HOST" "$PROBE_HTTP_PORT"; then
      info "HTTP 端口就绪（${i}s）"
      break
    fi
    if [ "$i" -ge "$READY_TIMEOUT_SECONDS" ]; then
      warn "HTTP 端口 $PROBE_HTTP_PORT 超时未就绪"
      return 1
    fi
    sleep 1
  done
  case "$arm" in
    C|D)
      info "等待静态放置标记（profile_load → initial_affinity → sampling_stopped）..."
      for i in $(seq 1 "$READY_TIMEOUT_SECONDS"); do
        local ok=1
        check_marker "$arm" profile_load || ok=0
        check_marker "$arm" initial_affinity || ok=0
        check_marker "$arm" sampling_stopped || ok=0
        if [ "$ok" = 1 ]; then
          info "静态放置完成（${i}s）"
          as_root sh -c "grep -E '\"type\":\"(profile_load|initial_affinity|sampling_stopped)\"' '/var/log/affinitygraph-clickhouse-probe-${arm}/runtime.jsonl' 2>/dev/null | tail -3" | sed 's/^/    /'
          return 0
        fi
        if [ "$i" -ge "$READY_TIMEOUT_SECONDS" ]; then
          warn "静态放置标记超时（${READY_TIMEOUT_SECONDS}s）；最近日志:"
          as_root tail -5 "/var/log/affinitygraph-clickhouse-probe-${arm}/runtime.jsonl" 2>/dev/null | sed 's/^/    /' || true
          return 1
        fi
        sleep 1
      done
      ;;
  esac
  return 0
}

# ============ 探针负载 ============
probe_run_python() { # $1=输出 tsv; python3 主路径（并发 + keep-alive + 动态键界）
  local outfile=$1
  python3 - "$PROBE_HTTP_HOST" "$PROBE_HTTP_PORT" "$PROBE_TABLE" \
    "$PROBE_CONCURRENCY" "$PROBE_ITERATIONS" "$PROBE_WARMUP_ITERATIONS" \
    "$PROBE_PREWARM_RUNS" "$outfile" "$PROBE_TIMEOUT_SECONDS" \
    "$PROBE_Q1_KEYNUM" "$PROBE_Q2_ROWS" "$PROBE_Q3_LIMITS" "$PROBE_EXPECT_ROWS" <<'PY'
import http.client
import queue
import struct
import sys
import threading
import time

HOST, PORT, TABLE = sys.argv[1], int(sys.argv[2]), sys.argv[3]
CONC = int(sys.argv[4])
ITERS = int(sys.argv[5])
WARM = int(sys.argv[6])
PREWARM = int(sys.argv[7])
OUTFILE = sys.argv[8]
TIMEOUT = int(sys.argv[9])
Q1_KEYNUM = int(sys.argv[10])
Q2_ROWS = [int(x) for x in sys.argv[11].replace(',', ' ').split()]
Q3_LIMITS = [int(x) for x in sys.argv[12].replace(',', ' ').split()]
EXPECT_ROWS = int(sys.argv[13])

OFF = 0xCBF29CE484222325
PRIME = 1099511628211
M64 = (1 << 64) - 1
M63 = 1 << 63


def fnv1a64(v):
    h = OFF
    for _ in range(8):
        h = ((h ^ (v & 0xFF)) * PRIME) & M64
        v >>= 8
    return h


def display(h):
    return abs(struct.unpack('<q', struct.pack('<Q', h))[0])


def fmt(v):
    return 'user%019d' % v


def q2_bounds(t, n):
    # 显示键空间 [0,2^63) 上密度 N/2^63（Math.abs 折叠使键均匀落在下半空间），
    # 覆盖 t 行需长度 L = t*2^63//N；key_a 取现存键（display(fnv(k1))），
    # 保证 A+L < 2^63 使字符串区间与数值区间一致。
    # 边界: t 不能超过 n；t 接近 n 时 L 逼近 2^63，会令扫描条件永假（死循环），
    # 退化为覆盖约 99% 键空间的宽区间（count 略小于 n，探针以实测 count 为准）。
    t = min(t, n)
    L = t * M63 // n
    if L > M63 - M63 // 100:
        L = (M63 * 99) // 100
    k1 = 1
    while True:
        a = display(fnv1a64(k1))
        if 1 <= a and a + L < M63:
            return fmt(a), fmt(a + L)
        k1 += 1


def run_query(conn, sql):
    t0 = time.perf_counter()
    conn.request('POST', '/', body=sql.encode())
    r = conn.getresponse()
    body = r.read().decode()
    ms = (time.perf_counter() - t0) * 1000.0
    if r.status != 200:
        raise RuntimeError('HTTP %d: %s' % (r.status, body[:200]))
    return ms, body


def pct(xs, p):
    xs = sorted(xs)
    if not xs:
        return float('nan')
    idx = int(p * len(xs))
    if idx >= len(xs):
        idx = len(xs) - 1
    return xs[idx]


# 0) 就绪 + 行数
c = http.client.HTTPConnection(HOST, PORT, timeout=TIMEOUT)
try:
    c.request('GET', '/ping')
    r = c.getresponse()
    if r.status != 200:
        sys.exit('FAIL: /ping HTTP %d' % r.status)
    r.read()
    n = int(run_query(c, 'SELECT count() FROM %s' % TABLE)[1].strip())
finally:
    c.close()
print('count()=%d expected=%d' % (n, EXPECT_ROWS), file=sys.stderr)
if n != EXPECT_ROWS:
    print('WARN: count() != PROBE_EXPECT_ROWS，Q2 键界按实际行数计算，无碍；若行数异常请检查建表', file=sys.stderr)

# 1) 预热 page cache（全表 10 列长度聚合）
sum_expr = ('length(field0)+length(field1)+length(field2)+length(field3)+length(field4)'
            '+length(field5)+length(field6)+length(field7)+length(field8)+length(field9)')
pre = http.client.HTTPConnection(HOST, PORT, timeout=TIMEOUT)
try:
    scan_all = 'SELECT sum(%s) FROM %s' % (sum_expr, TABLE)
    for _ in range(PREWARM):
        run_query(pre, scan_all)
finally:
    pre.close()

# 2) 构造任务
q1_key = fmt(display(fnv1a64(Q1_KEYNUM)))
queries = [('Q1', "SELECT count(), sum(%s) FROM %s WHERE YCSB_KEY = '%s'" % (sum_expr, TABLE, q1_key))]
for t in Q2_ROWS:
    ka, kb = q2_bounds(t, n)
    queries.append(('Q2-%d' % t,
                    "SELECT count(), sum(%s) FROM %s WHERE YCSB_KEY >= '%s' AND YCSB_KEY < '%s'"
                    % (sum_expr, TABLE, ka, kb)))
for lim in Q3_LIMITS:
    queries.append(('Q3-%d' % lim,
                    'SELECT count() FROM (SELECT YCSB_KEY FROM %s LIMIT %d)' % (TABLE, lim)))
print('queries: %s' % ' '.join(q for q, _ in queries), file=sys.stderr)

# 3) 并发执行（每 worker 一个 keep-alive 连接）
tasks = queue.Queue()
results = {}
for qid, sql in queries:
    results[qid] = []
    for _ in range(WARM):
        tasks.put((qid, sql, True))
    for _ in range(ITERS):
        tasks.put((qid, sql, False))

lock = threading.Lock()
errs = []


def worker():
    conn = http.client.HTTPConnection(HOST, PORT, timeout=TIMEOUT)
    try:
        while True:
            item = tasks.get()
            if item is None:
                break
            qid, sql, warm = item
            try:
                ms, body = run_query(conn, sql)
            except Exception as e:  # noqa: BLE001
                with lock:
                    errs.append('%s: %s' % (qid, e))
                continue
            if not warm:
                with lock:
                    results[qid].append((ms, body))
    finally:
        conn.close()


threads = [threading.Thread(target=worker) for _ in range(max(1, CONC))]
for t in threads:
    t.start()
for _ in threads:
    tasks.put(None)
for t in threads:
    t.join()

# 4) 校验 count 一致性 + 写 tsv
out = open(OUTFILE, 'w')
for qid in sorted(results):
    ms_list = [ms for ms, _ in results[qid]]
    counts = set()
    for _, body in results[qid]:
        first = body.strip().split('\t', 1)[0].strip()
        try:
            counts.add(int(first))
        except ValueError:
            counts.add(first[:40])
    if len(counts) == 1:
        cnt = '%s' % counts.pop()
    else:
        cnt = 'MISMATCH:' + ','.join(str(x) for x in sorted(counts))
        print('WARN: qid=%s 各次 count 不一致: %s' % (qid, cnt), file=sys.stderr)
    for ms in ms_list:
        out.write('%s\t%.3f\t%s\n' % (qid, ms, cnt))
out.close()
print('qid           n   p50(ms)  p95(ms)  count', file=sys.stderr)
for qid in sorted(results):
    ms_list = sorted(ms for ms, _ in results[qid])
    if not ms_list:
        print('%-13s 0     n/a      n/a      -' % qid, file=sys.stderr)
        continue
    print('%-13s %-3d %-8.3f %-8.3f %s' % (qid, len(ms_list), pct(ms_list, 0.5), pct(ms_list, 0.95),
                                            next((b.strip().split('\t', 1)[0] for _, b in results[qid]), '-')),
          file=sys.stderr)
if errs:
    print('ERRORS:', file=sys.stderr)
    for e in errs[:10]:
        print('  ' + e, file=sys.stderr)
    sys.exit(2)
PY
}

http_post_sql() { # $1=SQL; 输出 "ms\tbody"（bash 降级: 单连接/请求）
  local sql=$1 t0 t1 out ms body
  t0=$(date +%s%N)
  out=$(timeout "$PROBE_TIMEOUT_SECONDS" bash -c '
    host=$1; port=$2; sql=$3
    exec 3<>"/dev/tcp/$host/$port" || exit 1
    printf "POST / HTTP/1.1\r\nHost: %s\r\nContent-Type: text/plain\r\nContent-Length: %s\r\nConnection: close\r\n\r\n%s" "$host" "${#sql}" "$sql" >&3
    cat <&3
  ' _ "$PROBE_HTTP_HOST" "$PROBE_HTTP_PORT" "$sql" 2>/dev/null) || { printf 'ERR\n'; return 1; }
  t1=$(date +%s%N)
  body=${out#*$'\r\n\r\n'}
  ms=$(( (t1 - t0) / 1000000 ))
  printf '%s\t%s\n' "$ms" "$body"
}

probe_bash_driver() { # $1=输出 tsv; 降级路径（顺序、无并发、固定键界按 12M 行）
  local outfile=$1
  : > "$outfile"
  local sum_expr cnt q1 q2a q2b q2c q3a q3b q3c r
  sum_expr='length(field0)+length(field1)+length(field2)+length(field3)+length(field4)+length(field5)+length(field6)+length(field7)+length(field8)+length(field9)'
  cnt=$(http_post_sql "SELECT count() FROM $PROBE_TABLE" | cut -f2 | tr -d '[:space:]')
  if [ "$cnt" != 12000000 ]; then
    warn "bash 降级路径的 Q2 键界按 count()=12000000 预置，当前 count()='$cnt' → Q2 覆盖行数不准；建议安装 python3 走主路径"
  fi
  q1="SELECT count(), sum($sum_expr) FROM $PROBE_TABLE WHERE YCSB_KEY = 'user0055488592825689361'"
  q2a="SELECT count(), sum($sum_expr) FROM $PROBE_TABLE WHERE YCSB_KEY >= 'user1820151046732198393' AND YCSB_KEY < 'user2588765383136763043'"
  q2b="SELECT count(), sum($sum_expr) FROM $PROBE_TABLE WHERE YCSB_KEY >= 'user1820151046732198393' AND YCSB_KEY < 'user5663222728755021646'"
  q2c="SELECT count(), sum($sum_expr) FROM $PROBE_TABLE WHERE YCSB_KEY >= 'user1000385178204227360' AND YCSB_KEY < 'user8686528542249873866'"
  q3a="SELECT count() FROM (SELECT YCSB_KEY FROM $PROBE_TABLE LIMIT 1000000)"
  q3b="SELECT count() FROM (SELECT YCSB_KEY FROM $PROBE_TABLE LIMIT 5000000)"
  q3c="SELECT count() FROM (SELECT YCSB_KEY FROM $PROBE_TABLE LIMIT 10000000)"
  # 预热 page cache
  for _ in $(seq 1 "$PROBE_PREWARM_RUNS"); do
    http_post_sql "SELECT sum($sum_expr) FROM $PROBE_TABLE" >/dev/null
  done
  local spec
  for spec in "Q1:$q1" "Q2-1000000:$q2a" "Q2-5000000:$q2b" "Q2-10000000:$q2c" "Q3-1000000:$q3a" "Q3-5000000:$q3b" "Q3-10000000:$q3c"; do
    local qid=${spec%%:*} sql=${spec#*:}
    local i
    for i in $(seq 1 "$((PROBE_WARMUP_ITERATIONS + PROBE_ITERATIONS))"); do
      r=$(http_post_sql "$sql")
      case "$r" in ERR*) warn "qid=$qid 请求失败: $r"; continue ;; esac
      if [ "$i" -gt "$PROBE_WARMUP_ITERATIONS" ]; then
        printf '%s\t%s\n' "$qid" "$r" >> "$outfile"
      fi
    done
  done
}

run_probe() { # $1=arm
  local arm=$1
  local outfile="$PROBE_OUT_DIR/probe-$arm.tsv"
  info "运行探针负载（臂 $arm, table=$PROBE_TABLE）..."
  if command -v python3 >/dev/null 2>&1; then
    probe_run_python "$outfile"
  else
    warn "无 python3，使用 bash /dev/tcp 降级路径（顺序执行、无 keep-alive、Q2 键界固定按 12M 行）"
    probe_bash_driver "$outfile"
  fi
  print_arm_summary "$outfile" "$arm"
}

print_arm_summary() { # $1=tsv $2=arm
  local tsv=$1 arm=$2
  if [ ! -s "$tsv" ]; then
    warn "臂 $arm 无测量记录"
    return 0
  fi
  info "臂 $arm 摘要（p50/p95, ms）:"
  awk -F'\t' '
    { key=$1; vals[key, ++n[key]]=$2; if (!(key in cnt)) cnt[key]=$3; else if (cnt[key] != $3) cnt[key]=cnt[key] "|" $3 }
    END {
      for (q in n) {
        N=n[q]
        for (i=1;i<=N;i++) for (j=i+1;j<=N;j++) if (vals[q,j] < vals[q,i]) { t=vals[q,i]; vals[q,i]=vals[q,j]; vals[q,j]=t }
        i50=int(N*0.5)+1; i95=int(N*0.95)+1; if (i95>N) i95=N; if (i50>N) i50=N
        printf "%s\t%d\t%.3f\t%.3f\t%s\n", q, N, vals[q,i50], vals[q,i95], cnt[q]
      }
    }' "$tsv" | sort | awk -F'\t' '{ printf "  %-12s n=%-3d p50=%-10.3f p95=%-10.3f count=%s\n", $1, $2, $3, $4, $5 }'
}

# ============ 汇总与 Gate ============
pct_of() { # $1=arm $2=qid $3=p(0.5/0.95); 输出该臂该查询的百分位（ms）
  local arm=$1 qid=$2 p=$3
  local f="$PROBE_OUT_DIR/probe-$arm.tsv"
  [ -s "$f" ] || { printf 'n/a'; return 0; }
  awk -F'\t' -v q="$qid" -v p="$p" '
    $1==q { vals[++n]=$2 }
    END {
      if (!n) { printf "n/a"; exit }
      for (i=1;i<=n;i++) for (j=i+1;j<=n;j++) if (vals[j] < vals[i]) { t=vals[i]; vals[i]=vals[j]; vals[j]=t }
      idx=int(p*n)+1; if (idx>n) idx=n; if (idx<1) idx=1
      printf "%.3f", vals[idx]
    }' "$f"
}

cross_arm_counts() { # 交叉校验各臂 count 一致（同一数据布局应逐字节相同）
  local qid ref c arm
  for qid in $qids; do
    ref=""
    for arm in A B C D; do
      [ -s "$PROBE_OUT_DIR/probe-$arm.tsv" ] || continue
      c=$(awk -F'\t' -v q="$qid" '$1==q { print $3; exit }' "$PROBE_OUT_DIR/probe-$arm.tsv")
      [ -z "$c" ] && continue
      if [ -z "$ref" ]; then
        ref=$c
      elif [ "$c" != "$ref" ]; then
        warn "qid=$qid 臂 $arm count=$c ≠ 首见 count=$ref（数据布局不一致？）"
      fi
    done
  done
}

gate_verdict() {
  local f_c="$PROBE_OUT_DIR/probe-C.tsv" f_d="$PROBE_OUT_DIR/probe-D.tsv"
  if [ ! -s "$f_c" ] || [ ! -s "$f_d" ]; then
    warn "缺少 C 或 D 臂数据，跳过 Gate 判定"
    return 0
  fi
  info "Gate: C（pin node2+3, 本地）vs D（pin node0/1, 远端）— Q2/Q3 服务端 p50 差≥10% 且 p95 同向 → 有研究必要"
  printf '  %-12s %10s %10s %9s %9s  %s\n' qid 'C p50' 'D p50' 'Δp50%' 'Δp95%' verdict
  local sensitive=0 qid
  for qid in $qids; do
    case "$qid" in Q2-*|Q3-*) ;; *) continue ;; esac
    local cp50 dp50 cp95 dp95 dp50pct dp95pct flag verdict
    cp50=$(pct_of C "$qid" 0.5); dp50=$(pct_of D "$qid" 0.5)
    cp95=$(pct_of C "$qid" 0.95); dp95=$(pct_of D "$qid" 0.95)
    if [ "$cp50" = n/a ] || [ "$dp50" = n/a ]; then
      printf '  %-12s 数据不足（C/D 至少一臂无记录）\n' "$qid"
      continue
    fi
    read -r dp50pct dp95pct <<< "$(awk -v a="$cp50" -v b="$dp50" -v c="$cp95" -v d="$dp95" \
      'BEGIN { if (a > 0 && c > 0) printf "%.1f %.1f", (b-a)/a*100, (d-c)/c*100; else printf "n/a n/a" }')"
    verdict=no
    if [ "$dp50pct" != n/a ]; then
      flag=$(awk -v x="$dp50pct" -v y="$dp95pct" 'BEGIN { x=x+0; y=y+0; ax=(x<0?-x:x); print (ax >= 10 && x*y > 0) ? 1 : 0 }')
      [ "$flag" = 1 ] && { verdict="SENSITIVE"; sensitive=1; }
    fi
    printf '  %-12s %10s %10s %8s%% %8s%%  %s\n' "$qid" "$cp50" "$dp50" "$dp50pct" "$dp95pct" "$verdict"
  done
  if [ "$sensitive" = 1 ]; then
    info "Gate 结论: 扫描/聚合受 CPU 亲和性影响、有研究必要 → 进入正式 A/B（read 0.90 + scan 0.10，见 docs/clickhouse-manual-testing.md §7.4）"
  else
    info "Gate 结论: 该负载形态无研究必要（Q2/Q3 均未达 p50 差≥10% 且 p95 同向），不加 scan"
  fi
}

summary() {
  info "============ 探针结果汇总（$PROBE_OUT_DIR） ============"
  qids=$(cut -f1 "$PROBE_OUT_DIR"/probe-[ABCD].tsv 2>/dev/null | sort -u)
  [ -z "$qids" ] && { warn "无任何臂的测量记录"; return 0; }
  cross_arm_counts
  info "各臂 p50（ms）:"
  printf '  %-12s' qid
  for arm in A B C D; do printf ' %11s' "p50-$arm"; done
  printf '\n'
  for qid in $qids; do
    printf '  %-12s' "$qid"
    for arm in A B C D; do printf ' %11s' "$(pct_of "$arm" "$qid" 0.5)"; done
    printf '\n'
  done
  info "各臂 p95（ms）:"
  printf '  %-12s' qid
  for arm in A B C D; do printf ' %11s' "p95-$arm"; done
  printf '\n'
  for qid in $qids; do
    printf '  %-12s' "$qid"
    for arm in A B C D; do printf ' %11s' "$(pct_of "$arm" "$qid" 0.95)"; done
    printf '\n'
  done
  gate_verdict
}

# ============ 恢复 ============
restore_ch() {
  info "恢复: 以原启动方式（无 numactl / 无 affinitygraph）重启 ClickHouse"
  cleanup_leftovers
  local log="/tmp/affinity-clickhouse-baseline.log"
  run_as_root rm -f "$log"
  ch_launch "$log"
  if [ "$DRY_RUN" = 1 ]; then
    return 0
  fi
  sleep 3
  local waited=0 ok=0
  while [ "$waited" -lt 120 ]; do
    if port_ready "$PROBE_HTTP_HOST" "$PROBE_HTTP_PORT"; then
      if command -v python3 >/dev/null 2>&1; then
        if python3 - "$PROBE_HTTP_HOST" "$PROBE_HTTP_PORT" <<'PY'; then ok=1; break; fi
import http.client
import sys

c = http.client.HTTPConnection(sys.argv[1], int(sys.argv[2]), timeout=5)
c.request('POST', '/', body=b'SELECT 1')
r = c.getresponse()
sys.exit(0 if r.status == 200 and r.read().decode().strip() == '1' else 1)
PY
      else
        ok=1; break  # 降级: 端口可连即视为就绪
      fi
    fi
    sleep 2
    waited=$((waited + 2))
  done
  if [ "$ok" = 1 ]; then
    info "ClickHouse 已恢复并确认 SELECT 1（HTTP $PROBE_HTTP_PORT）"
  else
    warn "恢复后未就绪，请检查日志 $log"
  fi
}

# ============ 主流程 ============
info "ClickHouse NUMA 探针: arms=$ARMS table=$PROBE_TABLE dry-run=$DRY_RUN"
[ "$DRY_RUN" = 1 ] || mkdir -p "$PROBE_OUT_DIR"

for arm in $ARMS; do
  info "######## 臂 $arm ########"
  cleanup_leftovers
  arm_launch "$arm" || { warn "臂 $arm 启动失败，跳过"; continue; }
  if [ "$DRY_RUN" = 1 ]; then
    info "dry-run: 跳过就绪等待与探针"
    continue
  fi
  wait_arm_ready "$arm" || { warn "臂 $arm 未就绪，跳过探针"; continue; }
  run_probe "$arm"
  if [ "$arm" = A ] && [ "$PROBE_PAUSE_AFTER_A" = 1 ]; then
    ans=""
    printf '[交互] A 臂完成，请检查 %s/probe-A.tsv（Q1 命中、Q2/Q3 行数符合预期、耗时量级合理）。回车继续 B/C/D，输入 q 退出并恢复: ' "$PROBE_OUT_DIR"
    IFS= read -r ans || ans=q
    case "$ans" in
      q|Q)
        info "用户选择退出；恢复原启动方式"
        restore_ch
        summary
        exit 0
        ;;
    esac
  fi
  cleanup_leftovers
done

restore_ch
summary
info "探针完成；结果与原始测量记录: $PROBE_OUT_DIR/probe-{A,B,C,D}.tsv"
