# Modelo de memória do PC (2): guia para testes

Este guia explica a parte da memória que estamos trocando no port, o que já foi feito, o estado de cada
chave e como testar. Tudo aqui é **experimental e está desligado por padrão**. Nada muda no jogo sem as
chaves indicadas. O plano técnico completo, com endereços, tabelas e a seção de chaves, está em
[PC-MEMORY-REWRITE.md](../re-stutters/PC-MEMORY-REWRITE.md) (seção 8: estado e chaves).

## O que é o modelo UMA do PS4

O PS4 tem uma memória só, compartilhada entre a CPU e a GPU (UMA, *unified memory*). O jogo escreve
os dados e a GPU lê do mesmo lugar, sem cópia. No PC a GPU tem a sua própria memória (VRAM), e a RAM
fica separada. Para não quebrar o jogo, o port faz de conta que a memória é única:

- toda página que a GPU pode ler é marcada como protegida, para o port saber quando o jogo muda algo;
- o jogo escreve numa página protegida e o port copia o conteúdo para a VRAM na hora certa;
- o que a GPU escreve e o jogo lê volta por **leitura da GPU**, que espera a GPU terminar.

Essas proteções e esperas são a maior parte das travadas ao entrar em áreas novas.

## O que muda no modelo do PC

A ideia é que o port conheça o **dono** de cada bloco de memória e a **classe** dela:

| Classe | O que é | Exemplo |
|---|---|---|
| CPU | só a CPU escreve e lê | estado do jogo, física, dados de carga |
| GPU estática | a CPU escreve uma vez e a GPU só lê | malhas, texturas, buffers de vértices |
| GPU dinâmica | a CPU escreve a cada quadro e a GPU lê | constantes da câmera, instâncias, descritores |
| GPU escrita | a GPU escreve e a CPU lê de volta | máscaras de visibilidade (XPR), estatísticas de partículas |

Com as classes, a RAM guarda o que a CPU escreve, a VRAM guarda o que a GPU lê e escreve, e só o que
precisa de ida e volta fica nos dois lugares.

## Estado das chaves

Resumo. Os valores e os riscos completos estão no CHANGELOG (seção "Modelo de memória do PC") e na seção 8
do plano técnico.

| Chave | Estado | Resumo |
|---|---|---|
| `SOTC_PC_RING=1` | experimental | libera a memória de quadro da GPU na alocação. Faltas de escrita caem 24–60%; FPS cai 1–4 em média |
| `SOTC_PC_RING=2` | diagnóstico | só confere, sem mudar nada |
| `SOTC_PC_RING=3`, `=5` | não seguro | anel completo sem verificação (`=3`) ou página quente (`=5`) |
| `SOTC_PC_RING=4` | diagnóstico | anel completo com verificação; lento e ainda com bytes possivelmente desatualizados |
| `SOTC_STATIC_CLASS=1` | experimental | malhas e texturas da GPU não são protegidas na alocação; `=2` só conta |
| `SOTC_RT_CLASS=1` | experimental | alvos de renderização não são copiados de volta nem rastreados; `=2` só conta |
| `SOTC_RB_CLASS=1` | experimental | cópia antecipada das leituras de máscara e estatística; leituras que esperam a GPU caem ~4% (568 contra 593 no mesmo lote) |
| `SOTC_RB_CLASS=2` | **não usar** | serve leituras pela última cópia; produziu vértices estourados (correção em andamento) |
| `SOTC_PC_POOL=2` | verificação | confere o pool do jogo com um modelo, sem mudar nada; ~98% de acerto |
| `SOTC_PC_POOL_NATIVE=1` | experimental | o port executa o caminho exato de alocação (27 de 27 conferidas na rota) |
| `SOTC_COMMIT_ON_DEMAND=1` | experimental | a memória de backing só é comprometida quando usada |
| `SOTC_COMMIT_CHUNKS=2` | experimental | comprometimento no primeiro acesso: 9,6 GB viram 6,0–7,2 GB; `=1` não economiza |
| `SOTC_STALE_CHECK=N` | diagnóstico | confere dados de vértices e índices velhos; 0 páginas velhas nas execuções válidas |
| `SOTC_CLASS_TRACK=2` + `SOTC_MEMTYPE_CLASSES=1` | diagnóstico | classes por heap em modo sombra; nada é desprotegido ainda |

Nenhuma chave muda o quadro hoje: a thread de desenho fica no limite (~34,5 ms de 33 ms). O objetivo das
chaves é reduzir o trabalho dos outros threads e preparar as próximas fases.

## O que foi medido

Medições de 1 de novembro, rota de testes do Lab (`user/routes/tests.json`). Os lotes estão em
`user/pcmem/<data>/summary.md`.

- **Primeira medição** (cerca de 2 minutos de jogo): 9 donos de bloco somam 3,6 GiB. Os heaps por quadro
  da GPU causam quase metade das faltas de escrita (48%).
- **Lote `-032958`** (oito configurações, duas rodadas): FPS base 27,2; anel 23,4 (a segunda rodada caiu
  para 20,3, com a thread de desenho em 120 ms); classes estáticas 28,3; render targets 27,8; leituras 27,8
  (545 leituras por segundo, contra 742 do base); commit por blocos 28,4 (6.720 MiB comprometidos); pool
  nativo 27,2. Diferença entre as duas rodadas do base: 26,2 a 28,1. Ou seja, só o anel se destaca, e
  para baixo.
- **Lote `-032443`** (verify: todas as chaves de conferência): 6.022 de 6.173 alocações do pool conferidas; anel
  com 58 MB divergentes em 167 binds; FPS 23,1.
