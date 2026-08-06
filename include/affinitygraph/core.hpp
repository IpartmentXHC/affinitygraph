#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace affinitygraph {

enum class Mode { Observe, Plan, Active };

struct Config {
  Mode mode = Mode::Observe;
  std::vector<int> cpus;
  std::string calibration_path;
  std::string log_directory = "/tmp/affinitygraph";
  std::string socket_path = "/tmp/affinitygraph.sock";
  int sample_interval_seconds = 1;
  int graph_horizon_seconds = 60;
  int solve_interval_seconds = 10;
  double minimum_confidence = 0.8;
  int proposal_confirmations = 3;
  int minimum_dwell_seconds = 60;
  double maximum_migrated_active_threads_ratio = 0.25;
  int collector_failure_restore_seconds = 30;
  bool bpf_required = false;
  bool pthread_uprobe = true;
  std::string relationship_calibration_id = "clickhouse-gate2-fixed-v2";
  double activity_log_p95 = 2.4138804290562152;
  double sync_log_p95 = 2.5591179487485345;
  double share_log_p95 = 0.00894730347830295;
};

Config load_config(const std::string &path);
std::vector<int> parse_cpu_list(const std::string &text);
std::string format_cpu_list(const std::vector<int> &cpus);

struct Cpu {
  int id = -1;
  int node = -1;
  bool online = false;
};

struct HardwareGraph {
  std::vector<Cpu> cpus;
  std::map<std::pair<int, int>, double> cpu_latency;
  std::map<std::pair<int, int>, double> node_distance;

  static HardwareGraph discover(const std::vector<int> &envelope,
                                const std::string &sys_root = "/sys");
  void load_calibration(const std::string &path);
  double latency(int from_cpu, int to_cpu) const;
  std::vector<int> nodes() const;
  std::vector<int> cpus_in_node(int node) const;
};

struct ThreadIdentity {
  int tgid = -1;
  int tid = -1;
  uint64_t starttime = 0;
  bool operator==(const ThreadIdentity &) const = default;
};

struct ThreadSample {
  ThreadIdentity identity;
  uint64_t timestamp_ns = 0;
  uint64_t runtime_ns = 0;
  uint64_t runqueue_ns = 0;
  uint64_t voluntary_switches = 0;
  uint64_t involuntary_switches = 0;
  int recent_cpu = -1;
  char state = '?';
  std::string comm;
  std::vector<int> allowed_cpus;
  int parent_tid = -1;
  uintptr_t start_routine = 0;
  std::string start_symbol;
};

class ProcCollector {
public:
  explicit ProcCollector(std::string proc_root = "/proc");
  std::vector<ThreadSample> sample(int tgid) const;
  std::map<int, uint64_t> numa_pages(int tgid) const;

private:
  std::string proc_root_;
};

struct RelationObservation {
  int from_tid = -1;
  int to_tid = -1;
  uint64_t from_starttime = 0;
  uint64_t to_starttime = 0;
  uint64_t timestamp_ns = 0;
  double futex_per_second = 0;
  double shared_vfs_seconds = 0;
  double active_overlap = 0;
};

struct ThreadDemand {
  ThreadIdentity identity;
  std::string group;
  double demand = 0;
  double confidence = 0;
  int current_cpu = -1;
};

struct RelationEdge {
  int from_tid = -1;
  int to_tid = -1;
  double activity = 0;
  double sync = 0;
  double share = 0;
  double stability = 0;
  double score = 0;
};

struct Calibration {
  double activity_log_p95 = 1.9934573840570624;
  double sync_log_p95 = 4.407748051923179;
  double share_log_p95 = 0.013644871175407561;
};

class GraphWindow {
public:
  GraphWindow(int horizon_seconds, Calibration calibration = {});
  void observe_threads(const std::vector<ThreadSample> &samples);
  void observe_relation(const RelationObservation &observation);
  std::vector<ThreadDemand> demands() const;
  std::vector<RelationEdge> edges() const;
  static std::string normalize_group(const std::string &comm,
                                     uintptr_t start_routine = 0,
                                     int parent_tid = -1,
                                     const std::string &start_symbol = {});

private:
  int horizon_seconds_;
  Calibration calibration_;
  std::unordered_map<int, std::deque<ThreadSample>> threads_;
  std::deque<RelationObservation> relations_;
};

struct Placement {
  std::map<int, int> tid_to_cpu;
  double overload = 0;
  double relation_cost = 0;
  double migration_cost = 0;
};

struct SolveOptions {
  double maximum_migrated_active_threads_ratio = 0.25;
  double active_threshold = 0.05;
};

using ActiveCohort = std::set<std::tuple<int, int, uint64_t>>;
ActiveCohort active_cohort(const std::vector<ThreadDemand> &threads,
                           double active_threshold = 0.05);
bool active_cohort_continues(const ActiveCohort &previous,
                             const ActiveCohort &current,
                             double maximum_growth_ratio = 0.05);

class Solver {
public:
  Placement solve(const HardwareGraph &hardware,
                  const std::vector<ThreadDemand> &threads,
                  const std::vector<RelationEdge> &edges,
                  const SolveOptions &options) const;
};

class AffinityBackend {
public:
  virtual ~AffinityBackend() = default;
  virtual std::vector<int> get(int tid) = 0;
  virtual bool set(int tid, const std::vector<int> &cpus, int &error) = 0;
};

class LinuxAffinityBackend final : public AffinityBackend {
public:
  std::vector<int> get(int tid) override;
  bool set(int tid, const std::vector<int> &cpus, int &error) override;
};

struct ApplyResult {
  bool success = false;
  size_t requested = 0;
  size_t applied = 0;
  size_t committed = 0;
  size_t vanished = 0;
  size_t rolled_back = 0;
  bool rollback_success = true;
  int error = 0;
};

struct RestoreResult {
  size_t requested = 0;
  size_t restored = 0;
  size_t vanished = 0;
  size_t failed = 0;

  bool success() const { return failed == 0; }
};

class Actuator {
public:
  explicit Actuator(AffinityBackend &backend);
  ApplyResult apply(const Placement &placement,
                    const std::set<int> &live_tids);
  RestoreResult restore_all();
  void note_policy_action(int tid);
  void note_application_mask(int tid, const std::vector<int> &cpus);
  void note_inherited_mask(int tid, int parent_tid,
                           const std::vector<int> &observed_cpus);

private:
  AffinityBackend &backend_;
  std::map<int, std::vector<int>> restore_masks_;
  std::set<int> acted_tids_;
};

uint64_t monotonic_ns();

} // namespace affinitygraph
