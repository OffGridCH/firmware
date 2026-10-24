// In-memory VaultStore for host tests: same record API and slot bitmasks, no NVS or encryption.
// Reloading Contacts/Groups from the same store models a reboot.
#include "../../src/storage/VaultStore.h"

#include <map>
#include <string>
#include <string.h>
#include <vector>

namespace {
std::map<const VaultStore*, std::map<std::string, std::vector<uint8_t>>> stores;

bool read(const VaultStore* store, const std::string& name, uint8_t* output, size_t capacity, size_t& length) {
	length = 0;
	const auto record = stores[store].find(name);
	if (record == stores[store].end() || record->second.size() > capacity) return false;
	memcpy(output, record->second.data(), record->second.size());
	length = record->second.size();
	return true;
}

void write(const VaultStore* store, const std::string& name, const uint8_t* input, size_t length) {
	stores[store][name].assign(input, input + length);
}
}

bool VaultStore::load() {
	open_ = true;
	status_ = Status::Ready;
	return true;
}

void VaultStore::close() {
	open_ = false;
}

bool VaultStore::readContact(uint8_t slot, uint8_t* output, size_t capacity, size_t& length) {
	return open_ && (contacts_ & (uint32_t(1) << (slot - 1))) &&
		read(this, "c" + std::to_string(slot), output, capacity, length);
}

bool VaultStore::writeContact(uint8_t slot, const uint8_t* input, size_t length) {
	if (!open_ || slot < 1 || slot > 32) return false;
	write(this, "c" + std::to_string(slot), input, length);
	contacts_ |= uint32_t(1) << (slot - 1);
	return true;
}

bool VaultStore::deleteContact(uint8_t slot) {
	if (!open_ || slot < 1 || slot > 32) return false;
	contacts_ &= ~(uint32_t(1) << (slot - 1));
	return true;
}

bool VaultStore::readGroup(uint8_t slot, uint8_t* output, size_t capacity, size_t& length) {
	return open_ && (groups_ & (1 << (slot - 1))) && read(this, "g" + std::to_string(slot), output, capacity, length);
}

bool VaultStore::writeGroup(uint8_t slot, const uint8_t* input, size_t length) {
	if (!open_ || slot < 1 || slot > 8 || length > MaxPlaintext) return false;
	write(this, "g" + std::to_string(slot), input, length);
	groups_ |= 1 << (slot - 1);
	return true;
}

bool VaultStore::deleteGroup(uint8_t slot) {
	if (!open_ || slot < 1 || slot > 8) return false;
	groups_ &= ~(1 << (slot - 1));
	return true;
}

bool VaultStore::writeGroupCounter(uint32_t end) {
	if (!open_) return false;
	groupCounter_ = end;
	return true;
}
