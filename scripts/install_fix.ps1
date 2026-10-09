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
    Start-Process powershell -Verb RunAs -ArgumentList "-NoExit -ExecutionPolicy Bypass -File `"$PSCommandPath`" -GamePath `"$GamePath`""
    exit 0
}

$dxgi = Join-Path $GamePath "dxgi.dll"
if (Test-Path -LiteralPath $dxgi) {
    Write-Warning "dxgi.dll presente na pasta do jogo (possivel outro mod como OptiScaler)."
}

$dllHash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash

if (Test-Path -LiteralPath $target) {
    $targetHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    if ($targetHash -eq $dllHash) {
        Write-Host "Proxy ja instalada (hash igual). Nada a copiar."
        exit 0
    }
    if (Test-SpiderFixDll $target) {
        Write-Host "Proxy antiga detectada pelo marcador. Atualizando sem novo backup."
    } elseif (!(Test-Path -LiteralPath $backupDir)) {
        # Backup unico e fixo, so de arquivo que nao e a proxy.
        if ($PSCmdlet.ShouldProcess($backupDir, "Criar backup original")) {
            New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
            Copy-Item -LiteralPath $target -Destination (Join-Path $backupDir "d3d12.dll") -Force
            @{ originalHash = $targetHash; date = (Get-Date -Format "o"); proxyHash = $dllHash } |
                ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding Ascii
        }
    }
}

if ($PSCmdlet.ShouldProcess($target, "Instalar proxy d3d12.dll")) {
    Copy-Item -LiteralPath $dll -Destination $target -Force

    $dstHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    if ($dstHash -ne $dllHash) {
        $bak = Join-Path $backupDir "d3d12.dll"
        if (Test-Path -LiteralPath $bak) {
            Copy-Item -LiteralPath $bak -Destination $target -Force
            Write-Host "Backup restaurado apos falha."
        }
        throw "Hash diverge apos copia. Esperado $dllHash, obtido $dstHash."
    }
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
