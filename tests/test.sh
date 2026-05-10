#!/usr/bin/env bash

set -euo pipefail

LINUC=${1:-./build/linuc}
PASS=0
FAIL=0

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

ok()   { echo -e "  ${GREEN}[PASS]${NC} $*"; ((PASS++)); }
fail() { echo -e "  ${RED}[FAIL]${NC} $*"; ((FAIL++)); }
section() { echo -e "\n-- $* --"; }

require_root() {
    if [[ $EUID -ne 0 ]]; then
        echo "  [SKIP] $* - requires root"
        return 1
    fi
    return 0
}

if [[ ! -x "$LINUC" ]]; then
    echo "ERROR: binary not found at $LINUC"
    exit 1
fi

section "1. Basic execution"

code=$("$LINUC" run -- /bin/true 2>/dev/null; echo $?)
if [[ "$code" == "0" ]]; then ok "exit code 0 from /bin/true"
else fail "exit code from /bin/true was $code"; fi

code=$("$LINUC" run -- /bin/false 2>/dev/null; echo $?) || true
if [[ "$code" == "1" ]]; then ok "exit code 1 from /bin/false"
else fail "exit code from /bin/false was $code"; fi

out=$("$LINUC" run -- /bin/echo hello 2>/dev/null)
if [[ "$out" == "hello" ]]; then ok "stdout from /bin/echo works"
else fail "expected 'hello', got '$out'"; fi

section "2. Namespace checks"

inner_pid=$("$LINUC" run -- /bin/sh -c 'echo $$' 2>/dev/null)
if [[ "$inner_pid" == "1" ]]; then ok "container process is PID 1"
else fail "expected inner PID=1, got $inner_pid"; fi

inner_hostname=$("$LINUC" run --hostname mybox -- hostname 2>/dev/null)
if [[ "$inner_hostname" == "mybox" ]]; then ok "hostname isolation works"
else fail "expected hostname mybox, got '$inner_hostname'"; fi

inner_uid=$("$LINUC" run -- /usr/bin/id -u 2>/dev/null)
if [[ "$inner_uid" == "0" ]]; then ok "container UID is 0 inside user namespace"
else fail "expected UID=0, got $inner_uid"; fi

section "3. Supervisor restart"

counter_file=$(mktemp)
echo "0" > "$counter_file"

result=$("$LINUC" supervise --max-restarts 3 -- /bin/sh -c \
    "count=\$(cat '$counter_file'); count=\$((count+1)); echo \$count > '$counter_file'; if [ \$count -lt 3 ]; then exit 1; fi; exit 0" \
    2>/dev/null; echo $?) || true

final_count=$(cat "$counter_file")
rm -f "$counter_file"

if [[ "$result" == "0" ]] && [[ "$final_count" == "3" ]]; then
    ok "supervisor restarts a failing workload"
else
    fail "restart flow failed (code=$result, count=$final_count)"
fi

section "4. Supervisor signal forwarding"

term_file=$(mktemp)
rm -f "$term_file"

"$LINUC" supervise -- /bin/sh -c \
    "trap 'echo term > \"$term_file\"; exit 0' TERM INT; while :; do sleep 1; done" \
    >/dev/null 2>&1 &
sup_pid=$!

sleep 1
kill -TERM "$sup_pid"
set +e
wait "$sup_pid"
sup_code=$?
set -e

marker=$(cat "$term_file" 2>/dev/null || true)
rm -f "$term_file"

if [[ "$sup_code" == "0" ]] && [[ "$marker" == "term" ]]; then
    ok "supervisor forwards SIGTERM into the container"
else
    fail "signal forwarding failed (code=$sup_code, marker='$marker')"
fi

section "5. Memory limit"

if require_root "memory limit test"; then
    result=$("$LINUC" run --memory $((8 * 1024 * 1024)) -- \
        /bin/sh -c 'dd if=/dev/zero bs=1M count=64 | head -c 1 > /dev/null' \
        2>/dev/null; echo $?) || true

    if [[ "$result" != "0" ]]; then
        ok "memory limit stops oversized workload"
    else
        fail "memory limit was not enforced"
    fi
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[[ "$FAIL" -eq 0 ]]
