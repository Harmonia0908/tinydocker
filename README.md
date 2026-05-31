# tinydocker

`tinydocker` 是一个基于 `xanarry/tinydocker` 的 C 语言轻量级容器运行时二次开发分支。项目用于学习 Docker/容器运行时的核心机制：Linux namespace、cgroup v2、OverlayFS、`pivot_root`、卷挂载、bridge/veth 网络、iptables 端口映射以及容器 metadata 管理。

这个项目仍然是教学型系统编程项目，不是生产级 Docker、containerd 或 runc。它不兼容 OCI runtime spec，没有 daemon，没有镜像仓库能力，也没有完整的安全沙箱。

## 已实现功能

当前代码实际支持以下命令和能力：

| 功能 | 命令/参数 | 代码位置 | 说明 |
|---|---|---|---|
| 启动容器 | `run` | `docker/container.c` | 创建 workspace、cgroup、namespace、网络和 metadata 后执行用户命令。 |
| 前台/后台运行 | `-i`、`-d` | `cmdparser/cmdparser.c` | `-i` 和 `-d` 互斥。后台容器输出会重定向到 runtime logs。 |
| 容器命名 | `-n <name>` | `cmdparser/cmdparser.c` | 未指定时使用当前时间戳。名称只允许字母、数字、`.`、`_`、`-`。 |
| CPU 限制 | `-c <quota>` | `docker/cgroup.c` | 写入 cgroup v2 `cpu.max`，格式为 `<quota> 100000`，代码要求 quota 不小于 1000。 |
| 内存限制 | `-m <bytes>` | `docker/cgroup.c` | 写入 cgroup v2 `memory.max`，单位是 bytes。 |
| 环境变量 | `-e KEY=VALUE` | `cmdparser/cmdparser.c`、`docker/container.c` | `run` 和 `exec` 都支持传入用户环境变量。 |
| 卷挂载 | `-v host:container[:ro\|rw]` | `docker/volumes.c` | 使用 bind mount；默认读写，`ro` 会 remount 为只读。 |
| 端口映射 | `-p host_port:container_port` | `docker/network.c` | 通过 iptables DNAT 添加 OUTPUT 和 PREROUTING 规则。 |
| 容器列表 | `ps`、`ps -a` | `docker/container.c`、`docker/status_info.c` | 遍历 `container_info` metadata；查询时会做 lazy status refresh。 |
| 容器详情 | `inspect <name>` | `docker/container.c` | 输出 metadata、IP、卷和 cgroup 路径。端口映射当前未持久化，显示 `unavailable`。 |
| 资源快照 | `stats <name>` | `docker/container.c` | 读取 cgroup v2 的 CPU、内存、pids 指标。是单次快照，不是持续监控。 |
| 进程列表 | `top <name>` | `docker/container.c` | 读取 `cgroup.procs` 后调用宿主机 `ps -f -p ...`。 |
| 进入容器 | `exec <name> <cmd>` | `docker/container.c` | 使用 `setns` 加入容器 namespace，再 fork/exec 用户命令。 |
| 停止容器 | `stop [-t seconds] <name...>` | `docker/container.c` | 对 cgroup 中进程先发 SIGTERM，等待后发 SIGKILL，并更新状态。 |
| 删除容器 | `rm <name...>` | `docker/container.c` | 只删除非 RUNNING 容器，清理 cgroup、mount、workspace、IP、iptables 和 metadata。 |
| 导出快照 | `commit <name> [tar_path]` | `docker/container.c`、`util/utils.c` | 将容器当前 mountpoint 打包为 tar。 |
| 网络管理 | `network create/ls/rm` | `docker/network.c` | 使用 Linux bridge，网络信息记录在 runtime networks 文件。 |

## 工作原理概览

`tinydocker run` 的主流程如下：

```text
用户输入
-> main.c: main()
-> docker/container.c: init_docker_env()
-> cmdparser/cmdparser.c: parse_docker_cmd()
-> docker/container.c: docker_run()
-> docker/workspace.c: init_container_workerspace()
-> docker/volumes.c: mount_volumes()
-> docker/cgroup.c: init_cgroup() / set_cgroup_limits()
-> clone(CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWIPC)
-> docker/cgroup.c: apply_cgroup_limit_to_pid()
-> docker/network.c: connect_container()
-> docker/network.c: set_container_port_map()
-> docker/status_info.c: create_container_info() / write_container_info()
-> 父进程通过 pipe 通知子进程继续执行
-> docker/workspace.c: init_and_set_new_root()
-> execve(container command)
```

