#include "affinitygraph/core.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>

using affinitygraph::HardwareGraph;
using affinitygraph::Cpu;
using affinitygraph::IncrementalOptions;
using affinitygraph::IncrementalSolver;
using affinitygraph::Placement;
using affinitygraph::RelationEdge;
using affinitygraph::SolveOptions;
using affinitygraph::Solver;
using affinitygraph::ThreadDemand;
using affinitygraph::incremental_phase_name;
using affinitygraph::select_hotspot_edges;
using affinitygraph::select_managed_threads;
using boost::property_tree::ptree;

namespace {
std::string trim(std::string value) {
  auto space = [](unsigned char c) { return std::isspace(c); };
  value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), space));
  value.erase(std::find_if_not(value.rbegin(), value.rend(), space).base(), value.end());
  return value;
}

std::string unquote(std::string value) {
  value = trim(std::move(value));
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
    return value.substr(1, value.size() - 2);
  return value;
}

SolveOptions read_strategy(const std::string &path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open strategy: " + path);
  SolveOptions options;
  std::set<std::string> seen;
  std::string line;
  while (std::getline(input, line)) {
    if (auto comment = line.find('#'); comment != std::string::npos) line.resize(comment);
    line = trim(line);
    if (line.empty() || line.front() == '[') continue;
    auto equal = line.find('=');
    if (equal == std::string::npos) throw std::runtime_error("invalid strategy line: " + line);
    std::string key = trim(line.substr(0, equal));
    std::string value = trim(line.substr(equal + 1));
    if (!seen.insert(key).second) throw std::runtime_error("duplicate strategy key: " + key);
    if (key == "schema") {
      if (unquote(value) != "affinitygraph.strategy.v1")
        throw std::runtime_error("unsupported strategy schema");
    } else if (key == "id") options.strategy_id = unquote(value);
    else if (key == "demand_floor") options.demand_floor = std::stod(value);
    else if (key == "max_threads_per_cpu") options.max_threads_per_cpu = std::stoi(value);
    else if (key == "slot_slack") options.slot_slack = std::stoi(value);
    else if (key == "count_penalty") options.count_penalty = std::stod(value);
    else if (key == "low_demand_spread") options.low_demand_spread = std::stod(value);
    else if (key == "same_cpu_contention_penalty") options.same_cpu_contention_penalty = std::stod(value);
    else if (key == "group_spread_penalty") options.group_spread_penalty = std::stod(value);
    else if (key == "tie_break_policy") options.thread_count_tie_break = unquote(value) == "thread_count";
    else if (key == "fm_passes") options.fm_passes = std::stoi(value);
    else if (key == "initial_node_thread_slack_ratio")
      options.initial_node_thread_slack_ratio = std::stod(value);
    else if (key == "lpt_refinement_passes") options.lpt_refinement_passes = std::stoi(value);
    else if (key == "hotspot_edges_per_thread") options.hotspot_edges_per_thread = std::stoi(value);
    else if (key == "hotspot_edge_quantile") options.hotspot_edge_quantile = std::stod(value);
    else if (key == "hotspot_component_boost") options.hotspot_component_boost = std::stod(value);
    else if (key == "maximum_managed_threads") options.maximum_managed_threads = std::stoi(value);
    else if (key == "managed_thread_hysteresis_ratio") options.managed_thread_hysteresis_ratio = std::stod(value);
    else if (key == "hotspot_replan_growth_ratio") options.hotspot_replan_growth_ratio = std::stod(value);
    else if (key == "hotspot_replan_min_threads") options.hotspot_replan_min_threads = std::stoi(value);
    else if (key == "hotspot_stability_threshold") options.hotspot_stability_threshold = std::stod(value);
    else if (key == "hotspot_stability_confirmations") options.hotspot_stability_confirmations = std::stoi(value);
    else if (key == "seed") options.seed = std::stoull(value);
    else if (key == "maximum_migrated_active_threads_ratio" ||
             key == "maximum_migrated_threads_ratio")
      options.maximum_migrated_active_threads_ratio = std::stod(value);
    else if (key == "active_threshold") options.active_threshold = std::stod(value);
    else if (key == "inactive_threshold")
      options.inactive_threshold = std::stod(value);
    else if (key == "minimum_confidence") options.minimum_confidence = std::stod(value);
    else throw std::runtime_error("unapproved strategy key: " + key);
  }
  if (options.strategy_id.empty()) throw std::runtime_error("strategy id is required");
  if (options.demand_floor < 0 || options.demand_floor > 1 ||
      options.max_threads_per_cpu < -1 || options.slot_slack < 0 ||
      options.maximum_migrated_active_threads_ratio < 0 ||
      options.maximum_migrated_active_threads_ratio > 1 || options.fm_passes < 0 ||
      options.minimum_confidence < 0 || options.minimum_confidence > 1 ||
      options.inactive_threshold < 0 ||
      options.inactive_threshold > options.active_threshold ||
      options.initial_node_thread_slack_ratio < 0 ||
      options.initial_node_thread_slack_ratio > 1 ||
      options.hotspot_edges_per_thread < 1 || options.hotspot_edge_quantile < 0 ||
      options.hotspot_edge_quantile > 1 || options.hotspot_component_boost < 0 ||
      options.maximum_managed_threads < 0 ||
      options.managed_thread_hysteresis_ratio < 0 ||
      options.hotspot_replan_growth_ratio < 0 ||
      options.hotspot_replan_growth_ratio > 1 ||
      options.hotspot_replan_min_threads < 1 ||
      options.hotspot_stability_threshold < 0 ||
      options.hotspot_stability_threshold > 1 ||
      options.hotspot_stability_confirmations < 1)
    throw std::runtime_error("strategy value outside approved range");
  return options;
}

