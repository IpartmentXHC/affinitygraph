#ifndef AFFINITYGRAPH_BPF_EVENTS_H
#define AFFINITYGRAPH_BPF_EVENTS_H

#ifdef __cplusplus
#include <cstdint>
using ag_u32 = std::uint32_t;
using ag_u64 = std::uint64_t;
#else
typedef unsigned int ag_u32;
typedef unsigned long long ag_u64;
#endif

enum affinitygraph_event_kind {
  AFFINITYGRAPH_TASK_FORK = 1,
  AFFINITYGRAPH_TASK_EXIT = 2,
  AFFINITYGRAPH_FUTEX = 3,
  AFFINITYGRAPH_VFS = 4,
  AFFINITYGRAPH_TASK_EXEC = 5,
  AFFINITYGRAPH_TASK_RENAME = 6,
  AFFINITYGRAPH_AFFINITY = 7,
};

#define AFFINITYGRAPH_COMM_BYTES 16
#define AFFINITYGRAPH_MASK_BYTES 128
#define AFFINITYGRAPH_EVENT_KIND_COUNT 8
#define AFFINITYGRAPH_FUTEX_AGGREGATE_MAX_ENTRIES (256U << 10)
#define AFFINITYGRAPH_VFS_AGGREGATE_MAX_ENTRIES (256U << 10)

struct affinitygraph_target {
  ag_u32 root_tgid;
  ag_u32 reserved;
  ag_u64 start_time_ns;
};

struct affinitygraph_bpf_event {
  ag_u32 kind;
  ag_u32 tgid;
  ag_u32 tid;
  ag_u32 peer_tid;
  ag_u32 parent_tgid;
  ag_u32 parent_tid;
  ag_u32 mask_bytes;
  ag_u32 reserved;
  ag_u64 timestamp_ns;
  ag_u64 start_time_ns;
  ag_u64 peer_start_time_ns;
  ag_u64 value_ns;
  ag_u64 resource;
  ag_u64 start_routine;
  char comm[AFFINITYGRAPH_COMM_BYTES];
  unsigned char mask[AFFINITYGRAPH_MASK_BYTES];
};

struct affinitygraph_futex_key {
  ag_u32 tgid;
  ag_u32 tid;
  ag_u32 peer_tid;
  ag_u32 reserved;
  ag_u64 start_time_ns;
  ag_u64 peer_start_time_ns;
};

struct affinitygraph_futex_value {
  ag_u64 count;
  ag_u64 last_timestamp_ns;
};

struct affinitygraph_vfs_key {
  ag_u32 tgid;
  ag_u32 tid;
  ag_u32 peer_tid;
  ag_u32 reserved;
  ag_u64 start_time_ns;
  ag_u64 peer_start_time_ns;
  ag_u64 resource;
};

struct affinitygraph_vfs_value {
  ag_u64 total_time_ns;
  ag_u64 count;
  ag_u64 last_timestamp_ns;
};

struct affinitygraph_bpf_health {
  ag_u64 emitted;
  ag_u64 dropped;
  ag_u64 emitted_by_kind[AFFINITYGRAPH_EVENT_KIND_COUNT];
  ag_u64 dropped_by_kind[AFFINITYGRAPH_EVENT_KIND_COUNT];
  ag_u64 suppressed_by_kind[AFFINITYGRAPH_EVENT_KIND_COUNT];
};

#ifdef __cplusplus
static_assert(sizeof(affinitygraph_target) == 16);
static_assert(sizeof(affinitygraph_bpf_event) == 224);
static_assert(sizeof(affinitygraph_futex_key) == 32);
static_assert(sizeof(affinitygraph_futex_value) == 16);
static_assert(sizeof(affinitygraph_vfs_key) == 40);
static_assert(sizeof(affinitygraph_vfs_value) == 24);
#endif

#endif
