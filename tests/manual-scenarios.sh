#!/usr/bin/env bash
# 三场景手动测试自动化脚本（Doris / ClickHouse）
#
# 用法:
#   tests/manual-scenarios.sh --db doris|clickhouse [--scenario 1|2|3] [--dry-run]
#
# 三个场景:
#   1) 无 profile 文件的动态调度   (mode=active, solver 自行决策)
#   2) 有 profile 文件的动态调度   (profile 初始放置 + solver 增量微调)
#   3) 有 profile 文件的静态调度   (profile 放置后保持, dynamic=false, 可选零采样)
#
# 交互: 每个场景启动并校验就绪后, 提示按回车停止当前场景并进入下一场景,
#       输入 q 退出。设置 AUTO_NEXT_SECONDS>0 可无人值守自动推进。
# 校验: 脚本只负责启动与调度状态校验; 正式 YCSB 测量请在另一终端手动执行。
#
# 所有路径在下方 CONFIG 块, 默认值为 183 上的具体路径, 按需修改或通过
# 同名环境变量覆盖。非 root 用户运行时自动经 SUDO_ASKPASS + sudo -A 提权。
set -u

# ============ CONFIG（183 默认值，可用同名环境变量覆盖） ============
REPO=${REPO:-/home/xhc/affinitygraph}
BUILD=${BUILD:-$REPO/build}
BIN=${BIN:-$BUILD/affinity-run}
CTL=${CTL:-$BUILD/affinityctl}
BPF_OBJECT=${BPF_OBJECT:-$BUILD/affinitygraph.bpf.o}

TARGETS_DIR=${TARGETS_DIR:-/etc/affinitygraph/targets}
PROFILES_DIR=${PROFILES_DIR:-/etc/affinitygraph/profiles}
CALIBRATION_DIR=${CALIBRATION_DIR:-/etc/affinitygraph/calibration}

# 非 root 时的提权方式（183 的 askpass 脚本）
SUDO_ASKPASS=${SUDO_ASKPASS:-/home/xhc/ExperScript/doris-bench/askpass.sh}

# ---- Doris（183：数据为 root 属主，必须 --user root） ----
DORIS_HOME=${DORIS_HOME:-/home/xhc/doris/apache-doris-2.1.2-bin-arm64}
DORIS_FE_START=${DORIS_FE_START:-$DORIS_HOME/fe/bin/start_fe.sh}
DORIS_BE_START=${DORIS_BE_START:-$DORIS_HOME/be/bin/start_be.sh}
DORIS_PROFILE=${DORIS_PROFILE:-$PROFILES_DIR/doris-node2-dynamic.candidate.json}
DORIS_CPUS=${DORIS_CPUS:-64-95}
DORIS_QUIESCENT_WINDOWS=${DORIS_QUIESCENT_WINDOWS:-180}

# ---- ClickHouse（183：数据为 xhc 属主，禁止 --user root，否则 Code 430） ----
CLICKHOUSE_BIN=${CLICKHOUSE_BIN:-/home/xhc/clickhouse/ClickHouse/build/programs/clickhouse}
CLICKHOUSE_CONFIG=${CLICKHOUSE_CONFIG:-/home/xhc/clickhouse/etc/config.xml}
CLICKHOUSE_PROFILE=${CLICKHOUSE_PROFILE:-$PROFILES_DIR/clickhouse-threadpool-node2plus3.candidate.json}
CLICKHOUSE_CPUS=${CLICKHOUSE_CPUS:-64-127}
CLICKHOUSE_QUIESCENT_WINDOWS=${CLICKHOUSE_QUIESCENT_WINDOWS:-60}

READY_TIMEOUT_SECONDS=${READY_TIMEOUT_SECONDS:-300}   # 等待调度就绪的最长秒数
AUTO_NEXT_SECONDS=${AUTO_NEXT_SECONDS:-0}             # >0 时就绪后自动推进
SAMPLE_ZERO_STATIC=${SAMPLE_ZERO_STATIC:-1}           # 场景3 使用 sample_interval_seconds=0

# ============ 通用工具 ============
DRY_RUN=0
DB=""
SCENARIO=""

as_root() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  else
    SUDO_ASKPASS="$SUDO_ASKPASS" sudo -A "$@"
  fi
}

run() { # 非 dry-run 时执行，dry-run 时只打印
  if [ "$DRY_RUN" = 1 ]; then
    printf '  + %s\n' "$*"
  else
    "$@"
  fi
}

run_as_root() {
  if [ "$DRY_RUN" = 1 ]; then
    printf '  + [sudo] %s\n' "$*"
  else
    as_root "$@"
  fi
}

info() { printf '[INFO] %s\n' "$*"; }
warn() { printf '[WARN] %s\n' "$*"; }
die()  { printf '[FAIL] %s\n' "$*" >&2; exit 2; }

