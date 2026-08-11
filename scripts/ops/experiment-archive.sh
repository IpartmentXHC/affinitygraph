#!/usr/bin/env bash
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() {
  cat <<'EOF'
Usage: experiment-archive.sh [--dry-run]
       experiment-archive.sh --apply --confirm-plan-sha256 SHA256

Inventory experiment directories older than the configured threshold. Apply
only archives rows explicitly approved as "archive" in the unchanged TSV plan.
Sources are preserved after verified compression.
EOF
}

mode=dry-run confirm=
while (($#)); do
  case "$1" in --dry-run) mode=dry-run ;; --apply) mode=apply ;; --confirm-plan-sha256) confirm=${2:?}; shift ;; --help|-h) usage; exit 0 ;; *) die "unknown argument: $1" ;; esac
  shift
done
require_ops_config
workspace_root=$(ops_get local workspace_experiment_root)
archive_root=$(ops_get local archive_root)
days=$(ops_get archive local_experiment_days)
setup_dir="$PROJECT_ROOT/.agent/setup"
tsv="$setup_dir/local-archive-plan.tsv"
report="$setup_dir/local-archive-plan.md"

if test "$mode" = apply; then
  check_plan_confirmation "$tsv" "$confirm"
  while IFS=$'\t' read -r action source date bytes backup risk; do
    test "$action" = archive || continue
    case "$source" in "$PROJECT_ROOT"/*) ;; *) die "refusing cross-project archive: $source" ;; esac
    test -d "$source" || die "archive source disappeared: $source"
    month=${date:0:7}; destination="$archive_root/experiments/$month/$(basename "$source")"
    mkdir -p "$(dirname "$destination")"
    if command -v zstd >/dev/null 2>&1; then
      output="$destination.tar.zst"; tar --zstd -cf "$output" -C "$(dirname "$source")" "$(basename "$source")"; tar --zstd -tf "$output" >"$output.manifest.txt"
    else
      output="$destination.tar.gz"; tar -czf "$output" -C "$(dirname "$source")" "$(basename "$source")"; tar -tzf "$output" >"$output.manifest.txt"
    fi
    echo "[PASS] archived and verified; source preserved: $source"
  done < <(tail -n +2 "$tsv")
  exit 0
fi

mkdir -p "$setup_dir"
now_epoch=$(date +%s); cutoff=$((now_epoch - days * 86400))
tmp=$(mktemp); trap 'rm -f -- "$tmp"' EXIT
printf 'action\tsource\tdate\tbytes\tlocal_backup\trisk\n' >"$tmp"

# Repo-owned experiment roots are eligible in principle. Cross-project data is
# itemized for review but deliberately cannot become an automatic archive row.
for root in "$PROJECT_ROOT/experiments" "$PROJECT_ROOT/runs" "$PROJECT_ROOT/logs" "$PROJECT_ROOT/outputs"; do
  test -d "$root" || continue
  while IFS= read -r -d '' dir; do
    epoch=$(stat -c %Y "$dir"); ((epoch < cutoff)) || continue
    date_value=$(date -d "@$epoch" '+%Y-%m-%d %H:%M:%S')
    bytes=$(du -sb "$dir" | awk '{print $1}')
    printf 'review\t%s\t%s\t%s\tno\tmanual-confirmation\n' "$dir" "$date_value" "$bytes" >>"$tmp"
  done < <(find "$root" -mindepth 1 -maxdepth 1 -type d -print0)
done
if test -d "$workspace_root"; then
  # These are experiment containers. Other known top-level directories contain
  # configuration/calibration artifacts and are intentionally excluded.
  for family in "$workspace_root/clickhouse" "$workspace_root/doris" "$workspace_root/yba-staging"; do
    test -d "$family" || continue
    while IFS= read -r -d '' dir; do
      epoch=$(stat -c %Y "$dir"); ((epoch < cutoff)) || continue
      date_value=$(date -d "@$epoch" '+%Y-%m-%d %H:%M:%S')
      bytes=$(du -sb "$dir" | awk '{print $1}')
      printf 'no-action\t%s\t%s\t%s\tunknown\tcross-project-manual-only\n' "$dir" "$date_value" "$bytes" >>"$tmp"
    done < <(find "$family" -mindepth 1 -maxdepth 1 -type d -print0)
  done
  while IFS= read -r -d '' dir; do
    epoch=$(stat -c %Y "$dir"); ((epoch < cutoff)) || continue
    date_value=$(date -d "@$epoch" '+%Y-%m-%d %H:%M:%S')
    bytes=$(du -sb "$dir" | awk '{print $1}')
    printf 'no-action\t%s\t%s\t%s\tunknown\tcross-project-manual-only\n' "$dir" "$date_value" "$bytes" >>"$tmp"
  done < <(find "$workspace_root" -mindepth 1 -maxdepth 1 -type d -name 'smoke-*' -print0)
fi
mv "$tmp" "$tsv"; trap - EXIT
count=$(( $(wc -l <"$tsv") - 1 )); total=$(awk -F '\t' 'NR>1{s+=$4} END{printf "%.0f",s}' "$tsv")
{
  echo '# 本地实验数据归档清单'; echo; echo "生成时间：$(format_time)"; echo "归档阈值：$days 天之前"; echo "归档目标目录：$archive_root/experiments"; echo "清单项目：$count"; echo "合计大小：$(human_bytes "${total:-0}")"; echo "计划 SHA256：$(plan_sha256 "$tsv")"; echo; echo '> 当前所有跨项目数据均为 no-action；未授权归档或删除。'; echo; echo '| 路径 | 日期 | 大小 | 操作 | 风险 |'; echo '|---|---:|---:|---|---|'
  awk -F '\t' 'NR>1{printf "| `%s` | %s | %s bytes | %s | %s |\n",$2,$3,$4,$1,$6}' "$tsv"
} >"$report"
echo "[PASS] local archive dry-run written: $report ($count items, SHA256 $(plan_sha256 "$tsv"))"
