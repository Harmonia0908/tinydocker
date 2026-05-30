# tinydocker Full Function Test Guide

This document records a reproducible end-to-end test flow for this `xanarry/tinydocker` secondary-development branch. The project depends on Linux namespace, cgroup v2, overlayfs, iptables, bridge networking, and root privileges. Run these tests on a Linux VM, not on macOS.

Recommended environment:

- Debian 12 or Ubuntu 22.04
- root or passwordless sudo
- cgroup v2 mounted at `/sys/fs/cgroup`
- a rootfs tarball matching the VM CPU architecture

Important: the repository's checked-in `busybox.tar.xz` may not match every VM architecture. On an ARM64 VM, an x86_64 BusyBox image exits with `Exec format error`. If that happens, create an ARM64-compatible rootfs first and use it as `IMAGE`.

## 1. Prepare Environment

Install required packages:

```bash
sudo apt-get update
sudo apt-get install -y build-essential libssl-dev bridge-utils iproute2 iptables tar curl
```

Check cgroup v2 and overlayfs:

```bash
stat -fc %T /sys/fs/cgroup
grep overlay /proc/filesystems
```

Expected:

- `/sys/fs/cgroup` reports `cgroup2fs`
- `overlay` appears in `/proc/filesystems`

The project uses hardcoded runtime paths under `/home/xanarry/tinydocker_runtime`. Create the parent directory if needed:

```bash
sudo mkdir -p /home/xanarry
```

Build:

```bash
make clean && make
```

Expected:

```text
gcc -Wall logger/*.c util/*.c cmdparser/*.c docker/*.c main.c -lcrypto -o tinydocker
```

If the checked-in `busybox.tar.xz` does not match the VM architecture, create a local BusyBox rootfs from the VM's native BusyBox binary:

```bash
rm -rf /root/arm64-rootfs
mkdir -p /root/arm64-rootfs/bin \
         /root/arm64-rootfs/lib \
         /root/arm64-rootfs/lib/aarch64-linux-gnu \
         /root/arm64-rootfs/proc \
         /root/arm64-rootfs/tmp \
         /root/arm64-rootfs/etc

cp /usr/bin/busybox /root/arm64-rootfs/bin/busybox
for app in sh sleep cat echo ls ps true false; do
    ln -sf busybox "/root/arm64-rootfs/bin/$app"
done

cp /lib/ld-linux-aarch64.so.1 /root/arm64-rootfs/lib/
cp /lib/aarch64-linux-gnu/libc.so.6 /root/arm64-rootfs/lib/aarch64-linux-gnu/
cp /lib/aarch64-linux-gnu/libresolv.so.2 /root/arm64-rootfs/lib/aarch64-linux-gnu/

tar -C /root/arm64-rootfs -cJf busybox-arm64.tar.xz .
```

Then set:

```bash
export IMAGE=busybox-arm64.tar.xz
```

## 2. Clean Previous Test State

Use unique names for this test run:

```bash
export TD=./tinydocker
export IMAGE=${IMAGE:-busybox.tar.xz}
export C1=td_full_run
export C2=td_full_obs
export C3=td_full_port
export C4=td_full_commit
export NET=td_full_net
export HOST_RW=/tmp/td_host_rw
export HOST_RO=/tmp/td_host_ro
export COMMIT_TAR=/tmp/td_commit.tar
```

Cleanup before starting:

```bash
sudo "$TD" stop "$C1" "$C2" "$C3" "$C4" >/dev/null 2>&1 || true
sudo "$TD" rm "$C1" "$C2" "$C3" "$C4" >/dev/null 2>&1 || true
sudo "$TD" network rm "$NET" >/dev/null 2>&1 || true
sudo rm -rf "$HOST_RW" "$HOST_RO" "$COMMIT_TAR"
sudo mkdir -p "$HOST_RW" "$HOST_RO"
echo readonly-data | sudo tee "$HOST_RO/input.txt" >/dev/null
```

## 3. Run, Volume, Env, cgroup Limit

Start a detached container with cgroup limits, env, and two volume mounts:

```bash
sudo "$TD" run -d \
  -n "$C1" \
  -c 20000 \
  -m 134217728 \
  -e TD_ENV=ok \
  -v "$HOST_RW":/rw_dir \
  -v "$HOST_RO":/ro_dir:ro \
  "$IMAGE" /bin/sh -c 'echo "$TD_ENV" > /rw_dir/env.out; cat /ro_dir/input.txt > /rw_dir/ro-copy.out; sleep 120'
```

Verify volume and env behavior:

```bash
sudo test "$(cat "$HOST_RW/env.out")" = "ok"
sudo test "$(cat "$HOST_RW/ro-copy.out")" = "readonly-data"
```

Verify cgroup files exist:

```bash
sudo test -f "/sys/fs/cgroup/system.slice/tinydocker-$C1/memory.max"
sudo test -f "/sys/fs/cgroup/system.slice/tinydocker-$C1/cpu.max"
```

## 4. ps, inspect, stats, top, exec

List running containers:

```bash
sudo "$TD" ps
```

Expected: output contains `$C1` and `RUNNING`.

Inspect metadata:

```bash
sudo "$TD" inspect "$C1"
```

Expected fields:

- `Name: td_full_run`
- positive `PID`
- `Status: RUNNING`
- `Image: busybox.tar.xz`
- `Command: /bin/sh -c ...`
- `IP: ...`
- `CgroupPath: /sys/fs/cgroup/system.slice/tinydocker-td_full_run`
- volume entries
- `PortMappings: unavailable`

Read cgroup v2 runtime metrics:

