<#
.SYNOPSIS
    omc's Windows test run: unit tests, the CLI, print mode with every core tool against a local mock model, sessions,
    TLS, the TUI on a real console, Ctrl+C and the libomc embedding. One command, PASS/FAIL/SKIP per check, a summary
    and a report file to paste back.

.DESCRIPTION
    Everything runs in a throwaway HOME / USERPROFILE / agent dir under %TEMP%; the real %USERPROFILE%\.omc and \.omp
    are never read or written, and no model provider is contacted (the one real host is github.com for the TLS check;
    -Offline skips it).

    Needs: a build (`make WINDOWS=1 test-build`, or -Build here), Git for Windows (bash for the bash tool, openssl for
    the self-signed TLS check) and Python 3 for the mock servers (python.org; checks that need a mock are SKIPped
    without one, unless -MockPort points at a mock_openai.py already running elsewhere, e.g. in WSL).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\windows\run-tests.ps1
    powershell -ExecutionPolicy Bypass -File tests\windows\run-tests.ps1 -Build -Report $env:TEMP\omc-report.txt
#>
[CmdletBinding()]
param(
    [switch]$Build,
    [string]$Python = "",
    [int]$MockPort = 0,
    [string]$Report = "",
    [string]$Only = "",
    [switch]$Offline,
    [switch]$KeepTemp
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0
# .NET Framework's redirected stdin writer emits Console.InputEncoding's preamble (a UTF-8 BOM) the moment it is
# created, and omc reads piped stdin as part of the prompt: no BOM
try { [Console]::InputEncoding = New-Object System.Text.UTF8Encoding($false) } catch { }

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$Omc = Join-Path $Root "build\omc.exe"
$ObjTests = Join-Path $Root "build\obj\Windows-x86_64\tests"
$Driver = Join-Path $Root "build\windows\conpty_drive.exe"
$script:Results = New-Object System.Collections.ArrayList
$script:Log = New-Object System.Text.StringBuilder

# ---------- results ----------

function Add-Result([string]$Name, [string]$Status, [string]$Detail = "") {
    [void]$script:Results.Add([pscustomobject]@{ Name = $Name; Status = $Status; Detail = $Detail })
    $color = switch ($Status) { "PASS" { "Green" } "FAIL" { "Red" } default { "Yellow" } }
    $line = "{0,-5} {1}" -f $Status, $Name
    Write-Host $line -ForegroundColor $color
    [void]$script:Log.AppendLine($line)
    if ($Detail -and $Status -ne "PASS") {
        $d = ($Detail -split "`n" | Select-Object -First 25) -join "`n      "
        Write-Host "      $d"
        [void]$script:Log.AppendLine("      $d")
    }
}

function Test-Wanted([string]$Name) {
    if (-not $Only) { return $true }
    foreach ($o in $Only.Split(",")) { if ($o.Trim() -and $Name -like "*$($o.Trim())*") { return $true } }
    return $false
}

# ---------- processes ----------

function Invoke-Program([string]$File, [string[]]$Arguments, [int]$TimeoutSec = 60, [string]$WorkDir = "", [hashtable]$EnvOverride = $null) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $File
    $psi.Arguments = ($Arguments | ForEach-Object { ConvertTo-ArgString $_ }) -join " "
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardInput = $true
    $psi.CreateNoWindow = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    if ($WorkDir) { $psi.WorkingDirectory = $WorkDir } else { $psi.WorkingDirectory = $script:Work }
    if ($EnvOverride) {
        # the child block is rebuilt from this case-insensitive table: one entry per name, even when the parent (an MSYS
        # shell) carries case-variant duplicates such as PROGRAMFILES next to ProgramFiles
        foreach ($k in $EnvOverride.Keys) {
            if ($null -eq $EnvOverride[$k]) { $psi.EnvironmentVariables.Remove($k) } else { $psi.EnvironmentVariables[$k] = $EnvOverride[$k] }
        }
    }
    $p = [System.Diagnostics.Process]::Start($psi)
    # the raw stream: the StreamWriter would write a UTF-8 BOM, which omc reads as piped input
    $p.StandardInput.BaseStream.Close()
    $out = $p.StandardOutput.ReadToEndAsync()
    $err = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        try { $p.Kill() } catch { }
        return [pscustomobject]@{ Code = -1; Out = "TIMEOUT after ${TimeoutSec}s`n" + $out.Result + $err.Result }
    }
    $p.WaitForExit()
    return [pscustomobject]@{ Code = $p.ExitCode; Out = ($out.Result + $err.Result) }
}

function ConvertTo-ArgString([string]$A) {
    if ($A -and $A -notmatch '[\s"]') { return $A }
    # MSVC rules: backslashes before a quote are doubled, quotes escaped
    $s = [regex]::Replace($A, '(\\*)"', { param($m) $m.Groups[1].Value + $m.Groups[1].Value + '\"' })
    $s = [regex]::Replace($s, '(\\+)$', { param($m) $m.Groups[1].Value + $m.Groups[1].Value })
    return '"' + $s + '"'
}

function Test-Contains([string]$Name, [string]$Want, [string[]]$OmcArgs, [int]$TimeoutSec = 60) {
    if (-not (Test-Wanted $Name)) { return }
    $r = Invoke-Program $Omc $OmcArgs $TimeoutSec
    if ($r.Out.Contains($Want)) { Add-Result $Name "PASS" } else { Add-Result $Name "FAIL" "want: $Want`ngot: $($r.Out)" }
}

# ---------- setup ----------