template <typename T> T required(const ptree &tree, const std::string &path) {
  auto value = tree.get_optional<T>(path);
  if (!value) throw std::runtime_error("snapshot missing " + path);
  return *value;
}

void read_snapshot(const std::string &path, std::string &window_id,
                   HardwareGraph &hardware, std::vector<ThreadDemand> &threads,
                   std::vector<RelationEdge> &edges) {
  ptree root;
  boost::property_tree::read_json(path, root);
  if (required<std::string>(root, "schema") != "affinitygraph.snapshot.v1")
    throw std::runtime_error("unsupported snapshot schema");
  window_id = required<std::string>(root, "window_id");
  for (const auto &[_, item] : root.get_child("topology.cpus"))
    hardware.cpus.push_back({required<int>(item, "id"), required<int>(item, "node"),
                             item.get("online", true)});
  if (auto values = root.get_child_optional("topology.cpu_latencies"))
    for (const auto &[_, item] : *values)
      hardware.cpu_latency[{required<int>(item, "from_cpu"), required<int>(item, "to_cpu")}] =
          required<double>(item, "latency");
  if (auto values = root.get_child_optional("topology.node_distances"))
    for (const auto &[_, item] : *values)
      hardware.node_distance[{required<int>(item, "from_node"), required<int>(item, "to_node")}] =
          required<double>(item, "distance");
  for (const auto &[_, item] : root.get_child("threads"))
    threads.push_back({{required<int>(item, "tgid"), required<int>(item, "tid"),
                        required<uint64_t>(item, "starttime")},
                       required<std::string>(item, "group"), required<double>(item, "demand"),
                       required<double>(item, "confidence"), required<int>(item, "current_cpu")});
  if (auto values = root.get_child_optional("edges"))
  for (const auto &[_, item] : *values) {
    RelationEdge edge;
    edge.from_tid = required<int>(item, "from_tid");
    edge.to_tid = required<int>(item, "to_tid");
    edge.activity = item.get("activity", 0.0);
    edge.sync = item.get("sync", 0.0);
    edge.share = item.get("share", 0.0);
    edge.stability = item.get("stability", 0.0);
    edge.score = required<double>(item, "score");
    edge.handoff_rate = item.get("handoff_rate", 0.0);
    edge.shared_vfs_seconds = item.get("shared_vfs_seconds", 0.0);
    edge.active_overlap = item.get("active_overlap", 0.0);
    edge.observation_count = item.get("observation_count", 0U);
    edge.coverage = item.get("coverage", 0.0);
    edge.coefficient_of_variation = item.get("cv", 0.0);
    edges.push_back(edge);
  }
  if (hardware.cpus.empty()) throw std::runtime_error("snapshot has empty CPU envelope");
}

double percentile(std::vector<double> values, double quantile) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  return values[static_cast<size_t>(std::ceil(quantile * values.size()) - 1)];
}

std::string json_escape(const std::string &value) {
  std::ostringstream out;
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') out << '\\' << c;
    else if (c == '\n') out << "\\n";
    else if (c >= 0x20) out << c;
  }
  return out.str();
}

