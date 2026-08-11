# AffinityGraph Setup Report

生成时间：2026-08-10（Asia/Shanghai）
执行位置：local，主机 `x`，用户 `x`
初始 git commit：`db6d4e2973512538bad6204dca8e20a9e7c77ae8`
git 状态：仅有本次 ops 入口、配置和报告改动；核心算法源码未修改。

## 1. sudo askpass 固化

- 配置目录：`/home/xhc/.config/affinitygraph/`，权限 `0700`
- config.env：`/home/xhc/.config/affinitygraph/config.env`，权限 `0600`
- askpass：`/home/xhc/.config/affinitygraph/askpass`，权限 `0700`
- 属主：`xhc:xhc`
- `source config.env && sudo -A true`：通过
- helper 内容未读取、未打印、未写入仓库或报告。

## 2. 测试入口

- 脚本目录：`scripts/ops/`
- 配置文件：`config/ops.toml`
- Makefile targets：`ops-env-check`、`ops-local-test`、`ops-cloud-build`、`ops-cloud-preflight`、`ops-archive-*`、`ops-cloud-clean-*`
- 本地入口只做静态检查；所有编译和可执行测试均在 `kunpen183` 完成。
- 最近不可变 release：`/home/xhc/.local/share/affinitygraph/releases/1a082f00d21c5b27`

## 3. 本地实验数据归档

- dry-run 清单：`.agent/setup/local-archive-plan.md`
- 详细机器清单：`.agent/setup/local-archive-plan.tsv`（git ignored）
- 识别项目：382（按逻辑实验目录去重，不重复统计父子目录）
- 已归档数量：0
- 已删除源目录：否
- 状态：项目仓库内部没有实验数据候选；跨项目旧实验全部标为 `no-action`，等待人工确认。

## 4. 云端实验数据清理

- dry-run 清单：`.agent/setup/cloud-clean-plan.md`
- 详细机器清单：`.agent/setup/cloud-clean-plan.tsv`（git ignored）
- 识别云端实验：47
- 已删除数量：0
- 自动验证可删除：0
- 状态：同名本地目录不足以证明完整备份；文件清单或关键哈希未完全一致，因此全部保留。
- 后续 apply 需要用户确认清单，并传入对应 TSV 的 SHA256；脚本只会逐个处理清单中的精确路径。

## 5. 环境验证

- env-check：通过；SSH BatchMode、clang/clang++ 18、bpftool、libbpf.so.1、BTF 和 sudo askpass 均通过。
- local-test：通过；执行 `git diff --check`、shell 语法和 Makefile dry-run，不在本机编译。
- cloud-build：通过；`make -j8 runtime-test CXX=/usr/bin/clang++-18` 与 `make bpf CLANG=/usr/bin/clang-18` 成功。
- cloud-preflight：通过；Linux 6.12.64，CPU `0-127`，CO-RE BPF 通过 libbpf 加载并挂载。
- 未运行：数据库、observe/plan/active、smoke 或正式实验。

## 6. 给 Explain Agent 的入口命令

```bash
cd /data/threadState/affinitygraph
make ops-env-check
```

云端直接检查时：

```bash
source /home/xhc/.config/affinitygraph/config.env
sudo -A true
```

## 7. 给 Modify Agent 的入口命令

```bash
cd /data/threadState/affinitygraph
make ops-local-test
make ops-cloud-build
make ops-cloud-preflight CONFIG=<config.toml>
```

归档和云端清理仍处于 dry-run 状态，未经用户确认不得运行对应 `*-apply` target。
