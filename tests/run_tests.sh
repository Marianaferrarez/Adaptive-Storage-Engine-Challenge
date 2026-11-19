#!/usr/bin/env bash
# Suíte de testes do grupo. Uso:
#   bash tests/run_tests.sh          # tudo (inclui dataset grande, ~1 min)
#   QUICK=1 bash tests/run_tests.sh  # pula o dataset grande
#
# Camadas:
#   1. estrutura da entrega (run.sh executável, LF, arquivos obrigatórios)
#   2. testes unitários em C++ (tests/unit_tests.cpp)
#   3. contrato do run.sh (argumentos, caminhos relativos/com espaço, saída)
#   4. workloads de correção (tests/cases/*.jsonl)
#   5. persistência entre execuções (tests/persistence/*/)
#   6. integridade: VERIFY detecta corrupção, arquivo estranho, lock, repair
#   7. dataset maior que a memória permitida ao engine (ulimit -v)
#   8. validação pública do professor (se workloads/public existir)
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN="$ROOT/run.sh"
ENGINE="$ROOT/build/engine"
PY="$(command -v python3 || command -v python)"
CHECK="$PY $ROOT/tests/check_results.py"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/ed2-tests-XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0
FAILED=()
G=$'\e[32m' R=$'\e[31m' B=$'\e[1m' N=$'\e[0m'
[ -t 1 ] || { G="" R="" B="" N=""; }

ok()   { PASS=$((PASS + 1)); echo "  ${G}PASS${N} $1"; }
fail() { FAIL=$((FAIL + 1)); FAILED+=("$1"); echo "  ${R}FAIL${N} $1"; [ -n "${2:-}" ] && echo "$2" | sed 's/^/       /'; }
section() { echo; echo "${B}== $1 ==${N}"; }

# Roda run.sh e compara com o esperado.
run_and_check() {  # nome data_dir input expected
    local name="$1" dir="$2" input="$3" expected="$4" out="$TMP/out-$RANDOM-$RANDOM.jsonl" log
    if ! log="$("$RUN" --data-dir "$dir" --input "$input" --output "$out" 2>&1)"; then
        fail "$name" "run.sh terminou com erro: $log"
        return
    fi
    if log="$($CHECK "$expected" "$out")"; then ok "$name"; else fail "$name" "$log"; fi
}

count_lines() { grep -c . "$1" 2>/dev/null || echo 0; }

# ------------------------------------------------------------------------
section "1. Estrutura da entrega"
cd "$ROOT"
for f in README.md run.sh src tests; do
    if [ -e "$f" ]; then ok "existe $f"; else fail "existe $f"; fi
done
if [ -e report.pdf ]; then ok "existe report.pdf"; else fail "existe report.pdf" "gere o relatório antes de empacotar"; fi
[ -x run.sh ] && ok "run.sh é executável" || fail "run.sh é executável" "rode: chmod +x run.sh && git update-index --chmod=+x run.sh"
if command -v git >/dev/null && git -C "$ROOT" rev-parse >/dev/null 2>&1; then
    mode="$(git -C "$ROOT" ls-files -s run.sh | cut -d' ' -f1)"
    [ "$mode" = "100755" ] && ok "run.sh marcado como executável no git" || fail "run.sh marcado como executável no git" "modo atual: $mode"
fi
[ "$(head -c 2 run.sh)" = "#!" ] && ok "run.sh tem shebang" || fail "run.sh tem shebang"
crlf="$(grep -rlI $'\r' run.sh Makefile src tests/*.sh tests/*.py 2>/dev/null || true)"
[ -z "$crlf" ] && ok "sem quebras de linha CRLF nos scripts/fontes" || fail "sem quebras de linha CRLF" "$crlf"

# ------------------------------------------------------------------------
section "2. Testes unitários (C++)"
if make -s -C "$ROOT" build/engine build/unit_tests >/dev/null 2>"$TMP/build.log"; then
    ok "compilação sem erros"
    warnings="$(grep -c warning "$TMP/build.log" || true)"
    [ "$warnings" = "0" ] && ok "compilação sem warnings" || fail "compilação sem warnings" "$(cat "$TMP/build.log")"
    if out="$("$ROOT/build/unit_tests" 2>&1)"; then ok "unit_tests ($(echo "$out" | tail -1))"; else fail "unit_tests" "$out"; fi
