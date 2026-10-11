#!/bin/bash
# Source this file to put the project-local cross toolchain(s) on PATH.

TOOLS_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export OS_TOOLS="${OS_TOOLS:-$TOOLS_DIR/local}"
export I686_TOOLS="${I686_TOOLS:-$OS_TOOLS}"
export PATH="$OS_TOOLS/bin:$PATH"
