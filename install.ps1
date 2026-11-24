<#
.SYNOPSIS
    omc installer for Windows: installs a release (checked against its SHA256SUMS) or builds omc from source.

.DESCRIPTION
    The twin of install.sh. By default it downloads the release's prebuilt Windows binary, verifies it against the
    release's SHA256SUMS and installs omc.exe. -Build builds it from source instead: a git branch or tag (-Ref), a
    release's source archive, or a local checkout (-Source). Building needs a MinGW-w64 toolchain, either cross in
    WSL (sudo apt install mingw-w64 make gcc) or native in MSYS2 (pacman -S mingw-w64-ucrt-x86_64-gcc make), and
    Git for Windows for -Ref; this script checks for them, says what is missing and installs none of them.

    omc's bash tool runs Git for Windows' bash (Git Bash) when installed, else PowerShell.

    Works in Windows PowerShell 5.1 and PowerShell 7:
        irm https://raw.githubusercontent.com/buzzer-re/Oh-My-C/main/install.ps1 | iex
        & ([scriptblock]::Create((irm https://raw.githubusercontent.com/buzzer-re/Oh-My-C/main/install.ps1))) -Lib
        .\install.ps1 -Build -Ref main
        .\install.ps1 -Build -Source C:\src\Oh-My-C -Lib

.PARAMETER Build
    Build from source instead of installing the prebuilt binary.
.PARAMETER Ref
    With -Build: the git branch, tag or commit to build (cloned with Git for Windows). Without it, the release's
    source archive is built.
.PARAMETER Source
    With -Build: a local checkout to build instead of downloading one.
.PARAMETER Version
    The release to install, e.g. 19.1.0 (default: the latest release). Also $env:OMC_VERSION.
.PARAMETER InstallDir
    Where omc.exe goes (default: %LOCALAPPDATA%\Programs\omc). Also $env:OMC_INSTALL_DIR.
.PARAMETER Lib
    Also install libomc: lib\libomc.dll, lib\libomc.dll.a, lib\libomc.a and include\omc.h under the install directory.
.PARAMETER Jobs
    Parallel build jobs (default: the number of processors). Also $env:OMC_JOBS.
.PARAMETER AddToPath
    Add the install directory to the user PATH without asking.
.PARAMETER NoPath
    Leave the user PATH alone. Also $env:OMC_NO_PATH=1.
.PARAMETER Yes
    Never ask: implies -AddToPath.
#>
[CmdletBinding()]
param(
    [switch]$Build,
    [string]$Ref = "",
    [string]$Source = "",
    [string]$Version = "",
    [string]$InstallDir = "",
    [switch]$Lib,
    [int]$Jobs = 0,
    [switch]$AddToPath,
    [switch]$Yes,
    [switch]$NoPath
)

# `irm ... | iex` runs this script in the caller's own scope: everything that changes the session (strict mode, error
# preference) is set inside Invoke-OmcInstall, so the user's PowerShell is left as it was.

function Write-Step([string]$Message) { Write-Host "omc-install: $Message" }

function Stop-Install([string]$Message) {
    Write-Host "omc-install: error: $Message" -ForegroundColor Red
    throw "omc-install: $Message"
}

function Get-Setting([string]$Value, [string]$EnvName, [string]$Default) {
    if ($Value) { return $Value }
    $e = [Environment]::GetEnvironmentVariable($EnvName)
    if ($e) { return $e }
    return $Default
}

function Initialize-Tls {
    # Windows PowerShell 5.1 defaults to TLS 1.0 on older systems; GitHub needs 1.2+
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    } catch { }
}

function Get-Download([string]$Url, [string]$OutFile) {
    try {
        $old = $ProgressPreference
        $ProgressPreference = "SilentlyContinue"
        Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing
        $ProgressPreference = $old
    } catch {
        Stop-Install "download failed: $Url ($($_.Exception.Message))"
    }
}

function Get-LatestVersion([string]$Repo) {
    try {
        $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -UseBasicParsing -Headers @{ "User-Agent" = "omc-install" }
    } catch {
        Stop-Install "could not find the latest release of $Repo ($($_.Exception.Message))"
    }
    $tag = [string]$rel.tag_name
    if (-not $tag) { Stop-Install "could not find the latest release of $Repo (is it published?)" }
    return $tag.TrimStart("v")
}

function Test-Checksum([string]$File, [string]$SumsFile, [string]$Asset) {
    $want = $null
    foreach ($line in Get-Content -LiteralPath $SumsFile) {
        $parts = $line.Trim() -split "\s+", 2
        if ($parts.Count -eq 2 -and ($parts[1] -eq $Asset -or $parts[1] -eq "*$Asset")) { $want = $parts[0].ToLowerInvariant() }
    }
    if (-not $want) { Stop-Install "SHA256SUMS has no entry for $Asset" }
    $got = (Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($got -ne $want) { Stop-Install "checksum mismatch for $Asset (expected $want, got $got); nothing was installed" }
    Write-Step "checksum ok"
}

function Get-Arch {
    $a = $env:PROCESSOR_ARCHITECTURE
    if ($env:PROCESSOR_ARCHITEW6432) { $a = $env:PROCESSOR_ARCHITEW6432 }
    switch ($a) {
        "AMD64" { return "x86_64" }
        "ARM64" { return "arm64" }
        default { return $a.ToLowerInvariant() }
    }
}

# ---------- building ----------

function Test-Command([string]$Name) { return [bool](Get-Command $Name -ErrorAction SilentlyContinue) }

# the toolchain to build with: "wsl" (cross-compile in WSL) or "msys2" (native MinGW-w64), else $null
function Find-Toolchain {
    if (Test-Command "wsl.exe") {
        $probe = & wsl.exe -e sh -c "command -v x86_64-w64-mingw32-gcc-posix || command -v x86_64-w64-mingw32-gcc; command -v make; command -v cc" 2>$null
        $lines = @($probe | Where-Object { $_ })
        if ($lines.Count -ge 3) { return "wsl" }
    }
    if ((Test-Command "gcc.exe") -and ((Test-Command "make.exe") -or (Test-Command "mingw32-make.exe"))) { return "msys2" }
    return $null
}

function Show-ToolchainHelp {
    Write-Host @"
omc-install: building omc on Windows needs a MinGW-w64 C toolchain and make. Either:
  * WSL (Ubuntu):   sudo apt install mingw-w64 make gcc
  * MSYS2 (UCRT64): pacman -S mingw-w64-ucrt-x86_64-gcc make   (then run this from the UCRT64 shell or put its bin on PATH)
-Ref also needs Git for Windows (git.exe). Nothing was installed.
"@
}

function ConvertTo-WslPath([string]$WinPath) {
    $full = [IO.Path]::GetFullPath($WinPath)
    $drive = $full.Substring(0, 1).ToLowerInvariant()
    return "/mnt/$drive" + $full.Substring(2).Replace("\", "/")
}

function Invoke-SourceBuild([string]$Src, [string]$Toolchain, [int]$JobCount, [bool]$WithLib, [string]$LogFile) {
    $targets = "all"
    if ($WithLib) { $targets = "all lib" }
    Write-Step "building with $JobCount job(s) ($Toolchain); this takes a few minutes"
    if ($Toolchain -eq "wsl") {
        $wsrc = ConvertTo-WslPath $Src
        & wsl.exe -e sh -c "cd '$wsrc' && make WINDOWS=1 -j$JobCount $targets" *> $LogFile
    } else {
        $make = "make.exe"
        if (-not (Test-Command $make)) { $make = "mingw32-make.exe" }
        Push-Location $Src
        try { & $make WINDOWS=1 MINGW= HOSTCC=gcc "-j$JobCount" $targets.Split(" ") *> $LogFile } finally { Pop-Location }
    }
    if ($LASTEXITCODE -ne 0) {
        Get-Content -LiteralPath $LogFile -Tail 25 | ForEach-Object { Write-Host $_ }
        Stop-Install "the build failed (log above)"
    }
    $exe = Join-Path $Src "build\omc.exe"
    if (-not (Test-Path -LiteralPath $exe)) { Stop-Install "the build produced no build\omc.exe" }
    return $exe
}

# ---------- installing ----------

function Install-File([string]$From, [string]$To) {
    # a running omc.exe cannot be overwritten, but it can be renamed: move it aside, then put the new one in place
    if (Test-Path -LiteralPath $To) {
        $aside = "$To.old-" + (Get-Date -Format "yyyyMMddHHmmss")
        try {
            Move-Item -LiteralPath $To -Destination $aside -Force
        } catch {
            Stop-Install "cannot replace ${To}: $($_.Exception.Message)"
        }
        try { Remove-Item -LiteralPath $aside -Force -ErrorAction Stop } catch { }
    }
    Copy-Item -LiteralPath $From -Destination $To -Force
}

function Remove-StaleCopies([string]$Dir) {
    # copies a previous install moved aside while they were running
    Get-ChildItem -LiteralPath $Dir -Filter "*.old-*" -ErrorAction SilentlyContinue | ForEach-Object {
        try { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction Stop } catch { }
    }
}

function Add-UserPath([string]$Dir, [bool]$Ask) {
    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if (-not $userPath) { $userPath = "" }
    $parts = $userPath.Split(";") | Where-Object { $_ }
    foreach ($p in $parts) {
        if ($p.TrimEnd("\") -ieq $Dir.TrimEnd("\")) { return }
    }
    if ($Ask) {
        $answer = Read-Host "omc-install: add $Dir to your user PATH? [Y/n]"
        if ($answer -and $answer.Trim().ToLowerInvariant().StartsWith("n")) {
            Write-Step "not added; run it as $Dir\omc.exe or add $Dir to PATH yourself"
            return
        }
    }
    $new = ($parts + $Dir) -join ";"
    [Environment]::SetEnvironmentVariable("Path", $new, "User")
    $env:Path = "$env:Path;$Dir"
    Write-Step "added $Dir to your user PATH (new terminals pick it up)"
}

function Test-GitBash {
    $roots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $(if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA "Programs" }))
    foreach ($r in $roots) {
        if ($r -and (Test-Path -LiteralPath (Join-Path $r "Git\bin\bash.exe"))) { return $true }
    }
    return [bool](Get-Command "bash.exe" -ErrorAction SilentlyContinue | Where-Object { $_.Source -notmatch "\\System32\\|\\WindowsApps\\" })
}

# ---------- main ----------

function Invoke-OmcInstall {
    $ErrorActionPreference = "Stop"
    Set-StrictMode -Version 2.0
    Initialize-Tls
    $repo = Get-Setting "" "OMC_REPO" "buzzer-re/Oh-My-C"
    $base = Get-Setting "" "OMC_DOWNLOAD_URL" "https://github.com/$repo/releases/download"
    $dest = Get-Setting $InstallDir "OMC_INSTALL_DIR" (Join-Path $env:LOCALAPPDATA "Programs\omc")
    $doBuild = $Build -or ($env:OMC_BUILD -eq "1") -or [bool]$Source -or [bool]$Ref
    $jobCount = $Jobs
    if ($jobCount -le 0) { $jobCount = [int](Get-Setting "" "OMC_JOBS" "$([Environment]::ProcessorCount)") }
    $arch = Get-Arch

    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("omc-install-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
    New-Item -ItemType Directory -Path $tmp | Out-Null
    try {
        if ($doBuild) {
            $toolchain = Find-Toolchain
            if (-not $toolchain) {
                Show-ToolchainHelp
                Stop-Install "no MinGW-w64 toolchain found"
            }
            if ($Source) {
                $src = (Resolve-Path -LiteralPath $Source).Path
                if (-not (Test-Path -LiteralPath (Join-Path $src "Makefile"))) { Stop-Install "$src has no Makefile" }
                Write-Step "building omc from $src and installing it into $dest"
            } elseif ($Ref) {
                if (-not (Test-Command "git.exe")) { Show-ToolchainHelp; Stop-Install "git is not installed" }
                $src = Join-Path $tmp "src"
                Write-Step "cloning $repo at $Ref"
                & git.exe clone --quiet --depth 1 --branch $Ref -c core.autocrlf=false "https://github.com/$repo.git" $src 2>&1 | Out-Null
                if ($LASTEXITCODE -ne 0) {
                    # a commit, not a branch or tag
                    & git.exe clone --quiet -c core.autocrlf=false "https://github.com/$repo.git" $src 2>&1 | Out-Null
                    if ($LASTEXITCODE -ne 0) { Stop-Install "git clone of $repo failed" }
                    & git.exe -C $src checkout --quiet $Ref 2>&1 | Out-Null
                    if ($LASTEXITCODE -ne 0) { Stop-Install "no ref $Ref in $repo" }
                }
                Write-Step "building omc $Ref and installing it into $dest"
            } else {
                $ver = Get-Setting $Version "OMC_VERSION" ""
                if (-not $ver) { $ver = Get-LatestVersion $repo }
                $ver = $ver.TrimStart("v")
                $asset = "omc-$ver.tar.gz"
                Get-Download "$base/v$ver/$asset" (Join-Path $tmp $asset)
                Get-Download "$base/v$ver/SHA256SUMS" (Join-Path $tmp "SHA256SUMS")
                Test-Checksum (Join-Path $tmp $asset) (Join-Path $tmp "SHA256SUMS") $asset
                & tar.exe -xzf (Join-Path $tmp $asset) -C $tmp
                if ($LASTEXITCODE -ne 0) { Stop-Install "could not unpack $asset" }
                $src = Join-Path $tmp "omc-$ver"
                Write-Step "building omc $ver from source and installing it into $dest"
            }
            $exe = Invoke-SourceBuild $src $toolchain $jobCount $Lib.IsPresent (Join-Path $tmp "build.log")
            $libDir = Join-Path $src "build\windows"
            $header = Join-Path $src "include\omc.h"
        } else {
            if ($arch -ne "x86_64") { Stop-Install "no prebuilt omc for windows-$arch; build it from source with -Build" }
            $ver = Get-Setting $Version "OMC_VERSION" ""
            if (-not $ver) { $ver = Get-LatestVersion $repo }
            $ver = $ver.TrimStart("v")
            $asset = "omc-$ver-windows-$arch.zip"
            Write-Step "installing omc $ver (windows-$arch) into $dest"
            Get-Download "$base/v$ver/$asset" (Join-Path $tmp $asset)
            Get-Download "$base/v$ver/SHA256SUMS" (Join-Path $tmp "SHA256SUMS")
            Test-Checksum (Join-Path $tmp $asset) (Join-Path $tmp "SHA256SUMS") $asset
            Expand-Archive -LiteralPath (Join-Path $tmp $asset) -DestinationPath $tmp -Force
            $unpacked = Join-Path $tmp "omc-$ver-windows-$arch"
            $exe = Join-Path $unpacked "omc.exe"
            if (-not (Test-Path -LiteralPath $exe)) { Stop-Install "the archive has no omc.exe" }
            $libDir = Join-Path $unpacked "lib"
            $header = Join-Path $unpacked "include\omc.h"
        }

        & $exe --version *> $null
        if ($LASTEXITCODE -ne 0) { Stop-Install "omc.exe does not run on this system; try -Build" }

        New-Item -ItemType Directory -Force -Path $dest | Out-Null
        Remove-StaleCopies $dest
        Install-File $exe (Join-Path $dest "omc.exe")
        if ($Lib) {
            $ld = Join-Path $dest "lib"
            $id = Join-Path $dest "include"
            New-Item -ItemType Directory -Force -Path $ld, $id | Out-Null
            $copied = 0
            foreach ($f in @("libomc.dll", "libomc.dll.a", "libomc.a", "libomc.def", "omc.pc")) {
                $p = Join-Path $libDir $f
                if (Test-Path -LiteralPath $p) { Install-File $p (Join-Path $ld $f); $copied++ }
            }
            if (Test-Path -LiteralPath $header) { Copy-Item -LiteralPath $header -Destination (Join-Path $id "omc.h") -Force }
            if ($copied -eq 0) { Write-Step "this release carries no libomc; build it with -Build -Lib" }
            else { Write-Step "installed libomc into $ld and omc.h into $id" }
        }
        $installed = & (Join-Path $dest "omc.exe") --version 2>$null
        Write-Step "installed $installed to $(Join-Path $dest 'omc.exe')"

        if (-not ($NoPath -or $env:OMC_NO_PATH -eq "1")) { Add-UserPath $dest (-not ($AddToPath -or $Yes)) }
        if (-not (Test-GitBash)) {
            Write-Step "note: no Git Bash found, so omc's bash tool will run PowerShell; for bash, install Git for Windows: https://git-scm.com/download/win"
        }
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Invoke-OmcInstall