function Find-Python {
    if ($Python) { return $Python }
    foreach ($c in @("py.exe", "python.exe", "python3.exe")) {
        $cmd = Get-Command $c -ErrorAction SilentlyContinue
        if (-not $cmd) { continue }
        if ($cmd.Source -match "\\WindowsApps\\") { continue } # the Store's install stub
        return $cmd.Source
    }
    return $null
}

function Find-GitBin([string]$Name) {
    foreach ($r in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $(if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA "Programs" }))) {
        if (-not $r) { continue }
        foreach ($sub in @("Git\bin", "Git\usr\bin")) {
            $p = Join-Path $r (Join-Path $sub $Name)
            if (Test-Path -LiteralPath $p) { return $p }
        }
    }
    return $null
}

function Get-FreePort {
    $l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $l.Start()
    $port = $l.LocalEndpoint.Port
    $l.Stop()
    return $port
}

function Wait-Port([int]$Port, [int]$TimeoutSec = 10) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        try {
            $c = New-Object System.Net.Sockets.TcpClient
            $c.Connect("127.0.0.1", $Port)
            $c.Close()
            return $true
        } catch { Start-Sleep -Milliseconds 100 }
    }
    return $false
}

function Write-ModelsYml([string]$Dir, [string]$BaseUrl) {
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    $yml = "providers:`n  mock:`n    baseUrl: $BaseUrl`n    apiKey: MOCK_KEY`n    api: openai-completions`n    models:`n      - id: mock-1`n        contextWindow: 100000`n        maxTokens: 4096`n" +
           "      - id: mock-vision`n        input: [text, image]`n        contextWindow: 100000`n        maxTokens: 4096`n"
    [IO.File]::WriteAllText((Join-Path $Dir "models.yml"), $yml)
}

