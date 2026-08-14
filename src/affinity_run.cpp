#include "affinitygraph/core.hpp"
#include "affinitygraph/runtime.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <grp.h>
#include <iostream>
#include <linux/bpf.h>
#include <linux/capability.h>
#include <pwd.h>
#include <sched.h>
#include <string>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

using namespace affinitygraph;
namespace fs = std::filesystem;

namespace {
struct Arguments {
  std::string action;
  std::string config;
  std::string bpf_object;
  std::string user;
  std::string thread_profile;
  std::string profile_output;
  std::string experiment_id;
  std::string test_id;
  std::vector<char *> command;
};

Arguments parse(int argc, char **argv) {
  if (argc < 2)
    throw std::runtime_error("usage: affinity-run preflight|run --config PATH [--thread-profile PATH] [--profile-output PATH] [--experiment-id ID] [--test-id ID] [--bpf-object PATH] [--user USER] [-- command ...]");
  Arguments args;
  args.action = argv[1];
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--") {
      for (++i; i < argc; ++i) args.command.push_back(argv[i]);
      break;
    }
    if ((arg == "--config" || arg == "--bpf-object" || arg == "--user" || arg == "--thread-profile" || arg == "--profile-output" || arg == "--experiment-id" || arg == "--test-id") && i + 1 < argc) {
      std::string value = argv[++i];
      if (arg == "--config") args.config = value;
      else if (arg == "--bpf-object") args.bpf_object = value;
      else if (arg == "--user") args.user = value;
      else if (arg == "--thread-profile") args.thread_profile = value;
      else if (arg == "--profile-output") args.profile_output = value;
      else if (arg == "--experiment-id") args.experiment_id = value;
      else args.test_id = value;
    } else throw std::runtime_error("unknown or incomplete option: " + arg);
  }
  if (args.config.empty()) throw std::runtime_error("--config is required");
  return args;
}

std::vector<int> current_envelope() {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) != 0)
    throw std::runtime_error("sched_getaffinity failed");
  std::vector<int> result;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    if (CPU_ISSET(cpu, &set)) result.push_back(cpu);
  return result;
}

bool subset(const std::vector<int> &requested, const std::vector<int> &available) {
  return std::all_of(requested.begin(), requested.end(), [&](int cpu) {
    return std::find(available.begin(), available.end(), cpu) != available.end();
  });
}

struct Identity {
  uid_t uid;
  gid_t gid;
  std::string name;
};

Identity resolve_identity(const std::string &user) {
  if (!user.empty()) {
    passwd *entry = getpwnam(user.c_str());
    if (!entry) throw std::runtime_error("unknown run user: " + user);
    return {entry->pw_uid, entry->pw_gid, entry->pw_name};
  }
  if (geteuid() != 0) {
    passwd *entry = getpwuid(geteuid());
    return {geteuid(), getegid(), entry ? entry->pw_name : ""};
  }
  const char *uid_text = getenv("SUDO_UID"), *gid_text = getenv("SUDO_GID");
  if (!uid_text || !gid_text)
    throw std::runtime_error("root bootstrap requires --user outside sudo");
  uid_t uid = static_cast<uid_t>(std::stoul(uid_text));
  passwd *entry = getpwuid(uid);
  return {uid, static_cast<gid_t>(std::stoul(gid_text)), entry ? entry->pw_name : ""};
}

