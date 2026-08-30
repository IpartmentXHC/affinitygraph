#!/usr/bin/env bash
# 构建 ClickHouse 大表 ycsb.usertable_big（键格式与 YCSB 逐字节一致，纯 SQL 建表）
#
# 背景: 105 MiB 小表点查的服务端成本被 JDBC 客户端开销淹没，测不到 NUMA 收益。
#       本脚本生成 ≤10GB 大表（默认 12M 行），键 =
#       "user" + %019d(Math.abs(fnvhash64(keynum)))，与 YCSB
#       CoreWorkload.buildKeyName + Utils.fnvhash64（FNV-1a 64：offset
#       0xCBF29CE484222325、prime 1099511628211、8 字节小端、末尾 Math.abs）
#       逐字节一致，供 tests/ch-numa-probe.sh 与正式 YCSB A/B 使用。
#       表磁盘占用按 system.parts.bytes_on_disk 校验，超过 BIG_TABLE_MAX_GB
#       时打印建议行数并非零退出（不自动重建）。
#
# 用法:
#   tests/ch-bigtable-build.sh [--dry-run] [--verify] [--drop] [--help]
#     --dry-run  只打印将要执行的 SQL，不连接 CH
#     --verify   抽样 5 个键与 Python 参考 diff + 1000 个随机键 IN 点查 count()=1000
#                （也可对已有表单独运行）
#     --drop     先 DROP TABLE IF EXISTS 再重建（默认表已存在且行数一致时跳过插入）
#
# 环境变量（均有默认值）:
#   CH_CLIENT               clickhouse client 可执行文件（默认 183 构建路径）
#   CH_HOST / CH_PORT       默认 127.0.0.1 / 9000（native 协议）
#   CH_DATABASE / CH_TABLE  默认 ycsb / usertable_big
#   BIG_TABLE_ROWS          默认 12000000
#   BIG_TABLE_MAX_GB        默认 10
#   CH_READY_TIMEOUT_SECONDS 默认 120（等待 CH 就绪）
set -u

# ============ CONFIG（183 默认值，可用同名环境变量覆盖） ============
CH_CLIENT=${CH_CLIENT:-/home/xhc/clickhouse/ClickHouse/build/programs/clickhouse client}
CH_HOST=${CH_HOST:-127.0.0.1}
CH_PORT=${CH_PORT:-9000}
CH_DATABASE=${CH_DATABASE:-ycsb}
CH_TABLE=${CH_TABLE:-usertable_big}
BIG_TABLE_ROWS=${BIG_TABLE_ROWS:-12000000}
BIG_TABLE_MAX_GB=${BIG_TABLE_MAX_GB:-10}
CH_READY_TIMEOUT_SECONDS=${CH_READY_TIMEOUT_SECONDS:-120}

DRY_RUN=0
VERIFY=0
DROP=0

info() { printf '[INFO] %s\n' "$*"; }
warn() { printf '[WARN] %s\n' "$*"; }
die()  { printf '[FAIL] %s\n' "$*" >&2; exit 2; }

usage() { sed -n '2,30p' "$0"; exit 0; }

while [ "$#" -gt 0 ]; do
  case "$1" in
    --dry-run) DRY_RUN=1; shift ;;
    --verify)  VERIFY=1; shift ;;
    --drop)    DROP=1; shift ;;
    -h|--help) usage ;;
    *) die "未知参数: $1（--help 查看用法）" ;;
  esac
done

[[ "$BIG_TABLE_ROWS" =~ ^[0-9]+$ ]] || die "BIG_TABLE_ROWS 必须是正整数: '$BIG_TABLE_ROWS'"
[[ "$BIG_TABLE_MAX_GB" =~ ^[0-9]+([.][0-9]+)?$ ]] || die "BIG_TABLE_MAX_GB 必须是数字: '$BIG_TABLE_MAX_GB'"

# CH_CLIENT 默认含 "client" 子命令（多调用 clickhouse 二进制必须显式子命令）；
# 独立 clickhouse-client 可设 CH_CLIENT=/path/clickhouse-client。
read -r -a CH_CLIENT_ARR <<< "$CH_CLIENT"
CH_CMD=("${CH_CLIENT_ARR[@]}" --host "$CH_HOST" --port "$CH_PORT" --database "$CH_DATABASE")
CH_CLIENT_BIN=${CH_CLIENT_ARR[0]:-}

if [ "$DRY_RUN" = 0 ]; then
  [ -x "$CH_CLIENT_BIN" ] || die "CH_CLIENT 不存在或不可执行: $CH_CLIENT_BIN（CH_CLIENT=$CH_CLIENT）"
fi

chq() { # $1=SQL; 非 dry-run 执行并输出结果（去首尾空白）；dry-run 只打印
  local sql=$1
  if [ "$DRY_RUN" = 1 ]; then
    printf '  + %s --query %q\n' "${CH_CMD[*]}" "$sql"
    return 0
  fi
  "${CH_CMD[@]}" --query "$sql" 2>&1
}

