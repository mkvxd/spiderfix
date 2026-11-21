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
    $t = $null
    try {
        $t = Join-Path $Dir ".spiderfix_write_test"
        [System.IO.File]::WriteAllText($t, "x")
        return $true
    } catch {
        return $false
    } finally {
        # finally: o arquivo de teste nunca fica para tras. .NET: funciona mesmo em -WhatIf.
        if ($t) { try { [System.IO.File]::Delete($t) } catch {} }
    }
}

# Copia atomica: grava <Destination>.sfnew na mesma pasta, confere o SHA256 e troca por rename.
# Qualquer falha remove o .sfnew e deixa o destino intacto.
function Copy-SpiderFixAtomic([string]$Source, [string]$Destination, [string]$ExpectedHash) {
    if (!$ExpectedHash) {
        $ExpectedHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    }
    $tmp = $Destination + ".sfnew"
    try {
        # Atributos Normal: .sfnew antigo ou origem somente leitura impediriam Copy/Replace/Delete.
        if ([System.IO.File]::Exists($tmp)) { [System.IO.File]::SetAttributes($tmp, "Normal") }
        [System.IO.File]::Copy($Source, $tmp, $true)
        [System.IO.File]::SetAttributes($tmp, "Normal")
        $h = (Get-FileHash -LiteralPath $tmp -Algorithm SHA256).Hash
        if ($h -ne $ExpectedHash) {
            throw "Hash diverge apos copia de $Source. Esperado $ExpectedHash, obtido $h."
        }
        if ([System.IO.File]::Exists($Destination)) {
            # Destino somente leitura: Replace daria acesso negado (Copy-Item -Force antigo sobrescrevia).
            $attr = [System.IO.File]::GetAttributes($Destination)
            if ($attr -band [System.IO.FileAttributes]::ReadOnly) {
                [System.IO.File]::SetAttributes($Destination, ($attr -bxor [System.IO.FileAttributes]::ReadOnly))
            }
            # ReplaceFile: troca atomica no mesmo volume. [NullString]: $null viraria "" (caminho invalido).
            [System.IO.File]::Replace($tmp, $Destination, [NullString]::Value)
        } else {
            [System.IO.File]::Move($tmp, $Destination)
        }
    } finally {
        if ([System.IO.File]::Exists($tmp)) {
            try { [System.IO.File]::SetAttributes($tmp, "Normal"); [System.IO.File]::Delete($tmp) } catch {}
        }
    }
}

# Pasta onde o jogo grava o segundo cache.pso. SPIDERFIX_APPDATA substitui $env:APPDATA (testes).
function Get-SpiderFixAppDataDir {
    $root = $env:SPIDERFIX_APPDATA
    if (!$root) { $root = $env:APPDATA }
    if (!$root) { return $null }
    return [System.IO.Path]::Combine($root, "Insomniac Games", $SpiderFixGameFolder)
}

# Pastas com cache.pso: a do jogo e a de AppData.
function Get-SpiderFixCacheDirs([string]$GamePath) {
    return @(@($GamePath, (Get-SpiderFixAppDataDir)) | Where-Object { $_ })
}

function Get-SpiderFixCacheState([string]$Dir) {
    if (!$Dir) { return "ausente" }
    $s = @()
    try {
        if (Test-Path -LiteralPath ([System.IO.Path]::Combine($Dir, "cache.pso"))) { $s += "ativo" }
        if (Test-Path -LiteralPath ([System.IO.Path]::Combine($Dir, "cache.pso.disabled"))) { $s += "desativado" }
    } catch {}
    if ($s) { return ($s -join "+") }
    return "ausente"
}

# Marcador ao lado do cache.pso.disabled: so o que o SpiderFix desativou o uninstall reativa.
$SpiderFixCacheMarker = "cache.pso.spiderfix-disabled"

# Alterna cache.pso nas duas pastas juntas: se algum esta ativo desativa todos, senao reativa todos.
# Devolve as mensagens para o log; erro numa pasta nao impede a outra.
function Switch-SpiderFixCache([string]$GamePath) {
    $dirs = Get-SpiderFixCacheDirs $GamePath
    $on = @($dirs | Where-Object { Test-Path -LiteralPath ([System.IO.Path]::Combine($_, "cache.pso")) })
    $msgs = @()
    foreach ($d in $dirs) {
        $c = [System.IO.Path]::Combine($d, "cache.pso")
        $cd = [System.IO.Path]::Combine($d, "cache.pso.disabled")
        $mk = [System.IO.Path]::Combine($d, $SpiderFixCacheMarker)
        try {
            if ($on.Count -gt 0) {
                if (!(Test-Path -LiteralPath $c)) { continue }
                # cache.pso ativo e o atual (o jogo regenera): substitui um .disabled antigo.
                Move-Item -LiteralPath $c -Destination $cd -Force
                [System.IO.File]::WriteAllText($mk, "desativado pelo SpiderFix")
                $msgs += "cache.pso desativado em: $d"
            } else {
                if (!(Test-Path -LiteralPath $cd)) { continue }
                if (Test-Path -LiteralPath $c) {
                    $msgs += "cache.pso ja ativo em: $d"
                    continue
                }
                Rename-Item -LiteralPath $cd -NewName "cache.pso"
                if (Test-Path -LiteralPath $mk) { Remove-Item -LiteralPath $mk -Force }
                $msgs += "cache.pso ativado em: $d"
            }
        } catch {
            $msgs += "ERRO em ${d}: " + $_.Exception.Message
        }
    }
    if ($msgs.Count -eq 0) { $msgs = @("Nada a alternar.") }
    return $msgs
}

