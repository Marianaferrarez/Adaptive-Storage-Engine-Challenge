#!/usr/bin/env python3
"""Gera os workloads de teste do grupo (tests/cases e tests/persistence).

As respostas esperadas são calculadas por um MODELO DE REFERÊNCIA simples
(um dict Python em memória) que segue a especificação do protocolo. Assim os
arquivos *.expected.jsonl não dependem da implementação em C++.

Os arquivos gerados ficam versionados; este script só precisa ser rodado de
novo se algum caso for alterado:

    python3 tests/gen_cases.py
"""
import json
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))
U64_MAX = 2**64 - 1


class Model:
    """Modelo de referência do protocolo PUT/GET/DELETE/VERIFY."""

    def __init__(self):
        self.data = {}

    @staticmethod
    def _valid_key(k):
        return isinstance(k, int) and not isinstance(k, bool) and 0 <= k <= U64_MAX

    def apply(self, req):
        err = lambda i: {"id": i, "status": "error", "message": "*"}
        if not isinstance(req, dict):
            return err(None)
        if "id" not in req:
            return err(None)
        i = req["id"]
        op = req.get("op")
        if not isinstance(op, str):
            return err(i)
        op = op.lower()
        if op in ("put", "get", "delete") and not self._valid_key(req.get("key", None)):
            return err(i)
        k = req.get("key")
        if op == "put":
            if not isinstance(req.get("value"), str):
                return err(i)
            self.data[k] = req["value"]
            return {"id": i, "status": "ok"}
        if op == "get":
            if k in self.data:
                return {"id": i, "status": "ok", "value": self.data[k]}
            return {"id": i, "status": "not_found"}
        if op == "delete":
            if k in self.data:
                del self.data[k]
                return {"id": i, "status": "ok"}
            return {"id": i, "status": "not_found"}
        if op == "verify":
            return {"id": i, "status": "ok"}
        return err(i)


class Workload:
    def __init__(self, model=None):
        self.model = model or Model()
        self.lines, self.expected = [], []
        self.next_id = 1

    def op(self, **req):
        if "id" not in req:
            req = {"id": self.next_id, **req}
            self.next_id += 1
        self.lines.append(json.dumps(req, ensure_ascii=False))
        self.expected.append(self.model.apply(req))

    def raw(self, text, expected):
        """Linha de entrada literal (ex.: JSON inválido) com resposta explícita."""
        self.lines.append(text)
        self.expected.append(expected)
        self.next_id += 1

    put = lambda self, k, v: self.op(op="put", key=k, value=v)
    get = lambda self, k: self.op(op="get", key=k)
    delete = lambda self, k: self.op(op="delete", key=k)
    verify = lambda self: self.op(op="verify")

    def write(self, path_no_ext, eol="\n"):
        os.makedirs(os.path.dirname(path_no_ext), exist_ok=True)
        with open(path_no_ext + ".jsonl", "w", encoding="utf-8", newline="") as f:
            f.write("".join(line + eol for line in self.lines))
        with open(path_no_ext + ".expected.jsonl", "w", encoding="utf-8", newline="\n") as f:
            for e in self.expected:
                f.write(json.dumps(e, ensure_ascii=False) + "\n")


def case(name):
    return os.path.join(HERE, "cases", name)


# ---------------------------------------------------------------- casos simples
def basico():
    w = Workload()
    w.put(91, "abc"); w.get(91); w.delete(91); w.get(91)          # exemplo do enunciado
    for k, v in [(1, "um"), (2, "dois"), (3, "tres")]:
        w.put(k, v)
    w.get(2); w.get(1); w.get(3); w.get(4)
    w.verify()
    w.write(case("01_basico"))


def sobrescrita():
    w = Workload()
    w.put(10, "v1"); w.get(10)
    w.put(10, "valor-bem-mais-longo-que-o-anterior-" * 5); w.get(10)
    w.put(10, "x"); w.get(10)
    w.put(10, ""); w.get(10)
    w.put(10, "final"); w.put(11, "outra"); w.get(10); w.get(11)
    w.put(10, "final"); w.get(10)
    for n in range(20):
        w.put(42, f"versao-{n}")
    w.get(42)
    w.verify()
    w.write(case("02_sobrescrita"))


