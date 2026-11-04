<div align="center">

# SpiderFix

**A `d3d12.dll` proxy that lets Marvel's Spider-Man 2 boot on GPUs without feature level 12_1.**

![Windows](https://img.shields.io/badge/Windows-10%20%7C%2011%20x64-0078D4?logo=windows&logoColor=white)
![DirectX](https://img.shields.io/badge/DirectX-12-107C10)
![C++](https://img.shields.io/badge/C%2B%2B-MSVC%20x64-00599C?logo=cplusplus&logoColor=white)
![Tested](https://img.shields.io/badge/tested-RX%20580-ED1C24?logo=amd&logoColor=white)

[Versão em português](README.md)

</div>


The game demands a GPU with feature level 12_1 and refuses to start on cards that stop at 12_0, such as the RX 580. SpiderFix takes the place of `d3d12.dll`, fixes the calls that break on those cards and forwards everything else to the system DLL. You copy one file into the game folder and play.

## Contents

- [Quick start](#quick-start)
- [Requirements](#requirements)
- [Install and uninstall](#install-and-uninstall)
- [What the proxy does](#what-the-proxy-does)
- [Configuration](#configuration)
- [Graphical interface](#graphical-interface)
- [Build](#build)
- [Troubleshooting](#troubleshooting)
- [Credits](#credits)
- [Notice](#notice)

## Quick start

1. Close the game.
2. Clone the repository and open PowerShell in its folder:

   ```powershell
   git clone https://github.com/mkvxd/spiderfix.git
   cd spiderfix
   ```

3. Run the installer and point it at the game folder:

   ```powershell
   .\scripts\install_fix.ps1 -GamePath "C:\Program Files (x86)\Steam\steamapps\common\Marvel's Spider-Man 2"
   ```

4. Launch the game from Steam.

## Requirements

| Use | Requirement |
|-----|-------------|
| Play | Windows 10 or 11 x64 |
| Play | Steam copy of the game |
| Build | MSVC x64 and the Windows SDK |

## Install and uninstall

`install_fix.ps1` runs these steps:

- Checks that the game is closed.
- Stores the previous file in `SpiderFixBackup_original`, together with a `manifest.json`.
- Copies the DLL and verifies the hash after the copy.

To remove the fix, use `uninstall_fix.ps1`:

```powershell
.\scripts\uninstall_fix.ps1 -GamePath "C:\Program Files (x86)\Steam\steamapps\common\Marvel's Spider-Man 2"
```

Uninstall restores the backup and deletes the folders the installer created. It refuses to delete a `d3d12.dll` from another mod when no backup exists.

## What the proxy does

| Behavior | Detail |
|----------|--------|
| Feature level fallback | Tries device creation at 12_0 before the requested level, then 11_1 and 11_0. Probes with a null `ppDevice` follow the same sequence. |
| `CheckFeatureSupport` | Answers 12_0 for `FEATURE_LEVELS` when the driver denies it or reports less. Every other query passes through unchanged. |
| Depth-only PSO | Retries creation without a pixel shader when the driver returns `E_INVALIDARG`. Without this the GPU hangs (`0x887A0006`). |
| Incompatible-GPU popup | Skips the notice and returns the button each dialog type expects. |
| Exports | Resolves the jump-table once and forwards the other 18 exports to the System32 `d3d12.dll`. |
| GPU detection | On a 12_1 or newer GPU, the proxy skips the fix and runs as a pure passthrough with no hooks. `SPIDERFIX_FORCE=1` forces the fix. |

## Configuration

### Environment variables

| Variable | Effect |
|----------|--------|
| `SPIDERFIX_LOG=0` | Disables logging to `spiderfix.log` |
| `SPIDERFIX_DEPTH_RETRY=0` | Disables the depth-only retry (the game hangs without it) |
| `SPIDERFIX_FORCE=1` | Forces the fix on 12_1 or newer GPUs |

### `spiderfix.ini` file

Place `spiderfix.ini` next to the DLL to tune the same options without environment variables. Use `spiderfix.ini.example` as a template.

Available keys: `Log`, `LogLevel`, `ForceActive`, `DepthRetry`, `HookPipelineLibrary` and `ShowOSD`.

The first launch shows a SpiderFix notice for 4 seconds. `ShowOSD=0` disables it.

## Graphical interface

`scripts\SpiderFixGUI.ps1` opens a window with four actions:

- install
- uninstall
- toggle `cache.pso`
- export diagnostics to the Desktop

## Build

```powershell
.\scripts\build_spider_proxy.ps1
```

The script needs x64 PowerShell, x64 MSVC and the Windows SDK. It produces `release\d3d12.dll` and `release\d3d12.pdb` with the flags `/W4 /permissive- /guard:cf`.

## Troubleshooting

| Symptom | What to do |
|---------|------------|
| The game does not open after install | Check that `release\d3d12.dll` and `Spider-Man2.exe` are x64 and that the hash of the DLL in the game folder matches the release. `install_fix.ps1` validates both. |
| The game hangs on loading | Rename `cache.pso` to `cache.pso.disabled` in the game folder. |
| `spiderfix.log` does not appear | A global `SPIDERFIX_LOG=0` disables writing. Delete the variable or set `SPIDERFIX_LOG=1`. |

If the problem persists, open an [issue](https://github.com/mkvxd/spiderfix/issues) and attach the diagnostics exported by the graphical interface.

## Notice

Unofficial project, with no link to Nixxes, Insomniac, Sony or AMD. Testing covered the RX 580. Another card needs its own test before a long session.
