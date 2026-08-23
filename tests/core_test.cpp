#include "affinitygraph/core.hpp"
#include "affinitygraph/bpf_events.h"
#include "affinitygraph/thread_profile.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

using namespace affinitygraph;

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

HardwareGraph hardware() {
  HardwareGraph h;
  h.cpus = {{0, 0, true}, {1, 0, true}, {2, 1, true}, {3, 1, true}};
  h.node_distance = {{{0, 0}, 10}, {{0, 1}, 30}, {{1, 0}, 30}, {{1, 1}, 10}};
  return h;
}

ThreadSample sample(int tid, uint64_t ts, uint64_t run, uint64_t rq, uint64_t start = 1) {
  ThreadSample s;
  s.identity = {100, tid, start};
  s.timestamp_ns = ts;
  s.runtime_ns = run;
  s.runqueue_ns = rq;
  s.recent_cpu = tid % 4;
  s.comm = "worker_" + std::to_string(tid);
  return s;
}

class FakeBackend final : public AffinityBackend {
public:
  std::map<int, std::vector<int>> masks{{1, {0, 1}}, {2, {0, 1}}};
  int fail_tid = -1;
  std::vector<int> get(int tid) override { return masks[tid]; }
  bool set(int tid, const std::vector<int> &cpus, int &error) override {
    if (tid == fail_tid) { error = EPERM; return false; }
    masks[tid] = cpus;
    return true;
  }
};

void test_cpu_lists() {
  require(parse_cpu_list("0-3,6,8-9") == std::vector<int>({0, 1, 2, 3, 6, 8, 9}), "parse CPU list");
  require(format_cpu_list({3, 2, 1, 0, 6}) == "0-3,6", "format CPU list");
  auto test_config = load_config("tests/runtime.toml");
  require(test_config.pthread_uprobe,
          "pthread uprobe config parsed");
  auto production = load_config("config/affinitygraph.toml");
  require(production.solver == "incremental-hotspot-v1" &&
              production.affinity_granularity == "singleton_cpu" &&
              production.solve_interval_seconds == 1 &&
              production.proposal_confirmations == 2 &&
              production.candidate_hard_limit == 4 &&
              production.maximum_managed_threads == 128,
          "per-thread configuration parsed");
  auto static_config = load_config("tests/fixtures/config-static-sample0.toml");
  require(!static_config.dynamic && static_config.sample_interval_seconds == 0 &&
              static_config.static_quiescence_seconds == 30,
          "static sample-zero configuration parsed");
  bool invalid_rejected = false;
  try {
    load_config("tests/fixtures/config-static-sample0-invalid.toml");
  } catch (const std::exception &) {
    invalid_rejected = true;
  }
  require(invalid_rejected, "static_quiescence_seconds < 1 rejected");
}

void test_thread_profile() {
  auto profile = load_thread_profile("tests/fixtures/thread-profile-small.json", {0, 1, 2, 3});
  require(profile.schema_version == 2 && profile.placements.size() == 1 &&
              profile.dynamic.large_step_threads == 4,
          "thread profile v2 loads");
  require(profile.placements[0].affinities.size() == 1 &&
              profile.placements[0].affinities[0].cpus == std::vector<int>({0}),
          "v2 rule has a single affinity without count");
  ThreadSample worker = sample(10, 1, 0, 0);
  worker.comm = "worker";
  worker.cgroups = {"/job"};
  std::map<std::string, size_t> instances;
  std::map<ThreadIdentity, ProfileAssignment> assigned;
  auto first = profile_assignment(profile, worker, instances, assigned);
  require(first && first->target_cpus == std::vector<int>({0}) && first->instance == 0,
          "first profile assignment");
  worker.identity = {100, 11, 2};
  auto second = profile_assignment(profile, worker, instances, assigned);
  require(second && second->target_cpus == std::vector<int>({0}) && second->instance == 1,
          "every matching thread binds to the same target");
  worker.identity = {100, 12, 3};
  auto third = profile_assignment(profile, worker, instances, assigned);
  require(third && third->target_cpus == std::vector<int>({0}) && third->instance == 2,
          "no count cap: all matching threads are bound");
  bool rejected = false;
  try { load_thread_profile("tests/fixtures/thread-profile-small.json", {0}); }
  catch (...) { rejected = true; }
  require(rejected, "profile outside envelope rejected");
  auto v1 = load_thread_profile("tests/fixtures/thread-profile-v1-count.json", {0, 1, 2, 3});
  require(v1.schema_version == 1 && v1.placements.size() == 1 &&
              v1.placements[0].affinities.size() == 1 &&
              v1.placements[0].affinities[0].cpus == std::vector<int>({1}),
          "v1 profile with count loads, count ignored");
  bool multi_rejected = false;
  {
    const char *multi = R"({
      "schema_version": 2,
      "profile_id": "multi", "generated_at": "2026-08-23T00:00:00Z", "status": "candidate",
      "source": {"commit": "", "experiment_id": "", "test_id": ""},
      "applicability": {"description": "", "similarity": {"metric": "", "threshold": null, "reported_gap": null}},
      "dynamic": {"enabled": false, "small_step_threads": 1, "large_change_ratio": 0.3, "large_step_threads": 4, "cooldown_seconds": 10},
      "placements": [
        {"id": "multi", "match": {"comm": "worker", "comm_prefix": null, "cgroup": null, "cgroup_prefix": null, "tid": null},
         "allowed_cpus": "0-3", "dynamic": false,
         "affinities": [{"cpus": "0"}, {"cpus": "1"}]}
      ]
    })";
    std::ofstream("/tmp/affinitygraph-multi-affinity.json") << multi;
    try { load_thread_profile("/tmp/affinitygraph-multi-affinity.json", {0, 1, 2, 3}); }
    catch (...) { multi_rejected = true; }
  }
  require(multi_rejected, "multi-affinity rule rejected");
  profile.status = "candidate";
  profile.generated_at = "2026-08-12T00:00:00Z";
  write_thread_profile(profile, "/tmp/affinitygraph-thread-profile-test.json");
  auto roundtrip = load_thread_profile("/tmp/affinitygraph-thread-profile-test.json", {0, 1, 2, 3});
  require(roundtrip.schema_version == 2 && roundtrip.placements.size() == 1 &&
              roundtrip.placements[0].affinities.size() == 1 &&
              roundtrip.placements[0].affinities[0].cpus == std::vector<int>({0}),
          "thread profile export roundtrip (v2, single affinity)");
  auto empty = load_thread_profile("config/thread-profiles/runtime-only-empty.json", {0, 1, 2, 3});
  require(empty.placements.empty() && !empty.dynamic.enabled,
          "runtime-only empty profile loads without placement rules");

  auto cli_override = load_config("tests/runtime.toml", "3-4");
  require(format_cpu_list(cli_override.cpus) == "3-4",
          "command-line CPU override parsed");

  setenv("AFFINITY_CPUS", "5-6", 1);
  auto env_override = load_config("tests/runtime.toml");
  unsetenv("AFFINITY_CPUS");
  require(format_cpu_list(env_override.cpus) == "5-6",
          "environment CPU override parsed");

  auto profile_config = load_config("tests/fixtures/config-thread-profile.toml");
  require(profile_config.thread_profile_path == "/opt/affinitygraph/profiles/db.json" &&
              profile_config.profile_output_path == "/var/log/affinitygraph/profiles/db-candidate.json" &&
              profile_config.experiment_id == "exp-42" &&
              profile_config.test_id == "test-42",
          "thread profile TOML configuration parsed");

  setenv("AFFINITY_THREAD_PROFILE", "/env/profile.json", 1);
  auto env_profile = load_config("tests/fixtures/config-thread-profile.toml");
  unsetenv("AFFINITY_THREAD_PROFILE");
  require(env_profile.thread_profile_path == "/env/profile.json",
          "AFFINITY_THREAD_PROFILE overrides TOML path");
}

