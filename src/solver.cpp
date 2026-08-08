#include "affinitygraph/core.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace affinitygraph {

ActiveCohort active_cohort(const std::vector<ThreadDemand> &threads,
                           double active_threshold) {
  ActiveCohort active;
  for (const auto &thread : threads)
    if (thread.demand >= active_threshold)
      active.emplace(thread.identity.tgid, thread.identity.tid,
                     thread.identity.starttime);
  return active;
}

bool active_cohort_continues(const ActiveCohort &previous,
                             const ActiveCohort &current,
                             double maximum_growth_ratio) {
  if (previous == current) return !current.empty();
  if (previous.size() < 20 || current.size() <= previous.size() ||
      !std::includes(current.begin(), current.end(),
                     previous.begin(), previous.end())) return false;
  size_t additions = current.size() - previous.size();
  size_t allowance = static_cast<size_t>(
      std::ceil(previous.size() * maximum_growth_ratio));
  return additions <= allowance;
}

Placement Solver::solve(const HardwareGraph &hardware,
                        const std::vector<ThreadDemand> &threads,
                        const std::vector<RelationEdge> &edges,
                        const SolveOptions &options) const {
  Placement placement;
  if (threads.empty()) return placement;
  auto nodes = hardware.nodes();
  if (nodes.empty()) throw std::runtime_error("empty hardware graph");
  std::unordered_map<int, ThreadDemand> by_tid;
  for (const auto &thread : threads) by_tid[thread.identity.tid] = thread;
  std::unordered_map<int, std::vector<RelationEdge>> adjacency;
  for (const auto &edge : edges) {
    adjacency[edge.from_tid].push_back(edge);
    adjacency[edge.to_tid].push_back(edge);
  }

  const bool legacy = options.strategy_id == "legacy-v1";
  std::vector<ThreadDemand> ordered = threads;
  if (!legacy && options.demand_floor > 0)
    for (auto &thread : ordered)
      thread.demand = std::max(thread.demand, options.demand_floor);
  const int dynamic_cpu_cap = static_cast<int>(std::ceil(
      static_cast<double>(threads.size()) / hardware.cpus.size())) + options.slot_slack;
  const bool slot_constraints = !legacy && options.max_threads_per_cpu >= 0;
  const int cpu_cap = options.max_threads_per_cpu > 0
                          ? options.max_threads_per_cpu
                          : dynamic_cpu_cap;
  std::sort(ordered.begin(), ordered.end(), [&](const auto &a, const auto &b) {
    double ar = 0, br = 0;
    for (const auto &e : adjacency[a.identity.tid]) ar += e.score;
    for (const auto &e : adjacency[b.identity.tid]) br += e.score;
    if (a.demand != b.demand) return a.demand > b.demand;
    if (ar != br) return ar > br;
    return a.identity.tid < b.identity.tid;
  });

  std::map<int, double> node_load;
  std::map<int, int> node_count;
  std::map<int, int> tid_node;
  std::map<std::pair<int, int>, double> node_latency_cache;
  for (int from : nodes) for (int to : nodes) {
    double best = std::numeric_limits<double>::infinity();
    for (int a : hardware.cpus_in_node(from)) for (int b : hardware.cpus_in_node(to))
      best = std::min(best, hardware.latency(a, b));
    node_latency_cache[{from, to}] = best;
  }
  auto node_latency = [&](int from, int to) {
    return node_latency_cache.at({from, to});
  };
  for (const auto &thread : ordered) {
    int best_node = nodes.front();
    auto best = std::tuple<double, double, double, int>{std::numeric_limits<double>::infinity(), 0, 0, best_node};
    for (int node : nodes) {
      double capacity = hardware.cpus_in_node(node).size();
      double overload = std::max(0.0, node_load[node] + thread.demand - capacity);
      if (slot_constraints && node_count[node] >= cpu_cap * static_cast<int>(capacity))
        overload += 1e12;
      double relation = 0;
      for (const auto &edge : adjacency[thread.identity.tid]) {
        int other = edge.from_tid == thread.identity.tid ? edge.to_tid : edge.from_tid;
        if (auto it = tid_node.find(other); it != tid_node.end())
          relation += edge.score * node_latency(node, it->second);
      }
      double migration = 0;
      for (const auto &cpu : hardware.cpus) if (cpu.id == thread.current_cpu && cpu.node != node) migration = thread.demand;
      auto candidate = std::tuple{overload, relation, migration, node};
      if (candidate < best) { best = candidate; best_node = node; }
    }
    tid_node[thread.identity.tid] = best_node;
    node_load[best_node] += thread.demand;
    ++node_count[best_node];
  }

  // Deterministic FM-style single-vertex refinement after the greedy seed.
  for (int pass = 0; pass < options.fm_passes; ++pass) {
    bool changed = false;
    for (const auto &thread : ordered) {
      int original = tid_node[thread.identity.tid];
      auto objective = [&](int candidate) {
        double overload = 0;
        for (int node : nodes) {
          double load = node_load[node] - (node == original ? thread.demand : 0) + (node == candidate ? thread.demand : 0);
          overload += std::max(0.0, load - static_cast<double>(hardware.cpus_in_node(node).size()));
        }
        double relation = 0;
        double slot_violation = 0;
        if (slot_constraints) {
          int count = node_count[candidate] - (candidate == original ? 1 : 0) + 1;
          slot_violation = count > cpu_cap * static_cast<int>(hardware.cpus_in_node(candidate).size())
                               ? 1e12
                               : 0;
        }
        for (const auto &edge : adjacency[thread.identity.tid]) {
          int other = edge.from_tid == thread.identity.tid ? edge.to_tid : edge.from_tid;
          if (auto it = tid_node.find(other); it != tid_node.end()) relation += edge.score * node_latency(candidate, it->second);
        }
        double migration = 0;
        for (const auto &cpu : hardware.cpus) if (cpu.id == thread.current_cpu)
          migration = thread.demand * node_latency(cpu.node, candidate);
        return std::tuple{overload + slot_violation, relation, migration, candidate};
      };
      int best_node = original;
      auto best = objective(original);
      for (int node : nodes) if (objective(node) < best) { best = objective(node); best_node = node; }
      if (best_node != original) {
        node_load[original] -= thread.demand;
        node_load[best_node] += thread.demand;
        --node_count[original];
        ++node_count[best_node];
        tid_node[thread.identity.tid] = best_node;
        changed = true;
      }
    }
    if (!changed) break;
  }

  std::map<int, double> cpu_load;
  std::map<int, int> cpu_count;
  std::map<std::pair<int, std::string>, int> cpu_group_count;
  for (const auto &thread : ordered) {
    auto cpus = hardware.cpus_in_node(tid_node[thread.identity.tid]);
    if (cpus.empty()) throw std::runtime_error("node has no CPU in envelope");
    int best_cpu = cpus.front();
    auto best = std::tuple<double, double, double, int>{
        std::numeric_limits<double>::infinity(), 0, 0, best_cpu};
    for (int cpu : cpus) {
      double relation = 0;
      for (const auto &edge : adjacency[thread.identity.tid]) {
        int other = edge.from_tid == thread.identity.tid ? edge.to_tid : edge.from_tid;
        if (auto it = placement.tid_to_cpu.find(other); it != placement.tid_to_cpu.end())
          relation += edge.score * hardware.latency(cpu, it->second) +
                      (!legacy && cpu == it->second
                           ? edge.score * options.same_cpu_contention_penalty
                           : 0);
      }
      double count_cost = 0;
      if (!legacy) {
        if (slot_constraints && cpu_count[cpu] >= cpu_cap) count_cost += 1e12;
        count_cost += options.count_penalty * std::pow(cpu_count[cpu] + 1.0, 2.0);
        if (thread.demand < options.active_threshold)
          count_cost += options.low_demand_spread * cpu_count[cpu];
        count_cost += options.group_spread_penalty * cpu_group_count[{cpu, thread.group}];
      }
      double primary = cpu_load[cpu] + thread.demand;
      if (!legacy && options.thread_count_tie_break)
        primary += static_cast<double>(cpu_count[cpu]) * 1e-9;
      auto candidate = std::tuple{primary, relation, count_cost, cpu};
      if (candidate < best) { best = candidate; best_cpu = cpu; }
    }
    placement.tid_to_cpu[thread.identity.tid] = best_cpu;
    cpu_load[best_cpu] += thread.demand;
    ++cpu_count[best_cpu];
    ++cpu_group_count[{best_cpu, thread.group}];
  }

  // Optional deterministic CPU-local LPT refinement for reviewed candidates.
  for (int pass = 0; !legacy && pass < options.lpt_refinement_passes; ++pass) {
    bool changed = false;
    for (const auto &thread : ordered) {
      int original = placement.tid_to_cpu.at(thread.identity.tid);
      auto candidates = hardware.cpus_in_node(tid_node.at(thread.identity.tid));
      auto objective = [&](int cpu) {
        double load = cpu_load[cpu] + (cpu == original ? 0.0 : thread.demand);
        int count = cpu_count[cpu] + (cpu == original ? 0 : 1);
        double relation = 0;
        for (const auto &edge : adjacency[thread.identity.tid]) {
          int other = edge.from_tid == thread.identity.tid ? edge.to_tid : edge.from_tid;
          if (auto it = placement.tid_to_cpu.find(other); it != placement.tid_to_cpu.end())
            relation += edge.score * hardware.latency(cpu, it->second) +
                        (cpu == it->second ? edge.score * options.same_cpu_contention_penalty : 0);
        }
        double penalties = slot_constraints && count > cpu_cap ? 1e12 : 0;
        penalties += options.count_penalty * std::pow(static_cast<double>(count), 2.0);
        if (thread.demand < options.active_threshold)
          penalties += options.low_demand_spread * count;
        int same_group = cpu_group_count[{cpu, thread.group}] - (cpu == original ? 1 : 0);
        penalties += options.group_spread_penalty * same_group;
        return std::tuple{load, relation, penalties, cpu};
      };
      int best_cpu = original;
      auto best = objective(original);
      for (int cpu : candidates)
        if (objective(cpu) < best) { best = objective(cpu); best_cpu = cpu; }
      if (best_cpu != original) {
        cpu_load[original] -= thread.demand;
        cpu_load[best_cpu] += thread.demand;
        --cpu_count[original];
        ++cpu_count[best_cpu];
        --cpu_group_count[{original, thread.group}];
        ++cpu_group_count[{best_cpu, thread.group}];
        placement.tid_to_cpu[thread.identity.tid] = best_cpu;
        changed = true;
      }
    }
    if (!changed) break;
  }

  std::vector<const ThreadDemand *> migrations;
  for (const auto &thread : threads)
    if (thread.demand >= options.active_threshold && placement.tid_to_cpu[thread.identity.tid] != thread.current_cpu)
      migrations.push_back(&thread);
  size_t active = std::count_if(threads.begin(), threads.end(), [&](const auto &t) { return t.demand >= options.active_threshold; });
  size_t budget = static_cast<size_t>(std::floor(active * options.maximum_migrated_active_threads_ratio));
  std::sort(migrations.begin(), migrations.end(), [](auto a, auto b) { return a->demand < b->demand; });
  while (migrations.size() > budget) {
    auto *thread = migrations.front();
    migrations.erase(migrations.begin());
    int assigned = placement.tid_to_cpu[thread->identity.tid];
    bool current_eligible = std::any_of(hardware.cpus.begin(), hardware.cpus.end(),
        [&](const auto &c) { return c.id == thread->current_cpu; });
    bool slot_available = !slot_constraints || cpu_count[thread->current_cpu] < cpu_cap ||
                          assigned == thread->current_cpu;
    if (current_eligible && slot_available) {
      if (!legacy && assigned != thread->current_cpu) {
        --cpu_count[assigned];
        ++cpu_count[thread->current_cpu];
      }
      placement.tid_to_cpu[thread->identity.tid] = thread->current_cpu;
    }
  }

  cpu_load.clear();
  for (const auto &thread : threads) cpu_load[placement.tid_to_cpu[thread.identity.tid]] += thread.demand;
  for (const auto &[cpu, load] : cpu_load) placement.overload += std::max(0.0, load - 1.0);
  for (const auto &edge : edges) {
    auto a = placement.tid_to_cpu.find(edge.from_tid), b = placement.tid_to_cpu.find(edge.to_tid);
    if (a != placement.tid_to_cpu.end() && b != placement.tid_to_cpu.end()) {
      placement.relation_cost += edge.score * hardware.latency(a->second, b->second);
      if (a->second == b->second) placement.same_cpu_edge_weight += edge.score;
    }
  }
  for (const auto &thread : threads)
    if (placement.tid_to_cpu[thread.identity.tid] != thread.current_cpu)
      placement.migration_cost += thread.demand * hardware.latency(thread.current_cpu, placement.tid_to_cpu[thread.identity.tid]);
  return placement;
}

