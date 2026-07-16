# tinydocker behavior baseline

This document records the observable behavior of the repository before the
planned structural refactoring. It is a characterization of the current code,
including known defects. It is not a statement of desired future behavior.

Baseline date: 2026-07-16

## Scope and verification status

The baseline covers these public seams:

- the `tinydocker` command line;
- `parse_docker_cmd()` and the command argument structures;
- the public functions declared by `core/*.h`;
- container metadata through `status_info.h`;
- network state allocation through `network.h`;
- the privileged integration scripts.

The verification host was:

```text
Darwin 27.0.0 arm64
Apple clang 21.0.0
GNU Make 3.81
OpenSSL 3.6.3 from Homebrew
```

The host did not have the Linux `ip`, `iptables`, or `nsenter` commands. A
Docker CLI was present, but no Docker daemon was running. Therefore the Linux
runtime binary and privileged container paths were not executed on this host.

### Commands actually executed

| Command | Exit | Observed result |
|---|---:|---|
| `make clean && make` | 2 | Refused the runtime build because the platform is Darwin; printed `tinydocker runtime build requires Linux` and pointed to `make check` |
| `make test` before baseline tests | 0 | `all non-privileged core tests passed` |
| `make static-check` | 0 | Strict syntax check of the portable core passed |
| `make sanitize` | 0 | ASan/UBSan core tests passed |
| `make test` after baseline tests | 0 | Core tests and non-privileged runtime-state tests passed |
| `make check` from a clean tree | 0 | Rebuilt and passed portable tests, strict core syntax checks, ASan, and UBSan |
| `bash -n tests/test_cli_blackbox.sh` | 0 | Shell syntax passed |
| strict syntax check of `tests/test_cmdparser.c` | 0 | Test source passed; GNU `argp` implementation was not executable on Darwin |
| `make build/test_cmdparser` | 2 | Compilation stopped at `cmdparser/cmdparser.c`: `argp.h` is unavailable on Darwin |
| `./tinydocker` | not run | No Darwin runtime binary was produced, so there was no executable current version to launch safely |

The build/test commands made these file changes:

- `make clean` removed `build/`, `tinydocker`, `a.out`, and root-level `*.o`;
- the rejected Darwin `make` produced no `tinydocker` binary;
- `make check` created `build/test_core`, `build/test_runtime_state`, and
  `build/test_core_sanitize`;
- `build/test_runtime_state` created a unique
  `build/test-runtime-state.XXXXXX/runtime` workspace, wrote metadata,
  `networks`, and `networks.lock` below it, then removed that workspace;
- the failed parser build produced no `build/test_cmdparser` binary.

On Linux, the intended safe verification sequence is:

```bash
make clean
make
make check
```

Privileged tests require an explicitly selected disposable Linux VM and are
never part of the default CI run:

```bash
TINYDOCKER_ALLOW_PRIVILEGED_TESTS=1 \
PRIVILEGED_SUITE=observability \
bash tests/run_privileged.sh

TINYDOCKER_ALLOW_PRIVILEGED_TESTS=1 \
PRIVILEGED_SUITE=full \
bash tests/run_privileged.sh
```

## Build and runtime dependencies

### Build dependencies

- Linux for the complete `tinydocker` binary;
- a C11 compiler and GNU Make;
- glibc/GNU `argp` for command parsing;
- OpenSSL development headers and `libcrypto` for SHA-256;
- Bash, `cmp`, and `tar` for tests and image handling.

The default compile-time paths are:

```text
TINYDOCKER_RUNTIME_DIR=/home/xanarry/tinydocker_runtime
TINYDOCKER_CGROUP_PARENT=/sys/fs/cgroup/system.slice
```

They can be changed during a clean build:

```bash
make clean
make RUNTIME_DIR=/var/lib/tinydocker \
     CGROUP_PARENT=/sys/fs/cgroup/tinydocker.slice
```

### Privileged runtime dependencies

