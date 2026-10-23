#include "Commands.h"

#include <string.h>
#include <mbedtls/base64.h>

namespace {
class RequestReader {
public:
	RequestReader(const char* bytes, size_t length) : bytes_(bytes), length_(length) {
	}

	int read() {
		return offset_ < length_ ? static_cast<unsigned char>(bytes_[offset_++]) : -1;
	}

	size_t readBytes(char* buffer, size_t count) {
		if (count > length_ - offset_) {
			count = length_ - offset_;
		}
		memcpy(buffer, bytes_ + offset_, count);
		offset_ += count;
		return count;
	}

	bool onlyWhitespaceRemains() {
		while (offset_ < length_) {
			const char value = bytes_[offset_++];
			if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
				return false;
			}
		}
		return true;
	}

private:
	const char* bytes_;
	size_t length_;
	size_t offset_ = 0;
};
}

Commands::Commands(SerialLink& link, StateHandler handler, void* context)
	: link_(link), handler_(handler), context_(context) {
}

void Commands::init() {
	link_.setLineHandler(onLine, this);
}

bool Commands::stringArg(JsonObjectConst args, JsonObject response, const char* field,
						 size_t minimum, size_t maximum, bool required) {
	if (!required && !args.containsKey(field)) return true;
	const JsonVariantConst value = args[field];
	const JsonString text = value.as<JsonString>();
	if (!value.is<const char*>() || text.size() < minimum || text.size() > maximum ||
		memchr(text.c_str(), '\0', text.size()) != nullptr) {
		fail(response, "E_INVALID_ARG", "expected a string within the allowed byte length", field);
		return false;
	}
	return true;
}

bool Commands::integerArg(JsonObjectConst args, JsonObject response, const char* field,
						  int64_t minimum, int64_t maximum, bool required) {
	if (!required && !args.containsKey(field)) return true;
	const JsonVariantConst value = args[field];
	if (!value.is<int64_t>() || value.as<int64_t>() < minimum || value.as<int64_t>() > maximum) {
		fail(response, "E_INVALID_ARG", "expected an integer within the allowed range", field);
		return false;
	}
	return true;
}

bool Commands::boolArg(JsonObjectConst args, JsonObject response, const char* field,
					   bool required) {
	if (!required && !args.containsKey(field)) return true;
	if (!args[field].is<bool>()) {
		fail(response, "E_INVALID_ARG", "expected a boolean", field);
		return false;
	}
	return true;
}

bool Commands::base64Arg(JsonObjectConst args, JsonObject response, const char* field,
						 size_t decodedBytes) {
	if (!stringArg(args, response, field, 1, 172)) return false;
	const JsonString text = args[field].as<JsonString>();
	uint8_t decoded[128];
	unsigned char canonical[173];
	size_t actual = 0;
	size_t encoded = 0;
	if (mbedtls_base64_decode(decoded, sizeof(decoded), &actual,
		 reinterpret_cast<const unsigned char*>(text.c_str()), text.size()) != 0 ||
		actual != decodedBytes ||
		mbedtls_base64_encode(canonical, sizeof(canonical), &encoded, decoded, actual) != 0 ||
		encoded != text.size() || memcmp(canonical, text.c_str(), encoded) != 0) {
		fail(response, "E_INVALID_ARG", "expected canonical Base64 with the specified decoded length", field);
		return false;
	}
	return true;
}

void Commands::fail(JsonObject response, const char* code, const char* message,
					const char* field) {
	response.remove("result");
	response["ok"] = false;
	JsonObject error = response.createNestedObject("error");
	error["code"] = code;
	error["message"] = message;
	if (field != nullptr) {
		error["field"] = field;
	}
}

void Commands::onLine(void* context, const char* line, size_t length, uint64_t nowMs) {
	static_cast<Commands*>(context)->processLine(line, length, nowMs);
}

void Commands::processLine(const char* line, size_t length, uint64_t nowMs) {
	response_.clear();
	JsonObject response = response_.to<JsonObject>();
	RequestReader reader(line, length);
	const DeserializationError error = deserializeJson(
		request_, reader, DeserializationOption::NestingLimit(BoardConfig::JsonNestingLimit));

	if (error || !reader.onlyWhitespaceRemains()) {
		fail(response, "E_PARSE", "line is not a supported JSON document");
	} else if (!request_.is<JsonObject>()) {
		fail(response, "E_INVALID_ARG", "request must be an object", "request");
	} else if (!request_["id"].is<int64_t>()) {
		fail(response, "E_INVALID_ARG", "id must be a signed 64-bit integer", "id");
	} else {
		response["id"] = request_["id"];
		if (!request_["cmd"].is<const char*>()) {
			fail(response, "E_INVALID_ARG", "cmd must be a string", "cmd");
		} else {
			const JsonString command = request_["cmd"].as<JsonString>();
			if (command.size() == 0 || memchr(command.c_str(), '\0', command.size()) != nullptr) {
				fail(response, "E_INVALID_ARG", "cmd must be a nonempty command name", "cmd");
			} else if (request_.containsKey("args") && !request_["args"].is<JsonObject>()) {
				fail(response, "E_INVALID_ARG", "args must be an object", "args");
			} else if (handler_ == nullptr) {
				fail(response, "E_INTERNAL", "device handler is unavailable");
			} else {
				response["ok"] = true;
				response.createNestedObject("result");
				handler_(context_, command.c_str(), request_["args"].as<JsonObjectConst>(),
						 response, nowMs);
			}
		}
	}

	if (response_.overflowed() || measureJson(response_) >= BoardConfig::SerialMaxLineBytes) {
		response_.clear();
		response = response_.to<JsonObject>();
		if (request_.is<JsonObject>() && request_["id"].is<int64_t>() && !error) {
			response["id"] = request_["id"];
		}
		fail(response, "E_INTERNAL", "response exceeds the configured capacity");
	}
	link_.sendJson(response_);
	request_.clear();
}