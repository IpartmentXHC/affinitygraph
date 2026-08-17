# AffinityGraph 外部依赖清单

本目录统一登记 AffinityGraph 的外部依赖。仓库级依赖直接以
`git subtree` 形式放入 `dep/` 下随主仓库分发；系统级依赖无法入库，
以清单形式记录（检查工具见 `scripts/ops/env-check.sh` 与
`affinity-run preflight`）。

## 仓库依赖（随仓库分发）

- `dep/core-to-core-latency/` — `make calibrate` 的 CPU 延迟基准。
  上游：<https://github.com/nviennot/core-to-core-latency>，以
  `git subtree --squash` 导入（提交 `9d231aa`）。构建使用
  `.cargo/config.toml` 指向仓库内 `vendor/`（cargo 离线源），内网无需
  访问 crates.io。
- 更新流程（需在联网机执行）：
  1. `git subtree pull --prefix=dep/core-to-core-latency core-to-core-latency main --squash`
  2. 若 `Cargo.lock` 有变化：`cd dep/core-to-core-latency && cargo vendor vendor`
  3. 提交 vendor 变更后再推送。

## 系统依赖（运行时）

- Linux 6.12/ARM64（或经评审的等价内核），`/sys/kernel/btf/vmlinux` 存在
  （BPF CO-RE 必需）。
- `libbpf.so.1`（`affinity-run` 通过 `dlopen` 动态加载）。
- systemd（部署 `deploy/affinitygraph@.service` 实例）。
- `/proc`、sysfs 拓扑接口（`/sys/devices/system/cpu|cpuN/node*`）。

## 系统依赖（构建/校准）

- C++20 编译器（g++ 或 clang++-18）、GNU make。
- clang（带 BPF 后端，用于 `make bpf`）、bpftool（生成 `vmlinux.h`）。
- cargo/rust（`dep/core-to-core-latency` 离线构建）、python3、taskset
  （`make calibrate`）。
- 可选：STREAM 二进制（`--stream-bin`）或 p95/memory/STREAM CSV，用于
  生产级 calibration，避免启发式估计。

## 系统依赖（ops 脚本）

- ssh/scp、sha256sum、numfmt、zstd（`scripts/ops/*`）。

## 工作区协同仓库（非依赖）

- `ycsb-bench-all-database/` — 压测/实验工作负载仓库，位于工作区根目录
  （非 affinitygraph 代码引用），仅与实验编排协同，不进入 `dep/`。
