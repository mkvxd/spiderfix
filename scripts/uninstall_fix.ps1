[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$GamePath
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "SpiderFixCommon.ps1")

if (!$GamePath) {
    # Sem -GamePath: tenta achar o jogo (Steam/Epic).
    $GamePath = Find-SpiderManPath
    if (!$GamePath) {
        throw "Pasta do jogo nao encontrada automaticamente. Use -GamePath."
    }
    Write-Host "Pasta do jogo detectada: $GamePath"
}
$GamePath = (Resolve-Path -LiteralPath $GamePath).ProviderPath
$target = Join-Path $GamePath "d3d12.dll"
$cacheDisabled = Join-Path $GamePath "cache.pso.disabled"
$cache = Join-Path $GamePath "cache.pso"
$gameLog = Join-Path $GamePath "spiderfix.log"

$proc = Get-Process -Name "Spider-Man2" -ErrorAction SilentlyContinue
if ($proc) {
    throw "Feche Spider-Man2.exe antes de remover. Processo em execucao."
}

if (!(Test-AdminWrite $GamePath)) {
    Write-Host "Sem escrita em $GamePath. Relancando como administrador..."
    exit (Invoke-SpiderFixElevated $PSCommandPath $GamePath $PSBoundParameters.ContainsKey("WhatIf"))
}

# Timestampados primeiro (mais novo antes, timestamp esta no nome), backup fixo por ultimo.
$fixed = Join-Path $GamePath "SpiderFixBackup_original"
$candidates = @(Get-ChildItem -LiteralPath $GamePath -Filter "SpiderFixBackup_*" -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -ne $fixed } |
    Sort-Object Name -Descending)
if (Test-Path -LiteralPath $fixed) {
    $candidates += Get-Item -LiteralPath $fixed
}

# Se a d3d12.dll atual nao e a proxy (outro mod por cima), nao restaura nem remove.
$foreign = (Test-Path -LiteralPath $target) -and -not (Test-SpiderFixDll $target)
if ($foreign) {
    $candidates = @()
}

$restored = $false
foreach ($b in $candidates) {
    $cand = Join-Path $b.FullName "d3d12.dll"
    if (!(Test-Path -LiteralPath $cand)) {
        continue
    }
    $man = Join-Path $b.FullName "manifest.json"
    if (Test-Path -LiteralPath $man) {
        try {
            $m = Get-Content -LiteralPath $man -Raw | ConvertFrom-Json
            $candHash = (Get-FileHash -LiteralPath $cand -Algorithm SHA256).Hash
            if (-not $m.originalHash -or ($m.originalHash -ne $candHash)) {
                continue # Manifest nao bate: nao restaura.
            }
        } catch {
            continue
        }
    } elseif (Test-SpiderFixDll $cand) {
        continue # Sem manifest e candidato e a propria proxy: recusa.
    }
    if ($PSCmdlet.ShouldProcess($target, "Restaurar d3d12.dll original")) {
        Copy-Item -LiteralPath $cand -Destination $target -Force
    }
    Write-Host "d3d12.dll original restaurada de:"
    Write-Host $cand
    $restored = $true
    break
}

if (-not $restored) {
    if (Test-Path -LiteralPath $target) {
        $refuse = $false
        if ($foreign) {
            # Pode ser outro mod (OptiScaler, ReShade): nao apaga nem sobrescreve.
            Write-Warning "d3d12.dll atual nao e a proxy (outro mod?). Restauracao/remocao recusada."
            $refuse = $true
        }
        if (-not $refuse) {
            if ($PSCmdlet.ShouldProcess($target, "Remover d3d12.dll da proxy")) {
                Remove-Item -LiteralPath $target -Force
            }
            Write-Host "d3d12.dll removida. Jogo volta a usar System32."
        }
    } else {
        Write-Host "Nenhuma d3d12.dll encontrada na pasta do jogo."
    }
}

if ((Test-Path -LiteralPath $cacheDisabled) -and !(Test-Path -LiteralPath $cache)) {
    if ($PSCmdlet.ShouldProcess($cache, "Restaurar cache.pso")) {
        Rename-Item -LiteralPath $cacheDisabled -NewName "cache.pso"
    }
    Write-Host "cache.pso restaurado."
}

# Limpeza: pastas de backup so se restaurou; senao mantem (original pode estar nelas).
$backups = @(Get-ChildItem -LiteralPath $GamePath -Filter "SpiderFixBackup_*" -Directory -ErrorAction SilentlyContinue)
if ($restored) {
    $backups | ForEach-Object {
        if ($PSCmdlet.ShouldProcess($_.FullName, "Remover pasta de backup")) {
            Remove-Item -LiteralPath $_.FullName -Recurse -Force
        }
    }
} elseif ($backups.Count -gt 0) {
    Write-Warning ("Backup(s) nao restaurado(s), mantidos em:`n" + (($backups | ForEach-Object { $_.FullName }) -join "`n"))
}
if (Test-Path -LiteralPath $gameLog) {
    if ($PSCmdlet.ShouldProcess($gameLog, "Remover log do proxy")) {
        Remove-Item -LiteralPath $gameLog -Force
    }
}

Write-Host "Fix removido."
