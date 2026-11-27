#!/usr/bin/env bash
#
#       Installs the native build toolchain and initializes the submodules the
#       extension build needs, at their committed pins.
#
#       Idempotent: rerun after a restart. Initialized submodules, the checked-out
#       branch and local edits are never reset.
#
# ---------------------------------------------------------------------------------------
#

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"
export DEBIAN_FRONTEND=noninteractive

# Follow this checkout's ABI, not the separately published benchmark package.
DUCKDB_REF=$(git rev-parse HEAD:duckdb)

PACKAGES=(build-essential cmake ninja-build git ca-certificates python3 python3-pip python3-venv libssl-dev pkg-config zip unzip curl gh)
MISSING=()
for package in "${PACKAGES[@]}"; do
    dpkg-query -W -f='${Status}' "$package" 2>/dev/null | grep -q 'install ok installed' || MISSING+=("$package")
done
if ((${#MISSING[@]})); then
    echo "Installing ${MISSING[*]}"
    sudo apt-get update -qq
    sudo apt-get install -yqq -o Dpkg::Use-Pty=0 --no-install-recommends "${MISSING[@]}"
fi

init_missing() {
    local repo=$1
    shift
    for path in "$@"; do
        if git -C "$repo" submodule status -- "$path" | grep -q '^-'; then
            echo "Initializing $repo/$path"
            git -C "$repo" submodule update --init --depth 1 -- "$path"
        fi
    done
}

# benchbase, the LPTS duckdb/SQLStorm checkouts and the Claude skills are not part of the extension build.
init_missing . duckdb extension-ci-tools third_party/lpts
init_missing third_party/lpts third_party/ducklake

if [ "$(git -C duckdb rev-parse HEAD)" != "$DUCKDB_REF" ]; then
    echo "WARNING: duckdb is at $(git -C duckdb rev-parse HEAD), not committed gitlink $DUCKDB_REF; preserving the existing checkout." >&2
fi

echo "Done"
echo "openivm: $(git rev-parse --abbrev-ref HEAD) $(git rev-parse HEAD)"
git submodule status -- duckdb extension-ci-tools third_party/lpts
echo "duckdb: $(git -C duckdb describe --tags --always)"
echo "cmake: $(cmake --version | head -1)"
echo "ninja: $(ninja --version)"
echo "Build: GEN=ninja make    Test: make test"
