; 零式 / Zore installer (Inno Setup 6).
;
; Flow: language picker -> license (must accept) -> install folder ->
; options (associate formats / default viewer, both checked) -> install -> finish.
; Uninstall runs the app's own cleanup first, then removes every file, the
; HKCU\Software\ZoreView tree and the %LOCALAPPDATA%\ZoreView data folder.

#define AppExe "zoreview.exe"
#define AppVersion "0.3.0"
#define AppId "{{6B1F0C42-6C1E-4E7A-9B2E-73B1A2C4D5E6}"

[Setup]
AppId={#AppId}
AppName={cm:AppName}
AppVersion={#AppVersion}
AppVerName={cm:AppName} {#AppVersion}
AppPublisher={cm:AppName}
DefaultDirName={localappdata}\Programs\Zore
DefaultGroupName={cm:AppName}
DisableProgramGroupPage=yes
DisableDirPage=no
LicenseFile=installer\EULA.txt
OutputDir=dist
OutputBaseFilename=Zore-{#AppVersion}-setup
SetupIconFile=src\app\zoreview.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={cm:AppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ShowLanguageDialog=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
RestartApplications=no
AllowNoIcons=yes
VersionInfoCompany=Zore
VersionInfoVersion={#AppVersion}
VersionInfoProductName={cm:AppName}
VersionInfoDescription={cm:AppName} Setup

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
chinesesimplified.AppName=零式
english.AppName=Zore
chinesesimplified.TaskAssoc=关联常见图片格式（双击图片即用零式打开）
english.TaskAssoc=Associate common image formats with Zore
chinesesimplified.TaskDefault=把零式设为默认看图软件
english.TaskDefault=Make Zore the default image viewer
chinesesimplified.TaskDesktop=创建桌面快捷方式
chinesesimplified.UninstallName=卸载零式
english.TaskDesktop=Create a desktop shortcut
english.UninstallName=Uninstall Zore

[Tasks]
Name: "assoc"; Description: "{cm:TaskAssoc}"; GroupDescription: "{cm:AppName}"; Flags: checkedonce
Name: "defaultassoc"; Description: "{cm:TaskDefault}"; GroupDescription: "{cm:AppName}"; Flags: checkedonce
Name: "desktopicon"; Description: "{cm:TaskDesktop}"; GroupDescription: "{cm:AppName}"; Flags: checkedonce

[Files]
Source: "build\Release\zoreview.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\zoreview_cli.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\ZoreViewThumb.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "NOTICE"; DestDir: "{app}"; DestName: "NOTICE.md"; Flags: ignoreversion
Source: "third_party\licenses\*"; DestDir: "{app}\THIRD_PARTY_LICENSES"; Flags: ignoreversion
Source: "docs\*.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{cm:AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\{cm:UninstallName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{cm:AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; Runtime values live under this key; creating it here lets the uninstaller
; delete the whole tree (values, subkeys and the associations backup).
Root: HKCU; Subkey: "Software\ZoreView"; Flags: uninsdeletekey

[Run]
Filename: "{app}\{#AppExe}"; Parameters: "--register-shell --quiet"; Flags: runhidden waituntilterminated
Filename: "{app}\{#AppExe}"; Parameters: "--register-assoc --quiet"; Tasks: assoc; Flags: runhidden waituntilterminated
Filename: "{app}\{#AppExe}"; Parameters: "--set-default-assoc --quiet"; Tasks: defaultassoc; Flags: runhidden waituntilterminated
Filename: "{app}\{#AppExe}"; Parameters: "--set-language zh --quiet"; Languages: chinesesimplified; Flags: runhidden waituntilterminated
Filename: "{app}\{#AppExe}"; Parameters: "--set-language en --quiet"; Languages: english; Flags: runhidden waituntilterminated
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{cm:AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Thumbnail handler, shell verbs and file associations go first; the app also
; removes its registry tree and the data folder.
Filename: "{app}\{#AppExe}"; Parameters: "--uninstall --quiet"; RunOnceId: "ZoreCleanup"; Flags: runhidden waituntilterminated

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
Type: filesandordirs; Name: "{localappdata}\ZoreView"

