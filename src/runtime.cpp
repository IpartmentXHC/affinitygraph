#include "affinitygraph/runtime.hpp"
#include "affinitygraph/bpf_events.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <pthread.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

namespace affinitygraph {
std::shared_ptr<BpfRingReader> make_bpf_reader();
std::vector<RelationObservation> drain_bpf_reader(BpfRingReader &reader);
affinitygraph_bpf_health bpf_reader_health(BpfRingReader &reader);
namespace {
std::mutex instance_mutex;
Runtime *instance = nullptr;
thread_local bool inside_runtime = false;

std::string json_escape(const std::string &value) {
  std::ostringstream out;
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') out << '\\' << c;
    else if (c == '\n') out << "\\n";
    else if (c >= 0x20) out << c;
  }
  return out.str();
}

std::string signature(const Placement &placement) {
  std::ostringstream out;
  for (const auto &[tid, cpu] : placement.tid_to_cpu) out << tid << ':' << cpu << ';';
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

std::string bpf_kind_counts_json(const ag_u64 counts[5]) {
  return "{\"task_fork\":" + std::to_string(counts[AFFINITYGRAPH_TASK_FORK]) +
      ",\"task_exit\":" + std::to_string(counts[AFFINITYGRAPH_TASK_EXIT]) +
      ",\"futex\":" + std::to_string(counts[AFFINITYGRAPH_FUTEX]) +
      ",\"vfs\":" + std::to_string(counts[AFFINITYGRAPH_VFS]) + "}";
}

std::vector<int> intersect(std::vector<int> a, const std::vector<int> &b) {
  std::sort(a.begin(), a.end());
  std::vector<int> sorted_b = b;
  std::sort(sorted_b.begin(), sorted_b.end());
  std::vector<int> result;
  std::set_intersection(a.begin(), a.end(), sorted_b.begin(), sorted_b.end(), std::back_inserter(result));
  return result;
}
} // namespace

bool internal_call() { return inside_runtime; }
void set_internal_call(bool value) { inside_runtime = value; }
Runtime *runtime_instance() { return instance; }

Runtime::Runtime(Config config)
    : config_(std::move(config)), hardware_(HardwareGraph::discover(config_.cpus)),
      graph_(config_.graph_horizon_seconds, {config_.activity_log_p95, config_.sync_log_p95, config_.share_log_p95}), actuator_(backend_) {
  hardware_.load_calibration(config_.calibration_path);
  bpf_reader_ = make_bpf_reader();
  bool degraded = getenv("AFFINITYGRAPH_COLLECTOR_DISABLED") != nullptr;
  if (degraded && config_.mode == Mode::Active) config_.mode = Mode::Plan;
  std::filesystem::create_directories(config_.log_directory);
  std::string path = config_.log_directory + "/runtime.jsonl";
  log_fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  open_control_socket();
  inside_runtime = true;
  worker_ = std::thread([this] { run(); });
  inside_runtime = false;
  log("runtime_start", "\"mode\":\"" + std::string(config_.mode == Mode::Active ? "active" : config_.mode == Mode::Plan ? "plan" : "observe") +
      "\",\"relationship_calibration\":\"" + json_escape(config_.relationship_calibration_id) + "\",\"collector_degraded\":" +
      std::string(degraded ? "true" : "false"));
}

Runtime::~Runtime() {
  stopping_ = true;
  if (worker_.joinable()) worker_.join();
  inside_runtime = true;
  actuator_.restore_all();
  inside_runtime = false;
  log("runtime_stop");
  if (control_fd_ >= 0) close(control_fd_);
  unlink(config_.socket_path.c_str());
  if (log_fd_ >= 0) close(log_fd_);
}

void Runtime::open_control_socket() {
  control_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (control_fd_ < 0) return;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (config_.socket_path.size() >= sizeof(address.sun_path)) { close(control_fd_); control_fd_ = -1; return; }
  std::strncpy(address.sun_path, config_.socket_path.c_str(), sizeof(address.sun_path) - 1);
  unlink(address.sun_path);
  mode_t old = umask(0177);
  int rc = bind(control_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address));
  umask(old);
  if (rc != 0 || listen(control_fd_, 4) != 0) { close(control_fd_); control_fd_ = -1; return; }
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
  log("thread_start", "\"tid\":" + std::to_string(record.tid) + ",\"parent_tid\":" + std::to_string(record.parent_tid) +
      ",\"start_routine\":" + std::to_string(record.start_routine) + ",\"start_symbol\":\"" + json_escape(record.start_symbol) +
      "\",\"inherited_mask\":\"" + format_cpu_list(record.inherited_mask) + "\"");
}

