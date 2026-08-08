#include "affinitygraph/core.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace affinitygraph;
using boost::property_tree::ptree;

namespace {
struct ReplayFrame {
  std::string window_id;
  uint64_t timestamp_ns = 0;
  bool planned = false;
  std::vector<ThreadDemand> threads;
  std::vector<RelationEdge> edges;
  std::map<int, std::vector<int>> allowed_masks;
};

// 该工具复用线上 NumaDomainSolver，但不链接 runtime、BPF reader 或 actuator。
// 输入只取 runtime JSONL 中当时真实进入 plan 的 window，因而可以验证 selector
// 的确定性和解释字段，同时保证 replay 不可能修改本机 affinity。

struct FrameResult {
  std::string window_id;
  bool ready = false;
  bool valid = true;
  double solve_ms = 0;
  std::vector<FamilyMetric> families;
  std::vector<FamilyPairMetric> family_pairs;
  std::vector<NumaDomain> domains;
};

std::string json_escape(const std::string &value) {
  std::ostringstream out;
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') out << '\\' << c;
    else if (c == '\n') out << "\\n";
    else if (c >= 0x20) out << c;
  }
  return out.str();
}

std::vector<int> read_int_array(const ptree &tree, const std::string &path) {
  std::vector<int> result;
  if (auto values = tree.get_child_optional(path))
    for (const auto &[_, value] : *values) result.push_back(value.get_value<int>());
  return result;
}

void read_runtime_log(const std::string &path, HardwareGraph &hardware,
                      std::vector<ReplayFrame> &frames) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open runtime log: " + path);
  std::map<std::string, size_t> frame_index;
  auto frame_for = [&](const std::string &window_id) -> ReplayFrame & {
    auto found = frame_index.find(window_id);
    if (found != frame_index.end()) return frames[found->second];
    frame_index[window_id] = frames.size();
    ReplayFrame frame;
    frame.window_id = window_id;
    frames.push_back(std::move(frame));
    return frames.back();
  };
  std::string line;
  size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (line.empty()) continue;
    ptree item;
    std::istringstream json(line);
    try {
      boost::property_tree::read_json(json, item);
    } catch (const std::exception &error) {
      throw std::runtime_error("invalid runtime JSON at line " +
                               std::to_string(line_number) + ": " + error.what());
    }
    const std::string type = item.get("type", "");
    if (type == "topology_cpu" && item.get("in_envelope", true)) {
      hardware.cpus.push_back({item.get<int>("cpu"), item.get<int>("node"),
                               item.get("online", true)});
    } else if (type == "topology_edge") {
      int from = item.get<int>("from_node"), to = item.get<int>("to_node");
      hardware.node_distance[{from, to}] = item.get("numa_distance", 0.0);
    } else if (type == "solve_window_begin") {
      auto &frame = frame_for(item.get<std::string>("window_id"));
      frame.timestamp_ns = item.get<uint64_t>("timestamp_ns");
    } else if (type == "thread_window") {
      auto &frame = frame_for(item.get<std::string>("window_id"));
      ThreadDemand thread;
      thread.identity = {item.get<int>("tgid"), item.get<int>("tid"),
                         item.get<uint64_t>("starttime")};
      thread.group = item.get<std::string>("group");
      thread.demand = item.get<double>("demand");
      thread.confidence = item.get<double>("confidence");
      thread.current_cpu = item.get<int>("current_cpu");
      frame.threads.push_back(thread);
      frame.allowed_masks[thread.identity.tid] = read_int_array(item, "allowed_cpus");
    } else if (type == "relation_edge") {
      auto &frame = frame_for(item.get<std::string>("window_id"));
      RelationEdge edge;
      edge.from_tid = item.get<int>("from_tid");
      edge.to_tid = item.get<int>("to_tid");
      edge.activity = item.get("activity", 0.0);
      edge.sync = item.get("sync", 0.0);
      edge.share = item.get("share", 0.0);
      edge.stability = item.get("stability", 0.0);
      edge.score = item.get<double>("score");
      frame.edges.push_back(edge);
    } else if (type == "plan" &&
               item.get("strategy_id", "") == "numa-domain-v1") {
      frame_for(item.get<std::string>("window_id")).planned = true;
    }
  }
  std::sort(hardware.cpus.begin(), hardware.cpus.end(),
            [](const auto &a, const auto &b) { return a.id < b.id; });
  hardware.cpus.erase(std::unique(hardware.cpus.begin(), hardware.cpus.end(),
                                 [](const auto &a, const auto &b) {
                                   return a.id == b.id;
                                 }),
                      hardware.cpus.end());
  frames.erase(std::remove_if(frames.begin(), frames.end(),
                              [](const auto &frame) { return !frame.planned; }),
               frames.end());
  if (hardware.cpus.empty()) throw std::runtime_error("runtime log has no topology");
  if (frames.empty()) throw std::runtime_error("runtime log has no NUMA domain plans");
}

