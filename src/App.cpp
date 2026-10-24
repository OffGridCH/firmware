#include "App.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <string.h>
#include "security/CryptoEngine.h"

namespace {
#if OFFGRID_SERIAL_NATIVE_USB
auto& hostPort = Serial;
#else
auto& hostPort = Serial0;
#endif
}

App::App() : serialLink_(hostPort), commands_(serialLink_, handleCommand, this) {
}

void App::init() {
	CryptoEngine::init();
#if !OFFGRID_SERIAL_NATIVE_USB
	hostPort.setRxBufferSize(4096);
	hostPort.setTxBufferSize(4096);
#endif
	hostPort.begin(BoardConfig::SerialBaud);
	serialLink_.init();
	commands_.init();
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
	security_.setHandlers(onLoad, onLock, this);
	commands_.initSecure(configStore_, security_);
	if (!mesh_.init(security_.state() != DeviceState::Unprovisioned))
		serialLink_.sendError("E_INTERNAL", "mesh random-key initialization failed", 0);
	mesh_.configure(commands_.secureMeshConfig());
	mesh_.setEventHandler(onMeshEvent, this);
	if (security_.state() != DeviceState::Unprovisioned)
		radioDriver_.init(commands_.secureRadioConfig());
	if (mesh_.radioWanted() && radioDriver_.ready() && !radioDriver_.startReceive())
		serialLink_.sendError("E_RADIO", "locked relay receive failed", 0);
	if (!configStore_.writable() || security_.error() == Security::Error::Storage)
		serialLink_.sendError("E_INTERNAL", "storage unavailable; secure operations may be blocked", 0);
	ledReady_ = ledDriver_.init();
	oledReady_ = oledDriver_.init();
	const uint64_t nowMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000;
	updateUi(nowMs);
	onUiEvent(this, UiEvent::Boot, nowMs);
#if !OFFGRID_SERIAL_NATIVE_USB
	commands_.emitSecureBoot(security_, nowMs);
#endif
}

