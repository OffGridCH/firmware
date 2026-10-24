#include <unity.h>

#include <deque>
#include <memory>
#include <string.h>
#include "../../src/mesh/MeshEngine.h"
#include "../../src/security/CryptoEngine.h"

extern std::deque<uint8_t> testRandom;

namespace {
using Type = MeshEngine::EventType;

void fromHex(const char* hex, uint8_t* bytes) {
	auto digit = [](char value) { return value <= '9' ? value - '0' : value - 'a' + 10; };
	for (size_t index = 0; hex[index * 2]; ++index) bytes[index] = (digit(hex[index * 2]) << 4) | digit(hex[index * 2 + 1]);
}

void assertHex(const char* hex, const uint8_t* bytes, size_t length) {
	uint8_t expected[128];
	TEST_ASSERT_EQUAL(strlen(hex) / 2, length);
	fromHex(hex, expected);
	TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, bytes, length);
}

void queueRandom(const uint8_t* bytes, size_t length) {
	for (size_t index = 0; index < length; ++index) testRandom.push_back(bytes[index]);
}

uint16_t tagOf(const uint8_t key[16], const uint8_t id[4], uint8_t epoch) {
	uint16_t tag = 0;
	TEST_ASSERT_TRUE(Groups::deriveTag(key, id, epoch, tag));
	return tag;
}

// Varies the key until its TAG equals target (about 65 000 HKDF calls on average).
void keyWithTag(uint8_t key[16], const uint8_t id[4], uint16_t target) {
	for (uint32_t attempt = 0;; ++attempt) {
		memcpy(key + 12, &attempt, 4);
		if (tagOf(key, id, 1) == target) return;
	}
}

// One device: in-memory vault, contacts, groups and mesh engine, unlocked with the NodeID of its key.
struct Node {
	VaultStore vault;
	Contacts contacts{vault};
	Groups groups{vault};
	MeshEngine mesh{contacts, groups};
	uint8_t publicKey[32] = {};
	uint16_t id = 0;

	explicit Node(uint8_t seed) {
		publicKey[0] = seed; publicKey[31] = 0x5a;
		uint8_t digest[32];
		CryptoEngine::sha256(publicKey, 32, digest);
		id = (uint16_t(digest[0]) << 8) | digest[1];
		vault.load();
		boot();
	}

	// A reboot keeps the vault and loses RAM state (dedupe cache, replay windows above the saved highest).
	void boot() {
		mesh.close(0);
		TEST_ASSERT_TRUE(mesh.init(true) && contacts.load() && groups.load());
		mesh.open(id);
		MeshEngine::Config config;
		config.backoffSlots = 0;
		mesh.configure(config);
		drain();
	}

	void drain() { while (mesh.event()) mesh.consumeEvent(); }

	// Drains all events and keeps the last one of the given type.
	bool take(Type type, MeshEngine::Event& found) {
		bool seen = false;
		while (const MeshEngine::Event* event = mesh.event()) {
			if (event->type == type) { found = *event; seen = true; }
			mesh.consumeEvent();
		}
		return seen;
	}

	bool transmit(PacketCodec::Packet& packet, uint64_t nowMs) {
		if (!mesh.prepareTransmission(packet, nowMs)) return false;
		mesh.transmissionComplete(true, 0, nowMs);
		return true;
	}

	void receive(const PacketCodec::Packet& packet, uint64_t nowMs) {
		mesh.receive(packet.bytes, packet.length, -60, 8, nowMs);
	}
};

void pair(Node& a, uint8_t slotA, Node& b, uint8_t slotB, uint8_t seed) {
	uint8_t ab[16], ba[16];
	memset(ab, seed, 16); memset(ba, seed + 1, 16);
	TEST_ASSERT_TRUE(a.contacts.insert(slotA, b.publicKey, "Peer", "0000 0000 0000", ab, ba));
	TEST_ASSERT_TRUE(b.contacts.insert(slotB, a.publicKey, "Peer", "0000 0000 0000", ba, ab));
}

