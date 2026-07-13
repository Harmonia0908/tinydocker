#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

fail() {
    printf '[privileged-test] ERROR: %s\n' "$*" >&2
    exit 1
}

if [ "${TINYDOCKER_ALLOW_PRIVILEGED_TESTS:-0}" != "1" ]; then
    cat >&2 <<'WARNING'
[privileged-test] REFUSED: privileged tests are opt-in.
They create Linux namespaces, OverlayFS/bind mounts, cgroup v2 entries,
veth/bridge devices and iptables rules. Run only in a disposable Linux VM.

After reviewing the scripts, run as root with exactly one suite:
  TINYDOCKER_ALLOW_PRIVILEGED_TESTS=1 PRIVILEGED_SUITE=observability bash tests/run_privileged.sh
  TINYDOCKER_ALLOW_PRIVILEGED_TESTS=1 PRIVILEGED_SUITE=full bash tests/run_privileged.sh
WARNING
    exit 2
fi

[ "$(uname -s)" = "Linux" ] || fail "Linux is required"
[ "$(id -u)" -eq 0 ] || fail "run explicitly as root; this script never invokes sudo"
[ -f /sys/fs/cgroup/cgroup.controllers ] || fail "cgroup v2 is required"
[ -x "${TD:-./tinydocker}" ] || fail "build ./tinydocker first"

RUN_ID="${TINYDOCKER_TEST_RUN_ID:-$$_$(date +%s)}"
case "$RUN_ID" in
    *[!A-Za-z0-9_-]*) fail "unsafe test run id: $RUN_ID" ;;
esac
export TINYDOCKER_TEST_RUN_ID="$RUN_ID"

case "${PRIVILEGED_SUITE:-}" in
    observability) exec bash tests/test_observability.sh ;;
    full) exec bash tests/test_full_function.sh ;;
    *) fail "set PRIVILEGED_SUITE=observability or PRIVILEGED_SUITE=full" ;;
esac
