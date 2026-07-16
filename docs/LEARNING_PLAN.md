# tinydocker 14 天学习计划

## 使用方式

本计划按当前仓库真实模块安排，每天 1.5～2.5 小时。默认先在当前开发机完成非特权阅读和 `make check`；涉及 namespace、mount、cgroup、bridge/veth 或 iptables 的命令，只能在可丢弃 Linux VM 的 root shell 中执行。

建议每天保留一页笔记，固定记录四类内容：调用链、资源所有权、失败回滚、一个仍未解决的问题。不要把顶层 `test.c` 或 `docker.md` 中的早期示例当作当前实现；它们只用于比较项目演进。

## 第 1 天：建立项目地图（1.5～2 小时）

### 当天目标

知道项目解决什么问题、明确不解决什么问题，并能把顶层目录和正式/历史材料区分开。

### 需要阅读的具体文件

- `README.md`
- `main.c`
- `makefile`
- `core/config.h`
- `cmdparser/cmdparser.h`
- `docker/container.h`
- `docker/network.h`
- `graphify-out/GRAPH_REPORT.md`
- `.codegraph/.gitignore` 与 `.codegraph/daemon.log`

### 需要理解的具体函数

- `main()`
- `parse_docker_cmd()` 的函数签名和返回结构
- `init_runtime_dirs()`
- `init_docker_env()`

### 需要手动运行的命令

```bash
git rev-parse --short=8 HEAD
git status --short
rg --files --hidden -g '!.git/**'
rg -n '^##|^# ' README.md graphify-out/GRAPH_REPORT.md
sqlite3 .codegraph/codegraph.db 'select count(*) from files; select count(*) from nodes; select count(*) from edges;'
```

记录图谱版本与当前 HEAD 的差异：`graphify-out` 报告内写的是旧提交，`.codegraph` 也没有完整索引当前 `core/` 和测试。

### 需要回答的自测问题

1. 为什么项目应称为“教学型容器运行时”，不能称为 Docker/runc 替代品？
2. 哪些目录属于 runtime，哪些目录是可移植 core？
3. 哪些顶层文件是历史实验材料而不是正式构建输入？
4. `main()` 当前真正接通了哪些命令？

### 当天完成标准

能不看 README 画出一张包含 CLI、core、container、workspace、cgroup、network、status、tests 的模块图，并在每个模块旁写出一句真实职责。

## 第 2 天：构建、运行边界与测试入口（1.5～2.5 小时）

### 当天目标

理解 make target、平台分支、编译宏、依赖、CI job 和特权测试闸门。

### 需要阅读的具体文件

- `makefile`
- `.github/workflows/ci.yml`
- `docs/FULL_FUNCTION_TEST.md`
- `tests/run_privileged.sh`
- `demo.sh`
- `core/config.h`

### 需要理解的具体函数

- `init_runtime_dirs()`
- `init_docker_env()`
- `demo.sh` 的 `preflight()`、`verify_resource_ownership()`、`verify_cleanup()`
- `tests/run_privileged.sh` 的入口检查和 suite 分派

### 需要手动运行的命令

```bash
make clean
make check
make static-check
make sanitize
```

在 macOS 上可额外运行一次 `make`，观察它如何明确拒绝构建 Linux runtime；在 Linux 上运行 `make clean` 后再运行 `make`，确认链接需要 `libcrypto`。

### 需要回答的自测问题

1. `CORE_SOURCES` 与 `RUNTIME_SOURCES` 为什么分开？
2. macOS 的 `static-check` 与 Linux 的检查范围有什么不同？
3. CI 为什么不默认运行特权集成测试？
4. `RUNTIME_DIR` 与 `CGROUP_PARENT` 如何在编译时覆盖？
5. `make sanitize` 实际覆盖和没有覆盖哪些文件？

### 当天完成标准

`make check` 通过；能口述 7 个主要 make target 的用途，并能说明“本机 core 测试通过”为什么不能推出“Linux 特权 runtime 已验证”。

