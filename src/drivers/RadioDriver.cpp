#include "RadioDriver.h"

#include <math.h>
#include <string.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "../BoardConfig.h"

RadioDriver::RadioDriver()
	: spi_(HSPI),
	  module_(BoardConfig::RadioNss, BoardConfig::RadioDio1, BoardConfig::RadioReset,
			  BoardConfig::RadioBusy, spi_, SPISettings(2000000, MSBFIRST, SPI_MODE0)),
	  radio_(&module_) {
}

bool RadioDriver::validConfig(const Config& configuration) {
	const float bandwidth = configuration.bandwidthKhz;
	return isfinite(configuration.frequencyMhz) && configuration.frequencyMhz >= 150 &&
		configuration.frequencyMhz <= 960 &&
		(bandwidth == 62.5f || bandwidth == 125 || bandwidth == 250 || bandwidth == 500) &&
		configuration.spreadingFactor >= 5 && configuration.spreadingFactor <= 12 &&
		configuration.codingRate >= 5 && configuration.codingRate <= 8 &&
		configuration.powerDbm >= -9 && configuration.powerDbm <= 22 &&
		configuration.preamble >= 6 && configuration.preamble <= 64;
}

bool RadioDriver::result(int16_t code) {
	lastError_ = code;
	if (code == RADIOLIB_ERR_NONE) return true;
	state_ = State::Error;
	ready_ = false;
	return false;
}

bool RadioDriver::init(const Config& configuration) {
	if (busy()) { lastError_ = Busy; return false; }
	if (!validConfig(configuration)) {
		lastError_ = RADIOLIB_ERR_INVALID_DATA_RATE;
		return false;
	}
	ready_ = false;
	detachInterrupt(digitalPinToInterrupt(BoardConfig::RadioDio1));
	receiveWanted_ = false;
	module_.spiConfig.timeout = 100;
	spi_.begin(BoardConfig::RadioSck, BoardConfig::RadioMiso, BoardConfig::RadioMosi, BoardConfig::RadioNss);
	if (!result(radio_.begin(configuration.frequencyMhz, configuration.bandwidthKhz,
		configuration.spreadingFactor, configuration.codingRate, configuration.syncWord,
		configuration.powerDbm, configuration.preamble, BoardConfig::RadioTcxoVoltage))) return false;
	if (!result(radio_.setDio2AsRfSwitch(true)) || !result(radio_.setCRC(2)) ||
		!result(radio_.explicitHeader()) || !result(radio_.autoLDRO()) ||
		!result(radio_.standby())) return false;
	configuration_ = configuration;
	ready_ = true;
	state_ = State::Standby;
	configPending_ = false;
	module_.spiConfig.timeout = BoardConfig::RadioSpiTimeoutMs;
	takeInterrupt();
	attachInterruptArg(digitalPinToInterrupt(BoardConfig::RadioDio1), onInterrupt, this, RISING);
	return true;
}

bool RadioDriver::applyConfig(const Config& configuration) {
	if (busy()) {
		lastError_ = Busy;
		return false;
	}
	if (!ready_ || !validConfig(configuration)) {
		lastError_ = RADIOLIB_ERR_INVALID_DATA_RATE;
		return false;
	}
	if (!result(radio_.standby())) return false;
	state_ = State::Standby;
	if (!result(radio_.setFrequency(configuration.frequencyMhz)) ||
		!result(radio_.setBandwidth(configuration.bandwidthKhz)) ||
		!result(radio_.setSpreadingFactor(configuration.spreadingFactor)) ||
		!result(radio_.setCodingRate(configuration.codingRate)) ||
		!result(radio_.setOutputPower(configuration.powerDbm)) ||
		!result(radio_.setPreambleLength(configuration.preamble)) ||
		!result(radio_.setSyncWord(configuration.syncWord, 0x44)) ||
		!result(radio_.autoLDRO())) return false;
	configuration_ = configuration;
	return !receiveWanted_ || resumeReceive();
}

bool RadioDriver::standby() {
	if (!ready_) return false;
	const bool aborted = busy();
	if (!result(radio_.standby())) return false;
	radio_.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
	receiveWanted_ = false;
	takeInterrupt();
	state_ = State::Standby;
	if (aborted) {
		Event event;
		event.type = EventType::Failed;
		event.error = Cancelled;
		event.timeMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000;
		++stats_.failures;
		emit(event);
	}
	return true;
}

