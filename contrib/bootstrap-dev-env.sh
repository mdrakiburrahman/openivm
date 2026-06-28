#!/bin/bash
#
#   Bootstraps a Linux Devbox host idempotently.
#   If your Devbox restarts, rerun this script.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

cmn_ensure_passwordless_sudo

# Capture PATH *before* stripping Windows entries: on WSL the VS Code `code`
# CLI legitimately lives under /mnt/c/.../Microsoft VS Code/bin, and we still
# want to drive it for extension installs. (az/gh, by contrast, must be the
# native Linux builds, hence the strip below.)
export CMN_ORIGINAL_PATH="$PATH"
export PATH="$(cmn_strip_windows_paths)"

REPO_ROOT="$(git -C "${SCRIPT_DIR}" rev-parse --show-toplevel)"

cmn_ensure_jq
cmn_ensure_docker
cmn_configure_docker_daemon
cmn_kill_running_containers

cmn_ensure_build_toolchain
cmn_ensure_format_tools
cmn_ensure_submodules "${REPO_ROOT}"
cmn_build_openivm "${REPO_ROOT}"
cmn_install_vscode_extensions

cmn_ensure_az_cli
cmn_ensure_az_login
cmn_ensure_gh_cli
cmn_ensure_gh_login

echo
echo "Docker:      $(docker --version)"
echo "GitHub CLI:  $(gh --version | head -1)"
echo "Azure CLI:   $(az --version | head -1)"
echo "CMake:       $(cmake --version | head -1)"
echo "Ninja:       $(ninja --version)"
echo "g++:         $(g++ --version | head -1)"
echo "gdb:         $(gdb --version | head -1)"
echo
echo "OpenIVM is built. To debug: open this folder in VS Code and press F5"
echo "(\"Debug OpenIVM Demo\"). See examples/README.md."
