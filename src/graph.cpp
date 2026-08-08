#include "affinitygraph/core.hpp"

#include <algorithm>
#include <cmath>
#include <regex>

namespace affinitygraph {
namespace {
double normalized(double value, double scale) {
  return std::min(std::log1p(std::max(0.0, value)) / scale, 1.0);
}
std::pair<int, int> edge_key(int a, int b) {
  return {std::min(a, b), std::max(a, b)};
}
} // namespace

GraphWindow::GraphWindow(int horizon_seconds, Calibration calibration)
    : horizon_seconds_(horizon_seconds), calibration_(calibration) {}

void GraphWindow::observe_threads(const std::vector<ThreadSample> &samples) {
  demand_cache_valid_ = false;
  edge_cache_valid_ = false;
  uint64_t newest = 0;
  for (const auto &sample : samples) {
    newest = std::max(newest, sample.timestamp_ns);
    auto &history = threads_[sample.identity.tid];
    if (!history.empty() && history.back().identity.starttime != sample.identity.starttime) {
      history.clear();
      for (auto relation = relation_histories_.begin();
           relation != relation_histories_.end();) {
        if (relation->first.first == sample.identity.tid ||
            relation->first.second == sample.identity.tid) {
          relation_aggregates_.erase(relation->first);
          relation = relation_histories_.erase(relation);
        } else {
          ++relation;
        }
      }
    }
    history.push_back(sample);
  }
  uint64_t cutoff = newest > static_cast<uint64_t>(horizon_seconds_) * 1000000000ULL
                        ? newest - static_cast<uint64_t>(horizon_seconds_) * 1000000000ULL : 0;
  for (auto it = threads_.begin(); it != threads_.end();) {
    while (!it->second.empty() && it->second.front().timestamp_ns < cutoff) it->second.pop_front();
    if (it->second.empty()) it = threads_.erase(it); else ++it;
  }
  for (auto history = relation_histories_.begin();
       history != relation_histories_.end();) {
    auto &aggregate = relation_aggregates_.at(history->first);
    while (!history->second.empty() &&
           history->second.front().timestamp_ns < cutoff) {
      const auto &observation = history->second.front();
      double total = observation.futex_per_second + observation.shared_vfs_seconds;
      --aggregate.count;
      if (observation.from_tid == history->first.first)
        aggregate.forward_sync -= observation.futex_per_second;
      else
        aggregate.reverse_sync -= observation.futex_per_second;
      aggregate.handoff -= observation.futex_per_second;
      aggregate.share -= observation.shared_vfs_seconds;
      aggregate.overlap -= observation.active_overlap;
      aggregate.total -= total;
      aggregate.total_squared -= total * total;
      history->second.pop_front();
    }
    if (history->second.empty()) {
      relation_aggregates_.erase(history->first);
      history = relation_histories_.erase(history);
    } else {
      ++history;
    }
  }
}

void GraphWindow::observe_relation(const RelationObservation &observation) {
  auto key = edge_key(observation.from_tid, observation.to_tid);
  if (key.first == key.second) return;
  relation_histories_[key].push_back(observation);
  auto &aggregate = relation_aggregates_[key];
  double total = observation.futex_per_second + observation.shared_vfs_seconds;
  ++aggregate.count;
  if (observation.from_tid == key.first)
    aggregate.forward_sync += observation.futex_per_second;
  else
    aggregate.reverse_sync += observation.futex_per_second;
  aggregate.handoff += observation.futex_per_second;
  aggregate.share += observation.shared_vfs_seconds;
  aggregate.overlap += observation.active_overlap;
  aggregate.total += total;
  aggregate.total_squared += total * total;
  edge_cache_valid_ = false;
}

void GraphWindow::retain_relations(
    const std::set<std::pair<int, int>> &relations) {
  bool changed = false;
  for (auto it = relation_histories_.begin(); it != relation_histories_.end();) {
    if (!relations.contains(it->first)) {
      relation_aggregates_.erase(it->first);
      it = relation_histories_.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  if (changed) edge_cache_valid_ = false;
}

std::string GraphWindow::normalize_group(const std::string &comm, uintptr_t start_routine,
                                         int parent_tid, const std::string &start_symbol) {
  (void)parent_tid;
  // placement family 只由稳定名称和创建入口组成。末尾序号会随线程重建变化，
  // parent lineage 适合做身份追踪但不应拆分同一 worker pool，因此都不进入
  // family key。解析到符号时优先使用符号，避免 ASLR 地址跨进程变化。
  std::string normalized = std::regex_replace(comm, std::regex("([_-]?[0-9]+)+$"), "");
  if (normalized.empty()) normalized = "unnamed";
  std::string routine = start_symbol.empty() ? std::to_string(start_routine) : start_symbol;
  return normalized + "@" + routine;
}

std::vector<ThreadDemand> GraphWindow::demands() const {
  if (demand_cache_valid_) return demand_cache_;
  std::vector<ThreadDemand> result;
  for (const auto &[tid, history] : threads_) {
    if (history.empty()) continue;
    // demand 是窗口内 (runtime + runqueue) / wall-time 的 EWMA，范围 [0,1]。
    // coverage 只表示样本覆盖，不参与 NUMA-domain 的名称或关系判断。
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
  demand_cache_ = result;
  demand_cache_valid_ = true;
  return demand_cache_;
}

std::vector<ThreadWindowRecord> GraphWindow::thread_records() const {
  std::vector<ThreadWindowRecord> result;
  std::unordered_map<int, ThreadDemand> demand_by_tid;
  for (const auto &demand : demands()) demand_by_tid.emplace(demand.identity.tid, demand);
  for (const auto &[tid, history] : threads_) {
    if (history.empty()) continue;
    const auto &first = history.front();
    const auto &last = history.back();
    const auto demand = demand_by_tid.at(tid);
    ThreadWindowRecord record;
    record.identity = last.identity;
    record.comm = last.comm;
    record.parent_tid = last.parent_tid;
    record.start_routine = last.start_routine;
    record.start_symbol = last.start_symbol;
    record.group = demand.group;
    record.demand = demand.demand;
    record.confidence = demand.confidence;
    record.current_cpu = last.recent_cpu;
    record.allowed_cpus = last.allowed_cpus;
    record.state = last.state;
    record.sample_count = history.size();
    if (last.runtime_ns >= first.runtime_ns)
      record.runtime_delta_ns = last.runtime_ns - first.runtime_ns;
    if (last.runqueue_ns >= first.runqueue_ns)
      record.runqueue_delta_ns = last.runqueue_ns - first.runqueue_ns;
    if (last.voluntary_switches >= first.voluntary_switches)
      record.voluntary_switches_delta = last.voluntary_switches - first.voluntary_switches;
    if (last.involuntary_switches >= first.involuntary_switches)
      record.involuntary_switches_delta = last.involuntary_switches - first.involuntary_switches;
    result.push_back(std::move(record));
  }
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
    return a.identity.tid < b.identity.tid;
  });
  return result;
}

std::vector<RelationEdge> GraphWindow::edges() const {
  if (edge_cache_valid_) return edge_cache_;
  auto demand_values = demands();
  std::unordered_map<int, double> demand;
  for (const auto &item : demand_values) demand[item.identity.tid] = item.demand;
  std::vector<RelationEdge> result;
  for (const auto &[pair, aggregate] : relation_aggregates_) {
    if (!aggregate.count || !demand.contains(pair.first) ||
        !demand.contains(pair.second)) continue;
    double count = static_cast<double>(aggregate.count);
    // score 保留三类互补信号：双方活跃度、futex handoff、共享 VFS 活跃时间。
    // stability 同时惩罚覆盖不足和高变异，避免一次突发事件变成强关系边。
    double sync = std::max(aggregate.forward_sync, aggregate.reverse_sync) / count;
    double share = aggregate.share / count;
    double overlap = aggregate.overlap / count;
    double activity = normalized(std::sqrt(demand[pair.first] * demand[pair.second]), calibration_.activity_log_p95);
    double coverage = std::min(1.0, count / horizon_seconds_);
    double mean = aggregate.total / count;
    double variance = aggregate.count < 2 || mean <= 0 ? 0 :
        std::max(0.0, (aggregate.total_squared - count * mean * mean) /
                          (count - 1.0));
    double cv = mean > 0 ? std::sqrt(variance) / mean : 0;
    double stability = coverage / (1.0 + cv);
    double sync_n = normalized(sync, calibration_.sync_log_p95);
    double share_n = normalized(share, calibration_.share_log_p95) * std::clamp(overlap, 0.0, 1.0);
    result.push_back({pair.first, pair.second, activity, sync_n, share_n, stability,
                      100.0 * activity * stability * (0.7 * sync_n + 0.3 * share_n),
                      aggregate.handoff / count, share, overlap,
                      aggregate.count, coverage, cv});
  }
  edge_cache_ = result;
  edge_cache_valid_ = true;
  return edge_cache_;
}

GraphDelta GraphWindow::take_delta(double demand_threshold,
                                   double edge_absolute_threshold,
                                   double edge_relative_threshold) {
  GraphDelta delta;
  std::map<int, ThreadDemand> current_demands;
  for (const auto &thread : demands()) current_demands[thread.identity.tid] = thread;
  for (const auto &[tid, previous] : emitted_demands_) {
    auto current = current_demands.find(tid);
    if (current == current_demands.end() ||
        !(current->second.identity == previous.identity))
      delta.removed_threads.push_back(previous.identity);
  }
  for (const auto &identity : delta.removed_threads)
    emitted_demands_.erase(identity.tid);
  for (const auto &[tid, current] : current_demands) {
    auto previous = emitted_demands_.find(tid);
    if (previous == emitted_demands_.end() ||
        !(previous->second.identity == current.identity) ||
        std::abs(previous->second.demand - current.demand) >= demand_threshold ||
        previous->second.group != current.group) {
      delta.upsert_threads.push_back(current);
      emitted_demands_[tid] = current;
    }
  }

  std::map<std::pair<int, int>, RelationEdge> current_edges;
  for (const auto &edge : edges())
    current_edges[edge_key(edge.from_tid, edge.to_tid)] = edge;
  for (const auto &[key, _] : emitted_edges_)
    if (!current_edges.contains(key)) delta.removed_edges.push_back(key);
  for (const auto &[key, current] : current_edges) {
    auto previous = emitted_edges_.find(key);
    double old = previous == emitted_edges_.end() ? 0 : previous->second.score;
    double absolute = std::abs(current.score - old);
    double relative = absolute / std::max(std::abs(old), 1e-9);
    if (previous == emitted_edges_.end() ||
        absolute >= edge_absolute_threshold ||
        relative >= edge_relative_threshold) {
      delta.upsert_edges.push_back(current);
      emitted_edges_[key] = current;
    }
  }
  for (const auto &key : delta.removed_edges) emitted_edges_.erase(key);
  return delta;
}
} // namespace affinitygraph
