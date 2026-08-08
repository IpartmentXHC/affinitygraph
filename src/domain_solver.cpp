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

struct NodeChoice {
  std::vector<int> nodes;
  size_t online_cpu_count = 0;
  double capacity_limit = 0;
  double capacity_headroom = 0;
  double relation_latency = 0;
  double background_demand = 0;
  int initial_migrations = 0;
};

// 计算一个 node 集的完整诊断值。该函数同时服务候选排序和最终日志，确保
// 日志里的 background demand、首次迁移数与 solver 真正比较的数值一致。
NodeChoice evaluate_nodes(
    const HardwareGraph &hardware, const std::vector<int> &nodes,
    double demand, double capacity_ratio,
    const std::vector<ThreadDemand> &members,
    const std::vector<ThreadDemand> &all_threads,
    const std::vector<std::pair<std::vector<std::string>, std::vector<int>>>
        &placed_domains,
    const std::vector<std::string> &families,
    const std::map<std::pair<std::string, std::string>, double> &cross) {
  NodeChoice choice;
  choice.nodes = nodes;
  choice.online_cpu_count = mask_for_nodes(hardware, nodes).size();
  choice.capacity_limit = choice.online_cpu_count * capacity_ratio;
  choice.capacity_headroom = choice.capacity_limit - demand;
  std::set<int> member_tids;
  for (const auto &member : members) member_tids.insert(member.identity.tid);
  for (const auto &thread : members)
    if (!cpu_in_nodes(hardware, thread.current_cpu, nodes))
      ++choice.initial_migrations;
  for (const auto &thread : all_threads)
    if (!member_tids.contains(thread.identity.tid) &&
        cpu_in_nodes(hardware, thread.current_cpu, nodes))
      choice.background_demand += thread.demand;
  for (const auto &[other_families, other_nodes] : placed_domains) {
    double weight = 0;
    for (const auto &family : families)
      for (const auto &other : other_families) {
        auto found = cross.find(std::minmax(family, other));
        if (found != cross.end()) weight += found->second;
      }
    double distance = 1e30;
    for (int node : nodes)
      for (int other : other_nodes) {
        auto found = hardware.node_distance.find({node, other});
        distance = std::min(distance, found == hardware.node_distance.end()
                                          ? 1e6
                                          : found->second);
      }
    choice.relation_latency += weight * (distance == 1e30 ? 0 : distance);
  }
  return choice;
}