几个关键点：

- 容器本质是宿主机上的进程；隔离来自 namespace，资源限制来自 cgroup。
- 项目使用 `clone()` 创建带 UTS、PID、Mount、Network、IPC namespace 的子进程，没有启用 user namespace。
- 父进程创建子进程后先把 child pid 写入 cgroup，再通过 pipe 通知子进程执行用户命令，避免用户命令在资源限制生效前运行。
- 文件系统使用 OverlayFS：镜像/rootfs 是 lowerdir，每个容器有独立 upperdir、workdir 和 mountpoint。
- 子进程通过 `pivot_root` 把 mountpoint 切换为容器内 `/`，并重新挂载 `/proc`。
- 容器网络使用 veth pair，一端接入宿主机 bridge，一端移动到容器 network namespace。
- 端口映射通过 iptables DNAT 实现；但端口映射没有写入 `container_info`，所以 `inspect` 只能显示 `PortMappings: unavailable`。

## 仓库结构

```text
.
├── README.md
├── makefile
├── main.c
├── busybox.tar.xz
├── docker.md
├── cmdparser/
│   ├── cmdparser.c
│   └── cmdparser.h
├── docker/
│   ├── cgroup.c
│   ├── cgroup.h
│   ├── container.c
│   ├── container.h
│   ├── network.c
│   ├── network.h
│   ├── status_info.c
│   ├── status_info.h
│   ├── volumes.c
│   ├── volumes.h
│   ├── workspace.c
│   └── workspace.h
├── util/
│   ├── utils.c
│   └── utils.h
├── logger/
│   ├── log.c
│   └── log.h
├── tests/
│   ├── test_observability.sh
│   └── test_full_function.sh
└── docs/
    ├── FULL_FUNCTION_TEST.md
    └── project_interview_notes.md
```

重要文件说明：

- `main.c`：命令入口，负责初始化环境、解析命令并分发。
- `cmdparser/`：命令行解析和参数结构体定义。
- `docker/container.c`：容器生命周期主流程，包括 `run/ps/top/exec/stop/rm/commit/inspect/stats`。
- `docker/workspace.c`：镜像 rootfs、OverlayFS、`pivot_root` 和 `/proc` 挂载。
- `docker/cgroup.c`：cgroup v2 目录和文件操作。
- `docker/network.c`：bridge/veth/IP 分配/iptables 端口映射。
- `docker/status_info.c`：容器 metadata 读写、状态更新和 lazy status refresh。
- `docker/volumes.c`：卷 bind mount 和只读 remount。
- `util/utils.c`：路径、目录、tar、SHA256、时间格式化等工具函数。
- `logger/`：第三方轻量日志库 `rxi/log.c`。
- `tests/`：Linux root 环境下的端到端回归脚本。
- `docs/FULL_FUNCTION_TEST.md`：完整手工和自动测试说明。
- `docs/project_interview_notes.md`：项目学习与面试复习说明文档。
- `test.c`：历史临时实验文件，不参与当前构建，且当前内容不是可靠测试。
- `a.png`、`b.png`、`c.png`、`iplist.txt`：历史资料或实验残留，当前主流程代码不依赖。

## 环境要求

项目依赖 Linux 内核特性和 root 权限。推荐环境：

- Debian 12 或 Ubuntu 22.04
- root 或 passwordless sudo
- cgroup v2 mounted at `/sys/fs/cgroup`
- overlayfs 可用
- CPU 架构匹配的 rootfs tar 包

依赖软件：

```bash
sudo apt-get update
sudo apt-get install -y build-essential libssl-dev bridge-utils iproute2 iptables tar curl
```

检查 cgroup v2 和 overlayfs：

```bash
stat -fc %T /sys/fs/cgroup
grep overlay /proc/filesystems
```

期望：

