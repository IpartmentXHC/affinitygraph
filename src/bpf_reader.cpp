#include "affinitygraph/runtime.hpp"
#include "affinitygraph/bpf_events.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include <time.h>
#include <linux/bpf.h>
#include <memory>
#include <sys/syscall.h>
#include <unistd.h>

namespace affinitygraph {

class BpfRingReader {
public:
  explicit BpfRingReader(int fd, int health_fd, int futex_aggregates_fd,
                         int vfs_aggregates_fd)
      : fd_(fd), health_fd_(health_fd), futex_aggregates_fd_(futex_aggregates_fd),
        vfs_aggregates_fd_(vfs_aggregates_fd),
        page_(static_cast<size_t>(sysconf(_SC_PAGESIZE))) {
    bpf_map_info info{};
    union bpf_attr attr{};
    uint32_t info_length = sizeof(info);
    attr.info.bpf_fd = fd_;
    attr.info.info_len = info_length;
    attr.info.info = reinterpret_cast<uint64_t>(&info);
    if (syscall(SYS_bpf, BPF_OBJ_GET_INFO_BY_FD, &attr, sizeof(attr)) != 0 ||
        info.type != BPF_MAP_TYPE_RINGBUF || !info.max_entries)
      throw std::runtime_error("cannot query BPF ring buffer capacity: " +
                               std::string(std::strerror(errno)));
    capacity_ = info.max_entries;
    stats_.capacity_bytes = capacity_;
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

  std::vector<affinitygraph_bpf_event> drain() {
    constexpr uint32_t busy = 1U << 31;
    constexpr uint32_t discard = 1U << 30;
    auto *consumer = static_cast<uint64_t *>(consumer_map_);
    auto *producer = static_cast<uint64_t *>(producer_map_);
    auto *data = static_cast<unsigned char *>(producer_map_) + page_;
    uint64_t position = std::atomic_ref<uint64_t>(*consumer).load(std::memory_order_acquire);
    uint64_t end = std::atomic_ref<uint64_t>(*producer).load(std::memory_order_acquire);
    uint64_t started = boot_ns();
    stats_.occupancy_bytes = end - position;
    stats_.max_occupancy_bytes = std::max(stats_.max_occupancy_bytes,
                                         stats_.occupancy_bytes);
    std::vector<affinitygraph_bpf_event> result;
    uint64_t batch_max_lag = 0;
    while (position < end) {
      auto *length_ptr = reinterpret_cast<uint32_t *>(data + (position & (capacity_ - 1)));
      uint32_t length = std::atomic_ref<uint32_t>(*length_ptr).load(std::memory_order_acquire);
      if (length & busy) break;
      uint32_t payload_length = length & ~(busy | discard);
      if (!(length & discard) && payload_length >= sizeof(affinitygraph_bpf_event)) {
        affinitygraph_bpf_event event{};
        std::memcpy(&event, length_ptr + 2, sizeof(event));
        if (started >= event.timestamp_ns)
          batch_max_lag = std::max(batch_max_lag, started - event.timestamp_ns);
        result.push_back(event);
      }
      position += (static_cast<uint64_t>(payload_length) + 8 + 7) & ~7ULL;
    }
    std::atomic_ref<uint64_t>(*consumer).store(position, std::memory_order_release);
    stats_.occupancy_bytes = end - position;
    stats_.drain_calls++;
    stats_.events_consumed += result.size();
    stats_.last_batch_events = result.size();
    stats_.max_batch_events = std::max<uint64_t>(stats_.max_batch_events, result.size());
    stats_.last_drain_ns = boot_ns() - started;
    stats_.max_drain_ns = std::max(stats_.max_drain_ns, stats_.last_drain_ns);
    stats_.last_max_lag_ns = batch_max_lag;
    stats_.max_lag_ns = std::max(stats_.max_lag_ns, batch_max_lag);
    return result;
  }

  BpfHealthSnapshot health() const {
    BpfHealthSnapshot snapshot;
    if (health_fd_ < 0) {
      snapshot.error = EBADF;
      return snapshot;
    }
    uint32_t key = 0;
    union bpf_attr attr{};
    attr.map_fd = health_fd_;
    attr.key = reinterpret_cast<uint64_t>(&key);
    attr.value = reinterpret_cast<uint64_t>(&snapshot.counters);
    if (syscall(SYS_bpf, BPF_MAP_LOOKUP_ELEM, &attr, sizeof(attr)) != 0) {
      snapshot.error = errno;
      return snapshot;
    }
    snapshot.valid = true;
    return snapshot;
  }

  std::vector<affinitygraph_bpf_event> drain_futex_aggregates() {
    std::vector<affinitygraph_bpf_event> result;
    if (futex_aggregates_fd_ < 0) return result;
    auto append = [&](const affinitygraph_futex_key &key,
                      const affinitygraph_futex_value &value) {
      affinitygraph_bpf_event event{};
      event.kind = AFFINITYGRAPH_FUTEX;
      event.tgid = key.tgid;
      event.tid = key.tid;
      event.peer_tid = key.peer_tid;
      event.timestamp_ns = value.last_timestamp_ns;
      event.start_time_ns = key.start_time_ns;
      event.peer_start_time_ns = key.peer_start_time_ns;
      event.value_ns = value.count;
      result.push_back(event);
      stats_.futex_aggregate_records++;
      stats_.futex_handoffs += value.count;
    };
    constexpr uint32_t batch_size = 4096;
    constexpr size_t drain_limit = 16384;
    std::vector<affinitygraph_futex_key> keys(batch_size);
    std::vector<affinitygraph_futex_value> values(batch_size);
    affinitygraph_futex_key cursor{};
    bool have_cursor = false;
    bool batch_supported = true;
    size_t drained = 0;
    while (drained < drain_limit) {
      union bpf_attr attr{};
      attr.batch.map_fd = futex_aggregates_fd_;
      attr.batch.in_batch = have_cursor ? reinterpret_cast<uint64_t>(&cursor) : 0;
      attr.batch.out_batch = reinterpret_cast<uint64_t>(&cursor);
      attr.batch.keys = reinterpret_cast<uint64_t>(keys.data());
      attr.batch.values = reinterpret_cast<uint64_t>(values.data());
      attr.batch.count = static_cast<uint32_t>(
          std::min<size_t>(batch_size, drain_limit - drained));
      int rc = syscall(SYS_bpf, BPF_MAP_LOOKUP_AND_DELETE_BATCH, &attr, sizeof(attr));
      int error = errno;
      for (uint32_t index = 0; index < attr.batch.count; ++index)
        append(keys[index], values[index]);
      drained += attr.batch.count;
      if (rc == 0) {
        if (!attr.batch.count) break;
        have_cursor = true;
        continue;
      }
      if (error == ENOENT) break;
      if (!have_cursor && (error == EINVAL || error == EOPNOTSUPP))
        batch_supported = false;
      break;
    }
    if (batch_supported) return result;
    for (size_t index = 0; index < drain_limit; ++index) {
      affinitygraph_futex_key key{};
      union bpf_attr next_attr{};
      next_attr.map_fd = futex_aggregates_fd_;
      next_attr.key = 0;
      next_attr.next_key = reinterpret_cast<uint64_t>(&key);
      if (syscall(SYS_bpf, BPF_MAP_GET_NEXT_KEY, &next_attr, sizeof(next_attr)) != 0)
        break;
      affinitygraph_futex_value value{};
      union bpf_attr take_attr{};
      take_attr.map_fd = futex_aggregates_fd_;
      take_attr.key = reinterpret_cast<uint64_t>(&key);
      take_attr.value = reinterpret_cast<uint64_t>(&value);
      if (syscall(SYS_bpf, BPF_MAP_LOOKUP_AND_DELETE_ELEM,
                  &take_attr, sizeof(take_attr)) != 0)
        continue;
      append(key, value);
    }
    return result;
  }

  std::vector<affinitygraph_bpf_event> drain_vfs_aggregates() {
    std::vector<affinitygraph_bpf_event> result;
    if (vfs_aggregates_fd_ < 0) return result;
    auto append = [&](const affinitygraph_vfs_key &key,
                      const affinitygraph_vfs_value &value) {
      affinitygraph_bpf_event event{};
      event.kind = AFFINITYGRAPH_VFS;
      event.tgid = key.tgid;
      event.tid = key.tid;
      event.peer_tid = key.peer_tid;
      event.timestamp_ns = value.last_timestamp_ns;
      event.start_time_ns = key.start_time_ns;
      event.peer_start_time_ns = key.peer_start_time_ns;
      event.value_ns = value.total_time_ns;
      event.resource = key.resource;
      result.push_back(event);
      stats_.vfs_aggregate_records++;
      stats_.vfs_handoffs += value.count;
    };
    constexpr uint32_t batch_size = 4096;
    constexpr size_t drain_limit = 16384;
    std::vector<affinitygraph_vfs_key> keys(batch_size);
    std::vector<affinitygraph_vfs_value> values(batch_size);
    affinitygraph_vfs_key cursor{};
    bool have_cursor = false;
    bool batch_supported = true;
    size_t drained = 0;
    while (drained < drain_limit) {
      union bpf_attr attr{};
      attr.batch.map_fd = vfs_aggregates_fd_;
      attr.batch.in_batch = have_cursor ? reinterpret_cast<uint64_t>(&cursor) : 0;
      attr.batch.out_batch = reinterpret_cast<uint64_t>(&cursor);
      attr.batch.keys = reinterpret_cast<uint64_t>(keys.data());
      attr.batch.values = reinterpret_cast<uint64_t>(values.data());
      attr.batch.count = static_cast<uint32_t>(
          std::min<size_t>(batch_size, drain_limit - drained));
      int rc = syscall(SYS_bpf, BPF_MAP_LOOKUP_AND_DELETE_BATCH, &attr, sizeof(attr));
      int error = errno;
      for (uint32_t index = 0; index < attr.batch.count; ++index)
        append(keys[index], values[index]);
      drained += attr.batch.count;
      if (rc == 0) {
        if (!attr.batch.count) break;
        have_cursor = true;
        continue;
      }
      if (error == ENOENT) break;
      if (!have_cursor && (error == EINVAL || error == EOPNOTSUPP))
        batch_supported = false;
      break;
    }
    if (batch_supported) return result;
    for (size_t index = 0; index < drain_limit; ++index) {
      affinitygraph_vfs_key key{};
      union bpf_attr next_attr{};
      next_attr.map_fd = vfs_aggregates_fd_;
      next_attr.key = 0;
      next_attr.next_key = reinterpret_cast<uint64_t>(&key);
      if (syscall(SYS_bpf, BPF_MAP_GET_NEXT_KEY, &next_attr, sizeof(next_attr)) != 0)
        break;
      affinitygraph_vfs_value value{};
      union bpf_attr take_attr{};
      take_attr.map_fd = vfs_aggregates_fd_;
      take_attr.key = reinterpret_cast<uint64_t>(&key);
      take_attr.value = reinterpret_cast<uint64_t>(&value);
      if (syscall(SYS_bpf, BPF_MAP_LOOKUP_AND_DELETE_ELEM,
                  &take_attr, sizeof(take_attr)) != 0)
        continue;
      append(key, value);
    }
    return result;
  }

  BpfReaderStats stats() const { return stats_; }

private:
  static uint64_t boot_ns() {
    timespec value{};
    clock_gettime(CLOCK_BOOTTIME, &value);
    return static_cast<uint64_t>(value.tv_sec) * 1000000000ULL + value.tv_nsec;
  }
  int fd_;
  int health_fd_;
  int futex_aggregates_fd_;
  int vfs_aggregates_fd_;
  size_t page_;
  size_t capacity_;
  void *consumer_map_ = MAP_FAILED;
  void *producer_map_ = MAP_FAILED;
  BpfReaderStats stats_;
};

std::shared_ptr<BpfRingReader> make_bpf_reader(int events_fd, int health_fd,
                                               int futex_aggregates_fd,
                                               int vfs_aggregates_fd) {
  if (events_fd < 0) return {};
  return std::make_shared<BpfRingReader>(events_fd, health_fd,
                                         futex_aggregates_fd,
                                         vfs_aggregates_fd);
}
std::vector<affinitygraph_bpf_event> drain_bpf_events(BpfRingReader &reader) { return reader.drain(); }
std::vector<affinitygraph_bpf_event> drain_bpf_futex_aggregates(BpfRingReader &reader) {
  return reader.drain_futex_aggregates();
}
std::vector<affinitygraph_bpf_event> drain_bpf_vfs_aggregates(BpfRingReader &reader) {
  return reader.drain_vfs_aggregates();
}
BpfHealthSnapshot bpf_reader_health(BpfRingReader &reader) { return reader.health(); }
BpfReaderStats bpf_reader_stats(BpfRingReader &reader) { return reader.stats(); }
} // namespace affinitygraph
