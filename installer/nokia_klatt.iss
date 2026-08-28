; Nokia Klatt SAPI5 - Windows installer
;
; Written by hand rather than generated, because this package has two things a
; generated script does not handle well: a quarter of a gigabyte of phone
; firmware to lay down, and two SAPI engines of different architectures that
; each have to be registered with the matching regsvr32.
;
; Accessibility notes, since the people most likely to install this are
; screen reader users:
;   - no custom wizard pages, so every control is one Inno Setup labels itself
;   - the components page names each item in full rather than by abbreviation
;   - nothing is silent: the finish page says where the settings utility is
;   - AlwaysShowComponentsList/AlwaysShowDirOnReadyPage keep choices visible
;     rather than hiding them behind "advanced"

#define AppName "Nokia Klatt SAPI5"
#define AppVersion "1.0.0"
#define AppPublisher "Nokia Klatt SAPI5 project"
#define AppExeName "NokiaKlattConfig.exe"

; Where the built binaries and engine files are, relative to this script.
#define BuildX64 "..\build\x64\bin\Release"
#define BuildX86 "..\build\x86\bin\Release"
#define RomRoot "..\bin\roms"
#define UnicornDll "..\bin\_nokia\lib\unicorn\lib\unicorn.dll"

[Setup]
AppId={{8E3A4C71-5D92-4B18-9F07-2C6A1D4E8B35}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\NokiaKlatt
DefaultGroupName={#AppName}
OutputDir=..\dist
OutputBaseFilename=NokiaKlattSAPI5-{#AppVersion}-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

; The emulator is 64-bit only: the vendored unicorn.dll has no 32-bit build.
; The 32-bit SAPI engine is still installed and still works - it reaches the
; 64-bit host through a pipe - but the machine itself has to be 64-bit.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; Registering a SAPI voice enumerator writes to HKLM, which needs elevation.
PrivilegesRequired=admin

; The installer keeps its own log, which is the first thing to ask for when an
; install goes wrong.
SetupLogging=yes

UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName}
DisableWelcomePage=no
AlwaysShowComponentsList=yes
AlwaysShowDirOnReadyPage=yes
ShowLanguageDialog=no
ChangesAssociations=no

; Roughly what the ROMs and binaries need once expanded.
ExtraDiskSpaceRequired=10485760

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Everything: all 138 voices, both speech engines and the settings utility"
Name: "custom"; Description: "Choose which phone voices to install"; Flags: iscustom

[Components]
Name: "core"; Description: "Speech engines and the settings utility (required)"; Types: full custom; Flags: fixed

Name: "roms"; Description: "Phone voices"; Types: full custom
Name: "roms\p5320"; Description: "Nokia 5320 - 33 languages, male and female (67 MB)"; Types: full custom
Name: "roms\pe65"; Description: "Nokia E65 - 30 languages (19 MB)"; Types: full custom
Name: "roms\pn958gb"; Description: "Nokia N95 8GB - 30 languages (58 MB)"; Types: full custom
Name: "roms\p6650"; Description: "Nokia 6650 - American English, Canadian French, Brazilian Portuguese, Latin American Spanish (46 MB)"; Types: full custom
Name: "roms\pn85"; Description: "Nokia N85 - Tagalog and Vietnamese (42 MB)"; Types: full custom

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut for the Nokia Klatt settings"; GroupDescription: "Shortcuts:"
Name: "enablelog"; Description: "Write a detailed &log file, for troubleshooting"; GroupDescription: "Diagnostics:"; Flags: unchecked

[Files]
; ---- the 64-bit half: the emulator host, the 64-bit engine, the utility ----
Source: "{#BuildX64}\NokiaKlattHost.exe";   DestDir: "{app}"; Flags: ignoreversion; Components: core
Source: "{#BuildX64}\NokiaKlattSAPI.dll";   DestDir: "{app}"; Flags: ignoreversion; Components: core
Source: "{#BuildX64}\NokiaKlattConfig.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: core
Source: "{#UnicornDll}";                    DestDir: "{app}"; Flags: ignoreversion; Components: core

; ---- the 32-bit SAPI engine, for 32-bit applications ----
Source: "{#BuildX86}\NokiaKlattSAPI.dll"; DestDir: "{app}\x86"; Flags: ignoreversion; Components: core

; ---- the phone firmware and speech data ----
; Each ROM is one phone's build of the engine with its own languages. They are
; large and there is no way around that: the voices live inside them.
Source: "{#RomRoot}\5320\*";   DestDir: "{app}\roms\5320";   Flags: ignoreversion recursesubdirs createallsubdirs; Components: roms\p5320
Source: "{#RomRoot}\e65\*";    DestDir: "{app}\roms\e65";    Flags: ignoreversion recursesubdirs createallsubdirs; Components: roms\pe65
Source: "{#RomRoot}\n958gb\*"; DestDir: "{app}\roms\n958gb"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: roms\pn958gb
Source: "{#RomRoot}\6650\*";   DestDir: "{app}\roms\6650";   Flags: ignoreversion recursesubdirs createallsubdirs; Components: roms\p6650
Source: "{#RomRoot}\n85\*";    DestDir: "{app}\roms\n85";    Flags: ignoreversion recursesubdirs createallsubdirs; Components: roms\pn85

[Icons]
Name: "{group}\Nokia Klatt Speech Settings"; Filename: "{app}\{#AppExeName}"; Comment: "Choose the Nokia Klatt custom voice, speed, pitch and volume"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Nokia Klatt Speech Settings"; Filename: "{app}\{#AppExeName}"; Comment: "Choose the Nokia Klatt custom voice, speed, pitch and volume"; Tasks: desktopicon

[Registry]
; Where the engine finds its ROMs, whichever process asks.
Root: HKLM; Subkey: "SOFTWARE\NokiaKlatt"; ValueType: string; ValueName: "InstallDir"; ValueData: "{app}"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\NokiaKlatt"; ValueType: string; ValueName: "Version"; ValueData: "{#AppVersion}"; Flags: uninsdeletekey

[Run]
; Both engines are registered, each with the regsvr32 of its own architecture.
; Getting these the wrong way round is the classic way to end up with a SAPI
; voice that half the applications on the machine cannot see.
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\NokiaKlattSAPI.dll"""; StatusMsg: "Registering the 64-bit speech engine..."; Flags: runhidden waituntilterminated; Components: core
Filename: "{syswow64}\regsvr32.exe"; Parameters: "/s ""{app}\x86\NokiaKlattSAPI.dll"""; StatusMsg: "Registering the 32-bit speech engine..."; Flags: runhidden waituntilterminated; Components: core

Filename: "{app}\{#AppExeName}"; Description: "Open the Nokia Klatt speech settings"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "{sys}\regsvr32.exe"; Parameters: "/s /u ""{app}\NokiaKlattSAPI.dll"""; Flags: runhidden waituntilterminated; RunOnceId: "UnregX64"
Filename: "{syswow64}\regsvr32.exe"; Parameters: "/s /u ""{app}\x86\NokiaKlattSAPI.dll"""; Flags: runhidden waituntilterminated; RunOnceId: "UnregX86"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\roms"
Type: dirifempty; Name: "{app}"

[Code]

// The host holds its ROMs open and keeps the pipe. It has to be gone before
// the files under it can be replaced or deleted, and asking it politely is
// better than failing halfway through a 200 MB copy.
procedure StopSpeechHost;
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/IM NokiaKlattHost.exe /F',
       '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/IM NokiaKlattConfig.exe /F',
       '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  // Give Windows a moment to release the mapped ROM files.
  Sleep(700);
end;

function InitializeSetup(): Boolean;
begin
  StopSpeechHost;
  Result := True;
end;

// An upgrade over an older copy has to unregister the engines it is about to
// overwrite, or the old CLSIDs linger and SAPI keeps handing out tokens for a
// DLL that no longer exists.
procedure UnregisterExisting;
var
  ResultCode: Integer;
begin
  if FileExists(ExpandConstant('{app}\NokiaKlattSAPI.dll')) then
    Exec(ExpandConstant('{sys}\regsvr32.exe'),
         '/s /u "' + ExpandConstant('{app}\NokiaKlattSAPI.dll') + '"',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  if FileExists(ExpandConstant('{app}\x86\NokiaKlattSAPI.dll')) then
    Exec(ExpandConstant('{syswow64}\regsvr32.exe'),
         '/s /u "' + ExpandConstant('{app}\x86\NokiaKlattSAPI.dll') + '"',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure WriteLoggingFlag;
var
  Marker: String;
begin
  Marker := ExpandConstant('{localappdata}\NokiaKlatt');
  if not DirExists(Marker) then
    CreateDir(Marker);
  SaveStringToFile(Marker + '\logging.on',
                   'Delete this file to stop the Nokia Klatt speech engine '
                   + 'writing nokiaklatt.log.' + #13#10, False);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
  begin
    StopSpeechHost;
    UnregisterExisting;
  end;

  if CurStep = ssPostInstall then
  begin
    if WizardIsTaskSelected('enablelog') then
      WriteLoggingFlag;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    StopSpeechHost;
  end;
end;