void enqueue(Node& from, uint8_t contact, const PacketCodec::Payload& payload, uint8_t group = 0) {
	MeshEngine::Pending accepted;
	size_t bytes = 0;
	TEST_ASSERT_EQUAL(int(MeshEngine::Error::None), int(from.mesh.enqueue(contact, payload, 3, 1, accepted, bytes, group)));
}

PacketCodec::Payload text(const char* value) {
	PacketCodec::Payload payload;
	strcpy(payload.text, value);
	return payload;
}

PacketCodec::Payload syncPayload(const uint8_t* body, size_t length) {
	PacketCodec::Payload payload;
	payload.type = PacketCodec::Type::GroupSync;
	payload.ack = true;
	memcpy(payload.text, body, length);
	payload.syncLength = length;
	return payload;
}

// Owner sends the current group state to the member over their DM channel, member receipts it.
void sync(Node& owner, uint8_t group, uint8_t contact, Node& member, uint64_t nowMs) {
	PacketCodec::Payload payload;
	size_t length = 0;
	payload.type = PacketCodec::Type::GroupSync;
	payload.ack = true;
	uint8_t index = 0;
	for (uint8_t candidate = 1; candidate < owner.groups.view(group)->count; ++candidate)
		if (owner.groups.view(group)->contacts[candidate] == contact) index = candidate;
	TEST_ASSERT_TRUE(owner.groups.syncBody(group, index, reinterpret_cast<uint8_t*>(payload.text), length));
	payload.syncLength = length;
	enqueue(owner, contact, payload, group);
	PacketCodec::Packet packet;
	TEST_ASSERT_TRUE(owner.transmit(packet, nowMs));
	member.receive(packet, nowMs);
	TEST_ASSERT_TRUE(member.transmit(packet, nowMs));
	owner.receive(packet, nowMs);
}

PacketCodec::Packet groupPacket(const uint8_t key[16], uint16_t tag, uint16_t source, uint32_t counter,
	const PacketCodec::Payload& payload, uint8_t retry = 0) {
	PacketCodec::Header header;
	header.destination = tag; header.source = source; header.counter = counter; header.retry = retry;
	PacketCodec::Packet packet;
	TEST_ASSERT_EQUAL(int(PacketCodec::Error::None), int(PacketCodec::encode(header, payload, key, packet)));
	return packet;
}

const char* const GroupKey = "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf";
const char* const GroupId = "1a2b3c4d";
}

void setUp() { testRandom.clear(); }
void tearDown() {}

// Protocol section 15 under OpenSSL: the host crypto matches the mbedTLS vectors.
void test_dm_vectors() {
	uint8_t alice[16], bob[16];
	fromHex("b6c3836e8bfc1d784b0688a5963e1497", alice);
	fromHex("869a272db66553bf737a40f9fb8929ca", bob);
	PacketCodec::Header header;
	header.source = 0xe3f0; header.destination = 0xe841; header.counter = 1;
	PacketCodec::Payload payload = text("Hello!");
	payload.ack = true;
	PacketCodec::Packet packet;
	TEST_ASSERT_EQUAL(int(PacketCodec::Error::None), int(PacketCodec::encode(header, payload, alice, packet)));
	assertHex("6ce841e3f00000015d1d3fd92a3e04da794024db76adbc", packet.bytes, packet.length);
	header.source = 0xe841; header.destination = 0xe3f0; header.hopStart = header.hopLeft = 2;
	TEST_ASSERT_EQUAL(int(PacketCodec::Error::None), int(PacketCodec::encodeAck(header, 1, bob, packet)));
	assertHex("48e3f0e8410000017f15646b08384b1d", packet.bytes, packet.length);
}

