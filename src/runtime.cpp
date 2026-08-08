#include "affinitygraph/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <signal.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unordered_set>
#include <utility>
#include <unistd.h>

namespace affinitygraph {
namespace {
std::string json_escape(const std::string &value) {
  std::ostringstream out;
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') out << '\\' << c;
    else if (c == '\n') out << "\\n";
    else if (c >= 0x20) out << c;
  }
  return out.str();
}

std::string cpu_array_json(const std::vector<int> &cpus);

std::string assignments_json(const std::map<int, int> &assignments) {
  std::ostringstream out;
  out << '{';
  bool first = true;
  for (const auto &[tid, cpu] : assignments) {
    if (!first) out << ',';
    first = false;
    out << '"' << tid << "\":" << cpu;
  }
  out << '}';
  return out.str();
}

std::string masks_json(const std::map<int, std::vector<int>> &masks) {
  std::ostringstream out;
  out << '{';
  bool first = true;
  for (const auto &[tid, mask] : masks) {
    if (!first) out << ',';
    first = false;
    out << '"' << tid << "\":\"" << format_cpu_list(mask) << '"';
  }
  out << '}';
  return out.str();
}

std::string domain_actions_json(const std::vector<NumaDomainAction> &actions) {
  std::ostringstream out;
  out << '[';
  for (size_t index = 0; index < actions.size(); ++index) {
    if (index) out << ',';
    const auto &action = actions[index];
    out << "{\"tgid\":" << action.identity.tgid
        << ",\"tid\":" << action.identity.tid
        << ",\"starttime\":" << action.identity.starttime
        << ",\"from_mask\":\"" << format_cpu_list(action.from_mask)
        << "\",\"target_mask\":\"" << format_cpu_list(action.target_mask)
        << "\",\"target_nodes\":" << cpu_array_json(action.target_nodes)
        << ",\"forced_migration\":"
        << (action.forced_migration ? "true" : "false") << '}';
  }
  out << ']';
  return out.str();
}

std::string family_metrics_json(const std::vector<FamilyMetric> &families) {
  std::ostringstream out;
  out << '[';
  for (size_t index = 0; index < families.size(); ++index) {
    if (index) out << ',';
    const auto &family = families[index];
    out << "{\"name\":\"" << json_escape(family.name)
        << "\",\"thread_count\":" << family.thread_count
        << ",\"demand\":" << family.demand
        << ",\"internal_relation\":" << family.internal_relation
        << ",\"external_relation\":" << family.external_relation
        << ",\"self_containment\":" << family.self_containment
        << ",\"relative_internal\":" << family.relative_internal
        << ",\"confirmation\":" << family.confirmation
        << ",\"seed_confirmation\":" << family.seed_confirmation
        << ",\"cohesive_anchor\":"
        << (family.cohesive_anchor ? "true" : "false")
        << ",\"cross_seed\":" << (family.cross_seed ? "true" : "false")
        << ",\"anchor\":" << (family.anchor ? "true" : "false") << '}';
  }
  out << ']';
  return out.str();
}

std::string domains_json(const std::vector<NumaDomain> &domains) {
  std::ostringstream out;
  out << '[';
  for (size_t index = 0; index < domains.size(); ++index) {
    if (index) out << ',';
    const auto &domain = domains[index];
    out << "{\"id\":\"" << json_escape(domain.id) << "\",\"families\":[";
    for (size_t family = 0; family < domain.families.size(); ++family) {
      if (family) out << ',';
      out << '"' << json_escape(domain.families[family]) << '"';
    }
    out << "],\"thread_count\":" << domain.tids.size()
        << ",\"target_nodes\":" << cpu_array_json(domain.target_nodes)
        << ",\"target_mask\":\"" << format_cpu_list(domain.target_mask)
        << "\",\"demand\":" << domain.demand
        << ",\"confirmation\":" << domain.confirmation
        << ",\"valid\":" << (domain.valid ? "true" : "false")
        << ",\"invalid_reason\":\"" << json_escape(domain.invalid_reason)
        << "\"}";
  }
  out << ']';
  return out.str();
}

std::string actions_json(const std::vector<PlacementAction> &actions) {
  std::ostringstream out;
  out << '[';
  for (size_t index = 0; index < actions.size(); ++index) {
    const auto &action = actions[index];
    if (index) out << ',';
    out << "{\"tgid\":" << action.identity.tgid
        << ",\"tid\":" << action.identity.tid
        << ",\"starttime\":" << action.identity.starttime
        << ",\"from_cpu\":" << action.from_cpu
        << ",\"target_cpu\":" << action.target_cpu
        << ",\"initial_pin\":" << (action.initial_pin ? "true" : "false")
        << ",\"emergency\":" << (action.emergency ? "true" : "false")
        << ",\"capacity_regret\":" << action.capacity_regret
        << ",\"relation_regret\":" << action.relation_regret
        << ",\"load_regret\":" << action.load_regret << '}';
  }
  out << ']';
  return out.str();
}

std::string cpu_array_json(const std::vector<int> &cpus) {
  std::ostringstream out;
  out << '[';
  for (size_t index = 0; index < cpus.size(); ++index) {
    if (index) out << ',';
    out << cpus[index];
  }
  out << ']';
  return out.str();
}

std::string bpf_kind_counts_json(const ag_u64 counts[AFFINITYGRAPH_EVENT_KIND_COUNT]) {
  static const char *names[AFFINITYGRAPH_EVENT_KIND_COUNT] = {
      "invalid", "task_fork", "task_exit", "futex", "vfs", "task_exec",
      "task_rename", "affinity"};
  std::ostringstream out;
  out << '{';
  for (int kind = 1; kind < AFFINITYGRAPH_EVENT_KIND_COUNT; ++kind) {
    if (kind != 1) out << ',';
    out << '"' << names[kind] << "\":" << counts[kind];
  }
  out << '}';
  return out.str();
}

std::vector<int> cpus_from_mask(const affinitygraph_bpf_event &event) {
  std::vector<int> cpus;
  size_t bytes = std::min<size_t>(event.mask_bytes, AFFINITYGRAPH_MASK_BYTES);
  for (size_t bit = 0; bit < bytes * 8; ++bit)
    if (event.mask[bit / 8] & (1U << (bit % 8))) cpus.push_back(static_cast<int>(bit));
  return cpus;
}

std::string module_offset(int tgid, uintptr_t address) {
  if (!address) return {};
  std::ifstream maps("/proc/" + std::to_string(tgid) + "/maps");
  std::string line;
  while (std::getline(maps, line)) {
    unsigned long long begin = 0, end = 0, offset = 0;
    char permissions[5]{};
    int consumed = 0;
    if (std::sscanf(line.c_str(), "%llx-%llx %4s %llx %*s %*s %n",
                    &begin, &end, permissions, &offset, &consumed) < 4) continue;
    if (address < begin || address >= end) continue;
    std::string path = line.substr(static_cast<size_t>(consumed));
    while (!path.empty() && path.front() == ' ') path.erase(path.begin());
    if (path.empty()) path = "anonymous";
    uintptr_t relative = address - begin + offset;
    return path + "+0x" + [&] {
      std::ostringstream value;
      value << std::hex << relative;
      return value.str();
    }();
  }
  return "0x" + [&] {
    std::ostringstream value;
    value << std::hex << address;
    return value.str();
  }();
}
} // namespace

