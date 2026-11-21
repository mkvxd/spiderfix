<div align="center">

# SpiderFix

**Makes Marvel's Spider-Man 2 launch on graphics cards without DirectX 12 feature level 12_1.**

![Version](https://img.shields.io/badge/version-2.1-2ea44f)
![Windows](https://img.shields.io/badge/Windows-10%20%7C%2011%20x64-0078D4?logo=windows&logoColor=white)
![DirectX](https://img.shields.io/badge/DirectX-12-107C10)
![Tested](https://img.shields.io/badge/tested-RX%20580-ED1C24?logo=amd&logoColor=white)

[Português](README.md)

</div>

The game requires feature level 12_1 and refuses to start on cards that top out at 12_0, such as the RX 580. SpiderFix is a `d3d12.dll` that sits in the game folder: it fixes the few calls that break on those cards and forwards everything else to the original Windows DLL. Nothing on the system is changed, and uninstalling puts the folder back the way it was.

On cards that already support 12_1, the DLL detects it on its own and stays out of the way.

## Contents

- [Installation](#installation)
- [Removal](#removal)
- [GUI](#gui)
- [Configuration](#configuration)
- [Troubleshooting](#troubleshooting)
- [How it works](#how-it-works)
- [Building from source](#building-from-source)
- [What's new in v2.1](#whats-new-in-v21)
- [What's new in v2.0](#whats-new-in-v20)
- [Tested versions](#tested-versions)
- [Antivirus and safety](#antivirus-and-safety)
- [Disclaimer](#disclaimer)

## Installation

Requirements: Windows 10 or 11 x64 and the game on Steam or Epic.

1. Close the game.
2. Download `spiderfix.zip` and `SHA256SUMS.txt` from [Releases](https://github.com/mkvxd/spiderfix/releases/latest).
3. Verify the download (optional, recommended):

   ```powershell
   Get-FileHash .\spiderfix.zip -Algorithm SHA256              # must match the spiderfix.zip line in SHA256SUMS.txt
   gh attestation verify spiderfix.zip --repo mkvxd/spiderfix  # requires GitHub CLI
   ```

4. Extract the zip, open PowerShell inside the extracted folder and unblock the downloaded files:

   ```powershell
   Get-ChildItem -Recurse | Unblock-File
   ```

5. Install:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\scripts\install_fix.ps1
   ```

   The installer finds the game folder in your Steam libraries and Epic manifests. If it can't, pass the path:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\scripts\install_fix.ps1 -GamePath "D:\SteamLibrary\steamapps\common\Marvel's Spider-Man 2"
   ```

6. Launch the game as usual. A 4-second notice in the corner of the screen confirms SpiderFix loaded.

`-ExecutionPolicy Bypass` applies to that command only; the system policy is not changed. If the game folder needs administrator rights, the script asks for elevation (UAC) and waits for the result.

**What the installer does:** checks that the game is closed, backs up any `d3d12.dll` already in the folder (from another mod, for example) together with a `manifest.json`, copies the DLL and verifies its hash. If the copy comes out corrupted, it rolls back.

## Removal

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\uninstall_fix.ps1
```

The uninstaller restores the newest backup and only then deletes the backup folders. It is deliberately conservative:

- if the `d3d12.dll` in the folder is not SpiderFix (you installed another mod on top), it touches nothing;
- if the backup does not match the recorded hash, it does not restore it and keeps the backup, printing its path.

Both scripts accept `-WhatIf` to show what they would do without changing anything.

## GUI

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\SpiderFixGUI.ps1
```

The window shows the state of the selected folder (installed and version, not installed, or another `d3d12.dll` present) and has four buttons:

| Button | Action |
|--------|--------|
| Instalar (Install) | Runs `install_fix.ps1` without freezing the window and shows its output. |
| Desinstalar (Uninstall) | Runs `uninstall_fix.ps1`. |
| Alternar cache.pso | Enables or disables the game's shader cache in both places it lives: the game folder and `%APPDATA%\Insomniac Games\Marvel's Spider-Man 2`. Useful if it hangs on loading. |
| Diagnostico | Writes a report to the Desktop: Windows and game version, `spiderfix.ini` contents, other proxy DLLs in the folder and the DLL's SHA256. Attach it when opening an issue. |

## Configuration

Nothing needs configuring. To tweak, copy `spiderfix.ini.example` into the game folder as `spiderfix.ini`. Each option can also come from an environment variable; **a key present in the INI always wins over the variable**, including when it is 0.

| INI key | Variable | Default | Effect |
|---------|----------|---------|--------|
| `Log` | `SPIDERFIX_LOG` | 1 | Writes `spiderfix.log` next to the DLL. |
| `LogLevel` | — | 1 | 0 off, 1 logs each new event once, 2 also logs every repeated PSO retry. Wins over `Log`. |
| `ForceActive` | `SPIDERFIX_FORCE` | 0 | Applies the fix even on 12_1-or-newer cards. |
| `DepthRetry` | `SPIDERFIX_DEPTH_RETRY` | 1 | Recreates depth-only PSOs without a pixel shader when the driver rejects them. Without it the game hangs. |
| `HookPipelineLibrary` | `SPIDERFIX_HOOK_LIB` | 1 | Applies the same retry to shaders loaded from `cache.pso`. |
| `ShowOSD` | `SPIDERFIX_OSD` | 1 | Shows the 4 s notice when the game starts. |

Accepted values: `0`/`1` (and `2` for `LogLevel`), `yes`/`no`, `y`/`n`, `true`/`false`, `on`/`off`. Comments start with `#` or `;`, including at the end of a line. The file can be ANSI, UTF-8 (with or without BOM) or UTF-16. Invalid keys and values are reported in the log.

The log is capped at 512 KB; past that it becomes `spiderfix.log.old` and a new one starts.

## Troubleshooting

| Symptom | What to do |
|---------|------------|
| "Running scripts is disabled on this system" | Run `Get-ChildItem -Recurse \| Unblock-File` in the extracted folder and use `powershell -ExecutionPolicy Bypass -File ...`. |
| The installer can't find the game | Pass `-GamePath` with the folder that contains `Spider-Man2.exe`. |
| The game won't start after installing | Generate the diagnostic in the GUI and check for another proxy DLL (`dxgi.dll`, `dinput8.dll`...) in the folder. Open an issue with the report and `spiderfix.log`. |
| Hangs on the loading screen | Disable `cache.pso` (GUI button, or rename it to `cache.pso.disabled` in the game folder **and** in `%APPDATA%\Insomniac Games\Marvel's Spider-Man 2`). The game rebuilds its shaders on the next launch. With the cache enabled the game compiles fewer shaders mid-play and stutters less. |
| Some visual effect looks incomplete | Known side effect of the PSO retry (see [How it works](#how-it-works)). It is the price of not hanging. |
| `spiderfix.log` doesn't appear | Make sure the INI has no `Log=0` or `LogLevel=0` and that there is no `SPIDERFIX_LOG=0` variable on the system. |
| I want to know whether the fix is active | The 4 s notice says "modo FIX" or "modo BYPASS". The log records every step. |

Still stuck: open an [issue](https://github.com/mkvxd/spiderfix/issues) with the diagnostic report and `spiderfix.log`.

## How it works

Windows loads the `d3d12.dll` in the game folder before the system one. SpiderFix takes that slot and loads the original DLL by its full System32 path.

| Part | What it does |
|------|--------------|
| Detection | Asks the driver whether the card **the game picked** supports 12_1 (this matters on laptops with two GPUs). If it does, everything is forwarded with no hooks at all. |
| Device creation | Tries 12_0 before the level the game asked for, then 11_1 and 11_0. Requests above 12_1 (e.g. 12_2) pass straight through, with no faked support. Errors that are not about support go back to the game untouched. |
| `CheckFeatureSupport` | Answers 12_0 to the feature-level query when the driver reports less. No other query is changed: features the card lacks are still reported as missing. |
| Depth-only PSO | On the RX 580 the driver rejects depth-only pipelines whose pixel shader uses ROVs (rasterizer ordered views); plain alpha-test (`clip`) works. SpiderFix recreates those pipelines without the shader, which avoids the `0x887A0006` hang. The cost is losing that pass's ROV writes, which may leave some effect incomplete. |
| Incompatible-GPU notice | Dismisses the game's requirements popup. It only acts during the first 3 minutes and never on crash or device-removed messages. Every game `MessageBox`, dismissed or not, goes to the log. |
| Other exports | Forwarded straight to the system DLL through a jump table. |

Known limitations: devices created through `D3D12GetInterface`/`DeviceFactory` don't go through the fix (the game doesn't use that path), and the popup is only intercepted after the game creates its first D3D12 device.

## Building from source

Install Visual Studio 2022 or newer (or "Build Tools for Visual Studio") with the **Desktop development with C++** workload, ticking only:

- MSVC x64/x86 build tools (latest)
- Windows 11 SDK

Then, in an x64 PowerShell:

```powershell
.\scripts\build_spider_proxy.ps1      # builds release\d3d12.dll
.\tests\smoke_test.ps1                # loads the DLL and creates a real device
.\tests\config_test.ps1               # INI parser and export stubs
.\tests\test_install.ps1              # installer/uninstaller against a fake folder
.\scripts\install_fix.ps1             # installs the freshly built DLL
```

The build uses `/W4 /permissive- /guard:cf /Brepro` and the static CRT (no redistributable needed). Every `v*` tag pushed to GitHub builds the release automatically, with `SHA256SUMS.txt` and a provenance attestation.

## What's new in v2.1

**Stability alongside other tools**
- If a capture tool or overlay swaps or undoes the device hooks, the fix stays active instead of breaking shader creation.
- Feature-level requests above 12_1 go straight to the driver, with no faked support.

**Performance**
- The log records each event once. With `LogLevel=2`, `CheckFeatureSupport` no longer stutters (from ~28 µs with 4 ms spikes down to ~0.3 µs on the RX 580).
- GPU detection runs once per adapter.

**Install and removal**
- Atomic DLL copy (temp file, hash, swap): an interruption never leaves a corrupted DLL.
- Backups are built in a temp folder and renamed at the end; names include milliseconds.
- The uninstaller requires `manifest.json` and checks the hash before deleting any backup.
- Read-only files no longer break installation.
- Game-folder detection no longer fails with a Steam library on a disconnected drive or an accented path.

**GUI**
- "Alternar cache.pso" also covers `%APPDATA%\Insomniac Games\Marvel's Spider-Man 2`, where the game writes its own cache. The uninstaller only re-enables caches the GUI disabled.
- The window can't be closed mid-operation and shows the output of an elevated install.

**Documentation**
- The real cause of the RX 580 failure is explained: the driver rejects depth-only pipelines whose pixel shader uses ROVs.

## What's new in v2.0

**Your files are safer**
- The uninstaller no longer deletes backups when restoring fails or is refused.
- Reinstalling over another mod creates a new backup instead of overwriting it.
- The uninstaller won't touch a `d3d12.dll` that isn't SpiderFix.

**Installation**
- Automatic game-folder detection (Steam and Epic); `-GamePath` is now optional.
- Administrator elevation waits for the result and passes the exit code through.
- Fixed release package: the zip now installs as-is.

**DLL**
- GPU detection uses the adapter the game actually picks, not the system default.
- Fixed float arguments being corrupted in exports called before the first device.
- Failing to load the system DLL now returns an error instead of a false success.
- The popup is only dismissed when it really is the requirements notice; real errors show up.
- INI tolerates UTF-8 with BOM, UTF-16 and comments; `ForceActive=0` wins over the environment variable.
- Safe log rotation, and every `MessageBox` is logged.

**GUI**
- No longer freezes while installing, shows the folder's state and produces a fuller diagnostic.

**Project**
- Releases built by CI with SHA256 and attestation; actions pinned by SHA; tests cover forced mode, the INI parser and the backup scenarios.

## Tested versions

| Game version | Store | GPU | Driver | Result |
|--------------|-------|-----|--------|--------|
| 2.810.0.0 | Steam | RX 580 | 26.5.2 | ✅ |

Tested on another card? Open an issue with the result to get it added here.

## Antivirus and safety

Some antivirus products flag a false positive: the DLL is unsigned and patches the game's import table to dismiss the popup. Before allowing it, check the download's hash and attestation. If you need an exclusion, add it for the game folder only.

The DLL never touches the network, only writes its own log in the game folder and does not modify system files.

## Disclaimer

Unofficial project, not affiliated with Nixxes, Insomniac, Sony or AMD. Tested on the RX 580; on another card, do a short test before a long session. MIT license (see [LICENSE](LICENSE)).
