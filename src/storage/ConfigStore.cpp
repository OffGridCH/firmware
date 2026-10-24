#include "ConfigStore.h"
#include "../Types.h"

#include <math.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>

int32_t ConfigStore::resetPending(bool& pending) {
	pending = false;
	nvs_handle_t handle;
	esp_err_t error = nvs_open("og_cfg", NVS_READONLY, &handle);
	if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
	if (error != ESP_OK) return error;
	uint8_t marker = 0;
	error = nvs_get_u8(handle, "reset", &marker);
	nvs_close(handle);
	if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
	if (error != ESP_OK) return error;
	if (marker != 1) return ESP_ERR_INVALID_ARG;
	pending = true;
	return ESP_OK;
}

int32_t ConfigStore::beginReset() {
	nvs_handle_t handle;
	esp_err_t error = nvs_open("og_cfg", NVS_READWRITE, &handle);
	if (error != ESP_OK) return error;
	error = nvs_set_u8(handle, "reset", 1);
	if (error == ESP_OK) error = nvs_commit(handle);
	nvs_close(handle);
	return error;
}

bool ConfigStore::validCategory(const char* category, JsonObjectConst candidate) {
	candidate_.set(saved_);
	candidate_[category].set(candidate);
	return !candidate_.overflowed() && validRecord(candidate_.as<JsonObjectConst>());
}

int32_t ConfigStore::readVaultMetadata(uint8_t* output, size_t& length, const char* space) {
	nvs_handle_t handle;
	esp_err_t error = nvs_open(space, NVS_READONLY, &handle);
	if (error != ESP_OK) return error;
	error = nvs_get_blob(handle, "vault", output, &length);
	nvs_close(handle);
	return error;
}

int32_t ConfigStore::writeVaultMetadata(const uint8_t* input, size_t length, const char* space) {
	nvs_handle_t handle;
	esp_err_t error = nvs_open(space, NVS_READWRITE, &handle);
	if (error != ESP_OK) return error;
	error = nvs_set_blob(handle, "vault", input, length);
	if (error == ESP_OK) error = nvs_commit(handle);
	nvs_close(handle);
	return error;
}

int32_t ConfigStore::eraseVaultMetadata(const char* space) {
	nvs_handle_t handle;
	esp_err_t error = nvs_open(space, NVS_READWRITE, &handle);
	if (error != ESP_OK) return error;
	error = nvs_erase_key(handle, "vault");
	if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
	if (error == ESP_OK) error = nvs_commit(handle);
	nvs_close(handle);
	return error;
}

void ConfigStore::defaults() {
	deserializeJson(saved_, R"json({
	  "schema":2,
	  "radio":{"freq_mhz":869.525,"bw_khz":125,"sf":9,"cr":5,"tx_power_dbm":14,"preamble":8,"sync_word":79},
	  "mesh":{"hop_default":3,"relay_enabled":true,"relay_when_locked":false,"cad_enabled":true,"backoff_slots":8,"retries":3,"retry_base_ms":0,"rate_limit_per_src":20,"ack_default":{"text":true,"reaction":false,"geo":false}},
	  "led":{"enabled":true,"quiet":false,"brightness":128,"state_patterns":{"unprovisioned":"glow","locked":"glow","unlocked":"off"},"event_patterns":{"tx":"send","rx":"off","msg_received":"receive","delivered":"off","failed":"alert","error":"alert","pairing":"pairing"}},
	  "security":{"lock_timeout_s":15}
	})json");
}

bool ConfigStore::validPattern(JsonVariantConst pattern) {
	if (pattern.is<const char*>()) {
		const JsonString value = pattern.as<JsonString>();
		if (strlen(value.c_str()) != value.size()) return false;
		for (const char* name : LedPattern::Names) if (strcmp(value.c_str(), name) == 0) return true;
		return false;
	}
	if (!pattern.is<JsonObjectConst>()) return false;
	const JsonArrayConst steps = pattern["steps"].as<JsonArrayConst>();
	if (steps.isNull() || steps.size() == 0 || steps.size() > 16) return false;
	for (JsonVariantConst step : steps) {
		if (!step.is<int>() || step.as<int>() < 10 || step.as<int>() > 10000) return false;
	}
	if (pattern.as<JsonObjectConst>().containsKey("repeat")) {
		if (!pattern["repeat"].is<int>() || pattern["repeat"].as<int>() < 1 ||
			pattern["repeat"].as<int>() > 255) return false;
	}
	return true;
}

