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

private:
	static uint8_t sample(const LedPattern& pattern, uint64_t elapsed, bool loop, bool& active);

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
	bool overrideActive_ = false;
	bool ready_ = false;
	uint8_t duty_ = 0;
};