void write_result(const std::string &path, const std::string &window_id,
                  const SolveOptions &options, const HardwareGraph &hardware,
                  const std::vector<ThreadDemand> &threads,
                  const std::vector<RelationEdge> &edges, const Placement &placement,
                  double solve_ms, bool deterministic) {
  std::map<int, int> counts;
  std::map<int, double> cpu_demand, node_demand;
  std::map<int, int> cpu_node;
  std::set<int> envelope;
  for (const auto &cpu : hardware.cpus) {
    envelope.insert(cpu.id);
    cpu_node[cpu.id] = cpu.node;
    counts[cpu.id] = 0;
    cpu_demand[cpu.id] = 0;
  }
  size_t active = 0, active_migrations = 0, inactive_migrations = 0;
  double migration_distance = 0;
  std::map<int, ThreadDemand> by_tid;
  std::map<std::string, std::set<int>> group_cpus;
  for (const auto &thread : threads) {
    by_tid[thread.identity.tid] = thread;
    auto assigned = placement.tid_to_cpu.find(thread.identity.tid);
    if (assigned == placement.tid_to_cpu.end()) continue;
    ++counts[assigned->second];
    cpu_demand[assigned->second] += thread.demand;
    node_demand[cpu_node[assigned->second]] += thread.demand;
    group_cpus[thread.group].insert(assigned->second);
    if (thread.demand >= options.active_threshold) {
      ++active;
      if (assigned->second != thread.current_cpu) ++active_migrations;
    } else if (assigned->second != thread.current_cpu) ++inactive_migrations;
    if (assigned->second != thread.current_cpu)
      migration_distance += hardware.latency(thread.current_cpu, assigned->second);
  }
  std::vector<double> count_values, demand_values;
  for (const auto &[cpu, count] : counts) {
    count_values.push_back(count);
    demand_values.push_back(cpu_demand[cpu]);
  }
  double mean = std::accumulate(count_values.begin(), count_values.end(), 0.0) / count_values.size();
  double variance = 0;
  for (double value : count_values) variance += (value - mean) * (value - mean);
  double cv = mean > 0 ? std::sqrt(variance / count_values.size()) / mean : 0;
  double gini_sum = 0;
  for (double a : count_values) for (double b : count_values) gini_sum += std::abs(a - b);
  double gini = mean > 0 ? gini_sum / (2 * count_values.size() * count_values.size() * mean) : 0;
  int cap = options.max_threads_per_cpu > 0 ? options.max_threads_per_cpu :
      static_cast<int>(std::ceil(static_cast<double>(threads.size()) / hardware.cpus.size())) +
          options.slot_slack;
  int max_count = counts.empty() ? 0 : std::max_element(counts.begin(), counts.end(),
      [](const auto &a, const auto &b) { return a.second < b.second; })->second;
  double max_demand = *std::max_element(demand_values.begin(), demand_values.end());
  double node_overload = 0;
  for (const auto &[node, load] : node_demand)
    node_overload += std::max(0.0, load - static_cast<double>(hardware.cpus_in_node(node).size()));
  bool complete = placement.tid_to_cpu.size() == threads.size();
  bool inside = std::all_of(placement.tid_to_cpu.begin(), placement.tid_to_cpu.end(),
      [&](const auto &item) { return envelope.contains(item.second); });
  bool active_budget = active == 0 || static_cast<double>(active_migrations) / active <=
      options.maximum_migrated_active_threads_ratio + 1e-12;
  bool gates_pass = complete && inside && placement.overload <= 1e-12 &&
                    node_overload <= 1e-12 && max_count <= cap && active_budget &&
                    deterministic && solve_ms < 1000.0;
  double group_edge_weight = 0, local_group_edge_weight = 0;
  for (const auto &edge : edges) {
    auto from = by_tid.find(edge.from_tid), to = by_tid.find(edge.to_tid);
    if (from == by_tid.end() || to == by_tid.end() || from->second.group != to->second.group)
      continue;
    group_edge_weight += edge.score;
    int from_cpu = placement.tid_to_cpu.at(edge.from_tid);
    int to_cpu = placement.tid_to_cpu.at(edge.to_tid);
    if (cpu_node[from_cpu] == cpu_node[to_cpu]) local_group_edge_weight += edge.score;
  }
  double mean_group_cpu_spread = 0;
  for (const auto &[group, cpus] : group_cpus) mean_group_cpu_spread += cpus.size();
  if (!group_cpus.empty()) mean_group_cpu_spread /= group_cpus.size();

  std::ofstream out(path);
  if (!out) throw std::runtime_error("cannot open output: " + path);
  out << std::setprecision(17);
  out << "{\n  \"schema\": \"affinitygraph.replay-result.v1\",\n"
      << "  \"window_id\": \"" << json_escape(window_id) << "\",\n"
      << "  \"strategy_id\": \"" << json_escape(options.strategy_id) << "\",\n"
      << "  \"assignments\": [\n";
  size_t index = 0;
  for (const auto &[tid, cpu] : placement.tid_to_cpu) {
    const auto &thread = by_tid.at(tid);
    out << "    {\"tgid\": " << thread.identity.tgid << ", \"tid\": " << tid
        << ", \"starttime\": " << thread.identity.starttime << ", \"cpu\": " << cpu << "}"
        << (++index == placement.tid_to_cpu.size() ? "\n" : ",\n");
  }
  out << "  ],\n  \"gates\": {\n"
      << "    \"assignment_complete\": " << (complete ? "true" : "false") << ",\n"
      << "    \"cpu_in_envelope\": " << (inside ? "true" : "false") << ",\n"
      << "    \"singleton\": true,\n"
      << "    \"cpu_demand_overload_zero\": " << (placement.overload <= 1e-12 ? "true" : "false") << ",\n"
      << "    \"node_demand_overload_zero\": " << (node_overload <= 1e-12 ? "true" : "false") << ",\n"
      << "    \"thread_slot_limit\": " << (max_count <= cap ? "true" : "false") << ",\n"
      << "    \"active_migration_budget\": " << (active_budget ? "true" : "false") << ",\n"
      << "    \"deterministic\": " << (deterministic ? "true" : "false") << ",\n"
      << "    \"solve_below_1s\": " << (solve_ms < 1000 ? "true" : "false") << ",\n"
      << "    \"passed\": " << (gates_pass ? "true" : "false") << "\n  },\n"
      << "  \"metrics\": {\n"
      << "    \"max_threads_per_cpu\": " << max_count << ",\n"
      << "    \"p95_threads_per_cpu\": " << percentile(count_values, 0.95) << ",\n"
      << "    \"thread_count_cv\": " << cv << ",\n"
      << "    \"thread_count_gini\": " << gini << ",\n"
      << "    \"max_cpu_demand\": " << max_demand << ",\n"
      << "    \"relation_weighted_latency\": " << placement.relation_cost << ",\n"
      << "    \"same_cpu_edge_weight\": " << placement.same_cpu_edge_weight << ",\n"
      << "    \"active_migrations\": " << active_migrations << ",\n"
      << "    \"inactive_migrations\": " << inactive_migrations << ",\n"
      << "    \"migration_cost\": " << placement.migration_cost << ",\n"
      << "    \"migration_distance\": " << migration_distance << ",\n"
      << "    \"group_locality\": "
      << (group_edge_weight > 0 ? local_group_edge_weight / group_edge_weight : 1.0) << ",\n"
      << "    \"group_nonlocality\": "
      << (group_edge_weight > 0 ? 1.0 - local_group_edge_weight / group_edge_weight : 0.0) << ",\n"
      << "    \"mean_group_cpu_spread\": " << mean_group_cpu_spread << ",\n"
      << "    \"cpu_demand_overload\": " << placement.overload << ",\n"
      << "    \"node_demand_overload\": " << node_overload << ",\n"
      << "    \"solve_ms\": " << solve_ms << "\n  }\n}\n";
}

