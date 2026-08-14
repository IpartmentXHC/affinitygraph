# AffinityGraph Modify Report

## Goal and plan

Introduce JSON `thread-profile` loading for compact per-`comm` affinity reuse,
initial affinity application, candidate export, and profile-aware preflight.
The production configuration now defaults to the existing per-thread affinity
path instead of `numa-domain-v1`.

## Changes

- Added `ThreadProfile` v1 parsing, validation, matching, count-distribution
  assignment, and atomic JSON export.
- Added `comm`/cgroup matching data from `/proc/<tgid>/task/<tid>/cgroup`.
- Added `affinity-run --thread-profile`, `--profile-output`, `--experiment-id`,
  and `--test-id`; invalid runtime profiles log and fall back without stopping
  the target, while preflight fails.
- Applied profile assignments on first thread observation in active mode;
  observe and plan only log the initial affinity action.
- Added `config/thread-profiles/example.json` and profile fixture coverage.
- Added optional `PROFILE=` support to `make ops-cloud-preflight`.
- Changed the default configuration to `incremental-hotspot-v1`,
  `singleton_cpu`, and one-second solve intervals.

## Doris profile-v1 validation

- Added `config/thread-profiles/doris-light-pipe-node2.seed.json`: candidate
  profile with exact `brpc_light` (512) and `Pipe_normal` (128) selectors,
  both set to CPU list `64-95`; `brpc_heavy` and all unmatched threads are
  unmanaged. Dynamic adjustment is explicitly disabled for this static
  validation.
- The static-profile runtime now logs `profile_static_hold` and prevents the
  legacy incremental solver from rewriting the profile masks.
- Added `prism-sampler affinitygraph validate-doris-thread-profile`, which
  runs randomized unrestricted/profile pairs, passes the profile and export
  IDs to `affinity-run`, and only accepts a candidate export when both profile
  runs have identical placement structures.
- The runner's profile path disables the old solver-plan readiness gate and
  uses its own post-run checks: 512/128 rule matches, `64-95` masks, no
  unexpected match, static-hold window, BPF loss below 1%, and restoration.

## Verification

- `make ops-local-test`: passed.
- `make ops-cloud-build`: ARM64 compilation and test binary build completed on
  `kunpen183`; direct core test execution in the release passed.
- `make ops-cloud-preflight CONFIG=config/affinitygraph.toml`: passed.
- `make ops-cloud-preflight CONFIG=config/affinitygraph.toml PROFILE=config/thread-profiles/example.json`: passed.
- `make ops-cloud-preflight CONFIG=config/affinitygraph.toml PROFILE=config/thread-profiles/doris-light-pipe-node2.seed.json`: passed on `kunpen183`.
- `prism-sampler` focused tests: `36 passed`.
- Doris C4T4 first unrestricted run completed: `556.015890` throughput,
  `63711 us` P99, zero timeout.
- Doris static-profile smoke reached exactly 512 `brpc_light` and 128
  `Pipe_normal` matches, CPU list `64-95`, at least four
  `profile_static_hold` windows, and zero BPF window loss.
- The first conforming profile run was stopped by the existing hook because it
  incorrectly required a legacy solver plan despite static masks being
  applied. The runner gate has been corrected, but the two paired validation
  runs have not yet been re-executed after that correction.
- A subsequent corrected run accepted the static-profile readiness mode, but
  its YBA server-start stage exceeded six minutes before producing any phase
  artifact. It was terminated to retain the agreed short-experiment budget.
  The preserved root is
  `/data/threadState/experiments/doris/20260812-doris-thread-profile-v1-c4t4-final`.

## Risks and next step

The profile contract and initial placement are implemented, but legacy domain
and offline strategy source files still remain compiled as compatibility code.
The existing incremental runtime is the active dynamic path; it has not yet
been reduced to the planned minimal profile-only micro-adjuster. The next
change must remove `NumaDomainSolver`, offline replay/strategies, and the
remaining group/hotspot state, then add the required 30% large-change and
candidate-export aggregation tests.

