#pragma once

#include <ArduinoJson.h>

#include "SerialLink.h"

class Commands {
public:
	using StateHandler = void (*)(void* context, const char* command,
								  JsonObjectConst arguments, JsonObject response,
								  uint64_t nowMs);

	Commands(SerialLink& link, StateHandler handler, void* context);
	void init();
	static void fail(JsonObject response, const char* code, const char* message,
					 const char* field = nullptr);

private:
	static void onLine(void* context, const char* line, size_t length, uint64_t nowMs);
	void processLine(const char* line, size_t length, uint64_t nowMs);

	SerialLink& link_;
	StateHandler handler_;
	void* context_;
	StaticJsonDocument<BoardConfig::RequestDocumentBytes> request_;
	StaticJsonDocument<BoardConfig::ResponseDocumentBytes> response_;
};