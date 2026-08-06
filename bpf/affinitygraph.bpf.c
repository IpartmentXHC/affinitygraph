// SPDX-License-Identifier: (BSD-2-Clause OR GPL-2.0)
// Derived from the Prism taskstats/futex/VFS collector design.
#include "vmlinux.h"
#include "affinitygraph/bpf_events.h"

#define SEC(name) __attribute__((section(name), used))
#define __uint(name, value) int (*name)[value]
#define __type(name, value) value *name
#define __always_inline inline __attribute__((always_inline))
#define BPF_ANY 0
#define FUTEX_CMD_MASK 0x7f
#define FUTEX_WAIT 0
#define FUTEX_WAIT_BITSET 9
#define S_IFMT 00170000
#define S_IFIFO 0010000
#define S_IFCHR 0020000
#define RING_BYTES (16U << 20)
#define RENAME_MIN_INTERVAL_NS 500000000ULL

static void *(*bpf_map_lookup_elem)(void *, const void *) = (void *)BPF_FUNC_map_lookup_elem;
static long (*bpf_map_update_elem)(void *, const void *, const void *, __u64) = (void *)BPF_FUNC_map_update_elem;
static long (*bpf_map_delete_elem)(void *, const void *) = (void *)BPF_FUNC_map_delete_elem;
static long (*bpf_ringbuf_output)(void *, void *, __u64, __u64) = (void *)BPF_FUNC_ringbuf_output;
static __u64 (*bpf_get_current_pid_tgid)(void) = (void *)BPF_FUNC_get_current_pid_tgid;
static __u64 (*bpf_get_current_task)(void) = (void *)BPF_FUNC_get_current_task;
static __u64 (*bpf_ktime_get_boot_ns)(void) = (void *)BPF_FUNC_ktime_get_boot_ns;
static long (*bpf_probe_read_kernel)(void *, __u32, const void *) = (void *)BPF_FUNC_probe_read_kernel;
static long (*bpf_probe_read_kernel_str)(void *, __u32, const void *) = (void *)BPF_FUNC_probe_read_kernel_str;
static long (*bpf_probe_read_user)(void *, __u32, const void *) = (void *)BPF_FUNC_probe_read_user;

struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 1024);
  __type(key, __u32);
  __type(value, struct affinitygraph_target);
} pids SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_RINGBUF);
  __uint(max_entries, RING_BYTES);
} events SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_ARRAY);
  __uint(max_entries, 1);
  __type(key, __u32);
  __type(value, struct affinitygraph_bpf_health);
} health SEC(".maps");

struct thread_key { __u32 tgid; __u32 tid; };
struct rename_throttle_value { __u64 last_emit_ns; };
struct {
  __uint(type, BPF_MAP_TYPE_LRU_HASH);
  __uint(max_entries, 8192);
  __type(key, struct thread_key);
  __type(value, struct rename_throttle_value);
} rename_throttle SEC(".maps");

struct pending_pthread { __u64 routine; __u64 timestamp_ns; };
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 8192);
  __type(key, struct thread_key);
  __type(value, struct pending_pthread);
} pending_pthreads SEC(".maps");

struct waiter_key { __u32 tgid; __u64 address; };
struct waiter_value { __u32 tid; __u64 start_time_ns; };
struct {
  __uint(type, BPF_MAP_TYPE_LRU_HASH);
  __uint(max_entries, 65536);
  __type(key, struct waiter_key);
  __type(value, struct waiter_value);
} waiters SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 65536);
  __type(key, struct affinitygraph_futex_key);
  __type(value, struct affinitygraph_futex_value);
} futex_aggregates SEC(".maps");

struct resource_key { __u32 tgid; __u32 device; __u64 inode; };
struct resource_value { __u32 tid; __u64 timestamp_ns; __u64 start_time_ns; };
struct {
  __uint(type, BPF_MAP_TYPE_LRU_HASH);
  __uint(max_entries, 65536);
  __type(key, struct resource_key);
  __type(value, struct resource_value);
} resources SEC(".maps");

