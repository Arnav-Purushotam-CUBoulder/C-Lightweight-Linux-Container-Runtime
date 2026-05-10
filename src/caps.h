#pragma once

#include <sys/capability.h>
#include <sys/prctl.h>

bool dropAllCapabilities();
bool setNoNewPrivs();
