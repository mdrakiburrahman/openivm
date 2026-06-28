#!/bin/bash

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    echo "[common.sh] FATAL: source this file, do not execute it" >&2
    exit 1
fi

# ── pinned versions ─────────────────────────────────────────────────────────

CMN_DOCKER_VERSION="${CMN_DOCKER_VERSION:-5:27.5.1-1~ubuntu.24.04~noble}"
CMN_DOCKER_DAEMON_JSON='{"max-concurrent-downloads": 32}'

# ── tiny utilities ──────────────────────────────────────────────────────────

# cmn_log <prefix> <msg...> — pretty `[<prefix>] msg`.
cmn_log() {
    local prefix="$1"; shift
    echo "[$prefix] $*"
}

# cmn_has <cmd> — true iff $cmd is on PATH and executable.
cmn_has() {
    command -v "$1" >/dev/null 2>&1
}

# cmn_apt_install <pkg...> — quiet idempotent apt install.
cmn_apt_install() {
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "$@" >/dev/null
}

# ── jq ──────────────────────────────────────────────────────────────────────

cmn_ensure_jq() {
    if cmn_has jq; then
        cmn_log common "jq already installed ($(jq --version))"
        return 0
    fi
    cmn_log common "installing jq..."
    sudo apt-get update -qq
    cmn_apt_install jq
    cmn_log common "jq installed ($(jq --version))"
}

# ── docker ──────────────────────────────────────────────────────────────────

# cmn_ensure_docker — install Docker CE if missing. No-op when `docker` is on
# PATH (GitHub-hosted ubuntu-latest pre-installs it).
cmn_ensure_docker() {
    if cmn_has docker; then
        cmn_log common "docker already installed ($(docker --version))"
        return 0
    fi
    cmn_log common "installing docker..."
    sudo apt-get update -qq
    cmn_apt_install apt-transport-https ca-certificates curl gnupg lsb-release
    curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo apt-key add -
    sudo add-apt-repository -y "deb [arch=amd64] https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable"
    sudo apt-get update -qq
    sudo apt-get install -y --allow-downgrades \
        docker-ce="${CMN_DOCKER_VERSION}" \
        docker-ce-cli="${CMN_DOCKER_VERSION}" \
        containerd.io >/dev/null
    cmn_log common "docker installed ($(docker --version))"
}

# cmn_configure_docker_daemon — write /etc/docker/daemon.json with our
# desired settings. Only restarts dockerd when the file actually changes
# (avoids needless service bounces on GH runners). Also ensures the docker
# socket is world-rw so non-root callers can talk to it (matches the WSL dev
# setup; on GH runners the runner user is already in the `docker` group, so
# the chmod is harmless either way).
cmn_configure_docker_daemon() {
    local target="/etc/docker/daemon.json"
    local desired="${CMN_DOCKER_DAEMON_JSON}"
    sudo mkdir -p /etc/docker
    local current=""
    if [[ -f "$target" ]]; then
        current="$(sudo cat "$target" 2>/dev/null || echo '')"
    fi
    if [[ "$current" == "$desired" ]]; then
        cmn_log common "docker daemon.json already up to date"
    else
        cmn_log common "writing docker daemon.json..."
        echo "$desired" | sudo tee "$target" >/dev/null
        cmn_log common "restarting docker..."
        # systemctl works on the dev box; on GH runners docker is also a
        # systemd service. Fall back to `service` if systemctl is missing.
        if cmn_has systemctl; then
            sudo systemctl restart docker
        else
            sudo service docker restart
        fi
    fi
    # World-rw on the socket: matches existing dev-box behaviour, harmless
    # on GH runners (the runner user is already in the docker group).
    if [[ -S /var/run/docker.sock ]]; then
        sudo chmod 666 /var/run/docker.sock || true
    fi
}

# cmn_kill_running_containers — best-effort `docker kill` on every running
# container. Useful before a dev verify (PRE_CLEAN=1). NOT used on CI
# (runners are ephemeral).
cmn_kill_running_containers() {
    if ! cmn_has docker; then return 0; fi
    docker container ls >/dev/null 2>&1 || return 0
    local ids
    ids="$(docker ps -q || true)"
    if [[ -z "$ids" ]]; then
        cmn_log common "no running containers to kill"
        return 0
    fi
    cmn_log common "killing $(echo "$ids" | wc -l | tr -d ' ') running container(s)..."
    echo "$ids" | xargs -r docker kill >/dev/null
}

# ── WSL-only helpers ────────────────────────────────────────────────────────

# cmn_strip_windows_paths — remove `/mnt/c/...` entries from PATH so we don't
# accidentally pick up Windows-side az/gh on a WSL host. Caller must
# `export PATH=$(cmn_strip_windows_paths)` since shell functions can't modify
# the parent env.
cmn_strip_windows_paths() {
    echo "$PATH" | tr ':' '\n' | grep -v "^/mnt/c" | tr '\n' ':' | sed 's/:$//'
}

# ── azure cli ───────────────────────────────────────────────────────────────

