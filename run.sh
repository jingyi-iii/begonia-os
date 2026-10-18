#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TOOLS_DIR="$ROOT_DIR/tools"
TOOLCHAIN_DIR="$TOOLS_DIR/local"

force_tools=0
skip_deps=0
target=""
targets=()

usage() {
    cat <<EOF
Usage: $(basename -- "$0") [options] [make-target...]

Ask which toolchain to use (32-bit by default), make sure it exists (building
it on first use), then build and run the image in startup via
qemu.  Any make targets are passed through to the top-level Makefile
(default: run).

Options:
  --32             Use/build the 32-bit (i686-elf) toolchain, no prompt
  --64             Use/build the 64-bit (x86_64-elf) toolchain, no prompt
  --target TRIPLE  Explicit target triple (e.g. i686-elf, x86_64-elf)
  --force-tools    Rebuild the toolchain even if it is already present
  --skip-deps      Do not install system packages via apt when building tools
  -h, --help       Show this help

Examples:
  $(basename -- "$0")          # build the image and launch qemu
  $(basename -- "$0") all      # build only
  $(basename -- "$0") clean    # clean the image build
  $(basename -- "$0") --64     # use the 64-bit (long mode) toolchain
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --32)           target="i686-elf" ;;
        --64)           target="x86_64-elf" ;;
        --target)
            if [[ $# -lt 2 ]]; then
                echo >&2 "ERROR: $1 requires an argument"
                exit 1
            fi
            target="$2"
            shift
            ;;
        --target=*)     target="${1#*=}" ;;
        --force-tools)  force_tools=1 ;;
        --skip-deps)    skip_deps=1 ;;
        -h|--help)      usage; exit 0 ;;
        --) shift; targets+=("$@"); break ;;
        -*) echo >&2 "ERROR: Unknown option: $1"; usage >&2; exit 1 ;;
        *) targets+=("$1") ;;
    esac
    shift
done

if [[ ${#targets[@]} -eq 0 ]]; then
    targets=(run)
fi

normalize_target() {
    case "$1" in
        32|i686|i686-elf)     echo "i686-elf" ;;
        64|x86_64|x86_64-elf) echo "x86_64-elf" ;;
        *)                    echo "$1" ;;
    esac
}

select_target() {
    if [[ -n "$target" ]]; then
        target="$(normalize_target "$target")"
        return
    fi

    if [[ ! -t 0 ]]; then
        target="i686-elf"
        return
    fi

    local reply
    read -r -p "Which toolchain? 32-bit (i686-elf) or 64-bit (x86_64-elf)? [32/64] (default 32): " reply || true
    case "$reply" in
        ""|32|i686|i686-elf)  target="i686-elf" ;;
        64|x86_64|x86_64-elf) target="x86_64-elf" ;;
        *) echo >&2 "ERROR: invalid choice: $reply (expected 32 or 64)"; exit 1 ;;
    esac
}

needs_toolchain() {
    local t
    for t in "${targets[@]}"; do
        case "$t" in
            clean|distclean) ;;
            *) return 0 ;;
        esac
    done
    return 1
}

bootstrap_toolchain() {
    local gcc="$TOOLCHAIN_DIR/bin/$target-gcc"

    export TARGET="$target"

    if [[ -x "$gcc" ]] && (( ! force_tools )); then
        echo "Using existing $target toolchain at $TOOLCHAIN_DIR"
        return
    fi

    if [[ ! -f "$TOOLS_DIR/build-toolchain.sh" ]]; then
        echo >&2 "ERROR: Missing $TOOLS_DIR/build-toolchain.sh"
        exit 1
    fi

    local args=(--target "$target")
    if (( force_tools )); then
        echo "Rebuilding the $target toolchain..."
        args+=(--force)
    else
        echo "Toolchain for $target not found, building it first..."
    fi
    if (( skip_deps )); then
        args+=(--skip-deps)
    fi

    bash "$TOOLS_DIR/build-toolchain.sh" "${args[@]}"
}

main() {
    if needs_toolchain; then
        select_target
        bootstrap_toolchain
    fi

    # shellcheck source=tools/env.sh
    source "$TOOLS_DIR/env.sh"

    if [[ ! -f "$ROOT_DIR/Makefile" && ! -f "$ROOT_DIR/makefile" && ! -f "$ROOT_DIR/GNUmakefile" ]]; then
        echo >&2 "ERROR: No Makefile found in $ROOT_DIR"
        exit 1
    fi

    make -C "$ROOT_DIR" "${targets[@]}"
}

main "$@"
