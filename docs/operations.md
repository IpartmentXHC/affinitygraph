# Operations

## Build and install

The build requires a C++20 compiler, GNU make, clang with the BPF backend, a
working `bpftool` (a real binary is auto-discovered under
`/usr/lib/linux-tools-*/`; a packaged stub is rejected), and `libbpf.so.1` at
runtime. `make all` builds the user-space binaries, tests, and the CO-RE
object `build/affinitygraph.bpf.o` (generating `build/bpf/vmlinux.h` from
kernel BTF when missing). `make bpf` builds only the CO-RE object.

```sh
make -j CXX=/usr/bin/clang++-18
make test
sudo make install
```

Generate or copy the platform's reviewed `hardware-node-edges.csv` into
`/etc/affinitygraph/calibration/` (or set `resources.calibration_path` in the
target config). Its schema records firmware distance, handoff latency,
serialized memory latency, and STREAM bandwidth separately. Both the 10-column
schema and the 11-column schema ending in `is_estimated` are accepted. Only
handoff latency and firmware distance enter the active CPU objective.

The CPU latency benchmark is vendored under `dep/core-to-core-latency/`
(see `dep/README.md` for the full external-dependency manifest). Regenerate the
platform calibration on the target host with:

```sh
sudo make calibrate
```

`make calibrate` builds the vendored benchmark offline from
`dep/core-to-core-latency/vendor/` and writes the 11-column CSV to
`/etc/affinitygraph/calibration/hardware-node-edges.csv`. For production-grade
values supply `--p95-csv`/`--memory-csv`/`--stream-csv`/`--stream-bin` through
`scripts/generate_calibration.sh`; heuristic estimates are flagged by the
`is_estimated` column.

## Target configuration (generic supervisor)

The generic unit is the per-instance template `affinitygraph@.service`. Each
supervised workload is an instance `<name>` with three inputs:

- `/etc/affinitygraph/targets/<name>.toml` — runtime/resources/collector/
  calibration configuration (see `config/affinitygraph.toml`).
- `/etc/affinitygraph/targets/<name>.env` — optional instance overrides read by
  systemd: `TARGET_EXECUTABLE`, `TARGET_USER`, `TARGET_ARG_1..64`, and
  `AFFINITYGRAPH_CALIBRATION_FILE` (default
  `/etc/affinitygraph/calibration/hardware-node-edges.csv`).
- Optional thread profile JSON (see below).

The template never hardcodes the workload binary, user, calibration path, or
database. `deploy/affinitygraph-clickhouse.service` is a legacy single-workload
example and is not required for new deployments.

### Thread placement profiles (new in per-thread-profile-v1)

A JSON thread profile pins selected threads to explicit CPU lists on process
start. It is generic: rules match `comm`, `comm_prefix`, `cgroup`,
`cgroup_prefix`, or `tid`, and distribute `count` threads across affinity
tiers. A profile with `dynamic.enabled = false` is a static-hold experiment:
the runtime applies the initial masks and holds them for the measurement
window, skipping graph/solver activity. On exit the runtime exports a
`candidate` profile (status and `generated_at` refreshed) to
`runtime.profile_output`, or to
`<log_directory>/profiles/thread-profile-candidate.json` by default.

Enable it with the CLI flag, the environment variable, or TOML (priority:
CLI > `AFFINITY_THREAD_PROFILE` > `runtime.thread_profile`):

```sh
sudo affinity-run preflight --config /etc/affinitygraph/targets/db.toml \
  --thread-profile /etc/affinitygraph/profiles/db.json \
  --bpf-object /usr/lib/affinitygraph/affinitygraph.bpf.o
```

```toml
[runtime]
thread_profile = "/etc/affinitygraph/profiles/db.json"
profile_output = "/var/log/affinitygraph/profiles/thread-profile-candidate.json"
experiment_id = "20260817-db-profile-v1"
test_id = "profile-v1-10-profile-r1"
```

All `allowed_cpus`/affinity CPUs must be inside the resource envelope
(`resources.cpus`, `AFFINITY_CPUS`, or `--cpus`); otherwise preflight fails.
Reference profiles live under `config/thread-profiles/`; the Doris examples are
data artifacts and must be regenerated per workload and per node.

