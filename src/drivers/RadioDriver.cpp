#include "RadioDriver.h"

#include <math.h>
#include <string.h>

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
	if (state_ == State::Transmitting) { lastError_ = Busy; return false; }
	if (!validConfig(configuration)) {
		lastError_ = RADIOLIB_ERR_INVALID_DATA_RATE;
		return false;
	}
	ready_ = false;
	detachInterrupt(digitalPinToInterrupt(BoardConfig::RadioDio1));
	receiveWanted_ = false;
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
	takeInterrupt();
	attachInterruptArg(digitalPinToInterrupt(BoardConfig::RadioDio1), onInterrupt, this, RISING);
	return true;
}

bool RadioDriver::applyConfig(const Config& configuration) {
	if (state_ == State::Transmitting || state_ == State::Cad || state_ == State::Backoff) {
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
	const bool aborted = state_ == State::Transmitting;
	if (!result(radio_.standby())) return false;
	radio_.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
	receiveWanted_ = false;
	takeInterrupt();
	state_ = State::Standby;
	if (aborted) {
		Event event;
		event.type = EventType::Failed;
		event.error = Cancelled;
		event.timeMs = millis();
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
	if (state_ == State::Transmitting || state_ == State::Cad || state_ == State::Backoff) {
		lastError_ = Busy;
		return false;
	}
	receiveWanted_ = true;
	return resumeReceive();
}

bool RadioDriver::startTransmit(const uint8_t* bytes, size_t length, uint64_t nowMs) {
	if (!ready_) { lastError_ = NotReady; return false; }
	if (state_ == State::Transmitting || state_ == State::Cad || state_ == State::Backoff || eventCount_ == 4) {
		lastError_ = Busy;
		return false;
	}
	if (bytes == nullptr || length == 0 || length > sizeof(tx_)) {
		lastError_ = RADIOLIB_ERR_PACKET_TOO_LONG;
		return false;
	}
	if (!result(radio_.standby())) return false;
	memcpy(tx_, bytes, length);
	txLength_ = length;
	takeInterrupt();
	if (!result(radio_.startTransmit(tx_, txLength_))) return false;
	state_ = State::Transmitting;
	txStartedAt_ = nowMs;
	deadline_ = nowMs + airtimeMs(configuration_, length) + 2000;
	return true;
}

void RadioDriver::failTransfer(int16_t error, uint64_t nowMs) {
	radio_.standby();
	radio_.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
	takeInterrupt();
	state_ = State::Standby;
	++stats_.failures;
	Event event;
	event.type = EventType::Failed;
	event.timeMs = nowMs;
	event.error = error;
	event.length = txLength_;
	emit(event);
	if (receiveWanted_) resumeReceive();
	lastError_ = error;
}

void RadioDriver::tick(uint64_t nowMs) {
	if (!ready_) return;
	const bool interrupted = takeInterrupt();
	if (interrupted || digitalRead(BoardConfig::RadioDio1) == HIGH) {
		const uint32_t flags = radio_.getIrqFlags();
		if (state_ == State::Transmitting && (flags & RADIOLIB_SX126X_IRQ_TX_DONE)) {
			if (!result(radio_.finishTransmit())) {
				failTransfer(lastError_, nowMs);
				return;
			}
			Event event;
			event.type = EventType::Transmitted;
			event.timeMs = nowMs;
			event.length = txLength_;
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
	if (state_ == State::Transmitting && nowMs >= deadline_) failTransfer(RADIOLIB_ERR_TX_TIMEOUT, nowMs);
}

const RadioDriver::Stats& RadioDriver::stats() const { return stats_; }
void RadioDriver::resetStats() { stats_ = Stats{}; }

uint32_t RadioDriver::airtimeMs(const Config& configuration, size_t bytes) {
	if (!validConfig(configuration) || bytes > 255) return 0;
	const double sf = configuration.spreadingFactor;
	const double symbol = pow(2.0, sf) / configuration.bandwidthKhz;
	const double optimization = symbol >= 16 ? 1 : 0;
	const double payload = 8 + fmax(0, ceil((8.0 * bytes - 4 * sf + 44) /
		(4 * (sf - 2 * optimization))) * configuration.codingRate);
	return static_cast<uint32_t>(ceil((configuration.preamble + 4.25 + payload) * symbol));
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