struct SequenceFrameResult {
  uint64_t timestamp_ns = 0;
  std::string window_id;
  std::string phase;
  size_t demand_active = 0;
  size_t managed = 0;
  size_t retained_managed = 0;
  size_t replaced_managed = 0;
  double managed_jaccard = 1;
  double managed_internal_hotspot_coverage = 1;
  double managed_incident_hotspot_coverage = 1;
  size_t eligible = 0;
  size_t pinned = 0;
  size_t dirty = 0;
  size_t candidates = 0;
  size_t budget = 0;
  size_t actions = 0;
  int cpu_slot_cap = 0;
  int maximum_cpu_threads = 0;
  size_t predicted_demand_threads = 0;
  size_t relation_edges_input = 0;
  size_t hotspot_edges = 0;
  double hotspot_similarity = 0;
  int hotspot_stability_confirmation = 0;
  bool hotspot_replan_triggered = false;
  double relation_node_locality = 1;
  double p95_relation_node_locality = 1;
  double same_cpu_relation_ratio = 0;
  double hotspot_node_locality = 1;
  double hotspot_same_cpu_ratio = 0;
  double cross_node_hotspot_weight = 0;
  double minimum_node_utilization = 0;
  double maximum_node_utilization = 0;
  uint64_t generation = 0;
  bool effective = false;
  double solve_ms = 0;
  std::map<int, int> placement;
};