bool drop_identity(const Identity &identity, bool retain_sys_nice = false,
                   bool retain_sys_ptrace = false) {
  if (geteuid() != 0) return false;
  if (!identity.name.empty() && initgroups(identity.name.c_str(), identity.gid) != 0)
    throw std::runtime_error("initgroups failed: " + std::string(std::strerror(errno)));
  const bool retain_capabilities = retain_sys_nice || retain_sys_ptrace;
  if (retain_capabilities && prctl(PR_SET_KEEPCAPS, 1L) != 0)
    throw std::runtime_error("cannot retain supervisor capabilities");
  if (setgid(identity.gid) != 0 || setuid(identity.uid) != 0)
    throw std::runtime_error("failed to drop bootstrap privileges");
  if (retain_capabilities) {
    __user_cap_header_struct header{_LINUX_CAPABILITY_VERSION_3, 0};
    __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3]{};
    auto add_capability = [&](int capability) {
      const unsigned word = static_cast<unsigned>(capability) / 32;
      const __u32 mask = 1U << (capability % 32);
      data[word].effective |= mask;
      data[word].permitted |= mask;
    };
    if (retain_sys_nice) add_capability(CAP_SYS_NICE);
    if (retain_sys_ptrace) add_capability(CAP_SYS_PTRACE);
    if (syscall(SYS_capset, &header, data) != 0 || prctl(PR_SET_KEEPCAPS, 0L) != 0)
      throw std::runtime_error("cannot configure supervisor capabilities");
  }
  return retain_sys_nice;
}

bool bpf_map_update(int fd, const void *key, const void *value) {
  union bpf_attr attr{};
  attr.map_fd = fd;
  attr.key = reinterpret_cast<__u64>(key);
  attr.value = reinterpret_cast<__u64>(value);
  attr.flags = BPF_ANY;
  return syscall(SYS_bpf, BPF_MAP_UPDATE_ELEM, &attr, sizeof(attr)) == 0;
}

struct bpf_object;
struct bpf_program;
struct bpf_link;
struct bpf_map;
struct bpf_object_open_opts;
enum probe_attach_mode { PROBE_ATTACH_MODE_DEFAULT = 0 };
struct bpf_uprobe_opts {
  size_t sz;
  size_t ref_ctr_offset;
  __u64 bpf_cookie;
  bool retprobe;
  const char *func_name;
  probe_attach_mode attach_mode;
  size_t reserved[2];
};

class LibbpfApi {
public:
  using Open = bpf_object *(*)(const char *, const bpf_object_open_opts *);
  using Load = int (*)(bpf_object *);
  using Close = void (*)(bpf_object *);
  using NextProgram = bpf_program *(*)(const bpf_object *, bpf_program *);
  using SectionName = const char *(*)(const bpf_program *);
  using Attach = bpf_link *(*)(const bpf_program *);
  using AttachUprobe = bpf_link *(*)(const bpf_program *, pid_t, const char *, size_t,
                                     const bpf_uprobe_opts *);
  using DestroyLink = int (*)(bpf_link *);
  using FindMap = bpf_map *(*)(const bpf_object *, const char *);
  using MapFd = int (*)(const bpf_map *);
  using GetError = long (*)(const void *);

