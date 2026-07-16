# tinydocker 学习指南

## 0. 阅读说明与证据边界

本文以当前仓库代码为最终依据，重点核对了 `main.c`、`cmdparser/`、`core/`、`docker/`、`util/`、`logger/`、`tests/`、`makefile`、`.github/workflows/ci.yml`、`README.md`、`docs/FULL_FUNCTION_TEST.md`，并参考了 `.codegraph/` 与 `graphify-out/`。

- 生成本文时当前提交为 `9c2718a4`。
- `graphify-out/GRAPH_REPORT.md` 对应提交 `7d6e5e44`，比当前代码旧；它适合用来发现 `main()`、`docker_run()`、网络、cgroup、状态文件等高连接节点，但不能替代源码核对。
- `.codegraph/codegraph.db` 当时索引了 20 个 C/H 文件、338 个节点、692 条边，未覆盖当前 `core/` 与 `tests/` 的完整内容，因此同样只作为导航线索。
- `docker.md` 第一段已明确说明它是早期学习笔记，包含旧实现和不安全实验命令；当前行为以代码、README 和特权测试指南为准。
- 本文未把顶层 `test.c` 当作正式测试：它是未纳入 `makefile` 的早期实验代码，当前甚至不能独立通过编译。
- 本文中未特别标注的行为都能从当前代码或构建/测试配置直接确认；需要从代码行为外推的判断统一标为“推断”。

## 1. 项目概览

tinydocker 是一个用 C 编写的教学型 Linux 容器运行时。它没有 daemon，而是由每次启动的 CLI 进程直接创建或管理宿主机资源。核心能力包括：

- 使用 `clone()` 创建 UTS、PID、mount、network、IPC namespace。
- 使用 cgroup v2 子目录限制 CPU 和内存，并通过 `cgroup.procs` 跟踪容器进程。
- 使用单个只读 lowerdir、每容器 upperdir/workdir 和 OverlayFS mountpoint 组成 rootfs。
- 使用 `pivot_root` 切换根文件系统，卸载旧 root，重新挂载 `/proc`。
- 使用 bridge、veth、路由、MASQUERADE 和 DNAT 实现基础 IPv4 网络。
- 使用文本 metadata 支持 `ps`、`top`、`inspect`、`stats`、`exec`、`stop`、`rm` 和 lazy 状态刷新。
- 使用 tar 快照实现简化版 `commit`。

项目定位必须准确：它用于学习 Linux namespace、cgroup v2、OverlayFS、进程同步、资源回滚和安全边界，不兼容 OCI，也不是 Docker、containerd 或 runc 的替代品。

### 1.1 支持的 CLI

`parse_docker_cmd()` 与 `main()` 当前真正接通的命令是：

```text
run
commit
ps
top
exec
stop
rm
inspect
stats
network create
network ls
network rm
```

`enum docker_command_type` 中还有 `DOCKER_NETWORK_CONNECT` 和 `DOCKER_NETWORK_DISCONNECT`，但解析器与 `main()` 没有对应分支，因此它们不是可用 CLI 功能。

### 1.2 运行边界

- 容器运行时只能在 Linux 构建和运行。
- 特权路径需要 root、cgroup v2、OverlayFS，以及 `ip`、`brctl`、`iptables`、`nsenter`、`ps`、`tar` 等宿主工具。
- macOS 可以执行 `make test`、`make static-check`、`make sanitize` 和 `make check`，但 `make` 会明确拒绝构建 Linux runtime。
- 默认运行目录是 `/home/xanarry/tinydocker_runtime`，默认 cgroup parent 是 `/sys/fs/cgroup/system.slice`；两者可在构建时覆盖。

## 2. 目录和模块职责