NumaDomainOptions domain_options(const Config &config) {
  NumaDomainOptions options;
  options.family_minimum_demand = config.family_minimum_demand;
  options.family_minimum_internal_relation = config.family_minimum_internal_relation;
  options.family_minimum_self_containment = config.family_minimum_self_containment;
  options.family_minimum_relative_internal = config.family_minimum_relative_internal;
  options.domain_merge_ratio = config.domain_merge_ratio;
  options.family_edges_per_family = config.family_edges_per_family;
  options.family_stability_confirmations = config.family_stability_confirmations;
  options.domain_stability_confirmations = config.domain_stability_confirmations;
  options.plan_confirmations = config.domain_plan_confirmations;
  options.maximum_threads_per_domain = config.maximum_threads_per_domain;
  options.capacity_ratio = config.domain_capacity_ratio;
  options.expand_ratio = config.domain_expand_ratio;
  options.expand_confirmations = config.domain_expand_confirmations;
  options.shrink_ratio = config.domain_shrink_ratio;
  options.shrink_confirmations = config.domain_shrink_confirmations;
  options.minimum_dwell_ns =
      static_cast<uint64_t>(config.domain_minimum_dwell_seconds) * 1000000000ULL;
  return options;
}

std::vector<FrameResult> replay(const HardwareGraph &hardware,
                                const std::vector<ReplayFrame> &frames,
                                const NumaDomainOptions &options) {
  NumaDomainSolver solver;
  std::vector<FrameResult> results;
  for (const auto &frame : frames) {
    auto started = std::chrono::steady_clock::now();
    auto proposal = solver.propose(hardware, frame.threads, frame.edges,
                                   frame.allowed_masks, options,
                                   frame.timestamp_ns);
    double solve_ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - started)
                          .count();
    results.push_back({frame.window_id, proposal.ready, proposal.valid, solve_ms,
                       proposal.families, proposal.family_pairs,
                       proposal.domains});
    if (!proposal.valid ||
        (proposal.ready && (!proposal.actions.empty() ||
                            !proposal.released_tids.empty() ||
                            proposal.planned_masks != solver.placement())))
      solver.commit(proposal, frame.timestamp_ns);
    else
      solver.discard(proposal);
  }
  return results;
}

bool same_results(const std::vector<FrameResult> &left,
                  const std::vector<FrameResult> &right) {
  if (left.size() != right.size()) return false;
  for (size_t index = 0; index < left.size(); ++index) {
    if (left[index].window_id != right[index].window_id ||
        left[index].ready != right[index].ready ||
        left[index].valid != right[index].valid ||
        left[index].families.size() != right[index].families.size() ||
        left[index].family_pairs.size() != right[index].family_pairs.size() ||
        left[index].domains.size() != right[index].domains.size())
      return false;
    for (size_t family = 0; family < left[index].families.size(); ++family) {
      const auto &a = left[index].families[family];
      const auto &b = right[index].families[family];
      if (a.name != b.name || a.anchor != b.anchor ||
          a.cohesive_anchor != b.cohesive_anchor || a.cross_seed != b.cross_seed ||
          a.confirmation != b.confirmation ||
          a.seed_confirmation != b.seed_confirmation)
        return false;
    }
    for (size_t pair = 0; pair < left[index].family_pairs.size(); ++pair) {
      const auto &a = left[index].family_pairs[pair];
      const auto &b = right[index].family_pairs[pair];
      if (a.left != b.left || a.right != b.right ||
          a.cross_relation != b.cross_relation ||
          a.merge_ratio != b.merge_ratio || a.qualifies != b.qualifies ||
          a.confirmation != b.confirmation || a.confirmed != b.confirmed)
        return false;
    }
    for (size_t domain = 0; domain < left[index].domains.size(); ++domain) {
      const auto &a = left[index].domains[domain];
      const auto &b = right[index].domains[domain];
      if (a.id != b.id || a.families != b.families || a.tids != b.tids ||
          a.target_nodes != b.target_nodes || a.target_mask != b.target_mask ||
          a.valid != b.valid)
        return false;
    }
  }
  return true;
}

double percentile(std::vector<double> values, double quantile) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  return values[static_cast<size_t>(std::ceil(quantile * values.size()) - 1)];
}

