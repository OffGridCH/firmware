#pragma once

#include "../storage/VaultStore.h"

class Contacts {
public:
	static constexpr size_t Capacity = 32;
	static constexpr uint32_t CounterEnd = 0x1000000;
	enum class Replay { New, Duplicate, Stale, StorageFailure };
	struct View {
		bool used = false;
		bool active = false;
		uint8_t publicKey[32] = {};
		char name[25] = {};
		char fingerprint[15] = {};
		uint16_t nodeId = 0;
		uint32_t txCounter = 0;
		uint32_t rxHighest = 0;
		bool hasSignal = false;
		float rssi = 0;
		float snr = 0;
	};
	using ChangeHandler = void (*)(void* context, uint8_t slot);

	explicit Contacts(VaultStore& vault) : vault_(vault) {}
	bool load();
	void close();
	void setChangeHandler(ChangeHandler handler, void* context) { change_ = handler; context_ = context; }
	const View* view(uint8_t slot) const;
	uint8_t count() const;
	uint8_t findPublicKey(const uint8_t publicKey[32]) const;
	uint8_t freeSlot() const;
	bool insert(uint8_t slot, const uint8_t publicKey[32], const char* name,
		const char* fingerprint, const uint8_t outgoing[16], const uint8_t incoming[16]);
	bool rename(uint8_t slot, const char* name);
	bool remove(uint8_t slot);
	bool allocate(uint8_t slot, uint32_t& counter);
	Replay accept(uint8_t slot, uint32_t counter, float rssi, float snr);

private:
	friend class MeshEngine;
#if OFFGRID_RADIO_DIAGNOSTICS
	friend class VaultStore;
	static bool selfTest(VaultStore& vault);
#endif
	struct Record {
		View view;
		uint8_t outgoing[16] = {};
		uint8_t incoming[16] = {};
		uint32_t next = 1;
		uint32_t end = 1;
		uint64_t window = 0;
	};
	bool save(uint8_t slot, const Record& record);
	bool decode(const uint8_t* bytes, size_t length, Record& record);
	VaultStore& vault_;
	Record records_[Capacity];
	ChangeHandler change_ = nullptr;
	void* context_ = nullptr;
};