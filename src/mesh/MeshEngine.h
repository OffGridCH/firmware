#pragma once

#include "PacketCodec.h"
#include "../security/Groups.h"

class MeshEngine {
public:
	static constexpr size_t Capacity = 8;
	enum class Error { None, Locked, NoContact, Full, CounterExhausted, Storage, Invalid };
	enum class State { Queued, Sent, Relayed, Retrying, Delivered, Failed, Cancelled };
	enum class Reason { None, Timeout, Locked, ContactDeleted, RadioError };
	enum class EventType { Status, Received, ContactActive, GroupState };
	struct Pending {
		bool used = false;
		uint32_t reference = 0;
		uint8_t contact = 0;
		uint8_t group = 0;
		PacketCodec::Type type = PacketCodec::Type::Text;
		uint32_t counter = 0;
		State state = State::Queued;
		uint8_t retry = 0;
	};
	struct Event {
		EventType type = EventType::Status;
		Pending message;
		Reason reason = Reason::None;
		PacketCodec::Payload payload;
		uint64_t timeMs = 0;
		uint64_t rttMs = 0;
		uint8_t hops = 0;
		float rssi = 0;
		float snr = 0;
		uint8_t member = 0;
		uint16_t node = 0;
		uint8_t epoch = 0;
		bool joined = false;
	};
	struct Config {
		uint8_t retries = 3;
		uint32_t retryBaseMs = 0;
		bool relayEnabled = true;
		bool relayWhenLocked = false;
		uint8_t backoffSlots = 8;
		uint8_t ratePerSource = 20;
	};
	struct Stats {
		uint32_t tagFailures = 0;
		uint32_t replayDrops = 0;
		uint32_t malformed = 0;
		uint32_t acksSent = 0;
		uint32_t acksReceived = 0;
		uint32_t retries = 0;
		uint32_t droppedEvents = 0;
		uint32_t droppedAcks = 0;
		uint32_t relayed = 0;
		uint32_t relaySuppressed = 0;
		uint32_t relayDropped = 0;
		uint32_t rateDrops = 0;
#if OFFGRID_RADIO_DIAGNOSTICS
		uint32_t injectedAckDrops = 0;
#endif
	};
	MeshEngine(Contacts& contacts, Groups& groups);
	bool init(bool provisioned);
	bool ready() const { return cryptoReady_; }
	bool radioWanted() const;
	using EventHandler = void (*)(void* context, const Event& event);
	void setEventHandler(EventHandler handler, void* context) { handler_ = handler; context_ = context; }
	void open(uint16_t ownId);
	void close(uint64_t nowMs);
	void configure(const Config& config);
	Error enqueue(uint8_t contact, const PacketCodec::Payload& payload, uint8_t hops,
		uint64_t nowMs, Pending& accepted, size_t& bytes, uint8_t group = 0);
	bool cancel(uint32_t reference, uint64_t nowMs);
	void groupChanged(uint8_t group, bool deleted, uint64_t nowMs);
	const Pending* pending(size_t index) const;
	size_t count() const;
	void tick(uint64_t nowMs);
	bool prepareTransmission(PacketCodec::Packet& packet, uint64_t nowMs);
	void transmissionRejected(bool busy, uint64_t nowMs);
	void transmissionComplete(bool success, uint32_t airtimeMs, uint64_t nowMs);
	void radioUnavailable(uint64_t nowMs);
	bool takeAbort();
	bool takeStorageFailure();
	bool takeCryptoFailure();
	void receive(const uint8_t* bytes, size_t length, float rssi, float snr, uint64_t nowMs);
	const Event* event() const;
	void consumeEvent();
	const Stats& stats() const { return stats_; }
	const char* receiveAction() const { return receiveAction_; }
	void resetStats() { stats_ = {}; }
	static const char* stateName(State state);
	static const char* typeName(PacketCodec::Type type);
	static const char* reasonName(Reason reason);
#if OFFGRID_RADIO_DIAGNOSTICS
	static bool selfTest(VaultStore& vault);
	void dropNextAck() { dropNextAck_ = true; }
#endif

private:
	struct Slot {
		Pending view;
		PacketCodec::Payload payload;
		uint8_t hops = 3;
		bool sent = false;
		uint64_t created = 0;
		uint64_t due = 0;
		uint32_t lastHash = 0;
	};
	struct Ack {
		bool used = false;
		uint8_t contact = 0;
		PacketCodec::Packet packet;
	};
	struct Relay {
		bool used = false;
		uint32_t hash = 0;
		uint64_t due = 0;
		PacketCodec::Packet packet;
	};
	struct Source {
		bool used = false;
		uint16_t id = 0;
		uint16_t relays = 0;
		uint16_t failures = 0;
		uint64_t start = 0;
	};
	bool hash(const uint8_t* bytes, size_t length, uint32_t& result);
	bool remember(uint32_t hash);
	Source* source(uint16_t id, uint64_t nowMs);
	bool endpoint(const uint8_t* bytes, size_t length, const PacketCodec::Header& header,
		float rssi, float snr, uint64_t nowMs);
	bool groupEndpoint(const uint8_t* bytes, size_t length, const PacketCodec::Header& header,
		float rssi, float snr, uint64_t nowMs);
	static void contactChanged(void* context, uint8_t contact);
	void removeContact(uint8_t contact);
	void emit(const Event& event);
	void status(size_t index, State state, Reason reason, uint64_t nowMs, uint8_t hops = 0);
	void finish(size_t index, State state, Reason reason, uint64_t nowMs, uint8_t hops = 0);
	bool allocate(uint8_t contact, uint32_t& counter);
	void ack(uint8_t contact, const PacketCodec::Header& original);
	Contacts& contacts_;
	Groups& groups_;
	Config config_;
	Slot slots_[Capacity];
	Ack acks_[4];
	Relay relays_[8];
	uint32_t cache_[256] = {};
	size_t cacheCount_ = 0;
	size_t cacheNext_ = 0;
	uint8_t hashKey_[16] = {};
	Source sources_[64];
	Event events_[32];
	size_t eventHead_ = 0;
	size_t eventCount_ = 0;
	int flight_ = -1;
	bool flightAck_ = false;
	bool flightRelay_ = false;
	bool abort_ = false;
	bool storageFailure_ = false;
	bool cryptoReady_ = false;
	bool cryptoFailure_ = false;
	bool provisioned_ = false;
	bool unlocked_ = false;
	uint16_t ownId_ = 0;
	uint32_t nextReference_ = 1;
	uint64_t nowMs_ = 0;
	uint64_t nextTransmitAt_ = 0;
	Stats stats_;
	const char* receiveAction_ = "ignored";
	EventHandler handler_ = nullptr;
	void* context_ = nullptr;
#if OFFGRID_RADIO_DIAGNOSTICS
	bool dropNextAck_ = false;
#endif
};