# Linhas de um transcript sem os blocos de cabecalho/rodape (entre linhas de asteriscos).
function Read-SpiderFixTranscript([string]$Path) {
    $lines = @(Get-Content -LiteralPath $Path -ErrorAction SilentlyContinue)
    $stars = @(for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^\*{10,}$') { $i } })
    if ($stars.Count -lt 2) { return $lines }
    $end = $lines.Count
    if ($stars.Count -ge 4) { $end = $stars[2] }
    if ($end -le $stars[1] + 1) { return @() }
    return $lines[($stars[1] + 1)..($end - 1)]
}

# -LogFile: roda o script de novo no mesmo processo sob Start-Transcript e devolve o exit code.
# Usado pela elevacao: a console do filho elevado fecha ao terminar e a saida se perderia.
function Invoke-SpiderFixWithLog([string]$ScriptPath, [System.Collections.IDictionary]$BoundParams) {
    $log = $BoundParams["LogFile"]
    $p = @{}
    foreach ($k in $BoundParams.Keys) {
        if ($k -ne "LogFile") { $p[$k] = $BoundParams[$k] }
    }
    $started = $false
    try {
        Start-Transcript -LiteralPath $log -Force -WhatIf:$false | Out-Null
        $started = $true
    } catch {
        [Console]::Error.WriteLine("AVISO: nao foi possivel gravar log em ${log}: " + $_.Exception.Message)
    }
    $global:LASTEXITCODE = 0
    $code = 0
    try {
        & $ScriptPath @p | Out-Host # Out-Host: saida vai para o transcript, nao para o retorno.
        $code = $global:LASTEXITCODE
    } catch {
        Write-Host ("ERRO: " + $_.Exception.Message) # Write-Host: vai para o transcript.
        $code = 1
    } finally {
        if ($started) { try { Stop-Transcript | Out-Null } catch {} }
    }
    return $code
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
        Write-Host "ERRO: sem escrita em $GamePath mesmo como administrador." # Write-Host: chega ao transcript.
        return 1
    }
    $a = "-NoProfile -ExecutionPolicy Bypass -File `"$ScriptPath`" -GamePath `"$(ConvertTo-SpiderFixArgPath $GamePath)`""
    if ($WhatIf) {
        $a += " -WhatIf"
    }
    $log = [System.IO.Path]::GetTempFileName()
    $a += " -LogFile `"$log`""
    try {
        $p = Start-Process powershell -Verb RunAs -ArgumentList $a -Wait -PassThru
        return $p.ExitCode
    } catch {
        # UAC cancelado pelo usuario cai aqui.
        [Console]::Error.WriteLine("ERRO: elevacao cancelada ou falhou: " + $_.Exception.Message)
        return 1
    } finally {
        # Repassa a saida do filho elevado (GUI le pelo stdout deste processo).
        Read-SpiderFixTranscript $log | ForEach-Object { Write-Host $_ }
        Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue -WhatIf:$false
    }
}

function Test-SpiderManDir([string]$Dir) {
    try {
        # [IO.Path]::Combine: Join-Path lanca DriveNotFoundException para drive inexistente no PS 5.1.
        return ($Dir -and (Test-Path -LiteralPath ([System.IO.Path]::Combine($Dir, $SpiderFixGameExe))))
    } catch {
        return $false
    }
}

# Procura a pasta do jogo em bibliotecas Steam e manifests Epic. $null se nao achar.
# -SteamRoots/-EpicManifestDir substituem registro/ProgramData (testes).
function Find-SpiderManPath([string[]]$SteamRoots, [string]$EpicManifestDir) {
    $roots = @()
    if ($PSBoundParameters.ContainsKey("SteamRoots")) {
        $roots = @($SteamRoots)
    } else {
        try { $roots += (Get-ItemProperty -Path "HKCU:\Software\Valve\Steam" -Name SteamPath -ErrorAction Stop).SteamPath } catch {}
        try { $roots += (Get-ItemProperty -Path "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam" -Name InstallPath -ErrorAction Stop).InstallPath } catch {}
    }
    $libs = @()
    foreach ($r in $roots) {
        if (!$r) { continue }
        try {
            $r = $r -replace '/', '\'
            $libs += $r
            $vdf = [System.IO.Path]::Combine($r, "steamapps\libraryfolders.vdf")
            if (Test-Path -LiteralPath $vdf) {
                # UTF8: o vdf e UTF-8 sem BOM; sem -Encoding o PS 5.1 le como ANSI e estraga acentos.
                foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw -Encoding UTF8), '"path"\s+"([^"]+)"')) {
                    $libs += ($m.Groups[1].Value -replace '\\\\', '\')
                }
            }
        } catch {}
    }
    foreach ($l in $libs) {
        try {
            $d = [System.IO.Path]::Combine($l, "steamapps\common", $SpiderFixGameFolder)
            if (Test-SpiderManDir $d) {
                return $d
            }
        } catch {} # Biblioteca invalida ou drive desconectado: segue para a proxima.
    }
    $epic = $EpicManifestDir
    if (!$PSBoundParameters.ContainsKey("EpicManifestDir")) {
        $epic = Join-Path $env:ProgramData "Epic\EpicGamesLauncher\Data\Manifests"
    }
    if ($epic -and (Test-Path -LiteralPath $epic)) {
        foreach ($f in Get-ChildItem -LiteralPath $epic -Filter "*.item" -ErrorAction SilentlyContinue) {
            try {
                $loc = (Get-Content -LiteralPath $f.FullName -Raw -Encoding UTF8 | ConvertFrom-Json).InstallLocation
                if (Test-SpiderManDir $loc) {
                    return $loc
                }
            } catch {}
        }
    }
    return $null
}