struct pending_affinity {
  __u32 target_tid;
  __u32 mask_bytes;
  unsigned char mask[AFFINITYGRAPH_MASK_BYTES];
};
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 8192);
  __type(key, struct thread_key);
  __type(value, struct pending_affinity);
} pending_affinities SEC(".maps");

static __always_inline __u64 task_start_time(struct task_struct *task) {
  __u64 value = 0;
  bpf_probe_read_kernel(&value, sizeof(value),
      __builtin_preserve_access_index(&task->start_boottime));
  return value;
}

static __always_inline int tracked_task(__u32 tgid, struct task_struct *task) {
  struct affinitygraph_target *target = bpf_map_lookup_elem(&pids, &tgid);
  if (!target) return 0;
  struct task_struct *leader = 0;
  bpf_probe_read_kernel(&leader, sizeof(leader),
      __builtin_preserve_access_index(&task->group_leader));
  if (!leader) leader = task;
  __u64 generation = task_start_time(leader);
  return !target->start_time_ns || target->start_time_ns == generation;
}

static __always_inline int current_tracked(__u32 tgid) {
  return tracked_task(tgid, (struct task_struct *)bpf_get_current_task());
}

static __always_inline void account_emit(__u32 kind, long result) {
  __u32 zero = 0;
  struct affinitygraph_bpf_health *counters = bpf_map_lookup_elem(&health, &zero);
  if (!counters) return;
  if (result) {
    __sync_fetch_and_add(&counters->dropped, 1);
    if (kind < AFFINITYGRAPH_EVENT_KIND_COUNT)
      __sync_fetch_and_add(&counters->dropped_by_kind[kind], 1);
  } else {
    __sync_fetch_and_add(&counters->emitted, 1);
    if (kind < AFFINITYGRAPH_EVENT_KIND_COUNT)
      __sync_fetch_and_add(&counters->emitted_by_kind[kind], 1);
  }
}

static __always_inline void emit_event(struct affinitygraph_bpf_event *event) {
  event->timestamp_ns = bpf_ktime_get_boot_ns();
  account_emit(event->kind,
      bpf_ringbuf_output(&events, event, sizeof(*event), 0));
}

static __always_inline void account_suppressed(__u32 kind) {
  __u32 zero = 0;
  struct affinitygraph_bpf_health *counters = bpf_map_lookup_elem(&health, &zero);
  if (counters && kind < AFFINITYGRAPH_EVENT_KIND_COUNT)
    __sync_fetch_and_add(&counters->suppressed_by_kind[kind], 1);
}

SEC("tp_btf/sched_process_fork")
int on_fork(__u64 *ctx) {
  __u64 current = bpf_get_current_pid_tgid();
  __u32 parent_tgid = current >> 32, parent_tid = (__u32)current;
  if (!current_tracked(parent_tgid)) return 0;
  struct task_struct *child = (struct task_struct *)ctx[1];
  struct affinitygraph_bpf_event event = {.kind = AFFINITYGRAPH_TASK_FORK};
  bpf_probe_read_kernel(&event.tgid, sizeof(event.tgid),
      __builtin_preserve_access_index(&child->tgid));
  bpf_probe_read_kernel(&event.tid, sizeof(event.tid),
      __builtin_preserve_access_index(&child->pid));
  struct thread_key child_key = {.tgid = event.tgid, .tid = event.tid};
  bpf_map_delete_elem(&rename_throttle, &child_key);
  event.parent_tgid = parent_tgid;
  event.parent_tid = parent_tid;
  event.start_time_ns = task_start_time(child);
  bpf_probe_read_kernel(&event.comm, sizeof(event.comm),
      __builtin_preserve_access_index(&child->comm));
  if (event.tgid != parent_tgid) {
    struct affinitygraph_target *parent = bpf_map_lookup_elem(&pids, &parent_tgid);
    struct affinitygraph_target next = {
      .root_tgid = parent ? parent->root_tgid : parent_tgid,
      .start_time_ns = event.start_time_ns,
    };
    bpf_map_update_elem(&pids, &event.tgid, &next, BPF_ANY);
  } else {
    struct thread_key key = {.tgid = parent_tgid, .tid = parent_tid};
    struct pending_pthread *pending = bpf_map_lookup_elem(&pending_pthreads, &key);
    if (pending) {
      event.start_routine = pending->routine;
      bpf_map_delete_elem(&pending_pthreads, &key);
    }
  }
  emit_event(&event);
  return 0;
}

