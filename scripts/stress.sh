#!/usr/bin/env bash
# Starts a server on a scratch directory and runs mtfs-stress against it.
#
#   scripts/stress.sh [build-dir] [clients] [seconds]
#
# Exits non-zero if the stress run saw a corrupt read or an unexpected error.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
CLIENTS="${2:-32}"
DURATION="${3:-10}"
PORT="${PORT:-9292}"
BIN="$BUILD/bin"

WORK="$(mktemp -d)"
SERVER_PID=""
cleanup() {
  if [[ -n "$SERVER_PID" ]]; then kill "$SERVER_PID" 2>/dev/null || true; fi
  rm -rf "$WORK"
}
trap cleanup EXIT

echo "stress-pass" | "$BIN/mtfs-user" --db "$WORK/users.db" add stress writer >/dev/null
# One worker per client: each session occupies a worker for its whole lifetime.
"$BIN/mtfs-server" --port "$PORT" --root "$WORK/files" --users "$WORK/users.db" \
  --workers "$CLIENTS" 2>"$WORK/server.log" &
SERVER_PID=$!

# Wait until the server accepts connections (an empty session just says QUIT).
for _ in $(seq 50); do
  if "$BIN/mtfs-client" --port "$PORT" </dev/null >/dev/null 2>&1; then break; fi
  sleep 0.1
done

if "$BIN/mtfs-stress" --port "$PORT" --user stress --password stress-pass \
     --clients "$CLIENTS" --seconds "$DURATION"; then
  STATUS=0
else
  STATUS=$?
  echo "--- last lines of the server log ---"
  tail -n 50 "$WORK/server.log"
fi

kill -INT "$SERVER_PID"
wait "$SERVER_PID" || true
SERVER_PID=""
exit "$STATUS"
