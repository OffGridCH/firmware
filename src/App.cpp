#include "App.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <string.h>

#if !defined(OFFGRID_MOCK_DEVICE)
#error "Select an explicit mock or secure firmware profile."
#endif

namespace {
#if OFFGRID_SERIAL_NATIVE_USB
auto& hostPort = Serial;
#else
auto& hostPort = Serial0;
#endif
}

App::App() : serialLink_(hostPort),
#if OFFGRID_MOCK_DEVICE
#if OFFGRID_RADIO_DIAGNOSTICS
	device_(serialLink_, radioDriver_),
#else
	device_(serialLink_),
#endif
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
#if OFFGRID_MOCK_DEVICE
	device_.setUiHandler(onUiEvent, this);
	device_.init();
#else
	configStore_.load();
	bool resetPending = false;
	const int32_t resetError = ConfigStore::resetPending(resetPending);
	if (resetError == 0 && resetPending && !finishReset())
		serialLink_.sendError("E_INTERNAL", "authorized reset recovery failed", 0);
	if (resetError != 0)
		serialLink_.sendError("E_INTERNAL", "reset recovery marker unreadable", 0);
	uint8_t deviceId[6];
	const uint64_t mac = ESP.getEfuseMac();
	for (size_t index = 0; index < 6; ++index) deviceId[index] = (mac >> (index * 8)) & 255;
	security_.init(deviceId);
	security_.setHandlers(nullptr, onLock, this);
	commands_.initSecure(configStore_, security_);
	if (security_.state() != DeviceState::Unprovisioned)
		radioDriver_.init(commands_.secureRadioConfig());
	if (!configStore_.writable() || security_.error() == Security::Error::Storage)
		serialLink_.sendError("E_INTERNAL", "storage unavailable; secure operations may be blocked", 0);
#endif
	ledReady_ = ledDriver_.init();
	oledReady_ = oledDriver_.init();
	const uint64_t nowMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000;
	updateUi(nowMs);
	onUiEvent(this, UiEvent::Boot, nowMs);
#if !OFFGRID_SERIAL_NATIVE_USB
#if OFFGRID_MOCK_DEVICE
	device_.emitBoot(nowMs);
#else
	commands_.emitSecureBoot(security_, nowMs);
#endif
#endif
}

void App::tick() {
	const uint64_t nowMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000;
#if OFFGRID_MOCK_DEVICE
	device_.tick(nowMs);
#else
	security_.tick(nowMs);
	radioDriver_.tick(nowMs);
	RadioDriver::Event event;
	for (size_t index = 0; index < 4 && radioDriver_.popEvent(event); ++index) {
		if (event.type == RadioDriver::EventType::Failed) {
			onUiEvent(this, UiEvent::Error, nowMs);
			serialLink_.sendError("E_RADIO", "radio operation failed", nowMs);
		} else if (event.type == RadioDriver::EventType::Received) onUiEvent(this, UiEvent::Rx, nowMs);
	}
	if (security_.eventReason() && serialLink_.availableTxSlots() > 0) {
		StaticJsonDocument<384> event;
		event["evt"] = "sec.state"; event["t"] = nowMs;
		event["data"]["state"] = security_.state() == DeviceState::Unlocked ? "unlocked" : "locked";
		event["data"]["reason"] = security_.eventReason();
		if (serialLink_.sendJson(event)) security_.clearEvent();
	}
	serialLink_.setReceiveEnabled(security_.eventReason() == nullptr && action_ == Commands::Action::None);
#endif
#if OFFGRID_SERIAL_NATIVE_USB
	if (!hostPort) {
		wasConnected_ = false;
	} else {
		if (!wasConnected_) {
			serialLink_.init();
#if OFFGRID_MOCK_DEVICE
			device_.emitBoot(nowMs);
#else
			commands_.emitSecureBoot(security_, nowMs);
#endif
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
#if !OFFGRID_MOCK_DEVICE
	if (action_ != Commands::Action::None && serialLink_.availableTxSlots() == BoardConfig::SerialTxSlots) {
		const Commands::Action action = action_;
		action_ = Commands::Action::None;
		security_.lock(action == Commands::Action::Reset ? "factory_reset" : "lock", nowMs);
		hostPort.flush();
		if (action == Commands::Action::Reboot || finishReset()) ESP.restart();
		else serialLink_.sendError("E_INTERNAL", "authorized reset incomplete; retry maintenance recovery", nowMs);
	}
#endif
}

const UiSnapshot& App::snapshot() const {
	return snapshot_;
}

void App::handleCommand(void* context, const char* command,
	JsonObjectConst arguments, JsonObject response, uint64_t nowMs) {
	App& app = *static_cast<App*>(context);
#if OFFGRID_MOCK_DEVICE
	MockDevice::handle(&app.device_, command, arguments, response, nowMs);
#else
	app.action_ = app.commands_.handleSecure(command, arguments, response, nowMs,
		app.security_, app.vaultStore_, app.configStore_, app.radioDriver_);
	if ((strcmp(command, "sec.provision") == 0 || strcmp(command, "sec.unlock") == 0) &&
		response["ok"].as<bool>() && !app.radioDriver_.ready()) {
		if (!app.radioDriver_.init(app.commands_.secureRadioConfig()) || !app.radioDriver_.startReceive()) {
			onUiEvent(context, UiEvent::Error, nowMs);
		}
		response["result"]["radio"]["running"] = app.radioDriver_.state() == RadioDriver::State::Receiving;
		response["result"]["radio"]["last_error"] = app.radioDriver_.lastError();
	}
#endif
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
#if OFFGRID_MOCK_DEVICE
	snapshot_ = device_.uiSnapshot(nowMs);
	if (device_.takeLedConfig(ledConfig_) && ledReady_) ledDriver_.configure(ledConfig_);
	LedPattern preview;
	if (device_.takeLedPreview(preview) && ledReady_) ledDriver_.preview(preview, nowMs);
#else
	commands_.secureSnapshot(snapshot_, nowMs, security_, radioDriver_);
	if (commands_.takeSecureLedConfig(ledConfig_) && ledReady_) ledDriver_.configure(ledConfig_);
	LedPattern preview;
	if (commands_.takeSecureLedPreview(preview) && ledReady_) ledDriver_.preview(preview, nowMs);
#endif
	if (ledReady_) ledDriver_.show(snapshot_);
	if (oledReady_) oledDriver_.show(snapshot_);
}

#if !OFFGRID_MOCK_DEVICE
void App::onLock(void* context) {
	App& app = *static_cast<App*>(context);
	if (app.radioDriver_.ready()) app.radioDriver_.standby();
}

bool App::finishReset() {
	return vaultStore_.erase() && configStore_.eraseAll();
}
#endif