#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "../BoardConfig.h"

class SerialLink {
public:
	using LineHandler = void (*)(void* context, const char* line, size_t length,
								 uint64_t nowMs);

	explicit SerialLink(Stream& port);

	void init();
	void setLineHandler(LineHandler handler, void* context);
	void tick(uint64_t nowMs);
	bool sendJson(const JsonDocument& document);
	bool sendError(const char* code, const char* message, uint64_t nowMs);

private:
	struct TxLine {
		char bytes[BoardConfig::SerialMaxLineBytes];
		size_t length = 0;
		size_t offset = 0;
	};

	void drainTx();
	void receive(uint64_t nowMs);

	Stream& port_;
	LineHandler handler_ = nullptr;
	void* context_ = nullptr;
	char rx_[BoardConfig::SerialMaxLineBytes] = {};
	size_t rxLength_ = 0;
	bool discarding_ = false;
	TxLine tx_[BoardConfig::SerialTxSlots] = {};
	size_t txHead_ = 0;
	size_t txCount_ = 0;
};