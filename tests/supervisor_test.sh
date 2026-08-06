#!/bin/sh
set -eu

socket=/tmp/affinitygraph-test.sock
./build/affinity-run run --config tests/runtime.toml -- /bin/sleep 30 &
supervisor_pid=$!
child_pid=

cleanup() {
  kill -TERM "$supervisor_pid" 2>/dev/null || true
  test -z "$child_pid" || kill -TERM "$child_pid" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

attempt=0
while test "$attempt" -lt 40; do
  child_pid=$(pgrep -P "$supervisor_pid" | head -1 || true)
  test -S "$socket" && test -n "$child_pid" && break
  attempt=$((attempt + 1))
  sleep 0.05
done

test -S "$socket"
test -n "$child_pid"
test "$(stat -c %a "$socket")" = 600
status=$(./build/affinityctl status --socket "$socket")
printf '%s\n' "$status" | grep -q '"effective_mode":"observe"'
printf '%s\n' "$status" | grep -q '"collector_degraded":true'

kill -TERM "$supervisor_pid"
set +e
wait "$supervisor_pid"
result=$?
set -e
test "$result" -eq 143
! kill -0 "$child_pid" 2>/dev/null
test ! -e "$socket"

./build/affinity-run run --config tests/runtime.toml -- /bin/sh -c '/bin/sleep 30 &' &
supervisor_pid=$!
child_pid=
attempt=0
while test "$attempt" -lt 40; do
  child_pid=$(pgrep -P "$supervisor_pid" | head -1 || true)
  test -n "$child_pid" && break
  attempt=$((attempt + 1))
  sleep 0.05
done
test -n "$child_pid"
kill -0 "$supervisor_pid"
kill -TERM "$supervisor_pid"
wait "$supervisor_pid"
! kill -0 "$child_pid" 2>/dev/null
trap - EXIT INT TERM
