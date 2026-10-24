#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

class ConfigStore {
public:
	enum class Status { Defaults, Loaded, Invalid, UnsupportedVersion, Unavailable };

	bool load();
	void copyTo(JsonDocument& configuration, uint32_t& lockTimeoutS) const;
	bool saveCategory(const char* category, JsonObjectConst configuration);
	bool savePolicy(uint32_t lockTimeoutS);
	bool eraseAll();

	Status status() const;
	const char* statusName() const;
	bool writable() const;
	int32_t lastError() const;
	static bool validPattern(JsonVariantConst pattern);
	static int32_t readVaultMetadata(uint8_t* output, size_t& length, const char* space = "og_cfg");
	static int32_t writeVaultMetadata(const uint8_t* input, size_t length, const char* space = "og_cfg");
	static int32_t eraseVaultMetadata(const char* space = "og_cfg");
	static int32_t resetPending(bool& pending);
	static int32_t beginReset();
	bool validCategory(const char* category, JsonObjectConst candidate);

private:
	static constexpr uint32_t SchemaVersion = 1;
	static constexpr size_t MaxRecordBytes = 8192;
	static constexpr size_t DocumentBytes = 16384;

	void defaults();
	static bool validRecord(JsonObjectConst record);
	bool persist();

	StaticJsonDocument<DocumentBytes> saved_;
	StaticJsonDocument<DocumentBytes> candidate_;
	char bytes_[MaxRecordBytes + 1] = {};
	Status status_ = Status::Unavailable;
	bool writable_ = false;
	int32_t lastError_ = 0;
};