; AIControls' installer (Inno Setup 6), built by tools\dist.cmd:
;
;   ISCC /DAppVersion=0.1.0 /DSourceDir=<folder with aicontrols.exe> /DOutputDir=<out> installer\aicontrols.iss
;
; UTF-8 with a BOM: Inno Setup reads the Russian strings right only then.
;
; Per user, no administrator rights: into %LOCALAPPDATA%\Programs\AIControls, a shortcut in the Start menu.
; The same file updates an installed AIControls (a running one is closed first). Uninstalling keeps the
; secrets and the settings in %APPDATA%\AIControls unless the user says otherwise; the chats' history lives
; in ~\.claude (the CLI's) and is never touched.

#ifndef AppVersion
  #error Pass /DAppVersion=MAJOR.MINOR.PATCH
#endif
#ifndef SourceDir
  #error Pass /DSourceDir=<the folder with aicontrols.exe>
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
; Never change AppId: it is how an update finds the installed AIControls.
AppId={{B7D2E4A1-3C95-4F68-A0E1-52D9C8F4B613}
AppName=AIControls
AppVersion={#AppVersion}
AppVerName=AIControls {#AppVersion}
AppPublisher=sovereignbrains
AppPublisherURL=https://github.com/sovereignbrains/aicontrols
AppSupportURL=https://github.com/sovereignbrains/aicontrols/issues
AppUpdatesURL=https://github.com/sovereignbrains/aicontrols/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\AIControls
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#OutputDir}
OutputBaseFilename=AIControls-Setup-{#AppVersion}
SetupIconFile=..\assets\aicontrols.ico
UninstallDisplayIcon={app}\aicontrols.exe
UninstallDisplayName=AIControls
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=force
RestartApplications=no

[Languages]
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\aicontrols.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\AIControls"; Filename: "{app}\aicontrols.exe"
Name: "{autodesktop}\AIControls"; Filename: "{app}\aicontrols.exe"; Tasks: desktopicon

[Run]
; Not in silent mode: an update started by hand should not pop a window up on its own.
Filename: "{app}\aicontrols.exe"; Description: "{cm:LaunchProgram,AIControls}"; Flags: postinstall nowait skipifsilent
; The client's own "update" button runs this silently with /RELAUNCH: it closed itself for it, so it comes back.
Filename: "{app}\aicontrols.exe"; Flags: nowait; Check: Relaunch

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
// Asked before anything goes: the secrets are the one thing that cannot be made again by a reinstall.
var
  DeleteData: Boolean;

function Relaunch: Boolean;
begin
  Result := WizardSilent and (Pos('/RELAUNCH', UpperCase(GetCmdTail)) > 0);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    if UninstallSilent then
      DeleteData := False
    else
      DeleteData := (MsgBox('Удалить также сохранённые секреты и настройки AIControls?' + #13#10#13#10 +
                     '«Нет» оставит их на случай переустановки. История чатов лежит в папке .claude и не затрагивается.',
                     mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES);
  if (CurUninstallStep = usPostUninstall) and DeleteData then
    DelTree(ExpandConstant('{userappdata}\AIControls'), True, True, True);
end;
