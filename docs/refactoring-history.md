# tinydocker 渐进式重构记录

本文只记录能由当前代码、测试和构建文件直接确认的整理结果，不推断没有保留在仓库中的历史。这里的“本次”指当前工作区已经体现的行为基线、cgroup 生命周期解耦和工程化基础设施；不是一次完整架构迁移。

## 重构前暴露的问题

### 1. 启动流程缺少显式阶段

`docker/container.c:docker_run()` 顺序调用 filesystem、volume、cgroup、clone、network、metadata，但原先失败日志难以统一指出出错阶段。局部变量只能表达“有一个路径/PID”，不能表达资源是否由本次启动拥有，回滚容易依赖名称猜测。

### 2. cgroup 生命周期与编排耦合

run 路径原本分别调用 `init_cgroup()`、`set_cgroup_limits()`、`apply_cgroup_limit_to_pid()`、`remove_cgroup()`。这些低层接口本身仍存在并服务其他命令，但启动编排无法通过一个状态对象知道 cgroup 创建是否成功、cleanup 是否有权删除。

### 3. 配置、临时状态、持久状态没有明确命名

CLI 的 `struct docker_run_arguments` 同时被 parser、workspace、volume、status 和 run 直接消费。持久态 `struct container_info` 已存在，但单次启动的 acquisition state 没有独立表达。

### 4. 行为缺少足够基线

已有 `tests/test_core.c` 能覆盖纯 helper，但 parser 输出/退出码、metadata/network public interface、资源所有权语义和真实 failure rollback 需要更明确的 characterization tests。尤其不能在重构时把当前“奇怪但可见”的行为误当 bug 顺手修改。

### 5. 构建入口不够分层

默认 build、source groups、Debug/Release、依赖文件、parser/runtime-state tests、格式检查和本地脚本没有统一呈现。维护者难以快速判断在 macOS 能测什么、Linux CI 测什么、哪些测试会触发特权副作用。

## 本次实际完成的内容

### 行为基线

- `docs/behavior-baseline.md` 记录 CLI、公开头文件、文本格式、错误/退出码、宿主资源变化和已确认缺陷。
- `tests/test_cmdparser.c` 锁定 run/stop 参数与回显，以及当前 invalid command 行为。
- `tests/test_cli_blackbox.sh` 锁定无参数、未知命令、非法名称/CIDR、未实现 network connect、`-d -i` 冲突的 stdout/stderr/退出码，并验证 runtime 没有变化。
- `tests/test_runtime_state.c` 通过临时路径覆盖 metadata public interface、network allocation/release/concurrent allocation 和 cgroup state ownership。

这些测试记录当前真实行为，不把缺陷“修正后的期望”写成 baseline。

### 配置与状态表达

`runtime/container.h` 新增/集中表达：

- `struct container_config`：从 `docker_run_arguments` 构造的非 owning 只读视图；
- `enum container_stage`：从 `config.prepare` 到 `complete` 的启动阶段；
- `struct container_runtime_state`：本次启动的阶段与已迁移 subsystem state。

这没有替换现有 CLI 结构，也没有改变 `docker_run()` 的 public signature。当前 `container_config` 的多数成员仍只是为后续渐进迁移准备的语义视图，真正切换到 config/state 接口的 subsystem 只有 cgroup。

### cgroup 三段式生命周期

`docker/cgroup.h/.c` 增加：

```c
struct cgroup_config { int cpu; int memory; const char *cpuset; };
struct cgroup_state { char path[PATH_MAX]; int created; };

cgroup_prepare(...);
cgroup_apply(...);
cgroup_cleanup(...);
```

`docker_run()` 现在在 `cgroup.prepare` 阶段创建并设置限制，在 `cgroup.apply` 阶段写 child PID，失败时把 `state.cgroup` 交给 `cgroup_cleanup()`。prepare 内部设置限制失败也会回滚，并尽量保留原 `errno`。

