# Contributing

## How to use, on a Windows machine by installing WSL

1. Windows pre-reqs

   ```powershell
   winget install -e --id Microsoft.VisualStudioCode
   ```

1. Get a fresh new WSL machine up:

   > ⚠️ Warning: this removes Docker Desktop if you have it installed

   ```powershell
   $GIT_ROOT = git rev-parse --show-toplevel
   & "$GIT_ROOT\contrib\bootstrap-dev-env.ps1"
   ```

1. Clone the repo, and open VSCode in it:

   > ⚠️ Important: We use WSL in `~/` because Linux > Windows drive commits via `/mnt/c` is extremely slow for Spark I/O.
   > You can technically run the Devcontainer using Windows Docker Desktop, but the I/O experience is slow and poor.

   ```bash
   cd ~/

   read -p "Enter your name (e.g. 'FirstName LastName'): " user_name
   read -p "Enter your GitHub email (e.g. 'your-email@blah.com'): " user_email

   git clone https://github.com/mdrakiburrahman/openivm.git

   git config --global user.name "$user_name"
   git config --global user.email "$user_email"
   cd openivm/
   git pull origin

   code .
   ```

1. (Optional but recommended) Enable passwordless sudo so `dev.sh` helpers never block on a prompt:

   ```bash
   echo "$USER ALL=(ALL) NOPASSWD:ALL" | sudo tee /etc/sudoers.d/90-$USER-nopasswd >/dev/null && sudo chmod 0440 /etc/sudoers.d/90-$USER-nopasswd
   ```

1. Install recommended developer tooling (optional):

   ```bash
   curl -fsSL https://gh.io/copilot-install | bash
   $HOME/.local/bin/copilot --yolo
   ```

1. Run the bootstrapper script, that installs all tools idempotently:

   ```bash
   GIT_ROOT=$(git rev-parse --show-toplevel)
   chmod +x ${GIT_ROOT}/contrib/bootstrap-dev-env.sh && ${GIT_ROOT}/contrib/bootstrap-dev-env.sh
   ```

   This is **zero → debug** and idempotent. It installs the C++ build toolchain
   (cmake, ninja, g++, **gdb**, ccache), the PR-parity formatters (clang-format 11,
   clang-tidy), checks out the submodules, builds **both** `release` (runs
   `make test`) and `debug` (debug symbols), and installs the pinned VS Code C++
   extensions. Re-run it any time your dev box restarts.

## Debugging OpenIVM end to end

Once `bootstrap-dev-env.sh` has run:

1. Open the repo in VS Code: `code .`
2. Press **F5** and choose **“Debug OpenIVM Demo”** (or set a breakpoint in
   `examples/openivm_demo.cpp` first).
3. **Step Into (F11)** from a SQL call (e.g. `PRAGMA refresh`) straight into
   OpenIVM's C++ — parser → rewrite rules → upsert compiler.

See [`examples/README.md`](examples/README.md) for the full guide, the per-scenario
code paths, and the highest-value breakpoints. You can also pick **“Debug a single
SQL test (unittest)”** to step through any `test/sql/*.test` file.

## Build & test manually

```bash
GEN=ninja make            # release build -> build/release/
GEN=ninja make debug      # debug build   -> build/debug/
make test                 # run the SQLLogicTest suite
make format-fix           # clang-format + cmake-format (matches CI)
```
