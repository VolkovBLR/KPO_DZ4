#!/bin/sh

set -eu

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/racing-network.XXXXXX")
port=$((45000 + $$ % 10000))
server_pid=
first_pid=
second_pid=

cleanup() {
    if [ -n "$first_pid" ] && kill -0 "$first_pid" 2>/dev/null; then
        kill "$first_pid" 2>/dev/null || true
    fi
    if [ -n "$second_pid" ] && kill -0 "$second_pid" 2>/dev/null; then
        kill "$second_pid" 2>/dev/null || true
    fi
    if [ -n "$server_pid" ] && kill -0 "$server_pid" 2>/dev/null; then
        kill "$server_pid" 2>/dev/null || true
    fi
    if [ -n "$first_pid" ]; then
        wait "$first_pid" 2>/dev/null || true
    fi
    if [ -n "$second_pid" ]; then
        wait "$second_pid" 2>/dev/null || true
    fi
    if [ -n "$server_pid" ]; then
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf "$tmpdir"
}
trap cleanup EXIT HUP INT TERM

./build/race-server --bind 127.0.0.1 --port "$port" \
    --config tests/data/short-race.conf --delay-ms 80 --autostart --once \
    >"$tmpdir/server.log" 2>&1 &
server_pid=$!

attempt=0
while ! grep -q 'listening on' "$tmpdir/server.log"; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
        cat "$tmpdir/server.log"
        echo "race server did not start listening" >&2
        exit 1
    fi
    sleep 0.02
done

./build/race-observer --host 127.0.0.1 --port "$port" --name first \
    >"$tmpdir/first.log" 2>&1 &
first_pid=$!

attempt=0
while ! grep -q 'WELCOME role=observer name=first' "$tmpdir/first.log"; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
        cat "$tmpdir/server.log" "$tmpdir/first.log"
        echo "first observer did not connect" >&2
        exit 1
    fi
    sleep 0.02
done

sleep 0.08
if kill -0 "$first_pid" 2>/dev/null; then
    kill -TERM "$first_pid"
fi
wait "$first_pid" 2>/dev/null || true
first_pid=

./build/race-observer --host 127.0.0.1 --port "$port" --name second \
    >"$tmpdir/second.log" 2>&1 &
second_pid=$!

attempt=0
while ! grep -q 'server stopped' "$tmpdir/server.log"; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 1200 ]; then
        cat "$tmpdir/server.log" "$tmpdir/second.log"
        echo "race server did not finish" >&2
        exit 1
    fi
    sleep 0.02
done
wait "$server_pid"
server_pid=
wait "$second_pid"
second_pid=

grep -q 'observer first connected' "$tmpdir/server.log"
grep -q 'observer first disconnected' "$tmpdir/server.log"
grep -q 'observer second connected' "$tmpdir/server.log"
grep -q 'FINAL rank=' "$tmpdir/server.log"
grep -q 'FINAL rank=' "$tmpdir/second.log"
grep -q 'END status=complete' "$tmpdir/second.log"
grep -q 'SNAPSHOT' "$tmpdir/second.log"

echo "network tests passed (connect, disconnect, reconnect, event stream, shutdown)"