else
    fail "compilação" "$(cat "$TMP/build.log")"
    echo "não é possível continuar sem o binário"; exit 1
fi

# ------------------------------------------------------------------------
section "3. Contrato do run.sh"
printf '{"id": 1, "op": "put", "key": 1, "value": "a"}\n{"id": 2, "op": "get", "key": 1}\n' > "$TMP/mini.jsonl"
printf '{"id":1,"status":"ok"}\n{"id":2,"status":"ok","value":"a"}\n' > "$TMP/mini.expected.jsonl"

"$RUN" >/dev/null 2>&1; rc=$?
[ $rc -ne 0 ] && ok "sem argumentos -> erro de uso (rc=$rc)" || fail "sem argumentos -> erro de uso"
"$RUN" --data-dir "$TMP/x" --output "$TMP/x.out" >/dev/null 2>&1; rc=$?
[ $rc -ne 0 ] && ok "sem --input -> erro (rc=$rc)" || fail "sem --input -> erro"
msg="$("$RUN" --data-dir "$TMP/x" --input "$TMP/nao-existe.jsonl" --output "$TMP/x.out" 2>&1)"; rc=$?
[ $rc -ne 0 ] && [ -n "$msg" ] && ok "--input inexistente -> mensagem clara: $msg" || fail "--input inexistente -> erro"

run_and_check "cria --data-dir aninhado inexistente" "$TMP/novo/a/b/c" "$TMP/mini.jsonl" "$TMP/mini.expected.jsonl"
[ -f "$TMP/novo/a/b/c/data.log" ] && ok "data.log criado dentro do --data-dir" || fail "data.log criado dentro do --data-dir"

run_and_check "caminhos com espaços" "$TMP/dir com espaço" "$TMP/mini.jsonl" "$TMP/mini.expected.jsonl"

( cd "$TMP" && mkdir -p rel && cp mini.jsonl rel/in.jsonl && \
  "$RUN" --data-dir rel/dados --input rel/in.jsonl --output rel/out.jsonl ) >/dev/null 2>&1
if [ -f "$TMP/rel/dados/data.log" ] && $CHECK "$TMP/mini.expected.jsonl" "$TMP/rel/out.jsonl" >/dev/null; then
    ok "caminhos relativos ao diretório atual (executado de outra pasta)"
else
    fail "caminhos relativos ao diretório atual"
fi

"$RUN" --data-dir="$TMP/eq" --input="$TMP/mini.jsonl" --output="$TMP/eq.out" 2>/dev/null
$CHECK "$TMP/mini.expected.jsonl" "$TMP/eq.out" >/dev/null 2>&1 && ok "sintaxe --flag=valor" || fail "sintaxe --flag=valor"

printf 'lixo antigo\nlixo\nlixo\nlixo\n' > "$TMP/pre.jsonl"
"$RUN" --data-dir "$TMP/pre" --input "$TMP/mini.jsonl" --output "$TMP/pre.jsonl" 2>/dev/null
$CHECK "$TMP/mini.expected.jsonl" "$TMP/pre.jsonl" >/dev/null && ok "--output existente é sobrescrito" || fail "--output existente é sobrescrito"

"$RUN" --data-dir "$TMP/sub" --input "$TMP/mini.jsonl" --output "$TMP/sub/dir/out.jsonl" 2>/dev/null
[ -f "$TMP/sub/dir/out.jsonl" ] && ok "cria diretório do --output se faltar" || fail "cria diretório do --output se faltar"

: > "$TMP/empty.jsonl"
"$RUN" --data-dir "$TMP/empty" --input "$TMP/empty.jsonl" --output "$TMP/empty.out" 2>/dev/null; rc=$?
[ $rc -eq 0 ] && [ -f "$TMP/empty.out" ] && [ ! -s "$TMP/empty.out" ] && ok "entrada vazia -> saída vazia" || fail "entrada vazia -> saída vazia"

printf '{"id": 1, "op": "put", "key": 1, "value": "a"}' > "$TMP/noeol.jsonl"   # sem \n final
"$RUN" --data-dir "$TMP/noeol" --input "$TMP/noeol.jsonl" --output "$TMP/noeol.out" 2>/dev/null
[ "$(count_lines "$TMP/noeol.out")" = "1" ] && ok "última linha sem \\n é processada" || fail "última linha sem \\n é processada"

