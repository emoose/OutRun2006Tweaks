param(
    [Parameter(Mandatory=$true)]
    [ValidateSet("2d","d3d9","dxvk-safe","dxvk","dx12")]
    [string]$Backend
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$backendRoot = Join-Path $root "backends"
$payloadBackend = if ($Backend -eq "2d" -or $Backend -eq "dxvk-safe") { "d3d9" } else { $Backend }
$src = Join-Path $backendRoot $payloadBackend
if (-not (Test-Path $src)) { throw "Backend payload not found: $src" }

$logPatterns=@(
    'OutRun2006Tweaks*.log',
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

function Get-SessionFiles {
    $seen=@{}
    $files=@()
    foreach($pattern in $logPatterns){
        foreach($file in Get-ChildItem $root -Filter $pattern -File -ErrorAction SilentlyContinue){
            if($seen.ContainsKey($file.FullName)){continue}
            $seen[$file.FullName]=$true
            $files+=$file
        }
    }
    return $files
}

function Seal-PendingSessionLogs {
    $files=Get-SessionFiles
    if(-not $files -or $files.Count -eq 0){return}

    $current=Join-Path $root 'CURRENT_VR_SESSION.json'
    $dest=$null
    $oldState=$null
    if(Test-Path $current){
        try{$oldState=Get-Content $current -Raw|ConvertFrom-Json}catch{$oldState=$null}
    }

    if($oldState -and $oldState.SessionId -and $oldState.BuildMatrixId -and $oldState.VariantId){
        $dest=Join-Path $root ("logs/{0}/{1}/{2}" -f $oldState.BuildMatrixId,$oldState.VariantId,$oldState.SessionId)
    }else{
        $stamp=(Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssfffZ')
        $dest=Join-Path $root ("logs/_orphaned/{0}" -f $stamp)
    }
    New-Item -ItemType Directory -Force $dest|Out-Null

    $moved=@()
    foreach($file in $files){
        $target=Join-Path $dest $file.Name
        if(Test-Path $target){
            $prefix=(Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssfffZ')
            $target=Join-Path $dest ($prefix+'_'+$file.Name)
        }
        Move-Item $file.FullName $target -Force
        $moved+=[IO.Path]::GetFileName($target)
    }
    @(
        "AUTO_ARCHIVED_UTC=$((Get-Date).ToUniversalTime().ToString('o'))"
        "REASON=backend-selection-started-new-session"
        "FILES=$($moved -join ',')"
    )|Set-Content (Join-Path $dest 'AUTO_ARCHIVED_ON_NEXT_SESSION.txt') -Encoding UTF8
}

function Copy-Required([string]$name) {
    $p = Join-Path $src $name
    if (-not (Test-Path $p)) { throw "Required backend file missing: $p" }
    Copy-Item $p (Join-Path $root $name) -Force
}

function Remove-RootVerified([string]$name) {
    $p = Join-Path $root $name
    if (Test-Path $p) {
        Remove-Item $p -Force
        if (Test-Path $p) { throw "Could not remove $name. Close OutRun/VR host and retry." }
    }
}

function Repair-IniSectionHeaders([string]$text) {
    $text = $text -replace '(?m)^\[VR\]\\\s*$', '[VR]'
    $text = $text -replace '(?m)^\\\s*$\r?\n?', ''
    return $text
}

function Set-IniSectionValue([string]$text,[string]$section,[string]$key,[string]$value) {
    $escapedSection = [regex]::Escape($section)
    $escapedKey = [regex]::Escape($key)
    $sectionPattern = "(?ms)(^\[$escapedSection\]\s*\r?\n)(.*?)(?=^\[|\z)"
    $m = [regex]::Match($text,$sectionPattern)
    $nl = [Environment]::NewLine
    if (-not $m.Success) {
        if ($text.Length -gt 0 -and -not $text.EndsWith($nl)) { $text += $nl }
        return $text + "[$section]" + $nl + "$key = $value" + $nl
    }

    $body = $m.Groups[2].Value
    $keyPattern = "(?m)^$escapedKey\s*=.*$"
    if ([regex]::IsMatch($body,$keyPattern)) {
        $body = [regex]::Replace($body,$keyPattern,"$key = $value",1)
    } else {
        $body = "$key = $value" + $nl + $body
    }
    return $text.Substring(0,$m.Groups[2].Index) + $body +
        $text.Substring($m.Groups[2].Index + $m.Groups[2].Length)
}

$running = Get-Process -ErrorAction SilentlyContinue | Where-Object {
    $_.ProcessName -ieq "OR2006C2C" -or $_.ProcessName -ieq "outrun-vr-host"
}
if ($running) { throw "OutRun or outrun-vr-host.exe is still running. Close it before switching." }

# Preserve any uncollected logs before touching the backend or starting a new
# session. This makes game-side truncate/overwrite behavior harmless.
Seal-PendingSessionLogs

Copy-Required "dinput8.dll"

if ($Backend -eq "2d") {
    Remove-RootVerified "d3d9.dll"
    Remove-RootVerified "multiviewpatcher.dll"
    Remove-RootVerified "outrun-vr-host.exe"
} else {
    Copy-Required "outrun-vr-host.exe"
    if ($Backend -eq "dxvk") {
        Copy-Required "d3d9.dll"
        Copy-Required "multiviewpatcher.dll"
    } elseif ($Backend -eq "dxvk-safe") {
        $dxvkProvider = Join-Path (Join-Path $backendRoot "dxvk") "d3d9.dll"
        if (-not (Test-Path $dxvkProvider)) { throw "DXVK provider missing: $dxvkProvider" }
        Copy-Item $dxvkProvider (Join-Path $root "d3d9.dll") -Force
        Remove-RootVerified "multiviewpatcher.dll"
    } else {
        Remove-RootVerified "d3d9.dll"
        Remove-RootVerified "multiviewpatcher.dll"
        if (Test-Path (Join-Path $root "d3d9.dll")) {
            throw "Local d3d9.dll is still present; refusing $Backend mode."
        }
    }
}

$ini = Join-Path $root "OutRun2006Tweaks.ini"
if (Test-Path $ini) {
    $text = Get-Content $ini -Raw
    $text = Repair-IniSectionHeaders $text

    if ($Backend -eq "2d") {
        $text = Set-IniSectionValue $text "VR" "RenderBackend" "1"
        $text = Set-IniSectionValue $text "VR" "Enabled" "false"
        $text = Set-IniSectionValue $text "VR" "AutoLaunchHost" "false"
        $text = Set-IniSectionValue $text "VR" "AutoEnableWhenHostPresent" "false"
        $text = Set-IniSectionValue $text "VR" "PreferD3D9Ex" "false"
        $text = Set-IniSectionValue $text "VR" "DirectGpuOnly" "false"
        $text = Set-IniSectionValue $text "VR" "DisableDesktopDuplication" "false"
    } elseif ($Backend -eq "dxvk-safe") {
        $text = Set-IniSectionValue $text "VR" "RenderBackend" "1"
        $text = Set-IniSectionValue $text "VR" "Enabled" "true"
        $text = Set-IniSectionValue $text "VR" "AutoLaunchHost" "true"
        $text = Set-IniSectionValue $text "VR" "AutoEnableWhenHostPresent" "true"
        $text = Set-IniSectionValue $text "VR" "PreferD3D9Ex" "false"
        $text = Set-IniSectionValue $text "VR" "DirectGpuOnly" "false"
        $text = Set-IniSectionValue $text "VR" "DisableDesktopDuplication" "false"
        $text = Set-IniSectionValue $text "Graphics" "TransparencySupersampling" "false"
    } elseif ($Backend -eq "d3d9") {
        # D3D9 SAFE is the correctness/control path. Do not require D3D9Ex or
        # DirectGPU; plain D3D9 + SBS/Desktop Duplication must remain usable.
        $text = Set-IniSectionValue $text "VR" "RenderBackend" "1"
        $text = Set-IniSectionValue $text "VR" "Enabled" "true"
        $text = Set-IniSectionValue $text "VR" "AutoLaunchHost" "true"
        $text = Set-IniSectionValue $text "VR" "AutoEnableWhenHostPresent" "true"
        $text = Set-IniSectionValue $text "VR" "PreferD3D9Ex" "false"
        $text = Set-IniSectionValue $text "VR" "DirectGpuOnly" "false"
        $text = Set-IniSectionValue $text "VR" "DisableDesktopDuplication" "false"
    } else {
        $value = switch ($Backend) {
            "dxvk" { "2" }
            "dx12" { "3" }
        }
        $text = Set-IniSectionValue $text "VR" "RenderBackend" $value
        $text = Set-IniSectionValue $text "VR" "Enabled" "true"
        $text = Set-IniSectionValue $text "VR" "AutoLaunchHost" "true"
        $text = Set-IniSectionValue $text "VR" "AutoEnableWhenHostPresent" "true"
        $text = Set-IniSectionValue $text "VR" "PreferD3D9Ex" "true"
        $text = Set-IniSectionValue $text "VR" "DirectGpuOnly" "true"
        if ($Backend -eq "dxvk") {
            $text = Set-IniSectionValue $text "Graphics" "TransparencySupersampling" "false"
        }
    }
    Set-Content $ini $text -Encoding UTF8
}

$nl = [Environment]::NewLine
$variant = switch ($Backend) {
    "2d"        { "CONTROL_2D" }
    "d3d9"      { "A_CONTROL" }
    "dxvk-safe" { "E_DXVK_SAFE" }
    "dxvk"      { "E_DXVK_MULTIVIEW" }
    "dx12"      { "F_DX12_STRICT" }
}
$matrixFile = Join-Path $root "BUILD_MATRIX_ID.txt"
$matrix = if (Test-Path $matrixFile) { (Get-Content $matrixFile -Raw).Trim() } else { "UNIFIED_LOCAL" }
$startedUtc = (Get-Date).ToUniversalTime()
$session = $startedUtc.ToString("yyyyMMddTHHmmssfffZ") + "-" + [guid]::NewGuid().ToString("N").Substring(0,8)
$sessionRoot = Join-Path $root ("logs/{0}/{1}/{2}" -f $matrix,$variant,$session)
New-Item -ItemType Directory -Force $sessionRoot | Out-Null

$activeText = @(
    "backend=$Backend"
    "variant=$variant"
    "matrix=$matrix"
    "session=$session"
    "startedUtc=$($startedUtc.ToString('o'))"
    "selected=$((Get-Date).ToString('o'))"
) -join $nl
Set-Content (Join-Path $root "ACTIVE_VR_BACKEND.txt") $activeText -Encoding ascii

$configHash = "missing"
if (Test-Path $ini) { $configHash = (Get-FileHash $ini -Algorithm SHA256).Hash.ToLowerInvariant() }
$sessionManifest = [ordered]@{
    SchemaVersion = 2
    BuildMatrixId = $matrix
    VariantId = $variant
    Backend = $Backend
    SessionId = $session
    StartedUtc = $startedUtc.ToString("o")
    ConfigSha256 = $configHash
    CollectionStatus = "started-before-game-launch"
    PreexistingLogs = @()
}
$sessionManifest | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $root "CURRENT_VR_SESSION.json") -Encoding UTF8
$sessionManifest | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $sessionRoot "session_manifest.json") -Encoding UTF8

if (Test-Path $ini) {
    $allowed = '^(Enabled|AutoLaunchHost|AutoEnableWhenHostPresent|RenderBackend|PreferD3D9Ex|DirectGpuOnly|DisableDesktopDuplication|SkyGlowFactor)\s*='
    Get-Content $ini | Where-Object { $_ -match $allowed } |
        Set-Content (Join-Path $sessionRoot "VR_CONFIG_SNAPSHOT.txt") -Encoding UTF8
}
Copy-Item (Join-Path $root "ACTIVE_VR_BACKEND.txt") $sessionRoot -Force
if (Test-Path (Join-Path $root "BUILD_INPUTS.json")) {
    Copy-Item (Join-Path $root "BUILD_INPUTS.json") $sessionRoot -Force
}

Write-Host "OutRun renderer mode activated: $Backend"
Write-Host "Diagnostic session prepared before launch: $session"
Write-Host "Any previous root logs were archived before this session was created."
switch ($Backend) {
    "2d"   { Write-Host "2D ORIGINAL: classic D3D9, VR disabled, D3D9Ex promotion disabled, no VR host." }
    "d3d9" { Write-Host "D3D9 VR SAFE: guarded D3D9Ex/DirectGPU VR path." }
    "dxvk-safe" { Write-Host "DXVK SAFE: classic D3D9 calls translated by DXVK; validated two-pass VR, multiview patcher disabled." }
    "dxvk" { Write-Host "DXVK MULTIVIEW: local d3d9.dll + multiviewpatcher.dll active." }
    "dx12" { Write-Host "DX12 STRICT: local d3d9.dll verified absent; Windows D3D9On12 required." }
}
