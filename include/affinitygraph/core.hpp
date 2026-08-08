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
  int initial_proposal_confirmations = 1;
  int minimum_dwell_seconds = 60;
  double maximum_migrated_threads_ratio = 0.25;
  double initial_migrated_threads_ratio = 1.0;
  int collector_failure_restore_seconds = 30;
  std::string solver = "incremental-hotspot-v1";
  std::string affinity_granularity = "singleton_cpu";
  double family_minimum_demand = 1.0;
  double family_minimum_internal_relation = 1.0;
  double family_minimum_self_containment = 0.20;
  double family_minimum_relative_internal = 0.10;
  double domain_merge_ratio = 0.25;
  int family_edges_per_family = 4;
  int family_stability_confirmations = 3;
  int domain_stability_confirmations = 3;
  int domain_plan_confirmations = 3;
  int maximum_threads_per_domain = 1024;
  double domain_capacity_ratio = 0.80;
  double domain_expand_ratio = 0.90;
  int domain_expand_confirmations = 3;
  double domain_shrink_ratio = 0.55;
  int domain_shrink_confirmations = 6;
  int domain_minimum_dwell_seconds = 300;
  int initial_node_passes = 2;
  double initial_node_thread_slack_ratio = 0.5;
  int candidate_multiplier = 4;
  int candidate_hard_limit = 64;
  int rotating_scan_size = 32;
  double demand_dirty_threshold = 0.05;
  double edge_dirty_absolute_threshold = 1.0;
  double edge_dirty_relative_threshold = 0.10;
  double minimum_relative_gain = 0.05;
  int maximum_threads_per_cpu = 0;
  int thread_slot_slack = 0;
  double future_demand_floor = 0.02;
  double group_peak_demand_ratio = 0.25;
  double group_peak_demand_cap = 0.05;
  double group_peak_decay = 0.95;
  double node_balance_threshold = 0.05;
  int hotspot_edges_per_thread = 4;
  double hotspot_edge_quantile = 0.95;
  double hotspot_component_boost = 4.0;
  int maximum_managed_threads = 32;
  double managed_thread_hysteresis_ratio = 1.0;
  double hotspot_replan_growth_ratio = 0.25;
  int hotspot_replan_min_threads = 8;
  double hotspot_stability_threshold = 0.65;
  int hotspot_stability_confirmations = 3;
  double active_demand_threshold = 0.05;
  double inactive_demand_threshold = 0.0;
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
  bool operator==(const Cpu &) const = default;
};

struct NodeCalibration {
  double handoff_mean_ns = 0;
  double handoff_p95_ns = 0;
  double memory_load_mean_ns = 0;
  double memory_load_cv = 0;
  double stream_2t_triad_mbps = 0;
  double stream_32t_triad_mbps = 0;
};

struct HardwareGraph {
  std::vector<Cpu> cpus;
  std::map<std::pair<int, int>, double> cpu_latency;
  std::map<std::pair<int, int>, double> node_distance;
  std::map<std::pair<int, int>, NodeCalibration> node_calibration;

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
  bool operator<(const ThreadIdentity &other) const {
    return std::tie(tgid, tid, starttime) <
           std::tie(other.tgid, other.tid, other.starttime);
  }
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

struct ThreadWindowRecord {
  ThreadIdentity identity;
  std::string comm;
  int parent_tid = -1;
  uintptr_t start_routine = 0;
  std::string start_symbol;
  std::string group;
  double demand = 0;
  double confidence = 0;
  int current_cpu = -1;
  std::vector<int> allowed_cpus;
  char state = '?';
  size_t sample_count = 0;
  uint64_t runtime_delta_ns = 0;
  uint64_t runqueue_delta_ns = 0;
  uint64_t voluntary_switches_delta = 0;
  uint64_t involuntary_switches_delta = 0;
};

struct RelationEdge {
  int from_tid = -1;
  int to_tid = -1;
  double activity = 0;
  double sync = 0;
  double share = 0;
  double stability = 0;
  double score = 0;
  double handoff_rate = 0;
  double shared_vfs_seconds = 0;
  double active_overlap = 0;
  size_t observation_count = 0;
  double coverage = 0;
  double coefficient_of_variation = 0;
};

struct GraphDelta {
  std::vector<ThreadDemand> upsert_threads;
  std::vector<ThreadIdentity> removed_threads;
  std::vector<RelationEdge> upsert_edges;
  std::vector<std::pair<int, int>> removed_edges;