  bool open(std::string &reason) {
    handle_ = dlopen("libbpf.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!handle_) { reason = "libbpf.so.1 unavailable"; return false; }
    return symbol(open_file, "bpf_object__open_file", reason) &&
           symbol(load, "bpf_object__load", reason) &&
           symbol(close, "bpf_object__close", reason) &&
           symbol(next_program, "bpf_object__next_program", reason) &&
           symbol(section_name, "bpf_program__section_name", reason) &&
           symbol(attach, "bpf_program__attach", reason) &&
           symbol(attach_uprobe, "bpf_program__attach_uprobe_opts", reason) &&
           symbol(destroy_link, "bpf_link__destroy", reason) &&
           symbol(find_map, "bpf_object__find_map_by_name", reason) &&
           symbol(map_fd, "bpf_map__fd", reason) &&
           symbol(get_error, "libbpf_get_error", reason);
  }

  ~LibbpfApi() { if (handle_) dlclose(handle_); }

  Open open_file = nullptr;
  Load load = nullptr;
  Close close = nullptr;
  NextProgram next_program = nullptr;
  SectionName section_name = nullptr;
  Attach attach = nullptr;
  AttachUprobe attach_uprobe = nullptr;
  DestroyLink destroy_link = nullptr;
  FindMap find_map = nullptr;
  MapFd map_fd = nullptr;
  GetError get_error = nullptr;

private:
  template <typename T> bool symbol(T &destination, const char *name, std::string &reason) {
    destination = reinterpret_cast<T>(dlsym(handle_, name));
    if (destination) return true;
    reason = std::string("libbpf is missing ") + name;
    return false;
  }
  void *handle_ = nullptr;
};

class BpfSession {
public:
  bool load_object(const std::string &path, std::string &reason) {
    if (!api_.open(reason)) return false;
    object_ = api_.open_file(path.c_str(), nullptr);
    if (!object_ || api_.get_error(object_)) {
      reason = "cannot open BPF object";
      object_ = nullptr;
      return false;
    }
    if (api_.load(object_) != 0) { reason = "cannot load BPF object"; return false; }
    for (bpf_program *program = api_.next_program(object_, nullptr); program;
         program = api_.next_program(object_, program)) {
      std::string section = api_.section_name(program) ? api_.section_name(program) : "";
      if (section == "uprobe") { pthread_entry_ = program; continue; }
      if (section == "uretprobe") { pthread_return_ = program; continue; }
      bpf_link *link = api_.attach(program);
      if (!link || api_.get_error(link)) {
        reason = "cannot attach BPF section " + section;
        return false;
      }
      links_.push_back(link);
    }
    pids_fd_ = map_fd("pids");
    events_fd_ = map_fd("events");
    health_fd_ = map_fd("health");
    futex_aggregates_fd_ = map_fd("futex_aggregates");
    vfs_aggregates_fd_ = map_fd("vfs_aggregates");
    if (pids_fd_ < 0 || events_fd_ < 0 || health_fd_ < 0 ||
        futex_aggregates_fd_ < 0 || vfs_aggregates_fd_ < 0) {
      reason = "BPF object is missing required data maps";
      return false;
    }
    reason = "CO-RE programs loaded and attached with libbpf";
    return true;
  }

  bool add_target(pid_t pid) {
    __u32 key = static_cast<__u32>(pid);
    affinitygraph_target target{key, 0, 0};
    return pids_fd_ >= 0 && bpf_map_update(pids_fd_, &key, &target);
  }

  std::string attach_pthread_uprobe(pid_t pid) {
    if (!pthread_entry_ || !pthread_return_) return "unavailable: programs not loaded";
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::vector<std::string> candidates;
    std::string line;
    while (std::getline(maps, line)) {
      auto slash = line.find('/');
      if (slash == std::string::npos) continue;
      std::string path = line.substr(slash);
      if (path.find("libc.so") == std::string::npos &&
          path.find("libpthread.so") == std::string::npos) continue;
      if (std::find(candidates.begin(), candidates.end(), path) == candidates.end())
        candidates.push_back(path);
    }
    if (candidates.empty()) return "pending: libc not mapped";
    for (const auto &path : candidates) {
      bpf_uprobe_opts entry{};
      entry.sz = offsetof(bpf_uprobe_opts, reserved);
      entry.func_name = "pthread_create";
      bpf_link *first = api_.attach_uprobe(pthread_entry_, -1, path.c_str(), 0, &entry);
      if (!first || api_.get_error(first)) continue;
      bpf_uprobe_opts ret = entry;
      ret.retprobe = true;
      bpf_link *second = api_.attach_uprobe(pthread_return_, -1, path.c_str(), 0, &ret);
      if (!second || api_.get_error(second)) {
        api_.destroy_link(first);
        continue;
      }
      links_.push_back(first);
      links_.push_back(second);
      return "attached:" + path;
    }
    return "unavailable: pthread_create symbol not attachable";
  }

  int events_fd() const { return events_fd_; }
  int health_fd() const { return health_fd_; }
  int futex_aggregates_fd() const { return futex_aggregates_fd_; }
  int vfs_aggregates_fd() const { return vfs_aggregates_fd_; }

  void reset() {
    for (auto it = links_.rbegin(); it != links_.rend(); ++it) api_.destroy_link(*it);
    links_.clear();
    if (object_) api_.close(object_);
    object_ = nullptr;
    pthread_entry_ = nullptr;
    pthread_return_ = nullptr;
    pids_fd_ = events_fd_ = health_fd_ = futex_aggregates_fd_ =
        vfs_aggregates_fd_ = -1;
  }