```bash
sudo "$TD" stats "$C1"
```

Expected: table with `NAME`, `CPU_USEC`, `MEM_CURRENT`, `MEM_MAX`, `PIDS`, and `CPU_MAX`.

Check process listing:

```bash
sudo "$TD" top "$C1"
```

Expected: at least the container shell or sleep process is listed.

Run a command inside the container:

```bash
sudo "$TD" exec "$C1" /bin/sh -c 'echo exec-ok > /tmp/exec-ok'
```

Expected: command exits successfully. If this fails, inspect namespace and cgroup join logs.

## 5. Network create/list/rm

Create an extra bridge network:

```bash
sudo "$TD" network create "$NET" 172.18.0.0/24
sudo "$TD" network ls
ip addr show "$NET"
brctl show
```

Expected:

- `network ls` contains `$NET`
- `ip addr show "$NET"` contains `172.18.0.1/24`
- `brctl show` contains `$NET`

Remove it:

```bash
sudo "$TD" network rm "$NET"
sudo "$TD" network ls
brctl show
```

Expected:

- `network ls` no longer contains `$NET`
- `brctl show` no longer contains `$NET`
- no false `failed update network info` error is printed during removal

## 6. Port Mapping

Start a container with a port mapping. The project currently does not persist port mappings into `container_info`, so `inspect` reports `PortMappings: unavailable`; verify the iptables rule instead.

```bash
sudo "$TD" run -d -n "$C3" -p 18080:8080 "$IMAGE" /bin/sh -c 'sleep 120'
sudo iptables -t nat -S | grep "18080" | grep "$C3" || sudo iptables -t nat -S | grep "18080"
sudo "$TD" inspect "$C3" | grep "PortMappings: unavailable"
```

Expected:

- an iptables DNAT rule for host port `18080`
- `inspect` explicitly prints `PortMappings: unavailable`

## 7. Lazy Status Refresh

Start a short-lived background container:

```bash
sudo "$TD" run -d -n "$C2" "$IMAGE" /bin/sh -c 'sleep 3'
sudo "$TD" inspect "$C2" | grep "Status: RUNNING"
sleep 5
sudo "$TD" inspect "$C2" | grep "Status: EXITED"
sudo "$TD" ps -a | grep "$C2" | grep "EXITED"
```

Expected: after the process exits, `inspect` and `ps -a` lazily refresh stale `RUNNING` metadata to `EXITED`.

## 8. commit

Create a container that writes a marker file, then commit its workspace:

```bash
sudo "$TD" run -d -n "$C4" "$IMAGE" /bin/sh -c 'echo commit-ok > /commit-marker; sleep 5'
sleep 2
sudo "$TD" commit "$C4" "$COMMIT_TAR"
sudo test -f "$COMMIT_TAR"
tar -tf "$COMMIT_TAR" | grep "commit-marker"
```

Expected: `commit-marker` exists in the generated tar archive.

## 9. stop and rm

Stop and remove all test containers:

```bash
sudo "$TD" stop "$C1" "$C2" "$C3" "$C4" || true
sudo "$TD" rm "$C1" "$C2" "$C3" "$C4"
sudo "$TD" ps -a
```

Expected:

- stopped containers are removed from metadata/runtime directories
- `ps -a` does not list removed test containers

## 10. Observability Regression Script

Run the checked-in focused regression test:

```bash
sudo bash tests/test_observability.sh
```

Expected:

```text
[test-observability] observability test passed
```

## 11. Automated Full Function Script

The full manual flow above is also captured as an executable regression script:

```bash
IMAGE=busybox-arm64.tar.xz bash tests/test_full_function.sh
```

Expected final line:

```text
[test-full] full function test passed
```

## 12. Verified Run Record

Verified on:

```text
Debian 12 ARM64
Linux bad 6.1.0-45-arm64
cgroup2fs mounted at /sys/fs/cgroup
root user
```

Commands executed:

```bash
make clean && make
IMAGE=busybox-arm64.tar.xz bash tests/test_observability.sh
IMAGE=busybox-arm64.tar.xz bash tests/test_full_function.sh
```

Observed results:

```text
gcc -Wall logger/*.c util/*.c cmdparser/*.c docker/*.c main.c -lcrypto -o tinydocker
[test-observability] observability test passed
[test-full] full function test passed
```

One issue was found during the full test and fixed: `docker rm` crashed when removing a container with multiple volume mounts because `clean_container_runtime()` allocated only one `struct volume_config` but wrote `info.volume_cnt` entries. The fix allocates `info.volume_cnt` entries and handles allocation failure.

## 13. Final Cleanup

```bash
sudo "$TD" stop "$C1" "$C2" "$C3" "$C4" >/dev/null 2>&1 || true
sudo "$TD" rm "$C1" "$C2" "$C3" "$C4" >/dev/null 2>&1 || true
sudo "$TD" network rm "$NET" >/dev/null 2>&1 || true
sudo rm -rf "$HOST_RW" "$HOST_RO" "$COMMIT_TAR"
```

## 14. Known Boundaries

- This is a teaching-oriented container runtime, not production Docker/containerd/runc.
- `inspect` does not currently reconstruct port mappings from metadata; it prints `PortMappings: unavailable`.
- `stats` is a point-in-time cgroup v2 file-system snapshot, not a continuous monitoring system.
- Lazy status refresh checks `/proc/<pid>` and is suitable for this no-daemon design, but it is not a complete solution for pid reuse.
- `docker_run` failure cleanup is best-effort, not a strict transaction across cgroup, network, mount, and metadata subsystems.
