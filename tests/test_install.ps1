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
    $before = (Get-ChildItem -LiteralPath $sb -Recurse -File | Get-FileHash -Algorithm SHA256 |
        ForEach-Object { "$($_.Path)|$($_.Hash)" }) -join ";"
    & $install -GamePath $sb -WhatIf
    & $uninstall -GamePath $sb -WhatIf
    $after = (Get-ChildItem -LiteralPath $sb -Recurse -File | Get-FileHash -Algorithm SHA256 |
        ForEach-Object { "$($_.Path)|$($_.Hash)" }) -join ";"
    Assert-True ($before -eq $after) "WhatIf sem efeito colateral (hashes)"
} finally {
    Remove-Item -LiteralPath $sb -Recurse -Force
}

function Get-Sha([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

# 6: manifest com hash divergente: uninstall nao restaura e mantem o backup.
$sb = New-GameSandbox
try {
    $t = Join-Path $sb "d3d12.dll"
    $bak = Join-Path $sb "SpiderFixBackup_original"
    Copy-Item -LiteralPath "C:\Windows\System32\version.dll" -Destination $t -Force
    $origHash = Get-Sha $t
    & $install -GamePath $sb
    $man = Join-Path $bak "manifest.json"
    $m = Get-Content -LiteralPath $man -Raw | ConvertFrom-Json
    $m.originalHash = "0000"
    $m | ConvertTo-Json | Set-Content -LiteralPath $man -Encoding Ascii
    & $uninstall -GamePath $sb 3>$null
    Assert-True (Test-Path -LiteralPath (Join-Path $bak "d3d12.dll")) "manifest divergente mantem backup"
    Assert-True ((Get-Sha (Join-Path $bak "d3d12.dll")) -eq $origHash) "backup mantido intacto"
    Assert-True (!(Test-Path -LiteralPath $t) -or (Get-Sha $t) -ne $origHash) "manifest divergente nao restaura"
} finally {
    Remove-Item -LiteralPath $sb -Recurse -Force
}

# 7: outro mod por cima da proxy: uninstall recusa e deixa o mod intacto.
$sb = New-GameSandbox
try {
    $t = Join-Path $sb "d3d12.dll"
    Copy-Item -LiteralPath "C:\Windows\System32\version.dll" -Destination $t -Force
    & $install -GamePath $sb
    Copy-Item -LiteralPath "C:\Windows\System32\winmm.dll" -Destination $t -Force
    $modHash = Get-Sha $t
    & $uninstall -GamePath $sb 3>$null
    Assert-True ((Get-Sha $t) -eq $modHash) "uninstall nao sobrescreve outro mod"
    Assert-True (Test-Path -LiteralPath (Join-Path $sb "SpiderFixBackup_original\d3d12.dll")) "backup mantido com outro mod"
} finally {
    Remove-Item -LiteralPath $sb -Recurse -Force
}

# 8: reinstall sobre outro mod com backup existente: backup timestampado e uninstall restaura o mod.
$sb = New-GameSandbox
try {
    $t = Join-Path $sb "d3d12.dll"
    Copy-Item -LiteralPath "C:\Windows\System32\version.dll" -Destination $t -Force
    & $install -GamePath $sb
    Copy-Item -LiteralPath "C:\Windows\System32\winmm.dll" -Destination $t -Force
    $modHash = Get-Sha $t
    & $install -GamePath $sb
    $ts = @(Get-ChildItem -LiteralPath $sb -Filter "SpiderFixBackup_*" -Directory |
        Where-Object { $_.Name -match '^SpiderFixBackup_\d{8}_\d{6}$' })
    Assert-True ($ts.Count -eq 1) "backup timestampado criado"
    if ($ts.Count -eq 1) {
        Assert-True ((Get-Sha (Join-Path $ts[0].FullName "d3d12.dll")) -eq $modHash) "backup timestampado guarda o mod"
        $m = Get-Content -LiteralPath (Join-Path $ts[0].FullName "manifest.json") -Raw | ConvertFrom-Json
        Assert-True ($m.originalHash -eq $modHash) "manifest timestampado com hash do mod"
    }
    Assert-True ((Get-Sha $t) -eq $releaseHash) "proxy instalada sobre o mod"
    & $uninstall -GamePath $sb
    Assert-True ((Get-Sha $t) -eq $modHash) "uninstall restaura o mod (backup mais novo)"
} finally {
    Remove-Item -LiteralPath $sb -Recurse -Force
}

if ($script:failures -gt 0) {
    throw "$($script:failures) teste(s) falharam."
}
Write-Output "Todos os testes de install passaram."
