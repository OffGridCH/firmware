#include "Contacts.h"
#include "Contacts.h"

#include "CryptoEngine.h"
#include "../Types.h"
#include <string.h>

namespace {
void put32(uint8_t* output, uint32_t value) {
	for (size_t index = 0; index < 4; ++index) output[index] = value >> (24 - index * 8);
}
uint32_t get32(const uint8_t* input) {
	return (uint32_t(input[0]) << 24) | (uint32_t(input[1]) << 16) | (uint32_t(input[2]) << 8) | input[3];
}
}

void Contacts::close() {
	CryptoEngine::zeroize(records_, sizeof(records_));
}

const Contacts::View* Contacts::view(uint8_t slot) const {
	if (slot < 1 || slot > Capacity || !records_[slot - 1].view.used) return nullptr;
	return &records_[slot - 1].view;
}

uint8_t Contacts::count() const {
	uint8_t count = 0;
	for (const Record& record : records_) if (record.view.used) ++count;
	return count;
}

uint8_t Contacts::findPublicKey(const uint8_t publicKey[32]) const {
	for (uint8_t slot = 1; slot <= Capacity; ++slot)
		if (view(slot) && memcmp(view(slot)->publicKey, publicKey, 32) == 0) return slot;
	return 0;
}

uint8_t Contacts::freeSlot() const {
	for (uint8_t slot = 1; slot <= Capacity; ++slot) if (!view(slot)) return slot;
	return 0;
}

bool Contacts::save(uint8_t slot, const Record& record) {
	uint8_t bytes[128] = {};
	const size_t nameLength = strlen(record.view.name);
	bytes[0] = 1;
	memcpy(bytes + 1, record.view.publicKey, 32);
	bytes[33] = nameLength;
	memcpy(bytes + 34, record.view.name, nameLength);
	size_t offset = 34 + nameLength;
	memcpy(bytes + offset, record.outgoing, 16); offset += 16;
	memcpy(bytes + offset, record.incoming, 16); offset += 16;
	put32(bytes + offset, record.end); offset += 4;
	put32(bytes + offset, record.view.rxHighest); offset += 4;
	bytes[offset++] = record.view.active ? 1 : 0;
	memcpy(bytes + offset, record.view.fingerprint, 14); offset += 14;
	const bool ok = vault_.writeContact(slot, bytes, offset);
	CryptoEngine::zeroize(bytes, sizeof(bytes));
	return ok;
}

bool Contacts::decode(const uint8_t* bytes, size_t length, Record& record) {
	if (length < 89 || bytes[0] != 1 || bytes[33] < 1 || bytes[33] > 24 ||
		length != size_t(89 + bytes[33]) || memchr(bytes + 34, 0, bytes[33]) ||
		!validUtf8(bytes + 34, bytes[33])) return false;
	record = Record{};
	memcpy(record.view.publicKey, bytes + 1, 32);
	memcpy(record.view.name, bytes + 34, bytes[33]);
	size_t offset = 34 + bytes[33];
	memcpy(record.outgoing, bytes + offset, 16); offset += 16;
	memcpy(record.incoming, bytes + offset, 16); offset += 16;
	record.end = get32(bytes + offset); offset += 4;
	record.next = record.end;
	record.view.txCounter = record.next - 1;
	record.view.rxHighest = get32(bytes + offset); offset += 4;
	if (bytes[offset] > 1 || record.end < 1 || record.end > CounterEnd || record.view.rxHighest >= CounterEnd)
		return false;
	record.view.active = bytes[offset++] != 0;
	memcpy(record.view.fingerprint, bytes + offset, 14);
	for (size_t index = 0; index < 14; ++index) {
		const char value = record.view.fingerprint[index];
		if (index == 4 || index == 9) { if (value != ' ') return false; }
		else if (value < '0' || value > '9') return false;
	}
	uint8_t digest[32];
	if (!CryptoEngine::sha256(record.view.publicKey, 32, digest)) return false;
	record.view.nodeId = (uint16_t(digest[0]) << 8) | digest[1];
	record.window = record.view.rxHighest ? UINT64_MAX : 0;
	record.view.used = true;
	return true;
}

bool Contacts::load() {
	close();
	uint8_t bytes[128] = {};
	size_t length = 0;
	bool ok = vault_.isOpen();
	for (uint8_t slot = 1; ok && slot <= Capacity; ++slot) {
		if (!(vault_.contacts() & (uint32_t(1) << (slot - 1)))) continue;
		ok = vault_.readContact(slot, bytes, sizeof(bytes), length) && decode(bytes, length, records_[slot - 1]);
		CryptoEngine::zeroize(bytes, sizeof(bytes));
	}
	if (!ok) close();
	return ok;
}