# ------------------------------------------------------------------------
section "4. Workloads de correção (tests/cases)"
for input in "$ROOT"/tests/cases/*.jsonl; do
    case "$input" in *.expected.jsonl) continue ;; esac
    name="$(basename "$input" .jsonl)"
    expected="${input%.jsonl}.expected.jsonl"
    run_and_check "$name" "$TMP/case-$name" "$input" "$expected"
done

# Mesma entrada em dois diretórios novos deve produzir saídas idênticas.
"$RUN" --data-dir "$TMP/det1" --input "$ROOT/tests/cases/10_aleatorio_3000_ops.jsonl" --output "$TMP/det1.out" 2>/dev/null
"$RUN" --data-dir "$TMP/det2" --input "$ROOT/tests/cases/10_aleatorio_3000_ops.jsonl" --output "$TMP/det2.out" 2>/dev/null
cmp -s "$TMP/det1.out" "$TMP/det2.out" && ok "execução determinística" || fail "execução determinística"
cmp -s "$TMP/det1/data.log" "$TMP/det2/data.log" && ok "data.log determinístico (mesmos bytes)" || fail "data.log determinístico"

# ------------------------------------------------------------------------
section "5. Persistência entre execuções (mesmo --data-dir, processos novos)"
for scen in "$ROOT"/tests/persistence/*/; do
    sname="$(basename "$scen")"
    dir="$TMP/persist-$sname"
    for input in "$scen"*.jsonl; do
        case "$input" in *.expected.jsonl) continue ;; esac
        run_and_check "$sname / $(basename "$input" .jsonl)" "$dir" "$input" "${input%.jsonl}.expected.jsonl"
    done
done

# Processo morto com SIGKILL (sem encerramento normal) depois de PUTs já
# processados: os dados anteriores e os novos PUTs continuam lá.
"$RUN" --data-dir "$TMP/kill" --input "$ROOT/tests/persistence/A_ciclo_completo/01_setup.jsonl" --output "$TMP/kill1.out" 2>/dev/null
{ printf '{"id": 1, "op": "put", "key": 5000, "value": "antes do kill"}\n{"id": 2, "op": "put", "key": 5001, "value": "x"}\n{"id": 3, "op": "delete", "key": 5001}\n'; sleep 10; } \
    | "$ENGINE" run --data-dir "$TMP/kill" >/dev/null 2>&1 &
pid=$!
sleep 0.5; kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
run_and_check "dados anteriores intactos após SIGKILL" "$TMP/kill" \
    "$ROOT/tests/persistence/A_ciclo_completo/02_restart.jsonl" "$ROOT/tests/persistence/A_ciclo_completo/02_restart.expected.jsonl"
printf '{"id": 1, "op": "get", "key": 5000}\n{"id": 2, "op": "get", "key": 5001}\n{"id": 3, "op": "verify"}\n' > "$TMP/k.jsonl"
printf '{"id":1,"status":"ok","value":"antes do kill"}\n{"id":2,"status":"not_found"}\n{"id":3,"status":"ok"}\n' > "$TMP/k.expected.jsonl"
run_and_check "escritas feitas antes do SIGKILL persistem" "$TMP/kill" "$TMP/k.jsonl" "$TMP/k.expected.jsonl"

# ------------------------------------------------------------------------
section "6. Integridade (VERIFY) e arquivos corrompidos"
D="$TMP/corrupt"
"$RUN" --data-dir "$D" --input "$ROOT/tests/cases/01_basico.jsonl" --output "$TMP/c0.out" 2>/dev/null
"$ENGINE" verify --data-dir "$D" >/dev/null && ok "engine verify: arquivo íntegro -> ok" || fail "engine verify: arquivo íntegro"

