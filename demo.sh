#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

TD="${TD:-./tinydocker}"
IMAGE="${IMAGE:-busybox.tar.xz}"
RUNTIME_DIR="${RUNTIME_DIR:-/home/xanarry/tinydocker_runtime}"
CGROUP_PARENT="${CGROUP_PARENT:-/sys/fs/cgroup/system.slice}"

if [ "${TINYDOCKER_ALLOW_PRIVILEGED_DEMO:-0}" != "1" ]; then
    cat >&2 <<'WARNING'
[demo] REFUSED: this demo creates namespaces, mounts, cgroups, veth devices,
and iptables rules. Review demo.sh and run only in a disposable Linux VM:
  TINYDOCKER_ALLOW_PRIVILEGED_DEMO=1 bash demo.sh
The script requires an already-root shell and never invokes sudo.
WARNING
    exit 2
fi
[ "$(uname -s)" = "Linux" ] || { printf '[demo] Linux is required.\n' >&2; exit 2; }
[ "$(id -u)" -eq 0 ] || { printf '[demo] run explicitly as root; sudo is never invoked.\n' >&2; exit 2; }
TMP_DIR="$(mktemp -d /tmp/tinydocker-demo.XXXXXX)"
UNIQUE_ID="${TMP_DIR##*.}"
NAME="td_demo_${UNIQUE_ID}"
INFO_DIR="$RUNTIME_DIR/container_info"
CONTAINER_DIR="$RUNTIME_DIR/containers/$NAME"
CGROUP_DIR="$CGROUP_PARENT/tinydocker-$NAME"
CONTAINER_OWNED=0

step_no=0

step() {
    step_no=$((step_no + 1))
    printf '\n========== [%02d] %s ==========\n' "$step_no" "$*"
}

info() {
    printf '[demo] %s\n' "$*"
}

fail() {
    printf '\n[demo] ERROR: %s\n' "$1" >&2
    printf '[demo] HINT: %s\n' "${2:-请先修复环境后再重新运行 demo.sh。}" >&2
    exit 1
}

run() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    "$@"
}

run_capture() {
    local out_file="$1"
    shift
    printf '+'
    printf ' %q' "$@"
    printf ' > %q\n' "$out_file"
    if ! "$@" >"$out_file" 2>&1; then
        printf '[demo] command failed, output follows:\n' >&2
        sed 's/^/[demo-output] /' "$out_file" >&2 || true
        return 1
    fi
    sed 's/^/[demo-output] /' "$out_file"
}

cleanup_demo_artifacts() {
    local original_status=$?
    local cleanup_failed=0
    set +e
    if [ "${CONTAINER_OWNED:-0}" = "1" ]; then
        "$TD" stop -t 1 "$NAME" >/dev/null 2>&1 || true
        if ! "$TD" rm "$NAME" >/dev/null 2>&1; then
            printf '[demo] cleanup failed for owned container: %s\n' "$NAME" >&2
            cleanup_failed=1
        fi
    fi
    case "$TMP_DIR" in
        /tmp/tinydocker-demo.??????) rm -rf -- "$TMP_DIR" ;;
        *) printf '[demo] refusing unexpected temp path: %s\n' "$TMP_DIR" >&2; cleanup_failed=1 ;;
    esac
    if [ "$cleanup_failed" -ne 0 ] && [ "$original_status" -eq 0 ]; then
        original_status=1
    fi
    trap - EXIT
    exit "$original_status"
}
trap cleanup_demo_artifacts EXIT

check_command() {
    local cmd="$1"
    local hint="$2"
    command -v "$cmd" >/dev/null 2>&1 || fail "缺少命令: $cmd" "$hint"
}