void Runtime::thread_renamed(int tid, const std::string &name) {
  std::lock_guard lock(mutex_);
  if (auto it = lifecycle_.find(tid); it != lifecycle_.end()) {
    it->second.name = name;
    std::string group = GraphWindow::normalize_group(name, it->second.start_routine, it->second.parent_tid);
    if (config_.mode == Mode::Active && !paused_ && group_plan_.contains(group) && !group_plan_[group].empty()) {
      auto &cpus = group_plan_[group];
      int cpu = cpus[group_cursor_[group]++ % cpus.size()];
      int error = 0;
      inside_runtime = true;
      bool success = backend_.set(tid, {cpu}, error);
      inside_runtime = false;
      log("thread_inherit_plan", "\"tid\":" + std::to_string(tid) + ",\"cpu\":" + std::to_string(cpu) +
          ",\"success\":" + std::string(success ? "true" : "false") + ",\"error\":" + std::to_string(error));
    }
  }
  log("thread_name", "\"tid\":" + std::to_string(tid) + ",\"name\":\"" + json_escape(name) + "\"");
}

void Runtime::thread_exited(int tid, const char *reason) {
  std::lock_guard lock(mutex_);
  lifecycle_.erase(tid);
  current_plan_.erase(tid);
  log("thread_exit", "\"tid\":" + std::to_string(tid) + ",\"reason\":\"" + reason + "\"");
}

void Runtime::application_affinity(int tid, const std::vector<int> &cpus) {
  auto permitted = intersect(cpus, config_.cpus);
  std::lock_guard lock(mutex_);
  actuator_.note_application_mask(tid, permitted.empty() ? config_.cpus : permitted);
  log("application_affinity", "\"tid\":" + std::to_string(tid) + ",\"mask\":\"" + format_cpu_list(cpus) + "\"");
}

void Runtime::pause() {
  paused_ = true;
  std::lock_guard lock(mutex_);
  inside_runtime = true;
  bool restored = actuator_.restore_all();
  inside_runtime = false;
  current_plan_.clear();
  group_plan_.clear();
  proposal_count_ = 0;
  log("pause", "\"restored\":" + std::string(restored ? "true" : "false"));
}

void Runtime::resume() { paused_ = false; log("resume"); }