usage() {
  sed -n '2,14p' "$0"
  exit 0
}

# ============ 场景参数 ============
# 按 db+场景 返回: LOG_DIR SOCKET TOML PROFILE RUN_USER CMD...
db_config() { # $1=db $2=scenario
  case "$1" in
    doris)
      LOG_PREFIX="/var/log/affinitygraph-doris-s${2}"
      SOCKET="/tmp/affinitygraph-doris-s${2}.sock"
      RUN_USER=(--user root)
      CMD=(bash -c "$DORIS_FE_START --daemon && $DORIS_BE_START --daemon && wait")
      ;;
    clickhouse)
      LOG_PREFIX="/var/log/affinitygraph-clickhouse-s${2}"
      SOCKET="/tmp/affinitygraph-clickhouse-s${2}.sock"
      RUN_USER=()
      CMD=("$CLICKHOUSE_BIN" server --config-file "$CLICKHOUSE_CONFIG")
      ;;
  esac
  TOML="$TARGETS_DIR/${1}-s${2}.toml"
}

calibration_block() { # $1=db
  case "$1" in
    doris)
      cat <<'EOF'
[calibration]
id = "doris-manual-test"
activity_log_p95 = 1.0
sync_log_p95 = 1.0
share_log_p95 = 1.0
EOF
      ;;
    clickhouse)
      # 冻结的 ClickHouse 尺度（config/affinitygraph.toml 同源）
      cat <<'EOF'
[calibration]
id = "clickhouse-gate2-fixed-v2"
activity_log_p95 = 2.4138804290562152
sync_log_p95 = 2.5591179487485345
share_log_p95 = 0.00894730347830295
EOF
      ;;
  esac
}

write_toml() { # $1=db $2=scenario
  local db=$1 n=$2
  db_config "$db" "$n"
  local qw=1
  [ "$n" = 3 ] && qw=$([ "$db" = doris ] && echo "$DORIS_QUIESCENT_WINDOWS" || echo "$CLICKHOUSE_QUIESCENT_WINDOWS")
  if [ "$DRY_RUN" = 1 ]; then
    info "将生成配置 $TOML"
    return 0
  fi
  run_as_root mkdir -p "$TARGETS_DIR"
  run_as_root tee "$TOML" >/dev/null <<EOF
[runtime]
mode = "active"
sample_interval_seconds = $([ "$n" = 3 ] && [ "$SAMPLE_ZERO_STATIC" = 1 ] && echo 0 || echo 1)
$([ "$n" = 3 ] && printf 'dynamic = false\n')
$([ "$n" = 3 ] && printf 'static_quiescent_windows = %s\n' "$qw")
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
log_directory = "$LOG_PREFIX"
socket_path = "$SOCKET"

[resources]
calibration_path = "$CALIBRATION_DIR"

[collector]
required = true
pthread_uprobe = true

$(calibration_block "$db")
EOF
  info "已生成 $TOML"
}

ensure_profile() { # $1=db $2=scenario; 输出 profile 路径（空串=无 profile）
  local db=$1 n=$2 out=""
  case "$n" in
    1) out="" ;;
    2)
      local src dst
      if [ "$db" = doris ]; then src="$DORIS_PROFILE"; else src="$CLICKHOUSE_PROFILE"; fi
      dst="$PROFILES_DIR/${db}-s2.dynamic.json"
      if [ "$DRY_RUN" = 1 ]; then
        printf '[INFO] 将生成动态 profile %s（顶层 dynamic.enabled=true，源自 %s）\n' "$dst" "$src" >&2
      else
        run_as_root python3 - "$src" "$dst" <<'PY'
import json, sys
src, dst = sys.argv[1], sys.argv[2]
d = json.load(open(src))
d["dynamic"]["enabled"] = True
d["profile_id"] = d.get("profile_id", "dynamic") + "-s2-dynamic"
open(dst, "w").write(json.dumps(d, indent=2, ensure_ascii=False) + "\n")
PY
        printf '[INFO] 已生成动态 profile %s\n' "$dst" >&2
      fi
      out="$dst"
      ;;
    3)
      if [ "$db" = doris ]; then out="$DORIS_PROFILE"; else out="$CLICKHOUSE_PROFILE"; fi
      ;;
  esac
  printf '%s' "$out"
}

cleanup_leftovers() { # $1=db
  info "清理残留进程..."
  run_as_root pkill -x affinity-run 2>/dev/null || true
  sleep 2
  case "$1" in
    doris)
      run_as_root pkill -x java 2>/dev/null || true
      run_as_root pkill -x doris_be 2>/dev/null || true
      ;;
    clickhouse)
      run_as_root pkill -x clickhouse 2>/dev/null || true
      run_as_root pkill -x clckhouse-watch 2>/dev/null || true
      ;;
  esac
  sleep 3
}

