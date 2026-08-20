#!/usr/bin/env bash
# 从方案一（无 profile 动态调度）的观测结果，在新机器上生成静态 profile。
#
# 用法:
#   tests/export-profile.sh --db doris|clickhouse \
#     --socket /tmp/affinitygraph-<db>-s1.sock \
#     --log /var/log/affinitygraph-<db>-s1/runtime.jsonl \
#     --template /etc/affinitygraph/profiles/<old>.candidate.json \
#     --allowed-cpus <新机器目标 node CPU 集合> \
#     --output /etc/affinitygraph/profiles/<new>.candidate.json \
#     [--profile-id ID] [--preflight-config PATH] [--no-preflight] [--dry-run]
#
# 背景: profile 的 count 只是放置上限(超出不放置), CPU 集合必须落在新机器
# 资源信封内(越界 preflight 会拒绝)。本脚本保留 --template 的 comm 白名单
# (线程组不变), 用 affinityctl status 的 planned_assignments/planned_masks
# 观测结果重算 affinities[].cpus 与 count, 生成 dynamic.enabled=false 的
# candidate profile, 并自动跑 preflight 校验。
#
# 所有路径默认 183 值, 可用同名环境变量覆盖; 非 root 时经 SUDO_ASKPASS + sudo -A 提权。
set -u

# ============ CONFIG（默认值，可用同名环境变量覆盖） ============
REPO=${REPO:-/home/xhc/affinitygraph}
BUILD=${BUILD:-$REPO/build}
BIN=${BIN:-$BUILD/affinity-run}
CTL=${CTL:-$BUILD/affinityctl}
BPF_OBJECT=${BPF_OBJECT:-$BUILD/affinitygraph.bpf.o}
SUDO_ASKPASS=${SUDO_ASKPASS:-/home/xhc/ExperScript/doris-bench/askpass.sh}

as_root() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  else
    SUDO_ASKPASS="$SUDO_ASKPASS" sudo -A "$@"
  fi
}

info() { printf '[INFO] %s\n' "$*"; }
warn() { printf '[WARN] %s\n' "$*"; }
die()  { printf '[FAIL] %s\n' "$*" >&2; exit 2; }

usage() {
  sed -n '2,16p' "$0"
  exit 0
}

# ============ 参数 ============
DB=""
SOCKET=""
LOG=""
TEMPLATE=""
ALLOWED_CPUS=""
OUTPUT=""
PROFILE_ID=""
PREFLIGHT_CONFIG=${PREFLIGHT_CONFIG:-$REPO/config/affinitygraph.toml}
NO_PREFLIGHT=0
DRY_RUN=0

while [ $# -gt 0 ]; do
  case "$1" in
    --db) DB=$2; shift 2 ;;
    --socket) SOCKET=$2; shift 2 ;;
    --log) LOG=$2; shift 2 ;;
    --template) TEMPLATE=$2; shift 2 ;;
    --allowed-cpus) ALLOWED_CPUS=$2; shift 2 ;;
    --output) OUTPUT=$2; shift 2 ;;
    --profile-id) PROFILE_ID=$2; shift 2 ;;
    --preflight-config) PREFLIGHT_CONFIG=$2; shift 2 ;;
    --no-preflight) NO_PREFLIGHT=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage ;;
    *) die "未知参数: $1（见 --help）" ;;
  esac
done

case "$DB" in doris|clickhouse) ;; "") die "必须指定 --db doris|clickhouse" ;; *) die "未知 db: $DB" ;; esac
[ -n "$SOCKET" ] || die "必须指定 --socket"
[ -n "$LOG" ] || die "必须指定 --log"
[ -n "$TEMPLATE" ] || die "必须指定 --template"
[ -n "$ALLOWED_CPUS" ] || die "必须指定 --allowed-cpus（新机器目标 node 的 CPU 集合）"
[ -n "$OUTPUT" ] || die "必须指定 --output"