| 路径 | 当前职责 | 阅读重点 |
|---|---|---|
| `main.c` | CLI 总调度；把命令类型分派到容器或网络函数 | `main()` 的 switch、哪些命令会初始化运行目录/默认网络 |
| `cmdparser/` | 基于 glibc argp 解析参数；构造各命令参数结构；做名称、数值、卷、端口和 CIDR 的入口校验 | `parse_docker_cmd()`、`docker_run_parse_func()`、`docker_exec_parse_func()` |
| `core/safety.*` | 可移植的输入、路径、CIDR、PID start time、veth 名、archive entry 校验 | `td_validate_name()`、`td_join_rootfs_path()`、`td_parse_proc_stat_start_time()` |
| `core/status_codec.*` | 容器 metadata 的严格解析与序列化 | 必填字段位图、重复字段拒绝、文件名与内部 name 一致性 |
| `core/network_state.*` | 网络状态记录的严格解析与序列化 | CIDR 必须是网络地址、IP 范围、重复记录与容量检查 |
| `core/cgroup_parse.*` | `cpu.stat`、字节值和 `cgroup.procs` 的纯解析逻辑 | `td_parse_cgroup_stat()`、`td_format_bytes()`、`td_parse_cgroup_pid_list()` |
| `core/fs.*` | 幂等递归删除；使用 dirfd/openat 保证目录创建不穿越 rootfs | `td_remove_tree()`、`td_ensure_directory_beneath()` |
| `core/process.*` | 不经过 shell 地 fork/exec 外部命令，并可捕获 stdout | `td_run_command()`、`td_capture_command()` |
| `docker/container.c` | 容器生命周期主模块和 observability 命令 | `docker_run()`、`child_fn()`、`docker_exec()`、`docker_stop()`、`docker_rm()` |
| `docker/workspace.c` | image cache、OverlayFS workspace、`pivot_root`、`/proc` | `init_container_workerspace()`、`init_and_set_new_root()` |
| `docker/volumes.c` | bind volume 挂载、只读 remount、逆序卸载和局部回滚 | `mount_volumes()`、`umount_volumes()` |
| `docker/cgroup.c` | cgroup v2 目录、CPU/内存限制、进程加入与 PID 列表 | `cgroup_prepare()`、`cgroup_apply()`、`cgroup_cleanup()` 及 legacy helper |
| `docker/network.c` | 网络状态锁与原子写、bridge/veth、IP 分配、路由、NAT/DNAT、ownership cleanup | `create_network()`、`connect_container()`、`unset_container_port_map()` |
| `docker/status_info.c` | metadata 对象生成、安全原子写、读取、状态刷新、列表和删除 | `write_container_info()`、`refresh_container_status_if_needed()` |
| `util/` | SHA-256、目录帮助函数、tar 解包/打包、时间格式化 | `extract_tar()` 先列举再校验 entry，随后无 shell 解包 |
| `logger/` | 轻量日志库，支持可选回调与可选锁函数 | 当前项目没有安装日志锁回调，通常依赖单进程/进程隔离 |
| `tests/test_core.c` | 非特权纯逻辑与文件系统安全测试 | 10 组测试函数；是 `make test`/`make sanitize` 的主体 |
| `tests/run_privileged.sh` | 特权测试总闸门 | 必须显式 opt-in、Linux、root、只允许一个 suite |
| `tests/test_observability.sh` | `inspect`、`stats`、lazy EXITED、`ps -a` | 短生命周期 detached 容器 |
| `tests/test_full_function.sh` | env、volume、cgroup、top、exec、network、port、commit、stop/rm | 端到端功能面与 owned-resource cleanup |
| `demo.sh` | 安全门控的最小演示 | build → run → inspect → stats → stop → rm → 残留检查 |
| `makefile` | Linux runtime、非特权测试、静态检查、Sanitizer、聚合检查 | `CORE_SOURCES` 与 `RUNTIME_SOURCES` 的边界 |
| `.github/workflows/ci.yml` | Ubuntu 24.04 上的 build/test、strict static-check、ASan/UBSan | 默认不执行特权集成测试 |

顶层 `a.png`、`b.png`、`c.png`、`network` 和 `test.c` 属于早期学习/实验材料。顶层 `network` 不是当前 runtime 使用的网络状态文件；当前代码读写的是 `TINYDOCKER_RUNTIME_DIR/networks`。

## 3. 从 `main()` 开始的执行流程

### 3.1 总调度

`main(argc, argv)` 的执行顺序很短：

1. `parse_docker_cmd(argc, argv)` 校验并构造 `struct docker_cmd`。
2. 按 `cmd_type` 进入 switch。
3. 部分命令调用 `print_docker_cmds()` 打印解析结果。
4. 调用具体实现。
5. 负数结果统一转换为 `EXIT_FAILURE`；非负结果原样返回。

初始化行为并不对称：

- `run` 先调用 `init_docker_env()`，它会创建 runtime 目录、确保默认 bridge，并确保 POSTROUTING MASQUERADE 规则存在。
- `network create` 只调用 `init_runtime_dirs()`。
- `ps`、`top`、`exec`、`stop`、`rm`、`inspect`、`stats`、`network ls/rm` 不会隐式创建 runtime 或默认网络。

这种设计避免只读命令为了“查询”而修改宿主状态。

### 3.2 `run` 的父子执行路径

当前真实顺序如下：

```text
main
  -> parse_docker_cmd
  -> init_docker_env
       -> init_runtime_dirs
       -> create_default_bridge
       -> iptables -C/-A POSTROUTING MASQUERADE
  -> docker_run
       -> container_exists
       -> init_container_workerspace
            -> image 目录 realpath，或 tar SHA-256 cache + 解包
            -> 创建 upperdir/workdir/mountpoint
            -> mount overlay
       -> mount_volumes
       -> cgroup_prepare
       -> pipe
       -> clone(child_fn, NEWUTS|NEWPID|NEWNS|NEWNET|NEWIPC|SIGCHLD)
       -> cgroup_apply
       -> connect_container
       -> set_container_port_map
       -> create_container_info
       -> write_container_info
       -> pipe 写入单字节 '1'
       -> detached: 父进程返回
          foreground: 安装信号转发、waitpid、写 EXITED、返回子进程状态
```

子进程 `child_fn()` 的真实顺序是：

```text
detached 时打开日志文件并 dup2 stdout/stderr
  -> load_process_env
  -> init_and_set_new_root
       -> realpath(new_root)
       -> mount propagation 设为 MS_PRIVATE
       -> bind mount new_root 自身
       -> 安全创建 old_root
       -> pivot_root
       -> chdir("/")
       -> MNT_DETACH 卸载旧 root
       -> 创建并挂载新的 /proc
  -> 从 pipe 读取恰好一个字节
  -> 只有字节为 '1' 才 execve 用户命令
```

这是理解项目时最容易说错的一点：当前代码和 README 都是先做 `pivot_root`，再等待授权。屏障可靠保证的是“父进程完成 cgroup、网络、端口和 metadata 前，用户命令不能执行”，不是“子进程在授权前完全不做初始化”。EOF、短读、错误字节都会使子进程以 125 退出。

### 3.3 前台与后台生命周期

- 后台模式：父进程完成 metadata 与授权后立即返回，子进程继续运行；子进程 stdout/stderr 被重定向到 `runtime/logs/<name>`。
- 前台模式：父进程把 SIGINT、SIGTERM、SIGHUP 转发给 clone 子进程，循环处理 `waitpid()` 的 EINTR，恢复旧 signal handler，更新 metadata 为 EXITED，并返回真实退出码或 `128 + signal`。
- 无 daemon：后台容器退出时没有常驻进程立即更新状态；之后的 `ps`、`inspect`、`stats`、`exec`、`stop`、`rm` 通过 lazy refresh 修正状态。