Runtime::Runtime(Config config, int root_pid, std::shared_ptr<BpfRingReader> bpf_reader,
                 bool collector_degraded, std::string uprobe_status,
                 bool affinity_capability)
    : config_(std::move(config)), root_pid_(root_pid), supervisor_pid_(getpid()),
      collector_degraded_(collector_degraded), affinity_capability_(affinity_capability),
      uprobe_status_(std::move(uprobe_status)),
      hardware_(HardwareGraph::discover(config_.cpus)),
      graph_(config_.graph_horizon_seconds,
             {config_.activity_log_p95, config_.sync_log_p95, config_.share_log_p95}),
      bpf_reader_(std::move(bpf_reader)), actuator_(backend_),
      solver_(config_.mode == Mode::Plan) {
  runtime_instance_id_ = monotonic_ns();
  hardware_.load_calibration(config_.calibration_path);
  target_tgids_.insert(root_pid_);
  if (collector_degraded_ ||
      (config_.solver == "numa-domain-v1" && !bpf_reader_)) {
    collector_degraded_ = true;
    config_.mode = Mode::Observe;
  }
  std::filesystem::create_directories(config_.log_directory);
  std::string path = config_.log_directory + "/runtime.jsonl";
  log_fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  open_control_socket();
  if (bpf_reader_) bpf_worker_ = std::thread([this] { run_bpf(); });
  worker_ = std::thread([this] { run(); });
  log("runtime_start", "\"root_pid\":" + std::to_string(root_pid_) +
      ",\"effective_mode\":\"" +
      std::string(config_.mode == Mode::Active ? "active" : config_.mode == Mode::Plan ? "plan" : "observe") +
      "\",\"collector_degraded\":" + (collector_degraded_ ? "true" : "false") +
      ",\"affinity_capability\":" + (affinity_capability_ ? "true" : "false") +
      ",\"pthread_uprobe\":\"" + json_escape(uprobe_status_) + "\"");
  for (const auto &cpu : hardware_.cpus)
    log("topology_cpu", "\"cpu\":" + std::to_string(cpu.id) +
        ",\"node\":" + std::to_string(cpu.node) +
        ",\"online\":" + (cpu.online ? "true" : "false") +
        ",\"in_envelope\":true");
  for (const auto &[nodes, distance] : hardware_.node_distance)
  {
    auto calibration = hardware_.node_calibration.find(nodes);
    std::string calibrated;
    if (calibration != hardware_.node_calibration.end()) {
      const auto &value = calibration->second;
      calibrated = ",\"handoff_mean_ns\":" + std::to_string(value.handoff_mean_ns) +
          ",\"handoff_p95_ns\":" + std::to_string(value.handoff_p95_ns) +
          ",\"memory_load_mean_ns\":" + std::to_string(value.memory_load_mean_ns) +
          ",\"memory_load_cv\":" + std::to_string(value.memory_load_cv) +
          ",\"stream_2t_triad_mbps\":" + std::to_string(value.stream_2t_triad_mbps) +
          ",\"stream_32t_triad_mbps\":" + std::to_string(value.stream_32t_triad_mbps);
    }
    log("topology_edge", "\"from_node\":" + std::to_string(nodes.first) +
        ",\"to_node\":" + std::to_string(nodes.second) +
        ",\"numa_distance\":" + std::to_string(distance) + calibrated);
  }
}

Runtime::~Runtime() {
  stopping_ = true;
  if (bpf_worker_.joinable()) bpf_worker_.join();
  if (worker_.joinable()) worker_.join();
  consume_pending_bpf();
  sample_bpf_health(monotonic_ns());
  if (bpf_reader_) {
    BpfHealthSnapshot snapshot;
    {
      std::lock_guard lock(mutex_);
      snapshot = bpf_health_;
    }
    auto &health = snapshot.counters;
    double total = static_cast<double>(health.emitted + health.dropped);
    log("bpf_health", "\"valid\":" + std::string(snapshot.valid ? "true" : "false") +
        ",\"error\":" + std::to_string(snapshot.error) +
        ",\"emitted\":" + std::to_string(health.emitted) +
        ",\"dropped\":" + std::to_string(health.dropped) +
        ",\"loss_ratio\":" + std::to_string(total ? health.dropped / total : 0.0) +
        ",\"emitted_by_kind\":" + bpf_kind_counts_json(health.emitted_by_kind) +
        ",\"dropped_by_kind\":" + bpf_kind_counts_json(health.dropped_by_kind) +
        ",\"suppressed_by_kind\":" + bpf_kind_counts_json(health.suppressed_by_kind) +
        ",\"final\":true");
  }
  auto restored = actuator_.restore_all();
  log("runtime_stop", "\"restore_requested\":" + std::to_string(restored.requested) +
      ",\"restore_restored\":" + std::to_string(restored.restored) +
      ",\"restore_vanished\":" + std::to_string(restored.vanished) +
      ",\"restore_failed\":" + std::to_string(restored.failed) +
      ",\"restored\":" + (restored.success() ? "true" : "false"));
  if (control_fd_ >= 0) close(control_fd_);
  unlink(config_.socket_path.c_str());
  if (log_fd_ >= 0) close(log_fd_);
}

size_t Runtime::PendingRelationHash::operator()(
    const PendingRelationKey &key) const noexcept {
  size_t value = key.kind;
  auto combine = [&](uint64_t part) {
    value ^= std::hash<uint64_t>{}(part) + 0x9e3779b97f4a7c15ULL +
             (value << 6) + (value >> 2);
  };
  combine(key.tgid);
  combine(key.tid);
  combine(key.peer_tid);
  combine(key.start_time_ns);
  combine(key.peer_start_time_ns);
  return value;
}

void Runtime::open_control_socket() {
  control_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (control_fd_ < 0) return;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (config_.socket_path.size() >= sizeof(address.sun_path)) {
    close(control_fd_); control_fd_ = -1; return;
  }
  std::strncpy(address.sun_path, config_.socket_path.c_str(), sizeof(address.sun_path) - 1);
  unlink(address.sun_path);
  mode_t old = umask(0177);
  int rc = bind(control_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address));
  umask(old);
  if (rc != 0 || listen(control_fd_, 4) != 0) {
    close(control_fd_); control_fd_ = -1; return;
  }
  chmod(address.sun_path, 0600);
}

void Runtime::log(const std::string &type, const std::string &fields) {
  if (log_fd_ < 0) return;
  std::ostringstream line;
  line << "{\"timestamp_ns\":" << monotonic_ns() << ",\"type\":\"" << type << '"';
  if (!fields.empty()) line << ',' << fields;
  line << "}\n";
  auto value = line.str();
  [[maybe_unused]] ssize_t written = write(log_fd_, value.data(), value.size());
}

void Runtime::thread_started(const LifecycleRecord &record) {
  std::lock_guard lock(mutex_);
  lifecycle_[record.tid] = record;
  if (record.tgid > 0) target_tgids_.insert(record.tgid);
  log("thread_start", "\"tgid\":" + std::to_string(record.tgid) +
      ",\"tid\":" + std::to_string(record.tid) +
      ",\"parent_tid\":" + std::to_string(record.parent_tid) +
      ",\"start_routine\":" + std::to_string(record.start_routine) +
      ",\"start_symbol\":\"" + json_escape(record.start_symbol) + "\"");
}

void Runtime::thread_renamed(int tid, const std::string &name) {
  std::lock_guard lock(mutex_);
  if (auto it = lifecycle_.find(tid); it != lifecycle_.end()) {
    it->second.name = name;
  }
  log("thread_name", "\"tid\":" + std::to_string(tid) +
      ",\"name\":\"" + json_escape(name) + "\"");
}

void Runtime::thread_exited(int tid, const char *reason) {
  std::lock_guard lock(mutex_);
  lifecycle_.erase(tid);
  pending_application_masks_.erase(tid);
  current_plan_.erase(tid);
  current_masks_.erase(tid);
  active_cohort_.erase(tid);
  solver_.remove_thread(tid);
  domain_solver_.remove_thread(tid);
  log("thread_exit", "\"tid\":" + std::to_string(tid) +
      ",\"reason\":\"" + reason + "\"");
}

void Runtime::application_affinity(int tid, const std::vector<int> &cpus) {
  if (cpus.empty()) return;
  std::lock_guard lock(mutex_);
  bool pending = !lifecycle_.contains(tid);
  if (pending) pending_application_masks_[tid] = cpus;
  else actuator_.note_application_mask(tid, cpus);
  log("application_affinity", "\"tid\":" + std::to_string(tid) +
      ",\"mask\":\"" + format_cpu_list(cpus) + "\",\"pending\":" +
      (pending ? "true" : "false"));
}

void Runtime::handle_bpf_event(const affinitygraph_bpf_event &event) {
  if (event.kind == AFFINITYGRAPH_FUTEX || event.kind == AFFINITYGRAPH_VFS) {
    std::lock_guard lock(mutex_);
    auto from = lifecycle_.find(static_cast<int>(event.tid));
    auto peer = lifecycle_.find(static_cast<int>(event.peer_tid));
    if (from == lifecycle_.end() || peer == lifecycle_.end()) return;
    if ((event.start_time_ns && from->second.bpf_start_time_ns &&
         event.start_time_ns != from->second.bpf_start_time_ns) ||
        (event.peer_start_time_ns && peer->second.bpf_start_time_ns &&
         event.peer_start_time_ns != peer->second.bpf_start_time_ns)) return;
    RelationObservation observation;
    observation.from_tid = static_cast<int>(event.tid);
    observation.to_tid = static_cast<int>(event.peer_tid);
    observation.from_starttime = from->second.proc_starttime;
    observation.to_starttime = peer->second.proc_starttime;
    observation.timestamp_ns = event.timestamp_ns;
    observation.active_overlap = 1.0;
    const double aggregation_seconds = event.resource
        ? std::max(static_cast<double>(event.resource) / 1e9, 1e-6)
        : static_cast<double>(config_.sample_interval_seconds);
    if (event.kind == AFFINITYGRAPH_FUTEX)
      observation.futex_per_second =
          static_cast<double>(event.value_ns ? event.value_ns : 1) /
          aggregation_seconds;
    else
      observation.shared_vfs_seconds = static_cast<double>(event.value_ns) / 1e9 *
          config_.sample_interval_seconds / aggregation_seconds;
    graph_.observe_relation(observation);
    return;
  }
  if (event.kind == AFFINITYGRAPH_TASK_FORK) {
    LifecycleRecord record;
    record.tgid = static_cast<int>(event.tgid);
    record.parent_tgid = static_cast<int>(event.parent_tgid);
    record.parent_tid = static_cast<int>(event.parent_tid);
    record.tid = static_cast<int>(event.tid);
    record.start_routine = static_cast<uintptr_t>(event.start_routine);
    record.bpf_start_time_ns = event.start_time_ns;
    record.created_ns = event.timestamp_ns;
    record.name.assign(event.comm, strnlen(event.comm, sizeof(event.comm)));
    record.start_symbol = module_offset(record.tgid, record.start_routine);
    thread_started(record);
    return;
  }
  if (event.kind == AFFINITYGRAPH_TASK_EXEC) {
    std::lock_guard lock(mutex_);
    target_tgids_.insert(static_cast<int>(event.tgid));
    log("task_exec", "\"tgid\":" + std::to_string(event.tgid) +
        ",\"tid\":" + std::to_string(event.tid));
    return;
  }
  if (event.kind == AFFINITYGRAPH_TASK_EXIT) {
    thread_exited(static_cast<int>(event.tid), "sched_process_exit");
    return;
  }
  if (event.kind == AFFINITYGRAPH_TASK_RENAME) {
    thread_renamed(static_cast<int>(event.tid),
                   std::string(event.comm, strnlen(event.comm, sizeof(event.comm))));
    return;
  }
  if (event.kind == AFFINITYGRAPH_AFFINITY)
    application_affinity(static_cast<int>(event.peer_tid), cpus_from_mask(event));
}

