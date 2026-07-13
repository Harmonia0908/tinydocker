# Privileged integration test guide

This guide covers the Linux-only integration tests. They are intentionally excluded from default CI because they create mount/network/PID/UTS/IPC namespaces, OverlayFS and bind mounts, cgroup v2 entries, veth/bridge devices, routes and iptables rules.

Use a disposable VM with a recoverable snapshot. Do not run these tests on a workstation that already hosts important containers, custom iptables rules, mounts or cgroups.

## Safety contract

- The scripts refuse to run unless `TINYDOCKER_ALLOW_PRIVILEGED_TESTS=1` is set.
- They require an already-root shell and never invoke `sudo`.
- Exactly one suite must be selected through `PRIVILEGED_SUITE`.
- Each run derives unique container/network names from its private `mktemp` directory and refuses externally supplied run IDs.
- Before mutation, the scripts verify that every generated runtime name is absent.
- Cleanup calls tinydocker only for resources marked as owned by the current run, then removes only that unique temporary directory.
- The scripts do not scan or bulk-delete `/sys/fs/cgroup`, `/var/run`, mountpoints, network devices or unrelated runtime state.
- A failed cleanup is reported; the scripts do not fall back to broad `kill`, `umount` or `rm -rf` commands.

## Prerequisites

- Linux with cgroup v2 mounted at `/sys/fs/cgroup`
- OverlayFS support
- root shell in the disposable VM
- GCC/Clang, make and OpenSSL development headers
- `ip`, `brctl`, `iptables`, `nsenter`, `ps` and `tar`
- a trusted rootfs tar matching the VM CPU architecture

Read-only preflight checks:

```bash
uname -m
stat -fc %T /sys/fs/cgroup
grep -w overlay /proc/filesystems
command -v gcc make ip brctl iptables nsenter ps tar
```

The cgroup filesystem should report `cgroup2fs`. The repository's `busybox.tar.xz` may not match the VM architecture; an architecture mismatch normally appears as `Exec format error`. Use a trusted distro/rootfs artifact built for the VM instead of disabling the check.

## Build and safe checks first

Run these before entering a root shell when possible:

```bash
make clean
make
make check
```

`make check` is non-privileged. It does not create a container or modify mount, network, namespace or cgroup state.

If the configured paths do not fit the VM, rebuild explicitly:

```bash
make clean
make RUNTIME_DIR=/var/lib/tinydocker \
     CGROUP_PARENT=/sys/fs/cgroup/tinydocker.slice
```

The cgroup parent must already exist and be correctly delegated. tinydocker refuses to create an unknown host parent hierarchy.

## Observability suite

The observability suite starts one short-lived detached container and checks `inspect`, `stats`, lazy `EXITED` refresh and `ps -a`.

From an explicitly chosen root shell in the disposable VM:

```bash
TINYDOCKER_ALLOW_PRIVILEGED_TESTS=1 \
PRIVILEGED_SUITE=observability \
IMAGE=/path/to/trusted-rootfs.tar.xz \
bash tests/run_privileged.sh
```

Expected high-level result:

```text
[test-observability] observability test passed
```

## Full suite

The full suite additionally exercises environment variables, read-write/read-only bind mounts, CPU/memory limits, `top`, `exec`, bridge create/list/remove, a unique host port mapping, `commit`, `stop` and idempotent `rm` behavior.

```bash
TINYDOCKER_ALLOW_PRIVILEGED_TESTS=1 \
PRIVILEGED_SUITE=full \
IMAGE=/path/to/trusted-rootfs.tar.xz \
bash tests/run_privileged.sh
```

Expected high-level result:

```text
[test-full] full function test passed
```

## What to inspect after a run

Do not delete anything during this inspection. Read-only checks include:

```bash
./tinydocker ps -a
ip link show
iptables -t nat -S
find /sys/fs/cgroup -maxdepth 2 -type d -name 'tinydocker-*' -print
```

Compare any residual name with the run ID printed by the test. A similarly named resource not created by the current run must be treated as unrelated.

## Failure handling

1. Save the complete test output.
2. Record the generated run ID and container/network names.
3. Use `./tinydocker inspect NAME`, `./tinydocker ps -a`, `ip link show` and `iptables -t nat -S` to understand the state.
4. Do not run generic cleanup snippets copied from the internet.
5. Revert the disposable VM snapshot if resource ownership is uncertain.

The safe default is to discard/revert the VM, not to guess which host cgroup, mount or firewall rule can be deleted.

## Not verified by default CI

Default GitHub Actions verifies compilation, strict warnings, non-privileged tests and ASan/UBSan only. It does not prove that the host kernel permits `clone`, `setns`, OverlayFS, `pivot_root`, cgroup controller writes, bridge/veth setup or iptables changes. Those behaviors remain manual privileged integration coverage.
