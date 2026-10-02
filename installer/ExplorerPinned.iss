; Inno Setup script for Explorer Pinned.
; Build: iscc /DAppVersion=1.0.0 /DBinDir=..\dist installer\ExplorerPinned.iss
; BinDir must contain x64\ExplorerPinned.exe and arm64\ExplorerPinned.exe.

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef BinDir
  #define BinDir "..\dist"
#endif

#define AppName "Explorer Pinned"
#define AppExe "ExplorerPinned.exe"
#define AppUrl "https://github.com/ORCA-IRUKA-222/explorer-pinned"

[Setup]
AppId={{5C0F3E1A-8D2B-4C47-9F3E-2B6A1D7C9E41}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=ORCA
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=..\dist
OutputBaseFilename=ExplorerPinned-Setup-{#AppVersion}
SetupIconFile=..\src\app.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
LicenseFile=..\LICENSE
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=no
ShowLanguageDialog=auto

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
japanese.StartNow=Explorer Pinned を今すぐ開始する
english.StartNow=Start Explorer Pinned now
japanese.Registering=「ピン止め」グループを登録しています...
english.Registering=Registering the "Pinned" group...

[Files]
Source: "{#BinDir}\x64\{#AppExe}"; DestDir: "{app}"; Check: not IsArm64; Flags: ignoreversion
Source: "{#BinDir}\arm64\{#AppExe}"; DestDir: "{app}"; Check: IsArm64; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"
Source: "..\README.md"; DestDir: "{app}"

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; Parameters: "agent"

[Run]
; Machine-wide part (needs administrator rights): the property behind the "Pinned" group.
Filename: "{app}\{#AppExe}"; Parameters: "register-schema --lang {language}"; StatusMsg: "{cm:Registering}"; Flags: runhidden waituntilterminated
; Per-user part: right-click menu entries and the start-up entry.
Filename: "{app}\{#AppExe}"; Parameters: "setup --quiet --no-agent"; Flags: runasoriginaluser runhidden waituntilterminated
Filename: "{app}\{#AppExe}"; Parameters: "agent"; Description: "{cm:StartNow}"; Flags: runasoriginaluser nowait postinstall

[UninstallRun]
Filename: "{app}\{#AppExe}"; Parameters: "uninstall --quiet"; RunOnceId: "RemoveRegistrations"; Flags: runhidden waituntilterminated

[UninstallDelete]
Type: files; Name: "{app}\ExplorerPinned.propdesc"

[Code]
// Stop a running agent before its executable is replaced.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  if FileExists(ExpandConstant('{app}\{#AppExe}')) then
    Exec(ExpandConstant('{app}\{#AppExe}'), 'exit', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Result := '';
end;
