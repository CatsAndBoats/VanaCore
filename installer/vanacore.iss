; VanaCore's installer, made with Inno Setup 6 (free: jrsoftware.org). One setup exe with the whole package in it:
;
;   iscc installer\vanacore.iss /DVersion=0.9.9 [/DSource=<package folder>] [/DApp=<folder with VanaCoreApp.exe/.dll>]
;
; Source defaults to build\package\VanaCore (build-release.bat's); App to app\build\Release, used when the package hasn't
; VanaCoreApp itself (release.yaml's bundle_app.ps1 puts the released one in). The setup lands in build\installer.
;
; Per user, no administrator rights: VanaCore writes its settings and logs beside itself, so it goes in the player's own
; folder: VanaCore beside FINAL FANTASY XI (the registry's install, one folder up: ...\SquareEnix\VanaCore) when the player
; can write there, else %LOCALAPPDATA%\Programs\VanaCore; never Program Files (a folder the player can't write to is turned
; down). Installing over an install updates it, in its folder: the player's scripts\boot.txt, vanacore.ini, profiles,
; config\ and logs\ are never touched, and uninstalling leaves them too.
;
; Dark, on the App's backdrop (background.png: app\res\background.jpg past its border lines, darkened), the logo in the corner.

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
DefaultDirName={code:DefaultDir}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\build\installer
OutputBaseFilename=VanaCoreInstaller
SetupIconFile=..\app\res\VanaCore.ico
UninstallDisplayIcon={app}\VanaCoreApp.exe
UninstallDisplayName=VanaCore
WizardStyle=modern dark hidebevels
WizardBackImageFile=background.png
WizardBackColor=#0B1220
WizardSmallImageFile=logo.png
WizardImageFile=wizard.png
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x86compatible
MinVersion=6.1sp1
#ifdef Sign
; the release's build (installer.yaml): the setup and the uninstaller it puts in signed by its /Sazure=<command>
SignTool=azure
SignedUninstaller=yes
#endif

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

[Messages]
SetupWindowTitle=Install VanaCore by Vekien & Carver
FinishedHeadingLabel=VanaCore is installed!
FinishedLabel=Thank you for installing VanaCore! If you run into any issues, head over to the CatsEyeXI Discord, where you'll find VanaCore's support threads. Once in game, type /vana to get started!
FinishedLabelNoIcons=Thank you for installing VanaCore! If you run into any issues, head over to the CatsEyeXI Discord, where you'll find VanaCore's support threads. Once in game, type /vana to get started!
; the folder page: one line by the folder, the box under it
SelectDirLabel3=VanaCore will be installed in this folder. Click Next to go on, or Browse to pick another.
SelectDirBrowseLabel=

[Code]
// every page's heading is VanaCore's name and version, no line under it; the folder box up where the line it no longer has was
procedure InitializeWizard;
var
  Lift: Integer;
begin
  Lift := WizardForm.DirEdit.Top - WizardForm.SelectDirBrowseLabel.Top;
  WizardForm.SelectDirBrowseLabel.Visible := False;
  WizardForm.DirEdit.Top := WizardForm.DirEdit.Top - Lift;
  WizardForm.DirBrowseButton.Top := WizardForm.DirBrowseButton.Top - Lift;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  WizardForm.PageNameLabel.Caption := 'VANACORE v{#Version}';
  WizardForm.PageDescriptionLabel.Caption := '';
end;

// FINAL FANTASY XI's folder as PlayOnline installed it (32-bit registry; US, EU, then JP), '' when it isn't there
function FfxiFolder: String;
var
  Keys: array[0..2] of String;
  I: Integer;
begin
  Result := '';
  Keys[0] := 'PlayOnlineUS'; Keys[1] := 'PlayOnlineEU'; Keys[2] := 'PlayOnline';
  for I := 0 to 2 do
    if RegQueryStringValue(HKLM32, 'SOFTWARE\' + Keys[I] + '\InstallFolder', '0001', Result) and (Result <> '') then begin
      Result := RemoveBackslashUnlessRoot(Result);
      if DirExists(Result) then Exit;
    end;
  Result := '';
end;

// whether the player can write in Dir, or in the nearest folder above it that's there (Program Files, Windows: no)
function Writable(Dir: String): Boolean;
var
  Probe: String;
begin
  Dir := RemoveBackslashUnlessRoot(Dir);
  while (Dir <> '') and not DirExists(Dir) and (ExtractFileDir(Dir) <> Dir) do
    Dir := ExtractFileDir(Dir);
  Probe := AddBackslash(Dir) + 'vanacore-setup.tmp';
  Result := DirExists(Dir) and SaveStringToFile(Probe, '', False);
  if Result then DeleteFile(Probe);
end;

// VanaCore beside FINAL FANTASY XI (SquareEnix\VanaCore) when it can go there, else the player's own Programs
function DefaultDir(Param: String): String;
var
  Ffxi: String;
begin
  Ffxi := FfxiFolder;
  if (Ffxi <> '') and (ExtractFileDir(Ffxi) <> Ffxi) and Writable(ExtractFileDir(Ffxi)) then
    Result := AddBackslash(ExtractFileDir(Ffxi)) + 'VanaCore'
  else
    Result := ExpandConstant('{autopf}\VanaCore');
end;

// VanaCore keeps its settings and logs beside itself: a folder the player can't write to won't do
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and not Writable(WizardDirValue) then begin
    MsgBox('VanaCore can''t be installed in ' + WizardDirValue + ': Windows only lets administrators change that folder, and ' +
           'VanaCore keeps its settings and logs beside itself. Pick a folder outside Program Files.', mbError, MB_OK);
    Result := False;
  end;
end;
