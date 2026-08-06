#include <cstdlib>
#include <pthread.h>
#include <string>

namespace {
void *return_normally(void *value) { return value; }

void *cancelled(void *) {
  for (;;) pthread_testcancel();
}
} // namespace

int main() {
  const char *preload = std::getenv("LD_PRELOAD");
  if (preload && std::string(preload).find("affinitygraph") != std::string::npos) return 2;
  pthread_t normal{};
  pthread_t cancel{};
  if (pthread_create(&normal, nullptr, return_normally, nullptr) != 0) return 3;
  if (pthread_create(&cancel, nullptr, cancelled, nullptr) != 0) return 4;
  pthread_setname_np(normal, "ag-normal");
  pthread_setname_np(cancel, "ag-cancel");
  pthread_cancel(cancel);
  pthread_join(normal, nullptr);
  pthread_join(cancel, nullptr);
  return 0;
}
