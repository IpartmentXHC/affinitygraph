#include "affinitygraph/thread_profile.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <regex>
#include <type_traits>

namespace affinitygraph {
namespace {
std::string file_text(const std::string &path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open thread profile: " + path);
  return {std::istreambuf_iterator<char>(input), {}};
}

std::optional<std::string> string_field(const std::string &text, const std::string &key) {
  std::regex expression("\\\"" + key + "\\\"\\s*:\\s*(null|\\\"([^\\\"]*)\\\")");
  std::smatch match;
  if (!std::regex_search(text, match, expression)) return std::nullopt;
  if (match[1] == "null") return std::optional<std::string>{};
  return match[2].str();
}

template <typename T>
T number_field(const std::string &text, const std::string &key) {
  std::regex expression("\\\"" + key + "\\\"\\s*:\\s*([-+]?[0-9]+(?:\\.[0-9]+)?)");
  std::smatch match;
  if (!std::regex_search(text, match, expression)) throw std::runtime_error("profile field missing: " + key);
  if constexpr (std::is_integral_v<T>) return static_cast<T>(std::stoll(match[1]));
  else return static_cast<T>(std::stod(match[1]));
}

bool bool_field(const std::string &text, const std::string &key) {
  std::regex expression("\\\"" + key + "\\\"\\s*:\\s*(true|false)");
  std::smatch match;
  if (!std::regex_search(text, match, expression)) throw std::runtime_error("profile field missing: " + key);
  return match[1] == "true";
}

std::string object_after(const std::string &text, const std::string &key, char open, char close) {
  auto marker = text.find('"' + key + '"');
  if (marker == std::string::npos) throw std::runtime_error("profile field missing: " + key);
  auto start = text.find(open, marker);
  if (start == std::string::npos) throw std::runtime_error("profile container invalid: " + key);
  int depth = 0;
  bool quoted = false;
  for (size_t i = start; i < text.size(); ++i) {
    if (text[i] == '"' && (i == 0 || text[i - 1] != '\\')) quoted = !quoted;
    if (quoted) continue;
    if (text[i] == open) ++depth;
    if (text[i] == close && --depth == 0) return text.substr(start + 1, i - start - 1);
  }
  throw std::runtime_error("profile container unterminated: " + key);
}

std::vector<std::string> objects(const std::string &text) {
  std::vector<std::string> result;
  int depth = 0; size_t start = 0; bool quoted = false;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '"' && (i == 0 || text[i - 1] != '\\')) quoted = !quoted;
    if (quoted) continue;
    if (text[i] == '{' && depth++ == 0) start = i;
    else if (text[i] == '}' && --depth == 0) result.push_back(text.substr(start, i - start + 1));
  }
  return result;
}

bool subset(const std::vector<int> &left, const std::vector<int> &right) {
  return std::all_of(left.begin(), left.end(), [&](int cpu) {
    return std::binary_search(right.begin(), right.end(), cpu);
  });
}

void validate_rule(const ThreadProfileRule &rule, const std::vector<int> &envelope) {
  if (rule.id.empty() || rule.affinities.empty() || rule.allowed_cpus.empty())
    throw std::runtime_error("profile rule requires id, allowed_cpus, and affinities");
  if ((rule.match.comm && rule.match.comm_prefix) ||
      (rule.match.cgroup && rule.match.cgroup_prefix) ||
      (!rule.match.comm && !rule.match.comm_prefix && !rule.match.cgroup &&
       !rule.match.cgroup_prefix && !rule.match.tid))
    throw std::runtime_error("profile rule has invalid match fields: " + rule.id);
  if (!subset(rule.allowed_cpus, envelope))
    throw std::runtime_error("profile allowed_cpus outside resource envelope: " + rule.id);
  if (rule.affinities.size() != 1)
    throw std::runtime_error("profile rule must have exactly one affinity: " + rule.id);
  const auto &affinity = rule.affinities[0];
  if (affinity.cpus.empty() || !subset(affinity.cpus, rule.allowed_cpus))
    throw std::runtime_error("profile affinity is invalid: " + rule.id);
}

