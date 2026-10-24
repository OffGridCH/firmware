#include "MeshEngine.h"

#include <string.h>
#include <math.h>
#include "../security/CryptoEngine.h"

MeshEngine::MeshEngine(Contacts& contacts) : contacts_(contacts) {
	contacts_.setChangeHandler(contactChanged, this);
}

bool MeshEngine::init(bool provisioned) {
	provisioned_ = provisioned;
	cacheCount_ = cacheNext_ = 0;
	nextTransmitAt_ = 0;
	CryptoEngine::zeroize(sources_, sizeof(sources_));
	cryptoReady_ = CryptoEngine::randomBytes(hashKey_, sizeof(hashKey_));
	cryptoFailure_ = !cryptoReady_;
	return cryptoReady_;
}

bool MeshEngine::radioWanted() const {
	return cryptoReady_ && (unlocked_ || (provisioned_ && config_.relayEnabled && config_.relayWhenLocked));
}

void MeshEngine::configure(const Config& config) {
	config_ = config;
	if (!config_.relayEnabled) {
		CryptoEngine::zeroize(relays_, sizeof(relays_));
		if (flightRelay_ && flight_ >= 0) { flight_ = -1; abort_ = true; }
	}
}

bool MeshEngine::hash(const uint8_t* bytes, size_t length, uint32_t& result) {
	uint8_t normalized[PacketCodec::MaxPacketBytes], digest[32];
	memcpy(normalized, bytes, length);
	normalized[0] &= 0xe3;
	if (!CryptoEngine::hmacSha256(hashKey_, sizeof(hashKey_), normalized, length, digest)) {
		cryptoReady_ = false; cryptoFailure_ = true; return false;
	}
	result = (uint32_t(digest[0]) << 24) | (uint32_t(digest[1]) << 16) |
		(uint32_t(digest[2]) << 8) | digest[3];
	return true;
}

bool MeshEngine::remember(uint32_t value) {
	for (size_t index = 0; index < cacheCount_; ++index) if (cache_[index] == value) return false;
	cache_[cacheNext_] = value;
	cacheNext_ = (cacheNext_ + 1) % 256;
	if (cacheCount_ < 256) ++cacheCount_;
	return true;
}

MeshEngine::Source* MeshEngine::source(uint16_t id, uint64_t nowMs) {
	for (Source& source : sources_) {
		if (!source.used || source.id != id) continue;
		if (nowMs - source.start >= 60000) { source.relays = source.failures = 0; source.start = nowMs; }
		return &source;
	}
	for (Source& source : sources_) {
		if (source.used && nowMs - source.start < 60000) continue;
		source = {}; source.used = true; source.id = id; source.start = nowMs;
		return &source;
	}
	return nullptr;
}

const char* MeshEngine::stateName(State state) {
	switch (state) {
	case State::Queued: return "queued";
	case State::Sent: return "sent";
	case State::Relayed: return "relayed";
	case State::Retrying: return "retrying";
	case State::Delivered: return "delivered";
	case State::Failed: return "failed";
	case State::Cancelled: return "cancelled";
	}
	return "failed";
}

const char* MeshEngine::typeName(PacketCodec::Type type) {
	return type == PacketCodec::Type::Text ? "text" : type == PacketCodec::Type::Reaction ? "reaction" : "geo";
}

const char* MeshEngine::reasonName(Reason reason) {
	switch (reason) {
	case Reason::Timeout: return "timeout";
	case Reason::Locked: return "locked";
	case Reason::ContactDeleted: return "contact_deleted";
	case Reason::RadioError: return "radio_error";
	case Reason::None: return nullptr;
	}
	return nullptr;
}

void MeshEngine::open(uint16_t ownId) {
	ownId_ = ownId;
	unlocked_ = true;
	provisioned_ = true;
	storageFailure_ = false;
}

void MeshEngine::close(uint64_t nowMs) {
	nowMs_ = nowMs;
	CryptoEngine::zeroize(events_, sizeof(events_));
	eventHead_ = eventCount_ = 0;
	for (size_t index = 0; index < Capacity; ++index)
		if (slots_[index].view.used) finish(index, State::Failed, Reason::Locked, nowMs);
	CryptoEngine::zeroize(acks_, sizeof(acks_));
	CryptoEngine::zeroize(relays_, sizeof(relays_));
	if (flight_ >= 0) abort_ = true;
	flight_ = -1;
	unlocked_ = false;
	ownId_ = 0;
}

