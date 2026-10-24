#include "LedDriver.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

namespace {
float progress(uint64_t time, uint64_t start, uint64_t end) {
	if (time <= start) return 0;
	if (time >= end) return 1;
	const float x = float(time - start) / (end - start);
	return x * x * (3 - 2 * x);
}

float bloom(uint64_t time, uint32_t start, uint32_t end) {
	if (time <= start || time >= end) return 0;
	const float x = sinf(PI * float(time - start) / (end - start));
	return x * x;
}

uint32_t color(uint8_t red, uint8_t green, uint8_t blue, float amount) {
	amount = fmaxf(0, fminf(1, amount));
	const uint16_t level = uint16_t(amount * amount * 255 + 0.5f);
	return (uint32_t((red * level + 127) / 255) << 16) |
		(uint32_t((green * level + 127) / 255) << 8) | ((blue * level + 127) / 255);
}

uint32_t mix(uint32_t from, uint32_t to, float amount) {
	uint32_t result = 0;
	for (uint8_t shift : {0, 8, 16}) {
		const float a = (from >> shift) & 255, b = (to >> shift) & 255;
		result |= uint32_t(a + (b - a) * amount + 0.5f) << shift;
	}
	return result;
}
}

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
	transition_ = true;
}

void LedDriver::show(const UiSnapshot& snapshot) {
	if (state_ != snapshot.state) {
		state_ = snapshot.state;
		stateAt_ = snapshot.uptimeMs;
		overrideActive_ = false;
		booting_ = false;
	}
	if (pairing_ != snapshot.pairingActive) {
		pairing_ = snapshot.pairingActive;
		pairingAt_ = snapshot.uptimeMs;
	}
}

void LedDriver::onEvent(UiEvent event, uint64_t nowMs) {
	if (event == UiEvent::Boot) {
		bootAt_ = nowMs;
		booting_ = true;
	} else if (event == UiEvent::FactoryReset) {
		resetAt_ = nowMs;
		resetting_ = true;
		booting_ = false;
		overrideActive_ = false;
	} else if (event == UiEvent::Pairing) {
		booting_ = false;
		overrideActive_ = false;
	} else {
		const size_t index = static_cast<size_t>(event);
		if (event == UiEvent::Error) {
			resetting_ = false;
			booting_ = false;
		}
		if (index >= LedConfig::EventCount) return;
		if (booting_ && nowMs - bootAt_ < UiTiming::StartupEnd) return;
		const LedPattern& pattern = config_.eventPatterns[index];
		if (pattern.kind == LedPatternKind::Off ||
			(overrideActive_ && overridePattern_.kind == pattern.kind)) return;
		preview(pattern, nowMs);
	}
}

void LedDriver::preview(const LedPattern& pattern, uint64_t nowMs) {
	booting_ = false;
	overridePattern_ = pattern;
	overrideAt_ = nowMs;
	overrideActive_ = true;
	transition_ = true;
}

const char* LedDriver::pattern() const {
	return selected_ == LedPatternKind::Custom ? "custom" : LedPattern::Names[static_cast<size_t>(selected_)];
}