- Linux cgroup v2;
- OverlayFS;
- a root shell in a disposable VM;
- `ip`, `brctl`, `iptables`, `nsenter`, `ps`, and `tar`;
- a trusted rootfs archive matching the VM architecture.

The program does not invoke `sudo` itself.

## Supported commands and arguments

### `run`

```text
tinydocker run [OPTIONS] IMAGE COMMAND [ARG...]
```

| Option | Current meaning |
|---|---|
| `-v`, `--volume HOST:CONTAINER:ro|rw` | Add a bind mount |
| `-n`, `--name NAME` | Set the container name; otherwise a seconds-resolution timestamp is used |
| `-d`, `--detach[=VALUE]` | Enable detached mode whenever the option is present |
| `-i`, `--interactive[=VALUE]` | Set the parsed interactive flag whenever the option is present |
| `-c`, `--cpu-shares VALUE` | Set `cpu.max`; accepted range is 1000 through `INT_MAX` |
| `-m`, `--memory VALUE` | Set `memory.max`; accepted range is 1 through `INT_MAX` |
| `-e`, `--env KEY=VALUE` | Append an environment entry |
| `-p`, `--port HOST:CONTAINER` | Add a TCP DNAT mapping; ports must be 1 through 65535 |

Both `IMAGE` and at least one command argument are required. The parsed `-d`
and `-i` flags cannot be used together. An explicit optional value such as
`--detach=false` still enables detach because the value is ignored.

`IMAGE` may be an existing directory or a tar-compatible rootfs archive. A
directory is resolved with `realpath()` and used directly as the read-only
OverlayFS layer. For a non-directory input, the code calculates its SHA-256
and reuses `RUNTIME_DIR/images/SHA256` on a cache hit. On a cache miss it lists
the archive with `tar -tf`, rejects unsafe entry paths, extracts it without
owner/permission restoration, and publishes the cache directory.

Before starting a container, `main()` prints the parsed run configuration to
stdout. The stable labels are:

```text
interactive=
detach=
cpu=
memory=
image=
name=
host_dir:..., container_dir:..., ro:...
env_key:..., env_val:...
args_count=
port_mapping=
host:...->container:...
```

Foreground `run` returns the container process exit code. A signal termination
returns `128 + signal`. Detached `run` returns zero after host-side preparation
and metadata publication.

### Other lifecycle commands

```text
tinydocker commit CONTAINER [ARCHIVE]
tinydocker ps [-a]
tinydocker top CONTAINER
tinydocker exec [-d] [-i] [-t] [-e KEY=VALUE] CONTAINER COMMAND [ARG...]
tinydocker stop [-t SECONDS] CONTAINER [CONTAINER...]
tinydocker rm CONTAINER [CONTAINER...]
tinydocker inspect CONTAINER
tinydocker stats CONTAINER
```

- `commit` creates a gzip-compressed tar using `tar -czf`, even when the output
  name has a `.tar` suffix.
- `ps` lists running containers; `-a` includes stopped and exited containers.
- `top` runs host `ps -f -p` for PIDs read from the container cgroup.
- `exec` joins IPC, UTS, network, mount, and PID namespaces, then forks the
  actual command process.
- `stop` sends SIGTERM, polls for the configured time, then sends SIGKILL.
- `rm` refuses a running container and keeps metadata when cleanup is incomplete.
- `inspect` and `stats` may lazily change stale `RUNNING` metadata to `EXITED`.

### Network commands

```text
tinydocker network create NAME CIDR
tinydocker network ls
tinydocker network rm NAME [NAME...]
```

Only the `bridge` driver is created. `DOCKER_NETWORK_CONNECT` and
`DOCKER_NETWORK_DISCONNECT` exist in the command enum, but the command parser
and `main()` do not implement those subcommands.

## Key functional paths

- command dispatch: `main()` -> `parse_docker_cmd()` ->
  `print_docker_cmds()` where applicable -> the selected `docker_*()` or
  network function;