### 3.4 `exec` 路径

`docker_exec()` 不直接把宿主 CLI 进程移入容器：

1. 读取并刷新 metadata，只接受 RUNNING。
2. 读取容器 cgroup 的 PID 列表，并确认 metadata 中的 init PID 仍属于该 cgroup。
3. 从 `/proc/<pid>/environ` 读取环境，追加用户指定环境。
4. fork namespace worker。
5. worker 先把自己加入目标 cgroup，再打开目标进程的 `ipc`、`uts`、`net`、`mnt`、`pid` namespace fd。
6. 按顺序 `setns()`，然后 `chdir("/")`。
7. 再 fork command child；这个额外 fork 让命令真正进入目标 PID namespace 的进程视图。
8. command child `execve()`；worker 在非 detached 模式等待并传递退出状态；宿主父进程等待 worker。

### 3.5 `stop`、`rm` 与状态查询

- `stop`：读取并 lazy refresh metadata；对 cgroup 内全部 PID 发送 SIGTERM；按 100ms 轮询最多 `timeout` 秒；仍有进程则 SIGKILL，再最多等 2 秒；只有 cgroup PID 列表为空才写 STOPPED。
- `rm`：若全部资源已缺失则幂等成功；否则 metadata 缺失/损坏时拒绝猜测清理；RUNNING 时拒绝删除；非运行状态才进入 `clean_container_runtime()`。
- `ps`：最多读取 128 份 metadata，逐份 lazy refresh；默认仅显示 RUNNING，`-a` 显示全部。
- `inspect`：展示 metadata、cgroup/rootfs 路径与可用性、卷；端口映射固定显示 unavailable。
- `stats`：读取一次性 cgroup v2 快照；缺失指标显示 `N/A`。
- `top`：从 cgroup 读取 PID 列表，再无 shell 执行 `ps -f -p pid1,pid2,...`。
- `commit`：对容器 mountpoint 执行 `tar -czf ... -C mountpoint .`；它是文件快照，不是 OCI image。

## 4. 核心数据结构及生命周期

### 4.1 命令参数对象

`struct docker_cmd` 保存命令枚举和一个 `void *arguments`。具体参数结构包括：

- `docker_run_arguments`：镜像、名称、交互/后台、CPU/内存、环境、argv、卷、端口映射，以及运行时填入的 mountpoint。
- `docker_exec_arguments`：目标容器、环境、argv、detach/interactive/tty 标志。
- `docker_stop_arguments`、`docker_rm_arguments`：容器名数组。
- network create/rm、inspect、stats 等小型参数对象。

这些对象由短生命周期 CLI 进程在解析阶段动态分配，项目没有统一析构函数，通常依赖进程退出回收。运行时不把指针写入 metadata。

### 4.2 `struct container_info`

这是容器持久状态的内存模型：

| 字段 | 作用 |
|---|---|
| `pid` | 宿主机看到的容器 init PID |
| `pid_start_time` | `/proc/<pid>/stat` 第 22 字段，用于识别 PID reuse |
| `detach` | 是否后台启动 |
| `container_id` | 当前实现为创建时间戳字符串 |
| `image`、`command`、`created`、`status`、`name`、`ip_addr` | 展示和生命周期判断所需字段 |
| `volume_cnt`、`volumes[32][512]` | cleanup 所需的卷配置快照 |

状态只有 RUNNING、STOPPED、EXITED：

```text
run 成功 -> RUNNING
前台命令退出 -> EXITED
后台命令消失或 PID start time 不匹配 -> lazy refresh -> EXITED
stop 完成且 cgroup 无进程 -> STOPPED
rm 成功 -> metadata 删除，容器不再存在
```

metadata 文件采用严格文本格式。解析器拒绝 NUL、未知字段、重复字段、缺失字段、非法数值、非法状态、过多卷、额外卷记录，以及内部 name 与文件名不一致。`pid_start_time` 为兼容旧 metadata 不是必填字段；缺失时会警告无法排除 PID reuse。

### 4.3 `struct td_network_record`

网络记录包含：

- 最长 15 字节网络名。
- 当前唯一支持的 driver：`bridge`。
- IPv4 CIDR，且地址部分必须是网络地址，prefix 必须给 gateway 和容器留出空间。
- 最多 128 个已分配 IPv4 地址，以 host-order `uint32_t` 保存。

整个网络状态文件最多 128 条记录。写操作在 `networks.lock` 上使用 `flock(LOCK_EX)`，然后临时文件写入、`fflush()`、`fsync()`、原子 `rename()`，最后尽力同步 runtime 目录。

### 4.4 宿主资源生命周期

单个容器可能拥有：

1. 镜像 cache：`runtime/images/<sha256>`，跨容器复用，`rm` 不删除。
2. workspace：`runtime/containers/<name>/{upperdir,workdir,mountpoint}`。
3. bind volume mount：位于 mountpoint 内的目标路径。
4. cgroup：`<cgroup-parent>/tinydocker-<name>`。
5. network namespace 内的 `lo`、`eth0`、IP 与默认路由。
6. 宿主 veth 端、默认 bridge 成员关系。
7. iptables OUTPUT/PREROUTING DNAT 规则，以及全局 POSTROUTING MASQUERADE 规则。
8. metadata：`runtime/container_info/<name>`。
9. detached 日志：`runtime/logs/<name>`；当前 `rm` 不删除日志文件。