- `/sys/fs/cgroup` 输出 `cgroup2fs`
- `/proc/filesystems` 包含 `overlay`

注意：本项目不能在 macOS 上完整编译或运行。代码依赖 `argp.h`、`clone()`、`setns()`、Linux mount flags、cgroup v2、iptables、bridge 等 Linux 专有能力。

## 编译

```bash
make clean
make
```

`makefile` 实际执行：

```bash
gcc -Wall logger/*.c util/*.c cmdparser/*.c docker/*.c main.c -lcrypto -o tinydocker
```

清理：

```bash
make clean
```

## Runtime 目录

代码中硬编码 runtime 根目录为 `/home/xanarry/tinydocker_runtime`。如果运行环境没有 `/home/xanarry`，需要先创建，或者修改相关宏定义。

相关宏：

```c
#define TINYDOCKER_RUNTIME_DIR "/home/xanarry/tinydocker_runtime"
#define CONTAINER_STATUS_INFO_DIR "/home/xanarry/tinydocker_runtime/container_info"
#define CONTAINER_LOG_DIR "/home/xanarry/tinydocker_runtime/logs"
#define CONTAINER_NETWORKS_FILE "/home/xanarry/tinydocker_runtime/networks"
```

运行后目录结构：

```text
/home/xanarry/tinydocker_runtime
├── container_info    # 每个容器一个 metadata 文件
├── containers        # 容器 upperdir/workdir/mountpoint
├── images            # tar rootfs 按 SHA256 解压缓存后的只读层
├── logs              # 后台容器 stdout/stderr
└── networks          # bridge 网络和已分配 IP 记录
```

## 使用示例

启动一个后台容器：

```bash
sudo ./tinydocker run -d -n demo busybox.tar.xz /bin/sh -c 'sleep 120'
```

查看容器：

```bash
sudo ./tinydocker ps
sudo ./tinydocker ps -a
sudo ./tinydocker inspect demo
sudo ./tinydocker stats demo
sudo ./tinydocker top demo
```

进入容器执行命令：

```bash
sudo ./tinydocker exec demo /bin/sh -c 'echo exec-ok > /tmp/exec-ok'
```

停止和删除：

```bash
sudo ./tinydocker stop -t 1 demo
sudo ./tinydocker rm demo
```

带资源限制、环境变量、卷挂载、端口映射的启动示例：

```bash
sudo ./tinydocker run -d \
  -n test_container \
  -c 20000 \
  -m 134217728 \
  -e TD_ENV=ok \
  -v /tmp/td_host_rw:/rw_dir \
  -v /tmp/td_host_ro:/ro_dir:ro \
  -p 18080:8080 \
  busybox.tar.xz /bin/sh -c 'echo "$TD_ENV" > /rw_dir/env.out; sleep 120'
```

创建和删除网络：

```bash
sudo ./tinydocker network create td_net 172.18.0.0/24
sudo ./tinydocker network ls
sudo ./tinydocker network rm td_net
```

导出容器当前文件系统快照：

```bash
sudo ./tinydocker commit test_container /tmp/test_container.tar
```

## 参数说明

| 参数 | 说明 |
|---|---|
| `-i` | 前台交互模式。 |
| `-d` | 后台运行；不能和 `-i` 同时使用。 |
| `-n <name>` | 设置容器名；未设置时使用当前时间戳。 |
| `-v host:container[:ro\|rw]` | 卷挂载。默认读写，第三段为 `ro` 时只读。 |
| `-c <quota>` | CPU quota，写入 `cpu.max` 的第一个字段，代码要求不小于 1000。 |
| `-m <bytes>` | 内存限制，写入 `memory.max`，单位 bytes。 |
| `-e KEY=VALUE` | 传入环境变量。 |
| `-p host_port:container_port` | 端口映射。代码实际使用冒号分隔，不是等号。 |

## inspect 和 stats

`inspect` 读取 `container_info` metadata，并输出：

- Name
- PID
- Status
- Image
- Command
- Created
- IP
- CgroupPath
- Volumes
- PortMappings

示例：

```bash
sudo ./tinydocker inspect demo
```

端口映射当前没有持久化到 metadata，因此输出固定为：

```text
PortMappings: unavailable
```

`stats` 读取 cgroup v2 文件系统中的资源快照：

