[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)]
    [string]$GamePath
)

$ErrorActionPreference = "Stop"

$GamePath = (Resolve-Path -LiteralPath $GamePath).ProviderPath
$repoRoot = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $repoRoot "release\d3d12.dll"

$target = Join-Path $GamePath "d3d12.dll"
$cacheDisabled = Join-Path $GamePath "cache.pso.disabled"
$cache = Join-Path $GamePath "cache.pso"
$gameLog = Join-Path $GamePath "spiderfix.log"

function Test-AdminWrite([string]$Dir) {
    try {
        $t = Join-Path $Dir ".spiderfix_write_test"
        [System.IO.File]::WriteAllText($t, "x")
        Remove-Item -LiteralPath $t -Force
        return $true
    } catch {
        return $false
    }
}

$proc = Get-Process -Name "Spider-Man2" -ErrorAction SilentlyContinue
if ($proc) {
    throw "Feche Spider-Man2.exe antes de remover. Processo em execucao."
}

if (!(Test-AdminWrite $GamePath)) {
    Write-Host "Sem escrita em $GamePath. Relancando como administrador..."
    Start-Process powershell -Verb RunAs -ArgumentList "-ExecutionPolicy Bypass -File `"$PSCommandPath`" -GamePath `"$GamePath`""
    exit 0
}

$proxyHash = $null
if (Test-Path -LiteralPath $dll) {
    $proxyHash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
}

# Backup fixo primeiro, depois timestampados por nome (timestamp esta no nome).
$fixed = Join-Path $GamePath "SpiderFixBackup_original"
$candidates = @()
if (Test-Path -LiteralPath $fixed) {
    $candidates += Get-Item -LiteralPath $fixed
}
$candidates += Get-ChildItem -LiteralPath $GamePath -Filter "SpiderFixBackup_*" -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -ne $fixed } |
    Sort-Object Name -Descending

$restored = $false
foreach ($b in $candidates) {
    $cand = Join-Path $b.FullName "d3d12.dll"
    if (!(Test-Path -LiteralPath $cand)) {
        continue
    }
    $candHash = (Get-FileHash -LiteralPath $cand -Algorithm SHA256).Hash
    if ($proxyHash -and ($candHash -eq $proxyHash)) {
        continue # Backup contaminado com a propria proxy: recusa restaurar.
    }
    $man = Join-Path $b.FullName "manifest.json"
    if (Test-Path -LiteralPath $man) {
        try {
            $m = Get-Content -LiteralPath $man -Raw | ConvertFrom-Json
            if ($m.originalHash -and ($m.originalHash -ne $candHash)) {
                continue
            }
        } catch {
            continue
        }
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
        if ($proxyHash) {
            $targetHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
            if ($targetHash -ne $proxyHash) {
                # Pode ser outro mod (OptiScaler, ReShade): nao apaga.
                Write-Warning "d3d12.dll atual nao e a proxy e nao ha backup. Remocao recusada."
                $refuse = $true
            }
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

# Limpeza: pastas de backup deste fix e log do proxy.
Get-ChildItem -LiteralPath $GamePath -Filter "SpiderFixBackup_*" -Directory -ErrorAction SilentlyContinue |
    ForEach-Object {
        if ($PSCmdlet.ShouldProcess($_.FullName, "Remover pasta de backup")) {
            Remove-Item -LiteralPath $_.FullName -Recurse -Force
        }
    }
if (Test-Path -LiteralPath $gameLog) {
    if ($PSCmdlet.ShouldProcess($gameLog, "Remover log do proxy")) {
        Remove-Item -LiteralPath $gameLog -Force
    }
}

Write-Host "Fix removido."
