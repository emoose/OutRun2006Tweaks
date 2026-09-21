param(
    [string]$GameExe = 'OR2006C2C.EXE'
)

$ErrorActionPreference='Stop'
$root=Split-Path -Parent $MyInvocation.MyCommand.Path
$active=Join-Path $root 'ACTIVE_VR_BACKEND.txt'
$current=Join-Path $root 'CURRENT_VR_SESSION.json'
$collector=Join-Path $root 'Collect-OutRunVRLogs.ps1'
$selector=Join-Path $root 'Select-OutRunVRBackend.ps1'
$game=Join-Path $root $GameExe

if(!(Test-Path $active) -or !(Test-Path $current)){
    throw 'Select a renderer/backend once before using the test launcher.'
}
if(!(Test-Path $collector)){throw 'Collect-OutRunVRLogs.ps1 not found.'}
if(!(Test-Path $selector)){throw 'Select-OutRunVRBackend.ps1 not found.'}
if(!(Test-Path $game)){throw "Game executable not found: $game"}

$running=Get-Process -ErrorAction SilentlyContinue|Where-Object{
    $_.ProcessName -ieq 'OR2006C2C' -or $_.ProcessName -ieq 'outrun-vr-host'
}
if($running){throw 'OutRun or outrun-vr-host.exe is already running.'}

$kv=@{}
Get-Content $active|ForEach-Object{if($_ -match '^([^=]+)=(.*)$'){$kv[$matches[1]]=$matches[2]}}
$backend=$kv.backend
if(!$backend){throw 'Active backend identity is missing.'}

# If stale root logs exist, re-selecting the same backend seals them into the
# previous session and creates a clean session before launch.
$patterns=@(
    'OutRun2006Tweaks*.log',
    'OutRun2006Tweaks-hudtrace*.csv',
    'outrun-vr-host*.log',
    'outrun-vr-host-pipeline*.log',
    'outrun-vr-watchdog*.log',
    'backend*.log',
    'OR2006C2C_d3d9.log',
    'OR2006C2C_dxgi.log',
    'OR2006C2C_d3d11.log',
    'OR2006C2C_vkd3d*.log',
    'dxvk*.log',
    'vkd3d*.log',
    'crash.log',
    'OR2006C2C.EXE.*.zip',
    '*.dmp'
)
$stale=$false
foreach($pattern in $patterns){
    if(Get-ChildItem $root -Filter $pattern -File -ErrorAction SilentlyContinue|Select-Object -First 1){
        $stale=$true
        break
    }
}
if($stale){
    & $selector -Backend $backend
    if($LASTEXITCODE -and $LASTEXITCODE -ne 0){throw 'Failed to seal stale logs before launch.'}
}

$state=Get-Content $current -Raw|ConvertFrom-Json
Write-Host "Starting test session: $($state.SessionId)"
Write-Host "Backend: $backend"

# Recovery retest policy: first prove that the game can leave the white startup
# screen before re-enabling high-refresh interpolation / XR phase-lock. These
# command-line values are session-scoped and override both base and user INIs.
$recoveryBootArgs=@(
    '-FramerateLimit=60',
    '-FramerateFastLoad=0',
    '-FramerateInterpolation=false',
    '-FramerateUnlockExperimental=false',
    '-FrameCadenceMode=0',
    '-DisableDesktopVsync=false',
    '-SkyGlowFactor=1',
    '-HudInspector=true'
)
$gameArgs=@($recoveryBootArgs)
if($backend -eq 'd3d9' -or $backend -eq 'dxvk-safe'){
    $gameArgs += @(
        '-PreferD3D9Ex=false',
        '-DirectGpuOnly=false',
        '-DisableDesktopDuplication=false'
    )
}

$sessionRoot=Join-Path $root ("logs/{0}/{1}/{2}" -f $state.BuildMatrixId,$state.VariantId,$state.SessionId)
New-Item -ItemType Directory -Force $sessionRoot|Out-Null
@(
    "backend=$backend"
    "forceVrDisabled=$($backend -eq '2d')"
    "arguments=$($gameArgs -join ' ')"
)|Set-Content (Join-Path $sessionRoot 'RUN_OVERRIDES.txt') -Encoding UTF8
Write-Host "Recovery boot overrides: $($gameArgs -join ' ')"

# DXVK/Vulkan safety: third-party implicit capture/overlay layers can crash
# vkCreateInstance before DXVK gets control. The observed Bandicam path was
# bdcamvk32.dll -> NVIDIA vkCreateInstance. Run the DXVK comparison with
# implicit layers disabled and clear legacy forced instance layers.
$dxvkMode = $backend -eq 'dxvk-safe' -or $backend -eq 'dxvk'
$oldVkDisable = $env:VK_LOADER_LAYERS_DISABLE
$oldVkInstanceLayers = $env:VK_INSTANCE_LAYERS
$oldVkDebug = $env:VK_LOADER_DEBUG
$oldVrForceDisabled = $env:OUTRUN_VR_FORCE_DISABLED
$oldShaderFingerprint = $env:OUTRUN_VR_SHADER_FINGERPRINT
$env:OUTRUN_VR_SHADER_FINGERPRINT='1'
if($backend -eq '2d'){
    $env:OUTRUN_VR_FORCE_DISABLED='1'
    Write-Host '2D control isolation: all OpenXR/VR hook installers are disabled for this process.'
}else{
    $env:OUTRUN_VR_FORCE_DISABLED=$null
}
if($dxvkMode){
    $env:VK_LOADER_LAYERS_DISABLE='~implicit~'
    $env:VK_INSTANCE_LAYERS=$null
    $env:VK_LOADER_DEBUG='error,warn,layer'
    $bandicam=Get-Process -ErrorAction SilentlyContinue|Where-Object{
        $_.ProcessName -match '^bdcam' -or $_.ProcessName -match 'bandicam'
    }
    if($bandicam){
        Write-Warning 'Bandicam process detected. Vulkan implicit layers are disabled for this launch; close Bandicam too if DXVK still crashes.'
    }
    Write-Host 'DXVK Vulkan safety: implicit layers disabled for this test process.'
}

try{
    $p=Start-Process -FilePath $game -ArgumentList $gameArgs -WorkingDirectory $root -PassThru
} finally {
    $env:OUTRUN_VR_FORCE_DISABLED=$oldVrForceDisabled
    $env:OUTRUN_VR_SHADER_FINGERPRINT=$oldShaderFingerprint
    if($dxvkMode){
        $env:VK_LOADER_LAYERS_DISABLE=$oldVkDisable
        $env:VK_INSTANCE_LAYERS=$oldVkInstanceLayers
        $env:VK_LOADER_DEBUG=$oldVkDebug
    }
}
$p.WaitForExit()

# Give the auto-launched host a short chance to flush and exit normally.
$deadline=(Get-Date).AddSeconds(15)
do{
    $hostProc=Get-Process -Name 'outrun-vr-host' -ErrorAction SilentlyContinue
    if(!$hostProc){break}
    Start-Sleep -Milliseconds 500
}while((Get-Date) -lt $deadline)

if(Get-Process -Name 'outrun-vr-host' -ErrorAction SilentlyContinue){
    Write-Warning 'outrun-vr-host.exe is still running. Close it, then run Collect-OutRunVRLogs.cmd once. No logs were deleted.'
    exit 2
}

& $collector
if($LASTEXITCODE -and $LASTEXITCODE -ne 0){exit $LASTEXITCODE}
