param([switch]$All)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $MyInvocation.MyCommand.Path

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

function Get-SessionFiles {
    $seen=@{}
    $files=@()
    foreach($p in $patterns){
        foreach($file in Get-ChildItem $root -Filter $p -File -ErrorAction SilentlyContinue){
            if($seen.ContainsKey($file.FullName)){continue}
            $seen[$file.FullName]=$true
            $files+=$file
        }
    }
    return $files
}

function Write-ActiveSession([string]$backend,[string]$variant,[string]$matrix,[string]$session,[datetime]$startedUtc){
    $active=Join-Path $root 'ACTIVE_VR_BACKEND.txt'
    @(
        "backend=$backend"
        "variant=$variant"
        "matrix=$matrix"
        "session=$session"
        "startedUtc=$($startedUtc.ToString('o'))"
        "selected=$((Get-Date).ToString('o'))"
    ) | Set-Content $active -Encoding ascii
}

function Prepare-NextSession([string]$backend,[string]$variant,[string]$matrix){
    $startedUtc=(Get-Date).ToUniversalTime()
    $session=$startedUtc.ToString('yyyyMMddTHHmmssfffZ')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8)
    $sessionRoot=Join-Path $root ("logs/{0}/{1}/{2}" -f $matrix,$variant,$session)
    New-Item -ItemType Directory -Force $sessionRoot|Out-Null

    $ini=Join-Path $root 'OutRun2006Tweaks.ini'
    $configHash=if(Test-Path $ini){(Get-FileHash $ini -Algorithm SHA256).Hash.ToLowerInvariant()}else{'missing'}
    $state=[ordered]@{
        SchemaVersion=2
        BuildMatrixId=$matrix
        VariantId=$variant
        Backend=$backend
        SessionId=$session
        StartedUtc=$startedUtc.ToString('o')
        ConfigSha256=$configHash
        CollectionStatus='prepared-after-previous-collection'
        PreexistingLogs=@()
    }
    $state|ConvertTo-Json -Depth 4|Set-Content (Join-Path $root 'CURRENT_VR_SESSION.json') -Encoding UTF8
    $state|ConvertTo-Json -Depth 4|Set-Content (Join-Path $sessionRoot 'session_manifest.json') -Encoding UTF8
    Write-ActiveSession $backend $variant $matrix $session $startedUtc

    if(Test-Path $ini){
        $allowed='^(Enabled|AutoLaunchHost|AutoEnableWhenHostPresent|RenderBackend|PreferD3D9Ex|DirectGpuOnly|DisableDesktopDuplication|SkyGlowFactor|HudInspector)\s*='
        Get-Content $ini|Where-Object{$_ -match $allowed}|Set-Content (Join-Path $sessionRoot 'VR_CONFIG_SNAPSHOT.txt') -Encoding UTF8
    }
    Copy-Item (Join-Path $root 'ACTIVE_VR_BACKEND.txt') $sessionRoot -Force
    $inputs=Join-Path $root 'BUILD_INPUTS.json'
    if(Test-Path $inputs){Copy-Item $inputs $sessionRoot -Force}
    return $session
}

$running=Get-Process -ErrorAction SilentlyContinue|Where-Object{
    $_.ProcessName -ieq 'OR2006C2C' -or $_.ProcessName -ieq 'outrun-vr-host'
}
if($running){throw 'Close OutRun and outrun-vr-host.exe before collecting logs so the session can be sealed safely.'}

$active=Join-Path $root 'ACTIVE_VR_BACKEND.txt'
if(!(Test-Path $active)){throw 'ACTIVE_VR_BACKEND.txt not found; select a backend first.'}
$kv=@{}
Get-Content $active|ForEach-Object{if($_ -match '^([^=]+)=(.*)$'){$kv[$matches[1]]=$matches[2]}}
$backend=$kv.backend
if(!$backend){throw 'backend identity missing'}
$variant=if($kv.variant){$kv.variant}else{switch($backend){'d3d9'{'A_CONTROL'};'dxvk-safe'{'E_DXVK_SAFE'};'dxvk'{'E_DXVK_MULTIVIEW'};'dx12'{'F_DX12_STRICT'};'2d'{'CONTROL_2D'};default{'UNKNOWN'}}}
$matrix=if($kv.matrix){$kv.matrix}else{'UNIFIED'}

$sessionState=Join-Path $root 'CURRENT_VR_SESSION.json'
if(!(Test-Path $sessionState)){throw 'CURRENT_VR_SESSION.json not found; select the backend again before launching the game.'}
$state=Get-Content $sessionState -Raw|ConvertFrom-Json
$session=$state.SessionId
if(!$session -or $state.Backend -ne $backend -or $state.BuildMatrixId -ne $matrix){throw 'Current session identity does not match the active backend/matrix.'}
$startedUtc=[datetime]::Parse($state.StartedUtc).ToUniversalTime()

$base=Join-Path $root "logs/$matrix"
$dest=Join-Path $base "$variant/$session"
New-Item -ItemType Directory -Force $dest|Out-Null

