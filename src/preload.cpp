#include "affinitygraph/runtime.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <utility>
#include <unistd.h>

using namespace affinitygraph;

namespace {
template <typename T> T real_symbol(const char *name) {
  void *symbol = dlsym(RTLD_NEXT, name);
  return reinterpret_cast<T>(symbol);
}

int current_tid() { return static_cast<int>(syscall(SYS_gettid)); }

std::vector<int> cpus_from_set(size_t size, const cpu_set_t *set) {
  std::vector<int> result;
  if (!set) return result;
  for (size_t cpu = 0; cpu < size * 8; ++cpu) if (CPU_ISSET_S(cpu, size, set)) result.push_back(static_cast<int>(cpu));
  return result;
}

struct StartContext {
  void *(*routine)(void *);
  void *argument;
  int parent_tid;
  uintptr_t routine_address;
  uint64_t created_ns;
  std::string routine_symbol;
};

thread_local bool exit_recorded = false;
std::mutex pthread_tid_mutex;
std::map<pthread_t, int> pthread_tids;

int tid_for(pthread_t thread) {
  if (pthread_equal(thread, pthread_self())) return current_tid();
  std::lock_guard lock(pthread_tid_mutex);
  auto it = pthread_tids.find(thread);
  return it == pthread_tids.end() ? -1 : it->second;
}

void cleanup_thread(void *) {
  if (!exit_recorded) {
    exit_recorded = true;
    if (auto *runtime = runtime_instance()) runtime->thread_exited(current_tid(), "cleanup");
  }
  std::lock_guard lock(pthread_tid_mutex);
  pthread_tids.erase(pthread_self());
}

void *start_trampoline(void *opaque) {
  auto *context = static_cast<StartContext *>(opaque);
  auto routine = context->routine;
  void *argument = context->argument;
  cpu_set_t actual;
  CPU_ZERO(&actual);
  pthread_getaffinity_np(pthread_self(), sizeof(actual), &actual);
  LifecycleRecord record{context->parent_tid, current_tid(), context->routine_address,
                         context->created_ns, context->routine_symbol, {}, cpus_from_set(sizeof(actual), &actual)};
  delete context;
  exit_recorded = false;
  {
    std::lock_guard lock(pthread_tid_mutex);
    pthread_tids[pthread_self()] = record.tid;
  }
  if (auto *runtime = runtime_instance()) runtime->thread_started(record);
  void *result = nullptr;
  pthread_cleanup_push(cleanup_thread, nullptr);
  result = routine(argument);
  pthread_cleanup_pop(1);
  return result;
}
} // namespace

extern "C" int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                              void *(*routine)(void *), void *argument) {
  using Function = int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
  static Function real = real_symbol<Function>("pthread_create");
  if (!real || internal_call()) return real ? real(thread, attr, routine, argument) : EAGAIN;
  Dl_info symbol{};
  dladdr(reinterpret_cast<void *>(routine), &symbol);
  std::string routine_symbol;
  if (symbol.dli_sname) routine_symbol = symbol.dli_sname;
  else if (symbol.dli_fname && symbol.dli_fbase) routine_symbol = std::string(symbol.dli_fname) + "+" +
      std::to_string(reinterpret_cast<uintptr_t>(routine) - reinterpret_cast<uintptr_t>(symbol.dli_fbase));
  auto *context = new (std::nothrow) StartContext{routine, argument, current_tid(),
      reinterpret_cast<uintptr_t>(routine), monotonic_ns(), std::move(routine_symbol)};
  if (!context) return EAGAIN;
  int rc = real(thread, attr, start_trampoline, context);
  if (rc != 0) delete context;
  return rc;
}

extern "C" [[noreturn]] void pthread_exit(void *value) {
  using Function = void (*)(void *);
  static Function real = real_symbol<Function>("pthread_exit");
  if (!exit_recorded && !internal_call()) {
    exit_recorded = true;
    if (auto *runtime = runtime_instance()) runtime->thread_exited(current_tid(), "pthread_exit");
  }
  real(value);
  __builtin_unreachable();
}

extern "C" int pthread_setname_np(pthread_t thread, const char *name) {
  using Function = int (*)(pthread_t, const char *);
  static Function real = real_symbol<Function>("pthread_setname_np");
  int rc = real(thread, name);
  int tid = tid_for(thread);
  if (rc == 0 && !internal_call() && tid >= 0)
    if (auto *runtime = runtime_instance()) runtime->thread_renamed(tid, name);
  return rc;
}

extern "C" int pthread_setaffinity_np(pthread_t thread, size_t size, const cpu_set_t *set) {
  using Function = int (*)(pthread_t, size_t, const cpu_set_t *);
  static Function real = real_symbol<Function>("pthread_setaffinity_np");
  int rc = real(thread, size, set);
  int tid = tid_for(thread);
  if (rc == 0 && !internal_call() && tid >= 0)
    if (auto *runtime = runtime_instance()) runtime->application_affinity(tid, cpus_from_set(size, set));
  return rc;
}

extern "C" int sched_setaffinity(pid_t pid, size_t size, const cpu_set_t *set) {
  using Function = int (*)(pid_t, size_t, const cpu_set_t *);
  static Function real = real_symbol<Function>("sched_setaffinity");
  int rc = real(pid, size, set);
  if (rc == 0 && !internal_call()) {
    int tid = pid == 0 ? current_tid() : pid;
    if (auto *runtime = runtime_instance()) runtime->application_affinity(tid, cpus_from_set(size, set));
  }
  return rc;
}

__attribute__((constructor)) static void affinitygraph_constructor() { runtime_initialize_from_environment(); }
__attribute__((destructor)) static void affinitygraph_destructor() { runtime_shutdown(); }
