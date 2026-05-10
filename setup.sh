#!/usr/bin/env bash
# ==============================================================================
# setup.sh — Install Lima VM and prepare the Linux build environment on macOS
# ==============================================================================
# Run this ONCE on your Mac.  After it completes, the project is synced into
# the Lima VM and you interact with Linux via `limactl shell default`.
#
# Steps performed:
#   1. Install Homebrew (if missing)
#   2. Install Lima via Homebrew
#   3. Start a default Ubuntu 22.04 Lima VM
#   4. Inside the VM: install build dependencies (g++, libcap-dev, etc.)
#   5. Mount the project directory into the VM
#   6. Build the project
#   7. Run tests
# ==============================================================================

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIMA_VM="linuc-dev"

echo "════════════════════════════════════════════════════════"
echo "  linuc runtime — Lima VM setup"
echo "  Project: $PROJECT_DIR"
echo "════════════════════════════════════════════════════════"

# ── Step 1: Homebrew ──────────────────────────────────────────────────────────
if ! command -v brew &>/dev/null; then
    echo "[1/7] Installing Homebrew..."
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
else
    echo "[1/7] Homebrew already installed: $(brew --version | head -1)"
fi

# ── Step 2: Lima ─────────────────────────────────────────────────────────────
if ! command -v limactl &>/dev/null; then
    echo "[2/7] Installing Lima..."
    brew install lima
else
    echo "[2/7] Lima already installed: $(limactl --version)"
fi

# ── Step 3: Lima VM config ────────────────────────────────────────────────────
LIMA_CONFIG="$PROJECT_DIR/.lima-config.yaml"
cat > "$LIMA_CONFIG" <<YAML
# Lima VM config for linuc development
vmType: qemu
images:
  - location: "https://cloud-images.ubuntu.com/releases/22.04/release/ubuntu-22.04-server-cloudimg-amd64.img"
    arch: "x86_64"
  - location: "https://cloud-images.ubuntu.com/releases/22.04/release/ubuntu-22.04-server-cloudimg-arm64.img"
    arch: "aarch64"
cpus: 2
memory: "2GiB"
disk: "20GiB"
mounts:
  - location: "$PROJECT_DIR"
    mountPoint: "/linuc"
    writable: true
  - location: "~"
    writable: false
containerd:
  system: false
  user: false
YAML

echo "[3/7] Lima config written to $LIMA_CONFIG"

# ── Step 4: Start the VM ──────────────────────────────────────────────────────
if limactl list | grep -q "^${LIMA_VM}"; then
    echo "[4/7] VM '${LIMA_VM}' already exists."
    limactl start "$LIMA_VM" 2>/dev/null || true
else
    echo "[4/7] Creating and starting Lima VM '${LIMA_VM}'..."
    limactl start --name="$LIMA_VM" "$LIMA_CONFIG"
fi

echo "[4/7] VM is running."

# ── Step 5: Install build dependencies inside the VM ─────────────────────────
echo "[5/7] Installing build dependencies inside VM..."
limactl shell "$LIMA_VM" -- bash -s <<'INNER'
set -e
export DEBIAN_FRONTEND=noninteractive
sudo apt-get update -qq
sudo apt-get install -y -qq \
    build-essential \
    g++ \
    libcap-dev \
    linux-headers-$(uname -r) \
    busybox-static \
    jq \
    strace \
    util-linux
echo "Build dependencies installed."
INNER

# ── Step 6: Verify cgroup v2 ─────────────────────────────────────────────────
echo "[6/7] Checking cgroup v2 availability..."
limactl shell "$LIMA_VM" -- bash -s <<'INNER'
if mount | grep -q "cgroup2 on /sys/fs/cgroup"; then
    echo "  cgroup v2: YES — resource limits will work"
else
    echo "  cgroup v2: NO — checking /proc/mounts..."
    cat /proc/mounts | grep cgroup
fi
# Verify user namespace support.
if unshare --user true 2>/dev/null; then
    echo "  user namespaces: YES — rootless containers supported"
else
    echo "  user namespaces: NO — will need root for container creation"
    # Enable user namespaces for unprivileged users.
    echo 1 | sudo tee /proc/sys/kernel/unprivileged_userns_clone 2>/dev/null || true
fi
INNER

# ── Step 7: Build the project ─────────────────────────────────────────────────
echo "[7/7] Building linuc inside VM..."
limactl shell "$LIMA_VM" -- bash -s <<'INNER'
cd /linuc
make clean 2>/dev/null || true
make
echo ""
echo "Build successful! Binary at: /linuc/build/linuc"
echo ""
echo "Quick test (no root needed):"
/linuc/build/linuc run -- hostname
INNER

# ── Done ─────────────────────────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════════════════════"
echo "  Setup complete!"
echo ""
echo "  Enter the VM:     limactl shell $LIMA_VM"
echo "  Run a container:  /linuc/build/linuc run -- /bin/sh"
echo "  Run tests:        cd /linuc && bash tests/test.sh"
echo "  Run benchmarks:   cd /linuc && sudo make bench"
echo "  Stop the VM:      limactl stop $LIMA_VM"
echo "════════════════════════════════════════════════════════"