# corrompe 1 byte no meio do último registro
size=$(stat -c %s "$D/data.log")
printf '\xAA' | dd of="$D/data.log" bs=1 seek=$((size - 2)) conv=notrunc 2>/dev/null
sum_before="$(sha256sum "$D/data.log")"
cat > "$TMP/c1.jsonl" <<'EOF'
{"id": 1, "op": "verify"}
{"id": 2, "op": "get", "key": 1}
{"id": 3, "op": "put", "key": 50, "value": "não deve gravar"}
{"id": 4, "op": "delete", "key": 1}
EOF
"$RUN" --data-dir "$D" --input "$TMP/c1.jsonl" --output "$TMP/c1.out" 2>/dev/null
st() { "$PY" -c "import json,sys; print([json.loads(l)['status'] for l in open(sys.argv[1])])" "$1"; }
[ "$(st "$TMP/c1.out")" = "['error', 'ok', 'error', 'error']" ] \
    && ok "byte corrompido: VERIFY=error, leitura da parte íntegra ok, escritas recusadas" \
    || fail "byte corrompido" "$(cat "$TMP/c1.out")"
[ "$(sha256sum "$D/data.log")" = "$sum_before" ] && ok "arquivo corrompido não foi modificado" || fail "arquivo corrompido não foi modificado"
"$ENGINE" verify --data-dir "$D" >/dev/null 2>&1 && fail "engine verify detecta corrupção" || ok "engine verify detecta corrupção (rc!=0)"
"$ENGINE" repair --data-dir "$D" >/dev/null 2>&1 && ok "engine repair" || fail "engine repair"
ls "$D"/data.log.corrupted-* >/dev/null 2>&1 && ok "bytes inválidos movidos para quarentena" || fail "quarentena criada"
printf '{"id": 1, "op": "verify"}\n{"id": 2, "op": "put", "key": 50, "value": "v"}\n{"id": 3, "op": "get", "key": 50}\n' > "$TMP/c2.jsonl"
printf '{"id":1,"status":"ok"}\n{"id":2,"status":"ok"}\n{"id":3,"status":"ok","value":"v"}\n' > "$TMP/c2.expected.jsonl"
run_and_check "após repair: VERIFY ok e escrita volta a funcionar" "$D" "$TMP/c2.jsonl" "$TMP/c2.expected.jsonl"

# cauda truncada (escrita interrompida)
D="$TMP/trunc"
"$RUN" --data-dir "$D" --input "$ROOT/tests/cases/02_sobrescrita.jsonl" --output /dev/null 2>/dev/null
truncate -s -5 "$D/data.log"
printf '{"id": 1, "op": "verify"}\n' > "$TMP/v.jsonl"
"$RUN" --data-dir "$D" --input "$TMP/v.jsonl" --output "$TMP/t.out" 2>/dev/null
grep -q '"status":"error"' "$TMP/t.out" && ok "registro truncado: VERIFY=error" || fail "registro truncado: VERIFY=error" "$(cat "$TMP/t.out")"

# cabeçalho do arquivo alterado
D="$TMP/magic"
"$RUN" --data-dir "$D" --input "$TMP/mini.jsonl" --output /dev/null 2>/dev/null
printf 'XX' | dd of="$D/data.log" bs=1 seek=0 conv=notrunc 2>/dev/null
"$RUN" --data-dir "$D" --input "$TMP/mini.jsonl" --output "$TMP/m.out" 2>/dev/null
[ "$(st "$TMP/m.out")" = "['error', 'error']" ] && ok "cabeçalho inválido: toda operação responde error" || fail "cabeçalho inválido" "$(cat "$TMP/m.out")"

# arquivo que não é do engine
D="$TMP/foreign"; mkdir -p "$D"; echo "planilha qualquer" > "$D/data.log"
sum_before="$(sha256sum "$D/data.log")"
"$RUN" --data-dir "$D" --input "$TMP/mini.jsonl" --output "$TMP/f.out" 2>/dev/null; rc=$?
[ "$(count_lines "$TMP/f.out")" = "2" ] && [ "$(st "$TMP/f.out")" = "['error', 'error']" ] \
    && ok "data.log estranho: uma resposta de erro por operação (rc=$rc)" || fail "data.log estranho" "$(cat "$TMP/f.out")"
[ "$(sha256sum "$D/data.log")" = "$sum_before" ] && ok "data.log estranho não foi sobrescrito" || fail "data.log estranho não foi sobrescrito"

