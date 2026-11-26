; VanaCore's installer, made with Inno Setup 6 (free: jrsoftware.org). One setup exe with the whole package in it:
;
;   iscc installer\vanacore.iss /DVersion=0.9.9 [/DSource=<package folder>] [/DApp=<folder with VanaCoreApp.exe/.dll>]
;
; Source defaults to build\package\VanaCore (build-release.bat's); App to app\build\Release, used when the package hasn't
; VanaCoreApp itself (release.yaml's bundle_app.ps1 puts the released one in). The setup lands in build\installer.
;
; Per user, no administrator rights: VanaCore writes its settings and logs beside itself, so it goes in the player's own
; %LOCALAPPDATA%\Programs\VanaCore (or a folder they pick), never Program Files. Installing over an install updates it: the
; player's scripts\boot.txt, vanacore.ini, profiles, config\ and logs\ are never touched, and uninstalling leaves them too.

#ifndef Version
  #define Version "0.0.0"
#endif
#ifndef Source
  #define Source "..\build\package\VanaCore"
#endif
#ifndef App
  #define App "..\app\build\Release"
#endif

[Setup]
AppId={{6B0C5F57-4A8E-4C1B-9E55-6A1F3C2D7E10}
AppName=VanaCore
AppVersion={#Version}
AppVerName=VanaCore {#Version}
AppPublisher=VanaCore
AppPublisherURL=https://github.com/CatsAndBoats/VanaCore
DefaultDirName={autopf}\VanaCore
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\build\installer
OutputBaseFilename=VanaCore-{#Version}-Setup
SetupIconFile=..\app\res\VanaCore.ico
UninstallDisplayIcon={app}\VanaCoreApp.exe
UninstallDisplayName=VanaCore
WizardStyle=modern
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x86compatible
MinVersion=6.1sp1

[Tasks]
Name: desktopicon; Description: "Desktop shortcut"

[Files]
; the package, less the player's own boot.txt and the debug map
Source: "{#Source}\*"; DestDir: "{app}"; Excludes: "\scripts\boot.txt,*.map,*.pdb"; Flags: recursesubdirs createallsubdirs ignoreversion
; the default boot.txt, only when there's none (it's the player's from then on, and stays when VanaCore's uninstalled)
Source: "{#Source}\scripts\boot.txt"; DestDir: "{app}\scripts"; Flags: onlyifdoesntexist uninsneveruninstall
#if !FileExists(AddBackslash(Source) + "VanaCoreApp.exe")
Source: "{#App}\VanaCoreApp.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#App}\VanaCoreApp.dll"; DestDir: "{app}"; Flags: ignoreversion
#endif

[Icons]
Name: "{autoprograms}\VanaCore"; Filename: "{app}\VanaCoreApp.exe"; WorkingDir: "{app}"; Comment: "Start FINAL FANTASY XI with VanaCore"
Name: "{autodesktop}\VanaCore"; Filename: "{app}\VanaCoreApp.exe"; WorkingDir: "{app}"; Comment: "Start FINAL FANTASY XI with VanaCore"; Tasks: desktopicon

[Run]
Filename: "{app}\VanaCoreApp.exe"; Description: "Open VanaCore"; Flags: nowait postinstall skipifsilent