void MeshEngine::contactChanged(void* context, uint8_t contact) {
	static_cast<MeshEngine*>(context)->removeContact(contact);
}

void MeshEngine::removeContact(uint8_t contact) {
	for (size_t index = 0; index < Capacity; ++index)
		if (slots_[index].view.used && slots_[index].view.contact == contact)
			finish(index, State::Failed, Reason::ContactDeleted, nowMs_);
	for (size_t index = 0; index < 4; ++index) {
		if (!acks_[index].used || acks_[index].contact != contact) continue;
		if (flightAck_ && flight_ == int(index)) { abort_ = true; flight_ = -1; }
		CryptoEngine::zeroize(&acks_[index], sizeof(Ack));
	}
	size_t kept = 0;
	for (size_t index = 0; index < eventCount_; ++index) {
		const Event& event = events_[(eventHead_ + index) % 32];
		if (event.message.contact == contact && event.type != EventType::Status) continue;
		if (kept != index) events_[(eventHead_ + kept) % 32] = event;
		++kept;
	}
	for (size_t index = kept; index < eventCount_; ++index)
		CryptoEngine::zeroize(&events_[(eventHead_ + index) % 32], sizeof(Event));
	eventCount_ = kept;
}

bool MeshEngine::allocate(uint8_t contact, uint32_t& counter) {
	if (contacts_.records_[contact - 1].next >= Contacts::CounterEnd) return false;
	if (contacts_.allocate(contact, counter)) return true;
	storageFailure_ = true;
	return false;
}

MeshEngine::Error MeshEngine::enqueue(uint8_t contact, const PacketCodec::Payload& payload,
	uint8_t hops, uint64_t nowMs, Pending& accepted, size_t& bytes) {
	accepted = {};
	bytes = 0;
	nowMs_ = nowMs;
	if (!unlocked_) return Error::Locked;
	const Contacts::View* peer = contacts_.view(contact);
	if (!peer) return Error::NoContact;
	if (hops > 7) return Error::Invalid;
	size_t index = 0;
	while (index < Capacity && slots_[index].view.used) ++index;
	if (index == Capacity || nextReference_ == 0) return Error::Full;
	size_t frameLength = 0;
	if (PacketCodec::validate(payload, frameLength) != PacketCodec::Error::None)
		return Error::Invalid;
	uint32_t counter = 0;
	if (!allocate(contact, counter)) return storageFailure_ ? Error::Storage : Error::CounterExhausted;
	Slot& slot = slots_[index];
	slot = {};
	slot.view.used = true;
	slot.view.reference = nextReference_++;
	slot.view.contact = contact;
	slot.view.type = payload.type;
	slot.view.counter = counter;
	slot.payload = payload;
	slot.hops = hops;
	slot.created = nowMs;
	accepted = slot.view;
	bytes = frameLength;
	return Error::None;
}

const MeshEngine::Pending* MeshEngine::pending(size_t index) const {
	return index < Capacity && slots_[index].view.used ? &slots_[index].view : nullptr;
}

size_t MeshEngine::count() const {
	size_t count = 0;
	for (const Slot& slot : slots_) if (slot.view.used) ++count;
	return count;
}

void MeshEngine::emit(const Event& event) {
	if (handler_) handler_(context_, event);
	if (eventCount_ == 32) { ++stats_.droppedEvents; return; }
	events_[(eventHead_ + eventCount_) % 32] = event;
	++eventCount_;
}

const MeshEngine::Event* MeshEngine::event() const {
	return eventCount_ ? &events_[eventHead_] : nullptr;
}

void MeshEngine::consumeEvent() {
	if (!eventCount_) return;
	CryptoEngine::zeroize(&events_[eventHead_], sizeof(Event));
	eventHead_ = (eventHead_ + 1) % 32;
	--eventCount_;
}

void MeshEngine::status(size_t index, State state, Reason reason, uint64_t nowMs, uint8_t hops) {
	Slot& slot = slots_[index];
	slot.view.state = state;
	Event event;
	event.message = slot.view;
	event.reason = reason;
	event.timeMs = nowMs;
	event.hops = hops;
	event.rttMs = nowMs - slot.created;
	emit(event);
}