bool ConfigStore::validRecord(JsonObjectConst record) {
	if (!record["schema"].is<uint32_t>() || record["schema"].as<uint32_t>() != SchemaVersion ||
		!record["radio"].is<JsonObjectConst>() || !record["mesh"].is<JsonObjectConst>() ||
		!record["led"].is<JsonObjectConst>() || !record["security"].is<JsonObjectConst>()) return false;

	const JsonObjectConst radio = record["radio"];
	if (!radio["freq_mhz"].is<double>() || !isfinite(radio["freq_mhz"].as<double>()) ||
		radio["freq_mhz"].as<double>() < 150 || radio["freq_mhz"].as<double>() > 960 ||
		!radio["bw_khz"].is<double>()) return false;
	const double bandwidth = radio["bw_khz"];
	if (bandwidth != 62.5 && bandwidth != 125 && bandwidth != 250 && bandwidth != 500) return false;

	struct Range { const char* category; const char* key; int minimum; int maximum; };
	const Range ranges[] = {
		{"radio", "sf", 5, 12}, {"radio", "cr", 5, 8}, {"radio", "tx_power_dbm", -9, 22},
		{"radio", "preamble", 6, 64}, {"radio", "sync_word", 0, 255},
		{"mesh", "hop_default", 0, 7}, {"mesh", "backoff_slots", 0, 32},
		{"mesh", "retries", 0, 3}, {"mesh", "retry_base_ms", 0, 60000},
		{"mesh", "rate_limit_per_src", 0, 255}, {"led", "brightness", 0, 255},
		{"security", "lock_timeout_s", 0, 3600}
	};
	for (const Range& range : ranges) {
		const JsonVariantConst value = record[range.category][range.key];
		if (!value.is<int>() || value.as<int>() < range.minimum || value.as<int>() > range.maximum) return false;
	}
	const char* meshFlags[] = {"relay_enabled", "relay_when_locked", "cad_enabled"};
	for (const char* key : meshFlags) if (!record["mesh"][key].is<bool>()) return false;
	if (!record["mesh"]["ack_default"].is<JsonObjectConst>()) return false;
	const char* types[] = {"text", "reaction", "geo"};
	for (const char* key : types) if (!record["mesh"]["ack_default"][key].is<bool>()) return false;
	if (!record["led"]["enabled"].is<bool>() || !record["led"]["quiet"].is<bool>() ||
		!record["led"]["state_patterns"].is<JsonObjectConst>() ||
		!record["led"]["event_patterns"].is<JsonObjectConst>()) return false;
	const char* states[] = {"unprovisioned", "locked", "unlocked"};
	const char* events[] = {"tx", "rx", "msg_received", "delivered", "failed", "error", "pairing"};
	for (const char* key : states) if (!validPattern(record["led"]["state_patterns"][key])) return false;
	for (const char* key : events) if (!validPattern(record["led"]["event_patterns"][key])) return false;
	return true;
}

bool ConfigStore::load() {
	defaults();
	writable_ = false;
	status_ = Status::Unavailable;
	lastError_ = nvs_flash_init();
	if (lastError_ != ESP_OK) return false;

	nvs_handle_t handle;
	lastError_ = nvs_open("og_cfg", NVS_READONLY, &handle);
	if (lastError_ == ESP_ERR_NVS_NOT_FOUND) {
		status_ = Status::Defaults;
		writable_ = true;
		lastError_ = ESP_OK;
		return true;
	}
	if (lastError_ != ESP_OK) return false;
	size_t length = 0;
	lastError_ = nvs_get_blob(handle, "settings", nullptr, &length);
	if (lastError_ == ESP_ERR_NVS_NOT_FOUND) {
		nvs_close(handle);
		status_ = Status::Defaults;
		writable_ = true;
		lastError_ = ESP_OK;
		return true;
	}
	if (lastError_ != ESP_OK || length == 0 || length > MaxRecordBytes) {
		nvs_close(handle);
		if (lastError_ == ESP_OK) {
			status_ = Status::Invalid;
			lastError_ = ESP_ERR_INVALID_SIZE;
		}
		return false;
	}
	lastError_ = nvs_get_blob(handle, "settings", bytes_, &length);
	nvs_close(handle);
	if (lastError_ != ESP_OK) return false;
	const DeserializationError error = deserializeJson(candidate_, static_cast<const char*>(bytes_), length,
													   DeserializationOption::NestingLimit(8));
	if (error || !candidate_.is<JsonObject>()) {
		status_ = Status::Invalid;
		lastError_ = ESP_ERR_INVALID_ARG;
		return false;
	}
	const bool upgrading = candidate_["schema"].is<uint32_t>() && candidate_["schema"].as<uint32_t>() == 1;
	if (upgrading) {
		struct Replacement { const char* group; const char* key; const char* oldName; const char* newName; };
		const Replacement replacements[] = {
			{"state_patterns", "unprovisioned", "pulse", "glow"}, {"state_patterns", "locked", "blink_slow", "glow"},
			{"event_patterns", "tx", "flash", "send"}, {"event_patterns", "rx", "flash", "off"},
			{"event_patterns", "msg_received", "double", "receive"}, {"event_patterns", "error", "blink_fast", "alert"},
			{"event_patterns", "pairing", "heartbeat", "pairing"}
		};
		for (const Replacement& replacement : replacements) {
			JsonVariant entry = candidate_["led"][replacement.group][replacement.key];
			if (entry.is<const char*>() && strcmp(entry, replacement.oldName) == 0) entry.set(replacement.newName);
		}
		const JsonVariantConst failed = candidate_["led"]["event_patterns"]["failed"];
		if (failed.is<JsonObjectConst>() && failed.as<JsonObjectConst>().size() == 2 &&
			failed["repeat"].is<int>() && failed["repeat"].as<int>() == 3 &&
			failed["steps"].is<JsonArrayConst>() && failed["steps"].size() == 2 &&
			failed["steps"][0].is<int>() && failed["steps"][0].as<int>() == 100 &&
			failed["steps"][1].is<int>() && failed["steps"][1].as<int>() == 100)
			candidate_["led"]["event_patterns"]["failed"] = "alert";
		candidate_["schema"] = SchemaVersion;
	}
	if (candidate_["schema"].is<uint32_t>() && candidate_["schema"].as<uint32_t>() != SchemaVersion) {
		status_ = Status::UnsupportedVersion;
		lastError_ = ESP_ERR_NOT_SUPPORTED;
		return false;
	}
	if (!validRecord(candidate_.as<JsonObjectConst>())) {
		status_ = Status::Invalid;
		lastError_ = ESP_ERR_INVALID_ARG;
		return false;
	}
	writable_ = true;
	if (upgrading && !persist()) {
		writable_ = false;
		status_ = Status::Unavailable;
		return false;
	}
	saved_.set(candidate_);
	status_ = Status::Loaded;
	return true;
}

