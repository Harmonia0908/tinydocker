# tinydocker 构建、测试与调试

本文给刚接手项目的维护者一条从“确认环境”到“定位启动阶段”的实际路径。所有特权命令都只应在可丢弃的 Linux VM 中运行。

## 环境要求

完整 runtime 只能在 Linux 构建和运行，原因是 `docker/container.c`、`docker/workspace.c` 直接使用 Linux `clone()`、`setns()`、namespace flags、mount 和 `pivot_root`，`docker/cgroup.c` 使用 cgroup v2 文件接口。

构建依赖：

- C11 compiler、GNU Make；
- glibc/GNU `argp.h`；
- OpenSSL headers 和 `libcrypto`（`util/utils.c:calculate_sha256()`）；
- Bash 和 tar。

特权运行另需：

- Linux cgroup v2 与 OverlayFS；
- root shell；tinydocker 和脚本都不会自行调用 `sudo`；
- `ip`、`brctl`、`iptables`、`nsenter`、`ps`、`tar`；
- 与 VM CPU 架构匹配、可信的 rootfs 目录或归档。

Ubuntu/Debian 的典型依赖为 `build-essential libssl-dev bridge-utils iproute2 iptables tar`。安装系统包会修改机器，应由维护者在隔离 VM 中显式完成。

项目没有 CMakeLists.txt，也没有使用 CMake/CTest；测试入口是 Makefile。

## 构建

### 默认构建

```bash
make clean
make
```

默认使用 `-O2 -g`，输出仓库根目录的 `tinydocker`。Makefile 对 runtime 对象生成 `.d` 依赖文件。

### Debug 与 Release

```bash
make debug
make release
```

- Debug：`-O0 -g3`，输出 `build/debug/tinydocker`。
- Release：`-O2 -DNDEBUG`，输出 `build/release/tinydocker`。

两种配置使用独立对象目录。脚本是相同 Make 目标的薄封装：

```bash
scripts/build.sh
scripts/build.sh debug CC=clang
scripts/build.sh release
```

默认编译期路径在 `core/config.h`，可由 Make 参数覆盖：

```bash
make clean
make RUNTIME_DIR=/var/lib/tinydocker \
     CGROUP_PARENT=/sys/fs/cgroup/tinydocker.slice
```

更换这些参数后必须 clean build，因为它们通过 `CPPFLAGS` 编译进二进制。自定义 cgroup parent 必须事先存在并完成 controller delegation；`cgroup_prepare()` 不会创建未知父层级或修改 `cgroup.subtree_control`。

### macOS 上能做什么

macOS 的 `make` 会明确拒绝 runtime build。仍可运行 portable test：

```bash
make check
```

Darwin 分支不编译 GNU argp parser，也不编译 Linux runtime；它执行 core、runtime-state、core strict syntax 和 core sanitizers。当前环境的实测记录见 `docs/behavior-baseline.md`。

## 测试

### 默认非特权测试

```bash
make test
make static-check
make sanitize
make check
scripts/test.sh
```

| 目标 | Linux | macOS | 是否创建真实容器资源 |
|---|---|---|---|
| `make test` | core、runtime-state、cmdparser、CLI black-box；会先构建 runtime | core、runtime-state | 否 |
| `make static-check` | 所有 production sources，strict warnings + `-Werror` | portable core | 否 |
| `make sanitize` | `test_core` 的 ASan/UBSan | 同左 | 否 |
| `make check` | 上述全部 | 可用子集 | 否 |

`tests/test_runtime_state.c` 会在 `build/test-runtime-state.XXXXXX` 下建立临时 runtime/cgroup 目录，测试完成后删除。它不会写真实 `/sys/fs/cgroup`。`tests/test_cli_blackbox.sh` 只锁定无副作用错误路径，要求 stdout、stderr、退出码和 runtime tree 保持当前行为。

格式配置可单独验证。历史代码没有全量格式化，所以必须显式列出正在修改的文件：

```bash
make format-config-check
make format-check FILES="runtime/container.h"
```

### 特权集成测试

特权 suite 会真实创建 namespace、mount、cgroup、bridge/veth 和 iptables 规则。先阅读脚本，再在 disposable VM 的 root shell 中显式 opt in：

