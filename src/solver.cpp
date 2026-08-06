#include "affinitygraph/core.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

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

  std::vector<ThreadDemand> ordered = threads;
  std::sort(ordered.begin(), ordered.end(), [&](const auto &a, const auto &b) {
    double ar = 0, br = 0;
    for (const auto &e : adjacency[a.identity.tid]) ar += e.score;
    for (const auto &e : adjacency[b.identity.tid]) br += e.score;
    if (a.demand != b.demand) return a.demand > b.demand;
    if (ar != br) return ar > br;
    return a.identity.tid < b.identity.tid;
  });

  std::map<int, double> node_load;
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
  }

  // Deterministic FM-style single-vertex refinement after the greedy seed.
  for (int pass = 0; pass < 4; ++pass) {
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
        for (const auto &edge : adjacency[thread.identity.tid]) {
          int other = edge.from_tid == thread.identity.tid ? edge.to_tid : edge.from_tid;
          if (auto it = tid_node.find(other); it != tid_node.end()) relation += edge.score * node_latency(candidate, it->second);
        }
        double migration = 0;
        for (const auto &cpu : hardware.cpus) if (cpu.id == thread.current_cpu)
          migration = thread.demand * node_latency(cpu.node, candidate);
        return std::tuple{overload, relation, migration, candidate};
      };
      int best_node = original;
      auto best = objective(original);
      for (int node : nodes) if (objective(node) < best) { best = objective(node); best_node = node; }
      if (best_node != original) {
        node_load[original] -= thread.demand;
        node_load[best_node] += thread.demand;
        tid_node[thread.identity.tid] = best_node;
        changed = true;
      }
    }
    if (!changed) break;
  }

  std::map<int, double> cpu_load;
  for (const auto &thread : ordered) {
    auto cpus = hardware.cpus_in_node(tid_node[thread.identity.tid]);
    if (cpus.empty()) throw std::runtime_error("node has no CPU in envelope");
    int best_cpu = cpus.front();
    auto best = std::tuple<double, double, int>{std::numeric_limits<double>::infinity(), 0, best_cpu};
    for (int cpu : cpus) {
      double relation = 0;
      for (const auto &edge : adjacency[thread.identity.tid]) {
        int other = edge.from_tid == thread.identity.tid ? edge.to_tid : edge.from_tid;
        if (auto it = placement.tid_to_cpu.find(other); it != placement.tid_to_cpu.end())
          relation += edge.score * hardware.latency(cpu, it->second);
      }
      auto candidate = std::tuple{cpu_load[cpu] + thread.demand, relation, cpu};
      if (candidate < best) { best = candidate; best_cpu = cpu; }
    }
    placement.tid_to_cpu[thread.identity.tid] = best_cpu;
    cpu_load[best_cpu] += thread.demand;
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
    if (std::any_of(hardware.cpus.begin(), hardware.cpus.end(), [&](const auto &c) { return c.id == thread->current_cpu; }))
      placement.tid_to_cpu[thread->identity.tid] = thread->current_cpu;
  }

  cpu_load.clear();
  for (const auto &thread : threads) cpu_load[placement.tid_to_cpu[thread.identity.tid]] += thread.demand;
  for (const auto &[cpu, load] : cpu_load) placement.overload += std::max(0.0, load - 1.0);
  for (const auto &edge : edges) {
    auto a = placement.tid_to_cpu.find(edge.from_tid), b = placement.tid_to_cpu.find(edge.to_tid);
    if (a != placement.tid_to_cpu.end() && b != placement.tid_to_cpu.end())
      placement.relation_cost += edge.score * hardware.latency(a->second, b->second);
  }
  for (const auto &thread : threads)
    if (placement.tid_to_cpu[thread.identity.tid] != thread.current_cpu)
      placement.migration_cost += thread.demand * hardware.latency(thread.current_cpu, placement.tid_to_cpu[thread.identity.tid]);
  return placement;
}
} // namespace affinitygraph