void App::tick() {
	const uint64_t nowMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000;
	security_.tick(nowMs);
	pairing_.tick(nowMs);
	mesh_.tick(nowMs);
	radioDriver_.tick(nowMs);
	RadioDriver::Event event;
	for (size_t index = 0; index < 4 && radioDriver_.popEvent(event); ++index) {
		if (event.type == RadioDriver::EventType::Failed) {
			mesh_.transmissionComplete(false, event.airtimeMs, nowMs);
			onUiEvent(this, UiEvent::Error, nowMs);
			serialLink_.sendError("E_RADIO", "radio operation failed", nowMs);
		} else if (event.type == RadioDriver::EventType::Received) {
			onUiEvent(this, UiEvent::Rx, nowMs);
			mesh_.receive(event.bytes, event.length, event.rssi, event.snr, nowMs);
		} else if (event.type == RadioDriver::EventType::Transmitted)
			mesh_.transmissionComplete(true, event.airtimeMs, nowMs);
		commands_.traceRadio(event, security_.state() == DeviceState::Unlocked, mesh_.receiveAction());
	}
	if (mesh_.takeStorageFailure()) {
		security_.lock("error", nowMs);
		onUiEvent(this, UiEvent::Error, nowMs);
		serialLink_.sendError("E_INTERNAL", "contact counter/replay persistence failed; locked", nowMs);
	}
	if (mesh_.takeCryptoFailure()) {
		security_.lock("error", nowMs);
		if (radioDriver_.ready()) radioDriver_.standby();
		serialLink_.sendError("E_INTERNAL", "mesh cryptography failed; stopped", nowMs);
	}
	if (!radioDriver_.ready()) mesh_.radioUnavailable(nowMs);
	if (security_.eventReason() && serialLink_.availableTxSlots() > 0) {
		StaticJsonDocument<384> event;
		event["evt"] = "sec.state"; event["t"] = nowMs;
		event["data"]["state"] = security_.state() == DeviceState::Unlocked ? "unlocked" : "locked";
		event["data"]["reason"] = security_.eventReason();
		if (serialLink_.sendJson(event)) security_.clearEvent();
	}
	if (!security_.eventReason() && serialLink_.availableTxSlots() > 0) {
		const MeshEngine::Event* event = mesh_.event();
		if (event && commands_.emitMeshEvent(*event)) mesh_.consumeEvent();
		const uint32_t drops = mesh_.stats().droppedEvents + mesh_.stats().droppedAcks +
			radioDriver_.stats().droppedEvents + commands_.traceDrops();
		if (drops > reportedDrops_ &&
			serialLink_.sendError("E_INTERNAL", "radio/mesh event or ACK loss; inspect radio.stats", nowMs))
			reportedDrops_ = drops;
		else if (drops < reportedDrops_) reportedDrops_ = drops;
		commands_.emitNotices(nowMs);
	}
	serialLink_.setReceiveEnabled(security_.eventReason() == nullptr && action_ == Commands::Action::None);
#if OFFGRID_SERIAL_NATIVE_USB
	if (!hostPort) {
		wasConnected_ = false;
	} else {
		if (!wasConnected_) {
			serialLink_.init();
			commands_.emitSecureBoot(security_, nowMs);
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
	const bool canLaunch = !mesh_.takeAbort();
	if (!canLaunch && radioDriver_.ready()) {
		if (!radioDriver_.standby() ||
			(mesh_.radioWanted() && !radioDriver_.startReceive()))
			serialLink_.sendError("E_RADIO", "radio cancellation failed", nowMs);
	}
	if (canLaunch && mesh_.radioWanted() && action_ == Commands::Action::None &&
		(radioDriver_.state() == RadioDriver::State::Receiving || radioDriver_.state() == RadioDriver::State::Standby)) {
		PacketCodec::Packet packet;
		if (mesh_.prepareTransmission(packet, nowMs) &&
			!radioDriver_.startTransmit(packet.bytes, packet.length, nowMs, commands_.cadEnabled(), commands_.backoffSlots()))
			mesh_.transmissionRejected(radioDriver_.lastError() == RadioDriver::Busy, nowMs);
	}
	if (action_ != Commands::Action::None && serialLink_.availableTxSlots() == BoardConfig::SerialTxSlots &&
		(action_ != Commands::Action::Reset || nowMs - actionAt_ >= UiTiming::Reset)) {
		const Commands::Action action = action_;
		action_ = Commands::Action::None;
		security_.lock(action == Commands::Action::Reset ? "factory_reset" : "lock", nowMs);
		hostPort.flush();
		if (action == Commands::Action::Reboot || finishReset()) ESP.restart();
		else {
			onUiEvent(this, UiEvent::Error, nowMs);
			serialLink_.sendError("E_INTERNAL", "authorized reset incomplete; retry maintenance recovery", nowMs);
		}
	}
}

const UiSnapshot& App::snapshot() const {
	return snapshot_;
}

void App::handleCommand(void* context, const char* command,
	JsonObjectConst arguments, JsonObject response, uint64_t nowMs) {
	App& app = *static_cast<App*>(context);
	app.action_ = app.commands_.handleSecure(command, arguments, response, nowMs,
		app.security_, app.vaultStore_, app.configStore_, app.radioDriver_, app.contacts_, app.pairing_, app.mesh_, app.groups_);
	if (app.action_ == Commands::Action::Reset) {
		app.actionAt_ = nowMs;
		onUiEvent(context, UiEvent::FactoryReset, nowMs);
	}
	if (app.mesh_.takeStorageFailure()) app.security_.lock("error", nowMs);
	if ((strcmp(command, "sec.provision") == 0 || strcmp(command, "sec.unlock") == 0) && response["ok"].as<bool>())
		app.mesh_.open(app.security_.nodeId());
	if ((strcmp(command, "sec.provision") == 0 || strcmp(command, "sec.unlock") == 0) &&
		response["ok"].as<bool>() && !app.radioDriver_.ready()) {
		if (!app.radioDriver_.init(app.commands_.secureRadioConfig()) || !app.radioDriver_.startReceive()) {
			onUiEvent(context, UiEvent::Error, nowMs);
		}
		response["result"]["radio"]["running"] = app.radioDriver_.state() == RadioDriver::State::Receiving;
		response["result"]["radio"]["last_error"] = app.radioDriver_.lastError();
	}
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
		ui["oled_scene"] = app.oledDriver_.scene();
		ui["led_pattern"] = app.ledDriver_.pattern();
		ui["led_frame_ms"] = app.ledDriver_.frameTime();
		JsonArray pixels = ui.createNestedArray("strip_rgb");
		for (size_t pixel = 0; pixel < BoardConfig::StripPixels; ++pixel) pixels.add(app.ledDriver_.pixel(pixel));
	}
}

void App::onUiEvent(void* context, UiEvent event, uint64_t nowMs) {
	App& app = *static_cast<App*>(context);
	if (app.action_ == Commands::Action::Reset && event != UiEvent::FactoryReset) return;
	if (app.ledReady_) app.ledDriver_.onEvent(event, nowMs);
	if (app.oledReady_) app.oledDriver_.onEvent(event, nowMs);
}

void App::updateUi(uint64_t nowMs) {
	commands_.secureSnapshot(snapshot_, nowMs, security_, radioDriver_, contacts_, pairing_);
	snapshot_.pendingMessages = mesh_.count();
	snapshot_.relayOnly = mesh_.radioWanted() && security_.state() != DeviceState::Unlocked;
	if (commands_.takeSecureLedConfig(ledConfig_) && ledReady_) ledDriver_.configure(ledConfig_);
	LedPattern preview;
	if (commands_.takeSecureLedPreview(preview) && ledReady_) ledDriver_.preview(preview, nowMs);
	if (ledReady_) ledDriver_.show(snapshot_);
	if (oledReady_) oledDriver_.show(snapshot_);
}

void App::onLock(void* context) {
	App& app = *static_cast<App*>(context);
	app.mesh_.close(static_cast<uint64_t>(esp_timer_get_time()) / 1000);
	app.mesh_.takeAbort();
	app.commands_.clearDebug();
	if (app.radioDriver_.ready()) app.radioDriver_.standby();
	if (app.mesh_.radioWanted() && app.radioDriver_.ready() && !app.radioDriver_.startReceive())
		app.serialLink_.sendError("E_RADIO", "locked relay receive failed", 0);
	app.pairing_.cancel();
	app.pairing_.clearFingerprint();
	app.contacts_.close();
	app.groups_.close();
}

void App::onMeshEvent(void* context, const MeshEngine::Event& event) {
	if (event.type == MeshEngine::EventType::Received) onUiEvent(context, UiEvent::MessageReceived, event.timeMs);
	else if (event.type == MeshEngine::EventType::Status) {
		if (event.message.state == MeshEngine::State::Sent) onUiEvent(context, UiEvent::Tx, event.timeMs);
		else if (event.message.state == MeshEngine::State::Delivered) onUiEvent(context, UiEvent::Delivered, event.timeMs);
		else if (event.message.state == MeshEngine::State::Failed) onUiEvent(context, UiEvent::Failed, event.timeMs);
	}
}

bool App::onLoad(void* context) {
	App& app = *static_cast<App*>(context);
	return app.mesh_.ready() && app.contacts_.load() && app.groups_.load();
}

bool App::finishReset() {
	return vaultStore_.erase() && configStore_.eraseAll();
}