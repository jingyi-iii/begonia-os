#!/bin/bash
#
# fetch-packages.sh - restore the toolchain source tarballs into packages/.
#
# The tarballs are deliberately not committed: gcc-16.2.0.tar.xz alone is
# 107,200,820 bytes, over GitHub's 100 MiB per-file limit, and the whole set
# would add ~162 MB to every clone.  This script downloads the exact files
# build-toolchain.sh expects and verifies each one against its sha256.
#
# Usage:
#   bash tools/fetch-packages.sh           # fetch whatever is missing
#   bash tools/fetch-packages.sh --check   # verify only, download nothing
#
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PACKAGES_DIR="$SCRIPT_DIR/packages"

check_only=0
case "${1:-}" in
    --check)   check_only=1 ;;
    -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
    "")        ;;
    *)         echo >&2 "ERROR: unknown option: $1"; exit 1 ;;
esac

# file|sha256|mirror [mirror ...]   (same order as build-toolchain.sh builds them)
packages=(
    "gmp-6.3.0.tar.xz|a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898|https://mirrors.tuna.tsinghua.edu.cn/gnu/gmp/gmp-6.3.0.tar.xz https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz"

    "mpfr-4.2.2.tar.xz|b67ba0383ef7e8a8563734e2e889ef5ec3c3b898a01d00fa0a6869ad81c6ce01|https://mirrors.tuna.tsinghua.edu.cn/gnu/mpfr/mpfr-4.2.2.tar.xz https://ftp.gnu.org/gnu/mpfr/mpfr-4.2.2.tar.xz"

    "mpc-1.3.1.tar.gz|ab642492f5cf882b74aa0cb730cd410a81edcdbec895183ce930e706c1c759b8|https://mirrors.tuna.tsinghua.edu.cn/gnu/mpc/mpc-1.3.1.tar.gz https://ftp.gnu.org/gnu/mpc/mpc-1.3.1.tar.gz"

    "isl-0.24.tar.bz2|fcf78dd9656c10eb8cf9fbd5f59a0b6b01386205fe1934b3b287a0a1898145c0|https://gcc.gnu.org/pub/gcc/infrastructure/isl-0.24.tar.bz2 https://libisl.sourceforge.io/isl-0.24.tar.bz2"

    "zstd-1.5.7.tar.gz|eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3|https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz"

    "binutils-2.46.0.tar.xz|d75a94f4d73e7a4086f7513e67e439e8fcdcbb726ffe63f4661744e6256b2cf2|https://mirrors.tuna.tsinghua.edu.cn/gnu/binutils/binutils-2.46.0.tar.xz https://ftp.gnu.org/gnu/binutils/binutils-2.46.0.tar.xz"

    "gdb-17.2.tar.xz|1c036c0d72e4b3d1fb5c94c88632add6f9d76f4d7c4d2ea793c12a9f19a3228c|https://mirrors.tuna.tsinghua.edu.cn/gnu/gdb/gdb-17.2.tar.xz https://ftp.gnu.org/gnu/gdb/gdb-17.2.tar.xz"

    "gcc-16.2.0.tar.xz|e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e|https://mirrors.tuna.tsinghua.edu.cn/gnu/gcc/gcc-16.2.0/gcc-16.2.0.tar.xz https://ftp.gnu.org/gnu/gcc/gcc-16.2.0/gcc-16.2.0.tar.xz"
)

verify() {  # sha256 path
    printf '%s  %s\n' "$1" "$2" | sha256sum -c --status -
}

download() {  # url destination
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 --connect-timeout 20 --progress-bar -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget --tries=3 --timeout=20 -O "$2" "$1"
    else
        echo >&2 "ERROR: neither curl nor wget is available"
        exit 1
    fi
}

mkdir -p "$PACKAGES_DIR"

echo "Toolchain source packages -> $PACKAGES_DIR"
echo

missing=0
failed=0

for entry in "${packages[@]}"; do
    IFS='|' read -r file sum mirrors <<<"$entry"
    dest="$PACKAGES_DIR/$file"

    if [[ -f "$dest" ]] && verify "$sum" "$dest"; then
        printf '  ok       %s\n' "$file"
        continue
    fi

    if (( check_only )); then
        printf '  MISSING  %s\n' "$file"
        failed=$((failed + 1))
        continue
    fi

    missing=$((missing + 1))
    fetched=0
    for url in $mirrors; do
        printf '  fetch    %s\n             <- %s\n' "$file" "$url"
        if download "$url" "$dest.part" && verify "$sum" "$dest.part"; then
            mv -f "$dest.part" "$dest"
            fetched=1
            break
        fi
        printf '  warn     %s: bad download from %s\n' "$file" "$url" >&2
    done
    rm -f "$dest.part"

    if (( fetched )); then
        printf '  ok       %s\n' "$file"
    else
        printf '  FAILED   %s (no mirror worked)\n' "$file" >&2
        failed=$((failed + 1))
    fi
done

echo
if (( failed )); then
    echo >&2 "$failed of ${#packages[@]} package(s) missing or unverified."
    exit 1
fi

echo "All ${#packages[@]} packages present and verified."
if (( missing )); then
    echo "Next: bash tools/build-toolchain.sh"
fi
