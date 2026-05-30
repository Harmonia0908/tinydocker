#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

TD="${TD:-./tinydocker}"
IMAGE="${IMAGE:-busybox.tar.xz}"
NAME="${NAME:-td_demo_$$}"
RUNTIME_DIR="${RUNTIME_DIR:-/home/xanarry/tinydocker_runtime}"
INFO_DIR="$RUNTIME_DIR/container_info"
CONTAINER_DIR="$RUNTIME_DIR/containers/$NAME"
CGROUP_DIR="/sys/fs/cgroup/system.slice/tinydocker-$NAME"
TMP_DIR="$(mktemp -d)"
CLEANUP_ENABLED=0

if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    SUDO="${SUDO:-sudo}"
fi

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
    set +e
    if [ "${CLEANUP_ENABLED:-0}" != "1" ]; then
        rm -rf "$TMP_DIR"
        set -e
        return
    fi
    if [ -n "${SUDO:-}" ]; then
        $SUDO "$TD" stop -t 1 "$NAME" >/dev/null 2>&1
        $SUDO "$TD" rm "$NAME" >/dev/null 2>&1
        $SUDO rm -f "$INFO_DIR/$NAME" >/dev/null 2>&1
        $SUDO rmdir "$CGROUP_DIR" >/dev/null 2>&1
        $SUDO rm -rf "$CONTAINER_DIR" >/dev/null 2>&1
    else
        "$TD" stop -t 1 "$NAME" >/dev/null 2>&1
        "$TD" rm "$NAME" >/dev/null 2>&1
        rm -f "$INFO_DIR/$NAME" >/dev/null 2>&1
        rmdir "$CGROUP_DIR" >/dev/null 2>&1
        rm -rf "$CONTAINER_DIR" >/dev/null 2>&1
    fi
    rm -rf "$TMP_DIR"
    set -e
}
trap cleanup_demo_artifacts EXIT

check_command() {
    local cmd="$1"
    local hint="$2"
    command -v "$cmd" >/dev/null 2>&1 || fail "缺少命令: $cmd" "$hint"
}

preflight() {
    step "Preflight: check host environment"

    if [ "$(id -u)" -ne 0 ] && ! command -v sudo >/dev/null 2>&1; then
        fail "当前不是 root，且系统缺少 sudo。" "请使用 root 运行，或安装 sudo 后运行: sudo bash demo.sh"
    fi
    info "root 权限: $([ "$(id -u)" -eq 0 ] && printf 'yes' || printf 'will use sudo')"

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

    if ! $SUDO true >/dev/null 2>&1; then
        fail "无法获取 root 权限。" "请确认当前用户有 sudo 权限，或直接使用 root 运行。"
    fi
    CLEANUP_ENABLED=1
}

pre_cleanup() {
    step "Cleanup: remove stale demo leftovers before start"
    cleanup_demo_artifacts
    mkdir -p "$TMP_DIR"
    info "demo container name: $NAME"
}

build_binary() {
    step "Build: compile tinydocker"
    run make
    [ -x "$TD" ] || fail "编译后未找到可执行文件: $TD" "请检查 make 输出和 libcrypto/OpenSSL 开发包是否安装。"
}

run_container() {
    step "Run: start a busybox/alpine container"
    run_capture "$TMP_DIR/run.out" $SUDO "$TD" run -d \
        -n "$NAME" \
        -c 20000 \
        -m 134217728 \
        "$IMAGE" /bin/sh -c 'while true; do sleep 1; done'
    sleep 1
}

inspect_container() {
    step "Inspect: show container metadata"
    run_capture "$TMP_DIR/inspect.out" $SUDO "$TD" inspect "$NAME"
}

stats_container() {
    step "Stats: show cgroup v2 resource metrics"
    run_capture "$TMP_DIR/stats.out" $SUDO "$TD" stats "$NAME"
}

stop_and_remove_container() {
    step "Stop: terminate the container"
    run_capture "$TMP_DIR/stop.out" $SUDO "$TD" stop -t 1 "$NAME" || true

    step "Remove: remove container runtime artifacts"
    run_capture "$TMP_DIR/rm.out" $SUDO "$TD" rm "$NAME" || true
}

verify_cleanup() {
    step "Cleanup: verify and remove residual metadata"
    local dirty=0

    if [ -e "$INFO_DIR/$NAME" ]; then
        info "found residual metadata: $INFO_DIR/$NAME"
        run $SUDO rm -f "$INFO_DIR/$NAME"
        dirty=1
    fi
    if [ -d "$CGROUP_DIR" ]; then
        info "found residual cgroup: $CGROUP_DIR"
        run $SUDO rmdir "$CGROUP_DIR" || true
        dirty=1
    fi
    if [ -d "$CONTAINER_DIR" ]; then
        info "found residual workspace: $CONTAINER_DIR"
        run $SUDO rm -rf "$CONTAINER_DIR"
        dirty=1
    fi

    if [ "$dirty" -eq 0 ]; then
        info "no demo metadata/cgroup/workspace leftovers detected"
    fi
}

main() {
    preflight
    pre_cleanup
    build_binary
    run_container
    inspect_container
    stats_container
    stop_and_remove_container
    verify_cleanup

    step "Done"
    info "demo finished without leaving demo container data"
}

main "$@"
