# Windows/WSL development bootstrap

Run from the Windows checkout in elevated PowerShell 7 with WSL and [go-task](https://taskfile.dev/installation/) installed.
The current branch and commit must be available from `origin` for a full bootstrap.

**Warning:** The default task unregisters and recreates `Ubuntu-24.04` and permanently deletes all data in that distro.
Back up anything you want to keep before running it. Other WSL distros are left alone.

```powershell
# Full: recreate the distro, clone the checkout, and install build tools and pinned submodules.
task --taskfile contrib/bootstrap-dev-env.yaml

# Up: keep the existing distro and checkout, and install missing build tools and pinned submodules.
task --taskfile contrib/bootstrap-dev-env.yaml up
```