std::vector<SequenceFrameResult> run_sequence(const std::string &path,
                                              const SolveOptions &strategy) {
  ptree root;
  boost::property_tree::read_json(path, root);
  if (required<std::string>(root, "schema") !=
      "affinitygraph.replay-sequence.v1")
    throw std::runtime_error("unsupported replay sequence schema");
  std::filesystem::path base = std::filesystem::path(path).parent_path();
  IncrementalOptions options;
  options.maximum_migrated_threads_ratio =
      strategy.maximum_migrated_active_threads_ratio;
  options.proposal_confirmations = 3;
  options.initial_node_passes = strategy.fm_passes;
  options.initial_node_thread_slack_ratio =
      strategy.initial_node_thread_slack_ratio;
  options.maximum_threads_per_cpu =
      strategy.max_threads_per_cpu > 0 ? strategy.max_threads_per_cpu : 0;
  options.thread_slot_slack = strategy.slot_slack;
  options.hotspot_edges_per_thread = strategy.hotspot_edges_per_thread;
  options.hotspot_edge_quantile = strategy.hotspot_edge_quantile;
  options.hotspot_component_boost = strategy.hotspot_component_boost;
  options.maximum_managed_threads = strategy.maximum_managed_threads;
  options.managed_thread_hysteresis_ratio =
      strategy.managed_thread_hysteresis_ratio;
  options.hotspot_replan_growth_ratio = strategy.hotspot_replan_growth_ratio;
  options.hotspot_replan_min_threads = strategy.hotspot_replan_min_threads;
  options.hotspot_stability_threshold = strategy.hotspot_stability_threshold;
  options.hotspot_stability_confirmations = strategy.hotspot_stability_confirmations;
  IncrementalSolver solver(true);
  std::set<int> active_cohort;
  std::vector<SequenceFrameResult> results;
  std::vector<Cpu> topology;
  for (const auto &[_, frame] : root.get_child("frames")) {
    uint64_t timestamp_ns = required<uint64_t>(frame, "timestamp_ns");
    std::filesystem::path snapshot = required<std::string>(frame, "snapshot");
    if (snapshot.is_relative()) snapshot = base / snapshot;
    std::string window_id;
    HardwareGraph hardware;
    std::vector<ThreadDemand> threads;
    std::vector<RelationEdge> edges;
    read_snapshot(snapshot.string(), window_id, hardware, threads, edges);
    if (topology.empty()) topology = hardware.cpus;
    else if (hardware.cpus != topology)
      throw std::runtime_error("sequence changes CPU topology");
    std::vector<ThreadDemand> active_candidates;
    for (const auto &thread : threads) {
      if (thread.confidence < strategy.minimum_confidence) continue;
      bool retained = active_cohort.contains(thread.identity.tid) &&
                      thread.demand >= strategy.inactive_threshold;
      if (thread.demand >= strategy.active_threshold || retained) {
        active_candidates.push_back(thread);
      }
    }
    auto active_threads = select_managed_threads(
        active_candidates, edges, active_cohort, options);
    const size_t previous_managed_count = active_cohort.size();
    std::set<int> next_active_cohort;
    for (const auto &thread : active_threads)
      next_active_cohort.insert(thread.identity.tid);
    std::vector<int> retained_managed;
    std::set_intersection(active_cohort.begin(), active_cohort.end(),
                          next_active_cohort.begin(), next_active_cohort.end(),
                          std::back_inserter(retained_managed));
    std::vector<int> combined_managed;
    std::set_union(active_cohort.begin(), active_cohort.end(),
                   next_active_cohort.begin(), next_active_cohort.end(),
                   std::back_inserter(combined_managed));
    double managed_jaccard = combined_managed.empty()
                                 ? 1.0
                                 : static_cast<double>(retained_managed.size()) /
                                       combined_managed.size();
    std::map<int, ThreadDemand> candidate_map;
    for (const auto &thread : active_candidates)
      candidate_map[thread.identity.tid] = thread;
    double candidate_hotspot_weight = 0, internal_hotspot_weight = 0;
    double incident_hotspot_weight = 0;
    for (const auto &edge : select_hotspot_edges(edges, candidate_map, options)) {
      candidate_hotspot_weight += edge.score;
      bool from = next_active_cohort.contains(edge.from_tid);
      bool to = next_active_cohort.contains(edge.to_tid);
      if (from && to) internal_hotspot_weight += edge.score;
      if (from || to) incident_hotspot_weight += edge.score;
    }
    double internal_coverage = candidate_hotspot_weight > 0
                                   ? internal_hotspot_weight /
                                         candidate_hotspot_weight
                                   : 1.0;
    double incident_coverage = candidate_hotspot_weight > 0
                                   ? incident_hotspot_weight /
                                         candidate_hotspot_weight
                                   : 1.0;
    std::vector<int> cooled;
    std::set_difference(active_cohort.begin(), active_cohort.end(),
                        next_active_cohort.begin(), next_active_cohort.end(),
                        std::back_inserter(cooled));
    for (int tid : cooled) solver.remove_thread(tid);
    active_cohort = std::move(next_active_cohort);
    auto started = std::chrono::steady_clock::now();
    auto proposal = solver.propose(hardware, active_threads, edges, options,
                                   timestamp_ns);
    double solve_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    if (proposal.ready) solver.commit(proposal, timestamp_ns);
    std::map<int, int> cpu_threads;
    int maximum_cpu_threads = 0;
    for (const auto &[_, cpu] : solver.placement())
      maximum_cpu_threads = std::max(maximum_cpu_threads, ++cpu_threads[cpu]);
    std::map<int, int> cpu_node;
    std::map<int, int> node_cpus;
    std::map<int, double> node_demand;
    for (const auto &cpu : hardware.cpus) {
      cpu_node[cpu.id] = cpu.node;
      if (cpu.online) ++node_cpus[cpu.node];
    }
    for (const auto &thread : active_threads) {
      auto assigned = solver.placement().find(thread.identity.tid);
      if (assigned != solver.placement().end())
        node_demand[cpu_node[assigned->second]] += thread.demand;
    }
    double minimum_node_utilization = std::numeric_limits<double>::infinity();
    double maximum_node_utilization = 0;
    for (const auto &[node, cpus] : node_cpus) {
      double utilization = node_demand[node] / cpus;
      minimum_node_utilization = std::min(minimum_node_utilization, utilization);
      maximum_node_utilization = std::max(maximum_node_utilization, utilization);
    }
    if (!std::isfinite(minimum_node_utilization)) minimum_node_utilization = 0;
    std::vector<double> eligible_edge_scores;
    for (const auto &edge : edges)
      if (solver.placement().contains(edge.from_tid) &&
          solver.placement().contains(edge.to_tid) && edge.score > 0)
        eligible_edge_scores.push_back(edge.score);
    double p95_edge_score = percentile(eligible_edge_scores, 0.95);
    double relation_weight = 0, local_relation_weight = 0;
    double p95_weight = 0, p95_local_weight = 0, same_cpu_weight = 0;
    for (const auto &edge : edges) {
      auto from = solver.placement().find(edge.from_tid);
      auto to = solver.placement().find(edge.to_tid);
      if (from == solver.placement().end() || to == solver.placement().end() ||
          edge.score <= 0)
        continue;
      relation_weight += edge.score;
      bool local = cpu_node[from->second] == cpu_node[to->second];
      if (local) local_relation_weight += edge.score;
      if (from->second == to->second) same_cpu_weight += edge.score;
      if (edge.score + 1e-12 >= p95_edge_score) {
        p95_weight += edge.score;
        if (local) p95_local_weight += edge.score;
      }
    }
    std::map<int, ThreadDemand> active_thread_map;
    for (const auto &thread : active_threads)
      active_thread_map[thread.identity.tid] = thread;
    auto selected_hotspots = select_hotspot_edges(edges, active_thread_map, options);
    double hotspot_weight = 0, local_hotspot_weight = 0;
    double same_cpu_hotspot_weight = 0, cross_node_hotspot_weight = 0;
    for (const auto &edge : selected_hotspots) {
      auto from = solver.placement().find(edge.from_tid);
      auto to = solver.placement().find(edge.to_tid);
      if (from == solver.placement().end() || to == solver.placement().end())
        continue;
      hotspot_weight += edge.score;
      if (cpu_node[from->second] == cpu_node[to->second])
        local_hotspot_weight += edge.score;
      else
        cross_node_hotspot_weight += edge.score;
      if (from->second == to->second) same_cpu_hotspot_weight += edge.score;
    }
    results.push_back({timestamp_ns, window_id,
                       incremental_phase_name(solver.phase()),
                       active_candidates.size(), active_threads.size(),
                       retained_managed.size(),
                       previous_managed_count - retained_managed.size(),
                       managed_jaccard, internal_coverage, incident_coverage,
                       proposal.eligible_threads, solver.pinned_threads(),
                       proposal.dirty_threads, proposal.candidate_threads,
                       proposal.migration_budget, proposal.actions.size(),
                       proposal.cpu_slot_cap, maximum_cpu_threads,
                       proposal.predicted_demand_threads,
                       proposal.relation_edges_input, proposal.hotspot_edges,
                       proposal.hotspot_similarity,
                       proposal.hotspot_stability_confirmation,
                       proposal.hotspot_replan_triggered,
                       relation_weight > 0 ? local_relation_weight / relation_weight : 1,
                       p95_weight > 0 ? p95_local_weight / p95_weight : 1,
                       relation_weight > 0 ? same_cpu_weight / relation_weight : 0,
                       hotspot_weight > 0 ? local_hotspot_weight / hotspot_weight : 1,
                       hotspot_weight > 0 ? same_cpu_hotspot_weight / hotspot_weight : 0,
                       cross_node_hotspot_weight,
                       minimum_node_utilization, maximum_node_utilization,
                       solver.generation(), solver.effective(), solve_ms,
                       solver.placement()});
  }
  if (results.empty()) throw std::runtime_error("replay sequence has no frames");
  return results;
}