## 5. 关键调用链和数据流

### 5.1 CLI 数据流

```text
argv
 -> argp / 手工参数解析
 -> core/safety 的严格校验
 -> docker_*_arguments
 -> main switch
 -> 生命周期模块
 -> 系统调用或无 shell 外部命令
```

名称只允许 `[A-Za-z0-9._-]`，并拒绝 `.`、`..`。CPU、内存、timeout 和端口使用完整字符串数值解析，拒绝尾随垃圾。卷要求绝对 host path 和绝对 container path，并拒绝 `.`/`..` 路径组件。

### 5.2 rootfs 数据流

```text
image 参数
 -> 目录：realpath 后直接作为 lowerdir
 -> tar：SHA-256 -> runtime/images/<hash>
             -> tar -tf 列举 entry
             -> 拒绝绝对路径、换行、. / .. traversal
             -> 临时目录解包 -> rename 发布 cache
 -> 每容器 upperdir + workdir + mountpoint
 -> OverlayFS mount
 -> volume bind mounts
 -> clone 子进程中 pivot_root
 -> execve 用户命令
```

### 5.3 状态数据流

```text
docker_run 的 args + child PID + IP + time
 -> create_container_info
 -> 读取 PID start time
 -> td_write_container_info
 -> 临时文件 + fsync + renameat
 -> ps/inspect/stats/exec/stop/rm 读取
 -> td_parse_container_info 严格校验
 -> lazy refresh 比较 /proc start time
 -> 必要时原子重写 EXITED
```

### 5.4 网络数据流

```text
default network record
 -> 分配第一个空闲 IP（网络地址+2 起）
 -> 根据容器名生成 15 字节稳定 veth 名
 -> 宿主创建 veth pair
 -> host veth 写 interface alias，加入 bridge，设为 up
 -> peer 移入容器 netns，重命名 eth0
 -> 配 lo、容器 IP、eth0、默认路由
 -> 可选 OUTPUT + PREROUTING DNAT
 -> IP 写入 container metadata
```

容器删除时，通过 metadata IP 删除指向该 IP 的 DNAT 规则，通过 container name 重建 veth 名和期望 alias，核对 ownership 后删除，再释放网络状态中的 IP。

## 6. 网络、并发、文件、资源管理与安全设计

### 6.1 网络设计

- 默认 bridge 为 `tinydocker-0`，CIDR 为 `172.11.11.0/24`，gateway 为 `172.11.11.1`。
- 每个容器获得稳定哈希生成的宿主 veth 名，规避 Linux 15 字节接口名限制和相同前缀碰撞。
- bridge/veth 创建后写 `tinydocker-network:<name>` 或 `tinydocker-container:<name>` alias。
- 删除前核对 interface alias、bridge 类型和 ifindex；身份变化或 ownership 不匹配时拒绝删除。
- 所有外部命令都以 argv 数组调用 `execvp()`，不拼接 shell 命令。
- 端口映射仅支持 TCP，分别写 OUTPUT 与 PREROUTING DNAT；外网回包依赖默认网段的 POSTROUTING MASQUERADE。

### 6.2 并发和同步

- 启动同步：匿名 pipe 的单字节授权协议，防止用户命令在父进程配置完成前运行。
- 前台信号：进程级全局 `foreground_child_pid` 为 `sig_atomic_t`，handler 只调用 `kill()`。
- 网络状态：所有修改 IP 分配记录的路径都使用独占 `flock`，避免同一状态文件的 writer-writer 丢更新。
- metadata：单次写入是原子的，但跨 `read -> modify -> write` 没有容器级锁。
- CLI 无 daemon，各次命令是独立进程；不同命令并发时没有全局生命周期事务。
- logger 提供可选 lock callback，但当前主程序没有配置；fork/clone 后各进程持有各自地址空间，日志文件与终端输出仍可能交错。

### 6.3 文件与路径安全

- runtime 名称先校验，再拼接路径。
- metadata、网络状态、锁文件和 detached 日志使用 `O_NOFOLLOW`；metadata/网络状态还限制为普通文件和最大 1 MiB。
- `td_ensure_directory_beneath()` 从 rootfs dirfd 开始逐级 `mkdirat/openat(O_NOFOLLOW)`，拒绝通过目标目录中的 symlink 逃逸。
- 递归删除使用 `nftw(..., FTW_PHYS)`，不跟随 symlink；拒绝空路径和 `/`；缺失路径视为成功。
- volume host 先 `realpath()` 并要求为现有目录；container target 必须是安全绝对路径。
- archive 解包前先 `tar -tf` 检查 entry 名称，解包时使用 `--no-same-owner --no-same-permissions`。

### 6.4 资源管理原则

- 创建路径通常采用“创建一步、失败回滚已知 owned 资源”的方式。
- volume 按正序挂载、逆序卸载；中途失败只回滚已经挂载的部分。
- cgroup 只在父层级已存在且系统为 v2 时创建；不会擅自创建未知宿主 cgroup 层级。
- 网络删除以 ownership 标记为前提，宁可保留残留也不删除同名未知资源。
- mount 清理失败时拒绝递归删除 workspace，避免跨仍然活动的挂载点删除内容。
- cleanup 全部成功后才删除 metadata；失败则保留 metadata 供检查和重试。

### 6.5 安全边界

已实现的是输入与资源所有权层面的防护，不是生产容器安全边界。当前没有：

- user namespace 或 rootless。
- capability drop。
- seccomp。
- AppArmor/SELinux 等 LSM 策略。
- OCI bundle/image/runtime 验证。
- 签名镜像、可信供应链或完整 archive 恶意链接模型。
- pidfd 驱动的进程身份绑定。