Generated profile path: runtime default is
`<log_directory>/profiles/thread-profile-candidate.json`. The intended checked
in candidate path is
`config/thread-profiles/doris-light-pipe-node2-c4t4.candidate.json`; it has not
been produced because the two conforming paired runs are still pending.

## 2026-08-13 plan implementation update

### Goal

Implement the agreed cold-start comparison protocol, external other-thread
fallback control, and telemetry-only runtime control without changing the
thread-profile schema or starting a long Doris experiment.

### Changes

- Added YBA `cold_start_duration` scenario mode. It requires a first
  `server_idle` phase, waits without invoking YCSB, then runs the measurement
  phase. Existing `duration`/ratio modes retain their load-generating behavior.
- Recorded `server_start_epoch_ns` and `server_ready_epoch_ns`; cold-start
  summaries exclude `server_idle` from workload operations while using the
  server-start timestamp for lifecycle throughput. Dual-host runs copy server
  markers into the client experiment metadata.
- Added runner cold-start generation (30-second idle default, overridable by
  `--steady-warmup`), external Doris other-thread fallback settings, and an
  explicit runtime-only lifecycle mode. The `prism-sampler affinitygraph
  validate-doris-controls` entry point now exposes bounded C5T16 controls:
  `other_fallback` (profile versus profile+fallback) and `runtime_only`
  (telemetry-only).
- Added `config/thread-profiles/runtime-only-empty.json`. Empty profiles keep
  telemetry and `/proc` sampling active but cannot enter static-hold or issue
  affinity actions. Added loader/core-test coverage.
- Added scenario and runner tests for idle execution, timestamp-based summary,
  fallback CPU set, and runtime-only mode validation.

### Tests and verification

- `yba-bench-all-database`: `python3 -m unittest -q tests.test_scenario` ->
  `22` tests passed; shell syntax and `git diff --check` passed.
- `affinitygraph`: `make test` -> core tests passed; `make ops-local-test` ->
  passed.
- `affinitygraph`: `make ops-cloud-build` -> passed on `kunpen183`, release
  `/home/xhc/.local/share/affinitygraph/releases/d83cb36c9070ea8a`.
- `make ops-cloud-preflight CONFIG=config/affinitygraph.toml PROFILE=config/thread-profiles/runtime-only-empty.json`
  -> passed; BPF CO-RE loaded, CPU envelope `0-127`.
- `prism-sampler`: Python modules compile with `py_compile`. The environment
  has no `pytest` module, so the pytest suite was not executed.
- No Doris smoke or long experiment was started in this turn.

### Risks

- Existing legacy NUMA/domain source remains in the repository for compatibility;
  this update only prevents it from interfering with non-empty static profiles
  and telemetry-only empty profiles.
- The cold-start lifecycle throughput includes server startup and the explicit
  30-second idle interval by design; active throughput remains measurement-only.

### Generated profile paths

- Telemetry control: `config/thread-profiles/runtime-only-empty.json`.
- Runtime candidate export remains
  `<log_directory>/profiles/thread-profile-candidate.json` and is copied by the
  profile validation runner when a conforming run completes.

### Next step

Run a short, explicitly approved Doris C5T16 pair using the generated cold-start
scenario, then run the C5T16 other-fallback and runtime-only controls before any
long validation matrix.

## 2026-08-12 Three-load Verification Attempt

- Extended the Doris profile runner to schedule five randomized paired runs
  for each of C2T2, C4T4, and C5T16, compute paired throughput uplift and a
  95% CI, and export per-load candidates only after consistent exports.
- Added a six-minute pre-WARMUP startup guard. Static profile monitoring now
  uses WARMUP as its start signal and skips obsolete solver-readiness polling
  after the existing runtime/BPF health check.
- Reduced static profile runtime work to thread matching and affinity only:
  relation aggregation, NUMA page scanning, graph construction, and legacy
  solver state maintenance are skipped. Match smoke remained exact at 512
  `brpc_light` and 128 `Pipe_normal`, CPU list `64-95`, zero BPF loss.
- Validation passed: local test, ARM64 cloud build/core test, seed preflight,
  and 36 focused runner tests.
