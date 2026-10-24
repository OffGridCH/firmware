#include "Groups.h"

#include "CryptoEngine.h"
#include "../Types.h"
#include <string.h>

namespace {
void put24(uint8_t* output, uint32_t value) {
	output[0] = value >> 16; output[1] = value >> 8; output[2] = value;
}

uint32_t get24(const uint8_t* input) {
	return (uint32_t(input[0]) << 16) | (uint32_t(input[1]) << 8) | input[2];
}

// HKDF-SHA256(ikm = GK, salt = GID || EPOCH, info, length)
bool derive(const uint8_t key[16], const uint8_t id[4], uint8_t epoch, const uint8_t* info, size_t infoLength,
	uint8_t* output, size_t length) {
	uint8_t salt[5];
	memcpy(salt, id, 4); salt[4] = epoch;
	return CryptoEngine::hkdfSha256(key, 16, salt, sizeof(salt), info, infoLength, output, length);
}
}

bool Groups::deriveKey(const uint8_t key[16], const uint8_t id[4], uint8_t epoch, uint8_t member,
	uint8_t output[16]) {
	const uint8_t info[] = {'o', 'g', '1', '-', 'g', 'k', member};
	return derive(key, id, epoch, info, sizeof(info), output, 16);
}

bool Groups::deriveTag(const uint8_t key[16], const uint8_t id[4], uint8_t epoch, uint16_t& tag) {
	const uint8_t info[] = {'o', 'g', '1', '-', 'g', 't'};
	uint8_t output[2];
	if (!derive(key, id, epoch, info, sizeof(info), output, sizeof(output))) return false;
	tag = (uint16_t(output[0]) << 8) | output[1];
	return true;
}

void Groups::close() {
	CryptoEngine::zeroize(records_, sizeof(records_));
	next_ = end_ = 1;
}

bool Groups::load() {
	close();
	uint8_t bytes[VaultStore::MaxPlaintext] = {};
	size_t length = 0;
	bool ok = vault_.isOpen();
	for (uint8_t slot = 1; ok && slot <= Capacity; ++slot) {
		if (!(vault_.groups() & (1 << (slot - 1)))) continue;
		ok = vault_.readGroup(slot, bytes, sizeof(bytes), length) && decode(bytes, length, records_[slot - 1]);
		CryptoEngine::zeroize(bytes, sizeof(bytes));
	}
	if (vault_.groupCounter()) next_ = end_ = vault_.groupCounter();
	if (!ok) close();
	return ok;
}

const Groups::View* Groups::view(uint8_t slot) const {
	if (slot < 1 || slot > Capacity || !records_[slot - 1].view.used) return nullptr;
	return &records_[slot - 1].view;
}

uint8_t Groups::count() const {
	uint8_t count = 0;
	for (const Record& record : records_) if (record.view.used) ++count;
	return count;
}

uint8_t Groups::freeSlot() const {
	for (uint8_t slot = 1; slot <= Capacity; ++slot) if (!view(slot)) return slot;
	return 0;
}

bool Groups::addressed(uint16_t tag) const {
	for (const Record& record : records_) if (record.view.used && record.view.tag == tag) return true;
	return false;
}

bool Groups::save(uint8_t slot, const Record& record) {
	uint8_t bytes[VaultStore::MaxPlaintext] = {};
	const View& view = record.view;
	const size_t nameLength = strlen(view.name);
	bytes[0] = 1;
	memcpy(bytes + 1, view.id, 4);
	bytes[5] = view.epoch;
	memcpy(bytes + 6, record.key, 16);
	bytes[22] = view.owner ? 1 : 0;
	bytes[23] = view.ownerContact;
	bytes[24] = view.member;
	bytes[25] = view.count;
	bytes[26] = nameLength;
	memcpy(bytes + 27, view.name, nameLength);
	size_t offset = 27 + nameLength;
	for (uint8_t member = 0; member < view.count; ++member) {
		bytes[offset++] = view.nodes[member] >> 8;
		bytes[offset++] = view.nodes[member];
		bytes[offset++] = view.contacts[member];
		put24(bytes + offset, record.highest[member]); offset += 3;
	}
	const bool ok = vault_.writeGroup(slot, bytes, offset);
	CryptoEngine::zeroize(bytes, sizeof(bytes));
	return ok;
}