const char *incremental_phase_name(IncrementalPhase phase) {
  switch (phase) {
  case IncrementalPhase::Uninitialized: return "uninitialized";
  case IncrementalPhase::NodePlanning: return "node_planning";
  case IncrementalPhase::InitialPinning: return "initial_pinning";
  case IncrementalPhase::Optimizing: return "optimizing";
  }
  return "unknown";
}

namespace {
int cpu_node(const HardwareGraph &hardware, int cpu) {
  for (const auto &item : hardware.cpus)
    if (item.id == cpu && item.online) return item.node;
  return -1;
}

double node_latency(const HardwareGraph &hardware, int from, int to) {
  if (auto it = hardware.node_distance.find({from, to});
      it != hardware.node_distance.end()) return it->second;
  double best = std::numeric_limits<double>::infinity();
  for (int a : hardware.cpus_in_node(from))
    for (int b : hardware.cpus_in_node(to))
      best = std::min(best, hardware.latency(a, b));
  return std::isfinite(best) ? best : 1e12;
}

std::pair<int, int> edge_key(int a, int b) {
  return {std::min(a, b), std::max(a, b)};
}

} // namespace

std::vector<RelationEdge> select_hotspot_edges(
    const std::vector<RelationEdge> &edges,
    const std::map<int, ThreadDemand> &threads,
    const IncrementalOptions &options) {
  struct Candidate {
    RelationEdge edge;
    double weight = 0;
    std::pair<int, int> key;
  };
  std::vector<Candidate> candidates;
  for (const auto &edge : edges) {
    if (edge.from_tid == edge.to_tid || edge.score <= 0 ||
        !threads.contains(edge.from_tid) || !threads.contains(edge.to_tid))
      continue;
    double component = std::max(edge.sync, edge.share);
    candidates.push_back({edge,
        edge.score * (1.0 + options.hotspot_component_boost * component),
        edge_key(edge.from_tid, edge.to_tid)});
  }
  if (candidates.empty()) return {};

  std::vector<double> weights;
  weights.reserve(candidates.size());
  for (const auto &candidate : candidates) weights.push_back(candidate.weight);
  std::sort(weights.begin(), weights.end());
  size_t quantile_index = static_cast<size_t>(std::floor(
      options.hotspot_edge_quantile * static_cast<double>(weights.size() - 1)));
  double threshold = weights[quantile_index];

  std::map<int, std::vector<const Candidate *>> incident;
  for (const auto &candidate : candidates) {
    incident[candidate.edge.from_tid].push_back(&candidate);
    incident[candidate.edge.to_tid].push_back(&candidate);
  }
  std::set<std::pair<int, int>> selected;
  for (auto &[_, values] : incident) {
    std::sort(values.begin(), values.end(), [](const auto *a, const auto *b) {
      if (a->weight != b->weight) return a->weight > b->weight;
      return a->key < b->key;
    });
    values.resize(std::min(values.size(),
                           static_cast<size_t>(options.hotspot_edges_per_thread)));
    for (const auto *candidate : values) selected.insert(candidate->key);
  }

  std::vector<RelationEdge> result;
  for (auto &candidate : candidates) {
    if (candidate.weight + 1e-12 < threshold &&
        !selected.contains(candidate.key))
      continue;
    candidate.edge.score = candidate.weight;
    result.push_back(candidate.edge);
  }
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
    return edge_key(a.from_tid, a.to_tid) < edge_key(b.from_tid, b.to_tid);
  });
  return result;
}

