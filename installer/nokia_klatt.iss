; Nokia Klatt SAPI5 - Windows installer
;
; Written by hand rather than produced by the wizard, because this package has
; two things a generated script does not handle well: a quarter of a gigabyte
; of phone firmware to lay down, and two SAPI engines of different
; architectures that each have to be registered with the matching regsvr32.
; The one generated part is rom_languages.iss, which is the phones, their 99
; languages and every file those own - no place for a hand-made typo, and no
; place for the entry order the wizard's tree depends on to drift either.
;
; Accessibility notes, since the people most likely to install this are
; screen reader users:
;   - no custom wizard pages, so every control is one Inno Setup labels itself
;   - the components page names each item in full rather than by abbreviation
;   - nothing is silent: the finish page says where the settings utility is
;   - AlwaysShowComponentsList/AlwaysShowDirOnReadyPage keep choices visible
;     rather than hiding them behind "advanced"
;   - the components list is a tree, so a phone can be checked or cleared in
;     one keystroke without walking its languages one by one
;
; Choosing languages and voices:
;   138 voices in the voice list is a lot to arrow through, so every voice is
;   selectable. A phone's languages are its own - the same language from two
;   phones sounds different - so they are chosen per phone, under it in the
;   tree, and male and female sit at the top of each phone that has both.
;
;   This is about the voice list, not about disk space. Almost all of a
;   phone's size is the ROM image, which every one of its languages shares:
;   the per-language speech data is a few hundred kilobytes across a whole
;   phone. Deselecting languages makes the voice list short, not the install
;   small; deselecting a whole phone is what saves tens of megabytes.

#define AppName "Nokia Klatt SAPI5"
#define AppVersion "1.1.1"
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
Name: "full"; Description: "Everything: all 138 voices from all five phones"
Name: "english"; Description: "English only: every English voice, from each phone that has one"
Name: "compact"; Description: "Compact: English (UK) male, from the Nokia 5320 only"
Name: "custom"; Description: "Custom: choose phones, languages and voices one by one"; Flags: iscustom

[Components]
Name: "core"; Description: "Speech engines and the settings utility (required)"; Types: full english compact custom; Flags: fixed

Name: "roms"; Description: "Phone voices"; Types: full english compact custom

; The five phones, their languages and their voices, generated from the ROM
; tree and src\nk\catalog.cpp by tools\gen_rom_components.py.
;
; The phones are generated together with their languages rather than being
; listed here, because the wizard's components list is a tree built from the
; order of the entries and the depth of each name, not from the names
; themselves. A phone has to come immediately before the languages that belong
; to it, and keeping those two halves in step across two files is exactly what
; went wrong: with all five phones listed here and every language arriving
; afterwards from the include, all 99 languages became children of the last
; phone in the list.
;
; A phone's size is its ROM image, which all of its languages share, so
; clearing languages does not shrink it; clearing the phone itself is what
; saves the tens of megabytes.
#include "rom_languages.iss"

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
;
; Everything a phone shares between its languages comes down here. The two
; package kinds that belong to one language and no other - text-to-phoneme
; (srsf_0_*) and prosody (srsf_4_*) - are excluded and re-added per language by
; rom_languages.iss, which is how a language the user did not choose ends up
; absent from the voice list.
;
; srsf_0_0 is the exception the exclusion pattern would otherwise swallow:
; language 0 is ELangTest, not a voice anyone picks, and the engine expects it
; to be there whichever languages are installed.
#define RomExcludes "srsf_0_*.bin,srsf_4_*.bin"

Source: "{#RomRoot}\5320\*";   DestDir: "{app}\roms\5320";   Excludes: "{#RomExcludes}"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: roms\p5320
Source: "{#RomRoot}\e65\*";    DestDir: "{app}\roms\e65";    Excludes: "{#RomExcludes}"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: roms\pe65
Source: "{#RomRoot}\n958gb\*"; DestDir: "{app}\roms\n958gb"; Excludes: "{#RomExcludes}"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: roms\pn958gb
Source: "{#RomRoot}\6650\*";   DestDir: "{app}\roms\6650";   Excludes: "{#RomExcludes}"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: roms\p6650
Source: "{#RomRoot}\n85\*";    DestDir: "{app}\roms\n85";    Excludes: "{#RomExcludes}"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: roms\pn85

