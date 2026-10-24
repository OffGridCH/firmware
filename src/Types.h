#pragma once

#include <stddef.h>
#include <stdint.h>

inline bool validUtf8(const uint8_t* bytes, size_t length) {
	for (size_t index = 0; index < length;) {
		const uint8_t lead = bytes[index++];
		if (lead <= 0x7f) continue;
		const size_t count = lead >= 0xc2 && lead <= 0xdf ? 1 :
			lead >= 0xe0 && lead <= 0xef ? 2 : lead >= 0xf0 && lead <= 0xf4 ? 3 : 0;
		if (!count || length - index < count) return false;
		const uint8_t second = bytes[index];
		if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0) ||
			(lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second >= 0x90)) return false;
		for (size_t offset = 0; offset < count; ++offset)
			if ((bytes[index++] & 0xc0) != 0x80) return false;
	}
	return true;
}

enum class DeviceState : uint8_t { Unprovisioned, Locked, Unlocked };
enum class UiPage : uint8_t { Home, Radio, Pairing, Count };
enum class UiEvent : uint8_t {
	Tx, Rx, MessageReceived, Delivered, Failed, Error, Pairing, Boot, FactoryReset
};

namespace UiTiming {
constexpr uint16_t StartupLine = 260;
constexpr uint16_t StartupCollapse = 420;
constexpr uint16_t StartupGrow = 900;
constexpr uint16_t StartupRipple = 950;
constexpr uint16_t StartupSlide = 2000;
constexpr uint16_t StartupSlideEnd = 2450;
constexpr uint16_t StartupFill = 4450;
constexpr uint16_t StartupEnd = 6600;
constexpr uint16_t Reset = 1800;
}

struct UiSnapshot {
	uint64_t uptimeMs = 0;
	DeviceState state = DeviceState::Unprovisioned;
	UiPage page = UiPage::Home;
	bool mock = false;
	bool radioDiagnostics = false;
	char deviceId[13] = {};
	char nodeId[5] = {};
	char name[25] = {};
	uint8_t contacts = 0;
	uint8_t pendingMessages = 0;
	bool radioRunning = false;
	bool relayOnly = false;
	int32_t radioError = 0;
	float frequencyMhz = 0;
	float bandwidthKhz = 0;
	uint8_t spreadingFactor = 0;
	uint32_t txPackets = 0;
	uint32_t rxPackets = 0;
	bool hasSignal = false;
	float rssi = 0;
	float snr = 0;
	bool pairingActive = false;
	uint16_t pairingRemainingS = 0;
	char fingerprint[15] = {};
};

enum class LedPatternKind : uint8_t {
	Off, On, Flash, BlinkSlow, BlinkFast, Double, Heartbeat, Pulse,
	Glow, Send, Receive, Pairing, Startup, Reset, Alert, Custom
};

struct LedPattern {
	inline static constexpr const char* Names[] = {
		"off", "on", "flash", "blink_slow", "blink_fast", "double", "heartbeat", "pulse",
		"glow", "send", "receive", "pairing", "startup", "reset", "alert"
	};
	LedPatternKind kind = LedPatternKind::Off;
	uint16_t stepsMs[16] = {};
	uint8_t stepCount = 0;
	uint8_t repeat = 1;
};

struct LedConfig {
	static constexpr size_t StateCount = 3;
	static constexpr size_t EventCount = 7;
	bool enabled = true;
	bool quiet = false;
	uint8_t brightness = 128;
	LedPattern statePatterns[StateCount];
	LedPattern eventPatterns[EventCount];
};