因此容器内 root 仍然是宿主高风险权限主体，只应在可丢弃 Linux VM 中运行特权功能。

## 7. 错误处理与清理路径

### 7.1 `docker_run()` 失败清理

任一步失败都跳到 `fail_cleanup_run`，再调用 `cleanup_run_failure()`：

1. 若 clone 子进程已存在，SIGKILL 并可靠 waitpid。
2. 关闭 pipe 两端。
3. 若已分配 IP，删除 DNAT、断开 veth、释放 IP。
4. 逆序卸载 volumes，再卸载 OverlayFS mountpoint。
5. 只有 mount cleanup 成功时才递归删除 workspace。
6. 删除 cgroup。
7. mount cleanup 成功时删除部分 metadata；否则保留 metadata 并告警。

这条路径既处理父进程失败，也处理子进程已经先行 `pivot_root` 后父进程失败的情况：父进程先杀子进程，随后再尝试卸载宿主 mountpoint。

### 7.2 正常 `rm` 清理

`clean_container_runtime()` 的顺序是：

```text
从 metadata 重建 volume 配置
 -> 卸载 volume
 -> 卸载 rootfs
 -> 删除 DNAT
 -> 删除 owned veth
 -> 释放 IP
 -> 删除 workspace（仅 mount 已确认清理）
 -> rmdir cgroup
 -> 删除 metadata（仅前面全部成功）
```

缺失目录、cgroup 或 metadata 删除被设计为幂等；但 metadata 本身缺失或损坏且其他资源仍存在时，`rm` 会拒绝凭名称猜测清理。

### 7.3 网络创建回滚

- bridge 创建后若 alias 设置失败，只在 ifindex 仍匹配时删除刚创建的 bridge。
- metadata 写入失败时删除 owned bridge。
- bridge IP/up 配置失败时先删 owned bridge；删除成功才删 metadata，否则保留 metadata 供安全重试。
- veth 配置中途失败时删除带正确 alias 的宿主端，并释放 IP。

### 7.4 可观察的退出码

- `main()` 把负数转换为 `EXIT_FAILURE`。
- `run` 前台与 `exec` 尽量传播真实命令退出码。
- 被信号终止使用 `128 + signal`。
- 子进程初始化/授权错误主要使用 125/126，`execve` 失败使用 127。
- 参数解析大量使用 `exit(-1)`；在 Unix 上最终表现为 255。

## 8. 测试、Sanitizer、CI 和运行方式

### 8.1 构建目标

```bash
make clean
make
```

Linux runtime 会编译 `core/`、`logger/`、`util/`、`cmdparser/`、`docker/` 与 `main.c`，链接 `libcrypto`。默认使用 C11 和较严格的 warning 集；正式 `static-check` 再加 `-Werror`。

构建路径可覆盖：

```bash
make clean
make RUNTIME_DIR=/var/lib/tinydocker \
     CGROUP_PARENT=/sys/fs/cgroup/tinydocker.slice
```

自定义 cgroup parent 必须由管理员预先创建和正确 delegation。

### 8.2 非特权测试

```bash
make test
make static-check
make sanitize
make check
```

`make check` 顺序执行 test、static-check、sanitize。生成本文时在当前 macOS 工作区实际运行 `make check`，三部分均通过：

- `build/test_core`：全部非特权核心测试通过。
- macOS static-check：`tests/test_core.c` 加六个 `core/*.c` 以 `-Werror -fsyntax-only` 通过。
- `build/test_core_sanitize`：ASan + UBSan 版本通过。

这次本地结果不证明 Linux runtime 或特权路径可用，因为 macOS 分支不会编译 `docker/` runtime。

`tests/test_core.c` 当前覆盖：

1. 名称与保留路径组件。
2. 严格整数、端口、volume 解析。
3. runtime/rootfs 路径、veth 名和 archive entry。
4. network state codec、CIDR 边界、容量和去重。
5. cgroup stat/byte/PID list 解析。
6. `/proc/<pid>/stat` start time。
7. container metadata 严格解析与 round-trip。
8. 递归删除幂等性。
9. symlink 下的 rootfs 目录创建逃逸防护。
10. 无 shell 命令执行与 stdout 捕获。

### 8.3 Sanitizer 范围

`make sanitize` 使用：

```text
-fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g
```

它只编译 `tests/test_core.c` 与 `CORE_SOURCES`。因此它能覆盖纯逻辑和非特权文件操作，但不覆盖 `docker/container.c`、namespace、mount、network、cgroup、logger、cmdparser 和 util 的完整 runtime 路径。

CI 的 sanitizer job 另外设置：

```text
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
```

### 8.4 GitHub Actions

`.github/workflows/ci.yml` 在 push 和 pull request 上配置三个 Ubuntu 24.04 job：

| Job | 做什么 |
|---|---|
| `build-and-test` | 安装 build-essential/libssl-dev，`make clean && make`，`make test` |
| `static-check` | 对全部 runtime + core source 做严格 `-Werror -fsyntax-only` |
| `sanitizers` | 对非特权 core tests 运行 ASan/UBSan |

workflow 权限只有 `contents: read`，每个 job 超时 10 分钟。默认 CI 不运行 root、mount、namespace、bridge/veth 或 iptables 集成测试。本文未查询远端 Actions 的当前运行状态，只能确认 workflow 配置。

### 8.5 特权集成测试

仅在可丢弃 Linux VM 的已确认 root shell 中运行：

