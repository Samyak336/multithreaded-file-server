#!/usr/bin/env bash
# A two-minute tour: builds the project, creates three users, starts a
# server on a scratch directory and runs a few client sessions that show
# uploads, appends and role checks. Usage: scripts/demo.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
PORT="${PORT:-9191}"

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" -j >/dev/null
BIN="$BUILD/bin"

DEMO="$(mktemp -d)"
SERVER_PID=""
cleanup() {
  if [[ -n "$SERVER_PID" ]]; then kill "$SERVER_PID" 2>/dev/null || true; fi
  rm -rf "$DEMO"
}
trap cleanup EXIT

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }

step "Create users (passwords piped in; on a terminal mtfs-user prompts without echo)"
echo "admin-pass"  | "$BIN/mtfs-user" --db "$DEMO/users.db" add ada admin
echo "writer-pass" | "$BIN/mtfs-user" --db "$DEMO/users.db" add walt writer
echo "reader-pass" | "$BIN/mtfs-user" --db "$DEMO/users.db" add rita reader
"$BIN/mtfs-user" --db "$DEMO/users.db" list
echo "The database stores only salted hashes:"
grep -v '^#' "$DEMO/users.db" | head -1

step "Start the server (log in $DEMO/server.log)"
"$BIN/mtfs-server" --port "$PORT" --root "$DEMO/files" --users "$DEMO/users.db" 2>"$DEMO/server.log" &
SERVER_PID=$!
sleep 0.5

step "walt (writer) uploads a file and appends to it"
printf 'first line, uploaded with put\n' > "$DEMO/hello.txt"
"$BIN/mtfs-client" --port "$PORT" <<EOF
login walt writer-pass
put $DEMO/hello.txt notes.txt
append notes.txt second line, added with append
cp notes.txt notes-copy.txt
ls
EOF

step "rita (reader) can read, but every write is refused"
"$BIN/mtfs-client" --port "$PORT" <<EOF || true
login rita reader-pass
cat notes.txt
append notes.txt this should fail
rm notes.txt
EOF

step "ada (admin) is the only role allowed to delete"
"$BIN/mtfs-client" --port "$PORT" <<EOF
login ada admin-pass
rm notes-copy.txt
ls
EOF

step "Stop the server with SIGINT (graceful shutdown)"
kill -INT "$SERVER_PID"
wait "$SERVER_PID" || true
SERVER_PID=""
tail -n 6 "$DEMO/server.log"
