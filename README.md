# linuc — Lightweight Linux Container Runtime in C++

A from-scratch container runtime built with **Linux namespaces**, **cgroup v2**, and **capability dropping**.  
Compiles to a single binary with no runtime dependencies (besides `libcap`).

---

## Table of Contents
1. [What is a container (actually)?](#what-is-a-container)
2. [Core Linux primitives](#core-linux-primitives)
   - [Namespaces](#namespaces)
   - [cgroups (control groups)](#cgroups)
   - [Linux capabilities](#linux-capabilities)
3. [Architecture](#architecture)
4. [File layout](#file-layout)
5. [Setup on Mac with Lima](#setup-on-mac-with-lima)
6. [Building](#building)
7. [Usage](#usage)
8. [Running tests](#running-tests)
9. [Running benchmarks](#running-benchmarks)
10. [Concept deep-dives](#concept-deep-dives)

---

## What is a container?

A Linux container is **not** a virtual machine. It is a regular process (or process tree) running on the host kernel but isolated through three kernel mechanisms:

| Mechanism | Provides |
|---|---|
| Namespaces | What the process can *see* (its own PID tree, hostname, filesystem, network) |
| cgroups | How many resources it can *use* (CPU time, memory bytes, process count) |
| Capabilities | Which privileged operations it can *perform* |

Docker, containerd, and Podman all sit on these same primitives.  
This project implements them directly in ~2 500 lines of C++.

---

## Core Linux primitives

### Namespaces

Namespaces are kernel bookkeeping structures that give a process a *private copy* of a kernel resource.  
Six namespaces are used by `linuc`:

```
CLONE_NEWPID  — PID namespace
CLONE_NEWNS   — Mount namespace
CLONE_NEWUTS  — UTS (hostname) namespace
CLONE_NEWIPC  — IPC namespace
CLONE_NEWNET  — Network namespace
CLONE_NEWUSER — User namespace
```

**PID namespace (`CLONE_NEWPID`)**  
The container's first process appears as **PID 1** from inside the container.  
The host sees it as some other PID (e.g., 12345).  
Consequence: the container's "init" (PID 1) is responsible for reaping orphaned child processes, just like the real init.

**Mount namespace (`CLONE_NEWNS`)**  
The container gets a private mount table.  Bind-mounts, new `/proc`, etc., applied inside the container are invisible to the host.  
We call `mount("none", "/", MS_REC|MS_PRIVATE)` first to sever propagation from the host.

**User namespace (`CLONE_NEWUSER`)**  
Maps host UIDs to container UIDs.  The unprivileged host user (UID 1000) can appear as UID 0 ("root") inside the container without gaining any real kernel privileges.  
The parent writes the mapping into `/proc/<child>/uid_map` and `/proc/<child>/gid_map` before the child proceeds.

```
/proc/<child>/uid_map:   0  1000  1
                         ^   ^    ^
                  container  host  count
```

**Why the sync pipe?**  
After `clone()`, the child must *wait* for the parent to write UID/GID maps before proceeding.  Without this synchronisation the child has no capabilities inside the user namespace and any privileged setup will fail.  We use a simple Unix pipe as a semaphore.

### cgroups

cgroups (control groups) are a Linux feature that organises processes into a hierarchy and lets the kernel enforce resource quotas on each node.

**cgroup v2 unified hierarchy** (`/sys/fs/cgroup/`) — default in Ubuntu 22.04+:

```
/sys/fs/cgroup/
└── linuc/                      ← our parent group
    └── linuc-abc123/           ← per-container group
        ├── cgroup.procs        ← write PID here to add process
        ├── memory.max          ← hard memory limit in bytes
        ├── memory.swap.max     ← set to 0 to disable swap
        ├── cpu.max             ← "quota_us period_us"
        ├── pids.max            ← max process count
        ├── memory.current      ← live usage
        ├── memory.peak         ← high-water mark
        └── cpu.stat            ← CPU usage counters
```

**CPU quota example:**  
`cpu.max = "50000 100000"` means the cgroup can consume at most 50 ms out of every 100 ms period, i.e., 50% of one CPU core.

**Memory hard limit:**  
Once `memory.max` is reached, new allocations trigger the kernel's OOM (out-of-memory) killer, which sends SIGKILL to the most memory-hungry process in the cgroup.

### Linux capabilities

Traditional Unix privilege: root (UID 0) can do anything; everyone else is restricted.

Linux **capabilities** break root's power into ~40 fine-grained tokens:

| Capability | Allows |
|---|---|
| `CAP_NET_ADMIN` | Configure network interfaces |
| `CAP_SYS_ADMIN` | Mount filesystems, namespaces, etc. |
| `CAP_CHOWN` | Change file ownership |
| `CAP_KILL` | Send signals to any process |
| `CAP_SYS_PTRACE` | Use ptrace on any process |

Container hardening strategy in `linuc`:
1. Set up all namespaces/mounts (needs some caps).
2. Drop **all** capabilities via `cap_set_proc(empty_set)`.
3. Set `PR_SET_NO_NEW_PRIVS = 1` — exec() can never grant more capabilities, even for setuid binaries.
4. Set securebits so UID 0 inside the container doesn't auto-grant privileges.

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│  linuc CLI  (src/main.cpp)                          │
│  Parses flags, dispatches to subcommand             │
└────────────┬──────────────────┬────────────────────┘
             │                  │
    ┌────────▼──────┐   ┌───────▼────────┐
    │  Runtime      │   │  Supervisor    │
    │  (runtime.*)  │   │  (supervisor.*)│
    │               │   │                │
    │  1. Cgroup    │   │  - restart     │
    │  2. clone()   │   │  - signal fwd  │
    │  3. uid maps  │   │  - lifecycle   │
    │  4. mounts    │   │    logging     │
    │  5. pivot_root│   └───────┬────────┘
    │  6. cap drop  │           │ calls
    │  7. exec()    │◄──────────┘
    └────────┬──────┘
             │ uses
    ┌────────▼──────┐   ┌────────────────┐
    │  Cgroup       │   │  Caps          │
    │  (cgroup.*)   │   │  (caps.*)      │
    │  cgroup v2 API│   │  libcap wrapper│
    └───────────────┘   └────────────────┘

  Logger (logger.h) — used by all components, NDJSON to stderr
  Bench  (bench.cpp) — standalone microbenchmarks
```

### Execution flow

```
Parent                               Child (new namespaces)
  │                                     │
  ├─ Cgroup::create()                   │
  ├─ clone(childEntry, stack, flags) ──►│ blocked reading pipe
  ├─ Cgroup::addProcess(child_pid)      │
  ├─ write /proc/<pid>/uid_map          │
  ├─ write /proc/<pid>/gid_map          │
  ├─ write(pipe, 'R')  ─────────────── ►│ unblocked
  │                                     ├─ mount("none", "/", MS_PRIVATE)
  │                                     ├─ doPivotRoot()   [if --rootfs]
  │                                     ├─ mountProc()
  │                                     ├─ setHostname()
  │                                     ├─ dropAllCapabilities()
  │                                     ├─ setNoNewPrivs()
  │                                     └─ execvpe(cmd)
  │
  ├─ waitpid(child_pid)
  ├─ Cgroup::readStats()
  ├─ Cgroup::destroy()
  └─ return exit_code
```

---

## File layout

```
.
├── src/
│   ├── logger.h        Structured JSON logger (NDJSON to stderr)
│   ├── container.h     Config types: ContainerConfig, CgroupConfig, etc.
│   ├── cgroup.h / .cpp cgroup v2: create, limits, stats, destroy
│   ├── caps.h / .cpp   Linux capability dropping (libcap)
│   ├── runtime.h / .cpp  clone(), namespaces, pivot_root, exec
│   ├── supervisor.h / .cpp  Lifecycle, signal forwarding, restart policy
│   └── main.cpp        CLI: run | supervise | bench subcommands
├── bench/
│   └── bench.cpp       Startup latency, memory overhead, CPU isolation
├── tests/
│   └── test.sh         Integration test suite (9 test groups)
├── Makefile
├── setup.sh            One-shot Lima VM + dependency setup on macOS
└── README.md           This file
```

---

## Setup on Mac with Lima

**Prerequisites:** macOS (Apple Silicon or Intel), internet access.

```bash
# Clone / copy the project to your Mac, then:
bash setup.sh
```

`setup.sh` does:
1. Installs [Homebrew](https://brew.sh) if missing
2. Installs [Lima](https://lima-vm.io) via Homebrew
3. Creates an Ubuntu 22.04 VM named `linuc-dev` with 2 CPUs / 2 GB RAM
4. Mounts the project directory at `/linuc` inside the VM (writable, live-sync)
5. Installs build dependencies (`g++`, `libcap-dev`, `busybox-static`, …)
6. Runs `make` inside the VM

**After setup:**
```bash
limactl shell linuc-dev   # SSH into the VM
cd /linuc
./build/linuc run -- hostname
```

---

## Building

Inside the Lima VM (or any Linux with `g++ ≥ 9` and `libcap-dev`):

```bash
# Install deps (Ubuntu/Debian)
sudo apt-get install -y build-essential libcap-dev

# Build
make

# Binary produced at:
./build/linuc
```

---

## Usage

### `run` — one-shot container

```bash
# Minimal: run a shell
sudo ./build/linuc run -- /bin/sh

# Custom hostname
./build/linuc run --hostname mybox -- hostname

# Resource limits (requires root for cgroups)
sudo ./build/linuc run \
    --memory 134217728 \   # 128 MB hard limit
    --cpu 50 \             # 50% of one CPU core
    --max-pids 32 \        # max 32 processes
    -- /bin/sh

# With a custom rootfs
sudo ./build/linuc run --rootfs /tmp/myroot -- /bin/sh

# Pass environment variables
./build/linuc run --env "MY_VAR=hello" -- /usr/bin/env
```

### `supervise` — run under supervisor

```bash
# Restart on failure, up to 5 times
sudo ./build/linuc supervise \
    --restart on-failure --max-restarts 5 \
    -- /bin/my-service

# Always restart (like a daemonised service)
sudo ./build/linuc supervise --restart always -- /bin/server
```

### `bench` — run benchmarks

```bash
# Needs root for cgroup CPU isolation test
sudo ./build/linuc bench --iterations 20

# Pretty-print JSON output
sudo ./build/linuc bench | jq .
```

---

## Running tests

```bash
# Basic tests (no root needed — namespace + env tests)
bash tests/test.sh ./build/linuc

# Full test suite including cgroup limits
sudo bash tests/test.sh ./build/linuc

# Via make
make test        # runs as current user
sudo make test   # full suite
```

**Test coverage:**
1. Exit code propagation
2. PID namespace — container sees itself as PID 1
3. UTS namespace — hostname isolation, `--hostname` flag
4. Mount namespace — container mounts don't leak to host
5. User namespace — container UID = 0, host UID unchanged
6. Environment variables
7. Supervisor exit code + restart-on-failure
8. cgroup memory limit (OOM kill) *(root)*
9. cgroup pids.max *(root)*
10. pivot_root with custom rootfs

---

## Running benchmarks

```bash
sudo ./build/linuc bench --iterations 30 | tee results.ndjson
cat results.ndjson | jq .
```

**Benchmark 1 — Startup latency**

Measures `clone()` + namespace setup time vs plain `fork()`.
Typical result on a modern kernel:

```json
{"bench":"startup_latency","metric":"native_mean_us","value":120.5,"unit":"us"}
{"bench":"startup_latency","metric":"container_mean_us","value":1842.3,"unit":"us"}
{"bench":"startup_latency","metric":"overhead_us","value":1721.8,"unit":"us","note":"namespace setup cost"}
{"bench":"startup_latency","metric":"overhead_pct","value":1428.5,"unit":"%"}
```

Namespace setup costs ~1–3 ms depending on the kernel and VM.

**Benchmark 2 — Memory overhead**

Measures RSS of a minimal container process vs the same process without namespaces.

```json
{"bench":"memory_overhead","metric":"native_rss_kb","value":512.0,"unit":"kB"}
{"bench":"memory_overhead","metric":"container_rss_kb","value":896.0,"unit":"kB"}
{"bench":"memory_overhead","metric":"overhead_kb","value":384.0,"unit":"kB","note":"namespace kernel bookkeeping"}
```

**Benchmark 3 — CPU isolation**

Spawns a CPU spinner capped at 25% quota, measures actual CPU usage over 2 seconds.

```json
{"bench":"cpu_isolation","metric":"actual_cpu_pct","value":26.1,"unit":"%"}
{"bench":"cpu_isolation","metric":"expected_cpu_pct","value":25.0,"unit":"%"}
{"bench":"cpu_isolation","metric":"throttle_effective","value":1.0,"unit":"bool","note":"1=cgroup limit respected within 30% tolerance"}
```

---

## Concept deep-dives

### Why `clone()` instead of `fork()` + `unshare()`?

`unshare()` after `fork()` works for most namespaces but has a subtlety with **PID namespaces**: calling `unshare(CLONE_NEWPID)` moves *future children* into the new PID namespace, but not the calling process itself.  To be PID 1 inside the container you need to be spawned into the namespace from the start, which requires `clone()`.

### Why is the sync pipe necessary?

After `clone()` with `CLONE_NEWUSER`, the child process lives in a user namespace where nobody has written UID/GID mappings yet.  Without mappings the child's UID/GID show as `65534` (overflow UID) and it has no capabilities.  The parent must write `/proc/<child>/uid_map` and `/proc/<child>/gid_map` before the child proceeds.  The pipe provides this ordering guarantee: parent writes maps → writes 'R' to pipe → child unblocks.

### Why `pivot_root` instead of `chroot`?

`chroot` changes the root directory for the calling process but leaves the original root accessible to privileged processes (via `/proc/self/root` or by doing `chroot("/")`).  `pivot_root` actually *swaps* the filesystem root, making the old root unmountable, which is the approach used by production runtimes (runc, crun).  We unmount the old root after pivoting, leaving only the new rootfs visible.

### How does cgroup v2 CPU throttling work?

The CFS (Completely Fair Scheduler) in the kernel tracks CPU bandwidth using a token bucket: the cgroup is given `quota_us` tokens at the start of each `period_us` window.  When tokens run out, the kernel puts all threads in the cgroup to sleep until the next period.  This is called **CFS bandwidth control** and is what makes the 25% CPU limit in the benchmark repeatable and measurable.

### The self-pipe trick in the supervisor

POSIX restricts what you can do inside a signal handler to a short list of async-signal-safe functions.  Calling `Logger::instance().log()` (which takes a mutex) from a handler would risk deadlock.  The supervisor instead writes a single byte to a pipe in the handler, and the main event loop reads from that pipe — this way complex logic runs in the normal call stack, not in the signal handler.
