#Requires -Version 5.1
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

# WinForms precisa de STA; relanca se necessario.
if ([Threading.Thread]::CurrentThread.ApartmentState -ne 'STA') {
    Start-Process powershell -ArgumentList "-NoProfile -STA -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit 0
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

. (Join-Path $PSScriptRoot "SpiderFixCommon.ps1")
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
$detected = Find-SpiderManPath
if ($detected) {
    $txtPath.Text = $detected
} else {
    $txtPath.Text = "C:\Program Files (x86)\Steam\steamapps\common\$SpiderFixGameFolder"
}
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

$lblStatus = New-Object System.Windows.Forms.Label
$lblStatus.Location = New-Object System.Drawing.Point(12, 106)
$lblStatus.Size = New-Object System.Drawing.Size(418, 18)
$form.Controls.Add($lblStatus)

function Update-Status {
    $gp = $txtPath.Text
    $d = Join-Path $gp "d3d12.dll"
    if (!(Test-SpiderManDir $gp)) {
        $lblStatus.Text = "Status: pasta sem $SpiderFixGameExe"
    } elseif (!(Test-Path -LiteralPath $d)) {
        $lblStatus.Text = "Status: Nao instalado"
    } elseif (Test-SpiderFixDll $d) {
        $lblStatus.Text = "Status: Instalado (SpiderFix v" + (Get-Item -LiteralPath $d).VersionInfo.ProductVersion + ")"
    } else {
        $lblStatus.Text = "Status: Outra d3d12.dll presente (outro mod)"
    }
}
$txtPath.Add_TextChanged({ try { Update-Status } catch {} })

$log = New-Object System.Windows.Forms.TextBox
$log.Location = New-Object System.Drawing.Point(12, 128)
$log.Size = New-Object System.Drawing.Size(418, 162)
$log.Multiline = $true
$log.ScrollBars = "Vertical"
$log.ReadOnly = $true
$form.Controls.Add($log)

function Write-Log([string]$m) {
    $log.AppendText($m + "`r`n")
}

function Invoke-FixScript([string]$script) {
    $ErrorActionPreference = "Continue"
    Write-Log "> $script"
    $outF = [System.IO.Path]::GetTempFileName()
    $errF = [System.IO.Path]::GetTempFileName()
    $buttons | ForEach-Object { $_.Enabled = $false }
    try {
        $a = "-NoProfile -ExecutionPolicy Bypass -File `"$script`""
        if ($txtPath.Text) {
            $a += " -GamePath `"$(ConvertTo-SpiderFixArgPath $txtPath.Text)`""
        }
        $proc = Start-Process powershell -ArgumentList $a -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput $outF -RedirectStandardError $errF
        $null = $proc.Handle # PS5.1: sem isso ExitCode vem vazio.
        # Espera sem travar a janela.
        while (!$proc.HasExited) {
            [System.Windows.Forms.Application]::DoEvents()
            Start-Sleep -Milliseconds 100
        }
        $proc.WaitForExit()
        $o = [System.IO.File]::ReadAllText($outF)
        $e = [System.IO.File]::ReadAllText($errF)
        if ($o) { Write-Log $o }
        if ($e) { Write-Log ("STDERR:`r`n" + $e) }
        if ($proc.ExitCode -eq 0) {
            Write-Log "OK (codigo 0)."
        } else {
            Write-Log ("FALHOU (codigo " + $proc.ExitCode + ").")
        }
    } catch {
        Write-Log ("ERRO: " + $_.Exception.Message)
    } finally {
        Remove-Item -LiteralPath $outF, $errF -Force -ErrorAction SilentlyContinue
        $buttons | ForEach-Object { $_.Enabled = $true }
        try { Update-Status } catch {}
    }
}

$btnInstall = New-Object System.Windows.Forms.Button
$btnInstall.Text = "Instalar"
$btnInstall.Location = New-Object System.Drawing.Point(12, 70)
$btnInstall.Size = New-Object System.Drawing.Size(96, 30)
$btnInstall.Add_Click({ Invoke-FixScript $installScript })
$form.Controls.Add($btnInstall)

$btnUninstall = New-Object System.Windows.Forms.Button
$btnUninstall.Text = "Desinstalar"
$btnUninstall.Location = New-Object System.Drawing.Point(114, 70)
$btnUninstall.Size = New-Object System.Drawing.Size(96, 30)
$btnUninstall.Add_Click({ Invoke-FixScript $uninstallScript })
$form.Controls.Add($btnUninstall)

$btnCache = New-Object System.Windows.Forms.Button
$btnCache.Text = "Alternar cache.pso"
$btnCache.Location = New-Object System.Drawing.Point(216, 70)
$btnCache.Size = New-Object System.Drawing.Size(118, 30)
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
$btnDiag.Location = New-Object System.Drawing.Point(340, 70)
$btnDiag.Size = New-Object System.Drawing.Size(90, 30)
$btnDiag.Add_Click({
    try {
        $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
        $out = Join-Path ([Environment]::GetFolderPath("Desktop")) "SpiderFixDiag_$stamp.txt"
        $sb = New-Object System.Text.StringBuilder
        [void]$sb.AppendLine("SpiderFix diagnostico $stamp")
        $gp = $txtPath.Text
        try {
            $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
            [void]$sb.AppendLine("Windows: $($os.Caption) build $($os.BuildNumber)")
        } catch {
            [void]$sb.AppendLine("Windows: nao lido")
        }
        $exe = Join-Path $gp $SpiderFixGameExe
        if (Test-Path -LiteralPath $exe) {
            [void]$sb.AppendLine("$SpiderFixGameExe FileVersion: " + (Get-Item -LiteralPath $exe).VersionInfo.FileVersion)
        } else {
            [void]$sb.AppendLine("$SpiderFixGameExe ausente em: $gp")
        }
        $others = @("dxgi.dll", "d3d11.dll", "dinput8.dll", "version.dll", "winmm.dll") |
            Where-Object { Test-Path -LiteralPath (Join-Path $gp $_) }
        [void]$sb.AppendLine("Outras dlls proxy: " + $(if ($others) { $others -join ", " } else { "nenhuma" }))
        $ini = Join-Path $gp "spiderfix.ini"
        if (Test-Path -LiteralPath $ini) {
            [void]$sb.AppendLine("--- spiderfix.ini ---")
            [void]$sb.AppendLine((Get-Content -LiteralPath $ini -Raw))
        }
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
        Write-Log $sb.ToString()
        Write-Log "Diagnostico em: $out"
    } catch {
        Write-Log ("ERRO: " + $_.Exception.Message)
    }
})
$form.Controls.Add($btnDiag)

$buttons = @($btnBrowse, $btnInstall, $btnUninstall, $btnCache, $btnDiag)
try { Update-Status } catch {}

[void]$form.ShowDialog()