cmn_ensure_az_cli() {
    local current
    current="$(command -v az 2>/dev/null || echo '')"
    if [[ -n "$current" && "$current" != *"/mnt/c"* ]]; then
        cmn_log common "az already installed at $current"
        return 0
    fi
    cmn_log common "installing native Linux az cli..."
    curl -sL https://aka.ms/InstallAzureCLIDeb | sudo bash >/dev/null
    export PATH="$HOME/bin:$PATH"
    cmn_log common "az installed ($(az --version | head -1))"
}

cmn_ensure_az_login() {
    if az account get-access-token --query expiresOn -o tsv >/dev/null 2>&1; then
        cmn_log common "az already logged in"
        return 0
    fi
    cmn_log common "az not logged in — running 'az login'..."
    az login >/dev/null
}

# ── github cli ──────────────────────────────────────────────────────────────

cmn_ensure_gh_cli() {
    if cmn_has gh; then
        cmn_log common "gh already installed ($(gh --version | head -1))"
        return 0
    fi
    cmn_log common "installing gh cli..."
    cmn_has wget || cmn_apt_install wget
    sudo mkdir -p -m 755 /etc/apt/keyrings
    local key
    key="$(mktemp)"
    wget -nv -O"$key" https://cli.github.com/packages/githubcli-archive-keyring.gpg
    sudo install -m 0644 "$key" /etc/apt/keyrings/githubcli-archive-keyring.gpg
    rm -f "$key"
    sudo mkdir -p -m 755 /etc/apt/sources.list.d
    echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" \
        | sudo tee /etc/apt/sources.list.d/github-cli.list >/dev/null
    sudo apt-get update -qq
    cmn_apt_install gh
    cmn_log common "gh installed ($(gh --version | head -1))"
}

cmn_ensure_gh_login() {
    if gh auth status >/dev/null 2>&1; then
        cmn_log common "gh already logged in as $(gh api user --jq .login 2>/dev/null || echo unknown)"
        return 0
    fi
    cmn_log common "gh not logged in — running 'gh auth login'..."
    gh auth login
}

# ── passwordless sudo (dev box only) ────────────────────────────────────────

# cmn_ensure_passwordless_sudo — install /etc/sudoers.d/90-<user>-nopasswd so
# subsequent `sudo` calls never prompt. First invocation may prompt for the
# password once; idempotent after that.
cmn_ensure_passwordless_sudo() {
    local user="${SUDO_USER:-$USER}"
    if [[ "$user" == "root" ]]; then
        cmn_log common "running as root — passwordless sudo not applicable"
        return 0
    fi
    if sudo -n true 2>/dev/null; then
        cmn_log common "passwordless sudo already active for '$user'"
        return 0
    fi
    cmn_log common "configuring passwordless sudo for '$user' (may prompt once)..."
    local target="/etc/sudoers.d/90-${user}-nopasswd"
    local tmp
    tmp="$(mktemp)"
    printf '%s ALL=(ALL) NOPASSWD:ALL\n' "$user" > "$tmp"
    if ! sudo visudo -cf "$tmp" >/dev/null; then
        cmn_log common "FATAL: refused to install malformed sudoers fragment at $target" >&2
        rm -f "$tmp"
        return 1
    fi
    sudo install -m 0440 -o root -g root "$tmp" "$target"
    rm -f "$tmp"
    if ! sudo -n true 2>/dev/null; then
        cmn_log common "FATAL: passwordless sudo still not active after installing $target" >&2
        return 1
    fi
    cmn_log common "passwordless sudo configured at $target"
}

# ── openivm: c++ build toolchain ────────────────────────────────────────────

# Pinned VS Code extensions for C++ debugging (id@version). Pins keep a blown-up
# dev box reproducible — bump deliberately.
CMN_VSCODE_EXTENSIONS=(
    "ms-vscode.cpptools@1.32.2"
    "ms-vscode.cpptools-extension-pack@1.5.1"
    "ms-vscode.makefile-tools@0.12.17"
)

# cmn_ensure_local_bin_on_path — ensure ~/.local/bin (pip --user scripts such as
# clang-format) is on PATH for this shell and future login shells.
cmn_ensure_local_bin_on_path() {
    local localbin="$HOME/.local/bin"
    case ":$PATH:" in
        *":$localbin:"*) ;;
        *) export PATH="$localbin:$PATH" ;;
    esac
    if [[ -f "$HOME/.bashrc" ]] && ! grep -q '.local/bin' "$HOME/.bashrc" 2>/dev/null; then
        echo 'export PATH="$HOME/.local/bin:$PATH"' >> "$HOME/.bashrc"
    fi
}