if [ "$DRY_RUN" = 1 ]; then
  info "dry-run：以下为将执行的步骤"
  info "  1) ${CTL} status --socket ${SOCKET}  （读取 planned_assignments/planned_masks）"
  info "  2) 聚合观测: 模板 ${TEMPLATE} 的 comm 白名单, /proc/<tid>/comm 兜底 ${LOG} 的 thread_name"
  info "  3) 生成静态 profile → ${OUTPUT}（allowed_cpus=${ALLOWED_CPUS}, profile_id=${PROFILE_ID:-auto}）"
  if [ "$NO_PREFLIGHT" = 1 ]; then
    info "  4) 跳过 preflight（--no-preflight）"
  else
    info "  4) preflight --config ${PREFLIGHT_CONFIG} --thread-profile ${OUTPUT} --bpf-object ${BPF_OBJECT}"
  fi
  exit 0
fi

[ -x "$CTL" ] || die "缺少 $CTL，先 make all"
[ -r "$TEMPLATE" ] || die "模板 profile 不存在或不可读: $TEMPLATE"
[ -r "$LOG" ] || die "runtime 日志不存在或不可读: $LOG"

info "读取 affinityctl status（socket=$SOCKET）..."
status_json=$(as_root "$CTL" status --socket "$SOCKET" 2>/dev/null || true)
if [ -z "$status_json" ]; then
  die "无法读取 affinityctl status（--socket $SOCKET）；请确认场景 1 正在运行"
fi

info "聚合观测并生成 profile..."
status_file=$(mktemp /tmp/affinity-export-status.XXXXXX)
printf '%s' "$status_json" > "$status_file"
as_root python3 - "$TEMPLATE" "$ALLOWED_CPUS" "$OUTPUT" "$PROFILE_ID" "$LOG" "$status_file" <<'PY'
import json, re, sys, datetime, socket as pysocket

template_path, allowed_cpus_s, output_path, profile_id, log_path, status_path = sys.argv[1:7]
status = json.load(open(status_path))

def parse_cpus(s):
    out = []
    for part in s.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-")
            out += list(range(int(a), int(b) + 1))
        else:
            out.append(int(part))
    return sorted(set(out))

def fmt_cpus(cpus):
    cpus = sorted(set(cpus))
    if not cpus:
        return ""
    parts, start, prev = [], cpus[0], cpus[0]
    for c in cpus[1:]:
        if c == prev + 1:
            prev = c
            continue
        parts.append(str(start) if start == prev else f"{start}-{prev}")
        start = prev = c
    parts.append(str(start) if start == prev else f"{start}-{prev}")
    return ",".join(parts)

obs = {}
for tid_s, cpu in (status.get("planned_assignments") or {}).items():
    obs.setdefault(int(tid_s), []).append(int(cpu))
for tid_s, mask in (status.get("planned_masks") or {}).items():
    obs.setdefault(int(tid_s), []).extend(parse_cpus(mask))
if not obs:
    sys.stderr.write("[FAIL] planned_assignments/planned_masks 均为空：solver 尚无放置，请等 workload 稳定后重试\n")
    sys.exit(2)

def read_comm(tid):
    try:
        with open(f"/proc/{tid}/comm") as f:
            return f.read().strip()
    except OSError:
        return None

name_by_tid = {}
try:
    with open(log_path) as f:
        for line in f:
            if '"type":"thread_name"' not in line:
                continue
            try:
                d = json.loads(line)
                name_by_tid[d["tid"]] = d.get("name", "")
            except Exception:
                pass
except OSError:
    pass

def comm_of(tid):
    return read_comm(tid) or name_by_tid.get(tid) or f"tid-{tid}"

template = json.load(open(template_path))
rules = []
for r in template.get("placements", []):
    m = r.get("match", {}) or {}
    if m.get("comm"):
        rules.append((r.get("id", "rule"), "comm", m["comm"]))
    elif m.get("comm_prefix"):
        rules.append((r.get("id", "rule"), "prefix", m["comm_prefix"]))
    else:
        sys.stderr.write(f"[WARN] 跳过模板规则 {r.get('id')}: 无 comm/comm_prefix，无法按线程组聚合\n")

