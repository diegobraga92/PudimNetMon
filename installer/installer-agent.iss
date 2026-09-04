; PudimNetMon Agent - Windows install wizard (Inno Setup)
;
; CI (".github/workflows/ci.yml", job cpp-agent-windows) compiles this into a
; self-contained setup EXE that:
;   * collects the agent identity + collector settings on a wizard page,
;   * installs pudim-agent.exe under Program Files,
;   * registers the "PudimNetMonAgent" auto-start Windows service directly with
;     the Service Control Manager (via sc.exe, which ships with Windows - the
;     agent binary has no --install-service verb and only ever runs under the
;     SCM or in the console),
;   * writes %ProgramData%\PudimNetMon\agent.conf so settings are easy to
;     review/adjust later, and
;   * stops and removes the service again on uninstall (sc.exe stop/delete).
;
; Configuration model (single source of truth per setting):
;   * node-id is baked into the service command line (like the Linux unit's
;     --node-id=%H) and is immutable for the lifetime of the service, and
;   * every mutable setting (collector-endpoints, interval, ...) lives in
;     %ProgramData%\PudimNetMon\agent.conf, read by the agent at startup
;     (precedence: built-in defaults < agent.conf < service command line).
; This avoids the classic pitfall where an upgrade/reinstall leaves stale CLI
; arguments in the service registration that override the config file.
;
; Build (see docs/windows.md):
;   ISCC.exe /DMyAppVersion=0.1.0 installer\installer-agent.iss
;
; Compile-time inputs:
;   installer\payload\pudim-agent.exe    (Release build, staged by CI)
;   installer\payload\vc_redist.x64.exe  (optional; bundled when present)

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
; VC++ runtime for machines without the redistributable (the MSVC build links
; the dynamic CRT). CI always bundles it; local builds work without it.
#if FileExists("payload\vc_redist.x64.exe")
Source: "payload\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall
#endif

[Run]
#if FileExists("payload\vc_redist.x64.exe")
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Microsoft Visual C++ Redistributable..."; Flags: waituntilterminated
#endif
; Register the auto-start service directly through the Service Control Manager
; (sc.exe ships with every supported Windows version). The create step exits
; non-zero with "service already exists" when upgrading/reinstalling; that is
; expected, and the sc config step that follows rewrites the ImagePath in both
; cases so a reinstall never keeps stale command-line arguments. WriteAgentConfig
; (BeforeInstall) guarantees agent.conf exists before the service starts.
; sc create/config/description exit codes are only logged by Inno Setup. The
; service start itself is deferred to a detached helper (see StartAgentService)
; because Setup keeps the just-installed exe open until it exits.
Filename: "{sys}\sc.exe"; Parameters: {code:GetServiceCreateParams}; StatusMsg: "Registering PudimNetMonAgent service..."; Flags: waituntilterminated runhidden; BeforeInstall: WriteAgentConfig
Filename: "{sys}\sc.exe"; Parameters: {code:GetServiceConfigParams}; StatusMsg: "Configuring PudimNetMonAgent service..."; Flags: waituntilterminated runhidden
Filename: "{sys}\sc.exe"; Parameters: "description PudimNetMonAgent ""PudimNetMon network monitoring agent"""; Flags: waituntilterminated runhidden; AfterInstall: StartAgentService

[UninstallRun]
; Stop and delete the auto-start service before files are removed (the running
; service locks pudim-agent.exe). [UninstallRun] runs as the first step of the
; uninstaller; the commands are idempotent and non-zero exits are ignored.
Filename: "{sys}\sc.exe"; Parameters: "stop PudimNetMonAgent"; Flags: waituntilterminated runhidden
Filename: "{sys}\sc.exe"; Parameters: "delete PudimNetMonAgent"; Flags: waituntilterminated runhidden

[Code]

var
  AgentPage: TInputQueryWizardPage;
  NodeIdValue: String;
  CollectorValue: String;
  IntervalValue: String;

const
  DefaultInterval = '5000';

