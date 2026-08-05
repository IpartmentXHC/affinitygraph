#include "affinitygraph/core.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <linux/bpf.h>
#include <grp.h>
#include <pwd.h>
#include <sched.h>
#include <string>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

using namespace affinitygraph;
namespace fs = std::filesystem;

namespace {
std::string executable_directory() {
  std::vector<char> buffer(4096);
  ssize_t size = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (size < 0) return ".";
  buffer[size] = 0;
  return fs::path(buffer.data()).parent_path().string();
}

std::vector<int> current_envelope() {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) != 0) throw std::runtime_error("sched_getaffinity failed");
  std::vector<int> result;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) if (CPU_ISSET(cpu, &set)) result.push_back(cpu);
  return result;
}

bool subset(const std::vector<int> &requested, const std::vector<int> &available) {
  return std::all_of(requested.begin(), requested.end(), [&](int cpu) {
    return std::find(available.begin(), available.end(), cpu) != available.end();
  });
}

struct Arguments {
  std::string action;
  std::string config;
  std::string library;
  std::string bpf_object;
  std::string user;
  std::string loader;
  std::string library_path;
  std::vector<char *> command;
};

Arguments parse(int argc, char **argv) {
  if (argc < 2) throw std::runtime_error("usage: affinity-run preflight|run --config PATH [--library PATH] [--bpf-object PATH] [--user USER] [-- command ...]");
  Arguments args;
  args.action = argv[1];
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--") { for (++i; i < argc; ++i) args.command.push_back(argv[i]); break; }
    if ((arg == "--config" || arg == "--library" || arg == "--bpf-object" || arg == "--user" ||
         arg == "--loader" || arg == "--library-path") && i + 1 < argc) {
      std::string value = argv[++i];
      if (arg == "--config") args.config = value;
      else if (arg == "--library") args.library = value;
      else if (arg == "--user") args.user = value;
      else if (arg == "--loader") args.loader = value;
      else if (arg == "--library-path") args.library_path = value;
      else args.bpf_object = value;
    } else throw std::runtime_error("unknown or incomplete option: " + arg);
  }
  if (args.config.empty()) throw std::runtime_error("--config is required");
  return args;
}

bool bpf_available(const std::string &object, std::string &reason) {
  if (!fs::exists("/sys/kernel/btf/vmlinux")) { reason = "kernel BTF is missing"; return false; }
  if (object.empty() || !fs::exists(object)) { reason = "compiled CO-RE object is missing"; return false; }
  if (geteuid() != 0) { reason = "bootstrap is not root (CAP_BPF/CAP_PERFMON loader is not configured)"; return false; }
  reason = "CO-RE object and kernel BTF available";
  return true;
}

int run_command(const std::vector<std::string> &command) {
  pid_t child = fork();
  if (child < 0) return -1;
  if (child == 0) {
    std::vector<char *> arguments;
    for (const auto &item : command) arguments.push_back(const_cast<char *>(item.c_str()));
    arguments.push_back(nullptr);
    execvp(arguments[0], arguments.data());
    _exit(127);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int bpf_obj_get(const fs::path &path) {
  union bpf_attr attr{};
  std::string value = path.string();
  attr.pathname = reinterpret_cast<__u64>(value.c_str());
  return static_cast<int>(syscall(SYS_bpf, BPF_OBJ_GET, &attr, sizeof(attr)));
}

bool bpf_map_update(int fd, const void *key, const void *value) {
  union bpf_attr attr{};
  attr.map_fd = fd;
  attr.key = reinterpret_cast<__u64>(key);
  attr.value = reinterpret_cast<__u64>(value);
  attr.flags = BPF_ANY;
  return syscall(SYS_bpf, BPF_MAP_UPDATE_ELEM, &attr, sizeof(attr)) == 0;
}

struct BpfSession {
  std::vector<int> descriptors;
  bool load(const std::string &object, std::string &reason) {
    fs::path root = fs::path("/sys/fs/bpf") / ("affinitygraph-" + std::to_string(getpid()));
    fs::path programs = root / "programs", maps = root / "maps";
    std::error_code ec;
    fs::create_directories(programs, ec);
    fs::create_directories(maps, ec);
    if (ec) { reason = "cannot create private bpffs directory: " + ec.message(); return false; }
    int rc = run_command({"bpftool", "prog", "loadall", object, programs.string(), "autoattach", "pinmaps", maps.string()});
    if (rc != 0) { fs::remove_all(root, ec); reason = "bpftool CO-RE load/attach failed"; return false; }
    int pid_map = bpf_obj_get(maps / "pids");
    int events = bpf_obj_get(maps / "events");
    int health = bpf_obj_get(maps / "health");
    if (pid_map < 0 || events < 0 || health < 0) {
      if (pid_map >= 0) close(pid_map);
      if (events >= 0) close(events);
      if (health >= 0) close(health);
      fs::remove_all(root, ec);
      reason = "loaded object does not expose pids/events maps";
      return false;
    }
    __u32 pid = static_cast<__u32>(getpid());
    __u8 enabled = 1;
    if (!bpf_map_update(pid_map, &pid, &enabled)) {
      close(pid_map); close(events); close(health); fs::remove_all(root, ec);
      reason = "cannot initialize target PID map";
      return false;
    }
    close(pid_map);
    descriptors.push_back(events);
    descriptors.push_back(health);
    for (const auto &entry : fs::recursive_directory_iterator(programs, ec)) {
      if (!entry.is_regular_file()) continue;
      int fd = bpf_obj_get(entry.path());
      if (fd >= 0) descriptors.push_back(fd);
    }
    for (int fd : descriptors) fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) & ~FD_CLOEXEC);
    setenv("AFFINITYGRAPH_BPF_EVENTS_FD", std::to_string(events).c_str(), 1);
    setenv("AFFINITYGRAPH_BPF_HEALTH_FD", std::to_string(health).c_str(), 1);
    fs::remove_all(root, ec);
    reason = "CO-RE programs attached; descriptors inherited by target";
    return true;
  }
  ~BpfSession() { for (int fd : descriptors) close(fd); }
};

