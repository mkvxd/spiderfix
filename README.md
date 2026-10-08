# SpiderFix

[English version](README_EN.md)

Uma `d3d12.dll` proxy que deixa Marvel's Spider-Man 2 abrir em GPU sem feature level 12_1. Você copia um arquivo para a pasta do jogo e joga.

## Requisitos

- Windows 10 ou 11 x64.
- Cópia Steam do jogo.
- Para compilar: MSVC x64 e Windows SDK.

## Instalação

Rode no PowerShell a partir da pasta do repositório:

```powershell
.\scripts\install_fix.ps1 -GamePath "C:\Program Files (x86)\Steam\steamapps\common\Marvel's Spider-Man 2"
```

O script confere o jogo fechado, guarda o arquivo anterior em `SpiderFixBackup_original` com `manifest.json` e valida o hash após copiar. Para remover:

```powershell
.\scripts\uninstall_fix.ps1 -GamePath "C:\Program Files (x86)\Steam\steamapps\common\Marvel's Spider-Man 2"
```

O uninstall restaura o backup e limpa as pastas criadas. Ele recusa apagar `d3d12.dll` de outro mod sem backup.

## O que a proxy faz

- Tenta criar o device em 12_0 antes do nível pedido, com repetição em 11_1 e 11_0. Sonda com `ppDevice` nulo recebe o mesmo tratamento.
- Responde 12_0 em `CheckFeatureSupport(FEATURE_LEVELS)` quando o driver nega ou reporta menos, sem tocar no resto.
- Retenta PSO depth-only sem pixel shader quando o driver devolve `E_INVALIDARG`. Sem isso a GPU trava (`0x887A0006`).
- Dispensa o popup de GPU incompatível e devolve o botão que cada tipo de diálogo espera.
- Resolve a jump-table uma vez e repassa os outros 18 exports para a `d3d12.dll` do System32.
- Detecta GPU 12_1 ou superior e ignora o fix, repasse puro sem hooks. `SPIDERFIX_FORCE=1` força o fix.

## Variáveis de ambiente

| Variável | Efeito |
| --- | --- |
| `SPIDERFIX_LOG=0` | Desliga o log em `spiderfix.log` |
| `SPIDERFIX_DEPTH_RETRY=0` | Desliga o retry depth-only (o jogo trava sem ele) |

## Compilação

```powershell
.\scripts\build_spider_proxy.ps1
```

Exige PowerShell x64, MSVC x64 e Windows SDK. Gera `release\d3d12.dll` e `release\d3d12.pdb` com `/W4 /permissive- /guard:cf`.

## Problemas comuns

- Jogo nem abre após instalar: confira se `release\d3d12.dll` e `Spider-Man2.exe` são x64 e se o hash na pasta do jogo bate com o do release. O `install_fix.ps1` valida os dois.
- Trava no loading: renomeie `cache.pso` para `cache.pso.disabled` na pasta do jogo.
- Sem log: `SPIDERFIX_LOG=0` global desliga a escrita. Apague a variável ou defina `1`.

## Aviso

Projeto não oficial, sem vínculo com Nixxes, Insomniac, Sony ou AMD. Testado apenas na RX 580; outra placa pede teste próprio antes de sessão longa.
