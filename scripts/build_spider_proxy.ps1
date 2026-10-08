[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

if (-not [Environment]::Is64BitProcess) {
    throw "Rode em PowerShell x64. Alvo e x64 e ml64 nao existe em x86."
}

$Repo = Split-Path -Parent $PSScriptRoot
$SrcDir = Join-Path $Repo "src"
$OutDir = Join-Path $Repo "release"
$Src = Join-Path $SrcDir "SpiderD3D12Proxy.cpp"
$Asm = Join-Path $SrcDir "D3D12ProxyStubs.asm"
$Def = Join-Path $SrcDir "d3d12_proxy.def"
$Out = Join-Path $OutDir "d3d12.dll"
$Obj = Join-Path $OutDir "D3D12Proxy.obj"
$AsmObj = Join-Path $OutDir "D3D12ProxyStubs.obj"

foreach ($f in @($Src, $Asm, $Def)) {
    if (!(Test-Path -LiteralPath $f)) { throw "Nao encontrei: $f" }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$vcvars = $null
foreach ($c in @(
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
    "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
)) {
    if (Test-Path -LiteralPath $c) {
        $vsPath = & $c -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vsPath) {
            $candidate = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path -LiteralPath $candidate) {
                $vcvars = $candidate
            }
        }
        break
    }
}

$clArgs = @("/nologo", "/EHsc", "/std:c++17", "/O2", "/MT", "/W4", "/permissive-",
    "/Zi", "/guard:cf", "/DNDEBUG", "/DWIN32_LEAN_AND_MEAN", "/LD",
    $Src, $AsmObj, "/Fe:$Out", "/Fo:$Obj",
    "/link", "/DEF:$Def", "/DEBUG", "/OPT:REF", "/OPT:ICF", "/GUARD:CF", "/MACHINE:X64")

if ($vcvars) {
    # Importa o ambiente do vcvars via .cmd temporario (quotacao estavel no PS 5.1 e 7+).
    $tmpCmd = Join-Path ([System.IO.Path]::GetTempPath()) "spiderfix_vcvars.cmd"
    "@echo off`r`ncall `"$vcvars`" >nul`r`nset" | Set-Content -LiteralPath $tmpCmd -Encoding Ascii
    try {
        $lines = & cmd /s /c "`"$tmpCmd`""
        foreach ($line in $lines) {
            $i = $line.IndexOf("=")
            if ($i -gt 0) {
                Set-Item -Path ("env:" + $line.Substring(0, $i)) -Value $line.Substring($i + 1)
            }
        }
    } finally {
        Remove-Item -LiteralPath $tmpCmd -Force -ErrorAction SilentlyContinue
    }
} elseif ($env:VSCMD_ARG_TGT_ARCH -and ($env:VSCMD_ARG_TGT_ARCH -ne "x64")) {
    throw "Ambiente de build aponta para $($env:VSCMD_ARG_TGT_ARCH), precisa de x64 (Developer PowerShell x64)."
}

foreach ($t in @("cl.exe", "ml64.exe", "link.exe")) {
    if (-not (Get-Command $t -ErrorAction SilentlyContinue)) {
        throw "$t nao encontrado. Rode via Developer PowerShell x64 ou instale MSVC x64 + SDK."
    }
}

& ml64.exe /nologo /Zi /c /Fo $AsmObj $Asm
if ($LASTEXITCODE -ne 0) { throw "ml64 falhou: $LASTEXITCODE" }

& cl.exe @clArgs
if ($LASTEXITCODE -ne 0) { throw "cl falhou: $LASTEXITCODE" }

if (!(Test-Path -LiteralPath $Out)) {
    throw "Build terminou sem gerar $Out."
}

$pdb = Join-Path $OutDir "d3d12.pdb"
if (!(Test-Path -LiteralPath $pdb)) {
    Write-Warning "PDB nao gerado em $pdb."
}

Write-Host "Build OK: $Out"
