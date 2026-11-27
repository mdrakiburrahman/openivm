<#

.SYNOPSIS
  WARNING: Permanently deletes the selected WSL distro and all data in it.
  Recreates it and clones this checkout's branch and commit into it.

.NOTES

  - Non-interactive. Fails before touching WSL if the commit cannot be cloned without a prompt.
  - Only the target distro is recreated; other distros are left alone.
  - Must be run as Administrator in PowerShell 7+.

#>

#Requires -RunAsAdministrator
#Requires -Version 7

param(
    [string]$Distro = 'Ubuntu-24.04',
    [string]$RepoPath = '/workspaces/openivm',
    [string]$LinuxUser = ($env:USERNAME.ToLower() -replace '[^a-z0-9_-]', '')
)

$ErrorActionPreference = 'Stop'
$env:WSL_UTF8 = '1'
$env:GCM_INTERACTIVE = 'never'
$env:GIT_TERMINAL_PROMPT = '0'

function Assert-Exit([string]$Failure) {
    if ($LASTEXITCODE) { throw "$Failure (exit code $LASTEXITCODE)." }
}

$root = git -C $PSScriptRoot rev-parse --show-toplevel
Assert-Exit "$PSScriptRoot is not in a git checkout"
$commit = git -C $root rev-parse HEAD
$branch = git -C $root symbolic-ref --quiet --short HEAD
if (-not $branch) { throw "$root is on a detached HEAD; check out a branch first." }
$origin = git -C $root remote get-url origin
Assert-Exit "$root has no origin remote"

$tip = ((git ls-remote --exit-code $origin "refs/heads/$branch") -split '\s+')[0]
Assert-Exit "Cannot read $branch from $origin without a prompt; push it first"
if ($tip -ne $commit) {
    git -C $root fetch --quiet origin $branch
    Assert-Exit "Cannot fetch $branch from $origin"
    git -C $root merge-base --is-ancestor $commit $tip
    Assert-Exit "HEAD $commit is not on origin/$branch ($tip); push it first"
}
Write-Host "Cloning $origin $branch at $commit into ${Distro}:$RepoPath"

# VS Code windows attached to WSL go stale when it is recreated, and VS Code would focus them instead of reconnecting.
try {
    Add-Type -AssemblyName UIAutomationClient
    [Windows.Automation.AutomationElement]::RootElement.FindAll('Children', [Windows.Automation.Condition]::TrueCondition) |
        Where-Object { $_.Current.Name -match "\[WSL: $([regex]::Escape($Distro))\].*Visual Studio Code" } |
        ForEach-Object { Write-Host "Closing VS Code window: $($_.Current.Name)"; $_.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close() }
} catch {
    Write-Host "Could not close stale VS Code windows: $_" -ForegroundColor DarkYellow
}