def rule_match(rule, comm):
    kind, val = rule[1], rule[2]
    return comm == val if kind == "comm" else comm.startswith(val)

groups = {}
for tid in obs:
    c = comm_of(tid)
    for r in rules:
        if rule_match(r, c):
            groups.setdefault(r[0], []).append(tid)
            break
if not groups:
    sys.stderr.write("[FAIL] 没有观测到模板白名单内的线程组（comm）；请确认 workload 覆盖目标线程组\n")
    sys.exit(2)

allowed = parse_cpus(allowed_cpus_s)
if not allowed:
    sys.stderr.write(f"[FAIL] --allowed-cpus 解析为空: {allowed_cpus_s}\n")
    sys.exit(2)

placements = []
for rid, kind, val in rules:
    tids = groups.get(rid, [])
    if not tids:
        sys.stderr.write(f"[WARN] 模板规则 {rid} 未观测到线程，跳过（不生成空规则）\n")
        continue
    cpus = sorted({c for tid in tids for c in obs[tid]})
    if not set(cpus).issubset(set(allowed)):
        sys.stderr.write(f"[FAIL] 规则 {rid} 观测 CPU {fmt_cpus(cpus)} 超出 --allowed-cpus {allowed_cpus_s}\n")
        sys.exit(2)
    match = {"comm": None, "comm_prefix": None, "cgroup": None, "cgroup_prefix": None, "tid": None}
    if kind == "comm":
        match["comm"] = val
    else:
        match["comm_prefix"] = val
    placements.append({
        "id": rid,
        "match": match,
        "allowed_cpus": fmt_cpus(allowed),
        "dynamic": False,
        "affinities": [{"cpus": fmt_cpus(cpus), "count": len(tids)}],
    })

now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
pid = profile_id or f"exported-{pysocket.gethostname()}-r1"
profile = {
    "schema_version": 1,
    "profile_id": pid,
    "generated_at": now,
    "status": "candidate",
    "source": {"commit": "", "experiment_id": pid, "test_id": "r1"},
    "applicability": {
        "description": "exported from scenario-1 dynamic observation (template %s)" % template_path,
        "similarity": {"metric": "", "threshold": None, "reported_gap": None},
    },
    "dynamic": {"enabled": False, "small_step_threads": 1,
                "large_change_ratio": 0.3, "large_step_threads": 4,
                "cooldown_seconds": 10},
    "placements": placements,
}
with open(output_path, "w") as f:
    json.dump(profile, f, indent=2, ensure_ascii=False)
    f.write("\n")
for p in placements:
    comm = p["match"]["comm"] or p["match"]["comm_prefix"]
    print(f"[INFO]   rule={p['id']}  comm={comm}  cpus={p['affinities'][0]['cpus']}  count={p['affinities'][0]['count']}")
print(f"[INFO] 已生成 {output_path}: {len(placements)} rule(s), profile_id={pid}")
PY
rc=$?
rm -f "$status_file"
if [ "$rc" != 0 ]; then
  die "聚合/生成失败（python 退出码 $rc），未生成或仅生成部分内容"
fi

if [ "$NO_PREFLIGHT" = 1 ]; then
  info "跳过 preflight（--no-preflight）；请手动运行:"
  info "  sudo -A $BIN preflight --config $PREFLIGHT_CONFIG --thread-profile $OUTPUT --bpf-object $BPF_OBJECT"
  exit 0
fi
if [ ! -f "$PREFLIGHT_CONFIG" ] || [ ! -f "$BPF_OBJECT" ]; then
  warn "缺少 preflight 配置或 bpf 对象（config=$PREFLIGHT_CONFIG bpf=$BPF_OBJECT），跳过自动校验；请手动运行 preflight"
  exit 0
fi
info "运行 preflight 校验:"
as_root "$BIN" preflight --config "$PREFLIGHT_CONFIG" \
  --thread-profile "$OUTPUT" \
  --bpf-object "$BPF_OBJECT" \
  || warn "preflight 返回非零，请检查上方输出（常见：CPU 信封不匹配/格式错误）"
