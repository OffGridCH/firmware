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
	enum class EventType { Received, Transmitted, Failed, CadClear, CadBusy, Configured };
	static constexpr int16_t Busy = -10000;
	static constexpr int16_t NotReady = -10001;
	static constexpr int16_t Cancelled = -10002;
	static constexpr int16_t ChannelBusy = -10003;

	struct Event {
		EventType type = EventType::Failed;
		uint8_t bytes[255] = {};
		size_t length = 0;
		float rssi = 0;
		float snr = 0;
		uint64_t timeMs = 0;
		int16_t error = RADIOLIB_ERR_NONE;
	};

	struct Stats {
		uint32_t transmitted = 0;
		uint32_t received = 0;
		uint32_t crcErrors = 0;
		uint32_t failures = 0;
		uint32_t droppedEvents = 0;
		uint32_t cadScans = 0;
		uint32_t cadBusy = 0;
		uint32_t timeouts = 0;
		uint64_t txAirtimeMs = 0;
		float lastRssi = 0;
		float lastSnr = 0;
		bool hasSignal = false;
	};

	RadioDriver();
	RadioDriver(const RadioDriver&) = delete;
	RadioDriver& operator=(const RadioDriver&) = delete;

	bool init(const Config& configuration);
	bool applyConfig(const Config& configuration);
	bool standby();
	bool startReceive();
	bool startTransmit(const uint8_t* bytes, size_t length, uint64_t nowMs,
					   bool cadEnabled = true, uint8_t backoffSlots = 8);
	bool startCad(uint64_t nowMs);
	bool queueConfig(const Config& configuration);
	bool configPending() const;
	bool recover();
	void tick(uint64_t nowMs);
	bool popEvent(Event& event);
	const Stats& stats() const;
	void resetStats();
	bool ready() const;
	State state() const;
	const char* stateName() const;
	int16_t lastError() const;
	const Config& config() const;
	static bool validConfig(const Config& configuration);
	static uint32_t airtimeMs(const Config& configuration, size_t bytes);

private:
	bool result(int16_t code);
	static void IRAM_ATTR onInterrupt(void* context);
	bool takeInterrupt();
	bool resumeReceive();
	void emit(const Event& event);
	void failTransfer(int16_t error, uint64_t nowMs);
	bool beginTransmit(uint64_t nowMs);
	bool beginCad(uint64_t nowMs);
	bool busy() const;

	SPIClass spi_;
	Module module_;
	SX1262 radio_;
	Config configuration_;
	State state_ = State::Off;
	bool ready_ = false;
	int16_t lastError_ = RADIOLIB_ERR_NONE;
	portMUX_TYPE interruptMux_ = portMUX_INITIALIZER_UNLOCKED;
	volatile bool interrupted_ = false;
	bool receiveWanted_ = false;
	uint8_t tx_[255] = {};
	size_t txLength_ = 0;
	uint64_t txStartedAt_ = 0;
	uint64_t deadline_ = 0;
	Event events_[4] = {};
	size_t eventHead_ = 0;
	size_t eventCount_ = 0;
	Stats stats_;
	bool cadForTransmit_ = false;
	uint8_t backoffSlots_ = 8;
	uint64_t channelDeadline_ = 0;
	uint64_t nextActionAt_ = 0;
	Config pendingConfiguration_;
	bool configPending_ = false;
};