void Runtime::drain_bpf_once() {
  if (!bpf_reader_) return;
  auto lifecycle = drain_bpf_events(*bpf_reader_);
  auto futex = drain_bpf_futex_aggregates(*bpf_reader_);
  auto vfs = drain_bpf_vfs_aggregates(*bpf_reader_);
  std::lock_guard lock(bpf_pending_mutex_);
  constexpr size_t lifecycle_limit = 65536;
  constexpr size_t relation_limit = 524288;
  for (const auto &event : lifecycle) {
    if (pending_lifecycle_events_.size() >= lifecycle_limit) {
      ++pending_lifecycle_overflow_;
      continue;
    }
    pending_lifecycle_events_.push_back(event);
  }
  auto merge_relation = [&](const affinitygraph_bpf_event &event) {
    PendingRelationKey key{event.kind, event.tgid, event.tid, event.peer_tid,
                           event.start_time_ns, event.peer_start_time_ns};
    auto found = pending_relations_.find(key);
    if (found == pending_relations_.end()) {
      if (pending_relations_.size() >= relation_limit) {
        ++pending_relation_overflow_;
        return;
      }
      if (pending_relations_.empty()) pending_relation_started_ns_ = monotonic_ns();
      pending_relations_.emplace(
          key, PendingRelationValue{event.timestamp_ns, event.value_ns});
      return;
    }
    found->second.value_ns += event.value_ns;
    found->second.timestamp_ns =
        std::max(found->second.timestamp_ns, event.timestamp_ns);
  };
  for (const auto &event : futex) merge_relation(event);
  for (const auto &event : vfs) merge_relation(event);
}

void Runtime::consume_pending_bpf() {
  std::deque<affinitygraph_bpf_event> lifecycle;
  std::unordered_map<PendingRelationKey, PendingRelationValue,
                     PendingRelationHash> relations;
  uint64_t lifecycle_overflow = 0;
  uint64_t relation_overflow = 0;
  uint64_t relation_duration_ns = 0;
  {
    std::lock_guard lock(bpf_pending_mutex_);
    lifecycle.swap(pending_lifecycle_events_);
    relations.swap(pending_relations_);
    lifecycle_overflow = std::exchange(pending_lifecycle_overflow_, 0);
    relation_overflow = std::exchange(pending_relation_overflow_, 0);
    if (pending_relation_started_ns_) {
      relation_duration_ns = monotonic_ns() - pending_relation_started_ns_;
      pending_relation_started_ns_ = 0;
    }
  }
  std::deque<affinitygraph_bpf_event> exits;
  for (const auto &event : lifecycle) {
    if (event.kind == AFFINITYGRAPH_TASK_EXIT) exits.push_back(event);
    else handle_bpf_event(event);
  }
  const uint64_t minimum_duration_ns =
      static_cast<uint64_t>(config_.sample_interval_seconds) * 1000000000ULL;
  relation_duration_ns = std::max(relation_duration_ns, minimum_duration_ns);
  const double aggregation_seconds =
      static_cast<double>(relation_duration_ns) / 1e9;
  std::unordered_map<uint64_t, double> pair_weights;
  pair_weights.reserve(relations.size());
  auto pair_id = [](ag_u32 left, ag_u32 right) {
    ag_u32 first = std::min(left, right);
    ag_u32 second = std::max(left, right);
    return (static_cast<uint64_t>(first) << 32) | second;
  };
  for (const auto &[key, value] : relations) {
    uint64_t pair = pair_id(key.tid, key.peer_tid);
    double weight = 0;
    if (key.kind == AFFINITYGRAPH_FUTEX) {
      const double rate = static_cast<double>(value.value_ns) / aggregation_seconds;
      weight = 0.7 * std::min(std::log1p(rate) / config_.sync_log_p95, 1.0);
    } else {
      const double seconds = static_cast<double>(value.value_ns) / 1e9 *
          config_.sample_interval_seconds / aggregation_seconds;
      weight = 0.3 * std::min(std::log1p(seconds) / config_.share_log_p95, 1.0);
    }
    pair_weights[pair] += weight;
  }
  std::unordered_map<ag_u32, std::vector<std::pair<double, uint64_t>>> incident;
  incident.reserve(lifecycle_.size());
  double input_weight = 0;
  for (const auto &[pair, weight] : pair_weights) {
    if (config_.solver != "numa-domain-v1") {
      ag_u32 first = static_cast<ag_u32>(pair >> 32);
      ag_u32 second = static_cast<ag_u32>(pair);
      incident[first].push_back({weight, pair});
      incident[second].push_back({weight, pair});
    }
    input_weight += weight;
  }
  std::unordered_set<uint64_t> retained_pairs;
  if (config_.solver == "numa-domain-v1") {
    // NUMA placement aggregates complete relation evidence by family before
    // applying its family-level heavy-hitter bound.
    retained_pairs.reserve(pair_weights.size());
    for (const auto &[pair, _] : pair_weights) retained_pairs.insert(pair);
  } else {
    retained_pairs.reserve(std::min(
        pair_weights.size(), lifecycle_.size() * static_cast<size_t>(
                                                config_.hotspot_edges_per_thread)));
    for (auto &[tid, candidates] : incident) {
      (void)tid;
      std::sort(candidates.begin(), candidates.end(), [](const auto &left,
                                                          const auto &right) {
        if (left.first != right.first) return left.first > right.first;
        return left.second < right.second;
      });
      size_t limit = std::min(
          candidates.size(), static_cast<size_t>(config_.hotspot_edges_per_thread));
      for (size_t index = 0; index < limit; ++index)
        retained_pairs.insert(candidates[index].second);
    }
  }
  double retained_weight = 0;
  for (uint64_t pair : retained_pairs) retained_weight += pair_weights[pair];
  std::set<std::pair<int, int>> resident_relations;
  for (uint64_t pair : retained_pairs)
    resident_relations.emplace(static_cast<int>(pair >> 32),
                               static_cast<int>(static_cast<ag_u32>(pair)));
  {
    std::lock_guard lock(mutex_);
    graph_.retain_relations(resident_relations);
  }
  size_t retained_records = 0;
  for (const auto &[key, value] : relations) {
    if (!retained_pairs.contains(pair_id(key.tid, key.peer_tid))) continue;
    affinitygraph_bpf_event event{};
    event.kind = key.kind;
    event.tgid = key.tgid;
    event.tid = key.tid;
    event.peer_tid = key.peer_tid;
    event.start_time_ns = key.start_time_ns;
    event.peer_start_time_ns = key.peer_start_time_ns;
    event.timestamp_ns = value.timestamp_ns;
    event.value_ns = value.value_ns;
    event.resource = relation_duration_ns;
    handle_bpf_event(event);
    ++retained_records;
  }
  if (!relations.empty())
    log("relation_ingest_summary", "\"input_records\":" +
        std::to_string(relations.size()) + ",\"input_pairs\":" +
        std::to_string(pair_weights.size()) + ",\"retained_records\":" +
        std::to_string(retained_records) + ",\"retained_pairs\":" +
        std::to_string(retained_pairs.size()) + ",\"edges_per_tid\":" +
        std::to_string(config_.hotspot_edges_per_thread) +
        ",\"pruning_scope\":\"" +
        (config_.solver == "numa-domain-v1" ? std::string("family_solver")
                                             : std::string("tid_ingest")) + "\"" +
        ",\"weight_coverage\":" +
        std::to_string(input_weight > 0 ? retained_weight / input_weight : 1.0));
  for (const auto &event : exits) handle_bpf_event(event);
  if (lifecycle_overflow || relation_overflow) {
    log("bpf_pending_overflow", "\"lifecycle\":" +
        std::to_string(lifecycle_overflow) + ",\"relations\":" +
        std::to_string(relation_overflow));
    trigger_fatal("BPF user-space pending buffer overflow");
  }
}

