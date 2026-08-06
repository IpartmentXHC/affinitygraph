#include "affinitygraph/core.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <regex>

namespace affinitygraph {
namespace {
double normalized(double value, double scale) {
  return std::min(std::log1p(std::max(0.0, value)) / scale, 1.0);
}
double coefficient_of_variation(const std::vector<double> &values) {
  if (values.size() < 2) return 0;
  double mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
  if (mean <= 0) return 0;
  double variance = 0;
  for (double value : values) variance += (value - mean) * (value - mean);
  return std::sqrt(variance / (values.size() - 1)) / mean;
}
} // namespace

GraphWindow::GraphWindow(int horizon_seconds, Calibration calibration)
    : horizon_seconds_(horizon_seconds), calibration_(calibration) {}

void GraphWindow::observe_threads(const std::vector<ThreadSample> &samples) {
  uint64_t newest = 0;
  for (const auto &sample : samples) {
    newest = std::max(newest, sample.timestamp_ns);
    auto &history = threads_[sample.identity.tid];
    if (!history.empty() && history.back().identity.starttime != sample.identity.starttime) {
      history.clear();
      relations_.erase(std::remove_if(relations_.begin(), relations_.end(), [&](const auto &relation) {
        return relation.from_tid == sample.identity.tid || relation.to_tid == sample.identity.tid;
      }), relations_.end());
    }
    history.push_back(sample);
  }
  uint64_t cutoff = newest > static_cast<uint64_t>(horizon_seconds_) * 1000000000ULL
                        ? newest - static_cast<uint64_t>(horizon_seconds_) * 1000000000ULL : 0;
  for (auto it = threads_.begin(); it != threads_.end();) {
    while (!it->second.empty() && it->second.front().timestamp_ns < cutoff) it->second.pop_front();
    if (it->second.empty()) it = threads_.erase(it); else ++it;
  }
  while (!relations_.empty() && relations_.front().timestamp_ns < cutoff) relations_.pop_front();
}

void GraphWindow::observe_relation(const RelationObservation &observation) { relations_.push_back(observation); }

std::string GraphWindow::normalize_group(const std::string &comm, uintptr_t start_routine,
                                         int parent_tid, const std::string &start_symbol) {
  std::string normalized = std::regex_replace(comm, std::regex("([_-]?[0-9]+)+$"), "");
  if (normalized.empty()) normalized = "unnamed";
  // Symbol and lineage disambiguate identically named pools without depending on product names.
  std::string routine = start_symbol.empty() ? std::to_string(start_routine) : start_symbol;
  return normalized + "@" + routine + ":" + std::to_string(parent_tid);
}

std::vector<ThreadDemand> GraphWindow::demands() const {
  std::vector<ThreadDemand> result;
  for (const auto &[tid, history] : threads_) {
    if (history.empty()) continue;
    std::vector<double> pressure;
    for (size_t i = 1; i < history.size(); ++i) {
      const auto &a = history[i - 1], &b = history[i];
      if (b.timestamp_ns <= a.timestamp_ns || b.runtime_ns < a.runtime_ns || b.runqueue_ns < a.runqueue_ns) continue;
      pressure.push_back(std::clamp(static_cast<double>((b.runtime_ns - a.runtime_ns) + (b.runqueue_ns - a.runqueue_ns)) /
                                    static_cast<double>(b.timestamp_ns - a.timestamp_ns), 0.0, 1.0));
    }
    double ewma = 0;
    bool first = true;
    for (double value : pressure) {
      ewma = first ? value : 0.0645 * value + 0.9355 * ewma;
      first = false;
    }
    double coverage = std::min(1.0, static_cast<double>(pressure.size()) / std::max(1, horizon_seconds_ - 1));
    const auto &last = history.back();
    result.push_back({last.identity, normalize_group(last.comm, last.start_routine,
                                                     last.parent_tid, last.start_symbol),
                      ewma, coverage, last.recent_cpu});
  }
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.identity.tid < b.identity.tid; });
  return result;
}

std::vector<RelationEdge> GraphWindow::edges() const {
  std::map<std::pair<int, int>, std::vector<RelationObservation>> grouped;
  for (const auto &observation : relations_) {
    int a = std::min(observation.from_tid, observation.to_tid);
    int b = std::max(observation.from_tid, observation.to_tid);
    if (a != b) grouped[{a, b}].push_back(observation);
  }
  auto demand_values = demands();
  std::unordered_map<int, double> demand;
  for (const auto &item : demand_values) demand[item.identity.tid] = item.demand;
  std::vector<RelationEdge> result;
  for (const auto &[pair, observations] : grouped) {
    std::vector<double> totals;
    std::map<std::pair<int, int>, double> directional_sync;
    double share = 0, overlap = 0;
    for (const auto &o : observations) {
      directional_sync[{o.from_tid, o.to_tid}] += o.futex_per_second;
      share += o.shared_vfs_seconds;
      overlap += o.active_overlap;
      totals.push_back(o.futex_per_second + o.shared_vfs_seconds);
    }
    double sync = 0;
    for (const auto &[direction, value] : directional_sync) sync = std::max(sync, value / observations.size());
    share /= observations.size();
    overlap /= observations.size();
    double activity = normalized(std::sqrt(demand[pair.first] * demand[pair.second]), calibration_.activity_log_p95);
    double coverage = std::min(1.0, static_cast<double>(observations.size()) / horizon_seconds_);
    double stability = coverage / (1.0 + coefficient_of_variation(totals));
    double sync_n = normalized(sync, calibration_.sync_log_p95);
    double share_n = normalized(share, calibration_.share_log_p95) * std::clamp(overlap, 0.0, 1.0);
    result.push_back({pair.first, pair.second, activity, sync_n, share_n, stability,
                      100.0 * activity * stability * (0.7 * sync_n + 0.3 * share_n)});
  }
  return result;
}
} // namespace affinitygraph
