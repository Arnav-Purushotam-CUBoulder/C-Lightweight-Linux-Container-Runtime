// ============================================================================
// caps.h — Linux capability management
// ============================================================================
// CONCEPT: Linux Capabilities
// ───────────────────────────
// Traditional Unix has two privilege classes: root (UID 0) and everyone else.
// Linux capabilities break root's power into ~40 fine-grained units so a
// process can hold only the privileges it actually needs.
//
// Examples of capabilities:
//   CAP_NET_ADMIN  — configure network interfaces
//   CAP_SYS_ADMIN  — a catch-all (mount, namespace creation, etc.)
//   CAP_CHOWN      — change file ownership
//   CAP_KILL       — send signals to any process
//
// Each process has three capability sets:
//   Permitted    — the ceiling; can't gain caps not in this set
//   Effective    — currently active; what the kernel checks on syscalls
//   Inheritable  — passed across exec() to child processes
//
// Container hardening strategy:
//   1. Keep only the minimum caps needed (we keep none for maximum isolation).
//   2. Set PR_SET_NO_NEW_PRIVS so exec() can never gain new caps/suid bits.
//   3. Set the "securebits" so UID=0 inside the container doesn't auto-grant
//      capabilities (important when using user namespaces).
// ============================================================================
#pragma once

#include <vector>
#include <string>
#include <sys/capability.h>   // libcap  (apt install libcap-dev)
#include <sys/prctl.h>

// Drop all capabilities — leaves the process completely unprivileged.
// Must be called inside the container (after pivot_root/chroot, before exec).
bool dropAllCapabilities();

// Drop all capabilities EXCEPT those listed.
// Pass CAP_* constants from <sys/capability.h>.
bool dropCapabilitiesExcept(const std::vector<cap_value_t>& keep);

// Lock down future exec() calls: no suid binaries can grant new privileges.
// This is a one-way door — cannot be reversed after set.
bool setNoNewPrivs();

// Return a human-readable string of the current effective capability set.
std::string currentCapsString();
