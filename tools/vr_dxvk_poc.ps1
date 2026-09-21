param(
    [Parameter(Mandatory = $true)]
    [string]$GameDir,

    [Parameter(Mandatory = $true)]
    [string]$DxvkD3D9,

    [switch]$Run,

    [string]$GameArgs = ""
)

$ErrorActionPreference = "Stop"

function Get-PeMachine([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 256) { throw "PE file is too small: $Path" }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -lt 0 -or $peOffset + 6 -gt $bytes.Length) { throw "Invalid PE header: $Path" }
    return [BitConverter]::ToUInt16($bytes, $peOffset + 4)
}

$gameRoot = (Resolve-Path $GameDir).Path
$exe = Join-Path $gameRoot "OR2006C2C.EXE"
$dxvk = (Resolve-Path $DxvkD3D9).Path

if (-not (Test-Path $exe)) { throw "OR2006C2C.EXE not found in $gameRoot" }
if (-not (Test-Path $dxvk)) { throw "DXVK d3d9.dll not found: $DxvkD3D9" }

$gameMachine = Get-PeMachine $exe
$dxvkMachine = Get-PeMachine $dxvk
if ($gameMachine -ne 0x014C) { throw ("Expected 32-bit OutRun executable, PE machine=0x{0:X4}" -f $gameMachine) }
if ($dxvkMachine -ne 0x014C) { throw ("Use the x32 DXVK d3d9.dll, PE machine=0x{0:X4}" -f $dxvkMachine) }

$target = Join-Path $gameRoot "d3d9.dll"
$backup = Join-Path $gameRoot "d3d9.outrun-vr-poc-backup.dll"
$logDir = Join-Path $gameRoot "vr-dxvk-poc-logs"

Write-Host "OutRun executable : $exe"
Write-Host "DXVK x86 d3d9.dll: $dxvk"
Write-Host "Target            : $target"
Write-Host "Log directory     : $logDir"

if (-not $Run) {
    Write-Host ""
    Write-Host "Probe-only checks passed. Nothing was copied or launched."
    Write-Host "Run again with -Run to temporarily install x32 DXVK, launch OutRun, collect logs, and restore the original d3d9.dll."
    exit 0
}

if (Test-Path $backup) {
    throw "Safety backup already exists: $backup. Resolve it manually before another PoC run."
}

New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$hadOriginal = Test-Path $target
$process = $null
$restoreSafe = $true
$oldLogPath = $env:DXVK_LOG_PATH
$oldLogLevel = $env:DXVK_LOG_LEVEL
$oldHud = $env:DXVK_HUD

try {
    if ($hadOriginal) {
        Move-Item -LiteralPath $target -Destination $backup
        Write-Host "Backed up existing d3d9.dll -> $backup"
    }

    Copy-Item -LiteralPath $dxvk -Destination $target
    $env:DXVK_LOG_PATH = $logDir
    $env:DXVK_LOG_LEVEL = "info"
    $env:DXVK_HUD = "devinfo,fps,compiler"

    Write-Host "Launching isolated DXVK compatibility test. Close the game normally when the test is complete."
    if ([string]::IsNullOrWhiteSpace($GameArgs)) {
        $process = Start-Process -FilePath $exe -WorkingDirectory $gameRoot -PassThru
    } else {
        $process = Start-Process -FilePath $exe -WorkingDirectory $gameRoot -ArgumentList $GameArgs -PassThru
    }
    $process.WaitForExit()
    Write-Host "OutRun exited with code $($process.ExitCode)."
} finally {
    if ($process -and -not $process.HasExited) {
        $restoreSafe = $false
        Write-Warning "OutRun is still running. DXVK was left in place to avoid replacing a loaded DLL. Close the game, then restore $backup manually."
    }

    if ($restoreSafe) {
        if (Test-Path $target) { Remove-Item -LiteralPath $target -Force }
        if ($hadOriginal -and (Test-Path $backup)) {
            Move-Item -LiteralPath $backup -Destination $target
            Write-Host "Restored original d3d9.dll."
        }
    }

    $env:DXVK_LOG_PATH = $oldLogPath
    $env:DXVK_LOG_LEVEL = $oldLogLevel
    $env:DXVK_HUD = $oldHud
}

$logs = Get-ChildItem -Path $logDir -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending
if ($logs) {
    Write-Host ""
    Write-Host "DXVK logs:"
    $logs | ForEach-Object { Write-Host "  $($_.FullName)" }
    Write-Host ""
    Write-Host "PoC gate: confirm D3D9 initialization, gameplay rendering, Reset/alt-tab, HDR/window behavior, and absence of new input/FFB regressions before any Vulkan VR work."
} else {
    Write-Warning "No DXVK log was produced. Treat the PoC as failed until the loader path is understood."
}
