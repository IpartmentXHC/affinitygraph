#include "affinitygraph/runtime.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <elf.h>
#include <fcntl.h>
#include <fstream>
#include <signal.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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
    static std::unordered_map<std::string, std::string> cache;
    std::string cache_key = path + ':' + std::to_string(relative);
    if (auto cached = cache.find(cache_key); cached != cache.end()) return cached->second;
    auto remember = [&](std::string value) {
      cache.emplace(cache_key, value);
      return value;
    };
    std::string disk_path = path;
    constexpr const char deleted[] = " (deleted)";
    if (disk_path.ends_with(deleted)) disk_path.resize(disk_path.size() - sizeof(deleted) + 1);
    std::ifstream elf(disk_path, std::ios::binary);
    Elf64_Ehdr header{};
    if (elf.read(reinterpret_cast<char *>(&header), sizeof(header)) &&
        std::memcmp(header.e_ident, ELFMAG, SELFMAG) == 0 &&
        header.e_ident[EI_CLASS] == ELFCLASS64 && header.e_shentsize == sizeof(Elf64_Shdr)) {
      std::vector<Elf64_Shdr> sections(header.e_shnum);
      elf.seekg(static_cast<std::streamoff>(header.e_shoff));
      if (elf.read(reinterpret_cast<char *>(sections.data()),
                   static_cast<std::streamsize>(sections.size() * sizeof(Elf64_Shdr)))) {
        uint64_t best_value = 0;
        std::string best_name;
        uintptr_t lookup_address = header.e_type == ET_EXEC ? address : relative;
        for (const auto &section : sections) {
          if ((section.sh_type != SHT_DYNSYM && section.sh_type != SHT_SYMTAB) ||
              !section.sh_entsize || section.sh_link >= sections.size()) continue;
          const auto &strings_section = sections[section.sh_link];
          if (section.sh_size > 64 * 1024 * 1024 || strings_section.sh_size > 64 * 1024 * 1024) continue;
          std::vector<Elf64_Sym> symbols(section.sh_size / sizeof(Elf64_Sym));
          std::vector<char> strings(strings_section.sh_size);
          elf.clear();
          elf.seekg(static_cast<std::streamoff>(section.sh_offset));
          if (!elf.read(reinterpret_cast<char *>(symbols.data()),
                        static_cast<std::streamsize>(symbols.size() * sizeof(Elf64_Sym)))) continue;
          elf.clear();
          elf.seekg(static_cast<std::streamoff>(strings_section.sh_offset));
          if (!elf.read(strings.data(), static_cast<std::streamsize>(strings.size()))) continue;
          for (const auto &symbol : symbols) {
            if (ELF64_ST_TYPE(symbol.st_info) != STT_FUNC || symbol.st_name >= strings.size() ||
                symbol.st_shndx == SHN_UNDEF || !symbol.st_name ||
                symbol.st_value > lookup_address || symbol.st_value < best_value) continue;
            if (symbol.st_size && lookup_address >= symbol.st_value + symbol.st_size) continue;
            best_value = symbol.st_value;
            best_name = strings.data() + symbol.st_name;
          }
        }
        if (!best_name.empty()) {
          std::ostringstream value;
          value << path << ':' << best_name << "+0x" << std::hex
                << (lookup_address - best_value);
          return remember(value.str());
        }
      }
    }
    return remember(path + "+0x" + [&] {
      std::ostringstream value;
      value << std::hex << relative;
      return value.str();
    }());
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
      bpf_reader_(std::move(bpf_reader)), actuator_(backend_) {
  hardware_.load_calibration(config_.calibration_path);
  target_tgids_.insert(root_pid_);
  if (collector_degraded_) config_.mode = Mode::Observe;
  std::filesystem::create_directories(config_.log_directory);
  std::string path = config_.log_directory + "/runtime.jsonl";
  log_fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  open_control_socket();
  worker_ = std::thread([this] { run(); });
  log("runtime_start", "\"root_pid\":" + std::to_string(root_pid_) +
      ",\"effective_mode\":\"" +
      std::string(config_.mode == Mode::Active ? "active" : config_.mode == Mode::Plan ? "plan" : "observe") +
      "\",\"collector_degraded\":" + (collector_degraded_ ? "true" : "false") +
      ",\"affinity_capability\":" + (affinity_capability_ ? "true" : "false") +
      ",\"pthread_uprobe\":\"" + json_escape(uprobe_status_) + "\"");
}