// Protocol section 15.9; the expected bytes were computed independently with Python cryptography.
void test_group_vectors() {
	uint8_t key[16], id[4], sender[16], ab[16];
	fromHex(GroupKey, key); fromHex(GroupId, id);
	TEST_ASSERT_TRUE(Groups::deriveKey(key, id, 1, 0, sender));
	assertHex("a473d70b407fdcdb5e69da6778e99951", sender, 16);
	TEST_ASSERT_TRUE(Groups::deriveKey(key, id, 1, 1, sender));
	assertHex("ba4c2567d55a921313eb16df4e0eef8a", sender, 16);
	TEST_ASSERT_EQUAL_HEX16(0xd998, tagOf(key, id, 1));
	TEST_ASSERT_TRUE(Groups::deriveKey(key, id, 1, 0, sender));
	PacketCodec::Packet packet = groupPacket(sender, 0xd998, 0xe3f0, 1, text("Hallo Gruppe!"));
	assertHex("6cd998e3f0000001b3ae67a975c7f213bbd4e19a36a6c24c04dfe44e90c0", packet.bytes, packet.length);

	uint8_t body[79];
	fromHex("1a2b3c4d01a0a1a2a3a4a5a6a7a8a9aaabacadaeaf0102e3f0e8415465616d", body);
	fromHex("b6c3836e8bfc1d784b0688a5963e1497", ab);
	PacketCodec::Header header;
	header.source = 0xe3f0; header.destination = 0xe841; header.counter = 3;
	TEST_ASSERT_EQUAL(int(PacketCodec::Error::None),
		int(PacketCodec::encode(header, syncPayload(body, 31), ab, packet)));
	assertHex("6ce841e3f00000034892bda8b5c2e841ac6d8a4c4cf02c8d15e259ff283cff4f476662fa64d43167d762da4e5092658b",
		packet.bytes, packet.length);
	PacketCodec::Payload decoded;
	TEST_ASSERT_EQUAL(int(PacketCodec::Error::None), int(PacketCodec::decode(packet.bytes, packet.length, ab, decoded)));
	TEST_ASSERT_EQUAL(int(PacketCodec::Type::GroupSync), int(decoded.type));
	TEST_ASSERT_EQUAL(31, decoded.syncLength);
	TEST_ASSERT_EQUAL_HEX8_ARRAY(body, decoded.text, 31);
}

// 16 members and a 24-byte name are the largest sync: 79 body bytes, a 96-byte packet.
void test_sync_limits() {
	VaultStore vault, memberVault;
	vault.load(); memberVault.load();
	Groups owner(vault), member(memberVault);
	TEST_ASSERT_TRUE(owner.load() && member.load());
	uint16_t nodes[16];
	uint8_t contacts[16];
	for (uint8_t index = 0; index < 16; ++index) { nodes[index] = 0x1000 + index; contacts[index] = index; }
	uint8_t group = 0;
	TEST_ASSERT_EQUAL(int(Groups::Error::Invalid), int(owner.create("x", nodes, contacts, 17, group)));
	TEST_ASSERT_EQUAL(int(Groups::Error::None), int(owner.create("abcdefghijklmnopqrstuvwx", nodes, contacts, 16, group)));
	uint8_t body[96];
	size_t length = 0;
	TEST_ASSERT_TRUE(owner.syncBody(group, 15, body, length));
	TEST_ASSERT_EQUAL(79, length);
	size_t frame = 0;
	TEST_ASSERT_EQUAL(int(PacketCodec::Error::None), int(PacketCodec::validate(syncPayload(body, length), frame)));
	TEST_ASSERT_EQUAL(96, frame);
	uint8_t unused[96];
	size_t unusedLength = 0;
	TEST_ASSERT_FALSE(owner.syncBody(group, 0, unused, unusedLength));

	uint8_t slot = 0;
	uint8_t changed[96];
	auto applyChanged = [&](size_t offset, uint8_t value, uint16_t ownId) {
		memcpy(changed, body, sizeof(changed));
		changed[offset] = value;
		return member.apply(4, 0x1000, ownId, changed, length, slot);
	};
	TEST_ASSERT_EQUAL(int(Groups::Sync::Rejected), int(member.apply(4, 0x1001, 0x100f, body, length, slot)));
	TEST_ASSERT_EQUAL(int(Groups::Sync::Rejected), int(member.apply(4, 0x1000, 0x100e, body, length, slot)));
	TEST_ASSERT_EQUAL(int(Groups::Sync::Rejected), int(applyChanged(21, 0, 0x1000)));
	TEST_ASSERT_EQUAL(int(Groups::Sync::Rejected), int(applyChanged(22, 17, 0x100f)));
	TEST_ASSERT_EQUAL(int(Groups::Sync::Rejected), int(applyChanged(4, 0, 0x100f)));
	TEST_ASSERT_EQUAL(int(Groups::Sync::Rejected), int(applyChanged(78, 0, 0x100f)));
	TEST_ASSERT_EQUAL(int(Groups::Sync::Joined), int(member.apply(4, 0x1000, 0x100f, body, length, slot)));
	TEST_ASSERT_EQUAL(15, member.view(slot)->member);
	TEST_ASSERT_EQUAL_STRING("abcdefghijklmnopqrstuvwx", member.view(slot)->name);
	TEST_ASSERT_EQUAL_HEX16(owner.view(group)->tag, member.view(slot)->tag);
	TEST_ASSERT_FALSE(member.view(slot)->owner);
}

