#!/usr/bin/env bash
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() {
  cat <<'EOF'
Usage: local-test.sh [--dry-run] [--help]

Run non-compiling local static checks. All compiled tests run on kunpen183 via
cloud-build.sh.
EOF
}

dry_run=false
while (($#)); do
  case "$1" in --dry-run) dry_run=true ;; --help|-h) usage; exit 0 ;; *) die "unknown argument: $1" ;; esac
  shift
done

if $dry_run; then
  echo "git -C $PROJECT_ROOT diff --check"
  echo "bash -n scripts/ops/*.sh"
  echo "make -n runtime-test CXX=/usr/bin/clang++-18"
  echo "make -n bpf CLANG=/usr/bin/clang-18"
  exit 0
fi

git -C "$PROJECT_ROOT" diff --check
for script in "$PROJECT_ROOT"/scripts/ops/*.sh; do bash -n "$script"; done
make -C "$PROJECT_ROOT" -n runtime-test CXX=/usr/bin/clang++-18 >/dev/null
make -C "$PROJECT_ROOT" -n bpf CLANG=/usr/bin/clang-18 >/dev/null
echo "[PASS] local static checks completed; compiled tests remain cloud-only"
