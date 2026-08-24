#!/usr/bin/env bash
# YCSB 多客户端压测脚本（JDBC binding）
#
# 用法:
#   tests/run-ycsb-bench.sh [--config FILE] [--clients N] [--threads N]
#                           [--rounds N] [--tag NAME] [--dry-run] [--help]
#
# 功能:
#   - 配置文件驱动: YCSB 环境 / workload / properties / 客户端矩阵 / 轮次等
#     全部集中在配置文件中（默认 tests/ycsb-bench.conf，参照 example 创建）。
#   - 多客户端: CLIENT_HOSTS 逗号分隔，local=本机，其余为 ssh 目标；多客户端
#     系统吞吐 = 各客户端 [OVERALL] Throughput(ops/sec) 之和。
#   - 多轮 + 汇总: 每轮输出各客户端吞吐与本轮合计，跨轮输出
#     mean / sample stddev / min / max；失败轮仍展示但不计入统计。
#   - 预热: WARMUP_OPS>0 时每轮正式测量前先跑一遍同矩阵（结果丢弃）。
#
# 参数优先级: --cli 参数 > 环境变量 > 配置文件 > 内置默认值
#
# 本脚本只负责压测负载，不管理数据库与 affinity-run（由 manual-scenarios.sh 负责）。
set -u

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

CONFIG_KEYS="YCSB_HOME YCSB_ENTRY PYTHON2_LIB WORKLOAD PROPERTIES JDBC_CP TABLE STATUS_INTERVAL EXTRA_YCSB_ARGS CLIENTS CLIENT_HOSTS THREADS_PER_CLIENT OPERATIONCOUNT_PER_CLIENT ROUNDS ROUND_INTERVAL_SECONDS WARMUP_OPS READY_HOST READY_PORT READY_TIMEOUT_SECONDS LOG_DIR RUN_TAG SSH_OPTIONS"

info() { printf '[INFO] %s\n' "$*"; }
warn() { printf '[WARN] %s\n' "$*"; }
die()  { printf '[FAIL] %s\n' "$*" >&2; exit 2; }

usage() {
  sed -n '2,16p' "$0"
  exit 0
}

# ---------- 参数解析 ----------
CONFIG_PATH=""
CLI_CLIENTS=""
CLI_THREADS=""
CLI_ROUNDS=""
CLI_TAG=""
DRY_RUN=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --config) CONFIG_PATH=${2:-}; shift 2 ;;
    --clients) CLI_CLIENTS=${2:-}; shift 2 ;;
    --threads) CLI_THREADS=${2:-}; shift 2 ;;
    --rounds) CLI_ROUNDS=${2:-}; shift 2 ;;
    --tag) CLI_TAG=${2:-}; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    --help|-h) usage ;;
    *) die "未知参数: $1（--help 查看用法）" ;;
  esac
done

CONFIG_PATH=${CONFIG_PATH:-$SCRIPT_DIR/ycsb-bench.conf}
[ -f "$CONFIG_PATH" ] || die "缺少配置文件: $CONFIG_PATH（可复制 tests/ycsb-bench.conf.example 后修改）"

# ---------- 加载配置: 环境变量 > 配置文件 ----------
declare -A ENV_OVERRIDES=()
for k in $CONFIG_KEYS; do
  if [ -n "${!k+x}" ]; then
    ENV_OVERRIDES["$k"]="${!k}"
  fi
done
# shellcheck disable=SC1090
if ! . "$CONFIG_PATH"; then die "配置文件解析失败: $CONFIG_PATH"; fi
for k in "${!ENV_OVERRIDES[@]}"; do
  printf -v "$k" '%s' "${ENV_OVERRIDES[$k]}"
done

