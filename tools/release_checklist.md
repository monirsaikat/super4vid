# Camtesia release checklist

Copy this file per release (`out/release-vX.Y.Z.md` or the release PR description) and tick each line. Spec:
`docs/phases/P12-installer-release.md` section 12.5, test matrix `docs/06-testing-and-performance.md` section 7.
Automated steps are marked **[auto]**; everything else needs a person.

Release: `vX.Y.Z` · commit: `________` · release manager: `________` · date: `________`

## 1. Preconditions
- [ ] `PROGRESS.md`: every phase ticked (or the open items listed in the release notes as known issues).
- [ ] Section-8 numbers in `PROGRESS.md` (cold start, idle CPU, recording CPU/GPU/RSS, dropped frames, editor fps,
      scrub latency, export speed, binary sizes) were measured on this commit or a commit with no relevant change.
- [ ] `master` is green in CI (`.github/workflows/build.yml`).

## 2. Version bump
- [ ] `project(Camtesia VERSION X.Y.Z)` in the root `CMakeLists.txt`. Nothing else is edited by hand: `version.h`,
      `app.rc`, the manifest, the installer name (`CamtesiaSetup-X.Y.Z.exe`), the zip names and the About dialog derive
      from it.
- [ ] `CHANGELOG.md` (keep-a-changelog format): new `## [X.Y.Z] - YYYY-MM-DD` section with Added / Changed / Fixed;
      the `[Unreleased]` section emptied.
- [ ] Commit `Release X.Y.Z`; the tag comes in step 8, after the checks pass.

## 3. Clean build, tests, package
- [ ] **[auto]** `build.cmd clean`
- [ ] **[auto]** `build.cmd test`: Debug and Release, every ctest suite passes (device tests may skip only in CI).
- [ ] **[auto]** `build.cmd package`: prints `budget: ... OK` (exe < 20 MB, installer < 15 MB); `out/dist/` holds
      `CamtesiaSetup-X.Y.Z.exe`, `Camtesia-X.Y.Z-portable.zip`, `Camtesia-X.Y.Z-symbols.zip`, `SHA256SUMS.txt`.
- [ ] `cd out\dist && sha256sum -c SHA256SUMS.txt` (Git Bash) reports OK for all three files.
- [ ] Sizes written here: exe ____ MB, installer ____ MB, portable zip ____ MB.

## 4. Code signing (placeholder until a certificate exists)
- [ ] No certificate yet: the build is unsigned and SmartScreen warns on first run; say so in the release notes.
- [ ] With a certificate: configure the ISCC sign tool (`/S"signtool=signtool.exe sign /fd SHA256 /tr
      http://timestamp.digicert.com /td SHA256 /a $f"`), uncomment `SignTool=signtool` and `SignedUninstaller=yes` in
      `installer/camtesia.iss`, sign `Camtesia.exe` before packaging, then check `signtool verify /pa /v` on
      `Camtesia.exe`, `CamtesiaSetup-X.Y.Z.exe` and the installed `unins000.exe`.

## 5. Automated install / record / export / uninstall smoke test
- [ ] **[auto]** `powershell -NoProfile -ExecutionPolicy Bypass -File tools\release_smoke.ps1` prints `RESULT: PASS`.
      It installs per user into a temp folder (`/CURRENTUSER /DIR=... /TASKS=desktopicon,startup`), checks the files,
      the HKCU `.ctproj` -> `Camtesia.Project` association, the Run value and the shortcuts, records 5 s headless with
      the installed exe (`--record --seconds 5 --verify --synthetic-camera`), imports the recording into a project,
      saves it as `.ctproj` and exports MP4 (`export_demo --recording`), opens the project through the registered
      open command, then always uninstalls and lists every leftover. The log is in `out\smoke\`. It refuses to run
      when Camtesia is already installed. Allowed leftovers: user data only (`%APPDATA%\Camtesia`,
      `%LOCALAPPDATA%\Camtesia`, `Videos\Camtesia`).
- [ ] Attach the `RESULT` line and the leftover section of the log to the release notes / PR.

## 6. Human checks: install and uninstall on clean machines
Run on **(a)** a clean Windows 11 machine (reference machine, 1080p + 4K mixed DPI) and **(b)** a clean Windows 10
22H2 VM (no GPU acceleration / WARP). A clean user profile: no `%APPDATA%\Camtesia`, `%LOCALAPPDATA%\Camtesia`.

| Check | Win 11 | Win 10 VM |
|---|---|---|
| Installer starts (SmartScreen "More info > Run anyway" if unsigned), license page, per-user install without UAC | [ ] | [ ] |
| "Install for all users" from the privileges dialog asks for UAC and installs to Program Files | [ ] | [ ] |
| First run: toast / bar appears, folders created, settings defaults, no prompts, fonts render | [ ] | [ ] |
| F9 records 10 s with mic + system audio; the saved toast appears | [ ] | [ ] |
| Win 10 VM: software encoder path records 720p30 without errors | n/a | [ ] |
| "Open in editor" from the toast opens the bundle in the editor | [ ] | [ ] |
| Add a callout; auto-zoom keys are present on the timeline | [ ] | [ ] |
| Export MP4 and GIF; the MP4 plays in Movies & TV (or Media Player), the GIF in a browser | [ ] | [ ] |
| Save the project; double-clicking the `.ctproj` in Explorer opens it in the editor (also while the bar runs) | [ ] | [ ] |
| `.ctproj` files show the Camtesia icon in Explorer | [ ] | [ ] |
| "Start with Windows" task: after sign-out/in Camtesia is in the tray; unticking it on reinstall removes it | [ ] | [ ] |
| Uninstall from Settings > Apps while Camtesia runs in the tray: the tray instance ends, no error | [ ] | [ ] |
| After uninstall: no program folder, no Start menu / desktop shortcut, `.ctproj` no longer opens with Camtesia, no Run entry (Task Manager > Startup apps), user data still present | [ ] | [ ] |
| Upgrade: install the previous release, then this one over it; settings and recordings kept | [ ] | [ ] |
| Portable zip: unzip to a folder, run; data goes to `<folder>\data\`, nothing in `%APPDATA%` | [ ] | [ ] |
| Windows 11 laptop with NVIDIA or Intel GPU: HW encoder detected, HEVC availability correct, loopback records | [ ] | n/a |

## 7. Crash dump and security
- [ ] `Camtesia.exe --crash-test` on the installed build writes `%LOCALAPPDATA%\Camtesia\crashes\crash-*.dmp`; open it
      in WinDbg / Visual Studio with the PDB from the symbols zip; the stack resolves to the faulting line.
- [ ] Windows Defender full scan of `out\dist` is clean (optionally submit the installer to the Microsoft Defender
      portal to reduce SmartScreen friction for unsigned builds).

## 8. Publish
- [ ] `git tag vX.Y.Z && git push origin vX.Y.Z`; the CI `release` job verifies `SHA256SUMS.txt` and creates the
      GitHub release with the dist files.
- [ ] Release notes: changelog section, known issues, "unsigned build" note if applicable, smoke test result.
- [ ] Symbols zip kept private if the product is closed-source (remove it from the public release assets).
- [ ] CI is green on the tagged commit.