Source: "{#RomRoot}\5320\files\system\data\srsf_0_0.bin";   DestDir: "{app}\roms\5320\files\system\data";   Flags: ignoreversion; Components: roms\p5320
Source: "{#RomRoot}\e65\files\system\data\srsf_0_0.bin";    DestDir: "{app}\roms\e65\files\system\data";    Flags: ignoreversion; Components: roms\pe65
Source: "{#RomRoot}\n958gb\files\system\data\srsf_0_0.bin"; DestDir: "{app}\roms\n958gb\files\system\data"; Flags: ignoreversion; Components: roms\pn958gb
Source: "{#RomRoot}\6650\files\system\data\srsf_0_0.bin";   DestDir: "{app}\roms\6650\files\system\data";   Flags: ignoreversion; Components: roms\p6650
Source: "{#RomRoot}\n85\files\system\data\srsf_0_0.bin";    DestDir: "{app}\roms\n85\files\system\data";    Flags: ignoreversion; Components: roms\pn85

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

// ---- which languages and which voices ----------------------------------

// Records one phone's answer to male, female or both. Called only for the
// phones that offer the choice: the E65 and the N95 8GB ignore the voice name
// and have one voice per language, so there is nothing to write for them.
procedure WriteVoiceGenders(const Path, Phone, Component: String);
begin
  if not WizardIsComponentSelected(Component) then
    exit;

  // Neither half chosen is not an answer, it is a phone with no voices, and
  // the wizard refuses to leave the components page in that state. A silent
  // install can still reach it, by naming a language in /COMPONENTS without
  // naming a voice. Writing nothing leaves whatever the last install decided,
  // or on a first install the engine's default of both - which is what that
  // command line used to install.
  if not WizardIsComponentSelected(Component + '\male')
     and not WizardIsComponentSelected(Component + '\female') then
    exit;

  // Otherwise both keys are written, zeroes included: a missing key means
  // "both" to the engine, so leaving one out would put back a voice the user
  // had just cleared.
  if WizardIsComponentSelected(Component + '\male') then
    SetIniString(Phone, 'Male', '1', Path)
  else
    SetIniString(Phone, 'Male', '0', Path);
  if WizardIsComponentSelected(Component + '\female') then
    SetIniString(Phone, 'Female', '1', Path)
  else
    SetIniString(Phone, 'Female', '0', Path);
end;

// A language is chosen by whether its packages are on disk. Male and female
// are not: both are built from the same language data, so the choice has to be
// recorded somewhere the engine reads when it makes the voice list.
procedure WriteVoiceFilter;
var
  Path: String;
begin
  // Each phone's section is overwritten in place rather than the file being
  // started fresh: a phone left unchecked this time is not being reinstalled,
  // and its ROM is still on disk, so the answer it was given last time is
  // still the right one for it.
  Path := ExpandConstant('{app}\roms\voices.ini');
  WriteVoiceGenders(Path, '5320', 'roms\p5320');
  WriteVoiceGenders(Path, '6650', 'roms\p6650');
  WriteVoiceGenders(Path, 'n85', 'roms\pn85');
end;

// Installing over an older copy lays down the languages chosen this time, but
// it does not remove the ones chosen last time: Inno only ever adds files.
// Clearing a phone's per-language packages first is what makes deselecting a
// language actually take a voice out of the list on an upgrade.
//
// Only for the phones being installed this time. An unchecked phone keeps what
// it had - its ROM is not removed either, and stripping the languages from a
// ROM that stays behind would leave tens of megabytes that says nothing.
procedure ClearPhoneLanguages(const Phone, Component: String);
var
  Data: String;