void MeshEngine::finish(size_t index, State state, Reason reason, uint64_t nowMs, uint8_t hops) {
	status(index, state, reason, nowMs, hops);
	if (!flightAck_ && !flightRelay_ && flight_ == int(index)) { abort_ = true; flight_ = -1; }
	CryptoEngine::zeroize(&slots_[index], sizeof(Slot));
}

bool MeshEngine::cancel(uint32_t reference, uint64_t nowMs) {
	for (size_t index = 0; index < Capacity; ++index) {
		if (!slots_[index].view.used || slots_[index].view.reference != reference) continue;
		finish(index, State::Cancelled, Reason::None, nowMs);
		return true;
	}
	return false;
}

void MeshEngine::tick(uint64_t nowMs) {
	nowMs_ = nowMs;
	if (!unlocked_ || storageFailure_) return;
	for (size_t index = 0; index < Capacity; ++index) {
		Slot& slot = slots_[index];
		if (!slot.view.used || (slot.view.state != State::Sent && slot.view.state != State::Relayed) ||
			nowMs < slot.due) continue;
		if (slot.view.retry >= config_.retries) finish(index, State::Failed, Reason::Timeout, nowMs);
		else {
			++slot.view.retry;
			++stats_.retries;
			status(index, State::Retrying, Reason::None, nowMs);
		}
	}
}

bool MeshEngine::prepareTransmission(PacketCodec::Packet& packet, uint64_t nowMs) {
	packet = {};
	nowMs_ = nowMs;
	if (!radioWanted() || storageFailure_ || flight_ >= 0 || abort_ || nowMs < nextTransmitAt_) return false;
	for (size_t index = 0; index < 4; ++index) {
		if (!acks_[index].used) continue;
		packet = acks_[index].packet;
		flight_ = index;
		flightAck_ = true;
		flightRelay_ = false;
		uint32_t value;
		if (!hash(packet.bytes, packet.length, value)) { flight_ = -1; return false; }
		remember(value);
		return true;
	}
	for (size_t index = 0; index < Capacity; ++index) {
		const Slot& slot = slots_[index];
		if (!slot.view.used || (slot.view.state != State::Queued && slot.view.state != State::Retrying)) continue;
		PacketCodec::Header header;
		header.source = ownId_;
		header.destination = contacts_.records_[slot.view.contact - 1].view.nodeId;
		header.counter = slot.view.counter;
		header.hopStart = header.hopLeft = slot.hops;
		header.retry = slot.view.retry;
		if (PacketCodec::encode(header, slot.payload, contacts_.records_[slot.view.contact - 1].outgoing,
			packet) != PacketCodec::Error::None) {
			finish(index, State::Failed, Reason::RadioError, nowMs);
			return false;
		}
		flight_ = index;
		flightAck_ = false;
		flightRelay_ = false;
		if (!hash(packet.bytes, packet.length, slots_[index].lastHash)) { flight_ = -1; return false; }
		remember(slots_[index].lastHash);
		return true;
	}
	for (size_t index = 0; index < 8; ++index) {
		if (!relays_[index].used || nowMs < relays_[index].due) continue;
		packet = relays_[index].packet;
		flight_ = index; flightRelay_ = true; flightAck_ = false;
		return true;
	}
	return false;
}

void MeshEngine::transmissionRejected(bool busy, uint64_t nowMs) {
	if (busy) flight_ = -1;
	else transmissionComplete(false, 0, nowMs);
}

void MeshEngine::transmissionComplete(bool success, uint32_t airtimeMs, uint64_t nowMs) {
	if (airtimeMs) nextTransmitAt_ = nowMs + uint64_t(airtimeMs) * 99;
	if (flight_ < 0) return;
	const size_t index = flight_;
	flight_ = -1;
	if (flightRelay_) {
		if (success) ++stats_.relayed;
		else ++stats_.relayDropped;
		CryptoEngine::zeroize(&relays_[index], sizeof(Relay));
		return;
	}
	if (flightAck_) {
		if (success) ++stats_.acksSent;
		else ++stats_.droppedAcks;
		CryptoEngine::zeroize(&acks_[index], sizeof(Ack));
		return;
	}
	Slot& slot = slots_[index];
	if (!success) finish(index, State::Failed, Reason::RadioError, nowMs);
	else if (!slot.payload.ack) finish(index, State::Sent, Reason::None, nowMs);
	else {
		slot.sent = true;
		const uint64_t base = config_.retryBaseMs ? config_.retryBaseMs :
			uint64_t(2 * slot.hops + 2) * (airtimeMs + 750);
		slot.due = nowMs + (base << slot.view.retry);
		status(index, State::Sent, Reason::None, nowMs);
	}
}