preflight() {
    step "Preflight: check host environment"

    info "root 权限: yes (explicitly provided; sudo is not used)"

    check_command make "请安装 make，例如 Ubuntu: sudo apt-get install make"
    check_command gcc "请安装 gcc，例如 Ubuntu: sudo apt-get install build-essential"
    check_command tar "请安装 tar。"
    check_command ip "请安装 iproute2，例如 Ubuntu: sudo apt-get install iproute2"
    check_command iptables "请安装 iptables，例如 Ubuntu: sudo apt-get install iptables"
    check_command brctl "请安装 bridge-utils，例如 Ubuntu: sudo apt-get install bridge-utils"

    if [ ! -f /sys/fs/cgroup/cgroup.controllers ]; then
        fail "未检测到 cgroup v2: /sys/fs/cgroup/cgroup.controllers 不存在。" "请在启用 cgroup v2 的 Linux 主机或虚拟机中运行。"
    fi
    info "cgroup v2: detected"

    if [ ! -e "$IMAGE" ]; then
        fail "镜像/rootfs 不存在: $IMAGE" "默认使用 busybox.tar.xz；也可以指定 IMAGE=/path/to/alpine-rootfs.tar.xz bash demo.sh"
    fi
    info "image/rootfs: $IMAGE"

}

verify_resource_ownership() {
    step "Ownership: verify generated resources are absent"
    if [ -e "$INFO_DIR/$NAME" ] || [ -e "$CONTAINER_DIR" ] ||
       [ -e "$CGROUP_DIR" ] || "$TD" inspect "$NAME" >/dev/null 2>&1; then
        fail "generated demo name already exists: $NAME" \
             "do not remove it automatically; rerun to generate a different name"
    fi
    info "demo container name: $NAME"
}

build_binary() {
    step "Build: compile tinydocker"
    run make
    [ -x "$TD" ] || fail "编译后未找到可执行文件: $TD" "请检查 make 输出和 libcrypto/OpenSSL 开发包是否安装。"
}

run_container() {
    step "Run: start a busybox/alpine container"
    CONTAINER_OWNED=1
    run_capture "$TMP_DIR/run.out" "$TD" run -d \
        -n "$NAME" \
        -c 20000 \
        -m 134217728 \
        "$IMAGE" /bin/sh -c 'while true; do sleep 1; done'
    sleep 1
}

inspect_container() {
    step "Inspect: show container metadata"
    run_capture "$TMP_DIR/inspect.out" "$TD" inspect "$NAME"
}

stats_container() {
    step "Stats: show cgroup v2 resource metrics"
    run_capture "$TMP_DIR/stats.out" "$TD" stats "$NAME"
}

stop_and_remove_container() {
    step "Stop: terminate the container"
    run_capture "$TMP_DIR/stop.out" "$TD" stop -t 1 "$NAME"

    step "Remove: remove container runtime artifacts"
    run_capture "$TMP_DIR/rm.out" "$TD" rm "$NAME"
}

verify_cleanup() {
    step "Cleanup: verify and remove residual metadata"
    local dirty=0

    if [ -e "$INFO_DIR/$NAME" ]; then
        info "found residual metadata: $INFO_DIR/$NAME"
        dirty=1
    fi
    if [ -d "$CGROUP_DIR" ]; then
        info "found residual cgroup: $CGROUP_DIR"
        dirty=1
    fi
    if [ -d "$CONTAINER_DIR" ]; then
        info "found residual workspace: $CONTAINER_DIR"
        dirty=1
    fi

    if [ "$dirty" -eq 0 ]; then
        info "no demo metadata/cgroup/workspace leftovers detected"
        CONTAINER_OWNED=0
    else
        fail "demo cleanup left known artifacts; refusing direct rm/umount fallback" \
             "inspect the listed paths in the disposable VM before manual cleanup"
    fi
}

main() {
    preflight
    build_binary
    verify_resource_ownership
    run_container
    inspect_container
    stats_container
    stop_and_remove_container
    verify_cleanup

    step "Done"
    info "demo finished without leaving demo container data"
}

main "$@"