## 第 3 天：CLI 解析与 `main()` 分派（2～2.5 小时）

### 当天目标

从 argv 一直跟到具体 `docker_*` 入口，掌握所有参数对象和入口校验。

### 需要阅读的具体文件

- `main.c`
- `cmdparser/cmdparser.h`
- `cmdparser/cmdparser.c`
- `core/safety.h`
- `core/safety.c`

### 需要理解的具体函数

- `parse_docker_cmd()`
- `docker_run_parse_func()`
- `docker_run_cmd_check()`
- `docker_exec_parse_func()`
- `docker_stop_parse_func()`
- `parse_volume_config()`
- `validate_container_name()`、`validate_network_name()`
- `td_validate_name()`、`td_parse_long()`、`td_parse_port_mapping()`、`td_parse_volume_spec()`、`td_parse_ipv4_cidr()`

### 需要手动运行的命令

```bash
rg -n 'struct docker_|enum docker_command_type' cmdparser/cmdparser.h
rg -n 'strcmp\(action|strcmp\(argv\[2\]' cmdparser/cmdparser.c
rg -n 'case DOCKER_' main.c cmdparser/cmdparser.c
make test
```

在纸上分别展开以下三个输入的参数对象：

```text
tinydocker run -d -n demo -c 20000 -m 134217728 -p 18080:8080 busybox.tar.xz /bin/sh -c 'sleep 30'
tinydocker exec -e MODE=test demo /bin/sh -c 'echo ok'
tinydocker stop -t 1 demo
```

### 需要回答的自测问题

1. 为什么参数解析后仍需在底层路径构造函数再次校验名称？
2. `docker_run_arguments.container_argv` 如何形成 NULL 结尾数组？
3. enum 中哪两个 network 命令没有真正接到 CLI？
4. 参数解析失败为什么通常返回 255？
5. volume、port、CPU、memory 的边界分别在哪里校验？

### 当天完成标准

能从任意一个受支持命令写出 `argv -> 参数结构 -> main switch -> 实现函数` 的完整路径，并指出至少三个解析器错误处理不统一之处。

## 第 4 天：`docker_run()` 父进程主执行路径（2～2.5 小时）

### 当天目标

掌握创建容器时宿主父进程的严格顺序，以及每一步产生的资源。

### 需要阅读的具体文件

- `docker/container.c` 第 32～322、448～704 行
- `docker/cgroup.c`
- `docker/network.h`
- `docker/status_info.h`

### 需要理解的具体函数

- `container_exists()`
- `docker_run()`
- `cgroup_prepare()`、`cgroup_apply()`、`cgroup_cleanup()`
- `init_cgroup()`、`set_cgroup_limits()` 等被生命周期接口复用的 legacy helper
- `connect_container()` 的接口契约
- `create_container_info()`、`write_container_info()` 的接口契约
- `cleanup_run_failure()`

### 需要手动运行的命令

```bash
nl -ba docker/container.c | sed -n '448,704p'
rg -n 'goto fail_cleanup_run|cleanup_run_failure|pipe_created|child_pid|ip_addr' docker/container.c
rg -n 'cgroup_prepare|cgroup_apply|cgroup_cleanup' docker/cgroup.c docker/container.c
rg -n 'create_container_info|write_container_info' docker/container.c docker/status_info.c
```

自己画一张“步骤—资源—失败回滚函数”的三列表。

### 需要回答的自测问题

1. 为什么 cgroup 目录和 limit 在 clone 前创建，但 PID 在 clone 后加入？
2. 为什么 pipe 必须在 clone 前创建，而父进程读端要在 clone 后关闭？
3. metadata 为什么必须在发送授权字节前写完？
4. detached 与 foreground 从哪一行开始分叉？
5. foreground 如何转发信号并传播退出码？

### 当天完成标准

可以闭卷写出 `docker_run()` 的 14 个关键步骤；对任一步失败，都能说出已创建的资源和对应清理动作。

## 第 5 天：子进程、rootfs、volume 与 `exec`（2～2.5 小时）

