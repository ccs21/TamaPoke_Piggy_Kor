param([string]$Port)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()
$form = New-Object System.Windows.Forms.Form
$form.Text = 'TamaPoke 배터리 기록 가져오기'
$form.Size = New-Object System.Drawing.Size(600, 290)
$form.StartPosition = 'CenterScreen'
$label = New-Object System.Windows.Forms.Label
$label.Text = "기기의 화면을 켠 뒤 USB로 연결하세요. 두 기기를 각각 가져오면 됩니다."
$label.SetBounds(18, 18, 550, 42)
$ports = New-Object System.Windows.Forms.ComboBox
$ports.DropDownStyle = 'DropDownList'
$ports.SetBounds(18, 66, 200, 28)
$refresh = New-Object System.Windows.Forms.Button
$refresh.Text = '포트 새로고침'
$refresh.SetBounds(232, 64, 140, 30)
$download = New-Object System.Windows.Forms.Button
$download.Text = '기록을 PC에 저장'
$download.SetBounds(18, 110, 354, 40)
$status = New-Object System.Windows.Forms.TextBox
$status.Multiline = $true
$status.ReadOnly = $true
$status.SetBounds(18, 163, 550, 67)
$status.Text = '기록은 이 도구와 같은 폴더의 BatteryLogs에 저장됩니다.'
$form.Controls.AddRange(@($label, $ports, $refresh, $download, $status))
$refreshPorts = {
    $selected = [string]$ports.SelectedItem
    $ports.Items.Clear()
    foreach ($name in ([IO.Ports.SerialPort]::GetPortNames() | Sort-Object)) {
        [void]$ports.Items.Add($name)
    }
    if ($Port -and $ports.Items.Contains($Port)) { $ports.SelectedItem = $Port }
    elseif ($ports.Items.Contains($selected)) { $ports.SelectedItem = $selected }
    elseif ($ports.Items.Count -gt 0) { $ports.SelectedIndex = 0 }
}
$refresh.Add_Click($refreshPorts)
$download.Add_Click({
    if (-not $ports.SelectedItem) { $status.Text = 'COM 포트를 먼저 선택하세요.'; return }
    $download.Enabled = $false
    $refresh.Enabled = $false
    $ports.Enabled = $false
    $serial = $null
    try {
        $status.Text = '기록을 가져오는 중입니다. 잠시 기다려 주세요.'
        [System.Windows.Forms.Application]::DoEvents()
        $serial = New-Object IO.Ports.SerialPort ([string]$ports.SelectedItem), 115200
        $serial.DtrEnable = $false
        $serial.RtsEnable = $false
        $serial.ReadTimeout = 1200
        $serial.WriteTimeout = 1500
        $serial.ReadBufferSize = 262144
        $serial.NewLine = "`n"
        $serial.Open()
        Start-Sleep -Milliseconds 300
        $serial.DiscardInBuffer()
        $serial.WriteLine('POWERLOG')
        $begun = $false
        $finished = $false
        $lines = New-Object 'System.Collections.Generic.List[string]'
        $deadline = [DateTime]::UtcNow.AddSeconds(45)
        while ([DateTime]::UtcNow -lt $deadline) {
            try { $line = $serial.ReadLine().TrimEnd("`r") }
            catch [TimeoutException] { continue }
            if ($line.StartsWith('POWERLOG ERROR')) { throw $line }
            if ($line -eq 'POWERLOG BEGIN v1') { $begun = $true; continue }
            if ($begun -and $line -eq 'POWERLOG END') { $finished = $true; break }
            if ($begun -and ($line.StartsWith('kind,') -or $line.StartsWith('PL,'))) {
                $lines.Add($line)
            }
        }
        if (-not $finished) {
            throw '기록 수신이 완료되지 않았습니다. 새 펌웨어인지, 화면이 켜져 있는지, 다른 프로그램이 포트를 사용 중인지 확인하세요.'
        }
        if ($lines.Count -lt 2) { throw '아직 기록이 없습니다. 기기를 사용한 뒤 다시 시도하세요.' }
        $columnCount = $lines[0].Split(',').Count
        if ($lines | Where-Object { $_.Split(',').Count -ne $columnCount }) {
            throw '불완전한 기록이 수신되었습니다. 다시 가져오세요.'
        }
        $rows = @($lines | ConvertFrom-Csv)
        $device = ($rows[0].device -replace '[^a-zA-Z0-9]', '')
        $folder = Join-Path $PSScriptRoot 'BatteryLogs'
        [void][IO.Directory]::CreateDirectory($folder)
        $path = Join-Path $folder ("battery-{0}-{1}-{2}.csv" -f $device, $ports.SelectedItem, (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
        [IO.File]::WriteAllLines($path, $lines, (New-Object Text.UTF8Encoding($true)))
        $status.Text = "저장 완료 ($($rows.Count)개 기록)`r`n$path"
    } catch {
        $status.Text = $_.Exception.Message
    } finally {
        if ($serial) { if ($serial.IsOpen) { $serial.Close() }; $serial.Dispose() }
        $download.Enabled = $true
        $refresh.Enabled = $true
        $ports.Enabled = $true
    }
})
& $refreshPorts
[void]$form.ShowDialog()
