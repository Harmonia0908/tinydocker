#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

profile="${1:-default}"
if [ "$#" -gt 0 ]; then
    shift
fi

case "$profile" in
    default) exec make "$@" ;;
    debug) exec make debug "$@" ;;
    release) exec make release "$@" ;;
    *)
        printf 'usage: %s [default|debug|release] [make arguments...]\n' "$0" >&2
        exit 2
        ;;
esac
