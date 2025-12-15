// ============================================================================
// caps.cpp — Linux capability management implementation
// ============================================================================
#include "caps.h"
#include "logger.h"

#include <cerrno>
#include <cstring>
#include <sstream>
#include <sys/prctl.h>

// ── Helpers ──────────────────────────────────────────────────────────────────

// Build an empty capability set (all caps cleared in all three sets).
static cap_t makeEmptyCaps() {
    cap_t caps = cap_init();
    if (!caps) {
        LOG_ERROR("caps", "cap_init failed: ", strerror(errno));
    }
    return caps;
}

// Apply a cap_t to the calling process and free it.
static bool applyAndFree(cap_t caps) {
    if (!caps) return false;
    int rc = cap_set_proc(caps);
    cap_free(caps);
    if (rc != 0) {
        LOG_ERROR("caps", "cap_set_proc failed: ", strerror(errno));
        return false;
    }
    return true;
}

// ── Public API ───────────────────────────────────────────────────────────────

bool dropAllCapabilities() {
    // Step 1 — Clear all bits in all three capability sets.
    cap_t caps = makeEmptyCaps();
    if (!applyAndFree(caps)) return false;

    // Step 2 — Set securebits so UID 0 inside the container is just a number.
    //   SECBIT_NOROOT        — root doesn't auto-grant capabilities
    //   SECBIT_NOROOT_LOCKED — above can't be reversed by this process
    //   SECBIT_NO_SETUID_FIXUP        — uid→0 transition doesn't gain caps
    //   SECBIT_NO_SETUID_FIXUP_LOCKED — above can't be reversed
    int secure_bits =
        SECBIT_NOROOT | SECBIT_NOROOT_LOCKED |
        SECBIT_NO_SETUID_FIXUP | SECBIT_NO_SETUID_FIXUP_LOCKED;

    if (prctl(PR_SET_SECUREBITS, secure_bits) != 0) {
        // Non-fatal: may fail in user namespaces depending on kernel version.
        LOG_WARN("caps", "PR_SET_SECUREBITS failed: ", strerror(errno),
                 " (non-fatal in user namespaces)");
    }

    LOG_DEBUG("caps", "all capabilities dropped");
    return true;
}

bool dropCapabilitiesExcept(const std::vector<cap_value_t>& keep) {
    cap_t caps = cap_get_proc();
    if (!caps) {
        LOG_ERROR("caps", "cap_get_proc failed: ", strerror(errno));
        return false;
    }

    // Clear every capability in all three sets first.
    if (cap_clear(caps) != 0) {
        cap_free(caps);
        LOG_ERROR("caps", "cap_clear failed: ", strerror(errno));
        return false;
    }

    // Then set only the kept caps in Permitted + Effective.
    if (!keep.empty()) {
        const cap_value_t* data = keep.data();
        int n = static_cast<int>(keep.size());
        cap_set_flag(caps, CAP_PERMITTED,   n, data, CAP_SET);
        cap_set_flag(caps, CAP_EFFECTIVE,   n, data, CAP_SET);
        cap_set_flag(caps, CAP_INHERITABLE, n, data, CAP_SET);
    }

    return applyAndFree(caps);
}

bool setNoNewPrivs() {
    // PR_SET_NO_NEW_PRIVS=1 means:
    //   - execve() cannot grant more privileges than the process already has
    //   - suid/sgid bits on executables are ignored
    //   - This bit is inherited across fork/exec and cannot be unset.
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        LOG_ERROR("caps", "PR_SET_NO_NEW_PRIVS failed: ", strerror(errno));
        return false;
    }
    LOG_DEBUG("caps", "no_new_privs set");
    return true;
}

std::string currentCapsString() {
    cap_t caps = cap_get_proc();
    if (!caps) return "(error reading caps)";
    char* text = cap_to_text(caps, nullptr);
    std::string result(text ? text : "(null)");
    cap_free(text);
    cap_free(caps);
    return result;
}
