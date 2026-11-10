#!/usr/bin/env bash
# Opens DuckDB connected to the lab database: loads orkidea, creates the
# secret `lab`, and attaches the lab schema as `oracle`. Arguments go to
# duckdb, so `lab/orkidea-lab.sh -c ".read lab/03_aggregates.sql"` runs one
# module and exits.
#
# The extension is, in order: $ORKIDEA_EXTENSION (a .duckdb_extension file),
# this repository's release build, or `INSTALL orkidea FROM community`.
set -euo pipefail

lab="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ ! -f "$lab/.lab.env" ]]; then
    echo "No lab/.lab.env: run lab/setup.sh first." >&2
    exit 1
fi
# shellcheck source=/dev/null
source "$lab/.lab.env"

duckdb="${ORKIDEA_DUCKDB:-duckdb}"
build="$lab/../build/release/extension/orkidea/orkidea.duckdb_extension"
extension="${ORKIDEA_EXTENSION:-}"
if [[ -z "$extension" && -f "$build" ]]; then
    extension="$(cd "$(dirname "$build")" && pwd)/$(basename "$build")"
fi

flags=()
if [[ -n "$extension" ]]; then
    # A local build is not signed by DuckDB.
    flags+=(-unsigned -cmd "LOAD '$extension'")
else
    flags+=(-cmd "INSTALL orkidea FROM community" -cmd "LOAD orkidea")
fi

cd "$lab"
mkdir -p out
exec "$duckdb" -init /dev/null "${flags[@]}" -cmd ".read 00_connect.sql" "$@"
