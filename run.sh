#!/usr/bin/env bash
# Contrato de execução da disciplina:
#   ./run.sh --data-dir CAMINHO --input CAMINHO --output CAMINHO
#
# 1. Compila o engine (build/engine) se o binário não existir ou estiver
#    desatualizado. Requer g++ com suporte a C++17; usa make se disponível.
# 2. Cria o diretório de dados se necessário.
# 3. Processa as operações do --input em ordem e grava uma resposta JSON por
#    linha no --output.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$ROOT/build/engine"

usage() {
    echo "uso: $0 --data-dir CAMINHO --input CAMINHO --output CAMINHO" >&2
    exit 2
}

DATA_DIR="" INPUT="" OUTPUT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --data-dir) [ $# -ge 2 ] || usage; DATA_DIR="$2"; shift 2 ;;
        --input)    [ $# -ge 2 ] || usage; INPUT="$2";    shift 2 ;;
        --output)   [ $# -ge 2 ] || usage; OUTPUT="$2";   shift 2 ;;
        --data-dir=*) DATA_DIR="${1#*=}"; shift ;;
        --input=*)    INPUT="${1#*=}";    shift ;;
        --output=*)   OUTPUT="${1#*=}";   shift ;;
        -h|--help) usage ;;
        *) echo "argumento desconhecido: $1" >&2; usage ;;
    esac
done

[ -n "$DATA_DIR" ] && [ -n "$INPUT" ] && [ -n "$OUTPUT" ] || usage

if [ ! -f "$INPUT" ]; then
    echo "erro: arquivo de entrada não encontrado: $INPUT" >&2
    exit 1
fi

build() {
    if command -v make >/dev/null 2>&1; then
        make -s -C "$ROOT" build/engine >&2
    else
        local cxx="${CXX:-g++}"
        mkdir -p "$ROOT/build"
        "$cxx" -std=c++17 -O2 -Wall -Wextra -I"$ROOT/src" \
            "$ROOT/src/main.cpp" "$ROOT/src/engine.cpp" -o "$BIN" >&2
    fi
}

needs_build=0
if [ ! -x "$BIN" ]; then
    needs_build=1
elif [ -n "$(find "$ROOT/src" -type f -newer "$BIN" -print -quit)" ]; then
    needs_build=1
fi
if [ "$needs_build" -eq 1 ]; then
    build || { echo "erro: falha ao compilar o engine (é necessário g++ com C++17)" >&2; exit 1; }
fi

# Se o --data-dir não puder ser criado (ex.: já existe um arquivo com esse
# nome), o próprio engine responde uma mensagem de erro por operação.
mkdir -p "$DATA_DIR" 2>/dev/null || true
mkdir -p "$(dirname "$OUTPUT")"

exec "$BIN" run --data-dir "$DATA_DIR" --input "$INPUT" --output "$OUTPUT"