void MeshEngine::radioUnavailable(uint64_t nowMs) {
	for (size_t index = 0; index < Capacity; ++index)
		if (slots_[index].view.used) finish(index, State::Failed, Reason::RadioError, nowMs);
	for (const Ack& ack : acks_) if (ack.used) ++stats_.droppedAcks;
	CryptoEngine::zeroize(acks_, sizeof(acks_));
	CryptoEngine::zeroize(relays_, sizeof(relays_));
	if (flight_ >= 0) abort_ = true;
	flight_ = -1;
}

bool MeshEngine::takeAbort() {
	const bool abort = abort_;
	abort_ = false;
	return abort;
}

bool MeshEngine::takeStorageFailure() {
	const bool failed = storageFailure_;
	storageFailure_ = false;
	return failed;
}

bool MeshEngine::takeCryptoFailure() {
	const bool failed = cryptoFailure_;
	cryptoFailure_ = false;
	return failed;
}

void MeshEngine::ack(uint8_t contact, const PacketCodec::Header& original) {
	size_t index = 0;
	while (index < 4 && acks_[index].used) ++index;
	if (index == 4) { ++stats_.droppedAcks; return; }
	PacketCodec::Header header;
	header.destination = original.source;
	header.source = ownId_;
	header.hopStart = header.hopLeft = original.hopStart - original.hopLeft + 1;
	if (header.hopStart > 7) header.hopStart = header.hopLeft = 7;
	if (!allocate(contact, header.counter)) { ++stats_.droppedAcks; return; }
	if (PacketCodec::encodeAck(header, original.counter, contacts_.records_[contact - 1].outgoing,
		acks_[index].packet) != PacketCodec::Error::None) { ++stats_.droppedAcks; return; }
	acks_[index].used = true;
	acks_[index].contact = contact;
}

void MeshEngine::receive(const uint8_t* bytes, size_t length, float rssi, float snr, uint64_t nowMs) {
	nowMs_ = nowMs;
	if (!radioWanted() || storageFailure_) return;
	PacketCodec::Header header;
	if (PacketCodec::inspect(bytes, length, header) != PacketCodec::Error::None) {
		++stats_.malformed; return;
	}
	Source* budget = source(header.source, nowMs);
	const bool addressed = unlocked_ && header.destination == ownId_;
	bool authenticated = false;
	if (addressed && budget && budget->failures < 20) {
		authenticated = endpoint(bytes, length, header, rssi, snr, nowMs);
		if (!authenticated) ++budget->failures;
	} else if (addressed) ++stats_.rateDrops;
	uint32_t value = 0;
	if (!hash(bytes, length, value)) return;
	if (!remember(value)) {
		for (size_t index = 0; index < 8; ++index) {
			if (!relays_[index].used || relays_[index].hash != value ||
				(flightRelay_ && flight_ == int(index))) continue;
			CryptoEngine::zeroize(&relays_[index], sizeof(Relay));
			++stats_.relaySuppressed;
		}
		for (size_t index = 0; index < Capacity; ++index)
			if (slots_[index].view.used && slots_[index].view.state == State::Sent &&
				slots_[index].lastHash == value)
				status(index, State::Relayed, Reason::None, nowMs);
		return;
	}
	if (authenticated || storageFailure_ || !config_.relayEnabled || !header.hopLeft) return;
	if (!budget || (config_.ratePerSource && budget->relays >= config_.ratePerSource)) {
		++stats_.rateDrops; return;
	}
	size_t index = 0;
	while (index < 8 && relays_[index].used) ++index;
	if (index == 8) { ++stats_.relayDropped; return; }
	uint32_t random = 0;
	if (!CryptoEngine::randomBytes(reinterpret_cast<uint8_t*>(&random), sizeof(random))) {
		cryptoReady_ = false; cryptoFailure_ = true; return;
	}
	Relay& relay = relays_[index];
	relay.used = true; relay.hash = value; relay.packet.length = length;
	memcpy(relay.packet.bytes, bytes, length);
	relay.packet.bytes[0] = (bytes[0] & 0xe3) | ((header.hopLeft - 1) << 2);
	const float signal = isfinite(snr) ? fmaxf(-20, fminf(10, snr)) : 10;
	relay.due = nowMs + uint32_t((signal + 20) * 8) +
		(config_.backoffSlots ? random % (uint32_t(config_.backoffSlots) * 25 + 1) : 0);
	++budget->relays;
}