function Invoke-Build {
    Write-Host "== building (make WINDOWS=1 test-build)"
    if (Get-Command wsl.exe -ErrorAction SilentlyContinue) {
        $full = [IO.Path]::GetFullPath($Root)
        $w = "/mnt/" + $full.Substring(0, 1).ToLowerInvariant() + $full.Substring(2).Replace("\", "/")
        & wsl.exe -e sh -c "cd '$w' && make WINDOWS=1 -j$([Environment]::ProcessorCount) test-build"
    } elseif (Get-Command make.exe -ErrorAction SilentlyContinue) {
        Push-Location $Root
        try { & make.exe WINDOWS=1 MINGW= HOSTCC=gcc "-j$([Environment]::ProcessorCount)" test-build } finally { Pop-Location }
    } else {
        throw "no toolchain to build with: see install.ps1 -Build's requirements"
    }
    if ($LASTEXITCODE -ne 0) { throw "the build failed" }
}

# ---------- the checks ----------

function Invoke-UnitTests([string]$Py) {
    $needsPython = @{ "test_auth" = 1; "test_dump" = 1; "test_mcp" = 1 }
    $mk = Get-Content -LiteralPath (Join-Path $Root "Makefile") -Raw
    $m = [regex]::Match($mk, "UNIT_TESTS := ((?:[^\n]*\\\n)*[^\n]*)")
    $names = ($m.Groups[1].Value -replace "\\", " ") -split "\s+" | Where-Object { $_ }
    foreach ($t in $names) {
        $name = "unit $t"
        if (-not (Test-Wanted $name)) { continue }
        $exe = Join-Path $ObjTests "$t.exe"
        if (-not (Test-Path -LiteralPath $exe)) { Add-Result $name "FAIL" "not built: $exe (make WINDOWS=1 test-build)"; continue }
        if ($needsPython.ContainsKey($t) -and -not $Py) { Add-Result $name "SKIP" "needs Python 3 (its mock server)"; continue }
        $r = Invoke-Program $exe @() 900 $Root
        if ($r.Code -eq 0) { Add-Result $name "PASS" }
        else { Add-Result $name "FAIL" (($r.Out -split "`n" | Where-Object { $_ -match "FAIL|fail|error|TIMEOUT" } | Select-Object -First 12) -join "`n") }
    }
}

function Invoke-CliChecks {
    if (Test-Wanted "cli version") {
        $r = Invoke-Program $Omc @("--version")
        if ($r.Code -eq 0 -and $r.Out -match "^omc/\d+\.\d+\.\d+") { Add-Result "cli version" "PASS" } else { Add-Result "cli version" "FAIL" $r.Out }
    }
    if (Test-Wanted "cli help") {
        $r = Invoke-Program $Omc @("--help")
        if ($r.Out.Contains("USAGE") -and $r.Out.Contains("--model")) { Add-Result "cli help" "PASS" } else { Add-Result "cli help" "FAIL" $r.Out }
    }
}

function Invoke-PrintChecks {
    $m = @("--model", "mock/mock-1")
    Test-Contains "print say" "hello from windows" ($m + @("--no-session", "-p", "say:hello from windows"))
    Test-Contains "bash tool (Git Bash)" "Tool said: hello-e2e" ($m + @("-p", "bash:echo hello-e2e"))
    Test-Contains "bash exit code" "Command exited with code 3" ($m + @("--no-session", "-p", 'call:bash:{"command":"exit 3"}'))
    Test-Contains "bash pipes and tools" "Tool said: 3" ($m + @("--no-session", "-p", "bash:printf 'a\nb\nc\n' | wc -l"))
    Test-Contains "bash cwd" "Tool said: $(($script:Work -replace '\\', '/').Substring(2))" ($m + @("--no-session", "-p", 'bash:pwd -W | cut -c3-'))
    if ((Test-Wanted "bash notify job") -and $env:MOCK_LOG) {
        # a background job's matching output reaches the model as a notice that ends the wait (checked in the request)
        $null = Invoke-Program $Omc ($m + @("--no-session", "-p", 'seq:[["bash",{"command":"echo start; echo ERROR one; sleep 2","notify":"ERROR"}],["wait",{}]]')) 90
        $last = (Get-Content -LiteralPath $env:MOCK_LOG -Encoding UTF8 | Select-Object -Last 1)
        if ($last -and $last.Contains("Background job bg_1") -and $last.Contains("ERROR one")) { Add-Result "bash notify job" "PASS" }
        else { Add-Result "bash notify job" "FAIL" "the last request carries no background-job notice" }
    }
    Test-Contains "write tool" "Successfully wrote 14 bytes to a/b.txt" ($m + @("-p", "write:a/b.txt:written by omc"))
    if (Test-Wanted "file content") {
        $f = Join-Path $script:Work "a\b.txt"
        if ((Test-Path -LiteralPath $f) -and ([IO.File]::ReadAllText($f) -eq "written by omc")) { Add-Result "file content" "PASS" }
        else { Add-Result "file content" "FAIL" "a\b.txt missing or wrong" }
    }
    Test-Contains "read hashline" "1:written by omc" ($m + @("-p", "read:a/b.txt"))
    Test-Contains "glob tool" "b.txt" ($m + @("-p", "glob:**/*.txt"))
    Test-Contains "grep tool" "1:written by omc" ($m + @("-p", "grep:written"))
    Test-Contains "todo tool" "Remaining items (2)" ($m + @("-p", "todo:first,second"))
    $env:PI_EDIT_VARIANT = "replace"
    Test-Contains "edit tool" "Tool said" ($m + @("--no-session", "-p", 'call:edit:{"path":"a/b.txt","old_string":"written by omc","new_string":"edited on windows"}'))
    Remove-Item Env:PI_EDIT_VARIANT -ErrorAction SilentlyContinue
    if (Test-Wanted "edit result") {
        $f = Join-Path $script:Work "a\b.txt"
        $c = if (Test-Path -LiteralPath $f) { [IO.File]::ReadAllText($f) } else { "" }
        if ($c.Contains("edited on windows")) { Add-Result "edit result" "PASS" } else { Add-Result "edit result" "FAIL" "a\b.txt: $c" }
    }
    Test-Contains "path with spaces and non-ASCII" "Successfully wrote" ($m + @("-p", "write:dir with space/ñandú ü.txt:unicode ok"))
    Test-Contains "read spaces and non-ASCII" "1:unicode ok" ($m + @("-p", "read:dir with space/ñandú ü.txt"))
    Test-Contains "continue session" "Tool said" ($m + @("-c", "-p", "bash:echo again"))
    # PowerShell beside Git Bash: its own tool, UTF-8 output, native exit codes
    Test-Contains "powershell tool" "Tool said: ps-5" ($m + @("--no-session", "-p", 'call:powershell:{"command":"Write-Output (''ps-'' + (2+3))"}'))
    Test-Contains "powershell exit code" "Command exited with code 4" ($m + @("--no-session", "-p", 'call:powershell:{"command":"cmd /c exit 4"}'))
    Test-Contains "powershell utf-8" "Tool said: ñandú ü" ($m + @("--no-session", "-p", 'call:powershell:{"command":"Write-Output ''ñandú ü''"}'))
    if ((Test-Wanted "both shell tools") -and $env:MOCK_LOG) {
        $null = Invoke-Program $Omc ($m + @("--no-session", "-p", "say:tools"))
        $last = (Get-Content -LiteralPath $env:MOCK_LOG -Encoding UTF8 | Select-Object -Last 1)
        if ($last -and $last.Contains('"name": "bash"') -and $last.Contains('"name": "powershell"') -and $last.Contains("prefer it, and use ``powershell``")) {
            Add-Result "both shell tools" "PASS"
        } else { Add-Result "both shell tools" "FAIL" "the request lacks bash, powershell or the bash-first guidance" }
    }
    if (Test-Wanted "no bash fallback") {
        # no Git Bash anywhere: bash runs PowerShell, says so, and there is no second PowerShell tool
        # Windows derives ProgramFiles from ProgramW6432 when it creates a 64-bit process: both are hidden
        $hide = @{ ProgramFiles = "C:\omc-no-such-dir"; ProgramW6432 = "C:\omc-no-such-dir"; "ProgramFiles(x86)" = "C:\omc-no-such-dir";
                   LOCALAPPDATA = "C:\omc-no-such-dir";
                   GIT_INSTALL_ROOT = $null; SCOOP = $null; PATH = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\WindowsPowerShell\v1.0" }
        $r = Invoke-Program $Omc ($m + @("--no-session", "-p", 'bash:Write-Output ("fallback-" + $PSVersionTable.PSVersion.Major)')) 90 "" $hide
        $last = if ($env:MOCK_LOG) { (Get-Content -LiteralPath $env:MOCK_LOG -Encoding UTF8 | Select-Object -Last 1) } else { "" }
        if ($r.Out -match "Tool said: fallback-\d" -and $last.Contains("No bash on this system") -and -not $last.Contains('"name": "powershell"')) {
            Add-Result "no bash fallback" "PASS"
        } else { Add-Result "no bash fallback" "FAIL" $r.Out }
    }
    if (Test-Wanted "session files") {
        $sessions = Get-ChildItem -Recurse -Filter "*.jsonl" -Path (Join-Path $script:Agent "sessions") -ErrorAction SilentlyContinue
        if (@($sessions).Count -ge 1) { Add-Result "session files" "PASS" } else { Add-Result "session files" "FAIL" "no session .jsonl under $script:Agent\sessions" }
    }
    if (Test-Wanted "parallel processes") {
        # two omc processes on one agent dir at once (settings, auth and session locks)
        $outs = @()
        $procs = foreach ($word in @("one", "two")) {
            $o = Join-Path $script:Tmp "par-$word.txt"
            $outs += $o
            Start-Process -FilePath $Omc -ArgumentList "--model", "mock/mock-1", "-p", "`"say:job $word`"" -WorkingDirectory $script:Work `
                -RedirectStandardOutput $o -RedirectStandardError "$o.err" -WindowStyle Hidden -PassThru
        }
        foreach ($p in $procs) { [void]$p.WaitForExit(60000) }
        $ra = [IO.File]::ReadAllText($outs[0]); $rb = [IO.File]::ReadAllText($outs[1])
        if ($ra.Contains("job one") -and $rb.Contains("job two")) { Add-Result "parallel processes" "PASS" } else { Add-Result "parallel processes" "FAIL" "$ra`n$rb" }
    }
}

function Invoke-LibChecks {
    $embed = Join-Path $Root "build\examples\embed.exe"
    $embedDll = Join-Path $Root "build\windows\embed_dll.exe"
    foreach ($pair in @(@("libomc static", $embed), @("libomc.dll", $embedDll))) {
        $name = $pair[0]
        if (-not (Test-Wanted $name)) { continue }
        if (-not (Test-Path -LiteralPath $pair[1])) { Add-Result $name "SKIP" "not built: $($pair[1])"; continue }
        $r1 = Invoke-Program $pair[1] @('call:greet:{"name":"Ada"}', "mock/mock-1") 60 (Split-Path $pair[1])
        $r2 = Invoke-Program $pair[1] @("hi", "mock/mock-1") 60 (Split-Path $pair[1])
        if ($r1.Out.Contains("hello from the host app, Ada") -and $r2.Out.Contains('"command":"get_state","success":true')) { Add-Result $name "PASS" }
        else { Add-Result $name "FAIL" "$($r1.Out)`n$($r2.Out)" }
    }
}

function Invoke-TuiChecks {
    if (-not (Test-Path -LiteralPath $Driver)) { Add-Result "tui" "SKIP" "not built: $Driver"; return }
    $env:OMC_SKIP_SETUP = "1"
    $env:PI_NO_DESKTOP_NOTIFY = "1"
    $script = @"
expect OMP equivalent 15000
send say:hello from the tui
sleep 300
key enter
expect hello from the tui 20000
send bash:echo hi-from-git-bash
sleep 300
key enter
expect Tool said: hi-from-git-bash 20000
send first line
key enter shift
send second line
sleep 800
expect second line
screen
"@
    if (Test-Wanted "tui console") {
        $sf = Join-Path $script:Tmp "tui.script"
        [IO.File]::WriteAllText($sf, $script)
        $r = Invoke-DriverScript @("-m", "console", "-c", "100", "-r", "34", "--", $Omc, "--model", "mock/mock-1", "--no-session") $sf
        if ($r.Code -eq 0 -and $r.Out -notmatch "FAIL") { Add-Result "tui console" "PASS" } else { Add-Result "tui console" "FAIL" $r.Out }
    }
    if (Test-Wanted "tui ctrl+c") {
        # Ctrl+C clears the editor, a second one within the window quits (omp's double-press exit)
        $sf = Join-Path $script:Tmp "ctrlc.script"
        [IO.File]::WriteAllText($sf, "expect OMP equivalent 15000`nkey c ctrl`nsleep 200`nkey c ctrl`nwait 10000`n")
        $r = Invoke-DriverScript @("-m", "console", "-c", "100", "-r", "34", "--", $Omc, "--model", "mock/mock-1", "--no-session") $sf
        if ($r.Out -match "exit \d+") { Add-Result "tui ctrl+c" "PASS" } else { Add-Result "tui ctrl+c" "FAIL" $r.Out }
    }
    if (Test-Wanted "tui win32-input records") {
        # Windows Terminal's console host relays every key as a win32-input-mode record (CSI Vk;Sc;Uc;Kd;Cs;Rc _)
        # once omc asks for them: press + release of a and b, c with a repeat count of 3, then a typed Z
        $sf = Join-Path $script:Tmp "w32im.script"
        $raw = 'raw \e[65;30;97;1;0;1_\e[65;30;97;0;0;1_\e[66;48;98;1;0;1_\e[66;48;98;0;0;1_\e[67;46;99;1;0;3_\e[67;46;99;0;0;1_'
        [IO.File]::WriteAllText($sf, "expect OMP equivalent 15000`n$raw`nsleep 300`nsend Z`nexpect abcccZ 5000`n")
        $r = Invoke-DriverScript @("-m", "console", "-c", "100", "-r", "34", "--", $Omc, "--model", "mock/mock-1", "--no-session") $sf
        if ($r.Code -eq 0 -and $r.Out -notmatch "FAIL") { Add-Result "tui win32-input records" "PASS" }
        elseif ($env:GITHUB_ACTIONS -eq "true") {
            # GitHub's Windows Server 2025 console host drops these injected Vk-0 records before omc reads them (the
            # check passes on Windows 10/11 desktops and through Windows Terminal's OpenConsole)
            Add-Result "tui win32-input records" "SKIP" "the runner's console host drops injected Vk-0 key records"
        } else { Add-Result "tui win32-input records" "FAIL" $r.Out }
    }
    if (Test-Wanted "clipboard") { Invoke-ClipboardChecks }
    if (Test-Wanted "tui conpty") {
        $sf = Join-Path $script:Tmp "conpty.script"
        [IO.File]::WriteAllText($sf, "expect OMP equivalent 15000`nsend say:through conpty`nsleep 300`nsend \r`nexpect through conpty 20000`n")
        $r = Invoke-DriverScript @("-m", "conpty", "-c", "100", "-r", "34", "--", $Omc, "--model", "mock/mock-1", "--no-session") $sf
        if ($r.Code -eq 0 -and $r.Out -notmatch "FAIL") { Add-Result "tui conpty" "PASS" } else { Add-Result "tui conpty" "FAIL" $r.Out }
    }
    Remove-Item Env:OMC_SKIP_SETUP, Env:PI_NO_DESKTOP_NOTIFY -ErrorAction SilentlyContinue
}

# The Windows clipboard: Alt+V pastes an image (a bitmap, read as CF_DIB, and the registered PNG format) or text, and
# copies reach the native clipboard. These checks replace the clipboard and put the previous contents back afterwards.
function New-TestBitmap([int]$W, [int]$H) {
    $bmp = New-Object System.Drawing.Bitmap $W, $H
    for ($y = 0; $y -lt $H; $y++) { for ($x = 0; $x -lt $W; $x++) {
        $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(255, (40 * $x) % 256, (90 * $y) % 256, 200))
    } }
    return $bmp
}

function Get-MockLogLines {
    if ($env:MOCK_LOG -and (Test-Path -LiteralPath $env:MOCK_LOG)) { return @(Get-Content -LiteralPath $env:MOCK_LOG -Encoding UTF8) }
    return @()
}

function Get-RequestImage([int]$Skip) {
    # the image a request after the first $Skip carried: its `[Image #1, WxH]` marker (the image pipeline scales tiny
    # images up, keeping the aspect ratio) when an image part came with it; $null when none did
    foreach ($l in (Get-MockLogLines | Select-Object -Skip $Skip)) {
        $m = [regex]::Match($l, '\[Image #1, (\d+)x(\d+)\]')
        if ($m.Success -and $l.Contains('"type": "image_url"') -and $l -match 'data:image/') { return @([int]$m.Groups[1].Value, [int]$m.Groups[2].Value) }
    }
    return $null
}

function Invoke-ClipboardChecks {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    if ([Threading.Thread]::CurrentThread.GetApartmentState() -ne "STA") { Add-Result "clipboard" "SKIP" "needs an STA PowerShell"; return }
    $saved = $null
    try {
        $d = [Windows.Forms.Clipboard]::GetDataObject()
        if ($d) {
            $saved = New-Object Windows.Forms.DataObject
            foreach ($f in $d.GetFormats($false)) { try { $v = $d.GetData($f); if ($null -ne $v) { $saved.SetData($f, $v) } } catch { } }
        }
    } catch { }
    $omcArgs = @("-m", "console", "-c", "100", "-r", "34", "--", $Omc, "--model", "mock/mock-vision", "--no-session")
    try {
        # a bitmap (CF_BITMAP; omc reads the CF_DIB Windows synthesizes) and the registered PNG format alone
        foreach ($case in @(@{ Name = "clipboard bitmap"; W = 3; H = 2; Png = $false }, @{ Name = "clipboard png"; W = 5; H = 4; Png = $true })) {
            if (-not (Test-Wanted $case.Name)) { continue }
            $bmp = New-TestBitmap $case.W $case.H
            if ($case.Png) {
                $ms = New-Object System.IO.MemoryStream
                $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
                $do = New-Object Windows.Forms.DataObject
                $do.SetData("PNG", $false, $ms)
                [Windows.Forms.Clipboard]::SetDataObject($do, $true)
            } else { [Windows.Forms.Clipboard]::SetImage($bmp) }
            $bmp.Dispose()
            $sf = Join-Path $script:Tmp "clip.script"
            [IO.File]::WriteAllText($sf, "expect OMP equivalent 15000`nkey v alt`nexpect #1 8000`nsend  what is this`nkey enter`nexpect plain answer 20000`n")
            $before = @(Get-MockLogLines).Count
            $r = Invoke-DriverScript $omcArgs $sf
            $img = Get-RequestImage $before
            $ok = $img -and [Math]::Abs($img[0] * $case.H - $img[1] * $case.W) -le [Math]::Max($img[0], $img[1]) / 50
            if ($r.Code -eq 0 -and $r.Out -notmatch "FAIL" -and $ok) { Add-Result $case.Name "PASS" }
            else { Add-Result $case.Name "FAIL" "request image: $($img -join 'x') (want the aspect of $($case.W)x$($case.H))`n$($r.Out)" }
        }
        if (Test-Wanted "clipboard text") {
            [Windows.Forms.Clipboard]::SetText("pasted from the clipboard ñ")
            $sf = Join-Path $script:Tmp "cliptext.script"
            [IO.File]::WriteAllText($sf, "expect OMP equivalent 15000`nkey v alt`nexpect pasted from the clipboard ñ 8000`n")
            $r = Invoke-DriverScript $omcArgs $sf
            if ($r.Code -eq 0 -and $r.Out -notmatch "FAIL") { Add-Result "clipboard text" "PASS" } else { Add-Result "clipboard text" "FAIL" $r.Out }
        }
        if (Test-Wanted "clipboard copy") {
            # /copy cmd copies the last bash command through OSC 52 and the native clipboard (the console ignores OSC 52)
            [Windows.Forms.Clipboard]::SetText("before the copy")
            $sf = Join-Path $script:Tmp "clipcopy.script"
            [IO.File]::WriteAllText($sf, "expect OMP equivalent 15000`nsend bash:echo copy-me`nsleep 300`nkey enter`nexpect Tool said: copy-me 20000`nsend /copy cmd`nsleep 300`nkey enter`nexpect to clipboard 8000`n")
            $r = Invoke-DriverScript $omcArgs $sf
            $got = [Windows.Forms.Clipboard]::GetText()
            if ($got -eq "echo copy-me") { Add-Result "clipboard copy" "PASS" } else { Add-Result "clipboard copy" "FAIL" "clipboard: '$got'`n$($r.Out)" }
        }
    } finally {
        try { if ($saved -and $saved.GetFormats().Count) { [Windows.Forms.Clipboard]::SetDataObject($saved, $true) } else { [Windows.Forms.Clipboard]::Clear() } } catch { }
    }
}

function Invoke-DriverScript([string[]]$DriverArgs, [string]$ScriptFile) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Driver
    $psi.Arguments = ($DriverArgs | ForEach-Object { ConvertTo-ArgString $_ }) -join " "
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.WorkingDirectory = $script:Work
    $p = [System.Diagnostics.Process]::Start($psi)
    $bytes = [IO.File]::ReadAllBytes($ScriptFile)
    $p.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
    $p.StandardInput.BaseStream.Close()
    $out = $p.StandardOutput.ReadToEndAsync()
    $err = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(180000)) { try { $p.Kill() } catch { }; return [pscustomobject]@{ Code = -1; Out = "TIMEOUT`n" + $out.Result } }
    $p.WaitForExit()
    return [pscustomobject]@{ Code = $p.ExitCode; Out = $out.Result + $err.Result }
}

