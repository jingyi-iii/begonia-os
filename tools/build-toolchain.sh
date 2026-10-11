#!/bin/bash
set -euo pipefail

######################################################################
########################### + USER SETTINGS + ########################
######################################################################
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

PREFIX="$SCRIPT_DIR/local"
BUILD_ROOT="$SCRIPT_DIR/build"
JOBS="$(nproc 2>/dev/null || echo 4)"
TARGET="${TARGET:-i686-elf}"

seed_packages="build-essential m4"

required_libs=(lib/libgmp.a lib/libmpfr.a lib/libmpc.a lib/libisl.a lib/libzstd.a)

skip_deps=0
with_os_deps=0
force=0
clean=0
only=""

usage() {
    cat <<EOF
Usage: $(basename -- "$0") [options]

Build the $TARGET cross toolchain (binutils, gdb, gcc) from the bundled
sources, including the compiler libraries gmp, mpfr, mpc, isl and zstd.
Everything is installed inside this directory:

    $PREFIX

Multiple targets can coexist in the same prefix (their tool names and
lib/gcc/<triple> directories do not collide); the host libraries are shared.

Options:
  --target TRIPLE  Cross target triple (default: $TARGET)
  --skip-deps      Do not install host packages via apt
  --with-os-deps   Also install OS runtime packages (qemu, grub, xorriso)
  --force          Rebuild even if a toolchain already exists
  --clean          Delete the whole prefix and exit
  --only LIST      Build only the given comma-separated components
                   (gmp,mpfr,mpc,isl,zstd,binutils,gdb,gcc)
  -j, --jobs N     Parallel build jobs (default: $JOBS)
  -h, --help       Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target)
            if [[ $# -lt 2 ]]; then
                echo >&2 "ERROR: $1 requires an argument"
                exit 1
            fi
            TARGET="$2"
            shift
            ;;
        --target=*)     TARGET="${1#*=}" ;;
        --skip-deps)    skip_deps=1 ;;
        --with-os-deps) with_os_deps=1 ;;
        --force)        force=1 ;;
        --clean)        clean=1 ;;
        --only)
            if [[ $# -lt 2 ]]; then
                echo >&2 "ERROR: $1 requires an argument"
                exit 1
            fi
            only="$2"
            shift
            ;;
        -j|--jobs)
            if [[ $# -lt 2 ]]; then
                echo >&2 "ERROR: $1 requires an argument"
                exit 1
            fi
            JOBS="$2"
            shift
            ;;
        -h|--help) usage; exit 0 ;;
        *) echo >&2 "ERROR: Unknown option: $1"; usage >&2; exit 1 ;;
    esac
    shift
done

case "$TARGET" in
    x86_64-*) os_packages="xorriso grub-pc-bin qemu-system-x86_64" ;;
    *)        os_packages="xorriso grub-pc-bin qemu-system-i386" ;;
esac

required_bins=("$TARGET-ld" "$TARGET-as" "$TARGET-ar" "$TARGET-gcc" "$TARGET-g++" "$TARGET-gdb")

tarballs=("gmp" "mpfr" "mpc" "isl" "zstd" "binutils" "gdb" "gcc")

declare -A binutils=(
    [fpath]="$SCRIPT_DIR/packages/binutils-*.tar.xz"
    [confg]="configure --target=$TARGET --prefix=$PREFIX --with-sysroot --disable-nls --disable-werror"
    [build]="make -j$JOBS && make install"
)

declare -A gdb=(
    [fpath]="$SCRIPT_DIR/packages/gdb-*.tar.xz"
    [confg]="configure --target=$TARGET --prefix=$PREFIX --disable-werror --with-gmp=$PREFIX --with-mpfr=$PREFIX"
    [build]="make -j$JOBS all-gdb && make install-gdb"
)

declare -A gmp=(
    [fpath]="$SCRIPT_DIR/packages/gmp-*.tar.*"
    [confg]="configure --prefix=$PREFIX --disable-shared --enable-static"
    [build]="make -j$JOBS && make install"
)

declare -A mpfr=(
    [fpath]="$SCRIPT_DIR/packages/mpfr-*.tar.*"
    [confg]="configure --prefix=$PREFIX --disable-shared --enable-static --with-gmp=$PREFIX"
    [build]="make -j$JOBS && make install"
)

declare -A mpc=(
    [fpath]="$SCRIPT_DIR/packages/mpc-*.tar.*"
    [confg]="configure --prefix=$PREFIX --disable-shared --enable-static --with-gmp=$PREFIX --with-mpfr=$PREFIX"
    [build]="make -j$JOBS && make install"
)

declare -A isl=(
    [fpath]="$SCRIPT_DIR/packages/isl-*.tar.*"
    [confg]="configure --prefix=$PREFIX --disable-shared --enable-static --with-gmp-prefix=$PREFIX"
    [build]="make -j$JOBS && make install"
)

declare -A zstd=(
    [fpath]="$SCRIPT_DIR/packages/zstd-*.tar.*"
    [confg]=""
    [build]="make -C lib -j$JOBS install-static install-includes PREFIX=$PREFIX"
)