void test_identity_reuse_and_demand() {
  GraphWindow graph(60);
  graph.observe_threads({sample(7, 1000000000, 0, 0)});
  graph.observe_threads({sample(7, 2000000000, 500000000, 250000000)});
  auto demand = graph.demands();
  require(demand.size() == 1 && demand[0].demand > 0.74 && demand[0].demand < 0.76, "run+runnable demand");
  graph.observe_relation({.from_tid = 7, .to_tid = 8, .timestamp_ns = 2000000000,
                          .futex_per_second = 1, .active_overlap = 1});
  graph.observe_threads({sample(7, 3000000000, 1, 1, 2)});
  require(graph.demands()[0].demand == 0, "TID generation reset");
  require(graph.edges().empty(), "TID reuse purges stale relationships");
  require(GraphWindow::normalize_group("worker_17") == GraphWindow::normalize_group("worker_29"), "normalized group");
  require(GraphWindow::normalize_group("worker_1", 0x1000, 7, "pool_start") ==
              GraphWindow::normalize_group("worker_2", 0x2000, 99, "pool_start"),
          "resolved symbol stabilizes family across addresses and creators");
  require(GraphWindow::normalize_group("Pipe_normal [wo") ==
              GraphWindow::normalize_group("Pipe_normal") &&
              GraphWindow::normalize_group("Pipe_normal [worker]") ==
                  GraphWindow::normalize_group("Pipe_normal"),
          "truncated bracket role does not split a placement family");
}

void test_relationship_score() {
  GraphWindow graph(2, {1, 1, 1});
  graph.observe_threads({sample(1, 1000000000, 0, 0), sample(2, 1000000000, 0, 0)});
  graph.observe_threads({sample(1, 2000000000, 800000000, 0), sample(2, 2000000000, 800000000, 0)});
  graph.observe_relation({.from_tid = 1, .to_tid = 2, .timestamp_ns = 1000000000,
                          .futex_per_second = 2, .shared_vfs_seconds = 0.5,
                          .active_overlap = 1});
  graph.observe_relation({.from_tid = 2, .to_tid = 1, .timestamp_ns = 2000000000,
                          .futex_per_second = 2, .shared_vfs_seconds = 0.5,
                          .active_overlap = 1});
  auto edges = graph.edges();
  require(edges.size() == 1, "relation aggregation");
  require(edges[0].score > 0 && edges[0].sync > edges[0].share, "relationship components");
}

void test_relation_resident_set() {
  GraphWindow graph(60, {1, 1, 1});
  graph.observe_threads({sample(1, 1000000000, 0, 0),
                         sample(2, 1000000000, 0, 0),
                         sample(3, 1000000000, 0, 0)});
  graph.observe_threads({sample(1, 2000000000, 500000000, 0),
                         sample(2, 2000000000, 500000000, 0),
                         sample(3, 2000000000, 500000000, 0)});
  graph.observe_relation({.from_tid = 1, .to_tid = 2,
                          .timestamp_ns = 2000000000,
                          .futex_per_second = 10, .active_overlap = 1});
  graph.observe_relation({.from_tid = 2, .to_tid = 3,
                          .timestamp_ns = 2000000000,
                          .futex_per_second = 5, .active_overlap = 1});
  require(graph.edges().size() == 2, "resident set fixture has two edges");
  graph.take_delta(0.05, 0.01, 0.01);
  graph.retain_relations({{1, 2}});
  require(graph.edges().size() == 1 && graph.edges()[0].from_tid == 1 &&
              graph.edges()[0].to_tid == 2,
          "resident set evicts unselected edge history");
  auto delta = graph.take_delta(0.05, 0.01, 0.01);
  require(delta.removed_edges == std::vector<std::pair<int, int>>{{2, 3}},
          "resident set eviction is visible in graph delta");
}

void test_solver() {
  std::vector<ThreadDemand> threads;
  for (int tid = 10; tid < 14; ++tid) threads.push_back({{1, tid, 1}, "g", 0.9, 1, tid - 10});
  std::vector<RelationEdge> edges{{10, 11, 1, 1, 0, 1, 100}, {12, 13, 1, 1, 0, 1, 100}};
  auto placement = Solver().solve(hardware(), threads, edges, {1.0, 0.05});
  require(placement.tid_to_cpu.size() == 4, "all TIDs placed");
  std::set<int> cpus;
  for (auto [tid, cpu] : placement.tid_to_cpu) {
    require(cpu >= 0 && cpu < 4, "placement inside envelope");
    cpus.insert(cpu);
  }
  require(cpus.size() == 4, "LPT capacity distribution");
  require(placement.overload == 0 && placement.relation_cost == 2000 &&
              placement.migration_cost == 0,
          "legacy-v1 objective parity golden");
  auto repeat = Solver().solve(hardware(), threads, edges, {1.0, 0.05});
  require(repeat.tid_to_cpu == placement.tid_to_cpu, "deterministic solver");

  for (auto &thread : threads) thread.current_cpu = thread.identity.tid - 10;
  auto held = Solver().solve(hardware(), threads, edges, {0.0, 0.05});
  for (const auto &thread : threads) require(held.tid_to_cpu[thread.identity.tid] == thread.current_cpu, "migration budget");
}

void test_hardware_calibration() {
  auto h = hardware();
  h.load_calibration("calibration/kunpeng920");
  require(h.latency(0, 2) > h.latency(0, 1), "calibrated remote handoff latency");
  require(h.node_distance[{0, 1}] == 12, "calibrated firmware distance");
  require(h.node_calibration[{0, 1}].memory_load_mean_ns > 100 &&
              h.node_calibration[{0, 1}].stream_32t_triad_mbps > 30000,
          "raw memory and STREAM calibration retained");

  const char *eleven = "/tmp/affinitygraph-calibration-11col.csv";
  {
    std::ofstream out(eleven);
    out << "source_node,destination_node,same_socket,numa_distance,"
           "core_handoff_mean_ns,core_handoff_p95_ns,memory_load_mean_ns,"
           "memory_load_cv,stream_2t_triad_mbps,stream_32t_triad_mbps,is_estimated\n"
        << "0,1,0,12,200.0,300.0,400.0,0.05,60000.0,120000.0,true\n"
        << "1,0,0,12,200.0,300.0,400.0,0.05,60000.0,120000.0,false\n"
        << "0,0,1,10,120.0,180.0,250.0,0.04,70000.0,140000.0,true\n"
        << "1,1,1,10,120.0,180.0,250.0,0.04,70000.0,140000.0,true\n";
  }
  HardwareGraph eleven_graph;
  eleven_graph.cpus = {{0, 0, true}, {1, 0, true}, {2, 1, true}, {3, 1, true}};
  eleven_graph.load_calibration(eleven);
  require(eleven_graph.node_calibration[{0, 1}].handoff_p95_ns == 300.0,
          "11-column is_estimated calibration loads");
}

void test_active_cohort_confirmation_signature() {
  std::vector<ThreadDemand> threads{
      {{7, 11, 101}, "active", 0.8, 1, 0},
      {{7, 12, 102}, "inactive", 0.01, 1, 1},
  };
  auto inactive_churn = threads;
  inactive_churn[1].current_cpu = 9;
  require(active_cohort(threads) == active_cohort(inactive_churn),
          "inactive placement churn does not reset cohort confirmation");

  auto newly_active = threads;
  newly_active[1].demand = 0.1;
  require(active_cohort(threads) != active_cohort(newly_active),
          "active cohort membership change resets confirmation");

  auto reused = threads;
  reused[0].identity.starttime++;
  require(active_cohort(threads) != active_cohort(reused),
          "TID generation change resets confirmation");

  std::vector<ThreadDemand> stable;
  for (int tid = 1; tid <= 20; ++tid)
    stable.push_back({{7, tid, 100U + static_cast<uint64_t>(tid)},
                      "g", 0.8, 1, 0});
  auto extended = stable;
  extended.push_back({{7, 21, 121}, "g", 0.8, 1, 0});
  require(active_cohort_continues(active_cohort(stable), active_cohort(extended)),
          "small one-way cohort growth preserves confirmation");
  require(!active_cohort_continues(active_cohort(extended), active_cohort(stable)),
          "cohort removal resets confirmation");
  auto replaced = stable;
  replaced[0].identity.starttime++;
  require(!active_cohort_continues(active_cohort(stable), active_cohort(replaced)),
          "cohort identity replacement resets confirmation");
}