std::string esc(const std::string &value) {
  std::ostringstream out;
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') out << '\\' << c;
    else if (c == '\n') out << "\\n";
    else if (c >= 0x20) out << c;
  }
  return out.str();
}

std::string optional_json(const std::optional<std::string> &value) {
  return value ? "\"" + esc(*value) + "\"" : "null";
}
} // namespace

ThreadProfile load_thread_profile(const std::string &path,
                                  const std::vector<int> &envelope) {
  const std::string root = file_text(path);
  ThreadProfile profile;
  profile.schema_version = number_field<int>(root, "schema_version");
  profile.profile_id = string_field(root, "profile_id").value_or("");
  profile.generated_at = string_field(root, "generated_at").value_or("");
  profile.status = string_field(root, "status").value_or("");
  const auto source = object_after(root, "source", '{', '}');
  profile.source_commit = string_field(source, "commit").value_or("");
  profile.experiment_id = string_field(source, "experiment_id").value_or("");
  profile.test_id = string_field(source, "test_id").value_or("");
  const auto applicability = object_after(root, "applicability", '{', '}');
  profile.description = string_field(applicability, "description").value_or("");
  const auto dynamic = object_after(root, "dynamic", '{', '}');
  profile.dynamic.enabled = bool_field(dynamic, "enabled");
  profile.dynamic.small_step_threads = number_field<int>(dynamic, "small_step_threads");
  profile.dynamic.large_change_ratio = number_field<double>(dynamic, "large_change_ratio");
  profile.dynamic.large_step_threads = number_field<int>(dynamic, "large_step_threads");
  profile.dynamic.cooldown_seconds = number_field<int>(dynamic, "cooldown_seconds");
  if ((profile.schema_version != 1 && profile.schema_version != 2) || profile.profile_id.empty() ||
      profile.generated_at.empty() ||
      (profile.status != "candidate" && profile.status != "tested") ||
      profile.dynamic.small_step_threads < 1 ||
      profile.dynamic.large_step_threads < profile.dynamic.small_step_threads ||
      profile.dynamic.large_change_ratio <= 0 || profile.dynamic.large_change_ratio > 1 ||
      profile.dynamic.cooldown_seconds < 0)
    throw std::runtime_error("thread profile has unsupported metadata");
  std::set<std::string> ids;
  for (const auto &node : objects(object_after(root, "placements", '[', ']'))) {
    ThreadProfileRule rule;
    rule.id = string_field(node, "id").value_or("");
    auto load_optional = [&](const char *key) -> std::optional<std::string> {
      return string_field(object_after(node, "match", '{', '}'), key);
    };
    rule.match.comm = load_optional("comm");
    rule.match.comm_prefix = load_optional("comm_prefix");
    rule.match.cgroup = load_optional("cgroup");
    rule.match.cgroup_prefix = load_optional("cgroup_prefix");
    const auto match = object_after(node, "match", '{', '}');
    std::regex tid_expression("\\\"tid\\\"\\s*:\\s*([0-9]+)");
    std::smatch tid_match;
    if (std::regex_search(match, tid_match, tid_expression)) rule.match.tid = std::stoi(tid_match[1]);
    rule.allowed_cpus = parse_cpu_list(string_field(node, "allowed_cpus").value_or(""));
    rule.dynamic = bool_field(node, "dynamic");
    for (const auto &a : objects(object_after(node, "affinities", '[', ']'))) {
      // schema v2 has no count; v1 files may still carry one (ignored).
      rule.affinities.push_back({parse_cpu_list(string_field(a, "cpus").value_or(""))});
    }
    if (!ids.insert(rule.id).second) throw std::runtime_error("duplicate profile rule id: " + rule.id);
    validate_rule(rule, envelope);
    profile.placements.push_back(std::move(rule));
  }
  return profile;
}