void Runtime::pause() {
  paused_ = true;
  std::lock_guard lock(mutex_);
  last_restore_ = actuator_.restore_all();
  current_plan_.clear();
  current_masks_.clear();
  active_cohort_.clear();
  selector_ready_ = false;
  solver_.reset();
  domain_solver_.reset();
  log("pause", "\"restore_requested\":" + std::to_string(last_restore_.requested) +
      ",\"restore_restored\":" + std::to_string(last_restore_.restored) +
      ",\"restore_vanished\":" + std::to_string(last_restore_.vanished) +
      ",\"restore_failed\":" + std::to_string(last_restore_.failed) +
      ",\"restored\":" + (last_restore_.success() ? "true" : "false"));
}

void Runtime::resume() { paused_ = false; log("resume"); }

std::string Runtime::status_json() const {
  std::lock_guard lock(mutex_);
  std::ostringstream targets;
  targets << '[';
  bool first = true;
  for (int tgid : target_tgids_) {
    if (!first) targets << ',';
    first = false;
    targets << tgid;
  }
  targets << ']';
  std::ostringstream out;
  out << "{\"supervisor_pid\":" << supervisor_pid_ << ",\"root_pid\":" << root_pid_
      << ",\"target_tgids\":" << targets.str() << ",\"effective_mode\":\""
      << (config_.mode == Mode::Active ? "active" : config_.mode == Mode::Plan ? "plan" : "observe")
      << "\",\"bpf\":" << (bpf_reader_ ? "true" : "false")
      << ",\"bpf_health_valid\":" << (bpf_health_.valid ? "true" : "false")
      << ",\"bpf_health_error\":" << bpf_health_.error
      << ",\"bpf_emitted\":" << bpf_health_.counters.emitted
      << ",\"bpf_dropped\":" << bpf_health_.counters.dropped
      << ",\"bpf_window_ready\":" << (bpf_window_ready_ ? "true" : "false")
      << ",\"bpf_window_loss_ratio\":" << bpf_window_loss_ratio_
      << ",\"ring_capacity_bytes\":" << bpf_reader_stats_.capacity_bytes
      << ",\"ring_occupancy_bytes\":" << bpf_reader_stats_.occupancy_bytes
      << ",\"ring_max_occupancy_bytes\":" << bpf_reader_stats_.max_occupancy_bytes
      << ",\"consumer_drain_calls\":" << bpf_reader_stats_.drain_calls
      << ",\"consumer_events\":" << bpf_reader_stats_.events_consumed
      << ",\"consumer_last_batch\":" << bpf_reader_stats_.last_batch_events
      << ",\"consumer_max_batch\":" << bpf_reader_stats_.max_batch_events
      << ",\"consumer_last_drain_ns\":" << bpf_reader_stats_.last_drain_ns
      << ",\"consumer_max_drain_ns\":" << bpf_reader_stats_.max_drain_ns
      << ",\"consumer_last_lag_ns\":" << bpf_reader_stats_.last_max_lag_ns
      << ",\"consumer_max_lag_ns\":" << bpf_reader_stats_.max_lag_ns
      << ",\"futex_aggregate_records\":" << bpf_reader_stats_.futex_aggregate_records
      << ",\"futex_handoffs\":" << bpf_reader_stats_.futex_handoffs
      << ",\"vfs_aggregate_records\":" << bpf_reader_stats_.vfs_aggregate_records
      << ",\"vfs_handoffs\":" << bpf_reader_stats_.vfs_handoffs
      << ",\"collector_degraded\":" << (collector_degraded_ ? "true" : "false")
      << ",\"affinity_capability\":" << (affinity_capability_ ? "true" : "false")
      << ",\"pthread_uprobe\":\"" << json_escape(uprobe_status_) << "\""
      << ",\"paused\":" << (paused_ ? "true" : "false")
      << ",\"threads\":" << lifecycle_.size()
      << ",\"planned_threads\":"
      << (config_.solver == "numa-domain-v1" ? current_masks_.size()
                                               : current_plan_.size())
      << ",\"active_cohort_threads\":" << active_cohort_.size()
      << ",\"selector_ready\":" << (selector_ready_ ? "true" : "false")
      << ",\"policy_armed\":"
      << (config_.mode == Mode::Active && !paused_ && selector_ready_ &&
                  (!bpf_reader_ || (bpf_health_.valid && bpf_window_ready_ &&
                                    bpf_window_loss_ratio_ < 0.01)) &&
                  fatal_error_.empty() ? "true" : "false")
      << ",\"planned_assignments\":" << assignments_json(current_plan_)
      << ",\"planned_masks\":" << masks_json(current_masks_)
      << ",\"family_metrics\":" << family_metrics_json(domain_solver_.families())
      << ",\"active_domains\":" << domains_json(domain_solver_.domains())
      << ",\"active_effective\":"
      << (config_.mode == Mode::Active && !paused_ && selector_ready_ &&
                  (config_.solver == "numa-domain-v1"
                       ? (current_masks_.empty() || domain_solver_.effective())
                       : ((active_cohort_.empty() && current_plan_.empty() &&
                           solver_.pinned_threads() == 0) || solver_.effective())) &&
                  (!bpf_reader_ || (bpf_health_.valid && bpf_window_ready_ &&
                                    bpf_window_loss_ratio_ < 0.01)) &&
                  fatal_error_.empty() ? "true" : "false")
      << ",\"solver_phase\":\""
      << (config_.solver == "numa-domain-v1" ? "numa_domain"
                                               : incremental_phase_name(solver_.phase()))
      << '"' << ",\"placement_generation\":"
      << (config_.solver == "numa-domain-v1" ? domain_solver_.generation()
                                               : solver_.generation())
      << ",\"pinned_threads\":" << solver_.pinned_threads()
      << ",\"action_requested\":" << action_requested_
      << ",\"action_committed\":" << action_committed_
      << ",\"action_vanished\":" << action_vanished_
      << ",\"action_failures\":" << action_failures_
      << ",\"action_rolled_back\":" << action_rolled_back_
      << ",\"restore_requested\":" << last_restore_.requested
      << ",\"restore_restored\":" << last_restore_.restored
      << ",\"restore_vanished\":" << last_restore_.vanished
      << ",\"restore_failed\":" << last_restore_.failed
      << ",\"fatal_error\":\"" << json_escape(fatal_error_) << '"' << '}';
  return out.str();
}

