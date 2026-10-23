#pragma once

#include "BoardConfig.h"
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

private:
	SerialLink serialLink_;
#if OFFGRID_RADIO_DIAGNOSTICS
	RadioDriver radioDriver_;
#endif
	MockDevice device_;
	Commands commands_;
#if OFFGRID_SERIAL_NATIVE_USB
	bool wasConnected_ = false;
#endif
};