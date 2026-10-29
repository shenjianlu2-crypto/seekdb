#!/bin/bash
# Start a fresh seekdb instance for the FTS benchmark.
# Usage: start_seekdb.sh [data-root] [port]
set -u
ROOT="${1:-/c/Users/31731/Documents/Qoder/2026-10-29/4cd0d5ef/ftsbench/inst}"
PORT="${2:-2881}"
SEEDB_DIR="/c/Users/31731/Documents/Qoder/2026-10-28/fa21b63f/seekdb/build/bin/src/observer/seekdb"
WIN_ROOT=$(cygpath -w "$ROOT" 2>/dev/null || echo "$ROOT")

taskkill //F //IM seekdb.exe >/dev/null 2>&1 || true
sleep 2
rm -rf "$ROOT"
mkdir -p "$ROOT/etc" "$ROOT/log" "$ROOT/run" "$ROOT/store"

cat > "$ROOT/etc/seekdb.cnf" <<EOF
base-dir=$WIN_ROOT
data-dir=$WIN_ROOT/store
redo-dir=$WIN_ROOT/store/redo
port=$PORT
cpu_count=8
memory_limit=8G
log_disk_size=4G
datafile_size=2G
datafile_maxsize=8G
EOF

cd "$ROOT/run" || exit 1
nohup "$SEEDB_DIR/seekdb.exe" --nodaemon -P "$PORT" --base-dir "$WIN_ROOT" > "$ROOT/log/stdout.log" 2>&1 &
echo $! > "$ROOT/run/seekdb.pid"
echo "started pid=$(cat "$ROOT/run/seekdb.pid") base=$WIN_ROOT port=$PORT"
