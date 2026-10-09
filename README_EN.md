# SpiderFix

[Versão em português](README.md)

A `d3d12.dll` proxy that lets Marvel's Spider-Man 2 boot on GPUs without feature level 12_1. Copy one file into the game folder and play.

## Requirements

- Windows 10 or 11 x64.
- Steam copy of the game.
- To build: MSVC x64 and the Windows SDK.

## Install

Run from the repo folder in PowerShell:

```powershell
.\scripts\install_fix.ps1 -GamePath "C:\Program Files (x86)\Steam\steamapps\common\Marvel's Spider-Man 2"
```

The script checks the game is closed, stores the previous file in `SpiderFixBackup_original` with a `manifest.json`, and verifies the hash after copying. To remove:

```powershell
.\scripts\uninstall_fix.ps1 -GamePath "C:\Program Files (x86)\Steam\steamapps\common\Marvel's Spider-Man 2"
```

Uninstall restores the backup and deletes the folders it created. It refuses to delete a `d3d12.dll` from another mod without a backup.

## What the proxy does

- Tries device creation at 12_0 before the requested level, then 11_1 and 11_0. Null-`ppDevice` probes get the same loop.
- Answers 12_0 to `CheckFeatureSupport(FEATURE_LEVELS)` when the driver denies it or reports less, and leaves everything else alone.
- Retries depth-only PSO creation without a pixel shader when the driver returns `E_INVALIDARG`. Without this the GPU hangs (`0x887A0006`).
- Skips the incompatible-GPU popup and returns the button each dialog type expects.
- Resolves the jump-table once and forwards the other 18 exports to the System32 `d3d12.dll`.
- Detects 12_1 or newer GPUs and skips the fix, pure passthrough without hooks. `SPIDERFIX_FORCE=1` forces the fix.

## Environment variables

| Variable | Effect |
| --- | --- |
| `SPIDERFIX_LOG=0` | Disables logging to `spiderfix.log` |
| `SPIDERFIX_DEPTH_RETRY=0` | Disables the depth-only retry (the game hangs without it) |

`spiderfix.ini` next to the DLL (see `spiderfix.ini.example`) tunes the same without variables: `Log`, `LogLevel`, `ForceActive`, `DepthRetry`, `HookPipelineLibrary`, `ShowOSD`.

The first boot shows a SpiderFix notice for 4 seconds (`ShowOSD=0` disables it).

## Graphical interface

`scripts\SpiderFixGUI.ps1` opens a window with install, uninstall, `cache.pso` toggle and one-click diagnostics export to the Desktop.

## Build

```powershell
.\scripts\build_spider_proxy.ps1
```

Needs x64 PowerShell, x64 MSVC and the Windows SDK. Produces `release\d3d12.dll` and `release\d3d12.pdb` with `/W4 /permissive- /guard:cf`.

## Common issues

- Game never opens after install: check `release\d3d12.dll` and `Spider-Man2.exe` are x64 and the hash in the game folder matches release. `install_fix.ps1` validates both.
- Crash on loading: rename `cache.pso` to `cache.pso.disabled` in the game folder.
- No log: a global `SPIDERFIX_LOG=0` disables writing. Delete the variable or set it to `1`.

## Notice

Unofficial project, no link to Nixxes, Insomniac, Sony or AMD. Tested on RX 580 only; another card needs its own test before a long session.