procedure InitializeWizard();
var
  ResultCode: Integer;
begin
  // A reinstall/upgrade replaces pudim-agent.exe while the running service
  // holds it open. Stop it here, before Inno Setup's RestartManager scans for
  // in-use files on the Preparing page (that scan waits only ~5 s for the
  // agent to stop, then aborts the install -- exit code 5). We do this in
  // InitializeWizard rather than InitializeSetup because InitializeSetup runs
  // before UAC elevation, where sc.exe stop is denied. On a fresh install
  // there is no service and sc stop fails fast (error 1060), which is ignored.
  Exec('{sys}\sc.exe', 'stop PudimNetMonAgent', '', SW_HIDE,
       ewWaitUntilTerminated, ResultCode);
  if ResultCode = 0 then
    Sleep(1500);  // let the process release its image before file replacement

  // Defaults are captured here so silent installs (which skip the wizard
  // pages) still configure the service sensibly.
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
// baked in (like the Linux unit's --node-id=%H); mutable settings (interval,
// collector-endpoints) go into agent.conf instead, so re-installs/upgrades
// never leave stale command-line args behind. NodeIdValue is validated on the
// wizard page to contain no spaces/tabs/quotes, so the exe path is the only
// token that needs quoting.
//
// sc.exe parses its own command line with the standard Windows CRT rules, so
// the quotes that must survive verbatim into the service ImagePath are escaped
// as \" below. sc stores binPath as-is; the SCM re-parses it when starting the
// service, yielding the ImagePath above again.
function GetServiceCreateParams(Param: String): String;
var
  ExePath: String;
begin
  ExePath := ExpandConstant('{app}\{#MyAppExeName}');
  Result := 'create PudimNetMonAgent start= auto binPath= "\"' + ExePath +
            '\" --node-id=' + NodeIdValue + '"' +
            ' DisplayName= "PudimNetMon Agent"';
end;

// Runs on every install (including upgrades over an existing service) to
// refresh the ImagePath/start type, mirroring the create-or-reconfigure logic
// that used to live in the agent binary.
function GetServiceConfigParams(Param: String): String;
var
  ExePath: String;
begin
  ExePath := ExpandConstant('{app}\{#MyAppExeName}');
  Result := 'config PudimNetMonAgent start= auto binPath= "\"' + ExePath +
            '\" --node-id=' + NodeIdValue + '"';
end;

// Starts the service after registration. Inno Setup keeps the just-installed
// executable open (for its rollback / RestartManager bookkeeping) until the
// Setup process itself exits, so a synchronous `sc start` launched from here
// fails with error 2 for the entire install. Defer the start to a detached
// helper that outlives Setup: it waits a few seconds for Setup to exit (and
// for any real-time AV scan of the new exe to finish), then starts the
// service. The service is also configured AUTO_START, so a failed helper is
// non-fatal -- it will come up on the next boot.
procedure StartAgentService();
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\cmd.exe'),
       '/c ping -n 6 127.0.0.1 >nul & sc.exe start PudimNetMonAgent >nul 2>&1',
       '', SW_HIDE, ewNoWait, ResultCode);
  Log('Service start deferred to a detached helper after Setup exits');
end;

// Persists the wizard settings to %ProgramData%\PudimNetMon\agent.conf.
// Hooked as BeforeInstall of the service-registration [Run] entry so the file
// always exists before the service starts, independent of setup-step ordering.
// Agent precedence: built-in defaults < agent.conf < service command line.
// node-id intentionally lives on the service command line (not here), so every
// setting has exactly one authoritative source.
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
var
  ResultCode: Integer;
begin
  if CurStep = ssInstall then
  begin
    // Let an existing installation be upgraded: stop the service so the exe
    // is not locked while it is being replaced. The result is ignored -- a
    // fresh install has no service to stop, and net/sc both fail harmlessly.
    Exec('{sys}\sc.exe', 'stop PudimNetMonAgent', '', SW_HIDE,
         ewWaitUntilTerminated, ResultCode);
  end;
end;

