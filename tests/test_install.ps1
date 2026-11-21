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

# AppData falso para todo o arquivo: uninstall e toggle de cache nunca tocam no %APPDATA% real.
$oldAppData = $env:SPIDERFIX_APPDATA
$fakeAppData = Join-Path ([System.IO.Path]::GetTempPath()) ("spiderfix_appdata_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $fakeAppData | Out-Null
$env:SPIDERFIX_APPDATA = $fakeAppData
try {

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
            Where-Object { $_.Name -match '^SpiderFixBackup_\d{8}_\d{6}_\d{3}$' })
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

    . (Join-Path $repoRoot "scripts\SpiderFixCommon.ps1")

    # 9: install interrompido deixou d3d12.dll.sfnew: install remove a sobra e instala.
    $sb = New-GameSandbox
    try {
        $t = Join-Path $sb "d3d12.dll"
        [System.IO.File]::WriteAllText("$t.sfnew", "lixo")
        & $install -GamePath $sb
        Assert-True (!(Test-Path -LiteralPath "$t.sfnew")) "install remove .sfnew antigo"
        Assert-True ((Get-Sha $t) -eq $releaseHash) "install apos .sfnew antigo"
    } finally {
        Remove-Item -LiteralPath $sb -Recurse -Force
    }

    # 10: copia atomica com hash errado falha sem tocar no destino nem deixar .sfnew.
    $sb = New-GameSandbox
    try {
        $t = Join-Path $sb "d3d12.dll"
        Copy-Item -LiteralPath "C:\Windows\System32\version.dll" -Destination $t -Force
        $origHash = Get-Sha $t
        $threw = $false
        try { Copy-SpiderFixAtomic $releaseDll $t "0000" } catch { $threw = $true }
        Assert-True $threw "copia atomica lanca com hash divergente"
        Assert-True ((Get-Sha $t) -eq $origHash) "copia atomica falha deixa destino intacto"
        Assert-True (!(Test-Path -LiteralPath "$t.sfnew")) "copia atomica falha remove .sfnew"
    } finally {
        Remove-Item -LiteralPath $sb -Recurse -Force
    }

    # 11: backup sem manifest nao e restaurado e e mantido.
    $sb = New-GameSandbox
    try {
        $t = Join-Path $sb "d3d12.dll"
        $bak = Join-Path $sb "SpiderFixBackup_original"
        New-Item -ItemType Directory -Path $bak | Out-Null
        Copy-Item -LiteralPath "C:\Windows\System32\version.dll" -Destination (Join-Path $bak "d3d12.dll") -Force
        $origHash = Get-Sha (Join-Path $bak "d3d12.dll")
        Copy-Item -LiteralPath $releaseDll -Destination $t -Force
        & $uninstall -GamePath $sb 3>$null
        Assert-True (!(Test-Path -LiteralPath $t) -or (Get-Sha $t) -ne $origHash) "sem manifest nao restaura"
        Assert-True (Test-Path -LiteralPath (Join-Path $bak "d3d12.dll")) "sem manifest mantem backup"
    } finally {
        Remove-Item -LiteralPath $sb -Recurse -Force
    }

    # 12: SpiderFixBackup_tmp_* (backup interrompido) nao e usado nem apagado.
    $sb = New-GameSandbox
    try {
        $t = Join-Path $sb "d3d12.dll"
        Copy-Item -LiteralPath "C:\Windows\System32\version.dll" -Destination $t -Force
        $origHash = Get-Sha $t
        & $install -GamePath $sb
        $tmpBak = Join-Path $sb "SpiderFixBackup_tmp_0123"
        New-Item -ItemType Directory -Path $tmpBak | Out-Null
        Copy-Item -LiteralPath "C:\Windows\System32\winmm.dll" -Destination (Join-Path $tmpBak "d3d12.dll") -Force
        @{ originalHash = (Get-Sha (Join-Path $tmpBak "d3d12.dll")) } | ConvertTo-Json |
            Set-Content -LiteralPath (Join-Path $tmpBak "manifest.json") -Encoding Ascii
        & $uninstall -GamePath $sb 3>$null
        Assert-True ((Get-Sha $t) -eq $origHash) "tmp ignorado, restaura o original"
        Assert-True (Test-Path -LiteralPath (Join-Path $tmpBak "d3d12.dll")) "tmp mantido apos uninstall"
        Assert-True (!(Test-Path -LiteralPath (Join-Path $sb "SpiderFixBackup_original"))) "backup valido limpo apos restaurar"
    } finally {
        Remove-Item -LiteralPath $sb -Recurse -Force
    }

    # 13: deteccao com biblioteca em drive inexistente e caminho com acento (vdf UTF-8 sem BOM).
    $fake = Join-Path ([System.IO.Path]::GetTempPath()) ("spiderfix_steam_" + [Guid]::NewGuid().ToString("N"))
    try {
        $steam = Join-Path $fake "Steam"
        $lib = Join-Path $fake ("Bibliot" + [char]0xE9 + "ca") # e com acento agudo; arquivo de teste fica ASCII.
        $game = [System.IO.Path]::Combine($lib, "steamapps\common", $SpiderFixGameFolder)
        New-Item -ItemType Directory -Path (Join-Path $steam "steamapps") -Force | Out-Null
        New-Item -ItemType Directory -Path $game -Force | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $game $SpiderFixGameExe), "x")
        # Primeiro drive livre a partir de Q:.
        for ($c = [int][char]'Q'; (Test-Path -LiteralPath ([char]$c + ":\")) -and $c -lt [int][char]'Z'; $c++) {}
        $missing = [string][char]$c + ":\Lib"
        $vdf = "`"libraryfolders`"`n{`n`t`"0`"`n`t{`n`t`t`"path`"`t`t`"" + ($missing -replace '\\', '\\') +
            "`"`n`t}`n`t`"1`"`n`t{`n`t`t`"path`"`t`t`"" + ($lib -replace '\\', '\\') + "`"`n`t}`n}`n"
        [System.IO.File]::WriteAllText((Join-Path $steam "steamapps\libraryfolders.vdf"), $vdf, (New-Object System.Text.UTF8Encoding $false))
        $found = $null
        $threw = $false
        try {
            $found = Find-SpiderManPath -SteamRoots @($steam) -EpicManifestDir (Join-Path $fake "semEpic")
        } catch { $threw = $true }
        Assert-True (!$threw) "deteccao nao lanca com drive $missing inexistente"
        Assert-True ($found -eq $game) "deteccao acha biblioteca com acento"
        Assert-True ($null -eq (Find-SpiderManPath -SteamRoots @("${missing}x") -EpicManifestDir "")) "steam root em drive inexistente devolve null"
    } finally {
        Remove-Item -LiteralPath $fake -Recurse -Force -ErrorAction SilentlyContinue
    }

    # 14: toggle de cache.pso nas duas pastas (AppData falso) e uninstall restaura as duas.
    $sb = New-GameSandbox
    try {
        $ad = Get-SpiderFixAppDataDir
        Assert-True ($ad.StartsWith($fakeAppData)) "AppData sobreposto pelo teste"
        New-Item -ItemType Directory -Path $ad -Force | Out-Null
        $cg = Join-Path $sb "cache.pso"
        $ca = Join-Path $ad "cache.pso"
        [System.IO.File]::WriteAllText($cg, "g")
        [System.IO.File]::WriteAllText($ca, "a")
        $null = Switch-SpiderFixCache $sb
        Assert-True (!(Test-Path -LiteralPath $cg) -and (Test-Path -LiteralPath "$cg.disabled")) "toggle desativa cache do jogo"
        Assert-True (!(Test-Path -LiteralPath $ca) -and (Test-Path -LiteralPath "$ca.disabled")) "toggle desativa cache de AppData"
        Assert-True ((Get-SpiderFixCacheState $ad) -eq "desativado") "estado AppData desativado"
        $null = Switch-SpiderFixCache $sb
        Assert-True ((Test-Path -LiteralPath $cg) -and (Test-Path -LiteralPath $ca)) "toggle reativa os dois"
        Remove-Item -LiteralPath $ca -Force
        $null = Switch-SpiderFixCache $sb # So o do jogo existe: desativa so ele.
        Assert-True ((Test-Path -LiteralPath "$cg.disabled") -and (Get-SpiderFixCacheState $ad) -eq "ausente") "toggle com um so cache"
        Assert-True (Test-Path -LiteralPath (Join-Path $sb $SpiderFixCacheMarker)) "toggle grava marcador"
        $null = Switch-SpiderFixCache $sb
        Assert-True (!(Test-Path -LiteralPath (Join-Path $sb $SpiderFixCacheMarker))) "toggle remove marcador ao reativar"
        # Os dois desativados pelo toggle (com marcador): uninstall reativa os dois.
        [System.IO.File]::WriteAllText($ca, "a")
        $null = Switch-SpiderFixCache $sb
        & $install -GamePath $sb
        & $uninstall -GamePath $sb
        Assert-True ((Test-Path -LiteralPath $cg) -and (Test-Path -LiteralPath $ca)) "uninstall restaura cache.pso das duas pastas"
        Assert-True (!(Test-Path -LiteralPath (Join-Path $ad $SpiderFixCacheMarker))) "uninstall remove marcador"
        # Desativado manualmente (sem marcador): uninstall nao mexe.
        Rename-Item -LiteralPath $ca -NewName "cache.pso.disabled"
        & $uninstall -GamePath $sb
        Assert-True ((Test-Path -LiteralPath "$ca.disabled") -and !(Test-Path -LiteralPath $ca)) "uninstall mantem cache desativado manualmente"
        # cache.pso regenerado ao lado de .disabled antigo: desativar substitui o antigo.
        Remove-Item -LiteralPath "$ca.disabled" -Force
        [System.IO.File]::WriteAllText("$cg.disabled", "velho")
        [System.IO.File]::WriteAllText($cg, "novo")
        $null = Switch-SpiderFixCache $sb
        Assert-True (!(Test-Path -LiteralPath $cg) -and ([System.IO.File]::ReadAllText("$cg.disabled") -eq "novo")) "desativar substitui .disabled antigo"
        # Reativar com cache.pso ja presente: nao apaga nada.
        [System.IO.File]::WriteAllText($cg, "regenerado")
        Remove-Item -LiteralPath $ca -Force -ErrorAction SilentlyContinue
        Assert-True ((Get-SpiderFixCacheState $sb) -eq "ativo+desativado") "estado ativo+desativado"
        $null = Switch-SpiderFixCache $sb # Ha cache ativo: modo desativar, o atual substitui o .disabled.
        Assert-True (([System.IO.File]::ReadAllText("$cg.disabled") -eq "regenerado") -and !(Test-Path -LiteralPath $cg)) "toggle nao fica preso com os dois"
    } finally {
        Remove-Item -LiteralPath $sb -Recurse -Force
        Remove-Item -LiteralPath (Join-Path $fakeAppData "*") -Recurse -Force -ErrorAction SilentlyContinue
    }

    # 15: -LogFile grava a saida (inclusive com -WhatIf e em erro) e preserva o exit code.
    $sb = New-GameSandbox
    $lf = [System.IO.Path]::GetTempFileName()
    try {
        & powershell -NoProfile -ExecutionPolicy Bypass -File $install -GamePath $sb -WhatIf -LogFile $lf | Out-Null
        Assert-True ($LASTEXITCODE -eq 0) "LogFile WhatIf exit 0"
        $txt = (Read-SpiderFixTranscript $lf) -join "`n"
        Assert-True ($txt -match "Fix instalado") "LogFile grava a saida"
        Assert-True ((Get-Content -LiteralPath $lf -Raw) -match '(?m)^\*{10,}\r?$') "transcript bruto tem cabecalho"
        Assert-True ($txt -notmatch '(?m)^\*{10,}$') "transcript sem cabecalho"
        Assert-True (!(Test-Path -LiteralPath (Join-Path $sb "d3d12.dll"))) "LogFile WhatIf nao instala"
        Remove-Item -LiteralPath (Join-Path $sb $SpiderFixGameExe) -Force
        & powershell -NoProfile -ExecutionPolicy Bypass -File $uninstall -GamePath $sb -LogFile $lf 2>$null | Out-Null
        $code = $LASTEXITCODE
        & powershell -NoProfile -ExecutionPolicy Bypass -File $install -GamePath $sb -LogFile $lf 2>$null | Out-Null
        Assert-True ($LASTEXITCODE -eq 1) "LogFile preserva exit code de falha"
        Assert-True (((Read-SpiderFixTranscript $lf) -join "`n") -match "ERRO: Nao encontrei Spider-Man2.exe") "LogFile grava o erro"
        Assert-True ($code -eq 0) "uninstall com LogFile sem dll exit 0"
    } finally {
        Remove-Item -LiteralPath $sb -Recurse -Force
        Remove-Item -LiteralPath $lf -Force -ErrorAction SilentlyContinue
    }

    # 16: destino e origem somente leitura: copia atomica funciona e nao deixa .sfnew.
    $sb = New-GameSandbox
    try {
        $src = Join-Path $sb "src.dll"
        $t = Join-Path $sb "d3d12.dll"
        Copy-Item -LiteralPath "C:\Windows\System32\version.dll" -Destination $t -Force
        Copy-Item -LiteralPath $releaseDll -Destination $src -Force
        Set-ItemProperty -LiteralPath $t -Name IsReadOnly -Value $true
        Set-ItemProperty -LiteralPath $src -Name IsReadOnly -Value $true
        $threw = $null
        try { Copy-SpiderFixAtomic $src $t $releaseHash } catch { $threw = $_.Exception.Message }
        Assert-True ($null -eq $threw) "copia atomica com RO ($threw)"
        Assert-True ((Get-Sha $t) -eq $releaseHash) "copia atomica RO troca o destino"
        Assert-True (!(Test-Path -LiteralPath "$t.sfnew")) "copia atomica RO sem .sfnew"
        # Falha de hash com origem RO: .sfnew (que herdaria RO) tambem e removido.
        try { Copy-SpiderFixAtomic $src $t "0000" } catch {}
        Assert-True (!(Test-Path -LiteralPath "$t.sfnew")) "falha com origem RO remove .sfnew"
        # .sfnew antigo somente leitura: uninstall remove.
        Copy-Item -LiteralPath $src -Destination "$t.sfnew" -Force
        & $uninstall -GamePath $sb 3>$null
        Assert-True (!(Test-Path -LiteralPath "$t.sfnew")) "uninstall remove .sfnew antigo RO"
    } finally {
        Get-ChildItem -LiteralPath $sb -File | ForEach-Object { $_.IsReadOnly = $false }
        Remove-Item -LiteralPath $sb -Recurse -Force
    }

    # 17: installs seguidos sobre mods diferentes: backups timestampados distintos, uninstall restaura o mais novo.
    $sb = New-GameSandbox
    try {
        $t = Join-Path $sb "d3d12.dll"
        Copy-Item -LiteralPath "C:\Windows\System32\version.dll" -Destination $t -Force
        & $install -GamePath $sb
        Copy-Item -LiteralPath "C:\Windows\System32\winmm.dll" -Destination $t -Force
        & $install -GamePath $sb
        Copy-Item -LiteralPath "C:\Windows\System32\msimg32.dll" -Destination $t -Force
        $newest = Get-Sha $t
        & $install -GamePath $sb
        $ts = @(Get-ChildItem -LiteralPath $sb -Filter "SpiderFixBackup_2*" -Directory)
        Assert-True ($ts.Count -eq 2) "dois backups timestampados distintos"
        & $uninstall -GamePath $sb
        Assert-True ((Get-Sha $t) -eq $newest) "uninstall restaura o backup mais novo"
    } finally {
        Remove-Item -LiteralPath $sb -Recurse -Force
    }
} finally {
    Remove-Item -LiteralPath $fakeAppData -Recurse -Force -ErrorAction SilentlyContinue
    $env:SPIDERFIX_APPDATA = $oldAppData
}

if ($script:failures -gt 0) {
    throw "$($script:failures) teste(s) falharam."
}
Write-Output "Todos os testes de install passaram."
# exit explicito: o wrapper do GitHub (shell: powershell) sai com $LASTEXITCODE,
# que aqui seria o do ultimo processo filho (alguns testes saem com 1 de proposito).
exit 0
