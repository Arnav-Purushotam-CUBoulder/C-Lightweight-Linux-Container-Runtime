# linuc

A lightweight Linux container runtime in C++ built around the three kernel primitives that matter most for this project:

- namespaces for process and hostname isolation
- cgroup v2 for CPU and memory limits
- capability dropping for a smaller privilege surface

This trimmed version is intentionally smaller than the original repo so the code maps more directly to the core resume story:

> Built a lightweight Linux container runtime in C++ using namespaces, cgroups, and capability dropping to isolate processes, enforce CPU and memory limits, and support repeatable execution of sandboxed command-line workloads.
>
> Implemented a process supervisor with lifecycle management, signal forwarding, and structured logging, and benchmarked startup latency, memory overhead, and isolation behavior against native process execution.

## What It Does

`linuc` can:

- run a command in new PID, mount, UTS, IPC, network, and optionally user namespaces
- apply cgroup v2 memory and CPU limits
- drop Linux capabilities before `exec()`
- supervise a command, restart it on failure, and forward `SIGTERM` / `SIGINT`
- benchmark container startup latency, memory overhead, and CPU isolation against native execution

## Project Layout

```text
src/main.cpp         CLI entrypoint
src/runtime.*        container creation, namespaces, exec flow
src/cgroup.*         cgroup v2 setup and stats
src/caps.*           capability dropping and no_new_privs
src/supervisor.*     restart loop, signal forwarding, lifecycle logs
bench/bench.cpp      startup / memory / CPU benchmarks
tests/test.sh        integration tests
```

## Quick Start on macOS

This project needs a Linux kernel. On macOS, the included setup script uses Lima.

```bash
bash setup.sh
limactl shell linuc-dev
cd /linuc
```

Build inside the VM:

```bash
make
```

## First Run

Run a tiny command once:

```bash
./build/linuc run -- /bin/sh -c 'echo hello-from-container; hostname; echo pid=$$'
```

Try resource limits:

```bash
sudo ./build/linuc run --memory 67108864 --cpu 50 -- /bin/sh -c 'echo limited; sleep 1'
```

Run under the supervisor:

```bash
./build/linuc supervise --max-restarts 3 -- /bin/sh -c 'echo start; exit 1'
```

Run benchmarks:

```bash
sudo ./build/linuc bench | jq .
```

## CLI

```bash
./build/linuc run [OPTIONS] -- <cmd> [args...]
./build/linuc supervise [OPTIONS] -- <cmd> [args...]
./build/linuc bench
```

Common options:

- `--id <name>` container ID
- `--hostname <host>` hostname inside the container
- `--memory <bytes>` cgroup memory limit
- `--cpu <pct>` cgroup CPU quota percent per core
- `--no-user-ns` disable the user namespace
- `-v` or `--verbose` enable debug logs

Supervisor option:

- `--max-restarts <n>` restart on non-zero exit up to `n` times

## How It Works

High-level flow for `linuc run`:

1. Create a cgroup and apply CPU / memory limits if available.
2. `clone()` the child into new namespaces.
3. Write UID / GID maps when using a user namespace.
4. Mount a fresh `/proc` and set the container hostname.
5. Drop capabilities and set `PR_SET_NO_NEW_PRIVS`.
6. `exec()` the target command.

High-level flow for `linuc supervise`:

1. Start a container through `Runtime`.
2. Poll for exit while listening for `SIGTERM` / `SIGINT`.
3. Forward those signals to the container init process.
4. Emit structured lifecycle events on stdout.
5. Restart on non-zero exit until `--max-restarts` is exhausted.

## Testing

Run the basic suite:

```bash
bash tests/test.sh ./build/linuc
```

Run the memory-limit test as root too:

```bash
sudo bash tests/test.sh ./build/linuc
```

The tests cover:

- basic command execution and exit code propagation
- PID namespace behavior
- hostname isolation
- user namespace UID mapping
- supervisor restart behavior
- supervisor signal forwarding
- memory limit enforcement

## Benchmark Coverage

The benchmark command compares container execution with native process execution for:

- startup latency
- memory overhead
- CPU isolation under a 25% quota

## Learning Order

If you are reading this project to learn from it, this order is the easiest:

1. `src/main.cpp`
2. `src/container.h`
3. `src/runtime.h`
4. `src/runtime.cpp`
5. `src/cgroup.cpp`
6. `src/supervisor.cpp`
7. `bench/bench.cpp`