SEC("tp_btf/sched_process_exec")
int on_exec(__u64 *ctx) {
  struct task_struct *task = (struct task_struct *)ctx[0];
  __u32 tgid = 0, tid = 0;
  bpf_probe_read_kernel(&tgid, sizeof(tgid),
      __builtin_preserve_access_index(&task->tgid));
  if (!tracked_task(tgid, task)) return 0;
  bpf_probe_read_kernel(&tid, sizeof(tid),
      __builtin_preserve_access_index(&task->pid));
  __u64 generation = task_start_time(task);
  struct affinitygraph_target *tracked = bpf_map_lookup_elem(&pids, &tgid);
  if (tracked && !tracked->start_time_ns) {
    struct affinitygraph_target initialized = *tracked;
    initialized.start_time_ns = generation;
    bpf_map_update_elem(&pids, &tgid, &initialized, BPF_ANY);
  }
  struct affinitygraph_bpf_event event = {
    .kind = AFFINITYGRAPH_TASK_EXEC, .tgid = tgid, .tid = tid,
    .start_time_ns = generation,
  };
  bpf_probe_read_kernel(&event.comm, sizeof(event.comm),
      __builtin_preserve_access_index(&task->comm));
  emit_event(&event);
  return 0;
}

SEC("tp_btf/sched_process_exit")
int on_exit(__u64 *ctx) {
  struct task_struct *task = (struct task_struct *)ctx[0];
  struct affinitygraph_bpf_event event = {.kind = AFFINITYGRAPH_TASK_EXIT};
  bpf_probe_read_kernel(&event.tgid, sizeof(event.tgid),
      __builtin_preserve_access_index(&task->tgid));
  if (!tracked_task(event.tgid, task)) return 0;
  bpf_probe_read_kernel(&event.tid, sizeof(event.tid),
      __builtin_preserve_access_index(&task->pid));
  event.start_time_ns = task_start_time(task);
  struct thread_key key = {.tgid = event.tgid, .tid = event.tid};
  bpf_map_delete_elem(&rename_throttle, &key);
  emit_event(&event);
  return 0;
}

SEC("tp_btf/task_rename")
int on_rename(__u64 *ctx) {
  struct task_struct *task = (struct task_struct *)ctx[0];
  struct affinitygraph_bpf_event event = {.kind = AFFINITYGRAPH_TASK_RENAME};
  bpf_probe_read_kernel(&event.tgid, sizeof(event.tgid),
      __builtin_preserve_access_index(&task->tgid));
  if (!tracked_task(event.tgid, task)) return 0;
  bpf_probe_read_kernel(&event.tid, sizeof(event.tid),
      __builtin_preserve_access_index(&task->pid));
  __u64 now = bpf_ktime_get_boot_ns();
  struct thread_key key = {.tgid = event.tgid, .tid = event.tid};
  struct rename_throttle_value *previous = bpf_map_lookup_elem(&rename_throttle, &key);
  if (previous && now - previous->last_emit_ns < RENAME_MIN_INTERVAL_NS) {
    account_suppressed(AFFINITYGRAPH_TASK_RENAME);
    return 0;
  }
  struct rename_throttle_value next = {.last_emit_ns = now};
  bpf_map_update_elem(&rename_throttle, &key, &next, BPF_ANY);
  event.start_time_ns = task_start_time(task);
  bpf_probe_read_kernel_str(event.comm, sizeof(event.comm), (void *)ctx[1]);
  emit_event(&event);
  return 0;
}