void test_transactional_rollback() {
  FakeBackend backend;
  Actuator actuator(backend);
  Placement p;
  p.tid_to_cpu = {{1, 2}, {2, 3}};
  backend.fail_tid = 2;
  auto result = actuator.apply(p, {1, 2});
  require(!result.success && result.applied == 1, "partial failure reported");
  require(result.requested == 2 && result.committed == 0 &&
          result.rolled_back == 1 && result.rollback_success,
          "rolled-back work is not counted as committed");
  require(backend.masks[1] == std::vector<int>({0, 1}), "completed action rolled back");
  backend.fail_tid = -1;
  require(actuator.apply(p, {1, 2}).success, "successful transaction");
  PlacementDelta delta;
  delta.tid_to_cpu = {{1, 3}, {2, 2}};
  backend.fail_tid = 2;
  auto incremental_failure = actuator.apply(delta, {1, 2});
  require(!incremental_failure.success && incremental_failure.rolled_back == 1,
          "incremental batch failure rolls back completed writes");
  require(backend.masks[1] == std::vector<int>({2}),
          "incremental rollback restores pre-batch singleton placement");
  backend.fail_tid = -1;
  PlacementDelta masks;
  masks.tid_to_mask = {{1, {2, 3}}, {2, {0, 1}}};
  require(actuator.apply(masks, {1, 2}).success &&
              backend.masks[1] == std::vector<int>({2, 3}),
          "actuator accepts complete CPU masks");
  auto restored = actuator.restore_all();
  require(restored.success() && restored.restored == 2, "restore succeeds");
  require(backend.masks[1] == std::vector<int>({0, 1}), "original mask restored");
}

NumaDomainProposal domain_windows(
    NumaDomainSolver &solver, const HardwareGraph &h,
    const std::vector<ThreadDemand> &threads,
    const std::vector<RelationEdge> &edges, int windows,
    NumaDomainOptions options = {},
    const std::map<int, std::vector<int>> &allowed = {}) {
  NumaDomainProposal proposal;
  for (int window = 1; window <= windows; ++window) {
    proposal = solver.propose(h, threads, edges, allowed, options,
                              static_cast<uint64_t>(window) * 10000000000ULL);
    if (window != windows) solver.discard(proposal);
  }
  return proposal;
}

void test_numa_domain_family_selection_and_merge() {
  HardwareGraph h;
  for (int cpu = 0; cpu < 16; ++cpu) h.cpus.push_back({cpu, cpu / 4, true});
  std::vector<ThreadDemand> threads{
      {{1, 1, 1}, "Pipe_normal|pipe", 0.7, 1, 8},
      {{1, 2, 2}, "Pipe_normal|pipe", 0.7, 1, 9},
      {{1, 3, 3}, "brpc_light|worker", 0.6, 1, 10},
      {{1, 4, 4}, "brpc_light|worker", 0.6, 1, 11},
      {{1, 5, 5}, "brpc_heavy|handler", 1.0, 1, 0},
      {{1, 6, 6}, "brpc_heavy|handler", 1.0, 1, 1},
      {{1, 7, 7}, "background|worker", 0.2, 1, 2},
      {{1, 8, 8}, "background|worker", 0.2, 1, 3},
  };
  std::vector<RelationEdge> edges{
      {1, 2, 0, 0, 0, 1, 10}, {3, 4, 0, 0, 0, 1, 1.5},
      {1, 3, 0, 0, 0, 1, 8},  {5, 1, 0, 0, 0, 1, 8},
      {7, 8, 0, 0, 0, 1, 20},
  };
  NumaDomainSolver solver;
  auto proposal = domain_windows(solver, h, threads, edges, 7);
  require(proposal.ready && proposal.valid && proposal.domains.size() == 1,
          "stable related anchors form one domain");
  require(proposal.domains[0].families ==
              std::vector<std::string>({"Pipe_normal|pipe", "brpc_light|worker"}),
          "worker families selected without a name whitelist");
  require(proposal.domains[0].tids == std::vector<int>({1, 2, 3, 4}) &&
              proposal.domains[0].target_nodes.size() == 1 &&
              proposal.domains[0].target_mask.size() == 4,
          "external-edge handler excluded and low demand uses one node mask");
  const auto metric = [&](const std::string &name) -> const FamilyMetric & {
    return *std::find_if(proposal.families.begin(), proposal.families.end(),
                         [&](const auto &value) { return value.name == name; });
  };
  require(!metric("brpc_light|worker").cohesive_anchor &&
              metric("brpc_light|worker").cross_seed &&
              metric("brpc_light|worker").anchor,
          "strong cross-family evidence seeds a non-cohesive worker family");
  require(metric("brpc_light|worker").demand_eligible &&
              metric("brpc_light|worker").internal_relation_eligible &&
              !metric("brpc_light|worker").self_containment_eligible &&
              !metric("brpc_light|worker").relative_internal_eligible &&
              !metric("brpc_light|worker").cohesive_eligible,
          "family diagnostics identify each failed cohesive gate");
  require(!metric("brpc_heavy|handler").cross_seed &&
              !metric("brpc_heavy|handler").anchor,
          "external-only handler cannot seed without internal evidence");
  const auto pair = std::find_if(
      proposal.family_pairs.begin(), proposal.family_pairs.end(),
      [](const auto &value) {
        return value.left == "Pipe_normal|pipe" &&
               value.right == "brpc_light|worker";
      });
  require(pair != proposal.family_pairs.end() &&
              pair->cross_relation == 8 && pair->denominator == 1.5 &&
              pair->merge_ratio > 5.33 && pair->endpoints_eligible &&
              pair->qualifies && pair->confirmation >= 3 && pair->confirmed,
          "pair diagnostics expose aggregate ratio and confirmation");
  require(proposal.input_family_count == 4 &&
              proposal.input_cross_family_pair_count == 2 &&
              proposal.selected_family_pair_count == 2 &&
              std::abs(proposal.total_demand - 5.0) < 1e-12 &&
              std::abs(proposal.total_relation - 47.5) < 1e-12,
          "proposal diagnostics preserve selector input totals");
  require(proposal.domains[0].previous_nodes.empty() &&
              proposal.domains[0].target_nodes == std::vector<int>({2}) &&
              proposal.domains[0].online_cpu_count == 4 &&
              std::abs(proposal.domains[0].capacity_limit - 3.2) < 1e-12 &&
              std::abs(proposal.domains[0].capacity_headroom - 0.6) < 1e-12 &&
              proposal.domains[0].initial_migrations == 0 &&
              proposal.domains[0].node_decision == "initial",
          "node diagnostics expose capacity, migration, and decision reason");
  solver.commit(proposal, 70000000000ULL);
  auto stable = domain_windows(solver, h, threads, edges, 1);
  require(stable.ready && stable.actions.empty() &&
              stable.released_tids.empty() &&
              stable.domains[0].previous_nodes == std::vector<int>({2}) &&
              stable.domains[0].node_decision == "stable",
          "static domain produces no steady-state CPU or mask churn");
  solver.discard(stable);
}

void test_numa_domain_clickhouse_negative_control() {
  auto h = hardware();
  std::vector<ThreadDemand> threads{
      {{1, 11, 11}, "ThreadPool|worker", 0.7, 1, 0},
      {{1, 12, 12}, "ThreadPool|worker", 0.7, 1, 1},
      {{1, 13, 13}, "MySQLHandler|handler", 0.6, 1, 2},
      {{1, 14, 14}, "QueryPipelineEx|child", 0.6, 1, 3},
      {{1, 15, 15}, "Common|worker", 0.6, 1, 2},
      {{1, 16, 16}, "TCPHandler|handler", 0.6, 1, 3},
  };
  std::vector<RelationEdge> edges{
      {11, 12, 0, 0, 0, 1, 10}, {11, 13, 0, 0, 0, 1, 15},
      {12, 14, 0, 0, 0, 1, 15}, {11, 15, 0, 0, 0, 1, 10},
      {12, 16, 0, 0, 0, 1, 10},
  };
  NumaDomainSolver solver;
  auto proposal = domain_windows(solver, h, threads, edges, 5);
  require(!proposal.ready && proposal.domains.empty(),
          "external-heavy ClickHouse evidence does not force a domain");
  const auto thread_pool = std::find_if(
      proposal.families.begin(), proposal.families.end(),
      [](const auto &value) { return value.name == "ThreadPool|worker"; });
  require(thread_pool != proposal.families.end() &&
              !thread_pool->cohesive_anchor && !thread_pool->cross_seed &&
              !thread_pool->anchor,
          "ThreadPool fails containment and external-only handlers cannot seed it");
  solver.discard(proposal);
}