bool profile_rule_matches(const ThreadProfileRule &rule, const ThreadSample &sample) {
  auto has_cgroup = [&](const std::string &value, bool prefix) {
    return std::any_of(sample.cgroups.begin(), sample.cgroups.end(), [&](const auto &path) {
      return prefix ? path.rfind(value, 0) == 0 : path == value;
    });
  };
  return (!rule.match.comm || sample.comm == *rule.match.comm) &&
         (!rule.match.comm_prefix || sample.comm.rfind(*rule.match.comm_prefix, 0) == 0) &&
         (!rule.match.cgroup || has_cgroup(*rule.match.cgroup, false)) &&
         (!rule.match.cgroup_prefix || has_cgroup(*rule.match.cgroup_prefix, true)) &&
         (!rule.match.tid || sample.identity.tid == *rule.match.tid);
}

std::optional<ProfileAssignment> profile_assignment(
    const ThreadProfile &profile, const ThreadSample &sample,
    std::map<std::string, size_t> &next_instances,
    std::map<ThreadIdentity, ProfileAssignment> &assigned) {
  if (auto found = assigned.find(sample.identity); found != assigned.end()) return found->second;
  for (const auto &rule : profile.placements) {
    if (!profile_rule_matches(rule, sample)) continue;
    size_t instance = next_instances[rule.id]++;
    ProfileAssignment result{rule.id, instance, rule.affinities[0].cpus,
                             rule.allowed_cpus, rule.dynamic};
    assigned.emplace(sample.identity, result);
    return result;
  }
  return std::nullopt;
}

void write_thread_profile(const ThreadProfile &profile, const std::string &path) {
  std::filesystem::path output(path);
  std::filesystem::create_directories(output.parent_path());
  std::filesystem::path temporary = output.string() + ".tmp";
  std::ofstream out(temporary);
  if (!out) throw std::runtime_error("cannot write thread profile: " + temporary.string());
  out << "{\n  \"schema_version\": 2,\n  \"profile_id\": \"" << esc(profile.profile_id)
      << "\",\n  \"generated_at\": \"" << esc(profile.generated_at)
      << "\",\n  \"status\": \"" << esc(profile.status) << "\",\n"
      << "  \"source\": {\"commit\": \"" << esc(profile.source_commit)
      << "\", \"experiment_id\": \"" << esc(profile.experiment_id)
      << "\", \"test_id\": \"" << esc(profile.test_id) << "\"},\n"
      << "  \"applicability\": {\"description\": \"" << esc(profile.description)
      << "\", \"similarity\": {\"metric\": \"\", \"threshold\": null, \"reported_gap\": null}},\n"
      << "  \"dynamic\": {\"enabled\": " << (profile.dynamic.enabled ? "true" : "false")
      << ", \"small_step_threads\": " << profile.dynamic.small_step_threads
      << ", \"large_change_ratio\": " << profile.dynamic.large_change_ratio
      << ", \"large_step_threads\": " << profile.dynamic.large_step_threads
      << ", \"cooldown_seconds\": " << profile.dynamic.cooldown_seconds << "},\n  \"placements\": [\n";
  for (size_t i = 0; i < profile.placements.size(); ++i) {
    const auto &rule = profile.placements[i];
    out << "    {\"id\": \"" << esc(rule.id) << "\", \"match\": {\"comm\": "
        << optional_json(rule.match.comm) << ", \"comm_prefix\": " << optional_json(rule.match.comm_prefix)
        << ", \"cgroup\": " << optional_json(rule.match.cgroup) << ", \"cgroup_prefix\": "
        << optional_json(rule.match.cgroup_prefix) << ", \"tid\": "
        << (rule.match.tid ? std::to_string(*rule.match.tid) : "null") << "}, \"allowed_cpus\": \""
        << format_cpu_list(rule.allowed_cpus) << "\", \"dynamic\": " << (rule.dynamic ? "true" : "false")
        << ", \"affinities\": [";
    for (size_t j = 0; j < rule.affinities.size(); ++j) {
      if (j) out << ',';
      out << "{\"cpus\": \"" << format_cpu_list(rule.affinities[j].cpus) << "\"}";
    }
    out << "]}" << (i + 1 == profile.placements.size() ? "" : ",") << '\n';
  }
  out << "  ]\n}\n";
  out.close();
  std::filesystem::rename(temporary, output);
}

} // namespace affinitygraph