void ConfigStore::copyTo(JsonDocument& configuration, uint32_t& lockTimeoutS) const {
	configuration.clear();
	configuration["radio"].set(saved_["radio"]);
	configuration["mesh"].set(saved_["mesh"]);
	configuration["led"].set(saved_["led"]);
	lockTimeoutS = saved_["security"]["lock_timeout_s"];
}

bool ConfigStore::persist() {
	if (!writable_) return false;
	if (candidate_.overflowed() || !validRecord(candidate_.as<JsonObjectConst>())) {
		lastError_ = ESP_ERR_INVALID_ARG;
		return false;
	}
	const size_t length = measureJson(candidate_);
	if (length > MaxRecordBytes) {
		lastError_ = ESP_ERR_INVALID_SIZE;
		return false;
	}
	const size_t written = serializeJson(candidate_, bytes_, sizeof(bytes_));
	if (written != length) {
		lastError_ = ESP_ERR_INVALID_SIZE;
		return false;
	}
	nvs_handle_t handle;
	lastError_ = nvs_open("og_cfg", NVS_READWRITE, &handle);
	if (lastError_ != ESP_OK) return false;
	lastError_ = nvs_set_blob(handle, "settings", bytes_, length);
	if (lastError_ == ESP_OK) lastError_ = nvs_commit(handle);
	nvs_close(handle);
	if (lastError_ != ESP_OK) return false;
	saved_.set(candidate_);
	status_ = Status::Loaded;
	return true;
}

bool ConfigStore::saveCategory(const char* category, JsonObjectConst configuration) {
	if (category == nullptr || (strcmp(category, "radio") != 0 && strcmp(category, "mesh") != 0 &&
							   strcmp(category, "led") != 0)) {
		lastError_ = ESP_ERR_INVALID_ARG;
		return false;
	}
	candidate_.set(saved_);
	candidate_[category].set(configuration);
	return persist();
}

bool ConfigStore::savePolicy(uint32_t lockTimeoutS) {
	candidate_.set(saved_);
	candidate_["security"]["lock_timeout_s"] = lockTimeoutS;
	return persist();
}

bool ConfigStore::eraseAll() {
	lastError_ = nvs_flash_init();
	if (lastError_ != ESP_OK) return false;
	nvs_handle_t handle;
	lastError_ = nvs_open("og_cfg", NVS_READWRITE, &handle);
	if (lastError_ != ESP_OK) return false;
	lastError_ = nvs_erase_all(handle);
	if (lastError_ == ESP_OK) lastError_ = nvs_commit(handle);
	nvs_close(handle);
	if (lastError_ != ESP_OK) return false;
	defaults();
	writable_ = true;
	status_ = Status::Defaults;
	return true;
}

ConfigStore::Status ConfigStore::status() const { return status_; }
bool ConfigStore::writable() const { return writable_; }
int32_t ConfigStore::lastError() const { return lastError_; }

const char* ConfigStore::statusName() const {
	switch (status_) {
		case Status::Defaults: return "defaults";
		case Status::Loaded: return "loaded";
		case Status::Invalid: return "invalid";
		case Status::UnsupportedVersion: return "unsupported_version";
		default: return "unavailable";
	}
}