#!/usr/bin/env bash
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() {
  cat <<'EOF'
Usage: env-check.sh [--dry-run] [--help]

Check the local AffinityGraph workspace and the configured cloud build host.
The command never starts a database or an AffinityGraph runtime.
EOF
}

dry_run=false
while (($#)); do
  case "$1" in
    --dry-run) dry_run=true ;;
    --help|-h) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
  shift
done

require_ops_config
cloud_host=$(ops_get cloud host)
cloud_root=$(ops_get cloud project_root)
askpass_config=$(ops_get sudo askpass_config)
failures=0

if $dry_run; then
  echo "Would check local project: $PROJECT_ROOT"
  echo "Would check cloud host: $cloud_host"
  echo "Would validate askpass through: $askpass_config"
  exit 0
fi

pass() { echo "[PASS] $*"; }
warn() { echo "[WARN] $*"; }
fail() { echo "[FAIL] $*"; failures=$((failures + 1)); }

test -d "$PROJECT_ROOT" && pass "local project exists: $PROJECT_ROOT" || fail "local project missing"
test -f "$PROJECT_ROOT/Makefile" && pass "local Makefile exists" || fail "local Makefile missing"
command -v make >/dev/null 2>&1 && pass "local make exists" || fail "local make missing"
test -d "$PROJECT_ROOT/tests" && pass "local tests directory exists" || fail "local tests directory missing"
if test -e "$PROJECT_ROOT/build/affinitygraph.bpf.o"; then pass "local BPF build artifact exists"; else warn "local BPF build artifact absent; cloud build is authoritative"; fi
if command -v /usr/bin/clang-18 >/dev/null 2>&1 && command -v /usr/bin/clang++-18 >/dev/null 2>&1; then pass "local clang-18 toolchain exists"; else warn "local clang-18 toolchain incomplete; compilation is cloud-only"; fi
if test -z "$(git -C "$PROJECT_ROOT" status --porcelain)"; then pass "git worktree is clean"; else warn "git worktree has changes (expected during Modify Agent work)"; fi

if ! ssh -o BatchMode=yes "$cloud_host" true >/dev/null 2>&1; then
  fail "SSH BatchMode failed: $cloud_host"
else
  pass "SSH BatchMode works: $cloud_host"
  remote_output=$(ssh -o BatchMode=yes "$cloud_host" bash -s -- "$cloud_root" "$askpass_config" <<'REMOTE'
set -u
root=$1
config=$2
check() { if eval "$2"; then printf 'PASS|%s\n' "$1"; else printf 'FAIL|%s\n' "$1"; fi; }
check "cloud project root exists" 'test -d "$root"'
check "cloud make exists" 'command -v make >/dev/null 2>&1'
check "cloud clang-18 exists" 'test -x /usr/bin/clang-18'
check "cloud clang++-18 exists" 'test -x /usr/bin/clang++-18'
check "cloud bpftool exists" 'command -v bpftool >/dev/null 2>&1'
check "cloud libbpf.so.1 exists" 'ldconfig -p 2>/dev/null | grep -q "libbpf.so.1"'
check "cloud kernel BTF exists" 'test -r /sys/kernel/btf/vmlinux'
check "askpass config exists with mode 600" 'test -f "$config" && test "$(stat -c %a "$config")" = 600'
if test -r "$config"; then
  source "$config"
  check "askpass helper exists with mode 700" 'test -x "$SUDO_ASKPASS" && test "$(stat -c %a "$SUDO_ASKPASS")" = 700'
  check "sudo -A true succeeds" 'sudo -A true >/dev/null 2>&1'
else
  printf 'FAIL|askpass helper exists with mode 700\n'
  printf 'FAIL|sudo -A true succeeds\n'
fi
REMOTE
  )
  while IFS='|' read -r status message; do
    case "$status" in PASS) pass "$message" ;; FAIL) fail "$message" ;; esac
  done <<< "$remote_output"
fi

if ((failures)); then echo "[FAIL] environment check completed with $failures failure(s)" >&2; exit 1; fi
pass "environment check completed"
