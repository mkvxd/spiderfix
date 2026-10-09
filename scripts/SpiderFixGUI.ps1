#Requires -Version 5.1
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$repoRoot = Split-Path -Parent $PSScriptRoot
$installScript = Join-Path $PSScriptRoot "install_fix.ps1"
$uninstallScript = Join-Path $PSScriptRoot "uninstall_fix.ps1"

$form = New-Object System.Windows.Forms.Form
$form.Text = "SpiderFix - Gerenciador"
$form.Size = New-Object System.Drawing.Size(460, 340)
$form.StartPosition = "CenterScreen"
$form.FormBorderStyle = "FixedDialog"
$form.MaximizeBox = $false

$y = 12
$lbl = New-Object System.Windows.Forms.Label
$lbl.Text = "Pasta do jogo:"
$lbl.Location = New-Object System.Drawing.Point(12, $y)
$lbl.Size = New-Object System.Drawing.Size(90, 20)
$form.Controls.Add($lbl)

$txtPath = New-Object System.Windows.Forms.TextBox
$txtPath.Location = New-Object System.Drawing.Point(12, $y + 22)
$txtPath.Size = New-Object System.Drawing.Size(320, 20)
$txtPath.Text = "C:\Program Files (x86)\Steam\steamapps\common\Marvel's Spider-Man 2"
$form.Controls.Add($txtPath)

$btnBrowse = New-Object System.Windows.Forms.Button
$btnBrowse.Text = "..."
$btnBrowse.Location = New-Object System.Drawing.Point(340, $y + 20)
$btnBrowse.Size = New-Object System.Drawing.Size(90, 24)
$btnBrowse.Add_Click({
    $d = New-Object System.Windows.Forms.FolderBrowserDialog
    $d.Description = "Selecione a pasta do jogo"
    if ($d.ShowDialog() -eq "OK") {
        $txtPath.Text = $d.SelectedPath
    }
})
$form.Controls.Add($btnBrowse)

$log = New-Object System.Windows.Forms.TextBox
$log.Location = New-Object System.Drawing.Point(12, 120)
$log.Size = New-Object System.Drawing.Size(418, 170)
$log.Multiline = $true
$log.ScrollBars = "Vertical"
$log.ReadOnly = $true
$form.Controls.Add($log)

function Write-Log([string]$m) {
    $log.AppendText($m + "`r`n")
}

function Invoke-FixScript([string]$script) {
    Write-Log "> $script"
    try {
        $out = & powershell -ExecutionPolicy Bypass -File $script -GamePath $txtPath.Text 2>&1 |
            Out-String
        Write-Log $out
    } catch {
        Write-Log ("ERRO: " + $_.Exception.Message)
    }
}

$btnInstall = New-Object System.Windows.Forms.Button
$btnInstall.Text = "Instalar"
$btnInstall.Location = New-Object System.Drawing.Point(12, 70)
$btnInstall.Size = New-Object System.Drawing.Size(100, 30)
$btnInstall.Add_Click({ Invoke-FixScript $installScript })
$form.Controls.Add($btnInstall)

$btnUninstall = New-Object System.Windows.Forms.Button
$btnUninstall.Text = "Desinstalar"
$btnUninstall.Location = New-Object System.Drawing.Point(120, 70)
$btnUninstall.Size = New-Object System.Drawing.Size(100, 30)
$btnUninstall.Add_Click({ Invoke-FixScript $uninstallScript })
$form.Controls.Add($btnUninstall)

$btnCache = New-Object System.Windows.Forms.Button
$btnCache.Text = "Alternar cache.pso"
$btnCache.Location = New-Object System.Drawing.Point(228, 70)
$btnCache.Size = New-Object System.Drawing.Size(130, 30)
$btnCache.Add_Click({
    $c = Join-Path $txtPath.Text "cache.pso"
    $cd = Join-Path $txtPath.Text "cache.pso.disabled"
    try {
        if ((Test-Path -LiteralPath $c) -and !(Test-Path -LiteralPath $cd)) {
            Rename-Item -LiteralPath $c -NewName "cache.pso.disabled"
            Write-Log "cache.pso desativado."
        } elseif ((Test-Path -LiteralPath $cd) -and !(Test-Path -LiteralPath $c)) {
            Rename-Item -LiteralPath $cd -NewName "cache.pso"
            Write-Log "cache.pso ativado."
        } else {
            Write-Log "Nada a alternar."
        }
    } catch {
        Write-Log ("ERRO: " + $_.Exception.Message)
    }
})
$form.Controls.Add($btnCache)

$btnDiag = New-Object System.Windows.Forms.Button
$btnDiag.Text = "Diagnostico"
$btnDiag.Location = New-Object System.Drawing.Point(366, 70)
$btnDiag.Size = New-Object System.Drawing.Size(100, 30)
$btnDiag.Add_Click({
    try {
        $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
        $out = Join-Path ([Environment]::GetFolderPath("Desktop")) "SpiderFixDiag_$stamp.txt"
        $sb = New-Object System.Text.StringBuilder
        [void]$sb.AppendLine("SpiderFix diagnostico $stamp")
        $gp = $txtPath.Text
        $dll = Join-Path $gp "d3d12.dll"
        if (Test-Path -LiteralPath $dll) {
            $h = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
            [void]$sb.AppendLine("d3d12.dll SHA256: $h")
            [void]$sb.AppendLine("Produto: " + (Get-Item -LiteralPath $dll).VersionInfo.ProductName)
        } else {
            [void]$sb.AppendLine("d3d12.dll ausente na pasta do jogo.")
        }
        $pl = Join-Path $gp "spiderfix.log"
        if (Test-Path -LiteralPath $pl) {
            [void]$sb.AppendLine("--- spiderfix.log (ultimas 40) ---")
            [void]$sb.AppendLine((Get-Content -LiteralPath $pl -Tail 40 | Out-String))
        }
        try {
            $gpu = Get-CimInstance Win32_VideoController -ErrorAction Stop |
                Select-Object -First 1 Name, DriverVersion
            [void]$sb.AppendLine("GPU: $($gpu.Name) driver $($gpu.DriverVersion)")
        } catch {
            [void]$sb.AppendLine("GPU: nao lida")
        }
        [System.IO.File]::WriteAllText($out, $sb.ToString())
        Write-Log "Diagnostico em: $out"
    } catch {
        Write-Log ("ERRO: " + $_.Exception.Message)
    }
})
$form.Controls.Add($btnDiag)

[void]$form.ShowDialog()
