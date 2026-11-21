[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$GamePath,
    [string]$LogFile
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "SpiderFixCommon.ps1")

if ($LogFile) {
    exit (Invoke-SpiderFixWithLog $PSCommandPath $PSBoundParameters)
}

if (!$GamePath) {
    # Sem -GamePath: tenta achar o jogo (Steam/Epic). Drive desconectado nao pode abortar.
    try { $GamePath = Find-SpiderManPath } catch { $GamePath = $null }
    if (!$GamePath) {
        throw "Pasta do jogo nao encontrada automaticamente. Use -GamePath."
    }
    Write-Host "Pasta do jogo detectada: $GamePath"
}
$GamePath = (Resolve-Path -LiteralPath $GamePath).ProviderPath
$target = Join-Path $GamePath "d3d12.dll"
$gameLog = Join-Path $GamePath "spiderfix.log"

$proc = Get-Process -Name "Spider-Man2" -ErrorAction SilentlyContinue
if ($proc) {
    throw "Feche Spider-Man2.exe antes de remover. Processo em execucao."
}

if (!(Test-AdminWrite $GamePath)) {
    Write-Host "Sem escrita em $GamePath. Relancando como administrador..."
    exit (Invoke-SpiderFixElevated $PSCommandPath $GamePath $PSBoundParameters.ContainsKey("WhatIf"))
}

# Sobra de copia atomica interrompida: nunca e usada, so removida.
$stale = $target + ".sfnew"
if (Test-Path -LiteralPath $stale) {
    if ($PSCmdlet.ShouldProcess($stale, "Remover copia incompleta anterior")) {
        Remove-Item -LiteralPath $stale -Force
    }
}

# Timestampados primeiro (mais novo antes, timestamp esta no nome), backup fixo por ultimo.
$fixed = Join-Path $GamePath "SpiderFixBackup_original"
# SpiderFixBackup_tmp_*: backup de instalacao interrompida, nunca restaurado nem apagado.
$leftover = @(Get-ChildItem -LiteralPath $GamePath -Filter "SpiderFixBackup_tmp_*" -Directory -ErrorAction SilentlyContinue)
if ($leftover.Count -gt 0) {
    Write-Warning ("Backup(s) incompleto(s) de instalacao interrompida, ignorado(s) e mantido(s):`n" +
        (($leftover | ForEach-Object { $_.FullName }) -join "`n"))
}
$candidates = @(Get-ChildItem -LiteralPath $GamePath -Filter "SpiderFixBackup_*" -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -ne $fixed -and $_.Name -notlike "SpiderFixBackup_tmp_*" } |
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
    if (!(Test-Path -LiteralPath $man)) {
        continue # Sem manifest (backup incompleto ou antigo): nao restaura.
    }
    try {
        $m = Get-Content -LiteralPath $man -Raw | ConvertFrom-Json
        $candHash = (Get-FileHash -LiteralPath $cand -Algorithm SHA256).Hash
        if (-not $m.originalHash -or ($m.originalHash -ne $candHash)) {
            continue # Manifest nao bate: nao restaura.
        }
    } catch {
        continue
    }
    if ($PSCmdlet.ShouldProcess($target, "Restaurar d3d12.dll original")) {
        # Copia atomica com hash; confere o alvo antes de qualquer remocao de backup.
        Copy-SpiderFixAtomic $cand $target $m.originalHash
        $got = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
        if ($got -ne $m.originalHash) {
            throw "d3d12.dll restaurada diverge do manifest ($got). Backups mantidos."
        }
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

# cache.pso da pasta do jogo e de AppData: so reativa o que tem o marcador do SpiderFix.
# Erro aqui so avisa: a dll ja foi restaurada e a limpeza dos backups precisa rodar.
foreach ($cdir in (Get-SpiderFixCacheDirs $GamePath)) {
    $cacheDisabled = [System.IO.Path]::Combine($cdir, "cache.pso.disabled")
    $cache = [System.IO.Path]::Combine($cdir, "cache.pso")
    $marker = [System.IO.Path]::Combine($cdir, $SpiderFixCacheMarker)
    try {
        if (!(Test-Path -LiteralPath $cacheDisabled) -or (Test-Path -LiteralPath $cache)) { continue }
        if (!(Test-Path -LiteralPath $marker)) {
            Write-Host "cache.pso.disabled mantido (nao desativado pelo SpiderFix) em: $cdir"
            continue
        }
        if ($PSCmdlet.ShouldProcess($cache, "Restaurar cache.pso")) {
            Rename-Item -LiteralPath $cacheDisabled -NewName "cache.pso"
            Remove-Item -LiteralPath $marker -Force
        }
        Write-Host "cache.pso restaurado em: $cdir"
    } catch {
        Write-Warning ("cache.pso nao restaurado em ${cdir}: " + $_.Exception.Message)
    }
}

# Limpeza: pastas de backup so se restaurou; senao mantem (original pode estar nelas).
$backups = @(Get-ChildItem -LiteralPath $GamePath -Filter "SpiderFixBackup_*" -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -notlike "SpiderFixBackup_tmp_*" })
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
