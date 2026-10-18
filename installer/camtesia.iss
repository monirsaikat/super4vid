; Camtesia installer (Inno Setup 6.3+). Built by tools/package.ps1, which passes /DMyAppVersion from the root
; CMakeLists.txt project(Camtesia VERSION x.y.z). Per-user by default (no UAC); "Install for all users" is offered
; in the privileges dialog. User data (%APPDATA%\Camtesia, %LOCALAPPDATA%\Camtesia, Videos\Camtesia) is never deleted.
#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif
#ifndef BinDir
  #define BinDir "..\out\build\ninja-release\bin"
#endif
#define MyAppName      "Camtesia"
#define MyAppExeName   "Camtesia.exe"
#define MyAppPublisher "Camtesia"

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
OutputBaseFilename=CamtesiaSetup-{#MyAppVersion}
SetupIconFile=..\assets\icon.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
ChangesAssociations=yes
CloseApplications=yes
VersionInfoVersion={#MyAppVersion}
VersionInfoProductName={#MyAppName}
VersionInfoDescription={#MyAppName} Setup
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
Source: "{#BinDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\third_party\imgui\LICENSE.txt"; DestDir: "{app}\licenses"; DestName: "imgui-LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}";  Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.ctproj"; ValueType: string; ValueName: ""; ValueData: "Camtesia.Project"; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\Camtesia.Project"; ValueType: string; ValueName: ""; ValueData: "Camtesia Project"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Camtesia.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"
Root: HKA; Subkey: "Software\Classes\Camtesia.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" --editor ""%1"""
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Camtesia"; ValueData: """{app}\{#MyAppExeName}"" --tray"; Tasks: startup; Flags: uninsdeletevalue
; Reinstall/upgrade with "startup" unchecked: remove the Run value an earlier install wrote.
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "Camtesia"; Flags: deletevalue; Check: not WizardIsTaskSelected('startup')

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

; No [UninstallRun] "taskkill /im Camtesia.exe": that ends every Camtesia.exe on the machine (portable copies, other
; builds, another user's per-user install). The [Code] below closes only processes started from this {app}.

[Code]
// Ends the Camtesia.exe processes whose image is {app}\Camtesia.exe (a tray-only instance does not react to
// CloseApplications). WMI Win32_Process; a failure is logged and the uninstall goes on (a file still in use is then
// left for deletion at the next reboot, as Inno does for any locked file).
procedure KillInstalledInstances();
var
  Locator, Services, Procs, P: Variant;
  I, Killed, Pid: Integer;
  Target, Path: String;
begin
  Target := Lowercase(ExpandConstant('{app}\{#MyAppExeName}'));
  Killed := 0;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Services := Locator.ConnectServer('.', 'root\CIMV2');
    Procs := Services.ExecQuery('SELECT ProcessId, ExecutablePath FROM Win32_Process WHERE Name = ''{#MyAppExeName}''');
    for I := 0 to Procs.Count - 1 do begin
      P := Procs.ItemIndex(I);
      if not VarIsNull(P.ExecutablePath) then begin
        Path := P.ExecutablePath;
        Pid := P.ProcessId;
        if Lowercase(Path) = Target then begin
          Log('Ending ' + Path + ' (pid ' + IntToStr(Pid) + ')');
          P.Terminate(0);
          Killed := Killed + 1;
        end;
      end;
    end;
  except
    Log('KillInstalledInstances failed: ' + GetExceptionMessage);
  end;
  if Killed > 0 then Sleep(1000);   // Terminate is asynchronous; let the image unlock before files are deleted
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then KillInstalledInstances();
end;
