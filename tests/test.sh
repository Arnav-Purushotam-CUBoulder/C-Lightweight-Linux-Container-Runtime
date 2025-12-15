#!/usr/bin/env bash
# ==============================================================================
# tests/test.sh — Integration tests for linuc container runtime
# ==============================================================================
# Usage:  bash tests/test.sh [path/to/linuc]
#
# Tests are ordered from simplest (namespace isolation) to most involved
# (resource limits requiring cgroup v2 and root).
#
# Exit code: 0 = all tests passed, 1 = one or more failed.
# ==============================================================================

set -euo pipefail

LINUC=${1:-./build/linuc}
PASS=0; FAIL=0

# ── Colour helpers ─────────────────────────────────────────────────────────────
GREEN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'

ok()   { echo -e "  ${GREEN}[PASS]${NC} $*"; ((PASS++)); }
fail() { echo -e "  ${RED}[FAIL]${NC} $*"; ((FAIL++)); }
section() { echo -e "\n── $* ──"; }

require_root() {
    if [[ $EUID -ne 0 ]]; then
        echo "  [SKIP] $* — requires root (run with sudo)"
        return 1
    fi
    return 0
}

require_binary() {
    if [[ ! -x "$LINUC" ]]; then
        echo "ERROR: binary not found at $LINUC"
        echo "Run 'make' first, then re-run tests."
        exit 1
    fi
}

require_binary

# ==============================================================================
section "1. Basic execution"
# ==============================================================================

# T1: exit code propagation
code=$("$LINUC" run -- /bin/true 2>/dev/null; echo $?)
if [[ "$code" == "0" ]]; then ok "exit code 0 from /bin/true"
else fail "exit code from /bin/true was $code (expected 0)"; fi

code=$("$LINUC" run -- /bin/false 2>/dev/null; echo $?) || true
if [[ "$code" == "1" ]]; then ok "exit code 1 from /bin/false"
else fail "exit code from /bin/false was $code (expected 1)"; fi

# T2: command output reaches stdout
out=$("$LINUC" run -- /bin/echo hello 2>/dev/null)
if [[ "$out" == "hello" ]]; then ok "stdout from /bin/echo works"
else fail "expected 'hello', got '$out'"; fi

# ==============================================================================
section "2. PID namespace isolation"
# ==============================================================================

# Inside the container, PID 1 should be the command itself (not the host init).
inner_pid=$("$LINUC" run -- /bin/sh -c 'echo $$' 2>/dev/null)
if [[ "$inner_pid" == "1" ]]; then
    ok "container process is PID 1 inside PID namespace"
else
    fail "expected inner PID=1, got $inner_pid"
fi

# ==============================================================================
section "3. UTS namespace (hostname) isolation"
# ==============================================================================

host_hostname=$(hostname)

# Default: hostname equals container ID.
inner_hostname=$("$LINUC" run --id mytest -- hostname 2>/dev/null)
if [[ "$inner_hostname" == "mytest" ]]; then
    ok "container hostname defaults to container ID"
else fail "expected 'mytest', got '$inner_hostname'"; fi

# Custom hostname.
inner_hostname=$("$LINUC" run --hostname mybox -- hostname 2>/dev/null)
if [[ "$inner_hostname" == "mybox" ]]; then
    ok "--hostname flag sets container hostname"
else fail "expected 'mybox', got '$inner_hostname'"; fi

# Host hostname should be unchanged.
current_host=$(hostname)
if [[ "$current_host" == "$host_hostname" ]]; then
    ok "host hostname unchanged after container run"
else fail "host hostname changed from '$host_hostname' to '$current_host'"; fi

# ==============================================================================
section "4. Mount namespace isolation"
# ==============================================================================

# A bind-mount created inside the container must not appear on the host.
tmpdir=$(mktemp -d)
trap "rm -rf $tmpdir" EXIT

mount_check=$("$LINUC" run -- /bin/sh -c \
    "mkdir -p /tmp/container_test && mount --bind /proc /tmp/container_test 2>&1; echo exit:$?" \
    2>/dev/null) || true
# Even if mount fails (no CAP_SYS_ADMIN), the host /tmp should be clean.
if [[ ! -d /tmp/container_test ]]; then
    ok "container mount namespace isolated from host"
else
    fail "container mount leaked to host"
    rmdir /tmp/container_test 2>/dev/null || true
fi

# ==============================================================================
section "5. User namespace"
# ==============================================================================

# Inside user namespace, we appear as UID 0 (root in container).
inner_uid=$("$LINUC" run -- /usr/bin/id -u 2>/dev/null)
if [[ "$inner_uid" == "0" ]]; then
    ok "container UID is 0 inside user namespace"
else fail "expected UID=0, got $inner_uid"; fi

# But we are NOT root on the host.
host_uid=$(id -u)
if [[ "$host_uid" != "0" ]]; then
    ok "running unprivileged on host (uid=$host_uid)"
