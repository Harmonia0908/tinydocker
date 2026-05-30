#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

TD="${TD:-./tinydocker}"
IMAGE="${IMAGE:-busybox.tar.xz}"
C1="${C1:-td_full_run}"
C2="${C2:-td_full_obs}"
C3="${C3:-td_full_port}"
C4="${C4:-td_full_commit}"
NET="${NET:-td_full_net}"
HOST_RW="${HOST_RW:-/tmp/td_host_rw}"
HOST_RO="${HOST_RO:-/tmp/td_host_ro}"
COMMIT_TAR="${COMMIT_TAR:-/tmp/td_commit.tar}"

if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    SUDO="${SUDO:-sudo}"
fi

log() {
    printf '[test-full] %s\n' "$*"
}

fail() {
    printf '[test-full] ERROR: %s\n' "$*" >&2
    exit 1
}

run() {
    log "run: $*"
    "$@"
}

cleanup() {
    set +e
    log "cleanup"
    $SUDO "$TD" stop "$C1" "$C2" "$C3" "$C4" >/dev/null 2>&1
    $SUDO "$TD" rm "$C1" "$C2" "$C3" "$C4" >/dev/null 2>&1
    $SUDO "$TD" network rm "$NET" >/dev/null 2>&1
    rm -rf "$HOST_RW" "$HOST_RO" "$COMMIT_TAR"
}
trap cleanup EXIT

assert_file_contains() {
    local file="$1"
    local pattern="$2"
    local message="$3"
    if ! grep -Eq "$pattern" "$file"; then
        printf '[test-full] assertion failed: %s\n' "$message" >&2
        printf '[test-full] expected pattern: %s\n' "$pattern" >&2
        printf '%s\n' '--- file content ---' >&2
        cat "$file" >&2 || true
        printf '%s\n' '--------------------' >&2
        exit 1
    fi
}

[ -f "$IMAGE" ] || fail "image not found: $IMAGE"

cleanup
mkdir -p "$HOST_RW" "$HOST_RO"
printf 'readonly-data\n' >"$HOST_RO/input.txt"

run make clean
run make

log "run detached container with env, volumes, and cgroup limits"
run $SUDO "$TD" run -d \
    -n "$C1" \
    -c 20000 \
    -m 134217728 \
    -e TD_ENV=ok \
    -v "$HOST_RW:/rw_dir" \
    -v "$HOST_RO:/ro_dir:ro" \
    "$IMAGE" /bin/sh -c 'echo "$TD_ENV" > /rw_dir/env.out; cat /ro_dir/input.txt > /rw_dir/ro-copy.out; sleep 120'

sleep 1
[ "$(cat "$HOST_RW/env.out")" = "ok" ] || fail "env should be written through rw volume"
[ "$(cat "$HOST_RW/ro-copy.out")" = "readonly-data" ] || fail "ro volume content should be readable"

log "check ps"
$SUDO "$TD" ps > /tmp/td_full_ps.out
assert_file_contains /tmp/td_full_ps.out "$C1" "ps should include running container"
assert_file_contains /tmp/td_full_ps.out "RUNNING" "ps should show RUNNING"

log "check inspect"
$SUDO "$TD" inspect "$C1" > /tmp/td_full_inspect.out
assert_file_contains /tmp/td_full_inspect.out "Name:[[:space:]]*$C1" "inspect should show name"
assert_file_contains /tmp/td_full_inspect.out "Status:[[:space:]]*RUNNING" "inspect should show RUNNING"
assert_file_contains /tmp/td_full_inspect.out "CgroupPath:" "inspect should show cgroup path"
assert_file_contains /tmp/td_full_inspect.out "PortMappings:[[:space:]]*unavailable" "inspect should document unavailable port metadata"

log "check stats"
$SUDO "$TD" stats "$C1" > /tmp/td_full_stats.out
assert_file_contains /tmp/td_full_stats.out "CPU_USEC" "stats should show CPU_USEC"
assert_file_contains /tmp/td_full_stats.out "MEM_CURRENT" "stats should show MEM_CURRENT"
assert_file_contains /tmp/td_full_stats.out "PIDS" "stats should show PIDS"

log "check top"
$SUDO "$TD" top "$C1" > /tmp/td_full_top.out
assert_file_contains /tmp/td_full_top.out "sleep|sh" "top should show container process"

log "check exec"
run $SUDO "$TD" exec "$C1" /bin/sh -c 'echo exec-ok > /tmp/exec-ok'

log "check network create/list/rm"
run $SUDO "$TD" network create "$NET" 172.18.0.0/24
$SUDO "$TD" network ls > /tmp/td_full_net_ls.out
assert_file_contains /tmp/td_full_net_ls.out "$NET" "network ls should include created network"
ip addr show "$NET" | grep '172.18.0.1/24' >/dev/null || fail "bridge should have expected cidr host ip"
brctl show | grep "$NET" >/dev/null || fail "brctl should include created bridge"
$SUDO "$TD" network rm "$NET" > /tmp/td_full_net_rm.out 2>&1
if grep -q "failed update network info" /tmp/td_full_net_rm.out; then
    cat /tmp/td_full_net_rm.out >&2
    fail "network rm should not falsely report failed update network info"
fi
if brctl show | grep "$NET" >/dev/null; then
    fail "brctl should not include removed bridge"
fi

log "check port mapping"
run $SUDO "$TD" run -d -n "$C3" -p 18080:8080 "$IMAGE" /bin/sh -c 'sleep 120'
sleep 1
iptables -t nat -S | grep 18080 >/dev/null || fail "iptables should include DNAT rule for host port 18080"
$SUDO "$TD" inspect "$C3" | grep "PortMappings: unavailable" >/dev/null || fail "inspect should show unavailable port metadata"

log "check lazy status refresh"
run $SUDO "$TD" run -d -n "$C2" "$IMAGE" /bin/sh -c 'sleep 3'
$SUDO "$TD" inspect "$C2" | grep "Status: RUNNING" >/dev/null || fail "short-lived container should initially be RUNNING"
sleep 5
$SUDO "$TD" inspect "$C2" | grep "Status: EXITED" >/dev/null || fail "inspect should refresh EXITED"
$SUDO "$TD" ps -a | grep "$C2" | grep "EXITED" >/dev/null || fail "ps -a should show refreshed EXITED"

log "check commit"
run $SUDO "$TD" run -d -n "$C4" "$IMAGE" /bin/sh -c 'echo commit-ok > /commit-marker; sleep 5'
sleep 2
run $SUDO "$TD" commit "$C4" "$COMMIT_TAR"
[ -f "$COMMIT_TAR" ] || fail "commit tar should exist"
tar -tf "$COMMIT_TAR" | grep "commit-marker" >/dev/null || fail "commit tar should include marker file"

log "check stop/rm"
$SUDO "$TD" stop "$C1" "$C3" "$C4" >/dev/null 2>&1 || true
run $SUDO "$TD" rm "$C1" "$C2" "$C3" "$C4"
if $SUDO "$TD" ps -a | grep -E "$C1|$C2|$C3|$C4" >/dev/null; then
    fail "removed test containers should not appear in ps -a"
fi

log "full function test passed"