def delete_reinsercao():
    w = Workload()
    w.put(5, "a"); w.delete(5); w.get(5); w.delete(5)
    w.put(5, "b"); w.get(5)
    w.delete(5); w.put(5, "c"); w.put(5, "d"); w.get(5)
    w.delete(6)
    w.put(6, ""); w.delete(6); w.get(6)
    w.put(6, "volta"); w.get(6)
    for n in range(10):                       # ciclos delete/reinserção
        w.put(7, f"ciclo-{n}"); w.delete(7)
    w.get(7); w.put(7, "fim"); w.get(7)
    w.verify()
    w.write(case("03_delete_reinsercao"))


def vazio():
    w = Workload()
    w.get(1); w.delete(1); w.verify(); w.get(0); w.delete(U64_MAX); w.verify()
    w.write(case("04_engine_vazio"))


def valores():
    w = Workload()
    values = [
        "",
        "a",
        "ação, coração, pão — ñ ü ß 日本語 😀",
        'linha1\nlinha2\ttab "aspas" \\barra / barra',
        '{"id": 99, "op": "delete", "key": 1}',
        "nulo:\u0000:fim",
        "   espaços nas pontas   ",
        "\u0001\u001f controle",
        "x" * 1000,
        "".join(chr(0x41 + (i % 26)) for i in range(10_000)),
        "é" * 5000,                            # 10 000 bytes em UTF-8
    ]
    for k, v in enumerate(values, start=1):
        w.put(k, v)
    for k in range(1, len(values) + 1):        # lê depois de gravar tudo
        w.get(k)
    w.verify()
    w.write(case("05_valores_variados"))


def chaves_limite():
    w = Workload()
    keys = [0, 1, 2**31 - 1, 2**31, 2**32 - 1, 2**32, 2**53, 2**53 + 1,
            2**63 - 1, 2**63, U64_MAX - 1, U64_MAX]
    for k in keys:
        w.put(k, f"chave-{k}")
    for k in reversed(keys):
        w.get(k)
    w.delete(2**53); w.get(2**53); w.get(2**53 + 1)   # chaves vizinhas não colidem
    w.delete(U64_MAX); w.get(U64_MAX); w.get(U64_MAX - 1)
    w.verify()
    w.write(case("06_chaves_limite"))


def erros():
    w = Workload()
    w.put(1, "original")
    # Requisições inválidas: todas devem responder status=error, sem
    # derrubar o processo e sem alterar o estado.
    w.op(op="put", key=1)                                  # sem value
    w.op(op="put", value="sem chave")                      # sem key
    w.op(op="put", key=-1, value="negativa")
    w.op(op="put", key=1.5, value="fracionária")
    w.op(op="put", key="1", value="chave string")
    w.op(op="put", key=2**64, value="estoura uint64")
    w.op(op="put", key=True, value="booleano")
    w.op(op="put", key=None, value="nula")
    w.op(op="put", key=1, value=123)                       # value não-string
    w.op(op="put", key=1, value=None)
    w.op(op="put", key=1, value=["lista"])
    w.op(op="get")                                         # get sem key
    w.op(op="get", key=-5)
    w.op(op="delete", key="abc")
    w.op(op="scan_inexistente", key=1)                     # operação desconhecida
    w.op(op=42)
    w.op(key=1)                                            # sem op
    w.raw('{"id": 19, "op": "put", "key": 1, "value": "sem fechar"', {"id": 19, "status": "error", "message": "*"})
    w.raw("isto não é json", {"id": None, "status": "error", "message": "*"})
    w.raw('[1, 2, 3]', {"id": None, "status": "error", "message": "*"})
    w.raw('{"op": "get", "key": 1}', {"id": None, "status": "error", "message": "*"})  # sem id
    w.get(1)                                               # estado preservado
    w.verify()
    w.put(1, "depois dos erros"); w.get(1)
    w.write(case("07_erros"))