```bash
TINYDOCKER_ALLOW_PRIVILEGED_TESTS=1 \
PRIVILEGED_SUITE=observability \
make privileged-test

TINYDOCKER_ALLOW_PRIVILEGED_TESTS=1 \
PRIVILEGED_SUITE=full \
make privileged-test
```

`tests/run_privileged.sh` 负责 gate 和前置检查；`test_observability.sh` 验证 run/inspect/stats/ps lazy refresh；`test_full_function.sh` 覆盖 volume、exec、stop/rm、network、port、commit，以及 cgroup prepare/apply 失败回滚。脚本只清理由本次唯一名称标记为 owned 的资源，清理不确定时会失败并报告，不会盲目删除宿主资源。

## 运行

最小手工流程：

```bash
./tinydocker run -d -n debug-demo busybox.tar.xz /bin/sh -c 'sleep 120'
./tinydocker inspect debug-demo
./tinydocker stats debug-demo
./tinydocker top debug-demo
./tinydocker stop -t 1 debug-demo
./tinydocker rm debug-demo
```

这些命令必须在 root Linux VM 中执行，并会修改宿主机。更安全的完整演示入口是先审查 `demo.sh`，再运行：

```bash
TINYDOCKER_ALLOW_PRIVILEGED_DEMO=1 bash demo.sh
```

`run` 接受 rootfs 目录或 tar-compatible archive。不要使用不可信归档；`extract_tar()` 会拒绝绝对路径和 `..` entry，但没有实现生产级 tar symlink/hardlink 威胁模型。

## 推荐调试顺序

### 1. 先看阶段日志

`docker_run()` 在失败入口打印：

```text
container startup failed: stage=STAGE container=NAME
```

常见 stage 与第一检查点：

| stage | 首先检查 |
|---|---|
| `container.check` | 同名 metadata、workspace、cgroup 是否残留 |
| `filesystem.prepare` | image、tar、OverlayFS、runtime path、mount 权限 |
| `filesystem.volumes` | host 目录、container absolute path、symlink、bind/remount |
| `cgroup.prepare` | cgroup v2、parent、controller、目录权限、limit 值 |
| `process.pipe` | file descriptor/process limit |
| `process.clone` | root、kernel namespace 支持、内存/stack |
| `cgroup.apply` | `cgroup.procs` 是否可写，child 是否仍存活 |
| `network.prepare` | default bridge、IP state、veth ownership、`ip/brctl/nsenter` |
| `network.ports` | iptables nat table/权限/规则冲突 |
| `state.write` | `container_info` 目录、临时文件、fsync/rename、字段长度 |
| `process.release` | pipe 被提前关闭、child 已初始化失败 |
| `process.wait` | waitpid/信号竞争；当前路径不会执行 startup cleanup |

### 2. 推荐断点

使用 Debug binary：

```bash
gdb --args build/debug/tinydocker run -n debug-demo busybox.tar.xz /bin/true
```

最有价值的断点：

- `main`、`parse_docker_cmd`：命令结构是否符合预期；
- `init_docker_env`、`create_default_bridge`：宿主全局资源；
- `docker_run`、`container_stage_name`、`cleanup_run_failure`：编排与回滚；
- `init_container_workerspace`、`mount_volumes`、`init_and_set_new_root`：filesystem；
- `cgroup_prepare`、`cgroup_apply`、`cgroup_cleanup`：owned state；
- `connect_container`、`set_container_port_map`：网络；
- `create_container_info`、`write_container_info`、`read_container_info`：持久态；
- `child_fn`：126/125/127 的分界；
- `docker_exec` 及其 namespace worker 附近：cgroup membership、namespace fd、`setns()`；
- `docker_stop`、`clean_container_runtime`：正常终止与删除。

GDB 调试 clone/fork 时常用：

```gdb
set follow-fork-mode child
set detach-on-fork off
catch syscall clone
catch syscall setns
catch syscall mount
catch syscall pivot_root
```

需要观察父进程编排时把 `follow-fork-mode` 设为 `parent`。PID namespace 会让宿主 PID 与容器内 PID 不同，日志/metadata 中保存的是宿主侧 child PID。

### 3. 用 strace 看系统调用边界

```bash
strace -ff -s 256 \
  -e trace=clone,setns,mount,umount2,pivot_root,execve,wait4,kill,openat,write \
  -o /tmp/tinydocker.strace \
  ./tinydocker run -n debug-demo busybox.tar.xz /bin/true
```

