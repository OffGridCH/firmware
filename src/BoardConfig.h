#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef OFFGRID_SERIAL_NATIVE_USB
#define OFFGRID_SERIAL_NATIVE_USB 0
#endif

#ifndef OFFGRID_RADIO_DIAGNOSTICS
#define OFFGRID_RADIO_DIAGNOSTICS 0
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
constexpr uint8_t RadioNss = 8;
constexpr uint8_t RadioSck = 9;
constexpr uint8_t RadioMosi = 10;
constexpr uint8_t RadioMiso = 11;
constexpr uint8_t RadioReset = 12;
constexpr uint8_t RadioBusy = 13;
constexpr uint8_t RadioDio1 = 14;
constexpr float RadioTcxoVoltage = 1.8f;
constexpr uint32_t RadioSpiTimeoutMs = 20;
constexpr uint32_t RadioChannelTimeoutMs = 10000;
}