```bash
TINYDOCKER_ALLOW_PRIVILEGED_TESTS=1 \
PRIVILEGED_SUITE=observability \
IMAGE=/path/to/trusted-rootfs.tar.xz \
bash tests/run_privileged.sh

TINYDOCKER_ALLOW_PRIVILEGED_TESTS=1 \
PRIVILEGED_SUITE=full \
IMAGE=/path/to/trusted-rootfs.tar.xz \
bash tests/run_privileged.sh
```

入口脚本拒绝默认执行、拒绝非 Linux、拒绝非 root、拒绝外部指定测试 run ID，并要求 suite 二选一。测试用 `mktemp` 后缀生成唯一资源名，只清理由当前流程标记为 owned 的资源。

### 8.6 最小演示

```bash
TINYDOCKER_ALLOW_PRIVILEGED_DEMO=1 bash demo.sh
```

演示会 build、run、inspect、stats、stop、rm，并检查 metadata/cgroup/workspace 是否残留。它不会在 cleanup 失败时用宽泛 `rm -rf`、`umount` 或 cgroup 删除兜底。

## 9. 当前限制与风险

### 9.1 已明确的功能限制

- 仅 Linux、仅 cgroup v2、root-only。
- 不支持 OCI runtime/image spec、registry、layer manifest、pull/push、tag、multi-arch。
- 不支持 daemon、事件流、restart policy、health check、checkpoint/restore。
- 没有 user namespace、rootless、seccomp、capability/LSM 收敛。
- 资源限制只有 CPU quota、memory max，以及内部可调用但 CLI 未暴露的 cpuset；没有 `pids.max` CLI、CPU weight、I/O、OOM policy。
- 网络只有基础 IPv4 bridge；没有 DNS、IPv6、CNI、策略隔离。
- port mapping 没有写入 container metadata，因此 inspect 无法列出。
- `commit` 只是 mountpoint tar 快照，不保存运行配置和网络配置。
- 日志没有轮转；`rm` 当前不清理 detached 日志。

### 9.2 代码可直接确认的风险

1. `docker_run` CLI 最多接受 128 个 volume，但 `container_info` 只保存 32 个；`create_container_info()` 会截断。正常 `rm` 只能按 metadata 重建并卸载前 32 个 volume，容量不一致可能导致 mount cleanup 失败和残留。
2. `docker_commit()` 使用固定 512 字节数组和 `sprintf()` 构造 mountpoint；构建时允许覆盖的 `RUNTIME_DIR` 若过长，存在越界风险。其他多数路径已改用有界 `snprintf()`。
3. `make_path()` 内部只有 256 字节临时缓冲区，长自定义 runtime 路径会失败，即使调用者使用 `PATH_MAX` 缓冲区。
4. 参数解析的多处 `malloc()` 没有检查返回值，并大量直接 `exit(-1)`；短生命周期 CLI 降低了泄漏影响，但错误处理不统一。
5. metadata 里的 command 是把 argv 用空格重新拼接的展示字符串，不能无损恢复原始参数边界。
6. 用户环境变量被追加到继承环境末尾，没有显式替换同名键；最终出现重复键时目标程序如何选择取决于其环境处理方式。
7. `top` 只依赖 cgroup PID 列表，不先核验 metadata 状态；状态或 cgroup 异常时错误表现与其他 observability 命令不完全一致。
8. 顶层 `test.c` 是未构建的旧实验文件，包含 `system()`、不安全字符串拼接和语法/标识符错误；学习当前实现时不应把它当正式代码路径。
9. 顶层 `network` 是旧格式样例，其 CIDR 地址为 `172.11.11.1/24`，会被当前 codec 的“必须是网络地址”规则拒绝；它也不是 runtime 使用的状态文件。

### 9.3 需要标为“推断”的风险

- 推断：不同 CLI 同时对同一容器执行 `stop`、`rm`、`inspect` 或状态刷新时，metadata 的 `read -> modify -> rename` 缺少容器级锁，可能出现逻辑竞争或较新的状态被较旧快照覆盖。
- 推断：网络状态文件虽然有 writer lock，但 bridge/veth/iptables 与 metadata 更新不是同一事务；进程崩溃、断电或外部管理员修改宿主网络后，文件状态与内核状态可能分叉。
- 推断：image cache 的临时目录名只含 PID，且并发发布主要依赖 `rename()` 错误分支；高并发解包同一镜像时可能出现重复工作或平台相关的发布失败。
- 推断：PID start time 显著降低 PID reuse 误判，但“校验身份”到后续 `kill()`/`setns()` 之间仍有 TOCTOU 窗口；pidfd 能提供更强绑定。
- 推断：archive entry 名称检查能挡住明显路径穿越，但 tar 内 symlink/hardlink 的组合语义没有完整验证，只适合可信 rootfs。
- 推断：Sanitizer 只覆盖 core，runtime 中与 fork/clone、namespace、mount、network 和错误回滚相关的内存/UB 问题仍可能未被自动发现。

## 10. 面试高频问题及参考回答

### Q1：容器的本质是什么？tinydocker 做了哪些隔离？

容器首先是宿主机进程。tinydocker 用 `clone()` 创建 UTS、PID、mount、network、IPC namespace，让进程看到不同的主机名、PID、挂载、网络和 IPC 视图；再用 cgroup v2限制资源，用 OverlayFS + `pivot_root` 切换 rootfs。它没有 user namespace，因此容器内 root 仍是高风险宿主权限主体。

### Q2：为什么不用 `fork()`，而用 `clone()`？

`fork()` 默认继承现有 namespace；`clone()` 可以通过 `CLONE_NEWUTS|CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWNET|CLONE_NEWIPC` 在创建子进程时建立新的隔离视图。这里还带 `SIGCHLD`，让父进程可以正常 `waitpid()`。

