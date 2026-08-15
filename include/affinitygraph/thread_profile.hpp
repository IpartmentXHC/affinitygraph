#pragma once

#include "affinitygraph/core.hpp"

#include <optional>

namespace affinitygraph {

struct ThreadProfileMatch {
  std::optional<std::string> comm;
  std::optional<std::string> comm_prefix;
  std::optional<std::string> cgroup;
  std::optional<std::string> cgroup_prefix;
  std::optional<int> tid;
};

struct ThreadProfileAffinity {
  std::vector<int> cpus;
  size_t count = 0;
};

struct ThreadProfileRule {
  std::string id;
  ThreadProfileMatch match;
  std::vector<int> allowed_cpus;
  std::vector<ThreadProfileAffinity> affinities;
  bool dynamic = true;
};

struct ThreadProfileDynamic {
  bool enabled = true;
  int small_step_threads = 1;
  double large_change_ratio = 0.30;
  int large_step_threads = 4;
  int cooldown_seconds = 10;
};

struct ThreadProfile {
  int schema_version = 1;
  std::string profile_id;
  std::string generated_at;
  std::string status;
  std::string source_commit;
  std::string experiment_id;
  std::string test_id;
  std::string description;
  ThreadProfileDynamic dynamic;
  std::vector<ThreadProfileRule> placements;
};

struct ProfileAssignment {
  std::string rule_id;
  size_t instance = 0;
  std::vector<int> target_cpus;
  std::vector<int> allowed_cpus;
  bool dynamic = true;
};

ThreadProfile load_thread_profile(const std::string &path,
                                  const std::vector<int> &envelope);
bool profile_rule_matches(const ThreadProfileRule &rule,
                          const ThreadSample &sample);
std::optional<ProfileAssignment> profile_assignment(
    const ThreadProfile &profile, const ThreadSample &sample,
    std::map<std::string, size_t> &next_instances,
    std::map<ThreadIdentity, ProfileAssignment> &assigned);
void write_thread_profile(const ThreadProfile &profile, const std::string &path);

} // namespace affinitygraph
