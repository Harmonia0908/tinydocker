#!/usr/bin/env bash
set -Eeuo pipefail

TD="${TD:-./tinydocker}"
TMP_DIR="$(mktemp -d /tmp/tinydocker-cli-baseline.XXXXXX)"

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

fail() {
    printf '[test-cli] FAIL: %s\n' "$*" >&2
    exit 1
}

check_failure() {
    local case_name="$1"
    local expected="$2"
    shift 2
    local stdout_file="$TMP_DIR/${case_name}.stdout"
    local stderr_file="$TMP_DIR/${case_name}.stderr"
    local expected_file="$TMP_DIR/${case_name}.expected"
    local status

    printf '%s' "$expected" >"$expected_file"
    set +e
    "$@" >"$stdout_file" 2>"$stderr_file"
    status=$?
    set -e

    [ "$status" -eq 255 ] || {
        cat "$stdout_file" >&2
        cat "$stderr_file" >&2
        fail "$case_name exited with $status; expected 255"
    }
    cmp -s "$expected_file" "$stdout_file" || {
        cat "$stdout_file" >&2
        fail "$case_name stdout did not match the behavior baseline"
    }
    [ ! -s "$stderr_file" ] || {
        cat "$stderr_file" >&2
        fail "$case_name unexpectedly wrote to stderr"
    }
}

[ -x "$TD" ] || fail "tinydocker binary is required at $TD"

check_failure no-arguments $'请输入有效的docker命令\n' "$TD"
check_failure unknown-command $'wrong input command\n' "$TD" unknown
check_failure invalid-name \
    $'invalid container name: name contains invalid byte 0x2f; allowed: [A-Za-z0-9._-]\n' \
    "$TD" inspect ../host
check_failure invalid-cidr \
    $'invalid network CIDR: CIDR must have address/prefix form\n' \
    "$TD" network create baseline0 not-a-cidr
check_failure conflicting-run-options \
    $'ERROR: -d can not use with -t -i together\nUsage:  tinydocker run [OPTIONS] IMAGE [COMMAND] [ARG...]\nOPTIONS:\n  -v, --volume  设置卷\n  -n, --name  容器名字\n  -d, --detach  容器后台运行\n  -i, --interactive  开启交互模式\n  -c, --cpu-shares  设置cpu限制, 必须大于1000\n  -m, --memory  设置内存限制\n  -e, --env  环境变量\n  -p, --port  设置端口映射\n\n' \
    "$TD" run -d -i busybox.tar.xz /bin/true
check_failure unsupported-network-connect $'wrong input command\n' \
    "$TD" network connect baseline0 baseline

printf '[test-cli] all no-side-effect CLI behavior tests passed\n'
