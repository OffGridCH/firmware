#pragma once

#include <RadioLib.h>
#include <SPI.h>

class RadioDriver {
public:
	struct Config {
		float frequencyMhz = 869.525f;
		float bandwidthKhz = 125.0f;
		uint8_t spreadingFactor = 9;
		uint8_t codingRate = 5;
		int8_t powerDbm = 14;
		uint16_t preamble = 8;
		uint8_t syncWord = 79;
	};

	enum class State { Off, Standby, Receiving, Transmitting, Cad, Backoff, Error };

	RadioDriver();
	RadioDriver(const RadioDriver&) = delete;
	RadioDriver& operator=(const RadioDriver&) = delete;

	bool init(const Config& configuration);
	bool applyConfig(const Config& configuration);
	bool standby();
	bool ready() const;
	State state() const;
	const char* stateName() const;
	int16_t lastError() const;
	const Config& config() const;
	static bool validConfig(const Config& configuration);
	static uint32_t airtimeMs(const Config& configuration, size_t bytes);

private:
	bool result(int16_t code);

	SPIClass spi_;
	Module module_;
	SX1262 radio_;
	Config configuration_;
	State state_ = State::Off;
	bool ready_ = false;
	int16_t lastError_ = RADIOLIB_ERR_NONE;
};