struct arm64_user_regs { __u64 regs[31]; __u64 sp; __u64 pc; __u64 pstate; };
SEC("uprobe")
int on_pthread_create(struct pt_regs *ctx) {
  __u64 current = bpf_get_current_pid_tgid();
  __u32 tgid = current >> 32, tid = (__u32)current;
  if (!current_tracked(tgid)) return 0;
  struct thread_key key = {.tgid = tgid, .tid = tid};
  struct pending_pthread value = {
#if defined(__TARGET_ARCH_arm64)
    .routine = ((struct arm64_user_regs *)ctx)->regs[2],
#else
    .routine = ctx->dx,
#endif
    .timestamp_ns = bpf_ktime_get_boot_ns(),
  };
  bpf_map_update_elem(&pending_pthreads, &key, &value, BPF_ANY);
  return 0;
}

SEC("uretprobe")
int on_pthread_create_return(struct pt_regs *ctx) {
  __u64 current = bpf_get_current_pid_tgid();
  if (!current_tracked(current >> 32)) return 0;
  struct thread_key key = {.tgid = current >> 32, .tid = (__u32)current};
  bpf_map_delete_elem(&pending_pthreads, &key);
  return 0;
}

SEC("tp/syscalls/sys_enter_sched_setaffinity")
int on_sched_setaffinity_enter(struct trace_event_raw_sys_enter *ctx) {
  __u64 current = bpf_get_current_pid_tgid();
  __u32 tgid = current >> 32, tid = (__u32)current;
  if (!current_tracked(tgid)) return 0;
  struct thread_key key = {.tgid = tgid, .tid = tid};
  struct pending_affinity value = {};
  value.target_tid = ctx->args[0] ? (__u32)ctx->args[0] : tid;
  __u64 requested_bytes = ctx->args[1];
  if (requested_bytes > AFFINITYGRAPH_MASK_BYTES)
    requested_bytes = AFFINITYGRAPH_MASK_BYTES;
  value.mask_bytes = (__u32)requested_bytes;
  if (value.mask_bytes)
    bpf_probe_read_user(value.mask, value.mask_bytes, (void *)ctx->args[2]);
  bpf_map_update_elem(&pending_affinities, &key, &value, BPF_ANY);
  return 0;
}

SEC("tp/syscalls/sys_exit_sched_setaffinity")
int on_sched_setaffinity_exit(struct trace_event_raw_sys_exit *ctx) {
  __u64 current = bpf_get_current_pid_tgid();
  struct thread_key key = {.tgid = current >> 32, .tid = (__u32)current};
  struct pending_affinity *pending = bpf_map_lookup_elem(&pending_affinities, &key);
  if (!pending) return 0;
  if (ctx->ret == 0) {
    struct affinitygraph_bpf_event event = {
      .kind = AFFINITYGRAPH_AFFINITY, .tgid = key.tgid, .tid = key.tid,
      .peer_tid = pending->target_tid, .mask_bytes = pending->mask_bytes,
    };
    __builtin_memcpy(event.mask, pending->mask, sizeof(event.mask));
    emit_event(&event);
  }
  bpf_map_delete_elem(&pending_affinities, &key);
  return 0;
}

SEC("tp/syscalls/sys_enter_futex")
int on_futex(struct trace_event_raw_sys_enter *ctx) {
  __u64 current = bpf_get_current_pid_tgid();
  __u32 tgid = current >> 32, tid = (__u32)current;
  if (!current_tracked(tgid)) return 0;
  struct task_struct *task = (struct task_struct *)bpf_get_current_task();
  struct waiter_key key = {.tgid = tgid, .address = ctx->args[0]};
  __u32 command = (__u32)ctx->args[1] & FUTEX_CMD_MASK;
  if (command == FUTEX_WAIT || command == FUTEX_WAIT_BITSET) {
    struct waiter_value value = {.tid = tid, .start_time_ns = task_start_time(task)};
    bpf_map_update_elem(&waiters, &key, &value, BPF_ANY);
  } else {
    struct waiter_value *waiter = bpf_map_lookup_elem(&waiters, &key);
    if (waiter && waiter->tid != tid) {
      struct affinitygraph_futex_key aggregate_key = {
        .tgid = tgid, .tid = tid, .peer_tid = waiter->tid,
        .start_time_ns = task_start_time(task),
        .peer_start_time_ns = waiter->start_time_ns,
      };
      struct affinitygraph_futex_value *aggregate =
          bpf_map_lookup_elem(&futex_aggregates, &aggregate_key);
      long result = 0;
      if (aggregate) {
        __sync_fetch_and_add(&aggregate->count, 1);
        aggregate->last_timestamp_ns = bpf_ktime_get_boot_ns();
      } else {
        struct affinitygraph_futex_value value = {
          .count = 1, .last_timestamp_ns = bpf_ktime_get_boot_ns(),
        };
        result = bpf_map_update_elem(&futex_aggregates, &aggregate_key, &value, BPF_ANY);
      }
      account_emit(AFFINITYGRAPH_FUTEX, result);
    }
    bpf_map_delete_elem(&waiters, &key);
  }
  return 0;
}

