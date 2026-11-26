# Persistent Storage Engine — Notas de Design

## Arquitetura

Log estruturado append-only com índice hash em memória (modelo **Bitcask**).

```
 JSONL ──► main.cpp (protocolo, validação) ──► Engine ──► data.log (disco)
                                                 │
                                                 └─ índice em RAM: chave -> offset
```

- **PUT** grava um registro no fim de `data.log` e atualiza `índice[chave] = offset`.
- **DELETE** grava um *tombstone* (flag = 1) e remove a chave do índice.
- **GET** consulta o índice e lê **apenas aquele registro** do disco com
  `pread`, revalidando o CRC antes de devolver o valor.
- **Abertura**: o log é percorrido uma vez do início ao fim e o índice é
  reconstruído (a última ocorrência de cada chave vence; tombstone remove).
- **VERIFY** relê o arquivo inteiro: cabeçalho, CRC de cada registro, flags,
  tamanho do arquivo e compara um índice reconstruído do disco com o índice
  em memória.

Os bytes já gravados nunca são alterados: sobrescrita = novo registro no fim.
Isso torna impossível corromper um valor antigo durante a escrita de um novo.

### Por que essa estrutura

| Operação | Custo em memória secundária | Custo em RAM |
|----------|-----------------------------|--------------|
| PUT      | 1 escrita sequencial (append) | O(1) no hash |
| GET      | 1 leitura aleatória (`pread`) | O(1) no hash |
| DELETE   | 1 escrita sequencial (tombstone) | O(1) |
| Abertura | 1 leitura sequencial do log | O(chaves) |

O enunciado não exige B+ Tree/hash extensível/LSM. O log append-only é a
solução mais simples que garante persistência e sobrescrita corretas sem
nunca reescrever bytes no meio do arquivo.

### Memória

Só o índice (`unordered_map<uint64_t, uint64_t>`, ~40–60 bytes por chave
viva) fica em RAM; **os valores ficam somente em disco**. O teste
`tests/run_tests.sh` (seção 7) grava ~120 MiB de valores com o processo
limitado a 48 MiB de memória virtual (`ulimit -v`) e confere todos os GETs
depois de reiniciar.

## Formato do arquivo `data.log`

Todos os inteiros em **little-endian**.

**Cabeçalho do arquivo (16 bytes)**

| Campo    | Tamanho | Valor |
|----------|---------|-------|
| magic    | 8 bytes | `ED2KVLOG` |
| version  | 4 bytes | `1` |
| reserved | 4 bytes | `0` |

**Registro (17 + value_len bytes)**

| Campo     | Tamanho   | Descrição |
|-----------|-----------|-----------|
| crc32     | 4 bytes   | CRC32 (IEEE) de flag + key + value_len + value |
| flag      | 1 byte    | 0 = PUT, 1 = DELETE (tombstone) |
| key       | 8 bytes   | chave uint64 |
| value_len | 4 bytes   | tamanho do valor (0 em tombstones) |
| value     | value_len | bytes do valor (UTF-8 vindo do JSON) |

Outros arquivos no diretório de dados:

- `data.log.corrupted-<offset>` — criado apenas pelo comando `engine repair`;
  guarda os bytes inválidos removidos do fim do log.

## Persistência

- Cada PUT/DELETE é entregue ao kernel com `pwrite` antes de a resposta ser
  escrita, portanto sobrevive ao fim do processo (inclusive `kill -9`; veja o
  teste "SIGKILL" em `tests/run_tests.sh`).
- No encerramento normal é feito `fsync` do `data.log`.
- Não há `fsync` por operação: medimos que isso reduz o PUT de centenas de
  milhares para ~150 ops/s, e durabilidade contra queda de energia
  (crash recovery) não é requisito desta edição.
- Um `flock` exclusivo impede dois processos de abrirem o mesmo diretório.

## Integridade e tratamento de erros

| Situação | Comportamento |
|----------|---------------|
| `data.log` não existe | criado com cabeçalho |
| `data.log` com magic/versão errados | **nada é alterado**; toda operação responde `error` |
| registro com CRC inválido / truncado | índice construído até o último registro íntegro; GET da parte íntegra funciona; VERIFY = `error`; PUT/DELETE = `error` (para não gravar depois de lixo); `engine repair` move a parte ruim para quarentena |
| `value_len` corrompido gigante | detectado antes de alocar memória (limite = tamanho do arquivo) |
| linha JSON inválida, campo faltando, tipo errado, chave negativa/fracionária/> 2⁶⁴−1 | resposta `{"status":"error","message":...}`, processamento continua |
| `--data-dir` inválido ou em uso | uma resposta de erro por operação |

A decisão de **não** reparar automaticamente ao abrir é deliberada: um
reparo silencioso apagaria dados válidos que estivessem depois de um byte
corrompido no meio do arquivo. O reparo existe, mas é explícito.

## Limitações conhecidas

- **Sem compactação**: sobrescritas e remoções deixam versões antigas no log;
  o arquivo cresce com o número de escritas, não com o número de chaves vivas.
  Trabalho futuro: reescrever apenas os registros vivos num novo arquivo e
  trocá-lo atomicamente com `rename`.
- **Índice em RAM**: O(chaves vivas). Valores não ficam em memória, mas um
  número muito grande de chaves exigiria um índice em disco (B+ Tree ou
  hash extensível).
- **Abertura O(tamanho do log)**: o índice é reconstruído lendo o log todo.
  Um arquivo de *hint*/checkpoint do índice resolveria.
- Valor máximo: 4 GiB − 1 byte (campo `value_len` de 32 bits).
- Sem SCAN por intervalo (não exigido nesta edição): o índice hash não é
  ordenado; exigiria uma estrutura ordenada (B+ Tree, arquivo ordenado).
