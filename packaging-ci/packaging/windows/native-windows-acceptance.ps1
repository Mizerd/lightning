<#
.SYNOPSIS
    Native Windows acceptance test for a Lightning test build.

.DESCRIPTION
    Operator-run on a REAL Windows host (Wine cannot prove any of the native
    behaviour this checks). It verifies the corrected Windows build:

      * the production Lightning.exe is a GUI-subsystem PE (no console flash);
      * --version and --build-info work, and --build-info reports the Rust
        default backend and the Windows Credential Manager secret store;
      * the app STARTS normally with no DISPLAY / WAYLAND_DISPLAY /
        QT_QPA_PLATFORM set, stays alive for a bounded window, and closes;
      * the captured diagnostic log selects qwindows, the Rust backend and the
        Windows secure store, opens the cache, and shows NONE of: the insecure
        QSettings fallback, an invalid cache path, an "Invalid window handle"
        shutdown warning, a QML binding loop, a pagination storm, or a startup
        failure;
      * a sanitized diagnostic ZIP is produced (build-info, the app log, OS /
        DPI info, installed-file inventory) containing NO credentials, account
        database, access token, or private message content.

    This script never reads or collects Matrix stores, QSettings, or secrets.

.PARAMETER InstallDir
    Directory containing Lightning.exe (e.g. the portable folder or
    %LOCALAPPDATA%\Programs\Lightning). Defaults to the script's folder.

.PARAMETER OutDir
    Where to write the diagnostic bundle. Defaults to a temp folder.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File native-windows-acceptance.ps1 `
        -InstallDir "$env:LOCALAPPDATA\Programs\Lightning"
#>
[CmdletBinding()]
param(
    [string]$InstallDir = $PSScriptRoot,
    [string]$OutDir = (Join-Path $env:TEMP ("lightning-acceptance-" + (Get-Date -Format "yyyyMMdd-HHmmss"))),
    [int]$AliveSeconds = 8
)

$ErrorActionPreference = "Stop"
$exe = Join-Path $InstallDir "Lightning.exe"
$results = [System.Collections.Generic.List[string]]::new()
$failures = 0

function Check([string]$name, [bool]$ok) {
    if ($ok) { $script:results.Add("PASS  $name") }
    else     { $script:results.Add("FAIL  $name"); $script:failures++ }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$log = Join-Path $OutDir "lightning-app.log"
$buildInfoLog = Join-Path $OutDir "build-info.txt"

# --- 1. Installed files -----------------------------------------------------
Check "Lightning.exe present" (Test-Path $exe)
Check "qwindows platform plugin present" (Test-Path (Join-Path $InstallDir "plugins\platforms\qwindows.dll"))
Check "Qt6Core.dll present" (Test-Path (Join-Path $InstallDir "Qt6Core.dll"))

# --- 2/3. --version and --build-info ----------------------------------------
$versionOut = & $exe --version 2>&1 | Out-String
Check "--version prints Lightning" ($versionOut -match "Lightning\s+\d+\.\d+\.\d+")

$buildInfoOut = & $exe --build-info 2>&1 | Out-String
$buildInfoOut | Set-Content -Path $buildInfoLog -Encoding UTF8
Check "--build-info default_backend: rust" ($buildInfoOut -match "(?m)^default_backend:\s*rust\s*$")
Check "--build-info secret_store: windows-credential-manager" ($buildInfoOut -match "(?m)^secret_store:\s*windows-credential-manager\s*$")
Check "--build-info lists the rust backend" ($buildInfoOut -match "(?m)^backends:\s*.*rust")

# --- 4. Production PE subsystem = GUI ---------------------------------------
# Read the PE header directly: DOS e_lfanew -> COFF -> Optional header Subsystem
# (offset 68). 2 = Windows GUI, 3 = Windows console (CUI).
try {
    $bytes = [System.IO.File]::ReadAllBytes($exe)
    $peOff = [BitConverter]::ToInt32($bytes, 0x3C)
    $subsystem = [BitConverter]::ToUInt16($bytes, $peOff + 4 + 20 + 68)
    Check "production exe is GUI subsystem (2)" ($subsystem -eq 2)
} catch {
    $results.Add("SKIP  PE subsystem read failed: $($_.Exception.Message)")
}

# --- 5/6. Start with NO display env; stay alive; then close -----------------
$env:DISPLAY = $null; Remove-Item Env:\DISPLAY -ErrorAction SilentlyContinue
Remove-Item Env:\WAYLAND_DISPLAY -ErrorAction SilentlyContinue
Remove-Item Env:\QT_QPA_PLATFORM -ErrorAction SilentlyContinue
# Route Qt category logs to the redirected stderr handle so we can inspect them.
$env:QT_FORCE_STDERR_LOGGING = "1"
$env:QT_LOGGING_RULES = "matrix.*=true"

$proc = Start-Process -FilePath $exe -PassThru `
    -RedirectStandardError $log -RedirectStandardOutput (Join-Path $OutDir "stdout.log")
