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
};

struct affinitygraph_bpf_event {
  ag_u32 kind;
  ag_u32 tgid;
  ag_u32 tid;
  ag_u32 peer_tid;
  ag_u64 timestamp_ns;
  ag_u64 value_ns;
  ag_u64 resource;
};

struct affinitygraph_bpf_health {
  ag_u64 emitted;
  ag_u64 dropped;
  ag_u64 emitted_by_kind[5];
  ag_u64 dropped_by_kind[5];
};

#endif
