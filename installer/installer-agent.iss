; PudimNetMon Agent - Windows install wizard (Inno Setup)
;
; CI (".github/workflows/ci.yml", job cpp-agent-windows) compiles this into a
; self-contained setup EXE that:
;   * collects the agent identity + collector settings on a wizard page,
;   * installs pudim-agent.exe under Program Files,
;   * registers the "PudimNetMonAgent" auto-start Windows service through the
;     agent's own --install-service support (the same code path the legacy
;     scripts\install-agent-windows.ps1 wrapper used),
;   * writes %ProgramData%\PudimNetMon\agent.conf so settings are easy to
;     review/adjust later, and
;   * removes the service again on uninstall via --uninstall-service.
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
; Register the auto-start service via the agent's own installer support, then
; bring it up (failure is only informational; it is configured as auto-start).
Filename: "{app}\{#MyAppExeName}"; Parameters: {code:GetServiceArgs}; StatusMsg: "Registering PudimNetMonAgent service..."; Flags: waituntilterminated runhidden; AfterInstall: StartAgentService

[UninstallRun]
; Remove the service (it stops the service first) before files are deleted.
Filename: "{app}\{#MyAppExeName}"; Parameters: "--uninstall-service"; Flags: waituntilterminated runhidden

[Code]

var
  AgentPage: TInputQueryWizardPage;
  NodeIdValue: String;
  CollectorValue: String;
  IntervalValue: String;

const
  DefaultInterval = '5000';

procedure InitializeWizard();
begin
  // Defaults are captured here so silent installs (which skip the wizard
  // pages) still configure the service sensibly.
  NodeIdValue := GetComputerNameString();
  CollectorValue := '';
  IntervalValue := DefaultInterval;

  AgentPage := CreateInputQueryPage(wpSelectTasks,
    'Agent configuration',
    'Configure the PudimNetMon agent service.',
    'The agent runs as the "PudimNetMonAgent" auto-start Windows service. ' +
    'These values are baked into the service command line and are also saved ' +
    'to %ProgramData%\PudimNetMon\agent.conf, which you can edit later.');

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
    else if StrToIntDef(IntervalValue, 0) <= 0 then
    begin
      MsgBox('Polling interval must be a positive number of milliseconds.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

// Builds the service command-line arguments, mirroring what the legacy
// scripts\install-agent-windows.ps1 -ServiceArgs used to pass.
function GetServiceArgs(Param: String): String;
begin
  Result := '--install-service --node-id="' + NodeIdValue + '" --interval="' +
            IntervalValue + '"';
  if CollectorValue <> '' then
    Result := Result + ' --collector-endpoints="' + CollectorValue + '"';
end;

// Starts the service right after it is registered. Failure is informational:
// the service is configured as auto-start, so it comes up on next boot.
procedure StartAgentService();
var
  ResultCode: Integer;
begin
  if not Exec('net.exe', 'start PudimNetMonAgent', '', SW_HIDE,
              ewWaitUntilTerminated, ResultCode) or (ResultCode <> 0) then
    MsgBox('The PudimNetMonAgent service was registered but could not be ' +
           'started (net start exit ' + IntToStr(ResultCode) + '). It is ' +
           'configured to start automatically and will be available after ' +
           'the next reboot.', mbInformation, MB_OK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ConfDir, ConfPath, Conf: String;
  ResultCode: Integer;
begin
  if CurStep = ssInstall then
  begin
    // Let an existing installation be upgraded: stop the service so the exe
    // is not locked while it is being replaced.
    Exec('net.exe', 'stop PudimNetMonAgent', '', SW_HIDE,
         ewWaitUntilTerminated, ResultCode);
  end
  else if CurStep = ssPostInstall then
  begin
    // Record the wizard settings so they are easy to review/adjust later.
    // Agent precedence: built-in defaults < this file < service command line.
    ConfDir := ExpandConstant('{#MyAppStateDir}');
    ConfPath := ConfDir + '\agent.conf';
    if not DirExists(ConfDir) then
      CreateDir(ConfDir);
    if DirExists(ConfDir) then
    begin
      Conf := '# Generated by the PudimNetMon Agent installer' + #13#10 +
              '# Precedence: built-in defaults < this file < service command line' + #13#10 +
              'node-id=' + NodeIdValue + #13#10 +
              'interval=' + IntervalValue + #13#10;
      if CollectorValue <> '' then
        Conf := Conf + 'collector-endpoints=' + CollectorValue + #13#10;
      SaveStringToFile(ConfPath, Conf, False);
    end;
  end;
end;

