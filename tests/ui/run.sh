#!/bin/bash
# Runs one UI script against the test copy, in its own folder with its own database.
#   tests/ui/run.sh <PM_uitest.exe> <script> <work folder> [fresh|demo]
# fresh: start from a copy of release_db/empty.db (the original is never opened); demo: from a copy of release_db/data.db. Without it the work folder's test.db goes on
# from the previous script, so the scripts can be played in order (01, 02, 03...). Log and screenshots: <work folder>/out/<script>.
set -e
EXE="$1"; SCRIPT="$(realpath "$2")"; W="$3"; HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$W"
cp "$EXE" "$W/PM_uitest.exe"
if [ "$4" == "fresh" ] || [ "$4" == "demo" ] || [ ! -f "$W/test.db" ]; then
    rm -rf "$W"/test.db* "$W/backups" "$W/logs" "$W/ui_prefs.ini" "$W/imgui.ini"
    if [ "$4" == "demo" ]; then cp "$HERE/../../release_db/data.db" "$W/test.db"; else cp "$HERE/../../release_db/empty.db" "$W/test.db"; fi
fi
printf 'db_path=test.db\n' > "$W/config.ini"
OUT="$W/out/$(basename "$SCRIPT" .txt)"; rm -rf "$OUT"; mkdir -p "$OUT"
win() { if command -v cygpath >/dev/null; then cygpath -w "$1"; else echo "$1"; fi; }
# Excel exports skip the Save As dialog and land next to the screenshots (a second export of the same list gets " (2)")
cd "$W" && BM_UI_SCRIPT="$(win "$SCRIPT")" BM_UI_OUT="$(win "$OUT")" BM_UI_EXPORT_DIR="$(win "$OUT")" ./PM_uitest.exe || true
grep -E "FAIL|DONE" "$OUT/uitest.log"