void Runtime::service_control_socket() {
  if (control_fd_ < 0) return;
  int client = accept4(control_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
  if (client < 0) return;
  char buffer[32]{};
  ssize_t length = read(client, buffer, sizeof(buffer) - 1);
  std::string command = length > 0 ? std::string(buffer, static_cast<size_t>(length)) : "";
  command.erase(std::remove_if(command.begin(), command.end(),
                               [](unsigned char c) { return std::isspace(c); }), command.end());
  std::string response;
  if (command == "status" || command == "dump") response = status_json();
  else if (command == "pause") { pause(); response = status_json(); }
  else if (command == "resume") { resume(); response = status_json(); }
  else response = "{\"error\":\"unknown command\"}";
  response.push_back('\n');
  [[maybe_unused]] ssize_t written = write(client, response.data(), response.size());
  close(client);
}

void Runtime::reconcile_and_sample() {
  try {
    std::set<int> targets;
    {
      std::lock_guard lock(mutex_);
      targets = target_tgids_;
    }
    std::vector<ThreadSample> samples;
    std::set<int> existing_targets;
    for (int tgid : targets) {
      if (!std::filesystem::exists("/proc/" + std::to_string(tgid) + "/task")) continue;
      existing_targets.insert(tgid);
      auto process_samples = collector_.sample(tgid);
      samples.insert(samples.end(), process_samples.begin(), process_samples.end());
      auto pages = collector_.numa_pages(tgid);
      std::ostringstream fields;
      fields << "\"scope\":\"process\",\"tgid\":" << tgid << ",\"pages\":{";
      bool first = true;
      for (const auto &[node, count] : pages) {
        if (!first) fields << ',';
        first = false;
        fields << '"' << node << "\":" << count;
      }
      fields << '}';
      log("numa_maps", fields.str());
    }
    if (samples.empty()) throw std::runtime_error("no target task samples");
    std::set<int> seen;
    {
      std::lock_guard lock(mutex_);
      target_tgids_ = std::move(existing_targets);
      for (auto &sample : samples) {
        seen.insert(sample.identity.tid);
        auto life = lifecycle_.find(sample.identity.tid);
        bool fresh = life == lifecycle_.end() ||
                     (life->second.proc_starttime && life->second.proc_starttime != sample.identity.starttime);
        if (fresh) {
          LifecycleRecord record;
          record.tgid = sample.identity.tgid;
          record.parent_tgid = sample.identity.tgid;
          record.parent_tid = sample.identity.tgid;
          record.tid = sample.identity.tid;
          record.created_ns = sample.timestamp_ns;
          record.proc_starttime = sample.identity.starttime;
          record.name = sample.comm;
          lifecycle_[sample.identity.tid] = record;
          actuator_.note_inherited_mask(sample.identity.tid, record.parent_tid,
                                        sample.allowed_cpus);
          life = lifecycle_.find(sample.identity.tid);
        } else if (!life->second.proc_starttime) {
          life->second.proc_starttime = sample.identity.starttime;
          life->second.inherited_mask = sample.allowed_cpus;
          actuator_.note_inherited_mask(sample.identity.tid, life->second.parent_tid,
                                        sample.allowed_cpus);
        }
        life->second.name = sample.comm;
        if (auto pending = pending_application_masks_.find(sample.identity.tid);
            pending != pending_application_masks_.end()) {
          actuator_.note_application_mask(sample.identity.tid, pending->second);
          pending_application_masks_.erase(pending);
        }
        sample.parent_tid = life->second.parent_tid;
        sample.start_routine = life->second.start_routine;
        sample.start_symbol = life->second.start_symbol;
      }
      for (auto it = lifecycle_.begin(); it != lifecycle_.end();) {
        if (!seen.contains(it->first)) {
          current_plan_.erase(it->first);
          current_masks_.erase(it->first);
          domain_solver_.remove_thread(it->first);
          it = lifecycle_.erase(it);
        } else ++it;
      }
      graph_.observe_threads(samples);
    }
    collector_failed_since_ns_ = 0;
  } catch (const std::exception &error) {
    if (!collector_failed_since_ns_) collector_failed_since_ns_ = monotonic_ns();
    log("collector_failure", "\"message\":\"" + json_escape(error.what()) + "\"");
    uint64_t delay = static_cast<uint64_t>(config_.collector_failure_restore_seconds) * 1000000000ULL;
    if (monotonic_ns() - collector_failed_since_ns_ >= delay && !paused_) pause();
  }
}

void Runtime::solve_numa_domains(
    const std::string &window_id, const std::vector<ThreadDemand> &demands,
    const std::vector<RelationEdge> &edges,
    const std::map<int, std::vector<int>> &allowed_masks, uint64_t now) {
  if (!bpf_reader_ || !bpf_health_.valid || !bpf_window_ready_ ||
      bpf_window_loss_ratio_ >= 0.01) {
    selector_ready_ = false;
    log("solve_window_end", "\"window_id\":\"" + window_id +
        "\",\"complete\":true,\"outcome\":\"waiting_required_bpf\"");
    return;
  }
  NumaDomainOptions options;
  options.family_minimum_demand = config_.family_minimum_demand;
  options.family_minimum_internal_relation =
      config_.family_minimum_internal_relation;
  options.family_minimum_self_containment =
      config_.family_minimum_self_containment;
  options.family_minimum_relative_internal =
      config_.family_minimum_relative_internal;
  options.domain_merge_ratio = config_.domain_merge_ratio;
  options.family_edges_per_family =
      static_cast<size_t>(config_.family_edges_per_family);
  options.family_stability_confirmations =
      config_.family_stability_confirmations;
  options.domain_stability_confirmations =
      config_.domain_stability_confirmations;
  options.plan_confirmations = config_.domain_plan_confirmations;
  options.maximum_threads_per_domain =
      static_cast<size_t>(config_.maximum_threads_per_domain);
  options.capacity_ratio = config_.domain_capacity_ratio;
  options.expand_ratio = config_.domain_expand_ratio;
  options.expand_confirmations = config_.domain_expand_confirmations;
  options.shrink_ratio = config_.domain_shrink_ratio;
  options.shrink_confirmations = config_.domain_shrink_confirmations;
  options.minimum_dwell_ns =
      static_cast<uint64_t>(config_.domain_minimum_dwell_seconds) *
      1000000000ULL;

  auto started = std::chrono::steady_clock::now();
  auto proposal = domain_solver_.propose(hardware_, demands, edges,
                                         allowed_masks, options, now);
  auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - started)
                         .count();
  size_t forced_migrations = std::count_if(
      proposal.actions.begin(), proposal.actions.end(),
      [](const auto &action) { return action.forced_migration; });
  log("plan", "\"window_id\":\"" + window_id +
      "\",\"strategy_id\":\"numa-domain-v1\"" +
      ",\"affinity_granularity\":\"numa_node_mask\"" +
      ",\"valid\":" + (proposal.valid ? "true" : "false") +
      ",\"invalid_reason\":\"" + json_escape(proposal.invalid_reason) +
      "\",\"solve_duration_ns\":" + std::to_string(duration_ns) +
      ",\"mask_updates\":" + std::to_string(proposal.actions.size()) +
      ",\"inherited_threads\":" +
          std::to_string(proposal.inherited_tids.size()) +
      ",\"forced_migrations\":" + std::to_string(forced_migrations) +
      ",\"family_metrics\":" + family_metrics_json(proposal.families) +
      ",\"domains\":" + domains_json(proposal.domains) +
      ",\"planned_masks\":" + masks_json(proposal.planned_masks) +
      ",\"actions\":" + domain_actions_json(proposal.actions));

  if (!proposal.valid) {
    selector_ready_ = false;
    if (config_.mode == Mode::Active && !proposal.released_tids.empty()) {
      auto restored = actuator_.restore(proposal.released_tids);
      last_restore_ = restored;
      if (!restored.success()) {
        domain_solver_.discard(proposal);
        trigger_fatal("invalid NUMA domain affinity restore failed");
        return;
      }
    }
    domain_solver_.commit(proposal, now);
    current_masks_ = domain_solver_.placement();
    log("solve_window_end", "\"window_id\":\"" + window_id +
        "\",\"complete\":true,\"outcome\":\"invalid_domain_unrestricted\"");
    return;
  }
  selector_ready_ = true;
  if (!proposal.ready) {
    domain_solver_.discard(proposal);
    log("solve_window_end", "\"window_id\":\"" + window_id +
        "\",\"complete\":true,\"outcome\":\"planned\"");
    return;
  }
  if (proposal.actions.empty() && proposal.released_tids.empty() &&
      proposal.planned_masks == domain_solver_.placement()) {
    domain_solver_.discard(proposal);
    log("solve_window_end", "\"window_id\":\"" + window_id +
        "\",\"complete\":true,\"outcome\":\"domain_stable\"");
    return;
  }
  if (config_.mode == Mode::Plan) {
    domain_solver_.commit(proposal, now);
    current_masks_ = domain_solver_.placement();
    log("shadow_commit", "\"window_id\":\"" + window_id +
        "\",\"generation\":" +
        std::to_string(domain_solver_.generation()) +
        ",\"planned_masks\":" + masks_json(current_masks_));
    log("solve_window_end", "\"window_id\":\"" + window_id +
        "\",\"complete\":true,\"outcome\":\"shadow_committed\"");
    return;
  }

  if (!proposal.released_tids.empty()) {
    auto restored = actuator_.restore(proposal.released_tids);
    last_restore_ = restored;
    log("domain_restore", "\"window_id\":\"" + window_id +
        "\",\"requested\":" + std::to_string(restored.requested) +
        ",\"restored\":" + std::to_string(restored.restored) +
        ",\"vanished\":" + std::to_string(restored.vanished) +
        ",\"failed\":" + std::to_string(restored.failed));
    if (!restored.success()) {
      domain_solver_.discard(proposal);
      trigger_fatal("NUMA domain affinity restore failed");
      return;
    }
  }
  std::set<int> live;
  for (const auto &demand : demands) live.insert(demand.identity.tid);
  auto result = actuator_.apply(proposal.delta, live);
  action_requested_ += result.requested;
  action_committed_ += result.committed;
  action_vanished_ += result.vanished;
  action_rolled_back_ += result.rolled_back;
  if (!result.success) ++action_failures_;
  log("action", "\"window_id\":\"" + window_id +
      "\",\"success\":" + (result.success ? "true" : "false") +
      ",\"requested\":" + std::to_string(result.requested) +
      ",\"committed\":" + std::to_string(result.committed) +
      ",\"vanished\":" + std::to_string(result.vanished) +
      ",\"rolled_back\":" + std::to_string(result.rolled_back) +
      ",\"mask_updates\":" + std::to_string(proposal.actions.size()) +
      ",\"forced_migrations\":" + std::to_string(forced_migrations) +
      ",\"actions\":" + domain_actions_json(proposal.actions));
  if (!result.success) {
    domain_solver_.discard(proposal);
    log("solve_window_end", "\"window_id\":\"" + window_id +
        "\",\"complete\":true,\"outcome\":\"action_failed\"");
    return;
  }
  for (int tid : result.vanished_tids) domain_solver_.remove_thread(tid);
  if (!proposal.actions.empty() && result.committed_tids.empty()) {
    for (int tid : proposal.released_tids) domain_solver_.remove_thread(tid);
    domain_solver_.discard(proposal);
    current_masks_ = domain_solver_.placement();
    log("solve_window_end", "\"window_id\":\"" + window_id +
        "\",\"complete\":true,\"outcome\":\"actions_vanished\"");
    return;
  }
  domain_solver_.commit(proposal, now, result.committed_tids);
  current_masks_ = domain_solver_.placement();
  log("action_commit", "\"window_id\":\"" + window_id +
      "\",\"generation\":" + std::to_string(domain_solver_.generation()) +
      ",\"planned_masks\":" + masks_json(current_masks_));
  log("solve_window_end", "\"window_id\":\"" + window_id +
      "\",\"complete\":true,\"outcome\":\"action_committed\"");
}