### 当天目标

理解容器内进程如何建立文件系统视图，以及 `exec` 为什么使用 namespace worker + command child。

### 需要阅读的具体文件

- `docker/container.c` 第 282～462、740～863 行
- `docker/workspace.c`
- `docker/volumes.c`
- `core/fs.c`
- `core/process.c`
- `util/utils.c` 第 173～207 行

### 需要理解的具体函数

- `child_fn()`
- `load_process_env()`
- `init_container_workerspace()`
- `create_readonly_layer()`、`create_readwrite_layer()`、`create_workspace_mount_point()`
- `init_and_set_new_root()`
- `mount_volumes()`、`umount_volumes()`
- `td_ensure_directory_beneath()`
- `extract_tar()`
- `docker_exec()`

### 需要手动运行的命令

```bash
rg -n 'clone\(|pivot_root|setns\(|execve\(|MS_PRIVATE|MS_BIND|mount\("proc"' docker
rg -n 'O_NOFOLLOW|mkdirat|openat' core/fs.c docker/volumes.c
rg -n 'tar", "-tf|--no-same-owner|td_archive_entry_is_safe' util/utils.c core/safety.c
make test
```

将 README 的流程图与当前 `child_fn()` 对照，明确标出 rootfs 初始化和 pipe 等待的真实先后关系。

### 需要回答的自测问题

1. 为什么当前屏障保证用户命令不早跑，却不保证 `pivot_root` 在授权后发生？
2. OverlayFS 的 lowerdir、upperdir、workdir、mountpoint 分别属于谁？
3. `td_ensure_directory_beneath()` 如何阻止 symlink 逃逸？
4. `setns(CLONE_NEWPID)` 后为什么还要再 fork？
5. `extract_tar()` 已防住什么，尚未完整防住什么？

### 当天完成标准

能画出父进程与 clone 子进程的双泳道图，并能独立解释 `pivot_root` 五步、volume rollback 和 `exec` 双 fork。

## 第 6 天：核心数据结构与严格 codec（1.5～2.5 小时）

### 当天目标

掌握内存对象、磁盘格式和 parser/serializer 的一致性约束。

### 需要阅读的具体文件

- `core/container_state.h`
- `core/status_codec.h`
- `core/status_codec.c`
- `core/network_state.h`
- `core/network_state.c`
- `tests/test_core.c` 中 `test_status_codec()` 与 `test_network_state_codec()`

### 需要理解的具体函数

- `td_parse_container_info()`
- `parse_key_value()`
- `td_write_container_info()`
- `td_parse_network_record()`
- `td_format_network_record()`
- `td_network_record_names_are_unique()`
- `validate_cidr()`、`validate_allocated_addresses()`

### 需要手动运行的命令

```bash
rg -n '^struct |^enum |#define TD_NETWORK' core/container_state.h core/network_state.h
rg -n 'required_fields|claim_field|unknown metadata|duplicate' core/status_codec.c
rg -n 'TD_NETWORK_MAX|network address|duplicate|usable CIDR' core/network_state.c tests/test_core.c
make test
```

手写一份最小合法 container metadata 和一条合法 network record，再逐字段对照 parser。

### 需要回答的自测问题

1. 哪些 metadata 字段必填？为什么 `pid_start_time` 不是必填？
2. parser 如何发现重复字段和额外 volume？
3. 为什么内部 `name` 必须等于文件名？
4. network CIDR 为什么必须写网络地址而不是 gateway 地址？
5. 容器 volume 上限和 CLI 上限是否一致？后果是什么？

### 当天完成标准

能不看代码写出两种磁盘格式，并列出各自至少 8 个拒绝条件；明确记录“CLI 128 volume vs metadata 32 volume”这一真实缺陷。

## 第 7 天：状态、cgroup 与资源生命周期（2～2.5 小时）

### 当天目标

从 RUNNING 到 STOPPED/EXITED/rm，理解进程身份、cgroup 和 metadata 如何协同。

### 需要阅读的具体文件

