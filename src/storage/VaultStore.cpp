#include "VaultStore.h"
#include "ConfigStore.h"
#include "VaultStore.h"

#include "../security/CryptoEngine.h"
#include <nvs.h>
#include <nvs_flash.h>
#include <stdio.h>
#include <string.h>

namespace {
constexpr uint8_t Check[] = "offgrid-vault-v1";
constexpr size_t Header = 19;
constexpr size_t Tag = 16;

void put32(uint8_t* output, uint32_t value) {
	for (size_t index = 0; index < 4; ++index) output[index] = value >> (24 - index * 8);
}

uint32_t get32(const uint8_t* input) {
	return (uint32_t(input[0]) << 24) | (uint32_t(input[1]) << 16) |
		(uint32_t(input[2]) << 8) | input[3];
}

void physicalKey(uint8_t bank, const char* name, char output[16]) {
	snprintf(output, 16, "%c_%s", bank ? 'b' : 'a', name);
}

size_t associatedData(const char* space, const char* key, uint32_t generation, uint8_t output[48]) {
	size_t length = strlen(space) + 1;
	memcpy(output, space, length);
	const size_t keyLength = strlen(key) + 1;
	memcpy(output + length, key, keyLength);
	length += keyLength;
	put32(output + length, generation);
	return length + 4;
}
}

const char* VaultStore::statusName() const {
	switch (status_) {
		case Status::Missing: return "missing";
		case Status::Ready: return "ready";
		case Status::Corrupt: return "corrupt";
		case Status::Unsupported: return "unsupported_version";
		case Status::Incomplete: return "incomplete";
		default: return "unavailable";
	}
}

void VaultStore::close() {
	CryptoEngine::zeroize(key_, sizeof(key_));
	contacts_ = 0;
	open_ = false;
}

bool VaultStore::load() {
	close();
	generation_ = 0;
	bank_ = 0;
	memset(salt_, 0, sizeof(salt_));
	status_ = Status::Unavailable;
	error_ = nvs_flash_init();
	if (error_ != ESP_OK) return false;
	uint8_t metadata[26] = {};
	size_t length = sizeof(metadata);
	error_ = ConfigStore::readVaultMetadata(metadata, length, metadata_);
	if (error_ == ESP_ERR_NVS_NOT_FOUND) {
		nvs_iterator_t iterator = nvs_entry_find("nvs", records_, NVS_TYPE_ANY);
		const bool exists = iterator != nullptr;
		nvs_release_iterator(iterator);
		status_ = exists ? Status::Incomplete : Status::Missing;
		error_ = exists ? ESP_ERR_INVALID_STATE : ESP_OK;
		return !exists;
	}
	if (error_ != ESP_OK) {
		if (error_ == ESP_ERR_NVS_INVALID_LENGTH || error_ == ESP_ERR_NVS_TYPE_MISMATCH)
			status_ = Status::Corrupt;
		return false;
	}
	if (length != sizeof(metadata) || memcmp(metadata, "OGVM", 4) != 0 ||
		metadata[5] > 1 || get32(metadata + 6) == 0) {
		status_ = Status::Corrupt;
		error_ = ESP_ERR_INVALID_ARG;
		return false;
	}
	if (metadata[4] != 1) {
		status_ = Status::Unsupported;
		error_ = ESP_ERR_NOT_SUPPORTED;
		return false;
	}
	bank_ = metadata[5];
	generation_ = get32(metadata + 6);
	memcpy(salt_, metadata + 10, sizeof(salt_));
	status_ = Status::Ready;
	return true;
}

bool VaultStore::readRecord(uint8_t bank, uint32_t generation, const uint8_t key[32],
	const char* name, uint8_t* output, size_t capacity, size_t& length) {
	length = 0;
	if (output == nullptr) { error_ = ESP_ERR_INVALID_ARG; return false; }
	CryptoEngine::zeroize(output, capacity);
	char physical[16];
	physicalKey(bank, name, physical);
	uint8_t record[Header + MaxPlaintext + Tag] = {};
	size_t bytes = sizeof(record);
	nvs_handle_t handle;
	error_ = nvs_open(records_, NVS_READONLY, &handle);
	if (error_ != ESP_OK) return false;
	error_ = nvs_get_blob(handle, physical, record, &bytes);
	nvs_close(handle);
	if (error_ != ESP_OK) return false;
	if (bytes < Header + Tag || memcmp(record, "OGVR", 4) != 0 || record[4] != 1) {
		error_ = ESP_ERR_INVALID_ARG;
		return false;
	}
	const size_t plaintext = (size_t(record[5]) << 8) | record[6];
	if (plaintext > capacity || plaintext > MaxPlaintext || bytes != Header + plaintext + Tag) {
		error_ = ESP_ERR_INVALID_SIZE;
		return false;
	}
	uint8_t aad[48];
	const size_t aadLength = associatedData(records_, physical, generation, aad);
	const bool ok = CryptoEngine::aes256GcmDecrypt(key, record + 7, aad, aadLength,
		record + Header, plaintext, record + Header + plaintext, output);
	CryptoEngine::zeroize(record, sizeof(record));
	if (!ok) {
		CryptoEngine::zeroize(output, capacity);
		error_ = ESP_ERR_INVALID_CRC;
		return false;
	}
	length = plaintext;
	return true;
}

