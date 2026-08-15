#!/usr/bin/env bash
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() {
  cat <<'EOF'
Usage: cloud-preflight.sh --config FILE [--thread-profile FILE] [--release REMOTE_PATH] [--dry-run]

Copy an explicit runtime config to the cloud and run affinity-run preflight.
This command never starts the database or enters active mode.
EOF
}

config= profile= release= dry_run=false
while (($#)); do
  case "$1" in
    --config) config=${2:?}; shift ;;
    --thread-profile) profile=${2:?}; shift ;;
    --release) release=${2:?}; shift ;;
    --dry-run) dry_run=true ;;
    --help|-h) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
  shift
done
test -n "$config" || die "--config is required"
test -r "$config" || die "config is not readable: $config"
if test -n "$profile"; then test -r "$profile" || die "profile is not readable: $profile"; fi
require_ops_config
host=$(ops_get cloud host)
askpass_config=$(ops_get sudo askpass_config)
cloud_root=$(ops_get cloud project_root)
if test -z "$release"; then
  state_file="$PROJECT_ROOT/build/ops/last-release.env"
  test -r "$state_file" || die "no release specified and state file is missing: $state_file"
  source "$state_file"
  release=${AFFINITYGRAPH_LAST_RELEASE:?}
fi
remote_config="$cloud_root/ops/preflight/$(sha256sum "$config" | awk '{print substr($1,1,16)}').toml"
remote_profile=
if test -n "$profile"; then remote_profile="$cloud_root/ops/preflight/$(sha256sum "$profile" | awk '{print substr($1,1,16)}').json"; fi
if $dry_run; then
  echo "Would copy $config to $host:$remote_config"
  if test -n "$profile"; then echo "Would copy $profile to $host:$remote_profile"; fi
  echo "Would run sudo -A $release/build/affinity-run preflight"
  exit 0
fi
ssh "$host" mkdir -p "$cloud_root/ops/preflight"
scp -q "$config" "$host:$remote_config"
if test -n "$profile"; then scp -q "$profile" "$host:$remote_profile"; fi
ssh "$host" bash -s -- "$askpass_config" "$release" "$remote_config" "$remote_profile" <<'REMOTE'
set -euo pipefail
config_env=$1 release=$2 runtime_config=$3 runtime_profile=${4-}
test -r "$config_env"
source "$config_env"
args=(preflight --config "$runtime_config" --bpf-object "$release/build/affinitygraph.bpf.o")
if test -n "$runtime_profile"; then args+=(--thread-profile "$runtime_profile"); fi
sudo -A "$release/build/affinity-run" "${args[@]}"
REMOTE
echo "[PASS] cloud preflight completed for release: $release"