- container start: `main()` -> `init_docker_env()` -> `docker_run()` ->
  `init_container_workerspace()` / `mount_volumes()` -> `init_cgroup()` and
  `set_cgroup_limits()` through `cgroup_prepare()` -> `clone(child_fn)` ->
  `cgroup_apply()` and parent-side network, port-map, and metadata publication
  -> pipe authorization;
- container child: `child_fn()` -> optional log redirection -> environment
  preparation -> `init_and_set_new_root()` -> authorization-pipe read ->
  `execve()`;
- foreground completion: `docker_run()` -> signal forwarding -> `waitpid()` ->
  `update_container_status(..., EXITED)` -> child exit-code propagation;
- metadata/state: `write_container_info()` / `read_container_info()` and
  `alloc_new_ip()` / `release_used_ip()` -> locked, atomically replaced text
  files under the runtime root;
- removal: `docker_rm()` -> running-state guard -> volume/rootfs, DNAT/veth/IP,
  workspace, cgroup, and finally metadata cleanup. Incomplete cleanup retains
  metadata for inspection and retry.

## User-visible output formats

### `ps`

Tab-separated, dynamically padded columns:

```text
CONTAINER_ID  IMAGE  COMMAND  CREATED  STATUS  NAMES
```

### `inspect`

One `Label: value` field per line:

```text
Name:
PID:
Status:
Image:
Command:
Created:
IP:
CgroupPath:
CgroupAvailable:
RootfsPath:
RootfsAvailable:
Volumes:
PortMappings: unavailable
```

### `stats`

A one-shot cgroup v2 table:

```text
NAME  CPU_USEC  MEM_CURRENT  MEM_MAX  PIDS  CPU_MAX
```

Unavailable cgroup values are rendered as `N/A`.

### `network ls`

```text
NAME  DRIVER  CIDR  ALLOC_IPS
```

Allocated addresses are printed as comma-terminated dotted IPv4 values. An
empty allocation list is printed as `NULL`.

Logs are written to stderr. Several commands additionally print their parsed
arguments to stdout before performing runtime work; this is current
user-visible behavior.

### Parsed argument echoes

In addition to the `run` labels above, these commands write the following
formats before executing their operation. Repeated names and environment
entries are printed one per line.

```text
commit:
container_name=VALUE
tar_path=VALUE_OR_EMPTY

ps:
list_all=0_OR_1

exec:
interactive=0_OR_1
tty=0_OR_1
detach=0_OR_1
container_name=VALUE
env_key:KEY, env_val:VALUE
args_count=COUNT
ARG...

stop:
container_cnt=COUNT
wait_time=SECONDS
CONTAINER_NAME

rm:
container_cnt=COUNT
CONTAINER_NAME

network create:
name=VALUE
cidr=VALUE

network rm:
network_cnt=COUNT
NETWORK_NAME
```

`top`, `inspect`, `stats`, and `network ls` do not print a parsed-argument
echo. Their command output is described separately.

### No-side-effect CLI error cases

The Linux-only black-box test locks the following cases byte-for-byte. They
were derived from the current parser but were not executable on the Darwin
verification host. Every case expects exit 255, empty stderr, and no runtime
state changes; the test creates and then removes only its capture directory.

```text
tinydocker
stdout: 请输入有效的docker命令\n

tinydocker unknown
stdout: wrong input command\n

tinydocker inspect ../host
stdout: invalid container name: name contains invalid byte 0x2f; allowed: [A-Za-z0-9._-]\n

tinydocker network create baseline0 not-a-cidr
stdout: invalid network CIDR: CIDR must have address/prefix form\n

tinydocker network connect baseline0 baseline
stdout: wrong input command\n
```

The conflicting-option case is also exact:

