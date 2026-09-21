Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$selector = Join-Path $root "Select-OutRunVRBackend.ps1"

function Get-CurrentBackend {
    $active = Join-Path $root "ACTIVE_VR_BACKEND.txt"
    if (-not (Test-Path $active)) { return "unknown" }
    $line = Get-Content $active | Where-Object { $_ -like "backend=*" } | Select-Object -First 1
    if (-not $line) { return "unknown" }
    return $line.Substring(8).Trim()
}

function Refresh-Status {
    $current = Get-CurrentBackend
    $status.Text = switch ($current) {
        "2d"   { "현재 선택: 2D ORIGINAL (Classic D3D9)" }
        "d3d9" { "현재 선택: D3D9 VR SAFE" }
        "dxvk-safe" { "현재 선택: DXVK SAFE (2-pass)" }
        "dxvk" { "현재 선택: DXVK MULTIVIEW" }
        "dx12" { "현재 선택: DX12 STRICT" }
        default { "현재 선택: 확인되지 않음" }
    }
}

function Start-VRTest {
    $runner = Join-Path $root "Run-OutRunVRTest.cmd"
    if (-not (Test-Path $runner)) {
        [System.Windows.Forms.MessageBox]::Show("Run-OutRunVRTest.cmd 파일이 없습니다.","OutRun VR Test Launcher",[System.Windows.Forms.MessageBoxButtons]::OK,[System.Windows.Forms.MessageBoxIcon]::Error) | Out-Null
        return
    }
    Start-Process -FilePath $runner -WorkingDirectory $root
}

function Select-Backend([string]$backend) {
    if (-not (Test-Path $selector)) {
        [System.Windows.Forms.MessageBox]::Show("Select-OutRunVRBackend.ps1 파일이 없습니다.","OutRun VR Backend Selector",[System.Windows.Forms.MessageBoxButtons]::OK,[System.Windows.Forms.MessageBoxIcon]::Error) | Out-Null
        return
    }

    $outFile = Join-Path $env:TEMP "outrun-vr-selector-out.txt"
    $errFile = Join-Path $env:TEMP "outrun-vr-selector-err.txt"
    Remove-Item $outFile,$errFile -Force -ErrorAction SilentlyContinue

    $args = @("-NoProfile","-ExecutionPolicy","Bypass","-File",$selector,$backend)
    $p = Start-Process powershell -Wait -PassThru -WindowStyle Hidden -ArgumentList $args -RedirectStandardOutput $outFile -RedirectStandardError $errFile

    $output = ""
    if (Test-Path $outFile) { $output += (Get-Content $outFile -Raw) }
    if (Test-Path $errFile) { $output += (Get-Content $errFile -Raw) }

    if ($p.ExitCode -ne 0) {
        if ([string]::IsNullOrWhiteSpace($output)) { $output = "Backend switch failed." }
        [System.Windows.Forms.MessageBox]::Show($output.Trim(),"전환 실패",[System.Windows.Forms.MessageBoxButtons]::OK,[System.Windows.Forms.MessageBoxIcon]::Error) | Out-Null
        return
    }

    Refresh-Status
    $label = switch ($backend) {
        "2d"   { "2D ORIGINAL" }
        "d3d9" { "D3D9 VR SAFE" }
        "dxvk-safe" { "DXVK SAFE" }
        "dxvk" { "DXVK MULTIVIEW" }
        "dx12" { "DX12 STRICT" }
    }
    [System.Windows.Forms.MessageBox]::Show("$label 로 전환했습니다.","전환 완료",[System.Windows.Forms.MessageBoxButtons]::OK,[System.Windows.Forms.MessageBoxIcon]::Information) | Out-Null
}

$form = New-Object System.Windows.Forms.Form
$form.Text = "OutRun 2006 Renderer / VR Selector"
$form.StartPosition = "CenterScreen"
$form.ClientSize = New-Object System.Drawing.Size(500,535)
$form.FormBorderStyle = "FixedDialog"
$form.MaximizeBox = $false

$title = New-Object System.Windows.Forms.Label
$title.Text = "OutRun 2006 Renderer / VR Selector"
$title.Font = New-Object System.Drawing.Font("Segoe UI",16,[System.Drawing.FontStyle]::Bold)
$title.AutoSize = $true
$title.Location = New-Object System.Drawing.Point(58,20)
$form.Controls.Add($title)

$hint = New-Object System.Windows.Forms.Label
$hint.Text = "게임과 outrun-vr-host.exe를 종료한 뒤 선택하세요."
$hint.Font = New-Object System.Drawing.Font("Segoe UI",9)
$hint.AutoSize = $true
$hint.Location = New-Object System.Drawing.Point(62,58)
$form.Controls.Add($hint)

$status = New-Object System.Windows.Forms.Label
$status.Font = New-Object System.Drawing.Font("Segoe UI",10,[System.Drawing.FontStyle]::Bold)
$status.AutoSize = $true
$status.Location = New-Object System.Drawing.Point(62,88)
$form.Controls.Add($status)
Refresh-Status

$buttons = @(
    @{ Text="2D ORIGINAL  (Classic D3D9 / VR OFF)"; Backend="2d"; Y=125 },
    @{ Text="D3D9 VR SAFE"; Backend="d3d9"; Y=187 },
    @{ Text="DXVK SAFE  (2-pass / compatibility)"; Backend="dxvk-safe"; Y=249 },
    @{ Text="DXVK MULTIVIEW  (experimental)"; Backend="dxvk"; Y=311 },
    @{ Text="DX12 STRICT"; Backend="dx12"; Y=373 }
)

foreach ($b in $buttons) {
    $btn = New-Object System.Windows.Forms.Button
    $btn.Text = $b.Text
    $btn.Size = New-Object System.Drawing.Size(370,50)
    $btn.Location = New-Object System.Drawing.Point(64,$b.Y)
    $btn.Font = New-Object System.Drawing.Font("Segoe UI",10,[System.Drawing.FontStyle]::Bold)
    $btn.Tag = $b.Backend
    $btn.Add_Click({ Select-Backend $this.Tag })
    $form.Controls.Add($btn)
}

$runBtn = New-Object System.Windows.Forms.Button
$runBtn.Text = "현재 모드 테스트 실행  (종료 후 로그 자동수집)"
$runBtn.Size = New-Object System.Drawing.Size(370,55)
$runBtn.Location = New-Object System.Drawing.Point(64,438)
$runBtn.Font = New-Object System.Drawing.Font("Segoe UI",10,[System.Drawing.FontStyle]::Bold)
$runBtn.Add_Click({ Start-VRTest })
$form.Controls.Add($runBtn)

[void]$form.ShowDialog()