bool same_sequence(const std::vector<SequenceFrameResult> &a,
                   const std::vector<SequenceFrameResult> &b) {
  if (a.size() != b.size()) return false;
  for (size_t index = 0; index < a.size(); ++index)
    if (a[index].timestamp_ns != b[index].timestamp_ns ||
        a[index].phase != b[index].phase ||
        a[index].actions != b[index].actions ||
        a[index].generation != b[index].generation ||
        a[index].effective != b[index].effective ||
        a[index].placement != b[index].placement)
      return false;
  return true;
}

void write_sequence_result(const std::string &path,
                           const std::vector<SequenceFrameResult> &frames,
                           const SolveOptions &strategy, bool deterministic) {
  std::vector<double> times;
  for (const auto &frame : frames) times.push_back(frame.solve_ms);
  double p95 = percentile(times, 0.95);
  std::ofstream out(path);
  if (!out) throw std::runtime_error("cannot open output: " + path);
  out << std::setprecision(17)
      << "{\n  \"schema\": \"affinitygraph.replay-sequence-result.v1\",\n"
      << "  \"strategy_id\": \"" << json_escape(strategy.strategy_id)
      << "\",\n  \"deterministic\": " << (deterministic ? "true" : "false")
      << ",\n  \"solve_p95_ms\": " << p95
      << ",\n  \"solve_below_1s\": " << (p95 < 1000 ? "true" : "false")
      << ",\n  \"frames\": [\n";
  for (size_t index = 0; index < frames.size(); ++index) {
    const auto &frame = frames[index];
    out << "    {\"timestamp_ns\": " << frame.timestamp_ns
        << ", \"window_id\": \"" << json_escape(frame.window_id)
        << "\", \"phase\": \"" << frame.phase
        << "\", \"demand_active\": " << frame.demand_active
        << ", \"managed\": " << frame.managed
        << ", \"retained_managed\": " << frame.retained_managed
        << ", \"replaced_managed\": " << frame.replaced_managed
        << ", \"managed_jaccard\": " << frame.managed_jaccard
        << ", \"managed_internal_hotspot_coverage\": "
        << frame.managed_internal_hotspot_coverage
        << ", \"managed_incident_hotspot_coverage\": "
        << frame.managed_incident_hotspot_coverage
        << ", \"eligible\": " << frame.eligible
        << ", \"pinned\": " << frame.pinned
        << ", \"dirty\": " << frame.dirty
        << ", \"candidates\": " << frame.candidates
        << ", \"budget\": " << frame.budget
        << ", \"actions\": " << frame.actions
        << ", \"cpu_slot_cap\": " << frame.cpu_slot_cap
        << ", \"maximum_cpu_threads\": " << frame.maximum_cpu_threads
        << ", \"predicted_demand_threads\": "
        << frame.predicted_demand_threads
        << ", \"relation_edges_input\": " << frame.relation_edges_input
        << ", \"hotspot_edges\": " << frame.hotspot_edges
        << ", \"hotspot_similarity\": " << frame.hotspot_similarity
        << ", \"hotspot_stability_confirmation\": "
        << frame.hotspot_stability_confirmation
        << ", \"hotspot_replan_triggered\": "
        << (frame.hotspot_replan_triggered ? "true" : "false")
        << ", \"relation_node_locality\": " << frame.relation_node_locality
        << ", \"p95_relation_node_locality\": "
        << frame.p95_relation_node_locality
        << ", \"same_cpu_relation_ratio\": "
        << frame.same_cpu_relation_ratio
        << ", \"hotspot_node_locality\": "
        << frame.hotspot_node_locality
        << ", \"hotspot_same_cpu_ratio\": "
        << frame.hotspot_same_cpu_ratio
        << ", \"cross_node_hotspot_weight\": "
        << frame.cross_node_hotspot_weight
        << ", \"minimum_node_utilization\": "
        << frame.minimum_node_utilization
        << ", \"maximum_node_utilization\": "
        << frame.maximum_node_utilization
        << ", \"generation\": " << frame.generation
        << ", \"effective\": " << (frame.effective ? "true" : "false")
        << ", \"solve_ms\": " << frame.solve_ms
        << ", \"assignments\": {";
    size_t assignment_index = 0;
    for (const auto &[tid, cpu] : frame.placement) {
      if (assignment_index++) out << ',';
      out << '\"' << tid << "\":" << cpu;
    }
    out << "}}"
        << (index + 1 == frames.size() ? "\n" : ",\n");
  }
  out << "  ]\n}\n";
}
} // namespace

