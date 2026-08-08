#include "affinitygraph/core.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace affinitygraph {
namespace {

std::vector<int> intersect_masks(const std::vector<int> &left,
                                 const std::vector<int> &right) {
  std::vector<int> a = left, b = right, result;
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                        std::back_inserter(result));
  return result;
}

std::string domain_id(const std::vector<std::string> &families) {
  std::ostringstream out;
  for (size_t index = 0; index < families.size(); ++index) {
    if (index) out << '+';
    out << families[index];
  }
  return out.str();
}

std::string plan_signature(const std::vector<NumaDomain> &domains) {
  std::ostringstream out;
  for (const auto &domain : domains) {
    out << domain.id << ':';
    for (int node : domain.target_nodes) out << node << ',';
    out << ':' << (domain.valid ? "valid" : domain.invalid_reason) << ';';
  }
  return out.str();
}

std::vector<int> mask_for_nodes(const HardwareGraph &hardware,
                                const std::vector<int> &nodes) {
  std::vector<int> result;
  for (int node : nodes) {
    auto cpus = hardware.cpus_in_node(node);
    result.insert(result.end(), cpus.begin(), cpus.end());
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::set<std::pair<std::string, std::string>> select_family_edges(
    const std::map<std::pair<std::string, std::string>, double> &cross,
    size_t edges_per_family) {
  using Candidate = std::pair<double, std::pair<std::string, std::string>>;
  std::map<std::string, std::vector<Candidate>> incident;
  for (const auto &[pair, weight] : cross) {
    incident[pair.first].push_back({weight, pair});
    incident[pair.second].push_back({weight, pair});
  }
  std::set<std::pair<std::string, std::string>> selected;
  for (auto &[_, candidates] : incident) {
    std::sort(candidates.begin(), candidates.end(), [](const auto &left,
                                                        const auto &right) {
      if (left.first != right.first) return left.first > right.first;
      return left.second < right.second;
    });
    const size_t limit = std::min(edges_per_family, candidates.size());
    for (size_t index = 0; index < limit; ++index)
      selected.insert(candidates[index].second);
  }
  return selected;
}

bool cpu_in_nodes(const HardwareGraph &hardware, int cpu,
                  const std::vector<int> &nodes) {
  auto found = std::find_if(hardware.cpus.begin(), hardware.cpus.end(),
                            [&](const Cpu &value) { return value.id == cpu; });
  return found != hardware.cpus.end() &&
         std::find(nodes.begin(), nodes.end(), found->node) != nodes.end();
}

std::vector<int> choose_nodes(const HardwareGraph &hardware, double demand,
                              double capacity_ratio,
                              const std::vector<ThreadDemand> &members,
                              const std::vector<ThreadDemand> &all_threads,
                              const std::vector<std::pair<
                                  std::vector<std::string>, std::vector<int>>>
                                  &placed_domains,
                              const std::vector<std::string> &families,
                              const std::map<std::pair<std::string, std::string>,
                                             double> &cross) {
  auto nodes = hardware.nodes();
  if (nodes.empty()) return {};
  std::vector<int> best;
  double best_relation_latency = 0;
  double best_background_demand = 0;
  int best_migrations = 0;
  std::set<int> member_tids;
  for (const auto &member : members) member_tids.insert(member.identity.tid);
  std::vector<int> candidate;
  std::function<void(size_t)> visit = [&](size_t index) {
    if (index == nodes.size()) {
      if (candidate.empty()) return;
      size_t cpu_count = mask_for_nodes(hardware, candidate).size();
      if (static_cast<double>(cpu_count) * capacity_ratio + 1e-12 < demand)
        return;
      int migrations = 0;
      for (const auto &thread : members)
        if (!cpu_in_nodes(hardware, thread.current_cpu, candidate)) ++migrations;
      double background_demand = 0;
      for (const auto &thread : all_threads)
        if (!member_tids.contains(thread.identity.tid) &&
            cpu_in_nodes(hardware, thread.current_cpu, candidate))
          background_demand += thread.demand;
      double relation_latency = 0;
      for (const auto &[other_families, other_nodes] : placed_domains) {
        double weight = 0;
        for (const auto &family : families)
          for (const auto &other : other_families) {
            auto found = cross.find(std::minmax(family, other));
            if (found != cross.end()) weight += found->second;
          }
        double distance = 1e30;
        for (int node : candidate)
          for (int other : other_nodes) {
            auto found = hardware.node_distance.find({node, other});
            distance = std::min(distance, found == hardware.node_distance.end()
                                              ? 1e6
                                              : found->second);
          }
        relation_latency += weight * (distance == 1e30 ? 0 : distance);
      }
      if (best.empty() || candidate.size() < best.size() ||
          (candidate.size() == best.size() &&
           relation_latency < best_relation_latency) ||
          (candidate.size() == best.size() &&
           relation_latency == best_relation_latency &&
           background_demand < best_background_demand) ||
          (candidate.size() == best.size() &&
           relation_latency == best_relation_latency &&
           background_demand == best_background_demand &&
           migrations < best_migrations) ||
          (candidate.size() == best.size() &&
           relation_latency == best_relation_latency &&
           background_demand == best_background_demand &&
           migrations == best_migrations && candidate < best)) {
        best = candidate;
        best_relation_latency = relation_latency;
        best_background_demand = background_demand;
        best_migrations = migrations;
      }
      return;
    }
    candidate.push_back(nodes[index]);
    visit(index + 1);
    candidate.pop_back();
    visit(index + 1);
  };
  visit(0);
  return best;
}

} // namespace

NumaDomainProposal NumaDomainSolver::propose(
    const HardwareGraph &hardware, const std::vector<ThreadDemand> &threads,
    const std::vector<RelationEdge> &edges,
    const std::map<int, std::vector<int>> &allowed_masks,
    const NumaDomainOptions &options, uint64_t now_ns) {
  if (outstanding_) throw std::logic_error("outstanding NUMA domain proposal");
  NumaDomainProposal proposal;
  proposal.id = next_proposal_id_++;

  std::map<int, ThreadDemand> by_tid;
  std::map<std::string, std::vector<ThreadDemand>> by_family;
  for (const auto &thread : threads) {
    by_tid[thread.identity.tid] = thread;
    by_family[thread.group].push_back(thread);
  }
  std::map<std::string, double> internal, external;
  std::map<std::pair<std::string, std::string>, double> cross;
  for (const auto &edge : edges) {
    auto from = by_tid.find(edge.from_tid), to = by_tid.find(edge.to_tid);
    if (from == by_tid.end() || to == by_tid.end()) continue;
    const auto &a = from->second.group;
    const auto &b = to->second.group;
    if (a == b) {
      internal[a] += edge.score;
    } else {
      external[a] += edge.score;
      external[b] += edge.score;
      cross[std::minmax(a, b)] += edge.score;
    }
  }
  double maximum_internal = 0;
  for (const auto &[_, value] : internal)
    maximum_internal = std::max(maximum_internal, value);

  std::set<std::string> observed_families;
  std::map<std::string, FamilyMetric> metrics;
  for (const auto &[name, members] : by_family) {
    observed_families.insert(name);
    FamilyMetric metric;
    metric.name = name;
    metric.thread_count = members.size();
    for (const auto &member : members) metric.demand += member.demand;
    metric.internal_relation = internal[name];
    metric.external_relation = external[name];
    double total = metric.internal_relation + metric.external_relation;
    metric.self_containment = total > 0 ? metric.internal_relation / total : 0;
    metric.relative_internal = maximum_internal > 0
                                   ? metric.internal_relation / maximum_internal
                                   : 0;
    bool cohesive =
        metric.demand >= options.family_minimum_demand &&
        metric.internal_relation >= options.family_minimum_internal_relation &&
        metric.self_containment >= options.family_minimum_self_containment &&
        metric.relative_internal >= options.family_minimum_relative_internal;
    metric.confirmation = cohesive ? ++family_confirmations_[name] : 0;
    if (!cohesive) family_confirmations_[name] = 0;
    metric.cohesive_anchor =
        metric.confirmation >= options.family_stability_confirmations;
    metrics[name] = metric;
  }
  for (auto &[name, confirmation] : family_confirmations_)
    if (!observed_families.contains(name)) confirmation = 0;

  // Keep all TID evidence through family aggregation, then bound only the
  // family graph. Self-containment and relative-internal evidence describe a
  // cohesive singleton anchor; they are deliberately not prerequisites for a
  // stable cross-family seed.
  const auto selected_cross =
      select_family_edges(cross, options.family_edges_per_family);
  std::set<std::pair<std::string, std::string>> evaluated_merges;
  std::vector<std::pair<std::string, std::string>> confirmed_seeds;
  for (const auto &pair : selected_cross) {
    const double weight = cross.at(pair);
    const auto &left = metrics.at(pair.first);
    const auto &right = metrics.at(pair.second);
    const bool endpoints_eligible =
        left.demand >= options.family_minimum_demand &&
        right.demand >= options.family_minimum_demand &&
        left.internal_relation >= options.family_minimum_internal_relation &&
        right.internal_relation >= options.family_minimum_internal_relation;
    evaluated_merges.insert(pair);
    double denominator = std::min(internal[pair.first], internal[pair.second]);
    bool qualifies = endpoints_eligible && denominator > 0 &&
                     weight / denominator >= options.domain_merge_ratio;
    int confirmation = qualifies ? ++merge_confirmations_[pair] : 0;
    if (!qualifies) merge_confirmations_[pair] = 0;
    metrics[pair.first].seed_confirmation =
        std::max(metrics[pair.first].seed_confirmation, confirmation);
    metrics[pair.second].seed_confirmation =
        std::max(metrics[pair.second].seed_confirmation, confirmation);
    if (confirmation >= options.domain_stability_confirmations) {
      metrics[pair.first].cross_seed = true;
      metrics[pair.second].cross_seed = true;
      confirmed_seeds.push_back(pair);
    }
  }
  for (auto &[pair, confirmation] : merge_confirmations_)
    if (!evaluated_merges.contains(pair)) confirmation = 0;

  std::set<std::string> anchors;
  for (auto &[name, metric] : metrics) {
    metric.anchor = metric.cohesive_anchor || metric.cross_seed;
    if (metric.anchor) anchors.insert(name);
    proposal.families.push_back(metric);
  }
  std::map<std::string, std::string> parent;
  for (const auto &name : anchors) parent[name] = name;
  std::function<std::string(const std::string &)> root = [&](const std::string &name) {
    if (parent[name] == name) return name;
    return parent[name] = root(parent[name]);
  };
  for (const auto &pair : confirmed_seeds) {
    auto a = root(pair.first), b = root(pair.second);
    if (a != b) parent[std::max(a, b)] = std::min(a, b);
  }
  std::map<std::string, std::vector<std::string>> components;
  for (const auto &name : anchors) components[root(name)].push_back(name);

  std::vector<std::pair<std::vector<std::string>, std::vector<int>>>
      placed_domains;
  for (auto &[_, family_names] : components) {
    std::sort(family_names.begin(), family_names.end());
    NumaDomain domain;
    domain.id = domain_id(family_names);
    domain.families = family_names;
    std::vector<ThreadDemand> members;
    for (const auto &name : family_names)
      for (const auto &member : by_family[name]) {
        members.push_back(member);
        domain.tids.push_back(member.identity.tid);
        domain.demand += member.demand;
      }
    std::sort(domain.tids.begin(), domain.tids.end());
    if (domain.tids.size() > options.maximum_threads_per_domain) {
      domain.valid = false;
      domain.invalid_reason = "maximum_threads_per_domain_exceeded";
    }

    auto required_nodes = choose_nodes(
        hardware, domain.demand, options.capacity_ratio, members, threads,
        placed_domains, family_names, cross);
    if (required_nodes.empty()) {
      domain.valid = false;
      domain.invalid_reason = "insufficient_node_capacity";
    }
    auto previous = domain_nodes_.find(domain.id);
    if (previous != domain_nodes_.end() && !previous->second.empty() &&
        domain.valid) {
      auto previous_mask = mask_for_nodes(hardware, previous->second);
      double utilization = previous_mask.empty()
                               ? 1.0
                               : domain.demand / previous_mask.size();
      bool expand = required_nodes.size() > previous->second.size() &&
                    utilization > options.expand_ratio;
      bool shrink = required_nodes.size() < previous->second.size() &&
                    utilization < options.shrink_ratio &&
                    now_ns - last_changed_ns_[domain.id] >=
                        options.minimum_dwell_ns;
      expand_confirmations_[domain.id] =
          expand ? expand_confirmations_[domain.id] + 1 : 0;
      shrink_confirmations_[domain.id] =
          shrink ? shrink_confirmations_[domain.id] + 1 : 0;
      if (!(expand && expand_confirmations_[domain.id] >=
                         options.expand_confirmations) &&
          !(shrink && shrink_confirmations_[domain.id] >=
                         options.shrink_confirmations))
        required_nodes = previous->second;
    }
    domain.target_nodes = required_nodes;
    placed_domains.push_back({family_names, required_nodes});
    domain.target_mask = mask_for_nodes(hardware, required_nodes);
    for (const auto &member : members) {
      auto allowed = allowed_masks.find(member.identity.tid);
      auto target = allowed == allowed_masks.end()
                        ? domain.target_mask
                        : intersect_masks(domain.target_mask, allowed->second);
      if (target.empty()) {
        domain.valid = false;
        domain.invalid_reason = "empty_application_mask_intersection";
        break;
      }
      proposal.delta.tid_to_mask[member.identity.tid] = target;
    }
    if (!domain.valid) {
      proposal.valid = false;
      proposal.invalid_reason = domain.id + ":" + domain.invalid_reason;
      proposal.delta.tid_to_mask.clear();
    }
    proposal.domains.push_back(std::move(domain));
  }

  std::sort(proposal.domains.begin(), proposal.domains.end(),
            [](const auto &a, const auto &b) { return a.id < b.id; });
  if (!proposal.valid) proposal.delta.tid_to_mask.clear();
  proposal.planned_masks = proposal.delta.tid_to_mask;
  std::set<int> next_tids;
  for (const auto &[tid, _] : proposal.planned_masks) next_tids.insert(tid);
  for (const auto &[tid, _] : placement_)
    if (!next_tids.contains(tid)) proposal.released_tids.insert(tid);

  const std::string signature = plan_signature(proposal.domains);
  if (signature == pending_signature_)
    ++global_plan_confirmation_;
  else {
    pending_signature_ = signature;
    global_plan_confirmation_ = 1;
  }
  bool all_confirmed = !proposal.domains.empty() &&
                       global_plan_confirmation_ >= options.plan_confirmations;
  for (auto &domain : proposal.domains) {
    domain.confirmation = global_plan_confirmation_;
  }
  proposal.ready = proposal.valid &&
                   (all_confirmed ||
                    (proposal.domains.empty() && !proposal.released_tids.empty()));

  if (proposal.ready) {
    for (const auto &[tid, target] : proposal.delta.tid_to_mask) {
      auto current = placement_.find(tid);
      if (current != placement_.end() && current->second == target) continue;
      const auto &thread = by_tid.at(tid);
      std::vector<int> from;
      auto allowed = allowed_masks.find(tid);
      if (allowed != allowed_masks.end()) from = allowed->second;
      if (current == placement_.end() && from == target) {
        proposal.inherited_tids.insert(tid);
        continue;
      }
      const auto &domain = *std::find_if(
          proposal.domains.begin(), proposal.domains.end(),
          [&](const NumaDomain &value) {
            return std::find(value.tids.begin(), value.tids.end(), tid) !=
                   value.tids.end();
          });
      proposal.actions.push_back(
          {thread.identity, from, target, domain.target_nodes,
           std::find(target.begin(), target.end(), thread.current_cpu) ==
               target.end()});
    }
    std::map<int, std::vector<int>> changed;
    for (const auto &action : proposal.actions)
      changed[action.identity.tid] = action.target_mask;
    proposal.delta.tid_to_mask = std::move(changed);
  } else {
    proposal.delta.tid_to_mask.clear();
  }
  families_ = proposal.families;
  outstanding_ = proposal;
  return proposal;
}

void NumaDomainSolver::commit(const NumaDomainProposal &proposal,
                              uint64_t now_ns,
                              const std::set<int> &committed_tids) {
  if (!outstanding_ || outstanding_->id != proposal.id)
    throw std::logic_error("NUMA domain proposal is not outstanding");
  for (int tid : proposal.released_tids) placement_.erase(tid);
  std::set<int> action_tids;
  for (const auto &action : proposal.actions)
    action_tids.insert(action.identity.tid);
  for (const auto &[tid, mask] : proposal.planned_masks)
    if (placement_.contains(tid) || proposal.inherited_tids.contains(tid) ||
        (action_tids.contains(tid) &&
         (committed_tids.empty() || committed_tids.contains(tid))))
      placement_[tid] = mask;
  for (const auto &action : proposal.actions) {
    if (!committed_tids.empty() && !committed_tids.contains(action.identity.tid))
      continue;
    placement_[action.identity.tid] = action.target_mask;
  }
  for (const auto &domain : proposal.domains) {
    if (domain_nodes_[domain.id] != domain.target_nodes)
      last_changed_ns_[domain.id] = now_ns;
    domain_nodes_[domain.id] = domain.target_nodes;
  }
  domains_ = proposal.domains;
  ++generation_;
  outstanding_.reset();
}

void NumaDomainSolver::discard(const NumaDomainProposal &proposal) {
  if (outstanding_ && outstanding_->id == proposal.id) outstanding_.reset();
}

void NumaDomainSolver::reset() {
  *this = NumaDomainSolver{};
}

void NumaDomainSolver::remove_thread(int tid) { placement_.erase(tid); }

} // namespace affinitygraph