// Joins, repeats, foreign owners, newer and older epochs, and a full device.
void test_sync_rules() {
	VaultStore vault, memberVault;
	vault.load(); memberVault.load();
	Groups owner(vault), member(memberVault);
	TEST_ASSERT_TRUE(owner.load() && member.load());
	const uint16_t nodes[] = {0x1000, 0x2000, 0x3000};
	const uint8_t contacts[] = {0, 1, 2};
	uint8_t group = 0, slot = 0, body[79];
	size_t length = 0;
	TEST_ASSERT_EQUAL(int(Groups::Error::None), int(owner.create("Team", nodes, contacts, 3, group)));
	TEST_ASSERT_TRUE(owner.syncBody(group, 1, body, length));
	TEST_ASSERT_EQUAL(int(Groups::Sync::Joined), int(member.apply(5, 0x1000, 0x2000, body, length, slot)));
	TEST_ASSERT_EQUAL(int(Groups::Sync::Unchanged), int(member.apply(5, 0x1000, 0x2000, body, length, slot)));
	TEST_ASSERT_EQUAL(int(Groups::Sync::Rejected), int(member.apply(6, 0x1000, 0x2000, body, length, slot)));
	TEST_ASSERT_EQUAL(int(Contacts::Replay::New), int(member.accept(1, 0, 40)));
	uint8_t old[79];
	memcpy(old, body, sizeof(old));
	const uint16_t smaller[] = {0x1000, 0x2000};
	TEST_ASSERT_EQUAL(int(Groups::Error::None), int(owner.rekey(group, smaller, contacts, 2)));
	TEST_ASSERT_EQUAL(2, owner.view(group)->epoch);
	TEST_ASSERT_TRUE(owner.syncBody(group, 1, body, length));
	TEST_ASSERT_EQUAL(int(Groups::Sync::Updated), int(member.apply(5, 0x1000, 0x2000, body, length, slot)));
	TEST_ASSERT_EQUAL(2, member.view(1)->count);
	TEST_ASSERT_EQUAL(int(Contacts::Replay::New), int(member.accept(1, 0, 1)));
	TEST_ASSERT_EQUAL(int(Groups::Sync::Rejected), int(member.apply(5, 0x1000, 0x2000, old, 32, slot)));
	for (uint8_t index = 2; index <= Groups::Capacity; ++index) {
		uint8_t other = 0;
		TEST_ASSERT_EQUAL(int(Groups::Error::None), int(owner.create("More", nodes, contacts, 3, other)));
		TEST_ASSERT_TRUE(owner.syncBody(other, 2, body, length));
		TEST_ASSERT_EQUAL(int(Groups::Sync::Joined), int(member.apply(5, 0x1000, 0x3000, body, length, slot)));
	}
	TEST_ASSERT_EQUAL(int(Groups::Error::Full), int(owner.create("Full", nodes, contacts, 3, group)));
	VaultStore thirdVault;
	thirdVault.load();
	Groups third(thirdVault);
	TEST_ASSERT_TRUE(third.load());
	TEST_ASSERT_EQUAL(int(Groups::Error::None), int(third.create("Ninth", nodes, contacts, 3, group)));
	TEST_ASSERT_TRUE(third.syncBody(group, 1, body, length));
	TEST_ASSERT_EQUAL(int(Groups::Sync::Full), int(member.apply(5, 0x1000, 0x2000, body, length, slot)));
}