bool LedDriver::sample(const LedPattern& pattern, uint64_t elapsed, bool loop, uint32_t* pixels) {
	memset(pixels, 0, sizeof(uint32_t) * BoardConfig::StripPixels);
	static const uint16_t flash[] = {60}, slow[] = {500, 500}, fast[] = {100, 100};
	static const uint16_t doubleFlash[] = {80, 80, 80, 760}, heartbeat[] = {100, 150, 100, 650};
	const uint16_t* steps = nullptr;
	size_t count = 0;
	uint32_t period = 1000;
	switch (pattern.kind) {
		case LedPatternKind::Flash: steps = flash; count = 1; break;
		case LedPatternKind::BlinkSlow: steps = slow; count = 2; break;
		case LedPatternKind::BlinkFast: steps = fast; count = 2; break;
		case LedPatternKind::Double: steps = doubleFlash; count = 4; break;
		case LedPatternKind::Heartbeat: steps = heartbeat; count = 4; break;
		case LedPatternKind::Custom: steps = pattern.stepsMs; count = pattern.stepCount; break;
		case LedPatternKind::Pulse: period = 1500; break;
		case LedPatternKind::Glow: period = 5000; break;
		case LedPatternKind::Send:
		case LedPatternKind::Receive: period = 1600; break;
		case LedPatternKind::Pairing: period = 3600; break;
		case LedPatternKind::Startup: period = UiTiming::StartupEnd; break;
		case LedPatternKind::Reset: period = UiTiming::Reset; break;
		case LedPatternKind::Alert: period = 2000; break;
		default: break;
	}
	if (count) {
		period = 0;
		for (size_t index = 0; index < count; ++index) period += steps[index];
	}
	if (!period || (!loop && elapsed >= uint64_t(period) * pattern.repeat)) return false;
	const uint32_t phase = elapsed % period;
	float legacy = pattern.kind == LedPatternKind::On ? 1 : 0;
	if (pattern.kind == LedPatternKind::Pulse) legacy = (1 - cosf(phase * (2 * PI / period))) / 2;
	if (count) {
		uint32_t position = phase;
		for (size_t index = 0; index < count; ++index) {
			if (position < steps[index]) { legacy = index % 2 == 0 ? 1 : 0; break; }
			position -= steps[index];
		}
	}
	for (size_t pixel = 0; pixel < BoardConfig::StripPixels; ++pixel) {
		const float breath = (1 - cosf(2 * PI * phase / period - pixel * 0.45f)) / 2;
		switch (pattern.kind) {
			case LedPatternKind::Glow:
				pixels[pixel] = color(0, 110, 255, 0.22f + 0.55f * breath);
				break;
			case LedPatternKind::Send:
				pixels[pixel] = color(255, 255, 255, bloom(phase, pixel * 160, pixel * 160 + 850));
				break;
			case LedPatternKind::Receive: {
				const uint32_t start = (BoardConfig::StripPixels - 1 - pixel) * 160;
				pixels[pixel] = color(255, 255, 255, bloom(phase, start, start + 1100));
				break;
			}
			case LedPatternKind::Pairing:
				pixels[pixel] = color(160, 32, 255, 0.35f + 0.5f * breath);
				break;
			case LedPatternKind::Startup: {
				const float body = progress(phase, UiTiming::StartupCollapse, UiTiming::StartupGrow) *
					(1 - progress(phase, UiTiming::StartupEnd - 600, UiTiming::StartupEnd));
				const float amount = 0.45f * bloom(phase, pixel * 60, UiTiming::StartupGrow + pixel * 60) +
					body * (0.2f + 0.35f * bloom(phase, UiTiming::StartupRipple + pixel * 130,
						UiTiming::StartupSlide + pixel * 130) +
					0.25f * bloom(phase, UiTiming::StartupSlide + pixel * 120, UiTiming::StartupSlideEnd + 450 + pixel * 120) +
					0.75f * progress(phase, UiTiming::StartupFill + pixel * 180, UiTiming::StartupEnd - 1200 + pixel * 180));
				pixels[pixel] = color(0, 110, 255, amount);
				break;
			}
			case LedPatternKind::Reset:
			case LedPatternKind::Alert:
				pixels[pixel] = color(255, 0, 0, bloom(phase, 0, period));
				break;
			default: {
				const uint8_t level = uint8_t(legacy * 255 + 0.5f);
				pixels[pixel] = uint32_t(level) * 0x010101;
				break;
			}
		}
	}
	return true;
}

