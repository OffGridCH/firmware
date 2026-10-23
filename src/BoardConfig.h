#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef OFFGRID_SERIAL_NATIVE_USB
#define OFFGRID_SERIAL_NATIVE_USB 0
#endif

namespace BoardConfig {
constexpr uint32_t SerialBaud = 115200;
constexpr size_t SerialMaxLineBytes = 2048;
constexpr size_t SerialTxSlots = 6;
constexpr size_t SerialRxBytesPerTick = 256;
constexpr size_t SerialTxBytesPerTick = 256;
constexpr size_t RequestDocumentBytes = 8192;
constexpr size_t ResponseDocumentBytes = 8192;
constexpr uint8_t JsonNestingLimit = 8;
}