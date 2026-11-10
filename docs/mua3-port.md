# MUA3 port: estado e plano

Port por recompilação estática de Marvel Ultimate Alliance 3 (Switch) para Windows x86-64, usando este fork do
TotK recomp como base. Uso pessoal, a partir de um dump do próprio console.

## Layout

- `D:\Games\Marvel Ultimate Alliance 3\mua3-recomp\`: este projeto (clone local do TotK recomp; remote `totk-upstream`).
- `D:\Games\Marvel Ultimate Alliance 3\refs\totk-pc-recomp\`, `refs\LASRecomp\`: cópias de referência, só leitura.
- `D:\Games\Marvel Ultimate Alliance 3\toolchain\`: Python gerenciado pelo uv e cache do uv.

## Ambiente (preparado em 2026-11-03)

- MSYS2 UCRT64 em `C:\msys64`: clang/lld 23.1.4, ninja, gcc, make, sdl3, opus, libvpx, zstd, vulkan-devel, VMA,
  validation layers, shaderc, spirv-tools.
- `.venv` (Python 3.12) e `.venv-py314` (Python 3.14) com `tools/requirements*.txt`.
- uv: o app do Claude virtualiza `%APPDATA%`, o que quebra os junctions do uv. Antes de usar `uv`, definir
  `UV_PYTHON_INSTALL_DIR=D:\Games\Marvel Ultimate Alliance 3\toolchain\uv-python` e
  `UV_CACHE_DIR=D:\Games\Marvel Ultimate Alliance 3\toolchain\uv-cache`.
- .NET SDK 10.0.101 já instalado.

## Pendente no ambiente

- `hactool`: `.venv\Scripts\python tools\build_hactool.py` (precisa ser rodado pelo usuário ou liberado nas permissões).
- Visual Studio Build Tools (C++ e Windows SDK): só para compilar o tradutor de shaders `totk_xlate.exe`.
- Opcional, para engenharia reversa: Ghidra + Ghidra-Switch-Loader + JDK 21.

## Dump

- Base `010060700AC50000` v0, update `010060700AC50800` v524288, DLC Expansion Pass (ainda não usado). NSZ em
  `D:\Games\Marvel Ultimate Alliance 3\`, convertidos para `build\unpacked\{base,update}.nsp` com `tools/ncz.py`.
- Módulos: main, sdk, subsdk0, subsdk1, rtld. O update só troca o `main` (11,5 MB -> 14,5 MB NSO).
- Hashes em `build\extracted\hashes.json`; `build\extracted\expected-upd.json` é o formato que `build_game.py` lê.

## Comandos (a partir de `mua3-recomp\`)

```
.venv-py314\Scripts\python -I tools\mua3_extract.py --base build\unpacked\base.nsp --update build\unpacked\update.nsp --keys "D:\Games\Marvel Ultimate Alliance 3\prod.keys"
.venv\Scripts\python recomp\gen.py --exefs build\extracted\upd\exefs --out generated\upd --modules main,subsdk0,subsdk1,sdk --patches recomp\patches-mua3-v401.txt
.venv\Scripts\python tools\build_game.py --game mua3 --version upd --gen generated\upd --out build\upd --exefs build\extracted\upd\exefs --romfs build\extracted\upd\romfs --expected build\extracted\expected-upd.json
build\upd\totk.exe --selftest
```

`build_game.py` e `gen.py` não rodam com `python -I` (importam módulos irmãos).

## Estado (2026-11-03)

- Extração: ExeFS ok; RomFS base ok; merge do update em andamento.
- Lift: 104.614 funções, 567 MB de C, 0 instruções não suportadas, 250 s.
- Compilação: 569 objetos, 0 erros, 236 s. Link: `build\upd\totk.exe` (151,5 MB). `--selftest` passa.
- Runtime ainda é o do TotK sem nenhuma adaptação (patches, versão, gráficos).

## O que o MUA3 usa (log do Ryujinx 1.3.3, v4.0.1, boot até gameplay)

- Nome interno do programa: "Cube" (motor da Team Ninja). Guest log via `lm` (memory.cpp no boot).
- 3 núcleos (0 a 2), áudio `audren` REV6, `hid` com giroscópio e vibração, `vi` com 1 layer gerenciado.
- Módulos: `subsdk0` é a biblioteca `movie` da Nintendo (port do stagefright do Android, decodifica pelo NvDec via
  `mm:u`); `subsdk1` é o `glslc` (compilador de shaders NVN em tempo de execução); `sdk` exporta os 447 `nn::` que o
  `main` importa.
- Vídeos: 127 `.mp4` no RomFS (o TotK usa VP9 via libvpx). Vai precisar de decodificador H.264 no host.
- RomFS: formatos da Koei Tecmo (`.g1t`, `.kslt`, `.ktsl2asbin`, `.zl_`); o jogo decodifica tudo sozinho.
- Serviços vistos (CMIF): am/oe, acc (BAAS), aoc (DLC), fs (RomFS por `OpenDataStorageByCurrentProcess`, save),
  hid, nvdrv, vi + HOSBinder, time (relógio de rede), settings, pctl, friends, bcat, apm, mm, lm, audren, audout dev.
- SVCs vistas (27): todas comuns; conferir contra as 38 do runtime.
- `nn::` importados pelo main: 447, dos quais 62 o runtime do TotK já substitui. Faltam sobretudo socket, ldn, bcat,
  nifm, friends, err, ssl (online, podem virar "sem rede"), e partes de os, audio, hid, account, vi, time.

## Rodada de adaptação 1 (2026-11-03)

Primeira execução: parou no boot, `nn::init::Start` do SDK do MUA3 tem 2 callbacks (`_ZN2nn4init5StartEmmPFvvES2_`).
Mudanças no runtime, todas atrás de `--game mua3` (`RT_MUA3`, `rt_game_mua3` em modules.c) ou por nome de símbolo:

- `boot.c`: aceita `nn::init::Start` de 2 callbacks; DT_INIT de todos os módulos sem contagens do TotK; checkpoints
  só registram o chamador.
- `frame_rate.c` (posição do jogador do TotK) e `resolution.c` (patch page do TotK): desligados no MUA3.
- `RT_PROGRAM_ID` (host.h): `svc.c` GetInfo e `fs_host.c` save_exists usam o id do MUA3.
- `ov_offline.c` (novo): nifm, socket, ldn, bcat, friends, nsd, ssl, ec, err::ShowError(variant) respondem offline.
- `ov_input.c`: formas antigas do hid (`GetNpadState`, sem `detail::`).
- `fs_host.c`: `CommitSaveData`, `GetDirectoryEntryCount`, `MountAddOnContent` (sem DLC), cache de DLC.
- `ov_system.c`: `OpenPreselectedUser`, conta de rede offline, `AsyncContext`, aoc (0 DLC, evento),
  `oe::GetDisplayVersion` ("4.0.1"), relógio de rede, `detail::UnexpectedDefaultImpl` (fatal com mensagem).
- `ov_gfx.c`: `vi` Close/Destroy/Finalize, `nv::FinalizeGraphics`, `SetLayerScalingMode`.
- `ipc.c`: `TOTK_IPC_SOFT=1` responde "não registrado" a serviços desconhecidos em vez de parar.

Ainda não feito: vídeo (`movie`/`mm`, H.264), `glslc` em tempo de execução, DLC de verdade, `swkbd` do SDK antigo.

## Rodada de adaptação 2 (2026-11-03): primeira imagem

Execuções 2 a 13. O jogo passa boot, constructors, nnMain, inicializa NVN/Vulkan, áudio (SDL), controle, conta e
save, e aos ~45 s de jogo mostra o logo da Marvel (`build\upd\shot13.png`). Mudanças:

- `fs_host.c`: `SetAllocator` (o do SDK antigo abre fsp-srv).
- `ov_system.c`: `settings::LanguageCode::Make` (tabela fixa).
- `ov_offline.c`: `crypto::GenerateCryptographicallyRandomBytes` (plat_random), `~DeliveryCacheFile`.
- NVN: `nvnQueueWaitSync` (ordem de submit), `nvnQueueAcquireTexture`, `nvnDeviceApplyDeferredFinalizes` (novas
  linhas no fim de `nvn_procs.inc`), `nvnCommandBufferSetCopyImageStride` (`p_copy_stride.image`),
  `nvnTexturePoolRegisterImage` (= RegisterTexture).
- `vk_exec_draw.c`: **programa por estágio** (`stage_prog[6]`, `prog_of`). O MUA3 cria um programa por estágio e
  liga VS e FS separados; antes só o último ficava e todo draw era descartado (tela preta).
- Tradutor de shaders compilado (`build\xlate\totk_xlate.exe`; VS Build Tools 2022/18 já estavam instalados).

Ferramentas úteis: `TOTK_SHOTS=w0-45:3 TOTK_SHOTS_DIR=dir` (prints por segundo de jogo), `--seconds S --shot f.png`,
`TOTK_DUMP=<frame>` ou **F7** na janela (passes.txt, images.txt, PNGs; stencil, MSAA resolvido, BC decodificado, `.raw` dos formatos float/16/32 bits, lidos por `tools\dump_raw.py <arquivo>.raw X Y [W H]`; `TOTK_DUMP_RAW=0` desliga). `TOTK_DUMP_PASS=<n>[,sh:<hash>]`: detalhe dos passes escolhidos em draws.txt: alvos antes do pass e depois de cada draw, vertex/index/indirect/SSBO em `.bin`, todos os uniform blocks e texturas (opções no topo de `runtime\vk_dump.c`). `TOTK_TRACE=fs|gfx`. Análises em scratchpad: svc_reach.py (imports que chegam a
GetServiceHandle), nvn_gap.py.

## Rodada 3 (2026-11-03/04): título, save e vídeo

- O jogo chega à tela de título ("PRESS +"), cria save e entra no menu de saves (execução 14, com o usuário no
  controle). `TOTK_INPUT_SCRIPT=build\upd\to_movie.txt` aperta + e A por script.
- `gfx_window_win.c`: `TOTK_MONITOR=<n>|secondary` abre a janela em outro monitor; encolhe (16:9) se não couber.
  O usuário quer o jogo no segundo monitor (DISPLAY1, retrato): rodar sempre com `TOTK_MONITOR=secondary`.
- NVN: `nvnCommandBufferDrawArraysIndirect` (`OP_DRAW_IDX_IND` com itype -1, `vkCmdDrawIndirect`).
- Vídeo: todos os `.mp4` são H.264 1920x1080 60 fps + AAC estéreo 48 kHz.
  - `movie_codec.h`, `movie_codec_win.c` (Media Foundation: H.264 -> NV12, AAC -> PCM16), `movie_codec_sdl.c` (stub).
  - `ov_movie.c`: no MUA3 os decoders tipo 1 (H.264) e 4 (áudio) são do host; worker próprio (`worker_mc`) com fila
    de spill; áudio sem cadência; formato de saída de áudio `audio/raw`.
  - O extrator do jogo não passa SPS/PPS (sem csd-0, sem NAL 7/8 no fluxo): `romfs.c` anota o último `.mp4` aberto
    (`movie_note_open`) e `configure_mc` lê o `avcC` do arquivo (`mp4_avcc`).
  - Resultado: AAC e H.264 decodificam (271 quadros do `EV_0101_01.mp4`), mas a tela fica preta durante o vídeo.
- **Bug aberto:** depois que o vídeo toca, a GPU cai (`VK_ERROR_DEVICE_LOST`, "write invalid at 0") num passo de
  480x270 da tela de título. Sem vídeo (execuções 21, 22) não cai. Suspeitas: tempo de vida de pools/texturas que o
  jogo libera no fim do vídeo; `SetCopyImageStride` (novo, nunca zerado entre command buffers). O vídeo de abertura
  não toca em toda execução, o que dificulta reproduzir.
- Validação Vulkan: a camada do MSYS2 não carregou com `VK_ADD_LAYER_PATH`; instalar o Vulkan SDK resolve.
- Pendências: Opus por hardware (`HardwareOpusMultiStreamDecoder`) se o jogo usar; `audout` do subsdk0.

## Rodada 4 (2026-11-05): mudanças do Cadence of Hyrule trazidas para cá

O Cadence (repo `D:\Games\Cadence of Hyrule\cadence-recomp`, remote `cadence`) partiu do commit 1678c1c e tem o mesmo
SDK. Aplicado o diff 1678c1c..cadence/main de `runtime/`, `tools/build_game.py`, `game_profile.py`, `vendor_titles.py`,
`extract.py` e `tools/data/cadence` (fora: README, VERSION, instalador, make_release). Principais:
- `rt_game_id` e tabela `GAMES` (build `--game mua3` gera `build\upd\mua3.exe`, lê `mua3.ini`, log `logs\mua3.log`).
- NVN: profundidade padrão NEAR_IS_MINUS_W e origem inferior esquerda para quem não as define; passo de renderização
  mantido aberto entre barreiras do jogo (`TOTK_KEEP_PASS=0` desliga).
- Vídeo: decodificação síncrona na thread do jogo fora do TotK, sem cadência de vídeo, quadros entregues também em
  AcquireOutputBufferIndex; SPS/PPS da chave `avcc` da MediaData (a leitura do arquivo fica como plano B).
- `audout.c`, pilhas em .data/.bss (MapMemory), EnsureSaveData, IsVibrationPermitted, GetMouseState,
  StandardUserSystemClock::now, SubmitNetworkRequestAndWait, ShowShopApplicationInformation.
- `game_profile.py` ganhou o perfil `mua3` (padrão deste repo) e `tools/data/mua3/expected-v{1.0.0,4.0.1}.json`.

Resultado (execução 27): a cutscene de novo jogo (`EV_0101_01`) toca inteira com legendas, sem queda da GPU.
`build\upd\new_game.txt` + `TOTK_INPUT=none` reproduz o caminho título -> novo jogo -> vídeo.

## Rodada 5 (2026-11-05): queda da GPU nos computes com texture buffers

- Execução 12: queda no dispatch 22 da submissão 22750, cs `a4560d3f` (chave `e4c94f0e…`), "instruction pointer".
- Causa provável: memória compartilhada pequena demais. O SPIR-V declara `uint[1024]` (4 KiB, padrão do Ryujinx quando
  `QueryComputeSharedMemorySize` dá 0), mas o controle NVN do programa pede 4656 bytes (+0x754, logo após o tamanho do
  grupo em +0x748). Valores do campo nos 61 computes: 0, 4, 304, 512, 1024, 1072, 4656 (14 shaders). Com buffers
  zerados os índices ficavam abaixo de 4 KiB; com dados reais passam, e Workgroup não tem robustez.
- Correção: `with_shared` em `gfx_shader.c` preenche `shared_bytes` com +0x754 nos computes (muda a chave só desses).
- Execuções 13 e 14 (`TOTK_TEXBUF` padrão): sem queda da GPU; a 14 rodou ~35 s a 29,6 fps até o usuário fechar.
  Próximo: `TOTK_TEXBUF=all` (computes com storage image também) e ver se a iluminação voltou.

- Execução 15 (`TOTK_TEXBUF=all`): sem queda, mas tudo escuro. O passe de iluminação termina com
  `cor * fog.a + fog.rgb` (névoa volumétrica); a textura de névoa ligada é uma 3D 1x1x1 RGBA16F toda zerada (alfa 0).
- Execução 16 (`TOTK_TRACE_3D=1`): há volumes reais 128x72x48, mas a cena usa os 1x1x1 ("sem névoa"). Ficam num pool
  de flags 0xa1 (CPU_NO_ACCESS) com a memória do próprio jogo; nenhuma cópia, clear, render ou storage bind os escreve.
  No NVN o conteúdo da memória quando o pool é criado vale como conteúdo inicial; o runtime nunca o enviava.
- Execução 17: a memória desses volumes é zero mesmo (hipótese do conteúdo inicial descartada e revertida).
- Execução 18: quem escreve os volumes (1x1x1 e 128x72x48) é o compute `b7dea23e` (local 16x16, grupos 8x5x1 / 1x1x1):
  grava `c3[0]` em cada texel. Ele lê o tamanho do volume por uma textura no handle da imagem (0x48 -> byte 0x120,
  faixa de imagens); `handle_slot` dava -1 para texturas nessa faixa, então o tamanho vinha do dummy 1x1x1 e só um texel
  era limpo. Correção: `GFX_TEX_AT_IMG` + `tex_handle` (vk_exec_draw.c) ligam a imagem do slot. O trace agora mostra
  também `c3` no dispatch. Execução 19: teste.

## Rodada 6 (2026-11-05): profiler de funções do LAS Recomp

- `runtime/fnprof.c` (adaptado do LAS Recomp, MIT; linha em `THIRD_PARTY_NOTICES.md`). O gerador já emitia
  `FN_ENTER` em toda função; antes só o `resolution.c` do TotK usava. Agora `g_fnprof` tem dois bits: 1 = espera do
  tamanho de render do TotK (`res_fn_hit`), 2 = profiler. Sem regerar o C: só o runtime recompila (~11 s).
- `TOTK_FNPROF=<de>:<até>` conta as entradas de função entre esses quadros apresentados (`vk_backend.c` chama
  `fnprof_frame`); `0:<até>` conta desde o início do processo. Grava `TOTK_FNPROF_OUT` (padrão `fnprof.txt` na pasta
  de trabalho): `<endereço> <contagem> <módulo+offset (símbolo)>`, mais chamadas primeiro.
- `TOTK_FNPROF_BT=<hex>[,...]`: na primeira entrada de cada função listada, o log mostra quem chamou (lr) e x0.
- `tools/fnprof_diff.py a.txt b.txt`: funções que só uma execução chamou e as que mais mudaram de contagem.
- Teste (40 s sem input, `TOTK_FNPROF=300:360`): 2.058 funções na janela, sem queda.
- Uso previsto para a cena escura: perfilar uma janela igual com e sem a correção (ou em cenas com e sem névoa) e
  comparar; procurar quem deveria escrever os volumes de névoa 128x72x48 e não é chamado.

## Rodada 7 (2026-11-05): correções trazidas do Astral Chain Recomp

O Astral Chain Recomp (`refsstral-chain-pc-recomp`, MIT) parte da mesma base TotK. Três correções dele valiam aqui:
- `BindProgram(NULL, estágios)` desliga só os estágios da máscara (813d115). Antes `ex->prog` virava NULL e todo draw
  até o próximo bind era descartado (no AC: sombras piscando). `prog_unbound` em `vk_exec.h`; `prog_of` devolve NULL
  para um estágio desligado. O log mostra `first BindProgram(NULL, ...)` quando o jogo faz isso.
- Alpha test do NVN (78c4828): `nvnCommandBufferSetAlphaRef` grava bind-state tipo 7; a função do ColorState e a
  referência vão nas respostas do fragment shader e o tradutor descarta como o Ryujinx. `IN_VERSION` 2 e
  `ANS_VERSION` 2: as entradas antigas do cache de shaders são descartadas (a primeira execução retraduz).
- `audren` gera um quadro a cada 5 ms como o DSP do Switch, em vez de rajadas no período do dispositivo (0018973).
- Fora: o filtro de sementes MOD0 do `recomp/module.py` (a7fb686). O MUA3 já sai com 0 instruções não suportadas e a
  mudança exigiria regerar todo o C.

## Rodada 8 (2026-11-06): correções de GPU NVIDIA do fe3h-recomp

O fe3h-recomp (gitlab.com/onerag90/fe3h-recomp, MIT, base LAS Recomp, jogo da Koei Tecmo) corrigiu na NVIDIA halves que
viravam infinito (sombra em áreas inteiras) e NaN espalhados por blurs (quadrados pretos). A GPU daqui é uma RTX 3060.
- `tools/xlate/patches/0016-half-clamp-and-nan-scrub.patch` (`build_xlate.py` agora espera 16 patches):
  `GeneratePackHalf2x16` (SPIR-V) limita cada valor a +-65504 antes de empacotar (NaN passa igual);
  `EmitterContext.PrepareForReturn` grava 0 numa saída de cor float que seja NaN ou infinito.
- Execução de 2026-11-06 (`TOTK_TEXBUF=all`, F7 em gameplay, `dump/`): 225 shaders traduzidos, 0 falhas, sem queda da GPU,
  ~29,7 fps. A cena continua escura: personagens em silhueta.
- **Achado:** o G-buffer (passe com 5 alvos: rgba8 sRGB, rgb10a2, rgba8, r11g11b10f, rgba8 sRGB, depois do pré-passe de
  profundidade) sai quase todo zero, inclusive as normais em rgb10a2, embora a profundidade esteja certa. Já era assim
  em todos os dumps antigos (`dump17`, `dump20`, `dump21`, `dump_play26`, `dump_before_r7`), então não vem do patch 0016.
  O passe de iluminação multiplica por esse G-buffer, por isso os personagens ficam pretos; a névoa 1x1x1 é um problema
  à parte. Próximo: `TOTK_DUMP_PASS=<passe do G-buffer>` e `TOTK_XLATE_KEEP=1` para ver o que os fragment shaders desse
  passe (`fs ed46d93abd8c823f`, `2f1b99b86698ebd8`) escrevem em cada alvo.

## Rodada 9 (2026-11-06): correções do tradutor vistas no Pokopia Recomp

O Pokopia Recomp (github.com/drakolordx7/switch2-recomp, GPL-3.0, base nxrecomp) corrigiu o mesmo tradutor de shaders do
Ryujinx depois de testes no hardware. Como é GPL, as correções foram reescritas aqui a partir do comportamento descrito,
sem copiar os patches. `build_xlate.py` agora espera 20 patches; o exe anterior ficou em `build\xlate\totk_xlate.pre0017.exe`.
- `0017-fmz-multiply-by-zero.patch`: FMUL/FFMA FP32 com `.FMZ` dão +0 (FFMA: c) quando um fator é zero, mesmo com Inf/NaN
  no outro. O Ryujinx ignorava o modo e 0 * Inf virava NaN.
- `0018-nan-safe-float-clamp-min-max.patch`: Clamp/Min/Max float viram NClamp/NMin/NMax (o `.SAT` do Maxwell leva NaN a 0 e
  o FMNMX devolve o operando que não é NaN; FClamp/FMin/FMax são indefinidos com NaN).
- `0019-pixld-returns-zero.patch`: PIXLD (não implementado, o registrador ficava com o valor antigo) devolve 0. O Pokopia
  usa `gl_SampleID`, mas isso força shading por amostra nos passes MSAA; os nossos shaders rodam uma vez por pixel.
- `0020-storage-base-prefers-driver-cbuf.patch`: em `c[x] + c[0]` o storage buffer vem do `c[0]` (driver), mesmo quando
  ele é o primeiro operando.
- Teste offline (sem rodar o jogo): os 225 shaders do cache (`build\upd\shader-cache\in`) traduzidos com o exe antigo e o
  novo. 225/225 ok, 0 inválidos no `spirv-val`. 114 mudaram, todos só pela troca F -> N do 0018 (79 NMax, 64 NClamp,
  37 NMin). FMZ, PIXLD e o caso do g2s não aparecem em nenhum desses 225; podem aparecer em cenas ainda não vistas.
  Script: `.venv\Scripts\python -I tools\xlate_diff.py <cache> <exe antigo> <exe novo> <pasta em build\>`.
- A chave do cache muda com o tradutor: a próxima execução retraduz tudo. Ainda não testado no jogo; ver se a cena
  escura / G-buffer zerado (rodada 8) muda com o 0018.

## Rodada 10 (2026-11-06): por que o G-buffer sai vazio

Três execuções com F7 em gameplay (`build\upd\dump_gbuf` com `TOTK_DUMP_PASS=31`, `dump_nodepth`, `dump_import`):
- Nenhum dos 458 draws do passe do G-buffer (passe 31, 5 alvos) muda um pixel. Com `TOTK_DEBUG_DS=2` (sem teste de
  profundidade) e com `TOTK_MEMORY=import` (sem espelho em VRAM) continua tudo zero: não é profundidade nem espelho.
- Os draws são `DrawElementsIndirect` e os argumentos valem `count 0, instances 1, first 0, base 0, base instance 0`
  em todos. Eles ficam num bloco de 16 MB (índices em +0, zerados nos 8 MB inteiros; 455 registros de 48 bytes em
  +0x800000, um por draw, todos `0 1 0 0 0 1 0 0 0 0 0 0`). É o estado inicial de um culling que deveria gravar os
  índices visíveis e somar os contadores.
- Ninguém no quadro escreve ali: nenhum SSBO de compute, nenhuma cópia, nenhum shader do pré-passe (sem stores nem
  atomics) e nenhum `ReportCounter`. O transform final do vertex shader do G-buffer está certo (a matriz de c3[1..4]
  leva a câmera a w = 0). Conclusão provisória: quem preenche o bloco é código de CPU do jogo (culling na CPU) que não
  roda ou não grava, ou uma operação de GPU que o dump não mostra.
- Próximo: (1) detalhar também o pré-passe (passe 30): se ele desenha com argumentos indiretos do mesmo bloco, os
  contadores existiam e foram zerados antes do G-buffer; (2) um rastreador de escrita no runtime (proteger a página do
  bloco e registrar o PC do código que escreve) para achar o código do culling.
- Disco D: com ~2 GB livres; os dumps detalhados (`dump_gbuf` 4 GB) precisam de espaço.

## Nota (2026-11-06): hipóteses do ssbu-recomp descartadas (verificação offline)

O ssbu-recomp (OpenSmash-Ultimate, GPL-3.0, NVN -> Vulkan próprio com tradutor derivado do suyu) teve dois problemas que
poderiam deixar o G-buffer zerado. Nenhum dos dois explica o caso do MUA3:
- **ClipDistance declarado e não escrito** (o Vulkan recorta por todos os elementos declarados): os 393 vertex shaders
  do cache declaram `gl_ClipDistance[8]` (struct do Ryujinx), mas nenhum escreve nele. Os passes que saem certos
  (pré-passe de profundidade, UI) usam o mesmo padrão, então a geometria não está sendo recortada por isso.
- **Amostrar a profundidade ligada como alvo** (a correção deles, para AMD, amostra uma cópia): o runtime já trata
  feedback loops (`feedback_prepare` em `vk_exec_draw.c`, `TOTK_FEEDBACK`); o log da RTX 3060 mostra o modo
  "feedback layout + by-region barrier". Dá para comparar com `TOTK_FEEDBACK=restart` numa execução com gameplay.
Bate com a rodada 10: os draws do G-buffer têm contagem 0 nos argumentos indiretos, então nada chega a rasterizar.

## Rodada 11 (2026-11-06): rastreador de escrita (execução 31) e o xcx-recomp

**Execução 31** (`TOTK_WATCH_IND=15`, rastreador em `nvn_api_cmd.c` `watch_indirect`, ainda não documentado):
- O bloco de argumentos começa em 0x139de5900: 16 bytes de cabeçalho (também usado como render condition, "on words no
  report wrote") e registros de 0x30 a partir de +0x10. Os índices ficam 8 MB antes (0x1395e5900...).
- A CPU grava o bloco uma vez: `f_70066e6f60` (inicialização: cabeçalho e o estado `count 0, instances 1` de cada
  registro) e `f_7000445440` (alguns campos +4/+0x24). O rastreador registra cada instrução só na primeira escrita, então
  escritas repetidas por essas mesmas instruções não aparecem.
- **Nenhuma escrita da CPU nos índices.** Quem deveria preencher índices e contadores é a GPU (ou uma cópia), e isso não
  acontece no runtime.
- Hipótese testada offline e descartada para este quadro: o tradutor do Ryujinx apaga em silêncio stores na memória
  global que não consegue mapear (`GlobalToStorage.cs`, nota "Failed to find storage buffer for global memory operation
  \"Store\""). No cache, só 1 compute (`d819a639b6163f1f` no hash FNV do dump; 5 variantes, 3 stores apagados cada) tem
  isso, e ele **não** roda no quadro do `dump/` (passes 0-30 usam outros 11 computes). Fica anotado: se aparecer em
  outra cena, os stores dele somem.
- Próximo: (1) ver se algum compute do quadro grava um SSBO num *alias* do bloco (outro endereço de GPU para a mesma
  memória); (2) `TOTK_FNPROF_BT=70066e6f60` para achar quem inicializa o bloco e seguir dali até quem deveria preenchê-lo.

**xcx-recomp** (github.com/jmars125/xcx-recomp, MIT, Xenoblade X DE; em `..\refs\xcx-recomp`): fork do TotK Recomp
0.1.1, então quase todo o runtime dele é *mais antigo* que a nossa base. Commits próprios que valiam aqui (em
`vk_texture.c`; linha nova no `THIRD_PARTY_NOTICES.md`):
- a3eedd2: alvos COMPRESSIBLE pequenos (até 4 KiB) num pool visível à CPU passam a ter espelho, para a CPU ler o que a
  GPU renderizou (no XCX: o medidor de luminância 1x1 da exposição automática; sem isso o ganho do tonemap ficava
  errado). Pode valer para a cena escura do MUA3. `TOTK_MIRROR_SMALL=0` volta à regra antiga. O write-back também pula
  memória que o host não mapeia mais.
- fe79c75: um slot de textura liberado segue primeiro uma textura viva com a mesma forma (endereço, formato, tamanho)
  antes da mais nova no endereço (no XCX: sombra dupla por ler o atlas com o layout errado).
- Fora: o resto é do Xenoblade (60 fps, interface larga, Linux, AMD RDNA2) ou já existe na nossa base.
- Build e `--selftest` ok; ainda não testado no jogo.

## Rodada 12 (2026-11-09): dois logs para achar quem preenche os argumentos indiretos

No DKB Recomp (Donkey Kong Bananza, mesma base TotK; visto no hwaoi-recomp, devlog de 03/11) os registros indiretos
ficavam em `{0, 1, 0, 0, 0}` como os do G-buffer do MUA3. Duas causas lá: o reset é um draw que grava por ponteiros
(o tradutor apagava os stores) e as contagens vêm de um **geometry shader**, que o runtime não executava. O nosso
runtime também não executa geometry nem tesselação: `gfx_shader` só pede vs, ps e cs ao tradutor.

- `vk_exec_draw.c` `note_unreplayed_stages`: no `BindProgram`, loga `vk_exec: program ... has a geometry shader ...,
  which the replay skips` (também tess control/eval), os 32 primeiros programas.
- `gfx_shader.c` `note_lost_global_access`: loga `gfx_shader: <estágio> shader <hash> lost N global memory
  access(es): Store ...` para cada shader cujo `.res.json` tem a nota "Failed to find storage buffer" do
  `GlobalToStorage.cs` (loads viram 0, stores e atomics somem). Vale para os três estágios, não só compute.
- Compilam (`clang -fsyntax-only`); parser das notas testado com as duas formas de aspas do JSON. Ainda não rodado
  no jogo: a pasta `build` foi apagada e precisa ser refeita (extração, lift, build).
- Na próxima execução até o gameplay: procurar essas duas linhas no log. Um geometry shader ligado no pré-passe ou um
  store perdido num vertex shader explica o bloco vazio.

## Rodada 13 (2026-11-09): geometry shaders no replay

Cache de shaders do Ryujinx do MUA3 (`%APPDATA%\Ryujinx\games\010060700ac50000\cache\shader\guest.*`, Brotli com 1
byte de algoritmo na frente, cabeçalho SPH lido): 224 programas, 124 pixel, 76 vertex, 14 compute, **1 geometry**
(`0xe7ca1a16`, 1456 bytes), 0 tesselação, 9 entradas que não abriram. O geometry shader tem `DoesGlobalStore` no SPH,
3 `STG`, 12 `ALD`, 5 `SHFL`, saída máxima de 1 vértice e nenhuma instrução `OUT`: não desenha, só lê os vértices e
grava em memória. É o padrão do DKB (contagens dos draws indiretos vindas de um geometry shader). Referência lida: o
replay do fefw-recomp (GPL, em `..\refs\fefw-recomp`, só para comportamento; o código aqui é nosso).

- `gfx_shader.c` / `tools/xlate/Program.cs`: estágio `gs`. A topologia de entrada vem do draw
  (`GfxShaderAnswers.gs_topo`, `"topology"` no pedido) porque o SPIR-V a declara e o Vulkan exige que bata.
  `IN_VERSION` 3, `ANS_VERSION` 3 (o byte novo vai no fim do `pack_answers`, `tools/xlate_diff.py` continua lendo).
- `vk_exec_draw.c`: `prepare_draw` pede o geometry shader do programa (`NVN_STAGE_GS`) a cada draw (o memo guarda só
  vs/fs); pendente espera ou pula como os outros. Tabela de storage buffers do c0 do geometry em 0x410
  (`c0_ssbo_base`: ordem de estágios do Maxwell, 0x110 + 0x100·(estágio − 1), bate com vertex 0x110 e fragment 0x510;
  **não confirmado** num log do MUA3). Se estiver errado, aparece `a storage buffer whose address is at c0+... (stage
  2) is not replayed`. O redesenho de movimento por objeto pula draws com geometry.
- `vk_pipeline.c`: pipeline com geometry usa sempre um layout único (vertex, geometry, fragment em sequência no set 0),
  sem layouts independentes nem GPL (build monolítico; `gpl_link` recusa sozinho). O vertex ganha as saídas que o
  geometry lê e não existem, o geometry as que o fragment lê. `KEYS_VERSION` 4; pipelines com geometry ficam fora do
  arquivo de chaves (o prebuild não sabe montar o estágio).
- Tesselação continua sem replay (o MUA3 não usa no trecho do cache).
- Patches de warp do fefw-recomp (`portable-warp-*`) não entraram: só mudam algo quando o subgrupo não tem 32 lanes, e
  na NVIDIA tem.
- Compila: `clang -fsyntax-only -Wall` limpo nos 4 arquivos C, `dotnet build` do tradutor sem avisos.

**play1**: o geometry shader aparece, mas `reflection_valid` só aceitava `vs/ps/cs` e recusava o `.res.json` do `gs`
(corrigido). O SPIR-V tinha `GeometryShaderPassthroughNV` e `ShaderViewportMaskNV`: o shader é *passthrough* (SPH
GpPassthrough; por isso nenhuma instrução `OUT`), não "sem desenho". O tradutor agora responde não para passthrough e
viewport mask (`Program.cs`), e o Ryujinx escreve o laço de emissão (9 `OpEmitVertex`, `spirv-val` ok). A escrita de
ViewportMask (atributo 0x3A0) é descartada: o triângulo vai só ao viewport 0 (pendente: ligar
`VK_NV_geometry_shader_passthrough`/`VK_NV_viewport_array2` na NVIDIA). Tabela do c0 confirmada: `cb_offset` 0x410 e
0x420, nenhum store perdido.

**play2** (F7, quadro 2353, `build\upd\dump`): o geometry shader roda no **pré-passe** (passe 30, 456 draws), com os dois
SSBOs ligados exatamente no bloco dos argumentos indiretos (registros 0x139de5910, índices 0x1395e5900). É ele o
culling que preenche o bloco. **O G-buffer (passe 31) deixou de sair zerado**: t0 22550 cores distintas, t1 60504, t3
15974; personagens e cenário aparecem, mas fragmentados (muitos triângulos faltando), enquanto a profundidade do
pré-passe está completa. O shader compacta por warp: 2 `OpGroupNonUniformBallot`, 1 `OpBitCount`, 1 `OpAtomicIAdd`,
1 `OpGroupNonUniformShuffle` (o líder reserva espaço e repassa a base por shuffle). Suspeita: ballot/shuffle num
geometry shader no Vulkan não agrupa as invocações como o warp do Maxwell, ou o SHFL do Ryujinx (índice sem máscara
de 5 bits, origem inválida; é o que o `portable-warp-shuffle.patch` do fefw-recomp trata) erra a base. Próximo: ler o
GLSL/SPIR-V da compactação, conferir `supportedStages` de subgrupo para geometry, e despejar o bloco de índices.

**play3/play4** (diagnóstico): subgrupo em geometry suportado na RTX 3060 (`stages 0x3fff`, 32 fixo). Contadores zeram
a cada quadro e o pré-passe os soma (349 de 455 registros, 12453 índices, todos múltiplos de 3). O endereço do bloco
muda entre execuções (0x139de5900 na play2/3, 0x13aca7000 na play4). Índice de 32 bits nos draws do G-buffer, que
leem o bloco certo. Draw 38: slice de 240 índices (80 triângulos), contagem 6, 57 índices válidos no buffer (restos de
quadros anteriores ou escrita além do reservado). O compute que perde 3 stores (`d819a639b6163f1f`) não roda no quadro.
O VS do pré-passe passa `gl_VertexIndex` (com base vertex, como o VertexId do Maxwell no Ryujinx).

dkb-recomp (teasergames, cópia do dkb-pc-recomp; GPL por causa do Eglantine): mesmo sintoma, resolvido rodando o
geometry shader; o patch 0018 deles mostra que a ViewportMask 0 é como o passthrough descarta um triângulo.

**Passthrough nativo na NVIDIA** (`TOTK_NATIVE_GS`, padrão 1): `vk_device.c` liga `VK_NV_geometry_shader_passthrough`
e `VK_NV_viewport_array2` quando os dois existem; `gfx_shader_set_host` passa `native_gs` (entra na identidade do cache,
"nativegs") e o tradutor mantém o passthrough e a máscara. Assim o warp do geometry shader é o do Maxwell e os
triângulos com máscara 0 somem também do pré-passe. Sem as extensões continua a emulação. `vk_pipeline.c` não acrescenta
saídas a um geometry passthrough (as entradas dele são o que o fragment recebe). SPIR-V nativo validado offline.
**play5**: primeira execução com isso ("geometry passthrough native" no log). G-buffer igual (fragmentado): não era o
agrupamento dos warps. Pré-passe com profundidade completa: o jogo não zera a máscara.

**play6** (`TOTK_GS_CULL_FLIP=1`, diagnóstico em `vk_pipeline.c`, desligado por padrão: troca o único
`OpFOrdLessThanEqual` do geometry shader, o teste de costas, por `>=`): G-buffer quase vazio, só faces de costas e
lascas. O sentido do teste está certo.

**play7/play8** (`TOTK_DUMP_PASS=sh:e7387271d2011656`, os 12 draws de pele do pré-passe; `vk_dump.c` agora copia os
SSBOs do estágio geometry): base vertex 0, índices de 16 bits, slice do tamanho exato do draw. Comparando os
triângulos originais com o que o geometry shader gravou: só 4-5% contados (draw 13: 37 de 866; 14: 32 de 574; 18: 22 de
516), quase todos válidos, mas alguns misturam índices de triângulos diferentes (`(4209, 4210, 2657)`: as lascas), e há
valores depois da contagem (restos ou escrita além do reservado). Locations batem (GS lê outcodes na 2 e pixels na 1,
como o VS escreve). Barreira entre passes é completa (`barrier_to`, ALL_COMMANDS). Conclusão parcial: ou o teste de
visibilidade recebe valores diferentes do Switch, ou a compactação (ballot/shuffle-up/atomic) diverge; os triângulos
misturados apontam para a compactação.

hwaoi-recomp (Alano6678, derivado do DKB, GPL): não resolveu; terreno ainda preto, contorno grava os registros pela CPU.

**Correção (play9): o geometry shader de culling é trocado por um que lista todos os triângulos.**
`runtime/shaders/mua3_list_tris.geom` (compilado por `tools/build_shaders.py`, que agora aceita `.geom`, para
`runtime/mua3_list_tris_spv.h`): para cada triângulo, `atomicAdd(registro, 3)` e os 3 índices (o `gl_VertexIndex` que o
VS põe na location 0), mais a emissão do triângulo com a location 0 repassada ao fragment do pré-passe. Mesmos bindings da
tradução do shader do jogo (1 registro, 2 lista, 3 c0, com o desvio de alinhamento lido de c0+0x410/0x420).
`vk_exec_draw.c` `gs_list_all` reconhece o shader do jogo (estágio geometry, exatamente dois SSBOs gravados em
c0+0x410/0x420, sem texturas/imagens) e usa o nosso no lugar, só no MUA3; `TOTK_GS_LIST_ALL=0` volta ao do jogo.
Custo: o G-buffer desenha também o que o culling descartaria (o teste de profundidade EQUAL deixa os pixels iguais).

Resultado (F7, quadro 2734, `build\upd\dump_list`): **G-buffer completo** (passe 27: albedo 95885 cores distintas,
normais 507113; antes 16593 e 41273) e **quadro final com a cena inteira** pela primeira vez: hangar Kree, Drax,
Star-Lord, Groot e Rocket, HUD e diálogo. Pendente: a iluminação puxa muito para o ciano (ver se é luz, exposição ou
bloom); a causa de o shader do jogo contar só 5% continua desconhecida (contorno, não conserto).

**Ciano (play9-11).** Referência (vídeo do jogo na nave Kree): ambiente azul-petróleo escuro, personagens com cor. Na
play9 (diálogo da Gamora, câmera sobre o piso de painéis luminosos) o chão sai ciano estourado e os personagens
acinzentados; o t3 do G-buffer (luz acumulada, r11g11b10f) já tem o chão claro (máx. 4,6/20/16,5) e os personagens em
vermelho puro. `vk_dump.c` escreve "buffer texture (zero view bound)" para toda buffer texture (só um rótulo); o log
mostra que elas são ligadas. `TOTK_TEXBUF=all` (play10) não travou o carregamento; a chegada ("KREE SHIP") ficou parecida
com a referência. Com a regra padrão (play11, fala do Drax logo depois) também ficou: iluminação escura e personagens
com cor. Então `TOTK_TEXBUF` não é a causa e fica como está. Pendente: comparar o ponto da play9 com uma referência do
mesmo lugar; o vermelho puro dos personagens no t3 pode ser máscara do jogo.

## Rodada 14 (2026-11-09): crash no segundo vídeo (heap corrompido) e a ligação de símbolos

Sintoma (play9, ~206 s de jogo): access violation lendo `0xffffffffffffffff` (ponteiro não canônico) em `main+0x138d760`,
o `free` do heap geral do jogo, chamado pelo `delete[]` de `android::MPEG4Extractor::parseITunesMetaData` ao abrir
`EV_0101_02.mp4`. Os 127 `.mp4` têm os mesmos metadados (só `©too`) e o primeiro vídeo tocou, então o arquivo não era a
causa: o campo "bloco anterior" do cabeçalho do bloco estava lixo.

O alocador (objeto em `0x7003d4e7d0`, métodos virtuais em `main+0x138f930..fb00` com a trava global
`[[0x7002188460]]+0xa0`): cabeçalho de 0x20 bytes por bloco (`+0` bloco anterior, `+8` tamanho, negativo em uso,
`+0x10/+0x18` info de quem alocou ou links da lista livre); objeto: `+0x18` tamanho do cabeçalho, `+0x20` cabeças das
listas livres, `+0x28` primeiro bloco, `+0x30` fim. Heap de ~3 GB em 12 blocos (sub-heaps).

Ferramenta nova: `TOTK_HEAP_CHECK=<n>` (`runtime/heap_check.c`) percorre esse heap a cada n presents, com a trava do
jogo, e registra o primeiro cabeçalho quebrado com os blocos vizinhos e um dump. Na play12 acusou, logo depois de
`EV_0101_01.mp4`, blocos livres com links para `0x1eec0xxxx`, fora do heap (que termina em `0x1eea00000`), a mesma região
do buffer do crash.

Causa: o `new[](nothrow)` que o subsdk0 importa é o da SDK, que chama `operator new[]` pelo PLT da própria SDK; o
`loader.c` ligava esse import à cópia da própria SDK (regra "definição própria primeiro"), enquanto o `delete[]` ia para
o do jogo (`main+0x175f50`). Alocava num alocador e liberava no heap do jogo. O rtld do Horizon resolve símbolos de
visibilidade padrão pela lista global em ordem de carga (main primeiro), e só os protected/hidden localmente. O
`resolve` do `loader.c` passou a fazer o mesmo: 403 definições próprias agora vão para a cópia de um módulo anterior
(libc++ duplicada, tabelas do Opus, `operator new`). Na play13 o `new[]` da SDK aponta para `main+0x175ed0`, o heap ficou
íntegro (10800+ varreduras) e os vídeos `EV_0101_01..03` passaram sem crash.

## Rodada 15 (2026-11-09/10): sem música

- Sintoma: sons do menu tocam, música não. (As execuções play18-23 não chegaram ao menu, por isso pareciam mudas.)
- `TOTK_TRACE=audio` agora loga a cada 5 s: vozes que tocaram (formato, rota, nível), vozes em uso, os efeitos aux
  (cabeçalhos CPU/DSP, nível do retorno), mixes e memory pools por estado.
- play24: a música toca no motor (vozes 0 e 1, stream DSP-ADPCM estéreo 48 kHz, blocos de 2016 amostras, rms ~0,2)
  na mix 10, que segue pelo splitter 1. Os sons do menu (vozes 2 e 3, 28 kHz) vão pela mix 9 direto à final.
- Causa: `audio_mixer.c` `mix_depth` só seguia `dest_mix_id`; uma mix roteada por splitter ficava com profundidade 1
  e era somada no destino depois de ele já ter sido enviado adiante, ou seja, perdida. Agora segue também os
  destinos do splitter (como o Ryujinx MixContext). Confirmado em play25: a música toca.
- Também: `audren.c` `update_voice` passou a usar a posição do registro como slot (como o Ryujinx), não o campo id.
- Os 7 efeitos aux (mixes 3-9) são o reverb da Koei feito na CPU; o anel funciona.

## Rodada 16 (2026-11-10): triângulos e riscos nos efeitos

- Sintoma: estilhaços pretos/ciano em volta dos efeitos de golpe (F7, quadro 6986, `build\upd\dump`, passe 17).
- O passe 17 tem 38 draws de efeito, 15 deles com `prim 7` (QUADS). `op_draw` (DrawArrays) já convertia quads em
  triângulos; `op_draw_idx` (DrawElements) não, e os índices iam como lista de triângulos, agrupados de 3 em 3
  atravessando os quads.
- Correção: `vk_exec_draw.c` `op_draw_idx` reescreve os índices (0 1 2, 0 2 3 por quad) num buffer temporário.
  Draws indiretos com quads continuam sem conversão. **Confirmado na play26** (luta na nave Kree, gravação de 66 s):
  os efeitos de golpe saem sem estilhaços.

## Desempenho com 60 fps (2026-11-10, play24)

- Menus e cenas leves: 59-60 fps. Fases a ~48-56 fps; play22 (sem o cheat) ficava preso em 30.
- Nessas cenas a GPU não fica ociosa esperando a CPU (0 ms "waiting for the next submission"); a fila da GPU fica
  cheia: ~11,5 ms de trabalho medido + ~6,5-8,5 ms não medido por quadro. A thread de render passa ~16 ms esperando
  o `nvnSyncWait` do jogo. O "bound: cpu" do log engana: o tempo não medido (cópias) não entra na conta.
- Cópias por quadro: upload do espelho em VRAM ~28-37 MiB, write-back ~21 MiB (bindings que o shader escreve voltam
  inteiros para a memória do jogo), flush do jogo 2 x 4 MiB, 2 texturas de 4 MiB reenviadas por quadro.
- Diagnóstico novo (só com `trace = perf`): em 2 quadros a cada 1800 o log mostra `mirror upload/write-back` por
  pool e offset, `mirror flush span`, `written binding` (>= 256 KiB) e `re-upload of texture`. Próximo passo: uma
  partida numa fase, depois decidir entre (a) devolver só o que a CPU lê, (b) não reenviar texturas que só a GPU
  escreve, (c) cópias numa fila de transferência separada.

## Próximos marcos (revistos em 2026-11-10)

Base: continuar no runtime HLE (este repositório). O HWDER (`..\hwder-mua3`, GPU em baixo nível) fica parado como
referência: trava ~2 s depois do boot.

1. Desempenho nas fases (48-56 -> 60 fps): na play26 há 32 MiB de upload + 22 MiB de write-back por quadro e
   2 texturas de 4 MiB reenviadas por quadro (~0,85 ms); a thread de render passa ~15 ms no fence. Ordem: (b) não
   reenviar texturas que só a GPU escreve, (a) devolver só o que a CPU lê, (c) fila de transferência separada.
2. Iluminação ciano: comparar o ponto da play9 com uma referência do mesmo lugar.
3. Causa real do geometry shader de culling contar só 5% (hoje contornado por `mua3_list_tris.geom`); quads em
   draws indiretos.
4. Teste de cobertura pelo usuário: capítulos, vídeos, save/load, troca de personagens, DLC, dois controles.
5. Levar ao cadence-recomp as correções gerais: ligação de símbolos do `loader.c`, mix por splitter, quads indexados.