std::string Runtime::status_json() const {
  std::lock_guard lock(mutex_);
  std::ostringstream out;
  out << "{\"pid\":" << getpid() << ",\"mode\":\"" << (config_.mode == Mode::Active ? "active" : config_.mode == Mode::Plan ? "plan" : "observe")
      << "\",\"bpf\":" << (bpf_reader_ ? "true" : "false") << ",\"paused\":" << (paused_ ? "true" : "false")
      << ",\"runtime_tid\":" << runtime_tid_.load()
      << ",\"threads\":" << lifecycle_.size() << ",\"planned_threads\":" << current_plan_.size()
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
  command.erase(std::remove_if(command.begin(), command.end(), [](unsigned char c) { return std::isspace(c); }), command.end());
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
    auto samples = collector_.sample(getpid());
    samples.erase(std::remove_if(samples.begin(), samples.end(), [&](const auto &sample) {
      return sample.identity.tid == runtime_tid_.load();
    }), samples.end());
    if (samples.empty()) throw std::runtime_error("no task samples");
    graph_.observe_threads(samples);
    auto pages = collector_.numa_pages(getpid());
    std::ostringstream page_fields;
    page_fields << "\"scope\":\"process\",\"pages\":{";
    bool first_page = true;
    for (const auto &[node, count] : pages) {
      if (!first_page) page_fields << ',';
      first_page = false;
      page_fields << '"' << node << "\":" << count;
    }
    page_fields << '}';
    log("numa_maps", page_fields.str());
    if (bpf_reader_) {
      for (auto relation : drain_bpf_reader(*bpf_reader_)) {
        relation.futex_per_second /= config_.sample_interval_seconds;
        graph_.observe_relation(relation);
      }
      auto health = bpf_reader_health(*bpf_reader_);
      double loss = health.emitted + health.dropped == 0 ? 0.0 : static_cast<double>(health.dropped) / (health.emitted + health.dropped);
      log("bpf_health", "\"emitted\":" + std::to_string(health.emitted) + ",\"dropped\":" + std::to_string(health.dropped) +
          ",\"loss_ratio\":" + std::to_string(loss) + ",\"emitted_by_kind\":" + bpf_kind_counts_json(health.emitted_by_kind) +
          ",\"dropped_by_kind\":" + bpf_kind_counts_json(health.dropped_by_kind));
    }
    std::set<int> seen;
    for (const auto &sample : samples) seen.insert(sample.identity.tid);
    {
      std::lock_guard lock(mutex_);
      for (const auto &sample : samples) if (!lifecycle_.contains(sample.identity.tid))
        lifecycle_[sample.identity.tid] = {getpid(), sample.identity.tid, 0, monotonic_ns(), {}, sample.comm, sample.allowed_cpus};
      for (auto it = lifecycle_.begin(); it != lifecycle_.end();)
        if (!seen.contains(it->first)) it = lifecycle_.erase(it); else ++it;
    }
    collector_failed_since_ns_ = 0;
  } catch (const std::exception &error) {
    if (!collector_failed_since_ns_) collector_failed_since_ns_ = monotonic_ns();
    log("collector_failure", "\"message\":\"" + json_escape(error.what()) + "\"");
    uint64_t delay = static_cast<uint64_t>(config_.collector_failure_restore_seconds) * 1000000000ULL;
    if (monotonic_ns() - collector_failed_since_ns_ >= delay && !paused_) pause();
  }
}