void Runtime::maybe_solve(uint64_t now) {
  uint64_t interval = static_cast<uint64_t>(config_.solve_interval_seconds) * 1000000000ULL;
  if (now - last_solve_ns_ < interval || paused_) return;
  last_solve_ns_ = now;
  std::lock_guard lock(mutex_);
  const std::string window_id = std::to_string(root_pid_) + "-" +
                                std::to_string(runtime_instance_id_) + "-" +
                                std::to_string(++solve_window_sequence_);
  log("solve_window_begin", "\"window_id\":\"" + window_id +
      "\",\"sequence\":" + std::to_string(solve_window_sequence_) +
      ",\"mode\":\"" +
      std::string(config_.mode == Mode::Active ? "active" :
                  config_.mode == Mode::Plan ? "plan" : "observe") + "\"");
  auto demands = graph_.demands();
  std::map<int, std::vector<int>> allowed_masks;
  auto thread_records = graph_.thread_records();
  for (const auto &record : thread_records) {
    allowed_masks[record.identity.tid] = record.allowed_cpus;
    log("thread_window", "\"window_id\":\"" + window_id +
        "\",\"tgid\":" + std::to_string(record.identity.tgid) +
        ",\"tid\":" + std::to_string(record.identity.tid) +
        ",\"starttime\":" + std::to_string(record.identity.starttime) +
        ",\"comm\":\"" + json_escape(record.comm) +
        "\",\"parent_tid\":" + std::to_string(record.parent_tid) +
        ",\"start_routine\":" + std::to_string(record.start_routine) +
        ",\"start_symbol\":\"" + json_escape(record.start_symbol) +
        "\",\"group\":\"" + json_escape(record.group) +
        "\",\"demand\":" + std::to_string(record.demand) +
        ",\"confidence\":" + std::to_string(record.confidence) +
        ",\"current_cpu\":" + std::to_string(record.current_cpu) +
        ",\"allowed_cpus\":" + cpu_array_json(record.allowed_cpus) +
        ",\"state\":\"" + std::string(1, record.state) +
        "\",\"sample_count\":" + std::to_string(record.sample_count) +
        ",\"runtime_delta_ns\":" + std::to_string(record.runtime_delta_ns) +
        ",\"runqueue_delta_ns\":" + std::to_string(record.runqueue_delta_ns) +
        ",\"voluntary_switches_delta\":" +
            std::to_string(record.voluntary_switches_delta) +
        ",\"involuntary_switches_delta\":" +
            std::to_string(record.involuntary_switches_delta));
  }
  auto edges = graph_.edges();
  auto graph_delta = graph_.take_delta(
      config_.demand_dirty_threshold,
      config_.edge_dirty_absolute_threshold,
      config_.edge_dirty_relative_threshold);
  constexpr size_t relation_log_limit = 4096;
  std::vector<const RelationEdge *> logged_edges;
  logged_edges.reserve(edges.size());
  for (const auto &edge : edges) logged_edges.push_back(&edge);
  const size_t logged_edge_count =
      std::min(relation_log_limit, logged_edges.size());
  std::partial_sort(
      logged_edges.begin(), logged_edges.begin() + logged_edge_count,
      logged_edges.end(), [](const auto *left, const auto *right) {
        if (left->score != right->score) return left->score > right->score;
        if (left->from_tid != right->from_tid)
          return left->from_tid < right->from_tid;
        return left->to_tid < right->to_tid;
      });
  log("relation_edge_summary", "\"window_id\":\"" + window_id +
      "\",\"total\":" + std::to_string(edges.size()) +
      ",\"logged\":" + std::to_string(logged_edge_count) +
      ",\"truncated\":" +
      (edges.size() > logged_edge_count ? "true" : "false"));
  for (size_t index = 0; index < logged_edge_count; ++index) {
    const auto &edge = *logged_edges[index];
    log("relation_edge", "\"window_id\":\"" + window_id +
        "\",\"from_tid\":" + std::to_string(edge.from_tid) +
        ",\"to_tid\":" + std::to_string(edge.to_tid) +
        ",\"activity\":" + std::to_string(edge.activity) +
        ",\"sync\":" + std::to_string(edge.sync) +
        ",\"share\":" + std::to_string(edge.share) +
        ",\"stability\":" + std::to_string(edge.stability) +
        ",\"score\":" + std::to_string(edge.score) +
        ",\"handoff_rate\":" + std::to_string(edge.handoff_rate) +
        ",\"shared_vfs_seconds\":" + std::to_string(edge.shared_vfs_seconds) +
        ",\"active_overlap\":" + std::to_string(edge.active_overlap) +
        ",\"observation_count\":" + std::to_string(edge.observation_count) +
        ",\"coverage\":" + std::to_string(edge.coverage) +
        ",\"cv\":" + std::to_string(edge.coefficient_of_variation));
  }
  if (config_.mode == Mode::Observe) {
    log("solve_window_end", "\"window_id\":\"" + window_id +
        "\",\"complete\":true,\"outcome\":\"observed\"");
    return;
  }
  if (config_.solver == "numa-domain-v1") {
    solve_numa_domains(window_id, demands, edges, allowed_masks, now);
    return;
  }
  demands.erase(std::remove_if(demands.begin(), demands.end(), [&](const auto &d) {
    return d.confidence < config_.minimum_confidence;
  }), demands.end());
  std::vector<ThreadDemand> active_candidates;
  for (const auto &demand : demands) {
    bool retained = active_cohort_.contains(demand.identity.tid) &&
                    demand.demand >= config_.inactive_demand_threshold;
    if (demand.demand >= config_.active_demand_threshold || retained) {
      active_candidates.push_back(demand);
    }
  }
  IncrementalOptions selector_options;
  selector_options.hotspot_edges_per_thread = config_.hotspot_edges_per_thread;
  selector_options.hotspot_edge_quantile = config_.hotspot_edge_quantile;
  selector_options.hotspot_component_boost = config_.hotspot_component_boost;
  selector_options.maximum_managed_threads = config_.maximum_managed_threads;
  selector_options.managed_thread_hysteresis_ratio =
      config_.managed_thread_hysteresis_ratio;
  auto active_demands = select_managed_threads(
      active_candidates, edges, active_cohort_, selector_options);
  std::set<int> next_active_cohort;
  for (const auto &demand : active_demands)
    next_active_cohort.insert(demand.identity.tid);
  std::vector<int> retained_managed;
  std::set_intersection(active_cohort_.begin(), active_cohort_.end(),
                        next_active_cohort.begin(), next_active_cohort.end(),
                        std::back_inserter(retained_managed));
  std::vector<int> combined_managed;
  std::set_union(active_cohort_.begin(), active_cohort_.end(),
                 next_active_cohort.begin(), next_active_cohort.end(),
                 std::back_inserter(combined_managed));
  const double managed_jaccard = combined_managed.empty()
                                      ? 1.0
                                      : static_cast<double>(retained_managed.size()) /
                                            combined_managed.size();
  std::map<int, ThreadDemand> candidate_map;
  for (const auto &demand : active_candidates)
    candidate_map[demand.identity.tid] = demand;
  double hotspot_weight = 0, internal_hotspot_weight = 0;
  double incident_hotspot_weight = 0;
  for (const auto &edge : select_hotspot_edges(edges, candidate_map,
                                                selector_options)) {
    hotspot_weight += edge.score;
    bool from = next_active_cohort.contains(edge.from_tid);
    bool to = next_active_cohort.contains(edge.to_tid);
    if (from && to) internal_hotspot_weight += edge.score;
    if (from || to) incident_hotspot_weight += edge.score;
  }
  const double internal_hotspot_coverage = hotspot_weight > 0
                                               ? internal_hotspot_weight /
                                                     hotspot_weight
                                               : 1.0;
  const double incident_hotspot_coverage = hotspot_weight > 0
                                               ? incident_hotspot_weight /
                                                     hotspot_weight
                                               : 1.0;
  std::set<int> cooling_threads;
  std::set_difference(active_cohort_.begin(), active_cohort_.end(),
                      next_active_cohort.begin(), next_active_cohort.end(),
                      std::inserter(cooling_threads, cooling_threads.end()));
  if (!cooling_threads.empty() && config_.mode == Mode::Active) {
    auto restored = actuator_.restore(cooling_threads);
    log("cohort_restore", "\"window_id\":\"" + window_id +
        "\",\"requested\":" + std::to_string(restored.requested) +
        ",\"restored\":" + std::to_string(restored.restored) +
        ",\"vanished\":" + std::to_string(restored.vanished) +
        ",\"failed\":" + std::to_string(restored.failed));
    if (!restored.success()) {
      fatal_error_ = "active cohort affinity restore failed";
      log("hard_failure", "\"component\":\"actuator\",\"message\":\"" +
          json_escape(fatal_error_) + "\"");
      kill(-root_pid_, SIGTERM);
      log("solve_window_end", "\"window_id\":\"" + window_id +
          "\",\"complete\":true,\"outcome\":\"cohort_restore_failed\"");
      return;
    }
  }
  for (int tid : cooling_threads) {
    solver_.remove_thread(tid);
    current_plan_.erase(tid);
  }
  active_cohort_ = std::move(next_active_cohort);
  if (active_cohort_.empty() && !cooling_threads.empty()) solver_.reset();
  demands = std::move(active_demands);
  log("active_cohort", "\"window_id\":\"" + window_id +
      "\",\"active_threads\":" + std::to_string(demands.size()) +
      ",\"demand_active_threads\":" +
          std::to_string(active_candidates.size()) +
      ",\"cooled_threads\":" + std::to_string(cooling_threads.size()) +
      ",\"retained_managed_threads\":" +
          std::to_string(retained_managed.size()) +
      ",\"managed_jaccard\":" + std::to_string(managed_jaccard) +
      ",\"managed_internal_hotspot_coverage\":" +
          std::to_string(internal_hotspot_coverage) +
      ",\"managed_incident_hotspot_coverage\":" +
          std::to_string(incident_hotspot_coverage) +
      ",\"enter_threshold\":" +
          std::to_string(config_.active_demand_threshold) +
      ",\"exit_threshold\":" +
          std::to_string(config_.inactive_demand_threshold));
  if (bpf_reader_ && (!bpf_health_.valid || !bpf_window_ready_ ||
                      bpf_window_loss_ratio_ >= 0.01)) {
    log("solve_window_end", "\"window_id\":\"" + window_id +
        "\",\"complete\":true,\"outcome\":\"waiting_bpf_health\"");
    return;
  }
  selector_ready_ = true;
  if (demands.empty()) {
    log("solve_window_end", "\"window_id\":\"" + window_id +
        "\",\"complete\":true,\"outcome\":\"policy_armed_quiescent\"");
    return;
  }
  IncrementalOptions options;
  options.maximum_migrated_threads_ratio =
      config_.maximum_migrated_threads_ratio;
  options.initial_migrated_threads_ratio =
      config_.initial_migrated_threads_ratio;
  options.proposal_confirmations = config_.proposal_confirmations;
  options.initial_proposal_confirmations =
      config_.initial_proposal_confirmations;
  options.minimum_dwell_ns =
      static_cast<uint64_t>(config_.minimum_dwell_seconds) * 1000000000ULL;
  options.initial_node_passes = config_.initial_node_passes;
  options.initial_node_thread_slack_ratio =
      config_.initial_node_thread_slack_ratio;
  options.candidate_multiplier = config_.candidate_multiplier;
  options.candidate_hard_limit = config_.candidate_hard_limit;
  options.rotating_scan_size = config_.rotating_scan_size;
  options.demand_dirty_threshold = config_.demand_dirty_threshold;
  options.edge_dirty_absolute_threshold =
      config_.edge_dirty_absolute_threshold;
  options.edge_dirty_relative_threshold =
      config_.edge_dirty_relative_threshold;
  options.minimum_relative_gain = config_.minimum_relative_gain;
  options.maximum_threads_per_cpu = config_.maximum_threads_per_cpu;
  options.thread_slot_slack = config_.thread_slot_slack;
  options.future_demand_floor = config_.future_demand_floor;
  options.group_peak_demand_ratio = config_.group_peak_demand_ratio;
  options.group_peak_demand_cap = config_.group_peak_demand_cap;
  options.group_peak_decay = config_.group_peak_decay;
  options.node_balance_threshold = config_.node_balance_threshold;
  options.hotspot_edges_per_thread = config_.hotspot_edges_per_thread;
  options.hotspot_edge_quantile = config_.hotspot_edge_quantile;
  options.hotspot_component_boost = config_.hotspot_component_boost;
  options.maximum_managed_threads = config_.maximum_managed_threads;
  options.managed_thread_hysteresis_ratio =
      config_.managed_thread_hysteresis_ratio;
  options.hotspot_replan_growth_ratio = config_.hotspot_replan_growth_ratio;
  options.hotspot_replan_min_threads = config_.hotspot_replan_min_threads;
  options.hotspot_stability_threshold = config_.hotspot_stability_threshold;
  options.hotspot_stability_confirmations = config_.hotspot_stability_confirmations;
  auto solve_started = std::chrono::steady_clock::now();
  SolverProposal proposal = solver_.propose(hardware_, demands, edges, options,
                                             now, &graph_delta);
  auto solve_duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - solve_started).count();
  log("plan", "\"window_id\":\"" + window_id +
      "\",\"strategy_id\":\"incremental-hotspot-v1\"" +
      ",\"phase\":\"" + incremental_phase_name(proposal.phase) + "\"" +
      ",\"eligible_threads\":" + std::to_string(proposal.eligible_threads) +
      ",\"pinned_threads\":" + std::to_string(proposal.pinned_threads) +
      ",\"dirty_threads\":" + std::to_string(proposal.dirty_threads) +
      ",\"candidate_threads\":" + std::to_string(proposal.candidate_threads) +
      ",\"cooldown_skipped_threads\":" +
          std::to_string(proposal.cooldown_skipped_threads) +
      ",\"migration_budget\":" + std::to_string(proposal.migration_budget) +
      ",\"cpu_slot_cap\":" + std::to_string(proposal.cpu_slot_cap) +
      ",\"maximum_cpu_threads\":" +
          std::to_string(proposal.maximum_cpu_threads) +
      ",\"predicted_demand_threads\":" +
          std::to_string(proposal.predicted_demand_threads) +
      ",\"relation_edges_input\":" +
          std::to_string(proposal.relation_edges_input) +
      ",\"hotspot_edges\":" + std::to_string(proposal.hotspot_edges) +
      ",\"hotspot_similarity\":" +
          std::to_string(proposal.hotspot_similarity) +
      ",\"hotspot_stability_confirmation\":" +
          std::to_string(proposal.hotspot_stability_confirmation) +
      ",\"hotspot_replan_triggered\":" +
          (proposal.hotspot_replan_triggered ? "true" : "false") +
      ",\"global_replan_active\":" +
          (proposal.global_replan_active ? "true" : "false") +
      ",\"solve_duration_ns\":" + std::to_string(solve_duration_ns) +
      ",\"confirmation\":" + std::to_string(proposal.confirmation) +
      ",\"initial_plan_confirmed\":" +
          (proposal.initial_plan_confirmed ? "true" : "false") +
      ",\"node_overload\":" + std::to_string(proposal.node_overload) +
      ",\"relation_cost\":" + std::to_string(proposal.relation_cost) +
      ",\"actions\":" + actions_json(proposal.actions));
  if (!proposal.ready) {
    log("solve_window_end", "\"window_id\":\"" + window_id +
        "\",\"complete\":true,\"outcome\":\"planned\"");
    return;
  }
  if (config_.mode == Mode::Plan) {
    solver_.commit(proposal, now);
    current_plan_ = solver_.placement();
    log("shadow_commit", "\"window_id\":\"" + window_id +
        "\",\"generation\":" + std::to_string(solver_.generation()) +
        ",\"actions\":" + actions_json(proposal.actions));
    log("solve_window_end", "\"window_id\":\"" + window_id +
        "\",\"complete\":true,\"outcome\":\"shadow_committed\"");
    return;
  }
  std::set<int> live;
  for (const auto &d : demands) live.insert(d.identity.tid);
  auto result = actuator_.apply(proposal.delta, live);
  action_requested_ += result.requested;
  action_committed_ += result.committed;
  action_vanished_ += result.vanished;
  action_rolled_back_ += result.rolled_back;
  if (!result.success) ++action_failures_;
  log("action", "\"window_id\":\"" + window_id +
      "\",\"success\":" + std::string(result.success ? "true" : "false") +
      ",\"requested\":" + std::to_string(result.requested) +
      ",\"applied\":" + std::to_string(result.applied) +
      ",\"committed\":" + std::to_string(result.committed) +
      ",\"vanished\":" + std::to_string(result.vanished) +
      ",\"rolled_back\":" + std::to_string(result.rolled_back) +
      ",\"rollback_success\":" + (result.rollback_success ? "true" : "false") +
      ",\"error\":" + std::to_string(result.error) +
      ",\"actions\":" + actions_json(proposal.actions));
  if (!result.success) {
    solver_.discard(proposal);
    log("solve_window_end", "\"window_id\":\"" + window_id +
        "\",\"complete\":true,\"outcome\":\"action_failed\"");
    return;
  }
  for (int tid : result.vanished_tids) solver_.remove_thread(tid);
  if (result.committed_tids.empty()) {
    solver_.discard(proposal);
  } else {
    solver_.commit(proposal, now, result.committed_tids);
  }
  current_plan_ = solver_.placement();
  log("action_commit", "\"window_id\":\"" + window_id +
      "\",\"generation\":" + std::to_string(solver_.generation()) +
      ",\"phase\":\"" + incremental_phase_name(solver_.phase()) + "\"");
  log("solve_window_end", "\"window_id\":\"" + window_id +
      "\",\"complete\":true,\"outcome\":\"action_committed\"");
}