void drop_identity(const std::string &user) {
  if (!user.empty()) {
    passwd *entry = getpwnam(user.c_str());
    if (!entry) throw std::runtime_error("unknown run user: " + user);
    if (initgroups(entry->pw_name, entry->pw_gid) != 0 || setgid(entry->pw_gid) != 0 || setuid(entry->pw_uid) != 0)
      throw std::runtime_error("failed to drop privileges to " + user);
    return;
  }
  const char *uid_text = getenv("SUDO_UID"), *gid_text = getenv("SUDO_GID");
  if (geteuid() != 0) return;
  if (!uid_text || !gid_text) throw std::runtime_error("root bootstrap requires --user outside sudo");
  gid_t gid = static_cast<gid_t>(std::stoul(gid_text));
  uid_t uid = static_cast<uid_t>(std::stoul(uid_text));
  if (setgid(gid) != 0 || setuid(uid) != 0) throw std::runtime_error("failed to drop bootstrap privileges");
}

int preflight(const Arguments &args) {
  Config config = load_config(args.config);
  auto available = current_envelope();
  bool ok = true;
  std::cout << "config: ok\n";
  if (!subset(config.cpus, available)) { std::cout << "cpu_envelope: fail (outside startup/cgroup mask " << format_cpu_list(available) << ")\n"; ok = false; }
  else { HardwareGraph graph = HardwareGraph::discover(config.cpus); graph.load_calibration(config.calibration_path); std::cout << "cpu_envelope: ok (" << format_cpu_list(config.cpus) << ")\n"; }
  utsname kernel{};
  uname(&kernel);
  std::cout << "kernel: " << kernel.release << "\n";
  std::string reason;
  bool bpf = bpf_available(args.bpf_object, reason);
  std::cout << "bpf: " << (bpf ? "ok" : config.bpf_required ? "fail" : "disabled") << " (" << reason << ")\n";
  if (config.bpf_required && !bpf) ok = false;
  std::cout << "mode: " << (config.mode == Mode::Active ? "active" : config.mode == Mode::Plan ? "plan" : "observe") << '\n';
  return ok ? 0 : 1;
}
} // namespace

int main(int argc, char **argv) {
  try {
    Arguments args = parse(argc, argv);
    if (args.action == "preflight") return preflight(args);
    if (args.action != "run") throw std::runtime_error("action must be preflight or run");
    Config config = load_config(args.config);
    if (!subset(config.cpus, current_envelope())) throw std::runtime_error("configured CPU envelope exceeds startup/cgroup affinity");
    if (args.command.empty()) throw std::runtime_error("run requires -- command ...");
    std::string reason;
    BpfSession bpf_session;
    bool bpf_ready = bpf_available(args.bpf_object, reason) && bpf_session.load(args.bpf_object, reason);
    if (!bpf_ready && config.bpf_required) throw std::runtime_error("required BPF unavailable: " + reason);
    if (!bpf_ready) {
      setenv("AFFINITYGRAPH_COLLECTOR_DISABLED", "1", 1);
      std::cerr << "affinity-run: BPF disabled: " << reason << '\n';
    } else unsetenv("AFFINITYGRAPH_COLLECTOR_DISABLED");
    if (args.library.empty()) args.library = executable_directory() + "/libaffinitygraph.so";
    if (!fs::exists(args.library)) throw std::runtime_error("preload library not found: " + args.library);
    setenv("AFFINITYGRAPH_CONFIG", args.config.c_str(), 1);
    std::vector<std::string> loader_command;
    if (!args.loader.empty()) {
      if (!fs::exists(args.loader)) throw std::runtime_error("dynamic loader not found: " + args.loader);
      loader_command.push_back(args.loader);
      if (!args.library_path.empty()) {
        loader_command.push_back("--library-path");
        loader_command.push_back(args.library_path);
      }
      loader_command.push_back("--preload");
      loader_command.push_back(args.library);
      for (char *item : args.command) loader_command.emplace_back(item);
    } else {
      const char *old_preload = getenv("LD_PRELOAD");
      std::string preload = args.library + (old_preload && *old_preload ? " " + std::string(old_preload) : "");
      setenv("LD_PRELOAD", preload.c_str(), 1);
    }
    drop_identity(args.user);
    if (!loader_command.empty()) {
      std::vector<char *> command;
      for (auto &item : loader_command) command.push_back(item.data());
      command.push_back(nullptr);
      execv(args.loader.c_str(), command.data());
      throw std::runtime_error("dynamic loader exec failed: " + std::string(std::strerror(errno)));
    }
    args.command.push_back(nullptr);
    execvp(args.command[0], args.command.data());
    throw std::runtime_error("execvp failed: " + std::string(std::strerror(errno)));
  } catch (const std::exception &error) {
    std::cerr << "affinity-run: " << error.what() << '\n';
    return 2;
  }
}