def protocolo():
    w = Workload()
    # ids não sequenciais / string / grandes são devolvidos como vieram
    w.op(id=1000, op="put", key=1, value="a")
    w.op(id="abc", op="get", key=1)
    w.op(id=7, op="get", key=1)
    w.op(id=2**60, op="delete", key=1)
    # op em maiúsculas é aceita (case-insensitive)
    w.op(id=3, op="PUT", key=2, value="maiusc")
    w.op(id=4, op="Get", key=2)
    w.op(id=5, op="VERIFY")
    # campos extras são ignorados; ordem das chaves não importa
    w.op(id=6, value="ordem", key=3, op="put", extra="ignorado")
    w.op(id=8, key=3, op="get")
    w.write(case("08_protocolo"))


def crlf_linhas_em_branco():
    # Entrada com fim de linha Windows (CRLF); linhas em branco são ignoradas
    # (não são operações, logo não geram resposta).
    w = Workload()
    w.put(1, "crlf"); w.get(1)
    w.lines.append("")
    w.lines.append("   ")
    w.delete(1); w.get(1); w.verify()
    w.write(case("09_crlf_linhas_em_branco"), eol="\r\n")


def aleatorio():
    # Sequência pseudoaleatória (semente fixa) de 3000 operações em 200
    # chaves, com muitas sobrescritas, remoções e reinserções.
    rnd = random.Random(2026)
    w = Workload()
    for _ in range(3000):
        k = rnd.randrange(200)
        r = rnd.random()
        if r < 0.45:
            w.put(k, "v" * rnd.randrange(0, 300) + str(rnd.random()))
        elif r < 0.75:
            w.get(k)
        elif r < 0.97:
            w.delete(k)
        else:
            w.verify()
    for k in range(200):
        w.get(k)
    w.write(case("10_aleatorio_3000_ops"))


# ----------------------------------------------- cenários de persistência
# Cada cenário é uma sequência de execuções (processos separados) sobre o
# MESMO --data-dir. O modelo é compartilhado entre as fases.
def persist(name, phases):
    model = Model()
    for i, fn in enumerate(phases, start=1):
        w = Workload(model)
        fn(w)
        w.write(os.path.join(HERE, "persistence", name, f"{i:02d}_{fn.__name__.removeprefix('p_')}"))


def p_setup(w):
    for k in range(1, 51):
        w.put(k, f"valor-{k}")
    for k in range(1, 51, 5):
        w.put(k, f"sobrescrito-{k}")
    for k in range(2, 51, 7):
        w.delete(k)
    w.put(1_000_000, "")
    w.put(2**64 - 1, "maior chave")
    w.put(999, "ação 😀 \n multi-linha")
    w.verify()


def p_restart(w):
    for k in range(1, 51):
        w.get(k)
    w.get(1_000_000); w.get(2**64 - 1); w.get(999)
    w.verify()


def p_altera(w):
    for k in range(2, 51, 7):              # reinsere as removidas
        w.put(k, f"reinserido-{k}")
    for k in range(3, 51, 4):
        w.delete(k)
    w.put(1, "nova versão")
    w.delete(1_000_000)
    w.verify()


def p_confere(w):
    for k in range(1, 51):
        w.get(k)
    w.get(1_000_000); w.get(2**64 - 1)
    w.verify()


def p_reinicio_vazio(w):
    w.verify()  # execução sem nenhuma escrita não pode alterar nada


p_confere_de_novo = p_confere


def p_pub_setup(w):
    w.put(91, "abc"); w.put(92, "def"); w.put(91, "abc2"); w.delete(92)
    w.put(93, ""); w.put(94, "x" * 5000); w.verify()


def p_pub_restart(w):
    w.get(91); w.get(92); w.get(93); w.get(94); w.put(92, "voltou"); w.get(92); w.verify()


def main():
    for fn in (basico, sobrescrita, delete_reinsercao, vazio, valores, chaves_limite,
               erros, protocolo, crlf_linhas_em_branco, aleatorio):
        fn()
    persist("A_ciclo_completo", [p_setup, p_restart, p_altera, p_confere, p_reinicio_vazio, p_confere_de_novo])
    persist("B_estilo_publico", [p_pub_setup, p_pub_restart])
    print("workloads gerados em tests/cases e tests/persistence")


if __name__ == "__main__":
    main()