bool MeshEngine::endpoint(const uint8_t* bytes, size_t length, const PacketCodec::Header& header,
	float rssi, float snr, uint64_t nowMs) {
	for (uint8_t contact = 1; contact <= Contacts::Capacity; ++contact) {
		const Contacts::View* peer = contacts_.view(contact);
		if (!peer || peer->nodeId != header.source) continue;
		PacketCodec::Payload payload;
		int matching = -1;
		if (length == 16) {
			for (size_t index = 0; index < Capacity; ++index) {
				const Slot& slot = slots_[index];
				if (!slot.view.used || !slot.sent || slot.view.contact != contact || !slot.payload.ack) continue;
				if (PacketCodec::verifyAck(bytes, length, slot.view.counter,
					contacts_.records_[contact - 1].incoming) == PacketCodec::Error::None) { matching = index; break; }
			}
			if (matching < 0) continue;
		} else {
			const PacketCodec::Error error = PacketCodec::decode(bytes, length,
				contacts_.records_[contact - 1].incoming, payload);
			if (error == PacketCodec::Error::Authentication) continue;
			if (error != PacketCodec::Error::None) { ++stats_.malformed; return true; }
		}
		const bool wasActive = peer->active;
		const Contacts::Replay replay = contacts_.accept(contact, header.counter, rssi, snr);
		if (replay == Contacts::Replay::StorageFailure) {
			storageFailure_ = true;
			CryptoEngine::zeroize(&payload, sizeof(payload));
			return true;
		}
		if (replay == Contacts::Replay::New) {
			if (!wasActive) {
				Event event;
				event.type = EventType::ContactActive; event.message.contact = contact; event.timeMs = nowMs;
				emit(event);
			}
			if (length == 16) {
				++stats_.acksReceived;
				finish(matching, State::Delivered, Reason::None, nowMs, header.hopStart - header.hopLeft);
			} else {
				Event event;
				event.type = EventType::Received;
				event.message.contact = contact; event.message.counter = header.counter; event.message.retry = header.retry;
				event.payload = payload; event.timeMs = nowMs;
				event.hops = header.hopStart - header.hopLeft; event.rssi = rssi; event.snr = snr;
				emit(event);
				CryptoEngine::zeroize(&event, sizeof(event));
				if (payload.ack) ack(contact, header);
			}
		} else {
			++stats_.replayDrops;
			if (replay == Contacts::Replay::Duplicate && length != 16 && payload.ack && header.retry > 0)
				ack(contact, header);
		}
		CryptoEngine::zeroize(&payload, sizeof(payload));
		return true;
	}
	++stats_.tagFailures;
	return false;
}

