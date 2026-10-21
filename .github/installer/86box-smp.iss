; Installer for the nightly Windows x64 build of 86Box-smp.
;
; Built by .github/workflows/nightly_windows.yml, which defines:
;   AppVer     the 86Box version from CMakeLists.txt, e.g. 7.0
;   BuildDate  the build date, yyyymmdd
;   GitHash    the commit, abbreviated
;   SrcDir     the cmake --install tree
;   OutDir     where to write the installer
;   OutName    the installer's file name, without .exe
;
; The ROMs are not in the installer: setup fetches the newest commit of the
; ROM repository below and installs it into {app}\roms, unless that commit is
; the one already there.

#ifndef AppVer
  #error Build with the nightly workflow, which defines AppVer and the rest
#endif

#define RomsRepo "Hinoserm/86box-roms"

[Setup]
AppId={{68E014D9-4FBC-41D0-A14F-3AFDDAE3916D}
AppName=86Box-smp
AppVersion={#AppVer} nightly {#BuildDate} ({#GitHash})
AppVerName=86Box-smp nightly {#BuildDate}
AppPublisher=Hinoserm
AppPublisherURL=https://github.com/Hinoserm/86box-smp
AppSupportURL=https://github.com/Hinoserm/86box-smp/issues
AppUpdatesURL=https://github.com/Hinoserm/86box-smp/releases/tag/nightly
VersionInfoVersion={#AppVer}
DefaultDirName={autopf}\86Box-smp
DefaultGroupName=86Box-smp
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern dynamic
SetupIconFile=..\..\src\qt\icons\86Box-red.ico
UninstallDisplayIcon={app}\86Box.exe
UninstallDisplayName=86Box-smp
Compression=lzma2/ultra64
SolidCompression=yes
ArchiveExtraction=full
CloseApplications=yes
RestartApplications=no
OutputDir={#OutDir}
OutputBaseFilename={#OutName}

[Types]
Name: "full"; Description: "86Box-smp and the latest ROMs"
Name: "compact"; Description: "86Box-smp only"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "main"; Description: "86Box-smp"; Types: full compact custom; Flags: fixed
Name: "roms"; Description: "Latest ROMs from github.com/{#RomsRepo} (about 95 MB to download; replaces the roms folder here)"; Types: full; ExtraDiskSpaceRequired: 150000000

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[InstallDelete]
; A new ROM set replaces the old one whole, so ROMs removed upstream go too.
Type: filesandordirs; Name: "{app}\roms"; Components: roms; Check: RomsFetched

[Files]
Source: "{#SrcDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: main
Source: "..\..\COPYING"; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: "{code:RomsSourceDir}\*"; DestDir: "{app}\roms"; Excludes: "\.github\*"; Flags: external ignoreversion recursesubdirs; Components: roms; Check: RomsFetched

[Icons]
Name: "{autoprograms}\86Box-smp"; Filename: "{app}\86Box.exe"
Name: "{autodesktop}\86Box-smp"; Filename: "{app}\86Box.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\86Box.exe"; Description: "{cm:LaunchProgram,86Box-smp}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\roms"; Components: roms

[Code]
const
  RomsHeadUrl = 'https://api.github.com/repos/{#RomsRepo}/commits/master';
  RomsZipUrl  = 'https://codeload.github.com/{#RomsRepo}/zip/';

var
  DownloadPage: TDownloadWizardPage;
  ExtractPage:  TExtractionWizardPage;
  RomsSha:      String;  { the commit unpacked in the temporary folder, once there is one }
  RomsDir:      String;  { the folder it is in }
  RomsDone:     Boolean; { install it }

function RomsFetched: Boolean;
begin
  Result := RomsDone;
end;

function RomsSourceDir(Param: String): String;
begin
  Result := RomsDir;
end;

{ The archive holds one folder, which GitHub names after the repository and
  the commit; it is taken as found, so a renamed repository still works. }
function UnpackedRomsDir: String;
var
  FindRec: TFindRec;
  Dir:     String;
begin
  Result := '';
  Dir := ExpandConstant('{tmp}\romsx\');
  if FindFirst(Dir + '*', FindRec) then
    try
      repeat
        if ((FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0) and (FindRec.Name <> '.') and (FindRec.Name <> '..') then begin
          Result := Dir + FindRec.Name;
          Exit;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
end;

{ The newest commit of the ROM repository: the first "sha" in the API's reply
  is the commit's own. }
function LatestRomsSha(var Sha: String): Boolean;
var
  Json: AnsiString;
  S:    String;
  P, I: Integer;
begin
  Result := False;
  try
    DownloadTemporaryFile(RomsHeadUrl, 'roms-head.json', '', nil);
  except
    Log('ROM lookup failed: ' + GetExceptionMessage);
    Exit;
  end;
  if not LoadStringFromFile(ExpandConstant('{tmp}\roms-head.json'), Json) then
    Exit;
  S := String(Json);
  P := Pos('"sha"', S);
  if P = 0 then
    Exit;
  S := Copy(S, P + 5, 64);
  P := Pos('"', S);
  if P = 0 then
    Exit;
  S := Copy(S, P + 1, 40);
  if Length(S) <> 40 then
    Exit;
  for I := 1 to 40 do
    if Pos(S[I], '0123456789abcdef') = 0 then
      Exit;
  Sha := S;
  Result := True;
end;

function InstalledRomsSha: String;
var
  V: AnsiString;
begin
  Result := '';
  if LoadStringFromFile(AddBackslash(WizardDirValue) + 'roms\roms.version', V) then
    Result := Trim(String(V));
end;

{ Fetches and unpacks the commit; setup goes on without ROMs if it can't. }
procedure FetchRoms(const Sha: String);
var
  Downloaded: Boolean;
begin
  Downloaded := False;
  DownloadPage.Clear;
  DownloadPage.Add(RomsZipUrl + Sha, 'roms.zip', '');
  DownloadPage.Show;
  try
    try
      DownloadPage.Download;
      Downloaded := True;
    except
      if DownloadPage.AbortedByUser then
        Log('ROM download cancelled.')
      else
        SuppressibleMsgBox('The ROMs could not be downloaded (' + GetExceptionMessage + '). ' +
          '86Box-smp will be installed without them; run setup again later to fetch them.',
          mbError, MB_OK, IDOK);
    end;
  finally
    DownloadPage.Hide;
  end;
  if not Downloaded then
    Exit;

  DelTree(ExpandConstant('{tmp}\romsx'), True, True, True);
  ExtractPage.Clear;
  ExtractPage.Add(ExpandConstant('{tmp}\roms.zip'), ExpandConstant('{tmp}\romsx'), True);
  ExtractPage.Show;
  try
    try
      ExtractPage.Extract;
      RomsSha := Sha;
      RomsDir := UnpackedRomsDir;
      RomsDone := (RomsDir <> '') and DirExists(RomsDir + '\machines');
      if not RomsDone then
        SuppressibleMsgBox('The ROM archive did not hold the folder expected. ' +
          '86Box-smp will be installed without the ROMs.', mbError, MB_OK, IDOK);
    except
      if not ExtractPage.AbortedByUser then
        SuppressibleMsgBox('The ROMs could not be unpacked (' + GetExceptionMessage + '). ' +
          '86Box-smp will be installed without them.', mbError, MB_OK, IDOK);
    end;
  finally
    ExtractPage.Hide;
  end;
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage('Downloading ROMs',
    'Setup is downloading the latest ROMs from github.com/{#RomsRepo}.', nil);
  DownloadPage.ShowBaseNameInsteadOfUrl := True;
  ExtractPage := CreateExtractionPage('Unpacking ROMs', 'Setup is unpacking the ROMs.', nil);
  ExtractPage.ShowArchiveInsteadOfFile := True;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Sha: String;
begin
  Result := True;
  if CurPageID <> wpReady then
    Exit;
  RomsDone := False;
  if not WizardIsComponentSelected('roms') then
    Exit;
  if not LatestRomsSha(Sha) then begin
    SuppressibleMsgBox('Setup could not ask GitHub for the latest ROMs. ' +
      '86Box-smp will be installed without updating them; run setup again later to fetch them.',
      mbError, MB_OK, IDOK);
    Exit;
  end;
  if Sha = InstalledRomsSha then begin
    Log('The ROMs installed are already ' + Sha + '.');
    Exit;
  end;
  { Back and Next again: the commit unpacked already will do. }
  if (Sha = RomsSha) and (RomsDir <> '') and DirExists(RomsDir) then
    RomsDone := True
  else
    FetchRoms(Sha);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and RomsDone then
    SaveStringToFile(ExpandConstant('{app}\roms\roms.version'), RomsSha + #13#10, False);
end;