### Q3：启动 pipe 解决了什么竞争？

父进程只有拿到 child PID 后才能把它加入 cgroup、配置 veth/路由/iptables 并写 metadata。如果子进程立刻执行用户命令，就会有未受限运行窗口。子进程只在读到单字节 `'1'` 后 `execve()`；EOF、短读、其他字节都拒绝启动。要注意当前代码在等待前已经完成 rootfs 切换，屏障保护的是用户命令执行时点。

### Q4：OverlayFS 的四个目录分别是什么？

镜像目录是共享只读 lowerdir；每个容器有独立 upperdir 保存写入；workdir 是 OverlayFS 内部工作目录；mountpoint 是合并视图，也是后续 `pivot_root` 的 new root。容器删除时保留镜像 cache，删除每容器 workspace。

### Q5：`pivot_root` 为什么前面要设置 mount propagation？

代码先把 `/` 递归设为 `MS_PRIVATE`，避免容器 mount 事件传播回初始 mount namespace，也满足 `pivot_root` 对挂载关系的要求。随后把 new root bind 到自身使其成为独立挂载点，创建 `old_root`，pivot 后切到 `/`、卸载旧 root，再挂新的 `/proc`。

### Q6：cgroup v2 如何设置 CPU 和内存？

每个容器创建 `<parent>/tinydocker-<name>`。内存把字节数写入 `memory.max`；CPU 把 `<quota> 100000` 写入 `cpu.max`；PID 写入 `cgroup.procs`。项目要求 parent 已存在，不自动开启 controller 或修复 delegation。

### Q7：无 daemon 如何知道后台容器已经退出？

metadata 保存宿主 PID 与 `/proc/<pid>/stat` 的 start time。读取状态时，如果 PID 不存在，或同 PID 的 start time 已变化，就把 RUNNING 原子更新为 EXITED。这是 lazy refresh，不是实时事件或强一致状态机。

### Q8：为什么只保存 PID 不够？

PID 会复用。保存 start time 能区分“仍是原进程”和“PID 数字相同但已换成另一个进程”。旧 metadata 没有 start time 时仍可读，但只能警告。更强方案是 pidfd。

### Q9：metadata 如何避免半写和 symlink 攻击？

状态目录用 `O_DIRECTORY|O_NOFOLLOW` 打开；临时文件用 `openat(O_EXCL|O_NOFOLLOW)` 创建，写完 `fflush + fsync`，再同目录 `renameat` 原子发布。读取也用 `openat(O_NOFOLLOW)`，并验证普通文件、大小、字段完整性和内部 name。

### Q10：网络状态如何处理并发？

修改时先对独立 lock 文件 `flock(LOCK_EX)`，锁内重新加载全部记录，修改后写临时文件、fsync、rename。这样避免两个 IP 分配者同时基于同一旧快照写回。但内核网络资源和状态文件仍不是单事务。

### Q11：为什么删除 bridge/veth 前要检查 alias 和 ifindex？

名字可能被其他管理员或进程复用。项目创建接口时写 ownership alias，删除前确认 alias、类型和当前 ifindex；删除动作前再次核对身份。匹配失败宁可保留，也不误删同名宿主资源。

### Q12：`exec` 为什么要 fork 两次？

第一个 namespace worker 负责加入 cgroup并 `setns()`；加入 PID namespace 只影响之后创建的子进程，所以 worker 再 fork command child，后者才处于目标 PID namespace 的进程视图中并执行命令。宿主 CLI 本身不会被永久移入目标 namespace/cgroup。

### Q13：volume target 如何防止路径穿越和 symlink 逃逸？

解析阶段要求绝对 container path，拒绝 `.` 和 `..`。创建目标目录时，从 rootfs dirfd 开始逐级 `mkdirat`，再用 `openat(O_DIRECTORY|O_NOFOLLOW)` 进入下一层；如果某层是 symlink，打开失败，不会跟随到 rootfs 外。

### Q14：为什么 cleanup 失败时保留 metadata？

metadata 是后续判断资源 ownership 和重试清理的依据。若 mount 或网络清理不完整却先删 metadata，下一次就只能猜测哪些宿主资源属于容器。项目选择保守失败：保留证据，拒绝危险递归删除。

### Q15：`make sanitize` 能证明 runtime 安全无内存错误吗？

不能。它只编译 `tests/test_core.c` 和 `CORE_SOURCES`，覆盖纯解析、安全路径和非特权 helper。真正的 `docker/` runtime、fork/clone、mount、network 和特权失败路径没有进入这个 sanitizer binary。

### Q16：项目最值得讲的工程亮点是什么？

不是“实现了很多 Docker 命令”，而是把安全边界做成了可测试的小模块：严格 codec、无 shell 命令执行、dirfd/openat 路径防护、PID start time、原子 metadata、网络 writer lock、interface ownership 检查，以及 mount 失败时拒绝危险删除。

### Q17：当前最优先改进什么？

先修复 volume 上限不一致并统一资源 manifest；再引入每容器生命周期锁和事务日志；随后用 pidfd 强化进程身份；把 runtime 拆出可注入的 syscall/command adapter，扩大非特权故障注入和 sanitizer 覆盖；最后才考虑 OCI、rootless、seccomp 等更大能力。

### Q18：与 runc 的差异是什么？

runc 接受 OCI bundle，按规范管理 namespace、mount、capability、seccomp、hooks 和生命周期；tinydocker 使用自定义 CLI/metadata、单 lowerdir 和宿主工具命令，缺少 OCI、安全收敛与生产兼容性。它适合教学和系统编程展示，不是兼容替代品。

