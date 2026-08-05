#include "affinitygraph/core.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
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
}

void test_identity_reuse_and_demand() {
  GraphWindow graph(60);
  graph.observe_threads({sample(7, 1000000000, 0, 0)});
  graph.observe_threads({sample(7, 2000000000, 500000000, 250000000)});
  auto demand = graph.demands();
  require(demand.size() == 1 && demand[0].demand > 0.74 && demand[0].demand < 0.76, "run+runnable demand");
  graph.observe_relation({7, 8, 2000000000, 1, 0, 1});
  graph.observe_threads({sample(7, 3000000000, 1, 1, 2)});
  require(graph.demands()[0].demand == 0, "TID generation reset");
  require(graph.edges().empty(), "TID reuse purges stale relationships");
  require(GraphWindow::normalize_group("worker_17") == GraphWindow::normalize_group("worker_29"), "normalized group");
}

void test_relationship_score() {
  GraphWindow graph(2, {1, 1, 1});
  graph.observe_threads({sample(1, 1000000000, 0, 0), sample(2, 1000000000, 0, 0)});
  graph.observe_threads({sample(1, 2000000000, 800000000, 0), sample(2, 2000000000, 800000000, 0)});
  graph.observe_relation({1, 2, 1000000000, 2, 0.5, 1});
  graph.observe_relation({2, 1, 2000000000, 2, 0.5, 1});
  auto edges = graph.edges();
  require(edges.size() == 1, "relation aggregation");
  require(edges[0].score > 0 && edges[0].sync > edges[0].share, "relationship components");
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
}

void test_transactional_rollback() {
  FakeBackend backend;
  Actuator actuator(backend);
  Placement p;
  p.tid_to_cpu = {{1, 2}, {2, 3}};
  backend.fail_tid = 2;
  auto result = actuator.apply(p, {1, 2});
  require(!result.success && result.applied == 1, "partial failure reported");
  require(backend.masks[1] == std::vector<int>({0, 1}), "completed action rolled back");
  backend.fail_tid = -1;
  require(actuator.apply(p, {1, 2}).success, "successful transaction");
  require(actuator.restore_all(), "restore succeeds");
  require(backend.masks[1] == std::vector<int>({0, 1}), "original mask restored");
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
} // namespace

int main() {
  try {
    test_cpu_lists();
    test_identity_reuse_and_demand();
    test_relationship_score();
    test_solver();
    test_hardware_calibration();
    test_transactional_rollback();
    test_128_cpu_scale();
    std::cout << "all core tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