# ---------- 内置默认值（仅当键未设置时生效） ----------
YCSB_HOME=${YCSB_HOME-/home/xhc/ycsb-jdbc-binding-0.17.0}
YCSB_ENTRY=${YCSB_ENTRY-$YCSB_HOME/bin/ycsb}
PYTHON2_LIB=${PYTHON2_LIB-/usr/local/python2.7/lib}
WORKLOAD=${WORKLOAD-workloads/workloada_clickhouse_numa_read}
PROPERTIES=${PROPERTIES-conf/db_183_clickhouse.properties}
JDBC_CP=${JDBC_CP-lib/mysql-connector-java-8.0.28.jar}
TABLE=${TABLE-usertable}
STATUS_INTERVAL=${STATUS_INTERVAL-10}
EXTRA_YCSB_ARGS=${EXTRA_YCSB_ARGS-}
CLIENTS=${CLIENTS-}
CLIENT_HOSTS=${CLIENT_HOSTS-}
THREADS_PER_CLIENT=${THREADS_PER_CLIENT-4}
OPERATIONCOUNT_PER_CLIENT=${OPERATIONCOUNT_PER_CLIENT-8000}
ROUNDS=${ROUNDS-3}
ROUND_INTERVAL_SECONDS=${ROUND_INTERVAL_SECONDS-10}
WARMUP_OPS=${WARMUP_OPS-0}
READY_HOST=${READY_HOST-127.0.0.1}
READY_PORT=${READY_PORT-9004}
READY_TIMEOUT_SECONDS=${READY_TIMEOUT_SECONDS-60}
LOG_DIR=${LOG_DIR-/tmp/ycsb-bench}
RUN_TAG=${RUN_TAG-}
SSH_OPTIONS=${SSH_OPTIONS--o BatchMode=yes -o ConnectTimeout=10}

# ---------- CLI 覆盖 ----------
[ -n "$CLI_CLIENTS" ] && CLIENTS=$CLI_CLIENTS
[ -n "$CLI_THREADS" ] && THREADS_PER_CLIENT=$CLI_THREADS
[ -n "$CLI_ROUNDS" ] && ROUNDS=$CLI_ROUNDS
[ -n "$CLI_TAG" ] && RUN_TAG=$CLI_TAG

# ---------- 校验 ----------
[ -n "$YCSB_HOME" ] || die "YCSB_HOME 不能为空"
validate_int() { # $1=name $2=value $3=min
  local name=$1 val=$2 min=${3:-1}
  [[ "$val" =~ ^[0-9]+$ ]] || die "$name 必须是正整数，当前: '$val'"
  [ "$val" -ge "$min" ] || die "$name 必须 >= $min，当前: '$val'"
}
validate_int THREADS_PER_CLIENT "$THREADS_PER_CLIENT" 1
validate_int ROUNDS "$ROUNDS" 1
validate_int STATUS_INTERVAL "$STATUS_INTERVAL" 1
validate_int ROUND_INTERVAL_SECONDS "$ROUND_INTERVAL_SECONDS" 0
validate_int WARMUP_OPS "$WARMUP_OPS" 0
validate_int READY_TIMEOUT_SECONDS "$READY_TIMEOUT_SECONDS" 1
[ -z "$OPERATIONCOUNT_PER_CLIENT" ] || validate_int OPERATIONCOUNT_PER_CLIENT "$OPERATIONCOUNT_PER_CLIENT" 1
[ -z "$READY_PORT" ] || validate_int READY_PORT "$READY_PORT" 1

