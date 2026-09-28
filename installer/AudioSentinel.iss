; Inno Setup script for AudioSentinel.
; Build the Release|x64 exe first, then: ISCC.exe /DAppVersion=1.1.0 installer\AudioSentinel.iss

#ifndef AppVersion
  #define AppVersion "1.1.0"
#endif

[Setup]
AppId={{8C5E1A52-6B3F-4E0B-9E3A-2E0C7C1B7A11}
AppName=AudioSentinel
AppVersion={#AppVersion}
AppVerName=AudioSentinel {#AppVersion}
AppPublisher=Afaguayo
AppPublisherURL=https://github.com/Afaguayo/AudioSentinel
AppSupportURL=https://github.com/Afaguayo/AudioSentinel/issues
DefaultDirName={autopf}\AudioSentinel
DefaultGroupName=AudioSentinel
DisableProgramGroupPage=yes
; Per-user install by default: no admin prompt needed.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Setup asks the user to close a running AudioSentinel before upgrading.
AppMutex=Local\AudioSentinel.Instance
OutputDir=..\dist
OutputBaseFilename=AudioSentinel-Setup-{#AppVersion}
SetupIconFile=..\AudioSentinel\AudioSentinel.ico
UninstallDisplayIcon={app}\AudioSentinel.exe
UninstallDisplayName=AudioSentinel
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: "startup"; Description: "Start AudioSentinel automatically when I sign in"; GroupDescription: "Options:"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\x64\Release\AudioSentinel.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\AudioSentinel"; Filename: "{app}\AudioSentinel.exe"
Name: "{autodesktop}\AudioSentinel"; Filename: "{app}\AudioSentinel.exe"; Tasks: desktopicon

[Registry]
; Same value the app's "Start with Windows" menu item writes.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "AudioSentinel"; ValueData: """{app}\AudioSentinel.exe"" --startup"; Flags: uninsdeletevalue; Tasks: startup
; Always remove it on uninstall, even if it was turned on from the app.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "AudioSentinel"; Flags: uninsdeletevalue

[Run]
Filename: "{app}\AudioSentinel.exe"; Description: "Launch AudioSentinel"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM AudioSentinel.exe"; Flags: runhidden; RunOnceId: "StopAudioSentinel"

[UninstallDelete]
Type: filesandordirs; Name: "{localappdata}\AudioSentinel"
