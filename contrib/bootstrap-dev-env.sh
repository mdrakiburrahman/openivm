#!/bin/bash
#
#   Bootstraps a Linux Devbox host idempotently.
#   If your Devbox restarts, rerun this script.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

cmn_ensure_passwordless_sudo
export PATH="$(cmn_strip_windows_paths)"

cmn_ensure_jq
cmn_ensure_docker
cmn_configure_docker_daemon
cmn_kill_running_containers
cmn_ensure_az_cli
cmn_ensure_az_login
cmn_ensure_gh_cli
cmn_ensure_gh_login

echo
echo "Docker:     $(docker --version)"
echo "GitHub CLI: $(gh --version | head -1)"
echo "Azure CLI:  $(az --version | head -1)"