void IRAM_ATTR RadioDriver::onInterrupt(void* context) {
	RadioDriver* driver = static_cast<RadioDriver*>(context);
	portENTER_CRITICAL_ISR(&driver->interruptMux_);
	driver->interrupted_ = true;
	portEXIT_CRITICAL_ISR(&driver->interruptMux_);
}

bool RadioDriver::takeInterrupt() {
	portENTER_CRITICAL(&interruptMux_);
	const bool interrupted = interrupted_;
	interrupted_ = false;
	portEXIT_CRITICAL(&interruptMux_);
	return interrupted;
}

void RadioDriver::emit(const Event& event) {
	const size_t limit = event.type == EventType::Received ? 3 : 4;
	if (eventCount_ >= limit) { ++stats_.droppedEvents; return; }
	events_[(eventHead_ + eventCount_) % 4] = event;
	++eventCount_;
}

bool RadioDriver::popEvent(Event& event) {
	if (eventCount_ == 0) return false;
	event = events_[eventHead_];
	eventHead_ = (eventHead_ + 1) % 4;
	--eventCount_;
	return true;
}

bool RadioDriver::resumeReceive() {
	takeInterrupt();
	if (!result(radio_.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF,
		RADIOLIB_IRQ_RX_DEFAULT_FLAGS, RADIOLIB_IRQ_RX_DEFAULT_FLAGS))) return false;
	state_ = State::Receiving;
	return true;
}

bool RadioDriver::startReceive() {
	if (!ready_) { lastError_ = NotReady; return false; }
	if (busy()) {
		lastError_ = Busy;
		return false;
	}
	receiveWanted_ = true;
	return resumeReceive();
}

bool RadioDriver::startTransmit(const uint8_t* bytes, size_t length, uint64_t nowMs,
								bool cadEnabled, uint8_t backoffSlots) {
	if (!ready_) { lastError_ = NotReady; return false; }
	if (busy() || configPending_ || eventCount_ == 4) {
		lastError_ = Busy;
		return false;
	}
	if (bytes == nullptr || length == 0 || length > sizeof(tx_) || backoffSlots > 32) {
		lastError_ = RADIOLIB_ERR_PACKET_TOO_LONG;
		return false;
	}
	if (!result(radio_.standby())) return false;
	memcpy(tx_, bytes, length);
	txLength_ = length;
	takeInterrupt();
	cadForTransmit_ = cadEnabled;
	backoffSlots_ = backoffSlots;
	channelDeadline_ = nowMs + BoardConfig::RadioChannelTimeoutMs;
	if (cadEnabled) {
		state_ = State::Backoff;
		const uint32_t slot = static_cast<uint32_t>(ceil(2 * pow(2.0, configuration_.spreadingFactor) / configuration_.bandwidthKhz));
		nextActionAt_ = nowMs + (backoffSlots_ == 0 ? 0 : esp_random() % (backoffSlots_ + 1)) * (slot + 10);
		return true;
	}
	return beginTransmit(nowMs);
}

bool RadioDriver::beginTransmit(uint64_t nowMs) {
	if (!result(radio_.standby())) return false;
	takeInterrupt();
	RadioModeConfig_t mode = {};
	mode.transmit.data = tx_;
	mode.transmit.len = txLength_;
	mode.transmit.addr = 0;
	if (!result(radio_.stageMode(RADIOLIB_RADIO_MODE_TX, &mode))) return false;
	const uint32_t timeoutMs = airtimeMs(configuration_, txLength_) + 2000;
	const uint32_t timeoutTicks = timeoutMs * 64;
	uint8_t timeout[3] = {static_cast<uint8_t>(timeoutTicks >> 16),
		static_cast<uint8_t>(timeoutTicks >> 8), static_cast<uint8_t>(timeoutTicks)};
	if (!result(module_.SPIwriteStream(RADIOLIB_SX126X_CMD_SET_TX, timeout, sizeof(timeout)))) return false;
	state_ = State::Transmitting;
	txStartedAt_ = nowMs;
	deadline_ = nowMs + timeoutMs;
	return true;
}

