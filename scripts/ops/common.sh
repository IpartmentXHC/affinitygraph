#!/usr/bin/env bash
set -euo pipefail

OPS_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$OPS_DIR/../.." && pwd)
OPS_CONFIG=${AFFINITYGRAPH_OPS_CONFIG:-"$PROJECT_ROOT/config/ops.toml"}

die() { echo "[FAIL] $*" >&2; exit 1; }
info() { echo "[INFO] $*"; }

ops_get() {
  local section=$1 key=$2
  awk -v wanted_section="$section" -v wanted_key="$key" '
    /^[[:space:]]*\[/ {
      current=$0
      gsub(/^[[:space:]]*\[|\][[:space:]]*$/, "", current)
      next
    }
    current == wanted_section && $0 ~ "^[[:space:]]*" wanted_key "[[:space:]]*=" {
      line=$0
      sub(/^[^=]*=[[:space:]]*/, "", line)
      sub(/[[:space:]]*#.*/, "", line)
      gsub(/^[[:space:]]*"|"[[:space:]]*$/, "", line)
      print line
      exit
    }
  ' "$OPS_CONFIG"
}

require_ops_config() { test -r "$OPS_CONFIG" || die "operations config is not readable: $OPS_CONFIG"; }

human_bytes() {
  if command -v numfmt >/dev/null 2>&1; then numfmt --to=iec-i --suffix=B "$1"; else echo "$1 bytes"; fi
}

plan_sha256() { sha256sum "$1" | awk '{print $1}'; }

check_plan_confirmation() {
  local plan=$1 expected=$2 actual
  test -f "$plan" || die "plan file is missing: $plan"
  actual=$(plan_sha256 "$plan")
  test -n "$expected" || die "--confirm-plan-sha256 is required with --apply"
  test "$actual" = "$expected" || die "plan SHA256 mismatch: expected=$expected actual=$actual"
}

format_time() { date '+%Y-%m-%d %H:%M:%S %z'; }

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
  case "${1:---help}" in
    --help|-h) echo 'common.sh is an internal library for scripts/ops entrypoints.' ;;
    --dry-run) echo 'common.sh has no standalone actions.' ;;
    *) die "unknown argument: $1" ;;
  esac
fi
