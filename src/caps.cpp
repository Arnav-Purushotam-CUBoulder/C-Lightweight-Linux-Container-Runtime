#include "caps.h"
#include "logger.h"

#include <cerrno>
#include <cstring>

static cap_t makeEmptyCaps() {
    cap_t caps = cap_init();
    if (!caps) {
        LOG_ERROR("caps", "cap_init failed: ", strerror(errno));
    }
    return caps;
}

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

bool dropAllCapabilities() {
    cap_t caps = makeEmptyCaps();
    if (!applyAndFree(caps)) return false;

    int secure_bits =
        SECBIT_NOROOT | SECBIT_NOROOT_LOCKED |
        SECBIT_NO_SETUID_FIXUP | SECBIT_NO_SETUID_FIXUP_LOCKED;

    if (prctl(PR_SET_SECUREBITS, secure_bits) != 0) {
        LOG_WARN("caps", "PR_SET_SECUREBITS failed: ", strerror(errno));
    }

    return true;
}

bool setNoNewPrivs() {
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        LOG_ERROR("caps", "PR_SET_NO_NEW_PRIVS failed: ", strerror(errno));
        return false;
    }
    return true;
}
