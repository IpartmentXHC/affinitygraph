#pragma once

#include "affinitygraph/core.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace affinitygraph {

class BpfRingReader;

struct LifecycleRecord {
  int parent_tid = -1;
  int tid = -1;
  uintptr_t start_routine = 0;
  uint64_t created_ns = 0;
  std::string start_symbol;
  std::string name;
  std::vector<int> inherited_mask;
};

class Runtime {
public:
  explicit Runtime(Config config);
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
  void run();
  void reconcile_and_sample();
  void maybe_solve(uint64_t now);
  void open_control_socket();
  void service_control_socket();
  void log(const std::string &type, const std::string &fields = "");

  Config config_;
  HardwareGraph hardware_;
  ProcCollector collector_;
  GraphWindow graph_;
  std::shared_ptr<BpfRingReader> bpf_reader_;
  LinuxAffinityBackend backend_;
  Actuator actuator_;
  mutable std::mutex mutex_;
  std::unordered_map<int, LifecycleRecord> lifecycle_;
  std::map<int, int> current_plan_;
  std::map<std::string, std::vector<int>> group_plan_;
  std::map<std::string, size_t> group_cursor_;
  std::string proposal_signature_;
  int proposal_count_ = 0;
  uint64_t last_solve_ns_ = 0;
  uint64_t last_action_ns_ = 0;
  uint64_t collector_failed_since_ns_ = 0;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> paused_{false};
  std::atomic<int> runtime_tid_{-1};
  std::thread worker_;
  int control_fd_ = -1;
  int log_fd_ = -1;
};

Runtime *runtime_instance();
void runtime_initialize_from_environment();
void runtime_shutdown();
bool internal_call();
void set_internal_call(bool value);

} // namespace affinitygraph