launch_scenario() { # $1=db $2=scenario $3=profile
  local db=$1 n=$2 profile=$3
  db_config "$db" "$n"
  local run_log="/tmp/affinity-${db}-s${n}.log"
  info "启动场景 $n（$db）..."
  run_as_root rm -rf "$LOG_PREFIX"
  local args=("$BIN" run --config "$TOML")
  [ -n "$profile" ] && args+=(--thread-profile "$profile")
  args+=(--bpf-object "$BPF_OBJECT")
  [ ${#RUN_USER[@]} -gt 0 ] && args+=("${RUN_USER[@]}")
  args+=(-- "${CMD[@]}")
  info "affinity-run 命令: ${args[*]}"
  if [ "$DRY_RUN" = 1 ]; then
    printf '  + nohup %s > %s 2>&1 &\n' "${args[*]}" "$run_log"
    return 0
  fi
  if [ ! -x "$BIN" ]; then die "缺少 $BIN，先 make all"; fi
  if [ ! -f "$BPF_OBJECT" ]; then die "缺少 $BPF_OBJECT，先 make all"; fi
  as_root nohup "${args[@]}" > "$run_log" 2>&1 &
  sleep 3
  SUPERVISOR_PID=$(pgrep -x affinity-run | tail -1 || true)
  if [ -z "$SUPERVISOR_PID" ]; then
    warn "未发现 affinity-run 进程，启动日志:"
    as_root cat "$run_log" | tail -20
    return 1
  fi
  info "supervisor pid=$SUPERVISOR_PID socket=$SOCKET"
}

check_marker() { # $1=db $2=scenario $3=marker
  local db=$1 n=$2 m=$3
  local log="${LOG_PREFIX}/runtime.jsonl"
  local data
  data=$(as_root cat "$log" 2>/dev/null || true)
  case "$m" in
    runtime_start)              printf '%s' "$data" | grep -q '"type":"runtime_start"' ;;
    profile_load_success)       printf '%s' "$data" | grep '"type":"profile_load"' | tail -1 | grep -q '"success":true' ;;
    initial_affinity_committed) printf '%s' "$data" | grep '"type":"initial_affinity"' | tail -1 | grep -qE '"committed":[1-9]' ;;
    solve_window_end_any)       printf '%s' "$data" | grep -c '"type":"solve_window_end"' | grep -qv '^0$' ;;
    profile_static_hold)        printf '%s' "$data" | grep '"type":"solve_window_end"' | tail -1 | grep -q '"outcome":"profile_static_hold"' ;;
    sampling_stopped)           printf '%s' "$data" | grep -q '"type":"sampling_stopped"' ;;
    *) return 1 ;;
  esac
}

required_markers() { # $1=scenario
  case "$1" in
    1) echo "runtime_start solve_window_end_any" ;;
    2) echo "profile_load_success initial_affinity_committed solve_window_end_any" ;;
    3) echo "profile_load_success initial_affinity_committed profile_static_hold sampling_stopped" ;;
  esac
}

wait_ready() { # $1=db $2=scenario
  local db=$1 n=$2 marks ok=1
  marks=$(required_markers "$n")
  info "等待就绪标记（${READY_TIMEOUT_SECONDS}s 超时）: $marks"
  for i in $(seq 1 "$READY_TIMEOUT_SECONDS"); do
    ok=1
    for m in $marks; do
      if ! check_marker "$db" "$n" "$m"; then ok=0; break; fi
    done
    [ "$ok" = 1 ] && { info "场景 $n 就绪（${i}s）"; return 0; }
    if [ $((i % 15)) -eq 0 ]; then
      info "  等待中 ${i}s... 已见标记:"
      for m in $marks; do
        if check_marker "$db" "$n" "$m"; then printf '    [ok] %s\n' "$m"; else printf '    [..] %s\n' "$m"; fi
      done
    fi
    sleep 1
  done
  warn "场景 $n 超时未就绪（${READY_TIMEOUT_SECONDS}s）"
  return 1
}