# ---------- 客户端矩阵 ----------
declare -a HOSTS=()
if [ -n "$CLIENT_HOSTS" ]; then
  IFS=',' read -r -a HOSTS <<< "$CLIENT_HOSTS"
  derived=${#HOSTS[@]}
  [ "$derived" -ge 1 ] || die "CLIENT_HOSTS 条目数为 0"
  if [ -n "$CLIENTS" ] && [ "$CLIENTS" -ne "$derived" ]; then
    die "CLIENTS=$CLIENTS 与 CLIENT_HOSTS 条目数=$derived 不一致，请只保留一个"
  fi
  CLIENTS=$derived
else
  validate_int CLIENTS "$CLIENTS" 1
  for _ in $(seq 1 "$CLIENTS"); do HOSTS+=(local); done
fi
for h in "${HOSTS[@]}"; do
  [ -n "$h" ] || die "CLIENT_HOSTS 存在空条目"
  if [ "$h" != local ] && [ "$DRY_RUN" = 0 ]; then
    command -v ssh >/dev/null 2>&1 || die "检测到远程客户端 $h，但本机没有 ssh 命令"
  fi
done

# ---------- 运行目录 ----------
RUN_TAG=${RUN_TAG:-$(date +%Y%m%d-%H%M%S)}
[[ "$RUN_TAG" =~ ^[A-Za-z0-9_.-]+$ ]] || die "RUN_TAG 含非法字符: $RUN_TAG"
RUN_DIR="$LOG_DIR/$RUN_TAG"

info "配置: YCSB_HOME=$YCSB_HOME"
info "      负载: WORKLOAD=$WORKLOAD PROPERTIES=$PROPERTIES TABLE=$TABLE"
info "      矩阵: ${#HOSTS[@]} 客户端 × $THREADS_PER_CLIENT 线程 (hosts: ${HOSTS[*]})"
info "      轮次: $ROUNDS 预热: ${WARMUP_OPS} ops/client"
info "      日志: $RUN_DIR"

# ---------- 工具函数 ----------
sq() { printf '%s' "$1" | sed "s/'/'\\\\''/g"; }
q()  { printf "'%s'" "$(sq "$1")"; }

build_ycsb_cmd() { # $1=threads $2=opcount
  local t=$1 ops=$2
  local c="cd $(q "$YCSB_HOME") && "
  [ -n "$PYTHON2_LIB" ] && c+="LD_LIBRARY_PATH=$(q "$PYTHON2_LIB") "
  c+="$(q "$YCSB_ENTRY") run jdbc -s"
  c+=" -P $(q "$WORKLOAD")"
  c+=" -P $(q "$PROPERTIES")"
  c+=" -cp $(q "$JDBC_CP")"
  c+=" -p table=$(q "$TABLE")"
  c+=" -threads $t"
  [ -n "$ops" ] && c+=" -p operationcount=$ops"
  c+=" -p status.interval=$STATUS_INTERVAL"
  [ -n "$EXTRA_YCSB_ARGS" ] && c+=" $EXTRA_YCSB_ARGS"
  printf '%s' "$c"
}

ACTIVE_PIDS=()
cleanup() { # 中断时清理本脚本拉起的客户端
  [ "${#ACTIVE_PIDS[@]}" -gt 0 ] && kill "${ACTIVE_PIDS[@]}" 2>/dev/null
  exit 130
}
trap cleanup INT TERM

wait_ready() {
  if [ -z "$READY_PORT" ]; then
    warn "READY_PORT 为空，跳过就绪检查"
    return 0
  fi
  if [ "$DRY_RUN" = 1 ]; then
    printf '  + 将检查 %s:%s 就绪（最长 %ss）\n' "$READY_HOST" "$READY_PORT" "$READY_TIMEOUT_SECONDS"
    return 0
  fi
  info "等待 $READY_HOST:$READY_PORT 就绪（最长 ${READY_TIMEOUT_SECONDS}s）..."
  local waited=0
  while [ "$waited" -lt "$READY_TIMEOUT_SECONDS" ]; do
    if timeout 2 bash -c "exec 3<>/dev/tcp/$READY_HOST/$READY_PORT" 2>/dev/null; then
      info "就绪（${waited}s）"
      return 0
    fi
    sleep 2
    waited=$((waited + 2))
  done
  die "等待 $READY_HOST:$READY_PORT 超时（${READY_TIMEOUT_SECONDS}s）"
}

run_one_client_set() { # $1=日志目录 $2=opcount $3=阶段标签
  local dir=$1 ops=$2 label=$3
  local -a pids=()
  local i host cmd log
  [ "$DRY_RUN" = 0 ] && mkdir -p "$dir"
  for i in "${!HOSTS[@]}"; do
    host=${HOSTS[$i]}
    cmd=$(build_ycsb_cmd "$THREADS_PER_CLIENT" "$ops")
    log="$dir/client-$((i + 1)).log"
    if [ "$DRY_RUN" = 1 ]; then
      if [ "$host" = local ]; then
        printf '  + bash -c %s > %s 2>&1 &\n' "$cmd" "$log"
      else
        printf '  + ssh %s %s %s > %s 2>&1 &\n' "$SSH_OPTIONS" "$host" "$cmd" "$log"
      fi
      continue
    fi
    if [ "$host" = local ]; then
      [ -x "$YCSB_ENTRY" ] || die "本机客户端入口不存在: $YCSB_ENTRY"
      bash -c "$cmd" >"$log" 2>&1 &
    else
      # shellcheck disable=SC2086
      ssh $SSH_OPTIONS "$host" "$cmd" >"$log" 2>&1 &
    fi
    pids+=("$!")
  done
  [ "$DRY_RUN" = 1 ] && return 0
  ACTIVE_PIDS+=("${pids[@]}")
  local rc=0 pid
  for pid in "${pids[@]}"; do
    wait "$pid" || rc=1
  done
  ACTIVE_PIDS=()
  return "$rc"
}

client_metrics() { # $1=client 日志; 输出 "throughput errors"
  local log=$1 tput="" errs
  tput=$(grep -m1 '\[OVERALL\], Throughput(ops/sec),' "$log" 2>/dev/null | sed 's/^.*, *//' | tr -d ' ')
  errs=$(awk '
    /\[[A-Za-z-]+-FAILED\], Operations,/ { e++ }
    /Return=/ && $0 !~ /Return=OK,/ { e++ }
    /^\[ERROR\]/ { e++ }
    /Exception/ { e++ }
    END { print e+0 }' "$log" 2>/dev/null)
  printf '%s %s' "$tput" "$errs"
}

# ---------- 就绪检查 ----------
wait_ready

[ "$DRY_RUN" = 0 ] && mkdir -p "$RUN_DIR"
RESULTS_TSV="$RUN_DIR/results.tsv"
[ "$DRY_RUN" = 0 ] && : > "$RESULTS_TSV"

# ---------- 轮次执行 ----------
START_TS=$(date +%s)
FAILED_ROUNDS=0

for r in $(seq 1 "$ROUNDS"); do
  if [ "$DRY_RUN" = 1 ]; then
    printf '[INFO] 轮次 %s/%s\n' "$r" "$ROUNDS"
  else
    info "轮次 $r/$ROUNDS"
  fi
  round_fail=0

  if [ "$WARMUP_OPS" -gt 0 ]; then
    if [ "$DRY_RUN" = 1 ]; then
      printf '[INFO] 预热 %s ops/客户端...\n' "$WARMUP_OPS"
    else
      info "预热 ${WARMUP_OPS} ops/客户端..."
    fi
    if ! run_one_client_set "$RUN_DIR/warmup-r$r" "$WARMUP_OPS" warmup; then
      warn "轮次 $r 预热阶段有客户端非零退出，该轮标记失败"
      round_fail=1
    fi
  fi

  run_one_client_set "$RUN_DIR/round-$r" "$OPERATIONCOUNT_PER_CLIENT" real
  rc=$?
  [ "$rc" -ne 0 ] && { warn "轮次 $r 有客户端非零退出"; round_fail=1; }

  if [ "$DRY_RUN" = 1 ]; then
    [ "$r" -lt "$ROUNDS" ] && printf '  + 冷却 %ss 后进入下一轮\n' "$ROUND_INTERVAL_SECONDS"
    continue
  fi

  # 解析本轮各客户端吞吐
  per=()
  total=0
  ok=1
  for i in $(seq 0 $((CLIENTS - 1))); do
    log="$RUN_DIR/round-$r/client-$((i + 1)).log"
    read -r tput errs <<< "$(client_metrics "$log")"
    errs=${errs:-0}
    if [[ "$tput" =~ ^[0-9]+([.][0-9]+)?$ ]] && [[ "$errs" =~ ^[0-9]+$ ]] && [ "$errs" -eq 0 ]; then
      per+=("$tput")
      total=$(awk "BEGIN{printf \"%.2f\", $total+$tput}")
    else
      per+=("-")
      ok=0
      warn "客户端 $((i + 1)) 数据异常: throughput='$tput' errors='$errs'（日志: $log）"
      tail -5 "$log" 2>/dev/null >&2 || true
    fi
  done
  [ "$ok" -eq 0 ] && round_fail=1

  flag=""
  [ "$round_fail" -eq 1 ] && { flag="  [FAIL]"; FAILED_ROUNDS=$((FAILED_ROUNDS + 1)); }
  pc=""
  for i in "${!per[@]}"; do
    pc+="c$((i + 1))=${per[$i]} "
  done
  printf '%d\t%.2f\t%d\t%s\n' "$r" "$total" "$round_fail" "$(IFS=,; echo "${per[*]}")" >> "$RESULTS_TSV"
  printf '  round %-3d  %s total=%.2f ops/sec%s\n' "$r" "$pc" "$total" "$flag"
done

# ---------- 汇总 ----------
if [ "$DRY_RUN" = 1 ]; then
  printf '[INFO] dry-run 结束（未实际执行）\n'
  exit 0
fi

END_TS=$(date +%s)
ELAPSED=$((END_TS - START_TS))

{
  printf '=== YCSB 汇总 tag=%s (%d 客户端 × %d 线程, %d 轮) ===\n' "$RUN_TAG" "$CLIENTS" "$THREADS_PER_CLIENT" "$ROUNDS"
  printf 'round   '
  for i in $(seq 1 "$CLIENTS"); do printf 'client%-9s ' "$i"; done
  printf 'total(ops/sec)\n'
  while IFS=$'\t' read -r r t f clist; do
    IFS=',' read -r -a cvs <<< "$clist"
    printf '%-7s ' "$r"
    for i in $(seq 1 "$CLIENTS"); do
      printf '%-14s ' "${cvs[$((i - 1))]-}"
    done
    printf '%-14s%s\n' "$t" "$([ "$f" = 1 ] && printf ' [FAIL]')"
  done < "$RESULTS_TSV"
  printf '\n统计（不含失败轮）:\n'
  awk -F'\t' '
    $3==0 { s+=$2; a[c++]=$2; if (c==1) { mn=$2; mx=$2 } else { if ($2<mn) mn=$2; if ($2>mx) mx=$2 } }
    END {
      if (c==0) { print "  mean=n/a（无成功轮次）"; exit }
      mean=s/c
      if (c>1) { v=0; for (i=0;i<c;i++) v+=((a[i]-mean)^2); sd=sqrt(v/(c-1)) }
      printf "  mean=%.2f  stddev=%s  min=%.2f  max=%.2f  (n=%d)\n", mean, (c>1 ? sprintf("%.2f", sd) : "n/a"), mn, mx, c
    }' "$RESULTS_TSV"
  printf '总耗时: %ds\n' "$ELAPSED"
} | tee "$RUN_DIR/summary.txt"

if [ "$FAILED_ROUNDS" -gt 0 ]; then
  warn "共 $FAILED_ROUNDS 轮失败，详见 $RUN_DIR"
  exit 1
fi
info "完成，日志与汇总见 $RUN_DIR"