- `docker/status_info.c`
- `docker/cgroup.c`
- `core/cgroup_parse.c`
- `docker/container.c` 第 652～740、865～1168 行
- `core/config.h`

### 需要理解的具体函数

- `read_process_start_time()`
- `create_container_info()`
- `read_container_info()`、`write_container_info()`
- `refresh_container_status_if_needed()`
- `list_containers_info()`
- `get_container_processes_id()`
- `docker_ps()`、`docker_top()`、`docker_stop()`、`docker_inspect()`、`docker_stats()`
- `td_parse_cgroup_stat()`、`td_parse_cgroup_pid_list()`、`td_format_bytes()`

### 需要手动运行的命令

```bash
rg -n 'pid_start_time|RUNNING|STOPPED|EXITED' core docker tests
rg -n 'memory.max|cpu.max|cgroup.procs|pids.current|pids.max|cpu.stat' docker core
nl -ba docker/status_info.c | sed -n '394,442p'
make test
```

### 需要回答的自测问题

1. 为什么 PID start time 能识别 PID reuse？仍有什么窗口？
2. lazy refresh 在哪些命令中发生？
3. `stop` 什么条件下才写 STOPPED？
4. `stats` 哪些指标来自哪些 cgroup 文件？
5. 为什么项目拒绝自动创建未知 cgroup parent？

### 当天完成标准

完成一张状态转换图，并能解释 RUNNING metadata、实际进程和 cgroup 三者不一致时各命令的行为。

## 第 8 天：错误路径与保守清理（2～2.5 小时）

### 当天目标

把失败回滚当作主流程学习，理解“保留残留”何时比“强制删除”更安全。

### 需要阅读的具体文件

- `docker/container.c` 第 87～280、886～1003 行
- `docker/volumes.c`
- `docker/workspace.c`
- `docker/network.c` 中 create/delete/rollback 分支
- `core/fs.c`
- `demo.sh` 的 cleanup
- `tests/test_full_function.sh` 与 `tests/test_observability.sh` 的 cleanup

### 需要理解的具体函数

- `cleanup_run_failure()`
- `clean_container_runtime()`
- `docker_rm()`
- `mount_volumes()` 的 `rollback` 标签
- `remove_cgroup()`、`remove_status_info()`、`td_remove_tree()`
- `delete_owned_bridge()`、`delete_owned_veth()`

### 需要手动运行的命令

```bash
rg -n 'goto cleanup|goto rollback|goto fail_cleanup_run|refusing|kept|incomplete' docker core demo.sh tests
rg -n 'errno != EINVAL|errno != ENOENT|errno == ENOENT' docker core
make test
```

为以下故障各写一条预期清理序列：OverlayFS mount 后 volume 失败、clone 后网络失败、metadata 写失败、volume unmount 失败、veth ownership 不匹配。

### 需要回答的自测问题

1. 为什么要先杀 child 再卸载 mountpoint？
2. 为什么 mount cleanup 失败时不能递归删 workspace？
3. 为什么 `rm` 遇到损坏 metadata 会拒绝继续？
4. 哪些删除操作是幂等的？
5. 正常 `rm` 为什么最后才删除 metadata？

### 当天完成标准

完成至少 5 条故障注入推演，每条都能列出已创建资源、回滚顺序、保留证据和最终退出结果。

## 第 9 天：网络与并发控制（2～2.5 小时）

### 当天目标

理解网络状态文件、IP 分配、bridge/veth 配置、DNAT/MASQUERADE 和 ownership cleanup。

### 需要阅读的具体文件

- `docker/network.h`
- `docker/network.c`
- `core/network_state.c`
- `core/process.c`
- `tests/test_full_function.sh` 第 154～176 行

### 需要理解的具体函数

- `load_network_records()`、`lock_network_state()`、`write_network_records()`
- `create_network()`、`create_default_bridge()`、`delte_network()`
- `alloc_new_ip()`、`release_used_ip()`
- `connect_container()`、`disconnect_container()`
- `set_container_port_map()`、`delete_dnat_rules_for_chain()`、`unset_container_port_map()`
- `verify_interface_ownership()`