- No new performance conclusion was produced. A clean C2T2 baseline completed
  WARMUP and measurement (`294.240920 ops/s`, P99 `23631 us`, zero timeout),
  but YBA returned exit code 1 during post-scenario cleanup although its report
  marked both phases `ok`. The runner rejected the sample. The next action is
  to preserve a successfully summarized scenario whose only failure is cleanup
  while retaining hard failures for phase, hook, BPF, and workload errors; then
  rerun the clean three-load matrix.

## 2026-08-12 Cleanup Blocking Fix and C2T2 Retest

- Root cause: `bin/yba` propagated the EXIT-trap cleanup return code after a
  complete dual-host scenario. Doris stop can return 1 while waiting for the
  final BE/FE process to disappear, even when `scenario-report.md` and every
  phase are `ok` with zero workload errors/timeouts. The runner consequently
  rejected a valid baseline before pairing.
- Fix: added `yba_scenario_result_is_complete`, which checks the aggregate
  scenario status, every timeline row, and zero error/timeout counts. A cleanup
  nonzero is now recorded as `meta/cleanup-warning.txt` and does not override
  an otherwise successful scenario; phase, hook, BPF, workload, and restore
  failures remain hard failures.
- Tests: `python3 -m unittest -q tests.test_scenario` (18 passed),
  `pytest -q tests/test_affinitygraph_runner.py` (34 passed), shell syntax and
  `git diff --check` passed.
- C2T2 retest: `/data/threadState/experiments/doris/20260812-doris-thread-profile-v1-c2t2-retest`.
  Five valid paired rounds, 2/5 profile wins, mean throughput uplift
  `+1.1964%`, 95% CI half-width `4.2656%` (paired uplift), and all profile
  safety gates passed: 512 `brpc_light`, 128 `Pipe_normal`, CPU `64-95`, zero
  BPF loss, static holds, and restoration.
- Generated candidate: `/data/threadState/affinitygraph/config/thread-profiles/doris-light-pipe-node2-c2t2.candidate.json`.
  It remains `candidate`, not `tested`; C4T4 and C5T16 were not rerun in this
  turn and therefore have no new verified conclusion.

## Risks and next step

The cleanup warning classification is intentionally narrow and does not make
the runner accept incomplete or errored workload data. Run the same repaired
matrix for C4T4 and C5T16 before making a three-load performance claim or
marking any profile as tested.

## 2026-08-12 C4T4/C5T16 Retest

- Experiment: `/data/threadState/experiments/doris/20260812-doris-thread-profile-v1-c4t4-c5t16-retest`.
- The repaired runner completed all 20 runs: five unrestricted/profile pairs
  for C4T4 and five for C5T16. Every phase completed with zero workload errors
  and timeouts; cleanup warnings did not interrupt pairing.
- C4T4: unrestricted mean `587.012 ops/s`, profile mean `606.324 ops/s`,
  mean uplift `+3.394%`, 5/5 wins, 95% CI half-width `5.183%`. Mean P99
  changed from `58,930 us` to `56,274 us` (`-4.51%`); profile P99 was lower
  in all five pairs.
- C5T16: unrestricted mean `590.078 ops/s`, profile mean `595.669 ops/s`,
  mean uplift `+0.967%`, 3/5 wins, 95% CI half-width `2.268%`. Mean P99
  changed from `264,626 us` to `285,132 us` (`+7.75%`); profile P99 was
  higher in all five pairs. This is not a latency-safe optimization despite
  the small throughput gain.
- All ten profile runs passed the static safety checks: 512 `brpc_light`, 128
  `Pipe_normal`, CPU `64-95`, zero BPF loss, static-hold windows, and restore.
- Generated candidates:
  - `/data/threadState/affinitygraph/config/thread-profiles/doris-light-pipe-node2-c4t4.candidate.json`
  - `/data/threadState/affinitygraph/config/thread-profiles/doris-light-pipe-node2-c5t16.candidate.json`
  Both remain `candidate`; neither is marked `tested`.

## Next Step

Use the C4T4 candidate for a short replay/confirmation before promotion.
Keep C5T16 as a candidate only, or investigate a latency-aware placement
variant before reuse; do not claim it as an optimization based on throughput
alone.
