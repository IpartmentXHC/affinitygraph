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
#define RING_BYTES (1U << 20)

static void *(*bpf_map_lookup_elem)(void *, const void *) = (void *)BPF_FUNC_map_lookup_elem;
static long (*bpf_map_update_elem)(void *, const void *, const void *, __u64) = (void *)BPF_FUNC_map_update_elem;
static long (*bpf_map_delete_elem)(void *, const void *) = (void *)BPF_FUNC_map_delete_elem;
static long (*bpf_ringbuf_output)(void *, void *, __u64, __u64) = (void *)BPF_FUNC_ringbuf_output;
static __u64 (*bpf_get_current_pid_tgid)(void) = (void *)BPF_FUNC_get_current_pid_tgid;
static __u64 (*bpf_get_current_task)(void) = (void *)BPF_FUNC_get_current_task;
static __u64 (*bpf_ktime_get_boot_ns)(void) = (void *)BPF_FUNC_ktime_get_boot_ns;
static long (*bpf_probe_read_kernel)(void *, __u32, const void *) = (void *)BPF_FUNC_probe_read_kernel;

struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 64);
  __type(key, __u32);
  __type(value, __u8);
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

struct waiter_key { __u32 tgid; __u64 address; };
struct {
  __uint(type, BPF_MAP_TYPE_LRU_HASH);
  __uint(max_entries, 65536);
  __type(key, struct waiter_key);
  __type(value, __u32);
} waiters SEC(".maps");

struct resource_key { __u32 tgid; __u32 device; __u64 inode; };
struct resource_value { __u32 tid; __u64 timestamp_ns; };
struct {
  __uint(type, BPF_MAP_TYPE_LRU_HASH);
  __uint(max_entries, 65536);
  __type(key, struct resource_key);
  __type(value, struct resource_value);
} resources SEC(".maps");

static __always_inline int tracked(__u32 tgid) { return bpf_map_lookup_elem(&pids, &tgid) != 0; }

static __always_inline void emit(__u32 kind, __u32 tgid, __u32 tid, __u32 peer,
                                 __u64 value, __u64 resource) {
  struct affinitygraph_bpf_event event = {
    .kind = kind, .tgid = tgid, .tid = tid, .peer_tid = peer,
    .timestamp_ns = bpf_ktime_get_boot_ns(), .value_ns = value, .resource = resource,
  };
  __u32 zero = 0;
  struct affinitygraph_bpf_health *counters = bpf_map_lookup_elem(&health, &zero);
  long result = bpf_ringbuf_output(&events, &event, sizeof(event), 0);
  if (counters) {
    if (result) {
      __sync_fetch_and_add(&counters->dropped, 1);
      if (kind <= AFFINITYGRAPH_VFS) __sync_fetch_and_add(&counters->dropped_by_kind[kind], 1);
    } else {
      __sync_fetch_and_add(&counters->emitted, 1);
      if (kind <= AFFINITYGRAPH_VFS) __sync_fetch_and_add(&counters->emitted_by_kind[kind], 1);
    }
  }
}

SEC("tp_btf/sched_process_fork")
int on_fork(__u64 *ctx) {
  __u64 current = bpf_get_current_pid_tgid();
  __u32 tgid = current >> 32;
  if (!tracked(tgid)) return 0;
  struct task_struct *child = (struct task_struct *)ctx[1];
  __u32 child_tid = 0;
  bpf_probe_read_kernel(&child_tid, sizeof(child_tid), __builtin_preserve_access_index(&child->pid));
  emit(AFFINITYGRAPH_TASK_FORK, tgid, child_tid, (__u32)current, 0, 0);
  return 0;
}

SEC("tp_btf/sched_process_exit")
int on_exit(__u64 *ctx) {
  struct task_struct *task = (struct task_struct *)ctx[0];
  __u32 tgid = 0, tid = 0;
  bpf_probe_read_kernel(&tgid, sizeof(tgid), __builtin_preserve_access_index(&task->tgid));
  if (!tracked(tgid)) return 0;
  bpf_probe_read_kernel(&tid, sizeof(tid), __builtin_preserve_access_index(&task->pid));
  emit(AFFINITYGRAPH_TASK_EXIT, tgid, tid, 0, 0, 0);
  return 0;
}

SEC("tp/syscalls/sys_enter_futex")
int on_futex(struct trace_event_raw_sys_enter *ctx) {
  __u64 current = bpf_get_current_pid_tgid();
  __u32 tgid = current >> 32, tid = (__u32)current;
  if (!tracked(tgid)) return 0;
  struct waiter_key key = {.tgid = tgid, .address = ctx->args[0]};
  __u32 command = (__u32)ctx->args[1] & FUTEX_CMD_MASK;
  if (command == FUTEX_WAIT || command == FUTEX_WAIT_BITSET) {
    bpf_map_update_elem(&waiters, &key, &tid, BPF_ANY);
  } else {
    __u32 *waiter = bpf_map_lookup_elem(&waiters, &key);
    if (waiter && *waiter != tid) emit(AFFINITYGRAPH_FUTEX, tgid, tid, *waiter, 0, key.address);
    bpf_map_delete_elem(&waiters, &key);
  }
  return 0;
}

static __always_inline int account_fd(struct trace_event_raw_sys_enter *ctx) {
  __u64 current = bpf_get_current_pid_tgid();
  __u32 tgid = current >> 32, tid = (__u32)current;
  if (!tracked(tgid)) return 0;
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
  if (previous && previous->tid != tid) {
    __u64 elapsed = now > previous->timestamp_ns ? now - previous->timestamp_ns : 0;
    emit(AFFINITYGRAPH_VFS, tgid, tid, previous->tid, elapsed, key.inode);
  }
  struct resource_value next = {.tid = tid, .timestamp_ns = now};
  bpf_map_update_elem(&resources, &key, &next, BPF_ANY);
  return 0;
}

SEC("tp/syscalls/sys_enter_read") int on_read(struct trace_event_raw_sys_enter *ctx) { return account_fd(ctx); }
SEC("tp/syscalls/sys_enter_write") int on_write(struct trace_event_raw_sys_enter *ctx) { return account_fd(ctx); }

char LICENSE[] SEC("license") = "Dual BSD/GPL";