function Invoke-TlsChecks {
    if ((Test-Wanted "tls github") -and -not $Offline) {
        $r = Invoke-Program $Omc @("update", "--check") 60
        if ($r.Out -match "Current version|Update available|up to date") { Add-Result "tls github" "PASS" }
        elseif ($r.Out -match "resolve|DNS|connect to") { Add-Result "tls github" "SKIP" "no network: $($r.Out)" }
        else { Add-Result "tls github" "FAIL" $r.Out }
    }
    if (-not (Test-Wanted "tls self-signed")) { return }
    $openssl = Find-GitBin "openssl.exe"
    if (-not $openssl) { Add-Result "tls self-signed" "SKIP" "needs Git for Windows' openssl.exe"; return }
    $dir = Join-Path $script:Tmp "tls"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $k = Join-Path $dir "k.pem"; $c = Join-Path $dir "c.pem"
    # Git's openssl is an MSYS program: "//CN=" keeps MSYS from rewriting the subject into a path
    foreach ($subj in @("//CN=localhost", "/CN=localhost")) {
        if (Test-Path -LiteralPath $c) { break }
        Start-Process -FilePath $openssl -ArgumentList "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "`"$k`"", "-out", "`"$c`"",
            "-days", "2", "-subj", $subj -WindowStyle Hidden -Wait
    }
    $port = Get-FreePort
    $srv = Start-Process -FilePath $openssl -ArgumentList "s_server", "-accept", "$port", "-cert", "`"$c`"", "-key", "`"$k`"", "-www", "-quiet" -WindowStyle Hidden -PassThru
    try {
        if (-not (Wait-Port $port)) { Add-Result "tls self-signed" "SKIP" "openssl s_server did not start"; return }
        $agent = Join-Path $dir "agent"
        Write-ModelsYml $agent "https://127.0.0.1:$port/v1"
        [IO.File]::WriteAllText((Join-Path $agent "config.yml"), "retry:`n  enabled: false`n")
        $old = $env:OMC_CODING_AGENT_DIR
        $env:OMC_CODING_AGENT_DIR = $agent
        $r = Invoke-Program $Omc @("--model", "mock/mock-1", "--no-session", "-p", "hi") 120
        $env:OMC_CODING_AGENT_DIR = $old
        $want = "TLS certificate verification failed for 127.0.0.1:$port (DEPTH_ZERO_SELF_SIGNED_CERT)"
        if ($r.Out.Contains($want)) { Add-Result "tls self-signed" "PASS" } else { Add-Result "tls self-signed" "FAIL" "want: $want`ngot: $($r.Out)" }
    } finally {
        try { Stop-Process -Id $srv.Id -Force -ErrorAction Stop } catch { }
    }
}

function Invoke-RpcChecks([string]$Py) {
    if (-not (Test-Wanted "rpc")) { return }
    if (-not $Py) { Add-Result "rpc driver" "SKIP" "needs Python 3"; return }
    $driverPy = Join-Path $Root "tests\e2e\rpc_seq.py"
    $pyArgs = @()
    if ($Py -match "py\.exe$") { $pyArgs += "-3" }
    $r = Invoke-Program $Py ($pyArgs + @("-I", $driverPy, $Omc, "--arg", "--model", "--arg", "mock/mock-1", "--arg", "--no-session", "say:rpc works")) 90
    if ($r.Code -eq 0) { Add-Result "rpc driver" "PASS" } else { Add-Result "rpc driver" "FAIL" $r.Out }
}

# omc update and install.ps1 (run as `irm <url> | iex`) against tests/e2e/mock_release.py: GitHub is never contacted.
# update replaces a throwaway copy of omc.exe (the running binary, as a user's would be) with build\windows\fake_omc.exe
# packed as release 99.0.0; install.ps1 installs this build's omc.exe, packed as the current version's release zip.
function Invoke-ReleaseChecks([string]$Py) {
    if (-not ((Test-Wanted "update") -or (Test-Wanted "install"))) { return }
    if (-not $Py) { foreach ($n in @("update", "install.ps1")) { Add-Result $n "SKIP" "needs Python 3 (the release mock)" }; return }
    $fake = Join-Path $Root "build\windows\fake_omc.exe"
    if (-not (Test-Path -LiteralPath $fake)) { Add-Result "update" "FAIL" "not built: $fake (make WINDOWS=1 test-build)"; return }
    $pyArgs = @()
    if ($Py -match "py\.exe$") { $pyArgs += "-3" }
    $mockPy = Join-Path $Root "tests\e2e\mock_release.py"
    $rel = Join-Path $script:Tmp "rel"
    $cur = ((Invoke-Program $Omc @("--version") 10 $Root).Out.Trim()) -replace "^omc/", ""
    foreach ($m in @(@("99.0.0", $fake), @($cur, $Omc))) {
        $r = Invoke-Program $Py ($pyArgs + @("-I", $mockPy, "make", $rel, $m[0], "windows-x86_64", $m[1])) 60 $Root
        if ($r.Code -ne 0) { Add-Result "update" "FAIL" "mock_release.py make: $($r.Out)"; return }
    }
    Copy-Item -LiteralPath (Join-Path $Root "install.ps1") -Destination (Join-Path $rel "install.ps1")
    [IO.File]::WriteAllText((Join-Path $rel "LATEST"), "99.0.0")
    $port = Get-FreePort
    $srv = Start-Process -FilePath $Py -ArgumentList ($pyArgs + @("-I", "`"$mockPy`"", "serve", "$port", "`"$rel`"")) -WindowStyle Hidden -PassThru
    try {
        if (-not (Wait-Port $port 15)) { Add-Result "update" "SKIP" "the release mock did not start"; return }
        $urls = @{ "OMC_LATEST_URL" = "http://127.0.0.1:$port/buzzer-re/Oh-My-C/releases/latest"; "OMC_DOWNLOAD_URL" = "http://127.0.0.1:$port/download" }
        if (Test-Wanted "update") {
            $ud = Join-Path $script:Tmp "upd"
            New-Item -ItemType Directory -Force -Path $ud | Out-Null
            $copy = Join-Path $ud "omc.exe"
            Copy-Item -LiteralPath $Omc -Destination $copy
            $r = Invoke-Program $copy @("update", "--check") 60 $ud $urls
            if ($r.Out.Contains("New version available: 99.0.0")) { Add-Result "update check" "PASS" } else { Add-Result "update check" "FAIL" $r.Out }
            $sums = Join-Path $rel "v99.0.0\SHA256SUMS"
            $good = [IO.File]::ReadAllText($sums)
            [IO.File]::WriteAllText($sums, ("0" * 64) + "  omc-99.0.0-windows-x86_64.zip`n")
            $r = Invoke-Program $copy @("update") 60 $ud $urls
            $v = (Invoke-Program $copy @("--version") 10 $ud).Out.Trim()
            if ($r.Out.Contains("checksum mismatch for omc-99.0.0-windows-x86_64.zip") -and $v -eq "omc/$cur") { Add-Result "update checksum mismatch" "PASS" }
            else { Add-Result "update checksum mismatch" "FAIL" "$($r.Out)`nversion after: $v" }
            [IO.File]::WriteAllText($sums, $good)
            $r = Invoke-Program $copy @("update") 120 $ud $urls
            if ($r.Out.Contains("Updated to 99.0.0")) { Add-Result "update installs (running exe replaced)" "PASS" } else { Add-Result "update installs (running exe replaced)" "FAIL" $r.Out }
            $v = (Invoke-Program $copy @("--version") 10 $ud).Out.Trim()
            if ($v -eq "omc/99.0.0") { Add-Result "update new binary" "PASS" } else { Add-Result "update new binary" "FAIL" $v }
            $left = @(Get-ChildItem -LiteralPath $ud -Force | Where-Object { $_.Name -ne "omc.exe" -and $_.Name -notlike "omc.exe.old-*" })
            if ($left.Count -eq 0) { Add-Result "update no leftovers" "PASS" } else { Add-Result "update no leftovers" "FAIL" (($left | ForEach-Object { $_.Name }) -join ", ") }
        }
        if (Test-Wanted "install") {
            # exactly what the README tells users to paste, then proof that the session was left as it was
            $inst = Join-Path $script:Tmp "inst"
            $envs = $urls.Clone()
            $envs["OMC_VERSION"] = $cur
            $envs["OMC_INSTALL_DIR"] = $inst
            $envs["OMC_NO_PATH"] = "1"
            $cmd = "irm http://127.0.0.1:$port/assets/install.ps1 | iex; `$x = `$omcNeverSetVariable; Write-Output ('session-clean ' + `$ErrorActionPreference)"
            $r = Invoke-Program "powershell.exe" @("-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", $cmd) 180 $script:Work $envs
            $v = ""
            if (Test-Path -LiteralPath (Join-Path $inst "omc.exe")) { $v = (Invoke-Program (Join-Path $inst "omc.exe") @("--version") 10 $inst).Out.Trim() }
            if ($r.Out.Contains("checksum ok") -and $v -eq "omc/$cur") { Add-Result "install.ps1 via irm | iex" "PASS" }
            else { Add-Result "install.ps1 via irm | iex" "FAIL" "$($r.Out)`ninstalled: $v" }
            if ($r.Out.Contains("session-clean Continue")) { Add-Result "install.ps1 leaves the session alone" "PASS" }
            else { Add-Result "install.ps1 leaves the session alone" "FAIL" $r.Out }
            # parameters through a script block (the documented form for -Build, -Lib, ...); a re-install over itself
            $cmd = "& ([scriptblock]::Create((irm http://127.0.0.1:$port/assets/install.ps1))) -NoPath -Lib"
            $r = Invoke-Program "powershell.exe" @("-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", $cmd) 180 $script:Work $envs
            if ($r.Out.Contains("installed omc/$cur") -and $r.Out.Contains("carries no libomc")) { Add-Result "install.ps1 with parameters" "PASS" }
            else { Add-Result "install.ps1 with parameters" "FAIL" $r.Out }
        }
    } finally {
        try { Stop-Process -Id $srv.Id -Force -ErrorAction Stop } catch { }
    }
}

# ---------- main ----------

function Invoke-WindowsTests {
    if ($Build) { Invoke-Build }
    if (-not (Test-Path -LiteralPath $Omc)) { throw "no build\omc.exe: build it first (make WINDOWS=1 test-build) or pass -Build" }

    $saved = @{}
    foreach ($v in @("HOME", "USERPROFILE", "TMPDIR", "OMC_CODING_AGENT_DIR", "PI_CODING_AGENT_DIR", "MOCK_KEY", "OMC_NO_TITLE",
                     "OMC_NO_LOCAL_DISCOVERY", "OMC_NO_UPDATE_CHECK", "OMC_PLATFORM", "PI_INTENT_TRACING", "MOCK_LOG", "OMC_TEST_PYTHON", "PATH")) {
        $saved[$v] = [Environment]::GetEnvironmentVariable($v)
    }
    $script:Tmp = Join-Path ([IO.Path]::GetTempPath()) ("omc-wintest-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
    $home_ = Join-Path $script:Tmp "home"
    $script:Agent = Join-Path $script:Tmp "agent"
    $script:Work = Join-Path $script:Tmp "work"
    New-Item -ItemType Directory -Force -Path $home_, $script:Agent, $script:Work, (Join-Path $script:Tmp ".git") | Out-Null
    $mockProc = $null
    try {
        $env:HOME = $home_.Replace("\", "/")
        $env:USERPROFILE = $home_
        $env:TMPDIR = $script:Tmp.Replace("\", "/")
        $env:OMC_CODING_AGENT_DIR = $script:Agent
        $env:PI_CODING_AGENT_DIR = Join-Path $home_ ".omp\agent"
        $env:MOCK_KEY = "x"
        $env:OMC_NO_TITLE = "1"
        $env:OMC_NO_LOCAL_DISCOVERY = "1"
        $env:OMC_NO_UPDATE_CHECK = "1"
        $env:PI_INTENT_TRACING = "1"

        $py = Find-Python
        if ($py) {
            $env:OMC_TEST_PYTHON = $py
            # the tests and mocks call python3: a python.org install has no python3.exe, its py launcher stands in
            if ($py -match "py\.exe$") {
                $bin = Join-Path $script:Tmp "bin"
                New-Item -ItemType Directory -Force -Path $bin | Out-Null
                Copy-Item -LiteralPath $py -Destination (Join-Path $bin "python3.exe")
                $env:PATH = "$bin;$env:PATH"
            }
        }
        Write-Host "== omc Windows tests ($Omc)"
        Write-Host "   python: $(if ($py) { $py } else { 'none (mock checks are skipped)' })   git bash: $(if (Find-GitBin 'bash.exe') { 'yes' } else { 'no' })"

        Invoke-UnitTests $py
        $env:OMC_PLATFORM = $null
        Invoke-CliChecks

        $port = $MockPort
        if (-not $port -and $py) {
            $port = Get-FreePort
            $env:MOCK_LOG = Join-Path $script:Tmp "req.jsonl"
            $pyArgs = @()
            if ($py -match "py\.exe$") { $pyArgs += "-3" }
            $mockProc = Start-Process -FilePath $py -ArgumentList ($pyArgs + @("-I", "`"$(Join-Path $Root 'tests\e2e\mock_openai.py')`"", "$port")) -WindowStyle Hidden -PassThru
        }
        if ($port -and (Wait-Port $port 15)) {
            Write-ModelsYml $script:Agent "http://127.0.0.1:$port/v1"
            Invoke-PrintChecks
            Invoke-LibChecks
            Invoke-TuiChecks
            Invoke-RpcChecks $py
        } else {
            foreach ($n in @("print mode", "libomc", "tui", "rpc driver")) { Add-Result $n "SKIP" "no mock model (install Python 3, or -MockPort)" }
        }
        Invoke-TlsChecks
        Invoke-ReleaseChecks $py
    } finally {
        if ($mockProc) { try { Stop-Process -Id $mockProc.Id -Force -ErrorAction Stop } catch { } }
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
        if (-not $KeepTemp) { Remove-Item -LiteralPath $script:Tmp -Recurse -Force -ErrorAction SilentlyContinue }
    }

    $pass = @($script:Results | Where-Object { $_.Status -eq "PASS" }).Count
    $fail = @($script:Results | Where-Object { $_.Status -eq "FAIL" }).Count
    $skip = @($script:Results | Where-Object { $_.Status -eq "SKIP" }).Count
    $summary = "== $pass passed, $fail failed, $skip skipped"
    Write-Host $summary
    [void]$script:Log.AppendLine($summary)
    $os = [Environment]::OSVersion.VersionString
    $ver = (Invoke-Program $Omc @("--version") 10 $Root).Out.Trim()
    $header = "omc Windows test report`n$ver on $os, PowerShell $($PSVersionTable.PSVersion)`n$(Get-Date -Format u)`n`n"
    if (-not $Report) { $Report = Join-Path ([IO.Path]::GetTempPath()) "omc-windows-test-report.txt" }
    [IO.File]::WriteAllText($Report, $header + $script:Log.ToString())
    Write-Host "report: $Report"
    if ($fail -gt 0) { exit 1 }
}

Invoke-WindowsTests
