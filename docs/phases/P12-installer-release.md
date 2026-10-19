# P12 · Installer and release

**Goal:** a signed-ready Inno Setup installer, a portable ZIP, archived symbols, a CI workflow that builds/tests/
packages on every push, and a release checklist executed on clean machines.

**Read first:** `docs/01-toolchain-and-build.md` sections 8, 9, 11; `docs/06` section 7.
**Depends on:** P11. **Size:** S (about 400 lines of scripts).

## Deliverables

```
installer/super4vid.iss  installer/LICENSE.txt (product license text chosen by the owner; placeholder "All rights reserved" until then)
tools/package.ps1  tools/release_checklist.md  .github/workflows/build.yml
src/app/app.rc (final strings)  assets/icon.ico (final)
```

## 12.1 Version stamping
- Bump `project(super4vid VERSION x.y.z)` in the root `CMakeLists.txt`; everything else derives from it (`version.h`, `app.rc`, installer name, zip names, About dialog).
- `app.rc` strings: `FileDescription "super4vid Screen Recorder and Editor"`, `ProductName "super4vid"`, `CompanyName` (owner), `LegalCopyright "Copyright (C) 2026 <owner>"`, `OriginalFilename "super4vid.exe"`, versions from `CT_VERSION_*`.
- Add the `--tray` mode (start hidden to the tray; used by the optional "start with Windows" task) if not already present.

## 12.2 Inno Setup script (`installer/super4vid.iss`)

```iss
#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif
#define MyAppName      "super4vid"
#define MyAppExeName   "super4vid.exe"
#define MyAppPublisher "super4vid"

[Setup]
AppId={{B7E0C9F2-5C1C-4D5B-9B7A-2F3C4D5E6F70}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\out\dist
OutputBaseFilename=super4vidSetup-{#MyAppVersion}
SetupIconFile=..\assets\icon.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ChangesAssociations=yes
CloseApplications=yes
VersionInfoVersion={#MyAppVersion}
LicenseFile=LICENSE.txt
; Code signing (optional): configure a sign tool named "signtool" in ISCC with
;   /S"signtool=signtool.exe sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /a $f"
; then uncomment:
; SignTool=signtool
; SignedUninstaller=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "startup";     Description: "Start {#MyAppName} in the tray when Windows starts"; Flags: unchecked

[Files]
Source: "..\out\build\ninja-release\bin\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\third_party\imgui\LICENSE.txt"; DestDir: "{app}\licenses"; DestName: "imgui-LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}";  Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.ctproj"; ValueType: string; ValueName: ""; ValueData: "super4vid.Project"; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\super4vid.Project"; ValueType: string; ValueName: ""; ValueData: "super4vid Project"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\super4vid.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"
Root: HKA; Subkey: "Software\Classes\super4vid.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" --editor ""%1"""
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "super4vid"; ValueData: """{app}\{#MyAppExeName}"" --tray"; Tasks: startup; Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/im {#MyAppExeName} /f"; Flags: runhidden; RunOnceId: "Killsuper4vid"
```
User data (`%APPDATA%\super4vid`, `%LOCALAPPDATA%\super4vid`, `Videos\super4vid`) is intentionally never deleted by the uninstaller.

## 12.3 `tools/package.ps1`
```
param([string]$Config = "ninja-release")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$ver  = (Select-String -Path "$root\CMakeLists.txt" -Pattern 'project\(super4vid VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$bin  = "$root\out\build\$Config\bin"; $dist = "$root\out\dist"; New-Item -ItemType Directory -Force $dist | Out-Null
if (-not (Test-Path "$bin\super4vid.exe")) { throw "Build first: build.cmd release" }
# portable zip
$port = "$dist\portable"; Remove-Item -Recurse -Force $port -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force $port | Out-Null
Copy-Item "$bin\super4vid.exe" $port; Copy-Item "$root\installer\LICENSE.txt" $port; Set-Content "$port\portable.txt" "super4vid portable mode: data is stored next to the exe."
Compress-Archive -Path "$port\*" -DestinationPath "$dist\super4vid-$ver-portable.zip" -Force
# symbols
Compress-Archive -Path "$bin\super4vid.pdb" -DestinationPath "$dist\super4vid-$ver-symbols.zip" -Force
# installer
$iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"; if (-not (Test-Path $iscc)) { throw "Inno Setup 6 not found (winget install JRSoftware.InnoSetup)" }
& $iscc "/DMyAppVersion=$ver" "$root\installer\super4vid.iss"; if ($LASTEXITCODE -ne 0) { throw "ISCC failed" }
# checksums
Get-ChildItem $dist -File | Where-Object { $_.Extension -in ".exe", ".zip" } | ForEach-Object { "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash)  $($_.Name)" } | Set-Content "$dist\SHA256SUMS.txt"
Get-ChildItem $dist -File | Format-Table Name, @{n="MB";e={[math]::Round($_.Length/1MB,2)}}
```
Budget check printed at the end: exe < 20 MB, installer < 15 MB.

## 12.4 CI
`.github/workflows/build.yml` exactly as `docs/01` section 11. Artifacts: `out/dist/*`. Tag pushes (`v*`) additionally create a GitHub release with the dist files and `SHA256SUMS.txt` (`softprops/action-gh-release@v2`). Device tests are skipped in CI through `CT_CI=1`; unit, fuzz-lite and GIF tests run.

## 12.5 Release checklist (`tools/release_checklist.md`, copy per release)
1. `PROGRESS.md`: every phase ticked, section-8 numbers present for this commit.
2. Bump version, update `CHANGELOG.md` (keep-a-changelog format), commit, tag `vX.Y.Z`.
3. `build.cmd clean && build.cmd test && build.cmd package`; sizes within budget; `SHA256SUMS.txt` generated.
4. Install on a clean Windows 11 machine and a Windows 10 22H2 VM: first run toast, F9 records 10 s with mic + system, bundle opens in the editor via the saved toast, add a callout, auto-zoom present, export MP4 and GIF, open outputs in Movies & TV / a browser; `.ctproj` double-click opens the editor; uninstall leaves only user data.
5. Crash dump sanity: `super4vid.exe --crash-test` on the installed build; open the dump with the symbols zip; stack resolves.
6. Virus-scan the installer (Windows Defender full scan of `out/dist`), optionally submit to the Microsoft Defender portal to reduce SmartScreen friction for unsigned builds.
7. Publish: GitHub release (automated on tag) with notes; keep the symbols zip private if the product is closed-source.

## Acceptance (12.5)
Fresh install -> record -> edit -> export -> uninstall on both test machines without errors; installer and portable zip sizes within budget; CI green on the tagged commit.

## Pitfalls
- `PrivilegesRequired=lowest` installs per user under `%LOCALAPPDATA%\Programs` by default; `HKA` resolves to HKCU then, which is what the association needs.
- `CloseApplications=yes` cannot close a tray-only process reliably; the `[UninstallRun]` taskkill covers uninstall, and the app also exits when it receives `WM_QUERYENDSESSION` or `WM_CLOSE` from the installer's `WM_CLOSE` broadcast.
- Static CRT means no VC++ redistributable step; do not add one.
- SmartScreen will warn on unsigned installers; this is expected until a code-signing certificate is configured.