// The owner re-rolls a key whose TAG equals a member NodeID or another local group's TAG.
void test_tag_reroll() {
	VaultStore vault;
	vault.load();
	Groups groups(vault);
	TEST_ASSERT_TRUE(groups.load());
	uint8_t id[4] = {1, 2, 3, 4}, first[16] = {7}, second[16] = {8}, body[79];
	uint16_t nodes[] = {0x1111, tagOf(first, id, 1)};
	const uint8_t contacts[] = {0, 1};
	queueRandom(id, 4); queueRandom(first, 16); queueRandom(second, 16);
	uint8_t group = 0;
	size_t length = 0;
	TEST_ASSERT_EQUAL(int(Groups::Error::None), int(groups.create("Reroll", nodes, contacts, 2, group)));
	TEST_ASSERT_EQUAL_HEX16(tagOf(second, id, 1), groups.view(group)->tag);
	TEST_ASSERT_NOT_EQUAL(nodes[1], groups.view(group)->tag);
	TEST_ASSERT_TRUE(groups.syncBody(group, 1, body, length));
	TEST_ASSERT_EQUAL_HEX8_ARRAY(second, body + 5, 16);

	uint8_t otherId[4] = {5, 6, 7, 8}, colliding[16] = {9}, fresh[16] = {10};
	keyWithTag(colliding, otherId, groups.view(group)->tag);
	nodes[1] = 0x2222;
	queueRandom(otherId, 4); queueRandom(colliding, 16); queueRandom(fresh, 16);
	uint8_t other = 0;
	TEST_ASSERT_EQUAL(int(Groups::Error::None), int(groups.create("Second", nodes, contacts, 2, other)));
	TEST_ASSERT_EQUAL_HEX16(tagOf(fresh, otherId, 1), groups.view(other)->tag);
	TEST_ASSERT_NOT_EQUAL(groups.view(group)->tag, groups.view(other)->tag);
}

// Replay windows and the device-wide counter survive a reload the same way contacts do.
void test_replay_and_counter() {
	VaultStore vault;
	vault.load();
	Groups groups(vault);
	TEST_ASSERT_TRUE(groups.load());
	const uint16_t nodes[] = {0x1000, 0x2000};
	const uint8_t contacts[] = {0, 1};
	uint8_t group = 0;
	TEST_ASSERT_EQUAL(int(Groups::Error::None), int(groups.create("Replay", nodes, contacts, 2, group)));
	TEST_ASSERT_EQUAL(int(Contacts::Replay::New), int(groups.accept(group, 1, 10)));
	TEST_ASSERT_EQUAL(int(Contacts::Replay::Duplicate), int(groups.accept(group, 1, 10)));
	TEST_ASSERT_EQUAL(int(Contacts::Replay::New), int(groups.accept(group, 1, 9)));
	TEST_ASSERT_EQUAL(int(Contacts::Replay::New), int(groups.accept(group, 1, 80)));
	TEST_ASSERT_EQUAL(int(Contacts::Replay::Stale), int(groups.accept(group, 1, 10)));
	TEST_ASSERT_EQUAL(int(Contacts::Replay::New), int(groups.accept(group, 0, 10)));
	uint32_t counter = 0;
	TEST_ASSERT_TRUE(groups.allocate(counter)); TEST_ASSERT_EQUAL(1, counter);
	TEST_ASSERT_TRUE(groups.allocate(counter)); TEST_ASSERT_EQUAL(2, counter);
	TEST_ASSERT_TRUE(groups.load());
	TEST_ASSERT_EQUAL(int(Contacts::Replay::Duplicate), int(groups.accept(group, 1, 79)));
	TEST_ASSERT_EQUAL(int(Contacts::Replay::New), int(groups.accept(group, 1, 81)));
	TEST_ASSERT_TRUE(groups.allocate(counter)); TEST_ASSERT_EQUAL(257, counter);
	TEST_ASSERT_TRUE(groups.remove(group));
	TEST_ASSERT_TRUE(groups.load());
	TEST_ASSERT_NULL(groups.view(group));
}