bool Groups::decode(const uint8_t* bytes, size_t length, Record& record) {
	if (length < 27 || bytes[0] != 1) return false;
	const uint8_t count = bytes[25], nameLength = bytes[26];
	if (bytes[22] > 1 || !bytes[5] || count < 2 || count > MaxMembers || bytes[24] >= count ||
		(bytes[22] == 1) != (bytes[24] == 0) || (bytes[22] == 1) != (bytes[23] == 0) || bytes[23] > Contacts::Capacity ||
		nameLength < 1 || nameLength > 24 || length != size_t(27 + nameLength + 6 * count) ||
		memchr(bytes + 27, 0, nameLength) || !validUtf8(bytes + 27, nameLength)) return false;
	record = Record{};
	View& view = record.view;
	memcpy(view.id, bytes + 1, 4);
	view.epoch = bytes[5];
	memcpy(record.key, bytes + 6, 16);
	view.owner = bytes[22] == 1;
	view.ownerContact = bytes[23];
	view.member = bytes[24];
	view.count = count;
	memcpy(view.name, bytes + 27, nameLength);
	size_t offset = 27 + nameLength;
	for (uint8_t member = 0; member < count; ++member) {
		view.nodes[member] = (uint16_t(bytes[offset]) << 8) | bytes[offset + 1];
		view.contacts[member] = bytes[offset + 2];
		record.highest[member] = get24(bytes + offset + 3);
		if (view.contacts[member] > Contacts::Capacity) return false;
		// As for contacts, everything at or below the saved highest counts as seen after a reboot.
		record.window[member] = record.highest[member] ? UINT64_MAX : 0;
		offset += 6;
	}
	if (!deriveTag(record.key, view.id, view.epoch, view.tag)) return false;
	view.used = true;
	return true;
}

// A TAG equal to a member's NodeID or to another local group's TAG would make those packets ambiguous
// to address checks, so the owner re-rolls the key. Groups joined from other owners can still collide;
// receivers resolve that by trial decryption, never by the TAG alone.
bool Groups::assign(uint8_t slot, Record& record) {
	for (uint8_t attempt = 0; attempt < 16; ++attempt) {
		if (!CryptoEngine::randomBytes(record.key, sizeof(record.key)) ||
			!deriveTag(record.key, record.view.id, record.view.epoch, record.view.tag)) return false;
		bool collision = false;
		for (uint8_t member = 0; member < record.view.count; ++member)
			if (record.view.nodes[member] == record.view.tag) collision = true;
		for (uint8_t other = 1; other <= Capacity; ++other)
			if (other != slot && view(other) && view(other)->tag == record.view.tag) collision = true;
		if (!collision) return true;
	}
	return false;
}

Groups::Error Groups::create(const char* name, const uint16_t nodes[], const uint8_t contacts[], uint8_t count,
	uint8_t& slot) {
	slot = 0;
	if (count < 2 || count > MaxMembers || !name || strlen(name) < 1 || strlen(name) > 24) return Error::Invalid;
	const uint8_t free = freeSlot();
	if (!free) return Error::Full;
	Record record;
	record.view.used = true;
	record.view.owner = true;
	record.view.epoch = 1;
	record.view.count = count;
	strcpy(record.view.name, name);
	memcpy(record.view.nodes, nodes, count * sizeof(uint16_t));
	memcpy(record.view.contacts, contacts, count);
	record.view.contacts[0] = 0;
	Error error = Error::None;
	if (!CryptoEngine::randomBytes(record.view.id, sizeof(record.view.id)) || !assign(free, record))
		error = Error::Crypto;
	else if (!save(free, record)) error = Error::Storage;
	else { records_[free - 1] = record; slot = free; }
	CryptoEngine::zeroize(&record, sizeof(record));
	return error;
}

// Every membership change starts a new epoch with a fresh key, so a removed member cannot read on and a
// member index is never reused under the same key.
Groups::Error Groups::rekey(uint8_t slot, const uint16_t nodes[], const uint8_t contacts[], uint8_t count) {
	const View* current = view(slot);
	if (!current || !current->owner || count < 2 || count > MaxMembers) return Error::Invalid;
	if (current->epoch == 255) return Error::Exhausted;
	Record record;
	record.view = *current;
	++record.view.epoch;
	record.view.count = count;
	memset(record.view.nodes, 0, sizeof(record.view.nodes));
	memset(record.view.contacts, 0, sizeof(record.view.contacts));
	memcpy(record.view.nodes, nodes, count * sizeof(uint16_t));
	memcpy(record.view.contacts, contacts, count);
	record.view.contacts[0] = 0;
	Error error = Error::None;
	if (!assign(slot, record)) error = Error::Crypto;
	else if (!save(slot, record)) error = Error::Storage;
	else records_[slot - 1] = record;
	CryptoEngine::zeroize(&record, sizeof(record));
	return error;
}

bool Groups::remove(uint8_t slot) {
	if (!view(slot) || !vault_.deleteGroup(slot)) return false;
	CryptoEngine::zeroize(&records_[slot - 1], sizeof(Record));
	return true;
}

// GROUP_SYNC body: GID(4) EPOCH(1) GK(16) YOUR_INDEX(1) N(1) NodeID[N](2N) name(1..24).
bool Groups::syncBody(uint8_t slot, uint8_t member, uint8_t* bytes, size_t& length) const {
	length = 0;
	const View* group = view(slot);
	if (!group || !group->owner || member < 1 || member >= group->count) return false;
	memcpy(bytes, group->id, 4);
	bytes[4] = group->epoch;
	memcpy(bytes + 5, records_[slot - 1].key, 16);
	bytes[21] = member;
	bytes[22] = group->count;
	size_t offset = 23;
	for (uint8_t index = 0; index < group->count; ++index) {
		bytes[offset++] = group->nodes[index] >> 8;
		bytes[offset++] = group->nodes[index];
	}
	const size_t nameLength = strlen(group->name);
	memcpy(bytes + offset, group->name, nameLength);
	length = offset + nameLength;
	return true;
}