void test_numa_domain_pending_merge_suppresses_singleton() {
  auto h = hardware();
  std::vector<ThreadDemand> threads{
      {{1, 1, 1}, "family-a", 0.6, 1, 0},
      {{1, 2, 2}, "family-a", 0.6, 1, 1},
      {{1, 3, 3}, "family-b", 0.6, 1, 2},
      {{1, 4, 4}, "family-b", 0.6, 1, 3},
  };
  std::vector<RelationEdge> edges{
      {1, 2, 0, 0, 0, 1, 10},
      {3, 4, 0, 0, 0, 1, 1.5},
      {1, 3, 0, 0, 0, 1, 8},
  };
  NumaDomainOptions options;
  options.family_stability_confirmations = 1;
  options.domain_stability_confirmations = 3;
  options.plan_confirmations = 1;
  NumaDomainSolver solver;

  for (int window = 1; window <= 2; ++window) {
    auto pending = solver.propose(
        h, threads, edges, {}, options,
        static_cast<uint64_t>(window) * 10000000000ULL);
    const auto family_a = std::find_if(
        pending.families.begin(), pending.families.end(),
        [](const auto &family) { return family.name == "family-a"; });
    require(family_a != pending.families.end() &&
                family_a->cohesive_anchor && family_a->cross_pending &&
                !family_a->anchor && pending.domains.empty() && !pending.ready,
            "pending strong pair suppresses an intermediate singleton domain");
    solver.discard(pending);
  }

  auto confirmed = solver.propose(h, threads, edges, {}, options, 30000000000ULL);
  require(confirmed.ready && confirmed.domains.size() == 1 &&
              confirmed.domains[0].families ==
                  std::vector<std::string>({"family-a", "family-b"}),
          "confirmed pair becomes one atomic domain without singleton churn");
  solver.discard(confirmed);
}

void test_numa_domain_dwell_holds_transient_relation_gap() {
  auto h = hardware();
  std::vector<ThreadDemand> threads{
      {{1, 1, 1}, "family-a", 0.6, 1, 0},
      {{1, 2, 2}, "family-a", 0.6, 1, 1},
      {{1, 3, 3}, "family-b", 0.6, 1, 2},
      {{1, 4, 4}, "family-b", 0.6, 1, 3},
  };
  std::vector<RelationEdge> edges{
      {1, 2, 0, 0, 0, 1, 10},
      {3, 4, 0, 0, 0, 1, 2},
      {1, 3, 0, 0, 0, 1, 8},
  };
  NumaDomainOptions options;
  options.family_stability_confirmations = 1;
  options.domain_stability_confirmations = 1;
  options.plan_confirmations = 1;
  options.minimum_dwell_ns = 100;
  NumaDomainSolver solver;

  auto initial = solver.propose(h, threads, edges, {}, options, 10);
  require(initial.ready && initial.domains.size() == 1,
          "dwell fixture creates a joint domain");
  solver.commit(initial, 10);

  auto idle_threads = threads;
  for (auto &thread : idle_threads) thread.demand = 0;
  auto held = solver.propose(h, idle_threads, {}, {}, options, 50);
  require(held.ready && held.domains.size() == 1 &&
              held.domains[0].families ==
                  std::vector<std::string>({"family-a", "family-b"}) &&
              held.domains[0].node_decision == "held_domain_dwell" &&
              held.planned_masks.size() == 4 && held.actions.empty() &&
              held.released_tids.empty(),
          "transient relation gap keeps the committed domain during dwell");
  solver.discard(held);

  auto expired = solver.propose(h, idle_threads, {}, {}, options, 111);
  require(expired.ready && expired.domains.empty() &&
              expired.released_tids == std::set<int>({1, 2, 3, 4}),
          "domain can release after dwell expires with no surviving evidence");
  solver.discard(expired);
}

void test_numa_domain_recent_evidence_refreshes_release_dwell() {
  auto h = hardware();
  std::vector<ThreadDemand> threads{
      {{1, 1, 1}, "family-a", 0.6, 1, 0},
      {{1, 2, 2}, "family-a", 0.6, 1, 1},
      {{1, 3, 3}, "family-b", 0.6, 1, 2},
      {{1, 4, 4}, "family-b", 0.6, 1, 3},
  };
  std::vector<RelationEdge> edges{
      {1, 2, 0, 0, 0, 1, 10},
      {3, 4, 0, 0, 0, 1, 2},
      {1, 3, 0, 0, 0, 1, 8},
  };
  NumaDomainOptions options;
  options.family_stability_confirmations = 1;
  options.domain_stability_confirmations = 1;
  options.plan_confirmations = 1;
  options.minimum_dwell_ns = 100;
  NumaDomainSolver solver;

  auto initial = solver.propose(h, threads, edges, {}, options, 10);
  solver.commit(initial, 10);

  // placement 已远早于 dwell，但完整关系证据刚被再次确认。
  auto reconfirmed = solver.propose(h, threads, edges, {}, options, 1000);
  require(reconfirmed.ready && reconfirmed.domains.size() == 1 &&
              reconfirmed.domains[0].node_decision == "stable",
          "stable evidence reconfirms an old committed domain");
  solver.commit(reconfirmed, 1000);

  auto idle_threads = threads;
  for (auto &thread : idle_threads) thread.demand = 0;
  auto held = solver.propose(h, idle_threads, {}, {}, options, 1050);
  require(held.ready && held.domains.size() == 1 &&
              held.domains[0].families ==
                  std::vector<std::string>({"family-a", "family-b"}) &&
              held.domains[0].node_decision == "held_domain_dwell",
          "recent complete evidence holds a domain despite old placement");
  solver.commit(held, 1050);

  auto expired = solver.propose(h, idle_threads, {}, {}, options, 1101);
  require(expired.ready && expired.domains.empty() &&
              expired.released_tids == std::set<int>({1, 2, 3, 4}),
          "domain releases after continuous evidence absence reaches dwell");
  solver.discard(expired);
}

void test_numa_domain_capacity_dwell_uses_node_change_time() {
  HardwareGraph h;
  for (int cpu = 0; cpu < 8; ++cpu) h.cpus.push_back({cpu, cpu / 4, true});
  std::vector<ThreadDemand> threads;
  for (int tid = 1; tid <= 4; ++tid)
    threads.push_back({{1, tid, static_cast<uint64_t>(tid)}, "family-a", 1.0,
                       1, tid - 1});
  const std::vector<RelationEdge> edges{
      {1, 2, 0, 0, 0, 1, 5}, {2, 3, 0, 0, 0, 1, 5},
      {3, 4, 0, 0, 0, 1, 5}};
  NumaDomainOptions options;
  options.family_stability_confirmations = 1;
  options.plan_confirmations = 1;
  options.shrink_confirmations = 1;
  options.minimum_dwell_ns = 100;
  NumaDomainSolver solver;

  auto initial = solver.propose(h, threads, edges, {}, options, 10);
  require(initial.domains[0].target_nodes.size() == 2,
          "capacity fixture initially needs two nodes");
  solver.commit(initial, 10);
  auto reconfirmed = solver.propose(h, threads, edges, {}, options, 1000);
  solver.commit(reconfirmed, 1000);

  for (auto &thread : threads) thread.demand = 0.5;
  auto shrunk = solver.propose(h, threads, edges, {}, options, 1001);
  require(shrunk.ready && shrunk.domains.size() == 1 &&
              shrunk.domains[0].target_nodes.size() == 1 &&
              shrunk.domains[0].node_decision == "shrunk",
          "capacity shrink dwell remains tied to the last node change");
  solver.discard(shrunk);
}

void test_numa_domain_dwell_allows_superset_merge() {
  auto h = hardware();
  NumaDomainOptions options;
  options.family_stability_confirmations = 1;
  options.domain_stability_confirmations = 1;
  options.plan_confirmations = 1;
  options.minimum_dwell_ns = 100;
  NumaDomainSolver solver;
  std::vector<ThreadDemand> singleton_threads{
      {{1, 1, 1}, "family-a", 0.6, 1, 0},
      {{1, 2, 2}, "family-a", 0.6, 1, 1},
  };
  auto singleton = solver.propose(
      h, singleton_threads, {{1, 2, 0, 0, 0, 1, 10}}, {}, options, 10);
  require(singleton.ready && singleton.domains.size() == 1,
          "superset fixture creates a singleton domain");
  solver.commit(singleton, 10);

  std::vector<ThreadDemand> pair_threads{
      singleton_threads[0], singleton_threads[1],
      {{1, 3, 3}, "family-b", 0.6, 1, 2},
      {{1, 4, 4}, "family-b", 0.6, 1, 3},
  };
  std::vector<RelationEdge> pair_edges{
      {1, 2, 0, 0, 0, 1, 10},
      {3, 4, 0, 0, 0, 1, 2},
      {1, 3, 0, 0, 0, 1, 8},
  };
  auto merged = solver.propose(h, pair_threads, pair_edges, {}, options, 20);
  require(merged.ready && merged.domains.size() == 1 &&
              merged.domains[0].families ==
                  std::vector<std::string>({"family-a", "family-b"}) &&
              merged.domains[0].node_decision != "held_domain_dwell",
          "domain dwell permits a confirmed superset merge");
  solver.discard(merged);
}

