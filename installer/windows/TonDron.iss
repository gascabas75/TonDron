#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif

#ifndef MyAppSource
  #define MyAppSource "dist\\bin"
#endif

#define MyAppName "Drift"
#define MyAppPublisher "CutWire Studios"
#define MyAppExeName "drift.exe"

[Setup]
; Never change AppId: it is what lets an installer upgrade an existing install
; in place instead of leaving two copies behind.
AppId={{1FC80696-7700-464A-8E35-CCBB3239EDFB}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppSupportURL=https://github.com/CutWire-Studios/Drift/issues
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma
SolidCompression=yes
WizardStyle=modern
; Path is relative to this script. Without these two, setup runs under the stock
; Inno icon and the Apps & Features entry falls back to a generic one.
SetupIconFile=..\..\resources\windows\drift.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
OutputDir=output
OutputBaseFilename=Drift-Setup-x64

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Recursive: alongside the exe and its Qt runtime this carries the bundled
; effects\, transitions\, effect-templates\ and audio-effects\ package
; directories, which the app resolves relative to the executable.
Source: "{#MyAppSource}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