```text
tinydocker run -d -i busybox.tar.xz /bin/true
stdout:
ERROR: -d can not use with -t -i together
Usage:  tinydocker run [OPTIONS] IMAGE [COMMAND] [ARG...]
OPTIONS:
  -v, --volume  设置卷
  -n, --name  容器名字
  -d, --detach  容器后台运行
  -i, --interactive  开启交互模式
  -c, --cpu-shares  设置cpu限制, 必须大于1000
  -m, --memory  设置内存限制
  -e, --env  环境变量
  -p, --port  设置端口映射

```

## Public C interfaces

The project does not build a separately versioned library, but these headers
form the current source-level interface between modules:

- `cmdparser/cmdparser.h`: command enum, argument structures,
  `parse_docker_cmd()`, `print_docker_cmds()`, and `parse_volume_config()`;
- `docker/container.h`: lifecycle and observability commands;
- `docker/cgroup.h`: cgroup creation, limits, process enumeration, and cleanup;
- `docker/workspace.h`: workspace preparation and root switch;
- `docker/volumes.h`: volume mount and unmount;
- `docker/network.h`: network lifecycle, IP allocation, container connection,
  and port mapping;
- `docker/status_info.h`: metadata construction, persistence, refresh, listing,
  and removal;
- `core/*.h`: validation, codecs, safe filesystem helpers, and shell-free
  process execution;
- `logger/log.h`: shared stderr logging and quiet-mode control;
- `util/utils.h`: image hashing/extraction, filesystem helpers, IP conversion,
  and timestamp formatting used across the Docker modules.

Existing function names and structure fields are part of the refactoring
compatibility baseline, including the misspelled `delte_network()` and
`init_container_workerspace()` names.

## Persistent files and formats

### Runtime layout

For a runtime root `RUNTIME_DIR`, the current code uses:

```text
RUNTIME_DIR/
├── container_info/NAME
├── containers/NAME/
│   ├── upperdir/
│   ├── workdir/
│   └── mountpoint/
├── images/SHA256/
├── logs/NAME
├── networks
└── networks.lock
```

The cgroup directory is created at
`TINYDOCKER_CGROUP_PARENT/tinydocker-NAME`.

### Container metadata

UTF-8-compatible text with one key/value per line followed by raw volume
records:

```text
pid=12345
pid_start_time=67890
detach=1
container_id=baseline-id
image=busybox.tar.xz
command=/bin/sh -c echo baseline
created=2026-07-16 12:00:00
status=EXITED
ip_addr=10.77.0.2
name=baseline-meta
volume_cnt=1
/tmp/host:/data:ro
```

Writes use a same-directory temporary file, `fsync()`, and atomic `renameat()`.
Reads reject symlinks, non-regular files, embedded NUL bytes, duplicate or
unknown fields, invalid status values, and name/file mismatches.

Legacy records without `pid_start_time` remain readable. Their PID identity
cannot be safely refreshed and produces a warning.

### Network state

One network per line:

```text
NAME:bridge:NETWORK_CIDR:UINT32_IP;UINT32_IP;
```

Example:

```text
baseline0:bridge:10.77.0.0/29:172818434;
```

`172818434` is the host-order integer representation currently used for
`10.77.0.2`. Writers take an exclusive `flock()` on `networks.lock`, serialize
the full state to a temporary file, `fsync()` it, and atomically rename it.

No network protocol is exposed. Host networking is configured by executing
argument arrays for `ip`, `brctl`, `nsenter`, and `iptables` without a shell.

## Host and file changes by operation

| Operation | Necessary current changes |
|---|---|
| `run` | runtime directories, image cache, container workspace, OverlayFS and volume mounts, cgroup, child process/namespaces, IP allocation, veth, routes, optional DNAT, metadata, optional log file |
| foreground process exit | metadata status changes to `EXITED`; other resources remain until `rm` |
| detached process exit | metadata remains `RUNNING` until a lazy-refresh command observes process disappearance |
| `stop` | sends signals and writes `STOPPED`; it does not remove resources |
| `rm` | unmounts volumes/rootfs, removes DNAT/veth/IP allocation/workspace/cgroup, then metadata |
| `commit` | creates the requested archive; image/runtime state is unchanged |
| `network create` | creates a bridge, ownership alias, network state record, address, and link-up state |
| `network rm` | verifies ownership, deletes the bridge, then removes its state record |
| `inspect`, `stats`, `ps` | may atomically update stale metadata to `EXITED` |