- `cpu.stat` 中的 `usage_usec`
- `memory.current`
- `memory.max`
- `cpu.max`
- `pids.current`
- `pids.max`

示例：

```bash
sudo ./tinydocker stats demo
```

如果某些 cgroup 文件不存在或读取失败，对应字段会显示 `N/A`。

## Lazy Status Refresh

`tinydocker` 没有 daemon。后台容器启动后，父进程直接返回；如果容器后来自己退出，metadata 中的状态可能仍然是 `RUNNING`。

为缓解这个问题，`ps`、`inspect`、`stats` 查询路径会执行 lazy status refresh：

1. 如果 metadata 状态不是 `RUNNING`，直接使用原状态。
2. 如果状态是 `RUNNING`，检查 `/proc/<pid>` 是否存在。
3. 如果 `/proc/<pid>` 不存在，把状态写回为 `EXITED`。
4. 写回失败只输出 warning，不让查询命令崩溃。

这是无 daemon 设计下的折中方案，不是强一致状态管理。它不能完全解决 PID reuse 问题。

## 测试

完整测试需要 Linux root 环境。

可观测性回归：

```bash
sudo bash tests/test_observability.sh
```

覆盖内容：

- 编译；
- 后台短生命周期容器启动；
- `inspect` 输出检查；
- `stats` 指标检查；
- 容器退出后 lazy refresh 将 `RUNNING` 修正为 `EXITED`；
- `ps -a` 显示退出状态。

全功能回归：

```bash
sudo bash tests/test_full_function.sh
```

覆盖内容：

- `run`
- volume
- env
- cgroup limit
- `ps`
- `inspect`
- `stats`
- `top`
- `exec`
- `network create/ls/rm`
- port mapping
- lazy refresh
- `commit`
- `stop`
- `rm`

如果仓库内 `busybox.tar.xz` 和 VM 架构不匹配，可能出现 `Exec format error`。ARM64 VM 可参考 `docs/FULL_FUNCTION_TEST.md` 生成架构匹配的 rootfs，并通过 `IMAGE=/path/to/rootfs.tar.xz` 指定。

## 已知边界和风险

- 项目是教学型容器运行时，不是生产级 Docker/containerd/runc。
- 不支持 OCI runtime spec。
- 没有 daemon，状态通过 metadata 和 lazy refresh 弱同步。
- 没有完整镜像仓库、镜像 tag、镜像 manifest 或多 layer metadata。
- 没有 seccomp、AppArmor/SELinux、capabilities drop、user namespace 等完整安全隔离。
- 端口映射通过 iptables 生效，但没有持久化到 `container_info`。
- 网络 metadata 和容器 metadata 没有文件锁，并发执行多个命令可能产生竞争。
- 代码大量使用 `system()` 调用 `tar`、`ip`、`brctl`、`iptables`、`nsenter`，工程上仍有命令注入和错误处理粒度不足的问题。
- runtime 路径硬编码为 `/home/xanarry/tinydocker_runtime`。
- `test.c` 是历史临时实验代码，不参与构建，不应作为测试依据。

## 进一步阅读

- [完整功能测试说明](docs/FULL_FUNCTION_TEST.md)
- [项目学习与面试复习说明文档](docs/project_interview_notes.md)
- [容器原理说明](docker.md)

## 简历表述参考

**TinyDocker 二次开发：轻量级容器运行时可观测性与健壮性增强**

- 基于 C 语言教学型容器运行时实现容器生命周期管理，使用 Linux namespace、cgroup v2、OverlayFS、`pivot_root`、veth/bridge/iptables 串联容器启动、资源限制、文件系统隔离和基础网络。
- 扩展 `inspect` / `stats` 命令，支持查询容器 metadata、运行状态、挂载信息、IP、cgroup 路径及 cgroup v2 资源指标快照。
- 实现无 daemon 场景下的 lazy status refresh，在 `ps/inspect/stats` 查询路径中通过 `/proc/<pid>` 检测修正失效 `RUNNING` 状态。
- 加固 metadata 读写、命令解析和失败清理路径，并通过 Shell 端到端测试覆盖主流程、可观测性、网络、端口映射和容器清理。
