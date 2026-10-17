@echo off
setlocal
set "ROOT=%~dp0"
set "CMD=%~1"
if "%CMD%"=="" set "CMD=release"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo [build] vswhere.exe not found - install VS 2022 Build Tools & exit /b 1)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -version [17.0^,18.0^) -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (echo [build] No VS installation with the C++ toolset found & exit /b 1)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo [build] vcvars64 failed & exit /b 1)

if /i "%CMD%"=="doctor"  goto :doctor
if /i "%CMD%"=="debug"   (set "PRESET=ninja-debug")   & goto :build
if /i "%CMD%"=="release" (set "PRESET=ninja-release") & goto :build
if /i "%CMD%"=="test"    goto :test
if /i "%CMD%"=="package" goto :package
if /i "%CMD%"=="clean"   (if exist "%ROOT%out" rmdir /s /q "%ROOT%out") & exit /b 0
echo usage: build.cmd [doctor^|debug^|release^|test^|package^|clean]
exit /b 2

:build
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
exit /b 0

:test
for %%p in (ninja-debug ninja-release) do (
  cmake --preset %%p || exit /b 1
  cmake --build --preset %%p || exit /b 1
  ctest --preset %%p --output-on-failure || exit /b 1
)
exit /b 0

:package
call "%~f0" release || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\package.ps1" || exit /b 1
exit /b 0

:doctor
echo VS:      %VSPATH%
where cl cmake ninja fxc rc mt 2>nul
cmake --version | findstr /i version
ninja --version
echo SDK:     %WindowsSDKVersion%
set "ISCC="
if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if defined ISCC (echo Inno:    OK  %ISCC%) else (echo Inno:    MISSING  ^(winget install JRSoftware.InnoSetup^))
exit /b 0