declare -A gcc=(
    [fpath]="$SCRIPT_DIR/packages/gcc-*.tar.xz"
    [confg]="configure --target=$TARGET --prefix=$PREFIX --disable-nls --enable-languages=c,c++ --without-headers --with-gmp=$PREFIX --with-mpfr=$PREFIX --with-mpc=$PREFIX --with-isl=$PREFIX --with-zstd=$PREFIX"
    [build]="make -j$JOBS all-gcc && make -j$JOBS all-target-libgcc && make install-gcc && make install-target-libgcc"
)
######################################################################
########################### - USER SETTINGS - ########################
######################################################################

if [[ -n "$only" ]]; then
    IFS=',' read -r -a only_list <<< "$only"
    selected=()
    for tarball in "${tarballs[@]}"; do
        for wanted in "${only_list[@]}"; do
            if [[ "$tarball" == "$wanted" ]]; then
                selected+=("$tarball")
            fi
        done
    done
    for wanted in "${only_list[@]}"; do
        known=0
        for tarball in "${tarballs[@]}"; do
            [[ "$tarball" == "$wanted" ]] && known=1
        done
        if (( ! known )); then
            echo >&2 "ERROR: Unknown component for --only: $wanted"
            exit 1
        fi
    done
    tarballs=("${selected[@]}")
fi

check_bash_version() {
    if (( BASH_VERSINFO[0] < 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] < 4) )); then
        echo >&2 "ERROR: Bash 4.4 or higher is required. Current version: $BASH_VERSION"
        exit 1
    fi
}

install_prebuilts() {
    if (( skip_deps )); then
        echo "Skipping host dependency installation (--skip-deps)."
        return
    fi

    local packages="$seed_packages"
    if (( with_os_deps )); then
        packages="$packages $os_packages"
    fi

    echo "Installing host packages: $packages"
    if ! sudo apt update; then
        echo >&2 "ERROR: Failed to update package lists"
        exit 1
    fi

    if ! sudo apt install -y $packages; then
        echo >&2 "ERROR: Failed to install dependencies"
        exit 1
    fi
}

toolchain_ready() {
    local bin lib
    for bin in "${required_bins[@]}"; do
        if [[ ! -x "$PREFIX/bin/$bin" ]]; then
            return 1
        fi
    done
    for lib in "${required_libs[@]}"; do
        if [[ ! -f "$PREFIX/$lib" ]]; then
            return 1
        fi
    done
    return 0
}

resolve_fpath() {
    local pattern="$1"
    local matches=()

    shopt -s nullglob
    matches=($pattern)
    shopt -u nullglob

    if (( ${#matches[@]} == 0 )); then
        return 1
    fi
    printf '%s\n' "${matches[0]}"
}

install_tarballs() {
    local tarball src_dir build_dir fpath
    for tarball in "${tarballs[@]}"; do
        declare -n curr_tb="$tarball"
        src_dir="$BUILD_ROOT/src-$tarball"
        build_dir="$BUILD_ROOT/build-$tarball"

        echo "=========================================================="
        echo "Building $tarball..."
        echo "=========================================================="

        if ! fpath="$(resolve_fpath "${curr_tb[fpath]}")"; then
            echo >&2 "ERROR: No tarball matching: ${curr_tb[fpath]}"
            exit 1
        fi

        rm -rf "$src_dir" "$build_dir"
        mkdir -p "$src_dir" "$build_dir"

        echo "Extracting $(basename -- "$fpath")..."
        if ! tar xf "$fpath" -C "$src_dir" --strip-components=1; then
            echo >&2 "ERROR: Failed to extract $fpath"
            exit 1
        fi

        if [[ -n "${curr_tb[confg]:-}" ]]; then
            echo "Configuring..."
            if ! (cd "$build_dir" && "$src_dir"/${curr_tb[confg]}); then
                echo >&2 "ERROR: Configuration failed for $tarball"
                exit 1
            fi

            echo "Building and installing..."
            if ! (cd "$build_dir" && bash -c "${curr_tb[build]}"); then
                echo >&2 "ERROR: Build failed for $tarball"
                exit 1
            fi
        else
            echo "Building and installing (in-tree)..."
            if ! (cd "$src_dir" && bash -c "${curr_tb[build]}"); then
                echo >&2 "ERROR: Build failed for $tarball"
                exit 1
            fi
        fi

        rm -rf "$src_dir" "$build_dir"
        unset -n curr_tb

        echo "$tarball successfully installed!"
        echo
    done
}

main() {
    check_bash_version

    if (( clean )); then
        echo "Removing toolchain prefix $PREFIX ..."
        rm -rf "$PREFIX" "$BUILD_ROOT"
        echo "Done."
        exit 0
    fi

    if [[ -z "$only" ]] && (( ! force )) && toolchain_ready; then
        echo "Toolchain ($TARGET) already present at $PREFIX"
        echo "To use it in this shell: source \"$SCRIPT_DIR/env.sh\""
        exit 0
    fi

    install_prebuilts
    mkdir -p "$BUILD_ROOT"
    install_tarballs
    rm -rf "$BUILD_ROOT"

    echo "=========================================================="
    echo "Toolchain ($TARGET) installed at $PREFIX"
    echo "To use it in this shell:"
    echo "    source \"$SCRIPT_DIR/env.sh\""
    echo "=========================================================="
}

main "$@"