void test_numa_domain_aggregates_before_family_pruning() {
  auto h = hardware();
  std::vector<ThreadDemand> threads{
      {{1, 21, 21}, "family-a", 0.6, 1, 0},
      {{1, 22, 22}, "family-a", 0.6, 1, 1},
      {{1, 31, 31}, "family-b", 0.2, 1, 2},
      {{1, 32, 32}, "family-b", 0.2, 1, 3},
      {{1, 33, 33}, "family-b", 0.2, 1, 0},
      {{1, 34, 34}, "family-b", 0.2, 1, 1},
      {{1, 35, 35}, "family-b", 0.2, 1, 2},
      {{1, 36, 36}, "family-b", 0.2, 1, 3},
  };
  std::vector<RelationEdge> edges{
      {21, 22, 0, 0, 0, 1, 1}, {31, 32, 0, 0, 0, 1, 0.5},
      {33, 34, 0, 0, 0, 1, 0.5},
      {21, 31, 0, 0, 0, 1, 0.075},
      {21, 32, 0, 0, 0, 1, 0.075},
      {21, 33, 0, 0, 0, 1, 0.075},
      {21, 34, 0, 0, 0, 1, 0.075},
      {21, 35, 0, 0, 0, 1, 0.075},
      {21, 36, 0, 0, 0, 1, 0.075},
  };
  NumaDomainOptions options;
  options.family_edges_per_family = 1;
  options.family_stability_confirmations = 1;
  options.domain_stability_confirmations = 1;
  options.plan_confirmations = 1;
  NumaDomainSolver solver;
  auto proposal = domain_windows(solver, h, threads, edges, 1, options);
  require(proposal.ready && proposal.domains.size() == 1 &&
              proposal.domains[0].families ==
                  std::vector<std::string>({"family-a", "family-b"}),
          "distributed TID evidence is aggregated before family heavy-hitter pruning");
  solver.discard(proposal);
}

void test_numa_domain_capacity_atomicity_and_envelope() {
  HardwareGraph h;
  for (int cpu = 0; cpu < 8; ++cpu) h.cpus.push_back({cpu, cpu / 4, true});
  std::vector<ThreadDemand> threads;
  for (int tid = 1; tid <= 4; ++tid)
    threads.push_back({{1, tid, static_cast<uint64_t>(tid)}, "ThreadPool|worker",
                       1.0, 1, tid - 1});
  std::vector<RelationEdge> edges{{1, 2, 0, 0, 0, 1, 5},
                                  {2, 3, 0, 0, 0, 1, 5},
                                  {3, 4, 0, 0, 0, 1, 5}};
  NumaDomainOptions options;
  options.family_stability_confirmations = 1;
  options.plan_confirmations = 1;
  NumaDomainSolver solver;
  std::map<int, std::vector<int>> allowed;
  for (int tid = 1; tid <= 4; ++tid) allowed[tid] = {1, 2, 3, 4, 5, 6};
  auto proposal = domain_windows(solver, h, threads, edges, 1, options, allowed);
  require(proposal.ready && proposal.domains[0].target_nodes.size() == 2,
          "demand above one-node 80 percent capacity expands to two nodes");
  require(proposal.planned_masks.at(1) ==
              std::vector<int>({1, 2, 3, 4, 5, 6}),
          "node mask intersects the resource and application envelope");
  solver.discard(proposal);

  options.maximum_threads_per_domain = 3;
  proposal = domain_windows(solver, h, threads, edges, 1, options, allowed);
  require(!proposal.valid && proposal.delta.tid_to_mask.empty() &&
              proposal.domains[0].invalid_reason ==
                  "maximum_threads_per_domain_exceeded",
          "oversized domain is rejected atomically without partial binding");
  solver.discard(proposal);

  options.maximum_threads_per_domain = 1024;
  allowed[2] = {99};
  proposal = domain_windows(solver, h, threads, edges, 1, options, allowed);
  require(!proposal.valid && proposal.delta.tid_to_mask.empty(),
          "empty application-mask intersection invalidates the whole plan");
  solver.discard(proposal);
}

void test_incremental_initial_node_plan() {
  HardwareGraph h;
  for (int cpu = 0; cpu < 8; ++cpu) h.cpus.push_back({cpu, cpu / 4, true});
  h.node_distance = {{{0, 0}, 10}, {{0, 1}, 30},
                     {{1, 0}, 30}, {{1, 1}, 10}};
  std::vector<ThreadDemand> threads{
      {{1, 1, 11}, "a", 0.4, 1, 0},
      {{1, 2, 12}, "a", 0.4, 1, 4},
      {{1, 3, 13}, "b", 0.4, 1, 1},
      {{1, 4, 14}, "b", 0.4, 1, 5},
  };
  std::vector<RelationEdge> edges{
      {1, 2, 1, 1, 0, 1, 100},
      {3, 4, 1, 1, 0, 1, 100},
  };
  IncrementalOptions options;
  options.maximum_migrated_threads_ratio = 0.5;
  options.proposal_confirmations = 2;
  options.initial_migrated_threads_ratio = 0.5;
  options.initial_node_thread_slack_ratio = 1.0;
  options.initial_proposal_confirmations = 2;
  options.minimum_dwell_ns = 100;
  IncrementalSolver solver(true);
  auto first = solver.propose(h, threads, edges, options, 1000);
  require(first.phase == IncrementalPhase::NodePlanning &&
              first.confirmation == 1 && !first.ready,
          "initial NUMA plan starts with whole-plan confirmation");
  auto confirmed = solver.propose(h, threads, edges, options, 1010);
  require(confirmed.phase == IncrementalPhase::InitialPinning &&
              confirmed.initial_plan_confirmed && !confirmed.ready,
          "whole initial NUMA plan confirms once");
  auto batch1 = solver.propose(h, threads, edges, options, 1020);
  require(batch1.ready && batch1.actions.size() == 2,
          "initial pin uses total eligible-thread budget");
  bool cross_node = false;
  for (const auto &action : batch1.actions)
    cross_node |= h.cpus[action.from_cpu].node != h.cpus[action.target_cpu].node;
  require(cross_node, "bad CFS NUMA baseline corrected during initial pin");
  solver.commit(batch1, 1020);
  require(!solver.effective(), "partial initial pin is not active effective");
  auto batch2 = solver.propose(h, threads, edges, options, 1030);
  require(batch2.ready && batch2.actions.size() == 2,
          "confirmed initial plan proceeds without reconfirming each batch");
  solver.commit(batch2, 1030);
  require(solver.effective() && solver.pinned_threads() == threads.size(),
          "all singleton initial pins make solver effective");
  require(h.cpus[solver.placement().at(1)].node ==
              h.cpus[solver.placement().at(2)].node &&
              h.cpus[solver.placement().at(3)].node ==
              h.cpus[solver.placement().at(4)].node,
          "strong pairs become NUMA local in initial plan");
}