bool RadioDriver::busy() const {
	return state_ == State::Transmitting || state_ == State::Cad || state_ == State::Backoff;
}

bool RadioDriver::beginCad(uint64_t nowMs) {
	takeInterrupt();
	if (!result(radio_.startChannelScan())) return false;
	state_ = State::Cad;
	deadline_ = nowMs + static_cast<uint64_t>(ceil(6 * pow(2.0, configuration_.spreadingFactor) /
		configuration_.bandwidthKhz)) + 200;
	++stats_.cadScans;
	return true;
}

bool RadioDriver::startCad(uint64_t nowMs) {
	if (!ready_) { lastError_ = NotReady; return false; }
	if (busy() || configPending_ || eventCount_ == 4) { lastError_ = Busy; return false; }
	cadForTransmit_ = false;
	txLength_ = 0;
	return beginCad(nowMs);
}

bool RadioDriver::queueConfig(const Config& configuration) {
	if (!validConfig(configuration)) { lastError_ = RADIOLIB_ERR_INVALID_DATA_RATE; return false; }
	if (!ready_) { lastError_ = NotReady; return false; }
	pendingConfiguration_ = configuration;
	configPending_ = true;
	return true;
}

bool RadioDriver::configPending() const { return configPending_; }

bool RadioDriver::recover() {
	if (busy()) { lastError_ = Busy; return false; }
	const Config desired = configPending_ ? pendingConfiguration_ : configuration_;
	return init(desired);
}

void RadioDriver::failTransfer(int16_t error, uint64_t nowMs) {
	const bool wasReady = ready_;
	const int16_t stopped = radio_.standby();
	takeInterrupt();
	ready_ = wasReady && stopped == RADIOLIB_ERR_NONE;
	state_ = ready_ ? State::Standby : State::Error;
	++stats_.failures;
	if (error == RADIOLIB_ERR_TX_TIMEOUT || error == RADIOLIB_ERR_RX_TIMEOUT || error == ChannelBusy) ++stats_.timeouts;
	Event event;
	event.type = EventType::Failed;
	event.timeMs = nowMs;
	event.error = error;
	event.length = txLength_;
	emit(event);
	if (ready_ && receiveWanted_) resumeReceive();
	lastError_ = error;
}