## Preflight

Run this from the same cgroup and startup CPU mask as the service:

```sh
sudo affinity-run preflight \
  --config /etc/affinitygraph/targets/db.toml \
  --thread-profile /etc/affinitygraph/profiles/db.json \
  --bpf-object /usr/lib/affinitygraph/affinitygraph.bpf.o
```

Preflight reports `config`, `thread_profile`, `cpu_envelope`, `kernel`, `bpf`,
and `pthread_uprobe`, and rejects offline CPUs, an envelope outside the
inherited cgroup or process affinity, a malformed profile, malformed/partial
calibration, and invalid controls. Formal configurations require
`collector.required=true`; missing BTF, libbpf, privilege, the BPF object, or a
required attach fails before target exec. The `required=false` path exists only
for isolated degraded-mode tests and must not be used for an experiment.

The supervisor needs root only to load BPF and attach links. It forks the target
behind a pipe barrier, initializes the target map, then drops both parent and
child to `TARGET_USER` (or the original sudo identity). BPF descriptors remain
only in the supervisor; the target receives no AffinityGraph library,
descriptors, environment variables, or ambient root privilege. The control
socket is created after privilege drop with mode `0600`.

## Start and control

Write the target files, then:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now affinitygraph@db.service
sudo affinityctl status --socket /run/affinitygraph-db/control.sock
sudo affinityctl dump --socket /run/affinitygraph-db/control.sock
sudo affinityctl pause --socket /run/affinitygraph-db/control.sock
sudo affinityctl resume --socket /run/affinitygraph-db/control.sock
```

`pause` synchronously restores the latest application-declared mask, or the
mask observed before AffinityGraph first acted. Normal process teardown does
the same. A vanished TID is benign; another action failure rolls back that
batch. Thirty seconds of collector failure triggers the same restore and
pause. Because online application KPI is forbidden, performance regression
rollback remains an external experiment-orchestration responsibility.

## Upgrade, uninstall, and recovery

1. Pause and verify `planned_threads` is zero.
2. Stop the service and confirm the supervisor and target process group exited.
3. Replace binaries/configuration, run preflight, and restart in observe mode.
4. To uninstall, restore the original workload unit and remove the
   AffinityGraph unit, binaries, configuration, and empty log/runtime
   directories. Do not remove calibration or experiment logs until their
   checksums and retention requirements have been handled.

If the supervisor exits unexpectedly, systemd's control-group kill policy
prevents an unmanaged pinned database from being left behind. For a wedged but
live process, send `pause` first; if the socket is unavailable,
restore the configured envelope per TID with the approved host recovery tool,
then stop the process.

## Intranet manual deployment checklist

- [ ] Kernel 6.12/ARM64 (or reviewed equivalent) with BTF:
      `/sys/kernel/btf/vmlinux` exists.
- [ ] Toolchain: C++20 compiler, make, clang with BPF backend, bpftool, and
      `libbpf.so.1` installed. `make all` produces `build/affinitygraph.bpf.o`.
- [ ] Platform calibration reviewed and placed at
      `/etc/affinitygraph/calibration/hardware-node-edges.csv` (10 or 11
      columns), checksums recorded; override via
      `resources.calibration_path` or `AFFINITYGRAPH_CALIBRATION_FILE`.
- [ ] Target config `/etc/affinitygraph/targets/<name>.toml` with
      `collector.required = true`, correct `[resources]` and `[calibration]`.
- [ ] Target env `/etc/affinitygraph/targets/<name>.env` with
      `TARGET_EXECUTABLE`, `TARGET_USER`, optional `TARGET_ARG_*`.
- [ ] Optional thread profile JSON with all CPUs inside the resource envelope;
      dry-run it through preflight first.
- [ ] `sudo affinity-run preflight --config ... --thread-profile ... \
      --bpf-object /usr/lib/affinitygraph/affinitygraph.bpf.o` reports all
      `ok`.
- [ ] `sudo systemctl enable --now affinitygraph@<name>.service`; confirm
      `systemctl status` is active and the runtime JSONL shows `profile_load`
      success (when a profile is used).
- [ ] In `active` mode, verify first applied masks, `planned_threads`, and a
      clean `pause` restore before starting the measurement.