void test_sparse_hotspot_communities_cluster_by_node() {
  HardwareGraph h;
  for (int cpu = 0; cpu < 16; ++cpu) h.cpus.push_back({cpu, cpu / 4, true});
  for (int node = 0; node < 4; ++node)
    for (int other = 0; other < 4; ++other)
      h.node_distance[{node, other}] = node == other ? 10 : 30;
  std::vector<ThreadDemand> threads;
  for (int tid = 1; tid <= 16; ++tid)
    threads.push_back({{1, tid, static_cast<uint64_t>(tid)}, "worker", 0.2,
                       1, (tid * 5) % 16});
  std::vector<RelationEdge> edges;
  for (int first = 1; first <= 16; ++first)
    for (int second = first + 1; second <= 16; ++second) {
      bool same_hotspot = (first - 1) / 4 == (second - 1) / 4;
      edges.push_back({first, second, 1,
                       same_hotspot ? 1.0 : 0.0, 0, 1,
                       same_hotspot ? 100.0 : 1.0});
    }
  IncrementalOptions options;
  options.maximum_migrated_threads_ratio = 1;
  options.initial_migrated_threads_ratio = 1;
  options.proposal_confirmations = 1;
  options.hotspot_edges_per_thread = 4;
  options.hotspot_edge_quantile = 0.95;
  options.hotspot_component_boost = 4;
  options.maximum_threads_per_cpu = 2;
  std::map<int, ThreadDemand> thread_map;
  for (const auto &thread : threads) thread_map[thread.identity.tid] = thread;
  auto selected = select_hotspot_edges(edges, thread_map, options);
  require(selected.size() < edges.size() / 2,
          "dense weak background is removed from hotspot graph");
  for (int community = 0; community < 4; ++community)
    for (int first = community * 4 + 1; first <= community * 4 + 4; ++first)
      for (int second = first + 1; second <= community * 4 + 4; ++second)
        require(std::any_of(selected.begin(), selected.end(), [&](const auto &edge) {
          return edge.from_tid == first && edge.to_tid == second;
        }), "all strong hotspot edges survive sparse selection");

  auto solve = [&] {
    IncrementalSolver solver(true);
    auto plan = solver.propose(h, threads, edges, options, 1);
    require(plan.phase == IncrementalPhase::InitialPinning,
            "hotspot communities receive an initial node plan");
    auto pin = solver.propose(h, threads, edges, options, 2);
    require(pin.ready && pin.actions.size() == threads.size(),
            "hotspot initial placement pins the full cohort");
    solver.commit(pin, 2);
    return solver.placement();
  };
  auto placement = solve();
  require(placement == solve(), "hotspot community placement is deterministic");
  std::map<int, int> cpu_threads;
  for (const auto &[_, cpu] : placement) ++cpu_threads[cpu];
  bool colocated_pair = false;
  for (const auto &[_, count] : cpu_threads)
    require(count <= 2, "hotspot placement enforces the explicit CPU slot cap");
  for (const auto &[_, count] : cpu_threads)
    colocated_pair = colocated_pair || count == 2;
  require(colocated_pair,
          "low-demand strong relationships can share a CPU for locality");
  for (int community = 0; community < 4; ++community) {
    int node = h.cpus[placement.at(community * 4 + 1)].node;
    for (int tid = community * 4 + 2; tid <= community * 4 + 4; ++tid) {
      int actual = h.cpus[placement.at(tid)].node;
      std::string message =
          "strong hotspot community remains within one NUMA node: community=" +
          std::to_string(community) + " tid=" + std::to_string(tid) +
          " expected=" + std::to_string(node) +
          " actual=" + std::to_string(actual);
      require(actual == node, message.c_str());
    }
  }
}

void test_managed_hotspot_selection_is_bounded_and_stable() {
  std::vector<ThreadDemand> threads;
  for (int tid = 1; tid <= 6; ++tid)
    threads.push_back({{1, tid, static_cast<uint64_t>(tid)}, "worker",
                       0.7 - tid * 0.05, 1, tid - 1});
  IncrementalOptions options;
  options.maximum_managed_threads = 4;
  options.managed_thread_hysteresis_ratio = 0.5;
  auto initial = select_managed_threads(threads, {}, {}, options);
  require(initial.size() == 4 && initial.back().identity.tid == 4,
          "managed cohort is bounded by hotspot policy");
  auto retained = select_managed_threads(threads, {}, {6}, options);
  require(retained.size() == 4 &&
              std::any_of(retained.begin(), retained.end(), [](const auto &thread) {
                return thread.identity.tid == 6;
              }),
          "managed cohort hysteresis retains an existing boundary thread");
}

void test_managed_selection_keeps_strong_community_intact() {
  std::vector<ThreadDemand> threads;
  for (int tid = 1; tid <= 16; ++tid)
    threads.push_back({{1, tid, static_cast<uint64_t>(tid)}, "worker", 0.2,
                       1, tid - 1});
  std::vector<RelationEdge> edges;
  for (int first = 1; first <= 4; ++first)
    for (int second = first + 1; second <= 4; ++second)
      edges.push_back({first, second, 1, 1, 0, 1, 100});
  // A high-degree bipartite background makes vertex-degree truncation select
  // boundary vertices even though its best six-node internal weight is lower.
  for (int center = 5; center <= 8; ++center)
    for (int leaf = 9; leaf <= 16; ++leaf)
      edges.push_back({center, leaf, 1, 1, 0, 1, 60});
  IncrementalOptions options;
  options.maximum_managed_threads = 6;
  options.hotspot_edge_quantile = 0;
  options.hotspot_component_boost = 0;
  auto selected = select_managed_threads(threads, edges, {}, options);
  std::set<int> tids;
  for (const auto &thread : selected) tids.insert(thread.identity.tid);
  require(selected.size() == 6, "community selection respects managed limit");
  for (int tid = 1; tid <= 4; ++tid)
    require(tids.contains(tid),
            "strong hotspot community is not cut at managed boundary");
  std::set<int> repeated_tids;
  for (const auto &thread : select_managed_threads(threads, edges, {}, options))
    repeated_tids.insert(thread.identity.tid);
  require(repeated_tids == tids,
          "community-aware managed selection is deterministic");
}

void test_initial_confirmation_freezes_candidate_for_stable_population() {
  auto h = hardware();
  std::vector<ThreadDemand> threads;
  for (int tid = 1; tid <= 4; ++tid)
    threads.push_back({{1, tid, static_cast<uint64_t>(tid)}, "g", 0.2, 1,
                       tid - 1});
  IncrementalOptions options;
  options.maximum_migrated_threads_ratio = 1;
  options.proposal_confirmations = 3;
  options.initial_proposal_confirmations = 3;
  IncrementalSolver solver;
  auto first = solver.propose(
      h, threads, {{1, 3, 1, 1, 0, 1, 100}}, options, 1);
  auto second = solver.propose(
      h, threads, {{1, 2, 1, 1, 0, 1, 200}}, options, 2);
  auto third = solver.propose(
      h, threads, {{3, 4, 1, 1, 0, 1, 300}}, options, 3);
  require(first.confirmation == 1 && second.confirmation == 2,
          "stable population advances whole-plan confirmation");
  require(third.initial_plan_confirmed &&
              third.phase == IncrementalPhase::InitialPinning,
          "dynamic relationship weights cannot starve initial confirmation");
}

void test_incremental_arrival_does_not_cancel_global_replan() {
  HardwareGraph h;
  for (int cpu = 0; cpu < 8; ++cpu) h.cpus.push_back({cpu, cpu / 4, true});
  for (int node = 0; node < 2; ++node)
    for (int other = 0; other < 2; ++other)
      h.node_distance[{node, other}] = node == other ? 10 : 30;
  std::vector<ThreadDemand> threads;
  for (int tid = 1; tid <= 4; ++tid)
    threads.push_back({{1, tid, static_cast<uint64_t>(tid)}, "worker", 0.2,
                       1, tid - 1});
  IncrementalOptions options;
  options.initial_migrated_threads_ratio = 1;
  options.maximum_migrated_threads_ratio = 1;
  options.hotspot_replan_min_threads = 2;
  options.hotspot_replan_growth_ratio = 0.25;
  IncrementalSolver solver(true);
  solver.propose(h, threads, {}, options, 1);
  auto initial = solver.propose(h, threads, {}, options, 2);
  solver.commit(initial, 2);
  for (int tid = 5; tid <= 8; ++tid)
    threads.push_back({{1, tid, static_cast<uint64_t>(tid)}, "worker", 0.2,
                       1, tid - 1});
  auto replan = solver.propose(h, threads, {}, options, 3);
  require(replan.phase == IncrementalPhase::InitialPinning &&
              replan.global_replan_active,
          "substantial cohort growth starts a global node replan");
  threads.push_back({{1, 9, 9}, "worker", 0.2, 1, 0});
  auto arrival = solver.propose(h, threads, {}, options, 4);
  require(arrival.phase == IncrementalPhase::InitialPinning &&
              arrival.global_replan_active,
          "small arrival cannot cancel an in-flight global node replan");
}

void test_incremental_identity_and_reset() {
  IncrementalSolver solver;
  IncrementalOptions options;
  options.maximum_migrated_threads_ratio = 1;
  options.proposal_confirmations = 1;
  std::vector<ThreadDemand> threads{{{1, 7, 70}, "g", 0.2, 1, 0}};
  auto planned = solver.propose(hardware(), threads, {}, options, 1);
  require(planned.phase == IncrementalPhase::InitialPinning,
          "single-window initial plan confirmation");
  auto pin = solver.propose(hardware(), threads, {}, options, 2);
  solver.commit(pin, 2);
  require(solver.effective(), "single thread initial pin commits");
  threads[0].identity.starttime = 71;
  auto reused = solver.propose(hardware(), threads, {}, options, 3);
  require(reused.phase == IncrementalPhase::InitialPinning &&
              solver.pinned_threads() == 0,
          "TID generation reuse cannot inherit committed placement");
  solver.reset();
  require(solver.phase() == IncrementalPhase::Uninitialized &&
              solver.placement().empty(),
          "pause or restore resets placement history");
}

