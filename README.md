<div align="center">

# SpiderFix

**Faz Marvel's Spider-Man 2 abrir em placas de vídeo sem DirectX 12 feature level 12_1.**

![Versão](https://img.shields.io/badge/vers%C3%A3o-2.1-2ea44f)
![Windows](https://img.shields.io/badge/Windows-10%20%7C%2011%20x64-0078D4?logo=windows&logoColor=white)
![DirectX](https://img.shields.io/badge/DirectX-12-107C10)
![Testado](https://img.shields.io/badge/testado-RX%20580-ED1C24?logo=amd&logoColor=white)

[English](README_EN.md)

</div>

O jogo exige feature level 12_1 e se recusa a abrir em placas que param no 12_0, como a RX 580. O SpiderFix é uma `d3d12.dll` que fica na pasta do jogo: ela corrige as poucas chamadas que quebram nessas placas e repassa todo o resto para a DLL original do Windows. Nada no sistema é alterado e a remoção devolve a pasta ao estado anterior.

Em placas que já suportam 12_1, a DLL detecta isso sozinha e não interfere.

## Sumário

- [Instalação](#instalação)
- [Remoção](#remoção)
- [Interface gráfica](#interface-gráfica)
- [Configuração](#configuração)
- [Solução de problemas](#solução-de-problemas)
- [Como funciona](#como-funciona)
- [Compilar a partir do código](#compilar-a-partir-do-código)
- [Novidades da v2.1](#novidades-da-v21)
- [Novidades da v2.0](#novidades-da-v20)
- [Versões testadas](#versões-testadas)
- [Antivírus e segurança](#antivírus-e-segurança)
- [Aviso legal](#aviso-legal)

## Instalação

Requisitos: Windows 10 ou 11 x64 e o jogo pela Steam ou pela Epic.

1. Feche o jogo.
2. Baixe `spiderfix.zip` e `SHA256SUMS.txt` em [Releases](https://github.com/mkvxd/spiderfix/releases/latest).
3. Confira o download (opcional, recomendado):

   ```powershell
   Get-FileHash .\spiderfix.zip -Algorithm SHA256              # deve bater com a linha spiderfix.zip do SHA256SUMS.txt
   gh attestation verify spiderfix.zip --repo mkvxd/spiderfix  # requer GitHub CLI
   ```

4. Extraia o zip, abra o PowerShell dentro da pasta extraída e libere os arquivos baixados da internet:

   ```powershell
   Get-ChildItem -Recurse | Unblock-File
   ```

5. Instale:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\scripts\install_fix.ps1
   ```

   O instalador acha a pasta do jogo nas bibliotecas da Steam e nos manifestos da Epic. Se não achar, informe o caminho:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\scripts\install_fix.ps1 -GamePath "D:\SteamLibrary\steamapps\common\Marvel's Spider-Man 2"
   ```

6. Abra o jogo normalmente. Um aviso de 4 segundos no canto da tela confirma que o SpiderFix carregou.

O `-ExecutionPolicy Bypass` vale só para aquele comando; a política do sistema não muda. Se a pasta do jogo exigir administrador, o script pede elevação (UAC) e espera o resultado.

**O que o instalador faz:** confere que o jogo está fechado, guarda qualquer `d3d12.dll` que já esteja na pasta (de outro mod, por exemplo) num backup com `manifest.json`, copia a DLL e valida o hash. Se a cópia sair corrompida, ele desfaz.

## Remoção

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\uninstall_fix.ps1
```

O desinstalador restaura o backup mais recente e só então apaga as pastas de backup. Ele é conservador de propósito:

- se a `d3d12.dll` da pasta não for o SpiderFix (você instalou outro mod por cima), ele não mexe em nada;
- se o backup não bater com o hash registrado, ele não restaura e mantém o backup, avisando o caminho.

Os dois scripts aceitam `-WhatIf` para mostrar o que fariam sem alterar nada.

## Interface gráfica

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\SpiderFixGUI.ps1
```

A janela mostra o estado da pasta selecionada (instalado e versão, não instalado, ou outra `d3d12.dll` presente) e tem quatro botões:

| Botão | Ação |
|-------|------|
| Instalar | Roda o `install_fix.ps1` sem travar a janela e mostra a saída. |
| Desinstalar | Roda o `uninstall_fix.ps1`. |
| Alternar cache.pso | Ativa ou desativa o cache de shaders do jogo nos dois lugares onde ele fica: a pasta do jogo e `%APPDATA%\Insomniac Games\Marvel's Spider-Man 2`. Útil se travar no loading. |
| Diagnóstico | Gera um relatório na Área de Trabalho: versão do Windows e do jogo, conteúdo do `spiderfix.ini`, outras DLLs proxy na pasta e SHA256 da DLL. Anexe esse arquivo ao abrir uma issue. |

## Configuração

Nada precisa ser configurado. Para ajustar, copie `spiderfix.ini.example` para a pasta do jogo com o nome `spiderfix.ini`. Cada opção também pode vir de uma variável de ambiente; **uma chave presente no INI sempre vence a variável**, inclusive quando vale 0.

| Chave INI | Variável | Padrão | Efeito |
|-----------|----------|--------|--------|
| `Log` | `SPIDERFIX_LOG` | 1 | Grava `spiderfix.log` ao lado da DLL. |
| `LogLevel` | — | 1 | 0 desliga, 1 registra cada evento novo uma vez, 2 registra também cada retry de PSO repetido. Vence `Log`. |
| `ForceActive` | `SPIDERFIX_FORCE` | 0 | Aplica o fix mesmo em placa 12_1 ou superior. |
| `DepthRetry` | `SPIDERFIX_DEPTH_RETRY` | 1 | Recria PSOs depth-only sem pixel shader quando o driver recusa. Sem isso o jogo trava. |
| `HookPipelineLibrary` | `SPIDERFIX_HOOK_LIB` | 1 | Aplica o mesmo retry aos shaders carregados do `cache.pso`. |
| `ShowOSD` | `SPIDERFIX_OSD` | 1 | Mostra o aviso de 4 s ao abrir o jogo. |

Valores aceitos: `0`/`1` (e `2` em `LogLevel`), `yes`/`no`, `y`/`n`, `true`/`false`, `on`/`off`. Comentários com `#` ou `;`, inclusive no fim da linha. O arquivo pode ser ANSI, UTF-8 (com ou sem BOM) ou UTF-16. Chaves e valores inválidos são apontados no log.

O log tem no máximo 512 KB; ao passar disso vira `spiderfix.log.old` e um novo começa.

## Solução de problemas

| Sintoma | O que fazer |
|---------|-------------|
| "A execução de scripts foi desabilitada" | Rode `Get-ChildItem -Recurse \| Unblock-File` na pasta extraída e use `powershell -ExecutionPolicy Bypass -File ...`. |
| O instalador não acha o jogo | Passe `-GamePath` com a pasta que contém `Spider-Man2.exe`. |
| O jogo não abre depois de instalar | Gere o diagnóstico na interface gráfica e veja se há outra DLL proxy (`dxgi.dll`, `dinput8.dll`...) na pasta. Abra uma issue com o relatório e o `spiderfix.log`. |
| Trava na tela de loading | Desative o `cache.pso` (botão na interface, ou renomeie para `cache.pso.disabled` na pasta do jogo **e** em `%APPDATA%\Insomniac Games\Marvel's Spider-Man 2`). O jogo recompila os shaders na próxima abertura. Com o cache ativo, o jogo compila menos shaders durante a partida e engasga menos. |
| Algum efeito visual incompleto | Efeito conhecido do retry de PSO (veja [Como funciona](#como-funciona)). É o preço para o jogo não travar. |
| O `spiderfix.log` não aparece | Confira se o INI não tem `Log=0` ou `LogLevel=0` e se não existe uma variável `SPIDERFIX_LOG=0` no sistema. |
| Quero saber se o fix está ativo | O aviso de 4 s diz "modo FIX" ou "modo BYPASS". O log registra cada passo. |

Problema persistente: abra uma [issue](https://github.com/mkvxd/spiderfix/issues) com o relatório de diagnóstico e o `spiderfix.log`.

## Como funciona

O Windows carrega a `d3d12.dll` da pasta do jogo antes da do sistema. O SpiderFix aproveita isso e carrega a DLL original pelo caminho completo do System32.

| Parte | O que faz |
|-------|-----------|
| Detecção | Pergunta ao driver se a placa **que o jogo escolheu** suporta 12_1 (importa em notebooks com duas GPUs). Se suportar, tudo é repassado sem nenhum hook. |
| Criação do device | Tenta 12_0 antes do nível pedido pelo jogo, depois 11_1 e 11_0. Pedidos acima de 12_1 (por exemplo 12_2) passam direto, sem fingir suporte. Erros que não são de suporte voltam intactos para o jogo. |
| `CheckFeatureSupport` | Responde 12_0 na consulta de feature levels quando o driver reporta menos. Nenhuma outra consulta é alterada: recursos que a placa não tem continuam reportados como ausentes. |
| PSO depth-only | Na RX 580 o driver recusa pipelines só de profundidade cujo pixel shader usa ROV (rasterizer ordered views); com alpha-test comum (`clip`) eles funcionam. O SpiderFix recria esses pipelines sem o shader, o que evita o travamento `0x887A0006`. O custo é perder as escritas ROV desse passe, o que pode deixar algum efeito incompleto. |
| Aviso de GPU incompatível | Dispensa o popup de requisitos do jogo. Só age nos 3 primeiros minutos e nunca em mensagens de crash ou de device removido. Toda `MessageBox` do jogo, dispensada ou não, vai para o log. |
| Demais exports | Repassados direto para a DLL do sistema por uma tabela de salto. |

Limitações conhecidas: devices criados via `D3D12GetInterface`/`DeviceFactory` não passam pelo fix (o jogo não usa esse caminho), e o popup só é interceptado depois que o jogo cria o primeiro device D3D12.

## Compilar a partir do código

Instale o Visual Studio 2022 ou mais novo (ou o "Build Tools for Visual Studio") com a carga de trabalho **Desenvolvimento para desktop com C++**, marcando só:

- Ferramentas de Build do MSVC x64/x86 (mais recente)
- SDK do Windows 11

Depois, num PowerShell x64:

```powershell
.\scripts\build_spider_proxy.ps1      # gera release\d3d12.dll
.\tests\smoke_test.ps1                # carrega a DLL e cria um device real
.\tests\config_test.ps1               # parser do INI e stubs de export
.\tests\test_install.ps1              # instalador/desinstalador numa pasta falsa
.\scripts\install_fix.ps1             # instala a DLL recém-compilada
```

O build usa `/W4 /permissive- /guard:cf /Brepro` e CRT estático (não depende de redistributable). Cada tag `v*` publicada no GitHub gera o release automaticamente, com `SHA256SUMS.txt` e atestado de proveniência.

## Novidades da v2.1

**Estabilidade com outros programas**
- Se uma ferramenta de captura ou overlay trocar ou desfizer os hooks do device, o fix continua ativo em vez de quebrar a criação de shaders.
- Pedidos de feature level acima de 12_1 passam direto ao driver, sem fingir suporte.

**Desempenho**
- O log grava cada evento uma vez. Com `LogLevel=2`, `CheckFeatureSupport` deixou de engasgar (de ~28 µs com picos de 4 ms para ~0,3 µs na RX 580).
- A detecção de GPU roda uma vez por adaptador.

**Instalação e remoção**
- Cópia da DLL atômica (arquivo temporário, hash, troca): interrupção no meio não deixa DLL corrompida.
- Backups montados numa pasta temporária e renomeados no fim; nome com milissegundos.
- O desinstalador exige o `manifest.json` e confere o hash antes de apagar qualquer backup.
- Arquivos somente leitura não quebram mais a instalação.
- A detecção da pasta do jogo não falha com biblioteca Steam em disco desconectado nem com acento no caminho.

**Interface gráfica**
- "Alternar cache.pso" cobre também `%APPDATA%\Insomniac Games\Marvel's Spider-Man 2`, onde o jogo grava o próprio cache. O desinstalador só reativa caches que a interface desativou.
- A janela não fecha no meio de uma operação e mostra a saída da instalação feita como administrador.

**Documentação**
- A causa real do problema na RX 580 está explicada: o driver recusa pipelines só de profundidade cujo pixel shader usa ROV.

## Novidades da v2.0

**Segurança dos seus arquivos**
- O desinstalador não apaga mais backups quando a restauração falha ou é recusada.
- Reinstalar por cima de outro mod cria um backup novo em vez de sobrescrever.
- O desinstalador não mexe numa `d3d12.dll` que não seja o SpiderFix.

**Instalação**
- Detecção automática da pasta do jogo (Steam e Epic); `-GamePath` virou opcional.
- A elevação de administrador espera o resultado e repassa o código de saída.
- Pacote do release corrigido: o zip agora instala direto.

**DLL**
- Detecção de GPU pelo adaptador que o jogo usa, não pelo padrão do sistema.
- Corrigida a corrupção de argumentos float em exports chamados antes do primeiro device.
- Falha ao carregar a DLL do sistema devolve erro em vez de sucesso falso.
- Popup dispensado só quando é de fato o aviso de requisitos; erros reais aparecem.
- INI tolerante a UTF-8 com BOM, UTF-16 e comentários; `ForceActive=0` vence a variável de ambiente.
- Log com rotação segura e todas as `MessageBox` registradas.

**Interface gráfica**
- Não congela durante a instalação, mostra o estado da pasta e gera um diagnóstico mais completo.

**Projeto**
- Releases gerados pela CI com SHA256 e atestado; actions fixadas por SHA; testes cobrem o modo forçado, o parser do INI e os cenários de backup.

## Versões testadas

| Versão do jogo | Loja | GPU | Driver | Resultado |
|----------------|------|-----|--------|-----------|
| 2.810.0.0 | Steam | RX 580 | 26.5.2 | ✅ |

Testou em outra placa? Abra uma issue com o resultado para entrar nesta tabela.

## Antivírus e segurança

Alguns antivírus acusam falso positivo: a DLL não é assinada e altera a tabela de importação do jogo para dispensar o popup. Antes de liberar, confira o hash e o atestado do download. Se precisar criar exclusão, faça só para a pasta do jogo.

A DLL não acessa a rede, só grava o próprio log na pasta do jogo e não altera arquivos do sistema.

## Aviso legal

Projeto não oficial, sem vínculo com Nixxes, Insomniac, Sony ou AMD. Testado na RX 580; em outra placa, faça um teste curto antes de uma sessão longa. Licença MIT (veja [LICENSE](LICENSE)).
