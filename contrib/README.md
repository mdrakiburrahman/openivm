# Windows/WSL development bootstrap

Windows only bootstraps the environment; build and test OpenIVM inside WSL.
From the Windows checkout, use elevated PowerShell 7 with WSL installed and install [go-task](https://taskfile.dev/installation/):

```powershell
winget install -e --id Task.Task
```

The current branch and commit must be available from `origin` for a full bootstrap.

**Warning:** The default task unregisters and recreates `Ubuntu-24.04` and permanently deletes all data in that distro.
Back up anything you want to keep before running it. Other WSL distros are left alone.

```powershell
# Full: recreate the distro, clone the checkout, and install build tools and pinned submodules.
task --taskfile contrib/bootstrap-dev-env.yaml

# Up: keep the existing distro and checkout, and install missing build tools and pinned submodules.
task --taskfile contrib/bootstrap-dev-env.yaml up
```