// A owns {A, B, C}; B and C are not paired with each other. Sync, send, receive, relay, dedupe, replay.
void test_mesh_round_trip() {
	std::unique_ptr<Node> a(new Node(1)), b(new Node(2)), c(new Node(3));
	pair(*a, 1, *b, 1, 0x10);
	pair(*a, 2, *c, 1, 0x20);
	const uint16_t nodes[] = {a->id, b->id, c->id};
	const uint8_t contacts[] = {0, 1, 2};
	uint8_t group = 0;
	TEST_ASSERT_EQUAL(int(Groups::Error::None), int(a->groups.create("Team", nodes, contacts, 3, group)));
	sync(*a, group, 1, *b, 10);
	sync(*a, group, 2, *c, 20);
	MeshEngine::Event event;
	TEST_ASSERT_TRUE(b->take(Type::GroupState, event));
	TEST_ASSERT_TRUE(event.joined);
	TEST_ASSERT_TRUE(c->take(Type::GroupState, event));
	TEST_ASSERT_TRUE(a->take(Type::Status, event));
	TEST_ASSERT_EQUAL(int(MeshEngine::State::Delivered), int(event.message.state));
	TEST_ASSERT_EQUAL(group, event.message.group);
	TEST_ASSERT_EQUAL(2, a->mesh.stats().acksReceived);

	MeshEngine::Pending accepted;
	size_t bytes = 0;
	PacketCodec::Payload payload = text("Treffpunkt 18 Uhr");
	payload.ack = true;
	TEST_ASSERT_EQUAL(int(MeshEngine::Error::Invalid), int(b->mesh.enqueue(0, payload, 3, 30, accepted, bytes, 1)));
	payload.ack = false;
	TEST_ASSERT_EQUAL(int(MeshEngine::Error::NoContact), int(b->mesh.enqueue(0, payload, 3, 30, accepted, bytes, 2)));
	TEST_ASSERT_EQUAL(int(MeshEngine::Error::None), int(b->mesh.enqueue(0, payload, 3, 30, accepted, bytes, 1)));
	TEST_ASSERT_EQUAL(17 + 17, bytes);
	PacketCodec::Packet packet, relayed;
	TEST_ASSERT_TRUE(b->transmit(packet, 31));
	TEST_ASSERT_TRUE(b->take(Type::Status, event));
	TEST_ASSERT_EQUAL(int(MeshEngine::State::Sent), int(event.message.state));
	TEST_ASSERT_EQUAL(0, b->mesh.count());
	PacketCodec::Header header;
	TEST_ASSERT_EQUAL(int(PacketCodec::Error::None), int(PacketCodec::inspect(packet.bytes, packet.length, header)));
	TEST_ASSERT_EQUAL_HEX16(a->groups.view(group)->tag, header.destination);
	TEST_ASSERT_EQUAL_HEX16(b->id, header.source);

	c->receive(packet, 32);
	TEST_ASSERT_TRUE(c->take(Type::Received, event));
	TEST_ASSERT_EQUAL(1, event.message.group);
	TEST_ASSERT_EQUAL(1, event.member);
	TEST_ASSERT_EQUAL_HEX16(b->id, event.node);
	TEST_ASSERT_EQUAL(0, event.message.contact);
	TEST_ASSERT_EQUAL_STRING("Treffpunkt 18 Uhr", event.payload.text);
	TEST_ASSERT_TRUE(c->transmit(relayed, 5000));
	TEST_ASSERT_EQUAL(packet.length, relayed.length);
	TEST_ASSERT_EQUAL_HEX8(packet.bytes[0] - 4, relayed.bytes[0]);
	TEST_ASSERT_EQUAL_HEX8_ARRAY(packet.bytes + 1, relayed.bytes + 1, packet.length - 1);

	a->receive(relayed, 5001);
	TEST_ASSERT_TRUE(a->take(Type::Received, event));
	TEST_ASSERT_EQUAL(1, event.message.contact);
	TEST_ASSERT_EQUAL(1, event.hops);
	a->receive(packet, 5002);
	TEST_ASSERT_FALSE(a->take(Type::Received, event));
	c->receive(relayed, 5003);
	TEST_ASSERT_FALSE(c->take(Type::Received, event));
	a->boot();
	a->receive(packet, 6000);
	TEST_ASSERT_FALSE(a->take(Type::Received, event));
	TEST_ASSERT_EQUAL(1, a->mesh.stats().replayDrops);

	PacketCodec::Payload reaction;
	reaction.type = PacketCodec::Type::Reaction;
	TEST_ASSERT_EQUAL(int(MeshEngine::Error::Invalid), int(a->mesh.enqueue(0, reaction, 3, 7000, accepted, bytes, group)));
	TEST_ASSERT_EQUAL(int(MeshEngine::Error::None), int(a->mesh.enqueue(0, payload, 3, 7000, accepted, bytes, group)));
	a->mesh.groupChanged(group, true, 7001);
	TEST_ASSERT_EQUAL(0, a->mesh.count());
}

