#pragma once

#include <stddef.h>
#include <stdint.h>

enum class DeviceState : uint8_t { Unprovisioned, Locked, Unlocked };
enum class UiPage : uint8_t { Home, Radio, Pairing, Count };
enum class UiEvent : uint8_t {
	Tx, Rx, MessageReceived, Delivered, Failed, Error, Pairing, Boot
};

struct UiSnapshot {
	uint64_t uptimeMs = 0;
	DeviceState state = DeviceState::Unprovisioned;
	UiPage page = UiPage::Home;
	bool mock = true;
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
	Off, On, Flash, BlinkSlow, BlinkFast, Double, Heartbeat, Pulse, Custom
};

struct LedPattern {
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