bool VaultStore::writeRecord(uint8_t bank, uint32_t generation, const uint8_t key[32],
	const char* name, const uint8_t* input, size_t length) {
	if (input == nullptr || length > MaxPlaintext) { error_ = ESP_ERR_INVALID_ARG; return false; }
	uint8_t record[Header + MaxPlaintext + Tag] = {};
	memcpy(record, "OGVR", 4);
	record[4] = 1;
	record[5] = length >> 8;
	record[6] = length;
	char physical[16];
	physicalKey(bank, name, physical);
	uint8_t aad[48];
	const size_t aadLength = associatedData(records_, physical, generation, aad);
	if (!CryptoEngine::randomBytes(record + 7, 12) ||
		!CryptoEngine::aes256GcmEncrypt(key, record + 7, aad, aadLength,
			input, length, record + Header, record + Header + length)) {
		CryptoEngine::zeroize(record, sizeof(record));
		error_ = ESP_FAIL;
		return false;
	}
	nvs_handle_t handle;
	error_ = nvs_open(records_, NVS_READWRITE, &handle);
	if (error_ == ESP_OK) {
		error_ = nvs_set_blob(handle, physical, record, Header + length + Tag);
		if (error_ == ESP_OK) error_ = nvs_commit(handle);
		nvs_close(handle);
	}
	CryptoEngine::zeroize(record, sizeof(record));
	return error_ == ESP_OK;
}

void VaultStore::contactName(uint8_t slot, char name[4]) {
	snprintf(name, 4, "c%02u", slot);
}

bool VaultStore::validate(uint8_t bank, uint32_t generation, const uint8_t key[32], uint32_t& contacts) {
	uint8_t plaintext[MaxPlaintext] = {};
	size_t length = 0;
	bool ok = readRecord(bank, generation, key, "check", plaintext, sizeof(plaintext), length) &&
		length == sizeof(Check) && memcmp(plaintext, Check, length) == 0;
	if (ok) ok = readRecord(bank, generation, key, "identity", plaintext, sizeof(plaintext), length) &&
		length >= 34 && length <= 57 && plaintext[32] >= 1 &&
		plaintext[32] <= 24 && length == size_t(33 + plaintext[32]);
	if (ok) ok = readRecord(bank, generation, key, "index", plaintext, sizeof(plaintext), length) && length == 4;
	if (ok) {
		contacts = get32(plaintext);
		for (uint8_t slot = 1; ok && slot <= 32; ++slot) {
			if (!(contacts & (uint32_t(1) << (slot - 1)))) continue;
			char name[4];
			contactName(slot, name);
			ok = readRecord(bank, generation, key, name, plaintext, sizeof(plaintext), length);
		}
	}
	CryptoEngine::zeroize(plaintext, sizeof(plaintext));
	if (!ok && error_ == ESP_OK) error_ = ESP_ERR_INVALID_ARG;
	return ok;
}

bool VaultStore::open(const uint8_t key[32]) {
	close();
	if (status_ != Status::Ready) { error_ = ESP_ERR_INVALID_STATE; return false; }
	uint32_t contacts = 0;
	if (!validate(bank_, generation_, key, contacts)) return false;
	memcpy(key_, key, sizeof(key_));
	contacts_ = contacts;
	open_ = true;
	return true;
}

bool VaultStore::verifyKey(const uint8_t key[32]) {
	if (status_ != Status::Ready) { error_ = ESP_ERR_INVALID_STATE; return false; }
	uint32_t contacts = 0;
	return validate(bank_, generation_, key, contacts);
}

bool VaultStore::prepare(uint8_t bank) {
	nvs_handle_t handle;
	error_ = nvs_open(records_, NVS_READWRITE, &handle);
	if (error_ != ESP_OK) return false;
	for (uint8_t index = 0; index < 35; ++index) {
		char name[4], physical[16];
		const char* logical = index == 0 ? "check" : index == 1 ? "identity" : index == 2 ? "index" : name;
		if (index >= 3) contactName(index - 2, name);
		physicalKey(bank, logical, physical);
		error_ = nvs_erase_key(handle, physical);
		if (error_ == ESP_ERR_NVS_NOT_FOUND) error_ = ESP_OK;
		if (error_ != ESP_OK) break;
	}
	if (error_ == ESP_OK) error_ = nvs_commit(handle);
	nvs_close(handle);
	return error_ == ESP_OK;
}

