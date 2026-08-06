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

ApplyResult Actuator::apply(const Placement &placement, const std::set<int> &live_tids) {
  ApplyResult result;
  result.requested = placement.tid_to_cpu.size();
  std::vector<int> completed;
  for (const auto &[tid, cpu] : placement.tid_to_cpu) {
    if (!live_tids.contains(tid)) { ++result.vanished; continue; }
    if (!restore_masks_.contains(tid)) restore_masks_[tid] = backend_.get(tid);
    int error = 0;
    if (!backend_.set(tid, {cpu}, error)) {
      if (error == ESRCH) { ++result.vanished; continue; }
      result.error = error;
      for (auto it = completed.rbegin(); it != completed.rend(); ++it) {
        int ignored = 0;
        if (backend_.set(*it, restore_masks_[*it], ignored) || ignored == ESRCH) {
          ++result.rolled_back;
          acted_tids_.erase(*it);
        } else result.rollback_success = false;
      }
      return result;
    }
    completed.push_back(tid);
    acted_tids_.insert(tid);
    ++result.applied;
  }
  result.success = true;
  result.committed = result.applied;
  return result;
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

void Actuator::note_policy_action(int tid) { acted_tids_.insert(tid); }
} // namespace affinitygraph
