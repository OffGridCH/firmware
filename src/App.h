#pragma once

#include "BoardConfig.h"
#include "Types.h"
#include "drivers/LedDriver.h"
#include "drivers/OledDriver.h"
#include "host/Commands.h"
#include "host/MockDevice.h"
#include "host/SerialLink.h"

class App {
public:
	App();
	App(const App&) = delete;
	App& operator=(const App&) = delete;
	void init();
	void tick();
	const UiSnapshot& snapshot() const;

private:
	static void handleCommand(void* context, const char* command,
		JsonObjectConst arguments, JsonObject response, uint64_t nowMs);
	static void onUiEvent(void* context, UiEvent event, uint64_t nowMs);
	void updateUi(uint64_t nowMs);

	SerialLink serialLink_;
#if OFFGRID_RADIO_DIAGNOSTICS
	RadioDriver radioDriver_;
#endif
	MockDevice device_;
	Commands commands_;
	LedDriver ledDriver_;
	OledDriver oledDriver_;
	UiSnapshot snapshot_;
	LedConfig ledConfig_;
	bool ledReady_ = false;
	bool oledReady_ = false;
#if OFFGRID_SERIAL_NATIVE_USB
	bool wasConnected_ = false;
#endif
};