## 11. 项目介绍话术

### 11.1 1 分钟版本

tinydocker 是我用 C 实现的教学型 Linux 容器运行时。它从一个 CLI 直接创建 UTS、PID、mount、network、IPC namespace，用 cgroup v2 做 CPU 和内存限制，用 OverlayFS 和 `pivot_root` 组织容器 rootfs，再通过 bridge、veth 和 iptables 提供基础 IPv4 网络。项目没有 daemon，所以我设计了原子 metadata 和基于 PID start time 的 lazy 状态刷新。工程上我重点处理了启动 pipe 同步、失败回滚、无 shell 命令执行、路径穿越防护和网络资源 ownership 校验。它不是 OCI 或生产级安全边界，主要用于展示 Linux 系统编程和资源生命周期设计。

### 11.2 3 分钟版本

tinydocker 的入口是一个 C CLI。`run` 先准备 runtime 目录和默认 bridge，然后把镜像目录或 tar cache 作为 OverlayFS lowerdir，为容器创建独立 upperdir、workdir 和 mountpoint，再挂载 volumes。之后创建 cgroup v2、写 CPU/内存限制，并通过 `clone()` 创建五类 namespace。

父子进程之间有一条匿名 pipe。子进程准备环境、切换 rootfs 并等待；父进程把 child 加入 cgroup，创建 veth、配置 IP/路由和 DNAT，写入 metadata，最后发送单字节授权，用户命令才会 `execve()`。前台模式还转发信号并传播真实退出码，后台模式靠后续 `inspect` 或 `ps` 比较 PID start time 做 lazy EXITED 刷新。

我把容易出安全问题的逻辑拆到 `core/`：名称/数值/CIDR/路径校验、严格 metadata 和 network codec、cgroup parser、无 shell process runner、dirfd/openat 防 symlink 逃逸。状态文件和网络文件使用临时文件、fsync 与原子 rename；网络 writer 使用 flock；bridge/veth 删除前检查 alias、类型和 ifindex。cleanup 的原则是只删除能证明 owned 的资源，mount 清理失败就保留 workspace 和 metadata。

当前限制也很明确：root-only、无 user namespace/seccomp/capability drop、不兼容 OCI、没有 daemon 和完整并发事务，特权路径只能在可丢弃 VM 手工测。默认 CI 只做 Linux build、非特权测试、严格编译和 core ASan/UBSan。

### 11.3 5 分钟版本

这个项目的目标不是复制 Docker，而是把一个容器从“宿主进程”变成“具备隔离文件系统、资源限制和网络的进程”的完整链路做成可阅读代码。

入口 `main()` 只负责解析和分派。真正的核心是 `docker_run()`：它先检查同名 metadata、workspace 和 cgroup，随后把镜像目录直接作为 lowerdir，或对 tar 做 SHA-256 后解压到共享 cache。每个容器创建独立 upperdir/workdir/mountpoint 并挂 OverlayFS，volume target 则通过 dirfd/openat 逐级创建，防止 symlink 把挂载点引到 rootfs 外。

接着项目创建 cgroup v2 子目录，写 `memory.max` 与 `cpu.max`，再 `clone()` 出 UTS、PID、mount、network 和 IPC namespace。父子进程通过 pipe 同步：当前真实代码中，子进程先把 mount propagation 设为 private，bind new root、`pivot_root`、卸载 old root 并挂 `/proc`，然后阻塞；父进程把 PID 写入 `cgroup.procs`，创建唯一命名的 veth pair，把 peer 移入容器 netns，配置 `eth0`、loopback、默认路由、MASQUERADE/DNAT，并原子写 metadata；最后只有收到字节 `'1'`，子进程才执行用户命令。

生命周期没有 daemon。metadata 保存 PID 和 `/proc` start time，查询时发现 PID 消失或复用，就从 RUNNING lazy 更新为 EXITED。`exec` 会先确认 init PID 仍属于目标 cgroup，然后用 namespace worker 打开并加入 ipc/uts/net/mnt/pid namespace，再 fork 真正命令进程；`stop` 对 cgroup 全部进程先 TERM 后 KILL；`rm` 只有在容器不运行且 metadata 可信时才卸载 volume/rootfs、删除 DNAT/veth、释放 IP、删 workspace/cgroup，最后删 metadata。

我认为项目更有价值的部分是失败语义。metadata 和网络状态都拒绝 symlink、非普通文件、超大或损坏内容，写入采用 temp + fsync + rename；网络状态修改有 flock；外部命令全部用 argv 执行而不是 shell；bridge/veth 删除前核对 interface alias、类型和 ifindex；如果 mount 可能还活着，cleanup 明确拒绝递归删 workspace。也就是说，它倾向于留下可检查残留，而不是误删宿主资源。

测试分两层。默认 `make check` 是非特权的，覆盖解析器、codec、路径、PID start time、cgroup parser、幂等删除和无 shell runner，并运行严格 `-Werror` 与 ASan/UBSan。特权 observability/full suite 必须在 disposable Linux VM 中显式 opt-in，才会真的创建 namespace、mount、cgroup、veth 和 iptables。

项目仍有清晰缺陷：不支持 OCI、rootless、seccomp、capability、DNS/IPv6、daemon 与强一致并发；port mapping 不持久化；Sanitizer 不覆盖 runtime；CLI 接受 128 个 volume 而 metadata 只能保存 32 个，这是应优先修复的资源生命周期缺口。下一步我会先统一资源 manifest 和容量，再加容器级锁、pidfd 与可故障注入的系统调用抽象，然后扩大 Linux 特权测试矩阵。
