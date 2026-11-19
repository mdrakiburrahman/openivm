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

# The duckdb gitlink is the deployed extension ABI. The tag gives the DuckDB build
# its version string, which a shallow submodule clone would otherwise lack.
DUCKDB_REF=v1.5.2

PACKAGES=(build-essential cmake ninja-build git ca-certificates python3 python3-pip libssl-dev pkg-config zip unzip curl gh)
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

if ! git -C duckdb rev-parse -q --verify "refs/tags/$DUCKDB_REF" >/dev/null; then
    git -C duckdb fetch -q --depth 1 origin tag "$DUCKDB_REF"
fi
if [ "$(git -C duckdb rev-parse HEAD)" != "$(git -C duckdb rev-parse "$DUCKDB_REF^{commit}")" ]; then
    echo "WARNING: duckdb is at $(git -C duckdb rev-parse HEAD), not $DUCKDB_REF; the build will not match the deployed ABI." >&2
fi

echo "Done"
echo "openivm: $(git rev-parse --abbrev-ref HEAD) $(git rev-parse HEAD)"
git submodule status -- duckdb extension-ci-tools third_party/lpts
echo "duckdb: $(git -C duckdb describe --tags)"
echo "cmake: $(cmake --version | head -1)"
echo "ninja: $(ninja --version)"
echo "Build: GEN=ninja make    Test: make test"
