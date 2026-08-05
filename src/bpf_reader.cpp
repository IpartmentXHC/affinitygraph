#include "affinitygraph/core.hpp"
#include "affinitygraph/bpf_events.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include <linux/bpf.h>
#include <memory>
#include <sys/syscall.h>
#include <unistd.h>

namespace affinitygraph {

class BpfRingReader {
public:
  static std::shared_ptr<BpfRingReader> from_environment() {
    const char *text = getenv("AFFINITYGRAPH_BPF_EVENTS_FD");
    if (!text || !*text) return {};
    char *end = nullptr;
    long fd = std::strtol(text, &end, 10);
    if (!end || *end || fd < 0) throw std::runtime_error("invalid AFFINITYGRAPH_BPF_EVENTS_FD");
    int health_fd = -1;
    if (const char *health = getenv("AFFINITYGRAPH_BPF_HEALTH_FD")) health_fd = std::atoi(health);
    return std::shared_ptr<BpfRingReader>(new BpfRingReader(static_cast<int>(fd), health_fd));
  }

  explicit BpfRingReader(int fd, int health_fd) : fd_(fd), health_fd_(health_fd), page_(static_cast<size_t>(sysconf(_SC_PAGESIZE))) {
    constexpr size_t capacity = 1U << 20;
    capacity_ = capacity;
    consumer_map_ = mmap(nullptr, page_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    producer_map_ = mmap(nullptr, page_ + 2 * capacity_, PROT_READ, MAP_SHARED, fd_, static_cast<off_t>(page_));
    if (consumer_map_ == MAP_FAILED || producer_map_ == MAP_FAILED) {
      if (consumer_map_ != MAP_FAILED) munmap(consumer_map_, page_);
      if (producer_map_ != MAP_FAILED) munmap(producer_map_, page_ + 2 * capacity_);
      throw std::runtime_error("cannot mmap BPF ring buffer: " + std::string(std::strerror(errno)));
    }
  }

  ~BpfRingReader() {
    munmap(consumer_map_, page_);
    munmap(producer_map_, page_ + 2 * capacity_);
  }

  std::vector<RelationObservation> drain() {
    constexpr uint32_t busy = 1U << 31;
    constexpr uint32_t discard = 1U << 30;
    auto *consumer = static_cast<uint64_t *>(consumer_map_);
    auto *producer = static_cast<uint64_t *>(producer_map_);
    auto *data = static_cast<unsigned char *>(producer_map_) + page_;
    uint64_t position = std::atomic_ref<uint64_t>(*consumer).load(std::memory_order_acquire);
    uint64_t end = std::atomic_ref<uint64_t>(*producer).load(std::memory_order_acquire);
    std::map<std::pair<int, int>, RelationObservation> grouped;
    while (position < end) {
      auto *length_ptr = reinterpret_cast<uint32_t *>(data + (position & (capacity_ - 1)));
      uint32_t length = std::atomic_ref<uint32_t>(*length_ptr).load(std::memory_order_acquire);
      if (length & busy) break;
      uint32_t payload_length = length & ~(busy | discard);
      if (!(length & discard) && payload_length >= sizeof(affinitygraph_bpf_event)) {
        affinitygraph_bpf_event event{};
        std::memcpy(&event, length_ptr + 2, sizeof(event));
        if ((event.kind == AFFINITYGRAPH_FUTEX || event.kind == AFFINITYGRAPH_VFS) && event.tid != event.peer_tid) {
          auto key = std::pair{static_cast<int>(event.tid), static_cast<int>(event.peer_tid)};
          auto &item = grouped[key];
          item.from_tid = key.first;
          item.to_tid = key.second;
          item.timestamp_ns = std::max(item.timestamp_ns, event.timestamp_ns);
          if (event.kind == AFFINITYGRAPH_FUTEX) item.futex_per_second += 1.0;
          else item.shared_vfs_seconds += static_cast<double>(event.value_ns) / 1e9;
          item.active_overlap = 1.0;
        }
      }
      position += (static_cast<uint64_t>(payload_length) + 8 + 7) & ~7ULL;
    }
    std::atomic_ref<uint64_t>(*consumer).store(position, std::memory_order_release);
    std::vector<RelationObservation> result;
    for (auto &[key, observation] : grouped) result.push_back(observation);
    return result;
  }

  affinitygraph_bpf_health health() const {
    affinitygraph_bpf_health result{};
    if (health_fd_ < 0) return result;
    uint32_t key = 0;
    union bpf_attr attr{};
    attr.map_fd = health_fd_;
    attr.key = reinterpret_cast<uint64_t>(&key);
    attr.value = reinterpret_cast<uint64_t>(&result);
    if (syscall(SYS_bpf, BPF_MAP_LOOKUP_ELEM, &attr, sizeof(attr)) != 0) return {};
    return result;
  }

private:
  int fd_;
  int health_fd_;
  size_t page_;
  size_t capacity_;
  void *consumer_map_ = MAP_FAILED;
  void *producer_map_ = MAP_FAILED;
};

std::shared_ptr<BpfRingReader> make_bpf_reader() { return BpfRingReader::from_environment(); }
std::vector<RelationObservation> drain_bpf_reader(BpfRingReader &reader) { return reader.drain(); }
affinitygraph_bpf_health bpf_reader_health(BpfRingReader &reader) { return reader.health(); }
} // namespace affinitygraph
