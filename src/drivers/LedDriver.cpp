#include "LedDriver.h"

#include <Arduino.h>
#include <math.h>

bool LedDriver::init() {
	if (ledcSetup(BoardConfig::LedPwmChannel, 5000, 8) == 0) return false;
	ledcAttachPin(BoardConfig::OnboardLed, BoardConfig::LedPwmChannel);
	ledcWrite(BoardConfig::LedPwmChannel, 0);
	for (auto& strip : strips_) {
		if (strip.getPixels() == nullptr) return false;
		strip.begin();
		strip.clear();
		strip.show();
	}
	ready_ = true;
	return true;
}

void LedDriver::configure(const LedConfig& config) {
	config_ = config;
	overrideActive_ = false;
}

void LedDriver::show(const UiSnapshot& snapshot) {
	if (state_ == snapshot.state) return;
	state_ = snapshot.state;
	stateAt_ = snapshot.uptimeMs;
	overrideActive_ = false;
}

void LedDriver::onEvent(UiEvent event, uint64_t nowMs) {
	const size_t index = static_cast<size_t>(event);
	if (index < LedConfig::EventCount) preview(config_.eventPatterns[index], nowMs);
}

void LedDriver::preview(const LedPattern& pattern, uint64_t nowMs) {
	overridePattern_ = pattern;
	overrideAt_ = nowMs;
	overrideActive_ = true;
}

uint8_t LedDriver::sample(const LedPattern& pattern, uint64_t elapsed, bool loop, bool& active) {
	static const uint16_t flash[] = {60};
	static const uint16_t slow[] = {500, 500};
	static const uint16_t fast[] = {100, 100};
	static const uint16_t doubleFlash[] = {80, 80, 80, 760};
	static const uint16_t heartbeat[] = {100, 150, 100, 650};
	const uint16_t* steps = nullptr;
	size_t count = 0;
	uint32_t period = 0;
	switch (pattern.kind) {
		case LedPatternKind::Flash: steps = flash; count = 1; break;
		case LedPatternKind::BlinkSlow: steps = slow; count = 2; break;
		case LedPatternKind::BlinkFast: steps = fast; count = 2; break;
		case LedPatternKind::Double: steps = doubleFlash; count = 4; break;
		case LedPatternKind::Heartbeat: steps = heartbeat; count = 4; break;
		case LedPatternKind::Custom: steps = pattern.stepsMs; count = pattern.stepCount; break;
		case LedPatternKind::Pulse: period = 1500; break;
		default: period = 1000; break;
	}
	for (size_t index = 0; index < count; ++index) period += steps[index];
	active = period != 0 && (loop || elapsed < static_cast<uint64_t>(period) * pattern.repeat);
	if (!active) return 0;
	uint32_t phase = elapsed % period;
	if (pattern.kind == LedPatternKind::Off) return 0;
	if (pattern.kind == LedPatternKind::On) return 255;
	if (pattern.kind == LedPatternKind::Pulse) {
		return static_cast<uint8_t>((1 - cosf(phase * (2 * PI / 1500))) * 127.5f + 0.5f);
	}
	for (size_t index = 0; index < count; ++index) {
		if (phase < steps[index]) return (index % 2 == 0) ? 255 : 0;
		phase -= steps[index];
	}
	return 0;
}

void LedDriver::tick(uint64_t nowMs) {
	if (!ready_ || nowMs - writtenAt_ < 10) return;
	bool active = false;
	uint8_t level = 0;
	if (overrideActive_) {
		level = sample(overridePattern_, nowMs - overrideAt_, false, active);
		overrideActive_ = active;
	}
	if (!overrideActive_) {
		level = sample(config_.statePatterns[static_cast<size_t>(state_)],
			nowMs - stateAt_, true, active);
	}
	level = config_.enabled && !config_.quiet ?
		(static_cast<uint16_t>(level) * config_.brightness + 127) / 255 : 0;
	if (level == duty_) return;
	for (auto& strip : strips_) if (!strip.canShow()) return;
	duty_ = level;
	writtenAt_ = nowMs;
	ledcWrite(BoardConfig::LedPwmChannel, level);
	for (auto& strip : strips_) {
		strip.fill(strip.Color(level, level, level));
		strip.show();
	}
}