# --data-dir que é um arquivo comum
echo x > "$TMP/arquivo-comum"
"$RUN" --data-dir "$TMP/arquivo-comum" --input "$TMP/mini.jsonl" --output "$TMP/af.out" 2>/dev/null
[ "$(st "$TMP/af.out")" = "['error', 'error']" ] && ok "--data-dir que é arquivo: erro explícito" || fail "--data-dir que é arquivo" "$(cat "$TMP/af.out" 2>/dev/null)"

# dois processos no mesmo diretório
if command -v flock >/dev/null; then
    D="$TMP/locked"
    "$RUN" --data-dir "$D" --input "$TMP/mini.jsonl" --output /dev/null 2>/dev/null
    flock "$D/data.log" sleep 3 & lpid=$!
    sleep 0.3
    "$RUN" --data-dir "$D" --input "$TMP/mini.jsonl" --output "$TMP/l.out" 2>/dev/null
    [ "$(st "$TMP/l.out")" = "['error', 'error']" ] && ok "diretório em uso por outro processo: erro explícito" || fail "lock" "$(cat "$TMP/l.out")"
    wait "$lpid" 2>/dev/null
fi

# ------------------------------------------------------------------------
section "7. Dataset maior que a memória disponível ao engine"
if [ "${QUICK:-0}" = "1" ]; then
    echo "  (pulado: QUICK=1)"
else
    LIMIT_MB=${LIMIT_MB:-48}
    "$PY" "$ROOT/tests/gen_large.py" "$TMP/large" --keys 12000 --max-value 16384 | sed 's/^/  /'
    D="$TMP/large-data"
    for phase in setup restart; do
        start=$EPOCHREALTIME
        ( ulimit -v $((LIMIT_MB * 1024)); exec "$ENGINE" run --data-dir "$D" \
            --input "$TMP/large/large-$phase.jsonl" --output "$TMP/large-$phase.out" ) 2>"$TMP/large.err"
        rc=$?
        secs=$(awk -v a="$start" -v b="$EPOCHREALTIME" 'BEGIN { printf "%.2f", b - a }')
        if [ $rc -ne 0 ]; then
            fail "large-$phase com ulimit -v ${LIMIT_MB}MiB" "rc=$rc $(head -3 "$TMP/large.err")"
        elif res="$($CHECK "$TMP/large/large-$phase.expected.jsonl" "$TMP/large-$phase.out")"; then
            ok "large-$phase com memória limitada a ${LIMIT_MB} MiB: $res em ${secs}s"
        else
            fail "large-$phase" "$res"
        fi
    done
    echo "  data.log: $(du -h "$D/data.log" | cut -f1) em disco (> ${LIMIT_MB} MiB de memória permitida)"
fi

# ------------------------------------------------------------------------
section "8. Validação pública (guia de submissão)"
if [ -d "$ROOT/workloads/public" ] && [ -f "$ROOT/scripts/check_results.py" ]; then
    ( cd "$TMP" && rm -rf data-publica && \
      W="$ROOT/workloads/public" && \
      "$RUN" --data-dir ./data-publica --input "$W/persistence-setup.jsonl" --output setup-results.jsonl && \
      "$PY" "$ROOT/scripts/check_results.py" "$W/persistence-setup.expected.jsonl" setup-results.jsonl && \
      "$RUN" --data-dir ./data-publica --input "$W/persistence-restart.jsonl" --output restart-results.jsonl && \
      "$PY" "$ROOT/scripts/check_results.py" "$W/persistence-restart.expected.jsonl" restart-results.jsonl ) \
      > "$TMP/pub.log" 2>&1
    if [ "$(grep -c PASS "$TMP/pub.log")" -ge 2 ]; then ok "validação pública: 2x PASS"; else fail "validação pública" "$(cat "$TMP/pub.log")"; fi
else
    echo "  (workloads/public/ e scripts/check_results.py do professor não estão no repositório;"
    echo "   copie-os para a raiz para rodar a validação oficial — os cenários em"
    echo "   tests/persistence/B_estilo_publico simulam o mesmo fluxo)"
fi

# ------------------------------------------------------------------------
echo
if [ $FAIL -eq 0 ]; then
    echo "${G}${B}TODOS OS TESTES PASSARAM${N} ($PASS verificações)"
    exit 0
else
    echo "${R}${B}$FAIL FALHA(S)${N} de $((PASS + FAIL)):"
    printf '  - %s\n' "${FAILED[@]}"
    exit 1
fi
