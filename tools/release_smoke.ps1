# Release smoke test (P12 12.5, docs/phases/P12 + tools/release_checklist.md): install -> record -> project -> export
# -> uninstall -> leftover check, all headless and per-user. Run after `build.cmd package`:
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release_smoke.ps1 [-Installer out\dist\super4vidSetup-x.y.z.exe]
#              [-Seconds 5] [-Tasks desktopicon,startup] [-KeepTemp]
#
# What it does:
#   1. refuses to run when super4vid is already installed (per-user or machine-wide) or its association / Run value /
#      shortcuts exist, so it never overwrites or removes someone else's install;
#   2. installs silently and per user (/CURRENTUSER) into a fresh folder under %TEMP% (/DIR), with the optional tasks
#      (desktop icon + start with Windows) so their removal is checked too;
#   3. checks the installed files, the HKCU .ctproj association, the Run value and the shortcuts;
#   4. runs the installed exe headless: --record --seconds N --verify --synthetic-camera --out <temp>;
#   5. imports that recording into a project, saves it as .ctproj and exports it to MP4 (export_demo --recording from
#      the build tree: the installed app has no headless export mode);
#   6. opens the saved .ctproj through the registered association command (+ --screenshot, which closes the editor
#      after a few frames) to prove the double-click path launches the installed editor;
#   7. starts the installed exe with --tray plus a decoy super4vid.exe from the build tree (-TrayCheck $false skips
#      this), then ALWAYS (finally) uninstalls silently and waits for the uninstaller's second phase; the tray
#      instance must be ended by the uninstaller and the decoy must survive (it is then ended by this script);
#   8. reports what remains: install dir, HKCU/HKLM keys, shortcuts, Inno temp folders; user data folders
#      (%APPDATA%\super4vid, %LOCALAPPDATA%\super4vid, Videos\super4vid) are allowed to remain and are only listed.
# Every child process has a timeout and is killed (with its tree) when it runs over. Launches use CT_INSTANCE_SUFFIX
# so they never forward to (or block on) another super4vid instance on the machine. Exit code 0 = all checks passed.
param(
    [string]$Installer = "",
    [string]$ExportDemo = "",
    [double]$Seconds = 5,
    [string]$Tasks = "desktopicon,startup",
    [string]$Preset = "mp4-1080p",
    [bool]$TrayCheck = $true,
    [switch]$KeepTemp
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2
$repo = Split-Path $PSScriptRoot -Parent
$ver = (Select-String -Path "$repo\CMakeLists.txt" -Pattern 'project\(super4vid VERSION ([0-9.]+)').Matches[0].Groups[1].Value
if (-not $Installer) { $Installer = "$repo\out\dist\super4vidSetup-$ver.exe" }
if (-not $ExportDemo) { $ExportDemo = "$repo\out\build\ninja-release\bin\export_demo.exe" }
$Installer = (Resolve-Path $Installer).Path
$ExportDemo = (Resolve-Path $ExportDemo).Path

$appId = "{B7E0C9F2-5C1C-4D5B-9B7A-2F3C4D5E6F70}_is1"
$uninstKeys = @("HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$appId",
                "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$appId",
                "HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\$appId")
$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
$classKeys = @("HKCU:\Software\Classes\.ctproj", "HKCU:\Software\Classes\super4vid.Project",
               "HKLM:\Software\Classes\super4vid.Project")
$startMenu = Join-Path ([Environment]::GetFolderPath("Programs")) "super4vid.lnk"
$desktop = Join-Path ([Environment]::GetFolderPath("Desktop")) "super4vid.lnk"
$commonStartMenu = Join-Path ([Environment]::GetFolderPath("CommonPrograms")) "super4vid.lnk"
$userData = @("$env:APPDATA\super4vid", "$env:LOCALAPPDATA\super4vid", (Join-Path ([Environment]::GetFolderPath("MyVideos")) "super4vid"))

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$tmp = Join-Path $env:TEMP "super4vid-smoke-$stamp-$PID"
$appDir = "$tmp\app"; $recDir = "$tmp\rec"; $outDir = "$tmp\out"
$logDir = "$repo\out\smoke"
New-Item -ItemType Directory -Force $tmp, $recDir, $outDir, $logDir | Out-Null
$logFile = "$logDir\release_smoke-$stamp.log"
$script:failures = New-Object System.Collections.Generic.List[string]
$script:leftovers = New-Object System.Collections.Generic.List[string]

function Say([string]$msg) { $line = "[smoke] $msg"; Write-Host $line; Add-Content -Path $logFile -Value $line -Encoding UTF8 }
function Check([bool]$ok, [string]$what) {
    if ($ok) { Say "  ok    $what" } else { Say "  FAIL  $what"; $script:failures.Add($what) }
}
function Kill-Tree([int]$id) { & "$env:SystemRoot\System32\taskkill.exe" /pid $id /t /f 2>&1 | Out-Null }

# Runs a process with a timeout; stdout/stderr go to files under $tmp. Returns @{ code; out; timed_out }.
function Run-Timed([string]$exe, [string[]]$argv, [int]$timeoutS, [string]$name) {
    $so = "$tmp\$name.stdout.txt"; $se = "$tmp\$name.stderr.txt"
    $argLine = ($argv | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
    Say "run ($timeoutS s): $exe $argLine"
    $p = Start-Process -FilePath $exe -ArgumentList $argLine -PassThru -NoNewWindow -RedirectStandardOutput $so -RedirectStandardError $se
    $h = $p.Handle   # keep the handle so ExitCode is available after exit
    $timedOut = -not $p.WaitForExit($timeoutS * 1000)
    if ($timedOut) { Say "  timeout after $timeoutS s, killing pid $($p.Id)"; Kill-Tree $p.Id; $p.WaitForExit(10000) | Out-Null }
    $p.WaitForExit()
    $out = ""; if (Test-Path $so) { $out = [IO.File]::ReadAllText($so) }
    if ((Test-Path $se) -and (Get-Item $se).Length -gt 0) { $out += [IO.File]::ReadAllText($se) }
    foreach ($l in ($out -split "`r?`n")) { if ($l) { Add-Content -Path $logFile -Value "    | $l" -Encoding UTF8 } }
    $code = if ($timedOut) { -1 } else { $p.ExitCode }
    Say "  exit $code"
    return @{ code = $code; out = $out; timed_out = $timedOut }
}

function Get-RunValue { (Get-ItemProperty -Path $runKey -Name "super4vid" -ErrorAction SilentlyContinue) }

function Uninstall-Smoke {
    $unins = "$appDir\unins000.exe"
    if (-not (Test-Path $unins)) {
        $k = Get-ItemProperty -Path $uninstKeys[0] -ErrorAction SilentlyContinue
        if ($k -and $k.InstallLocation -and ($k.InstallLocation.TrimEnd('\') -ieq $appDir)) {
            $unins = ($k.UninstallString -replace '"', '')
        }
    }
    if (-not (Test-Path $unins)) { Say "uninstall: no uninstaller in $appDir (install did not get that far)"; return }
    Say "uninstall"
    $r = Run-Timed $unins @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/LOG=$tmp\uninstall.log") 180 "uninstall"
    Check ($r.code -eq 0) "uninstaller exit code 0"
    # unins000.exe hands over to a temp copy (second phase) that deletes the folder and itself; wait for it.
    $deadline = (Get-Date).AddSeconds(60)
    while ((Test-Path $appDir) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline -and (Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like "_iu*" })) {
        Start-Sleep -Milliseconds 500
    }
}

function Find-Leftovers {
    Say "leftover check"
    if (Test-Path $appDir) {
        $items = Get-ChildItem -Recurse -Force $appDir | ForEach-Object { $_.FullName }
        $script:leftovers.Add("install dir $appDir remains: " + ($items -join ", "))
    }
    foreach ($k in $uninstKeys + $classKeys) { if (Test-Path $k) { $script:leftovers.Add("registry key $k") } }
    $ext = Get-ItemProperty -Path "HKCU:\Software\Classes\.ctproj" -ErrorAction SilentlyContinue
    if ($ext -and $ext.PSObject.Properties["(default)"]) { $script:leftovers.Add("HKCU .ctproj default value '$($ext.'(default)')'") }
    if (Get-RunValue) { $script:leftovers.Add("Run value HKCU\...\Run\super4vid = '$((Get-RunValue).super4vid)'") }
    foreach ($lnk in $startMenu, $desktop, $commonStartMenu) { if (Test-Path $lnk) { $script:leftovers.Add("shortcut $lnk") } }
    # Windows' own per-extension bookkeeping (created only when Explorer opened a .ctproj; not ours to delete).
    $fe = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.ctproj"
    if (Test-Path $fe) { Say "  note  $fe exists (Explorer's per-user bookkeeping, created by Explorer, not by the installer)" }
    # Inno Setup 6.3+ runs the uninstaller from a copy in %TEMP%\is-XXXX-uninstall.tmp\_unins.tmp, which cannot
    # delete itself; the next Inno uninstall of any product cleans these up ("Cleaning up any residual temporary
    # files from previous Uninstall runs"). Not a product leftover: the folder this run created is noted and removed.
    $tempLeft = Get-ChildItem -Force $env:TEMP -Directory -ErrorAction SilentlyContinue |
        Where-Object { ($_.Name -like "is-*.tmp" -or $_.Name -like "_iu*") -and $_.CreationTime -ge $script:started }
    foreach ($t in $tempLeft) {
        $names = @(Get-ChildItem -Force -Recurse $t.FullName | ForEach-Object { $_.Name })
        $innoOnly = $t.Name -like "is-*-uninstall.tmp" -and @($names | Where-Object { $_ -notin "_unins.tmp", "_unins-done.tmp" }).Count -eq 0
        if ($innoOnly) {
            Say "  note  Inno uninstaller self-copy $($t.FullName) ($($names -join ', ')) - removed by this script"
            Remove-Item -Recurse -Force $t.FullName -ErrorAction SilentlyContinue
        } else {
            $script:leftovers.Add("temp folder $($t.FullName): $($names -join ', ')")
        }
    }
    foreach ($d in $userData) {
        if (Test-Path $d) {
            $n = (Get-ChildItem -Recurse -Force -File $d -ErrorAction SilentlyContinue | Measure-Object).Count
            $was = if ($script:userDataBefore[$d]) { "existed before" } else { "created by this run" }
            Say "  kept  user data $d ($n files, $was) - allowed"
        }
    }
    if ($script:leftovers.Count -eq 0) { Say "  ok    nothing left except user data" }
    foreach ($l in $script:leftovers) { Say "  LEFT  $l" }
}

# ---------------------------------------------------------------------------------------------------------------
$script:started = Get-Date
Say "super4vid $ver release smoke test, $(Get-Date -Format s), log $logFile"
Say "installer  $Installer ($([math]::Round((Get-Item $Installer).Length / 1MB, 2)) MB)"
Say "export     $ExportDemo"
Say "temp       $tmp"
$os = Get-CimInstance Win32_OperatingSystem
Say "os         $($os.Caption) $($os.Version) build $($os.BuildNumber)"

# 1. preconditions: never touch an existing install, association, Run value or shortcut
$pre = New-Object System.Collections.Generic.List[string]
foreach ($k in $uninstKeys + $classKeys) { if (Test-Path $k) { $pre.Add($k) } }
if (Get-RunValue) { $pre.Add("$runKey\super4vid") }
foreach ($lnk in $startMenu, $desktop, $commonStartMenu) { if (Test-Path $lnk) { $pre.Add($lnk) } }
if ($pre.Count -gt 0) {
    Say "ABORT: super4vid is already installed or registered on this machine; uninstall it first: $($pre -join '; ')"
    exit 2
}
$script:userDataBefore = @{}
foreach ($d in $userData) { $script:userDataBefore[$d] = (Test-Path $d) }

$suffix = "smoke-$PID"
$oldSuffix = $env:CT_INSTANCE_SUFFIX
$env:CT_INSTANCE_SUFFIX = $suffix
$exe = "$appDir\super4vid.exe"
$script:trayProc = $null; $script:decoyProc = $null
try {
    # 2. install
    Say "install (per user) into $appDir, tasks '$Tasks'"
    $r = Run-Timed $Installer @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CURRENTUSER", "/DIR=$appDir",
                                "/TASKS=$Tasks", "/LOG=$tmp\install.log") 180 "install"
    Check ($r.code -eq 0) "installer exit code 0"

    # 3. installed state
    Check (Test-Path $exe) "super4vid.exe installed"
    Check (Test-Path "$appDir\LICENSE.txt") "LICENSE.txt installed"
    Check (Test-Path "$appDir\licenses\imgui-LICENSE.txt") "licenses\imgui-LICENSE.txt installed"
    Check (Test-Path "$appDir\unins000.exe") "uninstaller present"
    Check (Test-Path $uninstKeys[0]) "per-user uninstall entry (HKCU)"
    Check (-not (Test-Path $uninstKeys[1]) -and -not (Test-Path $uninstKeys[2])) "no machine-wide uninstall entry (HKLM)"
    $assoc = (Get-ItemProperty -Path "HKCU:\Software\Classes\.ctproj" -ErrorAction SilentlyContinue)
    Check ($assoc -and $assoc.'(default)' -eq "super4vid.Project") "HKCU .ctproj -> super4vid.Project"
    $cmdKey = Get-ItemProperty -Path "HKCU:\Software\Classes\super4vid.Project\shell\open\command" -ErrorAction SilentlyContinue
    $openCmd = if ($cmdKey) { $cmdKey.'(default)' } else { "" }
    Check ($openCmd -eq "`"$exe`" --editor `"%1`"") "open command = '$openCmd'"
    $run = Get-RunValue
    if ($Tasks -match "startup") { Check ($run -and $run.super4vid -eq "`"$exe`" --tray") "Run value = '$(if ($run) { $run.super4vid })'" }
    Check (Test-Path $startMenu) "Start menu shortcut $startMenu"
    if ($Tasks -match "desktopicon") { Check (Test-Path $desktop) "desktop shortcut $desktop" }
    if (Test-Path $exe) {
        $vi = (Get-Item $exe).VersionInfo
        Check ($vi.ProductVersion -like "$ver*") "installed exe version $($vi.ProductVersion), $([math]::Round((Get-Item $exe).Length / 1MB, 2)) MB"
    }

    # 4. headless recording with the installed exe
    $bundle = $null
    if (Test-Path $exe) {
        $r = Run-Timed $exe @("--record", "--seconds", "$Seconds", "--verify", "--synthetic-camera", "--out", $recDir) ([int]($Seconds + 90)) "record"
        Check ($r.code -eq 0) "installed exe --record --verify exit code 0"
        Check ($r.out -match "verify: PASS") "bundle verification PASS"
        $m = [regex]::Match($r.out, "bundle: (.+)")
        if ($m.Success) { $bundle = $m.Groups[1].Value.Trim() }
        if (-not $bundle -or -not (Test-Path $bundle)) {
            $bundle = Get-ChildItem -Directory $recDir -Filter *.ctrec | Select-Object -First 1 | ForEach-Object { $_.FullName }
        }
        Check ([bool]$bundle -and (Test-Path "$bundle\meta.json")) "recording bundle $bundle"
    }

    # 5. project + export
    $proj = "$outDir\smoke.ctproj"; $mp4 = "$outDir\smoke.mp4"
    if ($bundle -and (Test-Path "$bundle\meta.json")) {
        $r = Run-Timed $ExportDemo @("--recording", $bundle, "--save-project", $proj, "--preset", $Preset, "--out", $mp4,
                                     "--normal-priority") 300 "export"
        Check ($r.code -eq 0 -and $r.out -match "(?m)^done:") "export_demo import + export exit code 0"
        Check (Test-Path $proj) "project saved $proj"
        $okMp4 = (Test-Path $mp4) -and (Get-Item $mp4).Length -gt 10000
        if ($okMp4) {
            $head = New-Object byte[] 12
            $fs = [IO.File]::OpenRead($mp4); [void]$fs.Read($head, 0, 12); $fs.Close()
            $okMp4 = [Text.Encoding]::ASCII.GetString($head, 4, 4) -eq "ftyp"
        }
        Check $okMp4 "MP4 written ($(if (Test-Path $mp4) { [math]::Round((Get-Item $mp4).Length / 1KB) } else { 0 }) KB, ftyp box)"
        $fm = [regex]::Match($r.out, "frames (\d+) in")
        if ($fm.Success) { Say "  info  exported $($fm.Groups[1].Value) frames" }
    }

    # 6. the association's command opens the saved project in the installed editor
    if ((Test-Path $proj) -and $openCmd) {
        $shot = "$outDir\editor.png"
        $line = $openCmd.Replace("%1", $proj)
        $m = [regex]::Match($line, '^"([^"]+)"\s*(.*)$')
        $argv = @([regex]::Matches($m.Groups[2].Value, '"([^"]*)"|(\S+)') | ForEach-Object { if ($_.Groups[1].Success) { $_.Groups[1].Value } else { $_.Groups[2].Value } })
        $r = Run-Timed $m.Groups[1].Value ($argv + @("--screenshot", $shot, "--frames", "90")) 90 "editor"
        Check ($r.code -eq 0 -and (Test-Path $shot)) "association command opens the project in the editor (screenshot $shot)"
    }

    # 7a. uninstall while the installed app runs in the tray; a second super4vid.exe from another folder (the build
    # tree, own instance suffix) must survive: the uninstaller may end only processes started from {app}.
    if ($TrayCheck -and (Test-Path $exe)) {
        Say "start the installed exe with --tray (must be ended by the uninstaller)"
        $script:trayProc = Start-Process -FilePath $exe -ArgumentList "--tray" -PassThru
        $buildExe = "$repo\out\build\ninja-release\bin\super4vid.exe"
        if (Test-Path $buildExe) {
            $env:CT_INSTANCE_SUFFIX = "$suffix-decoy"
            Say "start a decoy $buildExe --tray (must survive the uninstall)"
            $script:decoyProc = Start-Process -FilePath $buildExe -ArgumentList "--tray" -PassThru
            $env:CT_INSTANCE_SUFFIX = $suffix
        }
        Start-Sleep -Seconds 3
        Check (-not $script:trayProc.HasExited) "installed --tray instance running (pid $($script:trayProc.Id))"
        if ($script:decoyProc) { Check (-not $script:decoyProc.HasExited) "decoy instance running (pid $($script:decoyProc.Id))" }
    }
} catch {
    Say "ERROR: $($_.Exception.Message) at $($_.InvocationInfo.PositionMessage)"
    $script:failures.Add("script error: $($_.Exception.Message)")
} finally {
    # 7. always uninstall
    try { Uninstall-Smoke } catch { Say "ERROR during uninstall: $($_.Exception.Message)"; $script:failures.Add("uninstall error") }
    if ($script:trayProc) {
        $ended = $script:trayProc.WaitForExit(5000)
        Check $ended "installed --tray instance ended by the uninstaller"
        if (-not $ended) { Kill-Tree $script:trayProc.Id }
    }
    if ($script:decoyProc) {
        Check (-not $script:decoyProc.HasExited) "decoy instance from another folder survived the uninstall"
        if (-not $script:decoyProc.HasExited) { Kill-Tree $script:decoyProc.Id }
    }
    $env:CT_INSTANCE_SUFFIX = $oldSuffix
    # 8. leftovers
    try { Find-Leftovers } catch { Say "ERROR during leftover check: $($_.Exception.Message)"; $script:failures.Add("leftover check error") }
    foreach ($f in "install.log", "uninstall.log") { if (Test-Path "$tmp\$f") { Copy-Item "$tmp\$f" "$logDir\release_smoke-$stamp-$f" } }
    if (-not $KeepTemp) { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue } else { Say "temp kept: $tmp" }
}

foreach ($l in $script:leftovers) { $script:failures.Add("leftover: $l") }
if ($script:failures.Count -eq 0) { Say "RESULT: PASS"; exit 0 }
Say "RESULT: FAIL ($($script:failures.Count)): $($script:failures -join ' | ')"
exit 1
