#Requires -Version 5.1
# Testes dos instaladores em sandbox (pasta temporaria). Zero dependencia.
# Uso: powershell -ExecutionPolicy Bypass -File tests/test_install.ps1
# Saida 0 = tudo passou. Qualquer falha lanca excecao.

[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$install = Join-Path $repoRoot "scripts\install_fix.ps1"
$uninstall = Join-Path $repoRoot "scripts\uninstall_fix.ps1"
$releaseDll = Join-Path $repoRoot "release\d3d12.dll"

$script:failures = 0

function Assert-True([bool]$cond, [string]$name) {
    if ($cond) {
        Write-Output "PASS: $name"
    } else {
        Write-Output "FAIL: $name"
        $script:failures++
    }
}

function New-GameSandbox {
    $sb = Join-Path ([System.IO.Path]::GetTempPath()) ("spiderfix_test_" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $sb | Out-Null
    Copy-Item -LiteralPath "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe" `
        -Destination (Join-Path $sb "Spider-Man2.exe") -Force
    return $sb
}

if (!(Test-Path -LiteralPath $releaseDll)) {
    throw "release\d3d12.dll ausente. Rode build_spider_proxy.ps1 antes."
}
$releaseHash = (Get-FileHash -LiteralPath $releaseDll -Algorithm SHA256).Hash

# 1: install limpo sem dll previa nao cria backup e copia.
$sb = New-GameSandbox
try {
    & $install -GamePath $sb
    Assert-True (Test-Path -LiteralPath (Join-Path $sb "d3d12.dll")) "install copia sem previa"
    Assert-True (!(Test-Path -LiteralPath (Join-Path $sb "SpiderFixBackup_original"))) "sem backup sem previa"
    Assert-True ((Get-FileHash -LiteralPath (Join-Path $sb "d3d12.dll") -Algorithm SHA256).Hash -eq $releaseHash) "hash apos copia"
} finally {
    Remove-Item -LiteralPath $sb -Recurse -Force
}

# 2: segunda install nao re-backupa a propria proxy (item 1 do relatorio).
$sb = New-GameSandbox
try {
    Copy-Item -LiteralPath "C:\Windows\System32\version.dll" -Destination (Join-Path $sb "d3d12.dll") -Force
    $origHash = (Get-FileHash -LiteralPath (Join-Path $sb "d3d12.dll") -Algorithm SHA256).Hash
    & $install -GamePath $sb
    & $install -GamePath $sb
    $bakHash = (Get-FileHash -LiteralPath (Join-Path $sb "SpiderFixBackup_original\d3d12.dll") -Algorithm SHA256).Hash
    Assert-True ($bakHash -eq $origHash) "backup guarda original, nao proxy"
    $man = Get-Content -LiteralPath (Join-Path $sb "SpiderFixBackup_original\manifest.json") -Raw |
        ConvertFrom-Json
    Assert-True ($man.originalHash -eq $origHash) "manifest com hash original"
} finally {
    Remove-Item -LiteralPath $sb -Recurse -Force
}

# 3: uninstall restaura original e limpa backups.
$sb = New-GameSandbox
try {
    Copy-Item -LiteralPath "C:\Windows\System32\version.dll" -Destination (Join-Path $sb "d3d12.dll") -Force
    $origHash = (Get-FileHash -LiteralPath (Join-Path $sb "d3d12.dll") -Algorithm SHA256).Hash
    & $install -GamePath $sb
    & $uninstall -GamePath $sb
    Assert-True ((Get-FileHash -LiteralPath (Join-Path $sb "d3d12.dll") -Algorithm SHA256).Hash -eq $origHash) "uninstall restaura original"
    Assert-True (@(Get-ChildItem -LiteralPath $sb -Filter "SpiderFixBackup_*" -Directory -ErrorAction SilentlyContinue).Count -eq 0) "uninstall limpa backups"
} finally {
    Remove-Item -LiteralPath $sb -Recurse -Force
}

# 4: uninstall recusa apagar dll de outro mod sem backup (item 19).
$sb = New-GameSandbox
try {
    Copy-Item -LiteralPath "C:\Windows\System32\version.dll" -Destination (Join-Path $sb "d3d12.dll") -Force
    & $uninstall -GamePath $sb
    Assert-True (Test-Path -LiteralPath (Join-Path $sb "d3d12.dll")) "recusa apagar mod estranho"
} finally {
    Remove-Item -LiteralPath $sb -Recurse -Force
}

# 5: -WhatIf nao toca em nada.
$sb = New-GameSandbox
try {
    $before = @(Get-ChildItem -LiteralPath $sb -Recurse | Select-Object FullName)
    & $install -GamePath $sb -WhatIf
    & $uninstall -GamePath $sb -WhatIf
    $after = @(Get-ChildItem -LiteralPath $sb -Recurse | Select-Object FullName)
    Assert-True ($before.Count -eq $after.Count) "WhatIf sem efeito colateral"
} finally {
    Remove-Item -LiteralPath $sb -Recurse -Force
}

if ($script:failures -gt 0) {
    throw "$($script:failures) teste(s) falharam."
}
Write-Output "Todos os testes de install passaram."
