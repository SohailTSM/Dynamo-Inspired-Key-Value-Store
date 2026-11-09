#!/usr/bin/env bash
# Compile the PlantUML diagrams of both design documents to SVG and PNG:
#   docs/hld/diagrams/src/*.puml -> docs/hld/diagrams/out/
#   docs/lld/diagrams/src/*.puml -> docs/lld/diagrams/out/
# Shared style: docs/plantuml-style.iuml. Needs only Java: non-sequence diagrams use PlantUML's built-in
# Smetana layout (`!pragma layout smetana`), so Graphviz is not required.
set -euo pipefail
DOCS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DOCS/.." && pwd)"
VERSION="1.2026.9"
JAR="$ROOT/tools/plantuml-$VERSION.jar"
URL="https://github.com/plantuml/plantuml/releases/download/v$VERSION/plantuml-$VERSION.jar"

if [[ ! -f "$JAR" ]]; then
  mkdir -p "$ROOT/tools"
  echo "Downloading PlantUML $VERSION ..."
  curl -fsSL "$URL" -o "$JAR"
fi

for doc in hld lld; do
  dir="$DOCS/$doc/diagrams"
  mkdir -p "$dir/out"
  for fmt in svg png; do
    java -Djava.awt.headless=true -jar "$JAR" -t"$fmt" -failfast2 -o "$dir/out" "$dir/src/"*.puml
  done
  echo "$doc: $(ls "$dir/out" | wc -l) files in docs/$doc/diagrams/out/"
done
