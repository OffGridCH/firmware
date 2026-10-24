#pragma once

#include <stddef.h>
#include <stdint.h>

class VaultStore {
public:
	enum class Status { Missing, Ready, Corrupt, Unsupported, Unavailable, Incomplete };
	static constexpr size_t MaxPlaintext = 256;

	bool load();
	bool open(const uint8_t key[32]);
	bool verifyKey(const uint8_t key[32]);
	void close();
	bool create(const uint8_t key[32], const uint8_t salt[16],
		const uint8_t* identity, size_t length);
	bool rotate(const uint8_t key[32], const uint8_t salt[16]);
	bool readIdentity(uint8_t* output, size_t capacity, size_t& length);
	bool writeIdentity(const uint8_t* input, size_t length);
	bool readContact(uint8_t slot, uint8_t* output, size_t capacity, size_t& length);
	bool writeContact(uint8_t slot, const uint8_t* input, size_t length);
	bool deleteContact(uint8_t slot);
	bool erase();

	Status status() const { return status_; }
	const char* statusName() const;
	int32_t lastError() const { return error_; }
	const uint8_t* salt() const { return salt_; }
	uint32_t generation() const { return generation_; }
	uint32_t contacts() const { return contacts_; }
	bool isOpen() const { return open_; }

#if OFFGRID_RADIO_DIAGNOSTICS
	static bool selfTest(bool verify);
#endif

private:
	bool readRecord(uint8_t bank, uint32_t generation, const uint8_t key[32],
		const char* name, uint8_t* output, size_t capacity, size_t& length);
	bool writeRecord(uint8_t bank, uint32_t generation, const uint8_t key[32],
		const char* name, const uint8_t* input, size_t length);
	bool prepare(uint8_t bank);
	bool activate(uint8_t bank, uint32_t generation, const uint8_t salt[16],
		const uint8_t key[32], uint32_t contacts);
	bool validate(uint8_t bank, uint32_t generation, const uint8_t key[32], uint32_t& contacts);
	bool writeIndex(uint32_t contacts);
	static void contactName(uint8_t slot, char name[4]);

	const char* records_ = "og_vault";
	const char* metadata_ = "og_cfg";
	uint8_t key_[32] = {};
	uint8_t salt_[16] = {};
	uint32_t generation_ = 0;
	uint32_t contacts_ = 0;
	uint8_t bank_ = 0;
	bool open_ = false;
	Status status_ = Status::Unavailable;
	int32_t error_ = 0;
};