#include "affinitygraph/core.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace affinitygraph {
namespace {
std::string trim(std::string value) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

std::string unquote(const std::string &value) {
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
    return value.substr(1, value.size() - 2);
  throw std::runtime_error("expected quoted TOML string: " + value);
}

bool boolean(const std::string &value) {
  if (value == "true") return true;
  if (value == "false") return false;
  throw std::runtime_error("expected TOML boolean: " + value);
}
} // namespace

std::vector<int> parse_cpu_list(const std::string &text) {
  std::set<int> result;
  std::stringstream input(text);
  std::string part;
  while (std::getline(input, part, ',')) {
    part = trim(part);
    if (part.empty()) throw std::runtime_error("empty CPU-list element");
    auto dash = part.find('-');
    int first = std::stoi(part.substr(0, dash));
    int last = dash == std::string::npos ? first : std::stoi(part.substr(dash + 1));
    if (first < 0 || last < first || last > 1048575)
      throw std::runtime_error("invalid CPU range: " + part);
    for (int cpu = first; cpu <= last; ++cpu) result.insert(cpu);
  }
  if (result.empty()) throw std::runtime_error("CPU envelope must not be empty");
  return {result.begin(), result.end()};
}

std::string format_cpu_list(const std::vector<int> &input) {
  if (input.empty()) return "";
  std::vector<int> cpus = input;
  std::sort(cpus.begin(), cpus.end());
  cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
  std::ostringstream out;
  for (size_t i = 0; i < cpus.size();) {
    size_t j = i;
    while (j + 1 < cpus.size() && cpus[j + 1] == cpus[j] + 1) ++j;
    if (i) out << ',';
    out << cpus[i];
    if (j != i) out << '-' << cpus[j];
    i = j + 1;
  }
  return out.str();
}