wait_ready() {
  info "等待 $CH_HOST:$CH_PORT 就绪（最长 ${CH_READY_TIMEOUT_SECONDS}s）..."
  if [ "$DRY_RUN" = 1 ]; then
    printf '  + 轮询 %s --query "SELECT 1"\n' "${CH_CMD[*]}"
    return 0
  fi
  local waited=0
  while [ "$waited" -lt "$CH_READY_TIMEOUT_SECONDS" ]; do
    if "${CH_CMD[@]}" --query "SELECT 1" >/dev/null 2>&1; then
      info "就绪（${waited}s）"
      return 0
    fi
    sleep 2
    waited=$((waited + 2))
  done
  die "等待 ClickHouse 就绪超时；请先启动 CH（见 docs/clickhouse-manual-testing.md）"
}

# ---------- FNV-1a 64 键表达式（纯 SQL） ----------
# 逐句对应 YCSB Utils.fnvhash64: 8 字节小端，每字节 (h ^= byte) * prime (mod 2^64)，
# 末尾 Math.abs((long) h8)。UInt64 算术按 C++ 回绕，与 Java long 行为一致。
fnv_with_chain() { # 输出 WITH 别名链（输入列: numbers.number）
  {
    printf 'WITH\n  toUInt64(number) AS n0,\n  toUInt64(0xCBF29CE484222325) AS h0'
    local i
    for i in $(seq 0 7); do
      printf ',\n  bitAnd(n%s, toUInt64(255)) AS b%s' "$i" "$i"
      printf ',\n  toUInt64(bitXor(h%s, b%s) * toUInt64(1099511628211)) AS h%s' "$i" "$i" "$((i + 1))"
      printf ',\n  intDiv(n%s, toUInt64(256)) AS n%s' "$i" "$((i + 1))"
    done
    printf '\n'
  }
}

fnv_key_expr() { # 输出键表达式（引用 h8）; Java Math.abs(Long.MIN_VALUE) 边界在 N≤2^63 时不可达
  printf "concat('user', leftPad(toString(abs(toInt64(h8))), 19, '0'))"
}

create_table_sql() {
  cat <<EOF
CREATE TABLE IF NOT EXISTS ${CH_DATABASE}.${CH_TABLE} (
  YCSB_KEY String,
  field0 String,
  field1 String,
  field2 String,
  field3 String,
  field4 String,
  field5 String,
  field6 String,
  field7 String,
  field8 String,
  field9 String
) ENGINE = MergeTree
PRIMARY KEY YCSB_KEY
ORDER BY YCSB_KEY
SETTINGS index_granularity = 8192
EOF
}

insert_sql() {
  cat <<EOF
INSERT INTO ${CH_DATABASE}.${CH_TABLE} (YCSB_KEY, field0, field1, field2, field3, field4, field5, field6, field7, field8, field9)
SELECT
  $(fnv_key_expr) AS YCSB_KEY,
  randomPrintableASCII(100) AS field0,
  randomPrintableASCII(100) AS field1,
  randomPrintableASCII(100) AS field2,
  randomPrintableASCII(100) AS field3,
  randomPrintableASCII(100) AS field4,
  randomPrintableASCII(100) AS field5,
  randomPrintableASCII(100) AS field6,
  randomPrintableASCII(100) AS field7,
  randomPrintableASCII(100) AS field8,
  randomPrintableASCII(100) AS field9
FROM (
  $(fnv_with_chain)
  SELECT h8
  FROM numbers(${BIG_TABLE_ROWS})
)
EOF
}

count_sql() { printf 'SELECT count() FROM %s.%s' "$CH_DATABASE" "$CH_TABLE"; }

size_sql() {
  printf "SELECT sum(bytes_on_disk) FROM system.parts WHERE database = '%s' AND table = '%s' AND active" "$CH_DATABASE" "$CH_TABLE"
}