verify_scenario() { # $1=db $2=scenario
  local db=$1 n=$2
  local log="${LOG_PREFIX}/runtime.jsonl"
  info "===== 场景 $n 状态校验（$db） ====="
  for m in $(required_markers "$n"); do
    if check_marker "$db" "$n" "$m"; then printf '  [ok] %s\n' "$m"; else printf '  [!!] %s 缺失\n' "$m"; fi
  done
  info "affinityctl status 关键字段:"
  as_root "$CTL" status --socket "$SOCKET" 2>/dev/null | python3 -c '
import sys, json
try:
    d = json.load(sys.stdin)
except Exception:
    print("  (status 不可解析)"); sys.exit(0)
for k in ["effective_mode","effective_dynamic","sampling_stopped","bpf","collector_degraded",
          "target_tgids","threads","pinned_threads","active_effective","solver_phase","paused","fatal_error"]:
    print(f"  {k}: {d.get(k)}")
' 2>/dev/null || as_root "$CTL" status --socket "$SOCKET" 2>/dev/null | head -5
  if [ "$n" -ge 2 ]; then
    info "profile 匹配线程掩码抽查（最近 3 个）:"
    local data tids t
    data=$(as_root cat "$log" 2>/dev/null || true)
    tids=$(printf '%s' "$data" | grep '"type":"profile_match"' | tail -3 | sed -E 's/.*"tid":([0-9]+).*/\1/')
    for t in $tids; do
      if [ -d "/proc/$t" ]; then
        local m c
        m=$(as_root sh -c "grep Cpus_allowed_list /proc/$t/status" 2>/dev/null || echo "n/a")
        c=$(as_root sh -c "cat /proc/$t/comm" 2>/dev/null || echo "?")
        printf '  tid=%s comm=%s %s\n' "$t" "$c" "$m"
      else
        printf '  tid=%s (已退出)\n' "$t"
      fi
    done
  fi
}

stop_scenario() { # $1=db $2=scenario
  local db=$1 n=$2
  local log="${LOG_PREFIX}/runtime.jsonl"
  info "停止场景 $n（$db）..."
  as_root "$CTL" pause --socket "$SOCKET" 2>/dev/null && info "  pause 已请求（同步恢复掩码）" || warn "  pause 失败/不可用（继续停止）"
  if [ -n "${SUPERVISOR_PID:-}" ]; then
    as_root kill -TERM "$SUPERVISOR_PID" 2>/dev/null || true
    for _ in $(seq 1 60); do
      as_root kill -0 "$SUPERVISOR_PID" 2>/dev/null || break
      sleep 1
    done
    as_root kill -0 "$SUPERVISOR_PID" 2>/dev/null && { warn "  supervisor 未退出，SIGKILL"; as_root kill -KILL "$SUPERVISOR_PID" 2>/dev/null || true; }
  fi
  sleep 3
  info "  runtime 收尾事件:"
  as_root sh -c "grep -E '\"type\":\"(pause|runtime_stop)\"' '$log' 2>/dev/null | tail -2" || true
  cleanup_leftovers "$db"
}

# ============ 主流程 ============
while [ $# -gt 0 ]; do
  case "$1" in
    --db) DB=$2; shift 2 ;;
    --scenario) SCENARIO=$2; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage ;;
    *) die "未知参数: $1（见 --help）" ;;
  esac
done

case "$DB" in
  doris|clickhouse) ;;
  "") die "必须指定 --db doris|clickhouse" ;;
  *) die "未知 db: $DB（仅支持 doris|clickhouse）" ;;
esac

if [ -n "$SCENARIO" ]; then
  case "$SCENARIO" in 1|2|3) ;; *) die "--scenario 仅支持 1|2|3" ;; esac
  SCENARIOS=$SCENARIO
else
  SCENARIOS="1 2 3"
fi

info "数据库: $DB  场景: $SCENARIOS  dry-run=$DRY_RUN"
for n in $SCENARIOS; do
  info "######## 场景 $n / 3 ########"
  write_toml "$DB" "$n"
  PROFILE=$(ensure_profile "$DB" "$n")
  if [ -n "$PROFILE" ]; then info "使用 profile: $PROFILE"; fi
  cleanup_leftovers "$DB"
  launch_scenario "$DB" "$n" "$PROFILE" || { warn "场景 $n 启动失败，跳过"; continue; }
  if [ "$DRY_RUN" = 1 ]; then
    info "dry-run：跳过就绪等待/校验/交互"
    continue
  fi
  wait_ready "$DB" "$n" || true
  verify_scenario "$DB" "$n"
  if [ "$AUTO_NEXT_SECONDS" -gt 0 ]; then
    info "AUTO_NEXT_SECONDS=${AUTO_NEXT_SECONDS}s 后自动进入下一场景"
    sleep "$AUTO_NEXT_SECONDS"
  else
    ans=""
    while true; do
      printf '[交互] 在另一终端手动执行 YCSB 测量。按回车停止当前场景并继续；输入 q 退出: '
      IFS= read -r ans || ans=q
      case "$ans" in
        ""|q|Q) break ;;
        *) printf '  仅接受回车（继续）或 q（退出）\n' ;;
      esac
    done
    if [ "$ans" = q ] || [ "$ans" = Q ]; then
      info "用户选择退出，清理后结束"
      stop_scenario "$DB" "$n"
      exit 0
    fi
  fi
  stop_scenario "$DB" "$n"
done
info "全部场景执行完毕"