// Group packets with receipts, retries or non-group types are dropped after authentication.
void test_group_packet_rules() {
	std::unique_ptr<Node> a(new Node(1)), b(new Node(2));
	pair(*a, 1, *b, 1, 0x30);
	const uint16_t nodes[] = {a->id, b->id};
	const uint8_t contacts[] = {0, 1};
	uint8_t group = 0, key[16];
	TEST_ASSERT_EQUAL(int(Groups::Error::None), int(a->groups.create("Rules", nodes, contacts, 2, group)));
	sync(*a, group, 1, *b, 10);
	b->drain();
	TEST_ASSERT_TRUE(a->groups.senderKey(group, 0, key));
	const uint16_t tag = a->groups.view(group)->tag;
	PacketCodec::Payload payload = text("x");
	payload.ack = true;
	b->receive(groupPacket(key, tag, a->id, 1, payload), 20);
	payload.ack = false;
	b->receive(groupPacket(key, tag, a->id, 2, payload, 1), 21);
	PacketCodec::Payload reaction;
	reaction.type = PacketCodec::Type::Reaction;
	b->receive(groupPacket(key, tag, a->id, 3, reaction), 22);
	MeshEngine::Event event;
	TEST_ASSERT_FALSE(b->take(Type::Received, event));
	TEST_ASSERT_EQUAL(3, b->mesh.stats().malformed);
	b->receive(groupPacket(key, tag, a->id, 4, payload), 23);
	TEST_ASSERT_TRUE(b->take(Type::Received, event));
}