bool Contacts::insert(uint8_t slot, const uint8_t publicKey[32], const char* name,
	const char* fingerprint, const uint8_t outgoing[16], const uint8_t incoming[16]) {
	if (slot < 1 || slot > Capacity || strlen(name) < 1 || strlen(name) > 24 || strlen(fingerprint) != 14)
		return false;
	Record record;
	record.view.used = true;
	memcpy(record.view.publicKey, publicKey, 32);
	strcpy(record.view.name, name);
	strcpy(record.view.fingerprint, fingerprint);
	memcpy(record.outgoing, outgoing, 16);
	memcpy(record.incoming, incoming, 16);
	uint8_t digest[32];
	const bool ok = CryptoEngine::sha256(publicKey, 32, digest) && save(slot, record);
	if (ok) {
		record.view.nodeId = (uint16_t(digest[0]) << 8) | digest[1];
		if (change_ && records_[slot - 1].view.used) change_(context_, slot);
		CryptoEngine::zeroize(&records_[slot - 1], sizeof(Record));
		records_[slot - 1] = record;
	}
	CryptoEngine::zeroize(&record, sizeof(record));
	return ok;
}

bool Contacts::rename(uint8_t slot, const char* name) {
	if (!view(slot) || strlen(name) < 1 || strlen(name) > 24) return false;
	Record candidate = records_[slot - 1];
	memset(candidate.view.name, 0, sizeof(candidate.view.name));
	strcpy(candidate.view.name, name);
	const bool ok = save(slot, candidate);
	if (ok) records_[slot - 1] = candidate;
	CryptoEngine::zeroize(&candidate, sizeof(candidate));
	return ok;
}

bool Contacts::remove(uint8_t slot) {
	if (!view(slot) || !vault_.deleteContact(slot)) return false;
	if (change_) change_(context_, slot);
	CryptoEngine::zeroize(&records_[slot - 1], sizeof(Record));
	return true;
}

bool Contacts::allocate(uint8_t slot, uint32_t& counter) {
	counter = 0;
	if (!view(slot)) return false;
	Record& record = records_[slot - 1];
	if (record.next >= CounterEnd) return false;
	if (record.next >= record.end) {
		const uint32_t previous = record.end;
		record.end = record.next > CounterEnd - 256 ? CounterEnd : record.next + 256;
		if (!save(slot, record)) { record.end = previous; return false; }
	}
	counter = record.next++;
	record.view.txCounter = counter;
	return true;
}

Contacts::Replay Contacts::accept(uint8_t slot, uint32_t counter, float rssi, float snr) {
	if (!view(slot) || counter < 1 || counter >= CounterEnd) return Replay::Stale;
	Record& record = records_[slot - 1];
	const uint32_t highest = record.view.rxHighest;
	const uint32_t distance = counter > highest ? counter - highest : highest - counter;
	if (counter <= highest) {
		if (distance >= 64) return Replay::Stale;
		if (record.window & (uint64_t(1) << distance)) return Replay::Duplicate;
	}

	Record candidate = record;
	if (counter > highest) {
		candidate.window = distance >= 64 ? 1 : (record.window << distance) | 1;
		candidate.view.rxHighest = counter;
	} else candidate.window |= uint64_t(1) << distance;
	candidate.view.active = true;
	candidate.view.hasSignal = true; candidate.view.rssi = rssi; candidate.view.snr = snr;
	const bool ok = save(slot, candidate);
	if (ok) record = candidate;
	CryptoEngine::zeroize(&candidate, sizeof(candidate));
	return ok ? Replay::New : Replay::StorageFailure;
}

#if OFFGRID_RADIO_DIAGNOSTICS
bool Contacts::selfTest(VaultStore& vault) {
	static Contacts contacts(vault);
	uint8_t publicKey[32] = {9}, key[16] = {7};
	bool ok = contacts.load() && contacts.insert(1, publicKey, "Fixture", "0000 0000 0000", key, key);
	uint32_t counter = 0;
	ok = ok && contacts.allocate(1, counter) && counter == 1 &&
		contacts.allocate(1, counter) && counter == 2;
	contacts.close();
	ok = ok && contacts.view(1) == nullptr && contacts.load() &&
		contacts.allocate(1, counter) && counter == 257;
	ok = ok && contacts.accept(1, 10, -90, 5) == Replay::New &&
		contacts.accept(1, 10, -90, 5) == Replay::Duplicate &&
		contacts.accept(1, 9, -90, 5) == Replay::New &&
		contacts.accept(1, 80, -90, 5) == Replay::New &&
		contacts.accept(1, 10, -90, 5) == Replay::Stale;
	contacts.close();
	ok = ok && contacts.load() && contacts.accept(1, 79, -90, 5) == Replay::Duplicate;
	if (ok) {
		contacts.records_[0].next = CounterEnd - 1;
		contacts.records_[0].end = CounterEnd - 1;
		ok = contacts.allocate(1, counter) && counter == CounterEnd - 1 &&
			!contacts.allocate(1, counter) && counter == 0;
	}
	ok = ok && contacts.remove(1) && !contacts.view(1);
	contacts.close();
	return ok;
}
#endif