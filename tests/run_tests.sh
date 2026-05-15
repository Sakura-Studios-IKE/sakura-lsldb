#!/bin/sh
# Drive lsldb non-interactively against a known scenario and check for
# every expected lifecycle event in the transcript.
set -u
HERE="$(dirname "$0")"
LSLDB="${1:-./lsldb}"
ROOT="$(cd "$HERE/../.." && pwd)"
LSLC="$ROOT/sakura-lslc/lslc"
SLEMU="$ROOT/sakura-slemu/slemu"
SCRIPT="$ROOT/sakura-slemu/tests/scripts/01_hello.lsl"
BC="$ROOT/sakura-slemu/tests/scripts/01_hello.lslbc"

[ -x "$LSLDB" ] || { echo "lsldb not built ($LSLDB)"; exit 2; }
[ -x "$LSLC" ]  || { echo "lslc not built";  exit 2; }
[ -x "$SLEMU" ] || { echo "slemu not built"; exit 2; }

"$LSLC" -c "$SCRIPT" >/dev/null 2>&1

vol="/tmp/lsldb_test_vol"; rm -rf "$vol"

out=$(printf 'source %s\nbreak 4\ncontinue\nlist\nstep\nlocals\ncatch chat\ncontinue\ncontinue\nquit\n' "$SCRIPT" \
    | "$LSLDB" --slemu "$SLEMU" --source "$SCRIPT" -- --volume "$vol" "$BC" 2>&1)

fail=0
for expect in \
    'attached to slemu' \
    'reason=entry' \
    'breakpoint #1' \
    'reason=breakpoint script=Object line=4' \
    'llOwnerSay' \
    'reason=step' \
    'locals:' \
    'catchpoint #1 on chat' \
    'caught\] chat: hello from slemu' \
    'slemu exited'
do
    if echo "$out" | grep -qE "$expect"; then
        printf "  PASS  %s\n" "$expect"
    else
        printf "  FAIL  %s\n" "$expect"
        fail=$((fail+1))
    fi
done

echo
if [ $fail -eq 0 ]; then
    echo "All lsldb integration checks passed."
    exit 0
else
    echo "$fail check(s) FAILED"
    echo "------- transcript -------"
    echo "$out"
    exit 1
fi
