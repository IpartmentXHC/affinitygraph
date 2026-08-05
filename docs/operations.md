# Operations

## Build and install

The user-space build requires a C++20 compiler and GNU make. The optional BPF
build additionally requires clang with the BPF backend and a working bpftool.

```sh
make -j
make test
make bpf
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

Preflight rejects offline CPUs, an envelope outside the inherited cgroup or
process affinity, malformed/partial calibration, and invalid controls. With
`collector.required=false`, missing BTF, bpftool, privilege, or the BPF object
is reported as `disabled` and the database starts in affinity-disabled
`/proc` observation mode. With `required=true`, startup fails before exec.

The loader needs root for the default bpftool path. It removes all bpffs pins
after opening the maps/programs/links, marks those exact descriptors as
inheritable, then uses `--user` (or the original sudo identity) before exec.
The target receives no ambient root privilege. The control socket is created
with mode `0600` and is therefore accessible only to the target UID.

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
2. Stop the service and confirm no `affinitygraph-<pid>` pin directory remains.
3. Replace binaries/configuration, run preflight, and restart in observe mode.
4. To uninstall, restore the original ClickHouse unit and remove the
   AffinityGraph unit, binaries, library, configuration, and empty log/runtime
   directories. Do not remove calibration or experiment logs until their
   checksums and retention requirements have been handled.

If the process is killed without running destructors, Linux removes its task
affinity state with the tasks themselves and closes inherited BPF descriptors.
For a wedged but live process, send `pause` first; if the socket is unavailable,
restore the configured envelope per TID with the approved host recovery tool,
then stop the process. Do not change memory policy or ClickHouse slots.

