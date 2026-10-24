#pragma once

#include "Contacts.h"

// Group chats: one shared key per group epoch, distributed by the owner over paired DMs.
// Every member derives its own sending key from it, so (key, counter) nonces never collide.
class Groups {
public:
	static constexpr size_t Capacity = 8;
	static constexpr size_t MaxMembers = 16;
	static constexpr size_t MaxSyncBytes = 79;
	enum class Error { None, Full, Exhausted, Storage, Crypto, Invalid };
	enum class Sync { Joined, Updated, Unchanged, Rejected, Full, StorageFailure };
	struct View {
		bool used = false;
		bool owner = false;
		uint8_t id[4] = {};
		uint8_t epoch = 0;
		char name[25] = {};
		uint8_t ownerContact = 0;
		uint8_t member = 0;
		uint8_t count = 0;
		uint16_t nodes[MaxMembers] = {};
		uint8_t contacts[MaxMembers] = {};
		uint16_t tag = 0;
	};

	explicit Groups(VaultStore& vault) : vault_(vault) {}
	bool load();
	void close();
	const View* view(uint8_t slot) const;
	uint8_t count() const;
	bool addressed(uint16_t tag) const;
	Error create(const char* name, const uint16_t nodes[], const uint8_t contacts[], uint8_t count, uint8_t& slot);
	Error rekey(uint8_t slot, const uint16_t nodes[], const uint8_t contacts[], uint8_t count);
	bool remove(uint8_t slot);
	bool syncBody(uint8_t slot, uint8_t member, uint8_t* bytes, size_t& length) const;
	Sync apply(uint8_t contact, uint16_t source, uint16_t ownId, const uint8_t* bytes, size_t length, uint8_t& slot);
	bool senderKey(uint8_t slot, uint8_t member, uint8_t key[16]) const;
	bool exhausted() const { return next_ >= Contacts::CounterEnd; }
	bool allocate(uint32_t& counter);
	Contacts::Replay accept(uint8_t slot, uint8_t member, uint32_t counter);
	static bool deriveKey(const uint8_t key[16], const uint8_t id[4], uint8_t epoch, uint8_t member,
		uint8_t output[16]);
	static bool deriveTag(const uint8_t key[16], const uint8_t id[4], uint8_t epoch, uint16_t& tag);

private:
	struct Record {
		View view;
		uint8_t key[16] = {};
		uint32_t highest[MaxMembers] = {};
		uint64_t window[MaxMembers] = {};
	};
	bool save(uint8_t slot, const Record& record);
	bool decode(const uint8_t* bytes, size_t length, Record& record);
	bool assign(uint8_t slot, Record& record);
	uint8_t freeSlot() const;
	VaultStore& vault_;
	Record records_[Capacity];
	uint32_t next_ = 1;
	uint32_t end_ = 1;
};