### 需要手动运行的命令

```bash
rg -n 'flock|rename\(|fsync|ifalias|if_nametoindex' docker/network.c
rg -n 'iptables|brctl|nsenter|ip", "link|ip", "addr|ip", "route' docker/network.c
rg -n 'alloc_new_ip|release_used_ip|used_ip_count' docker/network.c core/network_state.c
make test
```

在纸上画出 host bridge、host veth、container eth0、默认路由、OUTPUT/PREROUTING DNAT、POSTROUTING MASQUERADE。

### 需要回答的自测问题

1. 为什么容器 IP 从网络地址 +2 开始？
2. veth 名如何满足 15 字节限制并降低同前缀碰撞？
3. network writer lock 保护了什么，没保护什么？
4. 为什么删除接口要检查 alias、类型和 ifindex？
5. port mapping 为什么 inspect 无法展示？

### 当天完成标准

能从 `alloc_new_ip()` 讲到 cleanup 释放 IP，且能明确指出“状态文件原子性”和“宿主网络资源事务性”不是一回事。

## 第 10 天：安全设计与边界处理（2～2.5 小时）

### 当天目标

系统梳理输入、路径、文件、进程和资源 ownership 的防护，同时准确说明生产安全能力缺口。

### 需要阅读的具体文件

- `core/safety.c`
- `core/fs.c`
- `core/process.c`
- `docker/status_info.c`
- `docker/network.c` 的安全文件打开和 ownership 部分
- `docker/volumes.c`
- `util/utils.c`
- `README.md` 的“风险警告”和“已知限制”

### 需要理解的具体函数

- `td_validate_name()`、`td_join_rootfs_path()`、`td_archive_entry_is_safe()`
- `td_ensure_directory_beneath()`、`td_remove_tree()`
- `td_run_command()`、`td_capture_command()`
- `write_container_info()`、`read_container_info()`
- `read_interface_alias()`、`verify_interface_ownership()`
- `mount_volumes()`

### 需要手动运行的命令

```bash
rg -n 'O_NOFOLLOW|O_EXCL|O_DIRECTORY|openat|mkdirat|renameat|fsync|FTW_PHYS' core docker
rg -n 'system\(|popen\(|/bin/sh -c|execvp|execve' --glob '*.[ch]'
rg -n 'seccomp|capability|user namespace|rootless|pidfd|OCI' README.md docker.md
make sanitize
```

注意：搜索结果中的 `test.c` 是旧实验文件；正式 runtime 的外部命令路径使用 argv + exec，不使用 shell。

### 需要回答的自测问题

1. 项目如何防 metadata symlink 替换？
2. `FTW_PHYS` 和 `openat(O_NOFOLLOW)` 各解决什么问题？
3. 无 shell 执行为什么降低命令注入风险？
4. archive 检查为什么仍不能等同于安全解包器？
5. 缺少 userns/seccomp/capability drop 对威胁模型意味着什么？

### 当天完成标准

完成一张“威胁—现有控制—剩余风险”表，至少覆盖名称、volume path、metadata、network state、PID reuse、archive、bridge/veth 和 root 权限。

## 第 11 天：非特权测试、Sanitizer 与 CI（1.5～2.5 小时）

### 当天目标

把每个 core 测试映射到生产函数，理解 CI 能证明与不能证明什么。

### 需要阅读的具体文件

- `tests/test_core.c`
- `makefile`
- `.github/workflows/ci.yml`
- `core/*.c` 中被测试的函数

### 需要理解的具体函数

- `test_name_validation()`
- `test_numeric_and_port_parsing()`
- `test_safe_paths()`
- `test_network_state_codec()`
- `test_cidr_and_cgroup_parsing()`
- `test_proc_stat_parser()`
- `test_status_codec()`
- `test_cleanup_idempotency()`
- `test_directory_creation_stays_beneath_rootfs()`
- `test_shell_free_command_runner()`