void Runtime::trigger_fatal(const std::string &message) {
  {
    std::lock_guard lock(mutex_);
    if (!fatal_error_.empty()) return;
    fatal_error_ = message;
  }
  log("hard_failure", "\"component\":\"bpf\",\"message\":\"" +
      json_escape(message) + "\"");
  pause();
  kill(-root_pid_, SIGTERM);
}

void Runtime::sample_bpf_health(uint64_t now) {
  std::deque<PendingHealthSample> samples;
  uint64_t latest_sample_ns = 0;
  {
    std::lock_guard lock(bpf_pending_mutex_);
    samples.swap(pending_health_samples_);
    latest_sample_ns = last_bpf_health_sample_ns_;
  }
  if (samples.empty()) {
    if (latest_sample_ns && now - latest_sample_ns > 10000000000ULL)
      trigger_fatal("BPF health worker stopped sampling");
    return;
  }
  for (const auto &sample : samples) {
  now = sample.timestamp_ns;
  auto snapshot = sample.health;
  auto reader_stats = sample.reader_stats;
  if (!snapshot.valid) {
    int failures = 0;
    bool fatal = false;
    {
      std::lock_guard lock(mutex_);
      bpf_health_ = snapshot;
      failures = ++bpf_health_consecutive_failures_;
      if (!bpf_health_failed_since_ns_) bpf_health_failed_since_ns_ = now;
      fatal = failures >= 3 || now - bpf_health_failed_since_ns_ >= 5000000000ULL;
    }
    log("bpf_health", "\"valid\":false,\"error\":" +
        std::to_string(snapshot.error) + ",\"consecutive_failures\":" +
        std::to_string(failures));
    if (fatal) trigger_fatal("BPF health map unavailable");
    continue;
  }

  affinitygraph_bpf_health oldest{};
  bool ready = false;
  double loss = 0.0;
  bool fatal = false;
  {
    std::lock_guard lock(mutex_);
    bpf_health_ = snapshot;
    bpf_reader_stats_ = reader_stats;
    bpf_health_consecutive_failures_ = 0;
    bpf_health_failed_since_ns_ = 0;
    if (!bpf_health_history_.empty() &&
        (snapshot.counters.emitted < bpf_health_history_.back().second.emitted ||
         snapshot.counters.dropped < bpf_health_history_.back().second.dropped))
      bpf_health_history_.clear();
    bpf_health_history_.push_back({now, snapshot.counters});
    constexpr uint64_t window = 30000000000ULL;
    while (bpf_health_history_.size() > 1 &&
           now - bpf_health_history_[1].first >= window)
      bpf_health_history_.pop_front();
    ready = now - bpf_health_history_.front().first >= window;
    oldest = bpf_health_history_.front().second;
    uint64_t emitted = snapshot.counters.emitted - oldest.emitted;
    uint64_t dropped = snapshot.counters.dropped - oldest.dropped;
    uint64_t total = emitted + dropped;
    loss = total ? static_cast<double>(dropped) / total : 0.0;
    bpf_window_ready_ = ready;
    bpf_window_loss_ratio_ = loss;
    fatal = ready && loss >= 0.01;
  }
  log("bpf_health", "\"valid\":true,\"emitted\":" +
      std::to_string(snapshot.counters.emitted) + ",\"dropped\":" +
      std::to_string(snapshot.counters.dropped) + ",\"window_ready\":" +
      (ready ? "true" : "false") + ",\"window_emitted\":" +
      std::to_string(snapshot.counters.emitted - oldest.emitted) +
      ",\"window_dropped\":" +
      std::to_string(snapshot.counters.dropped - oldest.dropped) +
      ",\"window_loss_ratio\":" + std::to_string(loss) +
      ",\"emitted_by_kind\":" + bpf_kind_counts_json(snapshot.counters.emitted_by_kind) +
      ",\"dropped_by_kind\":" + bpf_kind_counts_json(snapshot.counters.dropped_by_kind) +
      ",\"suppressed_by_kind\":" + bpf_kind_counts_json(snapshot.counters.suppressed_by_kind) +
      ",\"ring_capacity_bytes\":" + std::to_string(reader_stats.capacity_bytes) +
      ",\"ring_occupancy_bytes\":" + std::to_string(reader_stats.occupancy_bytes) +
      ",\"ring_max_occupancy_bytes\":" + std::to_string(reader_stats.max_occupancy_bytes) +
      ",\"consumer_drain_calls\":" + std::to_string(reader_stats.drain_calls) +
      ",\"consumer_events\":" + std::to_string(reader_stats.events_consumed) +
      ",\"consumer_last_batch\":" + std::to_string(reader_stats.last_batch_events) +
      ",\"consumer_max_batch\":" + std::to_string(reader_stats.max_batch_events) +
      ",\"consumer_last_drain_ns\":" + std::to_string(reader_stats.last_drain_ns) +
      ",\"consumer_max_drain_ns\":" + std::to_string(reader_stats.max_drain_ns) +
      ",\"consumer_last_lag_ns\":" + std::to_string(reader_stats.last_max_lag_ns) +
      ",\"consumer_max_lag_ns\":" + std::to_string(reader_stats.max_lag_ns) +
      ",\"futex_aggregate_records\":" +
      std::to_string(reader_stats.futex_aggregate_records) +
      ",\"futex_handoffs\":" + std::to_string(reader_stats.futex_handoffs) +
      ",\"vfs_aggregate_records\":" +
      std::to_string(reader_stats.vfs_aggregate_records) +
      ",\"vfs_handoffs\":" + std::to_string(reader_stats.vfs_handoffs));
  if (fatal) trigger_fatal("30-second BPF loss ratio reached one percent");
  }
}