旧接口没有删除：`cgroup_prepare()` 继续复用 `init_cgroup()` 和
`set_cgroup_limits()`，`docker_exec()` 使用 `apply_cgroup_limit_to_pid()`，
`clean_container_runtime()` 使用 `remove_cgroup()`；`top/stop/network` 等路径则使用
`get_container_processes_id()`。保留现有 cgroup 公共接口让本阶段 diff 可审查、可回滚。

### 阶段日志与失败回滚验证

`docker/container.c:container_stage_name()` 把 enum 映射为稳定的诊断文本。`docker_run()` 在失败时记录当前 stage，然后进入 `cleanup_run_failure()`。

`tests/test_full_function.sh` 增加两个需要 disposable Linux VM/root 的失败注入场景：

1. 不存在的 cgroup parent 触发 `stage=cgroup.prepare`，验证 workspace/metadata 不残留；
2. 普通目录模拟 cgroup parent，使 prepare 能 mkdir、apply 不能写 `cgroup.procs`，触发 `stage=cgroup.apply`，验证 blocked child 被 kill/reap、workspace/metadata/owned cgroup 被清理。

### 工程化基础设施

`makefile` 目前明确区分 `CORE_SOURCES`、`SUPPORT_SOURCES`、`COMMAND_SOURCES`、`DOCKER_SOURCES` 和 entrypoint；对象带 `.d` 依赖；默认 build 保持 `make -> ./tinydocker`，新增 `debug`/`release`、`test`、`static-check`、`sanitize`、`check`、`privileged-test`、显式文件格式检查。

`scripts/build.sh` 和 `scripts/test.sh` 只转发相同 Make 目标。`.clang-format` 只用于新增/正在修改的文件，没有批量格式化历史代码。`.github/workflows/ci.yml` 使用 Ubuntu，执行 runtime build、非特权 tests、strict compile 和 sanitizers，不运行特权集成测试。

## 为什么这样拆分

本阶段选择 cgroup，因为它比 namespace、filesystem 和 network 的外部依赖更少，而且具备清晰的资源生命周期：创建目录 -> 应用限制/加入进程 -> 删除目录。`path + created` 足以解决真实的 cleanup ownership 问题，不需要通用插件、回调表或复杂设计模式。

`runtime/container.h` 只保存 orchestration 语义，底层 cgroup 文件写入仍在 `docker/cgroup.c`；`docker_run()` 仍能直接看出 prepare/apply 的顺序。这样既让 runtime 负责编排，又保留学习 cgroup v2 的可见性。

阶段 enum 是诊断和 review 边界，不是可动态扩展的 workflow engine。每个阶段仍是普通 C 调用，失败统一跳到一个明确 rollback 入口。

## 刻意保持不变的行为

- `main.c` 的命令、参数、参数回显和退出码规则未改。
- `docker_run(struct docker_run_arguments *)` 等 public signatures 未改。
- namespace flags 仍是 UTS、PID、mount、network、IPC；没有 user namespace。
- child 仍先 `init_and_set_new_root()`，再等 pipe 授权，最后 `execve()`。
- 前台等待、signal forwarding、child exit-code 透传、detached 返回时机未改。
- cgroup 路径、`cpu.max`/`memory.max` 语义和 legacy cgroup public functions 未改。
- default bridge、CIDR、gateway、veth/iptables 行为未改。
- container metadata 与 network state 的文本格式未改。
- `delte_network()`、`init_container_workerspace()` 等现有拼写未改。
- `exec -i/-t` 仍只解析不提供 PTY；`inspect` 仍显示 `PortMappings: unavailable`。
- 没有顺带修复 behavior baseline 中记录的 parser 和 network 已知缺陷。

## 当前仍存在的技术债

### 高优先级：生命周期资源状态仍不完整

`container_runtime_state` 只追踪 cgroup。workspace、volume、network、port map、metadata 和 pipe 仍由 `mountpoint`、空/非空 IP、`pipe_created` 等分散变量推断。`cleanup_run_failure()` 已经集中回滚，但还不能精确表达每个 subsystem 已完成到哪一步。

