#include "affinitygraph/core.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <time.h>

namespace affinitygraph {
namespace fs = std::filesystem;

uint64_t monotonic_ns() {
  timespec ts{};
  // Match bpf_ktime_get_boot_ns so BPF and /proc windows share one clock.
  clock_gettime(CLOCK_BOOTTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

ProcCollector::ProcCollector(std::string proc_root) : proc_root_(std::move(proc_root)) {}

std::vector<ThreadSample> ProcCollector::sample(int tgid) const {
  std::vector<ThreadSample> result;
  fs::path task_root = fs::path(proc_root_) / std::to_string(tgid) / "task";
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(task_root, ec)) {
    int tid = -1;
    try { tid = std::stoi(entry.path().filename().string()); } catch (...) { continue; }
    std::ifstream stat_file(entry.path() / "stat");
    std::string stat;
    if (!std::getline(stat_file, stat)) continue;
    auto open = stat.find('('), close = stat.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close + 2 >= stat.size()) continue;
    ThreadSample sample;
    sample.identity.tgid = tgid;
    sample.identity.tid = tid;
    sample.comm = stat.substr(open + 1, close - open - 1);
    std::istringstream fields(stat.substr(close + 2));
    std::vector<std::string> values;
    std::string value;
    while (fields >> value) values.push_back(value);
    if (values.size() < 37) continue;
    sample.state = values[0][0];
    sample.identity.starttime = std::stoull(values[19]);
    sample.recent_cpu = std::stoi(values[36]);
    sample.timestamp_ns = monotonic_ns();

    std::ifstream schedstat(entry.path() / "schedstat");
    schedstat >> sample.runtime_ns >> sample.runqueue_ns;
    std::ifstream status(entry.path() / "status");
    std::string line;
    while (std::getline(status, line)) {
      auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      auto key = line.substr(0, colon), body = line.substr(colon + 1);
      if (key == "voluntary_ctxt_switches") sample.voluntary_switches = std::stoull(body);
      else if (key == "nonvoluntary_ctxt_switches") sample.involuntary_switches = std::stoull(body);
      else if (key == "Cpus_allowed_list") {
        try { sample.allowed_cpus = parse_cpu_list(body); } catch (...) {}
      }
    }
    result.push_back(std::move(sample));
  }
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.identity.tid < b.identity.tid; });
  return result;
}

std::map<int, uint64_t> ProcCollector::numa_pages(int tgid) const {
  std::ifstream input(fs::path(proc_root_) / std::to_string(tgid) / "numa_maps");
  std::map<int, uint64_t> result;
  std::string line, token;
  while (std::getline(input, line)) {
    std::istringstream fields(line);
    while (fields >> token) {
      if (token.size() < 4 || token[0] != 'N') continue;
      auto equal = token.find('=');
      if (equal == std::string::npos) continue;
      try { result[std::stoi(token.substr(1, equal - 1))] += std::stoull(token.substr(equal + 1)); }
      catch (...) {}
    }
  }
  return result;
}
} // namespace affinitygraph