// A TAG equal to the receiver's own NodeID (possible with another owner's firmware) and two groups with
// one TAG: DMs and group packets are told apart by trial decryption, and failed group trials never use
// up the sender's DM failure budget.
void test_tag_collisions() {
	std::unique_ptr<Node> a(new Node(1)), b(new Node(2));
	pair(*a, 1, *b, 1, 0x40);
	uint8_t id[4] = {9, 9, 9, 9}, key[16] = {0x55}, otherId[4] = {7, 7, 7, 7}, otherKey[16] = {0x66};
	uint8_t body[79], sender[16], junk[16];
	memset(junk, 0x77, sizeof(junk));
	keyWithTag(key, id, b->id);
	keyWithTag(otherKey, otherId, b->id);
	auto join = [&](const uint8_t* groupId, const uint8_t* groupKey, uint64_t nowMs) {
		memcpy(body, groupId, 4); body[4] = 1; memcpy(body + 5, groupKey, 16); body[21] = 1; body[22] = 2;
		body[23] = a->id >> 8; body[24] = a->id; body[25] = b->id >> 8; body[26] = b->id; body[27] = 'K';
		enqueue(*a, 1, syncPayload(body, 28));
		PacketCodec::Packet packet;
		TEST_ASSERT_TRUE(a->transmit(packet, nowMs));
		b->receive(packet, nowMs);
		MeshEngine::Event event;
		TEST_ASSERT_TRUE(b->take(Type::GroupState, event));
		TEST_ASSERT_TRUE(b->transmit(packet, nowMs));
		return event.message.group;
	};
	const uint8_t first = join(id, key, 10), second = join(otherId, otherKey, 11);
	TEST_ASSERT_EQUAL_HEX16(b->id, b->groups.view(first)->tag);
	TEST_ASSERT_EQUAL_HEX16(b->id, b->groups.view(second)->tag);

	MeshEngine::Event event;
	PacketCodec::Packet packet;
	TEST_ASSERT_TRUE(Groups::deriveKey(otherKey, otherId, 1, 0, sender));
	b->receive(groupPacket(sender, b->id, a->id, 1, text("zweite Gruppe")), 20);
	TEST_ASSERT_TRUE(b->take(Type::Received, event));
	TEST_ASSERT_EQUAL(second, event.message.group);
	TEST_ASSERT_TRUE(Groups::deriveKey(key, id, 1, 0, sender));
	b->receive(groupPacket(sender, b->id, a->id, 2, text("erste Gruppe")), 21);
	TEST_ASSERT_TRUE(b->take(Type::Received, event));
	TEST_ASSERT_EQUAL(first, event.message.group);

	for (uint32_t counter = 100; counter < 130; ++counter)
		b->receive(groupPacket(junk, b->id, a->id, counter, text("forged")), 30 + counter);
	TEST_ASSERT_FALSE(b->take(Type::Received, event));
	enqueue(*a, 1, text("direkt"));
	TEST_ASSERT_TRUE(a->transmit(packet, 200));
	b->receive(packet, 201);
	TEST_ASSERT_TRUE(b->take(Type::Received, event));
	TEST_ASSERT_EQUAL(0, event.message.group);
	TEST_ASSERT_EQUAL(1, event.message.contact);
	TEST_ASSERT_EQUAL_STRING("direkt", event.payload.text);

	// Control: without a colliding group the same forgeries do exhaust the DM budget, as before.
	std::unique_ptr<Node> d(new Node(4));
	pair(*a, 2, *d, 1, 0x50);
	for (uint32_t counter = 100; counter < 130; ++counter)
		d->receive(groupPacket(junk, d->id, a->id, counter, text("forged")), 300 + counter);
	enqueue(*a, 2, text("direkt"));
	TEST_ASSERT_TRUE(a->transmit(packet, 500));
	d->receive(packet, 501);
	TEST_ASSERT_FALSE(d->take(Type::Received, event));
}

int main() {
	UNITY_BEGIN();
	RUN_TEST(test_dm_vectors);
	RUN_TEST(test_group_vectors);
	RUN_TEST(test_sync_limits);
	RUN_TEST(test_sync_rules);
	RUN_TEST(test_tag_reroll);
	RUN_TEST(test_replay_and_counter);
	RUN_TEST(test_mesh_round_trip);
	RUN_TEST(test_group_packet_rules);
	RUN_TEST(test_tag_collisions);
	return UNITY_END();
}