void LedDriver::tick(uint64_t nowMs) {
	if (!ready_ || nowMs - writtenAt_ < 10) return;
	uint32_t target[BoardConfig::StripPixels] = {};
	LedPattern chosen;
	uint64_t at = stateAt_;
	bool loop = true;
	if (resetting_) {
		chosen.kind = LedPatternKind::Reset;
		at = resetAt_;
		loop = false;
	} else if (booting_ && nowMs - bootAt_ < UiTiming::StartupEnd) {
		chosen.kind = LedPatternKind::Startup;
		at = bootAt_;
		loop = false;
	} else {
		booting_ = false;
		if (overrideActive_ && !sample(overridePattern_, nowMs - overrideAt_, false, target))
			overrideActive_ = false;
		if (overrideActive_) {
			chosen = overridePattern_; at = overrideAt_; loop = false;
		} else if (pairing_) {
			chosen = config_.eventPatterns[static_cast<size_t>(UiEvent::Pairing)]; at = pairingAt_;
		} else chosen = config_.statePatterns[static_cast<size_t>(state_)];
	}
	sample(chosen, nowMs - at, loop, target);
	if (transition_ || chosen.kind != selected_ || at != selectedAt_) {
		memcpy(from_, pixels_, sizeof(from_));
		transitionAt_ = nowMs;
		smooth_ = (chosen.kind >= LedPatternKind::Glow && chosen.kind < LedPatternKind::Custom) ||
			(chosen.kind == LedPatternKind::Off && !overrideActive_ &&
			 selected_ >= LedPatternKind::Glow && selected_ < LedPatternKind::Custom);
		selected_ = chosen.kind;
		selectedAt_ = at;
		transition_ = false;
	}
	uint8_t level = 0;
	for (size_t pixel = 0; pixel < BoardConfig::StripPixels; ++pixel) {
		uint32_t scaled = 0;
		for (uint8_t shift : {0, 8, 16})
			scaled |= (((target[pixel] >> shift & 255) * config_.brightness + 127) / 255) << shift;
		// Explicit legacy previews retain their exact step timing and output levels.
		target[pixel] = config_.enabled && !config_.quiet ?
			(smooth_ ? mix(from_[pixel], scaled, progress(nowMs, transitionAt_, transitionAt_ + 250)) : scaled) : 0;
		for (uint8_t shift : {0, 8, 16})
			if ((target[pixel] >> shift & 255) > level) level = target[pixel] >> shift & 255;
	}
	if (memcmp(target, pixels_, sizeof(pixels_)) == 0 && level == duty_) return;
	for (auto& strip : strips_) if (!strip.canShow()) return;
	memcpy(pixels_, target, sizeof(pixels_));
	duty_ = level;
	writtenAt_ = nowMs;
	ledcWrite(BoardConfig::LedPwmChannel, level);
	for (auto& strip : strips_) {
		for (size_t pixel = 0; pixel < BoardConfig::StripPixels; ++pixel) strip.setPixelColor(pixel, pixels_[pixel]);
		strip.show();
	}
}

#if OFFGRID_RADIO_DIAGNOSTICS
bool LedDriver::selfTest() {
	for (size_t kind = size_t(LedPatternKind::Glow); kind <= size_t(LedPatternKind::Alert); ++kind) {
		LedPattern pattern; pattern.kind = static_cast<LedPatternKind>(kind);
		uint32_t previous[BoardConfig::StripPixels] = {};
		bool first = true, lit = false, spatial = false;
		for (uint32_t time = 0; time <= UiTiming::StartupEnd; time += 10) {
			uint32_t pixels[BoardConfig::StripPixels];
			if (!sample(pattern, time, false, pixels)) break;
			for (size_t pixel = 0; pixel < BoardConfig::StripPixels; ++pixel) {
				lit |= pixels[pixel] != 0;
				spatial |= pixels[pixel] != pixels[0];
				for (uint8_t shift : {0, 8, 16})
					if (!first && abs(int(pixels[pixel] >> shift & 255) - int(previous[pixel] >> shift & 255)) > 24) return false;
				if ((pattern.kind == LedPatternKind::Reset || pattern.kind == LedPatternKind::Alert) &&
					(pixels[pixel] & 0xFFFF)) return false;
				if ((pattern.kind == LedPatternKind::Send || pattern.kind == LedPatternKind::Receive) &&
					((pixels[pixel] & 255) != (pixels[pixel] >> 8 & 255) ||
					 (pixels[pixel] & 255) != (pixels[pixel] >> 16))) return false;
			}
			memcpy(previous, pixels, sizeof(previous));
			first = false;
		}
		if (!lit || (pattern.kind <= LedPatternKind::Startup && !spatial)) return false;
	}
	uint32_t send[BoardConfig::StripPixels], receive[BoardConfig::StripPixels];
	LedPattern pattern; pattern.kind = LedPatternKind::Send;
	if (!sample(pattern, 300, false, send) || sample(pattern, 1600, false, receive)) return false;
	pattern.kind = LedPatternKind::Receive;
	if (!sample(pattern, 300, false, receive) || send[0] <= send[3] || receive[3] <= receive[0]) return false;
	pattern.kind = LedPatternKind::Startup;
	if (sample(pattern, UiTiming::StartupEnd, false, send)) return false;
	pattern.kind = LedPatternKind::Reset;
	if (sample(pattern, UiTiming::Reset, false, send)) return false;
	pattern.kind = LedPatternKind::On;
	if (!sample(pattern, 999, false, send) || send[0] != 0xFFFFFF || sample(pattern, 1000, false, send)) return false;
	return true;
}
#endif