void write_result(const std::string &path, const std::vector<FrameResult> &frames,
                  bool deterministic) {
  std::vector<double> solve_times;
  for (const auto &frame : frames) solve_times.push_back(frame.solve_ms);
  const double p95 = percentile(solve_times, 0.95);
  std::ofstream out(path);
  if (!out) throw std::runtime_error("cannot open output: " + path);
  out << std::setprecision(17)
      << "{\n  \"schema\": \"affinitygraph.domain-replay-result.v1\",\n"
      << "  \"strategy_id\": \"numa-domain-v1\",\n"
      << "  \"deterministic\": " << (deterministic ? "true" : "false") << ",\n"
      << "  \"solve_p95_ms\": " << p95 << ",\n"
      << "  \"solve_below_1s\": " << (p95 < 1000 ? "true" : "false") << ",\n"
      << "  \"frames\": [\n";
  for (size_t index = 0; index < frames.size(); ++index) {
    const auto &frame = frames[index];
    out << "    {\"window_id\": \"" << json_escape(frame.window_id)
        << "\", \"ready\": " << (frame.ready ? "true" : "false")
        << ", \"valid\": " << (frame.valid ? "true" : "false")
        << ", \"solve_ms\": " << frame.solve_ms << ", \"anchors\": [";
    bool first = true;
    for (const auto &family : frame.families) {
      if (!family.anchor) continue;
      if (!first) out << ',';
      first = false;
      out << "{\"name\":\"" << json_escape(family.name)
          << "\",\"cohesive\":" << (family.cohesive_anchor ? "true" : "false")
          << ",\"cross_seed\":" << (family.cross_seed ? "true" : "false")
          << '}';
    }
    out << "], \"family_pairs\": [";
    for (size_t pair = 0; pair < frame.family_pairs.size(); ++pair) {
      const auto &value = frame.family_pairs[pair];
      if (pair) out << ',';
      out << "{\"left\":\"" << json_escape(value.left)
          << "\",\"right\":\"" << json_escape(value.right)
          << "\",\"cross_relation\":" << value.cross_relation
          << ",\"denominator\":" << value.denominator
          << ",\"merge_ratio\":" << value.merge_ratio
          << ",\"endpoints_eligible\":"
          << (value.endpoints_eligible ? "true" : "false")
          << ",\"qualifies\":" << (value.qualifies ? "true" : "false")
          << ",\"confirmation\":" << value.confirmation
          << ",\"confirmed\":" << (value.confirmed ? "true" : "false")
          << '}';
    }
    out << "], \"domains\": [";
    for (size_t domain = 0; domain < frame.domains.size(); ++domain) {
      const auto &value = frame.domains[domain];
      if (domain) out << ',';
      out << "{\"id\":\"" << json_escape(value.id) << "\",\"families\":[";
      for (size_t family = 0; family < value.families.size(); ++family) {
        if (family) out << ',';
        out << '"' << json_escape(value.families[family]) << '"';
      }
      out << "],\"thread_count\":" << value.tids.size() << ",\"demand\":"
          << value.demand << ",\"capacity_limit\":" << value.capacity_limit
          << ",\"capacity_headroom\":" << value.capacity_headroom
          << ",\"background_demand\":" << value.background_demand
          << ",\"initial_migrations\":" << value.initial_migrations
          << ",\"node_decision\":\"" << value.node_decision
          << "\",\"target_nodes\":[";
      for (size_t node = 0; node < value.target_nodes.size(); ++node) {
        if (node) out << ',';
        out << value.target_nodes[node];
      }
      out << "]}";
    }
    out << "]}" << (index + 1 == frames.size() ? "\n" : ",\n");
  }
  out << "  ]\n}\n";
}
} // namespace

int main(int argc, char **argv) {
  try {
    std::string log_path, config_path, output_path;
    for (int index = 1; index < argc; ++index) {
      std::string argument = argv[index];
      if (argument == "--runtime-log" && index + 1 < argc) log_path = argv[++index];
      else if (argument == "--config" && index + 1 < argc) config_path = argv[++index];
      else if (argument == "--output" && index + 1 < argc) output_path = argv[++index];
      else throw std::runtime_error("usage: affinity-domain-replay --runtime-log FILE --config FILE --output FILE");
    }
    if (log_path.empty() || config_path.empty() || output_path.empty())
      throw std::runtime_error("usage: affinity-domain-replay --runtime-log FILE --config FILE --output FILE");
    const Config config = load_config(config_path);
    HardwareGraph hardware;
    std::vector<ReplayFrame> frames;
    read_runtime_log(log_path, hardware, frames);
    const auto options = domain_options(config);
    const auto results = replay(hardware, frames, options);
    const auto repeated = replay(hardware, frames, options);
    write_result(output_path, results, same_results(results, repeated));
  } catch (const std::exception &error) {
    std::cerr << "affinity-domain-replay: " << error.what() << '\n';
    return 1;
  }
}
