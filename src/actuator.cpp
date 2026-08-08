#include "affinitygraph/core.hpp"

#include <cerrno>
#include <sched.h>
#include <unistd.h>

namespace affinitygraph {
std::vector<int> LinuxAffinityBackend::get(int tid) {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(tid, sizeof(set), &set) != 0) return {};
  std::vector<int> result;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) if (CPU_ISSET(cpu, &set)) result.push_back(cpu);
  return result;
}

bool LinuxAffinityBackend::set(int tid, const std::vector<int> &cpus, int &error) {
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int cpu : cpus) {
    if (cpu < 0 || cpu >= CPU_SETSIZE) { error = EINVAL; return false; }
    CPU_SET(cpu, &set);
  }
  if (sched_setaffinity(tid, sizeof(set), &set) != 0) { error = errno; return false; }
  auto actual = get(tid);
  if (actual != cpus) { error = EIO; return false; }
  return true;
}

Actuator::Actuator(AffinityBackend &backend) : backend_(backend) {}

void Actuator::note_application_mask(int tid, const std::vector<int> &cpus) { restore_masks_[tid] = cpus; }

void Actuator::note_inherited_mask(int tid, int parent_tid,
                                   const std::vector<int> &observed_cpus) {
  if (restore_masks_.contains(tid)) return;
  auto parent = restore_masks_.find(parent_tid);
  restore_masks_[tid] = parent == restore_masks_.end() ? observed_cpus : parent->second;
}

namespace {
ApplyResult apply_delta(AffinityBackend &backend,
                        std::map<int, std::vector<int>> &restore_masks,
                        std::set<int> &acted_tids,
                        const std::map<int, std::vector<int>> &assignments,
                        const std::set<int> &live_tids) {
  ApplyResult result;
  result.requested = assignments.size();
  std::vector<int> completed;
  std::map<int, std::vector<int>> previous_masks;
  std::map<int, bool> previously_acted;
  for (const auto &[tid, cpus] : assignments) {
    if (!live_tids.contains(tid)) {
      ++result.vanished;
      result.vanished_tids.insert(tid);
      continue;
    }
    previous_masks[tid] = backend.get(tid);
    previously_acted[tid] = acted_tids.contains(tid);
    if (!restore_masks.contains(tid)) restore_masks[tid] = previous_masks[tid];
    int error = 0;
    if (cpus.empty() || !backend.set(tid, cpus, error)) {
      if (cpus.empty()) error = EINVAL;
      if (error == ESRCH) {
        ++result.vanished;
        result.vanished_tids.insert(tid);
        continue;
      }
      result.error = error;
      for (auto it = completed.rbegin(); it != completed.rend(); ++it) {
        int ignored = 0;
        if (backend.set(*it, previous_masks[*it], ignored) || ignored == ESRCH) {
          ++result.rolled_back;
          if (!previously_acted[*it]) acted_tids.erase(*it);
        } else result.rollback_success = false;
      }
      return result;
    }
    completed.push_back(tid);
    acted_tids.insert(tid);
    ++result.applied;
  }
  result.success = true;
  result.committed = result.applied;
  result.committed_tids.insert(completed.begin(), completed.end());
  return result;
}

std::map<int, std::vector<int>> singleton_masks(
    const std::map<int, int> &assignments) {
  std::map<int, std::vector<int>> result;
  for (const auto &[tid, cpu] : assignments) result[tid] = {cpu};
  return result;
}
} // namespace

ApplyResult Actuator::apply(const Placement &placement,
                            const std::set<int> &live_tids) {
  return apply_delta(backend_, restore_masks_, acted_tids_,
                     singleton_masks(placement.tid_to_cpu), live_tids);
}

ApplyResult Actuator::apply(const PlacementDelta &placement,
                            const std::set<int> &live_tids) {
  auto masks = placement.tid_to_mask;
  for (const auto &[tid, cpu] : placement.tid_to_cpu)
    if (!masks.contains(tid)) masks[tid] = {cpu};
  return apply_delta(backend_, restore_masks_, acted_tids_, masks, live_tids);
}

RestoreResult Actuator::restore_all() {
  RestoreResult result;
  result.requested = acted_tids_.size();
  for (int tid : acted_tids_) {
    const auto &cpus = restore_masks_.at(tid);
    int error = 0;
    if (backend_.set(tid, cpus, error)) ++result.restored;
    else if (error == ESRCH) ++result.vanished;
    else ++result.failed;
  }
  acted_tids_.clear();
  restore_masks_.clear();
  return result;
}

RestoreResult Actuator::restore(const std::set<int> &tids) {
  RestoreResult result;
  for (int tid : tids) {
    if (!acted_tids_.contains(tid)) continue;
    ++result.requested;
    int error = 0;
    if (backend_.set(tid, restore_masks_.at(tid), error)) {
      ++result.restored;
      acted_tids_.erase(tid);
      restore_masks_.erase(tid);
    } else if (error == ESRCH) {
      ++result.vanished;
      acted_tids_.erase(tid);
      restore_masks_.erase(tid);
    } else {
      ++result.failed;
    }
  }
  return result;
}

void Actuator::note_policy_action(int tid) { acted_tids_.insert(tid); }
} // namespace affinitygraph