Runtime::~Runtime() {
  stopping_ = true;
  if (worker_.joinable()) worker_.join();
  consume_bpf_events();
  consume_bpf_futex_aggregates();
  if (bpf_reader_) {
    auto snapshot = bpf_reader_health(*bpf_reader_);
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

void Runtime::inherit_group_plan(int tid) {
  auto life = lifecycle_.find(tid);
  if (life == lifecycle_.end() || config_.mode != Mode::Active || paused_ ||
      current_plan_.contains(tid)) return;
  std::string group = GraphWindow::normalize_group(life->second.name,
                                                    life->second.start_routine,
                                                    life->second.parent_tid,
                                                    life->second.start_symbol);
  auto plan = group_plan_.find(group);
  if (plan == group_plan_.end() || plan->second.empty()) return;
  int cpu = plan->second[group_cursor_[group]++ % plan->second.size()];
  int error = 0;
  bool success = backend_.set(tid, {cpu}, error);
  if (success) {
    actuator_.note_policy_action(tid);
    current_plan_[tid] = cpu;
  }
  ++action_requested_;
  if (success) ++action_committed_;
  else if (error == ESRCH) ++action_vanished_;
  else ++action_failures_;
  log("thread_inherit_plan", "\"tid\":" + std::to_string(tid) +
      ",\"cpu\":" + std::to_string(cpu) + ",\"success\":" +
      (success ? "true" : "false") + ",\"error\":" + std::to_string(error));
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
  inherit_group_plan(record.tid);
}

void Runtime::thread_renamed(int tid, const std::string &name) {
  std::lock_guard lock(mutex_);
  if (auto it = lifecycle_.find(tid); it != lifecycle_.end()) {
    it->second.name = name;
    inherit_group_plan(tid);
  }
  log("thread_name", "\"tid\":" + std::to_string(tid) +
      ",\"name\":\"" + json_escape(name) + "\"");
}

void Runtime::thread_exited(int tid, const char *reason) {
  std::lock_guard lock(mutex_);
  lifecycle_.erase(tid);
  pending_application_masks_.erase(tid);
  current_plan_.erase(tid);
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
    if (event.kind == AFFINITYGRAPH_FUTEX)
      observation.futex_per_second =
          static_cast<double>(event.value_ns ? event.value_ns : 1) /
          config_.sample_interval_seconds;
    else observation.shared_vfs_seconds = static_cast<double>(event.value_ns) / 1e9;
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

void Runtime::consume_bpf_events() {
  if (!bpf_reader_) return;
  for (const auto &event : drain_bpf_events(*bpf_reader_)) handle_bpf_event(event);
}

void Runtime::consume_bpf_futex_aggregates() {
  if (!bpf_reader_) return;
  for (const auto &event : drain_bpf_futex_aggregates(*bpf_reader_))
    handle_bpf_event(event);
}

void Runtime::pause() {
  paused_ = true;
  std::lock_guard lock(mutex_);
  last_restore_ = actuator_.restore_all();
  current_plan_.clear();
  group_plan_.clear();
  proposal_cohort_.clear();
  proposal_count_ = 0;
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
      << ",\"collector_degraded\":" << (collector_degraded_ ? "true" : "false")
      << ",\"affinity_capability\":" << (affinity_capability_ ? "true" : "false")
      << ",\"pthread_uprobe\":\"" << json_escape(uprobe_status_) << "\""
      << ",\"paused\":" << (paused_ ? "true" : "false")
      << ",\"threads\":" << lifecycle_.size()
      << ",\"planned_threads\":" << current_plan_.size()
      << ",\"planned_assignments\":" << assignments_json(current_plan_)
      << ",\"active_effective\":"
      << (config_.mode == Mode::Active && !paused_ && !current_plan_.empty() ? "true" : "false")
      << ",\"action_requested\":" << action_requested_
      << ",\"action_committed\":" << action_committed_
      << ",\"action_vanished\":" << action_vanished_
      << ",\"action_failures\":" << action_failures_
      << ",\"action_rolled_back\":" << action_rolled_back_
      << ",\"restore_requested\":" << last_restore_.requested
      << ",\"restore_restored\":" << last_restore_.restored
      << ",\"restore_vanished\":" << last_restore_.vanished
      << ",\"restore_failed\":" << last_restore_.failed
      << ",\"fatal_error\":\"" << json_escape(fatal_error_) << '"'
      << ",\"proposal_confirmations\":" << proposal_count_ << '}';
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
          it = lifecycle_.erase(it);
        } else ++it;
      }
      graph_.observe_threads(samples);
    }
    if (bpf_reader_) sample_bpf_health(monotonic_ns());
    collector_failed_since_ns_ = 0;
  } catch (const std::exception &error) {
    if (!collector_failed_since_ns_) collector_failed_since_ns_ = monotonic_ns();
    log("collector_failure", "\"message\":\"" + json_escape(error.what()) + "\"");
    uint64_t delay = static_cast<uint64_t>(config_.collector_failure_restore_seconds) * 1000000000ULL;
    if (monotonic_ns() - collector_failed_since_ns_ >= delay && !paused_) pause();
  }
}

