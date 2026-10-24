#pragma once

#include <Adafruit_NeoPixel.h>

#include "../BoardConfig.h"
#include "../Types.h"

class LedDriver {
public:
	bool init();
	void configure(const LedConfig& config);
	void show(const UiSnapshot& snapshot);
	void onEvent(UiEvent event, uint64_t nowMs);
	void preview(const LedPattern& pattern, uint64_t nowMs);
	void tick(uint64_t nowMs);
	uint8_t duty() const { return duty_; }
	uint64_t frameTime() const { return writtenAt_; }
	uint32_t pixel(size_t index) const { return pixels_[index]; }
	const char* pattern() const;
#if OFFGRID_RADIO_DIAGNOSTICS
	static bool selfTest();
#endif

private:
	static bool sample(const LedPattern& pattern, uint64_t elapsed, bool loop, uint32_t* pixels);

	Adafruit_NeoPixel strips_[2] = {
		{BoardConfig::StripPixels, BoardConfig::StripPins[0], NEO_GRB + NEO_KHZ800},
		{BoardConfig::StripPixels, BoardConfig::StripPins[1], NEO_GRB + NEO_KHZ800}
	};
	LedConfig config_;
	LedPattern overridePattern_;
	DeviceState state_ = DeviceState::Unprovisioned;
	uint64_t stateAt_ = 0;
	uint64_t overrideAt_ = 0;
	uint64_t writtenAt_ = 0;
	uint64_t bootAt_ = 0;
	uint64_t resetAt_ = 0;
	uint64_t pairingAt_ = 0;
	uint64_t selectedAt_ = 0;
	uint64_t transitionAt_ = 0;
	uint32_t pixels_[BoardConfig::StripPixels] = {};
	uint32_t from_[BoardConfig::StripPixels] = {};
	LedPatternKind selected_ = LedPatternKind::Off;
	bool booting_ = false;
	bool resetting_ = false;
	bool pairing_ = false;
	bool transition_ = true;
	bool smooth_ = false;
	bool overrideActive_ = false;
	bool ready_ = false;
	uint8_t duty_ = 0;
};