int main(int argc, char **argv) {
  try {
    std::string snapshot_path, sequence_path, strategy_path, output_path;
    for (int index = 1; index < argc; ++index) {
      std::string argument = argv[index];
      if (argument == "--snapshot" && index + 1 < argc) snapshot_path = argv[++index];
      else if (argument == "--sequence" && index + 1 < argc) sequence_path = argv[++index];
      else if (argument == "--strategy" && index + 1 < argc) strategy_path = argv[++index];
      else if (argument == "--output" && index + 1 < argc) output_path = argv[++index];
      else throw std::runtime_error("usage: affinity-replay (--snapshot FILE | --sequence FILE) --strategy FILE --output FILE");
    }
    if ((snapshot_path.empty() == sequence_path.empty()) || strategy_path.empty() ||
        output_path.empty())
      throw std::runtime_error("usage: affinity-replay (--snapshot FILE | --sequence FILE) --strategy FILE --output FILE");
    SolveOptions options = read_strategy(strategy_path);
    if (!sequence_path.empty()) {
      auto frames = run_sequence(sequence_path, options);
      auto repeated = run_sequence(sequence_path, options);
      write_sequence_result(output_path, frames, options,
                            same_sequence(frames, repeated));
      return 0;
    }
    std::string window_id;
    HardwareGraph hardware;
    std::vector<ThreadDemand> threads;
    std::vector<RelationEdge> edges;
    read_snapshot(snapshot_path, window_id, hardware, threads, edges);
    auto start = std::chrono::steady_clock::now();
    Placement placement = Solver().solve(hardware, threads, edges, options);
    double solve_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    Placement repeated = Solver().solve(hardware, threads, edges, options);
    bool deterministic = repeated.tid_to_cpu == placement.tid_to_cpu &&
                         repeated.overload == placement.overload &&
                         repeated.relation_cost == placement.relation_cost &&
                         repeated.migration_cost == placement.migration_cost;
    write_result(output_path, window_id, options, hardware, threads, edges,
                 placement, solve_ms, deterministic);
  } catch (const std::exception &error) {
    std::cerr << "affinity-replay: " << error.what() << '\n';
    return 1;
  }
}