Groups::Sync Groups::apply(uint8_t contact, uint16_t source, uint16_t ownId, const uint8_t* bytes, size_t length,
	uint8_t& slot) {
	slot = 0;
	if (length < 28 || length > MaxSyncBytes) return Sync::Rejected;
	const uint8_t member = bytes[21], count = bytes[22];
	if (count < 2 || count > MaxMembers || length < size_t(24 + 2 * count) || length > size_t(47 + 2 * count))
		return Sync::Rejected;
	const uint8_t* name = bytes + 23 + 2 * count;
	const size_t nameLength = length - 23 - 2 * count;
	if (!bytes[4] || member < 1 || member >= count || memchr(name, 0, nameLength) || !validUtf8(name, nameLength))
		return Sync::Rejected;
	Record record;
	View& candidate = record.view;
	candidate.used = true;
	memcpy(candidate.id, bytes, 4);
	candidate.epoch = bytes[4];
	memcpy(record.key, bytes + 5, 16);
	candidate.ownerContact = contact;
	candidate.member = member;
	candidate.count = count;
	for (uint8_t index = 0; index < count; ++index)
		candidate.nodes[index] = (uint16_t(bytes[23 + 2 * index]) << 8) | bytes[24 + 2 * index];
	memcpy(candidate.name, name, nameLength);
	Sync result = Sync::Joined;
	uint8_t target = 0;
	for (uint8_t other = 1; other <= Capacity; ++other)
		if (view(other) && memcmp(view(other)->id, candidate.id, 4) == 0) target = other;
	if (candidate.nodes[0] != source || candidate.nodes[member] != ownId) result = Sync::Rejected;
	else if (target) {
		// Only the owner that created this group may change it, and only forward. A repeated sync of the
		// current epoch (for example after a lost receipt) changes nothing.
		const Record& current = records_[target - 1];
		if (current.view.owner || current.view.ownerContact != contact || candidate.epoch < current.view.epoch)
			result = Sync::Rejected;
		else if (candidate.epoch == current.view.epoch)
			result = memcmp(current.key, record.key, 16) == 0 && current.view.member == member &&
				current.view.count == count && memcmp(current.view.nodes, candidate.nodes, sizeof(candidate.nodes)) == 0 &&
				strcmp(current.view.name, candidate.name) == 0 ? Sync::Unchanged : Sync::Rejected;
		else result = Sync::Updated;
	} else if (!(target = freeSlot())) result = Sync::Full;
	if (result == Sync::Joined || result == Sync::Updated) {
		if (!deriveTag(record.key, candidate.id, candidate.epoch, candidate.tag) || !save(target, record))
			result = Sync::StorageFailure;
		else { records_[target - 1] = record; slot = target; }
	}
	CryptoEngine::zeroize(&record, sizeof(record));
	return result;
}

bool Groups::senderKey(uint8_t slot, uint8_t member, uint8_t key[16]) const {
	const View* group = view(slot);
	return group && member < group->count &&
		deriveKey(records_[slot - 1].key, group->id, group->epoch, member, key);
}

// One counter for all groups of this device. It never restarts while the identity exists, so even a
// re-sent sync of an epoch this device already used cannot repeat a (sender key, counter) pair.
bool Groups::allocate(uint32_t& counter) {
	counter = 0;
	if (next_ >= Contacts::CounterEnd) return false;
	if (next_ >= end_) {
		const uint32_t end = next_ > Contacts::CounterEnd - 256 ? Contacts::CounterEnd : next_ + 256;
		if (!vault_.writeGroupCounter(end)) return false;
		end_ = end;
	}
	counter = next_++;
	return true;
}

Contacts::Replay Groups::accept(uint8_t slot, uint8_t member, uint32_t counter) {
	const View* group = view(slot);
	if (!group || member >= group->count || counter < 1 || counter >= Contacts::CounterEnd)
		return Contacts::Replay::Stale;
	Record& record = records_[slot - 1];
	const uint32_t highest = record.highest[member];
	const uint32_t distance = counter > highest ? counter - highest : highest - counter;
	if (counter <= highest) {
		if (distance >= 64) return Contacts::Replay::Stale;
		if (record.window[member] & (uint64_t(1) << distance)) return Contacts::Replay::Duplicate;
	}
	Record candidate = record;
	if (counter > highest) {
		candidate.window[member] = distance >= 64 ? 1 : (record.window[member] << distance) | 1;
		candidate.highest[member] = counter;
	} else candidate.window[member] |= uint64_t(1) << distance;
	const bool ok = save(slot, candidate);
	if (ok) record = candidate;
	CryptoEngine::zeroize(&candidate, sizeof(candidate));
	return ok ? Contacts::Replay::New : Contacts::Replay::StorageFailure;
}
