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
$repoRoot = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $repoRoot "release\d3d12.dll"
$target = Join-Path $GamePath "d3d12.dll"
$exe = Join-Path $GamePath "Spider-Man2.exe"
$cache = Join-Path $GamePath "cache.pso"
$backupDir = Join-Path $GamePath "SpiderFixBackup_original"
$manifest = Join-Path $backupDir "manifest.json"

function Get-PEMachine([string]$Path) {
    $fs = [System.IO.File]::OpenRead($Path)
    try {
        $br = New-Object System.IO.BinaryReader($fs)
        if ($br.ReadUInt16() -ne 0x5A4D) {
            throw "Sem assinatura MZ: $Path"
        }
        $fs.Position = 0x3C
        $lf = $br.ReadInt32()
        if ($lf -lt 0 -or $lf -gt $fs.Length - 6) {
            throw "Cabecalho PE invalido: $Path"
        }
        $fs.Position = $lf
        if ($br.ReadUInt32() -ne 0x00004550) {
            throw "Sem assinatura PE: $Path"
        }
        return $br.ReadUInt16()
    } finally {
        $br.Close()
    }
}

if (!(Test-Path -LiteralPath $dll)) {
    throw "Nao encontrei release\d3d12.dll."
}

if (!(Test-Path -LiteralPath $exe)) {
    throw "Nao encontrei Spider-Man2.exe em: $GamePath"
}

if ((Get-PEMachine $exe) -ne 0x8664 -or (Get-PEMachine $dll) -ne 0x8664) {
    throw "Arquitetura inesperada: exe e DLL precisam ser x64."
}

$proc = Get-Process -Name "Spider-Man2" -ErrorAction SilentlyContinue
if ($proc) {
    throw "Feche Spider-Man2.exe antes de instalar. Processo em execucao."
}

if (!(Test-AdminWrite $GamePath)) {
    Write-Host "Sem escrita em $GamePath. Relancando como administrador..."
    exit (Invoke-SpiderFixElevated $PSCommandPath $GamePath $PSBoundParameters.ContainsKey("WhatIf"))
}

$dxgi = Join-Path $GamePath "dxgi.dll"
if (Test-Path -LiteralPath $dxgi) {
    Write-Warning "dxgi.dll presente na pasta do jogo (possivel outro mod como OptiScaler)."
}

$dllHash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash

# Sobra de instalacao interrompida: nunca e usada, so removida.
$stale = $target + ".sfnew"
if (Test-Path -LiteralPath $stale) {
    if ($PSCmdlet.ShouldProcess($stale, "Remover copia incompleta anterior")) {
        Remove-Item -LiteralPath $stale -Force
    }
}

if (Test-Path -LiteralPath $target) {
    $targetHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    if ($targetHash -eq $dllHash) {
        Write-Host "Proxy ja instalada (hash igual). Nada a copiar."
        exit 0
    }
    if (Test-SpiderFixDll $target) {
        Write-Host "Proxy antiga detectada pelo marcador. Atualizando sem novo backup."
    } else {
        if (Test-Path -LiteralPath $backupDir) {
            $origHash = $null
            try {
                $origHash = (Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json).originalHash
            } catch {}
            if ($origHash -ne $targetHash) {
                # Backup original ja existe e a dll atual e outra (outro mod): backup novo com timestamp.
                # Milissegundos: dois installs no mesmo segundo nao colidem (colisao pularia o backup).
                $backupDir = Join-Path $GamePath ("SpiderFixBackup_" + (Get-Date -Format "yyyyMMdd_HHmmss_fff"))
                $manifest = Join-Path $backupDir "manifest.json"
                if (Test-Path -LiteralPath $backupDir) {
                    throw "Pasta de backup ja existe: $backupDir. Rode de novo."
                }
            }
        }
        if (!(Test-Path -LiteralPath $backupDir)) {
            # Backup so de arquivo que nao e a proxy.
            if ($PSCmdlet.ShouldProcess($backupDir, "Criar backup original")) {
                # Monta em pasta tmp e renomeia no fim: pasta com nome final sempre tem dll + manifest.
                $tmpDir = Join-Path $GamePath ("SpiderFixBackup_tmp_" + [Guid]::NewGuid().ToString("N"))
                New-Item -ItemType Directory -Path $tmpDir | Out-Null
                try {
                    Copy-SpiderFixAtomic $target (Join-Path $tmpDir "d3d12.dll") $targetHash
                    @{ originalHash = $targetHash; date = (Get-Date -Format "o"); proxyHash = $dllHash } |
                        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $tmpDir "manifest.json") -Encoding Ascii
                    Rename-Item -LiteralPath $tmpDir -NewName (Split-Path -Leaf $backupDir)
                } catch {
                    Remove-Item -LiteralPath $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
                    throw
                }
            }
        }
    }
}

if ($PSCmdlet.ShouldProcess($target, "Instalar proxy d3d12.dll")) {
    # Copia atomica com hash: em falha a d3d12.dll anterior fica intacta.
    Copy-SpiderFixAtomic $dll $target $dllHash
}

Write-Host "Fix instalado em:"
Write-Host $target
Write-Host ""
if (Test-Path -LiteralPath $backupDir) {
    Write-Host "Backup original em:"
    Write-Host $backupDir
    Write-Host ""
}

if (Test-Path -LiteralPath $cache) {
    Write-Host "Dica: se o jogo travar no loading, renomeie cache.pso para cache.pso.disabled."
}
