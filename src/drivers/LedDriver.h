#pragma once

#include "../Types.h"

class LedDriver {
public:
	bool init();
	void configure(const LedConfig& config);
	void show(const UiSnapshot& snapshot);
	void onEvent(UiEvent event, uint64_t nowMs);
	void preview(const LedPattern& pattern, uint64_t nowMs);
	void tick(uint64_t nowMs);
};