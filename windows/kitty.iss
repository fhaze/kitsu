; Inno Setup script for the Windows Kitsu installer.
;
; Built by: python setup.py windows-package (when ISCC.exe is available), which
; runs iscc with the defines below pointing at the windows-package\kitsu tree.
;
; The installer is self contained and supports in-place upgrades: installing a
; newer version over an existing one replaces the old files, keeping the user's
; configuration (which lives outside {app}), tasks and install directory.

#ifndef AppVersion
  #error AppVersion must be defined, for example: iscc /DAppVersion=0.44.0 kitty.iss
#endif
#ifndef SourceDir
  #define SourceDir "..\windows-package\kitsu"
#endif
#ifndef OutputDir
  #define OutputDir "..\windows-package"
#endif
#ifndef Arch
  #define Arch "x86_64"
#endif
#ifndef AppURL
  #define AppURL "https://github.com/fhaze/kitsu"
#endif
#define AppName "Kitsu"
#define AppExe "bin\kitsu.exe"

[Setup]
; Never change the AppId, it is what lets a new installer upgrade an existing installation
AppId={{7D0F3A2B-5C4E-4F19-9B6A-2E8D1C3F5A70}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=FHaze
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
SetupIconFile=..\logo\kitsu.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
; Per-user installation by default (no UAC prompt), with the choice to install for all users
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
#if Arch == "arm64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#else
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif
; ConPTY needs Windows 10 1809
MinVersion=10.0.17763
OutputDir={#OutputDir}
OutputBaseFilename=kitsu-{#AppVersion}-windows-{#Arch}-setup
Compression=lzma2/max
SolidCompression=yes
LZMAUseSeparateProcess=yes
WizardStyle=modern
; Ask to close running kitty instances so their files can be replaced during an upgrade
CloseApplications=yes
CloseApplicationsFilter=*.exe,*.com,*.dll,*.pyd
RestartApplications=no
ChangesEnvironment=yes
UsePreviousAppDir=yes
UsePreviousTasks=yes
UsePreviousPrivileges=yes
#ifdef SignTool
; Authenticode signs setup.exe and unins000.exe. setup.py passes /DSignTool=kitsusign
; and the command as /Skitsusign=... when KITSU_SIGN_COMMAND is set, see windows/signing.py
SignTool={#SignTool}
SignedUninstaller=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "addtopath"; Description: "Add Kitsu and kitten to the &PATH"; GroupDescription: "Integration:"
Name: "wsl"; Description: "Install the kitten command into &WSL distributions and add it to their PATH"; GroupDescription: "Integration:"; Check: WslAvailable
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "terminalidentity"; Description: "Register Kitsu in Windows default &terminal settings (current user)"; GroupDescription: "Integration:"; Flags: unchecked; Check: TerminalIdentityAvailable

[InstallDelete]
; Remove shortcuts left by the old branding during an in-place upgrade.
Type: files; Name: "{autoprograms}\kitty.lnk"
Type: files; Name: "{autodesktop}\kitty.lnk"
; Remove the previous version before copying the new files, so that modules,
; DLLs and the bundled Python runtime that no longer exist do not linger
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\cmd"
Type: filesandordirs; Name: "{app}\etc"
Type: filesandordirs; Name: "{app}\lib"
Type: filesandordirs; Name: "{app}\share"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; AppUserModelID: "fhaze.Kitsu"; Comment: "The fast, feature-rich, GPU based terminal emulator"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\App Paths\kitsu.exe"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe}"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\App Paths\kitsu.exe"; ValueType: string; ValueName: "Path"; ValueData: "{app}\bin"
; Lets Win+R and the start menu run "kitty" without it being on the PATH
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\App Paths\kitty.exe"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe}"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\App Paths\kitty.exe"; ValueType: string; ValueName: "Path"; ValueData: "{app}\bin"

[Run]
Filename: "{app}\{#AppExe}"; Parameters: "+default-terminal register"; StatusMsg: "Registering Kitsu terminal identity..."; Flags: runhidden waituntilterminated; Tasks: terminalidentity
Filename: "{app}\{#AppExe}"; Parameters: "+wsl-setup"; StatusMsg: "Installing kitten into WSL distributions..."; Flags: runhidden waituntilterminated; Tasks: wsl
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\{#AppExe}"; Parameters: "+default-terminal unregister"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveTerminalIdentity"; Check: TerminalIdentityWasSelected
Filename: "{app}\{#AppExe}"; Parameters: "+wsl-setup --uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveWslKitten"; Check: WslTaskWasSelected

[UninstallDelete]
; Python byte code caches written at runtime
Type: filesandordirs; Name: "{app}\lib"

[Code]
function TerminalIdentityAvailable: Boolean;
begin
  #if FileExists(SourceDir + "\terminal-identity.msix")
    Result := (not IsAdminInstallMode) and (GetWindowsVersion >= $0A00585D);
  #else
    Result := False;
  #endif
end;

function TerminalIdentityWasSelected: Boolean;
begin
  Result := (not IsAdminInstallMode) and FileExists(ExpandConstant('{app}\terminal-identity.msix'));
end;

function WslAvailable: Boolean;
begin
  Result := FileExists(ExpandConstant('{sys}\wsl.exe'));
end;

function UninstallKey: String;
begin
  Result := ExpandConstant('Software\Microsoft\Windows\CurrentVersion\Uninstall\{#SetupSetting("AppId")}_is1');
end;

{ The tasks chosen at install time, read back by the uninstaller }
function WslTaskWasSelected: Boolean;
var
  Tasks: String;
begin
  Result := False;
  if RegQueryStringValue(HKA, UninstallKey, 'Inno Setup: Selected Tasks', Tasks) then
    Result := Pos(',wsl,', ',' + Lowercase(Tasks) + ',') > 0;
end;

{ PATH handling, per-user or per-machine depending on the install mode }

function EnvRootKey: Integer;
begin
  if IsAdminInstallMode then
    Result := HKLM
  else
    Result := HKCU;
end;

function EnvSubKey: String;
begin
  if IsAdminInstallMode then
    Result := 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment'
  else
    Result := 'Environment';
end;

{ Returns Paths (a ; separated list) without any entry equal to Dir, and whether Dir was present }
function RemoveDirFromPathList(Paths, Dir: String; var Found: Boolean): String;
var
  Entry, Rest: String;
  P: Integer;
begin
  Result := '';
  Found := False;
  Rest := Paths;
  while Rest <> '' do
  begin
    P := Pos(';', Rest);
    if P = 0 then
    begin
      Entry := Rest;
      Rest := '';
    end
    else
    begin
      Entry := Copy(Rest, 1, P - 1);
      Rest := Copy(Rest, P + 1, Length(Rest));
    end;
    if Entry = '' then
      continue;
    if CompareText(RemoveBackslash(Entry), RemoveBackslash(Dir)) = 0 then
    begin
      Found := True;
      continue;
    end;
    if Result = '' then
      Result := Entry
    else
      Result := Result + ';' + Entry;
  end;
end;

procedure AddDirToPath(Dir: String);
var
  Paths: String;
  Found: Boolean;
begin
  if not RegQueryStringValue(EnvRootKey, EnvSubKey, 'Path', Paths) then
    Paths := '';
  RemoveDirFromPathList(Paths, Dir, Found);
  if Found then
    exit;
  if (Paths <> '') and (Paths[Length(Paths)] <> ';') then
    Paths := Paths + ';';
  Paths := Paths + Dir;
  if not RegWriteExpandStringValue(EnvRootKey, EnvSubKey, 'Path', Paths) then
    Log('Failed to add ' + Dir + ' to the PATH');
end;

procedure RemoveDirFromPath(Dir: String);
var
  Paths: String;
  Found: Boolean;
begin
  if not RegQueryStringValue(EnvRootKey, EnvSubKey, 'Path', Paths) then
    exit;
  Paths := RemoveDirFromPathList(Paths, Dir, Found);
  if not Found then
    exit;
  if not RegWriteExpandStringValue(EnvRootKey, EnvSubKey, 'Path', Paths) then
    Log('Failed to remove ' + Dir + ' from the PATH');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    RemoveDirFromPath(ExpandConstant('{app}\bin'));
    if WizardIsTaskSelected('addtopath') then
      AddDirToPath(ExpandConstant('{app}\cmd'))
    else
      RemoveDirFromPath(ExpandConstant('{app}\cmd'));
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    RemoveDirFromPath(ExpandConstant('{app}\bin'));
    RemoveDirFromPath(ExpandConstant('{app}\cmd'));
  end;
end;