void RadioDriver::tick(uint64_t nowMs) {
	if (!ready_) return;
	if (state_ == State::Backoff) {
		if (nowMs >= channelDeadline_) failTransfer(ChannelBusy, nowMs);
		else if (nowMs >= nextActionAt_ && !beginCad(nowMs)) failTransfer(lastError_, nowMs);
		return;
	}
	const bool interrupted = takeInterrupt();
	if (interrupted || digitalRead(BoardConfig::RadioDio1) == HIGH) {
		const uint32_t flags = radio_.getIrqFlags();
		if (!result(module_.SPIcheckStream())) {
			failTransfer(lastError_, nowMs);
			return;
		}
		if (state_ == State::Cad && (flags & (RADIOLIB_SX126X_IRQ_CAD_DONE | RADIOLIB_SX126X_IRQ_CAD_DETECTED))) {
			if (cadForTransmit_ && nowMs >= channelDeadline_) {
				failTransfer(ChannelBusy, nowMs);
				return;
			}
			const bool detected = (flags & RADIOLIB_SX126X_IRQ_CAD_DETECTED) != 0;
			if (!result(radio_.standby()) || !result(radio_.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL))) {
				failTransfer(lastError_, nowMs);
				return;
			}
			if (detected) ++stats_.cadBusy;
			if (cadForTransmit_) {
				if (detected) {
					state_ = State::Backoff;
					const uint32_t slot = static_cast<uint32_t>(ceil(2 * pow(2.0, configuration_.spreadingFactor) / configuration_.bandwidthKhz));
					nextActionAt_ = nowMs + (1 + esp_random() % (backoffSlots_ + 1)) * (slot + 10);
				} else if (!beginTransmit(nowMs)) failTransfer(lastError_, nowMs);
			} else {
				Event event;
				event.type = detected ? EventType::CadBusy : EventType::CadClear;
				event.timeMs = nowMs;
				emit(event);
				state_ = State::Standby;
				if (receiveWanted_) resumeReceive();
			}
			return;
		}
		if (state_ == State::Transmitting && (flags & RADIOLIB_SX126X_IRQ_TX_DONE)) {
			if (!result(radio_.finishTransmit())) {
				failTransfer(lastError_, nowMs);
				return;
			}
			Event event;
			event.type = EventType::Transmitted;
			event.timeMs = nowMs;
			event.length = txLength_;
			event.airtimeMs = static_cast<uint32_t>(nowMs - txStartedAt_);
			memcpy(event.bytes, tx_, txLength_);
			++stats_.transmitted;
			stats_.txAirtimeMs += nowMs - txStartedAt_;
			emit(event);
			state_ = State::Standby;
			if (receiveWanted_) resumeReceive();
		} else if (state_ == State::Receiving) {
			if (flags & (RADIOLIB_SX126X_IRQ_CRC_ERR | RADIOLIB_SX126X_IRQ_HEADER_ERR)) {
				++stats_.crcErrors;
				resumeReceive();
				return;
			}
			if (flags & RADIOLIB_SX126X_IRQ_RX_DONE) {
				Event event;
				event.type = EventType::Received;
				event.timeMs = nowMs;
				event.length = radio_.getPacketLength();
				event.rssi = radio_.getRSSI();
				event.snr = radio_.getSNR();
				const int16_t code = event.length == 0 || event.length > sizeof(event.bytes) ?
					RADIOLIB_ERR_PACKET_TOO_LONG : radio_.readData(event.bytes, event.length);
				if (code == RADIOLIB_ERR_NONE) {
					++stats_.received;
					stats_.lastRssi = event.rssi;
					stats_.lastSnr = event.snr;
					stats_.hasSignal = true;
					emit(event);
				} else {
					++stats_.crcErrors;
					lastError_ = code;
				}
				resumeReceive();
			} else if (flags & RADIOLIB_SX126X_IRQ_TIMEOUT) resumeReceive();
		} else radio_.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
	}
	if (busy() && state_ != State::Backoff && nowMs >= deadline_) {
		failTransfer(state_ == State::Transmitting ? RADIOLIB_ERR_TX_TIMEOUT : RADIOLIB_ERR_RX_TIMEOUT, nowMs);
	}
	if (ready_ && !busy() && configPending_ && eventCount_ < 4) {
		Event event;
		event.timeMs = nowMs;
		if (applyConfig(pendingConfiguration_)) {
			configPending_ = false;
			event.type = EventType::Configured;
		} else {
			event.type = EventType::Failed;
			event.error = lastError_;
		}
		emit(event);
	}
}

const RadioDriver::Stats& RadioDriver::stats() const { return stats_; }
void RadioDriver::resetStats() { stats_ = Stats{}; }

uint32_t RadioDriver::airtimeMs(const Config& configuration, size_t bytes) {
	if (!validConfig(configuration) || bytes > 255) return 0;
	const double sf = configuration.spreadingFactor;
	const double symbol = pow(2.0, sf) / configuration.bandwidthKhz;
	const double optimization = symbol >= 16 ? 1 : 0;
	const bool shortSpreadingFactor = configuration.spreadingFactor <= 6;
	const double payload = 8 + fmax(0, ceil((8.0 * bytes - 4 * sf + 36 + (shortSpreadingFactor ? 0 : 8)) /
		(4 * (sf - 2 * optimization))) * configuration.codingRate);
	return static_cast<uint32_t>(ceil((configuration.preamble + (shortSpreadingFactor ? 6.25 : 4.25) + payload) * symbol));
}

bool RadioDriver::ready() const { return ready_; }
RadioDriver::State RadioDriver::state() const { return state_; }
int16_t RadioDriver::lastError() const { return lastError_; }
const RadioDriver::Config& RadioDriver::config() const { return configuration_; }

const char* RadioDriver::stateName() const {
	switch (state_) {
		case State::Off: return "off";
		case State::Standby: return "standby";
		case State::Receiving: return "receiving";
		case State::Transmitting: return "transmitting";
		case State::Cad: return "cad";
		case State::Backoff: return "backoff";
		default: return "error";
	}
}