void Runtime::maybe_solve(uint64_t now) {
  uint64_t solve_interval = static_cast<uint64_t>(config_.solve_interval_seconds) * 1000000000ULL;
  if (now - last_solve_ns_ < solve_interval || paused_) return;
  last_solve_ns_ = now;
  auto demands = graph_.demands();
  for (const auto &d : demands)
    log("thread_window", "\"tid\":" + std::to_string(d.identity.tid) + ",\"starttime\":" + std::to_string(d.identity.starttime) +
        ",\"demand\":" + std::to_string(d.demand) + ",\"confidence\":" + std::to_string(d.confidence) +
        ",\"current_cpu\":" + std::to_string(d.current_cpu) + ",\"group\":\"" + json_escape(d.group) + "\"");
  auto edges = graph_.edges();
  std::unordered_map<int, std::string> tid_group;
  for (const auto &d : demands) tid_group[d.identity.tid] = d.group;
  std::map<std::pair<std::string, std::string>, double> group_scores;
  for (const auto &edge : edges)
  {
    std::string from_group = tid_group[edge.from_tid], to_group = tid_group[edge.to_tid];
    if (to_group < from_group) std::swap(from_group, to_group);
    group_scores[{from_group, to_group}] += edge.score;
    log("relation_edge", "\"scope\":\"" + std::string(from_group == to_group ? "self" : "pair") +
        "\",\"from_tid\":" + std::to_string(edge.from_tid) + ",\"to_tid\":" + std::to_string(edge.to_tid) +
        ",\"activity\":" + std::to_string(edge.activity) + ",\"sync\":" + std::to_string(edge.sync) +
        ",\"share\":" + std::to_string(edge.share) + ",\"stability\":" + std::to_string(edge.stability) +
        ",\"score\":" + std::to_string(edge.score));
  }
  for (const auto &[groups, score] : group_scores)
    log("group_relation", "\"kind\":\"" + std::string(groups.first == groups.second ? "R_self" : "R_pair") +
        "\",\"from_group\":\"" + json_escape(groups.first) + "\",\"to_group\":\"" + json_escape(groups.second) +
        "\",\"score\":" + std::to_string(score));
  if (config_.mode == Mode::Observe) return;
  demands.erase(std::remove_if(demands.begin(), demands.end(), [&](const auto &d) { return d.confidence < config_.minimum_confidence; }), demands.end());
  if (demands.empty()) return;
  Placement proposal = Solver().solve(hardware_, demands, edges, {config_.maximum_migrated_active_threads_ratio, 0.05});
  std::string next = signature(proposal);
  if (next == proposal_signature_) ++proposal_count_;
  else { proposal_signature_ = next; proposal_count_ = 1; }
  log("plan", "\"threads\":" + std::to_string(proposal.tid_to_cpu.size()) + ",\"overload\":" + std::to_string(proposal.overload) +
      ",\"relation_cost\":" + std::to_string(proposal.relation_cost) + ",\"migration_cost\":" + std::to_string(proposal.migration_cost) +
      ",\"confirmation\":" + std::to_string(proposal_count_) + ",\"assignments\":" + assignments_json(proposal.tid_to_cpu));
  if (config_.mode != Mode::Active || proposal_count_ < config_.proposal_confirmations) return;
  uint64_t dwell = static_cast<uint64_t>(config_.minimum_dwell_seconds) * 1000000000ULL;
  if (last_action_ns_ && now - last_action_ns_ < dwell) return;
  std::set<int> live;
  for (const auto &d : demands) live.insert(d.identity.tid);
  std::lock_guard lock(mutex_);
  inside_runtime = true;
  auto result = actuator_.apply(proposal, live);
  inside_runtime = false;
  log("action", "\"success\":" + std::string(result.success ? "true" : "false") + ",\"applied\":" + std::to_string(result.applied) +
      ",\"vanished\":" + std::to_string(result.vanished) + ",\"error\":" + std::to_string(result.error) +
      ",\"rolled_back\":" + std::string(!result.success && result.applied ? "true" : "false") +
      ",\"assignments\":" + assignments_json(proposal.tid_to_cpu));
  if (result.success) {
    current_plan_ = proposal.tid_to_cpu;
    group_plan_.clear();
    for (const auto &d : demands) {
      auto life = lifecycle_.find(d.identity.tid);
      std::string group = life == lifecycle_.end() ? d.group : GraphWindow::normalize_group(
          life->second.name.empty() ? d.group : life->second.name, life->second.start_routine, life->second.parent_tid);
      group_plan_[group].push_back(proposal.tid_to_cpu[d.identity.tid]);
    }
    for (auto &[group, cpus] : group_plan_) {
      std::sort(cpus.begin(), cpus.end());
      cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
    }
    last_action_ns_ = now;
  }
}

void Runtime::run() {
  inside_runtime = true;
  runtime_tid_ = static_cast<int>(syscall(SYS_gettid));
  pthread_setname_np(pthread_self(), "affgraph");
  uint64_t next_sample = 0;
  while (!stopping_) {
    service_control_socket();
    uint64_t now = monotonic_ns();
    if (now >= next_sample) {
      reconcile_and_sample();
      maybe_solve(now);
      next_sample = now + static_cast<uint64_t>(config_.sample_interval_seconds) * 1000000000ULL;
    }
    usleep(50000);
  }
  inside_runtime = false;
}

void runtime_initialize_from_environment() {
  const char *path = getenv("AFFINITYGRAPH_CONFIG");
  if (!path || !*path) return;
  std::lock_guard lock(instance_mutex);
  if (instance) return;
  try { instance = new Runtime(load_config(path)); }
  catch (const std::exception &error) {
    const char prefix[] = "libaffinitygraph: disabled: ";
    [[maybe_unused]] ssize_t first = write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
    [[maybe_unused]] ssize_t second = write(STDERR_FILENO, error.what(), std::strlen(error.what()));
    [[maybe_unused]] ssize_t third = write(STDERR_FILENO, "\n", 1);
  }
}

void runtime_shutdown() {
  std::lock_guard lock(instance_mutex);
  delete instance;
  instance = nullptr;
}
} // namespace affinitygraph
