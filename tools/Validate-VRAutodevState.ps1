param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
)

$ErrorActionPreference = "Stop"

function Read-JsonFile([string]$RelativePath) {
    $path = Join-Path $RepoRoot $RelativePath
    if (-not (Test-Path $path)) { throw "Missing required file: $RelativePath" }
    try {
        return Get-Content -Raw -Path $path | ConvertFrom-Json -Depth 100
    } catch {
        throw "Invalid JSON in $RelativePath : $($_.Exception.Message)"
    }
}

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

$state = Read-JsonFile "docs/VR_AUTODEV_STATE.json"
$queue = Read-JsonFile "docs/VR_WORK_QUEUE.json"
$feedback = Read-JsonFile "docs/VR_RUNTIME_FEEDBACK.json"
$review = Read-JsonFile "docs/autodev/REVIEW_HANDOFF.json"
$fix = Read-JsonFile "docs/autodev/FIX_HANDOFF.json"
$validation = Read-JsonFile "docs/autodev/VALIDATION_HANDOFF.json"

Require (Test-Path (Join-Path $RepoRoot "docs/VR_AUTODEV_PROTOCOL.md")) "Missing docs/VR_AUTODEV_PROTOCOL.md"
Require ($state.coordination.protocol -eq "docs/VR_AUTODEV_PROTOCOL.md") "State protocol path mismatch"
Require ($state.coordination.queue -eq "docs/VR_WORK_QUEUE.json") "State queue path mismatch"
Require ($state.coordination.runtimeFeedback -eq "docs/VR_RUNTIME_FEEDBACK.json") "State runtime feedback path mismatch"
Require ($state.coordination.authorities.productionSourceWriter -eq "FIX") "Only FIX may own autonomous production source writes"
Require ($state.coordination.authorities.centralQueueWriter -eq "INTEGRATION_PLANNER") "Central queue owner must be INTEGRATION_PLANNER"
Require ($queue.policy.sourceWriteWorker -eq "FIX") "Queue sourceWriteWorker must be FIX"
Require ([int]$queue.policy.maxSourceAttemptsPerFinding -eq 2) "maxSourceAttemptsPerFinding must remain 2"
Require ([int]$queue.policy.maxRuntimeCandidates -le 6) "maxRuntimeCandidates must be <= 6"

$allowed = @("NEW","READY","IMPLEMENTING","IMPLEMENTED","VALIDATING","NEED_HMD_TEST","DONE","BLOCKED","REJECTED","SUPERSEDED")
$ids = @{}
foreach ($item in @($queue.items)) {
    Require (-not [string]::IsNullOrWhiteSpace([string]$item.id)) "Queue item missing id"
    Require (-not $ids.ContainsKey([string]$item.id)) "Duplicate queue id: $($item.id)"
    $ids[[string]$item.id] = $true
    Require ($allowed -contains [string]$item.status) "Invalid status for $($item.id): $($item.status)"
    Require ([int]$item.attempts -le [int]$queue.policy.maxSourceAttemptsPerFinding) "Attempt limit exceeded for $($item.id)"
}
foreach ($item in @($queue.items)) {
    foreach ($dep in @($item.dependsOn)) {
        Require ($ids.ContainsKey([string]$dep)) "Unknown dependency $dep referenced by $($item.id)"
    }
}

Require ($review.producer -eq "REVIEW") "REVIEW handoff producer mismatch"
Require ($fix.producer -eq "FIX") "FIX handoff producer mismatch"
Require ($validation.producer -eq "VALIDATION") "VALIDATION handoff producer mismatch"

$validationStates = @("IDLE","PASS_STATIC","PASS_BUILD_RUNTIME_PENDING","FAIL_BUILD","FAIL_REGRESSION","NEED_HMD_TEST")
Require ($validationStates -contains [string]$validation.status) "Invalid VALIDATION handoff status: $($validation.status)"

if ($fix.status -ne "IDLE") {
    Require (-not [string]::IsNullOrWhiteSpace([string]$fix.queueItemId)) "Active FIX handoff requires queueItemId"
    Require ($ids.ContainsKey([string]$fix.queueItemId)) "FIX handoff references unknown queue item"
}

foreach ($session in @($feedback.sessions)) {
    foreach ($required in @("buildMatrixId","variantId","sessionId","sourceCommit")) {
        Require (-not [string]::IsNullOrWhiteSpace([string]$session.$required)) "Runtime feedback session missing $required"
    }
}

Write-Host "VR autodev coordination state validation passed."
Write-Host ("Queue items: {0}; runtime feedback sessions: {1}" -f @($queue.items).Count, @($feedback.sessions).Count)