$wslConfig = Join-Path $env:USERPROFILE '.wslconfig'
if (-not (Test-Path $wslConfig)) {
    $memGB = [math]::Floor((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
    @"
[wsl2]
memory=${memGB}GB
processors=$([Environment]::ProcessorCount)
swap=$([math]::Floor($memGB / 4))GB
networkingMode=NAT

[general]
instanceIdleTimeout=-1
"@ | Set-Content -Path $wslConfig
    wsl --shutdown
}

Write-Host "Updating WSL (the --location flag requires WSL 2.4.4+)"
wsl --update
$wslVersion = [regex]::Match(((wsl --version) -join "`n"), '(\d+\.\d+\.\d+)')
if (-not $wslVersion.Success -or [version]$wslVersion.Value -lt [version]'2.4.4') {
    throw "WSL 2.4.4+ is required for 'wsl --install --location'; run 'wsl --update' and retry."
}

if (@(wsl -l -q | ForEach-Object { $_.Trim() }) -contains $Distro) {
    Write-Host "Unregistering WSL distro: $Distro"
    wsl --unregister $Distro
    Assert-Exit "Could not unregister $Distro"
}

$drive = Get-CimInstance Win32_LogicalDisk -Filter 'DriveType=3' |
    Where-Object { $_.VolumeName -notmatch 'Temporary' } |
    Sort-Object -Property FreeSpace -Descending |
    Select-Object -First 1
$location = "$($drive.DeviceID)\VHD\$Distro"
New-Item -ItemType Directory -Path $location -Force | Out-Null

Write-Host "Installing $Distro to $location"
wsl --install -d $Distro --location $location --no-launch --web-download
Assert-Exit 'wsl --install failed'

Write-Host "Creating default Linux user $LinuxUser with passwordless sudo"
wsl -d $Distro -u root --exec bash -c 'useradd -m -s /bin/bash -G sudo "$1" && echo "$1 ALL=(ALL) NOPASSWD:ALL" > "/etc/sudoers.d/$1" && install -d -o "$1" -g "$1" "$(dirname "$2")"' bash $LinuxUser $RepoPath
Assert-Exit "Could not create Linux user $LinuxUser"
wsl --manage $Distro --set-default-user $LinuxUser
Assert-Exit "Could not make $LinuxUser the default WSL user"

function Test-WslDns {
    foreach ($attempt in 1..3) {
        wsl -d $Distro --exec getent hosts github.com | Out-Null
        if (-not $LASTEXITCODE) { return $true }
        Start-Sleep -Seconds 2
    }
    return $false
}

# Some devboxes' WSL NAT DNS proxy never answers; fall back to the resolvers of Windows' default route.
if (-not (Test-WslDns)) {
    $route = Get-NetRoute -DestinationPrefix '0.0.0.0/0' | Sort-Object { $_.RouteMetric + $_.InterfaceMetric } | Select-Object -First 1
    $servers = @((Get-DnsClientServerAddress -InterfaceIndex $route.ifIndex -AddressFamily IPv4).ServerAddresses)
    if (-not $servers) { throw "WSL DNS does not resolve github.com and Windows has no IPv4 DNS servers to fall back to." }
    Write-Host "WSL DNS proxy is not answering; using Windows DNS servers $($servers -join ', ')" -ForegroundColor DarkYellow
    wsl -d $Distro -u root --exec bash -c 'grep -q "^generateResolvConf" /etc/wsl.conf 2>/dev/null || printf "\n[network]\ngenerateResolvConf = false\n" >> /etc/wsl.conf; rm -f /etc/resolv.conf; printf "nameserver %s\n" "$@" > /etc/resolv.conf' bash @servers
    Assert-Exit 'Could not write the WSL DNS configuration'
    wsl --terminate $Distro
    if (-not (Test-WslDns)) { throw 'WSL still cannot resolve github.com.' }
}

wsl -d $Distro -u root --exec bash -c 'command -v git >/dev/null && [ -s /etc/ssl/certs/ca-certificates.crt ] || { apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -yqq -o Dpkg::Use-Pty=0 --no-install-recommends git ca-certificates; }'
Assert-Exit 'Could not install git and ca-certificates in WSL'

# Git Credential Manager lets git inside WSL reuse the Windows sign-in for pushes.
$gcm = @((Get-Command git-credential-manager -ErrorAction SilentlyContinue).Source,
    (Join-Path (Split-Path (Split-Path (git --exec-path))) 'bin\git-credential-manager.exe')) |
    Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1

wsl -d $Distro --cd ~ --exec bash -c 'set -euo pipefail; git config --global user.name "$1"; git config --global user.email "$2"; if [ -n "$6" ]; then git config --global credential.helper "$(printf %q "$(wslpath -u "$6")")"; fi; export GIT_TERMINAL_PROMPT=0; git clone --no-checkout --branch "$3" "$4" "$7"; cd "$7"; git checkout --quiet -B "$3" "$5"; [ "$(git rev-parse HEAD)" = "$5" ] && [ "$(git symbolic-ref --short HEAD)" = "$3" ] && [ "$(git remote get-url origin)" = "$4" ]' bash `
    "$(git -C $root config user.name)" "$(git -C $root config user.email)" $branch $origin $commit "$gcm" $RepoPath
Assert-Exit "Could not clone $branch at $commit into $RepoPath"
