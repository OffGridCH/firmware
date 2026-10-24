// Minimal Arduino shim for native (host) tests. Time is controlled via g_fake_now.
#pragma once

#include <cstdint>

inline uint32_t g_fake_now = 0;

inline uint32_t millis() {
    return g_fake_now;
}