# ---------- 键正确性校验（--verify） ----------
verify_keys() { # $1=期望行数 N
  command -v python3 >/dev/null 2>&1 || { warn "无 python3，跳过 --verify"; return 0; }
  local n=$1 tmp
  tmp=$(mktemp -d)
  python3 - "$CH_DATABASE" "$CH_TABLE" "$n" "$tmp" <<'PY' || { warn "Python 参考生成失败，跳过 --verify"; rm -rf "$tmp"; return 0; }
import random
import struct
import sys

db, table, n, tmp = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
OFF = 0xCBF29CE484222325
PRIME = 1099511628211
M64 = (1 << 64) - 1


def fnv1a64(v):
    h = OFF
    for _ in range(8):
        h = ((h ^ (v & 0xFF)) * PRIME) & M64
        v >>= 8
    return h


def key_of(num):
    s = struct.unpack('<q', struct.pack('<Q', fnv1a64(num)))[0]
    return 'user%019d' % abs(s)


sample_nums = [1, 2, 42, 12345, 999999]
sample_keys = [key_of(k) for k in sample_nums]
open(tmp + '/ref.sql', 'w').write(
    "SELECT YCSB_KEY FROM %s.%s WHERE YCSB_KEY IN (%s) ORDER BY YCSB_KEY\n"
    % (db, table, ','.join("'%s'" % k for k in sample_keys)))
open(tmp + '/ref.keys', 'w').write('\n'.join(sorted(sample_keys)) + '\n')

rng = random.Random(20260824)
rand_keys = [key_of(k) for k in rng.sample(range(n), 1000)]
open(tmp + '/rand.sql', 'w').write(
    "SELECT count() FROM %s.%s WHERE YCSB_KEY IN (%s)\n"
    % (db, table, ','.join("'%s'" % k for k in rand_keys)))
PY
  if [ "$DRY_RUN" = 1 ]; then
    info "verify: 打印将执行的核对 SQL（dry-run 不执行）"
    sed 's/^/  + /' "$tmp/ref.sql" "$tmp/rand.sql"
    rm -rf "$tmp"
    return 0
  fi

  local rc=0 out got
  out=$(chq "$(cat "$tmp/ref.sql")" | tr '\t' '\n' | sort)
  got=$(cat "$tmp/ref.keys")
  if [ "$out" = "$got" ]; then
    info "[ok] 抽样 5 键与 Python 参考一致"
  else
    warn "[!!] 抽样键与参考不一致（diff 前 10 行）:"
    diff <(printf '%s\n' "$out") "$tmp/ref.keys" | head -10
    rc=1
  fi
  local hit
  hit=$(chq "$(cat "$tmp/rand.sql")" | tr -d '[:space:]')
  if [ "$hit" = 1000 ]; then
    info "[ok] 1000 随机键 IN 点查 count()=$hit（命中率 100%）"
  else
    warn "[!!] 1000 随机键 IN 点查 count()=$hit（期望 1000）"
    rc=1
  fi
  rm -rf "$tmp"
  return "$rc"
}

# ============ 主流程 ============
info "ClickHouse 大表构建: ${CH_DATABASE}.${CH_TABLE}  rows=$BIG_TABLE_ROWS  max=${BIG_TABLE_MAX_GB}GB  dry-run=$DRY_RUN"
wait_ready

if [ "$DROP" = 1 ]; then
  info "DROP TABLE IF EXISTS ${CH_DATABASE}.${CH_TABLE}"
  chq "DROP TABLE IF EXISTS ${CH_DATABASE}.${CH_TABLE}"
fi

info "建库建表（已存在则跳过）"
chq "CREATE DATABASE IF NOT EXISTS ${CH_DATABASE}"
chq "$(create_table_sql)"

if [ "$DRY_RUN" = 1 ]; then
  info "dry-run: 打印插入 SQL 后结束"
  printf '  + 插入 SQL:\n'
  sed 's/^/    /' <<< "$(insert_sql)"
  [ "$VERIFY" = 1 ] && verify_keys "$BIG_TABLE_ROWS"
  exit 0
fi

cur=$(chq "$(count_sql)" | tr -d '[:space:]')
[[ "$cur" =~ ^[0-9]+$ ]] || die "count() 查询失败: '$cur'"
if [ "$cur" = "$BIG_TABLE_ROWS" ]; then
  info "表已存在且行数=$cur，跳过插入"
elif [ "$cur" != 0 ]; then
  die "表已有 $cur 行（非 0 且非目标 $BIG_TABLE_ROWS）→ 疑似不完整构建；请 --drop 重跑"
else
  info "开始插入 $BIG_TABLE_ROWS 行（耗时取决于磁盘/CPU/压缩）..."
  chq "$(insert_sql)"
  cur=$(chq "$(count_sql)" | tr -d '[:space:]')
  [[ "$cur" =~ ^[0-9]+$ ]] || die "插入后 count() 查询失败: '$cur'"
  [ "$cur" = "$BIG_TABLE_ROWS" ] || die "插入后 count()=$cur ≠ $BIG_TABLE_ROWS"
  info "count()=$cur ✓"
fi

bytes=$(chq "$(size_sql)" | tr -d '[:space:]')
[[ "$bytes" =~ ^[0-9]+$ ]] || die "bytes_on_disk 查询失败: '$bytes'"
max_bytes=$(awk -v g="$BIG_TABLE_MAX_GB" 'BEGIN{printf "%d", g*1024*1024*1024}')
if [ "$bytes" -gt "$max_bytes" ]; then
  sug=$(awk -v b="$bytes" -v m="$max_bytes" -v n="$BIG_TABLE_ROWS" 'BEGIN{printf "%d", n*m/b}')
  gb=$(awk -v b="$bytes" 'BEGIN{printf "%.2f", b/1024/1024/1024}')
  warn "表磁盘占用 ${gb}GB（$bytes 字节）超过 BIG_TABLE_MAX_GB=$BIG_TABLE_MAX_GB"
  warn "建议: 以 BIG_TABLE_ROWS=$sug 重新构建（tests/ch-bigtable-build.sh --drop）"
  exit 2
fi
info "磁盘占用 $(awk -v b="$bytes" 'BEGIN{printf "%.2f", b/1024/1024/1024}')GB ≤ ${BIG_TABLE_MAX_GB}GB ✓"

if [ "$VERIFY" = 1 ]; then
  verify_keys "$cur"
fi
info "完成: ${CH_DATABASE}.${CH_TABLE}  rows=$cur  ${BIG_TABLE_MAX_GB}GB 约束内"
