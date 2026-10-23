#pragma once

#include "../Types.h"

class OledDriver {
public:
	bool init();
	void show(const UiSnapshot& snapshot);
	void onEvent(UiEvent event, uint64_t nowMs);
	void tick(uint64_t nowMs);
};