  ~BpfSession() { reset(); }

private:
  int map_fd(const char *name) {
    bpf_map *map = api_.find_map(object_, name);
    return map ? api_.map_fd(map) : -1;
  }
  LibbpfApi api_;
  bpf_object *object_ = nullptr;
  bpf_program *pthread_entry_ = nullptr;
  bpf_program *pthread_return_ = nullptr;
  std::vector<bpf_link *> links_;
  int pids_fd_ = -1;
  int events_fd_ = -1;
  int health_fd_ = -1;
  int futex_aggregates_fd_ = -1;
  int vfs_aggregates_fd_ = -1;
};

bool bpf_available(const std::string &object, std::string &reason) {
  if (!fs::exists("/sys/kernel/btf/vmlinux")) { reason = "kernel BTF is missing"; return false; }
  if (object.empty() || !fs::exists(object)) { reason = "compiled CO-RE object is missing"; return false; }
  if (geteuid() != 0) { reason = "bootstrap is not root"; return false; }
  void *handle = dlopen("libbpf.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!handle) { reason = "libbpf.so.1 is missing"; return false; }
  dlclose(handle);
  reason = "CO-RE object, kernel BTF, privilege, and libbpf available";
  return true;
}

volatile sig_atomic_t child_process_group = -1;
void forward_signal(int signal) {
  if (child_process_group > 0) kill(-child_process_group, signal);
}

void install_signal_handlers() {
  struct sigaction action{};
  action.sa_handler = forward_signal;
  sigemptyset(&action.sa_mask);
  for (int signal : {SIGTERM, SIGINT, SIGHUP, SIGQUIT}) sigaction(signal, &action, nullptr);
}

int preflight(const Arguments &args) {
  Config config = load_config(args.config);
  if (!args.thread_profile.empty()) load_thread_profile(args.thread_profile, config.cpus);
  auto available = current_envelope();
  bool ok = true;
  std::cout << "config: ok\n";
  if (!subset(config.cpus, available)) {
    std::cout << "cpu_envelope: fail (outside startup/cgroup mask "
              << format_cpu_list(available) << ")\n";
    ok = false;
  } else {
    HardwareGraph graph = HardwareGraph::discover(config.cpus);
    graph.load_calibration(config.calibration_path);
    std::cout << "cpu_envelope: ok (" << format_cpu_list(config.cpus) << ")\n";
  }
  utsname kernel{};
  uname(&kernel);
  std::cout << "kernel: " << kernel.release << '\n';
  std::string reason;
  bool bpf = bpf_available(args.bpf_object, reason);
  if (bpf) {
    BpfSession session;
    bpf = session.load_object(args.bpf_object, reason);
  }
  std::cout << "bpf: " << (bpf ? "ok" : config.bpf_required ? "fail" : "disabled")
            << " (" << reason << ")\n";
  std::cout << "pthread_uprobe: " << (config.pthread_uprobe ? "auto" : "off") << '\n';
  if (config.bpf_required && !bpf) ok = false;
  return ok ? 0 : 1;
}

int supervise(const Arguments &args, Config config) {
  config.thread_profile_path = args.thread_profile;
  config.profile_output_path = args.profile_output;
  config.experiment_id = args.experiment_id;
  config.test_id = args.test_id;
  if (!subset(config.cpus, current_envelope()))
    throw std::runtime_error("configured CPU envelope exceeds startup/cgroup affinity");
  if (args.command.empty()) throw std::runtime_error("run requires -- command ...");
  std::vector<char *> command = args.command;
  command.push_back(nullptr);
  Identity identity = resolve_identity(args.user);
  std::string bpf_reason;
  BpfSession bpf;
  bool bpf_ready = bpf_available(args.bpf_object, bpf_reason) &&
                   bpf.load_object(args.bpf_object, bpf_reason);
  if (!bpf_ready) bpf.reset();
  if (!bpf_ready && config.bpf_required)
    throw std::runtime_error("required BPF unavailable: " + bpf_reason);
  if (!bpf_ready) std::cerr << "affinity-run: BPF disabled: " << bpf_reason << '\n';

  int start_pipe[2];
  int exec_pipe[2];
  if (pipe2(start_pipe, O_CLOEXEC) != 0 || pipe2(exec_pipe, O_CLOEXEC) != 0)
    throw std::runtime_error("cannot create child synchronization pipes");
  prctl(PR_SET_CHILD_SUBREAPER, 1);
  pid_t child = fork();
  if (child < 0) throw std::runtime_error("fork failed");
  if (child == 0) {
    close(start_pipe[1]);
    close(exec_pipe[0]);
    setpgid(0, 0);
    char release = 0;
    if (read(start_pipe[0], &release, 1) != 1) _exit(126);
    close(start_pipe[0]);
    try {
      drop_identity(identity);
      execvp(command[0], command.data());
    } catch (...) {
      errno = EPERM;
    }
    int error = errno;
    [[maybe_unused]] ssize_t written = write(exec_pipe[1], &error, sizeof(error));
    _exit(127);
  }

  close(start_pipe[0]);
  close(exec_pipe[1]);
  setpgid(child, child);
  child_process_group = child;
  install_signal_handlers();
  if (bpf_ready && !bpf.add_target(child)) {
    kill(-child, SIGKILL);
    throw std::runtime_error("cannot initialize target TGID map");
  }
  char release = 1;
  if (write(start_pipe[1], &release, 1) != 1) {
    kill(-child, SIGKILL);
    throw std::runtime_error("cannot release target child");
  }
  close(start_pipe[1]);

  int exec_error = 0;
  ssize_t exec_result;
  do { exec_result = read(exec_pipe[0], &exec_error, sizeof(exec_error)); }
  while (exec_result < 0 && errno == EINTR);
  close(exec_pipe[0]);
  if (exec_result > 0) {
    int status = 0;
    waitpid(child, &status, 0);
    throw std::runtime_error("target exec failed: " + std::string(std::strerror(exec_error)));
  }

  std::string uprobe_status = config.pthread_uprobe ? "unavailable: BPF disabled" : "off";
  if (bpf_ready && config.pthread_uprobe) {
    for (int attempt = 0; attempt < 100; ++attempt) {
      uprobe_status = bpf.attach_pthread_uprobe(child);
      if (uprobe_status.rfind("attached:", 0) == 0) break;
      if (uprobe_status.rfind("pending:", 0) != 0) break;
      usleep(10000);
    }
  }
  bool affinity_capability = false;
  try {
    affinity_capability = drop_identity(identity, config.mode == Mode::Active,
                                        true);
    if (config.mode == Mode::Active) {
      LinuxAffinityBackend affinity;
      auto original = affinity.get(child);
      int error = 0;
      if (original.empty() || !affinity.set(child, original, error))
        throw std::runtime_error(
            "active supervisor affinity authority check failed: " +
            std::string(std::strerror(error ? error : ESRCH)));
    }
  } catch (...) {
    kill(-child, SIGKILL);
    int ignored = 0;
    while (waitpid(child, &ignored, 0) < 0 && errno == EINTR) {}
    child_process_group = -1;
    throw;
  }
  auto reader = bpf_ready ? make_bpf_reader(
      bpf.events_fd(), bpf.health_fd(), bpf.futex_aggregates_fd(),
      bpf.vfs_aggregates_fd()) : nullptr;
  Runtime runtime(std::move(config), child, std::move(reader), !bpf_ready,
                  std::move(uprobe_status), affinity_capability);

  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  int descendant_status = 0;
  for (;;) {
    pid_t waited = waitpid(-1, &descendant_status, 0);
    if (waited > 0) continue;
    if (waited < 0 && errno == EINTR) continue;
    if (waited < 0 && errno == ECHILD) break;
    break;
  }
  child_process_group = -1;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 1;
}
} // namespace

int main(int argc, char **argv) {
  try {
    Arguments args = parse(argc, argv);
    if (args.action == "preflight") return preflight(args);
    if (args.action != "run") throw std::runtime_error("action must be preflight or run");
    return supervise(args, load_config(args.config));
  } catch (const std::exception &error) {
    std::cerr << "affinity-run: " << error.what() << '\n';
    return 2;
  }
}
