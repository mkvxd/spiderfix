#Requires -Version 5.1
# Smoke test da DLL: carrega copia isolada, chama SpiderFixVersion e
# D3D12CreateDevice, confere log. Exige GPU D3D12 (WARP serve).
# Uso: powershell -ExecutionPolicy Bypass -File tests/smoke_test.ps1

[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$releaseDll = Join-Path $repoRoot "release\d3d12.dll"

if (!(Test-Path -LiteralPath $releaseDll)) {
    throw "release\d3d12.dll ausente. Rode build_spider_proxy.ps1 antes."
}

$dir = Join-Path ([System.IO.Path]::GetTempPath()) ("spiderfix_smoke_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $dir | Out-Null
try {
    Copy-Item -LiteralPath $releaseDll -Destination (Join-Path $dir "d3d12.dll") -Force
    $env:SPIDERFIX_LOG = "1"
    $env:SPIDERFIX_OSD = "0"
    $code = @'
using System;
using System.Runtime.InteropServices;
public static class H {
    [DllImport("kernel32", CharSet=CharSet.Unicode)] public static extern IntPtr LoadLibrary(string p);
    [DllImport("kernel32")] public static extern IntPtr GetProcAddress(IntPtr h, string n);
    [DllImport("kernel32")] public static extern bool FreeLibrary(IntPtr h);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] public delegate uint VerDel();
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] public delegate int CreateDel(IntPtr a, uint fl, ref Guid riid, out IntPtr dev);
}
'@
    Add-Type -TypeDefinition $code
    $h = [H]::LoadLibrary((Join-Path $dir "d3d12.dll"))
    if ($h -eq [IntPtr]::Zero) {
        throw "LoadLibrary falhou."
    }
    Write-Output "PASS: LoadLibrary"
    try {
        foreach ($n in @("D3D12CreateDevice", "D3D12GetDebugInterface", "D3D12GetInterface",
                "D3D12SerializeRootSignature", "SpiderFixVersion", "SetAppCompatStringPointer")) {
            if ([H]::GetProcAddress($h, $n) -eq [IntPtr]::Zero) {
                throw "Export ausente: $n"
            }
        }
        Write-Output "PASS: exports resolvem"
        $pv = [H]::GetProcAddress($h, "SpiderFixVersion")
        $vd = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($pv, [H+VerDel])
        $ver = $vd.Invoke()
        if ($ver -eq 0) {
            throw "SpiderFixVersion retornou 0."
        }
        Write-Output ("PASS: SpiderFixVersion=0x{0:X}" -f $ver)
        $pc = [H]::GetProcAddress($h, "D3D12CreateDevice")
        $cd = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($pc, [H+CreateDel])
        $g = [Guid]::new("189819f1-1db6-4b57-be54-1821339b85f7")
        [IntPtr]$dev = [IntPtr]::Zero
        $hr = $cd.Invoke([IntPtr]::Zero, 0xc000, [ref]$g, [ref]$dev)
        if ($hr -ne 0 -or $dev -eq [IntPtr]::Zero) {
            throw ("D3D12CreateDevice falhou: hr=0x{0:X8}" -f $hr)
        }
        Write-Output "PASS: D3D12CreateDevice S_OK"
        $log = Join-Path $dir "spiderfix.log"
        if (!(Test-Path -LiteralPath $log)) {
            throw "spiderfix.log nao criado."
        }
        Write-Output "PASS: log criado"
    } finally {
        [H]::FreeLibrary($h) | Out-Null
    }
} finally {
    Remove-Item Env:\SPIDERFIX_LOG -ErrorAction SilentlyContinue
    Remove-Item Env:\SPIDERFIX_OSD -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Output "Smoke test passou."