`docker_run()` 的 `process.wait` 发生 `waitpid()` 错误时只记录并返回，不走 startup cleanup；这条异常路径需要先定义行为和补故障测试，不能自动修改。

### 高优先级：`docker/container.c` 职责过多

该文件仍同时包含 run 编排、child init、namespace worker、environment、signal/wait、stop/rm、inspect/stats。namespace/process seam 尚未独立；一次性拆分会同时扰动 fork/clone 后的地址空间、fd 所有权和退出码，不适合自动执行。

### 中优先级：依赖方向仍有反向引用

`runtime/container.h` 直接包含 `docker/cgroup.h`；`docker/cgroup.c`、
`docker/network.c`、`docker/status_info.c` 依赖 `cmdparser/cmdparser.h`；
workspace/volume/status public interfaces 仍直接接收 CLI structs。

### 中优先级：network 模块过宽且有当前缺陷

`docker/network.c` 同时处理 codec I/O、锁、bridge ownership、IPAM、veth、namespace 配置和 DNAT。其通用 `connect_container()` 对传入非默认 network 仍硬编码 `/24` 和 default gateway；port mappings 不写 container metadata。两者是用户可见语义，必须先决定兼容策略和补特权测试。

### 中优先级：配置视图尚未成为真正边界

`container_config` 汇总了 image、volume、env、command、port 等字段，但 filesystem/network/status 仍读取 `docker_run_arguments`。在没有逐 subsystem 迁移前，它只是过渡 seam，不能宣称 runtime 与 command layer 已完全解耦。

### 中优先级：全局与隐式状态

`docker/container.c` 有静态全局 `pipe_fd`、`foreground_child_pid` 和 8 MiB `child_stack`；`logger/log.c` 有全局 `L`；`status_info.c` 的 `str_status` 是全局数组。它们让并发/重入测试困难。项目目前是单 CLI 流程，是否改为实例 state 需要先明确并发目标。

### 低优先级：历史 utility 与命名

`util/utils.c` 包含不同职责且有重复 include；`test.c` 是不编译的早期实验；root-level `network`/`iplist.txt` 是历史样例；多个 public 名称有拼写问题。清理这些内容可能影响学习资料或外部引用，应单独审查，不能与 lifecycle 变更混在一起。

## 下一步不建议自动执行的重构

以下动作都需要维护者先确认业务/兼容语义，不应由工具自行推进：

1. **一次性建立 `namespace/ filesystem/ network/ process/ config/ common/` 全套新目录。** 当前调用方和头文件高度交叉，批量移动只会制造不可审查 diff。
2. **改变 child 授权顺序。** 把 pipe wait 移到 `pivot_root` 前会改变失败时机、mount 副作用和 child 退出行为；历史 README 已按代码修正，不能反向用旧文档改变实现。
3. **修复 custom network `/24`/gateway。** 这是正确性问题，但会改变现有行为，需要专门 spec、metadata/CLI 决策和 privileged regression。
4. **持久化 port mappings 或改变 metadata/network 格式。** 必须设计向后兼容读取与旧状态迁移。
5. **将 PID 身份改为 pidfd 或引入 daemon。** 会改变生命周期模型和宿主要求，超出渐进整理。
6. **改名 public typo 或删除 legacy cgroup API。** 需要兼容 wrapper、调用方迁移和独立 deprecation 计划。
7. **引入 OCI、runc/containerd、CNI、插件系统或重量级框架。** 会破坏项目的轻量教学目标。
8. **全量 clang-format/clang-tidy 清理。** 历史格式/警告不应淹没行为性 diff；只检查正在修改的文件。
9. **顺手删除 root-level 历史文件或示例 image。** 需要先确认文档/学习资料引用和仓库发布意图。

## 推荐的后续推进原则

每次只选择一个真实 subsystem seam：先补 current-behavior regression，再引入该 subsystem 的最小 config/state，迁移 `docker_run()` 中一个连续阶段，验证 prepare/apply/cleanup 和 failure rollback，最后才考虑移动文件。每一步都应保持默认 build/test 可运行，并保留旧 public interface，直到所有调用方完成迁移。
