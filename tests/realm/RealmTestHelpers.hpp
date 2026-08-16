#pragma once

#include <cstdlib>

inline const char* realmTestHelperPath(const char* environment, const char* fallback) {
    const auto* value = std::getenv(environment);
    return value && *value ? value : fallback;
}
