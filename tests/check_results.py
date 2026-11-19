#!/usr/bin/env python3
"""Compara um arquivo de respostas do engine com o esperado.

Uso: python tests/check_results.py ESPERADO.jsonl OBTIDO.jsonl

Regras (iguais às do contrato da disciplina):
  * o número de linhas deve ser igual (uma resposta por operação);
  * cada linha é comparada como objeto JSON (ordem das chaves não importa);
  * em respostas com "status": "error" o texto de "message" não é comparado,
    mas o campo precisa existir e ser uma string não vazia.

Imprime PASS (código de saída 0) ou FAIL com as diferenças (código 1).
"""
import json
import sys


def load(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            if not line.strip():
                continue
            try:
                rows.append((n, json.loads(line)))
            except json.JSONDecodeError as e:
                rows.append((n, {"__invalid_json__": str(e), "__raw__": line.rstrip("\n")}))
    return rows


def compare(exp, got):
    if not isinstance(got, dict):
        return "resposta não é um objeto JSON"
    if "__invalid_json__" in got:
        return "linha não é JSON válido: " + got["__invalid_json__"]
    if exp.get("status") == "error":
        msg = got.get("message")
        if got.get("status") != "error":
            return f"esperado status=error, obtido {got.get('status')!r}"
        if not isinstance(msg, str) or not msg:
            return "resposta de erro sem 'message' explícita"
        exp = {k: v for k, v in exp.items() if k != "message"}
        got = {k: v for k, v in got.items() if k != "message"}
    if exp != got:
        return f"esperado {json.dumps(exp, ensure_ascii=False)}\n          obtido   {json.dumps(got, ensure_ascii=False)}"
    return None


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    expected, actual = load(sys.argv[1]), load(sys.argv[2])
    errors = []
    if len(expected) != len(actual):
        errors.append(f"número de respostas diferente: esperado {len(expected)}, obtido {len(actual)}")
    for (ln, e), (_, a) in zip(expected, actual):
        problem = compare(e, a)
        if problem:
            errors.append(f"linha {ln} (id={e.get('id')!r}): {problem}")
    if errors:
        print("FAIL")
        for err in errors[:20]:
            print("  - " + err)
        if len(errors) > 20:
            print(f"  ... e mais {len(errors) - 20} diferenças")
        return 1
    print(f"PASS ({len(expected)} respostas)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