bool VaultStore::activate(uint8_t bank, uint32_t generation, const uint8_t salt[16],
	const uint8_t key[32], uint32_t contacts) {
	uint32_t verified = 0;
	if (!validate(bank, generation, key, verified) || verified != contacts) return false;
	uint8_t metadata[26];
	memcpy(metadata, "OGVM", 4);
	metadata[4] = 1;
	metadata[5] = bank;
	put32(metadata + 6, generation);
	memcpy(metadata + 10, salt, 16);
	error_ = ConfigStore::writeVaultMetadata(metadata, sizeof(metadata), metadata_);
	if (error_ != ESP_OK) {
		close();
		status_ = Status::Unavailable;
		return false;
	}
	bank_ = bank;
	generation_ = generation;
	memcpy(salt_, salt, sizeof(salt_));
	memcpy(key_, key, sizeof(key_));
	contacts_ = contacts;
	open_ = true;
	status_ = Status::Ready;
	return true;
}

bool VaultStore::create(const uint8_t key[32], const uint8_t salt[16],
	const uint8_t* identity, size_t length) {
	if (status_ != Status::Missing) { error_ = ESP_ERR_INVALID_STATE; return false; }
	uint8_t index[4] = {};
	status_ = Status::Incomplete;
	if (!prepare(0) || !writeRecord(0, 1, key, "check", Check, sizeof(Check)) ||
		!writeRecord(0, 1, key, "identity", identity, length) ||
		!writeRecord(0, 1, key, "index", index, sizeof(index))) return false;
	return activate(0, 1, salt, key, 0);
}

bool VaultStore::rotate(const uint8_t key[32], const uint8_t salt[16]) {
	if (!open_ || generation_ == UINT32_MAX) { error_ = ESP_ERR_INVALID_STATE; return false; }
	const uint8_t bank = 1 - bank_;
	const uint32_t generation = generation_ + 1;
	if (!prepare(bank)) return false;
	uint8_t plaintext[MaxPlaintext] = {};
	size_t length = 0;
	bool ok = true;
	for (uint8_t index = 0; ok && index < 35; ++index) {
		char name[4];
		const char* logical = index == 0 ? "check" : index == 1 ? "identity" : index == 2 ? "index" : name;
		if (index >= 3) {
			const uint8_t slot = index - 2;
			if (!(contacts_ & (uint32_t(1) << (slot - 1)))) continue;
			contactName(slot, name);
		}
		ok = readRecord(bank_, generation_, key_, logical, plaintext, sizeof(plaintext), length) &&
			writeRecord(bank, generation, key, logical, plaintext, length);
		CryptoEngine::zeroize(plaintext, sizeof(plaintext));
	}
	return ok && activate(bank, generation, salt, key, contacts_);
}

bool VaultStore::readIdentity(uint8_t* output, size_t capacity, size_t& length) {
	if (!open_) {
		if (output) CryptoEngine::zeroize(output, capacity);
		length = 0; error_ = ESP_ERR_INVALID_STATE; return false;
	}
	return readRecord(bank_, generation_, key_, "identity", output, capacity, length);
}

bool VaultStore::writeIdentity(const uint8_t* input, size_t length) {
	if (!open_) { error_ = ESP_ERR_INVALID_STATE; return false; }
	return writeRecord(bank_, generation_, key_, "identity", input, length);
}

bool VaultStore::readContact(uint8_t slot, uint8_t* output, size_t capacity, size_t& length) {
	if (!open_ || slot < 1 || slot > 32 || !(contacts_ & (uint32_t(1) << (slot - 1)))) {
		if (output) CryptoEngine::zeroize(output, capacity);
		length = 0; error_ = ESP_ERR_NVS_NOT_FOUND; return false;
	}
	char name[4];
	contactName(slot, name);
	return readRecord(bank_, generation_, key_, name, output, capacity, length);
}

bool VaultStore::writeIndex(uint32_t contacts) {
	uint8_t index[4];
	put32(index, contacts);
	if (!writeRecord(bank_, generation_, key_, "index", index, sizeof(index))) return false;
	contacts_ = contacts;
	return true;
}

