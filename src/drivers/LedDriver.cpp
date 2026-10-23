#include "LedDriver.h"

bool LedDriver::init() {
	return false;
}

void LedDriver::configure(const LedConfig&) {
}

void LedDriver::show(const UiSnapshot&) {
}

void LedDriver::onEvent(UiEvent, uint64_t) {
}

void LedDriver::preview(const LedPattern&, uint64_t) {
}

void LedDriver::tick(uint64_t) {
}