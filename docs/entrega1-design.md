# Entrega 1 — Persistent Storage Engine — Notas de Design

## Arquitetura escolhida

Log estruturado (append-only log) com índice em memória, no estilo Bitcask.

- Todas as operações de escrita (PUT e DELETE) são gravadas apenas no final de
  um arquivo `data.log`. Nunca sobrescrevemos ou editamos bytes já escritos.
- Um DELETE grava um "tombstone": um registro marcando a chave como removida,
  em vez de apagar algo do arquivo.
- Um índice em memória (`unordered_map<uint64_t, uint64_t>`) mapeia cada chave
  para o offset do seu registro mais recente no `data.log`.
- Ao iniciar (`init`/`run`), o engine varre o `data.log` inteiro do início ao
  fim e reconstrói o índice. Esse mesmo processo serve como recuperação após
  crash: se o último registro estiver incompleto ou com checksum inválido,
  ele é descartado e a leitura para ali.

## Por que essa arquitetura

- É a forma mais simples de garantir persistência + recuperação corretas ao
  mesmo tempo, com pouquíssima chance de deixar o arquivo em estado
  inconsistente (só fazemos `append`, nunca `write` no meio do arquivo).
- PUT e DELETE são O(1) em disco (um `append`) e O(1) em memória (update no
  hash map).
- GET é O(1) esperado: uma consulta ao hash map + um `seek` + leitura do
  registro.
- É a base natural para evoluir na Entrega 2/3: esse log é essencialmente o
  "memtable + SSTable" de uma LSM Tree simplificada.

## Trade-offs conhecidos (a documentar/medir experimentalmente)

- O arquivo cresce indefinidamente com updates/deletes repetidos na mesma
  chave — vai precisar de compactação (merge dos registros válidos num novo
  arquivo, descartando tombstones e versões antigas) antes da Entrega 3, e
  possivelmente já mencionado como trabalho futuro na Entrega 1.
- O índice inteiro vive em RAM — para datasets muito maiores que a memória
  disponível, isso se torna um limite. É um ponto de discussão para a
  Entrega 2 (índice também persistido em disco).
- SCAN por intervalo de chave não é eficiente nessa estrutura (índice é hash,
  não ordenado) — por isso SCAN só é exigido a partir da Entrega 2.

## Formato do registro (`data.log`)

```
[checksum: uint32][flag: uint8][key: uint64][value_len: uint32][value: value_len bytes]
```

| Campo      | Tamanho          | Descrição                                   |
|------------|-------------------|----------------------------------------------|
| checksum   | 4 bytes           | CRC32 sobre (flag, key, value_len, value)     |
| flag       | 1 byte            | 0 = PUT, 1 = DELETE (tombstone)               |
| key        | 8 bytes           | chave uint64, little-endian                   |
| value_len  | 4 bytes           | tamanho do valor em bytes (0 se DELETE)       |
| value      | value_len bytes   | bytes do valor (ausente se DELETE)            |

Tamanho total do registro = 17 + value_len bytes.

## Verificação de integridade (`/engine verify`)

Percorre `data.log` do início ao fim revalidando o checksum de cada registro.
Reporta o primeiro offset corrompido/truncado encontrado, se houver.