std::vector<ThreadDemand> select_managed_threads(
    const std::vector<ThreadDemand> &threads,
    const std::vector<RelationEdge> &edges,
    const std::set<int> &previous_managed,
    const IncrementalOptions &options) {
  if (options.maximum_managed_threads <= 0 ||
      threads.size() <= static_cast<size_t>(options.maximum_managed_threads))
    return threads;
  std::map<int, ThreadDemand> by_tid;
  for (const auto &thread : threads) by_tid[thread.identity.tid] = thread;
  std::map<int, double> degree;
  std::map<int, std::vector<std::pair<int, double>>> adjacency;
  auto hotspots = select_hotspot_edges(edges, by_tid, options);
  for (const auto &edge : hotspots) {
    degree[edge.from_tid] += edge.score;
    degree[edge.to_tid] += edge.score;
    adjacency[edge.from_tid].push_back({edge.to_tid, edge.score});
    adjacency[edge.to_tid].push_back({edge.from_tid, edge.score});
  }
  std::vector<int> ranked;
  for (const auto &[tid, _] : by_tid) ranked.push_back(tid);
  std::sort(ranked.begin(), ranked.end(), [&](int a, int b) {
    if (degree[a] != degree[b]) return degree[a] > degree[b];
    if (by_tid[a].demand != by_tid[b].demand)
      return by_tid[a].demand > by_tid[b].demand;
    return by_tid[a].identity < by_tid[b].identity;
  });
  const size_t limit = std::min(
      ranked.size(), static_cast<size_t>(options.maximum_managed_threads));
  const size_t retention_limit = std::min(ranked.size(), static_cast<size_t>(
      std::ceil(limit * (1.0 + options.managed_thread_hysteresis_ratio))));
  std::set<int> selected;
  for (size_t index = 0; index < retention_limit && selected.size() < limit;
       ++index)
    if (previous_managed.contains(ranked[index])) selected.insert(ranked[index]);

  if (selected.empty() && limit >= 2 && !hotspots.empty()) {
    const auto *seed = &hotspots.front();
    for (const auto &edge : hotspots)
      if (std::tuple{edge.score, -std::min(edge.from_tid, edge.to_tid),
                     -std::max(edge.from_tid, edge.to_tid)} >
          std::tuple{seed->score, -std::min(seed->from_tid, seed->to_tid),
                     -std::max(seed->from_tid, seed->to_tid)})
        seed = &edge;
    selected.insert(seed->from_tid);
    selected.insert(seed->to_tid);
  }

  // Grow around retained hotspot communities. Degree-only truncation captures
  // many incident edges but cuts their strongest counterparts at the cohort
  // boundary, leaving the solver without the relationships it is meant to
  // optimize.
  while (selected.size() < limit) {
    int best_tid = -1;
    auto best = std::tuple{-1.0, size_t{0}, -1.0, -1.0,
                           ThreadIdentity{}};
    for (int tid : ranked) {
      if (selected.contains(tid)) continue;
      double internal_gain = 0;
      size_t internal_neighbors = 0;
      for (const auto &[other, weight] : adjacency[tid]) {
        if (!selected.contains(other)) continue;
        internal_gain += weight;
        ++internal_neighbors;
      }
      auto candidate = std::tuple{internal_gain, internal_neighbors, degree[tid],
                                  by_tid[tid].demand, by_tid[tid].identity};
      if (best_tid < 0 || candidate > best) {
        best_tid = tid;
        best = candidate;
      }
    }
    if (best_tid < 0) break;
    selected.insert(best_tid);
  }

  // A bounded 1-for-1 refinement removes weak boundary vertices when an
  // excluded endpoint would add materially more internal hotspot evidence.
  for (size_t pass = 0; pass < limit; ++pass) {
    int best_out = -1, best_in = -1;
    double best_gain = 0;
    for (int out : selected) {
      // Hysteresis admission already removes retained vertices that fell
      // outside the configured rank band. Do not churn the survivors during
      // the same window's locality refinement.
      if (previous_managed.contains(out)) continue;
      double loss = 0;
      for (const auto &[other, weight] : adjacency[out])
        if (selected.contains(other)) loss += weight;
      for (int in : ranked) {
        if (selected.contains(in)) continue;
        double gain = 0;
        for (const auto &[other, weight] : adjacency[in])
          if (other != out && selected.contains(other)) gain += weight;
        double improvement = gain - loss;
        double required = 0.0;
        if (improvement <= required + 1e-12) continue;
        if (best_in < 0 ||
            std::tuple{improvement, degree[in], by_tid[in].demand,
                       by_tid[in].identity, by_tid[out].identity} >
                std::tuple{best_gain, degree[best_in], by_tid[best_in].demand,
                           by_tid[best_in].identity, by_tid[best_out].identity}) {
          best_out = out;
          best_in = in;
          best_gain = improvement;
        }
      }
    }
    if (best_in < 0) break;
    selected.erase(best_out);
    selected.insert(best_in);
  }
  std::vector<ThreadDemand> result;
  for (const auto &thread : threads)
    if (selected.contains(thread.identity.tid)) result.push_back(thread);
  return result;
}

namespace {

std::string action_key(const std::vector<PlacementAction> &actions) {
  std::ostringstream out;
  for (const auto &action : actions)
    out << action.identity.tgid << ':' << action.identity.tid << ':'
        << action.identity.starttime << ':' << action.from_cpu << '>'
        << action.target_cpu << ';';
  return out.str();
}
} // namespace

void IncrementalSolver::reset() {
  phase_ = IncrementalPhase::Uninitialized;
  generation_ = 0;
  next_proposal_id_ = 1;
  scan_cursor_ = 0;
  last_node_plan_population_ = 0;
  replan_existing_threads_ = false;
  stable_hotspot_replan_pending_ = false;
  hotspot_stability_confirmation_ = 0;
  previous_hotspot_keys_.clear();
  initial_confirmation_ = 0;
  identities_.clear();
  threads_.clear();
  edges_.clear();
  previous_demand_.clear();
  group_peak_demand_.clear();
  previous_edge_score_.clear();
  baseline_cpu_.clear();
  initial_target_node_.clear();
  initial_target_cpu_.clear();
  previous_initial_target_node_.clear();
  initial_candidate_identities_.clear();
  placement_.clear();
  pinned_.clear();
  last_moved_ns_.clear();
  action_confirmations_.clear();
  outstanding_.reset();
}

void IncrementalSolver::remove_thread(int tid) {
  identities_.erase(tid);
  threads_.erase(tid);
  previous_demand_.erase(tid);
  baseline_cpu_.erase(tid);
  initial_target_node_.erase(tid);
  initial_target_cpu_.erase(tid);
  previous_initial_target_node_.erase(tid);
  initial_candidate_identities_.erase(tid);
  placement_.erase(tid);
  pinned_.erase(tid);
  last_moved_ns_.erase(tid);
  for (auto it = edges_.begin(); it != edges_.end();) {
    if (it->first.first == tid || it->first.second == tid) it = edges_.erase(it);
    else ++it;
  }
}

bool IncrementalSolver::effective() const {
  return phase_ == IncrementalPhase::Optimizing && !threads_.empty() &&
         pinned_.size() == threads_.size();
}