void Runtime::maybe_solve(uint64_t now) {
  uint64_t interval = static_cast<uint64_t>(config_.solve_interval_seconds) * 1000000000ULL;
  if (now - last_solve_ns_ < interval || paused_) return;
  last_solve_ns_ = now;
  std::lock_guard lock(mutex_);
  auto demands = graph_.demands();
  for (const auto &d : demands)
    log("thread_window", "\"tid\":" + std::to_string(d.identity.tid) +
        ",\"starttime\":" + std::to_string(d.identity.starttime) +
        ",\"demand\":" + std::to_string(d.demand) +
        ",\"confidence\":" + std::to_string(d.confidence) +
        ",\"current_cpu\":" + std::to_string(d.current_cpu) +
        ",\"group\":\"" + json_escape(d.group) + "\"");
  auto edges = graph_.edges();
  for (const auto &edge : edges)
    log("relation_edge", "\"from_tid\":" + std::to_string(edge.from_tid) +
        ",\"to_tid\":" + std::to_string(edge.to_tid) +
        ",\"activity\":" + std::to_string(edge.activity) +
        ",\"sync\":" + std::to_string(edge.sync) +
        ",\"share\":" + std::to_string(edge.share) +
        ",\"stability\":" + std::to_string(edge.stability) +
        ",\"score\":" + std::to_string(edge.score));
  if (config_.mode == Mode::Observe) return;
  demands.erase(std::remove_if(demands.begin(), demands.end(), [&](const auto &d) {
    return d.confidence < config_.minimum_confidence;
  }), demands.end());
  if (demands.empty()) return;
  constexpr double active_threshold = 0.05;
  Placement proposal = Solver().solve(hardware_, demands, edges,
      {config_.maximum_migrated_active_threads_ratio, active_threshold});
  ActiveCohort next = active_cohort(demands, active_threshold);
  size_t confirmation_threads = std::count_if(
      demands.begin(), demands.end(), [](const auto &d) { return d.demand >= active_threshold; });
  if (next.empty()) {
    proposal_cohort_.clear();
    proposal_count_ = 0;
  } else if (active_cohort_continues(proposal_cohort_, next)) {
    ++proposal_count_;
  } else {
    proposal_count_ = 1;
  }
  proposal_cohort_ = std::move(next);
  log("plan", "\"threads\":" + std::to_string(proposal.tid_to_cpu.size()) +
      ",\"confirmation_threads\":" + std::to_string(confirmation_threads) +
      ",\"confirmation_basis\":\"stable_active_cohort\"" +
      ",\"overload\":" + std::to_string(proposal.overload) +
      ",\"relation_cost\":" + std::to_string(proposal.relation_cost) +
      ",\"migration_cost\":" + std::to_string(proposal.migration_cost) +
      ",\"confirmation\":" + std::to_string(proposal_count_) +
      ",\"assignments\":" + assignments_json(proposal.tid_to_cpu));
  if (config_.mode != Mode::Active || proposal_count_ < config_.proposal_confirmations) return;
  uint64_t dwell = static_cast<uint64_t>(config_.minimum_dwell_seconds) * 1000000000ULL;
  if (last_action_ns_ && now - last_action_ns_ < dwell) return;
  std::set<int> live;
  for (const auto &d : demands) live.insert(d.identity.tid);
  auto result = actuator_.apply(proposal, live);
  action_requested_ += result.requested;
  action_committed_ += result.committed;
  action_vanished_ += result.vanished;
  action_rolled_back_ += result.rolled_back;
  if (!result.success) ++action_failures_;
  log("action", "\"success\":" + std::string(result.success ? "true" : "false") +
      ",\"requested\":" + std::to_string(result.requested) +
      ",\"applied\":" + std::to_string(result.applied) +
      ",\"committed\":" + std::to_string(result.committed) +
      ",\"vanished\":" + std::to_string(result.vanished) +
      ",\"rolled_back\":" + std::to_string(result.rolled_back) +
      ",\"rollback_success\":" + (result.rollback_success ? "true" : "false") +
      ",\"error\":" + std::to_string(result.error) +
      ",\"assignments\":" + assignments_json(proposal.tid_to_cpu));
  if (!result.success) return;
  current_plan_ = proposal.tid_to_cpu;
  group_plan_.clear();
  for (const auto &d : demands) group_plan_[d.group].push_back(proposal.tid_to_cpu[d.identity.tid]);
  for (auto &[group, cpus] : group_plan_) {
    std::sort(cpus.begin(), cpus.end());
    cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
  }
  last_action_ns_ = now;
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
  auto snapshot = bpf_reader_health(*bpf_reader_);
  auto reader_stats = bpf_reader_stats(*bpf_reader_);
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
    return;
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
      ",\"futex_handoffs\":" + std::to_string(reader_stats.futex_handoffs));
  if (fatal) trigger_fatal("30-second BPF loss ratio reached one percent");
}

void Runtime::run() {
#ifdef __linux__
  pthread_setname_np(pthread_self(), "affgraph");
#endif
  uint64_t next_sample = 0;
  while (!stopping_) {
    service_control_socket();
    consume_bpf_events();
    uint64_t now = monotonic_ns();
    if (now >= next_sample) {
      consume_bpf_futex_aggregates();
      reconcile_and_sample();
      maybe_solve(now);
      next_sample = now + static_cast<uint64_t>(config_.sample_interval_seconds) * 1000000000ULL;
    }
    usleep(50000);
  }
}
} // namespace affinitygraph