void Runtime::run_bpf() {
#ifdef __linux__
  pthread_setname_np(pthread_self(), "affgraph-bpf");
#endif
  uint64_t next_health = 0;
  auto sample_health = [&] {
    PendingHealthSample sample;
    sample.timestamp_ns = monotonic_ns();
    sample.health = bpf_reader_health(*bpf_reader_);
    sample.reader_stats = bpf_reader_stats(*bpf_reader_);
    std::lock_guard lock(bpf_pending_mutex_);
    constexpr size_t health_history_limit = 64;
    if (pending_health_samples_.size() >= health_history_limit)
      pending_health_samples_.pop_front();
    pending_health_samples_.push_back(sample);
    last_bpf_health_sample_ns_ = sample.timestamp_ns;
  };
  while (!stopping_) {
    uint64_t now = monotonic_ns();
    if (now >= next_health) {
      sample_health();
      next_health = now + 1000000000ULL;
    }
    drain_bpf_once();
    usleep(50000);
  }
  drain_bpf_once();
  sample_health();
}

void Runtime::run() {
#ifdef __linux__
  pthread_setname_np(pthread_self(), "affgraph");
#endif
  uint64_t next_sample = 0;
  uint64_t next_heap_trim = 0;
  while (!stopping_) {
    service_control_socket();
    uint64_t now = monotonic_ns();
    if (now >= next_sample) {
      consume_pending_bpf();
      if (bpf_reader_) sample_bpf_health(now);
      reconcile_and_sample();
      maybe_solve(now);
#ifdef __GLIBC__
      if (now >= next_heap_trim) {
        malloc_trim(0);
        next_heap_trim = now + 10000000000ULL;
      }
#endif
      next_sample = now + static_cast<uint64_t>(config_.sample_interval_seconds) * 1000000000ULL;
    }
    usleep(50000);
  }
}
} // namespace affinitygraph