The default bridge and POSTROUTING MASQUERADE rule are host-level runtime
resources and are not removed by per-container cleanup.

## Error and exit behavior

- Command syntax and validation failures generally print to stdout and call
  `exit(-1)`, observed by a shell as exit code 255.
- Runtime modules normally return `-1`; `main()` converts a negative value to
  `EXIT_FAILURE` (1).
- Logs and many runtime failures are written to stderr.
- A container child exits 125 when parent authorization is missing, 126 when
  child initialization fails, and 127 when `execve()` fails.
- Foreground container exit codes pass through `main()`.
- Multi-container `stop`, `rm`, and network removal continue processing and
  return failure if any requested item failed.
- Cleanup is conservative: a workspace is not recursively removed while a
  mount may still be active, and metadata can be retained for inspection and
  retry.

## Confirmed current defects and surprising behavior

These are recorded, not fixed, by this baseline:

1. `docker stop` allocates 128 name pointers but does not bound the number of
   copied positional arguments before writing the terminating NULL. More than
   127 names can cause undefined behavior. The regression suite does not encode
   memory corruption as an expected result.
2. `KEY=value=tail` is parsed as key `KEY`, value `value`; the remainder is
   discarded because the parser uses `strtok()`.
3. `--detach=false` and equivalent optional boolean values still enable the
   flag because the option argument is ignored.
4. `stop -t 0` is accepted even though the option help says `int > 0`.
5. The `-d` plus `-i` error says `-d can not use with -t -i together`, although
   `run` has no `-t` option.
6. Network connect/disconnect enum values and argument structures exist but are
   not reachable from the CLI.
7. The internal generic `connect_container()` builds an address with a fixed
   `/24` prefix and uses the default gateway even if passed another network.
8. `exec -i` and `exec -t` are parsed but no interactive terminal plumbing is
   implemented.
9. `inspect` cannot reconstruct port mappings and always reports
   `PortMappings: unavailable`.
10. At baseline time, `README.md` described the child as waiting for
    authorization before `pivot_root`; the implementation performs the root
    switch before waiting and gates only the final user `execve()`. The README
    has since been corrected without changing this behavior.
11. The tracked root-level `test.c` is an early experiment, is not part of the
    Makefile, and does not currently compile.

## Known limitations

- Linux and root only for actual container execution;
- no OCI runtime or image specification;
- no daemon, restart policy, event stream, or health checks;
- no user namespace, rootless mode, seccomp, capability drop, or LSM profile;
- no pidfd-based process identity binding;
- cgroup v2 only, with a pre-existing delegated parent;
- IPv4 bridge networking only; no DNS, IPv6, CNI, or policy isolation;
- no transaction spanning metadata, mount, cgroup, bridge/veth, and iptables;
- port mappings are not persisted in container metadata;
- archive validation is intentionally limited and assumes trusted rootfs input;
- privileged behavior is not executed by default CI and requires a disposable
  Linux VM for confirmation.

## Regression coverage added with this baseline

- exact metadata bytes plus public write/read/list/remove behavior;
- network-state allocation, release, and exact serialized bytes;
- two-process network allocation through the existing file lock;
- valid run and stop command parsing on Linux;
- current environment-value truncation and zero stop timeout behavior;
- parser output labels and invalid-command exit behavior on Linux;
- actual binary error paths that do not initialize runtime resources.

The baseline deliberately does not test undefined memory-overflow behavior or
invoke privileged host mutations on an unapproved machine.
