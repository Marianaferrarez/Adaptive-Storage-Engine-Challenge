#!/usr/bin/env bash
# Empacota a entrega no formato exigido pelo guia de submissão:
#   ED2_2026_2_grupo-glm.zip contendo na raiz README.md, run.sh, src/, tests/, report.pdf
#
# Uso: bash tests/make_zip.sh   (ou: make zip; GRUPO=... troca o nome do grupo)
# Usa `git archive` (só arquivos versionados: nada de build/, data/, .vscode/...)
# e mantém o bit executável do run.sh.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

GRUPO="${GRUPO:-glm}"
REF="${REF:-HEAD}"   # commit a empacotar
OUT="$ROOT/ED2_2026_2_grupo-$GRUPO.zip"

for f in README.md run.sh src tests report.pdf; do
    git ls-files --error-unmatch "$f" >/dev/null 2>&1 || git ls-files "$f" | grep -q . \
        || { echo "erro: '$f' não está versionado no git (faça git add)" >&2; exit 1; }
done
[ "$REF" = HEAD ] && [ -n "$(git status --porcelain)" ] && echo "aviso: há mudanças não commitadas; o zip usa o último commit (HEAD)" >&2

rm -f "$OUT"
git archive --format=zip -o "$OUT" "$REF" \
    README.md run.sh Makefile .gitattributes src tests docs report.pdf experiments

PY="$(command -v python3 || command -v python)"
# Extrai preservando permissões (o unzip pode não estar instalado; zipfile
# do Python não restaura o bit executável sozinho).
extract() {
    "$PY" - "$1" "$2" <<'EOF'
import os, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
for info in z.infolist():
    path = z.extract(info, sys.argv[2])
    mode = info.external_attr >> 16
    if mode and not info.is_dir():
        os.chmod(path, mode & 0o777)
print(f"{len(z.infolist())} entradas; raiz:", sorted({n.split('/')[0] for n in z.namelist()}))
EOF
}

echo "gerado: $OUT"

# Confere se o zip funciona sozinho: extrai num diretório limpo e roda os testes rápidos.
if [ "${SKIP_ZIP_TEST:-0}" != "1" ]; then
    T="$(mktemp -d)"
    extract "$OUT" "$T"
    [ -x "$T/run.sh" ] || { echo "erro: run.sh perdeu o bit executável dentro do zip" >&2; exit 1; }
    echo "testando o conteúdo extraído do zip..."
    ( cd "$T" && QUICK=1 bash tests/run_tests.sh >"$T/../zip-test.log" 2>&1 ) \
        && echo "OK: o zip extraído compila e passa nos testes" \
        || { echo "FALHA ao testar o zip extraído:"; tail -30 "$T/../zip-test.log"; exit 1; }
    rm -rf "$T"
fi
