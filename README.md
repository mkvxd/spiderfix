<div align="center">

# SpiderFix

**Proxy `d3d12.dll` que abre Marvel's Spider-Man 2 em GPUs sem feature level 12_1.**

![Windows](https://img.shields.io/badge/Windows-10%20%7C%2011%20x64-0078D4?logo=windows&logoColor=white)
![DirectX](https://img.shields.io/badge/DirectX-12-107C10)
![C++](https://img.shields.io/badge/C%2B%2B-MSVC%20x64-00599C?logo=cplusplus&logoColor=white)
![Testado](https://img.shields.io/badge/testado-RX%20580-ED1C24?logo=amd&logoColor=white)

[English](README_EN.md)

</div>


O jogo exige GPU com feature level 12_1 e recusa abrir em placas que ficam em 12_0, como a RX 580. O SpiderFix entra no lugar da `d3d12.dll`, corrige as chamadas que quebram nessas placas e repassa o resto para a DLL do sistema. Você copia um arquivo para a pasta do jogo e joga.

## Sumário

- [Início rápido](#início-rápido)
- [Requisitos](#requisitos)
- [Instalação e remoção](#instalação-e-remoção)
- [O que a proxy faz](#o-que-a-proxy-faz)
- [Configuração](#configuração)
- [Interface gráfica](#interface-gráfica)
- [Compilação](#compilação)
- [Solução de problemas](#solução-de-problemas)
- [Créditos](#créditos)
- [Aviso](#aviso)

## Início rápido

1. Feche o jogo.
2. Clone o repositório e abra o PowerShell na pasta dele:

   ```powershell
   git clone https://github.com/mkvxd/spiderfix.git
   cd spiderfix
   ```

3. Rode o instalador apontando para a pasta do jogo:

   ```powershell
   .\scripts\install_fix.ps1 -GamePath "C:\Program Files (x86)\Steam\steamapps\common\Marvel's Spider-Man 2"
   ```

4. Abra o jogo pela Steam.

## Requisitos

| Uso | Requisito |
|-----|-----------|
| Jogar | Windows 10 ou 11 x64 |
| Jogar | Cópia Steam do jogo |
| Compilar | MSVC x64 e Windows SDK |

## Instalação e remoção

O `install_fix.ps1` executa estes passos:

- Confere se o jogo está fechado.
- Guarda o arquivo anterior em `SpiderFixBackup_original`, junto com um `manifest.json`.
- Copia a DLL e valida o hash depois da cópia.

Para remover, use o `uninstall_fix.ps1`:

```powershell
.\scripts\uninstall_fix.ps1 -GamePath "C:\Program Files (x86)\Steam\steamapps\common\Marvel's Spider-Man 2"
```

O uninstall restaura o backup e apaga as pastas que o instalador criou. Ele recusa apagar a `d3d12.dll` de outro mod quando não existe backup.

## O que a proxy faz

| Comportamento | Detalhe |
|---------------|---------|
| Fallback de feature level | Tenta criar o device em 12_0 antes do nível pedido, depois em 11_1 e 11_0. Sondas com `ppDevice` nulo seguem a mesma sequência. |
| `CheckFeatureSupport` | Responde 12_0 em `FEATURE_LEVELS` quando o driver nega ou reporta menos. O resto das consultas passa sem alteração. |
| PSO depth-only | Repete a criação sem pixel shader quando o driver devolve `E_INVALIDARG`. Sem isso a GPU trava (`0x887A0006`). |
| Popup de GPU incompatível | Dispensa o aviso e devolve o botão que cada tipo de diálogo espera. |
| Exports | Resolve a jump-table uma vez e repassa os outros 18 exports para a `d3d12.dll` do System32. |
| Detecção de GPU | Em GPU 12_1 ou superior, a proxy ignora o fix e faz repasse puro, sem hooks. `SPIDERFIX_FORCE=1` força o fix. |

## Configuração

### Variáveis de ambiente

| Variável | Efeito |
|----------|--------|
| `SPIDERFIX_LOG=0` | Desliga o log em `spiderfix.log` |
| `SPIDERFIX_DEPTH_RETRY=0` | Desliga o retry depth-only (o jogo trava sem ele) |
| `SPIDERFIX_FORCE=1` | Força o fix em GPU 12_1 ou superior |

### Arquivo `spiderfix.ini`

Coloque o `spiderfix.ini` ao lado da DLL para ajustar as mesmas opções sem variável de ambiente. Use o `spiderfix.ini.example` como modelo.

Chaves disponíveis: `Log`, `LogLevel`, `ForceActive`, `DepthRetry`, `HookPipelineLibrary` e `ShowOSD`.

Na primeira abertura, o jogo mostra um aviso do SpiderFix por 4 segundos. `ShowOSD=0` desliga o aviso.

## Interface gráfica

O `scripts\SpiderFixGUI.ps1` abre uma janela com quatro ações:

- instalar
- desinstalar
- alternar o `cache.pso`
- exportar o diagnóstico para a Área de Trabalho

## Compilação

```powershell
.\scripts\build_spider_proxy.ps1
```

O script exige PowerShell x64, MSVC x64 e Windows SDK. Ele gera `release\d3d12.dll` e `release\d3d12.pdb` com as flags `/W4 /permissive- /guard:cf`.

## Solução de problemas

| Sintoma | O que fazer |
|---------|-------------|
| O jogo não abre depois da instalação | Confira se `release\d3d12.dll` e `Spider-Man2.exe` são x64 e se o hash da DLL na pasta do jogo bate com o do release. O `install_fix.ps1` valida os dois. |
| O jogo trava no loading | Renomeie `cache.pso` para `cache.pso.disabled` na pasta do jogo. |
| O `spiderfix.log` não aparece | Um `SPIDERFIX_LOG=0` global desliga a escrita. Apague a variável ou defina `SPIDERFIX_LOG=1`. |

Se o problema continuar, abra uma [issue](https://github.com/mkvxd/spiderfix/issues) e anexe o diagnóstico exportado pela interface gráfica.

## Aviso

Projeto não oficial, sem vínculo com Nixxes, Insomniac, Sony ou AMD. O teste cobriu a RX 580. Outra placa pede teste próprio antes de uma sessão longa.