$copied=@()
$sourceFiles=Get-SessionFiles
foreach($file in $sourceFiles){
    if($file.LastWriteTimeUtc -lt $startedUtc){continue}
    $target=Join-Path $dest $file.Name
    Copy-Item $file.FullName $target -Force
    $copied+=$file.Name
}

Copy-Item $active $dest -Force
Copy-Item $sessionState $dest -Force
$inputs=Join-Path $root 'BUILD_INPUTS.json'
if(Test-Path $inputs){Copy-Item $inputs $dest -Force}

$gameExe=Join-Path $root 'OR2006C2C.EXE'
if(Test-Path $gameExe){
    $exeItem=Get-Item $gameExe
    $exeSha=(Get-FileHash $gameExe -Algorithm SHA256).Hash.ToLowerInvariant()
    $upstreamReferenceSha='68ceb386829066f8455b9d027320af962584321f3e2e8a79c72841495a6134c3'
    $matchesUpstream=($exeSha -eq $upstreamReferenceSha)
    @(
        "filename=$($exeItem.Name)"
        "size=$($exeItem.Length)"
        "sha256=$exeSha"
        "upstreamReferenceSha256=$upstreamReferenceSha"
        "matchesUpstreamReplacementExe=$matchesUpstream"
        "lastWriteUtc=$($exeItem.LastWriteTimeUtc.ToString('o'))"
    )|Set-Content (Join-Path $dest 'EXE_IDENTITY.txt') -Encoding UTF8
    $copied+='EXE_IDENTITY.txt'
}

$gameLogs=Get-ChildItem $dest -Filter 'OutRun2006Tweaks*.log' -File -ErrorAction SilentlyContinue
$shaderLines=@()
foreach($log in $gameLogs){
    $shaderLines += Select-String -Path $log.FullName -Pattern 'VR GPL SHADER:' -SimpleMatch |
        ForEach-Object { $_.Line }
}
if($shaderLines.Count -gt 0){
    @(
        'SHADER FINGERPRINT SUMMARY'
        "session=$session"
        "pairs=$($shaderLines.Count)"
        ''
        $shaderLines
    )|Set-Content (Join-Path $dest 'SHADER_FINGERPRINT_SUMMARY.txt') -Encoding UTF8
    $copied+='SHADER_FINGERPRINT_SUMMARY.txt'
}

$hudCsv=Join-Path $dest 'OutRun2006Tweaks-hudtrace.csv'
if(Test-Path $hudCsv){
    $hudRows=Get-Content $hudCsv |
        Where-Object { $_ -and -not $_.StartsWith('#') } |
        ConvertFrom-Csv
    if($hudRows){
        $summary=@()
        $summary+='HUD TRACE SUMMARY'
        $summary+="session=$session"
        $summary+="rows=$($hudRows.Count)"
        $summary+=''
        $groups=$hudRows |
            Group-Object event,call_rva,known_area,semantic,space_policy,mode,stage,arg0,arg1 |
            ForEach-Object {
                $maxCount=($_.Group | ForEach-Object {[int]$_.count} | Measure-Object -Maximum).Maximum
                [pscustomobject]@{
                    Event=$_.Group[0].event
                    CallRva=$_.Group[0].call_rva
                    KnownArea=$_.Group[0].known_area
                    Semantic=$_.Group[0].semantic
                    SpacePolicy=$_.Group[0].space_policy
                    Mode=$_.Group[0].mode
                    Stage=$_.Group[0].stage
                    Arg0=$_.Group[0].arg0
                    Arg1=$_.Group[0].arg1
                    Arg2=$_.Group[0].arg2
                    Arg3=$_.Group[0].arg3
                    Arg4=$_.Group[0].arg4
                    Arg5=$_.Group[0].arg5
                    Arg6=$_.Group[0].arg6
                    Arg7=$_.Group[0].arg7
                    MaxCount=[int]$maxCount
                }
            } |
            Sort-Object @{Expression={if($_.Semantic -and $_.Semantic -ne 'UNKNOWN'){0}else{1}}}, @{Expression='MaxCount';Descending=$true}, CallRva
        $summary+='event | call_rva | known_area | semantic | space_policy | mode | stage | arg0 | arg1 | arg2 | arg3 | arg4 | arg5 | arg6 | arg7 | observed_count'
        $summary+='------|----------|------------|----------|--------------|------|-------|------|------|------|------|------|------|------|------|---------------'
        foreach($g in ($groups | Select-Object -First 250)){
            $summary+=("$($g.Event) | $($g.CallRva) | $($g.KnownArea) | $($g.Semantic) | $($g.SpacePolicy) | $($g.Mode) | $($g.Stage) | $($g.Arg0) | $($g.Arg1) | $($g.Arg2) | $($g.Arg3) | $($g.Arg4) | $($g.Arg5) | $($g.Arg6) | $($g.Arg7) | $($g.MaxCount)")
        }
        $summary|Set-Content (Join-Path $dest 'HUD_TRACE_SUMMARY.txt') -Encoding UTF8
        $copied+='HUD_TRACE_SUMMARY.txt'

        $expectedSemantics=@(
            'HUD_TIME_ATTACK','HUD_RANK','HUD_GEAR_REV','HUD_GHOST',
            'HUD_GOAL_TIME','HUD_HEART_TOTAL','HUD_RIVAL','HUD_GF_SPEECH',
            'HUD_RANK_EMOJI','HUD_RANK_TEXT','HUD_GF_WARNING','HUD_SLIPSTREAM',
            'HUD_FRUIT','WORLD_RIVAL_MARKER','WORLD_HEART'
        )
        $observedSemantics=@($hudRows |
            Where-Object {$_.semantic -and $_.semantic -ne 'UNKNOWN'} |
            Select-Object -ExpandProperty semantic -Unique)
        $coverage=@(
            'HUD SEMANTIC COVERAGE',
            "session=$session",
            'status means observed in this session, not pass/fail; unobserved modes may simply not have appeared.',
            '',
            'semantic | status',
            '---------|-------'
        )
        foreach($semanticName in $expectedSemantics){
            $status=if($observedSemantics -contains $semanticName){'OBSERVED'}else{'NOT_OBSERVED_THIS_SESSION'}
            $coverage+=("$semanticName | $status")
        }
        $unknownCount=@($hudRows | Where-Object {!$_.semantic -or $_.semantic -eq 'UNKNOWN'}).Count
        $coverage+=''
        $coverage+=("unknown_rows=$unknownCount")
        $coverage|Set-Content (Join-Path $dest 'HUD_SEMANTIC_COVERAGE.txt') -Encoding UTF8
        $copied+='HUD_SEMANTIC_COVERAGE.txt'
    }
}

