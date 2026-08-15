#pragma once

#include "affinitygraph/core.hpp"
#include "affinitygraph/thread_profile.hpp"
#include "affinitygraph/bpf_events.h"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace affinitygraph {

class BpfRingReader;

struct BpfHealthSnapshot {
  bool valid = false;
  int error = 0;
  affinitygraph_bpf_health counters{};
};

struct BpfReaderStats {
  uint64_t capacity_bytes = 0;
  uint64_t occupancy_bytes = 0;
  uint64_t max_occupancy_bytes = 0;
  uint64_t drain_calls = 0;
  uint64_t events_consumed = 0;
  uint64_t last_batch_events = 0;
  uint64_t max_batch_events = 0;
  uint64_t last_drain_ns = 0;
  uint64_t max_drain_ns = 0;
  uint64_t last_max_lag_ns = 0;
  uint64_t max_lag_ns = 0;
  uint64_t futex_aggregate_records = 0;
  uint64_t futex_handoffs = 0;
  uint64_t vfs_aggregate_records = 0;
  uint64_t vfs_handoffs = 0;
};

struct LifecycleRecord {
  int tgid = -1;
  int parent_tgid = -1;
  int parent_tid = -1;
  int tid = -1;
  uintptr_t start_routine = 0;
  uint64_t bpf_start_time_ns = 0;
  uint64_t proc_starttime = 0;
  uint64_t created_ns = 0;
  std::string start_symbol;
  std::string name;
  std::vector<int> inherited_mask;
};

class Runtime {
public:
  Runtime(Config config, int root_pid, std::shared_ptr<BpfRingReader> bpf_reader,
          bool collector_degraded, std::string uprobe_status,
          bool affinity_capability = false);
  ~Runtime();
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;

  void thread_started(const LifecycleRecord &record);
  void thread_renamed(int tid, const std::string &name);
  void thread_exited(int tid, const char *reason);
  void application_affinity(int tid, const std::vector<int> &cpus);
  std::string status_json() const;
  void pause();
  void resume();

private:
  struct PendingRelationKey {
    ag_u32 kind = 0;
    ag_u32 tgid = 0;
    ag_u32 tid = 0;
    ag_u32 peer_tid = 0;
    ag_u64 start_time_ns = 0;
    ag_u64 peer_start_time_ns = 0;

    bool operator==(const PendingRelationKey &) const = default;
  };

  struct PendingRelationHash {
    size_t operator()(const PendingRelationKey &key) const noexcept;
  };

  struct PendingRelationValue {
    ag_u64 timestamp_ns = 0;
    ag_u64 value_ns = 0;
  };

  struct PendingHealthSample {
    uint64_t timestamp_ns = 0;
    BpfHealthSnapshot health;
    BpfReaderStats reader_stats;
  };

  void run();
  void run_bpf();
  void reconcile_and_sample();
  void maybe_solve(uint64_t now);
  void solve_numa_domains(
      const std::string &window_id, const std::vector<ThreadDemand> &demands,
      const std::vector<RelationEdge> &edges,
      const std::map<int, std::vector<int>> &allowed_masks, uint64_t now);
  void open_control_socket();
  void service_control_socket();
  void drain_bpf_once();
  void consume_pending_bpf();
  void handle_bpf_event(const affinitygraph_bpf_event &event);
  void sample_bpf_health(uint64_t now);
  void trigger_fatal(const std::string &message);
  void log(const std::string &type, const std::string &fields = "");

  Config config_;
  int root_pid_ = -1;
  int supervisor_pid_ = -1;
  bool collector_degraded_ = false;
  bool affinity_capability_ = false;
  std::string uprobe_status_;
  HardwareGraph hardware_;
  ProcCollector collector_;
  GraphWindow graph_;
  std::shared_ptr<BpfRingReader> bpf_reader_;
  LinuxAffinityBackend backend_;
  Actuator actuator_;
  IncrementalSolver solver_;
  NumaDomainSolver domain_solver_;
  std::optional<ThreadProfile> thread_profile_;
  std::map<std::string, size_t> profile_next_instances_;
  std::map<ThreadIdentity, ProfileAssignment> profile_assignments_;
  mutable std::mutex mutex_;
  std::mutex bpf_pending_mutex_;
  std::deque<affinitygraph_bpf_event> pending_lifecycle_events_;
  std::unordered_map<PendingRelationKey, PendingRelationValue,
                     PendingRelationHash> pending_relations_;
  std::deque<PendingHealthSample> pending_health_samples_;
  uint64_t pending_lifecycle_overflow_ = 0;
  uint64_t pending_relation_overflow_ = 0;
  uint64_t pending_relation_started_ns_ = 0;
  uint64_t last_bpf_health_sample_ns_ = 0;
  std::unordered_map<int, LifecycleRecord> lifecycle_;
  std::map<int, std::vector<int>> pending_application_masks_;
  std::set<int> target_tgids_;
  std::map<int, int> current_plan_;
  std::map<int, std::vector<int>> current_masks_;
  std::set<int> active_cohort_;
  bool selector_ready_ = false;
  uint64_t last_solve_ns_ = 0;
  uint64_t runtime_instance_id_ = 0;
  uint64_t solve_window_sequence_ = 0;
  uint64_t collector_failed_since_ns_ = 0;
  BpfHealthSnapshot bpf_health_;
  BpfReaderStats bpf_reader_stats_;
  std::deque<std::pair<uint64_t, affinitygraph_bpf_health>> bpf_health_history_;
  double bpf_window_loss_ratio_ = 0.0;
  bool bpf_window_ready_ = false;
  int bpf_health_consecutive_failures_ = 0;
  uint64_t bpf_health_failed_since_ns_ = 0;
  std::string fatal_error_;
  size_t action_requested_ = 0;
  size_t action_committed_ = 0;
  size_t action_vanished_ = 0;
  size_t action_rolled_back_ = 0;
  size_t action_failures_ = 0;
  RestoreResult last_restore_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> paused_{false};
  std::thread bpf_worker_;
  std::thread worker_;
  int control_fd_ = -1;
  int log_fd_ = -1;
};

std::shared_ptr<BpfRingReader> make_bpf_reader(int events_fd, int health_fd,
                                               int futex_aggregates_fd,
                                               int vfs_aggregates_fd);
std::vector<affinitygraph_bpf_event> drain_bpf_events(BpfRingReader &reader);
std::vector<affinitygraph_bpf_event> drain_bpf_futex_aggregates(BpfRingReader &reader);
std::vector<affinitygraph_bpf_event> drain_bpf_vfs_aggregates(BpfRingReader &reader);
BpfHealthSnapshot bpf_reader_health(BpfRingReader &reader);
BpfReaderStats bpf_reader_stats(BpfRingReader &reader);

} // namespace affinitygraph
