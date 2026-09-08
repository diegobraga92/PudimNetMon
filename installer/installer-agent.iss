; PudimNetMon Agent - Windows install wizard (Inno Setup)
;
; Compiled by CI (".github/workflows/ci.yml", job cpp-agent-windows) into a
; self-contained setup EXE that does the following.
;   * Collects the node ID and collector settings on a wizard page.
;   * Installs pudim-agent.exe under Program Files.
;   * Registers the "PudimNetMonAgent" auto-start service with the Service
;     Control Manager through sc.exe, which ships with Windows. The agent
;     binary has no --install-service flag and only runs under the SCM or in
;     the console.
;   * Writes %ProgramData%\PudimNetMon\agent.conf for later edits.
;   * Stops and removes the service on uninstall.
;
; Configuration model. One authoritative source per setting. The node-id is
; baked into the service command line (like the Linux unit's --node-id=%H) and
; stays immutable for the service lifetime. Every mutable setting
; (collector-endpoints, interval) lives in %ProgramData%\PudimNetMon\agent.conf,
; which the agent reads at startup. Precedence is
; built-in defaults < agent.conf < service command line. This prevents an
; upgrade or reinstall from leaving stale CLI arguments in the service
; registration that override the config file.
;
; Build
;   ISCC.exe /DMyAppVersion=0.1.0 installer\installer-agent.iss
;
; Compile-time inputs
;   installer\payload\pudim-agent.exe    (Release build, staged by CI)
;   installer\payload\vc_redist.x64.exe  (optional, bundled when present)

#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif

#define MyAppName "PudimNetMon Agent"
#define MyAppExeName "pudim-agent.exe"
#define MyAppPublisher "PudimNetMon"
#define MyAppStateDir "{commonappdata}\PudimNetMon"

[Setup]
AppId={{9F0D4B2A-8C1E-4A6F-B5D3-2E7A9C0F1D2E}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
VersionInfoVersion={#MyAppVersion}
VersionInfoProductName={#MyAppName}
DefaultDirName={autopf}\PudimNetMon Agent
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist\installer
OutputBaseFilename=PudimNetMon-Agent-Setup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#MyAppExeName}
LicenseFile=..\LICENSE
RestartIfNeededByRun=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "payload\pudim-agent.exe"; DestDir: "{app}"; Flags: ignoreversion
; VC++ runtime for machines without the redistributable. The MSVC build links
; the dynamic CRT, so CI always bundles it. Local builds work without it.
#if FileExists("payload\vc_redist.x64.exe")
Source: "payload\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall
#endif

[Run]
#if FileExists("payload\vc_redist.x64.exe")
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Microsoft Visual C++ Redistributable..."; Flags: waituntilterminated
#endif
; Register the auto-start service with the Service Control Manager (sc.exe
; ships with Windows). On an upgrade the create step fails with "service
; already exists", which is expected. The sc config step below rewrites the
; ImagePath in both cases, so a reinstall never keeps stale arguments.
; WriteAgentConfig (BeforeInstall) writes agent.conf before the service starts.
; Service start is deferred to a detached helper (StartAgentService) because
; Setup keeps the installed exe open until it exits.
Filename: "{sys}\sc.exe"; Parameters: {code:GetServiceCreateParams}; StatusMsg: "Registering PudimNetMonAgent service..."; Flags: waituntilterminated runhidden; BeforeInstall: WriteAgentConfig
Filename: "{sys}\sc.exe"; Parameters: {code:GetServiceConfigParams}; StatusMsg: "Configuring PudimNetMonAgent service..."; Flags: waituntilterminated runhidden
Filename: "{sys}\sc.exe"; Parameters: "description PudimNetMonAgent ""PudimNetMon network monitoring agent"""; Flags: waituntilterminated runhidden; AfterInstall: StartAgentService

[UninstallRun]
; Service stop and delete happen in [Code] (CurUninstallStepChanged ->
; RemoveAgentService). Declarative [UninstallRun] steps ran too early, since sc
; stop returns before the service stops and sc delete on a STOP_PENDING service
; only marks it for deletion. That left the service registered with the agent
; process still running after the uninstaller exited.

[Code]

var
  AgentPage: TInputQueryWizardPage;
  NodeIdValue: String;
  CollectorValue: String;
  IntervalValue: String;

const
  DefaultInterval = '5000';

// Waits until the PudimNetMonAgent service stops. sc stop returns as soon as
// the stop control is accepted, while the service stays STOP_PENDING and the
// exe stays mapped, so a follow-up delete only marks it for deletion. Poll the
// locale-independent sc stop exit codes instead.
//   0    = stop control accepted (service stopping)
//   1061 = service cannot accept control yet (still stopping)
//   1062 = service is stopped
//   1060 = service does not exist (fresh install)
function WaitAgentStopped(): Boolean;
var
  ResultCode: Integer;
  Attempt: Integer;
begin
  Result := False;
  for Attempt := 1 to 60 do
  begin
    // Exec does not expand constants in FileName, so ExpandConstant is required.
    if Exec(ExpandConstant('{sys}\sc.exe'), 'stop PudimNetMonAgent', '', SW_HIDE,
            ewWaitUntilTerminated, ResultCode) then
    begin
      if (ResultCode = 1060) or (ResultCode = 1062) then
      begin
        Result := True;  // not installed, or fully stopped
        Exit;
      end;
    end;
    Sleep(1000);
  end;
end;

// Installer path. Stop the running service before the Preparing page, where
// RestartManager scans for in-use files and only waits about 5 s before
// aborting an upgrade with exit code 5. No-op on a fresh install (1060).
procedure StopAgentService();
begin
  if not WaitAgentStopped() then
  begin
    MsgBox('Setup could not stop the PudimNetMonAgent service. Stop it in ' +
           'Services, then run Setup again.', mbError, MB_OK);
    Abort();
  end;
end;

// Uninstaller path. Wait for the service to stop, then delete it. This cannot
// run from [UninstallRun] because sc stop returns before the service stops and
// sc delete on a STOP_PENDING service only marks it for deletion, leaving the
// service registered and the agent still shutting down. Delete exit codes are
// ignored (1060 = already gone, 1072 = already marked for deletion).
procedure RemoveAgentService();
var
  ResultCode: Integer;
begin
  WaitAgentStopped();
  Exec(ExpandConstant('{sys}\sc.exe'), 'delete PudimNetMonAgent', '', SW_HIDE,
       ewWaitUntilTerminated, ResultCode);
end;

// Runs in the uninstaller. usAppMutexCheck is the first uninstall step, before
// any file is removed, so stopping the agent there guarantees pudim-agent.exe
// is unlocked by the time the uninstaller deletes it.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usAppMutexCheck then
    RemoveAgentService();
end;

procedure InitializeWizard();
begin
  // A reinstall replaces pudim-agent.exe while the running service holds it
  // open, so stop the service and wait before the Preparing page. Do this in
  // InitializeWizard instead of InitializeSetup because InitializeSetup runs
  // before UAC elevation, where stopping the service is denied.
  StopAgentService();


  // Defaults are captured here so silent installs, which skip the wizard
  // pages, still configure the service sensibly.
  NodeIdValue := GetComputerNameString();
  CollectorValue := '';
  IntervalValue := DefaultInterval;

  AgentPage := CreateInputQueryPage(wpSelectTasks,
    'Agent configuration',
    'Configure the PudimNetMon agent service.',
    'The agent runs as the "PudimNetMonAgent" auto-start Windows service. ' +
    'The node ID is baked into the service command line; the other settings ' +
    'are saved to %ProgramData%\PudimNetMon\agent.conf, which you can edit ' +
    'later (restart the service to apply them).');

  AgentPage.Add('Node ID (unique per monitored host):', False);
  AgentPage.Values[0] := NodeIdValue;

  AgentPage.Add('Collector endpoint(s), comma-separated (e.g. collector.lan:50051):', False);
  AgentPage.Values[1] := CollectorValue;

  AgentPage.Add('Polling interval (milliseconds):', False);
  AgentPage.Values[2] := IntervalValue;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = AgentPage.ID then
  begin
    NodeIdValue := Trim(AgentPage.Values[0]);
    CollectorValue := Trim(AgentPage.Values[1]);
    IntervalValue := Trim(AgentPage.Values[2]);
    if NodeIdValue = '' then
    begin
      MsgBox('Node ID must not be empty.', mbError, MB_OK);
      Result := False;
    end
    else if (Pos(' ', NodeIdValue) > 0) or (Pos(#9, NodeIdValue) > 0) or
            (Pos('"', NodeIdValue) > 0) then
    begin
      MsgBox('Node ID must not contain spaces, tabs or double quotes because ' +
             'it is baked into the service command line.', mbError, MB_OK);
      Result := False;
    end
    else if StrToIntDef(IntervalValue, 0) <= 0 then
    begin
      MsgBox('Polling interval must be a positive number of milliseconds.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

// The service ImagePath is the quoted agent exe plus the immutable node ID
// (like the Linux unit's --node-id=%H). Mutable settings (interval,
// collector-endpoints) go into agent.conf instead, so reinstalls and upgrades
// never leave stale CLI arguments behind. The wizard page validates that the
// node ID has no spaces, tabs or quotes, so the exe path is the only token
// that needs quoting.
//
// sc.exe parses its command line with standard Windows CRT rules, so quotes
// that must survive into the service ImagePath are escaped as \" below. sc
// stores binPath as-is and the SCM re-parses it when starting the service,
// yielding the ImagePath above again.
function GetServiceCreateParams(Param: String): String;
var
  ExePath: String;
begin
  ExePath := ExpandConstant('{app}\{#MyAppExeName}');
  Result := 'create PudimNetMonAgent start= auto binPath= "\"' + ExePath +
            '\" --node-id=' + NodeIdValue + '"' +
            ' DisplayName= "PudimNetMon Agent"';
end;

// Runs on every install, including upgrades over an existing service, to
// refresh the ImagePath and start type. Mirrors the create-or-reconfigure
// logic that used to live in the agent binary.
function GetServiceConfigParams(Param: String): String;
var
  ExePath: String;
begin
  ExePath := ExpandConstant('{app}\{#MyAppExeName}');
  Result := 'config PudimNetMonAgent start= auto binPath= "\"' + ExePath +
            '\" --node-id=' + NodeIdValue + '"';
end;

// Starts the service after registration. Inno Setup keeps the installed
// executable open for its rollback and RestartManager bookkeeping until the
// Setup process exits, so a synchronous sc start fails with error 2 for the
// whole install. Defer the start to a detached helper that outlives Setup,
// waits a few seconds for Setup to exit, and then starts the service. The
// service is AUTO_START, so a failed helper is non-fatal and the service comes
// up on the next boot.
procedure StartAgentService();
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\cmd.exe'),
       '/c ping -n 6 127.0.0.1 >nul & sc.exe start PudimNetMonAgent >nul 2>&1',
       '', SW_HIDE, ewNoWait, ResultCode);
  Log('Service start deferred to a detached helper after Setup exits');
end;

// Persists the wizard settings to %ProgramData%\PudimNetMon\agent.conf. Runs
// as BeforeInstall of the service-registration [Run] entry so the file always
// exists before the service starts. Precedence is built-in defaults <
// agent.conf < service command line. The node-id deliberately stays on the
// service command line so each setting has one authoritative source.
procedure WriteAgentConfig();
var
  ConfDir, ConfPath, Conf: String;
begin
  ConfDir := ExpandConstant('{#MyAppStateDir}');
  ConfPath := ConfDir + '\agent.conf';
  if not DirExists(ConfDir) then
    CreateDir(ConfDir);
  if DirExists(ConfDir) then
  begin
    Conf := '# Generated by the PudimNetMon Agent installer' + #13#10 +
            '# node-id comes from the service command line (sc qc PudimNetMonAgent)' + #13#10 +
            '# Precedence: built-in defaults < this file < service command line' + #13#10 +
            'interval=' + IntervalValue + #13#10;
    if CollectorValue <> '' then
      Conf := Conf + 'collector-endpoints=' + CollectorValue + #13#10;
    SaveStringToFile(ConfPath, Conf, False);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
  begin
    // Stop the service and wait before replacing the exe during an upgrade.
    // No-op on a fresh install (sc stop returns 1060/1062 immediately).
    StopAgentService();
  end;
end;

