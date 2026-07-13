#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

TD="${TD:-./tinydocker}"
IMAGE="${IMAGE:-busybox.tar.xz}"

if [ "${TINYDOCKER_ALLOW_PRIVILEGED_TESTS:-0}" != "1" ]; then
    printf '[test-observability] REFUSED: use tests/run_privileged.sh after reviewing the risks.\n' >&2
    exit 2
fi
[ "$(uname -s)" = "Linux" ] || { printf '[test-observability] Linux is required.\n' >&2; exit 2; }
[ "$(id -u)" -eq 0 ] || { printf '[test-observability] explicit root execution is required; sudo is never invoked.\n' >&2; exit 2; }
[ "${TINYDOCKER_TEST_LAUNCHED:-0}" = "1" ] || { printf '[test-observability] use tests/run_privileged.sh.\n' >&2; exit 2; }
TMP_DIR="$(mktemp -d /tmp/tinydocker-observability.XXXXXX)"
UNIQUE_ID="${TMP_DIR##*.}"
NAME="tdobs_${UNIQUE_ID}"
CONTAINER_OWNED=0

log() {
    printf '[test-observability] %s\n' "$*"
}

fail() {
    printf '[test-observability] ERROR: %s\n' "$*" >&2
    exit 1
}

run_capture() {
    local out_file="$1"
    shift
    log "run: $*"
    if ! "$@" >"$out_file" 2>&1; then
        printf '[test-observability] command failed: %s\n' "$*" >&2
        printf '%s\n' '--- command output ---' >&2
        cat "$out_file" >&2 || true
        printf '%s\n' '----------------------' >&2
        return 1
    fi
}

assert_contains() {
    local file="$1"
    local pattern="$2"
    local message="$3"
    if ! grep -Eq "$pattern" "$file"; then
        printf '[test-observability] assertion failed: %s\n' "$message" >&2
        printf '[test-observability] expected pattern: %s\n' "$pattern" >&2
        printf '%s\n' '--- file content ---' >&2
        cat "$file" >&2 || true
        printf '%s\n' '--------------------' >&2
        exit 1
    fi
}

cleanup() {
    local original_status=$?
    local cleanup_failed=0
    set +e
    log "cleanup container and temp files"
    if [ "$CONTAINER_OWNED" = "1" ]; then
        "$TD" stop -t 1 "$NAME" >/dev/null 2>&1 || true
        if ! "$TD" rm "$NAME" >/dev/null 2>&1; then
            printf '[test-observability] cleanup failed for owned container: %s\n' "$NAME" >&2
            cleanup_failed=1
        fi
    fi
    case "$TMP_DIR" in
        /tmp/tinydocker-observability.??????) rm -rf -- "$TMP_DIR" ;;
        *) printf '[test-observability] refusing unexpected temp path: %s\n' "$TMP_DIR" >&2; cleanup_failed=1 ;;
    esac
    if [ "$cleanup_failed" -ne 0 ] && [ "$original_status" -eq 0 ]; then
        original_status=1
    fi
    trap - EXIT
    exit "$original_status"
}
trap cleanup EXIT

[ -f "$IMAGE" ] || fail "image not found: $IMAGE"

log "compile project"
make >"$TMP_DIR/make.out" 2>&1 || {
    printf '%s\n' '--- make output ---' >&2
    cat "$TMP_DIR/make.out" >&2
    printf '%s\n' '-------------------' >&2
    fail "make failed"
}

if "$TD" inspect "$NAME" >/dev/null 2>&1; then
    fail "generated container name already exists: $NAME"
fi

log "start detached short-lived container: $NAME"
CONTAINER_OWNED=1
run_capture "$TMP_DIR/run.out" "$TD" run -d -n "$NAME" "$IMAGE" /bin/sh -c "sleep 5"

log "inspect running container"
run_capture "$TMP_DIR/inspect-running.out" "$TD" inspect "$NAME"
assert_contains "$TMP_DIR/inspect-running.out" "Name:[[:space:]]*$NAME" "inspect should show container name"
assert_contains "$TMP_DIR/inspect-running.out" "PID:[[:space:]]*[1-9][0-9]*" "inspect should show a positive pid"
assert_contains "$TMP_DIR/inspect-running.out" "Status:[[:space:]]*RUNNING" "inspect should show RUNNING before the command exits"

log "read stats"
run_capture "$TMP_DIR/stats.out" "$TD" stats "$NAME"
assert_contains "$TMP_DIR/stats.out" "MEM_CURRENT|memory\\.current|PIDS|pids" "stats should include memory or pids metrics"

log "wait for container process to exit and lazy refresh to mark EXITED"
sleep 7
run_capture "$TMP_DIR/inspect-exited.out" "$TD" inspect "$NAME"
assert_contains "$TMP_DIR/inspect-exited.out" "Status:[[:space:]]*EXITED" "inspect should lazily refresh status to EXITED"

log "verify ps -a also shows EXITED"
run_capture "$TMP_DIR/ps-a.out" "$TD" ps -a
assert_contains "$TMP_DIR/ps-a.out" "$NAME" "ps -a should include the test container"
assert_contains "$TMP_DIR/ps-a.out" "EXITED" "ps -a should show EXITED after lazy refresh"

"$TD" rm "$NAME" >/dev/null
if "$TD" inspect "$NAME" >/dev/null 2>&1; then
    fail "removed container still has metadata: $NAME"
fi
CONTAINER_OWNED=0
log "observability test passed"
