# Testes da DLL compilada: slow path dos stubs ASM e parser do spiderfix.ini.
# Cada caso roda em processo proprio (config e lida uma vez e a DLL fica fixada).
param([string]$Case, [string]$Dir)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $repo 'release\d3d12.dll'

if (-not $Case) {
    $cases = @(
        @{ n='slowpath'; ini=$null; enc=$null; env=@{}; expect={ param($log,$r) $r -match 'SLOW hr=0x' -and $r -match 'PIX ok' } },
        @{ n='utf16-force-inline'; ini="ForceActive = yes # liga`r`nLogLevel = 0 ; desliga`r`n"; enc='Unicode'; env=@{}; expect={ param($log) $log -eq '<sem log>' } },
        @{ n='utf8bom-first-key'; ini="Log=0`r`n"; enc='UTF8'; env=@{}; expect={ param($log) $log -eq '<sem log>' } },
        @{ n='bom-unknown-only'; ini="ForceActive=off`r`nFoo=1`r`n"; enc='UTF8'; env=@{SPIDERFIX_FORCE='1'}; expect={ param($log) $log -match "spiderfix.ini: chave desconhecida 'Foo'(\r|$)" } },
        @{ n='unknown-key'; ini="Foo=1`r`nShowOSD=talvez`r`n"; enc='ASCII'; env=@{}; expect={ param($log) $log -match "chave desconhecida 'Foo'" -and $log -match "valor invalido em 'ShowOSD'" } },
        @{ n='loglevel-wins'; ini="LogLevel=0`r`nLog=1`r`n"; enc='ASCII'; env=@{}; expect={ param($log) $log -eq '<sem log>' } }
    )
    $fail = 0
    foreach ($c in $cases) {
        $d = Join-Path $env:TEMP ("sfchk_" + $c.n + "_" + [guid]::NewGuid().ToString('N').Substring(0,6))
        $envs = $c.env + @{ SPIDERFIX_OSD = '0' }
        $old = @{}
        try {
            New-Item -ItemType Directory -Path $d | Out-Null
            Copy-Item $dll (Join-Path $d 'd3d12.dll')
            if ($c.ini) { Set-Content -LiteralPath (Join-Path $d 'spiderfix.ini') -Value $c.ini -Encoding $c.enc -NoNewline }
            foreach ($k in $envs.Keys) { $old[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $envs[$k]) }
            # Continue local: no PS 5.1, stderr do filho com 2>&1 sob Stop vira excecao terminante.
            $r = & { $ErrorActionPreference = 'Continue'; & powershell -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Case $c.n -Dir $d 2>&1 | Out-String }
            $rc = $LASTEXITCODE
            $lp = Join-Path $d 'spiderfix.log'
            $log = if (Test-Path $lp) { Get-Content $lp -Raw } else { '<sem log>' }
            # DXGI_ERROR_UNSUPPORTED: runner sem D3D12 FL 12_0 no driver basico.
            # Em CI, SKIP conta como falha: senao tudo pulado daria verde sem exercitar nada.
            if ($r -match 'CreateDevice hr=0x887A0004') {
                if ($env:CI) { $fail++; "FAIL: SKIP nao aceito com CI definido, sem adaptador D3D12 ($($c.n))" } else { "SKIP: sem adaptador D3D12 ($($c.n))" }
                continue
            }
            # Exige que o filho chegou ao fim: '<sem log>' sozinho passaria com crash antes de logar.
            $ok = ($rc -eq 0) -and ($c.n -eq 'slowpath' -or $r -match 'CreateDevice hr=0x') -and (& $c.expect $log $r)
            if ($ok) { "PASS: $($c.n)" } else { $fail++; "FAIL: $($c.n) (exit=$rc)`n--- child:`n$r`n--- log:`n$log" }
        } catch {
            $fail++; "FAIL: $($c.n)`n$_"
        } finally {
            foreach ($k in $old.Keys) { [Environment]::SetEnvironmentVariable($k, $old[$k]) }
            Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue
        }
    }
    "falhas=$fail"
    exit $fail
}

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class N {
    [DllImport("kernel32", SetLastError=true, CharSet=CharSet.Unicode)] public static extern IntPtr LoadLibraryW(string p);
    [DllImport("kernel32", CharSet=CharSet.Ansi)] public static extern IntPtr GetProcAddress(IntPtr h, string n);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] public delegate int GetDebugIf(ref Guid riid, out IntPtr pp);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] public delegate void PixReport([MarshalAs(UnmanagedType.LPWStr)] string name, float v);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] public delegate int CreateDev(IntPtr a, int fl, ref Guid riid, out IntPtr pp);
}
"@
$h = [N]::LoadLibraryW((Join-Path $Dir 'd3d12.dll'))
if ($h -eq [IntPtr]::Zero) { throw "LoadLibrary falhou" }
if ($Case -eq 'slowpath') {
    # Primeira chamada a export de repasse antes de D3D12CreateDevice: slow path ASM.
    $f = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer([N]::GetProcAddress($h, 'D3D12GetDebugInterface'), [N+GetDebugIf])
    $g = [Guid]'344488b7-6846-474b-b989-f027448245e0' # ID3D12Debug
    $p = [IntPtr]::Zero
    $hr = $f.Invoke([ref]$g, [ref]$p)
    "SLOW hr=0x{0:X8}" -f $hr
    $pix = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer([N]::GetProcAddress($h, 'D3D12PIXReportCounter'), [N+PixReport])
    $pix.Invoke('teste', [float]1.5)
    "PIX ok"
    exit 0
}
$cd = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer([N]::GetProcAddress($h, 'D3D12CreateDevice'), [N+CreateDev])
$g = [Guid]'189819f1-1db6-4b57-be54-1821339b85f7' # ID3D12Device
$p = [IntPtr]::Zero
$hr = $cd.Invoke([IntPtr]::Zero, 0xb000, [ref]$g, [ref]$p)
"CreateDevice hr=0x{0:X8}" -f $hr