$payloadBackend=if($backend -eq '2d' -or $backend -eq 'dxvk-safe'){'d3d9'}else{$backend}
$source=Join-Path $root "backends/$payloadBackend/SOURCE_SHA.txt"
$sha=if(Test-Path $source){(Get-Content $source -Raw).Trim()}else{'unknown'}
$configHash=if(Test-Path (Join-Path $root 'OutRun2006Tweaks.ini')){(Get-FileHash (Join-Path $root 'OutRun2006Tweaks.ini') -Algorithm SHA256).Hash.ToLowerInvariant()}else{'missing'}

@(
    "VARIANT=$variant"
    "BACKEND=$backend"
    "SESSION=$session"
    "SESSION_STARTED_UTC=$($startedUtc.ToString('o'))"
    "BUILD_MATRIX=$matrix"
    "SOURCE_SHA=$sha"
    "CONFIG_SHA256=$configHash"
    "FILES=$($copied -join ',')"
    'LOG_BOUNDARY=clean-session-root'
)|Set-Content (Join-Path $dest 'MANIFEST.txt') -Encoding UTF8

@{
    SchemaVersion=2
    VariantId=$variant
    Backend=$backend
    SessionId=$session
    SessionStartedUtc=$startedUtc.ToString('o')
    BuildMatrixId=$matrix
    GitSha=$sha
    ConfigSha256=$configHash
    CollectedAtUtc=(Get-Date).ToUniversalTime().ToString('o')
    CollectedFiles=$copied
    LogBoundary='clean-session-root'
}|ConvertTo-Json -Depth 4|Set-Content (Join-Path $dest 'variant_manifest.json') -Encoding UTF8

$resultFile=Join-Path $dest 'TEST_RESULT.txt'
if(!(Test-Path $resultFile)){
    @('FPS=','HMD_SMOOTHNESS=','STEREO=','RECENTER=','HUD_RANK_SCORE=','SKY_CLOUD=','SMOKE_SKID=','MENU_CAR=','EXIT_YES_NO=','NOTES=')|Set-Content $resultFile -Encoding UTF8
}

if($All){
    $zipName='OutRun2_VR_MATRIX_LOGS_'+$matrix+'_'+$session+'.zip'
    $zip=Join-Path $root $zipName
    if(Test-Path $zip){Remove-Item $zip -Force}
    Compress-Archive -Path "$base/*" -DestinationPath $zip
}else{
    $zipName='OutRun2_VR_LOGS_'+$matrix+'_'+$variant+'_'+$session+'.zip'
    $zip=Join-Path $root $zipName
    if(Test-Path $zip){Remove-Item $zip -Force}
    Compress-Archive -Path "$dest/*" -DestinationPath $zip
}

foreach($file in $sourceFiles){
    if($copied -contains $file.Name -and (Test-Path $file.FullName)){
        Remove-Item $file.FullName -Force
    }
}

$nextSession=Prepare-NextSession $backend $variant $matrix
Write-Host "Diagnostic archive: $zip"
Write-Host "Next test session prepared automatically: $nextSession"
Write-Host 'You do NOT need to run the collector before the next test.'