- **Lote `-032624`** (seguro: anel 1, pool nativo, commit 2, leituras 2 e classes): FPS 26,7; 27 alocações
  nativas, todas conferidas; sem falhas.
- **Lote `-040243`**: leituras com classe 1 (568 por segundo) contra o heurístico (593) e a classe 2 (527,
  com vértices estourados em rodadas anteriores).
- **Detector de páginas velhas** (`-035931`): 0 páginas velhas em até cerca de 509 mil páginas comparadas.

## Como testar

Use sempre a rota de testes do repositório (`user/routes/tests.json`) para comparar execuções. Compare
configurações **no mesmo lote**, porque o FPS da máquina varia entre lotes. Só vale uma rodada cujo log
tenha `SOTC LAB ROUTE done:`.

Antes de tudo, confira se os ganchos instalam (o carregador recusa alguns ao carregar o jogo):

```
python tools/re/hooks_install_check.py
```

Para comparar várias configurações, use o orquestrador, que roda cada uma com as outras, em rodadas
alternadas:

```
python tools/pcmem_matrix.py --dry-run --configs base,ring,static,rt,rb2,chunks2
python tools/pcmem_matrix.py --configs base,ring,static,rt,rb2,chunks2 --repeat 2
python tools/pcmem_matrix.py --configs base,verify --env SOTC_STALE_CHECK=16
python tools/pcmem_matrix.py --parse-only user/pcmem/<data>/logs/*.log
```

- `--dry-run` mostra o plano sem abrir o jogo.
- Sem `--configs`, roda o conjunto padrão (`base, verify, ring5, chunks2, heaps, rb2, poolnative, safe`).
- `--env KEY=VALUE` aplica uma variável a todas as configurações.
- `--parse-only` refaz o resumo a partir de logs já gravados.
- As configurações estão em `tools/pcmem_matrix.py` (`SWITCHES`). Cada uma tem o conjunto exato de chaves.

Para um A/B direto, sem o orquestrador (anel contra o padrão):

```
SOTC_LAB=1 SOTC_LAB_ROUTE=user/routes/tests.json SOTC_FPS=1 SOTC_PC_RING=1 SOTC_HOOK_ALLOC_PROBE=1 SOTC_HOOK_FRAME_FLIP=1
```

Sem `SOTC_PC_RING=1` é a linha de referência. O mesmo pode ser feito pela aba **Ajustes
experimentais** do lançador, mudando uma opção por vez.

Não ligue `SOTC_HOOK_MEMTYPE_PROBE` junto com `SOTC_PC_RING`: os dois usam o mesmo ponto de gancho.

## Riscos

- **Final do jogo ainda não testado.** O critério para o modelo virar padrão é o teste de final do
  usuário, feito com todas as chaves ligadas. Até lá, nenhum padrão muda.
- **Queda 0x49500 (final do jogo).** Mudanças que alterem onde os heaps do jogo ficam podem reabrir essa
  queda. Antes de virar padrão, cada fase precisa passar no teste do final. Se o jogo cair no final com estas
  chaves ligadas, desligue-as e registre o log.
- **Caminho de pool sem blocos livres (`POOL_FIX`, eboot 0x1a321).** Com o bitmap cheio e a lista de endereços
  virtuais livres vazia, o caminho do jogo lê o endereço 0x58. O caminho nativo só age no acerto exato; os
  outros casos voltam ao jogo. Um pool completo precisa passar para o próximo conjunto nesse caso.
- **Custo de CPU.** O gancho de alocação roda dezenas de milhares de vezes por segundo nas threads do jogo.
  Medir sempre com `SOTC_FPS=1`, e nunca junto com outros probes sem necessidade.

## Problemas conhecidos

- **Congelamento de 10 s**, visto uma vez com o anel ligado. Não se repetiu e não foi explicado.
- **Vértices estourados com `SOTC_RB_CLASS=2`.** Por isso a chave fica desligada e não deve ser usada.
- **Queda antiga com `SOTC_DROP_FAST`** (liberação de memória sem espera, detalhes no CHANGELOG). Não é parte
  deste modelo, mas continua desligada por padrão.
- **Lotes com `SOTC_STALE_CHECK=16` que não terminaram** (`-035053`, `-035215`): quadros de 600–800 ms, sem
  saída da rota; a causa não foi determinada. Na primeira, apareceram linhas "stale page" durante a execução
  travada. Não se repetiram nas execuções válidas, mas ainda precisam de investigação.
- **Lotes a cerca de 13 FPS** (`-035657`, `-035931`): todas as configurações desses lotes rodaram lentas,
  inclusive o base. Não compare com os lotes de 27–28 FPS.

## O que vem depois

1. Anel: trocar o gancho amplo por outros mais estreitos (o alocador de descritores 0x8c0d0 ou o refil do
   heap), declarar só as páginas que a alocação usa e, por fim, um anel real: páginas nunca protegidas,
   enviadas uma vez por envio. A variante segura prevista é proteger e copiar a cada envio.
2. Leituras: corrigir o `=2` (vértices estourados), com limite de 4 quadros na janela de máscaras (com 2, as
   leituras chegam "antigas" e são recusadas).
3. Classes: medir `SOTC_CLASS_TRACK=1` (aplicar) e as classes por heap numa matriz, depois de checar o estado
   com o detector de páginas velhas.
4. Pool: estender o caminho nativo além do acerto exato, começando pelo caminho de nó (770 de 770 conferidas
   na verificação).
5. Commit: medir o FPS do commit sozinho (até agora só o tamanho foi medido).
6. Quando todas as fases estiverem medidas e o teste do final passar, os padrões mudam juntos.

Para o detalhe de cada resultado, veja [PC-MEMORY-REWRITE.md](../re-stutters/PC-MEMORY-REWRITE.md).
