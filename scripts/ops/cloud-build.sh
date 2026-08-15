#!/usr/bin/env bash
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() {
  cat <<'EOF'
Usage: cloud-build.sh [--dry-run] [--help]

Publish the current non-ignored source snapshot to an immutable release on the
configured cloud host, then run the ARM64 runtime tests and BPF build there.
EOF
}

dry_run=false
while (($#)); do
  case "$1" in --dry-run) dry_run=true ;; --help|-h) usage; exit 0 ;; *) die "unknown argument: $1" ;; esac
  shift
done
require_ops_config
host=$(ops_get cloud host)
release_root=$(ops_get cloud release_root)
cxx=$(ops_get build cxx)
clang=$(ops_get build clang)
jobs=$(ops_get build jobs)
state_file="$PROJECT_ROOT/build/ops/last-release.env"

if $dry_run; then
  echo "Would create an immutable source release below $host:$release_root"
  echo "Would run: make -j$jobs runtime-test CXX=$cxx"
  echo "Would run: make bpf CLANG=$clang"
  exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
file_list="$tmp/files.z"
archive="$tmp/source.tar"
(
  cd "$PROJECT_ROOT"
  { git ls-files -z; git ls-files --others --exclude-standard -z; } |
    awk -v RS='\0' -v ORS='\0' '$0 !~ /^(build|\.agent|experiments|runs|logs|outputs)\//'
) >"$file_list"
test -s "$file_list" || die "source snapshot is empty"
tar -C "$PROJECT_ROOT" --null -T "$file_list" --sort=name --mtime='UTC 1970-01-01' \
  --owner=0 --group=0 --numeric-owner -cf "$archive"
source_hash=$(sha256sum "$archive" | awk '{print substr($1,1,16)}')
release="$release_root/$source_hash"

if ssh "$host" test -e "$release"; then
  if ssh "$host" test -x "$release/build/affinity-run" && \
     ssh "$host" test -f "$release/build/affinitygraph.bpf.o"; then
    mkdir -p "$(dirname "$state_file")"
    printf 'AFFINITYGRAPH_LAST_RELEASE=%q\n' "$release" >"$state_file"
    echo "[PASS] immutable cloud release already built and tested: $release"
    exit 0
  fi
  die "immutable release exists but is incomplete: $host:$release"
fi
ssh "$host" mkdir -p "$release_root"
ssh "$host" mkdir "$release"
if ! ssh "$host" tar -C "$release" -xf - <"$archive"; then
  ssh "$host" rmdir "$release" 2>/dev/null || true
  die "failed to publish release"
fi
ssh "$host" bash -s -- "$release" "$jobs" "$cxx" "$clang" <<'REMOTE'
set -euo pipefail
release=$1 jobs=$2 cxx=$3 clang=$4
cd "$release"
make -j"$jobs" runtime-test CXX="$cxx"
make bpf CLANG="$clang"
REMOTE
mkdir -p "$(dirname "$state_file")"
printf 'AFFINITYGRAPH_LAST_RELEASE=%q\n' "$release" >"$state_file"
echo "[PASS] cloud release built and tested: $release"