### 需要手动运行的命令

```bash
make clean
make test
make static-check
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 make sanitize
make check
```

### 需要回答的自测问题

1. 每个测试函数对应哪些 production 函数？
2. 哪些安全分支只有负例测试，哪些有 round-trip 测试？
3. Linux static-check 比 macOS 多检查哪些文件？
4. 为什么 core sanitizer 通过仍不能覆盖 `docker_run()`？
5. CI 的三个 job 为什么拆开而不是只运行 `make check`？

### 当天完成标准

所有本机可运行目标通过；完成“测试函数—被测函数—未覆盖分支”矩阵，并找出至少 5 个 runtime 未自动覆盖点。

## 第 12 天：特权测试与问题复现（2～2.5 小时）

### 当天目标

先审查测试安全契约，再在合适环境中复现 observability 或 full 流程；没有 disposable Linux VM 时完成等价的静态演练，不冒险执行。

### 需要阅读的具体文件

- `docs/FULL_FUNCTION_TEST.md`
- `tests/run_privileged.sh`
- `tests/test_observability.sh`
- `tests/test_full_function.sh`
- `demo.sh`

### 需要理解的具体函数

- shell `cleanup()`、`run_capture()`、`assert_contains()`、`assert_file_contains()`
- `demo.sh` 的 `preflight()`、`run_container()`、`verify_cleanup()`
- 对应 runtime 的 `docker_inspect()`、`docker_stats()`、`refresh_container_status_if_needed()`、`docker_stop()`、`docker_rm()`

### 需要手动运行的命令

先做只读前置检查：

```bash
uname -m
stat -fc %T /sys/fs/cgroup
grep -w overlay /proc/filesystems
command -v gcc make ip brctl iptables nsenter ps tar
```

只有在可丢弃 Linux VM、root shell、可信且架构匹配的 rootfs 中，才选择一个 suite：

```bash
TINYDOCKER_ALLOW_PRIVILEGED_TESTS=1 PRIVILEGED_SUITE=observability IMAGE=/path/to/trusted-rootfs.tar.xz bash tests/run_privileged.sh
```

若 observability 完整通过且已确认无残留，再考虑：

```bash
TINYDOCKER_ALLOW_PRIVILEGED_TESTS=1 PRIVILEGED_SUITE=full IMAGE=/path/to/trusted-rootfs.tar.xz bash tests/run_privileged.sh
```

### 需要回答的自测问题

1. 为什么测试不接受外部 `TINYDOCKER_TEST_RUN_ID`？
2. owned 标志如何限制 cleanup 范围？
3. observability suite 与 full suite 各覆盖什么？
4. 哪些内核行为仍可能因发行版、controller delegation 或 iptables backend 不同而失败？
5. cleanup 失败时为什么推荐回滚 VM 快照，而不是复制通用清理命令？

### 当天完成标准

有 VM：保存完整输出、run ID、资源名和残留检查结果，并能定位任一失败属于环境、代码还是测试假设。无 VM：完成逐行安全审查和一份预期资源清单，且没有在工作站运行特权脚本。

## 第 13 天：整理亮点、缺陷与改进方案（2～2.5 小时）

### 当天目标

形成有证据、有优先级、可实施的项目评价，不把“功能多”当作唯一亮点。

### 需要阅读的具体文件

- `README.md` 的实现范围、风险、限制
- `docs/STUDY_GUIDE.md` 的第 6～10 节
- `docker/container.c`
- `docker/network.c`
- `docker/status_info.c`
- `core/fs.c`
- `tests/test_core.c`
- `makefile`

### 需要理解的具体函数

- `cleanup_run_failure()`、`clean_container_runtime()`
- `write_container_info()`、`refresh_container_status_if_needed()`
- `lock_network_state()`、`write_network_records()`、`verify_interface_ownership()`
- `td_ensure_directory_beneath()`
- `docker_exec()`

### 需要手动运行的命令

