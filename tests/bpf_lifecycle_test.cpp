#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
std::atomic<bool> stopping{false};
std::atomic<int> raw_clone_started{0};
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
int pipe_fds[2] = {-1, -1};

cpu_set_t first_allowed_cpus(int limit) {
  cpu_set_t available;
  cpu_set_t selected;
  CPU_ZERO(&available);
  CPU_ZERO(&selected);
  if (sched_getaffinity(0, sizeof(available), &available) != 0) return selected;
  for (int cpu = 0, count = 0; cpu < CPU_SETSIZE && count < limit; ++cpu) {
    if (!CPU_ISSET(cpu, &available)) continue;
    CPU_SET(cpu, &selected);
    ++count;
  }
  return selected;
}

void *producer(void *) {
  pthread_setname_np(pthread_self(), "ag-producer-17");
  cpu_set_t selected = first_allowed_cpus(2);
  pthread_setaffinity_np(pthread_self(), sizeof(selected), &selected);
  while (!stopping.load()) {
    char value = 'x';
    [[maybe_unused]] ssize_t written = write(pipe_fds[1], &value, 1);
    pthread_mutex_lock(&mutex);
    pthread_cond_signal(&condition);
    pthread_mutex_unlock(&mutex);
    usleep(10000);
  }
  return nullptr;
}

void *consumer(void *) {
  pthread_setname_np(pthread_self(), "ag-consumer-29");
  while (!stopping.load()) {
    pthread_mutex_lock(&mutex);
    timespec deadline{};
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 5000000;
    if (deadline.tv_nsec >= 1000000000) {
      ++deadline.tv_sec;
      deadline.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(&condition, &mutex, &deadline);
    pthread_mutex_unlock(&mutex);
    char value = 0;
    [[maybe_unused]] ssize_t length = read(pipe_fds[0], &value, 1);
  }
  return nullptr;
}

void *explicit_exit(void *) {
  pthread_setname_np(pthread_self(), "ag-exit-31");
  pthread_exit(nullptr);
}

void *cancelled(void *) {
  pthread_setname_np(pthread_self(), "ag-cancel-47");
  for (;;) {
    pthread_testcancel();
    usleep(1000);
  }
}

int raw_clone(void *) {
  raw_clone_started.store(1);
  while (!stopping.load()) sched_yield();
  raw_clone_started.store(2);
  return 0;
}
} // namespace

int main(int argc, char **argv) {
  int run_seconds = argc > 1 ? std::atoi(argv[1]) : 6;
  if (run_seconds < 1 || run_seconds > 60) return 1;
  pthread_setname_np(pthread_self(), "ag-main");
  if (pipe(pipe_fds) != 0) return 2;
  cpu_set_t main_mask = first_allowed_cpus(4);
  if (sched_setaffinity(0, sizeof(main_mask), &main_mask) != 0) return 3;
  sleep(1);

  pthread_t producer_thread{}, consumer_thread{}, exit_thread{}, cancel_thread{};
  if (pthread_create(&producer_thread, nullptr, producer, nullptr) ||
      pthread_create(&consumer_thread, nullptr, consumer, nullptr) ||
      pthread_create(&exit_thread, nullptr, explicit_exit, nullptr) ||
      pthread_create(&cancel_thread, nullptr, cancelled, nullptr)) return 4;

  constexpr size_t stack_size = 1U << 20;
  void *stack = mmap(nullptr, stack_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
  if (stack == MAP_FAILED) return 5;
  int clone_flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                    CLONE_THREAD | CLONE_SYSVSEM;
  if (clone(raw_clone, static_cast<char *>(stack) + stack_size,
            clone_flags, nullptr) < 0) return 6;

  pid_t child = fork();
  if (child == 0) {
    execl("/bin/sleep", "sleep", "2", nullptr);
    _exit(127);
  }
  if (child < 0) return 7;

  pthread_join(exit_thread, nullptr);
  sleep(static_cast<unsigned int>(run_seconds));
  stopping.store(true);
  char wake = 'x';
  [[maybe_unused]] ssize_t written = write(pipe_fds[1], &wake, 1);
  pthread_cancel(cancel_thread);
  pthread_join(cancel_thread, nullptr);
  pthread_join(producer_thread, nullptr);
  pthread_join(consumer_thread, nullptr);
  for (int attempt = 0; attempt < 100 && raw_clone_started.load() != 2; ++attempt)
    usleep(1000);
  int child_status = 0;
  waitpid(child, &child_status, 0);
  close(pipe_fds[0]);
  close(pipe_fds[1]);
  munmap(stack, stack_size);
  std::printf("raw_clone_started=%d child_exit=%d\n", raw_clone_started.load(),
              WIFEXITED(child_status) ? WEXITSTATUS(child_status) : -1);
  return raw_clone_started.load() == 2 && WIFEXITED(child_status) &&
                 WEXITSTATUS(child_status) == 0 ? 0 : 8;
}