// 枚举 NUMA node 子集并按设计中的字典序选最优解：最少 node、跨 domain
// 关系延迟、非受管 demand、首次迁移数、node ID。这里不做 CPU singleton
// 分配；输出始终是完整 node mask。
NodeChoice choose_nodes(
    const HardwareGraph &hardware, double demand, double capacity_ratio,
    const std::vector<ThreadDemand> &members,
    const std::vector<ThreadDemand> &all_threads,
    const std::vector<std::pair<std::vector<std::string>, std::vector<int>>>
        &placed_domains,
    const std::vector<std::string> &families,
    const std::map<std::pair<std::string, std::string>, double> &cross) {
  auto nodes = hardware.nodes();
  if (nodes.empty()) return {};
  NodeChoice best;
  std::vector<int> candidate;
  std::function<void(size_t)> visit = [&](size_t index) {
    if (index == nodes.size()) {
      if (candidate.empty()) return;
      auto current = evaluate_nodes(hardware, candidate, demand, capacity_ratio,
                                    members, all_threads, placed_domains,
                                    families, cross);
      if (current.capacity_headroom + 1e-12 < 0) return;
      if (best.nodes.empty() || current.nodes.size() < best.nodes.size() ||
          (current.nodes.size() == best.nodes.size() &&
           current.relation_latency < best.relation_latency) ||
          (current.nodes.size() == best.nodes.size() &&
           current.relation_latency == best.relation_latency &&
           current.background_demand < best.background_demand) ||
          (current.nodes.size() == best.nodes.size() &&
           current.relation_latency == best.relation_latency &&
           current.background_demand == best.background_demand &&
           current.initial_migrations < best.initial_migrations) ||
          (current.nodes.size() == best.nodes.size() &&
           current.relation_latency == best.relation_latency &&
           current.background_demand == best.background_demand &&
           current.initial_migrations == best.initial_migrations &&
           current.nodes < best.nodes)) {
        best = std::move(current);
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

  // 输入阶段：ThreadDemand 已携带 placement family。这里先完成全量 family
  // 聚合，任何 top-K 都发生在聚合之后。total_* 和 pair 数量会原样进入
  // selector_output，便于确认一次 solve 实际看到了多少证据。
  std::map<int, ThreadDemand> by_tid;
  std::map<std::string, std::vector<ThreadDemand>> by_family;
  for (const auto &thread : threads) {
    by_tid[thread.identity.tid] = thread;
    by_family[thread.group].push_back(thread);
    proposal.total_demand += thread.demand;
  }
  proposal.input_family_count = by_family.size();
  std::map<std::string, double> internal, external;
  std::map<std::pair<std::string, std::string>, double> cross;
  for (const auto &edge : edges) {
    proposal.total_relation += edge.score;
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
  proposal.input_cross_family_pair_count = cross.size();
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
    metric.demand_eligible =
        metric.demand >= options.family_minimum_demand;
    metric.internal_relation_eligible =
        metric.internal_relation >= options.family_minimum_internal_relation;
    metric.self_containment_eligible =
        metric.self_containment >= options.family_minimum_self_containment;
    metric.relative_internal_eligible =
        metric.relative_internal >= options.family_minimum_relative_internal;
    metric.cohesive_eligible =
        metric.demand_eligible && metric.internal_relation_eligible &&
        metric.self_containment_eligible &&
        metric.relative_internal_eligible;
    bool cohesive = metric.cohesive_eligible;
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
  proposal.selected_family_pair_count = selected_cross.size();
  std::set<std::pair<std::string, std::string>> evaluated_merges;
  std::set<std::pair<std::string, std::string>> qualifying_merges;
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
    if (qualifies) {
      qualifying_merges.insert(pair);
      // pair 证据偶尔会因 heavy-hitter 边界或短窗口 demand 波动消失。保留与
      // 完整 plan 确认等长的 acquisition grace，只用于阻止 singleton 抢先
      // commit；merge confirmation 本身仍严格要求连续窗口，不降低稳定门槛。
      merge_pending_grace_[pair] = options.plan_confirmations;
    }
    int confirmation = qualifies ? ++merge_confirmations_[pair] : 0;
    if (!qualifies) merge_confirmations_[pair] = 0;
    FamilyPairMetric pair_metric;
    pair_metric.left = pair.first;
    pair_metric.right = pair.second;
    pair_metric.cross_relation = weight;
    pair_metric.denominator = denominator;
    pair_metric.merge_ratio = denominator > 0 ? weight / denominator : 0;
    pair_metric.endpoints_eligible = endpoints_eligible;
    pair_metric.qualifies = qualifies;
    pair_metric.confirmation = confirmation;
    pair_metric.confirmed =
        confirmation >= options.domain_stability_confirmations;
    proposal.family_pairs.push_back(std::move(pair_metric));
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
  for (auto &[pair, grace] : merge_pending_grace_) {
    if (!qualifying_merges.contains(pair) && grace > 0) --grace;
    const auto confirmation = merge_confirmations_[pair];
    if (grace > 0 && confirmation < options.domain_stability_confirmations &&
        metrics.contains(pair.first) && metrics.contains(pair.second)) {
      metrics[pair.first].cross_pending = true;
      metrics[pair.second].cross_pending = true;
    }
  }

  std::set<std::string> anchors;
  // 输出 family 集合是两条证据路径的并集。cohesive_anchor 需要 S_g/P_g，
  // cross_seed 则只依赖双方绝对证据和稳定 X_gh；因此 self-containment 不是
  // 全局硬门槛，外部-only family 又不会被误纳入。若强跨组关系正在确认，
  // cohesive anchor 暂不单独输出；pair 一旦稳定便由 cross_seed 原子输出双方。
  for (auto &[name, metric] : metrics) {
    metric.anchor = metric.cross_seed ||
                    (metric.cohesive_anchor && !metric.cross_pending);
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

    // Capacity planner 先给出当前窗口的无状态最优解，再由 lifecycle 的
    // expand/shrink confirmation 与 dwell 决定是否采用。domain.node_decision
    // 明确记录最终是 initial/stable/held/expanded/shrunk。
    auto node_choice = choose_nodes(
        hardware, domain.demand, options.capacity_ratio, members, threads,
        placed_domains, family_names, cross);
    auto required_nodes = node_choice.nodes;
    if (required_nodes.empty()) {
      domain.valid = false;
      domain.invalid_reason = "insufficient_node_capacity";
    }
    auto previous = domain_nodes_.find(domain.id);
    if (previous != domain_nodes_.end() && !previous->second.empty() &&
        domain.valid) {
      domain.previous_nodes = previous->second;
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
      const bool expand_confirmed =
          expand && expand_confirmations_[domain.id] >=
                        options.expand_confirmations;
      const bool shrink_confirmed =
          shrink && shrink_confirmations_[domain.id] >=
                        options.shrink_confirmations;
      if (expand_confirmed) {
        domain.node_decision = "expanded";
      } else if (shrink_confirmed) {
        domain.node_decision = "shrunk";
      } else {
        if (required_nodes == previous->second)
          domain.node_decision = "stable";
        else if (expand)
          domain.node_decision = "held_expand_pending";
        else if (shrink)
          domain.node_decision = "held_shrink_pending";
        else
          domain.node_decision = "held_existing";
        required_nodes = previous->second;
      }
    } else if (domain.valid) {
      domain.node_decision = "initial";
    }
    domain.expand_confirmation = expand_confirmations_[domain.id];
    domain.shrink_confirmation = shrink_confirmations_[domain.id];
    node_choice = evaluate_nodes(hardware, required_nodes, domain.demand,
                                 options.capacity_ratio, members, threads,
                                 placed_domains, family_names, cross);
    domain.target_nodes = required_nodes;
    domain.online_cpu_count = node_choice.online_cpu_count;
    domain.capacity_limit = node_choice.capacity_limit;
    domain.capacity_headroom = node_choice.capacity_headroom;
    domain.relation_latency = node_choice.relation_latency;
    domain.background_demand = node_choice.background_demand;
    domain.initial_migrations = node_choice.initial_migrations;
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

  // evidence candidate 是经过 family/pair 稳定确认后得到的无状态 domain。
  // 它完整覆盖已提交 domain 时，刷新“最近完整关系证据”；该时间与 node mask
  // 最近变更时间必须分离，否则运行超过 minimum_dwell 后的第一个短暂低谷会
  // 立即拆散一个刚刚仍被证据确认的 domain。
  for (const auto &committed : domains_) {
    const bool complete_evidence = std::any_of(
        proposal.domains.begin(), proposal.domains.end(),
        [&](const auto &candidate) {
          return candidate.valid &&
                 std::all_of(
                     committed.families.begin(), committed.families.end(),
                     [&](const auto &family) {
                       return std::find(candidate.families.begin(),
                                        candidate.families.end(), family) !=
                              candidate.families.end();
                     });
        });
    if (complete_evidence)
      last_confirmed_domain_evidence_ns_[committed.id] = now_ns;
  }

  // 已提交 domain 的 dwell 约束作用于完整 family domain，而不仅是 node 数量。
  // workload phase 切换时关系边和 demand 会短暂消失；若此时把联合 domain
  // 降成 singleton，再在下一阶段重建，会制造一次无意义的 restore/bind 抖动。
  // 只要成员仍在线且应用 mask 仍兼容，在 dwell 内保留原 domain 和 node mask。
  for (const auto &committed : domains_) {
    const auto confirmed =
        last_confirmed_domain_evidence_ns_.find(committed.id);
    if (confirmed == last_confirmed_domain_evidence_ns_.end() ||
        now_ns - confirmed->second >= options.minimum_dwell_ns)
      continue;
    // 已确认候选若完整包含当前 family（例如 singleton 升级为联合 domain），
    // 允许升级；dwell 只阻止丢失 family 的降级或拆分。
    if (std::any_of(
            proposal.domains.begin(), proposal.domains.end(),
            [&](const auto &candidate) {
              return std::all_of(
                  committed.families.begin(), committed.families.end(),
                  [&](const auto &family) {
                    return std::find(candidate.families.begin(),
                                     candidate.families.end(), family) !=
                           candidate.families.end();
                  });
            }))
      continue;

    std::vector<ThreadDemand> members;
    NumaDomain held = committed;
    held.tids.clear();
    held.demand = 0;
    for (const auto &family : held.families) {
      auto observed = by_family.find(family);
      if (observed == by_family.end()) continue;
      for (const auto &member : observed->second) {
        members.push_back(member);
        held.tids.push_back(member.identity.tid);
        held.demand += member.demand;
      }
    }
    if (members.empty()) continue;

    // 同一 family 只能属于一个 domain。dwell 内的已提交联合 domain 优先于
    // 当前窗口因边缺失而产生的重叠 singleton proposal。
    std::set<std::string> held_families(held.families.begin(), held.families.end());
    proposal.domains.erase(
        std::remove_if(
            proposal.domains.begin(), proposal.domains.end(),
            [&](const auto &candidate) {
              return std::any_of(
                  candidate.families.begin(), candidate.families.end(),
                  [&](const auto &family) { return held_families.contains(family); });
            }),
        proposal.domains.end());

    std::sort(held.tids.begin(), held.tids.end());
    held.previous_nodes = held.target_nodes;
    held.node_decision = "held_domain_dwell";
    held.online_cpu_count = held.target_mask.size();
    held.capacity_limit = held.online_cpu_count * options.capacity_ratio;
    held.capacity_headroom = held.capacity_limit - held.demand;
    held.initial_migrations = 0;
    if (held.tids.size() > options.maximum_threads_per_domain) {
      held.valid = false;
      held.invalid_reason = "maximum_threads_per_domain_exceeded";
      proposal.valid = false;
      proposal.invalid_reason = held.id + ":" + held.invalid_reason;
    }
    proposal.domains.push_back(std::move(held));
  }

  std::sort(proposal.domains.begin(), proposal.domains.end(),
            [](const auto &a, const auto &b) { return a.id < b.id; });
  // dwell 可能替换重叠的候选 domain，因此从最终 domain 集合统一重建 mask，
  // 防止被替换 singleton 的 TID 残留在 actuator 输入中。
  proposal.delta.tid_to_mask.clear();
  if (proposal.valid) {
    for (auto &domain : proposal.domains) {
      if (!domain.valid) continue;
      for (int tid : domain.tids) {
        auto thread = by_tid.find(tid);
        if (thread == by_tid.end()) continue;
        auto allowed = allowed_masks.find(tid);
        auto target = allowed == allowed_masks.end()
                          ? domain.target_mask
                          : intersect_masks(domain.target_mask, allowed->second);
        if (target.empty()) {
          domain.valid = false;
          domain.invalid_reason = "empty_application_mask_intersection";
          proposal.valid = false;
          proposal.invalid_reason = domain.id + ":" + domain.invalid_reason;
          proposal.delta.tid_to_mask.clear();
          break;
        }
        proposal.delta.tid_to_mask[tid] = std::move(target);
      }
      if (!proposal.valid) break;
    }
  }
  proposal.planned_masks = proposal.delta.tid_to_mask;
  std::set<int> next_tids;
  for (const auto &[tid, _] : proposal.planned_masks) next_tids.insert(tid);
  for (const auto &[tid, _] : placement_)
    if (!next_tids.contains(tid)) proposal.released_tids.insert(tid);

  const std::string signature = plan_signature(proposal.domains);
  // plan confirmation 针对完整 domain+node signature，而不是单个 family。
  // 只有整个输出连续稳定，active 才会看到 ready=true。
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
    // 只生成真正发生 mask 变化的 action。新线程若已经继承正确 mask，只记入
    // inherited_tids，避免重复 sched_setaffinity 和无意义迁移。
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
  // commit 只接收 actuator 已确认成功的 TID。未提交的 action 不得进入内部
  // placement 状态，否则 status 会声称一个实际上不存在的 affinity mask。
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
  std::set<std::string> previously_active_domains;
  for (const auto &domain : domains_)
    previously_active_domains.insert(domain.id);
  for (const auto &domain : proposal.domains) {
    if (domain_nodes_[domain.id] != domain.target_nodes)
      last_changed_ns_[domain.id] = now_ns;
    // 新 domain 的 commit 来自已确认 plan，因此此刻也是它第一份完整证据。
    // held_domain_dwell 的后续 commit 不会进入此分支，不能借 commit 人为续期。
    if (!previously_active_domains.contains(domain.id))
      last_confirmed_domain_evidence_ns_[domain.id] = now_ns;
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
