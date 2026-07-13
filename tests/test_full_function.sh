#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

TD="${TD:-./tinydocker}"
IMAGE="${IMAGE:-busybox.tar.xz}"
RUN_ID="${TINYDOCKER_TEST_RUN_ID:-$$_$(date +%s)}"

if [ "${TINYDOCKER_ALLOW_PRIVILEGED_TESTS:-0}" != "1" ]; then
    printf '[test-full] REFUSED: use tests/run_privileged.sh after reviewing the risks.\n' >&2
    exit 2
fi
[ "$(uname -s)" = "Linux" ] || { printf '[test-full] Linux is required.\n' >&2; exit 2; }
[ "$(id -u)" -eq 0 ] || { printf '[test-full] explicit root execution is required; sudo is never invoked.\n' >&2; exit 2; }
case "$RUN_ID" in
    *[!A-Za-z0-9_-]*) printf '[test-full] unsafe run id.\n' >&2; exit 2 ;;
esac

SHORT_ID="${RUN_ID:0:8}"
C1="tdt_${SHORT_ID}_run"
C2="tdt_${SHORT_ID}_obs"
C3="tdt_${SHORT_ID}_port"
C4="tdt_${SHORT_ID}_commit"
NET="tdn_${SHORT_ID}"
TMP_ROOT="$(mktemp -d /tmp/tinydocker-full.XXXXXX)"
HOST_RW="$TMP_ROOT/host-rw"
HOST_RO="$TMP_ROOT/host-ro"
COMMIT_TAR="$TMP_ROOT/commit.tar"
HOST_PORT=$((30000 + ($$ % 20000)))

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
    "$TD" stop "$C1" "$C2" "$C3" "$C4" >/dev/null 2>&1
    "$TD" rm "$C1" "$C2" "$C3" "$C4" >/dev/null 2>&1
    "$TD" network rm "$NET" >/dev/null 2>&1
    rm -rf "$TMP_ROOT"
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
run "$TD" run -d \
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
"$TD" ps > "$TMP_ROOT/ps.out"
assert_file_contains "$TMP_ROOT/ps.out" "$C1" "ps should include running container"
assert_file_contains "$TMP_ROOT/ps.out" "RUNNING" "ps should show RUNNING"

log "check inspect"
"$TD" inspect "$C1" > "$TMP_ROOT/inspect.out"
assert_file_contains "$TMP_ROOT/inspect.out" "Name:[[:space:]]*$C1" "inspect should show name"
assert_file_contains "$TMP_ROOT/inspect.out" "Status:[[:space:]]*RUNNING" "inspect should show RUNNING"
assert_file_contains "$TMP_ROOT/inspect.out" "CgroupPath:" "inspect should show cgroup path"
assert_file_contains "$TMP_ROOT/inspect.out" "PortMappings:[[:space:]]*unavailable" "inspect should document unavailable port metadata"

log "check stats"
"$TD" stats "$C1" > "$TMP_ROOT/stats.out"
assert_file_contains "$TMP_ROOT/stats.out" "CPU_USEC" "stats should show CPU_USEC"
assert_file_contains "$TMP_ROOT/stats.out" "MEM_CURRENT" "stats should show MEM_CURRENT"
assert_file_contains "$TMP_ROOT/stats.out" "PIDS" "stats should show PIDS"

log "check top"
"$TD" top "$C1" > "$TMP_ROOT/top.out"
assert_file_contains "$TMP_ROOT/top.out" "sleep|sh" "top should show container process"

log "check exec"
run "$TD" exec "$C1" /bin/sh -c 'echo exec-ok > /tmp/exec-ok'

log "check network create/list/rm"
run "$TD" network create "$NET" 172.18.0.0/24
"$TD" network ls > "$TMP_ROOT/network-list.out"
assert_file_contains "$TMP_ROOT/network-list.out" "$NET" "network ls should include created network"
ip addr show "$NET" | grep '172.18.0.1/24' >/dev/null || fail "bridge should have expected cidr host ip"
brctl show | grep "$NET" >/dev/null || fail "brctl should include created bridge"
"$TD" network rm "$NET" > "$TMP_ROOT/network-rm.out" 2>&1
if grep -q "failed update network info" "$TMP_ROOT/network-rm.out"; then
    cat "$TMP_ROOT/network-rm.out" >&2
    fail "network rm should not falsely report failed update network info"
fi
if brctl show | grep "$NET" >/dev/null; then
    fail "brctl should not include removed bridge"
fi

log "check port mapping"
run "$TD" run -d -n "$C3" -p "$HOST_PORT:8080" "$IMAGE" /bin/sh -c 'sleep 120'
sleep 1
iptables -t nat -S | grep -- "--dport $HOST_PORT" >/dev/null || fail "iptables should include the unique DNAT rule"
"$TD" inspect "$C3" | grep "PortMappings: unavailable" >/dev/null || fail "inspect should show unavailable port metadata"

log "check lazy status refresh"
run "$TD" run -d -n "$C2" "$IMAGE" /bin/sh -c 'sleep 3'
"$TD" inspect "$C2" | grep "Status: RUNNING" >/dev/null || fail "short-lived container should initially be RUNNING"
sleep 5
"$TD" inspect "$C2" | grep "Status: EXITED" >/dev/null || fail "inspect should refresh EXITED"
"$TD" ps -a | grep "$C2" | grep "EXITED" >/dev/null || fail "ps -a should show refreshed EXITED"

log "check commit"
run "$TD" run -d -n "$C4" "$IMAGE" /bin/sh -c 'echo commit-ok > /commit-marker; sleep 5'
sleep 2
run "$TD" commit "$C4" "$COMMIT_TAR"
[ -f "$COMMIT_TAR" ] || fail "commit tar should exist"
tar -tf "$COMMIT_TAR" | grep "commit-marker" >/dev/null || fail "commit tar should include marker file"

log "check stop/rm"
"$TD" stop "$C1" "$C3" "$C4" >/dev/null 2>&1 || true
run "$TD" rm "$C1" "$C2" "$C3" "$C4"
if "$TD" ps -a | grep -E "$C1|$C2|$C3|$C4" >/dev/null; then
    fail "removed test containers should not appear in ps -a"
fi

log "full function test passed"