SolverProposal IncrementalSolver::propose(
    const HardwareGraph &hardware, const std::vector<ThreadDemand> &threads,
    const std::vector<RelationEdge> &edges, const IncrementalOptions &options,
    uint64_t now_ns, const GraphDelta *delta) {
  if (outstanding_) return *outstanding_;

  const size_t online_cpus = std::count_if(
      hardware.cpus.begin(), hardware.cpus.end(),
      [](const Cpu &cpu) { return cpu.online; });
  if (online_cpus == 0) throw std::runtime_error("empty CPU envelope");
  const int dynamic_slot_cap = static_cast<int>(std::ceil(
      static_cast<double>(threads.size()) / online_cpus)) +
      options.thread_slot_slack;
  const int cpu_slot_cap = options.maximum_threads_per_cpu > 0
                               ? options.maximum_threads_per_cpu
                               : std::max(1, dynamic_slot_cap);

  std::map<std::string, double> current_group_peak;
  std::set<std::string> live_groups;
  for (const auto &thread : threads) {
    live_groups.insert(thread.group);
    current_group_peak[thread.group] =
        std::max(current_group_peak[thread.group], thread.demand);
  }
  for (auto it = group_peak_demand_.begin();
       it != group_peak_demand_.end();) {
    if (!live_groups.contains(it->first)) it = group_peak_demand_.erase(it);
    else ++it;
  }
  for (const auto &group : live_groups)
    group_peak_demand_[group] = std::max(
        current_group_peak[group],
        group_peak_demand_[group] * options.group_peak_decay);

  std::map<int, double> effective_demand;
  size_t predicted_demand_threads = 0;
  for (const auto &thread : threads) {
    double group_reserve = std::min(
        options.group_peak_demand_cap,
        group_peak_demand_[thread.group] * options.group_peak_demand_ratio);
    double predicted = std::max(
        {thread.demand, options.future_demand_floor, group_reserve});
    effective_demand[thread.identity.tid] = std::min(1.0, predicted);
    if (effective_demand[thread.identity.tid] > thread.demand + 1e-12)
      ++predicted_demand_threads;
  }
  std::map<int, ThreadDemand> incoming_threads;
  for (const auto &thread : threads) {
    incoming_threads[thread.identity.tid] = thread;
    incoming_threads[thread.identity.tid].demand =
        effective_demand[thread.identity.tid];
  }
  auto incoming_hotspots = select_hotspot_edges(edges, incoming_threads, options);
  std::map<int, std::vector<RelationEdge>> incoming_adjacency;
  for (const auto &edge : incoming_hotspots) {
    incoming_adjacency[edge.from_tid].push_back(edge);
    incoming_adjacency[edge.to_tid].push_back(edge);
  }

  std::set<int> live;
  for (const auto &thread : threads) live.insert(thread.identity.tid);
  std::vector<int> stale;
  for (const auto &[tid, _] : threads_)
    if (!live.contains(tid)) stale.push_back(tid);
  for (int tid : stale) remove_thread(tid);

  bool added_after_initial = false;
  size_t added_threads = 0;
  for (const auto &thread : threads) {
    int tid = thread.identity.tid;
    auto identity = identities_.find(tid);
    if (identity != identities_.end() && !(identity->second == thread.identity))
      remove_thread(tid);
    if (!identities_.contains(tid)) {
      ++added_threads;
      identities_[tid] = thread.identity;
      baseline_cpu_[tid] = thread.current_cpu;
      if (phase_ == IncrementalPhase::InitialPinning ||
          phase_ == IncrementalPhase::Optimizing) {
        std::map<int, int> group_nodes;
        std::map<int, double> existing_cpu_load;
        std::map<int, int> existing_cpu_count;
        std::map<int, double> existing_node_load;
        std::map<int, int> existing_node_count;
        for (const auto &[other_tid, other] : threads_) {
          int cpu = -1;
          if (placement_.contains(other_tid)) cpu = placement_[other_tid];
          else if (initial_target_cpu_.contains(other_tid))
            cpu = initial_target_cpu_[other_tid];
          if (cpu >= 0) {
            existing_cpu_load[cpu] += other.demand;
            ++existing_cpu_count[cpu];
            int node = cpu_node(hardware, cpu);
            existing_node_load[node] += other.demand;
            ++existing_node_count[node];
            if (other.group == thread.group) ++group_nodes[node];
          }
        }
        int sampled_node = cpu_node(hardware, thread.current_cpu);
        int target_node = hardware.nodes().front();
        auto node_objective = [&](int node) {
          int cpus = static_cast<int>(hardware.cpus_in_node(node).size());
          int count_cap = static_cast<int>(std::ceil(
              static_cast<double>(threads.size()) * cpus / online_cpus *
              (1.0 + options.initial_node_thread_slack_ratio)));
          double demand = effective_demand[tid];
          double relation = 0;
          for (const auto &edge : incoming_adjacency[tid]) {
            int other = edge.from_tid == tid ? edge.to_tid : edge.from_tid;
            int other_cpu = placement_.contains(other) ? placement_[other] :
                            initial_target_cpu_.contains(other)
                                ? initial_target_cpu_[other] : -1;
            if (other_cpu >= 0)
              relation += edge.score * node_latency(
                  hardware, node, cpu_node(hardware, other_cpu));
          }
          return std::tuple{
              std::max(0, existing_node_count[node] + 1 - count_cap),
              std::max(0.0, existing_node_load[node] + demand - cpus),
              relation, (existing_node_load[node] + demand) / cpus,
              -group_nodes[node], node != sampled_node, node};
        };
        auto best_node_objective = node_objective(target_node);
        for (int node : hardware.nodes())
          if (node_objective(node) < best_node_objective) {
            target_node = node;
            best_node_objective = node_objective(node);
          }
        auto candidates = hardware.cpus_in_node(target_node);
        if (candidates.empty()) candidates = hardware.cpus_in_node(hardware.nodes().front());
        int target_cpu = *std::min_element(candidates.begin(), candidates.end(),
            [&](int a, int b) {
              double demand = effective_demand[tid];
              return std::tuple{existing_cpu_count[a] >= cpu_slot_cap,
                                std::max(0.0, existing_cpu_load[a] + demand - 1.0),
                                existing_cpu_load[a] + demand,
                                existing_cpu_count[a], a} <
                     std::tuple{existing_cpu_count[b] >= cpu_slot_cap,
                                std::max(0.0, existing_cpu_load[b] + demand - 1.0),
                                existing_cpu_load[b] + demand,
                                existing_cpu_count[b], b};
            });
        initial_target_node_[tid] = cpu_node(hardware, target_cpu);
        initial_target_cpu_[tid] = target_cpu;
        added_after_initial = true;
      }
    }
    threads_[tid] = thread;
    threads_[tid].demand = effective_demand[tid];
  }
  bool substantial_growth = last_node_plan_population_ > 0 &&
      added_threads >= static_cast<size_t>(options.hotspot_replan_min_threads) &&
      threads_.size() >= static_cast<size_t>(std::ceil(
          last_node_plan_population_ * (1.0 + options.hotspot_replan_growth_ratio)));
  if (added_after_initial && substantial_growth) {
    phase_ = IncrementalPhase::NodePlanning;
    replan_existing_threads_ = true;
    initial_confirmation_ = 0;
    previous_initial_target_node_.clear();
    initial_candidate_identities_.clear();
    // This call already starts a global node replan for the enlarged cohort.
    // Arming the stability trigger as well would repeat the same full replan a
    // few windows later and cause a second mass pinning batch.
    stable_hotspot_replan_pending_ = false;
    hotspot_stability_confirmation_ = 0;
  } else if (added_after_initial) {
    phase_ = IncrementalPhase::InitialPinning;
    // Do not cancel a confirmed global node replan merely because the
    // managed cohort gained a few members before its pinning batch committed.
    // The new members receive incremental targets above; existing members
    // must still converge to the node plan already calculated for them.
  }
  auto initial_targets_effective = [&] {
    if (pinned_.size() != threads_.size()) return false;
    if (!replan_existing_threads_) return true;
    for (const auto &[tid, _] : threads_)
      if (!initial_target_cpu_.contains(tid) || !placement_.contains(tid) ||
          placement_[tid] != initial_target_cpu_[tid])
        return false;
    return true;
  };
  if (phase_ == IncrementalPhase::InitialPinning && initial_targets_effective())
    phase_ = IncrementalPhase::Optimizing;

  auto selected_edges = select_hotspot_edges(edges, threads_, options);
  edges_.clear();
  std::set<std::pair<int, int>> hotspot_keys;
  for (const auto &edge : selected_edges) {
    auto key = edge_key(edge.from_tid, edge.to_tid);
    edges_[key] = edge;
    hotspot_keys.insert(key);
  }
  double hotspot_similarity = 0;
  if (!hotspot_keys.empty() && !previous_hotspot_keys_.empty()) {
    std::vector<std::pair<int, int>> intersection;
    std::set_intersection(hotspot_keys.begin(), hotspot_keys.end(),
                          previous_hotspot_keys_.begin(),
                          previous_hotspot_keys_.end(),
                          std::back_inserter(intersection));
    std::vector<std::pair<int, int>> combined;
    std::set_union(hotspot_keys.begin(), hotspot_keys.end(),
                   previous_hotspot_keys_.begin(),
                   previous_hotspot_keys_.end(),
                   std::back_inserter(combined));
    hotspot_similarity = combined.empty() ? 0 :
        static_cast<double>(intersection.size()) / combined.size();
  }
  bool hotspot_replan_triggered = false;
  if (stable_hotspot_replan_pending_) {
    if (hotspot_similarity >= options.hotspot_stability_threshold)
      ++hotspot_stability_confirmation_;
    else
      hotspot_stability_confirmation_ = 0;
    if (hotspot_stability_confirmation_ >=
            options.hotspot_stability_confirmations) {
      phase_ = IncrementalPhase::NodePlanning;
      replan_existing_threads_ = true;
      stable_hotspot_replan_pending_ = false;
      initial_confirmation_ = 0;
      previous_initial_target_node_.clear();
      initial_candidate_identities_.clear();
      hotspot_replan_triggered = true;
    }
  }
  previous_hotspot_keys_ = std::move(hotspot_keys);

  SolverProposal proposal;
  proposal.id = next_proposal_id_++;
  proposal.shadow = shadow_;
  proposal.eligible_threads = threads_.size();
  proposal.pinned_threads = pinned_.size();
  const double migration_ratio = phase_ == IncrementalPhase::InitialPinning
                                     ? options.initial_migrated_threads_ratio
                                     : options.maximum_migrated_threads_ratio;
  proposal.migration_budget = static_cast<size_t>(std::floor(
      threads_.size() * migration_ratio));
  proposal.cpu_slot_cap = cpu_slot_cap;
  proposal.predicted_demand_threads = predicted_demand_threads;
  proposal.relation_edges_input = edges.size();
  proposal.hotspot_edges = edges_.size();
  proposal.hotspot_similarity = hotspot_similarity;
  proposal.hotspot_stability_confirmation =
      hotspot_stability_confirmation_;
  proposal.hotspot_replan_triggered = hotspot_replan_triggered;
  proposal.global_replan_active = replan_existing_threads_;

  if (threads_.empty()) {
    phase_ = IncrementalPhase::Uninitialized;
    proposal.phase = phase_;
    return proposal;
  }
  if (phase_ == IncrementalPhase::Uninitialized)
    phase_ = IncrementalPhase::NodePlanning;

  std::map<int, int> node_cpu_count;
  for (int node : hardware.nodes())
    node_cpu_count[node] = static_cast<int>(hardware.cpus_in_node(node).size());

  if (phase_ == IncrementalPhase::NodePlanning) {
    std::map<int, int> assigned_node;
    std::map<int, double> node_load;
    std::map<int, int> node_threads;
    std::map<int, int> thread_cap;
    std::map<int, int> thread_floor;
    for (const auto &[node, cpus] : node_cpu_count) {
      double share = static_cast<double>(threads_.size()) * cpus /
                     static_cast<double>(hardware.cpus.size());
      thread_cap[node] = static_cast<int>(std::ceil(
          share * (1.0 + options.initial_node_thread_slack_ratio)));
      thread_cap[node] = std::min(thread_cap[node], cpu_slot_cap * cpus);
      thread_floor[node] = static_cast<int>(std::floor(
          share * (1.0 - options.initial_node_thread_slack_ratio)));
    }
    std::map<int, std::vector<RelationEdge>> adjacency;
    for (const auto &[_, edge] : edges_) {
      adjacency[edge.from_tid].push_back(edge);
      adjacency[edge.to_tid].push_back(edge);
    }
    std::vector<int> ordered;
    std::set<int> remaining;
    std::set<int> community_seeds;
    for (const auto &[tid, _] : threads_) remaining.insert(tid);
    auto append = [&](int tid) {
      ordered.push_back(tid);
      remaining.erase(tid);
    };
    while (!remaining.empty()) {
      const RelationEdge *best_pair = nullptr;
      const RelationEdge *best_frontier = nullptr;
      int frontier_tid = -1;
      for (const auto &[_, edge] : edges_) {
        bool from_remaining = remaining.contains(edge.from_tid);
        bool to_remaining = remaining.contains(edge.to_tid);
        if (from_remaining && to_remaining) {
          if (!best_pair || std::tuple{edge.score, -edge.from_tid, -edge.to_tid} >
                                std::tuple{best_pair->score, -best_pair->from_tid,
                                           -best_pair->to_tid})
            best_pair = &edge;
        } else if (from_remaining != to_remaining) {
          int candidate = from_remaining ? edge.from_tid : edge.to_tid;
          if (!best_frontier ||
              std::tuple{edge.score, -candidate} >
                  std::tuple{best_frontier->score, -frontier_tid}) {
            best_frontier = &edge;
            frontier_tid = candidate;
          }
        }
      }
      if (best_pair &&
          (!best_frontier || best_pair->score > best_frontier->score)) {
        int first = best_pair->from_tid;
        int second = best_pair->to_tid;
        if (threads_[second].demand > threads_[first].demand ||
            (threads_[second].demand == threads_[first].demand &&
             identities_[second] < identities_[first]))
          std::swap(first, second);
        community_seeds.insert(first);
        append(first);
        append(second);
      } else if (best_frontier) {
        append(frontier_tid);
      } else {
        int seed = *std::min_element(remaining.begin(), remaining.end(),
            [&](int a, int b) {
              if (threads_[a].demand != threads_[b].demand)
                return threads_[a].demand > threads_[b].demand;
              return identities_[a] < identities_[b];
            });
        community_seeds.insert(seed);
        append(seed);
      }
    }

    // Build a fresh capacity-feasible partition from the sparse hotspot graph.
    for (int tid : ordered) {
      int sampled_node = cpu_node(hardware, baseline_cpu_[tid]);
      int best_node = hardware.nodes().front();
      auto objective = [&](int node) {
        int count_overload = std::max(0, node_threads[node] + 1 - thread_cap[node]);
        double demand_overload = std::max(
            0.0, node_load[node] + threads_[tid].demand - node_cpu_count[node]);
        double relation = 0;
        if (!community_seeds.contains(tid))
          for (const auto &edge : adjacency[tid]) {
            int other = edge.from_tid == tid ? edge.to_tid : edge.from_tid;
            if (auto placed = assigned_node.find(other); placed != assigned_node.end())
              relation += edge.score * node_latency(hardware, node, placed->second);
          }
        double utilization = (node_load[node] + threads_[tid].demand) /
                             std::max(node_cpu_count[node], 1);
        return std::tuple{count_overload, demand_overload, relation,
                          node != sampled_node, utilization, node};
      };
      auto best = objective(best_node);
      for (const auto &[node, _] : node_cpu_count) {
        auto candidate = objective(node);
        if (candidate < best) { best = candidate; best_node = node; }
      }
      assigned_node[tid] = best_node;
      node_load[best_node] += threads_[tid].demand;
      ++node_threads[best_node];
    }

    // The slack is symmetric: relation attraction may use node headroom, but
    // it must not empty a node and discard aggregate memory bandwidth. Repair
    // only the lower-bound deficit, choosing the least costly relationship
    // move from a node that remains above its own floor.
    for (const auto &[target, floor] : thread_floor) {
      while (node_threads[target] < floor) {
        int best_tid = -1;
        int best_source = -1;
        double best_delta = std::numeric_limits<double>::infinity();
        for (const auto &[tid, source] : assigned_node) {
          if (source == target || node_threads[source] <= thread_floor[source] ||
              node_threads[target] + 1 > thread_cap[target] ||
              node_load[target] + threads_[tid].demand >
                  node_cpu_count[target] + 1e-12)
            continue;
          double before = 0, after = 0;
          for (const auto &edge : adjacency[tid]) {
            int other = edge.from_tid == tid ? edge.to_tid : edge.from_tid;
            if (!assigned_node.contains(other)) continue;
            before += edge.score * node_latency(
                hardware, source, assigned_node[other]);
            after += edge.score * node_latency(
                hardware, target, assigned_node[other]);
          }
          double delta = after - before;
          if (delta < best_delta - 1e-12 ||
              (std::abs(delta - best_delta) <= 1e-12 &&
               (best_tid < 0 || identities_[tid] < identities_[best_tid]))) {
            best_tid = tid;
            best_source = source;
            best_delta = delta;
          }
        }
        if (best_tid < 0)
          throw std::runtime_error("node thread floor is infeasible");
        assigned_node[best_tid] = target;
        node_load[best_source] -= threads_[best_tid].demand;
        node_load[target] += threads_[best_tid].demand;
        --node_threads[best_source];
        ++node_threads[target];
      }
    }

    // Capacity-neutral first-improvement swaps refine locality without
    // changing the bounded node population established above.
    for (int pass = 0; pass < options.initial_node_passes; ++pass) {
      bool changed = false;
      for (size_t first_index = 0; first_index < ordered.size(); ++first_index) {
        int first = ordered[first_index];
        for (size_t second_index = first_index + 1;
             second_index < ordered.size(); ++second_index) {
          int second = ordered[second_index];
          int first_node = assigned_node[first], second_node = assigned_node[second];
          if (first_node == second_node) continue;
          double before = 0, after = 0;
          for (const auto &[_, edge] : edges_) {
            if (edge.from_tid != first && edge.to_tid != first &&
                edge.from_tid != second && edge.to_tid != second)
              continue;
            int from_before = assigned_node[edge.from_tid];
            int to_before = assigned_node[edge.to_tid];
            int from_after = edge.from_tid == first ? second_node :
                             edge.from_tid == second ? first_node : from_before;
            int to_after = edge.to_tid == first ? second_node :
                           edge.to_tid == second ? first_node : to_before;
            before += edge.score * node_latency(hardware, from_before, to_before);
            after += edge.score * node_latency(hardware, from_after, to_after);
          }
          if (after + 1e-12 >= before) continue;
          double first_load = node_load[first_node] - threads_[first].demand +
                              threads_[second].demand;
          double second_load = node_load[second_node] - threads_[second].demand +
                               threads_[first].demand;
          if (first_load > node_cpu_count[first_node] + 1e-12 ||
              second_load > node_cpu_count[second_node] + 1e-12)
            continue;
          assigned_node[first] = second_node;
          assigned_node[second] = first_node;
          node_load[first_node] = first_load;
          node_load[second_node] = second_load;
          changed = true;
        }
      }
      if (!changed) break;
    }

    if (!previous_initial_target_node_.empty() &&
        initial_candidate_identities_ == identities_) {
      assigned_node = previous_initial_target_node_;
      ++initial_confirmation_;
    } else {
      initial_confirmation_ = 1;
      previous_initial_target_node_ = assigned_node;
      initial_candidate_identities_ = identities_;
    }
    node_load.clear();
    node_threads.clear();
    for (const auto &[tid, node] : assigned_node) {
      node_load[node] += threads_[tid].demand;
      ++node_threads[node];
    }
    proposal.confirmation = initial_confirmation_;
    proposal.phase = phase_;
    proposal.initial_plan_confirmed =
        initial_confirmation_ >= options.initial_proposal_confirmations;
    for (const auto &[node, cpus] : node_cpu_count) {
      proposal.node_overload += std::max(0.0, node_load[node] - cpus);
      proposal.node_overload += std::max(0, node_threads[node] - thread_cap[node]);
    }
    for (const auto &[_, edge] : edges_)
      proposal.relation_cost += edge.score * node_latency(
          hardware, assigned_node[edge.from_tid], assigned_node[edge.to_tid]);
    if (!proposal.initial_plan_confirmed) return proposal;

    initial_target_node_ = std::move(assigned_node);
    last_node_plan_population_ = threads_.size();
    initial_target_cpu_.clear();
    std::map<int, double> cpu_load;
    std::map<int, int> cpu_count;
    std::vector<int> cpu_order;
    for (const auto &[tid, _] : threads_) cpu_order.push_back(tid);
    std::sort(cpu_order.begin(), cpu_order.end(), [&](int a, int b) {
      if (threads_[a].demand != threads_[b].demand)
        return threads_[a].demand > threads_[b].demand;
      return identities_[a] < identities_[b];
    });
    for (int tid : cpu_order) {
      auto cpus = hardware.cpus_in_node(initial_target_node_[tid]);
      int preferred = baseline_cpu_[tid];
      auto cpu_objective = [&](int cpu) {
        double relation = 0;
        for (const auto &edge : adjacency[tid]) {
          int other = edge.from_tid == tid ? edge.to_tid : edge.from_tid;
          if (auto placed = initial_target_cpu_.find(other);
              placed != initial_target_cpu_.end())
            relation += edge.score * hardware.latency(cpu, placed->second);
        }
        return std::tuple{cpu_count[cpu] >= cpu_slot_cap,
                          std::max(0.0, cpu_load[cpu] + threads_[tid].demand - 1.0),
                          relation, cpu_load[cpu] + threads_[tid].demand,
                          cpu_count[cpu], cpu != preferred, cpu};
      };
      int best_cpu = *std::min_element(cpus.begin(), cpus.end(),
          [&](int a, int b) { return cpu_objective(a) < cpu_objective(b); });
      initial_target_cpu_[tid] = best_cpu;
      cpu_load[best_cpu] += threads_[tid].demand;
      ++cpu_count[best_cpu];
    }
    for (const auto &[_, count] : cpu_count)
      proposal.maximum_cpu_threads =
          std::max(proposal.maximum_cpu_threads, count);
    phase_ = IncrementalPhase::InitialPinning;
    proposal.phase = phase_;
    return proposal;
  }

  if (phase_ == IncrementalPhase::InitialPinning) {
    proposal.phase = phase_;
    proposal.initial_plan_confirmed = true;
    if (proposal.migration_budget == 0) return proposal;
    std::vector<int> pending;
    for (const auto &[tid, _] : threads_)
      if (!pinned_.contains(tid) ||
          (replan_existing_threads_ &&
           (!placement_.contains(tid) || placement_[tid] != initial_target_cpu_[tid])))
        pending.push_back(tid);
    std::sort(pending.begin(), pending.end(), [&](int a, int b) {
      int a_node = cpu_node(hardware, baseline_cpu_[a]);
      int b_node = cpu_node(hardware, baseline_cpu_[b]);
      bool a_invalid = a_node < 0, b_invalid = b_node < 0;
      bool a_cross = a_node != initial_target_node_[a];
      bool b_cross = b_node != initial_target_node_[b];
      if (a_invalid != b_invalid) return a_invalid > b_invalid;
      if (a_cross != b_cross) return a_cross > b_cross;
      if (threads_[a].demand != threads_[b].demand)
        return threads_[a].demand > threads_[b].demand;
      return identities_[a] < identities_[b];
    });
    pending.resize(std::min(pending.size(), proposal.migration_budget));

    // A target map can be globally capacity-feasible yet an incremental
    // subset can still depend on a CPU being vacated by a later batch. Repair
    // this batch against the placement that will actually remain committed.
    // This makes the slot cap an invariant after every commit, not merely at
    // the end of the complete initial plan.
    std::map<int, int> projected_count;
    std::map<int, double> projected_load;
    for (const auto &[tid, cpu] : placement_) {
      if (!threads_.contains(tid)) continue;
      ++projected_count[cpu];
      projected_load[cpu] += threads_[tid].demand;
    }
    for (int tid : pending) {
      if (!placement_.contains(tid)) continue;
      int source = placement_[tid];
      --projected_count[source];
      projected_load[source] -= threads_[tid].demand;
    }
    for (int tid : pending) {
      int target = initial_target_cpu_[tid];
      if (projected_count[target] >= cpu_slot_cap) {
        auto candidates = hardware.cpus_in_node(initial_target_node_[tid]);
        auto available = [&](int cpu) {
          return projected_count[cpu] < cpu_slot_cap;
        };
        if (std::none_of(candidates.begin(), candidates.end(), available)) {
          candidates.clear();
          for (const auto &cpu : hardware.cpus)
            if (cpu.online && available(cpu.id)) candidates.push_back(cpu.id);
        }
        if (candidates.empty())
          throw std::runtime_error("initial pinning has no slot-cap-feasible CPU");
        target = *std::min_element(candidates.begin(), candidates.end(),
            [&](int a, int b) {
              return std::tuple{projected_count[a] >= cpu_slot_cap,
                                std::max(0.0, projected_load[a] +
                                                  threads_[tid].demand - 1.0),
                                projected_load[a], projected_count[a], a} <
                     std::tuple{projected_count[b] >= cpu_slot_cap,
                                std::max(0.0, projected_load[b] +
                                                  threads_[tid].demand - 1.0),
                                projected_load[b], projected_count[b], b};
            });
        initial_target_cpu_[tid] = target;
        initial_target_node_[tid] = cpu_node(hardware, target);
      }
      ++projected_count[target];
      projected_load[target] += threads_[tid].demand;
    }
    for (int tid : pending) {
      PlacementAction action;
      action.identity = identities_[tid];
      action.from_cpu = placement_.contains(tid) ? placement_[tid] : baseline_cpu_[tid];
      action.target_cpu = initial_target_cpu_[tid];
      action.initial_pin = true;
      action.emergency = cpu_node(hardware, action.from_cpu) < 0;
      proposal.actions.push_back(action);
      proposal.delta.tid_to_cpu[tid] = action.target_cpu;
    }
    proposal.ready = !proposal.actions.empty();
    proposal.confirmation = initial_confirmation_;
    if (proposal.ready) outstanding_ = proposal;
    return proposal;
  }

  proposal.phase = phase_;
  proposal.initial_plan_confirmed = true;
  if (proposal.migration_budget == 0) return proposal;

  std::map<int, double> cpu_load, node_load;
  std::map<int, int> cpu_count;
  for (const auto &[tid, thread] : threads_) {
    int cpu = placement_.contains(tid) ? placement_[tid] : baseline_cpu_[tid];
    cpu_load[cpu] += thread.demand;
    ++cpu_count[cpu];
    node_load[cpu_node(hardware, cpu)] += thread.demand;
  }
  for (const auto &[_, count] : cpu_count)
    proposal.maximum_cpu_threads =
        std::max(proposal.maximum_cpu_threads, count);
  auto node_balance_cost = [&](const std::map<int, double> &loads) {
    double value = 0;
    for (const auto &[node, cpus] : node_cpu_count) {
      double load = loads.contains(node) ? loads.at(node) : 0.0;
      value += load * load / std::max(cpus, 1);
    }
    return value;
  };
  double minimum_node_utilization = std::numeric_limits<double>::infinity();
  for (const auto &[node, cpus] : node_cpu_count)
    minimum_node_utilization = std::min(
        minimum_node_utilization, node_load[node] / std::max(cpus, 1));
  std::map<int, std::vector<RelationEdge>> adjacency;
  for (const auto &[_, edge] : edges_) {
    adjacency[edge.from_tid].push_back(edge);
    adjacency[edge.to_tid].push_back(edge);
  }
  std::set<int> dirty = delta ? delta->dirty_tids() : std::set<int>{};
  for (const auto &[tid, thread] : threads_) {
    if (!delta && (!previous_demand_.contains(tid) ||
        std::abs(previous_demand_[tid] - thread.demand) >=
            options.demand_dirty_threshold))
      dirty.insert(tid);
    int cpu = placement_.contains(tid) ? placement_[tid] : baseline_cpu_[tid];
    int node = cpu_node(hardware, cpu);
    if (node < 0 || cpu_load[cpu] > 1.0 ||
        cpu_count[cpu] > cpu_slot_cap ||
        (node >= 0 && (node_load[node] > node_cpu_count[node] ||
         node_load[node] / std::max(node_cpu_count[node], 1) >
             minimum_node_utilization + options.node_balance_threshold)))
      dirty.insert(tid);
  }
  if (!delta) for (const auto &[key, edge] : edges_) {
    double old = previous_edge_score_.contains(key) ? previous_edge_score_[key] : 0;
    double absolute = std::abs(edge.score - old);
    double relative = absolute / std::max(std::abs(old), 1e-9);
    if (!previous_edge_score_.contains(key) ||
        absolute >= options.edge_dirty_absolute_threshold ||
        relative >= options.edge_dirty_relative_threshold) {
      dirty.insert(key.first);
      dirty.insert(key.second);
    }
  }
  if (!delta) for (const auto &[key, _] : previous_edge_score_)
    if (!edges_.contains(key)) {
      dirty.insert(key.first);
      dirty.insert(key.second);
    }
  std::vector<int> all_tids;
  for (const auto &[tid, _] : threads_) all_tids.push_back(tid);
  if (!all_tids.empty()) {
    for (int count = 0; count < options.rotating_scan_size; ++count)
      dirty.insert(all_tids[(scan_cursor_ + count) % all_tids.size()]);
    scan_cursor_ = (scan_cursor_ + options.rotating_scan_size) % all_tids.size();
  }
  proposal.dirty_threads = dirty.size();
  previous_demand_.clear();
  for (const auto &[tid, thread] : threads_) previous_demand_[tid] = thread.demand;
  previous_edge_score_.clear();
  for (const auto &[key, edge] : edges_) previous_edge_score_[key] = edge.score;

  struct Dissatisfaction {
    int tid;
    bool invalid;
    int slots;
    double capacity;
    double node_balance;
    double relation;
    double load;
  };
  std::vector<Dissatisfaction> ranked;
  for (int tid : dirty) {
    if (!threads_.contains(tid) || !placement_.contains(tid)) continue;
    int cpu = placement_[tid], node = cpu_node(hardware, cpu);
    double capacity = threads_[tid].demand * std::max(0.0, cpu_load[cpu] - 1.0);
    if (node >= 0)
      capacity += threads_[tid].demand *
                  std::max(0.0, node_load[node] - node_cpu_count[node]);
    double current_relation = 0;
    double best_node_relation = std::numeric_limits<double>::infinity();
    for (const auto &[candidate_node, _] : node_cpu_count) {
      double value = 0;
      for (const auto &edge : adjacency[tid]) {
        int other = edge.from_tid == tid ? edge.to_tid : edge.from_tid;
        if (!placement_.contains(other)) continue;
        value += edge.score * node_latency(
            hardware, candidate_node, cpu_node(hardware, placement_[other]));
      }
      if (candidate_node == node) current_relation = value;
      best_node_relation = std::min(best_node_relation, value);
    }
    double minimum_cpu_load = std::numeric_limits<double>::infinity();
    if (node >= 0)
      for (int candidate : hardware.cpus_in_node(node))
        minimum_cpu_load = std::min(minimum_cpu_load, cpu_load[candidate]);
    double node_imbalance = node >= 0
        ? threads_[tid].demand * std::max(
              0.0, node_load[node] / std::max(node_cpu_count[node], 1) -
                       minimum_node_utilization)
        : 0.0;
    ranked.push_back({tid, node < 0,
                      std::max(0, cpu_count[cpu] - cpu_slot_cap), capacity,
                      node_imbalance,
                      std::max(0.0, current_relation - best_node_relation),
                      threads_[tid].demand * std::max(
                          0.0, cpu_load[cpu] - minimum_cpu_load)});
  }
  std::sort(ranked.begin(), ranked.end(), [&](const auto &a, const auto &b) {
    auto left = std::tuple{a.invalid, a.slots, a.capacity, a.relation,
                           a.node_balance, a.load,
                           threads_[a.tid].demand};
    auto right = std::tuple{b.invalid, b.slots, b.capacity, b.relation,
                            b.node_balance, b.load,
                            threads_[b.tid].demand};
    if (left != right) return left > right;
    return identities_[a.tid] < identities_[b.tid];
  });
  size_t candidate_limit = std::min<size_t>(
      options.candidate_hard_limit,
      options.candidate_multiplier * proposal.migration_budget);
  ranked.resize(std::min(ranked.size(), candidate_limit));
  proposal.candidate_threads = ranked.size();

  struct Potential {
    std::vector<PlacementAction> actions;
    std::string key;
    bool emergency = false;
    double gain = 0;
  };
  std::vector<Potential> potentials;
  std::set<int> cooldown_skipped;
  auto incident_cost = [&](int tid, int candidate_cpu,
                           const std::map<int, int> &overrides) {
    double cost = 0;
    for (const auto &edge : adjacency[tid]) {
      int other = edge.from_tid == tid ? edge.to_tid : edge.from_tid;
      if (!placement_.contains(other)) continue;
      int other_cpu = overrides.contains(other) ? overrides.at(other) : placement_[other];
      cost += edge.score * hardware.latency(candidate_cpu, other_cpu);
    }
    return cost;
  };
  for (const auto &score : ranked) {
    int tid = score.tid;
    int source = placement_[tid];
    bool cooling = last_moved_ns_.contains(tid) &&
                   now_ns - last_moved_ns_[tid] < options.minimum_dwell_ns;
    double current_relation = incident_cost(tid, source, {});
    int best_cpu = source;
    bool best_emergency = false;
    double best_gain = 0;
    auto best_objective = std::tuple{true,
        std::numeric_limits<int>::max(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<int>::max()};
    int source_node = cpu_node(hardware, source);
    for (const auto &cpu_info : hardware.cpus) {
      int cpu = cpu_info.id;
      if (!cpu_info.online || cpu == source) continue;
      int target_node = cpu_info.node;
      int before_slot_violation =
          std::max(0, cpu_count[source] - cpu_slot_cap) +
          std::max(0, cpu_count[cpu] - cpu_slot_cap);
      int after_slot_violation =
          std::max(0, cpu_count[source] - 1 - cpu_slot_cap) +
          std::max(0, cpu_count[cpu] + 1 - cpu_slot_cap);
      double before_overload = std::max(0.0, cpu_load[source] - 1.0) +
          std::max(0.0, cpu_load[cpu] - 1.0);
      if (source_node >= 0)
        before_overload += std::max(0.0, node_load[source_node] - node_cpu_count[source_node]);
      if (target_node != source_node)
        before_overload += std::max(0.0, node_load[target_node] - node_cpu_count[target_node]);
      double after_overload =
          std::max(0.0, cpu_load[source] - threads_[tid].demand - 1.0) +
          std::max(0.0, cpu_load[cpu] + threads_[tid].demand - 1.0);
      if (source_node >= 0 && source_node != target_node) {
        after_overload += std::max(0.0, node_load[source_node] -
            threads_[tid].demand - node_cpu_count[source_node]);
        after_overload += std::max(0.0, node_load[target_node] +
            threads_[tid].demand - node_cpu_count[target_node]);
      }
      auto balanced_node_load = node_load;
      if (source_node >= 0 && source_node != target_node) {
        balanced_node_load[source_node] -= threads_[tid].demand;
        balanced_node_load[target_node] += threads_[tid].demand;
      }
      double before_balance = node_balance_cost(node_load);
      double after_balance = node_balance_cost(balanced_node_load);
      double relation = incident_cost(tid, cpu, {});
      bool balance_emergency = source_node >= 0 && source_node != target_node &&
          node_load[source_node] / std::max(node_cpu_count[source_node], 1) >
              node_load[target_node] / std::max(node_cpu_count[target_node], 1) +
                  options.node_balance_threshold &&
          after_balance + 1e-12 < before_balance &&
          relation <= current_relation + 1e-12;
      bool emergency = source_node < 0 ||
          after_slot_violation < before_slot_violation ||
          after_overload + 1e-12 < before_overload ||
          balance_emergency;
      if (cooling && !emergency) {
        cooldown_skipped.insert(tid);
        continue;
      }
      if (after_slot_violation > before_slot_violation ||
          (!emergency && after_overload > before_overload + 1e-12)) continue;
      double gain = current_relation - relation;
      if (!emergency && gain + 1e-12 <
          options.minimum_relative_gain * std::max(current_relation, 1e-9))
        continue;
      auto candidate_objective = std::tuple{!emergency, after_slot_violation,
                                             after_overload, relation, after_balance,
                                             hardware.latency(source, cpu), cpu};
      if (candidate_objective < best_objective) {
        best_cpu = cpu;
        best_emergency = emergency;
        best_gain = gain;
        best_objective = candidate_objective;
      }
    }
    if (best_cpu != source) {
      PlacementAction action{identities_[tid], source, best_cpu, false,
                             best_emergency, score.capacity, score.relation,
                             score.load};
      Potential potential{{action}, {}, best_emergency, best_gain};
      potential.key = action_key(potential.actions);
      potentials.push_back(std::move(potential));
    }
  }

  // Bounded pair swaps recover locality when a one-way move would overload a CPU.
  for (size_t a = 0; a < ranked.size(); ++a) {
    int first = ranked[a].tid;
    if (!placement_.contains(first)) continue;
    for (size_t b = a + 1; b < ranked.size(); ++b) {
      int second = ranked[b].tid;
      int first_cpu = placement_[first], second_cpu = placement_[second];
      if (first_cpu == second_cpu) continue;
      bool first_cooling = last_moved_ns_.contains(first) &&
          now_ns - last_moved_ns_[first] < options.minimum_dwell_ns;
      bool second_cooling = last_moved_ns_.contains(second) &&
          now_ns - last_moved_ns_[second] < options.minimum_dwell_ns;
      if (first_cooling || second_cooling) {
        if (first_cooling) cooldown_skipped.insert(first);
        if (second_cooling) cooldown_skipped.insert(second);
        continue;
      }
      double before = incident_cost(first, first_cpu, {}) +
                      incident_cost(second, second_cpu, {});
      std::map<int, int> overrides{{first, second_cpu}, {second, first_cpu}};
      double after = incident_cost(first, second_cpu, overrides) +
                     incident_cost(second, first_cpu, overrides);
      // The edge between the pair is counted twice both before and after.
      double gain = before - after;
      if (gain + 1e-12 < options.minimum_relative_gain * std::max(before, 1e-9))
        continue;
      double first_after = cpu_load[first_cpu] - threads_[first].demand + threads_[second].demand;
      double second_after = cpu_load[second_cpu] - threads_[second].demand + threads_[first].demand;
      double before_cpu_overload = std::max(0.0, cpu_load[first_cpu] - 1.0) +
                                   std::max(0.0, cpu_load[second_cpu] - 1.0);
      double after_cpu_overload = std::max(0.0, first_after - 1.0) +
                                  std::max(0.0, second_after - 1.0);
      if (after_cpu_overload > before_cpu_overload + 1e-12) continue;
      int first_node = cpu_node(hardware, first_cpu);
      int second_node = cpu_node(hardware, second_cpu);
      if (first_node != second_node) {
        double before_node_overload =
            std::max(0.0, node_load[first_node] - node_cpu_count[first_node]) +
            std::max(0.0, node_load[second_node] - node_cpu_count[second_node]);
        double after_node_overload = std::max(0.0,
            node_load[first_node] - threads_[first].demand +
            threads_[second].demand - node_cpu_count[first_node]) +
            std::max(0.0, node_load[second_node] - threads_[second].demand +
            threads_[first].demand - node_cpu_count[second_node]);
        if (after_node_overload > before_node_overload + 1e-12) continue;
      }
      std::vector<PlacementAction> actions{
          {identities_[first], first_cpu, second_cpu, false, false,
           ranked[a].capacity, ranked[a].relation, ranked[a].load},
          {identities_[second], second_cpu, first_cpu, false, false,
           ranked[b].capacity, ranked[b].relation, ranked[b].load}};
      Potential potential{actions, action_key(actions), false, gain};
      potentials.push_back(std::move(potential));
    }
  }

  std::sort(potentials.begin(), potentials.end(), [](const auto &a, const auto &b) {
    if (a.emergency != b.emergency) return a.emergency > b.emergency;
    if (a.gain != b.gain) return a.gain > b.gain;
    return a.key < b.key;
  });
  proposal.cooldown_skipped_threads = cooldown_skipped.size();
  std::map<std::string, int> next_confirmations;
  for (const auto &potential : potentials)
    next_confirmations[potential.key] = action_confirmations_[potential.key] + 1;
  action_confirmations_ = next_confirmations;
  std::set<int> selected;
  std::map<int, double> projected_cpu_load = cpu_load;
  std::map<int, double> projected_node_load = node_load;
  std::map<int, int> projected_cpu_count = cpu_count;
  auto projected_overload = [&] {
    double value = 0;
    for (const auto &cpu : hardware.cpus)
      value += std::max(0.0, projected_cpu_load[cpu.id] - 1.0);
    for (const auto &[node, cpus] : node_cpu_count)
      value += std::max(0.0, projected_node_load[node] - cpus);
    return value;
  };
  auto projected_slot_violation = [&] {
    int value = 0;
    for (const auto &cpu : hardware.cpus)
      value += std::max(0, projected_cpu_count[cpu.id] - cpu_slot_cap);
    return value;
  };
  auto projected_balance = [&] {
    return node_balance_cost(projected_node_load);
  };
  auto utilization_span = [&](const std::map<int, double> &loads) {
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = 0;
    for (const auto &[node, cpus] : node_cpu_count) {
      double utilization = loads.contains(node) ? loads.at(node) / cpus : 0;
      minimum = std::min(minimum, utilization);
      maximum = std::max(maximum, utilization);
    }
    return maximum - minimum;
  };
  for (const auto &potential : potentials) {
    if (!potential.emergency &&
        action_confirmations_[potential.key] < options.proposal_confirmations)
      continue;
    if (proposal.actions.size() + potential.actions.size() > proposal.migration_budget)
      continue;
    bool conflict = false;
    for (const auto &action : potential.actions)
      if (selected.contains(action.identity.tid)) conflict = true;
    if (conflict) continue;
    int before_slots = projected_slot_violation();
    double before = projected_overload();
    double before_balance = projected_balance();
    auto next_cpu_load = projected_cpu_load;
    auto next_node_load = projected_node_load;
    auto next_cpu_count = projected_cpu_count;
    for (const auto &action : potential.actions) {
      double demand = threads_[action.identity.tid].demand;
      next_cpu_load[action.from_cpu] -= demand;
      next_cpu_load[action.target_cpu] += demand;
      --next_cpu_count[action.from_cpu];
      ++next_cpu_count[action.target_cpu];
      int from_node = cpu_node(hardware, action.from_cpu);
      int target_node = cpu_node(hardware, action.target_cpu);
      if (from_node != target_node) {
        next_node_load[from_node] -= demand;
        next_node_load[target_node] += demand;
      }
    }
    int after_slots = 0;
    for (const auto &cpu : hardware.cpus)
      after_slots += std::max(0, next_cpu_count[cpu.id] - cpu_slot_cap);
    double after = 0;
    for (const auto &cpu : hardware.cpus)
      after += std::max(0.0, next_cpu_load[cpu.id] - 1.0);
    for (const auto &[node, cpus] : node_cpu_count)
      after += std::max(0.0, next_node_load[node] - cpus);
    double after_balance = node_balance_cost(next_node_load);
    bool capacity_improved = after_slots < before_slots || after + 1e-12 < before;
    bool balance_improved = after_balance + 1e-12 < before_balance;
    if (after_slots > before_slots || after > before + 1e-12 ||
        (potential.emergency && !capacity_improved && !balance_improved) ||
        (!potential.emergency && after_balance > before_balance + 1e-12 &&
         utilization_span(next_node_load) > options.node_balance_threshold))
      continue;
    projected_cpu_load = std::move(next_cpu_load);
    projected_node_load = std::move(next_node_load);
    projected_cpu_count = std::move(next_cpu_count);
    for (const auto &action : potential.actions) {
      selected.insert(action.identity.tid);
      proposal.actions.push_back(action);
      proposal.delta.tid_to_cpu[action.identity.tid] = action.target_cpu;
    }
  }
  proposal.ready = !proposal.actions.empty();
  if (proposal.ready) outstanding_ = proposal;
  return proposal;
}

void IncrementalSolver::commit(const SolverProposal &proposal, uint64_t now_ns,
                               const std::set<int> &committed_tids) {
  if (!outstanding_ || outstanding_->id != proposal.id) return;
  for (const auto &action : proposal.actions) {
    if (!committed_tids.empty() && !committed_tids.contains(action.identity.tid))
      continue;
    placement_[action.identity.tid] = action.target_cpu;
    pinned_.insert(action.identity.tid);
    if (action.from_cpu != action.target_cpu)
      last_moved_ns_[action.identity.tid] = now_ns;
  }
  if (phase_ == IncrementalPhase::InitialPinning) {
    bool targets_effective = pinned_.size() == threads_.size();
    if (replan_existing_threads_)
      for (const auto &[tid, _] : threads_)
        targets_effective = targets_effective && initial_target_cpu_.contains(tid) &&
                            placement_.contains(tid) &&
                            placement_[tid] == initial_target_cpu_[tid];
    if (targets_effective) {
      phase_ = IncrementalPhase::Optimizing;
      replan_existing_threads_ = false;
    }
  }
  ++generation_;
  outstanding_.reset();
}

void IncrementalSolver::discard(const SolverProposal &proposal) {
  if (outstanding_ && outstanding_->id == proposal.id) outstanding_.reset();
}
} // namespace affinitygraph