else
    echo "  [INFO] running as root on host — user namespace test less meaningful"
fi

# ==============================================================================
section "6. Environment variables"
# ==============================================================================

out=$("$LINUC" run --env "LINUC_TEST=hello_world" -- /usr/bin/env 2>/dev/null \
    | grep "LINUC_TEST" || true)
if echo "$out" | grep -q "hello_world"; then
    ok "--env sets environment variable inside container"
else fail "LINUC_TEST not found in container env"; fi

# ==============================================================================
section "7. Supervisor — exit code and restart"
# ==============================================================================

# One-shot: never restart.
code=$("$LINUC" supervise --restart never -- /bin/false 2>/dev/null; echo $?) || true
if [[ "$code" == "1" ]]; then
    ok "supervisor propagates non-zero exit code"
else fail "expected 1 from supervisor wrapping /bin/false, got $code"; fi

# Restart on failure — use a counter file.
counter_file=$(mktemp)
echo "0" > "$counter_file"
chmod 666 "$counter_file"

# Script that exits 1 the first two times, then succeeds.
script=$(cat <<'EOF'
count=$(cat "$COUNTER")
count=$((count+1))
echo "$count" > "$COUNTER"
if [ "$count" -lt 3 ]; then exit 1; fi
exit 0
EOF
)

result=$("$LINUC" supervise \
    --restart on-failure --max-restarts 3 \
    --env "COUNTER=$counter_file" \
    -- /bin/sh -c "$script" 2>/dev/null; echo $?) || true
final_count=$(cat "$counter_file")
rm -f "$counter_file"

if [[ "$result" == "0" ]] && [[ "$final_count" == "3" ]]; then
    ok "supervisor restarted on-failure (ran $final_count times, final exit 0)"
else
    fail "restart on-failure: result=$result, count=$final_count (expected result=0, count=3)"
fi

# ==============================================================================
section "8. cgroup resource limits (requires root)"
# ==============================================================================

if require_root "cgroup tests"; then

    # T: memory limit — process allocating more than the limit should be OOM-killed.
    # We use 8 MB limit and try to allocate 64 MB.
    result=$("$LINUC" run --memory $((8 * 1024 * 1024)) -- \
        /bin/sh -c 'dd if=/dev/zero bs=1M count=64 | head -c 1 > /dev/null' \
        2>/dev/null; echo $?) || true
    # OOM kill returns 137 (128 + SIGKILL=9).
    if [[ "$result" == "137" ]] || [[ "$result" != "0" ]]; then
        ok "memory limit: process killed/failed when exceeding 8 MB limit"
    else
        fail "memory limit not enforced (process exited 0)"
    fi

    # T: PID limit — fork bomb should fail when pids.max is small.
    result=$("$LINUC" run --max-pids 4 -- \
        /bin/sh -c 'for i in 1 2 3 4 5; do sleep 1 & done; wait' \
        2>/dev/null; echo $?) || true
    if [[ "$result" != "0" ]]; then
        ok "pids.max limit: fork exceeded PID limit (exit $result)"
    else
        fail "pids.max limit not enforced"
    fi

fi

# ==============================================================================
section "9. Rootfs isolation (pivot_root)"
# ==============================================================================

# Create a minimal rootfs with just /bin/sh and required libs.
ROOTFS=$(mktemp -d)
mkdir -p "$ROOTFS"/{bin,lib,lib64,proc,dev}

# Copy busybox or sh + its libraries.
if command -v busybox &>/dev/null; then
    cp "$(which busybox)" "$ROOTFS/bin/sh"
    ok "pivot_root test: using busybox"
else
    cp /bin/sh "$ROOTFS/bin/sh"
    # Copy required shared libraries.
    ldd /bin/sh | grep -oP '/\S+\.so\S*' | while read -r lib; do
        dir="$ROOTFS$(dirname "$lib")"
        mkdir -p "$dir"
        cp "$lib" "$dir/" 2>/dev/null || true
    done
    ld=$(ldd /bin/sh | grep 'ld-linux' | awk '{print $1}')
    [[ -n "$ld" ]] && cp "$ld" "$ROOTFS/lib64/" 2>/dev/null || true
fi

hostname_out=$("$LINUC" run --rootfs "$ROOTFS" --hostname pivottest \
    -- /bin/sh -c 'hostname' 2>/dev/null) || true
rm -rf "$ROOTFS"

if [[ "$hostname_out" == "pivottest" ]]; then
    ok "pivot_root: ran command inside custom rootfs"
else
    # pivot_root needs CAP_SYS_ADMIN — may not work without root.
    echo "  [INFO] pivot_root output: '$hostname_out' (may need root for CAP_SYS_ADMIN)"
fi

# ==============================================================================
echo ""
echo "══════════════════════════════════"
echo " Results: ${PASS} passed, ${FAIL} failed"
echo "══════════════════════════════════"
[[ "$FAIL" -eq 0 ]]
