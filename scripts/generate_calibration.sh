#!/usr/bin/env bash
set -euo pipefail

# Generate the AffinityGraph node calibration from the local Rust benchmark.
# The benchmark supplies CPU-to-CPU mean latency only. NUMA topology comes
# from sysfs; p95, memory-load, and STREAM values may come from supplementary
# CSV files or from explicitly marked heuristic estimates.

usage() {
  cat <<'EOF'
Usage: generate_calibration.sh --latency-repo PATH --output PATH [options]

Options:
  --cores LIST       CPU list/ranges, e.g. 0-31,64-95 (default: task affinity)
  --p95-csv PATH     CSV with source_node,destination_node,core_handoff_p95_ns
  --memory-csv PATH  CSV with source_node,destination_node,memory_load_mean_ns,memory_load_cv
  --stream-csv PATH  CSV with source_node,destination_node,stream_2t_triad_mbps,stream_32t_triad_mbps
  --stream-bin PATH  STREAM executable; its Triad result is used for all node pairs
  --p95-factor N      heuristic p95 multiplier (default: 1.30)
EOF
}

die() { echo "[FAIL] $*" >&2; exit 1; }
warn() { printf '\033[1;33m[WARNING] %s\033[0m\n' "$*" >&2; }

LATENCY_REPO=""
OUTPUT=""
CORES=""
P95_CSV=""
MEMORY_CSV=""
STREAM_CSV=""
STREAM_BIN="${STREAM_BIN:-}"
P95_FACTOR="${P95_FACTOR:-1.30}"

while (($#)); do
  case "$1" in
    --latency-repo) LATENCY_REPO=${2:?missing value}; shift 2 ;;
    --output) OUTPUT=${2:?missing value}; shift 2 ;;
    --cores) CORES=${2:?missing value}; shift 2 ;;
    --p95-csv) P95_CSV=${2:?missing value}; shift 2 ;;
    --memory-csv) MEMORY_CSV=${2:?missing value}; shift 2 ;;
    --stream-csv) STREAM_CSV=${2:?missing value}; shift 2 ;;
    --stream-bin) STREAM_BIN=${2:?missing value}; shift 2 ;;
    --p95-factor) P95_FACTOR=${2:?missing value}; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

[[ -n "$LATENCY_REPO" ]] || die "--latency-repo is required"
[[ -n "$OUTPUT" ]] || die "--output is required"
[[ -d "$LATENCY_REPO" ]] || die "latency repository does not exist: $LATENCY_REPO"
command -v cargo >/dev/null 2>&1 || die "cargo is required"
command -v python3 >/dev/null 2>&1 || die "python3 is required"

if [[ -z "$CORES" ]]; then
  command -v taskset >/dev/null 2>&1 || die "taskset is required when --cores is omitted"
  CORES=$(taskset -pc $$ | sed 's/.*: //')
fi

if [[ -z "$STREAM_BIN" ]] && command -v stream >/dev/null 2>&1; then
  STREAM_BIN=$(command -v stream)
fi

[[ "$P95_FACTOR" =~ ^[0-9]+([.][0-9]+)?$ ]] || die "invalid --p95-factor: $P95_FACTOR"
[[ -z "$P95_CSV" || -f "$P95_CSV" ]] || die "p95 CSV does not exist: $P95_CSV"
[[ -z "$MEMORY_CSV" || -f "$MEMORY_CSV" ]] || die "memory CSV does not exist: $MEMORY_CSV"
[[ -z "$STREAM_CSV" || -f "$STREAM_CSV" ]] || die "STREAM CSV does not exist: $STREAM_CSV"
[[ -z "$STREAM_BIN" || -x "$STREAM_BIN" ]] || die "STREAM binary is not executable: $STREAM_BIN"

TMP_DIR=$(mktemp -d)
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

echo "[INFO] building core-to-core-latency offline"
# Run cargo from inside the latency repo so its .cargo/config.toml source
# replacement (vendored crates) is discovered; cargo resolves configuration
# relative to the working directory, not --manifest-path.
(cd "$LATENCY_REPO" && cargo build --release --offline)
BENCH="$LATENCY_REPO/target/release/core-to-core-latency"
[[ -x "$BENCH" ]] || die "benchmark binary was not produced: $BENCH"

# clap accepts comma-delimited CPU IDs, not Linux range syntax.
CORE_IDS=$(python3 - "$CORES" <<'PY'
import sys

result = []
for part in sys.argv[1].split(','):
    part = part.strip()
    if not part:
        raise SystemExit('empty CPU-list element')
    bounds = part.split('-', 1)
    first = int(bounds[0])
    last = int(bounds[-1])
    if first < 0 or last < first:
        raise SystemExit(f'invalid CPU range: {part}')
    result.extend(range(first, last + 1))
print(','.join(str(cpu) for cpu in sorted(set(result))))
PY
)

echo "[INFO] measuring CPU latency for cores: $CORES"
"$BENCH" --csv --bench 1 --cores "$CORE_IDS" >"$TMP_DIR/latency.csv"

STREAM_OUTPUT=""
if [[ -n "$STREAM_BIN" ]]; then
  echo "[INFO] running STREAM: $STREAM_BIN"
  STREAM_OUTPUT=$("$STREAM_BIN" 2>&1 || true)
  STREAM32=$(printf '%s\n' "$STREAM_OUTPUT" | awk '$1 == "Triad:" {print $2; exit}')
  [[ "$STREAM32" =~ ^[0-9]+([.][0-9]+)?$ ]] || die "could not parse STREAM Triad output"
else
  STREAM32=""
fi