#if OFFGRID_RADIO_DIAGNOSTICS
bool MeshEngine::selfTest(VaultStore& vault) {
	static Contacts contacts(vault);
	static MeshEngine mesh(contacts);
	if (!mesh.init(true)) return false;
	mesh.close(0);
	while (mesh.event()) mesh.consumeEvent();
	mesh.resetStats();
	uint8_t publicKey[32] = {9}, outgoing[16] = {0xa1}, incoming[16] = {0xb2}, alternate[16] = {0xc3};
	if (!contacts.load() || !contacts.insert(1, publicKey, "Mesh", "0000 0000 0000", outgoing, incoming) ||
		!contacts.insert(2, publicKey, "Collision", "0000 0000 0000", outgoing, alternate)) return false;
	const uint16_t peerId = contacts.view(1)->nodeId;
	mesh.open(0xabcd);
	Config config;
	config.retryBaseMs = 100;
	config.relayEnabled = false;
	mesh.configure(config);
	PacketCodec::Payload payload, decoded;
	payload.ack = true;
	strcpy(payload.text, "Direct fixture");
	Pending accepted;
	size_t bytes = 0;
	uint32_t references[Capacity];
	for (size_t index = 0; index < Capacity; ++index) {
		if (mesh.enqueue(1, payload, 3, 1, accepted, bytes) != Error::None || bytes != 31) return false;
		references[index] = accepted.reference;
	}
	if (mesh.count() != Capacity || mesh.enqueue(1, payload, 3, 1, accepted, bytes) != Error::Full) return false;
	for (uint32_t reference : references) if (!mesh.cancel(reference, 2)) return false;
	while (mesh.event()) mesh.consumeEvent();
	if (mesh.count() || mesh.enqueue(1, payload, 3, 10, accepted, bytes) != Error::None) return false;
	const uint32_t original = accepted.counter;
	PacketCodec::Packet packet;
	if (!mesh.prepareTransmission(packet, 11) ||
		PacketCodec::decode(packet.bytes, packet.length, outgoing, decoded) != PacketCodec::Error::None ||
		strcmp(decoded.text, payload.text)) return false;
	mesh.tick(100000);
	if (!mesh.pending(0) || mesh.pending(0)->state != State::Queued || mesh.event()) return false;
	mesh.transmissionRejected(true, 100000);
	if (!mesh.prepareTransmission(packet, 100001)) return false;
	mesh.transmissionComplete(true, 0, 200000);
	mesh.tick(200099);
	if (mesh.pending(0)->state != State::Sent) return false;
	mesh.tick(200100);
	if (mesh.pending(0)->state != State::Retrying || mesh.pending(0)->retry != 1 ||
		!mesh.prepareTransmission(packet, 200101)) return false;
	PacketCodec::Header header;
	if (PacketCodec::inspect(packet.bytes, packet.length, header) != PacketCodec::Error::None ||
		header.counter != original || header.retry != 1) return false;
	mesh.transmissionComplete(true, 0, 200200);
	header.source = peerId; header.destination = 0xabcd; header.counter = 1; header.retry = 0;
	if (PacketCodec::encodeAck(header, original + 1, incoming, packet) != PacketCodec::Error::None) return false;
	mesh.receive(packet.bytes, packet.length, -90, 5, 200201);
	if (mesh.count() != 1 || contacts.view(1)->rxHighest) return false;
	if (PacketCodec::encodeAck(header, original, incoming, packet) != PacketCodec::Error::None) return false;
	mesh.receive(packet.bytes, packet.length, -90, 5, 200202);
	if (mesh.count() || mesh.stats().acksReceived != 1 || !contacts.view(1)->active) return false;
	bool delivered = false;
	while (const Event* event = mesh.event()) {
		if (event->message.state == State::Delivered) delivered = true;
		mesh.consumeEvent();
	}
	if (!delivered) return false;
	header.counter = 2;
	if (PacketCodec::encode(header, payload, incoming, packet) != PacketCodec::Error::None) return false;
	packet.bytes[packet.length - 1] ^= 1;
	mesh.receive(packet.bytes, packet.length, -90, 5, 200203);
	if (contacts.view(1)->rxHighest != 1) return false;
	packet.bytes[packet.length - 1] ^= 1;
	mesh.receive(packet.bytes, packet.length, -90, 5, 200204);
	if (!mesh.event() || mesh.event()->type != EventType::Received) return false;
	mesh.consumeEvent();
	PacketCodec::Packet ackPacket;
	if (!mesh.prepareTransmission(ackPacket, 200205) || ackPacket.length != 16 ||
		PacketCodec::verifyAck(ackPacket.bytes, 16, 2, outgoing) != PacketCodec::Error::None) return false;
	PacketCodec::Header ackHeader;
	if (PacketCodec::inspect(ackPacket.bytes, 16, ackHeader) != PacketCodec::Error::None ||
		ackHeader.counter <= original) return false;
	const uint32_t ackCounter = ackHeader.counter;
	mesh.transmissionComplete(true, 0, 200206);
	mesh.receive(packet.bytes, packet.length, -90, 5, 200207);
	if (mesh.event() || mesh.prepareTransmission(ackPacket, 200208)) return false;
	header.retry = 1;
	if (PacketCodec::encode(header, payload, incoming, packet) != PacketCodec::Error::None) return false;
	mesh.receive(packet.bytes, packet.length, -90, 5, 200209);
	if (mesh.event() || !mesh.prepareTransmission(ackPacket, 200210) ||
		PacketCodec::inspect(ackPacket.bytes, 16, ackHeader) != PacketCodec::Error::None ||
		ackHeader.counter <= ackCounter) return false;
	mesh.transmissionComplete(true, 0, 200211);
	header.retry = 0; header.counter = 100; payload.ack = false;
	if (PacketCodec::encode(header, payload, incoming, packet) != PacketCodec::Error::None) return false;
	mesh.receive(packet.bytes, packet.length, -90, 5, 200212);
	while (mesh.event()) mesh.consumeEvent();
	header.retry = 1; header.counter = 2; payload.ack = true;
	if (PacketCodec::encode(header, payload, incoming, packet) != PacketCodec::Error::None) return false;
	mesh.receive(packet.bytes, packet.length, -90, 5, 200213);
	if (mesh.event() || mesh.prepareTransmission(ackPacket, 200214)) return false;
	header.retry = 0; header.counter = 1; payload.ack = false;
	if (PacketCodec::encode(header, payload, alternate, packet) != PacketCodec::Error::None) return false;
	mesh.receive(packet.bytes, packet.length, -90, 5, 200215);
	if (!contacts.view(2)->active || contacts.view(2)->rxHighest != 1) return false;
	while (mesh.event()) mesh.consumeEvent();
	if (mesh.enqueue(1, payload, 3, 300000, accepted, bytes) != Error::None ||
		!mesh.prepareTransmission(packet, 300001) || !contacts.remove(1) || mesh.count() || !mesh.takeAbort())
		return false;
	if (!mesh.event() || mesh.event()->reason != Reason::ContactDeleted) return false;
	while (mesh.event()) mesh.consumeEvent();
	payload.ack = true;
	if (mesh.enqueue(2, payload, 3, 400000, accepted, bytes) != Error::None ||
		!mesh.prepareTransmission(packet, 400001)) return false;
	mesh.transmissionComplete(false, 0, 400002);
	if (mesh.count() || !mesh.event() || mesh.event()->reason != Reason::RadioError) return false;
	while (mesh.event()) mesh.consumeEvent();
	config.retries = 0; mesh.configure(config);
	if (mesh.enqueue(2, payload, 3, 500000, accepted, bytes) != Error::None ||
		!mesh.prepareTransmission(packet, 500001)) return false;
	mesh.transmissionComplete(true, 0, 500002);
	mesh.tick(500102);
	if (mesh.count()) return false;
	while (mesh.event()) mesh.consumeEvent();
	config.retries = 3; mesh.configure(config);
	uint64_t completed = 510000;
	if (mesh.enqueue(2, payload, 3, completed, accepted, bytes) != Error::None) return false;
	const uint32_t retryCounter = accepted.counter;
	for (uint8_t retry = 0; retry <= 3; ++retry) {
		if (!mesh.prepareTransmission(packet, completed) ||
			PacketCodec::inspect(packet.bytes, packet.length, header) != PacketCodec::Error::None ||
			header.retry != retry || header.counter != retryCounter) return false;
		mesh.transmissionComplete(true, 0, completed);
		const uint64_t due = completed + (uint64_t(100) << retry);
		mesh.tick(due - 1);
		if (mesh.count() != 1 || mesh.pending(0)->state != State::Sent) return false;
		mesh.tick(due);
		completed = due + 1;
		if (retry < 3 && (mesh.pending(0)->state != State::Retrying || mesh.pending(0)->retry != retry + 1))
			return false;
	}
	if (mesh.count() || mesh.prepareTransmission(packet, completed)) return false;
	while (mesh.event()) mesh.consumeEvent();
	payload.ack = false;
	if (mesh.enqueue(2, payload, 0, 520000, accepted, bytes) != Error::None ||
		!mesh.prepareTransmission(packet, 520001)) return false;
	mesh.transmissionComplete(true, 0, 520002);
	if (mesh.count() || !mesh.event() || mesh.event()->message.state != State::Sent) return false;
	while (mesh.event()) mesh.consumeEvent();
	payload.ack = true;
	for (size_t index = 0; index < 40; ++index) {
		if (mesh.enqueue(2, payload, 3, 600000, accepted, bytes) != Error::None ||
			!mesh.cancel(accepted.reference, 600001)) return false;
	}
	if (mesh.stats().droppedEvents != 8) return false;
	if (mesh.enqueue(2, payload, 3, 700000, accepted, bytes) != Error::None) return false;
	mesh.close(700001);
	if (mesh.count() || mesh.event()->reason != Reason::Locked ||
		mesh.enqueue(2, payload, 3, 700002, accepted, bytes) != Error::Locked) return false;
	mesh.receive(packet.bytes, packet.length, -90, 5, 700003);
	if (contacts.view(2)->rxHighest != 1) return false;
	while (mesh.event()) mesh.consumeEvent();
	bool ok = contacts.remove(2);
	contacts.close();
	if (!ok || !mesh.init(true)) return false;
	config.relayEnabled = config.relayWhenLocked = true;
	config.ratePerSource = 2; config.backoffSlots = 0;
	mesh.configure(config);
	mesh.resetStats();
	header = {};
	header.source = 0xabcd; header.destination = 0xabcd; header.counter = 1;
	payload.ack = false;
	if (!mesh.radioWanted() || PacketCodec::encode(header, payload, incoming, packet) != PacketCodec::Error::None)
		return false;
	uint32_t firstHash = 0, modifiedHash = 0;
	if (!mesh.hash(packet.bytes, packet.length, firstHash)) return false;
	packet.bytes[0] -= 4;
	if (!mesh.hash(packet.bytes, packet.length, modifiedHash) || firstHash != modifiedHash) return false;
	packet.bytes[packet.length - 1] ^= 1;
	if (!mesh.hash(packet.bytes, packet.length, modifiedHash) || firstHash == modifiedHash) return false;
	packet.bytes[packet.length - 1] ^= 1;
	packet.bytes[0] += 4;
	mesh.receive(packet.bytes, packet.length, -90, -20, 800000);
	if (!mesh.prepareTransmission(ackPacket, 800000) ||
		PacketCodec::inspect(ackPacket.bytes, ackPacket.length, ackHeader) != PacketCodec::Error::None ||
		ackHeader.hopLeft != 2 || memcmp(ackPacket.bytes + 1, packet.bytes + 1, packet.length - 1)) return false;
	mesh.transmissionRejected(true, 800000);
	mesh.receive(packet.bytes, packet.length, -90, -20, 800001);
	if (mesh.stats().relaySuppressed != 1 || mesh.prepareTransmission(ackPacket, 801000)) return false;
	header.counter = 2;
	if (PacketCodec::encode(header, payload, incoming, packet) != PacketCodec::Error::None) return false;
	mesh.receive(packet.bytes, packet.length, -90, -20, 802000);
	if (!mesh.prepareTransmission(ackPacket, 802000)) return false;
	mesh.transmissionComplete(true, 5, 802001);
	header.counter = 3;
	if (PacketCodec::encode(header, payload, incoming, packet) != PacketCodec::Error::None) return false;
	mesh.receive(packet.bytes, packet.length, -90, -20, 802002);
	if (mesh.stats().rateDrops != 1 || mesh.stats().relayed != 1) return false;
	header.source = 0x1234; header.counter = 1;
	if (PacketCodec::encode(header, payload, incoming, packet) != PacketCodec::Error::None) return false;
	mesh.receive(packet.bytes, packet.length, -90, -20, 802003);
	if (mesh.prepareTransmission(ackPacket, 802495) || !mesh.prepareTransmission(ackPacket, 802496))
		return false;
	mesh.transmissionComplete(true, 0, 802497);
	config.relayWhenLocked = false;
	mesh.configure(config);
	if (mesh.radioWanted()) return false;
	mesh.open(0xabcd);
	header.source = header.destination = 0xabcd; header.counter = 4;
	if (PacketCodec::encode(header, payload, incoming, packet) != PacketCodec::Error::None) return false;
	mesh.receive(packet.bytes, packet.length, -90, -20, 900000);
	if (!mesh.prepareTransmission(ackPacket, 900000)) return false;
	mesh.transmissionComplete(true, 0, 900001);
	mesh.close(900002);
	mesh.takeAbort();
	mesh.init(false);
	config.relayWhenLocked = true; mesh.configure(config);
	if (mesh.radioWanted()) return false;
	return ok;
}
#endif