void test_graph_delta_thresholds() {
  GraphWindow graph(60, {1, 1, 1});
  graph.observe_threads({sample(1, 1000000000, 0, 0),
                         sample(2, 1000000000, 0, 0)});
  graph.observe_threads({sample(1, 2000000000, 200000000, 0),
                         sample(2, 2000000000, 200000000, 0)});
  auto initial = graph.take_delta(0.05, 1.0, 0.1);
  require(initial.upsert_threads.size() == 2,
          "first graph delta contains current threads");
  require(graph.take_delta(0.05, 1.0, 0.1).dirty_tids().empty(),
          "unchanged cached graph emits no dirty frontier");
  graph.observe_threads({sample(1, 3000000000, 210000000, 0),
                         sample(2, 3000000000, 210000000, 0)});
  require(graph.take_delta(0.05, 1.0, 0.1).upsert_threads.empty(),
          "sub-threshold demand noise is suppressed");
  graph.observe_threads({sample(1, 4000000000, 1000000000, 0),
                         sample(2, 4000000000, 1000000000, 0)});
  graph.observe_threads({sample(1, 5000000000, 1900000000, 0),
                         sample(2, 5000000000, 1900000000, 0)});
  require(!graph.take_delta(0.05, 1.0, 0.1).upsert_threads.empty(),
          "material accumulated demand change becomes dirty");
}

void test_incremental_per_thread_cooldown() {
  auto h = hardware();
  std::vector<ThreadDemand> threads;
  for (int tid = 1; tid <= 4; ++tid)
    threads.push_back({{1, tid, static_cast<uint64_t>(tid)}, "g", 0.2, 1,
                       tid - 1});
  IncrementalOptions options;
  options.maximum_migrated_threads_ratio = 1;
  options.proposal_confirmations = 1;
  options.minimum_dwell_ns = 1000000;
  IncrementalSolver solver;
  solver.propose(h, threads, {}, options, 1);
  auto pins = solver.propose(h, threads, {}, options, 2);
  solver.commit(pins, 2);
  require(solver.effective(), "cooldown fixture initialized");

  std::vector<RelationEdge> first_edges{{1, 3, 1, 1, 0, 1, 100}};
  auto move = solver.propose(h, threads, first_edges, options, 10);
  require(move.ready && !move.actions.empty(),
          "material cross-node relation creates local move");
  std::set<int> moved;
  for (const auto &action : move.actions) moved.insert(action.identity.tid);
  solver.commit(move, 10);

  std::vector<RelationEdge> reversed{{1, 2, 1, 1, 0, 1, 1000},
                                     {3, 4, 1, 1, 0, 1, 1000}};
  auto cooled = solver.propose(h, threads, reversed, options, 11);
  require(cooled.cooldown_skipped_threads > 0,
          "cooldown suppression is exposed in proposal telemetry");
  for (const auto &action : cooled.actions)
    require(!moved.contains(action.identity.tid),
            "recently moved TID cannot reverse during per-thread dwell");
}

void test_incremental_721_thread_initial_scale() {
  HardwareGraph h;
  std::vector<ThreadDemand> threads;
  for (int cpu = 0; cpu < 128; ++cpu) h.cpus.push_back({cpu, cpu / 32, true});
  for (int node = 0; node < 4; ++node)
    for (int other = 0; other < 4; ++other)
      h.node_distance[{node, other}] = node == other ? 10 : 20;
  for (int index = 0; index < 721; ++index)
    threads.push_back({{1, 3000 + index, static_cast<uint64_t>(index + 1)},
                       "pool", index % 17 == 0 ? 0.5 : 0.01, 1,
                       index % 128});
  IncrementalOptions options;
  options.maximum_migrated_threads_ratio = 0.25;
  options.proposal_confirmations = 1;
  IncrementalSolver solver(true);
  auto started = std::chrono::steady_clock::now();
  auto planned = solver.propose(h, threads, {}, options, 1);
  auto elapsed = std::chrono::steady_clock::now() - started;
  require(planned.phase == IncrementalPhase::InitialPinning,
          "721-thread initial node plan completes");
  require(elapsed < std::chrono::seconds(1),
          "721-thread initial node plan below one second");
  auto batch = solver.propose(h, threads, {}, options, 2);
  require(batch.actions.size() == 721,
          "initial pin can arm the complete cohort in one batch");
}

void test_initial_cpu_capacity_repair() {
  auto h = hardware();
  std::vector<ThreadDemand> threads{
      {{1, 1, 1}, "a", 0.6, 1, 0},
      {{1, 2, 2}, "a", 0.6, 1, 0},
      {{1, 3, 3}, "b", 0.6, 1, 2},
      {{1, 4, 4}, "b", 0.6, 1, 2},
  };
  IncrementalOptions options;
  options.maximum_migrated_threads_ratio = 1;
  options.proposal_confirmations = 1;
  IncrementalSolver solver(true);
  solver.propose(h, threads, {}, options, 1);
  auto pins = solver.propose(h, threads, {}, options, 2);
  std::map<int, double> loads;
  for (const auto &action : pins.actions)
    loads[action.target_cpu] += threads[action.identity.tid - 1].demand;
  for (const auto &[_, load] : loads)
    require(load <= 1.0 + 1e-12,
            "initial CPU mapping repairs sampled CPU demand collision");
}

void test_incremental_slot_cap_and_future_demand() {
  HardwareGraph h;
  std::vector<ThreadDemand> threads;
  for (int cpu = 0; cpu < 128; ++cpu)
    h.cpus.push_back({cpu, cpu / 32, true});
  for (int node = 0; node < 4; ++node)
    for (int other = 0; other < 4; ++other)
      h.node_distance[{node, other}] = node == other ? 10 : 20;
  for (int index = 0; index < 721; ++index)
    threads.push_back({{1, 5000 + index, static_cast<uint64_t>(index + 1)},
                       "future-pool", index == 0 ? 0.2 : 0.0, 1,
                       index % 128});
  IncrementalOptions options;
  options.maximum_migrated_threads_ratio = 0.25;
  options.proposal_confirmations = 1;
  IncrementalSolver solver(true);
  auto node_plan = solver.propose(h, threads, {}, options, 1);
  require(node_plan.cpu_slot_cap == 6 &&
              node_plan.predicted_demand_threads == 720,
          "incremental planning reserves future pool demand");
  uint64_t now = 2;
  auto arrivals = solver.propose(h, threads, {}, options, now++);
  if (arrivals.ready) solver.commit(arrivals, now++);
  while (!solver.effective()) {
    auto proposal = solver.propose(h, threads, {}, options, now++);
    require(proposal.actions.size() <= 180,
            "initial slot repair respects migration budget");
    solver.commit(proposal, now++);
  }
  std::map<int, int> counts;
  for (const auto &[_, cpu] : solver.placement()) ++counts[cpu];
  int maximum = 0;
  for (const auto &[_, count] : counts) maximum = std::max(maximum, count);
  require(maximum <= 6,
          "incremental initial placement enforces dynamic CPU slot cap");

  for (int index = 0; index < 17; ++index)
    threads.push_back({{1, 6000 + index, static_cast<uint64_t>(800 + index)},
                       "future-pool", 0, 1, index});
  arrivals = solver.propose(h, threads, {}, options, now++);
  if (arrivals.ready) solver.commit(arrivals, now++);
  while (!solver.effective()) {
    auto proposal = solver.propose(h, threads, {}, options, now++);
    solver.commit(proposal, now++);
  }
  counts.clear();
  maximum = 0;
  for (const auto &[_, cpu] : solver.placement()) ++counts[cpu];
  for (const auto &[_, count] : counts) maximum = std::max(maximum, count);
  require(maximum <= 6,
          "same-window arrivals reserve pending CPU targets within slot cap");

  for (auto &thread : threads) thread.demand = 0;
  auto retained = solver.propose(h, threads, {}, options, now++);
  require(retained.predicted_demand_threads == threads.size(),
          "group demand high-water survives a temporarily idle window");
}

