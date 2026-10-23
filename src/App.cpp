#include "App.h"

#include <Arduino.h>
#include <esp_timer.h>

#if !defined(OFFGRID_MOCK_DEVICE) || OFFGRID_MOCK_DEVICE != 1
#error "App currently uses the development mock backend, not production security."
#endif

namespace {
#if OFFGRID_SERIAL_NATIVE_USB
auto& hostPort = Serial;
#else
auto& hostPort = Serial0;
#endif
}

App::App() : serialLink_(hostPort),
#if OFFGRID_RADIO_DIAGNOSTICS
	device_(serialLink_, radioDriver_),
#else
	device_(serialLink_),
#endif
	commands_(serialLink_, MockDevice::handle, &device_) {
}

void App::init() {
#if !OFFGRID_SERIAL_NATIVE_USB
	hostPort.setRxBufferSize(4096);
	hostPort.setTxBufferSize(4096);
#endif
	hostPort.begin(BoardConfig::SerialBaud);
	serialLink_.init();
	commands_.init();
	device_.init();
#if !OFFGRID_SERIAL_NATIVE_USB
	device_.emitBoot(static_cast<uint64_t>(esp_timer_get_time()) / 1000);
#endif
}

void App::tick() {
	const uint64_t nowMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000;
	device_.tick(nowMs);
#if OFFGRID_SERIAL_NATIVE_USB
	if (!hostPort) {
		wasConnected_ = false;
		return;
	}
	if (!wasConnected_) {
		serialLink_.init();
		device_.emitBoot(nowMs);
		wasConnected_ = true;
	}
#endif
	serialLink_.tick(nowMs);
}