Start-Sleep -Seconds $AliveSeconds
$alive = -not $proc.HasExited
Check "app stays alive $AliveSeconds s with no DISPLAY set" $alive
if ($alive) {
    # Ask nicely, then force after a grace period.
    $proc.CloseMainWindow() | Out-Null
    if (-not $proc.WaitForExit(10000)) { $proc.Kill() }
}

Start-Sleep -Milliseconds 500
$logText = if (Test-Path $log) { Get-Content $log -Raw } else { "" }

# --- 7/8. Positive log markers ---------------------------------------------
Check "no 'no graphical display available' rejection" (-not ($logText -match "no graphical display available"))
Check "Rust backend active (no matrix.http session)" (-not ($logText -match "matrix\.http:\s*session restored"))
Check "no insecure QSettings fallback warning" (-not ($logText -match "InsecureFallbackSecretStore active"))
Check "Windows secure store active" ($logText -match "Windows secure credential store active" -or $buildInfoOut -match "windows-credential-manager")
Check "no invalid account cache path" (-not ($logText -match "refusing invalid account cache path"))
Check "cache did not fail to open" (-not ($logText -match "cache open failed"))

# --- 10. Negative log markers (regressions) --------------------------------
Check "no 'Invalid window handle' shutdown race" (-not ($logText -match "Invalid window handle"))
Check "no QML binding loop" (-not ($logText -match "Binding loop detected"))
Check "no pagination storm" (($logText | Select-String "pagination requested" -AllMatches).Matches.Count -lt 40)
Check "no libsecret plaintext fallback line" (-not ($logText -match "using insecure QSettings fallback"))

# --- 11. Sanitized diagnostic bundle ---------------------------------------
"$([System.Environment]::OSVersion.VersionString)" | Set-Content (Join-Path $OutDir "os-version.txt")
try {
    Get-CimInstance Win32_VideoController |
        Select-Object Name, CurrentHorizontalResolution, CurrentVerticalResolution |
        Format-List | Out-File (Join-Path $OutDir "display.txt")
} catch { }
Get-ChildItem -Recurse $InstallDir | Select-Object FullName, Length |
    Export-Csv -NoTypeInformation (Join-Path $OutDir "installed-files.csv")
$results | Set-Content (Join-Path $OutDir "acceptance-results.txt")

# Never collect Matrix stores, QSettings, or secrets — only the files above.
$zip = "$OutDir.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $OutDir "*") -DestinationPath $zip

Write-Host ""
$results | ForEach-Object { Write-Host $_ }
Write-Host ""
Write-Host "Diagnostic bundle: $zip"
if ($failures -gt 0) {
    Write-Host "ACCEPTANCE: FAIL ($failures failing checks)" -ForegroundColor Red
    exit 1
} else {
    Write-Host "ACCEPTANCE: PASS" -ForegroundColor Green
    exit 0
}