# cmn_ensure_build_toolchain — install everything needed to COMPILE the OpenIVM
# DuckDB extension (same toolchain the PR build uses): a C++ compiler, CMake,
# Ninja, gdb (debugging), ccache (faster rebuilds), git, and pip/venv.
cmn_ensure_build_toolchain() {
    local missing=()
    cmn_has cmake      || missing+=(cmake)
    cmn_has ninja      || missing+=(ninja-build)
    cmn_has g++        || missing+=(build-essential)
    cmn_has gdb        || missing+=(gdb)
    cmn_has ccache     || missing+=(ccache)
    cmn_has git        || missing+=(git)
    cmn_has pkg-config || missing+=(pkg-config)
    python3 -m pip --version >/dev/null 2>&1 || missing+=(python3-pip python3-venv)
    if [[ ${#missing[@]} -eq 0 ]]; then
        cmn_log openivm "build toolchain already installed"
        return 0
    fi
    cmn_log openivm "installing build toolchain: ${missing[*]}"
    sudo apt-get update -qq
    cmn_apt_install "${missing[@]}"
    cmn_log openivm "build toolchain ready ($(cmake --version | head -1), $(g++ --version | head -1))"
}

# cmn_ensure_format_tools — install the exact formatter/linter the PR "code
# quality" job uses: clang-format 11.0.1 via the pip wheel (Ubuntu 24.04 has no
# clang-format-11 apt package), plus cmake-format/black and clang-tidy. Enables
# `make format-fix` and clang-tidy parity. Best-effort: never fails bootstrap.
cmn_ensure_format_tools() {
    cmn_ensure_local_bin_on_path
    if command -v clang-format >/dev/null 2>&1 && clang-format --version | grep -q ' 11\.'; then
        cmn_log openivm "format tools already installed (clang-format 11)"
    else
        cmn_log openivm "installing format tools (clang-format 11.0.1, cmake-format, black)..."
        python3 -m pip install --user --break-system-packages \
            'clang_format==11.0.1' cmake-format 'black==24.*' cxxheaderparser pcpp 'pybind11[global]' >/dev/null \
            || cmn_log openivm "  (pip format-tools install hit an issue; continuing)"
    fi
    cmn_has clang-tidy || { cmn_log openivm "installing clang-tidy..."; cmn_apt_install clang-tidy || true; }
    cmn_log openivm "format tools ready"
}

# ── openivm: submodules + build ─────────────────────────────────────────────

# cmn_ensure_submodules <repo_root> — check out the MINIMAL submodule set the
# build needs: duckdb (v1.5.2), extension-ci-tools, and lpts (+ its ducklake
# headers). Avoids lpts's heavy nested duckdb/sqlstorm clones. Idempotent.
cmn_ensure_submodules() {
    local root="$1"
    cmn_log openivm "initializing submodules (duckdb, extension-ci-tools, lpts)..."
    git -C "$root" submodule update --init duckdb extension-ci-tools third_party/lpts
    git -C "$root/third_party/lpts" submodule update --init third_party/ducklake
    cmn_log openivm "submodules ready"
}

# cmn_build_openivm <repo_root> — build BOTH configurations from a clean dev box:
#   release -> CI parity, runs `make test`         build/release/...
#   debug   -> symbols for line-by-line debugging  build/debug/...
# Uses Ninja + ccache. Idempotent: make no-ops when nothing changed. The first
# run compiles DuckDB from source and is slow (minutes); reruns are fast.
cmn_build_openivm() {
    local root="$1"
    export PATH="/usr/lib/ccache:$PATH"
    cmn_log openivm "building RELEASE (compiles DuckDB from source; first run is slow)..."
    ( cd "$root" && GEN=ninja make )
    cmn_log openivm "building DEBUG (symbols for the VS Code debugger)..."
    ( cd "$root" && GEN=ninja make debug )
    cmn_log openivm "build complete"
    cmn_log openivm "  release demo: build/release/extension/openivm/openivm_demo"
    cmn_log openivm "  debug demo:   build/debug/extension/openivm/openivm_demo"
}

# ── openivm: vs code extensions ─────────────────────────────────────────────

# cmn_install_vscode_extensions — install pinned C++ debugging extensions into
# VS Code. No-op when the `code` CLI is unavailable (headless / CI runners).
# On WSL, `code` lives under a /mnt/c Windows path that bootstrap strips from
# PATH, so fall back to CMN_ORIGINAL_PATH (captured before the strip) to find it.
cmn_install_vscode_extensions() {
    local code_bin=""
    if cmn_has code; then
        code_bin="code"
    elif [[ -n "${CMN_ORIGINAL_PATH:-}" ]] && PATH="$CMN_ORIGINAL_PATH" command -v code >/dev/null 2>&1; then
        code_bin="$(PATH="$CMN_ORIGINAL_PATH" command -v code)"
    fi
    if [[ -z "$code_bin" ]]; then
        cmn_log openivm "VS Code 'code' CLI not found — skipping extension install"
        return 0
    fi
    local ext
    for ext in "${CMN_VSCODE_EXTENSIONS[@]}"; do
        cmn_log openivm "installing VS Code extension ${ext}..."
        "$code_bin" --install-extension "${ext}" --force >/dev/null 2>&1 \
            || cmn_log openivm "  (could not install ${ext}; continuing)"
    done
    cmn_log openivm "VS Code extensions ready"
}
