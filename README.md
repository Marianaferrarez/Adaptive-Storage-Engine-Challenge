# Adaptive Storage Engine Challenge

Trabalho de ED2 — mecanismo de armazenamento persistente (storage engine)
chave-valor, desenvolvido em três entregas cumulativas.

## Status

- [x] Entrega 1 — Persistent Storage Engine (PUT/GET/DELETE, persistência,
      recuperação após crash, verificação de integridade)
- [ ] Entrega 2 — Indexed Storage Engine (SCAN eficiente, índice em disco)
- [ ] Entrega 3 — Adaptive Storage Engine

Detalhes de arquitetura e decisões de design: [docs/entrega1-design.md](docs/entrega1-design.md).

## Build

Requer um compilador C++17 (g++ recomendado) e `make`. Testado em Ubuntu
(WSL2).

```sh
make          # compila o binário ./engine
make test     # compila e roda os testes (persistência + recuperação de crash)
make clean    # remove binários e o diretório data/ de teste
```

## Uso do CLI

```sh
./engine init --data-dir /data
./engine run --data-dir /data --input /input/workload.jsonl --output /output/results.jsonl
./engine verify --data-dir /data
./engine describe
```

Se `--input`/`--output` forem omitidos em `run`, o engine lê de stdin e
escreve em stdout — útil para testar rapidamente:

```sh
echo '{"id": 1, "op": "put", "key": 91, "value": "abc"}' | ./engine run --data-dir /tmp/teste
```

## Estrutura do repositório

```
include/engine.hpp   interface do storage engine
src/engine.cpp        implementação (log append-only + índice em memória)
src/main.cpp           CLI (init/run/verify/describe) + parsing de JSON Lines
third_party/           dependências vendorizadas (nlohmann/json, header-only)
experiments/            testes manuais e workloads de exemplo
docs/                    documentação de design por entrega
```

## Dependências

- [nlohmann/json](https://github.com/nlohmann/json) (header-only, vendorizado
  em `third_party/`) — apenas para parsing do protocolo JSON Lines. Não
  participa do mecanismo de armazenamento em si.