void test_incremental_repairs_node_headroom_imbalance() {
  auto h = hardware();
  std::vector<ThreadDemand> threads;
  for (int index = 0; index < 8; ++index)
    threads.push_back({{1, 7000 + index, static_cast<uint64_t>(index + 1)},
                       "workers", 0.02, 1, index % 4});
  IncrementalOptions options;
  options.maximum_migrated_threads_ratio = 0.5;
  options.proposal_confirmations = 1;
  options.thread_slot_slack = 1;
  IncrementalSolver solver(true);
  uint64_t now = 1;
  solver.propose(h, threads, {}, options, now++);
  while (!solver.effective()) {
    auto proposal = solver.propose(h, threads, {}, options, now++);
    if (proposal.ready) solver.commit(proposal, now++);
  }
  for (auto &thread : threads) {
    int cpu = solver.placement().at(thread.identity.tid);
    thread.demand = cpu < 2 ? 1.0 : 0.01;
  }
  auto repair = solver.propose(h, threads, {}, options, now++);
  require(std::any_of(repair.actions.begin(), repair.actions.end(),
                      [](const PlacementAction &action) {
                        return action.from_cpu < 2 && action.target_cpu >= 2;
                      }),
          "node utilization imbalance triggers cross-node headroom repair");
}

void test_inherited_restore_mask() {
  FakeBackend backend;
  Actuator actuator(backend);
  actuator.note_application_mask(1, {0, 1});
  actuator.note_inherited_mask(2, 1, {3});
  Placement placement;
  placement.tid_to_cpu = {{2, 2}};
  require(actuator.apply(placement, {2}).success, "child affinity applied");
  require(actuator.restore_all().success(), "inherited mask restore succeeds");
  require(backend.masks[2] == std::vector<int>({0, 1}),
          "child restores parent application mask instead of policy pin");

  actuator.note_application_mask(2, {2, 3});
  actuator.note_inherited_mask(2, 1, {0});
  require(actuator.apply(placement, {2}).success, "application-mask action applied");
  require(actuator.restore_all().success(), "application-mask restore succeeds");
  require(backend.masks[2] == std::vector<int>({2, 3}),
          "observed inheritance cannot overwrite an application declaration");
}

void test_unmodified_thread_is_not_restored() {
  FakeBackend backend;
  Actuator actuator(backend);
  actuator.note_inherited_mask(2, 1, {0, 1});
  auto restored = actuator.restore_all();
  require(restored.success() && restored.requested == 0,
          "observed-only threads do not trigger affinity writes");
}

void test_selective_restore_releases_cooled_thread() {
  FakeBackend backend;
  Actuator actuator(backend);
  actuator.note_application_mask(1, {0, 1});
  actuator.note_application_mask(2, {0, 1});
  Placement placement;
  placement.tid_to_cpu = {{1, 0}, {2, 1}};
  require(actuator.apply(placement, {1, 2}).success,
          "active cohort affinity applied");
  auto cooled = actuator.restore({1});
  require(cooled.success() && cooled.restored == 1 &&
              backend.masks[1] == std::vector<int>({0, 1}) &&
              backend.masks[2] == std::vector<int>({1}),
          "cooled thread restores broad application mask independently");
  auto remaining = actuator.restore_all();
  require(remaining.success() && remaining.requested == 1 &&
              backend.masks[2] == std::vector<int>({0, 1}),
          "selective restore removes thread from later restore-all set");
}

void test_128_cpu_scale() {
  HardwareGraph h;
  std::vector<ThreadDemand> threads;
  std::vector<RelationEdge> edges;
  for (int cpu = 0; cpu < 128; ++cpu) {
    h.cpus.push_back({cpu, cpu / 32, true});
    threads.push_back({{1, 1000 + cpu, 1}, "pool", 0.8, 1, cpu});
    if (cpu) edges.push_back({999 + cpu, 1000 + cpu, 1, 1, 0, 1, 10});
  }
  for (int a = 0; a < 4; ++a) for (int b = 0; b < 4; ++b) h.node_distance[{a, b}] = a == b ? 10 : 20;
  auto start = std::chrono::steady_clock::now();
  auto placement = Solver().solve(h, threads, edges, {0.25, 0.05});
  auto elapsed = std::chrono::steady_clock::now() - start;
  require(placement.tid_to_cpu.size() == 128, "128-thread placement");
  require(elapsed < std::chrono::seconds(1), "128-thread solve below one second");
}

void test_low_demand_slot_cap() {
  HardwareGraph h;
  std::vector<ThreadDemand> threads;
  for (int cpu = 0; cpu < 128; ++cpu) h.cpus.push_back({cpu, cpu / 32, true});
  for (int node = 0; node < 4; ++node)
    for (int other = 0; other < 4; ++other)
      h.node_distance[{node, other}] = node == other ? 10 : 20;
  for (int index = 0; index < 721; ++index)
    threads.push_back({{1, 2000 + index, static_cast<uint64_t>(index + 1)},
                       "idle-pool", 0, 1, index % 128});
  SolveOptions options;
  options.strategy_id = "dynamic-slot-cap";
  options.max_threads_per_cpu = 0;
  options.maximum_migrated_active_threads_ratio = 0.25;
  auto started = std::chrono::steady_clock::now();
  auto placement = Solver().solve(h, threads, {}, options);
  auto elapsed = std::chrono::steady_clock::now() - started;
  std::map<int, int> counts;
  for (const auto &[tid, cpu] : placement.tid_to_cpu) ++counts[cpu];
  int maximum = 0;
  for (const auto &[cpu, count] : counts) maximum = std::max(maximum, count);
  require(placement.tid_to_cpu.size() == 721, "all low-demand TIDs placed");
  require(maximum <= 8, "dynamic 721/128 slot cap");
  require(elapsed < std::chrono::seconds(1), "721-thread solve below one second");
  auto repeat = Solver().solve(h, threads, {}, options);
  require(repeat.tid_to_cpu == placement.tid_to_cpu,
          "low-demand slot placement deterministic");
}

void test_same_cpu_contention_is_separate_from_latency() {
  HardwareGraph h;
  h.cpus = {{0, 0, true}, {1, 0, true}};
  h.node_distance[std::pair{0, 0}] = 10;
  std::vector<ThreadDemand> threads{
      {{1, 1, 1}, "pool", 0, 1, 0},
      {{1, 2, 2}, "pool", 0, 1, 1},
  };
  std::vector<RelationEdge> edges{{1, 2, 1, 1, 0, 1, 100}};
  auto legacy = Solver().solve(h, threads, edges, {});
  require(legacy.tid_to_cpu[1] == legacy.tid_to_cpu[2] &&
              legacy.same_cpu_edge_weight == 100,
          "legacy zero latency rewards same-CPU edge");
  SolveOptions options;
  options.strategy_id = "same-cpu-contention";
  options.same_cpu_contention_penalty = 20;
  auto separated = Solver().solve(h, threads, edges, options);
  require(separated.tid_to_cpu[1] != separated.tid_to_cpu[2] &&
              separated.same_cpu_edge_weight == 0,
          "same-CPU contention is evaluated independently");
}
} // namespace

int main() {
  static_assert(AFFINITYGRAPH_FUTEX_AGGREGATE_MAX_ENTRIES == 262144);
  static_assert(AFFINITYGRAPH_VFS_AGGREGATE_MAX_ENTRIES == 262144);
  try {
    test_cpu_lists();
    test_thread_profile();
    test_identity_reuse_and_demand();
    test_relationship_score();
    test_relation_resident_set();
    test_solver();
    test_active_cohort_confirmation_signature();
    test_hardware_calibration();
    test_transactional_rollback();
    test_numa_domain_family_selection_and_merge();
    test_numa_domain_clickhouse_negative_control();
    test_numa_domain_pending_merge_suppresses_singleton();
    test_numa_domain_dwell_holds_transient_relation_gap();
    test_numa_domain_recent_evidence_refreshes_release_dwell();
    test_numa_domain_capacity_dwell_uses_node_change_time();
    test_numa_domain_dwell_allows_superset_merge();
    test_numa_domain_aggregates_before_family_pruning();
    test_numa_domain_capacity_atomicity_and_envelope();
    test_incremental_initial_node_plan();
    test_sparse_hotspot_communities_cluster_by_node();
    test_managed_hotspot_selection_is_bounded_and_stable();
    test_managed_selection_keeps_strong_community_intact();
    test_initial_confirmation_freezes_candidate_for_stable_population();
    test_incremental_arrival_does_not_cancel_global_replan();
    test_incremental_identity_and_reset();
    test_graph_delta_thresholds();
    test_incremental_per_thread_cooldown();
    test_incremental_721_thread_initial_scale();
    test_initial_cpu_capacity_repair();
    test_incremental_slot_cap_and_future_demand();
    test_incremental_repairs_node_headroom_imbalance();
    test_inherited_restore_mask();
    test_unmodified_thread_is_not_restored();
    test_selective_restore_releases_cooled_thread();
    test_128_cpu_scale();
    test_low_demand_slot_cap();
    test_same_cpu_contention_is_separate_from_latency();
    std::cout << "all core tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
