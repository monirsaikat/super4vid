# Builds out/dist/: super4vidSetup-<ver>.exe, super4vid-<ver>-portable.zip, super4vid-<ver>-symbols.zip, SHA256SUMS.txt.
# Run through `build.cmd package` (release build first). The version comes from project(super4vid VERSION x.y.z).
param([string]$Config = "ninja-release")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$ver  = (Select-String -Path "$root\CMakeLists.txt" -Pattern 'project\(super4vid VERSION ([0-9.]+)').Matches[0].Groups[1].Value
if (-not $ver) { throw "version not found in CMakeLists.txt" }
$bin  = "$root\out\build\$Config\bin"; $dist = "$root\out\dist"; New-Item -ItemType Directory -Force $dist | Out-Null
if (-not (Test-Path "$bin\super4vid.exe")) { throw "Build first: build.cmd release" }
if (-not (Test-Path "$bin\super4vid.pdb")) { throw "super4vid.pdb missing next to the exe" }
Write-Host "[package] super4vid $ver from $bin"

$setupName = "super4vidSetup-$ver.exe"
$portName  = "super4vid-$ver-portable.zip"
$symName   = "super4vid-$ver-symbols.zip"
foreach ($n in $setupName, $portName, $symName, "SHA256SUMS.txt") { Remove-Item -Force "$dist\$n" -ErrorAction SilentlyContinue }

# portable zip: exe + licenses + portable.txt marker (data next to the exe)
$port = "$dist\portable"; Remove-Item -Recurse -Force $port -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$port\licenses" | Out-Null
Copy-Item "$bin\super4vid.exe" $port
Copy-Item "$root\installer\LICENSE.txt" $port
Copy-Item "$root\third_party\imgui\LICENSE.txt" "$port\licenses\imgui-LICENSE.txt"
Set-Content "$port\portable.txt" "super4vid portable mode: data is stored next to the exe."
Compress-Archive -Path "$port\*" -DestinationPath "$dist\$portName" -CompressionLevel Optimal -Force
Remove-Item -Recurse -Force $port

# symbols (keep private for closed-source releases)
Compress-Archive -Path "$bin\super4vid.pdb", "$bin\super4vid.exe" -DestinationPath "$dist\$symName" -CompressionLevel Optimal -Force

# installer: ISCC from a machine-wide (choco, CI) or per-user (winget) Inno Setup 6 install
$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
          "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 not found (winget install JRSoftware.InnoSetup)" }
& $iscc /Qp "/DMyAppVersion=$ver" "/DBinDir=$bin" "$root\installer\super4vid.iss"
if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }

# checksums (this version's artifacts only), LF line endings so `sha256sum -c SHA256SUMS.txt` works anywhere
$sums = $setupName, $portName, $symName | ForEach-Object { "$((Get-FileHash "$dist\$_" -Algorithm SHA256).Hash.ToLower())  $_`n" }
[IO.File]::WriteAllText("$dist\SHA256SUMS.txt", ($sums -join ""), (New-Object Text.UTF8Encoding($false)))

Get-ChildItem $dist -File | Where-Object { $_.Name -in $setupName, $portName, $symName, "SHA256SUMS.txt" } |
    Format-Table Name, @{n="MB";e={[math]::Round($_.Length/1MB,2)}} | Out-String | Write-Host

# budgets (docs/phases/P12): exe < 20 MB, installer < 15 MB
$exeMB   = (Get-Item "$bin\super4vid.exe").Length / 1MB
$setupMB = (Get-Item "$dist\$setupName").Length / 1MB
$ok = ($exeMB -lt 20) -and ($setupMB -lt 15)
Write-Host ("[package] budget: exe {0:N2} MB (< 20), installer {1:N2} MB (< 15): {2}" -f $exeMB, $setupMB, $(if ($ok) { "OK" } else { "OVER" }))
if (-not $ok) { throw "size budget exceeded" }