begin
  if not WizardIsComponentSelected(Component) then
    exit;
  Data := ExpandConstant('{app}\roms\') + Phone + '\files\system\data\';
  DelTree(Data + 'srsf_0_*.bin', False, True, False);
  DelTree(Data + 'srsf_4_*.bin', False, True, False);
end;

procedure ClearLanguagePackages;
begin
  ClearPhoneLanguages('5320', 'roms\p5320');
  ClearPhoneLanguages('e65', 'roms\pe65');
  ClearPhoneLanguages('n958gb', 'roms\pn958gb');
  ClearPhoneLanguages('6650', 'roms\p6650');
  ClearPhoneLanguages('n85', 'roms\pn85');
end;

// How many of one phone's language components are checked. Read off the
// selection rather than a list repeated here, so adding a ROM to
// rom_languages.iss does not need a matching edit in this file.
function SelectedLanguageCount(const Component: String): Integer;
var
  List, Prefix: String;
  At: Integer;
begin
  Result := 0;
  List := Lowercase(WizardSelectedComponents(False));
  // The separator is a comma, but not reliably a bare one; component names
  // have no spaces in them, so taking the spaces out makes the match exact.
  StringChangeEx(List, ' ', '', True);
  List := ',' + List + ',';
  Prefix := ',' + Lowercase(Component) + '\l';
  At := Pos(Prefix, List);
  while At > 0 do
  begin
    Result := Result + 1;
    Delete(List, 1, At + Length(Prefix) - 1);
    List := ',' + List;
    At := Pos(Prefix, List);
  end;
end;

// A phone with no languages, or no voices, installs its ROM and contributes
// nothing to the voice list. That is tens of megabytes and a silent
// disappointment, so it is worth saying so while the choice can still be
// changed.
function PhoneSelectionIsUsable(const Component, PhoneName: String;
                                HasGenders: Boolean): Boolean;
begin
  Result := True;
  if not WizardIsComponentSelected(Component) then
    exit;

  if SelectedLanguageCount(Component) = 0 then
  begin
    MsgBox('The ' + PhoneName + ' is selected but none of its languages are.'
           + #13#10#13#10
           + 'It would be installed and add no voices. Choose at least one of '
           + 'its languages, or clear the ' + PhoneName + ' itself.',
           mbError, MB_OK);
    Result := False;
    exit;
  end;

  if HasGenders and not WizardIsComponentSelected(Component + '\male')
     and not WizardIsComponentSelected(Component + '\female') then
  begin
    MsgBox('The ' + PhoneName + ' is selected but neither its male nor its '
           + 'female voices are.' + #13#10#13#10
           + 'It would be installed and add no voices. Choose male voices, '
           + 'female voices, or both.',
           mbError, MB_OK);
    Result := False;
  end;
end;

function SelectionIsUsable(): Boolean;
begin
  Result := False;
  if not PhoneSelectionIsUsable('roms\p5320', 'Nokia 5320', True) then exit;
  if not PhoneSelectionIsUsable('roms\pe65', 'Nokia E65', False) then exit;
  if not PhoneSelectionIsUsable('roms\pn958gb', 'Nokia N95 8GB', False) then exit;
  if not PhoneSelectionIsUsable('roms\p6650', 'Nokia 6650', True) then exit;
  if not PhoneSelectionIsUsable('roms\pn85', 'Nokia N85', True) then exit;

  if not WizardIsComponentSelected('roms') then
  begin
    // Not an error: someone may be installing the engines to point at a ROM
    // set they already have. It is a surprise worth confirming, though.
    Result := MsgBox('No phones are selected, so no voices will be installed.'
                     + #13#10#13#10
                     + 'The speech engines and the settings utility will be '
                     + 'installed, but no Nokia Klatt voice will appear in any '
                     + 'application. Continue anyway?',
                     mbConfirmation, MB_YESNO) = IDYES;
    exit;
  end;

  Result := True;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectComponents then
    Result := SelectionIsUsable();
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
    ClearLanguagePackages;
  end;

  if CurStep = ssPostInstall then
  begin
    WriteVoiceFilter;
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
