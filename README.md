# Persistent Storage Engine — ED2 2026/2

Trabalho semestral de Estrutura de Dados II (*Adaptive Storage Engine
Challenge*, entrega única): um storage engine chave-valor persistente com
chaves `uint64` e valores de tamanho variável, operações **PUT, GET, DELETE e
VERIFY** via JSON Lines.

## Integrantes

| Nome | Matrícula |
|------|-----------|
| Mariana Ribeiro | 20230001113 |
| Guilherme Ferrão | 20230024154 |
| Lethicia Silva | 20230018021 |

## Linguagem, versões e dependências

| Item | Versão |
|------|--------|
| Linguagem | C++17 |
| Compilador | g++ ≥ 9 (testado com g++ 15.2, Ubuntu / WSL2) |
| Build | GNU make ≥ 4 (opcional: sem `make`, o `run.sh` chama o g++ direto) |
| Testes | bash, Python ≥ 3.8 (só biblioteca padrão), coreutils, `flock` (util-linux) |
| Biblioteca | [nlohmann/json 3.x](https://github.com/nlohmann/json) — header-only, já incluída em `src/third_party/`. Usada **apenas** para ler/escrever o protocolo JSONL; não participa do armazenamento. |

Instalação no Ubuntu/Debian: `sudo apt install g++ make python3`.

Nenhum SGBD/storage engine pronto é usado; toda a persistência é feita pelo
grupo com `open`/`pwrite`/`pread`/`fsync` (POSIX).

## Executando (contrato da disciplina)

```sh
./run.sh --data-dir CAMINHO --input CAMINHO --output CAMINHO
```

Na primeira execução o `run.sh` **compila automaticamente** o projeto
(`make` → `build/engine`, ~10 s) e recompila sempre que algum arquivo de `src/`
mudar. Para compilar manualmente: `make`.

O script cria o `--data-dir` (e a pasta do `--output`) se necessário,
processa as operações em ordem e grava uma resposta JSON por linha.

### Validação pública

```sh
./run.sh --data-dir ./data-publica --input workloads/public/persistence-setup.jsonl --output setup-results.jsonl
python scripts/check_results.py workloads/public/persistence-setup.expected.jsonl setup-results.jsonl

./run.sh --data-dir ./data-publica --input workloads/public/persistence-restart.jsonl --output restart-results.jsonl
python scripts/check_results.py workloads/public/persistence-restart.expected.jsonl restart-results.jsonl
```

### Protocolo

```jsonc
{"id": 1, "op": "put", "key": 91, "value": "abc"}   → {"id": 1, "status": "ok"}
{"id": 2, "op": "get", "key": 91}                   → {"id": 2, "status": "ok", "value": "abc"}
{"id": 3, "op": "delete", "key": 91}                → {"id": 3, "status": "ok"}
{"id": 4, "op": "get", "key": 91}                   → {"id": 4, "status": "not_found"}
{"id": 5, "op": "delete", "key": 91}                → {"id": 5, "status": "not_found"}
{"id": 6, "op": "verify"}                           → {"id": 6, "status": "ok"}
{"id": 7, "op": "put", "key": -1, "value": "x"}     → {"id": 7, "status": "error", "message": "'key' deve ser uint64 ..."}
```

- `op` não diferencia maiúsculas/minúsculas; campos extras são ignorados.
- Qualquer falha (JSON inválido, campo ausente, tipo errado, chave fora de
  uint64, arquivo corrompido...) gera `{"id": ..., "status": "error", "message": "..."}`
  e o processamento **continua** nas linhas seguintes. Para linhas que nem
  são JSON, o `id` é recuperado do texto quando possível (senão `null`).
- Linhas em branco são ignoradas; entrada com CRLF é aceita.

### Ferramentas extras (CLI do binário)

```sh
build/engine verify --data-dir DIR   # relatório de integridade em JSON (rc=1 se houver problema)
build/engine repair --data-dir DIR   # move bytes inválidos para quarentena e trunca o log
build/engine describe                # resumo da arquitetura
```

## Testes do grupo

```sh
make test                      # suíte completa (~30 s)
QUICK=1 bash tests/run_tests.sh   # sem o dataset grande (~5 s)
make unit                      # apenas os testes unitários em C++
```

O que é testado (`tests/run_tests.sh`, ~60 verificações):

1. **Estrutura da entrega** — arquivos obrigatórios, `run.sh` executável (também no git), sem CRLF.
2. **Unitários C++** (`tests/unit_tests.cpp`, ~14 mil asserções) — PUT/GET/DELETE, sobrescrita,
   reinserção, valores binários, chaves 0 e 2⁶⁴−1, reabertura, corrupção, `repair`, lock e um teste
   diferencial de 20 000 operações aleatórias contra `std::map` reabrindo o engine a cada 2 000.
3. **Contrato do `run.sh`** — erros de uso, caminhos relativos/aninhados/com espaço, saída sobrescrita.
4. **Workloads de correção** (`tests/cases/`) — básico, sobrescrita, delete/reinserção, engine vazio,
   valores variados (vazio, UTF-8, emoji, `\u0000`, 10 KB), chaves-limite, 22 tipos de erro, protocolo,
   CRLF e 3 000 operações aleatórias.
5. **Persistência** (`tests/persistence/`) — sequências de 2 a 6 execuções no mesmo `--data-dir`,
   incluindo processo morto com `SIGKILL`.
6. **Integridade** — VERIFY detecta byte corrompido, registro truncado e cabeçalho inválido; arquivo
   estranho nunca é sobrescrito; diretório em uso gera erro.
7. **Dataset maior que a memória** — ~120 MiB de valores com o engine limitado a 48 MiB (`ulimit -v`).
8. **Validação pública** — se `workloads/public/` e `scripts/check_results.py` estiverem na raiz,
   roda exatamente os comandos do guia de submissão.

Os workloads esperados são gerados por um modelo de referência em Python
independente do código C++ (`python3 tests/gen_cases.py`). O comparador
`tests/check_results.py` segue as regras do verificador oficial e ignora apenas
o texto de `message` em respostas de erro.

## Arquitetura e formato dos arquivos

Log append-only (`DATA_DIR/data.log`) + índice hash em memória `chave → offset`
(modelo Bitcask). Valores ficam apenas em disco.

```
data.log = [cabeçalho 16 B: "ED2KVLOG" | version=1 | reserved]
           [registro]*
registro = [crc32 u32][flag u8: 0=PUT 1=DELETE][key u64][value_len u32][value]
```

Detalhes, justificativas e tabela de tratamento de erros: [docs/design.md](docs/design.md).

## Limitações conhecidas

- Sem compactação: o `data.log` cresce a cada PUT/DELETE (versões antigas permanecem no arquivo).
- O índice (não os valores) fica em RAM: ~50 bytes por chave viva.
- A abertura lê o log inteiro para reconstruir o índice (tempo proporcional ao tamanho do arquivo).
- Sem `fsync` por operação: dados sobrevivem ao fim/morte do processo, mas não necessariamente a uma
  queda de energia (crash recovery não é requisito desta edição).
- Arquivo corrompido não é reparado automaticamente: leituras continuam, escritas são recusadas até
  `build/engine repair`.
- Valor máximo de 4 GiB − 1 byte. Requer Linux/POSIX (`pread`, `flock`).

## Estrutura do repositório

```
run.sh                 contrato de execução (compila e executa)
Makefile               build/engine, build/unit_tests, make test, make zip
src/engine.hpp|cpp     storage engine (log + índice + verify/repair)
src/main.cpp           CLI e protocolo JSON Lines
src/third_party/       nlohmann/json (header-only)
tests/                 suíte de testes, workloads e geradores
docs/design.md         arquitetura e formato detalhados
experiments/           benchmark de throughput
report.pdf             relatório (2–4 páginas)
```

## Relatório

`report.pdf` é gerado a partir de [docs/report.html](docs/report.html): abra o HTML no navegador e use
*Imprimir → Salvar como PDF* (A4, sem cabeçalho/rodapé), ou no Windows:

```powershell
& "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe" --headless=new --no-pdf-header-footer `
  --print-to-pdf="$PWD\report.pdf" "file:///$($PWD -replace '\\','/')/docs/report.html"
```

## Empacotamento para o Moodle

```sh
GRUPO=07 make zip   # gera ED2_2026_2_grupo-07.zip a partir do último commit e testa o zip extraído
```