Config load_config(const std::string &path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open config: " + path);
  Config c;
  std::string section, line;
  size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    auto hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    line = trim(line);
    if (line.empty()) continue;
    if (line.front() == '[' && line.back() == ']') {
      section = line.substr(1, line.size() - 2);
      if (section != "runtime" && section != "resources" && section != "collector" && section != "calibration")
        throw std::runtime_error("unknown TOML section at line " + std::to_string(line_number));
      continue;
    }
    auto equal = line.find('=');
    if (equal == std::string::npos) throw std::runtime_error("invalid TOML assignment at line " + std::to_string(line_number));
    std::string key = section + '.' + trim(line.substr(0, equal));
    std::string value = trim(line.substr(equal + 1));
    if (key == "runtime.mode") {
      auto mode = unquote(value);
      if (mode == "observe") c.mode = Mode::Observe;
      else if (mode == "plan") c.mode = Mode::Plan;
      else if (mode == "active") c.mode = Mode::Active;
      else throw std::runtime_error("mode must be observe, plan, or active");
    } else if (key == "resources.cpus") c.cpus = parse_cpu_list(unquote(value));
    else if (key == "resources.calibration_path") c.calibration_path = unquote(value);
    else if (key == "runtime.log_directory") c.log_directory = unquote(value);
    else if (key == "runtime.socket_path") c.socket_path = unquote(value);
    else if (key == "runtime.sample_interval_seconds") c.sample_interval_seconds = std::stoi(value);
    else if (key == "runtime.graph_horizon_seconds") c.graph_horizon_seconds = std::stoi(value);
    else if (key == "runtime.solve_interval_seconds") c.solve_interval_seconds = std::stoi(value);
    else if (key == "runtime.minimum_confidence") c.minimum_confidence = std::stod(value);
    else if (key == "runtime.proposal_confirmations") c.proposal_confirmations = std::stoi(value);
    else if (key == "runtime.initial_proposal_confirmations") c.initial_proposal_confirmations = std::stoi(value);
    else if (key == "runtime.minimum_dwell_seconds") c.minimum_dwell_seconds = std::stoi(value);
    else if (key == "runtime.maximum_migrated_threads_ratio" ||
             key == "runtime.maximum_migrated_active_threads_ratio")
      c.maximum_migrated_threads_ratio = std::stod(value);
    else if (key == "runtime.initial_migrated_threads_ratio")
      c.initial_migrated_threads_ratio = std::stod(value);
    else if (key == "runtime.collector_failure_restore_seconds") c.collector_failure_restore_seconds = std::stoi(value);
    else if (key == "runtime.solver") c.solver = unquote(value);
    else if (key == "runtime.affinity_granularity") c.affinity_granularity = unquote(value);
    else if (key == "runtime.family_minimum_demand") c.family_minimum_demand = std::stod(value);
    else if (key == "runtime.family_minimum_internal_relation") c.family_minimum_internal_relation = std::stod(value);
    else if (key == "runtime.family_minimum_self_containment") c.family_minimum_self_containment = std::stod(value);
    else if (key == "runtime.family_minimum_relative_internal") c.family_minimum_relative_internal = std::stod(value);
    else if (key == "runtime.domain_merge_ratio") c.domain_merge_ratio = std::stod(value);
    else if (key == "runtime.family_edges_per_family") c.family_edges_per_family = std::stoi(value);
    else if (key == "runtime.family_stability_confirmations") c.family_stability_confirmations = std::stoi(value);
    else if (key == "runtime.domain_stability_confirmations") c.domain_stability_confirmations = std::stoi(value);
    else if (key == "runtime.domain_plan_confirmations") c.domain_plan_confirmations = std::stoi(value);
    else if (key == "runtime.maximum_threads_per_domain") c.maximum_threads_per_domain = std::stoi(value);
    else if (key == "runtime.domain_capacity_ratio") c.domain_capacity_ratio = std::stod(value);
    else if (key == "runtime.domain_expand_ratio") c.domain_expand_ratio = std::stod(value);
    else if (key == "runtime.domain_expand_confirmations") c.domain_expand_confirmations = std::stoi(value);
    else if (key == "runtime.domain_shrink_ratio") c.domain_shrink_ratio = std::stod(value);
    else if (key == "runtime.domain_shrink_confirmations") c.domain_shrink_confirmations = std::stoi(value);
    else if (key == "runtime.domain_minimum_dwell_seconds") c.domain_minimum_dwell_seconds = std::stoi(value);
    else if (key == "runtime.initial_node_passes") c.initial_node_passes = std::stoi(value);
    else if (key == "runtime.initial_node_thread_slack_ratio") c.initial_node_thread_slack_ratio = std::stod(value);
    else if (key == "runtime.candidate_multiplier") c.candidate_multiplier = std::stoi(value);
    else if (key == "runtime.candidate_hard_limit") c.candidate_hard_limit = std::stoi(value);
    else if (key == "runtime.rotating_scan_size") c.rotating_scan_size = std::stoi(value);
    else if (key == "runtime.demand_dirty_threshold") c.demand_dirty_threshold = std::stod(value);
    else if (key == "runtime.edge_dirty_absolute_threshold") c.edge_dirty_absolute_threshold = std::stod(value);
    else if (key == "runtime.edge_dirty_relative_threshold") c.edge_dirty_relative_threshold = std::stod(value);
    else if (key == "runtime.minimum_relative_gain") c.minimum_relative_gain = std::stod(value);
    else if (key == "runtime.maximum_threads_per_cpu") c.maximum_threads_per_cpu = std::stoi(value);
    else if (key == "runtime.thread_slot_slack") c.thread_slot_slack = std::stoi(value);
    else if (key == "runtime.future_demand_floor") c.future_demand_floor = std::stod(value);
    else if (key == "runtime.group_peak_demand_ratio") c.group_peak_demand_ratio = std::stod(value);
    else if (key == "runtime.group_peak_demand_cap") c.group_peak_demand_cap = std::stod(value);
    else if (key == "runtime.group_peak_decay") c.group_peak_decay = std::stod(value);
    else if (key == "runtime.node_balance_threshold") c.node_balance_threshold = std::stod(value);
    else if (key == "runtime.hotspot_edges_per_thread") c.hotspot_edges_per_thread = std::stoi(value);
    else if (key == "runtime.hotspot_edge_quantile") c.hotspot_edge_quantile = std::stod(value);
    else if (key == "runtime.hotspot_component_boost") c.hotspot_component_boost = std::stod(value);
    else if (key == "runtime.maximum_managed_threads") c.maximum_managed_threads = std::stoi(value);
    else if (key == "runtime.managed_thread_hysteresis_ratio") c.managed_thread_hysteresis_ratio = std::stod(value);
    else if (key == "runtime.hotspot_replan_growth_ratio") c.hotspot_replan_growth_ratio = std::stod(value);
    else if (key == "runtime.hotspot_replan_min_threads") c.hotspot_replan_min_threads = std::stoi(value);
    else if (key == "runtime.hotspot_stability_threshold") c.hotspot_stability_threshold = std::stod(value);
    else if (key == "runtime.hotspot_stability_confirmations") c.hotspot_stability_confirmations = std::stoi(value);
    else if (key == "runtime.active_demand_threshold") c.active_demand_threshold = std::stod(value);
    else if (key == "runtime.inactive_demand_threshold") c.inactive_demand_threshold = std::stod(value);
    else if (key == "collector.required") c.bpf_required = boolean(value);
    else if (key == "collector.pthread_uprobe") c.pthread_uprobe = boolean(value);
    else if (key == "calibration.id") c.relationship_calibration_id = unquote(value);
    else if (key == "calibration.activity_log_p95") c.activity_log_p95 = std::stod(value);
    else if (key == "calibration.sync_log_p95") c.sync_log_p95 = std::stod(value);
    else if (key == "calibration.share_log_p95") c.share_log_p95 = std::stod(value);
    else throw std::runtime_error("unknown TOML key '" + key + "' at line " + std::to_string(line_number));
  }
  if (c.cpus.empty()) throw std::runtime_error("resources.cpus is required");
  if (c.sample_interval_seconds < 1 || c.graph_horizon_seconds < c.sample_interval_seconds ||
      c.solve_interval_seconds < c.sample_interval_seconds || c.minimum_confidence < 0 ||
      c.minimum_confidence > 1 || c.proposal_confirmations < 1 ||
      c.initial_proposal_confirmations < 1 ||
      c.minimum_dwell_seconds < 0 || c.maximum_migrated_threads_ratio < 0 ||
      c.maximum_migrated_threads_ratio > 1 || c.initial_migrated_threads_ratio <= 0 ||
      c.initial_migrated_threads_ratio > 1 || c.collector_failure_restore_seconds < 1 ||
      (c.solver != "incremental-hotspot-v1" && c.solver != "numa-domain-v1") ||
      (c.affinity_granularity != "singleton_cpu" &&
       c.affinity_granularity != "numa_node_mask") ||
      (c.solver == "numa-domain-v1" &&
       c.affinity_granularity != "numa_node_mask") ||
      c.family_minimum_demand < 0 ||
      c.family_minimum_internal_relation < 0 ||
      c.family_minimum_self_containment < 0 ||
      c.family_minimum_self_containment > 1 ||
      c.family_minimum_relative_internal < 0 ||
      c.family_minimum_relative_internal > 1 ||
      c.domain_merge_ratio < 0 || c.family_edges_per_family < 1 ||
      c.family_stability_confirmations < 1 ||
      c.domain_stability_confirmations < 1 || c.domain_plan_confirmations < 1 ||
      c.maximum_threads_per_domain < 1 || c.domain_capacity_ratio <= 0 ||
      c.domain_capacity_ratio > 1 || c.domain_expand_ratio <= 0 ||
      c.domain_expand_ratio > 1 || c.domain_expand_confirmations < 1 ||
      c.domain_shrink_ratio < 0 || c.domain_shrink_ratio >= c.domain_expand_ratio ||
      c.domain_shrink_confirmations < 1 || c.domain_minimum_dwell_seconds < 0 ||
      c.initial_node_passes < 1 ||
      c.initial_node_passes > 8 || c.initial_node_thread_slack_ratio < 0 ||
      c.initial_node_thread_slack_ratio > 1 || c.candidate_multiplier < 1 ||
      c.candidate_hard_limit < 1 || c.rotating_scan_size < 1 ||
      c.demand_dirty_threshold < 0 || c.edge_dirty_absolute_threshold < 0 ||
      c.edge_dirty_relative_threshold < 0 || c.minimum_relative_gain < 0 ||
      c.minimum_relative_gain > 1 ||
      c.maximum_threads_per_cpu < 0 || c.thread_slot_slack < 0 ||
      c.future_demand_floor < 0 || c.future_demand_floor > 1 ||
      c.group_peak_demand_ratio < 0 || c.group_peak_demand_ratio > 1 ||
      c.group_peak_demand_cap < 0 || c.group_peak_demand_cap > 1 ||
      c.group_peak_decay < 0 || c.group_peak_decay > 1 ||
      c.node_balance_threshold < 0 || c.node_balance_threshold > 1 ||
      c.hotspot_edges_per_thread < 1 || c.hotspot_edge_quantile < 0 ||
      c.hotspot_edge_quantile > 1 || c.hotspot_component_boost < 0 ||
      c.maximum_managed_threads < 0 ||
      c.managed_thread_hysteresis_ratio < 0 ||
      c.hotspot_replan_growth_ratio < 0 || c.hotspot_replan_growth_ratio > 1 ||
      c.hotspot_replan_min_threads < 1 ||
      c.hotspot_stability_threshold < 0 || c.hotspot_stability_threshold > 1 ||
      c.hotspot_stability_confirmations < 1 ||
      c.active_demand_threshold <= 0 || c.active_demand_threshold > 1 ||
      c.inactive_demand_threshold < 0 ||
      c.inactive_demand_threshold >= c.active_demand_threshold ||
      c.relationship_calibration_id.empty() || c.activity_log_p95 <= 0 || c.sync_log_p95 <= 0 || c.share_log_p95 <= 0)
    throw std::runtime_error("configuration value outside supported range");
  return c;
}
} // namespace affinitygraph