  std::set<int> dirty_tids() const {
    std::set<int> result;
    for (const auto &thread : upsert_threads) result.insert(thread.identity.tid);
    for (const auto &thread : removed_threads) result.insert(thread.tid);
    for (const auto &edge : upsert_edges) {
      result.insert(edge.from_tid);
      result.insert(edge.to_tid);
    }
    for (const auto &edge : removed_edges) {
      result.insert(edge.first);
      result.insert(edge.second);
    }
    return result;
  }
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
  void retain_relations(const std::set<std::pair<int, int>> &relations);
  std::vector<ThreadDemand> demands() const;
  std::vector<ThreadWindowRecord> thread_records() const;
  std::vector<RelationEdge> edges() const;
  GraphDelta take_delta(double demand_threshold,
                        double edge_absolute_threshold,
                        double edge_relative_threshold);
  static std::string normalize_group(const std::string &comm,
                                     uintptr_t start_routine = 0,
                                     int parent_tid = -1,
                                     const std::string &start_symbol = {});

private:
  struct RelationAggregate {
    size_t count = 0;
    double forward_sync = 0;
    double reverse_sync = 0;
    double handoff = 0;
    double share = 0;
    double overlap = 0;
    double total = 0;
    double total_squared = 0;
  };
  int horizon_seconds_;
  Calibration calibration_;
  std::unordered_map<int, std::deque<ThreadSample>> threads_;
  std::map<std::pair<int, int>, std::deque<RelationObservation>> relation_histories_;
  std::map<std::pair<int, int>, RelationAggregate> relation_aggregates_;
  mutable bool demand_cache_valid_ = false;
  mutable bool edge_cache_valid_ = false;
  mutable std::vector<ThreadDemand> demand_cache_;
  mutable std::vector<RelationEdge> edge_cache_;
  std::map<int, ThreadDemand> emitted_demands_;
  std::map<std::pair<int, int>, RelationEdge> emitted_edges_;
};

struct Placement {
  std::map<int, int> tid_to_cpu;
  double overload = 0;
  double relation_cost = 0;
  double migration_cost = 0;
  double same_cpu_edge_weight = 0;
};

struct PlacementDelta {
  std::map<int, int> tid_to_cpu;
  std::map<int, std::vector<int>> tid_to_mask;
};

struct FamilyMetric {
  std::string name;
  size_t thread_count = 0;
  double demand = 0;
  double internal_relation = 0;
  double external_relation = 0;
  double self_containment = 0;
  double relative_internal = 0;
  // 以下 gate 字段只用于解释 selector 决策，不参与额外打分。把每个门槛
  // 单独暴露后，可以从日志直接判断 family 是卡在 demand、绝对关系，还是
  // S_g/P_g，而不必离线重新计算浮点比较。
  bool demand_eligible = false;
  bool internal_relation_eligible = false;
  bool self_containment_eligible = false;
  bool relative_internal_eligible = false;
  bool cohesive_eligible = false;
  int confirmation = 0;
  int seed_confirmation = 0;
  bool cohesive_anchor = false;
  // 已出现满足阈值、但尚未完成稳定确认的跨 family 关系。此时暂缓把
  // cohesive anchor 作为 singleton domain 输出，避免先绑定单组、随后释放
  // 并改绑联合 domain 的初始化抖动。
  bool cross_pending = false;
  bool cross_seed = false;
  bool anchor = false;
};

// family 聚合完成后的一条跨组候选边。这里只记录被 deterministic top-K
// 选中的 pair，因此日志规模受 family_edges_per_family 限制。merge_ratio
// 对应 X_gh / min(I_g, I_h)。绝对组内证据不足时 denominator 仍会记录，
// 但 endpoints_eligible/qualifies 为 false。
struct FamilyPairMetric {
  std::string left;
  std::string right;
  double cross_relation = 0;
  double denominator = 0;
  double merge_ratio = 0;
  bool endpoints_eligible = false;
  bool qualifies = false;
  int confirmation = 0;
  bool confirmed = false;
};

struct NumaDomain {
  std::string id;
  std::vector<std::string> families;
  std::vector<int> tids;
  std::vector<int> target_nodes;
  std::vector<int> target_mask;
  double demand = 0;
  // node planner 的可解释输出。capacity_limit = online_cpu_count *
  // capacity_ratio，capacity_headroom = capacity_limit - demand。
  std::vector<int> previous_nodes;
  size_t online_cpu_count = 0;
  double capacity_limit = 0;
  double capacity_headroom = 0;
  double relation_latency = 0;
  double background_demand = 0;
  int initial_migrations = 0;
  int expand_confirmation = 0;
  int shrink_confirmation = 0;
  std::string node_decision;
  int confirmation = 0;
  bool valid = true;
  std::string invalid_reason;
};

struct NumaDomainOptions {
  double family_minimum_demand = 1.0;
  double family_minimum_internal_relation = 1.0;
  double family_minimum_self_containment = 0.20;
  double family_minimum_relative_internal = 0.10;
  double domain_merge_ratio = 0.25;
  size_t family_edges_per_family = 4;
  int family_stability_confirmations = 3;
  int domain_stability_confirmations = 3;
  int plan_confirmations = 3;
  size_t maximum_threads_per_domain = 1024;
  double capacity_ratio = 0.80;
  double expand_ratio = 0.90;
  int expand_confirmations = 3;
  double shrink_ratio = 0.55;
  int shrink_confirmations = 6;
  uint64_t minimum_dwell_ns = 300000000000ULL;
};

struct NumaDomainAction {
  ThreadIdentity identity;
  std::vector<int> from_mask;
  std::vector<int> target_mask;
  std::vector<int> target_nodes;
  bool forced_migration = false;
};

struct NumaDomainProposal {
  uint64_t id = 0;
  PlacementDelta delta;
  std::map<int, std::vector<int>> planned_masks;
  std::set<int> inherited_tids;
  std::vector<FamilyMetric> families;
  std::vector<FamilyPairMetric> family_pairs;
  std::vector<NumaDomain> domains;
  std::vector<NumaDomainAction> actions;
  std::set<int> released_tids;
  size_t input_family_count = 0;
  size_t input_cross_family_pair_count = 0;
  size_t selected_family_pair_count = 0;
  double total_demand = 0;
  double total_relation = 0;
  bool ready = false;
  bool valid = true;
  std::string invalid_reason;
};

class NumaDomainSolver {
public:
  NumaDomainProposal propose(
      const HardwareGraph &hardware,
      const std::vector<ThreadDemand> &threads,
      const std::vector<RelationEdge> &edges,
      const std::map<int, std::vector<int>> &allowed_masks,
      const NumaDomainOptions &options, uint64_t now_ns);
  void commit(const NumaDomainProposal &proposal, uint64_t now_ns,
              const std::set<int> &committed_tids = {});
  void discard(const NumaDomainProposal &proposal);
  void reset();
  void remove_thread(int tid);
  const std::map<int, std::vector<int>> &placement() const { return placement_; }
  const std::vector<NumaDomain> &domains() const { return domains_; }
  const std::vector<FamilyMetric> &families() const { return families_; }
  bool effective() const { return !placement_.empty(); }
  uint64_t generation() const { return generation_; }

private:
  uint64_t next_proposal_id_ = 1;
  uint64_t generation_ = 0;
  std::map<std::string, int> family_confirmations_;
  std::map<std::pair<std::string, std::string>, int> merge_confirmations_;
  std::map<std::pair<std::string, std::string>, int> merge_pending_grace_;
  int global_plan_confirmation_ = 0;
  std::map<std::string, int> expand_confirmations_;
  std::map<std::string, int> shrink_confirmations_;
  // 节点变更时间只约束容量扩缩；完整 domain 证据时间单独约束关系低谷时
  // 是否保留 family 集合，避免长期稳定 domain 在一次 phase gap 后被拆散。
  std::map<std::string, uint64_t> last_changed_ns_;
  std::map<std::string, uint64_t> last_confirmed_domain_evidence_ns_;
  std::map<std::string, std::vector<int>> domain_nodes_;
  std::map<int, std::vector<int>> placement_;
  std::vector<NumaDomain> domains_;
  std::vector<FamilyMetric> families_;
  std::string pending_signature_;
  std::optional<NumaDomainProposal> outstanding_;
};

struct SolveOptions {
  double maximum_migrated_active_threads_ratio = 0.25;
  double active_threshold = 0.05;
  double inactive_threshold = 0.0;
  double minimum_confidence = 0.8;
  std::string strategy_id = "legacy-v1";
  double demand_floor = 0;
  int max_threads_per_cpu = -1;
  int slot_slack = 2;
  double count_penalty = 0;
  double low_demand_spread = 0;
  double same_cpu_contention_penalty = 0;
  double group_spread_penalty = 0;
  bool thread_count_tie_break = false;
  int fm_passes = 4;
  int lpt_refinement_passes = 0;
  double initial_node_thread_slack_ratio = 0.5;
  int hotspot_edges_per_thread = 4;
  double hotspot_edge_quantile = 0.95;
  double hotspot_component_boost = 4.0;
  int maximum_managed_threads = 32;
  double managed_thread_hysteresis_ratio = 1.0;
  double hotspot_replan_growth_ratio = 0.25;
  int hotspot_replan_min_threads = 8;
  double hotspot_stability_threshold = 0.65;
  int hotspot_stability_confirmations = 3;
  uint64_t seed = 0;
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

enum class IncrementalPhase {
  Uninitialized,
  NodePlanning,
  InitialPinning,
  Optimizing,
};

const char *incremental_phase_name(IncrementalPhase phase);

struct IncrementalOptions {
  double maximum_migrated_threads_ratio = 0.25;
  double initial_migrated_threads_ratio = 1.0;
  int proposal_confirmations = 3;
  int initial_proposal_confirmations = 1;
  uint64_t minimum_dwell_ns = 60000000000ULL;
  int initial_node_passes = 2;
  double initial_node_thread_slack_ratio = 0.5;
  int candidate_multiplier = 4;
  int candidate_hard_limit = 64;
  int rotating_scan_size = 32;
  double demand_dirty_threshold = 0.05;
  double edge_dirty_absolute_threshold = 1.0;
  double edge_dirty_relative_threshold = 0.10;
  double minimum_relative_gain = 0.05;
  int maximum_threads_per_cpu = 0;
  int thread_slot_slack = 0;
  double future_demand_floor = 0.02;
  double group_peak_demand_ratio = 0.25;
  double group_peak_demand_cap = 0.05;
  double group_peak_decay = 0.95;
  double node_balance_threshold = 0.05;
  int hotspot_edges_per_thread = 4;
  double hotspot_edge_quantile = 0.95;
  double hotspot_component_boost = 4.0;
  int maximum_managed_threads = 32;
  double managed_thread_hysteresis_ratio = 1.0;
  double hotspot_replan_growth_ratio = 0.25;
  int hotspot_replan_min_threads = 8;
  double hotspot_stability_threshold = 0.65;
  int hotspot_stability_confirmations = 3;
};

std::vector<RelationEdge> select_hotspot_edges(
    const std::vector<RelationEdge> &edges,
    const std::map<int, ThreadDemand> &threads,
    const IncrementalOptions &options);

std::vector<ThreadDemand> select_managed_threads(
    const std::vector<ThreadDemand> &threads,
    const std::vector<RelationEdge> &edges,
    const std::set<int> &previous_managed,
    const IncrementalOptions &options);

struct PlacementAction {
  ThreadIdentity identity;
  int from_cpu = -1;
  int target_cpu = -1;
  bool initial_pin = false;
  bool emergency = false;
  double capacity_regret = 0;
  double relation_regret = 0;
  double load_regret = 0;
};

struct SolverProposal {
  uint64_t id = 0;
  IncrementalPhase phase = IncrementalPhase::Uninitialized;
  PlacementDelta delta;
  std::vector<PlacementAction> actions;
  size_t eligible_threads = 0;
  size_t pinned_threads = 0;
  size_t dirty_threads = 0;
  size_t candidate_threads = 0;
  size_t cooldown_skipped_threads = 0;
  size_t migration_budget = 0;
  int confirmation = 0;
  bool initial_plan_confirmed = false;
  bool ready = false;
  bool shadow = false;
  double node_overload = 0;
  double relation_cost = 0;
  int cpu_slot_cap = 0;
  int maximum_cpu_threads = 0;
  size_t predicted_demand_threads = 0;
  size_t relation_edges_input = 0;
  size_t hotspot_edges = 0;
  double hotspot_similarity = 0;
  int hotspot_stability_confirmation = 0;
  bool hotspot_replan_triggered = false;
  bool global_replan_active = false;
};

class IncrementalSolver {
public:
  explicit IncrementalSolver(bool shadow = false) : shadow_(shadow) {}
  SolverProposal propose(const HardwareGraph &hardware,
                         const std::vector<ThreadDemand> &threads,
                         const std::vector<RelationEdge> &edges,
                         const IncrementalOptions &options,
                         uint64_t now_ns,
                         const GraphDelta *delta = nullptr);
  void commit(const SolverProposal &proposal, uint64_t now_ns,
              const std::set<int> &committed_tids = {});
  void discard(const SolverProposal &proposal);
  void reset();
  void remove_thread(int tid);
  IncrementalPhase phase() const { return phase_; }
  bool effective() const;
  uint64_t generation() const { return generation_; }
  const std::map<int, int> &placement() const { return placement_; }
  size_t pinned_threads() const { return pinned_.size(); }

private:
  bool shadow_ = false;
  IncrementalPhase phase_ = IncrementalPhase::Uninitialized;
  uint64_t generation_ = 0;
  uint64_t next_proposal_id_ = 1;
  size_t scan_cursor_ = 0;
  size_t last_node_plan_population_ = 0;
  bool replan_existing_threads_ = false;
  bool stable_hotspot_replan_pending_ = false;
  int hotspot_stability_confirmation_ = 0;
  std::set<std::pair<int, int>> previous_hotspot_keys_;
  int initial_confirmation_ = 0;
  std::map<int, ThreadIdentity> identities_;
  std::map<int, ThreadDemand> threads_;
  std::map<std::pair<int, int>, RelationEdge> edges_;
  std::map<int, double> previous_demand_;
  std::map<std::string, double> group_peak_demand_;
  std::map<std::pair<int, int>, double> previous_edge_score_;
  std::map<int, int> baseline_cpu_;
  std::map<int, int> initial_target_node_;
  std::map<int, int> initial_target_cpu_;
  std::map<int, int> previous_initial_target_node_;
  std::map<int, ThreadIdentity> initial_candidate_identities_;
  std::map<int, int> placement_;
  std::set<int> pinned_;
  std::map<int, uint64_t> last_moved_ns_;
  std::map<std::string, int> action_confirmations_;
  std::optional<SolverProposal> outstanding_;
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
  std::set<int> committed_tids;
  std::set<int> vanished_tids;
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
  ApplyResult apply(const PlacementDelta &placement,
                    const std::set<int> &live_tids);
  RestoreResult restore_all();
  RestoreResult restore(const std::set<int> &tids);
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