`-ff` 会按宿主 PID 分文件，适合区分 parent、clone child、exec namespace worker 和用户进程。网络配置主要由外部 `ip/brctl/nsenter/iptables` 子进程完成，也会出现在 `execve()` 记录中。

### 4. 检查运行时状态

默认路径：

```text
/home/xanarry/tinydocker_runtime/container_info/NAME
/home/xanarry/tinydocker_runtime/containers/NAME/mountpoint
/home/xanarry/tinydocker_runtime/networks
/home/xanarry/tinydocker_runtime/logs/NAME
/sys/fs/cgroup/system.slice/tinydocker-NAME
```

metadata 中的 `pid` 必须结合 `pid_start_time` 判断身份。`refresh_container_status_if_needed()` 就是读取 `/proc/PID/stat` 的 start time 防止 PID reuse；不要只凭 `kill(pid, 0)` 推断容器仍是同一进程。

## 常见错误

### `tinydocker runtime build requires Linux`

这是 Makefile 在非 Linux 上的预期拒绝，不是编译器故障。使用 `make check` 做 portable 验证，完整 build 移到 Linux。

### `argp.h` 找不到

完整 CLI parser 依赖 GNU argp/glibc。Darwin 默认没有该头；项目没有为此引入兼容层，Linux CI 才编译 `test_cmdparser` 和 runtime。

### OpenSSL header/link 错误

检查 OpenSSL development package 和 `pkg-config`。runtime 最终链接 `-lcrypto`；runtime-state test 会读取 `OPENSSL_CFLAGS`/`OPENSSL_LDFLAGS`。

### `cgroup.prepare` 失败

检查 `/sys/fs/cgroup/cgroup.controllers`、编译期 parent 是否存在可写，以及 systemd delegation/controller 状态。代码只支持 cgroup v2；CPU 最小 parser 值为 1000，memory 必须为正数。

### `filesystem.prepare` 或 child 126

检查 image 是否存在/架构正确、tar entry 是否被拒绝、OverlayFS 是否可用、mountpoint 是否是独立 mount。child 126 还可能来自 detached log、环境读取、private propagation、`pivot_root`、旧 root 卸载或 `/proc` 挂载。

### `network.prepare` 失败

确认 `ip/brctl/nsenter/iptables` 在 PATH，default bridge metadata 与真实 interface alias 一致。`docker/network.c` 会拒绝接管同名但没有 `tinydocker-network:` ownership alias 的 bridge，也会拒绝删除 alias/type/ifindex 不匹配的接口。

### metadata 仍显示 RUNNING

detached child 退出不会由 daemon 主动更新。运行 `inspect`、`stats` 或 `ps` 会触发 lazy refresh。legacy metadata 没有 `pid_start_time` 时，代码会警告无法排除 PID reuse，并保留 RUNNING。

### `rm` 失败后 artifact 仍在

这是保守策略。volume/rootfs 可能仍挂载时，代码不会递归删除 workspace；任何清理失败都会保留 metadata。先检查日志和真实 mount/network/cgroup 所有权，再重试或人工处理，不要直接对未知路径 `rm -rf`。

### child 125/126/127

- 125：父进程授权 pipe EOF/短读/错误字节；
- 126：child 初始化失败；
- 127：最终 `execve()` 失败，常见于容器 rootfs 内命令不存在或动态链接器缺失。

## 关键功能验证清单

非特权修改至少执行：

```bash
make clean
make check
git diff --check
```

Linux runtime/build 相关修改再执行 `make`、`make debug`、`make release`。只有 subsystem 副作用或 lifecycle 变化才在 disposable VM 运行对应 privileged suite。

特权验证完成后，应确认：

- 前台退出码与用户进程一致；detached run 在 release 后返回 0；
- `inspect/stats/top/exec/stop/rm` 的现有输出和错误行为未变；
- `container_info/NAME`、workspace、`tinydocker-NAME` cgroup、veth、IP allocation、DNAT 没有超出预期的残留；
- cleanup 失败时 metadata 被保留，日志指出失败阶段；
- 默认 bridge/MASQUERADE 的持续存在与设计一致，不误判为单容器泄漏。