bool VaultStore::writeContact(uint8_t slot, const uint8_t* input, size_t length) {
	if (!open_ || slot < 1 || slot > 32) { error_ = ESP_ERR_INVALID_ARG; return false; }
	char name[4];
	contactName(slot, name);
	if (!writeRecord(bank_, generation_, key_, name, input, length)) return false;
	const uint32_t contacts = contacts_ | (uint32_t(1) << (slot - 1));
	return contacts == contacts_ || writeIndex(contacts);
}

bool VaultStore::deleteContact(uint8_t slot) {
	if (!open_ || slot < 1 || slot > 32) { error_ = ESP_ERR_INVALID_ARG; return false; }
	return writeIndex(contacts_ & ~(uint32_t(1) << (slot - 1)));
}

bool VaultStore::erase() {
	close();
	nvs_handle_t handle;
	error_ = nvs_open(records_, NVS_READWRITE, &handle);
	if (error_ == ESP_OK) {
		error_ = nvs_erase_all(handle);
		if (error_ == ESP_OK) error_ = nvs_commit(handle);
		nvs_close(handle);
	}
	if (error_ == ESP_OK) error_ = ConfigStore::eraseVaultMetadata(metadata_);
	if (error_ != ESP_OK) { status_ = Status::Unavailable; return false; }
	return load();
}

#if OFFGRID_RADIO_DIAGNOSTICS
bool VaultStore::selfTest(bool verify) {
	VaultStore store;
	store.records_ = "og_vtest";
	store.metadata_ = "og_vtest";
	uint8_t key[32], salt[16], identity[34] = {};
	memset(key, 0x53, sizeof(key));
	memset(salt, 0x71, sizeof(salt));
	identity[0] = 7; identity[32] = 1; identity[33] = 'T';
	bool ok = store.load();
	if (!verify) ok = store.erase() && store.create(key, salt, identity, sizeof(identity));
	else ok = ok && store.open(key);
	uint8_t output[MaxPlaintext];
	size_t length = 0;
	ok = ok && store.readIdentity(output, sizeof(output), length) && length == sizeof(identity) &&
		memcmp(output, identity, length) == 0;
	store.close();
	key[0] ^= 1;
	ok = ok && !store.open(key) && !store.isOpen();
	key[0] ^= 1;
	if (ok) {
		memset(output, 0xff, sizeof(output));
		ok = !store.readRecord(store.bank_, store.generation_ + 1, key, "identity",
			output, sizeof(output), length);
		for (uint8_t value : output) if (value != 0) ok = false;
	}
	if (ok) {
		nvs_handle_t handle;
		uint8_t record[Header + MaxPlaintext + Tag];
		size_t bytes = sizeof(record);
		char physical[16];
		physicalKey(store.bank_, "identity", physical);
		ok = nvs_open("og_vtest", NVS_READWRITE, &handle) == ESP_OK;
		if (ok) {
			ok = nvs_get_blob(handle, physical, record, &bytes) == ESP_OK;
			if (ok) {
				record[bytes - 1] ^= 1;
				ok = nvs_set_blob(handle, physical, record, bytes) == ESP_OK &&
					nvs_commit(handle) == ESP_OK;
				if (ok) {
					ok = !store.open(key) && !store.isOpen();
					record[bytes - 1] ^= 1;
					const bool restored = nvs_set_blob(handle, physical, record, bytes) == ESP_OK &&
						nvs_commit(handle) == ESP_OK;
					ok = ok && restored;
				}
			}
			nvs_close(handle);
		}
		CryptoEngine::zeroize(record, sizeof(record));
	}
	if (ok) {
		ok = store.open(key) &&
			!store.activate(1 - store.bank_, store.generation_ + 1, salt, key, 0) &&
			store.isOpen();
	}
	if (ok) {
		uint8_t replacement[32];
		memcpy(replacement, key, sizeof(replacement));
		replacement[0] ^= 1;
		const uint32_t generation = store.generation_;
		ok = store.writeContact(32, Check, sizeof(Check)) &&
			store.rotate(replacement, salt) && store.generation_ == generation + 1 &&
			store.readContact(32, output, sizeof(output), length) &&
			length == sizeof(Check) && memcmp(output, Check, length) == 0;
		store.close();
		ok = ok && !store.open(key) && store.open(replacement) &&
			store.deleteContact(32) && !store.readContact(32, output, sizeof(output), length) &&
			store.rotate(key, salt) && store.generation_ == generation + 2;
		CryptoEngine::zeroize(replacement, sizeof(replacement));
	}
	store.close();
	CryptoEngine::zeroize(key, sizeof(key));
	CryptoEngine::zeroize(identity, sizeof(identity));
	CryptoEngine::zeroize(output, sizeof(output));
	return ok;
}
#endif