static __always_inline int account_fd(struct trace_event_raw_sys_enter *ctx) {
  __u64 current = bpf_get_current_pid_tgid();
  __u32 tgid = current >> 32, tid = (__u32)current;
  if (!current_tracked(tgid)) return 0;
  struct task_struct *task = (struct task_struct *)bpf_get_current_task();
  struct files_struct *files = 0;
  struct fdtable *table = 0;
  struct file **fds = 0;
  struct file *file = 0;
  struct inode *inode = 0;
  umode_t mode = 0;
  unsigned int fd = (__u32)ctx->args[0];
  bpf_probe_read_kernel(&files, sizeof(files), __builtin_preserve_access_index(&task->files));
  if (!files) return 0;
  bpf_probe_read_kernel(&table, sizeof(table), __builtin_preserve_access_index(&files->fdt));
  if (!table) return 0;
  bpf_probe_read_kernel(&fds, sizeof(fds), __builtin_preserve_access_index(&table->fd));
  if (!fds || fd >= 4096) return 0;
  bpf_probe_read_kernel(&file, sizeof(file), &fds[fd]);
  if (!file) return 0;
  bpf_probe_read_kernel(&inode, sizeof(inode), __builtin_preserve_access_index(&file->f_inode));
  if (!inode) return 0;
  bpf_probe_read_kernel(&mode, sizeof(mode), __builtin_preserve_access_index(&inode->i_mode));
  if ((mode & S_IFMT) != S_IFIFO && (mode & S_IFMT) != S_IFCHR) return 0;
  struct resource_key key = {.tgid = tgid};
  bpf_probe_read_kernel(&key.inode, sizeof(key.inode), __builtin_preserve_access_index(&inode->i_ino));
  bpf_probe_read_kernel(&key.device, sizeof(key.device), __builtin_preserve_access_index(&inode->i_rdev));
  struct resource_value *previous = bpf_map_lookup_elem(&resources, &key);
  __u64 now = bpf_ktime_get_boot_ns();
  __u64 generation = task_start_time(task);
  if (previous && previous->tid != tid) {
    struct affinitygraph_bpf_event event = {
      .kind = AFFINITYGRAPH_VFS, .tgid = tgid, .tid = tid,
      .peer_tid = previous->tid, .start_time_ns = generation,
      .peer_start_time_ns = previous->start_time_ns,
      .value_ns = now > previous->timestamp_ns ? now - previous->timestamp_ns : 0,
      .resource = key.inode,
    };
    emit_event(&event);
  }
  struct resource_value next = {.tid = tid, .timestamp_ns = now, .start_time_ns = generation};
  bpf_map_update_elem(&resources, &key, &next, BPF_ANY);
  return 0;
}

SEC("tp/syscalls/sys_enter_read") int on_read(struct trace_event_raw_sys_enter *ctx) { return account_fd(ctx); }
SEC("tp/syscalls/sys_enter_write") int on_write(struct trace_event_raw_sys_enter *ctx) { return account_fd(ctx); }

char LICENSE[] SEC("license") = "Dual BSD/GPL";