```bash
rg -n '128|32|volume_cnt|volumes\[' cmdparser core docker tests
rg -n 'sprintf\(|malloc\(|calloc\(|realloc\(' cmdparser core docker util
rg -n 'flock|pid_start_time|O_NOFOLLOW|renameat|MS_PRIVATE|setns' core docker
git status --short
```

整理三个列表：5 个亮点、5 个缺陷、5 个改进项。每一项必须附文件和函数证据。

### 需要回答的自测问题

1. 哪个亮点最能体现系统编程能力，证据是什么？
2. volume 上限不一致应如何修复，怎样加回归测试？
3. 容器级锁、事务日志、pidfd、syscall adapter 应按什么顺序落地？
4. 哪些改进属于安全正确性，哪些只是功能扩展？
5. 如果只有一周重构时间，最先做哪三件事？

### 当天完成标准

产出一页项目评审：每个缺陷标明“事实”或“推断”，每个改进有优先级、受影响模块、测试策略和不做的代价。

## 第 14 天：模拟面试与项目讲解（2～2.5 小时）

### 当天目标

能在 1、3、5 分钟三个时间档准确介绍项目，并回答执行流、资源生命周期、安全与测试追问。

### 需要阅读的具体文件

- `docs/STUDY_GUIDE.md` 的面试问答与三版介绍
- `main.c`
- `docker/container.c` 的 `docker_run()`、`child_fn()`、`docker_exec()`、`docker_rm()`
- `docker/network.c` 的 network state 与 connect/cleanup 路径
- `docker/status_info.c` 的原子写与 lazy refresh
- `makefile` 与 `.github/workflows/ci.yml`

### 需要理解的具体函数

- `main()`
- `docker_run()`、`child_fn()`、`docker_exec()`、`docker_stop()`、`docker_rm()`
- `init_and_set_new_root()`
- `refresh_container_status_if_needed()`
- `connect_container()`、`verify_interface_ownership()`
- `td_ensure_directory_beneath()`

### 需要手动运行的命令

```bash
time make check
git status --short
git diff --check
rg -n 'int main|int docker_run|static int child_fn|int docker_exec|int docker_rm' main.c docker/container.c
```

完成三轮口述：

1. 60 秒，只讲定位、主链路、一个亮点、一个边界。
2. 3 分钟，加入启动同步、状态模型、cleanup。
3. 5 分钟，加入网络、安全、测试和改进优先级。

然后随机回答至少 12 个问题：namespace、cgroup、OverlayFS、`pivot_root`、pipe、PID reuse、metadata 原子写、network lock、veth ownership、`exec` 双 fork、Sanitizer 范围、生产差距。

### 需要回答的自测问题

1. 能否准确说出真实 `child_fn()` 顺序，而不照搬 README 简图？
2. 能否从一个失败点讲完整回滚，而不是只讲 happy path？
3. 能否主动承认 root-only、非 OCI、CI 不跑特权路径？
4. 能否用代码证据解释一个亮点和一个缺陷？
5. 遇到“为什么不用 runc/libcontainer”时，能否回到教学目标和取舍？

### 当天完成标准

三版介绍都在目标时长内；12 个问题至少 10 个能在 90 秒内给出结构化回答；回答中没有把推断说成事实，也没有把顶层旧 `test.c`、旧 `docker.md` 示例或过期图谱当成当前实现。

## 14 天结束后的验收清单

- 能闭卷画出 `run` 的父子双泳道和所有宿主资源。
- 能解释 RUNNING/STOPPED/EXITED 与 lazy refresh。
- 能解释 rootfs、cgroup、network、metadata 四条生命周期。
- 能从代码证明路径防护、原子写、网络锁和 ownership cleanup。
- 能说明非特权测试、Sanitizer、CI、特权测试的覆盖边界。
- 能列出至少 5 个真实限制，其中包含 volume 上限不一致和 runtime sanitizer 缺口。
- 能给出按优先级排序、带测试策略的改进路线。
- 能完成 1 分钟、3 分钟、5 分钟项目介绍。
