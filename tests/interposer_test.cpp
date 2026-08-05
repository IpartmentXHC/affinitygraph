#include <atomic>
#include <chrono>
#include <pthread.h>
#include <thread>

void cleanup(void *value) { static_cast<std::atomic<int> *>(value)->fetch_add(1); }

void *normal(void *value) {
  pthread_setname_np(pthread_self(), "normal_17");
  static_cast<std::atomic<int> *>(value)->fetch_add(1);
  return nullptr;
}

void *explicit_exit(void *value) {
  pthread_setname_np(pthread_self(), "exit_29");
  static_cast<std::atomic<int> *>(value)->fetch_add(1);
  pthread_exit(nullptr);
}

void *cancelled(void *value) {
  pthread_cleanup_push(cleanup, value);
  for (;;) {
    pthread_testcancel();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  pthread_cleanup_pop(0);
  return nullptr;
}

int main() {
  std::atomic<int> count{0};
  pthread_t a, b, c;
  if (pthread_create(&a, nullptr, normal, &count) || pthread_create(&b, nullptr, explicit_exit, &count) ||
      pthread_create(&c, nullptr, cancelled, &count)) return 2;
  pthread_join(a, nullptr);
  pthread_join(b, nullptr);
  pthread_cancel(c);
  pthread_join(c, nullptr);
  return count == 3 ? 0 : 3;
}
