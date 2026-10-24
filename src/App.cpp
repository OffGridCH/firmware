#include "App.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <string.h>

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
	commands_(serialLink_, handleCommand, this) {
}

void App::init() {
#if !OFFGRID_SERIAL_NATIVE_USB
	hostPort.setRxBufferSize(4096);
	hostPort.setTxBufferSize(4096);
#endif
	hostPort.begin(BoardConfig::SerialBaud);
	serialLink_.init();
	commands_.init();
	device_.setUiHandler(onUiEvent, this);
	device_.init();
	ledReady_ = ledDriver_.init();
	oledReady_ = oledDriver_.init();
	const uint64_t nowMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000;
	updateUi(nowMs);
	onUiEvent(this, UiEvent::Boot, nowMs);
#if !OFFGRID_SERIAL_NATIVE_USB
	device_.emitBoot(nowMs);
#endif
}

void App::tick() {
	const uint64_t nowMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000;
	device_.tick(nowMs);
#if OFFGRID_SERIAL_NATIVE_USB
	if (!hostPort) {
		wasConnected_ = false;
	} else {
		if (!wasConnected_) {
			serialLink_.init();
			device_.emitBoot(nowMs);
			wasConnected_ = true;
		}
		serialLink_.tick(nowMs);
	}
#else
	serialLink_.tick(nowMs);
#endif
	updateUi(nowMs);
	if (ledReady_) ledDriver_.tick(nowMs);
	if (oledReady_) oledDriver_.tick(nowMs);
}

const UiSnapshot& App::snapshot() const {
	return snapshot_;
}

void App::handleCommand(void* context, const char* command,
	JsonObjectConst arguments, JsonObject response, uint64_t nowMs) {
	App& app = *static_cast<App*>(context);
	MockDevice::handle(&app.device_, command, arguments, response, nowMs);
	if (!response["ok"].as<bool>()) {
		onUiEvent(context, UiEvent::Error, nowMs);
		return;
	}
	if (strcmp(command, "pair.begin") == 0) onUiEvent(context, UiEvent::Pairing, nowMs);
	if (strcmp(command, "sys.status") == 0) {
		JsonObject ui = response["result"].createNestedObject("ui");
		ui["led_ready"] = app.ledReady_;
		ui["oled_ready"] = app.oledReady_;
		ui["page"] = static_cast<uint8_t>(UiPage::Home);
		ui["led_duty"] = app.ledDriver_.duty();
		ui["strip_pixels"] = BoardConfig::StripPixels;
	}
}

void App::onUiEvent(void* context, UiEvent event, uint64_t nowMs) {
	App& app = *static_cast<App*>(context);
	if (app.ledReady_) app.ledDriver_.onEvent(event, nowMs);
	if (app.oledReady_) app.oledDriver_.onEvent(event, nowMs);
}

void App::updateUi(uint64_t nowMs) {
	snapshot_ = device_.uiSnapshot(nowMs);
	if (device_.takeLedConfig(ledConfig_) && ledReady_) ledDriver_.configure(ledConfig_);
	LedPattern preview;
	if (device_.takeLedPreview(preview) && ledReady_) ledDriver_.preview(preview, nowMs);
	if (ledReady_) ledDriver_.show(snapshot_);
	if (oledReady_) oledDriver_.show(snapshot_);
}