mkdir -p "$(dirname -- "$OUTPUT")"
TMP_OUTPUT=$(mktemp "$(dirname -- "$OUTPUT")/.hardware-node-edges.XXXXXX")
trap 'rm -rf "$TMP_DIR"; rm -f "$TMP_OUTPUT"' EXIT

python3 - "$TMP_DIR/latency.csv" "$CORES" "$OUTPUT" "$P95_CSV" "$MEMORY_CSV" "$STREAM_CSV" "$STREAM32" "$P95_FACTOR" "$TMP_OUTPUT" <<'PY'
import csv
import math
import os
import statistics
import subprocess
import sys

latency_path, core_spec, output_path, p95_path, memory_path, stream_path, stream32, p95_factor, tmp_output = sys.argv[1:]
p95_factor = float(p95_factor)

def expand(spec):
    values = []
    for part in spec.split(','):
        bounds = [int(v) for v in part.strip().split('-', 1)]
        values.extend(range(bounds[0], bounds[-1] + 1))
    return sorted(set(values))

cpus = expand(core_spec)
with open(latency_path, newline='') as f:
    matrix = [[float(v) if v.strip() else None for v in row] for row in csv.reader(f)]
if len(matrix) != len(cpus):
    raise SystemExit(f'latency matrix rows={len(matrix)} CPUs={len(cpus)}')

def sysfs_value(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None

nodes = {}
packages = {}
for cpu in cpus:
    node = None
    cpu_dir = f'/sys/devices/system/cpu/cpu{cpu}'
    for entry in os.listdir(cpu_dir):
        if entry.startswith('node') and entry[4:].isdigit():
            node = int(entry[4:])
            break
    if node is None:
        node = 0
    nodes[cpu] = node
    package = sysfs_value(f'{cpu_dir}/topology/physical_package_id')
    packages[cpu] = package if package is not None else str(node)

def distance(src, dst):
    values = sysfs_value(f'/sys/devices/system/node/node{src}/distance')
    if values:
        row = values.split()
        if dst < len(row):
            return float(row[dst])
    return 10.0 if src == dst else 20.0

def supplement(path):
    result = {}
    if not path:
        return result
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            key = (int(row['source_node']), int(row['destination_node']))
            result[key] = row
    return result

p95 = supplement(p95_path)
memory = supplement(memory_path)
stream = supplement(stream_path)

def measured(a, b):
    i, j = cpus.index(a), cpus.index(b)
    if i == j:
        return 0.0
    hi, lo = max(i, j), min(i, j)
    if hi >= len(matrix) or lo >= len(matrix[hi]) or matrix[hi][lo] is None:
        return None
    return matrix[hi][lo]

node_pairs = sorted({(nodes[a], nodes[b]) for a in cpus for b in cpus})
header = [
    'source_node', 'destination_node', 'same_socket', 'numa_distance',
    'core_handoff_mean_ns', 'core_handoff_p95_ns', 'memory_load_mean_ns',
    'memory_load_cv', 'stream_2t_triad_mbps', 'stream_32t_triad_mbps',
    'is_estimated'
]

estimated_warning = False
rows = []
for src, dst in node_pairs:
    values = [measured(a, b) for a in cpus for b in cpus
              if nodes[a] == src and nodes[b] == dst and a != b]
    values = [v for v in values if v is not None and v > 0]
    if not values:
        mean = 10.0 if src == dst else 20.0
        estimated_warning = True
    else:
        mean = statistics.fmean(values)

    key = (src, dst)
    if key in p95:
        p95_value = float(p95[key]['core_handoff_p95_ns'])
        p95_estimated = False
    else:
        p95_value = mean * p95_factor
        p95_estimated = True

    if key in memory:
        memory_mean = float(memory[key]['memory_load_mean_ns'])
        memory_cv = float(memory[key]['memory_load_cv'])
        memory_estimated = False
    else:
        memory_mean = max(mean * 2.0, 1.0)
        memory_cv = 0.05
        memory_estimated = True

    if key in stream:
        stream2 = float(stream[key]['stream_2t_triad_mbps'])
        stream32 = float(stream[key]['stream_32t_triad_mbps'])
        stream_estimated = False
    elif stream32:
        stream32 = float(stream32)
        stream2 = stream32 * 0.5
        stream_estimated = True
    else:
        # This is deliberately explicit and configurable. It keeps the file
        # schema valid while is_estimated prevents mistaking it for a measured
        # platform calibration.
        stream32 = float(os.environ.get('STREAM32_FALLBACK_MBPS', '10000'))
        stream2 = stream32 * 0.5
        stream_estimated = True

    same_socket = int(src == dst or any(packages[a] == packages[b]
                                        for a in cpus for b in cpus
                                        if nodes[a] == src and nodes[b] == dst))
    estimated = int(p95_estimated or memory_estimated or stream_estimated or not values)
    estimated_warning |= bool(estimated)
    rows.append([
        src, dst, same_socket, distance(src, dst), mean, p95_value,
        memory_mean, memory_cv, stream2, stream32, estimated
    ])

with open(tmp_output, 'w', newline='') as f:
    writer = csv.writer(f, lineterminator='\n')
    writer.writerow(header)
    writer.writerows(rows)
os.replace(tmp_output, output_path)
if estimated_warning:
    print('[WARNING] Using heuristic estimates for p95/memory/STREAM on Kunpeng920', file=sys.stderr)
print(f'[INFO] generated {output_path} with {len(rows)} node-pair rows')
PY

chmod 0644 "$OUTPUT"
echo "[INFO] calibration atomically installed at $OUTPUT"
