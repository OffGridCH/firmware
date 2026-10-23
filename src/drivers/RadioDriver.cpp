#include "RadioDriver.h"

#include <math.h>

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
	if (!validConfig(configuration)) {
		lastError_ = RADIOLIB_ERR_INVALID_DATA_RATE;
		return false;
	}
	ready_ = false;
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
	return true;
}

bool RadioDriver::applyConfig(const Config& configuration) {
	if (!ready_ || !validConfig(configuration)) {
		lastError_ = RADIOLIB_ERR_INVALID_DATA_RATE;
		return false;
	}
	if (state_ != State::Standby && !standby()) return false;
	if (!result(radio_.setFrequency(configuration.frequencyMhz)) ||
		!result(radio_.setBandwidth(configuration.bandwidthKhz)) ||
		!result(radio_.setSpreadingFactor(configuration.spreadingFactor)) ||
		!result(radio_.setCodingRate(configuration.codingRate)) ||
		!result(radio_.setOutputPower(configuration.powerDbm)) ||
		!result(radio_.setPreambleLength(configuration.preamble)) ||
		!result(radio_.setSyncWord(configuration.syncWord, 0x44)) ||
		!result(radio_.autoLDRO())) return false;
	configuration_ = configuration;
	return true;
}

bool RadioDriver::standby() {
	if (!ready_) return false;
	if (!result(radio_.standby())) return false;
	state_ = State::Standby;
	return true;
}

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