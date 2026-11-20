# Funcoes compartilhadas por install_fix.ps1, uninstall_fix.ps1 e SpiderFixGUI.ps1.
# Uso: . (Join-Path $PSScriptRoot "SpiderFixCommon.ps1")

$SpiderFixGameFolder = "Marvel's Spider-Man 2"
$SpiderFixGameExe = "Spider-Man2.exe"

function Test-SpiderFixDll([string]$Path) {
    try {
        return ((Get-Item -LiteralPath $Path).VersionInfo.ProductName -eq "SpiderFix")
    } catch {
        return $false
    }
}

function Test-AdminWrite([string]$Dir) {
    try {
        $t = Join-Path $Dir ".spiderfix_write_test"
        [System.IO.File]::WriteAllText($t, "x")
        [System.IO.File]::Delete($t) # .NET: funciona mesmo em -WhatIf.
        return $true
    } catch {
        return $false
    }
}

# Caminho seguro para "...`"$p`"": barra final antes da aspa escaparia a aspa.
function ConvertTo-SpiderFixArgPath([string]$Path) {
    $p = $Path.TrimEnd('\')
    if ($p -match ':$') {
        $p += '\.' # Raiz (D:\) continua valida.
    }
    return $p
}

# Relanca o script como admin, espera e devolve o ExitCode do filho.
function Invoke-SpiderFixElevated([string]$ScriptPath, [string]$GamePath, [bool]$WhatIf) {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($isAdmin) {
        [Console]::Error.WriteLine("ERRO: sem escrita em $GamePath mesmo como administrador.")
        return 1
    }
    $a = "-NoProfile -ExecutionPolicy Bypass -File `"$ScriptPath`" -GamePath `"$(ConvertTo-SpiderFixArgPath $GamePath)`""
    if ($WhatIf) {
        $a += " -WhatIf"
    }
    try {
        $p = Start-Process powershell -Verb RunAs -ArgumentList $a -Wait -PassThru
        return $p.ExitCode
    } catch {
        # UAC cancelado pelo usuario cai aqui.
        [Console]::Error.WriteLine("ERRO: elevacao cancelada ou falhou: " + $_.Exception.Message)
        return 1
    }
}

function Test-SpiderManDir([string]$Dir) {
    return ($Dir -and (Test-Path -LiteralPath (Join-Path $Dir $SpiderFixGameExe)))
}

# Procura a pasta do jogo em bibliotecas Steam e manifests Epic. $null se nao achar.
function Find-SpiderManPath {
    $roots = @()
    try { $roots += (Get-ItemProperty -Path "HKCU:\Software\Valve\Steam" -Name SteamPath -ErrorAction Stop).SteamPath } catch {}
    try { $roots += (Get-ItemProperty -Path "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam" -Name InstallPath -ErrorAction Stop).InstallPath } catch {}
    $libs = @()
    foreach ($r in $roots) {
        if (!$r) { continue }
        $r = $r -replace '/', '\'
        $libs += $r
        $vdf = Join-Path $r "steamapps\libraryfolders.vdf"
        if (Test-Path -LiteralPath $vdf) {
            foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libs += ($m.Groups[1].Value -replace '\\\\', '\')
            }
        }
    }
    foreach ($l in $libs) {
        $d = Join-Path $l "steamapps\common\$SpiderFixGameFolder"
        if (Test-SpiderManDir $d) {
            return $d
        }
    }
    $epic = Join-Path $env:ProgramData "Epic\EpicGamesLauncher\Data\Manifests"
    if (Test-Path -LiteralPath $epic) {
        foreach ($f in Get-ChildItem -LiteralPath $epic -Filter "*.item" -ErrorAction SilentlyContinue) {
            try {
                $loc = (Get-Content -LiteralPath $f.FullName -Raw | ConvertFrom-Json).InstallLocation
                if (Test-SpiderManDir $loc) {
                    return $loc
                }
            } catch {}
        }
    }
    return $null
}
