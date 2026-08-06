# Operations

## Build and install

The user-space build requires a C++20 compiler, GNU make, and `libbpf.so.1` at
runtime. The required BPF build additionally requires clang with the BPF
backend and a reviewed `vmlinux.h`. `make bpf` generates the header with
bpftool when one has not already been staged under `build/bpf/`.

```sh
make -j CXX=/usr/bin/clang++-18
make test
make bpf CLANG=/usr/bin/clang-18
sudo make install
```

Copy the platform's reviewed `hardware-node-edges.csv` into
`/etc/affinitygraph/calibration/`. Its schema records firmware distance,
handoff latency, serialized memory latency, and STREAM bandwidth separately.
Only handoff latency and firmware distance enter the active CPU objective.

## Preflight

Run this from the same cgroup and startup CPU mask as the service:

```sh
sudo affinity-run preflight \
  --config /etc/affinitygraph/affinitygraph.toml \
  --bpf-object /usr/lib/affinitygraph/affinitygraph.bpf.o
```

The production target is Linux 6.12/ARM64. Preflight rejects offline CPUs, an
envelope outside the inherited cgroup or process affinity, malformed/partial
calibration, and invalid controls. Formal configurations require
`collector.required=true`; missing BTF, libbpf, privilege, the BPF object, or a
required attach fails before target exec. The `required=false` path exists only
for isolated degraded-mode tests and must not be used for an experiment.

The supervisor needs root only to load BPF and attach links. It forks the target
behind a pipe barrier, initializes the target map, then drops both parent and
child to `--user` (or the original sudo identity). BPF descriptors remain only
in the supervisor; the target receives no AffinityGraph library, descriptors,
environment variables, or ambient root privilege. The control socket is
created after privilege drop with mode `0600`.

## Start and control

Install and edit `deploy/affinitygraph-clickhouse.service`, then:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now affinitygraph-clickhouse.service
sudo -u clickhouse affinityctl status --socket /run/affinitygraph/control.sock
sudo -u clickhouse affinityctl dump --socket /run/affinitygraph/control.sock
sudo -u clickhouse affinityctl pause --socket /run/affinitygraph/control.sock
sudo -u clickhouse affinityctl resume --socket /run/affinitygraph/control.sock
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
4. To uninstall, restore the original ClickHouse unit and remove the
   AffinityGraph unit, binaries, configuration, and empty log/runtime
   directories. Do not remove calibration or experiment logs until their
   checksums and retention requirements have been handled.

If the supervisor exits unexpectedly, systemd's control-group kill policy
prevents an unmanaged pinned database from being left behind. For a wedged but
live process, send `pause` first; if the socket is unavailable,
restore the configured envelope per TID with the approved host recovery tool,
then stop the process. Do not change memory policy or ClickHouse slots.
