#!/usr/bin/env python3
"""Gera um workload grande em duas fases (setup + restart) com respostas esperadas.

Uso: python3 tests/gen_large.py DIR_SAIDA [--keys N] [--max-value BYTES] [--seed S]

O volume total de valores gravados (~ keys * max_value / 2) é escolhido pelo
run_tests.sh para ser MAIOR que o limite de memória imposto ao engine com
`ulimit -v`, comprovando que o engine não mantém o dataset inteiro em RAM.

Os valores são derivados deterministicamente de (chave, versão, tamanho), então
o gerador também não precisa guardá-los em memória.
"""
import argparse
import json
import os
import random


def value_for(key, version, size):
    prefix = f"{key}:{version}:"
    fill = chr(ord("a") + (key + version) % 26)
    return (prefix + fill * size)[: max(size, len(prefix))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--keys", type=int, default=12000)
    ap.add_argument("--max-value", type=int, default=16384)
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()

    rnd = random.Random(a.seed)
    os.makedirs(a.out, exist_ok=True)
    state = {}  # chave -> (versão, tamanho)  — só metadados, não os valores
    keys = [rnd.randrange(2**64) for _ in range(a.keys)]
    total_bytes = 0
    nid = 1

    def emit(fin, fexp, req, resp):
        nonlocal nid
        req = {"id": nid, **req}
        resp = {"id": nid, **resp}
        nid += 1
        fin.write(json.dumps(req) + "\n")
        fexp.write(json.dumps(resp) + "\n")

    # ------------------------------------------------------------- setup
    with open(os.path.join(a.out, "large-setup.jsonl"), "w") as fin, \
         open(os.path.join(a.out, "large-setup.expected.jsonl"), "w") as fexp:
        for k in keys:
            size = rnd.randrange(0, a.max_value)
            state[k] = (0, size)
            total_bytes += size
            emit(fin, fexp, {"op": "put", "key": k, "value": value_for(k, 0, size)}, {"status": "ok"})
        # valores bem grandes
        for big in (1 << 20, 3 << 20):
            k = rnd.randrange(2**64)
            keys.append(k)
            state[k] = (0, big)
            total_bytes += big
            emit(fin, fexp, {"op": "put", "key": k, "value": value_for(k, 0, big)}, {"status": "ok"})
        # sobrescritas de ~25% das chaves
        for k in rnd.sample(keys, len(keys) // 4):
            ver, _ = state[k]
            size = rnd.randrange(0, a.max_value)
            state[k] = (ver + 1, size)
            total_bytes += size
            emit(fin, fexp, {"op": "put", "key": k, "value": value_for(k, ver + 1, size)}, {"status": "ok"})
        # remoções de ~10%
        for k in rnd.sample(keys, len(keys) // 10):
            emit(fin, fexp, {"op": "delete", "key": k}, {"status": "ok"})
            del state[k]
        # leitura de uma amostra ainda na mesma execução
        for k in rnd.sample(keys, 500):
            if k in state:
                v, s = state[k]
                emit(fin, fexp, {"op": "get", "key": k}, {"status": "ok", "value": value_for(k, v, s)})
            else:
                emit(fin, fexp, {"op": "get", "key": k}, {"status": "not_found"})
        emit(fin, fexp, {"op": "verify"}, {"status": "ok"})

    # ----------------------------------------------------------- restart
    with open(os.path.join(a.out, "large-restart.jsonl"), "w") as fin, \
         open(os.path.join(a.out, "large-restart.expected.jsonl"), "w") as fexp:
        order = keys[:]
        rnd.shuffle(order)
        for k in order:
            if k in state:
                v, s = state[k]
                emit(fin, fexp, {"op": "get", "key": k}, {"status": "ok", "value": value_for(k, v, s)})
            else:
                emit(fin, fexp, {"op": "get", "key": k}, {"status": "not_found"})
        emit(fin, fexp, {"op": "verify"}, {"status": "ok"})

    print(f"{len(keys)} chaves, ~{total_bytes / 2**20:.0f} MiB de valores gravados")


if __name__ == "__main__":
    main()
