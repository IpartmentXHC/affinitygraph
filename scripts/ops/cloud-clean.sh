#!/usr/bin/env bash
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() {
  cat <<'EOF'
Usage: cloud-clean.sh [--dry-run]
       cloud-clean.sh --apply --confirm-plan-sha256 SHA256

Compare each cloud experiment with a unique same-name local copy. Deletion is
possible only for rows recorded as delete after file-size and critical-file
hash validation, and only after confirmation of the exact TSV SHA256.
EOF
}

mode=dry-run confirm=
while (($#)); do
  case "$1" in --dry-run) mode=dry-run ;; --apply) mode=apply ;; --confirm-plan-sha256) confirm=${2:?}; shift ;; --help|-h) usage; exit 0 ;; *) die "unknown argument: $1" ;; esac
  shift
done
require_ops_config
host=$(ops_get cloud host); remote_root=$(ops_get cloud experiment_root)
local_root=$(ops_get local workspace_experiment_root)
setup_dir="$PROJECT_ROOT/.agent/setup"; tsv="$setup_dir/cloud-clean-plan.tsv"; report="$setup_dir/cloud-clean-plan.md"

if test "$mode" = apply; then
  check_plan_confirmation "$tsv" "$confirm"
  mapfile -t targets < <(awk -F '\t' 'NR>1 && $1=="delete"{print $2}' "$tsv")
  echo "Validated deletion targets: ${#targets[@]}"
  for target in "${targets[@]}"; do
    case "$target" in "$remote_root"/*) ;; *) die "unsafe cloud deletion target: $target" ;; esac
    ssh "$host" rm -rf -- "$target"
    echo "[PASS] deleted: $target"
  done
  ssh "$host" "df -h /home/xhc; du -sh '$remote_root' 2>/dev/null || true"
  exit 0
fi

mkdir -p "$setup_dir"; tmpdir=$(mktemp -d); trap 'rm -rf -- "$tmpdir"' EXIT
printf 'action\tremote_path\tdate\tbytes\tlocal_backup\trisk\n' >"$tsv"
ssh "$host" bash -s -- "$remote_root" >"$tmpdir/names.z" <<'REMOTE_NAMES'
set -euo pipefail
find "$1" -mindepth 1 -maxdepth 1 -type d -printf '%f\0'
REMOTE_NAMES
while IFS= read -r -d '' name; do
  remote="$remote_root/$name"
  remote_meta=$(ssh "$host" bash -s -- "$remote" <<'REMOTE_STAT'
set -euo pipefail
stat -c '%Y %s' "$1"
REMOTE_STAT
  )
  remote_epoch=${remote_meta%% *}; date_value=$(date -d "@$remote_epoch" '+%Y-%m-%d %H:%M:%S')
  bytes=$(ssh -n "$host" du -sb "$remote" | awk '{print $1}')
  mapfile -d '' -t matches < <(find "$local_root" -mindepth 2 -maxdepth 2 -type d -name "$name" -print0 2>/dev/null || true)
  action=review backup=no risk=no-unique-local-backup
  if ((${#matches[@]} == 1)); then
    local_dir=${matches[0]}
    ssh "$host" bash -s -- "$remote" <<'REMOTE_MANIFEST' | LC_ALL=C sort >"$tmpdir/remote.manifest"
set -euo pipefail
find "$1" -type f -printf '%P\t%s\n'
REMOTE_MANIFEST
    find "$local_dir" -type f -printf '%P\t%s\n' | LC_ALL=C sort >"$tmpdir/local.manifest"
    if cmp -s "$tmpdir/remote.manifest" "$tmpdir/local.manifest"; then
      critical_ok=true; critical_count=0
      while IFS=$'\t' read -r rel size; do
        case "$rel" in *runtime.jsonl|*formal-result.json|*result*.json|*config*.toml)
          critical_count=$((critical_count + 1))
          local_hash=$(sha256sum "$local_dir/$rel" | awk '{print $1}')
          remote_hash=$(ssh -n "$host" sha256sum "$remote/$rel" | awk '{print $1}')
          test "$local_hash" = "$remote_hash" || critical_ok=false
        esac
      done <"$tmpdir/remote.manifest"
      if $critical_ok && ((critical_count > 0)); then action=delete; backup=verified; risk=low-after-user-confirmation
      else action=review; backup=manifest-only; risk=critical-files-unverified
      fi
    else action=review; backup=name-only; risk=file-manifest-mismatch
    fi
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$action" "$remote" "$date_value" "$bytes" "$backup" "$risk" >>"$tsv"
done <"$tmpdir/names.z"
count=$(( $(wc -l <"$tsv") - 1 )); deletable=$(awk -F '\t' 'NR>1&&$1=="delete"{n++} END{print n+0}' "$tsv"); total=$(awk -F '\t' 'NR>1{s+=$4} END{printf "%.0f",s}' "$tsv")
{
  echo '# 云端实验数据清理清单'; echo; echo "云端路径：$remote_root"; echo "生成时间：$(format_time)"; echo "项目数量：$count"; echo "已验证可删除：$deletable"; echo "总大小：$(human_bytes "${total:-0}")"; echo "计划 SHA256：$(plan_sha256 "$tsv")"; echo; echo '> 本次仅生成清单；即使标记 delete，也必须由用户确认计划 SHA256 后才会逐目录删除。'; echo; echo '| 云端路径 | 日期 | 大小 | 本地备份 | 操作 | 风险 |'; echo '|---|---:|---:|---|---|---|'
  awk -F '\t' 'NR>1{printf "| `%s` | %s | %s bytes | %s | %s | %s |\n",$2,$3,$4,$5,$1,$6}' "$tsv"
} >"$report"
echo "[PASS] cloud cleanup dry-run written: $report ($deletable/$count backup-validated, SHA256 $(plan_sha256 "$tsv"))"
